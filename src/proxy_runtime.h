#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <logos_result.h>

#include "proxy_config.h"
#include "resource_budget.h"

struct Context;  // opaque, from verifproxy.h

/// One in-flight proxy call.
///
/// Ownership is JOINT: the waiter holds a shared_ptr, and the heap CallBox we
/// hand Nim as `userData` holds another. Whoever drops last frees. That
/// replaces logos-storage-module's `abandoned` flag — a caller that times out
/// simply lets go, and a late callback is safe by construction rather than by
/// a race-sensitive protocol.
struct CallSlot {
    std::mutex mu;
    std::condition_variable cv;
    bool        done = false;
    int         status = -1;          // RET_*
    std::string result;               // COPIED out of the Nim-allocated string
    uint64_t    id = 0;

    // Argument backing store lives HERE, not in a temporary: we cannot assume
    // the Nim side copies its cstring arguments before its first await.
    std::string method;
    std::string params;

    // What the completion means. A User call has a waiter blocked on `cv`;
    // a Heartbeat is fire-and-forget and probes the whole request path; a
    // Sync / OpSync is one nvp_eth_sync / nvp_op_sync, which the library no
    // longer runs on its own (nimbus-eth1#4828).
    enum class Kind { User, Heartbeat, Sync, OpSync };
    Kind kind = Kind::User;
};

class ProxyRuntime {
public:
    /// `emit` is called with (eventName, jsonPayload). Safe from any thread —
    /// the host marshals it.
    using EmitFn = std::function<void(const std::string&, const std::string&)>;
    using DescriptorQuery = std::function<DescriptorSnapshot()>;

    explicit ProxyRuntime(EmitFn emit,
                          DescriptorQuery descriptorQuery = currentProcessDescriptorSnapshot);
    ~ProxyRuntime();

    ProxyRuntime(const ProxyRuntime&) = delete;
    ProxyRuntime& operator=(const ProxyRuntime&) = delete;

    /// Spin up the proxy thread and wait for `startVerifProxy` to return.
    /// Blocks up to `cfg.startTimeoutMs`. Safe to block: the latch is tripped
    /// by the PROXY thread, never by the caller's own.
    StdLogosResult start(const ProxyConfig& cfg);

    /// Drain in-flight calls, then stop and free the context. Idempotent.
    StdLogosResult stop();

    bool running() const { return m_state.load() == State::Running; }

    /// Running OR degraded. Degraded means the proxy is up but its heartbeat
    /// is failing, so it is still a stoppable, live process — lifecycle
    /// decisions want this, health checks want running().
    bool live() const {
        const State s = m_state.load();
        return s == State::Running || s == State::Degraded;
    }

    /// THE call path. Everything — the ~60 typed wrappers and the generic
    /// rpc() — funnels through `proxyCall`, which is a string `case` over the
    /// same exported procs the typed C entry points call.
    ///
    /// `params` must be a JSON ARRAY (upstream does `parseJson(params).getElems`).
    StdLogosResult call(const std::string& method, const nlohmann::json& params);

    /// The configured call ceiling after applying the process' descriptor
    /// limit. Used by the optional HTTP listener to bound its own connection
    /// threads at the same resource boundary.
    int64_t effectiveMaxInFlight() const { return m_effectiveMaxInFlight.load(); }

    nlohmann::json statusSnapshot() const;

    /// How long `processVerifProxyTasks` actually blocks.
    ///
    /// The pump already measures this to decide its 1ms backoff, so bucketing
    /// it is nearly free — and it answers the one question that makes the
    /// unconditional destructor join safe or unsafe: does the C call return in
    /// bounded time? A p100 in the seconds would mean stop() can stall the host
    /// for that long, and would also bound how late a queued command can be.
    ///
    /// Split by in-flight count because the two regimes are different: with a
    /// call pending, poll() blocks on I/O; idle, it returns immediately.
    static constexpr int kPumpBuckets = 7;   // <1, <5, <20, <100, <500, <2000, >=2000 ms
    nlohmann::json pumpHistogram() const;

private:
    enum class State { Idle, Starting, Running, Degraded, Draining, Stopped, Failed };
    enum class AdmissionBlock { None, Concurrency, Descriptors };
    static const char* stateName(State s);

    void threadMain();
    void runOnce();
    void teardown();
    void drainCommands();
    void issueKeepAlive();
    void advanceSync();
    void issueSync(CallSlot::Kind kind);
    void finishSyncCycle(bool ok, const std::string& error);
    void releaseStart(bool ok, const std::string& error);
    int64_t readSyncIntervalMs();
    void failAllPending(const std::string& why);
    /// Drop expired entries from m_pending. Call under m_mu, before pushing.
    /// Expired entries leave in issue order, so the front walk is O(1)
    /// amortised; the sweep exists for the case that breaks it — a user call
    /// that timed out keeps its slot alive (the CallBox owns a share) and can
    /// sit in front of any number of expired ones.
    void prunePendingLocked();
    void setState(State s, const std::string& error = {});
    bool keepAliveEnabled() const;

    /// Who is stuck, where, and on which thread. Every bounded wait ends in
    /// this string: a job that goes silent names nothing, the same wait
    /// expiring names the defect.
    std::string blockedReport(const std::string& what) const;

    /// C callback. Runs on the proxy thread; must never let an exception
    /// escape into Nim frames.
    static void callbackTrampoline(Context* ctx, int status, char* result, void* userData);
    void noteHeartbeat(const CallSlot& slot);
    void noteSync(const CallSlot& slot);
    void noteHead(const CallSlot& slot);
    void noteFinished(uint64_t id, bool ok);
    void recordPump(int64_t ms, bool busy);
    AdmissionBlock admissionBlockLocked(std::string& error);
    bool waitForAdmission(std::string& error);
    bool tryAdmit(std::string& error);
    void releaseAdmission();
    bool removeAdmissionTicketLocked(uint64_t ticket);

    // ── owned by the proxy thread ────────────────────────────────────────
    Context* m_ctx = nullptr;
    std::string m_upstreamJson;   // must outlive the startVerifProxy call
    std::thread::id m_threadId;

    // The sync cycle: nvp_eth_sync, then nvp_op_sync when OP is configured.
    // Completions only record the outcome; the pump acts on it, so nothing
    // re-enters the library from inside a callback.
    enum class SyncPhase { Idle, Eth, EthDone, Op, OpDone };
    SyncPhase m_syncPhase = SyncPhase::Idle;
    bool m_syncOk = false;
    std::string m_syncError;
    std::chrono::steady_clock::time_point m_syncCycleStart{};
    std::chrono::steady_clock::time_point m_nextSync{};
    int64_t m_syncIntervalMs = 0;
    // start() is released by the first good cycle, not by startVerifProxy.
    bool m_startReleased = false;

    // ── shared ───────────────────────────────────────────────────────────
    std::thread m_thread;
    mutable std::mutex m_mu;
    std::condition_variable m_cv;          // wakes the pump
    std::deque<std::function<void(Context*)>> m_queue;
    std::atomic<uint64_t> m_nextId{1};
    std::atomic<int64_t>  m_inFlight{0};
    // Reserved before a command is queued, unlike m_inFlight (which rises on
    // the proxy thread). This closes the check-then-enqueue race when many
    // multi-dispatch workers arrive together.
    std::atomic<int64_t>  m_admitted{0};
    std::atomic<int64_t>  m_effectiveMaxInFlight{64};
    std::atomic<int64_t>  m_descriptorsPerCall{12};
    std::atomic<int64_t>  m_resourceRejections{0};
    std::atomic<int64_t>  m_callsQueued{0};
    std::atomic<int64_t>  m_queueTimeouts{0};
    std::atomic<int64_t>  m_queuedNow{0};
    std::atomic<int64_t>  m_leaked{0};
    std::atomic<int64_t>  m_callsTotal{0};
    std::atomic<int64_t>  m_callsFailed{0};
    std::atomic<int64_t>  m_heartbeatFailures{0};
    // Consecutive failures, not the lifetime total: one blip must not latch
    // the proxy into degraded forever.
    std::atomic<int64_t>  m_heartbeatStreak{0};
    std::atomic<int64_t>  m_syncFailures{0};
    std::atomic<int64_t>  m_syncStreak{0};
    std::atomic<int64_t>  m_syncIntervalReported{0};
    std::atomic<int64_t>  m_pumpCalls{0};
    std::atomic<int64_t>  m_pumpMaxMs{0};
    std::atomic<int64_t>  m_pumpIdle[kPumpBuckets]{};
    std::atomic<int64_t>  m_pumpBusy[kPumpBuckets]{};
    std::atomic<bool>     m_stopRequested{false};
    std::atomic<State>    m_state{State::Idle};

    // ── the proxy thread's lifetime ──────────────────────────────────────
    //
    // ONE thread for the life of this object, not one per start().
    //
    // The Nim runtime is bound to whichever thread ran NimMain(), and every
    // later entry into the library must be on that same thread — reproduced
    // deterministically against the real archive, same network and config both
    // times: new thread -> 139 (SIGSEGV), same thread -> clean, with a fresh
    // Context returned.
    //
    // `library/nim.cfg` DOES set -d:setupForeignThreadGc, so both
    // `startVerifProxy`'s setupForeignThreadGc() and `stopVerifProxy`'s
    // tearDownForeignThreadGc() are compiled in (both symbols are undefined
    // references in libverifproxy.a). Neither rescues a foreign thread here,
    // and the teardown side makes one strictly worse: Nim's guards key on the
    // thread-local `threadType`, which threadimpl.nim sets to NimThread on the
    // thread that ran NimMain, so on THIS thread both calls are no-ops — while
    // on a foreign thread setupForeignThreadGc() would give it a SEPARATE refc
    // heap, and stopVerifProxy() would then zeroMem() that whole heap out from
    // under every object the run allocated.
    //
    // start()/stop() are therefore COMMANDS posted to this thread, and only the
    // destructor ends it.
    std::atomic<bool> m_shutdown{false};   // destructor asked the thread to exit
    std::atomic<bool> m_runActive{false};  // between a good startVerifProxy and teardown
    std::atomic<bool> m_parked{false};     // idle between runs, waiting for a start()

    // start()/stop() handshake
    std::mutex m_startMu;
    std::condition_variable m_startCv;
    bool m_runRequested = false;   // a start() is waiting to be picked up
    bool m_runFinished = true;     // teardown for the current run has completed
    bool m_startDone = false;
    bool m_startOk = false;
    bool m_threadExited = false;   // threadMain has left its loop
    std::string m_startError;

    mutable std::mutex m_errMu;
    std::string m_lastError;
    std::string m_headBlockNumber;
    int64_t m_headUpdatedAt = 0;
    std::string m_lastSyncError;
    int64_t m_lastSyncedAt = 0;
    int64_t m_startedAt = 0;

    ProxyConfig m_cfg;
    EmitFn m_emit;
    DescriptorQuery m_descriptorQuery;
    std::mutex m_admissionMu;
    std::condition_variable m_admissionCv;
    std::deque<uint64_t> m_admissionQueue;
    uint64_t m_nextAdmissionTicket = 1;
    uint64_t m_admissionEpoch = 0;

    // Live slots, so shutdown can release anyone still waiting. Pruned on every
    // push: a slot's control block — and with make_shared the CallSlot storage
    // behind it, a mutex and a condvar and three strings — stays alive as long
    // as one weak_ptr names it, so an unpruned deque grows for the whole run.
    std::deque<std::weak_ptr<CallSlot>> m_pending;
};
