// Where the library's beacon URLs come from: `beaconApiUrls`, or the providers
// enabled in beacon_api_module, read at start().

#include <string>
#include <vector>

#include <logos_test.h>
#include <nlohmann/json.hpp>

#include "proxy_config.h"
#include "verified_proxy_impl.h"
#include "mocks/mock_libverifproxy.h"

using json = nlohmann::json;

namespace {

const std::string kRoot = "0x" + std::string(64, 'a');

json moduleAnswer(json providers, json extra = json::object()) {
    json config = { { "providers", std::move(providers) }, { "proxyRequired", false }, { "timeoutSecs", 15 } };
    config.update(extra);
    return { { "ok", true }, { "chainId", 11155111 }, { "config", config } };
}

json provider(const std::string& id, const std::string& endpoint, bool enabled = true) {
    return { { "id", id }, { "endpoint", endpoint }, { "enabled", enabled }, { "source", "default" } };
}

json impljson(json extra = json::object()) {
    json j = {
        { "network", "sepolia" }, { "trustedBlockRoot", kRoot },
        { "executionApiUrls", json::array({ "https://exec.example" }) },
        { "startTimeoutMs", 5000 }, { "drainTimeoutMs", 500 }, { "pumpIntervalMs", 20 },
        { "keepAlive", "off" },
    };
    j.update(extra);
    return j;
}

} // namespace

// ── config ──────────────────────────────────────────────────────────────────

LOGOS_TEST(beacon_source_follows_whether_the_urls_were_edited) {
    auto t = LogosTestContext("verified_proxy_module");
    const json base = { { "network", "sepolia" }, { "trustedBlockRoot", kRoot } };
    ProxyConfig out;
    std::string err;

    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(base, out, err));
    LOGOS_ASSERT_EQ(out.beaconSource, std::string("module"));

    json same = base;
    same["beaconApiUrls"] = json::array({ networkProfile("sepolia")->beaconApiUrl });
    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(same, out, err));
    LOGOS_ASSERT_EQ(out.beaconSource, std::string("module"));

    json edited = base;
    edited["beaconApiUrls"] = json::array({ "https://my.beacon.example" });
    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(edited, out, err));
    LOGOS_ASSERT_EQ(out.beaconSource, std::string("urls"));

    edited["beaconSource"] = "module";
    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(edited, out, err));
    LOGOS_ASSERT_EQ(out.beaconSource, std::string("module"));
    LOGOS_ASSERT_EQ(out.beaconApiUrls[0], std::string("https://my.beacon.example"));
}

LOGOS_TEST(beacon_source_round_trips_and_is_validated) {
    auto t = LogosTestContext("verified_proxy_module");
    json in = { { "network", "sepolia" }, { "trustedBlockRoot", kRoot }, { "beaconSource", "urls" } };
    ProxyConfig out;
    std::string err;
    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(in, out, err));
    ProxyConfig again;
    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(out.raw(), again, err));
    LOGOS_ASSERT_EQ(again.beaconSource, std::string("urls"));

    in["beaconSource"] = "carrier-pigeon";
    LOGOS_ASSERT_FALSE(ProxyConfig::fromJson(in, out, err));
    LOGOS_ASSERT_CONTAINS(err, "beaconSource");
}

LOGOS_TEST(module_mode_needs_no_beacon_urls) {
    auto t = LogosTestContext("verified_proxy_module");
    json in = { { "network", "sepolia" }, { "trustedBlockRoot", kRoot },
                { "beaconSource", "module" }, { "beaconApiUrls", json::array() } };
    ProxyConfig out;
    std::string err;
    LOGOS_ASSERT_TRUE(ProxyConfig::fromJson(in, out, err));
    in["beaconSource"] = "urls";
    LOGOS_ASSERT_FALSE(ProxyConfig::fromJson(in, out, err));
}

// ── beacon_api_module's answer ──────────────────────────────────────────────

LOGOS_TEST(the_modules_enabled_providers_become_the_beacon_urls_in_order) {
    auto t = LogosTestContext("verified_proxy_module");
    std::string err;
    const auto urls = beaconUrlsFromModule(moduleAnswer(json::array({
        provider("lodestar", "https://lodestar-sepolia.chainsafe.io"),
        provider("off", "https://off.example", false),
        provider("nimbus-unstable", "http://unstable.sepolia.beacon-api.nimbus.team"),
    })), err);
    LOGOS_ASSERT_EQ(json(urls), json::array({
        "https://lodestar-sepolia.chainsafe.io", "http://unstable.sepolia.beacon-api.nimbus.team" }));
    LOGOS_ASSERT_TRUE(err.empty());

    ProxyConfig c;
    c.beaconApiUrls = urls;
    LOGOS_ASSERT_EQ(json::parse(c.toUpstreamJson())["beaconApiUrls"].get<std::string>(),
                    std::string("https://lodestar-sepolia.chainsafe.io,http://unstable.sepolia.beacon-api.nimbus.team"));
}

LOGOS_TEST(a_chain_set_to_go_through_a_proxy_is_refused) {
    auto t = LogosTestContext("verified_proxy_module");
    const json one = json::array({ provider("lodestar", "https://lodestar-sepolia.chainsafe.io") });
    std::string err;

    LOGOS_ASSERT_TRUE(beaconUrlsFromModule(moduleAnswer(one, { { "proxy", "socks5h://127.0.0.1:9050" } }), err).empty());
    LOGOS_ASSERT_CONTAINS(err, "proxy");
    LOGOS_ASSERT_TRUE(beaconUrlsFromModule(moduleAnswer(one, { { "proxyRequired", true } }), err).empty());
    LOGOS_ASSERT_CONTAINS(err, "proxy");

    LOGOS_ASSERT_EQ(beaconUrlsFromModule(moduleAnswer(one, { { "proxy", nullptr } }), err).size(), size_t(1));
    LOGOS_ASSERT_EQ(beaconUrlsFromModule(moduleAnswer(one, { { "proxy", "" } }), err).size(), size_t(1));
}

LOGOS_TEST(no_usable_provider_or_a_bad_answer_is_refused_in_words) {
    auto t = LogosTestContext("verified_proxy_module");
    std::string err;

    LOGOS_ASSERT_TRUE(beaconUrlsFromModule(moduleAnswer(json::array({
        provider("off", "https://off.example", false) })), err).empty());
    LOGOS_ASSERT_CONTAINS(err, "no enabled provider");

    LOGOS_ASSERT_TRUE(beaconUrlsFromModule(
        json{ { "ok", false }, { "code", "unknown_chain" }, { "error", "unknown chain 5" } }, err).empty());
    LOGOS_ASSERT_CONTAINS(err, "unknown chain 5");

    LOGOS_ASSERT_TRUE(beaconUrlsFromModule(json::parse("[1]"), err).empty());
    LOGOS_ASSERT_CONTAINS(err, "malformed");

    LOGOS_ASSERT_TRUE(beaconUrlsFromModule(moduleAnswer(json::array({
        provider("ftp", "ftp://beacon.example") })), err).empty());
    LOGOS_ASSERT_CONTAINS(err, "ftp://beacon.example");
}

// ── start() ─────────────────────────────────────────────────────────────────

LOGOS_TEST(module_mode_refuses_to_start_when_the_module_is_unreachable) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    VerifiedProxyImpl impl;
    LOGOS_ASSERT_TRUE(impl.configure(impljson()).success);
    const auto r = impl.start();
    LOGOS_ASSERT_FALSE(r.success);
    LOGOS_ASSERT_CONTAINS(r.error, "beaconSource \"module\"");
    LOGOS_ASSERT_CONTAINS(r.error, "unreachable");
    LOGOS_ASSERT_EQ(impl.status()["beacon"]["source"].get<std::string>(), std::string("module"));
}

LOGOS_TEST(status_shows_the_beacon_urls_the_last_start_used_redacted) {
    auto t = LogosTestContext("verified_proxy_module");
    mockReset();
    VerifiedProxyImpl impl;
    LOGOS_ASSERT_TRUE(impl.configure(impljson({
        { "beaconApiUrls", json::array({ "https://beacon.example/v1/secret-key" }) } })).success);
    LOGOS_ASSERT_TRUE(impl.status()["beacon"]["urls"].empty());
    LOGOS_ASSERT_TRUE(impl.start().success);
    const auto again = impl.start();
    LOGOS_ASSERT_FALSE(again.success);
    LOGOS_ASSERT_CONTAINS(again.error, "already started");
    const json beacon = impl.status()["beacon"];
    impl.stop();
    LOGOS_ASSERT_EQ(beacon["source"].get<std::string>(), std::string("urls"));
    LOGOS_ASSERT_EQ(beacon["urls"], json::array({ "https://beacon.example/<redacted>" }));
}
