#pragma once

#include "ReplayTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qreplay {

struct DeterministicStartDescriptor {
    DeterministicStart start{};
    SimulationContract contract{};
    BootstrapKind bootstrapKind = BootstrapKind::OfflineVsDirectBootV1;
    uint8_t vsRow = 1u;
    uint8_t cpuMask = 0u;
    // Native four-entry fighter-slot -> logical-source route (dword_4A37C4).
    std::array<uint8_t, kFighterSlots> slotToSourceRoute{{0u, 1u, 2u, 3u}};
    uint8_t activeLaneMask = 0x03u;
    uint8_t activeSlotMask = 0x03u;
    uint64_t bootstrapFingerprint = 0u;
};

struct TimelineTerminal {
    bool present = false;
    uint32_t nextRecord = 0u;
    uint64_t finalDigest = 0u;
    uint64_t postTailStateHash = 0u;
    TerminalReason reason = TerminalReason::Completed;
};

enum class TimelineFault : uint8_t {
    None = 0,
    NotStarted,
    BadStartDescriptor,
    AfterTerminal,
    FrameGap,
    FrameConflict,
    DuplicateBelowRetention,
    Overflow,
    FrameLimitExceeded,
    BadActionMask,
    BadLaneShape,
    UnknownInputDomain,
    DigestChainBreak,
    PreStateHashMismatch,
    PostStateMismatch,
    NoRecordAvailable,
    RewindBelowRetention,
    EventOutOfOrder,
    BadEventPayload,
    EventOverflow,
    TerminalMissing,
    TerminalFrameMismatch,
    TerminalNotDrained,
    TerminalDigestMismatch,
    TerminalStateHashMismatch,
};

const char* timelineFaultText(TimelineFault fault);

ParseStatus timelineFaultStatus(TimelineFault fault);

class ConfirmedTimelineBuffer {
public:
    explicit ConfirmedTimelineBuffer(size_t capacityFrames);

    void reset(const DeterministicStartDescriptor& start);

    bool started() const { return started_; }

    bool append(const ReplayFrameV1& frame, ParseStatus& why);

    bool appendEvent(const ReplayEvent& event, ParseStatus& why);

    void setTerminal(const TimelineTerminal& terminal);

    bool peek(ReplayFrameV1& out) const;

    bool consume(uint64_t observedPreStateHash, ParseStatus& why);

    void recordPostState(uint32_t afterFrame, uint64_t postHash);

    bool verifyTerminal(ParseStatus& why) const;

    bool peekEvent(ReplayEvent& out) const;
    bool consumeEvent(ReplayEvent& out);
    size_t pendingEvents() const;
    uint32_t eventCursor() const { return eventCursor_; }

    void trimConsumedBelow(uint32_t frame);

    bool rewindConsumeTo(uint32_t frame, ParseStatus& why);

    size_t bufferedFrames() const;
    size_t retainedFrames() const;
    size_t capacity() const { return capacity_; }
    size_t eventCapacity() const { return eventCapacity_; }

    uint32_t firstFrame() const { return firstFrame_; }
    uint32_t nextFrame() const { return nextFrame_; }
    uint32_t consumeCursor() const { return consumeFrame_; }
    uint32_t retentionFloor() const { return retentionFloor_; }
    bool lastConsumedFrame(uint32_t& out) const;

    uint64_t rollingDigest() const { return rollingDigest_; }

    const DeterministicStartDescriptor& start() const { return start_; }
    const TimelineTerminal& terminal() const { return terminal_; }

    TimelineFault lastFault() const { return lastFault_; }
    bool postStateConsistent() const { return !postMismatch_; }

private:
    bool fail(TimelineFault fault, ParseStatus& why) const;
    bool validateShape(const ReplayFrameV1& frame, ParseStatus& why) const;
    const ReplayFrameV1* retained(uint32_t frame) const;
    ReplayFrameV1& slot(uint32_t frame);

    std::vector<ReplayFrameV1> ring_;
    std::vector<ReplayEvent> events_;
    size_t capacity_ = 1u;
    size_t eventCapacity_ = 1u;

    DeterministicStartDescriptor start_{};
    TimelineTerminal terminal_{};

    bool started_ = false;
    bool startValid_ = false;

    uint32_t firstFrame_ = 0u;
    uint32_t retentionFloor_ = 0u;
    uint32_t consumeFrame_ = 0u;
    uint32_t nextFrame_ = 0u;

    uint64_t rollingDigest_ = 0u;

    size_t eventRead_ = 0u;
    uint32_t eventCursor_ = 0u;
    uint32_t lastEventFrame_ = 0u;
    bool hasEvent_ = false;

    bool hasPost_ = false;
    uint32_t lastPostFrame_ = 0u;
    uint64_t lastPostHash_ = 0u;
    bool postMismatch_ = false;

    mutable TimelineFault lastFault_ = TimelineFault::None;
};

ReplayFrameV1 makeFrameFromConfirmedPair(uint32_t timelineFrame, uint16_t p1,
                                         uint16_t p2, uint64_t preHash,
                                         uint64_t rollingDigest);

uint64_t sealFrameDigest(uint64_t previous, ReplayFrameV1& frame);
uint64_t sealEventDigest(uint64_t previous, ReplayEvent& event);

}
