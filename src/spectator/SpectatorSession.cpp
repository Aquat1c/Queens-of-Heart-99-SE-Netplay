#include "SpectatorSession.h"

#include "../net/FrameArithmetic.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <utility>

namespace qnetplay::spectator {
namespace {

IngressResult mapDecodeError(DecodeError error) {
    switch (error) {
        case DecodeError::None:             return IngressResult::Accepted;
        case DecodeError::TooShort:         return IngressResult::TooShort;
        case DecodeError::TooLarge:         return IngressResult::TooLarge;
        case DecodeError::BadMagic:         return IngressResult::BadMagic;
        case DecodeError::BadVersion:       return IngressResult::BadVersion;
        case DecodeError::BadType:          return IngressResult::BadType;
        case DecodeError::BadDirectionRole: return IngressResult::BadDirectionRole;
        case DecodeError::BadHeader:        return IngressResult::BadHeader;
        case DecodeError::BadLength:        return IngressResult::BadLength;
        case DecodeError::SessionMismatch:  return IngressResult::SessionMismatch;
        case DecodeError::EpochMismatch:    return IngressResult::EpochMismatch;
        case DecodeError::CookieMismatch:   return IngressResult::CookieMismatch;
        case DecodeError::BadCrc:           return IngressResult::BadCrc;
        case DecodeError::BadPayload:       return IngressResult::BadPayload;
    }
    return IngressResult::BadPayload;
}

bool intervalElapsed(uint64_t now, uint64_t then, uint64_t interval) {
    return now >= then && now - then >= interval;
}

bool sameImmutableStart(const GameStartPayload& left,
                        const GameStartPayload& right) {
    return left.gameId == right.gameId
        && left.simulationContract == right.simulationContract
        && compat::semanticallyEqual(
            left.compatibilityIdentity, right.compatibilityIdentity)
        && left.initialStateHash == right.initialStateHash
        && left.initialDigest == right.initialDigest
        && left.firstTimelineFrame == right.firstTimelineFrame
        && left.playerOneDelay == right.playerOneDelay
        && left.playerTwoDelay == right.playerTwoDelay
        && left.rollbackWindow == right.rollbackWindow
        && left.metadata == right.metadata;
}

bool sameTerminal(const ArchiveTerminal& left,
                  const ArchiveTerminal& right) {
    return left == right;
}

constexpr uint32_t kMaxPendingSetEvents = 256u;

size_t saturatingMul(size_t left, size_t right) {
    if (left != 0
        && right > (std::numeric_limits<size_t>::max)() / left) {
        return (std::numeric_limits<size_t>::max)();
    }
    return left * right;
}

size_t saturatingAddSize(size_t left, size_t right) {
    if (left > (std::numeric_limits<size_t>::max)() - right)
        return (std::numeric_limits<size_t>::max)();
    return left + right;
}

uint32_t saturatingAdd(uint32_t value, uint32_t increment) {
    if (value > (std::numeric_limits<uint32_t>::max)() - increment)
        return (std::numeric_limits<uint32_t>::max)();
    return value + increment;
}

}

SpectatorSource::SpectatorSource(const SourceConfig& config,
                                 SpectatorArchive& archive)
    : _config(config),
      _archive(archive),
      _configured(
          isValidIdentity(config.identity)
          && config.simulationContract != 0
          && compat::isValidCompatibilityIdentity(
              config.compatibilityIdentity)
          && config.maxViewers > 0
          && config.maxPendingRejects > 0
          && config.recordsPerBatch > 0
          && config.recordsPerBatch <= kMaxRecordsPerBatch
          && config.sendWindowRecords > 0
          && config.perViewerPacketBudget > 0
          && config.globalPacketBudget > 0
          && config.handshakeResendMs > 0
          && config.retransmitMs > 0
          && config.terminalResendMs > 0
          && config.idleTimeoutMs > 0
          && config.controlPacketBudget > 0
          && config.handshakeDeadlineMs > 0
          && config.noProgressDeadlineMs > 0
          && config.terminalAckDeadlineMs > 0
          && config.heartbeatIntervalMs > 0
          && config.maxUnackedRecords > 0
          && config.maxUnackedBytes >= sizeof(ConfirmedRecord)
          && config.retiredHistoryEntries > 0
          && archive.isConfigured()) {
}

void SpectatorSource::setSourcePhase(SourcePhase phase) {
    if (!isKnownSourcePhase(phase)) return;
    _phaseOverride = true;
    _phase = phase;
}

SourcePhase SpectatorSource::sourcePhase() const {
    if (_phaseOverride) return _phase;
    uint64_t latest = kLatestGameId;
    ArchiveExtent extent{};
    if (!_archive.latestGameId(latest) || !_archive.extent(latest, extent))
        return SourcePhase::Waiting;
    return extent.sealed ? SourcePhase::Sealed : SourcePhase::Active;
}

SourcePhase SpectatorSource::derivedPhase(const ArchiveExtent& extent,
                                          bool hasGame) const {
    if (_phaseOverride) return _phase;
    if (!hasGame) return SourcePhase::Waiting;
    return extent.sealed ? SourcePhase::Sealed : SourcePhase::Active;
}

bool SpectatorSource::appendEncoded(
    EndpointId destination,
    ViewerId viewer,
    MessageType type,
    uint32_t& sequence,
    const std::vector<uint8_t>& payload,
    std::vector<SourceDatagram>& out) {
    SourceDatagram packet{};
    packet.destination = destination;
    packet.viewer = viewer;
    packet.type = type;
    packet.sequence = sequence;
    if (!encodeDatagram(type, _config.identity, sequence,
                        payload, packet.bytes)) {
        return false;
    }
    ++sequence;
    out.push_back(std::move(packet));
    return true;
}

void SpectatorSource::queueReject(EndpointId destination,
                                  const RejectPayload& reject) {
    std::vector<uint8_t> payload;
    if (!encodePayload(reject, payload)) return;
    std::vector<SourceDatagram> packets;
    if (!appendEncoded(destination, reject.viewerId, MessageType::Reject,
                       _nextOutboundSequence, payload, packets)) {
        return;
    }
    if (_pendingRejects.size() >= _config.maxPendingRejects)
        _pendingRejects.pop_front();
    _pendingRejects.push_back(std::move(packets.front()));
}

SourceViewerSnapshot SpectatorSource::snapshotOf(const Viewer& viewer) const {
    SourceViewerSnapshot snapshot{};
    snapshot.phase = viewer.phase;
    snapshot.gameId = viewer.gameId;
    snapshot.acknowledgedNextRecord = viewer.acknowledgedNext;
    snapshot.nextNewRecord = viewer.nextNewRecord;
    snapshot.highestSentNextRecord = viewer.highestSentNext;
    snapshot.acknowledgedDigest = viewer.acknowledgedDigest;
    snapshot.lastValidReceiveMs = viewer.lastActivityMs;
    snapshot.viewerId = viewer.viewerId;
    snapshot.endpoint = viewer.endpoint;
    snapshot.generation = viewer.generation;
    snapshot.acknowledgedNextEvent = viewer.acknowledgedNextEvent;
    snapshot.wantsNewestCheckpoint = viewer.wantsNewestCheckpoint;
    snapshot.offeredCheckpointId = viewer.offeredCheckpointId;
    snapshot.checkpointChunksSent = viewer.nextCheckpointChunk;
    snapshot.checkpointPassesSent = viewer.checkpointPassesSent;
    snapshot.checkpointChunkCount = viewer.checkpointChunkCount;
    snapshot.checkpointPushBlocked = viewer.checkpointPushBlocked;
    snapshot.lastProgressMs = viewer.lastProgressMs;
    snapshot.recordRepairSends = viewer.recordRepairSends;
    snapshot.eventRepairSends = viewer.eventRepairSends;
    snapshot.unverifiedAcks = viewer.unverifiedAcks;
    snapshot.lastUnverifiedAckRecord = viewer.lastUnverifiedAckRecord;
    snapshot.lastUnverifiedAckEvent = viewer.lastUnverifiedAckEvent;
    snapshot.phaseEnteredMs = viewer.phaseEnteredMs;
    ArchiveExtent extent{};
    if (_archive.extent(viewer.gameId, extent)) {
        snapshot.caughtUp =
            viewer.acknowledgedNext >= extent.nextRecord
            && viewer.acknowledgedNextEvent >= extent.nextEvent;
        snapshot.unackedRecords = viewer.acknowledgedNext < extent.nextRecord
            ? extent.nextRecord - viewer.acknowledgedNext
            : 0u;
        const size_t unackedEvents =
            viewer.acknowledgedNextEvent < extent.nextEvent
                ? static_cast<size_t>(
                      extent.nextEvent - viewer.acknowledgedNextEvent)
                : 0u;
        snapshot.unackedBytes = saturatingAddSize(
            saturatingMul(
                static_cast<size_t>(snapshot.unackedRecords),
                sizeof(ConfirmedRecord)),
            saturatingMul(unackedEvents, sizeof(SetEvent)));
    }
    snapshot.migrations = viewer.migrations;
    snapshot.retired = false;
    snapshot.retirementReason = SourceRetirementReason::None;
    return snapshot;
}

void SpectatorSource::enterPhase(Viewer& viewer,
                                 SourceViewerPhase phase,
                                 uint64_t nowMs) {
    viewer.phase = phase;
    viewer.phaseEnteredMs = nowMs;
}

void SpectatorSource::armRepairsFromUnverifiedAck(
    Viewer& viewer, const uint32_t reportedRecord,
    const uint32_t reportedEvent, const uint64_t nowMs) {
    ++viewer.unverifiedAcks;
    viewer.lastUnverifiedAckRecord = reportedRecord;
    viewer.lastUnverifiedAckEvent = reportedEvent;
    if (reportedEvent <= viewer.highestSentEvent
        && reportedEvent < viewer.nextNewEvent) {
        viewer.eventFastRepairPending = true;
        viewer.duplicateEventAcks = 3u;
    }
    if (reportedRecord <= viewer.highestSentNext
        && reportedRecord < viewer.nextNewRecord) {
        viewer.recordFastRepairPending = true;
        viewer.duplicateRecordAcks = 3u;
    }
    viewer.lastActivityMs = nowMs;
}

void SpectatorSource::retire(std::map<ViewerId, Viewer>::iterator it,
                             SourceRetirementReason reason) {
    if (it == _viewers.end()) return;
    SourceViewerSnapshot snapshot = snapshotOf(it->second);
    snapshot.phase = SourceViewerPhase::Retired;
    snapshot.retired = true;
    snapshot.retirementReason = reason;
    try {
        _retired.push_back(snapshot);
        while (_retired.size() > _config.retiredHistoryEntries)
            _retired.pop_front();
    } catch (...) {
    }
    _viewers.erase(it);
}

IngressResult SpectatorSource::handleHello(EndpointId endpoint,
                                           const HelloPayload& hello,
                                           uint32_t sequence,
                                           uint64_t nowMs) {
    if (hello.viewerNonce
        != viewerGenerationNonce(hello.viewerId, hello.generation)) {
        return IngressResult::InitMismatch;
    }

    uint64_t latest = kLatestGameId;
    ArchiveExtent latestExtent{};
    const bool hasLatest = _archive.latestGameId(latest)
        && _archive.extent(latest, latestExtent);

    auto reject = [&](RejectReason reason,
                      IngressResult result) -> IngressResult {
        RejectPayload payload{};
        payload.requestedGameId = hello.requestedGameId;
        payload.availableGameId = hasLatest ? latest : kLatestGameId;
        payload.viewerNonce = hello.viewerNonce;
        payload.viewerId = hello.viewerId;
        payload.generation = hello.generation;
        payload.availableNextRecord =
            hasLatest ? latestExtent.nextRecord : 0;
        payload.reason = reason;
        queueReject(endpoint, payload);
        return result;
    };

    if (hello.simulationContract != _config.simulationContract) {
        return reject(RejectReason::ContractMismatch,
                      IngressResult::ContractMismatch);
    }
    if (!compat::semanticallyEqual(
            _config.compatibilityIdentity, hello.compatibilityIdentity)) {
        return reject(RejectReason::ContractMismatch,
                      IngressResult::ContractMismatch);
    }

    const auto existing = _viewers.find(hello.viewerId);
    const bool known = existing != _viewers.end();
    if (known) {
        const Viewer& viewer = existing->second;
        if (viewer.endpoint != endpoint) {
            return IngressResult::MigrationRejected;
        }
        if (qnet::frameAfter(viewer.generation, hello.generation)) {
            return IngressResult::StaleGeneration;
        }
    }
    const bool sameGeneration =
        known && existing->second.generation == hello.generation;

    uint64_t gameId = hello.requestedGameId;
    if (sameGeneration) {
        gameId = existing->second.gameId;
    } else if (gameId == kLatestGameId) {
        if (!hasLatest) {
            return IngressResult::Accepted;
        }
        gameId = latest;
    }

    GameDescriptor descriptor{};
    ArchiveExtent extent{};
    if (!_archive.descriptor(gameId, descriptor)
        || !_archive.extent(gameId, extent)) {
        return reject(RejectReason::GameUnavailable,
                      IngressResult::GameUnavailable);
    }
    if (descriptor.simulationContract != _config.simulationContract) {
        return reject(RejectReason::ContractMismatch,
                      IngressResult::ContractMismatch);
    }
    if (!compat::semanticallyEqual(_config.compatibilityIdentity,
                                   descriptor.compatibilityIdentity)) {
        return reject(RejectReason::ContractMismatch,
                      IngressResult::ContractMismatch);
    }
    if (hello.resumeRecord > extent.nextRecord
        || hello.resumeRecord < extent.firstRecord) {
        return reject(RejectReason::ResumeMismatch,
                      IngressResult::ResumeMismatch);
    }

    uint64_t selectedDigest = 0;
    if (!_archive.chainDigestAt(gameId, hello.resumeRecord,
                                hello.resumeEvent, selectedDigest)) {
        return reject(RejectReason::ResumeMismatch,
                      IngressResult::ResumeMismatch);
    }
    const bool allowSeedlessZero = (!known || sameGeneration)
        && hello.resumeRecord == 0
        && hello.resumeEvent == 0
        && hello.resumeDigest == 0;
    if (!(allowSeedlessZero || hello.resumeDigest == selectedDigest)) {
        return reject(RejectReason::ResumeMismatch,
                      IngressResult::ResumeMismatch);
    }

    const GameStartPayload canonical = canonicalGameStart(descriptor);
    const uint64_t startFingerprint = gameStartFingerprint(canonical);

    if (known) {
        Viewer& viewer = existing->second;
        if (!sequenceWindowAdmit(viewer.inbound, sequence))
            return IngressResult::ReplayedSequence;
        if (sameGeneration) {
            if (viewer.startFingerprint != startFingerprint
                || viewer.nonce != hello.viewerNonce
                || hello.resumeRecord != viewer.handshakeResumeRecord
                || hello.resumeEvent != viewer.handshakeResumeEvent
                || selectedDigest != viewer.handshakeResumeDigest) {
                return reject(RejectReason::ResumeMismatch,
                              IngressResult::ResumeMismatch);
            }
            viewer.lastActivityMs = nowMs;
            viewer.wantsNewestCheckpoint =
                viewer.wantsNewestCheckpoint
                || (hello.flags & kHelloFlagWantsNewestCheckpoint) != 0;
            if (viewer.phase == SourceViewerPhase::AwaitingInitAck)
                viewer.handshakeSent = false;
            return IngressResult::Accepted;
        }

        viewer.generation = hello.generation;
        viewer.gameId = gameId;
        viewer.nonce = hello.viewerNonce;
        viewer.startFingerprint = startFingerprint;
        viewer.wantsNewestCheckpoint =
            (hello.flags & kHelloFlagWantsNewestCheckpoint) != 0;
        viewer.offeredCheckpointId = 0;
        viewer.nextCheckpointChunk = 0;
        viewer.checkpointPassesSent = 0;
        viewer.checkpointPassCompleteMs = 0;
        viewer.handshakeResumeRecord = hello.resumeRecord;
        viewer.handshakeResumeEvent = hello.resumeEvent;
        viewer.handshakeResumeDigest = selectedDigest;
        viewer.acknowledgedNext = hello.resumeRecord;
        viewer.acknowledgedNextEvent = hello.resumeEvent;
        viewer.acknowledgedDigest = selectedDigest;
        viewer.nextNewRecord = hello.resumeRecord;
        viewer.highestSentNext =
            std::max(viewer.highestSentNext, hello.resumeRecord);
        viewer.nextNewEvent = hello.resumeEvent;
        viewer.highestSentEvent =
            std::max(viewer.highestSentEvent, hello.resumeEvent);
        viewer.handshakeSent = false;
        viewer.dataSent = false;
        viewer.terminalSent = false;
        viewer.heartbeatSent = false;
        viewer.eventSent = false;
        enterPhase(viewer, SourceViewerPhase::AwaitingInitAck, nowMs);
        viewer.lastActivityMs = nowMs;
        viewer.lastProgressMs = nowMs;
        viewer.lastRecordProgressMs = nowMs;
        viewer.lastEventProgressMs = nowMs;
        viewer.recordRepairSent = false;
        viewer.eventRepairSent = false;
        viewer.duplicateRecordAcks = 0;
        viewer.duplicateEventAcks = 0;
        viewer.recordFastRepairPending = false;
        viewer.eventFastRepairPending = false;
        viewer.recordRepairSends = 0;
        viewer.eventRepairSends = 0;
        return IngressResult::Accepted;
    }

    if (_viewers.size() >= _config.maxViewers) {
        return reject(RejectReason::CapacityReached,
                      IngressResult::CapacityReached);
    }

    Viewer viewer{};
    viewer.viewerId = hello.viewerId;
    viewer.endpoint = endpoint;
    viewer.generation = hello.generation;
    viewer.phase = SourceViewerPhase::AwaitingInitAck;
    viewer.gameId = gameId;
    viewer.nonce = hello.viewerNonce;
    viewer.startFingerprint = startFingerprint;
    viewer.wantsNewestCheckpoint =
        (hello.flags & kHelloFlagWantsNewestCheckpoint) != 0;
    viewer.offeredCheckpointId = 0;
    viewer.nextCheckpointChunk = 0;
    viewer.checkpointPassesSent = 0;
    viewer.checkpointPassCompleteMs = 0;
    viewer.handshakeResumeRecord = hello.resumeRecord;
    viewer.handshakeResumeEvent = hello.resumeEvent;
    viewer.handshakeResumeDigest = selectedDigest;
    viewer.acknowledgedNext = hello.resumeRecord;
    viewer.acknowledgedNextEvent = hello.resumeEvent;
    viewer.acknowledgedDigest = selectedDigest;
    viewer.nextNewRecord = hello.resumeRecord;
    viewer.highestSentNext = hello.resumeRecord;
    viewer.nextNewEvent = hello.resumeEvent;
    viewer.highestSentEvent = hello.resumeEvent;
    viewer.lastActivityMs = nowMs;
    viewer.lastProgressMs = nowMs;
    viewer.lastRecordProgressMs = nowMs;
    viewer.lastEventProgressMs = nowMs;
    viewer.recordRepairSent = false;
    viewer.eventRepairSent = false;
    viewer.duplicateRecordAcks = 0;
    viewer.duplicateEventAcks = 0;
    viewer.recordFastRepairPending = false;
    viewer.eventFastRepairPending = false;
    viewer.phaseEnteredMs = nowMs;
    sequenceWindowAdmit(viewer.inbound, sequence);
    _viewers[hello.viewerId] = viewer;
    return IngressResult::Accepted;
}

IngressResult SpectatorSource::handleDetach(EndpointId endpoint,
                                            const DetachPayload& detach,
                                            uint32_t sequence,
                                            uint64_t nowMs) {
    (void)nowMs;
    const auto found = _viewers.find(detach.viewerId);
    if (found == _viewers.end()) return IngressResult::UnknownViewer;
    Viewer& viewer = found->second;
    if (viewer.endpoint != endpoint) return IngressResult::EndpointMismatch;
    if (detach.generation != viewer.generation)
        return IngressResult::StaleGeneration;

    const bool seedlessInitialDetach =
        detach.nextRecord == 0u && detach.nextEvent == 0u
        && detach.digest == 0u
        && viewer.phase == SourceViewerPhase::AwaitingInitAck
        && viewer.handshakeResumeRecord == 0u
        && viewer.handshakeResumeEvent == 0u
        && viewer.acknowledgedNext == 0u
        && viewer.acknowledgedNextEvent == 0u
        && viewer.highestSentNext == 0u
        && viewer.highestSentEvent == 0u
        && !viewer.dataSent;
    if (!seedlessInitialDetach) {
        ArchiveExtent extent{};
        if (!_archive.extent(viewer.gameId, extent))
            return IngressResult::GameUnavailable;
        if (detach.nextRecord > extent.nextRecord
            || detach.nextRecord < extent.firstRecord
            || detach.nextEvent > extent.nextEvent) {
            return IngressResult::ResumeMismatch;
        }
        uint64_t expected = 0;
        if (!_archive.chainDigestAt(viewer.gameId, detach.nextRecord,
                                    detach.nextEvent, expected)) {
            return IngressResult::ResumeMismatch;
        }
        if (detach.digest != expected) return IngressResult::ResumeMismatch;
    }
    if (!sequenceWindowAdmit(viewer.inbound, sequence))
        return IngressResult::ReplayedSequence;

    retire(found, SourceRetirementReason::Detached);
    return IngressResult::Detached;
}

IngressResult SpectatorSource::handleMigrate(EndpointId endpoint,
                                             const MigratePayload& migrate,
                                             uint32_t sequence,
                                             uint64_t nowMs) {
    const auto found = _viewers.find(migrate.viewerId);
    if (found == _viewers.end()) return IngressResult::UnknownViewer;
    Viewer& viewer = found->second;

    auto rejectMigration = [&](RejectReason reason,
                               IngressResult result) -> IngressResult {
        RejectPayload payload{};
        payload.requestedGameId = viewer.gameId;
        payload.availableGameId = viewer.gameId;
        payload.viewerId = migrate.viewerId;
        payload.generation = migrate.newGeneration;
        payload.viewerNonce = viewerGenerationNonce(
            migrate.viewerId, migrate.newGeneration);
        payload.reason = reason;
        queueReject(endpoint, payload);
        return result;
    };

    if (migrate.oldGeneration != viewer.generation) {
        return qnet::frameAfter(viewer.generation, migrate.oldGeneration)
            ? IngressResult::StaleGeneration
            : IngressResult::MigrationRejected;
    }
    if (!qnet::frameAfter(migrate.newGeneration, viewer.generation))
        return IngressResult::MigrationRejected;

    if (viewer.migrations >= _config.maxMigrationsPerViewer) {
        return rejectMigration(RejectReason::RateLimited,
                               IngressResult::RateLimited);
    }
    if (viewer.migrations > 0
        && !intervalElapsed(nowMs, viewer.lastMigrationMs,
                            _config.migrationMinIntervalMs)) {
        return rejectMigration(RejectReason::RateLimited,
                               IngressResult::RateLimited);
    }
    if (_config.requireMigrationGrant) {
        if (!_config.migrationGrantVerifier
            || !_config.migrationGrantVerifier(
                   _config.migrationGrantContext, migrate,
                   _config.identity.sessionId)) {
            return rejectMigration(RejectReason::GrantInvalid,
                                   IngressResult::MigrationRejected);
        }
    }

    ArchiveExtent extent{};
    if (!_archive.extent(viewer.gameId, extent))
        return IngressResult::GameUnavailable;
    if (migrate.resumeRecord > extent.nextRecord
        || migrate.resumeRecord < extent.firstRecord
        || migrate.resumeRecord > viewer.highestSentNext
        || migrate.resumeEvent > extent.nextEvent
        || migrate.resumeEvent > viewer.highestSentEvent) {
        return rejectMigration(RejectReason::MigrationRejected,
                               IngressResult::MigrationRejected);
    }
    uint64_t expected = 0;
    if (!_archive.chainDigestAt(viewer.gameId, migrate.resumeRecord,
                                migrate.resumeEvent, expected)) {
        return rejectMigration(RejectReason::MigrationRejected,
                               IngressResult::MigrationRejected);
    }
    if (migrate.resumeDigest != expected) {
        return rejectMigration(RejectReason::MigrationRejected,
                               IngressResult::MigrationRejected);
    }

    viewer.endpoint = endpoint;
    viewer.generation = migrate.newGeneration;
    viewer.nonce = viewerGenerationNonce(
        viewer.viewerId, migrate.newGeneration);
    viewer.handshakeResumeRecord = migrate.resumeRecord;
    viewer.handshakeResumeEvent = migrate.resumeEvent;
    viewer.handshakeResumeDigest = migrate.resumeDigest;
    viewer.acknowledgedNext = migrate.resumeRecord;
    viewer.acknowledgedNextEvent = migrate.resumeEvent;
    viewer.acknowledgedDigest = migrate.resumeDigest;
    viewer.nextNewRecord = migrate.resumeRecord;
    viewer.highestSentNext =
        std::max(viewer.highestSentNext, migrate.resumeRecord);
    viewer.nextNewEvent = migrate.resumeEvent;
    viewer.handshakeSent = false;
    viewer.dataSent = false;
    viewer.terminalSent = false;
    viewer.heartbeatSent = false;
    viewer.eventSent = false;
    viewer.inbound = ReplayWindow{};
    sequenceWindowAdmit(viewer.inbound, sequence);
    ++viewer.migrations;
    viewer.lastMigrationMs = nowMs;
    enterPhase(viewer, SourceViewerPhase::AwaitingInitAck, nowMs);
    viewer.lastActivityMs = nowMs;
    viewer.lastProgressMs = nowMs;
    viewer.lastRecordProgressMs = nowMs;
    viewer.lastEventProgressMs = nowMs;
    viewer.recordRepairSent = false;
    viewer.eventRepairSent = false;
    viewer.duplicateRecordAcks = 0;
    viewer.duplicateEventAcks = 0;
    viewer.recordFastRepairPending = false;
    viewer.eventFastRepairPending = false;
    viewer.recordRepairSends = 0;
    viewer.eventRepairSends = 0;
    return IngressResult::Accepted;
}

IngressResult SpectatorSource::onDatagram(EndpointId endpoint,
                                          const uint8_t* bytes,
                                          size_t length,
                                          uint64_t nowMs) {
    if (!_configured) return IngressResult::Failed;
    Datagram datagram{};
    const DecodeError decoded = decodeDatagram(
        bytes, length, _config.identity, datagram);
    if (decoded != DecodeError::None) return mapDecodeError(decoded);
    if (datagram.header.direction != Direction::ViewerToSource
        || datagram.header.senderRole != PeerRole::Viewer) {
        return IngressResult::WrongDirection;
    }
    if (!datagram.header.compatibilityIdentityAttested)
        return IngressResult::CompatibilityUnattested;
    const uint32_t sequence = datagram.header.sequence;

    IngressResult located = IngressResult::Accepted;
    auto locate = [&](ViewerId viewerId,
                      uint32_t generation) -> Viewer* {
        const auto found = _viewers.find(viewerId);
        if (found == _viewers.end()) {
            located = IngressResult::UnknownViewer;
            return nullptr;
        }
        Viewer& viewer = found->second;
        if (viewer.endpoint != endpoint) {
            located = IngressResult::EndpointMismatch;
            return nullptr;
        }
        if (generation != viewer.generation) {
            located = qnet::frameAfter(viewer.generation, generation)
                ? IngressResult::StaleGeneration
                : IngressResult::UnexpectedMessage;
            return nullptr;
        }
        if (!sequenceWindowAdmit(viewer.inbound, sequence)) {
            located = IngressResult::ReplayedSequence;
            return nullptr;
        }
        return &viewer;
    };

    switch (datagram.header.type) {
        case MessageType::Hello: {
            HelloPayload hello{};
            if (!decodePayload(datagram.payload, hello))
                return IngressResult::BadPayload;
            return handleHello(endpoint, hello, sequence, nowMs);
        }

        case MessageType::Migrate: {
            MigratePayload migrate{};
            if (!decodePayload(datagram.payload, migrate))
                return IngressResult::BadPayload;
            return handleMigrate(endpoint, migrate, sequence, nowMs);
        }

        case MessageType::Detach: {
            DetachPayload detach{};
            if (!decodePayload(datagram.payload, detach))
                return IngressResult::BadPayload;
            return handleDetach(endpoint, detach, sequence, nowMs);
        }

        case MessageType::InitAck: {
            InitAckPayload ack{};
            if (!decodePayload(datagram.payload, ack))
                return IngressResult::BadPayload;
            Viewer* viewer = locate(ack.viewerId, ack.generation);
            if (!viewer) return located;
            if (ack.gameId != viewer->gameId)
                return IngressResult::WrongGame;
            if (ack.viewerNonce != viewer->nonce
                || ack.startFingerprint != viewer->startFingerprint
                || ack.resumeRecord != viewer->handshakeResumeRecord
                || ack.resumeEvent != viewer->handshakeResumeEvent
                || ack.resumeDigest != viewer->handshakeResumeDigest) {
                return IngressResult::InitMismatch;
            }
            if (viewer->phase != SourceViewerPhase::AwaitingInitAck)
                return IngressResult::UnexpectedMessage;
            enterPhase(*viewer, SourceViewerPhase::Streaming, nowMs);
            viewer->lastActivityMs = nowMs;
            viewer->lastProgressMs = nowMs;
            viewer->lastRecordProgressMs = nowMs;
            viewer->lastEventProgressMs = nowMs;
            viewer->recordRepairSent = false;
            viewer->eventRepairSent = false;
            viewer->duplicateRecordAcks = 0;
            viewer->duplicateEventAcks = 0;
            viewer->recordFastRepairPending = false;
            viewer->eventFastRepairPending = false;
            return IngressResult::Accepted;
        }

        case MessageType::Ack: {
            AckPayload ack{};
            if (!decodePayload(datagram.payload, ack))
                return IngressResult::BadPayload;
            Viewer* viewer = locate(ack.viewerId, ack.generation);
            if (!viewer) return located;
            if (ack.gameId != viewer->gameId)
                return IngressResult::WrongGame;
            if (viewer->phase == SourceViewerPhase::AwaitingInitAck)
                return IngressResult::UnexpectedMessage;
            ArchiveExtent extent{};
            if (!_archive.extent(viewer->gameId, extent))
                return IngressResult::GameUnavailable;
            if (ack.nextRecord > extent.nextRecord
                || ack.nextRecord > viewer->highestSentNext
                || ack.nextEvent > extent.nextEvent
                || ack.nextEvent > viewer->highestSentEvent) {
                return IngressResult::FutureAck;
            }
            uint64_t expectedDigest = 0;
            const bool haveChainPoint = _archive.chainDigestAt(
                viewer->gameId, ack.nextRecord, ack.nextEvent, expectedDigest);
            if (!haveChainPoint || ack.digest != expectedDigest) {
                armRepairsFromUnverifiedAck(*viewer, ack.nextRecord,
                                            ack.nextEvent, nowMs);
                return IngressResult::AckDigestMismatch;
            }
            if (ack.nextRecord < viewer->acknowledgedNext
                || ack.nextEvent < viewer->acknowledgedNextEvent) {
                return IngressResult::StaleAck;
            }
            viewer->lastActivityMs = nowMs;
            const bool recordAdvanced =
                ack.nextRecord > viewer->acknowledgedNext;
            const bool eventAdvanced =
                ack.nextEvent > viewer->acknowledgedNextEvent;
            if (!recordAdvanced
                && ack.nextRecord == viewer->acknowledgedNext
                && viewer->acknowledgedNext < viewer->nextNewRecord) {
                if (viewer->duplicateRecordAcks < 3u)
                    ++viewer->duplicateRecordAcks;
                if (viewer->duplicateRecordAcks >= 3u)
                    viewer->recordFastRepairPending = true;
            }
            if (!eventAdvanced
                && ack.nextEvent == viewer->acknowledgedNextEvent
                && viewer->acknowledgedNextEvent < viewer->nextNewEvent) {
                if (viewer->duplicateEventAcks < 3u)
                    ++viewer->duplicateEventAcks;
                if (viewer->duplicateEventAcks >= 3u)
                    viewer->eventFastRepairPending = true;
            }
            if (recordAdvanced || eventAdvanced) {
                viewer->acknowledgedNext = ack.nextRecord;
                viewer->acknowledgedNextEvent = ack.nextEvent;
                viewer->acknowledgedDigest = ack.digest;
                if (viewer->nextNewRecord < ack.nextRecord)
                    viewer->nextNewRecord = ack.nextRecord;
                if (viewer->nextNewEvent < ack.nextEvent)
                    viewer->nextNewEvent = ack.nextEvent;
                viewer->lastProgressMs = nowMs;
                if (recordAdvanced) {
                    viewer->lastRecordProgressMs = nowMs;
                    viewer->recordRepairSent = false;
                    viewer->duplicateRecordAcks = 0;
                    viewer->recordFastRepairPending = false;
                }
                if (eventAdvanced) {
                    viewer->lastEventProgressMs = nowMs;
                    viewer->eventRepairSent = false;
                    viewer->duplicateEventAcks = 0;
                    viewer->eventFastRepairPending = false;
                }
            }
            return IngressResult::Accepted;
        }

        case MessageType::TerminalAck: {
            TerminalAckPayload ack{};
            if (!decodePayload(datagram.payload, ack))
                return IngressResult::BadPayload;
            Viewer* viewer = locate(ack.viewerId, ack.generation);
            if (!viewer) return located;
            if (ack.gameId != viewer->gameId)
                return IngressResult::WrongGame;
            ArchiveExtent extent{};
            if (!_archive.extent(viewer->gameId, extent)
                || !extent.sealed) {
                return IngressResult::GameUnavailable;
            }
            if (ack.nextRecord != extent.terminal.nextRecord
                || ack.nextEvent != extent.terminal.nextEvent
                || ack.finalDigest != extent.terminal.finalDigest
                || ack.postTailStateHash
                    != extent.terminal.postTailStateHash
                || ack.reason != extent.terminal.reason) {
                return IngressResult::TerminalMismatch;
            }
            if (viewer->phase != SourceViewerPhase::AwaitingTerminalAck
                && viewer->phase != SourceViewerPhase::Complete) {
                return IngressResult::UnexpectedMessage;
            }
            enterPhase(*viewer, SourceViewerPhase::Complete, nowMs);
            viewer->lastActivityMs = nowMs;
            viewer->lastProgressMs = nowMs;
            return IngressResult::Accepted;
        }

        case MessageType::GameStart:
        case MessageType::ConfirmedInputBatch:
        case MessageType::Terminal:
        case MessageType::Reject:
        case MessageType::Heartbeat:
        case MessageType::CheckpointOffer:
        case MessageType::CheckpointChunk:
        case MessageType::SetEvent:
            return IngressResult::WrongDirection;
    }
    return IngressResult::UnexpectedMessage;
}

std::vector<SourceDatagram> SpectatorSource::pollOutbound(
    uint64_t nowMs,
    size_t maxPacketsOverride) {
    std::vector<SourceDatagram> out;
    if (!_configured) return out;
    const size_t globalBudget = maxPacketsOverride == 0
        ? _config.globalPacketBudget
        : maxPacketsOverride;
    if (globalBudget == 0) return out;

    for (auto it = _viewers.begin(); it != _viewers.end();) {
        Viewer& viewer = it->second;
        SourceRetirementReason reason = SourceRetirementReason::None;
        if (viewer.phase == SourceViewerPhase::Complete) {
            reason = SourceRetirementReason::Completed;
        } else if (intervalElapsed(nowMs, viewer.lastActivityMs,
                                   _config.idleTimeoutMs)) {
            reason = SourceRetirementReason::Idle;
        } else if (viewer.phase == SourceViewerPhase::AwaitingInitAck
                   && intervalElapsed(nowMs, viewer.phaseEnteredMs,
                                      _config.handshakeDeadlineMs)) {
            reason = SourceRetirementReason::HandshakeDeadline;
        } else if (viewer.phase == SourceViewerPhase::AwaitingTerminalAck
                   && intervalElapsed(nowMs, viewer.phaseEnteredMs,
                                      _config.terminalAckDeadlineMs)) {
            reason = SourceRetirementReason::TerminalAckDeadline;
        } else if (viewer.phase == SourceViewerPhase::Streaming) {
            ArchiveExtent extent{};
            const bool haveExtent = _archive.extent(viewer.gameId, extent);
            uint32_t unackedRecords = 0;
            size_t unackedBytes = 0;
            bool caughtUp = false;
            if (haveExtent) {
                unackedRecords =
                    viewer.acknowledgedNext < extent.nextRecord
                        ? extent.nextRecord - viewer.acknowledgedNext
                        : 0u;
                const size_t unackedEvents =
                    viewer.acknowledgedNextEvent < extent.nextEvent
                        ? static_cast<size_t>(
                              extent.nextEvent
                              - viewer.acknowledgedNextEvent)
                        : 0u;
                unackedBytes = saturatingAddSize(
                    saturatingMul(
                        static_cast<size_t>(unackedRecords),
                        sizeof(ConfirmedRecord)),
                    saturatingMul(unackedEvents, sizeof(SetEvent)));
                caughtUp =
                    viewer.acknowledgedNext >= extent.nextRecord
                    && viewer.acknowledgedNextEvent >= extent.nextEvent;
            }
            if (haveExtent
                && (unackedRecords > _config.maxUnackedRecords
                    || unackedBytes > _config.maxUnackedBytes)) {
                reason = SourceRetirementReason::LagLimit;
            } else if (caughtUp) {
                viewer.lastProgressMs = nowMs;
            } else if (intervalElapsed(nowMs, viewer.lastProgressMs,
                                       _config.noProgressDeadlineMs)) {
                reason = SourceRetirementReason::NoProgressDeadline;
            }
        }
        if (reason != SourceRetirementReason::None) {
            const auto next = std::next(it);
            retire(it, reason);
            it = next;
        } else {
            ++it;
        }
    }

    std::vector<ViewerId> ids;
    ids.reserve(_viewers.size());
    for (const auto& item : _viewers) ids.push_back(item.first);
    const size_t count = ids.size();
    if (count == 0) {
        _controlCursor = 0;
        _dataCursor = 0;
    } else {
        _controlCursor %= count;
        _dataCursor %= count;
    }

    size_t controlReserve =
        std::min(_config.controlPacketBudget, globalBudget);
    if (controlReserve == 0) controlReserve = 1;
    if (globalBudget >= 2 && controlReserve >= globalBudget)
        controlReserve = globalBudget - 1;
    const size_t rejectShare = (controlReserve + 1) / 2;

    while (!_pendingRejects.empty() && out.size() < rejectShare) {
        out.push_back(std::move(_pendingRejects.front()));
        _pendingRejects.pop_front();
    }

    std::vector<ViewerId> removeAfterSend;
    if (count != 0) {
        const size_t start = _controlCursor;
        size_t visited = 0;
        while (visited < count && out.size() < controlReserve) {
            const ViewerId id = ids[(start + visited) % count];
            ++visited;
            auto found = _viewers.find(id);
            if (found == _viewers.end()) continue;
            Viewer& viewer = found->second;

            GameDescriptor descriptor{};
            ArchiveExtent extent{};
            const bool hasGame =
                _archive.descriptor(viewer.gameId, descriptor)
                && _archive.extent(viewer.gameId, extent);
            if (!hasGame) {
                RejectPayload reject{};
                reject.requestedGameId = viewer.gameId;
                reject.viewerNonce = viewer.nonce;
                reject.viewerId = viewer.viewerId;
                reject.generation = viewer.generation;
                reject.reason = RejectReason::ArchiveUnavailable;
                uint64_t latest = kLatestGameId;
                ArchiveExtent latestExtent{};
                if (_archive.latestGameId(latest)
                    && _archive.extent(latest, latestExtent)) {
                    reject.availableGameId = latest;
                    reject.availableNextRecord = latestExtent.nextRecord;
                }
                std::vector<uint8_t> payload;
                if (encodePayload(reject, payload)
                    && appendEncoded(viewer.endpoint, id,
                                     MessageType::Reject,
                                     _nextOutboundSequence, payload, out)) {
                    removeAfterSend.push_back(id);
                }
                continue;
            }
            if (viewer.acknowledgedNext < extent.firstRecord) {
                RejectPayload reject{};
                reject.requestedGameId = viewer.gameId;
                reject.availableGameId = viewer.gameId;
                reject.viewerNonce = viewer.nonce;
                reject.viewerId = viewer.viewerId;
                reject.generation = viewer.generation;
                reject.availableNextRecord = extent.firstRecord;
                reject.reason = RejectReason::ResumeMismatch;
                std::vector<uint8_t> payload;
                if (encodePayload(reject, payload)
                    && appendEncoded(viewer.endpoint, id,
                                     MessageType::Reject,
                                     _nextOutboundSequence, payload, out)) {
                    removeAfterSend.push_back(id);
                }
                continue;
            }

            if (viewer.phase == SourceViewerPhase::AwaitingInitAck) {
                const bool due = !viewer.handshakeSent
                    || intervalElapsed(nowMs, viewer.lastHandshakeSendMs,
                                       _config.handshakeResendMs);
                if (!due) continue;
                GameStartPayload start = canonicalGameStart(descriptor);
                start.viewerNonce = viewer.nonce;
                start.viewerId = viewer.viewerId;
                start.generation = viewer.generation;
                start.resumeRecord = viewer.handshakeResumeRecord;
                start.resumeEvent = viewer.handshakeResumeEvent;
                start.resumeDigest = viewer.handshakeResumeDigest;
                std::vector<uint8_t> payload;
                if (encodePayload(start, payload)
                    && appendEncoded(viewer.endpoint, id,
                                     MessageType::GameStart,
                                     _nextOutboundSequence, payload, out)) {
                    viewer.handshakeSent = true;
                    viewer.lastHandshakeSendMs = nowMs;
                }
                continue;
            }

            if (viewer.phase == SourceViewerPhase::Streaming) {
                if (extent.sealed
                    && viewer.acknowledgedNext
                        == extent.terminal.nextRecord
                    && viewer.acknowledgedNextEvent
                        == extent.terminal.nextEvent
                    && viewer.acknowledgedDigest
                        == extent.terminal.finalDigest) {
                    TerminalPayload terminal{};
                    terminal.gameId = viewer.gameId;
                    terminal.finalDigest = extent.terminal.finalDigest;
                    terminal.postTailStateHash =
                        extent.terminal.postTailStateHash;
                    terminal.nextRecord = extent.terminal.nextRecord;
                    terminal.nextEvent = extent.terminal.nextEvent;
                    terminal.reason = extent.terminal.reason;
                    std::vector<uint8_t> payload;
                    if (encodePayload(terminal, payload)
                        && appendEncoded(viewer.endpoint, id,
                                         MessageType::Terminal,
                                         _nextOutboundSequence, payload,
                                         out)) {
                        enterPhase(
                            viewer,
                            SourceViewerPhase::AwaitingTerminalAck, nowMs);
                        viewer.terminalSent = true;
                        viewer.lastTerminalSendMs = nowMs;
                    }
                    continue;
                }

                const bool eventsPending =
                    viewer.nextNewEvent < extent.nextEvent;
                const bool eventRepairRateOpen =
                    !viewer.eventRepairSent
                    || intervalElapsed(nowMs, viewer.lastEventRepairSendMs,
                                       _config.retransmitMs);
                const bool eventRetransmitDue =
                    viewer.acknowledgedNextEvent < viewer.nextNewEvent
                    && eventRepairRateOpen
                    && (viewer.eventFastRepairPending
                        || intervalElapsed(nowMs,
                                           viewer.lastEventProgressMs,
                                           _config.retransmitMs));
                if (eventsPending || eventRetransmitDue) {
                    const uint32_t cursor = eventRetransmitDue
                        ? viewer.acknowledgedNextEvent
                        : viewer.nextNewEvent;
                    SetEventBatchPayload batch{};
                    std::vector<uint8_t> payload;
                    if (_archive.readEventBatch(
                            viewer.gameId, cursor,
                            kMaxSetEventsPerBatch, batch)
                            == ArchiveResult::Ok
                        && encodePayload(batch, payload)
                        && appendEncoded(viewer.endpoint, id,
                                         MessageType::SetEvent,
                                         _nextOutboundSequence, payload,
                                         out)) {
                        const uint32_t delivered = cursor
                            + static_cast<uint32_t>(batch.events.size());
                        if (eventRetransmitDue) {
                            viewer.eventRepairSent = true;
                            viewer.lastEventRepairSendMs = nowMs;
                            viewer.duplicateEventAcks = 0;
                            viewer.eventFastRepairPending = false;
                            ++viewer.eventRepairSends;
                        } else {
                            const bool opensOutstanding =
                                viewer.acknowledgedNextEvent
                                    == viewer.nextNewEvent;
                            viewer.nextNewEvent = delivered;
                            if (opensOutstanding) {
                                viewer.lastEventProgressMs = nowMs;
                                viewer.eventRepairSent = false;
                                viewer.duplicateEventAcks = 0;
                                viewer.eventFastRepairPending = false;
                            }
                        }
                        viewer.highestSentEvent =
                            std::max(viewer.highestSentEvent, delivered);
                        viewer.eventSent = true;
                        viewer.lastEventSendMs = nowMs;
                    }
                    continue;
                }
                const bool caughtUp =
                    viewer.nextNewRecord >= extent.nextRecord
                    && viewer.nextNewEvent >= extent.nextEvent;
                const bool due = caughtUp
                    && (!viewer.heartbeatSent
                        || intervalElapsed(nowMs, viewer.lastHeartbeatSendMs,
                                           _config.heartbeatIntervalMs));
                if (!due) continue;
                HeartbeatPayload beat{};
                beat.gameId = viewer.gameId;
                beat.digest = extent.digest;
                beat.nextRecord = extent.nextRecord;
                beat.sourcePhase = derivedPhase(extent, true);
                beat.viewerCount = static_cast<uint16_t>(std::min<size_t>(
                    _viewers.size(),
                    (std::numeric_limits<uint16_t>::max)()));
                ArchiveCheckpointRef checkpoint{};
                beat.checkpointRecord =
                    _archive.newestCheckpoint(viewer.gameId, checkpoint)
                        ? checkpoint.recordCursor
                        : 0u;
                beat.nextEvent = extent.nextEvent;
                SetMatchIndexEntry match{};
                beat.currentMatchId =
                    _archive.currentMatch(viewer.gameId,
                                          extent.nextRecord, match)
                        ? match.matchId
                        : 0u;
                std::vector<uint8_t> payload;
                if (encodePayload(beat, payload)
                    && appendEncoded(viewer.endpoint, id,
                                     MessageType::Heartbeat,
                                     _nextOutboundSequence, payload, out)) {
                    viewer.heartbeatSent = true;
                    viewer.lastHeartbeatSendMs = nowMs;
                }
                continue;
            }

            if (viewer.phase == SourceViewerPhase::AwaitingTerminalAck) {
                const bool due = !viewer.terminalSent
                    || intervalElapsed(nowMs, viewer.lastTerminalSendMs,
                                       _config.terminalResendMs);
                if (!due) continue;
                TerminalPayload terminal{};
                terminal.gameId = viewer.gameId;
                terminal.finalDigest = extent.terminal.finalDigest;
                terminal.postTailStateHash =
                    extent.terminal.postTailStateHash;
                terminal.nextRecord = extent.terminal.nextRecord;
                terminal.nextEvent = extent.terminal.nextEvent;
                terminal.reason = extent.terminal.reason;
                std::vector<uint8_t> payload;
                if (encodePayload(terminal, payload)
                    && appendEncoded(viewer.endpoint, id,
                                     MessageType::Terminal,
                                     _nextOutboundSequence, payload, out)) {
                    viewer.terminalSent = true;
                    viewer.lastTerminalSendMs = nowMs;
                }
            }
        }
        _controlCursor = (start + visited) % count;
    }

    while (!_pendingRejects.empty() && out.size() < controlReserve) {
        out.push_back(std::move(_pendingRejects.front()));
        _pendingRejects.pop_front();
    }

    for (auto& [id, viewer] : _viewers) {
        if (out.size() >= globalBudget) {
            viewer.checkpointPushBlocked = 8;
            break;
        }
        if (!viewer.wantsNewestCheckpoint) continue;
        if (viewer.phase != SourceViewerPhase::Streaming) continue;
        ArchiveCheckpointRef ref{};
        uint64_t blobId = 0u;
        uint32_t totalBytes = 0u;
        if (!_archive.newestCheckpoint(viewer.gameId, ref)) {
            viewer.checkpointPushBlocked = 1;
            continue;
        }
        if (!_archive.newestCheckpointBlobInfo(viewer.gameId, blobId,
                                               totalBytes)) {
            viewer.checkpointPushBlocked = 2;
            continue;
        }
        if (blobId != ref.checkpointId || totalBytes == 0u) {
            viewer.checkpointPushBlocked = 3;
            continue;
        }
        const uint32_t chunkBytes =
            static_cast<uint32_t>(kMaxCheckpointChunkBytes);
        const uint32_t chunkCount =
            (totalBytes + chunkBytes - 1u) / chunkBytes;
        if (viewer.offeredCheckpointId != ref.checkpointId) {
            CheckpointOfferPayload offer{};
            offer.gameId = viewer.gameId;
            offer.checkpointId = ref.checkpointId;
            offer.digest = ref.digest;
            offer.compatibilityIdentity = _config.compatibilityIdentity;
            offer.recordCursor = ref.recordCursor;
            offer.totalBytes = totalBytes;
            offer.chunkCount = chunkCount;
            offer.chunkBytes = chunkBytes;
            std::vector<uint8_t> payload;
            if (!encodePayload(offer, payload)) {
                viewer.checkpointPushBlocked = 4;
                continue;
            }
            if (!appendEncoded(viewer.endpoint, id,
                               MessageType::CheckpointOffer,
                               _nextOutboundSequence, payload, out)) {
                viewer.checkpointPushBlocked = 5;
                continue;
            }
            viewer.checkpointPushBlocked = 0;
            viewer.offeredCheckpointId = ref.checkpointId;
            viewer.nextCheckpointChunk = 0u;
            viewer.checkpointPassesSent = 0u;
            viewer.checkpointPassCompleteMs = 0u;
            viewer.checkpointChunkCount = chunkCount;
            viewer.checkpointTotalBytes = totalBytes;
            continue;
        }
        if (viewer.nextCheckpointChunk >= viewer.checkpointChunkCount
            && viewer.checkpointChunkCount != 0u) {
            if (viewer.checkpointPassesSent + 1u >= kCheckpointPushPasses) {
                viewer.checkpointPushBlocked = 6;
                continue;
            }
            if (viewer.checkpointPassCompleteMs == 0u) {
                viewer.checkpointPassCompleteMs = nowMs;
                viewer.checkpointPushBlocked = 7;
                continue;
            }
            if (!intervalElapsed(nowMs, viewer.checkpointPassCompleteMs,
                                 kCheckpointPushRepassMs)) {
                viewer.checkpointPushBlocked = 7;
                continue;
            }
            ++viewer.checkpointPassesSent;
            viewer.nextCheckpointChunk = 0u;
            viewer.checkpointPassCompleteMs = 0u;
            viewer.checkpointPushBlocked = 0;
        }
        uint32_t sentThisTurn = 0u;
        while (viewer.nextCheckpointChunk < viewer.checkpointChunkCount
               && sentThisTurn < 4u && out.size() < globalBudget) {
            const uint32_t index = viewer.nextCheckpointChunk;
            const uint32_t offset = index * chunkBytes;
            const uint32_t length =
                std::min(chunkBytes, totalBytes - offset);
            CheckpointChunkPayload chunk{};
            chunk.gameId = viewer.gameId;
            chunk.checkpointId = ref.checkpointId;
            chunk.chunkIndex = index;
            chunk.chunkCount = viewer.checkpointChunkCount;
            chunk.byteOffset = offset;
            if (!_archive.checkpointChunk(viewer.gameId, ref.checkpointId,
                                          offset, length, chunk.bytes)) {
                break;
            }
            std::vector<uint8_t> payload;
            if (!encodePayload(chunk, payload)
                || !appendEncoded(viewer.endpoint, id,
                                  MessageType::CheckpointChunk,
                                  _nextOutboundSequence, payload, out)) {
                break;
            }
            ++viewer.nextCheckpointChunk;
            ++sentThisTurn;
        }
    }

    if (count != 0) {
        const size_t start = _dataCursor;
        size_t visited = 0;
        while (visited < count && out.size() < globalBudget) {
            const ViewerId id = ids[(start + visited) % count];
            ++visited;
            auto found = _viewers.find(id);
            if (found == _viewers.end()) continue;
            Viewer& viewer = found->second;
            if (viewer.phase != SourceViewerPhase::Streaming) continue;

            ArchiveExtent extent{};
            if (!_archive.extent(viewer.gameId, extent)) continue;
            if (viewer.acknowledgedNext < extent.firstRecord) continue;
            size_t viewerPackets = 0;

            const uint32_t windowEnd = saturatingAdd(
                viewer.acknowledgedNext, _config.sendWindowRecords);
            const uint32_t newSendLimit =
                std::min(extent.nextRecord, windowEnd);
            const bool recordRepairRateOpen =
                !viewer.recordRepairSent
                || intervalElapsed(nowMs, viewer.lastRecordRepairSendMs,
                                   _config.retransmitMs);
            const bool recordRepairDue =
                viewer.acknowledgedNext < viewer.nextNewRecord
                && recordRepairRateOpen
                && (viewer.recordFastRepairPending
                    || intervalElapsed(nowMs,
                                       viewer.lastRecordProgressMs,
                                       _config.retransmitMs));
            if (recordRepairDue
                && viewerPackets < _config.perViewerPacketBudget
                && out.size() < globalBudget) {
                const uint32_t retransmitLimit =
                    std::min(viewer.nextNewRecord, windowEnd);
                const size_t available = retransmitLimit
                        > viewer.acknowledgedNext
                    ? static_cast<size_t>(
                        retransmitLimit - viewer.acknowledgedNext)
                    : 0u;
                ConfirmedInputBatchPayload batch{};
                std::vector<uint8_t> payload;
                if (available != 0u
                    && _archive.readBatch(
                        viewer.gameId, viewer.acknowledgedNext,
                        std::min(_config.recordsPerBatch, available), batch)
                           == ArchiveResult::Ok
                    && encodePayload(batch, payload)
                    && appendEncoded(viewer.endpoint, id,
                                     MessageType::ConfirmedInputBatch,
                                     _nextOutboundSequence, payload, out)) {
                    viewer.dataSent = true;
                    viewer.lastDataSendMs = nowMs;
                    viewer.recordRepairSent = true;
                    viewer.lastRecordRepairSendMs = nowMs;
                    viewer.duplicateRecordAcks = 0;
                    viewer.recordFastRepairPending = false;
                    ++viewer.recordRepairSends;
                    ++viewerPackets;
                }
            }

            while (viewer.nextNewRecord < newSendLimit
                   && viewerPackets < _config.perViewerPacketBudget
                   && out.size() < globalBudget) {
                const size_t available =
                    static_cast<size_t>(newSendLimit
                        - viewer.nextNewRecord);
                ConfirmedInputBatchPayload batch{};
                if (_archive.readBatch(
                        viewer.gameId, viewer.nextNewRecord,
                        std::min(_config.recordsPerBatch, available),
                        batch) != ArchiveResult::Ok) {
                    break;
                }
                std::vector<uint8_t> payload;
                if (!encodePayload(batch, payload)
                    || !appendEncoded(viewer.endpoint, id,
                                      MessageType::ConfirmedInputBatch,
                                      _nextOutboundSequence, payload, out)) {
                    break;
                }
                const bool opensOutstanding =
                    viewer.acknowledgedNext == viewer.nextNewRecord;
                viewer.nextNewRecord +=
                    static_cast<uint32_t>(batch.records.size());
                if (opensOutstanding) {
                    viewer.lastRecordProgressMs = nowMs;
                    viewer.recordRepairSent = false;
                    viewer.duplicateRecordAcks = 0;
                    viewer.recordFastRepairPending = false;
                }
                viewer.highestSentNext = std::max(
                    viewer.highestSentNext, viewer.nextNewRecord);
                viewer.dataSent = true;
                viewer.lastDataSendMs = nowMs;
                ++viewerPackets;
            }

        }
        _dataCursor = (start + visited) % count;
    }

    for (ViewerId id : removeAfterSend) {
        const auto found = _viewers.find(id);
        if (found != _viewers.end())
            retire(found, SourceRetirementReason::ArchiveUnavailable);
    }

    while (!_pendingRejects.empty() && out.size() < globalBudget) {
        out.push_back(std::move(_pendingRejects.front()));
        _pendingRejects.pop_front();
    }
    return out;
}

bool SpectatorSource::setViewerWantsNewestCheckpoint(ViewerId viewer) {
    const auto found = _viewers.find(viewer);
    if (found == _viewers.end()) return false;
    found->second.wantsNewestCheckpoint = true;
    return true;
}

bool SpectatorSource::viewerSnapshot(
    ViewerId viewerId,
    SourceViewerSnapshot& out) const {
    const auto found = _viewers.find(viewerId);
    if (found != _viewers.end()) {
        out = snapshotOf(found->second);
        return true;
    }
    for (auto it = _retired.rbegin(); it != _retired.rend(); ++it) {
        if (it->viewerId == viewerId) {
            out = *it;
            return true;
        }
    }
    return false;
}

bool SpectatorSource::removeViewer(ViewerId viewer) {
    const auto found = _viewers.find(viewer);
    if (found == _viewers.end()) return false;
    retire(found, SourceRetirementReason::Removed);
    return true;
}

SpectatorViewer::SpectatorViewer(const ViewerConfig& config)
    : _config(config),
      _configured(
          isValidIdentity(config.identity)
          && config.simulationContract != 0
          && compat::isValidCompatibilityIdentity(
              config.compatibilityIdentity)
          && (config.secureRandom != 0 || config.viewerNonce != 0)
          && config.helloResendMs > 0
          && config.ackResendMs > 0
          && config.terminalAckResendMs > 0
          && config.terminalAckMaxSends > 0
          && config.maxReorderRecords >= kMaxRecordsPerBatch
          && config.maxRecords > 0
          && (!config.relayIngestOnly
              || (config.maxRelayRetainedRecords > 0
                  && config.maxRelayRetainedEvents > 0))) {
    _viewerId = config.secureRandom != 0
        ? config.secureRandom
        : config.viewerNonce;
}

void SpectatorViewer::beginGeneration() {
    ++_generation;
    if (_generation == 0) _generation = 1;
    _activeNonce = viewerGenerationNonce(_viewerId, _generation);
}

void SpectatorViewer::clearValidatedParentRouteTransition() {
    _pendingValidatedParentRoute.reset();
}

bool SpectatorViewer::begin(uint64_t requestedGameId) {
    if (!_configured) return false;
    _generation = 0;
    beginGeneration();

    _phase = ViewerPhase::Connecting;
    _failure = IngressResult::Accepted;
    _requestedGameId = requestedGameId;
    _helloResumeRecord = 0;
    _helloResumeEvent = 0;
    _helloResumeDigest = 0;
    _nextSequence = _config.initialSequence != 0u
        ? _config.initialSequence : 1u;
    _lastValidReceiveMs = 0;
    _inbound = ReplayWindow{};
    clearValidatedParentRouteTransition();
    _hasStart = false;
    _start = {};
    _startFingerprint = 0;
    _receivedNext = 0;
    _receivedDigest = 0;
    _playbackNext = 0;
    _hasPlaybackPostState = false;
    _playbackPostNextRecord = 0;
    _playbackPostStateHash = 0;
    _history.clear();
    _historyBase = 0;
    _relayRecords.clear();
    _relayRecordBaseDigests.clear();
    _relayFirstRecord = 0;
    _pending.clear();
    _setTimeline = SpectatorSetTimeline{};
    _appliedEvents = 0;
    _pendingEvents.clear();
    _relayEvents.clear();
    _relayFirstEvent = 0;
    _helloSent = false;
    _lastHelloSendMs = 0;
    _initAckDirty = false;
    _ackDirty = false;
    _ackSent = false;
    _lastAckSendMs = 0;
    _hasTerminal = false;
    _terminal = {};
    _terminalAckDirty = false;
    _terminalAckSent = false;
    _terminalAckSends = 0;
    _lastTerminalAckSendMs = 0;
    _hasReject = false;
    _reject = {};
    _hasHeartbeat = false;
    _heartbeat = {};
    _hasCheckpointOffer = false;
    _checkpointOffer = {};
    _migrateDirty = false;
    _migrateOldGeneration = 0;
    _migrateGrantId = 0;
    _migrateGrantMacLow = 0;
    _migrateGrantMacHigh = 0;
    _detachDirty = false;
    return true;
}

bool SpectatorViewer::takeAssembledCheckpoint(Checkpoint& out) {
    if (!_hasAssembledCheckpoint) return false;
    out = _assembledCheckpoint;
    _hasAssembledCheckpoint = false;
    return true;
}

bool SpectatorViewer::resumeAtCheckpoint(
    uint32_t recordCursor, uint32_t eventCursor, uint64_t chainDigest,
    const SpectatorSetTimeline::BindSeed& seed) {
    if (!_configured || !_hasStart || chainDigest == 0u) return false;
    if (_phase != ViewerPhase::Streaming
        && _phase != ViewerPhase::Connecting) {
        return false;
    }
    _receivedNext = recordCursor;
    _appliedEvents = eventCursor;
    _receivedDigest = chainDigest;
    _history.clear();
    _historyBase = recordCursor;
    _playbackNext = recordCursor;
    _hasPlaybackPostState = false;
    _hasBoundBase = true;
    _boundBaseRecord = recordCursor;
    _boundBaseEvent = eventCursor;
    _boundBaseDigest = chainDigest;
    _setTimeline.bindAt(eventCursor, seed);
    _config.wantsNewestCheckpoint = false;
    _hasCheckpointOffer = false;
    return resume();
}

bool SpectatorViewer::resume() {
    if (!_configured || !_hasStart
        || _phase == ViewerPhase::Failed
        || _phase == ViewerPhase::Rejected
        || _phase == ViewerPhase::Detached
        || _pendingValidatedParentRoute.has_value()) {
        return false;
    }
    if (_hasTerminal) return true;

    beginGeneration();

    _phase = ViewerPhase::Connecting;
    _failure = IngressResult::Accepted;
    _requestedGameId = _start.gameId;
    _helloResumeRecord = _receivedNext;
    _helloResumeEvent = _appliedEvents;
    _helloResumeDigest = _receivedDigest;
    _pending.clear();
    _pendingEvents.clear();
    _helloSent = false;
    _lastHelloSendMs = 0;
    _initAckDirty = false;
    _ackDirty = false;
    _ackSent = false;
    _lastAckSendMs = 0;
    _hasTerminal = false;
    _terminal = {};
    _terminalAckDirty = false;
    _terminalAckSent = false;
    _terminalAckSends = 0;
    _hasReject = false;
    _reject = {};
    _migrateDirty = false;
    _inbound = ReplayWindow{};
    return true;
}

bool SpectatorViewer::migrate(uint64_t grantId,
                              uint64_t grantMacLow,
                              uint64_t grantMacHigh) {
    if (!_configured || !_hasStart
        || _phase == ViewerPhase::Failed
        || _phase == ViewerPhase::Rejected
        || _phase == ViewerPhase::Detached
        || _pendingValidatedParentRoute.has_value()) {
        return false;
    }
    if (_hasTerminal) return true;

    _migrateOldGeneration = _generation;
    beginGeneration();
    _migrateGrantId = grantId;
    _migrateGrantMacLow = grantMacLow;
    _migrateGrantMacHigh = grantMacHigh;
    _migrateDirty = true;

    _phase = ViewerPhase::Connecting;
    _failure = IngressResult::Accepted;
    _requestedGameId = _start.gameId;
    _helloResumeRecord = _receivedNext;
    _helloResumeEvent = _appliedEvents;
    _helloResumeDigest = _receivedDigest;
    _pending.clear();
    _pendingEvents.clear();
    _helloSent = false;
    _lastHelloSendMs = 0;
    _initAckDirty = false;
    _ackDirty = false;
    _ackSent = false;
    _lastAckSendMs = 0;
    _hasReject = false;
    _reject = {};
    _inbound = ReplayWindow{};
    return true;
}

bool SpectatorViewer::armValidatedParentRouteTransition(
    const uint64_t activationId, const uint32_t nextRecord,
    const uint32_t nextEvent, const uint64_t digest) {
    if (!_configured || activationId == 0u || !_hasStart
        || _phase != ViewerPhase::Streaming || _hasTerminal
        || _detachDirty || _migrateDirty
        || _pendingValidatedParentRoute.has_value()
        || nextRecord != _receivedNext || nextEvent != _appliedEvents
        || digest != _receivedDigest) {
        return false;
    }
    uint64_t provenDigest = 0u;
    if (!chainDigestAtLocal(nextRecord, nextEvent, provenDigest)
        || provenDigest != digest) {
        return false;
    }
    PendingValidatedParentRoute transition{};
    transition.activationId = activationId;
    transition.nextRecord = nextRecord;
    transition.nextEvent = nextEvent;
    transition.digest = digest;
    _pendingValidatedParentRoute = transition;
    return true;
}

bool SpectatorViewer::cancelValidatedParentRouteTransition(
    const uint64_t activationId) {
    if (activationId == 0u || !_pendingValidatedParentRoute.has_value()
        || _pendingValidatedParentRoute->activationId != activationId) {
        return false;
    }
    clearValidatedParentRouteTransition();
    return true;
}

IngressResult SpectatorViewer::onValidatedParentRouteDatagram(
    const uint64_t activationId, const uint8_t* const bytes,
    const size_t length, const uint64_t ) {
    if (!_configured || activationId == 0u
        || !_pendingValidatedParentRoute.has_value()
        || _pendingValidatedParentRoute->activationId != activationId
        || _pendingValidatedParentRoute->candidateIngressAccepted
        || !_hasStart || _phase != ViewerPhase::Streaming) {
        return IngressResult::UnexpectedMessage;
    }

    Datagram datagram{};
    const DecodeError decoded = decodeDatagram(
        bytes, length, _config.identity, datagram);
    if (decoded != DecodeError::None) return mapDecodeError(decoded);
    if (datagram.header.direction != Direction::SourceToViewer
        || datagram.header.senderRole != PeerRole::Source) {
        return IngressResult::WrongDirection;
    }
    if (!datagram.header.compatibilityIdentityAttested)
        return IngressResult::CompatibilityUnattested;

    PendingValidatedParentRoute& transition =
        *_pendingValidatedParentRoute;
    if (!sequenceWindowAdmit(transition.candidateInbound,
                             datagram.header.sequence)) {
        return IngressResult::ReplayedSequence;
    }
    if (datagram.header.type != MessageType::GameStart)
        return IngressResult::UnexpectedMessage;

    GameStartPayload start{};
    if (!decodePayload(datagram.payload, start))
        return IngressResult::BadPayload;
    if (start.viewerId != _viewerId || start.generation != _generation
        || start.viewerNonce != _activeNonce
        || start.simulationContract != _config.simulationContract
        || !compat::semanticallyEqual(_config.compatibilityIdentity,
                                      start.compatibilityIdentity)
        || (_requestedGameId != kLatestGameId
            && start.gameId != _requestedGameId)) {
        return IngressResult::InitMismatch;
    }
    if (start.initialDigest != computeInitialDigest(start))
        return IngressResult::RecordDigestMismatch;
    const uint64_t fingerprint = gameStartFingerprint(start);
    if (!sameImmutableStart(_start, start)
        || _startFingerprint != fingerprint
        || start.resumeRecord != transition.nextRecord
        || start.resumeEvent != transition.nextEvent
        || start.resumeDigest != transition.digest
        || _receivedNext < transition.nextRecord
        || _appliedEvents < transition.nextEvent) {
        return IngressResult::ResumeMismatch;
    }
    uint64_t provenDigest = 0u;
    if (!chainDigestAtLocal(transition.nextRecord, transition.nextEvent,
                            provenDigest)
        || provenDigest != transition.digest) {
        return IngressResult::ResumeMismatch;
    }
    transition.candidateIngressAccepted = true;
    return IngressResult::Accepted;
}

bool SpectatorViewer::commitValidatedParentRouteIngress(
    const uint64_t activationId, const uint64_t nowMs) {
    if (activationId == 0u || !_pendingValidatedParentRoute.has_value())
        return false;
    const PendingValidatedParentRoute transition =
        *_pendingValidatedParentRoute;
    if (transition.activationId != activationId
        || !transition.candidateIngressAccepted || !_hasStart
        || _phase != ViewerPhase::Streaming) {
        return false;
    }

    _inbound = transition.candidateInbound;
    _helloResumeRecord = transition.nextRecord;
    _helloResumeEvent = transition.nextEvent;
    _helloResumeDigest = transition.digest;
    _initAckDirty = true;
    _ackDirty = true;
    _ackSent = false;
    _lastAckSendMs = 0u;
    _hasHeartbeat = false;
    _heartbeat = {};
    _hasCheckpointOffer = false;
    _checkpointOffer = {};
    _lastValidReceiveMs = nowMs;
    clearValidatedParentRouteTransition();
    return true;
}

bool SpectatorViewer::detach() {
    if (!_configured || _phase == ViewerPhase::Idle
        || _phase == ViewerPhase::Detached) {
        return false;
    }
    clearValidatedParentRouteTransition();
    _detachDirty = true;
    return true;
}

bool SpectatorViewer::reportTransportFailure(IngressResult reason) {
    if (!_configured || reason != IngressResult::RouteLost) return false;
    if (_hasTerminal) return true;
    if (_phase == ViewerPhase::Idle
        || _phase == ViewerPhase::Rejected
        || _phase == ViewerPhase::Detached) {
        return false;
    }
    if (_phase == ViewerPhase::Failed)
        return _failure == IngressResult::RouteLost;
    clearValidatedParentRouteTransition();
    fail(reason);
    return true;
}

IngressResult SpectatorViewer::fail(IngressResult reason) {
    clearValidatedParentRouteTransition();
    _failure = reason;
    _phase = ViewerPhase::Failed;
    return reason;
}

bool SpectatorViewer::chainDigestAtLocal(uint32_t record,
                                         uint32_t eventCount,
                                         uint64_t& digest) const {
    if (!_hasStart) return false;
    if (eventCount > _appliedEvents || record > _receivedNext) return false;
    if (_hasBoundBase && record == _boundBaseRecord
        && eventCount == _boundBaseEvent) {
        digest = _boundBaseDigest;
        return true;
    }
    if (_hasBoundBase
        && (record < _boundBaseRecord || eventCount < _boundBaseEvent)) {
        return false;
    }
    if (eventCount > 0
        && !(_hasBoundBase && eventCount == _boundBaseEvent)) {
        SetEvent previous{};
        if (!_setTimeline.event(eventCount - 1, previous)) return false;
        if (previous.recordCursor > record) return false;
        if (previous.recordCursor == record) {
            digest = previous.rollingDigest;
            return true;
        }
    }
    if (eventCount < _appliedEvents) {
        SetEvent following{};
        if (!_setTimeline.event(eventCount, following)) return false;
        if (following.recordCursor < record) return false;
    }
    if (record == 0) {
        digest = _start.initialDigest;
        return true;
    }
    if (_config.relayIngestOnly
        && !_relayRecords.empty()
        && record == _relayFirstRecord) {
        if (_relayRecordBaseDigests.size() != _relayRecords.size())
            return false;
        digest = _relayRecordBaseDigests.front();
        return true;
    }
    ConfirmedRecord previous{};
    if (!verifiedRecordAt(record - 1, previous)) return false;
    digest = previous.rollingDigest;
    return true;
}

bool SpectatorViewer::chainBeforeRecord(uint32_t record,
                                        uint64_t& digest) const {
    return chainDigestAtLocal(
        record, _setTimeline.eventsBeforeRecord(record), digest);
}

IngressResult SpectatorViewer::drainChain() {
    for (;;) {
        const auto pendingEvent = _pendingEvents.find(_appliedEvents);
        if (pendingEvent != _pendingEvents.end()) {
            const SetEvent& event = pendingEvent->second;
            if (event.recordCursor < _receivedNext) {
                return fail(IngressResult::TimelineMismatch);
            }
            if (event.recordCursor == _receivedNext) {
                SetEvent stored{};
                if (_setTimeline.append(event, _receivedNext,
                                        _receivedDigest, &stored)
                    != SetEventResult::Ok) {
                    return fail(IngressResult::RecordDigestMismatch);
                }
                _receivedDigest = stored.rollingDigest;
                _appliedEvents = _setTimeline.nextEventIndex();
                if (_config.relayIngestOnly) retainRelayEvent(stored);
                _pendingEvents.erase(pendingEvent);
                continue;
            }
        }
        const auto pendingRecord = _pending.find(_receivedNext);
        if (pendingRecord == _pending.end()) {
            _awaitingEventIndex = 0;
            break;
        }
        if (pendingRecord->second.baseEventIndex > _appliedEvents) {
            _awaitingEventIndex = pendingRecord->second.baseEventIndex;
            break;
        }
        _awaitingEventIndex = 0;
        const ConfirmedRecord& record = pendingRecord->second.record;
        const uint64_t expected = advanceRollingDigest(
            _receivedDigest, _start.gameId, record);
        if (record.rollingDigest != expected)
            return fail(IngressResult::RecordDigestMismatch);
        if (_config.relayIngestOnly) {
            retainRelayRecord(record);
        } else {
            if (_history.size() >= _config.maxRecords)
                return fail(IngressResult::ReorderOverflow);
            _history.push_back(record);
        }
        _receivedDigest = record.rollingDigest;
        ++_receivedNext;
        _pending.erase(pendingRecord);
    }
    return IngressResult::Accepted;
}

void SpectatorViewer::retainRelayRecord(const ConfirmedRecord& record) {
    if (_relayRecords.empty()) _relayFirstRecord = record.record;
    _relayRecords.push_back(record);
    _relayRecordBaseDigests.push_back(_receivedDigest);
    while (_relayRecords.size() > _config.maxRelayRetainedRecords) {
        _relayRecords.pop_front();
        _relayRecordBaseDigests.pop_front();
        ++_relayFirstRecord;
    }
}

void SpectatorViewer::retainRelayEvent(const SetEvent& event) {
    if (_relayEvents.empty()) _relayFirstEvent = event.eventIndex;
    _relayEvents.push_back(event);
    while (_relayEvents.size() > _config.maxRelayRetainedEvents) {
        _relayEvents.pop_front();
        ++_relayFirstEvent;
    }
}

bool SpectatorViewer::appendEncoded(
    MessageType type,
    const std::vector<uint8_t>& payload,
    std::vector<ViewerDatagram>& out,
    const ViewerDatagramRoute route,
    const uint64_t parentRouteActivationId) {
    if ((route == ViewerDatagramRoute::ActiveParent
         && parentRouteActivationId != 0u)
        || (route == ViewerDatagramRoute::ValidatedPendingParent
            && parentRouteActivationId == 0u)) {
        return false;
    }
    ViewerDatagram packet{};
    packet.type = type;
    packet.sequence = _nextSequence;
    if (!encodeDatagram(type, _config.identity, _nextSequence,
                        payload, packet.bytes)) {
        return false;
    }
    packet.route = route;
    packet.parentRouteActivationId = parentRouteActivationId;
    ++_nextSequence;
    out.push_back(std::move(packet));
    return true;
}

IngressResult SpectatorViewer::onDatagram(const uint8_t* bytes,
                                          size_t length,
                                          uint64_t nowMs) {
    if (!_configured || _phase == ViewerPhase::Failed)
        return IngressResult::Failed;
    Datagram datagram{};
    const DecodeError decoded = decodeDatagram(
        bytes, length, _config.identity, datagram);
    if (decoded != DecodeError::None) return mapDecodeError(decoded);
    if (datagram.header.direction != Direction::SourceToViewer
        || datagram.header.senderRole != PeerRole::Source) {
        return IngressResult::WrongDirection;
    }
    if (!datagram.header.compatibilityIdentityAttested)
        return IngressResult::CompatibilityUnattested;
    if (!sequenceWindowAdmit(_inbound, datagram.header.sequence))
        return IngressResult::ReplayedSequence;

    switch (datagram.header.type) {
        case MessageType::GameStart: {
            if (_phase != ViewerPhase::Connecting
                && _phase != ViewerPhase::Streaming) {
                return IngressResult::UnexpectedMessage;
            }
            GameStartPayload start{};
            if (!decodePayload(datagram.payload, start))
                return IngressResult::BadPayload;
            if (start.viewerId != _viewerId
                || start.generation != _generation
                || start.viewerNonce != _activeNonce) {
                return IngressResult::InitMismatch;
            }
            if (start.simulationContract != _config.simulationContract)
                return IngressResult::ContractMismatch;
            if (!compat::semanticallyEqual(
                    _config.compatibilityIdentity,
                    start.compatibilityIdentity)) {
                return IngressResult::ContractMismatch;
            }
            if (_requestedGameId != kLatestGameId
                && start.gameId != _requestedGameId) {
                return IngressResult::WrongGame;
            }
            if (start.initialDigest != computeInitialDigest(start))
                return fail(IngressResult::RecordDigestMismatch);
            const uint64_t fingerprint = gameStartFingerprint(start);

            if (!_hasStart) {
                if (_helloResumeRecord != 0
                    || _helloResumeEvent != 0
                    || start.resumeRecord != 0
                    || start.resumeEvent != 0
                    || start.resumeDigest != start.initialDigest) {
                    return fail(IngressResult::ResumeMismatch);
                }
                _start = start;
                _startFingerprint = fingerprint;
                _hasStart = true;
                _receivedNext = 0;
                _receivedDigest = start.initialDigest;
                _helloResumeDigest = start.resumeDigest;
                _setTimeline.reset(start.gameId, start.firstTimelineFrame);
                _appliedEvents = 0;
            } else {
                if (!sameImmutableStart(_start, start)
                    || _startFingerprint != fingerprint) {
                    return fail(IngressResult::InitMismatch);
                }
                if (start.resumeRecord != _helloResumeRecord
                    || start.resumeEvent != _helloResumeEvent
                    || start.resumeDigest != _helloResumeDigest
                    || _receivedNext < _helloResumeRecord) {
                    return fail(IngressResult::ResumeMismatch);
                }
                uint64_t prefixDigest = 0;
                if (!chainDigestAtLocal(_helloResumeRecord,
                                        _helloResumeEvent, prefixDigest)
                    || prefixDigest != _helloResumeDigest) {
                    return fail(IngressResult::ResumeMismatch);
                }
            }
            _phase = ViewerPhase::Streaming;
            _migrateDirty = false;
            _initAckDirty = true;
            _ackDirty = true;
            _lastValidReceiveMs = nowMs;
            return IngressResult::Accepted;
        }

        case MessageType::ConfirmedInputBatch: {
            if (!_hasStart
                || (_phase != ViewerPhase::Streaming
                    && _phase != ViewerPhase::Complete)) {
                return IngressResult::UnexpectedMessage;
            }
            ConfirmedInputBatchPayload batch{};
            if (!decodePayload(datagram.payload, batch))
                return IngressResult::BadPayload;
            if (batch.gameId != _start.gameId)
                return IngressResult::WrongGame;
            if (batch.records.empty()) return IngressResult::BadPayload;
            const uint32_t lastRecord = batch.records.back().record;
            if (_hasTerminal
                && lastRecord >= _terminal.nextRecord) {
                return fail(IngressResult::TerminalMismatch);
            }
            if ((!_config.relayIngestOnly
                 && lastRecord >= _config.maxRecords)
                || lastRecord < batch.startRecord) {
                return fail(IngressResult::ReorderOverflow);
            }
            if (batch.startRecord > _receivedNext
                && batch.startRecord - _receivedNext
                    >= _config.maxReorderRecords) {
                return fail(IngressResult::ReorderOverflow);
            }
            if (lastRecord >= _receivedNext
                && lastRecord - _receivedNext
                    >= _config.maxReorderRecords) {
                return fail(IngressResult::ReorderOverflow);
            }

            uint64_t batchDigest = batch.baseDigest;
            for (const ConfirmedRecord& record : batch.records) {
                if (_start.firstTimelineFrame
                    > (std::numeric_limits<uint32_t>::max)()
                        - record.record
                    || record.timelineFrame
                        != _start.firstTimelineFrame + record.record) {
                    return fail(IngressResult::TimelineMismatch);
                }
                const uint64_t expected = advanceRollingDigest(
                    batchDigest, batch.gameId, record);
                if (record.rollingDigest != expected)
                    return fail(IngressResult::RecordDigestMismatch);
                batchDigest = record.rollingDigest;
            }
            if (batchDigest != batch.endDigest)
                return fail(IngressResult::RecordDigestMismatch);

            for (const ConfirmedRecord& record : batch.records) {
                if (record.record >= _receivedNext) continue;
                ConfirmedRecord existing{};
                if (!verifiedRecordAt(record.record, existing)
                    || existing != record) {
                    return fail(IngressResult::ConflictingRecord);
                }
            }

            if (batch.startRecord <= _receivedNext
                && _appliedEvents >= batch.baseEventIndex) {
                uint64_t knownBase = 0;
                if (!chainBeforeRecord(batch.startRecord, knownBase)
                    || batch.baseDigest != knownBase) {
                    return fail(IngressResult::RecordDigestMismatch);
                }
            }

            for (const ConfirmedRecord& record : batch.records) {
                if (record.record < _receivedNext) {
                    continue;
                }
                const auto existing = _pending.find(record.record);
                if (existing != _pending.end()
                    && existing->second.record != record) {
                    return fail(IngressResult::ConflictingRecord);
                }
            }

            for (const ConfirmedRecord& record : batch.records) {
                if (record.record < _receivedNext) continue;
                PendingRecord pending{};
                pending.record = record;
                pending.baseEventIndex = batch.baseEventIndex;
                _pending.emplace(record.record, pending);
            }
            if (_pending.size() > _config.maxReorderRecords)
                return fail(IngressResult::ReorderOverflow);

            const IngressResult drained = drainChain();
            if (drained != IngressResult::Accepted) return drained;
            _ackDirty = true;
            _lastValidReceiveMs = nowMs;
            return IngressResult::Accepted;
        }

        case MessageType::SetEvent: {
            if (!_hasStart
                || (_phase != ViewerPhase::Streaming
                    && _phase != ViewerPhase::Complete)) {
                return IngressResult::UnexpectedMessage;
            }
            SetEventBatchPayload batch{};
            if (!decodePayload(datagram.payload, batch))
                return IngressResult::BadPayload;
            if (batch.gameId != _start.gameId)
                return IngressResult::WrongGame;
            if (batch.events.empty()) return IngressResult::BadPayload;
            if (_hasTerminal
                && batch.events.back().eventIndex >= _terminal.nextEvent) {
                return fail(IngressResult::TerminalMismatch);
            }
            for (const SetEvent& event : batch.events) {
                if (event.eventIndex < _appliedEvents) {
                    SetEvent existing{};
                    if (!_setTimeline.event(event.eventIndex, existing)
                        || existing != event) {
                        return fail(IngressResult::ConflictingRecord);
                    }
                    continue;
                }
                if (event.eventIndex - _appliedEvents
                    >= kMaxPendingSetEvents) {
                    return fail(IngressResult::ReorderOverflow);
                }
                const auto existing = _pendingEvents.find(event.eventIndex);
                if (existing != _pendingEvents.end()
                    && existing->second != event) {
                    return fail(IngressResult::ConflictingRecord);
                }
                _pendingEvents.emplace(event.eventIndex, event);
            }
            const IngressResult drained = drainChain();
            if (drained != IngressResult::Accepted) return drained;
            _ackDirty = true;
            _lastValidReceiveMs = nowMs;
            return IngressResult::Accepted;
        }

        case MessageType::Terminal: {
            if (!_hasStart
                || (_phase != ViewerPhase::Streaming
                    && _phase != ViewerPhase::Complete)) {
                return IngressResult::UnexpectedMessage;
            }
            TerminalPayload terminal{};
            if (!decodePayload(datagram.payload, terminal))
                return IngressResult::BadPayload;
            if (terminal.gameId != _start.gameId)
                return IngressResult::WrongGame;
            if (terminal.nextRecord != _receivedNext
                || terminal.nextEvent != _appliedEvents
                || terminal.finalDigest != _receivedDigest) {
                return fail(IngressResult::PrematureTerminal);
            }
            if (!_pending.empty() || !_pendingEvents.empty())
                return fail(IngressResult::TerminalMismatch);
            ArchiveTerminal proof{};
            proof.reason = terminal.reason;
            proof.nextRecord = terminal.nextRecord;
            proof.nextEvent = terminal.nextEvent;
            proof.finalDigest = terminal.finalDigest;
            proof.postTailStateHash = terminal.postTailStateHash;
            if (_hasTerminal && !sameTerminal(_terminal, proof))
                return fail(IngressResult::TerminalMismatch);
            const bool repeated = _hasTerminal;
            _hasTerminal = true;
            _terminal = proof;
            if (repeated && _phase == ViewerPhase::Complete) {
                _terminalAckDirty = true;
            }
            _lastValidReceiveMs = nowMs;
            return completeTailIfVerified();
        }

        case MessageType::Heartbeat: {
            if (!_hasStart
                || (_phase != ViewerPhase::Streaming
                    && _phase != ViewerPhase::Complete)) {
                return IngressResult::UnexpectedMessage;
            }
            HeartbeatPayload beat{};
            if (!decodePayload(datagram.payload, beat))
                return IngressResult::BadPayload;
            if (beat.gameId != _start.gameId)
                return IngressResult::WrongGame;
            _hasHeartbeat = true;
            _heartbeat = beat;
            _lastValidReceiveMs = nowMs;
            return IngressResult::Accepted;
        }

        case MessageType::CheckpointOffer: {
            if (!_hasStart || _phase != ViewerPhase::Streaming)
                return IngressResult::UnexpectedMessage;
            CheckpointOfferPayload offer{};
            if (!decodePayload(datagram.payload, offer))
                return IngressResult::BadPayload;
            if (offer.gameId != _start.gameId)
                return IngressResult::WrongGame;
            if (!compat::semanticallyEqual(
                    _config.compatibilityIdentity,
                    offer.compatibilityIdentity)
                || !compat::semanticallyEqual(
                    _start.compatibilityIdentity,
                    offer.compatibilityIdentity)) {
                return IngressResult::ContractMismatch;
            }
            _hasCheckpointOffer = true;
            _hasAssembledCheckpoint = false;
            _checkpointChunksSeen = 0;
            _checkpointChunkSeen.clear();
            _checkpointBuffer.clear();
            if (offer.totalBytes != 0
                && offer.totalBytes
                       <= kMaxCheckpointTotalBytes
                              + kCheckpointHeaderBytesV3
                              + kMaxCheckpointRegions
                                    * kCheckpointRegionEntryBytes
                              + kCheckpointTrailerBytes
                && offer.chunkCount != 0
                && offer.chunkCount <= kMaxCheckpointChunkCount) {
                _checkpointBuffer.assign(offer.totalBytes, 0u);
                _checkpointChunkSeen.assign(offer.chunkCount, false);
            }
            _checkpointOffer = offer;
            _lastValidReceiveMs = nowMs;
            return IngressResult::Accepted;
        }

        case MessageType::CheckpointChunk: {
            if (!_hasStart || !_hasCheckpointOffer
                || _phase != ViewerPhase::Streaming) {
                return IngressResult::UnexpectedMessage;
            }
            CheckpointChunkPayload chunk{};
            if (!decodePayload(datagram.payload, chunk))
                return IngressResult::BadPayload;
            if (chunk.gameId != _start.gameId)
                return IngressResult::WrongGame;
            if (chunk.checkpointId != _checkpointOffer.checkpointId
                || chunk.chunkCount != _checkpointOffer.chunkCount
                || chunk.chunkIndex >= _checkpointOffer.chunkCount) {
                return IngressResult::UnexpectedMessage;
            }
            if (!_checkpointBuffer.empty()
                && chunk.chunkIndex < _checkpointChunkSeen.size()
                && !_hasAssembledCheckpoint) {
                const size_t offset = chunk.byteOffset;
                if (offset + chunk.bytes.size() > _checkpointBuffer.size()
                    || chunk.bytes.empty()) {
                    return IngressResult::BadPayload;
                }
                if (!_checkpointChunkSeen[chunk.chunkIndex]) {
                    std::copy(chunk.bytes.begin(), chunk.bytes.end(),
                              _checkpointBuffer.begin()
                                  + static_cast<std::ptrdiff_t>(offset));
                    _checkpointChunkSeen[chunk.chunkIndex] = true;
                    ++_checkpointChunksSeen;
                }
                if (_checkpointChunksSeen == _checkpointOffer.chunkCount) {
                    Checkpoint decoded{};
                    if (decodeCheckpoint(_checkpointBuffer.data(),
                                         _checkpointBuffer.size(), decoded)
                            == CheckpointError::None
                        && decoded.identity.gameId == _start.gameId
                        && checkpointMatchesCompatibilityIdentity(
                               decoded, _config.compatibilityIdentity)
                        && decoded.eventCursorPresent) {
                        _assembledCheckpoint = std::move(decoded);
                        _hasAssembledCheckpoint = true;
                    } else {
                        _checkpointChunksSeen = 0;
                        _checkpointChunkSeen.assign(
                            _checkpointOffer.chunkCount, false);
                    }
                }
            }
            _lastValidReceiveMs = nowMs;
            return IngressResult::Accepted;
        }

        case MessageType::Reject: {
            if (_phase != ViewerPhase::Connecting
                && _phase != ViewerPhase::Streaming
                && _phase != ViewerPhase::Complete) {
                return IngressResult::UnexpectedMessage;
            }
            RejectPayload reject{};
            if (!decodePayload(datagram.payload, reject))
                return IngressResult::BadPayload;
            const uint64_t expectedGame =
                _hasStart ? _start.gameId : _requestedGameId;
            const bool gameMatches =
                reject.requestedGameId == expectedGame
                || reject.requestedGameId == kLatestGameId;
            if (reject.viewerId != _viewerId
                || reject.generation != _generation
                || reject.viewerNonce != _activeNonce
                || !gameMatches) {
                return IngressResult::InitMismatch;
            }
            _hasReject = true;
            _reject = reject;
            _phase = ViewerPhase::Rejected;
            _lastValidReceiveMs = nowMs;
            return IngressResult::Accepted;
        }

        case MessageType::Hello:
        case MessageType::InitAck:
        case MessageType::Ack:
        case MessageType::TerminalAck:
        case MessageType::Detach:
        case MessageType::Migrate:
            return IngressResult::WrongDirection;
    }
    return IngressResult::UnexpectedMessage;
}

std::vector<ViewerDatagram> SpectatorViewer::pollOutbound(
    uint64_t nowMs,
    size_t packetBudget) {
    std::vector<ViewerDatagram> out;
    if (!_configured || packetBudget == 0
        || _phase == ViewerPhase::Idle
        || _phase == ViewerPhase::Rejected
        || _phase == ViewerPhase::Failed
        || _phase == ViewerPhase::Detached) {
        return out;
    }

    if (_detachDirty) {
        DetachPayload detach{};
        detach.viewerId = _viewerId;
        detach.generation = _generation;
        detach.nextRecord = _receivedNext;
        detach.nextEvent = _appliedEvents;
        detach.digest = _receivedDigest;
        std::vector<uint8_t> payload;
        if (encodePayload(detach, payload)
            && appendEncoded(MessageType::Detach, payload, out)) {
            _detachDirty = false;
            _phase = ViewerPhase::Detached;
        }
        return out;
    }

    if (_phase == ViewerPhase::Connecting) {
        const bool due = !_helloSent
            || intervalElapsed(nowMs, _lastHelloSendMs,
                               _config.helloResendMs);
        if (!due) return out;
        std::vector<uint8_t> payload;
        if (_migrateDirty) {
            MigratePayload migrate{};
            migrate.viewerId = _viewerId;
            migrate.oldGeneration = _migrateOldGeneration;
            migrate.newGeneration = _generation;
            migrate.resumeRecord = _helloResumeRecord;
            migrate.resumeEvent = _helloResumeEvent;
            migrate.resumeDigest = _helloResumeDigest;
            migrate.grantId = _migrateGrantId;
            migrate.grantMacLow = _migrateGrantMacLow;
            migrate.grantMacHigh = _migrateGrantMacHigh;
            if (encodePayload(migrate, payload)
                && appendEncoded(MessageType::Migrate, payload, out)) {
                _helloSent = true;
                _lastHelloSendMs = nowMs;
            }
            return out;
        }
        HelloPayload hello{};
        hello.simulationContract = _config.simulationContract;
        hello.compatibilityIdentity = _config.compatibilityIdentity;
        hello.requestedGameId = _requestedGameId;
        hello.viewerNonce = _activeNonce;
        hello.viewerId = _viewerId;
        hello.generation = _generation;
        hello.resumeRecord = _helloResumeRecord;
        hello.resumeEvent = _helloResumeEvent;
        hello.resumeDigest = _helloResumeDigest;
        if (_config.wantsNewestCheckpoint)
            hello.flags |= kHelloFlagWantsNewestCheckpoint;
        if (encodePayload(hello, payload)
            && appendEncoded(MessageType::Hello, payload, out)) {
            _helloSent = true;
            _lastHelloSendMs = nowMs;
        }
        return out;
    }

    if (_pendingValidatedParentRoute.has_value()
        && _phase == ViewerPhase::Streaming && out.size() < packetBudget) {
        PendingValidatedParentRoute& transition =
            *_pendingValidatedParentRoute;
        const bool due = !transition.helloSent
            || intervalElapsed(nowMs, transition.lastHelloSendMs,
                               _config.helloResendMs);
        if (due && !transition.candidateIngressAccepted) {
            HelloPayload hello{};
            hello.simulationContract = _config.simulationContract;
            hello.compatibilityIdentity = _config.compatibilityIdentity;
            hello.requestedGameId = _requestedGameId;
            hello.viewerNonce = _activeNonce;
            hello.viewerId = _viewerId;
            hello.generation = _generation;
            hello.resumeRecord = transition.nextRecord;
            hello.resumeEvent = transition.nextEvent;
            hello.resumeDigest = transition.digest;
            std::vector<uint8_t> payload;
            if (encodePayload(hello, payload)
                && appendEncoded(MessageType::Hello, payload, out,
                                 ViewerDatagramRoute::ValidatedPendingParent,
                                 transition.activationId)) {
                transition.helloSent = true;
                transition.lastHelloSendMs = nowMs;
            }
        }
    }

    if (_phase == ViewerPhase::Streaming) {
        if (_initAckDirty && out.size() < packetBudget) {
            InitAckPayload ack{};
            ack.gameId = _start.gameId;
            ack.viewerNonce = _activeNonce;
            ack.viewerId = _viewerId;
            ack.generation = _generation;
            ack.startFingerprint = _startFingerprint;
            ack.resumeDigest = _helloResumeDigest == 0
                ? _start.initialDigest
                : _helloResumeDigest;
            ack.resumeRecord = _helloResumeRecord;
            ack.resumeEvent = _helloResumeEvent;
            std::vector<uint8_t> payload;
            if (encodePayload(ack, payload)
                && appendEncoded(MessageType::InitAck, payload, out)) {
                _initAckDirty = false;
            }
        }

        const bool ackDue = _ackDirty
            || (_ackSent
                && intervalElapsed(nowMs, _lastAckSendMs,
                                   _config.ackResendMs));
        if (ackDue && out.size() < packetBudget) {
            AckPayload ack{};
            ack.gameId = _start.gameId;
            ack.viewerId = _viewerId;
            ack.generation = _generation;
            ack.digest = _receivedDigest;
            ack.nextRecord = _receivedNext;
            ack.nextEvent = _appliedEvents;
            std::vector<uint8_t> payload;
            if (encodePayload(ack, payload)
                && appendEncoded(MessageType::Ack, payload, out)) {
                _ackDirty = false;
                _ackSent = true;
                _lastAckSendMs = nowMs;
            }
        }
    }

    if (_phase == ViewerPhase::Complete) {
        const bool resendDue = _terminalAckSent
            && _terminalAckSends < _config.terminalAckMaxSends
            && intervalElapsed(nowMs, _lastTerminalAckSendMs,
                               _config.terminalAckResendMs);
        if ((_terminalAckDirty || resendDue)
            && out.size() < packetBudget) {
            TerminalAckPayload ack{};
            ack.gameId = _start.gameId;
            ack.viewerId = _viewerId;
            ack.generation = _generation;
            ack.finalDigest = _terminal.finalDigest;
            ack.postTailStateHash = _terminal.postTailStateHash;
            ack.nextRecord = _terminal.nextRecord;
            ack.nextEvent = _terminal.nextEvent;
            ack.reason = _terminal.reason;
            std::vector<uint8_t> payload;
            if (encodePayload(ack, payload)
                && appendEncoded(MessageType::TerminalAck,
                                 payload, out)) {
                _terminalAckDirty = false;
                _terminalAckSent = true;
                ++_terminalAckSends;
                _lastTerminalAckSendMs = nowMs;
            }
        }
    }
    return out;
}

bool SpectatorViewer::peekPlaybackRecord(ConfirmedRecord& out) const {
    if (_config.relayIngestOnly
        || !_hasStart || _playbackNext >= _receivedNext
        || _playbackNext < _historyBase
        || _playbackNext - _historyBase >= _history.size()) {
        return false;
    }
    out = _history[_playbackNext - _historyBase];
    return true;
}

bool SpectatorViewer::gameStart(GameStartPayload& out) const {
    if (!_hasStart) return false;
    out = _start;
    return true;
}

bool SpectatorViewer::verifiedRecordAt(uint32_t record,
                                       ConfirmedRecord& out) const {
    if (!_hasStart || record >= _receivedNext) return false;
    if (!_config.relayIngestOnly) {
        if (record < _historyBase) return false;
        if (record - _historyBase >= _history.size()) return false;
        out = _history[record - _historyBase];
        return true;
    }
    if (_relayRecords.empty() || record < _relayFirstRecord) return false;
    const uint64_t offset =
        static_cast<uint64_t>(record) - _relayFirstRecord;
    if (offset >= _relayRecords.size()) return false;
    const ConfirmedRecord& retained =
        _relayRecords[static_cast<size_t>(offset)];
    if (retained.record != record) return false;
    out = retained;
    return true;
}

bool SpectatorViewer::verifiedSetEventAt(uint32_t event,
                                         SetEvent& out) const {
    if (!_hasStart || event >= _appliedEvents) return false;
    if (!_config.relayIngestOnly)
        return _setTimeline.event(event, out);
    if (_relayEvents.empty() || event < _relayFirstEvent) return false;
    const uint64_t offset =
        static_cast<uint64_t>(event) - _relayFirstEvent;
    if (offset >= _relayEvents.size()) return false;
    const SetEvent& retained = _relayEvents[static_cast<size_t>(offset)];
    if (retained.eventIndex != event) return false;
    out = retained;
    return true;
}

bool SpectatorViewer::verifiedRelayExportExtent(
    VerifiedRelayExportExtent& out) const {
    if (!_hasStart) return false;
    out.firstRecord = _config.relayIngestOnly
        ? (_relayRecords.empty() ? _receivedNext : _relayFirstRecord)
        : 0u;
    out.nextRecord = _receivedNext;
    out.firstEvent = _config.relayIngestOnly
        ? (_relayEvents.empty() ? _appliedEvents : _relayFirstEvent)
        : 0u;
    out.nextEvent = _appliedEvents;
    out.digest = _receivedDigest;
    return true;
}

bool SpectatorViewer::sourceStatus(HeartbeatPayload& out) const {
    if (!_hasHeartbeat) return false;
    out = _heartbeat;
    return true;
}

bool SpectatorViewer::checkpointOffer(CheckpointOfferPayload& out) const {
    if (!_hasCheckpointOffer) return false;
    out = _checkpointOffer;
    return true;
}

IngressResult SpectatorViewer::completeTailIfVerified() {
    if (!_hasTerminal || _playbackNext != _terminal.nextRecord)
        return IngressResult::Accepted;

    const uint64_t observedPostTail = _terminal.nextRecord == 0
        ? _start.initialStateHash
        : (_hasPlaybackPostState
           && _playbackPostNextRecord == _terminal.nextRecord
               ? _playbackPostStateHash
               : 0);
    if (_terminal.nextRecord != 0
        && (!_hasPlaybackPostState
            || _playbackPostNextRecord != _terminal.nextRecord)) {
        return IngressResult::Accepted;
    }
    if (observedPostTail != _terminal.postTailStateHash)
        return fail(IngressResult::StateHashMismatch);

    if (_phase != ViewerPhase::Complete) {
        _phase = ViewerPhase::Complete;
        _terminalAckDirty = true;
    }
    return IngressResult::Accepted;
}

IngressResult SpectatorViewer::commitPlaybackRecord(
    uint32_t record,
    uint64_t observedPreStateHash,
    uint64_t observedPostStateHash) {
    if (!_hasStart || _phase == ViewerPhase::Failed)
        return IngressResult::Failed;
    if (_config.relayIngestOnly)
        return IngressResult::UnexpectedMessage;
    if (record != _playbackNext || record >= _receivedNext
        || record < _historyBase
        || record - _historyBase >= _history.size()) {
        return IngressResult::UnexpectedMessage;
    }
    if (_history[record - _historyBase].stateHash != observedPreStateHash)
        return fail(IngressResult::StateHashMismatch);
    ++_playbackNext;
    _hasPlaybackPostState = true;
    _playbackPostNextRecord = _playbackNext;
    _playbackPostStateHash = observedPostStateHash;

    return completeTailIfVerified();
}

bool SpectatorViewer::terminal(ArchiveTerminal& out) const {
    if (!_hasTerminal) return false;
    out = _terminal;
    return true;
}

bool SpectatorViewer::rejection(RejectPayload& out) const {
    if (!_hasReject) return false;
    out = _reject;
    return true;
}

}
