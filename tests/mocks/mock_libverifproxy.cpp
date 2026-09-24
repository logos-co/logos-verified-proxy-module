// Link-time mock of libverifproxy for unit tests.
//
// The essential difference from logos-storage-module's mock_libstorage.cpp:
// that one fires callbacks SYNCHRONOUSLY, so its waitSync never actually
// waits. Ours must not — the whole design under test is "commands cross to a
// proxy thread and completions arrive only from the pump", and a synchronous
// mock would exercise none of it. So completions queue here and are drained
// ONLY by processVerifProxyTasks.
//
// strdup here pairs with free() in freeNimAllocatedString below, which turns a
// missing or doubled release into an ASan/LSan failure instead of an invisible
// production leak.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <logos_clib_mock.h>

extern "C" {
#include "lib/verifproxy.h"
}

#include "mock_libverifproxy.h"

namespace {

struct MockCtx {
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::deque<std::function<void()>> completions;
};
MockCtx g_ctx;
std::atomic<bool> g_holdCompletions{false};

// Thread-affinity ledger and a global call-ordering log: the two invariants
// that matter most here and the two LogosCMockStore cannot express.
std::mutex g_obsMu;
std::unordered_map<std::string, std::thread::id> g_threadOf;
std::vector<std::string> g_order;
std::vector<std::string> g_proxyMethods;
std::unordered_map<std::string, std::string> g_params;

void observe(const char* fn) {
    std::lock_guard<std::mutex> lk(g_obsMu);
    g_threadOf.emplace(fn, std::this_thread::get_id());
    g_order.emplace_back(fn);
}

void enqueueCompletion(const char* fn, Context* c, CallBackProc cb, void* ud) {
    LOGOS_CMOCK_RECORD(fn);
    observe(fn);

    const int status = LOGOS_CMOCK_RETURN(int, std::string(fn) + "_status");
    if (status == mockNeverCompletes()) return;   // sentinel: no completion, ever

    const char* res = LOGOS_CMOCK_RETURN_STRING(fn);
    std::string payload = res ? res : "\"0x0\"";

    std::lock_guard<std::mutex> lk(g_ctx.mu);
    g_ctx.completions.push_back([c, cb, ud, status, payload] {
        cb(c, status, strdup(payload.c_str()), ud);
    });
}

} // namespace

// -- test-visible accessors --------------------------------------------------

std::thread::id mockThreadOf(const std::string& fn) {
    std::lock_guard<std::mutex> lk(g_obsMu);
    auto it = g_threadOf.find(fn);
    return it == g_threadOf.end() ? std::thread::id{} : it->second;
}

std::vector<std::string> mockCallOrder() {
    std::lock_guard<std::mutex> lk(g_obsMu);
    return g_order;
}

std::vector<std::string> mockProxyMethods() {
    std::lock_guard<std::mutex> lk(g_obsMu);
    return g_proxyMethods;
}

void mockReset() {
    {
        std::lock_guard<std::mutex> lk(g_obsMu);
        g_threadOf.clear();
        g_order.clear();
        g_proxyMethods.clear();
        g_params.clear();
    }
    std::lock_guard<std::mutex> lk(g_ctx.mu);
    g_ctx.completions.clear();
    g_ctx.stop = false;
    g_holdCompletions = false;
}

std::string mockParamsOf(const std::string& method) {
    std::lock_guard<std::mutex> lk(g_obsMu);
    const auto it = g_params.find(method);
    return it == g_params.end() ? std::string() : it->second;
}

size_t mockPendingCompletions() {
    std::lock_guard<std::mutex> lk(g_ctx.mu);
    return g_ctx.completions.size();
}

void mockHoldCompletions(bool hold) {
    g_holdCompletions.store(hold, std::memory_order_release);
}

// -- the mocked C surface ----------------------------------------------------

extern "C" void NimMain(void) {
    LOGOS_CMOCK_RECORD("NimMain");
    observe("NimMain");
}

extern "C" Context* startVerifProxy(char* /*configJson*/,
                                    ExecutionTransportProc,
                                    BeaconTransportProc) {
    LOGOS_CMOCK_RECORD("startVerifProxy");
    observe("startVerifProxy");

    // Model the BLOCKING prologue so a test can prove start() never runs it on
    // the dispatch thread.
    if (const int d = LOGOS_CMOCK_RETURN(int, "startVerifProxy_delay_ms"); d > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(d));

    if (LOGOS_CMOCK_RETURN(int, "startVerifProxy_fail") != 0) return nullptr;

    g_ctx.stop = false;
    return reinterpret_cast<Context*>(&g_ctx);
}

extern "C" int processVerifProxyTasks(Context*) {
    LOGOS_CMOCK_RECORD("processVerifProxyTasks");
    observe("processVerifProxyTasks");

    // Mirrors the Nim source: ctx.stop is checked BEFORE polling, so once
    // stopped no completion can ever fire.
    if (g_ctx.stop) return RET_CANCELLED;

    std::function<void()> job;
    {
        std::lock_guard<std::mutex> lk(g_ctx.mu);
        if (!g_holdCompletions.load(std::memory_order_acquire)
            && !g_ctx.completions.empty()) {
            job = std::move(g_ctx.completions.front());
            g_ctx.completions.pop_front();
        }
    }
    if (job) job();
    return RET_SUCCESS;
}

extern "C" void proxyCall(Context* c, char* name, char* params,
                          CallBackProc cb, void* ud) {
    // Record the method name too, so tests can assert WHICH RPC was issued
    // (the heartbeat in particular), and the params so they can assert HOW an
    // argument was encoded — upstream reads some of them as typed quantities.
    LOGOS_CMOCK_RECORD(std::string("proxyCall:") + (name ? name : ""));
    {
        std::lock_guard<std::mutex> lk(g_obsMu);
        g_proxyMethods.emplace_back(name ? name : "");
        g_params[name ? name : ""] = params ? params : "";
    }
    enqueueCompletion("proxyCall", c, cb, ud);
}

extern "C" void stopVerifProxy(Context*) {
    LOGOS_CMOCK_RECORD("stopVerifProxy");
    observe("stopVerifProxy");
    g_ctx.stop = true;
}

extern "C" void freeContext(Context*) {
    LOGOS_CMOCK_RECORD("freeContext");
    observe("freeContext");
}

extern "C" void freeNimAllocatedString(char* res) {
    LOGOS_CMOCK_RECORD("freeNimAllocatedString");
    free(res);   // pairs with the strdup above — ASan catches a missed release
}

// Queued like proxyCall, so a test can hold or fail the first sync.
extern "C" void nvp_eth_sync(Context* c, CallBackProc cb, void* ud) {
    enqueueCompletion("nvp_eth_sync", c, cb, ud);
}

extern "C" void nvp_op_sync(Context* c, CallBackProc cb, void* ud) {
    enqueueCompletion("nvp_op_sync", c, cb, ud);
}

// Synchronous, like upstream: one mainnet slot unless a test overrides it.
extern "C" void nvp_eth_syncInterval(Context* c, CallBackProc cb, void* ud) {
    LOGOS_CMOCK_RECORD("nvp_eth_syncInterval");
    observe("nvp_eth_syncInterval");
    const char* res = LOGOS_CMOCK_RETURN_STRING("nvp_eth_syncInterval");
    cb(c, RET_SUCCESS, strdup(res ? res : "\"0x2ee0\""), ud);
}
