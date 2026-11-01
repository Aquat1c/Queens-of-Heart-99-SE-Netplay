#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include "qoh99/AddressMap.hpp"
#include "qoh99/GameplayRegionCatalog.hpp"

namespace qoh99 {

constexpr uintptr_t kFrameDispatchFn = fnaddr::kSceneMatchDispatch;
constexpr uintptr_t kInputWordAddr   = addr::kSessionInputWord;

constexpr uintptr_t kKeyStateAddr    = addr::kKeyState;
constexpr size_t    kKeyStateSize    = 256;

constexpr uintptr_t kSceneStateAddr  = addr::kSceneCurrent;   // Scene: 1=menu 2=match 3=charsel 4=replay.
constexpr uintptr_t kSceneNextAddr   = addr::kSceneNext;
constexpr uintptr_t kMatchSubAddr    = addr::kMatchSubstate;  // Match sub-state: -1 intro, 0 play, 5 win.
constexpr uintptr_t kInMatchNetAddr  = addr::kNetInMatchFlag;
constexpr int       kSceneMatch      = 2;

constexpr uintptr_t kBlock0Addr      = addr::kArenaBlock0;
constexpr uintptr_t kBlock1Addr      = addr::kArenaBlock1;
constexpr size_t    kBlockSize       = addr::kArenaBlockSize;
constexpr uintptr_t kActiveBlockPtr  = addr::kArenaActivePtr;
constexpr uintptr_t kBlockSelector   = addr::kArenaSelector;

constexpr uintptr_t kFighterP1Addr   = addr::kFighterP1;
constexpr uintptr_t kFighterP2Addr   = addr::kFighterP2;
constexpr uintptr_t kFighterTableAddr= addr::kFighterPtrTable;

constexpr uintptr_t kRngSeedGameplay = addr::kRngSeedGameplay;
constexpr uintptr_t kRngSeedCosmetic = addr::kRngSeedCosmetic;
constexpr uintptr_t kRngSelector     = addr::kRngSelector;

// Lethal life is current + reserve - pendingDamage in this 16-byte/player block, not the 0..100 value at 0x6265C8.
constexpr uintptr_t kBattleLifeBase  = addr::kBattleLifeArray;
constexpr uintptr_t kGuardGaugeBase  = addr::kGuardGaugeArray;
constexpr uintptr_t kHealthBase      = kGuardGaugeBase;
constexpr uintptr_t kScoreBase       = addr::kScoreArray;

constexpr uintptr_t kNetSetupFn      = fnaddr::kNetSetupTransport;
constexpr uintptr_t kNetConnectFn    = fnaddr::kNetConnectDialogs;
constexpr uintptr_t kNetSendFn       = fnaddr::kNetSendMsg;
constexpr uintptr_t kNetDispatchPtr  = addr::kNetDispatchPtr;
constexpr uintptr_t kNetConnectCbPtr = addr::kNetOnMatchStartPtr;
constexpr uintptr_t kNetLocalIdAddr  = addr::kNetSessionId;
constexpr uintptr_t kNetIsHostAddr   = addr::kNetIsHost;
constexpr uintptr_t kNetSideAddr     = addr::kNetLocalSide;
constexpr uintptr_t kNetReadyAddr    = addr::kNetReadyFlag;
constexpr uintptr_t kNetClientGotAddr= addr::kNetClientGotFlag;
constexpr uintptr_t kNetIsNetworkAddr= addr::kNetIsNetworkSession;
// DirectPlay Receive is vtable index 25 (+100), Send is index 26 (+104).
constexpr int       kDPlayRecvSlot   = 25;
constexpr int       kDPlaySendSlot   = 26;

constexpr uintptr_t kInputSetKeyFn    = fnaddr::kInputInjectKey;
constexpr uintptr_t kSub48StateAddr   = addr::kMainMenuSubstate;
constexpr uintptr_t kMenuCursorAddr   = addr::kMainMenuCursor;
constexpr uintptr_t kMenuConfirmArmed = 0x4AB8CC;
constexpr uintptr_t kVsModeRowAddr    = addr::kScene3Cursor;
constexpr uintptr_t kVsModeConfirm    = addr::kScene3ConfirmFlag;
constexpr int       kScanConfirm      = 0x58;
constexpr int       kScanCursorDown   = 0xD3;

constexpr uintptr_t kVsModeSetupFn   = fnaddr::kApplyVsModeRow;
constexpr uintptr_t kHumanFlags      = addr::kCharSelConfirmP1;
// dword_62369C is the applied VS formation row: 0 = none (Arcade/Story), 1 = two-fighter (VS rows 0/1), 2 = four-fighter (VS rows 2..5).
constexpr uintptr_t kTeamSize        = addr::kTeamSize;
constexpr int       kVsRow_1v1Human  = 1;

enum class RegionRole {
    GameplayRequired,
    GameplayCandidate,
    RenderCosmetic,
    WallClockPacing,
    OSPointerUnsafe,
    DebugOnly
};
struct Region { uintptr_t addr; size_t size; const char* name; RegionRole role; };

inline bool inGameplayHash(RegionRole r) {
    return r == RegionRole::GameplayRequired || r == RegionRole::GameplayCandidate;
}
inline bool inRenderHash(RegionRole r) { return r == RegionRole::RenderCosmetic; }

inline bool gameplaySyncGreen(unsigned long long badFrames) { return badFrames == 0; }
inline bool safeToRestore(RegionRole r) {
    return r != RegionRole::WallClockPacing && r != RegionRole::OSPointerUnsafe && r != RegionRole::DebugOnly;
}

inline RegionRole roleFromRelevance(GameplayRelevance g) {
    switch (g) {
        case GameplayRelevance::DeterministicGameplay: return RegionRole::GameplayRequired;
        case GameplayRelevance::RenderCosmetic:        return RegionRole::RenderCosmetic;
        case GameplayRelevance::WallClockPacing:       return RegionRole::WallClockPacing;
        case GameplayRelevance::ExternalResource:      return RegionRole::OSPointerUnsafe;
        default:                                       return RegionRole::DebugOnly;
    }
}

inline const Region* saveRegions(size_t& count) {
    static const std::vector<Region> regions = [] {
        std::vector<Region> v;
        size_t n = 0;
        const RegionInfo* r = GameplayRegionCatalog::regions(n);
        for (size_t i = 0; i < n; ++i) {
            if (r[i].relevance == GameplayRelevance::DeterministicGameplay ||
                r[i].relevance == GameplayRelevance::RenderCosmetic)
                v.push_back({ (uintptr_t)r[i].address, (size_t)r[i].size, r[i].name,
                              roleFromRelevance(r[i].relevance) });
        }
        return v;
    }();
    count = regions.size();
    return regions.data();
}

inline const Region* legacyReferenceRegions(size_t& count) {
    static const Region regions[] = {
        { 0x4ECB40, 0x5FC,   "fighterP3@4ECB40",     RegionRole::GameplayRequired },
        { kBlock0Addr, kBlockSize, "block0@4ED140",  RegionRole::RenderCosmetic },
        { kBlock1Addr, kBlockSize, "block1@50A010",  RegionRole::RenderCosmetic },
        // Spawned-actor pool: sub_42DDB0 clones a fighter into &byte_526FA0 + 1532*slot, up to 120 slots; 120*0x5FC = 0x2CE20.
        { 0x526FA0, 0x2D000, "actor_clone_pool@526FA0", RegionRole::GameplayRequired },
        { 0x613000, 0x47C0,  "dw_lo1@613000",    RegionRole::GameplayRequired },
        { 0x6177C0, 0x10,    "arena_mirror@6177CC", RegionRole::RenderCosmetic },
        { 0x6177D0, 0x8,     "freeze_requests_23@6177D0", RegionRole::GameplayRequired },
        { 0x6177D8, 0x28,    "arena_mirror_tail@6177D8", RegionRole::RenderCosmetic },
        { 0x617800, 0xBCE0,  "dw_lo2@617800",    RegionRole::GameplayRequired },
        { 0x6234E0, 0x40,    "arena_active@6234E8", RegionRole::RenderCosmetic },
        { 0x623520, 0x1D0,   "dw_mid@623520",    RegionRole::GameplayRequired },
        { 0x6236F0, 0x30,    "arena_sel@623700", RegionRole::RenderCosmetic },
        { 0x623720, 0x14,    "screen_fx_state@623720", RegionRole::RenderCosmetic },
        { 0x623734, 0x1CC,   "dw_mid2a@623734",  RegionRole::GameplayRequired },
        { 0x623900, 0x100,   "hud_text@623940",  RegionRole::RenderCosmetic },
        { 0x623A00, 0x2DC0,  "dw_mid2b@623A00",  RegionRole::GameplayRequired },
        { 0x6267C0, 0x40,    "arena_latch@6267D4", RegionRole::RenderCosmetic },
        { 0x626800, 0x7800,  "dw_hi@626800",     RegionRole::GameplayRequired },
        { 0x49B520, 0x40,    "camera_zoom@49B520",   RegionRole::RenderCosmetic },
        { 0x4A9930, 0x20,    "rng_seeds@4A9930",     RegionRole::GameplayRequired },
        { 0x4ABB20, 0x10,    "rng_selector@4ABB20",  RegionRole::GameplayRequired },
        { 0x8716C0, 0x1400,  "matchsub_block@8716C0", RegionRole::GameplayRequired },
        { 0x8A5C00, 0x200,   "hud_counters@8A5C00",  RegionRole::GameplayRequired },
        { 0x87D000, 0x4000,  "hud_display@87D000",   RegionRole::RenderCosmetic },
    };
    count = sizeof(regions) / sizeof(regions[0]);
    return regions;
}

// Restore fixed regions first, then re-read *(fighterBase + 164) from the restored fighter and restore the pointee.
struct PtrRegion {
    uintptr_t fighterBase;
    size_t ptrOffset;
    size_t size;
    const char* name;
    RegionRole role;
    bool ownerCaptured;
    uintptr_t activationPointerAddress;
};

inline const PtrRegion* pointerRegions(size_t& count) {
    static const std::vector<PtrRegion> regions = [] {
        std::vector<PtrRegion> v;
        size_t n = 0;
        const PointerChildInfo* pc = GameplayRegionCatalog::pointerChildren(n);
        for (size_t i = 0; i < n; ++i) {
            constexpr size_t kChildPtrOff = 164;
            v.push_back({ (uintptr_t)pc[i].pointerAddress - kChildPtrOff, kChildPtrOff,
                          (size_t)pc[i].pointeeSize, pc[i].name,
                          pc[i].nullable ? RegionRole::GameplayCandidate
                                         : RegionRole::GameplayRequired,
                          true,
                          pc[i].nullable
                              ? kFighterTableAddr + i * sizeof(uint32_t)
                              : 0u });
        }
        return v;
    }();
    count = regions.size();
    return regions.data();
}

inline bool pointerChildParticipates(
    const PtrRegion& region, uint32_t activationPointerValue) {
    return region.activationPointerAddress == 0u
        || activationPointerValue != 0u;
}

}
