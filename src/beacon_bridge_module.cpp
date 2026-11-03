#include "beacon_bridge.h"

#include <nlohmann/json.hpp>
#include <logos_module_context.h>

#include "proxy_config.h"

// Generated at build time; declares modules().beacon_api_module.
#include "logos_sdk.h"

using json = nlohmann::json;

namespace {

constexpr int kConfigCallMs = 5000;

std::string describe(const logos::CallError& e) {
    return e.message.empty() ? e.code : e.code + ": " + e.message;
}

class ModuleBeaconBridge final : public BeaconBridge {
public:
    explicit ModuleBeaconBridge(LogosModuleContext& ctx) : m_ctx(ctx) {}

    std::vector<std::string> urls(int64_t chainId, std::string& err) override {
        auto& beacon = m_ctx.modules().beacon_api_module;
        logos::CallError ce;
        beacon.init_defaults(&ce, kConfigCallMs);
        if (!ce.ok()) {
            err = "beacon_api_module is unreachable (" + describe(ce) + ")";
            return {};
        }
        const std::string raw = beacon.get_beacon_config(chainId, &ce, kConfigCallMs);
        if (!ce.ok()) {
            err = "beacon_api_module is unreachable (" + describe(ce) + ")";
            return {};
        }
        auto urls = beaconUrlsFromModule(json::parse(raw, nullptr, false), err);
        if (urls.empty()) err = "beacon_api_module, chain " + std::to_string(chainId) + ": " + err;
        return urls;
    }

private:
    LogosModuleContext& m_ctx;
};

} // namespace

std::shared_ptr<BeaconBridge> makeModuleBeaconBridge(LogosModuleContext& ctx) {
    return std::make_shared<ModuleBeaconBridge>(ctx);
}
