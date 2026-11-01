#pragma once

#include <cstdint>

#include "input/InputActions.h"
#include "input/InputLaunchConfig.h"

namespace qinput {

constexpr uint32_t kNativeBindingPlayers = 4;
constexpr uint32_t kNativeBindingActions = 8;
constexpr uint32_t kNativeDescriptorBytes = 12;
constexpr uint32_t kNativeBindingTableBytes =
    kNativeBindingPlayers * kNativeBindingActions * kNativeDescriptorBytes;

constexpr uint32_t kNativeSourceKeyboard = 0;

struct NativeBindingDescriptor {
    uint32_t sourceType = kNativeSourceKeyboard;
    uint32_t codeOrMask = 0;
    uint32_t callback = 0;
};

struct NativeBindingPlan {
    NativeBindingDescriptor actions[kNativeBindingActions];
    uint8_t unexpressedMask = 0;
    bool anyExpressed = false;
};

// keyboardCallback is the SE address of the keyboard binding callback (sub_484B00), which sub_484B50 installs for keyboard descriptors; a descriptor must always carry its matching callback.
constexpr NativeBindingPlan buildNativeBindingPlan(
    const InputLaunchConfig& config, uint32_t keyboardCallback) {
    NativeBindingPlan plan{};
    for (uint32_t action = 0; action < kNativeBindingActions; ++action) {
        const CompiledActionBindings& bindings = config.actions[action];
        bool expressed = false;
        for (uint8_t i = 0; i < bindings.count && i < kMaxBindingsPerAction;
             ++i) {
            const CompiledBinding& binding = bindings.bindings[i];
            if (binding.type != CompiledBindingType::KeyboardScan) continue;
            plan.actions[action].sourceType = kNativeSourceKeyboard;
            plan.actions[action].codeOrMask = binding.dik;
            plan.actions[action].callback = keyboardCallback;
            expressed = true;
            plan.anyExpressed = true;
            break;
        }
        if (!expressed)
            plan.unexpressedMask |= static_cast<uint8_t>(1u << action);
    }
    return plan;
}

constexpr uint32_t nativeDescriptorOffset(uint32_t player, uint32_t action) {
    return ((player * kNativeBindingActions) + action)
        * kNativeDescriptorBytes;
}

}
