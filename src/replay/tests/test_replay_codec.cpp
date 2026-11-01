#include "TestHarness.h"

#include "replay/ReplayCodec.h"
#include "replay/ReplayIntegrity.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace qreplay;

std::vector<uint8_t> asBytes(const char* text) {
    std::vector<uint8_t> bytes;
    for (const char* p = text; *p != '\0'; ++p)
        bytes.push_back(static_cast<uint8_t>(*p));
    return bytes;
}

uint32_t crcOfText(const char* text) {
    const std::vector<uint8_t> bytes = asBytes(text);
    return crc32c(bytes.empty() ? nullptr : bytes.data(), bytes.size());
}

std::string sha256HexOfText(const char* text) {
    const std::vector<uint8_t> bytes = asBytes(text);
    return toHex(sha256(bytes.empty() ? nullptr : bytes.data(), bytes.size()));
}

uint32_t readLe32(const std::vector<uint8_t>& bytes, size_t off) {
    uint32_t value = 0u;
    for (unsigned i = 0; i < 4u; ++i)
        value |= static_cast<uint32_t>(bytes[off + i]) << (i * 8u);
    return value;
}

void writeLe32(std::vector<uint8_t>& bytes, size_t off, uint32_t value) {
    for (unsigned i = 0; i < 4u; ++i)
        bytes[off + i] = static_cast<uint8_t>(value >> (i * 8u));
}

void writeLe16(std::vector<uint8_t>& bytes, size_t off, uint16_t value) {
    for (unsigned i = 0; i < 2u; ++i)
        bytes[off + i] = static_cast<uint8_t>(value >> (i * 8u));
}

void reseal(std::vector<uint8_t>& bytes) {
    QNP_CHECK(bytes.size() >= 48u);
    if (bytes.size() < 48u)
        return;
    writeLe32(bytes, 44u, 0u);
    writeLe32(bytes, 44u, crc32c(bytes.data(), 48u));
}

void setDisplayName(std::array<char, 32>& field, const char* text) {
    field.fill('\0');
    for (size_t i = 0; text[i] != '\0' && i < 20u; ++i)
        field[i] = text[i];
}

template <typename Decoder>
void checkEveryTruncationFails(
    const std::vector<uint8_t>& bytes, Decoder decode) {
    for (size_t length = 0; length < bytes.size(); ++length) {
        std::vector<uint8_t> partial(
            bytes.begin(), bytes.begin() + static_cast<ptrdiff_t>(length));
        const ParseStatus status =
            decode(partial.empty() ? nullptr : partial.data(), partial.size());
        QNP_CHECK(status != ParseStatus::Ok);
    }
}

SimulationContract sampleContract() {
    SimulationContract contract{};
    for (size_t i = 0; i < contract.executableSha256.size(); ++i)
        contract.executableSha256[i] = static_cast<uint8_t>(0x10u + i);
    for (size_t i = 0; i < contract.contentManifestRoot.size(); ++i)
        contract.contentManifestRoot[i] = static_cast<uint8_t>(0xA0u + i);
    contract.runtimeSimulationId = 0x0102030405060708ull;
    contract.stateRegionFingerprint = 0x1112131415161718ull;
    contract.stateHashProjectionId = 0x2122232425262728ull;
    contract.stateHashAlgorithmId = 0x3132333435363738ull;
    contract.bootstrapCodecVersion = 0x4142434445464748ull;
    contract.eventSchemaVersion = 0x5152535455565758ull;
    contract.characterCatalogId = 0x6162636465666768ull;
    contract.fpuControlWord = 0x027Fu;
    contract.mxcsr = 0x1F80u;
    return contract;
}

FighterSlotDescriptor sampleSlot(
    SlotOwnership ownership, uint8_t side, uint8_t characterId) {
    FighterSlotDescriptor slot{};
    slot.ownership = ownership;
    slot.side = side;
    slot.identity.nativeCharacterId = characterId;
    slot.identity.paletteIndex = static_cast<uint8_t>(characterId % 4u);
    if (ownership != SlotOwnership::Inactive) {
        slot.selectorProofPresent = true;
        slot.selectorProof.cursorCell = static_cast<uint8_t>(characterId % 24u);
        slot.selectorProof.alternateFlag = 1u;
        slot.selectorProof.confirmState = 3u;
    }
    return slot;
}

SessionManifest sampleManifest() {
    SessionManifest manifest{};
    manifest.contract = sampleContract();
    manifest.bootstrapKind = BootstrapKind::NetworkVsDirectBootV1;
    manifest.vsRow = 2u;
    manifest.cpuMask = 0x08u;
    manifest.cpuContract = CpuContract::NativeAiDeterministicV1;
    manifest.slotToSourceRoute = { { 0u, 1u, 2u, 3u } };
    manifest.lanes[0].active = true;
    manifest.lanes[0].name = "Player One";
    manifest.lanes[1].active = true;
    manifest.lanes[1].name = "Player Two";
    manifest.lanes[2].active = true;
    manifest.lanes[2].name = "\xE3\x83\x97\xE3\x83\xAC\xE3\x82\xA4\xE3\x83\xA4";
    manifest.lanes[3].active = false;
    manifest.slots[0] = sampleSlot(SlotOwnership::RecordedHuman, 0u, 5u);
    manifest.slots[1] = sampleSlot(SlotOwnership::RecordedHuman, 1u, 12u);
    manifest.slots[2] = sampleSlot(SlotOwnership::RecordedHuman, 0u, 33u);
    manifest.slots[3] = sampleSlot(SlotOwnership::NativeCpu, 1u, 7u);
    manifest.rosterEpoch = 4u;
    manifest.display.recordedUnixSeconds = 1785000000;
    manifest.display.timezoneOffsetMinutes = -480;
    manifest.display.sourceKind = ReplaySourceKind::NetplayPlayer;
    manifest.display.routeLabel = "relay:eu-west";
    manifest.display.playerOneDelay = 2u;
    manifest.display.playerTwoDelay = 3u;
    manifest.display.rollbackWindow = 8u;
    manifest.display.averageRttMicros = 31500u;
    manifest.display.lossPermille = 12u;
    manifest.display.networkMetadataPresent = true;
    manifest.display.title = "Ranked set 7";
    return manifest;
}

DeterministicStart sampleStart(uint32_t firstFrame) {
    DeterministicStart start{};
    start.seedGameplay = 0xDEADBEEFu;
    start.seedCosmetic = 0x12345678u;
    start.selector = 1u;
    start.initialPhase = 3u;
    start.bootstrapHash = 0x0BADF00DDEADBEEFull;
    start.timerMode = 99u;
    start.aiDifficulty = 4u;
    start.lifeMultiplier = 100u;
    start.roundsToWinMinusOne = 1u;
    start.playerOneNameCodePage = 932u;
    start.playerTwoNameCodePage = 1252u;
    setDisplayName(start.displayNames[0], "AQUA");
    setDisplayName(start.displayNames[1], "RIVAL");
    setDisplayName(start.displayNames[2], "");
    setDisplayName(start.displayNames[3], "");
    start.firstTimelineFrame = firstFrame;
    start.initialStateHash = 0xFEEDFACECAFEBEEFull;
    start.initialDigest = 0x1122334455667788ull;
    return start;
}

MatchStartRecord sampleMatchStart() {
    MatchStartRecord record{};
    record.matchId = 7u;
    record.ordinal = 2u;
    record.firstTimelineFrame = 6000u;
    record.dependsOnMatchId = 0u;
    record.independentBootstrap = true;
    record.start = sampleStart(6000u);
    record.slots[0] = sampleSlot(SlotOwnership::RecordedHuman, 0u, 1u);
    record.slots[1] = sampleSlot(SlotOwnership::RecordedHuman, 1u, 2u);
    record.slots[2] = sampleSlot(SlotOwnership::Inactive, 0u, 0xFFu);
    record.slots[3] = sampleSlot(SlotOwnership::Inactive, 1u, 0xFFu);
    record.slotToSourceRoute = { { 0u, 1u, 2u, 3u } };
    record.vsRow = 1u;
    record.cpuMask = 0u;
    record.rosterEpoch = 3u;
    record.stageId = 11u;
    record.stageVerified = true;
    return record;
}

MatchEndRecord sampleMatchEnd() {
    MatchEndRecord record{};
    record.matchId = 7u;
    record.lastTimelineFrame = 9000u;
    record.winningSideMask = 0x05u;
    record.roundsSideZero = 2u;
    record.roundsSideOne = 1u;
    record.reason = TerminalReason::Completed;
    record.postTailStateHash = 0x8899AABBCCDDEEFFull;
    record.rollingDigest = 0x0011223344556677ull;
    return record;
}

SessionEndRecord sampleSessionEnd() {
    SessionEndRecord record{};
    record.lastTimelineFrame = 12345u;
    record.reason = TerminalReason::Disconnected;
    record.postTailStateHash = 0xAABBCCDDEEFF0011ull;
    record.rollingDigest = 0x7766554433221100ull;
    record.matchCount = 3u;
    return record;
}

ReplayFrameV1 sampleFrame(uint32_t timelineFrame) {
    ReplayFrameV1 frame{};
    frame.timelineFrame = timelineFrame;
    frame.inputDomain = InputDomain::LogicalSourceV1;
    frame.activeLaneMask = 0x03u;
    frame.actions[0] = 0x0102u;
    frame.actions[1] = 0x0801u;
    frame.routeEpoch = 2u;
    frame.preStateHash = 0x1111111111111111ull + timelineFrame;
    frame.postStateHash = 0x2222222222222222ull + timelineFrame;
    frame.lifecycleBefore.scene = 3u;
    frame.lifecycleBefore.requestedScene = 3u;
    frame.lifecycleBefore.sceneThreeState = 2u;
    frame.lifecycleBefore.matchState = 1u;
    frame.lifecycleBefore.roundState = 4u;
    frame.lifecycleBefore.layoutGeneration = 9u;
    frame.lifecycleAfter = frame.lifecycleBefore;
    frame.lifecycleAfter.roundState = 5u;
    frame.dispatcherResult = -1;
    frame.rollingDigest = 0x9999999999999999ull ^ timelineFrame;
    return frame;
}

ReplayEvent sampleEvent(EventKind kind, uint32_t timelineFrame) {
    ReplayEvent event{};
    event.timelineFrame = timelineFrame;
    event.kind = kind;
    event.matchId = 7u;
    event.valueA = 0u;
    event.valueB = 0u;
    event.valueC = 0u;
    event.stateProof = 0x4444444444444444ull;
    event.rollingDigest = 0x5555555555555555ull;
    if (kind == EventKind::RosterSelected) {
        event.rosterEpoch = 4u;
        for (size_t slot = 0; slot < kFighterSlots; ++slot) {
            event.roster[slot].nativeCharacterId =
                static_cast<uint8_t>(slot + 1u);
            event.roster[slot].paletteIndex = static_cast<uint8_t>(slot % 4u);
            event.selectorProof[slot].cursorCell =
                static_cast<uint8_t>(slot * 3u);
            event.selectorProof[slot].alternateFlag =
                static_cast<uint8_t>(slot % 2u);
            event.selectorProof[slot].confirmState = 4u;
        }
        event.selectorProofMask = 0x0Fu;
    } else if (kind == EventKind::SegmentTerminal) {
        event.terminalReason = TerminalReason::Disconnected;
    } else if (kind == EventKind::MatchResult) {
        event.valueA = 0x05u;
        event.valueB = 0x0102u;
    }
    return event;
}

MatchIndexEntry sampleIndexEntry(uint32_t ordinal) {
    MatchIndexEntry entry{};
    entry.matchId = ordinal + 1u;
    entry.ordinal = ordinal;
    entry.matchStartOffset = static_cast<uint64_t>(ordinal) * 1000ull + 48ull;
    entry.firstInputOffset = entry.matchStartOffset + 100ull;
    entry.firstTimelineFrame = ordinal * 100u;
    entry.lastTimelineFrame = ordinal * 100u + 50u;
    entry.frameCount = 51u;
    entry.roundCount = 3u;
    entry.winningSideMask = 0x01u;
    entry.independentBootstrap = ordinal == 0u;
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        entry.roster[slot].nativeCharacterId =
            static_cast<uint8_t>(slot < 2u ? slot + 1u : 0xFFu);
        entry.roster[slot].paletteIndex = 0u;
        entry.ownership[slot] = slot < 2u
            ? SlotOwnership::RecordedHuman
            : SlotOwnership::Inactive;
    }
    return entry;
}

FooterIndex sampleFooter(uint32_t matchCount) {
    FooterIndex footer{};
    footer.chunkCount = 128u;
    footer.matchCount = matchCount;
    footer.totalFrames = 20000u;
    footer.finalRollingDigest = 0xABCDEF0123456789ull;
    footer.matches.clear();
    for (uint32_t i = 0; i < matchCount; ++i)
        footer.matches.push_back(sampleIndexEntry(i));
    for (size_t i = 0; i < footer.fileDigest.size(); ++i)
        footer.fileDigest[i] = static_cast<uint8_t>(0xC0u + i);
    return footer;
}

std::vector<uint8_t> encodedManifest() {
    std::vector<uint8_t> bytes;
    QNP_CHECK(encodeSessionManifest(sampleManifest(), bytes));
    return bytes;
}

std::vector<uint8_t> encodedInputBlock(uint32_t firstFrame, size_t count) {
    std::vector<ReplayFrameV1> frames;
    for (size_t i = 0; i < count; ++i)
        frames.push_back(sampleFrame(firstFrame + static_cast<uint32_t>(i)));
    std::vector<uint8_t> bytes;
    QNP_CHECK(encodeInputBlock(
        frames.empty() ? nullptr : frames.data(), frames.size(), bytes));
    return bytes;
}

}

QNP_TEST(PaletteBlockRoundTripsAndRefusesAnOpaqueTransparentKey) {
    ReplayPaletteV2 palette{};
    palette.side = 1u;
    palette.variant = 3u;
    palette.present = 1u;
    for (size_t variant = 0; variant < 4u; ++variant) {
        for (size_t colour = 1; colour < 16u; ++colour) {
            uint8_t* entry = palette.table + variant * 64u + colour * 4u;
            entry[0] = static_cast<uint8_t>(colour * 11u + variant);
            entry[1] = static_cast<uint8_t>(colour * 13u + variant);
            entry[2] = static_cast<uint8_t>(colour * 17u + variant);
        }
    }

    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodePaletteBlockV2(palette, encoded));
    QNP_CHECK(encoded.size() == kReplayPaletteBlockBytes);

    ReplayPaletteV2 decoded{};
    QNP_REQUIRE(decodePaletteBlockV2(encoded.data(), encoded.size(), decoded)
                == ParseStatus::Ok);
    QNP_CHECK(decoded.side == 1u);
    QNP_CHECK(decoded.variant == 3u);
    QNP_CHECK(decoded.present == 1u);
    QNP_CHECK(std::memcmp(decoded.table, palette.table,
                          kReplayPaletteTableBytes) == 0);

    for (size_t variant = 0; variant < 4u; ++variant) {
        std::vector<uint8_t> opaque = encoded;
        opaque[4u + variant * 64u + 1u] = 0x40u;
        ReplayPaletteV2 rejected{};
        QNP_CHECK(decodePaletteBlockV2(opaque.data(), opaque.size(), rejected)
                  == ParseStatus::BadPalettePayload);
    }

    std::vector<uint8_t> badVariant = encoded;
    badVariant[1] = 4u;
    ReplayPaletteV2 out{};
    QNP_CHECK(decodePaletteBlockV2(badVariant.data(), badVariant.size(), out)
              == ParseStatus::BadPalettePayload);
    std::vector<uint8_t> shortBlock = encoded;
    shortBlock.pop_back();
    QNP_CHECK(decodePaletteBlockV2(shortBlock.data(), shortBlock.size(), out)
              == ParseStatus::Truncated);

    std::vector<uint8_t> reserved = encoded;
    reserved[3] = 1u;
    QNP_CHECK(decodePaletteBlockV2(reserved.data(), reserved.size(), out)
              == ParseStatus::BadPalettePayload);
}

QNP_TEST(PaletteChunksAreNonCriticalAndKnown) {
    QNP_CHECK(isKnownChunkKindV2(
        static_cast<uint16_t>(ChunkKindV2::Palette)));
    QNP_CHECK(!isCriticalChunkKindV2(ChunkKindV2::Palette));
    QNP_CHECK(isCriticalChunkKindV2(ChunkKindV2::GameStart));
    QNP_CHECK(isCriticalChunkKindV2(ChunkKindV2::InputBlock));
    QNP_CHECK(isCriticalChunkKindV2(ChunkKindV2::CompatibilityIdentity));
    QNP_CHECK(!isKnownChunkKindV2(
        static_cast<uint16_t>(ChunkKindV2::Palette) + 1u));
}

QNP_TEST(ReplayCrc32cKnownAnswerVectors) {
    QNP_CHECK(crcOfText("") == 0x00000000u);
    QNP_CHECK(crcOfText("a") == 0xC1D04330u);
    QNP_CHECK(crcOfText("abc") == 0x364B3FB7u);
    QNP_CHECK(crcOfText("123456789") == 0xE3069283u);

    std::vector<uint8_t> zeros(32, 0x00u);
    std::vector<uint8_t> ones(32, 0xFFu);
    std::vector<uint8_t> ramp(32, 0x00u);
    for (size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<uint8_t>(i);
    QNP_CHECK(crc32c(zeros.data(), zeros.size()) == 0x8A9136AAu);
    QNP_CHECK(crc32c(ones.data(), ones.size()) == 0x62A8AB43u);
    QNP_CHECK(crc32c(ramp.data(), ramp.size()) == 0x46DD794Eu);

    QNP_CHECK(crc32c(nullptr, 0u, 0x1234u) == 0x1234u);
}

QNP_TEST(ReplayCrc32cIncrementalMatchesOneShot) {
    const std::vector<uint8_t> head = asBytes("1234");
    const std::vector<uint8_t> tail = asBytes("56789");

    const uint32_t composed =
        crc32c(tail.data(), tail.size(), crc32c(head.data(), head.size()));
    QNP_CHECK(composed == 0xE3069283u);

    Crc32c incremental;
    QNP_CHECK(incremental.value() == 0x00000000u);
    incremental.update(head.data(), head.size());
    incremental.update(nullptr, 0u);
    incremental.update(tail.data(), tail.size());
    QNP_CHECK(incremental.value() == 0xE3069283u);
    incremental.reset();
    QNP_CHECK(incremental.value() == 0x00000000u);

    std::vector<uint8_t> blob(777u, 0u);
    for (size_t i = 0; i < blob.size(); ++i)
        blob[i] = static_cast<uint8_t>(i * 31u + 7u);
    Crc32c streamed;
    for (size_t i = 0; i < blob.size(); ++i)
        streamed.update(blob.data() + i, 1u);
    QNP_CHECK(streamed.value() == crc32c(blob.data(), blob.size()));
}

QNP_TEST(ReplaySha256KnownAnswerVectors) {
    QNP_CHECK(sha256HexOfText("")
              == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b785"
                 "2b855");
    QNP_CHECK(sha256HexOfText("abc")
              == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f200"
                 "15ad");
    QNP_CHECK(sha256HexOfText(
                  "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
              == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db"
                 "06c1");

    std::vector<uint8_t> million(1000000u, static_cast<uint8_t>('a'));
    QNP_CHECK(toHex(sha256(million.data(), million.size()))
              == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc711"
                 "2cd0");
}

QNP_TEST(ReplaySha256StreamingMatchesOneShot) {
    std::vector<uint8_t> blob(1000u, 0u);
    for (size_t i = 0; i < blob.size(); ++i)
        blob[i] = static_cast<uint8_t>(i * 17u + 3u);
    const std::array<uint8_t, 32> expected = sha256(blob.data(), blob.size());

    const size_t chunkSizes[] = { 1u, 3u, 55u, 64u, 65u, 127u, 999u };
    for (size_t c = 0; c < sizeof(chunkSizes) / sizeof(chunkSizes[0]); ++c) {
        Sha256 hasher;
        size_t offset = 0;
        while (offset < blob.size()) {
            const size_t take = blob.size() - offset < chunkSizes[c]
                ? blob.size() - offset
                : chunkSizes[c];
            hasher.update(blob.data() + offset, take);
            offset += take;
        }
        std::array<uint8_t, 32> digest{};
        hasher.finish(digest);
        QNP_CHECK(digest == expected);
        std::array<uint8_t, 32> again{};
        hasher.update(blob.data(), blob.size());
        hasher.finish(again);
        QNP_CHECK(again == expected);
    }

    const size_t boundaries[] = { 0u, 1u, 55u, 56u, 57u, 63u, 64u, 65u, 119u,
                                  120u, 128u };
    for (size_t b = 0; b < sizeof(boundaries) / sizeof(boundaries[0]); ++b) {
        const size_t length = boundaries[b];
        Sha256 hasher;
        hasher.update(blob.data(), length);
        std::array<uint8_t, 32> digest{};
        hasher.finish(digest);
        QNP_CHECK(digest == sha256(blob.data(), length));
    }
}

QNP_TEST(ReplayHexRoundTripAndRejects) {
    std::array<uint8_t, 32> digest{};
    for (size_t i = 0; i < digest.size(); ++i)
        digest[i] = static_cast<uint8_t>(i * 7u);
    const std::string text = toHex(digest);
    QNP_REQUIRE(text.size() == 64u);

    std::array<uint8_t, 32> parsed{};
    QNP_CHECK(fromHex(text, parsed));
    QNP_CHECK(parsed == digest);

    std::string upper = text;
    for (size_t i = 0; i < upper.size(); ++i) {
        if (upper[i] >= 'a' && upper[i] <= 'f')
            upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
    }
    std::array<uint8_t, 32> parsedUpper{};
    QNP_CHECK(fromHex(upper, parsedUpper));
    QNP_CHECK(parsedUpper == digest);

    std::array<uint8_t, 32> untouched{};
    untouched.fill(0x5Au);
    std::array<uint8_t, 32> guard = untouched;
    QNP_CHECK(!fromHex("", untouched));
    QNP_CHECK(!fromHex(text.substr(0, 63), untouched));
    QNP_CHECK(!fromHex(text + "0", untouched));
    std::string bad = text;
    bad[17] = 'g';
    QNP_CHECK(!fromHex(bad, untouched));
    bad = text;
    bad[0] = ' ';
    QNP_CHECK(!fromHex(bad, untouched));
    QNP_CHECK(untouched == guard);
}

QNP_TEST(ReplayRollingDigestIsOrderedAndFieldSensitive) {
    const ReplayFrameV1 first = sampleFrame(100u);
    const ReplayFrameV1 second = sampleFrame(101u);

    const uint64_t a = advanceReplayDigest(0u, first);
    const uint64_t b = advanceReplayDigest(a, second);
    const uint64_t reversed =
        advanceReplayDigest(advanceReplayDigest(0u, second), first);
    QNP_CHECK(a != 0u);
    QNP_CHECK(b != a);
    QNP_CHECK(b != reversed);
    QNP_CHECK(advanceReplayDigest(0u, first) == a);
    QNP_CHECK(advanceReplayDigest(1u, first) != a);

    ReplayFrameV1 restamped = first;
    restamped.rollingDigest ^= 0xFFFFFFFFFFFFFFFFull;
    QNP_CHECK(advanceReplayDigest(0u, restamped) == a);

    ReplayFrameV1 mutated = first;
    mutated.timelineFrame += 1u;
    QNP_CHECK(advanceReplayDigest(0u, mutated) != a);
    mutated = first;
    mutated.inputDomain = InputDomain::EffectiveFighterV1;
    QNP_CHECK(advanceReplayDigest(0u, mutated) != a);
    mutated = first;
    mutated.activeLaneMask = 0x07u;
    QNP_CHECK(advanceReplayDigest(0u, mutated) != a);
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        mutated = first;
        mutated.actions[lane] = static_cast<uint16_t>(
            mutated.actions[lane] ^ 0x0101u);
        QNP_CHECK(advanceReplayDigest(0u, mutated) != a);
    }
    mutated = first;
    mutated.routeEpoch += 1u;
    QNP_CHECK(advanceReplayDigest(0u, mutated) != a);
    mutated = first;
    mutated.preStateHash ^= 1ull;
    QNP_CHECK(advanceReplayDigest(0u, mutated) != a);
    mutated = first;
    mutated.postStateHash ^= 1ull;
    QNP_CHECK(advanceReplayDigest(0u, mutated) != a);
    mutated = first;
    mutated.lifecycleBefore.layoutGeneration += 1u;
    QNP_CHECK(advanceReplayDigest(0u, mutated) != a);
    mutated = first;
    mutated.lifecycleAfter.sceneThreeState += 1u;
    QNP_CHECK(advanceReplayDigest(0u, mutated) != a);
    mutated = first;
    mutated.dispatcherResult = 0;
    QNP_CHECK(advanceReplayDigest(0u, mutated) != a);

    const ReplayEvent event = sampleEvent(EventKind::MatchStarted, 100u);
    const uint64_t eventDigest = advanceReplayDigest(0u, event);
    QNP_CHECK(eventDigest != a);
    QNP_CHECK(advanceReplayDigest(0u, event) == eventDigest);

    ReplayEvent eventCopy = event;
    eventCopy.rollingDigest ^= 0xFFFFFFFFFFFFFFFFull;
    QNP_CHECK(advanceReplayDigest(0u, eventCopy) == eventDigest);
    eventCopy = event;
    eventCopy.kind = EventKind::RoundStarted;
    QNP_CHECK(advanceReplayDigest(0u, eventCopy) != eventDigest);
    eventCopy = event;
    eventCopy.matchId += 1u;
    QNP_CHECK(advanceReplayDigest(0u, eventCopy) != eventDigest);
    eventCopy = event;
    eventCopy.valueA ^= 1ull;
    QNP_CHECK(advanceReplayDigest(0u, eventCopy) != eventDigest);
    eventCopy = event;
    eventCopy.valueB ^= 1ull;
    QNP_CHECK(advanceReplayDigest(0u, eventCopy) != eventDigest);
    eventCopy = event;
    eventCopy.valueC ^= 1u;
    QNP_CHECK(advanceReplayDigest(0u, eventCopy) != eventDigest);
    eventCopy = event;
    eventCopy.stateProof ^= 1ull;
    QNP_CHECK(advanceReplayDigest(0u, eventCopy) != eventDigest);
    eventCopy = event;
    eventCopy.terminalReason = TerminalReason::ProcessExit;
    QNP_CHECK(advanceReplayDigest(0u, eventCopy) != eventDigest);

    const ReplayEvent roster = sampleEvent(EventKind::RosterSelected, 100u);
    const uint64_t rosterDigest = advanceReplayDigest(0u, roster);
    ReplayEvent rosterCopy = roster;
    rosterCopy.roster[2].paletteIndex ^= 1u;
    QNP_CHECK(advanceReplayDigest(0u, rosterCopy) != rosterDigest);
    rosterCopy = roster;
    rosterCopy.selectorProof[1].confirmState = 1u;
    QNP_CHECK(advanceReplayDigest(0u, rosterCopy) != rosterDigest);
    rosterCopy = roster;
    rosterCopy.selectorProofMask = 0x07u;
    QNP_CHECK(advanceReplayDigest(0u, rosterCopy) != rosterDigest);
    rosterCopy = roster;
    rosterCopy.rosterEpoch += 1u;
    QNP_CHECK(advanceReplayDigest(0u, rosterCopy) != rosterDigest);
}

QNP_TEST(ReplayPrimitivesRoundTripAndBounds) {
    std::vector<uint8_t> bytes;
    putU8(bytes, 0x12u);
    putU16(bytes, 0x3456u);
    putU32(bytes, 0x789ABCDEu);
    putU64(bytes, 0x0123456789ABCDEFull);
    putI32(bytes, -2147483647 - 1);
    putI64(bytes, -1);
    QNP_REQUIRE(bytes.size() == 1u + 2u + 4u + 8u + 4u + 8u);

    const uint8_t expected[] = {
        0x12u,
        0x56u, 0x34u,
        0xDEu, 0xBCu, 0x9Au, 0x78u,
        0xEFu, 0xCDu, 0xABu, 0x89u, 0x67u, 0x45u, 0x23u, 0x01u,
        0x00u, 0x00u, 0x00u, 0x80u,
        0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu,
    };
    QNP_CHECK(std::memcmp(bytes.data(), expected, sizeof(expected)) == 0);

    size_t off = 0;
    uint8_t u8 = 0u;
    uint16_t u16 = 0u;
    uint32_t u32 = 0u;
    uint64_t u64 = 0u;
    int32_t i32 = 0;
    int64_t i64 = 0;
    QNP_CHECK(getU8(bytes.data(), bytes.size(), off, u8) && u8 == 0x12u);
    QNP_CHECK(getU16(bytes.data(), bytes.size(), off, u16) && u16 == 0x3456u);
    QNP_CHECK(getU32(bytes.data(), bytes.size(), off, u32)
              && u32 == 0x789ABCDEu);
    QNP_CHECK(getU64(bytes.data(), bytes.size(), off, u64)
              && u64 == 0x0123456789ABCDEFull);
    QNP_CHECK(getI32(bytes.data(), bytes.size(), off, i32)
              && i32 == -2147483647 - 1);
    QNP_CHECK(getI64(bytes.data(), bytes.size(), off, i64) && i64 == -1);
    QNP_CHECK(off == bytes.size());

    QNP_CHECK(!getU8(bytes.data(), bytes.size(), off, u8));
    QNP_CHECK(off == bytes.size());

    for (size_t length = 0; length < 8u; ++length) {
        size_t partialOff = 0;
        uint64_t value = 0u;
        QNP_CHECK(!getU64(bytes.data(), length, partialOff, value));
        QNP_CHECK(partialOff == 0u);
    }

    size_t wild = 1000u;
    QNP_CHECK(!getU8(bytes.data(), bytes.size(), wild, u8));
    wild = static_cast<size_t>(0) - 1u;
    QNP_CHECK(!getU32(bytes.data(), bytes.size(), wild, u32));

    size_t nullOff = 0;
    QNP_CHECK(!getU8(nullptr, 0u, nullOff, u8));

    uint8_t raw[4] = { 0u, 0u, 0u, 0u };
    size_t rawOff = 0;
    QNP_CHECK(getBytes(bytes.data(), bytes.size(), rawOff, raw, 4u));
    QNP_CHECK(rawOff == 4u);
    QNP_CHECK(!getBytes(bytes.data(), 3u, rawOff, raw, 4u));
    size_t zeroOff = 2u;
    QNP_CHECK(getBytes(bytes.data(), bytes.size(), zeroOff, nullptr, 0u));
    QNP_CHECK(zeroOff == 2u);

    const int32_t signedProbes[] = { 0, 1, -1, 2147483647, -2147483647 - 1 };
    for (size_t i = 0; i < sizeof(signedProbes) / sizeof(signedProbes[0]);
         ++i) {
        std::vector<uint8_t> probe;
        putI32(probe, signedProbes[i]);
        size_t probeOff = 0;
        int32_t decoded = 0;
        QNP_CHECK(getI32(probe.data(), probe.size(), probeOff, decoded));
        QNP_CHECK(decoded == signedProbes[i]);
    }
}

QNP_TEST(ReplayCheckedArithmetic) {
    const size_t maxSize = static_cast<size_t>(0) - 1u;
    size_t result = 0;
    QNP_CHECK(addChecked(2u, 3u, result) && result == 5u);
    QNP_CHECK(!addChecked(maxSize, 1u, result));
    QNP_CHECK(!addChecked(maxSize / 2u + 1u, maxSize / 2u + 1u, result));
    QNP_CHECK(mulChecked(0u, maxSize, result) && result == 0u);
    QNP_CHECK(mulChecked(7u, 11u, result) && result == 77u);
    QNP_CHECK(!mulChecked(maxSize / 2u + 1u, 3u, result));
    QNP_CHECK(!mulChecked(maxSize, 2u, result));

    uint32_t result32 = 0u;
    QNP_CHECK(addChecked32(1u, 2u, result32) && result32 == 3u);
    QNP_CHECK(addChecked32(0xFFFFFFFEu, 1u, result32)
              && result32 == 0xFFFFFFFFu);
    QNP_CHECK(!addChecked32(0xFFFFFFFFu, 1u, result32));
    QNP_CHECK(!addChecked32(0x80000000u, 0x80000000u, result32));
}

QNP_TEST(ReplayUtf8Validation) {
    QNP_CHECK(isCanonicalReplayText(std::string()));
    QNP_CHECK(isCanonicalReplayText(std::string("Player One")));
    QNP_CHECK(isCanonicalReplayText(
        std::string("\xC3\xA9\xE3\x83\x97\xF0\x9F\x98\x80")));
    QNP_CHECK(isCanonicalReplayText(nullptr, 0u));
    QNP_CHECK(!isCanonicalReplayText(nullptr, 1u));

    struct Case {
        const char* label;
        std::vector<uint8_t> bytes;
    };
    const Case rejected[] = {
        { "overlong two-byte NUL", { 0xC0u, 0x80u } },
        { "overlong two-byte slash", { 0xC0u, 0xAFu } },
        { "overlong three-byte", { 0xE0u, 0x80u, 0xAFu } },
        { "overlong four-byte", { 0xF0u, 0x80u, 0x80u, 0xAFu } },
        { "lone high surrogate", { 0xEDu, 0xA0u, 0x80u } },
        { "lone low surrogate", { 0xEDu, 0xB0u, 0x80u } },
        { "surrogate pair (CESU-8)",
          { 0xEDu, 0xA0u, 0xBDu, 0xEDu, 0xB8u, 0x80u } },
        { "above U+10FFFF", { 0xF4u, 0x90u, 0x80u, 0x80u } },
        { "five-byte lead", { 0xF8u, 0x88u, 0x80u, 0x80u, 0x80u } },
        { "bare continuation", { 0x80u } },
        { "truncated two-byte", { 0xC3u } },
        { "truncated three-byte", { 0xE2u, 0x82u } },
        { "truncated four-byte", { 0xF0u, 0x9Fu, 0x98u } },
        { "bad continuation", { 0xE2u, 0x28u, 0xA1u } },
        { "embedded NUL", { 'a', 0x00u, 'b' } },
        { "newline", { 'a', 0x0Au } },
        { "carriage return", { 'a', 0x0Du } },
        { "tab", { 'a', 0x09u } },
        { "terminal escape", { 0x1Bu, '[', '3', '1', 'm' } },
        { "terminal bell", { 0x07u } },
        { "delete", { 0x7Fu } },
        { "C1 NEL (U+0085)", { 0xC2u, 0x85u } },
        { "C1 CSI (U+009B)", { 0xC2u, 0x9Bu } },
    };
    for (size_t i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
        const Case& probe = rejected[i];
        QNP_CHECK(!isCanonicalReplayText(
            probe.bytes.data(), probe.bytes.size()));
        std::vector<uint8_t> encoded;
        const std::string text(
            reinterpret_cast<const char*>(probe.bytes.data()),
            probe.bytes.size());
        QNP_CHECK(!putString(encoded, text, kMaxLabelBytes));
        QNP_CHECK(encoded.empty());
    }

    const uint8_t maxCodePoint[] = { 0xF4u, 0x8Fu, 0xBFu, 0xBFu };
    QNP_CHECK(isCanonicalReplayText(maxCodePoint, sizeof(maxCodePoint)));
    const uint8_t firstAboveC1[] = { 0xC2u, 0xA0u };
    QNP_CHECK(isCanonicalReplayText(firstAboveC1, sizeof(firstAboveC1)));
    const uint8_t space[] = { 0x20u };
    QNP_CHECK(isCanonicalReplayText(space, sizeof(space)));
}

QNP_TEST(ReplayStringLimitsZeroOneMaxAndOver) {
    const size_t limits[] = { kMaxLabelBytes, kMaxNoteBytes, kMaxTagBytes };
    for (size_t i = 0; i < sizeof(limits) / sizeof(limits[0]); ++i) {
        const size_t limit = limits[i];
        const size_t lengths[] = { 0u, 1u, limit };
        for (size_t j = 0; j < sizeof(lengths) / sizeof(lengths[0]); ++j) {
            const std::string text(lengths[j], 'x');
            std::vector<uint8_t> bytes;
            QNP_CHECK(putString(bytes, text, limit));
            QNP_REQUIRE(bytes.size() == lengths[j] + 2u);
            size_t off = 0;
            std::string decoded;
            QNP_CHECK(getString(bytes.data(), bytes.size(), off, limit, decoded)
                      == ParseStatus::Ok);
            QNP_CHECK(decoded == text);
            QNP_CHECK(off == bytes.size());
        }

        const std::string tooLong(limit + 1u, 'x');
        std::vector<uint8_t> overflow;
        QNP_CHECK(!putString(overflow, tooLong, limit));
        QNP_CHECK(overflow.empty());

        std::vector<uint8_t> crafted;
        QNP_CHECK(putString(crafted, tooLong, limit + 1u));
        size_t craftedOff = 0;
        std::string untouched = "sentinel";
        QNP_CHECK(getString(
                      crafted.data(), crafted.size(), craftedOff, limit,
                      untouched)
                  == ParseStatus::LimitExceeded);
        QNP_CHECK(untouched == "sentinel");
        QNP_CHECK(craftedOff == 2u);
    }

    std::vector<uint8_t> lying;
    putU16(lying, 0xFFFFu);
    lying.push_back('a');
    size_t off = 0;
    std::string decoded;
    QNP_CHECK(getString(lying.data(), lying.size(), off, kMaxNoteBytes, decoded)
              == ParseStatus::LimitExceeded);
    writeLe16(lying, 0u, static_cast<uint16_t>(kMaxNoteBytes));
    off = 0;
    QNP_CHECK(getString(lying.data(), lying.size(), off, kMaxNoteBytes, decoded)
              == ParseStatus::Truncated);
    QNP_CHECK(decoded.empty());

    std::vector<uint8_t> badText;
    putU16(badText, 2u);
    badText.push_back(0xC0u);
    badText.push_back(0x80u);
    off = 0;
    QNP_CHECK(getString(
                  badText.data(), badText.size(), off, kMaxLabelBytes, decoded)
              == ParseStatus::BadUtf8);
    QNP_CHECK(decoded.empty());
}

QNP_TEST(ReplayPrefixGoldenBytesAndRoundTrip) {
    ReplayPrefix prefix{};
    prefix.featureFlags = kFeatureFourPlayer | kFeatureCpuFighters;
    prefix.sessionUuidLow = 0x0807060504030201ull;
    prefix.sessionUuidHigh = 0x1817161514131211ull;

    std::vector<uint8_t> bytes;
    encodePrefix(prefix, bytes);
    QNP_REQUIRE(bytes.size() == kPrefixBytes);

    const uint8_t expected[44] = {
        0x51u, 0x39u, 0x39u, 0x52u,
        0x01u, 0x00u,
        0x00u, 0x00u,
        0x01u, 0x00u,
        0x30u, 0x00u,
        0x01u, 0x02u, 0x03u, 0x04u,
        0x03u, 0x00u, 0x00u, 0x00u,
        0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u,
        0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    };
    QNP_CHECK(std::memcmp(bytes.data(), expected, sizeof(expected)) == 0);

    std::vector<uint8_t> sealed(bytes.begin(), bytes.end());
    writeLe32(sealed, 44u, 0u);
    QNP_CHECK(readLe32(bytes, 44u) == crc32c(sealed.data(), sealed.size()));

    ReplayPrefix decoded{};
    QNP_CHECK(decodePrefix(bytes.data(), bytes.size(), decoded)
              == ParseStatus::Ok);
    QNP_CHECK(decoded.containerMajor == prefix.containerMajor);
    QNP_CHECK(decoded.containerMinor == prefix.containerMinor);
    QNP_CHECK(decoded.minimumReaderMajor == prefix.minimumReaderMajor);
    QNP_CHECK(decoded.prefixBytes == static_cast<uint16_t>(kPrefixBytes));
    QNP_CHECK(decoded.featureFlags == prefix.featureFlags);
    QNP_CHECK(decoded.sessionUuidLow == prefix.sessionUuidLow);
    QNP_CHECK(decoded.sessionUuidHigh == prefix.sessionUuidHigh);

    std::vector<uint8_t> withTail = bytes;
    withTail.push_back(0xAAu);
    ReplayPrefix tailDecoded{};
    QNP_CHECK(decodePrefix(withTail.data(), withTail.size(), tailDecoded)
              == ParseStatus::Ok);

    prefix.featureFlags = kFeatureRequiredMask;
    bytes.clear();
    encodePrefix(prefix, bytes);
    QNP_CHECK(decodePrefix(bytes.data(), bytes.size(), decoded)
              == ParseStatus::Ok);
    QNP_CHECK(decoded.featureFlags == kFeatureRequiredMask);
}

QNP_TEST(ReplayPrefixRejectsEveryInvalidField) {
    ReplayPrefix prefix{};
    prefix.sessionUuidLow = 1u;
    prefix.sessionUuidHigh = 2u;
    std::vector<uint8_t> valid;
    encodePrefix(prefix, valid);
    QNP_REQUIRE(valid.size() == kPrefixBytes);

    ReplayPrefix sentinel{};
    sentinel.sessionUuidLow = 0xDEADBEEFDEADBEEFull;
    ReplayPrefix out = sentinel;

    std::vector<uint8_t> bad = valid;
    bad[0] ^= 0xFFu;
    QNP_CHECK(decodePrefix(bad.data(), bad.size(), out)
              == ParseStatus::BadMagic);

    for (size_t i = 4; i < kPrefixBytes; ++i) {
        bad = valid;
        bad[i] ^= 0x01u;
        QNP_CHECK(decodePrefix(bad.data(), bad.size(), out)
                  == ParseStatus::BadPrefixCrc);
    }

    for (size_t i = 36; i < 44; ++i) {
        bad = valid;
        bad[i] = 0x01u;
        reseal(bad);
        QNP_CHECK(decodePrefix(bad.data(), bad.size(), out)
                  == ParseStatus::UnsupportedContainerVersion);
    }

    bad = valid;
    writeLe16(bad, 10u, 64u);
    reseal(bad);
    QNP_CHECK(decodePrefix(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);

    bad = valid;
    writeLe32(bad, 12u, 0x01020304u);
    reseal(bad);
    QNP_CHECK(decodePrefix(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);

    bad = valid;
    writeLe16(bad, 4u, 0u);
    reseal(bad);
    QNP_CHECK(decodePrefix(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);

    bad = valid;
    writeLe16(bad, 8u, 0u);
    reseal(bad);
    QNP_CHECK(decodePrefix(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);

    bad = valid;
    writeLe16(bad, 8u, 2u);
    reseal(bad);
    QNP_CHECK(decodePrefix(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);

    bad = valid;
    writeLe16(bad, 4u, 2u);
    writeLe16(bad, 6u, 7u);
    reseal(bad);
    ReplayPrefix newer{};
    QNP_CHECK(decodePrefix(bad.data(), bad.size(), newer) == ParseStatus::Ok);
    QNP_CHECK(newer.containerMajor == 2u && newer.containerMinor == 7u);

    for (unsigned bit = 0; bit < 32u; ++bit) {
        const uint32_t mask = 1u << bit;
        if ((mask & kFeatureRequiredMask) != 0u)
            continue;
        bad = valid;
        writeLe32(bad, 16u, mask);
        reseal(bad);
        QNP_CHECK(decodePrefix(bad.data(), bad.size(), out)
                  == ParseStatus::UnsupportedFeature);
    }

    QNP_CHECK(out.sessionUuidLow == sentinel.sessionUuidLow);
}

QNP_TEST(ReplayPrefixTruncationIsSafe) {
    ReplayPrefix prefix{};
    prefix.sessionUuidLow = 0x1122334455667788ull;
    std::vector<uint8_t> bytes;
    encodePrefix(prefix, bytes);
    checkEveryTruncationFails(bytes, [](const uint8_t* data, size_t length) {
        ReplayPrefix out{};
        return decodePrefix(data, length, out);
    });
}

QNP_TEST(ReplayChunkHeaderGoldenBytesAndRoundTrip) {
    ChunkHeader header{};
    header.kind = ChunkKind::InputBlock;
    header.schemaVersion = 1u;
    header.critical = 1u;
    header.codec = ChunkCodec::Raw;
    header.sequence = 0x00010203u;
    header.matchId = 7u;
    header.firstTimelineFrame = 100u;
    header.lastTimelineFrame = 103u;
    header.recordCount = 4u;
    header.payloadBytes = 368u;
    header.decodedBytes = 368u;
    header.payloadCrc32c = 0xA1B2C3D4u;

    std::vector<uint8_t> bytes;
    encodeChunkHeader(header, bytes);
    QNP_REQUIRE(bytes.size() == kChunkHeaderBytes);

    const uint8_t expected[12] = {
        0x43u, 0x48u, 0x4Eu, 0x4Bu,
        0x04u, 0x00u,
        0x01u, 0x00u,
        0x01u,
        0x00u,
        0x00u, 0x00u,
    };
    QNP_CHECK(std::memcmp(bytes.data(), expected, sizeof(expected)) == 0);
    QNP_CHECK(readLe32(bytes, 12u) == 0x00010203u);
    QNP_CHECK(readLe32(bytes, 16u) == 7u);
    QNP_CHECK(readLe32(bytes, 20u) == 100u);
    QNP_CHECK(readLe32(bytes, 24u) == 103u);
    QNP_CHECK(readLe32(bytes, 28u) == 4u);
    QNP_CHECK(readLe32(bytes, 32u) == 368u);
    QNP_CHECK(readLe32(bytes, 36u) == 368u);
    QNP_CHECK(readLe32(bytes, 40u) == 0xA1B2C3D4u);

    ChunkHeader decoded{};
    QNP_CHECK(decodeChunkHeader(bytes.data(), bytes.size(), decoded)
              == ParseStatus::Ok);
    QNP_CHECK(decoded.kind == header.kind);
    QNP_CHECK(decoded.schemaVersion == header.schemaVersion);
    QNP_CHECK(decoded.critical == header.critical);
    QNP_CHECK(decoded.codec == header.codec);
    QNP_CHECK(decoded.sequence == header.sequence);
    QNP_CHECK(decoded.matchId == header.matchId);
    QNP_CHECK(decoded.firstTimelineFrame == header.firstTimelineFrame);
    QNP_CHECK(decoded.lastTimelineFrame == header.lastTimelineFrame);
    QNP_CHECK(decoded.recordCount == header.recordCount);
    QNP_CHECK(decoded.payloadBytes == header.payloadBytes);
    QNP_CHECK(decoded.decodedBytes == header.decodedBytes);
    QNP_CHECK(decoded.payloadCrc32c == header.payloadCrc32c);
    QNP_CHECK(decoded.headerCrc32c == readLe32(bytes, 44u));

    for (uint16_t kind = 1u; kind <= 10u; ++kind) {
        ChunkHeader probe{};
        probe.kind = static_cast<ChunkKind>(kind);
        probe.schemaVersion = 1u;
        probe.critical = isCriticalChunkKind(probe.kind) ? 1u : 0u;
        probe.codec = ChunkCodec::Raw;
        std::vector<uint8_t> probeBytes;
        encodeChunkHeader(probe, probeBytes);
        ChunkHeader probeDecoded{};
        QNP_CHECK(decodeChunkHeader(
                      probeBytes.data(), probeBytes.size(), probeDecoded)
                  == ParseStatus::Ok);
        QNP_CHECK(probeDecoded.kind == probe.kind);
        QNP_CHECK(probeDecoded.critical == probe.critical);
    }
}

QNP_TEST(ReplayChunkHeaderRejectsEveryInvalidField) {
    ChunkHeader header{};
    header.kind = ChunkKind::EventBlock;
    header.schemaVersion = 1u;
    header.critical = 0u;
    header.codec = ChunkCodec::Raw;
    header.sequence = 12u;
    header.recordCount = 2u;
    header.payloadBytes = 152u;
    header.decodedBytes = 152u;
    std::vector<uint8_t> valid;
    encodeChunkHeader(header, valid);

    ChunkHeader sentinel{};
    sentinel.matchId = 0xFEEDFACEu;
    ChunkHeader out = sentinel;
    ChunkHeader accepted{};

    std::vector<uint8_t> bad = valid;
    writeLe32(bad, 0u, 0x12345678u);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::BadSyncWord);

    for (size_t i = 4; i < kChunkHeaderBytes; ++i) {
        bad = valid;
        bad[i] ^= 0x80u;
        QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
                  == ParseStatus::BadChunkHeaderCrc);
    }

    for (size_t i = 10; i < 12; ++i) {
        bad = valid;
        bad[i] = 0x01u;
        reseal(bad);
        QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
                  == ParseStatus::UnsupportedContainerVersion);
    }

    const uint16_t unknownKinds[] = { 0u, 11u, 0x00FFu, 0xFFFFu };
    for (size_t i = 0; i < sizeof(unknownKinds) / sizeof(unknownKinds[0]);
         ++i) {
        bad = valid;
        writeLe16(bad, 4u, unknownKinds[i]);
        reseal(bad);
        QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
                  == ParseStatus::UnknownCriticalChunk);
    }

    bad = valid;
    writeLe16(bad, 6u, 0u);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::UnknownCriticalChunk);

    bad = valid;
    bad[8] = 1u;
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::UnknownCriticalChunk);
    bad = valid;
    bad[8] = 2u;
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::UnknownCriticalChunk);

    ChunkHeader inputHeader{};
    inputHeader.kind = ChunkKind::InputBlock;
    inputHeader.schemaVersion = 1u;
    inputHeader.critical = 1u;
    inputHeader.recordCount = 1u;
    inputHeader.firstTimelineFrame = 5u;
    inputHeader.lastTimelineFrame = 5u;
    std::vector<uint8_t> inputValid;
    encodeChunkHeader(inputHeader, inputValid);
    QNP_CHECK(decodeChunkHeader(inputValid.data(), inputValid.size(), accepted)
              == ParseStatus::Ok);
    bad = inputValid;
    bad[8] = 0u;
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::UnknownCriticalChunk);

    for (unsigned codec = 1u; codec < 256u; ++codec) {
        bad = valid;
        bad[9] = static_cast<uint8_t>(codec);
        reseal(bad);
        QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
                  == ParseStatus::UnknownChunkCodec);
    }

    bad = valid;
    writeLe32(bad, 12u, kMaxChunksPerFile);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = valid;
    writeLe32(bad, 12u, kMaxChunksPerFile - 1u);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), accepted)
              == ParseStatus::Ok);

    bad = valid;
    writeLe32(bad, 32u, kMaxChunkPayloadBytes + 1u);
    writeLe32(bad, 36u, kMaxChunkPayloadBytes + 1u);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);

    bad = valid;
    writeLe32(bad, 36u, 153u);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);

    bad = valid;
    writeLe32(bad, 20u, kMaxTimelineFrames + 1u);
    writeLe32(bad, 24u, kMaxTimelineFrames + 1u);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);

    bad = valid;
    writeLe32(bad, 20u, 10u);
    writeLe32(bad, 24u, 9u);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::FrameConflict);

    bad = valid;
    writeLe32(bad, 28u, kMaxEventsPerBlock + 1u);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = valid;
    writeLe32(bad, 28u, kMaxEventsPerBlock);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), accepted)
              == ParseStatus::Ok);

    bad = inputValid;
    writeLe32(bad, 28u, kMaxRecordsPerInputBlock + 1u);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);

    bad = inputValid;
    writeLe32(bad, 24u, 7u);
    reseal(bad);
    QNP_CHECK(decodeChunkHeader(bad.data(), bad.size(), out)
              == ParseStatus::FrameGap);

    QNP_CHECK(out.matchId == sentinel.matchId);
}

QNP_TEST(ReplayChunkHeaderTruncationIsSafe) {
    ChunkHeader header{};
    header.kind = ChunkKind::MatchStart;
    header.schemaVersion = 1u;
    header.critical = 1u;
    header.matchId = 3u;
    std::vector<uint8_t> bytes;
    encodeChunkHeader(header, bytes);
    checkEveryTruncationFails(bytes, [](const uint8_t* data, size_t length) {
        ChunkHeader out{};
        return decodeChunkHeader(data, length, out);
    });
}

QNP_TEST(ReplayManifestRoundTrip) {
    const SessionManifest manifest = sampleManifest();
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeSessionManifest(manifest, bytes));

    SessionManifest decoded{};
    QNP_REQUIRE(decodeSessionManifest(bytes.data(), bytes.size(), decoded)
                == ParseStatus::Ok);

    QNP_CHECK(decoded.contract.executableSha256
              == manifest.contract.executableSha256);
    QNP_CHECK(decoded.contract.contentManifestRoot
              == manifest.contract.contentManifestRoot);
    QNP_CHECK(decoded.contract.runtimeSimulationId
              == manifest.contract.runtimeSimulationId);
    QNP_CHECK(decoded.contract.stateRegionFingerprint
              == manifest.contract.stateRegionFingerprint);
    QNP_CHECK(decoded.contract.stateHashProjectionId
              == manifest.contract.stateHashProjectionId);
    QNP_CHECK(decoded.contract.stateHashAlgorithmId
              == manifest.contract.stateHashAlgorithmId);
    QNP_CHECK(decoded.contract.bootstrapCodecVersion
              == manifest.contract.bootstrapCodecVersion);
    QNP_CHECK(decoded.contract.eventSchemaVersion
              == manifest.contract.eventSchemaVersion);
    QNP_CHECK(decoded.contract.characterCatalogId
              == manifest.contract.characterCatalogId);
    QNP_CHECK(decoded.contract.fpuControlWord
              == manifest.contract.fpuControlWord);
    QNP_CHECK(decoded.contract.mxcsr == manifest.contract.mxcsr);
    QNP_CHECK(decoded.bootstrapKind == manifest.bootstrapKind);
    QNP_CHECK(decoded.vsRow == manifest.vsRow);
    QNP_CHECK(decoded.cpuMask == manifest.cpuMask);
    QNP_CHECK(decoded.cpuContract == manifest.cpuContract);
    QNP_CHECK(decoded.slotToSourceRoute == manifest.slotToSourceRoute);
    QNP_CHECK(decoded.rosterEpoch == manifest.rosterEpoch);
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        QNP_CHECK(decoded.lanes[lane].active == manifest.lanes[lane].active);
        QNP_CHECK(decoded.lanes[lane].name == manifest.lanes[lane].name);
    }
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        QNP_CHECK(decoded.slots[slot].ownership
                  == manifest.slots[slot].ownership);
        QNP_CHECK(decoded.slots[slot].side == manifest.slots[slot].side);
        QNP_CHECK(decoded.slots[slot].identity.nativeCharacterId
                  == manifest.slots[slot].identity.nativeCharacterId);
        QNP_CHECK(decoded.slots[slot].identity.paletteIndex
                  == manifest.slots[slot].identity.paletteIndex);
        QNP_CHECK(decoded.slots[slot].selectorProofPresent
                  == manifest.slots[slot].selectorProofPresent);
        QNP_CHECK(decoded.slots[slot].selectorProof.cursorCell
                  == manifest.slots[slot].selectorProof.cursorCell);
        QNP_CHECK(decoded.slots[slot].selectorProof.alternateFlag
                  == manifest.slots[slot].selectorProof.alternateFlag);
        QNP_CHECK(decoded.slots[slot].selectorProof.confirmState
                  == manifest.slots[slot].selectorProof.confirmState);
    }
    QNP_CHECK(decoded.display.recordedUnixSeconds
              == manifest.display.recordedUnixSeconds);
    QNP_CHECK(decoded.display.timezoneOffsetMinutes
              == manifest.display.timezoneOffsetMinutes);
    QNP_CHECK(decoded.display.sourceKind == manifest.display.sourceKind);
    QNP_CHECK(decoded.display.routeLabel == manifest.display.routeLabel);
    QNP_CHECK(decoded.display.playerOneDelay
              == manifest.display.playerOneDelay);
    QNP_CHECK(decoded.display.playerTwoDelay
              == manifest.display.playerTwoDelay);
    QNP_CHECK(decoded.display.rollbackWindow
              == manifest.display.rollbackWindow);
    QNP_CHECK(decoded.display.averageRttMicros
              == manifest.display.averageRttMicros);
    QNP_CHECK(decoded.display.lossPermille == manifest.display.lossPermille);
    QNP_CHECK(decoded.display.networkMetadataPresent
              == manifest.display.networkMetadataPresent);
    QNP_CHECK(decoded.display.title == manifest.display.title);

    std::vector<uint8_t> again;
    QNP_CHECK(encodeSessionManifest(decoded, again));
    QNP_CHECK(again == bytes);

    std::vector<uint8_t> minimal;
    QNP_CHECK(encodeSessionManifest(SessionManifest{}, minimal));
    SessionManifest minimalDecoded{};
    QNP_CHECK(decodeSessionManifest(
                  minimal.data(), minimal.size(), minimalDecoded)
              == ParseStatus::Ok);
}

QNP_TEST(ReplayManifestRejectsInvalidModelsAndBytes) {
    SessionManifest manifest = sampleManifest();
    manifest.bootstrapKind = static_cast<BootstrapKind>(4);
    std::vector<uint8_t> bytes;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    QNP_CHECK(bytes.empty());

    manifest = sampleManifest();
    manifest.cpuContract = static_cast<CpuContract>(3);
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.display.sourceKind = static_cast<ReplaySourceKind>(5);
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.vsRow = 6u;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.cpuMask = 0x10u;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.slotToSourceRoute[2] = 4u;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.slots[1].identity.nativeCharacterId = 34u;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.slots[1].identity.paletteIndex = 4u;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.slots[1].selectorProof.cursorCell = 24u;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.slots[1].selectorProof.confirmState = 5u;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.slots[2].selectorProofPresent = false;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.slots[1].side = 2u;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.display.recordedUnixSeconds = -1;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.display.timezoneOffsetMinutes = 1441;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.display.lossPermille = 1001u;
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.display.title = std::string(kMaxLabelBytes + 1u, 'T');
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    manifest = sampleManifest();
    manifest.lanes[0].name = "bad\x1b[31m";
    QNP_CHECK(!encodeSessionManifest(manifest, bytes));
    QNP_CHECK(bytes.empty());

    std::vector<uint8_t> valid = encodedManifest();
    QNP_REQUIRE(valid.size() > kSimulationContractBytes + 8u);
    SessionManifest sentinel{};
    sentinel.rosterEpoch = 0xABCDEF01u;
    SessionManifest out = sentinel;

    const size_t bootstrapOffset = kSimulationContractBytes;
    const size_t vsRowOffset = bootstrapOffset + 1u;
    const size_t cpuMaskOffset = bootstrapOffset + 2u;
    const size_t cpuContractOffset = bootstrapOffset + 3u;
    const size_t routeOffset = bootstrapOffset + 4u;
    const size_t laneZeroActiveOffset = routeOffset + 4u;

    std::vector<uint8_t> bad = valid;
    bad[bootstrapOffset] = 4u;
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedFeature);
    bad = valid;
    bad[bootstrapOffset] = 0u;
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedFeature);
    bad = valid;
    bad[cpuContractOffset] = 3u;
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedFeature);
    bad = valid;
    bad[vsRowOffset] = 6u;
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::BadLaneShape);
    bad = valid;
    bad[cpuMaskOffset] = 0x10u;
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::BadLaneShape);
    bad = valid;
    bad[routeOffset + 1u] = 4u;
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::BadLaneShape);
    bad = valid;
    bad[laneZeroActiveOffset] = 2u;
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::BadLaneShape);
    bad = valid;
    writeLe16(bad, laneZeroActiveOffset + 1u,
              static_cast<uint16_t>(kMaxLabelBytes + 1u));
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = valid;
    bad[laneZeroActiveOffset + 3u] = 0x1Bu;
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::BadUtf8);

    bad = valid;
    bad.push_back(0x00u);
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);

    bad = valid;
    bad[bad.size() - 4u] = 1u;
    QNP_CHECK(decodeSessionManifest(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);

    QNP_CHECK(out.rosterEpoch == sentinel.rosterEpoch);
}

QNP_TEST(ReplayManifestTruncationIsSafe) {
    const std::vector<uint8_t> bytes = encodedManifest();
    checkEveryTruncationFails(bytes, [](const uint8_t* data, size_t length) {
        SessionManifest out{};
        return decodeSessionManifest(data, length, out);
    });
}

QNP_TEST(ReplayDeterministicStartRoundTripAndRejections) {
    const DeterministicStart start = sampleStart(1234u);
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeDeterministicStart(start, bytes));
    QNP_REQUIRE(bytes.size() == kDeterministicStartBytes);

    DeterministicStart decoded{};
    QNP_REQUIRE(decodeDeterministicStart(bytes.data(), bytes.size(), decoded)
                == ParseStatus::Ok);
    QNP_CHECK(decoded.seedGameplay == start.seedGameplay);
    QNP_CHECK(decoded.seedCosmetic == start.seedCosmetic);
    QNP_CHECK(decoded.selector == start.selector);
    QNP_CHECK(decoded.initialPhase == start.initialPhase);
    QNP_CHECK(decoded.bootstrapHash == start.bootstrapHash);
    QNP_CHECK(decoded.timerMode == start.timerMode);
    QNP_CHECK(decoded.aiDifficulty == start.aiDifficulty);
    QNP_CHECK(decoded.lifeMultiplier == start.lifeMultiplier);
    QNP_CHECK(decoded.roundsToWinMinusOne == start.roundsToWinMinusOne);
    QNP_CHECK(decoded.playerOneNameCodePage == start.playerOneNameCodePage);
    QNP_CHECK(decoded.playerTwoNameCodePage == start.playerTwoNameCodePage);
    QNP_CHECK(decoded.displayNames == start.displayNames);
    QNP_CHECK(decoded.firstTimelineFrame == start.firstTimelineFrame);
    QNP_CHECK(decoded.initialStateHash == start.initialStateHash);
    QNP_CHECK(decoded.initialDigest == start.initialDigest);

    std::vector<uint8_t> again;
    QNP_CHECK(encodeDeterministicStart(decoded, again));
    QNP_CHECK(again == bytes);

    DeterministicStart overLimit = start;
    overLimit.firstTimelineFrame = kMaxTimelineFrames + 1u;
    std::vector<uint8_t> rejected;
    QNP_CHECK(!encodeDeterministicStart(overLimit, rejected));
    QNP_CHECK(rejected.empty());
    overLimit.firstTimelineFrame = kMaxTimelineFrames;
    QNP_CHECK(encodeDeterministicStart(overLimit, rejected));

    DeterministicStart names = start;
    names.displayNames[0].fill('A');
    QNP_CHECK(!encodeDeterministicStart(names, rejected));
    names = start;
    for (size_t i = 0; i < 21u; ++i)
        names.displayNames[1][i] = 'B';
    names.displayNames[1][21] = '\0';
    QNP_CHECK(!encodeDeterministicStart(names, rejected));
    names = start;
    names.displayNames[2][31] = 'C';
    QNP_CHECK(!encodeDeterministicStart(names, rejected));
    names = start;
    names.displayNames[3][0] = '\x1b';
    names.displayNames[3][1] = '\0';
    QNP_CHECK(!encodeDeterministicStart(names, rejected));

    names = start;
    for (size_t i = 0; i < 20u; ++i)
        names.displayNames[0][i] = 'D';
    names.displayNames[0][20] = '\0';
    QNP_CHECK(encodeDeterministicStart(names, rejected));

    DeterministicStart sentinel{};
    sentinel.seedGameplay = 0x5A5A5A5Au;
    DeterministicStart out = sentinel;

    std::vector<uint8_t> bad = bytes;
    bad[kDeterministicStartBytes - 1u] = 1u;
    QNP_CHECK(decodeDeterministicStart(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);
    bad = bytes;
    writeLe32(bad, 176u, kMaxTimelineFrames + 1u);
    QNP_CHECK(decodeDeterministicStart(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = bytes;
    bad[48u + 31u] = 'x';
    QNP_CHECK(decodeDeterministicStart(bad.data(), bad.size(), out)
              == ParseStatus::BadLaneShape);
    bad = bytes;
    bad.push_back(0u);
    QNP_CHECK(decodeDeterministicStart(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    QNP_CHECK(out.seedGameplay == sentinel.seedGameplay);
}

QNP_TEST(ReplayDeterministicStartTruncationIsSafe) {
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeDeterministicStart(sampleStart(10u), bytes));
    checkEveryTruncationFails(bytes, [](const uint8_t* data, size_t length) {
        DeterministicStart out{};
        return decodeDeterministicStart(data, length, out);
    });
}

QNP_TEST(ReplayMatchStartRoundTripAndRejections) {
    const MatchStartRecord record = sampleMatchStart();
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeMatchStart(record, bytes));
    QNP_REQUIRE(bytes.size() == kMatchStartBytes);

    MatchStartRecord decoded{};
    QNP_REQUIRE(decodeMatchStart(bytes.data(), bytes.size(), decoded)
                == ParseStatus::Ok);
    QNP_CHECK(decoded.matchId == record.matchId);
    QNP_CHECK(decoded.ordinal == record.ordinal);
    QNP_CHECK(decoded.firstTimelineFrame == record.firstTimelineFrame);
    QNP_CHECK(decoded.dependsOnMatchId == record.dependsOnMatchId);
    QNP_CHECK(decoded.independentBootstrap == record.independentBootstrap);
    QNP_CHECK(decoded.start.bootstrapHash == record.start.bootstrapHash);
    QNP_CHECK(decoded.start.displayNames == record.start.displayNames);
    QNP_CHECK(decoded.slotToSourceRoute == record.slotToSourceRoute);
    QNP_CHECK(decoded.vsRow == record.vsRow);
    QNP_CHECK(decoded.cpuMask == record.cpuMask);
    QNP_CHECK(decoded.rosterEpoch == record.rosterEpoch);
    QNP_CHECK(decoded.stageId == record.stageId);
    QNP_CHECK(decoded.stageVerified == record.stageVerified);
    for (size_t slot = 0; slot < kFighterSlots; ++slot) {
        QNP_CHECK(decoded.slots[slot].ownership
                  == record.slots[slot].ownership);
        QNP_CHECK(decoded.slots[slot].identity.nativeCharacterId
                  == record.slots[slot].identity.nativeCharacterId);
    }
    std::vector<uint8_t> again;
    QNP_CHECK(encodeMatchStart(decoded, again));
    QNP_CHECK(again == bytes);

    MatchStartRecord dependent = record;
    dependent.matchId = 9u;
    dependent.dependsOnMatchId = 7u;
    dependent.independentBootstrap = false;
    std::vector<uint8_t> dependentBytes;
    QNP_CHECK(encodeMatchStart(dependent, dependentBytes));
    MatchStartRecord dependentDecoded{};
    QNP_CHECK(decodeMatchStart(
                  dependentBytes.data(), dependentBytes.size(),
                  dependentDecoded)
              == ParseStatus::Ok);
    QNP_CHECK(dependentDecoded.dependsOnMatchId == 7u);
    QNP_CHECK(!dependentDecoded.independentBootstrap);

    std::vector<uint8_t> rejected;
    MatchStartRecord invalid = record;
    invalid.matchId = 0u;
    QNP_CHECK(!encodeMatchStart(invalid, rejected));
    invalid = record;
    invalid.ordinal = kMaxMatchesPerSet;
    QNP_CHECK(!encodeMatchStart(invalid, rejected));
    invalid = record;
    invalid.ordinal = kMaxMatchesPerSet - 1u;
    QNP_CHECK(encodeMatchStart(invalid, rejected));
    rejected.clear();
    invalid = record;
    invalid.dependsOnMatchId = invalid.matchId;
    invalid.independentBootstrap = false;
    QNP_CHECK(!encodeMatchStart(invalid, rejected));
    invalid = record;
    invalid.independentBootstrap = false;
    QNP_CHECK(!encodeMatchStart(invalid, rejected));
    invalid = record;
    invalid.start.firstTimelineFrame += 1u;
    QNP_CHECK(!encodeMatchStart(invalid, rejected));
    invalid = record;
    invalid.slots[0].side = 3u;
    QNP_CHECK(!encodeMatchStart(invalid, rejected));
    invalid = record;
    invalid.slotToSourceRoute[0] = 9u;
    QNP_CHECK(!encodeMatchStart(invalid, rejected));
    QNP_CHECK(rejected.empty());

    MatchStartRecord sentinel{};
    sentinel.matchId = 0xFACEFEEDu;
    MatchStartRecord out = sentinel;
    std::vector<uint8_t> bad = bytes;
    writeLe32(bad, 0u, 0u);
    QNP_CHECK(decodeMatchStart(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = bytes;
    writeLe32(bad, 4u, kMaxMatchesPerSet);
    QNP_CHECK(decodeMatchStart(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = bytes;
    bad[16] = 2u;
    QNP_CHECK(decodeMatchStart(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = bytes;
    bad[19] = 2u;
    QNP_CHECK(decodeMatchStart(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = bytes;
    bad[16] = 0u;
    QNP_CHECK(decodeMatchStart(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = bytes;
    bad[17] = 6u;
    QNP_CHECK(decodeMatchStart(bad.data(), bad.size(), out)
              == ParseStatus::BadLaneShape);
    bad = bytes;
    bad[18] = 0x20u;
    QNP_CHECK(decodeMatchStart(bad.data(), bad.size(), out)
              == ParseStatus::BadLaneShape);
    bad = bytes;
    bad[bytes.size() - 1u] = 1u;
    QNP_CHECK(decodeMatchStart(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);
    bad = bytes;
    bad.push_back(0u);
    QNP_CHECK(decodeMatchStart(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    QNP_CHECK(out.matchId == sentinel.matchId);
}

QNP_TEST(ReplayMatchEndRoundTripAndRejections) {
    const MatchEndRecord record = sampleMatchEnd();
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeMatchEnd(record, bytes));
    QNP_REQUIRE(bytes.size() == kMatchEndBytes);

    MatchEndRecord decoded{};
    QNP_REQUIRE(decodeMatchEnd(bytes.data(), bytes.size(), decoded)
                == ParseStatus::Ok);
    QNP_CHECK(decoded.matchId == record.matchId);
    QNP_CHECK(decoded.lastTimelineFrame == record.lastTimelineFrame);
    QNP_CHECK(decoded.winningSideMask == record.winningSideMask);
    QNP_CHECK(decoded.roundsSideZero == record.roundsSideZero);
    QNP_CHECK(decoded.roundsSideOne == record.roundsSideOne);
    QNP_CHECK(decoded.reason == record.reason);
    QNP_CHECK(decoded.postTailStateHash == record.postTailStateHash);
    QNP_CHECK(decoded.rollingDigest == record.rollingDigest);
    std::vector<uint8_t> again;
    QNP_CHECK(encodeMatchEnd(decoded, again));
    QNP_CHECK(again == bytes);

    for (uint16_t reason = 1u; reason <= 6u; ++reason) {
        MatchEndRecord probe = record;
        probe.reason = static_cast<TerminalReason>(reason);
        std::vector<uint8_t> probeBytes;
        QNP_CHECK(encodeMatchEnd(probe, probeBytes));
        MatchEndRecord probeDecoded{};
        QNP_CHECK(decodeMatchEnd(
                      probeBytes.data(), probeBytes.size(), probeDecoded)
                  == ParseStatus::Ok);
        QNP_CHECK(probeDecoded.reason == probe.reason);
    }

    std::vector<uint8_t> rejected;
    MatchEndRecord invalid = record;
    invalid.matchId = 0u;
    QNP_CHECK(!encodeMatchEnd(invalid, rejected));
    invalid = record;
    invalid.reason = static_cast<TerminalReason>(7);
    QNP_CHECK(!encodeMatchEnd(invalid, rejected));
    invalid = record;
    invalid.winningSideMask = 0x10u;
    QNP_CHECK(!encodeMatchEnd(invalid, rejected));
    invalid = record;
    invalid.lastTimelineFrame = kMaxTimelineFrames + 1u;
    QNP_CHECK(!encodeMatchEnd(invalid, rejected));
    QNP_CHECK(rejected.empty());

    MatchEndRecord sentinel{};
    sentinel.matchId = 0x1234u;
    MatchEndRecord out = sentinel;
    std::vector<uint8_t> bad = bytes;
    writeLe16(bad, 8u, 7u);
    QNP_CHECK(decodeMatchEnd(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedFeature);
    bad = bytes;
    writeLe16(bad, 8u, 0u);
    QNP_CHECK(decodeMatchEnd(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedFeature);
    bad = bytes;
    bad[10] = 0x10u;
    QNP_CHECK(decodeMatchEnd(bad.data(), bad.size(), out)
              == ParseStatus::BadLaneShape);
    bad = bytes;
    writeLe32(bad, 0u, 0u);
    QNP_CHECK(decodeMatchEnd(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = bytes;
    writeLe32(bad, 4u, kMaxTimelineFrames + 1u);
    QNP_CHECK(decodeMatchEnd(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    for (size_t i = 13; i < 16; ++i) {
        bad = bytes;
        bad[i] = 1u;
        QNP_CHECK(decodeMatchEnd(bad.data(), bad.size(), out)
                  == ParseStatus::UnsupportedContainerVersion);
    }
    bad = bytes;
    bad.push_back(0u);
    QNP_CHECK(decodeMatchEnd(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    QNP_CHECK(out.matchId == sentinel.matchId);
}

QNP_TEST(ReplaySessionEndRoundTripAndRejections) {
    const SessionEndRecord record = sampleSessionEnd();
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeSessionEnd(record, bytes));
    QNP_REQUIRE(bytes.size() == kSessionEndBytes);

    SessionEndRecord decoded{};
    QNP_REQUIRE(decodeSessionEnd(bytes.data(), bytes.size(), decoded)
                == ParseStatus::Ok);
    QNP_CHECK(decoded.lastTimelineFrame == record.lastTimelineFrame);
    QNP_CHECK(decoded.reason == record.reason);
    QNP_CHECK(decoded.postTailStateHash == record.postTailStateHash);
    QNP_CHECK(decoded.rollingDigest == record.rollingDigest);
    QNP_CHECK(decoded.matchCount == record.matchCount);
    std::vector<uint8_t> again;
    QNP_CHECK(encodeSessionEnd(decoded, again));
    QNP_CHECK(again == bytes);

    std::vector<uint8_t> rejected;
    SessionEndRecord invalid = record;
    invalid.matchCount = kMaxMatchesPerSet + 1u;
    QNP_CHECK(!encodeSessionEnd(invalid, rejected));
    invalid.matchCount = kMaxMatchesPerSet;
    QNP_CHECK(encodeSessionEnd(invalid, rejected));
    rejected.clear();
    invalid = record;
    invalid.reason = static_cast<TerminalReason>(0);
    QNP_CHECK(!encodeSessionEnd(invalid, rejected));
    invalid = record;
    invalid.lastTimelineFrame = kMaxTimelineFrames + 1u;
    QNP_CHECK(!encodeSessionEnd(invalid, rejected));
    QNP_CHECK(rejected.empty());

    SessionEndRecord sentinel{};
    sentinel.matchCount = 0x99u;
    SessionEndRecord out = sentinel;
    std::vector<uint8_t> bad = bytes;
    writeLe16(bad, 6u, 1u);
    QNP_CHECK(decodeSessionEnd(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);
    bad = bytes;
    writeLe32(bad, 28u, 1u);
    QNP_CHECK(decodeSessionEnd(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);
    bad = bytes;
    writeLe16(bad, 4u, 9u);
    QNP_CHECK(decodeSessionEnd(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedFeature);
    bad = bytes;
    writeLe32(bad, 24u, kMaxMatchesPerSet + 1u);
    QNP_CHECK(decodeSessionEnd(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = bytes;
    bad.push_back(0u);
    QNP_CHECK(decodeSessionEnd(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    QNP_CHECK(out.matchCount == sentinel.matchCount);
}

QNP_TEST(ReplayMatchRecordTruncationIsSafe) {
    std::vector<uint8_t> matchStart;
    QNP_REQUIRE(encodeMatchStart(sampleMatchStart(), matchStart));
    checkEveryTruncationFails(
        matchStart, [](const uint8_t* data, size_t length) {
            MatchStartRecord out{};
            return decodeMatchStart(data, length, out);
        });

    std::vector<uint8_t> matchEnd;
    QNP_REQUIRE(encodeMatchEnd(sampleMatchEnd(), matchEnd));
    checkEveryTruncationFails(
        matchEnd, [](const uint8_t* data, size_t length) {
            MatchEndRecord out{};
            return decodeMatchEnd(data, length, out);
        });

    std::vector<uint8_t> sessionEnd;
    QNP_REQUIRE(encodeSessionEnd(sampleSessionEnd(), sessionEnd));
    checkEveryTruncationFails(
        sessionEnd, [](const uint8_t* data, size_t length) {
            SessionEndRecord out{};
            return decodeSessionEnd(data, length, out);
        });
}

QNP_TEST(ReplayInputBlockRoundTripAcrossCounts) {
    const size_t counts[] = { 0u, 1u, 2u, kMaxRecordsPerInputBlock };
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        const size_t count = counts[i];
        std::vector<ReplayFrameV1> frames;
        for (size_t f = 0; f < count; ++f)
            frames.push_back(sampleFrame(1000u + static_cast<uint32_t>(f)));

        std::vector<uint8_t> bytes;
        QNP_REQUIRE(encodeInputBlock(
            frames.empty() ? nullptr : frames.data(), frames.size(), bytes));
        QNP_REQUIRE(bytes.size()
                    == kInputBlockHeaderBytes + count * kInputRecordBytes);

        std::vector<ReplayFrameV1> decoded;
        QNP_REQUIRE(decodeInputBlock(
                        bytes.data(), bytes.size(), count == 0u ? 0u : 1000u,
                        static_cast<uint32_t>(count), decoded)
                    == ParseStatus::Ok);
        QNP_REQUIRE(decoded.size() == count);
        for (size_t f = 0; f < count; ++f) {
            QNP_CHECK(decoded[f] == frames[f]);
            QNP_CHECK(decoded[f].timelineFrame
                      == 1000u + static_cast<uint32_t>(f));
        }
        std::vector<uint8_t> again;
        QNP_CHECK(encodeInputBlock(
            decoded.empty() ? nullptr : decoded.data(), decoded.size(),
            again));
        QNP_CHECK(again == bytes);
    }

    std::vector<ReplayFrameV1> tooMany;
    for (size_t f = 0; f <= kMaxRecordsPerInputBlock; ++f)
        tooMany.push_back(sampleFrame(static_cast<uint32_t>(f)));
    std::vector<uint8_t> rejected;
    QNP_CHECK(!encodeInputBlock(tooMany.data(), tooMany.size(), rejected));
    QNP_CHECK(rejected.empty());

    std::vector<ReplayFrameV1> sentinel(1u, sampleFrame(4u));
    std::vector<ReplayFrameV1> out = sentinel;
    QNP_CHECK(decodeInputBlock(
                  nullptr, 0u, 0u, kMaxRecordsPerInputBlock + 1u, out)
              == ParseStatus::LimitExceeded);
    QNP_CHECK(out.size() == 1u && out[0] == sentinel[0]);

    ReplayFrameV1 saturated = sampleFrame(50u);
    saturated.activeLaneMask = 0x0Fu;
    for (size_t lane = 0; lane < kSourceLanes; ++lane)
        saturated.actions[lane] = kActionMask;
    std::vector<uint8_t> saturatedBytes;
    QNP_REQUIRE(encodeInputBlock(&saturated, 1u, saturatedBytes));
    std::vector<ReplayFrameV1> saturatedDecoded;
    QNP_CHECK(decodeInputBlock(
                  saturatedBytes.data(), saturatedBytes.size(), 50u, 1u,
                  saturatedDecoded)
              == ParseStatus::Ok);
    QNP_REQUIRE(saturatedDecoded.size() == 1u);
    QNP_CHECK(saturatedDecoded[0] == saturated);

    ReplayFrameV1 effective = sampleFrame(51u);
    effective.inputDomain = InputDomain::EffectiveFighterV1;
    std::vector<uint8_t> effectiveBytes;
    QNP_REQUIRE(encodeInputBlock(&effective, 1u, effectiveBytes));
    std::vector<ReplayFrameV1> effectiveDecoded;
    QNP_CHECK(decodeInputBlock(
                  effectiveBytes.data(), effectiveBytes.size(), 51u, 1u,
                  effectiveDecoded)
              == ParseStatus::Ok);
    QNP_REQUIRE(effectiveDecoded.size() == 1u);
    QNP_CHECK(effectiveDecoded[0].inputDomain
              == InputDomain::EffectiveFighterV1);
}

QNP_TEST(ReplayInputBlockFrameSequencing) {
    std::vector<uint8_t> rejected;

    std::vector<ReplayFrameV1> gapped;
    gapped.push_back(sampleFrame(10u));
    gapped.push_back(sampleFrame(12u));
    QNP_CHECK(!encodeInputBlock(gapped.data(), gapped.size(), rejected));

    std::vector<ReplayFrameV1> duplicated;
    duplicated.push_back(sampleFrame(10u));
    duplicated.push_back(sampleFrame(10u));
    QNP_CHECK(!encodeInputBlock(
        duplicated.data(), duplicated.size(), rejected));

    std::vector<ReplayFrameV1> descending;
    descending.push_back(sampleFrame(11u));
    descending.push_back(sampleFrame(10u));
    QNP_CHECK(!encodeInputBlock(
        descending.data(), descending.size(), rejected));

    std::vector<ReplayFrameV1> conflicting;
    conflicting.push_back(sampleFrame(10u));
    ReplayFrameV1 conflict = sampleFrame(10u);
    conflict.actions[0] = 0x0202u;
    conflicting.push_back(conflict);
    QNP_CHECK(!encodeInputBlock(
        conflicting.data(), conflicting.size(), rejected));
    QNP_CHECK(rejected.empty());

    std::vector<ReplayFrameV1> overrun;
    overrun.push_back(sampleFrame(kMaxTimelineFrames));
    overrun.push_back(sampleFrame(kMaxTimelineFrames + 1u));
    QNP_CHECK(!encodeInputBlock(overrun.data(), overrun.size(), rejected));

    const std::vector<uint8_t> bytes = encodedInputBlock(1000u, 3u);
    std::vector<ReplayFrameV1> sentinel(1u, sampleFrame(4u));
    std::vector<ReplayFrameV1> out = sentinel;
    QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 999u, 3u, out)
              == ParseStatus::FrameGap);
    QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 1001u, 3u, out)
              == ParseStatus::FrameGap);
    QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 0u, 3u, out)
              == ParseStatus::FrameGap);
    QNP_CHECK(out.size() == 1u && out[0] == sentinel[0]);

    QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 1000u, 2u, out)
              == ParseStatus::LimitExceeded);
    QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 1000u, 4u, out)
              == ParseStatus::Truncated);

    std::vector<uint8_t> padded = bytes;
    padded.push_back(0u);
    QNP_CHECK(decodeInputBlock(padded.data(), padded.size(), 1000u, 3u, out)
              == ParseStatus::LimitExceeded);

    std::vector<uint8_t> bad = bytes;
    writeLe32(bad, 4u, 1u);
    QNP_CHECK(decodeInputBlock(bad.data(), bad.size(), 1000u, 3u, out)
              == ParseStatus::UnsupportedContainerVersion);

    std::vector<uint8_t> crafted(
        kInputBlockHeaderBytes + 2u * kInputRecordBytes, 0u);
    writeLe32(crafted, 0u, kMaxTimelineFrames);
    QNP_CHECK(decodeInputBlock(
                  crafted.data(), crafted.size(), kMaxTimelineFrames, 2u, out)
              == ParseStatus::LimitExceeded);

    std::vector<uint8_t> wrapping(
        kInputBlockHeaderBytes
            + static_cast<size_t>(kMaxRecordsPerInputBlock) * kInputRecordBytes,
        0u);
    writeLe32(wrapping, 0u, 0xFFFFFFF0u);
    QNP_CHECK(decodeInputBlock(
                  wrapping.data(), wrapping.size(), 0xFFFFFFF0u,
                  kMaxRecordsPerInputBlock, out)
              == ParseStatus::IntegerOverflow);

    std::vector<uint8_t> empty;
    QNP_REQUIRE(encodeInputBlock(nullptr, 0u, empty));
    QNP_CHECK(decodeInputBlock(empty.data(), empty.size(), 0u, 0u, out)
              == ParseStatus::Ok);
    QNP_CHECK(out.empty());
    QNP_CHECK(decodeInputBlock(empty.data(), empty.size(), 1u, 0u, out)
              == ParseStatus::FrameGap);
}

QNP_TEST(ReplayInputBlockLaneAndActionValidation) {
    const size_t recordOffset = kInputBlockHeaderBytes;
    std::vector<ReplayFrameV1> sentinel(1u, sampleFrame(9u));
    std::vector<ReplayFrameV1> out = sentinel;

    const uint8_t badDomains[] = { 0u, 3u, 0xFFu };
    for (size_t i = 0; i < sizeof(badDomains) / sizeof(badDomains[0]); ++i) {
        std::vector<uint8_t> bytes = encodedInputBlock(0u, 1u);
        bytes[recordOffset] = badDomains[i];
        QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 0u, 1u, out)
                  == ParseStatus::BadLaneShape);
    }

    for (unsigned bit = 4u; bit < 8u; ++bit) {
        std::vector<uint8_t> bytes = encodedInputBlock(0u, 1u);
        bytes[recordOffset + 1u] =
            static_cast<uint8_t>(bytes[recordOffset + 1u] | (1u << bit));
        QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 0u, 1u, out)
                  == ParseStatus::BadLaneShape);
    }

    const uint16_t invalidBits[] = {
        0x0010u, 0x0020u, 0x0040u, 0x0080u,
        0x1000u, 0x2000u, 0x4000u, 0x8000u,
    };
    for (size_t i = 0; i < sizeof(invalidBits) / sizeof(invalidBits[0]); ++i) {
        for (size_t lane = 0; lane < kSourceLanes; ++lane) {
            ReplayFrameV1 frame = sampleFrame(0u);
            frame.activeLaneMask = 0x0Fu;
            for (size_t l = 0; l < kSourceLanes; ++l)
                frame.actions[l] = 0x0101u;
            std::vector<uint8_t> bytes;
            QNP_REQUIRE(encodeInputBlock(&frame, 1u, bytes));

            const size_t actionOffset = recordOffset + 2u + lane * 2u;
            writeLe16(
                bytes, actionOffset,
                static_cast<uint16_t>(0x0101u | invalidBits[i]));
            QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 0u, 1u, out)
                      == ParseStatus::BadActionMask);

            frame.activeLaneMask = 0u;
            for (size_t l = 0; l < kSourceLanes; ++l)
                frame.actions[l] = 0u;
            bytes.clear();
            QNP_REQUIRE(encodeInputBlock(&frame, 1u, bytes));
            writeLe16(bytes, actionOffset, invalidBits[i]);
            QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 0u, 1u, out)
                      == ParseStatus::BadLaneShape);

            bytes.clear();
            QNP_REQUIRE(encodeInputBlock(&frame, 1u, bytes));
            writeLe16(bytes, actionOffset, 0x0101u);
            QNP_CHECK(decodeInputBlock(bytes.data(), bytes.size(), 0u, 1u, out)
                      == ParseStatus::BadLaneShape);
        }
    }
    QNP_CHECK(out.size() == 1u && out[0] == sentinel[0]);

    ReplayFrameV1 frame = sampleFrame(0u);
    QNP_CHECK(isValidFrame(frame));
    frame.actions[2] = 0x0101u;
    QNP_CHECK(!isValidFrame(frame));
    std::vector<uint8_t> rejected;
    QNP_CHECK(!encodeInputBlock(&frame, 1u, rejected));
    frame = sampleFrame(0u);
    frame.actions[0] = 0x0110u;
    QNP_CHECK(!isValidFrame(frame));
    QNP_CHECK(!encodeInputBlock(&frame, 1u, rejected));
    frame = sampleFrame(0u);
    frame.activeLaneMask = 0x80u;
    QNP_CHECK(!isValidFrame(frame));
    QNP_CHECK(!encodeInputBlock(&frame, 1u, rejected));
    frame = sampleFrame(0u);
    frame.inputDomain = static_cast<InputDomain>(0);
    QNP_CHECK(!isValidFrame(frame));
    QNP_CHECK(!encodeInputBlock(&frame, 1u, rejected));
    QNP_CHECK(rejected.empty());
}

QNP_TEST(ReplayInputBlockTruncationIsSafe) {
    const std::vector<uint8_t> bytes = encodedInputBlock(500u, 3u);
    checkEveryTruncationFails(bytes, [](const uint8_t* data, size_t length) {
        std::vector<ReplayFrameV1> out;
        return decodeInputBlock(data, length, 500u, 3u, out);
    });
}

QNP_TEST(ReplayEventBlockRoundTripAcrossCounts) {
    const EventKind kinds[] = {
        EventKind::RosterSelected,
        EventKind::StageSelected,
        EventKind::MatchStarted,
        EventKind::RoundStarted,
        EventKind::RoundEnded,
        EventKind::MatchResult,
        EventKind::RematchRequested,
        EventKind::ReturnedToCharacterSelect,
        EventKind::RouteEpochChanged,
        EventKind::SessionPaused,
        EventKind::SessionResumed,
        EventKind::DelayChanged,
        EventKind::NetworkSample,
        EventKind::WallClockPause,
        EventKind::SegmentTerminal,
    };
    const size_t kindCount = sizeof(kinds) / sizeof(kinds[0]);

    std::vector<ReplayEvent> events;
    for (size_t i = 0; i < kindCount; ++i)
        events.push_back(sampleEvent(kinds[i], static_cast<uint32_t>(i * 10u)));

    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeEventBlock(events.data(), events.size(), bytes));
    QNP_REQUIRE(bytes.size() == events.size() * kEventRecordBytes);

    std::vector<ReplayEvent> decoded;
    QNP_REQUIRE(decodeEventBlock(
                    bytes.data(), bytes.size(),
                    static_cast<uint32_t>(events.size()), decoded)
                == ParseStatus::Ok);
    QNP_REQUIRE(decoded.size() == events.size());
    for (size_t i = 0; i < events.size(); ++i) {
        QNP_CHECK(decoded[i].timelineFrame == events[i].timelineFrame);
        QNP_CHECK(decoded[i].kind == events[i].kind);
        QNP_CHECK(decoded[i].matchId == events[i].matchId);
        QNP_CHECK(decoded[i].rosterEpoch == events[i].rosterEpoch);
        QNP_CHECK(decoded[i].selectorProofMask == events[i].selectorProofMask);
        QNP_CHECK(decoded[i].valueA == events[i].valueA);
        QNP_CHECK(decoded[i].valueB == events[i].valueB);
        QNP_CHECK(decoded[i].valueC == events[i].valueC);
        QNP_CHECK(decoded[i].terminalReason == events[i].terminalReason);
        QNP_CHECK(decoded[i].stateProof == events[i].stateProof);
        QNP_CHECK(decoded[i].rollingDigest == events[i].rollingDigest);
        for (size_t slot = 0; slot < kFighterSlots; ++slot) {
            QNP_CHECK(decoded[i].roster[slot].nativeCharacterId
                      == events[i].roster[slot].nativeCharacterId);
            QNP_CHECK(decoded[i].roster[slot].paletteIndex
                      == events[i].roster[slot].paletteIndex);
            QNP_CHECK(decoded[i].selectorProof[slot].cursorCell
                      == events[i].selectorProof[slot].cursorCell);
            QNP_CHECK(decoded[i].selectorProof[slot].alternateFlag
                      == events[i].selectorProof[slot].alternateFlag);
            QNP_CHECK(decoded[i].selectorProof[slot].confirmState
                      == events[i].selectorProof[slot].confirmState);
        }
    }
    std::vector<uint8_t> again;
    QNP_CHECK(encodeEventBlock(decoded.data(), decoded.size(), again));
    QNP_CHECK(again == bytes);

    std::vector<uint8_t> empty;
    QNP_CHECK(encodeEventBlock(nullptr, 0u, empty));
    QNP_CHECK(empty.empty());
    std::vector<ReplayEvent> emptyDecoded(1u, sampleEvent(
        EventKind::MatchStarted, 0u));
    QNP_CHECK(decodeEventBlock(nullptr, 0u, 0u, emptyDecoded)
              == ParseStatus::Ok);
    QNP_CHECK(emptyDecoded.empty());

    std::vector<ReplayEvent> full;
    for (uint32_t i = 0; i < kMaxEventsPerBlock; ++i)
        full.push_back(sampleEvent(EventKind::RoundStarted, i));
    std::vector<uint8_t> fullBytes;
    QNP_REQUIRE(encodeEventBlock(full.data(), full.size(), fullBytes));
    std::vector<ReplayEvent> fullDecoded;
    QNP_CHECK(decodeEventBlock(
                  fullBytes.data(), fullBytes.size(), kMaxEventsPerBlock,
                  fullDecoded)
              == ParseStatus::Ok);
    QNP_CHECK(fullDecoded.size() == kMaxEventsPerBlock);

    full.push_back(sampleEvent(EventKind::RoundStarted, kMaxEventsPerBlock));
    std::vector<uint8_t> rejected;
    QNP_CHECK(!encodeEventBlock(full.data(), full.size(), rejected));
    QNP_CHECK(rejected.empty());
    std::vector<ReplayEvent> out;
    QNP_CHECK(decodeEventBlock(
                  fullBytes.data(), fullBytes.size(), kMaxEventsPerBlock + 1u,
                  out)
              == ParseStatus::LimitExceeded);
    QNP_CHECK(out.empty());
}

QNP_TEST(ReplayEventBlockValidation) {
    const std::vector<uint8_t> valid = [] {
        const ReplayEvent event = sampleEvent(EventKind::MatchStarted, 5u);
        std::vector<uint8_t> bytes;
        QNP_CHECK(encodeEventBlock(&event, 1u, bytes));
        return bytes;
    }();
    QNP_REQUIRE(valid.size() == kEventRecordBytes);

    std::vector<ReplayEvent> sentinel(
        1u, sampleEvent(EventKind::WallClockPause, 1u));
    std::vector<ReplayEvent> out = sentinel;

    const uint16_t badKinds[] = { 0u, 10u, 19u, 22u, 39u, 43u, 59u, 61u,
                                  0xFFFFu };
    for (size_t i = 0; i < sizeof(badKinds) / sizeof(badKinds[0]); ++i) {
        std::vector<uint8_t> bad = valid;
        writeLe16(bad, 4u, badKinds[i]);
        QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
                  == ParseStatus::BadEventPayload);
    }

    std::vector<uint8_t> bad = valid;
    writeLe16(bad, 6u, 7u);
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);
    bad = valid;
    writeLe16(bad, 6u, 0u);
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);
    bad = valid;
    writeLe16(bad, 6u, 2u);
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);

    for (size_t i = 37; i < 40; ++i) {
        bad = valid;
        bad[i] = 1u;
        QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
                  == ParseStatus::UnsupportedContainerVersion);
    }

    bad = valid;
    bad[36] = 0x10u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);
    bad = valid;
    bad[24] = 1u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);

    bad = valid;
    bad[16] = 3u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);

    std::vector<uint8_t> rosterBytes;
    const ReplayEvent roster = sampleEvent(EventKind::RosterSelected, 5u);
    QNP_REQUIRE(encodeEventBlock(&roster, 1u, rosterBytes));
    bad = rosterBytes;
    bad[16] = 34u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);
    bad = rosterBytes;
    bad[17] = 4u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);
    bad = rosterBytes;
    bad[24] = 24u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);
    bad = rosterBytes;
    bad[25] = 2u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);
    bad = rosterBytes;
    bad[26] = 5u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);
    std::vector<ReplayEvent> accepted;
    bad = rosterBytes;
    bad[16] = 0xFFu;
    bad[17] = 0u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, accepted)
              == ParseStatus::Ok);

    std::vector<uint8_t> resultBytes;
    const ReplayEvent result = sampleEvent(EventKind::MatchResult, 5u);
    QNP_REQUIRE(encodeEventBlock(&result, 1u, resultBytes));
    bad = resultBytes;
    bad[40] = 0x10u;
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::BadEventPayload);

    QNP_CHECK(decodeEventBlock(valid.data(), valid.size(), 2u, out)
              == ParseStatus::Truncated);
    std::vector<uint8_t> doubled = valid;
    doubled.insert(doubled.end(), valid.begin(), valid.end());
    QNP_CHECK(decodeEventBlock(doubled.data(), doubled.size(), 1u, out)
              == ParseStatus::LimitExceeded);
    std::vector<uint8_t> padded = valid;
    padded.push_back(0u);
    QNP_CHECK(decodeEventBlock(padded.data(), padded.size(), 1u, out)
              == ParseStatus::LimitExceeded);

    bad = valid;
    writeLe32(bad, 0u, kMaxTimelineFrames + 1u);
    QNP_CHECK(decodeEventBlock(bad.data(), bad.size(), 1u, out)
              == ParseStatus::LimitExceeded);

    QNP_CHECK(out.size() == 1u);
    QNP_CHECK(out[0].kind == EventKind::WallClockPause);

    std::vector<uint8_t> rejected;
    ReplayEvent invalid = sampleEvent(EventKind::MatchStarted, 5u);
    invalid.kind = static_cast<EventKind>(999);
    QNP_CHECK(!encodeEventBlock(&invalid, 1u, rejected));
    invalid = sampleEvent(EventKind::MatchStarted, 5u);
    invalid.terminalReason = TerminalReason::RecorderOverflow;
    QNP_CHECK(!encodeEventBlock(&invalid, 1u, rejected));
    invalid = sampleEvent(EventKind::MatchStarted, 5u);
    invalid.selectorProofMask = 0x01u;
    QNP_CHECK(!encodeEventBlock(&invalid, 1u, rejected));
    invalid = sampleEvent(EventKind::MatchStarted, 5u);
    invalid.roster[0].nativeCharacterId = 2u;
    QNP_CHECK(!encodeEventBlock(&invalid, 1u, rejected));
    invalid = sampleEvent(EventKind::RoundEnded, 5u);
    invalid.valueA = 0x10u;
    QNP_CHECK(!encodeEventBlock(&invalid, 1u, rejected));
    invalid = sampleEvent(EventKind::MatchStarted, 5u);
    invalid.timelineFrame = kMaxTimelineFrames + 1u;
    QNP_CHECK(!encodeEventBlock(&invalid, 1u, rejected));
    QNP_CHECK(rejected.empty());
}

QNP_TEST(ReplayEventBlockTruncationIsSafe) {
    std::vector<ReplayEvent> events;
    events.push_back(sampleEvent(EventKind::RosterSelected, 1u));
    events.push_back(sampleEvent(EventKind::SegmentTerminal, 2u));
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeEventBlock(events.data(), events.size(), bytes));
    checkEveryTruncationFails(bytes, [](const uint8_t* data, size_t length) {
        std::vector<ReplayEvent> out;
        return decodeEventBlock(data, length, 2u, out);
    });
}

QNP_TEST(ReplayEventClassMapping) {
    QNP_CHECK(eventClassOf(EventKind::RosterSelected)
              == EventClass::Assertion);
    QNP_CHECK(eventClassOf(EventKind::StageSelected)
              == EventClass::Assertion);
    QNP_CHECK(eventClassOf(EventKind::MatchStarted) == EventClass::Assertion);
    QNP_CHECK(eventClassOf(EventKind::RoundStarted) == EventClass::Assertion);
    QNP_CHECK(eventClassOf(EventKind::RoundEnded) == EventClass::Assertion);
    QNP_CHECK(eventClassOf(EventKind::MatchResult) == EventClass::Assertion);
    QNP_CHECK(eventClassOf(EventKind::RematchRequested)
              == EventClass::Assertion);
    QNP_CHECK(eventClassOf(EventKind::ReturnedToCharacterSelect)
              == EventClass::Assertion);
    QNP_CHECK(eventClassOf(EventKind::RouteEpochChanged)
              == EventClass::Assertion);
    QNP_CHECK(eventClassOf(EventKind::SessionPaused)
              == EventClass::DeterministicCommand);
    QNP_CHECK(eventClassOf(EventKind::SessionResumed)
              == EventClass::DeterministicCommand);
    QNP_CHECK(eventClassOf(EventKind::DelayChanged)
              == EventClass::PresentationMetadata);
    QNP_CHECK(eventClassOf(EventKind::NetworkSample)
              == EventClass::PresentationMetadata);
    QNP_CHECK(eventClassOf(EventKind::WallClockPause)
              == EventClass::PresentationMetadata);
    QNP_CHECK(eventClassOf(EventKind::SegmentTerminal)
              == EventClass::Terminal);
    QNP_CHECK(eventClassOf(static_cast<EventKind>(9999))
              != EventClass::PresentationMetadata);
}

QNP_TEST(ReplayFooterRoundTripAcrossCounts) {
    const uint32_t counts[] = { 0u, 1u, 2u, kMaxMatchesPerSet };
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        const FooterIndex footer = sampleFooter(counts[i]);
        std::vector<uint8_t> bytes;
        QNP_REQUIRE(encodeFooterIndex(footer, bytes));
        QNP_REQUIRE(bytes.size()
                    == kFooterFixedBytes
                           + static_cast<size_t>(counts[i])
                                 * kMatchIndexEntryBytes);

        FooterIndex decoded{};
        QNP_REQUIRE(decodeFooterIndex(bytes.data(), bytes.size(), decoded)
                    == ParseStatus::Ok);
        QNP_CHECK(decoded.chunkCount == footer.chunkCount);
        QNP_CHECK(decoded.matchCount == footer.matchCount);
        QNP_CHECK(decoded.totalFrames == footer.totalFrames);
        QNP_CHECK(decoded.finalRollingDigest == footer.finalRollingDigest);
        QNP_CHECK(decoded.fileDigest == footer.fileDigest);
        QNP_REQUIRE(decoded.matches.size() == footer.matches.size());
        for (size_t m = 0; m < footer.matches.size(); ++m) {
            const MatchIndexEntry& a = decoded.matches[m];
            const MatchIndexEntry& b = footer.matches[m];
            QNP_CHECK(a.matchId == b.matchId);
            QNP_CHECK(a.ordinal == b.ordinal);
            QNP_CHECK(a.matchStartOffset == b.matchStartOffset);
            QNP_CHECK(a.firstInputOffset == b.firstInputOffset);
            QNP_CHECK(a.firstTimelineFrame == b.firstTimelineFrame);
            QNP_CHECK(a.lastTimelineFrame == b.lastTimelineFrame);
            QNP_CHECK(a.frameCount == b.frameCount);
            QNP_CHECK(a.roundCount == b.roundCount);
            QNP_CHECK(a.winningSideMask == b.winningSideMask);
            QNP_CHECK(a.independentBootstrap == b.independentBootstrap);
            for (size_t slot = 0; slot < kFighterSlots; ++slot) {
                QNP_CHECK(a.roster[slot].nativeCharacterId
                          == b.roster[slot].nativeCharacterId);
                QNP_CHECK(a.roster[slot].paletteIndex
                          == b.roster[slot].paletteIndex);
                QNP_CHECK(a.ownership[slot] == b.ownership[slot]);
            }
        }
        std::vector<uint8_t> again;
        QNP_CHECK(encodeFooterIndex(decoded, again));
        QNP_CHECK(again == bytes);
    }

    FooterIndex tooMany = sampleFooter(2u);
    tooMany.matchCount = kMaxMatchesPerSet + 1u;
    std::vector<uint8_t> rejected;
    QNP_CHECK(!encodeFooterIndex(tooMany, rejected));
    QNP_CHECK(rejected.empty());

    FooterIndex mismatched = sampleFooter(2u);
    mismatched.matchCount = 3u;
    QNP_CHECK(!encodeFooterIndex(mismatched, rejected));
    QNP_CHECK(rejected.empty());
}

QNP_TEST(ReplayFooterIndexValidation) {
    const FooterIndex footer = sampleFooter(2u);
    std::vector<uint8_t> valid;
    QNP_REQUIRE(encodeFooterIndex(footer, valid));

    FooterIndex sentinel{};
    sentinel.chunkCount = 0x5EED5EEDu;
    FooterIndex out = sentinel;

    std::vector<uint8_t> bad = valid;
    writeLe32(bad, 0u, kMaxChunksPerFile + 1u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = valid;
    writeLe32(bad, 8u, kMaxTimelineFrames + 1u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = valid;
    writeLe32(bad, 4u, kMaxMatchesPerSet + 1u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = valid;
    writeLe32(bad, 20u, 1u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);

    bad = valid;
    writeLe32(bad, 4u, 3u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::Truncated);
    bad = valid;
    writeLe32(bad, 4u, 1u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);

    const size_t entry0 = 24u;
    const size_t entry1 = entry0 + kMatchIndexEntryBytes;
    bad = valid;
    writeLe32(bad, entry0, 0u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    writeLe32(bad, entry0 + 4u, kMaxMatchesPerSet);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = valid;
    writeLe32(bad, entry1 + 4u, 0u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    writeLe32(bad, entry1, readLe32(valid, entry0));
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    writeLe32(bad, entry1 + 8u, 0u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    writeLe32(bad, entry0 + 16u, 0u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    writeLe32(bad, entry0 + 24u, 999999u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    writeLe32(bad, entry0 + 28u, kMaxTimelineFrames + 1u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);
    bad = valid;
    writeLe32(bad, entry0 + 32u, 100000u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    bad[entry0 + 40u] = 0x10u;
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    bad[entry0 + 41u] = 2u;
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    writeLe16(bad, entry0 + 42u, 1u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::UnsupportedContainerVersion);
    bad = valid;
    bad[entry0 + 44u] = 34u;
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    bad[entry0 + 45u] = 4u;
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);
    bad = valid;
    bad[entry0 + 52u] = 3u;
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::BadMatchIndex);

    bad = valid;
    bad.push_back(0u);
    QNP_CHECK(decodeFooterIndex(bad.data(), bad.size(), out)
              == ParseStatus::LimitExceeded);

    QNP_CHECK(out.chunkCount == sentinel.chunkCount);
    QNP_CHECK(out.matches.empty());

    std::vector<uint8_t> rejected;
    FooterIndex disordered = sampleFooter(2u);
    disordered.matches[1].ordinal = 0u;
    QNP_CHECK(!encodeFooterIndex(disordered, rejected));
    disordered = sampleFooter(2u);
    disordered.matches[1].matchStartOffset = 0u;
    QNP_CHECK(!encodeFooterIndex(disordered, rejected));
    disordered = sampleFooter(2u);
    disordered.matches[0].firstInputOffset =
        disordered.matches[0].matchStartOffset;
    QNP_CHECK(!encodeFooterIndex(disordered, rejected));
    disordered = sampleFooter(1u);
    disordered.matches[0].ownership[0] = static_cast<SlotOwnership>(3);
    QNP_CHECK(!encodeFooterIndex(disordered, rejected));
    QNP_CHECK(rejected.empty());
}

QNP_TEST(ReplayFooterTruncationIsSafe) {
    std::vector<uint8_t> bytes;
    QNP_REQUIRE(encodeFooterIndex(sampleFooter(2u), bytes));
    checkEveryTruncationFails(bytes, [](const uint8_t* data, size_t length) {
        FooterIndex out{};
        return decodeFooterIndex(data, length, out);
    });
}

QNP_TEST(ReplayStatusTextsAreDistinctAndPresent) {
    std::vector<std::string> seen;
    for (uint16_t i = 0; i <= static_cast<uint16_t>(ParseStatus::IoError);
         ++i) {
        const char* text = parseStatusText(static_cast<ParseStatus>(i));
        QNP_REQUIRE(text != nullptr);
        const std::string value(text);
        QNP_CHECK(!value.empty());
        for (size_t j = 0; j < seen.size(); ++j)
            QNP_CHECK(seen[j] != value);
        seen.push_back(value);
    }
    QNP_CHECK(std::string(parseStatusText(static_cast<ParseStatus>(9999)))
              == "unknown parse status");

    for (uint8_t i = 0; i <= static_cast<uint8_t>(CompatibilityState::Corrupt);
         ++i) {
        const char* text =
            compatibilityStateText(static_cast<CompatibilityState>(i));
        QNP_REQUIRE(text != nullptr);
        QNP_CHECK(std::string(text).size() > 0u);
    }
    QNP_CHECK(std::string(compatibilityStateText(
                  CompatibilityState::SimulationVerified))
              == "Simulation verified");
    QNP_CHECK(std::string(compatibilityStateText(
                  CompatibilityState::FileIntact))
              == "File intact");

    for (uint16_t i = 0;
         i <= static_cast<uint16_t>(CompatibilityReason::ActivelyRecording);
         ++i) {
        const char* text =
            compatibilityReasonText(static_cast<CompatibilityReason>(i));
        QNP_REQUIRE(text != nullptr);
        QNP_CHECK(std::string(text).size() > 0u);
    }
}
