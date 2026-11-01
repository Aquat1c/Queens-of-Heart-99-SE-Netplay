#pragma once

#include <windows.h>

#include <cstdint>

#include <cstdio>
#include <cstring>

#include "input/InputActions.h"
#include "input/InputLaunchConfig.h"

namespace qinput {

/* Must match Qoh99MenuNavigationV1 in the wrapper's header. */
enum class MenuNav : uint32_t {
    None = 0,
    Up = 1,
    Down = 2,
    Left = 3,
    Right = 4,
    Confirm = 5,
    Back = 6,
};

using MenuNavSubmitFn = int(__stdcall*)(unsigned long navigation);
using MenuVisibleFn = int(__stdcall*)();
using MenuToggleFn = void(__stdcall*)();

constexpr const wchar_t* kMenuWrapperModule = L"ddraw.dll";

struct MenuNavExports {
    bool resolved = false;
    MenuNavSubmitFn submit = nullptr;
    MenuVisibleFn visible = nullptr;
    MenuToggleFn toggle = nullptr;
};

inline MenuNavExports& menuNavExports() {
    static MenuNavExports exports;
    if (!exports.resolved) {
        HMODULE wrapper = GetModuleHandleW(kMenuWrapperModule);
        if (wrapper) {
            exports.resolved = true;
            exports.submit = reinterpret_cast<MenuNavSubmitFn>(
                reinterpret_cast<void*>(GetProcAddress(
                    wrapper, "EFZDX9_Presenter_SubmitMenuNavigation")));
            exports.visible = reinterpret_cast<MenuVisibleFn>(
                reinterpret_cast<void*>(GetProcAddress(
                    wrapper, "EFZDX9_Presenter_IsMenuVisible")));
            exports.toggle = reinterpret_cast<MenuToggleFn>(
                reinterpret_cast<void*>(GetProcAddress(
                    wrapper, "EFZDX9_Presenter_ToggleMenu")));
        }
    }
    return exports;
}

inline bool menuOverlayVisible() {
    const MenuNavExports& exports = menuNavExports();
    return exports.visible != nullptr && exports.visible() != 0;
}

inline void menuOverlayToggle() {
    const MenuNavExports& exports = menuNavExports();
    if (exports.toggle != nullptr) exports.toggle();
}

struct MenuNavSlot {
    bool down = false;
};

class MenuNavigationForwarder {
public:
    uint32_t step(uint32_t logicalMask, uint64_t nowMs) {
        (void)nowMs;
        uint32_t fired = 0;
        fired |= edge(0, (logicalMask & kLogicalUp) != 0u, MenuNav::Up);
        fired |= edge(1, (logicalMask & kLogicalDown) != 0u, MenuNav::Down);
        fired |= edge(2, (logicalMask & kLogicalLeft) != 0u, MenuNav::Left);
        fired |= edge(3, (logicalMask & kLogicalRight) != 0u, MenuNav::Right);
        fired |= edge(4, (logicalMask & kLogicalA) != 0u, MenuNav::Confirm);
        fired |= edge(5, (logicalMask & kLogicalB) != 0u, MenuNav::Back);
        return fired;
    }

    void reset() {
        for (MenuNavSlot& slot : slots_) slot = MenuNavSlot{};
    }

private:
    uint32_t edge(int index, bool down, MenuNav nav) {
        MenuNavSlot& slot = slots_[index];
        if (!down) {
            slot.down = false;
            return 0u;
        }
        if (slot.down) return 0u;
        slot.down = true;
        return 1u << static_cast<uint32_t>(nav);
    }

    MenuNavSlot slots_[6];
};

inline void menuSubmitNavigationMask(uint32_t fired) {
    const MenuNavExports& exports = menuNavExports();
    if (exports.submit == nullptr || fired == 0u) return;
    for (uint32_t nav = 1u; nav < 7u; ++nav) {
        if ((fired & (1u << nav)) != 0u)
            (void)exports.submit(static_cast<unsigned long>(nav));
    }
}

using MenuNavLabelsFn = void(__stdcall*)(const char*, const char*);

constexpr const char* kMenuNavLabelsExport =
    "EFZDX9_Presenter_SetMenuNavigationLabels";

inline void menuNavDescribeBinding(const CompiledBinding& binding,
                                   char* out, size_t outBytes) {
    if (out == nullptr || outBytes == 0) return;
    out[0] = '\0';
    switch (binding.type) {
        case CompiledBindingType::KeyboardScan: {
            /* Bits 16..23 hold the scan code and bit 24 marks extended keys, which separates the arrow cluster from the numeric keypad. */
            LONG lparam = static_cast<LONG>(binding.dik) << 16;
            if (binding.dik == 0xC5u || binding.dik >= 0x9Cu)
                lparam |= (1L << 24);
            char name[32] = {};
            if (GetKeyNameTextA(lparam, name, sizeof(name)) > 0) {
                std::snprintf(out, outBytes, "%s", name);
                return;
            }
            std::snprintf(out, outBytes, "Key %02X",
                          static_cast<unsigned>(binding.dik));
            return;
        }
        case CompiledBindingType::ProbeButton:
            std::snprintf(out, outBytes, "Button %u",
                          static_cast<unsigned>(binding.probeIndex) + 1u);
            return;
        case CompiledBindingType::ProbeHat:
            std::snprintf(out, outBytes, "Hat %u",
                          static_cast<unsigned>(binding.probeIndex) + 1u);
            return;
        case CompiledBindingType::ProbeAxis:
            std::snprintf(out, outBytes, "Axis %u",
                          static_cast<unsigned>(binding.probeIndex) + 1u);
            return;
        default:
            return;
    }
}

inline void menuNavPublishLabels(const InputLaunchConfig& config) {
    const MenuNavExports& exports = menuNavExports();
    static MenuNavLabelsFn setLabels = nullptr;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        HMODULE wrapper = GetModuleHandleW(kMenuWrapperModule);
        if (wrapper) {
            setLabels = reinterpret_cast<MenuNavLabelsFn>(
                reinterpret_cast<void*>(
                    GetProcAddress(wrapper, kMenuNavLabelsExport)));
        }
    }
    (void)exports;
    if (setLabels == nullptr) return;

    char confirm[24] = {};
    char back[24] = {};
    const CompiledActionBindings& aBindings =
        config.actions[static_cast<size_t>(FightAction::A)];
    const CompiledActionBindings& bBindings =
        config.actions[static_cast<size_t>(FightAction::B)];
    if (aBindings.count > 0)
        menuNavDescribeBinding(aBindings.bindings[0], confirm, sizeof(confirm));
    if (bBindings.count > 0)
        menuNavDescribeBinding(bBindings.bindings[0], back, sizeof(back));
    if (confirm[0] == '\0') std::snprintf(confirm, sizeof(confirm), "A");
    if (back[0] == '\0') std::snprintf(back, sizeof(back), "B");

    static char lastConfirm[24] = {};
    static char lastBack[24] = {};
    if (std::strcmp(lastConfirm, confirm) == 0
        && std::strcmp(lastBack, back) == 0) {
        return;
    }
    std::snprintf(lastConfirm, sizeof(lastConfirm), "%s", confirm);
    std::snprintf(lastBack, sizeof(lastBack), "%s", back);
    setLabels(confirm, back);
}

}
