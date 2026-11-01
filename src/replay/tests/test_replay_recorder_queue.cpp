#include "TestHarness.h"

#include "replay/ReplayRecorderQueue.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace qreplay;

namespace {

RecorderMessage frameMessage(uint32_t timelineFrame, uint16_t p1, uint16_t p2) {
    RecorderMessage message{};
    message.kind = RecorderMessageKind::Frame;
    message.frame.timelineFrame = timelineFrame;
    message.frame.inputDomain = InputDomain::LogicalSourceV1;
    message.frame.activeLaneMask = 0x03u;
    message.frame.actions[0] = p1;
    message.frame.actions[1] = p2;
    message.frame.preStateHash = 0x1000ull + timelineFrame;
    message.frame.rollingDigest = 0x2000ull + timelineFrame;
    return message;
}

}

QNP_TEST(RecorderQueueFillsToCapacityWithoutDropping) {
    ReplayRecorderQueue queue(8u);
    QNP_CHECK(queue.capacity() == 8u);
    QNP_CHECK(queue.empty());
    for (uint32_t i = 0; i < 8u; ++i) {
        QNP_CHECK(queue.publish(frameMessage(i, 0x0001u, 0x0100u)));
    }
    QNP_CHECK(queue.pending() == 8u);
    QNP_CHECK(queue.full());
    QNP_CHECK(queue.droppedCount() == 0u);
    QNP_CHECK(queue.highWater() == 8u);
    QNP_CHECK(!queue.disabled());

    for (uint32_t i = 0; i < 8u; ++i) {
        RecorderMessage out{};
        QNP_REQUIRE(queue.drain(out));
        QNP_CHECK(out.kind == RecorderMessageKind::Frame);
        QNP_CHECK(out.sequence == i + 1u);
        QNP_CHECK(out.frame.timelineFrame == i);
        QNP_CHECK(out.frame.actions[0] == 0x0001u);
    }
    RecorderMessage empty{};
    QNP_CHECK(!queue.drain(empty));
}

QNP_TEST(RecorderQueueOnePastCapacityLatchesDisabled) {
    ReplayRecorderQueue queue(4u);
    for (uint32_t i = 0; i < 4u; ++i) {
        QNP_CHECK(queue.publish(frameMessage(i, 0u, 0u)));
    }
    QNP_CHECK(!queue.publish(frameMessage(4u, 0u, 0u)));
    QNP_CHECK(queue.droppedCount() == 1u);
    QNP_CHECK(queue.disabled());

    RecorderMessage out{};
    QNP_REQUIRE(queue.drain(out));
    QNP_CHECK(out.frame.timelineFrame == 0u);
    QNP_CHECK(!queue.publish(frameMessage(5u, 0u, 0u)));
    QNP_CHECK(queue.droppedCount() == 2u);
    QNP_CHECK(queue.disabled());

    for (uint32_t i = 1; i < 4u; ++i) {
        QNP_REQUIRE(queue.drain(out));
        QNP_CHECK(out.frame.timelineFrame == i);
    }
    QNP_CHECK(!queue.drain(out));
}

QNP_TEST(RecorderQueueTracksHighWater) {
    ReplayRecorderQueue queue(16u);
    RecorderMessage out{};
    for (uint32_t i = 0; i < 3u; ++i) {
        QNP_CHECK(queue.publish(frameMessage(i, 0u, 0u)));
    }
    QNP_CHECK(queue.highWater() == 3u);
    for (uint32_t i = 0; i < 3u; ++i) QNP_CHECK(queue.drain(out));
    QNP_CHECK(queue.pending() == 0u);
    QNP_CHECK(queue.highWater() == 3u);

    for (uint32_t i = 0; i < 5u; ++i) {
        QNP_CHECK(queue.publish(frameMessage(100u + i, 0u, 0u)));
    }
    QNP_CHECK(queue.highWater() == 5u);
    for (uint32_t i = 0; i < 5u; ++i) QNP_CHECK(queue.drain(out));
    QNP_CHECK(queue.highWater() == 5u);
}

QNP_TEST(RecorderQueueIsolatesGenerations) {
    ReplayRecorderQueue queue(8u);
    queue.beginGeneration(1u);
    QNP_CHECK(queue.generation() == 1u);
    QNP_CHECK(queue.publishFor(1u, frameMessage(0u, 0u, 0u)));
    QNP_CHECK(queue.pending() == 1u);

    queue.beginGeneration(2u);
    QNP_CHECK(queue.generation() == 2u);
    QNP_CHECK(queue.pending() == 0u);
    RecorderMessage out{};
    QNP_CHECK(!queue.drain(out));

    QNP_CHECK(!queue.publishFor(1u, frameMessage(7u, 0u, 0u)));
    QNP_CHECK(queue.staleRejectCount() == 1u);
    QNP_CHECK(queue.droppedCount() == 0u);
    QNP_CHECK(!queue.disabled());
    QNP_CHECK(queue.pending() == 0u);

    QNP_CHECK(queue.publishFor(2u, frameMessage(9u, 0x0002u, 0u)));
    QNP_REQUIRE(queue.drain(out));
    QNP_CHECK(out.frame.timelineFrame == 9u);
    QNP_CHECK(out.sequence == 1u);
}

QNP_TEST(RecorderQueueGenerationClearsDisableLatch) {
    ReplayRecorderQueue queue(2u);
    queue.beginGeneration(5u);
    QNP_CHECK(queue.publish(frameMessage(0u, 0u, 0u)));
    QNP_CHECK(queue.publish(frameMessage(1u, 0u, 0u)));
    QNP_CHECK(!queue.publish(frameMessage(2u, 0u, 0u)));
    QNP_CHECK(queue.disabled());
    QNP_CHECK(queue.droppedCount() == 1u);

    queue.beginGeneration(6u);
    QNP_CHECK(!queue.disabled());
    QNP_CHECK(queue.droppedCount() == 0u);
    QNP_CHECK(queue.highWater() == 0u);
    QNP_CHECK(queue.publish(frameMessage(0u, 0u, 0u)));
}

QNP_TEST(RecorderQueueCarriesEveryMessageKind) {
    ReplayRecorderQueue queue(16u);
    RecorderMessage out{};

    RecorderMessage sessionStart{};
    sessionStart.kind = RecorderMessageKind::SessionStart;
    sessionStart.matchStart.start.seedGameplay = 0xAABBCCDDu;
    sessionStart.matchStart.start.initialStateHash = 0x1234ull;
    QNP_CHECK(queue.publish(sessionStart));

    RecorderMessage matchStart{};
    matchStart.kind = RecorderMessageKind::MatchStart;
    matchStart.matchStart.matchId = 3u;
    matchStart.matchStart.ordinal = 2u;
    matchStart.matchStart.independentBootstrap = true;
    matchStart.matchStart.slots[0].identity.nativeCharacterId = 24u;
    QNP_CHECK(queue.publish(matchStart));

    RecorderMessage event{};
    event.kind = RecorderMessageKind::Event;
    event.event.kind = EventKind::RosterSelected;
    event.event.rosterEpoch = 4u;
    event.event.roster[1].nativeCharacterId = 30u;
    QNP_CHECK(queue.publish(event));

    RecorderMessage matchEnd{};
    matchEnd.kind = RecorderMessageKind::MatchEnd;
    matchEnd.matchEnd.matchId = 3u;
    matchEnd.matchEnd.winningSideMask = 0x05u;
    matchEnd.matchEnd.postTailStateHash = 0x99ull;
    QNP_CHECK(queue.publish(matchEnd));

    RecorderMessage terminal{};
    terminal.kind = RecorderMessageKind::Terminal;
    terminal.sessionEnd.reason = TerminalReason::RecorderOverflow;
    terminal.sessionEnd.lastTimelineFrame = 4242u;
    QNP_CHECK(queue.publish(terminal));

    QNP_REQUIRE(queue.drain(out));
    QNP_CHECK(out.kind == RecorderMessageKind::SessionStart);
    QNP_CHECK(out.matchStart.start.seedGameplay == 0xAABBCCDDu);

    QNP_REQUIRE(queue.drain(out));
    QNP_CHECK(out.kind == RecorderMessageKind::MatchStart);
    QNP_CHECK(out.matchStart.ordinal == 2u);
    QNP_CHECK(out.matchStart.independentBootstrap);
    QNP_CHECK(out.matchStart.slots[0].identity.nativeCharacterId == 24u);

    QNP_REQUIRE(queue.drain(out));
    QNP_CHECK(out.kind == RecorderMessageKind::Event);
    QNP_CHECK(out.event.kind == EventKind::RosterSelected);
    QNP_CHECK(out.event.roster[1].nativeCharacterId == 30u);

    QNP_REQUIRE(queue.drain(out));
    QNP_CHECK(out.kind == RecorderMessageKind::MatchEnd);
    QNP_CHECK(out.matchEnd.winningSideMask == 0x05u);

    QNP_REQUIRE(queue.drain(out));
    QNP_CHECK(out.kind == RecorderMessageKind::Terminal);
    QNP_CHECK(out.sessionEnd.reason == TerminalReason::RecorderOverflow);
    QNP_CHECK(out.sessionEnd.lastTimelineFrame == 4242u);
}

QNP_TEST(RecorderQueueManualDisableStopsPublishing) {
    ReplayRecorderQueue queue(8u);
    QNP_CHECK(queue.publish(frameMessage(0u, 0u, 0u)));
    queue.disable();
    QNP_CHECK(queue.disabled());
    QNP_CHECK(!queue.publish(frameMessage(1u, 0u, 0u)));
    QNP_CHECK(queue.droppedCount() == 1u);
}

QNP_TEST(RecorderQueueClampsCapacity) {
    ReplayRecorderQueue zero(0u);
    QNP_CHECK(zero.capacity() == 1u);
    QNP_CHECK(zero.publish(frameMessage(0u, 0u, 0u)));
    QNP_CHECK(!zero.publish(frameMessage(1u, 0u, 0u)));

    ReplayRecorderQueue huge(kMaxRecorderQueueCapacity * 4u);
    QNP_CHECK(huge.capacity() == kMaxRecorderQueueCapacity);
}

QNP_TEST(RecorderQueueConcurrentProducerConsumerLosesNothing) {
    constexpr uint32_t kMessages = 20000u;
    constexpr uint64_t kSpinLimit = 20000000ull;

    ReplayRecorderQueue queue(64u);
    std::atomic<bool> producerStuck{false};
    std::atomic<bool> publishRejected{false};

    std::thread producer([&]() {
        for (uint32_t i = 0; i < kMessages; ++i) {
            uint64_t spins = 0u;
            while (queue.full()) {
                if (++spins > kSpinLimit) { producerStuck.store(true); return; }
                std::this_thread::yield();
            }
            if (!queue.publish(frameMessage(i, static_cast<uint16_t>(i & 0x0Fu),
                                            0x0100u))) {
                publishRejected.store(true);
                return;
            }
        }
    });

    std::vector<uint8_t> seen(kMessages, 0u);
    uint32_t received = 0u;
    uint32_t outOfOrder = 0u;
    uint32_t duplicated = 0u;
    uint64_t spins = 0u;
    bool consumerStuck = false;
    while (received < kMessages) {
        RecorderMessage out{};
        if (!queue.drain(out)) {
            if (++spins > kSpinLimit) { consumerStuck = true; break; }
            std::this_thread::yield();
            continue;
        }
        if (out.sequence != received + 1u) ++outOfOrder;
        const uint32_t index = out.frame.timelineFrame;
        if (index >= kMessages || seen[index] != 0u) {
            ++duplicated;
        } else {
            seen[index] = 1u;
        }
        ++received;
    }
    producer.join();

    QNP_CHECK(!producerStuck.load());
    QNP_CHECK(!publishRejected.load());
    QNP_CHECK(!consumerStuck);
    QNP_CHECK(received == kMessages);
    QNP_CHECK(outOfOrder == 0u);
    QNP_CHECK(duplicated == 0u);
    QNP_CHECK(queue.droppedCount() == 0u);
    QNP_CHECK(!queue.disabled());
    QNP_CHECK(queue.highWater() <= queue.capacity());

    uint32_t missing = 0u;
    for (uint32_t i = 0; i < kMessages; ++i) {
        if (seen[i] == 0u) ++missing;
    }
    QNP_CHECK(missing == 0u);
}
