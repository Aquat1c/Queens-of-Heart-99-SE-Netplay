#pragma once

#include <cstdint>

namespace qgame {

namespace directboot {

inline bool isNetworkTwoHumanCharacterSelectHandoff(
    bool networkDriven, int vsRow) {
    return networkDriven && vsRow == 1;
}

inline bool isTwoFighterDirectBootRow(int vsRow) {
    return vsRow == 0 || vsRow == 1;
}

// dword_6236CC after the launch commit sub_434710, which alone installs the row's CPU slots: row 0 (1P vs COM) reads 0 during character select and 2 once committed.
inline uint32_t vsRowCpuSlotMaskAtMatchEntry(uint32_t vsRow) {
    switch (vsRow) {
        case 0u: return 2u;
        case 1u: return 0u;
        case 2u: return 14u;
        case 3u: return 10u;
        case 4u: return 12u;
        case 5u: return 0u;
        default: return 0xFFFFFFFFu;
    }
}

enum class TwoFighterCharacterSelectStatus : uint8_t {
    NotAtTwoFighterCharacterSelect = 0,
    NativeFormationViolation,
    AwaitingHumanConfirmations,
    NativeLaunchEligible,
};

enum class TwoFighterCharacterSelectHandoffAction : uint8_t {
    PreserveNativeSelector = 0,
    FailClosed,
};

struct TwoFighterCharacterSelectObservation {
    uint32_t vsRow = 0;
    uint32_t scene = 0;
    uint32_t sceneSubstate = 0;
    uint32_t matchFormationRow = 0;
    uint32_t vsFormationApplied = 0;
    uint32_t cpuSlotMask = 0;
    uint32_t p1ConfirmationState = 0;
    uint32_t p2ConfirmationState = 0;
    uint32_t nativeConfirmationLatch = 0;
};

inline TwoFighterCharacterSelectObservation
expectedTwoFighterCharacterSelect(uint32_t vsRow) {
    TwoFighterCharacterSelectObservation expected{};
    expected.vsRow = vsRow;
    expected.scene = 3u;
    expected.sceneSubstate = 2u;
    expected.matchFormationRow = 1u;
    expected.vsFormationApplied = 1u;
    expected.cpuSlotMask = 0u;
    expected.p1ConfirmationState = 1u;
    expected.p2ConfirmationState = vsRow == 1u ? 1u : 0u;
    expected.nativeConfirmationLatch = 0u;
    return expected;
}

inline TwoFighterCharacterSelectStatus observeTwoFighterCharacterSelect(
    const TwoFighterCharacterSelectObservation& observation) {
    if (!isTwoFighterDirectBootRow(static_cast<int>(observation.vsRow))
        || observation.scene != 3u
        || observation.sceneSubstate != 2u) {
        return TwoFighterCharacterSelectStatus::
            NotAtTwoFighterCharacterSelect;
    }
    if (observation.matchFormationRow != 1u
        || observation.vsFormationApplied != 1u
        || observation.cpuSlotMask != 0u
        || observation.p1ConfirmationState > 4u
        || observation.p2ConfirmationState > 4u
        || observation.nativeConfirmationLatch > 30u) {
        return TwoFighterCharacterSelectStatus::NativeFormationViolation;
    }
    if (observation.p1ConfirmationState != 4u
        || observation.p2ConfirmationState != 4u) {
        return TwoFighterCharacterSelectStatus::AwaitingHumanConfirmations;
    }
    return TwoFighterCharacterSelectStatus::NativeLaunchEligible;
}

inline TwoFighterCharacterSelectHandoffAction
decideTwoFighterCharacterSelectHandoff(
    const TwoFighterCharacterSelectObservation& observation) {
    const TwoFighterCharacterSelectStatus status =
        observeTwoFighterCharacterSelect(observation);
    if (status == TwoFighterCharacterSelectStatus::
                      NotAtTwoFighterCharacterSelect
        || status == TwoFighterCharacterSelectStatus::
                         NativeFormationViolation) {
        return TwoFighterCharacterSelectHandoffAction::FailClosed;
    }
    return TwoFighterCharacterSelectHandoffAction::PreserveNativeSelector;
}

inline const char* twoFighterCharacterSelectStatusText(
    TwoFighterCharacterSelectStatus status) {
    switch (status) {
        case TwoFighterCharacterSelectStatus::
            NotAtTwoFighterCharacterSelect:
            return "not-two-fighter-character-select";
        case TwoFighterCharacterSelectStatus::NativeFormationViolation:
            return "native-formation-violation";
        case TwoFighterCharacterSelectStatus::AwaitingHumanConfirmations:
            return "awaiting-human-confirmations";
        case TwoFighterCharacterSelectStatus::NativeLaunchEligible:
            return "native-launch-eligible";
        default:
            return "unknown";
    }
}

enum class MatchEntryStatus : uint8_t {
    NotAtMatchEntry = 0,
    FormationViolation,
    TwoFighterMatchEntryVerified,
};

struct MatchEntryObservation {
    uint32_t vsRow = 0;
    uint32_t sceneNext = 0;
    uint32_t matchFormationRow = 0;
    uint32_t vsFormationApplied = 0;
    uint32_t cpuSlotMask = 0;
};

inline MatchEntryStatus observeTwoFighterMatchEntry(
    const MatchEntryObservation& observation) {
    if (!isTwoFighterDirectBootRow(static_cast<int>(observation.vsRow))
        || observation.sceneNext != 2u) {
        return MatchEntryStatus::NotAtMatchEntry;
    }
    if (observation.matchFormationRow != 1u
        || observation.vsFormationApplied != 1u
        || observation.cpuSlotMask
               != vsRowCpuSlotMaskAtMatchEntry(observation.vsRow)) {
        return MatchEntryStatus::FormationViolation;
    }
    return MatchEntryStatus::TwoFighterMatchEntryVerified;
}

inline const char* matchEntryStatusText(MatchEntryStatus status) {
    switch (status) {
        case MatchEntryStatus::NotAtMatchEntry:  return "not-at-match-entry";
        case MatchEntryStatus::FormationViolation:
            return "formation-violation";
        case MatchEntryStatus::TwoFighterMatchEntryVerified:
            return "two-fighter-match-entry-verified";
        default: return "unknown";
    }
}

enum class NetworkTwoHumanCharacterSelectStatus : uint8_t {
    NotAtNativeRowOneCharacterSelect = 0,
    NativeInvariantViolation,
    AwaitingBothHumanConfirmations,
    AwaitingNativeConfirmationLatch,
    NativeLaunchEligible,
};

enum class NetworkTwoHumanCharacterSelectHandoffAction : uint8_t {
    PreserveNativeSelector = 0,
    FailClosed,
};

struct NetworkTwoHumanCharacterSelectObservation {
    uint32_t vsRow = 0;
    uint32_t scene = 0;
    uint32_t sceneSubstate = 0;
    uint32_t matchFormationRow = 0;
    uint32_t vsFormationApplied = 0;
    uint32_t cpuSlotMask = 0;
    uint32_t p1InputSource = 0;
    uint32_t p2InputSource = 0;
    uint32_t p1InputRoute = 0;
    uint32_t p2InputRoute = 0;
    uint32_t p1ConfirmationState = 0;
    uint32_t p2ConfirmationState = 0;
    uint32_t nativeConfirmationLatch = 0;
    uint32_t p1Cursor = 0;
    uint32_t p2Cursor = 0;
    uint32_t p1PaletteOrForm = 0;
    uint32_t p2PaletteOrForm = 0;
};

inline NetworkTwoHumanCharacterSelectObservation
expectedNovember1126TwoHumanCharacterSelect() {
    NetworkTwoHumanCharacterSelectObservation expected{};
    expected.vsRow = 1u;
    expected.scene = 3u;
    expected.sceneSubstate = 2u;
    expected.matchFormationRow = 1u;
    expected.vsFormationApplied = 1u;
    expected.cpuSlotMask = 0u;
    expected.p1InputSource = 0u;
    expected.p2InputSource = 1u;
    expected.p1InputRoute = 0u;
    expected.p2InputRoute = 1u;
    expected.p1ConfirmationState = 1u;
    expected.p2ConfirmationState = 1u;
    expected.nativeConfirmationLatch = 0u;
    return expected;
}

inline NetworkTwoHumanCharacterSelectStatus
observeNetworkTwoHumanCharacterSelect(
    const NetworkTwoHumanCharacterSelectObservation& observation) {
    if (observation.vsRow != 1u
        || observation.scene != 3u
        || observation.sceneSubstate != 2u) {
        return NetworkTwoHumanCharacterSelectStatus::
            NotAtNativeRowOneCharacterSelect;
    }

    if (observation.matchFormationRow != 1u
        || observation.vsFormationApplied != 1u
        || observation.cpuSlotMask != 0u
        || observation.p1InputSource != 0u
        || observation.p2InputSource != 1u
        || observation.p1InputRoute != 0u
        || observation.p2InputRoute != 1u
        || observation.p1ConfirmationState < 1u
        || observation.p1ConfirmationState > 4u
        || observation.p2ConfirmationState < 1u
        || observation.p2ConfirmationState > 4u
        || observation.nativeConfirmationLatch > 30u) {
        return NetworkTwoHumanCharacterSelectStatus::
            NativeInvariantViolation;
    }

    if (observation.p1ConfirmationState != 4u
        || observation.p2ConfirmationState != 4u) {
        return NetworkTwoHumanCharacterSelectStatus::
            AwaitingBothHumanConfirmations;
    }
    if (observation.nativeConfirmationLatch == 30u) {
        return NetworkTwoHumanCharacterSelectStatus::NativeLaunchEligible;
    }
    return NetworkTwoHumanCharacterSelectStatus::
        AwaitingNativeConfirmationLatch;
}

inline NetworkTwoHumanCharacterSelectHandoffAction
decideNetworkTwoHumanCharacterSelectHandoff(
    const NetworkTwoHumanCharacterSelectObservation& observation) {
    const NetworkTwoHumanCharacterSelectStatus status =
        observeNetworkTwoHumanCharacterSelect(observation);
    if (status == NetworkTwoHumanCharacterSelectStatus::
                      NotAtNativeRowOneCharacterSelect
        || status == NetworkTwoHumanCharacterSelectStatus::
                         NativeInvariantViolation) {
        return NetworkTwoHumanCharacterSelectHandoffAction::FailClosed;
    }
    return NetworkTwoHumanCharacterSelectHandoffAction::
        PreserveNativeSelector;
}

inline const char* networkTwoHumanCharacterSelectStatusText(
    NetworkTwoHumanCharacterSelectStatus status) {
    switch (status) {
        case NetworkTwoHumanCharacterSelectStatus::
            NotAtNativeRowOneCharacterSelect:
            return "not-row-1-character-select";
        case NetworkTwoHumanCharacterSelectStatus::NativeInvariantViolation:
            return "native-invariant-violation";
        case NetworkTwoHumanCharacterSelectStatus::
            AwaitingBothHumanConfirmations:
            return "awaiting-both-human-confirmations";
        case NetworkTwoHumanCharacterSelectStatus::
            AwaitingNativeConfirmationLatch:
            return "awaiting-native-confirmation-latch";
        case NetworkTwoHumanCharacterSelectStatus::NativeLaunchEligible:
            return "native-launch-eligible";
        default:
            return "unknown";
    }
}

inline bool trainingLiveE2EEnvironmentValueExact(
    const char* value, uint32_t length) {
    return value != nullptr
        && length == 1u
        && value[0] == '1'
        && value[1] == '\0';
}

enum class TrainingFrontendInputGateState : uint8_t {
    WaitingForManagedRuntime = 0,
    Captured,
    Rearming,
    Armed,
};

struct TrainingFrontendInputGateSample {
    bool explicitTraining = false;
    bool managedRuntimeArmed = false;
    bool menuCaptured = false;
    bool physicalNeutralBeforeSocd = false;
    uint64_t dispatchSerial = 0;
    uint32_t captureGeneration = 0;
};

struct TrainingFrontendInputGateDecision {
    bool allowInput = false;
    TrainingFrontendInputGateState state =
        TrainingFrontendInputGateState::WaitingForManagedRuntime;
    uint32_t neutralDispatches = 0;
};

class TrainingFrontendInputGatePolicy {
public:
    void reset() {
        _state =
            TrainingFrontendInputGateState::WaitingForManagedRuntime;
        _neutralDispatches = 0;
        _lastDispatchSerial = 0;
        _lastCaptureGeneration = 0;
        _haveDispatch = false;
        _haveCaptureGeneration = false;
    }

    TrainingFrontendInputGateDecision advance(
        const TrainingFrontendInputGateSample& sample) {
        if (!sample.explicitTraining
            || !sample.managedRuntimeArmed) {
            _state =
                TrainingFrontendInputGateState::WaitingForManagedRuntime;
            _neutralDispatches = 0;
            remember(sample);
            return decision(false);
        }

        const bool captureChanged =
            !_haveCaptureGeneration
            || sample.captureGeneration != _lastCaptureGeneration;
        _lastCaptureGeneration = sample.captureGeneration;
        _haveCaptureGeneration = true;

        if (sample.menuCaptured) {
            _state = TrainingFrontendInputGateState::Captured;
            _neutralDispatches = 0;
            rememberDispatch(sample.dispatchSerial);
            return decision(false);
        }

        if (captureChanged
            || _state
                == TrainingFrontendInputGateState::WaitingForManagedRuntime
            || _state == TrainingFrontendInputGateState::Captured) {
            _state = TrainingFrontendInputGateState::Rearming;
            _neutralDispatches = 0;
        }

        if (_state == TrainingFrontendInputGateState::Armed) {
            rememberDispatch(sample.dispatchSerial);
            return decision(true);
        }

        if (_haveDispatch
            && sample.dispatchSerial == _lastDispatchSerial) {
            return decision(false);
        }
        rememberDispatch(sample.dispatchSerial);
        if (sample.physicalNeutralBeforeSocd) {
            if (_neutralDispatches < 2u)
                ++_neutralDispatches;
        } else {
            _neutralDispatches = 0;
        }
        if (_neutralDispatches >= 2u) {
            _state = TrainingFrontendInputGateState::Armed;
            return decision(true);
        }
        return decision(false);
    }

    TrainingFrontendInputGateState state() const {
        return _state;
    }

private:
    void remember(const TrainingFrontendInputGateSample& sample) {
        _lastCaptureGeneration = sample.captureGeneration;
        _haveCaptureGeneration = true;
        rememberDispatch(sample.dispatchSerial);
    }

    void rememberDispatch(uint64_t serial) {
        _lastDispatchSerial = serial;
        _haveDispatch = true;
    }

    TrainingFrontendInputGateDecision decision(bool allow) const {
        TrainingFrontendInputGateDecision result{};
        result.allowInput = allow;
        result.state = _state;
        result.neutralDispatches = _neutralDispatches;
        return result;
    }

    TrainingFrontendInputGateState _state =
        TrainingFrontendInputGateState::WaitingForManagedRuntime;
    uint32_t _neutralDispatches = 0;
    uint64_t _lastDispatchSerial = 0;
    uint32_t _lastCaptureGeneration = 0;
    bool _haveDispatch = false;
    bool _haveCaptureGeneration = false;
};

enum class TrainingStageInputGateState : uint8_t {
    Inactive = 0,
    Rearming,
    Armed,
};

struct TrainingStageInputGateSample {
    bool explicitTraining = false;
    bool managedRuntimeArmed = false;
    bool inCharacterSelect = false;
    bool stageSelectionActive = false;
    bool menuCaptured = false;
    bool actionButtonsNeutral = false;
    uint64_t dispatchSerial = 0;
};

struct TrainingStageInputGateDecision {
    bool allowInput = false;
    TrainingStageInputGateState state =
        TrainingStageInputGateState::Inactive;
    uint32_t neutralDispatches = 0;
};

class TrainingStageInputGatePolicy {
public:
    void reset() {
        _state = TrainingStageInputGateState::Inactive;
        _neutralDispatches = 0u;
        _lastDispatchSerial = 0u;
        _haveDispatch = false;
    }

    TrainingStageInputGateDecision advance(
        const TrainingStageInputGateSample& sample) {
        if (!sample.explicitTraining
            || !sample.managedRuntimeArmed
            || !sample.inCharacterSelect
            || !sample.stageSelectionActive) {
            reset();
            return decision(false);
        }

        if (_state == TrainingStageInputGateState::Inactive) {
            _state = TrainingStageInputGateState::Rearming;
            _neutralDispatches = 0u;
            _haveDispatch = false;
        }
        if (sample.menuCaptured) {
            _state = TrainingStageInputGateState::Rearming;
            _neutralDispatches = 0u;
            rememberDispatch(sample.dispatchSerial);
            return decision(false);
        }
        if (_state == TrainingStageInputGateState::Armed) {
            rememberDispatch(sample.dispatchSerial);
            return decision(true);
        }
        if (_haveDispatch
            && sample.dispatchSerial == _lastDispatchSerial) {
            return decision(false);
        }
        rememberDispatch(sample.dispatchSerial);
        if (sample.actionButtonsNeutral) {
            if (_neutralDispatches < 2u) ++_neutralDispatches;
        } else {
            _neutralDispatches = 0u;
        }
        if (_neutralDispatches >= 2u) {
            _state = TrainingStageInputGateState::Armed;
            return decision(true);
        }
        return decision(false);
    }

    TrainingStageInputGateState state() const { return _state; }

private:
    void rememberDispatch(uint64_t serial) {
        _lastDispatchSerial = serial;
        _haveDispatch = true;
    }

    TrainingStageInputGateDecision decision(bool allow) const {
        TrainingStageInputGateDecision result{};
        result.allowInput = allow;
        result.state = _state;
        result.neutralDispatches = _neutralDispatches;
        return result;
    }

    TrainingStageInputGateState _state =
        TrainingStageInputGateState::Inactive;
    uint32_t _neutralDispatches = 0u;
    uint64_t _lastDispatchSerial = 0u;
    bool _haveDispatch = false;
};

enum class TrainingSameCallConfirmAction : uint8_t {
    None = 0,
    HoldP2Inactive,
    FailClosed,
};

struct TrainingSameCallConfirmSample {
    bool explicitTraining = false;
    uint32_t scene = 0;
    uint32_t sceneSubstate = 0;
    uint32_t selectorPlayer = 0;
    uint32_t matchMode = 0;
    uint32_t controlMask = 0;
    uint32_t p1StateBefore = 0;
    uint32_t p1StateAfter = 0;
    uint32_t p2StateBefore = 0;
    uint32_t p2StateAfter = 0;
    uint32_t p1InputSource = 0;
    uint32_t p2InputSource = 0;
    uint32_t p2Route = 0;
};

inline TrainingSameCallConfirmAction decideTrainingSameCallConfirm(
    const TrainingSameCallConfirmSample& sample) {
    if (!sample.explicitTraining
        || sample.scene != 3u
        || sample.sceneSubstate != 2u
        || sample.selectorPlayer != 0u
        || sample.p1StateBefore != 2u
        || sample.p1StateAfter != 4u) {
        return TrainingSameCallConfirmAction::None;
    }

    if (sample.matchMode != 1u
        || (sample.controlMask & 3u) != 0u
        || sample.p2StateBefore > 1u
        || sample.p2StateAfter != 1u
        || sample.p1InputSource != 0u
        || sample.p2InputSource != sample.p1InputSource
        || sample.p2Route != 1u) {
        return TrainingSameCallConfirmAction::FailClosed;
    }
    return TrainingSameCallConfirmAction::HoldP2Inactive;
}

struct TrainingReturnedStageResetDecision {
    bool apply = false;
    uint32_t selectionKind = 2u;
    uint32_t selectionValue = 0u;
};

inline TrainingReturnedStageResetDecision decideTrainingReturnedStageReset(
    bool explicitTraining, bool awaitingReturnToCharacterSelect,
    uint32_t scene, uint32_t sceneSubstate) {
    TrainingReturnedStageResetDecision decision{};
    decision.apply = explicitTraining
        && awaitingReturnToCharacterSelect
        && scene == 3u
        && sceneSubstate == 2u;
    return decision;
}

enum class TrainingSelectionPhase : uint8_t {
    AwaitingCharacterSelect = 0,
    SelectingP1,
    ReleasingP1Buttons,
    SelectingP2,
    P2ConfirmedSettling,
    Complete,
    Failed,
};

enum class TrainingSelectionFailure : uint8_t {
    None = 0,
    UnexpectedScene,
    InvalidRow1State,
    HumanOwnershipLost,
    NativeP2NormalizationMissing,
};

struct TrainingSelectionSample {
    uint32_t scene = 0;
    uint32_t sceneSubstate = 0;
    uint32_t sceneNext = 0;
    uint32_t matchMode = 0;       // dword_62369C; row 1 keeps 1 in select
    uint32_t controlMask = 0;
    uint32_t p1State = 0;
    uint32_t p2State = 0;
    uint32_t p1InputSource = 0;
    uint32_t p2InputSource = 0;
    uint32_t p2Route = 0;         // dword_613F14; 1 = eventual human P2
    uint32_t p1LogicalMask = 0;
    bool stageSelectionActive = false;
};

struct TrainingSelectionDecision {
    bool writeP2State = false;
    uint32_t p2State = 0;
    bool writeP2InputSource = false;
    uint32_t p2InputSource = 0;
    bool writeP2Route = false;
    uint32_t p2Route = 1;
    bool resetP2SelectorOwner = false;
    bool completed = false;
    TrainingSelectionFailure failure = TrainingSelectionFailure::None;
};

class TrainingSelectionPolicy {
public:
    void reset() {
        _phase = TrainingSelectionPhase::AwaitingCharacterSelect;
        _failure = TrainingSelectionFailure::None;
        _releaseSteps = 0;
    }

    TrainingSelectionPhase phase() const { return _phase; }

    TrainingSelectionDecision advance(const TrainingSelectionSample& sample) {
        TrainingSelectionDecision decision{};
        if (_phase == TrainingSelectionPhase::Complete) {
            decision.completed = true;
            return decision;
        }
        if (_phase == TrainingSelectionPhase::Failed) {
            decision.failure = _failure;
            return decision;
        }

        if (sample.scene == 2 || sample.sceneNext == 2) {
            if (_phase == TrainingSelectionPhase::AwaitingCharacterSelect) {
                return fail(TrainingSelectionFailure::UnexpectedScene);
            }
            if (sample.p2InputSource != 1 || sample.p2Route != 1) {
                return fail(
                    TrainingSelectionFailure::NativeP2NormalizationMissing);
            }
            _phase = TrainingSelectionPhase::Complete;
            decision.completed = true;
            return decision;
        }

        if (sample.scene != 3 || sample.sceneSubstate != 2)
            return fail(TrainingSelectionFailure::UnexpectedScene);
        if (sample.matchMode != 1 || sample.p1InputSource != 0
            || sample.p1State < 1 || sample.p1State > 4
            || sample.p2State > 4) {
            return fail(TrainingSelectionFailure::InvalidRow1State);
        }
        if ((sample.controlMask & 3u) != 0 || sample.p2Route != 1)
            return fail(TrainingSelectionFailure::HumanOwnershipLost);

        decision.writeP2InputSource = true;
        decision.p2InputSource = 0;
        decision.writeP2Route = true;
        decision.p2Route = 1;
        if (sample.stageSelectionActive)
            decision.p2InputSource = 1;

        if (_phase == TrainingSelectionPhase::AwaitingCharacterSelect) {
            decision.writeP2State = true;
            decision.p2State = 0;
            decision.resetP2SelectorOwner = true;
            _phase = TrainingSelectionPhase::SelectingP1;
            return decision;
        }

        if (_phase == TrainingSelectionPhase::SelectingP1) {
            decision.writeP2State = true;
            decision.p2State = 0;
            if (sample.p1State == 4) {
                _releaseSteps = 0;
                _phase = TrainingSelectionPhase::ReleasingP1Buttons;
            }
            return decision;
        }

        if (_phase == TrainingSelectionPhase::ReleasingP1Buttons) {
            ++_releaseSteps;
            decision.writeP2State = true;
            decision.p2State = 0;
            if (_releaseSteps >= 2
                && (sample.p1LogicalMask & 0x0Fu) == 0) {
                decision.p2State = 1;
                _phase = TrainingSelectionPhase::SelectingP2;
            }
            return decision;
        }

        if (_phase == TrainingSelectionPhase::SelectingP2) {
            if (sample.p1State < 4) {
                decision.writeP2State = true;
                decision.p2State = 0;
                _phase = TrainingSelectionPhase::SelectingP1;
            } else if (sample.p2State == 4) {
                _phase = TrainingSelectionPhase::P2ConfirmedSettling;
            }
            return decision;
        }

        if (_phase == TrainingSelectionPhase::P2ConfirmedSettling) {
            if (sample.p1State < 4) {
                decision.writeP2State = true;
                decision.p2State = 0;
                _phase = TrainingSelectionPhase::SelectingP1;
            } else if (sample.p2State < 4) {
                _phase = TrainingSelectionPhase::SelectingP2;
            }
            return decision;
        }

        return fail(TrainingSelectionFailure::UnexpectedScene);
    }

private:
    TrainingSelectionDecision fail(TrainingSelectionFailure failure) {
        _phase = TrainingSelectionPhase::Failed;
        _failure = failure;
        TrainingSelectionDecision decision{};
        decision.failure = failure;
        return decision;
    }

    TrainingSelectionPhase _phase =
        TrainingSelectionPhase::AwaitingCharacterSelect;
    TrainingSelectionFailure _failure =
        TrainingSelectionFailure::None;
    uint32_t _releaseSteps = 0;
};

inline const char* trainingSelectionFailureText(
    TrainingSelectionFailure failure) {
    switch (failure) {
        case TrainingSelectionFailure::None:
            return "none";
        case TrainingSelectionFailure::UnexpectedScene:
            return "unexpected scene before native selector completion";
        case TrainingSelectionFailure::InvalidRow1State:
            return "native row-1 selector state is invalid";
        case TrainingSelectionFailure::HumanOwnershipLost:
            return "P1/P2 human ownership invariant was lost";
        case TrainingSelectionFailure::NativeP2NormalizationMissing:
            return "sub_434710 did not normalize P2 source to player 2";
        default:
            return "unknown training selector failure";
    }
}

}

bool startDirectBoot(int vsRow);

bool startDirectBootToCharSelect(int vsRow);

bool startTrainingDirectBootToCharSelect(int vsRow);

void rearmTrainingCharacterSelect();

bool directBootOfflineActive();
bool directBootOfflineFailed();
void directBootOfflineTick();

bool directBootUnattendedFrontendActive();

bool startNetworkDirectBootToCharSelect(int vsRow);

struct ReplayDirectBootSlot {
    uint8_t cursorCell = 0u;
    uint8_t alternateFlag = 0u;
    uint8_t paletteIndex = 0u;
    uint8_t confirmState = 4u;
};

struct ReplayDirectBootConfig {
    uint8_t vsRow = 1u;
    ReplayDirectBootSlot slots[2]{};
    uint32_t logicalStageIndex = 0u;
};

bool startReplayDirectBootToMatch(const ReplayDirectBootConfig& config);
bool directBootNetworkActive();
bool directBootNetworkComplete();
bool directBootNetworkFailed();
bool directBootNetworkAtStartBarrier();
void directBootNetworkTick();

void stopDirectBoot();

}
