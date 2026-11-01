#include "netplay/tests/TestHarness.h"

#include "training/TrainingContactPolicies.h"

#include <cstdint>

using namespace qtraining;

namespace {

constexpr uint32_t kSelectableActors[] = {
    0u,
    1u,
    2u,
    4u,
    5u,
    10u,
    12u,
    16u,
    19u,
    24u,
    27u,
    28u,
    31u,
};

bool contains(const uint32_t* set, size_t count, uint32_t value) {
    for (size_t i = 0; i < count; ++i) {
        if (set[i] == value) return true;
    }
    return false;
}

}

QNP_TEST(TrainingCriticalDivisorMatchesTheNativeLadder) {
    QNP_CHECK(criticalDivisorForIncomingHit(1u) == 32u);
    QNP_CHECK(criticalDivisorForIncomingHit(8u) == 32u);
    QNP_CHECK(criticalDivisorForIncomingHit(9u) == 8u);
    QNP_CHECK(criticalDivisorForIncomingHit(16u) == 8u);
    QNP_CHECK(criticalDivisorForIncomingHit(17u) == 4u);
    QNP_CHECK(criticalDivisorForIncomingHit(9999u) == 4u);

    QNP_CHECK(kCriticalDivisorEarly == 32u);
    QNP_CHECK(kCriticalDivisorMid == 8u);
    QNP_CHECK(kCriticalDivisorLate == 4u);
}

QNP_TEST(TrainingCriticalForcedSamplesWorkForEveryNativeDivisor) {
    const uint32_t divisors[] = {kCriticalDivisorEarly, kCriticalDivisorMid,
                                 kCriticalDivisorLate};
    for (const uint32_t divisor : divisors) {
        QNP_CHECK(kCriticalForcedSuccessSample % divisor == 0u);
        QNP_CHECK(kCriticalForcedFailureSample % divisor != 0u);
    }
}

QNP_TEST(TrainingCriticalPolicyTruthTableIsExhaustive) {
    struct Row {
        TrainingCriticalPolicy policy;
        bool latch;
        CriticalRollOverride expected;
        bool clearsLatch;
    };
    const Row rows[] = {
        {TrainingCriticalPolicy::Native, false,
         CriticalRollOverride::Native, false},
        {TrainingCriticalPolicy::Native, true,
         CriticalRollOverride::Native, false},

        {TrainingCriticalPolicy::AlwaysSingle, false,
         CriticalRollOverride::ForceSuccess, false},
        {TrainingCriticalPolicy::AlwaysSingle, true,
         CriticalRollOverride::ForceFailure, false},

        {TrainingCriticalPolicy::NeverRandom, false,
         CriticalRollOverride::ForceFailure, false},
        {TrainingCriticalPolicy::NeverRandom, true,
         CriticalRollOverride::ForceFailure, false},

        {TrainingCriticalPolicy::NeverAny, false,
         CriticalRollOverride::ForceFailure, false},
        {TrainingCriticalPolicy::NeverAny, true,
         CriticalRollOverride::ForceFailure, true},

        {TrainingCriticalPolicy::ForceRandomRoll, false,
         CriticalRollOverride::ForceSuccess, false},
        {TrainingCriticalPolicy::ForceRandomRoll, true,
         CriticalRollOverride::ForceSuccess, false},
    };
    for (const Row& row : rows) {
        QNP_CHECK(criticalRollOverrideFor(row.policy, row.latch)
                  == row.expected);
        QNP_CHECK(criticalPolicyClearsGuardCrushLatch(row.policy, row.latch)
                  == row.clearsLatch);
    }

    for (uint32_t i = 0u;
         i < static_cast<uint32_t>(TrainingCriticalPolicy::Count); ++i) {
        const auto policy = static_cast<TrainingCriticalPolicy>(i);
        if (policy == TrainingCriticalPolicy::NeverAny) continue;
        QNP_CHECK(!criticalPolicyClearsGuardCrushLatch(policy, true));
        QNP_CHECK(!criticalPolicyClearsGuardCrushLatch(policy, false));
    }
}

QNP_TEST(TrainingCriticalOverrideAlwaysConsumesTheNativeSample) {
    const uint32_t samples[] = {0u, 1u, 7u, 32u, 33u, 4294967295u};
    for (const uint32_t sample : samples) {
        QNP_CHECK(criticalSampleForOverride(CriticalRollOverride::Native,
                                            sample) == sample);
        QNP_CHECK(criticalSampleForOverride(CriticalRollOverride::ForceSuccess,
                                            sample)
                  == kCriticalForcedSuccessSample);
        QNP_CHECK(criticalSampleForOverride(CriticalRollOverride::ForceFailure,
                                            sample)
                  == kCriticalForcedFailureSample);
    }
}

QNP_TEST(TrainingCriticalPolicyIsSelectedByAttackerSlot) {
    TrainingContactPolicySet set{};
    set.criticalP1 = TrainingCriticalPolicy::AlwaysSingle;
    set.criticalP2 = TrainingCriticalPolicy::NeverAny;
    QNP_CHECK(criticalPolicyForAttackerSlot(set, 0u)
              == TrainingCriticalPolicy::AlwaysSingle);
    QNP_CHECK(criticalPolicyForAttackerSlot(set, 1u)
              == TrainingCriticalPolicy::NeverAny);
}

QNP_TEST(TrainingFaultlessWhitelistMatchesTheNativeResolver) {
    const uint32_t supported[] = {10u, 2u, 1u, 35u, 24u, 4u, 34u, 16u, 27u};
    for (const uint32_t id : supported) {
        QNP_CHECK(faultlessSupportedActor(id));
    }
    for (const uint32_t id : kSelectableActors) {
        const bool expected =
            contains(supported, sizeof(supported) / sizeof(supported[0]), id);
        QNP_CHECK(faultlessSupportedActor(id) == expected);
    }
    QNP_CHECK(!faultlessSupportedActor(3u));
    QNP_CHECK(!faultlessSupportedActor(31u));
    QNP_CHECK(!faultlessSupportedActor(255u));
}

QNP_TEST(TrainingFaultlessFamilyModeMatchesTheNativeDetector) {
    for (const uint32_t id : {4u, 34u, 1u, 35u, 24u}) {
        QNP_CHECK(faultlessFamilyModeForActor(id) == 3u);
    }
    for (const uint32_t id : {16u, 27u}) {
        QNP_CHECK(faultlessFamilyModeForActor(id) == 2u);
    }
    for (const uint32_t id : {10u, 2u}) {
        QNP_CHECK(faultlessFamilyModeForActor(id) == 1u);
    }
    QNP_CHECK(faultlessFamilyModeForActor(0u) == 0u);
    QNP_CHECK(faultlessFamilyModeForActor(12u) == 0u);
    QNP_CHECK(faultlessFamilyModeForActor(255u) == 0u);

    for (const uint32_t id : kSelectableActors) {
        QNP_CHECK(faultlessSupportedActor(id)
                  == (faultlessFamilyModeForActor(id) != 0u));
    }
}

QNP_TEST(TrainingFaultlessMeterGateIsStrictlyGreaterThanSix) {
    QNP_CHECK(!faultlessMeterSufficient(0u));
    QNP_CHECK(!faultlessMeterSufficient(6u));
    QNP_CHECK(faultlessMeterSufficient(7u));
    QNP_CHECK(faultlessMeterSufficient(53u));
    QNP_CHECK(kFaultlessMeterRequirement == 6u);
}

QNP_TEST(TrainingParryWhitelistIncludesRapidAyaka) {
    const uint32_t supported[] = {12u, 28u, 5u, 0u, 31u, 19u};
    for (const uint32_t id : supported) {
        QNP_CHECK(parrySupportedActor(id));
    }
    QNP_CHECK(parrySupportedActor(0u));
    QNP_CHECK(parrySupportedActor(31u));

    for (const uint32_t id : kSelectableActors) {
        const bool expected =
            contains(supported, sizeof(supported) / sizeof(supported[0]), id);
        QNP_CHECK(parrySupportedActor(id) == expected);
    }
    QNP_CHECK(!parrySupportedActor(255u));
}

QNP_TEST(TrainingParryGatesMatchTheNativeBranch) {
    QNP_CHECK(parryPostureEligible(0x09u));
    QNP_CHECK(!parryPostureEligible(0x01u));
    QNP_CHECK(!parryPostureEligible(0x08u));
    QNP_CHECK(!parryPostureEligible(0x0Au));
    QNP_CHECK(!parryPostureEligible(0x0Cu));

    QNP_CHECK(parryAttackerExcluded(32u, 6u));
    QNP_CHECK(!parryAttackerExcluded(32u, 5u));
    QNP_CHECK(!parryAttackerExcluded(31u, 6u));

    QNP_CHECK(parryAttackPermits(0x01u));
    QNP_CHECK(parryAttackPermits(0xFFu));
    QNP_CHECK(!parryAttackPermits(0x00u));
    QNP_CHECK(!parryAttackPermits(0xFEu));

    QNP_CHECK(kParryMatcherAcceptedResult == 2u);
    QNP_CHECK(kParryMatcherActionRequest == 18u);
    QNP_CHECK(kParryMatcherPendingFlag == 1u);
}

QNP_TEST(TrainingParryTakesPrecedenceOverFaultless) {
    ContactSituation s{};
    s.defenderActor = 0u;
    s.defenderPosture = 0x09u;
    s.attackRecordByte17 = 0x01u;
    s.defenderFractionalMeter = 53u;
    s.guardedContact = true;

    QNP_CHECK(resolveContactPrecedence(s, TrainingParryPolicy::AlwaysWhenLegal,
                                       TrainingFaultlessPolicy::AlwaysWhenLegal)
              == ContactResolution::Parry);

    QNP_CHECK(resolveContactPrecedence(s, TrainingParryPolicy::Never,
                                       TrainingFaultlessPolicy::AlwaysWhenLegal)
              == ContactResolution::None);

    ContactSituation fd = s;
    fd.defenderActor = 24u;
    QNP_CHECK(resolveContactPrecedence(fd, TrainingParryPolicy::AlwaysWhenLegal,
                                       TrainingFaultlessPolicy::AlwaysWhenLegal)
              == ContactResolution::Faultless);
}

QNP_TEST(TrainingFaultlessNeverAutoBlocks) {
    ContactSituation s{};
    s.defenderActor = 24u;
    s.defenderFractionalMeter = 53u;
    s.guardedContact = false;
    QNP_CHECK(resolveContactPrecedence(s, TrainingParryPolicy::NativeInput,
                                       TrainingFaultlessPolicy::AlwaysWhenLegal)
              == ContactResolution::None);

    s.guardedContact = true;
    QNP_CHECK(resolveContactPrecedence(s, TrainingParryPolicy::NativeInput,
                                       TrainingFaultlessPolicy::AlwaysWhenLegal)
              == ContactResolution::Faultless);

    s.defenderFractionalMeter = 6u;
    QNP_CHECK(resolveContactPrecedence(s, TrainingParryPolicy::NativeInput,
                                       TrainingFaultlessPolicy::AlwaysWhenLegal)
              == ContactResolution::None);
}

QNP_TEST(TrainingContactPolicyDefaultsAreFullyNative) {
    const TrainingContactPolicySet fresh{};
    QNP_CHECK(fresh.allNative());
    QNP_CHECK(fresh.faultless == TrainingFaultlessPolicy::NativeInput);
    QNP_CHECK(fresh.parry == TrainingParryPolicy::NativeInput);
    QNP_CHECK(fresh.criticalP1 == TrainingCriticalPolicy::Native);
    QNP_CHECK(fresh.criticalP2 == TrainingCriticalPolicy::Native);

    for (const bool latch : {false, true}) {
        QNP_CHECK(criticalRollOverrideFor(fresh.criticalP1, latch)
                  == CriticalRollOverride::Native);
        QNP_CHECK(!criticalPolicyClearsGuardCrushLatch(fresh.criticalP1, latch));
    }
}

QNP_TEST(TrainingContactPolicyAssignmentValidatesBeforeItMutates) {
    TrainingContactPolicySet set{};

    QNP_CHECK(applyContactPolicyAssignment(
        set, static_cast<uint32_t>(TrainingContactPolicySlot::FaultlessDummy),
        static_cast<uint32_t>(TrainingFaultlessPolicy::AlwaysWhenLegal)));
    QNP_CHECK(set.faultless == TrainingFaultlessPolicy::AlwaysWhenLegal);

    QNP_CHECK(applyContactPolicyAssignment(
        set, static_cast<uint32_t>(TrainingContactPolicySlot::CriticalP2Attacks),
        static_cast<uint32_t>(TrainingCriticalPolicy::ForceRandomRoll)));
    QNP_CHECK(set.criticalP2 == TrainingCriticalPolicy::ForceRandomRoll);

    const TrainingContactPolicySet before = set;
    QNP_CHECK(!applyContactPolicyAssignment(
        set, static_cast<uint32_t>(TrainingContactPolicySlot::ParryDummy), 99u));
    QNP_CHECK(!applyContactPolicyAssignment(set, 77u, 0u));
    QNP_CHECK(!applyContactPolicyAssignment(
        set, static_cast<uint32_t>(TrainingContactPolicySlot::CriticalP1Attacks),
        static_cast<uint32_t>(TrainingCriticalPolicy::Count)));
    QNP_CHECK(set.faultless == before.faultless);
    QNP_CHECK(set.parry == before.parry);
    QNP_CHECK(set.criticalP1 == before.criticalP1);
    QNP_CHECK(set.criticalP2 == before.criticalP2);

    for (uint32_t v = 0u;
         v < static_cast<uint32_t>(TrainingFaultlessPolicy::Count); ++v) {
        QNP_CHECK(contactPolicyAssignmentValid(
            static_cast<uint32_t>(TrainingContactPolicySlot::FaultlessDummy), v));
    }
    for (uint32_t v = 0u;
         v < static_cast<uint32_t>(TrainingParryPolicy::Count); ++v) {
        QNP_CHECK(contactPolicyAssignmentValid(
            static_cast<uint32_t>(TrainingContactPolicySlot::ParryDummy), v));
    }
    for (uint32_t v = 0u;
         v < static_cast<uint32_t>(TrainingCriticalPolicy::Count); ++v) {
        QNP_CHECK(contactPolicyAssignmentValid(
            static_cast<uint32_t>(TrainingContactPolicySlot::CriticalP1Attacks),
            v));
    }
}

QNP_TEST(TrainingReducePolicyMapsVerdictsExactly) {
    using qtraining::TrainingReducePolicy;
    using qtraining::reduceVerdictForPolicy;
    for (int native = 0; native <= 3; ++native) {
        QNP_CHECK(reduceVerdictForPolicy(
                      TrainingReducePolicy::Native, native) == native);
        QNP_CHECK(reduceVerdictForPolicy(
                      TrainingReducePolicy::AlwaysStrong, native) == 2);
        QNP_CHECK(reduceVerdictForPolicy(
                      TrainingReducePolicy::AlwaysLight, native) == 1);
        QNP_CHECK(reduceVerdictForPolicy(
                      TrainingReducePolicy::Never, native) == 0);
    }
}

QNP_TEST(TrainingReducePolicyIsPerVictimSideAndAppendOnly) {
    using namespace qtraining;
    TrainingContactPolicySet set{};
    QNP_CHECK(set.allNative());

    QNP_CHECK(applyContactPolicyAssignment(
        set, static_cast<uint32_t>(TrainingContactPolicySlot::ReduceP1Hit),
        static_cast<uint32_t>(TrainingReducePolicy::Never)));
    QNP_CHECK(reducePolicyForVictimSlot(set, 0u)
              == TrainingReducePolicy::Never);
    QNP_CHECK(reducePolicyForVictimSlot(set, 1u)
              == TrainingReducePolicy::Native);
    QNP_CHECK(!set.allNative());

    QNP_CHECK(applyContactPolicyAssignment(
        set, static_cast<uint32_t>(TrainingContactPolicySlot::ReduceP2Hit),
        static_cast<uint32_t>(TrainingReducePolicy::AlwaysStrong)));
    QNP_CHECK(reducePolicyForVictimSlot(set, 1u)
              == TrainingReducePolicy::AlwaysStrong);

    const TrainingContactPolicySet before = set;
    QNP_CHECK(!applyContactPolicyAssignment(
        set, static_cast<uint32_t>(TrainingContactPolicySlot::ReduceP1Hit),
        static_cast<uint32_t>(TrainingReducePolicy::Count)));
    QNP_CHECK(set.reduceP1 == before.reduceP1);
    QNP_CHECK(set.reduceP2 == before.reduceP2);

    for (uint32_t v = 0u;
         v < static_cast<uint32_t>(TrainingReducePolicy::Count); ++v) {
        QNP_CHECK(contactPolicyAssignmentValid(
            static_cast<uint32_t>(TrainingContactPolicySlot::ReduceP1Hit), v));
        QNP_CHECK(contactPolicyAssignmentValid(
            static_cast<uint32_t>(TrainingContactPolicySlot::ReduceP2Hit), v));
    }
}
