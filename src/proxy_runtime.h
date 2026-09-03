#pragma once

#include <atomic>
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
    // the other two are fire-and-forget and are the ONLY way the runtime
    // learns anything about proxy health, since the library exposes no getter
    // for light-client progress.
    enum class Kind { User, Heartbeat, HeadProbe };
    Kind kind = Kind::User;
};

class ProxyRuntime {
public:
    /// `emit` is called with (eventName, jsonPayload). Safe from any thread —
    /// the host marshals it.
    using EmitFn = std::function<void(const std::string&, const std::string&)>;

    explicit ProxyRuntime(EmitFn emit);
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
    static const char* stateName(State s);

    void threadMain();
    void runOnce();
    void teardown();
    void drainCommands();
    void issueKeepAlive();
    void failAllPending(const std::string& why);
    void setState(State s, const std::string& error = {});
    bool keepAliveEnabled() const;

    /// Who is stuck, where, and on which thread. Every bounded wait ends in
    /// this string: a job that goes silent names nothing, the same wait
    /// expiring names the defect.
    std::string blockedReport(const std::string& what) const;

    /// C callback. Runs on the proxy thread; must never let an exception
    /// escape into Nim frames.
    static void callbackTrampoline(Context* ctx, int status, char* result, void* userData);
    void issueHeadProbe();
    void noteHeartbeat(const CallSlot& slot);
    void noteHeadProbe(const CallSlot& slot);
    void noteFinished(uint64_t id, bool ok);
    void recordPump(int64_t ms, bool busy);

    // ── owned by the proxy thread ────────────────────────────────────────
    Context* m_ctx = nullptr;
    std::string m_upstreamJson;   // must outlive the startVerifProxy call
    std::thread::id m_threadId;

    // ── shared ───────────────────────────────────────────────────────────
    std::thread m_thread;
    mutable std::mutex m_mu;
    std::condition_variable m_cv;          // wakes the pump
    std::deque<std::function<void(Context*)>> m_queue;
    std::atomic<uint64_t> m_nextId{1};
    std::atomic<int64_t>  m_inFlight{0};
    std::atomic<int64_t>  m_leaked{0};
    std::atomic<int64_t>  m_callsTotal{0};
    std::atomic<int64_t>  m_callsFailed{0};
    std::atomic<int64_t>  m_heartbeatFailures{0};
    // Consecutive failures, not the lifetime total: one blip must not latch
    // the proxy into degraded forever.
    std::atomic<int64_t>  m_heartbeatStreak{0};
    std::atomic<int64_t>  m_beatsSinceHead{0};
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
    // The Nim runtime is bound to whichever thread ran NimMain(), and this
    // build compiles NEITHER setupForeignThreadGc nor tearDownForeignThreadGc:
    // both sites in verifproxy.nim sit behind `when defined(setupForeignThreadGc)`
    // and nothing defines it. So a second thread has no GC state at all and
    // segfaults inside startVerifProxy — reproduced deterministically against
    // the real archive, same network and config both times: new thread -> 139
    // (SIGSEGV), same thread -> clean, with a fresh Context returned.
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
    int64_t m_startedAt = 0;

    ProxyConfig m_cfg;
    EmitFn m_emit;

    // Live slots, so shutdown can release anyone still waiting.
    std::deque<std::weak_ptr<CallSlot>> m_pending;
};
