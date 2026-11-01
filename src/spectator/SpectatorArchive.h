/* Every publishing-path allocation is no-throw: exhaustion disables spectator service and must never terminate the game process. */
#pragma once

#include "SpectatorProtocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace qnetplay::spectator {

struct GameDescriptor {
    uint64_t gameId = 0;
    uint64_t simulationContract = 0;
    compat::CompatibilityIdentityWire compatibilityIdentity{};
    uint64_t initialStateHash = 0;
    uint64_t initialDigest = 0;
    uint32_t firstTimelineFrame = 0;
    uint16_t playerOneDelay = 0;
    uint16_t playerTwoDelay = 0;
    uint16_t rollbackWindow = 0;
    std::vector<uint8_t> metadata;

    bool operator==(const GameDescriptor& other) const {
        return gameId == other.gameId
            && simulationContract == other.simulationContract
            && compat::semanticallyEqual(
                compatibilityIdentity, other.compatibilityIdentity)
            && initialStateHash == other.initialStateHash
            && initialDigest == other.initialDigest
            && firstTimelineFrame == other.firstTimelineFrame
            && playerOneDelay == other.playerOneDelay
            && playerTwoDelay == other.playerTwoDelay
            && rollbackWindow == other.rollbackWindow
            && metadata == other.metadata;
    }
    bool operator!=(const GameDescriptor& other) const {
        return !(*this == other);
    }
};

struct ArchiveTerminal {
    TerminalReason reason = TerminalReason::MatchEnded;
    uint32_t nextRecord = 0;
    uint32_t nextEvent = 0;
    uint64_t finalDigest = 0;
    uint64_t postTailStateHash = 0;

    bool operator==(const ArchiveTerminal& other) const {
        return reason == other.reason
            && nextRecord == other.nextRecord
            && nextEvent == other.nextEvent
            && finalDigest == other.finalDigest
            && postTailStateHash == other.postTailStateHash;
    }
};

struct ArchiveExtent {
    uint32_t nextRecord = 0;
    uint32_t firstRecord = 0;
    uint32_t nextEvent = 0;
    uint64_t digest = 0;
    uint64_t stateHash = 0;
    bool sealed = false;
    ArchiveTerminal terminal{};
};

struct ArchiveCheckpointRef {
    uint64_t checkpointId = 0;
    uint32_t recordCursor = 0;
    uint64_t digest = 0;

    bool operator==(const ArchiveCheckpointRef& other) const {
        return checkpointId == other.checkpointId
            && recordCursor == other.recordCursor
            && digest == other.digest;
    }
};

struct ArchiveConfig {
    // The first two members are positional in aggregate initializers; append new members only.
    size_t maxGames = (std::numeric_limits<size_t>::max)();
    uint32_t maxRecordsPerGame =
        (std::numeric_limits<uint32_t>::max)();
    uint32_t maxRetainedRecords = 60u * 60u * 61u;
    size_t maxCheckpoints = 3;
    bool retainAllRecords = false;
    uint32_t segmentRecords = 4096;
    size_t maxRetainedBytes = 64u * 1024u * 1024u;
};

enum class ArchiveResult {
    Ok,
    ExactDuplicate,
    InvalidArgument,
    GameAlreadyExists,
    ActiveGameExists,
    GameUnavailable,
    ArchiveFull,
    RecordLimit,
    Gap,
    ConflictingDuplicate,
    TimelineMismatch,
    DigestMismatch,
    Sealed,
    TerminalMismatch,
    AllocationFailed,
    EventOrderViolation,
};

GameStartPayload canonicalGameStart(const GameDescriptor& descriptor);
uint64_t expectedInitialDigest(const GameDescriptor& descriptor);

class SpectatorArchive {
public:
    explicit SpectatorArchive(const ArchiveConfig& config = {});

    bool isConfigured() const { return _configured; }

    bool serviceDisabled() const;

    ArchiveResult beginGame(const GameDescriptor& descriptor);

    ArchiveResult appendConfirmed(uint64_t gameId,
                                  const ConfirmedRecord& record,
                                  ConfirmedRecord* stored = nullptr);

    ArchiveResult sealGame(uint64_t gameId,
                           TerminalReason reason,
                           uint64_t postTailStateHash);

    ArchiveResult appendEvent(uint64_t gameId,
                              const SetEvent& event,
                              SetEvent* stored = nullptr);
    bool setEventCount(uint64_t gameId, uint32_t& out) const;
    bool setEventAt(uint64_t gameId, uint32_t index, SetEvent& out) const;
    ArchiveResult readEventBatch(uint64_t gameId,
                                 uint32_t startEvent,
                                 size_t maxEvents,
                                 SetEventBatchPayload& out) const;
    bool matchIndex(uint64_t gameId,
                    std::vector<SetMatchIndexEntry>& out) const;
    bool currentMatch(uint64_t gameId,
                      uint32_t receivedNext,
                      SetMatchIndexEntry& out) const;
    bool seekRecordForMatch(uint64_t gameId,
                            uint32_t matchId,
                            uint32_t& outRecord) const;
    bool setPaused(uint64_t gameId, bool& out) const;

    bool latestGameId(uint64_t& gameId) const;
    bool descriptor(uint64_t gameId, GameDescriptor& out) const;
    bool extent(uint64_t gameId, ArchiveExtent& out) const;
    bool digestAt(uint64_t gameId,
                  uint32_t nextRecord,
                  uint64_t& digest) const;
    bool chainDigestAt(uint64_t gameId,
                       uint32_t nextRecord,
                       uint32_t nextEvent,
                       uint64_t& digest) const;
    bool record(uint64_t gameId,
                uint32_t recordIndex,
                ConfirmedRecord& out) const;

    ArchiveResult readBatch(uint64_t gameId,
                            uint32_t startRecord,
                            size_t maxRecords,
                            ConfirmedInputBatchPayload& out) const;

    size_t retainedGameCount() const;

    bool firstRetainedRecord(uint64_t gameId, uint32_t& out) const;

    bool setResumeFloor(uint64_t gameId, uint32_t lowestRequiredRecord);
    bool clearResumeFloor(uint64_t gameId);

    ArchiveResult addCheckpoint(uint64_t gameId,
                                const ArchiveCheckpointRef& checkpoint);
    bool checkpoints(uint64_t gameId,
                     std::vector<ArchiveCheckpointRef>& out) const;
    bool newestCheckpoint(uint64_t gameId,
                          ArchiveCheckpointRef& out) const;

    ArchiveResult setNewestCheckpointBlob(uint64_t gameId,
                                          uint64_t checkpointId,
                                          const uint8_t* bytes,
                                          size_t length);
    bool newestCheckpointBlobInfo(uint64_t gameId,
                                  uint64_t& checkpointId,
                                  uint32_t& totalBytes) const;
    bool checkpointChunk(uint64_t gameId,
                         uint64_t checkpointId,
                         uint32_t byteOffset,
                         uint32_t length,
                         std::vector<uint8_t>& out) const;

    bool retentionBlocked() const;
    size_t segmentCount(uint64_t gameId) const;
    size_t retainedRecordCount() const;
    size_t allocatedRecordBytes() const;

    void setSegmentAllocationFailuresForTest(size_t count);

private:
    struct Segment {
        uint32_t baseRecord = 0;
        uint32_t count = 0;
        uint32_t capacity = 0;
        std::unique_ptr<ConfirmedRecord[]> records;
    };

    struct Game {
        GameDescriptor descriptor{};
        std::deque<std::unique_ptr<Segment>> segments;
        uint32_t firstRecord = 0;
        uint32_t nextRecord = 0;
        uint64_t firstBaseDigest = 0;
        uint64_t tailDigest = 0;
        uint64_t tailStateHash = 0;
        bool hasResumeFloor = false;
        uint32_t resumeFloor = 0;
        std::vector<ArchiveCheckpointRef> checkpointRefs;
        uint64_t newestBlobCheckpointId = 0;
        std::vector<uint8_t> newestBlob;
        SpectatorSetTimeline timeline;
        bool sealed = false;
        ArchiveTerminal terminal{};
    };

    ArchiveConfig _config{};
    bool _configured = false;
    mutable std::mutex _mutex;
    std::vector<std::unique_ptr<Game>> _games;
    bool _serviceDisabled = false;
    bool _retentionBlocked = false;
    size_t _forcedSegmentFailures = 0;
    size_t _allocatedRecordBytes = 0;

    enum class SegmentAppendResult {
        Ok,
        ByteLimit,
        AllocationFailed,
    };

    Game* findGame(uint64_t gameId);
    const Game* findGame(uint64_t gameId) const;

    const ConfirmedRecord* recordPointer(const Game& game,
                                         uint32_t index) const;
    bool digestAtLocked(const Game& game,
                        uint32_t nextRecord,
                        uint64_t& digest) const;
    bool chainDigestAtLocked(const Game& game,
                             uint32_t nextRecord,
                             uint32_t nextEvent,
                             uint64_t& digest) const;
    uint32_t retentionFloor(const Game& game) const;
    void rotateLocked(Game& game, uint32_t projectedNextRecord);
    SegmentAppendResult appendSegmentLocked(Game& game,
                                            uint32_t baseRecord);
    size_t gameRecordBytesLocked(const Game& game) const;
    void eraseGameLocked(
        std::vector<std::unique_ptr<Game>>::iterator game);
};

}
