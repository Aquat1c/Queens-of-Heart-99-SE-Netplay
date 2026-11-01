#pragma once

#include <cstdint>

#include "input/InputActions.h"
#include "input/InputLaunchConfig.h"
#include "training/TrainingTypes.h"

namespace qtraining {

struct TrainingActionPoll {
    bool lifecycleAvailable = false;
    bool menuCaptured = false;

    bool operationNeutral = false;

    bool relevantPhysicalNeutralBeforeSocd = false;

    uint32_t heldTrainingActions = 0;
    uint32_t p1PhysicalMask = 0;
};

struct TrainingActionResult {
    bool valid = true;
    bool duplicatePoll = false;
    uint32_t actionEdges = 0;
    uint32_t heldLevelActions = 0;
    uint32_t resetModifier = 0;
    InputRearmState rearmState = InputRearmState::AwaitNeutral;
};

struct TrainingInputStep {
    bool lifecycleAvailable = false;
    bool menuCaptured = false;

    bool operationNeutral = false;

    uint32_t p1PhysicalMask = 0;
    uint32_t p2PhysicalMask = 0;

    bool dummyIsP1 = false;

    bool humanDrivesDummyLane = false;

    bool macroOverrideP1 = false;
    bool macroOverrideP2 = false;
    bool macroPlaybackActive = false;
    uint32_t macroP1Mask = 0;
    uint32_t macroP2Mask = 0;

    uint32_t dummyDirections = 0;
    uint32_t dummyOwnedAxes = 0;
    uint32_t dummyButtons = 0;
    uint32_t dummyOwnedButtons = 0;
    bool dummyExclusive = false;
    bool guardDuringPlayback = false;

    qinput::SocdPolicy horizontalSocd = qinput::SocdPolicy::Native;
    qinput::SocdPolicy verticalSocd = qinput::SocdPolicy::Native;
};

constexpr uint32_t kTrainingDrivenLaneP1 = 0u;
constexpr uint32_t kTrainingDrivenLaneP2 = 1u;

constexpr uint32_t trainingDrivenLane(bool humanDrivesDummyLane,
                                      bool dummyIsP1 = false) {
    const uint32_t dummyLane =
        dummyIsP1 ? kTrainingDrivenLaneP1 : kTrainingDrivenLaneP2;
    const uint32_t humanLane =
        dummyIsP1 ? kTrainingDrivenLaneP2 : kTrainingDrivenLaneP1;
    return humanDrivesDummyLane ? dummyLane : humanLane;
}

constexpr uint32_t trainingDrivenLane(const TrainingInputStep& step) {
    return trainingDrivenLane(step.humanDrivesDummyLane, step.dummyIsP1);
}

constexpr uint32_t trainingPolicyLane(bool humanDrivesDummyLane,
                                      bool dummyIsP1 = false) {
    return trainingDrivenLane(humanDrivesDummyLane, dummyIsP1)
                   == kTrainingDrivenLaneP1
        ? kTrainingDrivenLaneP2
        : kTrainingDrivenLaneP1;
}

constexpr uint32_t trainingPolicyLane(const TrainingInputStep& step) {
    return trainingPolicyLane(step.humanDrivesDummyLane, step.dummyIsP1);
}

struct TrainingInputResult {
    bool valid = true;
    bool neutralized = true;
    bool duplicateStep = false;
    uint32_t p1Mask = 0;
    uint32_t p2Mask = 0;
    uint32_t drivenLane = kTrainingDrivenLaneP1;
    InputRearmState rearmState = InputRearmState::AwaitNeutral;
    uint32_t postureLane = kTrainingDrivenLaneP2;
    uint32_t postureOwnedAxes = 0;
};

class TrainingInput {
public:
    void reset();

    TrainingActionResult pollActions(uint64_t pollSerial,
                                     const TrainingActionPoll& poll);

    TrainingInputResult advance(uint64_t realStepSerial,
                                const TrainingInputStep& step);

    InputRearmState rearmState() const { return rearmState_; }

    void resumeAfterOperation(uint32_t consumedFightMask);

private:
    struct SocdMemory {
        uint32_t previousRaw = 0;
        uint8_t horizontalWinner = 0;
        uint8_t verticalWinner = 0;
    };

    static uint32_t cleanSocd(uint32_t raw,
                              qinput::SocdPolicy horizontal,
                              qinput::SocdPolicy vertical,
                              SocdMemory& memory);
    void disarmActions(uint32_t currentlyHeld);
    void resetSocd();

    InputRearmState rearmState_ = InputRearmState::AwaitNeutral;
    uint32_t carryMask_ = 0;
    uint32_t previousActions_ = 0;
    bool actionsArmed_ = false;
    bool havePoll_ = false;
    uint64_t lastPollSerial_ = 0;
    TrainingActionResult cachedPoll_{};
    bool haveStep_ = false;
    uint64_t lastStepSerial_ = 0;
    TrainingInputResult cached_{};
    SocdMemory p1Socd_{};
    SocdMemory p2Socd_{};
};

}
