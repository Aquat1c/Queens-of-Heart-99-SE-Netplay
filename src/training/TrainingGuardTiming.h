#pragma once

#include <cstdint>

namespace qtraining {

enum class TrainingGuardTimingMode : uint32_t {
    Always = 0,
    AfterBeingHit = 1,
    UntilFirstBlock = 2,
    Count = 3,
};

constexpr uint32_t kTrainingGuardTimingWindowMin = 1u;
constexpr uint32_t kTrainingGuardTimingWindowMax = 600u;
constexpr uint32_t kTrainingGuardTimingWindowDefault = 60u;

constexpr bool guardTimingModeValid(uint32_t value) {
    return value < static_cast<uint32_t>(TrainingGuardTimingMode::Count);
}

constexpr bool guardTimingWindowValid(uint32_t value) {
    return value >= kTrainingGuardTimingWindowMin
        && value <= kTrainingGuardTimingWindowMax;
}

struct TrainingGuardTimingConfig {
    TrainingGuardTimingMode mode = TrainingGuardTimingMode::Always;
    uint32_t windowSteps = kTrainingGuardTimingWindowDefault;
};

struct TrainingGuardTimingState {
    uint32_t armedCountdown = 0u;
    bool blockSpent = false;
    uint32_t neutralSteps = 0u;
};

struct TrainingGuardTimingStep {
    bool dummyWasHit = false;
    bool dummyBlocked = false;
    bool dummyNeutral = false;
};

inline bool guardTimingAdvance(const TrainingGuardTimingConfig& config,
                               TrainingGuardTimingState& state,
                               const TrainingGuardTimingStep& step) {
    switch (config.mode) {
        case TrainingGuardTimingMode::AfterBeingHit: {
            if (step.dummyWasHit
                || (step.dummyBlocked && state.armedCountdown != 0u)) {
                state.armedCountdown = config.windowSteps;
            }
            const bool armed = state.armedCountdown != 0u;
            if (armed && step.dummyNeutral) --state.armedCountdown;
            state.blockSpent = false;
            state.neutralSteps = 0u;
            return armed;
        }
        case TrainingGuardTimingMode::UntilFirstBlock: {
            state.armedCountdown = 0u;
            if (!state.blockSpent) {
                if (step.dummyBlocked) {
                    state.blockSpent = true;
                    state.neutralSteps = 0u;
                    return true;
                }
                return true;
            }
            state.neutralSteps =
                step.dummyNeutral ? state.neutralSteps + 1u : 0u;
            if (state.neutralSteps >= config.windowSteps) {
                state.blockSpent = false;
                state.neutralSteps = 0u;
                return true;
            }
            return false;
        }
        case TrainingGuardTimingMode::Always:
        default:
            state = TrainingGuardTimingState{};
            return true;
    }
}

}
