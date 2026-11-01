#include "TestHarness.h"
#include "ReplayTestFixtures.h"
#include "client/replay/ReplayBattleLogFile.h"

#include "client/replay/ReplayParserV2.h"
#include "client/replay/ReplayRecorderHost.h"
#include "client/replay/ReplayRecoveryV2.h"
#include "client/replay/ReplayWriterV2.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace {

using namespace qreplay;
using namespace qclient::replay;
namespace wire3 = qreplay::ipc::v3;

struct GameFixtureV2 {
    ConfirmedGameStartV2 start{};
    std::vector<CanonicalTickBundleV2> ticks;
    ConfirmedGameEndV2 end{};
};

ReplayCompatibilityIdentityEnvelopeV3 attestedIdentity() {
    qnetplay::compat::CompatibilityIdentity identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = 10u;
    (void)qnetplay::compat::setProfileId(identity, "qoh99-se1126");
    for (size_t index = 0u;
         index < qnetplay::compat::kCompatibilitySha256Bytes; ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(0x10u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(0x50u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<uint8_t>(0x90u + index);
    }
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.stateSchemaId = 0x1111111111111111ull;
    identity.modSimulationId = 0x2222222222222222ull;
    identity.timingProfileId = 0x3333333333333333ull;
    ReplayCompatibilityIdentityEnvelopeV3 envelope{};
    envelope.status = ReplayCompatibilityIdentityStatusV3::Attested;
    envelope.identity = identity;
    return envelope;
}

ConfirmedGameEventV2 event(
    const GameEventKindV2 kind, const GameEventPhaseV2 phase,
    const uint32_t frame, const uint32_t sequence,
    const uint64_t proof) {
    ConfirmedGameEventV2 value{};
    value.kind = kind;
    value.phase = phase;
    value.replayFrame = frame;
    value.eventSequence = sequence;
    value.stateProof = proof;
    value.transitionSiteId = kind == GameEventKindV2::GameStarted
        ? 0u : 0x100u + static_cast<uint16_t>(kind);
    if (kind == GameEventKindV2::RoundStarted) {
        value.roundOrdinal = 1u;
    } else if (kind == GameEventKindV2::RoundEnded
               || kind == GameEventKindV2::GameResult) {
        value.roundOrdinal = 1u;
        value.winningSideMask = 1u;
        value.roundTallies = {{2u, 1u}};
        value.winnerSlot = 0;
    }
    return value;
}

CanonicalTickBundleV2 tick(
    const uint32_t source, const uint32_t replay,
    const GameLifecycleV2& before, const GameLifecycleV2& after,
    const uint64_t pre, const uint64_t post) {
    CanonicalTickBundleV2 value{};
    value.frame.sourceFrame = source;
    value.frame.replayFrame = replay;
    value.frame.core.inputDomain = InputDomain::LogicalSourceV1;
    value.frame.core.activeLaneMask = 3u;
    value.frame.core.actions[0] =
        static_cast<uint16_t>(replay & 1u ? 0x0100u : 0x0001u);
    value.frame.core.actions[1] =
        static_cast<uint16_t>(replay & 1u ? 0x0002u : 0x0200u);
    value.frame.core.before = before;
    value.frame.core.after = after;
    value.frame.core.dispatcherResult = 1;
    value.frame.core.preStateHash = pre;
    value.frame.core.postStateHash = post;
    return value;
}

GameFixtureV2 fixture(
    const uint64_t uuidLow = 0x1111222233334444ull,
    const uint64_t uuidHigh = 0x5555666677778888ull,
    const uint32_t generation = 7u) {
    GameFixtureV2 game{};
    GameManifestV2& manifest = game.start.manifest;
    manifest.identity.gameUuidLow = uuidLow;
    manifest.identity.gameUuidHigh = uuidHigh;
    manifest.identity.gameGeneration = generation;
    manifest.contract.executableSha256.fill(0x11u);
    manifest.contract.contentManifestRoot.fill(0x22u);
    manifest.contract.nativeBuildIdentity = 1u;
    manifest.contract.runtimeSimulationId = 2u;
    manifest.contract.stateRegionSchemaFingerprint = 3u;
    manifest.contract.stateHashProjectionId = 4u;
    manifest.contract.stateHashAlgorithmId = 5u;
    manifest.contract.bootstrapSchemaId = 6u;
    manifest.contract.eventSchemaId = 7u;
    manifest.contract.characterCatalogId = 8u;
    manifest.contract.terminalResultSchemaId = 9u;
    manifest.contract.compactActionDomainVersion = 1u;
    manifest.vsRow = 1u;
    manifest.activeLaneMask = 3u;
    manifest.activeFighterMask = 3u;
    manifest.cpuContract = CpuContract::None;
    manifest.lanes[0].active = true;
    manifest.lanes[1].active = true;
    if (!setReplayTextV2(manifest.lanes[0].name, "P1")
        || !setReplayTextV2(manifest.lanes[1].name, "P2")) {
        return GameFixtureV2{};
    }
    manifest.slots[0].ownership = SlotOwnership::RecordedHuman;
    manifest.slots[0].side = 0u;
    manifest.slots[0].identity.nativeCharacterId = 1u;
    manifest.slots[1].ownership = SlotOwnership::RecordedHuman;
    manifest.slots[1].side = 1u;
    manifest.slots[1].identity.nativeCharacterId = 2u;
    manifest.slots[2].side = 0u;
    manifest.slots[3].side = 1u;
    manifest.routeEpoch = 1u;
    manifest.rosterEpoch = 2u;
    manifest.layoutGeneration = 3u;
    manifest.stage.nativeStageId = 4u;
    manifest.stage.verified = true;
    manifest.stage.catalogEntryId = 5u;
    manifest.display.sourceKind = ReplaySourceKindV2::NetplayPlayer;
    manifest.display.recordedUnixSeconds = 1785456000;
    game.start.start.configuration.roundsToWinMinusOne = 1u;
    game.start.start.rng.gameplay = 10u;
    game.start.start.rng.cosmetic = 11u;
    game.start.start.rng.selector = 12u;
    game.start.start.bootstrapFingerprint = 13u;
    game.start.start.frameZeroPreStateHash = 0x1000u;
    game.start.start.initialRollingDigest =
        initialReplayDigestV2(game.start);

    GameLifecycleV2 zeroBefore{};
    zeroBefore.scene = 2u;
    zeroBefore.requestedScene = 2u;
    zeroBefore.matchSub = 0;
    zeroBefore.winnerSlot = -1;
    zeroBefore.routeEpoch = manifest.routeEpoch;
    zeroBefore.rosterEpoch = manifest.rosterEpoch;
    zeroBefore.layoutGeneration = manifest.layoutGeneration;
    GameLifecycleV2 zeroAfter = zeroBefore;
    zeroAfter.matchSub = 1;
    CanonicalTickBundleV2 zero = tick(
        100u, 0u, zeroBefore, zeroAfter, 0x1000u, 0x2000u);
    uint64_t rolling = game.start.start.initialRollingDigest;
    zero.preEvents.count = 2u;
    zero.preEvents.events[0] = event(
        GameEventKindV2::GameStarted, GameEventPhaseV2::PreFrame,
        0u, 0u, 0x1000u);
    zero.preEvents.events[0].rollingDigest =
        advanceReplayDigestV2(rolling, zero.preEvents.events[0]);
    rolling = zero.preEvents.events[0].rollingDigest;
    zero.preEvents.events[1] = event(
        GameEventKindV2::RoundStarted, GameEventPhaseV2::PreFrame,
        0u, 1u, 0x1000u);
    zero.preEvents.events[1].rollingDigest =
        advanceReplayDigestV2(rolling, zero.preEvents.events[1]);
    rolling = zero.preEvents.events[1].rollingDigest;
    zero.frame.rollingDigest = advanceReplayDigestV2(rolling, zero.frame);
    rolling = zero.frame.rollingDigest;
    game.ticks.push_back(zero);

    GameLifecycleV2 transitionAfter = zeroAfter;
    transitionAfter.matchSub = 2;
    transitionAfter.roundCounter = 5u;
    transitionAfter.resultCounter = 0u;
    transitionAfter.winnerSlot = 0;
    transitionAfter.roundTallies = {{2u, 1u}};
    CanonicalTickBundleV2 transition = tick(
        101u, 1u, zeroAfter, transitionAfter, 0x2000u, 0x3000u);
    transition.frame.rollingDigest =
        advanceReplayDigestV2(rolling, transition.frame);
    rolling = transition.frame.rollingDigest;
    transition.postEvents.count = 1u;
    transition.postEvents.events[0] = event(
        GameEventKindV2::RoundEnded, GameEventPhaseV2::PostFrame,
        1u, 2u, 0x3000u);
    transition.postEvents.events[0].rollingDigest =
        advanceReplayDigestV2(rolling, transition.postEvents.events[0]);
    rolling = transition.postEvents.events[0].rollingDigest;
    game.ticks.push_back(transition);

    GameLifecycleV2 winscreenAfter = transitionAfter;
    winscreenAfter.resultCounter = 1u;
    CanonicalTickBundleV2 winscreen = tick(
        102u, 2u, transitionAfter, winscreenAfter, 0x3000u, 0x4000u);
    winscreen.frame.rollingDigest =
        advanceReplayDigestV2(rolling, winscreen.frame);
    rolling = winscreen.frame.rollingDigest;
    winscreen.postEvents.count = 1u;
    winscreen.postEvents.events[0] = event(
        GameEventKindV2::GameResult, GameEventPhaseV2::PostFrame,
        2u, 3u, 0x4000u);
    winscreen.postEvents.events[0].rollingDigest =
        advanceReplayDigestV2(rolling, winscreen.postEvents.events[0]);
    rolling = winscreen.postEvents.events[0].rollingDigest;
    game.ticks.push_back(winscreen);

    game.end.reason = TerminalReason::Completed;
    game.end.nextRecord = 3u;
    game.end.winningSideMask = 1u;
    game.end.roundTallies = {{2u, 1u}};
    game.end.winnerSlot = 0;
    game.end.postTailStateHash = 0x4000u;
    game.end.finalRollingDigest = rolling;
    CanonicalGameDigestV2 canonical;
    if (!canonical.begin(game.start)) {
        return GameFixtureV2{};
    }
    for (const CanonicalTickBundleV2& value : game.ticks) {
        for (size_t i = 0u; i < value.preEvents.count; ++i)
            if (!canonical.append(value.preEvents.events[i])) {
                return GameFixtureV2{};
            }
        if (!canonical.append(value.frame)) {
            return GameFixtureV2{};
        }
        for (size_t i = 0u; i < value.postEvents.count; ++i)
            if (!canonical.append(value.postEvents.events[i])) {
                return GameFixtureV2{};
            }
    }
    if (!canonical.finish(game.end, game.end.canonicalGameDigest)) {
        return GameFixtureV2{};
    }
    return game;
}

WriterConfig memoryConfig() {
    WriterConfig config{};
    config.rootDirectory = L"C:\\q99r-test";
    config.nameHint = "storage-v2";
    config.recordedUnixSeconds = 1785456000;
    config.flushEveryChunks = 1024u;
    config.flushEveryMs = 0u;
    return config;
}

bool record(
    ReplayWriterV2& writer, const GameFixtureV2& game,
    const bool complete = true) {
    BeginGameBundleV2 begin{};
    begin.start = game.start;
    begin.frameZero = game.ticks[0];
    if (!writer.begin(memoryConfig(), begin)) return false;
    if (!writer.append(game.ticks[1])) return false;
    if (!complete) return true;
    EndGameBundleV2 end{};
    end.containsFinalFrame = true;
    end.finalFrame = game.ticks[2];
    end.end = game.end;
    return writer.finalize(end);
}

wire3::Frame inbound(
    const wire3::MessageType type, const uint32_t sequence,
    const uint32_t launchGeneration, const uint64_t nonce,
    const GameIdentityV2& game, const std::vector<uint8_t>& payload,
    const uint32_t param0 = 0u, const uint32_t param1 = 0u) {
    wire3::Frame frame{};
    frame.channel = wire3::Channel::Recorder;
    frame.type = type;
    frame.launchGeneration = launchGeneration;
    frame.launchNonce = nonce;
    frame.game = game;
    frame.sequence = sequence;
    frame.param0 = param0;
    frame.param1 = param1;
    frame.payload = payload;
    return frame;
}

}

QNP_TEST(ReplayWriterV2SealsAndParserRebuildsExactCompositeTimeline) {
    const GameFixtureV2 game = fixture();
    ReplayWriterV2 writer;
    std::unique_ptr<MemoryFileSink> sink(new MemoryFileSink());
    MemoryFileSink* bytes = sink.get();
    writer.setSink(std::move(sink));
    QNP_REQUIRE(record(writer, game));
    QNP_CHECK(writer.summary().finalized);
    QNP_CHECK(writer.summary().frameCount == 3u);
    QNP_CHECK(writer.summary().canonicalGameDigest
              == game.end.canonicalGameDigest);

    MemoryByteSource source(bytes->data());
    ParsedReplayV2 parsed{};
    ParseOptionsV2 options{};
    const ParseResult result =
        ReplayParserV2::parse(source, options, parsed);
    QNP_REQUIRE(result.ok());
    QNP_CHECK(parsed.classification == ReplayClassificationV2::Sealed);
    QNP_CHECK(parsed.fileDigestVerified);
    QNP_REQUIRE(parsed.ticks.size() == game.ticks.size());
    for (size_t i = 0u; i < parsed.ticks.size(); ++i) {
        QNP_CHECK(parsed.ticks[i].preEvents == game.ticks[i].preEvents);
        QNP_CHECK(parsed.ticks[i].frame.replayFrame
                  == game.ticks[i].frame.replayFrame);
        QNP_CHECK(parsed.ticks[i].frame.sourceFrame
                  == parsed.ticks[i].frame.replayFrame);
        QNP_CHECK(parsed.ticks[i].frame.core == game.ticks[i].frame.core);
        QNP_CHECK(parsed.ticks[i].frame.rollingDigest
                  == game.ticks[i].frame.rollingDigest);
        QNP_CHECK(parsed.ticks[i].postEvents == game.ticks[i].postEvents);
    }
    QNP_CHECK(parsed.end == game.end);
    QNP_CHECK(parsed.compatibilityIdentity.legacyUnattested());
    QNP_CHECK(!parsed.hasCompatibilityIdentity);
}

QNP_TEST(ReplayWriterV2RoundTripsPalettesAndOlderFilesReportNone) {
    const GameFixtureV2 game = fixture();

    ReplayPaletteV2 first{};
    first.side = 0u;
    first.variant = 2u;
    first.present = 1u;
    ReplayPaletteV2 second{};
    second.side = 1u;
    second.variant = 0u;
    second.present = 1u;
    for (size_t variant = 0; variant < 4u; ++variant) {
        for (size_t colour = 1; colour < 16u; ++colour) {
            const size_t at = variant * 64u + colour * 4u;
            first.table[at + 0] = static_cast<uint8_t>(colour * 3u);
            first.table[at + 1] = static_cast<uint8_t>(colour * 5u);
            first.table[at + 2] = static_cast<uint8_t>(colour * 7u);
            second.table[at + 0] = static_cast<uint8_t>(255u - colour * 3u);
            second.table[at + 1] = static_cast<uint8_t>(255u - colour * 5u);
            second.table[at + 2] = static_cast<uint8_t>(255u - colour * 7u);
        }
    }

    ReplayWriterV2 writer;
    std::unique_ptr<MemoryFileSink> sink(new MemoryFileSink());
    MemoryFileSink* bytes = sink.get();
    writer.setSink(std::move(sink));
    BeginGameBundleV2 begin{};
    begin.start = game.start;
    begin.frameZero = game.ticks[0];
    QNP_REQUIRE(writer.begin(memoryConfig(), begin));
    QNP_REQUIRE(writer.writePalette(first));
    QNP_REQUIRE(writer.writePalette(second));
    QNP_REQUIRE(writer.writePalette(first));
    QNP_REQUIRE(writer.append(game.ticks[1]));
    EndGameBundleV2 end{};
    end.containsFinalFrame = true;
    end.finalFrame = game.ticks[2];
    end.end = game.end;
    QNP_REQUIRE(writer.finalize(end));

    MemoryByteSource source(bytes->data());
    ParsedReplayV2 parsed{};
    ParseOptionsV2 options{};
    QNP_REQUIRE(ReplayParserV2::parse(source, options, parsed).ok());
    QNP_CHECK(parsed.classification == ReplayClassificationV2::Sealed);
    QNP_CHECK(parsed.fileDigestVerified);
    QNP_REQUIRE(parsed.ticks.size() == game.ticks.size());
    QNP_CHECK(parsed.end == game.end);
    QNP_REQUIRE(parsed.palettes.size() == 2u);
    for (const ReplayPaletteV2& got : parsed.palettes) {
        const ReplayPaletteV2& want = got.side == 0u ? first : second;
        QNP_CHECK(got.variant == want.variant);
        QNP_CHECK(got.present == 1u);
        QNP_CHECK(std::memcmp(got.table, want.table,
                              kReplayPaletteTableBytes) == 0);
    }

    ReplayWriterV2 plain;
    std::unique_ptr<MemoryFileSink> plainSink(new MemoryFileSink());
    MemoryFileSink* plainBytes = plainSink.get();
    plain.setSink(std::move(plainSink));
    QNP_REQUIRE(record(plain, game));

    MemoryByteSource plainSource(plainBytes->data());
    ParsedReplayV2 plainParsed{};
    QNP_REQUIRE(
        ReplayParserV2::parse(plainSource, options, plainParsed).ok());
    QNP_CHECK(plainParsed.classification == ReplayClassificationV2::Sealed);
    QNP_CHECK(plainParsed.fileDigestVerified);
    QNP_CHECK(plainParsed.palettes.empty());
    QNP_REQUIRE(plainParsed.ticks.size() == game.ticks.size());
    QNP_CHECK(plainParsed.end == game.end);
}

QNP_TEST(ReplayWriterV2BindsAnAttestedCompatibilityIdentityToItsGameEpoch) {
    const GameFixtureV2 game = fixture();
    const ReplayCompatibilityIdentityEnvelopeV3 identity =
        attestedIdentity();
    ReplayWriterV2 writer;
    std::unique_ptr<MemoryFileSink> sink(new MemoryFileSink());
    MemoryFileSink* bytes = sink.get();
    writer.setSink(std::move(sink));
    BeginGameBundleV2 begin{};
    begin.start = game.start;
    begin.frameZero = game.ticks[0];
    QNP_REQUIRE(writer.begin(memoryConfig(), begin, identity));
    QNP_REQUIRE(writer.append(game.ticks[1]));
    EndGameBundleV2 end{};
    end.containsFinalFrame = true;
    end.finalFrame = game.ticks[2];
    end.end = game.end;
    QNP_REQUIRE(writer.finalize(end));
    QNP_CHECK(writer.summary().compatibilityIdentityAttested);

    MemoryByteSource source(bytes->data());
    ParsedReplayV2 parsed{};
    ParseOptionsV2 options{};
    options.collectChunkIndex = true;
    QNP_REQUIRE(ReplayParserV2::parse(source, options, parsed).ok());
    QNP_CHECK(
        parsed.prefix.featureFlags == kFeatureCompatibilityIdentityV3);
    QNP_CHECK(parsed.hasCompatibilityIdentity);
    QNP_CHECK(parsed.compatibilityIdentity.attested());
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        parsed.compatibilityIdentity.identity, identity.identity));
    QNP_REQUIRE(parsed.chunks.size() >= 3u);
    QNP_CHECK(parsed.chunks[0].header.kind == ChunkKindV2::GameManifest);
    QNP_CHECK(parsed.chunks[1].header.kind
              == ChunkKindV2::CompatibilityIdentity);
    QNP_CHECK(parsed.chunks[2].header.kind == ChunkKindV2::GameStart);
}

QNP_TEST(ReplayParserV2RejectsIdentityFeatureWithoutItsCriticalEpochRecord) {
    const GameFixtureV2 game = fixture();
    ReplayWriterV2 writer;
    std::unique_ptr<MemoryFileSink> sink(new MemoryFileSink());
    MemoryFileSink* bytes = sink.get();
    writer.setSink(std::move(sink));
    QNP_REQUIRE(record(writer, game));
    std::vector<uint8_t> malformed = bytes->data();
    QNP_REQUIRE(malformed.size() >= kPrefixBytes);
    ReplayPrefixV2 prefix{};
    QNP_REQUIRE(decodePrefixV2(
        malformed.data(), kPrefixBytes, prefix) == ParseStatus::Ok);
    prefix.featureFlags = kFeatureCompatibilityIdentityV3;
    std::vector<uint8_t> encodedPrefix;
    encodePrefixV2(prefix, encodedPrefix);
    QNP_REQUIRE(encodedPrefix.size() == kPrefixBytes);
    std::copy(encodedPrefix.begin(), encodedPrefix.end(), malformed.begin());

    MemoryByteSource source(malformed);
    ParsedReplayV2 parsed{};
    ParseOptionsV2 options{};
    const ParseResult result = ReplayParserV2::parse(source, options, parsed);
    QNP_CHECK(!result.ok());
    QNP_CHECK(!parsed.hasCompatibilityIdentity);
    QNP_CHECK(parsed.compatibilityIdentity.legacyUnattested());
}

QNP_TEST(ReplayRecorderHostV3RefusesToArmWithoutFullCompatibilityIdentity) {
    RecorderHostConfigV3 config{};
    config.replayRoot = L"C:\\q99r-test";
    config.gamePid = 4321u;
    config.launchGeneration = 9u;
    config.launchNonce = 0x1234567890ABCDEFull;
    config.runtimeCompatibilityId = 0xCAFEBABEull;
    ReplayRecorderHostV3 host;
    QNP_CHECK(!host.start(config));
}

QNP_TEST(ReplayWriterV2EveryTypedAbnormalEndStillGetsFooterAndPublication) {
    for (const TerminalReason reason : {
             TerminalReason::Disconnected, TerminalReason::ProcessExit,
             TerminalReason::RecordingFailure,
             TerminalReason::RecorderOverflow}) {
        const GameFixtureV2 game = fixture(
            0x100u + static_cast<uint16_t>(reason), 0x200u,
            static_cast<uint32_t>(reason));
        ReplayWriterV2 writer;
        std::unique_ptr<MemoryFileSink> sink(new MemoryFileSink());
        MemoryFileSink* bytes = sink.get();
        writer.setSink(std::move(sink));
        QNP_REQUIRE(record(writer, game, false));
        ConfirmedGameEndV2 abnormal{};
        QNP_REQUIRE(writer.makeAbnormalEnd(reason, abnormal));
        EndGameBundleV2 end{};
        end.end = abnormal;
        QNP_REQUIRE(writer.finalize(end));
        QNP_CHECK(writer.summary().footerWritten);
        QNP_CHECK(writer.summary().finalized);
        MemoryByteSource source(bytes->data());
        ParsedReplayV2 parsed{};
        ParseOptionsV2 options{};
        QNP_REQUIRE(ReplayParserV2::parse(source, options, parsed).ok());
        QNP_CHECK(parsed.end.reason == reason);
        QNP_CHECK(parsed.end.nextRecord == 2u);
    }
}

QNP_TEST(ReplayWriterV2PublicationFailureDiscardsTheStream) {
    const GameFixtureV2 game = fixture();
    ReplayWriterV2 writer;
    std::unique_ptr<MemoryFileSink> sink(new MemoryFileSink());
    sink->faults.failRename = true;
    MemoryFileSink* bytes = sink.get();
    writer.setSink(std::move(sink));
    QNP_CHECK(!record(writer, game));
    QNP_CHECK(writer.error() == WriterError::RenameFailed);
    QNP_CHECK(writer.summary().footerWritten);

    QNP_CHECK(bytes->data().empty());
}

QNP_TEST(ReplayWriterV2InjectedRenameBoundaryCannotFallThroughToSuffix) {
    const GameFixtureV2 game = fixture();
    ReplayWriterV2 writer;
    std::unique_ptr<MemoryFileSink> sink(new MemoryFileSink());
    MemoryFileSink* bytes = sink.get();
    writer.setSink(std::move(sink));
    writer.setFaultInjector([](const WriterStageV2 stage) {
        return stage == WriterStageV2::Rename;
    });
    QNP_CHECK(!record(writer, game));
    QNP_CHECK(writer.error() == WriterError::RenameFailed);
    QNP_CHECK(writer.errorStage() == WriterStageV2::Rename);
    QNP_CHECK(writer.summary().footerWritten);
    QNP_CHECK(bytes->data().empty());
}

QNP_TEST(ReplayParserV2NeverPromotesAnyTruncationToSealed) {
    const GameFixtureV2 game = fixture();
    ReplayWriterV2 writer;
    std::unique_ptr<MemoryFileSink> sink(new MemoryFileSink());
    MemoryFileSink* memory = sink.get();
    writer.setSink(std::move(sink));
    QNP_REQUIRE(record(writer, game));
    const std::vector<uint8_t> bytes = memory->data();
    for (size_t cut = 0u; cut < bytes.size(); ++cut) {
        MemoryByteSource source(bytes.data(), cut);
        ParsedReplayV2 parsed{};
        ParseOptionsV2 options{};
        options.sourceNameEndsInPart = true;
        (void)ReplayParserV2::parse(source, options, parsed);
        QNP_CHECK(!parsed.sealed());
    }
}

QNP_TEST(ReplayRecoveryV2CreatesNewSealedIdentityWithoutMutatingSource) {
    const GameFixtureV2 game = fixture();
    ReplayWriterV2 sourceWriter;
    std::unique_ptr<MemoryFileSink> sourceSink(new MemoryFileSink());
    MemoryFileSink* sourceBytes = sourceSink.get();
    sourceWriter.setSink(std::move(sourceSink));
    QNP_REQUIRE(record(sourceWriter, game, false));
    const std::vector<uint8_t> original =
        sourceWriter.stagedBytesForTesting();
    sourceWriter.abandon();

    MemoryByteSource source(original);
    RecoveryPlanV2 plan = ReplayRecoveryV2::plan(source, true);
    QNP_REQUIRE(
        plan.classification == RecoveryClassV2::PlayableUnsealedPrefix);
    ReplayWriterV2 recoveredWriter;
    std::unique_ptr<MemoryFileSink> recoveredSink(new MemoryFileSink());
    MemoryFileSink* recoveredBytes = recoveredSink.get();
    recoveredWriter.setSink(std::move(recoveredSink));
    RecoveryOutcomeV2 outcome{};
    QNP_REQUIRE(ReplayRecoveryV2::execute(
        plan, L"C:\\q99r-test", L"source.q99r.part", outcome,
        &recoveredWriter));
    QNP_CHECK(outcome.summary.identity
              != game.start.manifest.identity);
    QNP_CHECK(sourceBytes->data().empty());

    MemoryByteSource recoveredSource(recoveredBytes->data());
    ParsedReplayV2 parsed{};
    ParseOptionsV2 options{};
    QNP_REQUIRE(
        ReplayParserV2::parse(recoveredSource, options, parsed).ok());
    QNP_CHECK(parsed.end.reason == TerminalReason::RecoveredPrefix);
    QNP_CHECK(parsed.end.nextRecord == 2u);
}

QNP_TEST(ReplayRecorderHostV3RotatesTwoGamesOnOneAuthenticatedStream) {
    for (const auto sourceKind : {ReplaySourceKindV2::NetplayPlayer,
                                  ReplaySourceKindV2::OfflineVsPlayer,
                                  ReplaySourceKindV2::SpectatorSave}) {
        qclient::replay::test::TempDir dir(L"per_game_clock");
        QNP_REQUIRE(dir.valid());
        int64_t clock = 1785456000 + 86390;
        size_t clockReads = 0;
        std::wstring setDirectory;
        constexpr uint32_t launch = 33u;
        constexpr uint64_t nonce = 0x1122334455667788ull;
        constexpr uint64_t runtime = 0x8877665544332211ull;
        RecorderHostConfigV3 config{};
        config.replayRoot = dir.path;
        config.gamePid = 1234u;
        config.launchGeneration = launch;
        config.launchNonce = nonce;
        config.runtimeCompatibilityId = runtime;
        ReplayRecorderHostV3 host;
        host.setConfigForTesting(config);
        host.setBattleClockForTesting([&] { ++clockReads; return clock; });

        wire3::RecorderHelloPayload hello{};
        hello.gamePid = config.gamePid;
        hello.runtimeCompatibilityId = runtime;
        hello.queueCapacity = 64u;
        std::vector<uint8_t> payload;
        QNP_REQUIRE(wire3::encodeRecorderHello(hello, payload));
        QNP_REQUIRE(host.feedFrameForTesting(inbound(
            wire3::MessageType::RecorderHello, 1u, launch, nonce,
            GameIdentityV2{}, payload)));

        uint32_t sequence = 2u;
        for (uint32_t index = 0u; index < 2u; ++index) {
            GameFixtureV2 game = fixture(
                0x1000u + index, 0x2000u + index, 10u + index);
            game.start.manifest.display.sourceKind = sourceKind;
            game.start.manifest.display.flags = DisplayMetadataSeriesV2;
            game.start.manifest.display.seriesUuidLow = 77;
            game.start.manifest.display.gameOrdinalInSeries = index;
            const int64_t expectedStart = clock;
            BeginGameBundleV2 begin{};
            begin.start = game.start;
            begin.frameZero = game.ticks[0];
            payload.clear();
            QNP_REQUIRE(wire3::encodeRecorderGameBegin(begin, payload));
            const auto beginFrame = inbound(
                wire3::MessageType::RecorderGameBegin, sequence++,
                launch, nonce, game.start.manifest.identity, payload, 0u, 1u);
            QNP_REQUIRE(host.feedFrameForTesting(beginFrame));
            const size_t readsAfterBegin = clockReads;
            clock += 10;
            QNP_REQUIRE(host.feedFrameForTesting(beginFrame));
            QNP_CHECK(clockReads == readsAfterBegin);

            payload.clear();
            QNP_REQUIRE(wire3::encodeRecorderFrames(
                &game.ticks[1].frame, 1u, payload));
            QNP_REQUIRE(host.feedFrameForTesting(inbound(
                wire3::MessageType::RecorderFrames, sequence++, launch,
                nonce, game.start.manifest.identity, payload, 1u, 1u)));
            payload.clear();
            QNP_REQUIRE(wire3::encodeRecorderEvents(
                game.ticks[1].postEvents.events.data(),
                game.ticks[1].postEvents.count, payload));
            QNP_REQUIRE(host.feedFrameForTesting(inbound(
                wire3::MessageType::RecorderEvents, sequence++, launch,
                nonce, game.start.manifest.identity, payload, 2u, 1u)));

            EndGameBundleV2 end{};
            end.containsFinalFrame = true;
            end.finalFrame = game.ticks[2];
            end.end = game.end;
            payload.clear();
            QNP_REQUIRE(wire3::encodeRecorderGameEnd(end, payload));
            QNP_REQUIRE(host.feedFrameForTesting(inbound(
                wire3::MessageType::RecorderGameEnd, sequence++, launch,
                nonce, game.start.manifest.identity, payload, 3u, 1u)));
            const std::wstring path = host.status().lastFinalPath;
            const std::wstring parent = path.substr(0, path.find_last_of(L"\\/"));
            if (index == 0) setDirectory = parent;
            QNP_CHECK(parent == setDirectory);
            FileByteSource file;
            QNP_REQUIRE(file.open(path));
            ParsedReplayV2 parsed;
            QNP_REQUIRE(ReplayParserV2::parse(file, ParseOptionsV2{}, parsed).ok());
            QNP_CHECK(parsed.start.manifest.display.recordedUnixSeconds == expectedStart);
            QNP_CHECK(parsed.start.start.initialRollingDigest == game.start.start.initialRollingDigest);
            QNP_CHECK(parsed.end.canonicalGameDigest == game.end.canonicalGameDigest);
            QNP_CHECK(parsed.start.manifest.display.gameOrdinalInSeries == index);
            clock += 30;
        }
        std::string battleText;
        QNP_REQUIRE(readBattleLogFile(dir.path, battleText));
        const auto rows = decodeBattleLogV1(battleText);
        QNP_REQUIRE(rows.size() == 2u);
        QNP_CHECK(rows[0].setStartUnix == 1785456000);
        QNP_CHECK(rows[1].setStartUnix == rows[0].setStartUnix);
        QNP_CHECK(rows[0].startUnix == 1785456000 + 86390);
        QNP_CHECK(rows[1].startUnix == 1785456000 + 86430);
        const RecorderStatusV3 status = host.status();
        QNP_CHECK(status.gamesBegun == 2u);
        QNP_CHECK(status.gamesCommitted == 2u);
        QNP_CHECK(!status.failed);
        const std::vector<wire3::Frame> replies =
            host.takeOutboundFramesForTesting();
        uint32_t accepted = 0u;
        uint32_t committed = 0u;
        GameIdentityV2 first{};
        GameIdentityV2 second{};
        for (const wire3::Frame& reply : replies) {
            if (reply.type == wire3::MessageType::CasterGameAccepted)
                ++accepted;
            if (reply.type == wire3::MessageType::CasterGameCommitted) {
                (committed++ == 0u ? first : second) = reply.game;
            }
        }
        QNP_CHECK(accepted == 4u);
        QNP_CHECK(committed == 2u);
        QNP_CHECK(first != second);
    }
}

QNP_TEST(ReplayRecorderHostV3DuplicateHelloIsRejected) {
    constexpr uint32_t launch = 44u;
    constexpr uint64_t nonce = 0xABCDEF0123456789ull;
    RecorderHostConfigV3 config{};
    config.replayRoot = L"C:\\q99r-test";
    config.gamePid = 4321u;
    config.launchGeneration = launch;
    config.launchNonce = nonce;
    config.runtimeCompatibilityId = 99u;
    ReplayRecorderHostV3 host;
    host.setConfigForTesting(config);
    wire3::RecorderHelloPayload hello{};
    hello.gamePid = config.gamePid;
    hello.runtimeCompatibilityId = config.runtimeCompatibilityId;
    hello.queueCapacity = 8u;
    std::vector<uint8_t> payload;
    QNP_REQUIRE(wire3::encodeRecorderHello(hello, payload));
    const wire3::Frame helloFrame = inbound(
        wire3::MessageType::RecorderHello, 1u, launch, nonce,
        GameIdentityV2{}, payload);
    QNP_REQUIRE(host.feedFrameForTesting(helloFrame));
    QNP_CHECK(!host.feedFrameForTesting(helloFrame));
}

QNP_TEST(ReplayRecorderHostV3ExactDataRetryIsIdempotentConflictFails) {
    constexpr uint32_t launch = 45u;
    constexpr uint64_t nonce = 0xABCDEF1123456789ull;
    RecorderHostConfigV3 config{};
    config.replayRoot = L"C:\\q99r-test";
    config.gamePid = 4322u;
    config.launchGeneration = launch;
    config.launchNonce = nonce;
    config.runtimeCompatibilityId = 100u;
    ReplayRecorderHostV3 host;
    host.setConfigForTesting(config);
    host.setWriterFactoryForTesting([] {
        std::unique_ptr<ReplayWriterV2> writer(new ReplayWriterV2());
        writer->setSink(
            std::unique_ptr<ReplayFileSink>(new MemoryFileSink()));
        return writer;
    });

    wire3::RecorderHelloPayload hello{};
    hello.gamePid = config.gamePid;
    hello.runtimeCompatibilityId = config.runtimeCompatibilityId;
    hello.queueCapacity = 8u;
    std::vector<uint8_t> payload;
    QNP_REQUIRE(wire3::encodeRecorderHello(hello, payload));
    QNP_REQUIRE(host.feedFrameForTesting(inbound(
        wire3::MessageType::RecorderHello, 1u, launch, nonce,
        GameIdentityV2{}, payload)));

    const GameFixtureV2 game = fixture(0x9001u, 0x9002u, 45u);
    BeginGameBundleV2 begin{};
    begin.start = game.start;
    begin.frameZero = game.ticks[0];
    payload.clear();
    QNP_REQUIRE(wire3::encodeRecorderGameBegin(begin, payload));
    const wire3::Frame beginFrame = inbound(
        wire3::MessageType::RecorderGameBegin, 2u, launch, nonce,
        game.start.manifest.identity, payload, 0u, 1u);
    QNP_REQUIRE(host.feedFrameForTesting(beginFrame));
    const std::vector<wire3::Frame> firstReplies =
        host.takeOutboundFramesForTesting();
    QNP_REQUIRE(firstReplies.size() == 1u);
    QNP_REQUIRE(host.feedFrameForTesting(beginFrame));
    const std::vector<wire3::Frame> retryReplies =
        host.takeOutboundFramesForTesting();
    QNP_REQUIRE(retryReplies.size() == 1u);
    QNP_CHECK(retryReplies[0].sequence == firstReplies[0].sequence);
    QNP_CHECK(retryReplies[0].payload == firstReplies[0].payload);
    QNP_CHECK(host.status().gamesBegun == 1u);

    wire3::Frame conflict = beginFrame;
    ++conflict.param0;
    QNP_CHECK(!host.feedFrameForTesting(conflict));
    QNP_CHECK(host.status().failed);
    QNP_CHECK(host.status().failure
              == wire3::RecorderFailureCode::Sequence);
}

QNP_TEST(ReplayRecorderHostV3PipeCloseUsesProcessProvenanceForTerminal) {
    for (const bool alive : {false, true}) {
        constexpr uint32_t launch = 46u;
        constexpr uint64_t nonce = 0xABCDEF2123456789ull;
        RecorderHostConfigV3 config{};
        config.replayRoot = L"C:\\q99r-test";
        config.gamePid = alive ? 5001u : 5002u;
        config.launchGeneration = launch;
        config.launchNonce = nonce;
        config.runtimeCompatibilityId = 101u;
        ReplayRecorderHostV3 host;
        host.setConfigForTesting(config);
        host.setProcessAliveProbeForTesting(
            [alive](uint32_t) { return alive; });
        host.setWriterFactoryForTesting([] {
            std::unique_ptr<ReplayWriterV2> writer(new ReplayWriterV2());
            writer->setSink(
                std::unique_ptr<ReplayFileSink>(new MemoryFileSink()));
            return writer;
        });

        wire3::RecorderHelloPayload hello{};
        hello.gamePid = config.gamePid;
        hello.runtimeCompatibilityId =
            config.runtimeCompatibilityId;
        hello.queueCapacity = 8u;
        std::vector<uint8_t> payload;
        QNP_REQUIRE(wire3::encodeRecorderHello(hello, payload));
        QNP_REQUIRE(host.feedFrameForTesting(inbound(
            wire3::MessageType::RecorderHello, 1u, launch, nonce,
            GameIdentityV2{}, payload)));

        const GameFixtureV2 game = fixture(
            alive ? 0xA001u : 0xB001u,
            alive ? 0xA002u : 0xB002u, 46u);
        BeginGameBundleV2 begin{};
        begin.start = game.start;
        begin.frameZero = game.ticks[0];
        payload.clear();
        QNP_REQUIRE(wire3::encodeRecorderGameBegin(begin, payload));
        QNP_REQUIRE(host.feedFrameForTesting(inbound(
            wire3::MessageType::RecorderGameBegin, 2u, launch, nonce,
            game.start.manifest.identity, payload, 0u, 1u)));

        host.stop();
        const RecorderStatusV3 status = host.status();
        QNP_CHECK(status.gamesCommitted == 1u);
        QNP_CHECK(status.lastProcessAlive == alive);
        QNP_CHECK(status.lastTerminalReason
                  == (alive ? TerminalReason::RecordingFailure
                            : TerminalReason::ProcessExit));
        QNP_CHECK(status.failed == alive);
    }
}
