#include "SpectatorProtocol.h"
#include "../game/SessionPresentation.h"
#include "../net/FrameArithmetic.h"
#include "input/InputActions.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace qnetplay::spectator {
namespace {

constexpr size_t kMagicOffset = 0;
constexpr size_t kVersionOffset = 4;
constexpr size_t kTypeOffset = 6;
constexpr size_t kDirectionOffset = 7;
constexpr size_t kRoleOffset = 8;
constexpr size_t kFlagsOffset = 9;
constexpr size_t kHeaderLengthOffset = 10;
constexpr size_t kPayloadLengthOffset = 12;
constexpr size_t kReservedOffset = 14;
constexpr size_t kSessionOffset = 16;
constexpr size_t kEpochOffset = 24;
constexpr size_t kCookieLowOffset = 32;
constexpr size_t kCookieHighOffset = 40;
constexpr size_t kSequenceOffset = 48;
constexpr size_t kCrcOffset = 52;

constexpr size_t kLegacyHelloPayloadSize = 56;
constexpr size_t kHelloPayloadSize =
    kLegacyHelloPayloadSize + compat::kCompatibilityIdentityWireBytes;
constexpr size_t kLegacyGameStartFixedSize = 88;
constexpr size_t kGameStartFixedSize =
    kLegacyGameStartFixedSize + compat::kCompatibilityIdentityWireBytes;
constexpr size_t kInitAckPayloadSize = 56;
constexpr size_t kBatchFixedSize = 40;
constexpr size_t kRecordWireSize = 28;
constexpr size_t kAckPayloadSize = 40;
constexpr size_t kTerminalPayloadSize = 40;
constexpr size_t kTerminalAckPayloadSize = 56;
constexpr size_t kRejectPayloadSize = 48;
constexpr size_t kDetachPayloadSize = 32;
constexpr size_t kMigratePayloadSize = 56;
constexpr size_t kHeartbeatPayloadSize = 40;
constexpr size_t kLegacyCheckpointOfferPayloadSize = 40;
constexpr size_t kCheckpointOfferPayloadSize =
    kLegacyCheckpointOfferPayloadSize
    + compat::kCompatibilityIdentityWireBytes;

constexpr size_t kReadyVsRowOffset = 64;
constexpr size_t kReadyCpuMaskOffset = 68;
constexpr size_t kReadyRouteOffset = 72;
constexpr size_t kReadyActiveLaneMaskOffset = 76;
constexpr size_t kReadyActiveSlotMaskOffset = 77;
constexpr size_t kReadyRosterEpochLowOffset = 78;
constexpr size_t kReadyReservedByteOffset = 79;
constexpr size_t kReadyCodePageOffset = 80;
constexpr size_t kReadyNameOffset = 96;

uint16_t readLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0])
         | static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readLe64(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(p[i]) << (i * 8);
    return value;
}

void writeLe16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void writeLe32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = static_cast<uint8_t>(value >> (i * 8));
}

void writeLe64(uint8_t* p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>(value >> (i * 8));
}

template <typename Byte>
bool isCanonicalReadyPlayerName(const Byte* name, uint32_t codePage) {
    bool terminated = false;
    size_t length = 0;
    for (size_t i = 0; i < kReadyPlayerNameBytes; ++i) {
        const uint8_t byte = static_cast<uint8_t>(name[i]);
        if (terminated) {
            if (byte != 0) return false;
            continue;
        }
        if (byte == 0) {
            terminated = true;
            continue;
        }
        ++length;
    }
    return terminated
        && qgame::isCanonicalPlayerNameForCodePage(
            std::string_view(
                reinterpret_cast<const char*>(name), length),
            codePage);
}

template <typename Byte>
bool isAbsentReadyPlayerName(const Byte* name, uint32_t codePage) {
    if (codePage != 0) return false;
    for (size_t i = 0; i < kReadyPlayerNameBytes; ++i) {
        if (static_cast<uint8_t>(name[i]) != 0) return false;
    }
    return true;
}

bool validTerminalReason(TerminalReason reason) {
    switch (reason) {
        case TerminalReason::MatchEnded:
        case TerminalReason::SourceStopped:
        case TerminalReason::Superseded:
        case TerminalReason::ProtocolFailure:
            return true;
    }
    return false;
}

bool validRejectReason(RejectReason reason) {
    switch (reason) {
        case RejectReason::GameUnavailable:
        case RejectReason::ResumeMismatch:
        case RejectReason::ContractMismatch:
        case RejectReason::CapacityReached:
        case RejectReason::InvalidRequest:
        case RejectReason::ArchiveUnavailable:
        case RejectReason::GrantInvalid:
        case RejectReason::GrantExpired:
        case RejectReason::MigrationRejected:
        case RejectReason::RateLimited:
            return true;
    }
    return false;
}

uint32_t crcUpdate(uint32_t crc, const uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint64_t avalanche(uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

void mixByte(uint64_t& hash, uint8_t value) {
    hash ^= value;
    hash *= 0x100000001B3ull;
}

void mix16(uint64_t& hash, uint16_t value) {
    for (unsigned i = 0; i < 2; ++i)
        mixByte(hash, static_cast<uint8_t>(value >> (i * 8)));
}

void mix32(uint64_t& hash, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        mixByte(hash, static_cast<uint8_t>(value >> (i * 8)));
}

void mix64(uint64_t& hash, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        mixByte(hash, static_cast<uint8_t>(value >> (i * 8)));
}

void writeCompatibilityIdentity(
    uint8_t* const output,
    const compat::CompatibilityIdentityWire& identity) {
    writeLe32(output, identity.schemaVersion);
    writeLe32(output + 4, identity.protocolVersion);
    std::copy_n(reinterpret_cast<const uint8_t*>(
                    identity.runtimeBuild.profileId),
                compat::kCompatibilityProfileIdBytes, output + 8);
    std::copy_n(identity.runtimeBuild.executableSha256,
                compat::kCompatibilitySha256Bytes, output + 40);
    writeLe64(output + 72, identity.runtimeBuild.executableSize);
    writeLe32(output + 80, identity.runtimeBuild.peTimestamp);
    writeLe32(output + 84,
              identity.simulationContent.manifestSchemaVersion);
    writeLe32(output + 88, identity.simulationContent.manifestFlags);
    std::copy_n(identity.simulationContent.simulationRootSha256,
                compat::kCompatibilitySha256Bytes, output + 92);
    std::copy_n(identity.simulationContent.validationRootSha256,
                compat::kCompatibilitySha256Bytes, output + 124);
    writeLe32(output + 156, identity.simulationContent.entryCount);
    writeLe32(output + 160,
              identity.simulationContent.requiredEntryCount);
    writeLe64(output + 164, identity.stateSchemaId);
    writeLe64(output + 172, identity.modSimulationId);
    writeLe64(output + 180, identity.timingProfileId);
    writeLe32(output + 188, identity.reserved);
}

bool readCompatibilityIdentity(
    const uint8_t* const input,
    compat::CompatibilityIdentityWire& output) {
    compat::CompatibilityIdentityWire decoded{};
    decoded.schemaVersion = readLe32(input);
    decoded.protocolVersion = readLe32(input + 4);
    std::copy_n(input + 8, compat::kCompatibilityProfileIdBytes,
                reinterpret_cast<uint8_t*>(decoded.runtimeBuild.profileId));
    std::copy_n(input + 40, compat::kCompatibilitySha256Bytes,
                decoded.runtimeBuild.executableSha256);
    decoded.runtimeBuild.executableSize = readLe64(input + 72);
    decoded.runtimeBuild.peTimestamp = readLe32(input + 80);
    decoded.simulationContent.manifestSchemaVersion = readLe32(input + 84);
    decoded.simulationContent.manifestFlags = readLe32(input + 88);
    std::copy_n(input + 92, compat::kCompatibilitySha256Bytes,
                decoded.simulationContent.simulationRootSha256);
    std::copy_n(input + 124, compat::kCompatibilitySha256Bytes,
                decoded.simulationContent.validationRootSha256);
    decoded.simulationContent.entryCount = readLe32(input + 156);
    decoded.simulationContent.requiredEntryCount = readLe32(input + 160);
    decoded.stateSchemaId = readLe64(input + 164);
    decoded.modSimulationId = readLe64(input + 172);
    decoded.timingProfileId = readLe64(input + 180);
    decoded.reserved = readLe32(input + 188);
    if (!compat::isValidCompatibilityIdentity(decoded)) return false;
    output = decoded;
    return true;
}

void mixCompatibilityIdentity(
    uint64_t& hash,
    const compat::CompatibilityIdentityWire& identity) {
    mix32(hash, identity.schemaVersion);
    mix32(hash, identity.protocolVersion);
    for (size_t i = 0; i < compat::kCompatibilityProfileIdBytes; ++i) {
        mixByte(hash, static_cast<uint8_t>(identity.runtimeBuild.profileId[i]));
    }
    for (uint8_t byte : identity.runtimeBuild.executableSha256)
        mixByte(hash, byte);
    mix64(hash, identity.runtimeBuild.executableSize);
    mix32(hash, identity.simulationContent.manifestSchemaVersion);
    mix32(hash, identity.simulationContent.manifestFlags);
    for (uint8_t byte : identity.simulationContent.simulationRootSha256)
        mixByte(hash, byte);
    for (uint8_t byte : identity.simulationContent.validationRootSha256)
        mixByte(hash, byte);
    mix32(hash, identity.simulationContent.entryCount);
    mix32(hash, identity.simulationContent.requiredEntryCount);
    mix64(hash, identity.stateSchemaId);
    mix64(hash, identity.modSimulationId);
    mix64(hash, identity.timingProfileId);
    mix32(hash, identity.reserved);
}

bool validPayloadShape(MessageType type, const std::vector<uint8_t>& payload) {
    switch (type) {
        case MessageType::Hello: {
            HelloPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::GameStart: {
            GameStartPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::InitAck: {
            InitAckPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::ConfirmedInputBatch: {
            ConfirmedInputBatchPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::Ack: {
            AckPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::Terminal: {
            TerminalPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::TerminalAck: {
            TerminalAckPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::Reject: {
            RejectPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::Detach: {
            DetachPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::Migrate: {
            MigratePayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::Heartbeat: {
            HeartbeatPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::CheckpointOffer: {
            CheckpointOfferPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::CheckpointChunk: {
            CheckpointChunkPayload decoded{};
            return decodePayload(payload, decoded);
        }
        case MessageType::SetEvent: {
            SetEventBatchPayload decoded{};
            return decodePayload(payload, decoded);
        }
    }
    return false;
}

bool validStartScalars(const GameStartPayload& payload) {
    if (payload.gameId == kLatestGameId
        || payload.simulationContract == 0
        || !compat::isValidCompatibilityIdentity(payload.compatibilityIdentity)
        || payload.viewerNonce == 0
        || payload.viewerId == 0
        || payload.metadata.size() > kMaxGameMetadataSize
        || payload.playerOneDelay > 120
        || payload.playerTwoDelay > 120
        || payload.rollbackWindow > 120) {
        return false;
    }
    return true;
}

bool validLegacyStartScalars(const GameStartPayload& payload) {
    return payload.gameId != kLatestGameId
        && payload.simulationContract != 0
        && payload.viewerNonce != 0
        && payload.viewerId != 0
        && payload.metadata.size() <= kMaxGameMetadataSize
        && payload.playerOneDelay <= 120
        && payload.playerTwoDelay <= 120
        && payload.rollbackWindow <= 120;
}

bool validLegacyPayloadShape(MessageType type,
                             const std::vector<uint8_t>& payload) {
    switch (type) {
        case MessageType::Hello:
            return payload.size() == kLegacyHelloPayloadSize
                && readLe32(payload.data() + 52) == 0
                && readLe64(payload.data()) != 0
                && readLe64(payload.data() + 16) != 0
                && readLe64(payload.data() + 24) != 0;

        case MessageType::GameStart: {
            if (payload.size() < kLegacyGameStartFixedSize
                || payload.size()
                    > kLegacyGameStartFixedSize + kMaxGameMetadataSize
                || readLe64(payload.data() + 80) != 0) {
                return false;
            }
            const uint16_t metadataLength = readLe16(payload.data() + 78);
            if (payload.size()
                != kLegacyGameStartFixedSize + metadataLength) {
                return false;
            }
            GameStartPayload decoded{};
            decoded.gameId = readLe64(payload.data());
            decoded.simulationContract = readLe64(payload.data() + 8);
            decoded.initialStateHash = readLe64(payload.data() + 16);
            decoded.initialDigest = readLe64(payload.data() + 24);
            decoded.resumeDigest = readLe64(payload.data() + 32);
            decoded.viewerNonce = readLe64(payload.data() + 40);
            decoded.viewerId = readLe64(payload.data() + 48);
            decoded.resumeRecord = readLe32(payload.data() + 56);
            decoded.firstTimelineFrame = readLe32(payload.data() + 60);
            decoded.generation = readLe32(payload.data() + 64);
            decoded.resumeEvent = readLe32(payload.data() + 68);
            decoded.playerOneDelay = readLe16(payload.data() + 72);
            decoded.playerTwoDelay = readLe16(payload.data() + 74);
            decoded.rollbackWindow = readLe16(payload.data() + 76);
            decoded.metadata.assign(
                payload.begin() + static_cast<std::ptrdiff_t>(
                    kLegacyGameStartFixedSize),
                payload.end());
            return validLegacyStartScalars(decoded);
        }

        case MessageType::CheckpointOffer: {
            if (payload.size() != kLegacyCheckpointOfferPayloadSize
                || readLe64(payload.data()) == kLatestGameId
                || readLe64(payload.data() + 8) == 0
                || readLe32(payload.data() + 28) == 0
                || readLe32(payload.data() + 32) == 0
                || readLe32(payload.data() + 36) == 0
                || readLe32(payload.data() + 36)
                    > kMaxCheckpointChunkBytes) {
                return false;
            }
            const uint64_t covered =
                static_cast<uint64_t>(readLe32(payload.data() + 32))
                * static_cast<uint64_t>(readLe32(payload.data() + 36));
            const uint32_t totalBytes = readLe32(payload.data() + 28);
            const uint32_t chunkBytes = readLe32(payload.data() + 36);
            return covered >= totalBytes
                && covered - totalBytes < chunkBytes;
        }

        default:
            return validPayloadShape(type, payload);
    }
}

}

bool isKnownSourcePhase(SourcePhase phase) {
    switch (phase) {
        case SourcePhase::Waiting:
        case SourcePhase::Active:
        case SourcePhase::Paused:
        case SourcePhase::Checkpointing:
        case SourcePhase::Sealed:
        case SourcePhase::Draining:
        case SourcePhase::Retained:
        case SourcePhase::Expired:
        case SourcePhase::Failed:
            return true;
    }
    return false;
}

bool sequenceWindowAdmit(ReplayWindow& window, uint32_t sequence) {
    if (window.mask == 0) {
        window.highest = sequence;
        window.mask = 1ull;
        return true;
    }
    const uint32_t highest = static_cast<uint32_t>(window.highest);
    if (sequence == highest) return false;
    if (qnet::frameAfter(sequence, highest)) {
        const uint32_t advance = qnet::forwardDistance(highest, sequence);
        window.mask = advance >= kSequenceWindowEntries
            ? 0ull
            : (window.mask << advance);
        window.mask |= 1ull;
        window.highest = sequence;
        return true;
    }
    const uint32_t back = qnet::forwardDistance(sequence, highest);
    if (back >= kSequenceWindowEntries) return false;
    const uint64_t bit = 1ull << back;
    if ((window.mask & bit) != 0) return false;
    window.mask |= bit;
    return true;
}

uint64_t viewerGenerationNonce(uint64_t viewerId, uint32_t generation) {
    uint64_t hash = 0xCBF29CE484222325ull;
    mix64(hash, 0x5153502D4E4F4E43ull); // "QSP-NONC"
    mix64(hash, viewerId);
    mix32(hash, generation);
    const uint64_t value = avalanche(hash);
    return value == 0 ? 1ull : value;
}

bool encodeReadyMetadata(
    const ReadyMetadata& metadata, std::vector<uint8_t>& out) {
    if (metadata.selector > 1u
        || metadata.initialPhase != kReadyInitialPhase
        || metadata.bootstrapHash == 0
        || metadata.vsRow > 5u
        || metadata.cpuMask > 0x0Fu
        || (metadata.activeLaneMask & 0xF0u) != 0
        || (metadata.activeLaneMask & 0x03u) != 0x03u
        || (metadata.activeSlotMask & 0xF0u) != 0
        || (metadata.activeSlotMask & 0x03u) != 0x03u
        || !isSupportedQohMatchConfig(
            metadata.timerMode,
            metadata.aiDifficulty,
            metadata.lifeMultiplier,
            metadata.roundsToWinMinusOne)) {
        return false;
    }
    for (size_t slot = 0; slot < kReadySourceLanes; ++slot) {
        const uint8_t route = metadata.slotToSourceRoute[slot];
        if (route >= kReadySourceLanes) return false;
        const bool slotActive =
            ((metadata.activeSlotMask >> slot) & 1u) != 0;
        if (slotActive
            && ((metadata.activeLaneMask >> route) & 1u) == 0) {
            return false;
        }
    }
    for (size_t lane = 0; lane < kReadySourceLanes; ++lane) {
        const bool laneActive =
            ((metadata.activeLaneMask >> lane) & 1u) != 0;
        const bool ok = laneActive
            ? isCanonicalReadyPlayerName(
                  metadata.playerName[lane],
                  metadata.playerNameCodePage[lane])
            : isAbsentReadyPlayerName(
                  metadata.playerName[lane],
                  metadata.playerNameCodePage[lane]);
        if (!ok) return false;
    }

    std::vector<uint8_t> encoded(kReadyMetadataBytes, 0);
    writeLe32(encoded.data(), kReadyMetadataMagic);
    writeLe32(encoded.data() + 4, kReadyMetadataVersion);
    writeLe32(encoded.data() + 8, metadata.seedGameplay);
    writeLe32(encoded.data() + 12, metadata.seedCosmetic);
    writeLe32(encoded.data() + 16, metadata.selector);
    writeLe32(encoded.data() + 20, metadata.initialPhase);
    writeLe32(encoded.data() + 24, 10000u);
    writeLe32(encoded.data() + 28, 606u);
    writeLe32(encoded.data() + 32, 0x027Fu);
    writeLe32(encoded.data() + 36, 0x1F80u);
    writeLe64(encoded.data() + 40, metadata.bootstrapHash);
    writeLe32(encoded.data() + 48, metadata.timerMode);
    writeLe32(encoded.data() + 52, metadata.aiDifficulty);
    writeLe32(encoded.data() + 56, metadata.lifeMultiplier);
    writeLe32(encoded.data() + 60, metadata.roundsToWinMinusOne);
    writeLe32(encoded.data() + kReadyVsRowOffset, metadata.vsRow);
    writeLe32(encoded.data() + kReadyCpuMaskOffset, metadata.cpuMask);
    for (size_t slot = 0; slot < kReadySourceLanes; ++slot) {
        encoded[kReadyRouteOffset + slot] =
            metadata.slotToSourceRoute[slot];
    }
    encoded[kReadyActiveLaneMaskOffset] = metadata.activeLaneMask;
    encoded[kReadyActiveSlotMaskOffset] = metadata.activeSlotMask;
    encoded[kReadyRosterEpochLowOffset] = metadata.rosterEpochLow;
    encoded[kReadyReservedByteOffset] = 0;
    for (size_t lane = 0; lane < kReadySourceLanes; ++lane) {
        writeLe32(
            encoded.data() + kReadyCodePageOffset
                + lane * sizeof(uint32_t),
            metadata.playerNameCodePage[lane]);
        std::copy_n(
            reinterpret_cast<const uint8_t*>(metadata.playerName[lane]),
            kReadyPlayerNameBytes,
            encoded.data() + kReadyNameOffset
                + lane * kReadyPlayerNameBytes);
    }
    out.swap(encoded);
    return true;
}

bool decodeReadyMetadata(
    const std::vector<uint8_t>& bytes, ReadyMetadata& out) {
    if (bytes.size() != kReadyMetadataBytes
        || readLe32(bytes.data()) != kReadyMetadataMagic
        || readLe32(bytes.data() + 4) != kReadyMetadataVersion
        || readLe32(bytes.data() + 24) != 10000u
        || readLe32(bytes.data() + 28) != 606u
        || readLe32(bytes.data() + 32) != 0x027Fu
        || readLe32(bytes.data() + 36) != 0x1F80u
        || bytes[kReadyReservedByteOffset] != 0) {
        return false;
    }

    ReadyMetadata decoded{};
    decoded.seedGameplay = readLe32(bytes.data() + 8);
    decoded.seedCosmetic = readLe32(bytes.data() + 12);
    decoded.selector = readLe32(bytes.data() + 16);
    decoded.initialPhase = readLe32(bytes.data() + 20);
    decoded.bootstrapHash = readLe64(bytes.data() + 40);
    decoded.timerMode = readLe32(bytes.data() + 48);
    decoded.aiDifficulty = readLe32(bytes.data() + 52);
    decoded.lifeMultiplier = readLe32(bytes.data() + 56);
    decoded.roundsToWinMinusOne = readLe32(bytes.data() + 60);
    decoded.vsRow = readLe32(bytes.data() + kReadyVsRowOffset);
    decoded.cpuMask = readLe32(bytes.data() + kReadyCpuMaskOffset);
    for (size_t slot = 0; slot < kReadySourceLanes; ++slot) {
        decoded.slotToSourceRoute[slot] =
            bytes[kReadyRouteOffset + slot];
    }
    decoded.activeLaneMask = bytes[kReadyActiveLaneMaskOffset];
    decoded.activeSlotMask = bytes[kReadyActiveSlotMaskOffset];
    decoded.rosterEpochLow = bytes[kReadyRosterEpochLowOffset];
    for (size_t lane = 0; lane < kReadySourceLanes; ++lane) {
        decoded.playerNameCodePage[lane] = readLe32(
            bytes.data() + kReadyCodePageOffset
                + lane * sizeof(uint32_t));
        std::copy_n(
            reinterpret_cast<const char*>(
                bytes.data() + kReadyNameOffset
                    + lane * kReadyPlayerNameBytes),
            kReadyPlayerNameBytes,
            decoded.playerName[lane]);
    }

    if (decoded.selector > 1u
        || decoded.initialPhase != kReadyInitialPhase
        || decoded.bootstrapHash == 0
        || decoded.vsRow > 5u
        || decoded.cpuMask > 0x0Fu
        || (decoded.activeLaneMask & 0xF0u) != 0
        || (decoded.activeLaneMask & 0x03u) != 0x03u
        || (decoded.activeSlotMask & 0xF0u) != 0
        || (decoded.activeSlotMask & 0x03u) != 0x03u
        || !isSupportedQohMatchConfig(
            decoded.timerMode,
            decoded.aiDifficulty,
            decoded.lifeMultiplier,
            decoded.roundsToWinMinusOne)) {
        return false;
    }
    for (size_t slot = 0; slot < kReadySourceLanes; ++slot) {
        const uint8_t route = decoded.slotToSourceRoute[slot];
        if (route >= kReadySourceLanes) return false;
        const bool slotActive =
            ((decoded.activeSlotMask >> slot) & 1u) != 0;
        if (slotActive
            && ((decoded.activeLaneMask >> route) & 1u) == 0) {
            return false;
        }
    }
    for (size_t lane = 0; lane < kReadySourceLanes; ++lane) {
        const bool laneActive =
            ((decoded.activeLaneMask >> lane) & 1u) != 0;
        const bool ok = laneActive
            ? isCanonicalReadyPlayerName(
                  decoded.playerName[lane],
                  decoded.playerNameCodePage[lane])
            : isAbsentReadyPlayerName(
                  decoded.playerName[lane],
                  decoded.playerNameCodePage[lane]);
        if (!ok) return false;
    }
    out = decoded;
    return true;
}

bool isValidIdentity(const SessionIdentity& identity) {
    return identity.sessionId != 0
        && identity.epoch != 0
        && (identity.cookie.low != 0 || identity.cookie.high != 0);
}

bool isKnownMessageType(MessageType type) {
    switch (type) {
        case MessageType::Hello:
        case MessageType::GameStart:
        case MessageType::InitAck:
        case MessageType::ConfirmedInputBatch:
        case MessageType::Ack:
        case MessageType::Terminal:
        case MessageType::TerminalAck:
        case MessageType::Reject:
        case MessageType::Detach:
        case MessageType::Migrate:
        case MessageType::Heartbeat:
        case MessageType::CheckpointOffer:
        case MessageType::CheckpointChunk:
        case MessageType::SetEvent:
            return true;
    }
    return false;
}

Direction expectedDirection(MessageType type) {
    switch (type) {
        case MessageType::Hello:
        case MessageType::InitAck:
        case MessageType::Ack:
        case MessageType::TerminalAck:
        case MessageType::Detach:
        case MessageType::Migrate:
            return Direction::ViewerToSource;
        case MessageType::GameStart:
        case MessageType::ConfirmedInputBatch:
        case MessageType::Terminal:
        case MessageType::Reject:
        case MessageType::Heartbeat:
        case MessageType::CheckpointOffer:
        case MessageType::CheckpointChunk:
        case MessageType::SetEvent:
            return Direction::SourceToViewer;
    }
    return Direction::ViewerToSource;
}

PeerRole expectedSenderRole(MessageType type) {
    return expectedDirection(type) == Direction::ViewerToSource
        ? PeerRole::Viewer
        : PeerRole::Source;
}

bool encodePayload(const HelloPayload& payload, std::vector<uint8_t>& out) {
    if (payload.simulationContract == 0
        || !compat::isValidCompatibilityIdentity(payload.compatibilityIdentity)
        || payload.viewerNonce == 0
        || payload.viewerId == 0
        || (payload.flags & ~kHelloKnownFlags) != 0) {
        return false;
    }
    std::vector<uint8_t> encoded(kHelloPayloadSize, 0);
    writeLe64(encoded.data(), payload.simulationContract);
    writeLe64(encoded.data() + 8, payload.requestedGameId);
    writeLe64(encoded.data() + 16, payload.viewerNonce);
    writeLe64(encoded.data() + 24, payload.viewerId);
    writeLe64(encoded.data() + 32, payload.resumeDigest);
    writeLe32(encoded.data() + 40, payload.resumeRecord);
    writeLe32(encoded.data() + 44, payload.generation);
    writeLe32(encoded.data() + 48, payload.resumeEvent);
    writeLe32(encoded.data() + 52, payload.flags);
    writeCompatibilityIdentity(
        encoded.data() + kLegacyHelloPayloadSize,
        payload.compatibilityIdentity);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, HelloPayload& out) {
    if (bytes.size() != kHelloPayloadSize
        || (readLe32(bytes.data() + 52) & ~kHelloKnownFlags) != 0) {
        return false;
    }
    HelloPayload decoded{};
    decoded.simulationContract = readLe64(bytes.data());
    decoded.requestedGameId = readLe64(bytes.data() + 8);
    decoded.viewerNonce = readLe64(bytes.data() + 16);
    decoded.viewerId = readLe64(bytes.data() + 24);
    decoded.resumeDigest = readLe64(bytes.data() + 32);
    decoded.resumeRecord = readLe32(bytes.data() + 40);
    decoded.generation = readLe32(bytes.data() + 44);
    decoded.resumeEvent = readLe32(bytes.data() + 48);
    decoded.flags = readLe32(bytes.data() + 52);
    if (!readCompatibilityIdentity(
            bytes.data() + kLegacyHelloPayloadSize,
            decoded.compatibilityIdentity)) {
        return false;
    }
    if (decoded.simulationContract == 0
        || decoded.viewerNonce == 0
        || decoded.viewerId == 0) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const GameStartPayload& payload, std::vector<uint8_t>& out) {
    if (!validStartScalars(payload)) return false;
    std::vector<uint8_t> encoded(
        kGameStartFixedSize + payload.metadata.size(), 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe64(encoded.data() + 8, payload.simulationContract);
    writeLe64(encoded.data() + 16, payload.initialStateHash);
    writeLe64(encoded.data() + 24, payload.initialDigest);
    writeLe64(encoded.data() + 32, payload.resumeDigest);
    writeLe64(encoded.data() + 40, payload.viewerNonce);
    writeLe64(encoded.data() + 48, payload.viewerId);
    writeLe32(encoded.data() + 56, payload.resumeRecord);
    writeLe32(encoded.data() + 60, payload.firstTimelineFrame);
    writeLe32(encoded.data() + 64, payload.generation);
    writeLe32(encoded.data() + 68, payload.resumeEvent);
    writeLe16(encoded.data() + 72, payload.playerOneDelay);
    writeLe16(encoded.data() + 74, payload.playerTwoDelay);
    writeLe16(encoded.data() + 76, payload.rollbackWindow);
    writeLe16(encoded.data() + 78,
              static_cast<uint16_t>(payload.metadata.size()));
    writeLe64(encoded.data() + 80, 0);
    writeCompatibilityIdentity(
        encoded.data() + kLegacyGameStartFixedSize,
        payload.compatibilityIdentity);
    std::copy(payload.metadata.begin(), payload.metadata.end(),
              encoded.begin() + static_cast<std::ptrdiff_t>(
                  kGameStartFixedSize));
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, GameStartPayload& out) {
    if (bytes.size() < kGameStartFixedSize
        || bytes.size() > kGameStartFixedSize + kMaxGameMetadataSize) {
        return false;
    }
    if (readLe64(bytes.data() + 80) != 0) return false;
    const uint16_t metadataLength = readLe16(bytes.data() + 78);
    if (bytes.size() != kGameStartFixedSize + metadataLength)
        return false;
    GameStartPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.simulationContract = readLe64(bytes.data() + 8);
    decoded.initialStateHash = readLe64(bytes.data() + 16);
    decoded.initialDigest = readLe64(bytes.data() + 24);
    decoded.resumeDigest = readLe64(bytes.data() + 32);
    decoded.viewerNonce = readLe64(bytes.data() + 40);
    decoded.viewerId = readLe64(bytes.data() + 48);
    decoded.resumeRecord = readLe32(bytes.data() + 56);
    decoded.firstTimelineFrame = readLe32(bytes.data() + 60);
    decoded.generation = readLe32(bytes.data() + 64);
    decoded.resumeEvent = readLe32(bytes.data() + 68);
    decoded.playerOneDelay = readLe16(bytes.data() + 72);
    decoded.playerTwoDelay = readLe16(bytes.data() + 74);
    decoded.rollbackWindow = readLe16(bytes.data() + 76);
    if (!readCompatibilityIdentity(
            bytes.data() + kLegacyGameStartFixedSize,
            decoded.compatibilityIdentity)) {
        return false;
    }
    decoded.metadata.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(kGameStartFixedSize),
        bytes.end());
    if (!validStartScalars(decoded)) return false;
    out = std::move(decoded);
    return true;
}

bool encodePayload(const InitAckPayload& payload, std::vector<uint8_t>& out) {
    if (payload.gameId == kLatestGameId
        || payload.viewerNonce == 0
        || payload.viewerId == 0) {
        return false;
    }
    std::vector<uint8_t> encoded(kInitAckPayloadSize, 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe64(encoded.data() + 8, payload.viewerNonce);
    writeLe64(encoded.data() + 16, payload.startFingerprint);
    writeLe64(encoded.data() + 24, payload.resumeDigest);
    writeLe64(encoded.data() + 32, payload.viewerId);
    writeLe32(encoded.data() + 40, payload.resumeRecord);
    writeLe32(encoded.data() + 44, payload.generation);
    writeLe32(encoded.data() + 48, payload.resumeEvent);
    writeLe32(encoded.data() + 52, 0);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, InitAckPayload& out) {
    if (bytes.size() != kInitAckPayloadSize
        || readLe32(bytes.data() + 52) != 0) {
        return false;
    }
    InitAckPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.viewerNonce = readLe64(bytes.data() + 8);
    decoded.startFingerprint = readLe64(bytes.data() + 16);
    decoded.resumeDigest = readLe64(bytes.data() + 24);
    decoded.viewerId = readLe64(bytes.data() + 32);
    decoded.resumeRecord = readLe32(bytes.data() + 40);
    decoded.generation = readLe32(bytes.data() + 44);
    decoded.resumeEvent = readLe32(bytes.data() + 48);
    if (decoded.gameId == kLatestGameId
        || decoded.viewerNonce == 0
        || decoded.viewerId == 0) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const ConfirmedInputBatchPayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.gameId == kLatestGameId
        || payload.records.empty()
        || payload.records.size() > kMaxRecordsPerBatch
        || payload.records.front().record != payload.startRecord) {
        return false;
    }
    for (size_t i = 0; i < payload.records.size(); ++i) {
        if (payload.records[i].record
            != payload.startRecord + static_cast<uint32_t>(i)) {
            return false;
        }
    }
    if (payload.records.back().rollingDigest != payload.endDigest)
        return false;

    std::vector<uint8_t> encoded(
        kBatchFixedSize + payload.records.size() * kRecordWireSize, 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe32(encoded.data() + 8, payload.startRecord);
    writeLe16(encoded.data() + 12,
              static_cast<uint16_t>(payload.records.size()));
    writeLe16(encoded.data() + 14, 0);
    writeLe64(encoded.data() + 16, payload.baseDigest);
    writeLe64(encoded.data() + 24, payload.endDigest);
    writeLe32(encoded.data() + 32, payload.baseEventIndex);
    writeLe32(encoded.data() + 36, 0);
    for (size_t i = 0; i < payload.records.size(); ++i) {
        const ConfirmedRecord& record = payload.records[i];
        if (!qinput::isCanonicalFightInput(record.playerOneInput)
            || !qinput::isCanonicalFightInput(record.playerTwoInput)) {
            return false;
        }
        uint8_t* dst = encoded.data() + kBatchFixedSize + i * kRecordWireSize;
        writeLe32(dst, record.record);
        writeLe32(dst + 4, record.timelineFrame);
        writeLe16(dst + 8, record.playerOneInput);
        writeLe16(dst + 10, record.playerTwoInput);
        writeLe64(dst + 12, record.stateHash);
        writeLe64(dst + 20, record.rollingDigest);
    }
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   ConfirmedInputBatchPayload& out) {
    if (bytes.size() < kBatchFixedSize
        || bytes.size() > kBatchFixedSize
            + kMaxRecordsPerBatch * kRecordWireSize) {
        return false;
    }
    const uint16_t count = readLe16(bytes.data() + 12);
    if (count == 0 || count > kMaxRecordsPerBatch
        || readLe16(bytes.data() + 14) != 0
        || readLe32(bytes.data() + 36) != 0
        || bytes.size() != kBatchFixedSize
            + static_cast<size_t>(count) * kRecordWireSize) {
        return false;
    }

    ConfirmedInputBatchPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.startRecord = readLe32(bytes.data() + 8);
    decoded.baseDigest = readLe64(bytes.data() + 16);
    decoded.endDigest = readLe64(bytes.data() + 24);
    decoded.baseEventIndex = readLe32(bytes.data() + 32);
    if (decoded.gameId == kLatestGameId
        || decoded.startRecord > (std::numeric_limits<uint32_t>::max)()
               - static_cast<uint32_t>(count - 1)) {
        return false;
    }
    decoded.records.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* src =
            bytes.data() + kBatchFixedSize + static_cast<size_t>(i)
                * kRecordWireSize;
        ConfirmedRecord record{};
        record.record = readLe32(src);
        record.timelineFrame = readLe32(src + 4);
        record.playerOneInput = readLe16(src + 8);
        record.playerTwoInput = readLe16(src + 10);
        record.stateHash = readLe64(src + 12);
        record.rollingDigest = readLe64(src + 20);
        if (record.record
            != decoded.startRecord + static_cast<uint32_t>(i)) {
            return false;
        }
        if (!qinput::isCanonicalFightInput(record.playerOneInput)
            || !qinput::isCanonicalFightInput(record.playerTwoInput)) {
            return false;
        }
        decoded.records.push_back(record);
    }
    if (decoded.records.back().rollingDigest != decoded.endDigest)
        return false;
    out = std::move(decoded);
    return true;
}

bool encodePayload(const AckPayload& payload, std::vector<uint8_t>& out) {
    if (payload.gameId == kLatestGameId || payload.viewerId == 0)
        return false;
    std::vector<uint8_t> encoded(kAckPayloadSize, 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe64(encoded.data() + 8, payload.digest);
    writeLe64(encoded.data() + 16, payload.viewerId);
    writeLe32(encoded.data() + 24, payload.nextRecord);
    writeLe32(encoded.data() + 28, payload.generation);
    writeLe32(encoded.data() + 32, payload.nextEvent);
    writeLe32(encoded.data() + 36, 0);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, AckPayload& out) {
    if (bytes.size() != kAckPayloadSize
        || readLe32(bytes.data() + 36) != 0) {
        return false;
    }
    AckPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.digest = readLe64(bytes.data() + 8);
    decoded.viewerId = readLe64(bytes.data() + 16);
    decoded.nextRecord = readLe32(bytes.data() + 24);
    decoded.generation = readLe32(bytes.data() + 28);
    decoded.nextEvent = readLe32(bytes.data() + 32);
    if (decoded.gameId == kLatestGameId || decoded.viewerId == 0)
        return false;
    out = decoded;
    return true;
}

bool encodePayload(const TerminalPayload& payload, std::vector<uint8_t>& out) {
    if (payload.gameId == kLatestGameId
        || !validTerminalReason(payload.reason)) {
        return false;
    }
    std::vector<uint8_t> encoded(kTerminalPayloadSize, 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe64(encoded.data() + 8, payload.finalDigest);
    writeLe64(encoded.data() + 16, payload.postTailStateHash);
    writeLe32(encoded.data() + 24, payload.nextRecord);
    writeLe16(encoded.data() + 28, static_cast<uint16_t>(payload.reason));
    writeLe16(encoded.data() + 30, 0);
    writeLe32(encoded.data() + 32, payload.nextEvent);
    writeLe32(encoded.data() + 36, 0);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, TerminalPayload& out) {
    if (bytes.size() != kTerminalPayloadSize
        || readLe16(bytes.data() + 30) != 0
        || readLe32(bytes.data() + 36) != 0) {
        return false;
    }
    TerminalPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.finalDigest = readLe64(bytes.data() + 8);
    decoded.postTailStateHash = readLe64(bytes.data() + 16);
    decoded.nextRecord = readLe32(bytes.data() + 24);
    decoded.reason =
        static_cast<TerminalReason>(readLe16(bytes.data() + 28));
    decoded.nextEvent = readLe32(bytes.data() + 32);
    if (decoded.gameId == kLatestGameId
        || !validTerminalReason(decoded.reason)) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const TerminalAckPayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.gameId == kLatestGameId
        || payload.viewerId == 0
        || !validTerminalReason(payload.reason)) {
        return false;
    }
    std::vector<uint8_t> encoded(kTerminalAckPayloadSize, 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe64(encoded.data() + 8, payload.finalDigest);
    writeLe64(encoded.data() + 16, payload.postTailStateHash);
    writeLe64(encoded.data() + 24, payload.viewerId);
    writeLe32(encoded.data() + 32, payload.nextRecord);
    writeLe16(encoded.data() + 36, static_cast<uint16_t>(payload.reason));
    writeLe16(encoded.data() + 38, 0);
    writeLe32(encoded.data() + 40, payload.generation);
    writeLe32(encoded.data() + 44, payload.nextEvent);
    writeLe64(encoded.data() + 48, 0);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   TerminalAckPayload& out) {
    if (bytes.size() != kTerminalAckPayloadSize
        || readLe16(bytes.data() + 38) != 0
        || readLe64(bytes.data() + 48) != 0) {
        return false;
    }
    TerminalAckPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.finalDigest = readLe64(bytes.data() + 8);
    decoded.postTailStateHash = readLe64(bytes.data() + 16);
    decoded.viewerId = readLe64(bytes.data() + 24);
    decoded.nextRecord = readLe32(bytes.data() + 32);
    decoded.reason =
        static_cast<TerminalReason>(readLe16(bytes.data() + 36));
    decoded.generation = readLe32(bytes.data() + 40);
    decoded.nextEvent = readLe32(bytes.data() + 44);
    if (decoded.gameId == kLatestGameId
        || decoded.viewerId == 0
        || !validTerminalReason(decoded.reason)) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const RejectPayload& payload, std::vector<uint8_t>& out) {
    if (payload.viewerNonce == 0
        || payload.viewerId == 0
        || !validRejectReason(payload.reason)) {
        return false;
    }
    std::vector<uint8_t> encoded(kRejectPayloadSize, 0);
    writeLe64(encoded.data(), payload.requestedGameId);
    writeLe64(encoded.data() + 8, payload.availableGameId);
    writeLe64(encoded.data() + 16, payload.viewerNonce);
    writeLe64(encoded.data() + 24, payload.viewerId);
    writeLe32(encoded.data() + 32, payload.availableNextRecord);
    writeLe16(encoded.data() + 36, static_cast<uint16_t>(payload.reason));
    writeLe16(encoded.data() + 38, 0);
    writeLe32(encoded.data() + 40, payload.generation);
    writeLe32(encoded.data() + 44, 0);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, RejectPayload& out) {
    if (bytes.size() != kRejectPayloadSize
        || readLe16(bytes.data() + 38) != 0
        || readLe32(bytes.data() + 44) != 0) {
        return false;
    }
    RejectPayload decoded{};
    decoded.requestedGameId = readLe64(bytes.data());
    decoded.availableGameId = readLe64(bytes.data() + 8);
    decoded.viewerNonce = readLe64(bytes.data() + 16);
    decoded.viewerId = readLe64(bytes.data() + 24);
    decoded.availableNextRecord = readLe32(bytes.data() + 32);
    decoded.reason =
        static_cast<RejectReason>(readLe16(bytes.data() + 36));
    decoded.generation = readLe32(bytes.data() + 40);
    if (decoded.viewerNonce == 0
        || decoded.viewerId == 0
        || !validRejectReason(decoded.reason)) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const DetachPayload& payload, std::vector<uint8_t>& out) {
    if (payload.viewerId == 0) return false;
    std::vector<uint8_t> encoded(kDetachPayloadSize, 0);
    writeLe64(encoded.data(), payload.viewerId);
    writeLe64(encoded.data() + 8, payload.digest);
    writeLe32(encoded.data() + 16, payload.nextRecord);
    writeLe32(encoded.data() + 20, payload.generation);
    writeLe32(encoded.data() + 24, payload.nextEvent);
    writeLe32(encoded.data() + 28, 0);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, DetachPayload& out) {
    if (bytes.size() != kDetachPayloadSize
        || readLe32(bytes.data() + 28) != 0) {
        return false;
    }
    DetachPayload decoded{};
    decoded.viewerId = readLe64(bytes.data());
    decoded.digest = readLe64(bytes.data() + 8);
    decoded.nextRecord = readLe32(bytes.data() + 16);
    decoded.generation = readLe32(bytes.data() + 20);
    decoded.nextEvent = readLe32(bytes.data() + 24);
    if (decoded.viewerId == 0) return false;
    out = decoded;
    return true;
}

bool encodePayload(const MigratePayload& payload, std::vector<uint8_t>& out) {
    if (payload.viewerId == 0) return false;
    std::vector<uint8_t> encoded(kMigratePayloadSize, 0);
    writeLe64(encoded.data(), payload.viewerId);
    writeLe64(encoded.data() + 8, payload.resumeDigest);
    writeLe64(encoded.data() + 16, payload.grantId);
    writeLe64(encoded.data() + 24, payload.grantMacLow);
    writeLe64(encoded.data() + 32, payload.grantMacHigh);
    writeLe32(encoded.data() + 40, payload.oldGeneration);
    writeLe32(encoded.data() + 44, payload.newGeneration);
    writeLe32(encoded.data() + 48, payload.resumeRecord);
    writeLe32(encoded.data() + 52, payload.resumeEvent);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, MigratePayload& out) {
    if (bytes.size() != kMigratePayloadSize) return false;
    MigratePayload decoded{};
    decoded.viewerId = readLe64(bytes.data());
    decoded.resumeDigest = readLe64(bytes.data() + 8);
    decoded.grantId = readLe64(bytes.data() + 16);
    decoded.grantMacLow = readLe64(bytes.data() + 24);
    decoded.grantMacHigh = readLe64(bytes.data() + 32);
    decoded.oldGeneration = readLe32(bytes.data() + 40);
    decoded.newGeneration = readLe32(bytes.data() + 44);
    decoded.resumeRecord = readLe32(bytes.data() + 48);
    decoded.resumeEvent = readLe32(bytes.data() + 52);
    if (decoded.viewerId == 0) return false;
    out = decoded;
    return true;
}

bool encodePayload(const HeartbeatPayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.gameId == kLatestGameId
        || !isKnownSourcePhase(payload.sourcePhase)) {
        return false;
    }
    std::vector<uint8_t> encoded(kHeartbeatPayloadSize, 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe64(encoded.data() + 8, payload.digest);
    writeLe32(encoded.data() + 16, payload.nextRecord);
    writeLe32(encoded.data() + 20, payload.checkpointRecord);
    encoded[24] = static_cast<uint8_t>(payload.sourcePhase);
    encoded[25] = 0;
    writeLe16(encoded.data() + 26, payload.viewerCount);
    writeLe32(encoded.data() + 28, payload.nextEvent);
    writeLe32(encoded.data() + 32, payload.currentMatchId);
    writeLe32(encoded.data() + 36, 0);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, HeartbeatPayload& out) {
    if (bytes.size() != kHeartbeatPayloadSize
        || bytes[25] != 0
        || readLe32(bytes.data() + 36) != 0) {
        return false;
    }
    HeartbeatPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.digest = readLe64(bytes.data() + 8);
    decoded.nextRecord = readLe32(bytes.data() + 16);
    decoded.checkpointRecord = readLe32(bytes.data() + 20);
    decoded.sourcePhase = static_cast<SourcePhase>(bytes[24]);
    decoded.viewerCount = readLe16(bytes.data() + 26);
    decoded.nextEvent = readLe32(bytes.data() + 28);
    decoded.currentMatchId = readLe32(bytes.data() + 32);
    if (decoded.gameId == kLatestGameId
        || !isKnownSourcePhase(decoded.sourcePhase)) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const CheckpointOfferPayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.gameId == kLatestGameId
        || payload.checkpointId == 0
        || !compat::isValidCompatibilityIdentity(payload.compatibilityIdentity)
        || payload.totalBytes == 0
        || payload.chunkCount == 0
        || payload.chunkBytes == 0
        || payload.chunkBytes > kMaxCheckpointChunkBytes) {
        return false;
    }
    const uint64_t covered = static_cast<uint64_t>(payload.chunkCount)
        * static_cast<uint64_t>(payload.chunkBytes);
    if (covered < payload.totalBytes
        || covered - payload.totalBytes >= payload.chunkBytes) {
        return false;
    }
    std::vector<uint8_t> encoded(kCheckpointOfferPayloadSize, 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe64(encoded.data() + 8, payload.checkpointId);
    writeLe64(encoded.data() + 16, payload.digest);
    writeLe32(encoded.data() + 24, payload.recordCursor);
    writeLe32(encoded.data() + 28, payload.totalBytes);
    writeLe32(encoded.data() + 32, payload.chunkCount);
    writeLe32(encoded.data() + 36, payload.chunkBytes);
    writeCompatibilityIdentity(
        encoded.data() + kLegacyCheckpointOfferPayloadSize,
        payload.compatibilityIdentity);
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   CheckpointOfferPayload& out) {
    if (bytes.size() != kCheckpointOfferPayloadSize) return false;
    CheckpointOfferPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.checkpointId = readLe64(bytes.data() + 8);
    decoded.digest = readLe64(bytes.data() + 16);
    decoded.recordCursor = readLe32(bytes.data() + 24);
    decoded.totalBytes = readLe32(bytes.data() + 28);
    decoded.chunkCount = readLe32(bytes.data() + 32);
    decoded.chunkBytes = readLe32(bytes.data() + 36);
    if (!readCompatibilityIdentity(
            bytes.data() + kLegacyCheckpointOfferPayloadSize,
            decoded.compatibilityIdentity)) {
        return false;
    }
    if (decoded.gameId == kLatestGameId
        || decoded.checkpointId == 0
        || !compat::isValidCompatibilityIdentity(decoded.compatibilityIdentity)
        || decoded.totalBytes == 0
        || decoded.chunkCount == 0
        || decoded.chunkBytes == 0
        || decoded.chunkBytes > kMaxCheckpointChunkBytes) {
        return false;
    }
    const uint64_t covered = static_cast<uint64_t>(decoded.chunkCount)
        * static_cast<uint64_t>(decoded.chunkBytes);
    if (covered < decoded.totalBytes
        || covered - decoded.totalBytes >= decoded.chunkBytes) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const CheckpointChunkPayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.gameId == kLatestGameId
        || payload.checkpointId == 0
        || payload.bytes.empty()
        || payload.bytes.size() > kMaxCheckpointChunkBytes
        || payload.chunkCount == 0
        || payload.chunkIndex >= payload.chunkCount) {
        return false;
    }
    std::vector<uint8_t> encoded(
        kCheckpointChunkFixedSize + payload.bytes.size(), 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe64(encoded.data() + 8, payload.checkpointId);
    writeLe32(encoded.data() + 16, payload.chunkIndex);
    writeLe32(encoded.data() + 20, payload.chunkCount);
    writeLe32(encoded.data() + 24, payload.byteOffset);
    writeLe16(encoded.data() + 28,
              static_cast<uint16_t>(payload.bytes.size()));
    writeLe16(encoded.data() + 30, 0);
    std::copy(payload.bytes.begin(), payload.bytes.end(),
              encoded.begin() + static_cast<std::ptrdiff_t>(
                  kCheckpointChunkFixedSize));
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   CheckpointChunkPayload& out) {
    if (bytes.size() < kCheckpointChunkFixedSize
        || bytes.size() > kCheckpointChunkFixedSize
            + kMaxCheckpointChunkBytes
        || readLe16(bytes.data() + 30) != 0) {
        return false;
    }
    const uint16_t byteLength = readLe16(bytes.data() + 28);
    if (byteLength == 0
        || bytes.size() != kCheckpointChunkFixedSize + byteLength) {
        return false;
    }
    CheckpointChunkPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.checkpointId = readLe64(bytes.data() + 8);
    decoded.chunkIndex = readLe32(bytes.data() + 16);
    decoded.chunkCount = readLe32(bytes.data() + 20);
    decoded.byteOffset = readLe32(bytes.data() + 24);
    if (decoded.gameId == kLatestGameId
        || decoded.checkpointId == 0
        || decoded.chunkCount == 0
        || decoded.chunkIndex >= decoded.chunkCount) {
        return false;
    }
    decoded.bytes.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(
            kCheckpointChunkFixedSize),
        bytes.end());
    out = std::move(decoded);
    return true;
}

namespace {

void writeSetEvent(uint8_t* dst, const SetEvent& event) {
    writeLe32(dst, event.eventIndex);
    writeLe32(dst + 4, event.recordCursor);
    writeLe64(dst + 8, event.recordDigest);
    writeLe32(dst + 16, event.timelineFrame);
    writeLe32(dst + 20, event.matchId);
    writeLe16(dst + 24, static_cast<uint16_t>(event.kind));
    writeLe16(dst + 26, static_cast<uint16_t>(event.terminalReason));
    writeLe32(dst + 28, event.rosterEpoch);
    writeLe64(dst + 32, event.valueA);
    writeLe64(dst + 40, event.valueB);
    writeLe32(dst + 48, event.valueC);
    dst[52] = event.selectorProofMask;
    dst[53] = event.activeSlotMask;
    writeLe16(dst + 54, 0);
    writeLe64(dst + 56, event.stateProof);
    writeLe64(dst + 64, event.rollingDigest);
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
        dst[72 + slot * 2] = event.roster[slot].nativeCharacterId;
        dst[73 + slot * 2] = event.roster[slot].paletteIndex;
    }
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
        dst[80 + slot * 3] = event.selectorProof[slot].cursorCell;
        dst[81 + slot * 3] = event.selectorProof[slot].alternateFlag;
        dst[82 + slot * 3] = event.selectorProof[slot].confirmState;
    }
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot)
        dst[92 + slot] = event.slotToSourceRoute[slot];
    std::memcpy(dst + 96, event.paletteEntries, kSetPaletteEntryBytes);
}

bool readSetEvent(const uint8_t* src, SetEvent& out) {
    if (readLe16(src + 54) != 0) return false;
    SetEvent event{};
    event.eventIndex = readLe32(src);
    event.recordCursor = readLe32(src + 4);
    event.recordDigest = readLe64(src + 8);
    event.timelineFrame = readLe32(src + 16);
    event.matchId = readLe32(src + 20);
    const uint16_t kind = readLe16(src + 24);
    const uint16_t reason = readLe16(src + 26);
    if (!isKnownSetEventKind(kind)
        || !isKnownSetSegmentTerminalReason(reason)) {
        return false;
    }
    event.kind = static_cast<SetEventKind>(kind);
    event.terminalReason = static_cast<SetSegmentTerminalReason>(reason);
    event.rosterEpoch = readLe32(src + 28);
    event.valueA = readLe64(src + 32);
    event.valueB = readLe64(src + 40);
    event.valueC = readLe32(src + 48);
    event.selectorProofMask = src[52];
    event.activeSlotMask = src[53];
    event.stateProof = readLe64(src + 56);
    event.rollingDigest = readLe64(src + 64);
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
        event.roster[slot].nativeCharacterId = src[72 + slot * 2];
        event.roster[slot].paletteIndex = src[73 + slot * 2];
    }
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
        event.selectorProof[slot].cursorCell = src[80 + slot * 3];
        event.selectorProof[slot].alternateFlag = src[81 + slot * 3];
        event.selectorProof[slot].confirmState = src[82 + slot * 3];
    }
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot)
        event.slotToSourceRoute[slot] = src[92 + slot];
    std::memcpy(event.paletteEntries, src + 96, kSetPaletteEntryBytes);
    if (!isValidSetEvent(event)) return false;
    if (event.rollingDigest == 0) return false;
    out = event;
    return true;
}

}

bool encodePayload(const SetEventBatchPayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.gameId == kLatestGameId
        || payload.events.empty()
        || payload.events.size() > kMaxSetEventsPerBatch) {
        return false;
    }
    for (size_t i = 0; i < payload.events.size(); ++i) {
        const SetEvent& event = payload.events[i];
        if (event.eventIndex
            != payload.startEvent + static_cast<uint32_t>(i)) {
            return false;
        }
        if (event.rollingDigest == 0) return false;
        if (!isValidSetEvent(event)) return false;
    }
    uint64_t chain = payload.baseDigest;
    for (const SetEvent& event : payload.events) {
        if (event.recordDigest != chain) return false;
        chain = advanceSetEventDigest(chain, payload.gameId, event);
        if (event.rollingDigest != chain) return false;
    }

    std::vector<uint8_t> encoded(
        kSetEventBatchFixedSize
            + payload.events.size() * kSetEventWireSize, 0);
    writeLe64(encoded.data(), payload.gameId);
    writeLe64(encoded.data() + 8, payload.baseDigest);
    writeLe32(encoded.data() + 16, payload.startEvent);
    writeLe16(encoded.data() + 20,
              static_cast<uint16_t>(payload.events.size()));
    writeLe16(encoded.data() + 22, 0);
    for (size_t i = 0; i < payload.events.size(); ++i) {
        writeSetEvent(
            encoded.data() + kSetEventBatchFixedSize
                + i * kSetEventWireSize,
            payload.events[i]);
    }
    out.swap(encoded);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   SetEventBatchPayload& out) {
    if (bytes.size() < kSetEventBatchFixedSize
        || bytes.size() > kSetEventBatchFixedSize
            + kMaxSetEventsPerBatch * kSetEventWireSize) {
        return false;
    }
    const uint16_t count = readLe16(bytes.data() + 20);
    if (count == 0 || count > kMaxSetEventsPerBatch
        || readLe16(bytes.data() + 22) != 0
        || bytes.size() != kSetEventBatchFixedSize
            + static_cast<size_t>(count) * kSetEventWireSize) {
        return false;
    }
    SetEventBatchPayload decoded{};
    decoded.gameId = readLe64(bytes.data());
    decoded.baseDigest = readLe64(bytes.data() + 8);
    decoded.startEvent = readLe32(bytes.data() + 16);
    if (decoded.gameId == kLatestGameId
        || decoded.startEvent > (std::numeric_limits<uint32_t>::max)()
               - static_cast<uint32_t>(count - 1)) {
        return false;
    }
    decoded.events.reserve(count);
    uint64_t chain = decoded.baseDigest;
    for (uint16_t i = 0; i < count; ++i) {
        SetEvent event{};
        if (!readSetEvent(
                bytes.data() + kSetEventBatchFixedSize
                    + static_cast<size_t>(i) * kSetEventWireSize,
                event)) {
            return false;
        }
        if (event.eventIndex
            != decoded.startEvent + static_cast<uint32_t>(i)) {
            return false;
        }
        if (event.recordDigest != chain) return false;
        chain = advanceSetEventDigest(chain, decoded.gameId, event);
        if (event.rollingDigest != chain) return false;
        decoded.events.push_back(event);
    }
    out = std::move(decoded);
    return true;
}

uint32_t datagramChecksum(const uint8_t* bytes, size_t length) {
    if (!bytes || length < kSpectatorHeaderSize) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    crc = crcUpdate(crc, bytes, kCrcOffset);
    constexpr uint8_t zeroCrc[4] = {};
    crc = crcUpdate(crc, zeroCrc, sizeof(zeroCrc));
    if (length > kSpectatorHeaderSize) {
        crc = crcUpdate(crc, bytes + kSpectatorHeaderSize,
                        length - kSpectatorHeaderSize);
    }
    return ~crc;
}

bool encodeDatagram(MessageType type,
                    const SessionIdentity& identity,
                    uint32_t sequence,
                    const std::vector<uint8_t>& payload,
                    std::vector<uint8_t>& out) {
    if (!isKnownMessageType(type)
        || !isValidIdentity(identity)
        || !validPayloadShape(type, payload)
        || payload.size() > (std::numeric_limits<uint16_t>::max)()
        || kSpectatorHeaderSize + payload.size()
            > kMaxSpectatorDatagramSize) {
        return false;
    }

    std::vector<uint8_t> encoded(
        kSpectatorHeaderSize + payload.size(), 0);
    writeLe32(encoded.data() + kMagicOffset, kSpectatorMagic);
    writeLe16(encoded.data() + kVersionOffset, kSpectatorVersion);
    encoded[kTypeOffset] = static_cast<uint8_t>(type);
    encoded[kDirectionOffset] =
        static_cast<uint8_t>(expectedDirection(type));
    encoded[kRoleOffset] = static_cast<uint8_t>(expectedSenderRole(type));
    encoded[kFlagsOffset] = 0;
    writeLe16(encoded.data() + kHeaderLengthOffset,
              static_cast<uint16_t>(kSpectatorHeaderSize));
    writeLe16(encoded.data() + kPayloadLengthOffset,
              static_cast<uint16_t>(payload.size()));
    writeLe16(encoded.data() + kReservedOffset, 0);
    writeLe64(encoded.data() + kSessionOffset, identity.sessionId);
    writeLe64(encoded.data() + kEpochOffset, identity.epoch);
    writeLe64(encoded.data() + kCookieLowOffset, identity.cookie.low);
    writeLe64(encoded.data() + kCookieHighOffset, identity.cookie.high);
    writeLe32(encoded.data() + kSequenceOffset, sequence);
    writeLe32(encoded.data() + kCrcOffset, 0);
    std::copy(payload.begin(), payload.end(),
              encoded.begin() + static_cast<std::ptrdiff_t>(
                  kSpectatorHeaderSize));
    writeLe32(encoded.data() + kCrcOffset,
              datagramChecksum(encoded.data(), encoded.size()));
    out.swap(encoded);
    return true;
}

DecodeError decodeDatagram(const uint8_t* bytes,
                           size_t length,
                           const SessionIdentity& expectedIdentity,
                           Datagram& out) {
    if (!bytes || length < kSpectatorHeaderSize)
        return DecodeError::TooShort;
    if (length > kMaxSpectatorDatagramSize)
        return DecodeError::TooLarge;
    if (readLe32(bytes + kMagicOffset) != kSpectatorMagic)
        return DecodeError::BadMagic;
    const uint16_t version = readLe16(bytes + kVersionOffset);
    const bool legacyV3 = version == kLegacySpectatorVersion;
    if (!legacyV3 && version != kSpectatorVersion)
        return DecodeError::BadVersion;

    const MessageType type = static_cast<MessageType>(bytes[kTypeOffset]);
    if (!isKnownMessageType(type)) return DecodeError::BadType;
    if (bytes[kDirectionOffset]
            != static_cast<uint8_t>(expectedDirection(type))
        || bytes[kRoleOffset]
            != static_cast<uint8_t>(expectedSenderRole(type))) {
        return DecodeError::BadDirectionRole;
    }
    if (bytes[kFlagsOffset] != 0
        || readLe16(bytes + kHeaderLengthOffset) != kSpectatorHeaderSize
        || readLe16(bytes + kReservedOffset) != 0) {
        return DecodeError::BadHeader;
    }

    const uint16_t payloadLength =
        readLe16(bytes + kPayloadLengthOffset);
    if (length != kSpectatorHeaderSize
            + static_cast<size_t>(payloadLength)) {
        return DecodeError::BadLength;
    }
    if (readLe64(bytes + kSessionOffset) != expectedIdentity.sessionId)
        return DecodeError::SessionMismatch;
    if (readLe64(bytes + kEpochOffset) != expectedIdentity.epoch)
        return DecodeError::EpochMismatch;

    const uint64_t cookieDifference =
        (readLe64(bytes + kCookieLowOffset) ^ expectedIdentity.cookie.low)
        | (readLe64(bytes + kCookieHighOffset)
            ^ expectedIdentity.cookie.high);
    if (cookieDifference != 0) return DecodeError::CookieMismatch;
    if (readLe32(bytes + kCrcOffset) != datagramChecksum(bytes, length))
        return DecodeError::BadCrc;

    std::vector<uint8_t> payload(
        bytes + kSpectatorHeaderSize, bytes + length);
    if (!(legacyV3
              ? validLegacyPayloadShape(type, payload)
              : validPayloadShape(type, payload))) {
        return DecodeError::BadPayload;
    }

    Datagram decoded{};
    decoded.header.version = version;
    decoded.header.compatibilityIdentityAttested = !legacyV3;
    decoded.header.type = type;
    decoded.header.direction =
        static_cast<Direction>(bytes[kDirectionOffset]);
    decoded.header.senderRole =
        static_cast<PeerRole>(bytes[kRoleOffset]);
    decoded.header.payloadLength = payloadLength;
    decoded.header.identity.sessionId =
        readLe64(bytes + kSessionOffset);
    decoded.header.identity.epoch = readLe64(bytes + kEpochOffset);
    decoded.header.identity.cookie.low =
        readLe64(bytes + kCookieLowOffset);
    decoded.header.identity.cookie.high =
        readLe64(bytes + kCookieHighOffset);
    decoded.header.sequence = readLe32(bytes + kSequenceOffset);
    decoded.header.crc32 = readLe32(bytes + kCrcOffset);
    decoded.payload = std::move(payload);
    out = std::move(decoded);
    return DecodeError::None;
}

uint64_t computeInitialDigest(const GameStartPayload& start) {
    uint64_t hash = 0xCBF29CE484222325ull;
    mix64(hash, 0x5153502D494E4954ull); // "QSP-INIT"
    mix64(hash, start.gameId);
    mix64(hash, start.simulationContract);
    mixCompatibilityIdentity(hash, start.compatibilityIdentity);
    mix64(hash, start.initialStateHash);
    mix32(hash, start.firstTimelineFrame);
    mix16(hash, start.playerOneDelay);
    mix16(hash, start.playerTwoDelay);
    mix16(hash, start.rollbackWindow);
    mix32(hash, static_cast<uint32_t>(start.metadata.size()));
    for (uint8_t byte : start.metadata) mixByte(hash, byte);
    return avalanche(hash);
}

uint64_t gameStartFingerprint(const GameStartPayload& start) {
    uint64_t hash = 0xCBF29CE484222325ull;
    mix64(hash, 0x5153502D53544152ull); // "QSP-STAR"
    mix64(hash, start.gameId);
    mix64(hash, start.simulationContract);
    mixCompatibilityIdentity(hash, start.compatibilityIdentity);
    mix64(hash, start.initialStateHash);
    mix64(hash, start.initialDigest);
    mix32(hash, start.firstTimelineFrame);
    mix16(hash, start.playerOneDelay);
    mix16(hash, start.playerTwoDelay);
    mix16(hash, start.rollbackWindow);
    mix32(hash, static_cast<uint32_t>(start.metadata.size()));
    for (uint8_t byte : start.metadata) mixByte(hash, byte);
    return avalanche(hash);
}

uint64_t advanceRollingDigest(uint64_t previous,
                              uint64_t gameId,
                              const ConfirmedRecord& record) {
    uint64_t hash = 0xCBF29CE484222325ull;
    mix64(hash, 0x5153502D4652414Dull); // "QSP-FRAM"
    mix64(hash, previous);
    mix64(hash, gameId);
    mix32(hash, record.record);
    mix32(hash, record.timelineFrame);
    mix16(hash, record.playerOneInput);
    mix16(hash, record.playerTwoInput);
    mix64(hash, record.stateHash);
    return avalanche(hash);
}

}
