#include "TestHarness.h"

#include "ReplayTestFixtures.h"

#include "client/replay/ReplayParser.h"
#include "client/replay/ReplayRecovery.h"
#include "client/replay/ReplayWriter.h"

#include "replay/ReplayCodec.h"

#include <memory>
#include <string>
#include <vector>

using namespace qreplay;
using namespace qclient::replay;
using namespace qclient::replay::test;

namespace {

ParseResult parseBytes(const std::vector<uint8_t>& bytes, ParsedReplay& out,
                       ParseMode mode = ParseMode::Verify) {
    MemoryByteSource source(bytes.data(), bytes.size());
    ParseOptions options{};
    options.mode = mode;
    return ReplayParser::parse(source, options, out);
}

struct WriteAttempt {
    bool beganOk = false;
    bool finalizeOk = false;
    WriterError error = WriterError::None;
    WriterSummary summary{};
    std::vector<uint8_t> bytes;
    std::wstring renamedTo;
    uint32_t flushes = 0u;
};

WriteAttempt runWriter(const MemoryFileSink::Faults& faults,
                       uint32_t matchCount = 1u,
                       uint32_t framesPerMatch = 6u) {
    WriteAttempt attempt{};
    MemoryFileSink* sink = new MemoryFileSink();
    sink->faults = faults;
    ReplayWriter writer;
    writer.setSink(std::unique_ptr<ReplayFileSink>(sink));

    FixtureOptions options{};
    options.matchCount = matchCount;
    options.framesPerMatch = framesPerMatch;
    const SessionManifest manifest = makeManifest(options);
    ReplayPrefix prefix{};
    prefix.sessionUuidLow = options.uuidLow;
    prefix.sessionUuidHigh = options.uuidHigh;

    WriterConfig config{};
    config.rootDirectory = L"R:\\replays";
    config.nameHint = "fault";
    config.recordedUnixSeconds = options.recordedUnixSeconds;
    config.flushEveryChunks = 0u;
    config.flushEveryMs = 0u;

    attempt.beganOk = writer.begin(config, prefix, manifest);
    if (attempt.beganOk) {
        const DeterministicStart start = makeStart(options, options.startFrame);
        uint64_t digest = options.initialDigest;
        uint32_t frame = options.startFrame;
        bool ok = writer.writeSessionStart(start);
        for (uint32_t m = 0; ok && m < matchCount; ++m) {
            MatchStartRecord match{};
            match.matchId = m + 1u;
            match.ordinal = m + 1u;
            match.firstTimelineFrame = frame;
            match.independentBootstrap = true;
            match.start = makeStart(options, frame);
            match.slots = manifest.slots;
            match.slotToSourceRoute = manifest.slotToSourceRoute;
            match.vsRow = manifest.vsRow;
            ok = writer.writeMatchStart(match);
            std::vector<ReplayFrameV1> block;
            for (uint32_t i = 0; ok && i < framesPerMatch; ++i)
                block.push_back(makeFrame(frame + i, 0x03u, digest));
            if (ok && !block.empty())
                ok = writer.writeInputBlock(block.data(), block.size());
            const uint32_t lastFrame = frame + framesPerMatch - 1u;
            MatchEndRecord end{};
            end.matchId = match.matchId;
            end.lastTimelineFrame = lastFrame;
            end.winningSideMask = 0x05u;
            end.rollingDigest = digest;
            if (ok) ok = writer.writeMatchEnd(end);
            frame = lastFrame + 1u;
        }
        if (ok) {
            SessionEndRecord sessionEnd{};
            sessionEnd.lastTimelineFrame = frame - 1u;
            sessionEnd.rollingDigest = digest;
            sessionEnd.matchCount = matchCount;
            attempt.finalizeOk = writer.finalize(sessionEnd);
        }
        if (!attempt.finalizeOk && writer.isOpen()) writer.abandon();
    }
    attempt.error = writer.error();
    attempt.summary = writer.summary();
    attempt.bytes = sink->data();
    attempt.renamedTo = sink->renamedTo();
    attempt.flushes = writer.flushCount();
    return attempt;
}

}

QNP_TEST(replay_writer_round_trips_through_the_parser) {
    const WriteAttempt attempt = runWriter(MemoryFileSink::Faults{}, 2u, 5u);
    QNP_REQUIRE(attempt.beganOk);
    QNP_CHECK(attempt.finalizeOk);
    QNP_CHECK(attempt.summary.finalized);
    QNP_CHECK(attempt.summary.matchCount == 2u);
    QNP_CHECK(attempt.summary.frameCount == 10u);
    QNP_CHECK(!attempt.renamedTo.empty());
    QNP_CHECK(attempt.summary.finalPath.size() > 5u);
    QNP_CHECK(attempt.summary.finalPath.compare(
                  attempt.summary.finalPath.size() - 5u, 5u, L".q99r") == 0);
    QNP_CHECK(attempt.summary.partPath.empty());

    ParsedReplay parsed;
    QNP_CHECK(parseBytes(attempt.bytes, parsed).ok());
    QNP_CHECK(parsed.hasFooter);
    QNP_CHECK(parsed.file.footer.fileDigest == attempt.summary.fileDigest);
}

QNP_TEST(replay_writer_never_replaces_an_existing_replay) {
    FixtureOptions options{};
    const std::wstring base = ReplayWriter::buildBaseName(
        options.recordedUnixSeconds, "fault", options.uuidHigh);

    MemoryFileSink::Faults faults{};
    faults.existingPaths.push_back(L"R:\\replays\\" + base + L".q99r");
    const WriteAttempt attempt = runWriter(faults);
    QNP_REQUIRE(attempt.beganOk);
    QNP_CHECK(attempt.finalizeOk);
    QNP_CHECK(attempt.summary.partPath.empty());
    QNP_CHECK(attempt.renamedTo != faults.existingPaths[0]);
    QNP_CHECK(!attempt.renamedTo.empty());
}

QNP_TEST(replay_writer_reports_name_exhaustion_instead_of_overwriting) {
    FixtureOptions options{};
    const std::wstring base = ReplayWriter::buildBaseName(
        options.recordedUnixSeconds, "fault", options.uuidHigh);
    MemoryFileSink::Faults faults{};
    faults.existingPaths.push_back(L"R:\\replays\\" + base + L".q99r");
    for (uint32_t i = 2u; i <= 8u; ++i) {
        faults.existingPaths.push_back(L"R:\\replays\\" + base + L"_"
                                       + std::to_wstring(i) + L".q99r");
    }

    MemoryFileSink* sink = new MemoryFileSink();
    sink->faults = faults;
    ReplayWriter writer;
    writer.setSink(std::unique_ptr<ReplayFileSink>(sink));
    WriterConfig config{};
    config.rootDirectory = L"R:\\replays";
    config.nameHint = "fault";
    config.recordedUnixSeconds = options.recordedUnixSeconds;
    config.maxNameAttempts = 4u;
    ReplayPrefix prefix{};
    prefix.sessionUuidLow = options.uuidLow;
    prefix.sessionUuidHigh = options.uuidHigh;
    QNP_REQUIRE(writer.begin(config, prefix, makeManifest(options)));
    QNP_CHECK(!writer.finalize(SessionEndRecord{}));
    QNP_CHECK(writer.error() == WriterError::NameExhausted);
    QNP_CHECK(sink->data().empty());
}

QNP_TEST(replay_writer_simultaneous_recorders_do_not_collide) {
    FixtureOptions options{};
    const std::wstring base = ReplayWriter::buildBaseName(
        options.recordedUnixSeconds, "fault", options.uuidHigh);
    const WriteAttempt first = runWriter(MemoryFileSink::Faults{});
    QNP_REQUIRE(first.beganOk);

    MemoryFileSink::Faults faults{};
    faults.existingPaths.push_back(first.renamedTo);
    const WriteAttempt second = runWriter(faults);
    QNP_REQUIRE(second.beganOk);
    QNP_CHECK(second.finalizeOk);
    QNP_CHECK(second.summary.partPath.empty());
    QNP_CHECK(second.renamedTo != first.renamedTo);
    QNP_CHECK(base.size() > 0u);
}

QNP_TEST(replay_writer_flushes_at_every_match_terminal) {
    const WriteAttempt attempt = runWriter(MemoryFileSink::Faults{}, 3u, 4u);
    QNP_REQUIRE(attempt.finalizeOk);
    QNP_CHECK(attempt.flushes >= 5u);
}

QNP_TEST(replay_writer_reports_access_denied_on_create) {
    MemoryFileSink::Faults faults{};
    faults.failCreate = true;
    faults.createError = WriterError::AccessDenied;
    const WriteAttempt attempt = runWriter(faults);
    QNP_CHECK(attempt.beganOk);
    QNP_CHECK(!attempt.finalizeOk);
    QNP_CHECK(attempt.error == WriterError::AccessDenied);
    QNP_CHECK(attempt.bytes.empty());
}

QNP_TEST(replay_writer_survives_disk_full_at_every_prefix_length) {
    const WriteAttempt whole = runWriter(MemoryFileSink::Faults{}, 1u, 6u);
    QNP_REQUIRE(whole.finalizeOk);

    for (uint64_t limit = 1u; limit < whole.bytes.size(); limit += 17u) {
        MemoryFileSink::Faults faults{};
        faults.failAppendAfterBytes = limit;
        faults.appendError = WriterError::DiskFull;
        const WriteAttempt attempt = runWriter(faults, 1u, 6u);
        QNP_CHECK(!attempt.finalizeOk);
        QNP_CHECK(!attempt.summary.finalized);
        ParsedReplay parsed;
        parseBytes(attempt.bytes, parsed);
        QNP_CHECK(!parsed.hasFooter);
        if (parsed.hasFooter) break;
    }
}

QNP_TEST(replay_writer_reports_flush_failure_without_finalizing) {
    MemoryFileSink::Faults faults{};
    faults.failFlush = true;
    const WriteAttempt attempt = runWriter(faults);
    QNP_CHECK(!attempt.finalizeOk);
    QNP_CHECK(attempt.error == WriterError::FlushFailed);
    QNP_CHECK(!attempt.summary.finalized);
}

QNP_TEST(replay_writer_discards_the_recording_when_publication_fails) {
    MemoryFileSink::Faults faults{};
    faults.failRename = true;
    const WriteAttempt attempt = runWriter(faults, 1u, 6u);
    QNP_REQUIRE(attempt.beganOk);
    QNP_CHECK(!attempt.finalizeOk);
    QNP_CHECK(attempt.error == WriterError::RenameFailed);
    QNP_CHECK(!attempt.summary.finalized);
    QNP_CHECK(attempt.summary.finalPath.empty());
    QNP_CHECK(attempt.summary.partPath.empty());
    QNP_CHECK(attempt.bytes.empty());
}

QNP_TEST(replay_writer_refuses_records_out_of_order) {
    MemoryFileSink* sink = new MemoryFileSink();
    ReplayWriter writer;
    writer.setSink(std::unique_ptr<ReplayFileSink>(sink));
    FixtureOptions options{};
    WriterConfig config{};
    config.rootDirectory = L"R:\\replays";
    config.recordedUnixSeconds = options.recordedUnixSeconds;
    ReplayPrefix prefix{};
    QNP_REQUIRE(writer.begin(config, prefix, makeManifest(options)));

    MatchStartRecord match{};
    match.matchId = 1u;
    match.ordinal = 1u;
    match.firstTimelineFrame = 0u;
    match.independentBootstrap = true;
    match.start = makeStart(options, 0u);
    QNP_CHECK(!writer.writeMatchStart(match));
    QNP_CHECK(writer.error() == WriterError::WrongOrder);
}

QNP_TEST(replay_writer_refuses_an_invalid_action_mask) {
    MemoryFileSink* sink = new MemoryFileSink();
    ReplayWriter writer;
    writer.setSink(std::unique_ptr<ReplayFileSink>(sink));
    FixtureOptions options{};
    WriterConfig config{};
    config.rootDirectory = L"R:\\replays";
    config.recordedUnixSeconds = options.recordedUnixSeconds;
    ReplayPrefix prefix{};
    QNP_REQUIRE(writer.begin(config, prefix, makeManifest(options)));
    QNP_REQUIRE(writer.writeSessionStart(makeStart(options, 0u)));

    MatchStartRecord match{};
    match.matchId = 1u;
    match.ordinal = 1u;
    match.firstTimelineFrame = 0u;
    match.independentBootstrap = true;
    match.start = makeStart(options, 0u);
    match.slots = makeManifest(options).slots;
    QNP_REQUIRE(writer.writeMatchStart(match));

    ReplayFrameV1 frame{};
    frame.timelineFrame = 0u;
    frame.activeLaneMask = 0x01u;
    frame.actions[0] = 0x1000u;
    QNP_CHECK(!writer.writeInputBlock(&frame, 1u));
    QNP_CHECK(writer.error() == WriterError::InvalidRecord);

    ReplayFrameV1 other{};
    other.timelineFrame = 0u;
    other.activeLaneMask = 0x01u;
    other.actions[0] = 0x0101u;
    other.actions[2] = 0x0001u;
    ReplayWriter second;
    MemoryFileSink* secondSink = new MemoryFileSink();
    second.setSink(std::unique_ptr<ReplayFileSink>(secondSink));
    QNP_REQUIRE(second.begin(config, prefix, makeManifest(options)));
    QNP_REQUIRE(second.writeSessionStart(makeStart(options, 0u)));
    QNP_REQUIRE(second.writeMatchStart(match));
    QNP_CHECK(!second.writeInputBlock(&other, 1u));
}

QNP_TEST(replay_writer_terminal_without_footer_stays_incomplete) {
    MemoryFileSink* sink = new MemoryFileSink();
    ReplayWriter writer;
    writer.setSink(std::unique_ptr<ReplayFileSink>(sink));
    FixtureOptions options{};
    WriterConfig config{};
    config.rootDirectory = L"R:\\replays";
    config.recordedUnixSeconds = options.recordedUnixSeconds;
    ReplayPrefix prefix{};
    QNP_REQUIRE(writer.begin(config, prefix, makeManifest(options)));
    QNP_REQUIRE(writer.writeSessionStart(makeStart(options, 0u)));

    SessionEndRecord terminal{};
    terminal.lastTimelineFrame = 0u;
    terminal.reason = TerminalReason::RecorderOverflow;
    QNP_CHECK(writer.writeTerminalAndAbandon(terminal));
    QNP_CHECK(!writer.isFinalized());
    QNP_CHECK(sink->renamedTo().empty());
    QNP_CHECK(sink->data().empty());
}

QNP_TEST(replay_recovery_handles_power_loss_at_every_chunk_boundary) {
    FixtureOptions options{};
    options.matchCount = 2u;
    options.framesPerMatch = 5u;
    options.roundsPerMatch = 1u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    QNP_REQUIRE(fixture.chunkOffsets.size() >= 4u);

    for (size_t i = 0; i < fixture.chunkOffsets.size(); ++i) {
        const uint64_t cut = fixture.chunkOffsets[i];
        std::vector<uint8_t> bytes(fixture.bytes.begin(),
                                   fixture.bytes.begin()
                                       + static_cast<ptrdiff_t>(cut));
        MemoryByteSource source(bytes.data(), bytes.size());
        const RecoveryPlan plan = ReplayRecovery::plan(source);
        QNP_CHECK(plan.prefixValid);
        QNP_CHECK(plan.validBytes == cut);
        QNP_CHECK(plan.hasManifest);
        if (plan.retainedFrames != 0u)
            QNP_CHECK(plan.classification == RecoveryClass::Playable);
        else
            QNP_CHECK(plan.classification != RecoveryClass::Playable);
    }
}

QNP_TEST(replay_recovery_handles_a_cut_inside_every_chunk) {
    FixtureOptions options{};
    options.framesPerMatch = 3u;
    options.roundsPerMatch = 1u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    uint64_t previousValid = 0u;
    for (size_t cut = kPrefixBytes; cut < fixture.bytes.size(); ++cut) {
        std::vector<uint8_t> bytes(fixture.bytes.begin(),
                                   fixture.bytes.begin()
                                       + static_cast<ptrdiff_t>(cut));
        MemoryByteSource source(bytes.data(), bytes.size());
        const RecoveryPlan plan = ReplayRecovery::plan(source);
        QNP_CHECK(plan.validBytes <= cut);
        QNP_CHECK(plan.validBytes >= previousValid);
        previousValid = plan.validBytes;
    }
}

QNP_TEST(replay_recovery_emits_a_playable_new_file_and_never_touches_the_source) {
    FixtureOptions options{};
    options.matchCount = 2u;
    options.framesPerMatch = 6u;
    options.finalize = false;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    const std::vector<uint8_t> sourceBefore = fixture.bytes;
    MemoryByteSource source(fixture.bytes.data(), fixture.bytes.size());
    const RecoveryPlan plan = ReplayRecovery::plan(source);
    QNP_CHECK(plan.prefixValid);
    QNP_CHECK(plan.classification == RecoveryClass::Playable);
    QNP_CHECK(plan.hasSessionStart);
    QNP_CHECK(plan.startedMatches == 2u);
    QNP_CHECK(plan.completedMatches == 2u);
    QNP_CHECK(!plan.hasFooter);

    MemoryFileSink* sink = new MemoryFileSink();
    ReplayWriter writer;
    writer.setSink(std::unique_ptr<ReplayFileSink>(sink));
    RecoveryOutcome outcome{};
    QNP_REQUIRE(ReplayRecovery::execute(source, plan, L"R:\\replays",
                                        L"broken.q99r.part", outcome, &writer));
    QNP_CHECK(outcome.ok);
    QNP_CHECK(outcome.summary.finalized);
    QNP_CHECK(outcome.provenance.sourceFileName == "broken.q99r.part");
    QNP_CHECK(outcome.provenance.salvagedBytes == plan.validBytes);

    QNP_CHECK(fixture.bytes == sourceBefore);

    ParsedReplay parsed;
    QNP_CHECK(parseBytes(sink->data(), parsed, ParseMode::Full).ok());
    QNP_CHECK(parsed.hasFooter);
    QNP_CHECK(parsed.file.manifest.display.sourceKind
              == ReplaySourceKind::RecoveredPrefix);
    QNP_CHECK(parsed.file.hasSessionEnd);
    QNP_CHECK(parsed.file.sessionEnd.reason == TerminalReason::RecoveredPrefix);
    QNP_CHECK(parsed.matches.size() == 2u);
    QNP_CHECK(parsed.totalFrames == 12u);
    QNP_CHECK(parsed.file.prefix.sessionUuidLow != options.uuidLow);
    QNP_CHECK(outcome.provenance.sourceSessionUuidLow == options.uuidLow);

    bool sawTerminal = false;
    for (const ReplayEvent& event : parsed.file.events) {
        if (event.kind == EventKind::SegmentTerminal
            && event.terminalReason == TerminalReason::RecoveredPrefix) {
            sawTerminal = true;
        }
    }
    QNP_CHECK(sawTerminal);
}

QNP_TEST(replay_recovery_uuid_is_deterministic) {
    std::array<uint8_t, 32> digest = digestFromSeed(0x1234u);
    uint64_t lowA = 0u;
    uint64_t highA = 0u;
    uint64_t lowB = 0u;
    uint64_t highB = 0u;
    ReplayRecovery::deriveRecoveredUuid(digest, lowA, highA);
    ReplayRecovery::deriveRecoveredUuid(digest, lowB, highB);
    QNP_CHECK(lowA == lowB && highA == highB);
    QNP_CHECK(lowA != 0u || highA != 0u);

    digest[0] ^= 0x01u;
    uint64_t lowC = 0u;
    uint64_t highC = 0u;
    ReplayRecovery::deriveRecoveredUuid(digest, lowC, highC);
    QNP_CHECK(lowC != lowA || highC != highA);
}

QNP_TEST(replay_recovery_closes_a_match_left_open) {
    FixtureOptions options{};
    options.matchCount = 1u;
    options.framesPerMatch = 6u;
    options.roundsPerMatch = 0u;
    options.finalize = false;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    QNP_REQUIRE(fixture.chunkOffsets.size() >= 4u);

    const uint64_t cut = fixture.chunkOffsets[3];
    std::vector<uint8_t> bytes(fixture.bytes.begin(),
                               fixture.bytes.begin()
                                   + static_cast<ptrdiff_t>(cut));
    MemoryByteSource source(bytes.data(), bytes.size());
    const RecoveryPlan plan = ReplayRecovery::plan(source);
    QNP_CHECK(plan.lastMatchOpen);
    QNP_CHECK(plan.classification == RecoveryClass::Playable);

    MemoryFileSink* sink = new MemoryFileSink();
    ReplayWriter writer;
    writer.setSink(std::unique_ptr<ReplayFileSink>(sink));
    RecoveryOutcome outcome{};
    QNP_REQUIRE(ReplayRecovery::execute(source, plan, L"R:\\replays",
                                        L"cut.q99r.part", outcome, &writer));
    ParsedReplay parsed;
    QNP_CHECK(parseBytes(sink->data(), parsed).ok());
    QNP_REQUIRE(parsed.matches.size() == 1u);
    QNP_CHECK(parsed.matches[0].hasEnd);
    QNP_CHECK(parsed.matches[0].endReason == TerminalReason::RecoveredPrefix);
    QNP_CHECK(parsed.matches[0].winningSideMask == 0u);
}

QNP_TEST(replay_recovery_classifies_metadata_only_and_nothing) {
    FixtureOptions options{};
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    QNP_REQUIRE(!fixture.chunkOffsets.empty());

    {
        std::vector<uint8_t> bytes(
            fixture.bytes.begin(),
            fixture.bytes.begin()
                + static_cast<ptrdiff_t>(fixture.chunkOffsets[0]));
        MemoryByteSource source(bytes.data(), bytes.size());
        const RecoveryPlan plan = ReplayRecovery::plan(source);
        QNP_CHECK(plan.hasManifest);
        QNP_CHECK(plan.classification == RecoveryClass::MetadataOnly);
    }
    {
        std::vector<uint8_t> bytes(fixture.bytes.begin(),
                                   fixture.bytes.begin() + kPrefixBytes);
        MemoryByteSource source(bytes.data(), bytes.size());
        const RecoveryPlan plan = ReplayRecovery::plan(source);
        QNP_CHECK(plan.prefixValid);
        QNP_CHECK(!plan.hasManifest);
        QNP_CHECK(plan.classification == RecoveryClass::Nothing);
    }
    {
        std::vector<uint8_t> bytes = fixture.bytes;
        bytes[0] ^= 0xFFu;
        MemoryByteSource source(bytes.data(), bytes.size());
        const RecoveryPlan plan = ReplayRecovery::plan(source);
        QNP_CHECK(!plan.prefixValid);
        QNP_CHECK(plan.classification == RecoveryClass::Nothing);

        RecoveryOutcome outcome{};
        QNP_CHECK(!ReplayRecovery::execute(source, plan, L"R:\\replays",
                                           L"x.part", outcome));
        QNP_CHECK(!outcome.ok);
    }
}

QNP_TEST(replay_recovery_stops_at_the_first_bad_chunk) {
    FixtureOptions options{};
    options.matchCount = 2u;
    options.framesPerMatch = 4u;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    QNP_REQUIRE(fixture.chunkOffsets.size() >= 5u);

    const uint64_t chunkStart = fixture.chunkOffsets[3];
    std::vector<uint8_t> bytes = fixture.bytes;
    bytes[static_cast<size_t>(chunkStart) + kChunkHeaderBytes + 2u] ^= 0x40u;

    MemoryByteSource source(bytes.data(), bytes.size());
    const RecoveryPlan plan = ReplayRecovery::plan(source);
    QNP_CHECK(plan.validBytes == chunkStart);
    QNP_CHECK(plan.stop.status == ParseStatus::BadChunkPayloadCrc);
    QNP_CHECK(plan.hasManifest);
    QNP_CHECK(plan.hasSessionStart);
}

QNP_TEST(replay_recovery_reports_a_writer_failure_without_claiming_success) {
    FixtureOptions options{};
    options.finalize = false;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    MemoryByteSource source(fixture.bytes.data(), fixture.bytes.size());
    const RecoveryPlan plan = ReplayRecovery::plan(source);

    MemoryFileSink* sink = new MemoryFileSink();
    sink->faults.failAppendAfterBytes = 200u;
    ReplayWriter writer;
    writer.setSink(std::unique_ptr<ReplayFileSink>(sink));
    RecoveryOutcome outcome{};
    QNP_CHECK(!ReplayRecovery::execute(source, plan, L"R:\\replays",
                                       L"broken.part", outcome, &writer));
    QNP_CHECK(!outcome.ok);
    QNP_CHECK(!outcome.summary.finalized);
    QNP_CHECK(sink->renamedTo().empty());
}
