// Numeric values are append-only.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qinput {

enum class ModeControlAction : uint16_t {
    None = 0,

    TogglePresentationMode = 1,
    ToggleOverlayMenu = 2,
    ToggleInputViewer = 3,
    // Value 4 is a free slot: modeControlBit() is 1ull << action, so action 64 would shift off the end of the mask word.
    OpenOverlayMenuPad = 4,
    ReplayRestart = 5,
    ReplayStepBack = 6,

    LeaveMatch = 10,
    InputDelayDecrease = 11,
    InputDelayIncrease = 12,
    RollbackWindowDecrease = 13,
    RollbackWindowIncrease = 14,

    ReplayPause = 20,
    ReplayStep = 21,
    ReplaySlower = 22,
    ReplayFaster = 23,

    TrainingResetPosition = 40,
    TrainingSaveState = 41,
    TrainingLoadState = 42,
    TrainingRecordToggle = 43,
    TrainingPlaybackToggle = 44,
    TrainingPreviousSlot = 45,
    TrainingNextSlot = 46,
    TrainingPause = 47,
    TrainingFrameStep = 48,
    TrainingReturnCharacterSelect = 49,
    TrainingCycleStance = 50,
    TrainingCycleGuard = 51,
    TrainingRefill = 52,
    TrainingChargeExFraction = 53,
    TrainingResetRoundStart = 54,
    TrainingToggleHitboxes = 55,

    ReplayTakeoverExit = 56,
    ReplayTakeoverP1 = 57,
    ReplayTakeoverP2 = 58,
    ReplayTakeoverRetry = 59,
    ReplayTakeoverDelayDecrease = 60,
    ReplayTakeoverDelayIncrease = 61,
    ReplayToggleCollisionDisplay = 62,
    OnlineReturnCharacterSelect = 63,

    Count = 64,
};

enum class ModeControlDomain : uint8_t {
    WrapperGlobal = 0,
    OnlinePlayer = 1,
    Training = 2,
    Replay = 3,
};

enum class ModeControlTrigger : uint8_t {
    Edge = 0,
    HeldLevel = 1,
};

enum class ModeControlDevicePolicy : uint8_t {
    KeyboardOnly = 0,
    KeyboardOrSelectedControllerDigital = 1,
};

struct ModeControlDescriptor {
    ModeControlAction action;
    ModeControlDomain domain;
    ModeControlTrigger trigger;
    ModeControlDevicePolicy devicePolicy;
    const char* stableName;
    const char* displayName;
};

constexpr ModeControlDescriptor kModeControlDescriptors[] = {
    {ModeControlAction::TogglePresentationMode, ModeControlDomain::WrapperGlobal,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "togglePresentationMode", "Toggle presentation mode"},
    {ModeControlAction::ToggleOverlayMenu, ModeControlDomain::WrapperGlobal,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "toggleOverlayMenu", "Open / close overlay menu"},
    {ModeControlAction::ToggleInputViewer, ModeControlDomain::WrapperGlobal,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "toggleInputViewer", "Toggle input viewer"},
    {ModeControlAction::OpenOverlayMenuPad, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "openOverlayMenuPad", "Open / close overlay menu (controller)"},

    {ModeControlAction::LeaveMatch, ModeControlDomain::OnlinePlayer,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "leaveMatch", "Leave match"},
    {ModeControlAction::InputDelayDecrease, ModeControlDomain::OnlinePlayer,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "inputDelayDecrease", "Decrease input delay"},
    {ModeControlAction::InputDelayIncrease, ModeControlDomain::OnlinePlayer,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "inputDelayIncrease", "Increase input delay"},
    {ModeControlAction::RollbackWindowDecrease, ModeControlDomain::OnlinePlayer,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "rollbackWindowDecrease", "Decrease rollback window"},
    {ModeControlAction::RollbackWindowIncrease, ModeControlDomain::OnlinePlayer,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "rollbackWindowIncrease", "Increase rollback window"},
    {ModeControlAction::OnlineReturnCharacterSelect,
     ModeControlDomain::OnlinePlayer, ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "onlineReturnCharacterSelect",
     "Return to character select (online or offline VS)"},

    {ModeControlAction::ReplayPause, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayPause", "Pause / resume replay"},
    {ModeControlAction::ReplayRestart, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayRestart", "Restart replay"},
    {ModeControlAction::ReplayStepBack, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayStepBack", "Previous replay frame"},
    {ModeControlAction::ReplayStep, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayStep", "Single-step replay"},
    {ModeControlAction::ReplaySlower, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replaySlower", "Slower replay"},
    {ModeControlAction::ReplayFaster, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayFaster", "Faster replay"},

    {ModeControlAction::TrainingResetPosition, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingResetPosition", "Reset position"},
    {ModeControlAction::TrainingSaveState, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingSaveState", "Save state"},
    {ModeControlAction::TrainingLoadState, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingLoadState", "Load state"},
    {ModeControlAction::TrainingRecordToggle, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingRecordToggle", "Record / stop"},
    {ModeControlAction::TrainingPlaybackToggle, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingPlaybackToggle", "Play / stop"},
    {ModeControlAction::TrainingPreviousSlot, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingPreviousSlot", "Previous slot"},
    {ModeControlAction::TrainingNextSlot, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingNextSlot", "Next slot"},
    {ModeControlAction::TrainingPause, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingPause", "Pause"},
    {ModeControlAction::TrainingFrameStep, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingFrameStep", "Frame step"},
    {ModeControlAction::TrainingReturnCharacterSelect, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingReturnCharacterSelect", "Return to character select"},
    {ModeControlAction::TrainingCycleStance, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingCycleStance", "Cycle dummy stance"},
    {ModeControlAction::TrainingCycleGuard, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingCycleGuard", "Cycle guard mode"},
    {ModeControlAction::TrainingRefill, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingRefill", "Refill resources"},
    {ModeControlAction::TrainingChargeExFraction, ModeControlDomain::Training,
     ModeControlTrigger::HeldLevel,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingChargeExFraction", "Fractional EX charge"},
    {ModeControlAction::TrainingResetRoundStart, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingResetRoundStart", "Reset to round start"},
    {ModeControlAction::TrainingToggleHitboxes, ModeControlDomain::Training,
     ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital,
     "trainingToggleHitboxes", "Toggle native hitboxes"},

    {ModeControlAction::ReplayTakeoverExit, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayTakeoverExit", "Exit replay takeover"},
    {ModeControlAction::ReplayTakeoverP1, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayTakeoverP1", "Take over Player 1"},
    {ModeControlAction::ReplayTakeoverP2, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayTakeoverP2", "Take over Player 2"},
    {ModeControlAction::ReplayTakeoverRetry, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayTakeoverRetry", "Retry replay takeover"},
    {ModeControlAction::ReplayTakeoverDelayDecrease, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayTakeoverDelayDecrease", "Decrease takeover delay"},
    {ModeControlAction::ReplayTakeoverDelayIncrease, ModeControlDomain::Replay,
     ModeControlTrigger::Edge, ModeControlDevicePolicy::KeyboardOnly,
     "replayTakeoverDelayIncrease", "Increase takeover delay"},
    {ModeControlAction::ReplayToggleCollisionDisplay,
     ModeControlDomain::Replay, ModeControlTrigger::Edge,
     ModeControlDevicePolicy::KeyboardOnly,
     "replayToggleCollisionDisplay", "Toggle collision-box display"},
};

constexpr size_t kModeControlDescriptorCount =
    sizeof(kModeControlDescriptors) / sizeof(kModeControlDescriptors[0]);

constexpr const ModeControlDescriptor* modeControlDescriptor(
        ModeControlAction action) {
    for (size_t i = 0; i < kModeControlDescriptorCount; ++i) {
        if (kModeControlDescriptors[i].action == action)
            return &kModeControlDescriptors[i];
    }
    return nullptr;
}

constexpr bool isCanonicalModeControlAction(ModeControlAction action) {
    return action != ModeControlAction::None
        && modeControlDescriptor(action) != nullptr;
}

constexpr bool modeDomainsOverlap(ModeControlDomain a, ModeControlDomain b) {
    if (a == b) return true;
    if (a == ModeControlDomain::WrapperGlobal
        || b == ModeControlDomain::WrapperGlobal) return true;
    if ((a == ModeControlDomain::OnlinePlayer && b == ModeControlDomain::Replay)
        || (b == ModeControlDomain::OnlinePlayer && a == ModeControlDomain::Replay))
        return false;
    if ((a == ModeControlDomain::Training && b == ModeControlDomain::Replay)
        || (b == ModeControlDomain::Training && a == ModeControlDomain::Replay))
        return false;
    return a == b;
}

constexpr bool modeDomainOverlapsFighter(ModeControlDomain domain) {
    return domain != ModeControlDomain::Replay;
}

constexpr bool trainingDikForbidden(uint8_t dik) {
    return dik == 0x44u || dik == 0x57u || dik == 0x58u || dik == 0xD3u;
}

static_assert(static_cast<uint16_t>(ModeControlAction::Count) == 64,
              "mode-control values are append-only");
// New controls use free slots: the mask cannot extend past 64 actions.
static_assert(kModeControlDescriptorCount == 39,
              "every released mode-control action needs one descriptor");

}
