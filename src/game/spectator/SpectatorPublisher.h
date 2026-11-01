#pragma once

#include "game/replay/ReplayIpcWorker.h"
#include "ipc/IpcConfig.h"
#include "spectator/SpectatorProtocol.h"
#include "spectator/SpectatorProtocolV4.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qreplay {
class ReplayRecordPublisherV2;
}

namespace qgame::spectator_publish {

constexpr uint32_t kSidecarMagic = 0x53504351u; // 'QCPS' little-endian
constexpr uint16_t kSidecarSchemaVersion = 2u;
constexpr size_t kSidecarHeaderBytes = 36u;
constexpr uint32_t kMaxSidecarPayloadBytes = 2u * 1024u * 1024u;

enum class SidecarMessageKind : uint16_t {
    Hello = 1,
    Start = 2,
    Confirmed = 3,   // param0 = record count
    Checkpoint = 4,  // param0 = record cursor
    Seal = 5,        // param0 = reason
    // param0 = record cursor (records published so far); carries the post-tail state hash of record cursor-1 without sealing.
    TailProof = 6,
    // param0 = PublishDisableReason.
    PublicationFault = 7,
    // Kinds 1..7 must stay byte-identical for the QSP3 service; QSP4 records are additive.
    ConnectedDescriptorV4 = 8,
    ConnectedPublicationV4 = 9,
    // param0 = 1; payload is a one-event SetEventBatchPayload, ordered by cursor among Confirmed batches.
    ChainedSetEvent = 10,
};

enum class PublishDisableReason : uint16_t {
    None = 0,
    RingOverflow,
    CheckpointOverflow,
    ChannelFailed,
    Stopped,
    InvalidRecord,
};

const char* publishDisableReasonText(PublishDisableReason reason);

struct SidecarStartInfo {
    uint64_t gameId = 0u;
    uint64_t simulationContract = 0u;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity{};
    uint64_t initialStateHash = 0u;
    uint64_t initialDigest = 0u;
    uint32_t firstTimelineFrame = 0u;
    uint16_t playerOneDelay = 0u;
    uint16_t playerTwoDelay = 0u;
    uint16_t rollbackWindow = 0u;
};

constexpr size_t kConfirmedRingCapacity = 8192u;

struct SidecarPublisherOptions {
    size_t checkpointSlots = 2u;
    size_t checkpointSlotBytes = 6u * 1024u * 1024u;
    size_t drainPerTurn = 512u;
    uint32_t connectTimeoutMs = 2000u;
    uint32_t writeTimeoutMs = 1000u;
    uint32_t maxReconnectAttempts = 8u;
    uint32_t reconnectDeadlineMs = 6000u;
    uint32_t reconnectBackoffMs = 25u;
    uint32_t idleSleepMs = 1u;
    uint32_t tailProofIdleMs = 40u;
    bool inlineWorker = false;
};

struct SidecarPublisherStats {
    uint8_t active = 0u;
    uint8_t connected = 0u;
    uint8_t disabled = 0u;
    uint16_t disableReason = 0u;
    uint64_t published = 0u;
    uint64_t dropped = 0u;
    uint64_t written = 0u;
    uint64_t checkpointsPublished = 0u;
    uint64_t checkpointsDropped = 0u;
    uint64_t checkpointsRejected = 0u;
    uint32_t ringHighWater = 0u;
    uint8_t sealed = 0u;
    uint8_t faulted = 0u;
    uint8_t faultSent = 0u;
    uint32_t terminalRecordCursor = 0u;
    uint64_t terminalPrefixDigest = 0u;
    uint64_t tailProofsWritten = 0u;
    uint64_t connectedPublicationsWritten = 0u;
    uint32_t connectedNextItemSequence = 0u;
    uint32_t connectedNextRecordCursor = 0u;
    uint8_t connectedEnabled = 0u;
    uint8_t connectedSealed = 0u;
};

bool startSidecarPublisher(const qipc::IpcConfig& cfg);

bool startClientPrivateSidecarPublisher(const qipc::IpcConfig& cfg);

void stopSidecarPublisher();

// A false return disables publication for this session; the caller must ignore it and continue the match unchanged.
bool publishConfirmed(const qnetplay::spectator::ConfirmedRecord& record);

bool publishChainedSetEvent(qnetplay::spectator::SetEvent event);

bool publishStateCheckpoint(const uint8_t* snapshot,
                            size_t snapshotBytes,
                            uint32_t regionSchema,
                            uint32_t regionSchemaVersion,
                            uint64_t stateHash,
                            const uint8_t* bootDescriptor,
                            size_t bootDescriptorBytes,
                            const uint8_t* audioRegion = nullptr,
                            size_t audioRegionBytes = 0u);

bool publishCheckpoint(uint32_t record, const void* bytes, size_t len);

void publishSeal(uint16_t reason, uint64_t postTailStateHash);

void publishFault(PublishDisableReason reason,
                  uint64_t postTailStateHash = 0u);

void publishTailProof(uint32_t recordCursor, uint64_t postTailStateHash);

bool sidecarPublisherActive();

bool publishStart(const SidecarStartInfo& info, const void* metadata,
                  size_t metadataLen);

SidecarPublisherStats sidecarStats();

bool startSidecarPublisherForTest(const qipc::IpcConfig& cfg,
                                  const SidecarPublisherOptions& options,
                                  ByteChannel* channel);
bool startClientPrivateSidecarPublisherForTest(
    const qipc::IpcConfig& cfg, const SidecarPublisherOptions& options,
    ByteChannel* channel);
void pumpSidecarForTest();

qreplay::ReplayRecordPublisherV2* connectedReplayPublisherV4();
bool connectedReplayPublisherReadyV4();
bool publishConnectedOutOfGameV4(
    uint32_t sourceTimelineFrame,
    const qreplay::ConfirmedGameFrameCoreV2& frame);
bool publishConnectedSetEventV4(
    const qnetplay::spectator::SetEventPayloadV4& event);
bool publishConnectedSetSealV4(
    qnetplay::spectator::ConnectedSetTerminalReasonV4 reason,
    uint64_t postTailStateHash);
bool popConnectedAcceptedEndAckV4(qreplay::GameCommitAckV2& out);

// One slot is reserved for an open game's End and one for SetSeal: ordinary/Begin need >2 free, End needs >1, Seal may consume the last slot.

constexpr size_t kConnectedPublisherMinCapacityV4 = 4u;
constexpr size_t kConnectedPublisherMaxCapacityV4 = 8192u;
constexpr size_t kConnectedPublisherDefaultCapacityV4 = 1024u;

enum class ConnectedPublishResultV4 : uint8_t {
    Accepted,
    InvalidArgument,
    OrderViolation,
    Overflow,
    Disabled,
};

struct ConnectedPublisherStatsV4 {
    uint64_t accepted = 0u;
    uint64_t consumed = 0u;
    uint64_t rejected = 0u;
    uint32_t highWater = 0u;
    bool overflowLatched = false;
    bool gameOpen = false;
    bool sealQueued = false;
};

class ConnectedPublisherQueueV4 {
public:
    explicit ConnectedPublisherQueueV4(
        size_t capacity = kConnectedPublisherDefaultCapacityV4);

    bool valid() const { return valid_; }
    ConnectedPublishResultV4 publish(
        const qnetplay::spectator::ConnectedPublicationV4& publication);
    bool pop(qnetplay::spectator::ConnectedPublicationV4& out);
    void reset();
    ConnectedPublisherStatsV4 stats() const;
    size_t capacity() const { return slots_.size(); }

private:
    std::vector<qnetplay::spectator::ConnectedPublicationV4> slots_;
    std::atomic<uint64_t> read_{0u};
    std::atomic<uint64_t> write_{0u};
    std::atomic<uint64_t> accepted_{0u};
    std::atomic<uint64_t> consumed_{0u};
    std::atomic<uint64_t> rejected_{0u};
    std::atomic<uint32_t> highWater_{0u};
    std::atomic<bool> overflowLatched_{false};
    std::atomic<bool> gameOpen_{false};
    std::atomic<bool> sealQueued_{false};
    bool valid_ = false;
};

}
