#pragma once

#include "ConfirmedTimeline.h"
#include "GameEpochTracker.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace qreplay {

constexpr size_t kMaxPendingReplayCommitsV2 = 8u;

enum class ReplayGameRecordStateV2 : uint8_t {
    Disabled = 0,
    WaitingForFreshGame,
    WaitingForFrameZero,
    Recording,
    AwaitFirstWinScreenTick,
    Failed,
};

enum class ReplayRecordingFaultV2 : uint8_t {
    None = 0,
    InvalidFreshGame,
    UnsupportedTopology,
    ConflictingFreshGame,
    CommitBacklog,
    WrongGameIdentity,
    NonContiguousSource,
    InvalidFrame,
    InvalidRoundTransition,
    MissingInitialSetup,
    FinalWinScreenPredicateMismatch,
    TimelineValidationFailure,
    PublisherOverflow,
    ReservedEndFailure,
    InvalidTerminalReason,
    CommitAckStale,
    CommitAckConflict,
};

enum class ReplayPublishCodeV2 : uint8_t {
    Ignored = 0,
    Armed,
    Published,
    TerminalPublished,
    NoFile,
    Unsupported,
    Failed,
};

struct ReplayPublishResultV2 {
    ReplayPublishCodeV2 code = ReplayPublishCodeV2::Ignored;
    ReplayRecordingFaultV2 fault = ReplayRecordingFaultV2::None;
};

enum class ReplayCloseProvenanceV2 : uint8_t {
    PeerOrderly = 1,
    LocalOrderly = 2,
    ProcessObserved = 3,
    RecorderChannel = 4,
};

enum class CommitAckResultV2 : uint8_t {
    Applied = 1,
    ExactDuplicate = 2,
    Stale = 3,
    Conflict = 4,
};

struct ProvenGameFrameV2 {
    uint32_t sourceFrame = 0u;
    uint32_t gameEpoch = 0u;
    GameIdentityV2 game{};
    ConfirmedGameFrameCoreV2 core{};
    RoundTransitionProofV2 roundTransition{};
    uint8_t activeFighterMask = 0u;
    std::array<FighterIdentityV2, kFighterSlots> roster{};
    StageIdentityV2 stage{};
};

class ReplayRecordPublisherV2 {
public:
    virtual ~ReplayRecordPublisherV2() = default;
    virtual bool tryPublishOrdinary(
        const PublisherRecordV2& record) noexcept = 0;
    virtual bool publishEndGameReserved(
        const PublisherRecordV2& record) noexcept = 0;
};

const char* replayRecordingFaultTextV2(ReplayRecordingFaultV2 fault);

class ReplayRecordingCoordinatorV2 {
public:
    explicit ReplayRecordingCoordinatorV2(
        ReplayRecordPublisherV2& publisher);

    void reset(bool enabled);

    ReplayPublishResultV2 armFreshGame(
        const FreshGameDescriptorV2& descriptor);
    ReplayPublishResultV2 acceptConfirmedFrame(
        const ProvenGameFrameV2& source);
    ReplayPublishResultV2 acceptCommittedFrame(
        const ProvenGameFrameV2& source) {
        return acceptConfirmedFrame(source);
    }
    bool publishPalette(const ReplayPaletteV2& palette);

    ReplayPublishResultV2 requestAbnormalEnd(
        TerminalReason reason, ReplayCloseProvenanceV2 provenance);
    CommitAckResultV2 onCasterCommitAck(const GameCommitAckV2& ack);

    ReplayGameRecordStateV2 state() const { return state_; }
    ReplayRecordingFaultV2 lastFault() const { return lastFault_; }
    ConfirmedTimelineFaultV2 lastTimelineFault() const {
        return lastTimelineFault_;
    }
    const GameLifecycleV2& timelineExpectedLifecycle() const {
        return timelineExpectedLifecycle_;
    }
    const GameLifecycleV2& timelineObservedLifecycle() const {
        return timelineObservedLifecycle_;
    }
    uint64_t timelineExpectedStateProof() const {
        return timelineExpectedStateProof_;
    }
    uint64_t timelineObservedStateProof() const {
        return timelineObservedStateProof_;
    }
    uint32_t nextReplayFrame() const {
        return validator_.started() ? validator_.nextRecord() : 0u;
    }
    size_t pendingCommitCount() const;
    const GameIdentityV2& currentGameIdentity() const {
        return current_.start.manifest.identity;
    }

private:
    struct FinalLatch {
        bool active = false;
        uint32_t expectedSourceFrame = 0u;
        uint32_t gameEpoch = 0u;
        uint32_t transitionSiteId = 0u;
        uint8_t roundOrdinal = 0u;
        uint8_t winningSideMask = 0u;
        int32_t winnerSlot = -1;
        std::array<uint8_t, 2> roundTallies{};
    };

    struct ReplayGameCapture {
        FreshGameDescriptorV2 descriptor{};
        ConfirmedGameStartV2 start{};
        ProvenGameFrameV2 lastAccepted{};
        GameLifecycleV2 expectedFrameZeroBefore{};
        uint32_t expectedFrameZeroSource = 0u;
        uint32_t initialTransitionSiteId = 0u;
        uint8_t activeRoundOrdinal = 0u;
        bool initialSetupSeen = false;
        bool hasAcceptedFrame = false;
        bool roundEndedForActive = false;
        bool pendingRoundStarted = false;
        uint8_t pendingRoundOrdinal = 0u;
        uint32_t pendingRoundTransitionSiteId = 0u;
        FinalLatch finalLatch{};
    };

    struct PendingCommitEntry {
        bool occupied = false;
        bool resolved = false;
        GameIdentityV2 identity{};
        uint32_t nextRecord = 0u;
        std::array<uint8_t, 32> canonicalGameDigest{};
        GameCommitAckV2 ack{};
        uint64_t serial = 0u;
    };

    ReplayPublishResultV2 acceptWaitingForFrameZero(
        const ProvenGameFrameV2& source);
    ReplayPublishResultV2 acceptRecording(
        const ProvenGameFrameV2& source);
    ReplayPublishResultV2 acceptFirstWinScreen(
        const ProvenGameFrameV2& source);

    ReplayPublishResultV2 failWithoutFile(
        ReplayRecordingFaultV2 fault);
    ReplayPublishResultV2 failCurrent(
        ReplayRecordingFaultV2 fault, TerminalReason reason);
    ReplayPublishResultV2 publishAbnormalEnd(
        TerminalReason reason, ReplayCloseProvenanceV2 provenance,
        bool enterFailedState, ReplayRecordingFaultV2 fault);

    bool topologyQualified(
        const FreshGameDescriptorV2& descriptor) const;
    bool validateProvenFrame(
        const ProvenGameFrameV2& source) const;
    bool validateRoundProof(
        const ProvenGameFrameV2& source) const;
    bool isInitialSetup(
        const ProvenGameFrameV2& source) const;
    bool isFirstActiveFrame(
        const ProvenGameFrameV2& source) const;
    bool isFinalOneToTwoTransition(
        const ProvenGameFrameV2& source) const;
    bool matchesFirstWinScreenTick(
        const ProvenGameFrameV2& source) const;

    ConfirmedGameEventV2 makeEvent(
        GameEventKindV2 kind, GameEventPhaseV2 phase,
        uint32_t replayFrame, uint32_t transitionSiteId,
        uint8_t roundOrdinal, uint8_t winningSideMask,
        const std::array<uint8_t, 2>& roundTallies,
        int32_t winnerSlot, uint64_t stateProof) const;
    bool stampAndValidateTick(
        CanonicalTickBundleV2& tick,
        ConfirmedTimelineValidatorV2& validator,
        CanonicalGameDigestV2& canonical);
    bool finalizeEnd(
        ConfirmedGameEndV2& end,
        ConfirmedTimelineValidatorV2& validator,
        CanonicalGameDigestV2& canonical) const;
    bool retireCurrent(const ConfirmedGameEndV2& end);
    bool commitSlotAvailable() const;
    void clearCurrent();

    ReplayRecordPublisherV2& publisher_;
    ReplayGameRecordStateV2 state_ =
        ReplayGameRecordStateV2::Disabled;
    ReplayRecordingFaultV2 lastFault_ =
        ReplayRecordingFaultV2::None;
    ConfirmedTimelineFaultV2 lastTimelineFault_ =
        ConfirmedTimelineFaultV2::None;
    GameLifecycleV2 timelineExpectedLifecycle_{};
    GameLifecycleV2 timelineObservedLifecycle_{};
    uint64_t timelineExpectedStateProof_ = 0u;
    uint64_t timelineObservedStateProof_ = 0u;
    ReplayGameCapture current_{};
    ConfirmedTimelineValidatorV2 validator_{};
    CanonicalGameDigestV2 canonical_{};
    std::array<PendingCommitEntry, kMaxPendingReplayCommitsV2>
        pendingCommits_{};
    uint64_t commitSerial_ = 0u;
};

}
