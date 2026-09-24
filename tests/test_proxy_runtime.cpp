// ProxyRuntime — the thread, the queue, the pump and the shutdown ordering.
//
// The mock queues completions and drains them ONLY from
// processVerifProxyTasks, so these tests exercise the real cross-thread design
// rather than a synchronous stand-in.

#include <atomic>
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

LOGOS_TEST(runtime_start_publishes_the_run_before_it_returns) {
    // Running and m_runActive used to be set AFTER start()'s latch was tripped,
    // so the caller could be handed `success` and then told "proxy not running"
    // by the very next call. Looped because it is a race, not a certainty: on
    // ubuntu CI it fired often enough to hang the job twice running.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    for (int i = 0; i < 500; ++i) {
        ProxyRuntime rt(nullptr);
        LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
        LOGOS_ASSERT_TRUE(rt.running());
        LOGOS_ASSERT_TRUE(rt.call("eth_blockNumber", json::array()).success);
        LOGOS_ASSERT_TRUE(rt.stop().success);
    }
}

LOGOS_TEST(runtime_destructor_ends_a_run_that_came_up_after_start_gave_up) {
    // start() deliberately leaves a slow run going, so a caller that gives up
    // and destroys the runtime never sets m_stopRequested. The pump loop has to
    // answer to the destructor as well, or join() waits on a thread that has no
    // reason left to return.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("startVerifProxy_delay_ms").returns(600);

    ProxyConfig cfg = testConfig();
    cfg.startTimeoutMs = 100;

    const auto t0 = steady_clock::now();
    {
        ProxyRuntime rt(nullptr);
        LOGOS_ASSERT_FALSE(rt.start(cfg).success);
        // Destruct while startVerifProxy is still inside its prologue: the run
        // is not published yet, so ~ProxyRuntime skips stop().
        std::this_thread::sleep_for(milliseconds(150));
    }
    LOGOS_ASSERT_LT(duration_cast<milliseconds>(steady_clock::now() - t0).count(), 5000);
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

LOGOS_TEST(runtime_queues_calls_beyond_the_in_flight_ceiling) {
    // Multi-dispatch workers can arrive together. The reservation must happen
    // before enqueueing to the proxy thread; checking m_inFlight (raised later
    // by that thread) lets every caller race through the same open gate.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall_status").returns(mockNeverCompletes());

    ProxyConfig cfg = testConfig();
    cfg.maxInFlight = 2;
    cfg.callTimeoutMs = 400;
    cfg.queueTimeoutMs = 200;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);

    std::atomic<bool> go{false};
    std::vector<StdLogosResult> results(3);
    std::vector<std::thread> callers;
    for (size_t i = 0; i < results.size(); ++i) {
        callers.emplace_back([&, i] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            results[i] = rt.call("eth_blockNumber", json::array());
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& th : callers) th.join();

    int queueTimeouts = 0;
    for (const auto& r : results) {
        if (r.error.find("admission queue timed out") != std::string::npos) ++queueTimeouts;
    }
    LOGOS_ASSERT_EQ(queueTimeouts, 1);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall"), 2);
    LOGOS_ASSERT_EQ(rt.statusSnapshot()["counters"]["queueTimeouts"].get<int64_t>(), 1);
    rt.stop();
}

LOGOS_TEST(runtime_serves_the_admission_queue_in_fifo_order) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyConfig cfg = testConfig();
    cfg.maxInFlight = 1;
    cfg.callTimeoutMs = 3000;
    cfg.queueTimeoutMs = 2000;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    // After start(): the first sync has to complete for start() to return.
    mockHoldCompletions(true);

    StdLogosResult first;
    StdLogosResult second;
    StdLogosResult third;
    std::thread a([&] { first = rt.call("first", json::array()); });
    LOGOS_ASSERT_TRUE(spinUntil([&] { return t.cFunctionCallCount("proxyCall:first") == 1; }));

    std::thread b([&] { second = rt.call("second", json::array()); });
    LOGOS_ASSERT_TRUE(spinUntil([&] {
        return rt.statusSnapshot()["resources"]["queuedCalls"].get<int64_t>() == 1;
    }));
    std::thread c([&] { third = rt.call("third", json::array()); });
    LOGOS_ASSERT_TRUE(spinUntil([&] {
        return rt.statusSnapshot()["resources"]["queuedCalls"].get<int64_t>() == 2;
    }));

    // Nothing beyond the admitted first call reaches libverifproxy while its
    // slot is occupied. Releasing it drains the waiters in ticket order.
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall"), 1);
    mockHoldCompletions(false);
    a.join();
    b.join();
    c.join();

    LOGOS_ASSERT_TRUE(first.success);
    LOGOS_ASSERT_TRUE(second.success);
    LOGOS_ASSERT_TRUE(third.success);
    const auto methods = mockProxyMethods();
    LOGOS_ASSERT_EQ(methods.size(), static_cast<size_t>(3));
    LOGOS_ASSERT_EQ(methods[0], std::string("first"));
    LOGOS_ASSERT_EQ(methods[1], std::string("second"));
    LOGOS_ASSERT_EQ(methods[2], std::string("third"));
    const json counters = rt.statusSnapshot()["counters"];
    LOGOS_ASSERT_EQ(counters["callsQueued"].get<int64_t>(), 2);
    LOGOS_ASSERT_EQ(counters["queueTimeouts"].get<int64_t>(), 0);
    rt.stop();
}

LOGOS_TEST(runtime_expires_a_call_that_waits_too_long_in_the_admission_queue) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyConfig cfg = testConfig();
    cfg.maxInFlight = 1;
    cfg.callTimeoutMs = 3000;
    cfg.queueTimeoutMs = 150;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    // After start(): the first sync has to complete for start() to return.
    mockHoldCompletions(true);

    StdLogosResult first;
    std::thread active([&] { first = rt.call("first", json::array()); });
    LOGOS_ASSERT_TRUE(spinUntil([&] { return t.cFunctionCallCount("proxyCall:first") == 1; }));

    const auto t0 = steady_clock::now();
    const auto queued = rt.call("second", json::array());
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

    LOGOS_ASSERT_FALSE(queued.success);
    LOGOS_ASSERT_CONTAINS(queued.error, "admission queue timed out");
    LOGOS_ASSERT_CONTAINS(queued.error, "concurrent call ceiling");
    LOGOS_ASSERT_GE(elapsed.count(), 100);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall"), 1);
    LOGOS_ASSERT_EQ(rt.statusSnapshot()["counters"]["queueTimeouts"].get<int64_t>(), 1);

    mockHoldCompletions(false);
    active.join();
    LOGOS_ASSERT_TRUE(first.success);
    rt.stop();
}

LOGOS_TEST(runtime_derives_its_ceiling_from_the_posix_descriptor_budget) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyConfig cfg = testConfig();
    cfg.maxInFlight = 64;
    cfg.parallelBlockDownloads = 10; // estimate: 12 descriptors per call

    ProxyRuntime rt(nullptr, [] { return DescriptorSnapshot{ 256, 36 }; });
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    const json resources = rt.statusSnapshot()["resources"];
    LOGOS_ASSERT_EQ(resources["effectiveMaxInFlight"].get<int64_t>(), 13);
    LOGOS_ASSERT_EQ(resources["descriptorReserve"].get<int64_t>(), 64);
    LOGOS_ASSERT_EQ(resources["estimatedDescriptorsPerCall"].get<int64_t>(), 12);
    rt.stop();
}

LOGOS_TEST(runtime_refuses_to_start_without_one_safe_call_slot) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr, [] { return DescriptorSnapshot{ 256, 192 }; });
    const auto r = rt.start(testConfig());
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_CONTAINS(r.error, "not enough file descriptors");
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("startVerifProxy"), 0);
}

LOGOS_TEST(runtime_times_out_a_queued_call_when_other_process_work_consumes_the_budget) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    std::atomic<int> queries{0};
    ProxyRuntime rt(nullptr, [&] {
        // start() sees room; admission sees that another module has since used
        // it. 181 + one 12-descriptor call would cross the safe ceiling 192.
        return queries.fetch_add(1) == 0
            ? DescriptorSnapshot{ 256, 36 }
            : DescriptorSnapshot{ 256, 181 };
    });
    ProxyConfig cfg = testConfig();
    cfg.queueTimeoutMs = 150;
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);

    const auto t0 = steady_clock::now();
    const auto r = rt.call("eth_blockNumber", json::array());
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_CONTAINS(r.error, "admission queue timed out");
    LOGOS_ASSERT_CONTAINS(r.error, "file-descriptor budget");
    LOGOS_ASSERT_GE(elapsed.count(), 100);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall"), 0);
    LOGOS_ASSERT_EQ(rt.statusSnapshot()["counters"]["resourceRejections"].get<int64_t>(), 1);
    LOGOS_ASSERT_EQ(rt.statusSnapshot()["counters"]["queueTimeouts"].get<int64_t>(), 1);
    rt.stop();
}

LOGOS_TEST(runtime_admits_a_queued_call_after_external_descriptor_pressure_eases) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    std::atomic<int> queries{0};
    std::atomic<bool> pressure{true};
    ProxyRuntime rt(nullptr, [&] {
        if (queries.fetch_add(1) == 0) return DescriptorSnapshot{ 256, 36 };
        return pressure.load(std::memory_order_acquire)
            ? DescriptorSnapshot{ 256, 181 }
            : DescriptorSnapshot{ 256, 36 };
    });
    ProxyConfig cfg = testConfig();
    cfg.queueTimeoutMs = 2000;
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);

    StdLogosResult result;
    std::thread caller([&] { result = rt.call("eth_blockNumber", json::array()); });
    LOGOS_ASSERT_TRUE(spinUntil([&] {
        return rt.statusSnapshot()["resources"]["queuedCalls"].get<int64_t>() == 1;
    }));
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall"), 0);

    pressure.store(false, std::memory_order_release);
    caller.join();
    LOGOS_ASSERT_TRUE(result.success);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall"), 1);
    LOGOS_ASSERT_EQ(rt.statusSnapshot()["counters"]["queueTimeouts"].get<int64_t>(), 0);
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

LOGOS_TEST(runtime_releases_an_admission_queued_caller_when_the_proxy_stops) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall_status").returns(mockNeverCompletes());

    ProxyConfig cfg = testConfig();
    cfg.maxInFlight = 1;
    cfg.callTimeoutMs = 10000;
    cfg.queueTimeoutMs = 10000;
    cfg.drainTimeoutMs = 200;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);

    StdLogosResult activeResult;
    StdLogosResult queuedResult;
    std::thread active([&] { activeResult = rt.call("first", json::array()); });
    LOGOS_ASSERT_TRUE(spinUntil([&] { return t.cFunctionCallCount("proxyCall:first") == 1; }));
    std::thread queued([&] { queuedResult = rt.call("second", json::array()); });
    LOGOS_ASSERT_TRUE(spinUntil([&] {
        return rt.statusSnapshot()["resources"]["queuedCalls"].get<int64_t>() == 1;
    }));

    const auto t0 = steady_clock::now();
    rt.stop();
    active.join();
    queued.join();
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - t0);

    LOGOS_ASSERT_FALSE(activeResult.success);
    LOGOS_ASSERT_CONTAINS(activeResult.error, "shutting down");
    LOGOS_ASSERT_FALSE(queuedResult.success);
    LOGOS_ASSERT_CONTAINS(queuedResult.error, "shutting down");
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall"), 1);
    LOGOS_ASSERT_LT(elapsed.count(), 5000);
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

LOGOS_TEST(runtime_heartbeat_issues_the_beat_only_when_enabled) {
    // The beat is eth_getBlockByNumber("latest"): unlike the eth_syncing it
    // replaced it reaches an execution backend and verifies the block, so a
    // backend that has gone ineligible shows up in status().
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
        LOGOS_ASSERT_GT(t.cFunctionCallCount("proxyCall:eth_getBlockByNumber"), 1);
        // One call per beat, not two: the beat answers with the head, so the
        // separate eth_blockNumber probe is gone.
        LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall:eth_blockNumber"), 0);
        LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall:eth_syncing"), 0);
    }
    {
        auto t = LogosTestContext("verified_proxy_module");
        mockReset();
        ProxyRuntime rt(nullptr);
        LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);   // keepAlive "off"
        std::this_thread::sleep_for(milliseconds(400));
        rt.stop();
        LOGOS_ASSERT_EQ(t.cFunctionCallCount("proxyCall:eth_getBlockByNumber"), 0);
    }
}

LOGOS_TEST(runtime_the_beat_records_the_head_it_verified) {
    // The head comes out of the beat itself now. Before any of this was wired,
    // status().head.blockNumber was a field that was read and never assigned,
    // so it stayed "" for the life of the process.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    // The wire shape: a block OBJECT whose `number` is a hex quantity string.
    t.mockCFunction("proxyCall").returns(R"({"number":"0xb0947c","hash":"0xabc"})");

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

    LOGOS_ASSERT_GT(t.cFunctionCallCount("proxyCall:eth_getBlockByNumber"), 0);
    LOGOS_ASSERT_EQ(s["head"]["blockNumber"].get<std::string>(), std::string("0xb0947c"));
    LOGOS_ASSERT_GT(s["head"]["updatedAt"].get<int64_t>(), 0);
}

LOGOS_TEST(runtime_the_head_is_refreshed_on_every_beat) {
    // The head used to come from a separate probe issued every FIFTH beat.
    // With the beat floored at a whole slot that would leave head.updatedAt a
    // minute stale, which is exactly where eth_rpc's readiness gate
    // (HEAD_STALE_SECS = 60) calls the proxy "not tracking".
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall").returns(R"({"number":"0xb0947c"})");

    ProxyConfig cfg = testConfig();
    cfg.keepAlive = "interval";
    cfg.keepAliveIntervalMs = 20;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    const bool beat = spinUntil([&] {
        return t.cFunctionCallCount("proxyCall:eth_getBlockByNumber") >= 4
            && rt.statusSnapshot()["head"]["updatedAt"].get<int64_t>() > 0;
    });
    const int64_t first = rt.statusSnapshot()["head"]["updatedAt"].get<int64_t>();
    const int beats = t.cFunctionCallCount("proxyCall:eth_getBlockByNumber");
    rt.stop();

    LOGOS_ASSERT_TRUE(beat);
    LOGOS_ASSERT_GE(beats, 4);
    LOGOS_ASSERT_GT(first, static_cast<int64_t>(0));
}

LOGOS_TEST(runtime_pending_slots_stay_bounded_across_many_beats) {
    // Every beat used to append weak_ptrs to m_pending that nothing removed
    // before teardown. make_shared puts the CallSlot's storage in the same
    // block as its control block, so a weak_ptr keeps a mutex, a condvar and
    // three strings alive — at the 1s beat of issue #11, thousands of them per
    // hour, in the class whose job is bounding this process' resources.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyConfig cfg = testConfig();
    cfg.keepAlive = "interval";
    cfg.keepAliveIntervalMs = 5;

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    const bool beat = spinUntil(
        [&] { return t.cFunctionCallCount("proxyCall:eth_getBlockByNumber") >= 25; });
    const json s = rt.statusSnapshot();
    rt.stop();

    LOGOS_ASSERT_TRUE(beat);
    // Anything near 25 is the old behaviour; a handful is the live ones plus
    // whatever has not been walked off the front yet.
    LOGOS_ASSERT_LT(s["counters"]["pendingSlots"].get<int64_t>(), static_cast<int64_t>(16));
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
    const bool beat = spinUntil(
        [&] { return t.cFunctionCallCount("proxyCall:eth_getBlockByNumber") >= 5; });
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
    t.mockCFunction("proxyCall").returns(R"({"number":"0xb0947c"})");

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

// ── The sync cycle (nimbus-eth1#4828: the library no longer syncs itself) ──

LOGOS_TEST(runtime_start_returns_only_after_the_first_sync) {
    // Every request fails with "sync first" until one nvp_eth_sync completes,
    // so start() handing back success before that would be a lie.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    const auto order = mockCallOrder();
    rt.stop();

    const int sync = lastIndexOf(order, "nvp_eth_sync");
    LOGOS_ASSERT_GE(sync, 0);
    LOGOS_ASSERT_LT(lastIndexOf(order, "startVerifProxy"), sync);
    LOGOS_ASSERT_EQ(t.cFunctionCallCount("nvp_op_sync"), 0);   // no OP configured
}

LOGOS_TEST(runtime_a_failing_first_sync_fails_start_with_its_reason) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("nvp_eth_sync_status").returns(RET_ERROR);
    t.mockCFunction("nvp_eth_sync").returns("UnavailableDataError: no beacon backend");

    ProxyConfig cfg = testConfig();
    cfg.startTimeoutMs = 300;

    ProxyRuntime rt(nullptr);
    const auto r = rt.start(cfg);
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_TRUE(r.error.find("no beacon backend") != std::string::npos);
    LOGOS_ASSERT_FALSE(rt.running());
    // The run is still live and retrying; stop() must end it.
    LOGOS_ASSERT_TRUE(rt.stop().success);
}

LOGOS_TEST(runtime_stop_before_the_first_sync_releases_start) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    mockHoldCompletions(true);

    ProxyRuntime rt(nullptr);
    StdLogosResult started;
    std::thread starter([&] { started = rt.start(testConfig()); });
    LOGOS_ASSERT_TRUE(spinUntil([&] { return t.cFunctionCallCount("nvp_eth_sync") == 1; }));

    const auto t0 = steady_clock::now();
    LOGOS_ASSERT_TRUE(rt.stop().success);
    starter.join();
    LOGOS_ASSERT_FALSE(started.success);
    // Released by teardown, not by start()'s 5 s timeout.
    LOGOS_ASSERT_LT(duration_cast<milliseconds>(steady_clock::now() - t0).count(), 3000);
    mockHoldCompletions(false);
}

LOGOS_TEST(runtime_syncs_every_interval_the_library_reports) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("nvp_eth_syncInterval").returns("\"0x14\"");   // 20 ms

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    const bool synced = spinUntil([&] { return t.cFunctionCallCount("nvp_eth_sync") >= 4; });
    const json s = rt.statusSnapshot();
    rt.stop();

    LOGOS_ASSERT_TRUE(synced);
    LOGOS_ASSERT_EQ(s["sync"]["intervalMs"].get<int64_t>(), 20);
    LOGOS_ASSERT_GT(s["sync"]["lastSyncedAt"].get<int64_t>(), 0);
    LOGOS_ASSERT_EQ(s["sync"]["failures"].get<int64_t>(), 0);
}

LOGOS_TEST(runtime_op_sync_follows_each_l1_sync_when_op_is_configured) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("nvp_eth_syncInterval").returns("\"0x14\"");

    ProxyConfig cfg = testConfig();
    cfg.opExecutionApiUrls = { "https://op.example" };

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(cfg).success);
    LOGOS_ASSERT_TRUE(spinUntil([&] { return t.cFunctionCallCount("nvp_op_sync") >= 2; }));
    const auto order = mockCallOrder();
    rt.stop();

    // Strict alternation: opSyncOnce reads L1 headers, so L1 goes first.
    std::vector<std::string> syncs;
    for (const auto& e : order)
        if (e == "nvp_eth_sync" || e == "nvp_op_sync") syncs.push_back(e);
    for (size_t i = 0; i < syncs.size(); ++i)
        LOGOS_ASSERT_EQ(syncs[i], std::string(i % 2 ? "nvp_op_sync" : "nvp_eth_sync"));
}

LOGOS_TEST(runtime_consecutive_sync_failures_degrade_the_proxy) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("nvp_eth_syncInterval").returns("\"0x14\"");

    ProxyRuntime rt(nullptr);
    LOGOS_ASSERT_TRUE(rt.start(testConfig()).success);
    t.mockCFunction("nvp_eth_sync_status").returns(RET_ERROR);
    t.mockCFunction("nvp_eth_sync").returns("BackendFetchError: 503");
    const bool degraded = spinUntil([&] {
        return rt.statusSnapshot()["state"].get<std::string>() == "degraded";
    });
    const json s = rt.statusSnapshot();

    // And back once the sync recovers.
    t.mockCFunction("nvp_eth_sync_status").returns(RET_SUCCESS);
    const bool recovered = spinUntil([&] { return rt.running(); });
    rt.stop();

    LOGOS_ASSERT_TRUE(degraded);
    LOGOS_ASSERT_GE(s["sync"]["consecutiveFailures"].get<int64_t>(), 3);
    LOGOS_ASSERT_TRUE(s["sync"]["lastError"].get<std::string>().find("503") != std::string::npos);
    LOGOS_ASSERT_TRUE(recovered);
}
