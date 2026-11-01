#pragma once

#include <cstdint>

#include "input/InputActions.h"
#include "training/TrainingTypes.h"

namespace qtraining {

constexpr uint32_t kRecoveryWindowAir = 30;
constexpr uint32_t kRecoveryWindowGround = 20;
constexpr uint32_t kRecoveryWindowTerminal = 10;
constexpr uint32_t kRecoveryWindowThrowEscape = 1;

constexpr uint8_t kRecoveryPermitGround = 0x04;
constexpr uint8_t kRecoveryPermitAir = 0x08;

constexpr uint32_t kRecoveryRingForward = 0x80000u;
constexpr uint32_t kRecoveryRingBack = 0x40000u;
constexpr uint32_t kRecoveryButtonRingThrow = 4u;

// Every matcher mode caps its backward scan at 16 ring entries.
constexpr uint32_t kRecoveryRingScanCap = 16;

constexpr uint32_t kRecoveryDirectionRingSize = 32;

struct DirectionRingSnapshot {
    uint32_t values[kRecoveryDirectionRingSize] = {};
    uint32_t dwell[kRecoveryDirectionRingSize] = {};
    uint8_t head = 0;
};

bool terminalRecoveryWouldMatch(const DirectionRingSnapshot& ring,
                                uint32_t nextDirection);

constexpr uint32_t kRecoveryAwaitCapSteps = 4;

constexpr uint32_t kRecoveryRetryGapSteps = 8;

constexpr uint32_t kRecoveryLateDelaySteps = 8;

constexpr bool nativeRecoveryReleaseQualified(
    ManualRecoveryMode mode, bool nativeRecoveryEvaluatorQualified) {
    switch (mode) {
        case ManualRecoveryMode::Off:
            return true;
        case ManualRecoveryMode::Earliest:
        case ManualRecoveryMode::FixedDelay:
        case ManualRecoveryMode::RandomDelay:
        case ManualRecoveryMode::Late:
        default:
            return nativeRecoveryEvaluatorQualified;
    }
}

constexpr RecoveryEvaluatorReason recoveryEvaluatorReasonFor(
    bool attempted, bool addressesResolved, bool prologuesMatched) {
    if (!attempted) return RecoveryEvaluatorReason::NotAttempted;
    if (!addressesResolved) return RecoveryEvaluatorReason::AddressUnresolved;
    if (!prologuesMatched) return RecoveryEvaluatorReason::PrologueMismatch;
    return RecoveryEvaluatorReason::Qualified;
}

constexpr bool recoveryAirEligibleAction(uint32_t action) {
    return action == 25 || action == 38
        || action == 56 || action == 57 || action == 58;
}

constexpr bool recoveryGroundEligibleAction(uint32_t action) {
    return action < 200
        && (action == 25 || action == 35 || action == 38
            || action == 56 || action == 57 || action == 58);
}

constexpr bool recoveryWallEligibleAction(uint32_t action) {
    return action == 37;
}

constexpr bool recoveryTerminalEligibleAction(uint32_t action) {
    return action == 25 || action == 35 || action == 37 || action == 38
        || action == 56 || action == 57 || action == 58
        || action >= 200;
}

constexpr bool recoveryPostureAdmitsManual(uint8_t postureNibble) {
    const uint8_t posture = postureNibble & 0x0Fu;
    return posture == 1 || posture == 2 || posture == 4;
}

constexpr bool recoveryPostureAdmitsTerminal(uint8_t postureNibble) {
    const uint8_t posture = postureNibble & 0x07u;
    return posture == 1 || posture == 2;
}

constexpr bool recoveryBranchConsultsManualLock(RecoveryBranch branch) {
    return branch == RecoveryBranch::Air22
        || branch == RecoveryBranch::Ground88
        || branch == RecoveryBranch::Wall66;
}

constexpr bool isRecoveryResultAction(uint32_t action) {
    return action == 13 || action == 14 || action == 31
        || action == 33 || action == 34;
}

bool recoveryBranchAcceptsResult(RecoveryBranch branch, uint32_t action);
bool recoveryBranchActionEligible(RecoveryBranch branch, uint32_t action);
uint32_t recoveryBranchWindow(RecoveryBranch branch);

struct RecoveryObservation {
    bool readable = false;

    uint32_t action = 0;            // fighter +48
    uint32_t nextAction = 0;        // +72   (matcher's request, never written)
    uint32_t transitionFlag = 0;    // +76
    uint32_t committedAction = 0;   // +84   (what sub_4017F0 dispatches on)
    int32_t initStatus = 0;         // +88   (-1 needs init, 0 running, 1 pending)
    uint32_t animFrame = 0;         // +64
    uint8_t permission = 0;         // +158 & 0x0F, mirror-OR'd when +1456 == 1
    uint8_t postureNibble = 0;      // +156 & 0x0F
    uint8_t facing = 0;             // +170
    uint8_t slot = 0;               // +172
    uint16_t wallFlags = 0;         // +1390 & 3, WORLD space
    bool terminalReady = false;     // +1528 bit 0
    // +1368: set when life goes below zero; blocks every transition commit, including 31/33.
    bool koLatch = false;
    bool comControlled = false;
    // commandChild + 432 == 1 forces the recorded direction to zero before insertion.
    bool directionSuppressed = false;
    uint32_t commandChild = 0;
    uint32_t recoveryLock = 0;      // 0x6265DC + 44*slot
    uint32_t recoveryPhaseLatch = 0;// 0x6265D8 + 44*slot, OBSERVE ONLY
    uint32_t directionRingHead = 0;
    uint32_t buttonRingHead = 0;

    bool lifecycleAvailable = false;
    bool sandboxActive = false;
    bool menuCaptured = false;
    bool operationNeutral = false;
    bool macroPlaybackActive = false;
    bool macroOverrideP1 = false;
    uint32_t trainingGeneration = 0;

    uint32_t hitEpoch = 0;
};

struct RecoverySettings {
    ManualRecoveryMode air = ManualRecoveryMode::Off;
    ManualRecoveryMode ground = ManualRecoveryMode::Off;
    ManualRecoveryMode wall = ManualRecoveryMode::Off;
    ManualRecoveryMode terminal = ManualRecoveryMode::Off;
    ManualRecoveryMode throwEscape = ManualRecoveryMode::Off;
    TerminalRecoveryChoice terminalChoice = TerminalRecoveryChoice::Preserve;
    uint32_t delaySteps = 0;
    uint32_t randomMinSteps = 0;
    uint32_t randomMaxSteps = 0;
    bool evaluatorQualified = false;
};

struct RecoveryEmission {
    bool valid = true;
    bool duplicateStep = false;

    bool active = false;
    uint32_t directions = 0;
    uint32_t buttons = 0;
    bool ownsButtons = false;

    RecoveryState state = RecoveryState::Idle;
    RecoveryBranch branch = RecoveryBranch::Air22;
    uint32_t eligibleMask = 0;
    uint32_t armableMask = 0;
    uint32_t standDownMask = RecoveryStandDownNone;
    RecoveryVerdict lastVerdict = RecoveryVerdict::None;
    uint32_t tapsEmitted = 0;
    bool verdictIsNew = false;
    bool contactTestUnknown = false;
    uint32_t stepsRemaining = 0;
};

struct RecoveryCounters {
    uint32_t attempts = 0;
    uint32_t succeeded = 0;
    uint32_t missed = 0;
    uint32_t cancelled = 0;
    uint32_t bufferedObserved = 0;
    uint32_t automaticObserved = 0;
    uint32_t commitBlocked = 0;
    uint32_t lastResultAction = 0;
};

uint32_t recoveryStandDownMask(const RecoveryObservation& observation);

uint32_t recoveryEligibleMask(const RecoveryObservation& observation);
uint32_t recoveryArmableMask(const RecoveryObservation& observation);

struct RecoveryStepMask {
    uint32_t directions = 0;
    uint32_t buttons = 0;
};

uint32_t recoveryEmissionStepCount(RecoveryBranch branch);
RecoveryStepMask recoveryEmissionStep(RecoveryBranch branch,
                                      uint32_t index, uint8_t facing);
RecoveryStepMask recoveryAwaitMask(RecoveryBranch branch, uint8_t facing);

// Training-local, never QOH's sub_4914B0: the native RNG is consumed by the simulation and must not be perturbed.
uint64_t recoverySplitMix64(uint64_t& state);
uint64_t recoverySeedFor(uint32_t trainingGeneration);

class TrainingRecoveryController {
public:
    void reset(uint32_t trainingGeneration);

    void resetTimeline();

    RecoveryEmission advance(uint64_t realStepSerial,
                             const RecoverySettings& settings,
                             const RecoveryObservation& observation);

    const RecoveryCounters& counters() const { return counters_; }

    uint64_t rngState() const { return rng_; }
    void restoreRngState(uint64_t state) { rng_ = state; }

private:
    struct Attempt {
        bool active = false;
        RecoveryBranch branch = RecoveryBranch::Air22;
        RecoveryState state = RecoveryState::Idle;
        uint8_t slot = 0;
        uint32_t commandChild = 0;
        uint32_t delayRemaining = 0;
        uint32_t emitIndex = 0;
        uint32_t awaitRemaining = 0;
        uint32_t tapsEmitted = 0;
        uint64_t lastTapStep = 0;
        uint64_t patternLiveUntilStep = 0;
    };

    void cancelAttempt(RecoveryVerdict verdict);
    bool generationStillValid(const RecoveryObservation& observation) const;
    uint32_t delayForMode(ManualRecoveryMode mode,
                          const RecoverySettings& settings);
    ManualRecoveryMode modeForBranch(const RecoverySettings& settings,
                                     RecoveryBranch branch) const;
    bool chooseArmableBranch(const RecoverySettings& settings,
                             uint32_t risingMask, RecoveryBranch& branch);

    void publishVerdict(RecoveryEmission& result, RecoveryVerdict verdict,
                        uint32_t taps);

    uint32_t reportedTaps() const {
        return attempt_.active ? attempt_.tapsEmitted : lastAttemptTaps_;
    }

    Attempt attempt_{};
    RecoveryCounters counters_{};
    RecoveryEmission cached_{};
    RecoveryVerdict lastVerdict_ = RecoveryVerdict::None;
    uint32_t lastAttemptTaps_ = 0;
    uint64_t rng_ = 0;
    uint32_t generation_ = 0;
    uint32_t previousArmableMask_ = 0;
    uint32_t previousAction_ = 0;
    bool haveObservation_ = false;
    uint32_t previousHitEpoch_ = 0;
    uint64_t retryNotBeforeStep_ = 0;
    bool haveStep_ = false;
    uint64_t lastStepSerial_ = 0;
};

}
