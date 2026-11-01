#include "TestHarness.h"

#include "replay/ConfirmedTimeline.h"
#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"
#include "replay/ReplayIpcWire.h"

#include <cstring>
#include <type_traits>
#include <vector>

namespace {

using namespace qreplay;
namespace wire3 = qreplay::ipc::v3;

void setNativeName(
    std::array<char, kNativeDisplayNameBytesV2>& out, const char* text) {
    out.fill('\0');
    for (size_t i = 0u; text[i] != '\0'
         && i < kNativeDisplayNamePayloadBytesV2; ++i) {
        out[i] = text[i];
    }
}

ConfirmedGameStartV2 sampleStartV2() {
    ConfirmedGameStartV2 start{};
    start.manifest.identity.gameUuidLow = 0x0123456789ABCDEFull;
    start.manifest.identity.gameUuidHigh = 0xFEDCBA9876543210ull;
    start.manifest.identity.gameGeneration = 7u;
    SimulationContractV2& contract = start.manifest.contract;
    for (size_t i = 0u; i < contract.executableSha256.size(); ++i)
        contract.executableSha256[i] = static_cast<uint8_t>(i + 1u);
    for (size_t i = 0u; i < contract.contentManifestRoot.size(); ++i)
        contract.contentManifestRoot[i] =
            static_cast<uint8_t>(0x80u + i);
    contract.nativeBuildIdentity = 0x1111222233334444ull;
    contract.runtimeSimulationId = 0x5555666677778888ull;
    contract.stateRegionSchemaFingerprint = 0x0102030405060708ull;
    contract.stateHashProjectionId = 0x1112131415161718ull;
    contract.stateHashAlgorithmId = 0x2122232425262728ull;
    contract.bootstrapSchemaId = 0x3132333435363738ull;
    contract.eventSchemaId = 0x4142434445464748ull;
    contract.characterCatalogId = 0x5152535455565758ull;
    contract.terminalResultSchemaId = 0x6162636465666768ull;
    contract.compactActionDomainVersion = 1u;

    GameManifestV2& manifest = start.manifest;
    manifest.vsRow = 1u;
    manifest.activeLaneMask = 0x03u;
    manifest.activeFighterMask = 0x03u;
    manifest.cpuMask = 0u;
    manifest.cpuContract = CpuContract::None;
    manifest.slotToSourceRoute = {{0u, 1u, 2u, 3u}};
    manifest.lanes[0].active = true;
    manifest.lanes[1].active = true;
    (void)setReplayTextV2(manifest.lanes[0].name, "Player One");
    (void)setReplayTextV2(manifest.lanes[1].name, "Player Two");
    manifest.slots[0].ownership = SlotOwnership::RecordedHuman;
    manifest.slots[0].side = 0u;
    manifest.slots[0].identity.nativeCharacterId = 1u;
    manifest.slots[0].identity.paletteIndex = 2u;
    manifest.slots[1].ownership = SlotOwnership::RecordedHuman;
    manifest.slots[1].side = 1u;
    manifest.slots[1].identity.nativeCharacterId = 8u;
    manifest.slots[1].identity.paletteIndex = 3u;
    manifest.slots[2].side = 0u;
    manifest.slots[3].side = 1u;
    manifest.routeEpoch = 10u;
    manifest.rosterEpoch = 11u;
    manifest.layoutGeneration = 12u;
    manifest.stage.nativeStageId = 4u;
    manifest.stage.verified = true;
    manifest.stage.catalogEntryId = 0xAABBCCDDu;
    manifest.display.recordedUnixSeconds = 1785456000;
    manifest.display.timezoneOffsetMinutes = 180;
    manifest.display.sourceKind = ReplaySourceKindV2::NetplayPlayer;
    manifest.display.flags =
        DisplayMetadataNetworkV2 | DisplayMetadataSeriesV2;
    manifest.display.playerOneDelay = 2u;
    manifest.display.playerTwoDelay = 3u;
    manifest.display.rollbackWindow = 8u;
    manifest.display.averageRttMicros = 45000u;
    manifest.display.jitterMicros = 4000u;
    manifest.display.lossPermille = 5u;
    manifest.display.seriesUuidLow = 0xABCDEFu;
    manifest.display.gameOrdinalInSeries = 2u;
    (void)setReplayTextV2(manifest.display.title, "Final game");
    (void)setReplayTextV2(
        manifest.display.routeLabel, "punched-direct");

    DeterministicGameStartV2& deterministic = start.start;
    deterministic.configuration.timerMode = 2u;
    deterministic.configuration.aiDifficulty = 4u;
    deterministic.configuration.lifeMultiplier = 0u;
    deterministic.configuration.roundsToWinMinusOne = 1u;
    deterministic.rng.gameplay = 0x11111111u;
    deterministic.rng.cosmetic = 0x22222222u;
    deterministic.rng.selector = 0x33333333u;
    deterministic.fpu.x87ControlWord = 0x027Fu;
    deterministic.fpu.mxcsr = 0x1F80u;
    deterministic.nativeDisplay.codePages = {{932u, 932u, 932u, 932u}};
    setNativeName(deterministic.nativeDisplay.names[0], "AQUA");
    setNativeName(deterministic.nativeDisplay.names[1], "RIVAL");
    deterministic.initialPhase = 0u;
    deterministic.bootstrapFingerprint = 0x1234567890ABCDEFull;
    deterministic.frameZeroPreStateHash = 0x1000u;
    deterministic.initialRollingDigest = initialReplayDigestV2(start);
    return start;
}

ConfirmedGameEventV2 eventV2(
    GameEventKindV2 kind, GameEventPhaseV2 phase, uint32_t frame,
    uint32_t sequence, uint64_t proof) {
    ConfirmedGameEventV2 event{};
    event.kind = kind;
    event.phase = phase;
    event.replayFrame = frame;
    event.eventSequence = sequence;
    event.stateProof = proof;
    if (kind == GameEventKindV2::RoundStarted) {
        event.transitionSiteId = 100u;
        event.roundOrdinal = 1u;
    } else if (kind == GameEventKindV2::RoundEnded
               || kind == GameEventKindV2::GameResult) {
        event.transitionSiteId = kind == GameEventKindV2::RoundEnded
            ? 200u : 201u;
        event.roundOrdinal = 2u;
        event.winningSideMask = 0x01u;
        event.roundTallies = {{2u, 1u}};
        event.winnerSlot = 0;
    }
    return event;
}

CanonicalTickBundleV2 frameV2(
    uint32_t sourceFrame, uint32_t replayFrame,
    const GameLifecycleV2& before, const GameLifecycleV2& after,
    uint64_t pre, uint64_t post) {
    CanonicalTickBundleV2 tick{};
    tick.frame.sourceFrame = sourceFrame;
    tick.frame.replayFrame = replayFrame;
    tick.frame.core.inputDomain = InputDomain::LogicalSourceV1;
    tick.frame.core.activeLaneMask = 0x03u;
    tick.frame.core.actions[0] =
        static_cast<uint16_t>(0x0100u | replayFrame);
    tick.frame.core.actions[1] =
        static_cast<uint16_t>(0x0001u | (replayFrame << 8));
    tick.frame.core.before = before;
    tick.frame.core.after = after;
    tick.frame.core.dispatcherResult = 1;
    tick.frame.core.preStateHash = pre;
    tick.frame.core.postStateHash = post;
    return tick;
}

struct CompleteGameV2 {
    ConfirmedGameStartV2 start;
    std::vector<CanonicalTickBundleV2> ticks;
    ConfirmedGameEndV2 end;
};

CompleteGameV2 completeGameV2(bool draw = false) {
    CompleteGameV2 game{};
    game.start = sampleStartV2();
    uint64_t rolling = game.start.start.initialRollingDigest;
    uint32_t eventSequence = 0u;

    GameLifecycleV2 zeroBefore{};
    zeroBefore.scene = 2u;
    zeroBefore.requestedScene = 2u;
    zeroBefore.matchSub = 0;
    zeroBefore.winnerSlot = -1;
    zeroBefore.routeEpoch = game.start.manifest.routeEpoch;
    zeroBefore.rosterEpoch = game.start.manifest.rosterEpoch;
    zeroBefore.layoutGeneration = game.start.manifest.layoutGeneration;
    GameLifecycleV2 zeroAfter = zeroBefore;
    zeroAfter.matchSub = 1;
    CanonicalTickBundleV2 zero =
        frameV2(500u, 0u, zeroBefore, zeroAfter, 0x1000u, 0x2000u);
    zero.preEvents.count = 2u;
    zero.preEvents.events[0] = eventV2(
        GameEventKindV2::GameStarted, GameEventPhaseV2::PreFrame, 0u,
        eventSequence++, zero.frame.core.preStateHash);
    zero.preEvents.events[0].rollingDigest =
        advanceReplayDigestV2(rolling, zero.preEvents.events[0]);
    rolling = zero.preEvents.events[0].rollingDigest;
    zero.preEvents.events[1] = eventV2(
        GameEventKindV2::RoundStarted, GameEventPhaseV2::PreFrame, 0u,
        eventSequence++, zero.frame.core.preStateHash);
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
    transitionAfter.winnerSlot = draw ? -1 : 0;
    transitionAfter.roundTallies =
        draw ? std::array<uint8_t, 2>{{1u, 1u}}
             : std::array<uint8_t, 2>{{2u, 1u}};
    CanonicalTickBundleV2 transition = frameV2(
        501u, 1u, zeroAfter, transitionAfter, 0x2000u, 0x3000u);
    transition.frame.rollingDigest =
        advanceReplayDigestV2(rolling, transition.frame);
    rolling = transition.frame.rollingDigest;
    transition.postEvents.count = 1u;
    transition.postEvents.events[0] = eventV2(
        GameEventKindV2::RoundEnded, GameEventPhaseV2::PostFrame, 1u,
        eventSequence++, transition.frame.core.postStateHash);
    if (draw) {
        transition.postEvents.events[0].winningSideMask = 0u;
        transition.postEvents.events[0].winnerSlot = -1;
        transition.postEvents.events[0].roundTallies = {{1u, 1u}};
    }
    transition.postEvents.events[0].rollingDigest =
        advanceReplayDigestV2(rolling, transition.postEvents.events[0]);
    rolling = transition.postEvents.events[0].rollingDigest;
    game.ticks.push_back(transition);

    GameLifecycleV2 winscreenAfter = transitionAfter;
    winscreenAfter.resultCounter = 1u;
    CanonicalTickBundleV2 winscreen = frameV2(
        502u, 2u, transitionAfter, winscreenAfter, 0x3000u, 0x4000u);
    winscreen.frame.rollingDigest =
        advanceReplayDigestV2(rolling, winscreen.frame);
    rolling = winscreen.frame.rollingDigest;
    winscreen.postEvents.count = 1u;
    winscreen.postEvents.events[0] = eventV2(
        GameEventKindV2::GameResult, GameEventPhaseV2::PostFrame, 2u,
        eventSequence++, winscreen.frame.core.postStateHash);
    if (draw) {
        winscreen.postEvents.events[0].winningSideMask = 0u;
        winscreen.postEvents.events[0].winnerSlot = -1;
        winscreen.postEvents.events[0].roundTallies = {{1u, 1u}};
    }
    winscreen.postEvents.events[0].rollingDigest =
        advanceReplayDigestV2(rolling, winscreen.postEvents.events[0]);
    rolling = winscreen.postEvents.events[0].rollingDigest;
    game.ticks.push_back(winscreen);

    game.end.reason = TerminalReason::Completed;
    game.end.nextRecord = 3u;
    game.end.winningSideMask = draw ? 0u : 0x01u;
    game.end.roundTallies =
        draw ? std::array<uint8_t, 2>{{1u, 1u}}
             : std::array<uint8_t, 2>{{2u, 1u}};
    game.end.winnerSlot = draw ? -1 : 0;
    game.end.postTailStateHash = 0x4000u;
    game.end.finalRollingDigest = rolling;

    CanonicalGameDigestV2 canonical;
    if (!canonical.begin(game.start)) return CompleteGameV2{};
    for (const CanonicalTickBundleV2& tick : game.ticks) {
        for (size_t i = 0u; i < tick.preEvents.count; ++i)
            if (!canonical.append(tick.preEvents.events[i]))
                return CompleteGameV2{};
        if (!canonical.append(tick.frame)) return CompleteGameV2{};
        for (size_t i = 0u; i < tick.postEvents.count; ++i)
            if (!canonical.append(tick.postEvents.events[i]))
                return CompleteGameV2{};
    }
    if (!canonical.finish(game.end, game.end.canonicalGameDigest))
        return CompleteGameV2{};
    return game;
}

template <typename Decoder>
void everyTruncationFailsV2(
    const std::vector<uint8_t>& bytes, Decoder decode) {
    for (size_t size = 0u; size < bytes.size(); ++size) {
        std::vector<uint8_t> partial(
            bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(size));
        QNP_CHECK(decode(
            partial.empty() ? nullptr : partial.data(), partial.size())
            != ParseStatus::Ok);
    }
}

template <typename Decoder>
void everyBoolTruncationFailsV2(
    const std::vector<uint8_t>& bytes, Decoder decode) {
    for (size_t size = 0u; size < bytes.size(); ++size) {
        std::vector<uint8_t> partial(
            bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(size));
        QNP_CHECK(!decode(
            partial.empty() ? nullptr : partial.data(), partial.size()));
    }
}

}

QNP_TEST(ReplayV2PrefixAndChunkHeadersAreDistinctFromLegacy) {
    QNP_CHECK(kContainerVersionMajor == 1u);
    QNP_CHECK(kContainerVersionMajorV2 == 2u);
    ReplayPrefixV2 prefix{};
    prefix.featureFlags = kFeatureFourPlayer;
    prefix.gameUuidLow = 0x1122334455667788ull;
    prefix.gameUuidHigh = 0x99AABBCCDDEEFF00ull;
    std::vector<uint8_t> bytes;
    encodePrefixV2(prefix, bytes);
    QNP_REQUIRE(bytes.size() == kPrefixBytes);
    QNP_CHECK(bytes[0] == 'Q' && bytes[1] == '9'
              && bytes[2] == '9' && bytes[3] == 'R');
    QNP_CHECK(bytes[4] == 2u && bytes[5] == 0u);
    ReplayPrefixV2 decoded{};
    QNP_CHECK(decodePrefixV2(bytes.data(), bytes.size(), decoded)
              == ParseStatus::Ok);
    QNP_CHECK(decoded.gameUuidLow == prefix.gameUuidLow);
    everyTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        ReplayPrefixV2 out{};
        return decodePrefixV2(p, n, out);
    });

    ReplayPrefix legacy{};
    legacy.sessionUuidLow = 1u;
    std::vector<uint8_t> legacyBytes;
    encodePrefix(legacy, legacyBytes);
    ReplayPrefix legacyDecoded{};
    QNP_CHECK(decodePrefix(
        legacyBytes.data(), legacyBytes.size(), legacyDecoded)
        == ParseStatus::Ok);
    QNP_CHECK(decodePrefixV2(
        legacyBytes.data(), legacyBytes.size(), decoded)
        == ParseStatus::UnsupportedContainerVersion);

    ChunkHeaderV2 header{};
    header.kind = ChunkKindV2::InputBlock;
    header.schemaVersion = 2u;
    header.critical = 1u;
    header.sequence = 3u;
    header.gameGeneration = 7u;
    header.firstRecord = 10u;
    header.nextRecord = 13u;
    header.recordCount = 3u;
    header.payloadBytes = 416u;
    header.decodedBytes = 416u;
    bytes.clear();
    encodeChunkHeaderV2(header, bytes);
    QNP_REQUIRE(bytes.size() == kChunkHeaderBytes);
    ChunkHeaderV2 headerOut{};
    QNP_CHECK(decodeChunkHeaderV2(bytes.data(), bytes.size(), headerOut)
              == ParseStatus::Ok);
    QNP_CHECK(headerOut.nextRecord == 13u);
    everyTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        ChunkHeaderV2 out{};
        return decodeChunkHeaderV2(p, n, out);
    });
    bytes[28] = 4u;
    QNP_CHECK(decodeChunkHeaderV2(bytes.data(), bytes.size(), headerOut)
              == ParseStatus::BadChunkHeaderCrc);
}

QNP_TEST(ReplayV2FooterRoundTripProjectionAndMalformedBounds) {
    CompleteGameV2 game = completeGameV2();
    FooterIndexV2 footer{};
    footer.chunkCount = 7u;
    footer.frameCount = 3u;
    footer.eventCount = 4u;
    footer.manifestOffset = kPrefixBytes;
    footer.startOffset = 800u;
    footer.firstInputOffset = 1200u;
    footer.gameEndOffset = 2400u;
    footer.finalRollingDigest = game.end.finalRollingDigest;
    footer.canonicalGameDigest = game.end.canonicalGameDigest;
    footer.fileDigest.fill(0xA5u);
    RoundIndexEntryV2 round{};
    round.roundOrdinal = 1u;
    round.winningSideMask = 0x01u;
    round.roundTallies = {{2u, 1u}};
    round.firstRecord = 0u;
    round.nextRecord = 3u;
    round.firstEventSequence = 0u;
    round.nextEventSequence = 4u;
    round.firstChunkOffset = footer.firstInputOffset;
    round.lastChunkOffset = 2200u;
    footer.rounds.push_back(round);

    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeFooterIndexV2(footer, bytes));
    QNP_CHECK(bytes.size()
              == kFooterFixedBytesV2 + kRoundIndexEntryBytesV2);
    FooterIndexV2 decoded{};
    QNP_CHECK(decodeFooterIndexV2(
        bytes.data(), bytes.size(), decoded) == ParseStatus::Ok);
    QNP_CHECK(decoded.chunkCount == footer.chunkCount);
    QNP_CHECK(decoded.rounds == footer.rounds);
    QNP_CHECK(decoded.fileDigest == footer.fileDigest);
    everyTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        FooterIndexV2 out{};
        return decodeFooterIndexV2(p, n, out);
    });

    std::vector<uint8_t> projection;
    QNP_REQUIRE(encodeFooterIndexFileDigestProjectionV2(
        footer, projection));
    QNP_REQUIRE(projection.size() == bytes.size());
    for (size_t i = projection.size() - 32u;
         i < projection.size(); ++i) {
        QNP_CHECK(projection[i] == 0u);
    }
    QNP_CHECK(!std::equal(
        projection.end() - 32, projection.end(), bytes.end() - 32));

    std::vector<uint8_t> hostile = bytes;
    hostile[88] = 1u;
    QNP_CHECK(decodeFooterIndexV2(
        hostile.data(), hostile.size(), decoded)
        == ParseStatus::UnsupportedContainerVersion);
    hostile = bytes;
    hostile[132] = 1u;
    QNP_CHECK(decodeFooterIndexV2(
        hostile.data(), hostile.size(), decoded)
        == ParseStatus::UnsupportedContainerVersion);

    FooterIndexV2 bad = footer;
    bad.eventCount = kMaxTimelineEventsV2 + 1u;
    std::vector<uint8_t> rejected;
    QNP_CHECK(!encodeFooterIndexV2(bad, rejected));
    bad = footer;
    bad.fileDigest.fill(0u);
    QNP_CHECK(!encodeFooterIndexV2(bad, rejected));
}

QNP_TEST(ReplayV2ManifestStartAndBeginBundleRoundTrip) {
    CompleteGameV2 game = completeGameV2();
    std::vector<uint8_t> manifest;
    QNP_REQUIRE(encodeGameManifestV2(game.start.manifest, manifest));
    QNP_CHECK(manifest.size() == kGameManifestBytesV2);
    GameManifestV2 manifestOut{};
    QNP_CHECK(decodeGameManifestV2(
        manifest.data(), manifest.size(), manifestOut) == ParseStatus::Ok);
    QNP_CHECK(manifestOut == game.start.manifest);
    everyTruncationFailsV2(manifest, [&](const uint8_t* p, size_t n) {
        GameManifestV2 out{};
        return decodeGameManifestV2(p, n, out);
    });

    std::vector<uint8_t> deterministic;
    QNP_REQUIRE(encodeDeterministicGameStartV2(
        game.start.start, deterministic));
    QNP_CHECK(deterministic.size() == kDeterministicGameStartBytesV2);
    DeterministicGameStartV2 startOut{};
    QNP_CHECK(decodeDeterministicGameStartV2(
        deterministic.data(), deterministic.size(), startOut)
        == ParseStatus::Ok);
    QNP_CHECK(startOut == game.start.start);
    everyTruncationFailsV2(
        deterministic, [&](const uint8_t* p, size_t n) {
            DeterministicGameStartV2 out{};
            return decodeDeterministicGameStartV2(p, n, out);
        });

    BeginGameBundleV2 begin{};
    begin.start = game.start;
    begin.frameZero = game.ticks[0];
    std::vector<uint8_t> beginBytes;
    QNP_REQUIRE(encodeBeginGameBundleV2(begin, beginBytes));
    BeginGameBundleV2 beginOut{};
    QNP_CHECK(decodeBeginGameBundleV2(
        beginBytes.data(), beginBytes.size(), beginOut) == ParseStatus::Ok);
    QNP_CHECK(beginOut.start == begin.start);
    QNP_CHECK(beginOut.frameZero == begin.frameZero);
    everyTruncationFailsV2(beginBytes, [&](const uint8_t* p, size_t n) {
        BeginGameBundleV2 out{};
        return wire3::decodeRecorderGameBegin(p, n, out);
    });
}

QNP_TEST(ReplayV2ManifestRejectsUnresolvedOrAmbiguousSimulationIdentity) {
    CompleteGameV2 game = completeGameV2();
    std::vector<uint8_t> rejected;

    GameManifestV2 bad = game.start.manifest;
    bad.vsRow = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.vsRow = 6u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));

    bad = game.start.manifest;
    bad.stage.nativeStageId = kMaxNativeStageIdV2 + 1u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.stage.verified = false;
    bad.stage.nativeStageId = 0u;
    bad.stage.catalogEntryId = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));

    bad = game.start.manifest;
    bad.contract = SimulationContractV2{};
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.executableSha256.fill(0u);
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.contentManifestRoot.fill(0u);
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.nativeBuildIdentity = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.runtimeSimulationId = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.stateRegionSchemaFingerprint = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.stateHashProjectionId = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.stateHashAlgorithmId = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.bootstrapSchemaId = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.eventSchemaId = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.characterCatalogId = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.terminalResultSchemaId = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
    bad = game.start.manifest;
    bad.contract.compactActionDomainVersion = 0u;
    QNP_CHECK(!encodeGameManifestV2(bad, rejected));
}

QNP_TEST(ReplayV2FrameEventEndAndBlocksAreExact) {
    CompleteGameV2 game = completeGameV2();
    ConfirmedGameFrameV2 changedSource = game.ticks[0].frame;
    changedSource.sourceFrame = 123456789u;
    std::vector<uint8_t> fileA;
    std::vector<uint8_t> fileB;
    QNP_REQUIRE(encodeGameFrameRecordV2(game.ticks[0].frame, fileA));
    QNP_REQUIRE(encodeGameFrameRecordV2(changedSource, fileB));
    QNP_CHECK(fileA.size() == kGameFrameRecordBytesV2);
    QNP_CHECK(fileA == fileB);
    everyTruncationFailsV2(fileA, [&](const uint8_t* p, size_t n) {
        ConfirmedGameFrameV2 out{};
        return decodeGameFrameRecordV2(p, n, 0u, 0x03u, out);
    });

    std::vector<uint8_t> transportA;
    std::vector<uint8_t> transportB;
    QNP_REQUIRE(encodeConfirmedGameFrameTransportV2(
        game.ticks[0].frame, transportA));
    QNP_REQUIRE(encodeConfirmedGameFrameTransportV2(
        changedSource, transportB));
    QNP_CHECK(transportA.size() == kConfirmedGameFrameTransportBytesV2);
    QNP_CHECK(transportA != transportB);
    everyTruncationFailsV2(
        transportA, [&](const uint8_t* p, size_t n) {
            ConfirmedGameFrameV2 out{};
            return decodeConfirmedGameFrameTransportV2(
                p, n, 0x03u, out);
        });

    std::vector<uint8_t> eventBytes;
    QNP_REQUIRE(encodeConfirmedGameEventV2(
        game.ticks[2].postEvents.events[0], eventBytes));
    QNP_CHECK(eventBytes.size() == kConfirmedGameEventBytesV2);
    ConfirmedGameEventV2 eventOut{};
    QNP_CHECK(decodeConfirmedGameEventV2(
        eventBytes.data(), eventBytes.size(), eventOut) == ParseStatus::Ok);
    QNP_CHECK(eventOut == game.ticks[2].postEvents.events[0]);
    everyTruncationFailsV2(eventBytes, [&](const uint8_t* p, size_t n) {
        ConfirmedGameEventV2 out{};
        return decodeConfirmedGameEventV2(p, n, out);
    });

    std::vector<uint8_t> endBytes;
    QNP_REQUIRE(encodeConfirmedGameEndV2(game.end, endBytes));
    QNP_CHECK(endBytes.size() == kConfirmedGameEndBytesV2);
    ConfirmedGameEndV2 endOut{};
    QNP_CHECK(decodeConfirmedGameEndV2(
        endBytes.data(), endBytes.size(), endOut) == ParseStatus::Ok);
    QNP_CHECK(endOut == game.end);
    everyTruncationFailsV2(endBytes, [&](const uint8_t* p, size_t n) {
        ConfirmedGameEndV2 out{};
        return decodeConfirmedGameEndV2(p, n, out);
    });

    std::vector<ConfirmedGameFrameV2> frames;
    for (const auto& tick : game.ticks) frames.push_back(tick.frame);
    std::vector<uint8_t> block;
    QNP_REQUIRE(encodeInputBlockV2(frames.data(), frames.size(), block));
    std::vector<ConfirmedGameFrameV2> framesOut;
    QNP_CHECK(decodeInputBlockV2(
        block.data(), block.size(), 0u,
        static_cast<uint32_t>(frames.size()), 0x03u, framesOut)
        == ParseStatus::Ok);
    QNP_REQUIRE(framesOut.size() == frames.size());
    for (size_t i = 0u; i < frames.size(); ++i) {
        QNP_CHECK(framesOut[i].replayFrame == frames[i].replayFrame);
        QNP_CHECK(framesOut[i].sourceFrame == frames[i].replayFrame);
        QNP_CHECK(framesOut[i].core == frames[i].core);
    }
    everyTruncationFailsV2(block, [&](const uint8_t* p, size_t n) {
        std::vector<ConfirmedGameFrameV2> out;
        return decodeInputBlockV2(
            p, n, 0u, static_cast<uint32_t>(frames.size()), 0x03u,
            out);
    });
}

QNP_TEST(ReplayV2EndBundleIsAnAtomicCompletedOrAbnormalBoundary) {
    CompleteGameV2 game = completeGameV2();
    EndGameBundleV2 completed{};
    completed.containsFinalFrame = true;
    completed.finalFrame = game.ticks.back();
    completed.end = game.end;

    std::vector<uint8_t> bytes;
    QNP_REQUIRE(wire3::encodeRecorderGameEnd(completed, bytes));
    EndGameBundleV2 decoded{};
    QNP_CHECK(wire3::decodeRecorderGameEnd(
        bytes.data(), bytes.size(),
        game.start.manifest.activeLaneMask, decoded) == ParseStatus::Ok);
    QNP_CHECK(decoded == completed);
    everyTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        EndGameBundleV2 out{};
        return wire3::decodeRecorderGameEnd(
            p, n, game.start.manifest.activeLaneMask, out);
    });

    EndGameBundleV2 missingFinal = completed;
    missingFinal.containsFinalFrame = false;
    missingFinal.finalFrame = CanonicalTickBundleV2{};
    std::vector<uint8_t> rejected;
    QNP_CHECK(!encodeEndGameBundleV2(missingFinal, rejected));

    EndGameBundleV2 eventAfterResult = completed;
    eventAfterResult.finalFrame.postEvents.count = 2u;
    eventAfterResult.finalFrame.postEvents.events[1] = eventV2(
        GameEventKindV2::NetworkSample, GameEventPhaseV2::PostFrame,
        eventAfterResult.finalFrame.frame.replayFrame, 4u,
        eventAfterResult.finalFrame.frame.core.postStateHash);
    eventAfterResult.finalFrame.postEvents.events[1].averageRttMicros =
        1000u;
    rejected.clear();
    QNP_CHECK(!wire3::encodeRecorderGameEnd(
        eventAfterResult, rejected));

    EndGameBundleV2 abnormal{};
    abnormal.end = game.end;
    abnormal.end.reason = TerminalReason::Disconnected;
    abnormal.end.winningSideMask = 0u;
    abnormal.end.roundTallies = {{0u, 0u}};
    abnormal.end.winnerSlot = -1;
    abnormal.end.canonicalGameDigest.fill(0u);
    bytes.clear();
    QNP_REQUIRE(wire3::encodeRecorderGameEnd(abnormal, bytes));
    QNP_CHECK(bytes.size() == 4u + kConfirmedGameEndBytesV2);
    decoded = EndGameBundleV2{};
    QNP_CHECK(wire3::decodeRecorderGameEnd(
        bytes.data(), bytes.size(),
        game.start.manifest.activeLaneMask, decoded) == ParseStatus::Ok);
    QNP_CHECK(!decoded.containsFinalFrame);
    QNP_CHECK(decoded.end == abnormal.end);

    abnormal.finalFrame = game.ticks.back();
    rejected.clear();
    QNP_CHECK(!encodeEndGameBundleV2(abnormal, rejected));
}

QNP_TEST(ReplayV2DigestDomainsExcludeDisplayIdentityAndSourceFrame) {
    CompleteGameV2 game = completeGameV2();
    ConfirmedGameStartV2 displayChanged = game.start;
    displayChanged.manifest.identity.gameUuidLow ^= 0xFFFFFFFFu;
    displayChanged.manifest.identity.gameGeneration += 100u;
    QNP_REQUIRE(setReplayTextV2(
        displayChanged.manifest.display.title, "Different title"));
    displayChanged.manifest.display.recordedUnixSeconds += 99;
    displayChanged.start.initialRollingDigest =
        initialReplayDigestV2(displayChanged);
    QNP_CHECK(displayChanged.start.initialRollingDigest
              == game.start.start.initialRollingDigest);

    ConfirmedGameStartV2 simulationChanged = game.start;
    simulationChanged.start.rng.gameplay ^= 1u;
    simulationChanged.start.initialRollingDigest =
        initialReplayDigestV2(simulationChanged);
    QNP_CHECK(simulationChanged.start.initialRollingDigest
              != game.start.start.initialRollingDigest);

    ConfirmedGameFrameV2 sourceChanged = game.ticks[0].frame;
    sourceChanged.sourceFrame += 999u;
    QNP_CHECK(advanceReplayDigestV2(
                  game.start.start.initialRollingDigest, sourceChanged)
              == advanceReplayDigestV2(
                  game.start.start.initialRollingDigest,
                  game.ticks[0].frame));

    ConfirmedGameEventV2 sample{};
    sample.kind = GameEventKindV2::NetworkSample;
    sample.phase = GameEventPhaseV2::PostFrame;
    sample.averageRttMicros = 90000u;
    sample.jitterMicros = 10000u;
    sample.lossPermille = 15u;
    QNP_CHECK(advanceReplayDigestV2(0x1234u, sample) == 0x1234u);
}

QNP_TEST(ReplayV2ConfirmedTimelineAcceptsOnlyExactCompletedBoundary) {
    CompleteGameV2 game = completeGameV2();
    ConfirmedTimelineBufferV2 buffer(game.ticks.size());
    ParseStatus why = ParseStatus::Ok;
    QNP_REQUIRE(buffer.begin(game.start, why));
    for (const CanonicalTickBundleV2& tick : game.ticks)
        QNP_REQUIRE(buffer.append(tick, why));
    QNP_CHECK(buffer.append(game.ticks[0], why));
    QNP_REQUIRE(buffer.seal(game.end, why));
    QNP_CHECK(buffer.seal(game.end, why));
    QNP_CHECK(buffer.size() == 3u);

    ConfirmedTimelineBufferV2 bad(game.ticks.size());
    QNP_REQUIRE(bad.begin(game.start, why));
    QNP_REQUIRE(bad.append(game.ticks[0], why));
    CanonicalTickBundleV2 gap = game.ticks[2];
    QNP_CHECK(!bad.append(gap, why));
    QNP_CHECK(why == ParseStatus::FrameGap);

    ConfirmedTimelineBufferV2 wrongBoundary(game.ticks.size());
    QNP_REQUIRE(wrongBoundary.begin(game.start, why));
    for (const CanonicalTickBundleV2& tick : game.ticks)
        QNP_REQUIRE(wrongBoundary.append(tick, why));
    ConfirmedGameEndV2 wrongEnd = game.end;
    wrongEnd.nextRecord = 2u;
    QNP_CHECK(!wrongBoundary.seal(wrongEnd, why));
    QNP_CHECK(why == ParseStatus::FrameGap);

    ConfirmedTimelineBufferV2 afterResult(game.ticks.size() + 1u);
    QNP_REQUIRE(afterResult.begin(game.start, why));
    for (const CanonicalTickBundleV2& tick : game.ticks)
        QNP_REQUIRE(afterResult.append(tick, why));
    CanonicalTickBundleV2 impossible = frameV2(
        503u, 3u, game.ticks.back().frame.core.after,
        game.ticks.back().frame.core.after, 0x4000u, 0x5000u);
    QNP_CHECK(!afterResult.append(impossible, why));
    QNP_CHECK(why == ParseStatus::FrameGap);
    QNP_CHECK(afterResult.validator().lastFault()
              == ConfirmedTimelineFaultV2::None);

    ConfirmedTimelineBufferV2 zeroCapacity(0u);
    QNP_CHECK(!zeroCapacity.validCapacity());
    QNP_CHECK(!zeroCapacity.begin(game.start, why));
    QNP_CHECK(why == ParseStatus::LimitExceeded);
    ConfirmedTimelineBufferV2 excessiveCapacity(
        static_cast<size_t>(kMaxPlaybackPrebufferFramesV2) + 1u);
    QNP_CHECK(!excessiveCapacity.validCapacity());
    QNP_CHECK(excessiveCapacity.capacity() == 0u);
    QNP_CHECK(!excessiveCapacity.begin(game.start, why));
    QNP_CHECK(why == ParseStatus::LimitExceeded);
}

QNP_TEST(ReplayV2OutcomeContractSupportsDrawsAndTeamWinnerSlots) {
    CompleteGameV2 draw = completeGameV2(true);
    ConfirmedTimelineBufferV2 timeline(draw.ticks.size());
    ParseStatus why = ParseStatus::Ok;
    QNP_REQUIRE(timeline.begin(draw.start, why));
    for (const CanonicalTickBundleV2& tick : draw.ticks)
        QNP_REQUIRE(timeline.append(tick, why));
    QNP_CHECK(timeline.seal(draw.end, why));
    QNP_CHECK(draw.end.winningSideMask == 0u);
    QNP_CHECK(draw.end.winnerSlot == -1);

    CompleteGameV2 game = completeGameV2();
    ConfirmedGameEventV2 teamEvent =
        game.ticks.back().postEvents.events[0];
    teamEvent.winningSideMask = 0x01u;
    teamEvent.winnerSlot = 2;
    QNP_CHECK(isValidConfirmedGameEventV2(teamEvent));
    teamEvent.winningSideMask = 0x04u;
    QNP_CHECK(!isValidConfirmedGameEventV2(teamEvent));

    ConfirmedGameEndV2 teamEnd = game.end;
    teamEnd.winningSideMask = 0x02u;
    teamEnd.winnerSlot = 3;
    QNP_CHECK(isValidConfirmedGameEndV2(teamEnd));
    teamEnd.winningSideMask = 0x08u;
    QNP_CHECK(!isValidConfirmedGameEndV2(teamEnd));
}

QNP_TEST(ReplayWireV3BindsLaunchAndEveryGameTransaction) {
    wire3::Frame frame{};
    frame.channel = wire3::Channel::Recorder;
    frame.type = wire3::MessageType::RecorderGameBegin;
    frame.launchGeneration = 9u;
    frame.launchNonce = 0x1122334455667788ull;
    frame.game.gameGeneration = 7u;
    frame.game.gameUuidLow = 1u;
    frame.sequence = 3u;
    frame.param0 = 0u;
    frame.param1 = 1u;
    frame.payload = {1u, 2u, 3u, 4u};
    std::vector<uint8_t> wire;
    QNP_REQUIRE(wire3::encodeFrame(frame, wire));
    QNP_CHECK(wire.size() == wire3::kHeaderBytes + frame.payload.size());
    QNP_CHECK(wire[0] == 'Q' && wire[1] == 'R'
              && wire[2] == 'P' && wire[3] == '3');
    wire3::Frame decoded{};
    size_t consumed = 0u;
    QNP_CHECK(wire3::decodeFrame(
        wire.data(), wire.size(), decoded, consumed)
        == wire3::FrameStatus::Ok);
    QNP_CHECK(consumed == wire.size());
    QNP_CHECK(decoded.launchNonce == frame.launchNonce);
    QNP_CHECK(decoded.game == frame.game);

    wire3::Frame missingNonce = frame;
    missingNonce.launchNonce = 0u;
    std::vector<uint8_t> rejected;
    QNP_CHECK(!wire3::encodeFrame(missingNonce, rejected));
    wire3::Frame missingGame = frame;
    missingGame.game = GameIdentityV2{};
    QNP_CHECK(!wire3::encodeFrame(missingGame, rejected));

    wire[wire3::kHeaderBytes] ^= 1u;
    QNP_CHECK(wire3::decodeFrame(
        wire.data(), wire.size(), decoded, consumed)
        == wire3::FrameStatus::BadPayloadCrc);

    wire3::Frame unknown = frame;
    unknown.type = static_cast<wire3::MessageType>(0xFFu);
    QNP_CHECK(!wire3::encodeFrame(unknown, rejected));

    std::vector<uint8_t> validWire;
    QNP_REQUIRE(wire3::encodeFrame(frame, validWire));
    everyTruncationFailsV2(
        validWire, [&](const uint8_t* p, size_t n) {
            wire3::Frame out{};
            size_t used = 0u;
            const wire3::FrameStatus status =
                wire3::decodeFrame(p, n, out, used);
            return status == wire3::FrameStatus::Ok
                ? ParseStatus::Ok : ParseStatus::Truncated;
        });
}

QNP_TEST(ReplayWireV3RecorderControlPayloadsAreCanonical) {
    std::vector<uint8_t> bytes;
    wire3::RecorderHelloPayload hello{};
    hello.gamePid = 1234u;
    hello.runtimeCompatibilityId = 0x1122334455667788ull;
    hello.queueCapacity = 512u;
    QNP_REQUIRE(wire3::encodeRecorderHello(hello, bytes));
    QNP_CHECK(bytes.size() == wire3::kRecorderHelloPayloadBytes);
    wire3::RecorderHelloPayload helloOut{};
    QNP_CHECK(wire3::decodeRecorderHello(
        bytes.data(), bytes.size(), helloOut));
    QNP_CHECK(helloOut.gamePid == hello.gamePid);
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::RecorderHelloPayload out{};
        return wire3::decodeRecorderHello(p, n, out);
    });
    std::vector<uint8_t> hostile = bytes;
    hostile.back() = 1u;
    QNP_CHECK(!wire3::decodeRecorderHello(
        hostile.data(), hostile.size(), helloOut));
    wire3::RecorderHelloPayload badHello = hello;
    badHello.featureBits = 0u;
    hostile.clear();
    QNP_CHECK(!wire3::encodeRecorderHello(badHello, hostile));

    wire3::RecorderDrainPayload drain{};
    drain.nextPublisherSequence = 77u;
    drain.gamesBegun = 3u;
    drain.gamesEnded = 2u;
    drain.abandonedOpenGame = true;
    bytes.clear();
    QNP_REQUIRE(wire3::encodeRecorderDrain(drain, bytes));
    wire3::RecorderDrainPayload drainOut{};
    QNP_CHECK(wire3::decodeRecorderDrain(
        bytes.data(), bytes.size(), drainOut));
    QNP_CHECK(drainOut.gamesEnded == 2u);
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::RecorderDrainPayload out{};
        return wire3::decodeRecorderDrain(p, n, out);
    });
    hostile = bytes;
    hostile[13] = 1u;
    QNP_CHECK(!wire3::decodeRecorderDrain(
        hostile.data(), hostile.size(), drainOut));
    wire3::RecorderDrainPayload badDrain = drain;
    badDrain.gamesEnded = 1u;
    hostile.clear();
    QNP_CHECK(!wire3::encodeRecorderDrain(badDrain, hostile));

    wire3::RecorderByePayload bye{};
    bye.reason = wire3::RecorderByeReason::NormalAfterDrain;
    bye.drainCompleted = true;
    bye.nextPublisherSequence = 78u;
    bytes.clear();
    QNP_REQUIRE(wire3::encodeRecorderBye(bye, bytes));
    wire3::RecorderByePayload byeOut{};
    QNP_CHECK(wire3::decodeRecorderBye(
        bytes.data(), bytes.size(), byeOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::RecorderByePayload out{};
        return wire3::decodeRecorderBye(p, n, out);
    });
    hostile = bytes;
    hostile[0] = 0u;
    QNP_CHECK(!wire3::decodeRecorderBye(
        hostile.data(), hostile.size(), byeOut));
    hostile = bytes;
    hostile[3] = 1u;
    QNP_CHECK(!wire3::decodeRecorderBye(
        hostile.data(), hostile.size(), byeOut));
}

QNP_TEST(ReplayWireV3NextGameCarriesPinnedBindingAndRejectsMalformedPayloads) {
    const auto start = sampleStartV2();
    wire3::PlaybackNextGamePayload next{};
    next.switchId = 7u;
    next.game = start.manifest.identity;
    next.frameCount = 3u;
    next.eventCount = 4u;
    next.validatedSourceByteCount = 999u;
    next.validatedSourceRangeDigest.fill(0x33u);
    next.endpointKind = wire3::PlaybackEndpointKind::RecordedGameEnd;
    next.supersededStateSchema = true;
    next.presentationFlags = 31u;
    next.runtimeContract = start.manifest.contract;
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(wire3::encodePlaybackNextGame(next, bytes));
    QNP_CHECK(bytes.size() == 224u);
    QNP_CHECK(bytes[0] == 7u && bytes[76] == 1u
              && bytes[77] == 1u && bytes[78] == 31u);
    wire3::PlaybackNextGamePayload decoded{};
    QNP_REQUIRE(wire3::decodePlaybackNextGame(
        bytes.data(), bytes.size(), decoded));
    QNP_CHECK(decoded.switchId == 7u && decoded.game == next.game);
    QNP_CHECK(decoded.frameCount == 3u && decoded.eventCount == 4u);
    QNP_CHECK(decoded.runtimeContract == next.runtimeContract);
    QNP_CHECK(decoded.supersededStateSchema && decoded.presentationFlags == 31u);
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        return wire3::decodePlaybackNextGame(p, n, decoded);
    });
    for (const size_t offset : {0u, 8u, 28u, 36u, 44u, 76u, 77u, 78u}) {
        auto hostile = bytes;
        if (offset == 77u) hostile[offset] = 2u;
        else if (offset == 78u) hostile[offset] = 32u;
        else if (offset == 36u)
            std::fill(hostile.begin() + 36u, hostile.begin() + 44u, 0u);
        else if (offset == 44u)
            std::fill(hostile.begin() + 44u, hostile.begin() + 76u, 0u);
        else hostile[offset] = 0u;
        QNP_CHECK(!wire3::decodePlaybackNextGame(
            hostile.data(), hostile.size(), decoded));
    }
    wire3::Frame frame{};
    frame.channel = wire3::Channel::Playback;
    frame.launchGeneration = 1u;
    frame.launchNonce = 2u;
    frame.game = start.manifest.identity;
    frame.sequence = 1u;
    frame.payload = bytes;
    for (auto type : {wire3::MessageType::PlaybackNextGame,
                      wire3::MessageType::PlaybackNextGameAck}) {
        frame.type = type;
        std::vector<uint8_t> wire;
        QNP_CHECK(wire3::encodeFrame(frame, wire));
        auto unbound = frame;
        unbound.game = {};
        wire.clear();
        QNP_CHECK(!wire3::encodeFrame(unbound, wire));
    }
}

QNP_TEST(ReplayWireV3PlaybackPreflightAndTimelinePayloadsAreBounded) {
    CompleteGameV2 game = completeGameV2();
    std::vector<uint8_t> bytes;

    wire3::PlaybackHelloPayload hello{};
    hello.gamePid = 4321u;
    hello.runtimeCompatibilityId =
        game.start.manifest.contract.runtimeSimulationId;
    hello.stateHashAlgorithmId =
        game.start.manifest.contract.stateHashAlgorithmId;
    hello.bootstrapSchemaId =
        game.start.manifest.contract.bootstrapSchemaId;
    QNP_REQUIRE(wire3::encodePlaybackHello(hello, bytes));
    QNP_CHECK(bytes.size() == wire3::kPlaybackHelloPayloadBytes);
    wire3::PlaybackHelloPayload helloOut{};
    QNP_CHECK(wire3::decodePlaybackHello(
        bytes.data(), bytes.size(), helloOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::PlaybackHelloPayload out{};
        return wire3::decodePlaybackHello(p, n, out);
    });
    std::vector<uint8_t> hostile = bytes;
    hostile[7] = 1u;
    QNP_CHECK(!wire3::decodePlaybackHello(
        hostile.data(), hostile.size(), helloOut));

    wire3::PlaybackEndpointPayload endpoint{};
    endpoint.kind = wire3::PlaybackEndpointKind::RecordedGameEnd;
    endpoint.hasRecordedGameEnd = true;
    endpoint.nextRecord = game.end.nextRecord;
    endpoint.postTailStateHash = game.end.postTailStateHash;
    endpoint.finalRollingDigest = game.end.finalRollingDigest;
    endpoint.validatedSourceByteCount = 999u;
    endpoint.validatedSourceRangeDigest.fill(0x33u);
    endpoint.recordedGameEnd = game.end;
    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackEndpoint(endpoint, bytes));
    wire3::PlaybackEndpointPayload endpointOut{};
    QNP_CHECK(wire3::decodePlaybackEndpoint(
        bytes.data(), bytes.size(), endpointOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::PlaybackEndpointPayload out{};
        return wire3::decodePlaybackEndpoint(p, n, out);
    });
    hostile = bytes;
    hostile[2] = 1u;
    QNP_CHECK(!wire3::decodePlaybackEndpoint(
        hostile.data(), hostile.size(), endpointOut));

    wire3::PlaybackSessionInfoPayload info{};
    info.frameCount = static_cast<uint32_t>(game.ticks.size());
    info.eventCount = 4u;
    info.activeLaneMask = game.start.manifest.activeLaneMask;
    info.activeFighterMask = game.start.manifest.activeFighterMask;
    info.hasFileDigest = true;
    info.hasCanonicalGameDigest = true;
    info.fileDigest.fill(0x44u);
    info.canonicalGameDigest = game.end.canonicalGameDigest;
    info.validatedSourceByteCount = endpoint.validatedSourceByteCount;
    info.validatedSourceRangeDigest =
        endpoint.validatedSourceRangeDigest;
    info.recordedContract = game.start.manifest.contract;
    info.endpoint = endpoint;
    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackSessionInfo(info, bytes));
    QNP_CHECK(bytes.size() == wire3::kPlaybackSessionInfoPayloadBytes);
    wire3::PlaybackSessionInfoPayload infoOut{};
    QNP_CHECK(wire3::decodePlaybackSessionInfo(
        bytes.data(), bytes.size(), infoOut));
    QNP_CHECK(infoOut.frameCount == info.frameCount);
    QNP_CHECK(infoOut.endpoint.recordedGameEnd == game.end);
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::PlaybackSessionInfoPayload out{};
        return wire3::decodePlaybackSessionInfo(p, n, out);
    });
    hostile = bytes;
    hostile[6] = 1u;
    QNP_CHECK(!wire3::decodePlaybackSessionInfo(
        hostile.data(), hostile.size(), infoOut));
    hostile = bytes;
    hostile[22] |= 0x80u;
    QNP_CHECK(!wire3::decodePlaybackSessionInfo(
        hostile.data(), hostile.size(), infoOut));

    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackGameStart(game.start, bytes));
    ConfirmedGameStartV2 startOut{};
    QNP_CHECK(wire3::decodePlaybackGameStart(
        bytes.data(), bytes.size(), startOut) == ParseStatus::Ok);
    everyTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        ConfirmedGameStartV2 out{};
        return wire3::decodePlaybackGameStart(p, n, out);
    });

    std::vector<ConfirmedGameFrameV2> frames;
    std::vector<ConfirmedGameEventV2> events;
    for (const CanonicalTickBundleV2& tick : game.ticks) {
        frames.push_back(tick.frame);
        for (size_t i = 0u; i < tick.preEvents.count; ++i)
            events.push_back(tick.preEvents.events[i]);
        for (size_t i = 0u; i < tick.postEvents.count; ++i)
            events.push_back(tick.postEvents.events[i]);
    }
    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackFrames(
        frames.data(), frames.size(), bytes));
    std::vector<ConfirmedGameFrameV2> framesOut;
    QNP_CHECK(wire3::decodePlaybackFrames(
        bytes.data(), bytes.size(), 0u,
        static_cast<uint32_t>(frames.size()), 0x03u, framesOut)
        == ParseStatus::Ok);
    everyTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        std::vector<ConfirmedGameFrameV2> out;
        return wire3::decodePlaybackFrames(
            p, n, 0u, static_cast<uint32_t>(frames.size()),
            0x03u, out);
    });

    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackEvents(
        events.data(), events.size(), bytes));
    std::vector<ConfirmedGameEventV2> eventsOut;
    QNP_CHECK(wire3::decodePlaybackEvents(
        bytes.data(), bytes.size(), 0u,
        static_cast<uint32_t>(events.size()), eventsOut)
        == ParseStatus::Ok);
    everyTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        std::vector<ConfirmedGameEventV2> out;
        return wire3::decodePlaybackEvents(
            p, n, 0u, static_cast<uint32_t>(events.size()), out);
    });
}

QNP_TEST(ReplayWireV3TransportCommandNumbersAreFrozen) {
    QNP_CHECK(static_cast<uint8_t>(wire3::PlaybackCommandKind::Pause) == 1u);
    QNP_CHECK(static_cast<uint8_t>(wire3::PlaybackCommandKind::Resume) == 2u);
    QNP_CHECK(static_cast<uint8_t>(wire3::PlaybackCommandKind::Step) == 3u);
    QNP_CHECK(
        static_cast<uint8_t>(wire3::PlaybackCommandKind::SpeedSet) == 4u);
    QNP_CHECK(static_cast<uint8_t>(wire3::PlaybackCommandKind::Restart) == 5u);
    QNP_CHECK(
        static_cast<uint8_t>(wire3::PlaybackCommandKind::SeekFrame) == 6u);
    QNP_CHECK(
        static_cast<uint8_t>(wire3::PlaybackCommandKind::SeekRound) == 7u);
    QNP_CHECK(static_cast<uint8_t>(wire3::PlaybackCommandKind::Exit) == 8u);
    QNP_CHECK(!wire3::isKnownPlaybackCommandKind(0u));
    QNP_CHECK(!wire3::isKnownPlaybackCommandKind(9u));
    QNP_CHECK(wire3::kPlaybackCommandPayloadBytes == 24u);

    QNP_CHECK(wire3::isPresentationOnlyPlaybackCommandKind(
        wire3::PlaybackCommandKind::Pause));
    QNP_CHECK(wire3::isPresentationOnlyPlaybackCommandKind(
        wire3::PlaybackCommandKind::Resume));
    QNP_CHECK(wire3::isPresentationOnlyPlaybackCommandKind(
        wire3::PlaybackCommandKind::Step));
    QNP_CHECK(wire3::isPresentationOnlyPlaybackCommandKind(
        wire3::PlaybackCommandKind::SpeedSet));
    QNP_CHECK(!wire3::isPresentationOnlyPlaybackCommandKind(
        wire3::PlaybackCommandKind::Restart));
    QNP_CHECK(!wire3::isPresentationOnlyPlaybackCommandKind(
        wire3::PlaybackCommandKind::SeekFrame));
    QNP_CHECK(!wire3::isPresentationOnlyPlaybackCommandKind(
        wire3::PlaybackCommandKind::SeekRound));
    QNP_CHECK(!wire3::isPresentationOnlyPlaybackCommandKind(
        wire3::PlaybackCommandKind::Exit));

    QNP_CHECK(wire3::isValidPlaybackCommandArgument(
        wire3::PlaybackCommandKind::Pause, 0u));
    QNP_CHECK(!wire3::isValidPlaybackCommandArgument(
        wire3::PlaybackCommandKind::Pause, 1u));
    QNP_CHECK(!wire3::isValidPlaybackCommandArgument(
        wire3::PlaybackCommandKind::Step, 1u));
    QNP_CHECK(!wire3::isValidPlaybackCommandArgument(
        wire3::PlaybackCommandKind::SpeedSet, 0u));
    QNP_CHECK(!wire3::isValidPlaybackCommandArgument(
        wire3::PlaybackCommandKind::SpeedSet, 750u));
    QNP_CHECK(!wire3::isValidPlaybackCommandArgument(
        wire3::PlaybackCommandKind::SpeedSet, 16000u));
    {
        wire3::PlaybackCommandPayload refused{};
        refused.commandId = 1u;
        refused.kind = wire3::PlaybackCommandKind::SpeedSet;
        refused.argument = 750u;
        std::vector<uint8_t> bytes;
        QNP_CHECK(!wire3::encodePlaybackCommand(refused, bytes));
        refused.argument = 1000u;
        refused.commandId = 0u;
        bytes.clear();
        QNP_CHECK(!wire3::encodePlaybackCommand(refused, bytes));
    }

    uint32_t stepped = 0u;
    QNP_CHECK(wire3::nextPlaybackSpeedV3(1000u, true, stepped));
    QNP_CHECK(stepped == 2000u);
    QNP_CHECK(wire3::nextPlaybackSpeedV3(1000u, false, stepped));
    QNP_CHECK(stepped == 500u);
    QNP_CHECK(wire3::nextPlaybackSpeedV3(8000u, true, stepped));
    QNP_CHECK(stepped == 8000u);
    QNP_CHECK(wire3::nextPlaybackSpeedV3(250u, false, stepped));
    QNP_CHECK(stepped == 250u);
    stepped = 0xDEADu;
    QNP_CHECK(!wire3::nextPlaybackSpeedV3(750u, true, stepped));
    QNP_CHECK(stepped == 0xDEADu);
    QNP_CHECK(!wire3::nextPlaybackSpeedV3(0u, false, stepped));
    for (uint32_t rung : wire3::kPlaybackSpeedLadderV3) {
        QNP_CHECK(wire3::isSupportedPlaybackSpeedV3(rung));
        QNP_CHECK(wire3::nextPlaybackSpeedV3(rung, true, stepped));
        QNP_CHECK(wire3::isSupportedPlaybackSpeedV3(stepped));
        QNP_CHECK(wire3::nextPlaybackSpeedV3(rung, false, stepped));
        QNP_CHECK(wire3::isSupportedPlaybackSpeedV3(stepped));
    }
}

QNP_TEST(ReplayWireV3EveryTransportCommandRoundTripsAndFailsClosed) {
    struct TransportSample {
        wire3::PlaybackCommandKind kind;
        uint64_t argument;
    };
    static const TransportSample kSamples[] = {
        {wire3::PlaybackCommandKind::Pause, 0u},
        {wire3::PlaybackCommandKind::Resume, 0u},
        {wire3::PlaybackCommandKind::Step, 0u},
        {wire3::PlaybackCommandKind::SpeedSet, 250u},
        {wire3::PlaybackCommandKind::SpeedSet, 500u},
        {wire3::PlaybackCommandKind::SpeedSet, 1000u},
        {wire3::PlaybackCommandKind::SpeedSet, 2000u},
        {wire3::PlaybackCommandKind::SpeedSet, 4000u},
        {wire3::PlaybackCommandKind::SpeedSet, 8000u},
        {wire3::PlaybackCommandKind::Restart, 0u},
        {wire3::PlaybackCommandKind::SeekFrame, 0u},
        {wire3::PlaybackCommandKind::SeekFrame,
         kMaxPlaybackPrebufferFramesV2 - 1u},
        {wire3::PlaybackCommandKind::SeekRound, 1u},
        {wire3::PlaybackCommandKind::SeekRound,
         kMaxPlaybackPrebufferFramesV2},
        {wire3::PlaybackCommandKind::Exit, 0u},
    };

    const auto readU64 = [](const std::vector<uint8_t>& bytes, size_t at) {
        uint64_t value = 0u;
        for (size_t i = 0u; i < 8u; ++i)
            value |= static_cast<uint64_t>(bytes[at + i]) << (8u * i);
        return value;
    };

    for (const TransportSample& sample : kSamples) {
        wire3::PlaybackCommandPayload command{};
        command.commandId = 0x0102030405060708ull;
        command.kind = sample.kind;
        command.argument = sample.argument;
        std::vector<uint8_t> bytes;
        QNP_REQUIRE(wire3::encodePlaybackCommand(command, bytes));
        QNP_REQUIRE(bytes.size() == wire3::kPlaybackCommandPayloadBytes);

        QNP_CHECK(readU64(bytes, 0u) == command.commandId);
        QNP_CHECK(bytes[8] == static_cast<uint8_t>(sample.kind));
        for (size_t i = 9u; i < 16u; ++i) QNP_CHECK(bytes[i] == 0u);
        QNP_CHECK(readU64(bytes, 16u) == sample.argument);

        wire3::PlaybackCommandPayload decoded{};
        QNP_REQUIRE(wire3::decodePlaybackCommand(
            bytes.data(), bytes.size(), decoded));
        QNP_CHECK(decoded.commandId == command.commandId);
        QNP_CHECK(decoded.kind == command.kind);
        QNP_CHECK(decoded.argument == command.argument);
        std::vector<uint8_t> again;
        QNP_CHECK(wire3::encodePlaybackCommand(decoded, again));
        QNP_CHECK(again == bytes);

        QNP_CHECK(!wire3::decodePlaybackCommand(
            nullptr, wire3::kPlaybackCommandPayloadBytes, decoded));
        std::vector<uint8_t> longer = bytes;
        longer.push_back(0u);
        QNP_CHECK(!wire3::decodePlaybackCommand(
            longer.data(), longer.size(), decoded));
        std::vector<uint8_t> shorter(bytes.begin(), bytes.end() - 1);
        QNP_CHECK(!wire3::decodePlaybackCommand(
            shorter.data(), shorter.size(), decoded));
        for (size_t i = 9u; i < 16u; ++i) {
            std::vector<uint8_t> hostile = bytes;
            hostile[i] = 0x5Au;
            QNP_CHECK(!wire3::decodePlaybackCommand(
                hostile.data(), hostile.size(), decoded));
        }
        std::vector<uint8_t> zeroId = bytes;
        for (size_t i = 0u; i < 8u; ++i) zeroId[i] = 0u;
        QNP_CHECK(!wire3::decodePlaybackCommand(
            zeroId.data(), zeroId.size(), decoded));

        for (unsigned value = 0u; value <= 255u; ++value) {
            if (value >= 1u && value <= 8u) {
                QNP_CHECK(wire3::isKnownPlaybackCommandKind(
                    static_cast<uint8_t>(value)));
                continue;
            }
            QNP_CHECK(!wire3::isKnownPlaybackCommandKind(
                static_cast<uint8_t>(value)));
            std::vector<uint8_t> hostile = bytes;
            hostile[8] = static_cast<uint8_t>(value);
            QNP_CHECK(!wire3::decodePlaybackCommand(
                hostile.data(), hostile.size(), decoded));
        }
    }

    static const uint64_t kOffLadderSpeeds[] = {
        0u, 1u, 249u, 251u, 499u, 750u, 999u, 1001u, 1500u, 3000u, 5000u,
        7999u, 8001u, 16000u, 0xFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
    };
    for (const uint64_t speed : kOffLadderSpeeds) {
        QNP_CHECK(!wire3::isSupportedPlaybackSpeedV3(speed));
        QNP_CHECK(!wire3::isValidPlaybackCommandArgument(
            wire3::PlaybackCommandKind::SpeedSet, speed));
        wire3::PlaybackCommandPayload refused{};
        refused.commandId = 3u;
        refused.kind = wire3::PlaybackCommandKind::SpeedSet;
        refused.argument = speed;
        std::vector<uint8_t> bytes;
        QNP_CHECK(!wire3::encodePlaybackCommand(refused, bytes));
        QNP_CHECK(bytes.empty());

        refused.argument = 1000u;
        bytes.clear();
        QNP_REQUIRE(wire3::encodePlaybackCommand(refused, bytes));
        for (size_t i = 0u; i < 8u; ++i)
            bytes[16u + i] = static_cast<uint8_t>((speed >> (8u * i)) & 0xFFu);
        wire3::PlaybackCommandPayload decoded{};
        QNP_CHECK(!wire3::decodePlaybackCommand(
            bytes.data(), bytes.size(), decoded));
    }

    static const wire3::PlaybackCommandKind kArgumentless[] = {
        wire3::PlaybackCommandKind::Pause,
        wire3::PlaybackCommandKind::Resume,
        wire3::PlaybackCommandKind::Step,
        wire3::PlaybackCommandKind::Restart,
        wire3::PlaybackCommandKind::Exit,
    };
    for (const wire3::PlaybackCommandKind kind : kArgumentless) {
        wire3::PlaybackCommandPayload command{};
        command.commandId = 11u;
        command.kind = kind;
        std::vector<uint8_t> bytes;
        QNP_REQUIRE(wire3::encodePlaybackCommand(command, bytes));
        bytes[16] = 1u;
        wire3::PlaybackCommandPayload decoded{};
        QNP_CHECK(!wire3::decodePlaybackCommand(
            bytes.data(), bytes.size(), decoded));
        QNP_CHECK(!wire3::isValidPlaybackCommandArgument(kind, 1u));
    }
    QNP_CHECK(!wire3::isValidPlaybackCommandArgument(
        wire3::PlaybackCommandKind::SeekFrame,
        kMaxPlaybackPrebufferFramesV2));
    QNP_CHECK(!wire3::isValidPlaybackCommandArgument(
        wire3::PlaybackCommandKind::SeekRound, 0u));
    QNP_CHECK(!wire3::isValidPlaybackCommandArgument(
        wire3::PlaybackCommandKind::SeekRound,
        kMaxPlaybackPrebufferFramesV2 + 1u));
}

QNP_TEST(ReplayWireV3TransportAckAndStatusAreClosedOverTheSpeedLadder) {
    for (unsigned kindValue = 1u; kindValue <= 8u; ++kindValue) {
        const wire3::PlaybackCommandKind kind =
            static_cast<wire3::PlaybackCommandKind>(kindValue);
        const uint64_t argument =
            kind == wire3::PlaybackCommandKind::SpeedSet ? 4000u
            : kind == wire3::PlaybackCommandKind::SeekRound ? 1u
            : 0u;
        for (unsigned resultValue = 1u; resultValue <= 6u; ++resultValue) {
            wire3::PlaybackCommandAckPayload ack{};
            ack.commandId = 0x1122334455667788ull;
            ack.kind = kind;
            ack.result =
                static_cast<wire3::PlaybackCommandAckResult>(resultValue);
            ack.phase = wire3::PlaybackRuntimePhase::Paused;
            ack.effectiveArgument = argument;
            ack.nextRecord = 7u;
            std::vector<uint8_t> bytes;
            QNP_REQUIRE(wire3::encodePlaybackCommandAck(ack, bytes));
            QNP_CHECK(bytes.size() == wire3::kPlaybackCommandAckPayloadBytes);
            wire3::PlaybackCommandAckPayload decoded{};
            QNP_REQUIRE(wire3::decodePlaybackCommandAck(
                bytes.data(), bytes.size(), decoded));
            QNP_CHECK(decoded.commandId == ack.commandId);
            QNP_CHECK(decoded.kind == ack.kind);
            QNP_CHECK(decoded.result == ack.result);
            QNP_CHECK(decoded.phase == ack.phase);
            QNP_CHECK(decoded.effectiveArgument == ack.effectiveArgument);
            QNP_CHECK(decoded.nextRecord == ack.nextRecord);
            QNP_CHECK(bytes[8] == static_cast<uint8_t>(kindValue));
            for (unsigned unknown = 9u; unknown <= 255u; unknown += 37u) {
                std::vector<uint8_t> hostile = bytes;
                hostile[8] = static_cast<uint8_t>(unknown);
                QNP_CHECK(!wire3::decodePlaybackCommandAck(
                    hostile.data(), hostile.size(), decoded));
            }
        }

        wire3::PlaybackCommandAckPayload applied{};
        applied.commandId = 5u;
        applied.kind = kind;
        applied.result = wire3::PlaybackCommandAckResult::Applied;
        applied.phase = wire3::PlaybackRuntimePhase::Playing;
        applied.effectiveArgument = 750u;
        std::vector<uint8_t> bytes;
        const bool sevenFiftyIsLegal =
            wire3::isValidPlaybackCommandArgument(kind, 750u);
        QNP_CHECK(wire3::encodePlaybackCommandAck(applied, bytes)
                  == sevenFiftyIsLegal);
        applied.result = wire3::PlaybackCommandAckResult::RejectedArgument;
        bytes.clear();
        QNP_CHECK(wire3::encodePlaybackCommandAck(applied, bytes));
    }

    for (const uint32_t rung : wire3::kPlaybackSpeedLadderV3) {
        for (int paused = 0; paused < 2; ++paused) {
            wire3::PlaybackStatusPayload status{};
            status.phase = paused != 0
                ? wire3::PlaybackRuntimePhase::Paused
                : wire3::PlaybackRuntimePhase::Playing;
            status.paused = paused != 0;
            status.nextRecord = 2u;
            status.endpointNextRecord = 3u;
            status.bufferedFrames = 1u;
            status.speedPermille = rung;
            status.rollingDigest = 0xFEEDFACECAFEBEEFull;
            std::vector<uint8_t> bytes;
            QNP_REQUIRE(wire3::encodePlaybackStatus(status, bytes));
            wire3::PlaybackStatusPayload decoded{};
            QNP_REQUIRE(wire3::decodePlaybackStatus(
                bytes.data(), bytes.size(), decoded));
            QNP_CHECK(decoded.paused == status.paused);
            QNP_CHECK(decoded.speedPermille == rung);
            QNP_CHECK(decoded.phase == status.phase);
            QNP_CHECK(decoded.rollingDigest == status.rollingDigest);
        }
    }
    {
        wire3::PlaybackStatusPayload status{};
        status.phase = wire3::PlaybackRuntimePhase::Playing;
        status.nextRecord = 1u;
        status.endpointNextRecord = 3u;
        status.speedPermille = 1000u;
        std::vector<uint8_t> bytes;
        QNP_REQUIRE(wire3::encodePlaybackStatus(status, bytes));
        static const uint32_t kOffLadder[] = {
            0u, 750u, 1500u, 3000u, 16000u,
        };
        for (const uint32_t bad : kOffLadder) {
            wire3::PlaybackStatusPayload refused = status;
            refused.speedPermille = bad;
            std::vector<uint8_t> out;
            QNP_CHECK(!wire3::encodePlaybackStatus(refused, out));
            std::vector<uint8_t> hostile = bytes;
            for (size_t i = 0u; i < 4u; ++i) {
                hostile[20u + i] =
                    static_cast<uint8_t>((bad >> (8u * i)) & 0xFFu);
            }
            wire3::PlaybackStatusPayload decoded{};
            QNP_CHECK(!wire3::decodePlaybackStatus(
                hostile.data(), hostile.size(), decoded));
        }
    }
}

QNP_TEST(ReplayWireV3PlaybackCommandsStatusAndByeAreCanonical) {
    std::vector<uint8_t> bytes;
    wire3::PlaybackCommandPayload command{};
    command.commandId = 9u;
    command.kind = wire3::PlaybackCommandKind::SpeedSet;
    command.argument = 2000u;
    QNP_REQUIRE(wire3::encodePlaybackCommand(command, bytes));
    wire3::PlaybackCommandPayload commandOut{};
    QNP_CHECK(wire3::decodePlaybackCommand(
        bytes.data(), bytes.size(), commandOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::PlaybackCommandPayload out{};
        return wire3::decodePlaybackCommand(p, n, out);
    });
    std::vector<uint8_t> hostile = bytes;
    hostile[8] = 0u;
    QNP_CHECK(!wire3::decodePlaybackCommand(
        hostile.data(), hostile.size(), commandOut));
    hostile = bytes;
    hostile[9] = 1u;
    QNP_CHECK(!wire3::decodePlaybackCommand(
        hostile.data(), hostile.size(), commandOut));

    wire3::PlaybackCommandAckPayload ack{};
    ack.commandId = command.commandId;
    ack.kind = command.kind;
    ack.result = wire3::PlaybackCommandAckResult::Applied;
    ack.phase = wire3::PlaybackRuntimePhase::Playing;
    ack.effectiveArgument = command.argument;
    ack.nextRecord = 1u;
    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackCommandAck(ack, bytes));
    wire3::PlaybackCommandAckPayload ackOut{};
    QNP_CHECK(wire3::decodePlaybackCommandAck(
        bytes.data(), bytes.size(), ackOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::PlaybackCommandAckPayload out{};
        return wire3::decodePlaybackCommandAck(p, n, out);
    });
    hostile = bytes;
    hostile[10] = 0u;
    QNP_CHECK(!wire3::decodePlaybackCommandAck(
        hostile.data(), hostile.size(), ackOut));
    hostile = bytes;
    hostile[11] = 1u;
    QNP_CHECK(!wire3::decodePlaybackCommandAck(
        hostile.data(), hostile.size(), ackOut));

    wire3::PlaybackStatusPayload status{};
    status.phase = wire3::PlaybackRuntimePhase::Playing;
    status.nextRecord = 1u;
    status.endpointNextRecord = 3u;
    status.nextEventSequence = 2u;
    status.bufferedFrames = 2u;
    status.speedPermille = 2000u;
    status.presentationSerial = 5u;
    status.rollingDigest = 0x1234u;
    status.postStateHash = 0x5678u;
    status.lastCommandId = command.commandId;
    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackStatus(status, bytes));
    QNP_CHECK(bytes.size() == wire3::kPlaybackStatusPayloadBytes);
    wire3::PlaybackStatusPayload statusOut{};
    QNP_CHECK(wire3::decodePlaybackStatus(
        bytes.data(), bytes.size(), statusOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::PlaybackStatusPayload out{};
        return wire3::decodePlaybackStatus(p, n, out);
    });
    hostile = bytes;
    hostile[1] |= 0x80u;
    QNP_CHECK(!wire3::decodePlaybackStatus(
        hostile.data(), hostile.size(), statusOut));
    hostile = bytes;
    hostile[76] = 1u;
    QNP_CHECK(!wire3::decodePlaybackStatus(
        hostile.data(), hostile.size(), statusOut));

    wire3::PlaybackStatusPayload failed = status;
    failed.phase = wire3::PlaybackRuntimePhase::Failed;
    failed.hasFailure = true;
    failed.failureReason =
        wire3::PlaybackResultReason::FramePreHashMismatch;
    bytes.clear();
    QNP_CHECK(wire3::encodePlaybackStatus(failed, bytes));

    wire3::PlaybackByePayload bye{};
    bye.reason = wire3::PlaybackByeReason::NormalAfterResult;
    bye.resultSent = true;
    bye.nextRecord = 3u;
    bye.lastCommandId = command.commandId;
    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackBye(bye, bytes));
    wire3::PlaybackByePayload byeOut{};
    QNP_CHECK(wire3::decodePlaybackBye(
        bytes.data(), bytes.size(), byeOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::PlaybackByePayload out{};
        return wire3::decodePlaybackBye(p, n, out);
    });
    hostile = bytes;
    hostile[0] = 0u;
    QNP_CHECK(!wire3::decodePlaybackBye(
        hostile.data(), hostile.size(), byeOut));
    hostile = bytes;
    hostile[3] = 1u;
    QNP_CHECK(!wire3::decodePlaybackBye(
        hostile.data(), hostile.size(), byeOut));
}

QNP_TEST(ReplayWireV3CasterResponsesAreCanonicalAndBounded) {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> hostile;

    wire3::CasterGameAcceptedPayload accepted{};
    accepted.nextRecord = 1u;
    accepted.nextEventSequence = 2u;
    QNP_REQUIRE(wire3::encodeCasterGameAccepted(accepted, bytes));
    wire3::CasterGameAcceptedPayload acceptedOut{};
    QNP_CHECK(wire3::decodeCasterGameAccepted(
        bytes.data(), bytes.size(), acceptedOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::CasterGameAcceptedPayload out{};
        return wire3::decodeCasterGameAccepted(p, n, out);
    });
    hostile = bytes;
    hostile[8] = 1u;
    QNP_CHECK(!wire3::decodeCasterGameAccepted(
        hostile.data(), hostile.size(), acceptedOut));

    wire3::CasterGameFailedPayload failed{};
    failed.code = wire3::RecorderFailureCode::Storage;
    failed.stage = 3u;
    failed.nextRecord = 2u;
    failed.nextEventSequence = 4u;
    failed.platformError = 5u;
    failed.detail = 6u;
    bytes.clear();
    QNP_REQUIRE(wire3::encodeCasterGameFailed(failed, bytes));
    wire3::CasterGameFailedPayload failedOut{};
    QNP_CHECK(wire3::decodeCasterGameFailed(
        bytes.data(), bytes.size(), failedOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::CasterGameFailedPayload out{};
        return wire3::decodeCasterGameFailed(p, n, out);
    });
    hostile = bytes;
    hostile[0] = 0u;
    QNP_CHECK(!wire3::decodeCasterGameFailed(
        hostile.data(), hostile.size(), failedOut));

    wire3::CasterRecorderStatusPayload status{};
    status.state = wire3::RecorderHostState::WritingGame;
    status.processAlive = 1u;
    status.commitPending = 1u;
    status.nextRecord = 2u;
    status.nextEventSequence = 4u;
    status.queueHighWater = 10u;
    status.chunkCount = 3u;
    status.bytesWritten = 1234u;
    bytes.clear();
    QNP_REQUIRE(wire3::encodeCasterRecorderStatus(status, bytes));
    wire3::CasterRecorderStatusPayload statusOut{};
    QNP_CHECK(wire3::decodeCasterRecorderStatus(
        bytes.data(), bytes.size(), statusOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::CasterRecorderStatusPayload out{};
        return wire3::decodeCasterRecorderStatus(p, n, out);
    });
    hostile = bytes;
    hostile[3] = 1u;
    QNP_CHECK(!wire3::decodeCasterRecorderStatus(
        hostile.data(), hostile.size(), statusOut));
    hostile = bytes;
    hostile[60] = 1u;
    QNP_CHECK(!wire3::decodeCasterRecorderStatus(
        hostile.data(), hostile.size(), statusOut));

    wire3::CasterDrainAckPayload drain{};
    drain.committedGames = 2u;
    drain.failedGames = 1u;
    bytes.clear();
    QNP_REQUIRE(wire3::encodeCasterDrainAck(drain, bytes));
    wire3::CasterDrainAckPayload drainOut{};
    QNP_CHECK(wire3::decodeCasterDrainAck(
        bytes.data(), bytes.size(), drainOut));
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::CasterDrainAckPayload out{};
        return wire3::decodeCasterDrainAck(p, n, out);
    });
    hostile = bytes;
    hostile[12] = 1u;
    QNP_CHECK(!wire3::decodeCasterDrainAck(
        hostile.data(), hostile.size(), drainOut));
}

QNP_TEST(ReplayWireV3StatusCommitEndpointAndResultRoundTrip) {
    wire3::CasterGameCommittedPayload committed{};
    committed.nextRecord = 3u;
    committed.reason = TerminalReason::Completed;
    committed.canonicalGameDigest.fill(0x11u);
    committed.fileDigest.fill(0x22u);
    committed.fileBytes = 12345u;
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(wire3::encodeCasterGameCommitted(committed, bytes));
    QNP_CHECK(bytes.size() == wire3::kCasterGameCommittedPayloadBytes);
    wire3::CasterGameCommittedPayload committedOut{};
    QNP_CHECK(wire3::decodeCasterGameCommitted(
        bytes.data(), bytes.size(), committedOut));
    QNP_CHECK(committedOut.canonicalGameDigest
              == committed.canonicalGameDigest);
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::CasterGameCommittedPayload out{};
        return wire3::decodeCasterGameCommitted(p, n, out);
    });
    std::vector<uint8_t> hostile = bytes;
    hostile[6] = 1u;
    QNP_CHECK(!wire3::decodeCasterGameCommitted(
        hostile.data(), hostile.size(), committedOut));

    CompleteGameV2 game = completeGameV2();
    wire3::PlaybackEndpointPayload endpoint{};
    endpoint.kind = wire3::PlaybackEndpointKind::RecordedGameEnd;
    endpoint.hasRecordedGameEnd = true;
    endpoint.nextRecord = game.end.nextRecord;
    endpoint.postTailStateHash = game.end.postTailStateHash;
    endpoint.finalRollingDigest = game.end.finalRollingDigest;
    endpoint.validatedSourceByteCount = 999u;
    endpoint.validatedSourceRangeDigest.fill(0x33u);
    endpoint.recordedGameEnd = game.end;
    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackEndpoint(endpoint, bytes));
    QNP_CHECK(bytes.size() == wire3::kPlaybackEndpointPayloadBytes);
    wire3::PlaybackEndpointPayload endpointOut{};
    QNP_CHECK(wire3::decodePlaybackEndpoint(
        bytes.data(), bytes.size(), endpointOut));
    QNP_CHECK(endpointOut.recordedGameEnd == game.end);

    wire3::PlaybackResultPayload result{};
    result.reason = wire3::PlaybackResultReason::VerifiedCompleted;
    result.endpointKind = wire3::PlaybackEndpointKind::RecordedGameEnd;
    result.hasFileDigest = true;
    result.hasCanonicalGameDigest = true;
    result.hasRecordedTerminal = true;
    result.fileDigest.fill(0x44u);
    result.canonicalGameDigest = game.end.canonicalGameDigest;
    result.validatedSourceByteCount = 999u;
    result.validatedSourceRangeDigest.fill(0x33u);
    result.runtimeContract = game.start.manifest.contract;
    result.nextRecordConsumed = game.end.nextRecord;
    result.finalRollingDigest = game.end.finalRollingDigest;
    result.postTailStateHash = game.end.postTailStateHash;
    result.recordedTerminal =
        static_cast<uint16_t>(TerminalReason::Completed);
    bytes.clear();
    QNP_REQUIRE(wire3::encodePlaybackResult(result, bytes));
    QNP_CHECK(bytes.size() == wire3::kPlaybackResultPayloadBytes);
    wire3::PlaybackResultPayload resultOut{};
    QNP_CHECK(wire3::decodePlaybackResult(
        bytes.data(), bytes.size(), resultOut));
    QNP_CHECK(resultOut.nextRecordConsumed == 3u);
    QNP_CHECK(resultOut.runtimeContract == result.runtimeContract);
    everyBoolTruncationFailsV2(bytes, [&](const uint8_t* p, size_t n) {
        wire3::PlaybackResultPayload out{};
        return wire3::decodePlaybackResult(p, n, out);
    });
    hostile = bytes;
    hostile[0] = 0u;
    QNP_CHECK(!wire3::decodePlaybackResult(
        hostile.data(), hostile.size(), resultOut));
    hostile = bytes;
    hostile[274] = 1u;
    QNP_CHECK(!wire3::decodePlaybackResult(
        hostile.data(), hostile.size(), resultOut));
}
