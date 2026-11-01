#include "netplay/tests/TestHarness.h"

#include "input/InputActions.h"
#include "training/TrainingActionCatalog.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <type_traits>

using namespace qtraining;

namespace {

constexpr std::size_t semanticIndex(TrainingActionSemantic semantic) {
    return static_cast<std::size_t>(semantic);
}

bool sameMeta(const TrainingActionMeta& lhs, const TrainingActionMeta& rhs) {
    return lhs.semantic == rhs.semantic && lhs.exitClass == rhs.exitClass &&
           lhs.flags == rhs.flags;
}

TrainingMoveView findMove(std::uint8_t characterId, std::string_view input,
                          TrainingMoveContext context,
                          bool matchContext = false) {
    const std::uint16_t count = trainingMoveCount(characterId);
    for (std::uint16_t index = 1u; index <= count; ++index) {
        const TrainingMoveView move = trainingMove(characterId, index);
        if (move.input == input && (!matchContext || move.context == context))
            return move;
    }
    return {};
}

bool sequenceEquals(const TrainingMoveView& move,
                    std::initializer_list<std::uint32_t> expected) {
    if (!move.valid() || move.stepCount != expected.size()) return false;
    std::size_t index = 0;
    for (const std::uint32_t mask : expected) {
        if (move.relativeSteps[index++] != mask) return false;
    }
    return true;
}

struct DispatchExpectation {
    std::uint32_t action;
    TrainingDispatchFamily family;
};

constexpr std::array<DispatchExpectation, kTrainingDispatchCaseCount>
    kDispatchExpectations{{
        {0, TrainingDispatchFamily::Neutral},
        {1, TrainingDispatchFamily::GenericAction},
        {2, TrainingDispatchFamily::GenericAction},
        {3, TrainingDispatchFamily::GenericAction},
        {4, TrainingDispatchFamily::GenericAction},
        {5, TrainingDispatchFamily::GenericAction},
        {6, TrainingDispatchFamily::GenericAction},
        {7, TrainingDispatchFamily::GenericAction},
        {8, TrainingDispatchFamily::GenericAction},
        {9, TrainingDispatchFamily::GenericAction},
        {10, TrainingDispatchFamily::RunningNormal},
        {11, TrainingDispatchFamily::RunningNormal},
        {12, TrainingDispatchFamily::RunningNormal},
        {13, TrainingDispatchFamily::GroundDash},
        {14, TrainingDispatchFamily::GroundDash},
        {15, TrainingDispatchFamily::Jump},
        {16, TrainingDispatchFamily::Jump},
        {17, TrainingDispatchFamily::Jump},
        {18, TrainingDispatchFamily::Guard},
        {19, TrainingDispatchFamily::Guard},
        {20, TrainingDispatchFamily::Guard},
        {21, TrainingDispatchFamily::GroundHitReaction},
        {22, TrainingDispatchFamily::GroundHitReaction},
        {23, TrainingDispatchFamily::GroundHitReaction},
        {24, TrainingDispatchFamily::AirGuardCrush},
        {25, TrainingDispatchFamily::AirborneReaction},
        {26, TrainingDispatchFamily::Walk},
        {27, TrainingDispatchFamily::Walk},
        {28, TrainingDispatchFamily::Crouch},
        {29, TrainingDispatchFamily::Scripted29Or30},
        {30, TrainingDispatchFamily::Scripted29Or30},
        {31, TrainingDispatchFamily::RecoveryResult},
        {33, TrainingDispatchFamily::RecoveryResult},
        {34, TrainingDispatchFamily::RecoveryResult},
        {35, TrainingDispatchFamily::HardKnockdown},
        {36, TrainingDispatchFamily::Scripted36},
        {37, TrainingDispatchFamily::WallState},
        {38, TrainingDispatchFamily::AirborneReaction},
        {40, TrainingDispatchFamily::GenericAction},
        {41, TrainingDispatchFamily::GenericAction},
        {56, TrainingDispatchFamily::AirborneReaction},
        {57, TrainingDispatchFamily::AirborneReaction},
        {58, TrainingDispatchFamily::AirborneReaction},
        {70, TrainingDispatchFamily::Thrower},
        {72, TrainingDispatchFamily::GenericAction},
        {73, TrainingDispatchFamily::GenericAction},
        {91, TrainingDispatchFamily::RecursiveSystem},
        {92, TrainingDispatchFamily::Scripted92Or215},
        {96, TrainingDispatchFamily::Scripted96To98},
        {97, TrainingDispatchFamily::Scripted96To98},
        {98, TrainingDispatchFamily::Scripted96To98},
        {150, TrainingDispatchFamily::Special150Family},
        {151, TrainingDispatchFamily::GenericAction},
        {152, TrainingDispatchFamily::Special152Family},
        {153, TrainingDispatchFamily::Special150Family},
        {154, TrainingDispatchFamily::GenericAction},
        {155, TrainingDispatchFamily::Special152Family},
        {156, TrainingDispatchFamily::Special150Family},
        {157, TrainingDispatchFamily::Special157},
        {158, TrainingDispatchFamily::Special152Family},
        {160, TrainingDispatchFamily::Special160},
        {161, TrainingDispatchFamily::Special161Family},
        {162, TrainingDispatchFamily::Special161Family},
        {163, TrainingDispatchFamily::Special161Family},
        {165, TrainingDispatchFamily::Special165},
        {166, TrainingDispatchFamily::Special166},
        {167, TrainingDispatchFamily::Special152Family},
        {168, TrainingDispatchFamily::Special152Family},
        {169, TrainingDispatchFamily::Special152Family},
        {200, TrainingDispatchFamily::ThrowReception},
        {202, TrainingDispatchFamily::ThrowReception},
        {204, TrainingDispatchFamily::ThrowReception},
        {206, TrainingDispatchFamily::ThrowReception},
        {208, TrainingDispatchFamily::ThrowReception208},
        {210, TrainingDispatchFamily::ThrowReception},
        {212, TrainingDispatchFamily::ThrowReception},
        {214, TrainingDispatchFamily::ThrowReception},
        {215, TrainingDispatchFamily::Scripted92Or215},
        {216, TrainingDispatchFamily::ThrowReception},
        {218, TrainingDispatchFamily::ThrowReception},
        {220, TrainingDispatchFamily::ThrowReception},
        {222, TrainingDispatchFamily::ThrowReception},
        {224, TrainingDispatchFamily::ThrowReception},
        {226, TrainingDispatchFamily::ThrowReception},
        {228, TrainingDispatchFamily::ThrowReception},
        {230, TrainingDispatchFamily::ThrowReception},
        {231, TrainingDispatchFamily::ThrowReception},
        {232, TrainingDispatchFamily::ThrowReception},
        {234, TrainingDispatchFamily::ThrowReception},
        {235, TrainingDispatchFamily::ThrowReception},
        {236, TrainingDispatchFamily::ThrowReception},
        {237, TrainingDispatchFamily::ThrowReception},
        {238, TrainingDispatchFamily::ThrowReception},
        {239, TrainingDispatchFamily::ThrowReception},
        {240, TrainingDispatchFamily::ThrowReception},
        {241, TrainingDispatchFamily::ThrowReception},
        {242, TrainingDispatchFamily::ThrowReception},
        {243, TrainingDispatchFamily::ThrowReception},
        {244, TrainingDispatchFamily::ThrowReception},
        {246, TrainingDispatchFamily::ThrowReception},
        {250, TrainingDispatchFamily::ThrowReception},
    }};

static_assert(sizeof(TrainingActionMeta) == 4,
              "catalog metadata must remain timing-free and compact");
static_assert(std::is_trivially_copyable<TrainingActionMeta>::value,
              "catalog metadata must remain a pure value");
static_assert(std::is_trivially_copyable<TrainingNativeMoveTransition>::value,
              "native transition queries must remain allocation-free values");
static_assert(std::is_trivially_copyable<TrainingMoveSequence>::value,
              "trigger move sequences must remain allocation-free values");
static_assert(kDispatchExpectations.size() == kTrainingDispatchCaseCount,
              "dispatcher fixture must name every executable case");

}

QNP_TEST(TrainingActionCatalogIsExhaustiveAndFailClosed) {
    std::uint32_t authored = 0;
    std::uint32_t terminal = 0;
    std::uint32_t loops = 0;
    std::uint32_t explicitLoops = 0;
    std::uint32_t landing = 0;
    std::uint32_t attacks = 0;
    std::uint32_t catalogMoves = 0;
    std::uint32_t broadThrowDeny = 0;
    std::uint32_t groundedRuntimeLanding = 0;
    std::uint32_t timingDependsOnLanding = 0;
    std::uint32_t activeUntilLanding = 0;
    std::array<bool, kTrainingActionStateCount> authoredIds{};
    std::array<std::uint32_t, 14> semantics{};

    for (std::uint32_t resource = 0;
         resource < kTrainingActionResourceCount; ++resource) {
        for (std::uint32_t action = 0;
             action < kTrainingActionStateCount; ++action) {
            const TrainingActionMeta meta = trainingActionMeta(
                static_cast<std::uint8_t>(resource), action);
            const bool isAuthored = meta.has(TrainingActionFlagAuthored);

            ++semantics[semanticIndex(meta.semantic)];
            if (isAuthored) {
                ++authored;
                authoredIds[action] = true;
                QNP_CHECK(meta.semantic != TrainingActionSemantic::Unused);
                QNP_CHECK(meta.exitClass != TrainingActionExitClass::Unused);
            } else {
                QNP_CHECK(meta.semantic == TrainingActionSemantic::Unused);
                QNP_CHECK(meta.exitClass == TrainingActionExitClass::Unused);
            }

            if (meta.has(TrainingActionFlagTerminal)) {
                ++terminal;
                QNP_CHECK(isAuthored);
            }
            if (meta.has(TrainingActionFlagLoop)) {
                ++loops;
                QNP_CHECK(isAuthored);
            }
            if (meta.has(TrainingActionFlagExplicitLoop)) {
                ++explicitLoops;
                QNP_CHECK(isAuthored);
                QNP_CHECK(meta.has(TrainingActionFlagLoop));
            }
            if (meta.has(TrainingActionFlagLanding)) {
                ++landing;
                QNP_CHECK(isAuthored);
            }
            if (meta.has(TrainingActionFlagAttack)) {
                ++attacks;
                QNP_CHECK(isAuthored);
            }
            if (meta.has(TrainingActionFlagCatalogMove)) {
                ++catalogMoves;
                QNP_CHECK(isAuthored);
            }
            if (meta.has(TrainingActionFlagBroadThrowDeny)) {
                ++broadThrowDeny;
                QNP_CHECK(action >= 200);
            } else {
                QNP_CHECK(action < 200);
            }
            if (meta.has(TrainingActionFlagGroundedRuntimeLanding))
                ++groundedRuntimeLanding;
            if (meta.has(TrainingActionFlagTimingDependsOnLanding))
                ++timingDependsOnLanding;
            if (meta.has(TrainingActionFlagActiveUntilLanding))
                ++activeUntilLanding;

            switch (meta.exitClass) {
                case TrainingActionExitClass::Unused:
                    QNP_CHECK(!isAuthored);
                    break;
                case TrainingActionExitClass::UnknownOrScripted:
                    QNP_CHECK(isAuthored);
                    QNP_CHECK(!meta.has(TrainingActionFlagTerminal));
                    QNP_CHECK(!meta.has(TrainingActionFlagLoop));
                    QNP_CHECK(!meta.has(TrainingActionFlagLanding));
                    break;
                case TrainingActionExitClass::Terminal:
                    QNP_CHECK(meta.has(TrainingActionFlagTerminal));
                    QNP_CHECK(!meta.has(TrainingActionFlagLanding));
                    break;
                case TrainingActionExitClass::Loop:
                    QNP_CHECK(meta.has(TrainingActionFlagLoop));
                    QNP_CHECK(!meta.has(TrainingActionFlagTerminal));
                    QNP_CHECK(!meta.has(TrainingActionFlagLanding));
                    break;
                case TrainingActionExitClass::LandingOrRuntime:
                    QNP_CHECK(meta.has(TrainingActionFlagLanding));
                    break;
            }
        }
    }

    std::uint32_t distinctAuthoredIds = 0;
    for (bool present : authoredIds) distinctAuthoredIds += present ? 1u : 0u;

    QNP_CHECK(kTrainingActionCatalogCellCount == 35u * 256u);
    QNP_CHECK(authored == kTrainingActionAuthoredStateCount);
    QNP_CHECK(distinctAuthoredIds == kTrainingActionDistinctAuthoredIdCount);
    QNP_CHECK(terminal == 1447u);
    QNP_CHECK(loops == 2398u);
    QNP_CHECK(explicitLoops == 2334u);
    QNP_CHECK(landing == 82u);
    QNP_CHECK(attacks == 988u);
    QNP_CHECK(catalogMoves == 1175u);
    QNP_CHECK(broadThrowDeny == 35u * 56u);
    QNP_CHECK(groundedRuntimeLanding == 34u);
    QNP_CHECK(timingDependsOnLanding == 36u);
    QNP_CHECK(activeUntilLanding == 56u);

    const std::array<std::uint32_t, 14> expectedSemantics{{
        5134u, 534u, 35u, 303u, 1151u, 105u, 315u,
        70u, 105u, 35u, 35u, 35u, 1103u, 0u,
    }};
    QNP_CHECK(semantics == expectedSemantics);
}

QNP_TEST(TrainingActionCatalogCorrectsThrowTechKnowledge) {
    for (std::uint32_t resource = 0;
         resource < kTrainingActionResourceCount; ++resource) {
        const TrainingActionMeta counterpart = trainingActionMeta(
            static_cast<std::uint8_t>(resource), 72);
        const TrainingActionMeta result = trainingActionMeta(
            static_cast<std::uint8_t>(resource), 73);
        QNP_CHECK(counterpart.semantic ==
                  TrainingActionSemantic::ThrowTechCounterpart);
        QNP_CHECK(result.semantic == TrainingActionSemantic::ThrowTech);
        QNP_CHECK(!counterpart.has(TrainingActionFlagAttack));
        QNP_CHECK(!result.has(TrainingActionFlagAttack));
    }
}

QNP_TEST(TrainingActionCatalogMapsInternalActorAliasesWithoutExtraRows) {
    for (std::uint32_t action = 0;
         action < kTrainingActionStateCount; ++action) {
        QNP_CHECK(sameMeta(trainingActionMeta(35, action),
                           trainingActionMeta(24, action)));
        QNP_CHECK(sameMeta(trainingActionMeta(36, action),
                           trainingActionMeta(25, action)));
    }

    const TrainingActionMeta invalidCharacter = trainingActionMeta(37, 0);
    const TrainingActionMeta invalidAction = trainingActionMeta(0, 256);
    QNP_CHECK(invalidCharacter.semantic == TrainingActionSemantic::Unused);
    QNP_CHECK(invalidCharacter.flags == TrainingActionFlagNone);
    QNP_CHECK(invalidAction.semantic == TrainingActionSemantic::Unused);
    QNP_CHECK(invalidAction.flags == TrainingActionFlagNone);
}

QNP_TEST(TrainingActionCatalogCarriesBothBuildEvidence) {
    QNP_CHECK(kTrainingActionProfilesClassifyEquivalently);
    QNP_CHECK(std::strcmp(kTrainingActionNovemberProfileKey,
                          "qoh99-se1126") == 0);
    QNP_CHECK(std::strcmp(kTrainingActionDecemberProfileKey,
                          "qoh99-withse") == 0);
}

QNP_TEST(TrainingActionDispatcherNamesAllAndOnlyExecutableCases) {
    std::array<bool, kTrainingActionStateCount> expectedCases{};
    for (const DispatchExpectation& expected : kDispatchExpectations) {
        QNP_REQUIRE(expected.action < expectedCases.size());
        QNP_CHECK(!expectedCases[expected.action]);
        expectedCases[expected.action] = true;
        QNP_CHECK(trainingDispatchFamily(expected.action) == expected.family);
    }

    std::uint32_t mapped = 0;
    for (std::uint32_t action = 0;
         action < kTrainingActionStateCount; ++action) {
        const bool isMapped = trainingDispatchFamily(action) !=
                              TrainingDispatchFamily::Unmapped;
        QNP_CHECK(isMapped == expectedCases[action]);
        mapped += isMapped ? 1u : 0u;
    }
    QNP_CHECK(mapped == kTrainingDispatchCaseCount);
    QNP_CHECK(trainingDispatchFamily(256) == TrainingDispatchFamily::Unmapped);
}

QNP_TEST(TrainingActionBroadDenyDoesNotInventReceptionDispatcherCases) {
    QNP_CHECK(trainingActionMeta(0, 200).has(
        TrainingActionFlagBroadThrowDeny));
    QNP_CHECK(trainingActionMeta(0, 215).has(
        TrainingActionFlagBroadThrowDeny));
    QNP_CHECK(trainingActionMeta(0, 248).has(
        TrainingActionFlagBroadThrowDeny));

    QNP_CHECK(trainingDispatchFamily(200) ==
              TrainingDispatchFamily::ThrowReception);
    QNP_CHECK(trainingDispatchFamily(208) ==
              TrainingDispatchFamily::ThrowReception208);
    QNP_CHECK(trainingDispatchFamily(215) ==
              TrainingDispatchFamily::Scripted92Or215);
    QNP_CHECK(trainingDispatchFamily(248) ==
              TrainingDispatchFamily::Unmapped);
}

QNP_TEST(TrainingNativeMoveBehaviorCatalogIsExhaustiveAndShared) {
    std::uint32_t rules = 0;
    std::uint32_t sourceStates = 0;
    for (std::uint32_t resource = 0;
         resource < kTrainingActionResourceCount; ++resource) {
        for (std::uint32_t action = 0;
             action < kTrainingActionStateCount; ++action) {
            const std::uint32_t count = trainingNativeMoveBehaviorRuleCount(
                static_cast<std::uint8_t>(resource), action);
            rules += count;
            sourceStates += count != 0 ? 1u : 0u;
        }
    }

    QNP_CHECK(rules == kTrainingNativeMoveBehaviorRuleCount);
    QNP_CHECK(sourceStates == kTrainingNativeMoveBehaviorSourceStateCount);
    QNP_CHECK(rules == 113u);
    QNP_CHECK(sourceStates == 69u);
    QNP_CHECK(kTrainingNativeMoveBehaviorProfilesClassifyEquivalently);

    QNP_CHECK(trainingNativeMoveBehaviorRuleCount(35, 160) ==
              trainingNativeMoveBehaviorRuleCount(24, 160));
    QNP_CHECK(trainingNativeMoveBehaviorRuleCount(36, 160) ==
              trainingNativeMoveBehaviorRuleCount(25, 160));
    QNP_CHECK(trainingNativeMoveBehaviorRuleCount(37, 160) == 0u);
    QNP_CHECK(trainingNativeMoveBehaviorRuleCount(0, 256) == 0u);
}

QNP_TEST(TrainingNativeMoveBehaviorDistinguishesAllFiveEffects) {
    const TrainingNativeMoveTransition redirect = trainingNativeMoveTransition(
        3, 11, 0, TrainingNativeContactKind::Hit, 11, 11);
    QNP_CHECK(redirect.proof ==
              TrainingNativeTransitionProof::ProvenAutomaticContinuation);
    QNP_CHECK(redirect.trigger == TrainingNativeMoveTrigger::AnyContact);
    QNP_CHECK(redirect.effect ==
              TrainingNativeMoveEffect::RedirectAnimationFrame);
    QNP_CHECK(redirect.targetAction == 11u);
    QNP_CHECK(redirect.targetLocalFrame == 11u);
    QNP_CHECK(redirect.nativeContactMask == 0x70u);
    QNP_CHECK(redirect.has(TrainingNativeMoveFlagResetsFrameTimer));
    QNP_CHECK(redirect.allowsConventionalFrameAdvantage());

    const TrainingNativeMoveTransition change = trainingNativeMoveTransition(
        21, 165, 3, TrainingNativeContactKind::Guard, 166, 0);
    QNP_CHECK(change.proof ==
              TrainingNativeTransitionProof::ProvenAutomaticContinuation);
    QNP_CHECK(change.effect == TrainingNativeMoveEffect::ChangeAction);
    QNP_CHECK(change.targetAction == 166u);
    QNP_CHECK(change.nativeContactMask == 0x70u);

    const TrainingNativeMoveTransition replace = trainingNativeMoveTransition(
        4, 10, 0, TrainingNativeContactKind::AlternateActorMode, 42, 0);
    QNP_CHECK(replace.proof ==
              TrainingNativeTransitionProof::ProvenAutomaticContinuation);
    QNP_CHECK(replace.trigger ==
              TrainingNativeMoveTrigger::AlternateActorMode);
    QNP_CHECK(replace.effect ==
              TrainingNativeMoveEffect::ReplaceActionOnEntry);
    QNP_CHECK(replace.targetAction == 42u);
    QNP_CHECK(!replace.has(TrainingNativeMoveFlagResetsFrameTimer));

    const TrainingNativeMoveTransition end = trainingNativeMoveTransition(
        16, 151, 4, TrainingNativeContactKind::Guard, 0, 0);
    QNP_CHECK(end.proof ==
              TrainingNativeTransitionProof::ProvenAutomaticContinuation);
    QNP_CHECK(end.trigger == TrainingNativeMoveTrigger::NoHit);
    QNP_CHECK(end.effect == TrainingNativeMoveEffect::EndAction);
    QNP_CHECK(end.targetAction == kTrainingNativeMoveUnknownValue);
    QNP_CHECK(end.nativeContactMask == 0x10u);

    const TrainingNativeMoveTransition update = trainingNativeMoveTransition(
        18, 151, 0, TrainingNativeContactKind::Hit, 151, 1);
    QNP_CHECK(update.proof ==
              TrainingNativeTransitionProof::ProvenEffectOnly);
    QNP_CHECK(update.effect ==
              TrainingNativeMoveEffect::UpdateCharacterState);
    QNP_CHECK(!update.provenAutomaticContinuation());
    QNP_CHECK(!update.allowsConventionalFrameAdvantage());
}

QNP_TEST(TrainingNativeMoveBehaviorRequiresExactContactFrameAndTarget) {
    const auto azusa159 = trainingNativeMoveTransition(
        5, 151, 4, TrainingNativeContactKind::Hit, 159, 0);
    const auto azusa160 = trainingNativeMoveTransition(
        5, 151, 4, TrainingNativeContactKind::Hit, 160, 0);
    QNP_CHECK(azusa159.provenAutomaticContinuation());
    QNP_CHECK(azusa159.targetAction == 159u);
    QNP_CHECK(azusa160.provenAutomaticContinuation());
    QNP_CHECK(azusa160.targetAction == 160u);
    QNP_CHECK(trainingNativeMoveTransition(
        5, 151, 5, TrainingNativeContactKind::Hit, 159, 0).proof ==
              TrainingNativeTransitionProof::Unproven);
    QNP_CHECK(trainingNativeMoveTransition(
        5, 151, 4, TrainingNativeContactKind::Guard, 159, 0).proof ==
              TrainingNativeTransitionProof::Unproven);
    QNP_CHECK(trainingNativeMoveTransition(
        5, 151, 4, TrainingNativeContactKind::Hit, 161, 0).proof ==
              TrainingNativeTransitionProof::Unproven);

    QNP_CHECK(trainingNativeMoveTransition(
        6, 150, 0, TrainingNativeContactKind::Hit, 147, 0)
                  .provenAutomaticContinuation());
    QNP_CHECK(trainingNativeMoveTransition(
        6, 150, 0, TrainingNativeContactKind::Hit, 146, 0)
                  .provenAutomaticContinuation());
    QNP_CHECK(trainingNativeMoveTransition(
        6, 150, 0, TrainingNativeContactKind::Guard, 146, 0)
                  .provenAutomaticContinuation());
    QNP_CHECK(trainingNativeMoveTransition(
        6, 150, 0, TrainingNativeContactKind::Guard, 147, 0).proof ==
              TrainingNativeTransitionProof::Unproven);

    QNP_CHECK(trainingNativeMoveTransition(
        16, 151, 4, TrainingNativeContactKind::Hit, 151, 5).effect ==
              TrainingNativeMoveEffect::RedirectAnimationFrame);
    QNP_CHECK(trainingNativeMoveTransition(
        16, 151, 4, TrainingNativeContactKind::None, 0, 0).effect ==
              TrainingNativeMoveEffect::EndAction);
    QNP_CHECK(trainingNativeMoveTransition(
        16, 151, 4, TrainingNativeContactKind::Clash, 0, 0).effect ==
              TrainingNativeMoveEffect::EndAction);
    QNP_CHECK(trainingNativeMoveTransition(
        16, 151, 4, TrainingNativeContactKind::Hit, 0, 0).proof ==
              TrainingNativeTransitionProof::Unproven);
}

QNP_TEST(TrainingNativeMoveBehaviorInheritanceIsResourceExact) {
    QNP_CHECK(trainingNativeMoveTransition(
        29, 11, 2, TrainingNativeContactKind::Guard, 11, 11)
                  .provenAutomaticContinuation());
    QNP_CHECK(trainingNativeMoveTransition(
        27, 151, 4, TrainingNativeContactKind::Hit, 151, 5)
                  .provenAutomaticContinuation());
    QNP_CHECK(trainingNativeMoveTransition(
        28, 170, 1, TrainingNativeContactKind::Clash, 171, 0)
                  .provenAutomaticContinuation());
    QNP_CHECK(trainingNativeMoveBehaviorRuleCount(26, 161) == 1u);
    QNP_CHECK(trainingNativeMoveBehaviorRuleCount(26, 151) == 0u);

    QNP_CHECK(trainingNativeMoveTransition(
        0, 11, 2, TrainingNativeContactKind::Guard, 11, 11).proof ==
              TrainingNativeTransitionProof::Unproven);
}

QNP_TEST(TrainingConventionalFrameAdvantageExclusionsFailClosed) {
    for (std::uint32_t resource = 0;
         resource < kTrainingActionResourceCount; ++resource) {
        const auto id = static_cast<std::uint8_t>(resource);
        QNP_CHECK(trainingActionAllowsConventionalFrameAdvantage(id, 0));
        QNP_CHECK(trainingActionAllowsConventionalFrameAdvantage(id, 1));
        QNP_CHECK(!trainingActionAllowsConventionalFrameAdvantage(id, 29));
        QNP_CHECK(!trainingActionAllowsConventionalFrameAdvantage(id, 70));
        QNP_CHECK(!trainingActionAllowsConventionalFrameAdvantage(id, 71));
        QNP_CHECK(!trainingActionAllowsConventionalFrameAdvantage(id, 72));
        QNP_CHECK(!trainingActionAllowsConventionalFrameAdvantage(id, 73));
        QNP_CHECK(!trainingActionAllowsConventionalFrameAdvantage(id, 200));
        QNP_CHECK(!trainingActionAllowsConventionalFrameAdvantage(id, 255));
    }
    QNP_CHECK(!trainingActionAllowsConventionalFrameAdvantage(37, 0));
    QNP_CHECK(!trainingActionAllowsConventionalFrameAdvantage(0, 256));

    const TrainingNativeMoveTransition state71Capture =
        trainingNativeMoveTransition(
            0, 152, 0, TrainingNativeContactKind::Hit, 71, 0);
    QNP_CHECK(state71Capture.proof ==
              TrainingNativeTransitionProof::ProvenExcludedContinuation);
    QNP_CHECK(state71Capture.effect ==
              TrainingNativeMoveEffect::ChangeAction);
    QNP_CHECK(state71Capture.has(
        TrainingNativeMoveFlagCaptureSequence));
    QNP_CHECK(state71Capture.has(
        TrainingNativeMoveFlagConventionalFrameAdvantageExcluded));
    QNP_CHECK(!state71Capture.allowsConventionalFrameAdvantage());

    const TrainingNativeMoveTransition state67Capture =
        trainingNativeMoveTransition(
            17, 151, 0, TrainingNativeContactKind::Hit, 67, 0);
    QNP_CHECK(state67Capture.proof ==
              TrainingNativeTransitionProof::ProvenExcludedContinuation);
    QNP_CHECK(state67Capture.has(
        TrainingNativeMoveFlagCaptureSequence));

    for (std::uint32_t action : {71u, 72u, 73u, 200u, 215u, 250u}) {
        QNP_CHECK(trainingNativeMoveTransition(
            0, action, 0, TrainingNativeContactKind::Hit, 0, 0).proof ==
                  TrainingNativeTransitionProof::Unproven);
    }
}

QNP_TEST(TrainingNativeMoveUnknownAndMalformedTransitionsStayUnproven) {
    QNP_CHECK(trainingNativeMoveTransition(
        0, 1, 0, TrainingNativeContactKind::Hit, 0, 0).proof ==
              TrainingNativeTransitionProof::Unproven);
    QNP_CHECK(trainingNativeMoveTransition(
        37, 11, 0, TrainingNativeContactKind::Hit, 11, 11).proof ==
              TrainingNativeTransitionProof::Unproven);
    QNP_CHECK(trainingNativeMoveTransition(
        3, 256, 0, TrainingNativeContactKind::Hit, 11, 11).proof ==
              TrainingNativeTransitionProof::Unproven);
    QNP_CHECK(trainingNativeMoveTransition(
        3, 11, 0, TrainingNativeContactKind::Hit, 256, 11).proof ==
              TrainingNativeTransitionProof::Unproven);
    QNP_CHECK(trainingNativeMoveTransition(
        3, 11, 0, static_cast<TrainingNativeContactKind>(0x7Fu), 11, 11)
                  .proof == TrainingNativeTransitionProof::Unproven);
    QNP_CHECK(trainingNativeMoveTransition(
        3, 11, 0, TrainingNativeContactKind::None, 11, 11).proof ==
              TrainingNativeTransitionProof::Unproven);
    QNP_CHECK(trainingNativeMoveTransition(
        3, 11, 0, TrainingNativeContactKind::Hit, 11, 10).proof ==
              TrainingNativeTransitionProof::Unproven);
}

QNP_TEST(TrainingTriggerMoveCatalogIsExhaustiveBoundedAndCharacterSpecific) {
    std::uint32_t total = 0u;
    std::uint16_t maximum = 0u;
    for (std::uint8_t character = 0u;
         character < kTrainingMoveSelectableCharacterCount; ++character) {
        const std::uint16_t count = trainingMoveCount(character);
        QNP_CHECK(count != 0u);
        QNP_CHECK(count <= kTrainingMoveMaximumPerCharacter);
        if (count > maximum) maximum = count;
        total += count;

        for (std::uint16_t index = 1u; index <= count; ++index) {
            const TrainingMoveView move = trainingMove(character, index);
            QNP_REQUIRE(move.valid());
            QNP_CHECK(move.characterId == character);
            QNP_CHECK(move.moveIndex == index);
            QNP_CHECK(!move.input.empty());
            QNP_CHECK(!move.label.empty());
            QNP_CHECK(!move.actorResource.empty());
            QNP_CHECK(move.stepCount <= kTrainingMoveMaximumSequenceSteps);
            QNP_CHECK(move.relativeSteps[0] == 0u);
            QNP_CHECK(move.relativeSteps[move.stepCount - 1u] == 0u);
            for (std::uint8_t step = 0u; step < move.stepCount; ++step)
                QNP_CHECK(qinput::isCanonicalLogicalFightInput(
                    move.relativeSteps[step]));

            TrainingMoveSequence copied{};
            QNP_REQUIRE(copyTrainingMoveSequence(character, index, copied));
            QNP_CHECK(copied.stepCount >= move.stepCount);
            QNP_CHECK(copied.stepCount <= kTrainingMoveMaximumSequenceSteps);
            QNP_CHECK(copied.relativeMasks[0] == 0u);
            QNP_CHECK(copied.relativeMasks[copied.stepCount - 1u] == 0u);
            std::uint8_t expected = 0u;
            for (std::uint8_t step = 0u; step < move.stepCount; ++step) {
                const std::uint32_t raw = move.relativeSteps[step];
                QNP_CHECK(copied.relativeMasks[expected] == raw);
                ++expected;
                const bool presses =
                    (raw & qinput::kLogicalButtonMask) != 0u;
                if (!presses || step + 1u >= move.stepCount) continue;
                const std::uint32_t next = move.relativeSteps[step + 1u];
                const bool nextKeeps =
                    (next & qinput::kLogicalButtonMask) == 0u
                    && (next & qinput::kLogicalDirectionMask)
                           == (raw & qinput::kLogicalDirectionMask);
                if (nextKeeps) continue;
                QNP_CHECK(copied.relativeMasks[expected]
                    == (raw & qinput::kLogicalDirectionMask));
                ++expected;
            }
            QNP_CHECK(expected == copied.stepCount);
            for (std::uint8_t step = copied.stepCount;
                 step < kTrainingMoveMaximumSequenceSteps; ++step) {
                QNP_CHECK(copied.relativeMasks[step] == 0u);
            }
        }
    }

    QNP_CHECK(total == kTrainingMoveCatalogRecordCount);
    QNP_CHECK(total == 1159u);
    QNP_CHECK(maximum == kTrainingMoveMaximumPerCharacter);
    QNP_CHECK(maximum == 54u);
    QNP_CHECK(kTrainingMoveCatalogStepCount == 4685u);
    QNP_CHECK(kTrainingMoveCatalogExcludedRowCount == 39u);
    QNP_CHECK(kTrainingMoveMaximumSequenceSteps == 9u);
    QNP_CHECK(kTrainingMoveProfilesClassifyEquivalently);
    QNP_CHECK(trainingMoveCatalogFingerprint()
        == "07CC4A1C082C115D657D12583753E45D4761EEEF935F252FD1F771100FEAE921");

    const TrainingMoveView saoriSuper = findMove(
        0u, "41236+C", TrainingMoveContext::Ground, true);
    QNP_REQUIRE(saoriSuper.valid());
    QNP_CHECK(saoriSuper.label.find("Hyper Tackle")
              != std::string_view::npos);
    const TrainingMoveView corinSuper = findMove(
        32u, "41236+C", TrainingMoveContext::Ground, true);
    QNP_REQUIRE(corinSuper.valid());
    QNP_CHECK(corinSuper.label.find("Angelic Hercules")
              != std::string_view::npos);
    QNP_CHECK(corinSuper.label.find("Hyper Tackle")
              == std::string_view::npos);
}

QNP_TEST(TrainingTriggerMoveCatalogNormalizesLoadedPartnerActorsOnly) {
    QNP_CHECK(trainingMoveCatalogId() == 0x07CC4A1Cu);
    QNP_CHECK(trainingMoveCatalogFingerprint().substr(0u, 8u)
              == "07CC4A1C");
    QNP_CHECK(trainingMoveSelectableCharacterId(0u) == 0u);
    QNP_CHECK(trainingMoveSelectableCharacterId(33u) == 33u);
    QNP_CHECK(trainingMoveSelectableCharacterId(34u) == 4u);
    QNP_CHECK(trainingMoveSelectableCharacterId(35u) == 1u);
    QNP_CHECK(trainingMoveSelectableCharacterId(36u) == 25u);
    QNP_CHECK(trainingMoveSelectableCharacterId(37u)
              == kTrainingMoveInvalidCharacter);
    QNP_CHECK(trainingMoveCount(34u) == trainingMoveCount(4u));
    QNP_CHECK(trainingMoveCount(35u) == trainingMoveCount(1u));
    QNP_CHECK(trainingMoveCount(36u) == trainingMoveCount(25u));
    QNP_CHECK(trainingMoveCount(37u) == 0u);

    const TrainingMoveView kaede = trainingMove(34u, 1u);
    const TrainingMoveView mizuho = trainingMove(35u, 1u);
    const TrainingMoveView comicSerika = trainingMove(36u, 1u);
    QNP_CHECK(kaede.valid() && kaede.characterId == 4u);
    QNP_CHECK(mizuho.valid() && mizuho.characterId == 1u);
    QNP_CHECK(comicSerika.valid() && comicSerika.characterId == 25u);

    QNP_CHECK(!trainingMove(0u, 0u).valid());
    QNP_CHECK(!trainingMove(0u, trainingMoveCount(0u) + 1u).valid());
    QNP_CHECK(!trainingMove(255u, 1u).valid());
    TrainingMoveSequence invalid{};
    invalid.stepCount = 9u;
    invalid.relativeMasks[0] = qinput::kLogicalA;
    QNP_CHECK(!copyTrainingMoveSequence(255u, 1u, invalid));
    QNP_CHECK(invalid.stepCount == 0u);
    QNP_CHECK(invalid.relativeMasks[0] == 0u);
}

QNP_TEST(TrainingTriggerMoveSequencesHoldTheDirectionOnTheReleaseFrame) {
    const std::uint32_t down = qinput::kLogicalDown;
    const std::uint32_t back = qinput::kLogicalLeft;
    const std::uint32_t forward = qinput::kLogicalRight;
    const std::uint32_t a = qinput::kLogicalA;
    const std::uint32_t c = qinput::kLogicalC;

    const auto copiedEquals = [](std::uint8_t character, std::string_view input,
                                 TrainingMoveContext context,
                                 std::initializer_list<std::uint32_t> expected) {
        const TrainingMoveView move = findMove(character, input, context, true);
        if (!move.valid()) return false;
        TrainingMoveSequence copied{};
        if (!copyTrainingMoveSequence(character, move.moveIndex, copied))
            return false;
        if (copied.stepCount != expected.size()) return false;
        std::uint8_t index = 0u;
        for (const std::uint32_t mask : expected) {
            if (copied.relativeMasks[index++] != mask) return false;
        }
        return true;
    };

    QNP_CHECK(copiedEquals(0u, "2A", TrainingMoveContext::Ground,
                           {0u, down | a, down, 0u}));
    QNP_CHECK(copiedEquals(0u, "5A", TrainingMoveContext::Ground,
                           {0u, a, 0u}));
    QNP_CHECK(copiedEquals(0u, "214+A", TrainingMoveContext::Ground,
                           {0u, down, down | back, back | a, back, 0u}));
    QNP_CHECK(copiedEquals(0u, "66A", TrainingMoveContext::Ground,
                           {0u, forward, 0u, forward | a, forward, 0u}));
    QNP_CHECK(copiedEquals(0u, "j.4+C (close)", TrainingMoveContext::Air,
                           {0u, back | c, back, 0u}));
    QNP_CHECK(copiedEquals(8u, "A repeatedly", TrainingMoveContext::Ground,
                           {0u, a, 0u, a, 0u, a, 0u, a, 0u}));
}

QNP_TEST(TrainingTriggerMoveSequencesPreserveNativeHistoryEdges) {
    const std::uint32_t up = qinput::kLogicalUp;
    const std::uint32_t down = qinput::kLogicalDown;
    const std::uint32_t back = qinput::kLogicalLeft;
    const std::uint32_t forward = qinput::kLogicalRight;

    QNP_CHECK(sequenceEquals(findMove(
        0u, "5A", TrainingMoveContext::Ground, true),
        {0u, qinput::kLogicalA, 0u}));
    QNP_CHECK(sequenceEquals(findMove(
        0u, "66A", TrainingMoveContext::Ground, true),
        {0u, forward, 0u, forward | qinput::kLogicalA, 0u}));
    QNP_CHECK(sequenceEquals(findMove(
        0u, "22+A", TrainingMoveContext::Ground, true),
        {0u, down, 0u, down | qinput::kLogicalA, 0u}));
    QNP_CHECK(sequenceEquals(findMove(
        32u, "222+C", TrainingMoveContext::Air, true),
        {0u, down, 0u, down, 0u, down | qinput::kLogicalC, 0u}));
    QNP_CHECK(sequenceEquals(findMove(
        0u, "41236+C", TrainingMoveContext::Ground, true),
        {0u, back, down | back, down, down | forward,
         forward | qinput::kLogicalC, 0u}));
    QNP_CHECK(sequenceEquals(findMove(
        18u, "A+B", TrainingMoveContext::Ground, true),
        {0u, qinput::kLogicalA | qinput::kLogicalB, 0u}));
    QNP_CHECK(sequenceEquals(findMove(
        0u, "j.4+C (close)", TrainingMoveContext::Air, true),
        {0u, back | qinput::kLogicalC, 0u}));
    QNP_CHECK(sequenceEquals(findMove(
        0u, "9", TrainingMoveContext::Ground, true),
        {0u, up | forward, 0u}));

    const TrainingMoveView repeated = findMove(
        8u, "A repeatedly", TrainingMoveContext::Ground, true);
    QNP_REQUIRE(repeated.valid());
    QNP_CHECK(repeated.has(TrainingMoveFlagRepeatedButton));
    QNP_CHECK(sequenceEquals(repeated,
        {0u, qinput::kLogicalA, 0u, qinput::kLogicalA, 0u,
         qinput::kLogicalA, 0u, qinput::kLogicalA, 0u}));

    QNP_CHECK(qinput::applyNativeFacing(forward, 0u) == forward);
    QNP_CHECK(qinput::applyNativeFacing(forward, 1u) == back);
}

QNP_TEST(TrainingTriggerMoveQualifiersRemainExplicitAndAmbiguityFailsClosed) {
    const TrainingMoveView airThrow = findMove(
        0u, "j.4+C (close)", TrainingMoveContext::Air, true);
    QNP_REQUIRE(airThrow.valid());
    QNP_CHECK(airThrow.has(TrainingMoveFlagAirNotation));
    QNP_CHECK(airThrow.has(TrainingMoveFlagCloseQualifier));

    const TrainingMoveView followUp = findMove(
        5u, "6+C after Pulverization", TrainingMoveContext::FollowUp, true);
    QNP_REQUIRE(followUp.valid());
    QNP_CHECK(followUp.has(TrainingMoveFlagFollowUp));
    QNP_CHECK(followUp.has(TrainingMoveFlagModeDependent));
    QNP_CHECK(sequenceEquals(
        followUp, {0u, qinput::kLogicalRight | qinput::kLogicalC, 0u}));

    const TrainingMoveView partnerToggle = findMove(
        1u, "D", TrainingMoveContext::Either, true);
    QNP_REQUIRE(partnerToggle.valid());
    QNP_CHECK(partnerToggle.actionId == kTrainingMoveUnknownAction);
    QNP_CHECK(partnerToggle.has(TrainingMoveFlagActionUnknown));
    QNP_CHECK(partnerToggle.has(TrainingMoveFlagModeDependent));
    QNP_CHECK(partnerToggle.category == TrainingMoveCategory::PartnerCommand);
    QNP_CHECK(sequenceEquals(partnerToggle, {0u, qinput::kLogicalD, 0u}));
    QNP_CHECK(!trainingMoveDirectTriggerEligible(partnerToggle));
    QNP_CHECK(!trainingMoveGuardCancelTriggerEligible(partnerToggle));

    const TrainingMoveView partnerAttack = findMove(
        1u, "236+A", TrainingMoveContext::Ground, true);
    QNP_REQUIRE(partnerAttack.valid());
    QNP_CHECK(partnerAttack.has(TrainingMoveFlagPartnerActor));
    QNP_CHECK(partnerAttack.actorResource == "KANAKO");
    QNP_CHECK(trainingMoveDirectTriggerEligible(partnerAttack));

    const TrainingMoveView tandem = findMove(
        1u, "214+D", TrainingMoveContext::Ground, true);
    QNP_REQUIRE(tandem.valid());
    QNP_CHECK(tandem.category == TrainingMoveCategory::PartnerCommand);
    QNP_CHECK(tandem.has(TrainingMoveFlagGuardCancelEligible));
    QNP_CHECK(trainingMoveDirectTriggerEligible(tandem));
    QNP_CHECK(trainingMoveGuardCancelTriggerEligible(tandem));

    const TrainingMoveView super = findMove(
        0u, "41236+C", TrainingMoveContext::Ground, true);
    const TrainingMoveView normal = findMove(
        0u, "5A", TrainingMoveContext::Ground, true);
    QNP_CHECK(super.has(TrainingMoveFlagGuardCancelEligible));
    QNP_CHECK(!normal.has(TrainingMoveFlagGuardCancelEligible));
    QNP_CHECK(trainingMoveDirectTriggerEligible(super));
    QNP_CHECK(trainingMoveGuardCancelTriggerEligible(super));
    QNP_CHECK(trainingMoveTriggerEligible(
        super, TrainingTriggerEvent::GuardCancel));
    QNP_CHECK(trainingMoveTriggerEligible(
        normal, TrainingTriggerEvent::OnHit));
    QNP_CHECK(!trainingMoveTriggerEligible(
        normal, TrainingTriggerEvent::GuardCancel));
    QNP_CHECK(trainingMoveDirectTriggerEligible(normal));
    QNP_CHECK(!trainingMoveGuardCancelTriggerEligible(normal));

    const TrainingMoveView airSpecial = findMove(
        8u, "214+A", TrainingMoveContext::Air, true);
    QNP_REQUIRE(airSpecial.valid());
    QNP_CHECK(airSpecial.context == TrainingMoveContext::Air);
    QNP_CHECK(airSpecial.has(TrainingMoveFlagGuardCancelEligible));
    QNP_CHECK(trainingMoveDirectTriggerEligible(airSpecial));
    QNP_CHECK(trainingMoveGuardCancelTriggerEligible(airSpecial));
    QNP_CHECK(trainingMoveTriggerEligible(
        airSpecial, TrainingTriggerEvent::AfterAirRecovery));
    QNP_CHECK(!trainingMoveTriggerEligible(
        airSpecial, TrainingTriggerEvent::OnWakeup));
    QNP_CHECK(trainingMoveContextAllowsPosture(airSpecial, true));
    QNP_CHECK(!trainingMoveContextAllowsPosture(airSpecial, false));
    QNP_CHECK(trainingMoveContextAllowsPosture(normal, false));
    QNP_CHECK(!trainingMoveContextAllowsPosture(normal, true));

    const TrainingMoveView repeatedChoice = findMove(
        8u, "A repeatedly", TrainingMoveContext::Ground, true);
    QNP_REQUIRE(repeatedChoice.valid());
    QNP_CHECK(!trainingMoveDirectTriggerEligible(followUp));
    QNP_CHECK(!trainingMoveGuardCancelTriggerEligible(followUp));
    QNP_CHECK(!trainingMoveDirectTriggerEligible(repeatedChoice));
    QNP_CHECK(!trainingMoveGuardCancelTriggerEligible(repeatedChoice));
    QNP_CHECK(!trainingMoveDirectTriggerEligible(TrainingMoveView{}));
    QNP_CHECK(!trainingMoveGuardCancelTriggerEligible(TrainingMoveView{}));

    for (std::uint8_t character = 0u;
         character < kTrainingMoveSelectableCharacterCount; ++character) {
        QNP_CHECK(!findMove(character, "automatic",
                            TrainingMoveContext::FollowUp, false).valid());
        QNP_CHECK(!findMove(character, "Attack while guarding",
                            TrainingMoveContext::Guard, false).valid());
        QNP_CHECK(!findMove(character, "-",
                            TrainingMoveContext::Ground, false).valid());
    }
}
