#pragma once
#include <windows.h>

#include "Log.h"
#include "../replay/ReplayTypes.h"

namespace qgame {

typedef int(__stdcall* PaletteQueryFn)(int, unsigned char*, int, int*);
typedef BOOL(__stdcall* PaletteSubmitFn)(int, const unsigned char*, int, int,
                                         int);
typedef void(__stdcall* PaletteOwnershipFn)(unsigned);

struct PaletteWrapperExports {
    bool resolved = false;
    PaletteQueryFn query = nullptr;
    PaletteSubmitFn submit = nullptr;
    PaletteOwnershipFn ownership = nullptr;
};

inline PaletteWrapperExports& paletteWrapperExports() {
    static PaletteWrapperExports exports;
    if (!exports.resolved) {
        HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
        if (wrapper) {
            exports.resolved = true;
            exports.query = reinterpret_cast<PaletteQueryFn>(
                reinterpret_cast<void*>(
                    GetProcAddress(wrapper, "EFZDDRAW_QueryPalette")));
            exports.submit = reinterpret_cast<PaletteSubmitFn>(
                reinterpret_cast<void*>(
                    GetProcAddress(wrapper, "EFZDDRAW_SubmitPalette")));
            exports.ownership = reinterpret_cast<PaletteOwnershipFn>(
                reinterpret_cast<void*>(
                    GetProcAddress(wrapper, "EFZDDRAW_SetPaletteOwnership")));
            QLOG("[palette.seam] ddraw.dll palette exports: query=%d "
                 "submit=%d", exports.query ? 1 : 0, exports.submit ? 1 : 0);
        } else {
            QLOG("[palette.seam] ddraw.dll is not loaded; custom palettes "
                 "are unavailable in this process");
        }
    }
    return exports;
}

inline void paletteDeclareOwnership(unsigned localSlotMask) {
    const PaletteWrapperExports& exports = paletteWrapperExports();
    if (exports.ownership == nullptr) return;
    exports.ownership(localSlotMask);
}

inline bool paletteQueryLocal(int side, unsigned char* table, size_t bytes,
                              int& variantOut) {
    const PaletteWrapperExports& exports = paletteWrapperExports();
    if (exports.query == nullptr || table == nullptr
        || bytes < qreplay::kReplayPaletteTableBytes) {
        return false;
    }
    variantOut = -1;
    return exports.query(side, table, static_cast<int>(bytes), &variantOut)
        == static_cast<int>(qreplay::kReplayPaletteTableBytes);
}

inline bool paletteSubmitToWrapper(const qreplay::ReplayPaletteV2& palette,
                                   bool remote) {
    const PaletteWrapperExports& exports = paletteWrapperExports();
    if (exports.submit == nullptr || palette.present == 0u) return false;
    return exports.submit(
               static_cast<int>(palette.side), palette.table,
               static_cast<int>(qreplay::kReplayPaletteTableBytes),
               static_cast<int>(palette.variant), remote ? 1 : 0) != FALSE;
}

inline bool paletteSubmitVariantToWrapper(int slot,
                                          const unsigned char* entries,
                                          int variant) {
    const PaletteWrapperExports& exports = paletteWrapperExports();
    if (exports.submit == nullptr || entries == nullptr) return false;
    if (variant < 0 || variant > 3) return false;
    return exports.submit(slot, entries, 64, variant, 1 )
        != FALSE;
}

struct PaletteReplayHold {
    uint32_t held = 0u;
    uint32_t applied = 0u;
    qreplay::ReplayPaletteV2 slots[4]{};
};

inline PaletteReplayHold& paletteReplayHold() {
    static PaletteReplayHold hold;
    return hold;
}

inline void paletteHoldFromReplay(const qreplay::ReplayPaletteV2& palette) {
    if (palette.side > 3u || palette.present == 0u) return;
    PaletteReplayHold& hold = paletteReplayHold();
    hold.slots[palette.side] = palette;
    hold.held |= 1u << palette.side;
    hold.applied &= ~(1u << palette.side);
}

inline void paletteClearReplayHold() {
    paletteReplayHold() = PaletteReplayHold{};
}

}
