// Every integer is explicit little-endian; no object layout crosses the wire.
// CRC32 detects damage only and is not authentication.
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "../compat/CompatibilityIdentity.h"
#include "../netplay/MatchConfig.h"
#include "SpectatorSetArchive.h"

namespace qnetplay::spectator {

// Little-endian bytes spell "QSP1".  This must never overlap a player packet.
constexpr uint32_t kSpectatorMagic = 0x31505351u;
constexpr uint16_t kLegacySpectatorVersion = 3;
constexpr uint16_t kSpectatorVersion = 4;
constexpr size_t kSpectatorHeaderSize = 56;
constexpr size_t kMaxSpectatorDatagramSize = 1200;
// GameStart (56 header + 280 fixed + 224 metadata = 560 bytes) must fit one datagram; there is no truncation path.
constexpr size_t kMaxGameMetadataSize = 512;
constexpr size_t kMaxRecordsPerBatch = 32;
constexpr uint64_t kLatestGameId =
    (std::numeric_limits<uint64_t>::max)();

struct SessionCookie {
    uint64_t low = 0;
    uint64_t high = 0;

    bool operator==(const SessionCookie& other) const {
        return low == other.low && high == other.high;
    }
    bool operator!=(const SessionCookie& other) const {
        return !(*this == other);
    }
};

struct SessionIdentity {
    uint64_t sessionId = 0;
    uint64_t epoch = 0;
    SessionCookie cookie{};

    bool operator==(const SessionIdentity& other) const {
        return sessionId == other.sessionId
            && epoch == other.epoch
            && cookie == other.cookie;
    }
    bool operator!=(const SessionIdentity& other) const {
        return !(*this == other);
    }
};

bool isValidIdentity(const SessionIdentity& identity);

enum class MessageType : uint8_t {
    Hello = 1,
    GameStart = 2,
    InitAck = 3,
    ConfirmedInputBatch = 4,
    Ack = 5,
    Terminal = 6,
    TerminalAck = 7,
    Reject = 8,
    Detach = 9,
    Migrate = 10,
    Heartbeat = 11,
    CheckpointOffer = 12,
    CheckpointChunk = 13,
    SetEvent = 14,
};

enum class Direction : uint8_t {
    ViewerToSource = 1,
    SourceToViewer = 2,
};

enum class PeerRole : uint8_t {
    Source = 1,
    Viewer = 2,
};

enum class SourcePhase : uint8_t {
    Waiting = 1,
    Active = 2,
    Paused = 3,
    Checkpointing = 4,
    Sealed = 5,
    Draining = 6,
    Retained = 7,
    Expired = 8,
    Failed = 9,
};

bool isKnownSourcePhase(SourcePhase phase);

enum class TerminalReason : uint16_t {
    MatchEnded = 1,
    SourceStopped = 2,
    Superseded = 3,
    ProtocolFailure = 4,
};

enum class RejectReason : uint16_t {
    GameUnavailable = 1,
    ResumeMismatch = 2,
    ContractMismatch = 3,
    CapacityReached = 4,
    InvalidRequest = 5,
    ArchiveUnavailable = 6,
    GrantInvalid = 7,
    GrantExpired = 8,
    MigrationRejected = 9,
    RateLimited = 10,
};

// 64-entry sliding anti-replay window over the wrap-safe serial space: bit 0 of
// `mask` is `highest`, bit N is `highest - N`; `mask == 0` means nothing observed.
struct ReplayWindow {
    uint64_t highest = 0;
    uint64_t mask = 0;
};

constexpr size_t kSequenceWindowEntries = 64;

bool sequenceWindowAdmit(ReplayWindow& window, uint32_t sequence);

struct HelloPayload {
    uint64_t simulationContract = 0;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
    uint64_t requestedGameId = kLatestGameId;
    uint64_t viewerNonce = 0;
    uint64_t viewerId = 0;
    uint64_t resumeDigest = 0;
    uint32_t resumeRecord = 0;
    uint32_t generation = 0;
    uint32_t resumeEvent = 0;
    // Bit0: viewer wants the newest full-state checkpoint pushed. Unknown bits are refused.
    uint32_t flags = 0;
};

constexpr uint32_t kHelloFlagWantsNewestCheckpoint = 1u << 0;
constexpr uint32_t kHelloKnownFlags = kHelloFlagWantsNewestCheckpoint;

struct GameStartPayload {
    uint64_t gameId = 0;
    uint64_t simulationContract = 0;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
    uint64_t initialStateHash = 0;
    uint64_t initialDigest = 0;
    uint64_t resumeDigest = 0;
    uint64_t viewerNonce = 0;
    uint64_t viewerId = 0;
    uint32_t resumeRecord = 0;
    uint32_t firstTimelineFrame = 0;
    uint32_t generation = 0;
    uint32_t resumeEvent = 0;
    uint16_t playerOneDelay = 0;
    uint16_t playerTwoDelay = 0;
    uint16_t rollbackWindow = 0;
    std::vector<uint8_t> metadata;
};

uint64_t viewerGenerationNonce(uint64_t viewerId, uint32_t generation);

constexpr uint32_t kReadyMetadataMagic = 0x31504E51u; // "QNP1"
constexpr uint32_t kReadyMetadataVersion = 5u;
constexpr size_t kReadyPlayerNameBytes = 32u;
constexpr size_t kReadyPlayerNameDisplayBytes = 20u;
constexpr size_t kReadySourceLanes = 4u;
constexpr size_t kReadyMetadataFixedBytes = 80u;
constexpr size_t kReadyMetadataBytes =
    kReadyMetadataFixedBytes
    + kReadySourceLanes * sizeof(uint32_t)
    + kReadySourceLanes * kReadyPlayerNameBytes;
constexpr uint32_t kReadyInitialPhase = 0x00010000u;
static_assert(kReadyMetadataBytes == 224u,
              "ready metadata v5 layout is exactly 224 bytes");
static_assert(kReadyMetadataBytes <= kMaxGameMetadataSize,
              "ready metadata must fit one spectator GameStart");
static_assert(kReadyPlayerNameDisplayBytes < kReadyPlayerNameBytes,
              "ready player names require a NUL terminator");

struct ReadyMetadata {
    uint32_t seedGameplay = 0;
    uint32_t seedCosmetic = 0;
    uint32_t selector = 0;
    uint32_t initialPhase = 0;
    uint64_t bootstrapHash = 0;
    uint32_t timerMode = 0;
    uint32_t aiDifficulty = 0;
    uint32_t lifeMultiplier = 0;
    uint32_t roundsToWinMinusOne = 0;
    uint32_t vsRow = 1;   // native VS layout row, 0..5
    uint32_t cpuMask = 0; // native fighter+1400 ownership bits, 4 bits
    // Native four-entry fighter-slot -> logical-source route (dword_4A37C4).
    uint8_t slotToSourceRoute[kReadySourceLanes] = {0, 1, 2, 3};
    // Bit N set = logical source lane N carries an action value and a name.
    uint8_t activeLaneMask = 0x03;
    // Bit N set = native fighter slot N participates.
    uint8_t activeSlotMask = 0x03;
    uint8_t rosterEpochLow = 0;
    uint32_t playerNameCodePage[kReadySourceLanes] = {};
    // Each named lane is a NUL-terminated, zero-padded 1..20-byte display form; an inactive lane is all zero with a zero code page.
    char playerName[kReadySourceLanes][kReadyPlayerNameBytes] = {};
};

bool encodeReadyMetadata(
    const ReadyMetadata& metadata, std::vector<uint8_t>& out);
bool decodeReadyMetadata(
    const std::vector<uint8_t>& bytes, ReadyMetadata& out);

struct InitAckPayload {
    uint64_t gameId = 0;
    uint64_t viewerNonce = 0;
    uint64_t startFingerprint = 0;
    uint64_t resumeDigest = 0;
    uint64_t viewerId = 0;
    uint32_t resumeRecord = 0;
    uint32_t generation = 0;
    uint32_t resumeEvent = 0;
};

struct ConfirmedRecord {
    uint32_t record = 0;
    uint32_t timelineFrame = 0;
    uint16_t playerOneInput = 0;
    uint16_t playerTwoInput = 0;
    uint64_t stateHash = 0;
    uint64_t rollingDigest = 0;

    bool operator==(const ConfirmedRecord& other) const {
        return record == other.record
            && timelineFrame == other.timelineFrame
            && playerOneInput == other.playerOneInput
            && playerTwoInput == other.playerTwoInput
            && stateHash == other.stateHash
            && rollingDigest == other.rollingDigest;
    }
    bool operator!=(const ConfirmedRecord& other) const {
        return !(*this == other);
    }
};

struct ConfirmedInputBatchPayload {
    uint64_t gameId = 0;
    uint32_t startRecord = 0;
    uint64_t baseDigest = 0;
    uint64_t endDigest = 0;
    uint32_t baseEventIndex = 0;
    std::vector<ConfirmedRecord> records;
};

struct AckPayload {
    uint64_t gameId = 0;
    uint64_t digest = 0;
    uint64_t viewerId = 0;
    uint32_t nextRecord = 0;
    uint32_t generation = 0;
    uint32_t nextEvent = 0;
};

struct TerminalPayload {
    uint64_t gameId = 0;
    uint64_t finalDigest = 0;
    uint64_t postTailStateHash = 0;
    uint32_t nextRecord = 0;
    uint32_t nextEvent = 0;
    TerminalReason reason = TerminalReason::MatchEnded;
};

struct TerminalAckPayload {
    uint64_t gameId = 0;
    uint64_t finalDigest = 0;
    uint64_t postTailStateHash = 0;
    uint64_t viewerId = 0;
    uint32_t nextRecord = 0;
    uint32_t generation = 0;
    uint32_t nextEvent = 0;
    TerminalReason reason = TerminalReason::MatchEnded;
};

struct RejectPayload {
    uint64_t requestedGameId = kLatestGameId;
    uint64_t availableGameId = kLatestGameId;
    uint64_t viewerNonce = 0;
    uint64_t viewerId = 0;
    uint32_t availableNextRecord = 0;
    uint32_t generation = 0;
    RejectReason reason = RejectReason::InvalidRequest;
};

struct DetachPayload {
    uint64_t viewerId = 0;
    uint64_t digest = 0;
    uint32_t nextRecord = 0;
    uint32_t generation = 0;
    uint32_t nextEvent = 0;
};

struct MigratePayload {
    uint64_t viewerId = 0;
    uint64_t resumeDigest = 0;
    uint64_t grantId = 0;
    uint64_t grantMacLow = 0;
    uint64_t grantMacHigh = 0;
    uint32_t oldGeneration = 0;
    uint32_t newGeneration = 0;
    uint32_t resumeRecord = 0;
    uint32_t resumeEvent = 0;
};

struct HeartbeatPayload {
    uint64_t gameId = 0;
    uint64_t digest = 0;
    uint32_t nextRecord = 0;
    uint32_t checkpointRecord = 0;
    SourcePhase sourcePhase = SourcePhase::Active;
    uint16_t viewerCount = 0;
    uint32_t nextEvent = 0;
    uint32_t currentMatchId = 0;
};

struct CheckpointOfferPayload {
    uint64_t gameId = 0;
    uint64_t checkpointId = 0;
    uint64_t digest = 0;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
    uint32_t recordCursor = 0;
    uint32_t totalBytes = 0;
    uint32_t chunkCount = 0;
    uint32_t chunkBytes = 0;
};

constexpr size_t kCheckpointChunkFixedSize = 32;
constexpr size_t kMaxCheckpointChunkBytes =
    kMaxSpectatorDatagramSize - kSpectatorHeaderSize
    - kCheckpointChunkFixedSize;

struct CheckpointChunkPayload {
    uint64_t gameId = 0;
    uint64_t checkpointId = 0;
    uint32_t chunkIndex = 0;
    uint32_t chunkCount = 0;
    uint32_t byteOffset = 0;
    std::vector<uint8_t> bytes;
};

// 96 scalar/roster bytes plus the 64-byte colour variant a PaletteSelected
// event carries (zero for every other kind).
constexpr size_t kSetEventWireSize = 96 + kSetPaletteEntryBytes;
constexpr size_t kSetEventBatchFixedSize = 24;
constexpr size_t kMaxSetEventsPerBatch =
    (kMaxSpectatorDatagramSize - kSpectatorHeaderSize
     - kSetEventBatchFixedSize) / kSetEventWireSize;
static_assert(kMaxSetEventsPerBatch >= 1,
              "a set-event batch must carry at least one event");
static_assert(kSpectatorHeaderSize + kSetEventBatchFixedSize
                  + kMaxSetEventsPerBatch * kSetEventWireSize
              <= kMaxSpectatorDatagramSize,
              "a full set-event batch must fit one spectator datagram");

struct SetEventBatchPayload {
    uint64_t gameId = 0;
    uint64_t baseDigest = 0;
    uint32_t startEvent = 0;
    std::vector<SetEvent> events;
};

struct Header {
    uint16_t version = kSpectatorVersion;
    bool compatibilityIdentityAttested = true;
    MessageType type = MessageType::Hello;
    Direction direction = Direction::ViewerToSource;
    PeerRole senderRole = PeerRole::Viewer;
    uint16_t payloadLength = 0;
    SessionIdentity identity{};
    uint32_t sequence = 0;
    uint32_t crc32 = 0;
};

struct Datagram {
    Header header{};
    std::vector<uint8_t> payload;
};

enum class DecodeError {
    None,
    TooShort,
    TooLarge,
    BadMagic,
    BadVersion,
    BadType,
    BadDirectionRole,
    BadHeader,
    BadLength,
    SessionMismatch,
    EpochMismatch,
    CookieMismatch,
    BadCrc,
    BadPayload,
};

bool isKnownMessageType(MessageType type);
Direction expectedDirection(MessageType type);
PeerRole expectedSenderRole(MessageType type);

bool encodePayload(const HelloPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, HelloPayload& out);
bool encodePayload(const GameStartPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, GameStartPayload& out);
bool encodePayload(const InitAckPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, InitAckPayload& out);
bool encodePayload(const ConfirmedInputBatchPayload& payload,
                   std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes,
                   ConfirmedInputBatchPayload& out);
bool encodePayload(const AckPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, AckPayload& out);
bool encodePayload(const TerminalPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, TerminalPayload& out);
bool encodePayload(const TerminalAckPayload& payload,
                   std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes,
                   TerminalAckPayload& out);
bool encodePayload(const RejectPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, RejectPayload& out);
bool encodePayload(const DetachPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, DetachPayload& out);
bool encodePayload(const MigratePayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, MigratePayload& out);
bool encodePayload(const HeartbeatPayload& payload, std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes, HeartbeatPayload& out);
bool encodePayload(const CheckpointOfferPayload& payload,
                   std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes,
                   CheckpointOfferPayload& out);
bool encodePayload(const CheckpointChunkPayload& payload,
                   std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes,
                   CheckpointChunkPayload& out);
bool encodePayload(const SetEventBatchPayload& payload,
                   std::vector<uint8_t>& out);
bool decodePayload(const std::vector<uint8_t>& bytes,
                   SetEventBatchPayload& out);

bool encodeDatagram(MessageType type,
                    const SessionIdentity& identity,
                    uint32_t sequence,
                    const std::vector<uint8_t>& payload,
                    std::vector<uint8_t>& out);

DecodeError decodeDatagram(const uint8_t* bytes,
                           size_t length,
                           const SessionIdentity& expectedIdentity,
                           Datagram& out);

uint32_t datagramChecksum(const uint8_t* bytes, size_t length);

uint64_t computeInitialDigest(const GameStartPayload& start);
uint64_t gameStartFingerprint(const GameStartPayload& start);
uint64_t advanceRollingDigest(uint64_t previous,
                              uint64_t gameId,
                              const ConfirmedRecord& record);

}
