#include "TestHarness.h"

#include "replay/ReplayPublisherQueueV2.h"

using namespace qreplay;

namespace {

PublisherRecordV2 recordOf(PublisherRecordKindV2 kind, uint32_t marker) {
    PublisherRecordV2 record{};
    record.kind = kind;
    if (kind == PublisherRecordKindV2::BeginGame)
        record.begin.frameZero.frame.sourceFrame = marker;
    else if (kind == PublisherRecordKindV2::Tick)
        record.tick.frame.sourceFrame = marker;
    else
        record.end.end.nextRecord = marker;
    return record;
}

uint32_t markerOf(const PublisherRecordV2& record) {
    if (record.kind == PublisherRecordKindV2::BeginGame)
        return record.begin.frameZero.frame.sourceFrame;
    if (record.kind == PublisherRecordKindV2::Tick)
        return record.tick.frame.sourceFrame;
    return record.end.end.nextRecord;
}

}

QNP_TEST(ReplayPublisherQueueV2RejectsInvalidCapacityAndGeneration) {
    ReplayPublisherQueueV2 tooSmall(1u);
    ReplayPublisherQueueV2 tooLarge(kMaxReplayPublisherCapacityV2 + 1u);
    QNP_CHECK(!tooSmall.valid());
    QNP_CHECK(!tooLarge.valid());

    ReplayPublisherQueueV2 queue(4u);
    QNP_REQUIRE(queue.valid());
    queue.beginGeneration(7u);
    QNP_CHECK(queue.tryPublishOrdinaryFor(
        6u, recordOf(PublisherRecordKindV2::Tick, 1u)) == false);
    QNP_CHECK(queue.staleRejects() == 1u);
    QNP_CHECK(queue.pending() == 0u);
}

QNP_TEST(ReplayPublisherQueueV2PreservesTheReservedEndSlot) {
    ReplayPublisherQueueV2 queue(3u);
    QNP_REQUIRE(queue.valid());
    queue.beginGeneration(11u);

    QNP_REQUIRE(queue.tryPublishOrdinary(
        recordOf(PublisherRecordKindV2::BeginGame, 100u)));
    QNP_REQUIRE(queue.tryPublishOrdinary(
        recordOf(PublisherRecordKindV2::Tick, 101u)));
    QNP_CHECK(!queue.tryPublishOrdinary(
        recordOf(PublisherRecordKindV2::Tick, 102u)));
    QNP_CHECK(queue.ordinaryOverflowed());
    QNP_CHECK(queue.pending() == 2u);

    QNP_REQUIRE(queue.publishEndGameReserved(
        recordOf(PublisherRecordKindV2::EndGame, 2u)));
    QNP_CHECK(queue.reservedEndUsed());
    QNP_CHECK(queue.pending() == 3u);
    QNP_CHECK(!queue.publishEndGameReserved(
        recordOf(PublisherRecordKindV2::EndGame, 2u)));

    PublisherRecordV2 drained{};
    QNP_REQUIRE(queue.drain(drained));
    QNP_CHECK(drained.kind == PublisherRecordKindV2::BeginGame);
    QNP_CHECK(markerOf(drained) == 100u);
    QNP_REQUIRE(queue.drain(drained));
    QNP_CHECK(drained.kind == PublisherRecordKindV2::Tick);
    QNP_CHECK(markerOf(drained) == 101u);
    QNP_REQUIRE(queue.drain(drained));
    QNP_CHECK(drained.kind == PublisherRecordKindV2::EndGame);
    QNP_CHECK(markerOf(drained) == 2u);
    QNP_CHECK(!queue.reservedEndUsed());
    QNP_CHECK(!queue.drain(drained));
}

QNP_TEST(ReplayPublisherQueueV2AllowsNextGameOnlyBehindPriorEnd) {
    ReplayPublisherQueueV2 queue(5u);
    QNP_REQUIRE(queue.valid());
    queue.beginGeneration(3u);

    QNP_REQUIRE(queue.tryPublishOrdinary(
        recordOf(PublisherRecordKindV2::BeginGame, 10u)));
    QNP_REQUIRE(queue.publishEndGameReserved(
        recordOf(PublisherRecordKindV2::EndGame, 1u)));
    QNP_REQUIRE(queue.tryPublishOrdinary(
        recordOf(PublisherRecordKindV2::BeginGame, 20u)));
    QNP_CHECK(!queue.publishEndGameReserved(
        recordOf(PublisherRecordKindV2::EndGame, 1u)));

    PublisherRecordV2 drained{};
    QNP_REQUIRE(queue.drain(drained));
    QNP_CHECK(markerOf(drained) == 10u);
    QNP_REQUIRE(queue.drain(drained));
    QNP_CHECK(drained.kind == PublisherRecordKindV2::EndGame);
    QNP_CHECK(!queue.reservedEndUsed());
    QNP_REQUIRE(queue.publishEndGameReserved(
        recordOf(PublisherRecordKindV2::EndGame, 1u)));

    QNP_REQUIRE(queue.drain(drained));
    QNP_CHECK(drained.kind == PublisherRecordKindV2::BeginGame);
    QNP_CHECK(markerOf(drained) == 20u);
    QNP_REQUIRE(queue.drain(drained));
    QNP_CHECK(drained.kind == PublisherRecordKindV2::EndGame);
}

QNP_TEST(ReplayPublisherQueueV2RejectsWrongPublicationClass) {
    ReplayPublisherQueueV2 queue(4u);
    queue.beginGeneration(5u);
    QNP_CHECK(!queue.tryPublishOrdinary(
        recordOf(PublisherRecordKindV2::EndGame, 1u)));
    QNP_CHECK(!queue.publishEndGameReserved(
        recordOf(PublisherRecordKindV2::Tick, 0u)));
    QNP_CHECK(queue.pending() == 0u);
}
