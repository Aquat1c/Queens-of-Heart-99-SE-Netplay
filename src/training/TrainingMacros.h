#pragma once

#include <cstdint>

#include "input/InputActions.h"
#include "training/TrainingTypes.h"

namespace qtraining {

struct MacroFrame {
    uint32_t logicalMask = 0;
    uint8_t recordedFacing = 0;
    uint8_t reserved[3] = {};
};

struct MacroSlot {
    uint32_t frameCount = 0;
    MacroFrame frames[kTrainingMacroFrameCapacity] = {};
};

struct MacroStepInput {
    bool paused = false;
    uint32_t recordSourceMask = 0;
    uint8_t playbackFacing = 0;
};

struct MacroStepResult {
    bool valid = true;
    bool duplicateStep = false;
    bool overrideP1 = false;
    bool overrideP2 = false;
    bool playbackFrame = false;
    bool generatedPlayback = false;
    bool recordFrameRequested = false;
    bool capacityReached = false;
    uint32_t p1Mask = 0;
    uint32_t p2Mask = 0;
    MacroState state = MacroState::Idle;
    uint32_t cursor = 0;
};

struct MacroRecordCommitResult {
    bool valid = true;
    bool committed = false;
    bool capacityReached = false;
    MacroState state = MacroState::Idle;
    uint32_t cursor = 0;
};

class TrainingMacros {
public:
    static constexpr uint32_t kTriggeredSequenceCapacity = 256u;

    void reset();

    bool selectSlot(uint32_t slot);
    uint32_t selectedSlot() const { return selectedSlot_; }
    const MacroSlot& slot(uint32_t index) const;
    uint32_t selectedFrameCount() const;
    bool clearSelectedSlot();

    bool setPlaybackMode(PlaybackMode mode) {
        if (mode > PlaybackMode::RandomLoop) return false;
        playbackMode_ = mode;
        return true;
    }
    bool setFacingPolicy(FacingPolicy policy) {
        if (policy > FacingPolicy::Absolute) return false;
        facingPolicy_ = policy;
        return true;
    }
    PlaybackMode playbackMode() const { return playbackMode_; }
    FacingPolicy facingPolicy() const { return facingPolicy_; }

    bool requestRecordToggle();
    bool requestRecording();
    bool requestPreRecord();
    bool requestPlaybackToggle();
    bool requestTriggeredPlayback(uint32_t zeroBasedSlot);
    bool requestTriggeredSequence(const uint32_t* relativeMasks,
                                  uint32_t frameCount,
                                  bool leadingNeutralRedundant = false);
    bool requestBufferedReversalSequence(const uint32_t* relativeMasks,
                                         uint32_t frameCount);
    bool releaseBufferedReversal();

    static constexpr uint32_t kBufferedCommitHold = 1u;
    bool bufferedReversalActive() const {
        return bufferedReversalActive_;
    }
    bool bufferedReversalAwaitingRelease() const {
        return bufferedReversalActive_
            && state_ == MacroState::Playing
            && cursor_ == bufferedReversalCommitIndex_
            && !bufferedReversalReleased_;
    }
    uint32_t bufferedReversalCommitIndex() const {
        return bufferedReversalCommitIndex_;
    }
    bool requestStop();

    MacroStepResult advance(uint64_t realStepSerial,
                            const MacroStepInput& input);

    MacroRecordCommitResult commitRecordedFrame(
        uint64_t realStepSerial, uint32_t effectiveP2Mask,
        uint8_t p2Facing);

    MacroState state() const { return state_; }
    uint32_t cursor() const { return cursor_; }
    uint32_t activePlaybackSlot() const { return activePlaybackSlot_; }

    static uint32_t remapFacingRelative(uint32_t logicalMask,
                                        uint8_t recordedFacing,
                                        uint8_t currentFacing);

private:
    bool pickRandomRecordedSlot(uint32_t& slotIndex);

    MacroSlot slots_[kTrainingMacroSlotCount] = {};
    uint64_t randomState_ = 0;
    uint32_t selectedSlot_ = 0;
    uint32_t activePlaybackSlot_ = 0;
    uint32_t cursor_ = 0;
    MacroState state_ = MacroState::Idle;
    PlaybackMode playbackMode_ = PlaybackMode::Once;
    FacingPolicy facingPolicy_ = FacingPolicy::Relative;
    bool haveStep_ = false;
    uint64_t lastStepSerial_ = 0;
    bool recordCommitPending_ = false;
    uint64_t pendingRecordSerial_ = 0;
    bool triggeredPlaybackOnce_ = false;
    bool generatedPlaybackActive_ = false;
    uint32_t generatedFrameCount_ = 0;
    bool bufferedReversalActive_ = false;
    bool bufferedReversalReleased_ = false;
    uint32_t bufferedReversalCommitIndex_ = 0;
    uint32_t bufferedReversalHoldRemaining_ = 0;
    MacroFrame generatedFrames_[kTriggeredSequenceCapacity] = {};
    MacroStepResult cached_{};
};

}
