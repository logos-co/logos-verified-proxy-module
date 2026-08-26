// URL construction and response parsing for the finalized-root helper.
//
// Both halves are pure, so the whole shape of the request and the whole shape
// of what we accept back are testable without a beacon node or a socket.

#include <string>

#include <logos_test.h>

#include "beacon_client.h"

namespace bc = beacon_client;

LOGOS_TEST(beacon_url_appends_the_finalized_headers_path) {
    LOGOS_ASSERT_EQ(bc::finalizedHeaderUrl("https://example.org"),
                    "https://example.org/eth/v1/beacon/headers/finalized");
}

LOGOS_TEST(beacon_url_tolerates_trailing_slashes) {
    // A URL pasted from a browser very often carries one.
    LOGOS_ASSERT_EQ(bc::finalizedHeaderUrl("https://example.org///"),
                    "https://example.org/eth/v1/beacon/headers/finalized");
}

LOGOS_TEST(beacon_url_keeps_a_path_prefix) {
    LOGOS_ASSERT_EQ(bc::finalizedHeaderUrl("https://example.org/beacon"),
                    "https://example.org/beacon/eth/v1/beacon/headers/finalized");
}

LOGOS_TEST(beacon_accepts_only_http_schemes) {
    // ws:// and wss:// are valid beacon transports for the library itself, but
    // cannot serve the REST GET this helper makes.
    LOGOS_ASSERT_TRUE(bc::isHttpUrl("http://example.org"));
    LOGOS_ASSERT_TRUE(bc::isHttpUrl("https://example.org"));
    LOGOS_ASSERT_FALSE(bc::isHttpUrl("wss://example.org"));
    LOGOS_ASSERT_FALSE(bc::isHttpUrl("ws://example.org"));
    LOGOS_ASSERT_FALSE(bc::isHttpUrl("file:///etc/passwd"));
    LOGOS_ASSERT_FALSE(bc::isHttpUrl(""));
}

LOGOS_TEST(beacon_trims_surrounding_whitespace) {
    LOGOS_ASSERT_EQ(bc::trim("  https://example.org \n"), "https://example.org");
    LOGOS_ASSERT_EQ(bc::trim("   "), "");
}

LOGOS_TEST(beacon_parses_root_and_slot) {
    const std::string body = R"({
      "data": {
        "root": "0xabc",
        "header": { "message": { "slot": "12345" } }
      }
    })";
    std::string slot;
    LOGOS_ASSERT_EQ(bc::parseFinalizedRoot(body, slot), "0xabc");
    LOGOS_ASSERT_EQ(slot, "12345");
}

LOGOS_TEST(beacon_parse_tolerates_a_missing_slot) {
    // The slot is informational; only the root is load-bearing.
    std::string slot = "stale";
    LOGOS_ASSERT_EQ(bc::parseFinalizedRoot(R"({"data":{"root":"0xdef"}})", slot), "0xdef");
    LOGOS_ASSERT_EQ(slot, "");
}

LOGOS_TEST(beacon_parse_rejects_malformed_payloads) {
    std::string slot;
    LOGOS_ASSERT_EQ(bc::parseFinalizedRoot("not json", slot), "");
    LOGOS_ASSERT_EQ(bc::parseFinalizedRoot("[]", slot), "");
    LOGOS_ASSERT_EQ(bc::parseFinalizedRoot("{}", slot), "");
    LOGOS_ASSERT_EQ(bc::parseFinalizedRoot(R"({"data":{}})", slot), "");
    // A non-string root is the shape an error page could plausibly take.
    LOGOS_ASSERT_EQ(bc::parseFinalizedRoot(R"({"data":{"root":42}})", slot), "");
}
