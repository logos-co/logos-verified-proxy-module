// Configuration validation.
//
// These are the cheapest and highest-value tests in the suite: pure C++, no
// mock, no threads — and two of them guard a path that would otherwise take
// down the whole HOST process, because `startVerifProxy` reaches a Nim `quit()`
// for an unrecognised network or log level.

#include <logos_test.h>
#include <nlohmann/json.hpp>

#include "proxy_config.h"

using json = nlohmann::json;

namespace {

json baseConfig() {
    return json{
        { "network", "sepolia" },
        { "trustedBlockRoot", "0x" + std::string(64, 'a') },
        { "executionApiUrls", json::array({ "wss://eth.example/v2/secret-key" }) },
        { "beaconApiUrls",    json::array({ "https://beaconstate.info" }) },
    };
}

bool accepts(const json& j, std::string& err) {
    ProxyConfig c;
    return ProxyConfig::fromJson(j, c, err);
}

json withField(const char* key, const json& value) {
    json j = baseConfig();
    j[key] = value;
    return j;
}

} // namespace

LOGOS_TEST(config_accepts_a_minimal_valid_document) {
    std::string err;
    LOGOS_ASSERT_TRUE(accepts(baseConfig(), err));
    LOGOS_ASSERT_TRUE(err.empty());
}

// --- the two host-killing fields -------------------------------------------

LOGOS_TEST(config_rejects_every_network_outside_the_whitelist) {
    // Upstream's getMetadataForNetwork has only mainnet/hoodi/sepolia compiled
    // in; anything else falls through to `fatal` + `quit 1`. "holesky" and
    // "op-mainnet" are the realistic mistakes — both are real network names
    // that simply are not valid for the LIBRARY's JSON config.
    for (const char* bad : { "goerli", "holesky", "op-mainnet", "base-mainnet",
                             "Mainnet", "MAINNET", "" }) {
        std::string err;
        LOGOS_ASSERT_FALSE(accepts(withField("network", bad), err));
        LOGOS_ASSERT_CONTAINS(err, "network");
    }
    for (const char* good : { "mainnet", "sepolia", "hoodi" }) {
        std::string err;
        LOGOS_ASSERT_TRUE(accepts(withField("network", good), err));
    }
}

LOGOS_TEST(config_rejects_every_log_level_outside_the_whitelist) {
    // Nim's updateLogLevel raises ValueError, and setupLogging turns that into
    // `quit 1`. Note lowercase "info" is rejected: upstream is case-sensitive.
    for (const char* bad : { "verbose", "info", "Silly", "" }) {
        std::string err;
        LOGOS_ASSERT_FALSE(accepts(withField("logLevel", bad), err));
        LOGOS_ASSERT_CONTAINS(err, "logLevel");
    }
    for (const char* good : { "TRACE", "DEBUG", "INFO", "NOTICE",
                              "WARN", "ERROR", "FATAL", "NONE" }) {
        std::string err;
        LOGOS_ASSERT_TRUE(accepts(withField("logLevel", good), err));
    }
}

// --- ordinary validation ----------------------------------------------------

LOGOS_TEST(config_requires_a_well_formed_trusted_block_root) {
    std::string err;
    json noRoot = baseConfig();
    noRoot.erase("trustedBlockRoot");
    LOGOS_ASSERT_FALSE(accepts(noRoot, err));

    LOGOS_ASSERT_FALSE(accepts(withField("trustedBlockRoot", "0xdeadbeef"), err));
    LOGOS_ASSERT_FALSE(accepts(withField("trustedBlockRoot", std::string(64, 'a')), err));
    LOGOS_ASSERT_FALSE(accepts(withField("trustedBlockRoot", "0x" + std::string(64, 'z')), err));
    LOGOS_ASSERT_FALSE(accepts(withField("trustedBlockRoot", 42), err));
}

LOGOS_TEST(config_requires_both_backend_url_lists) {
    std::string err;
    LOGOS_ASSERT_FALSE(accepts(withField("executionApiUrls", json::array()), err));
    LOGOS_ASSERT_CONTAINS(err, "executionApiUrls");
    LOGOS_ASSERT_FALSE(accepts(withField("beaconApiUrls", json::array()), err));
    LOGOS_ASSERT_CONTAINS(err, "beaconApiUrls");
}

LOGOS_TEST(config_rejects_url_schemes_upstream_would_reject) {
    std::string err;
    for (const char* bad : { "ftp://x", "file:///etc/passwd", "eth.example", "" }) {
        LOGOS_ASSERT_FALSE(accepts(withField("beaconApiUrls", json::array({ bad })), err));
    }
    for (const char* good : { "http://a", "https://a", "ws://a", "wss://a" }) {
        LOGOS_ASSERT_TRUE(accepts(withField("beaconApiUrls", json::array({ good })), err));
    }
}

LOGOS_TEST(config_rejects_a_comma_inside_a_single_url) {
    // Upstream's format is one comma-separated string, so a comma in an entry
    // would silently become two URLs after we join. Catch it while the caller
    // can still see which entry is wrong.
    std::string err;
    LOGOS_ASSERT_FALSE(
        accepts(withField("executionApiUrls", json::array({ "https://a,https://b" })), err));
    LOGOS_ASSERT_CONTAINS(err, "comma");
}

LOGOS_TEST(config_accepts_upstreams_own_comma_separated_spelling) {
    // A caller pasting the upstream shape should not be punished for it.
    std::string err;
    ProxyConfig c;
    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(
        withField("executionApiUrls", "https://a,https://b"), c, err));
    LOGOS_ASSERT_EQ(c.executionApiUrls.size(), static_cast<size_t>(2));
}

LOGOS_TEST(config_rejects_nonsensical_module_knobs) {
    std::string err;
    LOGOS_ASSERT_FALSE(accepts(withField("callTimeoutMs", 0), err));
    LOGOS_ASSERT_FALSE(accepts(withField("startTimeoutMs", -1), err));
    LOGOS_ASSERT_FALSE(accepts(withField("maxInFlight", 0), err));
    LOGOS_ASSERT_FALSE(accepts(withField("keepAlive", "sometimes"), err));
    LOGOS_ASSERT_TRUE(accepts(withField("keepAlive", "continuous"), err));
    LOGOS_ASSERT_TRUE(accepts(withField("keepAlive", "off"), err));
}

// --- translation to the upstream shape --------------------------------------

LOGOS_TEST(config_translates_url_arrays_to_upstreams_comma_separated_strings) {
    ProxyConfig c;
    std::string err;
    json j = baseConfig();
    j["executionApiUrls"] = json::array({ "https://a", "https://b" });
    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(j, c, err));

    const json up = json::parse(c.toUpstreamJson());
    LOGOS_ASSERT_TRUE(up["executionApiUrls"].is_string());
    LOGOS_ASSERT_EQ(up["executionApiUrls"].get<std::string>(), std::string("https://a,https://b"));
    // Upstream's key is eth2Network, not `network`.
    LOGOS_ASSERT_EQ(up["eth2Network"].get<std::string>(), std::string("sepolia"));
    // Module-only knobs must NOT leak into the library's config.
    LOGOS_ASSERT_FALSE(up.contains("callTimeoutMs"));
    LOGOS_ASSERT_FALSE(up.contains("keepAlive"));
    LOGOS_ASSERT_FALSE(up.contains("tuning"));
}

LOGOS_TEST(config_maps_each_network_to_its_chain_id) {
    ProxyConfig c;
    std::string err;
    ProxyConfig::fromJson(withField("network", "mainnet"), c, err);
    LOGOS_ASSERT_EQ(c.expectedChainId(), static_cast<int64_t>(1));
    ProxyConfig::fromJson(withField("network", "sepolia"), c, err);
    LOGOS_ASSERT_EQ(c.expectedChainId(), static_cast<int64_t>(11155111));
    ProxyConfig::fromJson(withField("network", "hoodi"), c, err);
    LOGOS_ASSERT_EQ(c.expectedChainId(), static_cast<int64_t>(560048));
}

LOGOS_TEST(config_redacts_provider_credentials) {
    ProxyConfig c;
    std::string err;
    json j = baseConfig();
    j["executionApiUrls"] = json::array({
        "wss://eth-mainnet.g.alchemy.com/v2/SUPER-SECRET",
        "https://user:password@node.example/rpc?apikey=SECRET",
    });
    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(j, c, err));

    const std::string dumped = c.redacted().dump();
    LOGOS_ASSERT_FALSE(dumped.find("SUPER-SECRET") != std::string::npos);
    LOGOS_ASSERT_FALSE(dumped.find("password") != std::string::npos);
    LOGOS_ASSERT_FALSE(dumped.find("apikey=SECRET") != std::string::npos);
    // The host must survive, or the redaction is useless for diagnosis.
    LOGOS_ASSERT_CONTAINS(dumped, "eth-mainnet.g.alchemy.com");
}
