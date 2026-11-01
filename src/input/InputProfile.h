#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "input/InputActions.h"
#include "input/InputLaunchConfig.h"

namespace qinput {

constexpr uint32_t kControlsSchemaVersion = 2;
constexpr size_t kMaxProfiles = 8;
constexpr size_t kMaxProfileNameLength = 64;
constexpr size_t kMaxProfileIdLength = 64;

struct ProfileBinding {
    CompiledBindingType type = CompiledBindingType::None;
    uint8_t dik = 0;
    ProbeKind probeKind = ProbeKind::None;
    uint8_t token = 0;
    AxisPolarity polarity = AxisPolarity::Positive;
    uint8_t hatBit = 0;
    uint16_t pressQ15 = 16384;
    uint16_t releaseQ15 = 11469;
};

struct ProfileAxisCalibration {
    uint8_t axisToken = 0;
    ProbeKind probeKind = ProbeKind::GamepadAxis;
    CompiledAxisCalibration values{};
};

struct ProfileDevice {
    std::string guid;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t version = 0;
    std::string serial;
    uint64_t serialHash = 0;
    uint64_t pathFingerprint = 0;
    std::string normalizedName;
};

struct InputProfile {
    std::string id;
    std::string name;
    CompiledSourceMode sourceMode = CompiledSourceMode::Keyboard;
    ProfileDevice device{};
    std::vector<ProfileBinding> actions[8];
    std::vector<ProfileAxisCalibration> axisCalibration;
    SocdPolicy horizontalSocd = SocdPolicy::Native;
    SocdPolicy verticalSocd = SocdPolicy::Native;
    FocusPolicy focusPolicy = FocusPolicy::GameWindowOnly;
    std::string mapping;
    std::string probeBackend;
};

struct ModeControlMigrationChoice {
    ModeControlAction action = ModeControlAction::None;
    std::vector<ProfileBinding> playerBindings[4];
};

struct ControlsFile {
    uint32_t schemaVersion = kControlsSchemaVersion;
    std::string activeProfileId;
    std::string activeByPlayer[4];
    std::vector<InputProfile> profiles;
    std::vector<ProfileBinding> modeControls[
        static_cast<size_t>(ModeControlAction::Count)];
    std::vector<ModeControlMigrationChoice> pendingModeMigrations;
};

// QOH native default keyboard profile (sub_484D70 player 1 defaults).
InputProfile makeDefaultKeyboardProfile();
ControlsFile makeDefaultControlsFile();

bool parseControlsFile(const std::string& text, ControlsFile& out,
                       std::string& error);

std::string serializeControlsFile(const ControlsFile& file);

uint64_t profileContentHash(const InputProfile& profile);

}
