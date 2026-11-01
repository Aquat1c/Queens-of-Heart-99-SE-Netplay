#include "SpectatorSetArchive.h"

#include <algorithm>
#include <cstring>

namespace qnetplay::spectator {
namespace {

uint64_t avalanche(uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

void mixByte(uint64_t& hash, uint8_t value) {
    hash ^= value;
    hash *= 0x100000001B3ull;
}

void mix16(uint64_t& hash, uint16_t value) {
    for (unsigned i = 0; i < 2; ++i)
        mixByte(hash, static_cast<uint8_t>(value >> (i * 8)));
}

void mix32(uint64_t& hash, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        mixByte(hash, static_cast<uint8_t>(value >> (i * 8)));
}

void mix64(uint64_t& hash, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        mixByte(hash, static_cast<uint8_t>(value >> (i * 8)));
}

bool isInactiveIdentity(const SetFighterIdentity& identity) {
    return identity.nativeCharacterId == kInactiveFighterId
        && identity.paletteIndex == 0;
}

bool isZeroProof(const SetSelectorProof& proof) {
    return proof.cursorCell == 0
        && proof.alternateFlag == 0
        && proof.confirmState == 0;
}

bool isValidProof(const SetSelectorProof& proof) {
    return proof.cursorCell < kSelectorCells
        && proof.alternateFlag <= 1u
        && proof.confirmState <= kMaxConfirmState;
}

bool hasNeutralRosterPayload(const SetEvent& event) {
    if (event.selectorProofMask != 0) return false;
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
        if (!isInactiveIdentity(event.roster[slot])) return false;
        if (!isZeroProof(event.selectorProof[slot])) return false;
    }
    return true;
}

bool hasDefaultRoute(const SetEvent& event) {
    for (uint8_t slot = 0; slot < kSetFighterSlots; ++slot) {
        if (event.slotToSourceRoute[slot] != slot) return false;
    }
    return true;
}

}

bool isKnownSetEventKind(uint16_t value) {
    switch (static_cast<SetEventKind>(value)) {
        case SetEventKind::RosterSelected:
        case SetEventKind::StageSelected:
        case SetEventKind::MatchStarted:
        case SetEventKind::RoundStarted:
        case SetEventKind::RoundEnded:
        case SetEventKind::MatchResult:
        case SetEventKind::RematchRequested:
        case SetEventKind::ReturnedToCharacterSelect:
        case SetEventKind::RouteEpochChanged:
        case SetEventKind::SessionPaused:
        case SetEventKind::SessionResumed:
        case SetEventKind::DelayChanged:
        case SetEventKind::NetworkSample:
        case SetEventKind::WallClockPause:
        case SetEventKind::PaletteSelected:
        case SetEventKind::SegmentTerminal:
            return true;
    }
    return false;
}

bool isKnownSetSegmentTerminalReason(uint16_t value) {
    return value >= 1u && value <= 6u;
}

SetEventClass setEventClassOf(SetEventKind kind) {
    const uint16_t value = static_cast<uint16_t>(kind);
    if (value >= 1u && value <= 9u) return SetEventClass::Assertion;
    if (value >= 20u && value <= 21u)
        return SetEventClass::DeterministicCommand;
    if (value >= 40u && value <= 43u)
        return SetEventClass::PresentationMetadata;
    if (value == 60u) return SetEventClass::Terminal;
    return SetEventClass::Assertion;
}

bool SetEvent::operator==(const SetEvent& other) const {
    if (eventIndex != other.eventIndex
        || recordCursor != other.recordCursor
        || recordDigest != other.recordDigest
        || timelineFrame != other.timelineFrame
        || kind != other.kind
        || terminalReason != other.terminalReason
        || matchId != other.matchId
        || rosterEpoch != other.rosterEpoch
        || valueA != other.valueA
        || valueB != other.valueB
        || valueC != other.valueC
        || selectorProofMask != other.selectorProofMask
        || activeSlotMask != other.activeSlotMask
        || stateProof != other.stateProof
        || rollingDigest != other.rollingDigest) {
        return false;
    }
    if (std::memcmp(paletteEntries, other.paletteEntries,
                    kSetPaletteEntryBytes) != 0) {
        return false;
    }
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
        if (roster[slot] != other.roster[slot]) return false;
        if (selectorProof[slot] != other.selectorProof[slot]) return false;
        if (slotToSourceRoute[slot] != other.slotToSourceRoute[slot])
            return false;
    }
    return true;
}

uint64_t advanceSetEventDigest(uint64_t previous,
                               uint64_t gameId,
                               const SetEvent& event) {
    uint64_t hash = 0xCBF29CE484222325ull;
    mix64(hash, 0x5153502D45564E54ull); // "QSP-EVNT"
    mix64(hash, previous);
    mix64(hash, gameId);
    mix32(hash, event.eventIndex);
    mix32(hash, event.recordCursor);
    mix64(hash, event.recordDigest);
    mix32(hash, event.timelineFrame);
    mix16(hash, static_cast<uint16_t>(event.kind));
    mix16(hash, static_cast<uint16_t>(event.terminalReason));
    mix32(hash, event.matchId);
    mix32(hash, event.rosterEpoch);
    mix64(hash, event.valueA);
    mix64(hash, event.valueB);
    mix32(hash, event.valueC);
    mixByte(hash, event.selectorProofMask);
    mixByte(hash, event.activeSlotMask);
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
        mixByte(hash, event.roster[slot].nativeCharacterId);
        mixByte(hash, event.roster[slot].paletteIndex);
    }
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
        mixByte(hash, event.selectorProof[slot].cursorCell);
        mixByte(hash, event.selectorProof[slot].alternateFlag);
        mixByte(hash, event.selectorProof[slot].confirmState);
    }
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot)
        mixByte(hash, event.slotToSourceRoute[slot]);
    for (size_t i = 0; i < kSetPaletteEntryBytes; ++i)
        mixByte(hash, event.paletteEntries[i]);
    mix64(hash, event.stateProof);
    return avalanche(hash);
}

bool isValidSetEvent(const SetEvent& event) {
    if (!isKnownSetEventKind(static_cast<uint16_t>(event.kind)))
        return false;
    if (!isKnownSetSegmentTerminalReason(
            static_cast<uint16_t>(event.terminalReason))) {
        return false;
    }
    if (event.kind != SetEventKind::SegmentTerminal
        && event.terminalReason != SetSegmentTerminalReason::Completed) {
        return false;
    }
    for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
        if (event.slotToSourceRoute[slot] >= kSetFighterSlots) return false;
    }
    if ((event.activeSlotMask & 0xF0u) != 0) return false;

    if (event.kind == SetEventKind::RosterSelected) {
        if (event.rosterEpoch == 0) return false;
        if ((event.activeSlotMask & 0x03u) != 0x03u) return false;
        if ((event.selectorProofMask & ~event.activeSlotMask) != 0)
            return false;
        if (event.valueA != 0 || event.valueB != 0 || event.valueC != 0)
            return false;
        for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
            const bool active =
                ((event.activeSlotMask >> slot) & 1u) != 0;
            const SetFighterIdentity& identity = event.roster[slot];
            if (active) {
                if (identity.nativeCharacterId > kMaxSelectableFighterId)
                    return false;
                if (identity.paletteIndex > kMaxPaletteIndex) return false;
            } else if (!isInactiveIdentity(identity)) {
                return false;
            }
            const bool proven =
                ((event.selectorProofMask >> slot) & 1u) != 0;
            if (proven) {
                if (!isValidProof(event.selectorProof[slot])) return false;
            } else if (!isZeroProof(event.selectorProof[slot])) {
                return false;
            }
        }
        return true;
    }

    if (!hasNeutralRosterPayload(event)) return false;
    if (event.rosterEpoch != 0) return false;
    if (event.kind != SetEventKind::PaletteSelected) {
        for (size_t i = 0; i < kSetPaletteEntryBytes; ++i) {
            if (event.paletteEntries[i] != 0) return false;
        }
    }

    switch (event.kind) {
        case SetEventKind::StageSelected:
            return event.valueA == 0 && event.valueB == 0
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::MatchStarted:
            return event.matchId != 0 && event.valueC != 0
                && event.valueA == 0 && event.valueB == 0
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::RoundStarted:
            return event.matchId != 0 && event.valueC != 0
                && event.valueA == 0 && event.valueB == 0
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::RoundEnded:
            return event.matchId != 0 && event.valueC != 0
                && event.valueA <= 0x0Fu && event.valueB == 0
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::MatchResult:
            return event.matchId != 0 && event.valueA <= 0x0Fu
                && event.valueC == 0
                && (event.valueB >> 16) == 0
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::RematchRequested:
            return event.matchId != 0
                && event.valueA == 0 && event.valueB == 0
                && event.valueC < kSetFighterSlots
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::ReturnedToCharacterSelect:
            return event.matchId != 0
                && event.valueA == 0 && event.valueB == 0
                && event.valueC == 0
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::RouteEpochChanged:
            return event.valueC != 0
                && (event.activeSlotMask & 0x03u) == 0x03u;
        case SetEventKind::PaletteSelected:
            return event.matchId != 0
                && event.valueA == 0 && event.valueB == 0
                && event.valueC <= kMaxPaletteIndex
                && event.activeSlotMask != 0
                && (event.activeSlotMask
                    & (event.activeSlotMask - 1u)) == 0u
                && hasDefaultRoute(event);
        case SetEventKind::SessionPaused:
        case SetEventKind::SessionResumed:
            return event.valueA == 0 && event.valueB == 0
                && event.valueC < kSetFighterSlots
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::DelayChanged:
            return event.valueA <= 120u && event.valueB <= 120u
                && event.valueC <= 120u
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::NetworkSample:
            return event.valueB <= 1000u && event.valueC == 0
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::WallClockPause:
            return event.valueB == 0 && event.valueC == 0
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::SegmentTerminal:
            return event.valueA == 0 && event.valueB == 0
                && event.valueC == 0
                && event.activeSlotMask == 0 && hasDefaultRoute(event);
        case SetEventKind::RosterSelected:
            break;
    }
    return false;
}

void SpectatorSetTimeline::reset(uint64_t gameId,
                                 uint32_t firstTimelineFrame) {
    _bound = gameId != 0;
    _gameId = gameId;
    _firstTimelineFrame = firstTimelineFrame;
    _events.clear();
    _matches.clear();
    _paused = false;
    _openMatchId = 0;
    _highestMatchId = 0;
    _baseEventIndex = 0;
    _matchOrdinalBase = 0;
    _pendingRosterEpoch = 0;
    _pendingStageId = 0;
    _pendingActiveSlotMask = 0;
    for (uint8_t slot = 0; slot < kSetFighterSlots; ++slot) {
        _pendingOwnership[slot] = slot;
        _pendingRoster[slot] = SetFighterIdentity{};
    }
}

void SpectatorSetTimeline::bindAt(uint32_t baseEventIndex,
                                  const BindSeed& seed) {
    _events.clear();
    _matches.clear();
    _baseEventIndex = baseEventIndex;
    _openMatchId = seed.openMatchId;
    _highestMatchId = seed.highestMatchId;
    _matchOrdinalBase = seed.matchesSoFar;
    _paused = seed.paused;
}

SetMatchIndexEntry* SpectatorSetTimeline::findMatch(uint32_t matchId) {
    for (SetMatchIndexEntry& entry : _matches) {
        if (entry.matchId == matchId) return &entry;
    }
    return nullptr;
}

SetEventResult SpectatorSetTimeline::checkOrder(const SetEvent& event) const {
    switch (event.kind) {
        case SetEventKind::MatchStarted:
            if (_openMatchId != 0) return SetEventResult::OrderViolation;
            if (event.matchId <= _highestMatchId)
                return SetEventResult::OrderViolation;
            if (event.valueC
                != _matchOrdinalBase
                       + static_cast<uint32_t>(_matches.size()) + 1u) {
                return SetEventResult::OrderViolation;
            }
            return SetEventResult::Ok;
        case SetEventKind::RoundStarted:
        case SetEventKind::RoundEnded:
        case SetEventKind::MatchResult:
            if (_openMatchId == 0 || event.matchId != _openMatchId)
                return SetEventResult::OrderViolation;
            return SetEventResult::Ok;
        case SetEventKind::RematchRequested:
        case SetEventKind::ReturnedToCharacterSelect: {
            for (const SetMatchIndexEntry& entry : _matches) {
                if (entry.matchId == event.matchId)
                    return entry.ended
                        ? SetEventResult::Ok
                        : SetEventResult::OrderViolation;
            }
            return SetEventResult::OrderViolation;
        }
        case SetEventKind::SessionPaused:
            return _paused ? SetEventResult::OrderViolation
                           : SetEventResult::Ok;
        case SetEventKind::SessionResumed:
            return _paused ? SetEventResult::Ok
                           : SetEventResult::OrderViolation;
        case SetEventKind::RosterSelected:
            return _openMatchId == 0 ? SetEventResult::Ok
                                     : SetEventResult::OrderViolation;
        case SetEventKind::PaletteSelected:
            if (_openMatchId == 0 || event.matchId != _openMatchId)
                return SetEventResult::OrderViolation;
            return SetEventResult::Ok;
        case SetEventKind::StageSelected:
        case SetEventKind::RouteEpochChanged:
        case SetEventKind::DelayChanged:
        case SetEventKind::NetworkSample:
        case SetEventKind::WallClockPause:
        case SetEventKind::SegmentTerminal:
            return SetEventResult::Ok;
    }
    return SetEventResult::InvalidArgument;
}

void SpectatorSetTimeline::applyToIndex(const SetEvent& event) {
    switch (event.kind) {
        case SetEventKind::RosterSelected: {
            _pendingRosterEpoch = event.rosterEpoch;
            _pendingActiveSlotMask = event.activeSlotMask;
            for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
                _pendingRoster[slot] = event.roster[slot];
                _pendingOwnership[slot] = event.slotToSourceRoute[slot];
            }
            break;
        }
        case SetEventKind::StageSelected:
            _pendingStageId = event.valueC;
            break;
        case SetEventKind::RouteEpochChanged:
            for (size_t slot = 0; slot < kSetFighterSlots; ++slot)
                _pendingOwnership[slot] = event.slotToSourceRoute[slot];
            _pendingActiveSlotMask = event.activeSlotMask;
            break;
        case SetEventKind::MatchStarted: {
            SetMatchIndexEntry entry{};
            entry.matchId = event.matchId;
            entry.ordinal = event.valueC;
            entry.firstRecord = event.recordCursor;
            entry.lastRecord = event.recordCursor;
            entry.firstTimelineFrame = event.timelineFrame;
            entry.lastTimelineFrame = event.timelineFrame;
            entry.startEventIndex = event.eventIndex;
            entry.rosterEpoch = _pendingRosterEpoch;
            entry.stageId = _pendingStageId;
            entry.activeSlotMask = _pendingActiveSlotMask;
            entry.independentBootstrap = _matches.empty();
            for (size_t slot = 0; slot < kSetFighterSlots; ++slot) {
                entry.roster[slot] = _pendingRoster[slot];
                entry.ownership[slot] = _pendingOwnership[slot];
            }
            _matches.push_back(entry);
            _openMatchId = event.matchId;
            _highestMatchId = event.matchId;
            break;
        }
        case SetEventKind::RoundStarted: {
            if (SetMatchIndexEntry* entry = findMatch(event.matchId)) {
                entry->roundCount = std::max(entry->roundCount, event.valueC);
                entry->lastRecord = event.recordCursor;
                entry->lastTimelineFrame = event.timelineFrame;
            }
            break;
        }
        case SetEventKind::RoundEnded: {
            if (SetMatchIndexEntry* entry = findMatch(event.matchId)) {
                entry->roundCount = std::max(entry->roundCount, event.valueC);
                entry->lastRecord = event.recordCursor;
                entry->lastTimelineFrame = event.timelineFrame;
                const uint8_t side =
                    static_cast<uint8_t>(event.valueA & 0x0Fu);
                if ((side & 0x05u) != 0) ++entry->roundsSideZero;
                else if ((side & 0x0Au) != 0) ++entry->roundsSideOne;
            }
            break;
        }
        case SetEventKind::MatchResult: {
            if (SetMatchIndexEntry* entry = findMatch(event.matchId)) {
                entry->ended = true;
                entry->endEventIndex = event.eventIndex;
                entry->lastRecord = event.recordCursor;
                entry->lastTimelineFrame = event.timelineFrame;
                entry->winningSideMask =
                    static_cast<uint8_t>(event.valueA & 0x0Fu);
                entry->roundsSideZero =
                    static_cast<uint8_t>(event.valueB & 0xFFu);
                entry->roundsSideOne =
                    static_cast<uint8_t>((event.valueB >> 8) & 0xFFu);
            }
            _openMatchId = 0;
            break;
        }
        case SetEventKind::SessionPaused:
            _paused = true;
            break;
        case SetEventKind::SessionResumed:
            _paused = false;
            break;
        case SetEventKind::RematchRequested:
        case SetEventKind::ReturnedToCharacterSelect:
        case SetEventKind::DelayChanged:
        case SetEventKind::NetworkSample:
        case SetEventKind::WallClockPause:
        case SetEventKind::PaletteSelected:
        case SetEventKind::SegmentTerminal:
            break;
    }
}

SetEventResult SpectatorSetTimeline::append(
    const SetEvent& requested,
    uint32_t confirmedNextRecord,
    uint64_t chainDigestBefore,
    SetEvent* stored) {
    if (!_bound) return SetEventResult::InvalidArgument;
    if (!isValidSetEvent(requested)) return SetEventResult::InvalidArgument;
    if (_events.size() >= kMaxSetEvents) return SetEventResult::LimitReached;

    const uint32_t frontier = nextEventIndex();
    if (requested.eventIndex < _baseEventIndex) {
        return SetEventResult::ExactDuplicate;
    }
    if (requested.eventIndex < frontier) {
        const SetEvent& existing =
            _events[requested.eventIndex - _baseEventIndex];
        SetEvent probe = requested;
        if (probe.rollingDigest == 0) probe.rollingDigest =
            existing.rollingDigest;
        return probe == existing
            ? SetEventResult::ExactDuplicate
            : SetEventResult::ConflictingDuplicate;
    }
    if (requested.eventIndex > frontier) return SetEventResult::Gap;

    if (requested.recordCursor != confirmedNextRecord)
        return SetEventResult::BoundaryMismatch;
    if (!_events.empty()
        && requested.recordCursor < _events.back().recordCursor) {
        return SetEventResult::BoundaryMismatch;
    }
    if (requested.timelineFrame
        != _firstTimelineFrame + requested.recordCursor) {
        return SetEventResult::BoundaryMismatch;
    }
    if (requested.recordDigest != chainDigestBefore)
        return SetEventResult::DigestMismatch;

    const SetEventResult order = checkOrder(requested);
    if (order != SetEventResult::Ok) return order;

    SetEvent canonical = requested;
    canonical.rollingDigest =
        advanceSetEventDigest(chainDigestBefore, _gameId, canonical);
    if (requested.rollingDigest != 0
        && requested.rollingDigest != canonical.rollingDigest) {
        return SetEventResult::DigestMismatch;
    }
    _events.push_back(canonical);
    applyToIndex(canonical);
    if (stored) *stored = canonical;
    return SetEventResult::Ok;
}

bool SpectatorSetTimeline::event(uint32_t index, SetEvent& out) const {
    if (index < _baseEventIndex) return false;
    const uint32_t relative = index - _baseEventIndex;
    if (relative >= _events.size()) return false;
    out = _events[relative];
    return true;
}

uint32_t SpectatorSetTimeline::eventsBeforeRecord(uint32_t cursor) const {
    uint32_t count = _baseEventIndex;
    for (const SetEvent& event : _events) {
        if (event.recordCursor > cursor) break;
        ++count;
    }
    return count;
}

bool SpectatorSetTimeline::boundaryDigest(uint32_t cursor,
                                          uint64_t& digest) const {
    for (size_t i = _events.size(); i > 0; --i) {
        const SetEvent& event = _events[i - 1];
        if (event.recordCursor < cursor) return false;
        if (event.recordCursor == cursor) {
            digest = event.rollingDigest;
            return true;
        }
    }
    return false;
}

bool SpectatorSetTimeline::nextEventCursorAfter(uint32_t cursor,
                                                uint32_t& out) const {
    for (const SetEvent& event : _events) {
        if (event.recordCursor > cursor) {
            out = event.recordCursor;
            return true;
        }
    }
    return false;
}

bool SpectatorSetTimeline::matchById(uint32_t matchId,
                                     SetMatchIndexEntry& out) const {
    for (const SetMatchIndexEntry& entry : _matches) {
        if (entry.matchId == matchId) {
            out = entry;
            return true;
        }
    }
    return false;
}

bool SpectatorSetTimeline::matchByOrdinal(uint32_t ordinal,
                                          SetMatchIndexEntry& out) const {
    for (const SetMatchIndexEntry& entry : _matches) {
        if (entry.ordinal == ordinal) {
            out = entry;
            return true;
        }
    }
    return false;
}

bool SpectatorSetTimeline::currentMatch(uint32_t receivedNext,
                                        SetMatchIndexEntry& out) const {
    const SetMatchIndexEntry* best = nullptr;
    for (const SetMatchIndexEntry& entry : _matches) {
        if (entry.firstRecord > receivedNext) continue;
        if (!best || entry.firstRecord >= best->firstRecord) best = &entry;
    }
    if (!best) return false;
    out = *best;
    return true;
}

bool SpectatorSetTimeline::seekRecordForMatch(uint32_t matchId,
                                              uint32_t& outRecord) const {
    SetMatchIndexEntry entry{};
    if (!matchById(matchId, entry)) return false;
    outRecord = entry.firstRecord;
    return true;
}

bool SpectatorSetTimeline::seekRecordForCurrentMatch(
    uint32_t receivedNext, uint32_t& outRecord) const {
    SetMatchIndexEntry entry{};
    if (!currentMatch(receivedNext, entry)) return false;
    outRecord = entry.firstRecord;
    return true;
}

bool SpectatorSetTimeline::noteCheckpoint(uint64_t checkpointId,
                                          uint32_t recordCursor) {
    SetMatchIndexEntry* best = nullptr;
    for (SetMatchIndexEntry& entry : _matches) {
        if (entry.firstRecord > recordCursor) continue;
        if (!best || entry.firstRecord >= best->firstRecord) best = &entry;
    }
    if (!best) return false;
    if (checkpointId != 0 && recordCursor >= best->checkpointRecord) {
        best->checkpointId = checkpointId;
        best->checkpointRecord = recordCursor;
    }
    return true;
}

bool SpectatorSetTimeline::checkpointAtOrAfter(
    uint32_t fromRecord,
    uint64_t& checkpointId,
    uint32_t& recordCursor) const {
    const SetMatchIndexEntry* best = nullptr;
    for (const SetMatchIndexEntry& entry : _matches) {
        if (entry.checkpointId == 0) continue;
        if (entry.checkpointRecord < fromRecord) continue;
        if (!best || entry.checkpointRecord >= best->checkpointRecord)
            best = &entry;
    }
    if (!best) return false;
    checkpointId = best->checkpointId;
    recordCursor = best->checkpointRecord;
    return true;
}

void SpectatorSetTimeline::setScore(uint32_t& sideZero,
                                    uint32_t& sideOne) const {
    sideZero = 0;
    sideOne = 0;
    for (const SetMatchIndexEntry& entry : _matches) {
        if (!entry.ended) continue;
        if ((entry.winningSideMask & 0x05u) != 0) ++sideZero;
        else if ((entry.winningSideMask & 0x0Au) != 0) ++sideOne;
    }
}

}
