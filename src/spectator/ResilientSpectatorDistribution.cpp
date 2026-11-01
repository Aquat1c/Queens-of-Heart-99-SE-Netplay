#include "ResilientSpectatorDistribution.h"

#include <algorithm>
#include <new>

namespace qnetplay::spectator::resilient {

bool StreamCompatibilityIdentity::valid() const {
    return setOrSessionId != 0
        && gameEpoch != 0
        && streamRootIdentity != 0
        && qnetplay::compat::isValidCompatibilityIdentity(compatibility);
}

bool StreamCompatibilityIdentity::operator==(
    const StreamCompatibilityIdentity& other) const {
    return setOrSessionId == other.setOrSessionId
        && gameEpoch == other.gameEpoch
        && qnetplay::compat::semanticallyEqual(compatibility,
                                                other.compatibility)
        && streamRootIdentity == other.streamRootIdentity;
}

bool makeStreamCompatibilityIdentity(
    const qnetplay::compat::CompatibilityIdentityWire& compatibility,
    uint64_t setOrSessionId, uint64_t gameEpoch, uint64_t streamRootIdentity,
    StreamCompatibilityIdentity& out) {
    out = {};
    if (setOrSessionId == 0 || gameEpoch == 0 || streamRootIdentity == 0
        || !qnetplay::compat::isValidCompatibilityIdentity(compatibility)) {
        return false;
    }
    out.setOrSessionId = setOrSessionId;
    out.gameEpoch = gameEpoch;
    out.compatibility = compatibility;
    out.streamRootIdentity = streamRootIdentity;
    return true;
}

bool RouteDescriptor::valid() const {
    return ipv4BE != 0 && port != 0 && generation != 0;
}

bool RouteDescriptor::sameEndpoint(const RouteDescriptor& other) const {
    return ipv4BE == other.ipv4BE && port == other.port;
}

bool ParentLeaseChildBinding::valid() const {
    return authorityGeneration != 0u && childViewerId != 0u
        && childViewerGeneration != 0u && childQspRoute.valid()
        && static_cast<uint8_t>(childQspRoute.kind)
            <= static_cast<uint8_t>(RouteKind::RootFallback)
        && childQspRoute.routeProved
        && childEndpointBindingGeneration != 0u
        && childRequestedCursor.digest != 0u;
}

bool ParentLeaseChildBinding::operator==(
    const ParentLeaseChildBinding& other) const {
    return authorityGeneration == other.authorityGeneration
        && childViewerId == other.childViewerId
        && childViewerGeneration == other.childViewerGeneration
        && childQspRoute.kind == other.childQspRoute.kind
        && childQspRoute.ipv4BE == other.childQspRoute.ipv4BE
        && childQspRoute.port == other.childQspRoute.port
        && childQspRoute.generation == other.childQspRoute.generation
        && childQspRoute.routeProved == other.childQspRoute.routeProved
        && childEndpointBindingGeneration
            == other.childEndpointBindingGeneration
        && childRequestedCursor.nextRecord
            == other.childRequestedCursor.nextRecord
        && childRequestedCursor.nextEvent
            == other.childRequestedCursor.nextEvent
        && childRequestedCursor.digest == other.childRequestedCursor.digest;
}

size_t ResilientLimits::requiredCacheRecords() const {
    return static_cast<size_t>(routeRaceRecordBudget)
        + static_cast<size_t>(continuityOverlapRecords)
        + static_cast<size_t>(retransmissionMarginRecords)
        + static_cast<size_t>(bootstrapMarginRecords);
}

bool ResilientLimits::valid() const {
    const bool recordsFit = maxCachedRecords
        <= maxCachedBytes / sizeof(VerifiedRelayRecord);
    const size_t recordBytes = recordsFit
        ? maxCachedRecords * sizeof(VerifiedRelayRecord)
        : 0u;
    const bool eventsFit = recordsFit && maxCachedEvents
        <= (maxCachedBytes - recordBytes) / sizeof(VerifiedRelayEvent);
    return maxTopologyNodes >= 2
        && maxActiveDepth > 0
        && maxActiveDepth <= kMaxTopologyAncestry
        && maxChildrenPerRoot > 0
        && maxChildrenPerRelay > 0
        && maxPendingChildren > 0
        && maxStandbyParents > 0
        && maxConcurrentRouteRaces > 0
        && maxConcurrentRouteRaces <= maxStandbyParents
        && routeRaceMs > 0
        && routeRaceRecordBudget > 0
        && continuityOverlapRecords > 0
        && retransmissionMarginRecords > 0
        && bootstrapMarginRecords > 0
        && maxCachedRecords >= requiredCacheRecords()
        && maxCachedEvents > 0
        && maxCheckpointBytes > 0
        && maxCheckpointBytes <= maxCachedBytes
        && recordsFit && eventsFit
        && maxCheckpoints > 0
        && standbyLeaseMs > 0
        && recoveryBudgetMs > 0;
}

VerifiedRelayCache::VerifiedRelayCache(const ResilientLimits& limits)
    : _limits(limits) {}

void VerifiedRelayCache::reset() {
    _configured = false;
    _stream = {};
    _gameId = 0;
    _sourceGeneration = 0;
    _range = {};
    _eventRange = {};
    _firstDigest = 0;
    _nextDigest = 0;
    _firstTimelineFrame = 0;
    _eventTimelineBound = false;
    _records.clear();
    _events.clear();
    _checkpoints.clear();
    _children.clear();
}

bool VerifiedRelayCache::bind(const StreamCompatibilityIdentity& stream,
                              uint64_t gameId, uint64_t initialDigest,
                              uint64_t sourceGeneration) {
    if (!stream.valid() || !_limits.valid() || gameId == 0
        || sourceGeneration == 0) {
        return false;
    }
    try {
        _records.clear();
        _events.clear();
        _checkpoints.clear();
        _children.clear();
        _records.reserve(_limits.maxCachedRecords);
        _events.reserve(_limits.maxCachedEvents);
        _checkpoints.reserve(_limits.maxCheckpoints);
        _children.reserve(_limits.maxChildrenPerRoot);
    } catch (const std::bad_alloc&) {
        reset();
        return false;
    }
    _stream = stream;
    _gameId = gameId;
    _sourceGeneration = sourceGeneration;
    _range = {};
    _eventRange = {};
    _firstDigest = initialDigest;
    _nextDigest = initialDigest;
    _firstTimelineFrame = 0;
    _eventTimelineBound = false;
    _configured = true;
    return true;
}

bool VerifiedRelayCache::bind(const StreamCompatibilityIdentity& stream,
                              uint64_t gameId, uint64_t initialDigest,
                              uint64_t sourceGeneration,
                              uint32_t firstTimelineFrame) {
    if (!bind(stream, gameId, initialDigest, sourceGeneration)) return false;
    _firstTimelineFrame = firstTimelineFrame;
    _eventTimelineBound = true;
    return true;
}

bool VerifiedRelayCache::configureEventTimeline(uint32_t firstTimelineFrame) {
    if (!_configured || !_events.empty()) return false;
    _firstTimelineFrame = firstTimelineFrame;
    _eventTimelineBound = true;
    return true;
}

size_t VerifiedRelayCache::retainedBytes() const {
    size_t bytes = _records.size() * sizeof(VerifiedRelayRecord);
    bytes += _events.size() * sizeof(VerifiedRelayEvent);
    for (const StoredCheckpoint& checkpoint : _checkpoints)
        bytes += checkpoint.bytes.size();
    return bytes;
}

CacheAppendResult VerifiedRelayCache::identityResult(
    const StreamCompatibilityIdentity& candidate) const {
    if (candidate.gameEpoch != _stream.gameEpoch)
        return CacheAppendResult::WrongEpoch;
    if (qnetplay::compat::compatibilityMismatch(candidate.compatibility,
                                                 _stream.compatibility)
        != qnetplay::compat::IdentityMismatch::None) {
        return CacheAppendResult::WrongContentIdentity;
    }
    return candidate == _stream ? CacheAppendResult::Accepted
                                : CacheAppendResult::WrongStream;
}

CacheEventAppendResult VerifiedRelayCache::eventIdentityResult(
    const StreamCompatibilityIdentity& candidate) const {
    switch (identityResult(candidate)) {
        case CacheAppendResult::Accepted:
            return CacheEventAppendResult::Accepted;
        case CacheAppendResult::WrongEpoch:
            return CacheEventAppendResult::WrongEpoch;
        case CacheAppendResult::WrongContentIdentity:
            return CacheEventAppendResult::WrongContentIdentity;
        case CacheAppendResult::WrongStream:
            return CacheEventAppendResult::WrongStream;
        default:
            return CacheEventAppendResult::WrongStream;
    }
}

CacheAppendResult VerifiedRelayCache::append(
    const RelayRecordEnvelope& envelope) {
    if (!_configured) return CacheAppendResult::NotConfigured;
    const CacheAppendResult identity = identityResult(envelope.stream);
    if (identity != CacheAppendResult::Accepted) return identity;
    if (envelope.gameId != _gameId) return CacheAppendResult::WrongGame;
    if (envelope.sourceGeneration != _sourceGeneration)
        return CacheAppendResult::WrongSourceGeneration;
    if (!envelope.confirmed) return CacheAppendResult::Speculative;

    const uint32_t cursor = envelope.record.record;
    if (cursor < _range.nextCursor) {
        if (!_range.containsRecord(cursor))
            return CacheAppendResult::ConflictingDuplicate;
        const VerifiedRelayRecord& stored =
            _records[static_cast<size_t>(cursor - _range.firstCursor)];
        return stored.record == envelope.record
                    && stored.sourceGeneration == envelope.sourceGeneration
            ? CacheAppendResult::ExactDuplicate
            : CacheAppendResult::ConflictingDuplicate;
    }
    if (cursor != _range.nextCursor) return CacheAppendResult::Gap;

    const uint64_t expectedDigest = advanceRollingDigest(
        _nextDigest, _gameId, envelope.record);
    if (envelope.record.rollingDigest != expectedDigest)
        return CacheAppendResult::DigestMismatch;
    while ((_records.size() >= _limits.maxCachedRecords
            || retainedBytes() + sizeof(VerifiedRelayRecord)
                   > _limits.maxCachedBytes)
           && evictOldestRecordBoundary()) {
    }
    if (_records.size() >= _limits.maxCachedRecords
        || retainedBytes() + sizeof(VerifiedRelayRecord)
               > _limits.maxCachedBytes) {
        return CacheAppendResult::MemoryLimit;
    }
    try {
        _records.push_back(VerifiedRelayRecord{
            envelope.record, envelope.sourceGeneration});
    } catch (const std::bad_alloc&) {
        return CacheAppendResult::MemoryLimit;
    }
    _nextDigest = expectedDigest;
    ++_range.nextCursor;
    evictToBounds();
    return CacheAppendResult::Accepted;
}

CacheEventAppendResult VerifiedRelayCache::appendEvent(
    const RelayEventEnvelope& envelope) {
    if (!_configured) return CacheEventAppendResult::NotConfigured;
    const CacheEventAppendResult identity = eventIdentityResult(envelope.stream);
    if (identity != CacheEventAppendResult::Accepted) return identity;
    if (envelope.gameId != _gameId) return CacheEventAppendResult::WrongGame;
    if (envelope.sourceGeneration != _sourceGeneration)
        return CacheEventAppendResult::WrongSourceGeneration;
    if (!envelope.confirmed) return CacheEventAppendResult::Speculative;
    if (!_eventTimelineBound)
        return CacheEventAppendResult::TimelineUnbound;

    const SetEvent& event = envelope.event;
    if (event.eventIndex < _eventRange.nextEvent) {
        if (!_eventRange.containsEvent(event.eventIndex))
            return CacheEventAppendResult::ConflictingDuplicate;
        const VerifiedRelayEvent& stored = _events[static_cast<size_t>(
            event.eventIndex - _eventRange.firstEvent)];
        return stored.event == event
                    && stored.sourceGeneration == envelope.sourceGeneration
            ? CacheEventAppendResult::ExactDuplicate
            : CacheEventAppendResult::ConflictingDuplicate;
    }
    if (event.eventIndex != _eventRange.nextEvent)
        return CacheEventAppendResult::Gap;
    // QSP folds all events at a boundary before admitting that boundary's record; any other order changes a sealed chain position.
    if (event.recordCursor != _range.nextCursor)
        return CacheEventAppendResult::BoundaryMismatch;
    const uint32_t expectedFrame = _firstTimelineFrame + event.recordCursor;
    if (expectedFrame < _firstTimelineFrame
        || event.timelineFrame != expectedFrame
        || !isValidSetEvent(event)) {
        return CacheEventAppendResult::TimelineMismatch;
    }
    if (event.recordDigest != _nextDigest)
        return CacheEventAppendResult::DigestMismatch;
    const uint64_t expectedDigest = advanceSetEventDigest(
        _nextDigest, _gameId, event);
    if (event.rollingDigest != expectedDigest)
        return CacheEventAppendResult::DigestMismatch;
    while ((_events.size() >= _limits.maxCachedEvents
            || retainedBytes() + sizeof(VerifiedRelayEvent)
                   > _limits.maxCachedBytes)
           && evictOldestRecordBoundary()) {
    }
    if (_events.size() >= _limits.maxCachedEvents
        || retainedBytes() + sizeof(VerifiedRelayEvent)
               > _limits.maxCachedBytes) {
        return CacheEventAppendResult::MemoryLimit;
    }
    try {
        _events.push_back(VerifiedRelayEvent{event, envelope.sourceGeneration});
    } catch (const std::bad_alloc&) {
        return CacheEventAppendResult::MemoryLimit;
    }
    _nextDigest = expectedDigest;
    ++_eventRange.nextEvent;
    return CacheEventAppendResult::Accepted;
}

CacheCheckpointResult VerifiedRelayCache::addCheckpoint(
    const VerifiedCheckpoint& checkpoint) {
    if (!_configured) return CacheCheckpointResult::NotConfigured;
    if (checkpoint.stream != _stream) return CacheCheckpointResult::WrongStream;
    if (checkpoint.gameId != _gameId) return CacheCheckpointResult::WrongGame;
    if (checkpoint.sourceGeneration != _sourceGeneration)
        return CacheCheckpointResult::WrongSourceGeneration;
    if (!checkpoint.complete) return CacheCheckpointResult::Incomplete;
    if (!checkpoint.integrityVerified)
        return CacheCheckpointResult::IntegrityUnverified;
    if (checkpoint.bytes.size() > _limits.maxCheckpointBytes)
        return CacheCheckpointResult::MemoryLimit;
    uint64_t expectedDigest = 0;
    if (!digestAt(checkpoint.cursor, checkpoint.eventCursor, expectedDigest))
        return CacheCheckpointResult::CursorUnavailable;
    if (checkpoint.digest != expectedDigest)
        return CacheCheckpointResult::DigestMismatch;

    for (StoredCheckpoint& stored : _checkpoints) {
        if (stored.cursor != checkpoint.cursor) continue;
        if (stored.eventCursor != checkpoint.eventCursor
            || stored.digest != checkpoint.digest
            || stored.sourceGeneration != checkpoint.sourceGeneration) {
            return CacheCheckpointResult::DigestMismatch;
        }
        if (retainedBytes() - stored.bytes.size() + checkpoint.bytes.size()
            > _limits.maxCachedBytes) {
            return CacheCheckpointResult::MemoryLimit;
        }
        try {
            stored.bytes = checkpoint.bytes;
        } catch (const std::bad_alloc&) {
            return CacheCheckpointResult::MemoryLimit;
        }
        return CacheCheckpointResult::Accepted;
    }

    while (!_checkpoints.empty()
           && (_checkpoints.size() >= _limits.maxCheckpoints
               || retainedBytes() + checkpoint.bytes.size()
                      > _limits.maxCachedBytes)) {
        _checkpoints.erase(_checkpoints.begin());
    }
    if (retainedBytes() + checkpoint.bytes.size() > _limits.maxCachedBytes)
        return CacheCheckpointResult::MemoryLimit;
    try {
        StoredCheckpoint stored{};
        stored.cursor = checkpoint.cursor;
        stored.eventCursor = checkpoint.eventCursor;
        stored.digest = checkpoint.digest;
        stored.sourceGeneration = checkpoint.sourceGeneration;
        stored.bytes = checkpoint.bytes;
        _checkpoints.push_back(std::move(stored));
    } catch (const std::bad_alloc&) {
        return CacheCheckpointResult::MemoryLimit;
    }
    return CacheCheckpointResult::Accepted;
}

bool VerifiedRelayCache::attachChild(NodeId childId,
                                     uint32_t acknowledgedCursor) {
    uint32_t acknowledgedEvent = 0;
    uint64_t acknowledgedDigest = 0;
    return eventCursorAt(acknowledgedCursor, acknowledgedEvent)
        && digestAt(acknowledgedCursor, acknowledgedEvent,
                    acknowledgedDigest)
        && attachChild(childId, acknowledgedCursor, acknowledgedEvent,
                       acknowledgedDigest);
}

bool VerifiedRelayCache::attachChild(NodeId childId,
                                     uint32_t acknowledgedCursor,
                                     uint32_t acknowledgedEvent) {
    uint64_t acknowledgedDigest = 0;
    if (!_configured || childId == kInvalidNodeId
        || !digestAt(acknowledgedCursor, acknowledgedEvent,
                      acknowledgedDigest)) {
        return false;
    }
    return attachChild(childId, acknowledgedCursor, acknowledgedEvent,
                       acknowledgedDigest);
}

bool VerifiedRelayCache::attachChild(NodeId childId,
                                     uint32_t acknowledgedCursor,
                                     uint32_t acknowledgedEvent,
                                     uint64_t acknowledgedDigest) {
    uint64_t expectedDigest = 0;
    if (!_configured || childId == kInvalidNodeId
        || !digestAt(acknowledgedCursor, acknowledgedEvent, expectedDigest)
        || acknowledgedDigest != expectedDigest) {
        return false;
    }
    for (CacheChildStatus& existing : _children) {
        if (existing.childId != childId) continue;
        if (acknowledgedCursor < existing.acknowledgedCursor
            || acknowledgedEvent < existing.acknowledgedEvent) {
            return false;
        }
        existing.acknowledgedCursor = acknowledgedCursor;
        existing.acknowledgedEvent = acknowledgedEvent;
        if (existing.acknowledgedCursor >= _range.firstCursor
            && existing.acknowledgedEvent >= _eventRange.firstEvent) {
            existing.controlledResyncRequired = false;
        }
        return true;
    }
    if (_children.size() >= _limits.maxChildrenPerRoot) return false;
    try {
        _children.push_back(CacheChildStatus{
            childId, acknowledgedCursor, acknowledgedEvent,
            acknowledgedCursor < _range.firstCursor
                || acknowledgedEvent < _eventRange.firstEvent,
        });
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

bool VerifiedRelayCache::detachChild(NodeId childId) {
    const auto erased = std::remove_if(
        _children.begin(), _children.end(), [childId](const CacheChildStatus& child) {
            return child.childId == childId;
        });
    if (erased == _children.end()) return false;
    _children.erase(erased, _children.end());
    return true;
}

bool VerifiedRelayCache::acknowledgeChild(NodeId childId, uint32_t nextCursor) {
    uint32_t nextEvent = 0;
    uint64_t acknowledgedDigest = 0;
    return eventCursorAt(nextCursor, nextEvent)
        && digestAt(nextCursor, nextEvent, acknowledgedDigest)
        && acknowledgeChild(childId, nextCursor, nextEvent,
                            acknowledgedDigest);
}

bool VerifiedRelayCache::acknowledgeChild(NodeId childId, uint32_t nextCursor,
                                          uint32_t nextEvent) {
    uint64_t acknowledgedDigest = 0;
    if (!digestAt(nextCursor, nextEvent, acknowledgedDigest)) return false;
    return acknowledgeChild(childId, nextCursor, nextEvent,
                            acknowledgedDigest);
}

bool VerifiedRelayCache::acknowledgeChild(NodeId childId, uint32_t nextCursor,
                                          uint32_t nextEvent,
                                          uint64_t acknowledgedDigest) {
    if (!canAcknowledgeChild(childId, nextCursor, nextEvent,
                             acknowledgedDigest)) return false;
    for (CacheChildStatus& status : _children) {
        if (status.childId != childId) continue;
        status.acknowledgedCursor = nextCursor;
        status.acknowledgedEvent = nextEvent;
        if (nextCursor >= _range.firstCursor
            && nextEvent >= _eventRange.firstEvent) {
            status.controlledResyncRequired = false;
        }
        return true;
    }
    return false;
}

bool VerifiedRelayCache::canAcknowledgeChild(
    NodeId childId, uint32_t nextCursor, uint32_t nextEvent,
    uint64_t acknowledgedDigest) const {
    uint64_t expectedDigest = 0;
    if (!digestAt(nextCursor, nextEvent, expectedDigest)
        || acknowledgedDigest != expectedDigest) {
        return false;
    }
    const CacheChildStatus* status = child(childId);
    return status != nullptr && nextCursor <= _range.nextCursor
        && nextCursor >= status->acknowledgedCursor
        && nextEvent >= status->acknowledgedEvent;
}

const CacheChildStatus* VerifiedRelayCache::child(NodeId childId) const {
    for (const CacheChildStatus& status : _children)
        if (status.childId == childId) return &status;
    return nullptr;
}

CacheReadResult VerifiedRelayCache::readForChild(
    NodeId childId, uint32_t startCursor, size_t maxRecords,
    std::vector<VerifiedRelayRecord>& out) const {
    out.clear();
    const CacheChildStatus* status = child(childId);
    if (status == nullptr) return CacheReadResult::UnknownChild;
    if (maxRecords == 0 || maxRecords > _limits.maxCachedRecords)
        return CacheReadResult::InvalidRequest;
    if (status->controlledResyncRequired || startCursor < _range.firstCursor)
        return CacheReadResult::ControlledResync;
    if (startCursor > _range.nextCursor) return CacheReadResult::FutureCursor;
    uint32_t requiredEvent = 0;
    if (!eventCursorAt(startCursor, requiredEvent))
        return CacheReadResult::InvalidRequest;
    if (status->acknowledgedEvent < requiredEvent)
        return CacheReadResult::EventCatchupRequired;
    size_t available = static_cast<size_t>(_range.nextCursor - startCursor);
    size_t take = (std::min)(maxRecords, available);
    // QSP input batches must not cross a set-event boundary, or a record is forwarded without its shared-chain prerequisite.
    for (const VerifiedRelayEvent& stored : _events) {
        if (stored.event.recordCursor <= startCursor) continue;
        const size_t beforeEvent = static_cast<size_t>(
            stored.event.recordCursor - startCursor);
        take = (std::min)(take, beforeEvent);
        break;
    }
    try {
        out.reserve(take);
        for (size_t index = 0; index < take; ++index) {
            out.push_back(_records[static_cast<size_t>(
                startCursor - _range.firstCursor) + index]);
        }
    } catch (const std::bad_alloc&) {
        out.clear();
        return CacheReadResult::InvalidRequest;
    }
    return CacheReadResult::Accepted;
}

CacheReadResult VerifiedRelayCache::readEventsForChild(
    NodeId childId, uint32_t startEvent, size_t maxEvents,
    std::vector<VerifiedRelayEvent>& out) const {
    out.clear();
    const CacheChildStatus* status = child(childId);
    if (status == nullptr) return CacheReadResult::UnknownChild;
    if (maxEvents == 0 || maxEvents > _limits.maxCachedEvents)
        return CacheReadResult::InvalidRequest;
    if (status->controlledResyncRequired
        || startEvent < _eventRange.firstEvent) {
        return CacheReadResult::ControlledResync;
    }
    if (startEvent > _eventRange.nextEvent)
        return CacheReadResult::FutureCursor;
    const size_t available = static_cast<size_t>(
        _eventRange.nextEvent - startEvent);
    const size_t take = (std::min)(maxEvents, available);
    try {
        out.reserve(take);
        for (size_t index = 0; index < take; ++index) {
            out.push_back(_events[static_cast<size_t>(
                startEvent - _eventRange.firstEvent) + index]);
        }
    } catch (const std::bad_alloc&) {
        out.clear();
        return CacheReadResult::InvalidRequest;
    }
    return CacheReadResult::Accepted;
}

uint32_t VerifiedRelayCache::eventCursorBefore(uint32_t recordCursor) const {
    uint32_t cursor = _eventRange.firstEvent;
    for (const VerifiedRelayEvent& stored : _events) {
        if (stored.event.recordCursor >= recordCursor) break;
        cursor = stored.event.eventIndex + 1u;
    }
    return cursor;
}

uint32_t VerifiedRelayCache::eventCursorAtOrBefore(
    uint32_t recordCursor) const {
    uint32_t cursor = _eventRange.firstEvent;
    for (const VerifiedRelayEvent& stored : _events) {
        if (stored.event.recordCursor > recordCursor) break;
        cursor = stored.event.eventIndex + 1u;
    }
    return cursor;
}

bool VerifiedRelayCache::eventCursorAt(uint32_t recordCursor,
                                        uint32_t& out) const {
    out = 0;
    if (!_configured || !_range.containsBoundary(recordCursor)) return false;
    out = eventCursorAtOrBefore(recordCursor);
    return true;
}

bool VerifiedRelayCache::digestAt(uint32_t cursor, uint64_t& digest) const {
    uint32_t eventCursor = 0;
    return eventCursorAt(cursor, eventCursor)
        && digestAt(cursor, eventCursor, digest);
}

bool VerifiedRelayCache::digestAt(uint32_t recordCursor,
                                  uint32_t eventCursor,
                                  uint64_t& digest) const {
    if (!_configured || !_range.containsBoundary(recordCursor)
        || !_eventRange.containsBoundary(eventCursor)) {
        return false;
    }
    const uint32_t before = eventCursorBefore(recordCursor);
    const uint32_t throughBoundary = eventCursorAtOrBefore(recordCursor);
    if (eventCursor < before || eventCursor > throughBoundary) return false;
    if (eventCursor > before) {
        const size_t index = static_cast<size_t>(
            eventCursor - _eventRange.firstEvent - 1u);
        if (index >= _events.size()
            || _events[index].event.recordCursor != recordCursor) {
            return false;
        }
        digest = _events[index].event.rollingDigest;
        return true;
    }
    if (recordCursor == _range.firstCursor) {
        digest = _firstDigest;
        return true;
    }
    const size_t index = static_cast<size_t>(
        recordCursor - _range.firstCursor - 1u);
    if (index >= _records.size()) return false;
    digest = _records[index].record.rollingDigest;
    return true;
}

bool VerifiedRelayCache::continuityProof(uint32_t exactNextCursor,
                                         uint32_t overlapRecords,
                                         ContinuityProof& out) const {
    uint32_t exactNextEvent = 0;
    return eventCursorAt(exactNextCursor, exactNextEvent)
        && continuityProof(exactNextCursor, exactNextEvent, overlapRecords,
                           out);
}

bool VerifiedRelayCache::continuityProof(uint32_t exactNextCursor,
                                         uint32_t exactNextEvent,
                                         uint32_t overlapRecords,
                                         ContinuityProof& out) const {
    out = {};
    if (!_configured || exactNextCursor < _range.firstCursor
        || exactNextCursor > _range.nextCursor
        || overlapRecords > exactNextCursor - _range.firstCursor) {
        return false;
    }
    const uint32_t overlapCursor = exactNextCursor - overlapRecords;
    uint32_t overlapEvent = 0;
    if (!eventCursorAt(overlapCursor, overlapEvent)) return false;
    uint64_t overlapDigest = 0;
    if (!digestAt(exactNextCursor, exactNextEvent, overlapDigest))
        return false;
    if (!digestAt(overlapCursor, overlapEvent, overlapDigest)) return false;
    out.stream = _stream;
    out.gameId = _gameId;
    out.sourceGeneration = _sourceGeneration;
    out.cacheFirstCursor = _range.firstCursor;
    out.cacheNextCursor = _range.nextCursor;
    out.cacheFirstEvent = _eventRange.firstEvent;
    out.cacheNextEvent = _eventRange.nextEvent;
    out.overlapCursor = overlapCursor;
    out.overlapEvent = overlapEvent;
    out.overlapDigest = overlapDigest;
    (void)newestCheckpointIdentity(out.cacheNextCursor, out.cacheNextEvent,
                                   out.checkpointCursor, out.checkpointEvent,
                                   out.checkpointDigest);
    out.available = true;
    return true;
}

bool VerifiedRelayCache::newestCheckpointIdentity(
    const uint32_t maxCursor, const uint32_t maxEvent, uint32_t& cursor,
    uint32_t& eventCursor, uint64_t& digest) const {
    cursor = 0u;
    eventCursor = 0u;
    digest = 0u;
    if (!_configured) return false;
    const StoredCheckpoint* newest = nullptr;
    for (const StoredCheckpoint& stored : _checkpoints) {
        if (stored.sourceGeneration != _sourceGeneration) continue;
        if (stored.cursor < _range.firstCursor
            || stored.cursor > maxCursor
            || stored.eventCursor > maxEvent || stored.digest == 0u) {
            continue;
        }
        if (newest == nullptr || stored.cursor > newest->cursor
            || (stored.cursor == newest->cursor
                && stored.eventCursor > newest->eventCursor)) {
            newest = &stored;
        }
    }
    if (newest == nullptr) return false;
    cursor = newest->cursor;
    eventCursor = newest->eventCursor;
    digest = newest->digest;
    return true;
}

bool VerifiedRelayCache::adoptSourceGeneration(
    uint64_t sourceGeneration, uint32_t exactNextCursor,
    uint32_t overlapCursor, uint64_t overlapDigest) {
    uint32_t exactNextEvent = 0;
    uint32_t overlapEvent = 0;
    if (!eventCursorAt(exactNextCursor, exactNextEvent)
        || !eventCursorAt(overlapCursor, overlapEvent)
        || !adoptSourceGeneration(sourceGeneration, exactNextCursor,
                                  exactNextEvent, overlapCursor,
                                  overlapEvent, overlapDigest)) {
        return false;
    }
    return true;
}

bool VerifiedRelayCache::adoptSourceGeneration(
    uint64_t sourceGeneration, uint32_t exactNextCursor,
    uint32_t exactNextEvent, uint32_t overlapCursor,
    uint32_t overlapEvent, uint64_t overlapDigest) {
    if (!canAdoptSourceGeneration(sourceGeneration, exactNextCursor,
                                  exactNextEvent, overlapCursor,
                                  overlapEvent, overlapDigest)) {
        return false;
    }
    _sourceGeneration = sourceGeneration;
    return true;
}

bool VerifiedRelayCache::canAdoptSourceGeneration(
    uint64_t sourceGeneration, uint32_t exactNextCursor,
    uint32_t overlapCursor, uint64_t overlapDigest) const {
    uint32_t exactNextEvent = 0;
    uint32_t overlapEvent = 0;
    return eventCursorAt(exactNextCursor, exactNextEvent)
        && eventCursorAt(overlapCursor, overlapEvent)
        && canAdoptSourceGeneration(sourceGeneration, exactNextCursor,
                                    exactNextEvent, overlapCursor,
                                    overlapEvent, overlapDigest);
}

bool VerifiedRelayCache::canAdoptSourceGeneration(
    uint64_t sourceGeneration, uint32_t exactNextCursor,
    uint32_t exactNextEvent, uint32_t overlapCursor,
    uint32_t overlapEvent, uint64_t overlapDigest) const {
    if (!_configured || sourceGeneration == 0
        || sourceGeneration < _sourceGeneration
        || exactNextCursor != _range.nextCursor
        || exactNextEvent != _eventRange.nextEvent
        || overlapCursor < _range.firstCursor
        || overlapCursor > exactNextCursor) {
        return false;
    }
    uint32_t expectedOverlapEvent = 0;
    if (!eventCursorAt(overlapCursor, expectedOverlapEvent)
        || overlapEvent != expectedOverlapEvent) {
        return false;
    }
    uint64_t expectedDigest = 0;
    if (!digestAt(overlapCursor, overlapEvent, expectedDigest)
        || expectedDigest != overlapDigest) {
        return false;
    }
    return true;
}

bool VerifiedRelayCache::evictOldestRecordBoundary() {
    if (_records.empty()) return false;
    _firstDigest = _records.front().record.rollingDigest;
    _records.erase(_records.begin());
    ++_range.firstCursor;
    trimEventsToRecordRange();
    trimCheckpointsToBounds();
    markLaggingChildrenForResync();
    return true;
}

void VerifiedRelayCache::evictToBounds() {
    while (_records.size() > _limits.maxCachedRecords)
        (void)evictOldestRecordBoundary();
    trimCheckpointsToBounds();
    markLaggingChildrenForResync();
}

void VerifiedRelayCache::trimEventsToRecordRange() {
    while (!_events.empty()
           && _events.front().event.recordCursor < _range.firstCursor) {
        _eventRange.firstEvent = _events.front().event.eventIndex + 1u;
        _events.erase(_events.begin());
    }
    if (_events.empty()) _eventRange.firstEvent = _eventRange.nextEvent;
}

void VerifiedRelayCache::trimCheckpointsToBounds() {
    const auto firstUnavailable = std::remove_if(
        _checkpoints.begin(), _checkpoints.end(), [this](const StoredCheckpoint& checkpoint) {
            uint64_t digest = 0;
            return !digestAt(checkpoint.cursor, checkpoint.eventCursor,
                             digest);
        });
    _checkpoints.erase(firstUnavailable, _checkpoints.end());
    while (!_checkpoints.empty()
           && (_checkpoints.size() > _limits.maxCheckpoints
               || retainedBytes() > _limits.maxCachedBytes)) {
        _checkpoints.erase(_checkpoints.begin());
    }
}

void VerifiedRelayCache::markLaggingChildrenForResync() {
    for (CacheChildStatus& child : _children) {
        if (child.acknowledgedCursor < _range.firstCursor
            || child.acknowledgedEvent < _eventRange.firstEvent) {
            child.controlledResyncRequired = true;
        }
    }
}

RelaySocketOwnership::~RelaySocketOwnership() { close(); }

bool RelaySocketOwnership::adopt(RelaySocketHandle upstream,
                                 RelaySocketHandle downstream,
                                 RelaySocketCloseFn closeFn,
                                 void* closeContext) {
    if (configured()) return false;
    if (upstream == kInvalidRelaySocket
        || downstream == kInvalidRelaySocket
        || upstream == downstream) {
        // Ownership transfers at the call boundary: a partial/bad pair is released exactly once here.
        if (closeFn != nullptr && upstream != kInvalidRelaySocket)
            closeFn(upstream, closeContext);
        if (closeFn != nullptr && downstream != kInvalidRelaySocket
            && downstream != upstream) {
            closeFn(downstream, closeContext);
        }
        return false;
    }
    _upstream = upstream;
    _downstream = downstream;
    _closeFn = closeFn;
    _closeContext = closeContext;
    return true;
}

void RelaySocketOwnership::close() {
    const RelaySocketHandle upstream = _upstream;
    const RelaySocketHandle downstream = _downstream;
    RelaySocketCloseFn closeFn = _closeFn;
    void* context = _closeContext;
    _upstream = kInvalidRelaySocket;
    _downstream = kInvalidRelaySocket;
    _closeFn = nullptr;
    _closeContext = nullptr;
    if (closeFn == nullptr) return;
    if (upstream != kInvalidRelaySocket) closeFn(upstream, context);
    if (downstream != kInvalidRelaySocket && downstream != upstream)
        closeFn(downstream, context);
}

VerifiedRelayDownstream::VerifiedRelayDownstream(const ResilientLimits& limits)
    : _ownedCache(limits), _cache(&_ownedCache) {}

VerifiedRelayDownstream::~VerifiedRelayDownstream() { stop(); }

bool VerifiedRelayDownstream::start(
    const StreamCompatibilityIdentity& stream, uint64_t gameId,
    uint64_t initialDigest, uint64_t sourceGeneration,
    RelaySocketHandle upstream, RelaySocketHandle downstream,
    RelaySocketCloseFn closeFn, void* closeContext) {
    if (_running) return false;
    _cache = &_ownedCache;
    _borrowedCache = false;
    if (!_sockets.adopt(upstream, downstream, closeFn, closeContext))
        return false;
    if (!_ownedCache.bind(stream, gameId, initialDigest, sourceGeneration)) {
        _sockets.close();
        return false;
    }
    _running = true;
    return true;
}

bool VerifiedRelayDownstream::startWithCache(
    VerifiedRelayCache& cache, RelaySocketHandle upstream,
    RelaySocketHandle downstream, RelaySocketCloseFn closeFn,
    void* closeContext) {
    if (_running || !cache.configured()) return false;
    if (!_sockets.adopt(upstream, downstream, closeFn, closeContext))
        return false;
    _cache = &cache;
    _borrowedCache = true;
    _running = true;
    return true;
}

void VerifiedRelayDownstream::stop() {
    _running = false;
    _sockets.close();
    if (!_borrowedCache) _ownedCache.reset();
    _cache = &_ownedCache;
    _borrowedCache = false;
}

CacheAppendResult VerifiedRelayDownstream::acceptUpstream(
    const RelayRecordEnvelope& envelope) {
    return _running ? _cache->append(envelope)
                    : CacheAppendResult::NotConfigured;
}

CacheEventAppendResult VerifiedRelayDownstream::acceptUpstreamEvent(
    const RelayEventEnvelope& envelope) {
    return _running ? _cache->appendEvent(envelope)
                    : CacheEventAppendResult::NotConfigured;
}

CacheReadResult VerifiedRelayDownstream::serveChild(
    NodeId childId, uint32_t startCursor, size_t maxRecords,
    std::vector<VerifiedRelayRecord>& out) const {
    if (!_running) {
        out.clear();
        return CacheReadResult::InvalidRequest;
    }
    return _cache->readForChild(childId, startCursor, maxRecords, out);
}

CacheReadResult VerifiedRelayDownstream::serveChildEvents(
    NodeId childId, uint32_t startEvent, size_t maxEvents,
    std::vector<VerifiedRelayEvent>& out) const {
    if (!_running) {
        out.clear();
        return CacheReadResult::InvalidRequest;
    }
    return _cache->readEventsForChild(childId, startEvent, maxEvents, out);
}

namespace {

constexpr size_t kParentLeaseBodyBytes = 512;

bool putU8(std::array<uint8_t, kParentLeaseBodyBytes>& out, size_t& cursor,
           uint8_t value) {
    if (cursor >= out.size()) return false;
    out[cursor++] = value;
    return true;
}

bool putU32(std::array<uint8_t, kParentLeaseBodyBytes>& out, size_t& cursor,
            uint32_t value) {
    if (cursor + 4 > out.size()) return false;
    for (unsigned byte = 0; byte < 4; ++byte)
        out[cursor++] = static_cast<uint8_t>(value >> (byte * 8u));
    return true;
}

bool putU64(std::array<uint8_t, kParentLeaseBodyBytes>& out, size_t& cursor,
            uint64_t value) {
    if (cursor + 8 > out.size()) return false;
    for (unsigned byte = 0; byte < 8; ++byte)
        out[cursor++] = static_cast<uint8_t>(value >> (byte * 8u));
    return true;
}

bool putBytes(std::array<uint8_t, kParentLeaseBodyBytes>& out, size_t& cursor,
              const uint8_t* bytes, size_t count) {
    if ((bytes == nullptr && count != 0u) || cursor + count > out.size())
        return false;
    for (size_t index = 0; index < count; ++index)
        out[cursor++] = bytes[index];
    return true;
}

bool putCompatibilityIdentity(
    std::array<uint8_t, kParentLeaseBodyBytes>& out, size_t& cursor,
    const qnetplay::compat::CompatibilityIdentityWire& identity) {
    using namespace qnetplay::compat;
    return putU32(out, cursor, identity.schemaVersion)
        && putU32(out, cursor, identity.protocolVersion)
        && putBytes(out, cursor,
                    reinterpret_cast<const uint8_t*>(
                        identity.runtimeBuild.profileId),
                    kCompatibilityProfileIdBytes)
        && putBytes(out, cursor, identity.runtimeBuild.executableSha256,
                    kCompatibilitySha256Bytes)
        && putU64(out, cursor, identity.runtimeBuild.executableSize)
        && putU32(out, cursor, identity.runtimeBuild.peTimestamp)
        && putU32(out, cursor,
                  identity.simulationContent.manifestSchemaVersion)
        && putU32(out, cursor, identity.simulationContent.manifestFlags)
        && putBytes(out, cursor,
                    identity.simulationContent.simulationRootSha256,
                    kCompatibilitySha256Bytes)
        && putBytes(out, cursor,
                    identity.simulationContent.validationRootSha256,
                    kCompatibilitySha256Bytes)
        && putU32(out, cursor, identity.simulationContent.entryCount)
        && putU32(out, cursor,
                  identity.simulationContent.requiredEntryCount)
        && putU64(out, cursor, identity.stateSchemaId)
        && putU64(out, cursor, identity.modSimulationId)
        && putU64(out, cursor, identity.timingProfileId)
        && putU32(out, cursor, identity.reserved);
}

uint64_t getLe64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned byte = 0; byte < 8; ++byte)
        value |= static_cast<uint64_t>(bytes[byte]) << (byte * 8u);
    return value;
}

bool routeKindValid(RouteKind kind) {
    return static_cast<uint8_t>(kind) <= static_cast<uint8_t>(RouteKind::RootFallback);
}

bool leaseShapeValid(const ParentLease& lease) {
    if (!lease.stream.valid() || lease.childId == kInvalidNodeId
        || lease.parentId == kInvalidNodeId || lease.childId == lease.parentId
        || !lease.childBinding.valid()
        || lease.parentSourceGeneration == 0 || lease.routeGeneration == 0
        || lease.routeCount == 0 || lease.routeCount > kMaxLeaseRoutes
        || lease.cacheRange.firstCursor > lease.cacheRange.nextCursor
        || lease.latestVerifiedCursor != lease.cacheRange.nextCursor
        || lease.childCapacity == 0 || lease.expiryMs == 0
        || lease.replayNonce == 0 || lease.ancestryCount > kMaxLeaseAncestry) {
        return false;
    }
    for (size_t index = 0; index < lease.routeCount; ++index) {
        const RouteDescriptor& route = lease.routes[index];
        if (!routeKindValid(route.kind) || !route.valid()
            || route.generation != lease.routeGeneration) {
            return false;
        }
    }
    for (size_t index = 0; index < lease.ancestryCount; ++index) {
        if (lease.ancestry[index] == kInvalidNodeId
            || lease.ancestry[index] == lease.childId) {
            return false;
        }
        for (size_t prior = 0; prior < index; ++prior)
            if (lease.ancestry[prior] == lease.ancestry[index]) return false;
    }
    return true;
}

bool encodeParentLeaseBody(
    const ParentLease& lease,
    std::array<uint8_t, kParentLeaseBodyBytes>& out) {
    if (!leaseShapeValid(lease)) return false;
    out.fill(0);
    size_t cursor = 0;
    if (!putU64(out, cursor, lease.stream.setOrSessionId)
        || !putU64(out, cursor, lease.stream.gameEpoch)
        || !putCompatibilityIdentity(out, cursor, lease.stream.compatibility)
        || !putU64(out, cursor, lease.stream.streamRootIdentity)
        || !putU64(out, cursor, lease.childId)
        || !putU64(out, cursor, lease.parentId)
        || !putU64(out, cursor, lease.childBinding.authorityGeneration)
        || !putU64(out, cursor, lease.childBinding.childViewerId)
        || !putU32(out, cursor, lease.childBinding.childViewerGeneration)
        || !putU8(out, cursor,
                  static_cast<uint8_t>(lease.childBinding.childQspRoute.kind))
        || !putU32(out, cursor, lease.childBinding.childQspRoute.ipv4BE)
        || !putU32(out, cursor,
                   static_cast<uint32_t>(lease.childBinding.childQspRoute.port))
        || !putU64(out, cursor, lease.childBinding.childQspRoute.generation)
        || !putU8(out, cursor,
                  lease.childBinding.childQspRoute.routeProved ? 1u : 0u)
        || !putU64(out, cursor,
                   lease.childBinding.childEndpointBindingGeneration)
        || !putU32(out, cursor,
                   lease.childBinding.childRequestedCursor.nextRecord)
        || !putU32(out, cursor,
                   lease.childBinding.childRequestedCursor.nextEvent)
        || !putU64(out, cursor,
                   lease.childBinding.childRequestedCursor.digest)
        || !putU8(out, cursor, lease.parentDepth)
        || !putU64(out, cursor, lease.parentSourceGeneration)
        || !putU64(out, cursor, lease.routeGeneration)
        || !putU8(out, cursor, lease.routeCount)) {
        return false;
    }
    for (const RouteDescriptor& route : lease.routes) {
        if (!putU8(out, cursor, static_cast<uint8_t>(route.kind))
            || !putU32(out, cursor, route.ipv4BE)
            || !putU32(out, cursor, route.port)
            || !putU64(out, cursor, route.generation)
            || !putU8(out, cursor, route.routeProved ? 1u : 0u)) {
            return false;
        }
    }
    if (!putU32(out, cursor, lease.cacheRange.firstCursor)
        || !putU32(out, cursor, lease.cacheRange.nextCursor)
        || !putU32(out, cursor, lease.latestVerifiedCursor)
        || !putU32(out, cursor, lease.childCapacity)
        || !putU64(out, cursor, lease.expiryMs)
        || !putU64(out, cursor, lease.replayNonce)
        || !putU8(out, cursor, lease.ancestryCount)) {
        return false;
    }
    for (NodeId ancestor : lease.ancestry)
        if (!putU64(out, cursor, ancestor)) return false;
    return cursor <= out.size();
}

bool equalMac(uint64_t leftLow, uint64_t leftHigh,
              uint64_t rightLow, uint64_t rightHigh) {
    const uint64_t difference = (leftLow ^ rightLow) | (leftHigh ^ rightHigh);
    return difference == 0;
}

bool proofBindsExactLease(const LeaseBinding& binding,
                          const ParentLease& lease) {
    return binding.childId == lease.childId
        && binding.parentId == lease.parentId
        && binding.parentSourceGeneration == lease.parentSourceGeneration
        && binding.routeGeneration == lease.routeGeneration
        && binding.expiryMs == lease.expiryMs
        && binding.replayNonce == lease.replayNonce
        && binding.macLow == lease.macLow
        && binding.macHigh == lease.macHigh
        && binding.childBinding == lease.childBinding;
}

}

ParentLeaseRegistry::ParentLeaseRegistry(const SpectatorGrantKey& key,
                                         size_t maxRememberedNonces)
    : _key(key), _maxRememberedNonces(maxRememberedNonces) {
    _redeemed.reserve(maxRememberedNonces);
}

bool ParentLeaseRegistry::issue(ParentLease& lease) const {
    std::array<uint8_t, kParentLeaseBodyBytes> body{};
    if (!encodeParentLeaseBody(lease, body)) return false;
    std::array<uint8_t, 32> digest{};
    hmacSha256(_key.data(), _key.size(), body.data(), body.size(), digest);
    lease.macLow = getLe64(digest.data());
    lease.macHigh = getLe64(digest.data() + 8);
    return true;
}

ParentLeaseVerdict ParentLeaseRegistry::verify(
    const ParentLease& lease, const ParentLeaseExpectation& expectation) const {
    const ParentLeaseVerdict verified = verifyContinuity(lease, expectation);
    if (verified != ParentLeaseVerdict::Accepted) return verified;
    return wasRedeemed(lease.replayNonce) ? ParentLeaseVerdict::Replayed
                                          : ParentLeaseVerdict::Accepted;
}

ParentLeaseVerdict ParentLeaseRegistry::verifyContinuity(
    const ParentLease& lease, const ParentLeaseExpectation& expectation) const {
    std::array<uint8_t, kParentLeaseBodyBytes> body{};
    if (!encodeParentLeaseBody(lease, body)) return ParentLeaseVerdict::Malformed;
    std::array<uint8_t, 32> digest{};
    hmacSha256(_key.data(), _key.size(), body.data(), body.size(), digest);
    if (!equalMac(lease.macLow, lease.macHigh, getLe64(digest.data()),
                  getLe64(digest.data() + 8))) {
        return ParentLeaseVerdict::BadMac;
    }
    if (lease.expiryMs <= expectation.nowMs) return ParentLeaseVerdict::Expired;
    if (lease.stream != expectation.stream) return ParentLeaseVerdict::WrongStream;
    if (lease.childId != expectation.expectedChildId)
        return ParentLeaseVerdict::WrongChild;
    if (lease.parentId != expectation.expectedParentId)
        return ParentLeaseVerdict::WrongParent;
    if (lease.childBinding != expectation.expectedChildBinding)
        return ParentLeaseVerdict::ChildBindingMismatch;
    if (lease.parentSourceGeneration != expectation.minimumSourceGeneration)
        return ParentLeaseVerdict::StaleSourceGeneration;
    if (lease.routeGeneration != expectation.minimumRouteGeneration)
        return ParentLeaseVerdict::StaleRouteGeneration;
    if (!lease.cacheRange.containsBoundary(expectation.requiredCursor)
        || lease.latestVerifiedCursor < expectation.requiredCursor) {
        return ParentLeaseVerdict::CursorUnavailable;
    }
    if (!expectation.parentHasCapacity || lease.childCapacity == 0)
        return ParentLeaseVerdict::CapacityReached;
    if (!expectation.loopFree) return ParentLeaseVerdict::LoopRejected;
    if (expectation.maxActiveDepth == 0
        || lease.parentDepth >= expectation.maxActiveDepth) {
        return ParentLeaseVerdict::DepthExceeded;
    }
    return ParentLeaseVerdict::Accepted;
}

bool ParentLeaseRegistry::wasRedeemed(const uint64_t replayNonce) const {
    return std::any_of(_redeemed.begin(), _redeemed.end(),
                       [replayNonce](const RedeemedNonce& redeemed) {
                           return redeemed.nonce == replayNonce;
                       });
}

ParentLeaseVerdict ParentLeaseRegistry::redeem(
    const ParentLease& lease, const ParentLeaseExpectation& expectation) {
    expire(expectation.nowMs);
    const ParentLeaseVerdict verified = verify(lease, expectation);
    if (verified != ParentLeaseVerdict::Accepted) return verified;
    if (_redeemed.size() >= _maxRememberedNonces)
        return ParentLeaseVerdict::ReplayMemoryBound;
    try {
        _redeemed.push_back(RedeemedNonce{lease.replayNonce, lease.expiryMs});
    } catch (const std::bad_alloc&) {
        return ParentLeaseVerdict::ReplayMemoryBound;
    }
    return ParentLeaseVerdict::Accepted;
}

void ParentLeaseRegistry::expire(uint64_t nowMs) {
    const auto expired = std::remove_if(
        _redeemed.begin(), _redeemed.end(), [nowMs](const RedeemedNonce& nonce) {
            return nonce.expiryMs <= nowMs;
        });
    _redeemed.erase(expired, _redeemed.end());
}

ResilientTopologyCoordinator::ResilientTopologyCoordinator(
    const StreamCompatibilityIdentity& stream, const ResilientLimits& limits)
    : _configured(stream.valid() && limits.valid()),
      _stream(stream),
      _limits(limits) {
    if (_configured) _nodes.reserve(_limits.maxTopologyNodes);
}

NodeRegistrationResult ResilientTopologyCoordinator::registerNode(
    const NodeAdvertisement& advertisement) {
    if (!_configured || advertisement.nodeId == kInvalidNodeId)
        return NodeRegistrationResult::InvalidNode;
    if (advertisement.stream != _stream) return NodeRegistrationResult::InvalidIdentity;
    if (advertisement.capabilityModelRevision
        != kResilientCapabilityModelRevision) {
        return NodeRegistrationResult::UnsupportedCapabilityModel;
    }
    const RouteDescriptor& controlRoute = advertisement.resolvedControlRoute();
    if (!advertisement.route.valid() || !controlRoute.valid()
        || advertisement.sourceGeneration == 0
        || advertisement.routeGeneration == 0
        || advertisement.route.generation != advertisement.routeGeneration
        || controlRoute.generation != advertisement.routeGeneration
        || advertisement.cacheFirstCursor > advertisement.cacheNextCursor) {
        return NodeRegistrationResult::InvalidNode;
    }

    TopologyNodeView* existing = findMutable(advertisement.nodeId);
    if (existing != nullptr) {
        if (existing->advertisement.kind != advertisement.kind
            || existing->advertisement.stream != advertisement.stream) {
            return NodeRegistrationResult::NodeIdConflict;
        }
        const RouteDescriptor oldRoute = existing->advertisement.route;
        const RouteDescriptor oldControlRoute =
            existing->advertisement.resolvedControlRoute();
        existing->advertisement = advertisement;
        existing->activeParent = existing->activeParent;
        if (!oldRoute.sameEndpoint(advertisement.route)
            || oldRoute.generation != advertisement.route.generation
            || !oldControlRoute.sameEndpoint(controlRoute)
            || oldControlRoute.generation != controlRoute.generation) {
            return NodeRegistrationResult::UpdatedRoute;
        }
        return NodeRegistrationResult::Accepted;
    }
    if (_nodes.size() >= _limits.maxTopologyNodes)
        return NodeRegistrationResult::CapacityReached;

    TopologyNodeView node{};
    node.advertisement = advertisement;
    _nodes.push_back(node);
    return NodeRegistrationResult::Accepted;
}

bool ResilientTopologyCoordinator::refreshRoute(NodeId nodeId,
                                                const RouteDescriptor& route,
                                                uint64_t routeGeneration) {
    TopologyNodeView* node = findMutable(nodeId);
    if (node == nullptr || !route.valid()
        || routeGeneration == 0
        || routeGeneration < node->advertisement.routeGeneration) {
        return false;
    }
    node->advertisement.route = route;
    node->advertisement.routeGeneration = routeGeneration;
    return true;
}

bool ResilientTopologyCoordinator::setDraining(NodeId nodeId, bool draining) {
    TopologyNodeView* node = findMutable(nodeId);
    if (node == nullptr) return false;
    node->advertisement.draining = draining;
    return true;
}

bool ResilientTopologyCoordinator::seedExternalAncestorPrefix(
    const NodeId nodeId, const uint8_t depth,
    const std::array<NodeId, kMaxLeaseAncestry>& ancestry,
    const uint8_t ancestryCount) {
    TopologyNodeView* const node = findMutable(nodeId);
    if (!_configured || node == nullptr
        || node->activeParent != kInvalidNodeId || !node->children.empty()
        || ancestryCount != depth || ancestryCount > ancestry.size()
        || depth > _limits.maxActiveDepth) {
        return false;
    }
    for (size_t index = 0u; index < ancestry.size(); ++index) {
        const NodeId ancestor = ancestry[index];
        if (index >= ancestryCount) {
            if (ancestor != kInvalidNodeId) return false;
            continue;
        }
        if (ancestor == kInvalidNodeId || ancestor == nodeId) return false;
        for (size_t previous = 0u; previous < index; ++previous) {
            if (ancestry[previous] == ancestor) return false;
        }
    }
    node->depth = depth;
    node->ancestry = ancestry;
    node->ancestryCount = ancestryCount;
    return true;
}

TopologyAttachResult ResilientTopologyCoordinator::attachRootChild(
    NodeId rootId, NodeId childId) {
    const TopologyAttachResult eligible =
        attachmentEligibility(rootId, childId, true);
    if (eligible != TopologyAttachResult::Attached) return eligible;
    return attachChild(rootId, childId);
}

TopologyAttachResult ResilientTopologyCoordinator::attachChild(
    NodeId parentId, NodeId childId) {
    const TopologyAttachResult eligible =
        attachmentEligibility(parentId, childId, false);
    if (eligible != TopologyAttachResult::Attached) return eligible;
    TopologyNodeView* parent = findMutable(parentId);
    TopologyNodeView* child = findMutable(childId);
    if (parent == nullptr || child == nullptr) return TopologyAttachResult::UnknownNode;
    if (child->activeParent == parentId) return TopologyAttachResult::Attached;
    try {
        parent->children.push_back(childId);
    } catch (const std::bad_alloc&) {
        return TopologyAttachResult::ParentAtCapacity;
    }
    child->activeParent = parentId;
    child->depth = static_cast<uint8_t>(parent->depth + 1u);
    child->ancestry = parent->ancestry;
    child->ancestryCount = parent->ancestryCount;
    child->ancestry[child->ancestryCount++] = parentId;
    return TopologyAttachResult::Attached;
}

TopologyAttachResult ResilientTopologyCoordinator::replaceParentAfterContinuity(
    NodeId parentId, NodeId childId) {
    const TopologyAttachResult eligible = attachmentEligibility(
        parentId, childId, false, true);
    if (eligible != TopologyAttachResult::Attached) return eligible;
    TopologyNodeView* parent = findMutable(parentId);
    TopologyNodeView* child = findMutable(childId);
    if (parent == nullptr || child == nullptr) return TopologyAttachResult::UnknownNode;
    if (child->activeParent == parentId) return TopologyAttachResult::Attached;
    if (static_cast<size_t>(parent->depth) + 1u + subtreeHeight(*child)
        > _limits.maxActiveDepth) {
        return TopologyAttachResult::DepthExceeded;
    }
    try {
        parent->children.push_back(childId);
    } catch (const std::bad_alloc&) {
        return TopologyAttachResult::ParentAtCapacity;
    }
    if (child->activeParent != kInvalidNodeId) {
        TopologyNodeView* former = findMutable(child->activeParent);
        if (former != nullptr) {
            const auto erased = std::remove(former->children.begin(),
                                            former->children.end(), childId);
            former->children.erase(erased, former->children.end());
        }
    }
    child->activeParent = parentId;
    child->depth = static_cast<uint8_t>(parent->depth + 1u);
    child->ancestry = parent->ancestry;
    child->ancestryCount = parent->ancestryCount;
    child->ancestry[child->ancestryCount++] = parentId;
    rewriteDescendantTopology(*child);
    return TopologyAttachResult::Attached;
}

bool ResilientTopologyCoordinator::detach(NodeId childId) {
    TopologyNodeView* child = findMutable(childId);
    if (child == nullptr || child->activeParent == kInvalidNodeId) return false;
    TopologyNodeView* parent = findMutable(child->activeParent);
    if (parent != nullptr) {
        const auto erased = std::remove(parent->children.begin(),
                                        parent->children.end(), childId);
        parent->children.erase(erased, parent->children.end());
    }
    child->activeParent = kInvalidNodeId;
    child->depth = 0;
    child->ancestry = {};
    child->ancestryCount = 0;
    return true;
}

std::vector<ParentSelection> ResilientTopologyCoordinator::selectStandbyParents(
    const ParentSelectionRequest& request) const {
    std::vector<ParentSelection> selected;
    const TopologyNodeView* child = find(request.childId);
    if (!_configured || child == nullptr || isRoot(*child)
        || request.maxOffers == 0) {
        return selected;
    }
    struct Ranked {
        ParentSelection selection{};
        unsigned tier = 0;
        uint8_t depth = 0;
        uint32_t lossPerMille = 0;
        uint32_t rttMs = 0;
    };
    std::vector<Ranked> ranked;
    const TopologyNodeView* activeParent =
        child->activeParent == kInvalidNodeId ? nullptr
                                              : find(child->activeParent);
    const NodeId activeBranch = activeParent == nullptr
        ? kInvalidNodeId : branchRoot(*activeParent);
    for (const TopologyNodeView& parent : _nodes) {
        if (parent.advertisement.nodeId == child->advertisement.nodeId
            || parent.advertisement.stream != _stream
            || isRoot(*child)
            || !parent.advertisement.reachable
            || parent.advertisement.draining
            || parent.advertisement.lastProgressMs == 0
            || parent.advertisement.uploadCapacity == 0
            || parent.advertisement.uploadInUse
                   >= parent.advertisement.uploadCapacity
            || parent.children.size() >= childLimit(parent)
            || parent.depth >= _limits.maxActiveDepth
            || !parent.advertisement.route.valid()
            || parent.advertisement.sourceGeneration
                   < request.minimumSourceGeneration
            || parent.advertisement.routeGeneration
                   < request.minimumRouteGeneration
            || !VerifiedCacheRange{
                    parent.advertisement.cacheFirstCursor,
                    parent.advertisement.cacheNextCursor
               }.containsBoundary(request.requiredCursor)
            || wouldCreateLoop(parent, child->advertisement.nodeId)) {
            continue;
        }
        const NodeId candidateBranch = branchRoot(parent);
        const bool trafficFallback = parent.advertisement.kind
                == NodeKind::TrafficRelay
            || parent.advertisement.route.kind == RouteKind::TrafficRelay;
        const bool diverse = activeBranch != kInvalidNodeId
            && candidateBranch != kInvalidNodeId
            && candidateBranch != activeBranch;
        Ranked item{};
        item.selection.parentId = parent.advertisement.nodeId;
        item.selection.route = parent.advertisement.route;
        item.selection.branchDiverse = diverse;
        item.selection.trafficRelayFallback = trafficFallback;
        item.tier = trafficFallback ? 2u : (diverse ? 0u : 1u);
        item.depth = parent.depth;
        item.lossPerMille = parent.advertisement.lossPerMille;
        item.rttMs = parent.advertisement.rttMs;
        ranked.push_back(item);
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& left,
                                                const Ranked& right) {
        if (left.tier != right.tier) return left.tier < right.tier;
        if (left.depth != right.depth) return left.depth < right.depth;
        if (left.lossPerMille != right.lossPerMille)
            return left.lossPerMille < right.lossPerMille;
        if (left.rttMs != right.rttMs) return left.rttMs < right.rttMs;
        return left.selection.parentId < right.selection.parentId;
    });
    const size_t count = (std::min)(
        (std::min)(request.maxOffers, _limits.maxStandbyParents), ranked.size());
    selected.reserve(count);
    for (size_t index = 0; index < count; ++index)
        selected.push_back(ranked[index].selection);
    return selected;
}

bool ResilientTopologyCoordinator::makeUnsignedParentOffer(
    NodeId childId, NodeId parentId, uint32_t requiredCursor,
    uint64_t expiryMs, uint64_t replayNonce, ParentLease& out) const {
    out = {};
    return false;
}

bool ResilientTopologyCoordinator::makeUnsignedParentOffer(
    NodeId childId, NodeId parentId, uint32_t requiredCursor,
    uint64_t expiryMs, uint64_t replayNonce,
    const ParentLeaseChildBinding& childBinding, ParentLease& out) const {
    out = {};
    const TopologyNodeView* child = find(childId);
    const TopologyNodeView* parent = find(parentId);
    if (!childBinding.valid() || child == nullptr || parent == nullptr
        || isRoot(*child)
        || childBinding.childRequestedCursor.nextRecord != requiredCursor
        || childBinding.childQspRoute.kind != child->advertisement.route.kind
        || !childBinding.childQspRoute.sameEndpoint(child->advertisement.route)
        || childBinding.childQspRoute.generation
               != child->advertisement.route.generation
        || childBinding.childQspRoute.routeProved
               != child->advertisement.route.routeProved
        || expiryMs == 0 || replayNonce == 0
        || !parent->advertisement.reachable
        || parent->advertisement.draining
        || parent->advertisement.lastProgressMs == 0
        || parent->advertisement.uploadCapacity == 0
        || parent->advertisement.uploadInUse
               >= parent->advertisement.uploadCapacity
        || parent->children.size() >= childLimit(*parent)
        || parent->depth >= _limits.maxActiveDepth
        || !parent->advertisement.route.valid()
        || !VerifiedCacheRange{parent->advertisement.cacheFirstCursor,
                               parent->advertisement.cacheNextCursor}
                .containsBoundary(requiredCursor)
        || wouldCreateLoop(*parent, childId)) {
        return false;
    }
    out.stream = _stream;
    out.childId = childId;
    out.parentId = parentId;
    out.childBinding = childBinding;
    out.parentDepth = parent->depth;
    out.parentSourceGeneration = parent->advertisement.sourceGeneration;
    out.routeGeneration = parent->advertisement.routeGeneration;
    out.routes[0] = parent->advertisement.route;
    out.routes[0].generation = out.routeGeneration;
    out.routeCount = 1;
    out.cacheRange = {parent->advertisement.cacheFirstCursor,
                      parent->advertisement.cacheNextCursor};
    out.latestVerifiedCursor = out.cacheRange.nextCursor;
    out.childCapacity = static_cast<uint32_t>(
        childLimit(*parent) - parent->children.size());
    out.expiryMs = expiryMs;
    out.replayNonce = replayNonce;
    if (parent->ancestryCount >= out.ancestry.size()) return false;
    out.ancestry = parent->ancestry;
    out.ancestry[parent->ancestryCount] = parentId;
    out.ancestryCount = static_cast<uint8_t>(parent->ancestryCount + 1u);
    return true;
}

ParentLeaseVerdict ResilientTopologyCoordinator::redeemAndAttach(
    ParentLeaseRegistry&, const ParentLease&, uint64_t, uint32_t) {
    return ParentLeaseVerdict::ChildBindingMismatch;
}

ParentLeaseVerdict ResilientTopologyCoordinator::redeemAndAttach(
    ParentLeaseRegistry& registry, const ParentLease& lease,
    uint64_t nowMs, uint32_t requiredCursor,
    const ParentLeaseChildBinding& expectedChildBinding) {
    const TopologyAttachResult eligible = attachmentEligibility(
        lease.parentId, lease.childId, false);
    if (eligible != TopologyAttachResult::Attached)
        return ParentLeaseVerdict::TopologyRejected;
    const TopologyNodeView* parent = find(lease.parentId);
    if (parent == nullptr) return ParentLeaseVerdict::TopologyRejected;
    ParentLeaseExpectation expectation{};
    expectation.stream = _stream;
    expectation.expectedChildId = lease.childId;
    expectation.expectedParentId = lease.parentId;
    expectation.expectedChildBinding = expectedChildBinding;
    expectation.nowMs = nowMs;
    expectation.minimumSourceGeneration = parent->advertisement.sourceGeneration;
    expectation.minimumRouteGeneration = parent->advertisement.routeGeneration;
    expectation.requiredCursor = requiredCursor;
    expectation.maxActiveDepth = _limits.maxActiveDepth;
    expectation.parentHasCapacity = parent->children.size() < childLimit(*parent);
    expectation.loopFree = !wouldCreateLoop(*parent, lease.childId);
    const ParentLeaseVerdict redeemed = registry.redeem(lease, expectation);
    if (redeemed != ParentLeaseVerdict::Accepted) return redeemed;
    return attachChild(lease.parentId, lease.childId)
            == TopologyAttachResult::Attached
        ? ParentLeaseVerdict::Accepted
        : ParentLeaseVerdict::TopologyRejected;
}

ParentLeaseVerdict ResilientTopologyCoordinator::validateStandbyLease(
    const ParentLeaseRegistry&, const ParentLease&, uint64_t,
    uint32_t) const {
    return ParentLeaseVerdict::ChildBindingMismatch;
}

ParentLeaseVerdict ResilientTopologyCoordinator::validateStandbyLease(
    const ParentLeaseRegistry& registry, const ParentLease& lease,
    uint64_t nowMs, uint32_t requiredCursor,
    const ParentLeaseChildBinding& expectedChildBinding) const {
    const TopologyNodeView* parent = find(lease.parentId);
    const TopologyNodeView* child = find(lease.childId);
    if (parent == nullptr || child == nullptr)
        return ParentLeaseVerdict::TopologyRejected;
    if (parent->advertisement.lastProgressMs == 0
        || parent->advertisement.uploadCapacity == 0
        || parent->advertisement.uploadInUse
               >= parent->advertisement.uploadCapacity) {
        return ParentLeaseVerdict::TopologyRejected;
    }
    const TopologyAttachResult eligible = attachmentEligibility(
        lease.parentId, lease.childId, false,
        true);
    if (eligible == TopologyAttachResult::ParentIsDescendant)
        return ParentLeaseVerdict::LoopRejected;
    if (eligible == TopologyAttachResult::DepthExceeded)
        return ParentLeaseVerdict::DepthExceeded;
    if (eligible == TopologyAttachResult::ParentAtCapacity)
        return ParentLeaseVerdict::CapacityReached;
    if (eligible != TopologyAttachResult::Attached)
        return ParentLeaseVerdict::TopologyRejected;
    ParentLeaseExpectation expectation{};
    expectation.stream = _stream;
    expectation.expectedChildId = child->advertisement.nodeId;
    expectation.expectedParentId = parent->advertisement.nodeId;
    expectation.expectedChildBinding = expectedChildBinding;
    expectation.nowMs = nowMs;
    expectation.minimumSourceGeneration = parent->advertisement.sourceGeneration;
    expectation.minimumRouteGeneration = parent->advertisement.routeGeneration;
    expectation.requiredCursor = requiredCursor;
    expectation.maxActiveDepth = _limits.maxActiveDepth;
    expectation.parentHasCapacity = parent->children.size() < childLimit(*parent)
        || child->activeParent == parent->advertisement.nodeId;
    expectation.loopFree = !wouldCreateLoop(*parent, child->advertisement.nodeId);
    return registry.verify(lease, expectation);
}

ParentLeaseVerdict
ResilientTopologyCoordinator::redeemAndReparentAfterContinuity(
    ParentLeaseRegistry&, const ParentLease&, uint64_t, uint32_t) {
    return ParentLeaseVerdict::ChildBindingMismatch;
}

ParentLeaseVerdict
ResilientTopologyCoordinator::redeemAndReparentAfterContinuity(
    ParentLeaseRegistry& registry, const ParentLease& lease,
    uint64_t nowMs, uint32_t requiredCursor,
    const ParentLeaseChildBinding& expectedChildBinding) {
    const TopologyAttachResult eligible = attachmentEligibility(
        lease.parentId, lease.childId, false,
        true);
    if (eligible != TopologyAttachResult::Attached)
        return ParentLeaseVerdict::TopologyRejected;
    const TopologyNodeView* parent = find(lease.parentId);
    if (parent == nullptr || parent->advertisement.lastProgressMs == 0
        || parent->advertisement.uploadCapacity == 0
        || parent->advertisement.uploadInUse
               >= parent->advertisement.uploadCapacity) {
        return ParentLeaseVerdict::TopologyRejected;
    }
    ParentLeaseExpectation expectation{};
    expectation.stream = _stream;
    expectation.expectedChildId = lease.childId;
    expectation.expectedParentId = lease.parentId;
    expectation.expectedChildBinding = expectedChildBinding;
    expectation.nowMs = nowMs;
    expectation.minimumSourceGeneration = parent->advertisement.sourceGeneration;
    expectation.minimumRouteGeneration = parent->advertisement.routeGeneration;
    expectation.requiredCursor = requiredCursor;
    expectation.maxActiveDepth = _limits.maxActiveDepth;
    expectation.parentHasCapacity = parent->children.size() < childLimit(*parent)
        || find(lease.childId)->activeParent == lease.parentId;
    expectation.loopFree = !wouldCreateLoop(*parent, lease.childId);
    const ParentLeaseVerdict redeemed = registry.redeem(lease, expectation);
    if (redeemed != ParentLeaseVerdict::Accepted) return redeemed;
    return replaceParentAfterContinuity(lease.parentId, lease.childId)
            == TopologyAttachResult::Attached
        ? ParentLeaseVerdict::Accepted
        : ParentLeaseVerdict::TopologyRejected;
}

const TopologyNodeView* ResilientTopologyCoordinator::find(NodeId nodeId) const {
    for (const TopologyNodeView& node : _nodes)
        if (node.advertisement.nodeId == nodeId) return &node;
    return nullptr;
}

size_t ResilientTopologyCoordinator::rootCount() const {
    size_t count = 0;
    for (const TopologyNodeView& node : _nodes)
        if (isRoot(node)) ++count;
    return count;
}

size_t ResilientTopologyCoordinator::rootChildCount(NodeId rootId) const {
    const TopologyNodeView* root = find(rootId);
    return root != nullptr && isRoot(*root) ? root->children.size() : 0;
}

bool ResilientTopologyCoordinator::isRoot(const TopologyNodeView& node) const {
    return node.advertisement.kind == NodeKind::HostRoot
        || node.advertisement.kind == NodeKind::ClientRoot;
}

size_t ResilientTopologyCoordinator::childLimit(
    const TopologyNodeView& node) const {
    if ((node.advertisement.capabilities & kCapabilityTopologyLeaf) != 0u)
        return 0u;
    const size_t policy = isRoot(node) ? _limits.maxChildrenPerRoot
                                       : _limits.maxChildrenPerRelay;
    return node.advertisement.childCapacity == 0
        ? policy
        : (std::min)(policy, node.advertisement.childCapacity);
}

TopologyAttachResult ResilientTopologyCoordinator::attachmentEligibility(
    NodeId parentId, NodeId childId, bool requireRoot,
    bool permitReparent) const {
    const TopologyNodeView* parent = find(parentId);
    const TopologyNodeView* child = find(childId);
    if (parent == nullptr || child == nullptr) return TopologyAttachResult::UnknownNode;
    if (parent->advertisement.stream != child->advertisement.stream)
        return TopologyAttachResult::StreamMismatch;
    if (parentId == childId) return TopologyAttachResult::SelfParent;
    if (requireRoot && !isRoot(*parent)) return TopologyAttachResult::ParentNotRoot;
    if (isRoot(*child)) return TopologyAttachResult::ChildIsRoot;
    if (!parent->advertisement.reachable)
        return TopologyAttachResult::ParentUnreachable;
    if (parent->advertisement.draining)
        return TopologyAttachResult::ParentDraining;
    if (child->activeParent == parentId) return TopologyAttachResult::Attached;
    if (!permitReparent && child->activeParent != kInvalidNodeId)
        return TopologyAttachResult::AlreadyAttachedElsewhere;
    if (wouldCreateLoop(*parent, childId))
        return TopologyAttachResult::ParentIsDescendant;
    if (parent->children.size() >= childLimit(*parent))
        return TopologyAttachResult::ParentAtCapacity;
    if (parent->depth >= _limits.maxActiveDepth)
        return TopologyAttachResult::DepthExceeded;
    return TopologyAttachResult::Attached;
}

bool ResilientTopologyCoordinator::wouldCreateLoop(
    const TopologyNodeView& parent, NodeId childId) const {
    if (parent.advertisement.nodeId == childId) return true;
    for (size_t index = 0; index < parent.ancestryCount; ++index)
        if (parent.ancestry[index] == childId) return true;
    return false;
}

NodeId ResilientTopologyCoordinator::branchRoot(
    const TopologyNodeView& node) const {
    if (isRoot(node)) return node.advertisement.nodeId;
    for (size_t index = 0; index < node.ancestryCount; ++index) {
        const TopologyNodeView* ancestor = find(node.ancestry[index]);
        if (ancestor != nullptr && isRoot(*ancestor))
            return ancestor->advertisement.nodeId;
    }
    return kInvalidNodeId;
}

size_t ResilientTopologyCoordinator::subtreeHeight(
    const TopologyNodeView& node) const {
    size_t height = 0;
    for (NodeId childId : node.children) {
        const TopologyNodeView* child = find(childId);
        if (child == nullptr) continue;
        height = (std::max)(height, 1u + subtreeHeight(*child));
    }
    return height;
}

void ResilientTopologyCoordinator::rewriteDescendantTopology(
    TopologyNodeView& node) {
    for (NodeId childId : node.children) {
        TopologyNodeView* child = findMutable(childId);
        if (child == nullptr) continue;
        child->activeParent = node.advertisement.nodeId;
        child->depth = static_cast<uint8_t>(node.depth + 1u);
        child->ancestry = node.ancestry;
        child->ancestryCount = node.ancestryCount;
        child->ancestry[child->ancestryCount++] = node.advertisement.nodeId;
        rewriteDescendantTopology(*child);
    }
}

ResilientRelayNode::ResilientRelayNode(
    NodeId nodeId, const StreamCompatibilityIdentity& stream,
    uint64_t gameId, uint64_t initialDigest,
    ResilientTopologyCoordinator& topology, ParentLeaseRegistry& leases,
    const ResilientLimits& limits)
    : _nodeId(nodeId), _stream(stream), _gameId(gameId),
      _initialDigest(initialDigest), _limits(limits), _topology(&topology),
      _leases(&leases), _cache(limits) {}

bool ResilientRelayNode::start(uint64_t sourceGeneration) {
    const TopologyNodeView* self = _topology == nullptr ? nullptr
                                                        : _topology->find(_nodeId);
    if (_nodeId == kInvalidNodeId || !_stream.valid() || _gameId == 0
        || !_limits.valid() || _topology == nullptr || _leases == nullptr
        || !_topology->configured() || _topology->stream() != _stream
        || self == nullptr || self->advertisement.stream != _stream
        || self->advertisement.kind != NodeKind::SpectatorRelay
        || !_cache.bind(_stream, _gameId, _initialDigest, sourceGeneration)) {
        return false;
    }
    try {
        _children.clear();
        _drainReplacementParents.clear();
        _attempts.clear();
        _children.reserve(_limits.maxChildrenPerRelay);
        _drainReplacementParents.reserve(_limits.maxStandbyParents);
        _attempts.reserve(_limits.maxConcurrentRouteRaces);
    } catch (const std::bad_alloc&) {
        _cache.reset();
        return false;
    }
    _state = RelayRecoveryState::Stable;
    _lastFailure = UpstreamFailureReason::TransportClosed;
    _activeParent = kInvalidNodeId;
    _parentGeneration = 0;
    _routeGeneration = 0;
    _recoveryGeneration = 0;
    _recoveryDeadlineMs = 0;
    _drainDeadlineMs = 0;
    _requestedOverlapRecords = 0;
    _localContinuity = {};
    _cacheExhausted = false;
    _recoveryFailureAnnounced = false;
    return true;
}

bool ResilientRelayNode::setInitialParent(NodeId parentId,
                                          uint64_t parentGeneration,
                                          uint64_t routeGeneration) {
    if (_state == RelayRecoveryState::Closed || parentId == kInvalidNodeId
        || parentGeneration == 0 || routeGeneration == 0
        || _topology == nullptr || _topology->find(parentId) == nullptr) {
        return false;
    }
    _activeParent = parentId;
    _parentGeneration = parentGeneration;
    _routeGeneration = routeGeneration;
    return true;
}

bool ResilientRelayNode::configureEventTimeline(
    uint32_t firstTimelineFrame) {
    return (_state == RelayRecoveryState::Stable
            || _state == RelayRecoveryState::CommittedToNewParent)
        && _cache.configureEventTimeline(firstTimelineFrame);
}

bool ResilientRelayNode::attachChild(NodeId childId,
                                     uint32_t acknowledgedCursor) {
    uint32_t acknowledgedEvent = 0;
    uint64_t acknowledgedDigest = 0;
    return _cache.eventCursorAt(acknowledgedCursor, acknowledgedEvent)
        && _cache.digestAt(acknowledgedCursor, acknowledgedEvent,
                           acknowledgedDigest)
        && attachChild(childId, acknowledgedCursor, acknowledgedEvent,
                       acknowledgedDigest);
}

bool ResilientRelayNode::attachChild(NodeId childId,
                                     uint32_t acknowledgedCursor,
                                     uint32_t acknowledgedEvent,
                                     uint64_t acknowledgedDigest) {
    if (_state != RelayRecoveryState::Stable
        && _state != RelayRecoveryState::CommittedToNewParent) {
        return false;
    }
    if (childId == kInvalidNodeId)
        return false;
    if (std::find(_children.begin(), _children.end(), childId)
        != _children.end()) {
        return _cache.attachChild(childId, acknowledgedCursor,
                                  acknowledgedEvent, acknowledgedDigest);
    }
    if (_children.size() >= _limits.maxChildrenPerRelay
        || !_cache.attachChild(childId, acknowledgedCursor,
                               acknowledgedEvent, acknowledgedDigest)) {
        return false;
    }
    try {
        _children.push_back(childId);
    } catch (const std::bad_alloc&) {
        _cache.detachChild(childId);
        return false;
    }
    return true;
}

bool ResilientRelayNode::retireChild(NodeId childId) {
    const auto found = std::find(_children.begin(), _children.end(), childId);
    if (found == _children.end()) return false;
    _cache.detachChild(childId);
    _children.erase(found);
    if (_state == RelayRecoveryState::Draining && _children.empty())
        _state = RelayRecoveryState::Closed;
    return true;
}

bool ResilientRelayNode::noteUpstreamFailure(UpstreamFailureReason reason,
                                             uint64_t nowMs) {
    if (_state != RelayRecoveryState::Stable
        && _state != RelayRecoveryState::CommittedToNewParent) {
        return false;
    }
    _lastFailure = reason;
    _state = RelayRecoveryState::Suspect;
    ++_recoveryGeneration;
    if (_recoveryGeneration == 0) ++_recoveryGeneration;
    _recoveryDeadlineMs = nowMs + _limits.recoveryBudgetMs;
    _attempts.clear();
    _expectedChildBinding = {};
    _localContinuity = {};
    _cacheExhausted = false;
    _recoveryFailureAnnounced = false;
    return true;
}

size_t ResilientRelayNode::beginRecovery(
    const std::vector<ParentLease>& offers, uint32_t overlapRecords,
    uint64_t nowMs) {
    return beginRecovery(offers, overlapRecords, nowMs,
                         ParentLeaseChildBinding{});
}

size_t ResilientRelayNode::beginRecovery(
    const std::vector<ParentLease>& offers, uint32_t overlapRecords,
    uint64_t nowMs, const ParentLeaseChildBinding& expectedChildBinding) {
    if (_state != RelayRecoveryState::Suspect) return 0;
    if (!expectedChildBinding.valid()) {
        _state = RelayRecoveryState::DegradedBufferOnly;
        _recoveryFailureAnnounced = true;
        return 0;
    }
    const uint32_t exactNextCursor = _cache.range().nextCursor;
    if (expectedChildBinding.childRequestedCursor.nextRecord
        != exactNextCursor) {
        _state = RelayRecoveryState::DegradedBufferOnly;
        _recoveryFailureAnnounced = true;
        return 0;
    }
    _expectedChildBinding = expectedChildBinding;
    const uint32_t availableOverlap = (std::min)(
        overlapRecords, exactNextCursor - _cache.range().firstCursor);
    if (!_cache.continuityProof(exactNextCursor, availableOverlap,
                                _localContinuity)) {
        _state = RelayRecoveryState::DegradedBufferOnly;
        _recoveryFailureAnnounced = true;
        return 0;
    }
    _requestedOverlapRecords = availableOverlap;
    _attempts.clear();
    for (const ParentLease& lease : offers) {
        if (_attempts.size() >= _limits.maxConcurrentRouteRaces) break;
        if (lease.stream != _stream || lease.childId != _nodeId
            || lease.parentId == kInvalidNodeId || lease.parentId == _nodeId
            || lease.expiryMs <= nowMs || lease.routeCount == 0) {
            continue;
        }
        if (_topology == nullptr || _leases == nullptr
            || _topology->validateStandbyLease(
                   *_leases, lease, nowMs, exactNextCursor,
                   _expectedChildBinding)
                   != ParentLeaseVerdict::Accepted) {
            continue;
        }
        if (findAttempt(lease.parentId) != nullptr) continue;
        try {
            _attempts.push_back(RecoveryAttempt{lease});
        } catch (const std::bad_alloc&) {
            break;
        }
    }
    if (_attempts.empty()) {
        _state = RelayRecoveryState::DegradedBufferOnly;
        _recoveryFailureAnnounced = true;
        return 0;
    }
    _state = RelayRecoveryState::RacingStandbys;
    return _attempts.size();
}

bool ResilientRelayNode::continuityRequest(NodeId parentId,
                                           ContinuityRequest& out) const {
    out = {};
    ContinuityRequest request{};
    if (!continuityRequest(request)) return false;
    const RecoveryAttempt* attempt = findAttempt(parentId);
    if (attempt == nullptr || attempt->cancelled || attempt->quarantined) {
        return false;
    }
    const ParentLease& lease = attempt->lease;
    if (lease.stream != _stream || lease.childId != _nodeId
        || lease.parentId != parentId || lease.parentSourceGeneration == 0u
        || lease.routeGeneration == 0u || lease.expiryMs == 0u
        || lease.replayNonce == 0u
        || lease.childBinding != _expectedChildBinding) {
        return false;
    }
    request.expectedSourceGeneration = lease.parentSourceGeneration;
    request.lease.childId = lease.childId;
    request.lease.parentId = lease.parentId;
    request.lease.parentSourceGeneration = lease.parentSourceGeneration;
    request.lease.routeGeneration = lease.routeGeneration;
    request.lease.expiryMs = lease.expiryMs;
    request.lease.replayNonce = lease.replayNonce;
    request.lease.macLow = lease.macLow;
    request.lease.macHigh = lease.macHigh;
    request.lease.childBinding = lease.childBinding;
    out = request;
    return true;
}

bool ResilientRelayNode::continuityRequest(ContinuityRequest& out) const {
    out = {};
    if ((_state != RelayRecoveryState::RacingStandbys
         && _state != RelayRecoveryState::VerifyingContinuity)
        || !_localContinuity.available) {
        return false;
    }
    out.stream = _stream;
    out.gameId = _gameId;
    out.expectedSourceGeneration = _cache.sourceGeneration();
    out.parentGeneration = _parentGeneration;
    out.exactNextCursor = _localContinuity.cacheNextCursor;
    out.exactNextEvent = _localContinuity.cacheNextEvent;
    out.overlapCursor = _localContinuity.overlapCursor;
    out.overlapEvent = _localContinuity.overlapEvent;
    out.overlapDigest = _localContinuity.overlapDigest;
    (void)_cache.newestCheckpointIdentity(
        out.overlapCursor, out.overlapEvent, out.checkpointCursor,
        out.checkpointEvent, out.checkpointDigest);
    return true;
}

bool ResilientRelayNode::submitContinuityProof(
    NodeId parentId, const ContinuityProof& proof) {
    if (_state != RelayRecoveryState::RacingStandbys
        && _state != RelayRecoveryState::VerifyingContinuity) {
        return false;
    }
    RecoveryAttempt* attempt = findAttempt(parentId);
    if (attempt == nullptr || attempt->cancelled || attempt->quarantined)
        return false;
    _state = RelayRecoveryState::VerifyingContinuity;
    bool checkpointConsistent = proof.checkpointDigest == 0u;
    if (!checkpointConsistent) {
        uint64_t localDigest = 0u;
        checkpointConsistent =
            !_cache.digestAt(proof.checkpointCursor, proof.checkpointEvent,
                             localDigest)
            || localDigest == proof.checkpointDigest;
    }
    const bool exactProof = proof.available
        && proof.stream == _stream
        && proofBindsExactLease(proof.lease, attempt->lease)
        && proof.gameId == _gameId
        && proof.sourceGeneration == attempt->lease.parentSourceGeneration
        && proof.sourceGeneration >= _cache.sourceGeneration()
        && proof.parentGeneration == _parentGeneration
        && proof.cacheFirstCursor <= _localContinuity.cacheNextCursor
        && proof.cacheNextCursor >= _localContinuity.cacheNextCursor
        && proof.cacheFirstEvent <= _localContinuity.cacheNextEvent
        && proof.cacheNextEvent >= _localContinuity.cacheNextEvent
        && proof.overlapCursor == _localContinuity.overlapCursor
        && proof.overlapEvent == _localContinuity.overlapEvent
        && proof.overlapDigest == _localContinuity.overlapDigest
        && checkpointConsistent;
    if (!exactProof) {
        quarantine(*attempt);
        _state = hasUsableAttempt() ? RelayRecoveryState::RacingStandbys
                                     : RelayRecoveryState::DegradedBufferOnly;
        if (_state == RelayRecoveryState::DegradedBufferOnly)
            _recoveryFailureAnnounced = true;
        return false;
    }
    attempt->proof = proof;
    attempt->continuityVerified = true;
    return true;
}

bool ResilientRelayNode::commitVerifiedParent(NodeId parentId, uint64_t nowMs) {
    if (_state != RelayRecoveryState::VerifyingContinuity) return false;
    RecoveryAttempt* attempt = findAttempt(parentId);
    if (attempt == nullptr || !attempt->continuityVerified
        || attempt->cancelled || attempt->quarantined) {
        return false;
    }
    if (!_cache.canAdoptSourceGeneration(attempt->proof.sourceGeneration,
                                         _localContinuity.cacheNextCursor,
                                         _localContinuity.cacheNextEvent,
                                         _localContinuity.overlapCursor,
                                         _localContinuity.overlapEvent,
                                         _localContinuity.overlapDigest)) {
        quarantine(*attempt);
        _state = hasUsableAttempt() ? RelayRecoveryState::RacingStandbys
                                     : RelayRecoveryState::DegradedBufferOnly;
        return false;
    }
    if (_topology == nullptr || _leases == nullptr
        || _topology->redeemAndReparentAfterContinuity(
                *_leases, attempt->lease, nowMs,
                _localContinuity.cacheNextCursor, _expectedChildBinding)
               != ParentLeaseVerdict::Accepted) {
        quarantine(*attempt);
        _state = hasUsableAttempt() ? RelayRecoveryState::RacingStandbys
                                     : RelayRecoveryState::DegradedBufferOnly;
        if (_state == RelayRecoveryState::DegradedBufferOnly)
            _recoveryFailureAnnounced = true;
        return false;
    }
    if (!_cache.adoptSourceGeneration(attempt->proof.sourceGeneration,
                                      _localContinuity.cacheNextCursor,
                                      _localContinuity.cacheNextEvent,
                                      _localContinuity.overlapCursor,
                                      _localContinuity.overlapEvent,
                                      _localContinuity.overlapDigest)) {
        _state = RelayRecoveryState::DegradedBufferOnly;
        _recoveryFailureAnnounced = true;
        return false;
    }
    _activeParent = parentId;
    ++_parentGeneration;
    if (_parentGeneration == 0) ++_parentGeneration;
    _routeGeneration = attempt->lease.routeGeneration;
    cancelLosingAttempts(parentId);
    _state = RelayRecoveryState::CommittedToNewParent;
    _recoveryFailureAnnounced = false;
    return true;
}

void ResilientRelayNode::noteVerifiedProgress() {
    if (_state == RelayRecoveryState::CommittedToNewParent)
        _state = RelayRecoveryState::Stable;
}

void ResilientRelayNode::tick(uint64_t nowMs) {
    if (_state == RelayRecoveryState::Draining) {
        if (_children.empty() || nowMs >= _drainDeadlineMs)
            _state = RelayRecoveryState::Closed;
        return;
    }
    if ((_state == RelayRecoveryState::Suspect
         || _state == RelayRecoveryState::RacingStandbys
         || _state == RelayRecoveryState::VerifyingContinuity)
        && nowMs >= _recoveryDeadlineMs) {
        _state = RelayRecoveryState::DegradedBufferOnly;
        _recoveryFailureAnnounced = true;
        for (RecoveryAttempt& attempt : _attempts)
            if (!attempt.continuityVerified) attempt.cancelled = true;
    }
    if (_state == RelayRecoveryState::DegradedBufferOnly && _cacheExhausted)
        _state = RelayRecoveryState::RecoveryFailed;
}

void ResilientRelayNode::markCacheExhausted() {
    _cacheExhausted = true;
    if (_state != RelayRecoveryState::Stable
        && _state != RelayRecoveryState::CommittedToNewParent
        && _state != RelayRecoveryState::Draining
        && _state != RelayRecoveryState::Closed) {
        _state = RelayRecoveryState::RecoveryFailed;
        _recoveryFailureAnnounced = true;
    }
}

bool ResilientRelayNode::childFallbackPermitted() const {
    return _state == RelayRecoveryState::RecoveryFailed;
}

ChildRecoveryNotice ResilientRelayNode::childRecoveryNotice() const {
    ChildRecoveryNotice out{};
    out.state = _state;
    out.latestVerifiedCursor = _cache.range().nextCursor;
    out.cacheRunwayRecords = _cache.range().nextCursor
        - _cache.range().firstCursor;
    out.recoveryGeneration = _recoveryGeneration;
    out.childFallbackPermitted = childFallbackPermitted();
    return out;
}

bool ResilientRelayNode::beginDrain(
    uint64_t shutdownDeadlineMs, const std::vector<NodeId>& replacementParents,
    uint64_t routeGeneration) {
    if (_state == RelayRecoveryState::Closed || shutdownDeadlineMs == 0
        || routeGeneration == 0) {
        return false;
    }
    _drainReplacementParents.clear();
    for (NodeId replacement : replacementParents) {
        if (_drainReplacementParents.size() >= _limits.maxStandbyParents) break;
        if (replacement == kInvalidNodeId || replacement == _nodeId) continue;
        if (std::find(_drainReplacementParents.begin(),
                      _drainReplacementParents.end(), replacement)
            != _drainReplacementParents.end()) {
            continue;
        }
        _drainReplacementParents.push_back(replacement);
    }
    _drainDeadlineMs = shutdownDeadlineMs;
    _routeGeneration = routeGeneration;
    if (_topology != nullptr && !_topology->setDraining(_nodeId, true))
        return false;
    _state = RelayRecoveryState::Draining;
    return true;
}

bool ResilientRelayNode::drainNotice(DrainNotice& out) const {
    out = {};
    if (_state != RelayRecoveryState::Draining) return false;
    out.shutdownDeadlineMs = _drainDeadlineMs;
    out.lastAvailableCursor = _cache.range().nextCursor;
    out.cacheEndCursor = _cache.range().nextCursor;
    out.sourceGeneration = _cache.sourceGeneration();
    out.routeGeneration = _routeGeneration;
    out.recommendedReplacementParents = _drainReplacementParents;
    out.activeChildIds = _children;
    return true;
}

void ResilientRelayNode::close() {
    _cache.reset();
    _children.clear();
    _drainReplacementParents.clear();
    _attempts.clear();
    _expectedChildBinding = {};
    _activeParent = kInvalidNodeId;
    _state = RelayRecoveryState::Closed;
}

size_t ResilientRelayNode::cancelledAttemptCount() const {
    size_t count = 0;
    for (const RecoveryAttempt& attempt : _attempts)
        if (attempt.cancelled) ++count;
    return count;
}

size_t ResilientRelayNode::quarantinedAttemptCount() const {
    size_t count = 0;
    for (const RecoveryAttempt& attempt : _attempts)
        if (attempt.quarantined) ++count;
    return count;
}

ResilientRelayNode::RecoveryAttempt* ResilientRelayNode::findAttempt(
    NodeId parentId) {
    for (RecoveryAttempt& attempt : _attempts)
        if (attempt.lease.parentId == parentId) return &attempt;
    return nullptr;
}

const ResilientRelayNode::RecoveryAttempt* ResilientRelayNode::findAttempt(
    NodeId parentId) const {
    for (const RecoveryAttempt& attempt : _attempts)
        if (attempt.lease.parentId == parentId) return &attempt;
    return nullptr;
}

void ResilientRelayNode::quarantine(RecoveryAttempt& attempt) {
    attempt.continuityVerified = false;
    attempt.quarantined = true;
}

bool ResilientRelayNode::hasUsableAttempt() const {
    for (const RecoveryAttempt& attempt : _attempts) {
        if (!attempt.cancelled && !attempt.quarantined) return true;
    }
    return false;
}

void ResilientRelayNode::cancelLosingAttempts(NodeId winnerId) {
    for (RecoveryAttempt& attempt : _attempts) {
        if (attempt.lease.parentId != winnerId && !attempt.quarantined)
            attempt.cancelled = true;
    }
}

TopologyNodeView* ResilientTopologyCoordinator::findMutable(NodeId nodeId) {
    for (TopologyNodeView& node : _nodes)
        if (node.advertisement.nodeId == nodeId) return &node;
    return nullptr;
}

}
