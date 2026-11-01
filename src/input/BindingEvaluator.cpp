#include "input/BindingEvaluator.h"

namespace qinput {

int32_t normalizeAxisMagnitudeQ15(int32_t raw,
                                  const CompiledAxisCalibration& calibration,
                                  AxisPolarity polarity) {
    int64_t signedQ15 = 0;
    if (calibration.unipolar != 0) {
        if (calibration.minimum >= calibration.maximum) return 0;
        if (polarity != AxisPolarity::Positive) return 0;
        int64_t clamped = raw;
        if (clamped < calibration.minimum) clamped = calibration.minimum;
        if (clamped > calibration.maximum) clamped = calibration.maximum;
        signedQ15 = (clamped - calibration.minimum) * 32767ll
                    / (static_cast<int64_t>(calibration.maximum)
                       - calibration.minimum);
        if (calibration.invert != 0) signedQ15 = 32767ll - signedQ15;
    } else {
        if (!(calibration.minimum < calibration.center
              && calibration.center < calibration.maximum)) {
            return 0;
        }
        int64_t clamped = raw;
        if (clamped < calibration.minimum) clamped = calibration.minimum;
        if (clamped > calibration.maximum) clamped = calibration.maximum;
        if (clamped >= calibration.center) {
            signedQ15 = (clamped - calibration.center) * 32767ll
                        / (static_cast<int64_t>(calibration.maximum)
                           - calibration.center);
        } else {
            signedQ15 = -((static_cast<int64_t>(calibration.center) - clamped)
                          * 32767ll
                          / (static_cast<int64_t>(calibration.center)
                             - calibration.minimum));
        }
        if (calibration.invert != 0) signedQ15 = -signedQ15;
        if (polarity == AxisPolarity::Negative) signedQ15 = -signedQ15;
    }
    int64_t magnitude = signedQ15 < 0 ? 0 : signedQ15;
    if (magnitude > 32767) magnitude = 32767;
    if (magnitude <= calibration.deadzoneQ15) magnitude = 0;
    return static_cast<int32_t>(magnitude);
}

namespace {

int32_t probeValue(const ControllerView& controller, uint8_t probeIndex) {
    if (controller.probes == nullptr
        || probeIndex >= controller.probeCount
        || probeIndex >= kMaxControllerProbes) {
        return 0;
    }
    return static_cast<int32_t>(controller.probes[probeIndex]);
}

}

bool digitalBindingHeld(const CompiledBinding& binding,
                        const KeyboardView& keyboard,
                        const ControllerView& controller) {
    switch (binding.type) {
        case CompiledBindingType::KeyboardScan:
            return keyboard.healthy
                && keyboardKeyHeld(keyboard, binding.dik);
        case CompiledBindingType::ProbeButton:
            return controller.healthy
                && probeValue(controller, binding.probeIndex) != 0;
        case CompiledBindingType::ProbeHat:
            return controller.healthy
                && (static_cast<uint32_t>(
                        probeValue(controller, binding.probeIndex))
                    & binding.hatBit) != 0;
        case CompiledBindingType::ProbeAxis:
        case CompiledBindingType::None:
            return false;
    }
    return false;
}

bool axisBindingHeld(const CompiledBinding& binding,
                     const ControllerView& controller,
                     bool& latch) {
    if (binding.type != CompiledBindingType::ProbeAxis
        || binding.calibration.unipolar == 0
        || !controller.healthy) {
        latch = false;
        return false;
    }
    const int32_t magnitude = normalizeAxisMagnitudeQ15(
        probeValue(controller, binding.probeIndex),
        binding.calibration, binding.polarity);
    if (!latch && magnitude >= binding.pressQ15) latch = true;
    else if (latch && magnitude <= binding.releaseQ15) latch = false;
    return latch;
}

void InputEvaluator::configure(const InputLaunchConfig* config) {
    config_ = config;
    reset();
}

void InputEvaluator::reset() {
    for (auto& perAction : latches_)
        for (auto& latch : perAction) latch = AxisLatch{};
    keyboardGate_ = SourceGate{};
    controllerGate_ = SourceGate{};
    focusGeneration_ = 0;
    focusWasActive_ = false;
    lastHorizontalWinner_ = 0;
    lastVerticalWinner_ = 0;
    previousMergedMask_ = 0;
    evaluations_ = 0;
}

bool InputEvaluator::sourceUsesKeyboard() const {
    return config_ != nullptr
        && config_->sourceMode != CompiledSourceMode::Controller;
}

bool InputEvaluator::sourceUsesController() const {
    return config_ != nullptr
        && config_->sourceMode != CompiledSourceMode::Keyboard;
}

void InputEvaluator::resetSourceLatches(bool keyboardSource) {
    if (config_ == nullptr) return;
    for (uint8_t action = 0; action < 8; ++action) {
        const CompiledActionBindings& bindings = config_->actions[action];
        for (uint8_t i = 0; i < bindings.count
                            && i < kMaxBindingsPerAction; ++i) {
            const bool isKeyboardBinding =
                bindings.bindings[i].type
                == CompiledBindingType::KeyboardScan;
            if (isKeyboardBinding == keyboardSource)
                latches_[action][i] = AxisLatch{};
        }
    }
    lastHorizontalWinner_ = 0;
    lastVerticalWinner_ = 0;
}

uint16_t InputEvaluator::evaluateSourceMask(bool keyboardSource,
                                            const KeyboardView& keyboard,
                                            const ControllerView& controller,
                                            bool advanceLatches) {
    uint16_t mask = 0;
    for (uint8_t action = 0; action < 8; ++action) {
        const CompiledActionBindings& bindings = config_->actions[action];
        bool held = false;
        for (uint8_t i = 0; i < bindings.count
                            && i < kMaxBindingsPerAction; ++i) {
            const CompiledBinding& binding = bindings.bindings[i];
            const bool isKeyboardBinding =
                binding.type == CompiledBindingType::KeyboardScan;
            if (isKeyboardBinding != keyboardSource) continue;
            switch (binding.type) {
                case CompiledBindingType::KeyboardScan:
                    held = held || keyboardKeyHeld(keyboard, binding.dik);
                    break;
                case CompiledBindingType::ProbeButton:
                    held = held
                        || probeValue(controller, binding.probeIndex) != 0;
                    break;
                case CompiledBindingType::ProbeHat:
                    held = held
                        || (static_cast<uint32_t>(probeValue(
                                controller, binding.probeIndex))
                            & binding.hatBit) != 0;
                    break;
                case CompiledBindingType::ProbeAxis: {
                    const int32_t magnitude = normalizeAxisMagnitudeQ15(
                        probeValue(controller, binding.probeIndex),
                        binding.calibration, binding.polarity);
                    AxisLatch& latch = latches_[action][i];
                    if (advanceLatches) {
                        if (!latch.active
                            && magnitude >= binding.pressQ15) {
                            latch.active = true;
                        } else if (latch.active
                                   && magnitude <= binding.releaseQ15) {
                            latch.active = false;
                        }
                        held = held || latch.active;
                    } else {
                        held = held || magnitude >= binding.pressQ15;
                    }
                    break;
                }
                case CompiledBindingType::None:
                    break;
            }
        }
        if (held) {
            mask |= compactBit(static_cast<FightAction>(action));
        }
    }
    return mask;
}

void InputEvaluator::advanceGate(SourceGate& gate, bool healthy,
                                 uint32_t generation, uint32_t sampleSerial,
                                 bool focusGated, bool physicallyNeutral) {
    if (!healthy || focusGated) {
        gate.state = SourceGateState::Disconnected;
        gate.neutralStreak = 0;
        gate.serialBarrier = true;
        gate.barrierSerial = sampleSerial;
        gate.generation = generation;
        return;
    }
    if (gate.state == SourceGateState::Disconnected
        || gate.generation != generation) {
        gate.state = SourceGateState::AwaitNeutral;
        gate.generation = generation;
        gate.neutralStreak = 0;
    }
    if (gate.serialBarrier) {
        if (sampleSerial == gate.barrierSerial) {
            gate.neutralStreak = 0;
            return;
        }
        gate.serialBarrier = false;
    }
    switch (gate.state) {
        case SourceGateState::AwaitNeutral:
            if (physicallyNeutral) {
                if (++gate.neutralStreak >= 2)
                    gate.state = SourceGateState::NeutralSettle;
            } else {
                gate.neutralStreak = 0;
            }
            break;
        case SourceGateState::NeutralSettle:
            if (physicallyNeutral) {
                gate.state = SourceGateState::Armed;
            } else {
                gate.state = SourceGateState::AwaitNeutral;
                gate.neutralStreak = 0;
            }
            break;
        case SourceGateState::Armed:
        case SourceGateState::Disconnected:
            break;
    }
}

EvaluateResult InputEvaluator::evaluate(const KeyboardView& keyboard,
                                        const ControllerView& controller,
                                        const FocusView& focus) {
    EvaluateResult result{};
    if (config_ == nullptr) return result;
    ++evaluations_;

    const bool focusGates =
        config_->focusPolicy == FocusPolicy::GameWindowOnly
        && !focus.active;
    if (focus.generation != focusGeneration_
        || focus.active != focusWasActive_) {
        focusGeneration_ = focus.generation;
        focusWasActive_ = focus.active;
    }

    const bool useKeyboard = sourceUsesKeyboard();
    const bool useController = sourceUsesController();

    if (useKeyboard && keyboardGate_.generation != keyboard.generation)
        resetSourceLatches(true);
    if (useController
        && controllerGate_.generation != controller.generation)
        resetSourceLatches(false);
    if (focusGates) {
        if (useKeyboard) resetSourceLatches(true);
        if (useController) resetSourceLatches(false);
    }

    const auto stalled = [&](const SourceGate& gate, bool healthy,
                             uint32_t generation) {
        return !healthy && !focusGates
            && gate.state == SourceGateState::Armed
            && gate.generation == generation
            && gate.stallEvaluations < kSourceStallHoldEvaluations;
    };
    const bool keyboardStalled =
        useKeyboard && stalled(keyboardGate_, keyboard.healthy,
                               keyboard.generation);
    const bool controllerStalled =
        useController && stalled(controllerGate_, controller.healthy,
                                 controller.generation);

    uint16_t keyboardMask = 0;
    uint16_t controllerMask = 0;
    bool keyboardNeutral = true;
    bool controllerNeutral = true;
    if (useKeyboard && keyboard.healthy) {
        const uint16_t observed = evaluateSourceMask(
            true, keyboard, controller,
            keyboardGate_.state == SourceGateState::Armed && !focusGates);
        keyboardNeutral = observed == 0;
        if (keyboardGate_.state == SourceGateState::Armed && !focusGates) {
            keyboardMask = observed;
            keyboardGate_.lastArmedMask = observed;
            keyboardGate_.stallEvaluations = 0;
        }
    } else if (keyboardStalled) {
        keyboardMask = keyboardGate_.lastArmedMask;
        ++keyboardGate_.stallEvaluations;
    }
    if (useController && controller.healthy) {
        const uint16_t observed = evaluateSourceMask(
            false, keyboard, controller,
            controllerGate_.state == SourceGateState::Armed && !focusGates);
        controllerNeutral = observed == 0;
        if (controllerGate_.state == SourceGateState::Armed && !focusGates) {
            controllerMask = observed;
            controllerGate_.lastArmedMask = observed;
            controllerGate_.stallEvaluations = 0;
        }
    } else if (controllerStalled) {
        controllerMask = controllerGate_.lastArmedMask;
        ++controllerGate_.stallEvaluations;
    }

    result.physicalNeutralBeforeSocd =
        (!useKeyboard || !keyboard.healthy || keyboardNeutral)
        && (!useController || !controller.healthy || controllerNeutral);

    if (useKeyboard && !keyboardStalled) {
        advanceGate(keyboardGate_, keyboard.healthy, keyboard.generation,
                    keyboard.sampleSerial, focusGates, keyboardNeutral);
    }
    if (useController && !controllerStalled) {
        advanceGate(controllerGate_, controller.healthy,
                    controller.generation, controller.sampleSerial,
                    focusGates, controllerNeutral);
    }
    result.keyboardGate = keyboardGate_.state;
    result.controllerGate = controllerGate_.state;
    result.keyboardStallHeld = keyboardStalled;
    result.controllerStallHeld = controllerStalled;

    uint16_t merged = keyboardMask | controllerMask;

    const uint16_t pressEdges =
        static_cast<uint16_t>(merged & ~previousMergedMask_);
    previousMergedMask_ = merged;

    const bool left = merged & kCompactLeft;
    const bool right = merged & kCompactRight;
    if (left && right) {
        switch (config_->horizontalSocd) {
            case SocdPolicy::Native:
                break;
            case SocdPolicy::Neutral:
            case SocdPolicy::UpPriority:
                merged &= static_cast<uint16_t>(
                    ~(kCompactLeft | kCompactRight));
                break;
            case SocdPolicy::LastInput: {
                uint8_t winner = lastHorizontalWinner_;
                if ((pressEdges & kCompactLeft)
                    && !(pressEdges & kCompactRight)) {
                    winner = 1;
                } else if ((pressEdges & kCompactRight)
                           && !(pressEdges & kCompactLeft)) {
                    winner = 2;
                } else if ((pressEdges
                            & (kCompactLeft | kCompactRight))
                               == (kCompactLeft | kCompactRight)
                           && winner == 0) {
                    winner = 0;
                }
                merged &= static_cast<uint16_t>(
                    ~(kCompactLeft | kCompactRight));
                if (winner == 1) merged |= kCompactLeft;
                if (winner == 2) merged |= kCompactRight;
                break;
            }
        }
    }
    if (left != right) {
        lastHorizontalWinner_ = left ? 1 : 2;
    } else if (!left && !right) {
        lastHorizontalWinner_ = 0;
    } else if (config_->horizontalSocd == SocdPolicy::LastInput) {
        if ((pressEdges & kCompactLeft) && !(pressEdges & kCompactRight))
            lastHorizontalWinner_ = 1;
        else if ((pressEdges & kCompactRight)
                 && !(pressEdges & kCompactLeft))
            lastHorizontalWinner_ = 2;
    }

    const bool up = merged & kCompactUp;
    const bool down = merged & kCompactDown;
    if (up && down) {
        switch (config_->verticalSocd) {
            case SocdPolicy::Native:
                break;
            case SocdPolicy::Neutral:
                merged &= static_cast<uint16_t>(
                    ~(kCompactUp | kCompactDown));
                break;
            case SocdPolicy::UpPriority:
                merged &= static_cast<uint16_t>(~kCompactDown);
                break;
            case SocdPolicy::LastInput: {
                uint8_t winner = lastVerticalWinner_;
                if ((pressEdges & kCompactUp)
                    && !(pressEdges & kCompactDown)) {
                    winner = 1;
                } else if ((pressEdges & kCompactDown)
                           && !(pressEdges & kCompactUp)) {
                    winner = 2;
                }
                merged &= static_cast<uint16_t>(
                    ~(kCompactUp | kCompactDown));
                if (winner == 1) merged |= kCompactUp;
                if (winner == 2) merged |= kCompactDown;
                break;
            }
        }
    }
    if (up != down) {
        lastVerticalWinner_ = up ? 1 : 2;
    } else if (!up && !down) {
        lastVerticalWinner_ = 0;
    }

    result.mask = merged & kCanonicalFightMask;
    return result;
}

namespace {

const char* validateBinding(const CompiledBinding& binding,
                            const CompiledProbe* probes, uint32_t probeCount,
                            bool controlPlane) {
    switch (binding.type) {
        case CompiledBindingType::None:
            return "empty binding slot inside count";
        case CompiledBindingType::KeyboardScan:
            if (binding.dik == 0) return "zero DIK";
            return nullptr;
        case CompiledBindingType::ProbeButton:
            if (binding.probeIndex >= probeCount)
                return "probe index out of range";
            return nullptr;
        case CompiledBindingType::ProbeHat:
            if (binding.probeIndex >= probeCount)
                return "probe index out of range";
            if (binding.hatBit != kHatUp && binding.hatBit != kHatDown
                && binding.hatBit != kHatLeft
                && binding.hatBit != kHatRight) {
                return "invalid hat direction bit";
            }
            return nullptr;
        case CompiledBindingType::ProbeAxis: {
            if (binding.probeIndex >= probeCount)
                return "probe index out of range";
            const CompiledAxisCalibration& c = binding.calibration;
            if (controlPlane) {
                if (probes == nullptr) return "probe table missing";
                const CompiledProbe& probe = probes[binding.probeIndex];
                if (!probeAxisIsAnalogTrigger(probe.kind, probe.token)) {
                    return "stick predicate on the control plane; use a "
                           "button, the d-pad, or an analog trigger";
                }
                if (c.unipolar == 0)
                    return "trigger on the control plane is not unipolar";
            }
            if (binding.pressQ15 == 0 || binding.pressQ15 > kAxisScale)
                return "press threshold out of range";
            if (binding.releaseQ15 >= binding.pressQ15)
                return "release threshold not below press";
            if (c.deadzoneQ15 > binding.releaseQ15)
                return "deadzone above release threshold";
            if (c.unipolar != 0) {
                if (c.minimum >= c.maximum)
                    return "unipolar minimum >= maximum";
                if (binding.polarity != AxisPolarity::Positive)
                    return "unipolar Negative polarity";
            } else {
                if (!(c.minimum < c.center && c.center < c.maximum))
                    return "bipolar calibration order";
            }
            if (c.invert > 1) return "invert flag out of range";
            return nullptr;
        }
    }
    return "unknown binding type";
}

}

const char* validateInputLaunchConfig(const InputLaunchConfig& config,
                                      bool spectator) {
    if (config.schemaVersion != kInputConfigSchemaVersion)
        return "input-config schema version mismatch";
    if (config.probeCount > kMaxControllerProbes)
        return "probe count above hard limit";
    if (static_cast<uint32_t>(config.probeBackend)
        > static_cast<uint32_t>(CompiledProbeBackend::DirectInput8)) {
        return "invalid controller probe backend";
    }
    if (spectator) {
        if (config.probeCount != 0) return "spectator with probes";
        for (const CompiledActionBindings& action : config.actions)
            if (action.count != 0) return "spectator with fighter bindings";
        return nullptr;
    }
    switch (config.sourceMode) {
        case CompiledSourceMode::Keyboard:
        case CompiledSourceMode::Controller:
        case CompiledSourceMode::KeyboardAndController:
            break;
        default:
            return "invalid source mode";
    }
    if (static_cast<uint8_t>(config.horizontalSocd) > 3
        || static_cast<uint8_t>(config.verticalSocd) > 3) {
        return "invalid SOCD policy";
    }
    if (static_cast<uint8_t>(config.focusPolicy) > 1)
        return "invalid focus policy";
    for (uint32_t i = 0; i < config.probeCount; ++i) {
        if (config.probes[i].kind == ProbeKind::None)
            return "empty probe slot inside count";
    }
    for (const CompiledActionBindings& action : config.actions) {
        if (action.count > kMaxBindingsPerAction)
            return "fighter binding count above limit";
        for (uint8_t i = 0; i < action.count; ++i) {
            const char* why = validateBinding(
                action.bindings[i], config.probes, config.probeCount, false);
            if (why != nullptr) return why;
        }
    }
    if (config.mappingLength > kMaxMappingStringLength)
        return "mapping string above limit";
    if (config.mappingLength > 0
        && config.mapping[config.mappingLength - 1] == '\0'
        && config.mappingLength != 1) {
    }
    for (uint32_t i = 0; i + 1 < config.mappingLength; ++i)
        if (config.mapping[i] == '\0') return "embedded NUL in mapping";
    if (config.probeCount > 0) {
        bool hasGamepadProbe = false;
        for (uint32_t i = 0; i < config.probeCount; ++i) {
            if (config.probes[i].kind == ProbeKind::GamepadButton
                || config.probes[i].kind == ProbeKind::GamepadAxis) {
                hasGamepadProbe = true;
            }
        }
        if (hasGamepadProbe && config.mappingLength == 0)
            return "standardized probes without a mapping identity";
        if (hasGamepadProbe
            && config.probeBackend == CompiledProbeBackend::DirectInput8) {
            return "standardized probes with a DirectInput index space";
        }
        if (config.device.guid[0] == '\0')
            return "controller probes without a device matcher";
    } else if (config.probeBackend != CompiledProbeBackend::Unknown) {
        return "controller probe backend without probes";
    }
    return nullptr;
}

const char* validateCompiledModeControls(
        const CompiledModeControlsV1& controls,
        const InputLaunchConfig& input,
        bool spectator) {
    if (controls.structSize != sizeof(CompiledModeControlsV1))
        return "mode-control struct size mismatch";
    if (controls.version != 1u)
        return "mode-control version mismatch";
    if (controls.count > kMaxModeControlBindings)
        return "mode-control binding count above limit";
    if (spectator && controls.count != 0)
        return "spectator with runtime mode controls";
    for (uint16_t i = 0; i < controls.count; ++i) {
        const CompiledModeControlBindingV1& entry = controls.bindings[i];
        const ModeControlDescriptor* descriptor =
            modeControlDescriptor(entry.action);
        if (descriptor == nullptr) return "invalid mode-control action";
        if (descriptor->domain == ModeControlDomain::WrapperGlobal
            || descriptor->domain == ModeControlDomain::Replay) {
            return "wrapper-owned action in injected runtime table";
        }
        const char* why = validateBinding(entry.binding, input.probes,
                                          input.probeCount, true);
        if (why != nullptr) return why;
        if (descriptor->domain == ModeControlDomain::Training
            && entry.binding.type == CompiledBindingType::KeyboardScan
            && trainingDikForbidden(entry.binding.dik)) {
            return "reserved Training physical key";
        }
    }
    return nullptr;
}

}
