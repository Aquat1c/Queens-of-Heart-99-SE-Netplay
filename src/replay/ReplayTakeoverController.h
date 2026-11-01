#pragma once

#include "replay/ReplayTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qreplay {

constexpr uint32_t kReplayTakeoverCountdownFrames = 40u;
constexpr uint8_t kReplayTakeoverMaximumDelay = 9u;
constexpr uint32_t kReplayTakeoverContinuationFrames = 300000u;

enum class ReplayTakeoverPhase : uint8_t {
    Inactive = 0,
    Countdown = 1,
    Active = 2,
    RestorePending = 3,
    Faulted = 4,
};

enum class ReplayTakeoverRestoreReason : uint8_t {
    None = 0,
    Retry = 1,
    SwitchP1 = 2,
    SwitchP2 = 3,
    Exit = 4,
};

enum class ReplayTakeoverFault : uint8_t {
    None = 0,
    StorageUnavailable = 1,
    CapacityExceeded = 2,
    InvalidInput = 3,
    RestoreFailed = 4,
    SimulationFailed = 5,
};

struct ReplayTakeoverResolvedFrame {
    uint32_t replayFrame = 0u;
    uint32_t sourceFrame = 0u;
    uint8_t activeLaneMask = 0u;
    std::array<uint16_t, kSourceLanes> actions{};
    bool beyondEndpoint = false;
};

class ReplayTakeoverController {
public:
    bool prepare(uint32_t capacityFrames);
    void reset();

    bool begin(uint32_t branchReplayFrame, uint32_t branchSourceFrame,
               uint8_t selectedLane);
    bool requestRetry();
    bool requestSwitch(uint8_t selectedLane);
    bool requestExit();
    bool completeRestore(bool succeeded);
    bool failSimulation();

    bool advanceCountdown();

    bool setDelay(uint8_t delay);
    void increaseDelay();
    void decreaseDelay();

    bool capture(uint16_t compactAction);
    bool resolve(const CanonicalTickBundleV2* recorded,
                 uint8_t fallbackLaneMask,
                 ReplayTakeoverResolvedFrame& out) const;
    bool advance();

    ReplayTakeoverPhase phase() const { return phase_; }
    ReplayTakeoverRestoreReason restoreReason() const { return restoreReason_; }
    ReplayTakeoverFault fault() const { return fault_; }
    bool active() const { return phase_ != ReplayTakeoverPhase::Inactive; }
    bool simulating() const { return phase_ == ReplayTakeoverPhase::Active; }
    uint8_t selectedLane() const { return selectedLane_; }
    uint8_t delay() const { return delay_; }
    uint32_t countdown() const { return countdown_; }
    uint32_t branchReplayFrame() const { return branchReplayFrame_; }
    uint32_t branchSourceFrame() const { return branchSourceFrame_; }
    uint32_t currentReplayFrame() const { return currentReplayFrame_; }
    uint32_t elapsedFrames() const { return elapsedFrames_; }
    uint32_t capacityFrames() const {
        return static_cast<uint32_t>(slots_.size());
    }

private:
    static constexpr uint16_t kNoInput = 0xFFFFu;
    static constexpr uint32_t kEmptySlot =
        static_cast<uint32_t>(kNoInput)
        | (static_cast<uint32_t>(kNoInput) << 16u);

    bool validLane(uint8_t lane) const { return lane < 2u; }
    uint16_t laneValue(uint32_t relativeFrame, uint8_t lane) const;
    void setLaneValue(uint32_t relativeFrame, uint8_t lane, uint16_t value);
    void faultWith(ReplayTakeoverFault fault);

    ReplayTakeoverPhase phase_ = ReplayTakeoverPhase::Inactive;
    ReplayTakeoverRestoreReason restoreReason_ =
        ReplayTakeoverRestoreReason::None;
    ReplayTakeoverFault fault_ = ReplayTakeoverFault::None;
    uint8_t selectedLane_ = 0u;
    uint8_t delay_ = 0u;
    uint32_t countdown_ = 0u;
    uint32_t branchReplayFrame_ = 0u;
    uint32_t branchSourceFrame_ = 0u;
    uint32_t currentReplayFrame_ = 0u;
    uint32_t elapsedFrames_ = 0u;
    bool haveLastCapture_ = false;
    uint32_t lastCaptureRelativeFrame_ = 0u;
    std::vector<uint32_t> slots_;
};

}
