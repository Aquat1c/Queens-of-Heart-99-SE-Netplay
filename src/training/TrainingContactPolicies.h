#pragma once

#include <cstdint>

namespace qtraining {

/* Critical divisor uses incoming = dword_6265D0[11 * defenderSlot] + 1: incoming <= 8 ? 32 : (incoming <= 16 ? 8 : 4). */
constexpr uint32_t kCriticalDivisorEarly = 32u;
constexpr uint32_t kCriticalDivisorMid = 8u;
constexpr uint32_t kCriticalDivisorLate = 4u;

constexpr uint32_t criticalDivisorForIncomingHit(uint32_t incomingHit) {
    if (incomingHit <= 8u) return kCriticalDivisorEarly;
    if (incomingHit <= 16u) return kCriticalDivisorMid;
    return kCriticalDivisorLate;
}

enum class TrainingCriticalPolicy : uint32_t {
    Native = 0,

    AlwaysSingle = 1,

    NeverRandom = 2,

    NeverAny = 3,

    ForceRandomRoll = 4,

    Count = 5,
};

/* The native draw must never be skipped: the seed has to advance identically under every verdict or the stream desyncs. */
enum class CriticalRollOverride : uint32_t {
    Native = 0,
    ForceSuccess = 1,
    ForceFailure = 2,
};

constexpr uint32_t kCriticalForcedSuccessSample = 0u;
constexpr uint32_t kCriticalForcedFailureSample = 1u;

constexpr uint32_t criticalSampleForOverride(CriticalRollOverride override,
                                             uint32_t nativeSample) {
    switch (override) {
        case CriticalRollOverride::ForceSuccess:
            return kCriticalForcedSuccessSample;
        case CriticalRollOverride::ForceFailure:
            return kCriticalForcedFailureSample;
        case CriticalRollOverride::Native:
        default:
            return nativeSample;
    }
}

/* guardCrushLatch is dword_6265C4[11 * defenderSlot] sampled BEFORE the original scaler runs, because the scaler consumes it. */
constexpr CriticalRollOverride criticalRollOverrideFor(
    TrainingCriticalPolicy policy, bool guardCrushLatch) {
    switch (policy) {
        case TrainingCriticalPolicy::AlwaysSingle:
            return guardCrushLatch ? CriticalRollOverride::ForceFailure
                                   : CriticalRollOverride::ForceSuccess;
        case TrainingCriticalPolicy::NeverRandom:
        case TrainingCriticalPolicy::NeverAny:
            return CriticalRollOverride::ForceFailure;
        case TrainingCriticalPolicy::ForceRandomRoll:
            return CriticalRollOverride::ForceSuccess;
        case TrainingCriticalPolicy::Native:
        default:
            return CriticalRollOverride::Native;
    }
}

constexpr bool criticalPolicyClearsGuardCrushLatch(
    TrainingCriticalPolicy policy, bool guardCrushLatch) {
    return policy == TrainingCriticalPolicy::NeverAny && guardCrushLatch;
}

constexpr bool criticalPolicyValid(uint32_t value) {
    return value < static_cast<uint32_t>(TrainingCriticalPolicy::Count);
}

// sub_47D510 mode 1 verdict: 2 = 70% damage, 1 = 90%, 0 = full. Mode 0 is the struggle check and must pass through untouched.
enum class TrainingReducePolicy : uint32_t {
    Native = 0,
    AlwaysStrong = 1,
    AlwaysLight = 2,
    Never = 3,
    Count = 4,
};

constexpr bool reducePolicyValid(uint32_t value) {
    return value < static_cast<uint32_t>(TrainingReducePolicy::Count);
}

/* The verdict handed back to the hit resolver: 2 = 70%, 1 = 90%, 0 = full. */
constexpr int reduceVerdictForPolicy(TrainingReducePolicy policy,
                                     int nativeVerdict) {
    switch (policy) {
        case TrainingReducePolicy::AlwaysStrong:
            return 2;
        case TrainingReducePolicy::AlwaysLight:
            return 1;
        case TrainingReducePolicy::Never:
            return 0;
        case TrainingReducePolicy::Native:
        default:
            return nativeVerdict;
    }
}

enum class TrainingFaultlessPolicy : uint32_t {
    NativeInput = 0,
    AlwaysWhenLegal = 1,
    Never = 2,
    Count = 3,
};

/* Detector family mode: {4, 34, 1, 35, 24} -> 3; {16, 27} -> 2; other listed -> 1; unsupported -> 0. */
constexpr uint32_t faultlessFamilyModeForActor(uint32_t actorId) {
    switch (actorId) {
        case 4u:
        case 34u:
        case 1u:
        case 35u:
        case 24u:
            return 3u;
        case 16u:
        case 27u:
            return 2u;
        case 10u:
        case 2u:
            return 1u;
        default:
            return 0u;
    }
}

/* Ids 34 and 35 are partner actors and must stay in the set: they take the contact for Kaede and Mizuho. */
constexpr bool faultlessSupportedActor(uint32_t actorId) {
    switch (actorId) {
        case 10u: case 2u: case 1u: case 35u: case 24u:
        case 4u: case 34u: case 16u: case 27u:
            return true;
        default:
            return false;
    }
}

/* Native meter gate: strictly greater than 6 fractional units, then 6 are spent. */
constexpr uint32_t kFaultlessMeterRequirement = 6u;

constexpr bool faultlessMeterSufficient(uint32_t fractionalMeter) {
    return fractionalMeter > kFaultlessMeterRequirement;
}

constexpr bool faultlessPolicyValid(uint32_t value) {
    return value < static_cast<uint32_t>(TrainingFaultlessPolicy::Count);
}

enum class TrainingParryPolicy : uint32_t {
    NativeInput = 0,
    AlwaysWhenLegal = 1,
    Never = 2,
    Count = 3,
};

/* Saori is id 0; Rapid Ayaka (31) is in the native set. */
constexpr bool parrySupportedActor(uint32_t actorId) {
    switch (actorId) {
        case 12u:
        case 28u:
        case 5u:
        case 0u:
        case 31u:
        case 19u:
            return true;
        default:
            return false;
    }
}

constexpr bool parryPostureEligible(uint32_t postureByte) {
    return (postureByte & 8u) == 8u && (postureByte & 7u) == 1u;
}

constexpr uint32_t kParryExcludedAttackerActor = 32u;
constexpr uint32_t kParryExcludedAttackerAction = 6u;

constexpr bool parryAttackerExcluded(uint32_t attackerActor,
                                     uint32_t attackerAction) {
    return attackerActor == kParryExcludedAttackerActor
        && attackerAction == kParryExcludedAttackerAction;
}

/* The attack record's stand-guard/parry permission bit: (+17 & 1) == 1. */
constexpr bool parryAttackPermits(uint32_t attackRecordByte17) {
    return (attackRecordByte17 & 1u) == 1u;
}

/* A parry return of 2 requires these two writes as well: AlwaysWhenLegal must reproduce them. */
constexpr uint32_t kParryMatcherAcceptedResult = 2u;
constexpr uint32_t kParryMatcherActionRequest = 18u;
constexpr uint32_t kParryMatcherPendingFlag = 1u;

constexpr bool parryPolicyValid(uint32_t value) {
    return value < static_cast<uint32_t>(TrainingParryPolicy::Count);
}

/* Native resolver tries parry first, then the guarded path holding Faultless; do not reorder. */
enum class ContactResolution : uint32_t {
    None = 0,
    Parry = 1,
    Faultless = 2,
};

struct ContactSituation {
    uint32_t defenderActor = 0u;
    uint32_t attackerActor = 0u;
    uint32_t attackerAction = 0u;
    uint32_t defenderPosture = 0u;
    uint32_t attackRecordByte17 = 0u;
    uint32_t defenderFractionalMeter = 0u;
    bool guardedContact = false;
};

constexpr bool parryWouldTake(const ContactSituation& s,
                              TrainingParryPolicy policy) {
    return policy == TrainingParryPolicy::AlwaysWhenLegal
        && parrySupportedActor(s.defenderActor)
        && parryPostureEligible(s.defenderPosture)
        && !parryAttackerExcluded(s.attackerActor, s.attackerAction)
        && parryAttackPermits(s.attackRecordByte17);
}

constexpr bool faultlessWouldTake(const ContactSituation& s,
                                  TrainingFaultlessPolicy policy) {
    return policy == TrainingFaultlessPolicy::AlwaysWhenLegal
        && s.guardedContact
        && faultlessSupportedActor(s.defenderActor)
        && faultlessMeterSufficient(s.defenderFractionalMeter);
}

constexpr ContactResolution resolveContactPrecedence(
    const ContactSituation& s, TrainingParryPolicy parry,
    TrainingFaultlessPolicy faultless) {
    if (parryWouldTake(s, parry)) return ContactResolution::Parry;
    if (faultlessWouldTake(s, faultless)) return ContactResolution::Faultless;
    return ContactResolution::None;
}

struct TrainingContactPolicySet {
    TrainingFaultlessPolicy faultless = TrainingFaultlessPolicy::NativeInput;
    TrainingParryPolicy parry = TrainingParryPolicy::NativeInput;
    TrainingCriticalPolicy criticalP1 = TrainingCriticalPolicy::Native;
    TrainingCriticalPolicy criticalP2 = TrainingCriticalPolicy::Native;
    TrainingReducePolicy reduceP1 = TrainingReducePolicy::Native;
    TrainingReducePolicy reduceP2 = TrainingReducePolicy::Native;

    constexpr bool allNative() const {
        return faultless == TrainingFaultlessPolicy::NativeInput
            && parry == TrainingParryPolicy::NativeInput
            && criticalP1 == TrainingCriticalPolicy::Native
            && criticalP2 == TrainingCriticalPolicy::Native
            && reduceP1 == TrainingReducePolicy::Native
            && reduceP2 == TrainingReducePolicy::Native;
    }
};

constexpr TrainingCriticalPolicy criticalPolicyForAttackerSlot(
    const TrainingContactPolicySet& set, uint32_t attackerSlot) {
    return attackerSlot == 0u ? set.criticalP1 : set.criticalP2;
}

constexpr TrainingReducePolicy reducePolicyForVictimSlot(
    const TrainingContactPolicySet& set, uint32_t victimSlot) {
    return victimSlot == 0u ? set.reduceP1 : set.reduceP2;
}

enum class TrainingContactPolicySlot : uint32_t {
    FaultlessDummy = 0,
    ParryDummy = 1,
    CriticalP1Attacks = 2,
    CriticalP2Attacks = 3,
    ReduceP1Hit = 4,
    ReduceP2Hit = 5,
    Count = 6,
};

constexpr bool contactPolicyAssignmentValid(uint32_t slot, uint32_t value) {
    switch (static_cast<TrainingContactPolicySlot>(slot)) {
        case TrainingContactPolicySlot::FaultlessDummy:
            return faultlessPolicyValid(value);
        case TrainingContactPolicySlot::ParryDummy:
            return parryPolicyValid(value);
        case TrainingContactPolicySlot::CriticalP1Attacks:
        case TrainingContactPolicySlot::CriticalP2Attacks:
            return criticalPolicyValid(value);
        case TrainingContactPolicySlot::ReduceP1Hit:
        case TrainingContactPolicySlot::ReduceP2Hit:
            return reducePolicyValid(value);
        default:
            return false;
    }
}

constexpr bool applyContactPolicyAssignment(TrainingContactPolicySet& set,
                                            uint32_t slot, uint32_t value) {
    if (!contactPolicyAssignmentValid(slot, value)) return false;
    switch (static_cast<TrainingContactPolicySlot>(slot)) {
        case TrainingContactPolicySlot::FaultlessDummy:
            set.faultless = static_cast<TrainingFaultlessPolicy>(value);
            return true;
        case TrainingContactPolicySlot::ParryDummy:
            set.parry = static_cast<TrainingParryPolicy>(value);
            return true;
        case TrainingContactPolicySlot::CriticalP1Attacks:
            set.criticalP1 = static_cast<TrainingCriticalPolicy>(value);
            return true;
        case TrainingContactPolicySlot::CriticalP2Attacks:
            set.criticalP2 = static_cast<TrainingCriticalPolicy>(value);
            return true;
        case TrainingContactPolicySlot::ReduceP1Hit:
            set.reduceP1 = static_cast<TrainingReducePolicy>(value);
            return true;
        case TrainingContactPolicySlot::ReduceP2Hit:
            set.reduceP2 = static_cast<TrainingReducePolicy>(value);
            return true;
        default:
            return false;
    }
}

}
