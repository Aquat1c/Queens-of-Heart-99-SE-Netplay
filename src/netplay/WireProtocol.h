#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../compat/CompatibilityIdentity.h"
#include "MatchConfig.h"

namespace qnetplay::wire {

// Little-endian bytes spell "QNP5"; kVersion below is the compatibility check.
constexpr uint32_t kMagic = 0x35504E51u;
constexpr uint16_t kVersion = 14;
constexpr size_t kHeaderSize = 44;
constexpr size_t kInputWindowSize = 30;
constexpr uint32_t kNoFrame = 0xFFFFFFFFu;
// Reserved SyncHash tag for the post-load rendezvous; RFC-1982 ordering places frame zero immediately after it.
constexpr uint32_t kLayoutFingerprintFrame = 0xFFFFFFFEu;
constexpr size_t kMaxDatagramSize = 1200;
constexpr uint32_t kReadyAcknowledgesPeer = 1u << 0;
constexpr size_t kHelloPayloadBytes = 12u
    + compat::kCompatibilityIdentityWireBytes;
static_assert(kLayoutFingerprintFrame + 1u == kNoFrame,
              "layout fingerprint must immediately precede no-frame");
static_assert(kInputWindowSize > 0 && kInputWindowSize <= 32,
              "validMask is a 32-bit input-window bitmap");
static_assert(kHeaderSize + kHelloPayloadBytes <= kMaxDatagramSize,
              "full compatibility hello must fit one datagram");

struct SessionCookie {
    uint64_t low = 0;
    uint64_t high = 0;

    bool operator==(const SessionCookie& other) const {
        return low == other.low && high == other.high;
    }
    bool operator!=(const SessionCookie& other) const { return !(*this == other); }
};

enum class MessageType : uint8_t {
    Hello = 1,
    HelloAck = 2,
    Ping = 3,
    Pong = 4,
    Inputs = 5,
    Ready = 6,
    SyncHash = 7,
    SyncHashAck = 8,
    Disconnect = 9,
    DisconnectAck = 10,
    RouteProbe = 11,
    RouteProbeAck = 12,
    SessionEvent = 13,
    RematchChoice = 14,
    Palette = 15,
};

enum class PeerRole : uint8_t {
    Host = 1,
    Client = 2,
};

struct HelloPayload {
    uint64_t compatibilityId = 0;
    PeerRole role = PeerRole::Host;
    uint8_t player = 0;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
};

struct PingPayload {
    uint32_t pingId = 0;
    uint64_t sentMs = 0;
};

struct RouteProbePayload {
    uint32_t sequence = 0;
    uint32_t generation = 0;
    uint64_t stampMicros = 0;
    uint8_t routeId = 0;
    PeerRole role = PeerRole::Host;
};

struct RouteProbeAckPayload {
    uint32_t sequence = 0;
    uint32_t generation = 0;
    uint64_t echoStampMicros = 0;
    uint32_t dwellMicros = 0;
    uint8_t routeId = 0;
    PeerRole role = PeerRole::Host;
};

struct PressureReport {
    uint32_t inputProducedThrough = kNoFrame;
    uint32_t confirmedFrontier = kNoFrame;
    uint16_t predictionDepth = 0;
    uint8_t correctionDebt = 0;
    uint8_t runState = 0;
    uint32_t holdGeneration = 0;
};

// Bit i in validMask describes frame (newestFrame - i), and inputs[i] is that frame's input.
// simFrontier is kNoFrame until the sender's first canonical tick.
struct InputsPayload {
    uint32_t newestFrame = 0;
    uint32_t ackThrough = kNoFrame;
    uint32_t validMask = 0;
    std::array<uint16_t, kInputWindowSize> inputs{};
    uint32_t simFrontier = kNoFrame;
    PressureReport pressure{};
};

constexpr uint8_t kMaxRunStateOrdinal = 8;

struct ReadyPayload {
    uint32_t seedGameplay = 0;
    uint32_t seedCosmetic = 0;
    uint32_t selector = 0;
    uint32_t flags = 0;
    uint64_t bootstrapHash = 0;
    uint32_t timerMode = 0;
    uint32_t aiDifficulty = 0;
    uint32_t lifeMultiplier = 0;
    uint32_t roundsToWinMinusOne = 0;
};

struct SyncHashTag {
    uint32_t layoutGeneration = 0;
    uint32_t epoch = 0;
    uint32_t frame = 0;

    bool operator==(const SyncHashTag& other) const {
        return layoutGeneration == other.layoutGeneration
            && epoch == other.epoch
            && frame == other.frame;
    }
    bool operator!=(const SyncHashTag& other) const {
        return !(*this == other);
    }
};

struct SyncHashPayload {
    SyncHashTag tag{};
    uint32_t rngGameplay = 0;
    uint32_t rngCosmetic = 0;
    uint32_t rngSelector = 0;
    int32_t hp0 = 0;
    int32_t hp1 = 0;
    uint64_t gameplayHash = 0;
};

bool isValidSyncHashPayload(const SyncHashPayload& payload);

struct SyncHashAckPayload {
    SyncHashTag tag{};
};

enum class TerminalErrorCode : uint32_t {
    PeerRequested = 1,
    ProtocolViolation = 2,
    TransportFailure = 3,
    Timeout = 4,
    InternalFailure = 5,
    ConfirmedDesync = 6,
    InvalidConfiguration = 7,
};

const char* terminalErrorCodeName(TerminalErrorCode code);

constexpr size_t kTerminalReasonBytes = 128;
uint32_t terminalReasonId(const char* reason);
bool setTerminalReason(std::array<char, kTerminalReasonBytes>& out,
                       const char* reason);

struct DisconnectPayload {
    TerminalErrorCode code = TerminalErrorCode::PeerRequested;
    uint32_t detail = 0;
    uint32_t reasonId = 0;
    std::array<char, kTerminalReasonBytes> reason{};
};

struct DisconnectAckPayload {
    TerminalErrorCode code = TerminalErrorCode::PeerRequested;
    uint32_t detail = 0;
    uint32_t reasonId = 0;
};

struct Header {
    MessageType type = MessageType::Hello;
    uint8_t flags = 0;
    uint16_t payloadLength = 0;
    uint64_t sessionId = 0;
    SessionCookie cookie{};
    uint32_t sequence = 0;
    uint32_t crc32 = 0;
};

struct Datagram {
    Header header{};
    std::vector<uint8_t> payload;
};

enum class DecodeError : uint8_t {
    None = 0,
    TooShort = 1,
    BadMagic = 2,
    BadVersion = 3,
    BadType = 4,
    BadLength = 5,
    BadFlags = 6,
    SessionMismatch = 7,
    BadCrc = 8,
};

const char* decodeErrorName(DecodeError error);
const char* messageTypeName(MessageType type);

enum class SessionEventStage : uint8_t {
    Request = 1,      // non-host -> host: start kind/value
    Quiesce = 2,      // host -> peer: freeze + report
    Report = 3,       // either -> host: frozen scheduler facts
    Prepare = 4,      // host -> peer: chosen boundary B
    AtBoundary = 5,   // either -> host: exactly-before-B reached
    Commit = 6,       // host -> peer: apply exactly once
    CommitAck = 7,    // peer -> host: applied (stops retransmission)
    Abort = 8,        // host -> peer: recoverable, pre-Prepare only
};

struct SessionEventPayload {
    SessionEventStage stage = SessionEventStage::Request;
    uint8_t kind = 0;
    uint8_t flags = 0;           // bit0 delayPending, bit1 correctionPending
    uint32_t eventId = 0;
    uint32_t value = 0;
    uint32_t boundary = 0;
    uint32_t nextFrame = 0;
    uint32_t assignedTailExclusive = 0;
    uint32_t oldestRestorable = 0;
    uint32_t spectatorFrontier = 0;
    uint32_t lifecycleKey = 0;
};

constexpr uint8_t kRematchChoiceUndecided = 0;
constexpr uint8_t kRematchChoiceRematch = 1;
constexpr uint8_t kRematchChoiceLeave = 2;

// One player's palette: four variants of sixteen BGRX entries, 256 bytes in total.
constexpr size_t kPaletteTableWireBytes = 256;

struct PalettePayload {
    uint32_t epoch = 0;
    uint8_t side = 0;           // sender's QOH slot (0 = P1, 1 = P2)
    uint8_t variant = 0;
    uint8_t present = 0;
    uint8_t reserved = 0;
    uint8_t table[kPaletteTableWireBytes] = {};
};

struct RematchChoicePayload {
    uint32_t epoch = 0;
    uint32_t promptFrame = 0;
    uint8_t side = 0;           // sender's QOH slot (0 = P1, 1 = P2)
    uint8_t choice = kRematchChoiceUndecided;
    uint8_t ready = 0;
    uint8_t reserved = 0;
};

bool isKnownType(MessageType type);
size_t payloadSize(MessageType type);
bool isValidReadyPayload(const ReadyPayload& payload);

bool encodePayload(const HelloPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, HelloPayload& out);
bool encodePayload(const PingPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, PingPayload& out);
bool encodePayload(const RouteProbePayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, RouteProbePayload& out);
bool encodePayload(const RouteProbeAckPayload& payload,
                   std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes,
                   RouteProbeAckPayload& out);
bool encodePayload(const InputsPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, InputsPayload& out);
bool encodePayload(const ReadyPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, ReadyPayload& out);
bool encodePayload(const SyncHashPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, SyncHashPayload& out);
bool encodePayload(const SyncHashAckPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, SyncHashAckPayload& out);
bool encodePayload(const DisconnectPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, DisconnectPayload& out);
bool encodePayload(const DisconnectAckPayload& payload,
                   std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes,
                   DisconnectAckPayload& out);
bool encodePayload(const SessionEventPayload& payload,
                   std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes,
                   SessionEventPayload& out);
bool encodePayload(const RematchChoicePayload& payload,
                   std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes,
                   RematchChoicePayload& out);
bool encodePayload(const PalettePayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, PalettePayload& out);

bool encodeDatagram(MessageType type,
                    uint64_t sessionId,
                    const SessionCookie& cookie,
                    uint32_t sequence,
                    const std::vector<uint8_t>& payload,
                    std::vector<uint8_t>& out);

DecodeError decodeDatagram(const uint8_t* bytes,
                           size_t length,
                           uint64_t expectedSessionId,
                           const SessionCookie& expectedCookie,
                           Datagram& out);

}
