#include "qoh99/AddressMap.hpp"
#include <cstring>

namespace qoh99 {

using C = Confidence;
using M = MemClass;

static const AddressEntry kEntries[] = {
    { "sceneCurrent",      "dword_6234EC", addr::kSceneCurrent, 4, M::ControlFlow, C::RuntimeVerified, "live 1->3->2 capture" },
    { "sceneNext",         "dword_61801C", addr::kSceneNext, 4, M::ControlFlow, C::RuntimeVerified, "loop copies next->cur" },
    { "scene3Substate",    "dword_4AA454", addr::kScene3Substate, 4, M::ControlFlow, C::RuntimeVerified, "capture: 0/1/2/3" },
    { "scene3ConfirmFlag", "dword_4AA450", addr::kScene3ConfirmFlag, 4, M::ControlFlow, C::RuntimeVerified, "capture conf450 1->0; VS-list confirm" },
    { "scene3Cursor",      "dword_4AA458", addr::kScene3Cursor, 4, M::ControlFlow, C::RuntimeVerified, "VS-list row; char cursors live at 870954 + 0x460*slot" },
    { "vsListEntryFlag",   "dword_49EF00", addr::kVsListEntryFlag, 4, M::ControlFlow, C::RuntimeVerified, "sub_432E00 entry gate; NOT confirm" },
    { "mainMenuSubstate",  "dword_4AB8C4", addr::kMainMenuSubstate, 4, M::ControlFlow, C::RuntimeVerified, "capture; 4=launch" },
    { "mainMenuCursor",    "dword_4AB8D0", addr::kMainMenuCursor, 4, M::ControlFlow, C::RuntimeVerified, "CE +AB8D0" },
    { "menuModeState",     "dword_4AB784", addr::kMenuModeState, 4, M::ControlFlow, C::StaticAnalysis, "0-5 transitions" },
    { "matchSubstate",     "dword_8719A4", addr::kMatchSubstate, 4, M::MutableGameplay, C::RuntimeVerified, "0 whole match live; 5 at win" },
    { "vsFormationRow",    "dword_62369C", addr::kVsFormationRow, 4, M::MutableGameplay, C::RuntimeVerified, "applied VS formation: sub_432EE0 rows 0/1 -> 1 (two-fighter), rows 2..5 -> 2 (four-fighter 2v2); 0 = no VS formation applied, which the ARCADE menu entry (case 0) writes - the VS entry (case 1) writes 1" },
    { "controlMask",       "dword_6236CC", addr::kControlMask, 4, M::MutableGameplay, C::RuntimeVerified, "capture ctrl" },
    { "roundCounter",      "dword_871950", addr::kRoundCounter, 4, M::MutableGameplay, C::StaticAnalysis, "=5 at match end" },
    { "roundWinTable",     "dword_625628", addr::kRoundWinTable, 4, M::MutableGameplay, C::StaticAnalysis, "static analysis" },
    { "roundWinTable0",    "dword_625620", addr::kRoundWinTable0, 4, M::MutableGameplay, C::StaticAnalysis, "static analysis" },
    { "sceneInitBranch",   "dword_6236FC", addr::kSceneInitBranch, 4, M::ControlFlow, C::StaticAnalysis, "sub_48CA20 state0" },
    { "p1CharCursor",      "dword_870954", addr::kP1CharCursor, 4, M::ControlFlow, C::RuntimeVerified, "[280*player]; CE +470954" },
    { "p2CharCursor",      "dword_870DB4", addr::kP2CharCursor, 4, M::ControlFlow, C::StaticAnalysis, "+280*4" },
    { "charSelConfirmP1",  "dword_8716D0", addr::kCharSelConfirmP1, 4, M::ControlFlow, C::RuntimeVerified, "1->2->4 capture" },
    { "charSelConfirmP2",  "dword_8716D4", addr::kCharSelConfirmP2, 4, M::ControlFlow, C::RuntimeVerified, "1->2->4 capture" },
    { "charTypeTable",     "byte_49EEE4",  addr::kCharTypeTable, 25, M::ReadOnlyTable, C::StaticAnalysis, "24 row-major cells + FF sentinel" },
    { "matchConfigPtr",    "dword_613DE0", addr::kMatchConfigPtr, 4, M::MutableGameplay, C::RuntimeVerified, "POINTER (direct-boot bug proved deref)" },
    { "netConfigBlock",    "dword_617498", addr::kNetConfigBlock, 0x334, M::ControlFlow, C::StaticAnalysis, "frontend type-6 config landing; excluded from rollback" },
    { "netConfigBlock20",  "unk_616750",   addr::kNetConfigBlock20, 20, M::ControlFlow, C::StaticAnalysis, "frontend type-6 config landing; excluded from rollback" },
    { "matchConfigSnapshot","unk_623520",  addr::kMatchConfigSnapshot, 0x17C, M::MutableGameplay, C::RuntimeVerified, "launch snapshot" },
    { "vsModeFlagA",       "dword_613F14", addr::kVsModeFlagA, 4, M::ControlFlow, C::RuntimeVerified, "129/1" },
    { "vsHumanFlag",       "dword_4A37C8", addr::kVsHumanFlag, 4, M::ControlFlow, C::RuntimeVerified, "0/1" },
    { "fighterPtrTable",   "off_4A37B4",   addr::kFighterPtrTable, 16, M::MutableGameplay, C::RuntimeVerified, "live [623E00,626880,0,0]" },
    { "fighterBackupTable","dword_623500", addr::kFighterBackupTable, 16, M::MutableGameplay, C::StaticAnalysis, "sub_40AAD0 swap" },
    { "fighterP1",         "dword_623E00", addr::kFighterP1, addr::kFighterSize, M::MutableGameplay, C::RuntimeVerified, "196 qmemcpy 0x5FC sites + probe" },
    { "fighterP2",         "dword_626880", addr::kFighterP2, addr::kFighterSize, M::MutableGameplay, C::RuntimeVerified, "same" },
    { "fighterP3",         "dword_4ECB40", addr::kFighterP3, addr::kFighterSize, M::MutableGameplay, C::StaticAnalysis, "team" },
    { "fighterP4",         "dword_617A20", addr::kFighterP4, addr::kFighterSize, M::MutableGameplay, C::StaticAnalysis, "team" },
    { "helperCloneP1",     "dword_614060", addr::kHelperCloneP1, addr::kFighterSize, M::MutableGameplay, C::StaticAnalysis, "clone on move gate" },
    { "helperCloneP2",     "dword_61465C", addr::kHelperCloneP2, addr::kFighterSize, M::MutableGameplay, C::StaticAnalysis, "clone" },
    { "helperCloneP3",     "dword_614C58", addr::kHelperCloneP3, addr::kFighterSize, M::MutableGameplay, C::StaticAnalysis, "clone" },
    { "helperCloneP4",     "dword_615254", addr::kHelperCloneP4, addr::kFighterSize, M::MutableGameplay, C::StaticAnalysis, "clone" },
    { "effectPool",        "unk_618160",   addr::kEffectPool, 30 * addr::kFighterSize, M::MutableGameplay, C::StaticAnalysis, "sub_421EE0" },
    { "actorClonePool",    "byte_526FA0",  addr::kActorClonePool, 120 * addr::kFighterSize, M::MutableGameplay, C::StaticAnalysis, "sub_42DDB0" },
    { "shadowPool",        "unk_6275C0",   addr::kShadowPool, 16 * addr::kFighterSize, M::MutableGameplay, C::StrongLikely, "probe band 1532-3477" },
    { "fighterCopyScratch","unk_6287B4",   addr::kFighterCopyScratch, addr::kFighterSize, M::MutableGameplay, C::StaticAnalysis, "qmemcpy target" },
    { "arenaBlock0",       "unk_4ED140",   addr::kArenaBlock0, addr::kArenaBlockSize, M::MutableGameplay, C::RuntimeVerified, "ping-pong live" },
    { "arenaBlock1",       "(computed)",   addr::kArenaBlock1, addr::kArenaBlockSize, M::MutableGameplay, C::RuntimeVerified, "= block0 + 0x1CED0" },
    { "arenaActivePtr",    "dword_6234E8", addr::kArenaActivePtr, 4, M::MutableGameplay, C::RuntimeVerified, "PHASE TRAP: use this ptr, not selector" },
    { "arenaSelector",     "dword_623700", addr::kArenaSelector, 4, M::MutableGameplay, C::RuntimeVerified, "^=1/frame, out of phase" },
    { "arenaMirrorPtr",    "dword_6177CC", addr::kArenaMirrorPtr, 4, M::MutableGameplay, C::StaticAnalysis, "mirror ptr" },
    { "arenaHeadPtr",      "dword_623510", addr::kArenaHeadPtr, 4, M::MutableGameplay, C::StaticAnalysis, "head copy" },
    { "arenaSwapLatch",    "dword_6267D4", addr::kArenaSwapLatch, 4, M::MutableGameplay, C::StaticAnalysis, "per-frame latch" },
    { "battleLifeArray",   "dword_6254E0", addr::kBattleLifeArray, 4 * addr::kBattleLifeStride, M::MutableGameplay, C::StaticAnalysis, "sub_42D740: current+reserve-pendingDamage; <0 starts KO" },
    { "healthArray",       "dword_6265C8", addr::kHealthArray, 44 * 4, M::MutableGameplay, C::RuntimeVerified, "legacy name: regenerating guard/crush gauge, stride 44, 0..100" },
    { "exGaugeRecords",    "dword_6267E0", addr::kExGaugeRecords, addr::kExGaugeRecordStride * addr::kExGaugeStorageRecordCount, M::MutableGameplay, C::StaticAnalysis, "4x0x20 owner records; P1/P3 owner 0, P2/P4 owner 1; stocks +4 (0..9), fraction +1C (0..53)" },
    { "scoreArray",        "dword_626670", addr::kScoreArray, 16, M::MutableGameplay, C::RuntimeVerified, "CE p1 score" },
    { "rngSeedGameplay",   "dword_4A9938", addr::kRngSeedGameplay, 4, M::MutableGameplay, C::RuntimeVerified, "advances live" },
    { "rngSeedCosmetic",   "dword_4A993C", addr::kRngSeedCosmetic, 4, M::MutableGameplay, C::StaticAnalysis, "effects seed" },
    { "rngSelector",       "dword_4ABB24", addr::kRngSelector, 4, M::MutableGameplay, C::StaticAnalysis, "selects seed" },
    { "logicalMaskTable",  "dword_4A2A38", addr::kLogicalMaskTable, 32, M::ReadOnlyTable, C::StaticAnalysis, "8-entry mask table" },
    { "keyRemapTable",     "byte_4A2130",  addr::kKeyRemapTable, 256, M::ReadOnlyTable, C::StaticAnalysis, "remap" },
    { "keyState",          "dword_4AA920", addr::kKeyState, 256, M::MutableGameplay, C::StaticAnalysis, "poll target; injectable" },
    { "diActiveFlag",      "dword_4AA6F8", addr::kDiActiveFlag, 4, M::ControlFlow, C::StaticAnalysis, "DI ok flag" },
    { "inputBindingsPtr",  "dword_626860", addr::kInputBindingsPtr, 4, M::ControlFlow, C::StaticAnalysis, "null early init (crash cause)" },
    { "joyDeviceState",    "dword_4ECB20", addr::kJoyDeviceState, 4, M::MutableGameplay, C::StaticAnalysis, "joystick state" },
    { "inputSourceTable",  "dword_4A37C4", addr::kInputSourceTable, 16, M::MutableGameplay, C::StaticAnalysis, "per-fighter source" },
    { "netDispatchPtr",    "dword_66ECC0", addr::kNetDispatchPtr, 4, M::UnsafeOSPointer, C::StaticAnalysis, "= sub_45D6B0" },
    { "sessionInputWord",  "dword_66ECC4", addr::kSessionInputWord, 4, M::MutableGameplay, C::StaticAnalysis, "menu/session word" },
    { "netFrameSeq",       "dword_66ECCC", addr::kNetFrameSeq, 4, M::MutableGameplay, C::StaticAnalysis, "seq" },
    { "netRemoteInput",    "dword_66ECD4", addr::kNetRemoteInput, 4, M::MutableGameplay, C::StaticAnalysis, "unpacked remote input" },
    { "netParam5",         "dword_66ECD8", addr::kNetParam5, 4, M::MutableGameplay, C::StaticAnalysis, "type-5" },
    { "netInputRing",      "dword_66ECE0", addr::kNetInputRing, 12, M::MutableGameplay, C::StaticAnalysis, "depth 3" },
    { "netInputRingCount", "dword_66EE28", addr::kNetInputRingCount, 4, M::MutableGameplay, C::StaticAnalysis, "count" },
    { "netSyncPosXRing",   "word_66EE10",  addr::kNetSyncPosXRing, 6, M::MutableGameplay, C::StaticAnalysis, "3-word X ring indexed by input count" },
    { "netSyncPosYRing",   "word_66EE18",  addr::kNetSyncPosYRing, 6, M::MutableGameplay, C::StaticAnalysis, "3-word Y ring indexed by input count" },
    { "netSessionId",      "dword_66EE20", addr::kNetSessionId, 4, M::UnsafeOSPointer, C::StaticAnalysis, "DP id" },
    { "netInMatchFlag",    "dword_66EE38", addr::kNetInMatchFlag, 4, M::ControlFlow, C::RuntimeVerified, "0 whole local match" },
    { "netIsHost",         "dword_66EE00", addr::kNetIsHost, 4, M::ControlFlow, C::StaticAnalysis, "host-create=1/join=0; pre-scene wait gate" },
    { "netOnMatchStartPtr","dword_66EE08", addr::kNetOnMatchStartPtr, 4, M::UnsafeOSPointer, C::StaticAnalysis, "= sub_45D660" },
    { "netCbPtr66EE04",    "dword_66EE04", addr::kNetCbPtr66EE04, 4, M::UnsafeOSPointer, C::StrongLikely, "= sub_45D2B0" },
    { "netCbPtr66EBA4",    "dword_66EBA4", addr::kNetCbPtr66EBA4, 4, M::UnsafeOSPointer, C::StrongLikely, "= sub_45D2D0" },
    { "netCbPtr66ECC8",    "dword_66ECC8", addr::kNetCbPtr66ECC8, 4, M::UnsafeOSPointer, C::StrongLikely, "= sub_45D3D0" },
    { "netLocalSide",      "dword_4AAA44", addr::kNetLocalSide, 4, M::MutableGameplay, C::StaticAnalysis, "type-3" },
    { "netClientGotFlag",  "dword_4AAA48", addr::kNetClientGotFlag, 4, M::ControlFlow, C::RuntimeVerified, "peer-lock sets it" },
    { "netReadyFlag",      "dword_4AAA4C", addr::kNetReadyFlag, 4, M::ControlFlow, C::StaticAnalysis, "type-7" },
    { "netIsNetworkSession","dword_4AAA50",addr::kNetIsNetworkSession, 4, M::ControlFlow, C::RuntimeVerified, "CLI token" },
    { "chatCallbackPtr",   "dword_8A5C68", addr::kChatCallbackPtr, 4, M::UnsafeOSPointer, C::StaticAnalysis, "external cb" },
    { "cameraZoomA",       "dbl_49B530",   addr::kCameraZoomA, 8, M::MutableGameplay, C::RuntimeVerified, "zoom -> transforms" },
    { "cameraZoomB",       "dbl_49B538",   addr::kCameraZoomB, 8, M::MutableGameplay, C::RuntimeVerified, "zoom pair" },
    { "hudHitDescriptorA", "dword_8807C0", addr::kHudHitDescriptorA, 4, M::RenderCosmetic, C::RuntimeVerified, "sub_415E10 feed" },
    { "hudHitDescriptorB", "dword_8808CC", addr::kHudHitDescriptorB, 4, M::RenderCosmetic, C::RuntimeVerified, "sub_415E10 feed" },
    { "hudDisplayA",       "dword_87D8FC", addr::kHudDisplayA, 4, M::RenderCosmetic, C::RuntimeVerified, "hud_display_head" },
    { "hudDisplayB",       "dword_87D890", addr::kHudDisplayB, 4, M::RenderCosmetic, C::RuntimeVerified, "hud_display_head" },
    { "hudTimerDigits",    "dword_8A5CA0", addr::kHudTimerDigits, 4, M::MutableGameplay, C::RuntimeVerified, "hud_counters" },
    { "hudComboDigits",    "dword_8A5CA8", addr::kHudComboDigits, 4, M::MutableGameplay, C::RuntimeVerified, "affects damage" },
    { "hudTextBuffer",     "byte_623940",  addr::kHudTextBuffer, 64, M::RenderCosmetic, C::RuntimeVerified, "wsprintfA buffer" },
    { "frameCounterCosmetic","dword_4AB778",addr::kFrameCounterCosmetic, 4, M::RenderCosmetic, C::RuntimeVerified, "wide-diff residual" },
    { "frameTimeRing",     "dword_4ABA2C", addr::kFrameTimeRing, 0x40, M::WallClockPacing, C::RuntimeVerified, "timeGetTime ring — NEVER save" },
    { "ddraw2Ptr",         "dword_66FACC", addr::kDdraw2Ptr, 4, M::UnsafeOSPointer, C::StaticAnalysis, "IDirectDraw2*" },
    { "appActiveFlag",     "dword_6248B0", addr::kAppActiveFlag, 4, M::UnsafeOSPointer, C::StaticAnalysis, "WM_ACTIVATEAPP-derived local window state; never save/hash" },
    { "spriteSurfaceTable","dword_68BC00", addr::kSpriteSurfaceTable, 4, M::UnsafeOSPointer, C::RuntimeVerified, "load-once surfaces" },
    { "spriteMetaTable",   "dword_68C020", addr::kSpriteMetaTable, 124, M::ReadOnlyTable, C::RuntimeVerified, "load-once metadata" },
    { "openingMovieFilename","byte_4AB794",addr::kOpeningMovieFilename, 1, M::ControlFlow, C::RuntimeVerified, "resolved Opening.avi path (sub_46F9E0); non-empty selects the MCI digital-video scene 4" },
    { "force16BitFlag",    "dword_4AB898", addr::kForce16BitFlag, 4, M::ControlFlow, C::StaticAnalysis, "bpp gate" },
    { "configFileImage",   "unk_615860",   addr::kConfigFileImage, 0x1F6C, M::ControlFlow, C::StaticAnalysis, "QOHcnf.key" },
    { "commandTable",      "dword_4A857C", addr::kCommandTable, 4, M::ReadOnlyTable, C::StaticAnalysis, "sub_47B910 patterns" },
    { "dsoundBufferA",     "dword_4AAAE4", addr::kDsoundBufferA, 4, M::UnsafeOSPointer, C::StaticAnalysis, "IDirectSoundBuffer*" },
    { "dsoundBufferB",     "dword_4AB118", addr::kDsoundBufferB, 4, M::UnsafeOSPointer, C::StaticAnalysis, "IDirectSoundBuffer*" },
    { "mciLastError",      "dword_4EC988", addr::kMciLastError, 4, M::ControlFlow, C::StaticAnalysis, "mci result" },
};

static const LegacyNameAlias kLegacyNames[] = {
    { "matchMode", "vsFormationRow",
      "dword_62369C selects the VS formation (1 two-fighter / 2 four-fighter), "
      "not a story/special match mode" },
    { "teamSize",  "vsFormationRow",
      "the value is a formation code, not a count of fighters per team" },
};

const AddressEntry* AddressMap::entries(std::size_t& count) {
    count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

const AddressEntry* AddressMap::findExact(const char* semanticName) {
    for (const auto& e : kEntries)
        if (std::strcmp(e.name, semanticName) == 0)
            return &e;
    return nullptr;
}

const AddressEntry* AddressMap::find(const char* semanticName) {
    if (const AddressEntry* e = findExact(semanticName))
        return e;
    for (const auto& alias : kLegacyNames)
        if (std::strcmp(alias.legacyName, semanticName) == 0)
            return findExact(alias.currentName);
    return nullptr;
}

const LegacyNameAlias* AddressMap::legacyNames(std::size_t& count) {
    count = sizeof(kLegacyNames) / sizeof(kLegacyNames[0]);
    return kLegacyNames;
}

const AddressEntry* AddressMap::findByVa(GameAddr va) {
    for (const auto& e : kEntries)
        if (e.va == va)
            return &e;
    return nullptr;
}

}
