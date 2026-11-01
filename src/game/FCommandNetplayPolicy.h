#pragma once

#include <cstdint>

namespace qgame {

struct FCommandRiskyState {
    std::uint32_t qohNetworkMode = 0;
    std::uint32_t waitTimeMode = 0;
    std::uint32_t waitPeriod = 0;
    std::uint32_t printDecDamage = 0;
    std::uint32_t printMasterDamage = 0;
    std::uint32_t comVsAsobiVersion = 0;
    std::uint32_t extraFunctionFlags = 0;
    std::uint32_t waitCount = 0;
    std::uint32_t waitCounter = 0;
    std::uint32_t extraSkip = 0;
    std::uint32_t extraSkipCounter = 0;
    std::uint32_t debugWindow = 0;
    // profile+376 flags dword (shared by config.exe and QOHcnf.key): bit 0 = full-colour (16-bit) mode, bit 1 = NoSound, bit 2 = Vsync.
    std::uint32_t displayFlags = 0;
};

constexpr std::uint32_t kFullColorDisplayFlag = 1u;

struct FCommandBootPacing {
    std::uint32_t waitTimeMode = 0;
    std::uint32_t waitPeriod = 0;
};

constexpr void applyNetplayFCommandDefaults(
    FCommandRiskyState& state,
    const FCommandBootPacing& bootPacing) {
    state.qohNetworkMode = 0;
    state.waitTimeMode = bootPacing.waitTimeMode;
    state.waitPeriod = bootPacing.waitPeriod;
    state.printDecDamage = 0;
    state.printMasterDamage = 0;
    state.comVsAsobiVersion = 0;
    state.extraFunctionFlags = 0;
    state.waitCount = 0;
    state.waitCounter = 0;
    state.extraSkip = 0;
    state.extraSkipCounter = 0;
    state.debugWindow = 0;
    state.displayFlags |= kFullColorDisplayFlag;
}

constexpr bool netplayFCommandDefaultsApplied(
    const FCommandRiskyState& state,
    const FCommandBootPacing& bootPacing) {
    return state.qohNetworkMode == 0
        && state.waitTimeMode == bootPacing.waitTimeMode
        && state.waitPeriod == bootPacing.waitPeriod
        && state.printDecDamage == 0
        && state.printMasterDamage == 0
        && state.comVsAsobiVersion == 0
        && state.extraFunctionFlags == 0
        && state.waitCount == 0
        && state.waitCounter == 0
        && state.extraSkip == 0
        && state.extraSkipCounter == 0
        && state.debugWindow == 0
        && (state.displayFlags & kFullColorDisplayFlag) != 0u;
}

}
