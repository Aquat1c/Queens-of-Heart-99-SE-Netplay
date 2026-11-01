#include "training/TrainingResources.h"

namespace qtraining {

uint32_t evaluateFighterNeutral(
    const FighterNeutralObservation& observation) {
    if (!observation.readable) return NeutralFighterUnreadable;

    uint32_t reasons = TrainingNeutralOk;
    if (observation.slot >= kTrainingRefillPlayerCount)
        reasons |= NeutralSlotOutOfRange;
    if (observation.alternateForm != 0)
        reasons |= NeutralAlternateFormActive;
    if (!trainingFreeMovementAction(observation.action))
        reasons |= NeutralActionNotFreeMovement;
    if (!trainingFreeMovementAction(observation.dispatchAction))
        reasons |= NeutralDispatchNotFreeMovement;
    if (observation.initStatus != 0)
        reasons |= NeutralInitNotRunning;
    if (observation.transitionFlag != 0)
        reasons |= NeutralTransitionPending;
    if (observation.actionLockEnd != 0)
        reasons |= NeutralActionLockSet;
    const uint8_t posture = observation.postureNibble & 0x07u;
    if (posture != 1 && posture != 2)
        reasons |= NeutralPostureNotGrounded;
    if ((observation.terminalReady & 1u) != 0)
        reasons |= NeutralTerminalLatched;
    if (observation.roundEnd != 0 || observation.roundLost != 0)
        reasons |= NeutralRoundResultLatched;
    if (observation.fatalCause != -1)
        reasons |= NeutralFatalRecorded;
    if (observation.comOwnership != 0)
        reasons |= NeutralComOwned;
    if (observation.recoveryLock != 0)
        reasons |= NeutralRecoveryLockSet;
    if (observation.recoveryPhaseLatch != 0)
        reasons |= NeutralRecoveryPhaseLatch;
    if (observation.comboHits != 0)
        reasons |= NeutralComboActive;
    if (observation.lifePendingDamage != 0
        || observation.lifeReserve != 0
        || observation.lifeSettleTimer != 0) {
        reasons |= NeutralDamageUnsettled;
    }
    return reasons;
}

uint32_t evaluateNeutralGlobals(
    const NeutralGlobalObservation& observation) {
    if (!observation.readable) return NeutralFighterUnreadable;
    uint32_t reasons = TrainingNeutralOk;
    if (observation.roundResultState != 0)
        reasons |= NeutralRoundResultState;
    if (observation.fatalHitDisableFlag != 0)
        reasons |= NeutralFatalDisableFlag;
    return reasons;
}

void NeutralTrigger::reset() {
    hold_ = 0;
    neutral_ = false;
}

bool NeutralTrigger::advance(bool bothFightersNeutral) {
    neutral_ = bothFightersNeutral;
    if (!bothFightersNeutral) {
        hold_ = 0;
        return false;
    }
    if (hold_ < kTrainingNeutralHoldSteps) ++hold_;
    return hold_ >= kTrainingNeutralHoldSteps;
}

void IdleStateTrigger::reset() {
    hold_ = 0;
}

bool IdleStateTrigger::advance(bool bothFightersIdle) {
    if (!bothFightersIdle) {
        hold_ = 0;
        return false;
    }
    if (hold_ < kTrainingExactIdleHoldSteps) ++hold_;
    return hold_ >= kTrainingExactIdleHoldSteps;
}

void ComboEpisodeMachine::reset() {
    state_ = ComboEpisodeState::Idle;
    settle_ = 0;
    lastDamageSerial_ = 0;
    haveSerial_ = false;
}

bool ComboEpisodeMachine::advance(
    const ComboEpisodeObservation& observation, uint32_t settleSteps) {
    if (!observation.readable) {
        return false;
    }

    const bool newHit = haveSerial_
        && observation.admittedDamageSerial != lastDamageSerial_;
    lastDamageSerial_ = observation.admittedDamageSerial;
    haveSerial_ = true;

    if (newHit || observation.inReactionAction
        || observation.nativeComboHits != 0) {
        if (state_ != ComboEpisodeState::InEpisode) {
            state_ = ComboEpisodeState::InEpisode;
            settle_ = 0;
        }
        return false;
    }

    if (state_ == ComboEpisodeState::Idle) return false;

    if (state_ == ComboEpisodeState::InEpisode) {
        state_ = ComboEpisodeState::Settling;
        settle_ = 0;
    }

    const bool quiet = observation.nativeComboHits == 0
        && observation.damageSettled
        && !observation.inReactionAction
        && observation.recoveryLocksClear;
    if (!quiet) {
        settle_ = 0;
        return false;
    }
    ++settle_;
    if (settle_ < settleSteps) return false;
    state_ = ComboEpisodeState::Idle;
    settle_ = 0;
    return true;
}

void RecoveryExitTrigger::reset() {
    wasInReaction_ = false;
}

bool RecoveryExitTrigger::advance(bool inReactionAction, bool locksClear) {
    const bool exited = wasInReaction_ && !inReactionAction && locksClear;
    wasInReaction_ = inReactionAction;
    return exited;
}

uint32_t resolvePolicyFire(const ResourcePolicySettings& policy,
                           const ResourceTriggerSignals& signals) {
    if (!isValidRefillTarget(policy.target)) return 0;
    const uint32_t configured = refillTargetPlayerMask(policy.target);
    switch (policy.trigger) {
        case ResourceTrigger::Off:
            return 0;
        case ResourceTrigger::OnResetOnly:
            return signals.resetAccepted ? configured : 0;
        case ResourceTrigger::WhenNeutral:
            return (signals.resetAccepted || signals.neutralReady
                    || signals.exactIdleReady)
                ? configured : 0;
        case ResourceTrigger::AfterCombo: {
            uint32_t mask = 0;
            for (uint32_t p = 0; p < kTrainingRefillPlayerCount; ++p) {
                if (signals.comboEpisodeClosed[p]) mask |= 1u << p;
            }
            return mask & configured;
        }
        case ResourceTrigger::AfterRecovery: {
            uint32_t mask = 0;
            for (uint32_t p = 0; p < kTrainingRefillPlayerCount; ++p) {
                if (signals.recoveryExited[p]) mask |= 1u << p;
            }
            return mask & configured;
        }
        default:
            return 0;
    }
}

uint32_t resolveHealthPolicyFire(const ResourcePolicySettings& policy,
                                 const ResourceTriggerSignals& signals) {
    if (policy.trigger != ResourceTrigger::WhenNeutral)
        return resolvePolicyFire(policy, signals);
    if (!isValidRefillTarget(policy.target)) return 0;
    return (signals.resetAccepted || signals.exactIdleReady)
        ? refillTargetPlayerMask(policy.target) : 0;
}

uint32_t evaluateRoundStartNeutral(
    const RoundStartNeutralObservation& observation) {
    if (!observation.readable) return 0;
    uint32_t bits = 0;
    if (observation.action == 0) bits |= RoundStartNeutralAction;
    if (observation.dispatchAction == 0) bits |= RoundStartNeutralDispatch;
    if (observation.initStatus == 0) bits |= RoundStartNeutralInit;
    if (observation.transitionFlag == 0) bits |= RoundStartNeutralNoPending;
    const uint8_t posture = observation.postureNibble & 0x07u;
    if (posture == 1 || posture == 2) bits |= RoundStartNeutralGrounded;
    if ((observation.terminalReady & 1u) == 0)
        bits |= RoundStartNeutralNoTerminal;
    if (observation.wallFlags == 0) bits |= RoundStartNeutralNoWall;
    if (observation.facing == observation.expectedFacing)
        bits |= RoundStartNeutralFacing;
    if (observation.roundEnd == 0 && observation.roundLost == 0)
        bits |= RoundStartNeutralNoResult;
    if (observation.fatalCause == -1) bits |= RoundStartNeutralFatalIdle;
    if (observation.comOwnership == 0) bits |= RoundStartNeutralNoCom;
    if (observation.recoveryLock == 0 && observation.recoveryPhaseLatch == 0)
        bits |= RoundStartNeutralLocksClear;
    if (observation.positionMatchesPlan) bits |= RoundStartNeutralPosition;
    if (observation.framePointerInRange) bits |= RoundStartNeutralFrameOk;
    return bits;
}

}
