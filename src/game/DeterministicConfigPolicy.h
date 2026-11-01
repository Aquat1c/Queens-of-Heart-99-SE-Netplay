#pragma once

#include <cstdint>

namespace qgame {

constexpr bool netActiveConfigIdentityValid(
    bool captured,
    std::uint32_t activeProfile,
    std::uint32_t capturedProfile,
    std::uint32_t bindingProfile,
    std::uint32_t expectedBindingProfile) {
    return captured
        && activeProfile == capturedProfile
        && bindingProfile == expectedBindingProfile;
}

constexpr bool qohConfigSaveSucceeded(bool originalValuesStaged,
                                      unsigned int nativeResult) {
    return originalValuesStaged && nativeResult == 1u;
}

}
