#include "SpectatorV4TestFixtures.h"

#include "../../client/spectator/SpectatorReplaySaveAdapter.h"
#include "../../game/spectator/SpectatorPublisher.h"
#include "../../game/spectator/SpectatorReplayPublisherV4.h"
#include "../../netplay/tests/TestHarness.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

namespace qsp = qnetplay::spectator;
namespace fixture = qnetplay::spectator::testv4;
namespace save = qclient::spectator;
namespace publish = qgame::spectator_publish;

struct FakeSaveSink final : save::SpectatorReplaySaveSinkV4 {
    struct Saved {
        qreplay::ConfirmedGameStartV2 start{};
        std::vector<qreplay::CanonicalTickBundleV2> ticks;
        qreplay::ConfirmedGameEndV2 end{};
    };

    bool begin(const qreplay::ConfirmedGameStartV2& start) override {
        if (failBegin || active) return false;
        current = Saved{};
        current.start = start;
        active = true;
        return true;
    }
    bool append(
        const qreplay::CanonicalTickBundleV2& tick) override {
        if (failAppend || !active) return false;
        current.ticks.push_back(tick);
        return true;
    }
    bool finalize(const qreplay::ConfirmedGameEndV2& end) override {
        if (failFinalize || !active) return false;
        current.end = end;
        saved.push_back(current);
        current = Saved{};
        active = false;
        return true;
    }
    void abort(save::SpectatorReplaySaveFailureV4 reason) override {
        aborts.push_back(reason);
        current = Saved{};
        active = false;
    }

    bool failBegin = false;
    bool failAppend = false;
    bool failFinalize = false;
    bool active = false;
    Saved current{};
    std::vector<Saved> saved;
    std::vector<save::SpectatorReplaySaveFailureV4> aborts;
};

struct BuiltSet {
    qsp::ConnectedSetDescriptorV4 descriptor{};
    std::vector<qsp::ConnectedPublicationV4> publications;
};

BuiltSet threeGameSet(bool withSeal = true) {
    BuiltSet out{};
    out.descriptor = fixture::descriptor();
    qsp::ConnectedChainBuilderV4 builder;
    if (!builder.reset(out.descriptor)) return out;
    for (uint32_t game = 1u; game <= 3u; ++game) {
        if (!fixture::appendCompletedGame(
                builder, game, out.publications)) {
            out.publications.clear();
            return out;
        }
        if (game != 3u)
            out.publications.push_back(
                fixture::setEvent(builder, game));
    }
    if (withSeal) {
        const auto& final =
            out.publications[out.publications.size() - 1u];
        out.publications.push_back(fixture::seal(
            builder,
            final.end.end.end.postTailStateHash));
    }
    return out;
}

bool commitAll(
    qsp::SpectatorConnectedArchiveV4& archive,
    const BuiltSet& set, uint64_t sealTime = 1000u) {
    if (!archive.reset(set.descriptor)) return false;
    for (const qsp::ConnectedPublicationV4& publication :
         set.publications) {
        if (archive.commit(publication, sealTime)
            != qsp::ConnectedCommitResultV4::Committed) {
            return false;
        }
    }
    return true;
}

bool coordinatorRecordsForOneGame(
    const std::vector<qsp::ConnectedPublicationV4>& connected,
    std::array<qreplay::PublisherRecordV2, 3u>& out) {
    if (connected.size() != 3u
        || connected[0].kind
            != qsp::ConnectedPublicationKindV4::BeginGameBundle
        || connected[1].kind
            != qsp::ConnectedPublicationKindV4::ConfirmedRecord
        || connected[2].kind
            != qsp::ConnectedPublicationKindV4::EndGameBundle) {
        return false;
    }
    const uint32_t startCursor =
        connected[0].begin.start.chain.recordCursorBefore;
    uint32_t eventSequence = 0u;
    uint64_t rolling =
        connected[0].begin.start.start.start.initialRollingDigest;

    out = {};
    out[0].kind = qreplay::PublisherRecordKindV2::BeginGame;
    out[0].begin.start = connected[0].begin.start.start;
    if (!qsp::rebaseConnectedRecordV4(
            connected[0].begin.frameZero, startCursor,
            eventSequence, rolling, out[0].begin.frameZero)) {
        return false;
    }
    out[1].kind = qreplay::PublisherRecordKindV2::Tick;
    if (!qsp::rebaseConnectedRecordV4(
            connected[1].record, startCursor,
            eventSequence, rolling, out[1].tick)) {
        return false;
    }
    out[2].kind = qreplay::PublisherRecordKindV2::EndGame;
    out[2].end.containsFinalFrame =
        connected[2].end.containsFinalRecord;
    if (out[2].end.containsFinalFrame
        && !qsp::rebaseConnectedRecordV4(
            connected[2].end.finalRecord, startCursor,
            eventSequence, rolling, out[2].end.finalFrame)) {
        return false;
    }
    out[2].end.end = connected[2].end.end.end;
    return true;
}

}

QNP_TEST(SpectatorV4FixtureStartIsValid) {
    const qreplay::ConfirmedGameStartV2 value = fixture::start(1u);
    QNP_CHECK(qreplay::isValidGameIdentityV2(
        value.manifest.identity));
    QNP_CHECK(qreplay::isValidSimulationContractV2(
        value.manifest.contract));
    QNP_CHECK(qreplay::isValidGameManifestV2(value.manifest));
    QNP_CHECK(qreplay::isValidDeterministicGameStartV2(value.start));
    std::vector<uint8_t> bytes;
    QNP_CHECK(qreplay::encodeConfirmedGameStartV2(value, bytes));
    QNP_CHECK(value.start.initialRollingDigest
              == qreplay::initialReplayDigestV2(value));
    const qsp::ConnectedConfirmedRecordV4 zero = fixture::record(
        value.manifest.identity, 1u, 0u, true, false);
    QNP_CHECK(qsp::isCanonicalConnectedRecordV4(zero));
    qsp::ConnectedChainBuilderV4 builder;
    QNP_REQUIRE(builder.reset(fixture::descriptor()));
    qsp::ConnectedPublicationV4 begin{};
    begin.kind = qsp::ConnectedPublicationKindV4::BeginGameBundle;
    begin.begin.start.game = value.manifest.identity;
    begin.begin.start.start = value;
    begin.begin.frameZero = zero;
    QNP_REQUIRE(builder.stamp(begin));
    qreplay::CanonicalGameDigestV2 canonical;
    QNP_REQUIRE(canonical.begin(value));
    uint32_t eventSequence = 0u;
    uint64_t rolling = value.start.initialRollingDigest;
    QNP_CHECK(fixture::fold(
        begin.begin.frameZero, 0u, eventSequence, rolling, canonical));
    qsp::ConnectedPublicationV4 middle{};
    middle.kind = qsp::ConnectedPublicationKindV4::ConfirmedRecord;
    middle.record = fixture::record(
        value.manifest.identity, 1u, 1u, false, false);
    QNP_REQUIRE(builder.stamp(middle));
    QNP_CHECK(middle.record.chain.recordCursorBefore == 1u);
    QNP_CHECK(qsp::isCanonicalConnectedRecordV4(middle.record));
    qreplay::CanonicalTickBundleV2 middleTick{};
    uint32_t middleEvents = eventSequence;
    uint64_t middleRolling = rolling;
    QNP_REQUIRE(qsp::rebaseConnectedRecordV4(
        middle.record, 0u, middleEvents, middleRolling, middleTick));
    QNP_CHECK(middleTick.frame.replayFrame == 1u);
    QNP_CHECK(canonical.append(middleTick.frame));
    eventSequence = middleEvents;
    rolling = middleRolling;
    qsp::ConnectedConfirmedRecordV4 final = fixture::record(
        value.manifest.identity, 1u, 2u, false, true);
    final.chain.recordCursorBefore = builder.nextRecordCursor();
    QNP_CHECK(fixture::fold(
        final, 0u, eventSequence, rolling, canonical));
}

QNP_TEST(SpectatorV4CodecRejectsEveryTruncationAndBadFraming) {
    BuiltSet set = threeGameSet(false);
    QNP_REQUIRE(!set.publications.empty());
    std::vector<uint8_t> descriptorBytes;
    QNP_REQUIRE(qsp::encodeConnectedSetDescriptorV4(
        set.descriptor, descriptorBytes));
    QNP_CHECK(descriptorBytes.size()
              == qsp::kConnectedDescriptorBytesV4);
    qsp::ConnectedSetDescriptorV4 descriptorOut{};
    QNP_CHECK(qsp::decodeConnectedSetDescriptorV4(
        descriptorBytes.data(), descriptorBytes.size(), descriptorOut)
        == qsp::ConnectedCodecErrorV4::None);
    QNP_CHECK(qsp::computeConnectedGenesisDigestV4(descriptorOut)
              == qsp::computeConnectedGenesisDigestV4(set.descriptor));
    for (size_t length = 0u; length < descriptorBytes.size(); ++length) {
        QNP_CHECK(qsp::decodeConnectedSetDescriptorV4(
            length == 0u ? nullptr : descriptorBytes.data(), length,
            descriptorOut) != qsp::ConnectedCodecErrorV4::None);
    }
    descriptorBytes.back() ^= 1u;
    QNP_CHECK(qsp::decodeConnectedSetDescriptorV4(
        descriptorBytes.data(), descriptorBytes.size(), descriptorOut)
        == qsp::ConnectedCodecErrorV4::BadChecksum);

    const qsp::ConnectedPublicationV4& publication =
        set.publications.front();
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(qsp::encodeConnectedPublicationV4(publication, bytes));
    QNP_CHECK(bytes.size() > qsp::kMaxConnectedFragmentBytesV4);
    qsp::ConnectedPublicationV4 decoded{};
    QNP_CHECK(qsp::decodeConnectedPublicationV4(
        bytes.data(), bytes.size(), decoded)
        == qsp::ConnectedCodecErrorV4::None);
    QNP_CHECK(decoded == publication);

    for (size_t length = 0u; length < bytes.size(); ++length) {
        QNP_CHECK(qsp::decodeConnectedPublicationV4(
            length == 0u ? nullptr : bytes.data(), length, decoded)
            != qsp::ConnectedCodecErrorV4::None);
    }
    std::vector<uint8_t> damaged = bytes;
    damaged.back() ^= 0x80u;
    QNP_CHECK(qsp::decodeConnectedPublicationV4(
        damaged.data(), damaged.size(), decoded)
        == qsp::ConnectedCodecErrorV4::BadChecksum);
    damaged = bytes;
    damaged.push_back(0u);
    QNP_CHECK(qsp::decodeConnectedPublicationV4(
        damaged.data(), damaged.size(), decoded)
        == qsp::ConnectedCodecErrorV4::BadLength);

    qsp::SpectatorResumeCursorV4 cursor{};
    cursor.setStreamId = set.descriptor.setStreamId;
    cursor.setDigestBeforeNext =
        qsp::computeConnectedGenesisDigestV4(set.descriptor);
    cursor.viewerGeneration = 7u;
    bytes.clear();
    QNP_REQUIRE(qsp::encodeSpectatorResumeCursorV4(cursor, bytes));
    QNP_CHECK(bytes.size() == 32u);
    qsp::SpectatorResumeCursorV4 cursorOut{};
    QNP_CHECK(qsp::decodeSpectatorResumeCursorV4(
        bytes.data(), bytes.size(), cursorOut)
        == qsp::ConnectedCodecErrorV4::None);
    QNP_CHECK(cursorOut == cursor);
    bytes[31] = 1u;
    QNP_CHECK(qsp::decodeSpectatorResumeCursorV4(
        bytes.data(), bytes.size(), cursorOut)
        == qsp::ConnectedCodecErrorV4::BadScalar);
}

QNP_TEST(SpectatorV4FragmentsReorderAndDuplicateWithoutPartialCommit) {
    BuiltSet set = threeGameSet(false);
    QNP_REQUIRE(!set.publications.empty());
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(qsp::encodeConnectedPublicationV4(
        set.publications.front(), bytes));
    std::vector<qsp::ConnectedPublicationFragmentV4> fragments;
    QNP_REQUIRE(qsp::fragmentConnectedPublicationV4(
        set.descriptor.setStreamId,
        qsp::firstItemSequenceV4(set.publications.front()),
        set.publications.front().kind, bytes, fragments));
    QNP_REQUIRE(fragments.size() >= 2u);

    qsp::ConnectedPublicationAssemblerV4 assembler;
    qsp::ConnectedPublicationV4 completed{};
    QNP_CHECK(assembler.accept(fragments.back(), completed)
              == qsp::ConnectedAssemblyResultV4::Accepted);
    QNP_CHECK(assembler.accept(fragments.back(), completed)
              == qsp::ConnectedAssemblyResultV4::ExactDuplicate);
    for (size_t index = fragments.size() - 1u; index-- > 0u;) {
        const qsp::ConnectedAssemblyResultV4 result =
            assembler.accept(fragments[index], completed);
        QNP_CHECK(result
            == (index == 0u
                    ? qsp::ConnectedAssemblyResultV4::Complete
                    : qsp::ConnectedAssemblyResultV4::Accepted));
    }
    QNP_CHECK(completed == set.publications.front());

    assembler.reset();
    QNP_CHECK(assembler.accept(fragments[0], completed)
              == qsp::ConnectedAssemblyResultV4::Accepted);
    auto conflict = fragments[0];
    conflict.bytes[0] ^= 1u;
    QNP_CHECK(assembler.accept(conflict, completed)
              == qsp::ConnectedAssemblyResultV4::Conflict);
    QNP_CHECK(assembler.failed());
}

QNP_TEST(SpectatorV4ThreeRematchesAreExplicitNonoverlappingRanges) {
    BuiltSet set = threeGameSet();
    QNP_REQUIRE(set.publications.size() == 12u);
    qsp::SpectatorConnectedArchiveV4 archive;
    QNP_REQUIRE(archive.reset(set.descriptor));
    for (size_t i = 0u; i < set.publications.size(); ++i) {
        const auto result = archive.commit(set.publications[i], 1000u);
        if (result != qsp::ConnectedCommitResultV4::Committed)
            std::printf("    commit[%zu] kind=%u result=%u\n", i,
                        static_cast<unsigned>(set.publications[i].kind),
                        static_cast<unsigned>(result));
        QNP_CHECK(result == qsp::ConnectedCommitResultV4::Committed);
        QNP_CHECK(archive.commit(set.publications[i], 1000u)
                  == qsp::ConnectedCommitResultV4::ExactDuplicate);
        if (i + 1u != set.publications.size())
            QNP_CHECK(!archive.sealed());
    }
    QNP_CHECK(archive.sealed());
    QNP_CHECK(archive.retainedUntilMs()
              == 1000u + qsp::kSpectatorPostGameRetentionMsV4);
    QNP_REQUIRE(archive.ranges().size() == 3u);
    for (size_t i = 0u; i < archive.ranges().size(); ++i) {
        const qsp::ConnectedGameRangeV4& range = archive.ranges()[i];
        QNP_CHECK(range.complete);
        QNP_CHECK(range.startRecordCursor == i * 3u);
        QNP_CHECK(range.nextRecordCursor == i * 3u + 3u);
        QNP_CHECK(range.end.nextRecord == 3u);
        if (i != 0u) {
            QNP_CHECK(archive.ranges()[i - 1u].nextRecordCursor
                      <= range.startRecordCursor);
        }
    }
}

QNP_TEST(SpectatorV4GapConflictAndIdentityReuseFailClosed) {
    BuiltSet set = threeGameSet(false);
    QNP_REQUIRE(set.publications.size() >= 4u);

    qsp::SpectatorConnectedArchiveV4 gap;
    QNP_REQUIRE(gap.reset(set.descriptor));
    QNP_CHECK(gap.commit(set.publications[1], 0u)
              == qsp::ConnectedCommitResultV4::SequenceGap);
    QNP_CHECK(gap.failed());

    qsp::SpectatorConnectedArchiveV4 duplicate;
    QNP_REQUIRE(duplicate.reset(set.descriptor));
    QNP_REQUIRE(duplicate.commit(set.publications[0], 0u)
                == qsp::ConnectedCommitResultV4::Committed);
    auto conflicting = set.publications[0];
    conflicting.begin.start.start.manifest.display.recordedUnixSeconds++;
    QNP_CHECK(duplicate.commit(conflicting, 0u)
              == qsp::ConnectedCommitResultV4::ConflictingDuplicate);
    QNP_CHECK(duplicate.failed());

    qsp::ConnectedChainBuilderV4 builder;
    QNP_REQUIRE(builder.reset(set.descriptor));
    std::vector<qsp::ConnectedPublicationV4> reused;
    QNP_REQUIRE(fixture::appendCompletedGame(builder, 1u, reused));
    QNP_REQUIRE(fixture::appendCompletedGame(builder, 1u, reused));
    qsp::SpectatorConnectedArchiveV4 identity;
    QNP_REQUIRE(identity.reset(set.descriptor));
    for (size_t i = 0u; i < 3u; ++i)
        QNP_REQUIRE(identity.commit(reused[i], 0u)
                    == qsp::ConnectedCommitResultV4::Committed);
    QNP_CHECK(identity.commit(reused[3], 0u)
              == qsp::ConnectedCommitResultV4::RangeOverlap);
    QNP_CHECK(identity.failed());
}

QNP_TEST(SpectatorV4PaletteReachesViewersWithoutMovingTheRecordCursor) {
    const qsp::ConnectedSetDescriptorV4 descriptor = fixture::descriptor();

    qreplay::ReplayPaletteV2 palette{};
    palette.side = 1u;
    palette.variant = 2u;
    palette.present = 1u;
    for (size_t variant = 0; variant < 4u; ++variant) {
        for (size_t colour = 1; colour < 16u; ++colour) {
            uint8_t* entry = palette.table + variant * 64u + colour * 4u;
            entry[0] = static_cast<uint8_t>(colour * 9u + variant);
            entry[1] = static_cast<uint8_t>(colour * 6u + variant);
            entry[2] = static_cast<uint8_t>(colour * 4u + variant);
        }
    }

    qsp::ConnectedChainBuilderV4 builder;
    QNP_REQUIRE(builder.reset(descriptor));
    std::vector<qsp::ConnectedPublicationV4> stream;
    QNP_REQUIRE(fixture::appendCompletedGame(builder, 1u, stream));
    QNP_REQUIRE(stream.size() == 3u);
    std::array<qreplay::PublisherRecordV2, 3u> source{};
    QNP_REQUIRE(coordinatorRecordsForOneGame(stream, source));

    publish::ConnectedPublisherQueueV4 queue(8u);
    publish::ConnectedReplayPublisherV4 adapter(queue);
    QNP_REQUIRE(adapter.reset(descriptor));
    QNP_REQUIRE(adapter.tryPublishOrdinary(source[0]));

    qreplay::PublisherRecordV2 paletteRecord{};
    paletteRecord.kind = qreplay::PublisherRecordKindV2::Palette;
    paletteRecord.palette = palette;
    QNP_REQUIRE(adapter.tryPublishOrdinary(paletteRecord));

    qsp::ConnectedPublicationV4 begin{};
    QNP_REQUIRE(queue.pop(begin));
    qsp::ConnectedPublicationV4 published{};
    QNP_REQUIRE(queue.pop(published));
    QNP_REQUIRE(published.kind == qsp::ConnectedPublicationKindV4::Palette);
    QNP_CHECK(published.palette.palette.side == 1u);
    QNP_CHECK(published.palette.palette.variant == 2u);
    QNP_CHECK(std::memcmp(published.palette.palette.table, palette.table,
                          qreplay::kReplayPaletteTableBytes) == 0);

    QNP_CHECK(qsp::nextItemSequenceV4(published)
              == qsp::firstItemSequenceV4(published) + 1u);
    QNP_CHECK(qsp::nextRecordCursorV4(published)
              == qsp::firstRecordCursorV4(published));
    QNP_CHECK(qsp::firstRecordCursorV4(published)
              == qsp::nextRecordCursorV4(begin));

    std::vector<uint8_t> encoded;
    QNP_REQUIRE(qsp::encodeConnectedPublicationV4(published, encoded));
    qsp::ConnectedPublicationV4 decoded{};
    QNP_REQUIRE(qsp::decodeConnectedPublicationV4(
        encoded.data(), encoded.size(), decoded) == qsp::ConnectedCodecErrorV4::None);
    QNP_CHECK(decoded == published);
    uint32_t nextSequence = 0u;
    uint32_t nextCursor = 0u;
    uint64_t nextDigest = 0u;
    QNP_CHECK(qsp::verifyConnectedPublicationChainV4(
        decoded, published.palette.chain.setStreamId,
        published.palette.chain.itemSequence,
        published.palette.chain.recordCursorBefore,
        published.palette.chain.setDigestBefore,
        nextSequence, nextCursor, nextDigest));
    QNP_CHECK(nextSequence == published.palette.chain.itemSequence + 1u);
    QNP_CHECK(nextCursor == published.palette.chain.recordCursorBefore);
    QNP_CHECK(nextDigest == published.palette.chain.setDigestAfter);

    publish::ConnectedPublisherQueueV4 idleQueue(8u);
    publish::ConnectedReplayPublisherV4 idle(idleQueue);
    QNP_REQUIRE(idle.reset(descriptor));
    QNP_CHECK(idle.tryPublishOrdinary(paletteRecord));
    qsp::ConnectedPublicationV4 nothing{};
    QNP_CHECK(!idleQueue.pop(nothing));
}

QNP_TEST(SpectatorV4ResumeAckMigrationAndRetentionAreExact) {
    BuiltSet set = threeGameSet();
    qsp::SpectatorConnectedArchiveV4 archive;
    QNP_REQUIRE(commitAll(archive, set, 5000u));
    qsp::SpectatorConnectedSourceSessionV4 session;
    QNP_REQUIRE(session.bind(&archive));

    qsp::SpectatorResumeCursorV4 cursor{};
    QNP_REQUIRE(archive.resumeCursor(0u, 1u, cursor));
    QNP_CHECK(session.attach(0xAAu, cursor, 5000u)
              == qsp::ConnectedViewerResultV4::Ok);
    qsp::ConnectedPublicationV4 publication{};
    QNP_REQUIRE(session.nextPublication(0xAAu, publication)
                == qsp::ConnectedViewerResultV4::Ok);
    qsp::SpectatorAckV4 ack{};
    ack.viewerId = 0xAAu;
    QNP_REQUIRE(archive.resumeCursor(
        qsp::nextItemSequenceV4(publication), 1u, ack.committed));
    QNP_CHECK(session.acknowledge(ack)
              == qsp::ConnectedViewerResultV4::Ok);
    QNP_CHECK(session.acknowledge(ack)
              == qsp::ConnectedViewerResultV4::ExactDuplicate);

    qsp::SpectatorResumeCursorV4 migrated = ack.committed;
    migrated.viewerGeneration = 2u;
    QNP_CHECK(session.attach(0xAAu, migrated, 6000u)
              == qsp::ConnectedViewerResultV4::Ok);
    QNP_CHECK(session.attach(0xAAu, ack.committed, 6000u)
              == qsp::ConnectedViewerResultV4::StaleGeneration);

    qsp::SpectatorSetSealAckV4 sealAck{};
    QNP_REQUIRE(archive.sealAck(sealAck, 0xAAu, 2u));
    QNP_CHECK(session.acknowledgeSeal(sealAck)
              == qsp::ConnectedViewerResultV4::Ok);
    QNP_CHECK(session.viewerCount() == 1u);
    session.enforceRetention(
        5000u + qsp::kSpectatorPostGameRetentionMsV4 - 1u);
    QNP_CHECK(session.viewerCount() == 1u);
    session.enforceRetention(
        5000u + qsp::kSpectatorPostGameRetentionMsV4);
    QNP_CHECK(session.viewerCount() == 0u);

    qsp::ConnectedSessionConfigV4 tight{};
    tight.maxUnackedItems = 1u;
    qsp::SpectatorConnectedSourceSessionV4 limited(tight);
    QNP_REQUIRE(limited.bind(&archive));
    QNP_REQUIRE(archive.resumeCursor(0u, 1u, cursor));
    QNP_REQUIRE(limited.attach(0xBBu, cursor, 5000u)
                == qsp::ConnectedViewerResultV4::Ok);
    QNP_CHECK(limited.nextPublication(0xBBu, publication)
              == qsp::ConnectedViewerResultV4::LiabilityLimit);
}

QNP_TEST(SpectatorV4PublisherReservesEndAndSealAfterOverflow) {
    BuiltSet set = threeGameSet();
    QNP_REQUIRE(set.publications.size() >= 4u);
    publish::ConnectedPublisherQueueV4 queue(4u);
    QNP_REQUIRE(queue.valid());
    QNP_CHECK(queue.publish(set.publications[0])
              == publish::ConnectedPublishResultV4::Accepted);
    QNP_CHECK(queue.publish(set.publications[1])
              == publish::ConnectedPublishResultV4::Accepted);
    QNP_CHECK(queue.publish(set.publications[1])
              == publish::ConnectedPublishResultV4::Overflow);
    QNP_CHECK(queue.publish(set.publications[2])
              == publish::ConnectedPublishResultV4::Accepted);
    QNP_CHECK(queue.publish(set.publications.back())
              == publish::ConnectedPublishResultV4::Accepted);
    const publish::ConnectedPublisherStatsV4 stats = queue.stats();
    QNP_CHECK(stats.overflowLatched);
    QNP_CHECK(stats.highWater == 4u);
    QNP_CHECK(stats.sealQueued);

    qsp::ConnectedPublicationV4 publication{};
    const qsp::ConnectedPublicationKindV4 expected[4] = {
        qsp::ConnectedPublicationKindV4::BeginGameBundle,
        qsp::ConnectedPublicationKindV4::ConfirmedRecord,
        qsp::ConnectedPublicationKindV4::EndGameBundle,
        qsp::ConnectedPublicationKindV4::SetSeal,
    };
    for (qsp::ConnectedPublicationKindV4 kind : expected) {
        QNP_REQUIRE(queue.pop(publication));
        QNP_CHECK(publication.kind == kind);
    }
    QNP_CHECK(!queue.pop(publication));
}

QNP_TEST(SpectatorV4ConnectedDescriptorBindsImmutableContentIdentity) {
    const qsp::ConnectedSetDescriptorV4 descriptor = fixture::descriptor();
    QNP_REQUIRE(qsp::connectedDescriptorMatchesCompatibilityIdentityV4(
        descriptor, fixture::compatibilityIdentity()));
    const uint64_t attestedGenesis =
        qsp::computeConnectedGenesisDigestV4(descriptor);
    QNP_REQUIRE(attestedGenesis != 0u);

    qsp::ConnectedSetDescriptorV4 wrongRoot = descriptor;
    wrongRoot.compatibilityIdentity
        .simulationContent.simulationRootSha256[0] ^= 0x5Au;
    QNP_CHECK(!qsp::connectedDescriptorMatchesCompatibilityIdentityV4(
        wrongRoot, fixture::compatibilityIdentity()));
    QNP_CHECK(qsp::computeConnectedGenesisDigestV4(wrongRoot) != 0u);
    QNP_CHECK(qsp::computeConnectedGenesisDigestV4(wrongRoot)
              != attestedGenesis);

    qsp::ConnectedSetDescriptorV4 timestampOnly = descriptor;
    timestampOnly.compatibilityIdentity.runtimeBuild.peTimestamp ^= 0xFFFFFFFFu;
    QNP_CHECK(qsp::connectedDescriptorMatchesCompatibilityIdentityV4(
        timestampOnly, fixture::compatibilityIdentity()));
    QNP_CHECK(qsp::computeConnectedGenesisDigestV4(timestampOnly)
              == attestedGenesis);

    std::vector<uint8_t> encoded;
    QNP_REQUIRE(qsp::encodeConnectedSetDescriptorV4(descriptor, encoded));
    std::vector<uint8_t> legacy(
        encoded.begin(),
        encoded.begin() + qsp::kLegacyConnectedDescriptorBytesV4);
    legacy[4] = static_cast<uint8_t>(
        qsp::kLegacyConnectedDescriptorSchemaV4);
    legacy[5] = 0u;
    const uint32_t crc = qreplay::crc32c(
        legacy.data(), legacy.size() - sizeof(uint32_t));
    const size_t crcOffset = legacy.size() - sizeof(uint32_t);
    for (unsigned shift = 0u; shift != 32u; shift += 8u) {
        legacy[crcOffset + shift / 8u] =
            static_cast<uint8_t>(crc >> shift);
    }
    qsp::ConnectedSetDescriptorV4 decodedLegacy{};
    QNP_REQUIRE(qsp::decodeConnectedSetDescriptorV4(
        legacy.data(), legacy.size(), decodedLegacy)
        == qsp::ConnectedCodecErrorV4::None);
    QNP_CHECK(!decodedLegacy.contentIdentityAttested);
    QNP_CHECK(qsp::computeConnectedGenesisDigestV4(decodedLegacy) == 0u);
    QNP_CHECK(!qsp::connectedDescriptorMatchesCompatibilityIdentityV4(
        decodedLegacy, fixture::compatibilityIdentity()));
}

QNP_TEST(SpectatorV4CoordinatorAdapterPublishesExactGameAndAck) {
    qsp::ConnectedSetDescriptorV4 descriptor = fixture::descriptor();
    qsp::ConnectedChainBuilderV4 expectedBuilder;
    QNP_REQUIRE(expectedBuilder.reset(descriptor));
    std::vector<qsp::ConnectedPublicationV4> expected;
    QNP_REQUIRE(fixture::appendCompletedGame(
        expectedBuilder, 1u, expected));
    QNP_REQUIRE(expected.size() == 3u);
    std::array<qreplay::PublisherRecordV2, 3u> source{};
    QNP_REQUIRE(coordinatorRecordsForOneGame(expected, source));

    publish::ConnectedPublisherQueueV4 queue(8u);
    publish::ConnectedReplayPublisherV4 adapter(queue);
    QNP_REQUIRE(adapter.reset(descriptor));
    QNP_REQUIRE(adapter.tryPublishOrdinary(source[0]));
    QNP_REQUIRE(adapter.tryPublishOrdinary(source[1]));
    QNP_REQUIRE(adapter.publishEndGameReserved(source[2]));

    qsp::ConnectedPublicationV4 actual{};
    for (size_t i = 0u; i < expected.size(); ++i) {
        QNP_REQUIRE(queue.pop(actual));
        QNP_CHECK(actual == expected[i]);
    }
    QNP_CHECK(!queue.pop(actual));

    qreplay::GameCommitAckV2 ack{};
    QNP_REQUIRE(adapter.popAcceptedEndAck(ack));
    QNP_CHECK(ack.identity
              == source[0].begin.start.manifest.identity);
    QNP_CHECK(ack.status
              == qreplay::GameCommitStatusV2::Committed);
    QNP_CHECK(ack.nextRecord == source[2].end.end.nextRecord);
    QNP_CHECK(ack.canonicalGameDigest
              == source[2].end.end.canonicalGameDigest);
    QNP_CHECK(!adapter.popAcceptedEndAck(ack));

    QNP_REQUIRE(adapter.publishSetSeal(
        qsp::ConnectedSetTerminalReasonV4::SourceStopped,
        source[2].end.end.postTailStateHash));
    QNP_REQUIRE(queue.pop(actual));
    QNP_CHECK(actual.kind
              == qsp::ConnectedPublicationKindV4::SetSeal);
    QNP_CHECK(actual.seal.chain.itemSequence
              == expectedBuilder.nextItemSequence());
    const publish::ConnectedReplayPublisherStatusV4 status =
        adapter.status();
    QNP_CHECK(status.sealed);
    QNP_CHECK(status.nextRecordCursor == 3u);
}

QNP_TEST(SpectatorV4SaveAdapterBackfillsThreeIndependentGames) {
    BuiltSet set = threeGameSet();
    qsp::SpectatorConnectedArchiveV4 archive;
    QNP_REQUIRE(commitAll(archive, set));
    FakeSaveSink sink;
    save::SpectatorReplaySaveAdapterV4 adapter(&sink);
    QNP_REQUIRE(adapter.attach(&archive));
    adapter.setEnabled(true);
    while (adapter.readItemSequence() < archive.nextItemSequence())
        QNP_REQUIRE(adapter.pump(2u) != 0u);

    QNP_REQUIRE(sink.saved.size() == 3u);
    QNP_REQUIRE(adapter.outcomes().size() == 3u);
    for (size_t game = 0u; game < sink.saved.size(); ++game) {
        const FakeSaveSink::Saved& saved = sink.saved[game];
        QNP_CHECK(saved.start.manifest.display.sourceKind
                  == qreplay::ReplaySourceKindV2::SpectatorSave);
        QNP_CHECK(saved.start.manifest.display.routeLabel.bytes == 0u);
        QNP_REQUIRE(saved.ticks.size() == 3u);
        for (size_t frame = 0u; frame < saved.ticks.size(); ++frame)
            QNP_CHECK(saved.ticks[frame].frame.replayFrame == frame);
        QNP_CHECK(saved.end.nextRecord == 3u);
        QNP_CHECK(adapter.outcomes()[game].kind
                  == save::SpectatorReplaySaveOutcomeKindV4::Saved);
    }
}

QNP_TEST(SpectatorV4SaveToggleMidgameUsesOnlyRetainedCompletePrefix) {
    BuiltSet set = threeGameSet(false);
    QNP_REQUIRE(set.publications.size() >= 3u);
    qsp::SpectatorConnectedArchiveV4 archive;
    QNP_REQUIRE(archive.reset(set.descriptor));
    QNP_REQUIRE(archive.commit(set.publications[0], 0u)
                == qsp::ConnectedCommitResultV4::Committed);
    QNP_REQUIRE(archive.commit(set.publications[1], 0u)
                == qsp::ConnectedCommitResultV4::Committed);

    FakeSaveSink sink;
    save::SpectatorReplaySaveAdapterV4 adapter(&sink);
    QNP_REQUIRE(adapter.attach(&archive));
    adapter.setEnabled(true);
    QNP_CHECK(adapter.state()
              == save::SpectatorReplaySaveState::
                     BackfillingRetainedRange);
    QNP_REQUIRE(adapter.pump(8u) == 2u);
    QNP_CHECK(sink.saved.empty());
    QNP_REQUIRE(archive.commit(set.publications[2], 0u)
                == qsp::ConnectedCommitResultV4::Committed);
    QNP_REQUIRE(adapter.pump(8u) == 1u);
    QNP_CHECK(sink.saved.size() == 1u);
    QNP_CHECK(sink.saved[0].ticks.size() == 3u);

    QNP_CHECK(archive.commit(set.publications[2], 0u)
              == qsp::ConnectedCommitResultV4::ExactDuplicate);
    QNP_CHECK(adapter.pump(8u) == 0u);
    QNP_CHECK(sink.saved.size() == 1u);
}

QNP_TEST(SpectatorV4SaveResourceCapFailsGameWithoutAFile) {
    BuiltSet set = threeGameSet(false);
    qsp::SpectatorConnectedArchiveV4 archive;
    QNP_REQUIRE(archive.reset(set.descriptor));
    QNP_REQUIRE(archive.commit(set.publications[0], 0u)
                == qsp::ConnectedCommitResultV4::Committed);
    FakeSaveSink sink;
    save::SpectatorReplaySaveConfigV4 config{};
    config.maxPendingItems = 1u;
    save::SpectatorReplaySaveAdapterV4 adapter(&sink, config);
    QNP_REQUIRE(adapter.attach(&archive));
    adapter.setEnabled(true);
    QNP_CHECK(adapter.pump(8u) == 0u);
    QNP_CHECK(adapter.state()
              == save::SpectatorReplaySaveState::FailedCurrentGame);
    QNP_CHECK(sink.saved.empty());
}
