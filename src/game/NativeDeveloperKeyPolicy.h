#pragma once

#include <cstdint>

namespace qgame {

enum class NativeDeveloperKeyPolicy : uint8_t {
    RespectUserFCommand = 0,
    ForceDisabled = 1,
};

enum class NativeDeveloperRuntimeMode : uint8_t {
    OfflineInteractive = 0,
    SelfTestOrProbe = 1,
};

constexpr NativeDeveloperKeyPolicy nativeDeveloperKeyPolicy(
        NativeDeveloperRuntimeMode mode,
        bool launchAllowsInteractiveOfflineKeys,
        bool trainingEnabled,
        bool replayOwned,
        bool spectatorOwned,
        bool netplayOwned) {
    return mode == NativeDeveloperRuntimeMode::OfflineInteractive
            && launchAllowsInteractiveOfflineKeys
            && !trainingEnabled
            && !replayOwned
            && !spectatorOwned
            && !netplayOwned
        ? NativeDeveloperKeyPolicy::RespectUserFCommand
        : NativeDeveloperKeyPolicy::ForceDisabled;
}

}
