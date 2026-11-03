#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class LogosModuleContext;

/// The verified proxy's view of beacon_api_module.
class BeaconBridge {
public:
    virtual ~BeaconBridge() = default;

    /// Enabled provider URLs for `chainId`, after seeding the module's defaults.
    /// Empty, with `err` set, when there are none or the module is unreachable.
    virtual std::vector<std::string> urls(int64_t chainId, std::string& err) = 0;
};

/// Backed by `modules().beacon_api_module`. Only the module build links this;
/// unit tests link a fake, so they need no generated client.
std::shared_ptr<BeaconBridge> makeModuleBeaconBridge(LogosModuleContext& ctx);
