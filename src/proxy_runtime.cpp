#include "proxy_runtime.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

extern "C" {
#include "lib/verifproxy.h"
}

using json = nlohmann::json;
using namespace std::chrono;

namespace {

std::once_flag g_nimMainOnce;

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

ProxyRuntime::ProxyRuntime(EmitFn emit) : m_emit(std::move(emit)) {}

ProxyRuntime::~ProxyRuntime() { stop(); }

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
    if (m_thread.joinable())
        return { false, {}, "proxy already started" };

    m_cfg = cfg;
    m_upstreamJson = cfg.toUpstreamJson();
    m_stopRequested = false;
    {
        std::lock_guard<std::mutex> lk(m_startMu);
        m_startDone = false; m_startOk = false; m_startError.clear();
    }
    setState(State::Starting);

    m_thread = std::thread([this] { threadMain(); });

    std::unique_lock<std::mutex> lk(m_startMu);
    const bool signalled = m_startCv.wait_for(
        lk, milliseconds(m_cfg.startTimeoutMs), [this] { return m_startDone; });

    if (!signalled) {
        // startVerifProxy has an unbounded prologue and no cancel. Leave the
        // thread running rather than tearing down underneath it; stop() will
        // join once it returns.
        return { false, {}, "timed out after " + std::to_string(m_cfg.startTimeoutMs)
                            + "ms waiting for the light client to initialise" };
    }
    if (!m_startOk)
        return { false, {}, m_startError };

    m_startedAt = nowSeconds();
    return { true, json{ { "chainId", m_cfg.expectedChainId() } }, "" };
}

StdLogosResult ProxyRuntime::stop() {
    if (!m_thread.joinable())
        return { false, {}, "proxy is not running" };

    m_stopRequested = true;
    m_cv.notify_all();
    // Join UNCONDITIONALLY, never detach: LogosModule::unload() unmaps the
    // plugin image while the host keeps running, so a detached thread would
    // execute unmapped code.
    m_thread.join();
    setState(State::Stopped);
    return { true, {}, "" };
}

void ProxyRuntime::threadMain() {
    m_threadId = std::this_thread::get_id();

    // NimMain must run before anything else (library/nim.cfg sets --noMain:on),
    // and it must run on the thread that later registers itself for the foreign
    // GC, because setupForeignThreadGc/tearDownForeignThreadGc are bound to
    // startVerifProxy/stopVerifProxy.
    std::call_once(g_nimMainOnce, [] { ::NimMain(); });

    m_ctx = ::startVerifProxy(m_upstreamJson.data(), nullptr, nullptr);

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
        return;
    }

    setState(State::Running);
    if (m_emit)
        m_emit("proxyStarted",
               json{ { "success", true },
                     { "chainId", m_cfg.expectedChainId() } }.dump());

    auto nextKeepAlive = steady_clock::now();
    for (;;) {
        drainCommands();
        if (m_stopRequested.load(std::memory_order_acquire)) break;

        if (m_inFlight.load() == 0 && keepAliveEnabled()
            && steady_clock::now() >= nextKeepAlive) {
            issueKeepAlive();
            nextKeepAlive = steady_clock::now() + milliseconds(m_cfg.keepAliveIntervalMs);
        }

        const auto t0 = steady_clock::now();
        const int rc = ::processVerifProxyTasks(m_ctx);
        const auto dt = steady_clock::now() - t0;
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
                      [this] { return !m_queue.empty() || m_stopRequested.load(); });
    }

    teardown();
}

void ProxyRuntime::teardown() {
    assert(std::this_thread::get_id() == m_threadId);
    setState(State::Draining);

    // DRAIN BEFORE STOPPING. stopVerifProxy sets ctx.stop, and
    // processVerifProxyTasks checks ctx.stop BEFORE polling — so after it, no
    // callback can ever fire and anything in flight would hang forever.
    const auto deadline = steady_clock::now() + milliseconds(m_cfg.drainTimeoutMs);
    while (m_inFlight.load() > 0 && steady_clock::now() < deadline) {
        if (::processVerifProxyTasks(m_ctx) == RET_CANCELLED) break;
        std::this_thread::sleep_for(milliseconds(1));
    }

    failAllPending("proxy shutting down");

    ::stopVerifProxy(m_ctx);
    ::freeContext(m_ctx);
    m_ctx = nullptr;

    if (m_emit) m_emit("proxyStopped", json{ { "success", true } }.dump());
}

void ProxyRuntime::failAllPending(const std::string& why) {
    std::deque<std::weak_ptr<CallSlot>> pending;
    { std::lock_guard<std::mutex> lk(m_mu); pending.swap(m_pending); }

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
    if (m_inFlight.load() >= m_cfg.maxInFlight)
        return { false, {}, "too many calls in flight (max " +
                            std::to_string(m_cfg.maxInFlight) + ")" };

    auto slot = std::make_shared<CallSlot>();
    slot->id     = m_nextId.fetch_add(1, std::memory_order_relaxed);
    slot->method = method;
    slot->params = params.dump();

    {
        std::lock_guard<std::mutex> lk(m_mu);
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
        return { false, {}, "timed out after " + std::to_string(m_cfg.callTimeoutMs) + "ms" };
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
        box->rt->noteFinished(slot->id, status == RET_SUCCESS);
    } catch (...) {
        // Never propagate into Nim.
    }
}

void ProxyRuntime::noteFinished(uint64_t, bool ok) {
    m_inFlight.fetch_sub(1, std::memory_order_acq_rel);
    if (!ok) m_callsFailed.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

void ProxyRuntime::issueKeepAlive() {
    assert(std::this_thread::get_id() == m_threadId);

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

    auto* box = new CallBox{ slot, this };
    m_inFlight.fetch_add(1, std::memory_order_acq_rel);
    ::proxyCall(m_ctx, slot->method.data(), slot->params.data(),
                &ProxyRuntime::callbackTrampoline, box);

    // Fire and forget; the outcome is observed on a later pump turn by
    // pollHeartbeat(). Recording the slot lets shutdown release it.
    std::lock_guard<std::mutex> lk(m_mu);
    m_pending.push_back(slot);
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
    j["counters"] = json{
        { "callsTotal",  m_callsTotal.load() },
        { "callsFailed", m_callsFailed.load() },
        { "callsInFlight", m_inFlight.load() },
        { "leakedCalls", m_leaked.load() },
        { "heartbeatFailures", m_heartbeatFailures.load() },
    };
    j["keepAlive"] = m_cfg.keepAlive;
    return j;
}
