#include "TestHarness.h"

#include "../RollbackSession.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

static uint16_t canonWord(uint32_t n) {
    return static_cast<uint16_t>((n & 0x0Fu) | ((n & 0xF0u) << 4));
}

namespace {

using namespace qnetplay;

RollbackConfig baseConfig() {
    RollbackConfig config{};
    config.localPlayer = 0;
    config.inputDelayFrames = 0;
    config.maxRollbackFrames = 4;
    config.neutralInput = 0;
    config.firstFrame = 0;
    config.maxRemoteFutureFrames = 32;
    config.historyCapacity = 64;
    return config;
}

std::vector<TaggedInput> drainSubmissions(RollbackSession& session) {
    std::vector<TaggedInput> result;
    TaggedInput input{};
    while (session.popLocalSubmission(input)) result.push_back(input);
    return result;
}

}

QNP_TEST(RollbackSessionRejectsUnsafeConfiguration) {
    RollbackConfig config = baseConfig();
    config.localPlayer = 2;
    QNP_CHECK(!RollbackSession(config).valid());

    config = baseConfig();
    config.firstFrame = std::numeric_limits<uint32_t>::max();
    QNP_CHECK(!RollbackSession(config).valid());

    config = baseConfig();
    config.maxRemoteFutureFrames =
        static_cast<uint32_t>(config.historyCapacity);
    QNP_CHECK(!RollbackSession(config).valid());

    config = baseConfig();
    config.firstFrame = std::numeric_limits<uint32_t>::max() - 2u;
    config.inputDelayFrames = 2;
    QNP_CHECK(!RollbackSession(config).valid());

    config = baseConfig();
    config.historyCapacity = 8;
    config.inputDelayFrames = 3;
    config.maxRollbackFrames = 3;
    QNP_CHECK(!RollbackSession(config).valid());
}

QNP_TEST(RollbackSessionAppliesPeerLocalDelayOnceAndCapsPrediction) {
    RollbackConfig config = baseConfig();
    config.inputDelayFrames = 3;
    config.maxRollbackFrames = 2;
    RollbackSession session(config);
    QNP_CHECK(session.valid());

    QNP_CHECK(session.needsLocalCapture());
    QNP_CHECK(session.captureLocalInput(0, 5));
    QNP_CHECK(!session.needsLocalCapture());
    QNP_CHECK(session.captureLocalInput(0, 5));
    QNP_CHECK(!session.captureLocalInput(0, 6));

    const std::vector<TaggedInput> initial = drainSubmissions(session);
    QNP_REQUIRE(initial.size() == 4);
    QNP_CHECK(initial[0].frame == 0 && initial[0].value == 0);
    QNP_CHECK(initial[1].frame == 1 && initial[1].value == 0);
    QNP_CHECK(initial[2].frame == 2 && initial[2].value == 0);
    QNP_CHECK(initial[3].frame == 3 && initial[3].value == 5);

    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(action.inputs.frame == 0
              && action.inputs.p1 == 0
              && action.inputs.p2 == 0
              && action.inputs.remotePredicted);
    QNP_CHECK(session.commitAdvance(action.inputs, 100));

    QNP_CHECK(session.captureLocalInput(1, 6));
    TaggedInput submission{};
    QNP_CHECK(session.popLocalSubmission(submission));
    QNP_CHECK(submission.frame == 4 && submission.value == 6);
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 101));

    QNP_CHECK(session.captureLocalInput(2, 7));
    QNP_CHECK(session.popLocalSubmission(submission));
    QNP_CHECK(submission.frame == 5 && submission.value == 7);
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Stall);
    QNP_CHECK(action.stallReason == StallReason::PredictionLimit);
    QNP_CHECK(session.currentFrame() == 2);
    QNP_CHECK(session.stats().uniqueCaptures == 3);
    QNP_CHECK(session.stats().repeatedCaptureCalls == 2);
    QNP_CHECK(session.stats().maxSpeculativeFrames == 2);
}

QNP_TEST(SessionEventNeutralDrainBypassesDelayedSourceCapture) {
    RollbackConfig config = baseConfig();
    config.inputDelayFrames = 1;
    RollbackSession session(config);
    QNP_REQUIRE(session.valid());
    (void)drainSubmissions(session);

    for (uint32_t frame = 0; frame < 4; ++frame) {
        QNP_CHECK(session.captureLocalInput(frame, canonWord(frame + 1u)));
        QNP_CHECK(session.receiveRemoteInput(frame, canonWord(frame + 16u))
                  == RemoteInputResult::Inserted);
        const FrameAction action = session.nextAction();
        QNP_REQUIRE(action.kind == ActionKind::Advance);
        QNP_CHECK(session.commitAdvance(action.inputs, 100u + frame));
    }
    QNP_CHECK(session.currentFrame() == 4u);
    QNP_CHECK(session.assignedLocalTailExclusive() == 5u);

    QNP_CHECK(session.fillLocalNeutralThrough(5u));
    QNP_CHECK(session.assignedLocalTailExclusive() == 5u);
    QNP_CHECK(session.needsLocalCapture());
    const FrameAction ordinaryAction = session.nextAction();
    QNP_CHECK(ordinaryAction.kind == ActionKind::Stall);
    QNP_CHECK(ordinaryAction.stallReason == StallReason::LocalInputMissing);

    const FrameAction boundaryAction = session.nextQuiesceDrainAction();
    QNP_REQUIRE(boundaryAction.kind == ActionKind::Advance);
    QNP_CHECK(boundaryAction.frame == 4u);
    QNP_CHECK(boundaryAction.inputs.remotePredicted);
    QNP_CHECK(session.commitAdvance(boundaryAction.inputs, 104u));
    QNP_CHECK(session.currentFrame() == 5u);
    uint16_t unassigned = 0u;
    QNP_CHECK(!session.getLocalInput(5u, unassigned));
    QNP_CHECK(session.assignedLocalTailExclusive() == 5u);

    QNP_CHECK(session.needsLocalCapture());

    QNP_CHECK(session.receiveRemoteInput(4u, canonWord(20u))
              == RemoteInputResult::Inserted);
    QNP_CHECK(session.hasPendingRollback());
    const FrameAction correction = session.nextQuiesceDrainAction();
    QNP_CHECK(correction.kind == ActionKind::Rollback);
    QNP_CHECK(correction.rollbackFrom == 4u);
    QNP_CHECK(correction.replayUntilExclusive == 5u);
}

QNP_TEST(RollbackSessionIncreasesDelayWithoutReplacingScheduledInputs) {
    RollbackConfig config = baseConfig();
    config.inputDelayFrames = 1;
    config.neutralInput = 0x0E0E;
    RollbackSession session(config);
    (void)drainSubmissions(session);

    QNP_CHECK(session.captureLocalInput(0, 0x0100));
    (void)drainSubmissions(session);
    QNP_CHECK(session.receiveRemoteInput(0, 0x0200)
              == RemoteInputResult::Inserted);
    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 100));

    QNP_CHECK(session.captureLocalInput(1, 0x0101));
    QNP_CHECK(session.receiveRemoteInput(1, 0x0201)
              == RemoteInputResult::Inserted);
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 101));
    QNP_CHECK(session.currentFrame() == 2);
    QNP_CHECK(session.stableConfirmedThrough() == 1);

    QNP_CHECK(session.receiveRemoteInput(5, 0x0205)
              == RemoteInputResult::Inserted);
    QNP_CHECK(session.changeInputDelayAtConfirmedBoundary(4));
    QNP_CHECK(session.config().inputDelayFrames == 4);

    uint16_t value = 0;
    QNP_CHECK(session.getLocalInput(2, value) && value == 0x0101);
    QNP_CHECK(session.getRemoteActual(5, value) && value == 0x0205);
    const std::vector<TaggedInput> submissions = drainSubmissions(session);
    QNP_REQUIRE(submissions.size() == 4);
    QNP_CHECK(submissions[0].frame == 2
              && submissions[0].value == 0x0101);
    QNP_CHECK(submissions[1].frame == 3
              && submissions[1].value == 0x0E0E);
    QNP_CHECK(submissions[2].frame == 4
              && submissions[2].value == 0x0E0E);
    QNP_CHECK(submissions[3].frame == 5
              && submissions[3].value == 0x0E0E);

    QNP_CHECK(session.needsLocalCapture());
    QNP_CHECK(session.captureLocalInput(2, 0x0102));
    TaggedInput capture{};
    QNP_CHECK(session.popLocalSubmission(capture));
    QNP_CHECK(capture.frame == 6 && capture.value == 0x0102);

    QNP_CHECK(session.changeInputDelayAtConfirmedBoundary(5));
    QNP_CHECK(session.config().inputDelayFrames == 5);
    QNP_CHECK(!session.needsLocalCapture());
    QNP_CHECK(session.captureLocalInput(2, 0x0102));
    QNP_CHECK(!session.captureLocalInput(2, 0xFFFF));
    QNP_CHECK(session.getLocalInput(6, value) && value == 0x0102);
    QNP_CHECK(session.getLocalInput(7, value) && value == 0x0E0E);
    QNP_CHECK(session.pendingLocalSubmissions() == 1);
    QNP_CHECK(session.popLocalSubmission(capture));
    QNP_CHECK(capture.frame == 7 && capture.value == 0x0E0E);
    QNP_CHECK(session.pendingLocalSubmissions() == 0);
}

QNP_TEST(RollbackSessionDecreasesDelayByDrainingImmutableFutureInputs) {
    RollbackConfig config = baseConfig();
    config.inputDelayFrames = 4;
    config.neutralInput = 0x0C0C;
    RollbackSession session(config);
    (void)drainSubmissions(session);

    QNP_CHECK(session.captureLocalInput(0, 0x010D));
    (void)drainSubmissions(session);
    QNP_CHECK(session.receiveRemoteInput(0, 0x0203)
              == RemoteInputResult::Inserted);
    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 200));

    QNP_CHECK(session.captureLocalInput(1, 0x010E));
    (void)drainSubmissions(session);
    QNP_CHECK(session.receiveRemoteInput(1, 0x0204)
              == RemoteInputResult::Inserted);
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 201));
    QNP_CHECK(session.currentFrame() == 2);

    QNP_CHECK(session.captureLocalInput(2, 0x010F));
    for (uint32_t frame = 2; frame <= 6; ++frame) {
        QNP_CHECK(session.receiveRemoteInput(
                      frame, canonWord(0xA0u + frame))
                  == RemoteInputResult::Inserted);
    }

    QNP_CHECK(session.changeInputDelayAtConfirmedBoundary(1));
    QNP_CHECK(session.config().inputDelayFrames == 1);
    QNP_CHECK(!session.needsLocalCapture());
    QNP_CHECK(session.pendingLocalSubmissions() == 1);
    TaggedInput submission{};
    QNP_CHECK(session.popLocalSubmission(submission));
    QNP_CHECK(submission.frame == 6 && submission.value == 0x010F);
    QNP_CHECK(session.pendingLocalSubmissions() == 0);

    uint16_t value = 0;
    QNP_CHECK(session.getLocalInput(3, value) && value == 0x0C0C);
    QNP_CHECK(session.getLocalInput(4, value) && value == 0x010D);
    QNP_CHECK(session.getLocalInput(5, value) && value == 0x010E);
    QNP_CHECK(session.getLocalInput(6, value) && value == 0x010F);
    QNP_CHECK(session.getRemoteActual(6, value) && value == 0x0A06);

    for (uint32_t frame = 2; frame < 6; ++frame) {
        QNP_CHECK(session.currentFrame() == frame);
        QNP_CHECK(!session.needsLocalCapture());
        action = session.nextAction();
        QNP_CHECK(action.kind == ActionKind::Advance);
        QNP_CHECK(!action.inputs.remotePredicted);
        QNP_CHECK(session.commitAdvance(action.inputs, 200 + frame));
        QNP_CHECK(session.pendingLocalSubmissions() == 0);
    }

    QNP_CHECK(session.currentFrame() == 6);
    QNP_CHECK(session.needsLocalCapture());
    QNP_CHECK(session.captureLocalInput(6, 0x0202));
    QNP_CHECK(session.captureLocalInput(6, 0x0202));
    QNP_CHECK(!session.captureLocalInput(6, 0xFFFF));
    QNP_CHECK(session.pendingLocalSubmissions() == 1);
    QNP_CHECK(session.popLocalSubmission(submission));
    QNP_CHECK(submission.frame == 7 && submission.value == 0x0202);
    QNP_CHECK(session.pendingLocalSubmissions() == 0);
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(action.inputs.p1 == 0x010F);
    QNP_CHECK(session.commitAdvance(action.inputs, 206));
}

QNP_TEST(RollbackSessionRejectsUnsafeOrUnconfirmedDelayChanges) {
    RollbackSession unconfirmed(baseConfig());
    QNP_CHECK(unconfirmed.captureLocalInput(0, 2));
    FrameAction action = unconfirmed.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(action.inputs.remotePredicted);
    QNP_CHECK(unconfirmed.commitAdvance(action.inputs, 300));
    QNP_CHECK(
        unconfirmed.tryChangeInputDelayAtConfirmedBoundary(2)
        == InputDelayChangeResult::Deferred);
    QNP_CHECK(
        unconfirmed.inputDelayChangeBlocker(2)
        == InputDelayChangeBlocker::UnconfirmedBoundary);
    QNP_CHECK(unconfirmed.config().inputDelayFrames == 0);

    RollbackSession replaying(baseConfig());
    QNP_CHECK(replaying.captureLocalInput(0, 3));
    action = replaying.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(replaying.commitAdvance(action.inputs, 301));
    QNP_CHECK(replaying.receiveRemoteInput(0, 9)
              == RemoteInputResult::Inserted);
    QNP_CHECK(replaying.hasPendingRollback());
    QNP_CHECK(
        replaying.inputDelayChangeBlocker(2)
        == InputDelayChangeBlocker::PendingMismatch);
    QNP_CHECK(
        replaying.tryChangeInputDelayAtConfirmedBoundary(2)
        == InputDelayChangeResult::Deferred);
    action = replaying.nextAction();
    QNP_CHECK(action.kind == ActionKind::Rollback);
    QNP_CHECK(replaying.beginRollback(
        action.rollbackFrom, action.replayUntilExclusive));
    QNP_CHECK(!replaying.changeInputDelayAtConfirmedBoundary(2));
    QNP_CHECK(replaying.config().inputDelayFrames == 0);

    RollbackSession invalid(baseConfig());
    QNP_CHECK(
        invalid.tryChangeInputDelayAtConfirmedBoundary(58)
        == InputDelayChangeResult::Invalid);
    QNP_CHECK(invalid.config().inputDelayFrames == 0);
    QNP_CHECK(invalid.pendingLocalSubmissions() == 0);

    RollbackConfig nearEndConfig = baseConfig();
    nearEndConfig.firstFrame =
        std::numeric_limits<uint32_t>::max() - 2u;
    RollbackSession nearEnd(nearEndConfig);
    QNP_CHECK(nearEnd.valid());
    QNP_CHECK(
        nearEnd.tryChangeInputDelayAtConfirmedBoundary(2)
        == InputDelayChangeResult::Invalid);
    QNP_CHECK(nearEnd.config().inputDelayFrames == 0);

    QNP_CHECK(invalid.changeInputDelayAtConfirmedBoundary(3));
    const std::vector<TaggedInput> prefix = drainSubmissions(invalid);
    QNP_REQUIRE(prefix.size() == 3);
    QNP_CHECK(invalid.changeInputDelayAtConfirmedBoundary(3));
    QNP_CHECK(invalid.pendingLocalSubmissions() == 0);
}

QNP_TEST(RollbackSessionConfirmedDrainAppliesAfterSustainedSpeculation) {
    RollbackConfig config = baseConfig();
    config.inputDelayFrames = 1;
    config.maxRollbackFrames = 8;
    RollbackSession session(config);
    (void)drainSubmissions(session);

    for (uint32_t frame = 0; frame < 6; ++frame) {
        QNP_CHECK(session.currentFrame() == frame);
        QNP_CHECK(session.captureLocalInput(
            frame, canonWord(0x10u + frame)));
        (void)drainSubmissions(session);
        if (frame >= 4) {
            QNP_CHECK(session.receiveRemoteInput(frame - 4, 0)
                      == RemoteInputResult::Inserted);
        }
        const FrameAction action = session.nextAction();
        QNP_CHECK(action.kind == ActionKind::Advance);
        QNP_CHECK(session.commitAdvance(action.inputs, 500 + frame));
    }
    QNP_CHECK(
        session.tryChangeInputDelayAtConfirmedBoundary(6)
        == InputDelayChangeResult::Deferred);
    QNP_CHECK(
        session.inputDelayChangeBlocker(6)
        == InputDelayChangeBlocker::UnconfirmedBoundary);
    const uint32_t heldFrontier = session.currentFrame();

    for (uint32_t frame = 2; frame < heldFrontier; ++frame) {
        QNP_CHECK(session.receiveRemoteInput(frame, 0)
                  == RemoteInputResult::Inserted);
        QNP_CHECK(session.currentFrame() == heldFrontier);
    }
    QNP_CHECK(
        session.tryChangeInputDelayAtConfirmedBoundary(6)
        == InputDelayChangeResult::Applied);
    QNP_CHECK(session.currentFrame() == heldFrontier);
    QNP_CHECK(session.config().inputDelayFrames == 6);

    uint16_t value = 0xFFFFu;
    QNP_CHECK(session.getLocalInput(heldFrontier, value));
    QNP_CHECK(value == 0x105u);
    for (uint32_t frame = heldFrontier + 1;
         frame < heldFrontier + 6; ++frame) {
        QNP_CHECK(session.getLocalInput(frame, value));
        QNP_CHECK(value == config.neutralInput);
    }
}

QNP_TEST(RollbackSessionCorrectsEarliestMismatchAndPublishesOnlyConfirmedReplay) {
    RollbackConfig config = baseConfig();
    config.inputDelayFrames = 3;
    config.maxRollbackFrames = 3;
    RollbackSession session(config);
    QNP_CHECK(session.captureLocalInput(0, 5));
    (void)drainSubmissions(session);

    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 100));
    QNP_CHECK(session.captureLocalInput(1, 6));
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 101));

    ConfirmedFrame confirmed{};
    QNP_CHECK(!session.popConfirmedFrame(confirmed));
    QNP_CHECK(session.receiveRemoteInput(1, 9)
              == RemoteInputResult::Inserted);
    QNP_CHECK(session.pendingRollbackFrom() == 1);
    QNP_CHECK(session.receiveRemoteInput(0, 8)
              == RemoteInputResult::Inserted);
    QNP_CHECK(session.pendingRollbackFrom() == 0);
    QNP_CHECK(session.receiveRemoteInput(0, 8)
              == RemoteInputResult::Duplicate);
    QNP_CHECK(session.receiveRemoteInput(0, 7)
              == RemoteInputResult::Conflict);
    QNP_CHECK(!session.popConfirmedFrame(confirmed));

    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Rollback);
    QNP_CHECK(action.rollbackFrom == 0
              && action.replayUntilExclusive == 2);
    QNP_CHECK(session.beginRollback(action.rollbackFrom,
                                    action.replayUntilExclusive));
    QNP_CHECK(!session.needsLocalCapture());
    QNP_CHECK(!session.captureLocalInput(2, 0x010C));

    CanonicalInputs replay{};
    QNP_CHECK(session.inputsForFrame(0, replay));
    QNP_CHECK(replay.p1 == 0 && replay.p2 == 8
              && !replay.remotePredicted);
    QNP_CHECK(session.commitReplayFrame(replay, 200));
    QNP_CHECK(session.inputsForFrame(1, replay));
    QNP_CHECK(replay.p1 == 0 && replay.p2 == 9
              && !replay.remotePredicted);
    QNP_CHECK(session.commitReplayFrame(replay, 201));
    QNP_CHECK(session.finishRollback());
    QNP_CHECK(!session.hasPendingRollback());

    QNP_CHECK(session.popConfirmedFrame(confirmed));
    QNP_CHECK(confirmed.frame == 0
              && confirmed.p1 == 0 && confirmed.p2 == 8
              && confirmed.preStateHash == 200);
    QNP_CHECK(session.popConfirmedFrame(confirmed));
    QNP_CHECK(confirmed.frame == 1
              && confirmed.p1 == 0 && confirmed.p2 == 9
              && confirmed.preStateHash == 201);
    QNP_CHECK(!session.popConfirmedFrame(confirmed));
    QNP_CHECK(session.hasStableConfirmedThrough());
    QNP_CHECK(session.stableConfirmedThrough() == 1);
    QNP_CHECK(session.stats().rollbackCount == 1);
    QNP_CHECK(session.stats().replayedFrames == 2);
    QNP_CHECK(session.stats().maxRollbackDepth == 2);
}

QNP_TEST(RollbackSessionCommitsBoundaryAndDiscardsSpeculativeSuffix) {
    RollbackConfig config = baseConfig();
    config.inputDelayFrames = 1;
    config.maxRollbackFrames = 4;
    RollbackSession session(config);

    QNP_CHECK(session.captureLocalInput(0, 10));
    (void)drainSubmissions(session);
    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 100));

    QNP_CHECK(session.captureLocalInput(1, 11));
    (void)drainSubmissions(session);
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 101));

    QNP_CHECK(session.captureLocalInput(2, 12));
    (void)drainSubmissions(session);
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 102));

    QNP_CHECK(session.captureLocalInput(3, 13));
    (void)drainSubmissions(session);
    QNP_CHECK(session.currentFrame() == 3);

    QNP_CHECK(session.receiveRemoteInput(0, 0x0103)
              == RemoteInputResult::Inserted);
    QNP_CHECK(session.receiveRemoteInput(1, 0x0104)
              == RemoteInputResult::Inserted);
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Rollback);
    QNP_CHECK(action.rollbackFrom == 0
              && action.replayUntilExclusive == 3);
    QNP_CHECK(session.beginRollback(action.rollbackFrom,
                                    action.replayUntilExclusive));
    QNP_CHECK(!session.finishRollbackAtBoundary());

    CanonicalInputs replay{};
    QNP_CHECK(session.inputsForFrame(0, replay));
    QNP_CHECK(session.commitReplayFrame(replay, 200));
    QNP_CHECK(session.inputsForFrame(1, replay));
    QNP_CHECK(session.commitReplayFrame(replay, 201));

    QNP_CHECK(session.finishRollbackAtBoundary());
    QNP_CHECK(!session.replayInProgress());
    QNP_CHECK(!session.hasPendingRollback());
    QNP_CHECK(session.currentFrame() == 2);
    QNP_CHECK(!session.needsLocalCapture());
    QNP_CHECK(session.pendingLocalSubmissions() == 0);

    CanonicalInputs consumed{};
    uint64_t hash = 0;
    QNP_CHECK(session.getConsumedFrame(0, consumed, hash)
              && hash == 200);
    QNP_CHECK(session.getConsumedFrame(1, consumed, hash)
              && hash == 201);
    QNP_CHECK(!session.getConsumedFrame(2, consumed, hash));

    QNP_CHECK(session.captureLocalInput(2, 12));
    QNP_CHECK(session.pendingLocalSubmissions() == 0);
    QNP_CHECK(session.stats().rollbackCount == 1);
    QNP_CHECK(session.stats().replayedFrames == 2);
    QNP_CHECK(session.stats().maxRollbackDepth == 3);

    ConfirmedFrame confirmed{};
    QNP_CHECK(session.popConfirmedFrame(confirmed));
    QNP_CHECK(confirmed.frame == 0 && confirmed.preStateHash == 200);
    QNP_CHECK(session.popConfirmedFrame(confirmed));
    QNP_CHECK(confirmed.frame == 1 && confirmed.preStateHash == 201);
    QNP_CHECK(!session.popConfirmedFrame(confirmed));

    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(action.frame == 2);
    QNP_CHECK(session.commitAdvance(action.inputs, 302));
    QNP_CHECK(session.currentFrame() == 3);
    QNP_CHECK(!session.needsLocalCapture());
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(action.frame == 3);
    QNP_CHECK(session.commitAdvance(action.inputs, 303));
    QNP_CHECK(session.currentFrame() == 4);
    QNP_CHECK(session.needsLocalCapture());
    QNP_CHECK(session.pendingLocalSubmissions() == 0);
}

QNP_TEST(RollbackSessionHoldsPredictedBoundaryBeforeUncommittedFrame) {
    RollbackConfig config = baseConfig();
    config.inputDelayFrames = 1;
    config.maxRollbackFrames = 4;
    RollbackSession session(config);

    for (uint32_t frame = 0; frame < 3; ++frame) {
        QNP_CHECK(session.captureLocalInput(
            frame, canonWord(30u + frame)));
        (void)drainSubmissions(session);
        const FrameAction advance = session.nextAction();
        QNP_CHECK(advance.kind == ActionKind::Advance);
        QNP_CHECK(session.commitAdvance(
            advance.inputs, 400 + frame));
    }
    QNP_CHECK(session.captureLocalInput(3, 0x0107));
    (void)drainSubmissions(session);

    QNP_CHECK(session.receiveRemoteInput(0, 0x0108)
              == RemoteInputResult::Inserted);
    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Rollback);
    QNP_CHECK(session.beginRollback(
        action.rollbackFrom, action.replayUntilExclusive));

    CanonicalInputs replay{};
    QNP_CHECK(session.inputsForFrame(0, replay));
    QNP_CHECK(session.commitReplayFrame(replay, 500));
    QNP_CHECK(session.inputsForFrame(1, replay));
    QNP_CHECK(replay.remotePredicted);

    QNP_CHECK(session.finishRollbackBeforeBoundary());
    QNP_CHECK(session.currentFrame() == 1);
    QNP_CHECK(!session.hasPendingRollback());
    QNP_CHECK(!session.replayInProgress());
    QNP_CHECK(!session.needsLocalCapture());

    CanonicalInputs consumed{};
    uint64_t hash = 0;
    QNP_CHECK(session.getConsumedFrame(0, consumed, hash)
              && hash == 500);
    QNP_CHECK(!session.getConsumedFrame(1, consumed, hash));
    QNP_CHECK(!session.getConsumedFrame(2, consumed, hash));
    QNP_CHECK(session.stats().rollbackCount == 1);
    QNP_CHECK(session.stats().replayedFrames == 1);
    QNP_CHECK(session.stats().maxRollbackDepth == 3);

    ConfirmedFrame confirmed{};
    QNP_CHECK(session.popConfirmedFrame(confirmed));
    QNP_CHECK(confirmed.frame == 0 && confirmed.preStateHash == 500);
    QNP_CHECK(!session.popConfirmedFrame(confirmed));

    QNP_CHECK(session.receiveRemoteInput(1, 0x0109)
              == RemoteInputResult::Inserted);
    QNP_CHECK(!session.hasPendingRollback());
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(action.frame == 1);
    QNP_CHECK(!action.inputs.remotePredicted);
    QNP_CHECK(action.inputs.p2 == 0x0109);
    QNP_CHECK(session.commitAdvance(action.inputs, 501));
}

QNP_TEST(RollbackSessionCanHoldBoundaryBeforeAnyReplayCommit) {
    RollbackSession session(baseConfig());
    for (uint32_t frame = 0; frame < 2; ++frame) {
        QNP_CHECK(session.captureLocalInput(
            frame, canonWord(50u + frame)));
        (void)drainSubmissions(session);
        const FrameAction advance = session.nextAction();
        QNP_CHECK(advance.kind == ActionKind::Advance);
        QNP_CHECK(session.commitAdvance(
            advance.inputs, 600 + frame));
    }
    QNP_CHECK(session.captureLocalInput(2, 0x010A));
    (void)drainSubmissions(session);
    QNP_CHECK(session.receiveRemoteInput(0, 0x010B)
              == RemoteInputResult::Inserted);
    const FrameAction rollback = session.nextAction();
    QNP_CHECK(rollback.kind == ActionKind::Rollback);
    QNP_CHECK(session.beginRollback(
        rollback.rollbackFrom, rollback.replayUntilExclusive));

    QNP_CHECK(session.finishRollbackBeforeBoundary());
    QNP_CHECK(session.currentFrame() == 0);
    QNP_CHECK(!session.replayInProgress());
    QNP_CHECK(!session.hasPendingRollback());
    QNP_CHECK(!session.needsLocalCapture());
    QNP_CHECK(session.stats().rollbackCount == 1);
    QNP_CHECK(session.stats().replayedFrames == 0);

    CanonicalInputs consumed{};
    uint64_t hash = 0;
    QNP_CHECK(!session.getConsumedFrame(0, consumed, hash));
    QNP_CHECK(!session.getConsumedFrame(1, consumed, hash));
    const FrameAction advance = session.nextAction();
    QNP_CHECK(advance.kind == ActionKind::Advance);
    QNP_CHECK(advance.frame == 0);
    QNP_CHECK(!advance.inputs.remotePredicted);
    QNP_CHECK(advance.inputs.p2 == 0x010B);
}

QNP_TEST(RollbackSessionMatchingLateActualConfirmsWithoutRollback) {
    RollbackSession session(baseConfig());
    QNP_CHECK(session.captureLocalInput(0, 0x0206));
    TaggedInput local{};
    QNP_CHECK(session.popLocalSubmission(local));
    QNP_CHECK(local.frame == 0 && local.value == 0x0206);

    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(action.inputs.p1 == 0x0206
              && action.inputs.p2 == 0
              && action.inputs.remotePredicted);
    QNP_CHECK(session.commitAdvance(action.inputs, 0xABC));

    ConfirmedFrame confirmed{};
    QNP_CHECK(!session.popConfirmedFrame(confirmed));
    QNP_CHECK(session.receiveRemoteInput(0, 0)
              == RemoteInputResult::Inserted);
    QNP_CHECK(!session.hasPendingRollback());
    QNP_CHECK(session.popConfirmedFrame(confirmed));
    QNP_CHECK(confirmed.frame == 0
              && confirmed.preStateHash == 0xABC
              && confirmed.p1 == 0x0206
              && confirmed.p2 == 0);
}

QNP_TEST(RollbackSessionRejectsStaleAdvanceAfterMismatchArrives) {
    RollbackSession session(baseConfig());
    QNP_CHECK(session.captureLocalInput(0, 1));
    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 10));

    QNP_CHECK(session.captureLocalInput(1, 2));
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.receiveRemoteInput(0, 9)
              == RemoteInputResult::Inserted);
    QNP_CHECK(session.hasPendingRollback());
    QNP_CHECK(!session.commitAdvance(action.inputs, 11));
    QNP_CHECK(session.currentFrame() == 1);

    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Rollback);
    QNP_CHECK(action.rollbackFrom == 0
              && action.replayUntilExclusive == 1);
}

QNP_TEST(RollbackSessionMapsPlayerTwoAndSupportsExactZeroRollback) {
    RollbackConfig config = baseConfig();
    config.localPlayer = 1;
    config.inputDelayFrames = 1;
    config.maxRollbackFrames = 0;
    config.neutralInput = 0x00FF;
    RollbackSession session(config);
    QNP_CHECK(session.valid());

    QNP_CHECK(session.captureLocalInput(0, 0x0207));
    const std::vector<TaggedInput> submissions = drainSubmissions(session);
    QNP_REQUIRE(submissions.size() == 2);
    QNP_CHECK(submissions[0].frame == 0
              && submissions[0].value == 0x00FF);
    QNP_CHECK(submissions[1].frame == 1
              && submissions[1].value == 0x0207);

    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Stall);
    QNP_CHECK(action.stallReason == StallReason::PredictionLimit);
    QNP_CHECK(session.receiveRemoteInput(0, 0x0206)
              == RemoteInputResult::Inserted);
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(action.inputs.p1 == 0x0206
              && action.inputs.p2 == 0x00FF
              && !action.inputs.remotePredicted);
    QNP_CHECK(session.commitAdvance(action.inputs, 55));
}

QNP_TEST(RollbackSessionIndependentPeerDelaysProduceOneCanonicalTimeline) {
    RollbackConfig playerOneConfig = baseConfig();
    playerOneConfig.localPlayer = 0;
    playerOneConfig.inputDelayFrames = 1;
    playerOneConfig.maxRollbackFrames = 2;
    RollbackConfig playerTwoConfig = baseConfig();
    playerTwoConfig.localPlayer = 1;
    playerTwoConfig.inputDelayFrames = 3;
    playerTwoConfig.maxRollbackFrames = 4;

    RollbackSession playerOne(playerOneConfig);
    RollbackSession playerTwo(playerTwoConfig);
    auto exchange = [&]() {
        for (const TaggedInput& input : drainSubmissions(playerOne)) {
            QNP_CHECK(playerTwo.receiveRemoteInput(input.frame, input.value)
                      == RemoteInputResult::Inserted);
        }
        for (const TaggedInput& input : drainSubmissions(playerTwo)) {
            QNP_CHECK(playerOne.receiveRemoteInput(input.frame, input.value)
                      == RemoteInputResult::Inserted);
        }
    };
    exchange();

    for (uint32_t frame = 0; frame < 16; ++frame) {
        QNP_CHECK(playerOne.captureLocalInput(
            frame, canonWord(0x10u + frame)));
        QNP_CHECK(playerTwo.captureLocalInput(
            frame, canonWord(0x20u + frame)));
        exchange();

        const FrameAction one = playerOne.nextAction();
        const FrameAction two = playerTwo.nextAction();
        QNP_CHECK(one.kind == ActionKind::Advance);
        QNP_CHECK(two.kind == ActionKind::Advance);
        QNP_CHECK(!one.inputs.remotePredicted);
        QNP_CHECK(!two.inputs.remotePredicted);
        QNP_CHECK(one.inputs.frame == two.inputs.frame);
        QNP_CHECK(one.inputs.p1 == two.inputs.p1);
        QNP_CHECK(one.inputs.p2 == two.inputs.p2);

        const uint16_t expectedP1 = frame < 1
            ? 0
            : canonWord(0x10u + frame - 1u);
        const uint16_t expectedP2 = frame < 3
            ? 0
            : canonWord(0x20u + frame - 3u);
        QNP_CHECK(one.inputs.p1 == expectedP1);
        QNP_CHECK(one.inputs.p2 == expectedP2);
        const uint64_t preHash = 0x100000ull + frame;
        QNP_CHECK(playerOne.commitAdvance(one.inputs, preHash));
        QNP_CHECK(playerTwo.commitAdvance(two.inputs, preHash));

        ConfirmedFrame confirmedOne{};
        ConfirmedFrame confirmedTwo{};
        QNP_CHECK(playerOne.popConfirmedFrame(confirmedOne));
        QNP_CHECK(playerTwo.popConfirmedFrame(confirmedTwo));
        QNP_CHECK(confirmedOne.frame == confirmedTwo.frame);
        QNP_CHECK(confirmedOne.p1 == confirmedTwo.p1);
        QNP_CHECK(confirmedOne.p2 == confirmedTwo.p2);
        QNP_CHECK(confirmedOne.preStateHash
                  == confirmedTwo.preStateHash);
    }
}

QNP_TEST(RollbackSessionAsymmetricBoundaryDelayChangesNeverConflict) {
    RollbackConfig playerOneConfig = baseConfig();
    playerOneConfig.localPlayer = 0;
    playerOneConfig.inputDelayFrames = 1;
    playerOneConfig.maxRollbackFrames = 0;
    RollbackConfig playerTwoConfig = baseConfig();
    playerTwoConfig.localPlayer = 1;
    playerTwoConfig.inputDelayFrames = 4;
    playerTwoConfig.maxRollbackFrames = 0;

    RollbackSession playerOne(playerOneConfig);
    RollbackSession playerTwo(playerTwoConfig);
    std::vector<uint32_t> playerOneSent;
    std::vector<uint32_t> playerTwoSent;
    auto exchange = [&]() {
        for (const TaggedInput& input : drainSubmissions(playerOne)) {
            QNP_CHECK(std::find(playerOneSent.begin(), playerOneSent.end(),
                                input.frame) == playerOneSent.end());
            playerOneSent.push_back(input.frame);
            QNP_CHECK(playerTwo.receiveRemoteInput(input.frame, input.value)
                      == RemoteInputResult::Inserted);
        }
        for (const TaggedInput& input : drainSubmissions(playerTwo)) {
            QNP_CHECK(std::find(playerTwoSent.begin(), playerTwoSent.end(),
                                input.frame) == playerTwoSent.end());
            playerTwoSent.push_back(input.frame);
            QNP_CHECK(playerOne.receiveRemoteInput(input.frame, input.value)
                      == RemoteInputResult::Inserted);
        }
    };
    exchange();

    for (uint32_t frame = 0; frame < 6; ++frame) {
        QNP_CHECK(playerOne.needsLocalCapture());
        QNP_CHECK(playerTwo.needsLocalCapture());
        QNP_CHECK(playerOne.captureLocalInput(
            frame, canonWord(0x10u + frame)));
        QNP_CHECK(playerTwo.captureLocalInput(
            frame, canonWord(0x20u + frame)));
        exchange();

        const FrameAction one = playerOne.nextAction();
        const FrameAction two = playerTwo.nextAction();
        QNP_CHECK(one.kind == ActionKind::Advance);
        QNP_CHECK(two.kind == ActionKind::Advance);
        QNP_CHECK(!one.inputs.remotePredicted);
        QNP_CHECK(!two.inputs.remotePredicted);
        QNP_CHECK(one.inputs.p1 == two.inputs.p1);
        QNP_CHECK(one.inputs.p2 == two.inputs.p2);
        QNP_CHECK(playerOne.commitAdvance(one.inputs, 0x1000 + frame));
        QNP_CHECK(playerTwo.commitAdvance(two.inputs, 0x1000 + frame));
    }
    QNP_CHECK(playerOne.currentFrame() == 6);
    QNP_CHECK(playerTwo.currentFrame() == 6);
    QNP_CHECK(playerOne.stableConfirmedThrough() == 5);
    QNP_CHECK(playerTwo.stableConfirmedThrough() == 5);

    QNP_CHECK(playerOne.changeInputDelayAtConfirmedBoundary(5));
    QNP_CHECK(playerTwo.changeInputDelayAtConfirmedBoundary(1));
    exchange();

    for (uint32_t frame = 6; frame < 22; ++frame) {
        if (playerOne.needsLocalCapture()) {
            QNP_CHECK(playerOne.captureLocalInput(
                frame, canonWord(0x30u + frame)));
        }
        if (playerTwo.needsLocalCapture()) {
            QNP_CHECK(playerTwo.captureLocalInput(
                frame, canonWord(0x40u + frame)));
        }
        exchange();

        const FrameAction one = playerOne.nextAction();
        const FrameAction two = playerTwo.nextAction();
        QNP_CHECK(one.kind == ActionKind::Advance);
        QNP_CHECK(two.kind == ActionKind::Advance);
        QNP_CHECK(!one.inputs.remotePredicted);
        QNP_CHECK(!two.inputs.remotePredicted);
        QNP_CHECK(one.inputs.frame == two.inputs.frame);
        QNP_CHECK(one.inputs.p1 == two.inputs.p1);
        QNP_CHECK(one.inputs.p2 == two.inputs.p2);
        const uint64_t hash = 0x2000 + frame;
        QNP_CHECK(playerOne.commitAdvance(one.inputs, hash));
        QNP_CHECK(playerTwo.commitAdvance(two.inputs, hash));
    }

    QNP_CHECK(playerOne.currentFrame() == 22);
    QNP_CHECK(playerTwo.currentFrame() == 22);
    QNP_CHECK(playerOne.stableConfirmedThrough() == 21);
    QNP_CHECK(playerTwo.stableConfirmedThrough() == 21);
    QNP_CHECK(playerOne.config().inputDelayFrames == 5);
    QNP_CHECK(playerTwo.config().inputDelayFrames == 1);
    QNP_REQUIRE(playerOneSent.size() == 27);
    QNP_REQUIRE(playerTwoSent.size() == 23);
    for (std::size_t index = 0; index < playerOneSent.size(); ++index) {
        QNP_CHECK(playerOneSent[index] == static_cast<uint32_t>(index));
    }
    for (std::size_t index = 0; index < playerTwoSent.size(); ++index) {
        QNP_CHECK(playerTwoSent[index] == static_cast<uint32_t>(index));
    }
}

QNP_TEST(RollbackSessionRejectsRemoteRangeAndNeverExposesPredictions) {
    RollbackConfig config = baseConfig();
    config.maxRemoteFutureFrames = 2;
    RollbackSession session(config);
    QNP_CHECK(session.receiveRemoteInput(3, 0x0106)
              == RemoteInputResult::TooFuture);
    QNP_CHECK(session.receiveRemoteInput(2, 0x0103)
              == RemoteInputResult::Inserted);

    QNP_CHECK(session.captureLocalInput(0, 1));
    FrameAction action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 10));
    QNP_CHECK(session.captureLocalInput(1, 2));
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(session.commitAdvance(action.inputs, 11));
    QNP_CHECK(session.captureLocalInput(2, 3));
    action = session.nextAction();
    QNP_CHECK(action.kind == ActionKind::Advance);
    QNP_CHECK(!action.inputs.remotePredicted);
    QNP_CHECK(session.commitAdvance(action.inputs, 12));

    ConfirmedFrame confirmed{};
    QNP_CHECK(!session.popConfirmedFrame(confirmed));
    QNP_CHECK(session.receiveRemoteInput(0, 10)
              == RemoteInputResult::Inserted);
    QNP_CHECK(session.receiveRemoteInput(1, 10)
              == RemoteInputResult::Inserted);
    QNP_CHECK(session.hasPendingRollback());
    QNP_CHECK(!session.popConfirmedFrame(confirmed));
}

QNP_TEST(RollbackSessionTracksRemoteNewestActualAcrossHoles) {
    RollbackSession session(baseConfig());
    QNP_CHECK(!session.hasRemoteNewestActual());

    QNP_CHECK(session.receiveRemoteInput(0, 1) == RemoteInputResult::Inserted);
    QNP_CHECK(session.hasRemoteNewestActual());
    QNP_CHECK(session.remoteNewestActual() == 0);
    QNP_CHECK(session.remoteContiguousThrough() == 0);

    QNP_CHECK(session.receiveRemoteInput(3, 4) == RemoteInputResult::Inserted);
    QNP_CHECK(session.remoteNewestActual() == 3);
    QNP_CHECK(session.remoteContiguousThrough() == 0);

    QNP_CHECK(session.receiveRemoteInput(1, 2) == RemoteInputResult::Inserted);
    QNP_CHECK(session.remoteNewestActual() == 3);
    QNP_CHECK(session.remoteContiguousThrough() == 1);

    QNP_CHECK(session.receiveRemoteInput(2, 3) == RemoteInputResult::Inserted);
    QNP_CHECK(session.remoteNewestActual() == 3);
    QNP_CHECK(session.remoteContiguousThrough() == 3);

    QNP_CHECK(session.receiveRemoteInput(3, 4) == RemoteInputResult::Duplicate);
    QNP_CHECK(session.remoteNewestActual() == 3);
}

QNP_TEST(RollbackSessionNamesEveryDiagnosticResult) {
    QNP_CHECK(std::strcmp(
        remoteInputResultName(RemoteInputResult::Inserted),
        "inserted") == 0);
    QNP_CHECK(std::strcmp(
        remoteInputResultName(RemoteInputResult::Duplicate),
        "duplicate") == 0);
    QNP_CHECK(std::strcmp(
        remoteInputResultName(RemoteInputResult::Conflict),
        "conflict") == 0);
    QNP_CHECK(std::strcmp(
        remoteInputResultName(RemoteInputResult::TooOld),
        "too-old") == 0);
    QNP_CHECK(std::strcmp(
        remoteInputResultName(RemoteInputResult::TooFuture),
        "too-future") == 0);
    QNP_CHECK(std::strcmp(
        remoteInputResultName(RemoteInputResult::InvalidSession),
        "invalid-session") == 0);
    QNP_CHECK(std::strcmp(
        remoteInputResultName(RemoteInputResult::InvalidValue),
        "invalid-value") == 0);
    QNP_CHECK(std::strcmp(
        remoteInputResultName(RemoteInputResult::Count),
        "unknown") == 0);
    QNP_CHECK(std::strcmp(
        remoteInputResultName(static_cast<RemoteInputResult>(255)),
        "unknown") == 0);

    QNP_CHECK(std::strcmp(
        stallReasonName(StallReason::None), "none") == 0);
    QNP_CHECK(std::strcmp(
        stallReasonName(StallReason::LocalInputMissing),
        "local-input-missing") == 0);
    QNP_CHECK(std::strcmp(
        stallReasonName(StallReason::PredictionLimit),
        "prediction-limit") == 0);
    QNP_CHECK(std::strcmp(
        stallReasonName(StallReason::ReplayInProgress),
        "replay-in-progress") == 0);
    QNP_CHECK(std::strcmp(
        stallReasonName(StallReason::InvalidSession),
        "invalid-session") == 0);
    QNP_CHECK(std::strcmp(
        stallReasonName(StallReason::Count), "unknown") == 0);
    QNP_CHECK(std::strcmp(
        stallReasonName(static_cast<StallReason>(255)),
        "unknown") == 0);
}

QNP_TEST(RollbackSessionCountsEveryRemoteIngressResult) {
    const auto resultIndex = [](const RemoteInputResult result) {
        return static_cast<size_t>(result);
    };

    RollbackConfig config = baseConfig();
    config.maxRemoteFutureFrames = 2;
    RollbackSession session(config);
    QNP_CHECK(session.receiveRemoteInput(0, 1)
              == RemoteInputResult::Inserted);
    QNP_CHECK(session.receiveRemoteInput(0, 1)
              == RemoteInputResult::Duplicate);
    QNP_CHECK(session.receiveRemoteInput(0, 2)
              == RemoteInputResult::Conflict);
    QNP_CHECK(session.receiveRemoteInput(3, 3)
              == RemoteInputResult::TooFuture);
    QNP_CHECK(session.receiveRemoteInput(1, 0x0010)
              == RemoteInputResult::InvalidValue);
    const RollbackStats& direct = session.stats();
    QNP_CHECK(direct.remoteInputResults[
                  resultIndex(RemoteInputResult::Inserted)] == 1);
    QNP_CHECK(direct.remoteInputResults[
                  resultIndex(RemoteInputResult::Duplicate)] == 1);
    QNP_CHECK(direct.remoteInputResults[
                  resultIndex(RemoteInputResult::Conflict)] == 1);
    QNP_CHECK(direct.remoteInputResults[
                  resultIndex(RemoteInputResult::TooFuture)] == 1);
    QNP_CHECK(direct.remoteInputResults[
                  resultIndex(RemoteInputResult::InvalidValue)] == 1);
    QNP_CHECK(direct.remoteInputResults[
                  resultIndex(RemoteInputResult::TooOld)] == 0);
    QNP_CHECK(direct.remoteInputResults[
                  resultIndex(RemoteInputResult::InvalidSession)] == 0);

    RollbackSession aged(baseConfig());
    for (uint32_t frame = 0; frame < 64; ++frame) {
        QNP_REQUIRE(aged.captureLocalInput(frame, canonWord(frame)));
        QNP_REQUIRE(aged.receiveRemoteInput(
                        frame, canonWord(0x80u + frame))
                    == RemoteInputResult::Inserted);
        const FrameAction action = aged.nextAction();
        QNP_REQUIRE(action.kind == ActionKind::Advance);
        QNP_REQUIRE(aged.commitAdvance(action.inputs, 1000u + frame));
    }
    QNP_CHECK(aged.currentFrame() == 64);
    QNP_CHECK(aged.receiveRemoteInput(0, canonWord(0x80u))
              == RemoteInputResult::TooOld);
    QNP_CHECK(aged.stats().remoteInputResults[
                  resultIndex(RemoteInputResult::Inserted)] == 64);
    QNP_CHECK(aged.stats().remoteInputResults[
                  resultIndex(RemoteInputResult::TooOld)] == 1);

    RollbackConfig invalidConfig = baseConfig();
    invalidConfig.localPlayer = 2;
    RollbackSession invalid(invalidConfig);
    QNP_CHECK(invalid.receiveRemoteInput(0, 0)
              == RemoteInputResult::InvalidSession);
    QNP_CHECK(invalid.stats().remoteInputResults[
                  resultIndex(RemoteInputResult::InvalidSession)] == 1);
}

QNP_TEST(RollbackSessionDiagnosesRemoteHoles) {
    RollbackSession session(baseConfig());
    RollbackDiagnostics diagnostics = session.diagnostics();
    QNP_CHECK(diagnostics.valid);
    QNP_CHECK(diagnostics.frontier == 0);
    QNP_CHECK(!diagnostics.localInputAtFrontier);
    QNP_CHECK(!diagnostics.remoteActualAtFrontier);
    QNP_CHECK(diagnostics.nextRemoteExpected == 0);
    QNP_CHECK(!diagnostics.hasRemoteContiguousThrough);
    QNP_CHECK(!diagnostics.hasRemoteNewestActual);
    QNP_CHECK(!diagnostics.hasRemoteHole);
    QNP_CHECK(diagnostics.remoteHoleSpan == 0);

    QNP_CHECK(session.receiveRemoteInput(3, 4)
              == RemoteInputResult::Inserted);
    diagnostics = session.diagnostics();
    QNP_CHECK(diagnostics.nextRemoteExpected == 0);
    QNP_CHECK(diagnostics.hasRemoteNewestActual);
    QNP_CHECK(diagnostics.remoteNewestActual == 3);
    QNP_CHECK(diagnostics.hasRemoteHole);
    QNP_CHECK(diagnostics.remoteHoleSpan == 4);

    QNP_CHECK(session.receiveRemoteInput(1, 2)
              == RemoteInputResult::Inserted);
    diagnostics = session.diagnostics();
    QNP_CHECK(diagnostics.nextRemoteExpected == 0);
    QNP_CHECK(diagnostics.remoteHoleSpan == 4);

    QNP_CHECK(session.receiveRemoteInput(0, 1)
              == RemoteInputResult::Inserted);
    diagnostics = session.diagnostics();
    QNP_CHECK(diagnostics.remoteActualAtFrontier);
    QNP_CHECK(diagnostics.nextRemoteExpected == 2);
    QNP_CHECK(diagnostics.hasRemoteContiguousThrough);
    QNP_CHECK(diagnostics.remoteContiguousThrough == 1);
    QNP_CHECK(diagnostics.hasRemoteHole);
    QNP_CHECK(diagnostics.remoteHoleSpan == 2);

    QNP_CHECK(session.receiveRemoteInput(2, 3)
              == RemoteInputResult::Inserted);
    diagnostics = session.diagnostics();
    QNP_CHECK(diagnostics.nextRemoteExpected == 4);
    QNP_CHECK(diagnostics.remoteContiguousThrough == 3);
    QNP_CHECK(!diagnostics.hasRemoteHole);
    QNP_CHECK(diagnostics.remoteHoleSpan == 0);

    const RollbackStats& stats = session.stats();
    QNP_CHECK(stats.remoteOutOfOrderInputs == 2);
    QNP_CHECK(stats.remoteHolePrefixAdvances == 2);
    QNP_CHECK(stats.maxRemoteHoleSpan == 4);
    QNP_CHECK(stats.remoteInputResults[
                  static_cast<size_t>(RemoteInputResult::Inserted)] == 4);
}

QNP_TEST(RollbackSessionAccountsPredictionCorrectionAndConfirmation) {
    RollbackSession session(baseConfig());

    QNP_REQUIRE(session.captureLocalInput(0, 0x0101));
    FrameAction action = session.nextAction();
    QNP_REQUIRE(action.kind == ActionKind::Advance);
    QNP_CHECK(action.inputs.remotePredicted);
    QNP_REQUIRE(session.commitAdvance(action.inputs, 100));

    QNP_REQUIRE(session.captureLocalInput(1, 0x0102));
    action = session.nextAction();
    QNP_REQUIRE(action.kind == ActionKind::Advance);
    QNP_CHECK(action.inputs.remotePredicted);
    QNP_REQUIRE(session.commitAdvance(action.inputs, 101));

    RollbackDiagnostics diagnostics = session.diagnostics();
    QNP_CHECK(diagnostics.frontier == 2);
    QNP_CHECK(diagnostics.speculativeFrames == 2);
    QNP_CHECK(!diagnostics.pendingRollback);
    QNP_CHECK(!diagnostics.hasStableConfirmedThrough);

    QNP_REQUIRE(session.receiveRemoteInput(0, 0x0201)
                == RemoteInputResult::Inserted);
    QNP_REQUIRE(session.receiveRemoteInput(1, 0x0202)
                == RemoteInputResult::Inserted);
    diagnostics = session.diagnostics();
    QNP_CHECK(diagnostics.pendingRollback);
    QNP_CHECK(diagnostics.pendingRollbackFrom == 0);
    QNP_CHECK(diagnostics.nextRemoteExpected == 2);
    QNP_CHECK(!diagnostics.hasStableConfirmedThrough);

    action = session.nextAction();
    QNP_REQUIRE(action.kind == ActionKind::Rollback);
    QNP_CHECK(action.rollbackFrom == 0);
    QNP_CHECK(action.replayUntilExclusive == 2);
    QNP_REQUIRE(session.beginRollback(
        action.rollbackFrom, action.replayUntilExclusive));
    diagnostics = session.diagnostics();
    QNP_CHECK(diagnostics.pendingRollback);
    QNP_CHECK(diagnostics.replayInProgress);
    QNP_CHECK(diagnostics.replayFrom == 0);
    QNP_CHECK(diagnostics.replayUntilExclusive == 2);
    QNP_CHECK(diagnostics.nextReplayFrame == 0);

    CanonicalInputs replay{};
    QNP_REQUIRE(session.inputsForFrame(0, replay));
    QNP_CHECK(!replay.remotePredicted);
    QNP_REQUIRE(session.commitReplayFrame(replay, 200));
    QNP_REQUIRE(session.inputsForFrame(1, replay));
    QNP_CHECK(!replay.remotePredicted);
    QNP_REQUIRE(session.commitReplayFrame(replay, 201));
    QNP_REQUIRE(session.finishRollback());

    const RollbackStats& stats = session.stats();
    QNP_CHECK(stats.visibleAdvances == 2);
    QNP_CHECK(stats.predictedAdvances == 2);
    QNP_CHECK(stats.replayedFrames == 2);
    QNP_CHECK(stats.rollbackCount == 1);
    QNP_CHECK(stats.rollbackFrames == 2);
    QNP_CHECK(stats.maxRollbackDepth == 2);
    QNP_CHECK(stats.maxSpeculativeFrames == 2);
    QNP_CHECK(stats.mismatchingRemoteInputs == 2);
    QNP_CHECK(stats.confirmedFrames == 2);

    diagnostics = session.diagnostics();
    QNP_CHECK(!diagnostics.pendingRollback);
    QNP_CHECK(!diagnostics.replayInProgress);
    QNP_CHECK(diagnostics.hasStableConfirmedThrough);
    QNP_CHECK(diagnostics.stableConfirmedThrough == 1);
    QNP_CHECK(diagnostics.nextConfirmedFrame == 2);
    QNP_CHECK(diagnostics.pendingConfirmedFrames == 2);
    ConfirmedFrame confirmed{};
    QNP_CHECK(session.popConfirmedFrame(confirmed));
    QNP_CHECK(session.popConfirmedFrame(confirmed));
    QNP_CHECK(!session.popConfirmedFrame(confirmed));
    QNP_CHECK(session.diagnostics().pendingConfirmedFrames == 0);
}

QNP_TEST(RollbackSessionTracksStallReasonsAndResetsDiagnostics) {
    const auto stallIndex = [](const StallReason reason) {
        return static_cast<size_t>(reason);
    };

    RollbackConfig predictionConfig = baseConfig();
    predictionConfig.maxRollbackFrames = 1;
    RollbackSession prediction(predictionConfig);
    FrameAction action = prediction.nextAction();
    QNP_CHECK(action.kind == ActionKind::Stall);
    QNP_CHECK(action.stallReason == StallReason::LocalInputMissing);
    action = prediction.nextAction();
    QNP_CHECK(action.stallReason == StallReason::LocalInputMissing);
    RollbackDiagnostics diagnostics = prediction.diagnostics();
    QNP_CHECK(diagnostics.activeStallReason
              == StallReason::LocalInputMissing);
    QNP_CHECK(diagnostics.consecutiveStallPolls == 2);

    QNP_REQUIRE(prediction.captureLocalInput(0, 1));
    action = prediction.nextAction();
    QNP_REQUIRE(action.kind == ActionKind::Advance);
    QNP_CHECK(prediction.diagnostics().activeStallReason
              == StallReason::None);
    QNP_CHECK(prediction.diagnostics().consecutiveStallPolls == 0);
    QNP_REQUIRE(prediction.commitAdvance(action.inputs, 300));
    QNP_REQUIRE(prediction.captureLocalInput(1, 2));
    action = prediction.nextAction();
    QNP_CHECK(action.stallReason == StallReason::PredictionLimit);
    action = prediction.nextAction();
    QNP_CHECK(action.stallReason == StallReason::PredictionLimit);

    const RollbackStats& predictionStats = prediction.stats();
    QNP_CHECK(predictionStats.stallPolls == 4);
    QNP_CHECK(predictionStats.stallEpisodes == 2);
    QNP_CHECK(predictionStats.stallPollsByReason[
                  stallIndex(StallReason::LocalInputMissing)] == 2);
    QNP_CHECK(predictionStats.stallEpisodesByReason[
                  stallIndex(StallReason::LocalInputMissing)] == 1);
    QNP_CHECK(predictionStats.stallPollsByReason[
                  stallIndex(StallReason::PredictionLimit)] == 2);
    QNP_CHECK(predictionStats.stallEpisodesByReason[
                  stallIndex(StallReason::PredictionLimit)] == 1);
    QNP_CHECK(predictionStats.maxConsecutiveStallPolls == 2);

    RollbackSession replaying(baseConfig());
    QNP_REQUIRE(replaying.captureLocalInput(0, 1));
    action = replaying.nextAction();
    QNP_REQUIRE(action.kind == ActionKind::Advance);
    QNP_REQUIRE(replaying.commitAdvance(action.inputs, 400));
    QNP_REQUIRE(replaying.receiveRemoteInput(0, 2)
                == RemoteInputResult::Inserted);
    action = replaying.nextAction();
    QNP_REQUIRE(action.kind == ActionKind::Rollback);
    QNP_REQUIRE(replaying.beginRollback(
        action.rollbackFrom, action.replayUntilExclusive));
    action = replaying.nextAction();
    QNP_CHECK(action.stallReason == StallReason::ReplayInProgress);
    action = replaying.nextAction();
    QNP_CHECK(action.stallReason == StallReason::ReplayInProgress);
    CanonicalInputs replay{};
    QNP_REQUIRE(replaying.inputsForFrame(0, replay));
    QNP_REQUIRE(replaying.commitReplayFrame(replay, 401));
    QNP_REQUIRE(replaying.finishRollback());
    action = replaying.nextAction();
    QNP_CHECK(action.stallReason == StallReason::LocalInputMissing);
    action = replaying.nextAction();
    QNP_CHECK(action.stallReason == StallReason::LocalInputMissing);
    const RollbackStats& replayStats = replaying.stats();
    QNP_CHECK(replayStats.stallPolls == 4);
    QNP_CHECK(replayStats.stallEpisodes == 1);
    QNP_CHECK(replayStats.stallPollsByReason[
                  stallIndex(StallReason::ReplayInProgress)] == 2);
    QNP_CHECK(replayStats.stallEpisodesByReason[
                  stallIndex(StallReason::ReplayInProgress)] == 1);
    QNP_CHECK(replayStats.stallPollsByReason[
                  stallIndex(StallReason::LocalInputMissing)] == 2);
    QNP_CHECK(replayStats.stallEpisodesByReason[
                  stallIndex(StallReason::LocalInputMissing)] == 1);
    QNP_CHECK(replayStats.maxConsecutiveStallPolls == 4);

    RollbackConfig invalidConfig = baseConfig();
    invalidConfig.localPlayer = 2;
    RollbackSession invalid(invalidConfig);
    action = invalid.nextAction();
    QNP_CHECK(action.stallReason == StallReason::InvalidSession);
    action = invalid.nextAction();
    QNP_CHECK(action.stallReason == StallReason::InvalidSession);
    QNP_CHECK(invalid.stats().stallPolls == 2);
    QNP_CHECK(invalid.stats().stallEpisodes == 1);
    QNP_CHECK(invalid.stats().stallPollsByReason[
                  stallIndex(StallReason::InvalidSession)] == 2);
    QNP_CHECK(invalid.stats().stallEpisodesByReason[
                  stallIndex(StallReason::InvalidSession)] == 1);

    RollbackConfig resetConfig = baseConfig();
    resetConfig.firstFrame = 11;
    QNP_REQUIRE(replaying.reset(resetConfig));
    const RollbackStats& resetStats = replaying.stats();
    QNP_CHECK(resetStats.uniqueCaptures == 0);
    QNP_CHECK(resetStats.repeatedCaptureCalls == 0);
    QNP_CHECK(resetStats.localSubmissions == 0);
    QNP_CHECK(resetStats.visibleAdvances == 0);
    QNP_CHECK(resetStats.predictedAdvances == 0);
    QNP_CHECK(resetStats.replayedFrames == 0);
    QNP_CHECK(resetStats.rollbackCount == 0);
    QNP_CHECK(resetStats.rollbackFrames == 0);
    QNP_CHECK(resetStats.mismatchingRemoteInputs == 0);
    QNP_CHECK(resetStats.confirmedFrames == 0);
    QNP_CHECK(resetStats.stallPolls == 0);
    QNP_CHECK(resetStats.stallEpisodes == 0);
    QNP_CHECK(std::all_of(
        resetStats.stallPollsByReason.begin(),
        resetStats.stallPollsByReason.end(),
        [](const uint64_t value) { return value == 0; }));
    QNP_CHECK(std::all_of(
        resetStats.stallEpisodesByReason.begin(),
        resetStats.stallEpisodesByReason.end(),
        [](const uint64_t value) { return value == 0; }));
    QNP_CHECK(std::all_of(
        resetStats.remoteInputResults.begin(),
        resetStats.remoteInputResults.end(),
        [](const uint64_t value) { return value == 0; }));
    QNP_CHECK(resetStats.remoteOutOfOrderInputs == 0);
    QNP_CHECK(resetStats.remoteHolePrefixAdvances == 0);
    QNP_CHECK(resetStats.maxRollbackDepth == 0);
    QNP_CHECK(resetStats.maxSpeculativeFrames == 0);
    QNP_CHECK(resetStats.maxConsecutiveStallPolls == 0);
    QNP_CHECK(resetStats.maxRemoteHoleSpan == 0);

    diagnostics = replaying.diagnostics();
    QNP_CHECK(diagnostics.valid);
    QNP_CHECK(diagnostics.frontier == 11);
    QNP_CHECK(diagnostics.inputDelayFrames == 0);
    QNP_CHECK(diagnostics.maxRollbackFrames
              == resetConfig.maxRollbackFrames);
    QNP_CHECK(!diagnostics.localInputAtFrontier);
    QNP_CHECK(!diagnostics.remoteActualAtFrontier);
    QNP_CHECK(diagnostics.speculativeFrames == 0);
    QNP_CHECK(diagnostics.nextRemoteExpected == 11);
    QNP_CHECK(!diagnostics.hasRemoteContiguousThrough);
    QNP_CHECK(!diagnostics.hasRemoteNewestActual);
    QNP_CHECK(!diagnostics.hasRemoteHole);
    QNP_CHECK(diagnostics.nextConfirmedFrame == 11);
    QNP_CHECK(!diagnostics.hasStableConfirmedThrough);
    QNP_CHECK(diagnostics.pendingLocalSubmissions == 0);
    QNP_CHECK(diagnostics.pendingConfirmedFrames == 0);
    QNP_CHECK(!diagnostics.pendingRollback);
    QNP_CHECK(!diagnostics.replayInProgress);
    QNP_CHECK(diagnostics.replayFrom == 11);
    QNP_CHECK(diagnostics.replayUntilExclusive == 11);
    QNP_CHECK(diagnostics.nextReplayFrame == 11);
    QNP_CHECK(diagnostics.activeStallReason == StallReason::None);
    QNP_CHECK(diagnostics.consecutiveStallPolls == 0);
}
