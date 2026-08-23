// The JSON-RPC framing, tested without binding a socket.
//
// handleBody() is pure apart from its dispatch callback, so the whole protocol
// surface — envelopes, batches, notifications, error codes, and the one
// signature adaptation stock clients depend on — is exercised here.

#include <string>
#include <vector>

#include <logos_test.h>
#include <nlohmann/json.hpp>

#include "rpc_http_server.h"

using json = nlohmann::json;

namespace {

/// Records what reached the proxy, and answers a canned value.
struct Recorder {
    std::vector<std::pair<std::string, json>> seen;
    StdLogosResult next{ true, json("0x1"), "" };

    RpcHttpServer::Dispatch fn() {
        return [this](const std::string& m, const json& p) {
            seen.emplace_back(m, p);
            return next;
        };
    }
};

json call(const std::string& body, Recorder& r) {
    const std::string out = RpcHttpServer::handleBody(body, r.fn());
    return out.empty() ? json() : json::parse(out);
}

} // namespace

LOGOS_TEST(http_wraps_a_bare_result_in_a_jsonrpc_envelope) {
    // The library answers Json.encode(value) with no envelope, so the server is
    // what makes this a JSON-RPC endpoint at all.
    Recorder r;
    const json resp = call(R"({"jsonrpc":"2.0","id":7,"method":"eth_blockNumber","params":[]})", r);

    LOGOS_ASSERT_EQ(resp["jsonrpc"].get<std::string>(), std::string("2.0"));
    LOGOS_ASSERT_EQ(resp["id"].get<int>(), 7);
    LOGOS_ASSERT_EQ(resp["result"].get<std::string>(), std::string("0x1"));
    LOGOS_ASSERT_FALSE(resp.contains("error"));
    LOGOS_ASSERT_EQ(r.seen.size(), static_cast<size_t>(1));
    LOGOS_ASSERT_EQ(r.seen[0].first, std::string("eth_blockNumber"));
}

LOGOS_TEST(http_preserves_the_id_type) {
    // Clients use strings, numbers and null; echoing the wrong type breaks
    // correlation in a batch.
    Recorder r;
    LOGOS_ASSERT_TRUE(call(R"({"jsonrpc":"2.0","id":"abc","method":"eth_chainId"})", r)["id"].is_string());
    LOGOS_ASSERT_TRUE(call(R"({"jsonrpc":"2.0","id":42,"method":"eth_chainId"})", r)["id"].is_number());
}

// --- the adaptation stock clients depend on ---------------------------------

LOGOS_TEST(http_appends_optimisticStateFetch_for_the_three_methods_that_need_it) {
    // eth_call/estimateGas/createAccessList carry a THIRD positional parameter
    // upstream that the JSON-RPC spec does not have. ethers, viem, cast and
    // eth_rpc_module all send two, and the library answers "parameters
    // missing". Appending the default is what makes a stock client work.
    for (const char* m : { "eth_call", "eth_estimateGas", "eth_createAccessList",
                           "op_call",  "op_estimateGas",  "op_createAccessList" }) {
        Recorder r;
        const std::string body =
            std::string(R"({"jsonrpc":"2.0","id":1,"method":")") + m +
            R"(","params":[{"to":"0xabc","data":"0x"},"latest"]})";
        call(body, r);

        LOGOS_ASSERT_EQ(r.seen.size(), static_cast<size_t>(1));
        const json& p = r.seen[0].second;
        LOGOS_ASSERT_EQ(p.size(), static_cast<size_t>(3));
        LOGOS_ASSERT_TRUE(p[2].is_boolean());
        LOGOS_ASSERT_FALSE(p[2].get<bool>());
    }
}

LOGOS_TEST(http_does_not_override_an_explicit_optimisticStateFetch) {
    Recorder r;
    call(R"({"jsonrpc":"2.0","id":1,"method":"eth_call",
             "params":[{"to":"0xabc"},"latest",true]})", r);
    const json& p = r.seen[0].second;
    LOGOS_ASSERT_EQ(p.size(), static_cast<size_t>(3));
    LOGOS_ASSERT_TRUE(p[2].get<bool>());
}

LOGOS_TEST(http_leaves_other_methods_params_untouched) {
    Recorder r;
    call(R"({"jsonrpc":"2.0","id":1,"method":"eth_getBalance","params":["0xabc","latest"]})", r);
    LOGOS_ASSERT_EQ(r.seen[0].second.size(), static_cast<size_t>(2));
}

LOGOS_TEST(http_renders_a_bare_number_result_as_a_hex_quantity) {
    // Upstream is not uniform: eth_chainId and eth_gasPrice answer hex strings,
    // but eth_blockNumber answers a bare JSON number — which no client expects,
    // since every QUANTITY in the spec is a hex string. Measured against
    // sepolia, not assumed.
    Recorder r;
    r.next = { true, json(11546453u), "" };
    const json resp = call(R"({"jsonrpc":"2.0","id":1,"method":"eth_blockNumber"})", r);
    LOGOS_ASSERT_TRUE(resp["result"].is_string());
    LOGOS_ASSERT_EQ(resp["result"].get<std::string>(), std::string("0xb02f55"));
}

LOGOS_TEST(http_leaves_strings_and_objects_alone) {
    Recorder r;
    r.next = { true, json("0xaa36a7"), "" };
    LOGOS_ASSERT_EQ(call(R"({"jsonrpc":"2.0","id":1,"method":"eth_chainId"})", r)["result"]
                        .get<std::string>(), std::string("0xaa36a7"));

    r.next = { true, json{ { "baseFeePerGas", "0x3e03d63d" } }, "" };
    const json blk = call(R"({"jsonrpc":"2.0","id":1,"method":"eth_getBlockByNumber"})", r);
    LOGOS_ASSERT_TRUE(blk["result"].is_object());
    LOGOS_ASSERT_EQ(blk["result"]["baseFeePerGas"].get<std::string>(), std::string("0x3e03d63d"));
}

// --- errors ------------------------------------------------------------------

LOGOS_TEST(http_reports_a_parse_error_with_the_reserved_code) {
    Recorder r;
    const json resp = call("{not json", r);
    LOGOS_ASSERT_EQ(resp["error"]["code"].get<int>(), -32700);
    LOGOS_ASSERT_TRUE(resp["id"].is_null());
    LOGOS_ASSERT_TRUE(r.seen.empty());
}

LOGOS_TEST(http_rejects_named_params_clearly) {
    // The proxy's own dispatcher would silently see an empty list and answer
    // "parameters missing", which sends the caller looking in the wrong place.
    Recorder r;
    const json resp = call(R"({"jsonrpc":"2.0","id":1,"method":"eth_call","params":{"to":"0x"}})", r);
    LOGOS_ASSERT_EQ(resp["error"]["code"].get<int>(), -32602);
    LOGOS_ASSERT_CONTAINS(resp["error"]["message"].get<std::string>(), "array");
    LOGOS_ASSERT_TRUE(r.seen.empty());
}

LOGOS_TEST(http_requires_a_method_field) {
    Recorder r;
    LOGOS_ASSERT_EQ(call(R"({"jsonrpc":"2.0","id":1})", r)["error"]["code"].get<int>(), -32600);
}

LOGOS_TEST(http_maps_an_unknown_method_to_method_not_found) {
    Recorder r;
    r.next = { false, {}, "bad request: unknown method" };
    const json resp = call(R"({"jsonrpc":"2.0","id":1,"method":"eth_nope"})", r);
    LOGOS_ASSERT_EQ(resp["error"]["code"].get<int>(), -32601);
}

LOGOS_TEST(http_maps_a_verification_failure_to_a_server_error) {
    Recorder r;
    r.next = { false, {}, "VerificationError: unviable fork" };
    const json resp = call(R"({"jsonrpc":"2.0","id":1,"method":"eth_getBalance","params":["0x","latest"]})", r);
    LOGOS_ASSERT_EQ(resp["error"]["code"].get<int>(), -32000);
    LOGOS_ASSERT_CONTAINS(resp["error"]["message"].get<std::string>(), "unviable fork");
}

// --- batches and notifications ----------------------------------------------

LOGOS_TEST(http_answers_a_batch_in_order) {
    Recorder r;
    const json resp = call(R"([
        {"jsonrpc":"2.0","id":1,"method":"eth_blockNumber"},
        {"jsonrpc":"2.0","id":2,"method":"eth_chainId"}
    ])", r);
    LOGOS_ASSERT_TRUE(resp.is_array());
    LOGOS_ASSERT_EQ(resp.size(), static_cast<size_t>(2));
    LOGOS_ASSERT_EQ(resp[0]["id"].get<int>(), 1);
    LOGOS_ASSERT_EQ(resp[1]["id"].get<int>(), 2);
    LOGOS_ASSERT_EQ(r.seen.size(), static_cast<size_t>(2));
}

LOGOS_TEST(http_still_dispatches_a_notification_but_answers_nothing) {
    // No id means no response — but the call must still happen.
    Recorder r;
    const std::string out =
        RpcHttpServer::handleBody(R"({"jsonrpc":"2.0","method":"eth_blockNumber"})", r.fn());
    LOGOS_ASSERT_TRUE(out.empty());
    LOGOS_ASSERT_EQ(r.seen.size(), static_cast<size_t>(1));
}

LOGOS_TEST(http_drops_notifications_from_a_batch_response) {
    Recorder r;
    const json resp = call(R"([
        {"jsonrpc":"2.0","method":"eth_blockNumber"},
        {"jsonrpc":"2.0","id":9,"method":"eth_chainId"}
    ])", r);
    LOGOS_ASSERT_EQ(r.seen.size(), static_cast<size_t>(2));   // both dispatched
    LOGOS_ASSERT_EQ(resp.size(), static_cast<size_t>(1));     // one answered
    LOGOS_ASSERT_EQ(resp[0]["id"].get<int>(), 9);
}

LOGOS_TEST(http_rejects_an_empty_batch) {
    Recorder r;
    LOGOS_ASSERT_EQ(call("[]", r)["error"]["code"].get<int>(), -32600);
}
