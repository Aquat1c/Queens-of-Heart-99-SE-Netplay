#include "training/TrainingDummy.h"

namespace qtraining {

GuardClassification classifyGuardPermission(uint8_t permission) {
    if (permission & ~kGuardPermissionMask)
        return GuardClassification::Invalid;
    const uint8_t grounded =
        permission & (kGuardPermitStand | kGuardPermitCrouch);
    if (grounded
        == (kGuardPermitStand | kGuardPermitCrouch)) {
        return GuardClassification::Either;
    }
    if (grounded & kGuardPermitStand)
        return GuardClassification::Stand;
    if (grounded & kGuardPermitCrouch)
        return GuardClassification::Crouch;
    if (permission & kGuardPermitAir)
        return GuardClassification::Air;
    return GuardClassification::Unguardable;
}

DummyPostureResult chooseDummyPosture(const DummyPostureInput& input) {
    DummyPostureResult result{};
    if (input.defenderFacing > 1
        || !guardModeReleaseQualified(input.guardMode, true)
        || !dummyStanceValid(input.stance)
        || !dummyTechValid(input.tech)
        || (input.guardDirections & ~qinput::kLogicalDirectionMask) != 0
        || (input.guarding && input.guardDirections == 0)
        || (input.recoveryDirections & ~qinput::kLogicalDirectionMask) != 0
        || (input.recoveryButtons & ~qinput::kLogicalButtonMask) != 0
        || (!input.recoveryOwnsButtons && input.recoveryButtons != 0)) {
        result.valid = false;
        return result;
    }

    // Recovery takes the whole mask: the native matcher tests exact DWORD equality on the direction nibble.
    if (input.recoveryActive) {
        result.directions = input.recoveryDirections;
        result.ownedAxes = kDummyHorizontalAxis | kDummyVerticalAxis;
        result.horizontalSource = DummyPostureSource::Recovery;
        result.verticalSource = DummyPostureSource::Recovery;
        if (input.recoveryOwnsButtons) {
            result.buttons = input.recoveryButtons;
            result.ownedButtons = qinput::kLogicalButtonMask;
        }
        if ((result.directions & ~result.ownedAxes) != 0
            || (result.buttons & ~result.ownedButtons) != 0
            || !isCanonicalDummyOwnedAxes(result.ownedAxes)
            || !isCanonicalDummyOwnedButtons(result.ownedButtons)) {
            result = DummyPostureResult{};
            result.valid = false;
        }
        return result;
    }

    uint32_t directions = 0;
    uint32_t owned = 0;

    if (input.guarding) {
        directions |= input.guardDirections & kDummyHorizontalAxis;
        owned |= kDummyHorizontalAxis;
        result.horizontalSource = DummyPostureSource::Guard;
        directions |= input.guardDirections & kDummyVerticalAxis;
        owned |= kDummyVerticalAxis;
        result.verticalSource = DummyPostureSource::Guard;
    } else if (input.stance == DummyStance::Crouch) {
        directions |= qinput::kLogicalDown;
        owned |= kDummyVerticalAxis;
        result.verticalSource = DummyPostureSource::Stance;
    } else if (input.stance == DummyStance::Jump) {
        directions |= qinput::kLogicalUp;
        owned |= kDummyVerticalAxis;
        result.verticalSource = DummyPostureSource::Stance;
    }

    result.directions = directions;
    result.ownedAxes = owned;
    if ((result.directions & ~result.ownedAxes) != 0
        || (result.directions & ~qinput::kLogicalDirectionMask) != 0
        || !isCanonicalDummyOwnedAxes(result.ownedAxes)) {
        result = DummyPostureResult{};
        result.valid = false;
    }
    return result;
}

namespace {
GuardDecision chooseGuardDirectionsFor(const GuardDecisionInput& input);
}

GuardDecision chooseGuardDirections(const GuardDecisionInput& input) {
    if (!input.defenderInTechResult
        || input.defenderPosture == PostureClass::Unreadable
        || postureClassGrounded(input.defenderPosture)) {
        return chooseGuardDirectionsFor(input);
    }
    GuardDecisionInput landing = input;
    landing.defenderPosture = PostureClass::Standing;
    landing.defenderAirborne = false;
    return chooseGuardDirectionsFor(landing);
}

namespace {
GuardDecision chooseGuardDirectionsFor(const GuardDecisionInput& input) {
    GuardDecision decision{};
    decision.classification =
        classifyGuardPermission(input.permission);
    if (input.defenderFacing > 1
        || decision.classification == GuardClassification::Invalid) {
        decision.valid = false;
        return decision;
    }
    const uint32_t away = input.defenderFacing == 0
        ? qinput::kLogicalLeft : qinput::kLogicalRight;
    const bool defenderPostureAir = postureClassAirborne(input.defenderPosture);

    switch (input.mode) {
        case GuardMode::Off:
            return decision;
        case GuardMode::AlwaysStand:
            decision.guarding = true;
            decision.logicalDirections = away;
            return decision;
        case GuardMode::AlwaysCrouch:
            decision.guarding = true;
            decision.logicalDirections = defenderPostureAir
                ? away
                : (away | qinput::kLogicalDown);
            return decision;
        case GuardMode::AlwaysAir:
            decision.guarding = true;
            decision.logicalDirections = away | qinput::kLogicalUp;
            return decision;
        case GuardMode::AutoHeuristic: {
            decision.postureDivergedFromY =
                (input.defenderPosture == PostureClass::Air)
                != input.defenderAirborne;
            if (input.defenderPosture == PostureClass::Unreadable) {
                decision.valid = false;
                return decision;
            }
            decision.attackerPostureAmbiguous =
                input.attackerReadableCount > 1
                && input.attackerGroundedCount != 0
                && input.attackerGroundedCount != input.attackerReadableCount;
            decision.defenderPostureUnguardable =
                input.defenderPosture == PostureClass::NotGuardable;
            decision.guarding = true;
            if (!postureClassGrounded(input.defenderPosture)) {
                decision.logicalDirections = away;
                return decision;
            }
            const bool everyAttackerAirborne =
                input.attackerReadableCount > 0
                && input.attackerGroundedCount == 0;
            decision.logicalDirections = everyAttackerAirborne
                ? away
                : (away | qinput::kLogicalDown);
            return decision;
        }
        case GuardMode::AutoRecord: {
            decision.postureDivergedFromY =
                (input.defenderPosture == PostureClass::Air)
                != input.defenderAirborne;
            if (input.defenderPosture == PostureClass::Unreadable) {
                decision.valid = false;
                return decision;
            }
            decision.defenderPostureUnguardable =
                input.defenderPosture == PostureClass::NotGuardable;
            decision.guarding = true;

            if (!postureClassGrounded(input.defenderPosture)) {
                decision.logicalDirections = away;
                return decision;
            }

            if (!input.recordEvidenceValid) {
                decision.recordEvidenceMissing = true;
                decision.logicalDirections = away | qinput::kLogicalDown;
                return decision;
            }
            if (input.activeRecordCount == 0
                && input.captureRecordCount == 0) {
                decision.logicalDirections = away | qinput::kLogicalDown;
                return decision;
            }
            uint8_t legal = kGuardPermissionMask;
            if (input.activeRecordCount != 0) {
                legal = static_cast<uint8_t>(
                    legal & input.activeLegalPostureMask);
            }
            if (input.captureRecordCount != 0) {
                legal = static_cast<uint8_t>(
                    legal & input.captureAvoidPostureMask);
                decision.captureAvoidanceApplied = true;
            }
            legal = static_cast<uint8_t>(legal & kGuardPermissionMask);
            decision.legalPostureMask = legal;
            decision.activeRecordCount = input.activeRecordCount;
            decision.captureRecordCount = input.captureRecordCount;
            if (legal == 0u) {
                decision.recordMixupUnblockable = true;
                decision.logicalDirections = away | qinput::kLogicalDown;
                return decision;
            }
            if (legal & kGuardPermitCrouch) {
                decision.logicalDirections = away | qinput::kLogicalDown;
                return decision;
            }
            if (legal & kGuardPermitStand) {
                decision.recordForcedStandGuard = true;
                decision.logicalDirections = away;
                return decision;
            }
            decision.recordMixupUnblockable = true;
            decision.logicalDirections = away | qinput::kLogicalDown;
            return decision;
        }
        case GuardMode::Auto:
            break;
        default:
            decision.valid = false;
            return decision;
    }

    if (!input.autoPredictionQualified || !input.threatPresent)
        return decision;
    if (input.defenderAirborne) {
        if (input.permission & kGuardPermitAir) {
            decision.guarding = true;
            decision.logicalDirections = away;
        }
        return decision;
    }
    if (input.permission & kGuardPermitCrouch) {
        decision.guarding = true;
        decision.logicalDirections = away | qinput::kLogicalDown;
    } else if (input.permission & kGuardPermitStand) {
        decision.guarding = true;
        decision.logicalDirections = away;
    }
    return decision;
}
}

void GuardPredictionTelemetry::reset() {
    for (Prediction& prediction : predictions_)
        prediction = Prediction{};
    replacementCursor_ = 0;
    counters_ = GuardTelemetryCounters{};
}

bool GuardPredictionTelemetry::recordPrediction(
    uint64_t threatKey, uint64_t predictedContactStep,
    uint8_t predictedPermission, uint32_t predictedDirections) {
    if (threatKey == 0
        || (predictedPermission & ~kGuardPermissionMask) != 0
        || (predictedDirections & ~qinput::kLogicalDirectionMask) != 0) {
        ++counters_.invalidObservations;
        return false;
    }

    for (Prediction& prediction : predictions_) {
        if (prediction.valid
            && prediction.threatKey == threatKey
            && prediction.contactStep == predictedContactStep) {
            prediction.permission = predictedPermission;
            prediction.predictedDirections = predictedDirections;
            prediction.consumed = false;
            ++counters_.predictionUpdates;
            return true;
        }
    }

    uint32_t chosen = kGuardPredictionCapacity;
    for (uint32_t i = 0; i < kGuardPredictionCapacity; ++i) {
        const uint32_t index =
            (replacementCursor_ + i) % kGuardPredictionCapacity;
        if (!predictions_[index].valid
            || predictions_[index].consumed) {
            chosen = index;
            break;
        }
    }
    if (chosen == kGuardPredictionCapacity) {
        chosen = replacementCursor_;
        if (predictions_[chosen].valid
            && !predictions_[chosen].consumed) {
            ++counters_.predictionEvictions;
        }
    }
    replacementCursor_ = (chosen + 1) % kGuardPredictionCapacity;
    Prediction& prediction = predictions_[chosen];
    prediction.threatKey = threatKey;
    prediction.contactStep = predictedContactStep;
    prediction.predictedDirections = predictedDirections;
    prediction.permission = predictedPermission;
    prediction.valid = true;
    prediction.consumed = false;
    ++counters_.predictions;
    return true;
}

GuardContactResult GuardPredictionTelemetry::recordExactContact(
    uint64_t threatKey, uint64_t contactStep, uint8_t exactPermission,
    uint32_t actualDirections, bool firstActive) {
    if (threatKey == 0
        || (exactPermission & ~kGuardPermissionMask) != 0
        || (actualDirections & ~qinput::kLogicalDirectionMask) != 0) {
        ++counters_.invalidObservations;
        return GuardContactResult::Invalid;
    }
    ++counters_.contacts;
    for (Prediction& prediction : predictions_) {
        if (!prediction.valid
            || prediction.threatKey != threatKey
            || prediction.contactStep != contactStep) {
            continue;
        }
        if (prediction.consumed) {
            ++counters_.duplicateContacts;
            return GuardContactResult::Duplicate;
        }
        prediction.consumed = true;
        if (prediction.permission == exactPermission) {
            if (prediction.predictedDirections == actualDirections) {
                ++counters_.matches;
                return GuardContactResult::Match;
            }
            ++counters_.directionMismatches;
            if (firstActive) ++counters_.firstActiveMisses;
            return GuardContactResult::DirectionMismatch;
        }
        ++counters_.permissionMismatches;
        if (firstActive) ++counters_.firstActiveMisses;
        return GuardContactResult::PermissionMismatch;
    }
    ++counters_.missingPredictions;
    if (firstActive) ++counters_.firstActiveMisses;
    return GuardContactResult::MissingPrediction;
}

}
