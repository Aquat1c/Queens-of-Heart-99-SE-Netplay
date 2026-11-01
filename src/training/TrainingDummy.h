#pragma once

#include <cstdint>

#include "input/InputActions.h"
#include "training/TrainingTypes.h"

namespace qtraining {

constexpr bool guardModeReleaseQualified(
    GuardMode mode, bool autoPredictionQualified) {
    switch (mode) {
        case GuardMode::Off:
        case GuardMode::AlwaysStand:
        case GuardMode::AlwaysCrouch:
        case GuardMode::AutoHeuristic:
        case GuardMode::AlwaysAir:
        case GuardMode::AutoRecord:
            return true;
        case GuardMode::Auto:
            return autoPredictionQualified;
        default:
            return false;
    }
}

constexpr uint32_t kDummyHorizontalAxis =
    qinput::kLogicalLeft | qinput::kLogicalRight;
constexpr uint32_t kDummyVerticalAxis =
    qinput::kLogicalUp | qinput::kLogicalDown;

constexpr bool isCanonicalDummyOwnedAxes(uint32_t axes) {
    return (axes & ~qinput::kLogicalDirectionMask) == 0
        && ((axes & kDummyHorizontalAxis) == 0
            || (axes & kDummyHorizontalAxis) == kDummyHorizontalAxis)
        && ((axes & kDummyVerticalAxis) == 0
            || (axes & kDummyVerticalAxis) == kDummyVerticalAxis);
}

// Button ownership is all four or none: sub_483450 requires the button ring head to be exactly 4 (C alone).
constexpr bool isCanonicalDummyOwnedButtons(uint32_t buttons) {
    return buttons == 0u || buttons == qinput::kLogicalButtonMask;
}

constexpr bool dummyStanceValid(DummyStance stance) {
    switch (stance) {
        case DummyStance::Stand:
        case DummyStance::Crouch:
        case DummyStance::Jump:
            return true;
        default:
            return false;
    }
}

constexpr DummyStance nextDummyStance(DummyStance stance) {
    switch (stance) {
        case DummyStance::Stand: return DummyStance::Crouch;
        case DummyStance::Crouch: return DummyStance::Jump;
        case DummyStance::Jump:
        default: return DummyStance::Stand;
    }
}

constexpr GuardMode nextReleasedGuardMode(GuardMode mode) {
    const uint32_t last = static_cast<uint32_t>(GuardMode::AutoRecord);
    uint32_t candidate = static_cast<uint32_t>(mode);
    for (uint32_t step = 0; step <= last; ++step) {
        candidate = candidate >= last ? 0u : candidate + 1u;
        if (guardModeReleaseQualified(static_cast<GuardMode>(candidate), false))
            return static_cast<GuardMode>(candidate);
    }
    return GuardMode::Off;
}

// HoldGuard is withdrawn through this gate; its enum value is frozen and must never be renumbered or reused.
constexpr bool dummyTechReleaseQualified(
    DummyTech mode, bool knockdownDetectionQualified) {
    switch (mode) {
        case DummyTech::Off:
            return true;
        case DummyTech::HoldGuard:
        default:
            return knockdownDetectionQualified;
    }
}

constexpr bool dummyTechValid(DummyTech mode) {
    switch (mode) {
        case DummyTech::Off:
        case DummyTech::HoldGuard:
            return true;
        default:
            return false;
    }
}

enum class DummyPostureSource : uint32_t {
    None = 0,
    Guard = 1,
    Tech = 2,
    Stance = 3,
    // Never renumber this value; it outranks all three above and claims both axes.
    Recovery = 4,
};

struct DummyPostureInput {
    GuardMode guardMode = GuardMode::Off;
    DummyStance stance = DummyStance::Stand;
    DummyTech tech = DummyTech::Off;
    bool guarding = false;
    uint32_t guardDirections = 0;
    uint8_t defenderFacing = 0;

    // The ring stores raw & 0xF0000 and matcher modes 0 and 1 compare exact DWORDs, so recovery owns both axes.
    bool recoveryActive = false;
    uint32_t recoveryDirections = 0;
    uint32_t recoveryButtons = 0;
    bool recoveryOwnsButtons = false;
};

struct DummyPostureResult {
    bool valid = true;
    uint32_t directions = 0;
    uint32_t ownedAxes = 0;
    uint32_t buttons = 0;
    uint32_t ownedButtons = 0;
    DummyPostureSource horizontalSource = DummyPostureSource::None;
    DummyPostureSource verticalSource = DummyPostureSource::None;
};

DummyPostureResult chooseDummyPosture(const DummyPostureInput& input);

struct GuardDecisionInput {
    GuardMode mode = GuardMode::Off;
    bool threatPresent = false;
    bool defenderAirborne = false;
    bool autoPredictionQualified = false;
    uint8_t permission = 0;
    uint8_t defenderFacing = 0;

    PostureClass defenderPosture = PostureClass::Unreadable;
    bool defenderInTechResult = false;
    uint32_t attackerReadableCount = 0;
    uint32_t attackerGroundedCount = 0;

    bool recordEvidenceValid = false;
    uint32_t activeRecordCount = 0;
    uint8_t activeLegalPostureMask = 0;

    // Type-15 capture: captured only when (rawRecordByte17 & victimFullByte156) == 0 (sub_464AA0). Never fold into the ordinary mask.
    uint32_t captureRecordCount = 0;
    uint8_t captureAvoidPostureMask = 0;
};

struct GuardDecision {
    bool valid = true;
    bool guarding = false;
    uint32_t logicalDirections = 0;
    GuardClassification classification =
        GuardClassification::Unguardable;
    bool postureDivergedFromY = false;
    bool attackerPostureAmbiguous = false;
    bool defenderPostureUnguardable = false;
    bool recordMixupUnblockable = false;
    bool recordForcedStandGuard = false;
    bool captureAvoidanceApplied = false;
    bool recordEvidenceMissing = false;
    uint8_t legalPostureMask = 0;
    uint32_t activeRecordCount = 0;
    uint32_t captureRecordCount = 0;
};

GuardClassification classifyGuardPermission(uint8_t permission);
GuardDecision chooseGuardDirections(const GuardDecisionInput& input);

constexpr uint32_t kGuardPredictionCapacity = 32;

enum class GuardContactResult : uint32_t {
    Match = 0,
    PermissionMismatch = 1,
    DirectionMismatch = 2,
    MissingPrediction = 3,
    Duplicate = 4,
    Invalid = 5,
};

struct GuardTelemetryCounters {
    uint32_t predictions = 0;
    uint32_t predictionUpdates = 0;
    uint32_t predictionEvictions = 0;
    uint32_t contacts = 0;
    uint32_t matches = 0;
    uint32_t permissionMismatches = 0;
    uint32_t directionMismatches = 0;
    uint32_t missingPredictions = 0;
    uint32_t firstActiveMisses = 0;
    uint32_t duplicateContacts = 0;
    uint32_t invalidObservations = 0;
};

class GuardPredictionTelemetry {
public:
    void reset();

    bool recordPrediction(uint64_t threatKey,
                          uint64_t predictedContactStep,
                          uint8_t predictedPermission,
                          uint32_t predictedDirections);

    GuardContactResult recordExactContact(uint64_t threatKey,
                                          uint64_t contactStep,
                                          uint8_t exactPermission,
                                          uint32_t actualDirections,
                                          bool firstActive);

    const GuardTelemetryCounters& counters() const { return counters_; }

private:
    struct Prediction {
        uint64_t threatKey = 0;
        uint64_t contactStep = 0;
        uint32_t predictedDirections = 0;
        uint8_t permission = 0;
        bool valid = false;
        bool consumed = false;
    };

    Prediction predictions_[kGuardPredictionCapacity] = {};
    uint32_t replacementCursor_ = 0;
    GuardTelemetryCounters counters_{};
};

}
