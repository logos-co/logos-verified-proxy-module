#include "verified_proxy_impl.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "proxy_config.h"
#include "proxy_runtime.h"

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
        // Persist the ORIGINAL, not the redacted view — this file is the
        // module's private state under a host-owned directory, and a redacted
        // copy would be useless on restart.
        if (f) f << config.dump(2);
    }
    return { true, {}, "" };
}

LogosMap VerifiedProxyImpl::getConfig() {
    if (!m_configured || !m_cfg) return json::object();
    return m_cfg->redacted();
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

StdLogosResult VerifiedProxyImpl::start() {
    if (!m_configured)
        return { false, {}, "not configured — call configure() first" };
    return m_rt->start(*m_cfg);
}

StdLogosResult VerifiedProxyImpl::stop() { return m_rt->stop(); }

bool VerifiedProxyImpl::ok() { return m_rt->running(); }

LogosMap VerifiedProxyImpl::status() {
    json s = m_rt->statusSnapshot();
    if (!m_configured) s["state"] = "uninitialized";
    else if (s["state"] == "uninitialized") s["state"] = "configured";
    s["moduleVersion"] = VERIFIED_PROXY_MODULE_VERSION;
    s["libraryVersion"] = VERIFIED_PROXY_NIMBUS_REV;
    return s;
}

std::string VerifiedProxyImpl::moduleVersion()  { return VERIFIED_PROXY_MODULE_VERSION; }
std::string VerifiedProxyImpl::libraryVersion() { return VERIFIED_PROXY_NIMBUS_REV; }

// ── Verified JSON-RPC ───────────────────────────────────────────────────────
//
// Every one of these is three lines over the same dispatch path: the library's
// `proxyCall` is a string `case` over the very procs its typed C entry points
// call, so there is one FFI path rather than sixty.

StdLogosResult VerifiedProxyImpl::rpc(const std::string& method, const LogosList& params) {
    return m_rt->call(method, params.is_null() ? json::array() : params);
}

StdLogosResult VerifiedProxyImpl::ethBlockNumber() {
    return m_rt->call("eth_blockNumber", json::array());
}

StdLogosResult VerifiedProxyImpl::ethChainId() {
    return m_rt->call("eth_chainId", json::array());
}

StdLogosResult VerifiedProxyImpl::ethGetBalance(const std::string& address,
                                                const std::string& blockTag) {
    return m_rt->call("eth_getBalance", json::array({ address, blockTag }));
}

StdLogosResult VerifiedProxyImpl::ethGetCode(const std::string& address,
                                             const std::string& blockTag) {
    return m_rt->call("eth_getCode", json::array({ address, blockTag }));
}

StdLogosResult VerifiedProxyImpl::ethGetBlockByNumber(const std::string& blockTag,
                                                      bool fullTransactions) {
    return m_rt->call("eth_getBlockByNumber", json::array({ blockTag, fullTransactions }));
}

StdLogosResult VerifiedProxyImpl::ethCall(const LogosMap& txArgs,
                                          const std::string& blockTag,
                                          bool optimisticStateFetch) {
    // The third positional parameter is upstream's own extension; a standard
    // JSON-RPC eth_call has only two.
    return m_rt->call("eth_call", json::array({ txArgs, blockTag, optimisticStateFetch }));
}

StdLogosResult VerifiedProxyImpl::ethGetTransactionByBlockNumberAndIndex(
    const std::string& blockTag, uint64_t index) {
    return m_rt->call("eth_getTransactionByBlockNumberAndIndex",
                      json::array({ blockTag, index }));
}
