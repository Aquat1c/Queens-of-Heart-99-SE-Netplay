#pragma once

#include <cstdint>

#include "input/InputActions.h"
#include "input/InputLaunchConfig.h"

namespace qinput {

int32_t normalizeAxisMagnitudeQ15(int32_t raw,
                                  const CompiledAxisCalibration& calibration,
                                  AxisPolarity polarity);

struct KeyboardView {
    const uint32_t* keyBits = nullptr;
    bool healthy = false;
    uint32_t generation = 0;
    uint32_t sampleSerial = 0;
};

struct ControllerView {
    const uint32_t* probes = nullptr;
    uint32_t probeCount = 0;
    bool healthy = false;
    uint32_t generation = 0;
    uint32_t sampleSerial = 0;
};

struct FocusView {
    bool active = false;
    uint32_t generation = 0;
};

inline bool keyboardKeyHeld(const KeyboardView& view, uint8_t dik) {
    if (view.keyBits == nullptr) return false;
    return (view.keyBits[dik >> 5] >> (dik & 31u)) & 1u;
}

bool digitalBindingHeld(const CompiledBinding& binding,
                        const KeyboardView& keyboard,
                        const ControllerView& controller);

bool axisBindingHeld(const CompiledBinding& binding,
                     const ControllerView& controller,
                     bool& latch);

constexpr uint32_t kSourceStallHoldEvaluations = 30u;

enum class SourceGateState : uint8_t {
    Disconnected = 0,
    AwaitNeutral = 1,
    NeutralSettle = 2,
    Armed = 3,
};

struct EvaluateResult {
    uint16_t mask = 0;
    bool physicalNeutralBeforeSocd = false;
    SourceGateState keyboardGate = SourceGateState::Disconnected;
    SourceGateState controllerGate = SourceGateState::Disconnected;
    bool keyboardStallHeld = false;
    bool controllerStallHeld = false;
};

class InputEvaluator {
public:
    // The config must outlive the evaluator and is immutable after launch.
    void configure(const InputLaunchConfig* config);
    void reset();

    EvaluateResult evaluate(const KeyboardView& keyboard,
                            const ControllerView& controller,
                            const FocusView& focus);

    uint64_t evaluationCount() const { return evaluations_; }

private:
    struct SourceGate {
        SourceGateState state = SourceGateState::Disconnected;
        uint32_t generation = 0;
        uint32_t neutralStreak = 0;
        bool serialBarrier = false;
        uint32_t barrierSerial = 0;
        uint16_t lastArmedMask = 0;
        uint32_t stallEvaluations = 0;
    };

    struct AxisLatch {
        bool active = false;
    };

    AxisLatch latches_[8][kMaxBindingsPerAction] = {};

    bool sourceUsesKeyboard() const;
    bool sourceUsesController() const;

    uint16_t evaluateSourceMask(bool keyboardSource,
                                const KeyboardView& keyboard,
                                const ControllerView& controller,
                                bool advanceLatches);

    void resetSourceLatches(bool keyboardSource);
    void advanceGate(SourceGate& gate, bool healthy, uint32_t generation,
                     uint32_t sampleSerial, bool focusGated,
                     bool physicallyNeutral);

    const InputLaunchConfig* config_ = nullptr;
    SourceGate keyboardGate_{};
    SourceGate controllerGate_{};
    uint32_t focusGeneration_ = 0;
    bool focusWasActive_ = false;

    uint8_t lastHorizontalWinner_ = 0;
    uint8_t lastVerticalWinner_ = 0;
    uint16_t previousMergedMask_ = 0;

    uint64_t evaluations_ = 0;
};

}
