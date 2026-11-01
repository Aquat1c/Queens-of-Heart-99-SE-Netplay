#pragma once

#include "ReplayIntegrity.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qreplay {

enum class ConfirmedTimelineFaultV2 : uint8_t {
    None = 0,
    AlreadyStarted,
    BadStart,
    NotStarted,
    AfterEnd,
    FrameGap,
    SourceFrameGap,
    ConflictingDuplicate,
    CapacityExceeded,
    BadFrame,
    BadEvent,
    EventSequenceGap,
    EventPhaseMismatch,
    LifecycleDiscontinuity,
    StateProofDiscontinuity,
    RollingDigestMismatch,
    MissingInitialAssertions,
    TerminalMismatch,
    CompletedBoundaryMismatch,
    CanonicalDigestMismatch,
};

const char* confirmedTimelineFaultTextV2(ConfirmedTimelineFaultV2 fault);
ParseStatus confirmedTimelineFaultStatusV2(ConfirmedTimelineFaultV2 fault);

class ConfirmedTimelineValidatorV2 {
public:
    bool begin(
        const ConfirmedGameStartV2& start, ParseStatus& why);
    bool append(
        const CanonicalTickBundleV2& tick, ParseStatus& why);
    bool seal(const ConfirmedGameEndV2& end, ParseStatus& why);

    bool started() const { return started_; }
    bool sealed() const { return sealed_; }
    bool failed() const { return failed_; }
    uint32_t nextRecord() const { return nextRecord_; }
    uint32_t nextEventSequence() const { return nextEventSequence_; }
    uint64_t rollingDigest() const { return rollingDigest_; }
    ConfirmedTimelineFaultV2 lastFault() const { return lastFault_; }
    const ConfirmedGameStartV2& start() const { return start_; }
    const ConfirmedGameEndV2& end() const { return end_; }

private:
    bool fail(ConfirmedTimelineFaultV2 fault, ParseStatus& why);
    bool appendEvent(
        const ConfirmedGameEventV2& event, GameEventPhaseV2 phase,
        uint32_t frame, ParseStatus& why);

    ConfirmedGameStartV2 start_{};
    ConfirmedGameEndV2 end_{};
    CanonicalGameDigestV2 canonical_{};
    ConfirmedGameFrameV2 previousFrame_{};
    ConfirmedGameFrameV2 lastFrame_{};
    ConfirmedGameEventV2 gameResult_{};

    bool started_ = false;
    bool sealed_ = false;
    bool failed_ = false;
    bool hasPreviousFrame_ = false;
    bool hasLastFrame_ = false;
    bool sawGameResult_ = false;
    uint32_t nextRecord_ = 0u;
    uint32_t nextEventSequence_ = 0u;
    uint64_t rollingDigest_ = 0u;
    ConfirmedTimelineFaultV2 lastFault_ =
        ConfirmedTimelineFaultV2::None;
};

class ConfirmedTimelineBufferV2 {
public:
    explicit ConfirmedTimelineBufferV2(size_t capacityFrames);

    bool begin(
        const ConfirmedGameStartV2& start, ParseStatus& why);
    bool append(
        const CanonicalTickBundleV2& tick, ParseStatus& why);
    bool seal(const ConfirmedGameEndV2& end, ParseStatus& why);

    bool started() const { return validator_.started(); }
    bool sealed() const { return validator_.sealed(); }
    bool validCapacity() const { return capacity_ != 0u; }
    size_t capacity() const { return capacity_; }
    size_t size() const { return ticks_.size(); }
    const ConfirmedGameStartV2& start() const { return validator_.start(); }
    const CanonicalTickBundleV2* tick(uint32_t replayFrame) const;
    const ConfirmedGameEndV2& end() const { return validator_.end(); }
    const ConfirmedTimelineValidatorV2& validator() const {
        return validator_;
    }

private:
    size_t capacity_ = 1u;
    ConfirmedTimelineValidatorV2 validator_{};
    std::vector<CanonicalTickBundleV2> ticks_;
};

}
