// Unit tests never reach beacon_api_module; this stands in for the
// generated-client bridge. The answer parsing has its own tests.
#include "beacon_bridge.h"

namespace {

class UnreachableBeaconBridge final : public BeaconBridge {
public:
    std::vector<std::string> urls(int64_t, std::string& err) override {
        err = "beacon_api_module is unreachable (unit test)";
        return {};
    }
};

} // namespace

std::shared_ptr<BeaconBridge> makeModuleBeaconBridge(LogosModuleContext&) {
    return std::make_shared<UnreachableBeaconBridge>();
}
