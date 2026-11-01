#pragma once

#include <cstddef>
#include <cstdint>

#include "training/TrainingTypes.h"

namespace qtraining {

constexpr uint8_t kCombatOwnerCount = 2;
constexpr uint8_t kCombatDummyOwner = 1;
constexpr std::size_t kTriggerPendingCapacity = 16;
constexpr uint64_t kFrameAdvantageTimeoutSteps = 300;
constexpr uint64_t kFrameAdvantageReadoutLifetimeSteps = 60;
constexpr uint64_t kFrameAdvantageMaxGapSteps = 60;
constexpr uint64_t kFrameAdvantageGapDisplaySteps = 20;

constexpr bool combatActionIsGuard(uint32_t action) {
    return action >= 18 && action <= 20;
}

constexpr bool combatActionIsGroundHit(uint32_t action) {
    return action >= 21 && action <= 23;
}

constexpr bool combatActionIsAirReaction(uint32_t action) {
    return action == 24 || action == 25 || action == 38
        || (action >= 56 && action <= 58);
}

constexpr bool combatActionIsKnockdown(uint32_t action) {
    return action == 35 || action == 36;
}

constexpr bool combatActionIsAirRecovery(uint32_t action) {
    return action == 31;
}

constexpr bool combatActionIsGroundRecovery(uint32_t action) {
    return action == 33 || action == 34;
}

constexpr bool combatActionIsWallRecoveryPath(uint32_t action) {
    return action == 37;
}

// A natural wake finishes from actions 25/38/56..58 (recoverable), 35/36 (hard knockdown/pre-wall) or 37 (wall); 24 is air guard crush and 31/33/34 are recovery results.
constexpr bool combatActionIsNaturalWakeupPath(uint32_t action) {
    return action == 25 || action == 35 || action == 36 || action == 37
        || action == 38 || (action >= 56 && action <= 58)
        || action >= 200;
}

constexpr bool combatActionIsThrowTechResult(uint32_t action) {
    return action == 73;
}

constexpr bool combatActionIsThrowAttackerPath(uint32_t action) {
    return action == 70u || action == 72u;
}

constexpr bool combatThrowAttackerTupleIsHold(
        uint32_t resourceAction, uint32_t dispatchAction) {
    return (resourceAction == 70u
            && (dispatchAction == 70u || dispatchAction == 72u))
        || (resourceAction == 72u && dispatchAction == 72u);
}

constexpr bool combatThrowTechCounterpartContinues(
        uint32_t sourceResourceAction, uint32_t sourceDispatchAction,
        uint32_t targetResourceAction, uint32_t targetDispatchAction) {
    return (sourceResourceAction == 70u && sourceDispatchAction == 70u
            && ((targetResourceAction == 70u
                 && targetDispatchAction == 72u)
                || (targetResourceAction == 72u
                    && targetDispatchAction == 72u)))
        || (sourceResourceAction == 70u && sourceDispatchAction == 72u
            && targetResourceAction == 72u
            && targetDispatchAction == 72u);
}

constexpr bool combatActionIsThrowReception(uint32_t action) {
    return action >= 200u;
}

// Native reaction list (sub_483F80): +48 in {25, 35, 37, 38, 56, 57, 58} or +48 >= 200.
constexpr bool combatActionIsNativeReactionFamily(uint32_t action) {
    return action == 25u || action == 35u || action == 37u || action == 38u
        || (action >= 56u && action <= 58u)
        || action >= 200u;
}

// Action 36 (sub_4037B0) commits a transition only when +88 == 1; a command never takes effect in it, so it arms a wakeup but must never fire one.
constexpr bool combatActionCannotCommitCommand(uint32_t action) {
    return action == 36u;
}

constexpr bool combatActionIsHitReaction(uint32_t action) {
    return combatActionIsGroundHit(action)
        || combatActionIsAirReaction(action)
        || combatActionIsKnockdown(action);
}

enum class CombatContactKind : uint8_t {
    None = 0,
    Hit = 1,
    Block = 2,
    Throw = 3,
};

enum class PrimaryRecoveryDisposition : uint8_t {
    Pending = 0,
    Ready = 1,
    Cancelled = 2,
    Interrupted = 3,
    ActorChanged = 4,
    Continued = 5,
    Rechained = 6,
};

struct CombatOwnerSample {
    uint32_t action = 0;
    bool naturalReady = false;
    bool responseReady = false;
    // Guard-cancel permission is bit 0x04 of +158.
    bool guardCancelReady = false;

    PrimaryRecoveryDisposition primaryRecovery =
        PrimaryRecoveryDisposition::Pending;
    uint64_t actorToken = 0;
    uint64_t actionEpoch = 0;

    bool defensiveLock = false;
    bool semanticPathValid = true;
};

struct CombatFrameSample {
    bool readable = false;
    uint32_t trainingGeneration = 0;
    uint64_t stepSerial = 0;
    CombatOwnerSample owners[kCombatOwnerCount] = {};
    bool roundValid = true;
};

struct CombatContactEvent {
    uint64_t serial = 0;
    uint64_t stepSerial = 0;
    CombatContactKind kind = CombatContactKind::None;
    uint8_t attackerOwner = 0;
    uint8_t victimOwner = 0;
    bool attackerIsPrimary = true;

    uint64_t attackerActorToken = 0;
    uint64_t victimActorToken = 0;
    uint64_t attackerActionEpoch = 0;
    uint64_t victimActionEpoch = 0;

    bool victimIsPrimary = true;
    bool defenderLockEligible = true;
};

constexpr bool combatContactIsWellFormed(const CombatContactEvent& contact) {
    return contact.serial != 0
        && (contact.kind == CombatContactKind::Hit
            || contact.kind == CombatContactKind::Block
            || contact.kind == CombatContactKind::Throw)
        && contact.attackerOwner < kCombatOwnerCount
        && contact.victimOwner < kCombatOwnerCount
        && contact.attackerOwner != contact.victimOwner;
}

enum class TriggerRequestOutcome : uint8_t {
    None = 0,
    Started = 1,
    ChanceSkipped = 2,
    DroppedBusy = 3,
    DroppedEmptySlot = 4,
    DroppedCollision = 5,
    DroppedQueueFull = 6,
    DroppedUnavailableMove = 7,
    DroppedWindowClosed = 8,
    DroppedConfigurationChanged = 9,
    DroppedSuperseded = 10,
};

struct TrainingTriggerStatus {
    uint32_t trainingGeneration = 0;
    uint32_t pendingCount = 0;
    uint32_t startedCount = 0;
    uint32_t chanceSkippedCount = 0;
    uint32_t droppedCount = 0;
    uint32_t invalidContactCount = 0;
    uint64_t lastContactSerial = 0;
    uint64_t lastEventStep = 0;
    TrainingTriggerEvent lastEvent = TrainingTriggerEvent::OnHit;
    uint8_t lastMacroSlot = 0;
    uint16_t lastMoveIndex = 0;
    TriggerRequestOutcome lastOutcome = TriggerRequestOutcome::None;
};

enum class TriggerSuppressionReason : uint8_t {
    None = 0,
    NoEventArmed = 1,
    EventArmedNotConfigured = 2,
    WaitingForResponseReady = 3,
    WaitingForDelay = 4,
    WaitingForGuardCancelWindow = 5,
    WaitingForWakeupRelease = 6,
    BlockedMacroBusy = 7,
    BlockedMoveUnavailable = 8,
    BlockedEmptySlot = 9,
    ChanceSkipped = 10,
    TimelineNotMeasurable = 11,
};

struct TrainingTriggerDiagnostics {
    TriggerSuppressionReason reason = TriggerSuppressionReason::NoEventArmed;
    uint8_t armedEventMask = 0;
    bool haveArmedEvent = false;
    TrainingTriggerEvent armedEvent = TrainingTriggerEvent::OnHit;
    bool havePending = false;
    TrainingTriggerEvent pendingEvent = TrainingTriggerEvent::OnHit;
    uint8_t pendingMacroSlot = 0;
    uint16_t pendingMoveIndex = 0;
    uint64_t pendingDueStep = 0;
    bool responseReady = false;
    bool guardCancelWindowOpen = false;
    bool macroIdle = true;
};

struct TrainingTriggerStepResult {
    bool requestMacroStart = false;
    bool requestMoveStart = false;
    uint8_t macroSlotIndex = 0;
    uint16_t moveIndex = 0;
    TrainingTriggerEvent event = TrainingTriggerEvent::OnHit;
    TriggerRequestOutcome outcome = TriggerRequestOutcome::None;
};

class TrainingTriggerController {
public:
    TrainingTriggerController();

    void reset();
    void resetTimeline(uint32_t trainingGeneration,
                       bool preserveObservationCounters = false);

    bool setConfig(TrainingTriggerEvent event,
                   const TrainingTriggerConfig& config);
    TrainingTriggerConfig config(TrainingTriggerEvent event) const;

    TrainingTriggerStepResult advance(
        const CombatFrameSample& sample,
        const CombatContactEvent* contacts,
        std::size_t contactCount,
        bool macroIdle,
        uint8_t validMacroSlotMask,
        uint64_t validMoveMask = 0u,
        bool zeroDelayWakeupMoveReady = true,
        bool wakeupPrebufferInFlight = false);

    const TrainingTriggerStatus& status() const { return status_; }
    const TrainingTriggerDiagnostics& diagnostics() const {
        return diagnostics_;
    }
    bool wakeupPrebufferApproved(uint16_t moveIndex) const {
        return prebufferApproved(TrainingTriggerEvent::OnWakeup, moveIndex);
    }
    bool prebufferApproved(TrainingTriggerEvent event,
                           uint16_t moveIndex) const {
        if (!prebufferEvent(event)) return false;
        const std::size_t index = static_cast<std::size_t>(event);
        return prebufferPrepared_[index]
            && prebufferAccepted_[index]
            && prebufferMoveIndex_[index] == moveIndex;
    }
    /* Techs must never be prebuffered: a motion in the ring can satisfy sub_47CAE0's fixed tech patterns by accident. */
    static constexpr bool prebufferEvent(TrainingTriggerEvent event) {
        return event == TrainingTriggerEvent::OnWakeup
            || event == TrainingTriggerEvent::AfterBlock
            || event == TrainingTriggerEvent::AfterHitstun;
    }

    static constexpr bool prebufferRestoresGuard(TrainingTriggerEvent event) {
        return event == TrainingTriggerEvent::AfterBlock
            || event == TrainingTriggerEvent::AfterHitstun;
    }

    void markEventAnswered(TrainingTriggerEvent event, uint64_t stepSerial);

    uint32_t drainJournal(TrainingTriggerJournalEntry* out, uint32_t max);
    uint32_t journalDropped() const { return journalDropped_; }

private:
    struct PendingRequest {
        bool active = false;
        TrainingTriggerEvent event = TrainingTriggerEvent::OnHit;
        uint8_t macroSlot = 0;
        uint16_t moveIndex = 0;
        uint64_t dueStep = 0;
        uint64_t sequence = 0;
        bool requiresWakeupRelease = false;
    };

    void clearTimelineState(uint32_t trainingGeneration,
                            bool preserveObservationCounters);
    void discardPendingForNewCombatEpisode(uint64_t stepSerial);
    bool schedule(TrainingTriggerEvent event, uint64_t edgeStep,
                  uint64_t sourceToken,
                  bool chanceAlreadyDecided = false,
                  bool chanceAccepted = false);
    TrainingTriggerStepResult serviceDue(uint64_t stepSerial, bool macroIdle,
                                         uint8_t validMacroSlotMask,
                                         uint64_t validMoveMask,
                                         bool moveResponseReady,
                                         bool guardCancelWindowOpen,
                                         bool zeroDelayWakeupMoveReady,
                                         bool wakeupPrebufferInFlight);
    void recordOutcome(TrainingTriggerEvent event, uint8_t macroSlot,
                       uint16_t moveIndex, uint64_t stepSerial,
                       TriggerRequestOutcome outcome);
    void explainStep(const TrainingTriggerStepResult& result,
                     uint64_t stepSerial, bool macroIdle,
                     bool moveResponseReady, bool guardCancelWindowOpen,
                     bool zeroDelayWakeupMoveReady,
                     bool wakeupPrebufferInFlight);
    bool eventHasResponse(TrainingTriggerEvent event) const;
    uint8_t armedEventMask() const;

    TrainingTriggerConfig configs_[kTrainingTriggerEventCount] = {};
    PendingRequest pending_[kTriggerPendingCapacity] = {};
    TrainingTriggerStatus status_{};
    TrainingTriggerDiagnostics diagnostics_{};

    bool timelineEstablished_ = false;
    bool haveSample_ = false;
    uint64_t lastStepSerial_ = 0;
    uint32_t previousDummyAction_ = 0;
    uint64_t previousDummyActorToken_ = 0;
    uint64_t previousDummyActionEpoch_ = 0;
    uint64_t lastContactSerial_ = 0;
    uint64_t nextSequence_ = 1;

    bool blockLatched_ = false;
    bool hitstunLatched_ = false;
    bool wakeupLatched_ = false;
    bool airRecoveryLatched_ = false;
    bool groundRecoveryLatched_ = false;
    bool throwTechLatched_ = false;
    TrainingTriggerJournalEntry journal_[kTrainingTriggerJournalCapacity] = {};
    uint32_t journalCount_ = 0;
    uint32_t journalDropped_ = 0;
    void journal(TrainingTriggerEvent event, TriggerJournalKind kind,
                 TriggerJournalCause cause, uint64_t stepSerial,
                 uint16_t moveIndex = 0u, uint8_t macroSlot = 0u,
                 uint32_t detail = 0u);
    bool prebufferPrepared_[kTrainingTriggerEventCount] = {};
    bool prebufferAccepted_[kTrainingTriggerEventCount] = {};
    uint16_t prebufferMoveIndex_[kTrainingTriggerEventCount] = {};
    void preparePrebuffer(TrainingTriggerEvent event, uint64_t stepSerial);
    void clearPrebuffer(TrainingTriggerEvent event);
    bool guardCancelLatched_ = false;
    uint64_t blockLatchStep_ = 0;
    uint64_t hitstunLatchStep_ = 0;
    uint64_t wakeupLatchStep_ = 0;
    uint64_t airRecoveryLatchStep_ = 0;
    uint64_t groundRecoveryLatchStep_ = 0;
    uint64_t throwTechLatchStep_ = 0;
    uint64_t guardCancelLatchStep_ = 0;
    uint64_t guardCancelContactSerial_ = 0;
};

enum class FrameAdvantageState : uint8_t {
    Idle = 0,
    Measuring = 1,
    Complete = 2,
    Trade = 3,
    TimedOut = 4,
    Unmeasurable = 5,
};

enum class FrameAdvantageReason : uint8_t {
    None = 0,
    Completed = 1,
    Trade = 2,
    TimeoutAttacker = 3,
    TimeoutDefender = 4,
    TimeoutBoth = 5,
    UnreadableSample = 6,
    RoundEnded = 7,
    StepGap = 8,
    NonMonotonicStep = 9,
    AttackerCancelled = 10,
    AttackerInterrupted = 11,
    AttackerActorChanged = 12,
    DefenderActorChanged = 13,
    NonPrimaryVictim = 14,
    DefenderNoLock = 15,
    DefenderPathInvalid = 16,
    DefenderLockLost = 17,
    InvalidContactBatch = 18,
};

struct FrameAdvantageStatus {
    uint32_t trainingGeneration = 0;
    FrameAdvantageState state = FrameAdvantageState::Idle;
    CombatContactKind contactKind = CombatContactKind::None;
    uint8_t attackerOwner = 0;
    uint8_t defenderOwner = 0;
    bool attackerIsPrimary = true;
    bool attackerReady = false;
    bool defenderReady = false;
    bool advantageValid = false;
    int32_t advantageFrames = 0;
    uint64_t contactSerial = 0;
    uint64_t contactStep = 0;
    uint64_t attackerReadyDelay = 0;
    uint64_t defenderReadyDelay = 0;
    uint32_t completedCount = 0;
    uint32_t tradeCount = 0;
    uint32_t timeoutCount = 0;
    uint32_t invalidContactCount = 0;

    FrameAdvantageReason reason = FrameAdvantageReason::None;
    uint32_t unmeasurableCount = 0;
    uint64_t attackerActorToken = 0;
    uint64_t defenderActorToken = 0;
    uint64_t attackerActionEpoch = 0;
    uint64_t defenderActionEpoch = 0;
    bool defenderLockObserved = false;

    uint32_t chainedActions = 0;

    bool gapValid = false;
    uint32_t gapFrames = 0;
    uint32_t gapCount = 0;
};

class FrameAdvantageTracker {
public:
    explicit FrameAdvantageTracker(
        uint64_t timeoutSteps = kFrameAdvantageTimeoutSteps);

    void reset();
    void resetTimeline(uint32_t trainingGeneration,
                       bool preserveObservationCounters = false);
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    void advance(const CombatFrameSample& sample,
                 const CombatContactEvent* contacts,
                 std::size_t contactCount);

    const FrameAdvantageStatus& status() const { return status_; }

private:
    void beginEpisode(const CombatContactEvent& contact,
                      const CombatFrameSample& sample);
    void markTrade(const CombatContactEvent& contact);
    void markUnmeasurable(FrameAdvantageReason reason);
    void replaceWithUnmeasurable(FrameAdvantageReason reason);
    void clearReadout();
    void ageRetainedReadout();
    void invalidateActiveOrClear(FrameAdvantageReason reason);
    void openGapCandidate(uint64_t stepSerial);
    void closeGapCandidate();
    void applyGapToEpisode(const CombatContactEvent& contact,
                           const CombatOwnerSample& attacker,
                           const CombatOwnerSample& defender,
                           bool hadResolvedReadout);
    void ageGapReadout();
    bool validateContactBatch(const CombatFrameSample& sample,
                              const CombatContactEvent* contacts,
                              std::size_t contactCount,
                              uint32_t& invalidCount) const;
    bool identityChanged(uint64_t expected, uint64_t observed) const;

    bool enabled_ = true;
    bool timelineEstablished_ = false;
    bool haveStep_ = false;
    uint64_t lastStepSerial_ = 0;
    uint64_t lastContactSerial_ = 0;
    uint64_t retainedReadoutAgeSteps_ = 0;
    uint64_t timeoutSteps_ = kFrameAdvantageTimeoutSteps;
    FrameAdvantageStatus status_{};

    bool gapCandidateOpen_ = false;
    uint64_t gapOpenStep_ = 0;
    uint8_t gapAttackerOwner_ = 0;
    uint8_t gapDefenderOwner_ = 0;
    uint64_t gapAttackerActorToken_ = 0;
    uint64_t gapDefenderActorToken_ = 0;
    uint64_t gapDisplaySteps_ = 0;
};

}
