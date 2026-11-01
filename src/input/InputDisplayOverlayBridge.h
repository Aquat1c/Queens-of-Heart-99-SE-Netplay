#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "input/InputDisplayPublisher.h"
#include "input/InputDisplayTypes.h"

namespace qinput {

using InputDisplayProviderFnV1 =
    int(__stdcall*)(InputDisplaySnapshotV1* outSnapshot,
                    unsigned long outBytes,
                    void* userData);

using RegisterInputDisplayProviderFnV1 =
    void(__stdcall*)(unsigned long apiVersion,
                     InputDisplayProviderFnV1 provider,
                     void* userData);

constexpr const char* kRegisterInputDisplayProviderExport =
    "EFZDX9_RegisterInputDisplayProvider";

constexpr const wchar_t* kInputDisplayWrapperModule = L"ddraw.dll";

constexpr unsigned long long kInputDisplayRegisterRetryMs = 500ull;

inline int __stdcall inputDisplayOverlayProviderThunkV1(
        InputDisplaySnapshotV1* outSnapshot,
        unsigned long outBytes,
        void*) {
    if (!outSnapshot) return 0;
    const uint32_t tier = inputDisplayTierFor(static_cast<uint32_t>(outBytes));
    if (tier == 0u) return 0;
    InputDisplaySnapshotV1 snapshot{};
    if (!inputDisplay().read(snapshot)) return 0;
    snapshot.structSize = tier;
    std::memcpy(outSnapshot, &snapshot, tier);
    return 1;
}

struct InputDisplayOverlayBridgeState {
    bool registered = false;
    unsigned long long retryMs = 0ull;
};

inline InputDisplayOverlayBridgeState& inputDisplayOverlayState() {
    static InputDisplayOverlayBridgeState state;
    return state;
}

inline bool inputDisplayOverlayRegistered() {
    return inputDisplayOverlayState().registered;
}

inline RegisterInputDisplayProviderFnV1 inputDisplayRegisterExport() {
    HMODULE wrapper = GetModuleHandleW(kInputDisplayWrapperModule);
    if (!wrapper) return nullptr;
    return reinterpret_cast<RegisterInputDisplayProviderFnV1>(
        reinterpret_cast<void*>(
            GetProcAddress(wrapper, kRegisterInputDisplayProviderExport)));
}

inline bool inputDisplayTryRegisterOverlay() {
    InputDisplayOverlayBridgeState& state = inputDisplayOverlayState();
    if (state.registered) return false;
    const unsigned long long now = GetTickCount64();
    if (state.retryMs != 0ull
        && now - state.retryMs < kInputDisplayRegisterRetryMs) {
        return false;
    }
    state.retryMs = now;
    const RegisterInputDisplayProviderFnV1 registerProvider =
        inputDisplayRegisterExport();
    if (!registerProvider) return false;
    registerProvider(kInputDisplayApiVersion,
                     &inputDisplayOverlayProviderThunkV1,
                     nullptr);
    state.registered = true;
    return true;
}

inline bool inputDisplayReleaseOverlay() {
    InputDisplayOverlayBridgeState& state = inputDisplayOverlayState();
    if (!state.registered) return false;
    state.registered = false;
    state.retryMs = 0ull;
    const RegisterInputDisplayProviderFnV1 registerProvider =
        inputDisplayRegisterExport();
    if (!registerProvider) return false;
    registerProvider(kInputDisplayApiVersion, nullptr, nullptr);
    return true;
}

}
