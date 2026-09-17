#include "proxy_runtime.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <sstream>
#include <utility>

extern "C" {
#include "lib/verifproxy.h"
}

using json = nlohmann::json;
using namespace std::chrono;

namespace {

std::once_flag g_nimMainOnce;

/// A healthy exit from threadMain is sub-millisecond, so no loaded builder
/// comes near this; it exists only so a thread that never observes m_shutdown
/// fails in half a minute instead of wedging the process until CI gives up.
constexpr int kThreadExitDeadlineMs = 30000;

/// When the expired-at-the-front walk stops being enough and m_pending gets a
/// full sweep. Above any plausible maxInFlight, so a healthy run never pays for
/// one; small enough that the deque cannot grow past a few kilobytes.
constexpr size_t kPendingSweepAt = 256;

std::string tidOf(std::thread::id id) {
    std::ostringstream o; o << id; return o.str();
}

int64_t nowSeconds() {
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

/// Owns a Nim-allocated string. It must be released with
/// freeNimAllocatedString and NEVER with free()/delete: `library/nim.cfg` does
/// not set -d:useMalloc, so Nim uses its own shared-heap allocator here.
///
/// The null guard below is load-bearing, not defensive style:
/// freeNimAllocatedString(NULL) SEGFAULTS (it is a bare deallocShared), and the
/// C API hands back a null `result` on some paths.
class NimString {
public:
    explicit NimString(char* p) noexcept : m_p(p) {}
    ~NimString() { if (m_p) ::freeNimAllocatedString(m_p); }
    NimString(const NimString&) = delete;
    NimString& operator=(const NimString&) = delete;
    /// Copy OUT before the Nim string dies.
    std::string str() const { return m_p ? std::string(m_p) : std::string(); }
private:
    char* m_p;
};

/// Decode a callback payload.
///
/// The shapes are inconsistent upstream and both must be tolerated:
///   * RET_SUCCESS            -> `Json.encode(value)`, e.g. "\"0x10d4f\"" or an object
///   * RET_ERROR from a Result-> RAW "errType: errMsg", NOT json
///   * RET_ERROR from a Future-> `Json.encode(msg)`, i.e. a JSON string
///   * RET_DESER_ERROR        -> a plain string ("unknown method", "parameters missing")
json decodePayload(const std::string& raw, bool& parsedAsJson) {
    parsedAsJson = false;
    if (raw.empty()) return json();
    try {
        json v = json::parse(raw);
        parsedAsJson = true;
        return v;
    } catch (const std::exception&) {
        return json(raw);
    }
}

std::string errorMessage(int status, const std::string& raw) {
    bool wasJson = false;
    const json v = decodePayload(raw, wasJson);
    std::string msg = v.is_string() ? v.get<std::string>() : raw;
    if (msg.empty()) msg = "no detail";
    switch (status) {
        case RET_CANCELLED:   return "cancelled: " + msg;
        case RET_DESER_ERROR: return "bad request: " + msg;
        default:              return msg;
    }
}

} // namespace

// The heap box we hand Nim as `userData`. Deleted exactly once, in the
// callback's first statement.
struct CallBox {
    std::shared_ptr<CallSlot> slot;
    ProxyRuntime* rt;
};

const char* ProxyRuntime::stateName(State s) {
    switch (s) {
        case State::Idle:     return "uninitialized";
        case State::Starting: return "starting";
        case State::Running:  return "running";
        case State::Degraded: return "degraded";
        case State::Draining: return "stopping";
        case State::Stopped:  return "stopped";
        case State::Failed:   return "error";
    }
    return "unknown";
}

ProxyRuntime::ProxyRuntime(EmitFn emit, DescriptorQuery descriptorQuery)
    : m_emit(std::move(emit)), m_descriptorQuery(std::move(descriptorQuery)) {}

ProxyRuntime::~ProxyRuntime() {
    if (m_runActive.load()) stop();

    // Only the destructor ends the thread. Join UNCONDITIONALLY, never detach:
    // LogosModule::unload() unmaps the plugin image while the host keeps
    // running, so a detached thread would execute unmapped code.
    //
    // Under m_startMu, because threadMain reads it as a condvar predicate: a
    // broadcast landing between its predicate test and its futex registration
    // wakes nobody, and parks the thread for good.
    { std::lock_guard<std::mutex> lk(m_startMu); m_shutdown = true; }
    m_startCv.notify_all();
    m_cv.notify_all();
    m_admissionCv.notify_all();
    if (!m_thread.joinable()) return;

    // join() takes no deadline, so a proxy thread that never notices m_shutdown
    // hangs its caller forever with no output at all. Detaching is not an
    // option here (the image gets unmapped), so name who is stuck where and die.
    std::unique_lock<std::mutex> lk(m_startMu);
    if (!m_startCv.wait_for(lk, milliseconds(kThreadExitDeadlineMs),
                            [this] { return m_threadExited; })) {
        const std::string why = blockedReport(
            "the proxy thread has not left threadMain "
            + std::to_string(kThreadExitDeadlineMs) + "ms after shutdown was requested");
        lk.unlock();
        std::fprintf(stderr, "\nFATAL ProxyRuntime: %s\n", why.c_str());
        std::fflush(stderr);
        std::abort();
    }
    lk.unlock();
    m_thread.join();
}

std::string ProxyRuntime::blockedReport(const std::string& what) const {
    std::ostringstream o;
    o << what
      << " — waiting thread " << tidOf(std::this_thread::get_id())
      << ", proxy thread " << tidOf(m_threadId)
      << (m_parked.load() ? " (parked between runs)" : " (inside a run)")
      << ", state=" << stateName(m_state.load())
      << ", runActive=" << (m_runActive.load() ? "yes" : "no")
      << ", stopRequested=" << (m_stopRequested.load() ? "yes" : "no")
      << ", shutdown=" << (m_shutdown.load() ? "yes" : "no")
      << ", inFlight=" << m_inFlight.load()
      << ", admitted=" << m_admitted.load()
      << ", pumps=" << m_pumpCalls.load();
    // try_lock: a diagnostic that can itself block is worse than an incomplete one.
    std::unique_lock<std::mutex> lk(m_mu, std::try_to_lock);
    if (lk) o << ", queued=" << m_queue.size() << ", pending=" << m_pending.size();
    else    o << ", queue depth unknown (m_mu is held)";
    return o.str();
}

void ProxyRuntime::setState(State s, const std::string& error) {
    const State prev = m_state.exchange(s);
    if (!error.empty()) {
        std::lock_guard<std::mutex> lk(m_errMu);
        m_lastError = error;
    }
    if (prev == s) return;
    if (m_emit) {
        json p{ { "state", stateName(s) }, { "previous", stateName(prev) } };
        if (!error.empty()) p["error"] = error;
        m_emit("proxyStateChanged", p.dump());
    }
}

bool ProxyRuntime::keepAliveEnabled() const { return m_cfg.keepAlive != "off"; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

StdLogosResult ProxyRuntime::start(const ProxyConfig& cfg) {
    if (m_runActive.load())
        return { false, {}, "proxy already started" };

    {
        std::lock_guard<std::mutex> lk(m_admissionMu);
        // A caller from the previous run can still be waking after stop(). A
        // fresh epoch prevents it from crossing into this run if start()
        // wins that race.
        ++m_admissionEpoch;
        m_admissionQueue.clear();
        m_queuedNow = 0;
        m_admitted = 0;
        m_cfg = cfg;
        m_upstreamJson = cfg.toUpstreamJson();
        m_stopRequested = false;
    }
    m_admissionCv.notify_all();
    m_inFlight = 0;

    // Nimbus can fan one verified request out across parallelBlockDownloads
    // fresh Chronos HTTP connections. Add two for the primary provider call
    // and DNS/TLS/runtime overhead, with a floor for cheap methods.
    const int64_t fanOutEstimate = cfg.parallelBlockDownloads
            > std::numeric_limits<int64_t>::max() - 2
        ? std::numeric_limits<int64_t>::max()
        : cfg.parallelBlockDownloads + 2;
    m_descriptorsPerCall = std::max<int64_t>(4, fanOutEstimate);
    m_effectiveMaxInFlight = cfg.maxInFlight;

    const DescriptorSnapshot initial = m_descriptorQuery();
    const int64_t capacity = descriptorOperationCapacity(
        initial, m_descriptorsPerCall.load());
    if (capacity >= 0) {
        m_effectiveMaxInFlight = std::min<int64_t>(cfg.maxInFlight, capacity);
        if (capacity == 0) {
            const std::string why =
                "not enough file descriptors to start the proxy safely (open "
                + std::to_string(initial.openCount) + " of "
                + std::to_string(initial.softLimit) + "; reserve "
                + std::to_string(descriptorReserve(initial.softLimit)) + ")";
            setState(State::Failed, why);
            return { false, {}, why };
        }
    }

    // The thread outlives every individual run: see the note on m_shutdown.
    // Created lazily so a module that never starts the proxy never spawns it.
    if (!m_thread.joinable())
        m_thread = std::thread([this] { threadMain(); });

    setState(State::Starting);
    {
        std::lock_guard<std::mutex> lk(m_startMu);
        m_startDone = false; m_startOk = false; m_startError.clear();
        m_runFinished = false;
        m_runRequested = true;
    }
    m_startCv.notify_all();

    std::unique_lock<std::mutex> lk(m_startMu);
    const bool signalled = m_startCv.wait_for(
        lk, milliseconds(m_cfg.startTimeoutMs), [this] { return m_startDone; });

    if (!signalled) {
        // startVerifProxy has an unbounded prologue and no cancel. Leave the
        // run going rather than tearing down underneath it; stop() waits for it.
        lk.unlock();
        return { false, {}, blockedReport(
                     "start() timed out after " + std::to_string(m_cfg.startTimeoutMs)
                     + "ms waiting for the light client to initialise") };
    }
    if (!m_startOk)
        return { false, {}, m_startError };

    m_startedAt = nowSeconds();
    return { true, json{ { "chainId", m_cfg.expectedChainId() } }, "" };
}

StdLogosResult ProxyRuntime::stop() {
    if (!m_thread.joinable())
        return { false, {}, "proxy is not running" };
    {
        std::lock_guard<std::mutex> lk(m_startMu);
        if (m_runFinished && !m_runActive.load())
            return { false, {}, "proxy is not running" };
    }

    // Serialize the gate with call()'s enqueue. Without this lock a caller can
    // observe Running immediately after the pump's final drain, enqueue work,
    // and then watch the pump exit without ever dispatching it.
    {
        std::lock_guard<std::mutex> queueLock(m_mu);
        m_stopRequested = true;
    }
    m_cv.notify_all();
    m_admissionCv.notify_all();

    // Wait for the RUN to finish, not for the thread to exit — the thread is
    // reused by the next start(). The drain is bounded (drainTimeoutMs plus at
    // most one processVerifProxyTasks, measured up to 3253ms), so this waits
    // generously rather than forever, and never blocks the caller for the life
    // of the process.
    std::unique_lock<std::mutex> lk(m_startMu);
    const bool finished = m_startCv.wait_for(
        lk, milliseconds(m_cfg.drainTimeoutMs + 15000), [this] { return m_runFinished; });
    if (!finished) {
        lk.unlock();
        return { false, {}, blockedReport("stop() timed out waiting for the proxy to drain") };
    }

    return { true, {}, "" };
}

void ProxyRuntime::threadMain() {
    m_threadId = std::this_thread::get_id();

    // NimMain must run before anything else (library/nim.cfg sets --noMain:on),
    // and it binds the Nim runtime to THIS thread for good. Any other thread
    // calling into the library dies in startVerifProxy; see the note on
    // m_shutdown for why the foreign-thread GC hooks do not change that.
    std::call_once(g_nimMainOnce, [] { ::NimMain(); });

    // One thread, many runs. Everything that touches a verifproxy.h symbol
    // happens below this line, on this thread, for the life of the module.
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_startMu);
            m_parked = true;
            m_startCv.wait(lk, [this] { return m_runRequested || m_shutdown.load(); });
            m_parked = false;
            if (m_shutdown.load()) break;
            m_runRequested = false;
        }
        runOnce();
    }

    // Tells the destructor's bounded wait "exited" apart from "wedged".
    {
        std::lock_guard<std::mutex> lk(m_startMu);
        m_threadExited = true;
    }
    m_startCv.notify_all();
}

void ProxyRuntime::runOnce() {
    assert(std::this_thread::get_id() == m_threadId);

    // Every run gets a fresh Context; the previous one was released by
    // teardown(). Reset the per-run counters that describe the CURRENT run so
    // a restart does not inherit the last run's health.
    m_heartbeatStreak.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(m_errMu);
        m_headBlockNumber.clear();
        m_headUpdatedAt = 0;
    }

    m_ctx = ::startVerifProxy(m_upstreamJson.data(), nullptr, nullptr);

    // BEFORE releasing start(), not after. A caller handed `success` calls
    // straight into call(), which refuses anything that is not yet Running, and
    // ~ProxyRuntime skips stop() while m_runActive is still false — which
    // strands the pump loop below with nobody left alive to end it.
    if (m_ctx) {
        m_runActive = true;
        setState(State::Running);
    }

    {
        std::lock_guard<std::mutex> lk(m_startMu);
        m_startDone = true;
        m_startOk = (m_ctx != nullptr);
        if (!m_startOk) {
            // The C API has no error out-param: startVerifProxy caught a
            // CatchableError, destroyed its context and returned nil. The
            // reason exists only in the chronicles output on stdout.
            m_startError = "startVerifProxy returned NULL — the library reports no "
                           "reason through the C API; see the module log for the "
                           "chronicles output (topics vp_main / vp_engine)";
        }
    }
    m_startCv.notify_all();

    if (!m_ctx) {
        setState(State::Failed, "startVerifProxy returned NULL");
        if (m_emit)
            m_emit("proxyStarted",
                   json{ { "success", false }, { "error", m_startError } }.dump());
        // The run is over before it began; release anyone in stop().
        {
            std::lock_guard<std::mutex> lk(m_startMu);
            m_runFinished = true;
        }
        m_startCv.notify_all();
        return;
    }
    if (m_emit)
        m_emit("proxyStarted",
               json{ { "success", true },
                     { "chainId", m_cfg.expectedChainId() } }.dump());

    auto nextKeepAlive = steady_clock::now();
    for (;;) {
        drainCommands();
        // m_shutdown as well: a destructor that never reached stop() — because
        // start() timed out, or the run was published late — must still be able
        // to end this loop, and join() has nothing else to wait on.
        if (m_stopRequested.load(std::memory_order_acquire) || m_shutdown.load()) break;

        if (m_inFlight.load() == 0 && keepAliveEnabled()
            && steady_clock::now() >= nextKeepAlive) {
            // Ask for the head on EVERY beat, head probe first. The heartbeat
            // cannot report it — eth_syncing answers a hardcoded `false` — and
            // the probe is not the extra round trip it looks like: both methods
            // open with the engine's beaconSync(), which serialises them on one
            // async lock, so whichever runs first pays for the light-client
            // sync and the second finds isSynced() true and skips it. The
            // number itself comes from the LOCAL headerStore, not from an
            // execution backend. One sync round per beat either way, and a head
            // that is never more than one slot old — which consumers depend on:
            // eth_rpc's readiness gate calls the proxy "not tracking" once
            // head.updatedAt is 60s behind the snapshot's own clock, so with the
            // beat floored at a slot, an every-Nth-beat probe would trip it.
            issueHeadProbe();
            issueKeepAlive();
            nextKeepAlive = steady_clock::now() + milliseconds(m_cfg.keepAliveIntervalMs);
        }

        const bool wasBusy = m_inFlight.load(std::memory_order_acquire) > 0;
        const auto t0 = steady_clock::now();
        const int rc = ::processVerifProxyTasks(m_ctx);
        const auto dt = steady_clock::now() - t0;
        recordPump(duration_cast<milliseconds>(dt).count(), wasBusy);
        if (rc == RET_CANCELLED) break;

        if (m_inFlight.load(std::memory_order_acquire) > 0) {
            // Hot path. processVerifProxyTasks blocks inside chronos poll()
            // only while something is pending; if it returned instantly it did
            // no work, so back off 1ms rather than spinning a core.
            if (dt < milliseconds(1))
                std::this_thread::sleep_for(milliseconds(1));
            continue;
        }
        // Idle: sleep on the condvar so an enqueue wakes us immediately.
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv.wait_for(lk, milliseconds(m_cfg.pumpIntervalMs),
                      [this] { return !m_queue.empty() || m_stopRequested.load()
                                      || m_shutdown.load(); });
    }

    teardown();
}

void ProxyRuntime::teardown() {
    assert(std::this_thread::get_id() == m_threadId);
    setState(State::Draining);

    // DRAIN BEFORE STOPPING. stopVerifProxy sets ctx.stop, and
    // processVerifProxyTasks checks ctx.stop BEFORE polling — so after it, no
    // callback can ever fire and anything in flight would hang forever.
    //
    // The deadline is checked BETWEEN pump calls, so it bounds when we stop
    // STARTING new ones, not when we return: a single processVerifProxyTasks
    // was measured at up to 3253ms (sepolia, 21510 samples), so this can
    // overshoot drainTimeoutMs by about that much. Bounded, which is what makes
    // the unconditional join in stop() safe — but not the tight bound the name
    // suggests.
    const auto deadline = steady_clock::now() + milliseconds(m_cfg.drainTimeoutMs);
    while (m_inFlight.load() > 0 && steady_clock::now() < deadline) {
        if (::processVerifProxyTasks(m_ctx) == RET_CANCELLED) break;
        std::this_thread::sleep_for(milliseconds(1));
    }

    failAllPending("proxy shutting down");

    ::stopVerifProxy(m_ctx);
    ::freeContext(m_ctx);
    m_ctx = nullptr;
    m_runActive = false;
    // The C ABI cannot cancel calls. Once the Context is gone no callback can
    // release their reservations, so the run teardown is the authority.
    m_inFlight = 0;
    {
        std::lock_guard<std::mutex> lk(m_admissionMu);
        m_admitted = 0;
    }
    m_admissionCv.notify_all();

    setState(State::Stopped);
    if (m_emit) m_emit("proxyStopped", json{ { "success", true } }.dump());

    // Release stop(). Must come AFTER freeContext, so a start() that follows
    // cannot race a half-released Context.
    {
        std::lock_guard<std::mutex> lk(m_startMu);
        m_runFinished = true;
    }
    m_startCv.notify_all();
}

void ProxyRuntime::failAllPending(const std::string& why) {
    std::deque<std::weak_ptr<CallSlot>> pending;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        pending.swap(m_pending);
        m_queue.clear();
    }

    for (auto& w : pending) {
        auto slot = w.lock();
        if (!slot) continue;
        std::lock_guard<std::mutex> lk(slot->mu);
        if (slot->done) continue;
        slot->done = true;
        slot->status = RET_ERROR;
        slot->result = why;
        slot->cv.notify_all();
        // The matching CallBox is DELIBERATELY LEAKED: after freeContext there
        // is no dispatcher left to run its callback, and freeing it while Nim
        // might still hold the pointer would be a use-after-free. A few hundred
        // bytes per abandoned call, only at shutdown.
        m_leaked.fetch_add(1, std::memory_order_relaxed);
    }
}

void ProxyRuntime::prunePendingLocked() {
    while (!m_pending.empty() && m_pending.front().expired()) m_pending.pop_front();
    // The heartbeat alone pushes one entry per beat for the life of a run, so
    // a front walk that a single stuck slot can block is not enough on its own.
    if (m_pending.size() < kPendingSweepAt) return;
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [](const std::weak_ptr<CallSlot>& w) { return w.expired(); }),
                    m_pending.end());
}

void ProxyRuntime::drainCommands() {
    assert(std::this_thread::get_id() == m_threadId);
    for (;;) {
        std::function<void(Context*)> cmd;
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_queue.empty()) return;
            cmd = std::move(m_queue.front());
            m_queue.pop_front();
        }
        cmd(m_ctx);
    }
}

// ---------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------

StdLogosResult ProxyRuntime::call(const std::string& method, const json& params) {
    if (!running() && m_state.load() != State::Degraded)
        return { false, {}, "proxy not running" };
    if (!params.is_array())
        return { false, {}, "params must be a JSON array" };
    std::string admissionError;
    if (!waitForAdmission(admissionError))
        return { false, {}, admissionError };

    auto slot = std::make_shared<CallSlot>();
    slot->id     = m_nextId.fetch_add(1, std::memory_order_relaxed);
    slot->method = method;
    slot->params = params.dump();

    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_stopRequested.load(std::memory_order_acquire)
            || m_shutdown.load(std::memory_order_acquire)) {
            releaseAdmission();
            return { false, {}, "proxy shutting down" };
        }
        prunePendingLocked();
        m_pending.push_back(slot);
        m_queue.push_back([this, slot](Context* ctx) {
            auto* box = new CallBox{ slot, this };   // freed in the callback
            m_inFlight.fetch_add(1, std::memory_order_acq_rel);
            m_callsTotal.fetch_add(1, std::memory_order_relaxed);
            ::proxyCall(ctx, slot->method.data(), slot->params.data(),
                        &ProxyRuntime::callbackTrampoline, box);
        });
    }
    m_cv.notify_one();

    std::unique_lock<std::mutex> lk(slot->mu);
    if (!slot->cv.wait_for(lk, milliseconds(m_cfg.callTimeoutMs),
                           [&] { return slot->done; })) {
        // The slot stays alive — the CallBox owns a share — so a late callback
        // is harmless. There is no per-call cancel in the C API.
        lk.unlock();
        return { false, {}, blockedReport(
                     "call(\"" + method + "\", id " + std::to_string(slot->id) + ") timed out after "
                     + std::to_string(m_cfg.callTimeoutMs) + "ms") };
    }

    if (slot->status != RET_SUCCESS)
        return { false, {}, errorMessage(slot->status, slot->result) };

    bool wasJson = false;
    json value = decodePayload(slot->result, wasJson);
    return { true, std::move(value), "" };
}

void ProxyRuntime::callbackTrampoline(Context*, int status, char* result, void* userData) {
    // Runs inside Nim frames: an escaping C++ exception is undefined behaviour.
    try {
        std::unique_ptr<CallBox> box(static_cast<CallBox*>(userData));  // exactly once
        NimString owned(result);                                        // freed at scope exit
        if (!box) return;
        auto slot = box->slot;
        {
            std::lock_guard<std::mutex> lk(slot->mu);
            if (!slot->done) {
                slot->status = status;
                slot->result = owned.str();   // COPY before the Nim string dies
                slot->done = true;
            }
            slot->cv.notify_all();
        }
        switch (slot->kind) {
            case CallSlot::Kind::Heartbeat: box->rt->noteHeartbeat(*slot); break;
            case CallSlot::Kind::HeadProbe: box->rt->noteHeadProbe(*slot); break;
            case CallSlot::Kind::User:      break;
        }
        box->rt->noteFinished(slot->id, status == RET_SUCCESS);
    } catch (...) {
        // Never propagate into Nim.
    }
}

void ProxyRuntime::recordPump(int64_t ms, bool busy) {
    static constexpr int64_t kEdges[kPumpBuckets - 1] = { 1, 5, 20, 100, 500, 2000 };
    int b = kPumpBuckets - 1;
    for (int i = 0; i < kPumpBuckets - 1; ++i)
        if (ms < kEdges[i]) { b = i; break; }
    (busy ? m_pumpBusy : m_pumpIdle)[b].fetch_add(1, std::memory_order_relaxed);
    m_pumpCalls.fetch_add(1, std::memory_order_relaxed);

    int64_t prev = m_pumpMaxMs.load(std::memory_order_relaxed);
    while (ms > prev && !m_pumpMaxMs.compare_exchange_weak(prev, ms,
                                                           std::memory_order_relaxed)) { }
}

json ProxyRuntime::pumpHistogram() const {
    auto dump = [](const std::atomic<int64_t>* a) {
        json out = json::array();
        for (int i = 0; i < kPumpBuckets; ++i) out.push_back(a[i].load());
        return out;
    };
    return json{
        { "calls", m_pumpCalls.load() },
        { "maxMs", m_pumpMaxMs.load() },
        { "bucketEdgesMs", json::array({ 1, 5, 20, 100, 500, 2000 }) },
        { "idle", dump(m_pumpIdle) },
        { "busy", dump(m_pumpBusy) },
    };
}

void ProxyRuntime::noteFinished(uint64_t, bool ok) {
    m_inFlight.fetch_sub(1, std::memory_order_acq_rel);
    releaseAdmission();
    if (!ok) m_callsFailed.fetch_add(1, std::memory_order_relaxed);
}

ProxyRuntime::AdmissionBlock ProxyRuntime::admissionBlockLocked(std::string& error) {
    const int64_t admitted = m_admitted.load(std::memory_order_relaxed);
    const int64_t effective = m_effectiveMaxInFlight.load(std::memory_order_relaxed);
    if (admitted >= effective) {
        error = "the concurrent call ceiling is busy (effective max "
              + std::to_string(effective) + ", configured max "
              + std::to_string(m_cfg.maxInFlight) + ")";
        return AdmissionBlock::Concurrency;
    }

    const DescriptorSnapshot live = m_descriptorQuery();
    const int64_t perCall = m_descriptorsPerCall.load(std::memory_order_relaxed);
    if (live.softLimit >= 0 && live.openCount >= 0) {
        const int64_t available = live.softLimit - descriptorReserve(live.softLimit)
                                - live.openCount;
        // Some admitted calls may not have opened their sockets yet, so count
        // every reservation again here. This deliberately errs on the safe
        // side when active sockets are already reflected in openCount.
        const int64_t reservableCalls = available > 0 ? available / perCall : 0;
        if (admitted >= reservableCalls) {
            error = "OS file-descriptor budget is exhausted (open "
                  + std::to_string(live.openCount) + " of "
                  + std::to_string(live.softLimit) + ", reserving "
                  + std::to_string(descriptorReserve(live.softLimit))
                  + " for the host)";
            return AdmissionBlock::Descriptors;
        }
    }

    error.clear();
    return AdmissionBlock::None;
}

bool ProxyRuntime::removeAdmissionTicketLocked(uint64_t ticket) {
    const auto it = std::find(m_admissionQueue.begin(), m_admissionQueue.end(), ticket);
    if (it == m_admissionQueue.end()) return false;
    m_admissionQueue.erase(it);
    m_queuedNow.fetch_sub(1, std::memory_order_relaxed);
    return true;
}

bool ProxyRuntime::waitForAdmission(std::string& error) {
    std::unique_lock<std::mutex> lk(m_admissionMu);

    if (m_stopRequested.load(std::memory_order_acquire)
        || m_shutdown.load(std::memory_order_acquire)) {
        error = "proxy shutting down";
        return false;
    }

    const uint64_t epoch = m_admissionEpoch;
    std::string blocker;
    AdmissionBlock blockedBy = AdmissionBlock::None;

    // Preserve FIFO once anybody is waiting. A late caller must not steal a
    // slot merely because it happened to take this mutex before the front
    // waiter woke up.
    if (m_admissionQueue.empty()) {
        blockedBy = admissionBlockLocked(blocker);
        if (blockedBy == AdmissionBlock::None) {
            m_admitted.fetch_add(1, std::memory_order_release);
            return true;
        }
    } else {
        blocker = "earlier calls are waiting in the admission queue";
        blockedBy = AdmissionBlock::Concurrency;
    }

    const uint64_t ticket = m_nextAdmissionTicket++;
    m_admissionQueue.push_back(ticket);
    m_queuedNow.fetch_add(1, std::memory_order_relaxed);
    m_callsQueued.fetch_add(1, std::memory_order_relaxed);
    const auto deadline = steady_clock::now() + milliseconds(m_cfg.queueTimeoutMs);
    m_admissionCv.notify_all();

    for (;;) {
        if (m_shutdown.load(std::memory_order_acquire)
            || m_stopRequested.load(std::memory_order_acquire)
            || epoch != m_admissionEpoch) {
            removeAdmissionTicketLocked(ticket);
            error = "proxy shutting down";
            lk.unlock();
            m_admissionCv.notify_all();
            return false;
        }

        if (!m_admissionQueue.empty() && m_admissionQueue.front() == ticket) {
            blockedBy = admissionBlockLocked(blocker);
            if (blockedBy == AdmissionBlock::None) {
                m_admissionQueue.pop_front();
                m_queuedNow.fetch_sub(1, std::memory_order_relaxed);
                m_admitted.fetch_add(1, std::memory_order_release);
                lk.unlock();
                // There may be room for more than one waiter. Wake the next
                // ticket now rather than making it wait for this call to end.
                m_admissionCv.notify_all();
                return true;
            }
        }

        const auto now = steady_clock::now();
        if (now >= deadline) {
            removeAdmissionTicketLocked(ticket);
            m_queueTimeouts.fetch_add(1, std::memory_order_relaxed);
            if (blockedBy == AdmissionBlock::Descriptors)
                m_resourceRejections.fetch_add(1, std::memory_order_relaxed);
            error = "admission queue timed out after "
                  + std::to_string(m_cfg.queueTimeoutMs) + "ms waiting for capacity";
            if (!blocker.empty()) error += "; last blocker: " + blocker;
            lk.unlock();
            m_admissionCv.notify_all();
            return false;
        }

        // Slot releases notify directly. The bounded poll also notices when
        // descriptors consumed elsewhere in the host become available again.
        m_admissionCv.wait_until(lk, std::min(deadline, now + milliseconds(50)));
    }
}

bool ProxyRuntime::tryAdmit(std::string& error) {
    std::lock_guard<std::mutex> lk(m_admissionMu);

    // Heartbeats/head probes run on the only thread that can complete active
    // work, so they must never wait. They also yield to queued user traffic.
    if (!m_admissionQueue.empty()) {
        error = "user calls are waiting for admission";
        return false;
    }
    if (admissionBlockLocked(error) != AdmissionBlock::None)
        return false;

    m_admitted.fetch_add(1, std::memory_order_release);
    return true;
}

void ProxyRuntime::releaseAdmission() {
    {
        std::lock_guard<std::mutex> lk(m_admissionMu);
        const int64_t admitted = m_admitted.load(std::memory_order_relaxed);
        if (admitted > 0) m_admitted.store(admitted - 1, std::memory_order_release);
    }
    m_admissionCv.notify_all();
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

void ProxyRuntime::issueKeepAlive() {
    assert(std::this_thread::get_id() == m_threadId);

    std::string ignored;
    if (!tryAdmit(ignored)) return;

    // eth_syncing is the cheapest possible keep-alive: its frontend runs
    // engine.beaconSync() and touches no execution backend, and issuing it
    // bumps ctx.pendingCalls so processVerifProxyTasks actually poll()s. Its
    // RETURN value is a hardcoded `false` and useless; its ERROR string is the
    // only machine-readable sync-health signal the C ABI exposes.
    //
    // Reached through proxyCall rather than a hand-declared extern: eth_syncing
    // is exported by c_frontend.nim but absent from verifproxy.h, so declaring
    // it ourselves would risk a link failure against another build.
    auto slot = std::make_shared<CallSlot>();
    slot->id = m_nextId.fetch_add(1, std::memory_order_relaxed);
    slot->method = "eth_syncing";
    slot->params = "[]";
    slot->kind = CallSlot::Kind::Heartbeat;

    auto* box = new CallBox{ slot, this };
    m_inFlight.fetch_add(1, std::memory_order_acq_rel);
    ::proxyCall(m_ctx, slot->method.data(), slot->params.data(),
                &ProxyRuntime::callbackTrampoline, box);

    // Fire and forget; the outcome is observed on a later pump turn by
    // pollHeartbeat(). Recording the slot lets shutdown release it.
    std::lock_guard<std::mutex> lk(m_mu);
    prunePendingLocked();
    m_pending.push_back(slot);
}

// The heartbeat's own return value is a hardcoded `false` and tells us nothing
// about the head, so a separate, less frequent probe asks for the block number
// outright. Same fire-and-forget shape; observed in noteHeadProbe().
void ProxyRuntime::issueHeadProbe() {
    assert(std::this_thread::get_id() == m_threadId);

    std::string ignored;
    if (!tryAdmit(ignored)) return;

    auto slot = std::make_shared<CallSlot>();
    slot->id = m_nextId.fetch_add(1, std::memory_order_relaxed);
    slot->method = "eth_blockNumber";
    slot->params = "[]";
    slot->kind = CallSlot::Kind::HeadProbe;

    auto* box = new CallBox{ slot, this };
    m_inFlight.fetch_add(1, std::memory_order_acq_rel);
    ::proxyCall(m_ctx, slot->method.data(), slot->params.data(),
                &ProxyRuntime::callbackTrampoline, box);

    std::lock_guard<std::mutex> lk(m_mu);
    prunePendingLocked();
    m_pending.push_back(slot);
}

// Consecutive heartbeat failures are the only sync-health signal the C ABI
// offers: the error STRING is machine-readable ("UnavailableDataError: trusted
// block root not set", "VerificationError: unviable fork"), the return value is
// not. Three in a row is deliberately more than one blip and less than a long
// outage.
void ProxyRuntime::noteHeartbeat(const CallSlot& slot) {
    static constexpr int64_t kDegradeAfter = 3;

    if (slot.status == RET_SUCCESS) {
        m_heartbeatStreak.store(0, std::memory_order_relaxed);
        // Only climb back out of Degraded — never overwrite Draining/Stopped,
        // which a concurrent stop() may have just set.
        if (m_state.load() == State::Degraded) setState(State::Running);
        return;
    }

    m_heartbeatFailures.fetch_add(1, std::memory_order_relaxed);
    const int64_t streak = m_heartbeatStreak.fetch_add(1, std::memory_order_relaxed) + 1;
    if (streak >= kDegradeAfter && m_state.load() == State::Running)
        setState(State::Degraded, errorMessage(slot.status, slot.result));
}

void ProxyRuntime::noteHeadProbe(const CallSlot& slot) {
    if (slot.status != RET_SUCCESS) return;

    bool wasJson = false;
    const json v = decodePayload(slot.result, wasJson);

    if (!v.is_string()) return;
    const std::string hex = v.get<std::string>();
    if (hex.empty()) return;

    std::lock_guard<std::mutex> lk(m_errMu);
    m_headBlockNumber = hex;
    m_headUpdatedAt = nowSeconds();
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

json ProxyRuntime::statusSnapshot() const {
    json j;
    j["state"] = stateName(m_state.load());
    j["network"] = m_cfg.network;
    j["chainId"] = m_cfg.expectedChainId();
    j["startedAt"] = m_startedAt;
    j["uptimeSeconds"] = m_startedAt ? (nowSeconds() - m_startedAt) : 0;
    {
        std::lock_guard<std::mutex> lk(m_errMu);
        j["lastError"] = m_lastError;
        j["head"] = json{ { "blockNumber", m_headBlockNumber },
                          { "updatedAt", m_headUpdatedAt } };
    }
    size_t pendingSlots = 0;
    { std::lock_guard<std::mutex> lk(m_mu); pendingSlots = m_pending.size(); }
    j["counters"] = json{
        { "callsTotal",  m_callsTotal.load() },
        { "callsFailed", m_callsFailed.load() },
        { "callsInFlight", m_inFlight.load() },
        { "callsAdmitted", m_admitted.load() },
        { "leakedCalls", m_leaked.load() },
        // Slots the runtime is still tracking so shutdown can release them.
        // Bounded by pruning; a number that climbs with uptime is the leak
        // this counter exists to make visible.
        { "pendingSlots", static_cast<int64_t>(pendingSlots) },
        { "heartbeatFailures", m_heartbeatFailures.load() },
        { "resourceRejections", m_resourceRejections.load() },
        { "callsQueued", m_callsQueued.load() },
        { "queueTimeouts", m_queueTimeouts.load() },
    };
    const DescriptorSnapshot descriptors = m_descriptorQuery();
    j["resources"] = json{
        { "openDescriptors", descriptors.openCount },
        { "softDescriptorLimit", descriptors.softLimit },
        { "descriptorReserve", descriptorReserve(descriptors.softLimit) },
        { "estimatedDescriptorsPerCall", m_descriptorsPerCall.load() },
        { "configuredMaxInFlight", m_cfg.maxInFlight },
        { "effectiveMaxInFlight", m_effectiveMaxInFlight.load() },
        { "queuedCalls", m_queuedNow.load() },
        { "queueTimeoutMs", m_cfg.queueTimeoutMs },
    };
    j["keepAlive"] = m_cfg.keepAlive;
    j["pump"] = pumpHistogram();
    return j;
}
