#include "proxy_config.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace {

// Upstream's `getMetadataForNetwork` only has mainnet, hoodi and sepolia
// compiled in; anything else falls through to `fatal` + `quit 1`.
const std::set<std::string>& kNetworks() {
    static const std::set<std::string> v{ "mainnet", "sepolia", "hoodi" };
    return v;
}

// Nim's `updateLogLevel` raises ValueError on anything else, and setupLogging
// turns that into `quit 1`.
const std::set<std::string>& kLogLevels() {
    static const std::set<std::string> v{
        "TRACE", "DEBUG", "INFO", "NOTICE", "WARN", "ERROR", "FATAL", "NONE" };
    return v;
}

const std::set<std::string>& kLogFormats() {
    static const std::set<std::string> v{ "Colors", "NoColors", "Json", "Auto", "None" };
    return v;
}

const std::set<std::string>& kKeepAliveModes() {
    static const std::set<std::string> v{ "off", "interval", "continuous" };
    return v;
}

std::string join(const std::set<std::string>& s) {
    std::string out;
    for (const auto& v : s) { if (!out.empty()) out += ", "; out += v; }
    return out;
}

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// Upstream's `parseCmdArg(UrlList, ...)` rejects any scheme outside this set.
bool schemeOk(const std::string& url) {
    static const char* kSchemes[] = { "http://", "https://", "ws://", "wss://" };
    for (const char* s : kSchemes)
        if (url.rfind(s, 0) == 0) return true;
    return false;
}

bool readStringList(const json& in, const char* key,
                    std::vector<std::string>& out, std::string& err) {
    if (!in.contains(key) || in[key].is_null()) return true;
    const json& v = in[key];
    // Accept a bare string too — upstream's own format is comma-separated, so
    // a caller pasting that shape should not be punished for it.
    if (v.is_string()) {
        std::stringstream ss(v.get<std::string>());
        std::string item;
        while (std::getline(ss, item, ',')) if (!item.empty()) out.push_back(item);
        return true;
    }
    if (!v.is_array()) {
        err = std::string("'") + key + "' must be an array of URL strings";
        return false;
    }
    for (const auto& e : v) {
        if (!e.is_string()) {
            err = std::string("'") + key + "' must contain only strings";
            return false;
        }
        out.push_back(e.get<std::string>());
    }
    return true;
}

bool validateUrls(const std::vector<std::string>& urls, const char* key,
                  bool required, std::string& err) {
    if (required && urls.empty()) {
        err = std::string("'") + key + "' is required and must contain at least one URL";
        return false;
    }
    for (const auto& u : urls) {
        if (!schemeOk(u)) {
            err = std::string("'") + key + "' entry '" + u
                + "' must use one of the http, https, ws or wss schemes";
            return false;
        }
        // A comma inside a single entry would silently split into two URLs when
        // we join for upstream, so reject it where the caller can still see it.
        if (u.find(',') != std::string::npos) {
            err = std::string("'") + key + "' entry '" + u
                + "' must not contain a comma (the upstream format is comma-separated)";
            return false;
        }
    }
    return true;
}

bool readInt(const json& in, const char* key, int64_t& out, std::string& err) {
    if (!in.contains(key) || in[key].is_null()) return true;
    if (!in[key].is_number_integer()) {
        err = std::string("'") + key + "' must be an integer";
        return false;
    }
    out = in[key].get<int64_t>();
    return true;
}

bool readBool(const json& in, const char* key, bool& out, std::string& err) {
    if (!in.contains(key) || in[key].is_null()) return true;
    if (!in[key].is_boolean()) {
        err = std::string("'") + key + "' must be a boolean";
        return false;
    }
    out = in[key].get<bool>();
    return true;
}

bool readEnum(const json& in, const char* key, const std::set<std::string>& allowed,
              std::string& out, std::string& err) {
    if (!in.contains(key) || in[key].is_null()) return true;
    if (!in[key].is_string()) {
        err = std::string("'") + key + "' must be a string";
        return false;
    }
    const std::string v = in[key].get<std::string>();
    if (!allowed.count(v)) {
        err = std::string("'") + key + "' must be one of: " + join(allowed)
            + " (got '" + v + "')";
        return false;
    }
    out = v;
    return true;
}

std::string joinCsv(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) { if (!out.empty()) out += ","; out += s; }
    return out;
}

/// Strip userinfo and query, and keep only the first path segment. Provider
/// URLs commonly carry the API key as the last path segment or in the query.
std::string redactUrl(const std::string& url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return "<redacted>";
    const std::string scheme = url.substr(0, schemeEnd + 3);
    std::string rest = url.substr(schemeEnd + 3);

    if (const auto at = rest.find('@'); at != std::string::npos)
        rest = rest.substr(at + 1);              // drop user:password@
    if (const auto q = rest.find('?'); q != std::string::npos)
        rest = rest.substr(0, q) + "?<redacted>";

    const auto slash = rest.find('/');
    if (slash == std::string::npos) return scheme + rest;
    return scheme + rest.substr(0, slash) + "/<redacted>";
}

json urlsRedacted(const std::vector<std::string>& v) {
    json out = json::array();
    for (const auto& u : v) out.push_back(redactUrl(u));
    return out;
}

} // namespace

bool ProxyConfig::fromJson(const json& in, ProxyConfig& out, std::string& err) {
    err.clear();
    out = ProxyConfig{};

    if (!in.is_object()) { err = "config must be a JSON object"; return false; }

    // --- the two fields that can kill the host -------------------------------
    if (!readEnum(in, "network", kNetworks(), out.network, err)) return false;
    if (!readEnum(in, "logLevel", kLogLevels(), out.logLevel, err)) return false;
    if (!readEnum(in, "logFormat", kLogFormats(), out.logFormat, err)) return false;

    // --- trustedBlockRoot ----------------------------------------------------
    if (!in.contains("trustedBlockRoot") || !in["trustedBlockRoot"].is_string()) {
        err = "'trustedBlockRoot' is required and must be a 0x-prefixed 32-byte hex string";
        return false;
    }
    out.trustedBlockRoot = in["trustedBlockRoot"].get<std::string>();
    if (out.trustedBlockRoot.rfind("0x", 0) != 0 || out.trustedBlockRoot.size() != 66
        || !std::all_of(out.trustedBlockRoot.begin() + 2, out.trustedBlockRoot.end(), isHex)) {
        err = "'trustedBlockRoot' must be 0x followed by exactly 64 hex digits (got '"
            + out.trustedBlockRoot + "')";
        return false;
    }

    // --- backends ------------------------------------------------------------
    if (!readStringList(in, "executionApiUrls",   out.executionApiUrls,   err)) return false;
    if (!readStringList(in, "beaconApiUrls",      out.beaconApiUrls,      err)) return false;
    if (!readStringList(in, "opExecutionApiUrls", out.opExecutionApiUrls, err)) return false;
    if (!readStringList(in, "privateTxUrls",      out.privateTxUrls,      err)) return false;
    if (!readStringList(in, "archiveUrls",        out.archiveUrls,        err)) return false;

    if (!validateUrls(out.executionApiUrls,   "executionApiUrls",   true,  err)) return false;
    if (!validateUrls(out.beaconApiUrls,      "beaconApiUrls",      true,  err)) return false;
    if (!validateUrls(out.opExecutionApiUrls, "opExecutionApiUrls", false, err)) return false;
    if (!validateUrls(out.privateTxUrls,      "privateTxUrls",      false, err)) return false;
    if (!validateUrls(out.archiveUrls,        "archiveUrls",        false, err)) return false;

    // --- upstream tuning -----------------------------------------------------
    const json tuning = in.value("tuning", json::object());
    if (!tuning.is_object()) { err = "'tuning' must be an object"; return false; }
    if (!readInt(tuning, "maxBlockWalk",           out.maxBlockWalk,           err)) return false;
    if (!readInt(tuning, "maxWindowJumps",         out.maxWindowJumps,         err)) return false;
    if (!readInt(tuning, "parallelBlockDownloads", out.parallelBlockDownloads, err)) return false;
    if (!readInt(tuning, "maxLightClientUpdates",  out.maxLightClientUpdates,  err)) return false;
    if (!readInt(tuning, "headerStoreLen",         out.headerStoreLen,         err)) return false;
    if (!readInt(tuning, "storageCacheLen",        out.storageCacheLen,        err)) return false;
    if (!readInt(tuning, "codeCacheLen",           out.codeCacheLen,           err)) return false;
    if (!readInt(tuning, "accountCacheLen",        out.accountCacheLen,        err)) return false;
    if (!readInt(tuning, "freezeAtSlot",           out.freezeAtSlot,           err)) return false;
    if (!readBool(tuning, "syncHeaderStore",       out.syncHeaderStore,        err)) return false;

    // --- module knobs --------------------------------------------------------
    if (!readInt(in, "callTimeoutMs",       out.callTimeoutMs,       err)) return false;
    if (!readInt(in, "startTimeoutMs",      out.startTimeoutMs,      err)) return false;
    if (!readInt(in, "drainTimeoutMs",      out.drainTimeoutMs,      err)) return false;
    if (!readInt(in, "pumpIntervalMs",      out.pumpIntervalMs,      err)) return false;
    if (!readInt(in, "maxInFlight",         out.maxInFlight,         err)) return false;
    if (!readInt(in, "keepAliveIntervalMs", out.keepAliveIntervalMs, err)) return false;
    if (!readBool(in, "autoStart",          out.autoStart,           err)) return false;
    if (!readEnum(in, "keepAlive", kKeepAliveModes(), out.keepAlive, err)) return false;

    if (out.callTimeoutMs <= 0)  { err = "'callTimeoutMs' must be positive";  return false; }
    if (out.startTimeoutMs <= 0) { err = "'startTimeoutMs' must be positive"; return false; }
    if (out.maxInFlight <= 0)    { err = "'maxInFlight' must be positive";    return false; }
    if (out.pumpIntervalMs <= 0) { err = "'pumpIntervalMs' must be positive"; return false; }

    return true;
}

std::string ProxyConfig::toUpstreamJson() const {
    json j;
    j["eth2Network"]      = network;
    j["trustedBlockRoot"] = trustedBlockRoot;
    // Comma-separated STRINGS, not arrays — this is upstream's UrlList format.
    j["executionApiUrls"] = joinCsv(executionApiUrls);
    j["beaconApiUrls"]    = joinCsv(beaconApiUrls);
    if (!opExecutionApiUrls.empty()) j["opExecutionApiUrls"] = joinCsv(opExecutionApiUrls);
    if (!privateTxUrls.empty())      j["privateTxUrls"]      = joinCsv(privateTxUrls);
    if (!archiveUrls.empty())        j["archiveUrls"]        = joinCsv(archiveUrls);

    j["logLevel"]  = logLevel;
    j["logFormat"] = logFormat;

    j["maxBlockWalk"]           = maxBlockWalk;
    j["maxWindowJumps"]         = maxWindowJumps;
    j["parallelBlockDownloads"] = parallelBlockDownloads;
    j["maxLightClientUpdates"]  = maxLightClientUpdates;
    j["headerStoreLen"]         = headerStoreLen;
    j["storageCacheLen"]        = storageCacheLen;
    j["codeCacheLen"]           = codeCacheLen;
    j["accountCacheLen"]        = accountCacheLen;
    j["syncHeaderStore"]        = syncHeaderStore;
    j["freezeAtSlot"]           = freezeAtSlot;
    return j.dump();
}

json ProxyConfig::redacted() const {
    json j;
    j["network"]          = network;
    j["trustedBlockRoot"] = trustedBlockRoot;
    j["chainId"]          = expectedChainId();
    j["executionApiUrls"]   = urlsRedacted(executionApiUrls);
    j["beaconApiUrls"]      = urlsRedacted(beaconApiUrls);
    j["opExecutionApiUrls"] = urlsRedacted(opExecutionApiUrls);
    j["privateTxUrls"]      = urlsRedacted(privateTxUrls);
    j["archiveUrls"]        = urlsRedacted(archiveUrls);
    j["logLevel"]  = logLevel;
    j["logFormat"] = logFormat;
    j["tuning"] = {
        { "maxBlockWalk", maxBlockWalk },
        { "maxWindowJumps", maxWindowJumps },
        { "parallelBlockDownloads", parallelBlockDownloads },
        { "maxLightClientUpdates", maxLightClientUpdates },
        { "headerStoreLen", headerStoreLen },
        { "storageCacheLen", storageCacheLen },
        { "codeCacheLen", codeCacheLen },
        { "accountCacheLen", accountCacheLen },
        { "syncHeaderStore", syncHeaderStore },
        { "freezeAtSlot", freezeAtSlot },
    };
    j["callTimeoutMs"]       = callTimeoutMs;
    j["startTimeoutMs"]      = startTimeoutMs;
    j["drainTimeoutMs"]      = drainTimeoutMs;
    j["pumpIntervalMs"]      = pumpIntervalMs;
    j["maxInFlight"]         = maxInFlight;
    j["keepAlive"]           = keepAlive;
    j["keepAliveIntervalMs"] = keepAliveIntervalMs;
    j["autoStart"]           = autoStart;
    return j;
}

int64_t ProxyConfig::expectedChainId() const {
    if (network == "mainnet") return 1;
    if (network == "sepolia") return 11155111;
    if (network == "hoodi")   return 560048;
    return 0;
}
