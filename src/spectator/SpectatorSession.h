#pragma once

#include "SpectatorArchive.h"
#include "SpectatorCheckpoint.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <vector>

namespace qnetplay::spectator {

using ViewerId = uint64_t;
using EndpointId = uint64_t;

enum class IngressResult {
    Accepted,
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
    WrongDirection,
    UnexpectedMessage,
    UnknownViewer,
    ContractMismatch,
    GameUnavailable,
    ResumeMismatch,
    CapacityReached,
    InitMismatch,
    StaleAck,
    FutureAck,
    AckDigestMismatch,
    WrongGame,
    ReorderOverflow,
    ConflictingRecord,
    RecordDigestMismatch,
    TimelineMismatch,
    PrematureTerminal,
    TerminalMismatch,
    StateHashMismatch,
    Failed,
    // Append only: existing ordinals are stable.
    ReplayedSequence,
    StaleGeneration,
    EndpointMismatch,
    MigrationRejected,
    RateLimited,
    Detached,
    RouteLost,
    CompatibilityUnattested,
};

enum class SourceRetirementReason {
    None = 0,
    Completed,
    Detached,
    Idle,
    HandshakeDeadline,
    NoProgressDeadline,
    TerminalAckDeadline,
    LagLimit,
    ArchiveUnavailable,
    Removed,
};

struct SourceConfig {
    SessionIdentity identity{};
    uint64_t simulationContract = 0;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
    size_t maxViewers = 32;
    size_t maxPendingRejects = 64;
    size_t recordsPerBatch = 24;
    uint32_t sendWindowRecords = 96;
    size_t perViewerPacketBudget = 4;
    size_t globalPacketBudget = 128;
    uint64_t handshakeResendMs = 250;
    uint64_t retransmitMs = 350;
    uint64_t terminalResendMs = 250;
    uint64_t idleTimeoutMs = 15000;
    size_t controlPacketBudget = 4;
    uint64_t handshakeDeadlineMs = 10000;
    uint64_t noProgressDeadlineMs = 30000;
    uint64_t terminalAckDeadlineMs = 15000;
    uint64_t heartbeatIntervalMs = 1000;
    uint64_t migrationMinIntervalMs = 1000;
    size_t maxMigrationsPerViewer = 8;
    size_t retiredHistoryEntries = 64;
    uint32_t maxUnackedRecords = 8192;
    size_t maxUnackedBytes = 512u * 1024u;
    // The shared viewer cookie must never be the grant MAC key; see SpectatorGrant.h.
    bool requireMigrationGrant = false;
    bool (*migrationGrantVerifier)(void* context,
                                   const MigratePayload& migrate,
                                   uint64_t sessionId) = nullptr;
    void* migrationGrantContext = nullptr;
};

struct SourceDatagram {
    EndpointId destination = 0;
    ViewerId viewer = 0;
    MessageType type = MessageType::Reject;
    uint32_t sequence = 0;
    std::vector<uint8_t> bytes;
};

enum class SourceViewerPhase {
    AwaitingInitAck,
    Streaming,
    AwaitingTerminalAck,
    Complete,
    Retired,
};

struct SourceViewerSnapshot {
    SourceViewerPhase phase = SourceViewerPhase::AwaitingInitAck;
    uint64_t gameId = 0;
    uint32_t acknowledgedNextRecord = 0;
    uint32_t nextNewRecord = 0;
    uint32_t highestSentNextRecord = 0;
    uint64_t acknowledgedDigest = 0;
    uint64_t lastValidReceiveMs = 0;
    ViewerId viewerId = 0;
    EndpointId endpoint = 0;
    uint32_t generation = 0;
    uint32_t acknowledgedNextEvent = 0;
    uint64_t lastProgressMs = 0;
    uint64_t recordRepairSends = 0;
    uint64_t eventRepairSends = 0;
    uint64_t unverifiedAcks = 0;
    uint32_t lastUnverifiedAckRecord = 0;
    uint32_t lastUnverifiedAckEvent = 0;
    uint64_t phaseEnteredMs = 0;
    uint32_t unackedRecords = 0;
    size_t unackedBytes = 0;
    bool caughtUp = false;
    size_t migrations = 0;
    bool retired = false;
    SourceRetirementReason retirementReason = SourceRetirementReason::None;
    bool wantsNewestCheckpoint = false;
    uint64_t offeredCheckpointId = 0;
    uint32_t checkpointChunksSent = 0;
    uint32_t checkpointChunkCount = 0;
    uint32_t checkpointPassesSent = 0;
    uint8_t checkpointPushBlocked = 0;
};

class SpectatorSource {
public:
    SpectatorSource(const SourceConfig& config, SpectatorArchive& archive);

    bool isConfigured() const { return _configured; }

    IngressResult onDatagram(EndpointId endpoint,
                             const uint8_t* bytes,
                             size_t length,
                             uint64_t nowMs);

    std::vector<SourceDatagram> pollOutbound(
        uint64_t nowMs,
        size_t maxPacketsOverride = 0);

    size_t viewerCount() const { return _viewers.size(); }
    bool setViewerWantsNewestCheckpoint(ViewerId viewer);

    bool viewerSnapshot(ViewerId viewer,
                        SourceViewerSnapshot& out) const;
    bool removeViewer(ViewerId viewer);

    void setSourcePhase(SourcePhase phase);
    SourcePhase sourcePhase() const;

    size_t pendingRejectCount() const { return _pendingRejects.size(); }

    uint32_t takeOutboundSequence() { return _nextOutboundSequence++; }

private:
    struct Viewer {
        ViewerId viewerId = 0;
        EndpointId endpoint = 0;
        uint32_t generation = 0;
        SourceViewerPhase phase = SourceViewerPhase::AwaitingInitAck;
        uint64_t gameId = 0;
        uint64_t nonce = 0;
        uint64_t startFingerprint = 0;
        uint32_t handshakeResumeRecord = 0;
        uint32_t handshakeResumeEvent = 0;
        uint64_t handshakeResumeDigest = 0;
        uint32_t acknowledgedNext = 0;
        uint32_t acknowledgedNextEvent = 0;
        uint64_t acknowledgedDigest = 0;
        uint32_t nextNewRecord = 0;
        uint32_t highestSentNext = 0;
        uint32_t nextNewEvent = 0;
        uint32_t highestSentEvent = 0;
        uint64_t lastEventSendMs = 0;
        bool eventSent = false;
        bool wantsNewestCheckpoint = false;
        uint64_t offeredCheckpointId = 0;
        uint32_t nextCheckpointChunk = 0;
        uint32_t checkpointChunkCount = 0;
        uint32_t checkpointTotalBytes = 0;
        uint32_t checkpointPassesSent = 0;
        uint64_t checkpointPassCompleteMs = 0;
        uint8_t checkpointPushBlocked = 0;
        ReplayWindow inbound{};
        uint64_t lastActivityMs = 0;
        uint64_t lastProgressMs = 0;
        uint64_t lastRecordProgressMs = 0;
        uint64_t lastEventProgressMs = 0;
        uint64_t phaseEnteredMs = 0;
        uint64_t lastHandshakeSendMs = 0;
        uint64_t lastDataSendMs = 0;
        uint64_t lastRecordRepairSendMs = 0;
        uint64_t lastEventRepairSendMs = 0;
        uint64_t lastTerminalSendMs = 0;
        uint64_t lastHeartbeatSendMs = 0;
        uint64_t lastMigrationMs = 0;
        size_t migrations = 0;
        bool handshakeSent = false;
        bool dataSent = false;
        bool recordRepairSent = false;
        bool eventRepairSent = false;
        uint8_t duplicateRecordAcks = 0;
        uint8_t duplicateEventAcks = 0;
        bool recordFastRepairPending = false;
        bool eventFastRepairPending = false;
        uint64_t unverifiedAcks = 0;
        uint32_t lastUnverifiedAckRecord = 0;
        uint32_t lastUnverifiedAckEvent = 0;
        uint64_t recordRepairSends = 0;
        uint64_t eventRepairSends = 0;
        bool terminalSent = false;
        bool heartbeatSent = false;
    };

    SourceConfig _config{};
    SpectatorArchive& _archive;
    bool _configured = false;
    std::map<ViewerId, Viewer> _viewers;
    std::deque<SourceDatagram> _pendingRejects;
    std::deque<SourceViewerSnapshot> _retired;
    // ONE monotonic sequence for every source -> viewer datagram: the viewer has a
    // single 64-entry anti-replay window, so independent counters collide on it.
    uint32_t _nextOutboundSequence = 1;
    size_t _controlCursor = 0;
    size_t _dataCursor = 0;
    bool _phaseOverride = false;
    SourcePhase _phase = SourcePhase::Waiting;

    IngressResult handleHello(EndpointId endpoint,
                              const HelloPayload& hello,
                              uint32_t sequence,
                              uint64_t nowMs);
    IngressResult handleMigrate(EndpointId endpoint,
                                const MigratePayload& migrate,
                                uint32_t sequence,
                                uint64_t nowMs);
    IngressResult handleDetach(EndpointId endpoint,
                               const DetachPayload& detach,
                               uint32_t sequence,
                               uint64_t nowMs);
    void queueReject(EndpointId destination,
                     const RejectPayload& reject);
    bool appendEncoded(EndpointId destination,
                       ViewerId viewer,
                       MessageType type,
                       uint32_t& sequence,
                       const std::vector<uint8_t>& payload,
                       std::vector<SourceDatagram>& out);
    SourceViewerSnapshot snapshotOf(const Viewer& viewer) const;
    void armRepairsFromUnverifiedAck(Viewer& viewer, uint32_t reportedRecord,
                                     uint32_t reportedEvent, uint64_t nowMs);

    void retire(std::map<ViewerId, Viewer>::iterator it,
                SourceRetirementReason reason);
    void enterPhase(Viewer& viewer,
                    SourceViewerPhase phase,
                    uint64_t nowMs);
    SourcePhase derivedPhase(const ArchiveExtent& extent,
                             bool hasGame) const;
};

constexpr uint32_t kCheckpointPushPasses = 6u;
constexpr uint64_t kCheckpointPushRepassMs = 500u;

struct ViewerConfig {
    SessionIdentity identity{};
    uint64_t simulationContract = 0;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
    uint64_t viewerNonce = 0;
    uint64_t helloResendMs = 250;
    uint64_t ackResendMs = 100;
    uint64_t terminalAckResendMs = 250;
    uint32_t maxReorderRecords = 512;
    bool wantsNewestCheckpoint = false;
    uint32_t maxRecords =
        (std::numeric_limits<uint32_t>::max)();
    uint64_t secureRandom = 0;
    size_t terminalAckMaxSends = 8;
    bool relayIngestOnly = false;
    uint32_t maxRelayRetainedRecords = 1024;
    uint32_t maxRelayRetainedEvents = 256;
    uint32_t initialSequence = 0;
};

enum class ViewerDatagramRoute : uint8_t {
    ActiveParent = 0,
    ValidatedPendingParent,
};

struct ViewerDatagram {
    MessageType type = MessageType::Hello;
    uint32_t sequence = 0;
    std::vector<uint8_t> bytes;
    ViewerDatagramRoute route = ViewerDatagramRoute::ActiveParent;
    uint64_t parentRouteActivationId = 0u;
};

enum class ViewerPhase {
    Idle,
    Connecting,
    Streaming,
    Complete,
    Rejected,
    Failed,
    Detached,
};

struct VerifiedRelayExportExtent {
    uint32_t firstRecord = 0;
    uint32_t nextRecord = 0;
    uint32_t firstEvent = 0;
    uint32_t nextEvent = 0;
    uint64_t digest = 0;
};

class SpectatorViewer {
public:
    explicit SpectatorViewer(const ViewerConfig& config);

    bool isConfigured() const { return _configured; }

    bool begin(uint64_t requestedGameId = kLatestGameId);

    bool resume();

    bool migrate(uint64_t grantId, uint64_t grantMacLow, uint64_t grantMacHigh);

    bool armValidatedParentRouteTransition(uint64_t activationId,
                                           uint32_t nextRecord,
                                           uint32_t nextEvent,
                                           uint64_t digest);
    bool cancelValidatedParentRouteTransition(uint64_t activationId);
    IngressResult onValidatedParentRouteDatagram(uint64_t activationId,
                                                 const uint8_t* bytes,
                                                 size_t length,
                                                 uint64_t nowMs);
    bool commitValidatedParentRouteIngress(uint64_t activationId,
                                           uint64_t nowMs);

    bool detach();

    bool reportTransportFailure(IngressResult reason);

    IngressResult onDatagram(const uint8_t* bytes,
                             size_t length,
                             uint64_t nowMs);
    std::vector<ViewerDatagram> pollOutbound(
        uint64_t nowMs,
        size_t packetBudget = 4);

    ViewerPhase phase() const { return _phase; }
    IngressResult failure() const { return _failure; }
    uint64_t gameId() const {
        return _hasStart ? _start.gameId : kLatestGameId;
    }
    ViewerId viewerId() const { return _viewerId; }
    uint32_t generation() const { return _generation; }
    uint64_t lastValidReceiveMs() const { return _lastValidReceiveMs; }
    uint32_t receivedNextRecord() const { return _receivedNext; }
    uint64_t receivedDigest() const { return _receivedDigest; }
    uint32_t receivedNextEvent() const { return _appliedEvents; }
    const SpectatorSetTimeline& setTimeline() const { return _setTimeline; }
    uint32_t playbackNextRecord() const { return _playbackNext; }
    size_t bufferedPlaybackRecords() const {
        return _config.relayIngestOnly
            ? 0u
            : static_cast<size_t>(_receivedNext - _playbackNext);
    }
    size_t reorderedRecordCount() const { return _pending.size(); }
    uint32_t awaitingEventIndex() const { return _awaitingEventIndex; }
    uint32_t appliedEventCount() const { return _appliedEvents; }
    bool relayIngestOnly() const { return _config.relayIngestOnly; }

    bool gameStart(GameStartPayload& out) const;
    bool peekPlaybackRecord(ConfirmedRecord& out) const;

    bool verifiedRecordAt(uint32_t record, ConfirmedRecord& out) const;
    bool verifiedSetEventAt(uint32_t event, SetEvent& out) const;
    bool verifiedRelayExportExtent(VerifiedRelayExportExtent& out) const;

    bool sourceStatus(HeartbeatPayload& out) const;
    bool checkpointOffer(CheckpointOfferPayload& out) const;
    bool takeAssembledCheckpoint(Checkpoint& out);
    bool checkpointAssemblyProgress(uint32_t& chunksSeen,
                                    uint32_t& chunkCount) const {
        if (!_hasCheckpointOffer) return false;
        chunksSeen = _checkpointChunksSeen;
        chunkCount = _checkpointOffer.chunkCount;
        return true;
    }
    bool resumeAtCheckpoint(uint32_t recordCursor,
                            uint32_t eventCursor,
                            uint64_t chainDigest,
                            const SpectatorSetTimeline::BindSeed& seed);

    IngressResult commitPlaybackRecord(uint32_t record,
                                       uint64_t observedPreStateHash,
                                       uint64_t observedPostStateHash);

    bool terminal(ArchiveTerminal& out) const;
    bool rejection(RejectPayload& out) const;

private:
    ViewerConfig _config{};
    bool _configured = false;
    ViewerPhase _phase = ViewerPhase::Idle;
    IngressResult _failure = IngressResult::Accepted;
    uint64_t _requestedGameId = kLatestGameId;
    ViewerId _viewerId = 0;
    uint64_t _activeNonce = 0;
    uint32_t _generation = 0;
    uint32_t _helloResumeRecord = 0;
    uint32_t _helloResumeEvent = 0;
    uint64_t _helloResumeDigest = 0;
    uint32_t _nextSequence = 1;
    uint64_t _lastValidReceiveMs = 0;
    ReplayWindow _inbound{};

    struct PendingValidatedParentRoute {
        uint64_t activationId = 0u;
        uint32_t nextRecord = 0u;
        uint32_t nextEvent = 0u;
        uint64_t digest = 0u;
        bool helloSent = false;
        uint64_t lastHelloSendMs = 0u;
        ReplayWindow candidateInbound{};
        bool candidateIngressAccepted = false;
    };
    std::optional<PendingValidatedParentRoute> _pendingValidatedParentRoute;

    bool _hasStart = false;
    GameStartPayload _start{};
    uint64_t _startFingerprint = 0;
    uint32_t _receivedNext = 0;
    uint64_t _receivedDigest = 0;
    uint32_t _playbackNext = 0;
    bool _hasPlaybackPostState = false;
    uint32_t _playbackPostNextRecord = 0;
    uint64_t _playbackPostStateHash = 0;
    std::vector<ConfirmedRecord> _history;
    uint32_t _historyBase = 0;
    bool _hasBoundBase = false;
    uint32_t _boundBaseRecord = 0;
    uint32_t _boundBaseEvent = 0;
    uint64_t _boundBaseDigest = 0;
    std::deque<ConfirmedRecord> _relayRecords;
    std::deque<uint64_t> _relayRecordBaseDigests;
    uint32_t _relayFirstRecord = 0;
    struct PendingRecord {
        ConfirmedRecord record{};
        uint32_t baseEventIndex = 0;
    };
    std::map<uint32_t, PendingRecord> _pending;
    SpectatorSetTimeline _setTimeline;
    uint32_t _appliedEvents = 0;
    uint32_t _awaitingEventIndex = 0;
    std::map<uint32_t, SetEvent> _pendingEvents;
    std::deque<SetEvent> _relayEvents;
    uint32_t _relayFirstEvent = 0;

    bool _helloSent = false;
    uint64_t _lastHelloSendMs = 0;
    bool _initAckDirty = false;
    bool _ackDirty = false;
    bool _ackSent = false;
    uint64_t _lastAckSendMs = 0;
    bool _hasTerminal = false;
    ArchiveTerminal _terminal{};
    bool _terminalAckDirty = false;
    bool _terminalAckSent = false;
    size_t _terminalAckSends = 0;
    uint64_t _lastTerminalAckSendMs = 0;
    bool _hasReject = false;
    RejectPayload _reject{};
    bool _hasHeartbeat = false;
    HeartbeatPayload _heartbeat{};
    bool _hasCheckpointOffer = false;
    CheckpointOfferPayload _checkpointOffer{};
    std::vector<uint8_t> _checkpointBuffer;
    std::vector<bool> _checkpointChunkSeen;
    uint32_t _checkpointChunksSeen = 0;
    bool _hasAssembledCheckpoint = false;
    Checkpoint _assembledCheckpoint{};

    bool _migrateDirty = false;
    uint32_t _migrateOldGeneration = 0;
    uint64_t _migrateGrantId = 0;
    uint64_t _migrateGrantMacLow = 0;
    uint64_t _migrateGrantMacHigh = 0;
    bool _detachDirty = false;

    IngressResult fail(IngressResult reason);
    IngressResult completeTailIfVerified();
    bool appendEncoded(MessageType type,
                       const std::vector<uint8_t>& payload,
                       std::vector<ViewerDatagram>& out,
                       ViewerDatagramRoute route =
                           ViewerDatagramRoute::ActiveParent,
                       uint64_t parentRouteActivationId = 0u);
    void clearValidatedParentRouteTransition();
    void beginGeneration();
    IngressResult drainChain();
    void retainRelayRecord(const ConfirmedRecord& record);
    void retainRelayEvent(const SetEvent& event);
    bool chainDigestAtLocal(uint32_t record,
                            uint32_t eventCount,
                            uint64_t& digest) const;
    bool chainBeforeRecord(uint32_t record, uint64_t& digest) const;
};

}
