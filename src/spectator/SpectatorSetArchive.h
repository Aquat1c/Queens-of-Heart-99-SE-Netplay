#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qnetplay::spectator {

constexpr size_t kSetFighterSlots = 4u;
constexpr uint8_t kInactiveFighterId = 0xFFu;
constexpr uint8_t kMaxSelectableFighterId = 33u;
constexpr uint8_t kSelectorCells = 24u;
constexpr uint8_t kMaxPaletteIndex = 3u;
constexpr uint8_t kMaxConfirmState = 4u;

enum class SetEventClass : uint8_t {
    DeterministicCommand = 1,
    Assertion = 2,
    PresentationMetadata = 3,
    Terminal = 4,
};

// Numeric values are mirrored from qreplay::EventKind and must not drift.
enum class SetEventKind : uint16_t {
    RosterSelected = 1,
    StageSelected = 2,
    MatchStarted = 3,
    RoundStarted = 4,
    RoundEnded = 5,
    MatchResult = 6,
    RematchRequested = 7,
    ReturnedToCharacterSelect = 8,
    RouteEpochChanged = 9,
    SessionPaused = 20,
    SessionResumed = 21,
    DelayChanged = 40,
    NetworkSample = 41,
    WallClockPause = 42,
    PaletteSelected = 43,
    SegmentTerminal = 60,
};

enum class SetSegmentTerminalReason : uint16_t {
    Completed = 1,
    Disconnected = 2,
    ProcessExit = 3,
    RecordingFailure = 4,
    RecoveredPrefix = 5,
    RecorderOverflow = 6,
};

bool isKnownSetEventKind(uint16_t value);
bool isKnownSetSegmentTerminalReason(uint16_t value);

SetEventClass setEventClassOf(SetEventKind kind);

struct SetFighterIdentity {
    uint8_t nativeCharacterId = kInactiveFighterId; // 0..33, 0xFF inactive
    uint8_t paletteIndex = 0;

    bool operator==(const SetFighterIdentity& o) const {
        return nativeCharacterId == o.nativeCharacterId
            && paletteIndex == o.paletteIndex;
    }
    bool operator!=(const SetFighterIdentity& o) const {
        return !(*this == o);
    }
};

struct SetSelectorProof {
    uint8_t cursorCell = 0;
    uint8_t alternateFlag = 0;
    uint8_t confirmState = 0;  // native selector lifecycle 0..4

    bool operator==(const SetSelectorProof& o) const {
        return cursorCell == o.cursorCell
            && alternateFlag == o.alternateFlag
            && confirmState == o.confirmState;
    }
    bool operator!=(const SetSelectorProof& o) const {
        return !(*this == o);
    }
};

constexpr size_t kSetPaletteEntryBytes = 64;

struct SetEvent {
    uint32_t eventIndex = 0;
    uint32_t recordCursor = 0;
    uint64_t recordDigest = 0;
    uint32_t timelineFrame = 0;
    SetEventKind kind = SetEventKind::MatchStarted;
    SetSegmentTerminalReason terminalReason =
        SetSegmentTerminalReason::Completed;
    uint32_t matchId = 0;
    uint32_t rosterEpoch = 0;
    uint64_t valueA = 0;
    uint64_t valueB = 0;
    uint32_t valueC = 0;
    uint8_t selectorProofMask = 0;
    uint8_t activeSlotMask = 0;
    SetFighterIdentity roster[kSetFighterSlots]{};
    SetSelectorProof selectorProof[kSetFighterSlots]{};
    uint8_t slotToSourceRoute[kSetFighterSlots] = {0, 1, 2, 3};
    uint8_t paletteEntries[kSetPaletteEntryBytes] = {};
    uint64_t stateProof = 0;
    uint64_t rollingDigest = 0;

    bool operator==(const SetEvent& other) const;
    bool operator!=(const SetEvent& other) const { return !(*this == other); }
};

uint64_t advanceSetEventDigest(uint64_t previous,
                               uint64_t gameId,
                               const SetEvent& event);

bool isValidSetEvent(const SetEvent& event);

enum class SetEventResult {
    Ok,
    ExactDuplicate,
    InvalidArgument,
    Gap,
    ConflictingDuplicate,
    BoundaryMismatch,
    DigestMismatch,
    OrderViolation,
    LimitReached,
};

struct SetMatchIndexEntry {
    uint32_t matchId = 0;
    uint32_t ordinal = 0;
    uint32_t firstRecord = 0;
    uint32_t lastRecord = 0;
    uint32_t firstTimelineFrame = 0;
    uint32_t lastTimelineFrame = 0;
    uint32_t roundCount = 0;
    uint8_t winningSideMask = 0;
    uint8_t roundsSideZero = 0;
    uint8_t roundsSideOne = 0;
    bool independentBootstrap = false;
    bool ended = false;
    uint32_t startEventIndex = 0;
    uint32_t endEventIndex = 0;
    uint32_t rosterEpoch = 0;
    uint32_t stageId = 0;
    SetFighterIdentity roster[kSetFighterSlots]{};
    uint8_t activeSlotMask = 0;
    uint8_t ownership[kSetFighterSlots] = {0, 1, 2, 3};
    uint64_t checkpointId = 0;
    uint32_t checkpointRecord = 0;
};

constexpr size_t kMaxSetEvents = 65536u;

class SpectatorSetTimeline {
public:
    SpectatorSetTimeline() = default;

    void reset(uint64_t gameId, uint32_t firstTimelineFrame);

    struct BindSeed {
        uint32_t openMatchId = 0;
        uint32_t highestMatchId = 0;
        uint32_t matchesSoFar = 0;
        bool paused = false;
    };

    void bindAt(uint32_t baseEventIndex, const BindSeed& seed);
    uint32_t baseEventIndex() const { return _baseEventIndex; }
    bool isBound() const { return _bound; }
    uint64_t gameId() const { return _gameId; }

    SetEventResult append(const SetEvent& event,
                          uint32_t confirmedNextRecord,
                          uint64_t chainDigestBefore,
                          SetEvent* stored = nullptr);

    size_t eventCount() const { return _events.size(); }
    uint32_t nextEventIndex() const {
        return _baseEventIndex + static_cast<uint32_t>(_events.size());
    }
    bool event(uint32_t index, SetEvent& out) const;
    const std::vector<SetEvent>& events() const { return _events; }

    uint32_t eventsBeforeRecord(uint32_t cursor) const;
    bool boundaryDigest(uint32_t cursor, uint64_t& digest) const;
    bool nextEventCursorAfter(uint32_t cursor, uint32_t& out) const;

    const std::vector<SetMatchIndexEntry>& matches() const {
        return _matches;
    }
    bool matchById(uint32_t matchId, SetMatchIndexEntry& out) const;
    bool matchByOrdinal(uint32_t ordinal, SetMatchIndexEntry& out) const;
    bool currentMatch(uint32_t receivedNext, SetMatchIndexEntry& out) const;

    bool seekRecordForMatch(uint32_t matchId, uint32_t& outRecord) const;
    bool seekRecordForCurrentMatch(uint32_t receivedNext,
                                   uint32_t& outRecord) const;

    bool noteCheckpoint(uint64_t checkpointId, uint32_t recordCursor);
    bool checkpointAtOrAfter(uint32_t fromRecord,
                             uint64_t& checkpointId,
                             uint32_t& recordCursor) const;

    bool paused() const { return _paused; }
    uint32_t matchCount() const {
        return static_cast<uint32_t>(_matches.size());
    }
    void setScore(uint32_t& sideZero, uint32_t& sideOne) const;

private:
    bool _bound = false;
    uint64_t _gameId = 0;
    uint32_t _firstTimelineFrame = 0;
    std::vector<SetEvent> _events;
    std::vector<SetMatchIndexEntry> _matches;
    bool _paused = false;
    uint32_t _openMatchId = 0;
    uint32_t _highestMatchId = 0;
    uint32_t _baseEventIndex = 0;
    uint32_t _matchOrdinalBase = 0;
    uint32_t _pendingRosterEpoch = 0;
    uint32_t _pendingStageId = 0;
    uint8_t _pendingActiveSlotMask = 0;
    uint8_t _pendingOwnership[kSetFighterSlots] = {0, 1, 2, 3};
    SetFighterIdentity _pendingRoster[kSetFighterSlots]{};

    SetEventResult checkOrder(const SetEvent& event) const;
    void applyToIndex(const SetEvent& event);
    SetMatchIndexEntry* findMatch(uint32_t matchId);
};

}
