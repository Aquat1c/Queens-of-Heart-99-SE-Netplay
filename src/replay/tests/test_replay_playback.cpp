#include "TestHarness.h"

#include "replay/ReplayPlaybackController.h"
#include "replay/ReplayTimeline.h"

#include <vector>

using namespace qreplay;

namespace {

constexpr uint64_t kInitialDigest = 0x0F1E2D3C4B5A6978ull;
constexpr uint64_t kInitialState = 0x0123456789ABCDEFull;
constexpr uint64_t kNominalSlotMicros = 16501ull;

uint64_t advanceState(uint64_t state, const std::array<uint16_t, 4>& actions) {
    for (size_t i = 0; i < actions.size(); ++i) {
        state ^= static_cast<uint64_t>(actions[i]) + 0x9E3779B97F4A7C15ull
                 + (state << 6) + (state >> 2);
        state *= 0x100000001B3ull;
    }
    return state;
}

uint16_t laneOneAction(uint32_t frame) {
    return static_cast<uint16_t>((frame % 4u) == 0u ? 0x0001u : 0x0100u);
}

uint16_t laneTwoAction(uint32_t frame) {
    return static_cast<uint16_t>((frame % 3u) == 0u ? 0x0002u : 0x0200u);
}

uint64_t fillTimeline(ConfirmedTimelineBuffer& timeline, uint32_t count,
                      bool sealTerminal) {
    DeterministicStartDescriptor start{};
    start.start.firstTimelineFrame = 0u;
    start.start.initialDigest = kInitialDigest;
    start.start.initialStateHash = kInitialState;
    start.activeLaneMask = 0x03u;
    start.activeSlotMask = 0x03u;
    timeline.reset(start);

    uint64_t digest = kInitialDigest;
    uint64_t state = kInitialState;
    for (uint32_t f = 0; f < count; ++f) {
        ReplayFrameV1 frame = makeFrameFromConfirmedPair(
            f, laneOneAction(f), laneTwoAction(f), state, 0u);
        state = advanceState(state, frame.actions);
        frame.postStateHash = state;
        digest = sealFrameDigest(digest, frame);
        ParseStatus why = ParseStatus::Ok;
        if (!timeline.append(frame, why)) return 0u;
    }
    if (sealTerminal) {
        TimelineTerminal terminal{};
        terminal.nextRecord = count;
        terminal.finalDigest = digest;
        terminal.postTailStateHash = state;
        timeline.setTerminal(terminal);
    }
    return state;
}

struct SlotRunner {
    ConfirmedTimelineBuffer& timeline;
    PlaybackController& controller;
    uint64_t state = kInitialState;
    uint64_t tickCostMicros = 1000u;

    uint32_t ticks = 0u;
    uint32_t presentations = 0u;
    uint32_t holds = 0u;
    uint32_t hidden = 0u;
    uint32_t slots = 0u;
    bool consumeFailed = false;

    SlotRunner(ConfirmedTimelineBuffer& t, PlaybackController& c)
        : timeline(t), controller(c) {}

    OuterSlotPlan runSlot(uint64_t elapsedMicros) {
        const OuterSlotPlan plan = controller.planOuterSlot(elapsedMicros);
        const uint32_t total = plan.totalTicks();
        for (uint32_t i = 0; i < total; ++i) doTick();
        if (plan.present) {
            ++presentations;
            if (plan.hold) ++holds;
        }
        hidden += plan.hiddenTicks;
        ++slots;
        return plan;
    }

    void doTick() {
        ReplayFrameV1 frame{};
        if (!timeline.peek(frame)) { consumeFailed = true; return; }
        ParseStatus why = ParseStatus::Ok;
        if (!timeline.consume(state, why)) { consumeFailed = true; return; }
        state = advanceState(state, frame.actions);
        timeline.recordPostState(frame.timelineFrame, state);
        controller.noteTickExecuted(tickCostMicros);
        ++ticks;
    }

    void reposition(uint64_t restoredState) { state = restoredState; }
};

PlaybackTuning generousTuning() {
    PlaybackTuning tuning{};
    tuning.slotBudgetMicros = 100000u;
    tuning.maxTicksPerSlot = 32u;
    tuning.initialTickCostMicros = 1000u;
    return tuning;
}

}

QNP_TEST(PlaybackNormalSpeedOwnsEverySlot) {
    ConfirmedTimelineBuffer timeline(256u);
    QNP_REQUIRE(fillTimeline(timeline, 64u, false) != 0u);
    PlaybackController controller(timeline, generousTuning());
    controller.beginPlayback();
    SlotRunner runner(timeline, controller);

    for (uint32_t i = 0; i < 8u; ++i) {
        const OuterSlotPlan plan = runner.runSlot(kNominalSlotMicros);
        QNP_CHECK(plan.hiddenTicks == 0u);
        QNP_CHECK(plan.present);
        QNP_CHECK(!plan.hold);
    }
    QNP_CHECK(!runner.consumeFailed);
    QNP_CHECK(runner.ticks == 8u);
    QNP_CHECK(runner.presentations == 8u);
    QNP_CHECK(runner.holds == 0u);
    QNP_CHECK(runner.hidden == 0u);
    QNP_CHECK(timeline.consumeCursor() == 8u);
    QNP_CHECK(controller.status().phase == PlaybackPhase::Live);
    QNP_CHECK(controller.status().lastVerifiedFrame == 7u);
}

QNP_TEST(PlaybackFastSpeedsRunBoundedHiddenTicks) {
    struct Case { uint32_t permille; uint32_t perSlot; };
    const Case cases[] = {
        {kSpeedDouble, 2u}, {kSpeedQuadruple, 4u}, {kSpeedOctuple, 8u}};

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        ConfirmedTimelineBuffer timeline(256u);
        QNP_REQUIRE(fillTimeline(timeline, 128u, false) != 0u);
        PlaybackController controller(timeline, generousTuning());
        controller.beginPlayback();
        controller.submit(PlaybackCommand::SpeedSet, cases[c].permille);
        SlotRunner runner(timeline, controller);

        for (uint32_t i = 0; i < 6u; ++i) {
            const OuterSlotPlan plan = runner.runSlot(kNominalSlotMicros);
            QNP_CHECK(plan.present);
            QNP_CHECK(!plan.hold);
            QNP_CHECK(plan.hiddenTicks == cases[c].perSlot - 1u);
            QNP_CHECK(plan.totalTicks() == cases[c].perSlot);
        }
        QNP_CHECK(!runner.consumeFailed);
        QNP_CHECK(runner.ticks == cases[c].perSlot * 6u);
        QNP_CHECK(runner.presentations == 6u);
        QNP_CHECK(runner.holds == 0u);
        QNP_CHECK(controller.speedPermille() == cases[c].permille);
        QNP_CHECK(controller.status().phase == PlaybackPhase::CatchingUp);
    }
}

QNP_TEST(PlaybackSlowSpeedsRepeatPresentationWithoutExtraTicks) {
    struct Case { uint32_t permille; uint32_t slots; uint32_t expectedTicks; };
    const Case cases[] = {{kSpeedHalf, 8u, 4u}, {kSpeedQuarter, 16u, 4u}};

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        ConfirmedTimelineBuffer timeline(256u);
        QNP_REQUIRE(fillTimeline(timeline, 64u, false) != 0u);
        PlaybackController controller(timeline, generousTuning());
        controller.beginPlayback();
        controller.submit(PlaybackCommand::SpeedSet, cases[c].permille);
        SlotRunner runner(timeline, controller);

        for (uint32_t i = 0; i < cases[c].slots; ++i) {
            const OuterSlotPlan plan = runner.runSlot(kNominalSlotMicros);
            QNP_CHECK(plan.present);
            QNP_CHECK(plan.hiddenTicks == 0u);
            QNP_CHECK(plan.totalTicks() <= 1u);
        }
        QNP_CHECK(!runner.consumeFailed);
        QNP_CHECK(runner.ticks == cases[c].expectedTicks);
        QNP_CHECK(runner.presentations == cases[c].slots);
        QNP_CHECK(runner.holds == cases[c].slots - cases[c].expectedTicks);
        QNP_CHECK(timeline.consumeCursor() == cases[c].expectedTicks);
    }
}

QNP_TEST(PlaybackRejectsUnsupportedSpeed) {
    ConfirmedTimelineBuffer timeline(64u);
    QNP_REQUIRE(fillTimeline(timeline, 16u, false) != 0u);
    PlaybackController controller(timeline, generousTuning());
    controller.beginPlayback();

    controller.submit(PlaybackCommand::SpeedSet, 1500u);
    controller.planOuterSlot(kNominalSlotMicros);
    QNP_CHECK(controller.speedPermille() == kSpeedNormal);

    controller.submit(PlaybackCommand::SpeedSet, 0u);
    controller.planOuterSlot(kNominalSlotMicros);
    QNP_CHECK(controller.speedPermille() == kSpeedNormal);

    controller.submit(PlaybackCommand::SpeedSet, kSpeedOctuple);
    controller.planOuterSlot(kNominalSlotMicros);
    QNP_CHECK(controller.speedPermille() == kSpeedOctuple);
}

QNP_TEST(PlaybackPauseHoldsAndStepAdvancesExactlyOneTick) {
    ConfirmedTimelineBuffer timeline(256u);
    QNP_REQUIRE(fillTimeline(timeline, 64u, false) != 0u);
    PlaybackController controller(timeline, generousTuning());
    controller.beginPlayback();
    SlotRunner runner(timeline, controller);

    runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(runner.ticks == 1u);

    controller.submit(PlaybackCommand::Pause, 0u);
    for (uint32_t i = 0; i < 5u; ++i) {
        const OuterSlotPlan plan = runner.runSlot(kNominalSlotMicros);
        QNP_CHECK(plan.present);
        QNP_CHECK(plan.hold);
        QNP_CHECK(plan.totalTicks() == 0u);
    }
    QNP_CHECK(runner.ticks == 1u);
    QNP_CHECK(controller.paused());

    controller.submit(PlaybackCommand::Step, 0u);
    const OuterSlotPlan stepped = runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(stepped.present);
    QNP_CHECK(!stepped.hold);
    QNP_CHECK(stepped.totalTicks() == 1u);
    QNP_CHECK(runner.ticks == 2u);

    const OuterSlotPlan after = runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(after.hold);
    QNP_CHECK(runner.ticks == 2u);

    controller.submit(PlaybackCommand::Resume, 0u);
    runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(runner.ticks == 3u);
    QNP_CHECK(!controller.paused());
    QNP_CHECK(!runner.consumeFailed);
}

QNP_TEST(PlaybackStepFromRunningImpliesPause) {
    ConfirmedTimelineBuffer timeline(64u);
    QNP_REQUIRE(fillTimeline(timeline, 32u, false) != 0u);
    PlaybackController controller(timeline, generousTuning());
    controller.beginPlayback();
    SlotRunner runner(timeline, controller);

    controller.submit(PlaybackCommand::Step, 0u);
    const OuterSlotPlan plan = runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(plan.totalTicks() == 1u);
    QNP_CHECK(controller.paused());
    runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(runner.ticks == 1u);
}

QNP_TEST(PlaybackSlotBudgetCapsHiddenTicks) {
    ConfirmedTimelineBuffer timeline(256u);
    QNP_REQUIRE(fillTimeline(timeline, 128u, false) != 0u);
    PlaybackTuning tuning{};
    tuning.slotBudgetMicros = 20000u;
    tuning.initialTickCostMicros = 5000u;
    tuning.maxTicksPerSlot = 32u;
    PlaybackController controller(timeline, tuning);
    controller.beginPlayback();
    controller.submit(PlaybackCommand::SpeedSet, kSpeedOctuple);
    SlotRunner runner(timeline, controller);
    runner.tickCostMicros = 5000u;

    for (uint32_t i = 0; i < 4u; ++i) {
        const OuterSlotPlan plan = runner.runSlot(kNominalSlotMicros);
        QNP_CHECK(plan.totalTicks() == 4u);
        QNP_CHECK(plan.hiddenTicks == 3u);
        QNP_CHECK(plan.present);
    }
    QNP_CHECK(runner.ticks == 16u);
    QNP_CHECK(!runner.consumeFailed);
    QNP_CHECK(timeline.consumeCursor() == 16u);

    PlaybackTuning capped{};
    capped.slotBudgetMicros = 1000000u;
    capped.initialTickCostMicros = 1u;
    capped.maxTicksPerSlot = 3u;
    ConfirmedTimelineBuffer other(256u);
    QNP_REQUIRE(fillTimeline(other, 128u, false) != 0u);
    PlaybackController limited(other, capped);
    limited.beginPlayback();
    limited.submit(PlaybackCommand::SpeedSet, kSpeedOctuple);
    SlotRunner limitedRunner(other, limited);
    const OuterSlotPlan plan = limitedRunner.runSlot(kNominalSlotMicros);
    QNP_CHECK(plan.totalTicks() == 3u);
}

QNP_TEST(PlaybackResetsDeadlineAfterPause) {
    ConfirmedTimelineBuffer timeline(256u);
    QNP_REQUIRE(fillTimeline(timeline, 128u, false) != 0u);
    PlaybackController controller(timeline, generousTuning());
    controller.beginPlayback();
    SlotRunner runner(timeline, controller);

    runner.runSlot(kNominalSlotMicros);
    controller.submit(PlaybackCommand::Pause, 0u);
    runner.runSlot(5000000ull);
    QNP_CHECK(runner.ticks == 1u);

    controller.submit(PlaybackCommand::Resume, 0u);
    const OuterSlotPlan resumed = runner.runSlot(5000000ull);
    QNP_CHECK(resumed.totalTicks() == 1u);
    QNP_CHECK(runner.ticks == 2u);

    const OuterSlotPlan hitch = runner.runSlot(5000000ull);
    QNP_CHECK(hitch.totalTicks() <= 4u);
    QNP_CHECK(!runner.consumeFailed);
}

QNP_TEST(PlaybackExplicitDeadlineResetDropsCredit) {
    ConfirmedTimelineBuffer timeline(256u);
    QNP_REQUIRE(fillTimeline(timeline, 128u, false) != 0u);
    PlaybackController controller(timeline, generousTuning());
    controller.beginPlayback();
    SlotRunner runner(timeline, controller);

    runner.runSlot(kNominalSlotMicros);
    controller.resetDeadlineAfterPauseOrSeek();
    const OuterSlotPlan plan = runner.runSlot(1000000ull);
    QNP_CHECK(plan.totalTicks() == 1u);
}

QNP_TEST(PlaybackBuffersWhenStarvedAndCompletesAtTerminal) {
    ConfirmedTimelineBuffer timeline(64u);
    const uint64_t finalState = fillTimeline(timeline, 4u, true);
    QNP_REQUIRE(finalState != 0u);
    PlaybackController controller(timeline, generousTuning());
    controller.beginPlayback();
    SlotRunner runner(timeline, controller);

    for (uint32_t i = 0; i < 4u; ++i) runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(runner.ticks == 4u);
    QNP_CHECK(runner.state == finalState);
    QNP_CHECK(controller.status().phase == PlaybackPhase::Complete);

    for (uint32_t i = 0; i < 3u; ++i) {
        const OuterSlotPlan plan = runner.runSlot(kNominalSlotMicros);
        QNP_CHECK(plan.present && plan.hold);
        QNP_CHECK(plan.totalTicks() == 0u);
    }
    QNP_CHECK(runner.ticks == 4u);
    QNP_CHECK(!runner.consumeFailed);

    ParseStatus why = ParseStatus::Ok;
    QNP_CHECK(timeline.verifyTerminal(why));

    ConfirmedTimelineBuffer starved(64u);
    QNP_REQUIRE(fillTimeline(starved, 1u, false) != 0u);
    PlaybackController hungry(starved, generousTuning());
    hungry.beginPlayback();
    SlotRunner hungryRunner(starved, hungry);
    hungryRunner.runSlot(kNominalSlotMicros);
    QNP_CHECK(hungryRunner.ticks == 1u);
    const OuterSlotPlan idle = hungryRunner.runSlot(kNominalSlotMicros);
    QNP_CHECK(idle.present && idle.hold && idle.totalTicks() == 0u);
    QNP_CHECK(hungry.status().phase == PlaybackPhase::Buffering);
    QNP_CHECK(hungry.status().buffering);
}

QNP_TEST(PlaybackPresentsNothingBeforeBootstrapCompletes) {
    ConfirmedTimelineBuffer timeline(64u);
    QNP_REQUIRE(fillTimeline(timeline, 16u, false) != 0u);
    PlaybackController controller(timeline, generousTuning());
    QNP_CHECK(controller.status().phase == PlaybackPhase::Preflight);
    OuterSlotPlan plan = controller.planOuterSlot(kNominalSlotMicros);
    QNP_CHECK(!plan.present && plan.totalTicks() == 0u);

    controller.beginBootstrap();
    QNP_CHECK(controller.status().phase == PlaybackPhase::Bootstrapping);
    plan = controller.planOuterSlot(kNominalSlotMicros);
    QNP_CHECK(!plan.present && plan.totalTicks() == 0u);

    controller.beginPlayback();
    plan = controller.planOuterSlot(kNominalSlotMicros);
    QNP_CHECK(plan.present && plan.totalTicks() == 1u);

    controller.noteFailure(ParseStatus::DigestMismatch);
    QNP_CHECK(controller.status().phase == PlaybackPhase::Failed);
    QNP_CHECK(controller.status().failure == ParseStatus::DigestMismatch);
    plan = controller.planOuterSlot(kNominalSlotMicros);
    QNP_CHECK(plan.hold && plan.totalTicks() == 0u);
}

QNP_TEST(PlaybackSeekToMatchEqualsLinearPlayback) {
    constexpr uint32_t kFrames = 64u;
    constexpr uint32_t kMatchTwoFrame = 30u;

    ConfirmedTimelineBuffer linear(256u);
    QNP_REQUIRE(fillTimeline(linear, kFrames, false) != 0u);
    PlaybackController linearController(linear, generousTuning());
    linearController.beginPlayback();
    SlotRunner linearRunner(linear, linearController);
    while (linear.consumeCursor() < kMatchTwoFrame) {
        linearRunner.runSlot(kNominalSlotMicros);
    }
    QNP_REQUIRE(!linearRunner.consumeFailed);
    QNP_CHECK(linear.consumeCursor() == kMatchTwoFrame);
    const uint64_t linearState = linearRunner.state;
    const uint64_t linearDigest = linear.rollingDigest();

    ConfirmedTimelineBuffer seeked(256u);
    QNP_REQUIRE(fillTimeline(seeked, kFrames, false) != 0u);
    PlaybackController controller(seeked, generousTuning());
    QNP_CHECK(controller.addMatchMarker(1u, 0u, kMatchTwoFrame - 1u));
    QNP_CHECK(controller.addMatchMarker(2u, kMatchTwoFrame, kFrames - 1u));
    QNP_CHECK(controller.matchCount() == 2u);
    controller.beginPlayback();
    SlotRunner runner(seeked, controller);

    for (uint32_t i = 0; i < 5u; ++i) runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(controller.currentMatchOrdinal() == 1u);

    controller.submit(PlaybackCommand::SeekMatch, 2u);
    const OuterSlotPlan pending = runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(pending.hold);
    QNP_CHECK(pending.totalTicks() == 0u);
    QNP_CHECK(controller.seeking());

    SeekRequest request{};
    QNP_REQUIRE(controller.takeSeekRequest(request));
    QNP_CHECK(request.active);
    QNP_CHECK(request.targetFrame == kMatchTwoFrame);
    QNP_CHECK(request.matchOrdinal == 2u);
    QNP_CHECK(!controller.takeSeekRequest(request));

    runner.reposition(kInitialState);
    QNP_REQUIRE(controller.completeSeek(0u));
    QNP_CHECK(seeked.consumeCursor() == 0u);

    const uint32_t presentationsBefore = runner.presentations;
    uint32_t guard = 0u;
    while (controller.seeking() && guard++ < 100u) {
        const OuterSlotPlan plan = runner.runSlot(kNominalSlotMicros);
        if (controller.seeking()) {
            QNP_CHECK(!plan.present);
            QNP_CHECK(plan.totalTicks() > 0u);
            QNP_CHECK(plan.totalTicks() <= 32u);
        }
    }
    QNP_CHECK(guard < 100u);
    QNP_CHECK(!runner.consumeFailed);
    QNP_CHECK(seeked.consumeCursor() == kMatchTwoFrame);
    QNP_CHECK(runner.state == linearState);
    QNP_CHECK(seeked.rollingDigest() == linearDigest);
    QNP_CHECK(controller.currentMatchOrdinal() == 2u);
    QNP_CHECK(runner.presentations > presentationsBefore);

    QNP_CHECK(!controller.seeking());
}

QNP_TEST(PlaybackMatchNavigationUsesMarkerIndex) {
    ConfirmedTimelineBuffer timeline(256u);
    QNP_REQUIRE(fillTimeline(timeline, 64u, false) != 0u);
    PlaybackController controller(timeline, generousTuning());
    QNP_CHECK(controller.addMatchMarker(1u, 0u, 19u));
    QNP_CHECK(controller.addMatchMarker(2u, 20u, 39u));
    QNP_CHECK(controller.addMatchMarker(3u, 40u, 63u));
    QNP_CHECK(!controller.addMatchMarker(2u, 10u, 15u));
    QNP_CHECK(!controller.addMatchMarker(4u, 10u, 15u));
    controller.beginPlayback();
    SlotRunner runner(timeline, controller);

    controller.submit(PlaybackCommand::SeekMatch, 3u);
    controller.planOuterSlot(kNominalSlotMicros);
    SeekRequest request{};
    QNP_REQUIRE(controller.takeSeekRequest(request));
    QNP_CHECK(request.targetFrame == 40u);

    runner.reposition(runner.state);
    QNP_REQUIRE(controller.completeSeek(0u));
    uint32_t guard = 0u;
    while (controller.seeking() && guard++ < 100u) {
        runner.runSlot(kNominalSlotMicros);
    }
    QNP_CHECK(timeline.consumeCursor() == 40u);
    QNP_CHECK(controller.currentMatchOrdinal() == 3u);

    controller.submit(PlaybackCommand::PreviousMatch, 0u);
    controller.planOuterSlot(kNominalSlotMicros);
    QNP_REQUIRE(controller.takeSeekRequest(request));
    QNP_CHECK(request.targetFrame == 20u);
    QNP_CHECK(request.matchOrdinal == 2u);
    QNP_REQUIRE(controller.completeSeek(20u));

    controller.submit(PlaybackCommand::NextMatch, 0u);
    controller.planOuterSlot(kNominalSlotMicros);
    QNP_REQUIRE(controller.takeSeekRequest(request));
    QNP_CHECK(request.targetFrame == 40u);

    QNP_REQUIRE(controller.completeSeek(40u));
    controller.submit(PlaybackCommand::Restart, 0u);
    controller.planOuterSlot(kNominalSlotMicros);
    QNP_REQUIRE(controller.takeSeekRequest(request));
    QNP_CHECK(request.targetFrame == 40u);
    QNP_REQUIRE(controller.completeSeek(40u));

    controller.submit(PlaybackCommand::Restart, 1u);
    controller.planOuterSlot(kNominalSlotMicros);
    QNP_REQUIRE(controller.takeSeekRequest(request));
    QNP_CHECK(request.targetFrame == 0u);
}

QNP_TEST(PlaybackSeekBelowRetentionFails) {
    ConfirmedTimelineBuffer timeline(256u);
    QNP_REQUIRE(fillTimeline(timeline, 64u, false) != 0u);
    PlaybackController controller(timeline, generousTuning());
    QNP_CHECK(controller.addMatchMarker(1u, 0u, 63u));
    controller.beginPlayback();
    SlotRunner runner(timeline, controller);
    for (uint32_t i = 0; i < 20u; ++i) runner.runSlot(kNominalSlotMicros);
    timeline.trimConsumedBelow(15u);

    controller.submit(PlaybackCommand::SeekMatch, 1u);
    controller.planOuterSlot(kNominalSlotMicros);
    SeekRequest request{};
    QNP_REQUIRE(controller.takeSeekRequest(request));
    QNP_CHECK(!controller.completeSeek(0u));
    QNP_CHECK(controller.status().phase == PlaybackPhase::Failed);
    QNP_CHECK(controller.failure() == ParseStatus::SequenceRegression);
    QNP_CHECK(controller.lastFault() == TimelineFault::RewindBelowRetention);
}

QNP_TEST(PlaybackCommandQueueIsBounded) {
    ConfirmedTimelineBuffer timeline(64u);
    QNP_REQUIRE(fillTimeline(timeline, 16u, false) != 0u);
    PlaybackTuning tuning = generousTuning();
    tuning.commandQueueDepth = 4u;
    PlaybackController controller(timeline, tuning);
    controller.beginPlayback();

    for (uint32_t i = 0; i < 32u; ++i) {
        controller.submit(PlaybackCommand::None, 0u);
    }
    QNP_CHECK(controller.droppedCommands() == 28u);
    controller.planOuterSlot(kNominalSlotMicros);
    controller.submit(PlaybackCommand::Exit, 0u);
    controller.planOuterSlot(kNominalSlotMicros);
    QNP_CHECK(controller.exitRequested());
}

QNP_TEST(PlaybackStatusReportsHonestSnapshot) {
    ConfirmedTimelineBuffer timeline(256u);
    QNP_REQUIRE(fillTimeline(timeline, 40u, true) != 0u);
    PlaybackController controller(timeline, generousTuning());
    QNP_CHECK(controller.addMatchMarker(1u, 0u, 19u));
    QNP_CHECK(controller.addMatchMarker(2u, 20u, 39u));
    controller.beginPlayback();
    SlotRunner runner(timeline, controller);
    for (uint32_t i = 0; i < 25u; ++i) runner.runSlot(kNominalSlotMicros);

    const PlaybackStatus status = controller.status();
    QNP_CHECK(status.currentFrame == 25u);
    QNP_CHECK(status.lastVerifiedFrame == 24u);
    QNP_CHECK(status.matchOrdinal == 2u);
    QNP_CHECK(status.matchCount == 2u);
    QNP_CHECK(status.speedPermille == kSpeedNormal);
    QNP_CHECK(!status.paused);
    QNP_CHECK(!status.seeking);
    QNP_CHECK(!status.buffering);
    QNP_CHECK(status.bufferedFrames == 15u);
    QNP_CHECK(status.rollingDigest == timeline.rollingDigest());
    QNP_CHECK(status.failure == ParseStatus::Ok);
    QNP_CHECK(status.phase == PlaybackPhase::Live);

    controller.submit(PlaybackCommand::SpeedSet, kSpeedQuadruple);
    runner.runSlot(kNominalSlotMicros);
    QNP_CHECK(controller.status().phase == PlaybackPhase::SealedCatchUp);
}
