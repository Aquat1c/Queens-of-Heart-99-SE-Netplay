#include "input/ModeControlEvaluator.h"

namespace qinput {

void ModeControlEvaluator::configure(
        const CompiledModeControlsV1* controls) {
    controls_ = controls;
    reset();
}

void ModeControlEvaluator::resetAxisLatches() {
    for (bool& latch : axisLatch_) latch = false;
}

void ModeControlEvaluator::reset() {
    resetAxisLatches();
    armed_ = 0;
    previousHeld_ = 0;
    keyboardGeneration_ = 0;
    controllerGeneration_ = 0;
    focusWasActive_ = false;
    lastKeyboardSerial_ = 0;
    lastControllerSerial_ = 0;
    lastSafeHeld_ = 0;
    haveSample_ = false;
}

ModeControlEvaluation ModeControlEvaluator::evaluate(
        const KeyboardView& keyboard, const ControllerView& controller,
        const FocusView& focus, FocusPolicy focusPolicy,
        uint64_t enabledActions) {
    ModeControlEvaluation result{};
    result.keyboardSampleSerial = keyboard.sampleSerial;
    result.controllerSampleSerial = controller.sampleSerial;
    if (controls_ == nullptr) return result;

    const bool sourceGenerationChanged = !haveSample_
        || keyboard.generation != keyboardGeneration_
        || controller.generation != controllerGeneration_;
    const bool focusStateChanged = !haveSample_
        || focus.active != focusWasActive_;
    keyboardGeneration_ = keyboard.generation;
    controllerGeneration_ = controller.generation;
    focusWasActive_ = focus.active;

    uint64_t physicalHeld = 0;
    for (uint16_t i = 0; i < controls_->count && i < kMaxModeControlBindings;
         ++i) {
        const CompiledModeControlBindingV1& entry = controls_->bindings[i];
        bool held = false;
        if (entry.binding.type == CompiledBindingType::ProbeAxis) {
            held = axisBindingHeld(entry.binding, controller, axisLatch_[i]);
        } else {
            held = digitalBindingHeld(entry.binding, keyboard, controller);
        }
        if (held) physicalHeld |= modeControlBit(entry.action);
    }

    const bool focusAllowed = focusPolicy == FocusPolicy::AllowBackground
        || focus.active;
    if (sourceGenerationChanged || focusStateChanged || !focusAllowed) {
        armed_ &= ~physicalHeld;
        previousHeld_ = physicalHeld;
        haveSample_ = true;
        lastKeyboardSerial_ = keyboard.sampleSerial;
        lastControllerSerial_ = controller.sampleSerial;
        lastSafeHeld_ = 0;
        return result;
    }

    const bool duplicate = haveSample_
        && keyboard.sampleSerial == lastKeyboardSerial_
        && controller.sampleSerial == lastControllerSerial_;
    if (duplicate) {
        result.held = lastSafeHeld_ & enabledActions;
        return result;
    }
    haveSample_ = true;
    lastKeyboardSerial_ = keyboard.sampleSerial;
    lastControllerSerial_ = controller.sampleSerial;

    const uint64_t released = ~physicalHeld;
    armed_ |= released;
    armed_ &= ~(physicalHeld & ~enabledActions);
    const uint64_t safeHeld = physicalHeld & armed_ & enabledActions;
    result.edges = safeHeld & ~previousHeld_;
    result.held = safeHeld;
    previousHeld_ = physicalHeld;
    lastSafeHeld_ = safeHeld;
    return result;
}

}
