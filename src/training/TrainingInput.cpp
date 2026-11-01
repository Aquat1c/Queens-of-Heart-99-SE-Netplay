#include "training/TrainingInput.h"

#include "training/TrainingDummy.h"

namespace qtraining {

void TrainingInput::resumeAfterOperation(uint32_t consumedFightMask) {
    carryMask_ = consumedFightMask & qinput::kLogicalDirectionMask;
    previousActions_ = 0;
    actionsArmed_ = false;
    resetSocd();
}

void TrainingInput::reset() {
    rearmState_ = InputRearmState::AwaitNeutral;
    carryMask_ = 0;
    previousActions_ = 0;
    actionsArmed_ = false;
    havePoll_ = false;
    lastPollSerial_ = 0;
    cachedPoll_ = TrainingActionResult{};
    haveStep_ = false;
    lastStepSerial_ = 0;
    cached_ = TrainingInputResult{};
    resetSocd();
}

void TrainingInput::resetSocd() {
    p1Socd_ = SocdMemory{};
    p2Socd_ = SocdMemory{};
}

void TrainingInput::disarmActions(uint32_t currentlyHeld) {
    previousActions_ = currentlyHeld & kTrainingActionMask;
    actionsArmed_ = false;
}

uint32_t TrainingInput::cleanSocd(uint32_t raw,
                                  qinput::SocdPolicy horizontal,
                                  qinput::SocdPolicy vertical,
                                  SocdMemory& memory) {
    raw &= qinput::kLogicalFightMask;
    const uint32_t edges = raw & ~memory.previousRaw;
    memory.previousRaw = raw;
    uint32_t clean = raw;

    const bool left = (raw & qinput::kLogicalLeft) != 0;
    const bool right = (raw & qinput::kLogicalRight) != 0;
    if (left && right) {
        switch (horizontal) {
            case qinput::SocdPolicy::Native:
                break;
            case qinput::SocdPolicy::Neutral:
            case qinput::SocdPolicy::UpPriority:
                clean &= ~(qinput::kLogicalLeft | qinput::kLogicalRight);
                break;
            case qinput::SocdPolicy::LastInput: {
                uint8_t winner = memory.horizontalWinner;
                const bool leftEdge = (edges & qinput::kLogicalLeft) != 0;
                const bool rightEdge = (edges & qinput::kLogicalRight) != 0;
                if (leftEdge != rightEdge)
                    winner = leftEdge ? 1 : 2;
                clean &= ~(qinput::kLogicalLeft | qinput::kLogicalRight);
                if (winner == 1) clean |= qinput::kLogicalLeft;
                if (winner == 2) clean |= qinput::kLogicalRight;
                break;
            }
        }
    }
    if (left != right) {
        memory.horizontalWinner = left ? 1 : 2;
    } else if (!left && !right) {
        memory.horizontalWinner = 0;
    } else if (horizontal == qinput::SocdPolicy::LastInput) {
        const bool leftEdge = (edges & qinput::kLogicalLeft) != 0;
        const bool rightEdge = (edges & qinput::kLogicalRight) != 0;
        if (leftEdge != rightEdge)
            memory.horizontalWinner = leftEdge ? 1 : 2;
    }

    const bool up = (raw & qinput::kLogicalUp) != 0;
    const bool down = (raw & qinput::kLogicalDown) != 0;
    if (up && down) {
        switch (vertical) {
            case qinput::SocdPolicy::Native:
                break;
            case qinput::SocdPolicy::Neutral:
                clean &= ~(qinput::kLogicalUp | qinput::kLogicalDown);
                break;
            case qinput::SocdPolicy::UpPriority:
                clean &= ~qinput::kLogicalDown;
                break;
            case qinput::SocdPolicy::LastInput: {
                uint8_t winner = memory.verticalWinner;
                const bool upEdge = (edges & qinput::kLogicalUp) != 0;
                const bool downEdge = (edges & qinput::kLogicalDown) != 0;
                if (upEdge != downEdge)
                    winner = upEdge ? 1 : 2;
                clean &= ~(qinput::kLogicalUp | qinput::kLogicalDown);
                if (winner == 1) clean |= qinput::kLogicalUp;
                if (winner == 2) clean |= qinput::kLogicalDown;
                break;
            }
        }
    }
    if (up != down) {
        memory.verticalWinner = up ? 1 : 2;
    } else if (!up && !down) {
        memory.verticalWinner = 0;
    } else if (vertical == qinput::SocdPolicy::LastInput) {
        const bool upEdge = (edges & qinput::kLogicalUp) != 0;
        const bool downEdge = (edges & qinput::kLogicalDown) != 0;
        if (upEdge != downEdge)
            memory.verticalWinner = upEdge ? 1 : 2;
    }
    return clean & qinput::kLogicalFightMask;
}

TrainingActionResult TrainingInput::pollActions(
    uint64_t pollSerial, const TrainingActionPoll& poll) {
    if (havePoll_) {
        if (pollSerial == lastPollSerial_) {
            TrainingActionResult duplicate = cachedPoll_;
            duplicate.duplicatePoll = true;
            duplicate.actionEdges = 0;
            duplicate.resetModifier = 0;
            return duplicate;
        }
        if (pollSerial < lastPollSerial_) {
            TrainingActionResult failed{};
            failed.valid = false;
            failed.rearmState = rearmState_;
            return failed;
        }
    }
    havePoll_ = true;
    lastPollSerial_ = pollSerial;

    TrainingActionResult result{};
    const uint32_t heldActions =
        poll.heldTrainingActions & kTrainingActionMask;
    auto publish = [&]() {
        result.rearmState = rearmState_;
        cachedPoll_ = result;
        return result;
    };

    if (!isCanonicalTrainingActions(poll.heldTrainingActions)
        || !qinput::isCanonicalLogicalFightInput(poll.p1PhysicalMask)) {
        result.valid = false;
        rearmState_ = InputRearmState::AwaitNeutral;
        disarmActions(heldActions);
        resetSocd();
        return publish();
    }
    if (!poll.lifecycleAvailable) {
        rearmState_ = InputRearmState::AwaitNeutral;
        disarmActions(heldActions);
        resetSocd();
        return publish();
    }
    if (poll.menuCaptured) {
        rearmState_ = InputRearmState::Captured;
        disarmActions(heldActions);
        resetSocd();
        return publish();
    }
    if (rearmState_ == InputRearmState::Captured)
        rearmState_ = InputRearmState::AwaitNeutral;

    if (poll.operationNeutral) {
        disarmActions(heldActions);
        resetSocd();
        return publish();
    }

    const bool relevantNeutral = poll.relevantPhysicalNeutralBeforeSocd;
    if (rearmState_ == InputRearmState::AwaitNeutral) {
        disarmActions(heldActions);
        if (relevantNeutral)
            rearmState_ = InputRearmState::NeutralSettle;
        return publish();
    }
    if (rearmState_ == InputRearmState::NeutralSettle) {
        disarmActions(heldActions);
        if (!relevantNeutral) {
            rearmState_ = InputRearmState::AwaitNeutral;
        } else {
            rearmState_ = InputRearmState::Armed;
            actionsArmed_ = heldActions == 0;
            previousActions_ = heldActions;
        }
        return publish();
    }

    if (!actionsArmed_) {
        if (heldActions == 0) actionsArmed_ = true;
        previousActions_ = heldActions;
    } else {
        result.actionEdges = heldActions & ~previousActions_;
        result.heldLevelActions = heldActions & kTrainingLevelActionMask;
        previousActions_ = heldActions;
    }
    if (result.actionEdges
        & trainingActionBit(TrainingAction::ResetPosition)) {
        result.resetModifier =
            poll.p1PhysicalMask & qinput::kLogicalDirectionMask;
    }
    return publish();
}

TrainingInputResult TrainingInput::advance(
    uint64_t realStepSerial, const TrainingInputStep& step) {
    if (haveStep_) {
        if (realStepSerial == lastStepSerial_) {
            TrainingInputResult duplicate = cached_;
            duplicate.duplicateStep = true;
            return duplicate;
        }
        if (realStepSerial < lastStepSerial_) {
            TrainingInputResult failed{};
            failed.valid = false;
            failed.neutralized = true;
            failed.drivenLane = trainingDrivenLane(step);
            failed.rearmState = rearmState_;
            return failed;
        }
    }
    haveStep_ = true;
    lastStepSerial_ = realStepSerial;

    TrainingInputResult result{};
    result.drivenLane = trainingDrivenLane(step);
    const bool canonical =
        qinput::isCanonicalLogicalFightInput(step.p1PhysicalMask)
        && qinput::isCanonicalLogicalFightInput(step.p2PhysicalMask)
        && qinput::isCanonicalLogicalFightInput(step.macroP1Mask)
        && qinput::isCanonicalLogicalFightInput(step.macroP2Mask)
        && isCanonicalDummyOwnedAxes(step.dummyOwnedAxes)
        && isCanonicalDummyOwnedButtons(step.dummyOwnedButtons)
        && (step.dummyDirections & ~step.dummyOwnedAxes) == 0
        && (step.dummyButtons & ~step.dummyOwnedButtons) == 0
        && !(step.dummyExclusive && step.macroPlaybackActive)
        && (!step.dummyExclusive
            || step.dummyOwnedAxes
                   == (kDummyHorizontalAxis | kDummyVerticalAxis))
        && static_cast<uint8_t>(step.horizontalSocd)
            <= static_cast<uint8_t>(qinput::SocdPolicy::UpPriority)
        && static_cast<uint8_t>(step.verticalSocd)
            <= static_cast<uint8_t>(qinput::SocdPolicy::UpPriority);
    if (!canonical) {
        result.valid = false;
        resetSocd();
        result.rearmState = rearmState_;
        cached_ = result;
        return result;
    }

    if (!step.lifecycleAvailable || step.operationNeutral) {
        resetSocd();
        result.rearmState = rearmState_;
        cached_ = result;
        return result;
    }

    const bool physicalSuppressed =
        step.menuCaptured || rearmState_ != InputRearmState::Armed;

    uint32_t p1 = 0;
    uint32_t p2 = 0;
    if (!physicalSuppressed) {
        p1 = step.p1PhysicalMask;
        p2 = step.p2PhysicalMask;
        if (step.macroOverrideP1) p1 = step.macroP1Mask;
        if (step.macroOverrideP2) p2 = step.macroP2Mask;
    }

    if (carryMask_ != 0u) {
        const bool humanOnP1 =
            trainingDrivenLane(step) == kTrainingDrivenLaneP1;
        carryMask_ &= humanOnP1 ? step.p1PhysicalMask : step.p2PhysicalMask;
        uint32_t& humanLane = humanOnP1 ? p1 : p2;
        humanLane &= ~carryMask_;
    }

    const bool allowDummyPosture =
        !step.macroPlaybackActive || step.guardDuringPlayback;
    if (allowDummyPosture) {
        uint32_t& lane =
            trainingPolicyLane(step) == kTrainingDrivenLaneP1 ? p1 : p2;
        uint32_t owned = step.dummyOwnedAxes;
        if (step.macroPlaybackActive) {
            if ((lane & kDummyHorizontalAxis) != 0)
                owned &= ~kDummyHorizontalAxis;
            if ((lane & kDummyVerticalAxis) != 0)
                owned &= ~kDummyVerticalAxis;
        }
        lane = (lane & ~owned) | (step.dummyDirections & owned);
        result.postureLane = trainingPolicyLane(step);
        result.postureOwnedAxes = owned;
        const uint32_t ownedButtons = step.dummyOwnedButtons;
        lane = (lane & ~ownedButtons) | (step.dummyButtons & ownedButtons);
    }

    result.p1Mask = cleanSocd(p1, step.horizontalSocd,
                              step.verticalSocd, p1Socd_);
    result.p2Mask = cleanSocd(p2, step.horizontalSocd,
                              step.verticalSocd, p2Socd_);
    result.neutralized = result.p1Mask == 0 && result.p2Mask == 0;
    result.rearmState = rearmState_;
    cached_ = result;
    return result;
}

}
