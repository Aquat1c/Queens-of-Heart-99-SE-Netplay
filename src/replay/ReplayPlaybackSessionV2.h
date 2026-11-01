#pragma once

#include "ConfirmedTimeline.h"
#include "ReplayIpcWire.h"

#include <array>
#include <cstdint>

namespace qreplay {

enum class ReplayPlaybackPhaseV2 : uint8_t {
    Bound = 1,
    Ready,
    FrameInFlight,
    TerminalArmed,
    AwaitingFinalPresentation,
    ResultReady,
    Faulted,
};

enum class ReplayPlaybackFaultV2 : uint8_t {
    None = 0,
    InvalidPrebuffer,
    EndpointMismatch,
    RuntimeContractMismatch,
    StageIdentityMismatch,
    BootstrapPhaseMismatch,
    BootstrapFingerprintMismatch,
    BootstrapPreHashMismatch,
    BootstrapLifecycleMismatch,
    UnsupportedInputDomain,
    FrameNumberMismatch,
    FramePreHashMismatch,
    FrameLifecycleBeforeMismatch,
    FrameLaneMaskMismatch,
    FramePostHashMismatch,
    FrameLifecycleAfterMismatch,
    DispatcherResultMismatch,
    PresentationSerialMissing,
    PresentationAckTimeout,
    DeviceIsolationViolation,
    InvalidTransition,
    BrokerFailure,
};

enum class ReplayPlaybackProofRegionV2 : uint32_t {
    None = 0,
    Prebuffer = 1,
    Endpoint = 2,
    RuntimeContract = 3,
    StageIdentity = 4,
    BootstrapPhase = 5,
    BootstrapFingerprint = 6,
    BootstrapPreState = 7,
    BootstrapLifecycle = 8,
    InputDomain = 9,
    FrameNumber = 10,
    FramePreState = 11,
    FrameLifecycleBefore = 12,
    FrameLaneMask = 13,
    FramePostState = 14,
    FrameLifecycleAfter = 15,
    DispatcherResult = 16,
    PresentationSerial = 17,
    DeviceBoundary = 18,
    StateTransition = 19,
};

enum class ReplayPresentationAckV2 : uint8_t {
    Accepted = 0,
    NotWaiting,
    WrongSerial,
    AlreadyCompleted,
};

enum class ReplayPlaybackExitStateV2 : uint8_t {
    Running = 0,
    ResultPending,
    ResultAcknowledged,
    ExitRequested,
};

constexpr uint32_t kNoReplayMismatchFrameV2 = 0xFFFFFFFFu;

struct ReplayPlaybackBindingV2 {
    ipc::v3::PlaybackEndpointPayload endpoint{};
    SimulationContractV2 runtimeContract{};
    bool headless = false;
    bool hasFileDigest = false;
    std::array<uint8_t, 32> fileDigest{};
    bool supersededStateSchema = false;
};

struct ReplayBootstrapObservationV2 {
    uint32_t initialPhase = 0u;
    uint64_t bootstrapFingerprint = 0u;
    uint64_t frameZeroPreStateHash = 0u;
    GameLifecycleV2 frameZeroLifecycle{};
};

struct ReplayFrameBeginObservationV2 {
    uint32_t replayFrame = 0u;
    uint64_t preStateHash = 0u;
    GameLifecycleV2 lifecycleBefore{};
    uint8_t activeLaneMask = 0u;

    // Required and nonzero only for the interactive final tick; reserve it before entering the native dispatcher so the session is TerminalArmed before the game can publish the frame.
    uint64_t finalPresentationSerial = 0u;
};

struct ReplayFrameCommitObservationV2 {
    uint32_t replayFrame = 0u;
    uint64_t postStateHash = 0u;
    GameLifecycleV2 lifecycleAfter{};
    int32_t dispatcherResult = 0;
};

class ReplayPlaybackSessionV2 {
public:
    class Checkpoint {
    public:
        Checkpoint() = default;

    private:
        friend class ReplayPlaybackSessionV2;
        const ReplayPlaybackSessionV2* owner_ = nullptr;
        const CanonicalTickBundleV2* timelineIdentity_ = nullptr;
        uint32_t nextRecord_ = 0u;
        uint32_t divergenceCount_ = 0u;
        uint32_t firstDivergenceFrame_ = kNoReplayMismatchFrameV2;
        ReplayPlaybackProofRegionV2 firstDivergenceRegion_ =
            ReplayPlaybackProofRegionV2::None;
        uint64_t firstDivergenceExpected_ = 0u;
        uint64_t firstDivergenceActual_ = 0u;
    };

    ReplayPlaybackSessionV2(
        ConfirmedTimelineBufferV2&& timeline,
        const ReplayPlaybackBindingV2& binding);

    ReplayPlaybackSessionV2(
        const ReplayPlaybackSessionV2&) = delete;
    ReplayPlaybackSessionV2& operator=(
        const ReplayPlaybackSessionV2&) = delete;
    ReplayPlaybackSessionV2(
        ReplayPlaybackSessionV2&&) = default;
    ReplayPlaybackSessionV2& operator=(
        ReplayPlaybackSessionV2&&) = default;

    ReplayPlaybackPhaseV2 phase() const { return phase_; }
    ReplayPlaybackFaultV2 fault() const { return fault_; }
    ReplayPlaybackExitStateV2 exitState() const { return exitState_; }
    bool headless() const { return binding_.headless; }

    bool transportMayHold() const;

    ReplayVerificationModeV2 mode() const { return mode_; }
    bool byteExactStateProof() const {
        return mode_ == ReplayVerificationModeV2::ByteExact;
    }

    const ConfirmedGameStartV2& start() const {
        return timeline_.start();
    }
    const ipc::v3::PlaybackEndpointPayload& endpoint() const {
        return binding_.endpoint;
    }

    uint32_t nextRecord() const { return nextRecord_; }
    uint32_t divergenceCount() const { return divergenceCount_; }
    uint32_t firstDivergenceFrame() const { return firstDivergenceFrame_; }
    uint64_t deviceCallCount() const { return deviceCallCount_; }
    uint64_t currentRollingDigest() const;

    bool completeBootstrap(
        const ReplayBootstrapObservationV2& observed);

    bool restart();

    bool captureCheckpoint(Checkpoint& checkpoint) const;
    bool restoreCheckpoint(const Checkpoint& checkpoint);

    const CanonicalTickBundleV2* peek() const;

    const CanonicalTickBundleV2* tickAt(uint32_t replayFrame) const {
        return timeline_.tick(replayFrame);
    }

    bool beginFrame(const ReplayFrameBeginObservationV2& observed);

    const std::array<uint16_t, kSourceLanes>& latchedActions() const {
        return latchedActions_;
    }
    uint8_t latchedActiveLaneMask() const {
        return latchedActiveLaneMask_;
    }
    uint32_t inFlightReplayFrame() const {
        return inFlightReplayFrame_;
    }

    bool commitFrame(const ReplayFrameCommitObservationV2& observed);

    ReplayPresentationAckV2 acknowledgeFinalPresentation(
        uint64_t presentationSerial);
    bool finalPresentationTimedOut();

    bool noteDeviceBoundaryCall(uint64_t count = 1u);

    bool requestUserExit();
    bool reportBrokerFailure();

    bool hasResult() const {
        return exitState_ != ReplayPlaybackExitStateV2::Running;
    }
    const ipc::v3::PlaybackResultPayload* result() const {
        return hasResult() ? &result_ : nullptr;
    }

    bool acknowledgeResultDelivery();
    bool requestGracefulExit();

private:
    bool validateBinding();
    bool isFinalRecord(uint32_t replayFrame) const;
    void finishVerified();
    void finishTyped(
        ipc::v3::PlaybackResultReason reason,
        ReplayPlaybackFaultV2 fault,
        ReplayPlaybackProofRegionV2 region,
        uint32_t mismatchFrame,
        uint64_t expected,
        uint64_t actual);
    void populateResultBase();
    void noteDivergence(ReplayPlaybackProofRegionV2 region,
                        uint32_t replayFrame, uint64_t expected,
                        uint64_t actual);

    ConfirmedTimelineBufferV2 timeline_;
    ReplayPlaybackBindingV2 binding_{};
    ReplayVerificationModeV2 mode_ =
        ReplayVerificationModeV2::ByteExact;
    ReplayPlaybackPhaseV2 phase_ =
        ReplayPlaybackPhaseV2::Bound;
    ReplayPlaybackFaultV2 fault_ = ReplayPlaybackFaultV2::None;
    ReplayPlaybackExitStateV2 exitState_ =
        ReplayPlaybackExitStateV2::Running;

    uint32_t divergenceCount_ = 0u;
    uint32_t firstDivergenceFrame_ = kNoReplayMismatchFrameV2;
    ReplayPlaybackProofRegionV2 firstDivergenceRegion_ =
        ReplayPlaybackProofRegionV2::None;
    uint64_t firstDivergenceExpected_ = 0u;
    uint64_t firstDivergenceActual_ = 0u;

    uint32_t nextRecord_ = 0u;
    uint32_t inFlightReplayFrame_ = kNoReplayMismatchFrameV2;
    uint8_t latchedActiveLaneMask_ = 0u;
    std::array<uint16_t, kSourceLanes> latchedActions_{};
    uint64_t finalPresentationSerial_ = 0u;
    uint64_t deviceCallCount_ = 0u;
    ipc::v3::PlaybackResultPayload result_{};
};

}
