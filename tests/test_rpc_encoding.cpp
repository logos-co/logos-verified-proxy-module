// How the generated eth_*/op_* wrappers encode their arguments.
//
// blockCount and the two transaction indices reach the library through
// unpackArg(..., Quantity), which takes a 0x-prefixed hex string and REJECTS a
// bare JSON number. Before nimbus #4771 they were read with getBiggestInt(), so
// a number worked and this encoding was invisible. The generator learns the
// distinction from c_frontend.nim; only these tests pin it.

#include <logos_test.h>
#include <nlohmann/json.hpp>

#include "verified_proxy_impl.h"
#include "mocks/mock_libverifproxy.h"

using json = nlohmann::json;

namespace {

json testConfigJson() {
    return json{
        { "network",          "sepolia" },
        { "trustedBlockRoot", "0x" + std::string(64, 'a') },
        { "executionApiUrls", json::array({ "https://exec.example" }) },
        { "beaconApiUrls",    json::array({ "https://beacon.example" }) },
        { "callTimeoutMs",    15000 },
        { "startTimeoutMs",   5000 },
        { "drainTimeoutMs",   500 },
        { "pumpIntervalMs",   20 },
        { "keepAlive",        "off" },
    };
}

}  // namespace

LOGOS_TEST(rpc_quantity_params_cross_the_ffi_as_hex_strings) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall").returns("null");

    VerifiedProxyImpl impl;
    LOGOS_ASSERT_TRUE(impl.configure(testConfigJson()).success);
    LOGOS_ASSERT_TRUE(impl.start().success);

    impl.ethFeeHistory(4, "latest", json::array({ 25, 50, 75 }));
    impl.ethGetTransactionByBlockNumberAndIndex("latest", 26);
    impl.ethGetTransactionByBlockHashAndIndex("0xdead", 0);
    impl.stop();

    LOGOS_ASSERT_EQ(mockParamsOf("eth_feeHistory"),
                    std::string(R"(["0x4","latest",[25,50,75]])"));
    LOGOS_ASSERT_EQ(mockParamsOf("eth_getTransactionByBlockNumberAndIndex"),
                    std::string(R"(["latest","0x1a"])"));
    LOGOS_ASSERT_EQ(mockParamsOf("eth_getTransactionByBlockHashAndIndex"),
                    std::string(R"(["0xdead","0x0"])"));
}

LOGOS_TEST(rpc_string_params_are_passed_through_untouched) {
    // The other half of the contract: only the quantity kind is rewritten.
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    t.mockCFunction("proxyCall").returns("null");

    VerifiedProxyImpl impl;
    LOGOS_ASSERT_TRUE(impl.configure(testConfigJson()).success);
    LOGOS_ASSERT_TRUE(impl.start().success);

    impl.ethGetBalance("0xabc", "latest");
    impl.stop();

    LOGOS_ASSERT_EQ(mockParamsOf("eth_getBalance"),
                    std::string(R"(["0xabc","latest"])"));
}
