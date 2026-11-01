#include "../SpectatorArchive.h"
#include "../SpectatorCheckpoint.h"
#include "../SpectatorSession.h"
#include "../../netplay/NetplayCore.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {

using namespace qnetplay::spectator;
namespace compat = qnetplay::compat;
using qnetplay::CoreConfig;
using qnetplay::Endpoint;
using qnetplay::NetplayCore;
namespace wire = qnetplay::wire;

int g_failures = 0;

#define CHECK_TRUE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::fprintf(stderr, "%s:%d CHECK failed: %s\n",                \
                         __FILE__, __LINE__, #expr);                          \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

#define CHECK_EQ(left, right)                                                \
    do {                                                                     \
        const auto checkLeft = (left);                                       \
        const auto checkRight = (right);                                     \
        if (!(checkLeft == checkRight)) {                                    \
            std::fprintf(stderr, "%s:%d CHECK_EQ failed: %s != %s\n",       \
                         __FILE__, __LINE__, #left, #right);                  \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

constexpr uint64_t kSimulationContract = 0x514F48393953494Dull;

SessionIdentity spectatorIdentity() {
    return SessionIdentity{
        0x1020304050607080ull,
        7,
        SessionCookie{
            0x1122334455667788ull,
            0x8877665544332211ull,
        },
    };
}

compat::CompatibilityIdentityWire compatibilityIdentity(
    const uint8_t simulationRootSeed = 0xA5u) {
    compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion = compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = wire::kVersion;
    CHECK_TRUE(compat::setProfileId(identity, "qoh99-se1126"));
    for (size_t i = 0; i < compat::kCompatibilitySha256Bytes; ++i) {
        identity.runtimeBuild.executableSha256[i] =
            static_cast<uint8_t>(0x31u + i);
        identity.simulationContent.simulationRootSha256[i] =
            static_cast<uint8_t>(simulationRootSeed + i);
        identity.simulationContent.validationRootSha256[i] =
            static_cast<uint8_t>(0x61u + i);
    }
    identity.runtimeBuild.executableSize = 0x123450u;
    identity.runtimeBuild.peTimestamp = 0x4A514F48u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.entryCount = 24u;
    identity.simulationContent.requiredEntryCount = 21u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.stateSchemaId = 0x535441544531u;
    identity.modSimulationId = 0x4D4F44494431u;
    identity.timingProfileId = 0x54494D494E4731u;
    CHECK_TRUE(compat::isValidCompatibilityIdentity(identity));
    return identity;
}

GameDescriptor descriptor(uint64_t gameId,
                          uint32_t firstFrame = 100) {
    GameDescriptor game{};
    game.gameId = gameId;
    game.simulationContract = kSimulationContract;
    game.compatibilityIdentity = compatibilityIdentity();
    game.initialStateHash = 0xA500000000000000ull ^ gameId;
    game.firstTimelineFrame = firstFrame;
    game.playerOneDelay = 2;
    game.playerTwoDelay = 4;
    game.rollbackWindow = 12;
    game.metadata = {
        0x51, 0x4F, 0x48, 0x39, 0x39,
        static_cast<uint8_t>(gameId),
    };
    return game;
}

ConfirmedRecord recordFor(uint32_t index,
                          uint32_t firstFrame = 100) {
    ConfirmedRecord record{};
    record.record = index;
    record.timelineFrame = firstFrame + index;
    record.playerOneInput = static_cast<uint16_t>(
        (index & 0x0Fu) | ((index & 0xF0u) << 4));
    record.playerTwoInput = static_cast<uint16_t>(
        ((index * 3u + 1u) & 0x0Fu)
        | (((index * 3u + 1u) & 0xF0u) << 4));
    record.stateHash =
        0xD15EA5E000000000ull ^ (static_cast<uint64_t>(index) * 17u);
    return record;
}

uint64_t postStateHashFor(uint32_t index) {
    return 0xA11E0F5000000000ull
        ^ (static_cast<uint64_t>(index) * 0x101u);
}

std::vector<uint8_t> spectatorHello(const SessionIdentity& identity,
                                    uint64_t viewerId = 99,
                                    uint32_t generation = 1,
                                    uint32_t sequence = 1) {
    HelloPayload hello{};
    hello.simulationContract = kSimulationContract;
    hello.compatibilityIdentity = compatibilityIdentity();
    hello.viewerId = viewerId;
    hello.generation = generation;
    hello.viewerNonce = viewerGenerationNonce(viewerId, generation);
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(hello, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Hello, identity, sequence, payload, packet));
    return packet;
}

GameStartPayload startFor(const SpectatorArchive& archive,
                          uint64_t gameId,
                          uint64_t viewerId,
                          uint32_t generation = 1,
                          uint32_t resumeRecord = 0,
                          uint32_t resumeEvent = 0) {
    GameDescriptor canonical{};
    CHECK_TRUE(archive.descriptor(gameId, canonical));
    GameStartPayload start = canonicalGameStart(canonical);
    start.viewerId = viewerId;
    start.generation = generation;
    start.viewerNonce = viewerGenerationNonce(viewerId, generation);
    start.resumeRecord = resumeRecord;
    start.resumeEvent = resumeEvent;
    uint64_t digest = 0;
    CHECK_TRUE(archive.chainDigestAt(
        gameId, resumeRecord, resumeEvent, digest));
    start.resumeDigest = digest;
    return start;
}

TerminalPayload terminalFor(const SpectatorArchive& archive,
                            uint64_t gameId) {
    ArchiveExtent extent{};
    CHECK_TRUE(archive.extent(gameId, extent));
    TerminalPayload terminal{};
    terminal.gameId = gameId;
    terminal.finalDigest = extent.terminal.finalDigest;
    terminal.postTailStateHash = extent.terminal.postTailStateHash;
    terminal.nextRecord = extent.terminal.nextRecord;
    terminal.nextEvent = extent.terminal.nextEvent;
    terminal.reason = extent.terminal.reason;
    return terminal;
}

void writeLe32(std::vector<uint8_t>& bytes, size_t offset,
               uint32_t value) {
    CHECK_TRUE(offset + sizeof(value) <= bytes.size());
    if (offset + sizeof(value) > bytes.size()) return;
    for (unsigned byte = 0; byte < sizeof(value); ++byte) {
        bytes[offset + byte] =
            static_cast<uint8_t>(value >> (byte * 8u));
    }
}

void writeLe64(std::vector<uint8_t>& bytes, size_t offset,
               uint64_t value) {
    CHECK_TRUE(offset + sizeof(value) <= bytes.size());
    if (offset + sizeof(value) > bytes.size()) return;
    for (unsigned byte = 0; byte < sizeof(value); ++byte) {
        bytes[offset + byte] =
            static_cast<uint8_t>(value >> (byte * 8u));
    }
}

uint32_t checkpointCrcForTest(const uint8_t* bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < length; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1)
                ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint64_t checkpointAvalancheForTest(uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

void checkpointMixByteForTest(uint64_t& hash, uint8_t value) {
    hash ^= value;
    hash *= 0x100000001B3ull;
}

void checkpointMix32ForTest(uint64_t& hash, uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte)
        checkpointMixByteForTest(
            hash, static_cast<uint8_t>(value >> (byte * 8u)));
}

void checkpointMix64ForTest(uint64_t& hash, uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte)
        checkpointMixByteForTest(
            hash, static_cast<uint8_t>(value >> (byte * 8u)));
}

uint64_t checkpointDigestV1ForTest(const Checkpoint& checkpoint) {
    uint64_t hash = 0xCBF29CE484222325ull;
    checkpointMix64ForTest(hash, 0x5153502D43484B31ull);
    checkpointMix64ForTest(hash, checkpoint.identity.sessionId);
    checkpointMix64ForTest(hash, checkpoint.identity.gameId);
    checkpointMix64ForTest(hash, checkpoint.identity.checkpointId);
    checkpointMix32ForTest(hash, checkpoint.identity.timelineFrame);
    checkpointMix32ForTest(hash, checkpoint.identity.recordCursor);
    checkpointMix64ForTest(hash, checkpoint.simulationContract);
    checkpointMix64ForTest(hash, checkpoint.bootstrapFingerprint);
    checkpointMix32ForTest(hash, checkpoint.regionSchema);
    checkpointMix32ForTest(hash, checkpoint.regionSchemaVersion);
    checkpointMix64ForTest(hash, checkpoint.digestChainPrefix);
    checkpointMix64ForTest(hash, checkpoint.stateHash);
    checkpointMixByteForTest(
        hash, static_cast<uint8_t>(checkpoint.compression));
    checkpointMix32ForTest(
        hash, static_cast<uint32_t>(checkpoint.regions.size()));
    for (const CheckpointRegion& region : checkpoint.regions) {
        checkpointMix32ForTest(hash, region.id);
        checkpointMix32ForTest(hash, region.byteLength);
        checkpointMix64ForTest(hash, region.hash);
    }
    return checkpointAvalancheForTest(hash);
}

void testReadyMetadataV5RoundTripAndStrictFields() {
    constexpr size_t kVsRowOffset = 64u;
    constexpr size_t kCpuMaskOffset = 68u;
    constexpr size_t kRouteOffset = 72u;
    constexpr size_t kLaneMaskOffset = 76u;
    constexpr size_t kSlotMaskOffset = 77u;
    constexpr size_t kReservedOffset = 79u;
    constexpr size_t kCodePageOffset = 80u;
    constexpr size_t kNameOffset = 96u;
    ReadyMetadata ready{};
    ready.seedGameplay = 0x10203040u;
    ready.seedCosmetic = 0x50607080u;
    ready.selector = 1u;
    ready.initialPhase = kReadyInitialPhase;
    ready.bootstrapHash = 0x1122334455667788ull;
    ready.timerMode = 2u;
    ready.aiDifficulty = 4u;
    ready.lifeMultiplier = 0u;
    ready.roundsToWinMinusOne = 2u;
    ready.vsRow = 3u;
    ready.cpuMask = 0x04u;
    ready.slotToSourceRoute[0] = 0u;
    ready.slotToSourceRoute[1] = 1u;
    ready.slotToSourceRoute[2] = 0u;
    ready.slotToSourceRoute[3] = 1u;
    ready.activeLaneMask = 0x03u;
    ready.activeSlotMask = 0x0Fu;
    ready.rosterEpochLow = 0x5Au;
    ready.playerNameCodePage[0] = 932u;
    ready.playerNameCodePage[1] = 1252u;
    std::snprintf(
        ready.playerName[0], kReadyPlayerNameBytes, "%s", "Alice");
    std::snprintf(
        ready.playerName[1], kReadyPlayerNameBytes, "%s", "Bob");

    std::vector<uint8_t> encoded;
    CHECK_TRUE(encodeReadyMetadata(ready, encoded));
    CHECK_EQ(encoded.size(), kReadyMetadataBytes);
    CHECK_EQ(kReadyMetadataBytes, size_t{224});

    ReadyMetadata decoded{};
    CHECK_TRUE(decodeReadyMetadata(encoded, decoded));
    CHECK_EQ(decoded.seedGameplay, ready.seedGameplay);
    CHECK_EQ(decoded.seedCosmetic, ready.seedCosmetic);
    CHECK_EQ(decoded.selector, ready.selector);
    CHECK_EQ(decoded.initialPhase, ready.initialPhase);
    CHECK_EQ(decoded.bootstrapHash, ready.bootstrapHash);
    CHECK_EQ(decoded.timerMode, ready.timerMode);
    CHECK_EQ(decoded.aiDifficulty, ready.aiDifficulty);
    CHECK_EQ(decoded.lifeMultiplier, ready.lifeMultiplier);
    CHECK_EQ(decoded.roundsToWinMinusOne, ready.roundsToWinMinusOne);
    CHECK_EQ(decoded.vsRow, 3u);
    CHECK_EQ(decoded.cpuMask, 0x04u);
    CHECK_EQ(decoded.activeLaneMask, 0x03u);
    CHECK_EQ(decoded.activeSlotMask, 0x0Fu);
    CHECK_EQ(decoded.rosterEpochLow, 0x5Au);
    CHECK_EQ(decoded.slotToSourceRoute[2], 0u);
    CHECK_EQ(decoded.slotToSourceRoute[3], 1u);
    CHECK_EQ(decoded.playerNameCodePage[0], 932u);
    CHECK_EQ(decoded.playerNameCodePage[1], 1252u);
    CHECK_EQ(decoded.playerNameCodePage[2], 0u);
    CHECK_EQ(decoded.playerNameCodePage[3], 0u);
    CHECK_TRUE(std::strcmp(decoded.playerName[0], "Alice") == 0);
    CHECK_TRUE(std::strcmp(decoded.playerName[1], "Bob") == 0);
    CHECK_EQ(decoded.playerName[2][0], '\0');
    CHECK_EQ(decoded.playerName[3][0], '\0');

    ReadyMetadata team = ready;
    team.activeLaneMask = 0x0Fu;
    team.slotToSourceRoute[2] = 2u;
    team.slotToSourceRoute[3] = 3u;
    team.playerNameCodePage[2] = 932u;
    team.playerNameCodePage[3] = 1252u;
    std::snprintf(team.playerName[2], kReadyPlayerNameBytes, "%s", "Carol");
    std::snprintf(team.playerName[3], kReadyPlayerNameBytes, "%s", "Dave");
    std::vector<uint8_t> teamBytes;
    CHECK_TRUE(encodeReadyMetadata(team, teamBytes));
    ReadyMetadata teamDecoded{};
    CHECK_TRUE(decodeReadyMetadata(teamBytes, teamDecoded));
    CHECK_TRUE(std::strcmp(teamDecoded.playerName[3], "Dave") == 0);
    CHECK_EQ(teamDecoded.activeLaneMask, 0x0Fu);

    ReadyMetadata invalid = ready;
    invalid.selector = 2u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.timerMode = 3u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.aiDifficulty = 5u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.lifeMultiplier = 1u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.roundsToWinMinusOne = 3u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.vsRow = 6u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.cpuMask = 0x10u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.activeLaneMask = 0x01u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.activeSlotMask = 0x10u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.slotToSourceRoute[0] = 4u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.slotToSourceRoute[2] = 2u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.playerNameCodePage[0] = 0u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.playerNameCodePage[2] = 932u;
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    std::snprintf(invalid.playerName[2], kReadyPlayerNameBytes, "%s", "Ghost");
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.playerName[0][0] = '\t';
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.playerName[1][1] = '|';
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    std::fill_n(invalid.playerName[0], kReadyPlayerNameBytes, 'A');
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    invalid.playerName[1][4] = '\0';
    invalid.playerName[1][5] = 'X';
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    std::memset(invalid.playerName[0], 0, kReadyPlayerNameBytes);
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    std::snprintf(
        invalid.playerName[0], kReadyPlayerNameBytes, "%s", " Alice");
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    std::snprintf(
        invalid.playerName[0], kReadyPlayerNameBytes, "%s", "Alice ");
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    std::snprintf(
        invalid.playerName[0], kReadyPlayerNameBytes, "%s", "Alice  Bob");
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));
    invalid = ready;
    std::fill_n(invalid.playerName[0], 21u, 'A');
    invalid.playerName[0][21] = '\0';
    CHECK_TRUE(!encodeReadyMetadata(invalid, encoded));

    ReadyMetadata cp932Trail = ready;
    std::memset(cp932Trail.playerName[0], 0, kReadyPlayerNameBytes);
    cp932Trail.playerName[0][0] = static_cast<char>(0x81u);
    cp932Trail.playerName[0][1] = static_cast<char>(0x7Cu);
    CHECK_TRUE(encodeReadyMetadata(cp932Trail, encoded));
    ReadyMetadata cp932Decoded{};
    CHECK_TRUE(decodeReadyMetadata(encoded, cp932Decoded));
    CHECK_EQ(
        static_cast<uint8_t>(cp932Decoded.playerName[0][0]), 0x81u);
    CHECK_EQ(
        static_cast<uint8_t>(cp932Decoded.playerName[0][1]), 0x7Cu);
    CHECK_EQ(cp932Decoded.playerNameCodePage[0], 932u);

    CHECK_TRUE(encodeReadyMetadata(ready, encoded));
    const struct InvalidField {
        size_t offset;
        uint32_t value;
    } invalidFields[] = {
        {16u, 2u},
        {20u, 0x00030002u},
        {48u, 3u},
        {52u, 5u},
        {56u, 1u},
        {60u, 3u},
        {kVsRowOffset, 6u},
        {kCpuMaskOffset, 0x10u},
        {kCodePageOffset, 0u},
        {kCodePageOffset + 4u, 1200u},
    };
    for (const InvalidField& field : invalidFields) {
        std::vector<uint8_t> malformed = encoded;
        writeLe32(malformed, field.offset, field.value);
        ReadyMetadata untouched{};
        untouched.selector = 0xDEADBEEFu;
        CHECK_TRUE(!decodeReadyMetadata(malformed, untouched));
        CHECK_EQ(untouched.selector, 0xDEADBEEFu);
    }

    const struct InvalidByte {
        size_t offset;
        uint8_t value;
    } invalidBytes[] = {
        {kNameOffset, '\n'},
        {kNameOffset + 2u, '|'},
        {kNameOffset + 32u + 3u, 0x7Fu},
        {kNameOffset + 32u + 4u, 'X'},
        {kNameOffset + 64u, 'X'},
        {kLaneMaskOffset, 0x01u},
        {kSlotMaskOffset, 0x10u},
        {kReservedOffset, 1u},
        {kRouteOffset, 4u},
    };
    for (const InvalidByte& field : invalidBytes) {
        std::vector<uint8_t> malformed = encoded;
        malformed[field.offset] = field.value;
        ReadyMetadata untouched{};
        untouched.selector = 0xDEADBEEFu;
        CHECK_TRUE(!decodeReadyMetadata(malformed, untouched));
        CHECK_EQ(untouched.selector, 0xDEADBEEFu);
    }

    std::vector<uint8_t> unterminated = encoded;
    std::fill_n(unterminated.begin() + kNameOffset, 32u, 'A');
    ReadyMetadata untouched{};
    untouched.selector = 0xDEADBEEFu;
    CHECK_TRUE(!decodeReadyMetadata(unterminated, untouched));
    CHECK_EQ(untouched.selector, 0xDEADBEEFu);

    std::vector<std::vector<uint8_t>> nonCanonicalNames;
    std::vector<uint8_t> emptyName = encoded;
    std::fill_n(emptyName.begin() + kNameOffset, 32u, 0);
    nonCanonicalNames.push_back(emptyName);
    std::vector<uint8_t> leadingSpace = encoded;
    leadingSpace[kNameOffset] = ' ';
    nonCanonicalNames.push_back(leadingSpace);
    std::vector<uint8_t> trailingSpace = encoded;
    trailingSpace[kNameOffset + 4u] = ' ';
    nonCanonicalNames.push_back(trailingSpace);
    std::vector<uint8_t> repeatedSpace = encoded;
    repeatedSpace[kNameOffset + 1u] = ' ';
    repeatedSpace[kNameOffset + 2u] = ' ';
    nonCanonicalNames.push_back(repeatedSpace);
    std::vector<uint8_t> tooLong = encoded;
    std::fill_n(tooLong.begin() + kNameOffset, 21u, 'A');
    tooLong[kNameOffset + 21u] = 0;
    nonCanonicalNames.push_back(tooLong);
    for (const std::vector<uint8_t>& malformed : nonCanonicalNames) {
        ReadyMetadata unchanged{};
        unchanged.selector = 0xDEADBEEFu;
        CHECK_TRUE(!decodeReadyMetadata(malformed, unchanged));
        CHECK_EQ(unchanged.selector, 0xDEADBEEFu);
    }

    std::vector<uint8_t> wrongVersion = encoded;
    writeLe32(wrongVersion, 4u, 4u);
    CHECK_TRUE(!decodeReadyMetadata(wrongVersion, untouched));
}

void testProtocolStrictScoping() {
    const SessionIdentity identity = spectatorIdentity();
    const std::vector<uint8_t> packet = spectatorHello(identity);

    Datagram decoded{};
    CHECK_EQ(decodeDatagram(packet.data(), packet.size(), identity, decoded),
             DecodeError::None);
    CHECK_EQ(decoded.header.direction, Direction::ViewerToSource);
    CHECK_EQ(decoded.header.senderRole, PeerRole::Viewer);

    Datagram untouched{};
    untouched.header.sequence = 0xDEADBEEFu;

    auto changed = packet;
    changed[0] ^= 1;
    CHECK_EQ(decodeDatagram(
                 changed.data(), changed.size(), identity, untouched),
             DecodeError::BadMagic);
    CHECK_EQ(untouched.header.sequence, 0xDEADBEEFu);

    changed = packet;
    changed[4] = 1;
    changed[5] = 0;
    CHECK_EQ(decodeDatagram(
                 changed.data(), changed.size(), identity, untouched),
             DecodeError::BadVersion);

    changed = packet;
    changed[7] = static_cast<uint8_t>(Direction::SourceToViewer);
    CHECK_EQ(decodeDatagram(
                 changed.data(), changed.size(), identity, untouched),
             DecodeError::BadDirectionRole);

    changed = packet;
    changed[8] = static_cast<uint8_t>(PeerRole::Source);
    CHECK_EQ(decodeDatagram(
                 changed.data(), changed.size(), identity, untouched),
             DecodeError::BadDirectionRole);

    SessionIdentity wrong = identity;
    ++wrong.sessionId;
    CHECK_EQ(decodeDatagram(
                 packet.data(), packet.size(), wrong, untouched),
             DecodeError::SessionMismatch);
    wrong = identity;
    ++wrong.epoch;
    CHECK_EQ(decodeDatagram(
                 packet.data(), packet.size(), wrong, untouched),
             DecodeError::EpochMismatch);
    wrong = identity;
    wrong.cookie.low ^= 1;
    CHECK_EQ(decodeDatagram(
                 packet.data(), packet.size(), wrong, untouched),
             DecodeError::CookieMismatch);

    changed = packet;
    changed.back() ^= 0x80;
    CHECK_EQ(decodeDatagram(
                 changed.data(), changed.size(), identity, untouched),
             DecodeError::BadCrc);
}

void testLegacyDirectV3IsExplicitlyUnattested() {
    const SessionIdentity identity = spectatorIdentity();
    HelloPayload hello{};
    hello.simulationContract = kSimulationContract;
    hello.compatibilityIdentity = compatibilityIdentity();
    hello.viewerId = 0x123456789ull;
    hello.generation = 1u;
    hello.viewerNonce = viewerGenerationNonce(
        hello.viewerId, hello.generation);

    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(hello, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Hello, identity, 7u, payload, packet));

    packet.resize(kSpectatorHeaderSize + 56u);
    packet[4] = static_cast<uint8_t>(kLegacySpectatorVersion);
    packet[5] = 0;
    packet[12] = 56u;
    packet[13] = 0;
    writeLe32(packet, 52u, 0u);
    writeLe32(packet, 52u, datagramChecksum(packet.data(), packet.size()));

    Datagram legacy{};
    CHECK_EQ(decodeDatagram(packet.data(), packet.size(), identity, legacy),
             DecodeError::None);
    CHECK_EQ(legacy.header.version, kLegacySpectatorVersion);
    CHECK_TRUE(!legacy.header.compatibilityIdentityAttested);

    SpectatorArchive archive;
    SourceConfig sourceConfig{};
    sourceConfig.identity = identity;
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    SpectatorSource source(sourceConfig, archive);
    CHECK_TRUE(source.isConfigured());
    CHECK_EQ(source.onDatagram(
                 77u, packet.data(), packet.size(), 1u),
             IngressResult::CompatibilityUnattested);
    CHECK_EQ(source.viewerCount(), size_t{0});
}

void testSpectatorContentIdentityRejectsBeforeStreamMutation() {
    const compat::CompatibilityIdentityWire local = compatibilityIdentity();
    const compat::CompatibilityIdentityWire wrongRoot =
        compatibilityIdentity(0xB5u);
    CHECK_TRUE(!compat::semanticallyEqual(local, wrongRoot));

    SpectatorArchive archive;
    const GameDescriptor game = descriptor(0xC1u, 400u);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = local;
    SpectatorSource source(sourceConfig, archive);
    CHECK_TRUE(source.isConfigured());
    SourceConfig missingIdentityConfig = sourceConfig;
    missingIdentityConfig.compatibilityIdentity = {};
    SpectatorSource missingIdentitySource(missingIdentityConfig, archive);
    CHECK_TRUE(!missingIdentitySource.isConfigured());

    HelloPayload mismatchedHello{};
    mismatchedHello.simulationContract = kSimulationContract;
    mismatchedHello.compatibilityIdentity = wrongRoot;
    mismatchedHello.requestedGameId = game.gameId;
    mismatchedHello.viewerId = 0xC0FFEEu;
    mismatchedHello.generation = 1u;
    mismatchedHello.viewerNonce = viewerGenerationNonce(
        mismatchedHello.viewerId, mismatchedHello.generation);
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(mismatchedHello, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Hello, sourceConfig.identity, 1u, payload, packet));
    CHECK_EQ(source.onDatagram(
                 100u, packet.data(), packet.size(), 1u),
             IngressResult::ContractMismatch);
    CHECK_EQ(source.viewerCount(), size_t{0});

    SpectatorArchive descriptorMismatchArchive;
    GameDescriptor descriptorMismatch = descriptor(0xC2u, 410u);
    descriptorMismatch.compatibilityIdentity = wrongRoot;
    CHECK_EQ(descriptorMismatchArchive.beginGame(descriptorMismatch),
             ArchiveResult::Ok);
    SpectatorSource descriptorMismatchSource(
        sourceConfig, descriptorMismatchArchive);
    HelloPayload locallyCompatibleHello = mismatchedHello;
    locallyCompatibleHello.compatibilityIdentity = local;
    locallyCompatibleHello.requestedGameId = descriptorMismatch.gameId;
    CHECK_TRUE(encodePayload(locallyCompatibleHello, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Hello, sourceConfig.identity, 2u, payload, packet));
    CHECK_EQ(descriptorMismatchSource.onDatagram(
                 101u, packet.data(), packet.size(), 1u),
             IngressResult::ContractMismatch);
    CHECK_EQ(descriptorMismatchSource.viewerCount(), size_t{0});

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = local;
    viewerConfig.viewerNonce = 0xD00DCAFEu;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(viewer.isConfigured());
    CHECK_TRUE(viewer.begin(game.gameId));

    GameStartPayload wrongStart = startFor(
        archive, game.gameId, viewerConfig.viewerNonce);
    wrongStart.compatibilityIdentity = wrongRoot;
    wrongStart.initialDigest = 0;
    wrongStart.initialDigest = computeInitialDigest(wrongStart);
    wrongStart.resumeDigest = wrongStart.initialDigest;
    CHECK_TRUE(encodePayload(wrongStart, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::GameStart, viewerConfig.identity, 2u, payload, packet));
    CHECK_EQ(viewer.onDatagram(packet.data(), packet.size(), 2u),
             IngressResult::ContractMismatch);
    CHECK_EQ(viewer.phase(), ViewerPhase::Connecting);
    CHECK_EQ(viewer.receivedNextRecord(), 0u);

    const GameStartPayload goodStart = startFor(
        archive, game.gameId, viewerConfig.viewerNonce);
    CHECK_TRUE(encodePayload(goodStart, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::GameStart, viewerConfig.identity, 3u, payload, packet));
    CHECK_EQ(viewer.onDatagram(packet.data(), packet.size(), 3u),
             IngressResult::Accepted);
    CHECK_EQ(viewer.phase(), ViewerPhase::Streaming);
    CHECK_TRUE(gameStartFingerprint(goodStart)
               != gameStartFingerprint(wrongStart));

    CHECK_TRUE(encodePayload(wrongStart, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::GameStart, viewerConfig.identity, 4u, payload, packet));
    CHECK_EQ(viewer.onDatagram(packet.data(), packet.size(), 4u),
             IngressResult::ContractMismatch);
    GameStartPayload retained{};
    CHECK_TRUE(viewer.gameStart(retained));
    CHECK_TRUE(compat::semanticallyEqual(
        retained.compatibilityIdentity, local));
    CHECK_EQ(viewer.receivedNextRecord(), 0u);

    CheckpointOfferPayload wrongOffer{};
    wrongOffer.gameId = game.gameId;
    wrongOffer.checkpointId = 0xAA55u;
    wrongOffer.compatibilityIdentity = wrongRoot;
    wrongOffer.totalBytes = 64u;
    wrongOffer.chunkCount = 1u;
    wrongOffer.chunkBytes = 64u;
    CHECK_TRUE(encodePayload(wrongOffer, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::CheckpointOffer, viewerConfig.identity,
        5u, payload, packet));
    CHECK_EQ(viewer.onDatagram(packet.data(), packet.size(), 5u),
             IngressResult::ContractMismatch);
    CheckpointOfferPayload retainedOffer{};
    CHECK_TRUE(!viewer.checkpointOffer(retainedOffer));
}

void testAtomicArchiveAndConnectedSetRetention() {
    SpectatorArchive archive;
    GameDescriptor first = descriptor(1);
    CHECK_EQ(archive.beginGame(first), ArchiveResult::Ok);

    ConfirmedRecord stored{};
    ConfirmedRecord frame0 = recordFor(0);
    CHECK_EQ(archive.appendConfirmed(1, frame0, &stored),
             ArchiveResult::Ok);
    CHECK_TRUE(stored.rollingDigest != 0);
    CHECK_EQ(stored.playerOneInput, frame0.playerOneInput);
    CHECK_EQ(stored.playerTwoInput, frame0.playerTwoInput);
    CHECK_EQ(archive.appendConfirmed(1, frame0, nullptr),
             ArchiveResult::ExactDuplicate);

    ConfirmedRecord conflicting = frame0;
    conflicting.playerTwoInput ^= 1;
    CHECK_EQ(archive.appendConfirmed(1, conflicting, nullptr),
             ArchiveResult::ConflictingDuplicate);
    ArchiveExtent extent{};
    CHECK_TRUE(archive.extent(1, extent));
    CHECK_EQ(extent.nextRecord, 1u);

    ConfirmedRecord gap = recordFor(2);
    CHECK_EQ(archive.appendConfirmed(1, gap, nullptr),
             ArchiveResult::Gap);
    ConfirmedRecord frame1 = recordFor(1);
    frame1.timelineFrame += 1;
    CHECK_EQ(archive.appendConfirmed(1, frame1, nullptr),
             ArchiveResult::TimelineMismatch);
    frame1 = recordFor(1);
    CHECK_EQ(archive.appendConfirmed(1, frame1, &stored),
             ArchiveResult::Ok);

    ConfirmedInputBatchPayload batch{};
    CHECK_EQ(archive.readBatch(1, 0, 32, batch),
             ArchiveResult::Ok);
    CHECK_EQ(batch.records.size(), size_t{2});
    CHECK_EQ(batch.records[0].playerOneInput, frame0.playerOneInput);
    CHECK_EQ(batch.records[0].playerTwoInput, frame0.playerTwoInput);
    CHECK_EQ(batch.records[1].record, 1u);
    CHECK_EQ(batch.baseDigest, expectedInitialDigest(first));
    CHECK_EQ(batch.endDigest, batch.records.back().rollingDigest);

    CHECK_EQ(archive.sealGame(
                 1, TerminalReason::MatchEnded, postStateHashFor(1)),
             ArchiveResult::Ok);
    ArchiveExtent sealed{};
    CHECK_TRUE(archive.extent(1, sealed));
    CHECK_EQ(sealed.stateHash, stored.stateHash);
    CHECK_EQ(sealed.terminal.postTailStateHash, postStateHashFor(1));
    CHECK_TRUE(sealed.terminal.postTailStateHash != sealed.stateHash);
    CHECK_EQ(archive.appendConfirmed(1, recordFor(2), nullptr),
             ArchiveResult::Sealed);

    for (uint64_t gameId = 2; gameId <= 12; ++gameId) {
        const GameDescriptor game = descriptor(
            gameId, static_cast<uint32_t>(1000 + gameId * 10));
        CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
        CHECK_EQ(archive.sealGame(
                     gameId, TerminalReason::MatchEnded,
                     game.initialStateHash),
                 ArchiveResult::Ok);
    }
    CHECK_EQ(archive.retainedGameCount(), size_t{12});
    GameDescriptor retained{};
    CHECK_TRUE(archive.descriptor(1, retained));
    CHECK_EQ(retained.gameId, 1u);
}

void testLossReorderLateJoinAndTerminal() {
    SpectatorArchive archive;
    const GameDescriptor game = descriptor(77, 500);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    ConfirmedRecord last{};
    for (uint32_t index = 0; index < 96; ++index) {
        ConfirmedRecord next = recordFor(index, 500);
        CHECK_EQ(archive.appendConfirmed(77, next, &last),
                 ArchiveResult::Ok);
    }
    const uint64_t postTailStateHash = postStateHashFor(95);
    CHECK_EQ(archive.sealGame(
                 77, TerminalReason::MatchEnded, postTailStateHash),
             ArchiveResult::Ok);

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    sourceConfig.recordsPerBatch = 8;
    sourceConfig.perViewerPacketBudget = 4;
    sourceConfig.globalPacketBudget = 32;
    sourceConfig.sendWindowRecords = 96;
    sourceConfig.handshakeResendMs = 50;
    sourceConfig.retransmitMs = 50;
    sourceConfig.terminalResendMs = 50;
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.viewerNonce = 0xABCDEF1234567890ull;
    viewerConfig.helloResendMs = 50;
    viewerConfig.ackResendMs = 50;
    viewerConfig.terminalAckResendMs = 50;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(source.isConfigured());
    CHECK_TRUE(viewer.begin());

    constexpr EndpointId endpoint = 0x010203040BADull;
    const ViewerId viewerId = viewerConfig.viewerNonce;
    uint64_t now = 0;
    uint32_t toSourceCount = 0;
    uint32_t toViewerCount = 0;

    ArchiveTerminal receivedTerminal{};
    for (unsigned step = 0;
         step < 4000
             && (viewer.receivedNextRecord() != 96u
                 || !viewer.terminal(receivedTerminal));
         ++step) {
        now += 25;

        auto viewerPackets = viewer.pollOutbound(now, 8);
        for (const auto& packet : viewerPackets) {
            ++toSourceCount;
            if (toSourceCount % 5 == 1) continue;
            const auto result = source.onDatagram(
                endpoint, packet.bytes.data(), packet.bytes.size(), now);
            CHECK_TRUE(result == IngressResult::Accepted
                       || result == IngressResult::StaleAck
                       || result == IngressResult::UnknownViewer);
            if (toSourceCount % 11 == 0) {
                CHECK_EQ(source.onDatagram(
                             endpoint, packet.bytes.data(),
                             packet.bytes.size(), now),
                         IngressResult::ReplayedSequence);
            }
        }

        auto sourcePackets = source.pollOutbound(now, 32);
        std::reverse(sourcePackets.begin(), sourcePackets.end());
        for (const auto& packet : sourcePackets) {
            ++toViewerCount;
            if (toViewerCount % 7 == 2) continue;
            const auto result = viewer.onDatagram(
                packet.bytes.data(), packet.bytes.size(), now);
            CHECK_TRUE(result == IngressResult::Accepted
                       || result == IngressResult::InitMismatch
                       || result == IngressResult::UnexpectedMessage
                       || result == IngressResult::ReplayedSequence);
            if (toViewerCount % 13 == 0) {
                viewer.onDatagram(
                    packet.bytes.data(), packet.bytes.size(), now);
            }
        }
        CHECK_TRUE(viewer.phase() != ViewerPhase::Failed);
    }

    CHECK_EQ(viewer.phase(), ViewerPhase::Streaming);
    CHECK_EQ(viewer.receivedNextRecord(), 96u);
    CHECK_EQ(viewer.bufferedPlaybackRecords(), size_t{96});
    GameStartPayload start{};
    CHECK_TRUE(viewer.gameStart(start));
    CHECK_EQ(start.gameId, 77u);
    CHECK_EQ(start.resumeRecord, 0u);

    for (uint32_t index = 0; index < 96; ++index) {
        ConfirmedRecord next{};
        CHECK_TRUE(viewer.peekPlaybackRecord(next));
        CHECK_EQ(next.record, index);
        CHECK_EQ(viewer.commitPlaybackRecord(
                     next.record, next.stateHash,
                     postStateHashFor(index)),
                 IngressResult::Accepted);
    }
    CHECK_EQ(viewer.bufferedPlaybackRecords(), size_t{0});
    CHECK_EQ(viewer.phase(), ViewerPhase::Complete);

    ArchiveTerminal terminal{};
    CHECK_TRUE(viewer.terminal(terminal));
    CHECK_EQ(terminal.nextRecord, 96u);
    CHECK_EQ(terminal.postTailStateHash, postTailStateHash);
    CHECK_TRUE(terminal.postTailStateHash != last.stateHash);

    now += 50;
    const auto terminalAcks = viewer.pollOutbound(now, 4);
    CHECK_EQ(terminalAcks.size(), size_t{1});
    CHECK_EQ(terminalAcks[0].type, MessageType::TerminalAck);
    Datagram ackDatagram{};
    CHECK_EQ(decodeDatagram(
                 terminalAcks[0].bytes.data(), terminalAcks[0].bytes.size(),
                 spectatorIdentity(), ackDatagram),
             DecodeError::None);
    TerminalAckPayload badAck{};
    CHECK_TRUE(decodePayload(ackDatagram.payload, badAck));
    badAck.postTailStateHash ^= 1u;
    std::vector<uint8_t> badPayload;
    std::vector<uint8_t> badPacket;
    CHECK_TRUE(encodePayload(badAck, badPayload));
    CHECK_TRUE(encodeDatagram(
        MessageType::TerminalAck, spectatorIdentity(),
        terminalAcks[0].sequence + 1u, badPayload, badPacket));
    CHECK_EQ(source.onDatagram(
                 endpoint, badPacket.data(), badPacket.size(), now),
             IngressResult::TerminalMismatch);
    CHECK_EQ(source.viewerCount(), size_t{1});

    CHECK_EQ(source.onDatagram(
                 endpoint, terminalAcks[0].bytes.data(),
                 terminalAcks[0].bytes.size(), now),
             IngressResult::Accepted);
    SourceViewerSnapshot completed{};
    CHECK_TRUE(source.viewerSnapshot(viewerId, completed));
    CHECK_EQ(completed.phase, SourceViewerPhase::Complete);
    source.pollOutbound(now, 4);
    CHECK_EQ(source.viewerCount(), size_t{0});
    SourceViewerSnapshot retired{};
    CHECK_TRUE(source.viewerSnapshot(viewerId, retired));
    CHECK_TRUE(retired.retired);
    CHECK_EQ(retired.retirementReason,
             SourceRetirementReason::Completed);
}

bool establishEmptyStreamingSession(SpectatorSource& source,
                                    SpectatorViewer& viewer,
                                    EndpointId endpoint,
                                    uint64_t& now) {
    SourceViewerSnapshot snapshot{};
    for (unsigned step = 0u; step < 32u; ++step) {
        ++now;
        for (const ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
            const IngressResult result = source.onDatagram(
                endpoint, packet.bytes.data(), packet.bytes.size(), now);
            CHECK_TRUE(result == IngressResult::Accepted
                       || result == IngressResult::StaleAck
                       || result == IngressResult::UnexpectedMessage);
        }
        for (const SourceDatagram& packet : source.pollOutbound(now, 8u)) {
            if (packet.destination != endpoint) continue;
            const IngressResult result = viewer.onDatagram(
                packet.bytes.data(), packet.bytes.size(), now);
            CHECK_TRUE(result == IngressResult::Accepted
                       || result == IngressResult::ReplayedSequence);
        }
        if (source.viewerSnapshot(viewer.viewerId(), snapshot)
            && snapshot.phase == SourceViewerPhase::Streaming) {
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> cumulativeAckPacket(uint64_t gameId,
                                         ViewerId viewerId,
                                         uint32_t generation,
                                         uint32_t nextRecord,
                                         uint32_t nextEvent,
                                         uint64_t digest,
                                         uint32_t sequence) {
    AckPayload ack{};
    ack.gameId = gameId;
    ack.digest = digest;
    ack.viewerId = viewerId;
    ack.nextRecord = nextRecord;
    ack.generation = generation;
    ack.nextEvent = nextEvent;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(ack, payload));
    CHECK_TRUE(encodeDatagram(MessageType::Ack, spectatorIdentity(),
                              sequence, payload, packet));
    return packet;
}

void testIdleBeforeFirstPublicationDoesNotExpireRepairTimer() {
    constexpr uint64_t gameId = 175u;
    constexpr uint32_t firstFrame = 650u;
    constexpr EndpointId endpoint = 0x010203040C0Bull;
    constexpr uint64_t retransmitMs = 100u;

    SpectatorArchive archive;
    CHECK_EQ(archive.beginGame(descriptor(gameId, firstFrame)),
             ArchiveResult::Ok);

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    sourceConfig.recordsPerBatch = 8u;
    sourceConfig.sendWindowRecords = 96u;
    sourceConfig.perViewerPacketBudget = 1u;
    sourceConfig.globalPacketBudget = 8u;
    sourceConfig.handshakeResendMs = 20u;
    sourceConfig.retransmitMs = retransmitMs;
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.viewerNonce = 0xABCDEF1234567802ull;
    viewerConfig.helloResendMs = 20u;
    viewerConfig.ackResendMs = 20u;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(source.isConfigured());
    CHECK_TRUE(viewer.begin(gameId));

    uint64_t now = 1u;
    CHECK_TRUE(establishEmptyStreamingSession(
        source, viewer, endpoint, now));

    now += retransmitMs * 3u;
    for (const SourceDatagram& packet : source.pollOutbound(now, 8u)) {
        CHECK_TRUE(packet.type != MessageType::ConfirmedInputBatch);
    }
    CHECK_EQ(archive.appendConfirmed(
                 gameId, recordFor(0u, firstFrame), nullptr),
             ArchiveResult::Ok);
    const uint64_t publicationMs = ++now;
    std::vector<SourceDatagram> packets = source.pollOutbound(now, 8u);
    size_t freshBatches = 0u;
    for (const SourceDatagram& packet : packets) {
        if (packet.type != MessageType::ConfirmedInputBatch) continue;
        Datagram datagram{};
        CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                spectatorIdentity(), datagram),
                 DecodeError::None);
        ConfirmedInputBatchPayload batch{};
        CHECK_TRUE(decodePayload(datagram.payload, batch));
        CHECK_EQ(batch.startRecord, 0u);
        ++freshBatches;
    }
    CHECK_EQ(freshBatches, size_t{1});

    SourceViewerSnapshot snapshot{};
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.nextNewRecord, 1u);
    CHECK_EQ(snapshot.acknowledgedNextRecord, 0u);
    CHECK_EQ(snapshot.recordRepairSends, 0u);

    now = publicationMs + retransmitMs - 1u;
    packets = source.pollOutbound(now, 8u);
    for (const SourceDatagram& packet : packets) {
        CHECK_TRUE(packet.type != MessageType::ConfirmedInputBatch);
    }
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.recordRepairSends, 0u);

    now = publicationMs + retransmitMs;
    packets = source.pollOutbound(now, 8u);
    size_t repairs = 0u;
    for (const SourceDatagram& packet : packets) {
        if (packet.type != MessageType::ConfirmedInputBatch) continue;
        Datagram datagram{};
        CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                spectatorIdentity(), datagram),
                 DecodeError::None);
        ConfirmedInputBatchPayload batch{};
        CHECK_TRUE(decodePayload(datagram.payload, batch));
        CHECK_EQ(batch.startRecord, 0u);
        ++repairs;
    }
    CHECK_EQ(repairs, size_t{1});
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.recordRepairSends, 1u);
}

void testDuplicateAcksFastRepairAndLostRepairRto() {
    constexpr uint64_t gameId = 176u;
    constexpr uint32_t firstFrame = 675u;
    constexpr EndpointId endpoint = 0x010203040C0Cull;
    constexpr uint64_t retransmitMs = 100u;

    SpectatorArchive archive;
    CHECK_EQ(archive.beginGame(descriptor(gameId, firstFrame)),
             ArchiveResult::Ok);

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    sourceConfig.recordsPerBatch = 8u;
    sourceConfig.sendWindowRecords = 96u;
    sourceConfig.perViewerPacketBudget = 1u;
    sourceConfig.globalPacketBudget = 8u;
    sourceConfig.handshakeResendMs = 20u;
    sourceConfig.retransmitMs = retransmitMs;
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.viewerNonce = 0xABCDEF1234567803ull;
    viewerConfig.helloResendMs = 20u;
    viewerConfig.ackResendMs = 20u;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(source.isConfigured());
    CHECK_TRUE(viewer.begin(gameId));

    uint64_t now = 1u;
    CHECK_TRUE(establishEmptyStreamingSession(
        source, viewer, endpoint, now));

    uint64_t holeOpenedMs = 0u;
    for (uint32_t index = 0u; index < 3u; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     gameId, recordFor(index, firstFrame), nullptr),
                 ArchiveResult::Ok);
        ++now;
        if (index == 0u) holeOpenedMs = now;
        const std::vector<SourceDatagram> packets =
            source.pollOutbound(now, 8u);
        size_t batches = 0u;
        for (const SourceDatagram& packet : packets) {
            if (packet.type != MessageType::ConfirmedInputBatch) continue;
            Datagram datagram{};
            CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                    spectatorIdentity(), datagram),
                     DecodeError::None);
            ConfirmedInputBatchPayload batch{};
            CHECK_TRUE(decodePayload(datagram.payload, batch));
            CHECK_EQ(batch.startRecord, index);
            ++batches;
        }
        CHECK_EQ(batches, size_t{1});
    }

    uint64_t zeroDigest = 0u;
    CHECK_TRUE(archive.chainDigestAt(gameId, 0u, 0u, zeroDigest));
    uint64_t fastRepairMs = 0u;
    for (uint32_t duplicate = 0u; duplicate < 3u; ++duplicate) {
        ++now;
        const std::vector<uint8_t> ack = cumulativeAckPacket(
            gameId, viewer.viewerId(), viewer.generation(), 0u, 0u,
            zeroDigest, 0x1000u + duplicate);
        CHECK_EQ(source.onDatagram(endpoint, ack.data(), ack.size(), now),
                 IngressResult::Accepted);
        const std::vector<SourceDatagram> packets =
            source.pollOutbound(now, 8u);
        size_t recordBatches = 0u;
        for (const SourceDatagram& packet : packets) {
            if (packet.type != MessageType::ConfirmedInputBatch) continue;
            Datagram datagram{};
            CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                    spectatorIdentity(), datagram),
                     DecodeError::None);
            ConfirmedInputBatchPayload batch{};
            CHECK_TRUE(decodePayload(datagram.payload, batch));
            CHECK_EQ(batch.startRecord, 0u);
            ++recordBatches;
        }
        if (duplicate < 2u) {
            CHECK_EQ(recordBatches, size_t{0});
        } else {
            CHECK_EQ(recordBatches, size_t{1});
            fastRepairMs = now;
        }
    }
    CHECK_TRUE(fastRepairMs != 0u);
    CHECK_TRUE(fastRepairMs - holeOpenedMs < retransmitMs);

    SourceViewerSnapshot snapshot{};
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.recordRepairSends, 1u);
    CHECK_EQ(snapshot.acknowledgedNextRecord, 0u);
    CHECK_EQ(snapshot.nextNewRecord, 3u);

    CHECK_EQ(archive.appendConfirmed(
                 gameId, recordFor(3u, firstFrame), nullptr),
             ArchiveResult::Ok);
    now = fastRepairMs + retransmitMs - 1u;
    std::vector<SourceDatagram> packets = source.pollOutbound(now, 8u);
    size_t beforeDeadline = 0u;
    for (const SourceDatagram& packet : packets) {
        if (packet.type != MessageType::ConfirmedInputBatch) continue;
        Datagram datagram{};
        CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                spectatorIdentity(), datagram),
                 DecodeError::None);
        ConfirmedInputBatchPayload batch{};
        CHECK_TRUE(decodePayload(datagram.payload, batch));
        CHECK_EQ(batch.startRecord, 3u);
        ++beforeDeadline;
    }
    CHECK_EQ(beforeDeadline, size_t{1});
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.recordRepairSends, 1u);

    CHECK_EQ(archive.appendConfirmed(
                 gameId, recordFor(4u, firstFrame), nullptr),
             ArchiveResult::Ok);
    now = fastRepairMs + retransmitMs;
    packets = source.pollOutbound(now, 8u);
    size_t repeatedRepairs = 0u;
    for (const SourceDatagram& packet : packets) {
        if (packet.type != MessageType::ConfirmedInputBatch) continue;
        Datagram datagram{};
        CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                spectatorIdentity(), datagram),
                 DecodeError::None);
        ConfirmedInputBatchPayload batch{};
        CHECK_TRUE(decodePayload(datagram.payload, batch));
        CHECK_EQ(batch.startRecord, 0u);
        ++repeatedRepairs;
    }
    CHECK_EQ(repeatedRepairs, size_t{1});
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.recordRepairSends, 2u);
    CHECK_EQ(snapshot.nextNewRecord, 4u);
}

void testLiveRecordRepairPreemptsFreshPublication() {
    constexpr uint64_t gameId = 177u;
    constexpr uint32_t firstFrame = 700u;
    constexpr EndpointId endpoint = 0x010203040C0Dull;

    SpectatorArchive archive;
    CHECK_EQ(archive.beginGame(descriptor(gameId, firstFrame)),
             ArchiveResult::Ok);

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    sourceConfig.recordsPerBatch = 8u;
    sourceConfig.sendWindowRecords = 96u;
    sourceConfig.perViewerPacketBudget = 1u;
    sourceConfig.globalPacketBudget = 8u;
    sourceConfig.handshakeResendMs = 20u;
    sourceConfig.retransmitMs = 40u;
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.viewerNonce = 0xABCDEF1234567801ull;
    viewerConfig.helloResendMs = 20u;
    viewerConfig.ackResendMs = 20u;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(source.isConfigured());
    CHECK_TRUE(viewer.isConfigured());
    CHECK_TRUE(viewer.begin(gameId));

    uint64_t now = 1u;
    SourceViewerSnapshot snapshot{};
    bool sourceStreaming = false;
    for (unsigned step = 0u; step < 32u && !sourceStreaming; ++step) {
        ++now;
        for (const ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
            const IngressResult result = source.onDatagram(
                endpoint, packet.bytes.data(), packet.bytes.size(), now);
            CHECK_TRUE(result == IngressResult::Accepted
                       || result == IngressResult::StaleAck
                       || result == IngressResult::UnexpectedMessage);
        }
        for (const SourceDatagram& packet : source.pollOutbound(now, 8u)) {
            if (packet.destination != endpoint) continue;
            const IngressResult result = viewer.onDatagram(
                packet.bytes.data(), packet.bytes.size(), now);
            CHECK_TRUE(result == IngressResult::Accepted
                       || result == IngressResult::ReplayedSequence);
        }
        sourceStreaming = source.viewerSnapshot(viewer.viewerId(), snapshot)
            && snapshot.phase == SourceViewerPhase::Streaming;
    }
    CHECK_TRUE(sourceStreaming);
    CHECK_EQ(snapshot.acknowledgedNextRecord, 0u);
    CHECK_EQ(snapshot.nextNewRecord, 0u);

    bool droppedHole = false;
    bool newerRecordSentWhileHoleOpen = false;
    bool repairObserved = false;
    uint64_t dropMs = 0u;
    uint64_t repairMs = 0u;
    uint32_t publishedRecords = 0u;
    uint32_t repairCoveredThrough = 0u;

    for (unsigned step = 0u; step < 32u && !repairObserved; ++step) {
        now += 10u;
        CHECK_EQ(archive.appendConfirmed(
                     gameId, recordFor(publishedRecords, firstFrame), nullptr),
                 ArchiveResult::Ok);
        ++publishedRecords;

        for (const ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
            const IngressResult result = source.onDatagram(
                endpoint, packet.bytes.data(), packet.bytes.size(), now);
            CHECK_TRUE(result == IngressResult::Accepted
                       || result == IngressResult::StaleAck
                       || result == IngressResult::UnexpectedMessage);
        }

        const std::vector<SourceDatagram> packets =
            source.pollOutbound(now, 8u);
        for (const SourceDatagram& packet : packets) {
            if (packet.destination != endpoint) continue;
            Datagram datagram{};
            CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                    spectatorIdentity(), datagram),
                     DecodeError::None);
            if (datagram.header.type != MessageType::ConfirmedInputBatch) {
                CHECK_EQ(viewer.onDatagram(packet.bytes.data(),
                                           packet.bytes.size(), now),
                         IngressResult::Accepted);
                continue;
            }

            ConfirmedInputBatchPayload batch{};
            CHECK_TRUE(decodePayload(datagram.payload, batch));
            CHECK_TRUE(!batch.records.empty());
            if (!droppedHole) {
                CHECK_EQ(batch.startRecord, 0u);
                droppedHole = true;
                dropMs = now;
                continue;
            }

            if (batch.startRecord == 0u) {
                repairObserved = true;
                repairMs = now;
                repairCoveredThrough = batch.records.back().record + 1u;
            } else {
                newerRecordSentWhileHoleOpen = true;
                CHECK_TRUE(batch.startRecord > 0u);
            }
            CHECK_EQ(viewer.onDatagram(packet.bytes.data(),
                                       packet.bytes.size(), now),
                     IngressResult::Accepted);
        }
    }

    CHECK_TRUE(droppedHole);
    CHECK_TRUE(newerRecordSentWhileHoleOpen);
    CHECK_TRUE(repairObserved);
    CHECK_TRUE(repairMs >= dropMs);
    CHECK_TRUE(repairMs - dropMs
               <= sourceConfig.retransmitMs + 10u);
    CHECK_TRUE(repairCoveredThrough != 0u);
    CHECK_EQ(viewer.receivedNextRecord(), repairCoveredThrough);

    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.recordRepairSends, 1u);
    CHECK_TRUE(snapshot.nextNewRecord < publishedRecords);
    CHECK_TRUE(snapshot.nextNewRecord < sourceConfig.sendWindowRecords);

    ++now;
    for (const ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
        const IngressResult result = source.onDatagram(
            endpoint, packet.bytes.data(), packet.bytes.size(), now);
        CHECK_TRUE(result == IngressResult::Accepted
                   || result == IngressResult::StaleAck);
    }
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.acknowledgedNextRecord, repairCoveredThrough);

    for (unsigned step = 0u; step < 16u; ++step) {
        now += 10u;
        if (publishedRecords < 16u) {
            CHECK_EQ(archive.appendConfirmed(
                         gameId,
                         recordFor(publishedRecords, firstFrame), nullptr),
                     ArchiveResult::Ok);
            ++publishedRecords;
        }
        for (const ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
            (void)source.onDatagram(
                endpoint, packet.bytes.data(), packet.bytes.size(), now);
        }
        for (const SourceDatagram& packet : source.pollOutbound(now, 8u)) {
            if (packet.destination != endpoint) continue;
            CHECK_EQ(viewer.onDatagram(packet.bytes.data(),
                                       packet.bytes.size(), now),
                     IngressResult::Accepted);
        }
    }
    CHECK_EQ(viewer.receivedNextRecord(), publishedRecords);
}

void testTerminalArrivingAfterPlaybackVerifiesRememberedPostState() {
    SpectatorArchive archive;
    const GameDescriptor game = descriptor(78, 700);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    ConfirmedRecord stored{};
    CHECK_EQ(archive.appendConfirmed(
                 78, recordFor(0, 700), &stored),
             ArchiveResult::Ok);
    CHECK_EQ(archive.appendConfirmed(
                 78, recordFor(1, 700), &stored),
             ArchiveResult::Ok);
    const uint64_t postTailStateHash = postStateHashFor(1);
    CHECK_EQ(archive.sealGame(
                 78, TerminalReason::MatchEnded, postTailStateHash),
             ArchiveResult::Ok);

    ViewerConfig config{};
    config.identity = spectatorIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = compatibilityIdentity();
    config.viewerNonce = 0xFEEDFACE12345678ull;
    SpectatorViewer viewer(config);
    CHECK_TRUE(viewer.begin(78));

    const GameStartPayload start =
        startFor(archive, 78, config.viewerNonce);
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(start, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::GameStart, config.identity, 1, payload, packet));
    CHECK_EQ(viewer.onDatagram(
                 packet.data(), packet.size(), 1),
             IngressResult::Accepted);

    ConfirmedInputBatchPayload batch{};
    CHECK_EQ(archive.readBatch(78, 0, 2, batch), ArchiveResult::Ok);
    CHECK_TRUE(encodePayload(batch, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::ConfirmedInputBatch, config.identity, 2,
        payload, packet));
    CHECK_EQ(viewer.onDatagram(
                 packet.data(), packet.size(), 2),
             IngressResult::Accepted);

    for (uint32_t index = 0; index < 2; ++index) {
        ConfirmedRecord next{};
        CHECK_TRUE(viewer.peekPlaybackRecord(next));
        CHECK_EQ(viewer.commitPlaybackRecord(
                     next.record, next.stateHash,
                     postStateHashFor(index)),
                 IngressResult::Accepted);
    }
    CHECK_EQ(viewer.phase(), ViewerPhase::Streaming);

    const TerminalPayload terminal = terminalFor(archive, 78);
    CHECK_TRUE(encodePayload(terminal, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Terminal, config.identity, 3, payload, packet));
    CHECK_EQ(viewer.onDatagram(
                 packet.data(), packet.size(), 3),
             IngressResult::Accepted);
    CHECK_EQ(viewer.phase(), ViewerPhase::Complete);

    SpectatorViewer mismatch(config);
    CHECK_TRUE(mismatch.begin(78));
    CHECK_TRUE(encodePayload(start, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::GameStart, config.identity, 4, payload, packet));
    CHECK_EQ(mismatch.onDatagram(
                 packet.data(), packet.size(), 4),
             IngressResult::Accepted);
    CHECK_TRUE(encodePayload(batch, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::ConfirmedInputBatch, config.identity, 5,
        payload, packet));
    CHECK_EQ(mismatch.onDatagram(
                 packet.data(), packet.size(), 5),
             IngressResult::Accepted);
    for (uint32_t index = 0; index < 2; ++index) {
        ConfirmedRecord next{};
        CHECK_TRUE(mismatch.peekPlaybackRecord(next));
        const uint64_t post = index == 1
            ? postStateHashFor(index) ^ 1u
            : postStateHashFor(index);
        CHECK_EQ(mismatch.commitPlaybackRecord(
                     next.record, next.stateHash, post),
                 IngressResult::Accepted);
    }
    CHECK_TRUE(encodePayload(terminal, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Terminal, config.identity, 6, payload, packet));
    CHECK_EQ(mismatch.onDatagram(
                 packet.data(), packet.size(), 6),
             IngressResult::StateHashMismatch);
    CHECK_EQ(mismatch.phase(), ViewerPhase::Failed);
    CHECK_TRUE(mismatch.pollOutbound(7, 4).empty());
}

void testAcceptedTerminalSurvivesOfflineCatchup() {
    SpectatorArchive archive;
    const GameDescriptor game = descriptor(79, 750);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    ConfirmedRecord stored{};
    for (uint32_t index = 0; index < 4; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     79, recordFor(index, 750), &stored),
                 ArchiveResult::Ok);
    }
    CHECK_EQ(archive.sealGame(
                 79, TerminalReason::MatchEnded, postStateHashFor(3)),
             ArchiveResult::Ok);

    ViewerConfig config{};
    config.identity = spectatorIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = compatibilityIdentity();
    config.viewerNonce = 0x7900790079007900ull;
    config.ackResendMs = 50;
    SpectatorViewer viewer(config);
    CHECK_TRUE(viewer.begin(79));

    const GameStartPayload start =
        startFor(archive, 79, config.viewerNonce);
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(start, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::GameStart, config.identity, 1, payload, packet));
    CHECK_EQ(viewer.onDatagram(
                 packet.data(), packet.size(), 1),
             IngressResult::Accepted);

    auto outbound = viewer.pollOutbound(2, 4);
    CHECK_EQ(outbound.size(), size_t{2});
    CHECK_EQ(outbound[0].type, MessageType::InitAck);
    CHECK_EQ(outbound[1].type, MessageType::Ack);

    ConfirmedInputBatchPayload batch{};
    CHECK_EQ(archive.readBatch(79, 0, 4, batch), ArchiveResult::Ok);
    CHECK_TRUE(encodePayload(batch, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::ConfirmedInputBatch, config.identity, 2,
        payload, packet));
    CHECK_EQ(viewer.onDatagram(
                 packet.data(), packet.size(), 3),
             IngressResult::Accepted);

    ArchiveExtent extent{};
    CHECK_TRUE(archive.extent(79, extent));
    const TerminalPayload terminal = terminalFor(archive, 79);
    CHECK_TRUE(encodePayload(terminal, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Terminal, config.identity, 3, payload, packet));
    CHECK_EQ(viewer.onDatagram(
                 packet.data(), packet.size(), 4),
             IngressResult::Accepted);
    CHECK_EQ(viewer.phase(), ViewerPhase::Streaming);

    ArchiveTerminal retained{};
    CHECK_TRUE(viewer.terminal(retained));
    CHECK_EQ(retained, extent.terminal);

    CHECK_TRUE(viewer.reportTransportFailure(IngressResult::RouteLost));
    CHECK_EQ(viewer.phase(), ViewerPhase::Streaming);
    CHECK_EQ(viewer.failure(), IngressResult::Accepted);

    CHECK_TRUE(viewer.resume());
    CHECK_EQ(viewer.phase(), ViewerPhase::Streaming);
    ArchiveTerminal afterResume{};
    CHECK_TRUE(viewer.terminal(afterResume));
    CHECK_EQ(afterResume, retained);
    CHECK_EQ(viewer.receivedNextRecord(), 4u);

    outbound = viewer.pollOutbound(2 + config.ackResendMs, 4);
    CHECK_EQ(outbound.size(), size_t{1});
    CHECK_EQ(outbound[0].type, MessageType::Ack);

    for (uint32_t index = 0; index < 4; ++index) {
        ConfirmedRecord next{};
        CHECK_TRUE(viewer.peekPlaybackRecord(next));
        CHECK_EQ(viewer.commitPlaybackRecord(
                     next.record, next.stateHash,
                     postStateHashFor(index)),
                 IngressResult::Accepted);
    }
    CHECK_EQ(viewer.phase(), ViewerPhase::Complete);
    outbound = viewer.pollOutbound(
        3 + config.ackResendMs, 4);
    CHECK_EQ(outbound.size(), size_t{1});
    CHECK_EQ(outbound[0].type, MessageType::TerminalAck);
}

void testExplicitRouteLossIsTypedAndTerminal() {
    ViewerConfig config{};
    config.identity = spectatorIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = compatibilityIdentity();
    config.viewerNonce = 0xA11CE55A11CE55ull;
    SpectatorViewer viewer(config);
    CHECK_TRUE(viewer.begin());

    CHECK_TRUE(!viewer.reportTransportFailure(IngressResult::BadPayload));
    CHECK_EQ(viewer.phase(), ViewerPhase::Connecting);
    CHECK_TRUE(viewer.reportTransportFailure(IngressResult::RouteLost));
    CHECK_EQ(viewer.phase(), ViewerPhase::Failed);
    CHECK_EQ(viewer.failure(), IngressResult::RouteLost);
    CHECK_TRUE(viewer.pollOutbound(1, 4).empty());
    CHECK_TRUE(viewer.reportTransportFailure(IngressResult::RouteLost));
    CHECK_EQ(viewer.failure(), IngressResult::RouteLost);
}

void testTerminalCursorIsImmutable() {
    SpectatorArchive archive;
    const GameDescriptor game = descriptor(80, 800);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    ConfirmedRecord stored{};
    CHECK_EQ(archive.appendConfirmed(
                 80, recordFor(0, 800), &stored),
             ArchiveResult::Ok);
    CHECK_EQ(archive.sealGame(
                 80, TerminalReason::MatchEnded, postStateHashFor(0)),
             ArchiveResult::Ok);

    ViewerConfig config{};
    config.identity = spectatorIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = compatibilityIdentity();
    config.viewerNonce = 0x8000800080008000ull;
    const GameStartPayload start =
        startFor(archive, 80, config.viewerNonce);

    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    SpectatorViewer extension(config);
    CHECK_TRUE(extension.begin(80));
    CHECK_TRUE(encodePayload(start, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::GameStart, config.identity, 10, payload, packet));
    CHECK_EQ(extension.onDatagram(
                 packet.data(), packet.size(), 1),
             IngressResult::Accepted);

    ConfirmedInputBatchPayload first{};
    CHECK_EQ(archive.readBatch(80, 0, 1, first), ArchiveResult::Ok);
    CHECK_TRUE(encodePayload(first, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::ConfirmedInputBatch, config.identity, 11,
        payload, packet));
    CHECK_EQ(extension.onDatagram(
                 packet.data(), packet.size(), 2),
             IngressResult::Accepted);

    ArchiveExtent extent{};
    CHECK_TRUE(archive.extent(80, extent));
    const TerminalPayload terminal = terminalFor(archive, 80);
    CHECK_TRUE(encodePayload(terminal, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Terminal, config.identity, 12, payload, packet));
    CHECK_EQ(extension.onDatagram(
                 packet.data(), packet.size(), 3),
             IngressResult::Accepted);

    ConfirmedInputBatchPayload beyond{};
    beyond.gameId = 80;
    beyond.startRecord = 1;
    beyond.baseDigest = extent.terminal.finalDigest;
    ConfirmedRecord extra = recordFor(1, 800);
    extra.rollingDigest = advanceRollingDigest(
        beyond.baseDigest, beyond.gameId, extra);
    beyond.records.push_back(extra);
    beyond.endDigest = extra.rollingDigest;
    CHECK_TRUE(encodePayload(beyond, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::ConfirmedInputBatch, config.identity, 13,
        payload, packet));
    CHECK_EQ(extension.onDatagram(
                 packet.data(), packet.size(), 4),
             IngressResult::TerminalMismatch);
    CHECK_EQ(extension.phase(), ViewerPhase::Failed);

    SpectatorViewer pending(config);
    CHECK_TRUE(pending.begin(80));
    CHECK_TRUE(encodePayload(start, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::GameStart, config.identity, 20, payload, packet));
    CHECK_EQ(pending.onDatagram(
                 packet.data(), packet.size(), 1),
             IngressResult::Accepted);

    ConfirmedInputBatchPayload future{};
    future.gameId = 80;
    future.startRecord = 1;
    future.baseDigest = 0x123456789ABCDEF0ull;
    ConfirmedRecord futureRecord = recordFor(1, 800);
    futureRecord.rollingDigest = advanceRollingDigest(
        future.baseDigest, future.gameId, futureRecord);
    future.records.push_back(futureRecord);
    future.endDigest = futureRecord.rollingDigest;
    CHECK_TRUE(encodePayload(future, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::ConfirmedInputBatch, config.identity, 21,
        payload, packet));
    CHECK_EQ(pending.onDatagram(
                 packet.data(), packet.size(), 2),
             IngressResult::Accepted);
    CHECK_EQ(pending.reorderedRecordCount(), size_t{1});

    TerminalPayload shorter{};
    shorter.gameId = 80;
    shorter.finalDigest = start.initialDigest;
    shorter.postTailStateHash = start.initialStateHash;
    shorter.nextRecord = 0;
    shorter.reason = TerminalReason::MatchEnded;
    CHECK_TRUE(encodePayload(shorter, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Terminal, config.identity, 22, payload, packet));
    CHECK_EQ(pending.onDatagram(
                 packet.data(), packet.size(), 3),
             IngressResult::TerminalMismatch);
    CHECK_EQ(pending.phase(), ViewerPhase::Failed);
}

void testResumeRejectsDelayedOldHandshakeWithoutFailing() {
    SpectatorArchive archive;
    const GameDescriptor game = descriptor(88, 900);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    ConfirmedRecord stored{};
    for (uint32_t index = 0; index < 40; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     88, recordFor(index, 900), &stored),
                 ArchiveResult::Ok);
    }

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    sourceConfig.recordsPerBatch = 8;
    sourceConfig.perViewerPacketBudget = 1;
    sourceConfig.globalPacketBudget = 4;
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.viewerNonce = 0x1234000012340000ull;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(viewer.begin(88));
    constexpr EndpointId viewerId = 1234;
    const ViewerId identity = viewerConfig.viewerNonce;
    uint64_t now = 1;

    auto outbound = viewer.pollOutbound(now, 4);
    CHECK_EQ(outbound.size(), size_t{1});
    CHECK_EQ(source.onDatagram(
                 viewerId, outbound[0].bytes.data(),
                 outbound[0].bytes.size(), now),
             IngressResult::Accepted);
    auto sourcePackets = source.pollOutbound(now, 4);
    CHECK_EQ(sourcePackets.size(), size_t{1});
    const std::vector<uint8_t> oldGameStart = sourcePackets[0].bytes;
    CHECK_EQ(viewer.onDatagram(
                 oldGameStart.data(), oldGameStart.size(), now),
             IngressResult::Accepted);
    outbound = viewer.pollOutbound(++now, 4);
    for (const auto& packet : outbound) {
        source.onDatagram(
            viewerId, packet.bytes.data(), packet.bytes.size(), now);
    }
    sourcePackets = source.pollOutbound(++now, 4);
    CHECK_EQ(sourcePackets.size(), size_t{1});
    CHECK_EQ(viewer.onDatagram(
                 sourcePackets[0].bytes.data(),
                 sourcePackets[0].bytes.size(), now),
             IngressResult::Accepted);
    CHECK_EQ(viewer.receivedNextRecord(), 8u);

    CHECK_TRUE(viewer.resume());
    CHECK_EQ(viewer.onDatagram(
                 oldGameStart.data(), oldGameStart.size(), ++now),
             IngressResult::InitMismatch);
    CHECK_EQ(viewer.phase(), ViewerPhase::Connecting);
    CHECK_EQ(viewer.failure(), IngressResult::Accepted);

    outbound = viewer.pollOutbound(++now, 4);
    CHECK_EQ(outbound.size(), size_t{1});
    CHECK_EQ(source.onDatagram(
                 viewerId, outbound[0].bytes.data(),
                 outbound[0].bytes.size(), now),
             IngressResult::Accepted);
    sourcePackets = source.pollOutbound(++now, 4);
    CHECK_EQ(sourcePackets.size(), size_t{1});
    CHECK_EQ(viewer.onDatagram(
                 sourcePackets[0].bytes.data(),
                 sourcePackets[0].bytes.size(), now),
             IngressResult::Accepted);
    GameStartPayload resumed{};
    CHECK_TRUE(viewer.gameStart(resumed));
    CHECK_EQ(viewer.receivedNextRecord(), 8u);

    outbound = viewer.pollOutbound(++now, 4);
    CHECK_EQ(outbound.size(), size_t{2});
    CHECK_EQ(outbound[0].type, MessageType::InitAck);
    CHECK_EQ(outbound[1].type, MessageType::Ack);
    for (const auto& packet : outbound) {
        CHECK_EQ(source.onDatagram(
                     viewerId, packet.bytes.data(), packet.bytes.size(), now),
                 IngressResult::Accepted);
    }
    SourceViewerSnapshot resumedSource{};
    CHECK_TRUE(source.viewerSnapshot(identity, resumedSource));
    CHECK_EQ(resumedSource.phase, SourceViewerPhase::Streaming);
    CHECK_EQ(resumedSource.acknowledgedNextRecord, 8u);

    for (unsigned step = 0;
         step < 200 && viewer.receivedNextRecord() < 40;
         ++step) {
        now += 50;
        outbound = viewer.pollOutbound(now, 4);
        for (const auto& packet : outbound) {
            source.onDatagram(
                viewerId, packet.bytes.data(), packet.bytes.size(), now);
        }
        sourcePackets = source.pollOutbound(now, 4);
        for (const auto& packet : sourcePackets) {
            viewer.onDatagram(
                packet.bytes.data(), packet.bytes.size(), now);
        }
    }
    CHECK_EQ(viewer.receivedNextRecord(), 40u);
    CHECK_TRUE(viewer.phase() != ViewerPhase::Failed);
}

void testDelayedCurrentGameStartAfterStreamingProgress() {
    SpectatorArchive archive;
    const GameDescriptor game = descriptor(89, 1200);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    ConfirmedRecord stored{};
    for (uint32_t index = 0; index < 16; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     89, recordFor(index, 1200), &stored),
                 ArchiveResult::Ok);
    }

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    sourceConfig.recordsPerBatch = 8;
    sourceConfig.perViewerPacketBudget = 1;
    sourceConfig.globalPacketBudget = 4;
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.viewerNonce = 0xABCD0000ABCD0000ull;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(viewer.begin(89));

    constexpr EndpointId viewerId = 5678;
    const ViewerId identity = viewerConfig.viewerNonce;
    uint64_t now = 1;
    auto viewerPackets = viewer.pollOutbound(now, 4);
    CHECK_EQ(viewerPackets.size(), size_t{1});
    const std::vector<uint8_t> delayedHello = viewerPackets[0].bytes;
    CHECK_EQ(source.onDatagram(
                 viewerId, viewerPackets[0].bytes.data(),
                 viewerPackets[0].bytes.size(), now),
             IngressResult::Accepted);

    auto sourcePackets = source.pollOutbound(now, 4);
    CHECK_EQ(sourcePackets.size(), size_t{1});
    const std::vector<uint8_t> delayedGameStart = sourcePackets[0].bytes;
    CHECK_EQ(viewer.onDatagram(
                 delayedGameStart.data(), delayedGameStart.size(), now),
             IngressResult::Accepted);

    viewerPackets = viewer.pollOutbound(++now, 4);
    for (const auto& packet : viewerPackets) {
        CHECK_EQ(source.onDatagram(
                     viewerId, packet.bytes.data(), packet.bytes.size(), now),
                 IngressResult::Accepted);
    }
    sourcePackets = source.pollOutbound(++now, 4);
    CHECK_EQ(sourcePackets.size(), size_t{1});
    CHECK_EQ(sourcePackets[0].type, MessageType::ConfirmedInputBatch);
    CHECK_EQ(viewer.onDatagram(
                 sourcePackets[0].bytes.data(),
                 sourcePackets[0].bytes.size(), now),
             IngressResult::Accepted);
    CHECK_EQ(viewer.receivedNextRecord(), 8u);

    CHECK_EQ(source.onDatagram(
                 viewerId, delayedHello.data(), delayedHello.size(), ++now),
             IngressResult::ReplayedSequence);
    SourceViewerSnapshot snapshot{};
    CHECK_TRUE(source.viewerSnapshot(identity, snapshot));
    CHECK_EQ(snapshot.highestSentNextRecord, 8u);

    {
        HelloPayload replayBody{};
        Datagram decodedHello{};
        CHECK_EQ(decodeDatagram(
                     delayedHello.data(), delayedHello.size(),
                     spectatorIdentity(), decodedHello),
                 DecodeError::None);
        CHECK_TRUE(decodePayload(decodedHello.payload, replayBody));
        std::vector<uint8_t> replayPayload;
        std::vector<uint8_t> replayPacket;
        CHECK_TRUE(encodePayload(replayBody, replayPayload));
        CHECK_TRUE(encodeDatagram(
            MessageType::Hello, spectatorIdentity(),
            decodedHello.header.sequence + 10u,
            replayPayload, replayPacket));
        CHECK_EQ(source.onDatagram(
                     viewerId, replayPacket.data(), replayPacket.size(),
                     ++now),
                 IngressResult::Accepted);
        CHECK_TRUE(source.viewerSnapshot(identity, snapshot));
        CHECK_EQ(snapshot.highestSentNextRecord, 8u);
    }

    CHECK_EQ(viewer.onDatagram(
                 delayedGameStart.data(), delayedGameStart.size(), ++now),
             IngressResult::ReplayedSequence);

    {
        Datagram decodedStart{};
        CHECK_EQ(decodeDatagram(
                     delayedGameStart.data(), delayedGameStart.size(),
                     spectatorIdentity(), decodedStart),
                 DecodeError::None);
        std::vector<uint8_t> resent;
        CHECK_TRUE(encodeDatagram(
            MessageType::GameStart, spectatorIdentity(),
            decodedStart.header.sequence + 500u,
            decodedStart.payload, resent));
        CHECK_EQ(viewer.onDatagram(resent.data(), resent.size(), ++now),
                 IngressResult::Accepted);
    }
    CHECK_EQ(viewer.phase(), ViewerPhase::Streaming);
    CHECK_EQ(viewer.failure(), IngressResult::Accepted);
    CHECK_EQ(viewer.receivedNextRecord(), 8u);

    viewerPackets = viewer.pollOutbound(++now, 4);
    for (const auto& packet : viewerPackets) {
        const IngressResult result = source.onDatagram(
            viewerId, packet.bytes.data(), packet.bytes.size(), now);
        CHECK_TRUE(result == IngressResult::Accepted
                   || (packet.type == MessageType::InitAck
                       && result == IngressResult::UnexpectedMessage));
    }
}

void testLatestViewerWaitsForArchiveWithoutTerminalReject() {
    SpectatorArchive archive;
    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.viewerNonce = 0xC001D00Du;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(viewer.begin());

    auto helloPackets = viewer.pollOutbound(1, 4);
    CHECK_EQ(helloPackets.size(), size_t{1});
    CHECK_EQ(source.onDatagram(
                 77, helloPackets[0].bytes.data(),
                 helloPackets[0].bytes.size(), 1),
             IngressResult::Accepted);
    CHECK_EQ(source.viewerCount(), size_t{0});
    CHECK_TRUE(source.pollOutbound(1, 4).empty());
    CHECK_EQ(viewer.phase(), ViewerPhase::Connecting);

    GameDescriptor game = descriptor(99);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    helloPackets = viewer.pollOutbound(
        1 + viewerConfig.helloResendMs, 4);
    CHECK_EQ(helloPackets.size(), size_t{1});
    CHECK_EQ(source.onDatagram(
                 77, helloPackets[0].bytes.data(),
                 helloPackets[0].bytes.size(),
                 1 + viewerConfig.helloResendMs),
             IngressResult::Accepted);
    CHECK_EQ(source.viewerCount(), size_t{1});
}

void testSpectatorDatagramsCannotMutatePlayerCore() {
    CoreConfig playerConfig{};
    playerConfig.localRole = wire::PeerRole::Host;
    playerConfig.localPlayer = 0;
    playerConfig.compatibilityId = 0x9988776655443322ull;
    playerConfig.compatibilityIdentity = compatibilityIdentity();
    playerConfig.sessionId = 0x1020304050607080ull;
    playerConfig.cookie = wire::SessionCookie{
        0xCAFEBABE12345678ull,
        0x8877665544332211ull,
    };
    playerConfig.expectedPeer = Endpoint{0x0100007Fu, 45000};
    playerConfig.learnPeerPort = false;
    NetplayCore player(playerConfig);

    const std::vector<uint8_t> packet =
        spectatorHello(spectatorIdentity());
    CHECK_EQ(player.onDatagram(
                 playerConfig.expectedPeer, packet.data(), packet.size(), 10),
             qnetplay::IngressResult::BadMagic);
    CHECK_TRUE(!player.peerLocked() || !player.isEstablished());
    CHECK_TRUE(!player.isEstablished());
    CHECK_EQ(player.peerEndpoint(), playerConfig.expectedPeer);
    CHECK_EQ(player.lastValidReceiveMs(), uint64_t{0});
    CHECK_EQ(player.remoteAckThrough(), wire::kNoFrame);
    CHECK_EQ(player.peerAckThrough(), wire::kNoFrame);
    CHECK_TRUE(!player.hasTerminalError());

    SpectatorArchive archive;
    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    SpectatorSource source(sourceConfig, archive);

    CoreConfig clientConfig = playerConfig;
    clientConfig.localRole = wire::PeerRole::Client;
    clientConfig.localPlayer = 1;
    clientConfig.expectedPeer = Endpoint{0x0200007Fu, 45001};
    NetplayCore client(clientConfig);
    const auto playerPackets = client.pollOutbound(1);
    CHECK_EQ(playerPackets.size(), size_t{1});
    const auto sourceResult = source.onDatagram(
        55, playerPackets[0].bytes.data(),
        playerPackets[0].bytes.size(), 1);
    CHECK_TRUE(sourceResult == IngressResult::BadMagic
               || sourceResult == IngressResult::TooShort);
    CHECK_EQ(source.viewerCount(), size_t{0});

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.viewerNonce = 42;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(viewer.begin());
    const auto viewerResult = viewer.onDatagram(
        playerPackets[0].bytes.data(),
        playerPackets[0].bytes.size(), 1);
    CHECK_TRUE(viewerResult == IngressResult::BadMagic
               || viewerResult == IngressResult::TooShort);
    CHECK_EQ(viewer.phase(), ViewerPhase::Connecting);
    CHECK_EQ(viewer.receivedNextRecord(), 0u);
}

void testSequenceWindowReplayAndWrap() {
    ReplayWindow window{};
    CHECK_TRUE(sequenceWindowAdmit(window, 1000u));
    CHECK_TRUE(!sequenceWindowAdmit(window, 1000u));
    CHECK_TRUE(sequenceWindowAdmit(window, 999u));
    CHECK_TRUE(!sequenceWindowAdmit(window, 999u));
    CHECK_TRUE(sequenceWindowAdmit(window, 1001u));
    CHECK_TRUE(!sequenceWindowAdmit(window, 1001u - 65u));
    CHECK_TRUE(sequenceWindowAdmit(window, 1001u - 63u));
    CHECK_TRUE(sequenceWindowAdmit(window, 5000u));
    CHECK_TRUE(!sequenceWindowAdmit(window, 1001u));
    CHECK_TRUE(sequenceWindowAdmit(window, 4999u));

    ReplayWindow wrapped{};
    CHECK_TRUE(sequenceWindowAdmit(wrapped, 0xFFFFFFFEu));
    CHECK_TRUE(sequenceWindowAdmit(wrapped, 0xFFFFFFFFu));
    CHECK_TRUE(sequenceWindowAdmit(wrapped, 0u));
    CHECK_TRUE(sequenceWindowAdmit(wrapped, 1u));
    CHECK_TRUE(!sequenceWindowAdmit(wrapped, 0xFFFFFFFFu));
    CHECK_TRUE(!sequenceWindowAdmit(wrapped, 0u));
    CHECK_TRUE(sequenceWindowAdmit(wrapped, 0xFFFFFFFDu));
    CHECK_TRUE(!sequenceWindowAdmit(wrapped, 0xFFFFFFFDu));
    CHECK_TRUE(!sequenceWindowAdmit(wrapped, 1u + 0x80000000u));
}

struct StreamingFixture {
    SpectatorArchive archive;
    SourceConfig sourceConfig{};
    ViewerConfig viewerConfig{};
    std::unique_ptr<SpectatorSource> source;
    std::unique_ptr<SpectatorViewer> viewer;
    EndpointId endpoint = 4242;
    ViewerId identity = 0;
    uint64_t gameId = 0;
    uint64_t now = 1;

    void pump(unsigned steps, uint64_t stepMs = 30) {
        for (unsigned step = 0; step < steps; ++step) {
            now += stepMs;
            for (const auto& packet : viewer->pollOutbound(now, 8)) {
                source->onDatagram(
                    endpoint, packet.bytes.data(), packet.bytes.size(),
                    now);
            }
            for (const auto& packet : source->pollOutbound(now, 16)) {
                if (packet.destination != endpoint) continue;
                viewer->onDatagram(
                    packet.bytes.data(), packet.bytes.size(), now);
            }
        }
    }
};

void buildStreaming(StreamingFixture& fixture,
                    uint64_t gameId,
                    uint32_t records,
                    uint32_t firstFrame,
                    uint64_t viewerSecret) {
    fixture.gameId = gameId;
    const GameDescriptor game = descriptor(gameId, firstFrame);
    CHECK_EQ(fixture.archive.beginGame(game), ArchiveResult::Ok);
    for (uint32_t index = 0; index < records; ++index) {
        CHECK_EQ(fixture.archive.appendConfirmed(
                     gameId, recordFor(index, firstFrame), nullptr),
                 ArchiveResult::Ok);
    }
    fixture.sourceConfig.identity = spectatorIdentity();
    fixture.sourceConfig.simulationContract = kSimulationContract;
    fixture.sourceConfig.compatibilityIdentity = compatibilityIdentity();
    fixture.sourceConfig.recordsPerBatch = 8;
    fixture.sourceConfig.perViewerPacketBudget = 4;
    fixture.sourceConfig.globalPacketBudget = 16;
    fixture.sourceConfig.handshakeResendMs = 50;
    fixture.sourceConfig.retransmitMs = 50;
    fixture.sourceConfig.terminalResendMs = 50;
    fixture.source = std::unique_ptr<SpectatorSource>(
        new SpectatorSource(fixture.sourceConfig, fixture.archive));

    fixture.viewerConfig.identity = spectatorIdentity();
    fixture.viewerConfig.simulationContract = kSimulationContract;
    fixture.viewerConfig.compatibilityIdentity = compatibilityIdentity();
    fixture.viewerConfig.secureRandom = viewerSecret;
    fixture.viewerConfig.viewerNonce = viewerSecret;
    fixture.viewerConfig.helloResendMs = 50;
    fixture.viewerConfig.ackResendMs = 50;
    fixture.viewerConfig.terminalAckResendMs = 50;
    fixture.identity = viewerSecret;
    fixture.viewer = std::unique_ptr<SpectatorViewer>(
        new SpectatorViewer(fixture.viewerConfig));
    CHECK_TRUE(fixture.source->isConfigured());
    CHECK_TRUE(fixture.viewer->isConfigured());
    CHECK_TRUE(fixture.viewer->begin(gameId));
    fixture.pump(40);
    CHECK_EQ(fixture.viewer->receivedNextRecord(), records);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);
}

void testPreflightSequenceHandoffKeepsRuntimeHelloAdmissible() {
    SpectatorArchive archive;
    constexpr uint64_t gameId = 131u;
    const GameDescriptor game = descriptor(gameId, 3100u);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    for (uint32_t index = 0; index < 24u; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     gameId, recordFor(index, 3100u), nullptr),
                 ArchiveResult::Ok);
    }
    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    sourceConfig.handshakeResendMs = 50;
    SpectatorSource source(sourceConfig, archive);
    CHECK_TRUE(source.isConfigured());

    constexpr EndpointId endpoint = 7171u;
    constexpr uint64_t viewerId = 0x5E0C0DE000000131ull;
    uint64_t now = 1000;

    constexpr uint32_t spent = 6u;
    for (uint32_t sequence = 1; sequence <= spent; ++sequence) {
        now += 10;
        const std::vector<uint8_t> hello =
            spectatorHello(spectatorIdentity(), viewerId, 1u, sequence);
        CHECK_EQ(source.onDatagram(endpoint, hello.data(), hello.size(), now),
                 IngressResult::Accepted);
    }
    (void)source.pollOutbound(now, 16);

    auto runtimeConfig = [&](uint32_t initialSequence) {
        ViewerConfig config{};
        config.identity = spectatorIdentity();
        config.simulationContract = kSimulationContract;
        config.compatibilityIdentity = compatibilityIdentity();
        config.secureRandom = viewerId;
        config.viewerNonce = viewerId;
        config.helloResendMs = 50;
        config.initialSequence = initialSequence;
        return config;
    };

    {
        SpectatorViewer runtime(runtimeConfig(0u));
        CHECK_TRUE(runtime.begin(gameId));
        now += 10;
        auto packets = runtime.pollOutbound(now, 8);
        CHECK_EQ(packets.size(), size_t{1});
        CHECK_EQ(source.onDatagram(endpoint, packets[0].bytes.data(),
                                   packets[0].bytes.size(), now),
                 IngressResult::ReplayedSequence);
    }

    {
        SpectatorViewer runtime(runtimeConfig(spent + 1u));
        CHECK_TRUE(runtime.begin(gameId));
        now += 10;
        auto packets = runtime.pollOutbound(now, 8);
        CHECK_EQ(packets.size(), size_t{1});
        CHECK_EQ(source.onDatagram(endpoint, packets[0].bytes.data(),
                                   packets[0].bytes.size(), now),
                 IngressResult::Accepted);
        for (unsigned step = 0;
             step < 40 && runtime.phase() != ViewerPhase::Streaming;
             ++step) {
            now += 30;
            for (const auto& packet : source.pollOutbound(now, 16)) {
                if (packet.destination != endpoint) continue;
                runtime.onDatagram(packet.bytes.data(), packet.bytes.size(),
                                   now);
            }
            for (const auto& packet : runtime.pollOutbound(now, 8)) {
                const IngressResult result = source.onDatagram(
                    endpoint, packet.bytes.data(), packet.bytes.size(), now);
                CHECK_TRUE(result == IngressResult::Accepted
                           || result == IngressResult::StaleAck
                           || result == IngressResult::UnexpectedMessage);
            }
        }
        CHECK_EQ(runtime.phase(), ViewerPhase::Streaming);
        SourceViewerSnapshot snapshot{};
        CHECK_TRUE(source.viewerSnapshot(viewerId, snapshot));
        CHECK_EQ(snapshot.phase, SourceViewerPhase::Streaming);
    }
}

void testValidatedParentRouteRehandshakeStaysSameGeneration() {
    StreamingFixture fixture;
    buildStreaming(fixture, 119u, 20u, 2900u,
                   0x51A7E00000000019ull);

    const uint32_t cursorRecord = fixture.viewer->receivedNextRecord();
    const uint32_t cursorEvent = fixture.viewer->receivedNextEvent();
    const uint64_t cursorDigest = fixture.viewer->receivedDigest();
    const uint32_t originalGeneration = fixture.viewer->generation();
    constexpr uint64_t firstActivation = 0xA11CE70000000019ull;
    constexpr uint64_t activation = 0xA11CE7000000001Aull;
    constexpr EndpointId candidateEndpoint = 5151u;

    CHECK_TRUE(!fixture.viewer->armValidatedParentRouteTransition(
        firstActivation, cursorRecord + 1u, cursorEvent, cursorDigest));
    CHECK_TRUE(fixture.viewer->armValidatedParentRouteTransition(
        firstActivation, cursorRecord, cursorEvent, cursorDigest));
    CHECK_TRUE(!fixture.viewer->resume());
    CHECK_TRUE(!fixture.viewer->migrate(0u, 0u, 0u));

    auto firstHello = fixture.viewer->pollOutbound(++fixture.now, 8u);
    const auto firstCandidate = std::find_if(
        firstHello.begin(), firstHello.end(),
        [](const ViewerDatagram& packet) {
            return packet.route
                    == ViewerDatagramRoute::ValidatedPendingParent;
        });
    CHECK_TRUE(firstCandidate != firstHello.end());
    if (firstCandidate != firstHello.end()) {
        CHECK_EQ(firstCandidate->type, MessageType::Hello);
        CHECK_EQ(firstCandidate->parentRouteActivationId, firstActivation);
        Datagram decoded{};
        CHECK_EQ(decodeDatagram(firstCandidate->bytes.data(),
                                firstCandidate->bytes.size(),
                                fixture.viewerConfig.identity, decoded),
                 DecodeError::None);
        HelloPayload hello{};
        CHECK_TRUE(decodePayload(decoded.payload, hello));
        CHECK_EQ(hello.generation, originalGeneration);
        CHECK_EQ(hello.resumeRecord, cursorRecord);
        CHECK_EQ(hello.resumeEvent, cursorEvent);
        CHECK_EQ(hello.resumeDigest, cursorDigest);
    }

    CHECK_TRUE(!fixture.viewer->cancelValidatedParentRouteTransition(
        firstActivation + 1u));
    CHECK_TRUE(fixture.viewer->cancelValidatedParentRouteTransition(
        firstActivation));
    auto cancelledPackets = fixture.viewer->pollOutbound(
        fixture.now += fixture.viewerConfig.helloResendMs, 8u);
    for (const ViewerDatagram& packet : cancelledPackets) {
        CHECK_TRUE(packet.route == ViewerDatagramRoute::ActiveParent);
    }
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);
    CHECK_EQ(fixture.viewer->generation(), originalGeneration);

    CHECK_TRUE(fixture.viewer->armValidatedParentRouteTransition(
        activation, cursorRecord, cursorEvent, cursorDigest));
    auto packets = fixture.viewer->pollOutbound(++fixture.now, 8u);
    const auto candidateHello = std::find_if(
        packets.begin(), packets.end(),
        [activation](const ViewerDatagram& packet) {
            return packet.route
                    == ViewerDatagramRoute::ValidatedPendingParent
                && packet.parentRouteActivationId == activation;
        });
    CHECK_TRUE(candidateHello != packets.end());
    for (const ViewerDatagram& packet : packets) {
        if (&packet != (candidateHello == packets.end()
                            ? nullptr : &*candidateHello)) {
            CHECK_TRUE(packet.route == ViewerDatagramRoute::ActiveParent);
            CHECK_EQ(packet.parentRouteActivationId, uint64_t{0});
        }
    }
    if (candidateHello == packets.end()) return;

    CHECK_EQ(fixture.viewer->onValidatedParentRouteDatagram(
                 activation, candidateHello->bytes.data(),
                 candidateHello->bytes.size(), ++fixture.now),
             IngressResult::WrongDirection);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);
    CHECK_EQ(fixture.viewer->generation(), originalGeneration);

    SpectatorSource candidate(fixture.sourceConfig, fixture.archive);
    CHECK_TRUE(candidate.isConfigured());
    CHECK_EQ(candidate.onDatagram(candidateEndpoint,
                                  candidateHello->bytes.data(),
                                  candidateHello->bytes.size(),
                                  ++fixture.now),
             IngressResult::Accepted);
    std::vector<uint8_t> candidateStart;
    for (const SourceDatagram& packet : candidate.pollOutbound(
             ++fixture.now, 8u)) {
        if (packet.destination == candidateEndpoint
            && packet.type == MessageType::GameStart) {
            candidateStart = packet.bytes;
            break;
        }
    }
    CHECK_TRUE(!candidateStart.empty());
    if (candidateStart.empty()) return;

    CHECK_EQ(fixture.viewer->onValidatedParentRouteDatagram(
                 activation + 1u, candidateStart.data(),
                 candidateStart.size(), ++fixture.now),
             IngressResult::UnexpectedMessage);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);
    CHECK_EQ(fixture.viewer->generation(), originalGeneration);

    CHECK_EQ(fixture.viewer->onValidatedParentRouteDatagram(
                 activation, candidateStart.data(), candidateStart.size(),
                 ++fixture.now),
             IngressResult::Accepted);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);
    auto preCommit = fixture.viewer->pollOutbound(++fixture.now, 8u);
    for (const ViewerDatagram& packet : preCommit) {
        CHECK_TRUE(packet.route == ViewerDatagramRoute::ActiveParent);
    }

    CHECK_TRUE(fixture.viewer->commitValidatedParentRouteIngress(
        activation, ++fixture.now));
    CHECK_EQ(fixture.viewer->generation(), originalGeneration);
    CHECK_EQ(fixture.viewer->receivedNextRecord(), cursorRecord);
    CHECK_EQ(fixture.viewer->receivedNextEvent(), cursorEvent);
    CHECK_EQ(fixture.viewer->receivedDigest(), cursorDigest);
    CHECK_TRUE(!fixture.viewer->commitValidatedParentRouteIngress(
        activation, fixture.now));

    bool deliveredInitAck = false;
    for (const ViewerDatagram& packet : fixture.viewer->pollOutbound(
             ++fixture.now, 8u)) {
        CHECK_EQ(packet.route, ViewerDatagramRoute::ActiveParent);
        if (packet.type == MessageType::InitAck) {
            CHECK_EQ(candidate.onDatagram(candidateEndpoint,
                                          packet.bytes.data(),
                                          packet.bytes.size(),
                                          fixture.now),
                     IngressResult::Accepted);
            deliveredInitAck = true;
        }
    }
    CHECK_TRUE(deliveredInitAck);

    bool candidateProgressAccepted = false;
    for (const SourceDatagram& packet : candidate.pollOutbound(
             fixture.now += fixture.sourceConfig.heartbeatIntervalMs, 8u)) {
        if (packet.destination != candidateEndpoint) continue;
        const IngressResult result = fixture.viewer->onDatagram(
            packet.bytes.data(), packet.bytes.size(), fixture.now);
        CHECK_EQ(result, IngressResult::Accepted);
        candidateProgressAccepted = true;
    }
    CHECK_TRUE(candidateProgressAccepted);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);
}

void testDelayedOldHelloCannotRewindNewerGeneration() {
    StreamingFixture fixture;
    buildStreaming(fixture, 120, 32, 3000, 0xA1B2C3D4E5F60718ull);

    HelloPayload oldHello{};
    oldHello.simulationContract = kSimulationContract;
    oldHello.compatibilityIdentity = compatibilityIdentity();
    oldHello.requestedGameId = fixture.gameId;
    oldHello.viewerId = fixture.identity;
    oldHello.generation = 1u;
    oldHello.viewerNonce = viewerGenerationNonce(fixture.identity, 1u);
    oldHello.resumeRecord = 0;
    oldHello.resumeEvent = 0;
    oldHello.resumeDigest = 0;

    CHECK_TRUE(fixture.viewer->resume());
    CHECK_EQ(fixture.viewer->generation(), 2u);
    fixture.pump(10);
    SourceViewerSnapshot healthy{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, healthy));
    CHECK_EQ(healthy.generation, 2u);
    CHECK_EQ(healthy.phase, SourceViewerPhase::Streaming);
    const uint32_t healthyAck = healthy.acknowledgedNextRecord;
    const uint64_t healthyActivity = healthy.lastValidReceiveMs;
    CHECK_EQ(healthyAck, 32u);

    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(oldHello, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Hello, spectatorIdentity(), 0x40000u,
        payload, packet));
    fixture.now += 500;
    CHECK_EQ(fixture.source->onDatagram(
                 fixture.endpoint, packet.data(), packet.size(),
                 fixture.now),
             IngressResult::StaleGeneration);

    SourceViewerSnapshot after{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, after));
    CHECK_EQ(after.generation, 2u);
    CHECK_EQ(after.phase, SourceViewerPhase::Streaming);
    CHECK_EQ(after.acknowledgedNextRecord, healthyAck);
    CHECK_EQ(after.lastValidReceiveMs, healthyActivity);
    CHECK_EQ(fixture.source->pendingRejectCount(), size_t{0});
}

void testAuthenticatedMigrationAndSpoofRejection() {
    StreamingFixture fixture;
    buildStreaming(fixture, 121, 24, 4000, 0x1122334455667788ull);
    constexpr EndpointId newEndpoint = 5150;
    constexpr EndpointId attacker = 6060;

    HelloPayload sneaky{};
    sneaky.simulationContract = kSimulationContract;
    sneaky.compatibilityIdentity = compatibilityIdentity();
    sneaky.requestedGameId = fixture.gameId;
    sneaky.viewerId = fixture.identity;
    sneaky.generation = 5u;
    sneaky.viewerNonce = viewerGenerationNonce(fixture.identity, 5u);
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(sneaky, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Hello, spectatorIdentity(), 0x50000u, payload, packet));
    CHECK_EQ(fixture.source->onDatagram(
                 attacker, packet.data(), packet.size(), ++fixture.now),
             IngressResult::MigrationRejected);
    SourceViewerSnapshot pinned{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, pinned));
    CHECK_EQ(pinned.endpoint, fixture.endpoint);
    CHECK_EQ(pinned.generation, 1u);

    CHECK_TRUE(fixture.viewer->migrate(0, 0, 0));
    CHECK_EQ(fixture.viewer->generation(), 2u);
    auto migratePackets = fixture.viewer->pollOutbound(++fixture.now, 4);
    CHECK_EQ(migratePackets.size(), size_t{1});
    CHECK_EQ(migratePackets[0].type, MessageType::Migrate);

    Datagram decodedMigrate{};
    CHECK_EQ(decodeDatagram(
                 migratePackets[0].bytes.data(),
                 migratePackets[0].bytes.size(),
                 spectatorIdentity(), decodedMigrate),
             DecodeError::None);
    MigratePayload spoof{};
    CHECK_TRUE(decodePayload(decodedMigrate.payload, spoof));
    spoof.resumeDigest ^= 1u;
    CHECK_TRUE(encodePayload(spoof, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Migrate, spectatorIdentity(), 0x60000u,
        payload, packet));
    CHECK_EQ(fixture.source->onDatagram(
                 attacker, packet.data(), packet.size(), ++fixture.now),
             IngressResult::MigrationRejected);
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, pinned));
    CHECK_EQ(pinned.endpoint, fixture.endpoint);
    CHECK_EQ(pinned.generation, 1u);

    CHECK_EQ(fixture.source->onDatagram(
                 newEndpoint, migratePackets[0].bytes.data(),
                 migratePackets[0].bytes.size(), ++fixture.now),
             IngressResult::Accepted);
    SourceViewerSnapshot migrated{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, migrated));
    CHECK_EQ(migrated.endpoint, newEndpoint);
    CHECK_EQ(migrated.generation, 2u);
    CHECK_EQ(migrated.migrations, size_t{1});
    CHECK_EQ(migrated.phase, SourceViewerPhase::AwaitingInitAck);
    CHECK_EQ(fixture.source->viewerCount(), size_t{1});

    const EndpointId oldEndpoint = fixture.endpoint;
    fixture.endpoint = newEndpoint;
    fixture.pump(20);
    CHECK_EQ(fixture.viewer->receivedNextRecord(), 24u);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);

    fixture.now += 100;
    auto stale = fixture.viewer->pollOutbound(fixture.now, 4);
    CHECK_TRUE(!stale.empty());
    CHECK_EQ(fixture.source->onDatagram(
                 oldEndpoint, stale[0].bytes.data(),
                 stale[0].bytes.size(), fixture.now),
             IngressResult::EndpointMismatch);
    CHECK_EQ(fixture.source->viewerCount(), size_t{1});

    CHECK_TRUE(fixture.viewer->migrate(0, 0, 0));
    auto second = fixture.viewer->pollOutbound(++fixture.now, 4);
    CHECK_EQ(second.size(), size_t{1});
    CHECK_EQ(fixture.source->onDatagram(
                 7070, second[0].bytes.data(), second[0].bytes.size(),
                 fixture.now),
             IngressResult::RateLimited);
}

void testExplicitDetachReleasesCapacityImmediately() {
    StreamingFixture fixture;
    buildStreaming(fixture, 122, 16, 5000, 0x2233445566778899ull);
    CHECK_EQ(fixture.source->viewerCount(), size_t{1});

    CHECK_TRUE(fixture.viewer->detach());
    auto packets = fixture.viewer->pollOutbound(++fixture.now, 4);
    CHECK_EQ(packets.size(), size_t{1});
    CHECK_EQ(packets[0].type, MessageType::Detach);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Detached);
    CHECK_EQ(fixture.source->onDatagram(
                 fixture.endpoint, packets[0].bytes.data(),
                 packets[0].bytes.size(), fixture.now),
             IngressResult::Detached);
    CHECK_EQ(fixture.source->viewerCount(), size_t{0});
    SourceViewerSnapshot retired{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, retired));
    CHECK_EQ(retired.retirementReason, SourceRetirementReason::Detached);

    StreamingFixture other;
    buildStreaming(other, 123, 16, 5100, 0x33445566778899AAull);
    DetachPayload forged{};
    forged.viewerId = other.identity;
    forged.generation = 1u;
    forged.nextRecord = 4u;
    forged.digest = 0xDEADBEEFCAFEF00Dull;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(forged, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Detach, spectatorIdentity(), 0x70000u,
        payload, packet));
    CHECK_EQ(other.source->onDatagram(
                 other.endpoint, packet.data(), packet.size(),
                 ++other.now),
             IngressResult::ResumeMismatch);
    CHECK_EQ(other.source->viewerCount(), size_t{1});
}

void testPreGameViewerCanEmitCapabilityScopedDetach() {
    ViewerConfig config{};
    config.identity = spectatorIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = compatibilityIdentity();
    config.viewerNonce = 0xD37AC4001ull;
    SpectatorViewer viewer(config);
    CHECK_TRUE(viewer.begin());
    CHECK_TRUE(viewer.detach());

    const auto packets = viewer.pollOutbound(1, 1);
    CHECK_EQ(packets.size(), size_t{1});
    CHECK_EQ(packets[0].type, MessageType::Detach);
    CHECK_EQ(viewer.phase(), ViewerPhase::Detached);

    Datagram datagram{};
    CHECK_EQ(decodeDatagram(
                 packets[0].bytes.data(), packets[0].bytes.size(),
                 config.identity, datagram),
             DecodeError::None);
    DetachPayload detach{};
    CHECK_TRUE(decodePayload(datagram.payload, detach));
    CHECK_EQ(detach.viewerId, viewer.viewerId());
    CHECK_EQ(detach.generation, viewer.generation());
    CHECK_EQ(detach.nextRecord, 0u);
    CHECK_EQ(detach.nextEvent, 0u);
    CHECK_EQ(detach.digest, 0u);
}

void testSeedlessInitialDetachRetiresUnacknowledgedSourceSlot() {
    SpectatorArchive archive;
    const uint64_t gameId = 124u;
    CHECK_EQ(archive.beginGame(descriptor(gameId, 6000)), ArchiveResult::Ok);

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    SpectatorSource source(sourceConfig, archive);
    CHECK_TRUE(source.isConfigured());

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.secureRandom = 0x9988776655443322ull;
    viewerConfig.viewerNonce = viewerConfig.secureRandom;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(viewer.isConfigured());
    CHECK_TRUE(viewer.begin(gameId));

    const EndpointId endpoint = 6060u;
    const auto hello = viewer.pollOutbound(1u, 1u);
    CHECK_EQ(hello.size(), size_t{1});
    CHECK_EQ(hello[0].type, MessageType::Hello);
    CHECK_EQ(source.onDatagram(endpoint, hello[0].bytes.data(),
                                hello[0].bytes.size(), 1u),
             IngressResult::Accepted);
    CHECK_EQ(source.viewerCount(), size_t{1});

    const auto lostGameStart = source.pollOutbound(1u, 4u);
    CHECK_EQ(lostGameStart.size(), size_t{1});
    CHECK_EQ(lostGameStart[0].type, MessageType::GameStart);

    CHECK_TRUE(viewer.detach());
    const auto detach = viewer.pollOutbound(2u, 1u);
    CHECK_EQ(detach.size(), size_t{1});
    CHECK_EQ(detach[0].type, MessageType::Detach);
    CHECK_EQ(source.onDatagram(endpoint, detach[0].bytes.data(),
                                detach[0].bytes.size(), 2u),
             IngressResult::Detached);
    CHECK_EQ(source.viewerCount(), size_t{0});
    SourceViewerSnapshot retired{};
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), retired));
    CHECK_EQ(retired.retirementReason, SourceRetirementReason::Detached);
}

void testSeedlessDetachCannotRetireStreamingSourceSlot() {
    StreamingFixture fixture;
    buildStreaming(fixture, 125, 16, 6100, 0xAABBCCDDEEFF0011ull);

    DetachPayload zero{};
    zero.viewerId = fixture.identity;
    zero.generation = 1u;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(zero, payload));
    CHECK_TRUE(encodeDatagram(
        MessageType::Detach, spectatorIdentity(), 0x70000u, payload,
        packet));
    CHECK_EQ(fixture.source->onDatagram(
                 fixture.endpoint, packet.data(), packet.size(),
                 ++fixture.now),
             IngressResult::ResumeMismatch);
    CHECK_EQ(fixture.source->viewerCount(), size_t{1});
    SourceViewerSnapshot streaming{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, streaming));
    CHECK_EQ(streaming.phase, SourceViewerPhase::Streaming);
}

void testStalePinningHitsBoundedPhaseDeadlines() {
    StreamingFixture fixture;
    buildStreaming(fixture, 124, 12, 6000, 0x445566778899AABBull);

    InitAckPayload duplicate{};
    duplicate.gameId = fixture.gameId;
    duplicate.viewerId = fixture.identity;
    duplicate.generation = 1u;
    duplicate.viewerNonce = viewerGenerationNonce(fixture.identity, 1u);
    GameDescriptor canonical{};
    CHECK_TRUE(fixture.archive.descriptor(fixture.gameId, canonical));
    duplicate.startFingerprint =
        gameStartFingerprint(canonicalGameStart(canonical));
    duplicate.resumeRecord = 0;
    duplicate.resumeEvent = 0;
    uint64_t zeroDigest = 0;
    CHECK_TRUE(fixture.archive.chainDigestAt(
        fixture.gameId, 0, 0, zeroDigest));
    duplicate.resumeDigest = zeroDigest;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(duplicate, payload));
    fixture.now += 100;
    const auto probe = fixture.viewer->pollOutbound(fixture.now, 4);
    CHECK_TRUE(!probe.empty());
    uint32_t injectedSequence = 1u;
    for (const auto& out : probe) {
        injectedSequence = out.sequence + 1u;
        fixture.source->onDatagram(
            fixture.endpoint, out.bytes.data(), out.bytes.size(),
            fixture.now);
    }
    CHECK_TRUE(encodeDatagram(
        MessageType::InitAck, spectatorIdentity(), injectedSequence,
        payload, packet));
    SourceViewerSnapshot before{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, before));
    fixture.now += 1000;
    CHECK_EQ(fixture.source->onDatagram(
                 fixture.endpoint, packet.data(), packet.size(),
                 fixture.now),
             IngressResult::UnexpectedMessage);
    SourceViewerSnapshot afterDuplicate{};
    CHECK_TRUE(
        fixture.source->viewerSnapshot(fixture.identity, afterDuplicate));
    CHECK_EQ(afterDuplicate.lastValidReceiveMs, before.lastValidReceiveMs);

    CHECK_EQ(fixture.archive.sealGame(
                 fixture.gameId, TerminalReason::MatchEnded,
                 postStateHashFor(11)),
             ArchiveResult::Ok);
    fixture.pump(20);
    SourceViewerSnapshot awaiting{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, awaiting));
    CHECK_EQ(awaiting.phase, SourceViewerPhase::AwaitingTerminalAck);

    const uint64_t deadlineStart = fixture.now;
    for (unsigned step = 0;
         step < 400 && fixture.source->viewerCount() != 0;
         ++step) {
        fixture.now += 100;
        for (const auto& out : fixture.viewer->pollOutbound(fixture.now, 4)) {
            fixture.source->onDatagram(
                fixture.endpoint, out.bytes.data(), out.bytes.size(),
                fixture.now);
        }
        fixture.source->pollOutbound(fixture.now, 16);
    }
    CHECK_EQ(fixture.source->viewerCount(), size_t{0});
    const uint64_t elapsed = fixture.now - deadlineStart;
    CHECK_TRUE(elapsed
               <= fixture.sourceConfig.terminalAckDeadlineMs + 1000u);
    CHECK_TRUE(elapsed < fixture.sourceConfig.idleTimeoutMs * 3u);
    SourceViewerSnapshot retired{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, retired));
    CHECK_EQ(retired.retirementReason,
             SourceRetirementReason::TerminalAckDeadline);
}

void testSourceHeartbeatKeepsPausedViewerAlive() {
    StreamingFixture fixture;
    buildStreaming(fixture, 125, 20, 7000, 0x5566778899AABBCCull);
    fixture.source->setSourcePhase(SourcePhase::Paused);

    const uint32_t generationBefore = fixture.viewer->generation();
    unsigned heartbeats = 0;
    for (unsigned step = 0; step < 60; ++step) {
        fixture.now += 500;
        for (const auto& out : fixture.viewer->pollOutbound(fixture.now, 4)) {
            fixture.source->onDatagram(
                fixture.endpoint, out.bytes.data(), out.bytes.size(),
                fixture.now);
        }
        for (const auto& out : fixture.source->pollOutbound(fixture.now, 16)) {
            if (out.type == MessageType::Heartbeat) ++heartbeats;
            CHECK_EQ(fixture.viewer->onDatagram(
                         out.bytes.data(), out.bytes.size(), fixture.now),
                     IngressResult::Accepted);
        }
    }
    CHECK_TRUE(heartbeats >= 10u);
    CHECK_EQ(fixture.viewer->generation(), generationBefore);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);
    CHECK_EQ(fixture.source->viewerCount(), size_t{1});
    CHECK_TRUE(fixture.viewer->lastValidReceiveMs() + 2000u >= fixture.now);

    HeartbeatPayload status{};
    CHECK_TRUE(fixture.viewer->sourceStatus(status));
    CHECK_EQ(status.sourcePhase, SourcePhase::Paused);
    CHECK_EQ(status.nextRecord, 20u);
    CHECK_EQ(status.viewerCount, uint16_t{1});
}

void testPerViewerLagCapsAndSlowViewerIsolation() {
    auto runLimit = [](uint64_t gameId,
                       uint32_t recordLimit,
                       size_t byteLimit,
                       uint64_t viewerSecret) {
        SpectatorArchive archive;
        CHECK_EQ(archive.beginGame(descriptor(gameId, 9000)),
                 ArchiveResult::Ok);

        SourceConfig sourceConfig{};
        sourceConfig.identity = spectatorIdentity();
        sourceConfig.simulationContract = kSimulationContract;
        sourceConfig.compatibilityIdentity = compatibilityIdentity();
        sourceConfig.recordsPerBatch = 4;
        sourceConfig.perViewerPacketBudget = 2;
        sourceConfig.globalPacketBudget = 8;
        sourceConfig.maxUnackedRecords = recordLimit;
        sourceConfig.maxUnackedBytes = byteLimit;
        sourceConfig.noProgressDeadlineMs = 60000;
        sourceConfig.idleTimeoutMs = 60000;
        SpectatorSource source(sourceConfig, archive);

        ViewerConfig viewerConfig{};
        viewerConfig.identity = spectatorIdentity();
        viewerConfig.simulationContract = kSimulationContract;
        viewerConfig.compatibilityIdentity = compatibilityIdentity();
        viewerConfig.secureRandom = viewerSecret;
        viewerConfig.viewerNonce = viewerSecret;
        SpectatorViewer viewer(viewerConfig);
        CHECK_TRUE(source.isConfigured());
        CHECK_TRUE(viewer.begin(gameId));

        uint64_t now = 1;
        constexpr EndpointId endpoint = 0x4400u;
        for (unsigned step = 0;
             step < 32 && viewer.phase() != ViewerPhase::Streaming;
             ++step) {
            now += 25;
            for (const auto& packet : viewer.pollOutbound(now, 8)) {
                source.onDatagram(endpoint, packet.bytes.data(),
                                  packet.bytes.size(), now);
            }
            for (const auto& packet : source.pollOutbound(now, 8)) {
                if (packet.destination == endpoint) {
                    viewer.onDatagram(packet.bytes.data(),
                                      packet.bytes.size(), now);
                }
            }
        }
        CHECK_EQ(viewer.phase(), ViewerPhase::Streaming);
        for (const auto& packet : viewer.pollOutbound(++now, 8)) {
            source.onDatagram(endpoint, packet.bytes.data(),
                              packet.bytes.size(), now);
        }

        for (uint32_t index = 0; index < 7; ++index) {
            CHECK_EQ(archive.appendConfirmed(
                         gameId, recordFor(index, 9000), nullptr),
                     ArchiveResult::Ok);
        }
        source.pollOutbound(++now, 8);
        CHECK_EQ(source.viewerCount(), size_t{1});

        CHECK_EQ(archive.appendConfirmed(
                     gameId, recordFor(7, 9000), nullptr),
                 ArchiveResult::Ok);
        source.pollOutbound(++now, 8);
        CHECK_EQ(source.viewerCount(), size_t{1});
        SourceViewerSnapshot atCap{};
        CHECK_TRUE(source.viewerSnapshot(viewerSecret, atCap));
        CHECK_EQ(atCap.unackedRecords, 8u);
        CHECK_TRUE(atCap.unackedBytes
                   >= 8u * sizeof(ConfirmedRecord));
        CHECK_TRUE(!atCap.caughtUp);

        CHECK_EQ(archive.appendConfirmed(
                     gameId, recordFor(8, 9000), nullptr),
                 ArchiveResult::Ok);
        source.pollOutbound(++now, 8);
        CHECK_EQ(source.viewerCount(), size_t{0});
        SourceViewerSnapshot retired{};
        CHECK_TRUE(source.viewerSnapshot(viewerSecret, retired));
        CHECK_EQ(retired.retirementReason,
                 SourceRetirementReason::LagLimit);
    };

    runLimit(126, 8u, 1024u * 1024u,
             0x7060504030201001ull);
    runLimit(127, 100u, 8u * sizeof(ConfirmedRecord),
             0x7060504030201002ull);

    ArchiveConfig archiveConfig{};
    archiveConfig.maxRetainedRecords = 8u;
    archiveConfig.segmentRecords = 4u;
    archiveConfig.maxRetainedBytes =
        16u * sizeof(ConfirmedRecord);
    SpectatorArchive archive(archiveConfig);
    const uint64_t gameId = 128;
    CHECK_EQ(archive.beginGame(descriptor(gameId, 9100)),
             ArchiveResult::Ok);

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    sourceConfig.recordsPerBatch = 4;
    sourceConfig.perViewerPacketBudget = 4;
    sourceConfig.globalPacketBudget = 16;
    sourceConfig.maxUnackedRecords = 1000;
    sourceConfig.maxUnackedBytes = 1024u * 1024u;
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig fastConfig{};
    fastConfig.identity = spectatorIdentity();
    fastConfig.simulationContract = kSimulationContract;
    fastConfig.compatibilityIdentity = compatibilityIdentity();
    fastConfig.secureRandom = 0x8100000000000001ull;
    fastConfig.viewerNonce = fastConfig.secureRandom;
    SpectatorViewer fast(fastConfig);
    ViewerConfig slowConfig = fastConfig;
    slowConfig.secureRandom = 0x8100000000000002ull;
    slowConfig.viewerNonce = slowConfig.secureRandom;
    SpectatorViewer slow(slowConfig);
    CHECK_TRUE(fast.begin(gameId));
    CHECK_TRUE(slow.begin(gameId));
    constexpr EndpointId fastEndpoint = 0x5501u;
    constexpr EndpointId slowEndpoint = 0x5502u;
    uint64_t now = 1;

    auto pump = [&](bool deliverSlowRecords) {
        now += 25;
        for (const auto& packet : fast.pollOutbound(now, 8)) {
            source.onDatagram(fastEndpoint, packet.bytes.data(),
                              packet.bytes.size(), now);
        }
        for (const auto& packet : slow.pollOutbound(now, 8)) {
            source.onDatagram(slowEndpoint, packet.bytes.data(),
                              packet.bytes.size(), now);
        }
        for (const auto& packet : source.pollOutbound(now, 16)) {
            if (packet.destination == fastEndpoint) {
                fast.onDatagram(packet.bytes.data(),
                                packet.bytes.size(), now);
            } else if (packet.destination == slowEndpoint
                       && (deliverSlowRecords
                           || packet.type
                               != MessageType::ConfirmedInputBatch)) {
                slow.onDatagram(packet.bytes.data(),
                                packet.bytes.size(), now);
            }
        }
    };

    for (unsigned step = 0;
         step < 64
         && (fast.phase() != ViewerPhase::Streaming
             || slow.phase() != ViewerPhase::Streaming);
         ++step) {
        pump(true);
    }
    CHECK_EQ(fast.phase(), ViewerPhase::Streaming);
    CHECK_EQ(slow.phase(), ViewerPhase::Streaming);

    for (uint32_t index = 0; index < 9; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     gameId, recordFor(index, 9100), nullptr),
                 ArchiveResult::Ok);
        for (unsigned step = 0; step < 32; ++step) {
            pump(false);
            SourceViewerSnapshot fastState{};
            if (source.viewerSnapshot(fast.viewerId(), fastState)
                && fastState.acknowledgedNextRecord == index + 1u) {
                break;
            }
        }
    }
    CHECK_EQ(source.viewerCount(), size_t{1});
    SourceViewerSnapshot fastState{};
    CHECK_TRUE(source.viewerSnapshot(fast.viewerId(), fastState));
    CHECK_TRUE(!fastState.retired);
    CHECK_EQ(fastState.acknowledgedNextRecord, 9u);
    SourceViewerSnapshot slowState{};
    CHECK_TRUE(source.viewerSnapshot(slow.viewerId(), slowState));
    CHECK_TRUE(slowState.retired);
    CHECK_EQ(slowState.retirementReason,
             SourceRetirementReason::ArchiveUnavailable);
    CHECK_EQ(slow.phase(), ViewerPhase::Rejected);
    ArchiveExtent extent{};
    CHECK_TRUE(archive.extent(gameId, extent));
    CHECK_EQ(extent.nextRecord - extent.firstRecord, 8u);
    CHECK_TRUE(archive.allocatedRecordBytes()
               <= archiveConfig.maxRetainedBytes);
}

void testArchiveSegmentRotationAndPinning() {
    ArchiveConfig config{};
    config.maxRetainedRecords = 8u;
    config.segmentRecords = 4u;
    config.maxRetainedBytes =
        16u * sizeof(ConfirmedRecord);
    SpectatorArchive archive(config);
    CHECK_TRUE(archive.isConfigured());
    const GameDescriptor game = descriptor(200, 10);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);

    for (uint32_t index = 0; index < 7; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     200, recordFor(index, 10), nullptr),
                 ArchiveResult::Ok);
    }
    ArchiveExtent extent{};
    CHECK_TRUE(archive.extent(200, extent));
    CHECK_EQ(extent.nextRecord - extent.firstRecord, 7u);
    CHECK_EQ(archive.retainedRecordCount(), size_t{7});
    CHECK_EQ(archive.appendConfirmed(
                 200, recordFor(7, 10), nullptr),
             ArchiveResult::Ok);
    CHECK_TRUE(archive.extent(200, extent));
    CHECK_EQ(extent.nextRecord - extent.firstRecord, 8u);
    CHECK_EQ(archive.retainedRecordCount(), size_t{8});
    CHECK_TRUE(!archive.retentionBlocked());

    CHECK_TRUE(archive.setResumeFloor(200, 0u));
    ArchiveCheckpointRef checkpoint{};
    checkpoint.checkpointId = 0x9001u;
    checkpoint.recordCursor = 0u;
    checkpoint.digest = 1u;
    CHECK_EQ(archive.addCheckpoint(200, checkpoint), ArchiveResult::Ok);

    CHECK_EQ(archive.appendConfirmed(
                 200, recordFor(8, 10), nullptr),
             ArchiveResult::Ok);
    CHECK_TRUE(archive.extent(200, extent));
    CHECK_EQ(extent.nextRecord, 9u);
    CHECK_EQ(extent.firstRecord, 1u);
    CHECK_EQ(extent.nextRecord - extent.firstRecord, 8u);
    CHECK_EQ(archive.retainedRecordCount(), size_t{8});
    CHECK_TRUE(archive.retentionBlocked());
    std::vector<ArchiveCheckpointRef> retainedCheckpoints;
    CHECK_TRUE(archive.checkpoints(200, retainedCheckpoints));
    CHECK_TRUE(retainedCheckpoints.empty());

    ConfirmedRecord dropped{};
    CHECK_TRUE(!archive.record(200, 0, dropped));
    ConfirmedInputBatchPayload batch{};
    CHECK_EQ(archive.readBatch(200, 0, 8, batch), ArchiveResult::Gap);
    CHECK_EQ(archive.readBatch(200, 1, 8, batch), ArchiveResult::Ok);
    CHECK_EQ(batch.startRecord, 1u);
    uint64_t floorDigest = 0;
    CHECK_TRUE(archive.digestAt(200, 1, floorDigest));
    CHECK_EQ(batch.baseDigest, floorDigest);
    CHECK_TRUE(!archive.digestAt(200, 0, floorDigest));

    for (uint32_t index = 9; index < 40; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     200, recordFor(index, 10), nullptr),
                 ArchiveResult::Ok);
        CHECK_TRUE(archive.extent(200, extent));
        CHECK_TRUE(extent.nextRecord - extent.firstRecord <= 8u);
        CHECK_TRUE(archive.retainedRecordCount() <= size_t{8});
        CHECK_TRUE(archive.allocatedRecordBytes()
                   <= config.maxRetainedBytes);
    }

    ArchiveCheckpointRef stale{};
    stale.checkpointId = 0x9002u;
    stale.recordCursor = 0u;
    CHECK_EQ(archive.addCheckpoint(200, stale), ArchiveResult::Gap);
}

void testArchiveHardByteCeiling() {
    ArchiveConfig config{};
    config.maxRetainedRecords =
        (std::numeric_limits<uint32_t>::max)();
    config.segmentRecords = 4u;
    config.maxRetainedBytes = 8u * sizeof(ConfirmedRecord);
    SpectatorArchive archive(config);
    CHECK_TRUE(archive.isConfigured());
    CHECK_EQ(archive.beginGame(descriptor(202, 30)), ArchiveResult::Ok);

    for (uint32_t index = 0; index < 8; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     202, recordFor(index, 30), nullptr),
                 ArchiveResult::Ok);
    }
    CHECK_EQ(archive.allocatedRecordBytes(), config.maxRetainedBytes);
    ArchiveExtent before{};
    CHECK_TRUE(archive.extent(202, before));

    CHECK_EQ(archive.appendConfirmed(
                 202, recordFor(8, 30), nullptr),
             ArchiveResult::ArchiveFull);
    ArchiveExtent after{};
    CHECK_TRUE(archive.extent(202, after));
    CHECK_EQ(after.nextRecord, before.nextRecord);
    CHECK_EQ(after.digest, before.digest);
    CHECK_EQ(archive.allocatedRecordBytes(), config.maxRetainedBytes);
    CHECK_TRUE(!archive.serviceDisabled());
}

void testArchiveAllocationFailureDisablesServiceWithoutThrowing() {
    ArchiveConfig config{};
    config.segmentRecords = 4u;
    SpectatorArchive archive(config);
    const GameDescriptor game = descriptor(201, 20);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    for (uint32_t index = 0; index < 4; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     201, recordFor(index, 20), nullptr),
                 ArchiveResult::Ok);
    }
    CHECK_TRUE(!archive.serviceDisabled());

    archive.setSegmentAllocationFailuresForTest(1);
    CHECK_EQ(archive.appendConfirmed(201, recordFor(4, 20), nullptr),
             ArchiveResult::AllocationFailed);
    CHECK_TRUE(archive.serviceDisabled());
    CHECK_EQ(archive.appendConfirmed(201, recordFor(4, 20), nullptr),
             ArchiveResult::AllocationFailed);
    ArchiveExtent extent{};
    CHECK_TRUE(archive.extent(201, extent));
    CHECK_EQ(extent.nextRecord, 4u);
    ConfirmedInputBatchPayload batch{};
    CHECK_EQ(archive.readBatch(201, 0, 4, batch), ArchiveResult::Ok);
    CHECK_EQ(batch.records.size(), size_t{4});
}

SetEvent setEventAt(const SpectatorArchive& archive,
                    uint64_t gameId,
                    uint32_t eventIndex,
                    uint32_t recordCursor,
                    uint32_t firstFrame,
                    SetEventKind kind) {
    SetEvent event{};
    event.eventIndex = eventIndex;
    event.recordCursor = recordCursor;
    event.timelineFrame = firstFrame + recordCursor;
    event.kind = kind;
    uint64_t chain = 0;
    CHECK_TRUE(archive.chainDigestAt(
        gameId, recordCursor, eventIndex, chain));
    event.recordDigest = chain;
    return event;
}

SetEvent rosterEventAt(const SpectatorArchive& archive,
                       uint64_t gameId,
                       uint32_t eventIndex,
                       uint32_t recordCursor,
                       uint32_t firstFrame,
                       uint32_t rosterEpoch) {
    SetEvent event = setEventAt(
        archive, gameId, eventIndex, recordCursor, firstFrame,
        SetEventKind::RosterSelected);
    event.rosterEpoch = rosterEpoch;
    event.activeSlotMask = 0x03u;
    event.selectorProofMask = 0x03u;
    event.roster[0].nativeCharacterId = 4u;
    event.roster[0].paletteIndex = 1u;
    event.roster[1].nativeCharacterId = 17u;
    event.roster[1].paletteIndex = 2u;
    event.selectorProof[0].cursorCell = 4u;
    event.selectorProof[0].confirmState = 3u;
    event.selectorProof[1].cursorCell = 17u;
    event.selectorProof[1].alternateFlag = 1u;
    event.selectorProof[1].confirmState = 3u;
    return event;
}

void testLiveEventRepairPreemptsFreshPublication() {
    constexpr uint64_t gameId = 299u;
    constexpr uint32_t firstFrame = 3950u;
    constexpr EndpointId endpoint = 0x010203040D0Eull;
    constexpr uint64_t retransmitMs = 80u;

    SpectatorArchive archive;
    CHECK_EQ(archive.beginGame(descriptor(gameId, firstFrame)),
             ArchiveResult::Ok);

    SourceConfig sourceConfig{};
    sourceConfig.identity = spectatorIdentity();
    sourceConfig.simulationContract = kSimulationContract;
    sourceConfig.compatibilityIdentity = compatibilityIdentity();
    sourceConfig.recordsPerBatch = 8u;
    sourceConfig.sendWindowRecords = 96u;
    sourceConfig.perViewerPacketBudget = 1u;
    sourceConfig.globalPacketBudget = 8u;
    sourceConfig.handshakeResendMs = 20u;
    sourceConfig.retransmitMs = retransmitMs;
    sourceConfig.heartbeatIntervalMs = 1000u;
    SpectatorSource source(sourceConfig, archive);

    ViewerConfig viewerConfig{};
    viewerConfig.identity = spectatorIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.viewerNonce = 0xABCDEF1234567804ull;
    viewerConfig.helloResendMs = 20u;
    viewerConfig.ackResendMs = 20u;
    SpectatorViewer viewer(viewerConfig);
    CHECK_TRUE(source.isConfigured());
    CHECK_TRUE(viewer.begin(gameId));

    uint64_t now = 1u;
    CHECK_TRUE(establishEmptyStreamingSession(
        source, viewer, endpoint, now));

    CHECK_EQ(archive.appendConfirmed(
                 gameId, recordFor(0u, firstFrame), nullptr),
             ArchiveResult::Ok);
    ++now;
    for (const SourceDatagram& packet : source.pollOutbound(now, 8u)) {
        if (packet.destination != endpoint) continue;
        CHECK_EQ(viewer.onDatagram(packet.bytes.data(), packet.bytes.size(),
                                   now),
                 IngressResult::Accepted);
    }
    for (unsigned step = 0u; step < 8u; ++step) {
        ++now;
        for (const ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
            const IngressResult result = source.onDatagram(
                endpoint, packet.bytes.data(), packet.bytes.size(), now);
            CHECK_TRUE(result == IngressResult::Accepted
                       || result == IngressResult::StaleAck);
        }
        SourceViewerSnapshot snapshot{};
        if (source.viewerSnapshot(viewer.viewerId(), snapshot)
            && snapshot.acknowledgedNextRecord == 1u) {
            break;
        }
    }
    SourceViewerSnapshot snapshot{};
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.acknowledgedNextRecord, 1u);
    CHECK_EQ(snapshot.acknowledgedNextEvent, 0u);

    SetEvent roster = rosterEventAt(
        archive, gameId, 0u, 1u, firstFrame, 12u);
    CHECK_EQ(archive.appendEvent(gameId, roster, nullptr),
             ArchiveResult::Ok);
    const uint64_t holeOpenedMs = ++now;
    std::vector<SourceDatagram> packets = source.pollOutbound(now, 8u);
    size_t droppedBatches = 0u;
    for (const SourceDatagram& packet : packets) {
        if (packet.type != MessageType::SetEvent) continue;
        Datagram datagram{};
        CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                spectatorIdentity(), datagram),
                 DecodeError::None);
        SetEventBatchPayload batch{};
        CHECK_TRUE(decodePayload(datagram.payload, batch));
        CHECK_EQ(batch.startEvent, 0u);
        ++droppedBatches;
    }
    CHECK_EQ(droppedBatches, size_t{1});

    SetEvent stage = setEventAt(
        archive, gameId, 1u, 1u, firstFrame,
        SetEventKind::StageSelected);
    stage.valueC = 7u;
    CHECK_EQ(archive.appendEvent(gameId, stage, nullptr),
             ArchiveResult::Ok);
    ++now;
    packets = source.pollOutbound(now, 8u);
    size_t newerBatches = 0u;
    for (const SourceDatagram& packet : packets) {
        if (packet.type != MessageType::SetEvent) continue;
        Datagram datagram{};
        CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                spectatorIdentity(), datagram),
                 DecodeError::None);
        SetEventBatchPayload batch{};
        CHECK_TRUE(decodePayload(datagram.payload, batch));
        CHECK_EQ(batch.startEvent, 1u);
        ++newerBatches;
        CHECK_EQ(viewer.onDatagram(packet.bytes.data(), packet.bytes.size(),
                                   now),
                 IngressResult::Accepted);
    }
    CHECK_EQ(newerBatches, size_t{1});
    ++now;
    for (const ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
        const IngressResult result = source.onDatagram(
            endpoint, packet.bytes.data(), packet.bytes.size(), now);
        CHECK_TRUE(result == IngressResult::Accepted
                   || result == IngressResult::StaleAck);
    }
    CHECK_EQ(viewer.receivedNextEvent(), 0u);

    SetEvent started = setEventAt(
        archive, gameId, 2u, 1u, firstFrame,
        SetEventKind::MatchStarted);
    started.matchId = 1u;
    started.valueC = 1u;
    CHECK_EQ(archive.appendEvent(gameId, started, nullptr),
             ArchiveResult::Ok);

    now = holeOpenedMs + retransmitMs;
    packets = source.pollOutbound(now, 8u);
    size_t repairBatches = 0u;
    SourceDatagram repairPacket{};
    for (const SourceDatagram& packet : packets) {
        if (packet.type != MessageType::SetEvent) continue;
        Datagram datagram{};
        CHECK_EQ(decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                                spectatorIdentity(), datagram),
                 DecodeError::None);
        SetEventBatchPayload batch{};
        CHECK_TRUE(decodePayload(datagram.payload, batch));
        CHECK_EQ(batch.startEvent, 0u);
        CHECK_EQ(batch.events.size(), size_t{3});
        repairPacket = packet;
        ++repairBatches;
    }
    CHECK_EQ(repairBatches, size_t{1});
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.eventRepairSends, 1u);
    CHECK_EQ(snapshot.acknowledgedNextEvent, 0u);

    if (!repairPacket.bytes.empty()) {
        CHECK_EQ(viewer.onDatagram(repairPacket.bytes.data(),
                                   repairPacket.bytes.size(), now),
                 IngressResult::Accepted);
    }
    CHECK_EQ(viewer.receivedNextEvent(), 3u);
    ++now;
    for (const ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
        const IngressResult result = source.onDatagram(
            endpoint, packet.bytes.data(), packet.bytes.size(), now);
        CHECK_TRUE(result == IngressResult::Accepted
                   || result == IngressResult::StaleAck);
    }
    CHECK_TRUE(source.viewerSnapshot(viewer.viewerId(), snapshot));
    CHECK_EQ(snapshot.acknowledgedNextEvent, 3u);
}

void testPaletteEventBindsToTheOpenMatch() {
    SpectatorArchive archive;
    const uint32_t firstFrame = 6000;
    const GameDescriptor game = descriptor(310, firstFrame);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    for (uint32_t index = 0; index < 3; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     310, recordFor(index, firstFrame), nullptr),
                 ArchiveResult::Ok);
    }

    const auto paletteEventAt = [&](uint32_t eventIndex, uint32_t matchId,
                                    uint8_t slotMask, uint32_t variant) {
        SetEvent event = setEventAt(
            archive, 310, eventIndex, 3, firstFrame,
            SetEventKind::PaletteSelected);
        event.matchId = matchId;
        event.activeSlotMask = slotMask;
        event.valueC = variant;
        for (size_t i = 4; i < kSetPaletteEntryBytes; ++i)
            event.paletteEntries[i] = static_cast<uint8_t>(i * 7u);
        return event;
    };

    CHECK_EQ(archive.appendEvent(
                 310, paletteEventAt(0, 1u, 0x01u, 2u), nullptr),
             ArchiveResult::EventOrderViolation);

    SetEvent started = setEventAt(
        archive, 310, 0, 3, firstFrame, SetEventKind::MatchStarted);
    started.matchId = 1u;
    started.valueC = 1u;
    CHECK_EQ(archive.appendEvent(310, started, nullptr), ArchiveResult::Ok);

    CHECK_EQ(archive.appendEvent(
                 310, paletteEventAt(1, 2u, 0x01u, 2u), nullptr),
             ArchiveResult::EventOrderViolation);
    CHECK_EQ(archive.appendEvent(
                 310, paletteEventAt(1, 1u, 0x03u, 2u), nullptr),
             ArchiveResult::InvalidArgument);
    CHECK_EQ(archive.appendEvent(
                 310, paletteEventAt(1, 1u, 0x01u, 4u), nullptr),
             ArchiveResult::InvalidArgument);

    SetEvent stored{};
    CHECK_EQ(archive.appendEvent(
                 310, paletteEventAt(1, 1u, 0x02u, 3u), &stored),
             ArchiveResult::Ok);
    CHECK_TRUE(stored.rollingDigest != 0);

    SetEvent smuggling = setEventAt(
        archive, 310, 2, 3, firstFrame, SetEventKind::RoundStarted);
    smuggling.matchId = 1u;
    smuggling.valueC = 1u;
    smuggling.paletteEntries[10] = 0x55u;
    CHECK_EQ(archive.appendEvent(310, smuggling, nullptr),
             ArchiveResult::InvalidArgument);

    SetEventBatchPayload batch{};
    CHECK_EQ(archive.readEventBatch(310, 0, kMaxSetEventsPerBatch, batch),
             ArchiveResult::Ok);
    CHECK_EQ(batch.events.size(), size_t{2});
    std::vector<uint8_t> wire;
    CHECK_TRUE(encodePayload(batch, wire));
    SetEventBatchPayload decoded{};
    CHECK_TRUE(decodePayload(wire, decoded));
    CHECK_EQ(decoded.events.size(), size_t{2});
    CHECK_TRUE(decoded.events[1] == stored);

    SetEvent ended = setEventAt(
        archive, 310, 2, 3, firstFrame, SetEventKind::MatchResult);
    ended.matchId = 1u;
    CHECK_EQ(archive.appendEvent(310, ended, nullptr), ArchiveResult::Ok);
    CHECK_EQ(archive.appendEvent(
                 310, paletteEventAt(3, 1u, 0x02u, 3u), nullptr),
             ArchiveResult::EventOrderViolation);
}

void testSetEventOrderingDigestBindingAndIndex() {
    SpectatorArchive archive;
    const uint32_t firstFrame = 4000;
    const GameDescriptor game = descriptor(300, firstFrame);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);

    for (uint32_t index = 0; index < 4; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     300, recordFor(index, firstFrame), nullptr),
                 ArchiveResult::Ok);
    }

    SetEvent early = rosterEventAt(archive, 300, 0, 4, firstFrame, 7u);
    early.recordCursor = 9u;
    early.timelineFrame = firstFrame + 9u;
    CHECK_EQ(archive.appendEvent(300, early, nullptr),
             ArchiveResult::TimelineMismatch);
    SetEvent late = rosterEventAt(archive, 300, 0, 4, firstFrame, 7u);
    late.recordCursor = 2u;
    late.timelineFrame = firstFrame + 2u;
    CHECK_EQ(archive.appendEvent(300, late, nullptr),
             ArchiveResult::TimelineMismatch);

    SetEvent roster = rosterEventAt(archive, 300, 0, 4, firstFrame, 7u);
    SetEvent storedRoster{};
    CHECK_EQ(archive.appendEvent(300, roster, &storedRoster),
             ArchiveResult::Ok);
    CHECK_TRUE(storedRoster.rollingDigest != 0);
    CHECK_EQ(archive.appendEvent(300, storedRoster, nullptr),
             ArchiveResult::ExactDuplicate);

    SetEvent partial = rosterEventAt(archive, 300, 1, 4, firstFrame, 8u);
    partial.activeSlotMask = 0x01u;
    CHECK_EQ(archive.appendEvent(300, partial, nullptr),
             ArchiveResult::InvalidArgument);
    SetEvent unproven = rosterEventAt(archive, 300, 1, 4, firstFrame, 8u);
    unproven.selectorProofMask = 0x07u;
    CHECK_EQ(archive.appendEvent(300, unproven, nullptr),
             ArchiveResult::InvalidArgument);
    SetEvent epochless = rosterEventAt(archive, 300, 1, 4, firstFrame, 0u);
    CHECK_EQ(archive.appendEvent(300, epochless, nullptr),
             ArchiveResult::InvalidArgument);

    SetEvent stage = setEventAt(
        archive, 300, 1, 4, firstFrame, SetEventKind::StageSelected);
    stage.valueC = 12u;
    CHECK_EQ(archive.appendEvent(300, stage, nullptr), ArchiveResult::Ok);

    SetEvent matchOne = setEventAt(
        archive, 300, 2, 4, firstFrame, SetEventKind::MatchStarted);
    matchOne.matchId = 1u;
    matchOne.valueC = 1u;
    CHECK_EQ(archive.appendEvent(300, matchOne, nullptr), ArchiveResult::Ok);

    SetEvent orphan = setEventAt(
        archive, 300, 3, 4, firstFrame, SetEventKind::MatchResult);
    orphan.matchId = 99u;
    CHECK_EQ(archive.appendEvent(300, orphan, nullptr),
             ArchiveResult::EventOrderViolation);

    for (uint32_t index = 4; index < 20; ++index) {
        CHECK_EQ(archive.appendConfirmed(
                     300, recordFor(index, firstFrame), nullptr),
                 ArchiveResult::Ok);
    }
    SetEvent result = setEventAt(
        archive, 300, 3, 20, firstFrame, SetEventKind::MatchResult);
    result.matchId = 1u;
    result.valueA = 0x01u;
    result.valueB = 0x0002u | (0x0001u << 8);
    CHECK_EQ(archive.appendEvent(300, result, nullptr), ArchiveResult::Ok);

    SetEvent rematch = setEventAt(
        archive, 300, 4, 20, firstFrame,
        SetEventKind::ReturnedToCharacterSelect);
    rematch.matchId = 1u;
    CHECK_EQ(archive.appendEvent(300, rematch, nullptr), ArchiveResult::Ok);

    SetEvent roster2 = rosterEventAt(archive, 300, 5, 20, firstFrame, 9u);
    CHECK_EQ(archive.appendEvent(300, roster2, nullptr), ArchiveResult::Ok);
    SetEvent matchTwo = setEventAt(
        archive, 300, 6, 20, firstFrame, SetEventKind::MatchStarted);
    matchTwo.matchId = 2u;
    matchTwo.valueC = 2u;
    CHECK_EQ(archive.appendEvent(300, matchTwo, nullptr), ArchiveResult::Ok);

    std::vector<SetMatchIndexEntry> matches;
    CHECK_TRUE(archive.matchIndex(300, matches));
    CHECK_EQ(matches.size(), size_t{2});
    CHECK_EQ(matches[0].matchId, 1u);
    CHECK_EQ(matches[0].firstRecord, 4u);
    CHECK_EQ(matches[0].lastRecord, 20u);
    CHECK_TRUE(matches[0].ended);
    CHECK_TRUE(matches[0].independentBootstrap);
    CHECK_EQ(matches[0].stageId, 12u);
    CHECK_EQ(matches[0].rosterEpoch, 7u);
    CHECK_EQ(matches[0].winningSideMask, uint8_t{1});
    CHECK_EQ(matches[0].roundsSideZero, uint8_t{2});
    CHECK_EQ(matches[0].roundsSideOne, uint8_t{1});
    CHECK_EQ(matches[0].roster[1].nativeCharacterId, uint8_t{17});
    CHECK_EQ(matches[1].matchId, 2u);
    CHECK_EQ(matches[1].firstRecord, 20u);
    CHECK_EQ(matches[1].rosterEpoch, 9u);
    CHECK_TRUE(!matches[1].independentBootstrap);

    uint32_t seek = 0;
    CHECK_TRUE(archive.seekRecordForMatch(300, 1u, seek));
    CHECK_EQ(seek, 4u);
    CHECK_TRUE(archive.seekRecordForMatch(300, 2u, seek));
    CHECK_EQ(seek, 20u);
    CHECK_TRUE(!archive.seekRecordForMatch(300, 3u, seek));

    SetMatchIndexEntry current{};
    CHECK_TRUE(archive.currentMatch(300, 20u, current));
    CHECK_EQ(current.matchId, 2u);
    CHECK_TRUE(archive.currentMatch(300, 10u, current));
    CHECK_EQ(current.matchId, 1u);

    SetEvent pause = setEventAt(
        archive, 300, 7, 20, firstFrame, SetEventKind::SessionPaused);
    CHECK_EQ(archive.appendEvent(300, pause, nullptr), ArchiveResult::Ok);
    bool paused = false;
    CHECK_TRUE(archive.setPaused(300, paused));
    CHECK_TRUE(paused);
    SetEvent pauseAgain = setEventAt(
        archive, 300, 8, 20, firstFrame, SetEventKind::SessionPaused);
    CHECK_EQ(archive.appendEvent(300, pauseAgain, nullptr),
             ArchiveResult::EventOrderViolation);
    SetEvent resumeEvent = setEventAt(
        archive, 300, 8, 20, firstFrame, SetEventKind::SessionResumed);
    CHECK_EQ(archive.appendEvent(300, resumeEvent, nullptr),
             ArchiveResult::Ok);

    ArchiveExtent beforeDelay{};
    CHECK_TRUE(archive.extent(300, beforeDelay));
    SetEvent delayed = setEventAt(
        archive, 300, 9, 20, firstFrame, SetEventKind::DelayChanged);
    delayed.valueA = 3u;
    delayed.valueB = 5u;
    delayed.valueC = 8u;
    CHECK_EQ(archive.appendEvent(300, delayed, nullptr), ArchiveResult::Ok);
    ArchiveExtent afterDelay{};
    CHECK_TRUE(archive.extent(300, afterDelay));
    CHECK_EQ(afterDelay.nextRecord, beforeDelay.nextRecord);
    CHECK_TRUE(afterDelay.digest != beforeDelay.digest);
    CHECK_EQ(afterDelay.nextEvent, beforeDelay.nextEvent + 1u);

    ConfirmedRecord next = recordFor(20, firstFrame);
    ConfirmedRecord storedNext{};
    CHECK_EQ(archive.appendConfirmed(300, next, &storedNext),
             ArchiveResult::Ok);
    CHECK_EQ(storedNext.rollingDigest,
             advanceRollingDigest(afterDelay.digest, 300, storedNext));
    CHECK_TRUE(storedNext.rollingDigest
               != advanceRollingDigest(
                      beforeDelay.digest, 300, storedNext));

    ConfirmedInputBatchPayload spanning{};
    CHECK_EQ(archive.readBatch(300, 0, 32, spanning), ArchiveResult::Ok);
    CHECK_EQ(spanning.records.size(), size_t{4});
    CHECK_EQ(spanning.baseEventIndex, 0u);
    ConfirmedInputBatchPayload afterEvents{};
    CHECK_EQ(archive.readBatch(300, 4, 32, afterEvents),
             ArchiveResult::Ok);
    CHECK_EQ(afterEvents.baseEventIndex, 3u);
    CHECK_EQ(afterEvents.records.size(), size_t{16});
}

void testSetEventChainRejectsDropReorderAndInsertion() {
    SpectatorArchive archive;
    const uint32_t firstFrame = 900;
    const GameDescriptor game = descriptor(301, firstFrame);
    CHECK_EQ(archive.beginGame(game), ArchiveResult::Ok);
    CHECK_EQ(archive.appendConfirmed(301, recordFor(0, firstFrame), nullptr),
             ArchiveResult::Ok);

    SetEvent first = rosterEventAt(archive, 301, 0, 1, firstFrame, 3u);
    CHECK_EQ(archive.appendEvent(301, first, nullptr), ArchiveResult::Ok);
    SetEvent second = setEventAt(
        archive, 301, 1, 1, firstFrame, SetEventKind::StageSelected);
    second.valueC = 5u;
    CHECK_EQ(archive.appendEvent(301, second, nullptr), ArchiveResult::Ok);
    SetEvent third = setEventAt(
        archive, 301, 2, 1, firstFrame, SetEventKind::MatchStarted);
    third.matchId = 1u;
    third.valueC = 1u;
    CHECK_EQ(archive.appendEvent(301, third, nullptr), ArchiveResult::Ok);

    SetEventBatchPayload batch{};
    CHECK_EQ(archive.readEventBatch(301, 0, 8, batch), ArchiveResult::Ok);
    CHECK_EQ(batch.events.size(), size_t{3});
    std::vector<uint8_t> payload;
    CHECK_TRUE(encodePayload(batch, payload));
    SetEventBatchPayload roundTrip{};
    CHECK_TRUE(decodePayload(payload, roundTrip));
    CHECK_EQ(roundTrip.events.size(), size_t{3});
    CHECK_EQ(roundTrip.events[2].rollingDigest,
             batch.events[2].rollingDigest);

    SetEventBatchPayload dropped = batch;
    dropped.events.erase(dropped.events.begin() + 1);
    dropped.events[1].eventIndex = 1u;
    CHECK_TRUE(!encodePayload(dropped, payload));

    SetEventBatchPayload reordered = batch;
    std::swap(reordered.events[0], reordered.events[1]);
    CHECK_TRUE(!encodePayload(reordered, payload));

    SetEventBatchPayload inserted = batch;
    SetEvent forged = inserted.events[1];
    forged.valueC = 6u;
    inserted.events[1] = forged;
    CHECK_TRUE(!encodePayload(inserted, payload));

    CHECK_TRUE(encodePayload(batch, payload));
    std::vector<uint8_t> tampered = payload;
    tampered[24u + 64u] ^= 0x01u;
    SetEventBatchPayload untouched{};
    untouched.startEvent = 0xABCDEFu;
    CHECK_TRUE(!decodePayload(tampered, untouched));
    CHECK_EQ(untouched.startEvent, 0xABCDEFu);
}

void testEventStarvedViewerRecoversWhenItsUnverifiedAckArmsAnEventRepair() {
    StreamingFixture fixture;
    buildStreaming(fixture, 511, 8, 8000, 0x1122334455667788ull);

    SetEvent roster = rosterEventAt(fixture.archive, 511, 0, 8, 8000, 5u);
    CHECK_EQ(fixture.archive.appendEvent(511, roster, nullptr),
             ArchiveResult::Ok);
    for (uint32_t index = 8; index < 32; ++index) {
        CHECK_EQ(fixture.archive.appendConfirmed(
                     511, recordFor(index, 8000), nullptr),
                 ArchiveResult::Ok);
    }

    for (unsigned step = 0; step < 60; ++step) {
        fixture.now += 30;
        for (const auto& packet : fixture.viewer->pollOutbound(fixture.now, 8))
            fixture.source->onDatagram(fixture.endpoint, packet.bytes.data(),
                                       packet.bytes.size(), fixture.now);
        for (const auto& packet :
             fixture.source->pollOutbound(fixture.now, 16)) {
            if (packet.destination != fixture.endpoint) continue;
            Datagram decoded{};
            if (decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                               spectatorIdentity(), decoded)
                    == DecodeError::None
                && decoded.header.type == MessageType::SetEvent) {
                continue;
            }
            fixture.viewer->onDatagram(packet.bytes.data(),
                                       packet.bytes.size(), fixture.now);
        }
    }

    CHECK_EQ(fixture.viewer->receivedNextEvent(), 0u);
    CHECK_EQ(fixture.viewer->awaitingEventIndex(), 1u);
    CHECK_TRUE(fixture.viewer->reorderedRecordCount() > 0);
    const uint32_t stalledAt = fixture.viewer->receivedNextRecord();
    CHECK_TRUE(stalledAt < 32u);

    SourceViewerSnapshot stalled{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, stalled));
    CHECK_EQ(stalled.acknowledgedNextEvent, 0u);
    CHECK_TRUE(stalled.acknowledgedNextRecord < 32u);

    fixture.pump(60);
    CHECK_EQ(fixture.viewer->awaitingEventIndex(), 0u);
    CHECK_EQ(fixture.viewer->receivedNextEvent(), 1u);
    CHECK_EQ(fixture.viewer->receivedNextRecord(), 32u);
    CHECK_EQ(fixture.viewer->reorderedRecordCount(), size_t{0});

    uint64_t sourceChain = 0;
    CHECK_TRUE(fixture.archive.chainDigestAt(511, 32u, 1u, sourceChain));
    CHECK_EQ(fixture.viewer->receivedDigest(), sourceChain);

    SourceViewerSnapshot healed{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, healed));
    CHECK_TRUE(healed.eventRepairSends > 0);
}

void testSetEventsReachViewerAndBindPlaybackChain() {
    StreamingFixture fixture;
    buildStreaming(fixture, 302, 8, 8000, 0x66778899AABBCCDDull);

    SetEvent roster =
        rosterEventAt(fixture.archive, 302, 0, 8, 8000, 11u);
    CHECK_EQ(fixture.archive.appendEvent(302, roster, nullptr),
             ArchiveResult::Ok);
    SetEvent started = setEventAt(
        fixture.archive, 302, 1, 8, 8000, SetEventKind::MatchStarted);
    started.matchId = 1u;
    started.valueC = 1u;
    CHECK_EQ(fixture.archive.appendEvent(302, started, nullptr),
             ArchiveResult::Ok);
    for (uint32_t index = 8; index < 24; ++index) {
        CHECK_EQ(fixture.archive.appendConfirmed(
                     302, recordFor(index, 8000), nullptr),
                 ArchiveResult::Ok);
    }

    fixture.pump(60);
    CHECK_EQ(fixture.viewer->receivedNextRecord(), 24u);
    CHECK_EQ(fixture.viewer->receivedNextEvent(), 2u);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);

    uint64_t sourceChain = 0;
    CHECK_TRUE(fixture.archive.chainDigestAt(302, 24u, 2u, sourceChain));
    CHECK_EQ(fixture.viewer->receivedDigest(), sourceChain);

    const SpectatorSetTimeline& timeline = fixture.viewer->setTimeline();
    CHECK_EQ(timeline.eventCount(), size_t{2});
    CHECK_EQ(timeline.matchCount(), 1u);
    SetMatchIndexEntry current{};
    CHECK_TRUE(timeline.currentMatch(
        fixture.viewer->receivedNextRecord(), current));
    CHECK_EQ(current.matchId, 1u);
    CHECK_EQ(current.firstRecord, 8u);
    uint32_t seek = 0;
    CHECK_TRUE(timeline.seekRecordForCurrentMatch(
        fixture.viewer->receivedNextRecord(), seek));
    CHECK_EQ(seek, 8u);
    CHECK_EQ(current.rosterEpoch, 11u);
    CHECK_EQ(current.roster[0].nativeCharacterId, uint8_t{4});
}

void testCheckpointPushSurvivesALostChunk() {
    StreamingFixture fixture;
    fixture.viewerConfig.wantsNewestCheckpoint = true;
    buildStreaming(fixture, 411, 24, 4200, 0x2468ACE013579BDFull);

    uint64_t chainAt12 = 0;
    CHECK_TRUE(fixture.archive.chainDigestAt(411, 12u, 0u, chainAt12));
    Checkpoint capture{};
    capture.identity.sessionId = spectatorIdentity().sessionId;
    capture.identity.gameId = 411u;
    capture.identity.checkpointId = 0xBEEF01u;
    capture.identity.recordCursor = 12u;
    capture.identity.timelineFrame = 4200u + 12u;
    capture.eventCursor = 0u;
    capture.simulationContract = kSimulationContract;
    capture.compatibilityIdentity = compatibilityIdentity();
    capture.bootstrapFingerprint = 0xFEEDFACEull;
    capture.regionSchema = 0x51524231u;
    capture.regionSchemaVersion = 1u;
    capture.digestChainPrefix = chainAt12;
    capture.stateHash = 0xD15EA5Eull;
    capture.chunkBytes = 256u;
    for (uint32_t region = 0; region < 4; ++region) {
        CheckpointRegion entry{};
        entry.id = region + 1u;
        entry.bytes.assign(1200u + region * 200u,
                           static_cast<uint8_t>(0x31u + region));
        entry.byteLength = static_cast<uint32_t>(entry.bytes.size());
        capture.regions.push_back(std::move(entry));
    }
    std::vector<uint8_t> blob;
    CHECK_TRUE(encodeCheckpoint(capture, blob));
    ArchiveCheckpointRef ref{};
    ref.checkpointId = capture.identity.checkpointId;
    ref.recordCursor = 12u;
    ref.digest = 0x5EEDu;
    CHECK_EQ(fixture.archive.addCheckpoint(411, ref), ArchiveResult::Ok);
    CHECK_EQ(fixture.archive.setNewestCheckpointBlob(
                 411, ref.checkpointId, blob.data(), blob.size()),
             ArchiveResult::Ok);

    const SessionIdentity identity = spectatorIdentity();
    uint32_t droppedIndex = 0xFFFFFFFFu;
    size_t chunksSeenOnWire = 0;
    size_t chunksDelivered = 0;
    for (unsigned step = 0; step < 600; ++step) {
        fixture.now += 30;
        for (const auto& packet : fixture.viewer->pollOutbound(fixture.now, 8))
        {
            fixture.source->onDatagram(fixture.endpoint, packet.bytes.data(),
                                       packet.bytes.size(), fixture.now);
        }
        for (const auto& packet :
             fixture.source->pollOutbound(fixture.now, 16)) {
            if (packet.destination != fixture.endpoint) continue;
            Datagram decoded{};
            if (decodeDatagram(packet.bytes.data(), packet.bytes.size(),
                               identity, decoded) == DecodeError::None
                && decoded.header.type == MessageType::CheckpointChunk) {
                CheckpointChunkPayload chunk{};
                if (decodePayload(decoded.payload, chunk)) {
                    ++chunksSeenOnWire;
                    if (droppedIndex == 0xFFFFFFFFu && chunk.chunkIndex == 1u
                        && chunk.chunkCount > 2u) {
                        droppedIndex = chunk.chunkIndex;
                        continue;
                    }
                }
            }
            ++chunksDelivered;
            fixture.viewer->onDatagram(packet.bytes.data(),
                                       packet.bytes.size(), fixture.now);
        }
    }
    (void)chunksDelivered;
    CHECK_EQ(droppedIndex, 1u);
    CHECK_TRUE(chunksSeenOnWire > 2);

    Checkpoint assembled{};
    CHECK_TRUE(fixture.viewer->takeAssembledCheckpoint(assembled));
    CHECK_EQ(assembled.identity.checkpointId, capture.identity.checkpointId);
    CHECK_EQ(assembled.identity.recordCursor, 12u);
    CHECK_EQ(assembled.regions.size(), size_t{4});
    if (assembled.regions.size() == 4u) {
        CHECK_EQ(assembled.regions[0].bytes, capture.regions[0].bytes);
        CHECK_EQ(assembled.regions[3].bytes, capture.regions[3].bytes);
    }

    SourceViewerSnapshot snapshot{};
    CHECK_TRUE(fixture.source->viewerSnapshot(fixture.identity, snapshot));
    CHECK_TRUE(snapshot.checkpointPassesSent < kCheckpointPushPasses);
}

void testCheckpointPushRepeatIsBounded() {
    CHECK_EQ(kCheckpointPushPasses, 6u);
    CHECK_EQ(kCheckpointPushRepassMs, uint64_t{500});
    CHECK_TRUE(kCheckpointPushPasses * kCheckpointPushRepassMs < 15000u);
}

void testCheckpointSkipRebindsViewerAtItsProvenCursor() {
    StreamingFixture fixture;
    fixture.viewerConfig.wantsNewestCheckpoint = true;
    buildStreaming(fixture, 305, 8, 9500, 0x1122334455667788ull);
    SetEvent started = setEventAt(
        fixture.archive, 305, 0, 8, 9500, SetEventKind::MatchStarted);
    started.matchId = 1u;
    started.valueC = 1u;
    CHECK_EQ(fixture.archive.appendEvent(305, started, nullptr),
             ArchiveResult::Ok);
    for (uint32_t index = 8; index < 40; ++index) {
        CHECK_EQ(fixture.archive.appendConfirmed(
                     305, recordFor(index, 9500), nullptr),
                 ArchiveResult::Ok);
    }

    uint64_t chainAt20 = 0;
    CHECK_TRUE(fixture.archive.chainDigestAt(305, 20u, 1u, chainAt20));
    Checkpoint capture{};
    capture.identity.sessionId = spectatorIdentity().sessionId;
    capture.identity.gameId = 305u;
    capture.identity.checkpointId = 0xC0FFEEu;
    capture.identity.recordCursor = 20u;
    capture.identity.timelineFrame = 9500u + 20u;
    capture.eventCursor = 1u;
    capture.simulationContract = kSimulationContract;
    capture.compatibilityIdentity = compatibilityIdentity();
    capture.bootstrapFingerprint = 0xFEEDFACEull;
    capture.regionSchema = 0x51524231u;
    capture.regionSchemaVersion = 1u;
    capture.digestChainPrefix = chainAt20;
    capture.stateHash = 0xD15EA5Eull;
    capture.chunkBytes = 256u;
    for (uint32_t region = 0; region < 3; ++region) {
        CheckpointRegion entry{};
        entry.id = region + 1u;
        entry.bytes.assign(300u + region * 40u,
                           static_cast<uint8_t>(0x21u + region));
        entry.byteLength = static_cast<uint32_t>(entry.bytes.size());
        capture.regions.push_back(std::move(entry));
    }
    std::vector<uint8_t> blob;
    CHECK_TRUE(encodeCheckpoint(capture, blob));
    ArchiveCheckpointRef ref{};
    ref.checkpointId = capture.identity.checkpointId;
    ref.recordCursor = 20u;
    ref.digest = 0x5EEDu;
    CHECK_EQ(fixture.archive.addCheckpoint(305, ref), ArchiveResult::Ok);
    CHECK_EQ(fixture.archive.setNewestCheckpointBlob(
                 305, ref.checkpointId, blob.data(), blob.size()),
             ArchiveResult::Ok);

    fixture.pump(400);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);

    Checkpoint assembled{};
    CHECK_TRUE(fixture.viewer->takeAssembledCheckpoint(assembled));
    CHECK_EQ(assembled.identity.checkpointId, capture.identity.checkpointId);
    CHECK_EQ(assembled.identity.recordCursor, 20u);
    CHECK_TRUE(assembled.eventCursorPresent);
    CHECK_EQ(assembled.eventCursor, 1u);
    CHECK_EQ(assembled.digestChainPrefix, chainAt20);
    CHECK_EQ(assembled.regions.size(), size_t{3});
    CHECK_EQ(assembled.regions[2].bytes, capture.regions[2].bytes);

    SpectatorSetTimeline::BindSeed seed{};
    seed.openMatchId = 1u;
    seed.highestMatchId = 1u;
    seed.matchesSoFar = 1u;
    CHECK_TRUE(fixture.viewer->resumeAtCheckpoint(
        20u, 1u, assembled.digestChainPrefix, seed));
    fixture.pump(200);
    CHECK_EQ(fixture.viewer->phase(), ViewerPhase::Streaming);
    CHECK_EQ(fixture.viewer->playbackNextRecord(), 20u);
    CHECK_EQ(fixture.viewer->receivedNextRecord(), 40u);

    ConfirmedRecord head{};
    CHECK_TRUE(fixture.viewer->peekPlaybackRecord(head));
    CHECK_EQ(head.record, 20u);
    ConfirmedRecord below{};
    CHECK_TRUE(!fixture.viewer->verifiedRecordAt(5u, below));

    uint64_t sourceChain = 0;
    CHECK_TRUE(fixture.archive.chainDigestAt(305, 40u, 1u, sourceChain));
    CHECK_EQ(fixture.viewer->receivedDigest(), sourceChain);

    SetEvent ended = setEventAt(
        fixture.archive, 305, 1, 40, 9500, SetEventKind::MatchResult);
    ended.matchId = 1u;
    CHECK_EQ(fixture.archive.appendEvent(305, ended, nullptr),
             ArchiveResult::Ok);
    SetEvent second = setEventAt(
        fixture.archive, 305, 2, 40, 9500, SetEventKind::MatchStarted);
    second.matchId = 2u;
    second.valueC = 2u;
    CHECK_EQ(fixture.archive.appendEvent(305, second, nullptr),
             ArchiveResult::Ok);
    fixture.pump(120);
    CHECK_EQ(fixture.viewer->receivedNextEvent(), 3u);
    const SpectatorSetTimeline& timeline = fixture.viewer->setTimeline();
    SetEvent got{};
    CHECK_TRUE(timeline.event(2u, got));
    CHECK_EQ(static_cast<uint16_t>(got.kind),
             static_cast<uint16_t>(SetEventKind::MatchStarted));
    CHECK_EQ(got.matchId, 2u);
    SetEvent elided{};
    CHECK_TRUE(!timeline.event(0u, elided));
}

void testRelayIngestExportsBoundedVerifiedSuffixWithoutPlayback() {
    const uint64_t gameId = 303u;
    const uint32_t firstFrame = 9100u;
    SpectatorArchive archive;
    CHECK_EQ(archive.beginGame(descriptor(gameId, firstFrame)),
             ArchiveResult::Ok);
    for (uint32_t record = 0; record < 2; ++record) {
        CHECK_EQ(archive.appendConfirmed(
                     gameId, recordFor(record, firstFrame), nullptr),
                 ArchiveResult::Ok);
    }

    SetEvent roster = rosterEventAt(
        archive, gameId, 0u, 2u, firstFrame, 9u);
    CHECK_EQ(archive.appendEvent(gameId, roster, nullptr), ArchiveResult::Ok);
    SetEvent stage = setEventAt(
        archive, gameId, 1u, 2u, firstFrame, SetEventKind::StageSelected);
    stage.valueC = 4u;
    CHECK_EQ(archive.appendEvent(gameId, stage, nullptr), ArchiveResult::Ok);
    SetEvent started = setEventAt(
        archive, gameId, 2u, 2u, firstFrame, SetEventKind::MatchStarted);
    started.matchId = 1u;
    started.valueC = 1u;
    CHECK_EQ(archive.appendEvent(gameId, started, nullptr),
             ArchiveResult::Ok);
    for (uint32_t record = 2; record < 6; ++record) {
        CHECK_EQ(archive.appendConfirmed(
                     gameId, recordFor(record, firstFrame), nullptr),
                 ArchiveResult::Ok);
    }

    ViewerConfig config{};
    config.identity = spectatorIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = compatibilityIdentity();
    config.secureRandom = 0x3033033033033033ull;
    config.relayIngestOnly = true;
    config.maxRelayRetainedRecords = 3u;
    config.maxRelayRetainedEvents = 2u;
    SpectatorViewer viewer(config);
    CHECK_TRUE(viewer.isConfigured());
    CHECK_TRUE(viewer.relayIngestOnly());
    CHECK_TRUE(viewer.begin(gameId));

    ViewerConfig invalid = config;
    invalid.maxRelayRetainedRecords = 0u;
    SpectatorViewer invalidRelay(invalid);
    CHECK_TRUE(!invalidRelay.isConfigured());

    auto deliver = [&](MessageType type,
                       const auto& typedPayload,
                       uint32_t sequence) {
        std::vector<uint8_t> payload;
        std::vector<uint8_t> packet;
        CHECK_TRUE(encodePayload(typedPayload, payload));
        CHECK_TRUE(encodeDatagram(
            type, config.identity, sequence, payload, packet));
        return viewer.onDatagram(packet.data(), packet.size(), sequence);
    };

    const GameStartPayload start = startFor(
        archive, gameId, config.secureRandom);
    CHECK_EQ(deliver(MessageType::GameStart, start, 1u),
             IngressResult::Accepted);

    ConfirmedInputBatchPayload prefix{};
    CHECK_EQ(archive.readBatch(gameId, 0u, 8u, prefix), ArchiveResult::Ok);
    CHECK_EQ(prefix.records.size(), size_t{2});
    CHECK_EQ(deliver(MessageType::ConfirmedInputBatch, prefix, 2u),
             IngressResult::Accepted);

    SetEventBatchPayload events{};
    CHECK_EQ(archive.readEventBatch(gameId, 0u, 8u, events),
             ArchiveResult::Ok);
    CHECK_EQ(events.events.size(), size_t{3});
    CHECK_EQ(deliver(MessageType::SetEvent, events, 3u),
             IngressResult::Accepted);

    ConfirmedInputBatchPayload suffix{};
    CHECK_EQ(archive.readBatch(gameId, 2u, 8u, suffix), ArchiveResult::Ok);
    CHECK_EQ(suffix.records.size(), size_t{4});
    CHECK_EQ(deliver(MessageType::ConfirmedInputBatch, suffix, 4u),
             IngressResult::Accepted);

    VerifiedRelayExportExtent extent{};
    CHECK_TRUE(viewer.verifiedRelayExportExtent(extent));
    CHECK_EQ(extent.firstRecord, 3u);
    CHECK_EQ(extent.nextRecord, 6u);
    CHECK_EQ(extent.firstEvent, 1u);
    CHECK_EQ(extent.nextEvent, 3u);
    ArchiveExtent sourceExtent{};
    CHECK_TRUE(archive.extent(gameId, sourceExtent));
    CHECK_EQ(extent.digest, sourceExtent.digest);

    ConfirmedRecord record{};
    CHECK_TRUE(!viewer.verifiedRecordAt(2u, record));
    CHECK_TRUE(viewer.verifiedRecordAt(3u, record));
    CHECK_EQ(record.record, 3u);
    CHECK_EQ(viewer.playbackNextRecord(), 0u);
    CHECK_EQ(viewer.bufferedPlaybackRecords(), size_t{0});
    CHECK_TRUE(!viewer.peekPlaybackRecord(record));
    CHECK_EQ(viewer.commitPlaybackRecord(
                 0u, recordFor(0u, firstFrame).stateHash,
                 postStateHashFor(0u)),
             IngressResult::UnexpectedMessage);

    SetEvent event{};
    CHECK_TRUE(!viewer.verifiedSetEventAt(0u, event));
    CHECK_TRUE(viewer.verifiedSetEventAt(1u, event));
    CHECK_EQ(event.eventIndex, 1u);
    CHECK_EQ(event.kind, SetEventKind::StageSelected);

    ConfirmedInputBatchPayload retainedReplay{};
    CHECK_EQ(archive.readBatch(
                 gameId, extent.firstRecord, 8u, retainedReplay),
             ArchiveResult::Ok);
    CHECK_EQ(deliver(
                 MessageType::ConfirmedInputBatch, retainedReplay, 5u),
             IngressResult::Accepted);
    CHECK_EQ(viewer.receivedNextRecord(), 6u);

    ConfirmedInputBatchPayload forgedOld = prefix;
    forgedOld.records[0].playerOneInput ^= 0x01u;
    uint64_t digest = forgedOld.baseDigest;
    for (ConfirmedRecord& record : forgedOld.records) {
        record.rollingDigest = advanceRollingDigest(digest, gameId, record);
        digest = record.rollingDigest;
    }
    forgedOld.endDigest = digest;
    CHECK_EQ(deliver(
                 MessageType::ConfirmedInputBatch, forgedOld, 6u),
             IngressResult::ConflictingRecord);
    CHECK_EQ(viewer.phase(), ViewerPhase::Failed);
}

Checkpoint sampleCheckpoint() {
    Checkpoint checkpoint{};
    checkpoint.identity.sessionId = 0x1020304050607080ull;
    checkpoint.identity.gameId = 0x77ull;
    checkpoint.identity.checkpointId = 0x9001ull;
    checkpoint.identity.timelineFrame = 1234u;
    checkpoint.identity.recordCursor = 734u;
    checkpoint.eventCursor = 12u;
    checkpoint.simulationContract = kSimulationContract;
    checkpoint.compatibilityIdentity = compatibilityIdentity();
    checkpoint.bootstrapFingerprint = 0xFEEDFACECAFEBEEFull;
    checkpoint.regionSchema = 0x51434B31u;
    checkpoint.regionSchemaVersion = 3u;
    checkpoint.digestChainPrefix = 0x0123456789ABCDEFull;
    checkpoint.stateHash = 0xD15EA5E0D15EA5E0ull;
    checkpoint.chunkBytes = 256u;
    checkpoint.hostMacLow = 0x1111222233334444ull;
    checkpoint.hostMacHigh = 0x5555666677778888ull;
    for (uint32_t region = 0; region < 3; ++region) {
        CheckpointRegion entry{};
        entry.id = 0x100u + region;
        entry.bytes.resize(64u + region * 37u);
        for (size_t i = 0; i < entry.bytes.size(); ++i) {
            entry.bytes[i] =
                static_cast<uint8_t>((i * 7u + region * 31u) & 0xFFu);
        }
        entry.byteLength = static_cast<uint32_t>(entry.bytes.size());
        checkpoint.regions.push_back(std::move(entry));
    }
    return checkpoint;
}

void testCheckpointCodecRoundTripAndBounds() {
    const Checkpoint original = sampleCheckpoint();
    std::vector<uint8_t> encoded;
    CHECK_TRUE(encodeCheckpoint(original, encoded));
    CHECK_EQ(encoded.size(), checkpointEncodedSize(original));

    Checkpoint decoded{};
    CHECK_EQ(decodeCheckpoint(encoded.data(), encoded.size(), decoded),
             CheckpointError::None);
    CHECK_EQ(decoded.identity.checkpointId, 0x9001ull);
    CHECK_EQ(decoded.identity.recordCursor, 734u);
    CHECK_EQ(decoded.digestChainPrefix, 0x0123456789ABCDEFull);
    CHECK_EQ(decoded.hostMacHigh, 0x5555666677778888ull);
    CHECK_EQ(decoded.regions.size(), size_t{3});
    CHECK_EQ(decoded.regions[2].bytes, original.regions[2].bytes);
    CHECK_TRUE(decoded.contentIdentityAttested);
    CHECK_TRUE(compat::semanticallyEqual(
        decoded.compatibilityIdentity, original.compatibilityIdentity));
    CHECK_TRUE(checkpointMatchesCompatibilityIdentity(
        decoded, original.compatibilityIdentity));
    CHECK_TRUE(decoded.eventCursorPresent);
    CHECK_EQ(decoded.eventCursor, 12u);
    CHECK_EQ(decoded.digest, checkpointDigestV3(decoded));
    Checkpoint movedEvent = decoded;
    movedEvent.eventCursor ^= 1u;
    CHECK_TRUE(checkpointDigestV3(movedEvent) != decoded.digest);

    Checkpoint differentContent = original;
    differentContent.compatibilityIdentity = compatibilityIdentity(0xB5u);
    CHECK_TRUE(checkpointDigest(differentContent)
               != checkpointDigest(original));
    CHECK_TRUE(!checkpointMatchesCompatibilityIdentity(
        decoded, differentContent.compatibilityIdentity));

    Checkpoint timestampOnly = original;
    timestampOnly.compatibilityIdentity.runtimeBuild.peTimestamp ^= 0xFFFFFFFFu;
    CHECK_EQ(checkpointDigest(timestampOnly), checkpointDigest(original));
    CHECK_TRUE(checkpointMatchesCompatibilityIdentity(
        decoded, timestampOnly.compatibilityIdentity));

    std::vector<uint8_t> again;
    CHECK_TRUE(encodeCheckpoint(decoded, again));
    CHECK_TRUE(again == encoded);

    for (size_t length = 0; length < encoded.size(); ++length) {
        Checkpoint untouched{};
        untouched.identity.checkpointId = 0xDEADBEEFull;
        CHECK_TRUE(decodeCheckpoint(encoded.data(), length, untouched)
                   != CheckpointError::None);
        CHECK_EQ(untouched.identity.checkpointId, 0xDEADBEEFull);
    }
    std::vector<uint8_t> extended = encoded;
    extended.push_back(0);
    Checkpoint untouched{};
    untouched.identity.checkpointId = 0xDEADBEEFull;
    CHECK_EQ(decodeCheckpoint(extended.data(), extended.size(), untouched),
             CheckpointError::Truncated);
    CHECK_EQ(untouched.identity.checkpointId, 0xDEADBEEFull);

    std::vector<uint8_t> badCrc = encoded;
    badCrc.back() ^= 0x40u;
    CHECK_EQ(decodeCheckpoint(badCrc.data(), badCrc.size(), untouched),
             CheckpointError::BadCrc);

    std::vector<uint8_t> badRegion = encoded;
    const size_t bodyOffset =
        kCheckpointHeaderBytes + 3u * kCheckpointRegionEntryBytes;
    badRegion[bodyOffset + 5u] ^= 0x80u;
    CHECK_EQ(decodeCheckpoint(badRegion.data(), badRegion.size(), untouched),
             CheckpointError::BadCrc);

    std::vector<uint8_t> tooManyRegions = encoded;
    writeLe32(tooManyRegions, 88u, 0x40000000u);
    CHECK_EQ(decodeCheckpoint(
                 tooManyRegions.data(), tooManyRegions.size(), untouched),
             CheckpointError::RegionLimit);

    std::vector<uint8_t> hugeRegion = encoded;
    writeLe32(hugeRegion, kCheckpointHeaderBytesV3 + 4u,
                   9u * 1024u * 1024u);
    CHECK_EQ(decodeCheckpoint(
                 hugeRegion.data(), hugeRegion.size(), untouched),
             CheckpointError::RegionTooLarge);

    std::vector<uint8_t> bomb = encoded;
    writeLe32(bomb, 100u, 0x7FFFFFFFu);
    CHECK_EQ(decodeCheckpoint(bomb.data(), bomb.size(), untouched),
             CheckpointError::TotalTooLarge);
    std::vector<uint8_t> smallerBomb = encoded;
    writeLe32(smallerBomb, 100u, 0x00FF0000u);
    CHECK_EQ(decodeCheckpoint(
                 smallerBomb.data(), smallerBomb.size(), untouched),
             CheckpointError::Truncated);

    std::vector<uint8_t> badCompression = encoded;
    badCompression[96u] = 9u;
    CHECK_EQ(decodeCheckpoint(
                 badCompression.data(), badCompression.size(), untouched),
             CheckpointError::BadCompression);

    Checkpoint oversize = original;
    CheckpointRegion big{};
    big.id = 0x999u;
    big.byteLength = static_cast<uint32_t>(kMaxCheckpointRegionBytes + 1u);
    CHECK_EQ(checkpointEncodedSize(oversize), encoded.size());
    oversize.regions.push_back(big);
    CHECK_EQ(checkpointEncodedSize(oversize), size_t{0});
    CHECK_TRUE(!encodeCheckpoint(oversize, encoded));
}

void testLegacyCheckpointRemainsExplicitlyUnattested() {
    const Checkpoint original = sampleCheckpoint();
    std::vector<uint8_t> v2;
    CHECK_TRUE(encodeCheckpoint(original, v2));

    Checkpoint canonical{};
    CHECK_EQ(decodeCheckpoint(v2.data(), v2.size(), canonical),
             CheckpointError::None);
    const size_t v2Trailer = v2.size() - kCheckpointTrailerBytes;
    const size_t tableAndBody = v2Trailer - kCheckpointHeaderBytesV3;
    std::vector<uint8_t> legacy(
        kLegacyCheckpointHeaderBytes + tableAndBody
        + kCheckpointTrailerBytes,
        0u);
    std::copy_n(v2.begin(), kLegacyCheckpointHeaderBytes, legacy.begin());
    std::copy(v2.begin() + static_cast<std::ptrdiff_t>(
                  kCheckpointHeaderBytesV3),
              v2.begin() + static_cast<std::ptrdiff_t>(v2Trailer),
              legacy.begin() + static_cast<std::ptrdiff_t>(
                  kLegacyCheckpointHeaderBytes));
    writeLe32(legacy, 4u, kLegacyCheckpointVersion);
    writeLe32(legacy, 8u,
              static_cast<uint32_t>(kLegacyCheckpointHeaderBytes));

    const size_t trailer = legacy.size() - kCheckpointTrailerBytes;
    writeLe64(legacy, trailer, checkpointDigestV1ForTest(canonical));
    writeLe64(legacy, trailer + 8u, canonical.hostMacLow);
    writeLe64(legacy, trailer + 16u, canonical.hostMacHigh);
    writeLe32(legacy, trailer + 24u,
              checkpointCrcForTest(legacy.data(), trailer + 24u));

    Checkpoint decoded{};
    CHECK_EQ(decodeCheckpoint(legacy.data(), legacy.size(), decoded),
             CheckpointError::None);
    CHECK_TRUE(!decoded.contentIdentityAttested);
    CHECK_TRUE(!compat::isValidCompatibilityIdentity(
        decoded.compatibilityIdentity));
    CHECK_TRUE(!checkpointMatchesCompatibilityIdentity(
        decoded, original.compatibilityIdentity));
    CHECK_EQ(decoded.identity.checkpointId, original.identity.checkpointId);
    CHECK_EQ(decoded.regions, canonical.regions);

    std::vector<uint8_t> rejected;
    CHECK_TRUE(!encodeCheckpoint(decoded, rejected));
}

void testCheckpointChunkingAndReassembly() {
    const Checkpoint original = sampleCheckpoint();
    std::vector<uint8_t> encoded;
    CHECK_TRUE(encodeCheckpoint(original, encoded));

    CheckpointChunkPlan plan{};
    CHECK_TRUE(planCheckpointChunks(encoded.size(), 128u, plan));
    CHECK_EQ(plan.totalBytes, static_cast<uint32_t>(encoded.size()));
    CHECK_EQ(plan.chunkBytes, 128u);
    CHECK_TRUE(plan.chunkCount * plan.chunkBytes >= plan.totalBytes);
    CheckpointChunkPlan rejected{};
    CHECK_TRUE(!planCheckpointChunks(encoded.size(), 0u, rejected));
    CHECK_TRUE(!planCheckpointChunks(0u, 128u, rejected));

    CheckpointReassembler reassembler;
    CHECK_TRUE(reassembler.begin(original.identity.checkpointId, plan));
    CHECK_EQ(reassembler.acceptChunk(
                 original.identity.checkpointId, 1u, 999u,
                 encoded.data(), 4u),
             CheckpointError::InvalidArgument);
    for (uint32_t index = plan.chunkCount; index > 0; --index) {
        const uint32_t chunk = index - 1u;
        const uint32_t offset = chunk * plan.chunkBytes;
        const uint32_t length = std::min<uint32_t>(
            plan.chunkBytes, plan.totalBytes - offset);
        CHECK_EQ(reassembler.acceptChunk(
                     original.identity.checkpointId, chunk, offset,
                     encoded.data() + offset, length),
                 CheckpointError::None);
        if (chunk == 2u) {
            CHECK_EQ(reassembler.acceptChunk(
                         original.identity.checkpointId, chunk, offset,
                         encoded.data() + offset, length),
                     CheckpointError::None);
        }
    }
    CHECK_TRUE(reassembler.complete());
    std::vector<uint8_t> rebuilt;
    CHECK_TRUE(reassembler.finish(rebuilt));
    CHECK_TRUE(rebuilt == encoded);
    Checkpoint decoded{};
    CHECK_EQ(decodeCheckpoint(rebuilt.data(), rebuilt.size(), decoded),
             CheckpointError::None);
    CHECK_EQ(decoded.identity.recordCursor, 734u);
}

}

int main() {
    testReadyMetadataV5RoundTripAndStrictFields();
    testProtocolStrictScoping();
    testLegacyDirectV3IsExplicitlyUnattested();
    testSpectatorContentIdentityRejectsBeforeStreamMutation();
    testAtomicArchiveAndConnectedSetRetention();
    testLossReorderLateJoinAndTerminal();
    testIdleBeforeFirstPublicationDoesNotExpireRepairTimer();
    testDuplicateAcksFastRepairAndLostRepairRto();
    testLiveRecordRepairPreemptsFreshPublication();
    testTerminalArrivingAfterPlaybackVerifiesRememberedPostState();
    testAcceptedTerminalSurvivesOfflineCatchup();
    testExplicitRouteLossIsTypedAndTerminal();
    testTerminalCursorIsImmutable();
    testResumeRejectsDelayedOldHandshakeWithoutFailing();
    testDelayedCurrentGameStartAfterStreamingProgress();
    testLatestViewerWaitsForArchiveWithoutTerminalReject();
    testSpectatorDatagramsCannotMutatePlayerCore();
    testSequenceWindowReplayAndWrap();
    testValidatedParentRouteRehandshakeStaysSameGeneration();
    testDelayedOldHelloCannotRewindNewerGeneration();
    testAuthenticatedMigrationAndSpoofRejection();
    testExplicitDetachReleasesCapacityImmediately();
    testPreGameViewerCanEmitCapabilityScopedDetach();
    testSeedlessInitialDetachRetiresUnacknowledgedSourceSlot();
    testSeedlessDetachCannotRetireStreamingSourceSlot();
    testStalePinningHitsBoundedPhaseDeadlines();
    testSourceHeartbeatKeepsPausedViewerAlive();
    testPerViewerLagCapsAndSlowViewerIsolation();
    testArchiveSegmentRotationAndPinning();
    testArchiveHardByteCeiling();
    testArchiveAllocationFailureDisablesServiceWithoutThrowing();
    testLiveEventRepairPreemptsFreshPublication();
    testSetEventOrderingDigestBindingAndIndex();
    testPaletteEventBindsToTheOpenMatch();
    testSetEventChainRejectsDropReorderAndInsertion();
    testSetEventsReachViewerAndBindPlaybackChain();
    testCheckpointPushSurvivesALostChunk();
    testCheckpointPushRepeatIsBounded();
    testCheckpointSkipRebindsViewerAtItsProvenCursor();
    testRelayIngestExportsBoundedVerifiedSuffixWithoutPlayback();
    testCheckpointCodecRoundTripAndBounds();
    testLegacyCheckpointRemainsExplicitlyUnattested();
    testCheckpointChunkingAndReassembly();
    testEventStarvedViewerRecoversWhenItsUnverifiedAckArmsAnEventRepair();
    testPreflightSequenceHandoffKeepsRuntimeHelloAdmissible();
    if (g_failures != 0) {
        std::fprintf(stderr, "spectator tests FAILED: %d\n", g_failures);
        return 1;
    }
    std::puts("spectator tests passed");
    return 0;
}
