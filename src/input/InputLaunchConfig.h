#pragma once

#include <cstddef>
#include <cstdint>

#include "input/ModeControlContract.h"

namespace qinput {

constexpr uint32_t kInputConfigSchemaVersion = 2;

constexpr uint32_t kMaxControllerProbes = 32;
constexpr uint32_t kMaxBindingsPerAction = 4;
constexpr uint32_t kMaxKeyboardBindings = 96;
constexpr uint32_t kMaxModeControlBindings = 96;
constexpr uint32_t kMaxMappingStringLength = 1024;
constexpr uint16_t kAxisScale = 32767;

enum class CompiledSourceMode : uint8_t {
    Keyboard = 0,
    Controller = 1,
    KeyboardAndController = 2,
};

enum class SocdPolicy : uint8_t {
    Native = 0,
    Neutral = 1,
    LastInput = 2,
    UpPriority = 3,
};

enum class FocusPolicy : uint8_t {
    GameWindowOnly = 0,
    AllowBackground = 1,
};

enum class ProbeKind : uint8_t {
    None = 0,
    GamepadButton = 1,
    GamepadAxis = 2,
    RawButton = 3,
    RawAxis = 4,
    RawHat = 5,
};

enum class AxisPolarity : uint8_t {
    Positive = 0,
    Negative = 1,
};

constexpr bool gamepadAxisTokenIsTrigger(uint8_t token) {
    return token == 4 || token == 5;
}

constexpr bool probeAxisIsAnalogTrigger(ProbeKind kind, uint8_t token) {
    return kind == ProbeKind::GamepadAxis && gamepadAxisTokenIsTrigger(token);
}

constexpr uint8_t kHatUp = 0x01;
constexpr uint8_t kHatRight = 0x02;
constexpr uint8_t kHatDown = 0x04;
constexpr uint8_t kHatLeft = 0x08;

struct CompiledProbe {
    ProbeKind kind = ProbeKind::None;
    uint8_t token = 0;
    uint8_t reservedA = 0;
    uint8_t reservedB = 0;
};

constexpr bool compiledProbeNeedsStandardizedBackend(ProbeKind kind) {
    return kind == ProbeKind::GamepadButton || kind == ProbeKind::GamepadAxis;
}

enum class CompiledProbeBackend : uint32_t {
    Unknown = 0,
    Sdl = 1,
    DirectInput8 = 2,
};

inline bool compiledProbesRequireStandardizedBackend(
        const CompiledProbe* probes, uint32_t count) {
    if (probes == nullptr) return false;
    for (uint32_t i = 0; i < count; ++i) {
        if (compiledProbeNeedsStandardizedBackend(probes[i].kind)) return true;
    }
    return false;
}

inline bool compiledControllerNeedsSdl(const CompiledProbe* probes,
                                       uint32_t count,
                                       CompiledProbeBackend captured) {
    if (count == 0) return false;
    return compiledProbesRequireStandardizedBackend(probes, count)
        || captured != CompiledProbeBackend::DirectInput8;
}

struct CompiledAxisCalibration {
    int32_t minimum = -32768;
    int32_t center = 0;
    int32_t maximum = 32767;
    uint16_t deadzoneQ15 = 0;
    uint8_t invert = 0;
    uint8_t unipolar = 0;
};

enum class CompiledBindingType : uint8_t {
    None = 0,
    KeyboardScan = 1,
    ProbeButton = 2,
    ProbeAxis = 3,
    ProbeHat = 4,
};

struct CompiledBinding {
    CompiledBindingType type = CompiledBindingType::None;
    uint8_t dik = 0;
    uint8_t probeIndex = 0;
    AxisPolarity polarity = AxisPolarity::Positive;
    uint8_t hatBit = 0;
    uint8_t reserved = 0;
    uint16_t pressQ15 = 16384;
    uint16_t releaseQ15 = 11469;
    CompiledAxisCalibration calibration{};
};

struct CompiledDeviceMatcher {
    uint8_t hasSerial = 0;
    uint8_t hasPathFingerprint = 0;
    uint8_t reservedA = 0;
    uint8_t reservedB = 0;
    char guid[64] = {};
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t version = 0;
    uint16_t reservedC = 0;
    uint64_t serialHash = 0;
    uint64_t pathFingerprint = 0;
    char normalizedName[128] = {};
};

struct CompiledActionBindings {
    uint8_t count = 0;
    CompiledBinding bindings[kMaxBindingsPerAction];
};

struct CompiledModeControlBindingV1 {
    ModeControlAction action = ModeControlAction::None;
    uint16_t reserved = 0;
    CompiledBinding binding{};
};

struct CompiledModeControlsV1 {
    uint32_t structSize = sizeof(CompiledModeControlsV1);
    uint16_t version = 1;
    uint16_t count = 0;
    CompiledModeControlBindingV1 bindings[kMaxModeControlBindings];
};

struct InputLaunchConfig {
    uint32_t schemaVersion = kInputConfigSchemaVersion;
    CompiledSourceMode sourceMode = CompiledSourceMode::Keyboard;
    SocdPolicy horizontalSocd = SocdPolicy::Native;
    SocdPolicy verticalSocd = SocdPolicy::Native;
    FocusPolicy focusPolicy = FocusPolicy::GameWindowOnly;

    CompiledActionBindings actions[8];

    uint32_t probeCount = 0;
    CompiledProbe probes[kMaxControllerProbes];

    CompiledDeviceMatcher device{};

    uint32_t mappingLength = 0;
    char mapping[kMaxMappingStringLength] = {};

    CompiledProbeBackend probeBackend = CompiledProbeBackend::Unknown;

    uint64_t profileHash = 0;
    uint32_t reservedTail[3] = {};
};

struct InputConfigEnvelope {
    uint32_t magic = 0x51494E31;        // "QIN1"
    uint32_t schemaVersion = kInputConfigSchemaVersion;
    uint32_t byteSize = 0;
    uint32_t reserved = 0;
    uint64_t canonicalHash = 0;
};

inline uint64_t hashInputConfigBytes(const void* data, size_t length) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

const char* validateInputLaunchConfig(const InputLaunchConfig& config,
                                      bool spectator);

const char* validateCompiledModeControls(
    const CompiledModeControlsV1& controls,
    const InputLaunchConfig& input,
    bool spectator);

}
