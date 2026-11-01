#include "WireProtocol.h"

#include "input/InputActions.h"

#include <cstring>
#include <limits>
#include <utility>

namespace qnetplay::wire {
namespace {

constexpr size_t kMagicOffset = 0;
constexpr size_t kVersionOffset = 4;
constexpr size_t kTypeOffset = 6;
constexpr size_t kFlagsOffset = 7;
constexpr size_t kPayloadLengthOffset = 8;
constexpr size_t kReservedOffset = 10;
constexpr size_t kSessionIdOffset = 12;
constexpr size_t kCookieLowOffset = 20;
constexpr size_t kCookieHighOffset = 28;
constexpr size_t kSequenceOffset = 36;
constexpr size_t kCrcOffset = 40;
constexpr size_t kHelloFixedBytes = 12u;
static_assert(kHelloFixedBytes + compat::kCompatibilityIdentityWireBytes
                  == kHelloPayloadBytes,
              "hello identity layout and public payload size differ");

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

void writeCompatibilityIdentity(
    uint8_t* const bytes,
    const compat::CompatibilityIdentityWire& identity) {
    size_t offset = 0u;
    writeLe32(bytes + offset, identity.schemaVersion);
    offset += sizeof(uint32_t);
    writeLe32(bytes + offset, identity.protocolVersion);
    offset += sizeof(uint32_t);
    std::memcpy(bytes + offset, identity.runtimeBuild.profileId,
                compat::kCompatibilityProfileIdBytes);
    offset += compat::kCompatibilityProfileIdBytes;
    std::memcpy(bytes + offset, identity.runtimeBuild.executableSha256,
                compat::kCompatibilitySha256Bytes);
    offset += compat::kCompatibilitySha256Bytes;
    writeLe64(bytes + offset, identity.runtimeBuild.executableSize);
    offset += sizeof(uint64_t);
    writeLe32(bytes + offset, identity.runtimeBuild.peTimestamp);
    offset += sizeof(uint32_t);
    writeLe32(bytes + offset,
              identity.simulationContent.manifestSchemaVersion);
    offset += sizeof(uint32_t);
    writeLe32(bytes + offset, identity.simulationContent.manifestFlags);
    offset += sizeof(uint32_t);
    std::memcpy(bytes + offset,
                identity.simulationContent.simulationRootSha256,
                compat::kCompatibilitySha256Bytes);
    offset += compat::kCompatibilitySha256Bytes;
    std::memcpy(bytes + offset,
                identity.simulationContent.validationRootSha256,
                compat::kCompatibilitySha256Bytes);
    offset += compat::kCompatibilitySha256Bytes;
    writeLe32(bytes + offset, identity.simulationContent.entryCount);
    offset += sizeof(uint32_t);
    writeLe32(bytes + offset, identity.simulationContent.requiredEntryCount);
    offset += sizeof(uint32_t);
    writeLe64(bytes + offset, identity.stateSchemaId);
    offset += sizeof(uint64_t);
    writeLe64(bytes + offset, identity.modSimulationId);
    offset += sizeof(uint64_t);
    writeLe64(bytes + offset, identity.timingProfileId);
    offset += sizeof(uint64_t);
    writeLe32(bytes + offset, identity.reserved);
    offset += sizeof(uint32_t);
    (void)offset;
}

bool readCompatibilityIdentity(
    const uint8_t* const bytes,
    compat::CompatibilityIdentityWire& out) {
    compat::CompatibilityIdentityWire decoded{};
    size_t offset = 0u;
    decoded.schemaVersion = readLe32(bytes + offset);
    offset += sizeof(uint32_t);
    decoded.protocolVersion = readLe32(bytes + offset);
    offset += sizeof(uint32_t);
    std::memcpy(decoded.runtimeBuild.profileId, bytes + offset,
                compat::kCompatibilityProfileIdBytes);
    offset += compat::kCompatibilityProfileIdBytes;
    std::memcpy(decoded.runtimeBuild.executableSha256, bytes + offset,
                compat::kCompatibilitySha256Bytes);
    offset += compat::kCompatibilitySha256Bytes;
    decoded.runtimeBuild.executableSize = readLe64(bytes + offset);
    offset += sizeof(uint64_t);
    decoded.runtimeBuild.peTimestamp = readLe32(bytes + offset);
    offset += sizeof(uint32_t);
    decoded.simulationContent.manifestSchemaVersion =
        readLe32(bytes + offset);
    offset += sizeof(uint32_t);
    decoded.simulationContent.manifestFlags = readLe32(bytes + offset);
    offset += sizeof(uint32_t);
    std::memcpy(decoded.simulationContent.simulationRootSha256,
                bytes + offset, compat::kCompatibilitySha256Bytes);
    offset += compat::kCompatibilitySha256Bytes;
    std::memcpy(decoded.simulationContent.validationRootSha256,
                bytes + offset, compat::kCompatibilitySha256Bytes);
    offset += compat::kCompatibilitySha256Bytes;
    decoded.simulationContent.entryCount = readLe32(bytes + offset);
    offset += sizeof(uint32_t);
    decoded.simulationContent.requiredEntryCount = readLe32(bytes + offset);
    offset += sizeof(uint32_t);
    decoded.stateSchemaId = readLe64(bytes + offset);
    offset += sizeof(uint64_t);
    decoded.modSimulationId = readLe64(bytes + offset);
    offset += sizeof(uint64_t);
    decoded.timingProfileId = readLe64(bytes + offset);
    offset += sizeof(uint64_t);
    decoded.reserved = readLe32(bytes + offset);
    offset += sizeof(uint32_t);
    if (offset != compat::kCompatibilityIdentityWireBytes
        || !compat::isValidCompatibilityIdentity(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

uint32_t crcUpdate(uint32_t crc, const uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

uint32_t datagramCrc(const uint8_t* bytes, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crcUpdate(crc, bytes, kCrcOffset);
    constexpr uint8_t zeroCrc[4] = {};
    crc = crcUpdate(crc, zeroCrc, sizeof(zeroCrc));
    if (length > kHeaderSize)
        crc = crcUpdate(crc, bytes + kHeaderSize, length - kHeaderSize);
    return ~crc;
}

bool validRole(PeerRole role) {
    return role == PeerRole::Host || role == PeerRole::Client;
}

bool validTerminalError(TerminalErrorCode code) {
    switch (code) {
        case TerminalErrorCode::PeerRequested:
        case TerminalErrorCode::ProtocolViolation:
        case TerminalErrorCode::TransportFailure:
        case TerminalErrorCode::Timeout:
        case TerminalErrorCode::InternalFailure:
        case TerminalErrorCode::ConfirmedDesync:
        case TerminalErrorCode::InvalidConfiguration:
            return true;
    }
    return false;
}

bool validTerminalReason(
    const std::array<char, kTerminalReasonBytes>& reason,
    const uint32_t expectedId) {
    if (expectedId == 0 || reason[0] == '\0')
        return false;
    bool terminated = false;
    for (const unsigned char byte : reason) {
        if (terminated) {
            if (byte != 0) return false;
            continue;
        }
        if (byte == 0) {
            terminated = true;
            continue;
        }
        if (byte < 0x20u || byte > 0x7Eu)
            return false;
    }
    return terminated && terminalReasonId(reason.data()) == expectedId;
}

}

const char* terminalErrorCodeName(const TerminalErrorCode code) {
    switch (code) {
        case TerminalErrorCode::PeerRequested: return "peer-requested";
        case TerminalErrorCode::ProtocolViolation: return "protocol-violation";
        case TerminalErrorCode::TransportFailure: return "transport-failure";
        case TerminalErrorCode::Timeout: return "timeout";
        case TerminalErrorCode::InternalFailure: return "internal-failure";
        case TerminalErrorCode::ConfirmedDesync: return "confirmed-desync";
        case TerminalErrorCode::InvalidConfiguration:
            return "invalid-configuration";
    }
    return "unknown";
}

const char* decodeErrorName(const DecodeError error) {
    switch (error) {
        case DecodeError::None: return "none";
        case DecodeError::TooShort: return "too-short";
        case DecodeError::BadMagic: return "bad-magic";
        case DecodeError::BadVersion: return "bad-version";
        case DecodeError::BadType: return "bad-type";
        case DecodeError::BadLength: return "bad-length";
        case DecodeError::BadFlags: return "bad-flags";
        case DecodeError::SessionMismatch: return "session-mismatch";
        case DecodeError::BadCrc: return "bad-crc";
    }
    return "unknown";
}

const char* messageTypeName(const MessageType type) {
    switch (type) {
        case MessageType::Hello: return "hello";
        case MessageType::HelloAck: return "hello-ack";
        case MessageType::Ping: return "ping";
        case MessageType::Pong: return "pong";
        case MessageType::Inputs: return "inputs";
        case MessageType::Ready: return "ready";
        case MessageType::SyncHash: return "sync-hash";
        case MessageType::SyncHashAck: return "sync-hash-ack";
        case MessageType::Disconnect: return "disconnect";
        case MessageType::DisconnectAck: return "disconnect-ack";
        case MessageType::RouteProbe: return "route-probe";
        case MessageType::RouteProbeAck: return "route-probe-ack";
        case MessageType::SessionEvent: return "session-event";
        case MessageType::RematchChoice: return "rematch-choice";
        case MessageType::Palette: return "palette";
    }
    return "unknown";
}

uint32_t terminalReasonId(const char* reason) {
    if (reason == nullptr || *reason == '\0')
        return 0;
    uint32_t hash = 2166136261u;
    size_t count = 0;
    while (*reason != '\0' && count + 1u < kTerminalReasonBytes) {
        const unsigned char byte =
            static_cast<unsigned char>(*reason++);
        const uint8_t canonical =
            byte >= 0x20u && byte <= 0x7Eu ? byte : '?';
        hash ^= canonical;
        hash *= 16777619u;
        ++count;
    }
    return hash != 0 ? hash : 1u;
}

bool setTerminalReason(
    std::array<char, kTerminalReasonBytes>& out,
    const char* reason) {
    out.fill('\0');
    if (reason == nullptr || *reason == '\0')
        return false;
    size_t count = 0;
    while (*reason != '\0' && count + 1u < out.size()) {
        const unsigned char byte =
            static_cast<unsigned char>(*reason++);
        out[count++] =
            byte >= 0x20u && byte <= 0x7Eu
                ? static_cast<char>(byte) : '?';
    }
    out[count] = '\0';
    return count != 0;
}

bool isValidSyncHashPayload(const SyncHashPayload& payload) {
    if (payload.tag.frame == kNoFrame) return false;
    if (payload.tag.frame != kLayoutFingerprintFrame) return true;

    return payload.gameplayHash != 0;
}

bool isKnownType(MessageType type) {
    switch (type) {
        case MessageType::Hello:
        case MessageType::HelloAck:
        case MessageType::Ping:
        case MessageType::Pong:
        case MessageType::Inputs:
        case MessageType::Ready:
        case MessageType::SyncHash:
        case MessageType::SyncHashAck:
        case MessageType::Disconnect:
        case MessageType::DisconnectAck:
        case MessageType::RouteProbe:
        case MessageType::RouteProbeAck:
        case MessageType::SessionEvent:
        case MessageType::RematchChoice:
        case MessageType::Palette:
            return true;
    }
    return false;
}

bool isValidReadyPayload(const ReadyPayload& payload) {
    return (payload.flags & ~kReadyAcknowledgesPeer) == 0
        && payload.selector <= 1u
        && payload.bootstrapHash != 0
        && isSupportedQohMatchConfig(
            payload.timerMode,
            payload.aiDifficulty,
            payload.lifeMultiplier,
            payload.roundsToWinMinusOne);
}

size_t payloadSize(MessageType type) {
    switch (type) {
        case MessageType::Hello:
        case MessageType::HelloAck:
            return kHelloPayloadBytes;
        case MessageType::Ping:
        case MessageType::Pong:
            return 12;
        case MessageType::Inputs:
            return 12 + kInputWindowSize * 2 + 4 + 16;
        case MessageType::Ready:
            return 40;
        case MessageType::SyncHash:
            return 40;
        case MessageType::SyncHashAck:
            return 12;
        case MessageType::Disconnect:
            return 12 + kTerminalReasonBytes;
        case MessageType::DisconnectAck:
            return 12;
        case MessageType::RouteProbe:
            return 20;
        case MessageType::RouteProbeAck:
            return 24;
        case MessageType::SessionEvent:
            return 36;
        case MessageType::RematchChoice:
            return 12;
        case MessageType::Palette:
            return 8 + kPaletteTableWireBytes;
    }
    return std::numeric_limits<size_t>::max();
}

bool encodePayload(const SessionEventPayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.stage < SessionEventStage::Request
        || payload.stage > SessionEventStage::Abort
        || payload.kind > 4 || (payload.flags & ~0x03u) != 0
        || payload.eventId == 0) {
        return false;
    }
    out.assign(payloadSize(MessageType::SessionEvent), 0);
    out[0] = static_cast<uint8_t>(payload.stage);
    out[1] = payload.kind;
    out[2] = payload.flags;
    out[3] = 0;
    writeLe32(out.data() + 4, payload.eventId);
    writeLe32(out.data() + 8, payload.value);
    writeLe32(out.data() + 12, payload.boundary);
    writeLe32(out.data() + 16, payload.nextFrame);
    writeLe32(out.data() + 20, payload.assignedTailExclusive);
    writeLe32(out.data() + 24, payload.oldestRestorable);
    writeLe32(out.data() + 28, payload.spectatorFrontier);
    writeLe32(out.data() + 32, payload.lifecycleKey);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   SessionEventPayload& out) {
    if (bytes.size() != payloadSize(MessageType::SessionEvent)) return false;
    SessionEventPayload decoded{};
    decoded.stage = static_cast<SessionEventStage>(bytes[0]);
    decoded.kind = bytes[1];
    decoded.flags = bytes[2];
    if (decoded.stage < SessionEventStage::Request
        || decoded.stage > SessionEventStage::Abort
        || decoded.kind > 4 || (decoded.flags & ~0x03u) != 0
        || bytes[3] != 0) {
        return false;
    }
    decoded.eventId = readLe32(bytes.data() + 4);
    if (decoded.eventId == 0) return false;
    decoded.value = readLe32(bytes.data() + 8);
    decoded.boundary = readLe32(bytes.data() + 12);
    decoded.nextFrame = readLe32(bytes.data() + 16);
    decoded.assignedTailExclusive = readLe32(bytes.data() + 20);
    decoded.oldestRestorable = readLe32(bytes.data() + 24);
    decoded.spectatorFrontier = readLe32(bytes.data() + 28);
    decoded.lifecycleKey = readLe32(bytes.data() + 32);
    out = decoded;
    return true;
}

bool encodePayload(const RematchChoicePayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.side > 1 || payload.choice > kRematchChoiceLeave
        || payload.ready > 1 || payload.reserved != 0) {
        return false;
    }
    out.assign(payloadSize(MessageType::RematchChoice), 0);
    writeLe32(out.data() + 0, payload.epoch);
    writeLe32(out.data() + 4, payload.promptFrame);
    out[8] = payload.side;
    out[9] = payload.choice;
    out[10] = payload.ready;
    out[11] = 0;
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   RematchChoicePayload& out) {
    if (bytes.size() != payloadSize(MessageType::RematchChoice)) return false;
    RematchChoicePayload decoded{};
    decoded.epoch = readLe32(bytes.data() + 0);
    decoded.promptFrame = readLe32(bytes.data() + 4);
    decoded.side = bytes[8];
    decoded.choice = bytes[9];
    decoded.ready = bytes[10];
    decoded.reserved = bytes[11];
    if (decoded.side > 1 || decoded.choice > kRematchChoiceLeave
        || decoded.ready > 1 || decoded.reserved != 0) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const PalettePayload& payload, std::vector<uint8_t>& out) {
    if (payload.side > 1 || payload.variant > 3 || payload.present > 1
        || payload.reserved != 0) {
        return false;
    }
    out.assign(payloadSize(MessageType::Palette), 0);
    writeLe32(out.data() + 0, payload.epoch);
    out[4] = payload.side;
    out[5] = payload.variant;
    out[6] = payload.present;
    out[7] = 0;
    std::memcpy(out.data() + 8, payload.table, kPaletteTableWireBytes);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, PalettePayload& out) {
    if (bytes.size() != payloadSize(MessageType::Palette)) return false;
    PalettePayload decoded{};
    decoded.epoch = readLe32(bytes.data() + 0);
    decoded.side = bytes[4];
    decoded.variant = bytes[5];
    decoded.present = bytes[6];
    decoded.reserved = bytes[7];
    if (decoded.side > 1 || decoded.variant > 3 || decoded.present > 1
        || decoded.reserved != 0) {
        return false;
    }
    std::memcpy(decoded.table, bytes.data() + 8, kPaletteTableWireBytes);
    // Entry 0 of every variant must be black: it is the game's transparent key.
    if (decoded.present != 0) {
        for (size_t variant = 0; variant < 4; ++variant) {
            const uint8_t* key = decoded.table + variant * 64;
            if (key[0] != 0 || key[1] != 0 || key[2] != 0) return false;
        }
    }
    out = decoded;
    return true;
}

bool encodePayload(const HelloPayload& payload, std::vector<uint8_t>& out) {
    if (!validRole(payload.role) || payload.player > 1
        || payload.compatibilityId == 0u
        || !compat::isValidCompatibilityIdentity(
               payload.compatibilityIdentity)
        || payload.compatibilityIdentity.protocolVersion != kVersion) {
        return false;
    }
    out.assign(payloadSize(MessageType::Hello), 0);
    writeLe64(out.data(), payload.compatibilityId);
    out[8] = static_cast<uint8_t>(payload.role);
    out[9] = payload.player;
    writeCompatibilityIdentity(
        out.data() + kHelloFixedBytes, payload.compatibilityIdentity);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, HelloPayload& out) {
    if (bytes.size() != payloadSize(MessageType::Hello)
        || bytes[10] != 0 || bytes[11] != 0) {
        return false;
    }
    HelloPayload decoded{};
    decoded.compatibilityId = readLe64(bytes.data());
    decoded.role = static_cast<PeerRole>(bytes[8]);
    decoded.player = bytes[9];
    if (!validRole(decoded.role) || decoded.player > 1
        || decoded.compatibilityId == 0u
        || !readCompatibilityIdentity(
               bytes.data() + kHelloFixedBytes,
               decoded.compatibilityIdentity)
        || decoded.compatibilityIdentity.protocolVersion != kVersion) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const PingPayload& payload, std::vector<uint8_t>& out) {
    out.assign(12, 0);
    writeLe32(out.data(), payload.pingId);
    writeLe64(out.data() + 4, payload.sentMs);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, PingPayload& out) {
    if (bytes.size() != 12) return false;
    PingPayload decoded{};
    decoded.pingId = readLe32(bytes.data());
    decoded.sentMs = readLe64(bytes.data() + 4);
    out = decoded;
    return true;
}

bool encodePayload(const RouteProbePayload& payload, std::vector<uint8_t>& out) {
    if (payload.sequence == 0 || payload.generation == 0
        || !validRole(payload.role)) {
        return false;
    }
    out.assign(payloadSize(MessageType::RouteProbe), 0);
    writeLe32(out.data(), payload.sequence);
    writeLe32(out.data() + 4, payload.generation);
    writeLe64(out.data() + 8, payload.stampMicros);
    out[16] = payload.routeId;
    out[17] = static_cast<uint8_t>(payload.role);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, RouteProbePayload& out) {
    if (bytes.size() != payloadSize(MessageType::RouteProbe)
        || bytes[18] != 0 || bytes[19] != 0) {
        return false;
    }
    RouteProbePayload decoded{};
    decoded.sequence = readLe32(bytes.data());
    decoded.generation = readLe32(bytes.data() + 4);
    decoded.stampMicros = readLe64(bytes.data() + 8);
    decoded.routeId = bytes[16];
    decoded.role = static_cast<PeerRole>(bytes[17]);
    if (decoded.sequence == 0 || decoded.generation == 0
        || !validRole(decoded.role)) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const RouteProbeAckPayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.sequence == 0 || payload.generation == 0
        || !validRole(payload.role)) {
        return false;
    }
    out.assign(payloadSize(MessageType::RouteProbeAck), 0);
    writeLe32(out.data(), payload.sequence);
    writeLe32(out.data() + 4, payload.generation);
    writeLe64(out.data() + 8, payload.echoStampMicros);
    writeLe32(out.data() + 16, payload.dwellMicros);
    out[20] = payload.routeId;
    out[21] = static_cast<uint8_t>(payload.role);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   RouteProbeAckPayload& out) {
    if (bytes.size() != payloadSize(MessageType::RouteProbeAck)
        || bytes[22] != 0 || bytes[23] != 0) {
        return false;
    }
    RouteProbeAckPayload decoded{};
    decoded.sequence = readLe32(bytes.data());
    decoded.generation = readLe32(bytes.data() + 4);
    decoded.echoStampMicros = readLe64(bytes.data() + 8);
    decoded.dwellMicros = readLe32(bytes.data() + 16);
    decoded.routeId = bytes[20];
    decoded.role = static_cast<PeerRole>(bytes[21]);
    if (decoded.sequence == 0 || decoded.generation == 0
        || !validRole(decoded.role)) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const InputsPayload& payload, std::vector<uint8_t>& out) {
    if ((payload.validMask >> kInputWindowSize) != 0) return false;
    for (size_t i = 0; i < payload.inputs.size(); ++i) {
        if ((payload.validMask & (1u << i)) == 0
            && payload.inputs[i] != 0) {
            return false;
        }
        if ((payload.validMask & (1u << i)) != 0
            && payload.newestFrame - static_cast<uint32_t>(i)
                == kNoFrame) {
            return false;
        }
        if ((payload.validMask & (1u << i)) != 0
            && !qinput::isCanonicalFightInput(payload.inputs[i])) {
            return false;
        }
    }
    if (payload.pressure.runState > kMaxRunStateOrdinal) return false;
    out.assign(payloadSize(MessageType::Inputs), 0);
    writeLe32(out.data(), payload.newestFrame);
    writeLe32(out.data() + 4, payload.ackThrough);
    writeLe32(out.data() + 8, payload.validMask);
    for (size_t i = 0; i < payload.inputs.size(); ++i)
        writeLe16(out.data() + 12 + i * 2, payload.inputs[i]);
    uint8_t* tail = out.data() + 12 + kInputWindowSize * 2;
    writeLe32(tail, payload.simFrontier);
    writeLe32(tail + 4, payload.pressure.inputProducedThrough);
    writeLe32(tail + 8, payload.pressure.confirmedFrontier);
    writeLe16(tail + 12, payload.pressure.predictionDepth);
    tail[14] = payload.pressure.correctionDebt;
    tail[15] = payload.pressure.runState;
    writeLe32(tail + 16, payload.pressure.holdGeneration);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, InputsPayload& out) {
    if (bytes.size() != payloadSize(MessageType::Inputs)) return false;
    InputsPayload decoded{};
    decoded.newestFrame = readLe32(bytes.data());
    decoded.ackThrough = readLe32(bytes.data() + 4);
    decoded.validMask = readLe32(bytes.data() + 8);
    if ((decoded.validMask >> kInputWindowSize) != 0) return false;
    for (size_t i = 0; i < decoded.inputs.size(); ++i) {
        decoded.inputs[i] = readLe16(bytes.data() + 12 + i * 2);
        if ((decoded.validMask & (1u << i)) == 0
            && decoded.inputs[i] != 0) {
            return false;
        }
        if ((decoded.validMask & (1u << i)) != 0
            && decoded.newestFrame - static_cast<uint32_t>(i)
                == kNoFrame) {
            return false;
        }
        if ((decoded.validMask & (1u << i)) != 0
            && !qinput::isCanonicalFightInput(decoded.inputs[i])) {
            return false;
        }
    }
    const uint8_t* tail = bytes.data() + 12 + kInputWindowSize * 2;
    decoded.simFrontier = readLe32(tail);
    decoded.pressure.inputProducedThrough = readLe32(tail + 4);
    decoded.pressure.confirmedFrontier = readLe32(tail + 8);
    decoded.pressure.predictionDepth = readLe16(tail + 12);
    decoded.pressure.correctionDebt = tail[14];
    decoded.pressure.runState = tail[15];
    decoded.pressure.holdGeneration = readLe32(tail + 16);
    if (decoded.pressure.runState > kMaxRunStateOrdinal) return false;
    out = decoded;
    return true;
}

bool encodePayload(const ReadyPayload& payload, std::vector<uint8_t>& out) {
    if (!isValidReadyPayload(payload)) return false;
    out.assign(payloadSize(MessageType::Ready), 0);
    writeLe32(out.data(), payload.seedGameplay);
    writeLe32(out.data() + 4, payload.seedCosmetic);
    writeLe32(out.data() + 8, payload.selector);
    writeLe32(out.data() + 12, payload.flags);
    writeLe64(out.data() + 16, payload.bootstrapHash);
    writeLe32(out.data() + 24, payload.timerMode);
    writeLe32(out.data() + 28, payload.aiDifficulty);
    writeLe32(out.data() + 32, payload.lifeMultiplier);
    writeLe32(out.data() + 36, payload.roundsToWinMinusOne);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, ReadyPayload& out) {
    if (bytes.size() != payloadSize(MessageType::Ready)) return false;
    ReadyPayload decoded{};
    decoded.seedGameplay = readLe32(bytes.data());
    decoded.seedCosmetic = readLe32(bytes.data() + 4);
    decoded.selector = readLe32(bytes.data() + 8);
    decoded.flags = readLe32(bytes.data() + 12);
    decoded.bootstrapHash = readLe64(bytes.data() + 16);
    decoded.timerMode = readLe32(bytes.data() + 24);
    decoded.aiDifficulty = readLe32(bytes.data() + 28);
    decoded.lifeMultiplier = readLe32(bytes.data() + 32);
    decoded.roundsToWinMinusOne = readLe32(bytes.data() + 36);
    if (!isValidReadyPayload(decoded)) return false;
    out = decoded;
    return true;
}

bool encodePayload(const SyncHashPayload& payload, std::vector<uint8_t>& out) {
    if (!isValidSyncHashPayload(payload)) return false;
    out.assign(payloadSize(MessageType::SyncHash), 0);
    writeLe32(out.data(), payload.tag.layoutGeneration);
    writeLe32(out.data() + 4, payload.tag.epoch);
    writeLe32(out.data() + 8, payload.tag.frame);
    writeLe32(out.data() + 12, payload.rngGameplay);
    writeLe32(out.data() + 16, payload.rngCosmetic);
    writeLe32(out.data() + 20, payload.rngSelector);
    writeLe32(out.data() + 24, static_cast<uint32_t>(payload.hp0));
    writeLe32(out.data() + 28, static_cast<uint32_t>(payload.hp1));
    writeLe64(out.data() + 32, payload.gameplayHash);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes, SyncHashPayload& out) {
    if (bytes.size() != payloadSize(MessageType::SyncHash)) return false;
    SyncHashPayload decoded{};
    decoded.tag.layoutGeneration = readLe32(bytes.data());
    decoded.tag.epoch = readLe32(bytes.data() + 4);
    decoded.tag.frame = readLe32(bytes.data() + 8);
    decoded.rngGameplay = readLe32(bytes.data() + 12);
    decoded.rngCosmetic = readLe32(bytes.data() + 16);
    decoded.rngSelector = readLe32(bytes.data() + 20);
    decoded.hp0 = static_cast<int32_t>(readLe32(bytes.data() + 24));
    decoded.hp1 = static_cast<int32_t>(readLe32(bytes.data() + 28));
    decoded.gameplayHash = readLe64(bytes.data() + 32);
    if (!isValidSyncHashPayload(decoded)) return false;
    out = decoded;
    return true;
}

bool encodePayload(const SyncHashAckPayload& payload,
                   std::vector<uint8_t>& out) {
    if (payload.tag.frame == kNoFrame) return false;
    out.assign(payloadSize(MessageType::SyncHashAck), 0);
    writeLe32(out.data(), payload.tag.layoutGeneration);
    writeLe32(out.data() + 4, payload.tag.epoch);
    writeLe32(out.data() + 8, payload.tag.frame);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   SyncHashAckPayload& out) {
    if (bytes.size() != payloadSize(MessageType::SyncHashAck)) return false;
    SyncHashAckPayload decoded{};
    decoded.tag.layoutGeneration = readLe32(bytes.data());
    decoded.tag.epoch = readLe32(bytes.data() + 4);
    decoded.tag.frame = readLe32(bytes.data() + 8);
    if (decoded.tag.frame == kNoFrame) return false;
    out = decoded;
    return true;
}

bool encodePayload(const DisconnectPayload& payload,
                   std::vector<uint8_t>& out) {
    if (!validTerminalError(payload.code)
        || !validTerminalReason(payload.reason, payload.reasonId)) {
        return false;
    }
    out.assign(payloadSize(MessageType::Disconnect), 0);
    writeLe32(out.data(), static_cast<uint32_t>(payload.code));
    writeLe32(out.data() + 4, payload.detail);
    writeLe32(out.data() + 8, payload.reasonId);
    std::memcpy(out.data() + 12, payload.reason.data(),
                payload.reason.size());
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   DisconnectPayload& out) {
    if (bytes.size() != payloadSize(MessageType::Disconnect)) return false;
    DisconnectPayload decoded{};
    decoded.code = static_cast<TerminalErrorCode>(readLe32(bytes.data()));
    decoded.detail = readLe32(bytes.data() + 4);
    decoded.reasonId = readLe32(bytes.data() + 8);
    std::memcpy(decoded.reason.data(), bytes.data() + 12,
                decoded.reason.size());
    if (!validTerminalError(decoded.code)
        || !validTerminalReason(decoded.reason, decoded.reasonId)) {
        return false;
    }
    out = decoded;
    return true;
}

bool encodePayload(const DisconnectAckPayload& payload,
                   std::vector<uint8_t>& out) {
    if (!validTerminalError(payload.code) || payload.reasonId == 0)
        return false;
    out.assign(payloadSize(MessageType::DisconnectAck), 0);
    writeLe32(out.data(), static_cast<uint32_t>(payload.code));
    writeLe32(out.data() + 4, payload.detail);
    writeLe32(out.data() + 8, payload.reasonId);
    return true;
}

bool decodePayload(const std::vector<uint8_t>& bytes,
                   DisconnectAckPayload& out) {
    if (bytes.size() != payloadSize(MessageType::DisconnectAck)) return false;
    DisconnectAckPayload decoded{};
    decoded.code = static_cast<TerminalErrorCode>(readLe32(bytes.data()));
    decoded.detail = readLe32(bytes.data() + 4);
    decoded.reasonId = readLe32(bytes.data() + 8);
    if (!validTerminalError(decoded.code) || decoded.reasonId == 0)
        return false;
    out = decoded;
    return true;
}

bool encodeDatagram(MessageType type,
                    uint64_t sessionId,
                    const SessionCookie& cookie,
                    uint32_t sequence,
                    const std::vector<uint8_t>& payload,
                    std::vector<uint8_t>& out) {
    if (!isKnownType(type) || sessionId == 0
        || payload.size() != payloadSize(type)
        || payload.size() > std::numeric_limits<uint16_t>::max()
        || kHeaderSize + payload.size() > kMaxDatagramSize) {
        return false;
    }

    std::vector<uint8_t> encoded(kHeaderSize + payload.size(), 0);
    writeLe32(encoded.data() + kMagicOffset, kMagic);
    writeLe16(encoded.data() + kVersionOffset, kVersion);
    encoded[kTypeOffset] = static_cast<uint8_t>(type);
    encoded[kFlagsOffset] = 0;
    writeLe16(encoded.data() + kPayloadLengthOffset,
              static_cast<uint16_t>(payload.size()));
    writeLe16(encoded.data() + kReservedOffset, 0);
    writeLe64(encoded.data() + kSessionIdOffset, sessionId);
    writeLe64(encoded.data() + kCookieLowOffset, cookie.low);
    writeLe64(encoded.data() + kCookieHighOffset, cookie.high);
    writeLe32(encoded.data() + kSequenceOffset, sequence);
    writeLe32(encoded.data() + kCrcOffset, 0);
    for (size_t i = 0; i < payload.size(); ++i)
        encoded[kHeaderSize + i] = payload[i];
    writeLe32(encoded.data() + kCrcOffset,
              datagramCrc(encoded.data(), encoded.size()));
    out.swap(encoded);
    return true;
}

DecodeError decodeDatagram(const uint8_t* bytes,
                           size_t length,
                           uint64_t expectedSessionId,
                           const SessionCookie& expectedCookie,
                           Datagram& out) {
    if (!bytes || length < kHeaderSize) return DecodeError::TooShort;
    if (readLe32(bytes + kMagicOffset) != kMagic) return DecodeError::BadMagic;
    if (readLe16(bytes + kVersionOffset) != kVersion) return DecodeError::BadVersion;

    const MessageType type = static_cast<MessageType>(bytes[kTypeOffset]);
    if (!isKnownType(type)) return DecodeError::BadType;

    const uint16_t encodedLength = readLe16(bytes + kPayloadLengthOffset);
    if (encodedLength != payloadSize(type)
        || length != kHeaderSize + static_cast<size_t>(encodedLength)
        || length > kMaxDatagramSize) {
        return DecodeError::BadLength;
    }
    if (bytes[kFlagsOffset] != 0 || readLe16(bytes + kReservedOffset) != 0)
        return DecodeError::BadFlags;

    const uint64_t receivedSessionId =
        readLe64(bytes + kSessionIdOffset);
    const SessionCookie receivedCookie{
        readLe64(bytes + kCookieLowOffset),
        readLe64(bytes + kCookieHighOffset),
    };
    if (((receivedSessionId ^ expectedSessionId)
       | (receivedCookie.low ^ expectedCookie.low)
       | (receivedCookie.high ^ expectedCookie.high)) != 0) {
        return DecodeError::SessionMismatch;
    }

    const uint32_t receivedCrc = readLe32(bytes + kCrcOffset);
    if (receivedCrc != datagramCrc(bytes, length)) return DecodeError::BadCrc;

    Datagram decoded{};
    decoded.header.type = type;
    decoded.header.flags = bytes[kFlagsOffset];
    decoded.header.payloadLength = encodedLength;
    decoded.header.sessionId = receivedSessionId;
    decoded.header.cookie = receivedCookie;
    decoded.header.sequence = readLe32(bytes + kSequenceOffset);
    decoded.header.crc32 = receivedCrc;
    decoded.payload.assign(bytes + kHeaderSize, bytes + length);
    out = std::move(decoded);
    return DecodeError::None;
}

}
