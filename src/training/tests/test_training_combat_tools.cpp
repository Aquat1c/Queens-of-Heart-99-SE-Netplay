#include "netplay/tests/TestHarness.h"

#include "training/TrainingCombatTools.h"
#include "training/TrainingMacros.h"

#include <cstddef>
#include <cstdint>

using namespace qtraining;

namespace {

CombatFrameSample combatFrame(uint32_t generation, uint64_t step,
                              uint32_t p1Action, bool p1Natural,
                              bool p1Response, uint32_t p2Action,
                              bool p2Natural, bool p2Response) {
    CombatFrameSample sample{};
    sample.readable = true;
    sample.trainingGeneration = generation;
    sample.stepSerial = step;
    sample.roundValid = true;
    sample.owners[0].action = p1Action;
    sample.owners[0].naturalReady = p1Natural;
    sample.owners[0].responseReady = p1Response;
    sample.owners[0].actorToken = 1;
    sample.owners[0].actionEpoch = 1;
    sample.owners[0].semanticPathValid = true;
    sample.owners[1].action = p2Action;
    sample.owners[1].naturalReady = p2Natural;
    sample.owners[1].responseReady = p2Response;
    sample.owners[1].actorToken = 2;
    sample.owners[1].actionEpoch = 1;
    sample.owners[1].semanticPathValid = true;
    return sample;
}

CombatContactEvent combatContact(uint64_t serial, uint64_t step,
                                 CombatContactKind kind,
                                 uint8_t attacker = 0,
                                 uint8_t victim = 1,
                                 bool attackerIsPrimary = true) {
    CombatContactEvent contact{};
    contact.serial = serial;
    contact.stepSerial = step;
    contact.kind = kind;
    contact.attackerOwner = attacker;
    contact.victimOwner = victim;
    contact.attackerIsPrimary = attackerIsPrimary;
    contact.attackerActorToken = static_cast<uint64_t>(attacker) + 1u;
    contact.victimActorToken = static_cast<uint64_t>(victim) + 1u;
    contact.attackerActionEpoch = 1;
    contact.victimActionEpoch = 1;
    contact.victimIsPrimary = true;
    contact.defenderLockEligible = true;
    return contact;
}

TrainingTriggerStepResult triggerStep(
    TrainingTriggerController& controller, const CombatFrameSample& sample,
    bool macroIdle = true, uint8_t validSlots = 0xFFu,
    uint64_t validMoves = 0u,
    bool zeroDelayWakeupMoveReady = true) {
    return controller.advance(
        sample, nullptr, 0, macroIdle, validSlots, validMoves,
        zeroDelayWakeupMoveReady);
}

TrainingTriggerStepResult triggerContactStep(
    TrainingTriggerController& controller, const CombatFrameSample& sample,
    const CombatContactEvent& contact, bool macroIdle = true,
    uint8_t validSlots = 0xFFu, uint64_t validMoves = 0u,
    bool zeroDelayWakeupMoveReady = true) {
    return controller.advance(
        sample, &contact, 1, macroIdle, validSlots, validMoves,
        zeroDelayWakeupMoveReady);
}

TrainingTriggerConfig triggerConfig(uint8_t slot, uint16_t delay = 0,
                                    uint8_t chance = 100) {
    TrainingTriggerConfig config{};
    config.macroSlot = slot;
    config.delaySteps = delay;
    config.chancePercent = chance;
    return config;
}

TrainingTriggerConfig triggerMoveConfig(uint16_t move, uint16_t delay = 0,
                                        uint8_t chance = 100) {
    TrainingTriggerConfig config{};
    config.moveIndex = move;
    config.delaySteps = delay;
    config.chancePercent = chance;
    return config;
}

static bool pressRecordThroughPlacement(qtraining::TrainingMacros& macros) {
    if (!macros.requestRecordToggle()) return false;
    if (macros.state() != qtraining::MacroState::PreRecord) return true;
    return macros.requestRecordToggle();
}

bool recordOneFrame(TrainingMacros& macros, uint64_t& step,
                    uint32_t slot, uint32_t mask, uint8_t facing = 0) {
    if (!macros.selectSlot(slot)
        || !pressRecordThroughPlacement(macros)) {
        return false;
    }
    MacroStepInput input{};
    if (macros.advance(step++, input).state != MacroState::Recording)
        return false;
    input.recordSourceMask = mask;
    const uint64_t recordStep = step++;
    const MacroStepResult record = macros.advance(recordStep, input);
    if (!record.recordFrameRequested) return false;
    if (!macros.commitRecordedFrame(recordStep, mask, facing).committed)
        return false;
    if (!pressRecordThroughPlacement(macros)) return false;
    input = MacroStepInput{};
    return macros.advance(step++, input).state == MacroState::Idle;
}

}

QNP_TEST(TrainingCombatActionFamiliesAreQohSpecific) {
    for (uint32_t action = 0; action < 220; ++action) {
        QNP_CHECK(combatActionIsGuard(action)
                  == (action == 18 || action == 19 || action == 20));
        QNP_CHECK(combatActionIsGroundHit(action)
                  == (action == 21 || action == 22 || action == 23));
        QNP_CHECK(combatActionIsAirReaction(action)
                  == (action == 24 || action == 25 || action == 38
                      || action == 56 || action == 57 || action == 58));
        QNP_CHECK(combatActionIsKnockdown(action)
                  == (action == 35 || action == 36));
        QNP_CHECK(combatActionIsAirRecovery(action) == (action == 31));
        QNP_CHECK(combatActionIsGroundRecovery(action)
                  == (action == 33 || action == 34));
        QNP_CHECK(combatActionIsWallRecoveryPath(action) == (action == 37));
        QNP_CHECK(combatActionIsNaturalWakeupPath(action)
                  == (action == 25 || action == 35 || action == 36
                      || action == 37 || action == 38
                      || action == 56 || action == 57 || action == 58
                      || action >= 200));
        QNP_CHECK(combatActionIsThrowTechResult(action) == (action == 73));
    }
    QNP_CHECK(!combatActionIsHitReaction(73));
    QNP_CHECK(static_cast<uint8_t>(TrainingTriggerEvent::AfterThrowTech) == 6);
    QNP_CHECK(static_cast<uint8_t>(TrainingTriggerEvent::GuardCancel) == 7);
    QNP_CHECK(static_cast<uint8_t>(TrainingTriggerEvent::Count) == 8);
}

QNP_TEST(TrainingNaturalWakeupPathsOwnOnlyNoTechReactionExits) {
    const uint32_t wakePaths[] = {25u, 35u, 36u, 37u, 38u, 56u, 57u, 58u};
    uint32_t generation = 200u;
    for (const uint32_t wakePath : wakePaths) {
        TrainingTriggerController controller;
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::AfterHitstun, triggerConfig(1u)));
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::OnWakeup, triggerConfig(2u)));
        triggerStep(controller, combatFrame(
            generation, 1u, 0u, true, true, 0u, true, true));
        const CombatContactEvent hit =
            combatContact(1u, 2u, CombatContactKind::Hit);
        triggerContactStep(controller, combatFrame(
            generation, 2u, 70u, false, false,
            wakePath, false, false), hit);
        const TrainingTriggerStepResult result = triggerStep(
            controller, combatFrame(
                generation, 3u, 70u, false, false,
                0u, false, true));
        QNP_CHECK(result.requestMacroStart);
        QNP_CHECK(result.event == TrainingTriggerEvent::OnWakeup);
        QNP_CHECK(result.macroSlotIndex == 1u);
        ++generation;
    }

    const uint32_t nonWakeReactions[] = {21u, 22u, 23u, 24u};
    for (const uint32_t reaction : nonWakeReactions) {
        TrainingTriggerController controller;
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::AfterHitstun, triggerConfig(1u)));
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::OnWakeup, triggerConfig(2u)));
        triggerStep(controller, combatFrame(
            generation, 1u, 0u, true, true, 0u, true, true));
        const CombatContactEvent hit =
            combatContact(1u, 2u, CombatContactKind::Hit);
        triggerContactStep(controller, combatFrame(
            generation, 2u, 70u, false, false,
            reaction, false, false), hit);
        const TrainingTriggerStepResult result = triggerStep(
            controller, combatFrame(
                generation, 3u, 70u, false, false,
                0u, false, true));
        QNP_CHECK(result.requestMacroStart);
        QNP_CHECK(result.event == TrainingTriggerEvent::AfterHitstun);
        QNP_CHECK(result.macroSlotIndex == 0u);
        ++generation;
    }
}

QNP_TEST(TrainingTriggerConfigurationIsBoundedAndAppendSafe) {
    TrainingTriggerController controller;
    QNP_CHECK(controller.setConfig(TrainingTriggerEvent::OnHit,
                                   triggerConfig(8, 240, 100)));
    QNP_CHECK(controller.config(TrainingTriggerEvent::OnHit).macroSlot == 8);
    QNP_CHECK(controller.setConfig(
        TrainingTriggerEvent::AfterBlock,
        triggerConfig(kTrainingTriggerRandomMacroSlot, 0, 100)));
    QNP_CHECK(!controller.setConfig(TrainingTriggerEvent::OnHit,
                                    triggerConfig(10, 0, 100)));
    QNP_CHECK(!controller.setConfig(TrainingTriggerEvent::OnHit,
                                    triggerConfig(1, 241, 100)));
    QNP_CHECK(!controller.setConfig(TrainingTriggerEvent::OnHit,
                                    triggerConfig(1, 0, 101)));
    QNP_CHECK(!controller.setConfig(
        static_cast<TrainingTriggerEvent>(99), triggerConfig(1)));
    QNP_CHECK(controller.config(TrainingTriggerEvent::OnHit).macroSlot == 8);

    QNP_CHECK(controller.setConfig(
        TrainingTriggerEvent::OnHit,
        triggerMoveConfig(kTrainingTriggerMaximumMoveIndex)));
    QNP_CHECK(controller.config(TrainingTriggerEvent::OnHit).macroSlot == 0u);
    QNP_CHECK(controller.config(TrainingTriggerEvent::OnHit).moveIndex
              == kTrainingTriggerMaximumMoveIndex);
    QNP_CHECK(!controller.setConfig(
        TrainingTriggerEvent::OnHit,
        triggerMoveConfig(kTrainingTriggerMaximumMoveIndex + 1u)));
    TrainingTriggerConfig ambiguous = triggerMoveConfig(1u);
    ambiguous.macroSlot = 1u;
    QNP_CHECK(!controller.setConfig(
        TrainingTriggerEvent::OnHit, ambiguous));
}

QNP_TEST(TrainingOnHitUsesExactDummyContactAndDebouncesSerials) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1)));

    const CombatContactEvent hit =
        combatContact(1, 10, CombatContactKind::Hit);
    TrainingTriggerStepResult result = triggerContactStep(
        controller, combatFrame(4, 10, 70, true, true, 22, false, false), hit);
    QNP_CHECK(result.requestMacroStart);
    QNP_CHECK(result.macroSlotIndex == 0);
    QNP_CHECK(result.event == TrainingTriggerEvent::OnHit);

    result = triggerContactStep(
        controller, combatFrame(4, 11, 70, true, true, 22, false, false), hit);
    QNP_CHECK(!result.requestMacroStart);
    QNP_CHECK(controller.status().startedCount == 1);

    const CombatContactEvent reverse =
        combatContact(2, 12, CombatContactKind::Hit, 1, 0);
    result = triggerContactStep(
        controller, combatFrame(4, 12, 22, false, false, 70, true, true),
        reverse);
    QNP_CHECK(!result.requestMacroStart);
    QNP_CHECK(controller.status().startedCount == 1);
}

QNP_TEST(TrainingOnHitMoveWaitsForANativeResponseWindow) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::OnHit, triggerMoveConfig(2u)));

    const CombatContactEvent hit =
        combatContact(1u, 10u, CombatContactKind::Hit);
    TrainingTriggerStepResult result = triggerContactStep(
        controller,
        combatFrame(4u, 10u, 70u, true, true, 22u, false, false),
        hit, true, 0u, 0x1Fu);
    QNP_CHECK(!result.requestMoveStart);
    QNP_CHECK(controller.status().pendingCount == 1u);

    result = triggerStep(
        controller,
        combatFrame(4u, 11u, 70u, true, true, 22u, false, false),
        true, 0u, 0x1Fu);
    QNP_CHECK(!result.requestMoveStart);
    QNP_CHECK(controller.status().pendingCount == 1u);

    result = triggerStep(
        controller,
        combatFrame(4u, 12u, 0u, true, true, 0u, false, true),
        true, 0u, 0x1Fu);
    QNP_CHECK(result.requestMoveStart);
    QNP_CHECK(result.moveIndex == 2u);
    QNP_CHECK(result.event == TrainingTriggerEvent::OnHit);
    QNP_CHECK(controller.status().pendingCount == 0u);
    QNP_CHECK(controller.status().lastMoveIndex == 2u);
}

QNP_TEST(TrainingDeferredMoveTracksLatestHitAndConfigCleanup) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::OnHit, triggerMoveConfig(1u, 2u)));

    CombatContactEvent hit = combatContact(1u, 10u, CombatContactKind::Hit);
    triggerContactStep(
        controller,
        combatFrame(1u, 10u, 0u, true, true, 22u, false, false),
        hit, true, 0u, 1u);
    hit = combatContact(2u, 11u, CombatContactKind::Hit);
    triggerContactStep(
        controller,
        combatFrame(1u, 11u, 0u, true, true, 22u, false, false),
        hit, true, 0u, 1u);
    QNP_CHECK(controller.status().pendingCount == 1u);
    QNP_CHECK(controller.status().droppedCount == 1u);
    QNP_CHECK(controller.status().lastOutcome
              == TriggerRequestOutcome::DroppedSuperseded);

    QNP_CHECK(!triggerStep(
        controller,
        combatFrame(1u, 12u, 0u, true, true, 0u, false, true),
        true, 0u, 1u).requestMoveStart);
    const TrainingTriggerStepResult latest = triggerStep(
        controller,
        combatFrame(1u, 13u, 0u, true, true, 0u, false, true),
        true, 0u, 1u);
    QNP_CHECK(latest.requestMoveStart);
    QNP_CHECK(latest.moveIndex == 1u);

    controller.resetTimeline(2u);
    hit = combatContact(1u, 1u, CombatContactKind::Hit);
    triggerContactStep(
        controller,
        combatFrame(2u, 1u, 0u, true, true, 22u, false, false),
        hit, true, 0u, 1u);
    QNP_CHECK(controller.status().pendingCount == 1u);
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::OnHit, TrainingTriggerConfig{}));
    QNP_CHECK(controller.status().pendingCount == 0u);
    QNP_CHECK(controller.status().lastOutcome
              == TriggerRequestOutcome::DroppedConfigurationChanged);
}

QNP_TEST(TrainingAfterBlockFiresOnceAtDummyResponseBoundary) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::AfterBlock,
                                     triggerConfig(2)));
    triggerStep(controller, combatFrame(1, 1, 0, true, true, 0, true, true));

    const CombatContactEvent block =
        combatContact(1, 2, CombatContactKind::Block);
    QNP_CHECK(!triggerContactStep(
        controller, combatFrame(1, 2, 70, false, false, 18, false, false),
        block).requestMacroStart);
    QNP_CHECK(!triggerStep(
        controller, combatFrame(1, 3, 70, false, true, 18, false, false))
                   .requestMacroStart);

    const TrainingTriggerStepResult result = triggerStep(
        controller, combatFrame(1, 4, 0, true, true, 0, false, true));
    QNP_CHECK(result.requestMacroStart);
    QNP_CHECK(result.macroSlotIndex == 1);
    QNP_CHECK(result.event == TrainingTriggerEvent::AfterBlock);
    QNP_CHECK(!triggerStep(
        controller, combatFrame(1, 5, 0, true, true, 0, true, true))
                   .requestMacroStart);
}

QNP_TEST(TrainingAfterHitstunSupportsNaturalExitAndRecoveryEntry) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::AfterHitstun,
                                     triggerConfig(3)));
    triggerStep(controller, combatFrame(1, 1, 0, true, true, 0, true, true));
    CombatContactEvent hit = combatContact(1, 2, CombatContactKind::Hit);
    triggerContactStep(
        controller, combatFrame(1, 2, 70, false, false, 22, false, false), hit);
    TrainingTriggerStepResult result = triggerStep(
        controller, combatFrame(1, 3, 0, true, true, 0, false, true));
    QNP_CHECK(result.requestMacroStart);
    QNP_CHECK(result.event == TrainingTriggerEvent::AfterHitstun);

    controller.resetTimeline(2);
    triggerStep(controller, combatFrame(2, 1, 0, true, true, 0, true, true));
    hit = combatContact(1, 2, CombatContactKind::Hit);
    triggerContactStep(
        controller, combatFrame(2, 2, 70, false, false, 38, false, false), hit);
    result = triggerStep(
        controller, combatFrame(2, 3, 70, false, false, 31, false, false));
    QNP_CHECK(result.requestMacroStart);
    QNP_CHECK(result.event == TrainingTriggerEvent::AfterHitstun);
    QNP_CHECK(result.macroSlotIndex == 2);
}

QNP_TEST(TrainingRecoveryWakeupAndThrowTechTriggersUseTypedEntries) {
    struct Case {
        TrainingTriggerEvent event;
        uint32_t pathAction;
        uint32_t resultAction;
    };
    const Case cases[] = {
        { TrainingTriggerEvent::OnWakeup, 35, 35 },
        { TrainingTriggerEvent::AfterAirRecovery, 31, 31 },
        { TrainingTriggerEvent::AfterGroundRecovery, 37, 33 },
        { TrainingTriggerEvent::AfterThrowTech, 73, 73 },
    };

    uint32_t generation = 10;
    for (const Case& testCase : cases) {
        TrainingTriggerController controller;
        QNP_REQUIRE(controller.setConfig(testCase.event, triggerConfig(1)));
        triggerStep(controller,
                    combatFrame(generation, 1, 0, true, true, 0, true, true));
        if (testCase.pathAction != testCase.resultAction) {
            QNP_CHECK(!triggerStep(
                controller, combatFrame(generation, 2, 0, true, true,
                                        testCase.pathAction, false, false))
                           .requestMacroStart);
        }
        const uint64_t entryStep =
            testCase.pathAction == testCase.resultAction ? 2 : 3;
        QNP_CHECK(!triggerStep(
            controller, combatFrame(generation, entryStep, 0, true, true,
                                    testCase.resultAction, false, false))
                       .requestMacroStart);
        const TrainingTriggerStepResult result = triggerStep(
            controller, combatFrame(generation, entryStep + 1, 0, true, true,
                                    0, false, true));
        QNP_CHECK(result.requestMacroStart);
        QNP_CHECK(result.event == testCase.event);
        ++generation;
    }

    TrainingTriggerController primed;
    QNP_REQUIRE(primed.setConfig(TrainingTriggerEvent::AfterThrowTech,
                                 triggerConfig(1)));
    triggerStep(primed, combatFrame(99, 50, 0, true, true, 73, false, false));
    QNP_CHECK(!triggerStep(
        primed, combatFrame(99, 51, 0, true, true, 0, false, true))
                   .requestMacroStart);

    TrainingTriggerController techPath;
    QNP_REQUIRE(techPath.setConfig(TrainingTriggerEvent::OnWakeup,
                                   triggerConfig(1)));
    QNP_REQUIRE(techPath.setConfig(TrainingTriggerEvent::AfterGroundRecovery,
                                   triggerConfig(2)));
    triggerStep(techPath, combatFrame(100, 1, 0, true, true, 0, true, true));
    triggerStep(techPath,
                combatFrame(100, 2, 0, true, true, 35, false, false));
    triggerStep(techPath,
                combatFrame(100, 3, 0, true, true, 33, false, false));
    const TrainingTriggerStepResult techResult = triggerStep(
        techPath, combatFrame(100, 4, 0, true, true, 0, false, true));
    QNP_CHECK(techResult.requestMacroStart);
    QNP_CHECK(techResult.event
              == TrainingTriggerEvent::AfterGroundRecovery);
}

QNP_TEST(TrainingWakeupPrebufferChanceIsDecidedBeforeInputHistoryChanges) {
    TrainingTriggerController accepted;
    QNP_REQUIRE(accepted.setConfig(
        TrainingTriggerEvent::OnWakeup, triggerMoveConfig(3u, 0u, 100u)));
    triggerStep(accepted, combatFrame(1u, 1u, 0u, true, true,
                                      0u, false, true));
    triggerStep(accepted, combatFrame(1u, 2u, 0u, true, true,
                                      35u, false, false));
    QNP_CHECK(accepted.wakeupPrebufferApproved(3u));
    QNP_CHECK(!accepted.wakeupPrebufferApproved(2u));
    const TrainingTriggerStepResult release = triggerStep(
        accepted, combatFrame(1u, 3u, 0u, true, true,
                              0u, false, true),
        true, 0u, uint64_t{1u} << 2u);
    QNP_CHECK(release.requestMoveStart);
    QNP_CHECK(release.moveIndex == 3u);
    QNP_CHECK(release.event == TrainingTriggerEvent::OnWakeup);

    TrainingTriggerController skipped;
    QNP_REQUIRE(skipped.setConfig(
        TrainingTriggerEvent::OnWakeup, triggerMoveConfig(3u, 0u, 0u)));
    triggerStep(skipped, combatFrame(2u, 1u, 0u, true, true,
                                     0u, false, true));
    triggerStep(skipped, combatFrame(2u, 2u, 0u, true, true,
                                     35u, false, false));
    QNP_CHECK(!skipped.wakeupPrebufferApproved(3u));
    const TrainingTriggerStepResult none = triggerStep(
        skipped, combatFrame(2u, 3u, 0u, true, true,
                             0u, false, true),
        true, 0u, uint64_t{1u} << 2u);
    QNP_CHECK(!none.requestMoveStart);
    QNP_CHECK(skipped.status().lastEvent
              == TrainingTriggerEvent::OnWakeup);
    QNP_CHECK(skipped.status().lastOutcome
              == TriggerRequestOutcome::ChanceSkipped);

    TrainingTriggerController delayed;
    QNP_REQUIRE(delayed.setConfig(
        TrainingTriggerEvent::OnWakeup, triggerMoveConfig(3u, 1u, 100u)));
    triggerStep(delayed, combatFrame(3u, 1u, 0u, true, true,
                                     0u, false, true));
    triggerStep(delayed, combatFrame(3u, 2u, 0u, true, true,
                                     35u, false, false));
    QNP_CHECK(!delayed.wakeupPrebufferApproved(3u));
}

QNP_TEST(AnAnsweredEpisodeDoesNotFireASecondTime) {
    TrainingTriggerController control;
    QNP_REQUIRE(control.setConfig(TrainingTriggerEvent::OnWakeup,
                                  triggerMoveConfig(3u, 0u, 100u)));
    triggerStep(control, combatFrame(1u, 1u, 0u, true, true, 0u, false, true));
    triggerStep(control,
                combatFrame(1u, 2u, 0u, true, true, 35u, false, false));
    const TrainingTriggerStepResult fired = triggerStep(
        control, combatFrame(1u, 3u, 0u, true, true, 0u, false, true),
        true, 0u, uint64_t{1u} << 2u);
    QNP_CHECK(fired.requestMoveStart);
    QNP_CHECK(fired.moveIndex == 3u);

    TrainingTriggerController answered;
    QNP_REQUIRE(answered.setConfig(TrainingTriggerEvent::OnWakeup,
                                   triggerMoveConfig(3u, 0u, 100u)));
    triggerStep(answered,
                combatFrame(1u, 1u, 0u, true, true, 0u, false, true));
    triggerStep(answered,
                combatFrame(1u, 2u, 0u, true, true, 35u, false, false));
    answered.markEventAnswered(TrainingTriggerEvent::OnWakeup, 2u);
    const TrainingTriggerStepResult late = triggerStep(
        answered, combatFrame(1u, 3u, 0u, true, true, 0u, false, true),
        true, 0u, uint64_t{1u} << 2u);
    QNP_CHECK(!late.requestMoveStart);

    triggerStep(answered,
                combatFrame(1u, 10u, 0u, true, true, 35u, false, false));
    const TrainingTriggerStepResult again = triggerStep(
        answered, combatFrame(1u, 11u, 0u, true, true, 0u, false, true),
        true, 0u, uint64_t{1u} << 2u);
    QNP_CHECK(again.requestMoveStart);
    QNP_CHECK(again.moveIndex == 3u);
}

QNP_TEST(TriggerJournalRecordsArmingSeparatelyFromFiring) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::AfterBlock,
                                     triggerMoveConfig(4u, 0u, 100u)));
    TrainingTriggerJournalEntry entries[kTrainingTriggerJournalCapacity];

    QNP_CHECK(controller.drainJournal(
                  entries, kTrainingTriggerJournalCapacity) == 0u);

    triggerStep(controller,
                combatFrame(2u, 1u, 0u, true, true, 0u, false, true));
    triggerContactStep(
        controller, combatFrame(2u, 2u, 0u, true, true, 18u, false, false),
        combatContact(1u, 2u, CombatContactKind::Block));

    uint32_t taken = controller.drainJournal(
        entries, kTrainingTriggerJournalCapacity);
    QNP_CHECK(taken > 0u);
    bool sawBlockLatch = false;
    for (uint32_t i = 0; i < taken; ++i) {
        if (entries[i].event == TrainingTriggerEvent::AfterBlock
            && entries[i].kind == TriggerJournalKind::Latched) {
            sawBlockLatch = true;
            QNP_CHECK(entries[i].cause == TriggerJournalCause::ContactBlock);
            QNP_CHECK(entries[i].moveIndex == 4u);
        }
    }
    QNP_CHECK(sawBlockLatch);

    QNP_CHECK(controller.drainJournal(
                  entries, kTrainingTriggerJournalCapacity) == 0u);

    const TrainingTriggerStepResult started = triggerStep(
        controller, combatFrame(2u, 3u, 0u, true, true, 0u, false, true),
        true, 0u, uint64_t{1u} << 3u);
    QNP_CHECK(started.requestMoveStart);
    taken = controller.drainJournal(entries, kTrainingTriggerJournalCapacity);
    bool sawOutcome = false;
    for (uint32_t i = 0; i < taken; ++i) {
        if (entries[i].event == TrainingTriggerEvent::AfterBlock
            && entries[i].kind == TriggerJournalKind::Outcome) {
            sawOutcome = true;
        }
    }
    QNP_CHECK(sawOutcome);

    TrainingTriggerController spare;
    QNP_REQUIRE(spare.setConfig(TrainingTriggerEvent::AfterBlock,
                                triggerMoveConfig(4u, 0u, 100u)));
    triggerStep(spare, combatFrame(5u, 1u, 0u, true, true, 0u, false, true));
    triggerContactStep(
        spare, combatFrame(5u, 2u, 0u, true, true, 18u, false, false),
        combatContact(1u, 2u, CombatContactKind::Block));
    QNP_CHECK(spare.drainJournal(nullptr, 0u) == 0u);
    QNP_CHECK(spare.drainJournal(entries, kTrainingTriggerJournalCapacity)
              == 0u);
    QNP_CHECK(spare.journalDropped() == 0u);
}

QNP_TEST(PrebufferCoversTheStunEventsAndNeverATech) {
    QNP_CHECK(TrainingTriggerController::prebufferEvent(
        TrainingTriggerEvent::OnWakeup));
    QNP_CHECK(TrainingTriggerController::prebufferEvent(
        TrainingTriggerEvent::AfterBlock));
    QNP_CHECK(TrainingTriggerController::prebufferEvent(
        TrainingTriggerEvent::AfterHitstun));

    QNP_CHECK(!TrainingTriggerController::prebufferEvent(
        TrainingTriggerEvent::AfterAirRecovery));
    QNP_CHECK(!TrainingTriggerController::prebufferEvent(
        TrainingTriggerEvent::AfterGroundRecovery));
    QNP_CHECK(!TrainingTriggerController::prebufferEvent(
        TrainingTriggerEvent::AfterThrowTech));
    QNP_CHECK(!TrainingTriggerController::prebufferEvent(
        TrainingTriggerEvent::OnHit));
    QNP_CHECK(!TrainingTriggerController::prebufferEvent(
        TrainingTriggerEvent::GuardCancel));

    QNP_CHECK(TrainingTriggerController::prebufferRestoresGuard(
        TrainingTriggerEvent::AfterBlock));
    QNP_CHECK(TrainingTriggerController::prebufferRestoresGuard(
        TrainingTriggerEvent::AfterHitstun));
    QNP_CHECK(!TrainingTriggerController::prebufferRestoresGuard(
        TrainingTriggerEvent::OnWakeup));
    QNP_CHECK(!TrainingTriggerController::prebufferRestoresGuard(
        TrainingTriggerEvent::AfterGroundRecovery));

    TrainingTriggerController block;
    QNP_REQUIRE(block.setConfig(TrainingTriggerEvent::AfterBlock,
                                triggerMoveConfig(4u, 0u, 100u)));
    triggerStep(block, combatFrame(2u, 1u, 0u, true, true, 0u, false, true));
    triggerContactStep(
        block, combatFrame(2u, 2u, 0u, true, true, 18u, false, false),
        combatContact(1u, 2u, CombatContactKind::Block));
    QNP_CHECK(block.prebufferApproved(TrainingTriggerEvent::AfterBlock, 4u));
    QNP_CHECK(!block.prebufferApproved(TrainingTriggerEvent::AfterBlock, 5u));
    const TrainingTriggerStepResult started = triggerStep(
        block, combatFrame(2u, 3u, 0u, true, true, 0u, false, true),
        true, 0u, uint64_t{1u} << 3u);
    QNP_CHECK(started.requestMoveStart);
    QNP_CHECK(started.moveIndex == 4u);
    QNP_CHECK(started.event == TrainingTriggerEvent::AfterBlock);

    TrainingTriggerController wake;
    QNP_REQUIRE(wake.setConfig(TrainingTriggerEvent::OnWakeup,
                               triggerMoveConfig(3u, 0u, 100u)));
    triggerStep(wake, combatFrame(1u, 1u, 0u, true, true, 0u, false, true));
    triggerStep(wake, combatFrame(1u, 2u, 0u, true, true, 35u, false, false));
    QNP_CHECK(wake.prebufferApproved(TrainingTriggerEvent::OnWakeup, 3u));
    QNP_CHECK(wake.wakeupPrebufferApproved(3u));
    QNP_CHECK(!wake.wakeupPrebufferApproved(2u));
}

QNP_TEST(TrainingRecoveryTriggersUseAnActionableEntryWithoutLosingAFrame) {
    const TrainingTriggerEvent events[] = {
        TrainingTriggerEvent::AfterAirRecovery,
        TrainingTriggerEvent::AfterGroundRecovery,
        TrainingTriggerEvent::AfterThrowTech,
    };
    const uint32_t actions[] = {31u, 33u, 73u};

    for (std::size_t index = 0; index < 3u; ++index) {
        TrainingTriggerController controller;
        QNP_REQUIRE(controller.setConfig(events[index], triggerConfig(1)));
        triggerStep(controller,
                    combatFrame(30u + static_cast<uint32_t>(index), 1,
                                0, true, true, 0, true, true));

        const TrainingTriggerStepResult result = triggerStep(
            controller,
            combatFrame(30u + static_cast<uint32_t>(index), 2,
                        0, true, true, actions[index], false, true));
        QNP_CHECK(result.requestMacroStart);
        QNP_CHECK(result.event == events[index]);
    }
}

QNP_TEST(TrainingSpecificTechTriggerOutranksTheOverlappingHitstunExit) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::AfterHitstun,
                                     triggerConfig(1)));
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::AfterAirRecovery,
                                     triggerConfig(2)));
    triggerStep(controller, combatFrame(40, 1, 0, true, true,
                                        0, true, true));
    const CombatContactEvent hit =
        combatContact(1, 2, CombatContactKind::Hit);
    triggerContactStep(controller, combatFrame(40, 2, 70, false, false,
                                               38, false, false), hit);

    const TrainingTriggerStepResult result = triggerStep(
        controller, combatFrame(40, 3, 70, false, false,
                                31, false, true));
    QNP_CHECK(result.requestMacroStart);
    QNP_CHECK(result.event == TrainingTriggerEvent::AfterAirRecovery);
    QNP_CHECK(result.macroSlotIndex == 1);
    QNP_CHECK(controller.status().startedCount == 1);
    QNP_CHECK(controller.status().droppedCount == 1);
    QNP_CHECK(controller.status().pendingCount == 0);
}

QNP_TEST(TrainingWakeupOwnsTheGenericHitstunEpisodeAcrossDelays) {
    {
        TrainingTriggerController controller;
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::AfterHitstun, triggerConfig(1u, 0u)));
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::OnWakeup, triggerConfig(2u, 2u)));
        triggerStep(controller,
                    combatFrame(41u, 1u, 0u, true, true,
                                0u, true, true));
        const CombatContactEvent hit =
            combatContact(1u, 2u, CombatContactKind::Hit);
        triggerContactStep(
            controller,
            combatFrame(41u, 2u, 70u, false, false,
                        22u, false, false),
            hit);
        triggerStep(controller,
                    combatFrame(41u, 3u, 70u, false, false,
                                35u, false, false));

        QNP_CHECK(!triggerStep(
            controller,
            combatFrame(41u, 4u, 70u, false, false,
                        0u, false, true)).requestMacroStart);
        QNP_CHECK(controller.status().pendingCount == 2u);
        QNP_CHECK(!triggerStep(
            controller,
            combatFrame(41u, 5u, 70u, false, false,
                        0u, false, true)).requestMacroStart);
        const TrainingTriggerStepResult wake = triggerStep(
            controller,
            combatFrame(41u, 6u, 70u, false, false,
                        0u, false, true));
        QNP_CHECK(wake.requestMacroStart);
        QNP_CHECK(wake.event == TrainingTriggerEvent::OnWakeup);
        QNP_CHECK(wake.macroSlotIndex == 1u);
        QNP_CHECK(controller.status().pendingCount == 0u);
        QNP_CHECK(controller.status().droppedCount == 1u);
    }

    {
        TrainingTriggerController controller;
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::AfterHitstun, triggerConfig(1u, 2u)));
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::OnWakeup, triggerConfig(2u, 0u)));
        triggerStep(controller,
                    combatFrame(42u, 1u, 0u, true, true,
                                0u, true, true));
        const CombatContactEvent hit =
            combatContact(1u, 2u, CombatContactKind::Hit);
        triggerContactStep(
            controller,
            combatFrame(42u, 2u, 70u, false, false,
                        22u, false, false),
            hit);
        triggerStep(controller,
                    combatFrame(42u, 3u, 70u, false, false,
                                35u, false, false));
        const TrainingTriggerStepResult wake = triggerStep(
            controller,
            combatFrame(42u, 4u, 70u, false, false,
                        0u, false, true));
        QNP_CHECK(wake.requestMacroStart);
        QNP_CHECK(wake.event == TrainingTriggerEvent::OnWakeup);
        QNP_CHECK(wake.macroSlotIndex == 1u);
        QNP_CHECK(controller.status().pendingCount == 0u);
        QNP_CHECK(controller.status().droppedCount == 1u);

        QNP_CHECK(!triggerStep(
            controller,
            combatFrame(42u, 6u, 70u, false, false,
                        0u, false, true)).requestMacroStart);
    }
}

QNP_TEST(TrainingUnavailableZeroDelayWakeupMoveFallsBackToHitstun) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::AfterHitstun, triggerConfig(1u)));
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::OnWakeup, triggerMoveConfig(3u)));
    triggerStep(controller,
                combatFrame(43u, 1u, 0u, true, true,
                            0u, true, true));
    const CombatContactEvent hit =
        combatContact(1u, 2u, CombatContactKind::Hit);
    triggerContactStep(
        controller,
        combatFrame(43u, 2u, 70u, false, false,
                    22u, false, false),
        hit, true, 0xFFu, uint64_t{1u} << 2u);
    triggerStep(controller,
                combatFrame(43u, 3u, 70u, false, false,
                            35u, false, false),
                true, 0xFFu, uint64_t{1u} << 2u);
    QNP_CHECK(controller.wakeupPrebufferApproved(3u));

    const TrainingTriggerStepResult fallback = triggerStep(
        controller,
        combatFrame(43u, 4u, 70u, false, false,
                    0u, false, true),
        true, 0xFFu, uint64_t{1u} << 2u, false);
    QNP_CHECK(fallback.requestMacroStart);
    QNP_CHECK(!fallback.requestMoveStart);
    QNP_CHECK(fallback.event == TrainingTriggerEvent::AfterHitstun);
    QNP_CHECK(fallback.macroSlotIndex == 0u);
    QNP_CHECK(controller.status().startedCount == 1u);
    QNP_CHECK(controller.status().droppedCount == 1u);
    QNP_CHECK(controller.status().pendingCount == 0u);
}

QNP_TEST(TrainingSameActionWakeRestartUsesEpochAndActorIdentity) {
    for (uint32_t identityCase = 0u; identityCase < 2u; ++identityCase) {
        TrainingTriggerController controller;
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::OnWakeup, triggerConfig(1u)));

        CombatFrameSample sample = combatFrame(
            44u + identityCase, 1u, 0u, true, true,
            0u, true, true);
        sample.owners[kCombatDummyOwner].actionEpoch = 10u;
        triggerStep(controller, sample);

        sample = combatFrame(
            44u + identityCase, 2u, 0u, true, true,
            35u, false, false);
        sample.owners[kCombatDummyOwner].actionEpoch = 11u;
        triggerStep(controller, sample);

        sample.stepSerial = 3u;
        if (identityCase == 0u) {
            sample.owners[kCombatDummyOwner].actionEpoch = 12u;
        } else {
            sample.owners[kCombatDummyOwner].actorToken = 99u;
        }
        const CombatContactEvent restart =
            combatContact(1u, 3u, CombatContactKind::Hit);
        triggerContactStep(controller, sample, restart);

        sample = combatFrame(
            44u + identityCase, 4u, 0u, true, true,
            0u, false, true);
        if (identityCase == 0u) {
            sample.owners[kCombatDummyOwner].actionEpoch = 13u;
        } else {
            sample.owners[kCombatDummyOwner].actorToken = 99u;
            sample.owners[kCombatDummyOwner].actionEpoch = 12u;
        }
        const TrainingTriggerStepResult wake = triggerStep(controller, sample);
        QNP_CHECK(wake.requestMacroStart);
        QNP_CHECK(wake.event == TrainingTriggerEvent::OnWakeup);
        QNP_CHECK(wake.macroSlotIndex == 0u);
    }
}

QNP_TEST(TrainingNaturalWakeupRearmsWhenANewerHitKeepsTheSameAction) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::AfterHitstun, triggerConfig(1u)));
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::OnWakeup, triggerMoveConfig(3u)));

    triggerStep(controller,
                combatFrame(46u, 1u, 0u, true, true,
                            0u, true, true));
    CombatContactEvent hit =
        combatContact(1u, 2u, CombatContactKind::Hit);
    triggerContactStep(
        controller,
        combatFrame(46u, 2u, 70u, false, false,
                    25u, false, false),
        hit, true, 0xFFu, uint64_t{1u} << 2u);
    QNP_CHECK(controller.wakeupPrebufferApproved(3u));

    hit = combatContact(2u, 3u, CombatContactKind::Hit);
    triggerContactStep(
        controller,
        combatFrame(46u, 3u, 70u, false, false,
                    25u, false, false),
        hit, true, 0xFFu, uint64_t{1u} << 2u);
    QNP_CHECK(controller.wakeupPrebufferApproved(3u));

    const TrainingTriggerStepResult wake = triggerStep(
        controller,
        combatFrame(46u, 4u, 70u, false, false,
                    0u, false, true),
        true, 0xFFu, uint64_t{1u} << 2u, true);
    QNP_CHECK(wake.requestMoveStart);
    QNP_CHECK(!wake.requestMacroStart);
    QNP_CHECK(wake.event == TrainingTriggerEvent::OnWakeup);
    QNP_CHECK(wake.moveIndex == 3u);
    QNP_CHECK(controller.status().pendingCount == 0u);
    QNP_CHECK(controller.status().droppedCount == 1u);
}

QNP_TEST(TrainingTriggerDelayAndChanceAreDeterministic) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1, 2, 100)));
    CombatContactEvent hit = combatContact(1, 10, CombatContactKind::Hit);
    QNP_CHECK(!triggerContactStep(
        controller, combatFrame(7, 10, 0, true, true, 22, false, false), hit)
                   .requestMacroStart);
    QNP_CHECK(controller.status().pendingCount == 1);
    QNP_CHECK(!triggerStep(
        controller, combatFrame(7, 11, 0, true, true, 22, false, false))
                   .requestMacroStart);
    QNP_CHECK(triggerStep(
        controller, combatFrame(7, 12, 0, true, true, 22, false, false))
                  .requestMacroStart);

    controller.resetTimeline(8);
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1, 0, 0)));
    hit = combatContact(1, 20, CombatContactKind::Hit);
    QNP_CHECK(!triggerContactStep(
        controller, combatFrame(8, 20, 0, true, true, 22, false, false), hit)
                   .requestMacroStart);
    QNP_CHECK(controller.status().chanceSkippedCount == 1);
    QNP_CHECK(controller.status().lastOutcome
              == TriggerRequestOutcome::ChanceSkipped);

    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1, 0, 37)));
    controller.resetTimeline(123);
    hit = combatContact(55, 80, CombatContactKind::Hit);
    const bool first = triggerContactStep(
        controller, combatFrame(123, 80, 0, true, true, 22, false, false), hit)
                           .requestMacroStart;
    controller.resetTimeline(123);
    const bool second = triggerContactStep(
        controller, combatFrame(123, 80, 0, true, true, 22, false, false), hit)
                            .requestMacroStart;
    QNP_CHECK(first == second);
}

QNP_TEST(TrainingGuardCancelUsesNativeWindowAndEveryBlockRearmsIt) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::GuardCancel,
                                     triggerConfig(4)));
    CombatFrameSample sample =
        combatFrame(1, 1, 0, true, true, 0, true, true);
    triggerStep(controller, sample);

    CombatContactEvent block =
        combatContact(1, 2, CombatContactKind::Block);
    sample = combatFrame(1, 2, 70, false, false, 18, false, false);
    sample.owners[kCombatDummyOwner].guardCancelReady = false;
    QNP_CHECK(!triggerContactStep(controller, sample, block).requestMacroStart);

    sample = combatFrame(1, 3, 70, false, false, 18, false, false);
    sample.owners[kCombatDummyOwner].guardCancelReady = false;
    QNP_CHECK(!triggerStep(controller, sample).requestMacroStart);
    sample = combatFrame(1, 4, 70, false, false, 18, false, false);
    sample.owners[kCombatDummyOwner].guardCancelReady = true;
    TrainingTriggerStepResult result = triggerStep(controller, sample);
    QNP_CHECK(result.requestMacroStart);
    QNP_CHECK(result.event == TrainingTriggerEvent::GuardCancel);
    QNP_CHECK(result.macroSlotIndex == 3);

    sample.stepSerial = 5;
    QNP_CHECK(!triggerStep(controller, sample).requestMacroStart);

    block = combatContact(2, 7, CombatContactKind::Block);
    sample.stepSerial = 7;
    result = triggerContactStep(controller, sample, block);
    QNP_CHECK(result.requestMacroStart);
    QNP_CHECK(result.event == TrainingTriggerEvent::GuardCancel);
    QNP_CHECK(controller.status().startedCount == 2);
}

QNP_TEST(TrainingGuardCancelOwnsAnOverlappingAfterBlockBoundary) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::AfterBlock, triggerConfig(1u)));
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::GuardCancel, triggerConfig(2u, 1u)));
    triggerStep(controller,
                combatFrame(3u, 1u, 0u, true, true,
                            0u, true, true));

    CombatFrameSample guard = combatFrame(
        3u, 2u, 70u, false, false, 18u, false, false);
    guard.owners[kCombatDummyOwner].guardCancelReady = true;
    const CombatContactEvent block =
        combatContact(1u, 2u, CombatContactKind::Block);
    QNP_CHECK(!triggerContactStep(controller, guard, block)
                   .requestMacroStart);

    guard.stepSerial = 3u;
    guard.owners[kCombatDummyOwner].responseReady = true;
    const TrainingTriggerStepResult result = triggerStep(controller, guard);
    QNP_CHECK(result.requestMacroStart);
    QNP_CHECK(result.event == TrainingTriggerEvent::GuardCancel);
    QNP_CHECK(result.macroSlotIndex == 1u);
    QNP_CHECK(controller.status().droppedCount == 1u);
    QNP_CHECK(controller.status().pendingCount == 0u);

    QNP_CHECK(!triggerStep(
        controller,
        combatFrame(3u, 4u, 70u, false, false,
                    0u, false, true)).requestMacroStart);
}

QNP_TEST(TrainingGuardCancelDoesNotSurviveGuardExitWithoutPermission) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::GuardCancel,
                                     triggerConfig(1)));
    CombatFrameSample sample =
        combatFrame(2, 1, 0, true, true, 0, true, true);
    triggerStep(controller, sample);

    CombatContactEvent block =
        combatContact(1, 2, CombatContactKind::Block);
    sample = combatFrame(2, 2, 70, false, false, 18, false, false);
    QNP_CHECK(!triggerContactStep(controller, sample, block).requestMacroStart);

    sample = combatFrame(2, 3, 70, false, false, 21, false, false);
    QNP_CHECK(!triggerStep(controller, sample).requestMacroStart);
    sample = combatFrame(2, 4, 70, false, false, 18, false, false);
    sample.owners[kCombatDummyOwner].guardCancelReady = true;
    QNP_CHECK(!triggerStep(controller, sample).requestMacroStart);
}

QNP_TEST(TrainingGuardCancelDelayCannotBecomeAPostBlockAction) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::GuardCancel, triggerConfig(1u, 2u)));
    triggerStep(controller, combatFrame(1u, 1u, 0u, true, true,
                                        0u, true, true));

    CombatContactEvent block =
        combatContact(1u, 2u, CombatContactKind::Block);
    CombatFrameSample guard = combatFrame(
        1u, 2u, 70u, false, false, 18u, false, false);
    guard.owners[kCombatDummyOwner].guardCancelReady = true;
    QNP_CHECK(!triggerContactStep(controller, guard, block)
                   .requestMacroStart);
    QNP_CHECK(controller.status().pendingCount == 1u);

    const TrainingTriggerStepResult leftGuard = triggerStep(
        controller,
        combatFrame(1u, 3u, 0u, true, true, 0u, false, true));
    QNP_CHECK(!leftGuard.requestMacroStart);
    QNP_CHECK(controller.status().pendingCount == 0u);
    QNP_CHECK(controller.status().lastOutcome
              == TriggerRequestOutcome::DroppedWindowClosed);
}

QNP_TEST(TrainingDelayedRecoveryCannotCrossANewerCombatEpisode) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::AfterBlock, triggerConfig(1u, 10u)));
    triggerStep(controller, combatFrame(1u, 1u, 0u, true, true,
                                        0u, true, true));

    CombatContactEvent block =
        combatContact(1u, 2u, CombatContactKind::Block);
    QNP_CHECK(!triggerContactStep(
        controller,
        combatFrame(1u, 2u, 70u, false, false, 18u, false, false),
        block).requestMacroStart);
    QNP_CHECK(!triggerStep(
        controller,
        combatFrame(1u, 3u, 70u, false, false, 0u, false, true))
                   .requestMacroStart);
    QNP_CHECK(controller.status().pendingCount == 1u);

    const CombatContactEvent hit =
        combatContact(2u, 4u, CombatContactKind::Hit);
    QNP_CHECK(!triggerContactStep(
        controller,
        combatFrame(1u, 4u, 70u, false, false, 22u, false, false),
        hit).requestMacroStart);
    QNP_CHECK(controller.status().pendingCount == 0u);
    QNP_CHECK(controller.status().lastOutcome
              == TriggerRequestOutcome::DroppedSuperseded);
    QNP_CHECK(!triggerStep(
        controller,
        combatFrame(1u, 13u, 70u, false, false, 22u, false, false))
                   .requestMacroStart);
}

QNP_TEST(TrainingDelayedCharacterMoveStillRequiresNativeResponsePermission) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::AfterBlock, triggerMoveConfig(2u, 1u)));
    triggerStep(controller, combatFrame(1u, 1u, 0u, true, true,
                                        0u, true, true), true, 0xFFu,
                uint64_t{1u} << 1u);

    const CombatContactEvent block =
        combatContact(1u, 2u, CombatContactKind::Block);
    triggerContactStep(
        controller,
        combatFrame(1u, 2u, 70u, false, false, 18u, false, false),
        block, true, 0xFFu, uint64_t{1u} << 1u);
    triggerStep(controller,
                combatFrame(1u, 3u, 70u, false, false, 0u, false, true),
                true, 0xFFu, uint64_t{1u} << 1u);

    const TrainingTriggerStepResult locked = triggerStep(
        controller,
        combatFrame(1u, 4u, 70u, false, false, 29u, false, false),
        true, 0xFFu, uint64_t{1u} << 1u);
    QNP_CHECK(!locked.requestMoveStart);
    QNP_CHECK(controller.status().pendingCount == 1u);

    const TrainingTriggerStepResult ready = triggerStep(
        controller,
        combatFrame(1u, 5u, 70u, false, false, 0u, false, true),
        true, 0xFFu, uint64_t{1u} << 1u);
    QNP_CHECK(ready.requestMoveStart);
    QNP_CHECK(ready.moveIndex == 2u);
    QNP_CHECK(ready.event == TrainingTriggerEvent::AfterBlock);
}

QNP_TEST(TrainingTriggerDropsBusyEmptyAndCollidingRequestsExplicitly) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1)));
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    TrainingTriggerStepResult result = triggerContactStep(
        controller, combatFrame(1, 1, 0, true, true, 22, false, false), hit,
        false, 0xFFu);
    QNP_CHECK(!result.requestMacroStart);
    QNP_CHECK(result.outcome == TriggerRequestOutcome::DroppedBusy);
    QNP_CHECK(controller.status().droppedCount == 1);

    controller.resetTimeline(2);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    result = triggerContactStep(
        controller, combatFrame(2, 1, 0, true, true, 22, false, false), hit,
        true, 0x00u);
    QNP_CHECK(!result.requestMacroStart);
    QNP_CHECK(result.outcome == TriggerRequestOutcome::DroppedEmptySlot);
    QNP_CHECK(controller.status().droppedCount == 1);

    controller.resetTimeline(3);
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::OnHit, triggerMoveConfig(2u)));
    hit = combatContact(1, 1, CombatContactKind::Hit);
    result = triggerContactStep(
        controller, combatFrame(3, 1, 0, true, true, 0, false, true), hit,
        true, 0u, 0u);
    QNP_CHECK(!result.requestMoveStart);
    QNP_CHECK(result.outcome
              == TriggerRequestOutcome::DroppedUnavailableMove);
    QNP_CHECK(controller.status().lastMoveIndex == 2u);

    controller.resetTimeline(4);
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1, 2)));
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::AfterBlock,
                                     triggerConfig(2)));
    triggerStep(controller, combatFrame(4, 1, 0, true, true, 0, true, true));
    hit = combatContact(1, 2, CombatContactKind::Hit);
    triggerContactStep(
        controller, combatFrame(4, 2, 70, false, false, 22, false, false), hit);
    const CombatContactEvent block =
        combatContact(2, 3, CombatContactKind::Block);
    triggerContactStep(
        controller, combatFrame(4, 3, 70, false, false, 18, false, false),
        block);
    result = triggerStep(
        controller, combatFrame(4, 4, 0, true, true, 0, false, true));
    QNP_CHECK(result.requestMacroStart);
    QNP_CHECK(result.event == TrainingTriggerEvent::AfterBlock);
    QNP_CHECK(result.macroSlotIndex == 1u);
    QNP_CHECK(controller.status().startedCount == 1);
    QNP_CHECK(controller.status().droppedCount == 1);
    QNP_CHECK(controller.status().pendingCount == 0);
}

QNP_TEST(TrainingTriggerNewestCombatEpisodeSupersedesPendingAndResets) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1, 240)));
    for (uint64_t index = 1; index <= kTriggerPendingCapacity + 1; ++index) {
        const CombatContactEvent hit =
            combatContact(index, index, CombatContactKind::Hit);
        triggerContactStep(
            controller, combatFrame(1, index, 0, true, true, 22, false, false),
            hit);
    }
    QNP_CHECK(controller.status().pendingCount == 1u);
    QNP_CHECK(controller.status().droppedCount
              == kTriggerPendingCapacity);
    QNP_CHECK(controller.status().lastOutcome
              == TriggerRequestOutcome::DroppedSuperseded);

    triggerStep(controller, combatFrame(2, 1, 0, true, true, 0, true, true));
    QNP_CHECK(controller.status().trainingGeneration == 2);
    QNP_CHECK(controller.status().pendingCount == 0);
    QNP_CHECK(controller.status().droppedCount == 0);
    QNP_CHECK(controller.config(TrainingTriggerEvent::OnHit).macroSlot == 1);
}

QNP_TEST(TrainingTriggerRoundAndUnreadableBoundariesDiscardTimelineWork) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::OnHit, triggerConfig(1, 2)));

    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    triggerContactStep(
        controller, combatFrame(1, 1, 0, true, true, 22, false, false),
        hit);
    QNP_CHECK(controller.status().pendingCount == 1);

    CombatFrameSample ended =
        combatFrame(1, 2, 0, true, true, 22, false, false);
    ended.roundValid = false;
    QNP_CHECK(!triggerStep(controller, ended).requestMacroStart);
    QNP_CHECK(controller.status().pendingCount == 0);

    QNP_CHECK(!triggerStep(
        controller, combatFrame(1, 3, 0, true, true, 0, true, true))
                   .requestMacroStart);

    hit = combatContact(2, 4, CombatContactKind::Hit);
    triggerContactStep(
        controller, combatFrame(1, 4, 0, true, true, 22, false, false),
        hit);
    QNP_CHECK(controller.status().pendingCount == 1);
    CombatFrameSample unreadable =
        combatFrame(1, 5, 0, true, true, 22, false, false);
    unreadable.readable = false;
    QNP_CHECK(!triggerStep(controller, unreadable).requestMacroStart);
    QNP_CHECK(controller.status().pendingCount == 0);
    QNP_CHECK(controller.config(TrainingTriggerEvent::OnHit).macroSlot == 1);
}

QNP_TEST(TrainingTriggerTimelineResetCanPreserveSessionCounters) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1)));

    CombatContactEvent contact =
        combatContact(1, 1, CombatContactKind::Hit);
    QNP_CHECK(triggerContactStep(
        controller, combatFrame(1, 1, 0, true, true, 22, false, false),
        contact).requestMacroStart);

    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1, 0, 0)));
    contact = combatContact(2, 2, CombatContactKind::Hit);
    triggerContactStep(
        controller, combatFrame(1, 2, 0, true, true, 22, false, false),
        contact);

    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnHit,
                                     triggerConfig(1)));
    contact = combatContact(3, 3, CombatContactKind::Hit);
    triggerContactStep(
        controller, combatFrame(1, 3, 0, true, true, 22, false, false),
        contact, false);

    contact = combatContact(4, 4, CombatContactKind::Hit, 0, 0);
    triggerContactStep(
        controller, combatFrame(1, 4, 0, true, true, 22, false, false),
        contact);
    QNP_CHECK(controller.status().startedCount == 1);
    QNP_CHECK(controller.status().chanceSkippedCount == 1);
    QNP_CHECK(controller.status().droppedCount == 1);
    QNP_CHECK(controller.status().invalidContactCount == 1);

    controller.resetTimeline(2, true);
    QNP_CHECK(controller.status().trainingGeneration == 2);
    QNP_CHECK(controller.status().pendingCount == 0);
    QNP_CHECK(controller.status().startedCount == 1);
    QNP_CHECK(controller.status().chanceSkippedCount == 1);
    QNP_CHECK(controller.status().droppedCount == 1);
    QNP_CHECK(controller.status().invalidContactCount == 1);
    QNP_CHECK(controller.config(TrainingTriggerEvent::OnHit).macroSlot == 1);

    controller.resetTimeline(3, false);
    QNP_CHECK(controller.status().startedCount == 0);
    QNP_CHECK(controller.status().chanceSkippedCount == 0);
    QNP_CHECK(controller.status().droppedCount == 0);
    QNP_CHECK(controller.status().invalidContactCount == 0);
}

QNP_TEST(TrainingTriggeredMacroStartsImmediatelyOnceAndPreservesSelection) {
    TrainingMacros macros;
    macros.reset();
    uint64_t step = 1;
    QNP_REQUIRE(recordOneFrame(macros, step, 0, qinput::kLogicalA));
    QNP_REQUIRE(recordOneFrame(macros, step, 1, qinput::kLogicalB));
    QNP_CHECK(macros.selectedSlot() == 1);
    QNP_REQUIRE(macros.setPlaybackMode(PlaybackMode::Loop));

    QNP_REQUIRE(macros.requestTriggeredPlayback(0));
    QNP_CHECK(macros.state() == MacroState::Playing);
    QNP_CHECK(macros.selectedSlot() == 1);
    QNP_CHECK(macros.playbackMode() == PlaybackMode::Loop);

    MacroStepInput input{};
    MacroStepResult result = macros.advance(step++, input);
    QNP_CHECK(result.playbackFrame);
    QNP_CHECK(result.overrideP2);
    QNP_CHECK(result.p2Mask == qinput::kLogicalA);
    QNP_CHECK(result.state == MacroState::PlayStopping);
    result = macros.advance(step++, input);
    QNP_CHECK(result.overrideP2);
    QNP_CHECK(result.p2Mask == 0);
    QNP_CHECK(result.state == MacroState::Idle);
    QNP_CHECK(macros.selectedSlot() == 1);
    QNP_CHECK(macros.playbackMode() == PlaybackMode::Loop);

    QNP_REQUIRE(macros.requestPlaybackToggle());
    QNP_CHECK(macros.advance(step++, input).state == MacroState::Playing);
    result = macros.advance(step++, input);
    QNP_CHECK(result.playbackFrame);
    QNP_CHECK(result.p2Mask == qinput::kLogicalB);
    QNP_CHECK(result.state == MacroState::Playing);
}

QNP_TEST(TrainingRandomPlaybackPlaysARecordedSlotWhateverIsSelected) {
    TrainingMacros macros;
    macros.reset();
    uint64_t step = 1;
    QNP_REQUIRE(recordOneFrame(macros, step, 0, qinput::kLogicalA));
    QNP_REQUIRE(recordOneFrame(macros, step, 2, qinput::kLogicalC));
    QNP_REQUIRE(macros.selectSlot(5));

    QNP_CHECK(!macros.requestPlaybackToggle());

    QNP_REQUIRE(macros.setPlaybackMode(PlaybackMode::Random));
    QNP_REQUIRE(macros.requestPlaybackToggle());
    QNP_CHECK(macros.activePlaybackSlot() == 0u
              || macros.activePlaybackSlot() == 2u);
    MacroStepInput input{};
    QNP_CHECK(macros.advance(step++, input).state == MacroState::Playing);
    MacroStepResult result = macros.advance(step++, input);
    QNP_CHECK(result.playbackFrame);
    QNP_CHECK(result.p2Mask == qinput::kLogicalA
              || result.p2Mask == qinput::kLogicalC);
    QNP_CHECK(result.state == MacroState::PlayStopping);
    QNP_CHECK(macros.advance(step++, input).state == MacroState::Idle);
    QNP_CHECK(macros.selectedSlot() == 5u);
}

QNP_TEST(TrainingRandomLoopPicksAgainEveryRunAndOnlyRecordedSlots) {
    TrainingMacros macros;
    macros.reset();
    uint64_t step = 1;
    QNP_REQUIRE(recordOneFrame(macros, step, 1, qinput::kLogicalA));
    QNP_REQUIRE(recordOneFrame(macros, step, 6, qinput::kLogicalB));
    QNP_REQUIRE(macros.setPlaybackMode(PlaybackMode::RandomLoop));
    QNP_REQUIRE(macros.requestPlaybackToggle());
    MacroStepInput input{};
    QNP_CHECK(macros.advance(step++, input).state == MacroState::Playing);

    uint32_t seenA = 0u;
    uint32_t seenB = 0u;
    for (uint32_t run = 0u; run < 200u; ++run) {
        const MacroStepResult result = macros.advance(step++, input);
        QNP_CHECK(result.playbackFrame);
        QNP_CHECK(result.state == MacroState::Playing);
        if (result.p2Mask == qinput::kLogicalA) ++seenA;
        else if (result.p2Mask == qinput::kLogicalB) ++seenB;
        else QNP_CHECK(false);
    }
    QNP_CHECK(seenA >= 50u && seenB >= 50u);
}

QNP_TEST(TrainingRandomPlaybackRefusesWhenNothingIsRecorded) {
    TrainingMacros macros;
    macros.reset();
    QNP_REQUIRE(macros.setPlaybackMode(PlaybackMode::Random));
    QNP_CHECK(!macros.requestPlaybackToggle());
    QNP_REQUIRE(macros.setPlaybackMode(PlaybackMode::RandomLoop));
    QNP_CHECK(!macros.requestPlaybackToggle());
    QNP_CHECK(macros.state() == MacroState::Idle);
    QNP_CHECK(!macros.setPlaybackMode(static_cast<PlaybackMode>(4)));
}

QNP_TEST(TrainingRandomRecordingTriggerStartsOneOfTheRecordedSlots) {
    constexpr uint8_t kRecorded = (1u << 2) | (1u << 5);
    uint32_t seen3 = 0u;
    uint32_t seen6 = 0u;
    for (uint32_t generation = 1u; generation <= 64u; ++generation) {
        TrainingTriggerController controller;
        QNP_REQUIRE(controller.setConfig(
            TrainingTriggerEvent::OnHit,
            triggerConfig(kTrainingTriggerRandomMacroSlot)));
        const CombatContactEvent hit =
            combatContact(1, 10, CombatContactKind::Hit);
        const TrainingTriggerStepResult result = triggerContactStep(
            controller,
            combatFrame(generation, 10, 70, true, true, 22, false, false),
            hit, true, kRecorded);
        QNP_CHECK(result.requestMacroStart);
        if (result.macroSlotIndex == 2u) ++seen3;
        else if (result.macroSlotIndex == 5u) ++seen6;
        else QNP_CHECK(false);
    }
    QNP_CHECK(seen3 > 0u && seen6 > 0u);

    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(
        TrainingTriggerEvent::OnHit,
        triggerConfig(kTrainingTriggerRandomMacroSlot)));
    const CombatContactEvent hit =
        combatContact(1, 10, CombatContactKind::Hit);
    const TrainingTriggerStepResult result = triggerContactStep(
        controller, combatFrame(99, 10, 70, true, true, 22, false, false),
        hit, true, 0u);
    QNP_CHECK(!result.requestMacroStart);
    QNP_CHECK(controller.status().droppedCount == 1u);
}

QNP_TEST(TrainingTriggeredMacroRejectsEmptyInvalidAndBusyRequests) {
    TrainingMacros macros;
    macros.reset();
    uint64_t step = 1;
    QNP_REQUIRE(recordOneFrame(macros, step, 0, qinput::kLogicalC));
    QNP_CHECK(!macros.requestTriggeredPlayback(kTrainingMacroSlotCount));
    QNP_CHECK(!macros.requestTriggeredPlayback(1));
    QNP_REQUIRE(macros.requestTriggeredPlayback(0));
    QNP_CHECK(!macros.requestTriggeredPlayback(0));

    MacroStepInput input{};
    input.paused = true;
    MacroStepResult result = macros.advance(step++, input);
    QNP_CHECK(result.overrideP2);
    QNP_CHECK(!result.playbackFrame);
    QNP_CHECK(result.state == MacroState::Playing);

    input.paused = false;
    result = macros.advance(step++, input);
    QNP_CHECK(result.playbackFrame);
    QNP_CHECK(result.p2Mask == qinput::kLogicalC);
    QNP_CHECK(result.state == MacroState::PlayStopping);
}

QNP_TEST(FrameAdvantageUsesNaturalAttackerAndResponseDefenderReadiness) {
    FrameAdvantageTracker tracker;
    CombatContactEvent hit = combatContact(1, 10, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 10, 70, true, true, 22, false, false), &hit, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().attackerReady);
    QNP_CHECK(!tracker.status().defenderReady);
    QNP_CHECK(tracker.status().attackerReadyDelay == 0);

    for (uint64_t step = 11; step < 15; ++step) {
        tracker.advance(
            combatFrame(1, step, 70, true, true, 22, false, false),
            nullptr, 0);
    }
    tracker.advance(
        combatFrame(1, 15, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().advantageValid);
    QNP_CHECK(tracker.status().advantageFrames == 5);

    tracker.resetTimeline(2);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(2, 1, 70, false, true, 22, false, false), &hit, 1);
    QNP_CHECK(!tracker.status().attackerReady);
    tracker.advance(
        combatFrame(2, 2, 70, false, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(2, 3, 70, false, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(2, 4, 0, true, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(2, 5, 0, true, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(2, 6, 0, true, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(2, 7, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().advantageFrames == 3);

    tracker.resetTimeline(3);
    hit = combatContact(1, 20, CombatContactKind::Hit, 0, 1, false);
    tracker.advance(
        combatFrame(3, 20, 70, false, true, 22, false, false), &hit, 1);
    QNP_CHECK(tracker.status().attackerReady);
    QNP_CHECK(!tracker.status().attackerIsPrimary);
    QNP_CHECK(tracker.status().attackerReadyDelay == 0);
    for (uint64_t step = 21; step < 24; ++step) {
        tracker.advance(
            combatFrame(3, step, 70, false, true, 22, false, false),
            nullptr, 0);
    }
    tracker.advance(
        combatFrame(3, 24, 70, false, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().advantageFrames == 4);
}

QNP_TEST(FrameAdvantageSignsZeroAndBlockKindAreExact) {
    FrameAdvantageTracker tracker;
    CombatContactEvent contact =
        combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 70, false, false, 18, false, true), &contact, 1);
    tracker.advance(
        combatFrame(1, 2, 70, false, false, 18, false, true), nullptr, 0);
    tracker.advance(
        combatFrame(1, 3, 70, false, false, 0, false, true), nullptr, 0);
    tracker.advance(
        combatFrame(1, 4, 70, false, false, 0, false, true), nullptr, 0);
    tracker.advance(
        combatFrame(1, 5, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().contactKind == CombatContactKind::Block);
    QNP_CHECK(tracker.status().advantageFrames == -3);

    tracker.resetTimeline(2);
    contact = combatContact(1, 10, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(2, 10, 70, false, true, 22, false, true), &contact, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    tracker.advance(
        combatFrame(2, 11, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().advantageFrames == 0);
}

QNP_TEST(FrameAdvantageLatestMultihitContactRestartsTheEpisode) {
    FrameAdvantageTracker tracker;
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, true, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(1, 2, 70, true, true, 22, false, false), nullptr, 0);

    hit = combatContact(2, 3, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 3, 70, false, true, 22, false, false), &hit, 1);
    QNP_CHECK(!tracker.status().attackerReady);
    QNP_CHECK(tracker.status().contactSerial == 2);
    QNP_CHECK(tracker.status().contactStep == 3);
    tracker.advance(
        combatFrame(1, 4, 70, false, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(1, 5, 0, true, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(1, 6, 0, true, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(1, 7, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().attackerReadyDelay == 2);
    QNP_CHECK(tracker.status().defenderReadyDelay == 4);
    QNP_CHECK(tracker.status().advantageFrames == 2);
    QNP_CHECK(tracker.status().completedCount == 1);
}

QNP_TEST(FrameAdvantageOppositeContactsAreTradeUntilANewContact) {
    FrameAdvantageTracker tracker;
    const CombatContactEvent contacts[] = {
        combatContact(1, 10, CombatContactKind::Hit, 0, 1),
        combatContact(2, 10, CombatContactKind::Hit, 1, 0),
    };
    tracker.advance(
        combatFrame(1, 10, 70, true, true, 70, true, true), contacts, 2);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Trade);
    QNP_CHECK(!tracker.status().advantageValid);
    QNP_CHECK(tracker.status().tradeCount == 1);

    const CombatContactEvent later =
        combatContact(3, 12, CombatContactKind::Hit, 0, 1);
    tracker.advance(
        combatFrame(1, 12, 0, true, true, 22, false, false), &later, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().contactSerial == 3);
}

QNP_TEST(FrameAdvantageTimelineResetCanPreserveSessionCounters) {
    FrameAdvantageTracker tracker(2);

    CombatContactEvent contact =
        combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, false, false, 22, false, false), &contact, 1);
    tracker.advance(
        combatFrame(1, 2, 70, false, false, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(1, 3, 70, false, false, 22, false, false), nullptr, 0);
    QNP_CHECK(tracker.status().timeoutCount == 1);

    tracker.resetTimeline(1, true);
    const CombatContactEvent trade[] = {
        combatContact(1, 4, CombatContactKind::Hit, 0, 1),
        combatContact(2, 4, CombatContactKind::Hit, 1, 0),
    };
    tracker.advance(
        combatFrame(1, 4, 70, true, true, 70, true, true), trade, 2);
    QNP_CHECK(tracker.status().tradeCount == 1);
    QNP_CHECK(tracker.status().timeoutCount == 1);

    tracker.resetTimeline(1, true);
    contact = combatContact(1, 5, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 5, 70, true, true, 18, false, false), &contact, 1);
    tracker.advance(
        combatFrame(1, 6, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().completedCount == 1);

    contact = combatContact(2, 7, CombatContactKind::Hit, 0, 0);
    tracker.advance(
        combatFrame(1, 7, 0, true, true, 0, true, true), &contact, 1);
    QNP_CHECK(tracker.status().invalidContactCount == 1);

    tracker.resetTimeline(2, true);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);
    QNP_CHECK(tracker.status().completedCount == 1);
    QNP_CHECK(tracker.status().tradeCount == 1);
    QNP_CHECK(tracker.status().timeoutCount == 1);
    QNP_CHECK(tracker.status().invalidContactCount == 1);

    tracker.resetTimeline(3, false);
    QNP_CHECK(tracker.status().completedCount == 0);
    QNP_CHECK(tracker.status().tradeCount == 0);
    QNP_CHECK(tracker.status().timeoutCount == 0);
    QNP_CHECK(tracker.status().invalidContactCount == 0);
}

QNP_TEST(FrameAdvantageTimeoutDisableAndGenerationResetAreBounded) {
    FrameAdvantageTracker tracker(3);
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, false, false, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(1, 2, 70, false, false, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(1, 3, 70, false, false, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(1, 4, 70, false, false, 22, false, false), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::TimedOut);
    QNP_CHECK(tracker.status().timeoutCount == 1);

    tracker.advance(
        combatFrame(2, 1, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().trainingGeneration == 2);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);
    QNP_CHECK(tracker.status().timeoutCount == 0);

    tracker.setEnabled(false);
    hit = combatContact(1, 2, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(2, 2, 0, true, true, 22, false, false), &hit, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);
    tracker.setEnabled(true);
    tracker.advance(
        combatFrame(2, 3, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);
}

QNP_TEST(FrameAdvantagePublicStatesRemainAppendCompatible) {
    QNP_CHECK(static_cast<uint8_t>(FrameAdvantageState::Idle) == 0);
    QNP_CHECK(static_cast<uint8_t>(FrameAdvantageState::Measuring) == 1);
    QNP_CHECK(static_cast<uint8_t>(FrameAdvantageState::Complete) == 2);
    QNP_CHECK(static_cast<uint8_t>(FrameAdvantageState::Trade) == 3);
    QNP_CHECK(static_cast<uint8_t>(FrameAdvantageState::TimedOut) == 4);
    QNP_CHECK(static_cast<uint8_t>(FrameAdvantageState::Unmeasurable) == 5);
    QNP_CHECK(static_cast<uint8_t>(PrimaryRecoveryDisposition::Pending) == 0);
    QNP_CHECK(static_cast<uint8_t>(PrimaryRecoveryDisposition::Ready) == 1);
    QNP_CHECK(static_cast<uint8_t>(PrimaryRecoveryDisposition::Cancelled) == 2);
    QNP_CHECK(static_cast<uint8_t>(PrimaryRecoveryDisposition::Interrupted) == 3);
    QNP_CHECK(static_cast<uint8_t>(PrimaryRecoveryDisposition::ActorChanged) == 4);
    QNP_CHECK(static_cast<uint8_t>(PrimaryRecoveryDisposition::Continued) == 5);
    QNP_CHECK(static_cast<uint8_t>(PrimaryRecoveryDisposition::Rechained) == 6);
}

QNP_TEST(ThrowReceptionIsAnExactIndependentCombatEpisode) {
    QNP_CHECK(static_cast<uint8_t>(CombatContactKind::Throw) == 3u);
    QNP_CHECK(combatActionIsThrowAttackerPath(70u));
    QNP_CHECK(combatActionIsThrowAttackerPath(72u));
    QNP_CHECK(!combatActionIsThrowAttackerPath(69u));
    QNP_CHECK(!combatActionIsThrowAttackerPath(73u));
    QNP_CHECK(combatThrowAttackerTupleIsHold(70u, 70u));
    QNP_CHECK(combatThrowAttackerTupleIsHold(70u, 72u));
    QNP_CHECK(combatThrowAttackerTupleIsHold(72u, 72u));
    QNP_CHECK(!combatThrowAttackerTupleIsHold(72u, 70u));
    QNP_CHECK(!combatThrowAttackerTupleIsHold(70u, 73u));
    QNP_CHECK(combatThrowTechCounterpartContinues(
        70u, 70u, 70u, 72u));
    QNP_CHECK(combatThrowTechCounterpartContinues(
        70u, 70u, 72u, 72u));
    QNP_CHECK(combatThrowTechCounterpartContinues(
        70u, 72u, 72u, 72u));
    QNP_CHECK(!combatThrowTechCounterpartContinues(
        70u, 72u, 70u, 70u));
    QNP_CHECK(!combatThrowTechCounterpartContinues(
        72u, 72u, 70u, 72u));
    QNP_CHECK(combatActionIsThrowReception(200u));
    QNP_CHECK(combatActionIsThrowReception(250u));
    QNP_CHECK(!combatActionIsThrowReception(199u));

    CombatContactEvent thrown =
        combatContact(1u, 1u, CombatContactKind::Throw);
    QNP_REQUIRE(combatContactIsWellFormed(thrown));

    FrameAdvantageTracker advantage;
    CombatFrameSample contact =
        combatFrame(1u, 1u, 70u, false, false,
                    200u, false, false);
    contact.owners[1].defensiveLock = true;
    advantage.advance(contact, &thrown, 1u);
    QNP_REQUIRE(advantage.status().state
                == FrameAdvantageState::Measuring);
    QNP_CHECK(advantage.status().contactKind
              == CombatContactKind::Throw);

    CombatFrameSample throwerReady =
        combatFrame(1u, 2u, 70u, true, true,
                    210u, false, false);
    throwerReady.owners[1].defensiveLock = true;
    advantage.advance(throwerReady, nullptr, 0u);
    QNP_CHECK(advantage.status().attackerReady);
    QNP_CHECK(!advantage.status().defenderReady);

    CombatFrameSample techResult =
        combatFrame(1u, 3u, 0u, false, true,
                    73u, false, false);
    techResult.owners[1].defensiveLock = true;
    advantage.advance(techResult, nullptr, 0u);
    QNP_CHECK(advantage.status().state
              == FrameAdvantageState::Measuring);

    CombatFrameSample defenderReady =
        combatFrame(1u, 4u, 0u, false, true,
                    0u, false, true);
    advantage.advance(defenderReady, nullptr, 0u);
    QNP_CHECK(advantage.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(advantage.status().advantageValid);
    QNP_CHECK(advantage.status().advantageFrames == 2);

    TrainingTriggerController triggers;
    triggers.resetTimeline(1u);
    QNP_REQUIRE(triggers.setConfig(
        TrainingTriggerEvent::OnHit, triggerConfig(1u)));
    QNP_REQUIRE(triggers.setConfig(
        TrainingTriggerEvent::AfterBlock, triggerConfig(2u)));
    QNP_REQUIRE(triggers.setConfig(
        TrainingTriggerEvent::AfterThrowTech, triggerConfig(3u)));
    QNP_CHECK(!triggerContactStep(
        triggers, contact, thrown).requestMacroStart);
    QNP_CHECK(!triggerStep(triggers, throwerReady).requestMacroStart);
    QNP_CHECK(!triggerStep(triggers, techResult).requestMacroStart);
    const TrainingTriggerStepResult afterTech =
        triggerStep(triggers, defenderReady);
    QNP_CHECK(afterTech.requestMacroStart);
    QNP_CHECK(afterTech.macroSlotIndex == 2u);
    QNP_CHECK(afterTech.event == TrainingTriggerEvent::AfterThrowTech);
}

QNP_TEST(FrameAdvantagePrimaryRecoveryDispositionTerminatesImmediately) {
    struct Case {
        PrimaryRecoveryDisposition disposition;
        FrameAdvantageReason reason;
    };
    const Case cases[] = {
        { PrimaryRecoveryDisposition::Cancelled,
          FrameAdvantageReason::AttackerCancelled },
        { PrimaryRecoveryDisposition::Interrupted,
          FrameAdvantageReason::AttackerInterrupted },
        { PrimaryRecoveryDisposition::ActorChanged,
          FrameAdvantageReason::AttackerActorChanged },
    };

    uint32_t generation = 1;
    for (const Case& testCase : cases) {
        FrameAdvantageTracker tracker;
        CombatContactEvent hit =
            combatContact(1, 1, CombatContactKind::Hit);
        tracker.advance(
            combatFrame(generation, 1, 70, false, true,
                        22, false, false),
            &hit, 1);
        CombatFrameSample terminal =
            combatFrame(generation, 2, 70, false, true,
                        22, false, false);
        terminal.owners[0].primaryRecovery = testCase.disposition;
        tracker.advance(terminal, nullptr, 0);
        QNP_CHECK(tracker.status().state
                  == FrameAdvantageState::Unmeasurable);
        QNP_CHECK(tracker.status().reason == testCase.reason);
        QNP_CHECK(tracker.status().unmeasurableCount == 1);

        terminal.stepSerial = 3;
        tracker.advance(terminal, nullptr, 0);
        QNP_CHECK(tracker.status().unmeasurableCount == 1);
        ++generation;
    }

    FrameAdvantageTracker readyTracker;
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    readyTracker.advance(
        combatFrame(9, 1, 70, false, true, 22, false, false), &hit, 1);
    CombatFrameSample ready =
        combatFrame(9, 2, 0, false, true, 0, false, true);
    ready.owners[0].primaryRecovery = PrimaryRecoveryDisposition::Ready;
    ready.owners[0].actionEpoch = 2;
    readyTracker.advance(ready, nullptr, 0);
    QNP_CHECK(readyTracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(readyTracker.status().reason == FrameAdvantageReason::Completed);
    QNP_CHECK(readyTracker.status().advantageFrames == 0);

    FrameAdvantageTracker continuedTracker;
    CombatContactEvent throwContact =
        combatContact(1, 1, CombatContactKind::Throw);
    continuedTracker.advance(
        combatFrame(10, 1, 70, false, false, 200, false, false),
        &throwContact, 1);
    CombatFrameSample counterpart =
        combatFrame(10, 2, 72, false, false, 73, false, false);
    counterpart.owners[0].actionEpoch = 2;
    counterpart.owners[0].primaryRecovery =
        PrimaryRecoveryDisposition::Continued;
    counterpart.owners[1].defensiveLock = true;
    continuedTracker.advance(counterpart, nullptr, 0);
    QNP_CHECK(continuedTracker.status().state
              == FrameAdvantageState::Measuring);
    QNP_CHECK(continuedTracker.status().attackerActionEpoch == 2);

    CombatFrameSample throwRelease =
        combatFrame(10, 3, 0, false, true, 0, false, true);
    throwRelease.owners[0].actionEpoch = 3;
    throwRelease.owners[0].primaryRecovery =
        PrimaryRecoveryDisposition::Ready;
    continuedTracker.advance(throwRelease, nullptr, 0);
    QNP_CHECK(continuedTracker.status().state
              == FrameAdvantageState::Complete);
    QNP_CHECK(continuedTracker.status().advantageFrames == 0);
}

QNP_TEST(FrameAdvantageRechainMeasuresTheLastMoveOfAString) {
    FrameAdvantageTracker tracker;
    CombatContactEvent blocked =
        combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &blocked, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().chainedActions == 0u);

    CombatFrameSample chained =
        combatFrame(1, 2, 61, false, false, 18, false, false);
    chained.owners[0].actionEpoch = 2;
    chained.owners[0].primaryRecovery = PrimaryRecoveryDisposition::Rechained;
    chained.owners[1].defensiveLock = true;
    tracker.advance(chained, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().attackerActionEpoch == 2);
    QNP_CHECK(!tracker.status().attackerReady);
    QNP_CHECK(tracker.status().chainedActions == 1u);

    CombatFrameSample defenderOut =
        combatFrame(1, 3, 61, false, false, 0, false, true);
    defenderOut.owners[0].actionEpoch = 2;
    tracker.advance(defenderOut, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().defenderReady);
    QNP_CHECK(tracker.status().defenderReadyDelay == 2u);

    CombatFrameSample attackerOut =
        combatFrame(1, 4, 0, true, true, 0, false, true);
    attackerOut.owners[0].actionEpoch = 2;
    attackerOut.owners[0].primaryRecovery = PrimaryRecoveryDisposition::Ready;
    tracker.advance(attackerOut, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().reason == FrameAdvantageReason::Completed);
    QNP_CHECK(tracker.status().advantageValid);
    QNP_CHECK(tracker.status().attackerReadyDelay == 3u);
    QNP_CHECK(tracker.status().advantageFrames == -1);
    QNP_CHECK(tracker.status().chainedActions == 1u);
}

QNP_TEST(FrameAdvantageRechainSurvivesMoreThanOneLink) {
    FrameAdvantageTracker tracker;
    CombatContactEvent blocked =
        combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &blocked, 1);
    for (uint64_t link = 0; link < 2; ++link) {
        CombatFrameSample chained =
            combatFrame(1, 2 + link, 61 + static_cast<uint32_t>(link),
                        false, false, 18, false, false);
        chained.owners[0].actionEpoch = 2 + link;
        chained.owners[0].primaryRecovery =
            PrimaryRecoveryDisposition::Rechained;
        chained.owners[1].defensiveLock = true;
        tracker.advance(chained, nullptr, 0);
        QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    }
    QNP_CHECK(tracker.status().chainedActions == 2u);
    QNP_CHECK(tracker.status().attackerActionEpoch == 3);

    CombatFrameSample done = combatFrame(1, 4, 0, true, true, 0, false, true);
    done.owners[0].actionEpoch = 3;
    done.owners[0].primaryRecovery = PrimaryRecoveryDisposition::Ready;
    tracker.advance(done, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().advantageValid);
    QNP_CHECK(tracker.status().advantageFrames == 0);
    QNP_CHECK(tracker.status().chainedActions == 2u);
}

QNP_TEST(FrameAdvantageRechainCounterIsPerEpisode) {
    FrameAdvantageTracker tracker;
    CombatContactEvent first = combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &first, 1);
    CombatFrameSample chained =
        combatFrame(1, 2, 61, false, false, 18, false, false);
    chained.owners[0].actionEpoch = 2;
    chained.owners[0].primaryRecovery = PrimaryRecoveryDisposition::Rechained;
    chained.owners[1].defensiveLock = true;
    tracker.advance(chained, nullptr, 0);
    QNP_CHECK(tracker.status().chainedActions == 1u);

    CombatContactEvent second = combatContact(2, 3, CombatContactKind::Hit);
    second.attackerActionEpoch = 2;
    CombatFrameSample restart =
        combatFrame(1, 3, 61, false, false, 22, false, false);
    restart.owners[0].actionEpoch = 2;
    tracker.advance(restart, &second, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().chainedActions == 0u);
    QNP_CHECK(tracker.status().contactSerial == 2u);
}

QNP_TEST(FrameAdvantageGapIsMeasuredIntoTheEpisodeItPrecedes) {
    FrameAdvantageTracker tracker;
    CombatContactEvent first = combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &first, 1);
    QNP_CHECK(!tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapCount == 0u);

    CombatFrameSample defenderOut =
        combatFrame(1, 2, 60, false, false, 0, false, true);
    tracker.advance(defenderOut, nullptr, 0);
    QNP_CHECK(tracker.status().defenderReady);
    QNP_CHECK(!tracker.status().gapValid);

    tracker.advance(
        combatFrame(1, 3, 60, false, false, 0, false, true), nullptr, 0);
    tracker.advance(
        combatFrame(1, 4, 60, false, false, 0, false, true), nullptr, 0);

    CombatContactEvent second = combatContact(2, 5, CombatContactKind::Block);
    CombatFrameSample reblock =
        combatFrame(1, 5, 61, false, false, 18, false, false);
    tracker.advance(reblock, &second, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapFrames == 3u);
    QNP_CHECK(tracker.status().gapCount == 1u);

    uint64_t step = 6;
    for (; step < 5 + kFrameAdvantageGapDisplaySteps; ++step) {
        tracker.advance(
            combatFrame(1, step, 61, false, false, 18, false, false),
            nullptr, 0);
        QNP_CHECK(tracker.status().gapValid);
    }
    tracker.advance(
        combatFrame(1, step, 61, false, false, 18, false, false), nullptr, 0);
    QNP_CHECK(!tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapFrames == 0u);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().gapCount == 1u);
}

QNP_TEST(FrameAdvantageGapIsSuppressedWhenANumberIsAlreadyOnScreen) {
    FrameAdvantageTracker tracker;
    CombatContactEvent first = combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &first, 1);

    CombatFrameSample done = combatFrame(1, 2, 0, true, true, 0, false, true);
    done.owners[0].primaryRecovery = PrimaryRecoveryDisposition::Ready;
    tracker.advance(done, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().advantageValid);

    tracker.advance(
        combatFrame(1, 3, 0, true, true, 0, false, true), nullptr, 0);
    tracker.advance(
        combatFrame(1, 4, 0, true, true, 0, false, true), nullptr, 0);

    CombatContactEvent second = combatContact(2, 5, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 5, 61, false, false, 18, false, false), &second, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(!tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapCount == 0u);
}

QNP_TEST(FrameAdvantageZeroLengthGapIsNotReported) {
    FrameAdvantageTracker tracker;
    CombatContactEvent first = combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &first, 1);
    CombatFrameSample out2 =
        combatFrame(1, 2, 60, false, false, 0, false, true);
    tracker.advance(out2, nullptr, 0);
    QNP_CHECK(tracker.status().defenderReady);

    CombatContactEvent second = combatContact(2, 2, CombatContactKind::Block);
    second.stepSerial = 2;
    CombatFrameSample reblock =
        combatFrame(1, 3, 61, false, false, 18, false, false);
    second.stepSerial = 2;
    tracker.advance(reblock, &second, 1);
    QNP_CHECK(!tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapCount == 0u);
}

QNP_TEST(FrameAdvantageTrueBlockstringReportsNoGap) {
    FrameAdvantageTracker tracker;
    CombatContactEvent first = combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &first, 1);
    tracker.advance(
        combatFrame(1, 2, 60, false, false, 18, false, false), nullptr, 0);
    CombatContactEvent second = combatContact(2, 3, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 3, 61, false, false, 18, false, false), &second, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(!tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapFrames == 0u);
    QNP_CHECK(tracker.status().gapCount == 0u);
}

QNP_TEST(FrameAdvantageGapExpiresBeyondItsWindow) {
    FrameAdvantageTracker tracker;
    CombatContactEvent first = combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &first, 1);
    tracker.advance(
        combatFrame(1, 2, 60, false, false, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().defenderReady);

    uint64_t step = 3;
    for (; step <= 2 + kFrameAdvantageMaxGapSteps + 1; ++step) {
        tracker.advance(
            combatFrame(1, step, 0, true, true, 0, false, true), nullptr, 0);
    }
    CombatContactEvent late =
        combatContact(2, step, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, step, 61, false, false, 18, false, false), &late, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(!tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapCount == 0u);
}

QNP_TEST(FrameAdvantageGapDoesNotSurviveReversedRoles) {
    FrameAdvantageTracker tracker;
    CombatContactEvent first = combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &first, 1);
    tracker.advance(
        combatFrame(1, 2, 60, false, false, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().defenderReady);

    CombatContactEvent punish =
        combatContact(2, 4, CombatContactKind::Hit, 1, 0);
    tracker.advance(
        combatFrame(1, 3, 60, false, false, 61, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(1, 4, 22, false, false, 61, false, false), &punish, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().attackerOwner == 1u);
    QNP_CHECK(!tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapCount == 0u);
}

QNP_TEST(FrameAdvantageUnmeasurableEpisodeDropsItsGapCandidate) {
    FrameAdvantageTracker tracker;
    CombatContactEvent first = combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &first, 1);
    tracker.advance(
        combatFrame(1, 2, 60, false, false, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().defenderReady);

    CombatFrameSample interrupted =
        combatFrame(1, 3, 99, false, false, 0, false, true);
    interrupted.owners[0].primaryRecovery =
        PrimaryRecoveryDisposition::Interrupted;
    tracker.advance(interrupted, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Unmeasurable);

    CombatContactEvent second = combatContact(2, 4, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 4, 61, false, false, 18, false, false), &second, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(!tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapCount == 0u);
}

QNP_TEST(FrameAdvantageGapIsReportedInsideARechainedString) {
    FrameAdvantageTracker tracker;
    CombatContactEvent first = combatContact(1, 1, CombatContactKind::Block);
    tracker.advance(
        combatFrame(1, 1, 60, false, false, 18, false, false), &first, 1);
    CombatFrameSample chained =
        combatFrame(1, 2, 61, false, false, 18, false, false);
    chained.owners[0].actionEpoch = 2;
    chained.owners[0].primaryRecovery = PrimaryRecoveryDisposition::Rechained;
    chained.owners[1].defensiveLock = true;
    tracker.advance(chained, nullptr, 0);
    QNP_CHECK(tracker.status().chainedActions == 1u);

    CombatFrameSample out = combatFrame(1, 3, 61, false, false, 0, false, true);
    out.owners[0].actionEpoch = 2;
    tracker.advance(out, nullptr, 0);
    QNP_CHECK(tracker.status().defenderReady);

    CombatFrameSample idle = combatFrame(1, 4, 61, false, false, 0, false, true);
    idle.owners[0].actionEpoch = 2;
    tracker.advance(idle, nullptr, 0);

    CombatContactEvent third = combatContact(3, 5, CombatContactKind::Block);
    third.attackerActionEpoch = 2;
    CombatFrameSample reblock =
        combatFrame(1, 5, 62, false, false, 18, false, false);
    reblock.owners[0].actionEpoch = 2;
    tracker.advance(reblock, &third, 1);
    QNP_CHECK(tracker.status().gapValid);
    QNP_CHECK(tracker.status().gapFrames == 2u);
    QNP_CHECK(tracker.status().chainedActions == 0u);
}

QNP_TEST(FrameAdvantageLifecycleFailuresCannotResumeAnEpisode) {
    FrameAdvantageTracker tracker;
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, false, true, 22, false, false), &hit, 1);
    CombatFrameSample unreadable =
        combatFrame(1, 2, 70, true, true, 0, false, true);
    unreadable.readable = false;
    tracker.advance(unreadable, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Unmeasurable);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::UnreadableSample);
    unreadable.readable = true;
    tracker.advance(unreadable, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);

    tracker.resetTimeline(2);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(2, 1, 70, false, true, 22, false, false), &hit, 1);
    CombatFrameSample roundEnd =
        combatFrame(2, 2, 70, true, true, 0, false, true);
    roundEnd.roundValid = false;
    tracker.advance(roundEnd, nullptr, 0);
    QNP_CHECK(tracker.status().reason == FrameAdvantageReason::RoundEnded);

    tracker.resetTimeline(3);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(3, 1, 70, false, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(3, 3, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().reason == FrameAdvantageReason::StepGap);

    tracker.resetTimeline(4);
    hit = combatContact(1, 5, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(4, 5, 70, false, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(4, 5, 0, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::NonMonotonicStep);
}

QNP_TEST(FrameAdvantageIdentityAndEpochChangesAreExplicit) {
    FrameAdvantageTracker tracker;
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, false, true, 22, false, false), &hit, 1);
    CombatFrameSample changed =
        combatFrame(1, 2, 70, false, true, 22, false, false);
    changed.owners[0].actorToken = 99;
    tracker.advance(changed, nullptr, 0);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::AttackerActorChanged);

    tracker.resetTimeline(2);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(2, 1, 70, false, true, 22, false, false), &hit, 1);
    changed = combatFrame(2, 2, 70, false, true, 22, false, false);
    changed.owners[1].actorToken = 88;
    tracker.advance(changed, nullptr, 0);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::DefenderActorChanged);

    tracker.resetTimeline(3);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(3, 1, 70, false, true, 22, false, false), &hit, 1);
    changed = combatFrame(3, 2, 70, false, true, 22, false, false);
    changed.owners[0].actionEpoch = 2;
    tracker.advance(changed, nullptr, 0);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::AttackerInterrupted);
}

QNP_TEST(FrameAdvantageRejectsNonConventionalDefenderEpisodes) {
    FrameAdvantageTracker tracker;
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    hit.victimIsPrimary = false;
    tracker.advance(
        combatFrame(1, 1, 70, true, true, 22, false, false), &hit, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Unmeasurable);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::NonPrimaryVictim);

    tracker.resetTimeline(2);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    hit.defenderLockEligible = false;
    tracker.advance(
        combatFrame(2, 1, 70, true, true, 22, false, false), &hit, 1);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::DefenderNoLock);

    tracker.resetTimeline(3);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    CombatFrameSample locked =
        combatFrame(3, 1, 70, true, true, 22, false, false);
    locked.owners[1].defensiveLock = true;
    tracker.advance(locked, &hit, 1);
    CombatFrameSample escaped =
        combatFrame(3, 2, 70, true, true, 0, false, false);
    escaped.owners[1].defensiveLock = false;
    tracker.advance(escaped, nullptr, 0);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::DefenderLockLost);

    tracker.resetTimeline(4);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(4, 1, 70, true, true, 22, false, false), &hit, 1);
    CombatFrameSample invalidPath =
        combatFrame(4, 2, 70, true, true, 22, false, false);
    invalidPath.owners[1].semanticPathValid = false;
    tracker.advance(invalidPath, nullptr, 0);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::DefenderPathInvalid);

    tracker.resetTimeline(5);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(5, 1, 70, true, true, 22, false, false), &hit, 1);
    invalidPath = combatFrame(5, 2, 70, true, true, 22, false, true);
    invalidPath.owners[1].semanticPathValid = false;
    tracker.advance(invalidPath, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
}

QNP_TEST(FrameAdvantageContactBatchValidationIsAtomicAndCurrentStepOnly) {
    FrameAdvantageTracker tracker;
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, false, true, 22, false, false), &hit, 1);

    const CombatContactEvent duplicateBatch[] = {
        combatContact(2, 2, CombatContactKind::Hit),
        combatContact(2, 2, CombatContactKind::Hit),
    };
    tracker.advance(
        combatFrame(1, 2, 70, false, true, 22, false, false),
        duplicateBatch, 2);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Unmeasurable);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::InvalidContactBatch);
    QNP_CHECK(tracker.status().invalidContactCount == 1);
    QNP_CHECK(tracker.status().contactSerial == 1);

    hit = combatContact(2, 3, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 3, 70, false, true, 22, false, false), &hit, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().contactSerial == 2);

    hit = combatContact(3, 3, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 4, 70, false, true, 22, false, false), &hit, 1);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::InvalidContactBatch);
    QNP_CHECK(tracker.status().contactSerial == 2);
    QNP_CHECK(tracker.status().invalidContactCount == 2);

    hit = combatContact(3, 5, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 5, 70, false, true, 22, false, false), &hit, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Measuring);
    QNP_CHECK(tracker.status().contactSerial == 3);
}

QNP_TEST(FrameAdvantageTerminalReadoutsPersistUntilReplacementResetOrExpiry) {
    FrameAdvantageTracker tracker;
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, true, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(1, 2, 0, true, true, 0, false, true), nullptr, 0);
    QNP_REQUIRE(tracker.status().state == FrameAdvantageState::Complete);
    const int32_t completedValue = tracker.status().advantageFrames;
    tracker.advance(
        combatFrame(1, 3, 0, true, true, 0, true, true), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().advantageFrames == completedValue);

    hit = combatContact(2, 4, CombatContactKind::Hit);
    hit.defenderLockEligible = false;
    tracker.advance(
        combatFrame(1, 4, 70, false, true, 22, false, false), &hit, 1);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Unmeasurable);
    QNP_CHECK(!tracker.status().advantageValid);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::DefenderNoLock);
    tracker.advance(
        combatFrame(1, 5, 70, true, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Unmeasurable);

    tracker.setEnabled(false);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);
    QNP_CHECK(tracker.status().reason == FrameAdvantageReason::None);
    QNP_CHECK(tracker.status().unmeasurableCount == 0);
}

QNP_TEST(FrameAdvantageVisibleReadoutsExpireAfterSixtyAdmittedSteps) {
    FrameAdvantageTracker tracker;
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, true, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(1, 2, 0, true, true, 0, false, true), nullptr, 0);
    QNP_REQUIRE(tracker.status().state == FrameAdvantageState::Complete);

    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    for (uint64_t age = 1;
         age < kFrameAdvantageReadoutLifetimeSteps; ++age) {
        tracker.advance(
            combatFrame(1, 2 + age, 0, true, true, 0, true, true),
            nullptr, 0);
        QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    }
    tracker.advance(
        combatFrame(
            1, 2 + kFrameAdvantageReadoutLifetimeSteps,
            0, true, true, 0, true, true),
        nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);
    QNP_CHECK(tracker.status().completedCount == 1);

    tracker.resetTimeline(2, true);
    const CombatContactEvent trade[] = {
        combatContact(1, 1, CombatContactKind::Hit, 0, 1),
        combatContact(2, 1, CombatContactKind::Hit, 1, 0),
    };
    tracker.advance(
        combatFrame(2, 1, 70, true, true, 70, true, true), trade, 2);
    QNP_REQUIRE(tracker.status().state == FrameAdvantageState::Trade);
    for (uint64_t age = 1;
         age < kFrameAdvantageReadoutLifetimeSteps; ++age) {
        tracker.advance(
            combatFrame(2, 1 + age, 0, true, true, 0, true, true),
            nullptr, 0);
        QNP_CHECK(tracker.status().state == FrameAdvantageState::Trade);
    }
    tracker.advance(
        combatFrame(
            2, 1 + kFrameAdvantageReadoutLifetimeSteps,
            0, true, true, 0, true, true),
        nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);
    QNP_CHECK(tracker.status().tradeCount == 1);
}

QNP_TEST(FrameAdvantageLifecycleEdgesClearStaleTerminalReadouts) {
    FrameAdvantageTracker tracker;
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, true, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(1, 2, 0, true, true, 0, false, true), nullptr, 0);
    QNP_REQUIRE(tracker.status().state == FrameAdvantageState::Complete);
    CombatFrameSample unreadable =
        combatFrame(1, 3, 0, true, true, 0, true, true);
    unreadable.readable = false;
    tracker.advance(unreadable, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);
    QNP_CHECK(tracker.status().reason == FrameAdvantageReason::None);

    tracker.resetTimeline(2);
    const CombatContactEvent trade[] = {
        combatContact(1, 1, CombatContactKind::Hit, 0, 1),
        combatContact(2, 1, CombatContactKind::Hit, 1, 0),
    };
    tracker.advance(
        combatFrame(2, 1, 70, true, true, 70, true, true), trade, 2);
    QNP_REQUIRE(tracker.status().state == FrameAdvantageState::Trade);
    CombatFrameSample roundEnd =
        combatFrame(2, 2, 70, true, true, 70, true, true);
    roundEnd.roundValid = false;
    tracker.advance(roundEnd, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);

    tracker.resetTimeline(3);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(3, 1, 70, true, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(3, 2, 0, true, true, 0, false, true), nullptr, 0);
    QNP_REQUIRE(tracker.status().state == FrameAdvantageState::Complete);
    CombatFrameSample replacement =
        combatFrame(3, 3, 0, true, true, 0, true, true);
    replacement.owners[0].actorToken = 99;
    tracker.advance(replacement, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Idle);

    tracker.resetTimeline(4);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(4, 1, 70, true, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(4, 2, 0, true, true, 0, false, true), nullptr, 0);
    QNP_REQUIRE(tracker.status().state == FrameAdvantageState::Complete);
    const CombatContactEvent invalid[] = {
        combatContact(2, 3, CombatContactKind::Hit),
        combatContact(2, 3, CombatContactKind::Hit),
    };
    tracker.advance(
        combatFrame(4, 3, 0, true, true, 0, true, true), invalid, 2);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Unmeasurable);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::InvalidContactBatch);
    QNP_CHECK(!tracker.status().advantageValid);
}

QNP_TEST(FrameAdvantageTimeoutReasonsAndBoundaryCompletionAreExact) {
    FrameAdvantageTracker tracker(2);
    CombatContactEvent hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(1, 1, 70, false, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(1, 2, 70, false, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(1, 3, 70, false, true, 22, false, false), nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::TimedOut);
    QNP_CHECK(tracker.status().reason == FrameAdvantageReason::TimeoutBoth);

    tracker.resetTimeline(2);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(2, 1, 70, false, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(2, 2, 70, false, true, 0, false, true), nullptr, 0);
    tracker.advance(
        combatFrame(2, 3, 70, false, true, 0, false, true), nullptr, 0);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::TimeoutAttacker);

    tracker.resetTimeline(3);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(3, 1, 70, true, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(3, 2, 0, true, true, 22, false, false), nullptr, 0);
    tracker.advance(
        combatFrame(3, 3, 0, true, true, 22, false, false), nullptr, 0);
    QNP_CHECK(tracker.status().reason
              == FrameAdvantageReason::TimeoutDefender);

    tracker.resetTimeline(4);
    hit = combatContact(1, 1, CombatContactKind::Hit);
    tracker.advance(
        combatFrame(4, 1, 70, false, true, 22, false, false), &hit, 1);
    tracker.advance(
        combatFrame(4, 2, 70, false, true, 22, false, false), nullptr, 0);
    CombatFrameSample boundary =
        combatFrame(4, 3, 0, false, true, 0, false, true);
    boundary.owners[0].primaryRecovery =
        PrimaryRecoveryDisposition::Ready;
    tracker.advance(boundary, nullptr, 0);
    QNP_CHECK(tracker.status().state == FrameAdvantageState::Complete);
    QNP_CHECK(tracker.status().reason == FrameAdvantageReason::Completed);
    QNP_CHECK(tracker.status().timeoutCount == 0);
}

QNP_TEST(TriggerDiagnosticsSeparateUnconfiguredFromWaiting) {
    TrainingTriggerController controller;

    triggerStep(controller, combatFrame(1, 1, 0, true, true, 0, true, true));
    QNP_CHECK(controller.diagnostics().reason
              == TriggerSuppressionReason::NoEventArmed);
    QNP_CHECK(controller.diagnostics().armedEventMask == 0u);

    triggerStep(controller, combatFrame(1, 2, 0, true, true, 35, false, false));
    QNP_CHECK(controller.diagnostics().haveArmedEvent);
    QNP_CHECK(controller.diagnostics().armedEvent
              == TrainingTriggerEvent::OnWakeup);
    QNP_CHECK(controller.diagnostics().reason
              == TriggerSuppressionReason::EventArmedNotConfigured);
    QNP_CHECK(!controller.diagnostics().havePending);

    TrainingTriggerController configured;
    QNP_REQUIRE(configured.setConfig(TrainingTriggerEvent::OnWakeup,
                                     triggerConfig(1)));
    triggerStep(configured, combatFrame(2, 1, 0, true, true, 0, true, true));
    triggerStep(configured, combatFrame(2, 2, 0, true, true, 35, false, false));
    QNP_CHECK(configured.diagnostics().reason
              == TriggerSuppressionReason::WaitingForResponseReady);
    QNP_CHECK(!configured.diagnostics().responseReady);

    const TrainingTriggerStepResult started = triggerStep(
        configured, combatFrame(2, 3, 0, true, true, 0, false, true));
    QNP_CHECK(started.outcome == TriggerRequestOutcome::Started);
    QNP_CHECK(configured.diagnostics().reason
              == TriggerSuppressionReason::None);
}

QNP_TEST(TriggerDiagnosticsNameAnUnmeasurableTimeline) {
    TrainingTriggerController controller;
    triggerStep(controller, combatFrame(1, 1, 0, true, true, 0, true, true));
    triggerStep(controller, combatFrame(1, 2, 0, true, true, 35, false, false));
    QNP_CHECK(controller.diagnostics().haveArmedEvent);

    CombatFrameSample ended =
        combatFrame(1, 3, 0, true, true, 35, false, false);
    ended.roundValid = false;
    triggerStep(controller, ended);
    QNP_CHECK(controller.diagnostics().reason
              == TriggerSuppressionReason::TimelineNotMeasurable);
}

QNP_TEST(WakeupEpisodeGuardAnchorsToTheFirstReactionStep) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnWakeup,
                                     triggerConfig(1)));
    triggerStep(controller, combatFrame(1, 1, 0, true, true, 0, true, true));
    triggerStep(controller, combatFrame(1, 2, 0, true, true, 25, false, false));
    triggerStep(controller, combatFrame(1, 3, 0, true, true, 56, false, false));
    const TrainingTriggerStepResult onEdge = triggerStep(
        controller, combatFrame(1, 4, 0, true, true, 57, false, true));
    QNP_CHECK(onEdge.outcome == TriggerRequestOutcome::Started);
    QNP_CHECK(onEdge.event == TrainingTriggerEvent::OnWakeup);
}

QNP_TEST(ZeroDelayWakeupMoveStartsEvenWhenNothingWasPrebuffered) {
    TrainingTriggerConfig move{};
    move.moveIndex = 3;
    move.chancePercent = 100;

    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnWakeup, move));
    const uint64_t moves = uint64_t{1} << 2;

    controller.advance(combatFrame(1, 1, 0, true, true, 0, true, true),
                       nullptr, 0, true, 0xFFu, moves, false, false);
    controller.advance(combatFrame(1, 2, 0, true, true, 35, false, false),
                       nullptr, 0, true, 0xFFu, moves, false, false);
    const TrainingTriggerStepResult started = controller.advance(
        combatFrame(1, 3, 0, true, true, 0, false, true),
        nullptr, 0, true, 0xFFu, moves, false, false);
    QNP_CHECK(started.outcome == TriggerRequestOutcome::Started);
    QNP_CHECK(started.requestMoveStart);
    QNP_CHECK(started.moveIndex == 3);
    QNP_CHECK(controller.status().droppedCount == 0u);
}

QNP_TEST(ZeroDelayWakeupMoveWaitsWhileAReversalIsPrimed) {
    TrainingTriggerConfig move{};
    move.moveIndex = 3;
    move.chancePercent = 100;

    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnWakeup, move));
    const uint64_t moves = uint64_t{1} << 2;

    controller.advance(combatFrame(2, 1, 0, true, true, 0, true, true),
                       nullptr, 0, true, 0xFFu, moves, false, true);
    controller.advance(combatFrame(2, 2, 0, true, true, 35, false, false),
                       nullptr, 0, true, 0xFFu, moves, false, true);
    const TrainingTriggerStepResult held = controller.advance(
        combatFrame(2, 3, 0, true, true, 0, false, true),
        nullptr, 0, true, 0xFFu, moves, false, true);
    QNP_CHECK(held.outcome != TriggerRequestOutcome::Started);
    QNP_CHECK(controller.status().droppedCount == 0u);
    QNP_CHECK(controller.diagnostics().reason
              == TriggerSuppressionReason::WaitingForWakeupRelease);

    const TrainingTriggerStepResult released = controller.advance(
        combatFrame(2, 4, 0, true, true, 0, false, true),
        nullptr, 0, true, 0xFFu, moves, true, true);
    QNP_CHECK(released.outcome == TriggerRequestOutcome::Started);
    QNP_CHECK(released.moveIndex == 3);
}

QNP_TEST(NativeReactionFamilyMatchesTheCommandEvaluator) {
    for (const uint32_t action : {25u, 35u, 37u, 38u, 56u, 57u, 58u}) {
        QNP_CHECK(combatActionIsNativeReactionFamily(action));
    }
    QNP_CHECK(combatActionIsNativeReactionFamily(200u));
    QNP_CHECK(combatActionIsNativeReactionFamily(210u));
    QNP_CHECK(combatActionIsNativeReactionFamily(255u));

    for (const uint32_t action : {21u, 22u, 23u}) {
        QNP_CHECK(!combatActionIsNativeReactionFamily(action));
    }
    for (const uint32_t action : {0u, 18u, 19u, 20u, 24u, 26u, 27u, 28u,
                                  31u, 33u, 34u, 36u, 70u, 73u, 199u}) {
        QNP_CHECK(!combatActionIsNativeReactionFamily(action));
    }
}

QNP_TEST(TrainingActorSlotEncodingRoundTrips) {
    const TrainingActorPoolId pools[] = {
        TrainingActorPoolId::Effect,
        TrainingActorPoolId::Clone,
        TrainingActorPoolId::Helper,
    };
    for (const TrainingActorPoolId pool : pools) {
        for (const uint32_t slot : {0u, 1u, 19u, 29u, 119u, 255u, 4096u}) {
            const uint32_t encoded = encodeTrainingActorSlot(pool, slot);
            QNP_CHECK(decodeTrainingActorPool(encoded) == pool);
            QNP_CHECK(decodeTrainingActorSlotIndex(encoded) == slot);
        }
    }
    QNP_CHECK(encodeTrainingActorSlot(TrainingActorPoolId::Effect, 1u)
              != encodeTrainingActorSlot(TrainingActorPoolId::Clone, 1u));
    QNP_CHECK(kTrainingActorOwnerUnknown != 0u
              && kTrainingActorOwnerUnknown != 1u);
}

QNP_TEST(CollisionFallRouteCannotCommitACommand) {
    QNP_CHECK(combatActionCannotCommitCommand(36u));
    QNP_CHECK(combatActionIsNaturalWakeupPath(36u));
    QNP_CHECK(!combatActionIsNativeReactionFamily(36u));

    for (const uint32_t action : {0u, 25u, 28u, 33u, 34u, 35u, 37u, 38u,
                                  56u, 57u, 58u, 70u, 200u}) {
        QNP_CHECK(!combatActionCannotCommitCommand(action));
    }
}

QNP_TEST(WakeupEpisodeSurvivesTheCollisionFallRoute) {
    TrainingTriggerController controller;
    QNP_REQUIRE(controller.setConfig(TrainingTriggerEvent::OnWakeup,
                                     triggerConfig(1)));
    triggerStep(controller, combatFrame(1, 1, 0, true, true, 0, true, true));
    triggerStep(controller, combatFrame(1, 2, 0, true, true, 36, false, false));
    QNP_CHECK(controller.diagnostics().haveArmedEvent);
    QNP_CHECK(controller.diagnostics().armedEvent
              == TrainingTriggerEvent::OnWakeup);
    triggerStep(controller, combatFrame(1, 3, 0, true, true, 35, false, false));
    QNP_CHECK(controller.diagnostics().reason
              == TriggerSuppressionReason::WaitingForResponseReady);
    const TrainingTriggerStepResult fired = triggerStep(
        controller, combatFrame(1, 4, 0, true, true, 0, false, true));
    QNP_CHECK(fired.outcome == TriggerRequestOutcome::Started);
    QNP_CHECK(fired.event == TrainingTriggerEvent::OnWakeup);
}

QNP_TEST(TrainingMacroPreRecordPositionsTheDummyWithoutKeepingIt) {
    TrainingMacros macros;
    macros.reset();
    uint64_t step = 1;
    QNP_REQUIRE(recordOneFrame(macros, step, 0, qinput::kLogicalA));
    QNP_CHECK(macros.selectedFrameCount() == 1u);

    QNP_REQUIRE(macros.requestPreRecord());
    QNP_CHECK(macros.state() == MacroState::PreRecord);
    MacroStepInput input{};
    input.recordSourceMask = qinput::kLogicalRight;
    MacroStepResult result = macros.advance(step++, input);
    QNP_CHECK(result.overrideP1 && result.overrideP2);
    QNP_CHECK(result.p2Mask == qinput::kLogicalRight);
    QNP_CHECK(!result.recordFrameRequested);
    QNP_CHECK(result.state == MacroState::PreRecord);
    for (int i = 0; i < 30; ++i) {
        result = macros.advance(step++, input);
        QNP_CHECK(result.state == MacroState::PreRecord);
        QNP_CHECK(!result.recordFrameRequested);
    }
    QNP_CHECK(macros.selectedFrameCount() == 1u);

    QNP_REQUIRE(pressRecordThroughPlacement(macros));
    QNP_CHECK(macros.advance(step++, input).state == MacroState::Recording);
    result = macros.advance(step++, input);
    QNP_CHECK(result.recordFrameRequested);
    QNP_CHECK(result.p2Mask == qinput::kLogicalRight);
    QNP_CHECK(macros.selectedFrameCount() == 0u);
    QNP_CHECK(macros.commitRecordedFrame(step - 1, result.p2Mask, 0)
                  .committed);
    QNP_CHECK(macros.selectedFrameCount() == 1u);
}

QNP_TEST(TrainingMacroPreRecordIsLeftByPlayAndByStopWithoutPlaying) {
    for (int exitByPlay = 0; exitByPlay < 2; ++exitByPlay) {
        TrainingMacros macros;
        macros.reset();
        uint64_t step = 1;
        QNP_REQUIRE(recordOneFrame(macros, step, 0, qinput::kLogicalB));
        QNP_REQUIRE(macros.requestPreRecord());
        MacroStepInput input{};
        input.recordSourceMask = qinput::kLogicalLeft;
        QNP_REQUIRE(macros.advance(step++, input).state
                    == MacroState::PreRecord);

        if (exitByPlay != 0) {
            QNP_REQUIRE(macros.requestPlaybackToggle());
        } else {
            QNP_REQUIRE(macros.requestStop());
        }
        const MacroStepResult leaving = macros.advance(step++, input);
        QNP_CHECK(leaving.state == MacroState::Idle);
        QNP_CHECK(!leaving.playbackFrame);
        const MacroStepResult after = macros.advance(step++, input);
        QNP_CHECK(after.state == MacroState::Idle);
        QNP_CHECK(!after.playbackFrame);
        QNP_CHECK(!after.overrideP2);
        QNP_CHECK(macros.selectedFrameCount() == 1u);
    }
}

QNP_TEST(TrainingMacroPreRecordIsLeftByPlayEvenWithAnEmptySlot) {
    TrainingMacros macros;
    macros.reset();
    uint64_t step = 1;
    QNP_REQUIRE(macros.requestRecordToggle());
    QNP_CHECK(macros.state() == MacroState::PreRecord);
    QNP_CHECK(macros.selectedFrameCount() == 0u);

    MacroStepInput input{};
    input.recordSourceMask = qinput::kLogicalRight;
    QNP_REQUIRE(macros.advance(step++, input).state == MacroState::PreRecord);

    QNP_REQUIRE(macros.requestPlaybackToggle());
    const MacroStepResult leaving = macros.advance(step++, input);
    QNP_CHECK(leaving.state == MacroState::Idle);
    QNP_CHECK(!leaving.playbackFrame);
    QNP_CHECK(macros.selectedFrameCount() == 0u);
    const MacroStepResult after = macros.advance(step++, input);
    QNP_CHECK(!after.overrideP1 && !after.overrideP2);
}

QNP_TEST(TrainingMacroPreRecordOnlyStartsFromIdle) {
    TrainingMacros macros;
    macros.reset();
    uint64_t step = 1;
    QNP_REQUIRE(recordOneFrame(macros, step, 0, qinput::kLogicalA));
    MacroStepInput input{};
    QNP_REQUIRE(pressRecordThroughPlacement(macros));
    QNP_CHECK(!macros.requestPreRecord());
    QNP_REQUIRE(macros.requestStop());
    QNP_REQUIRE(macros.advance(step++, input).state == MacroState::Idle);
    QNP_REQUIRE(macros.requestPlaybackToggle());
    QNP_CHECK(!macros.requestPreRecord());
}

QNP_TEST(TrainingAutomaticRecordingSkipsPlacementForEverySlot) {
    TrainingMacros macros;
    macros.reset();
    uint64_t step = 1;
    for (uint32_t slot = 0; slot < qtraining::kTrainingMacroSlotCount; ++slot)
        QNP_REQUIRE(recordOneFrame(macros, step, slot, qinput::kLogicalA));
    for (uint32_t slot = 0; slot < qtraining::kTrainingMacroSlotCount; ++slot) {
        QNP_REQUIRE(macros.selectSlot(slot));
        QNP_REQUIRE(macros.requestRecording());
        QNP_CHECK(macros.state() == MacroState::RecordArmed);
        QNP_CHECK(!macros.requestRecording());
        MacroStepInput input{};
        input.paused = true;
        QNP_CHECK(macros.advance(step++, input).state == MacroState::RecordArmed);
        QNP_CHECK(macros.selectedFrameCount() == 1);
        input.paused = false;
        QNP_CHECK(macros.advance(step++, input).state == MacroState::Recording);
        const uint64_t recordStep = step++;
        QNP_REQUIRE(macros.advance(recordStep, input).recordFrameRequested);
        QNP_CHECK(!macros.requestRecording());
        QNP_REQUIRE(macros.commitRecordedFrame(recordStep, qinput::kLogicalB, 0).committed);
        QNP_REQUIRE(macros.requestStop());
        QNP_CHECK(macros.advance(step++, input).state == MacroState::Idle);
        QNP_CHECK(macros.selectedFrameCount() == 1);
        for (uint32_t other = 0; other < qtraining::kTrainingMacroSlotCount; ++other)
            QNP_CHECK(macros.slot(other).frameCount == 1);
    }
}

QNP_TEST(TrainingRecordKeyPlacesTheDummyBeforeItKeepsAnything) {
    TrainingMacros macros;
    macros.reset();
    uint64_t step = 1;

    QNP_REQUIRE(macros.requestRecordToggle());
    QNP_CHECK(macros.state() == MacroState::PreRecord);

    MacroStepInput input{};
    input.recordSourceMask = qinput::kLogicalRight;
    MacroStepResult placing = macros.advance(step++, input);
    QNP_CHECK(placing.state == MacroState::PreRecord);
    QNP_CHECK(placing.overrideP1 && placing.overrideP2);
    QNP_CHECK(placing.p1Mask == 0);
    QNP_CHECK(placing.p2Mask == qinput::kLogicalRight);
    QNP_CHECK(!placing.recordFrameRequested);
    QNP_CHECK(macros.selectedFrameCount() == 0);

    for (int held = 0; held < 30; ++held) {
        const MacroStepResult more = macros.advance(step++, input);
        QNP_CHECK(more.state == MacroState::PreRecord);
        QNP_CHECK(!more.recordFrameRequested);
    }
    QNP_CHECK(macros.selectedFrameCount() == 0);

    QNP_REQUIRE(macros.requestRecordToggle());
    QNP_CHECK(macros.state() == MacroState::RecordArmed);
    QNP_CHECK(macros.advance(step++, input).state == MacroState::Recording);
    const uint64_t recordStep = step++;
    QNP_CHECK(macros.advance(recordStep, input).recordFrameRequested);
    QNP_CHECK(macros.commitRecordedFrame(
                  recordStep, input.recordSourceMask, 0).committed);
    QNP_CHECK(macros.selectedFrameCount() == 1);

    QNP_REQUIRE(macros.requestRecordToggle());
    QNP_CHECK(macros.state() == MacroState::RecordStopping);
}
