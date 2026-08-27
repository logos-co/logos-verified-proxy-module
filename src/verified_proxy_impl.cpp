#include "verified_proxy_impl.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "beacon_client.h"
#include "proxy_config.h"
#include "proxy_runtime.h"
#include "rpc_http_server.h"

// Generated at build time. Only needed where modules() is used; included here
// so the impl header the generator parses stays free of codegen types.
// #include "logos_sdk.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

#ifndef VERIFIED_PROXY_MODULE_VERSION
#define VERIFIED_PROXY_MODULE_VERSION "0.0.0-dev"
#endif

// Stamped by the flake (preConfigure writes the header) so status() and
// libraryVersion() can name the exact upstream build — the library exposes no
// version symbol of its own. Guarded so a plain cmake build still works.
#if defined(__has_include)
#  if __has_include("verified_proxy_nimbus_rev.h")
#    include "verified_proxy_nimbus_rev.h"
#  endif
#endif
#ifndef VERIFIED_PROXY_NIMBUS_REV
#define VERIFIED_PROXY_NIMBUS_REV "unknown"
#endif

namespace {

/// Reads VERIFIED_PROXY_MODULE_CONFIG: inline JSON when the first non-space
/// character is '{', otherwise a path to a JSON file. Returns "" when unset or
/// unreadable. Mirrors logos-libp2p-module's documented deploy-time channel.
std::string readEnvConfig() {
    const char* raw = std::getenv("VERIFIED_PROXY_MODULE_CONFIG");
    if (!raw || !*raw) return {};
    std::string v(raw);
    const auto first = v.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && v[first] == '{') return v;
    std::ifstream f(v);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ---------------------------------------------------------------------------

VerifiedProxyImpl::VerifiedProxyImpl() {
    // Route the runtime's events onto the generated typed emitters. Safe from
    // any thread: emitEventImpl_ is marshalled by the host, and it is a no-op
    // outside a framework-provisioned context (unit tests).
    m_rt = std::make_unique<ProxyRuntime>(
        [this](const std::string& name, const std::string& payload) {
            if (name == "proxyStarted")           proxyStarted(payload);
            else if (name == "proxyStopped")      proxyStopped(payload);
            else if (name == "proxyStateChanged") proxyStateChanged(payload);
        });
}

VerifiedProxyImpl::~VerifiedProxyImpl() = default;

void VerifiedProxyImpl::onContextReady() {
    // Deploy-time config first, then the persisted one (which wins, since it is
    // what a user set through configure()).
    if (const std::string envCfg = readEnvConfig(); !envCfg.empty()) {
        try { configure(json::parse(envCfg)); } catch (const std::exception&) {}
    }
    if (instancePersistencePath().empty()) return;

    const fs::path p = fs::path(instancePersistencePath()) / "config.json";
    const std::string text = readFile(p);
    if (text.empty()) return;
    try {
        const json j = json::parse(text);
        if (configure(j).success && j.value("autoStart", false)) start();
    } catch (const std::exception&) {
        // A corrupt persisted config must not stop the module from loading;
        // configure() can still be called with a good one.
    }
}

// ── Configuration ───────────────────────────────────────────────────────────

StdLogosResult VerifiedProxyImpl::configure(const LogosMap& config) {
    ProxyConfig cfg;
    std::string err;
    if (!ProxyConfig::fromJson(config, cfg, err))
        return { false, {}, err };

    if (m_rt->running())
        return { false, {}, "cannot reconfigure while the proxy is running; call stop() first" };

    m_cfg = std::make_unique<ProxyConfig>(cfg);
    m_configured = true;

    if (!instancePersistencePath().empty()) {
        std::error_code ec;
        fs::create_directories(instancePersistencePath(), ec);
        std::ofstream f(fs::path(instancePersistencePath()) / "config.json");
        // Persist the RESOLVED config, not the caller's input and not the
        // redacted view.
        //
        // Not redacted, because this file is the module's private state under a
        // host-owned directory and a masked copy would be useless on restart.
        //
        // Resolved rather than as-supplied, so what comes back is what actually
        // ran. Persisting `{"network": "mainnet", "trustedBlockRoot": "…"}`
        // verbatim would re-derive the endpoints on every load, and a later
        // change to the default table would silently move a running deployment
        // onto different providers — which is not a trust problem, since
        // providers are untrusted by construction, but it does decide whether
        // eth_getProof works: an archive endpoint answers proofs at the
        // finalized header, a pruning one does not.
        if (f) f << cfg.raw().dump(2);
    }
    return { true, {}, "" };
}

LogosMap VerifiedProxyImpl::getConfig() {
    if (!m_configured || !m_cfg) return json::object();
    return m_cfg->redacted();
}

LogosMap VerifiedProxyImpl::getConfigUnredacted() {
    if (!m_configured || !m_cfg) return json::object();
    return m_cfg->raw();
}

LogosMap VerifiedProxyImpl::defaultConfig(const std::string& network) {
    if (!networkProfile(network)) return json::object();

    // Round-trip a default-constructed config through fromJson so the result is
    // exactly what configure() would produce for this network — including the
    // endpoint defaults it fills in — rather than a second, drifting copy of
    // the same defaults written out by hand.
    // Seed a syntactically valid placeholder root purely to get PAST
    // validation — fromJson requires one, deliberately — then blank it in the
    // result. The point of the round trip is that what comes back is exactly
    // what configure() would produce for this network, endpoint defaults
    // included, rather than a second copy of the same defaults written by hand.
    ProxyConfig cfg;
    std::string err;
    json seed = json::object();
    seed["network"] = network;
    seed["trustedBlockRoot"] = "0x" + std::string(64, '0');
    if (!ProxyConfig::fromJson(seed, cfg, err)) return json::object();

    json out = cfg.raw();
    out["trustedBlockRoot"] = "";
    return out;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

StdLogosResult VerifiedProxyImpl::start() {
    if (!m_configured)
        return { false, {}, "not configured — call configure() first" };

    auto r = m_rt->start(*m_cfg);
    if (!r.success || !m_cfg->httpEnabled) return r;

    // Forward every HTTP request through the SAME verified path the typed
    // methods use, so there is one verification path and one error shape.
    m_http = std::make_unique<RpcHttpServer>(
        [this](const std::string& method, const nlohmann::json& params) {
            return m_rt->call(method, params);
        });

    std::string httpErr;
    if (!m_http->start(m_cfg->httpHost,
                       static_cast<uint16_t>(m_cfg->httpPort), httpErr)) {
        // Fail the whole start rather than leave a half-started module: a
        // caller that asked for an endpoint and silently did not get one would
        // point a wallet at a dead port.
        m_http.reset();
        m_rt->stop();
        return { false, {}, "proxy started but the JSON-RPC endpoint could not: " + httpErr };
    }

    r.value = nlohmann::json{ { "chainId", m_cfg->expectedChainId() },
                              { "endpoint", m_http->endpoint() } };
    return r;
}

StdLogosResult VerifiedProxyImpl::stop() {
    // Stop accepting HTTP first: otherwise a request in flight would reach a
    // runtime that is already draining and get "proxy shutting down" for no
    // reason the caller can act on.
    if (m_http) { m_http->stop(); m_http.reset(); }
    return m_rt->stop();
}

std::string VerifiedProxyImpl::localEndpoint() {
    return m_http ? m_http->endpoint() : std::string();
}

bool VerifiedProxyImpl::ok() { return m_rt->running(); }

LogosMap VerifiedProxyImpl::status() {
    json s = m_rt->statusSnapshot();
    if (!m_configured) s["state"] = "uninitialized";
    else if (s["state"] == "uninitialized") s["state"] = "configured";

    // ProxyRuntime only learns the config at start(), so before the first run
    // its snapshot carries a DEFAULT-constructed one — reporting network
    // "mainnet" and chainId 1 for a module configured for something else.
    // The impl's config is the authority whenever it has one.
    if (m_configured && m_cfg) {
        s["network"] = m_cfg->network;
        s["chainId"] = m_cfg->expectedChainId();
    }
    s["moduleVersion"] = VERIFIED_PROXY_MODULE_VERSION;
    s["libraryVersion"] = VERIFIED_PROXY_NIMBUS_REV;
    s["httpServer"] = json{
        { "running",  m_http && m_http->running() },
        { "endpoint", m_http ? m_http->endpoint() : std::string() },
    };
    return s;
}

std::string VerifiedProxyImpl::moduleVersion()  { return VERIFIED_PROXY_MODULE_VERSION; }
std::string VerifiedProxyImpl::libraryVersion() { return VERIFIED_PROXY_NIMBUS_REV; }

LogosList VerifiedProxyImpl::supportedNetworks() {
    LogosList out = json::array();
    for (const auto& p : networkProfiles()) {
        out.push_back(json{
            { "name",            p.name },
            { "chainId",         p.chainId },
            { "beaconApiUrl",    p.beaconApiUrl },
            { "executionApiUrl", p.executionApiUrl },
        });
    }
    return out;
}

StdLogosResult VerifiedProxyImpl::fetchFinalizedRoot(const std::string& beaconUrl) {
    const std::string base = beacon_client::trim(beaconUrl);
    if (base.empty()) return { false, {}, "beacon URL is required" };
    if (!beacon_client::isHttpUrl(base))
        return { false, {}, "beacon URL must be http(s): " + base };

    const std::string url = beacon_client::finalizedHeaderUrl(base);
    const beacon_client::HttpResponse res = beacon_client::httpGet(url);
    if (!res.error.empty()) return { false, {}, "beacon request failed: " + res.error };
    if (res.status != 200)
        return { false, {}, "beacon returned HTTP " + std::to_string(res.status) };

    std::string slot;
    const std::string root = beacon_client::parseFinalizedRoot(res.body, slot);
    if (root.empty())
        return { false, {}, "beacon response did not contain data.root" };

    LogosMap out;
    out["root"] = root;
    out["slot"] = slot;
    out["source"] = url;
    return { true, out };
}

// ── Verified JSON-RPC ───────────────────────────────────────────────────────
//
// Every one of these is three lines over the same dispatch path: the library's
// `proxyCall` is a string `case` over the very procs its typed C entry points
// call, so there is one FFI path rather than sixty.

StdLogosResult VerifiedProxyImpl::rpc(const std::string& method, const LogosList& params) {
    return m_rt->call(method, params.is_null() ? json::array() : params);
}
