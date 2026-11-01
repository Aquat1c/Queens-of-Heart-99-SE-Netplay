#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "DirectBoot.h"
#include "../GameHooks.h"
#include "../qoh99_game.h"
#include "../qoh99/qoh99_functions.h"
#include "../qoh99/qoh99_probe.h"
#include "../Log.h"
#include "qoh99/generated/NativeSymbolCatalog.hpp"

namespace qgame {
using namespace qoh99;
namespace {

typedef int (__cdecl* VoidRetFn)();

bool g_active = false;
bool g_done   = false;
bool g_failed = false;
bool g_networkDriven = false;
bool g_offlineDispatchTicked = false;
bool g_trainingSelection = false;
bool g_trainingAwaitReturnCharSelect = false;
bool g_replaySelection = false;
ReplayDirectBootConfig g_replayConfig{};
int  g_vsRow  = 1;
bool g_stopAtCharSel = false;
bool g_unattendedRematch = false;
uint64_t g_forcedLaunchAt = 0;
directboot::TrainingSelectionPolicy g_trainingSelectionPolicy;
bool g_trainingStageCursorObserved = false;
uint32_t g_trainingLastStageCursor = 0u;

inline uint32_t rd32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
inline void     wr32(uintptr_t a, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(a) = v; }

constexpr uintptr_t kSceneNext   = 0x61801C;

namespace nsc = qoh99::generated_model;

constexpr uintptr_t kSub_434710 = static_cast<uintptr_t>(
    nsc::nativeSymbolNovemberVa(nsc::NativeSymbolId::DirectBootApplyRoster));
static_assert(kSub_434710 == 0x434710u,
              "sub_434710 must resolve to the November address this file has "
              "always called");
constexpr uintptr_t kMatchFormationRow = 0x62369C;
// dword_6236A0: boolean "a VS formation has been applied"; written to 1 by every sub_432EE0 row.
constexpr uintptr_t kVsFormationApplied = 0x6236A0;
constexpr uintptr_t kCpuSlotMask = 0x6236CC;
constexpr uintptr_t kP1InputSource = 0x4A37C4;
constexpr uintptr_t kP2InputSource = 0x4A37C8;
constexpr uintptr_t kP1InputRoute   = 0x613F10;
constexpr uintptr_t kP2InputRoute  = 0x613F14;
constexpr uintptr_t kP1CharacterSelectState = 0x8716D0;
constexpr uintptr_t kP2CharacterSelectState = 0x8716D4;
constexpr uintptr_t kNativeConfirmationLatch = 0x8718B8;
constexpr uintptr_t kP2SelectorOwner = 0x870980;
// dword_6236D4: scene-2's committed copy of the CPU slot mask; match init (sub_428430 case -1) copies dword_6236CC here and zeroes the source for an applied VS formation.
constexpr uintptr_t kMatchCommittedCpuMask = 0x6236D4;

bool g_captureMode = false;

uint64_t g_frame = 0;

constexpr uintptr_t kScene     = 0x6234EC;   // 1=menu 2=match 3=scene3
constexpr uintptr_t kNext      = 0x61801C;
constexpr uintptr_t kSub48     = 0x4AB8C4;   // main-menu substate (2 = menu up)
constexpr uintptr_t kS3sub     = 0x4AA454;   // scene-3 substate: 1=VS-list, 2=CHARSELECT
constexpr uintptr_t kCur458    = 0x4AA458;
constexpr uintptr_t kConf450   = 0x4AA450;
constexpr uintptr_t k49EF00    = 0x49EF00;
constexpr uintptr_t kMatchStart= 0x8716E8;   // dword_8716E8: "chars confirmed, proceed" (sub_434C80 LABEL_21)
constexpr uintptr_t kP1CharCur = 0x870954;         // dword_870954[0]   P1 char cursor
constexpr uintptr_t kP2CharCur = 0x870954 + 280*4; // dword_870954[280] P2 char cursor
constexpr uintptr_t kSelectorStride = 0x460;
constexpr uintptr_t kSelectorPalette = 0x870940;
constexpr uintptr_t kSelectorAlternate = 0x87095C;
constexpr uintptr_t kStageSelectionKind = 0x6236E4;
constexpr uintptr_t kStageSelectionValue = 0x6236E8;
constexpr uintptr_t kLogicalStage = 0x4ECB1C;

struct Watch { const char* name; uintptr_t addr; uint32_t prev; };
Watch g_watch[] = {
    { "scene@6234EC",  0x6234EC, 0xFFFFFFFF },
    { "next@61801C",   0x61801C, 0xFFFFFFFF },
    { "sub48@4AB8C4",  0x4AB8C4, 0xFFFFFFFF },
    { "menuCur@4AB8D0",0x4AB8D0, 0xFFFFFFFF },
    { "s3sub@4AA454",  0x4AA454, 0xFFFFFFFF },
    { "cur458@4AA458", 0x4AA458, 0xFFFFFFFF },
    { "conf450@4AA450",0x4AA450, 0xFFFFFFFF },
    { "f49EF00@49EF00",0x49EF00, 0xFFFFFFFF },
    { "formation@62369C", 0x62369C, 0xFFFFFFFF },
    { "applied@6236A0",   0x6236A0, 0xFFFFFFFF },
    { "cpuMask@6236CC",   0x6236CC, 0xFFFFFFFF },
    { "human0@8716D0", 0x8716D0, 0xFFFFFFFF },
    { "human1@8716D4", 0x8716D4, 0xFFFFFFFF },
    { "p1char@623EAC", 0x623EAC, 0xFFFFFFFF },
    { "p2char@62692C", 0x62692C, 0xFFFFFFFF },
    { "inMatch@66EE38",0x66EE38, 0xFFFFFFFF },
};

void captureTick() {
    g_frame++;
    bool changed = false;
    char line[512]; int n = 0;
    for (Watch& w : g_watch) {
        uint32_t v = rd32(w.addr);
        if (v != w.prev) { changed = true; }
    }
    if (!changed) return;
    n += wsprintfA(line + n, "[cap] f=%llu ", (unsigned long long)g_frame);
    for (Watch& w : g_watch) {
        uint32_t v = rd32(w.addr);
        const char* mark = (v != w.prev) ? "*" : "";
        n += wsprintfA(line + n, "%s%s=%d ", mark, w.name, (int)v);
        w.prev = v;
        if (n > 440) break;
    }
    QLOG("%s", line);
}

enum Phase { P_Menu, P_VsList, P_CharSel, P_Match, P_Done };
const char* phName(int p){ static const char* n[]={"Menu","VsList","CharSel","Match","Done"}; return (p>=0&&p<=P_Done)?n[p]:"?"; }
int      g_phase = P_Menu;
uint64_t g_phaseStart = 0;

void bootLog(const char* tag) {
    QLOG("[boot] %s phase=%s f=%llu scene=%d sub48=%d s3sub=%d "
         "cur458=%d conf450=%d 49EF00=%d formation=%d applied=%d cpuMask=%d "
         "human=%d/%d p1c=%d p2c=%d",
         tag, phName(g_phase), (unsigned long long)g_frame,
         (int)rd32(kScene), (int)rd32(kSub48), (int)rd32(kS3sub),
         (int)rd32(kCur458), (int)rd32(kConf450), (int)rd32(k49EF00),
         (int)rd32(kMatchFormationRow), (int)rd32(kVsFormationApplied),
         (int)rd32(kCpuSlotMask),
         (int)rd32(kP1CharacterSelectState),
         (int)rd32(kP2CharacterSelectState),
         (int)rd32(kP1CharCur), (int)rd32(kP2CharCur));
}

void trainingSelectionTick(int scene, int s3sub) {
    directboot::TrainingSelectionSample sample{};
    sample.scene = static_cast<uint32_t>(scene);
    sample.sceneSubstate = static_cast<uint32_t>(s3sub);
    sample.sceneNext = rd32(kSceneNext);
    sample.matchMode = rd32(kMatchFormationRow);
    sample.controlMask = rd32(kCpuSlotMask);
    sample.p1State = rd32(kP1CharacterSelectState);
    sample.p2State = rd32(kP2CharacterSelectState);
    sample.p1InputSource = rd32(kP1InputSource);
    sample.p2InputSource = rd32(kP2InputSource);
    sample.p2Route = rd32(kP2InputRoute);
    sample.stageSelectionActive = scene == 3 && s3sub == 2
        && rd32(kMatchStart) == 1u;
    if (scene == 3 && s3sub == 2) {
        const fn::ReadLogicalFn readLogical = fn::readLogical();
        if (!readLogical) {
            QLOG("[training-select] FAILED: no qualified profile logical-input address");
            g_failed = true;
            g_done = true;
            g_phase = P_Done;
            return;
        }
        sample.p1LogicalMask = static_cast<uint32_t>(readLogical(0));
    }

    const directboot::TrainingSelectionPhase before =
        g_trainingSelectionPolicy.phase();
    const directboot::TrainingSelectionDecision decision =
        g_trainingSelectionPolicy.advance(sample);

    if (decision.writeP2InputSource)
        wr32(kP2InputSource, decision.p2InputSource);
    if (decision.writeP2Route)
        wr32(kP2InputRoute, decision.p2Route);
    if (decision.resetP2SelectorOwner)
        wr32(kP2SelectorOwner, 0);
    if (decision.writeP2State)
        wr32(kP2CharacterSelectState, decision.p2State);

    if (sample.stageSelectionActive) {
        const uint32_t cursor = rd32(kStageSelectionValue);
        if (!g_trainingStageCursorObserved
            || cursor != g_trainingLastStageCursor) {
            QLOG("[training-select] stage cursor=%u source=%u/%u",
                 cursor, rd32(kP1InputSource), rd32(kP2InputSource));
            g_trainingLastStageCursor = cursor;
            g_trainingStageCursorObserved = true;
        }
    } else {
        g_trainingStageCursorObserved = false;
    }

    if (decision.failure !=
        directboot::TrainingSelectionFailure::None) {
        QLOG("[training-select] FAILED: %s scene=%d sub=%d next=%u "
             "mode=%u ctrl=%u state=%u/%u source=%u/%u route2=%u",
             directboot::trainingSelectionFailureText(decision.failure),
             scene, s3sub, sample.sceneNext, sample.matchMode,
             sample.controlMask, sample.p1State, sample.p2State,
             sample.p1InputSource, sample.p2InputSource, sample.p2Route);
        g_failed = true;
        g_done = true;
        g_phase = P_Done;
        return;
    }

    if (g_trainingSelectionPolicy.phase() != before) {
        QLOG("[training-select] phase %u -> %u p1State=%u p2State=%u "
             "buttons=%X source2=%u next=%u",
             static_cast<unsigned>(before),
             static_cast<unsigned>(g_trainingSelectionPolicy.phase()),
             sample.p1State, sample.p2State,
             sample.p1LogicalMask & 0x0Fu, rd32(kP2InputSource),
             sample.sceneNext);
    }
    if (decision.completed) {
        QLOG("[training-select] native row-1 handoff complete; "
             "P2 source=%u route=%u logical-stage=%u",
             rd32(kP2InputSource), rd32(kP2InputRoute),
             rd32(kLogicalStage));
        g_phase = P_Done;
        g_done = true;
    }
}

void tick() {
    g_frame++;
    if (g_captureMode) { captureTick(); return; }

    int scene = (int)rd32(kScene);
    int s3sub = (int)rd32(kS3sub);
    int sub48 = (int)rd32(kSub48);
    if (g_frame == 1)
        bootLog("first dispatcher-owned tick");

    switch (g_phase) {
        case P_Menu:
            if (scene == 1 && sub48 == 2) {
                wr32(kNext, 3);
                wr32(k49EF00, 1);
                wr32(kMatchFormationRow, 1);
                wr32(kCpuSlotMask, 2);
                bootLog("menu -> VS confirm");
                g_phase = P_VsList; g_phaseStart = g_frame;
            } else if (g_trainingSelection
                       && g_frame - g_phaseStart > 600) {
                bootLog("training: main-menu bootstrap TIMEOUT");
                g_failed = true;
                g_done = true;
                g_phase = P_Done;
            }
            break;

        case P_VsList:
            if (scene == 3 && s3sub == 1) {
                wr32(kCur458, (uint32_t)g_vsRow);
                wr32(kConf450, 1);
                bootLog("VS-list -> select row + confirm");
                g_phase = P_CharSel; g_phaseStart = g_frame;
            } else if (g_frame - g_phaseStart > 300) {
                bootLog("VsList TIMEOUT");
                g_failed = true;
                g_done = true;
                g_phase = P_Done;
            }
            break;

        case P_CharSel:
            if (g_trainingSelection) {
                if (g_trainingAwaitReturnCharSelect) {
                    if (scene == 3 && s3sub == 2) {
                        const directboot::TrainingReturnedStageResetDecision
                            stageReset =
                                directboot::decideTrainingReturnedStageReset(
                                    g_trainingSelection,
                                    g_trainingAwaitReturnCharSelect,
                                    static_cast<uint32_t>(scene),
                                    static_cast<uint32_t>(s3sub));
                        if (stageReset.apply) {
                            const uint32_t previousKind =
                                rd32(kStageSelectionKind);
                            const uint32_t previousValue =
                                rd32(kStageSelectionValue);
                            wr32(kStageSelectionKind,
                                 stageReset.selectionKind);
                            wr32(kStageSelectionValue,
                                 stageReset.selectionValue);
                            g_trainingStageCursorObserved = false;
                            QLOG("[training-select] returned selector reset "
                                 "stage kind/cursor %u/%u -> %u/%u",
                                 previousKind, previousValue,
                                 stageReset.selectionKind,
                                 stageReset.selectionValue);
                        }
                        g_trainingAwaitReturnCharSelect = false;
                        g_trainingSelectionPolicy.reset();
                        g_phaseStart = g_frame;
                        bootLog("training: returned to character select; "
                                "selector handoff re-armed");
                        trainingSelectionTick(scene, s3sub);
                    } else if (g_frame - g_phaseStart > 600) {
                        bootLog("training: return-to-charselect TIMEOUT");
                        g_failed = true;
                        g_done = true;
                        g_phase = P_Done;
                    }
                    break;
                }
                if (scene == 3 && s3sub == 1) {
                    bootLog("training: P1 cancel -> re-enter native row 1");
                    g_trainingSelectionPolicy.reset();
                    g_phase = P_VsList;
                    g_phaseStart = g_frame;
                    break;
                }
                trainingSelectionTick(scene, s3sub);
                break;
            }
            if (scene == 3 && s3sub == 2) {
                if (g_stopAtCharSel) {
                    if (directboot::isNetworkTwoHumanCharacterSelectHandoff(
                            g_networkDriven, g_vsRow)) {
                        directboot::NetworkTwoHumanCharacterSelectObservation
                            observation{};
                        observation.vsRow = static_cast<uint32_t>(g_vsRow);
                        observation.scene = static_cast<uint32_t>(scene);
                        observation.sceneSubstate =
                            static_cast<uint32_t>(s3sub);
                        observation.matchFormationRow =
                            rd32(kMatchFormationRow);
                        observation.vsFormationApplied =
                            rd32(kVsFormationApplied);
                        observation.cpuSlotMask = rd32(kCpuSlotMask);
                        observation.p1InputSource = rd32(kP1InputSource);
                        observation.p2InputSource = rd32(kP2InputSource);
                        observation.p1InputRoute = rd32(kP1InputRoute);
                        observation.p2InputRoute = rd32(kP2InputRoute);
                        observation.p1ConfirmationState =
                            rd32(kP1CharacterSelectState);
                        observation.p2ConfirmationState =
                            rd32(kP2CharacterSelectState);
                        observation.nativeConfirmationLatch =
                            rd32(kNativeConfirmationLatch);
                        observation.p1Cursor = rd32(kP1CharCur);
                        observation.p2Cursor = rd32(kP2CharCur);
                        observation.p1PaletteOrForm =
                            rd32(kSelectorPalette);
                        observation.p2PaletteOrForm =
                            rd32(kSelectorPalette + kSelectorStride);
                        const auto status =
                            directboot::observeNetworkTwoHumanCharacterSelect(
                                observation);
                        const auto handoff =
                            directboot::decideNetworkTwoHumanCharacterSelectHandoff(
                                observation);
                        if (handoff == directboot::
                                           NetworkTwoHumanCharacterSelectHandoffAction::
                                               FailClosed) {
                            QLOG("[boot.charsel] network row-1 refused: %s "
                                 "formation=%u policy=%u cpuMask=%u "
                                 "source=%u/%u route=%u/%u confirm=%u/%u latch=%u",
                                 directboot::networkTwoHumanCharacterSelectStatusText(
                                     status),
                                 observation.matchFormationRow,
                                 observation.vsFormationApplied,
                                 observation.cpuSlotMask,
                                 observation.p1InputSource,
                                 observation.p2InputSource,
                                 observation.p1InputRoute,
                                 observation.p2InputRoute,
                                 observation.p1ConfirmationState,
                                 observation.p2ConfirmationState,
                                 observation.nativeConfirmationLatch);
                            g_failed = true;
                            g_done = true;
                            g_phase = P_Done;
                            break;
                        }
                        QLOG("[boot.charsel] network row-1 native state=%s "
                             "formation=%u policy=%u cpuMask=%u "
                             "source=%u/%u route=%u/%u confirm=%u/%u latch=%u",
                             directboot::networkTwoHumanCharacterSelectStatusText(
                                 status),
                             observation.matchFormationRow,
                             observation.vsFormationApplied,
                             observation.cpuSlotMask,
                             observation.p1InputSource,
                             observation.p2InputSource,
                             observation.p1InputRoute,
                             observation.p2InputRoute,
                             observation.p1ConfirmationState,
                             observation.p2ConfirmationState,
                             observation.nativeConfirmationLatch);
                    } else {
                        directboot::TwoFighterCharacterSelectObservation
                            offline{};
                        offline.vsRow = static_cast<uint32_t>(g_vsRow);
                        offline.scene = static_cast<uint32_t>(scene);
                        offline.sceneSubstate = static_cast<uint32_t>(s3sub);
                        offline.matchFormationRow = rd32(kMatchFormationRow);
                        offline.vsFormationApplied = rd32(kVsFormationApplied);
                        offline.cpuSlotMask = rd32(kCpuSlotMask);
                        offline.p1ConfirmationState =
                            rd32(kP1CharacterSelectState);
                        offline.p2ConfirmationState =
                            rd32(kP2CharacterSelectState);
                        offline.nativeConfirmationLatch =
                            rd32(kNativeConfirmationLatch);
                        const auto offlineStatus =
                            directboot::observeTwoFighterCharacterSelect(
                                offline);
                        if (directboot::
                                decideTwoFighterCharacterSelectHandoff(offline)
                            == directboot::
                                   TwoFighterCharacterSelectHandoffAction::
                                       FailClosed) {
                            QLOG("[boot.charsel] offline row-%d refused: %s "
                                 "formation=%u applied=%u cpuMask=%u "
                                 "confirm=%u/%u latch=%u",
                                 g_vsRow,
                                 directboot::
                                     twoFighterCharacterSelectStatusText(
                                         offlineStatus),
                                 offline.matchFormationRow,
                                 offline.vsFormationApplied,
                                 offline.cpuSlotMask,
                                 offline.p1ConfirmationState,
                                 offline.p2ConfirmationState,
                                 offline.nativeConfirmationLatch);
                            g_failed = true;
                            g_done = true;
                            g_phase = P_Done;
                            break;
                        }
                        QLOG("[boot.charsel] offline row-%d native state=%s "
                             "formation=%u applied=%u cpuMask=%u "
                             "confirm=%u/%u latch=%u",
                             g_vsRow,
                             directboot::twoFighterCharacterSelectStatusText(
                                 offlineStatus),
                             offline.matchFormationRow,
                             offline.vsFormationApplied,
                             offline.cpuSlotMask,
                             offline.p1ConfirmationState,
                             offline.p2ConfirmationState,
                             offline.nativeConfirmationLatch);
                    }
                    bootLog("CHARSELECT reached - players choose from here");
                    g_phase = P_Done; g_done = true;
                    break;
                }
                if (g_replaySelection) {
                    for (size_t slot = 0u; slot < 2u; ++slot) {
                        const uintptr_t base = kSelectorStride * slot;
                        wr32(kP1CharCur + base,
                             g_replayConfig.slots[slot].cursorCell);
                        wr32(kSelectorAlternate + base,
                             g_replayConfig.slots[slot].alternateFlag);
                        wr32(kSelectorPalette + base,
                             g_replayConfig.slots[slot].paletteIndex);
                    }
                    wr32(kStageSelectionKind, 2u);
                    wr32(kStageSelectionValue,
                         g_replayConfig.logicalStageIndex);
                    wr32(kLogicalStage,
                         g_replayConfig.logicalStageIndex);
                    if (((g_frame - g_phaseStart) % 8) == 0) {
                        uint32_t h0 = rd32(kP1CharacterSelectState),
                                 h1 = rd32(kP2CharacterSelectState);
                        if (h0 < 4) wr32(kP1CharacterSelectState, h0 + 1);
                        if (h1 < 4) wr32(kP2CharacterSelectState, h1 + 1);
                        if (((g_frame - g_phaseStart) % 40) == 0)
                            bootLog("charsel: advancing confirm flags");
                    }
                    if (rd32(kP1CharacterSelectState) == 4
                        && rd32(kP2CharacterSelectState) == 4
                        && rd32(kScene) == 3) {
                        if (!g_forcedLaunchAt) g_forcedLaunchAt = g_frame;
                        if (g_frame - g_forcedLaunchAt >= 8
                            && rd32(kSceneNext) != 2) {
                            wr32(kMatchStart, 1);
                            wr32(kNativeConfirmationLatch, 30);
                            reinterpret_cast<VoidRetFn>(kSub_434710)();
                            bootLog("charsel: replay launch via sub_434710 "
                                    "(roster committed from the selector)");
                        }
                    }
                } else {
                    if (((g_frame - g_phaseStart) % 300) == 0)
                        bootLog("charsel: unattended wait - harness presses "
                                "drive the native selector");
                }
            }
            if (scene == 2) {
                bootLog("MATCH reached");
                if (g_unattendedRematch) {
                    directboot::MatchEntryObservation entry{};
                    entry.vsRow = static_cast<uint32_t>(g_vsRow);
                    entry.sceneNext = rd32(kSceneNext);
                    entry.matchFormationRow = rd32(kMatchFormationRow);
                    entry.vsFormationApplied = rd32(kVsFormationApplied);
                    entry.cpuSlotMask = rd32(kMatchCommittedCpuMask);
                    const auto entryStatus =
                        directboot::observeTwoFighterMatchEntry(entry);
                    QLOG("[boot.charsel] native launch row=%d entry=%s "
                         "formation=%u applied=%u cpuMask=%u",
                         g_vsRow,
                         directboot::matchEntryStatusText(entryStatus),
                         entry.matchFormationRow,
                         entry.vsFormationApplied,
                         entry.cpuSlotMask);
                    if (entryStatus
                        != directboot::MatchEntryStatus::
                               TwoFighterMatchEntryVerified) {
                        bootLog("charsel: match entry REFUSED");
                        g_failed = true;
                        g_done = true;
                        g_phase = P_Done;
                        break;
                    }
                    g_phase = P_Match;
                    g_phaseStart = g_frame;
                    g_forcedLaunchAt = 0;
                } else {
                    g_phase = P_Done;
                    g_done = true;
                }
            }
            else if (g_frame - g_phaseStart > 1800) {
                bootLog("CharSel TIMEOUT");
                g_failed = true;
                g_done = true;
                g_phase = P_Done;
            }
            break;

        case P_Match:
            if (scene == 3 && s3sub == 2) {
                bootLog("match returned to character select - re-arming "
                        "unattended boot");
                g_phase = P_CharSel;
                g_phaseStart = g_frame;
                g_forcedLaunchAt = 0;
            }
            break;

        default: break;
    }
    (void)sub48;
}

}

bool startDirectBoot(int vsRow) {
    g_vsRow = vsRow;
    g_stopAtCharSel = false;
    g_frame = 0;
    g_phase = P_Menu;
    g_phaseStart = 0;
    g_done = false;
    g_failed = false;
    g_networkDriven = false;
    g_offlineDispatchTicked = false;
    g_unattendedRematch = false;
    g_forcedLaunchAt = 0;
    g_trainingSelection = false;
    g_trainingAwaitReturnCharSelect = false;
    g_replaySelection = false;
    g_unattendedRematch = true;
    g_trainingSelectionPolicy.reset();
    g_active = true;
    QLOG("[boot] direct-boot armed (verified substate machine for the menus; "
         "the harness press driver plays character select and the native "
         "gate launches), vsRow=%d captureMode=%d unattended-rematch=1",
         vsRow, g_captureMode);
    return true;
}

bool startDirectBootToCharSelect(int vsRow) {
    g_vsRow = vsRow;
    g_stopAtCharSel = true;
    g_frame = 0;
    g_phase = P_Menu;
    g_phaseStart = 0;
    g_done = false;
    g_failed = false;
    g_networkDriven = false;
    g_offlineDispatchTicked = false;
    g_unattendedRematch = false;
    g_forcedLaunchAt = 0;
    g_trainingSelection = false;
    g_trainingAwaitReturnCharSelect = false;
    g_replaySelection = false;
    g_trainingSelectionPolicy.reset();
    g_active = true;
    QLOG("[boot] direct-boot armed -> STOP AT CHARACTER SELECT (netplay: each player picks their own), vsRow=%d", vsRow);
    return true;
}

bool startTrainingDirectBootToCharSelect(int vsRow) {
    if (vsRow != 1) {
        QLOG("[training-select] refused direct-boot row %d; training "
             "requires native row 1 (1P vs 2P)", vsRow);
        return false;
    }
    g_vsRow = 1;
    g_stopAtCharSel = true;
    g_frame = 0;
    g_phase = P_Menu;
    g_phaseStart = 0;
    g_done = false;
    g_failed = false;
    g_networkDriven = false;
    g_offlineDispatchTicked = false;
    g_unattendedRematch = false;
    g_forcedLaunchAt = 0;
    g_trainingSelection = true;
    g_trainingAwaitReturnCharSelect = false;
    g_replaySelection = false;
    g_trainingSelectionPolicy.reset();
    g_active = true;
    QLOG("[boot] explicit TRAINING direct-boot armed -> native row 1, "
         "P1 selects P1 then P2");
    return true;
}

void rearmTrainingCharacterSelect() {
    g_vsRow = 1;
    g_stopAtCharSel = true;
    g_phase = P_CharSel;
    g_phaseStart = g_frame;
    g_done = false;
    g_failed = false;
    g_networkDriven = false;
    g_offlineDispatchTicked = false;
    g_unattendedRematch = false;
    g_forcedLaunchAt = 0;
    g_trainingSelection = true;
    g_trainingAwaitReturnCharSelect = true;
    g_replaySelection = false;
    g_trainingSelectionPolicy.reset();
    g_active = true;
    QLOG("[training-select] waiting for native return to character select");
}

bool startNetworkDirectBootToCharSelect(int vsRow) {
    g_vsRow = vsRow;
    g_stopAtCharSel = true;
    g_frame = 0;
    g_phase = P_Menu;
    g_phaseStart = 0;
    g_done = false;
    g_failed = false;
    g_networkDriven = true;
    g_offlineDispatchTicked = false;
    g_unattendedRematch = false;
    g_forcedLaunchAt = 0;
    g_trainingSelection = false;
    g_trainingAwaitReturnCharSelect = false;
    g_replaySelection = false;
    g_trainingSelectionPolicy.reset();
    g_active = true;
    QLOG("[boot] deterministic network-driven boot armed -> character select "
         "(render callback disabled), vsRow=%d", vsRow);
    return true;
}

bool startReplayDirectBootToMatch(const ReplayDirectBootConfig& config) {
    if (config.vsRow != 1u || config.logicalStageIndex >= 28u) {
        QLOG("[boot.replay] refused row=%u stage=%u",
             config.vsRow, config.logicalStageIndex);
        return false;
    }
    for (size_t slot = 0u; slot < 2u; ++slot) {
        if (config.slots[slot].cursorCell >= 24u
            || config.slots[slot].alternateFlag > 1u
            || config.slots[slot].paletteIndex > 3u
            || config.slots[slot].confirmState == 0u
            || config.slots[slot].confirmState > 4u) {
            QLOG("[boot.replay] refused selector slot=%u cell=%u alt=%u "
                 "palette=%u confirm=%u",
                 static_cast<unsigned>(slot),
                 config.slots[slot].cursorCell,
                 config.slots[slot].alternateFlag,
                 config.slots[slot].paletteIndex,
                 config.slots[slot].confirmState);
            return false;
        }
    }
    g_replayConfig = config;
    g_vsRow = config.vsRow;
    g_stopAtCharSel = false;
    g_frame = 0u;
    g_phase = P_Menu;
    g_phaseStart = 0u;
    g_done = false;
    g_failed = false;
    g_networkDriven = true;
    g_offlineDispatchTicked = false;
    g_unattendedRematch = false;
    g_forcedLaunchAt = 0;
    g_trainingSelection = false;
    g_trainingAwaitReturnCharSelect = false;
    g_replaySelection = true;
    g_trainingSelectionPolicy.reset();
    g_active = true;
    QLOG("[boot.replay] deterministic recorded match boot armed stage=%u "
         "cells=%u/%u alt=%u/%u palette=%u/%u",
         config.logicalStageIndex,
         config.slots[0].cursorCell, config.slots[1].cursorCell,
         config.slots[0].alternateFlag,
         config.slots[1].alternateFlag,
         config.slots[0].paletteIndex,
         config.slots[1].paletteIndex);
    return true;
}

bool directBootNetworkActive() {
    return g_active && g_networkDriven && !g_done;
}

bool directBootOfflineActive() {
    return g_active && !g_networkDriven && !g_done;
}

bool directBootUnattendedFrontendActive() {
    return g_active && !g_done && !g_networkDriven && g_unattendedRematch;
}

bool directBootOfflineFailed() {
    return g_active && !g_networkDriven && g_done && g_failed;
}

void directBootOfflineTick() {
    if (!directBootOfflineActive()) return;
    g_offlineDispatchTicked = true;
    __try {
        tick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        QLOG("[boot] deterministic offline dispatch tick FAULTED");
        g_failed = true;
        g_done = true;
        g_phase = P_Done;
    }
}

bool directBootNetworkComplete() {
    return g_active && g_networkDriven && g_done && !g_failed;
}

bool directBootNetworkFailed() {
    return g_active && g_networkDriven && g_done && g_failed;
}

bool directBootNetworkAtStartBarrier() {
    return g_active && g_networkDriven && !g_done
        && g_phase == P_Menu
        && rd32(kScene) == 1u
        && rd32(kSub48) == 2u;
}

void directBootNetworkTick() {
    if (!directBootNetworkActive()) return;
    __try {
        tick();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        QLOG("[boot] deterministic network tick FAULTED");
        g_failed = true;
        g_done = true;
        g_phase = P_Done;
    }
}

void stopDirectBoot() {
    g_active = false;
    g_networkDriven = false;
    g_offlineDispatchTicked = false;
    g_unattendedRematch = false;
    g_forcedLaunchAt = 0;
    g_trainingSelection = false;
    g_trainingAwaitReturnCharSelect = false;
    g_replaySelection = false;
}

}

extern "C" __declspec(dllexport) void qoh99np_boot_tick() {
    if (qgame::g_offlineDispatchTicked) {
        qgame::g_offlineDispatchTicked = false;
    } else if (qgame::g_active && !qgame::g_done
               && !qgame::g_networkDriven) {
        __try { qgame::tick(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            QLOG("[boot] tick FAULTED - stopping direct-boot");
            qgame::g_failed = true;
            qgame::g_done = true;
            qgame::g_phase = qgame::P_Done;
        }
    }
    qoh99::harnessFrontendPressTick();
    if (qoh99::probeActive()) qoh99::probeFrameTick();
}
