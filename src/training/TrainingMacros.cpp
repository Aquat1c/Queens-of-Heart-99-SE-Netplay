#include "training/TrainingMacros.h"

namespace qtraining {

namespace {

const MacroSlot& emptyMacroSlot() {
    static const MacroSlot empty{};
    return empty;
}

uint64_t mixMacroRandom(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

}

bool TrainingMacros::pickRandomRecordedSlot(uint32_t& slotIndex) {
    uint32_t recorded[kTrainingMacroSlotCount] = {};
    uint32_t count = 0;
    for (uint32_t index = 0; index < kTrainingMacroSlotCount; ++index) {
        if (slots_[index].frameCount != 0u) recorded[count++] = index;
    }
    if (count == 0u) return false;
    randomState_ = mixMacroRandom(
        randomState_ ^ (lastStepSerial_ * 0xA0761D6478BD642Full));
    slotIndex = recorded[randomState_ % count];
    return true;
}

void TrainingMacros::reset() {
    for (MacroSlot& slot : slots_) slot.frameCount = 0;
    selectedSlot_ = 0;
    activePlaybackSlot_ = 0;
    randomState_ = 0;
    cursor_ = 0;
    state_ = MacroState::Idle;
    playbackMode_ = PlaybackMode::Once;
    facingPolicy_ = FacingPolicy::Relative;
    haveStep_ = false;
    lastStepSerial_ = 0;
    recordCommitPending_ = false;
    pendingRecordSerial_ = 0;
    triggeredPlaybackOnce_ = false;
    generatedPlaybackActive_ = false;
    generatedFrameCount_ = 0;
    bufferedReversalActive_ = false;
    bufferedReversalReleased_ = false;
    bufferedReversalCommitIndex_ = 0;
    bufferedReversalHoldRemaining_ = 0;
    cached_ = MacroStepResult{};
}

bool TrainingMacros::selectSlot(uint32_t slotIndex) {
    if (slotIndex >= kTrainingMacroSlotCount
        || state_ != MacroState::Idle) {
        return false;
    }
    selectedSlot_ = slotIndex;
    activePlaybackSlot_ = slotIndex;
    cursor_ = 0;
    return true;
}

const MacroSlot& TrainingMacros::slot(uint32_t index) const {
    return index < kTrainingMacroSlotCount
        ? slots_[index] : emptyMacroSlot();
}

uint32_t TrainingMacros::selectedFrameCount() const {
    return slots_[selectedSlot_].frameCount;
}

bool TrainingMacros::clearSelectedSlot() {
    if (state_ != MacroState::Idle) return false;
    slots_[selectedSlot_].frameCount = 0;
    cursor_ = 0;
    return true;
}

bool TrainingMacros::requestRecordToggle() {
    if (recordCommitPending_) return false;
    switch (state_) {
        case MacroState::Idle:
            cursor_ = 0;
            triggeredPlaybackOnce_ = false;
            generatedPlaybackActive_ = false;
            generatedFrameCount_ = 0;
            bufferedReversalActive_ = false;
            bufferedReversalReleased_ = false;
            bufferedReversalCommitIndex_ = 0;
            bufferedReversalHoldRemaining_ = 0;
            state_ = MacroState::PreRecord;
            return true;
        case MacroState::PreRecord:
            return requestRecording();
        case MacroState::RecordArmed:
            cursor_ = 0;
            state_ = MacroState::RecordStopping;
            return true;
        case MacroState::Recording:
            state_ = MacroState::RecordStopping;
            return true;
        default:
            return false;
    }
}

bool TrainingMacros::requestRecording() {
    if (recordCommitPending_) return false;
    if (state_ == MacroState::Idle && !requestPreRecord()) return false;
    if (state_ != MacroState::PreRecord) return false;
    cursor_ = 0;
    state_ = MacroState::RecordArmed;
    return true;
}

bool TrainingMacros::requestPreRecord() {
    if (recordCommitPending_ || state_ != MacroState::Idle) return false;
    cursor_ = 0;
    triggeredPlaybackOnce_ = false;
    generatedPlaybackActive_ = false;
    generatedFrameCount_ = 0;
    bufferedReversalActive_ = false;
    bufferedReversalReleased_ = false;
    bufferedReversalCommitIndex_ = 0;
    bufferedReversalHoldRemaining_ = 0;
    state_ = MacroState::PreRecord;
    return true;
}

bool TrainingMacros::requestPlaybackToggle() {
    if (recordCommitPending_) return false;
    switch (state_) {
        case MacroState::PreRecord:
            cursor_ = 0;
            state_ = MacroState::RecordStopping;
            return true;
        case MacroState::Idle: {
            uint32_t slotIndex = selectedSlot_;
            if (playbackModeRandom(playbackMode_)) {
                if (!pickRandomRecordedSlot(slotIndex)) return false;
            } else if (slots_[selectedSlot_].frameCount == 0) {
                return false;
            }
            cursor_ = 0;
            activePlaybackSlot_ = slotIndex;
            triggeredPlaybackOnce_ = false;
            generatedPlaybackActive_ = false;
            generatedFrameCount_ = 0;
            bufferedReversalActive_ = false;
            bufferedReversalReleased_ = false;
            bufferedReversalCommitIndex_ = 0;
            bufferedReversalHoldRemaining_ = 0;
            state_ = MacroState::PlayArmed;
            return true;
        }
        case MacroState::PlayArmed:
            cursor_ = 0;
            state_ = MacroState::PlayStopping;
            return true;
        case MacroState::Playing:
            state_ = MacroState::PlayStopping;
            return true;
        default:
            return false;
    }
}

bool TrainingMacros::requestTriggeredPlayback(uint32_t zeroBasedSlot) {
    if (recordCommitPending_
        || state_ != MacroState::Idle
        || zeroBasedSlot >= kTrainingMacroSlotCount
        || slots_[zeroBasedSlot].frameCount == 0) {
        return false;
    }
    cursor_ = 0;
    activePlaybackSlot_ = zeroBasedSlot;
    triggeredPlaybackOnce_ = true;
    generatedPlaybackActive_ = false;
    generatedFrameCount_ = 0;
    bufferedReversalActive_ = false;
    bufferedReversalReleased_ = false;
    bufferedReversalCommitIndex_ = 0;
    bufferedReversalHoldRemaining_ = 0;
    state_ = MacroState::Playing;
    return true;
}

bool TrainingMacros::requestTriggeredSequence(
    const uint32_t* relativeMasks, uint32_t frameCount,
    bool leadingNeutralRedundant) {
    if (recordCommitPending_
        || state_ != MacroState::Idle
        || relativeMasks == nullptr
        || frameCount == 0u
        || frameCount > kTriggeredSequenceCapacity) {
        return false;
    }
    for (uint32_t index = 0; index < frameCount; ++index) {
        if (!qinput::isCanonicalLogicalFightInput(relativeMasks[index])) {
            return false;
        }
    }

    const uint32_t sourceBegin =
        leadingNeutralRedundant && frameCount > 1u && relativeMasks[0] == 0u
            ? 1u : 0u;
    const uint32_t copiedCount = frameCount - sourceBegin;
    for (uint32_t index = 0; index < copiedCount; ++index) {
        generatedFrames_[index].logicalMask =
            relativeMasks[sourceBegin + index];
        generatedFrames_[index].recordedFacing = 0u;
    }
    generatedFrameCount_ = copiedCount;
    generatedPlaybackActive_ = true;
    bufferedReversalActive_ = false;
    bufferedReversalReleased_ = false;
    bufferedReversalCommitIndex_ = 0;
    bufferedReversalHoldRemaining_ = 0;
    triggeredPlaybackOnce_ = true;
    cursor_ = 0u;
    state_ = MacroState::Playing;
    return true;
}

bool TrainingMacros::requestBufferedReversalSequence(
    const uint32_t* relativeMasks, uint32_t frameCount) {
    if (recordCommitPending_
        || state_ != MacroState::Idle
        || relativeMasks == nullptr
        || frameCount < 2u
        || frameCount > kTriggeredSequenceCapacity
        || relativeMasks[0] != 0u) {
        return false;
    }

    uint32_t commitIndex = frameCount;
    for (uint32_t index = 0u; index < frameCount; ++index) {
        if (!qinput::isCanonicalLogicalFightInput(relativeMasks[index]))
            return false;
        if ((relativeMasks[index] & qinput::kLogicalButtonMask) != 0u)
            commitIndex = index;
    }
    if (commitIndex == frameCount || commitIndex == 0u
        || commitIndex + 1u >= frameCount) {
        return false;
    }
    for (uint32_t index = commitIndex + 1u;
         index < frameCount; ++index) {
        if ((relativeMasks[index] & qinput::kLogicalButtonMask) != 0u)
            return false;
    }

    for (uint32_t index = 0u; index < frameCount; ++index) {
        generatedFrames_[index].logicalMask = relativeMasks[index];
        generatedFrames_[index].recordedFacing = 0u;
    }
    generatedFrameCount_ = frameCount;
    generatedPlaybackActive_ = true;
    bufferedReversalActive_ = true;
    bufferedReversalReleased_ = false;
    bufferedReversalCommitIndex_ = commitIndex;
    bufferedReversalHoldRemaining_ = 0;
    triggeredPlaybackOnce_ = true;
    cursor_ = 0u;
    state_ = MacroState::Playing;
    return true;
}

bool TrainingMacros::releaseBufferedReversal() {
    if (!bufferedReversalAwaitingRelease()) return false;
    bufferedReversalReleased_ = true;
    bufferedReversalHoldRemaining_ = kBufferedCommitHold;
    return true;
}

bool TrainingMacros::requestStop() {
    if (recordCommitPending_) return false;
    switch (state_) {
        case MacroState::Idle:
            return false;
        case MacroState::RecordArmed:
            state_ = MacroState::RecordStopping;
            cursor_ = 0;
            return true;
        case MacroState::PreRecord:
            state_ = MacroState::RecordStopping;
            cursor_ = 0;
            return true;
        case MacroState::PlayArmed:
            state_ = MacroState::PlayStopping;
            cursor_ = 0;
            return true;
        case MacroState::Recording:
            state_ = MacroState::RecordStopping;
            return true;
        case MacroState::Playing:
            state_ = MacroState::PlayStopping;
            return true;
        case MacroState::RecordStopping:
        case MacroState::PlayStopping:
            return true;
    }
    return false;
}

uint32_t TrainingMacros::remapFacingRelative(
    uint32_t logicalMask, uint8_t recordedFacing, uint8_t currentFacing) {
    if (!qinput::isCanonicalLogicalFightInput(logicalMask)
        || recordedFacing > 1 || currentFacing > 1) {
        return 0;
    }
    if (recordedFacing == currentFacing) return logicalMask;
    const uint32_t horizontal =
        logicalMask & (qinput::kLogicalLeft | qinput::kLogicalRight);
    uint32_t remapped =
        logicalMask & ~(qinput::kLogicalLeft | qinput::kLogicalRight);
    if (horizontal & qinput::kLogicalLeft)
        remapped |= qinput::kLogicalRight;
    if (horizontal & qinput::kLogicalRight)
        remapped |= qinput::kLogicalLeft;
    return remapped;
}

MacroStepResult TrainingMacros::advance(
    uint64_t realStepSerial, const MacroStepInput& input) {
    if (haveStep_) {
        if (realStepSerial == lastStepSerial_) {
            MacroStepResult duplicate = cached_;
            duplicate.duplicateStep = true;
            duplicate.recordFrameRequested = false;
            duplicate.capacityReached = false;
            return duplicate;
        }
        if (realStepSerial < lastStepSerial_) {
            MacroStepResult failed{};
            failed.valid = false;
            failed.state = state_;
            failed.cursor = cursor_;
            return failed;
        }
    }
    haveStep_ = true;
    lastStepSerial_ = realStepSerial;

    MacroStepResult result{};
    if (recordCommitPending_) {
        recordCommitPending_ = false;
        state_ = MacroState::RecordStopping;
        result.valid = false;
    }
    if (!qinput::isCanonicalLogicalFightInput(input.recordSourceMask)
        || input.playbackFacing > 1) {
        result.valid = false;
        if (state_ == MacroState::Recording
            || state_ == MacroState::RecordArmed
            || state_ == MacroState::PreRecord
            || state_ == MacroState::RecordStopping) {
            state_ = MacroState::RecordStopping;
            result.overrideP1 = true;
            result.overrideP2 = true;
        } else if (state_ == MacroState::Playing
                   || state_ == MacroState::PlayArmed
                   || state_ == MacroState::PlayStopping) {
            state_ = MacroState::PlayStopping;
            result.overrideP2 = true;
        }
        result.state = state_;
        result.cursor = cursor_;
        cached_ = result;
        return result;
    }

    if (input.paused) {
        switch (state_) {
            case MacroState::PreRecord:
            case MacroState::RecordArmed:
            case MacroState::Recording:
            case MacroState::RecordStopping:
                result.overrideP1 = true;
                result.overrideP2 = true;
                break;
            case MacroState::PlayArmed:
            case MacroState::Playing:
            case MacroState::PlayStopping:
                result.overrideP2 = true;
                break;
            case MacroState::Idle:
                break;
        }
        result.state = state_;
        result.cursor = cursor_;
        cached_ = result;
        return result;
    }

    switch (state_) {
        case MacroState::Idle:
            break;

        case MacroState::PreRecord:
            result.overrideP1 = true;
            result.overrideP2 = true;
            result.p2Mask = input.recordSourceMask;
            break;

        case MacroState::RecordArmed:
            result.overrideP1 = true;
            result.overrideP2 = true;
            slots_[selectedSlot_].frameCount = 0;
            cursor_ = 0;
            state_ = MacroState::Recording;
            break;

        case MacroState::Recording: {
            result.overrideP1 = true;
            result.overrideP2 = true;
            result.p2Mask = input.recordSourceMask;
            MacroSlot& selected = slots_[selectedSlot_];
            if (selected.frameCount >= kTrainingMacroFrameCapacity) {
                state_ = MacroState::RecordStopping;
                result.p2Mask = 0;
                result.capacityReached = true;
                break;
            }
            result.recordFrameRequested = true;
            recordCommitPending_ = true;
            pendingRecordSerial_ = realStepSerial;
            break;
        }

        case MacroState::RecordStopping:
            result.overrideP1 = true;
            result.overrideP2 = true;
            state_ = MacroState::Idle;
            cursor_ = 0;
            break;

        case MacroState::PlayArmed:
            result.overrideP2 = true;
            cursor_ = 0;
            state_ = MacroState::Playing;
            break;

        case MacroState::Playing: {
            result.overrideP2 = true;
            result.generatedPlayback = generatedPlaybackActive_;
            if (bufferedReversalActive_
                && !bufferedReversalReleased_
                && cursor_ == bufferedReversalCommitIndex_) {
                result.playbackFrame = false;
                result.p2Mask = 0u;
                break;
            }
            if (bufferedReversalActive_ && bufferedReversalReleased_
                && bufferedReversalHoldRemaining_ != 0u
                && generatedPlaybackActive_
                && cursor_ == bufferedReversalCommitIndex_ + 1u
                && bufferedReversalCommitIndex_ < generatedFrameCount_) {
                --bufferedReversalHoldRemaining_;
                const MacroFrame& held =
                    generatedFrames_[bufferedReversalCommitIndex_];
                result.playbackFrame = true;
                result.p2Mask = remapFacingRelative(
                    held.logicalMask, held.recordedFacing,
                    input.playbackFacing);
                break;
            }
            result.playbackFrame = true;
            const uint32_t frameCount = generatedPlaybackActive_
                ? generatedFrameCount_
                : slots_[activePlaybackSlot_].frameCount;
            if (frameCount == 0u || cursor_ >= frameCount) {
                result.playbackFrame = false;
                state_ = MacroState::PlayStopping;
                result.p2Mask = 0;
                break;
            }
            const MacroFrame& frame = generatedPlaybackActive_
                ? generatedFrames_[cursor_]
                : slots_[activePlaybackSlot_].frames[cursor_];
            result.p2Mask = (generatedPlaybackActive_
                             || facingPolicy_ == FacingPolicy::Relative)
                ? remapFacingRelative(frame.logicalMask,
                                      frame.recordedFacing,
                                      input.playbackFacing)
                : frame.logicalMask;
            ++cursor_;
            if (cursor_ == frameCount) {
                if (!triggeredPlaybackOnce_
                    && playbackModeLoops(playbackMode_)) {
                    cursor_ = 0;
                    uint32_t next = activePlaybackSlot_;
                    if (playbackModeRandom(playbackMode_)
                        && !generatedPlaybackActive_
                        && pickRandomRecordedSlot(next)) {
                        activePlaybackSlot_ = next;
                    }
                } else {
                    state_ = MacroState::PlayStopping;
                }
            }
            break;
        }

        case MacroState::PlayStopping:
            result.overrideP2 = true;
            state_ = MacroState::Idle;
            cursor_ = 0;
            activePlaybackSlot_ = selectedSlot_;
            triggeredPlaybackOnce_ = false;
            generatedPlaybackActive_ = false;
            generatedFrameCount_ = 0;
            bufferedReversalActive_ = false;
            bufferedReversalReleased_ = false;
            bufferedReversalCommitIndex_ = 0;
            bufferedReversalHoldRemaining_ = 0;
            break;
    }

    result.state = state_;
    result.cursor = cursor_;
    cached_ = result;
    return result;
}

MacroRecordCommitResult TrainingMacros::commitRecordedFrame(
    uint64_t realStepSerial, uint32_t effectiveP2Mask,
    uint8_t p2Facing) {
    MacroRecordCommitResult result{};
    if (!recordCommitPending_
        || realStepSerial != pendingRecordSerial_
        || state_ != MacroState::Recording
        || !qinput::isCanonicalLogicalFightInput(effectiveP2Mask)
        || p2Facing > 1) {
        result.valid = false;
        if (recordCommitPending_) {
            recordCommitPending_ = false;
            state_ = MacroState::RecordStopping;
        }
        result.state = state_;
        result.cursor = cursor_;
        return result;
    }

    MacroSlot& selected = slots_[selectedSlot_];
    if (selected.frameCount >= kTrainingMacroFrameCapacity) {
        recordCommitPending_ = false;
        state_ = MacroState::RecordStopping;
        result.valid = false;
        result.capacityReached = true;
        result.state = state_;
        result.cursor = cursor_;
        return result;
    }
    MacroFrame& frame = selected.frames[selected.frameCount++];
    frame.logicalMask = effectiveP2Mask;
    frame.recordedFacing = p2Facing;
    recordCommitPending_ = false;
    cursor_ = selected.frameCount;
    result.committed = true;
    if (selected.frameCount == kTrainingMacroFrameCapacity) {
        state_ = MacroState::RecordStopping;
        result.capacityReached = true;
    }
    result.state = state_;
    result.cursor = cursor_;
    return result;
}

}
