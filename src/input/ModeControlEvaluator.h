#pragma once

#include <cstdint>

#include "input/BindingEvaluator.h"

namespace qinput {

constexpr uint64_t modeControlBit(ModeControlAction action) {
    return 1ull << static_cast<uint16_t>(action);
}

struct ModeControlEvaluation {
    uint64_t edges = 0;
    uint64_t held = 0;
    uint32_t keyboardSampleSerial = 0;
    uint32_t controllerSampleSerial = 0;
};

class ModeControlEvaluator {
public:
    void configure(const CompiledModeControlsV1* controls);
    void reset();
    void resetAxisLatches();

    ModeControlEvaluation evaluate(const KeyboardView& keyboard,
                                   const ControllerView& controller,
                                   const FocusView& focus,
                                   FocusPolicy focusPolicy,
                                   uint64_t enabledActions = ~0ull);

private:
    const CompiledModeControlsV1* controls_ = nullptr;
    bool axisLatch_[kMaxModeControlBindings] = {};
    uint64_t armed_ = 0;
    uint64_t previousHeld_ = 0;
    uint32_t keyboardGeneration_ = 0;
    uint32_t controllerGeneration_ = 0;
    bool focusWasActive_ = false;
    uint32_t lastKeyboardSerial_ = 0;
    uint32_t lastControllerSerial_ = 0;
    uint64_t lastSafeHeld_ = 0;
    bool haveSample_ = false;
};

}
