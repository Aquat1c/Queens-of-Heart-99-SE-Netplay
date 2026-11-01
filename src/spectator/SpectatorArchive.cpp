#include "SpectatorArchive.h"
#include "SpectatorCheckpoint.h"

#include "input/InputActions.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace qnetplay::spectator {
namespace {

bool validTerminalReasonValue(TerminalReason reason) {
    switch (reason) {
        case TerminalReason::MatchEnded:
        case TerminalReason::SourceStopped:
        case TerminalReason::Superseded:
        case TerminalReason::ProtocolFailure:
            return true;
    }
    return false;
}

bool validDescriptorScalars(const GameDescriptor& descriptor) {
    return descriptor.gameId != kLatestGameId
        && descriptor.simulationContract != 0
        && compat::isValidCompatibilityIdentity(
            descriptor.compatibilityIdentity)
        && descriptor.metadata.size() <= kMaxGameMetadataSize
        && descriptor.playerOneDelay <= 120
        && descriptor.playerTwoDelay <= 120
        && descriptor.rollbackWindow <= 120;
}

bool sameRecordWithoutOptionalDigest(const ConfirmedRecord& requested,
                                     const ConfirmedRecord& stored) {
    return requested.record == stored.record
        && requested.timelineFrame == stored.timelineFrame
        && requested.playerOneInput == stored.playerOneInput
        && requested.playerTwoInput == stored.playerTwoInput
        && requested.stateHash == stored.stateHash
        && (requested.rollingDigest == 0
            || requested.rollingDigest == stored.rollingDigest);
}

template <typename Container, typename Value>
bool nothrowPushBack(Container& container, Value&& value) {
    try {
        container.push_back(std::forward<Value>(value));
        return true;
    } catch (...) {
        return false;
    }
}

}

GameStartPayload canonicalGameStart(const GameDescriptor& descriptor) {
    GameStartPayload start{};
    start.gameId = descriptor.gameId;
    start.simulationContract = descriptor.simulationContract;
    start.compatibilityIdentity = descriptor.compatibilityIdentity;
    start.initialStateHash = descriptor.initialStateHash;
    start.initialDigest = descriptor.initialDigest;
    start.viewerNonce = 1;
    start.viewerId = 1;
    start.firstTimelineFrame = descriptor.firstTimelineFrame;
    start.playerOneDelay = descriptor.playerOneDelay;
    start.playerTwoDelay = descriptor.playerTwoDelay;
    start.rollbackWindow = descriptor.rollbackWindow;
    start.metadata = descriptor.metadata;
    return start;
}

uint64_t expectedInitialDigest(const GameDescriptor& descriptor) {
    GameStartPayload start = canonicalGameStart(descriptor);
    start.initialDigest = 0;
    return computeInitialDigest(start);
}

SpectatorArchive::SpectatorArchive(const ArchiveConfig& config)
    : _config(config),
      _configured(config.maxGames > 0
                  && config.maxRecordsPerGame > 0
                  && config.segmentRecords > 0
                  && config.maxRetainedBytes >= sizeof(ConfirmedRecord)
                  && static_cast<size_t>(config.segmentRecords)
                      <= config.maxRetainedBytes / sizeof(ConfirmedRecord)
                  && (config.retainAllRecords
                      || config.maxRetainedRecords > 0)) {
}

bool SpectatorArchive::serviceDisabled() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _serviceDisabled;
}

bool SpectatorArchive::retentionBlocked() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _retentionBlocked;
}

void SpectatorArchive::setSegmentAllocationFailuresForTest(size_t count) {
    std::lock_guard<std::mutex> lock(_mutex);
    _forcedSegmentFailures = count;
}

SpectatorArchive::Game* SpectatorArchive::findGame(uint64_t gameId) {
    for (const std::unique_ptr<Game>& game : _games) {
        if (game && game->descriptor.gameId == gameId) return game.get();
    }
    return nullptr;
}

const SpectatorArchive::Game*
SpectatorArchive::findGame(uint64_t gameId) const {
    for (const std::unique_ptr<Game>& game : _games) {
        if (game && game->descriptor.gameId == gameId) return game.get();
    }
    return nullptr;
}

const ConfirmedRecord* SpectatorArchive::recordPointer(
    const Game& game, uint32_t index) const {
    if (index < game.firstRecord || index >= game.nextRecord)
        return nullptr;
    for (const std::unique_ptr<Segment>& segment : game.segments) {
        if (!segment) continue;
        if (index < segment->baseRecord) continue;
        const uint32_t offset = index - segment->baseRecord;
        if (offset < segment->count)
            return &segment->records[offset];
    }
    return nullptr;
}

bool SpectatorArchive::digestAtLocked(const Game& game,
                                      uint32_t nextRecord,
                                      uint64_t& digest) const {
    if (nextRecord > game.nextRecord || nextRecord < game.firstRecord)
        return false;
    // When a set event sits exactly on the boundary, the base for record `nextRecord` is the event's digest, not the previous record's.
    uint64_t boundary = 0;
    if (game.timeline.boundaryDigest(nextRecord, boundary)) {
        digest = boundary;
        return true;
    }
    if (nextRecord == game.firstRecord) {
        digest = game.firstBaseDigest;
        return true;
    }
    if (nextRecord == game.nextRecord) {
        digest = game.tailDigest;
        return true;
    }
    const ConfirmedRecord* previous = recordPointer(game, nextRecord - 1);
    if (!previous) return false;
    digest = previous->rollingDigest;
    return true;
}

bool SpectatorArchive::chainDigestAtLocked(const Game& game,
                                           uint32_t nextRecord,
                                           uint32_t nextEvent,
                                           uint64_t& digest) const {
    const uint32_t totalEvents = game.timeline.nextEventIndex();
    if (nextEvent > totalEvents) return false;
    if (nextRecord > game.nextRecord || nextRecord < game.firstRecord)
        return false;

    if (nextEvent > 0) {
        SetEvent previous{};
        if (!game.timeline.event(nextEvent - 1, previous)) return false;
        if (previous.recordCursor > nextRecord) return false;
        if (previous.recordCursor == nextRecord) {
            digest = previous.rollingDigest;
            return true;
        }
    }
    if (nextEvent < totalEvents) {
        SetEvent following{};
        if (!game.timeline.event(nextEvent, following)) return false;
        if (following.recordCursor < nextRecord) return false;
    }
    if (nextRecord == 0) {
        digest = game.descriptor.initialDigest;
        return true;
    }
    const ConfirmedRecord* previousRecord =
        recordPointer(game, nextRecord - 1);
    if (!previousRecord) return false;
    digest = previousRecord->rollingDigest;
    return true;
}

uint32_t SpectatorArchive::retentionFloor(const Game& game) const {
    uint32_t floor = (std::numeric_limits<uint32_t>::max)();
    if (game.hasResumeFloor) floor = game.resumeFloor;
    for (const ArchiveCheckpointRef& reference : game.checkpointRefs) {
        if (reference.recordCursor < floor) floor = reference.recordCursor;
    }
    return floor;
}

size_t SpectatorArchive::gameRecordBytesLocked(const Game& game) const {
    size_t bytes = 0;
    for (const std::unique_ptr<Segment>& segment : game.segments) {
        if (!segment) continue;
        const size_t segmentBytes =
            static_cast<size_t>(segment->capacity)
            * sizeof(ConfirmedRecord);
        if (bytes > (std::numeric_limits<size_t>::max)() - segmentBytes)
            return (std::numeric_limits<size_t>::max)();
        bytes += segmentBytes;
    }
    return bytes;
}

void SpectatorArchive::eraseGameLocked(
    std::vector<std::unique_ptr<Game>>::iterator game) {
    if (game == _games.end()) return;
    if (*game) {
        const size_t bytes = gameRecordBytesLocked(**game);
        _allocatedRecordBytes =
            bytes > _allocatedRecordBytes ? 0 : _allocatedRecordBytes - bytes;
    }
    _games.erase(game);
}

SpectatorArchive::SegmentAppendResult
SpectatorArchive::appendSegmentLocked(Game& game, uint32_t baseRecord) {
    if (_forcedSegmentFailures > 0) {
        --_forcedSegmentFailures;
        return SegmentAppendResult::AllocationFailed;
    }

    const size_t segmentBytes =
        static_cast<size_t>(_config.segmentRecords)
        * sizeof(ConfirmedRecord);

    while (segmentBytes > _config.maxRetainedBytes
               - (std::min)(_allocatedRecordBytes,
                            _config.maxRetainedBytes)) {
        const auto oldestSealed = std::find_if(
            _games.begin(), _games.end(),
            [&game](const std::unique_ptr<Game>& candidate) {
                return candidate && candidate.get() != &game
                    && candidate->sealed;
            });
        if (oldestSealed == _games.end())
            return SegmentAppendResult::ByteLimit;
        eraseGameLocked(oldestSealed);
    }

    std::unique_ptr<ConfirmedRecord[]> storage(
        new (std::nothrow) ConfirmedRecord[_config.segmentRecords]);
    if (!storage) return SegmentAppendResult::AllocationFailed;
    std::unique_ptr<Segment> segment(new (std::nothrow) Segment);
    if (!segment) return SegmentAppendResult::AllocationFailed;
    segment->baseRecord = baseRecord;
    segment->count = 0;
    segment->capacity = _config.segmentRecords;
    segment->records = std::move(storage);
    if (!nothrowPushBack(game.segments, std::move(segment)))
        return SegmentAppendResult::AllocationFailed;
    _allocatedRecordBytes += segmentBytes;
    return SegmentAppendResult::Ok;
}

void SpectatorArchive::rotateLocked(Game& game,
                                    uint32_t projectedNextRecord) {
    if (_config.retainAllRecords) return;
    if (projectedNextRecord < game.firstRecord
        || projectedNextRecord - game.firstRecord
            <= _config.maxRetainedRecords) {
        return;
    }

    const uint32_t floor = retentionFloor(game);
    const uint32_t desiredFirst =
        projectedNextRecord - _config.maxRetainedRecords;

    if (desiredFirst > floor) {
        _retentionBlocked = true;
        if (game.hasResumeFloor && game.resumeFloor < desiredFirst)
            game.hasResumeFloor = false;
        game.checkpointRefs.erase(
            std::remove_if(
                game.checkpointRefs.begin(), game.checkpointRefs.end(),
                [desiredFirst](const ArchiveCheckpointRef& checkpoint) {
                    return checkpoint.recordCursor < desiredFirst;
                }),
            game.checkpointRefs.end());
    }

    uint64_t base = 0;
    if (!digestAtLocked(game, desiredFirst, base)) {
        _retentionBlocked = true;
        return;
    }
    game.firstRecord = desiredFirst;
    game.firstBaseDigest = base;

    while (!game.segments.empty()) {
        const std::unique_ptr<Segment>& oldest = game.segments.front();
        if (!oldest) {
            game.segments.pop_front();
            continue;
        }
        const uint32_t end = oldest->baseRecord + oldest->count;
        if (end > desiredFirst) break;
        const size_t bytes =
            static_cast<size_t>(oldest->capacity)
            * sizeof(ConfirmedRecord);
        game.segments.pop_front();
        _allocatedRecordBytes =
            bytes > _allocatedRecordBytes ? 0 : _allocatedRecordBytes - bytes;
    }
}

ArchiveResult SpectatorArchive::beginGame(
    const GameDescriptor& requestedDescriptor) {
    if (!_configured || !validDescriptorScalars(requestedDescriptor))
        return ArchiveResult::InvalidArgument;

    GameDescriptor descriptor = requestedDescriptor;
    const uint64_t expected = expectedInitialDigest(descriptor);
    if (descriptor.initialDigest != 0
        && descriptor.initialDigest != expected) {
        return ArchiveResult::DigestMismatch;
    }
    descriptor.initialDigest = expected;

    std::lock_guard<std::mutex> lock(_mutex);
    if (_serviceDisabled) return ArchiveResult::AllocationFailed;
    if (Game* existing = findGame(descriptor.gameId)) {
        return existing->descriptor == descriptor
            ? ArchiveResult::ExactDuplicate
            : ArchiveResult::GameAlreadyExists;
    }
    for (const std::unique_ptr<Game>& game : _games) {
        if (game && !game->sealed) return ArchiveResult::ActiveGameExists;
    }

    if (_games.size() >= _config.maxGames) {
        const auto oldestSealed = std::find_if(
            _games.begin(), _games.end(),
            [](const std::unique_ptr<Game>& game) {
                return game && game->sealed;
            });
        if (oldestSealed == _games.end()) return ArchiveResult::ArchiveFull;
        eraseGameLocked(oldestSealed);
    }

    std::unique_ptr<Game> game(new (std::nothrow) Game);
    if (!game) {
        _serviceDisabled = true;
        return ArchiveResult::AllocationFailed;
    }
    game->descriptor = std::move(descriptor);
    game->firstRecord = 0;
    game->nextRecord = 0;
    game->firstBaseDigest = game->descriptor.initialDigest;
    game->tailDigest = game->descriptor.initialDigest;
    game->tailStateHash = game->descriptor.initialStateHash;
    game->timeline.reset(game->descriptor.gameId,
                         game->descriptor.firstTimelineFrame);
    const SegmentAppendResult segment = appendSegmentLocked(*game, 0);
    if (segment != SegmentAppendResult::Ok) {
        if (segment == SegmentAppendResult::ByteLimit)
            return ArchiveResult::ArchiveFull;
        _serviceDisabled = true;
        return ArchiveResult::AllocationFailed;
    }
    if (!nothrowPushBack(_games, std::move(game))) {
        _allocatedRecordBytes -=
            static_cast<size_t>(_config.segmentRecords)
            * sizeof(ConfirmedRecord);
        _serviceDisabled = true;
        return ArchiveResult::AllocationFailed;
    }
    return ArchiveResult::Ok;
}

ArchiveResult SpectatorArchive::appendConfirmed(
    uint64_t gameId,
    const ConfirmedRecord& requested,
    ConfirmedRecord* stored) {
    if (!qinput::isCanonicalFightInput(requested.playerOneInput)
        || !qinput::isCanonicalFightInput(requested.playerTwoInput)) {
        return ArchiveResult::InvalidArgument;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (_serviceDisabled) return ArchiveResult::AllocationFailed;
    Game* game = findGame(gameId);
    if (!game) return ArchiveResult::GameUnavailable;

    if (requested.record < game->nextRecord) {
        if (requested.record < game->firstRecord) {
            return ArchiveResult::Gap;
        }
        const ConfirmedRecord* existing =
            recordPointer(*game, requested.record);
        if (!existing) return ArchiveResult::Gap;
        if (!sameRecordWithoutOptionalDigest(requested, *existing))
            return ArchiveResult::ConflictingDuplicate;
        if (stored) *stored = *existing;
        return ArchiveResult::ExactDuplicate;
    }
    if (requested.record > game->nextRecord) return ArchiveResult::Gap;
    if (game->sealed) return ArchiveResult::Sealed;
    if (game->nextRecord >= _config.maxRecordsPerGame
        || game->nextRecord == (std::numeric_limits<uint32_t>::max)()) {
        return ArchiveResult::RecordLimit;
    }
    if (game->descriptor.firstTimelineFrame
        > (std::numeric_limits<uint32_t>::max)() - requested.record) {
        return ArchiveResult::TimelineMismatch;
    }
    const uint32_t expectedTimeline =
        game->descriptor.firstTimelineFrame + requested.record;
    if (requested.timelineFrame != expectedTimeline)
        return ArchiveResult::TimelineMismatch;

    ConfirmedRecord canonical = requested;
    canonical.rollingDigest =
        advanceRollingDigest(game->tailDigest, gameId, canonical);
    if (requested.rollingDigest != 0
        && requested.rollingDigest != canonical.rollingDigest) {
        return ArchiveResult::DigestMismatch;
    }

    rotateLocked(*game, game->nextRecord + 1u);

    if (game->segments.empty()
        || !game->segments.back()
        || game->segments.back()->count
            >= game->segments.back()->capacity) {
        const SegmentAppendResult segment =
            appendSegmentLocked(*game, game->nextRecord);
        if (segment != SegmentAppendResult::Ok) {
            if (segment == SegmentAppendResult::ByteLimit)
                return ArchiveResult::ArchiveFull;
            _serviceDisabled = true;
            return ArchiveResult::AllocationFailed;
        }
    }
    Segment& segment = *game->segments.back();
    segment.records[segment.count] = canonical;
    ++segment.count;
    ++game->nextRecord;
    game->tailDigest = canonical.rollingDigest;
    game->tailStateHash = canonical.stateHash;
    if (stored) *stored = canonical;
    return ArchiveResult::Ok;
}

ArchiveResult SpectatorArchive::sealGame(uint64_t gameId,
                                         TerminalReason reason,
                                         uint64_t postTailStateHash) {
    if (!validTerminalReasonValue(reason))
        return ArchiveResult::InvalidArgument;
    std::lock_guard<std::mutex> lock(_mutex);
    Game* game = findGame(gameId);
    if (!game) return ArchiveResult::GameUnavailable;

    if (game->nextRecord == 0
        && postTailStateHash != game->descriptor.initialStateHash) {
        return ArchiveResult::TerminalMismatch;
    }

    ArchiveTerminal terminal{};
    terminal.reason = reason;
    terminal.nextRecord = game->nextRecord;
    terminal.nextEvent = game->timeline.nextEventIndex();
    terminal.finalDigest = game->tailDigest;
    terminal.postTailStateHash = postTailStateHash;
    if (game->sealed) {
        return game->terminal == terminal
            ? ArchiveResult::ExactDuplicate
            : ArchiveResult::TerminalMismatch;
    }
    game->terminal = terminal;
    game->sealed = true;
    return ArchiveResult::Ok;
}

bool SpectatorArchive::latestGameId(uint64_t& gameId) const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_games.empty() || !_games.back()) return false;
    gameId = _games.back()->descriptor.gameId;
    return true;
}

bool SpectatorArchive::descriptor(uint64_t gameId,
                                  GameDescriptor& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    out = game->descriptor;
    return true;
}

bool SpectatorArchive::extent(uint64_t gameId, ArchiveExtent& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    ArchiveExtent extent{};
    extent.nextRecord = game->nextRecord;
    extent.firstRecord = game->firstRecord;
    extent.nextEvent = game->timeline.nextEventIndex();
    extent.digest = game->tailDigest;
    extent.stateHash = game->tailStateHash;
    extent.sealed = game->sealed;
    extent.terminal = game->terminal;
    out = extent;
    return true;
}

bool SpectatorArchive::digestAt(uint64_t gameId,
                                uint32_t nextRecord,
                                uint64_t& digest) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    return digestAtLocked(*game, nextRecord, digest);
}

bool SpectatorArchive::chainDigestAt(uint64_t gameId,
                                     uint32_t nextRecord,
                                     uint32_t nextEvent,
                                     uint64_t& digest) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    return chainDigestAtLocked(*game, nextRecord, nextEvent, digest);
}

ArchiveResult SpectatorArchive::appendEvent(uint64_t gameId,
                                            const SetEvent& event,
                                            SetEvent* stored) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_serviceDisabled) return ArchiveResult::AllocationFailed;
    Game* game = findGame(gameId);
    if (!game) return ArchiveResult::GameUnavailable;

    SetEvent canonical{};
    const SetEventResult result = game->timeline.append(
        event, game->nextRecord, game->tailDigest, &canonical);
    switch (result) {
        case SetEventResult::Ok:
            game->tailDigest = canonical.rollingDigest;
            if (stored) *stored = canonical;
            return ArchiveResult::Ok;
        case SetEventResult::ExactDuplicate:
            if (stored) *stored = canonical;
            return ArchiveResult::ExactDuplicate;
        case SetEventResult::InvalidArgument:
            return ArchiveResult::InvalidArgument;
        case SetEventResult::Gap:
            return ArchiveResult::Gap;
        case SetEventResult::ConflictingDuplicate:
            return ArchiveResult::ConflictingDuplicate;
        case SetEventResult::BoundaryMismatch:
            return ArchiveResult::TimelineMismatch;
        case SetEventResult::DigestMismatch:
            return ArchiveResult::DigestMismatch;
        case SetEventResult::OrderViolation:
            return ArchiveResult::EventOrderViolation;
        case SetEventResult::LimitReached:
            return ArchiveResult::RecordLimit;
    }
    return ArchiveResult::InvalidArgument;
}

bool SpectatorArchive::setEventCount(uint64_t gameId, uint32_t& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    out = game->timeline.nextEventIndex();
    return true;
}

bool SpectatorArchive::setEventAt(uint64_t gameId,
                                  uint32_t index,
                                  SetEvent& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    return game->timeline.event(index, out);
}

ArchiveResult SpectatorArchive::readEventBatch(
    uint64_t gameId,
    uint32_t startEvent,
    size_t maxEvents,
    SetEventBatchPayload& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return ArchiveResult::GameUnavailable;
    if (maxEvents == 0) return ArchiveResult::InvalidArgument;
    const uint32_t total = game->timeline.nextEventIndex();
    if (startEvent >= total) return ArchiveResult::Gap;

    const size_t count = std::min({
        maxEvents,
        kMaxSetEventsPerBatch,
        static_cast<size_t>(total - startEvent),
    });
    SetEvent first{};
    if (!game->timeline.event(startEvent, first))
        return ArchiveResult::Gap;

    SetEventBatchPayload batch{};
    batch.gameId = gameId;
    batch.startEvent = startEvent;
    batch.baseDigest = first.recordDigest;
    batch.events.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        SetEvent event{};
        if (!game->timeline.event(
                startEvent + static_cast<uint32_t>(i), event)) {
            return ArchiveResult::Gap;
        }
        batch.events.push_back(event);
    }
    out = std::move(batch);
    return ArchiveResult::Ok;
}

bool SpectatorArchive::matchIndex(
    uint64_t gameId, std::vector<SetMatchIndexEntry>& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    out = game->timeline.matches();
    return true;
}

bool SpectatorArchive::currentMatch(uint64_t gameId,
                                    uint32_t receivedNext,
                                    SetMatchIndexEntry& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    return game->timeline.currentMatch(receivedNext, out);
}

bool SpectatorArchive::seekRecordForMatch(uint64_t gameId,
                                          uint32_t matchId,
                                          uint32_t& outRecord) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    return game->timeline.seekRecordForMatch(matchId, outRecord);
}

bool SpectatorArchive::setPaused(uint64_t gameId, bool& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    out = game->timeline.paused();
    return true;
}

bool SpectatorArchive::record(uint64_t gameId,
                              uint32_t recordIndex,
                              ConfirmedRecord& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    const ConfirmedRecord* found = recordPointer(*game, recordIndex);
    if (!found) return false;
    out = *found;
    return true;
}

ArchiveResult SpectatorArchive::readBatch(
    uint64_t gameId,
    uint32_t startRecord,
    size_t maxRecords,
    ConfirmedInputBatchPayload& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return ArchiveResult::GameUnavailable;
    if (startRecord < game->firstRecord
        || startRecord >= game->nextRecord) {
        return ArchiveResult::Gap;
    }
    if (maxRecords == 0) return ArchiveResult::InvalidArgument;

    uint32_t clip = game->nextRecord;
    uint32_t eventCursor = 0;
    if (game->timeline.nextEventCursorAfter(startRecord, eventCursor)
        && eventCursor < clip) {
        clip = eventCursor;
    }
    if (clip <= startRecord) return ArchiveResult::Gap;

    const size_t count = std::min({
        maxRecords,
        kMaxRecordsPerBatch,
        static_cast<size_t>(clip - startRecord),
    });
    uint64_t baseDigest = 0;
    if (!digestAtLocked(*game, startRecord, baseDigest))
        return ArchiveResult::Gap;

    ConfirmedInputBatchPayload batch{};
    batch.gameId = gameId;
    batch.startRecord = startRecord;
    batch.baseDigest = baseDigest;
    batch.baseEventIndex = game->timeline.eventsBeforeRecord(startRecord);
    batch.records.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const ConfirmedRecord* found = recordPointer(
            *game, startRecord + static_cast<uint32_t>(i));
        if (!found) return ArchiveResult::Gap;
        batch.records.push_back(*found);
    }
    batch.endDigest = batch.records.back().rollingDigest;
    out = std::move(batch);
    return ArchiveResult::Ok;
}

size_t SpectatorArchive::retainedGameCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _games.size();
}

bool SpectatorArchive::firstRetainedRecord(uint64_t gameId,
                                           uint32_t& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    out = game->firstRecord;
    return true;
}

bool SpectatorArchive::setResumeFloor(uint64_t gameId,
                                      uint32_t lowestRequiredRecord) {
    std::lock_guard<std::mutex> lock(_mutex);
    Game* game = findGame(gameId);
    if (!game
        || lowestRequiredRecord < game->firstRecord
        || lowestRequiredRecord > game->nextRecord) {
        return false;
    }
    game->hasResumeFloor = true;
    game->resumeFloor = lowestRequiredRecord;
    return true;
}

bool SpectatorArchive::clearResumeFloor(uint64_t gameId) {
    std::lock_guard<std::mutex> lock(_mutex);
    Game* game = findGame(gameId);
    if (!game) return false;
    game->hasResumeFloor = false;
    game->resumeFloor = 0;
    return true;
}

ArchiveResult SpectatorArchive::setNewestCheckpointBlob(
    uint64_t gameId, uint64_t checkpointId,
    const uint8_t* bytes, size_t length) {
    if (checkpointId == 0 || bytes == nullptr || length == 0
        || length > kMaxCheckpointTotalBytes) {
        return ArchiveResult::InvalidArgument;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (_serviceDisabled) return ArchiveResult::AllocationFailed;
    Game* game = findGame(gameId);
    if (!game) return ArchiveResult::GameUnavailable;
    game->newestBlobCheckpointId = checkpointId;
    game->newestBlob.assign(bytes, bytes + length);
    return ArchiveResult::Ok;
}

bool SpectatorArchive::newestCheckpointBlobInfo(
    uint64_t gameId, uint64_t& checkpointId, uint32_t& totalBytes) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game || game->newestBlobCheckpointId == 0
        || game->newestBlob.empty()) {
        return false;
    }
    checkpointId = game->newestBlobCheckpointId;
    totalBytes = static_cast<uint32_t>(game->newestBlob.size());
    return true;
}

bool SpectatorArchive::checkpointChunk(
    uint64_t gameId, uint64_t checkpointId,
    uint32_t byteOffset, uint32_t length,
    std::vector<uint8_t>& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game || game->newestBlobCheckpointId != checkpointId)
        return false;
    const size_t total = game->newestBlob.size();
    if (byteOffset >= total || length == 0
        || static_cast<size_t>(byteOffset) + length > total) {
        return false;
    }
    out.assign(game->newestBlob.begin() + byteOffset,
               game->newestBlob.begin() + byteOffset + length);
    return true;
}

ArchiveResult SpectatorArchive::addCheckpoint(
    uint64_t gameId, const ArchiveCheckpointRef& checkpoint) {
    if (checkpoint.checkpointId == 0)
        return ArchiveResult::InvalidArgument;
    std::lock_guard<std::mutex> lock(_mutex);
    if (_serviceDisabled) return ArchiveResult::AllocationFailed;
    Game* game = findGame(gameId);
    if (!game) return ArchiveResult::GameUnavailable;
    if (checkpoint.recordCursor > game->nextRecord
        || checkpoint.recordCursor < game->firstRecord) {
        return ArchiveResult::Gap;
    }
    for (const ArchiveCheckpointRef& existing : game->checkpointRefs) {
        if (existing.checkpointId == checkpoint.checkpointId) {
            return existing == checkpoint
                ? ArchiveResult::ExactDuplicate
                : ArchiveResult::ConflictingDuplicate;
        }
    }
    if (!nothrowPushBack(game->checkpointRefs, checkpoint)) {
        _serviceDisabled = true;
        return ArchiveResult::AllocationFailed;
    }
    game->timeline.noteCheckpoint(
        checkpoint.checkpointId, checkpoint.recordCursor);
    while (game->checkpointRefs.size() > _config.maxCheckpoints) {
        auto oldest = std::min_element(
            game->checkpointRefs.begin(), game->checkpointRefs.end(),
            [](const ArchiveCheckpointRef& left,
               const ArchiveCheckpointRef& right) {
                return left.recordCursor < right.recordCursor;
            });
        game->checkpointRefs.erase(oldest);
    }
    return ArchiveResult::Ok;
}

bool SpectatorArchive::checkpoints(
    uint64_t gameId, std::vector<ArchiveCheckpointRef>& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return false;
    out = game->checkpointRefs;
    return true;
}

bool SpectatorArchive::newestCheckpoint(uint64_t gameId,
                                        ArchiveCheckpointRef& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game || game->checkpointRefs.empty()) return false;
    const ArchiveCheckpointRef* newest = &game->checkpointRefs.front();
    for (const ArchiveCheckpointRef& reference : game->checkpointRefs) {
        if (reference.recordCursor > newest->recordCursor)
            newest = &reference;
    }
    out = *newest;
    return true;
}

size_t SpectatorArchive::segmentCount(uint64_t gameId) const {
    std::lock_guard<std::mutex> lock(_mutex);
    const Game* game = findGame(gameId);
    if (!game) return 0;
    return game->segments.size();
}

size_t SpectatorArchive::retainedRecordCount() const {
    std::lock_guard<std::mutex> lock(_mutex);
    size_t records = 0;
    for (const std::unique_ptr<Game>& game : _games) {
        if (!game) continue;
        const size_t retained =
            static_cast<size_t>(game->nextRecord - game->firstRecord);
        if (records > (std::numeric_limits<size_t>::max)() - retained)
            return (std::numeric_limits<size_t>::max)();
        records += retained;
    }
    return records;
}

size_t SpectatorArchive::allocatedRecordBytes() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _allocatedRecordBytes;
}

}
