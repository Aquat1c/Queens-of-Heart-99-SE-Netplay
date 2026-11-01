#pragma once
#include "qoh99/Types.hpp"
#include "qoh99/generated/AddressConstants.hpp"
#include "qoh99/generated/NativeSymbolCatalog.hpp"
#include <cstdint>
#include <cstddef>

namespace qoh99 {

enum class Confidence {
    VerifiedRuntime,
    VerifiedStatic,
    StrongLikely,
    Hypothesis
};

inline const char* confStr(Confidence c) {
    switch (c) {
        case Confidence::VerifiedRuntime: return "RT";
        case Confidence::VerifiedStatic:  return "SD";
        case Confidence::StrongLikely:    return "SL";
        default:                          return "??";
    }
}

enum class Kind { Global, Function, SaveRegion, FighterPtr };

struct AddressInfo {
    const char* name;
    uintptr_t   va;
    size_t      size;
    Kind        kind;
    Confidence  confidence;
    const char* evidence;
    const char* notes;
};

struct FieldInfo {
    const char* owner;
    const char* name;
    size_t      offset;
    size_t      size;
    Confidence  confidence;
    const char* evidence;
    const char* notes;
};

namespace size_ = ::qoh99::generated_model::size;
namespace nsym = ::qoh99::generated_model;

inline constexpr uintptr_t reVa(nsym::NativeSymbolId id) {
    return nsym::nativeSymbolNovemberVa(id);
}

inline const AddressInfo* addresses(size_t& count) {
    using Id = nsym::NativeSymbolId;
    static const AddressInfo a[] = {
    { "scene",            reVa(Id::SceneCurrent), 4, Kind::Global,   Confidence::VerifiedRuntime, "live 1->3->2 capture", "1=menu 2=match 3=vs/charsel 4=replay" },
    { "sceneNext",        reVa(Id::SceneNext), 4, Kind::Global,   Confidence::VerifiedRuntime, "capture", "next scene; loop copies next->cur" },
    { "scene3Sub",        reVa(Id::Scene3Substate), 4, Kind::Global,   Confidence::VerifiedRuntime, "screenshot+capture", "1=VS-list 2=CHARSELECT (sub_43A790 switch)" },
    { "mainMenuSub",      reVa(Id::MainMenuSubstate), 4, Kind::Global,   Confidence::VerifiedRuntime, "capture", "sub_48CA20 substate; 2=menu up" },
    { "mainMenuCursor",   reVa(Id::MainMenuCursor), 4, Kind::Global,   Confidence::VerifiedRuntime, "CE +AB8D0 + capture", "1=VS MODE" },
    { "matchSub",         reVa(Id::MatchSubstate), 4, Kind::Global,   Confidence::VerifiedRuntime, "gameState=0 whole match", "sub_428430 switch; 0=gameplay 5=win" },
    { "vsListCursor",     reVa(Id::Scene3Cursor), 4, Kind::Global,   Confidence::VerifiedRuntime, "capture cur458 0->1", "VS-list row; per-player character cursors start at p1CharCursor" },
    { "confirmFlag",      reVa(Id::Scene3ConfirmFlag), 4, Kind::Global,   Confidence::VerifiedRuntime, "capture conf450", "confirm; in charselect = GO BACK (line 34344)" },
    { "vsConfirmedFlag",  reVa(Id::VsListEntryFlag), 4, Kind::Global,   Confidence::VerifiedRuntime, "capture 49EF00->1", "sub_432E00 uses it to enter VS-list" },
    { "teamSize",         reVa(Id::VsFormationRow), 4, Kind::Global,   Confidence::VerifiedRuntime, "capture team->1", "applied VS formation row; 1=two-fighter 2=2v2" },
    { "controlMask",      reVa(Id::ControlMask), 4, Kind::Global,   Confidence::VerifiedRuntime, "capture ctrl", "player-control mask" },
    { "human0",           reVa(Id::CharSelConfirmP1), 4, Kind::Global,   Confidence::VerifiedRuntime, "capture 1->2->4", "P1 charsel confirm state; dword_8716D0[player]" },
    { "human1",           reVa(Id::CharSelConfirmP2), 4, Kind::Global,   Confidence::VerifiedRuntime, "capture 1->2->4", "P2 charsel confirm state" },
    { "p1CharCursor",     reVa(Id::P1CharCursor), 4, Kind::Global,   Confidence::VerifiedStatic,  "dword_870954[280*p]; CE +470954", "P1 char cursor" },
    { "p2CharCursor",     reVa(Id::P2CharCursor), 4, Kind::Global,   Confidence::VerifiedStatic,  "dword_870954[280]", "P2 char cursor" },
    { "roundCounter",     reVa(Id::RoundCounter), 4, Kind::Global,   Confidence::VerifiedStatic,  "static analysis; =5 at match end", "round counter" },
    { "inMatchNetFlag",   reVa(Id::NetInMatchFlag), 4, Kind::Global,   Confidence::VerifiedRuntime, "0 whole local match", "NETPLAY-only in-match flag; NOT general" },

    { "frameDispatch",    reVa(Id::SceneMatchDispatch), 0, Kind::Function, Confidence::VerifiedRuntime, "hooked live", "sub_428430; per-frame match tick (scene==2)" },
    { "framePacing",      reVa(Id::FramePacing), 0, Kind::Function, Confidence::VerifiedStatic,  "timeGetTime", "ShiftedEntry: November 0x491010 / December 0x491030" },
    { "msgPump",          reVa(Id::MessagePump), 0, Kind::Function, Confidence::VerifiedRuntime, "MinHook -> EIP=0 crash", "ShiftedEntry: November 0x491740 / December 0x491760; DO NOT HOOK" },
    { "gameplayStepFn",   reVa(Id::GameplayTick), 0, Kind::Function, Confidence::VerifiedRuntime, "matchSub=0 runs it", "sub_421EE0 main gameplay" },

    { "keyState",         reVa(Id::KeyState), 256, Kind::Global, Confidence::VerifiedStatic,  "sub_45D1A0 fills", "DI/keyboard state" },
    { "inputPollFn",      reVa(Id::InputPollRaw), 0, Kind::Function, Confidence::VerifiedStatic,  "static analysis", "raw poll" },
    { "inputTestFn",      reVa(Id::KeyTest), 0, Kind::Function, Confidence::VerifiedStatic,  "static analysis", "KeyState[remap[sc]]&0x80" },
    { "inputSetFn",       reVa(Id::InputInjectKey), 0, Kind::Function, Confidence::VerifiedStatic,  "static analysis", "inject KeyState" },
    { "inputReadLogical", reVa(Id::ReadLogicalInput), 0, Kind::Function, Confidence::VerifiedStatic,  "WinMain readLogical(0)", "ShiftedEntry: November 0x484B90 / December 0x484BB0" },
    { "inputApplyLogical",reVa(Id::ApplyLogicalInput), 0, Kind::Function, Confidence::VerifiedStatic,  "static analysis", "ShiftedEntry: November 0x484C40 / December 0x484C60" },
    { "sessionInputWord", reVa(Id::SessionInputWord), 4, Kind::Global,   Confidence::VerifiedStatic,  "few bits used", "menu/session control word (NOT fighter dir)" },

    { "matchBlock0",      reVa(Id::ArenaBlock0), size_::kArenaBlockSize, Kind::SaveRegion, Confidence::VerifiedRuntime, "ping-pong live", "118480 B" },
    { "matchBlock1",      reVa(Id::ArenaBlock1), size_::kArenaBlockSize, Kind::SaveRegion, Confidence::VerifiedRuntime, "ping-pong live", "= block0 + 0x1CED0" },
    { "activeBlockPtr",   reVa(Id::ArenaActivePtr), 4, Kind::Global,   Confidence::VerifiedRuntime, "= 4ED140+118480*sel", "active block pointer" },
    { "blockSelector",    reVa(Id::ArenaSelector), 4, Kind::Global,   Confidence::VerifiedRuntime, "toggles ^=1/frame", "ping-pong selector" },
    { "blockMirrorPtr",   reVa(Id::ArenaMirrorPtr), 4, Kind::Global,   Confidence::VerifiedStatic,  "static analysis", "mirror/pair ptr" },
    { "swapLatch",        reVa(Id::ArenaSwapLatch), 4, Kind::Global,   Confidence::VerifiedStatic,  "static analysis", "per-frame swap latch" },

    { "fighterPtrTable",  reVa(Id::FighterPtrTable), 16, Kind::Global,  Confidence::VerifiedStatic,  "off_4A37B4[4]", "4 fighter ptrs" },
    { "fighterBackupTbl", reVa(Id::FighterBackupTable), 16, Kind::Global,  Confidence::VerifiedStatic,  "dword_623500[4]", "backup table" },
    { "fighterP1Static",  reVa(Id::FighterP1), size_::kFighterSize, Kind::FighterPtr, Confidence::VerifiedRuntime, "capture [623E00 626880 0 0]", "P1 static fighter (1v1)" },
    { "fighterP2Static",  reVa(Id::FighterP2), size_::kFighterSize, Kind::FighterPtr, Confidence::VerifiedRuntime, "capture", "P2 static fighter (1v1)" },

    { "health",           reVa(Id::HealthArray), 4, Kind::Global,   Confidence::VerifiedRuntime, "climbed/reset live; CE", "[11*plr] stride 44, 0..100" },
    { "score",            reVa(Id::ScoreArray), 4, Kind::Global,   Confidence::VerifiedRuntime, "CE +226670; climbed", "per-player score" },
    { "rngSeedGameplay",  reVa(Id::RngSeedGameplay), 4, Kind::Global,   Confidence::VerifiedRuntime, "advanced live", "LCG seed" },
    { "rngSeedCosmetic",  reVa(Id::RngSeedCosmetic), 4, Kind::Global,   Confidence::VerifiedStatic,  "static analysis", "cosmetic seed" },
    { "rngSelector",      reVa(Id::RngSelector), 4, Kind::Global,   Confidence::VerifiedStatic,  "static analysis", "0=>gameplay" },
    { "matchConfigPtr",   reVa(Id::MatchConfigPtr), 4, Kind::Global,   Confidence::VerifiedStatic,  "used as *(ptr+off)", "POINTER to match config" },

    { "netDispatch",      reVa(Id::NetDispatchMsg), 0, Kind::Function, Confidence::VerifiedStatic,  "8-type switch", "sub_45D6B0 (=*0x66ECC0)" },
    { "netSend",          reVa(Id::NetSendMsg), 0, Kind::Function, Confidence::VerifiedStatic,  "static analysis", "sub_45D970 -> ppv+104" },
    { "netSetup",         reVa(Id::NetSetupTransport), 0, Kind::Function, Confidence::VerifiedRuntime, "fake ppv works", "CoCreateInstance + recv thread" },
    { "netConnectDlg",    reVa(Id::NetConnectDialogs), 0, Kind::Function, Confidence::VerifiedRuntime, "hook -> skip", "host/join dialogs" },
    { "netSideAddr",      reVa(Id::NetLocalSide), 4, Kind::Global,   Confidence::VerifiedStatic,  "msg type 3", "local side" },
    { "netReadyFlag",     reVa(Id::NetReadyFlag), 4, Kind::Global,   Confidence::VerifiedStatic,  "msg type 7", "ready/merge enable" },
    { "netClientGotFlag", reVa(Id::NetClientGotFlag), 4, Kind::Global,   Confidence::VerifiedRuntime, "we set on peer-lock", "host<-client connected" },
    { "netIsNetworkFlag", reVa(Id::NetIsNetworkSession), 4, Kind::Global,   Confidence::VerifiedStatic,  "CLI Local/Host", "1=network session" },
    };
    count = sizeof(a) / sizeof(a[0]);
    return a;
}

static_assert(reVa(nsym::NativeSymbolId::FramePacing) == 0x491010u, "");
static_assert(reVa(nsym::NativeSymbolId::MessagePump) == 0x491740u, "");
static_assert(reVa(nsym::NativeSymbolId::ReadLogicalInput) == 0x484B90u, "");
static_assert(reVa(nsym::NativeSymbolId::ApplyLogicalInput) == 0x484C40u, "");

inline const FieldInfo* fighterFields(size_t& count) {
    static const FieldInfo f[] = {
    // Fighter object (size 0x5FC).
    { "fighter", "childCmdPtr",  164,  4, Confidence::VerifiedStatic, "sub_47BF90: v1=*(this+164)", "-> command/input-history object; save-by-pointer" },
    { "fighter", "facing",       170,  1, Confidence::VerifiedStatic, "sub_47BF90 ==1 swaps L/R", "facing byte; 1 => LEFT<->RIGHT swap" },
    { "fighter", "playerIndex",  172,  1, Confidence::VerifiedStatic, "dword_4A37C4[*(this+172)]", "input-source index" },
    { "fighter", "charOrType",   174,  1, Confidence::StrongLikely,   "obs 35/10/41; move gate ==25/1", "char/type/state byte (byte+0xAE)" },
    { "fighter", "controlLock",  1400, 4, Confidence::VerifiedStatic, "sub_47BF90: *(this+1400)=>input 0", "nonzero disables input recording" },
    { "fighter", "pairedPtr",    1436, 4, Confidence::StrongLikely,   "sub_41A540 pairing", "opponent pointer candidate" },
    // Child command/input-history object (fighter+164).
    { "cmd", "dirRing",   8,   128, Confidence::VerifiedStatic, "sub_47BF90 v1+4*i+8",   "32x dword direction ring (dir<<16)" },
    { "cmd", "dirHold",   136, 128, Confidence::VerifiedStatic, "sub_47BF90 v1+4*i+136", "32x dword dir hold counters" },
    { "cmd", "dirHead",   264, 1,   Confidence::VerifiedStatic, "sub_47BF90 (h+1)&0x1F", "direction ring head index" },
    { "cmd", "btnRing",   268, 64,  Confidence::VerifiedStatic, "sub_47BF90 v1+4*i+268", "16x dword button ring (btn nibble)" },
    { "cmd", "btnHold",   332, 64,  Confidence::VerifiedStatic, "sub_47BF90 v1+4*i+332", "16x dword btn hold counters" },
    { "cmd", "btnHead",   396, 1,   Confidence::VerifiedStatic, "sub_47BF90 (h+1)&0xF",  "button ring head index" },
    };
    count = sizeof(f) / sizeof(f[0]);
    return f;
}

}
