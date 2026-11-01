#include "replay/ReplayTakeoverController.h"

#include <algorithm>
#include <new>

namespace qreplay {

bool ReplayTakeoverController::prepare(uint32_t capacityFrames) {
    if (capacityFrames == 0u || capacityFrames > kMaxTimelineFrames)
        return false;
    try {
        slots_.assign(capacityFrames, kEmptySlot);
    } catch (const std::bad_alloc&) {
        slots_.clear();
        faultWith(ReplayTakeoverFault::StorageUnavailable);
        return false;
    }
    reset();
    return true;
}

void ReplayTakeoverController::reset() {
    phase_ = ReplayTakeoverPhase::Inactive;
    restoreReason_ = ReplayTakeoverRestoreReason::None;
    fault_ = ReplayTakeoverFault::None;
    selectedLane_ = 0u;
    delay_ = 0u;
    countdown_ = 0u;
    branchReplayFrame_ = 0u;
    branchSourceFrame_ = 0u;
    currentReplayFrame_ = 0u;
    elapsedFrames_ = 0u;
    haveLastCapture_ = false;
    lastCaptureRelativeFrame_ = 0u;
    std::fill(slots_.begin(), slots_.end(), kEmptySlot);
}

bool ReplayTakeoverController::begin(uint32_t branchReplayFrame,
                                     uint32_t branchSourceFrame,
                                     uint8_t selectedLane) {
    if (phase_ != ReplayTakeoverPhase::Inactive || slots_.empty()
        || !validLane(selectedLane)) {
        return false;
    }
    std::fill(slots_.begin(), slots_.end(), kEmptySlot);
    branchReplayFrame_ = branchReplayFrame;
    branchSourceFrame_ = branchSourceFrame;
    currentReplayFrame_ = branchReplayFrame;
    elapsedFrames_ = 0u;
    selectedLane_ = selectedLane;
    countdown_ = kReplayTakeoverCountdownFrames;
    haveLastCapture_ = false;
    restoreReason_ = ReplayTakeoverRestoreReason::None;
    fault_ = ReplayTakeoverFault::None;
    phase_ = ReplayTakeoverPhase::Countdown;
    return true;
}

bool ReplayTakeoverController::requestRetry() {
    if (!active() || phase_ == ReplayTakeoverPhase::Faulted
        || phase_ == ReplayTakeoverPhase::RestorePending) {
        return false;
    }
    restoreReason_ = ReplayTakeoverRestoreReason::Retry;
    phase_ = ReplayTakeoverPhase::RestorePending;
    return true;
}

bool ReplayTakeoverController::requestSwitch(uint8_t selectedLane) {
    if (!validLane(selectedLane) || !active()
        || phase_ == ReplayTakeoverPhase::Faulted
        || phase_ == ReplayTakeoverPhase::RestorePending) {
        return false;
    }
    selectedLane_ = selectedLane;
    restoreReason_ = selectedLane == 0u
        ? ReplayTakeoverRestoreReason::SwitchP1
        : ReplayTakeoverRestoreReason::SwitchP2;
    phase_ = ReplayTakeoverPhase::RestorePending;
    return true;
}

bool ReplayTakeoverController::requestExit() {
    if (!active() || phase_ == ReplayTakeoverPhase::RestorePending)
        return false;
    restoreReason_ = ReplayTakeoverRestoreReason::Exit;
    phase_ = ReplayTakeoverPhase::RestorePending;
    return true;
}

bool ReplayTakeoverController::completeRestore(bool succeeded) {
    if (phase_ != ReplayTakeoverPhase::RestorePending) return false;
    const ReplayTakeoverRestoreReason completed = restoreReason_;
    restoreReason_ = ReplayTakeoverRestoreReason::None;
    if (!succeeded) {
        faultWith(ReplayTakeoverFault::RestoreFailed);
        return false;
    }
    currentReplayFrame_ = branchReplayFrame_;
    elapsedFrames_ = 0u;
    haveLastCapture_ = false;
    lastCaptureRelativeFrame_ = 0u;
    if (completed == ReplayTakeoverRestoreReason::Exit) {
        phase_ = ReplayTakeoverPhase::Inactive;
        countdown_ = 0u;
        std::fill(slots_.begin(), slots_.end(), kEmptySlot);
    } else {
        phase_ = ReplayTakeoverPhase::Countdown;
        countdown_ = kReplayTakeoverCountdownFrames;
    }
    return true;
}

bool ReplayTakeoverController::failSimulation() {
    if (phase_ != ReplayTakeoverPhase::Active) return false;
    faultWith(ReplayTakeoverFault::SimulationFailed);
    return true;
}

bool ReplayTakeoverController::advanceCountdown() {
    if (phase_ != ReplayTakeoverPhase::Countdown) return false;
    if (countdown_ > 0u) --countdown_;
    if (countdown_ != 0u) return false;
    phase_ = ReplayTakeoverPhase::Active;
    return true;
}

bool ReplayTakeoverController::setDelay(uint8_t delay) {
    if (delay > kReplayTakeoverMaximumDelay) return false;
    delay_ = delay;
    return true;
}

void ReplayTakeoverController::increaseDelay() {
    if (delay_ < kReplayTakeoverMaximumDelay) ++delay_;
}

void ReplayTakeoverController::decreaseDelay() {
    if (delay_ > 0u) --delay_;
}

bool ReplayTakeoverController::capture(uint16_t compactAction) {
    if (phase_ != ReplayTakeoverPhase::Active
        || !isValidActionMask(compactAction)) {
        if (phase_ == ReplayTakeoverPhase::Active)
            faultWith(ReplayTakeoverFault::InvalidInput);
        return false;
    }
    const uint64_t target64 = static_cast<uint64_t>(elapsedFrames_) + delay_;
    if (target64 >= slots_.size()) {
        faultWith(ReplayTakeoverFault::CapacityExceeded);
        return false;
    }
    const uint32_t target = static_cast<uint32_t>(target64);
    if (!haveLastCapture_) {
        setLaneValue(target, selectedLane_, compactAction);
    } else if (lastCaptureRelativeFrame_ < target) {
        for (uint32_t frame = lastCaptureRelativeFrame_ + 1u;
             frame <= target; ++frame) {
            setLaneValue(frame, selectedLane_, compactAction);
        }
    }
    lastCaptureRelativeFrame_ = target;
    haveLastCapture_ = true;
    return true;
}

bool ReplayTakeoverController::resolve(
        const CanonicalTickBundleV2* recorded, uint8_t fallbackLaneMask,
        ReplayTakeoverResolvedFrame& out) const {
    if (phase_ != ReplayTakeoverPhase::Active
        || elapsedFrames_ >= slots_.size()) {
        return false;
    }
    ReplayTakeoverResolvedFrame resolved{};
    resolved.replayFrame = currentReplayFrame_;
    resolved.sourceFrame = recorded != nullptr
        ? recorded->frame.sourceFrame
        : branchSourceFrame_ + elapsedFrames_;
    resolved.activeLaneMask = recorded != nullptr
        ? recorded->frame.core.activeLaneMask : fallbackLaneMask;
    resolved.beyondEndpoint = recorded == nullptr;
    if (recorded != nullptr)
        resolved.actions = recorded->frame.core.actions;
    for (uint8_t lane = 0u; lane < 2u; ++lane) {
        const uint16_t value = laneValue(elapsedFrames_, lane);
        if (value != kNoInput) resolved.actions[lane] = value;
    }
    out = resolved;
    return true;
}

bool ReplayTakeoverController::advance() {
    if (phase_ != ReplayTakeoverPhase::Active) return false;
    if (elapsedFrames_ + 1u >= slots_.size()) {
        faultWith(ReplayTakeoverFault::CapacityExceeded);
        return false;
    }
    ++elapsedFrames_;
    ++currentReplayFrame_;
    return true;
}

uint16_t ReplayTakeoverController::laneValue(uint32_t relativeFrame,
                                             uint8_t lane) const {
    if (relativeFrame >= slots_.size() || !validLane(lane)) return kNoInput;
    const uint32_t slot = slots_[relativeFrame];
    return lane == 0u
        ? static_cast<uint16_t>(slot & 0xFFFFu)
        : static_cast<uint16_t>((slot >> 16u) & 0xFFFFu);
}

void ReplayTakeoverController::setLaneValue(uint32_t relativeFrame,
                                            uint8_t lane, uint16_t value) {
    uint32_t& slot = slots_[relativeFrame];
    if (lane == 0u)
        slot = (slot & 0xFFFF0000u) | value;
    else
        slot = (slot & 0x0000FFFFu) | (static_cast<uint32_t>(value) << 16u);
}

void ReplayTakeoverController::faultWith(ReplayTakeoverFault fault) {
    fault_ = fault;
    phase_ = ReplayTakeoverPhase::Faulted;
    countdown_ = 0u;
    restoreReason_ = ReplayTakeoverRestoreReason::None;
}

}
