#include "ReplayTimeline.h"

#include "ReplayIntegrity.h"

namespace qreplay {

const char* timelineFaultText(TimelineFault fault) {
    switch (fault) {
        case TimelineFault::None: return "none";
        case TimelineFault::NotStarted: return "timeline not started";
        case TimelineFault::BadStartDescriptor: return "bad start descriptor";
        case TimelineFault::AfterTerminal: return "record after terminal";
        case TimelineFault::FrameGap: return "frame gap";
        case TimelineFault::FrameConflict: return "conflicting duplicate frame";
        case TimelineFault::DuplicateBelowRetention:
            return "duplicate below retention floor";
        case TimelineFault::Overflow: return "timeline buffer overflow";
        case TimelineFault::FrameLimitExceeded: return "timeline frame limit";
        case TimelineFault::BadActionMask: return "action outside 0x0F0F";
        case TimelineFault::BadLaneShape: return "lane shape mismatch";
        case TimelineFault::UnknownInputDomain: return "unknown input domain";
        case TimelineFault::DigestChainBreak: return "rolling digest chain break";
        case TimelineFault::PreStateHashMismatch: return "pre-state hash mismatch";
        case TimelineFault::PostStateMismatch: return "post-state hash mismatch";
        case TimelineFault::NoRecordAvailable: return "no buffered record";
        case TimelineFault::RewindBelowRetention:
            return "rewind below retention floor";
        case TimelineFault::EventOutOfOrder: return "event out of order";
        case TimelineFault::BadEventPayload: return "bad event payload";
        case TimelineFault::EventOverflow: return "event buffer overflow";
        case TimelineFault::TerminalMissing: return "terminal missing";
        case TimelineFault::TerminalFrameMismatch: return "terminal frame mismatch";
        case TimelineFault::TerminalNotDrained: return "terminal before drain";
        case TimelineFault::TerminalDigestMismatch: return "terminal digest mismatch";
        case TimelineFault::TerminalStateHashMismatch:
            return "terminal post-tail state mismatch";
    }
    return "unknown";
}

ParseStatus timelineFaultStatus(TimelineFault fault) {
    switch (fault) {
        case TimelineFault::None: return ParseStatus::Ok;
        case TimelineFault::NotStarted: return ParseStatus::MissingSessionStart;
        case TimelineFault::BadStartDescriptor: return ParseStatus::BadLaneShape;
        case TimelineFault::AfterTerminal: return ParseStatus::SequenceRegression;
        case TimelineFault::FrameGap: return ParseStatus::FrameGap;
        case TimelineFault::FrameConflict: return ParseStatus::FrameConflict;
        case TimelineFault::DuplicateBelowRetention:
            return ParseStatus::SequenceRegression;
        case TimelineFault::Overflow: return ParseStatus::LimitExceeded;
        case TimelineFault::FrameLimitExceeded: return ParseStatus::LimitExceeded;
        case TimelineFault::BadActionMask: return ParseStatus::BadActionMask;
        case TimelineFault::BadLaneShape: return ParseStatus::BadLaneShape;
        case TimelineFault::UnknownInputDomain: return ParseStatus::UnsupportedFeature;
        case TimelineFault::DigestChainBreak: return ParseStatus::DigestMismatch;
        case TimelineFault::PreStateHashMismatch: return ParseStatus::DigestMismatch;
        case TimelineFault::PostStateMismatch: return ParseStatus::DigestMismatch;
        case TimelineFault::NoRecordAvailable: return ParseStatus::Truncated;
        case TimelineFault::RewindBelowRetention:
            return ParseStatus::SequenceRegression;
        case TimelineFault::EventOutOfOrder: return ParseStatus::SequenceRegression;
        case TimelineFault::BadEventPayload: return ParseStatus::BadEventPayload;
        case TimelineFault::EventOverflow: return ParseStatus::LimitExceeded;
        case TimelineFault::TerminalMissing: return ParseStatus::MissingFooter;
        case TimelineFault::TerminalFrameMismatch: return ParseStatus::FrameGap;
        case TimelineFault::TerminalNotDrained: return ParseStatus::Truncated;
        case TimelineFault::TerminalDigestMismatch: return ParseStatus::DigestMismatch;
        case TimelineFault::TerminalStateHashMismatch:
            return ParseStatus::DigestMismatch;
    }
    return ParseStatus::IoError;
}

ConfirmedTimelineBuffer::ConfirmedTimelineBuffer(size_t capacityFrames) {
    capacity_ = capacityFrames == 0u ? 1u : capacityFrames;
    if (capacity_ > kMaxTimelineFrames) capacity_ = kMaxTimelineFrames;
    ring_.resize(capacity_);
    eventCapacity_ = capacity_ < 64u ? 64u : capacity_;
    if (eventCapacity_ > 65536u) eventCapacity_ = 65536u;
    events_.reserve(eventCapacity_);
}

void ConfirmedTimelineBuffer::reset(const DeterministicStartDescriptor& start) {
    start_ = start;
    terminal_ = TimelineTerminal{};
    started_ = true;
    startValid_ =
        (start.activeLaneMask & static_cast<uint8_t>(~0x0Fu)) == 0u
        && (start.activeSlotMask & static_cast<uint8_t>(~0x0Fu)) == 0u
        && start.start.firstTimelineFrame < kMaxTimelineFrames;
    for (size_t i = 0; i < kFighterSlots; ++i) {
        if (start.slotToSourceRoute[i] >= kSourceLanes) startValid_ = false;
    }

    firstFrame_ = start.start.firstTimelineFrame;
    retentionFloor_ = firstFrame_;
    consumeFrame_ = firstFrame_;
    nextFrame_ = firstFrame_;
    rollingDigest_ = start.start.initialDigest;

    events_.clear();
    eventRead_ = 0u;
    eventCursor_ = 0u;
    lastEventFrame_ = firstFrame_;
    hasEvent_ = false;

    hasPost_ = false;
    lastPostFrame_ = 0u;
    lastPostHash_ = 0u;
    postMismatch_ = false;

    lastFault_ = startValid_ ? TimelineFault::None
                             : TimelineFault::BadStartDescriptor;
}

bool ConfirmedTimelineBuffer::fail(TimelineFault fault, ParseStatus& why) const {
    lastFault_ = fault;
    why = timelineFaultStatus(fault);
    return false;
}

ReplayFrameV1& ConfirmedTimelineBuffer::slot(uint32_t frame) {
    return ring_[static_cast<size_t>(frame % static_cast<uint32_t>(capacity_))];
}

const ReplayFrameV1* ConfirmedTimelineBuffer::retained(uint32_t frame) const {
    if (frame < retentionFloor_ || frame >= nextFrame_) return nullptr;
    return &ring_[static_cast<size_t>(frame % static_cast<uint32_t>(capacity_))];
}

bool ConfirmedTimelineBuffer::validateShape(const ReplayFrameV1& frame,
                                            ParseStatus& why) const {
    const uint8_t domain = static_cast<uint8_t>(frame.inputDomain);
    if (domain != static_cast<uint8_t>(InputDomain::LogicalSourceV1)
        && domain != static_cast<uint8_t>(InputDomain::EffectiveFighterV1)) {
        return fail(TimelineFault::UnknownInputDomain, why);
    }
    if ((frame.activeLaneMask & static_cast<uint8_t>(~0x0Fu)) != 0u) {
        return fail(TimelineFault::BadLaneShape, why);
    }
    if ((frame.activeLaneMask & static_cast<uint8_t>(~start_.activeLaneMask))
        != 0u) {
        return fail(TimelineFault::BadLaneShape, why);
    }
    for (size_t lane = 0; lane < kSourceLanes; ++lane) {
        const bool active =
            (frame.activeLaneMask & static_cast<uint8_t>(1u << lane)) != 0u;
        const uint16_t action = frame.actions[lane];
        if (!active) {
            if (action != 0u) return fail(TimelineFault::BadLaneShape, why);
            continue;
        }
        if ((action & static_cast<uint16_t>(~kActionMask)) != 0u) {
            return fail(TimelineFault::BadActionMask, why);
        }
    }
    return true;
}

bool ConfirmedTimelineBuffer::append(const ReplayFrameV1& frame,
                                     ParseStatus& why) {
    why = ParseStatus::Ok;
    if (!started_) return fail(TimelineFault::NotStarted, why);
    if (!startValid_) return fail(TimelineFault::BadStartDescriptor, why);
    if (!validateShape(frame, why)) return false;

    if (frame.timelineFrame < nextFrame_) {
        const ReplayFrameV1* held = retained(frame.timelineFrame);
        if (held == nullptr) {
            return fail(TimelineFault::DuplicateBelowRetention, why);
        }
        if (*held != frame) return fail(TimelineFault::FrameConflict, why);
        lastFault_ = TimelineFault::None;
        return true;
    }
    if (frame.timelineFrame > nextFrame_) {
        return fail(TimelineFault::FrameGap, why);
    }
    if (terminal_.present) return fail(TimelineFault::AfterTerminal, why);
    if (frame.timelineFrame >= kMaxTimelineFrames) {
        return fail(TimelineFault::FrameLimitExceeded, why);
    }
    if (retainedFrames() >= capacity_) return fail(TimelineFault::Overflow, why);

    const uint64_t expected = advanceReplayDigest(rollingDigest_, frame);
    if (expected != frame.rollingDigest) {
        return fail(TimelineFault::DigestChainBreak, why);
    }

    slot(frame.timelineFrame) = frame;
    rollingDigest_ = expected;
    ++nextFrame_;
    lastFault_ = TimelineFault::None;
    return true;
}

bool ConfirmedTimelineBuffer::appendEvent(const ReplayEvent& event,
                                          ParseStatus& why) {
    why = ParseStatus::Ok;
    if (!started_) return fail(TimelineFault::NotStarted, why);
    if (!startValid_) return fail(TimelineFault::BadStartDescriptor, why);
    if (terminal_.present) return fail(TimelineFault::AfterTerminal, why);
    if (!isKnownEventKind(static_cast<uint16_t>(event.kind))) {
        return fail(TimelineFault::BadEventPayload, why);
    }
    if (!isKnownTerminalReason(static_cast<uint16_t>(event.terminalReason))) {
        return fail(TimelineFault::BadEventPayload, why);
    }
    if ((event.selectorProofMask & static_cast<uint8_t>(~0x0Fu)) != 0u) {
        return fail(TimelineFault::BadEventPayload, why);
    }
    if (event.timelineFrame > nextFrame_ || event.timelineFrame < firstFrame_) {
        return fail(TimelineFault::EventOutOfOrder, why);
    }
    if (hasEvent_ && event.timelineFrame < lastEventFrame_) {
        return fail(TimelineFault::EventOutOfOrder, why);
    }
    if (events_.size() >= eventCapacity_) {
        return fail(TimelineFault::EventOverflow, why);
    }

    const uint64_t expected = advanceReplayDigest(rollingDigest_, event);
    if (expected != event.rollingDigest) {
        return fail(TimelineFault::DigestChainBreak, why);
    }

    events_.push_back(event);
    rollingDigest_ = expected;
    lastEventFrame_ = event.timelineFrame;
    hasEvent_ = true;
    lastFault_ = TimelineFault::None;
    return true;
}

void ConfirmedTimelineBuffer::setTerminal(const TimelineTerminal& terminal) {
    terminal_ = terminal;
    terminal_.present = true;
}

bool ConfirmedTimelineBuffer::peek(ReplayFrameV1& out) const {
    if (consumeFrame_ >= nextFrame_) return false;
    const ReplayFrameV1* held = retained(consumeFrame_);
    if (held == nullptr) return false;
    out = *held;
    return true;
}

bool ConfirmedTimelineBuffer::consume(uint64_t observedPreStateHash,
                                      ParseStatus& why) {
    why = ParseStatus::Ok;
    if (!started_) return fail(TimelineFault::NotStarted, why);
    if (consumeFrame_ >= nextFrame_) {
        return fail(TimelineFault::NoRecordAvailable, why);
    }
    const ReplayFrameV1* held = retained(consumeFrame_);
    if (held == nullptr) return fail(TimelineFault::NoRecordAvailable, why);
    if (held->preStateHash != observedPreStateHash) {
        return fail(TimelineFault::PreStateHashMismatch, why);
    }
    ++consumeFrame_;
    lastFault_ = TimelineFault::None;
    return true;
}

void ConfirmedTimelineBuffer::recordPostState(uint32_t afterFrame,
                                              uint64_t postHash) {
    hasPost_ = true;
    lastPostFrame_ = afterFrame;
    lastPostHash_ = postHash;
    const ReplayFrameV1* held = retained(afterFrame);
    if (held != nullptr && held->postStateHash != 0u
        && held->postStateHash != postHash) {
        postMismatch_ = true;
        lastFault_ = TimelineFault::PostStateMismatch;
    }
}

bool ConfirmedTimelineBuffer::verifyTerminal(ParseStatus& why) const {
    why = ParseStatus::Ok;
    if (!started_) return fail(TimelineFault::NotStarted, why);
    if (!terminal_.present) return fail(TimelineFault::TerminalMissing, why);
    if (terminal_.nextRecord != nextFrame_) {
        return fail(TimelineFault::TerminalFrameMismatch, why);
    }
    if (consumeFrame_ != nextFrame_) {
        return fail(TimelineFault::TerminalNotDrained, why);
    }
    if (terminal_.finalDigest != rollingDigest_) {
        return fail(TimelineFault::TerminalDigestMismatch, why);
    }
    if (postMismatch_) return fail(TimelineFault::PostStateMismatch, why);

    if (nextFrame_ == firstFrame_) {
        if (terminal_.postTailStateHash != start_.start.initialStateHash) {
            return fail(TimelineFault::TerminalStateHashMismatch, why);
        }
        lastFault_ = TimelineFault::None;
        return true;
    }
    if (!hasPost_ || lastPostFrame_ + 1u != nextFrame_) {
        return fail(TimelineFault::TerminalStateHashMismatch, why);
    }
    if (lastPostHash_ != terminal_.postTailStateHash) {
        return fail(TimelineFault::TerminalStateHashMismatch, why);
    }
    lastFault_ = TimelineFault::None;
    return true;
}

bool ConfirmedTimelineBuffer::peekEvent(ReplayEvent& out) const {
    if (eventRead_ >= events_.size()) return false;
    out = events_[eventRead_];
    return true;
}

bool ConfirmedTimelineBuffer::consumeEvent(ReplayEvent& out) {
    if (eventRead_ >= events_.size()) return false;
    out = events_[eventRead_];
    ++eventRead_;
    ++eventCursor_;
    return true;
}

size_t ConfirmedTimelineBuffer::pendingEvents() const {
    return events_.size() - eventRead_;
}

void ConfirmedTimelineBuffer::trimConsumedBelow(uint32_t frame) {
    uint32_t floor = frame;
    if (floor > consumeFrame_) floor = consumeFrame_;
    if (floor <= retentionFloor_) return;
    retentionFloor_ = floor;

    size_t drop = 0u;
    while (drop < eventRead_ && events_[drop].timelineFrame < retentionFloor_) {
        ++drop;
    }
    if (drop > 0u) {
        events_.erase(events_.begin(),
                      events_.begin() + static_cast<std::ptrdiff_t>(drop));
        eventRead_ -= drop;
    }
}

bool ConfirmedTimelineBuffer::rewindConsumeTo(uint32_t frame, ParseStatus& why) {
    why = ParseStatus::Ok;
    if (!started_) return fail(TimelineFault::NotStarted, why);
    if (frame > nextFrame_) return fail(TimelineFault::FrameGap, why);
    if (frame < retentionFloor_) {
        return fail(TimelineFault::RewindBelowRetention, why);
    }
    consumeFrame_ = frame;
    hasPost_ = false;
    lastPostFrame_ = 0u;
    lastPostHash_ = 0u;
    postMismatch_ = false;

    size_t read = 0u;
    while (read < events_.size() && events_[read].timelineFrame < frame) ++read;
    eventRead_ = read;
    eventCursor_ = static_cast<uint32_t>(read);

    lastFault_ = TimelineFault::None;
    return true;
}

size_t ConfirmedTimelineBuffer::bufferedFrames() const {
    return static_cast<size_t>(nextFrame_ - consumeFrame_);
}

size_t ConfirmedTimelineBuffer::retainedFrames() const {
    return static_cast<size_t>(nextFrame_ - retentionFloor_);
}

bool ConfirmedTimelineBuffer::lastConsumedFrame(uint32_t& out) const {
    if (consumeFrame_ <= firstFrame_) return false;
    out = consumeFrame_ - 1u;
    return true;
}

ReplayFrameV1 makeFrameFromConfirmedPair(uint32_t timelineFrame, uint16_t p1,
                                         uint16_t p2, uint64_t preHash,
                                         uint64_t rollingDigest) {
    ReplayFrameV1 frame{};
    frame.timelineFrame = timelineFrame;
    frame.inputDomain = InputDomain::LogicalSourceV1;
    frame.activeLaneMask = 0x03u;
    frame.actions[0] = p1;
    frame.actions[1] = p2;
    frame.actions[2] = 0u;
    frame.actions[3] = 0u;
    frame.preStateHash = preHash;
    frame.postStateHash = 0u;
    frame.rollingDigest = rollingDigest;
    return frame;
}

uint64_t sealFrameDigest(uint64_t previous, ReplayFrameV1& frame) {
    const uint64_t digest = advanceReplayDigest(previous, frame);
    frame.rollingDigest = digest;
    return digest;
}

uint64_t sealEventDigest(uint64_t previous, ReplayEvent& event) {
    const uint64_t digest = advanceReplayDigest(previous, event);
    event.rollingDigest = digest;
    return digest;
}

}
