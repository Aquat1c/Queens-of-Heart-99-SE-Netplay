#pragma once
#include <cstdint>
#include <cstddef>
#include "qoh99_addresses.h"
#include "qoh99_fields.h"

namespace qoh99 { namespace save {

enum class Conf { RT, SD, SL, Hyp };

struct SaveRegion {
    const char* name;
    uintptr_t   addr;
    size_t      size;
    Conf        confidence;
    bool        required;
    const char* evidence;
};

struct PointerSaveRegion {
    const char* name;
    uintptr_t   pointerAddress;
    size_t      pointeeSize;
    Conf        confidence;
    bool        nullable;
    const char* evidence;
};

inline const SaveRegion* regions(size_t& count) {
    static const SaveRegion r[] = {
    { "fighterP1", ga::kFighterP1, fld::kFighterSize, Conf::RT, true,  "qmemcpy(_,623E00,0x5FC) x196 sites" },
    { "fighterP2", ga::kFighterP2, fld::kFighterSize, Conf::RT, true,  "qmemcpy(_,626880,0x5FC)" },
    { "fighterP3", ga::kFighterP3, fld::kFighterSize, Conf::SD, false, "team mode; 0x5FC" },
    { "fighterP4", ga::kFighterP4, fld::kFighterSize, Conf::SD, false, "team mode; 0x5FC" },
    { "helperP1",  ga::kHelperP1,  fld::kFighterSize, Conf::SD, false, "qmemcpy(614060,623E00,0x5FC) when byte+0xAE==25" },
    { "helperP2",  ga::kHelperP2,  fld::kFighterSize, Conf::SD, false, "qmemcpy(61465C,626880,0x5FC)" },
    { "helperP3",  ga::kHelperP3,  fld::kFighterSize, Conf::SD, false, "0x5FC clone" },
    { "helperP4",  ga::kHelperP4,  fld::kFighterSize, Conf::SD, false, "0x5FC clone" },
    // Shadow/afterimage clone pool: 16 contiguous 0x5FC objects from 0x6275C0.
    { "shadowPool", 0x6275C0, 16 * fld::kFighterSize, Conf::SL, false, "16x0x5FC from init qmemcpy; probe 1532-3477 band" },
    // Dynamic actor arena: active = base + 118480*selector, alternating each frame.
    { "actorArena", ga::kArenaBase, 2 * ga::kArenaBlockSize, Conf::RT, true, "unk_4ED140 2x0x1CED0, sel 623700^=1" },
    { "arenaSelector", ga::kArenaSelector, 4, Conf::RT, true, "623700 ping-pong selector" },
    { "healthScore", ga::kHealth, 0x100, Conf::SL, true, "6265C8 health[11*p]/44 + 626670 score block" },
    { "roundCounter", ga::kRoundCounter, 4, Conf::SD, true, "871950" },
    { "rngSeeds", ga::kRngSeedGame, 8, Conf::RT, true, "4A9938 gameplay + 4A993C cosmetic" },
    { "rngSelector", ga::kRngSelector, 4, Conf::SD, true, "4ABB24 selects seed" },
    };
    count = sizeof(r) / sizeof(r[0]);
    return r;
}

inline const PointerSaveRegion* pointerRegions(size_t& count) {
    static const PointerSaveRegion p[] = {
    { "cmdP1", ga::kFighterP1 + fld::kFi_ChildPtr, fld::kCmd_SaveSize, Conf::SD, true, "*(623E00+164) sub_47BF90 rings" },
    { "cmdP2", ga::kFighterP2 + fld::kFi_ChildPtr, fld::kCmd_SaveSize, Conf::SD, true, "*(626880+164) sub_47BF90 rings" },
    { "cmdP3", ga::kFighterP3 + fld::kFi_ChildPtr, fld::kCmd_SaveSize, Conf::SD, false, "team" },
    { "cmdP4", ga::kFighterP4 + fld::kFi_ChildPtr, fld::kCmd_SaveSize, Conf::SD, false, "team" },
    };
    count = sizeof(p) / sizeof(p[0]);
    return p;
}

inline size_t totalFixedBytes(bool includeOptional) {
    size_t n = 0, total = 0; const SaveRegion* r = regions(n);
    for (size_t i = 0; i < n; ++i) if (includeOptional || r[i].required) total += r[i].size;
    return total;
}

inline int firstOverlap() {
    size_t n = 0; const SaveRegion* r = regions(n);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            uintptr_t a0 = r[i].addr, a1 = a0 + r[i].size;
            uintptr_t b0 = r[j].addr, b1 = b0 + r[j].size;
            if (a0 < b1 && b0 < a1) return (int)i;
        }
    return -1;
}

}}
