// ProxyRuntime — the thread, the queue, the pump and the shutdown ordering.
//
// The mock queues completions and drains them ONLY from
// processVerifProxyTasks, so these tests exercise the real cross-thread design
// rather than a synchronous stand-in.

#include <chrono>
#include <thread>

#include <logos_test.h>
#include <nlohmann/json.hpp>

#include "proxy_config.h"
#include "proxy_runtime.h"
#include "mocks/mock_libverifproxy.h"

extern "C" {
#include "lib/verifproxy.h"   // RET_* status codes
}

using json = nlohmann::json;
using namespace std::chrono;

namespace {

ProxyConfig testConfig() {
    ProxyConfig c;
    c.network = "sepolia";
    c.trustedBlockRoot = "0x" + std::string(64, 'a');
    c.executionApiUrls = { "https://exec.example" };
    c.beaconApiUrls    = { "https://beacon.example" };
    // Generous on purpose. Tests that exercise a TIMEOUT set their own short
    // value; every other test only needs the call to complete, and a tight
    // budget here made them fail under a parallel nix build rather than merely
    // run slower. Observed: runtime_confines_every_c_call_to_one_non_caller_thread
    // failing at 1500ms on a loaded machine and passing on a quiet one.
    c.callTimeoutMs    = 15000;
    c.startTimeoutMs   = 5000;
    c.drainTimeoutMs   = 500;
    c.pumpIntervalMs   = 20;
    c.keepAlive        = "off";      // most tests do not want heartbeat noise
    return c;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& e : v) if (e == s) return true;
    return false;
}

/// Index of the LAST occurrence of `s`, or -1.
int lastIndexOf(const std::vector<std::string>& v, const std::string& s) {
    for (int i = static_cast<int>(v.size()) - 1; i >= 0; --i)
        if (v[static_cast<size_t>(i)] == s) return i;
    return -1;
}

/// Spin until `pred` holds or `budgetMs` elapses. Sleeping a fixed interval and
/// hoping N heartbeats fit inside it makes a test that is green on an idle
/// machine and red under a parallel nix build; this makes a loaded builder
/// slower rather than flaky.
template <typename Pred>
bool spinUntil(Pred pred, int budgetMs = 8000) {
    const auto deadline = steady_clock::now() + milliseconds(budgetMs);
    while (steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(milliseconds(5));
    }
    return pred();
}

} // namespace

LOGOS_TEST(runtime_start_and_stop_round_trip) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr);
    const auto r = rt.start(testConfig());
    LOGOS_ASSERT_TRUE(r.success);
    LOGOS_ASSERT_TRUE(rt.running());

    const auto s = rt.stop();
    LOGOS_ASSERT_TRUE(s.success);
    LOGOS_ASSERT_FALSE(rt.running());
}

LOGOS_TEST(runtime_confines_every_c_call_to_one_non_caller_thread) {
    // The invariant that rots silently. setupForeignThreadGc /
    // tearDownForeignThreadGc are bound to startVerifProxy / stopVerifProxy, so
    // start, stop, the pump and every call must share one thread — and it must
    // not be the dispatch thread, because startVerifProxy blocks.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    LOGOS_ASSERT_TRUE(rt.call("eth_blockNumber", json::array()).success);
    rt.stop();

    const auto proxyThread = mockThreadOf("startVerifProxy");
    LOGOS_ASSERT_TRUE(proxyThread != std::thread::id{});
    LOGOS_ASSERT_TRUE(proxyThread != std::this_thread::get_id());

    // These are called on every cycle, so they must be recorded AND match.
    for (const char* fn : { "processVerifProxyTasks", "proxyCall",
                            "stopVerifProxy", "freeContext" }) {
        LOGOS_ASSERT_TRUE(mockThreadOf(fn) != std::thread::id{});
        LOGOS_ASSERT_TRUE(mockThreadOf(fn) == proxyThread);
    }

    // NimMain is once per PROCESS (std::call_once), so if an earlier test in
    // this binary already started a proxy it will not have been re-recorded
    // after mockReset(). Assert it only when it was actually observed here —
    // an unconditional check would make this test order-dependent.
    if (const auto nimMainThread = mockThreadOf("NimMain");
        nimMainThread != std::thread::id{}) {
        LOGOS_ASSERT_TRUE(nimMainThread == proxyThread);
    }
}

LOGOS_TEST(runtime_never_calls_NimMain_a_second_time) {
    // NimMain is process-global: a second call would re-initialise the Nim
    // runtime underneath live GC state.
    //
    // Assert the DELTA, not the absolute count. std::call_once fires once per
    // PROCESS, so whether this test sees 1 or 0 depends on whether an earlier
    // test already started a proxy — the invariant that actually matters is
    // that a restart adds none.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    { ProxyRuntime rt(nullptr); rt.start(testConfig()); rt.stop(); }
    const int afterFirst = t.cFunctionCallCount("NimMain");
    { ProxyRuntime rt(nullptr); rt.start(testConfig()); rt.stop(); }
    const int afterSecond = t.cFunctionCallCount("NimMain");

    LOGOS_ASSERT_EQ(afterSecond, afterFirst);
    LOGOS_ASSERT_LE(afterFirst, 1);
}

LOGOS_TEST(runtime_runs_the_blocking_prologue_off_the_callers_thread) {
    // startVerifProxy blocks for an unbounded prologue. start() may block the
    // CALLER — the latch is tripped by a different thread, so nothing starves —
    // but it must never run the prologue inline.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("startVerifProxy_delay_ms").returns(250);

    ProxyRuntime rt(nullptr);
    const auto t0 = steady_clock::now();
    const auto r = rt.start(testConfig());
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

    LOGOS_ASSERT_TRUE(r.success);
    LOGOS_ASSERT_GE(elapsed.count(), 200);                       // we did wait for it
    LOGOS_ASSERT_TRUE(mockThreadOf("startVerifProxy") != std::this_thread::get_id());
    rt.stop();
}

LOGOS_TEST(runtime_reports_a_null_start_and_refuses_calls_afterwards) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("startVerifProxy_fail").returns(1);

    ProxyRuntime rt(nullptr);
    const auto r = rt.start(testConfig());
    LOGOS_ASSERT_FALSE(r.success);
    // The C API has no error out-param, so the message must say so rather than
    // inventing a cause.
    LOGOS_ASSERT_CONTAINS(r.error, "NULL");
    LOGOS_ASSERT_FALSE(rt.running());

    const auto c = rt.call("eth_blockNumber", json::array());
    LOGOS_ASSERT_FALSE(c.success);
    LOGOS_ASSERT_CONTAINS(c.error, "not running");
    LOGOS_ASSERT_FALSE(t.cFunctionCalled("proxyCall"));
}

LOGOS_TEST(runtime_requires_a_pump_turn_to_complete_a_call) {
    // Proves the queue really crosses threads: no completion can be delivered
    // without processVerifProxyTasks running.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);

    const int before = t.cFunctionCallCount("processVerifProxyTasks");
    LOGOS_ASSERT_TRUE(rt.call("eth_blockNumber", json::array()).success);
    const int after = t.cFunctionCallCount("processVerifProxyTasks");
    LOGOS_ASSERT_GT(after, before);
    rt.stop();
}

LOGOS_TEST(runtime_rejects_a_params_value_that_is_not_an_array) {
    // Upstream does parseJson(params).getElems, which silently yields an empty
    // list for a non-array — so the caller would get "parameters missing"
    // instead of a useful message.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    const auto r = rt.call("eth_getBalance", json::object({ { "a", 1 } }));
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_CONTAINS(r.error, "array");
    rt.stop();
}

LOGOS_TEST(runtime_decodes_a_bare_json_encoded_result) {
    // The library returns Json.encode(value) with no JSON-RPC envelope.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall").returns("\"0x10d4f\"");

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    const auto r = rt.call("eth_blockNumber", json::array());
    LOGOS_ASSERT_TRUE(r.success);
    LOGOS_ASSERT_TRUE(r.value.is_string());
    LOGOS_ASSERT_EQ(r.value.get<std::string>(), std::string("0x10d4f"));
    rt.stop();
}

LOGOS_TEST(runtime_tolerates_the_non_json_error_payload) {
    // A Result failure yields a RAW "errType: errMsg" string, while a failed
    // Future yields a JSON-encoded one. Both must produce a readable error.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall_status").returns(RET_ERROR);
    t.mockCFunction("proxyCall").returns("VerificationError: unviable fork");

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    const auto r = rt.call("eth_blockNumber", json::array());
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_CONTAINS(r.error, "unviable fork");
    rt.stop();
}

LOGOS_TEST(runtime_reports_an_unknown_method_without_dying) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall_status").returns(RET_DESER_ERROR);
    t.mockCFunction("proxyCall").returns("unknown method");

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    const auto r = rt.call("eth_nonsense", json::array());
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_CONTAINS(r.error, "unknown method");
    rt.stop();
}

LOGOS_TEST(runtime_times_out_safely_when_a_call_never_completes) {
    // There is no per-call cancel in the C API, so a stalled future leaves the
    // slot live forever. Joint ownership (waiter + CallBox) is what makes a
    // late callback harmless; under ASan this test is the proof.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall_status").returns(mockNeverCompletes());

    ProxyConfig cfg = testConfig();
    cfg.callTimeoutMs = 300;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);

    const auto t0 = steady_clock::now();
    const auto r = rt.call("eth_blockNumber", json::array());
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_CONTAINS(r.error, "timed out");
    LOGOS_ASSERT_GE(elapsed.count(), 250);
    rt.stop();   // must not crash on the abandoned slot
}

LOGOS_TEST(runtime_rejects_calls_beyond_the_in_flight_ceiling) {
    // concurrency:"multi" spawns a QThread PER CALL, not a bounded pool, so
    // without admission control a runaway caller becomes an OOM rather than an
    // error string.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall_status").returns(mockNeverCompletes());

    ProxyConfig cfg = testConfig();
    cfg.maxInFlight = 2;
    cfg.callTimeoutMs = 400;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);

    std::vector<std::thread> hold;
    for (int i = 0; i < 2; ++i)
        hold.emplace_back([&rt] { rt.call("eth_blockNumber", json::array()); });

    // Give the pump time to dispatch both and raise m_inFlight.
    std::this_thread::sleep_for(milliseconds(150));
    const auto r = rt.call("eth_blockNumber", json::array());
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_CONTAINS(r.error, "in flight");

    for (auto& th : hold) th.join();
    rt.stop();
}

LOGOS_TEST(runtime_drains_before_stopping_and_frees_the_context_last) {
    // stopVerifProxy sets ctx.stop, and processVerifProxyTasks checks it BEFORE
    // polling — so anything still in flight when we stop can never complete.
    // Draining first is therefore load-bearing, not tidiness.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    LOGOS_ASSERT_TRUE(rt.call("eth_blockNumber", json::array()).success);
    rt.stop();

    const auto order = mockCallOrder();
    LOGOS_ASSERT_TRUE(contains(order, "stopVerifProxy"));
    LOGOS_ASSERT_TRUE(contains(order, "freeContext"));

    const int lastPump = lastIndexOf(order, "processVerifProxyTasks");
    const int stopAt   = lastIndexOf(order, "stopVerifProxy");
    const int freeAt   = lastIndexOf(order, "freeContext");

    LOGOS_ASSERT_LT(lastPump, stopAt);   // drained before stopping
    LOGOS_ASSERT_LT(stopAt, freeAt);     // freed only after stopping
    LOGOS_ASSERT_EQ(freeAt, static_cast<int>(order.size()) - 1);
}

LOGOS_TEST(runtime_releases_a_blocked_caller_when_the_proxy_stops) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall_status").returns(mockNeverCompletes());

    ProxyConfig cfg = testConfig();
    cfg.callTimeoutMs = 10000;   // far longer than the test would tolerate
    cfg.drainTimeoutMs = 200;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);

    StdLogosResult captured;
    std::thread caller([&] { captured = rt.call("eth_blockNumber", json::array()); });
    std::this_thread::sleep_for(milliseconds(150));

    const auto t0 = steady_clock::now();
    rt.stop();
    caller.join();
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

    LOGOS_ASSERT_FALSE(captured.success);
    LOGOS_ASSERT_CONTAINS(captured.error, "shutting down");
    LOGOS_ASSERT_LT(elapsed.count(), 5000);   // nobody waits out callTimeoutMs
}

LOGOS_TEST(runtime_pump_does_not_busy_spin_while_idle) {
    // The one CPU assertion stable enough for CI: bounded, not 10^6.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyConfig cfg = testConfig();
    cfg.pumpIntervalMs = 50;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    std::this_thread::sleep_for(milliseconds(500));
    const int pumps = t.cFunctionCallCount("processVerifProxyTasks");
    rt.stop();

    LOGOS_ASSERT_GT(pumps, 2);
    LOGOS_ASSERT_LT(pumps, 100);
}

LOGOS_TEST(runtime_heartbeat_issues_eth_syncing_only_when_enabled) {
    // processVerifProxyTasks only poll()s while pendingCalls > 0, so an idle
    // proxy does not advance its light client at all. eth_syncing is the
    // cheapest keep-alive: it drives beaconSync() and touches no execution
    // backend.
    {
        auto t = LogosTestContext("verified_proxy_module");
        mockReset();
        ProxyConfig cfg = testConfig();
        cfg.keepAlive = "interval";
        cfg.keepAliveIntervalMs = 100;

        ProxyRuntime rt(nullptr);
        LOGOS_ASSERT_TRUE(rt.start(cfg).success);
        std::this_thread::sleep_for(milliseconds(600));
        rt.stop();
        LOGOS_ASSERT_GT(t.cFunctionCallCount("proxyCall:eth_syncing"), 1);
    }
    {
        auto t = LogosTestContext("verified_proxy_module");
        mockReset();
        ProxyRuntime rt(nullptr);
        LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);   // keepAlive "off"
        std::this_thread::sleep_for(milliseconds(400));
        rt.stop();
        LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall:eth_syncing"), 0);
    }
}

LOGOS_TEST(runtime_head_probe_records_the_block_number) {
    // The heartbeat cannot report the head — upstream's eth_syncing answers a
    // hardcoded `false` — so a separate eth_blockNumber probe populates it.
    // Before this was wired, status().head.blockNumber was a field that was
    // read and never assigned, so it stayed "" for the life of the process.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall").returns("11572348");   // a bare JSON number

    ProxyConfig cfg = testConfig();
    cfg.keepAlive = "interval";
    cfg.keepAliveIntervalMs = 20;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    const bool got = spinUntil([&] {
        return !rt.statusSnapshot()["head"]["blockNumber"].get<std::string>().empty();
    });
    const json s = rt.statusSnapshot();
    rt.stop();
    LOGOS_ASSERT_TRUE(got);

    LOGOS_ASSERT_GT(t.cFunctionCallCount("proxyCall:eth_blockNumber"), 0);
    // Normalised to the "0x…" form status() documents, not the bare number
    // upstream returns. 11572348 == 0xb0947c.
    LOGOS_ASSERT_EQ(s["head"]["blockNumber"].get<std::string>(), std::string("0xb0947c"));
    LOGOS_ASSERT_GT(s["head"]["updatedAt"].get<int64_t>(), 0);
}

LOGOS_TEST(runtime_consecutive_heartbeat_failures_degrade_the_proxy) {
    // The error string of a failing heartbeat is the only machine-readable
    // sync-health signal the C ABI exposes. Three in a row is the threshold —
    // more than a blip, less than an outage.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall_status").returns(RET_ERROR);

    ProxyConfig cfg = testConfig();
    cfg.keepAlive = "interval";
    cfg.keepAliveIntervalMs = 20;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    const bool degraded = spinUntil([&] {
        return rt.statusSnapshot()["state"].get<std::string>() == "degraded";
    });
    const json s = rt.statusSnapshot();

    LOGOS_ASSERT_TRUE(degraded);
    LOGOS_ASSERT_EQ(s["state"].get<std::string>(), std::string("degraded"));
    LOGOS_ASSERT_GE(s["counters"]["heartbeatFailures"].get<int64_t>(), 3);
    // Degraded is not running — ok() must report unhealthy...
    LOGOS_ASSERT_FALSE(rt.running());
    // ...but the proxy is still a live, stoppable process.
    LOGOS_ASSERT_TRUE(rt.live());
    rt.stop();
}

LOGOS_TEST(runtime_a_healthy_heartbeat_leaves_state_running) {
    // The mirror of the test above: the streak must not latch. A proxy whose
    // heartbeats succeed stays Running no matter how many beats elapse.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyConfig cfg = testConfig();
    cfg.keepAlive = "interval";
    cfg.keepAliveIntervalMs = 20;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    // Wait for real beats rather than a fixed nap, so "still running" is a
    // statement about many successful heartbeats and not about a short sleep.
    const bool beat = spinUntil([&] { return t.cFunctionCallCount("proxyCall:eth_syncing") >= 5; });
    const json s = rt.statusSnapshot();
    rt.stop();

    LOGOS_ASSERT_TRUE(beat);
    LOGOS_ASSERT_EQ(s["state"].get<std::string>(), std::string("running"));
    LOGOS_ASSERT_EQ(s["counters"]["heartbeatFailures"].get<int64_t>(), 0);
}

LOGOS_TEST(runtime_restart_reuses_the_very_same_thread) {
    // THE regression test for a real crash: stop-then-start segfaulted the
    // module process (signal 11), reproduced deterministically against the real
    // archive with the network and config held identical across both runs.
    //
    // Cause: NimMain() binds the Nim runtime to the thread that calls it, and
    // this build compiles NEITHER setupForeignThreadGc NOR tearDownForeignThreadGc
    // — both sites in verifproxy.nim sit behind `when defined(setupForeignThreadGc)`
    // and nothing defines it. A second thread therefore has no GC state at all
    // and dies inside startVerifProxy. The old code created a fresh
    // std::thread per start(); the thread must instead outlive every run.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    const auto firstRunThread = mockThreadOf("startVerifProxy");
    LOGOS_ASSERT_TRUE(firstRunThread != std::thread::id{});
    LOGOS_ASSERT_TRUE(rt.stop().success);

    // Second run, same object.
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    const auto secondRunThread = mockThreadOf("startVerifProxy");
    LOGOS_ASSERT_TRUE(secondRunThread != std::thread::id{});

    // The invariant that keeps the Nim runtime alive.
    LOGOS_ASSERT_TRUE(secondRunThread == firstRunThread);
    LOGOS_ASSERT_TRUE(secondRunThread != std::this_thread::get_id());

    // And the second run is genuinely usable, not merely alive.
    LOGOS_ASSERT_TRUE(rt.call("eth_blockNumber", json::array()).success);
    rt.stop();
}

LOGOS_TEST(runtime_survives_several_restarts) {
    // The failure was on the SECOND run; make sure it is not merely pushed to
    // the third. Also pins the lifecycle guards: start() on a running proxy and
    // stop() on a stopped one are errors, not crashes or hangs.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr);
    std::thread::id firstThread{};

    for (int i = 0; i < 4; ++i) {
        LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
        const auto tid = mockThreadOf("startVerifProxy");
        if (i == 0) firstThread = tid; else LOGOS_ASSERT_TRUE(tid == firstThread);

        // Starting an already-running proxy is refused, not honoured.
        LOGOS_ASSERT_FALSE(rt.start(testConfig()).success);

        LOGOS_ASSERT_TRUE(rt.running());
        LOGOS_ASSERT_TRUE(rt.stop().success);
        LOGOS_ASSERT_FALSE(rt.running());

        // Stopping a stopped proxy is refused, not a second teardown.
        LOGOS_ASSERT_FALSE(rt.stop().success);
    }
}

LOGOS_TEST(runtime_restart_does_not_inherit_the_previous_runs_head) {
    // status().head describes the CURRENT run. Carrying the old value across a
    // restart would report a head from a chain the proxy is no longer on — the
    // exact situation that prompted this bug report, where the operator stopped,
    // switched network, and started again.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall").returns("11572348");

    ProxyConfig cfg = testConfig();
    cfg.keepAlive = "interval";
    cfg.keepAliveIntervalMs = 20;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    LOGOS_ASSERT_TRUE(spinUntil([&] {
        return !rt.statusSnapshot()["head"]["blockNumber"].get<std::string>().empty();
    }));
    rt.stop();

    // Restart with the heartbeat off so nothing can repopulate it.
    ProxyConfig quiet = testConfig();
    quiet.keepAlive = "off";
    LOGOS_ASSERT_TRUE(rt.start(quiet).success);
    const json s = rt.statusSnapshot();
    rt.stop();

    LOGOS_ASSERT_EQ(s["head"]["blockNumber"].get<std::string>(), std::string(""));
    LOGOS_ASSERT_EQ(s["head"]["updatedAt"].get<int64_t>(), 0);
}

LOGOS_TEST(runtime_status_reports_a_default_network_before_any_start) {
    // Documents WHY VerifiedProxyImpl::status() overrides network/chainId from
    // its own config: ProxyRuntime is only handed a config by start(), so until
    // then its snapshot describes a default-constructed one. A panel that
    // trusted this directly showed "chain 1 / mainnet" for a module configured
    // for sepolia, and warned the operator about a mismatch that did not exist.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr);
    const json s = rt.statusSnapshot();
    LOGOS_ASSERT_EQ(s["network"].get<std::string>(), std::string("mainnet"));
    LOGOS_ASSERT_EQ(s["chainId"].get<int64_t>(), 1);

    // After a start with a real config it reflects that config.
    ProxyConfig cfg = testConfig();          // sepolia
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    const json after = rt.statusSnapshot();
    rt.stop();
    LOGOS_ASSERT_EQ(after["network"].get<std::string>(), std::string("sepolia"));
    LOGOS_ASSERT_EQ(after["chainId"].get<int64_t>(), 11155111);
}
