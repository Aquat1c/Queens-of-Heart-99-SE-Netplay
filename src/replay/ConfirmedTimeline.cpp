#include "ConfirmedTimeline.h"

#include <limits>

namespace qreplay {

const char* confirmedTimelineFaultTextV2(ConfirmedTimelineFaultV2 fault) {
    switch (fault) {
        case ConfirmedTimelineFaultV2::None: return "none";
        case ConfirmedTimelineFaultV2::AlreadyStarted:
            return "timeline already started";
        case ConfirmedTimelineFaultV2::BadStart: return "bad game start";
        case ConfirmedTimelineFaultV2::NotStarted:
            return "timeline not started";
        case ConfirmedTimelineFaultV2::AfterEnd: return "item after game end";
        case ConfirmedTimelineFaultV2::FrameGap: return "replay frame gap";
        case ConfirmedTimelineFaultV2::SourceFrameGap:
            return "source frame gap";
        case ConfirmedTimelineFaultV2::ConflictingDuplicate:
            return "conflicting duplicate";
        case ConfirmedTimelineFaultV2::CapacityExceeded:
            return "timeline capacity exceeded";
        case ConfirmedTimelineFaultV2::BadFrame: return "bad game frame";
        case ConfirmedTimelineFaultV2::BadEvent: return "bad game event";
        case ConfirmedTimelineFaultV2::EventSequenceGap:
            return "event sequence gap";
        case ConfirmedTimelineFaultV2::EventPhaseMismatch:
            return "event phase mismatch";
        case ConfirmedTimelineFaultV2::LifecycleDiscontinuity:
            return "lifecycle discontinuity";
        case ConfirmedTimelineFaultV2::StateProofDiscontinuity:
            return "state proof discontinuity";
        case ConfirmedTimelineFaultV2::RollingDigestMismatch:
            return "rolling digest mismatch";
        case ConfirmedTimelineFaultV2::MissingInitialAssertions:
            return "missing initial assertions";
        case ConfirmedTimelineFaultV2::TerminalMismatch:
            return "game terminal mismatch";
        case ConfirmedTimelineFaultV2::CompletedBoundaryMismatch:
            return "completed boundary mismatch";
        case ConfirmedTimelineFaultV2::CanonicalDigestMismatch:
            return "canonical game digest mismatch";
    }
    return "unknown";
}

ParseStatus confirmedTimelineFaultStatusV2(
    ConfirmedTimelineFaultV2 fault) {
    switch (fault) {
        case ConfirmedTimelineFaultV2::None: return ParseStatus::Ok;
        case ConfirmedTimelineFaultV2::AlreadyStarted:
        case ConfirmedTimelineFaultV2::AfterEnd:
        case ConfirmedTimelineFaultV2::ConflictingDuplicate:
        case ConfirmedTimelineFaultV2::EventSequenceGap:
            return ParseStatus::SequenceRegression;
        case ConfirmedTimelineFaultV2::BadStart:
        case ConfirmedTimelineFaultV2::NotStarted:
        case ConfirmedTimelineFaultV2::MissingInitialAssertions:
            return ParseStatus::MissingSessionStart;
        case ConfirmedTimelineFaultV2::FrameGap:
        case ConfirmedTimelineFaultV2::SourceFrameGap:
        case ConfirmedTimelineFaultV2::TerminalMismatch:
        case ConfirmedTimelineFaultV2::CompletedBoundaryMismatch:
            return ParseStatus::FrameGap;
        case ConfirmedTimelineFaultV2::CapacityExceeded:
            return ParseStatus::LimitExceeded;
        case ConfirmedTimelineFaultV2::BadFrame:
            return ParseStatus::BadLaneShape;
        case ConfirmedTimelineFaultV2::BadEvent:
        case ConfirmedTimelineFaultV2::EventPhaseMismatch:
            return ParseStatus::BadEventPayload;
        case ConfirmedTimelineFaultV2::LifecycleDiscontinuity:
            return ParseStatus::FrameConflict;
        case ConfirmedTimelineFaultV2::StateProofDiscontinuity:
        case ConfirmedTimelineFaultV2::RollingDigestMismatch:
        case ConfirmedTimelineFaultV2::CanonicalDigestMismatch:
            return ParseStatus::DigestMismatch;
    }
    return ParseStatus::IoError;
}

bool ConfirmedTimelineValidatorV2::fail(
    ConfirmedTimelineFaultV2 fault, ParseStatus& why) {
    failed_ = true;
    lastFault_ = fault;
    why = confirmedTimelineFaultStatusV2(fault);
    return false;
}

bool ConfirmedTimelineValidatorV2::begin(
    const ConfirmedGameStartV2& start, ParseStatus& why) {
    why = ParseStatus::Ok;
    if (started_)
        return fail(ConfirmedTimelineFaultV2::AlreadyStarted, why);
    if (!isValidGameManifestV2(start.manifest)
        || !isValidDeterministicGameStartV2(start.start)
        || start.start.initialRollingDigest
            != initialReplayDigestV2(start)) {
        return fail(ConfirmedTimelineFaultV2::BadStart, why);
    }

    CanonicalGameDigestV2 canonical;
    if (!canonical.begin(start))
        return fail(ConfirmedTimelineFaultV2::BadStart, why);

    start_ = start;
    canonical_ = canonical;
    started_ = true;
    sealed_ = false;
    failed_ = false;
    hasPreviousFrame_ = false;
    hasLastFrame_ = false;
    sawGameResult_ = false;
    nextRecord_ = 0u;
    nextEventSequence_ = 0u;
    rollingDigest_ = start.start.initialRollingDigest;
    lastFault_ = ConfirmedTimelineFaultV2::None;
    return true;
}

bool ConfirmedTimelineValidatorV2::appendEvent(
    const ConfirmedGameEventV2& event, GameEventPhaseV2 phase,
    uint32_t frame, ParseStatus& why) {
    if (sawGameResult_)
        return fail(
            ConfirmedTimelineFaultV2::CompletedBoundaryMismatch, why);
    if (!isValidConfirmedGameEventV2(event))
        return fail(ConfirmedTimelineFaultV2::BadEvent, why);
    if (event.phase != phase || event.replayFrame != frame)
        return fail(ConfirmedTimelineFaultV2::EventPhaseMismatch, why);
    if (event.eventSequence != nextEventSequence_)
        return fail(ConfirmedTimelineFaultV2::EventSequenceGap, why);
    const uint64_t expected = advanceReplayDigestV2(
        rollingDigest_, event);
    if (event.rollingDigest != expected)
        return fail(ConfirmedTimelineFaultV2::RollingDigestMismatch, why);
    if (event.kind == GameEventKindV2::GameResult) {
        if (sawGameResult_)
            return fail(ConfirmedTimelineFaultV2::BadEvent, why);
        sawGameResult_ = true;
        gameResult_ = event;
    }
    if (!canonical_.append(event))
        return fail(ConfirmedTimelineFaultV2::CanonicalDigestMismatch, why);
    rollingDigest_ = expected;
    ++nextEventSequence_;
    return true;
}

bool ConfirmedTimelineValidatorV2::append(
    const CanonicalTickBundleV2& tick, ParseStatus& why) {
    why = ParseStatus::Ok;
    if (!started_)
        return fail(ConfirmedTimelineFaultV2::NotStarted, why);
    if (failed_) {
        why = confirmedTimelineFaultStatusV2(lastFault_);
        return false;
    }
    if (sealed_) return fail(ConfirmedTimelineFaultV2::AfterEnd, why);
    if (sawGameResult_)
        return fail(
            ConfirmedTimelineFaultV2::CompletedBoundaryMismatch, why);
    if (tick.preEvents.count > kMaxEventsPerTickV2
        || tick.postEvents.count > kMaxEventsPerTickV2) {
        return fail(ConfirmedTimelineFaultV2::BadEvent, why);
    }
    const ConfirmedGameFrameV2& frame = tick.frame;
    if (frame.replayFrame != nextRecord_)
        return fail(ConfirmedTimelineFaultV2::FrameGap, why);
    if (!isValidConfirmedGameFrameCoreV2(
            frame.core, start_.manifest.activeLaneMask)
        || frame.core.activeLaneMask != start_.manifest.activeLaneMask) {
        return fail(ConfirmedTimelineFaultV2::BadFrame, why);
    }

    if (nextRecord_ == 0u) {
        if (frame.core.before.scene != 2u
            || frame.core.before.matchSub != 0
            || frame.core.preStateHash
                != start_.start.frameZeroPreStateHash
            || tick.preEvents.count < 2u
            || tick.preEvents.events[0].kind
                != GameEventKindV2::GameStarted
            || tick.preEvents.events[0].eventSequence != 0u
            || tick.preEvents.events[1].kind
                != GameEventKindV2::RoundStarted
            || tick.preEvents.events[1].roundOrdinal != 1u
            || tick.preEvents.events[1].eventSequence != 1u) {
            return fail(
                ConfirmedTimelineFaultV2::MissingInitialAssertions, why);
        }
    } else {
        if (lastFrame_.sourceFrame
                == std::numeric_limits<uint32_t>::max()
            || frame.sourceFrame != lastFrame_.sourceFrame + 1u) {
            return fail(ConfirmedTimelineFaultV2::SourceFrameGap, why);
        }
        if (frame.core.before != lastFrame_.core.after)
            return fail(
                ConfirmedTimelineFaultV2::LifecycleDiscontinuity, why);
        if (frame.core.preStateHash != lastFrame_.core.postStateHash)
            return fail(
                ConfirmedTimelineFaultV2::StateProofDiscontinuity, why);
    }

    for (size_t i = 0u; i < tick.preEvents.count; ++i) {
        if (tick.preEvents.events[i].stateProof
            != frame.core.preStateHash) {
            return fail(
                ConfirmedTimelineFaultV2::StateProofDiscontinuity, why);
        }
        if (!appendEvent(
                tick.preEvents.events[i], GameEventPhaseV2::PreFrame,
                frame.replayFrame, why)) {
            return false;
        }
    }

    const uint64_t expected =
        advanceReplayDigestV2(rollingDigest_, frame);
    if (frame.rollingDigest != expected)
        return fail(ConfirmedTimelineFaultV2::RollingDigestMismatch, why);
    if (!canonical_.append(frame))
        return fail(ConfirmedTimelineFaultV2::CanonicalDigestMismatch, why);
    rollingDigest_ = expected;

    for (size_t i = 0u; i < tick.postEvents.count; ++i) {
        if (tick.postEvents.events[i].stateProof
            != frame.core.postStateHash) {
            return fail(
                ConfirmedTimelineFaultV2::StateProofDiscontinuity, why);
        }
        if (!appendEvent(
                tick.postEvents.events[i], GameEventPhaseV2::PostFrame,
                frame.replayFrame, why)) {
            return false;
        }
    }

    if (hasLastFrame_) {
        previousFrame_ = lastFrame_;
        hasPreviousFrame_ = true;
    }
    lastFrame_ = frame;
    hasLastFrame_ = true;
    ++nextRecord_;
    lastFault_ = ConfirmedTimelineFaultV2::None;
    return true;
}

bool ConfirmedTimelineValidatorV2::seal(
    const ConfirmedGameEndV2& end, ParseStatus& why) {
    why = ParseStatus::Ok;
    if (!started_)
        return fail(ConfirmedTimelineFaultV2::NotStarted, why);
    if (failed_) {
        why = confirmedTimelineFaultStatusV2(lastFault_);
        return false;
    }
    if (sealed_) return fail(ConfirmedTimelineFaultV2::AfterEnd, why);
    if (!hasLastFrame_ || !isValidConfirmedGameEndV2(end)
        || end.nextRecord != nextRecord_
        || end.finalRollingDigest != rollingDigest_
        || end.postTailStateHash != lastFrame_.core.postStateHash) {
        return fail(ConfirmedTimelineFaultV2::TerminalMismatch, why);
    }

    if (end.reason == TerminalReason::Completed) {
        if (!hasPreviousFrame_ || !sawGameResult_
            || gameResult_.replayFrame + 1u != end.nextRecord
            || gameResult_.winningSideMask != end.winningSideMask
            || gameResult_.roundTallies != end.roundTallies
            || gameResult_.winnerSlot != end.winnerSlot
            || previousFrame_.core.before.scene != 2u
            || previousFrame_.core.before.matchSub != 1
            || previousFrame_.core.after.scene != 2u
            || previousFrame_.core.after.matchSub != 2
            || previousFrame_.core.after.roundCounter != 5u
            || lastFrame_.core.before.scene != 2u
            || lastFrame_.core.before.matchSub != 2
            || lastFrame_.core.before.resultCounter != 0u
            || lastFrame_.core.before.winnerSlot != end.winnerSlot
            || lastFrame_.core.before.roundTallies != end.roundTallies
            || previousFrame_.sourceFrame + 1u
                != lastFrame_.sourceFrame) {
            return fail(
                ConfirmedTimelineFaultV2::CompletedBoundaryMismatch, why);
        }
    } else if (sawGameResult_) {
        return fail(
            ConfirmedTimelineFaultV2::CompletedBoundaryMismatch, why);
    }

    std::array<uint8_t, 32> canonical{};
    if (!canonical_.finish(end, canonical)
        || canonical != end.canonicalGameDigest) {
        return fail(
            ConfirmedTimelineFaultV2::CanonicalDigestMismatch, why);
    }
    end_ = end;
    sealed_ = true;
    lastFault_ = ConfirmedTimelineFaultV2::None;
    return true;
}

ConfirmedTimelineBufferV2::ConfirmedTimelineBufferV2(
    size_t capacityFrames) {
    if (capacityFrames == 0u
        || capacityFrames > kMaxPlaybackPrebufferFramesV2) {
        capacity_ = 0u;
        return;
    }
    capacity_ = capacityFrames;
    ticks_.reserve(capacity_);
}

bool ConfirmedTimelineBufferV2::begin(
    const ConfirmedGameStartV2& start, ParseStatus& why) {
    if (capacity_ == 0u) {
        why = ParseStatus::LimitExceeded;
        return false;
    }
    if (validator_.started()) {
        why = ParseStatus::SequenceRegression;
        return false;
    }
    ConfirmedTimelineValidatorV2 candidate;
    if (!candidate.begin(start, why)) return false;
    ticks_.clear();
    validator_ = candidate;
    return true;
}

bool ConfirmedTimelineBufferV2::append(
    const CanonicalTickBundleV2& tick, ParseStatus& why) {
    if (tick.frame.replayFrame < ticks_.size()) {
        if (ticks_[tick.frame.replayFrame] == tick) {
            why = ParseStatus::Ok;
            return true;
        }
        why = ParseStatus::FrameConflict;
        return false;
    }
    if (tick.frame.replayFrame != ticks_.size()) {
        why = ParseStatus::FrameGap;
        return false;
    }
    if (ticks_.size() >= capacity_) {
        why = ParseStatus::LimitExceeded;
        return false;
    }
    ConfirmedTimelineValidatorV2 candidate = validator_;
    if (!candidate.append(tick, why)) return false;
    ticks_.push_back(tick);
    validator_ = candidate;
    return true;
}

bool ConfirmedTimelineBufferV2::seal(
    const ConfirmedGameEndV2& end, ParseStatus& why) {
    if (validator_.sealed()) {
        if (validator_.end() == end) {
            why = ParseStatus::Ok;
            return true;
        }
        why = ParseStatus::SequenceRegression;
        return false;
    }
    ConfirmedTimelineValidatorV2 candidate = validator_;
    if (!candidate.seal(end, why)) return false;
    validator_ = candidate;
    return true;
}

const CanonicalTickBundleV2* ConfirmedTimelineBufferV2::tick(
    uint32_t replayFrame) const {
    if (replayFrame >= ticks_.size()) return nullptr;
    return &ticks_[replayFrame];
}

}
