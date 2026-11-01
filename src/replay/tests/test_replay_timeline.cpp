#include "TestHarness.h"

#include "replay/ReplayIntegrity.h"
#include "replay/ReplayTimeline.h"

#include <vector>

using namespace qreplay;

namespace {

constexpr uint64_t kInitialDigest = 0xABCDEF0123456789ull;
constexpr uint64_t kInitialState = 0x1111222233334444ull;

DeterministicStartDescriptor makeStart(uint32_t firstFrame = 0u,
                                       uint8_t laneMask = 0x03u) {
    DeterministicStartDescriptor start{};
    start.start.firstTimelineFrame = firstFrame;
    start.start.initialDigest = kInitialDigest;
    start.start.initialStateHash = kInitialState;
    start.start.seedGameplay = 0x1234u;
    start.bootstrapKind = BootstrapKind::OfflineVsDirectBootV1;
    start.activeLaneMask = laneMask;
    start.activeSlotMask = laneMask;
    start.bootstrapFingerprint = 0x5A5A5A5Aull;
    return start;
}

uint64_t nextState(uint64_t state, uint16_t p1, uint16_t p2) {
    state ^= static_cast<uint64_t>(p1) + 0x9E3779B97F4A7C15ull + (state << 6)
             + (state >> 2);
    state *= 0x100000001B3ull;
    state ^= static_cast<uint64_t>(p2) + 0xC2B2AE3D27D4EB4Full;
    return state;
}

struct Producer {
    uint64_t digest = kInitialDigest;
    uint64_t state = kInitialState;
    uint32_t frame = 0u;

    ReplayFrameV1 make(uint16_t p1, uint16_t p2) {
        ReplayFrameV1 out =
            makeFrameFromConfirmedPair(frame, p1, p2, state, 0u);
        state = nextState(state, p1, p2);
        out.postStateHash = state;
        digest = sealFrameDigest(digest, out);
        ++frame;
        return out;
    }
};

}

QNP_TEST(TimelineAppendsContiguousFramesAndChainsDigest) {
    ConfirmedTimelineBuffer buffer(64u);
    buffer.reset(makeStart());
    QNP_CHECK(buffer.started());
    QNP_CHECK(buffer.rollingDigest() == kInitialDigest);
    QNP_CHECK(buffer.nextFrame() == 0u);
    QNP_CHECK(buffer.bufferedFrames() == 0u);

    Producer producer;
    for (uint32_t i = 0; i < 8u; ++i) {
        ReplayFrameV1 frame = producer.make(0x0001u, 0x0100u);
        ParseStatus why = ParseStatus::Ok;
        QNP_REQUIRE(buffer.append(frame, why));
        QNP_CHECK(why == ParseStatus::Ok);
    }
    QNP_CHECK(buffer.nextFrame() == 8u);
    QNP_CHECK(buffer.bufferedFrames() == 8u);
    QNP_CHECK(buffer.retainedFrames() == 8u);
    QNP_CHECK(buffer.rollingDigest() == producer.digest);
    QNP_CHECK(buffer.lastFault() == TimelineFault::None);

    ReplayFrameV1 head{};
    QNP_REQUIRE(buffer.peek(head));
    QNP_CHECK(head.timelineFrame == 0u);
    QNP_CHECK(head.activeLaneMask == 0x03u);
    QNP_CHECK(head.inputDomain == InputDomain::LogicalSourceV1);
}

QNP_TEST(TimelineRejectsFrameGap) {
    ConfirmedTimelineBuffer buffer(16u);
    buffer.reset(makeStart());
    Producer producer;
    ParseStatus why = ParseStatus::Ok;
    ReplayFrameV1 zero = producer.make(0x0001u, 0u);
    QNP_REQUIRE(buffer.append(zero, why));

    (void)producer.make(0u, 0u);
    ReplayFrameV1 two = producer.make(0u, 0u);
    QNP_CHECK(!buffer.append(two, why));
    QNP_CHECK(why == ParseStatus::FrameGap);
    QNP_CHECK(buffer.lastFault() == TimelineFault::FrameGap);
    QNP_CHECK(buffer.nextFrame() == 1u);
}

QNP_TEST(TimelineAcceptsConsistentDuplicateAndRejectsConflict) {
    ConfirmedTimelineBuffer buffer(16u);
    buffer.reset(makeStart());
    Producer producer;
    ParseStatus why = ParseStatus::Ok;
    const ReplayFrameV1 zero = producer.make(0x0001u, 0x0002u);
    const ReplayFrameV1 one = producer.make(0x0100u, 0x0200u);
    QNP_REQUIRE(buffer.append(zero, why));
    QNP_REQUIRE(buffer.append(one, why));

    QNP_CHECK(buffer.append(zero, why));
    QNP_CHECK(why == ParseStatus::Ok);
    QNP_CHECK(buffer.nextFrame() == 2u);
    QNP_CHECK(buffer.rollingDigest() == producer.digest);

    ReplayFrameV1 conflicting = zero;
    conflicting.actions[1] = 0x0004u;
    QNP_CHECK(!buffer.append(conflicting, why));
    QNP_CHECK(why == ParseStatus::FrameConflict);
    QNP_CHECK(buffer.lastFault() == TimelineFault::FrameConflict);
}

QNP_TEST(TimelineRejectsInvalidActionMask) {
    ConfirmedTimelineBuffer buffer(16u);
    buffer.reset(makeStart());
    Producer producer;
    ParseStatus why = ParseStatus::Ok;

    ReplayFrameV1 frame = producer.make(0u, 0u);
    frame.actions[0] = 0x1000u;
    QNP_CHECK(!buffer.append(frame, why));
    QNP_CHECK(why == ParseStatus::BadActionMask);
    QNP_CHECK(buffer.lastFault() == TimelineFault::BadActionMask);

    frame.actions[0] = 0x00F0u;
    QNP_CHECK(!buffer.append(frame, why));
    QNP_CHECK(why == ParseStatus::BadActionMask);

    frame.actions[0] = 0x0F0Fu;
    frame.rollingDigest = advanceReplayDigest(kInitialDigest, frame);
    QNP_CHECK(buffer.append(frame, why));
}

QNP_TEST(TimelineRejectsWrongLaneShape) {
    ConfirmedTimelineBuffer buffer(16u);
    buffer.reset(makeStart(0u, 0x03u));
    Producer producer;
    ParseStatus why = ParseStatus::Ok;

    ReplayFrameV1 wide = producer.make(0u, 0u);
    wide.activeLaneMask = 0x0Fu;
    QNP_CHECK(!buffer.append(wide, why));
    QNP_CHECK(why == ParseStatus::BadLaneShape);
    QNP_CHECK(buffer.lastFault() == TimelineFault::BadLaneShape);

    ReplayFrameV1 impossible = wide;
    impossible.activeLaneMask = 0x10u;
    QNP_CHECK(!buffer.append(impossible, why));
    QNP_CHECK(why == ParseStatus::BadLaneShape);

    ReplayFrameV1 ghost = wide;
    ghost.activeLaneMask = 0x03u;
    ghost.actions[2] = 0x0001u;
    QNP_CHECK(!buffer.append(ghost, why));
    QNP_CHECK(why == ParseStatus::BadLaneShape);

    ReplayFrameV1 unknownDomain = wide;
    unknownDomain.activeLaneMask = 0x03u;
    unknownDomain.inputDomain = static_cast<InputDomain>(9);
    QNP_CHECK(!buffer.append(unknownDomain, why));
    QNP_CHECK(why == ParseStatus::UnsupportedFeature);
    QNP_CHECK(buffer.lastFault() == TimelineFault::UnknownInputDomain);
}

QNP_TEST(TimelineRejectsDigestChainBreak) {
    ConfirmedTimelineBuffer buffer(16u);
    buffer.reset(makeStart());
    Producer producer;
    ParseStatus why = ParseStatus::Ok;

    ReplayFrameV1 zero = producer.make(0x0001u, 0u);
    QNP_REQUIRE(buffer.append(zero, why));

    ReplayFrameV1 one = producer.make(0x0002u, 0u);
    one.rollingDigest ^= 1ull;
    QNP_CHECK(!buffer.append(one, why));
    QNP_CHECK(why == ParseStatus::DigestMismatch);
    QNP_CHECK(buffer.lastFault() == TimelineFault::DigestChainBreak);
    QNP_CHECK(buffer.nextFrame() == 1u);

    ReplayFrameV1 stale = one;
    stale.rollingDigest = zero.rollingDigest;
    QNP_CHECK(!buffer.append(stale, why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::DigestChainBreak);
}

QNP_TEST(TimelineConsumeVerifiesPreStateHash) {
    ConfirmedTimelineBuffer buffer(16u);
    buffer.reset(makeStart());
    Producer producer;
    ParseStatus why = ParseStatus::Ok;
    std::vector<ReplayFrameV1> frames;
    for (uint32_t i = 0; i < 4u; ++i) {
        frames.push_back(producer.make(static_cast<uint16_t>(0x0001u << (i & 3u)),
                                       0x0100u));
        QNP_REQUIRE(buffer.append(frames.back(), why));
    }

    QNP_CHECK(!buffer.consume(kInitialState ^ 1ull, why));
    QNP_CHECK(why == ParseStatus::DigestMismatch);
    QNP_CHECK(buffer.lastFault() == TimelineFault::PreStateHashMismatch);
    QNP_CHECK(buffer.consumeCursor() == 0u);

    uint64_t state = kInitialState;
    for (size_t i = 0; i < frames.size(); ++i) {
        QNP_REQUIRE(buffer.consume(state, why));
        state = nextState(state, frames[i].actions[0], frames[i].actions[1]);
        buffer.recordPostState(frames[i].timelineFrame, state);
    }
    QNP_CHECK(buffer.consumeCursor() == 4u);
    QNP_CHECK(buffer.bufferedFrames() == 0u);
    uint32_t last = 0u;
    QNP_CHECK(buffer.lastConsumedFrame(last) && last == 3u);
    QNP_CHECK(buffer.postStateConsistent());

    QNP_CHECK(!buffer.consume(state, why));
    QNP_CHECK(why == ParseStatus::Truncated);
    QNP_CHECK(buffer.lastFault() == TimelineFault::NoRecordAvailable);
}

QNP_TEST(TimelineDetectsPostStateMismatch) {
    ConfirmedTimelineBuffer buffer(16u);
    buffer.reset(makeStart());
    Producer producer;
    ParseStatus why = ParseStatus::Ok;
    const ReplayFrameV1 zero = producer.make(0x0001u, 0u);
    QNP_REQUIRE(buffer.append(zero, why));
    QNP_REQUIRE(buffer.consume(kInitialState, why));
    buffer.recordPostState(0u, zero.postStateHash ^ 0x40ull);
    QNP_CHECK(!buffer.postStateConsistent());

    TimelineTerminal terminal{};
    terminal.nextRecord = 1u;
    terminal.finalDigest = producer.digest;
    terminal.postTailStateHash = zero.postStateHash ^ 0x40ull;
    buffer.setTerminal(terminal);
    QNP_CHECK(!buffer.verifyTerminal(why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::PostStateMismatch);
}

QNP_TEST(TimelineTerminalPostTailProof) {
    ConfirmedTimelineBuffer buffer(16u);
    buffer.reset(makeStart());
    Producer producer;
    ParseStatus why = ParseStatus::Ok;
    std::vector<ReplayFrameV1> frames;
    for (uint32_t i = 0; i < 5u; ++i) {
        frames.push_back(producer.make(0x0001u, 0x0200u));
        QNP_REQUIRE(buffer.append(frames.back(), why));
    }

    TimelineTerminal good{};
    good.nextRecord = 5u;
    good.finalDigest = producer.digest;
    good.postTailStateHash = frames.back().postStateHash;
    good.reason = TerminalReason::Completed;

    buffer.setTerminal(good);
    QNP_CHECK(!buffer.verifyTerminal(why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::TerminalNotDrained);

    uint64_t state = kInitialState;
    for (size_t i = 0; i < frames.size(); ++i) {
        QNP_REQUIRE(buffer.consume(state, why));
        state = nextState(state, frames[i].actions[0], frames[i].actions[1]);
        buffer.recordPostState(frames[i].timelineFrame, state);
    }
    QNP_CHECK(buffer.verifyTerminal(why));
    QNP_CHECK(why == ParseStatus::Ok);

    TimelineTerminal wrongHash = good;
    wrongHash.postTailStateHash ^= 0x100ull;
    buffer.setTerminal(wrongHash);
    QNP_CHECK(!buffer.verifyTerminal(why));
    QNP_CHECK(why == ParseStatus::DigestMismatch);
    QNP_CHECK(buffer.lastFault() == TimelineFault::TerminalStateHashMismatch);

    TimelineTerminal wrongDigest = good;
    wrongDigest.finalDigest ^= 0x1ull;
    buffer.setTerminal(wrongDigest);
    QNP_CHECK(!buffer.verifyTerminal(why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::TerminalDigestMismatch);

    TimelineTerminal wrongCount = good;
    wrongCount.nextRecord = 6u;
    buffer.setTerminal(wrongCount);
    QNP_CHECK(!buffer.verifyTerminal(why));
    QNP_CHECK(why == ParseStatus::FrameGap);
    QNP_CHECK(buffer.lastFault() == TimelineFault::TerminalFrameMismatch);

    ReplayFrameV1 late = producer.make(0u, 0u);
    buffer.setTerminal(good);
    QNP_CHECK(!buffer.append(late, why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::AfterTerminal);
}

QNP_TEST(TimelineEmptyTerminalProvesInitialState) {
    ConfirmedTimelineBuffer buffer(8u);
    buffer.reset(makeStart());
    ParseStatus why = ParseStatus::Ok;

    TimelineTerminal terminal{};
    terminal.nextRecord = 0u;
    terminal.finalDigest = kInitialDigest;
    terminal.postTailStateHash = kInitialState;
    buffer.setTerminal(terminal);
    QNP_CHECK(buffer.verifyTerminal(why));

    terminal.postTailStateHash = kInitialState ^ 1ull;
    buffer.setTerminal(terminal);
    QNP_CHECK(!buffer.verifyTerminal(why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::TerminalStateHashMismatch);
}

QNP_TEST(TimelineMissingTerminalIsNotComplete) {
    ConfirmedTimelineBuffer buffer(8u);
    buffer.reset(makeStart());
    ParseStatus why = ParseStatus::Ok;
    QNP_CHECK(!buffer.verifyTerminal(why));
    QNP_CHECK(why == ParseStatus::MissingFooter);
    QNP_CHECK(buffer.lastFault() == TimelineFault::TerminalMissing);
}

QNP_TEST(TimelineTrimConsumedBelowBoundsRetention) {
    ConfirmedTimelineBuffer buffer(8u);
    buffer.reset(makeStart());
    Producer producer;
    ParseStatus why = ParseStatus::Ok;
    std::vector<ReplayFrameV1> frames;
    for (uint32_t i = 0; i < 8u; ++i) {
        frames.push_back(producer.make(0x0001u, 0u));
        QNP_REQUIRE(buffer.append(frames.back(), why));
    }
    QNP_CHECK(buffer.retainedFrames() == 8u);

    ReplayFrameV1 ninth = producer.make(0x0002u, 0u);
    QNP_CHECK(!buffer.append(ninth, why));
    QNP_CHECK(why == ParseStatus::LimitExceeded);
    QNP_CHECK(buffer.lastFault() == TimelineFault::Overflow);

    uint64_t state = kInitialState;
    for (size_t i = 0; i < 4u; ++i) {
        QNP_REQUIRE(buffer.consume(state, why));
        state = nextState(state, frames[i].actions[0], frames[i].actions[1]);
        buffer.recordPostState(frames[i].timelineFrame, state);
    }

    buffer.trimConsumedBelow(99u);
    QNP_CHECK(buffer.retentionFloor() == 4u);
    QNP_CHECK(buffer.retainedFrames() == 4u);

    QNP_CHECK(buffer.append(ninth, why));
    QNP_CHECK(buffer.nextFrame() == 9u);

    QNP_CHECK(!buffer.append(frames[0], why));
    QNP_CHECK(why == ParseStatus::SequenceRegression);
    QNP_CHECK(buffer.lastFault() == TimelineFault::DuplicateBelowRetention);

    QNP_CHECK(!buffer.rewindConsumeTo(3u, why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::RewindBelowRetention);
    QNP_REQUIRE(buffer.rewindConsumeTo(4u, why));
    QNP_CHECK(buffer.consumeCursor() == 4u);
    QNP_CHECK(buffer.bufferedFrames() == 5u);
    QNP_CHECK(buffer.consume(state, why));
}

QNP_TEST(TimelineEventsChainAndOrder) {
    ConfirmedTimelineBuffer buffer(32u);
    buffer.reset(makeStart());
    Producer producer;
    ParseStatus why = ParseStatus::Ok;

    for (uint32_t i = 0; i < 3u; ++i) {
        ReplayFrameV1 frame = producer.make(0x0001u, 0u);
        QNP_REQUIRE(buffer.append(frame, why));
    }

    ReplayEvent started{};
    started.timelineFrame = 3u;
    started.kind = EventKind::RoundStarted;
    started.matchId = 1u;
    started.valueA = 2u;
    producer.digest = sealEventDigest(producer.digest, started);
    QNP_REQUIRE(buffer.appendEvent(started, why));
    QNP_CHECK(buffer.rollingDigest() == producer.digest);
    QNP_CHECK(buffer.pendingEvents() == 1u);

    ReplayFrameV1 next = producer.make(0x0002u, 0u);
    QNP_CHECK(buffer.append(next, why));

    ReplayEvent future{};
    future.timelineFrame = 99u;
    future.kind = EventKind::MatchResult;
    future.rollingDigest = advanceReplayDigest(buffer.rollingDigest(), future);
    QNP_CHECK(!buffer.appendEvent(future, why));
    QNP_CHECK(why == ParseStatus::SequenceRegression);
    QNP_CHECK(buffer.lastFault() == TimelineFault::EventOutOfOrder);

    ReplayEvent backwards{};
    backwards.timelineFrame = 1u;
    backwards.kind = EventKind::RoundEnded;
    backwards.rollingDigest =
        advanceReplayDigest(buffer.rollingDigest(), backwards);
    QNP_CHECK(!buffer.appendEvent(backwards, why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::EventOutOfOrder);

    ReplayEvent unknown{};
    unknown.timelineFrame = 4u;
    unknown.kind = static_cast<EventKind>(9999);
    unknown.rollingDigest = advanceReplayDigest(buffer.rollingDigest(), unknown);
    QNP_CHECK(!buffer.appendEvent(unknown, why));
    QNP_CHECK(why == ParseStatus::BadEventPayload);

    ReplayEvent broken = started;
    broken.timelineFrame = 4u;
    broken.rollingDigest ^= 1ull;
    QNP_CHECK(!buffer.appendEvent(broken, why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::DigestChainBreak);

    ReplayEvent drained{};
    QNP_REQUIRE(buffer.consumeEvent(drained));
    QNP_CHECK(drained.kind == EventKind::RoundStarted);
    QNP_CHECK(buffer.eventCursor() == 1u);
    QNP_CHECK(buffer.pendingEvents() == 0u);
    QNP_CHECK(!buffer.consumeEvent(drained));
}

QNP_TEST(TimelineRefusesAppendBeforeReset) {
    ConfirmedTimelineBuffer buffer(8u);
    ParseStatus why = ParseStatus::Ok;
    ReplayFrameV1 frame = makeFrameFromConfirmedPair(0u, 0u, 0u, 0u, 0u);
    QNP_CHECK(!buffer.append(frame, why));
    QNP_CHECK(why == ParseStatus::MissingSessionStart);
    QNP_CHECK(buffer.lastFault() == TimelineFault::NotStarted);
}

QNP_TEST(TimelineRejectsImpossibleStartDescriptor) {
    ConfirmedTimelineBuffer buffer(8u);
    DeterministicStartDescriptor start = makeStart();
    start.activeLaneMask = 0x30u;
    buffer.reset(start);
    QNP_CHECK(buffer.lastFault() == TimelineFault::BadStartDescriptor);

    ParseStatus why = ParseStatus::Ok;
    ReplayFrameV1 frame = makeFrameFromConfirmedPair(0u, 0u, 0u, kInitialState, 0u);
    frame.rollingDigest = advanceReplayDigest(kInitialDigest, frame);
    QNP_CHECK(!buffer.append(frame, why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::BadStartDescriptor);

    DeterministicStartDescriptor badRoute = makeStart();
    badRoute.slotToSourceRoute[2] = 7u;
    buffer.reset(badRoute);
    QNP_CHECK(buffer.lastFault() == TimelineFault::BadStartDescriptor);
}

QNP_TEST(MakeFrameFromConfirmedPairShapesTwoActiveLanes) {
    const ReplayFrameV1 frame =
        makeFrameFromConfirmedPair(17u, 0x0105u, 0x0A02u, 0xFEEDull, 0xBEEFull);
    QNP_CHECK(frame.timelineFrame == 17u);
    QNP_CHECK(frame.inputDomain == InputDomain::LogicalSourceV1);
    QNP_CHECK(frame.activeLaneMask == 0x03u);
    QNP_CHECK(frame.actions[0] == 0x0105u);
    QNP_CHECK(frame.actions[1] == 0x0A02u);
    QNP_CHECK(frame.actions[2] == 0u && frame.actions[3] == 0u);
    QNP_CHECK(frame.preStateHash == 0xFEEDull);
    QNP_CHECK(frame.rollingDigest == 0xBEEFull);
    QNP_CHECK(frame.postStateHash == 0u);

    const ReplayFrameV1 dirty =
        makeFrameFromConfirmedPair(0u, 0xF0F0u, 0u, 0u, 0u);
    QNP_CHECK(dirty.actions[0] == 0xF0F0u);
}

QNP_TEST(TimelineHonoursNonZeroFirstFrame) {
    ConfirmedTimelineBuffer buffer(16u);
    DeterministicStartDescriptor start = makeStart(1000u);
    buffer.reset(start);
    QNP_CHECK(buffer.firstFrame() == 1000u);
    QNP_CHECK(buffer.nextFrame() == 1000u);
    QNP_CHECK(buffer.consumeCursor() == 1000u);

    ParseStatus why = ParseStatus::Ok;
    ReplayFrameV1 early =
        makeFrameFromConfirmedPair(999u, 0u, 0u, kInitialState, 0u);
    early.rollingDigest = advanceReplayDigest(kInitialDigest, early);
    QNP_CHECK(!buffer.append(early, why));
    QNP_CHECK(buffer.lastFault() == TimelineFault::DuplicateBelowRetention);

    Producer producer;
    producer.frame = 1000u;
    ReplayFrameV1 first = producer.make(0x0001u, 0u);
    QNP_CHECK(buffer.append(first, why));
    QNP_CHECK(buffer.nextFrame() == 1001u);
}
