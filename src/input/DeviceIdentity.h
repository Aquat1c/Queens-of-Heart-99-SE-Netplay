#pragma once

#include <cstdint>

#include "input/InputLaunchConfig.h"

namespace qinput {

constexpr uint32_t kMaxIdentityCandidates = 16;

struct DeviceCandidate {
    uint32_t liveHandle = 0;
    bool hasSerial = false;
    uint64_t serialHash = 0;
    bool hasPathFingerprint = false;
    uint64_t pathFingerprint = 0;
    char guid[64] = {};
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t version = 0;
    char normalizedName[128] = {};

    bool isGamepad = false;
    bool probeOpened = false;
    uint16_t axisCount = 0;
    uint16_t buttonCount = 0;
    uint16_t hatCount = 0;
    char probeError[96] = {};
};

inline bool candidateHasUsableControls(const DeviceCandidate& candidate) {
    return candidate.axisCount != 0 || candidate.buttonCount != 0
        || candidate.hatCount != 0;
}

enum class IdentityOutcome : uint8_t {
    NoCandidate = 0,
    Unique = 1,
    Ambiguous = 2,
};

enum class IdentityMatchTier : uint8_t {
    None = 0,
    Serial = 1,
    PathFingerprint = 2,
    Class = 3,
    VendorProduct = 4,
};

struct IdentityResolution {
    IdentityOutcome outcome = IdentityOutcome::NoCandidate;
    uint32_t candidateIndex = 0;
    IdentityMatchTier tier = IdentityMatchTier::None;
};

IdentityResolution resolveDeviceIdentity(
    const CompiledDeviceMatcher& matcher,
    const DeviceCandidate* candidates,
    uint32_t candidateCount);

uint64_t identityStringHash(const char* text);

constexpr uint16_t kVendorValve = 0x28DEu;
constexpr uint16_t kProductSteamVirtualGamepad = 0x11FFu;
constexpr uint16_t kProductSteamController = 0x1102u;

constexpr bool isSteamVirtualController(uint16_t vendor, uint16_t product) {
    return vendor == kVendorValve
        && (product == kProductSteamVirtualGamepad
            || product == kProductSteamController);
}

constexpr bool isValveInputDevice(uint16_t vendor) {
    return vendor == kVendorValve;
}

constexpr uint16_t kVendorWiseGroup = 0x0925u;
constexpr uint16_t kProductPsSsDcJoypadBox = 0x8868u;
constexpr uint16_t kProductPsSsN64Joypad = 0x1700u;
constexpr uint16_t kProductMp8866DualJoypad = 0x8866u;
constexpr uint16_t kProductMp8800QuadJoypad = 0x8800u;

constexpr bool isMultiConsoleAdapterBox(uint16_t vendor) {
    return vendor == kVendorWiseGroup;
}

constexpr bool isMultiPortAdapterBox(uint16_t vendor, uint16_t product) {
    return vendor == kVendorWiseGroup
        && (product == kProductMp8866DualJoypad
            || product == kProductMp8800QuadJoypad);
}

}
