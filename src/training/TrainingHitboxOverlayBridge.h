#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "training/TrainingHitboxOverlay.h"
#include "training/TrainingHitboxPublisher.h"

namespace qtraining {

constexpr int kOverlayMaxCommands = 192;
constexpr int kOverlayTextCap = 128;

constexpr int kOverlayCommandText = 1;
constexpr int kOverlayCommandRectFilled = 2;
constexpr int kOverlayCommandRect = 3;

constexpr unsigned long kOverlayTextShadow = 0x1u;
constexpr unsigned long kOverlayTextAlignCenter = 0x2u;

// command.flags bits 8..9: owner + 1 (1 = P1, 2 = P2, 0 = unowned).
// command.flags bits 12..15: kind + 1 (HitboxBoxKind 0..6 -> 1..7; 8 = the axis line).
constexpr unsigned long kOverlayMetaOwnerShift = 8u;
constexpr unsigned long kOverlayMetaKindShift = 12u;
constexpr unsigned long kOverlayMetaAxisKind = 8u;

constexpr unsigned long hitboxOverlayMetaFlags(const uint8_t owner,
                                               const unsigned long kindTag) {
    return owner > 1u
        ? 0ul
        : ((static_cast<unsigned long>(owner) + 1ul)
               << kOverlayMetaOwnerShift)
            | (kindTag << kOverlayMetaKindShift);
}

struct OverlayCommandAbi {
    unsigned long structSize;
    int type;
    float x;
    float y;
    float w;
    float h;
    float fontSize;
    float thickness;
    unsigned long color;
    unsigned long flags;
    char text[kOverlayTextCap];
};

struct OverlayFrameAbi {
    unsigned long structSize;
    int visible;
    float virtualWidth;
    float virtualHeight;
    int commandCount;
    OverlayCommandAbi commands[kOverlayMaxCommands];
};

static_assert(sizeof(OverlayCommandAbi) == 168u,
              "overlay command must match the wrapper's declaration");

using OverlayProviderFn = int(__stdcall*)(OverlayFrameAbi* outFrame,
                                          void* userData);

using RegisterOverlayProviderFn = void(__stdcall*)(OverlayProviderFn provider,
                                                   void* userData);

constexpr const char* kRegisterOverlayProviderExport =
    "EFZDX9_RegisterOverlayProvider";

constexpr const wchar_t* kHitboxWrapperModule = L"ddraw.dll";

constexpr unsigned long long kHitboxRegisterRetryMs = 500ull;

inline int hitboxComposeOverlayFrame(const HitboxSnapshotV1& snapshot,
                                     OverlayFrameAbi& frame) {
    frame.visible = 0;
    frame.virtualWidth = kHitboxVirtualWidth;
    frame.virtualHeight = kHitboxVirtualHeight;
    frame.commandCount = 0;

    if (!snapshot.visible
        || !hitboxModeWantsCustomOverlay(
               hitboxDisplayModeFrom(snapshot.mode))) {
        return 1;
    }

    const uint32_t boxes = snapshot.boxCount > kHitboxSnapshotMaxBoxes
        ? kHitboxSnapshotMaxBoxes
        : snapshot.boxCount;

    const auto emit = [&](const HitboxBoxV1& box, const bool outline) {
        if (frame.commandCount >= kOverlayMaxCommands) return;
        const HitboxPalette palette =
            hitboxPaletteFor(static_cast<HitboxBoxKind>(box.kind));
        const unsigned long color = outline
            ? static_cast<unsigned long>(palette.outlineArgb)
            : static_cast<unsigned long>(palette.fillArgb);
        if (!outline && (palette.fillArgb >> 24) == 0u) return;

        OverlayCommandAbi& command = frame.commands[frame.commandCount++];
        std::memset(&command, 0, sizeof(command));
        command.structSize = sizeof(OverlayCommandAbi);
        command.type = outline ? kOverlayCommandRect : kOverlayCommandRectFilled;
        command.x = static_cast<float>(box.x);
        command.y = static_cast<float>(box.y);
        command.w = static_cast<float>(box.w);
        command.h = static_cast<float>(box.h);
        command.fontSize = 0.0f;
        command.thickness = 1.0f;
        command.color = color;
        command.flags = hitboxOverlayMetaFlags(
            box.owner, static_cast<unsigned long>(box.kind) + 1ul);
    };

    for (uint32_t i = 0; i < boxes; ++i) {
        const HitboxBoxV1& box = snapshot.boxes[i];
        if (static_cast<HitboxBoxKind>(box.kind) != HitboxBoxKind::Collision)
            continue;
        if (frame.commandCount >= kOverlayMaxCommands) break;
        OverlayCommandAbi& command = frame.commands[frame.commandCount++];
        std::memset(&command, 0, sizeof(command));
        command.structSize = sizeof(OverlayCommandAbi);
        command.type = kOverlayCommandRectFilled;
        command.x = static_cast<float>(box.x)
            + static_cast<float>(box.w) * 0.5f - 1.0f;
        command.y = 0.0f;
        command.w = 1.0f;
        command.h = kHitboxVirtualHeight;
        command.thickness = 1.0f;
        command.color = hitboxArgb(kHitboxRgbCollision, 210u);
        command.flags = hitboxOverlayMetaFlags(box.owner,
                                               kOverlayMetaAxisKind);
    }

    for (int rank = 0; rank <= 6; ++rank) {
        for (uint32_t i = 0; i < boxes; ++i) {
            const HitboxBoxV1& box = snapshot.boxes[i];
            if (hitboxDrawRank(static_cast<HitboxBoxKind>(box.kind)) == rank)
                emit(box, false);
        }
    }
    for (int rank = 0; rank <= 6; ++rank) {
        for (uint32_t i = 0; i < boxes; ++i) {
            const HitboxBoxV1& box = snapshot.boxes[i];
            if (hitboxDrawRank(static_cast<HitboxBoxKind>(box.kind)) == rank)
                emit(box, true);
        }
    }

    if (snapshot.clashMask != 0u && snapshot.clashPointValid
        && frame.commandCount < kOverlayMaxCommands) {
        OverlayCommandAbi& command = frame.commands[frame.commandCount++];
        std::memset(&command, 0, sizeof(command));
        command.structSize = sizeof(OverlayCommandAbi);
        command.type = kOverlayCommandText;
        command.x = static_cast<float>(snapshot.clashX);
        command.y = static_cast<float>(snapshot.clashY - 14);
        command.w = 0.0f;
        command.h = 0.0f;
        command.fontSize = 0.0f;
        command.thickness = 0.0f;
        command.color = hitboxArgb(kHitboxRgbClash, 255u);
        command.flags = kOverlayTextAlignCenter | kOverlayTextShadow;
        std::memcpy(command.text, "CLASH", 6);
    }

    frame.visible = frame.commandCount > 0 ? 1 : 0;
    return 1;
}

inline int __stdcall hitboxOverlayProviderThunk(OverlayFrameAbi* outFrame,
                                                void*) {
    if (!outFrame) return 0;
    if (outFrame->structSize < sizeof(OverlayFrameAbi)) return 0;
    HitboxSnapshotV1 snapshot{};
    if (!trainingHitboxes().read(snapshot)) return 0;
    return hitboxComposeOverlayFrame(snapshot, *outFrame);
}

struct HitboxOverlayBridgeState {
    bool registered = false;
    unsigned long long retryMs = 0ull;
};

inline HitboxOverlayBridgeState& hitboxOverlayState() {
    static HitboxOverlayBridgeState state;
    return state;
}

inline bool hitboxOverlayRegistered() {
    return hitboxOverlayState().registered;
}

inline RegisterOverlayProviderFn hitboxRegisterExport() {
    HMODULE wrapper = GetModuleHandleW(kHitboxWrapperModule);
    if (!wrapper) return nullptr;
    return reinterpret_cast<RegisterOverlayProviderFn>(
        reinterpret_cast<void*>(
            GetProcAddress(wrapper, kRegisterOverlayProviderExport)));
}

inline bool hitboxTryRegisterOverlay() {
    HitboxOverlayBridgeState& state = hitboxOverlayState();
    if (state.registered) return false;
    const unsigned long long now = GetTickCount64();
    if (state.retryMs != 0ull
        && now - state.retryMs < kHitboxRegisterRetryMs) {
        return false;
    }
    state.retryMs = now;
    const RegisterOverlayProviderFn registerProvider = hitboxRegisterExport();
    if (!registerProvider) return false;
    registerProvider(&hitboxOverlayProviderThunk, nullptr);
    state.registered = true;
    return true;
}

inline bool hitboxReleaseOverlay() {
    HitboxOverlayBridgeState& state = hitboxOverlayState();
    if (!state.registered) return false;
    state.registered = false;
    state.retryMs = 0ull;
    const RegisterOverlayProviderFn registerProvider = hitboxRegisterExport();
    if (!registerProvider) return false;
    registerProvider(nullptr, nullptr);
    return true;
}

}
