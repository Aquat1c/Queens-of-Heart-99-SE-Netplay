#pragma once

#include <cstdint>

#include "training/TrainingTypes.h"

namespace qtraining {

enum TrainingNeutralReason : uint32_t {
    TrainingNeutralOk = 0u,
    NeutralFighterUnreadable = 1u << 0,
    // Slots 2 and 3 must never be touched: QOH's round adjudication dereferences off_4A37BC, which is null in a 1v1, without a null check.
    NeutralSlotOutOfRange = 1u << 1,
    // +1456: an alternate FORM is participating (Kaede's prior life, etc.).
    NeutralAlternateFormActive = 1u << 2,
    // The VALUES below are the wire contract shared with the wrapper.
    NeutralActionNotFreeMovement = 1u << 3,
    NeutralDispatchNotFreeMovement = 1u << 4,
    NeutralInitNotRunning = 1u << 5,
    NeutralTransitionPending = 1u << 6,
    NeutralActionLockSet = 1u << 7,
    NeutralPostureNotGrounded = 1u << 8,
    NeutralTerminalLatched = 1u << 9,
    NeutralRoundResultLatched = 1u << 10,
    NeutralFatalRecorded = 1u << 11,
    NeutralComOwned = 1u << 12,
    NeutralRecoveryLockSet = 1u << 13,
    NeutralRecoveryPhaseLatch = 1u << 14,
    NeutralComboActive = 1u << 15,
    // Life record +4/+8/+12: settlement still moving reserve or subtracting pending damage.
    NeutralDamageUnsettled = 1u << 16,
    NeutralRoundResultState = 1u << 17,
    // 0x6275A0: broad F4 flag, forced to zero by training.
    NeutralFatalDisableFlag = 1u << 18,
};

constexpr uint32_t kTrainingNeutralHoldSteps = 2;

constexpr uint32_t kTrainingExactIdleHoldSteps = 11;

constexpr uint32_t kTrainingComboSettleSteps = 30;

struct FighterNeutralObservation {
    bool readable = false;
    uint8_t slot = 0xFFu;
    uint32_t alternateForm = 0;     // +1456
    uint32_t action = 0;            // +48
    uint32_t dispatchAction = 0;    // +84
    int32_t initStatus = 0;         // +88
    uint32_t transitionFlag = 0;    // +76
    uint32_t actionLockEnd = 0;     // +56
    uint8_t postureNibble = 0;      // +156
    uint32_t terminalReady = 0;     // +1528
    uint32_t roundEnd = 0;          // +1364
    uint32_t roundLost = 0;         // +1368
    int32_t fatalCause = -1;        // +1376, round-start value is -1
    uint32_t comOwnership = 0;      // +1400
    uint32_t recoveryLock = 0;      // 0x6265DC + 44*slot
    uint32_t recoveryPhaseLatch = 0;// 0x6265D8 + 44*slot
    uint32_t comboHits = 0;         // 0x6265D0 + 44*slot
    int32_t lifePendingDamage = 0;  // life record +4
    int32_t lifeReserve = 0;        // life record +8
    int32_t lifeSettleTimer = 0;    // life record +12
};

struct NeutralGlobalObservation {
    bool readable = false;
    uint32_t roundResultState = 0;    // 0x88098C
    uint32_t fatalHitDisableFlag = 0; // 0x6275A0
};

uint32_t evaluateFighterNeutral(const FighterNeutralObservation& observation);
uint32_t evaluateNeutralGlobals(const NeutralGlobalObservation& observation);

// Free-movement ids: 0 idle (sub_401CD0), 26/27 walk forward/backward (sub_401D10), 28 crouch (sub_402A80).
constexpr bool trainingFreeMovementAction(uint32_t action) {
    return action == 0u || action == 26u || action == 27u || action == 28u;
}

constexpr bool trainingExactIdleAction(
        const FighterNeutralObservation& observation) {
    return observation.readable
        && trainingFreeMovementAction(observation.action);
}

constexpr bool trainingReactionOrRecoveryAction(uint32_t action) {
    if (action >= 200u) return true;
    switch (action) {
        case 18: case 19: case 20:  // guard: stand / crouch / air, sub_401F10
        case 21: case 22: case 23:  // ground hitstun, sub_4025D0
        case 24:
        case 25: case 38:
        case 56: case 57: case 58:  // 25 swept/launched, 56/57/58 air hitstun
        case 31: case 33: case 34:
        case 35:                    // hard knockdown, sub_4034E0
        case 36:
        case 37:
        case 70:
        case 73:                    // exact native throw-tech result
            return true;
        default:
            return false;
    }
}

class NeutralTrigger {
public:
    void reset();
    bool advance(bool bothFightersNeutral);
    bool neutral() const { return neutral_; }
    uint32_t holdSteps() const { return hold_; }

private:
    uint32_t hold_ = 0;
    bool neutral_ = false;
};

class IdleStateTrigger {
public:
    void reset();
    bool advance(bool bothFightersIdle);
    uint32_t holdSteps() const { return hold_; }

private:
    uint32_t hold_ = 0;
};

enum class ComboEpisodeState : uint32_t {
    Idle = 0,
    InEpisode = 1,
    Settling = 2,
};

struct ComboEpisodeObservation {
    bool readable = false;
    bool inReactionAction = false;
    uint32_t nativeComboHits = 0;
    bool damageSettled = false;
    bool recoveryLocksClear = false;
    uint64_t admittedDamageSerial = 0;
};

class ComboEpisodeMachine {
public:
    void reset();
    bool advance(const ComboEpisodeObservation& observation,
                 uint32_t settleSteps);
    ComboEpisodeState state() const { return state_; }

private:
    ComboEpisodeState state_ = ComboEpisodeState::Idle;
    uint32_t settle_ = 0;
    uint64_t lastDamageSerial_ = 0;
    bool haveSerial_ = false;
};

class RecoveryExitTrigger {
public:
    void reset();
    bool advance(bool inReactionAction, bool locksClear);

private:
    bool wasInReaction_ = false;
};

struct ResourcePolicySettings {
    ResourceTrigger trigger = ResourceTrigger::Off;
    TrainingRefillTarget target = TrainingRefillTarget::Both;
};

struct ResourcePolicySet {
    ResourcePolicySettings health{};
    ResourcePolicySettings ex{};
    ResourcePolicySettings guardGauge{};
};

struct ResourceTriggerSignals {
    bool neutralReady = false;
    bool exactIdleReady = false;
    bool resetAccepted = false;
    bool comboEpisodeClosed[kTrainingRefillPlayerCount] = {};
    bool recoveryExited[kTrainingRefillPlayerCount] = {};
};

uint32_t resolvePolicyFire(const ResourcePolicySettings& policy,
                           const ResourceTriggerSignals& signals);

uint32_t resolveHealthPolicyFire(const ResourcePolicySettings& policy,
                                 const ResourceTriggerSignals& signals);

enum TrainingRoundStartNeutralBit : uint32_t {
    RoundStartNeutralAction = 1u << 0,
    RoundStartNeutralDispatch = 1u << 1,
    RoundStartNeutralInit = 1u << 2,
    RoundStartNeutralNoPending = 1u << 3,
    RoundStartNeutralGrounded = 1u << 4,
    RoundStartNeutralNoTerminal = 1u << 5,
    RoundStartNeutralNoWall = 1u << 6,
    RoundStartNeutralFacing = 1u << 7,
    RoundStartNeutralNoResult = 1u << 8,
    RoundStartNeutralFatalIdle = 1u << 9,
    RoundStartNeutralNoCom = 1u << 10,
    RoundStartNeutralLocksClear = 1u << 11,
    RoundStartNeutralPosition = 1u << 12,
    RoundStartNeutralFrameOk = 1u << 13,
    RoundStartNeutralAllBits = (1u << 14) - 1u,
};

constexpr uint32_t trainingRoundStartNeutralEscalatingBits() {
    return RoundStartNeutralNoResult
        | RoundStartNeutralNoCom
        | RoundStartNeutralFrameOk;
}

struct RoundStartNeutralObservation {
    bool readable = false;
    uint32_t action = 0;
    uint32_t dispatchAction = 0;
    int32_t initStatus = 0;
    uint32_t transitionFlag = 0;
    uint8_t postureNibble = 0;
    uint32_t terminalReady = 0;
    uint32_t wallFlags = 0;
    uint8_t facing = 0;
    uint8_t expectedFacing = 0;
    uint32_t roundEnd = 0;
    uint32_t roundLost = 0;
    int32_t fatalCause = -1;
    uint32_t comOwnership = 0;
    uint32_t recoveryLock = 0;
    uint32_t recoveryPhaseLatch = 0;
    bool positionMatchesPlan = false;
    bool framePointerInRange = false;
};

uint32_t evaluateRoundStartNeutral(
    const RoundStartNeutralObservation& observation);

}
