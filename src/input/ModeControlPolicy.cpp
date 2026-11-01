#include "input/ModeControlPolicy.h"

#include <algorithm>

namespace qinput {
namespace {

ProfileBinding key(uint8_t dik) {
    ProfileBinding binding{};
    binding.type = CompiledBindingType::KeyboardScan;
    binding.dik = dik;
    return binding;
}

void add(ControlsFile& file, ModeControlAction action, uint8_t dik) {
    file.modeControls[static_cast<size_t>(action)].push_back(key(dik));
}

ProfileBinding padButton(uint8_t sdlGamepadButton) {
    ProfileBinding binding{};
    binding.type = CompiledBindingType::ProbeButton;
    binding.probeKind = ProbeKind::GamepadButton;
    binding.token = sdlGamepadButton;
    return binding;
}

void addPad(ControlsFile& file, ModeControlAction action,
            uint8_t sdlGamepadButton) {
    file.modeControls[static_cast<size_t>(action)].push_back(
        padButton(sdlGamepadButton));
}

bool actionAcceptsController(ModeControlAction action) {
    const ModeControlDescriptor* descriptor = modeControlDescriptor(action);
    return descriptor != nullptr
        && descriptor->devicePolicy
            == ModeControlDevicePolicy::KeyboardOrSelectedControllerDigital;
}

bool actionOverlapsFight(ModeControlAction action) {
    const ModeControlDescriptor* descriptor = modeControlDescriptor(action);
    return descriptor != nullptr
        && modeDomainOverlapsFighter(descriptor->domain);
}

bool actionsOverlap(ModeControlAction a, ModeControlAction b) {
    const ModeControlDescriptor* da = modeControlDescriptor(a);
    const ModeControlDescriptor* db = modeControlDescriptor(b);
    return da != nullptr && db != nullptr
        && modeDomainsOverlap(da->domain, db->domain);
}

template <class Bindings>
void erasePhysical(Bindings& bindings, const ProfileBinding& target) {
    bindings.erase(
        std::remove_if(bindings.begin(), bindings.end(),
                       [&](const ProfileBinding& current) {
                           return samePhysicalBinding(current, target);
                       }),
        bindings.end());
}

}

void installDefaultModeControls(ControlsFile& file) {
    for (size_t i = 0; i < static_cast<size_t>(ModeControlAction::Count); ++i)
        file.modeControls[i].clear();

    add(file, ModeControlAction::TogglePresentationMode, 0x57);
    add(file, ModeControlAction::ToggleOverlayMenu, 0xD2);
    add(file, ModeControlAction::ToggleInputViewer, 0x0E);
    addPad(file, ModeControlAction::OpenOverlayMenuPad, 6);

    add(file, ModeControlAction::InputDelayDecrease, 0x0C);
    add(file, ModeControlAction::InputDelayIncrease, 0x0D);
    add(file, ModeControlAction::RollbackWindowDecrease, 0x1A);
    add(file, ModeControlAction::RollbackWindowIncrease, 0x1B);
    add(file, ModeControlAction::OnlineReturnCharacterSelect, 0x3B);

    add(file, ModeControlAction::ReplayPause, 0x39);
    add(file, ModeControlAction::ReplayRestart, 0xC7);
    add(file, ModeControlAction::ReplayStepBack, 0xCB);
    add(file, ModeControlAction::ReplayStepBack, 0x33);
    add(file, ModeControlAction::ReplayStep, 0xCD);
    add(file, ModeControlAction::ReplayStep, 0x34);
    add(file, ModeControlAction::ReplaySlower, 0x0C);
    add(file, ModeControlAction::ReplaySlower, 0x4A);
    add(file, ModeControlAction::ReplayFaster, 0x0D);
    add(file, ModeControlAction::ReplayFaster, 0x4E);
    add(file, ModeControlAction::ReplayTakeoverExit, 0x10);
    add(file, ModeControlAction::ReplayTakeoverP1, 0x21);
    add(file, ModeControlAction::ReplayTakeoverP2, 0x22);
    add(file, ModeControlAction::ReplayTakeoverRetry, 0x13);
    add(file, ModeControlAction::ReplayTakeoverDelayDecrease, 0x0A);
    add(file, ModeControlAction::ReplayTakeoverDelayIncrease, 0x0B);
    add(file, ModeControlAction::ReplayToggleCollisionDisplay, 0x41);

    add(file, ModeControlAction::TrainingResetPosition, 0x02);
    add(file, ModeControlAction::TrainingSaveState, 0x03);
    add(file, ModeControlAction::TrainingLoadState, 0x04);
    add(file, ModeControlAction::TrainingRecordToggle, 0x05);
    add(file, ModeControlAction::TrainingPlaybackToggle, 0x06);
    add(file, ModeControlAction::TrainingPreviousSlot, 0x07);
    add(file, ModeControlAction::TrainingNextSlot, 0x08);
    add(file, ModeControlAction::TrainingPause, 0x09);
    add(file, ModeControlAction::TrainingPause, 0x42);
    add(file, ModeControlAction::TrainingFrameStep, 0x0A);
    add(file, ModeControlAction::TrainingFrameStep, 0x43);
    add(file, ModeControlAction::TrainingReturnCharacterSelect, 0x3B);
    add(file, ModeControlAction::TrainingCycleStance, 0x3C);
    add(file, ModeControlAction::TrainingCycleGuard, 0x3D);
    add(file, ModeControlAction::TrainingRefill, 0x3E);
    add(file, ModeControlAction::TrainingChargeExFraction, 0x3F);
    add(file, ModeControlAction::TrainingResetRoundStart, 0x40);
    add(file, ModeControlAction::TrainingToggleHitboxes, 0x41);
}

bool samePhysicalBinding(const ProfileBinding& a, const ProfileBinding& b) {
    if (a.type != b.type) return false;
    if (a.type == CompiledBindingType::KeyboardScan)
        return a.dik == b.dik;
    if (a.type == CompiledBindingType::ProbeButton)
        return a.probeKind == b.probeKind && a.token == b.token;
    if (a.type == CompiledBindingType::ProbeHat)
        return a.probeKind == b.probeKind && a.token == b.token
            && a.hatBit == b.hatBit;
    if (a.type == CompiledBindingType::ProbeAxis)
        return a.probeKind == b.probeKind && a.token == b.token
            && a.polarity == b.polarity;
    return false;
}

bool validModeControlBinding(ModeControlAction action,
                             const ProfileBinding& binding,
                             std::string& error) {
    const ModeControlDescriptor* descriptor = modeControlDescriptor(action);
    if (descriptor == nullptr) {
        error = "unknown mode-control action";
        return false;
    }
    if (binding.type == CompiledBindingType::KeyboardScan) {
        if (binding.dik == 0) {
            error = "keyboard binding has no physical DIK";
            return false;
        }
        if (descriptor->domain == ModeControlDomain::Training
            && trainingDikForbidden(binding.dik)) {
            error = "that physical key is reserved from Training";
            return false;
        }
        return true;
    }
    if (!actionAcceptsController(action)) {
        error = "this action is keyboard-only";
        return false;
    }
    if (binding.type == CompiledBindingType::ProbeAxis) {
        if (!probeAxisIsAnalogTrigger(binding.probeKind, binding.token)) {
            error = "sticks and unidentified raw axes cannot be bound here; "
                    "use a button, the d-pad, or L2/R2 from a standardized "
                    "controller layout";
            return false;
        }
        if (binding.polarity != AxisPolarity::Positive) {
            error = "a trigger binding must use positive polarity";
            return false;
        }
        if (binding.pressQ15 <= binding.releaseQ15) {
            error = "a trigger binding needs a press threshold above its "
                    "release threshold";
            return false;
        }
        return true;
    }
    if (binding.type != CompiledBindingType::ProbeButton
        && binding.type != CompiledBindingType::ProbeHat) {
        error = "unsupported mode-control binding";
        return false;
    }
    if (binding.type == CompiledBindingType::ProbeHat
        && binding.hatBit != kHatUp && binding.hatBit != kHatRight
        && binding.hatBit != kHatDown && binding.hatBit != kHatLeft) {
        error = "hat binding requires one canonical direction";
        return false;
    }
    return true;
}

std::vector<ModeControlConflict> findModeControlConflicts(
        const ControlsFile& file, ModeControlAction destination,
        const ProfileBinding& binding) {
    std::vector<ModeControlConflict> result;
    if (!isCanonicalModeControlAction(destination)) return result;

    if (actionOverlapsFight(destination)) {
        for (size_t p = 0; p < file.profiles.size(); ++p) {
            for (uint32_t action = 0; action < 8; ++action) {
                for (const ProfileBinding& candidate
                     : file.profiles[p].actions[action]) {
                    if (samePhysicalBinding(candidate, binding)) {
                        result.push_back({true, p, action,
                                          ModeControlAction::None});
                    }
                }
            }
        }
    }
    for (const ModeControlDescriptor& descriptor : kModeControlDescriptors) {
        if (descriptor.action == destination
            || !actionsOverlap(destination, descriptor.action)) continue;
        const auto& candidates =
            file.modeControls[static_cast<size_t>(descriptor.action)];
        for (const ProfileBinding& candidate : candidates) {
            if (samePhysicalBinding(candidate, binding)) {
                result.push_back({false, 0, 0, descriptor.action});
                break;
            }
        }
    }
    return result;
}

bool moveModeControlBinding(ControlsFile& file,
                            ModeControlAction destination,
                            const ProfileBinding& binding,
                            std::string& error) {
    if (!validModeControlBinding(destination, binding, error)) return false;
    ControlsFile next = file;
    const std::vector<ModeControlConflict> conflicts =
        findModeControlConflicts(next, destination, binding);
    for (const ModeControlConflict& conflict : conflicts) {
        if (conflict.fighter) {
            erasePhysical(next.profiles[conflict.profileIndex]
                              .actions[conflict.fightAction], binding);
        } else {
            erasePhysical(next.modeControls[static_cast<size_t>(
                              conflict.modeAction)], binding);
        }
    }
    auto& destinationBindings =
        next.modeControls[static_cast<size_t>(destination)];
    bool alreadyPresent = false;
    for (const ProfileBinding& candidate : destinationBindings)
        alreadyPresent |= samePhysicalBinding(candidate, binding);
    if (!alreadyPresent) destinationBindings.push_back(binding);
    file = std::move(next);
    return true;
}

const char* modeControlRuntimeCapabilityReason(ModeControlAction action,
                                               bool runtimeModeControls,
                                               bool meterCharge) {
    const ModeControlDescriptor* descriptor = modeControlDescriptor(action);
    if (descriptor == nullptr) return "Unknown action";
    if ((descriptor->domain == ModeControlDomain::OnlinePlayer
         || descriptor->domain == ModeControlDomain::Training)
        && !runtimeModeControls) {
        return "The injected runtime does not support shared mode controls";
    }
    if (action == ModeControlAction::TrainingChargeExFraction
        && !meterCharge) {
        return "The injected runtime does not support fractional EX charge";
    }
    return nullptr;
}

}
