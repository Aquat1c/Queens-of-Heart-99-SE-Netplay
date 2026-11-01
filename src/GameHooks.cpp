#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <float.h>
#include "GameHooks.h"
#include "RollbackManager.h"
#include "client/BuildIdentity.h"
#include "qoh99_game.h"
#include "qoh99_build_se.h"
#include "RuntimeProfileRegistry.h"
#include "CrashDump.h"
#include "Log.h"
#include "MinHook.h"
#include "qoh99/SceneSystem.hpp"
#include "qoh99/MatchSystem.hpp"
#include "qoh99/GameplayRegionCatalog.hpp"
#include "game/DPlayTransport.h"
#include "game/DirectBoot.h"
#include "game/PaletteBridge.h"
#include "game/GameErrorJournal.h"
#include "game/DeterministicAudioPolicy.h"
#include "game/DeterministicConfigPolicy.h"
#include "game/FCommandNetplayPolicy.h"
#include "game/NativeHudPolicy.h"
#include "game/FrontendDelayPolicy.h"
#include "game/NativeDeveloperKeyPolicy.h"
#include "game/NetplayLifecyclePolicy.h"
#include "game/NetplayPacingDeadline.h"
#include "game/NetplayPacingTelemetry.h"
#include "game/LifecycleStateTrace.h"
#include "game/RollbackHealthTelemetry.h"
#include "game/NetplayPresentationPolicy.h"
#include "game/NetplayRunState.h"
#include "game/RollbackBalancePolicy.h"
#include "game/NetplaySchedulePolicy.h"
#include "game/RuntimeDelayControl.h"
#include "game/SessionPresentation.h"
#include "game/SpectatorPlaybackPolicy.h"
#include "netplay/RematchPrompt.h"
#include "netplay/RematchTheater.h"
#include "replay/ReplayWinScreen.h"
#include "replay/ReplaySeekCache.h"
#include "net/PacingBudget.h"
#include "netplay/RollbackSession.h"
#include "audio/QohAudioRuntime.h"
#include "input/InputActions.h"
#include "input/BindingEvaluator.h"
#include "input/InputSnapshots.h"
#include "input/InputWorkers.h"
#include "input/ModeControlEvaluator.h"
#include "input/KeyboardScanCodec.h"
#include "input/MenuNavigationBridge.h"
#include "input/SessionEventTimeline.h"
#include "input/NativeBindingTable.h"
#include "input/SdlLifecycle.h"
#include "platform/PeImage.h"
#include "platform/WidePath.h"
#include "platform/CompatEnv.h"
#include "platform/CompatPreamble.h"
#include "qoh99/generated/BuildProfiles.hpp"
#include "qoh99/generated/NativeSymbolCatalog.hpp"
#include "qoh99/Fighter.hpp"
#include "replay/CharacterCatalog.h"
#include "replay/ReplayTypes.h"
#include "replay/StageCatalog.h"
#include "replay/ReplayPlaybackController.h"
#include "game/replay/ReplayRuntime.h"
#include "replay/ReplaySimulationContractV2.h"
#include "game/spectator/SpectatorPublisher.h"
#include "training/TrainingCommandQueue.h"
#include "training/TrainingDummy.h"
#include "training/TrainingInput.h"
#include "training/TrainingMacros.h"
#include "training/TrainingPositions.h"
#include "training/TrainingStatusPublisher.h"
#include "render_dx9_imgui.h"
#include <mmsystem.h>
#ifndef DIRECTSOUND_VERSION
#define DIRECTSOUND_VERSION 0x0800
#endif
#include <dsound.h>
#pragma comment(lib, "winmm.lib")
#include <vector>

#include "ipc/BuildContract.h"
#include <array>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <deque>
#include <new>
#include <algorithm>
#include <limits>
#include <string>

namespace qoh99 {

namespace {

typedef int (__cdecl* FrameDispatchFn)();

inline uint32_t rd32_(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

inline GamePhase currentPhase() {
    return SceneSystem::classify((int)rd32_(kSceneStateAddr), 0, (int)rd32_(kMatchSubAddr));
}
inline bool rollbackEligible() {
    return SceneSystem::policyFor(currentPhase()).rollbackAllowed;
}

// Pool depth = kMaxWindow+1: the pool and all per-frame rings must hold >=31 slots.
constexpr int      kMaxWindow  = 30;
constexpr int      kPoolSlots  = kMaxWindow + 1;
static_assert(static_cast<int>(qgame::kMaximumGameplayRollback) <= kMaxWindow,
              "rollback ceiling exceeds the savestate pool depth");
constexpr uint64_t kGrowAfter  = 180;
static int         g_window    = 1;
static int         g_windowPin = 0;
static bool        g_advSlot[kPoolSlots] = {};
static uint32_t    g_advCount[kPoolSlots] = {};
static uint32_t    g_truthP2Steps[kPoolSlots] = {};
static uint32_t    g_p2_56_truth[kPoolSlots] = {};
// +56 holds the anim frame duration (frame_data[a1+64]); a1+48 is the anim id and a1+52 the frame index.
static uint32_t    g_p2_48_truth[kPoolSlots] = {}, g_p2_52_truth[kPoolSlots] = {}, g_p2_64_truth[kPoolSlots] = {};
static uint32_t    g_p2_16_truth[kPoolSlots] = {}, g_p2_24_truth[kPoolSlots] = {};
static uint32_t    g_winTruth56[kPoolSlots], g_winRep56[kPoolSlots];
static bool        g_winValid[kPoolSlots];
static uint32_t    g_winAct[kPoolSlots], g_win208[kPoolSlots], g_winP2x[kPoolSlots];
static uint32_t    g_winT48[kPoolSlots], g_winR48[kPoolSlots], g_winT52[kPoolSlots], g_winR52[kPoolSlots];
static uint32_t    g_winT64[kPoolSlots], g_winR64[kPoolSlots];
static uint32_t    g_winRfd[kPoolSlots], g_winRdur[kPoolSlots];
static int32_t     g_winRd[kPoolSlots];
static uint32_t    g_gT[8][kPoolSlots] = {}, g_gR[8][kPoolSlots] = {};
static const uintptr_t kGateAddr[8] = { 0x626880 + 1500, 0x626880 + 1456, 0x626880 + 1452,
                                        0x626880 + 1448, 0x626E7C, 0x526F28, 0x626880 + 174, 0x8719A4 };
static const char*     kGateName[8] = { "P2+1500", "P2+1456", "P2+1452", "P2+1448",
                                        "d626E7C", "d526F28", "P2+174", "matchSub" };

static bool  g_desyncDumpEnabled = true;
static char  g_sessionDir[MAX_PATH] = {0};
static std::vector<RollbackManager::RegionHash> g_hashBeforeTruth, g_hashAfterTruth,
                                                g_hashAfterRestore, g_hashAfterReplay;
static bool     g_lastTruthAdvanced = false, g_lastReplayAdvanced = false;
static uint32_t g_lastFrameCtrTruth = 0, g_lastFrameCtrReplay = 0;
static size_t   g_lastSlot = 0;
static uint8_t  g_replayDiffBytes[16] = {0};
static std::vector<uint8_t> g_replayRegionSnap;
static uintptr_t g_replayRegionSnapAddr = 0; static size_t g_replayRegionSnapSize = 0;
static uint32_t g_bkP2Ptr = 0; static uint32_t g_bkP2Fields[15] = {0};
static bool g_wideDesync = false;
static bool g_feederRing = false;
static std::vector<std::vector<uint8_t>> g_wideRing;
static bool     g_wideRestore = false;
static uintptr_t g_wideRestoreLo = 0, g_wideRestoreHi = 0;

RunMode          g_mode      = RunMode::Off;
FrameDispatchFn  g_orig      = nullptr;
RollbackManager  g_rb;
static bool      g_traceResidual = false;
std::vector<uint8_t> g_expected;
uint64_t         g_frame     = 0;
bool             g_rbReady   = false;
uint64_t         g_selftestOk = 0, g_selftestBad = 0;
static bool      g_trainingLaunchRequested = false;
static bool      g_nativeDeveloperKeysAllowedByLaunch = false;

inline uint32_t rd32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
inline uint8_t  rd8(uintptr_t a)  { return *reinterpret_cast<volatile uint8_t*>(a); }
inline uint16_t rd16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }
inline void     wr32(uintptr_t a, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(a) = v; }
inline void     wr8(uintptr_t a, uint8_t v) { *reinterpret_cast<volatile uint8_t*>(a) = v; }
// The motion lanes QOH integrates are 16-bit pairs; writing them as dwords
// would clobber the neighbouring axis.
inline void     wr16(uintptr_t a, uint16_t v) { *reinterpret_cast<volatile uint16_t*>(a) = v; }
static bool rangeReadable(uintptr_t addr, size_t size);

bool trainingOverrideActive(int player);
int trainingOverrideMask(int player);
void trainingObserveOfflinePhysicalSnapshot(
    uint32_t p1Mask, uint32_t p2Mask,
    bool p1PhysicalNeutralBeforeSocd,
    uint32_t heldTrainingActions);

typedef int (__fastcall* InputReadFn)(int player);
static InputReadFn   g_origRead    = nullptr;
static bool          g_readHooked  = false;
static volatile bool g_ovrActive   = false;
static volatile bool g_ovrEnabled  = true;
static volatile uint64_t g_ovrFrame = 0;
static uint64_t g_dispatchN = 0;

// Logical mask (dword_4A2A38): directions 0x10000..0x80000, buttons 0x1..0x8.
inline int aiInput(uint64_t frame, int player) {
    uint32_t block = (uint32_t)(frame / 22) + (player ? 7u : 0u);
    uint32_t h = block * 2654435761u + (uint32_t)player * 40503u + 0x9E3779B9u;
    h ^= h >> 13; h *= 0x85EBCA6Bu; h ^= h >> 16;
    static const int kDirs[8] = { 0, 0x10000, 0x20000, 0x40000, 0x80000, 0x40000, 0x80000, 0 };
    int dir = kDirs[h & 7];
    int btn = ((h >> 6) & 1) ? (1 << (int)((h >> 2) & 3)) : 0;
    return dir | btn;
}

static bool g_inputTest = false;
inline int inputTestMask(uint64_t frame, int player) {
    if (player != 0) return 0;
    static const int kPhaseMask[10] = {
        0x00000, 0x10000, 0x00000, 0x20000, 0x00000,
        0x40000, 0x00000, 0x80000, 0x00001, 0x00002,
    };
    return kPhaseMask[(frame / 90) % 10];
}

static volatile bool g_unattendedHarnessLaunch = false;
static int g_frontendInject[2] = { 0, 0 };
static bool     g_netActive     = false;
static bool     g_replayRoleActive = false;
static uint64_t g_replayManifestDigest = 0;
static uint32_t g_replaySpeedPermille = 1000;
static uint32_t g_replayMatchOrdinal = 0;
static bool     g_netRunning    = false;
static int      g_netLocalCtrl  = 0;
static uint32_t g_netDelay      = 2;
static uint32_t g_netRollback   = 0;
static uint32_t g_netRemoteDelay = 0;
static uint32_t g_netRemoteRollback = 0;
static char g_netPlayerNames[2][qgame::kPlayerNameStorageBytes] = {
    "Player", "Player"
};
static uint32_t g_netPlayerNameCodePages[2] = {0, 0};
static uint32_t g_netFrontendDelay = 1;
static qgame::FrontendDelayLatch g_netFrontendDelayLatch;
static uint32_t g_netDeferredDelayTarget = UINT32_MAX;
static uint64_t g_netDeferredDelayLastLogMs = 0;
static uint32_t g_netGameplayDelayApplied = 2;
static qgame::RuntimeDelayHotkeys g_netDelayHotkeys;
static qgame::RuntimeDelayDrain g_netDelayDrain;
static qgame::NetworkDirectBootInputGate g_netDirectBootInputGate;
static uint32_t g_netDirectBootLastBlockedInput = UINT32_MAX;
static qgame::WinscreenCarryGate g_netWinscreenCarry[2];
static uint32_t g_netWinscreenCarryBlocked = 0;
static uint32_t g_netCharSelectBackBlocked = 0;
static qgame::NetplayPacingWindow g_netPacingWindow;
static qgame::NetplayPacingWindow g_netPresentationPacingWindow;
static qgame::RollbackHealthTelemetry g_netRollbackHealth;
static uint64_t g_netRollbackNextStallLogUs = 100000u;
static qgame::AdapterHoldReason g_netAdapterHoldReason =
    qgame::AdapterHoldReason::None;
static uint64_t g_netRollbackNextAdapterHoldLogUs = 100000u;
static qnetplay::RollbackStats g_netPacingStatsBaseline{};
static uint64_t g_netPacingCorrectionWorkUs = 0;
static uint32_t g_netPacingCorrectionBatches = 0;
static uint32_t g_netPacingDriverCalls = 0;
static uint32_t g_netPacingSessionStalls = 0;
static uint32_t g_netPacingExactWaits = 0;
static uint32_t g_netPacingDeadlineWaits = 0;
static uint32_t g_netPacingSpacingWaits = 0;
static uint32_t g_netPacingDeadlineRebases = 0;
static uint32_t g_netFrame      = 0;
static int      g_netInject[2]  = { 0, 0 };
static volatile bool g_netCanonicalPoll = false;
static std::unique_ptr<qnetplay::RollbackSession> g_netSession;
static uint32_t g_netEpoch = 1;
static qcrash::RecentInputSample
    g_ownedTickCrashInputs[qcrash::kCrashRecentInputCapacity]{};
static qcrash::RecentRollbackDecision
    g_ownedTickCrashRollback[qcrash::kCrashRecentRollbackCapacity]{};
static uint8_t g_ownedTickCrashInputCount = 0u;
static uint8_t g_ownedTickCrashRollbackCount = 0u;
static uint32_t g_ownedTickCrashInputNext = 0u;
static uint32_t g_ownedTickCrashRollbackNext = 0u;
static uint32_t g_netRemoteIngest = 0;
static uint32_t g_netTransitionBlocked = 0xFFFFFFFFu;
static bool g_netHalted = false;
static char g_netHaltReason[160] = {};
constexpr uint32_t kNetPeerRequested = static_cast<uint32_t>(
    qnetplay::wire::TerminalErrorCode::PeerRequested);
constexpr uint32_t kNetProtocolViolation = static_cast<uint32_t>(
    qnetplay::wire::TerminalErrorCode::ProtocolViolation);
constexpr uint32_t kNetTransportFailure = static_cast<uint32_t>(
    qnetplay::wire::TerminalErrorCode::TransportFailure);
constexpr uint32_t kNetTimeout = static_cast<uint32_t>(
    qnetplay::wire::TerminalErrorCode::Timeout);
constexpr uint32_t kNetInternalFailure = static_cast<uint32_t>(
    qnetplay::wire::TerminalErrorCode::InternalFailure);
constexpr uint32_t kNetConfirmedDesync = static_cast<uint32_t>(
    qnetplay::wire::TerminalErrorCode::ConfirmedDesync);
constexpr uint32_t kNetInvalidConfiguration = static_cast<uint32_t>(
    qnetplay::wire::TerminalErrorCode::InvalidConfiguration);
static uint32_t g_netHaltCode = 0;
constexpr size_t kNetMetaSlots = 4096;
struct NetFrameMeta {
    uint32_t frame = 0;
    uint32_t layoutGeneration = 0;
    uint32_t epoch = 0;
    uint32_t phaseBefore = 0;
    uint32_t phaseAfter = 0;
    uint32_t rngGameplay = 0;
    uint32_t rngCosmetic = 0;
    uint32_t rngSelector = 0;
    int32_t hp0 = 0;
    int32_t hp1 = 0;
    uint64_t preHash = 0;
    uint16_t fpuControlWord = 0;
    uint32_t fpuMxcsrRaw = 0;
    uint32_t fpuMxcsrCanonical = 0;
    uint64_t fpuHash = 0;
    uint64_t virtualAudioHash = 0;
    std::array<
        uint64_t,
        qgame::kSimulationStatusSoundIds.size()> virtualAudioStatusUntil{};
    uint64_t spectatorPreHash = 0;
    uint64_t spectatorPostHash = 0;
    qreplay::GameLifecycleV2 replayLifecycleBefore{};
    qreplay::GameLifecycleV2 replayLifecycleAfter{};
    qreplay::RoundTransitionProofV2 replayRoundTransition{};
    std::array<
        qreplay::FighterIdentityV2,
        qreplay::kFighterSlots> replayRoster{};
    std::array<
        qreplay::SelectorProofV2,
        qreplay::kFighterSlots> replaySelectorProofBefore{};
    qreplay::StageIdentityV2 replayStage{};
    int32_t replayDispatcherResult = 0;
    uint8_t replayActiveFighterMask = 0u;
    std::array<uint16_t, 2> canonicalActions{};
    bool valid = false;
};
static NetFrameMeta g_netMeta[kNetMetaSlots];
static std::deque<qgame::NetConfirmedHash> g_netPendingPeerHashes;
static uint32_t g_netConfirmedThrough = 0;
static bool g_netHasConfirmed = false;
static uint64_t g_netConfirmedHashMatches = 0;
static bool g_netGameplaySegmentActive = false;
static bool g_netGameplaySegmentSeen = false;
static bool g_netGameplayRefreshPending = false;
static uint32_t g_netLayoutTestMutationGeneration = 0;
static bool g_netAutomatchTest = false;
static bool g_netAutomatchFrontendOnly = false;
static bool g_netAutomatchHarnessEnabled = false;
static uint32_t g_netTestF1ReturnFrame = 0u;
static bool g_netTestPauseBarrierEnabled = false;
static volatile LONG g_netTestPauseFrame = 0;
static volatile LONG g_netTestPauseHeldFrame = 0;
static volatile LONG g_netTestPauseRequest = 0;
static volatile LONG g_netTestPauseAcknowledged = 0;
static uint32_t g_netTestPauseTailWaits = 0;
constexpr uint32_t kNetTestPauseTailWaitTicks = 2000u;
static uint32_t g_netAutomatchLowHpAfter = 120;
static uint32_t g_netAutomatchForceKoAfter = 360;
static bool g_netAutomatchForceKoEnabled = true;
static bool g_netAutomatchWasGameplay = false;
static uint32_t g_netAutomatchSegment = 0;
static uint32_t g_netAutomatchSegmentStart = 0;
static uint32_t g_netAutomatchCharselectVisits = 0;
static bool g_netAutomatchInCharselect = false;
static uint32_t g_netAutomatchCharselectBaseFrame = 0;
static uint32_t g_netAutomatchRoundSeenMask = 0;
static uint32_t g_netAutomatchLowLifeLoggedMask = 0;
static uint32_t g_netAutomatchNativeKoLoggedMask = 0;
static uint32_t g_netAutomatchNativeLatchLoggedMask = 0;
static uint32_t g_netAutomatchForcedKoLoggedMask = 0;
static uint64_t g_netAutomatchInputLogs = 0;
static uint64_t g_netAutomatchCombatInputLogs = 0;
static bool g_netAutomatchFrontendNonzeroLogged = false;
static bool g_netAutomatchCombatNonzeroLogged = false;
static int32_t g_netAutomatchP1Character = -1;
static int32_t g_netAutomatchP2Character = -1;
static int32_t g_netAutomatchP1Alternate = 0;
static int32_t g_netAutomatchP2Alternate = 0;
static bool g_netAutomatchForcedCharacterSceneActive = false;
struct NetTestDelayStep {
    uint32_t gameplayTick = 0;
    uint8_t delay = 0;
};
static std::array<NetTestDelayStep, 8> g_netTestDelaySteps{};
static size_t g_netTestDelayStepCount = 0;
static size_t g_netTestDelayNextStep = 0;
static uint32_t g_netTestDelayGameplayTicks = 0;
static uint32_t g_netTestDelayLastFrame = UINT32_MAX;
static uint32_t g_netTestDelayPendingTarget = UINT32_MAX;
static bool g_netTestDelayScriptEnabled = false;
constexpr uintptr_t kBattleLifeCurrentOffset = 0u;
constexpr uintptr_t kBattleLifeDamageOffset = 4u;
constexpr uintptr_t kBattleLifeReserveOffset = 8u;
constexpr uintptr_t kBattleLifeTimerOffset = 12u;
constexpr uintptr_t kRoundResultStateAddr = 0x88098Cu;
static uintptr_t g_nativeDamageFn = 0;
static uintptr_t g_nativeReactionGateFn = 0;
static uintptr_t g_nativeHudScoreDrawFn = 0;
static uintptr_t g_nativeHudTimeDrawFn = 0;
static uintptr_t g_readLogicalInputFn = 0;
static uintptr_t g_rngDrawFn = 0;
static uintptr_t g_frameDispatchFn = 0;
constexpr uintptr_t kNativeFatalHitModeAddr = 0x6236C4u;
constexpr uintptr_t kNativeFatalHitDisableAddr = 0x6275A0u;
constexpr uintptr_t kFighterPendingActionOffset = 92u;
constexpr uintptr_t kFighterReactionVariantOffset = 157u;
constexpr uintptr_t kFighterPlayerSlotOffset = 172u;
constexpr uintptr_t kFighterRoundEndOffset = 1364u;
constexpr uintptr_t kFighterRoundLostOffset = 1368u;
constexpr uintptr_t kFighterHitTargetOffset = 1436u;
constexpr uint32_t kNativeFatalAction = 58u;
constexpr uint32_t kNativeGenericKoCause = 129u;

// 0x4AA3D0 is the primary outro-stage countdown (stage 8 loads its 60-tick round-reset countdown here); 0x4AA408 is only the secondary zoom timer.
constexpr uintptr_t kFinishStageTimerAddr = 0x4AA3D0u;

inline bool netLifecycleTransitionRequiresExactInput() {
    return qgame::lifecycleTransitionRequiresExactInput(
        rd32(kRoundResultStateAddr), rd32(addr::kRoundCounter),
        rd32(kFinishStageTimerAddr));
}

inline bool netRollbackGameplayPolicyActive() {
    return qgame::rollbackGameplayPolicyActive(
        rollbackEligible(),
        rd32(kRoundResultStateAddr),
        rd32(addr::kRoundCounter),
        rd32(kFinishStageTimerAddr));
}

using NativeDamageFn = int (__fastcall*)(int attackerSlot, int victimActor,
                                         int cause, int damage);
using NativeReactionGateFn = int (__thiscall*)(void* reaction,
                                               int victimActor);

bool netAutomatchNativeKoCodeMatches() {
    static const uint8_t kDamageSignature[] = {
        0x56, 0x8B, 0xF2, 0x33, 0xC0, 0x57, 0x8A,
        0x86, 0xAC, 0x00, 0x00, 0x00, 0x8B, 0xF9,
    };
    static const uint8_t kReactionSignature[] = {
        0x8B, 0x44, 0x24, 0x04, 0x33, 0xD2, 0x56,
        0x8A, 0x90, 0x9D, 0x00, 0x00, 0x00, 0x8B,
        0x40, 0x5C,
    };
    return g_nativeDamageFn != 0 && g_nativeReactionGateFn != 0
        && se::verifyCodeBytes(g_nativeDamageFn, kDamageSignature,
                               sizeof(kDamageSignature))
        && se::verifyCodeBytes(g_nativeReactionGateFn, kReactionSignature,
                               sizeof(kReactionSignature));
}

inline uintptr_t netBattleLifeAddress(uint32_t player) {
    return kBattleLifeBase
        + static_cast<uintptr_t>(player) * addr::kBattleLifeStride;
}

inline uintptr_t netActiveFighterAddress(uint32_t player) {
    if (player >= 4u) return 0;
    const uintptr_t active = rd32(
        addr::kFighterPtrTable + 4u * player);
    if (active >= 0x400000u && active < 0x900000u)
        return active;
    static constexpr uintptr_t fallback[4] = {
        kFighterP1Addr, kFighterP2Addr,
        addr::kFighterP3, addr::kFighterP4,
    };
    return fallback[player];
}

inline int32_t netBattleLifeEffective(uint32_t player) {
    const uintptr_t base = netBattleLifeAddress(player);
    return static_cast<int32_t>(
               rd32(base + kBattleLifeCurrentOffset))
        + static_cast<int32_t>(
               rd32(base + kBattleLifeReserveOffset))
        - static_cast<int32_t>(
               rd32(base + kBattleLifeDamageOffset));
}

inline void netSetBattleLifeEffective(uint32_t player, int32_t effective) {
    const uintptr_t base = netBattleLifeAddress(player);
    const int32_t current = static_cast<int32_t>(
        rd32(base + kBattleLifeCurrentOffset));
    const int32_t reserve = static_cast<int32_t>(
        rd32(base + kBattleLifeReserveOffset));
    wr32(base + kBattleLifeDamageOffset,
         static_cast<uint32_t>(current + reserve - effective));
}

inline void netSetBattleLifeTimer(uint32_t player, int32_t timer) {
    const uintptr_t base = netBattleLifeAddress(player);
    wr32(base + kBattleLifeTimerOffset,
         static_cast<uint32_t>(timer));
}
struct NetLayoutBarrierState {
    uint32_t layoutGeneration = 0;
    uint32_t epoch = 0;
    uint64_t digest = 0;
    uint64_t startedMs = 0;
    bool submitted = false;
    bool remoteMatched = false;
};
static NetLayoutBarrierState g_netLayoutBarrier{};
static bool g_netReadySnapped = false;
static qgame::NetReadyState g_netLocalReady{};
static bool g_netSpectatorMode = false;
static bool g_netServeSpectators = false;
static bool g_netSpectatorArchiveBegun = false;
static bool g_netSpectatorArchiveFailed = false;
static bool g_netSpectatorArchiveSealed = false;
static uint64_t g_netSpectatorGameId = 1;
static uint32_t g_netSpectatorRecord = 0;
static uint64_t g_netSpectatorPostTailHash = 0;
static uint8_t g_spectatorServiceMode = 0;
static uint32_t g_spectatorSidecarRecord = 0;
static bool g_spectatorSidecarDropLogged = false;
static uint32_t g_netLifecycleLoggedScene = 0xFFFFFFFFu;
static uint32_t g_netLifecycleLoggedNext = 0xFFFFFFFFu;
static int32_t g_netLifecycleLoggedMatchSub = INT32_MIN;
static int32_t g_netLifecycleLoggedScene3Sub = INT32_MIN;
static bool g_spectatorStartReceived = false;
static bool g_spectatorPlaybackStarted = false;
enum class SpectatorSkipState : uint8_t {
    Off = 0,
    Waiting,
    Booting,
    Restoring,
};
static bool g_spectatorWantsLatest = false;
static SpectatorSkipState g_spectatorSkip = SpectatorSkipState::Off;
static qnetplay::spectator::Checkpoint g_spectatorSkipCheckpoint{};
static qgame::ReplayDirectBootConfig g_spectatorSkipBoot{};
static qnetplay::spectator::SpectatorSetTimeline::BindSeed
    g_spectatorSkipSeed{};
static qgame::NetReadyState g_spectatorSkipReady{};
static uint64_t g_spectatorSkipArmedMs = 0;
static uint32_t g_spectatorSkipHoldTicks = 0;
static bool g_spectatorBufferPrimed = false;
static bool g_spectatorHasPrimed = false;
static bool g_spectatorPacingSummaryLogged = false;
static qnetplay::spectator::GameStartPayload g_spectatorGameStart{};
static uint32_t g_spectatorTimelineFrame = 0;
static uint64_t g_spectatorLastResumeMs = 0;
static uint64_t g_spectatorArmMs = 0;
static uint64_t g_spectatorStartReceivedMs = 0;
static uint64_t g_spectatorGameplayEntryStartedMs = 0;
static uint64_t g_spectatorGameplayEntryLastLogMs = 0;
static uint64_t g_spectatorLinkLostSinceMs = 0;
static bool g_spectatorCatchingUp = false;
static uint64_t g_spectatorCatchupStartedMs = 0;
static uint64_t g_spectatorUnderruns = 0;
static uint64_t g_spectatorCatchupSlicesTruncated = 0;
static uint64_t g_spectatorLastCatchupLogMs = 0;
static uint32_t g_spectatorElasticCredit = 0;
static uint64_t g_spectatorElasticHolds = 0;
static uint64_t g_spectatorLastElasticLogMs = 0;
static uint32_t g_spectatorCatchupCredit = 0;
static size_t g_spectatorCushionBonus = 0;
static uint32_t g_spectatorCatchupSpeedPermille = 1000u;

constexpr uint32_t kSpectatorCommandQueueDepth = 32u;
struct SpectatorCommandSlot {
    qreplay::PlaybackCommand command = qreplay::PlaybackCommand::None;
    uint64_t argument = 0u;
};
static SpectatorCommandSlot
    g_spectatorCommands[kSpectatorCommandQueueDepth] = {};
static volatile LONG g_spectatorCommandHead = 0;
static volatile LONG g_spectatorCommandTail = 0;
static volatile LONG g_spectatorCommandDrops = 0;
static bool g_spectatorLocalPaused = false;
static bool g_spectatorFastForwardHeld = false;
static bool g_spectatorFastForwardLatched = false;
static uint32_t g_spectatorPendingSteps = 0;
static uint32_t g_spectatorSpeedPermille = qreplay::kSpeedNormal;
static uint32_t g_spectatorSeekMatchRequest = 0;
static bool g_spectatorSeekLatestRequested = false;

constexpr uint64_t kSpectatorHeartbeatFreshMs = 3000u;

static const char* spectatorRejectReasonText(
    qnetplay::spectator::RejectReason reason) {
    using qnetplay::spectator::RejectReason;
    switch (reason) {
    case RejectReason::GameUnavailable:
        return "the host has no match to serve right now";
    case RejectReason::ResumeMismatch:
        return "the host no longer holds the cursor this viewer asked to "
               "resume from";
    case RejectReason::ContractMismatch:
        return "the host runs a different build or different game content";
    case RejectReason::CapacityReached:
        return "the host has reached its spectator limit";
    case RejectReason::InvalidRequest:
        return "the host considered the request malformed";
    case RejectReason::ArchiveUnavailable:
        return "the host is no longer keeping that recording";
    case RejectReason::GrantInvalid:
        return "the host no longer recognises this viewer's pass (it was "
               "spent or replaced; try watching again)";
    case RejectReason::GrantExpired:
        return "this viewer's pass expired before the game connected";
    case RejectReason::MigrationRejected:
        return "the host refused the route change";
    case RejectReason::RateLimited:
        return "the host is rate-limiting this address";
    }
    return "unknown reason";
}

static bool g_spectatorHotkeySinkRegistered = false;
static uint64_t g_spectatorHotkeySinkRetryMs = 0;
static char g_spectatorSkipKeyLabel[24] = "";
typedef int(__stdcall* SpectatorHotkeySinkFn)(unsigned long command,
                                              int repeat, void* userData);
typedef void(__stdcall* RegisterSpectatorHotkeySinkFn)(
    SpectatorHotkeySinkFn sink, void* userData);
typedef unsigned long(__stdcall* QueryWrapperHotkeyFn)(
    const char* name, unsigned char* outDiks, unsigned long cap);
constexpr unsigned long kWrapperHotkeyTogglePause = 3ul;
constexpr unsigned long kWrapperHotkeyStep = 4ul;
constexpr unsigned long kWrapperHotkeySlower = 13ul;
constexpr unsigned long kWrapperHotkeyFaster = 14ul;

static int __stdcall spectatorHotkeySinkThunk(unsigned long command,
                                              int repeat, void*) {
    if (!g_netSpectatorMode) return 0;
    const auto submit = [](qreplay::PlaybackCommand playback,
                           unsigned long long argument) {
        spectatorSubmitPlaybackCommand(
            static_cast<unsigned char>(playback), argument);
    };
    switch (command) {
    case kWrapperHotkeyTogglePause:
        submit(g_spectatorLocalPaused ? qreplay::PlaybackCommand::Resume
                                      : qreplay::PlaybackCommand::Pause,
               0ull);
        return 1;
    case kWrapperHotkeyStep:
        (void)repeat;
        submit(qreplay::PlaybackCommand::Step, 0ull);
        return 1;
    case kWrapperHotkeySlower:
        submit(qreplay::PlaybackCommand::SpeedSet, qreplay::kSpeedNormal);
        return 1;
    case kWrapperHotkeyFaster:
        submit(qreplay::PlaybackCommand::Exit, 0ull);
        return 1;
    default:
        return 0;
    }
}

static void spectatorTryRegisterHotkeySink(uint64_t nowMs) {
    if (g_spectatorHotkeySinkRegistered) return;
    if (g_spectatorHotkeySinkRetryMs != 0
        && nowMs - g_spectatorHotkeySinkRetryMs < 500u) {
        return;
    }
    g_spectatorHotkeySinkRetryMs = nowMs;
    HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
    if (!wrapper) return;
    const auto registerSink = reinterpret_cast<RegisterSpectatorHotkeySinkFn>(
        reinterpret_cast<void*>(
            GetProcAddress(wrapper, "EFZDX9_RegisterSpectatorHotkeySink")));
    g_spectatorHotkeySinkRegistered = true;
    if (!registerSink) {
        QLOG("[spectator] wrapper predates the spectator hotkey sink; the "
             "replay keys stay inert while spectating");
        return;
    }
    registerSink(&spectatorHotkeySinkThunk, nullptr);
    const auto query = reinterpret_cast<QueryWrapperHotkeyFn>(
        reinterpret_cast<void*>(
            GetProcAddress(wrapper, "EFZDDRAW_QueryWrapperHotkey")));
    unsigned char diks[4] = {};
    const unsigned long bound =
        query ? query("ReplayFaster", diks, 4ul) : 0ul;
    g_spectatorSkipKeyLabel[0] = '\0';
    if (bound != 0ul) {
        const char* label = qinput::dikDisplayLabel(diks[0]);
        if (label != nullptr && label[0] != '\0') {
            std::snprintf(g_spectatorSkipKeyLabel,
                          sizeof(g_spectatorSkipKeyLabel), "%s", label);
        }
    }
    QLOG("[spectator] wrapper hotkeys wired: pause/step/slower/faster reach "
         "the viewer; skip-to-live key=%s (%lu binding%s)",
         g_spectatorSkipKeyLabel[0] != '\0' ? g_spectatorSkipKeyLabel
                                            : "unbound",
         bound, bound == 1ul ? "" : "s");
}

static uint64_t g_spectatorTelemetryLastMs = 0;
static uint64_t g_spectatorJoinTelemetryLastMs = 0;
static uint64_t g_spectatorLayoutHoldSinceMs = 0;
static uint32_t g_spectatorLayoutHoldStreak = 0;
static uint64_t g_spectatorTelemetrySampleMs = 0;
static uint64_t g_spectatorTelemetryWindowStartMs = 0;
static uint32_t g_spectatorTelemetryWindowStartRecord = 0;
constexpr size_t kSpectatorBacklogRingSize = 20u;
static uint32_t g_spectatorBacklogRing[kSpectatorBacklogRingSize] = {};
static uint32_t g_spectatorBacklogRingCount = 0;
static bool g_spectatorSourceStale = false;
constexpr uint64_t kSpectatorTelemetryIntervalMs = 5000u;
constexpr uint64_t kSpectatorSourceStaleMs = 2000u;

static void spectatorTelemetryReset() {
    g_spectatorTelemetryLastMs = 0;
    g_spectatorJoinTelemetryLastMs = 0;
    g_spectatorLayoutHoldSinceMs = 0;
    g_spectatorLayoutHoldStreak = 0;
    g_spectatorTelemetrySampleMs = 0;
    g_spectatorTelemetryWindowStartMs = 0;
    g_spectatorTelemetryWindowStartRecord = 0;
    g_spectatorBacklogRingCount = 0;
    g_spectatorSourceStale = false;
    g_spectatorCatchupCredit = 0;
    g_spectatorCushionBonus = 0;
}

static void spectatorTelemetryTick(const qgame::SpectatorViewerStatus& status,
                                   uint64_t nowMs) {
    if (nowMs - g_spectatorTelemetrySampleMs >= 250u) {
        g_spectatorTelemetrySampleMs = nowMs;
        g_spectatorBacklogRing[g_spectatorBacklogRingCount
                               % kSpectatorBacklogRingSize] =
            static_cast<uint32_t>(status.bufferedRecords);
        ++g_spectatorBacklogRingCount;
    }
    const bool stale = status.sawSourceContact
        && status.sourceContactAgeMs >= kSpectatorSourceStaleMs;
    if (stale != g_spectatorSourceStale) {
        g_spectatorSourceStale = stale;
        if (stale) {
            QLOG("[spectator] source silent for %llums (backlog=%zu "
                 "received=%u sourceFrontier=%s%u)",
                 static_cast<unsigned long long>(status.sourceContactAgeMs),
                 status.bufferedRecords, status.receivedNextRecord,
                 status.sourceFrontierKnown ? "" : "unknown/",
                 status.sourceNextRecord);
        } else {
            QLOG("[spectator] source contact resumed (backlog=%zu "
                 "received=%u)",
                 status.bufferedRecords, status.receivedNextRecord);
        }
    }
    if (g_spectatorTelemetryWindowStartMs == 0) {
        g_spectatorTelemetryWindowStartMs = nowMs;
        g_spectatorTelemetryWindowStartRecord = status.receivedNextRecord;
        g_spectatorTelemetryLastMs = nowMs;
        return;
    }
    if (nowMs - g_spectatorTelemetryLastMs < kSpectatorTelemetryIntervalMs)
        return;
    const uint64_t spanMs = nowMs - g_spectatorTelemetryWindowStartMs;
    const uint32_t receivedInWindow =
        status.receivedNextRecord - g_spectatorTelemetryWindowStartRecord;
    const uint64_t rateDeci = spanMs != 0
        ? (static_cast<uint64_t>(receivedInWindow) * 10000ull) / spanMs
        : 0ull;
    const unsigned long long behindLive = status.sourceFrontierKnown
        && status.sourceNextRecord >= status.playbackNextRecord
        ? status.sourceNextRecord - status.playbackNextRecord
        : static_cast<unsigned long long>(status.bufferedRecords);
    const unsigned long long inFlight = status.sourceFrontierKnown
        && status.sourceNextRecord >= status.receivedNextRecord
        ? status.sourceNextRecord - status.receivedNextRecord
        : 0ull;
    QLOG("[spectator] link backlog=%zu behindLive=%llu (%llu.%llus%s) "
         "inFlight=%llu recv=%llu.%llu rec/s reordered=%zu contactAge=%llums "
         "primed=%d catchingUp=%d speed=%upm cushion=+%zu holds=%llu "
         "underruns=%llu match=%u/%u",
         status.bufferedRecords, behindLive,
         behindLive * 10ull / 606ull, (behindLive * 100ull / 606ull) % 10ull,
         status.sourceFrontierKnown ? "" : ", frontier unknown",
         inFlight, rateDeci / 10ull, rateDeci % 10ull,
         status.reorderedRecords,
         static_cast<unsigned long long>(status.sourceContactAgeMs),
         g_spectatorBufferPrimed ? 1 : 0, g_spectatorCatchingUp ? 1 : 0,
         g_spectatorSpeedPermille, g_spectatorCushionBonus,
         static_cast<unsigned long long>(g_spectatorElasticHolds),
         static_cast<unsigned long long>(g_spectatorUnderruns),
         status.currentMatchId, status.sourceMatchId);
    g_spectatorTelemetryLastMs = nowMs;
    g_spectatorTelemetryWindowStartMs = nowMs;
    g_spectatorTelemetryWindowStartRecord = status.receivedNextRecord;
}

static void spectatorJoinTelemetryTick(
    const qgame::SpectatorViewerStatus& status, uint64_t nowMs) {
    if (g_spectatorJoinTelemetryLastMs != 0
        && nowMs - g_spectatorJoinTelemetryLastMs
               < kSpectatorTelemetryIntervalMs) {
        return;
    }
    g_spectatorJoinTelemetryLastMs = nowMs;
    QLOG("[spectator] joining: received=%u sourceFrontier=%s%u static=%llums "
         "sourcePhase=%u contactAge=%llums reordered=%zu awaitingEvent=%u "
         "setSeek=%u",
         status.receivedNextRecord,
         status.sourceFrontierKnown ? "" : "unknown/", status.sourceNextRecord,
         static_cast<unsigned long long>(status.sourceFrontierStaticMs),
         static_cast<unsigned>(status.sourcePhase),
         static_cast<unsigned long long>(status.sourceContactAgeMs),
         status.reorderedRecords, status.awaitingEventIndex,
         static_cast<unsigned>(g_spectatorSkip));
}

static void spectatorLogFreezeDiagnostics(
    const qgame::SpectatorViewerStatus& status, uint64_t nowMs) {
    char samples[kSpectatorBacklogRingSize * 8 + 1];
    size_t used = 0;
    const uint32_t count = g_spectatorBacklogRingCount < kSpectatorBacklogRingSize
        ? g_spectatorBacklogRingCount
        : static_cast<uint32_t>(kSpectatorBacklogRingSize);
    const uint32_t first = g_spectatorBacklogRingCount - count;
    samples[0] = '\0';
    for (uint32_t i = 0; i < count; ++i) {
        const int n = std::snprintf(
            samples + used, sizeof(samples) - used, "%s%u", i ? " " : "",
            g_spectatorBacklogRing[(first + i) % kSpectatorBacklogRingSize]);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(samples) - used) break;
        used += static_cast<size_t>(n);
    }
    const unsigned long long inFlight = status.sourceFrontierKnown
        && status.sourceNextRecord >= status.receivedNextRecord
        ? status.sourceNextRecord - status.receivedNextRecord
        : 0ull;
    char eventVerdict[192];
    if (status.awaitingEventIndex != 0) {
        std::snprintf(eventVerdict, sizeof(eventVerdict),
                      "playback is starved of a SET EVENT: it needs event #%u "
                      "and has applied %u, while %zu record(s) sit ready in "
                      "the reorder buffer - repairing records cannot clear "
                      "this",
                      status.awaitingEventIndex, status.appliedEvents,
                      status.reorderedRecords);
    } else {
        eventVerdict[0] = 0;
    }
    const char* verdict = status.awaitingEventIndex != 0
        ? eventVerdict
        : !status.sourceFrontierKnown
            ? "the source's frontier is unknown (no Heartbeat seen)"
            : inFlight == 0ull
                ? "the SOURCE has nothing newer: the match itself stalled, was "
                  "paused, or its publication stopped"
                : "the source is ahead but its records have not arrived: the "
                  "route is late or losing packets";
    QLOG("[spectator] freeze: buffer empty at received=%u; backlog over the "
         "last 5s (oldest first): %s; source frontier=%s%u inFlight=%llu "
         "contactAge=%llums reordered=%zu awaitingEvent=%u appliedEvents=%u "
         "-> %s",
         status.receivedNextRecord, samples,
         status.sourceFrontierKnown ? "" : "unknown/", status.sourceNextRecord,
         inFlight, static_cast<unsigned long long>(status.sourceContactAgeMs),
         status.reorderedRecords, status.awaitingEventIndex,
         status.appliedEvents, verdict);
    (void)nowMs;
}

void spectatorApplyPendingLocalCommands() {
    for (int guard = 0; guard < static_cast<int>(kSpectatorCommandQueueDepth);
         ++guard) {
        const LONG tail = InterlockedCompareExchange(
            &g_spectatorCommandTail, 0, 0);
        const LONG head = InterlockedCompareExchange(
            &g_spectatorCommandHead, 0, 0);
        if (tail == head) break;
        const SpectatorCommandSlot slot =
            g_spectatorCommands[static_cast<uint32_t>(tail)
                                % kSpectatorCommandQueueDepth];
        InterlockedExchange(&g_spectatorCommandTail, tail + 1);
        switch (slot.command) {
            case qreplay::PlaybackCommand::Pause:
                g_spectatorLocalPaused = true;
                g_spectatorFastForwardHeld = false;
                g_spectatorFastForwardLatched = false;
                break;
            case qreplay::PlaybackCommand::Resume:
                g_spectatorLocalPaused = false;
                break;
            case qreplay::PlaybackCommand::Step:
                if (g_spectatorPendingSteps < 8u) ++g_spectatorPendingSteps;
                break;
            case qreplay::PlaybackCommand::SpeedSet: {
                const uint32_t permille =
                    static_cast<uint32_t>(slot.argument);
                if (!qreplay::isSupportedSpeedPermille(permille)) {
                    QLOG("[spectator] rejecting unsupported local speed %u "
                         "permille (never silently clamped)", permille);
                    break;
                }
                g_spectatorSpeedPermille = permille;
                g_spectatorFastForwardHeld = permille > qreplay::kSpeedNormal;
                if (permille <= qreplay::kSpeedNormal)
                    g_spectatorFastForwardLatched = false;
                if (permille > qreplay::kSpeedNormal)
                    g_spectatorLocalPaused = false;
                break;
            }
            case qreplay::PlaybackCommand::SeekMatch:
                g_spectatorSeekMatchRequest =
                    static_cast<uint32_t>(slot.argument);
                break;
            case qreplay::PlaybackCommand::NextMatch:
            case qreplay::PlaybackCommand::SeekRound:
            case qreplay::PlaybackCommand::PreviousMatch:
            case qreplay::PlaybackCommand::Restart:
                QLOG("[spectator] local seek command %u not available yet "
                     "(per-match index/checkpoint cursor not published)",
                     static_cast<unsigned>(slot.command));
                break;
            case qreplay::PlaybackCommand::Exit:
                g_spectatorSeekLatestRequested = true;
                break;
            case qreplay::PlaybackCommand::None:
            default:
                break;
        }
    }
    if (g_spectatorSeekLatestRequested) {
        g_spectatorSeekLatestRequested = false;
        g_spectatorFastForwardLatched = true;
        g_spectatorLocalPaused = false;
    }
}
inline uint32_t netPhaseTag() {
    return ((rd32(kSceneStateAddr) & 0xFFFFu) << 16)
         | (rd32(kMatchSubAddr) & 0xFFFFu);
}
constexpr int   kNetHoldFrameRet = 1;

static int64_t  g_qpcFreq       = 0;
static int64_t  g_paceTicks     = 0;
static qgame::NetplayPacingDeadline g_paceDeadline;
static qgame::PacingDeadlinePoll g_lastPacePoll{};
static qgame::CadenceDebt g_netCadenceDebt;
static qgame::HoldCause g_netLastHoldCause = qgame::HoldCause::None;

static qgame::NetplayRunState g_netRunState = qgame::NetplayRunState::Running;
static qgame::HoldEpisodeLedger g_netHoldLedger;
static uint64_t g_netRunStateEnteredUs = 0;
static uint64_t g_netRunStateSampledUs = 0;
static uint32_t g_netRunStateTransitions = 0;
static uint32_t g_netRunStateDiscontinuities = 0;
static uint64_t g_netRunStateUs[
    static_cast<size_t>(qgame::NetplayRunState::Terminated) + 1u] = {};
static uint32_t g_netPacingFrameAdvantageHolds = 0;
static uint32_t g_netPacingLifecycleHolds = 0;

inline qgame::AdapterHoldReason netAdapterHoldReasonForHoldCause(
    const qgame::HoldCause cause) {
    switch (cause) {
        case qgame::HoldCause::LifecycleBoundary:
        case qgame::HoldCause::PredictionLimit:
            return qgame::AdapterHoldReason::ExactInput;
        case qgame::HoldCause::LocalInputMissing:
        case qgame::HoldCause::NotCadence:
        case qgame::HoldCause::None:
            break;
    }
    return qgame::AdapterHoldReason::None;
}
static uint64_t g_netCatchupBatches = 0;
static uint64_t g_hiddenForwardNativeTicks = 0;
constexpr uint64_t kNetPeerFrontierFreshMs = 64;
constexpr qgame::SchedulePolicyConfig kNetSchedulePolicy{3, 3};

struct NetRoundIntroPresentationAddresses {
    uintptr_t phase = 0;
    uintptr_t timer = 0;
    bool valid = false;
};

const NetRoundIntroPresentationAddresses&
netRoundIntroPresentationAddresses() {
    static const NetRoundIntroPresentationAddresses addresses = [] {
        NetRoundIntroPresentationAddresses result{};
        const qoh99::RegionInfo* phase =
            qoh99::GameplayRegionCatalog::find("round_intro_state");
        const qoh99::RegionInfo* control =
            qoh99::GameplayRegionCatalog::find("round_intro_control");
        if (!phase || phase->size < sizeof(uint32_t)
            || !control || control->size < 3u * sizeof(uint32_t))
            return result;
        result.phase = static_cast<uintptr_t>(phase->address);
        result.timer =
            static_cast<uintptr_t>(control->address)
            + 2u * sizeof(uint32_t);
        result.valid = true;
        return result;
    }();
    return addresses;
}

bool netReadRoundIntroPresentationState(
    uint32_t& phase, uint32_t& timer) {
    const NetRoundIntroPresentationAddresses& addresses =
        netRoundIntroPresentationAddresses();
    if (!addresses.valid) return false;
    phase = rd32(addresses.phase);
    timer = rd32(addresses.timer);
    return true;
}

constexpr uint64_t kNetCatchupBatchBudgetUs = 6000;
static uint32_t g_netCatchupBudgetStops = 0;
static uint32_t g_netStarvedOpportunities = 0;
static uint32_t g_netPresentedThisWindow = 0;
constexpr uint64_t kNetStallWarnUs  =  8000000ull;
constexpr uint64_t kNetStallAbortUs = 20000000ull;
static uint32_t g_netStallDeadlineFrame = 0;
static uint64_t g_netStallDeadlineSinceUs = 0;
static bool     g_netStallDeadlineWarned = false;

class NetWorkSampler {
public:
    static constexpr size_t kCapacity = 256;

    void add(const uint64_t micros) {
        samples_[next_ % kCapacity] = static_cast<uint32_t>(
            micros > 0xFFFFFFFFull ? 0xFFFFFFFFull : micros);
        ++next_;
    }
    size_t count() const {
        return next_ < kCapacity ? static_cast<size_t>(next_) : kCapacity;
    }
    uint32_t percentile(const unsigned p) const {
        const size_t n = count();
        if (n == 0u) return 0u;
        uint32_t scratch[kCapacity];
        for (size_t i = 0; i < n; ++i) scratch[i] = samples_[i];
        std::sort(scratch, scratch + n);
        size_t index = (static_cast<size_t>(p) * (n - 1u) + 50u) / 100u;
        if (index >= n) index = n - 1u;
        return scratch[index];
    }
    void reset() { next_ = 0; }

private:
    uint32_t samples_[kCapacity] = {};
    uint64_t next_ = 0;
};

inline uint32_t netRouteRequiredCoverageFrames();

static NetWorkSampler g_netPredictionDepth;

static NetWorkSampler g_netSaveWorkUs;
static NetWorkSampler g_netLoadWorkUs;
static NetWorkSampler g_netHiddenTickUs;

inline constexpr size_t kNetPredictionDepthSmoothingFloor = 64;

inline uint16_t netSmoothedPredictionDepth(const uint32_t instantaneous) {
    const uint32_t chosen =
        g_netPredictionDepth.count() >= kNetPredictionDepthSmoothingFloor
            ? g_netPredictionDepth.percentile(50)
            : instantaneous;
    return static_cast<uint16_t>(
        chosen > 0xFFFFu ? 0xFFFFu : chosen);
}

static uint32_t g_netProducerStarveSpan = 0;
static uint32_t g_netProducerStarveMaxSpan = 0;
static uint64_t g_netProducerStarveOpportunities = 0;
static uint64_t g_netProducerOpportunities = 0;
static uint32_t g_netLastProducedThrough = 0xFFFFFFFFu;

static uint32_t g_netProducerAheadFrames = 0;
static uint32_t g_netProducerAheadMaxLead = 0;
static uint32_t g_netProducerFreshSamples = 0;
static uint32_t g_netProducerHeldRepeats = 0;

inline uint32_t netProducerMaxLeadFrames() {
    const uint32_t peerCapacity = g_netRemoteRollback + g_netRemoteDelay;
    constexpr uint32_t kReserve = 2u;
    const uint32_t budget = peerCapacity + kReserve;
    constexpr uint32_t kHardCap = 32u;
    return budget > kHardCap ? kHardCap : budget;
}

inline bool netInputProducerDisabled() {
    static int cached = -1;
    if (cached < 0) {
        char value[16] = {};
        const DWORD length = GetEnvironmentVariableA(
            "QOH99_NP_DISABLE_INPUT_PRODUCER", value, sizeof(value));
        cached = (length != 0 && length < sizeof(value)
                  && std::strcmp(value, "1") == 0) ? 1 : 0;
    }
    return cached != 0;
}

inline void netObserveInputProduction(const uint32_t producedThrough) {
    ++g_netProducerOpportunities;
    if (g_netLastProducedThrough != 0xFFFFFFFFu
        && producedThrough == g_netLastProducedThrough) {
        ++g_netProducerStarveSpan;
        ++g_netProducerStarveOpportunities;
        if (g_netProducerStarveSpan > g_netProducerStarveMaxSpan)
            g_netProducerStarveMaxSpan = g_netProducerStarveSpan;
    } else {
        g_netProducerStarveSpan = 0;
    }
    g_netLastProducedThrough = producedThrough;
}
constexpr uint32_t kNetFaHoldCooldownOpportunities = 60;
static uint32_t g_netFaHoldCooldown = 0;

inline int32_t netPaceSlewDeadbandFrames() {
    const int32_t budget = static_cast<int32_t>(g_netDelay)
                         + static_cast<int32_t>(g_netRemoteRollback);
    const uint32_t rttMs =
        qgame::netPeerHasRtt() ? qgame::netPeerRttMs() : 0u;
    const int32_t transit = static_cast<int32_t>(
        ((rttMs / 2u) * 6060ull + 50000ull) / 100000ull);
    const int32_t headroom = budget - transit;
    const int32_t derived = headroom / 2;
    if (derived < 1) return 1;
    if (derived > 3) return 3;
    return derived;
}
constexpr uint32_t kNetPaceSlewIntervalOpportunities = 30;
static int32_t  g_netPaceSlewIntegralPpm = 0;
static int32_t  g_netPaceSlewPpm = 0;
static uint32_t g_netPaceSlewCountdown = 0;
static uint32_t g_netPaceSlewAdjustments = 0;
static int32_t  g_netPaceSlewPeakPpm = 0;
static uint32_t g_netRollbackBalanceGameplayFrames = 0;
static uint32_t g_netRollbackBalanceLastFrame = 0;
static bool g_netRollbackBalanceHasLastFrame = false;
static qgame::RollbackBalanceReason g_netRollbackBalanceLastReason =
    qgame::RollbackBalanceReason::OutsideRollbackGameplay;
static bool g_netRollbackBalanceHasLastReason = false;

inline bool netRollbackBalanceDisabled() {
    static int cached = -1;
    if (cached < 0) {
        char value[16] = {};
        const DWORD length = GetEnvironmentVariableA(
            "QOH99_NP_DISABLE_ROLLBACK_BALANCE", value, sizeof(value));
        cached = (length != 0 && length < sizeof(value)
                  && std::strcmp(value, "1") == 0) ? 1 : 0;
    }
    return cached != 0;
}
static bool     g_netPacingSemanticHold = false;
static bool     g_timerPeriodActive = false;
static bool     g_offlinePollTimerPeriodActive = false;
using SetWrapperFrameLimitFn = BOOL (__stdcall*)(UINT);
using GetWrapperNetplayPacingAbiFn = UINT (__stdcall*)();
constexpr UINT kWrapperNetplayPacingAbi = 3u;
static bool g_wrapperPacingOwned = false;
static volatile LONG g_netOwnedTickSerial = 0;
static volatile LONG g_replayPresentedTickSerial = 0;
static volatile LONG g_netPacedTickSerial = 0;
static volatile LONG g_netCompletedTickSerial = 0;
static volatile LONG g_netPublishedTickSerial = 0;
static volatile LONG g_netPresentationSlotGrants = 0;
static volatile LONG g_netPresentationWaitCallbacks = 0;
static volatile LONG g_netPresentationCommitCallbacks = 0;
static volatile LONG g_netPresentationFailureCallbacks = 0;
static volatile LONG g_netPresentationFailureReasons[7] = {};
static volatile LONG g_netUnownedCommitCallbacks = 0;
static volatile LONG g_netPacingClockFault = 0;
static volatile LONG g_netPacingClockFaults = 0;
static volatile LONG g_netPacingClosing = 0;
static volatile LONG g_netPacingConsumersInFlight = 0;
static volatile LONG g_netCommitBoundaryGrants = 0;
static volatile LONG g_netNoCommitFallbackGrants = 0;
static volatile LONG g_netSpectatorBatchFallbackGrants = 0;
static volatile LONG g_netDuplicateCommitCallbacks = 0;
static volatile LONG g_netDuplicatePublishCallbacks = 0;
static volatile LONG g_netHiddenCommitBypasses = 0;
static volatile LONG g_netTestPacingSummaryRequest = 0;
static volatile LONG g_netTestPacingSummaryAcknowledged = 0;

void netLatchPacingClockFault() {
    if (InterlockedCompareExchange(
            &g_netPacingClockFault, 1, 0) == 0) {
        InterlockedIncrement(&g_netPacingClockFaults);
    }
}

class ScopedPacingConsumer {
public:
    ScopedPacingConsumer() {
        if (InterlockedCompareExchange(
                &g_netPacingClosing, 0, 0) != 0) {
            return;
        }
        InterlockedIncrement(&g_netPacingConsumersInFlight);
        if (InterlockedCompareExchange(
                &g_netPacingClosing, 0, 0) != 0) {
            InterlockedDecrement(&g_netPacingConsumersInFlight);
            return;
        }
        entered_ = true;
    }

    ~ScopedPacingConsumer() {
        if (entered_)
            InterlockedDecrement(&g_netPacingConsumersInFlight);
    }

    explicit operator bool() const { return entered_; }

private:
    bool entered_ = false;
};

SetWrapperFrameLimitFn netResolveWrapperFrameLimit() {
    const HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
    return wrapper != nullptr
        ? reinterpret_cast<SetWrapperFrameLimitFn>(
              GetProcAddress(wrapper, "EFZDDRAW_SetFrameLimit"))
        : nullptr;
}

GetWrapperNetplayPacingAbiFn netResolveWrapperPacingAbi() {
    const HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
    return wrapper != nullptr
        ? reinterpret_cast<GetWrapperNetplayPacingAbiFn>(
              GetProcAddress(
                  wrapper, "EFZDDRAW_GetNetplayPacingAbi"))
        : nullptr;
}

static qnetplay::RematchPrompt g_rematchPrompt;
static bool g_rematchEndRequested = false;

static bool g_netInstantRematchLaunch = false;
static uint32_t g_netInstantRematchBoundary = 0;
static std::array<qreplay::SelectorProofV2, qreplay::kFighterSlots>
    g_netStagedSelectorProof{};
static bool g_netStagedSelectorProofValid = false;
static uint32_t g_netRematchAutoAnswer = 0;
constexpr uint32_t kNetTerminateValueBack = 1;
constexpr uint32_t kNetTerminateValueRematchDeclined = 2;
constexpr uint32_t kNetTerminateValueRematchTimeout = 3;

constexpr uintptr_t kRematchTheaterCursorAddr = 0x8A5450u;
constexpr uintptr_t kRematchTheaterDwordAddrs[] = {
    0x62369Cu,
    0x61801Cu,
    0x8719A4u,
    0x6236A0u,
    0x6236A8u,  // Native loser verdict.
    0x6236CCu,  // Vacancy/COM mask.
    0x6236ACu,  // Postgame decide latch (sub_404C60 scratch).
    0x6236C0u,  // Continue-prompt slide counter.
    0x6236C4u,
    0x6236C8u,
    kRematchTheaterCursorAddr,
    0x8A5D20u,
    0x613F10u, 0x613F14u, 0x613F18u, 0x613F1Cu,
};
constexpr size_t kRematchTheaterDwordCount =
    sizeof(kRematchTheaterDwordAddrs) / sizeof(kRematchTheaterDwordAddrs[0]);

struct CanonicalRematchState {
    bool active = false;
    bool done = false;
    bool restartWritesPending = false;
    uint32_t doorwayFrame = 0;
    uint32_t loserSide = 0;
    uint32_t savedDwords[kRematchTheaterDwordCount] = {};
    uint32_t prev[2] = {};
    bool ready[2] = {};
    bool choiceNo[2] = {};
};
static CanonicalRematchState g_canonRematch;
constexpr uint32_t kCanonRematchTimeoutFrames = 3660;

struct RematchTheaterState {
    bool flipped = false;
    bool regionsSnapshotValid = false;
    uint32_t layoutGeneration = 0;
    uint32_t doorwayFrame = 0;
    uint32_t loserSide = 0;
    uint32_t savedDwords[kRematchTheaterDwordCount] = {};
    alignas(16) uint8_t fpu[512] = {};
    uint32_t prevSampleMask = 0;
    bool fallbackCursorNo = false;
    int64_t nextTickQpcUs = 0;
    uint32_t asyncTicks = 0;
    uint32_t feedMask = 0;
};
static RematchTheaterState g_rematchTheater;
static std::vector<uint8_t> g_rematchTheaterRegions;
static void netRematchTheaterRestore(const char* why);

struct OfflineRematchTheaterState {
    bool flipped = false;
    bool done = false;
    bool nativeRestartArmed = false;
    uint32_t loserSide = 0;
    uint32_t savedDwords[kRematchTheaterDwordCount] = {};
    uint32_t raw[2] = {};
    uint32_t prev[2] = {};
    bool ready[2] = {};
    bool choiceNo[2] = {};
};
static OfflineRematchTheaterState g_offlineRematch;

static uint32_t offlineRematchMaskServe(int player, uint32_t mask) {
    if (!g_offlineRematch.flipped || g_offlineRematch.done
        || player < 0 || player > 1)
        return mask;
    g_offlineRematch.raw[player] = mask;
    return mask & ~(qinput::kLogicalButtonMask
                    | qinput::kLogicalDirectionMask);
}
static void netOfflineRematchTick();

using PrepareOverlayDeviceFn = BOOL(__stdcall*)(void);
using RegisterHudProviderFn = void(__stdcall*)(
    EfzDx9NetplayHudProvider provider, void* userData);
static bool g_netHudRegistered = false;

static uint32_t g_netGameWins[2] = { 0u, 0u };
static bool g_netGameWinLatched = false;
static bool g_offlineVsScoreboard = false;
constexpr uintptr_t kRoundWinsP1Addr = 0x625628u;
constexpr uintptr_t kRoundWinsP2Addr = 0x62562Cu;
// dword_6236A8: the game's own winner-screen verdict (see sub_404660).
constexpr uintptr_t kMatchWinnerSlotAddr = 0x6236A8u;

void netTrackCompletedGameWins() {
    if (!g_netActive && !g_offlineVsScoreboard) return;
    const uint32_t roundCounter = rd32(addr::kRoundCounter);
    if (roundCounter < 5u) {
        g_netGameWinLatched = false;
        return;
    }
    if (g_netGameWinLatched) return;
    const int32_t matchSub = static_cast<int32_t>(rd32(kMatchSubAddr));
    if (matchSub < 2) return;
    const uint32_t winnerSlot = rd32(kMatchWinnerSlotAddr);
    if (winnerSlot > 1u) return;
    const uint32_t roundWinsP1 = rd32(kRoundWinsP1Addr);
    const uint32_t roundWinsP2 = rd32(kRoundWinsP2Addr);
    const uint32_t derivedSlot = roundWinsP1 <= roundWinsP2 ? 1u : 0u;
    if (derivedSlot != winnerSlot) {
        QLOG("[human-match] winner-slot DISAGREEMENT native=P%u derived=P%u "
             "(rounds=[%u,%u] matchSub=%d) - crediting the round tally",
             winnerSlot + 1u, derivedSlot + 1u, roundWinsP1, roundWinsP2,
             matchSub);
    }
    g_netGameWinLatched = true;
    ++g_netGameWins[derivedSlot];
    QLOG("[human-match] game complete winnerSlot=P%u wins=[%u,%u] "
         "(rounds=[%u,%u] matchSub=%d)",
         derivedSlot + 1u, g_netGameWins[0], g_netGameWins[1],
         roundWinsP1, roundWinsP2, matchSub);
}

template <size_t N>
void netHudStorePlayerIdentity(char (&destination)[N], size_t slot) {
    const std::string utf8Name = qgame::playerNameForUtf8Display(
        g_netPlayerNames[slot], g_netPlayerNameCodePages[slot]);
    std::snprintf(destination, N, "%s (%u)", utf8Name.c_str(),
                  g_netGameWins[slot]);
}

BOOL __stdcall netHudProvider(EfzDx9NetplayHudState* outState, void*) {
    if (outState == nullptr
        || outState->structSize != sizeof(EfzDx9NetplayHudState)) {
        return FALSE;
    }
    if (g_netHalted) {
        const char* banner;
        if (std::strcmp(g_netHaltReason, "rematch declined") == 0) {
            banner = "Rematch declined - session over";
        } else if (std::strcmp(g_netHaltReason, "rematch timed out") == 0) {
            banner = "Rematch timed out - session over";
        } else switch (g_netHaltCode) {
            case kNetProtocolViolation:
            case kNetConfirmedDesync:
                banner = "Match stopped: desync"; break;
            case kNetTransportFailure:
            case kNetTimeout:
                banner = "Opponent disconnected"; break;
            case kNetInternalFailure:
            case kNetInvalidConfiguration:
                banner = "Netplay error - match stopped"; break;
            case kNetPeerRequested:
            default:
                banner = "Opponent left the match"; break;
        }
        outState->playerOneName[0] = '\0';
        outState->playerTwoName[0] = '\0';
        std::snprintf(outState->matchHeader, sizeof(outState->matchHeader),
                      "%s", banner);
        std::snprintf(outState->sessionDetail, sizeof(outState->sessionDetail),
                      "%s", "returning to menu...");
        outState->practiceLineCount = 0;
        outState->debugLineCount = 0;
        outState->toastLineCount = 0;
        outState->visible = TRUE;
        return TRUE;
    }
    if (g_netActive && !g_netSpectatorMode
        && (g_rematchPrompt.active() || g_canonRematch.active)) {
        netHudStorePlayerIdentity(outState->playerOneName, 0u);
        netHudStorePlayerIdentity(outState->playerTwoName, 1u);
        if (g_rematchPrompt.advancing()) {
            std::snprintf(outState->matchHeader,
                          sizeof(outState->matchHeader), "%s",
                          "Rematch agreed!");
            std::snprintf(outState->sessionDetail,
                          sizeof(outState->sessionDetail), "%s",
                          g_spectatorServiceMode == 0u
                              ? "restarting the match..."
                              : "returning to character select...");
        } else {
            outState->matchHeader[0] = '\0';
            char you[32];
            if (g_canonRematch.active) {
                const size_t localSlot = g_netLocalCtrl == 0 ? 0u : 1u;
                const size_t remoteSlot = localSlot == 0u ? 1u : 0u;
                const bool hoverNo = !qnetplay::rematchTheaterCursorIsYes(
                    rd32(kRematchTheaterCursorAddr));
                if (g_canonRematch.ready[localSlot]) {
                    std::snprintf(you, sizeof(you), "%s",
                                  g_canonRematch.choiceNo[localSlot]
                                      ? "NO [OK]" : "YES [OK]");
                } else {
                    std::snprintf(you, sizeof(you), "on %s",
                                  hoverNo ? "NO" : "YES");
                }
                std::snprintf(outState->sessionDetail,
                              sizeof(outState->sessionDetail),
                              "You: %s   Opponent: %s", you,
                              g_canonRematch.ready[remoteSlot]
                                  ? (g_canonRematch.choiceNo[remoteSlot]
                                         ? "NO [OK]" : "YES [OK]")
                                  : "deciding...");
            } else if (g_rematchPrompt.localReady()) {
                std::snprintf(you, sizeof(you), "%s",
                              g_rematchPrompt.localChoice()
                                      == qnetplay::wire::kRematchChoiceRematch
                                  ? "YES [OK]" : "NO [OK]");
            } else if (g_rematchTheater.flipped) {
                std::snprintf(you, sizeof(you), "on %s",
                              qnetplay::rematchTheaterCursorIsYes(
                                  rd32(kRematchTheaterCursorAddr))
                                  ? "YES" : "NO");
            } else {
                std::snprintf(you, sizeof(you), "on %s",
                              g_rematchTheater.fallbackCursorNo
                                  ? "NO" : "YES");
            }
            if (!g_canonRematch.active) {
                const char* opp = "deciding...";
                if (g_rematchPrompt.remoteSeen()
                    && g_rematchPrompt.remoteReady()) {
                    opp = g_rematchPrompt.remoteChoice()
                            == qnetplay::wire::kRematchChoiceRematch
                        ? "YES [OK]" : "NO [OK]";
                }
                const uint64_t heldMs =
                    GetTickCount64() - g_rematchPrompt.startMs();
                const uint64_t leftMs =
                    heldMs < qnetplay::kRematchDecisionTimeoutMs
                        ? qnetplay::kRematchDecisionTimeoutMs - heldMs
                        : 0;
                std::snprintf(outState->sessionDetail,
                              sizeof(outState->sessionDetail),
                              "You: %s   Opponent: %s   (%us)",
                              you, opp,
                              static_cast<unsigned>(leftMs / 1000u));
            }
        }
        if (g_canonRematch.active) {
            const bool hoverNo = !qnetplay::rematchTheaterCursorIsYes(
                rd32(kRematchTheaterCursorAddr));
            outState->rematchPromptActive = TRUE;
            outState->rematchP1No =
                (g_canonRematch.ready[0] ? g_canonRematch.choiceNo[0]
                                         : hoverNo) ? TRUE : FALSE;
            outState->rematchP1Ready =
                g_canonRematch.ready[0] ? TRUE : FALSE;
            outState->rematchP2No =
                (g_canonRematch.ready[1] ? g_canonRematch.choiceNo[1]
                                         : hoverNo) ? TRUE : FALSE;
            outState->rematchP2Ready =
                g_canonRematch.ready[1] ? TRUE : FALSE;
        } else {
            const bool localNo = g_rematchPrompt.localReady()
                ? g_rematchPrompt.localChoice()
                      == qnetplay::wire::kRematchChoiceLeave
                : (g_rematchTheater.flipped
                       ? !qnetplay::rematchTheaterCursorIsYes(
                             rd32(kRematchTheaterCursorAddr))
                       : g_rematchTheater.fallbackCursorNo);
            const bool remoteNo = g_rematchPrompt.remoteSeen()
                && g_rematchPrompt.remoteChoice()
                       == qnetplay::wire::kRematchChoiceLeave;
            const bool localIsP1 = g_netLocalCtrl == 0;
            outState->rematchPromptActive = TRUE;
            outState->rematchP1No = (localIsP1 ? localNo : remoteNo)
                ? TRUE : FALSE;
            outState->rematchP1Ready =
                (localIsP1 ? g_rematchPrompt.localReady()
                           : g_rematchPrompt.remoteSeen()
                                 && g_rematchPrompt.remoteReady())
                ? TRUE : FALSE;
            outState->rematchP2No = (localIsP1 ? remoteNo : localNo)
                ? TRUE : FALSE;
            outState->rematchP2Ready =
                (localIsP1 ? g_rematchPrompt.remoteSeen()
                                 && g_rematchPrompt.remoteReady()
                           : g_rematchPrompt.localReady())
                ? TRUE : FALSE;
        }
        outState->practiceLineCount = 0;
        outState->debugLineCount = 0;
        outState->toastLineCount = 0;
        outState->visible = TRUE;
        return TRUE;
    }
    if (g_offlineRematch.flipped && !g_offlineRematch.done
        && !g_offlineRematch.nativeRestartArmed) {
        const bool hoverNo = !qnetplay::rematchTheaterCursorIsYes(
            rd32(kRematchTheaterCursorAddr));
        outState->rematchPromptActive = TRUE;
        outState->rematchP1No =
            (g_offlineRematch.ready[0] ? g_offlineRematch.choiceNo[0]
                                       : hoverNo) ? TRUE : FALSE;
        outState->rematchP1Ready = g_offlineRematch.ready[0] ? TRUE : FALSE;
        outState->rematchP2No =
            (g_offlineRematch.ready[1] ? g_offlineRematch.choiceNo[1]
                                       : hoverNo) ? TRUE : FALSE;
        outState->rematchP2Ready = g_offlineRematch.ready[1] ? TRUE : FALSE;
        outState->visible = TRUE;
        return TRUE;
    }
    const uint32_t hudScene = rd32(kSceneStateAddr);
    const bool hudGameplay = hudScene == 2u && rd32(kMatchSubAddr) == 0u;
    const bool hudCharSelect = hudScene == 3u;
    if (!g_netActive && g_offlineVsScoreboard) {
        if (!hudGameplay && !hudCharSelect) {
            outState->visible = FALSE;
            return TRUE;
        }
        std::snprintf(outState->playerOneName,
                      sizeof(outState->playerOneName), "Wins: %u",
                      g_netGameWins[0]);
        std::snprintf(outState->playerTwoName,
                      sizeof(outState->playerTwoName), "Wins: %u",
                      g_netGameWins[1]);
        outState->matchHeader[0] = '\0';
        outState->sessionDetail[0] = '\0';
        outState->practiceLineCount = 0;
        outState->debugLineCount = 0;
        outState->toastLineCount = 0;
        outState->visible = TRUE;
        return TRUE;
    }
    if (!g_netActive || (!hudGameplay && !hudCharSelect)) {
        outState->visible = FALSE;
        return TRUE;
    }
    netHudStorePlayerIdentity(outState->playerOneName, 0u);
    netHudStorePlayerIdentity(outState->playerTwoName, 1u);
    outState->matchHeader[0] = '\0';
    char pingText[48];
    qgame::NetPingStats ping{};
    if (qgame::netPeerPingStats(ping)) {
        ping.meanMicros = ping.lastMicros;
        if (ping.meanMicros < 100u) {
            std::snprintf(pingText, sizeof(pingText), "<0.1 ms");
        } else if (ping.meanMicros < 10000u) {
            std::snprintf(pingText, sizeof(pingText), "%u.%u ms",
                          ping.meanMicros / 1000u,
                          (ping.meanMicros % 1000u) / 100u);
        } else {
            std::snprintf(pingText, sizeof(pingText), "%u ms",
                          (ping.meanMicros + 500u) / 1000u);
        }
    } else if (qgame::netPeerHasRtt()) {
        std::snprintf(pingText, sizeof(pingText), "%u ms",
                      qgame::netPeerRttMs());
    } else {
        std::snprintf(pingText, sizeof(pingText), "...");
    }
    if (g_netSpectatorMode) {
        SpectatorPlaybackReadout readout;
        spectatorPlaybackStatus(readout);
        char progress[64];
        char game[24] = "";
        if (!readout.active) {
            std::snprintf(progress, sizeof(progress), "connecting...");
        } else {
            if (readout.currentMatchId != 0ul && readout.sourceMatchId != 0ul) {
                std::snprintf(game, sizeof(game), "game %lu/%lu   ",
                              readout.currentMatchId, readout.sourceMatchId);
            } else if (readout.currentMatchId != 0ul) {
                std::snprintf(game, sizeof(game), "game %lu   ",
                              readout.currentMatchId);
            }
            const unsigned long long behindRecords = readout.behindLiveRecords;
            const unsigned behindSeconds = static_cast<unsigned>(
                (behindRecords * 10ull + 303ull) / 606ull);
            const unsigned long long liveTarget =
                static_cast<unsigned long long>(readout.liveTargetRecords)
                + readout.cushionBonusRecords;
            const bool live = !readout.catchingUp
                && behindRecords <= liveTarget + 120ull;
            if (readout.paused) {
                std::snprintf(progress, sizeof(progress), "paused   %us behind",
                              behindSeconds);
            } else if (live) {
                std::snprintf(progress, sizeof(progress), "live");
            } else if (readout.catchingUp) {
                std::snprintf(progress, sizeof(progress),
                              "catching up %lux   %us behind",
                              readout.catchupSpeedPermille / 1000ul,
                              behindSeconds);
            } else {
                std::snprintf(progress, sizeof(progress), "%us behind",
                              behindSeconds);
            }
            const bool behindByGame =
                readout.sourceMatchId != 0ul && readout.currentMatchId != 0ul
                && readout.sourceMatchId > readout.currentMatchId;
            if (g_spectatorSkipKeyLabel[0] != '\0' && !readout.fastForward
                && !readout.paused && (behindByGame || behindSeconds >= 15u)
                && ((GetTickCount64() / 1000ull) % 8ull) < 3ull) {
                std::snprintf(progress, sizeof(progress),
                              "press %s to skip to %s",
                              g_spectatorSkipKeyLabel,
                              behindByGame ? "the latest game" : "live");
            }
        }
        std::snprintf(outState->sessionDetail,
                      sizeof(outState->sessionDetail),
                      "spectating   %s%s", game, progress);
    } else {
        std::snprintf(outState->sessionDetail,
                      sizeof(outState->sessionDetail),
                      "ping %s   delay %u   rollback %u",
                      pingText, g_netDelay, g_netRollback);
    }
    outState->practiceLineCount = 0;
    outState->debugLineCount = 0;
    outState->toastLineCount = 0;
    outState->visible = TRUE;
    return TRUE;
}

bool netPrepareOverlayHud() {
    const HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
    if (wrapper == nullptr) return false;
    const auto prepare = reinterpret_cast<PrepareOverlayDeviceFn>(
        GetProcAddress(wrapper, "EFZDDRAW_PrepareOverlayDevice"));
    const auto registerHud = reinterpret_cast<RegisterHudProviderFn>(
        GetProcAddress(wrapper, "EFZDX9_RegisterNetplayHudProvider"));
    if (prepare == nullptr || registerHud == nullptr) {
        QLOG("[netplay-hud] wrapper lacks overlay entry points "
             "(prepare=%p register=%p); HUD disabled",
             reinterpret_cast<void*>(prepare),
             reinterpret_cast<void*>(registerHud));
        return false;
    }
    const BOOL readyNow = prepare();
    if (!readyNow) {
        QLOG("[netplay-hud] renderer readiness barrier is pending/failed");
        return false;
    }
    if (!g_netHudRegistered) {
        registerHud(&netHudProvider, nullptr);
        g_netHudRegistered = true;
        QLOG("[netplay-hud] registered after renderer readiness barrier");
    }
    return true;
}

static uint64_t g_netHudNextPrepareAttemptMs = 0u;
static constexpr uint64_t kNetHudPrepareRetryIntervalMs = 250u;

static bool netRetryDeferredOverlayHud() {
    if ((!g_wrapperPacingOwned && !g_offlineVsScoreboard)
        || g_netHudRegistered)
        return g_netHudRegistered;
    const uint64_t nowMs = GetTickCount64();
    if (nowMs < g_netHudNextPrepareAttemptMs)
        return false;
    if (netPrepareOverlayHud()) {
        g_netHudNextPrepareAttemptMs = 0u;
        return true;
    }
    g_netHudNextPrepareAttemptMs = nowMs + kNetHudPrepareRetryIntervalMs;
    return false;
}

bool netAcquireWrapperPacingOwnership() {
    if (g_wrapperPacingOwned)
        return true;
    const GetWrapperNetplayPacingAbiFn getPacingAbi =
        netResolveWrapperPacingAbi();
    if (getPacingAbi == nullptr
        || getPacingAbi() != kWrapperNetplayPacingAbi) {
        QLOG_ISSUE("[pacing-owner] FAILED: ddraw wrapper lacks canonical "
             "presentation-slot pacing ABI %u",
             kWrapperNetplayPacingAbi);
        return false;
    }
    const SetWrapperFrameLimitFn setFrameLimit =
        netResolveWrapperFrameLimit();
    if (setFrameLimit == nullptr) {
        QLOG_ISSUE("[pacing-owner] FAILED: ddraw wrapper lacks "
             "EFZDDRAW_SetFrameLimit");
        return false;
    }
    if (!netPrepareOverlayHud())
        QLOG("[pacing-owner] HUD preparation deferred until QOH window is live");
    if (!setFrameLimit(0u)) {
        QLOG_ISSUE("[pacing-owner] FAILED: ddraw wrapper rejected limiter handoff");
        return false;
    }
    g_wrapperPacingOwned = true;
    QLOG("[pacing-owner] acquired: wrapper limiter=off owner=netplay-dll");
    return true;
}

void netReleaseWrapperPacingOwnership() {
    if (!g_wrapperPacingOwned)
        return;
    const SetWrapperFrameLimitFn setFrameLimit =
        netResolveWrapperFrameLimit();
    if (setFrameLimit == nullptr || !setFrameLimit(60u)) {
        QLOG("[pacing-owner] release FAILED: wrapper limiter restore=60");
    } else {
        QLOG("[pacing-owner] released: wrapper limiter=60");
    }
    g_wrapperPacingOwned = false;
}

int64_t netUnhookedQpcNow();

inline void netPaceInit() {
    LARGE_INTEGER f{};
    g_qpcFreq =
        QueryPerformanceFrequency(&f) ? f.QuadPart : 0;
    g_paceTicks = (g_qpcFreq * 10) / 606;
    g_paceDeadline.configure(g_paceTicks);
    g_lastPacePoll = {};
    g_netPacingSemanticHold = false;
    InterlockedExchange(&g_netOwnedTickSerial, 0);
    InterlockedExchange(&g_netPacedTickSerial, 0);
    InterlockedExchange(&g_netCompletedTickSerial, 0);
    InterlockedExchange(&g_netPublishedTickSerial, 0);
    InterlockedExchange(&g_netPresentationSlotGrants, 0);
    InterlockedExchange(&g_netPresentationWaitCallbacks, 0);
    InterlockedExchange(&g_netPresentationCommitCallbacks, 0);
    InterlockedExchange(&g_netPresentationFailureCallbacks, 0);
    for (volatile LONG& reason : g_netPresentationFailureReasons)
        InterlockedExchange(&reason, 0);
    InterlockedExchange(&g_netUnownedCommitCallbacks, 0);
    InterlockedExchange(&g_netPacingClockFault, 0);
    InterlockedExchange(&g_netPacingClockFaults, 0);
    InterlockedExchange(&g_netPacingClosing, 0);
    InterlockedExchange(&g_netPacingConsumersInFlight, 0);
    InterlockedExchange(&g_netCommitBoundaryGrants, 0);
    InterlockedExchange(&g_netNoCommitFallbackGrants, 0);
    InterlockedExchange(&g_netSpectatorBatchFallbackGrants, 0);
    InterlockedExchange(&g_netDuplicateCommitCallbacks, 0);
    InterlockedExchange(&g_netDuplicatePublishCallbacks, 0);
    InterlockedExchange(&g_netHiddenCommitBypasses, 0);
    InterlockedExchange(&g_netTestPacingSummaryRequest, 0);
    InterlockedExchange(&g_netTestPacingSummaryAcknowledged, 0);
    if (!g_timerPeriodActive) {
        g_timerPeriodActive = (timeBeginPeriod(1) == TIMERR_NOERROR);
    }
    QLOG("[pacing-owner] timer-period-1ms active=%u",
         g_timerPeriodActive ? 1u : 0u);
}
inline void netPaceShutdown() {
    g_netPacingSemanticHold = false;
    if (g_timerPeriodActive) {
        timeEndPeriod(1);
        g_timerPeriodActive = false;
    }
}

inline void offlinePollWaitBrief() {
    if (!g_offlinePollTimerPeriodActive) {
        g_offlinePollTimerPeriodActive =
            timeBeginPeriod(1) == TIMERR_NOERROR;
        QLOG("[offline-poll] native no-progress wait enabled timer1ms=%u",
             g_offlinePollTimerPeriodActive ? 1u : 0u);
    }
    if (g_offlinePollTimerPeriodActive) {
        Sleep(1u);
        return;
    }
    if (!SwitchToThread())
        Sleep(0u);
}

inline void offlinePollWaitShutdown() {
    if (!g_offlinePollTimerPeriodActive)
        return;
    timeEndPeriod(1);
    g_offlinePollTimerPeriodActive = false;
}

inline void netPaceWaitBrief() {
    if (g_timerPeriodActive) {
        Sleep(1);
        return;
    }
    if (!SwitchToThread())
        Sleep(0);
}

bool netConsumePresentationSlot(const LONG ownedTickSerial) {
    const ScopedPacingConsumer consumer;
    if (!consumer)
        return false;
    if (ownedTickSerial == 0)
        return false;
    if (InterlockedCompareExchange(
            &g_netPacingClockFault, 0, 0) != 0) {
        return false;
    }
    if (InterlockedCompareExchange(
            &g_netPacedTickSerial, 0, 0) == ownedTickSerial) {
        return true;
    }

    qgame::netPublishGameThreadHeartbeat(
        qgame::NetTransportDiagnostics::GameThreadStage::PresentationWait,
        static_cast<uint32_t>(g_ovrFrame));
    const uint64_t waitStartedMs = GetTickCount64();
    while (InterlockedCompareExchange(
               &g_netPacingClosing, 0, 0) == 0
           && ((g_netActive && g_netRunning)
               || (g_replayRoleActive
                   && !qgame::replay::replayHeadlessV3()))
           && g_wrapperPacingOwned) {
        const int64_t now = netUnhookedQpcNow();
        if (now <= 0) {
            netLatchPacingClockFault();
            QLOG_ISSUE("[pacing-owner] FAILED: unhooked presentation clock "
                 "is unavailable");
            return false;
        }
        g_lastPacePoll = g_paceDeadline.poll(now);
        if (g_lastPacePoll.rebased())
            ++g_netPacingDeadlineRebases;
        if (g_lastPacePoll.ready()) {
            g_netPacingSemanticHold = false;
            InterlockedExchange(
                &g_netPacedTickSerial, ownedTickSerial);
            InterlockedIncrement(&g_netPresentationSlotGrants);
            return true;
        }
        if (GetTickCount64() - waitStartedMs > 500u) {
            netLatchPacingClockFault();
            QLOG_ISSUE("[pacing-owner] FAILED: unhooked presentation clock "
                 "did not reach its deadline within 500 ms");
            return false;
        }
        if (g_lastPacePoll.spacingBlocked())
            ++g_netPacingSpacingWaits;
        else
            ++g_netPacingDeadlineWaits;
        netPaceWaitBrief();
    }
    return false;
}

static bool netPaceIdleSlot() {
    const ScopedPacingConsumer consumer;
    if (!consumer)
        return false;
    if (InterlockedCompareExchange(
            &g_netPacingClockFault, 0, 0) != 0) {
        return false;
    }
    const uint64_t waitStartedMs = GetTickCount64();
    while (InterlockedCompareExchange(
               &g_netPacingClosing, 0, 0) == 0
           && ((g_netActive && g_netRunning)
               || (g_replayRoleActive
                   && !qgame::replay::replayHeadlessV3()))
           && g_wrapperPacingOwned) {
        const int64_t now = netUnhookedQpcNow();
        if (now <= 0) {
            netLatchPacingClockFault();
            QLOG_ISSUE("[pacing-owner] FAILED: unhooked presentation clock "
                 "is unavailable");
            return false;
        }
        g_lastPacePoll = g_paceDeadline.poll(now);
        if (g_lastPacePoll.rebased())
            ++g_netPacingDeadlineRebases;
        if (g_lastPacePoll.ready()) {
            g_netPacingSemanticHold = false;
            return true;
        }
        if (GetTickCount64() - waitStartedMs > 500u) {
            netLatchPacingClockFault();
            QLOG_ISSUE("[pacing-owner] FAILED: unhooked presentation clock "
                 "did not reach its deadline within 500 ms");
            return false;
        }
        if (g_lastPacePoll.spacingBlocked())
            ++g_netPacingSpacingWaits;
        else
            ++g_netPacingDeadlineWaits;
        netPaceWaitBrief();
    }
    return false;
}

inline void netPaceSemanticHold(
    qgame::HoldCause cause = qgame::HoldCause::None) {
    g_netLastHoldCause = cause;
    if (g_netPacingSemanticHold)
        return;
    g_paceDeadline.semanticHold(netUnhookedQpcNow());
    g_lastPacePoll = {};
    g_netPacingSemanticHold = true;
}
inline uint64_t netPaceDebtUs() {
    const int64_t debt =
        g_paceDeadline.debtTicks(netUnhookedQpcNow());
    if (debt <= 0 || g_qpcFreq <= 0)
        return 0;
    return static_cast<uint64_t>(debt) * 1000000ull
        / static_cast<uint64_t>(g_qpcFreq);
}

inline uint64_t netQpcTicksToUs(const int64_t sampled) {
    if (sampled <= 0 || g_qpcFreq <= 0)
        return GetTickCount64() * 1000ull;
    const uint64_t ticks = static_cast<uint64_t>(sampled);
    const uint64_t frequency = static_cast<uint64_t>(g_qpcFreq);
    return (ticks / frequency) * 1000000ull
        + ((ticks % frequency) * 1000000ull) / frequency;
}

inline uint64_t netRealQpcUs() {
    return netQpcTicksToUs(netUnhookedQpcNow());
}

inline uint32_t netTraceFrameNumber() {
    return g_netActive && g_netSession
        ? g_netSession->currentFrame()
        : static_cast<uint32_t>(g_frame);
}

static qgame::LifecycleStateSample g_stateLast{};
static bool g_stateLastValid = false;
static uint64_t g_stateLastUs = 0;
static qgame::KoWindowAccounting g_koWindow{};

inline qgame::LifecycleStateSample netSampleLifecycleState() {
    qgame::LifecycleStateSample v{};
    v.nativeTick   = rd32(0x4AB778u);
    v.scene        = rd32(kSceneStateAddr);
    v.sceneNext    = rd32(kSceneNextAddr);
    v.matchSub     = static_cast<int32_t>(rd32(kMatchSubAddr));
    v.scene3Sub    = rd32(addr::kScene3Substate);
    v.koStage      = rd32(kRoundResultStateAddr);
    v.roundCounter = rd32(addr::kRoundCounter);
    v.matchOver    = rd32(0x8719A4u);
    v.roundWinsP1  = rd32(kRoundWinsP1Addr);
    v.roundWinsP2  = rd32(kRoundWinsP2Addr);
    v.formationRow = rd32(0x62369Cu);
    v.cpuMask      = rd32(0x6236CCu);
    v.finishTimerA = rd32(kFinishStageTimerAddr);
    v.finishTimerB = rd32(0x4AA408u);
    return v;
}

void netReportKoWindow(const char* why, const uint64_t nowUs) {
    if (!g_koWindow.active()) return;
    const uint32_t frames = g_koWindow.frames();
    const uint64_t wallUs = g_koWindow.elapsedUs(nowUs);
    const uint64_t nativeUs = qgame::KoWindowAccounting::nativeCostUs(frames);
    QLOG("[ko-window] END reason=%s startFrame=%u frames=%u wallUs=%llu "
         "nativeUs=%llu stretchPermille=%u meanFps=%u.%02u",
         why, g_koWindow.startFrame(), frames,
         static_cast<unsigned long long>(wallUs),
         static_cast<unsigned long long>(nativeUs),
         qgame::KoWindowAccounting::stretchPermille(frames, wallUs),
         wallUs ? static_cast<unsigned>(
                      (static_cast<uint64_t>(frames) * 1000000ull) / wallUs)
                : 0u,
         wallUs ? static_cast<unsigned>(
                      ((static_cast<uint64_t>(frames) * 100000000ull) / wallUs)
                      % 100ull)
                : 0u);
    for (size_t slot = 0; slot < qgame::KoWindowAccounting::kSlots; ++slot) {
        const uint32_t sf = g_koWindow.stageFrames(slot);
        if (sf == 0u) continue;
        const uint64_t su = g_koWindow.stageUs(slot);
        const uint32_t stage =
            qgame::KoWindowAccounting::stageForSlot(slot);
        QLOG("[ko-window]   stage=%d %-16s frames=%u wallUs=%llu "
             "usPerFrame=%llu nativeUsPerFrame=16500",
             static_cast<int>(stage),
             stage == 0xFFFFFFFFu ? "variant"
                                  : qgame::koStageName(stage),
             sf, static_cast<unsigned long long>(su),
             static_cast<unsigned long long>(su / (sf ? sf : 1u)));
    }
    g_koWindow.close();
}

void netObserveLifecycleState() {
    const uint32_t tick = rd32(0x4AB778u);
    if (g_stateLastValid && tick == g_stateLast.nativeTick)
        return;
    const qgame::LifecycleStateSample now = netSampleLifecycleState();
    const uint64_t nowUs = netRealQpcUs();
    const uint64_t deltaUs =
        (g_stateLastValid && nowUs > g_stateLastUs) ? nowUs - g_stateLastUs
                                                    : 0u;
    if (!g_stateLastValid) {
        g_stateLast = now;
        g_stateLastValid = true;
        g_stateLastUs = nowUs;
        QLOG("[state] FIRST scene=%u(%s) next=%u matchSub=%d(%s) scene3Sub=%u "
             "koStage=%u(%s) round=%u tallies=%u/%u formation=%u cpuMask=%u",
             now.scene, qgame::sceneName(now.scene), now.sceneNext,
             now.matchSub, qgame::matchSubName(now.matchSub), now.scene3Sub,
             now.koStage, qgame::koStageName(now.koStage), now.roundCounter,
             now.roundWinsP1, now.roundWinsP2, now.formationRow, now.cpuMask);
        return;
    }
    const qgame::LifecycleStateSample& was = g_stateLast;

    if (g_koWindow.active())
        g_koWindow.accrue(was.koStage, deltaUs);

    if (was.koStage == 0u && now.koStage != 0u) {
        g_koWindow.open(netTraceFrameNumber(), nowUs);
        QLOG("[ko-window] OPEN stage=%u(%s) frame=%u tallies=%u/%u round=%u "
             "delay=%u maxRb=%u",
             now.koStage, qgame::koStageName(now.koStage),
             netTraceFrameNumber(),
             now.roundWinsP1, now.roundWinsP2, now.roundCounter,
             g_netActive && g_netSession
                 ? static_cast<unsigned>(
                       g_netSession->config().inputDelayFrames) : 0u,
             static_cast<unsigned>(g_netRollback));
    } else if (was.koStage != 0u && now.koStage == 0u) {
        netReportKoWindow("stage-returned-to-combat", nowUs);
    } else if (g_koWindow.active() && now.scene != 2u) {
        netReportKoWindow("left-match-scene", nowUs);
    }

    if (now.differsIgnoringTimers(was)) {
        char what[320];
        what[0] = '\0';
        int n = 0;
        auto add = [&](const char* fmt, auto... args) {
            if (n < 0 || n >= static_cast<int>(sizeof(what))) return;
            const int w = std::snprintf(what + n, sizeof(what) - n,
                                        fmt, args...);
            if (w > 0) n += w;
        };
        if (now.scene != was.scene)
            add(" scene=%u(%s)->%u(%s)", was.scene, qgame::sceneName(was.scene),
                now.scene, qgame::sceneName(now.scene));
        if (now.sceneNext != was.sceneNext)
            add(" next=%u->%u", was.sceneNext, now.sceneNext);
        if (now.matchSub != was.matchSub)
            add(" matchSub=%d(%s)->%d(%s)", was.matchSub,
                qgame::matchSubName(was.matchSub), now.matchSub,
                qgame::matchSubName(now.matchSub));
        if (now.scene3Sub != was.scene3Sub)
            add(" scene3Sub=%u->%u", was.scene3Sub, now.scene3Sub);
        if (now.koStage != was.koStage)
            add(" koStage=%u(%s)->%u(%s)", was.koStage,
                qgame::koStageName(was.koStage), now.koStage,
                qgame::koStageName(now.koStage));
        if (now.roundCounter != was.roundCounter)
            add(" round=%u->%u", was.roundCounter, now.roundCounter);
        if (now.matchOver != was.matchOver)
            add(" matchOver=%u->%u", was.matchOver, now.matchOver);
        if (now.roundWinsP1 != was.roundWinsP1
            || now.roundWinsP2 != was.roundWinsP2)
            add(" tallies=%u/%u->%u/%u", was.roundWinsP1, was.roundWinsP2,
                now.roundWinsP1, now.roundWinsP2);
        if (now.formationRow != was.formationRow)
            add(" formation=%u->%u", was.formationRow, now.formationRow);
        if (now.cpuMask != was.cpuMask)
            add(" cpuMask=%u->%u", was.cpuMask, now.cpuMask);
        QLOG("[state] frame=%u tick=%u sinceUs=%llu%s | stageTimer=%u "
             "exactWindow=%u",
             netTraceFrameNumber(),
             now.nativeTick, static_cast<unsigned long long>(deltaUs),
             what, now.finishTimerA,
             qgame::lifecycleTransitionRequiresExactInput(
                 now.koStage, now.roundCounter,
                 now.finishTimerA) ? 1u : 0u);
    }
    g_stateLast = now;
    g_stateLastUs = nowUs;
}

const char* netRollbackTransitionName(
    const qgame::RollbackStallTransitionKind kind) {
    switch (kind) {
        case qgame::RollbackStallTransitionKind::None: return "none";
        case qgame::RollbackStallTransitionKind::Began: return "begin";
        case qgame::RollbackStallTransitionKind::ReasonChanged:
            return "reason-change";
        case qgame::RollbackStallTransitionKind::Ended: return "end";
    }
    return "unknown";
}

void netLogRollbackStallContext(
    const char* event,
    const qgame::RollbackStallTransition* transition,
    const qnetplay::RollbackDiagnostics& diag,
    const qnetplay::RollbackStats& stats,
    const uint64_t episodeAgeUs) {
    qgame::NetTransportDiagnostics link{};
    (void)qgame::netGetTransportDiagnostics(link);
    const qnetplay::StallReason from =
        transition ? transition->from : diag.activeStallReason;
    const qnetplay::StallReason to =
        transition ? transition->to : diag.activeStallReason;
    const uint64_t priorReasonUs =
        transition ? transition->previousReasonDurationUs : 0u;
    QLOG("[rollback-stall-v1] event=%s from=%s to=%s "
         "episodeUs=%llu reasonUs=%llu frame=%u predDepth=%u "
         "nextRemote=%u sessionNewest=%u/%u hole=%u/%u "
         "remoteAtFrontier=%u localAtFrontier=%u "
         "confirmedNext=%u confirmedLag=%u pendingRb=%u/%u "
         "replay=%u/%u/%u/%u polls=%llu episodes=%llu consecutive=%u "
         "coreRemote=%u/%u newest=%u/%u reorderSpan=%u "
         "lastInputAgeMs=%llu lastValidAgeMs=%llu "
         "peerAck=%u/%u localHigh=%u/%u peerFrontier=%u/%u ageMs=%llu "
         "ingress=%u/%u lastPresent=%u last=%s(%u) lastAgeMs=%llu "
         "pingUs=%u/%u jitterUs=%u lossPermille=%u "
         "gameThread=%s/%u/%llu socketErrors=%u/%u lastWsa=%d",
         event ? event : "unknown",
         qnetplay::stallReasonName(from),
         qnetplay::stallReasonName(to),
         static_cast<unsigned long long>(episodeAgeUs),
         static_cast<unsigned long long>(priorReasonUs),
         diag.frontier, diag.speculativeFrames,
         diag.nextRemoteExpected,
         diag.hasRemoteNewestActual ? 1u : 0u,
         diag.remoteNewestActual,
         diag.hasRemoteHole ? 1u : 0u, diag.remoteHoleSpan,
         diag.remoteActualAtFrontier ? 1u : 0u,
         diag.localInputAtFrontier ? 1u : 0u,
         diag.nextConfirmedFrame,
         diag.frontier - diag.nextConfirmedFrame,
         diag.pendingRollback ? 1u : 0u, diag.pendingRollbackFrom,
         diag.replayInProgress ? 1u : 0u,
         diag.replayFrom, diag.nextReplayFrame,
         diag.replayUntilExclusive,
         static_cast<unsigned long long>(stats.stallPolls),
         static_cast<unsigned long long>(stats.stallEpisodes),
         diag.consecutiveStallPolls,
         link.core.hasRemoteContiguous ? 1u : 0u,
         link.core.remoteContiguousThrough,
         link.core.hasRemoteNewest ? 1u : 0u,
         link.core.remoteNewest, link.core.remoteReorderSpan,
         static_cast<unsigned long long>(
             link.core.lastNewRemoteInputAgeMs),
         static_cast<unsigned long long>(
             link.core.lastValidReceiveAgeMs),
         link.core.hasPeerAck ? 1u : 0u,
         link.core.peerAckThrough,
         link.core.hasLocalInput ? 1u : 0u,
         link.core.highestLocalInput,
         link.core.hasPeerSimFrontier ? 1u : 0u,
         link.core.peerSimFrontier,
         static_cast<unsigned long long>(
             link.core.peerFrontierAgeMs),
         link.ingressAccepted, link.ingressRejected,
         link.hasLastIngress ? 1u : 0u,
         qnetplay::ingressResultName(link.lastIngress),
         static_cast<unsigned>(link.lastIngress),
         static_cast<unsigned long long>(link.lastIngressAgeMs),
         link.hasPing ? link.ping.meanMicros : 0u,
         link.hasPing ? link.ping.p95Micros : 0u,
         link.hasPing ? link.ping.jitterMicros : 0u,
         link.hasPing ? link.ping.lossPerMille : 0u,
         qgame::netGameThreadStageName(link.gameThreadStage),
         link.gameThreadFrame,
         static_cast<unsigned long long>(
             link.gameThreadStageAgeMs),
         link.receiveErrors, link.sendFailures,
         link.lastSocketError);
}

void netLogRollbackAdapterHoldContext(
    const char* event,
    const qgame::AdapterHoldTransition* transition,
    const qnetplay::RollbackDiagnostics& diag,
    const qnetplay::RollbackStats& stats,
    const uint64_t episodeAgeUs) {
    qgame::NetTransportDiagnostics link{};
    (void)qgame::netGetTransportDiagnostics(link);
    const qgame::AdapterHoldReason from =
        transition ? transition->from : g_netAdapterHoldReason;
    const qgame::AdapterHoldReason to =
        transition ? transition->to : g_netAdapterHoldReason;
    const uint64_t priorReasonUs =
        transition ? transition->previousReasonDurationUs : 0u;
    QLOG("[rollback-adapter-hold-v1] event=%s from=%s to=%s "
         "episodeUs=%llu reasonUs=%llu frame=%u scene=%u matchSub=%d "
         "schedulerStall=%s predDepth=%u nextRemote=%u "
         "sessionNewest=%u/%u hole=%u/%u confirmedNext=%u lag=%u "
         "pendingRb=%u/%u replay=%u/%u/%u/%u "
         "forward=%llu replayFrames=%llu "
         "coreRemote=%u/%u newest=%u/%u reorderSpan=%u "
         "inputAgeMs=%llu validRxAgeMs=%llu peerAck=%u/%u "
         "peerFrontier=%u/%u/%llu ingress=%u/%u last=%u/%s(%u)/%llu "
         "pingUs=%u/%u jitterUs=%u lossPermille=%u "
         "gameThread=%s/%u/%llu socketErrors=%u/%u/%d",
         event ? event : "unknown",
         qgame::adapterHoldReasonName(from),
         qgame::adapterHoldReasonName(to),
         static_cast<unsigned long long>(episodeAgeUs),
         static_cast<unsigned long long>(priorReasonUs),
         diag.frontier, rd32(kSceneStateAddr),
         static_cast<int32_t>(rd32(kMatchSubAddr)),
         qnetplay::stallReasonName(diag.activeStallReason),
         diag.speculativeFrames, diag.nextRemoteExpected,
         diag.hasRemoteNewestActual ? 1u : 0u,
         diag.remoteNewestActual,
         diag.hasRemoteHole ? 1u : 0u, diag.remoteHoleSpan,
         diag.nextConfirmedFrame,
         diag.frontier - diag.nextConfirmedFrame,
         diag.pendingRollback ? 1u : 0u, diag.pendingRollbackFrom,
         diag.replayInProgress ? 1u : 0u,
         diag.replayFrom, diag.nextReplayFrame,
         diag.replayUntilExclusive,
         static_cast<unsigned long long>(stats.visibleAdvances),
         static_cast<unsigned long long>(stats.replayedFrames),
         link.core.hasRemoteContiguous ? 1u : 0u,
         link.core.remoteContiguousThrough,
         link.core.hasRemoteNewest ? 1u : 0u,
         link.core.remoteNewest, link.core.remoteReorderSpan,
         static_cast<unsigned long long>(
             link.core.lastNewRemoteInputAgeMs),
         static_cast<unsigned long long>(
             link.core.lastValidReceiveAgeMs),
         link.core.hasPeerAck ? 1u : 0u,
         link.core.peerAckThrough,
         link.core.hasPeerSimFrontier ? 1u : 0u,
         link.core.peerSimFrontier,
         static_cast<unsigned long long>(
             link.core.peerFrontierAgeMs),
         link.ingressAccepted, link.ingressRejected,
         link.hasLastIngress ? 1u : 0u,
         qnetplay::ingressResultName(link.lastIngress),
         static_cast<unsigned>(link.lastIngress),
         static_cast<unsigned long long>(link.lastIngressAgeMs),
         link.hasPing ? link.ping.meanMicros : 0u,
         link.hasPing ? link.ping.p95Micros : 0u,
         link.hasPing ? link.ping.jitterMicros : 0u,
         link.hasPing ? link.ping.lossPerMille : 0u,
         qgame::netGameThreadStageName(link.gameThreadStage),
         link.gameThreadFrame,
         static_cast<unsigned long long>(
             link.gameThreadStageAgeMs),
         link.receiveErrors, link.sendFailures,
         link.lastSocketError);
}

void netObserveRollbackHealth(
    const char* source, const bool forceReport = false,
    const qgame::AdapterHoldReason adapterHoldUpdate =
        qgame::AdapterHoldReason::Count) {
    if (!g_netSession)
        return;
    if (adapterHoldUpdate != qgame::AdapterHoldReason::Count)
        g_netAdapterHoldReason = adapterHoldUpdate;
    const qnetplay::RollbackDiagnostics diag =
        g_netSession->diagnostics();
    const qnetplay::RollbackStats stats =
        g_netSession->stats();
    const qgame::RollbackHealthObservation observed =
        g_netRollbackHealth.observe(
            netRealQpcUs(), diag, stats, g_netAdapterHoldReason);

    if (observed.stall.occurred()) {
        netLogRollbackStallContext(
            netRollbackTransitionName(observed.stall.kind),
            &observed.stall, diag, stats,
            observed.stall.episodeDurationUs);
        if (observed.stall.kind
                == qgame::RollbackStallTransitionKind::Began
            || observed.stall.kind
                == qgame::RollbackStallTransitionKind::ReasonChanged) {
            g_netRollbackNextStallLogUs = 100000u;
        } else if (observed.stall.kind
                   == qgame::RollbackStallTransitionKind::Ended) {
            g_netRollbackNextStallLogUs = 100000u;
        }
    }

    if (observed.adapterHold.occurred()) {
        netLogRollbackAdapterHoldContext(
            netRollbackTransitionName(observed.adapterHold.kind),
            &observed.adapterHold, diag, stats,
            observed.adapterHold.episodeDurationUs);
        g_netRollbackNextAdapterHoldLogUs = 100000u;
    }
    if (g_netAdapterHoldReason != qgame::AdapterHoldReason::None) {
        const uint64_t ageUs =
            g_netRollbackHealth.currentAdapterHoldReasonAgeUs();
        if (ageUs >= g_netRollbackNextAdapterHoldLogUs) {
            netLogRollbackAdapterHoldContext(
                "progress", nullptr, diag, stats,
                g_netRollbackHealth.currentAdapterHoldEpisodeAgeUs());
            if (g_netRollbackNextAdapterHoldLogUs < 250000u)
                g_netRollbackNextAdapterHoldLogUs = 250000u;
            else if (g_netRollbackNextAdapterHoldLogUs < 500000u)
                g_netRollbackNextAdapterHoldLogUs = 500000u;
            else if (g_netRollbackNextAdapterHoldLogUs < 1000000u)
                g_netRollbackNextAdapterHoldLogUs = 1000000u;
            else if (g_netRollbackNextAdapterHoldLogUs < 2000000u)
                g_netRollbackNextAdapterHoldLogUs = 2000000u;
            else if (g_netRollbackNextAdapterHoldLogUs < 5000000u)
                g_netRollbackNextAdapterHoldLogUs = 5000000u;
            else
                g_netRollbackNextAdapterHoldLogUs += 5000000u;
        }
    }

    if (diag.activeStallReason != qnetplay::StallReason::None) {
        const uint64_t ageUs =
            g_netRollbackHealth.currentStallReasonAgeUs();
        if (ageUs >= g_netRollbackNextStallLogUs) {
            netLogRollbackStallContext(
                "progress", nullptr, diag, stats,
                g_netRollbackHealth.currentStallEpisodeAgeUs());
            if (g_netRollbackNextStallLogUs < 250000u)
                g_netRollbackNextStallLogUs = 250000u;
            else if (g_netRollbackNextStallLogUs < 500000u)
                g_netRollbackNextStallLogUs = 500000u;
            else if (g_netRollbackNextStallLogUs < 1000000u)
                g_netRollbackNextStallLogUs = 1000000u;
            else if (g_netRollbackNextStallLogUs < 2000000u)
                g_netRollbackNextStallLogUs = 2000000u;
            else if (g_netRollbackNextStallLogUs < 5000000u)
                g_netRollbackNextStallLogUs = 5000000u;
            else
                g_netRollbackNextStallLogUs += 5000000u;
        }
    }

    if (!observed.reportDue && !forceReport)
        return;
    const qgame::RollbackHealthReport report =
        g_netRollbackHealth.finishReport();
    qgame::NetTransportDiagnostics link{};
    (void)qgame::netGetTransportDiagnostics(link);
    const auto& stallWall = report.stalledWallUsByReason;
    const auto& adapterWall = report.adapterHoldWallUsByReason;
    QLOG("[rollback-health-v1] source=%s elapsedUs=%llu frame=%u "
         "newForward=%llu simFps=%.2f speedPermille=%u "
         "confirmed=%llu confirmedFps=%.2f confirmedLag=%u/%u "
         "replayFrames=%llu rollbacks=%llu rollbackFrames=%llu "
         "runnableUs=%llu blockedUs=%llu schedulerNonstallUs=%llu "
         "schedulerStallUs=%llu adapterHoldUs=%llu "
         "stallWallUs=[local:%llu "
         "prediction:%llu replay:%llu invalid:%llu] "
         "adapterWallUs=[test:%llu delay:%llu event:%llu refresh:%llu "
         "exact:%llu presentation:%llu rbSetup:%llu layout:%llu "
         "correction:%llu frameAdvantage:%llu] "
         "currentStall=%s currentAdapterHold=%s noProgressUs=%llu "
         "predDepth=%u nextRemote=%u sessionNewest=%u/%u hole=%u/%u "
         "pendingRb=%u/%u replayState=%u/%u/%u/%u queues=%zu/%zu "
         "delay=%u maxRb=%u "
         "coreRemote=%u/%u newest=%u/%u reorderSpan=%u "
         "inputAgeMs=%llu validRxAgeMs=%llu peerAck=%u/%u "
         "peerFrontier=%u/%u/%llu ingress=%u/%u "
         "lastPresent=%u last=%s(%u)/%llu "
         "pingUs=%u/%u jitterUs=%u lossPermille=%u "
         "adapterDrops=%u/%u socketErrors=%u/%u/%d "
         "gameThread=%s/%u/%llu "
         "clockFaults=%llu/%llu statsFaults=%llu/%llu "
         "scene=%u matchSub=%d koRes=%u roundCounter=%u exactWindow=%u "
         "appliedDelay=%u lifecycleHolds=%u",
         source ? source : "unknown",
         static_cast<unsigned long long>(report.elapsedUs),
         report.diagnostics.frontier,
         static_cast<unsigned long long>(report.newForwardFrames),
         report.newForwardFps(),
         report.newForwardSpeedPermille,
         static_cast<unsigned long long>(report.confirmedFrames),
         report.confirmedFps(),
         report.confirmedLagFrames,
         report.maximumConfirmedLagFrames,
         static_cast<unsigned long long>(report.replayedFrames),
         static_cast<unsigned long long>(report.rollbackCount),
         static_cast<unsigned long long>(report.rollbackFrames),
         static_cast<unsigned long long>(report.runnableWallUs),
         static_cast<unsigned long long>(report.blockedWallUs),
         static_cast<unsigned long long>(report.activeWallUs),
         static_cast<unsigned long long>(report.stalledWallUs),
         static_cast<unsigned long long>(report.adapterHoldWallUs),
         static_cast<unsigned long long>(
             stallWall[static_cast<size_t>(
                 qnetplay::StallReason::LocalInputMissing)]),
         static_cast<unsigned long long>(
             stallWall[static_cast<size_t>(
                 qnetplay::StallReason::PredictionLimit)]),
         static_cast<unsigned long long>(
             stallWall[static_cast<size_t>(
                 qnetplay::StallReason::ReplayInProgress)]),
         static_cast<unsigned long long>(
             stallWall[static_cast<size_t>(
                 qnetplay::StallReason::InvalidSession)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::TestBarrier)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::DelayDrain)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::SessionEvent)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::LayoutRefresh)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::ExactInput)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::PresentationGate)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::RollbackSetup)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::LayoutBarrier)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::CorrectionWait)]),
         static_cast<unsigned long long>(
             adapterWall[static_cast<size_t>(
                 qgame::AdapterHoldReason::FrameAdvantage)]),
         qnetplay::stallReasonName(
             report.diagnostics.activeStallReason),
         qgame::adapterHoldReasonName(
             report.adapterHoldReason),
         static_cast<unsigned long long>(report.noProgressAgeUs),
         report.diagnostics.speculativeFrames,
         report.diagnostics.nextRemoteExpected,
         report.diagnostics.hasRemoteNewestActual ? 1u : 0u,
         report.diagnostics.remoteNewestActual,
         report.diagnostics.hasRemoteHole ? 1u : 0u,
         report.diagnostics.remoteHoleSpan,
         report.diagnostics.pendingRollback ? 1u : 0u,
         report.diagnostics.pendingRollbackFrom,
         report.diagnostics.replayInProgress ? 1u : 0u,
         report.diagnostics.replayFrom,
         report.diagnostics.nextReplayFrame,
         report.diagnostics.replayUntilExclusive,
         report.diagnostics.pendingLocalSubmissions,
         report.diagnostics.pendingConfirmedFrames,
         static_cast<unsigned>(
             report.diagnostics.inputDelayFrames),
         static_cast<unsigned>(
             report.diagnostics.maxRollbackFrames),
         link.core.hasRemoteContiguous ? 1u : 0u,
         link.core.remoteContiguousThrough,
         link.core.hasRemoteNewest ? 1u : 0u,
         link.core.remoteNewest, link.core.remoteReorderSpan,
         static_cast<unsigned long long>(
             link.core.lastNewRemoteInputAgeMs),
         static_cast<unsigned long long>(
             link.core.lastValidReceiveAgeMs),
         link.core.hasPeerAck ? 1u : 0u,
         link.core.peerAckThrough,
         link.core.hasPeerSimFrontier ? 1u : 0u,
         link.core.peerSimFrontier,
         static_cast<unsigned long long>(
             link.core.peerFrontierAgeMs),
         link.ingressAccepted, link.ingressRejected,
         link.hasLastIngress ? 1u : 0u,
         qnetplay::ingressResultName(link.lastIngress),
         static_cast<unsigned>(link.lastIngress),
         static_cast<unsigned long long>(link.lastIngressAgeMs),
         link.hasPing ? link.ping.meanMicros : 0u,
         link.hasPing ? link.ping.p95Micros : 0u,
         link.hasPing ? link.ping.jitterMicros : 0u,
         link.hasPing ? link.ping.lossPerMille : 0u,
         link.crossDomainDrops, link.wrongSourceDrops,
         link.receiveErrors, link.sendFailures,
         link.lastSocketError,
         qgame::netGameThreadStageName(link.gameThreadStage),
         link.gameThreadFrame,
         static_cast<unsigned long long>(
             link.gameThreadStageAgeMs),
         static_cast<unsigned long long>(
             report.clockRegressionFaults),
         static_cast<unsigned long long>(
             report.totalClockRegressionFaults),
         static_cast<unsigned long long>(
             report.statsRegressionFaults),
         static_cast<unsigned long long>(
             report.totalStatsRegressionFaults),
         rd32(kSceneStateAddr),
         static_cast<int32_t>(rd32(kMatchSubAddr)),
         // dword_88098C is the native outro sequencer stage: 0 combat, 1..8 outro, 10/11/20/101 variants.
         rd32(kRoundResultStateAddr),
         rd32(addr::kRoundCounter),
         netLifecycleTransitionRequiresExactInput() ? 1u : 0u,
         static_cast<unsigned>(report.diagnostics.inputDelayFrames),
         g_netPacingLifecycleHolds);

    {
        const uint64_t win = report.elapsedUs ? report.elapsedUs : 1u;
        const auto share = [win](const uint64_t us) -> unsigned {
            return static_cast<unsigned>((us * 1000ull) / win);
        };
        const uint64_t usExact = adapterWall[static_cast<size_t>(
            qgame::AdapterHoldReason::ExactInput)];
        const uint64_t usFrameAdv = adapterWall[static_cast<size_t>(
            qgame::AdapterHoldReason::FrameAdvantage)];
        const uint64_t usCorrection =
            adapterWall[static_cast<size_t>(
                qgame::AdapterHoldReason::CorrectionWait)]
            + report.stalledWallUsByReason[static_cast<size_t>(
                  qnetplay::StallReason::ReplayInProgress)];
        const uint64_t usPrediction =
            report.stalledWallUsByReason[static_cast<size_t>(
                qnetplay::StallReason::PredictionLimit)];
        const uint64_t usSetup =
            adapterWall[static_cast<size_t>(
                qgame::AdapterHoldReason::RollbackSetup)]
            + adapterWall[static_cast<size_t>(
                  qgame::AdapterHoldReason::LayoutBarrier)]
            + adapterWall[static_cast<size_t>(
                  qgame::AdapterHoldReason::LayoutRefresh)]
            + adapterWall[static_cast<size_t>(
                  qgame::AdapterHoldReason::DelayDrain)]
            + adapterWall[static_cast<size_t>(
                  qgame::AdapterHoldReason::SessionEvent)]
            + adapterWall[static_cast<size_t>(
                  qgame::AdapterHoldReason::PresentationGate)]
            + adapterWall[static_cast<size_t>(
                  qgame::AdapterHoldReason::TestBarrier)];
        const uint64_t modelled =
            report.runnableWallUs + usExact + usFrameAdv + usCorrection
            + usPrediction + usSetup;
        const uint64_t unaccounted =
            win > modelled ? win - modelled : 0u;
        const uint32_t want = static_cast<uint32_t>(
            (win * 6060ull) / 100000000ull);
        QLOG("[cadence] presented=%u starvedOpps=%u "
             "windowUs=%llu ran=%llu want=%u speedPermille=%u "
             "| runUs=%llu(%u) exactInput=%llu(%u) prediction=%llu(%u) "
             "frameAdv=%llu(%u) correction=%llu(%u) setup=%llu(%u) "
             "unaccounted=%llu(%u) "
             "| holds=[lifecycle:%u frameAdv:%u exactWait:%u] "
             "rollbacks=%llu replayFrames=%llu catchup=%llu hiddenFwd=%llu "
             "budgetStops=%u "
             "debt=%u scene=%u matchSub=%d koStage=%u(%s) exactWindow=%u "
             "delay=%u maxRb=%u pingUs=%u",
             g_netPresentedThisWindow, g_netStarvedOpportunities,
             static_cast<unsigned long long>(report.elapsedUs),
             static_cast<unsigned long long>(report.newForwardFrames), want,
             report.newForwardSpeedPermille,
             static_cast<unsigned long long>(report.runnableWallUs),
             share(report.runnableWallUs),
             static_cast<unsigned long long>(usExact), share(usExact),
             static_cast<unsigned long long>(usPrediction),
             share(usPrediction),
             static_cast<unsigned long long>(usFrameAdv), share(usFrameAdv),
             static_cast<unsigned long long>(usCorrection),
             share(usCorrection),
             static_cast<unsigned long long>(usSetup), share(usSetup),
             static_cast<unsigned long long>(unaccounted),
             share(unaccounted),
             g_netPacingLifecycleHolds, g_netPacingFrameAdvantageHolds,
             g_netPacingExactWaits,
             static_cast<unsigned long long>(report.rollbackCount),
             static_cast<unsigned long long>(report.replayedFrames),
             static_cast<unsigned long long>(g_netCatchupBatches),
             static_cast<unsigned long long>(g_hiddenForwardNativeTicks),
             g_netCatchupBudgetStops,
             g_netCadenceDebt.frames(),
             rd32(kSceneStateAddr),
             static_cast<int32_t>(rd32(kMatchSubAddr)),
             rd32(kRoundResultStateAddr),
             qgame::koStageName(rd32(kRoundResultStateAddr)),
             netLifecycleTransitionRequiresExactInput() ? 1u : 0u,
             static_cast<unsigned>(report.diagnostics.inputDelayFrames),
             static_cast<unsigned>(g_netRollback),
             qgame::netPeerHasRtt() ? qgame::netPeerRttMicros() : 0u);

        if (g_netRunning && report.newForwardFrames > 0
            && g_netPresentedThisWindow == 0) {
            QLOG("[presentation-starved] *** %llu canonical frames advanced and "
                 "NOTHING was drawn in %llu us *** starvedOpps=%u debt=%u "
                 "predDepth=%u maxRb=%u - the simulation is running and the "
                 "screen is not. This is a bug in the frame scheduler, not the "
                 "network; the peer is receiving these frames.",
                 static_cast<unsigned long long>(report.newForwardFrames),
                 static_cast<unsigned long long>(report.elapsedUs),
                 g_netStarvedOpportunities, g_netCadenceDebt.frames(),
                 g_netSession
                     ? qnet::forwardDistance(
                           g_netSession->diagnostics().nextRemoteExpected,
                           g_netSession->currentFrame())
                     : 0u,
                 static_cast<unsigned>(g_netRollback));
        }
        g_netPresentedThisWindow = 0;

        const uint32_t hiddenP95 = g_netHiddenTickUs.percentile(95);
        const uint32_t loadP95 = g_netLoadWorkUs.percentile(95);
        QLOG("[work-budget] saveUs=[p50:%u p95:%u p99:%u n=%zu] "
             "loadUs=[p50:%u p95:%u p99:%u n=%zu] "
             "hiddenTickUs=[p50:%u p95:%u p99:%u n=%zu] "
             "frameIntervalUs=16500 maxDepthAtP95=%u",
             g_netSaveWorkUs.percentile(50), g_netSaveWorkUs.percentile(95),
             g_netSaveWorkUs.percentile(99), g_netSaveWorkUs.count(),
             g_netLoadWorkUs.percentile(50), loadP95,
             g_netLoadWorkUs.percentile(99), g_netLoadWorkUs.count(),
             g_netHiddenTickUs.percentile(50), hiddenP95,
             g_netHiddenTickUs.percentile(99), g_netHiddenTickUs.count(),
             hiddenP95 == 0u ? 0u
                             : static_cast<unsigned>(
                                   (16500u > loadP95 ? 16500u - loadP95 : 0u)
                                   / hiddenP95));

        {
            const uint32_t depthP95 = g_netPredictionDepth.percentile(95);
            const uint32_t ceiling = static_cast<uint32_t>(g_netRollback);
            QLOG("[coverage-measured] depthFrames=[p50:%u p95:%u p99:%u "
                 "n=%zu] ceiling=%u saturated=%u | estimateForRoute=%u "
                 "peerDelay=%u ourRollback=%u budget=%u",
                 g_netPredictionDepth.percentile(50), depthP95,
                 g_netPredictionDepth.percentile(99),
                 g_netPredictionDepth.count(), ceiling,
                 (ceiling != 0u && depthP95 >= ceiling) ? 1u : 0u,
                 netRouteRequiredCoverageFrames(),
                 g_netRemoteDelay, ceiling,
                 g_netRemoteDelay + ceiling);
        }

        QLOG("[input-production] starvedOpportunities=%llu/%llu (%u permille) "
             "currentSpan=%u maxSpan=%u producedThrough=%u frontier=%u "
             "delay=%u",
             static_cast<unsigned long long>(
                 g_netProducerStarveOpportunities),
             static_cast<unsigned long long>(g_netProducerOpportunities),
             g_netProducerOpportunities
                 ? static_cast<unsigned>(
                       (g_netProducerStarveOpportunities * 1000ull)
                       / g_netProducerOpportunities)
                 : 0u,
             g_netProducerStarveSpan, g_netProducerStarveMaxSpan,
             g_netLastProducedThrough,
             report.diagnostics.frontier,
             static_cast<unsigned>(report.diagnostics.inputDelayFrames));
        QLOG("[input-production] producer=%s aheadFrames=%u maxLead=%u "
             "budget=%u fresh=%u held=%u (produced while the simulation could "
             "not advance; without this a stall starves the peer and the pair "
             "locks below full speed. fresh = 6.2 device-worker samples, so "
             "input pressed during the stall survived; held = the safe "
             "fallback that repeats the last captured word)",
             netInputProducerDisabled() ? "off" : "on",
             g_netProducerAheadFrames, g_netProducerAheadMaxLead,
             netProducerMaxLeadFrames(),
             g_netProducerFreshSamples, g_netProducerHeldRepeats);

        QLOG("[run-state] now=%s transitions=%u gaps=%u residencyUs=["
             "running:%llu pressure:%llu emergency:%llu correction:%llu "
             "recovery:%llu barrier:%llu suspended:%llu]",
             qgame::runStateName(g_netRunState), g_netRunStateTransitions,
             g_netRunStateDiscontinuities,
             static_cast<unsigned long long>(g_netRunStateUs[
                 static_cast<size_t>(qgame::NetplayRunState::Running)]),
             static_cast<unsigned long long>(g_netRunStateUs[
                 static_cast<size_t>(qgame::NetplayRunState::PredictionPressure)]),
             static_cast<unsigned long long>(g_netRunStateUs[
                 static_cast<size_t>(qgame::NetplayRunState::EmergencyNetworkHold)]),
             static_cast<unsigned long long>(g_netRunStateUs[
                 static_cast<size_t>(qgame::NetplayRunState::CorrectionCatchUp)]),
             static_cast<unsigned long long>(g_netRunStateUs[
                 static_cast<size_t>(qgame::NetplayRunState::RecoveryCatchUp)]),
             static_cast<unsigned long long>(g_netRunStateUs[
                 static_cast<size_t>(qgame::NetplayRunState::ExactLifecycleBarrier)]),
             static_cast<unsigned long long>(g_netRunStateUs[
                 static_cast<size_t>(qgame::NetplayRunState::ExternalSuspension)]));
        QLOG("[hold-episode] episodes=%u active=%u totalHeldUs=%llu "
             "heldSamples=%llu worstUs=%llu worstFirstMissing=%u "
             "worstRecoveryDepth=%u deepestRecovery=%u",
             g_netHoldLedger.episodes(), g_netHoldLedger.active() ? 1u : 0u,
             static_cast<unsigned long long>(g_netHoldLedger.totalHeldUs()),
             static_cast<unsigned long long>(
                 g_netHoldLedger.heldOpportunities()),
             static_cast<unsigned long long>(
                 g_netHoldLedger.worst().durationUs()),
             g_netHoldLedger.worst().firstMissingFrame,
             g_netHoldLedger.worst().recoveryDepth,
             g_netHoldLedger.deepestRecovery());

        qgame::NetPeerPressure peer{};
        if (qgame::netPeerPressure(peer)) {
            const uint32_t ourPrefix = report.diagnostics.nextRemoteExpected;
            const uint32_t inFlight =
                (peer.inputProducedThrough != 0xFFFFFFFFu
                 && qnet::frameAfter(peer.inputProducedThrough, ourPrefix))
                    ? qnet::forwardDistance(ourPrefix,
                                            peer.inputProducedThrough)
                    : 0u;
            const int32_t balanceDelta =
                static_cast<int32_t>(report.diagnostics.speculativeFrames)
                - static_cast<int32_t>(peer.predictionDepth);
            QLOG("[peer-pressure] state=%u producedThrough=%u ourPrefix=%u "
                 "inFlight=%u localDepth=%u peerDepth=%u balanceDelta=%d "
                 "slewPpm=%d debt=%u confirmed=%u holdGen=%u ageMs=%llu",
                 static_cast<unsigned>(peer.runState),
                 peer.inputProducedThrough, ourPrefix, inFlight,
                 report.diagnostics.speculativeFrames,
                 static_cast<unsigned>(peer.predictionDepth),
                 balanceDelta, g_netPaceSlewPpm,
                 static_cast<unsigned>(peer.correctionDebt),
                 peer.confirmedFrontier, peer.holdGeneration,
                 static_cast<unsigned long long>(peer.ageMs));
        }
    }
}

void netResetFramePacingTelemetry() {
    g_netPacingWindow.reset();
    g_netPresentationPacingWindow.reset();
    g_netPacingStatsBaseline =
        g_netSession ? g_netSession->stats()
                     : qnetplay::RollbackStats{};
    g_netPacingCorrectionWorkUs = 0;
    g_netPacingCorrectionBatches = 0;
    g_netPacingDriverCalls = 0;
    g_netPacingSessionStalls = 0;
    g_netPacingExactWaits = 0;
    g_netPacingDeadlineWaits = 0;
    g_netPacingSpacingWaits = 0;
    g_netPacingDeadlineRebases = 0;
}

void netResetPacingTelemetry() {
    netResetFramePacingTelemetry();
    g_netRollbackHealth.reset();
    g_netRollbackNextStallLogUs = 100000u;
    g_netAdapterHoldReason = qgame::AdapterHoldReason::None;
    g_netRollbackNextAdapterHoldLogUs = 100000u;
}

void netObservePacing(
    const uint32_t frame, const bool gameplay,
    const uint32_t nativeCounterBefore,
    const uint32_t nativeCounterAfter,
    const uint32_t arenaSelectorBefore,
    const uint32_t arenaSelectorAfter) {
    const uint64_t nowUs = netRealQpcUs();
    // dword_4AB778 is QOH's completed-simulation-step counter; it resets when sub_4294F0 publishes the one-second FPS sample.
    const bool nativeAdvanced = qgame::nativeSimulationAdvanced(
        gameplay, nativeCounterBefore, nativeCounterAfter,
        arenaSelectorBefore, arenaSelectorAfter);
    g_netPacingWindow.observe(nowUs, gameplay, nativeAdvanced);
    if (!g_netPacingWindow.reportDue(nowUs))
        return;

    const qgame::NetplayPacingReport report =
        g_netPacingWindow.finish(nowUs);
    const qnetplay::RollbackStats stats =
        g_netSession ? g_netSession->stats()
                     : qnetplay::RollbackStats{};
    const auto delta64 = [](const uint64_t now, const uint64_t before) {
        return now >= before ? now - before : now;
    };
    const uint64_t rollbackDelta =
        delta64(stats.rollbackCount,
                g_netPacingStatsBaseline.rollbackCount);
    const uint64_t replayDelta =
        delta64(stats.replayedFrames,
                g_netPacingStatsBaseline.replayedFrames);
    g_netPacingStatsBaseline = stats;

    const double nativeFps = report.elapsedUs == 0
        ? 0.0
        : static_cast<double>(report.nativeAdvanceTicks)
            * 1000000.0 / static_cast<double>(report.elapsedUs);
    const uintptr_t p1 = netActiveFighterAddress(0);
    const uintptr_t p2 = netActiveFighterAddress(1);
    uint32_t schedPredDepth = 0;
    int32_t schedPeerAdvRaw = 0;
    int32_t schedPeerAdvEst = 0;
    unsigned schedPeerFresh = 0;
    if (g_netSession) {
        const qnetplay::RollbackDiagnostics diagnostics =
            g_netSession->diagnostics();
        const uint32_t frontier = diagnostics.frontier;
        const uint32_t prefix = diagnostics.nextRemoteExpected;
        if (qnet::frameAfter(frontier, prefix))
            schedPredDepth = frontier - prefix;
        if (qgame::netPeerHasSimFrontier()) {
            const uint32_t reported = qgame::netPeerSimFrontier();
            schedPeerAdvRaw = qnet::signedLead(frontier, reported);
            const uint64_t ageMs = qgame::netPeerFrontierAgeMs();
            const uint32_t rttMs =
                qgame::netPeerHasRtt() ? qgame::netPeerRttMs() : 0u;
            const uint32_t staleFrames = static_cast<uint32_t>(
                ((ageMs + rttMs / 2u) * 6060ull + 50000ull) / 100000ull);
            schedPeerAdvEst =
                qnet::signedLead(frontier, reported + staleFrames);
            schedPeerFresh = ageMs <= kNetPeerFrontierFreshMs ? 1u : 0u;
        }
    }
    QLOG("[pacing-world] frame=%u elapsedUs=%llu visible=%u "
         "visibleFps=%.2f gameplay=%u nativeAdvance=%u nativeMiss=%u "
         "nativeFps=%.2f qohFps=%u qohCounter=%u "
         "intervalUs=%u/%u/%u burst=%u late=%u "
         "driverCalls=%u stalls=%u exactWaits=%u deadlineWaits=%u "
         "spacingWaits=%u deadlineRebases=%u paceDebtUs=%llu "
         "rollbacks=%llu replayFrames=%llu correctionBatches=%u "
         "correctionWorkUs=%llu scene=%u matchSub=%d result=%u round=%u "
         "finishTimers=%d/%d "
         "P1=[x:%d y:%d action:%u anim:%u/%u left:%u life:%d] "
         "P2=[x:%d y:%d action:%u anim:%u/%u left:%u life:%d] "
         "predDepth=%u maxRb=%u debt=%u hiddenFwd=%llu catchupBatches=%llu "
         "peerAdvRaw=%d peerAdvEst=%d peerFresh=%u faHolds=%u",
         frame, static_cast<unsigned long long>(report.elapsedUs),
         report.visibleTicks, report.visibleFps(),
         report.gameplayTicks, report.nativeAdvanceTicks,
         report.nativeAdvanceMisses, nativeFps,
         rd32(0x4AA44Cu), rd32(0x4AB778u),
         report.minimumIntervalUs, report.p95IntervalUs,
         report.maximumIntervalUs, report.burstIntervals,
         report.lateIntervals, g_netPacingDriverCalls,
         g_netPacingSessionStalls, g_netPacingExactWaits,
         g_netPacingDeadlineWaits, g_netPacingSpacingWaits,
         g_netPacingDeadlineRebases,
         static_cast<unsigned long long>(netPaceDebtUs()),
         static_cast<unsigned long long>(rollbackDelta),
         static_cast<unsigned long long>(replayDelta),
         g_netPacingCorrectionBatches,
         static_cast<unsigned long long>(g_netPacingCorrectionWorkUs),
         rd32(kSceneStateAddr),
         static_cast<int32_t>(rd32(kMatchSubAddr)),
         rd32(kRoundResultStateAddr), rd32(addr::kRoundCounter),
         static_cast<int32_t>(rd32(0x4AA3D0u)),
         static_cast<int32_t>(rd32(0x4AA408u)),
         static_cast<int32_t>(rd32(p1 + 136u)),
         static_cast<int32_t>(rd32(p1 + 140u)),
         rd32(p1 + 88u), rd32(p1 + 48u), rd32(p1 + 52u),
         rd32(p1 + 56u), netBattleLifeEffective(0),
         static_cast<int32_t>(rd32(p2 + 136u)),
         static_cast<int32_t>(rd32(p2 + 140u)),
         rd32(p2 + 88u), rd32(p2 + 48u), rd32(p2 + 52u),
         rd32(p2 + 56u), netBattleLifeEffective(1),
         schedPredDepth, static_cast<unsigned>(g_netRollback),
         g_netCadenceDebt.frames(),
         static_cast<unsigned long long>(g_hiddenForwardNativeTicks),
         static_cast<unsigned long long>(g_netCatchupBatches),
         schedPeerAdvRaw, schedPeerAdvEst, schedPeerFresh,
         g_netPacingFrameAdvantageHolds);

    g_netPacingCorrectionWorkUs = 0;
    g_netPacingCorrectionBatches = 0;
    g_netPacingDriverCalls = 0;
    g_netPacingSessionStalls = 0;
    g_netPacingExactWaits = 0;
    g_netPacingDeadlineWaits = 0;
    g_netPacingSpacingWaits = 0;
    g_netPacingDeadlineRebases = 0;
}

inline uint16_t packLogical(int logical) {
    return qinput::compactFromLogical(
        static_cast<uint32_t>(logical) & qinput::kLogicalFightMask);
}
inline int unpackLogical(uint16_t wire) {
    return static_cast<int>(qinput::logicalFromCompact(
        wire & qinput::kCanonicalFightMask));
}

struct NetCharacterSelectTraceState {
    bool active = false;
    uint16_t canonicalP1 = 0;
    uint16_t canonicalP2 = 0;
    uint32_t cursorP1 = 0;
    uint32_t cursorP2 = 0;
    uint32_t confirmP1 = 0;
    uint32_t confirmP2 = 0;
};
static NetCharacterSelectTraceState g_netCharacterSelectTrace{};

void netResetCharacterSelectTrace() {
    g_netCharacterSelectTrace = NetCharacterSelectTraceState{};
}

void netTraceProductionCharacterSelect(
    const char* stage,
    const uint32_t frame,
    const qnetplay::CanonicalInputs& inputs) {
    if (g_netAutomatchTest) {
        netResetCharacterSelectTrace();
        return;
    }

    const uint32_t scene = rd32(kSceneStateAddr);
    const int32_t substate =
        static_cast<int32_t>(rd32(addr::kScene3Substate));
    const bool active = scene == 3u && substate == 2;
    if (!active) {
        if (g_netCharacterSelectTrace.active) {
            QLOG("[charselect-input] EXIT stage=%s frame=%u scene=%u "
                 "scene3Sub=%d",
                 stage ? stage : "unknown", frame, scene, substate);
        }
        netResetCharacterSelectTrace();
        return;
    }

    const uint32_t cursorP1 = rd32(addr::kP1CharCursor);
    const uint32_t cursorP2 = rd32(addr::kP2CharCursor);
    const uint32_t confirmP1 = rd32(addr::kCharSelConfirmP1);
    const uint32_t confirmP2 = rd32(addr::kCharSelConfirmP2);
    uint32_t changes = 0;
    if (!g_netCharacterSelectTrace.active)
        changes |= 0x1u;
    if (inputs.p1 != g_netCharacterSelectTrace.canonicalP1
        || inputs.p2 != g_netCharacterSelectTrace.canonicalP2) {
        changes |= 0x2u;
    }
    if (cursorP1 != g_netCharacterSelectTrace.cursorP1
        || cursorP2 != g_netCharacterSelectTrace.cursorP2) {
        changes |= 0x4u;
    }
    if (confirmP1 != g_netCharacterSelectTrace.confirmP1
        || confirmP2 != g_netCharacterSelectTrace.confirmP2) {
        changes |= 0x8u;
    }

    if (changes != 0u) {
        QLOG("[charselect-input] EDGE stage=%s frame=%u changes=%X "
             "canonicalLogical=[%08X,%08X] wire=[%04X,%04X] "
             "remotePredicted=%u cursor=[%u,%u] confirm=[%u,%u]",
             stage ? stage : "unknown", frame, changes,
             static_cast<unsigned>(unpackLogical(inputs.p1)),
             static_cast<unsigned>(unpackLogical(inputs.p2)),
             static_cast<unsigned>(inputs.p1),
             static_cast<unsigned>(inputs.p2),
             inputs.remotePredicted ? 1u : 0u,
             cursorP1, cursorP2, confirmP1, confirmP2);
    }

    g_netCharacterSelectTrace.active = true;
    g_netCharacterSelectTrace.canonicalP1 = inputs.p1;
    g_netCharacterSelectTrace.canonicalP2 = inputs.p2;
    g_netCharacterSelectTrace.cursorP1 = cursorP1;
    g_netCharacterSelectTrace.cursorP2 = cursorP2;
    g_netCharacterSelectTrace.confirmP1 = confirmP1;
    g_netCharacterSelectTrace.confirmP2 = confirmP2;
}

void netConfigureAutomatchTest() {
    InterlockedExchange(&g_netTestPauseRequest, 0);
    InterlockedExchange(&g_netTestPauseAcknowledged, 0);
    InterlockedExchange(&g_netTestPauseFrame, 0);
    InterlockedExchange(&g_netTestPauseHeldFrame, 0);
    char f1Frame[32] = {};
    const DWORD f1FrameLength = GetEnvironmentVariableA(
        "QOH99_NP_TEST_F1_RETURN_FRAME", f1Frame, sizeof(f1Frame));
    g_netTestF1ReturnFrame =
        (f1FrameLength != 0 && f1FrameLength < sizeof(f1Frame))
            ? static_cast<uint32_t>(std::strtoul(f1Frame, nullptr, 10))
            : 0u;
    if (g_netTestF1ReturnFrame != 0u) {
        QLOG("[test-automatch] F1 return armed at canonical frame %u",
             g_netTestF1ReturnFrame);
    }
    char optIn[64] = {};
    const DWORD optInLength = GetEnvironmentVariableA(
        "QOH99_NP_TEST_AUTOMATCH", optIn, sizeof(optIn));
    const bool nativeRematch =
        optInLength != 0
        && optInLength < sizeof(optIn)
        && std::strcmp(optIn, "live-e2e:native-rematch") == 0;
    const bool spectatorTransition =
        optInLength != 0
        && optInLength < sizeof(optIn)
        && std::strcmp(
               optIn, "live-e2e:spectator-confirmed-transition") == 0;
    g_netAutomatchHarnessEnabled =
        nativeRematch || spectatorTransition;
    g_netAutomatchTest = g_netAutomatchHarnessEnabled;
    g_netAutomatchFrontendOnly = spectatorTransition;
    char pauseOptIn[64] = {};
    const DWORD pauseOptInLength = GetEnvironmentVariableA(
        "QOH99_NP_TEST_PAUSE", pauseOptIn, sizeof(pauseOptIn));
    g_netTestPauseBarrierEnabled =
        pauseOptInLength != 0
        && pauseOptInLength < sizeof(pauseOptIn)
        && std::strcmp(pauseOptIn, "live-e2e:frame-barrier") == 0;
    g_netAutomatchLowHpAfter = 120;
    g_netAutomatchForceKoAfter = 360;
    g_netAutomatchForceKoEnabled = true;
    g_netAutomatchWasGameplay = false;
    g_netAutomatchSegment = 0;
    g_netAutomatchSegmentStart = 0;
    g_netAutomatchRoundSeenMask = 0;
    g_netAutomatchLowLifeLoggedMask = 0;
    g_netAutomatchNativeKoLoggedMask = 0;
    g_netAutomatchNativeLatchLoggedMask = 0;
    g_netAutomatchForcedKoLoggedMask = 0;
    g_netTestDelayScriptEnabled = false;
    g_netTestDelayStepCount = 0;
    g_netTestDelayNextStep = 0;
    g_netTestDelayPendingTarget = UINT32_MAX;
    g_netAutomatchInputLogs = 0;
    g_netAutomatchCombatInputLogs = 0;
    g_netAutomatchFrontendNonzeroLogged = false;
    g_netAutomatchCombatNonzeroLogged = false;
    g_netAutomatchP1Character = -1;
    g_netAutomatchP2Character = -1;
    g_netAutomatchP1Alternate = 0;
    g_netAutomatchP2Alternate = 0;
    g_netAutomatchForcedCharacterSceneActive = false;
    g_netRematchAutoAnswer = 0;
    if (!g_netAutomatchTest) return;

    if (!g_netAutomatchFrontendOnly) {
        g_netRematchAutoAnswer = 1;
        char answerText[16] = {};
        const DWORD answerLength = GetEnvironmentVariableA(
            "QOH99_NP_TEST_REMATCH_ANSWER", answerText,
            sizeof(answerText));
        if (answerLength != 0 && answerLength < sizeof(answerText)) {
            if (std::strcmp(answerText, "leave") == 0)
                g_netRematchAutoAnswer = 2;
            else if (std::strcmp(answerText, "none") == 0)
                g_netRematchAutoAnswer = 0;
        }
    }

    char characterText[32] = {};
    const DWORD characterLength = GetEnvironmentVariableA(
        "QOH99_NP_TEST_CHARACTERS",
        characterText, sizeof(characterText));
    if (characterLength != 0 && characterLength < sizeof(characterText)) {
        char* comma = std::strchr(characterText, ',');
        if (comma != nullptr) {
            *comma = '\0';
            long alternate[2] = {0, 0};
            char* const side[2] = { characterText, comma + 1 };
            bool valid = true;
            long cursor[2] = {0, 0};
            for (size_t i = 0; i < 2u && valid; ++i) {
                char* end = nullptr;
                cursor[i] = std::strtol(side[i], &end, 10);
                if (end == side[i] || cursor[i] < 0 || cursor[i] > 23) {
                    valid = false;
                    break;
                }
                if (*end == ':') {
                    char* altEnd = nullptr;
                    alternate[i] = std::strtol(end + 1, &altEnd, 10);
                    valid = altEnd != end + 1 && *altEnd == '\0'
                        && (alternate[i] == 0 || alternate[i] == 1);
                } else {
                    valid = *end == '\0';
                }
            }
            if (valid) {
                g_netAutomatchP1Character = static_cast<int32_t>(cursor[0]);
                g_netAutomatchP2Character = static_cast<int32_t>(cursor[1]);
                g_netAutomatchP1Alternate =
                    static_cast<int32_t>(alternate[0]);
                g_netAutomatchP2Alternate =
                    static_cast<int32_t>(alternate[1]);
            }
        }
    }

    char thresholdText[32] = {};
    const DWORD thresholdLength = GetEnvironmentVariableA(
        "QOH99_NP_TEST_FORCE_KO_FRAMES",
        thresholdText, sizeof(thresholdText));
    if (thresholdLength != 0 && thresholdLength < sizeof(thresholdText)) {
        char* end = nullptr;
        const unsigned long parsed =
            std::strtoul(thresholdText, &end, 10);
        if (end != thresholdText && *end == '\0') {
            if (parsed == 0ul) {
                g_netAutomatchForceKoAfter = 0u;
                g_netAutomatchForceKoEnabled = false;
            } else {
                g_netAutomatchForceKoAfter = static_cast<uint32_t>(
                    (std::max)(120ul, (std::min)(parsed, 3600ul)));
            }
        }
    }
    g_netAutomatchLowHpAfter =
        g_netAutomatchForceKoEnabled
            ? (std::min)(120u, g_netAutomatchForceKoAfter / 3u)
            : 0u;
    QLOG("[test-automatch] ENABLED mode=%s local=P%d lowHpAfter=%u "
         "forceKoAfter=%u characterCursors=[%d:%d,%d:%d] "
         "(isolated live-E2E only)",
         g_netAutomatchFrontendOnly
             ? "spectator-confirmed-transition"
             : "native-rematch",
         g_netLocalCtrl + 1, g_netAutomatchLowHpAfter,
         g_netAutomatchForceKoAfter,
         g_netAutomatchP1Character, g_netAutomatchP1Alternate,
         g_netAutomatchP2Character, g_netAutomatchP2Alternate);
}

void netConfigureTestDelayScript() {
    g_netTestDelaySteps = {};
    g_netTestDelayStepCount = 0;
    g_netTestDelayNextStep = 0;
    g_netTestDelayGameplayTicks = 0;
    g_netTestDelayLastFrame = UINT32_MAX;
    g_netTestDelayPendingTarget = UINT32_MAX;
    g_netTestDelayScriptEnabled = false;
    if (!g_netAutomatchTest) return;

    char text[128] = {};
    const DWORD length = GetEnvironmentVariableA(
        "QOH99_NP_TEST_DELAY_SCRIPT", text, sizeof(text));
    if (length == 0 || length >= sizeof(text)) return;

    std::array<NetTestDelayStep, 8> parsed{};
    size_t count = 0;
    uint32_t previousTick = 0;
    char* cursor = text;
    bool valid = true;
    while (*cursor && count < parsed.size()) {
        char* tickEnd = nullptr;
        const unsigned long tick = std::strtoul(cursor, &tickEnd, 10);
        if (tickEnd == cursor || *tickEnd != ':' || tick == 0
            || tick > UINT32_MAX || tick <= previousTick) {
            valid = false;
            break;
        }
        char* delayEnd = nullptr;
        const unsigned long delay =
            std::strtoul(tickEnd + 1, &delayEnd, 10);
        if (delayEnd == tickEnd + 1 || delay > 15
            || (*delayEnd != '\0' && *delayEnd != ',')) {
            valid = false;
            break;
        }
        parsed[count++] = NetTestDelayStep{
            static_cast<uint32_t>(tick),
            static_cast<uint8_t>(delay),
        };
        previousTick = static_cast<uint32_t>(tick);
        if (*delayEnd == '\0') {
            cursor = delayEnd;
            break;
        }
        cursor = delayEnd + 1;
    }
    if (*cursor != '\0' || count == 0 || count > parsed.size())
        valid = false;
    if (!valid) {
        QLOG("[test-delay] invalid script '%s'; expected "
             "strict tick:delay pairs with delay 0..15", text);
        return;
    }

    g_netTestDelaySteps = parsed;
    g_netTestDelayStepCount = count;
    g_netTestDelayScriptEnabled = true;
    QLOG("[test-delay] ENABLED local=P%d script='%s' steps=%zu "
         "(automatch live-E2E only)",
         g_netLocalCtrl + 1, text, count);
}

int netAutomatchLocalInput(uint32_t frame) {
    if (!g_netAutomatchTest) return 0;
    const int scene = static_cast<int>(rd32(kSceneStateAddr));
    const int matchSub = static_cast<int>(rd32(kMatchSubAddr));
    const bool tap = (frame % 18u) < 3u;
    if (g_netAutomatchFrontendOnly && scene != 3) return 0;

    if (scene == 1) {
        if (g_netLocalCtrl != 0
            || static_cast<int32_t>(rd32(0x4AB8C4u)) != 2) {
            return 0;
        }
        const uint32_t cursor = rd32(0x4AB8D0u);
        return tap
            ? (cursor == 0 ? 0x00000001 : 0x00010000)
            : 0;
    }

    if (scene == 3) {
        const int sub =
            static_cast<int32_t>(rd32(addr::kScene3Substate));
        if (sub == 2) {
            if (g_netAutomatchCharselectVisits > 1u) {
                const uint32_t moves = g_netAutomatchCharselectVisits - 1u;
                const uint32_t sinceEntry =
                    frame - g_netAutomatchCharselectBaseFrame;
                constexpr uint32_t kSettleFrames = 240u;
                if (sinceEntry < kSettleFrames) return 0;
                const uint32_t tapPhase = sinceEntry - kSettleFrames;
                if (tapPhase / 18u < moves)
                    return (tapPhase % 18u) < 3u ? 0x00080000 : 0;
            }
            return tap ? 0x00000001 : 0;
        }
        if ((sub == 1 || sub == 4) && g_netLocalCtrl == 0)
            return tap ? 0x00000001 : 0;
        return 0;
    }

    if (scene == 2 && g_canonRematch.active) {
        if (g_netRematchAutoAnswer == 0) return 0;
        const uint32_t since = frame - g_canonRematch.doorwayFrame;
        constexpr uint32_t kSettleFrames = 12u;
        if (since < kSettleFrames) return 0;
        const uint32_t phase = since - kSettleFrames;
        const bool pressWindow = (phase % 18u) < 3u;
        const bool cursorYes = qnetplay::rematchTheaterCursorIsYes(
            rd32(kRematchTheaterCursorAddr));
        const bool wantDecline = g_netRematchAutoAnswer == 2;
        if (wantDecline && cursorYes) {
            if (g_netLocalCtrl != 0) return 0;
            return pressWindow ? static_cast<int>(qinput::kLogicalLeft) : 0;
        }
        return pressWindow ? static_cast<int>(qinput::kLogicalA) : 0;
    }

    if (scene == 2 && matchSub != 0) {
        if (matchSub == 2) {
            const uint32_t winner = rd32(0x6236A8u);
            if (winner <= 1u
                && static_cast<uint32_t>(g_netLocalCtrl)
                    == (winner ^ 1u)) {
                return tap ? 0x00000001 : 0;
            }
            return 0;
        }
        if (matchSub == 4 && g_netLocalCtrl == 0)
            return tap ? 0x00000001 : 0;
        return 0;
    }
    if (scene != 2 || matchSub != 0) return 0;

    const uintptr_t own = netActiveFighterAddress(
        static_cast<uint32_t>(g_netLocalCtrl));
    const uintptr_t peer = netActiveFighterAddress(
        static_cast<uint32_t>(g_netLocalCtrl ^ 1));
    const int32_t ownX = static_cast<int32_t>(rd32(own + 136u));
    const int32_t peerX = static_cast<int32_t>(rd32(peer + 136u));
    const int toward = ownX <= peerX ? 0x00080000 : 0x00040000;
    const uint32_t distance = static_cast<uint32_t>(
        ownX <= peerX ? peerX - ownX : ownX - peerX);
    const uint32_t resultState = rd32(kRoundResultStateAddr);
    const uint32_t roundCounter = rd32(0x871950u);
    if (resultState != 0u) {
        return resultState == 7u && g_netLocalCtrl == 0 && tap
            ? 0x00000001
            : 0;
    }
    if (roundCounter >= 5u) return 0;

    const uint32_t loser = (roundCounter & 1u) ? 0u : 1u;
    if (static_cast<uint32_t>(g_netLocalCtrl) == loser) return 0;

    if (distance > 40u) return toward;
    const uint32_t phase = frame % 32u;
    int mask = 0;
    if ((phase % 8u) < 3u) {
        const uint32_t attack = phase / 8u;
        mask = static_cast<int>(1u << attack);
        if (attack == 1u)
            mask |= 0x00020000;
    }
    return mask;
}

void netAutomatchPrepareCanonicalFrame(uint32_t frame, bool replay) {
    if (!g_netAutomatchTest) return;
    const bool gameplay = rollbackEligible();
    const bool charselectScene =
        !gameplay
        && rd32(kSceneStateAddr) == 3u
        && static_cast<int32_t>(rd32(addr::kScene3Substate)) == 2;
    if (charselectScene && !g_netAutomatchInCharselect) {
        ++g_netAutomatchCharselectVisits;
        g_netAutomatchCharselectBaseFrame = frame;
        QLOG("[test-automatch] charselect visit=%u frame=%u replay=%d",
             g_netAutomatchCharselectVisits, frame,
             static_cast<int>(replay));
    }
    g_netAutomatchInCharselect = charselectScene;
    const bool forceCharacterCursors =
        !g_netAutomatchFrontendOnly
        && charselectScene
        && g_netAutomatchCharselectVisits <= 1u
        && g_netAutomatchP1Character >= 0
        && g_netAutomatchP2Character >= 0;
    if (forceCharacterCursors) {
        constexpr uintptr_t kSelectorAlternateP1 = 0x87095Cu;
        constexpr uintptr_t kSelectorStride = 0x460u;
        if (rd32(addr::kCharSelConfirmP1) == 1u) {
            wr32(addr::kP1CharCursor,
                 static_cast<uint32_t>(g_netAutomatchP1Character));
            wr32(kSelectorAlternateP1,
                 static_cast<uint32_t>(g_netAutomatchP1Alternate));
        }
        if (rd32(addr::kCharSelConfirmP2) == 1u) {
            wr32(addr::kP2CharCursor,
                 static_cast<uint32_t>(g_netAutomatchP2Character));
            wr32(kSelectorAlternateP1 + kSelectorStride,
                 static_cast<uint32_t>(g_netAutomatchP2Alternate));
        }
        if (!g_netAutomatchForcedCharacterSceneActive) {
            QLOG("[test-automatch] forced character cursors frame=%u "
                 "P1=%d:%d P2=%d:%d replay=%d (cursor:alternate)",
                 frame, g_netAutomatchP1Character,
                 g_netAutomatchP1Alternate,
                 g_netAutomatchP2Character,
                 g_netAutomatchP2Alternate,
                 static_cast<int>(replay));
        }
    }
    g_netAutomatchForcedCharacterSceneActive = forceCharacterCursors;

    if (gameplay && !g_netAutomatchWasGameplay) {
        ++g_netAutomatchSegment;
        g_netAutomatchSegmentStart = frame;
        g_netAutomatchRoundSeenMask = 0;
        g_netAutomatchLowLifeLoggedMask = 0;
        g_netAutomatchNativeKoLoggedMask = 0;
        g_netAutomatchNativeLatchLoggedMask = 0;
        g_netAutomatchForcedKoLoggedMask = 0;
        QLOG("[test-automatch] segment=%u ENTER gameplay frame=%u "
             "epoch=%u replay=%d chars=[%u,%u]",
             g_netAutomatchSegment, frame, g_netEpoch,
             static_cast<int>(replay),
             rd32(addr::kP1CharCursor), rd32(addr::kP2CharCursor));
    } else if (!gameplay && g_netAutomatchWasGameplay) {
        QLOG("[test-automatch] segment=%u EXIT gameplay frame=%u "
             "matchSub=%d roundCounter=%u wins=[%u,%u] life=[%d,%d] "
             "resultState=%u",
             g_netAutomatchSegment, frame,
             static_cast<int>(rd32(kMatchSubAddr)),
             rd32(0x871950u), rd32(0x625628u), rd32(0x62562Cu),
             netBattleLifeEffective(0), netBattleLifeEffective(1),
             rd32(kRoundResultStateAddr));
    }
    g_netAutomatchWasGameplay = gameplay;
    if (g_netAutomatchFrontendOnly
        || !gameplay || g_netAutomatchSegment == 0
        || frame < g_netAutomatchSegmentStart) {
        return;
    }

    const uint32_t segmentAge = frame - g_netAutomatchSegmentStart;
    const uint32_t roundCounter = rd32(0x871950u);
    const uint32_t resultState = rd32(kRoundResultStateAddr);
    if (roundCounter >= 3u) return;

    const uint32_t round = roundCounter + 1u;
    const uint32_t roundBit = 1u << roundCounter;
    const uint32_t loser = (roundCounter & 1u) ? 0u : 1u;
    const uintptr_t lifeBase = netBattleLifeAddress(loser);
    int32_t life = netBattleLifeEffective(loser);

    if ((g_netAutomatchRoundSeenMask & roundBit) == 0u) {
        g_netAutomatchRoundSeenMask |= roundBit;
        QLOG("[test-automatch] round=%u BEGIN frame=%u segment=%u "
             "counter=%u wins=[%u,%u] life=[%d,%d] replay=%d",
             round, frame, g_netAutomatchSegment, roundCounter,
             rd32(0x625628u), rd32(0x62562Cu),
             netBattleLifeEffective(0), netBattleLifeEffective(1),
             static_cast<int>(replay));
    }

    if (resultState != 0u) {
        if ((g_netAutomatchNativeLatchLoggedMask & roundBit) == 0u) {
            g_netAutomatchNativeLatchLoggedMask |= roundBit;
            const uintptr_t active =
                netActiveFighterAddress(loser);
            const uintptr_t backup = rd32(0x623500u + 4u * loser);
            const uint32_t activeDeath =
                active >= 0x400000u && active < 0x900000u
                    ? rd32(active + 1368u) : 0xFFFFFFFFu;
            const uint32_t backupDeath =
                backup >= 0x400000u && backup < 0x900000u
                    ? rd32(backup + 1368u) : 0xFFFFFFFFu;
            QLOG("[test-automatch] round=%u native KO LATCH P%u "
                 "frame=%u resultState=%u activeDeath=%u backupDeath=%u "
                 "loserPtr=%08X resultLoser=%08X attacker=%u "
                 "wins=[%u,%u] deadlineArmed=%d",
                 round, loser + 1u, frame, resultState,
                 activeDeath, backupDeath, rd32(0x6267DCu),
                 rd32(0x625624u), rd32(0x625638u),
                 rd32(0x625628u), rd32(0x62562Cu),
                 (g_netAutomatchForcedKoLoggedMask & roundBit) != 0u);
        }
        if ((g_netAutomatchNativeKoLoggedMask & roundBit) == 0u) {
            g_netAutomatchNativeKoLoggedMask |= roundBit;
            QLOG("[test-automatch] round=%u native combat KO P%u "
                 "frame=%u segmentAge=%u resultState=%u life=%d",
                 round, loser + 1u, frame, segmentAge,
                 resultState, life);
        }
        return;
    }

    const uint32_t roundStride =
        (g_netAutomatchForceKoEnabled
             ? g_netAutomatchForceKoAfter
             : 360u)
        + 600u;
    const uint32_t lowLifeAt =
        roundCounter * roundStride + g_netAutomatchLowHpAfter;
    const uint32_t forceKoAt =
        roundCounter * roundStride + g_netAutomatchForceKoAfter;

    if (g_netAutomatchForceKoEnabled
        && segmentAge >= lowLifeAt && life > 300) {
        const int32_t lifeBefore = life;
        netSetBattleLifeEffective(loser, 300);
        life = 300;
        if ((g_netAutomatchLowLifeLoggedMask & roundBit) == 0u) {
            g_netAutomatchLowLifeLoggedMask |= roundBit;
            QLOG("[test-automatch] round=%u armed P%u native life=300 "
                 "frame=%u segmentAge=%u lifeBefore=%d components=%d/%d/%d "
                 "replay=%d",
                 round, loser + 1u, frame, segmentAge, lifeBefore,
                 static_cast<int32_t>(
                     rd32(lifeBase + kBattleLifeCurrentOffset)),
                 static_cast<int32_t>(
                     rd32(lifeBase + kBattleLifeDamageOffset)),
                 static_cast<int32_t>(
                     rd32(lifeBase + kBattleLifeReserveOffset)),
                 static_cast<int>(replay));
        }
    }
    if (segmentAge >= forceKoAt && life > 0 && resultState == 0u) {
        if (!g_netAutomatchForceKoEnabled) return;
        const uint32_t winner = loser ^ 1u;
        const uintptr_t loserActive = rd32(
            addr::kFighterPtrTable + 4u * loser);
        const uintptr_t winnerActive = rd32(
            addr::kFighterPtrTable + 4u * winner);
        const uintptr_t loserMirror = rd32(
            addr::kFighterBackupTable + 4u * loser);
        const bool loserReadable =
            rangeReadable(loserActive, addr::kFighterSize);
        const bool winnerReadable =
            rangeReadable(winnerActive, addr::kFighterSize);
        const bool mirrorReadable =
            rangeReadable(loserMirror, addr::kFighterSize);
        // 0x62369C is the applied VS formation row: 1 for two-fighter rows, 2 for four-fighter rows, 0 when no VS row has been applied.
        const bool nativeGuards =
            rollbackEligible()
            && rd32(kSceneStateAddr) == 2u
            && rd32(kMatchSubAddr) == 0u
            && rd32(addr::kVsFormationRow) <= 1u
            && rd32(kRoundResultStateAddr) == 0u
            && rd32(kNativeFatalHitModeAddr) == 0xFFFFFFFFu
            && rd32(kNativeFatalHitDisableAddr) == 0u
            && loserReadable && winnerReadable && mirrorReadable
            && loserActive != winnerActive
            && loserActive != loserMirror
            && rd8(loserActive + kFighterPlayerSlotOffset) == loser
            && rd8(winnerActive + kFighterPlayerSlotOffset) == winner
            && rd8(loserMirror + kFighterPlayerSlotOffset) == loser
            && rd32(loserActive + kFighterRoundEndOffset) == 0u
            && rd32(loserActive + kFighterRoundLostOffset) == 0u
            && rd32(winnerActive + kFighterRoundEndOffset) == 0u
            && rd32(winnerActive + kFighterRoundLostOffset) == 0u
            && rd32(loserMirror + kFighterRoundEndOffset) == 0u
            && rd32(loserMirror + kFighterRoundLostOffset) == 0u
            && netAutomatchNativeKoCodeMatches();
        if (!nativeGuards) {
            if (segmentAge == forceKoAt && !replay) {
                QLOG("[test-automatch] round=%u native-KO fallback BLOCKED "
                     "frame=%u age=%u active=%08X/%08X mirror=%08X "
                     "scene=%u sub=%u result=%u mode=%08X disable=%u "
                     "end/lost=%u/%u replay=%d",
                     round, frame, segmentAge,
                     static_cast<unsigned>(winnerActive),
                     static_cast<unsigned>(loserActive),
                     static_cast<unsigned>(loserMirror),
                     rd32(kSceneStateAddr), rd32(kMatchSubAddr),
                     rd32(kRoundResultStateAddr),
                     rd32(kNativeFatalHitModeAddr),
                     rd32(kNativeFatalHitDisableAddr),
                     loserReadable
                         ? rd32(loserActive + kFighterRoundEndOffset) : ~0u,
                     loserReadable
                         ? rd32(loserActive + kFighterRoundLostOffset) : ~0u,
                     static_cast<int>(replay));
            }
            return;
        }

        const auto reactionGate =
            reinterpret_cast<NativeReactionGateFn>(
                g_nativeReactionGateFn);
        if (reactionGate(reinterpret_cast<void*>(9u),
                         static_cast<int>(loserActive)) != 1) {
            if (segmentAge == forceKoAt && !replay) {
                QLOG("[test-automatch] round=%u native-KO fallback WAIT "
                     "reaction=9 rejected frame=%u age=%u loser=P%u "
                     "pending=%u variant=%u replay=%d",
                     round, frame, segmentAge, loser + 1u,
                     rd32(loserActive + kFighterPendingActionOffset),
                     rd32(loserActive + kFighterReactionVariantOffset)
                         & 0xFFu,
                     static_cast<int>(replay));
            }
            return;
        }

        const int32_t lifeBefore = netBattleLifeEffective(loser);
        const int damage = lifeBefore + 1;
        const auto applyDamage =
            reinterpret_cast<NativeDamageFn>(g_nativeDamageFn);
        (void)applyDamage(
            static_cast<int>(winner), static_cast<int>(loserActive),
            static_cast<int>(kNativeGenericKoCause), damage);
        const int32_t lifeAfter = netBattleLifeEffective(loser);
        if (lifeAfter >= 0) {
            if (segmentAge == forceKoAt && !replay) {
                QLOG("[test-automatch] round=%u native-KO fallback FAILED "
                     "damage=%d life=%d->%d frame=%u replay=%d",
                     round, damage, lifeBefore, lifeAfter, frame,
                     static_cast<int>(replay));
            }
            return;
        }

        wr32(winnerActive + kFighterHitTargetOffset,
             static_cast<uint32_t>(loserActive));
        wr32(loserActive + kFighterPendingActionOffset,
             kNativeFatalAction);
        *reinterpret_cast<volatile uint8_t*>(
            loserActive + kFighterReactionVariantOffset) = 2u;
        if ((g_netAutomatchForcedKoLoggedMask & roundBit) == 0u) {
            g_netAutomatchForcedKoLoggedMask |= roundBit;
            g_netAutomatchNativeKoLoggedMask |= roundBit;
            QLOG("[test-automatch] round=%u forced native KO fallback P%u "
                 "winner=P%u frame=%u age=%u damage=%d "
                 "life=%d->%d pending=%u variant=%u cause=%u replay=%d",
                 round, loser + 1u, winner + 1u, frame, segmentAge,
                 damage, lifeBefore, lifeAfter,
                 rd32(loserActive + kFighterPendingActionOffset),
                 rd32(loserActive + kFighterReactionVariantOffset) & 0xFFu,
                 kNativeGenericKoCause, static_cast<int>(replay));
        }
    }
}

constexpr uintptr_t kAiVmBaseAddr = 0x62EB80u;
constexpr uintptr_t kAiVmHeaderBytes = 0x20u;
constexpr uintptr_t kAiVmCursorAddr = kAiVmBaseAddr + 0x40020u;
static uint32_t g_netAiVmNormalizeLogs = 0;
static uint32_t g_netBootstrapTransientsEpoch = 0;
static bool pageWritable(const void* p);

void netNormalizeHumanMatchAiVm(uint32_t frame, bool replay) {
    netTrackCompletedGameWins();
    if (rollbackEligible()) return;

    bool dirty = false;
    for (uintptr_t offset = 0; offset < kAiVmHeaderBytes; offset += 4u) {
        if (rd32(kAiVmBaseAddr + offset) != 0u) {
            wr32(kAiVmBaseAddr + offset, 0u);
            dirty = true;
        }
    }
    if (rd32(kAiVmCursorAddr) != 0u) {
        wr32(kAiVmCursorAddr, 0u);
        dirty = true;
    }
    if (dirty && g_netAiVmNormalizeLogs < 8u) {
        ++g_netAiVmNormalizeLogs;
        QLOG("[human-match] AI VM normalized to inactive frame=%u replay=%d "
             "scene=%u matchSub=%d",
             frame, static_cast<int>(replay), rd32(kSceneStateAddr),
             static_cast<int>(rd32(kMatchSubAddr)));
    }
}

bool netCanonicalizeHumanGameStartTransients(uint32_t frame) {
    if (g_netBootstrapTransientsEpoch == g_netEpoch) return true;
    if (!rollbackEligible()
        || rd32(addr::kControlMask) != 0u
        || rd32(addr::kVsFormationRow) > 1u)
        return false;

    static constexpr std::array<uintptr_t, 2> kHumanFighters{{
        addr::kFighterP1, addr::kFighterP2,
    }};
    for (const uintptr_t fighter : kHumanFighters) {
        if (!rangeReadable(fighter + 164u, sizeof(uint32_t))) return false;
        const uintptr_t command = rd32(fighter + 164u);
        if (!rangeReadable(command, 0x200u)
            || !pageWritable(reinterpret_cast<const void*>(command + 8u))
            || !pageWritable(reinterpret_cast<const void*>(command + 435u))) {
            return false;
        }
        std::memset(reinterpret_cast<void*>(command + 8u), 0, 392u);
        std::memset(reinterpret_cast<void*>(command + 408u), 0, 28u);
    }

    constexpr uintptr_t kAiPlayerState = 0x8A5E00u;
    constexpr size_t kAiPlayerStateBytes = 0x8B0u;
    if (!rangeReadable(kAiPlayerState, kAiPlayerStateBytes)
        || !pageWritable(reinterpret_cast<const void*>(kAiPlayerState))
        || !pageWritable(reinterpret_cast<const void*>(
            kAiPlayerState + kAiPlayerStateBytes - 1u))) {
        return false;
    }
    std::memset(
        reinterpret_cast<void*>(kAiPlayerState), 0,
        kAiPlayerStateBytes);
    g_netBootstrapTransientsEpoch = g_netEpoch;
    QLOG("[replay.bootstrap] canonicalized human-game transients "
         "frame=%u epoch=%u", frame, g_netEpoch);
    return true;
}

void netPrepareNextCanonicalBoundary(uint32_t frame, bool replay) {
    if (frame == UINT32_MAX) return;
    const uint32_t nextFrame = frame + 1u;
    netAutomatchPrepareCanonicalFrame(nextFrame, replay);
    netNormalizeHumanMatchAiVm(nextFrame, replay);
}

static uint64_t g_ovrLogged = 0;
bool netOfflineOverrideActive(int player);
int netOfflineOverrideMask(int player);

static int paletteCustomConfirmTranslate(int player, int mask) {
    constexpr uintptr_t kSelectorLive       = 0x870520;  // Stride is 0x460 per slot.
    constexpr uintptr_t kSelectConfirmState = 0x8716D0;  // Stride is 4 per slot; 2 = colour.
    if (player < 0 || player > 3) return mask;
    if ((mask & 0x8) == 0) return mask;
    if ((mask & 0x1) != 0) return mask;
    if ((int)rd32(kSceneStateAddr) != 3) return mask;

    const bool live = rd32(kSelectorLive + 0x460u * (unsigned)player) != 0;
    const int confirmState =
        (int)rd32(kSelectConfirmState + 4u * (unsigned)player);
    if (live && confirmState == 2) return mask | 0x1;

    if (g_trainingLaunchRequested && player == 0) {
        for (int other = 1; other < 4; ++other) {
            const bool otherLive =
                rd32(kSelectorLive + 0x460u * (unsigned)other) != 0;
            const int otherState =
                (int)rd32(kSelectConfirmState + 4u * (unsigned)other);
            if (otherLive && otherState == 2) return mask | 0x1;
        }
    }

    static int s_lastRefused[4] = { -1, -1, -1, -1 };
    const int refusal = live ? confirmState : -2;
    if (s_lastRefused[player] != refusal) {
        s_lastRefused[player] = refusal;
        QLOG("[palette] D at character select not turned into a confirm for "
             "player %d: selector live=%d confirmState=%d training=%d (needs "
             "live=1 state=2, or a training selector on the colour stage)",
             player, live ? 1 : 0, confirmState,
             g_trainingLaunchRequested ? 1 : 0);
    }
    return mask;
}

int __fastcall hkInputReadResolved(int player);

int __fastcall hkInputRead(int player) {
    return paletteCustomConfirmTranslate(player, hkInputReadResolved(player));
}

int __fastcall hkInputReadResolved(int player) {
    if (g_netActive && (player == 0 || player == 1))
        return g_netRunning ? g_netInject[player] : 0;
    // .q99r stores the compact domain (buttons 0..3, directions 8..11); sub_484B90's consumer wants the expanded one (buttons 0..3, directions 16..19).
    if (g_replayRoleActive && player >= 0 && player < 4)
        return unpackLogical(qgame::replay::replayInjectedAction(player));
    if (trainingOverrideActive(player))
        return trainingOverrideMask(player);
    if (g_unattendedHarnessLaunch && !g_netActive && !g_replayRoleActive
        && (player == 0 || player == 1)
        && (int)rd32(kSceneStateAddr) == 3
        && qgame::directBootUnattendedFrontendActive())
        return g_frontendInject[player];
    if (netOfflineOverrideActive(player))
        return offlineRematchMaskServe(
            player, netOfflineOverrideMask(player));
    if (g_ovrActive && g_ovrEnabled && rollbackEligible() && (player == 0 || player == 1)) {
        int mask = g_inputTest ? inputTestMask(g_ovrFrame, player)
                               : aiInput(g_ovrFrame, player);
        if (g_ovrLogged < 8) {
            QLOG("[input_override] frame=%llu player=%d mask=%08X",
                 (unsigned long long)g_ovrFrame, player, (unsigned)mask);
            g_ovrLogged++;
        }
        return mask;
    }
    if (g_inputTest && g_ovrEnabled && player == 0 &&
        (int)rd32(kSceneStateAddr) == 2 && (int)rd32(kMatchSubAddr) != 0) {
        int drive = ((g_dispatchN >> 4) & 1) ? 0x0000000F : 0x00000000;
        static uint64_t s_driveLog = 0;
        if (drive && s_driveLog < 6) {
            QLOG("[match_drive] matchSub=%d dispatch=%llu confirm-pulse=0x%X",
                 (int)rd32(kMatchSubAddr), (unsigned long long)g_dispatchN, (unsigned)drive);
            s_driveLog++;
        }
        return drive;
    }
    return offlineRematchMaskServe(
        player, g_origRead ? g_origRead(player) : 0);
}
struct InputPhaseSample { int32_t x, y, act, facing; uint32_t ringHash; };
static InputPhaseSample g_ipStart{};
static int g_ipLastPhase = -1;
inline uint32_t cmdRingHash() {
    uintptr_t child = rd32(kFighterP1Addr + 164);
    if (!child) return 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i < 0x200; i += 4) { h ^= rd32(child + i); h *= 16777619u; }
    return h;
}
static void inputTestSample(uint64_t f) {
    int phase = (int)((f / 90) % 10);
    if (phase == g_ipLastPhase) return;
    InputPhaseSample now{ (int32_t)rd32(kFighterP1Addr + 136), (int32_t)rd32(kFighterP1Addr + 140),
                          (int32_t)rd32(kFighterP1Addr + 48),  (int32_t)(rd32(kFighterP1Addr + 168) >> 16 & 0xFF),
                          cmdRingHash() };
    if (g_ipLastPhase >= 0) {
        static const char* kName[10] = { "idle","dir10000","idle","dir20000","idle",
                                         "dir40000","idle","dir80000","btn1","btn2" };
        int mask = inputTestMask((f - 1), 0);
        QLOG("[input_test] phase=%s mask=0x%05X dx=%d dy=%d action %d->%d ringChanged=%d %s",
             kName[g_ipLastPhase], (unsigned)mask,
             now.x - g_ipStart.x, now.y - g_ipStart.y, g_ipStart.act, now.act,
             (int)(now.ringHash != g_ipStart.ringHash),
             (mask && (now.x != g_ipStart.x || now.act != g_ipStart.act ||
                       now.ringHash != g_ipStart.ringHash)) ? "EFFECT_OK"
             : (mask ? "<<NO_EFFECT" : ""));
    }
    g_ipLastPhase = phase;
    g_ipStart = now;
}

static uint32_t g_netPaletteSentEpoch = 0xFFFFFFFFu;
static uint32_t g_netPaletteRecordedMask = 0u;
static uint32_t g_netPaletteAppliedEpoch = 0xFFFFFFFFu;

void spectatorSetEventStateReset();
void spectatorPaletteViewerReset();

void netPaletteResetForSession() {
    g_netPaletteSentEpoch = 0xFFFFFFFFu;
    g_netPaletteAppliedEpoch = 0xFFFFFFFFu;
    g_netPaletteRecordedMask = 0u;
    spectatorSetEventStateReset();
    spectatorPaletteViewerReset();
}

static void paletteRecordingTick() {
    if (!qgame::replay::replayRecordingActive()) {
        g_netPaletteRecordedMask = 0u;
        return;
    }
    for (int side = 0; side < 2; ++side) {
        const uint32_t bit = 1u << side;
        if ((g_netPaletteRecordedMask & bit) != 0u) continue;
        unsigned char table[qreplay::kReplayPaletteTableBytes] = {};
        int variant = -1;
        if (!qgame::paletteQueryLocal(side, table, sizeof(table), variant))
            continue;
        qreplay::ReplayPaletteV2 palette{};
        palette.side = static_cast<uint8_t>(side);
        palette.variant =
            static_cast<uint8_t>((variant >= 0 && variant <= 3) ? variant : 0);
        palette.present = 1u;
        std::memcpy(palette.table, table, sizeof(table));
        if (qgame::replay::replayRecordPalette(palette)) {
            g_netPaletteRecordedMask |= bit;
            QLOG("[palette.replay] recorded slot %d (variant %d) into the "
                 "replay", side, palette.variant + 1);
        }
    }
}

static void paletteReplayApplyTick() {
    qgame::PaletteReplayHold& hold = qgame::paletteReplayHold();
    if (!g_replayRoleActive || hold.held == 0u) return;
    if ((int)rd32(kSceneStateAddr) != 2) {
        hold.applied = 0u;
        return;
    }
    for (int side = 0; side < 4; ++side) {
        const uint32_t bit = 1u << side;
        if ((hold.held & bit) == 0u) continue;
        if ((hold.applied & bit) != 0u) continue;
        if (qgame::paletteSubmitToWrapper(hold.slots[side], true)) {
            hold.applied |= bit;
            QLOG("[palette.replay] applied slot %d (variant %d) from the "
                 "recording", side, (int)hold.slots[side].variant + 1);
        }
    }
}

static void paletteOwnershipTick() {
    unsigned mask = 0xFu;
    if (g_replayRoleActive || g_mode == RunMode::Replay) {
        mask = 0u;
    } else if (g_mode == RunMode::Spectator || g_netSpectatorMode) {
        mask = 0u;
    } else if (g_netActive) {
        mask = 1u << ((g_netLocalCtrl == 1) ? 1 : 0);
    }
    qgame::paletteDeclareOwnership(mask);
}

struct SpectatorPaletteHold {
    bool valid = false;
    bool applied = false;
    uint32_t matchId = 0u;
    uint32_t recordCursor = 0u;
    int variant = 0;
    unsigned char entries[64] = {};
};
static SpectatorPaletteHold g_spectatorPaletteHold[2];
static uint32_t g_spectatorSetEventsConsumed = 0u;
static struct { uint32_t matchId; uint32_t cursor; }
    g_spectatorMatchMarks[8] = {};
static uint32_t g_spectatorMatchMarkCount = 0u;

static uint32_t g_spectatorReturnScanCursor = 0u;
static bool g_spectatorReturnPending = false;
static uint32_t g_spectatorReturnAtRecord = 0u;

void spectatorPaletteViewerReset() {
    for (SpectatorPaletteHold& hold : g_spectatorPaletteHold)
        hold = SpectatorPaletteHold{};
    g_spectatorSetEventsConsumed = 0u;
    g_spectatorMatchMarkCount = 0u;
    g_spectatorReturnScanCursor = 0u;
    g_spectatorReturnPending = false;
    g_spectatorReturnAtRecord = 0u;
}

static void spectatorPaletteViewerTick() {
    if (!g_netActive || !g_netSpectatorMode) return;

    const uint32_t applied = qgame::spectatorViewerSetEventCount();
    while (g_spectatorSetEventsConsumed < applied) {
        qnetplay::spectator::SetEvent event{};
        if (!qgame::spectatorViewerSetEventAt(
                g_spectatorSetEventsConsumed, event)) {
            break;
        }
        ++g_spectatorSetEventsConsumed;
        if (event.kind
                == qnetplay::spectator::SetEventKind::MatchStarted) {
            const uint32_t at = g_spectatorMatchMarkCount
                % (sizeof(g_spectatorMatchMarks)
                   / sizeof(g_spectatorMatchMarks[0]));
            g_spectatorMatchMarks[at] = { event.matchId,
                                          event.recordCursor };
            ++g_spectatorMatchMarkCount;
            QLOG("[palette.spectate] viewer saw match %u start at record %u",
                 event.matchId, event.recordCursor);
        } else if (event.kind
                   == qnetplay::spectator::SetEventKind::PaletteSelected) {
            const int side = (event.activeSlotMask & 0x1u) ? 0
                : (event.activeSlotMask & 0x2u)            ? 1
                                                           : -1;
            if (side >= 0) {
                SpectatorPaletteHold& hold = g_spectatorPaletteHold[side];
                hold.valid = true;
                hold.applied = false;
                hold.matchId = event.matchId;
                hold.recordCursor = event.recordCursor;
                hold.variant = static_cast<int>(event.valueC);
                std::memcpy(hold.entries, event.paletteEntries,
                            sizeof(hold.entries));
                QLOG("[palette.spectate] viewer holding slot %d palette "
                     "(variant %d) for match %u", side, hold.variant + 1,
                     event.matchId);
            }
        }
    }

    if ((int)rd32(kSceneStateAddr) != 2) return;
    qgame::SpectatorViewerStatus status{};
    if (!qgame::spectatorViewerGetStatus(status) || !status.active) return;
    const uint32_t playback = status.playbackNextRecord;

    uint32_t currentMatch = 0u;
    const uint32_t kept = g_spectatorMatchMarkCount
        < (sizeof(g_spectatorMatchMarks)
           / sizeof(g_spectatorMatchMarks[0]))
        ? g_spectatorMatchMarkCount
        : static_cast<uint32_t>(sizeof(g_spectatorMatchMarks)
                                / sizeof(g_spectatorMatchMarks[0]));
    for (uint32_t i = 0u; i < kept; ++i) {
        const auto& mark = g_spectatorMatchMarks[i];
        if (mark.cursor <= playback
            && (currentMatch == 0u || mark.matchId > currentMatch)) {
            currentMatch = mark.matchId;
        }
    }
    if (currentMatch == 0u) return;

    for (int side = 0; side < 2; ++side) {
        SpectatorPaletteHold& hold = g_spectatorPaletteHold[side];
        if (!hold.valid || hold.applied) continue;
        if (hold.matchId != currentMatch) continue;
        if (playback < hold.recordCursor) continue;
        if (qgame::paletteSubmitVariantToWrapper(side, hold.entries,
                                                 hold.variant)) {
            hold.applied = true;
            QLOG("[palette.spectate] viewer applied slot %d palette "
                 "(variant %d) for match %u at playback record %u",
                 side, hold.variant + 1, currentMatch, playback);
        }
    }
}

static uint32_t g_spectatorSetMatchOrdinal = 0u;
static bool g_spectatorSetMatchOpen = false;
static uint32_t g_spectatorSetPaletteSentMask = 0u;
static bool g_spectatorSetCheckpointDone = false;
static bool g_spectatorSetCheckpointArmed = false;
static uint32_t g_spectatorSetCheckpointSettle = 0u;

void spectatorSetEventStateReset() {
    g_spectatorSetMatchOrdinal = 0u;
    g_spectatorSetMatchOpen = false;
    g_spectatorSetPaletteSentMask = 0u;
    g_spectatorSetCheckpointDone = false;
    g_spectatorSetCheckpointArmed = false;
    g_spectatorSetCheckpointSettle = 0u;
}

constexpr uint32_t kSetSeekBootMagic = 0x42535351u;
constexpr size_t kSetSeekBootBytes = 40u;
constexpr uintptr_t kSetSeekStageAddr = 0x4ECB1Cu;
std::array<qreplay::FighterIdentityV2, qreplay::kFighterSlots>
netReplayRosterV2();

static void packSetSeekBoot(unsigned char* out,
                            uint32_t logicalStage) {
    const auto w32 = [&](size_t at, uint32_t v) {
        out[at] = static_cast<unsigned char>(v);
        out[at + 1] = static_cast<unsigned char>(v >> 8);
        out[at + 2] = static_cast<unsigned char>(v >> 16);
        out[at + 3] = static_cast<unsigned char>(v >> 24);
    };
    std::memset(out, 0, kSetSeekBootBytes);
    w32(0, kSetSeekBootMagic);
    w32(4, 1u);
    out[8] = 1u;
    const auto roster = netReplayRosterV2();
    for (size_t slot = 0; slot < 2u; ++slot) {
        const size_t at = 12u + slot * 5u;
        out[at + 0] = roster[slot].nativeCharacterId;
        out[at + 1] = roster[slot].paletteIndex;
        out[at + 2] = g_netStagedSelectorProof[slot].cursorCell;
        out[at + 3] = g_netStagedSelectorProof[slot].alternateFlag;
        out[at + 4] = g_netStagedSelectorProof[slot].confirmState;
    }
    w32(24, logicalStage);
    w32(28, g_spectatorSetMatchOrdinal);
    w32(32, g_spectatorSetMatchOrdinal);
    w32(36, g_spectatorSetMatchOrdinal);
}

static bool unpackSetSeekBoot(const unsigned char* bytes, size_t length,
                              qgame::ReplayDirectBootConfig& boot,
                              qnetplay::spectator::SpectatorSetTimeline::
                                  BindSeed& seed) {
    const auto r32 = [&](size_t at) {
        return static_cast<uint32_t>(bytes[at])
            | (static_cast<uint32_t>(bytes[at + 1]) << 8)
            | (static_cast<uint32_t>(bytes[at + 2]) << 16)
            | (static_cast<uint32_t>(bytes[at + 3]) << 24);
    };
    if (bytes == nullptr || length != kSetSeekBootBytes) return false;
    if (r32(0) != kSetSeekBootMagic || r32(4) != 1u) return false;
    boot.vsRow = bytes[8];
    for (size_t slot = 0; slot < 2u; ++slot) {
        const size_t at = 12u + slot * 5u;
        boot.slots[slot].cursorCell = bytes[at + 2];
        boot.slots[slot].alternateFlag = bytes[at + 3];
        boot.slots[slot].paletteIndex = bytes[at + 1];
        boot.slots[slot].confirmState = bytes[at + 4];
    }
    boot.logicalStageIndex = r32(24);
    seed.openMatchId = r32(28);
    seed.highestMatchId = r32(32);
    seed.matchesSoFar = r32(36);
    return seed.openMatchId != 0u;
}

static void spectatorSetCheckpointTick() {
    if (!g_spectatorSetMatchOpen || g_spectatorSetCheckpointDone
        || g_spectatorSetCheckpointArmed) {
        return;
    }
    if (++g_spectatorSetCheckpointSettle < 30u) return;
    const bool sceneOk = (int)rd32(kSceneStateAddr) == 2;
    const bool stateReady = g_rbReady && g_rb.totalSize() != 0;
    const uint32_t logicalStage = rd32(kSetSeekStageAddr);
    // 0x4ECB1C is already the logical stage index; do not pass it through the native-id catalog lookup.
    const bool bootReady =
        g_netStagedSelectorProofValid && logicalStage < 28u;
    if (!(sceneOk && stateReady && bootReady)) {
        if ((g_spectatorSetCheckpointSettle % 300u) == 0u) {
            QLOG("[checkpoint] waiting (scene2=%d stateReady=%d "
                 "selectorProof=%d logicalStage=%u settle=%u)",
                 sceneOk ? 1 : 0, stateReady ? 1 : 0,
                 g_netStagedSelectorProofValid ? 1 : 0, logicalStage,
                 g_spectatorSetCheckpointSettle);
        }
        return;
    }
    g_spectatorSetCheckpointArmed = true;
    QLOG("[checkpoint] armed: game %u captures its canonical slot at the "
         "next confirmed publication",
         g_spectatorSetMatchOrdinal);
}

static bool spectatorSetEventPublishF1Return() {
    if (!qgame::spectator_publish::sidecarPublisherActive()) return false;
    if (g_spectatorSetMatchOrdinal == 0u) return false;
    if (g_spectatorSetMatchOpen) {
        qnetplay::spectator::SetEvent result{};
        result.kind = qnetplay::spectator::SetEventKind::MatchResult;
        result.matchId = g_spectatorSetMatchOrdinal;
        if (!qgame::spectator_publish::publishChainedSetEvent(result))
            return false;
        g_spectatorSetMatchOpen = false;
        QLOG("[palette.spectate] match %u marked ended (F1 return)",
             g_spectatorSetMatchOrdinal);
    }
    qnetplay::spectator::SetEvent returned{};
    returned.kind =
        qnetplay::spectator::SetEventKind::ReturnedToCharacterSelect;
    returned.matchId = g_spectatorSetMatchOrdinal;
    if (!qgame::spectator_publish::publishChainedSetEvent(returned)) {
        QLOG("[spectator-source] F1 return event refused; the feed will "
             "seal instead");
        return false;
    }
    QLOG("[spectator-source] F1 return published as a chained event "
         "(match %u); the feed stays open for the next game",
         g_spectatorSetMatchOrdinal);
    return true;
}

static void spectatorSetEventTick() {
    if (!g_netActive || g_netSpectatorMode || g_replayRoleActive) return;
    if (!qgame::spectator_publish::sidecarPublisherActive()) return;

    const bool recording = qgame::replay::replayRecordingActive();

    if (recording && !g_spectatorSetMatchOpen) {
        qnetplay::spectator::SetEvent started{};
        started.kind = qnetplay::spectator::SetEventKind::MatchStarted;
        started.matchId = g_spectatorSetMatchOrdinal + 1u;
        started.valueC = g_spectatorSetMatchOrdinal + 1u;
        if (qgame::spectator_publish::publishChainedSetEvent(started)) {
            ++g_spectatorSetMatchOrdinal;
            g_spectatorSetMatchOpen = true;
            g_spectatorSetPaletteSentMask = 0u;
            g_spectatorSetCheckpointDone = false;
            g_spectatorSetCheckpointArmed = false;
            g_spectatorSetCheckpointSettle = 0u;
            QLOG("[palette.spectate] match %u marked started",
                 g_spectatorSetMatchOrdinal);
        }
    } else if (!recording && g_spectatorSetMatchOpen) {
        qnetplay::spectator::SetEvent result{};
        result.kind = qnetplay::spectator::SetEventKind::MatchResult;
        result.matchId = g_spectatorSetMatchOrdinal;
        if (qgame::spectator_publish::publishChainedSetEvent(result)) {
            g_spectatorSetMatchOpen = false;
            QLOG("[palette.spectate] match %u marked ended",
                 g_spectatorSetMatchOrdinal);
        }
    }

    if (!g_spectatorSetMatchOpen) return;
    spectatorSetCheckpointTick();
    for (int side = 0; side < 2; ++side) {
        const uint32_t bit = 1u << side;
        if ((g_spectatorSetPaletteSentMask & bit) != 0u) continue;
        unsigned char table[qreplay::kReplayPaletteTableBytes] = {};
        int variant = -1;
        if (!qgame::paletteQueryLocal(side, table, sizeof(table), variant))
            continue;
        if (variant < 0 || variant > 3) variant = 0;
        qnetplay::spectator::SetEvent palette{};
        palette.kind = qnetplay::spectator::SetEventKind::PaletteSelected;
        palette.matchId = g_spectatorSetMatchOrdinal;
        palette.valueC = static_cast<uint32_t>(variant);
        palette.activeSlotMask = static_cast<uint8_t>(bit);
        std::memcpy(palette.paletteEntries,
                    table + static_cast<size_t>(variant) * 64u, 64u);
        if (qgame::spectator_publish::publishChainedSetEvent(palette)) {
            g_spectatorSetPaletteSentMask |= bit;
            QLOG("[palette.spectate] slot %d palette (variant %d) published "
                 "for match %u", side, variant + 1,
                 g_spectatorSetMatchOrdinal);
        }
    }
}

static void netPaletteExchangeTick() {
    paletteOwnershipTick();
    paletteReplayApplyTick();
    paletteRecordingTick();
    spectatorSetEventTick();
    spectatorPaletteViewerTick();
    if (!g_netActive || !g_netRunning) return;
    const qgame::PaletteWrapperExports& exports =
        qgame::paletteWrapperExports();
    if (exports.query == nullptr || exports.submit == nullptr) return;

    const int localSide = (g_netLocalCtrl == 1) ? 1 : 0;

    if (g_netPaletteSentEpoch != g_netEpoch) {
        unsigned char table[qnetplay::wire::kPaletteTableWireBytes] = {};
        int variant = -1;
        const int got = exports.query(localSide, table,
                                      static_cast<int>(sizeof(table)),
                                      &variant);
        if (got == static_cast<int>(sizeof(table))) {
            qnetplay::wire::PalettePayload payload{};
            payload.epoch = g_netEpoch;
            payload.side = static_cast<uint8_t>(localSide);
            payload.variant =
                static_cast<uint8_t>((variant >= 0 && variant <= 3) ? variant
                                                                    : 0);
            payload.present = 1;
            std::memcpy(payload.table, table, sizeof(table));
            if (qgame::netSubmitPalette(payload)) {
                g_netPaletteSentEpoch = g_netEpoch;
                QLOG("[palette.net] sending our slot %d palette (variant %d, "
                     "%u bytes) to the peer; epoch=%u",
                     localSide, payload.variant + 1,
                     (unsigned)sizeof(table), g_netEpoch);
            }
        }
    }

    qnetplay::wire::PalettePayload inbound{};
    while (qgame::netTakePalette(inbound)) {
        if (inbound.side == localSide) {
            QLOG("[palette.net] IGNORED a peer palette claiming our own slot "
                 "%d - a peer may only recolour its own fighter",
                 (int)inbound.side);
            continue;
        }
        if (inbound.present == 0) continue;
        if (inbound.epoch != g_netEpoch) {
            QLOG("[palette.net] IGNORED a peer palette from epoch %u (we are "
                 "on %u): it belongs to a match that has ended",
                 inbound.epoch, g_netEpoch);
            continue;
        }
        if (g_netPaletteAppliedEpoch == g_netEpoch) continue;
        const BOOL ok = exports.submit(
            static_cast<int>(inbound.side), inbound.table,
            static_cast<int>(sizeof(inbound.table)),
            static_cast<int>(inbound.variant), 1 );
        QLOG("[palette.net] peer palette for slot %d (variant %d) %s",
             (int)inbound.side, (int)inbound.variant + 1,
             ok ? "accepted by the wrapper validator"
                : "REFUSED by the wrapper validator");
        if (ok) g_netPaletteAppliedEpoch = g_netEpoch;
    }
}

static void frontendPressDriverTick() {
    constexpr uintptr_t kS3SubstateAddr = 0x4AA454;  // Values: 1 = VS list, 2 = charselect.
    constexpr uintptr_t kP1ConfirmState = 0x8716D0;  // Values: 1 browse, 2 char, 4 palette done.
    constexpr uintptr_t kP2ConfirmState = 0x8716D4;  // 0 means the selector is native/CPU-owned.
    constexpr uintptr_t kProceedLatch   = 0x8716E8;  // 1 means the launch gate consumes presses.
    constexpr int kConfirmBit    = 0x1;   // Logical confirm in the expanded domain.
    constexpr int kPressFrames   = 2;
    constexpr int kNeutralFrames = 8;
    constexpr uint32_t kSettleFrames = 30;
    constexpr uint64_t kRetryFrames  = 30;
    static const int s_paletteDLane = [] {
        char buf[8] = {};
        DWORD n = GetEnvironmentVariableA("QOH99_HARNESS_PALETTE_D", buf,
                                          sizeof(buf));
        if (n == 0 || n >= sizeof(buf)) return 0;
        const int want = atoi(buf);
        return (want >= 1 && want <= 4) ? want : 0;
    }();
    static int s_edgeBits = kConfirmBit;
    static int s_pressLeft = 0;
    static int s_neutralLeft = 0;
    static int s_player = 0;
    static uint32_t s_settle = 0;
    static uint64_t s_tick = 0;
    static uint64_t s_lastEdgeTick = 0;
    static uint32_t s_lastAimKey = 0xFFFFFFFFu;
    static bool s_inCharSelect = false;

    if (!g_unattendedHarnessLaunch || g_netActive || g_replayRoleActive
        || !qgame::directBootUnattendedFrontendActive()) {
        g_frontendInject[0] = 0; g_frontendInject[1] = 0;
        s_pressLeft = 0; s_neutralLeft = 0; s_settle = 0;
        s_lastAimKey = 0xFFFFFFFFu;
        s_inCharSelect = false;
        return;
    }
    ++s_tick;
    const int scene = (int)rd32(kSceneStateAddr);
    const int s3sub = (int)rd32(kS3SubstateAddr);
    if (scene != 3 || s3sub != 2) {
        g_frontendInject[0] = 0; g_frontendInject[1] = 0;
        s_pressLeft = 0; s_neutralLeft = 0; s_settle = 0;
        s_lastAimKey = 0xFFFFFFFFu;
        s_inCharSelect = false;
        return;
    }
    if (!s_inCharSelect) {
        s_inCharSelect = true;
        QLOG("[frontend_press] character select: press driver engaged "
             "(p1=%u p2=%u)", rd32(kP1ConfirmState), rd32(kP2ConfirmState));
    }
    if (s_settle < kSettleFrames) { ++s_settle; return; }

    if (s_pressLeft > 0) {
        --s_pressLeft;
        g_frontendInject[s_player] = s_edgeBits;
        g_frontendInject[s_player ^ 1] = 0;
        return;
    }
    g_frontendInject[0] = 0; g_frontendInject[1] = 0;
    if (s_neutralLeft > 0) { --s_neutralLeft; return; }

    const uint32_t p1 = rd32(kP1ConfirmState);
    const uint32_t p2 = rd32(kP2ConfirmState);
    const uint32_t proceed = rd32(kProceedLatch);
    const auto armPaletteD = [](int slot) {
        constexpr uintptr_t kSelectorPalette = 0x870940;
        constexpr uintptr_t kSelectorStride  = 0x460;
        wr32(kSelectorPalette + kSelectorStride * (unsigned)slot,
             (uint32_t)(s_paletteDLane - 1));
        return 0x8;
    };
    int target = -1;
    const char* what = nullptr;
    s_edgeBits = kConfirmBit;
    if (proceed == 1u) {
        target = 0; what = "stage/proceed";
    } else if (p1 == 1u || p1 == 2u) {
        target = 0; what = p1 == 1u ? "P1 character" : "P1 palette";
        if (p1 == 2u && s_paletteDLane) s_edgeBits = armPaletteD(0);
    } else if (p1 == 4u && (p2 == 1u || p2 == 2u)) {
        target = 1; what = p2 == 1u ? "P2 character" : "P2 palette";
        if (p2 == 2u && s_paletteDLane) s_edgeBits = armPaletteD(1);
    }
    if (target < 0) {
        s_lastAimKey = 0xFFFFFFFFu;
        return;
    }
    const uint32_t aimKey = (proceed << 16)
        | (static_cast<uint32_t>(target) << 8)
        | (target == 0 ? p1 : p2);
    if (aimKey == s_lastAimKey && s_tick - s_lastEdgeTick < kRetryFrames) {
        return;
    }
    s_player = target;
    s_pressLeft = kPressFrames;
    s_neutralLeft = kNeutralFrames;
    s_lastEdgeTick = s_tick;
    s_lastAimKey = aimKey;
    QLOG("[frontend_press] confirm edge -> %s (p1=%u p2=%u proceed=%u)",
         what, p1, p2, proceed);
}

typedef int (__cdecl* PollFn)();
static PollFn        g_origPoll = nullptr;
static bool          g_pollHooked = false;
static volatile bool g_netPhysicalPoll = false;
static uintptr_t     g_fullInputPollFn = 0;
constexpr uintptr_t  kKeyStateAddr = 0x4AA920;
constexpr int        kKsRingSlots = 16;
struct KsRing { uint8_t ks[0x100]; bool valid = false; };
static KsRing        g_ksRings[kKsRingSlots];
static KsRing*       g_ksActive = nullptr;
static volatile int  g_ksMode = 0;
inline void ksBeginRecord(int slot) { g_ksActive = &g_ksRings[slot % kKsRingSlots]; g_ksActive->valid = false; g_ksMode = 1; }
inline void ksBeginReplay(int slot) { g_ksActive = &g_ksRings[slot % kKsRingSlots]; g_ksMode = 2; }
inline void ksOff() { g_ksMode = 0; g_ksActive = nullptr; }
int __cdecl hkPoll() {
    if (g_netActive && !g_netPhysicalPoll) {
        std::memset(reinterpret_cast<void*>(kKeyStateAddr), 0, 0x100);
        return 1;
    }
    if (g_ksMode == 2 && g_ksActive && g_ksActive->valid) {
        std::memcpy(reinterpret_cast<void*>(kKeyStateAddr), g_ksActive->ks, 0x100);
        return 1;
    }
    int r = g_origPoll ? g_origPoll() : 1;
    if (g_ksMode == 1 && g_ksActive) {
        std::memcpy(g_ksActive->ks, reinterpret_cast<const void*>(kKeyStateAddr), 0x100);
        g_ksActive->valid = true;
    }
    return r;
}

// 45B590 is a DirectDraw palette upload; 45F9C0/45FB20 start DirectSound buffers, 45FB60 stops them, 45FBB0 exposes playback status to game logic.
static void fxSaveTo(void* buf);
static void fxRestoreFrom(void* buf);
typedef int (__fastcall* PaletteApplyFn)(int palette, int first, int apply);
typedef int (__fastcall* SoundLoadFn)(int sound, const char* path);
typedef int (__fastcall* SoundDuplicatePlayFn)(
    int sound, unsigned int slot, int voices, int flags);
typedef int (__fastcall* SoundDirectPlayFn)(int sound, int flags);
typedef int (__fastcall* SoundStopFn)(int slot, int domain);
typedef int (__fastcall* SoundStatusFn)(int slot, int domain);
static PaletteApplyFn       g_origPaletteApply = nullptr;
static SoundLoadFn          g_origSoundLoad = nullptr;
static SoundDuplicatePlayFn g_origSoundDuplicatePlay = nullptr;
static SoundDirectPlayFn    g_origSoundDirectPlay = nullptr;
static SoundStopFn          g_origSoundStop = nullptr;
static SoundStatusFn        g_origSoundStatus = nullptr;
static bool g_paletteHooked = false;
static bool g_soundLoadHooked = false;
static bool g_scoreDrawHooked = false;
static bool g_timeDrawHooked  = false;
static bool g_hideNativeScore = false;
static bool g_hideNativeTimer = false;

static qgame::OfflineNativeHudPreferencesV1 g_nativeHudPrefs{};
static uint32_t g_nativeHudGeneration = 0u;
static bool g_nativeHudModularEligible = false;

static qgame::NativeHudOfflineOwner nativeHudOfflineOwner();
static qgame::NativeHudEffectiveMasks nativeHudEffectiveMasksFor(
    qgame::NativeHudOfflineOwner owner);
// sub_42A990: __fastcall(ecx=slot, edx=playerMask, stack: score, altX, x), ret 0Ch.
using QohScoreDrawFn = int (__fastcall*)(int, int, unsigned int, int, int);
// sub_42C040: __cdecl, no args, plain ret (returns the sub_42A5C0 result pointer).
using QohTimeDrawFn  = void* (__cdecl*)();
static QohScoreDrawFn g_origScoreDraw = nullptr;
static QohTimeDrawFn  g_origTimeDraw  = nullptr;

// sub_41F290: __fastcall(ecx=fighterA, edx=fighterB) - BOTH gauges, one call.
using QohGuardDrawFn = int (__fastcall*)(int, int);
using QohPortraitLifeDrawFn = int (__fastcall*)(int, uint32_t*);
// sub_42BB30: __fastcall(ecx=index, edx=characterId, stack: x). index 0/2 are the left team, 1/4 the right.
using QohNameDrawFn = int (__fastcall*)(int, int, int);
// sub_42BD60: __fastcall(ecx=table, edx=wins, stack: index, x, ...). index 0 = left, 4 = right.
using QohRoundWinsDrawFn = void (__fastcall*)(uint32_t*, int, int, int, int,
                                              int);
using QohExDrawFn = int (__fastcall*)(int, uint32_t*);
using QohComboDrawFn = int (__fastcall*)(int);
using QohHudEmitFn = void* (__fastcall*)(int, int, uint32_t*, int);
static QohExDrawFn g_origExDraw = nullptr;
static QohComboDrawFn g_origComboDraw = nullptr;
static QohHudEmitFn g_origHudEmit = nullptr;
static bool g_exDrawHooked = false;
static bool g_comboDrawHooked = false;
static bool g_hudEmitHooked = false;
static QohGuardDrawFn g_origGuardDraw = nullptr;
static QohPortraitLifeDrawFn g_origPortraitLifeDraw = nullptr;
static QohNameDrawFn g_origNameDraw = nullptr;
static QohRoundWinsDrawFn g_origRoundWinsDraw = nullptr;
static bool g_guardDrawHooked = false;
static bool g_portraitLifeDrawHooked = false;
static bool g_nameDrawHooked = false;
static bool g_roundWinsDrawHooked = false;
static bool g_soundDuplicatePlayHooked = false;
static bool g_soundDirectPlayHooked = false;
static bool g_soundStopHooked = false;
static bool g_soundStatusHooked = false;
static volatile bool g_replayActive = false;
static volatile LONG g_presentationDisposition =
    static_cast<LONG>(PresentationDisposition::Normal);
static volatile LONG g_presentationDispositionQueries = 0;
static volatile LONG g_wrapperNormalVisiblePresents = 0;
static volatile LONG g_wrapperSuppressedPresents = 0;
static volatile LONG g_wrapperCorrectedFinalPresents = 0;
static uint64_t g_hiddenReplayNativeTicks = 0;
static uint64_t g_correctedFinalNativeTicks = 0;
static uint64_t g_verifiedHiddenPresentationTicks = 0;
static uint64_t g_missingPresentationGateTicks = 0;

void netLogAudioLedger();

void netLogPresentationSummary() {
    netLogAudioLedger();
    QLOG("[presentation-summary] "
         "hiddenReplayNativeTicks=%llu correctedFinalNativeTicks=%llu "
         "hiddenForwardNativeTicks=%llu "
         "verifiedHiddenPresentationTicks=%llu "
         "missingPresentationGateTicks=%llu dispositionQueries=%lu "
         "wrapper normalVisiblePresents=%lu suppressedPresents=%lu "
         "correctedFinalPresents=%lu",
         static_cast<unsigned long long>(g_hiddenReplayNativeTicks),
         static_cast<unsigned long long>(g_correctedFinalNativeTicks),
         static_cast<unsigned long long>(g_hiddenForwardNativeTicks),
         static_cast<unsigned long long>(
             g_verifiedHiddenPresentationTicks),
         static_cast<unsigned long long>(g_missingPresentationGateTicks),
         presentationDispositionQueryCount(),
         static_cast<unsigned long>(InterlockedCompareExchange(
             &g_wrapperNormalVisiblePresents, 0, 0)),
         static_cast<unsigned long>(InterlockedCompareExchange(
             &g_wrapperSuppressedPresents, 0, 0)),
         static_cast<unsigned long>(InterlockedCompareExchange(
             &g_wrapperCorrectedFinalPresents, 0, 0)));
    QLOG("[pacing-ledger] "
         "commitGrants=%ld noCommitFallbackGrants=%ld "
         "spectatorBatchFallbackGrants=%ld duplicateCommitCallbacks=%ld "
         "duplicatePublishCallbacks=%ld hiddenCommitBypasses=%ld "
         "unownedCommitCallbacks=%ld "
         "totalGrants=%ld waitCallbacks=%ld publishCallbacks=%ld "
         "publishFailures=%ld clockFaults=%ld",
         InterlockedCompareExchange(
             &g_netCommitBoundaryGrants, 0, 0),
         InterlockedCompareExchange(
             &g_netNoCommitFallbackGrants, 0, 0),
         InterlockedCompareExchange(
             &g_netSpectatorBatchFallbackGrants, 0, 0),
         InterlockedCompareExchange(
             &g_netDuplicateCommitCallbacks, 0, 0),
         InterlockedCompareExchange(
             &g_netDuplicatePublishCallbacks, 0, 0),
         InterlockedCompareExchange(
             &g_netHiddenCommitBypasses, 0, 0),
         InterlockedCompareExchange(
             &g_netUnownedCommitCallbacks, 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationSlotGrants, 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationWaitCallbacks, 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationCommitCallbacks, 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationFailureCallbacks, 0, 0),
         InterlockedCompareExchange(
             &g_netPacingClockFaults, 0, 0));
    QLOG("[pacing-publish-failures] "
         "invalid=%ld noTarget=%ld noSource=%ld emptyClient=%ld "
         "getDc=%ld gdi=%ld disposition=%ld",
         InterlockedCompareExchange(
             &g_netPresentationFailureReasons[0], 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationFailureReasons[1], 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationFailureReasons[2], 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationFailureReasons[3], 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationFailureReasons[4], 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationFailureReasons[5], 0, 0),
         InterlockedCompareExchange(
             &g_netPresentationFailureReasons[6], 0, 0));
}

void netServiceTestPacingSummaryRequest() {
    if (!g_netActive)
        return;
    const LONG request = InterlockedCompareExchange(
        &g_netTestPacingSummaryRequest, 0, 0);
    if (request == 0
        || InterlockedCompareExchange(
               &g_netTestPacingSummaryAcknowledged, 0, 0) == request) {
        return;
    }
    if (!g_spectatorPacingSummaryLogged) {
        g_spectatorPacingSummaryLogged = true;
        netLogPresentationSummary();
    }
    InterlockedExchange(
        &g_netTestPacingSummaryAcknowledged, request);
}

class ScopedPresentationDisposition {
public:
    explicit ScopedPresentationDisposition(
        PresentationDisposition disposition)
        : previous_(InterlockedExchange(
              &g_presentationDisposition,
              static_cast<LONG>(disposition))) {
    }

    ~ScopedPresentationDisposition() {
        InterlockedExchange(&g_presentationDisposition, previous_);
    }

    ScopedPresentationDisposition(
        const ScopedPresentationDisposition&) = delete;
    ScopedPresentationDisposition& operator=(
        const ScopedPresentationDisposition&) = delete;

private:
    LONG previous_;
};
static uint64_t g_audioSuppressed = 0;
static bool g_sfxPassthrough = false;

static bool g_audioV2Enabled = true;
static bool g_audioV2Muted = false;

// DirectSound buffer tables: 256 static base buffers at 0x4AAD14, 64 duplicate endpoints at 0x4AAC0C, BGM Wave stream secondary buffer at 0x4AAAEC.
static uint8_t g_musicVolume = 100;
static uint8_t g_soundVolume = 100;
constexpr uintptr_t kSfxBaseBufferTable = 0x4AAD14u;
constexpr uintptr_t kSfxDuplicateTable = 0x4AAC0Cu;
constexpr uintptr_t kBgmStreamBufferAlias = 0x4AAAECu;

inline LONG volumeToMillibels(uint8_t volume) {
    if (volume == 0) return -10000;
    if (volume >= 100) return 0;
    const double ratio = static_cast<double>(volume) / 100.0;
    const double millibels = 2000.0 * log10(ratio);
    return millibels < -10000.0 ? -10000
                                : static_cast<LONG>(millibels);
}

enum class VolumeApplyResult : uint8_t {
    Applied = 0,
    NullPointer,
    UnreadableObject,
    BadVtable,
    BadEntry,
    Faulted,
    HResultFailed,
};

bool comRangeUsable(uintptr_t address, size_t bytes, bool executable) {
    if (address == 0 || bytes == 0) return false;
    if (address + bytes < address) return false;
    uintptr_t cursor = address;
    const uintptr_t end = address + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi,
                         sizeof(mbi)) == 0)
            return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD protect = mbi.Protect & 0xFFu;
        if (mbi.Protect & PAGE_GUARD) return false;
        const bool readable =
            protect == PAGE_READONLY || protect == PAGE_READWRITE
            || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READ
            || protect == PAGE_EXECUTE_READWRITE
            || protect == PAGE_EXECUTE_WRITECOPY;
        if (!readable) return false;
        if (executable) {
            const bool exec = protect == PAGE_EXECUTE_READ
                || protect == PAGE_EXECUTE_READWRITE
                || protect == PAGE_EXECUTE_WRITECOPY;
            if (!exec) return false;
        }
        const uintptr_t regionBase =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t next = regionBase + mbi.RegionSize;
        if (regionBase > cursor || next <= cursor) return false;
        cursor = next;
    }
    return true;
}

using DsSetVolumeFn = long(__stdcall*)(void*, LONG);

bool invokeSetVolumeGuarded(DsSetVolumeFn entry, void* object, LONG millibels,
                            long& hr) {
    __try {
        hr = entry(object, millibels);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = 0;
        return false;
    }
}

VolumeApplyResult bufferSetVolume(uintptr_t comBuffer, LONG millibels,
                                  long& hr) {
    hr = 0;
    if (comBuffer == 0) return VolumeApplyResult::NullPointer;
    if (!comRangeUsable(comBuffer, sizeof(uint32_t), false))
        return VolumeApplyResult::UnreadableObject;
    const uintptr_t vtable = rd32(comBuffer);
    // Slot 15 is SetVolume; the IDirectSoundBuffer vtable prefix is 16 slots.
    if (!comRangeUsable(vtable, 16u * sizeof(uint32_t), false))
        return VolumeApplyResult::BadVtable;
    const uintptr_t entry = rd32(vtable + 15u * sizeof(uint32_t));
    if (!comRangeUsable(entry, 1u, true))
        return VolumeApplyResult::BadEntry;
    if (!invokeSetVolumeGuarded(reinterpret_cast<DsSetVolumeFn>(entry),
                                reinterpret_cast<void*>(comBuffer), millibels,
                                hr)) {
        return VolumeApplyResult::Faulted;
    }
    return hr < 0 ? VolumeApplyResult::HResultFailed
                  : VolumeApplyResult::Applied;
}

const char* volumeApplyResultName(VolumeApplyResult result) {
    switch (result) {
        case VolumeApplyResult::Applied:         return "applied";
        case VolumeApplyResult::NullPointer:     return "null";
        case VolumeApplyResult::UnreadableObject:return "unreadable-object";
        case VolumeApplyResult::BadVtable:       return "bad-vtable";
        case VolumeApplyResult::BadEntry:        return "bad-slot15";
        case VolumeApplyResult::Faulted:         return "SEH-fault";
        case VolumeApplyResult::HResultFailed:   return "hresult-failed";
    }
    return "?";
}

static volatile LONG g_audioBufferGeneration = 1;
static LONG g_audioAppliedGeneration = 0;
static uint64_t g_audioVolumeFailures = 0;

void netAudioNoteBufferGenerationChanged() {
    InterlockedIncrement(&g_audioBufferGeneration);
}

void netAudioVolumeSweep() {
    // Profile +60 is the BGM mode: 0=Wave 1=MIDI 3=off 4=CD.
    static bool s_bgmModeLogged = false;
    if (!s_bgmModeLogged) {
        const uint32_t activeProfile = rd32(0x615860u);
        if (activeProfile < 10u) {
            s_bgmModeLogged = true;
            const uint32_t bgmMode =
                rd32(0x615864u + activeProfile * 764u + 60u);
            QLOG("[audio] game config: profile=%u bgmMode=%u "
                 "(0=Wave 1=MIDI 3=OFF 4=CD)%s",
                 activeProfile, bgmMode,
                 bgmMode == 3u ? " - music is DISABLED in the game's own "
                                 "config (QOHcnf.key +60)" : "");
        }
    }
    constexpr uint32_t kVolumeSlots = 256u + 64u + 1u;
    static uintptr_t s_applied[kVolumeSlots] = {};
    static bool s_failed[kVolumeSlots] = {};
    static uint8_t s_appliedMusic = 255;
    static uint8_t s_appliedSound = 255;
    const bool settingsChanged = s_appliedMusic != g_musicVolume
        || s_appliedSound != g_soundVolume;
    if (g_musicVolume == 100 && g_soundVolume == 100 && !settingsChanged)
        return;
    const LONG generation =
        InterlockedCompareExchange(&g_audioBufferGeneration, 0, 0);
    static uint32_t s_dispatchesSinceScan = 0;
    const bool periodicRescan = ++s_dispatchesSinceScan >= 600u;
    if (!settingsChanged && generation == g_audioAppliedGeneration
        && !periodicRescan) {
        return;
    }
    s_dispatchesSinceScan = 0;
    const LONG soundMb = volumeToMillibels(g_soundVolume);
    const LONG musicMb = volumeToMillibels(g_musicVolume);
    uint32_t applied = 0;
    uint32_t skipped = 0;
    const auto reconcile = [&](uint32_t index, uintptr_t buffer, LONG mb,
                               const char* what) {
        if (buffer != s_applied[index]) {
            s_applied[index] = buffer;
            s_failed[index] = false;
        } else if (!settingsChanged) {
            return;
        }
        if (buffer == 0) return;
        if (s_failed[index]) { ++skipped; return; }
        long hr = 0;
        const VolumeApplyResult result = bufferSetVolume(buffer, mb, hr);
        if (result == VolumeApplyResult::Applied) { ++applied; return; }
        s_failed[index] = true;
        if (++g_audioVolumeFailures <= 16u
            || g_audioVolumeFailures % 500u == 0u) {
            QLOG("[audio] volume apply FAILED #%llu %s slot=%u buffer=%08X "
                 "reason=%s hr=0x%08lX (cached as bad; playback continues at "
                 "the game's own level)",
                 static_cast<unsigned long long>(g_audioVolumeFailures), what,
                 index, static_cast<unsigned>(buffer),
                 volumeApplyResultName(result),
                 static_cast<unsigned long>(hr));
        }
    };
    for (uint32_t i = 0; i < 256u; ++i)
        reconcile(i, rd32(kSfxBaseBufferTable + i * 4u), soundMb, "sfx");
    for (uint32_t i = 0; i < 64u; ++i)
        reconcile(256u + i, rd32(kSfxDuplicateTable + i * 4u), soundMb, "dup");
    reconcile(320u, rd32(kBgmStreamBufferAlias), musicMb, "bgm");
    g_audioAppliedGeneration = generation;
    s_appliedMusic = g_musicVolume;
    s_appliedSound = g_soundVolume;
    static uint64_t s_sweeps = 0;
    if (++s_sweeps <= 8u || (applied != 0 && s_sweeps % 200u == 0u)) {
        QLOG("[audio] volume reconcile #%llu generation=%ld applied=%u "
             "skippedBad=%u music=%u sound=%u",
             static_cast<unsigned long long>(s_sweeps),
             static_cast<long>(generation), applied, skipped,
             g_musicVolume, g_soundVolume);
    }
}
static bool g_audioOpsConfigured = false;
static qaudio::AudioHorizonConfig g_audioV2Horizons{};
typedef int (__fastcall* SoundUnloadFn)(int sound);
typedef int (__fastcall* BgmCommandFn)(int trackId, int flags);
typedef int (__fastcall* AudioTeardownFn)();
typedef int (__fastcall* AudioInitFn)(void* hwnd);
static SoundUnloadFn   g_origSoundUnload = nullptr;
static BgmCommandFn    g_origBgmCommand = nullptr;
static AudioTeardownFn g_origAudioTeardown = nullptr;
static AudioInitFn     g_origAudioInit = nullptr;
static bool g_soundUnloadHooked = false;
static bool g_bgmCommandHooked = false;
static bool g_audioTeardownHooked = false;
static bool g_audioInitHooked = false;
// BGM code range: sub_46FB00 (status service) and sub_46FBC0 (command), ending at sub_4700B0.
constexpr uint32_t kBgmInternalRangeBegin = static_cast<uint32_t>(
    generated_model::nativeSymbolNovemberVa(
        generated_model::NativeSymbolId::BgmInternalRangeBegin));
constexpr uint32_t kBgmInternalRangeEnd = static_cast<uint32_t>(
    generated_model::nativeSymbolNovemberVa(
        generated_model::NativeSymbolId::BgmInternalRangeEnd));
static_assert(kBgmInternalRangeBegin == 0x46FB00u
                  && kBgmInternalRangeEnd == 0x4700B0u,
              "the BGM-internal return-address range must keep the exact "
              "bounds the caller classifier was verified against");

inline bool audioHooksReady() {
    return g_soundLoadHooked
        && g_soundDuplicatePlayHooked && g_soundDirectPlayHooked
        && g_soundStopHooked && g_soundStatusHooked
        && (!g_audioV2Enabled
            || (g_soundUnloadHooked && g_bgmCommandHooked
                && g_audioTeardownHooked && g_audioInitHooked));
}

void netLogAudioLedger() {
    if (!g_audioV2Enabled || !g_audioOpsConfigured) return;
    char audioLedger[768];
    qaudio::runtime().formatLedger(audioLedger, sizeof(audioLedger));
    QLOG("%s", audioLedger);
}
inline bool playbackSideEffectHooksReady() {
    return g_paletteHooked && audioHooksReady();
}

constexpr uint64_t kVirtualOneShotSoundFrames =
    qgame::kFallbackSoundDurationFrames;
constexpr uint64_t kVirtualSoundLooping = UINT64_MAX;
struct VirtualAudioState {
    std::array<uint64_t, qgame::kSimulationStatusSoundIds.size()> statusUntil{};
};
static VirtualAudioState g_virtualAudio{};
static std::array<uint32_t, qgame::kSimulationStatusSoundIds.size()>
    g_statusSoundDurationFrames{};
static std::array<bool, qgame::kSimulationStatusSoundIds.size()>
    g_soundMetadataFailureLogged{};
static bool g_audioManifestChecked = false;
static uint64_t g_audioManifestHash = 0;
static bool g_audioManifestLegacyChecked = false;
static uint64_t g_audioManifestLegacyHash = 0;

inline VirtualAudioState canonicalVirtualAudioForHash(
    const VirtualAudioState& state, uint64_t frame) {
    VirtualAudioState canonical{};
    for (size_t i = 0; i < state.statusUntil.size(); ++i) {
        const uint64_t until = state.statusUntil[i];
        if (until == kVirtualSoundLooping) {
            canonical.statusUntil[i] = kVirtualSoundLooping;
        } else if (until > frame) {
            canonical.statusUntil[i] = until - frame;
        }
    }
    return canonical;
}

inline void resetCanonicalAudioSession() {
    g_virtualAudio = VirtualAudioState{};
    g_statusSoundDurationFrames.fill(0);
    g_soundMetadataFailureLogged.fill(false);
    g_audioManifestChecked = false;
    g_audioManifestHash = 0;
    g_audioManifestLegacyChecked = false;
    g_audioManifestLegacyHash = 0;
}

inline uint64_t virtualSoundDuration(int sound) {
    return qgame::canonicalStatusSoundFrames(sound);
}
inline uint64_t virtualSoundEnd(int sound, int flags) {
    return (flags & 1) != 0
        ? kVirtualSoundLooping
        : g_ovrFrame + virtualSoundDuration(sound);
}
inline void virtualPlayBase(int sound, int flags) {
    if (!g_netCanonicalPoll) return;
    const int index = qgame::simulationStatusSoundIndex(sound);
    if (index < 0) return;
    g_virtualAudio.statusUntil[static_cast<size_t>(index)] =
        virtualSoundEnd(sound, flags);
}
inline void virtualStopSound(int slot, int domain) {
    if (!g_netCanonicalPoll || domain != 2) return;
    const int index = qgame::simulationStatusSoundIndex(slot);
    if (index < 0) return;
    g_virtualAudio.statusUntil[static_cast<size_t>(index)] = 0;
}
inline int virtualSoundStatus(int slot, int domain) {
    if (domain != 2) {
        return domain == 1 || slot < 0 ? 0 : (slot & 1);
    }
    const int index = qgame::simulationStatusSoundIndex(slot);
    if (index < 0) return 0;
    const uint64_t until =
        g_virtualAudio.statusUntil[static_cast<size_t>(index)];
    return until == kVirtualSoundLooping || g_ovrFrame < until ? 1 : 0;
}

inline uint32_t waveLe32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8)
        | (static_cast<uint32_t>(bytes[2]) << 16)
        | (static_cast<uint32_t>(bytes[3]) << 24);
}
bool readWaveDurationFrames(const char* path, uint32_t& framesOut) {
    framesOut = 0;
    if (!path || !path[0]) return false;
    HANDLE file = CreateFileA(
        path, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    uint8_t riff[12] = {};
    DWORD read = 0;
    bool ok = GetFileSizeEx(file, &fileSize)
        && fileSize.QuadPart >= static_cast<LONGLONG>(sizeof(riff))
        && ReadFile(file, riff, sizeof(riff), &read, nullptr)
        && read == sizeof(riff)
        && std::memcmp(riff, "RIFF", 4) == 0
        && std::memcmp(riff + 8, "WAVE", 4) == 0;
    uint32_t averageBytesPerSecond = 0;
    uint32_t dataBytes = 0;
    uint64_t offset = sizeof(riff);
    while (ok && offset + 8u <= static_cast<uint64_t>(fileSize.QuadPart)
           && (averageBytesPerSecond == 0 || dataBytes == 0)) {
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        uint8_t chunk[8] = {};
        ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
            && ReadFile(file, chunk, sizeof(chunk), &read, nullptr)
            && read == sizeof(chunk);
        if (!ok) break;
        const uint32_t chunkBytes = waveLe32(chunk + 4);
        const uint64_t payload = offset + 8u;
        const uint64_t next = qgame::waveChunkNextOffset(
            payload, chunkBytes,
            static_cast<uint64_t>(fileSize.QuadPart));
        if (next == UINT64_MAX) {
            ok = false;
            break;
        }
        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkBytes >= 16u) {
            position.QuadPart = static_cast<LONGLONG>(payload);
            uint8_t format[16] = {};
            ok = SetFilePointerEx(file, position, nullptr, FILE_BEGIN)
                && ReadFile(file, format, sizeof(format), &read, nullptr)
                && read == sizeof(format);
            if (ok) averageBytesPerSecond = waveLe32(format + 8);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            dataBytes = chunkBytes;
        }
        offset = next;
    }
    CloseHandle(file);
    if (!ok || averageBytesPerSecond == 0 || dataBytes == 0)
        return false;
    framesOut =
        qgame::waveDurationFrames(dataBytes, averageBytesPerSecond);
    return true;
}

enum class AudioCallKind : uint8_t {
    DuplicatePlay = 1,
    DirectPlay = 2,
    Stop = 3,
    Status = 4,
};
constexpr int kAudioRingCap = 128;
constexpr int kAudioRingSlots = 16;
struct AudioCallResult {
    AudioCallKind kind = AudioCallKind::DuplicatePlay;
    int value = 0;
};
struct AudioRing {
    AudioCallResult calls[kAudioRingCap]{};
    int n = 0;
    int idx = 0;
};
static AudioRing g_audioRings[kAudioRingSlots];
static AudioRing* g_audioActive = nullptr;
static volatile int g_audioMode = 0;
inline void audioBeginRecord(int slot) {
    g_audioActive = &g_audioRings[slot % kAudioRingSlots];
    g_audioActive->n = 0;
    g_audioMode = 1;
}
inline void audioBeginReplay(int slot) {
    g_audioActive = &g_audioRings[slot % kAudioRingSlots];
    g_audioActive->idx = 0;
    g_audioMode = 2;
}
inline void audioOff() {
    g_audioMode = 0;
    g_audioActive = nullptr;
}
inline void audioRecord(AudioCallKind kind, int value) {
    if (g_audioMode != 1 || !g_audioActive
        || g_audioActive->n >= kAudioRingCap)
        return;
    g_audioActive->calls[g_audioActive->n++] = { kind, value };
}
inline bool audioReplayResult(AudioCallKind kind, int& value) {
    if (g_audioMode != 2 || !g_audioActive
        || g_audioActive->idx >= g_audioActive->n)
        return false;
    const AudioCallResult& call =
        g_audioActive->calls[g_audioActive->idx++];
    if (call.kind != kind) return false;
    value = call.value;
    return true;
}

int callNativePalettePreservingFpu(int palette, int first, int apply) {
    if (!g_origPaletteApply) return 1;
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const int result = g_origPaletteApply(palette, first, apply);
    fxRestoreFrom(before);
    return result;
}
int callNativeSoundLoadPreservingFpu(int sound, const char* path) {
    if (!g_origSoundLoad) return 0;
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const ULONGLONG started = GetTickCount64();
    const int result = g_origSoundLoad(sound, path);
    const ULONGLONG elapsedMs = GetTickCount64() - started;
    fxRestoreFrom(before);
    if (elapsedMs >= 25u) {
        QLOG("[audio-load] slow native load slot=%d elapsedMs=%llu path=%s",
             sound, static_cast<unsigned long long>(elapsedMs),
             path != nullptr ? path : "<null>");
    }
    return result;
}
int callNativeSoundDuplicatePlayPreservingFpu(
    int sound, unsigned int slot, int voices, int flags) {
    if (!g_origSoundDuplicatePlay) return static_cast<int>(slot);
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const int result =
        g_origSoundDuplicatePlay(sound, slot, voices, flags);
    fxRestoreFrom(before);
    return result;
}
int callNativeSoundDirectPlayPreservingFpu(int sound, int flags) {
    if (!g_origSoundDirectPlay) return 1;
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const ULONGLONG started = GetTickCount64();
    const int result = g_origSoundDirectPlay(sound, flags);
    const ULONGLONG elapsedMs = GetTickCount64() - started;
    fxRestoreFrom(before);
    if (elapsedMs >= 25u) {
        QLOG("[audio-play] slow native base play frame=%u slot=%d flags=%d "
             "elapsedMs=%llu scene=%u scene3Sub=%u",
             static_cast<uint32_t>(g_ovrFrame), sound, flags,
             static_cast<unsigned long long>(elapsedMs),
             rd32(kSceneStateAddr), rd32(addr::kScene3Substate));
    }
    return result;
}
int callNativeSoundStopPreservingFpu(int slot, int domain) {
    if (!g_origSoundStop) return 1;
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const ULONGLONG started = GetTickCount64();
    const int result = g_origSoundStop(slot, domain);
    const ULONGLONG elapsedMs = GetTickCount64() - started;
    fxRestoreFrom(before);
    if (elapsedMs >= 25u) {
        QLOG("[audio-stop] slow native stop frame=%u slot=%d domain=%d "
             "elapsedMs=%llu scene=%u scene3Sub=%u",
             static_cast<uint32_t>(g_ovrFrame), slot, domain,
             static_cast<unsigned long long>(elapsedMs),
             rd32(kSceneStateAddr), rd32(addr::kScene3Substate));
    }
    return result;
}
int callNativeSoundStatusPreservingFpu(int slot, int domain) {
    if (!g_origSoundStatus) return 0;
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const int result = g_origSoundStatus(slot, domain);
    fxRestoreFrom(before);
    return result;
}
int callNativeSoundUnloadPreservingFpu(int sound) {
    if (!g_origSoundUnload) return 1;
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const int result = g_origSoundUnload(sound);
    fxRestoreFrom(before);
    return result;
}
int callNativeBgmPreservingFpu(int trackId, int flags) {
    if (!g_origBgmCommand) return 1;
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const int result = g_origBgmCommand(trackId, flags);
    fxRestoreFrom(before);
    return result;
}
int callNativeAudioTeardownPreservingFpu() {
    if (!g_origAudioTeardown) return 0;
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const int result = g_origAudioTeardown();
    fxRestoreFrom(before);
    return result;
}
int callNativeAudioInitPreservingFpu(void* hwnd) {
    if (!g_origAudioInit) return 0;
    alignas(16) uint8_t before[512];
    fxSaveTo(before);
    const int result = g_origAudioInit(hwnd);
    fxRestoreFrom(before);
    return result;
}

void warmNativeDirectSoundDevice() {
    constexpr uintptr_t kDirectSoundDeviceAddress = 0x4AAAB4u;
    constexpr DWORD kWarmBufferBytes = 256u;

    const DWORD savedLastError = GetLastError();
    alignas(16) uint8_t savedFpu[512];
    fxSaveTo(savedFpu);
    const ULONGLONG started = GetTickCount64();

    IDirectSound* device = reinterpret_cast<IDirectSound*>(
        static_cast<uintptr_t>(rd32(kDirectSoundDeviceAddress)));
    HRESULT createResult = DSERR_UNINITIALIZED;
    HRESULT lockResult = DSERR_UNINITIALIZED;
    HRESULT playResult = DSERR_UNINITIALIZED;
    HRESULT stopResult = DSERR_UNINITIALIZED;

    if (device != nullptr) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1u;
        format.nSamplesPerSec = 11025u;
        format.wBitsPerSample = 16u;
        format.nBlockAlign = static_cast<WORD>(
            format.nChannels * format.wBitsPerSample / 8u);
        format.nAvgBytesPerSec =
            format.nSamplesPerSec * format.nBlockAlign;

        DSBUFFERDESC description{};
        description.dwSize = sizeof(description);
        description.dwFlags = DSBCAPS_STATIC;
        description.dwBufferBytes = kWarmBufferBytes;
        description.lpwfxFormat = &format;

        IDirectSoundBuffer* buffer = nullptr;
        createResult = device->CreateSoundBuffer(
            &description, &buffer, nullptr);
        if (SUCCEEDED(createResult) && buffer != nullptr) {
            void* first = nullptr;
            void* second = nullptr;
            DWORD firstBytes = 0u;
            DWORD secondBytes = 0u;
            lockResult = buffer->Lock(
                0u, 0u, &first, &firstBytes, &second, &secondBytes,
                DSBLOCK_ENTIREBUFFER);
            if (SUCCEEDED(lockResult)) {
                if (first != nullptr && firstBytes != 0u)
                    std::memset(first, 0, firstBytes);
                if (second != nullptr && secondBytes != 0u)
                    std::memset(second, 0, secondBytes);
                (void)buffer->Unlock(
                    first, firstBytes, second, secondBytes);
                playResult = buffer->Play(0u, 0u, 0u);
                if (SUCCEEDED(playResult))
                    stopResult = buffer->Stop();
            }
            buffer->Release();
        }
    }

    const ULONGLONG elapsedMs = GetTickCount64() - started;
    fxRestoreFrom(savedFpu);
    QLOG("[audio-warmup] silent device activation device=%p create=0x%08lX "
         "lock=0x%08lX play=0x%08lX stop=0x%08lX elapsedMs=%llu",
         static_cast<void*>(device), static_cast<unsigned long>(createResult),
         static_cast<unsigned long>(lockResult),
         static_cast<unsigned long>(playResult),
         static_cast<unsigned long>(stopResult),
         static_cast<unsigned long long>(elapsedMs));
    SetLastError(savedLastError);
}

int qohAudioOpLoad(int sample, const char* path) {
    return callNativeSoundLoadPreservingFpu(sample, path);
}
int qohAudioOpUnload(int sample) {
    return callNativeSoundUnloadPreservingFpu(sample);
}
int qohAudioOpPlayDuplicate(int sample, unsigned int slot, int voices,
                            int flags) {
    return callNativeSoundDuplicatePlayPreservingFpu(
        sample, slot, voices, flags);
}
int qohAudioOpPlayBase(int sample, int flags) {
    return callNativeSoundDirectPlayPreservingFpu(sample, flags);
}
int qohAudioOpStop(int index, int domain) {
    return callNativeSoundStopPreservingFpu(index, domain);
}
int qohAudioOpBgm(int trackId, int flags) {
    return callNativeBgmPreservingFpu(trackId, flags);
}
// IDirectSoundBuffer::SetCurrentPosition is vtable slot 13; the buffer tables hold live pointers that can be stale.
int qohAudioOpSetCursorSeh(void* buffer, uint32_t cursorBytes) {
    typedef long(__stdcall * SetCurrentPositionFn)(void*, unsigned long);
    __try {
        void** vtbl = *reinterpret_cast<void***>(buffer);
        const long hr = reinterpret_cast<SetCurrentPositionFn>(vtbl[13])(
            buffer, cursorBytes);
        return hr >= 0 ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
int qohAudioOpSetCursor(const qaudio::AudioEndpoint& endpoint,
                        uint32_t cursorBytes) {
    const uintptr_t table =
        endpoint.type == qaudio::AudioEndpoint::Type::Duplicate
            ? 0x4AAC0Cu + endpoint.index * 4u
            : 0x4AAD14u + endpoint.index * 4u;
    void* buffer = reinterpret_cast<void*>(
        static_cast<uintptr_t>(rd32(table)));
    if (buffer == nullptr) return 0;
    return qohAudioOpSetCursorSeh(buffer, cursorBytes);
}

struct WaveParseEntry {
    uint64_t pathHash = 0;
    bool used = false;
    bool success = false;
    uint32_t durationFrames = 0;
    qaudio::WaveFacts facts{};
};
constexpr size_t kWaveParseCacheSlots = 512;
static WaveParseEntry g_waveParseCache[kWaveParseCacheSlots];
static uint64_t g_waveParseHistoricalMisses = 0;

bool audioWaveParse(const char* path, bool allowFilesystem, bool& success,
                    uint32_t& durationFrames, qaudio::WaveFacts& facts) {
    success = false;
    durationFrames = 0;
    facts = qaudio::WaveFacts{};
    if (path == nullptr || path[0] == '\0') return true;
    const uint64_t hash = qaudio::normalizedPathHash(path);
    const size_t start = static_cast<size_t>(hash % kWaveParseCacheSlots);
    size_t firstFree = kWaveParseCacheSlots;
    for (size_t probe = 0; probe < kWaveParseCacheSlots; ++probe) {
        const size_t slot = (start + probe) % kWaveParseCacheSlots;
        WaveParseEntry& entry = g_waveParseCache[slot];
        if (!entry.used) {
            firstFree = slot;
            break;
        }
        if (entry.pathHash == hash) {
            success = entry.success;
            durationFrames = entry.durationFrames;
            facts = entry.facts;
            return true;
        }
    }
    if (!allowFilesystem) {
        ++g_waveParseHistoricalMisses;
        return false;
    }
    const DWORD nativeLastError = GetLastError();
    uint32_t duration = 0;
    const bool parsed = readWaveDurationFrames(path, duration);
    qaudio::WaveFacts parsedFacts = qaudio::loadWaveFactsFromFile(path);
    SetLastError(nativeLastError);
    if (firstFree != kWaveParseCacheSlots) {
        WaveParseEntry& entry = g_waveParseCache[firstFree];
        entry.pathHash = hash;
        entry.used = true;
        entry.success = parsed;
        entry.durationFrames = parsed ? duration : 0u;
        entry.facts = parsedFacts;
    }
    success = parsed;
    durationFrames = parsed ? duration : 0u;
    facts = parsedFacts;
    return true;
}

void netFailClosed(uint32_t code, uint32_t detail, const char* why,
                   bool notifyPeer, bool graceful);
inline void netFailClosedFromAudioHook(const char* why) {
    netFailClosed(kNetInternalFailure, g_ovrFrame, why, true, false);
}

int __fastcall hkQohScoreDraw(int slot, int playerMask, unsigned int score,
                              int altX, int x) {
    if (!g_origScoreDraw) return 0;
    const qgame::NativeHudOfflineOwner owner = nativeHudOfflineOwner();
    if (owner != qgame::NativeHudOfflineOwner::None) {
        const qgame::NativeHudEffectiveMasks masks =
            nativeHudEffectiveMasksFor(owner);
        const uint8_t side = slot == 0 ? 0u : 1u;
        if (qgame::nativeHudScoreSideHidden(masks, side)) return 0;
        return g_origScoreDraw(slot, playerMask, score, altX, x);
    }
    if (g_hideNativeScore) return 0;
    return g_origScoreDraw(slot, playerMask, score, altX, x);
}

void* __cdecl hkQohTimeDraw() {
    if (!g_origTimeDraw) return nullptr;
    const qgame::NativeHudOfflineOwner owner = nativeHudOfflineOwner();
    if (owner != qgame::NativeHudOfflineOwner::None) {
        if (qgame::nativeHudBottomTimeHidden(nativeHudEffectiveMasksFor(owner)))
            return nullptr;
        return g_origTimeDraw();
    }
    if (g_hideNativeTimer) return nullptr;
    return g_origTimeDraw();
}

// Descriptor node: arena + 44*count + 100408; next at +100412; bump count at arena + 100404.
struct NativeHudDescriptorTransaction {
    uintptr_t arena = 0u;
    uintptr_t headAddr = 0u;
    uint32_t glyphCount = 0u;
    uint32_t spriteCount = 0u;
    uint32_t head = 0u;
    bool valid = false;
};

constexpr uintptr_t kNativeHudGlyphCountOffset = 100404u;   // Stride 44 at offset 100408.
constexpr uintptr_t kNativeHudSpriteCountOffset = 85524u;   // Stride 104 at offset 68884.

// dword_6177CC is a pointer variable: the list head lives at rd32(0x6177CC) + 4.
constexpr uintptr_t kNativeHudListHeadPtr = qoh99::addr::kArenaMirrorPtr;
static_assert(kNativeHudListHeadPtr == 0x6177CCu,
              "HUD descriptor list head base moved");

static NativeHudDescriptorTransaction nativeHudBeginDescriptors() {
    NativeHudDescriptorTransaction tx{};
    tx.arena = rd32(qoh99::addr::kArenaActivePtr);
    if (tx.arena == 0u) return tx;
    const uintptr_t listBase = rd32(kNativeHudListHeadPtr);
    if (listBase == 0u) return tx;
    tx.headAddr = listBase + 4u;
    tx.glyphCount = rd32(tx.arena + kNativeHudGlyphCountOffset);
    tx.spriteCount = rd32(tx.arena + kNativeHudSpriteCountOffset);
    tx.head = rd32(tx.headAddr);
    tx.valid = true;
    return tx;
}

static void nativeHudDiscardDescriptors(
        const NativeHudDescriptorTransaction& tx) {
    if (!tx.valid) return;
    if (rd32(tx.headAddr) != tx.head) wr32(tx.headAddr, tx.head);
    if (rd32(tx.arena + kNativeHudGlyphCountOffset) > tx.glyphCount)
        wr32(tx.arena + kNativeHudGlyphCountOffset, tx.glyphCount);
    if (rd32(tx.arena + kNativeHudSpriteCountOffset) > tx.spriteCount)
        wr32(tx.arena + kNativeHudSpriteCountOffset, tx.spriteCount);
}

// sub_42B0D0 advances the EX spend transaction, so the original must always run.
int __fastcall hkQohExDraw(int fighter, uint32_t* dest) {
    if (!g_origExDraw) return 0;
    const qgame::NativeHudOfflineOwner owner = nativeHudOfflineOwner();
    bool hide = false;
    if (owner != qgame::NativeHudOfflineOwner::None) {
        const uintptr_t arena = rd32(qoh99::addr::kArenaActivePtr);
        const uintptr_t got = reinterpret_cast<uintptr_t>(dest);
        if (arena != 0u
            && (got == arena + 88400u || got == arena + 88488u)) {
            hide = qgame::nativeHudExSideHidden(
                nativeHudEffectiveMasksFor(owner),
                got == arena + 88400u ? 0u : 1u);
        }
    }
    if (!hide) return g_origExDraw(fighter, dest);
    const NativeHudDescriptorTransaction tx = nativeHudBeginDescriptors();
    const int result = g_origExDraw(fighter, dest);
    nativeHudDiscardDescriptors(tx);
    return result;
}

int __fastcall hkQohComboDraw(int slot) {
    if (!g_origComboDraw) return 0;
    const qgame::NativeHudOfflineOwner owner = nativeHudOfflineOwner();
    const bool hide = owner != qgame::NativeHudOfflineOwner::None
        && (slot == 0 || slot == 1)
        && qgame::nativeHudComboSideHidden(nativeHudEffectiveMasksFor(owner),
                                           static_cast<uint8_t>(slot));
    if (!hide) return g_origComboDraw(slot);
    const NativeHudDescriptorTransaction tx = nativeHudBeginDescriptors();
    const int result = g_origComboDraw(slot);
    nativeHudDiscardDescriptors(tx);
    return result;
}

void* __fastcall hkQohHudEmit(int a1, int a2, uint32_t* table,
                                          int selector) {
    if (!g_origHudEmit) return nullptr;
    const qgame::NativeHudOfflineOwner owner = nativeHudOfflineOwner();
    bool hide = false;
    if (owner != qgame::NativeHudOfflineOwner::None) {
        const uintptr_t got = reinterpret_cast<uintptr_t>(table);
        const qgame::NativeHudEffectiveMasks masks =
            nativeHudEffectiveMasksFor(owner);
        if (got == 0x49B4E8u) hide = qgame::nativeHudFpsHidden(masks);
        else if (got == 0x49B4A0u) hide = qgame::nativeHudTopRoundTimerHidden(
            masks);
    }
    if (!hide) return g_origHudEmit(a1, a2, table, selector);
    const NativeHudDescriptorTransaction tx = nativeHudBeginDescriptors();
    void* result = g_origHudEmit(a1, a2, table, selector);
    nativeHudDiscardDescriptors(tx);
    return result;
}

int __fastcall hkQohGuardDraw(int fighterA, int fighterB) {
    if (!g_origGuardDraw) return 0;
    const qgame::NativeHudOfflineOwner owner = nativeHudOfflineOwner();
    if (owner != qgame::NativeHudOfflineOwner::None
        && qgame::nativeHudGuardProducerHidden(
               nativeHudEffectiveMasksFor(owner))) {
        return 0;
    }
    return g_origGuardDraw(fighterA, fighterB);
}

int __fastcall hkQohPortraitLifeDraw(int fighter, uint32_t* dest) {
    if (!g_origPortraitLifeDraw) return 0;
    const qgame::NativeHudOfflineOwner owner = nativeHudOfflineOwner();
    if (owner != qgame::NativeHudOfflineOwner::None) {
        const uintptr_t arena = rd32(qoh99::addr::kArenaActivePtr);
        const uintptr_t left = arena + 87608u;
        const uintptr_t right = arena + 87740u;
        const uintptr_t got = reinterpret_cast<uintptr_t>(dest);
        if (arena != 0u && (got == left || got == right)) {
            const uint8_t side = got == left ? 0u : 1u;
            if (qgame::nativeHudPortraitLifeSideHidden(
                    nativeHudEffectiveMasksFor(owner), side)) {
                return 0;
            }
        }
    }
    return g_origPortraitLifeDraw(fighter, dest);
}

int __fastcall hkQohNameDraw(int index, int characterId, int x) {
    if (!g_origNameDraw) return 0;
    const qgame::NativeHudOfflineOwner owner = nativeHudOfflineOwner();
    if (owner != qgame::NativeHudOfflineOwner::None) {
        if (index == 0 || index == 1 || index == 2 || index == 4) {
            const uint8_t side = (index == 0 || index == 2) ? 0u : 1u;
            if (qgame::nativeHudNameSideHidden(
                    nativeHudEffectiveMasksFor(owner), side)) {
                return 0;
            }
        }
    }
    return g_origNameDraw(index, characterId, x);
}

void __fastcall hkQohRoundWinsDraw(uint32_t* table, int wins, int index, int x,
                                   int y, int step) {
    if (!g_origRoundWinsDraw) return;
    const qgame::NativeHudOfflineOwner owner = nativeHudOfflineOwner();
    if (owner != qgame::NativeHudOfflineOwner::None
        && (index == 0 || index == 4)) {
        const uint8_t side = index == 0 ? 0u : 1u;
        if (qgame::nativeHudRoundWinsSideHidden(
                nativeHudEffectiveMasksFor(owner), side)) {
            return;
        }
    }
    g_origRoundWinsDraw(table, wins, index, x, y, step);
}

void installModularNativeHudHooksOnce();

void installNativeHudHooksOnce() {
    if (g_hideNativeScore && !g_scoreDrawHooked) {
        static const uint8_t kScoreSig[] = {
            0xA1, 0xCC, 0x36, 0x62, 0x00, 0x56, 0x23, 0xC2,
            0x3B, 0xC2, 0x0F, 0x85, 0x06, 0x01, 0x00, 0x00
        };
        void* t = reinterpret_cast<void*>(g_nativeHudScoreDrawFn);
        if (!g_nativeHudScoreDrawFn
            || !se::verifyCodeBytes(g_nativeHudScoreDrawFn, kScoreSig,
                                    sizeof(kScoreSig))) {
            QLOG("[native-hud] prologue mismatch at score sub_42A990 - not hooked");
        } else if (MH_CreateHook(t, reinterpret_cast<void*>(&hkQohScoreDraw),
                                 reinterpret_cast<void**>(&g_origScoreDraw)) == MH_OK
                   && MH_QueueEnableHook(t) == MH_OK) {
            g_scoreDrawHooked = true;
            QLOG("[native-hud] 1P/2P SCORE readout suppressed (sub_42A990)");
        } else {
            QLOG("[native-hud] could not hook score sub_42A990 (visible)");
        }
    }
    if (g_hideNativeTimer && !g_timeDrawHooked) {
        static const uint8_t kTimeSig[] = {
            0x8B, 0x0D, 0xC0, 0x3D, 0x55, 0x00, 0xB8, 0xD5,
            0x52, 0x04, 0xE9, 0xF7, 0xE1, 0xC1, 0xEA, 0x0F
        };
        void* t = reinterpret_cast<void*>(g_nativeHudTimeDrawFn);
        if (!g_nativeHudTimeDrawFn
            || !se::verifyCodeBytes(g_nativeHudTimeDrawFn, kTimeSig,
                                    sizeof(kTimeSig))) {
            QLOG("[native-hud] prologue mismatch at time sub_42C040 - not hooked");
        } else if (MH_CreateHook(t, reinterpret_cast<void*>(&hkQohTimeDraw),
                                 reinterpret_cast<void**>(&g_origTimeDraw)) == MH_OK
                   && MH_QueueEnableHook(t) == MH_OK) {
            g_timeDrawHooked = true;
            QLOG("[native-hud] TIME readout suppressed (sub_42C040)");
        } else {
            QLOG("[native-hud] could not hook time sub_42C040 (visible)");
        }
    }
    installModularNativeHudHooksOnce();
}

void installModularNativeHudHooksOnce() {
    if (!g_nativeHudModularEligible) return;

    struct ModularHud {
        const char* name;
        uintptr_t address;
        const uint8_t* signature;
        void* detour;
        void** trampoline;
        bool* installed;
    };
    static const uint8_t kGuardSig[] = {
        0x83, 0xEC, 0x10, 0x33, 0xC0, 0x89, 0x54, 0x24,
        0x08, 0x8A, 0x82, 0xAC, 0x00, 0x00, 0x00, 0x53};
    static const uint8_t kPortraitLifeSig[] = {
        0x83, 0xEC, 0x1C, 0xA1, 0xE0, 0x3D, 0x61, 0x00,
        0x53, 0x55, 0x56, 0x8B, 0x40, 0x2C, 0x8B, 0xF2};
    static const uint8_t kNameSig[] = {
        0x8B, 0xC2, 0x8B, 0x15, 0xE8, 0x34, 0x62, 0x00,
        0x8B, 0x8A, 0x34, 0x88, 0x01, 0x00, 0x81, 0xF9};
    static const uint8_t kRoundWinsSig[] = {
        0x51, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xEA, 0x33,
        0xFF, 0x3B, 0xEF, 0x89, 0x6C, 0x24, 0x10, 0x0F};
    static const uint8_t kExSig[] = {
        0x83, 0xEC, 0x24, 0x8A, 0x81, 0xAC, 0x00, 0x00,
        0x00, 0x53, 0x55, 0x33, 0xDB, 0x56, 0x57, 0x3A};
    static const uint8_t kComboSig[] = {
        0x83, 0xEC, 0x20, 0x53, 0x56, 0x83, 0xF9, 0x01,
        0x57, 0x89, 0x4C, 0x24, 0x28, 0x75, 0x3A, 0x8B};
    static const uint8_t kHudEmitSig[] = {
        0x8B, 0x44, 0x24, 0x08, 0x85, 0xC0, 0x0F, 0x8E,
        0x9E, 0x01, 0x00, 0x00, 0x55, 0x56, 0x57, 0x8B};

    const ModularHud hooks[] = {
        {"guard sub_41F290", 0x41F290u, kGuardSig,
         reinterpret_cast<void*>(&hkQohGuardDraw),
         reinterpret_cast<void**>(&g_origGuardDraw), &g_guardDrawHooked},
        {"portrait/life sub_42AE60", 0x42AE60u, kPortraitLifeSig,
         reinterpret_cast<void*>(&hkQohPortraitLifeDraw),
         reinterpret_cast<void**>(&g_origPortraitLifeDraw),
         &g_portraitLifeDrawHooked},
        {"names sub_42BB30", 0x42BB30u, kNameSig,
         reinterpret_cast<void*>(&hkQohNameDraw),
         reinterpret_cast<void**>(&g_origNameDraw), &g_nameDrawHooked},
        {"round wins sub_42BD60", 0x42BD60u, kRoundWinsSig,
         reinterpret_cast<void*>(&hkQohRoundWinsDraw),
         reinterpret_cast<void**>(&g_origRoundWinsDraw),
         &g_roundWinsDrawHooked},
        {"EX sub_42B0D0", 0x42B0D0u, kExSig,
         reinterpret_cast<void*>(&hkQohExDraw),
         reinterpret_cast<void**>(&g_origExDraw), &g_exDrawHooked},
        {"combo sub_40BC40", 0x40BC40u, kComboSig,
         reinterpret_cast<void*>(&hkQohComboDraw),
         reinterpret_cast<void**>(&g_origComboDraw), &g_comboDrawHooked},
        {"hud emitter sub_42A5C0", 0x42A5C0u, kHudEmitSig,
         reinterpret_cast<void*>(&hkQohHudEmit),
         reinterpret_cast<void**>(&g_origHudEmit), &g_hudEmitHooked},
    };

    for (const ModularHud& hook : hooks) {
        if (*hook.installed) continue;
        void* target = reinterpret_cast<void*>(hook.address);
        if (!se::verifyCodeBytes(hook.address, hook.signature, 16u)) {
            QLOG("[native-hud] prologue mismatch at %s - not hooked "
                 "(element stays visible)", hook.name);
            continue;
        }
        if (MH_CreateHook(target, hook.detour, hook.trampoline) == MH_OK
            && MH_QueueEnableHook(target) == MH_OK) {
            *hook.installed = true;
            QLOG("[native-hud] modular detour armed: %s", hook.name);
        } else {
            QLOG("[native-hud] could not hook %s (element stays visible)",
                 hook.name);
        }
    }
}

inline bool deterministicAudioRoleActive() {
    return g_netActive || g_replayRoleActive;
}

typedef int(__fastcall* GameErrorWriteFn)(const char* text, void* edx);
static GameErrorWriteFn g_origGameErrorWrite = nullptr;
static bool g_gameErrorWriteHooked = false;

static int __fastcall hkGameErrorWrite(const char* text, void* edx) {
    QLOG_ISSUE("[game-error] %s", qgame::gameErrorTextUtf8(text).c_str());
    return g_origGameErrorWrite(text, edx);
}

int __fastcall hkPaletteApply(int palette, int first, int apply) {
    const int result =
        callNativePalettePreservingFpu(palette, first, apply);
    return deterministicAudioRoleActive() ? 1 : result;
}
int __fastcall hkSoundLoad(int sound, const char* path) {
    const uint32_t callsiteRva =
        reinterpret_cast<uint32_t>(_ReturnAddress());
    const bool v2 = g_audioV2Enabled
                 && deterministicAudioRoleActive();
    qaudio::QohAudioRuntime& audio = qaudio::runtime();
    const bool v2Capture = v2 && audio.capturesIntent();
    const bool v2Historical = v2 && audio.forbidsNativeAudio();
    const int nativeResult = v2Capture
        ? 0
        : callNativeSoundLoadPreservingFpu(sound, path);
    if (!v2Capture) netAudioNoteBufferGenerationChanged();
    const bool canonical = deterministicAudioRoleActive()
                        && g_netCanonicalPoll;
    const int statusIndex = qgame::simulationStatusSoundIndex(sound);
    uint32_t duration = 0;
    bool parsed = false;
    qaudio::WaveFacts facts{};
    if (canonical || statusIndex >= 0 || g_audioV2Enabled) {
        if (!audioWaveParse(path, !v2Historical, parsed, duration, facts)
            && canonical) {
            netFailClosedFromAudioHook(
                "historical audio load missed the parse cache");
            return 0;
        }
    }
    if (statusIndex >= 0) {
        const size_t index = static_cast<size_t>(statusIndex);
        g_statusSoundDurationFrames[index] = parsed ? duration : 0u;
        if (g_netActive && !parsed
            && !g_soundMetadataFailureLogged[index]) {
            g_soundMetadataFailureLogged[index] = true;
            QLOG("[presentation] sound %d has no readable WAV timing "
                 "(path=%s); it will not be audible. The match is "
                 "unaffected - the simulation treats every status sound as "
                 "loaded, for a canonical length, on every installation.",
                 sound, path ? path : "<null>");
        }
    }
    if (g_audioV2Enabled && qaudio::validSampleId(sound)) {
        audio.catalog().observeLoad(
            static_cast<int16_t>(sound), path, parsed, facts);
        if (v2Capture) {
            audio.captureLoad(static_cast<int16_t>(sound), path, parsed,
                              audio.catalog().assetOf(
                                  static_cast<int16_t>(sound)),
                              callsiteRva);
        }
    }
    if (statusIndex >= 0 && deterministicAudioRoleActive()) return 1;
    return canonical ? 1 : nativeResult;
}
int __fastcall hkSoundDuplicatePlay(
    int sound, unsigned int slot, int voices, int flags) {
    if (g_audioV2Enabled && deterministicAudioRoleActive()
        && qaudio::runtime().capturesIntent()) {
        qaudio::runtime().capturePlayDuplicate(
            static_cast<int16_t>(sound), static_cast<uint16_t>(slot),
            static_cast<uint16_t>(voices),
            static_cast<uint32_t>(flags),
            reinterpret_cast<uint32_t>(_ReturnAddress()));
        return static_cast<int>(slot);
    }
    if (g_netActive) {
        if (g_netCanonicalPoll && g_replayActive && !g_sfxPassthrough) {
            ++g_audioSuppressed;
        } else {
            (void)callNativeSoundDuplicatePlayPreservingFpu(
                sound, slot, voices, flags);
        }
        return static_cast<int>(slot);
    }
    if (g_audioMode == 1 && g_audioActive) {
        const int result = callNativeSoundDuplicatePlayPreservingFpu(
            sound, slot, voices, flags);
        audioRecord(AudioCallKind::DuplicatePlay, result);
        return result;
    }
    if (g_replayActive) {
        ++g_audioSuppressed;
        if (g_sfxPassthrough) {
            return callNativeSoundDuplicatePlayPreservingFpu(
                sound, slot, voices, flags);
        }
        int result = static_cast<int>(slot);
        (void)audioReplayResult(AudioCallKind::DuplicatePlay, result);
        return result;
    }
    return callNativeSoundDuplicatePlayPreservingFpu(
        sound, slot, voices, flags);
}
int __fastcall hkSoundDirectPlay(int sound, int flags) {
    virtualPlayBase(sound, flags);
    if (g_audioV2Enabled && deterministicAudioRoleActive()
        && qaudio::runtime().capturesIntent()) {
        qaudio::runtime().capturePlayBase(
            static_cast<int16_t>(sound),
            static_cast<uint32_t>(flags),
            reinterpret_cast<uint32_t>(_ReturnAddress()));
        return 1;
    }
    if (g_netActive) {
        if (g_netCanonicalPoll && g_replayActive && !g_sfxPassthrough) {
            ++g_audioSuppressed;
        } else {
            (void)callNativeSoundDirectPlayPreservingFpu(sound, flags);
        }
        return 1;
    }
    if (g_audioMode == 1 && g_audioActive) {
        const int result =
            callNativeSoundDirectPlayPreservingFpu(sound, flags);
        audioRecord(AudioCallKind::DirectPlay, result);
        return result;
    }
    if (g_replayActive) {
        ++g_audioSuppressed;
        if (g_sfxPassthrough)
            return callNativeSoundDirectPlayPreservingFpu(sound, flags);
        int result = 1;
        (void)audioReplayResult(AudioCallKind::DirectPlay, result);
        return result;
    }
    return callNativeSoundDirectPlayPreservingFpu(sound, flags);
}
int __fastcall hkSoundStop(int slot, int domain) {
    virtualStopSound(slot, domain);
    if (g_audioV2Enabled && deterministicAudioRoleActive()
        && qaudio::runtime().capturesIntent()) {
        // domain 1 = duplicate slot, 2 = base sample (sub_45FB60).
        qaudio::AudioEndpoint endpoint{};
        endpoint.type = domain == 1
            ? qaudio::AudioEndpoint::Type::Duplicate
            : qaudio::AudioEndpoint::Type::Base;
        endpoint.index = static_cast<uint16_t>(slot);
        qaudio::runtime().captureStop(
            endpoint, reinterpret_cast<uint32_t>(_ReturnAddress()));
        return 1;
    }
    if (g_netActive) {
        (void)callNativeSoundStopPreservingFpu(slot, domain);
        return 1;
    }
    if (g_audioMode == 1 && g_audioActive) {
        const int result = callNativeSoundStopPreservingFpu(slot, domain);
        audioRecord(AudioCallKind::Stop, result);
        return result;
    }
    if (g_replayActive) {
        (void)callNativeSoundStopPreservingFpu(slot, domain);
        int result = 1;
        (void)audioReplayResult(AudioCallKind::Stop, result);
        return result;
    }
    return callNativeSoundStopPreservingFpu(slot, domain);
}
int __fastcall hkSoundStatus(int slot, int domain) {
    if (deterministicAudioRoleActive() && g_netCanonicalPoll)
        return virtualSoundStatus(slot, domain);
    if (g_audioMode == 1 && g_audioActive) {
        const int result =
            callNativeSoundStatusPreservingFpu(slot, domain);
        audioRecord(AudioCallKind::Status, result);
        return result;
    }
    if (g_replayActive && !g_sfxPassthrough) {
        int result = 0;
        (void)audioReplayResult(AudioCallKind::Status, result);
        return result;
    }
    return callNativeSoundStatusPreservingFpu(slot, domain);
}

int __fastcall hkSoundUnload(int sound) {
    if (g_audioV2Enabled && deterministicAudioRoleActive()) {
        qaudio::QohAudioRuntime& audio = qaudio::runtime();
        if (audio.capturesIntent()) {
            if (qaudio::validSampleId(sound))
                audio.catalog().observeUnload(static_cast<int16_t>(sound));
            audio.captureUnload(
                static_cast<int16_t>(sound),
                reinterpret_cast<uint32_t>(_ReturnAddress()));
            return 1;
        }
    }
    const int result = callNativeSoundUnloadPreservingFpu(sound);
    if (g_audioV2Enabled && qaudio::validSampleId(sound))
        qaudio::runtime().catalog().observeUnload(
            static_cast<int16_t>(sound));
    netAudioNoteBufferGenerationChanged();
    return result;
}

int __fastcall hkBgmCommand(int trackId, int flags) {
    const uint32_t callsiteRva =
        reinterpret_cast<uint32_t>(_ReturnAddress());
    const bool internalBgm = callsiteRva >= kBgmInternalRangeBegin
        && callsiteRva < kBgmInternalRangeEnd;
    if (g_audioV2Enabled && deterministicAudioRoleActive()) {
        qaudio::QohAudioRuntime& audio = qaudio::runtime();
        if (audio.capturesIntent()) {
            if (!internalBgm && qaudio::bgmOwnedCallsite(callsiteRva)) {
                (void)audio.captureBgm(
                    static_cast<uint8_t>(trackId & 0xFF),
                    static_cast<uint32_t>(flags), callsiteRva);
                return 1;
            }
            if (audio.forbidsNativeAudio()) {
                return 1;
            }
            static LONG s_bgmUnownedCallsiteLogs = 0;
            if (!internalBgm
                && InterlockedIncrement(&s_bgmUnownedCallsiteLogs) <= 4) {
                QLOG("[audio-v2] BGM passthrough from unowned callsite "
                     "rva=0x%08X track=%d flags=%d",
                     callsiteRva, trackId, flags);
            }
            const qaudio::AudioTickDisposition saved = audio.disposition();
            audio.setDisposition(
                qaudio::AudioTickDisposition::PhysicalFlush);
            const int nativeResult =
                callNativeBgmPreservingFpu(trackId, flags);
            audio.setDisposition(saved);
            return nativeResult;
        }
        if (audio.forbidsNativeAudio()) {
            return 1;
        }
    }
    return callNativeBgmPreservingFpu(trackId, flags);
}

int __fastcall hkAudioTeardown() {
    if (g_audioV2Enabled && deterministicAudioRoleActive()
        && qaudio::runtime().forbidsNativeAudio()) {
        QLOG("[audio-v2] BLOCKED native audio teardown during historical work");
        return 0;
    }
    const int result = callNativeAudioTeardownPreservingFpu();
    if (g_audioV2Enabled) {
        qaudio::QohAudioRuntime& audio = qaudio::runtime();
        audio.onDeviceTeardown();
        for (int sample = 0; sample <= qaudio::kMaxSampleId; ++sample)
            audio.catalog().observeUnload(static_cast<int16_t>(sample));
    }
    netAudioNoteBufferGenerationChanged();
    return result;
}

int __fastcall hkAudioInit(void* hwnd) {
    const int result = callNativeAudioInitPreservingFpu(hwnd);
    if (result != 0)
        warmNativeDirectSoundDevice();
    if (g_audioV2Enabled) {
        QLOG("[audio-v2] native audio (re)initialized result=%d "
             "deviceGeneration=%u",
             result, qaudio::runtime().sink().deviceGeneration());
    }
    netAudioNoteBufferGenerationChanged();
    return result;
}

bool netProcessOwnsForegroundWindow();

static qinput::InputLaunchConfig g_inputLaunchConfig{};
static qinput::InputLaunchConfig g_replayTakeoverInputConfig{};
static qinput::CompiledModeControlsV1 g_modeControls{};
static qinput::ModeControlEvaluator g_modeControlEvaluator;
static qinput::ModeControlEvaluation g_modeControlEvaluation{};
static uint64_t g_modeControlEvaluationIteration = ~0ull;
static bool g_inputConfigPresent = false;
static bool g_replayTakeoverInputConfigPresent = false;
static bool g_replayTakeoverInputRuntimeReady = false;
static HWND g_replayTakeoverInputWindow = nullptr;
static bool g_inputSpectatorRole = false;
static bool g_inputRuntimeReady = false;
static qinput::KeyboardWorker* g_inputKeyboardWorker = nullptr;
static qinput::ControllerWorker* g_inputControllerWorker = nullptr;
static qinput::InputEvaluator* g_inputEvaluator = nullptr;

static qinput::InputLaunchConfig g_offlineExtraConfigs[3] = {};
static bool g_offlineExtraPresent[3] = {};
struct OfflinePlayerRuntime {
    const qinput::InputLaunchConfig* config = nullptr;
    qinput::InputEvaluator* evaluator = nullptr;
    qinput::ControllerWorker* worker = nullptr;
    bool overrideActive = false;
    int mask = 0;
    bool physicalNeutralBeforeSocd = false;
};
static OfflinePlayerRuntime g_offlinePlayers[4] = {};
static bool g_offlineManagedArmed = false;
static bool g_offlineManagedLaunch = false;

bool netOfflineOverrideActive(int player) {
    return g_offlineManagedArmed && player >= 0 && player < 4
        && g_offlinePlayers[player].overrideActive;
}
int netOfflineOverrideMask(int player) {
    return player >= 0 && player < 4 ? g_offlinePlayers[player].mask : 0;
}

inline bool offlineManagedWanted() {
    if (g_inputSpectatorRole || !g_inputConfigPresent) return false;
    return g_mode == RunMode::Observe || g_offlineManagedLaunch;
}

inline bool offlineAnyProbes() {
    if (g_inputLaunchConfig.probeCount > 0) return true;
    for (int slot = 0; slot < 3; ++slot)
        if (g_offlineExtraPresent[slot]
            && g_offlineExtraConfigs[slot].probeCount > 0)
            return true;
    return false;
}

inline bool offlineAnySdlProbes() {
    if (qinput::compiledControllerNeedsSdl(
            g_inputLaunchConfig.probes, g_inputLaunchConfig.probeCount,
            g_inputLaunchConfig.probeBackend)) return true;
    for (int slot = 0; slot < 3; ++slot) {
        if (g_offlineExtraPresent[slot]
            && qinput::compiledControllerNeedsSdl(
                g_offlineExtraConfigs[slot].probes,
                g_offlineExtraConfigs[slot].probeCount,
                g_offlineExtraConfigs[slot].probeBackend)) return true;
    }
    return false;
}

inline const qinput::InputLaunchConfig* offlinePlayerConfig(
    uint32_t player) {
    if (player == 0)
        return g_inputConfigPresent ? &g_inputLaunchConfig : nullptr;
    return player <= 3 && g_offlineExtraPresent[player - 1]
        ? &g_offlineExtraConfigs[player - 1] : nullptr;
}
static qinput::PublishedWindowFocus g_inputWindowFocus;
static uint32_t g_inputFocusGeneration = 0;
static WNDPROC g_inputPrevWndProc = nullptr;
struct NetInputBundle {
    qinput::KeyboardSnapshotPayload keyboard{};
    bool keyboardRead = false;
    qinput::ControllerSnapshotPayload controller{};
    bool controllerRead = false;
    bool focusActive = false;
    uint32_t focusGeneration = 0;
    uint32_t nowMs = 0;
};
static NetInputBundle g_inputBundle{};
static uint64_t g_inputBundleIteration = 0;
static uint64_t g_inputOuterIteration = 0;
static uint64_t g_inputPhysicalCaptures = 0;
static uint64_t g_inputStaleNeutralEvents = 0;
static uint64_t g_nativeInputBypasses = 0;

static qinput::SessionEventTimeline g_sessionEvents;
static bool g_sessionIsHost = false;
static bool g_sessionCaptureFrozen = false;
static bool g_sessionReportSubmitted = false;
static bool g_sessionAtBoundarySent = false;
static bool g_sessionRemoteAtBoundary = false;
static bool g_sessionFillDone = false;
static uint32_t g_sessionClientRequestId = 0;
static uint64_t g_sessionClientRequestMs = 0;
static bool g_netReturnToCharacterSelectPending = false;
static bool g_offlineReturnToCharacterSelectPending = false;
static uint32_t g_netReturnToCharacterSelectBoundary = 0;

static
constexpr uintptr_t kNativeBindingTablePtr = 0x626860u;
static uintptr_t g_nativeKeyboardCallbackFn = 0;
static bool g_nativeBindingsApplied = false;
static bool g_nativeBindingsCaptured = false;
// Binding table layout: 4 players x 8 controls x 3 words.
static uint32_t g_nativeBindingOriginal[
    qinput::kNativeBindingPlayers * qinput::kNativeBindingActions * 3] = {};
static uint32_t g_nativeBindingPlayer = 0;

bool netApplyOfflineInputBindings() {
    if (g_nativeBindingsApplied || !g_inputConfigPresent
        || g_inputSpectatorRole) {
        return g_nativeBindingsApplied;
    }
    const uintptr_t table = static_cast<uintptr_t>(
        rd32(kNativeBindingTablePtr));
    if (table == 0) return false;

    if (!g_nativeBindingsCaptured) {
        for (uint32_t player = 0;
             player < qinput::kNativeBindingPlayers; ++player) {
            for (uint32_t action = 0;
                 action < qinput::kNativeBindingActions; ++action) {
                const uintptr_t descriptor = table
                    + qinput::nativeDescriptorOffset(player, action);
                const uint32_t base =
                    (player * qinput::kNativeBindingActions + action) * 3;
                for (uint32_t word = 0; word < 3; ++word) {
                    g_nativeBindingOriginal[base + word] =
                        rd32(descriptor + word * 4u);
                }
            }
        }
        g_nativeBindingsCaptured = true;
    }

    qinput::NativeBindingPlan plans[4] = {};
    bool configured[4] = {};
    for (uint32_t player = 0; player < qinput::kNativeBindingPlayers;
         ++player) {
        const qinput::InputLaunchConfig* config =
            offlinePlayerConfig(player);
        if (config == nullptr) continue;
        configured[player] = true;
        plans[player] = qinput::buildNativeBindingPlan(
            *config, g_nativeKeyboardCallbackFn);
        for (uint32_t action = 0; action < qinput::kNativeBindingActions;
             ++action) {
            const uintptr_t descriptor = table
                + qinput::nativeDescriptorOffset(player, action);
            if (rd32(descriptor) == qinput::kNativeSourceKeyboard)
                wr32(descriptor + 4u, 0);
            if (plans[player].unexpressedMask & (1u << action)) continue;
            wr32(descriptor, plans[player].actions[action].sourceType);
            wr32(descriptor + 4u,
                 plans[player].actions[action].codeOrMask);
            wr32(descriptor + 8u,
                 plans[player].actions[action].callback);
        }
    }

    uint32_t separated = 0;
    for (uint32_t player = 0; player < qinput::kNativeBindingPlayers;
         ++player) {
        if (configured[player]) continue;
        for (uint32_t action = 0; action < qinput::kNativeBindingActions;
             ++action) {
            const uintptr_t descriptor = table
                + qinput::nativeDescriptorOffset(player, action);
            if (rd32(descriptor) != qinput::kNativeSourceKeyboard) continue;
            const uint32_t code = rd32(descriptor + 4u);
            if (code == 0) continue;
            bool collides = false;
            for (uint32_t other = 0;
                 other < qinput::kNativeBindingPlayers && !collides;
                 ++other) {
                if (!configured[other]) continue;
                for (uint32_t bound = 0;
                     bound < qinput::kNativeBindingActions; ++bound) {
                    if (plans[other].unexpressedMask & (1u << bound))
                        continue;
                    if (plans[other].actions[bound].codeOrMask == code) {
                        collides = true;
                        break;
                    }
                }
            }
            if (collides) {
                wr32(descriptor + 4u, 0);
                ++separated;
            }
        }
    }

    g_nativeBindingsApplied = true;
    QLOG("[input-offline] applied per-player plans to QOH's native table "
         "configured=%d%d%d%d unexpressed=[%02X %02X %02X %02X] "
         "separatedStockKeys=%u",
         configured[0] ? 1 : 0, configured[1] ? 1 : 0,
         configured[2] ? 1 : 0, configured[3] ? 1 : 0,
         plans[0].unexpressedMask, plans[1].unexpressedMask,
         plans[2].unexpressedMask, plans[3].unexpressedMask, separated);
    return true;
}

void netRestoreOfflineInputBindings() {
    if (!g_nativeBindingsCaptured) return;
    const uintptr_t table = static_cast<uintptr_t>(
        rd32(kNativeBindingTablePtr));
    if (table == 0) return;
    for (uint32_t player = 0; player < qinput::kNativeBindingPlayers;
         ++player) {
        for (uint32_t action = 0; action < qinput::kNativeBindingActions;
             ++action) {
            const uintptr_t descriptor = table
                + qinput::nativeDescriptorOffset(player, action);
            const uint32_t base =
                (player * qinput::kNativeBindingActions + action) * 3;
            for (uint32_t word = 0; word < 3; ++word) {
                wr32(descriptor + word * 4u,
                     g_nativeBindingOriginal[base + word]);
            }
        }
    }
    g_nativeBindingsCaptured = false;
    g_nativeBindingsApplied = false;
    QLOG("[input-offline] restored QOH's stock binding descriptors (all "
         "four players) before config save");
}

void netResetSessionEventDriver() {
    g_sessionEvents.reset(g_sessionIsHost);
    g_modeControlEvaluator.reset();
    g_modeControlEvaluation = qinput::ModeControlEvaluation{};
    g_modeControlEvaluationIteration = ~0ull;
    g_sessionCaptureFrozen = false;
    g_sessionReportSubmitted = false;
    g_sessionAtBoundarySent = false;
    g_sessionRemoteAtBoundary = false;
    g_sessionFillDone = false;
    g_sessionClientRequestId = 0;
    g_sessionClientRequestMs = 0;
    g_netReturnToCharacterSelectPending = false;
    g_netReturnToCharacterSelectBoundary = 0;
    g_netCharSelectBackBlocked = 0;
    netRematchTheaterRestore("session-event driver reset");
    g_netInstantRematchLaunch = false;
    g_netStagedSelectorProofValid = false;
    g_rematchPrompt.reset();
    g_rematchEndRequested = false;
}

inline const uint8_t* seSelectPerBuildSignature(const uint8_t* november,
                                                const uint8_t* december) {
    return runtime_profile::selectedProfileId()
            == qoh99::generated_model::RuntimeProfileId::Qoh99DecemberCandidate
        ? december
        : november;
}

constexpr uint32_t kInputStaleThresholdMs = 100;
constexpr uintptr_t kNativeInputInitFn = static_cast<uintptr_t>(
    generated_model::nativeSymbolNovemberVa(
        generated_model::NativeSymbolId::NativeInputInit));
static_assert(kNativeInputInitFn == 0x45CD20u,
              "the managed-input guard must keep its verified November target");
static uintptr_t g_nativePresenceProbeFn = 0;
constexpr uintptr_t kNativeJoyLowStateFn = static_cast<uintptr_t>(
    generated_model::nativeSymbolNovemberVa(
        generated_model::NativeSymbolId::NativeJoyLowState));
static_assert(kNativeJoyLowStateFn == 0x45D100u,
              "the joystick low-state guard must keep its verified November "
              "target");
constexpr uintptr_t kNativeKeyStateAddrManaged = 0x4AA920;

typedef int (__fastcall* NativeInputInitFn)(void* hwnd, void* edxUnused);
typedef int (__cdecl* NativePresenceProbeFn)();
typedef int (__fastcall* NativeJoyLowFn)(int index, int out);
static NativeInputInitFn g_origInputInit = nullptr;
static NativePresenceProbeFn g_origPresenceProbe = nullptr;
static NativeJoyLowFn g_origJoyLow = nullptr;
static PollFn g_origFullPoll = nullptr;
static bool g_inputInitHooked = false;
static bool g_presenceProbeHooked = false;
static bool g_joyLowHooked = false;
static bool g_fullPollHooked = false;

bool netFCommandPolicyReady();

inline qgame::NativeDeveloperKeyPolicy activeNativeDeveloperKeyPolicy() {
    const qgame::NativeDeveloperRuntimeMode runtimeMode =
        g_mode == RunMode::SelfTest || g_unattendedHarnessLaunch
            ? qgame::NativeDeveloperRuntimeMode::SelfTestOrProbe
            : qgame::NativeDeveloperRuntimeMode::OfflineInteractive;
    return qgame::nativeDeveloperKeyPolicy(
        runtimeMode,
        g_nativeDeveloperKeysAllowedByLaunch,
        g_trainingLaunchRequested,
        g_replayRoleActive,
        g_netSpectatorMode,
        g_netActive);
}

inline void clearOwnedNativeDeveloperEdges() {
    for (uintptr_t address = 0x8A5D40u;
         address <= 0x8A5D4Fu; ++address) {
        wr8(address, 0u);
    }
    wr8(0x8A5D52u, 0u);
    wr8(0x8A5D53u, 0u);
    wr32(0x66ECC4u, rd32(0x66ECC4u) & ~3u);
}

inline void applyNativeDeveloperEdgePolicy() {
    if (activeNativeDeveloperKeyPolicy()
        == qgame::NativeDeveloperKeyPolicy::ForceDisabled) {
        clearOwnedNativeDeveloperEdges();
    }
}

inline bool managedInputRole() {
    if (g_replayRoleActive) return true;
    return g_inputConfigPresent
        && (g_mode == RunMode::Netplay || g_mode == RunMode::Spectator);
}

static uint32_t inputNowMs() {
    return static_cast<uint32_t>(GetTickCount64());
}

static qinput::IControllerBackend* inputCreateBackendFor(
        const qinput::CompiledProbe* probes, uint32_t probeCount,
        qinput::CompiledProbeBackend captured, void* focusWindow,
        unsigned player) {
    const bool needsSemantic =
        qinput::compiledProbesRequireStandardizedBackend(probes, probeCount);

    if (!needsSemantic
        && captured == qinput::CompiledProbeBackend::DirectInput8) {
        return qinput::createDirectInput8ControllerBackend(focusWindow);
    }
    const bool sdlHere = qinput::standardizedBackendAvailable();

    if (needsSemantic) {
        if (!sdlHere) {
            QLOG("[input] controller OFF for player %u: these bindings use "
                 "SDL's standardized button names, and SDL3.dll did not load "
                 "in the game process (backend=%s), so nothing here can "
                 "answer them. DirectInput8 would read those names as raw "
                 "button numbers and every control would come out wrong, so "
                 "the pad is left off and the keyboard keeps working.\n"
                 "  To fix it, either:\n"
                 "    1. put SDL3.dll in the same folder as the game .exe "
                 "(it ships in the bundle) and make sure antivirus has not "
                 "quarantined it - see the SDL3.dll line earlier in this log "
                 "for what went wrong; or\n"
                 "    2. open the caster's controller screen, pick the pad "
                 "again and choose \"By position\".",
                 player, qinput::controllerBackendName());
            return nullptr;
        }
        return qinput::createControllerBackend(focusWindow);
    }

    switch (captured) {
        case qinput::CompiledProbeBackend::DirectInput8:
            return qinput::createDirectInput8ControllerBackend(focusWindow);
        case qinput::CompiledProbeBackend::Sdl: {
            if (!sdlHere) {
                QLOG("[input] controller OFF for player %u: these bindings "
                     "are button POSITIONS counted by SDL, and SDL3.dll did "
                     "not load in the game process. DirectInput numbers the "
                     "same pad differently, so replaying them here would "
                     "press the wrong buttons rather than fail. Put SDL3.dll "
                     "next to the game .exe (see the SDL3.dll line earlier "
                     "in this log), or rebind the pad with SDL unavailable "
                     "so the positions are counted the same way both times.",
                     player);
                return nullptr;
            }
            qinput::IControllerBackend* backend =
                qinput::createControllerBackend(focusWindow);
            if (backend != nullptr) backend->preferRawJoystick(true);
            return backend;
        }
        case qinput::CompiledProbeBackend::Unknown:
        default:
            QLOG("[input] player %u has a positional profile from before the "
                 "capture backend was recorded; using %s, which is what the "
                 "caster would have captured it with. Rebind the pad if the "
                 "buttons come out wrong.",
                 player, sdlHere ? "SDL" : "DirectInput8");
            if (sdlHere) {
                qinput::IControllerBackend* backend =
                    qinput::createControllerBackend(focusWindow);
                if (backend != nullptr) backend->preferRawJoystick(true);
                return backend;
            }
            return qinput::createDirectInput8ControllerBackend(focusWindow);
    }
}

static void mirrorInteractiveOfflineDeveloperKeysFromManagedSnapshot() {
    if (activeNativeDeveloperKeyPolicy()
        != qgame::NativeDeveloperKeyPolicy::RespectUserFCommand) {
        return;
    }
    if (g_inputKeyboardWorker == nullptr
        || !g_inputKeyboardWorker->running()
        || !netProcessOwnsForegroundWindow()) {
        return;
    }

    qinput::KeyboardSnapshotPayload snapshot{};
    if (!g_inputKeyboardWorker->snapshot().read(snapshot)
        || snapshot.status
               != static_cast<uint32_t>(qinput::SnapshotStatus::Connected)
        || qinput::snapshotAgeMs(inputNowMs(), snapshot.sampleTickMs)
               >= kInputStaleThresholdMs) {
        return;
    }

    constexpr uintptr_t kNativeDirectInputActive = 0x4AA6F8u;
    constexpr uintptr_t kNativeDikToVirtualKey = 0x4A2130u;
    static constexpr uint8_t kDikF2ThroughF9[] = {
        0x3Cu, 0x3Du, 0x3Eu, 0x3Fu, 0x40u, 0x41u, 0x42u, 0x43u
    };
    constexpr uint8_t kDikF12 = 0x58u;
    constexpr uint8_t kDikDelete = 0xD3u;
    const bool directInputActive = rd32(kNativeDirectInputActive) != 0u;
    uint16_t pressedMask = 0u;
    const auto mirrorKey = [&](const uint8_t dik) {
        const bool held =
            ((snapshot.keyBits[dik >> 5] >> (dik & 31u)) & 1u) != 0u;
        const uint8_t nativeIndex = directInputActive
            ? dik : rd8(kNativeDikToVirtualKey + dik);
        wr8(kNativeKeyStateAddrManaged + nativeIndex,
            held ? 0x80u : 0u);
        return held;
    };
    for (size_t key = 0; key < sizeof(kDikF2ThroughF9); ++key) {
        if (mirrorKey(kDikF2ThroughF9[key]))
            pressedMask |= static_cast<uint16_t>(1u << key);
    }
    if (mirrorKey(kDikF12)) pressedMask |= 0x100u;
    if (mirrorKey(kDikDelete)) pressedMask |= 0x200u;

    static bool s_readyLogged = false;
    static uint16_t s_previousPressedMask = 0u;
    if (!s_readyLogged) {
        s_readyLogged = true;
        QLOG("[extra-fn] native F2-F9/F12/Delete keyboard bridge READY "
             "(layout=%s; VS Player/VS CPU/Free Play only)",
             directInputActive ? "DIK" : "VK");
    }
    if (pressedMask != s_previousPressedMask) {
        QLOG("[extra-fn] native F2-F9/F12/Delete held mask=%03X",
             static_cast<unsigned>(pressedMask));
        s_previousPressedMask = pressedMask;
    }
}

static LRESULT CALLBACK inputFocusWndProc(HWND hwnd, UINT message,
                                          WPARAM wParam, LPARAM lParam) {
    if (message == WM_ACTIVATEAPP) {
        g_inputFocusGeneration =
            (g_inputFocusGeneration + 1u) & 0x7FFFFFFFu;
        g_inputWindowFocus.publish(wParam != 0, g_inputFocusGeneration);
    }
    return CallWindowProcA(g_inputPrevWndProc, hwnd, message, wParam,
                           lParam);
}

static void netPublishManagedFocus() {
    const bool active = netProcessOwnsForegroundWindow();
    bool published = false;
    uint32_t generation = 0;
    g_inputWindowFocus.readInto(published, generation);
    if (published == active && generation != 0) return;
    g_inputFocusGeneration = (g_inputFocusGeneration + 1u) & 0x7FFFFFFFu;
    g_inputWindowFocus.publish(active, g_inputFocusGeneration);
}

static void netLogInputStall(const char* who, bool held, const char* source) {
    struct Track { const char* who; const char* source; bool held; uint32_t frames; };
    static Track tracks[12] = {};
    Track* track = nullptr;
    for (Track& t : tracks) {
        if (t.who == who && t.source == source) { track = &t; break; }
    }
    if (track == nullptr) {
        for (Track& t : tracks) {
            if (t.who == nullptr) {
                t.who = who;
                t.source = source;
                track = &t;
                break;
            }
        }
    }
    if (track == nullptr) return;
    if (held) {
        if (!track->held) {
            QLOG("[input-stall] %s %s went quiet (sample older than %u ms); "
                 "carrying its last input instead of dropping it",
                 who, source, static_cast<unsigned>(kInputStaleThresholdMs));
        }
        track->held = true;
        ++track->frames;
        return;
    }
    if (track->held) {
        QLOG("[input-stall] %s %s back after %u frame(s)%s", who, source,
             static_cast<unsigned>(track->frames),
             track->frames >= qinput::kSourceStallHoldEvaluations
                 ? " - too long to carry; it was treated as disconnected"
                 : "; held through it, nothing was lost");
    }
    track->held = false;
    track->frames = 0u;
}

static void netRefreshInputBundle() {
    if (g_inputBundleIteration == g_inputOuterIteration) return;
    g_inputBundleIteration = g_inputOuterIteration;
    netPublishManagedFocus();
    g_inputBundle.keyboardRead = false;
    g_inputBundle.controllerRead = false;
    g_inputBundle.nowMs = inputNowMs();
    if (g_inputKeyboardWorker != nullptr) {
        g_inputBundle.keyboardRead =
            g_inputKeyboardWorker->snapshot().read(g_inputBundle.keyboard);
    }
    if (g_inputControllerWorker != nullptr) {
        g_inputBundle.controllerRead =
            g_inputControllerWorker->snapshot().read(
                g_inputBundle.controller);
    }
    g_inputWindowFocus.readInto(g_inputBundle.focusActive,
                                g_inputBundle.focusGeneration);
}

inline bool bundleKeyboardHealthy() {
    return g_inputBundle.keyboardRead
        && g_inputBundle.keyboard.status
               == static_cast<uint32_t>(qinput::SnapshotStatus::Connected)
        && qinput::snapshotAgeMs(g_inputBundle.nowMs,
                                 g_inputBundle.keyboard.sampleTickMs)
               < kInputStaleThresholdMs;
}

inline bool bundleKeyHeld(uint8_t dik) {
    return bundleKeyboardHealthy()
        && ((g_inputBundle.keyboard.keyBits[dik >> 5]
             >> (dik & 31u)) & 1u) != 0u;
}

static const char* inputGateName(qinput::SourceGateState state) {
    switch (state) {
        case qinput::SourceGateState::Disconnected: return "disconnected";
        case qinput::SourceGateState::AwaitNeutral: return "await-neutral";
        case qinput::SourceGateState::NeutralSettle: return "settling";
        case qinput::SourceGateState::Armed: return "armed";
    }
    return "?";
}

static const char* inputAttachName(qinput::ControllerAttachState state) {
    switch (state) {
        case qinput::ControllerAttachState::Starting: return "starting";
        case qinput::ControllerAttachState::BackendUnavailable:
            return "backend-unavailable";
        case qinput::ControllerAttachState::NoCandidate:
            return "no-matching-device";
        case qinput::ControllerAttachState::Ambiguous:
            return "ambiguous-duplicates";
        case qinput::ControllerAttachState::OpenFailed: return "open-failed";
        case qinput::ControllerAttachState::LayoutChanged:
            return "layout-changed";
        case qinput::ControllerAttachState::Attached: return "attached";
    }
    return "?";
}

static void netLogManagedCaptureState(const qinput::KeyboardView& keyboard,
                                      const qinput::ControllerView& controller,
                                      const qinput::EvaluateResult& result) {
    static uint32_t s_logged = 0;
    static uint32_t s_signature = 0xFFFFFFFFu;
    static bool s_sawNonNeutral = false;
    const bool padConfigured = g_inputLaunchConfig.probeCount > 0;
    const qinput::ControllerAttachState attach =
        g_inputControllerWorker != nullptr
            ? g_inputControllerWorker->attachState()
            : qinput::ControllerAttachState::Starting;
    const uint32_t signature =
        (static_cast<uint32_t>(attach) << 16)
        | (static_cast<uint32_t>(g_inputBundle.controller.status & 7u) << 12)
        | (static_cast<uint32_t>(result.controllerGate) << 8)
        | (static_cast<uint32_t>(result.keyboardGate) << 4)
        | (controller.healthy ? 8u : 0u) | (keyboard.healthy ? 4u : 0u)
        | (g_inputBundle.focusActive ? 2u : 0u)
        | (padConfigured ? 1u : 0u);
    if (signature != s_signature && s_logged < 48u) {
        ++s_logged;
        s_signature = signature;
        QLOG("[input-net] local capture pad=%s(%s candidates=%u healthy=%d "
             "gate=%s) keyboard=(healthy=%d gate=%s) focus=%d mask=%04X",
             padConfigured ? "configured" : "none",
             inputAttachName(attach),
             g_inputControllerWorker != nullptr
                 ? g_inputControllerWorker->lastCandidateCount() : 0u,
             controller.healthy ? 1 : 0,
             inputGateName(result.controllerGate),
             keyboard.healthy ? 1 : 0,
             inputGateName(result.keyboardGate),
             g_inputBundle.focusActive ? 1 : 0,
             static_cast<unsigned>(result.mask));
    }
    if (!s_sawNonNeutral && result.mask != 0) {
        s_sawNonNeutral = true;
        QLOG("[input-net] first non-neutral local sample mask=%04X after %llu "
             "captures", static_cast<unsigned>(result.mask),
             static_cast<unsigned long long>(g_inputPhysicalCaptures));
    }
}

static int netManagedPhysicalCapture(bool& physicalNeutralBeforeSocd) {
    physicalNeutralBeforeSocd = true;
    if (g_inputEvaluator == nullptr) return 0;
    netRefreshInputBundle();
    const bool keyboardHealthy = bundleKeyboardHealthy();
    if (!keyboardHealthy && g_inputBundle.keyboardRead)
        ++g_inputStaleNeutralEvents;
    qinput::KeyboardView keyboard{};
    keyboard.keyBits = g_inputBundle.keyboard.keyBits;
    keyboard.healthy = keyboardHealthy;
    keyboard.generation = g_inputBundle.keyboard.deviceGeneration;
    keyboard.sampleSerial = g_inputBundle.keyboard.sampleSerial;
    qinput::ControllerView controller{};
    if (g_inputControllerWorker != nullptr && g_inputBundle.controllerRead) {
        const bool controllerHealthy =
            g_inputBundle.controller.status
                == static_cast<uint32_t>(qinput::SnapshotStatus::Connected)
            && qinput::snapshotAgeMs(g_inputBundle.nowMs,
                                     g_inputBundle.controller.sampleTickMs)
                   < kInputStaleThresholdMs;
        controller.probes = g_inputBundle.controller.probe;
        controller.probeCount = g_inputLaunchConfig.probeCount;
        controller.healthy = controllerHealthy;
        controller.generation = g_inputBundle.controller.deviceGeneration;
        controller.sampleSerial = g_inputBundle.controller.sampleSerial;
        if (!controllerHealthy) ++g_inputStaleNeutralEvents;
    }
    qinput::FocusView focus{g_inputBundle.focusActive,
                            g_inputBundle.focusGeneration};
    const qinput::EvaluateResult result =
        g_inputEvaluator->evaluate(keyboard, controller, focus);
    netLogInputStall("netplay", result.controllerStallHeld, "pad");
    netLogInputStall("netplay", result.keyboardStallHeld, "keyboard");
    physicalNeutralBeforeSocd = result.physicalNeutralBeforeSocd;
    ++g_inputPhysicalCaptures;
    netLogManagedCaptureState(keyboard, controller, result);
    return static_cast<int>(qinput::logicalFromCompact(result.mask));
}

static bool replayTakeoverStartInputRuntime() {
    if (g_replayTakeoverInputRuntimeReady) return true;
    if (!g_replayRoleActive || !g_replayTakeoverInputConfigPresent
        || g_replayTakeoverInputWindow == nullptr
        || g_inputEvaluator != nullptr || g_inputKeyboardWorker != nullptr
        || g_inputControllerWorker != nullptr) {
        return false;
    }

    g_inputLaunchConfig = g_replayTakeoverInputConfig;
    g_inputEvaluator = new (std::nothrow) qinput::InputEvaluator();
    if (g_inputEvaluator == nullptr) return false;
    g_inputEvaluator->configure(&g_inputLaunchConfig);

    g_inputKeyboardWorker = new (std::nothrow) qinput::KeyboardWorker();
    if (g_inputKeyboardWorker == nullptr) {
        delete g_inputEvaluator;
        g_inputEvaluator = nullptr;
        return false;
    }
    qinput::KeyboardWorkerConfig keyboardConfig{};
    keyboardConfig.backend = qinput::createDirectInput8KeyboardBackend(
        g_replayTakeoverInputWindow);
    keyboardConfig.nowMs = &inputNowMs;
    if (!g_inputKeyboardWorker->start(keyboardConfig)) {
        delete g_inputKeyboardWorker;
        g_inputKeyboardWorker = nullptr;
        delete g_inputEvaluator;
        g_inputEvaluator = nullptr;
        return false;
    }

    if (g_inputLaunchConfig.probeCount > 0u) {
        qinput::SdlBootstrapResult sdl =
            qinput::SdlBootstrapResult::NotRequired;
        if (qinput::compiledControllerNeedsSdl(
                g_inputLaunchConfig.probes,
                g_inputLaunchConfig.probeCount,
                g_inputLaunchConfig.probeBackend)) {
            sdl = qinput::sdlBootstrapOnMainThread();
        }
        const bool standardized =
            qinput::compiledProbesRequireStandardizedBackend(
                g_inputLaunchConfig.probes,
                g_inputLaunchConfig.probeCount);
        QLOG("[input-v2] controller backend: sdl-bootstrap=%d probes=%u "
             "requires=%s selected=%s",
             static_cast<int>(sdl),
             static_cast<unsigned>(g_inputLaunchConfig.probeCount),
             standardized ? "standardized" : "raw",
             standardized || g_inputLaunchConfig.probeBackend
                                 != qinput::CompiledProbeBackend::DirectInput8
                 ? qinput::controllerBackendName()
                 : "DirectInput8 raw");
        g_inputControllerWorker =
            new (std::nothrow) qinput::ControllerWorker();
        if (g_inputControllerWorker != nullptr) {
            qinput::ControllerWorkerConfig controllerConfig{};
            controllerConfig.backend = inputCreateBackendFor(
                g_inputLaunchConfig.probes, g_inputLaunchConfig.probeCount,
                g_inputLaunchConfig.probeBackend,
                g_replayTakeoverInputWindow, 1u);
            controllerConfig.nowMs = &inputNowMs;
            controllerConfig.matcher = &g_inputLaunchConfig.device;
            controllerConfig.probes = g_inputLaunchConfig.probes;
            controllerConfig.probeCount = g_inputLaunchConfig.probeCount;
            controllerConfig.mapping = g_inputLaunchConfig.mappingLength > 0u
                ? g_inputLaunchConfig.mapping : nullptr;
            controllerConfig.mappingLength =
                g_inputLaunchConfig.mappingLength;
            if (!g_inputControllerWorker->start(controllerConfig)) {
                delete g_inputControllerWorker;
                g_inputControllerWorker = nullptr;
            }
        }
    }

    ++g_inputOuterIteration;
    g_inputBundleIteration = 0u;
    g_inputFocusGeneration =
        (g_inputFocusGeneration + 1u) & 0x7FFFFFFFu;
    g_inputWindowFocus.publish(netProcessOwnsForegroundWindow(),
                               g_inputFocusGeneration);
    g_replayTakeoverInputRuntimeReady = true;
    QLOG("[replay.takeover] local P1 input ready keyboard=1 controller=%d "
         "profileHash=%016llX",
         g_inputControllerWorker != nullptr ? 1 : 0,
         static_cast<unsigned long long>(g_inputLaunchConfig.profileHash));
    return true;
}

static void replayTakeoverStopInputRuntime() {
    if (g_inputControllerWorker != nullptr) {
        g_inputControllerWorker->requestStop();
        if (g_inputControllerWorker->join(250u))
            delete g_inputControllerWorker;
        g_inputControllerWorker = nullptr;
    }
    if (g_inputKeyboardWorker != nullptr) {
        g_inputKeyboardWorker->requestStop();
        if (g_inputKeyboardWorker->join(250u))
            delete g_inputKeyboardWorker;
        g_inputKeyboardWorker = nullptr;
    }
    delete g_inputEvaluator;
    g_inputEvaluator = nullptr;
    g_replayTakeoverInputRuntimeReady = false;
    g_inputBundle = NetInputBundle{};
    ++g_inputOuterIteration;
}

static uint16_t replayTakeoverCaptureCompactInput() {
    ++g_inputOuterIteration;
    bool neutralBeforeSocd = true;
    const int logical = netManagedPhysicalCapture(neutralBeforeSocd);
    (void)neutralBeforeSocd;
    return qinput::compactFromLogical(static_cast<uint32_t>(logical));
}

static bool offlineManagedBootstrapStep(void* hwnd) {
    static uint32_t s_stage = 0;
    static int s_started = 0;
    if (s_stage == 0) {
        ++s_stage;
        if (offlineAnySdlProbes()) {
            // SDL subsystem init is main/window-thread only (SdlLifecycle.h).
            const DWORD startedMs = GetTickCount();
            const qinput::SdlBootstrapResult sdl =
                qinput::sdlBootstrapOnMainThread();
            const DWORD elapsedMs = GetTickCount() - startedMs;
            char sdlVersion[64] = {};
            qinput::sdlVersionText(sdlVersion, sizeof(sdlVersion));
            QLOG("[input-offline] SDL gamepad subsystem init on the game's "
                 "main thread: result=%d backend=%s sdl=%s enumeration=%lums "
                 "(cold HID/Bluetooth enumeration is expected to be slow; the "
                 "Stage-B budget is finite, not absent)",
                 static_cast<int>(sdl), qinput::controllerBackendName(),
                 sdlVersion, static_cast<unsigned long>(elapsedMs));
        }
        return false;
    }
    if (s_stage == 1) {
        ++s_stage;
        if (g_inputKeyboardWorker == nullptr) {
            g_inputKeyboardWorker =
                new (std::nothrow) qinput::KeyboardWorker();
            if (g_inputKeyboardWorker != nullptr) {
                qinput::KeyboardWorkerConfig workerConfig{};
                workerConfig.backend =
                    qinput::createDirectInput8KeyboardBackend(hwnd);
                workerConfig.nowMs = &inputNowMs;
                if (!g_inputKeyboardWorker->start(workerConfig)) {
                    delete g_inputKeyboardWorker;
                    g_inputKeyboardWorker = nullptr;
                }
            }
        }
        return false;
    }
    if (s_stage >= 2 && s_stage < 6) {
        const uint32_t player = s_stage - 2;
        ++s_stage;
        OfflinePlayerRuntime& runtime = g_offlinePlayers[player];
        runtime.config = offlinePlayerConfig(player);
        if (runtime.config != nullptr) {
            runtime.evaluator = new (std::nothrow) qinput::InputEvaluator();
            if (runtime.evaluator != nullptr) {
                runtime.evaluator->configure(runtime.config);
                if (runtime.config->probeCount > 0) {
                    runtime.worker =
                        new (std::nothrow) qinput::ControllerWorker();
                    if (runtime.worker != nullptr) {
                        qinput::ControllerWorkerConfig controllerConfig{};
                        controllerConfig.backend = inputCreateBackendFor(
                            runtime.config->probes,
                            runtime.config->probeCount,
                            runtime.config->probeBackend, hwnd, player + 1u);
                        controllerConfig.nowMs = &inputNowMs;
                        controllerConfig.matcher = &runtime.config->device;
                        controllerConfig.probes = runtime.config->probes;
                        controllerConfig.probeCount =
                            runtime.config->probeCount;
                        controllerConfig.mapping =
                            runtime.config->mappingLength > 0
                                ? runtime.config->mapping : nullptr;
                        controllerConfig.mappingLength =
                            runtime.config->mappingLength;
                        if (!runtime.worker->start(controllerConfig)) {
                            QLOG("[input-offline] controller worker for "
                                 "player %u FAILED to start; that pad "
                                 "stays neutral this session", player + 1u);
                            delete runtime.worker;
                            runtime.worker = nullptr;
                        }
                    }
                }
                runtime.overrideActive = true;
                ++s_started;
            }
        }
        return false;
    }
    if (s_stage == 6) {
        ++s_stage;
        const bool trainingP1RuntimeReady =
            g_offlinePlayers[0].overrideActive
            && g_offlinePlayers[0].evaluator != nullptr
            && g_inputKeyboardWorker != nullptr
            && g_inputKeyboardWorker->running();
        const bool stageSucceeded =
            !g_trainingLaunchRequested
            || trainingP1RuntimeReady;
        g_offlineManagedArmed =
            s_started > 0 && stageSucceeded;
        char sdlVersion[64] = "not needed";
        if (offlineAnySdlProbes())
            qinput::sdlVersionText(sdlVersion, sizeof(sdlVersion));
        QLOG("[input-offline] managed controller runtime: status=%s "
             "backend=%s "
             "sdl=%s playersStarted=%d keyboardWorker=%d (staged over %u "
             "frames)",
             stageSucceeded ? "READY" : "FAILED",
             qinput::controllerBackendName(), sdlVersion, s_started,
             g_inputKeyboardWorker != nullptr
                 && g_inputKeyboardWorker->running() ? 1 : 0,
             s_stage);
        wchar_t eventName[64];
        if (stageSucceeded) {
            qipc::ipcInputReadyEventName(
                eventName, 64, GetCurrentProcessId());
        } else {
            qipc::ipcInputFailedEventName(
                eventName, 64, GetCurrentProcessId());
        }
        HANDLE stageEvent =
            OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
        if (stageEvent != nullptr) {
            SetEvent(stageEvent);
            CloseHandle(stageEvent);
        }
        return true;
    }
    return true;
}

bool netRecorderPublishFrame(uint32_t frame, uint16_t p1, uint16_t p2,
                             uint64_t preStateHash, uint64_t postStateHash,
                             const qreplay::LifecycleTag& before,
                             const qreplay::LifecycleTag& after,
                             int dispatcherResult, uint32_t routeEpoch);
qreplay::LifecycleTag netLifecycleTagFromPhase(uint32_t phaseTag,
                                               uint32_t layoutGeneration);
bool netRecorderArmed();
bool netRecorderCaptureOfflineStart();
bool netRecorderCloseMatchSegment(qreplay::TerminalReason reason);
static qnetplay::RematchTheaterObserved netRematchTheaterObserve();
bool netOfflineRematchTheaterFlipped();
void netOfflineRecorderCommitV2();
uint64_t netSpectatorLiveStateHash(uint64_t audioFrame = g_ovrFrame);

static bool g_offlineRecorderRebaseline = false;

static void netOfflineRecorderCommit() {
    if (!netRecorderArmed()) return;
    static uint32_t s_lastFrameCounter = 0;
    static bool s_haveCounter = false;
    static uint32_t s_timelineFrame = 0;
    static uint64_t s_pendingPreHash = 0;
    static uint32_t s_pendingPhase = 0;
    static bool s_pendingOpen = false;
    const uint32_t counter = rd32(0x4AB778);
    if (g_offlineRecorderRebaseline) {
        g_offlineRecorderRebaseline = false;
        s_haveCounter = false;
        s_pendingOpen = false;
    }
    if (!s_haveCounter) {
        s_haveCounter = true;
        s_lastFrameCounter = counter;
        s_pendingPreHash = netSpectatorLiveStateHash();
        s_pendingPhase = netPhaseTag();
        s_pendingOpen = netRecorderCaptureOfflineStart();
        return;
    }
    if (counter == s_lastFrameCounter) return;
    s_lastFrameCounter = counter;
    const uint64_t postHash = netSpectatorLiveStateHash();
    const uint32_t phaseAfter = netPhaseTag();
    if (s_pendingOpen) {
        // g_offlinePlayers[].mask is the expanded logical value: masking with 0x0F0F drops direction bits 16..19, so use the codec.
        netRecorderPublishFrame(
            s_timelineFrame,
            qinput::compactFromLogical(g_offlinePlayers[0].mask),
            qinput::compactFromLogical(g_offlinePlayers[1].mask),
            s_pendingPreHash, postHash,
            netLifecycleTagFromPhase(s_pendingPhase, 0),
            netLifecycleTagFromPhase(phaseAfter, 0),
            0, 0);
        ++s_timelineFrame;
    }
    s_pendingPreHash = postHash;
    s_pendingPhase = phaseAfter;
    s_pendingOpen = netRecorderCaptureOfflineStart();
}

void netOfflineReturnToCharacterSelectTick(const bool edge) {
    if (!edge) return;
    if (g_offlineReturnToCharacterSelectPending) return;
    if (g_mode != RunMode::Observe || g_netActive || g_replayActive
        || g_replayRoleActive || g_trainingLaunchRequested
        || !offlineManagedWanted()
        || qgame::directBootUnattendedFrontendActive()) {
        return;
    }
    /* Scene 2 only: returning zero from the menu or character-select dispatcher exits the process. */
    if (!qgame::onlineReturnToCharacterSelectEligible(rd32(kSceneStateAddr)))
        return;

    if (netOfflineRematchTheaterFlipped()) {
        QLOG("[offline] F1 ignored: the rematch prompt is open and owns the "
             "post-game state; answer it instead");
        return;
    }

    const qnetplay::RematchTheaterObserved observed =
        netRematchTheaterObserve();
    if (!qnetplay::rematchTheaterHumanVersusHuman(observed)) {
        QLOG("[offline] F1 ignored: not a versus-human match "
             "(formationActive=%u committedCpuMask=%u)",
             observed.formationActive, observed.committedCpuMask);
        return;
    }
    if (qgame::replay::replayRecorderV2Active()) {
        const bool closed = qgame::replay::replayAbandonOpenGameV2(
            qreplay::TerminalReason::ProcessExit,
            qreplay::ReplayCloseProvenanceV2::LocalOrderly);
        QLOG("[replay.record-v2] offline F1 tail %s",
             closed ? "closed" : "close REFUSED; the return continues");
    } else if (netRecorderArmed()) {
        const bool closed = netRecorderCloseMatchSegment(
            qreplay::TerminalReason::ProcessExit);
        g_offlineRecorderRebaseline = true;
        QLOG("[replay.record] offline F1 tail %s",
             closed ? "closed" : "close REFUSED; the return continues");
    }
    g_offlineReturnToCharacterSelectPending = true;
    QLOG("[offline] F1 return armed: leaving the match for character select "
         "(scene=%u matchSub=%d)",
         rd32(kSceneStateAddr),
         static_cast<int>(rd32(kMatchSubAddr)));
}

void netOfflineFrameTick() {
    (void)netApplyOfflineInputBindings();
    netOfflineRecorderCommit();
    netOfflineRecorderCommitV2();
    netTrackCompletedGameWins();
    netOfflineRematchTick();
    {
        static bool s_armLogged = false;
        static uint8_t s_lastHitboxToggle = 0xFF;
        if (!s_armLogged) {
            const uint32_t active =
                rd32(RollbackManager::kQohActiveConfigPointerAddress);
            if (RollbackManager::validQohActiveConfigPointer(active)) {
                s_armLogged = true;
                QLOG("[extra-fn] offline: config+80=0x%X (bit0 -> F2..F9 "
                     "armed via FCommand ExtraFunctionEnable; keys act in "
                     "the MATCH scene only)", rd32(active + 80u));
            }
        }
        const uint8_t hitboxToggle = rd8(0x4AA424u);
        if (hitboxToggle != s_lastHitboxToggle) {
            if (s_lastHitboxToggle != 0xFF)
                QLOG("[extra-fn] hitbox display (F7) -> %s",
                     hitboxToggle ? "ON" : "off");
            s_lastHitboxToggle = hitboxToggle;
        }
    }
    static bool s_bootstrapDone = false;
    if (!s_bootstrapDone && offlineManagedWanted()) {
        const uintptr_t gameWindow =
            static_cast<uintptr_t>(rd32(0x4AA4C8u));
        if (gameWindow != 0) {
            s_bootstrapDone = offlineManagedBootstrapStep(
                reinterpret_cast<void*>(gameWindow));
        }
    }
    if (!g_offlineManagedArmed) return;
    g_inputFocusGeneration = (g_inputFocusGeneration + 1u) & 0x7FFFFFFFu;
    g_inputWindowFocus.publish(netProcessOwnsForegroundWindow(),
                               g_inputFocusGeneration);
    qinput::KeyboardSnapshotPayload keyboardSnapshot{};
    bool keyboardRead = false;
    if (g_inputKeyboardWorker != nullptr) {
        keyboardRead =
            g_inputKeyboardWorker->snapshot().read(keyboardSnapshot);
    }
    const uint32_t nowMs = inputNowMs();
    qinput::ControllerSnapshotPayload trainingPadSnapshot{};
    bool trainingPadRead = false;
    for (uint32_t player = 0; player < 4; ++player) {
        OfflinePlayerRuntime& runtime = g_offlinePlayers[player];
        if (!runtime.overrideActive || runtime.evaluator == nullptr)
            continue;
        qinput::KeyboardView keyboard{};
        if (keyboardRead) {
            const bool healthy = keyboardSnapshot.status
                    == static_cast<uint32_t>(
                           qinput::SnapshotStatus::Connected)
                && qinput::snapshotAgeMs(nowMs,
                                         keyboardSnapshot.sampleTickMs)
                       < kInputStaleThresholdMs;
            keyboard.keyBits = keyboardSnapshot.keyBits;
            keyboard.healthy = healthy;
            keyboard.generation = keyboardSnapshot.deviceGeneration;
            keyboard.sampleSerial = keyboardSnapshot.sampleSerial;
        }
        qinput::ControllerView controller{};
        qinput::ControllerSnapshotPayload pad{};
        const bool padRead = runtime.worker != nullptr
            && runtime.worker->snapshot().read(pad);
        if (player == 0 && padRead) {
            trainingPadSnapshot = pad;
            trainingPadRead = true;
        }
        if (padRead) {
            const bool healthy = pad.status
                    == static_cast<uint32_t>(
                           qinput::SnapshotStatus::Connected)
                && qinput::snapshotAgeMs(nowMs, pad.sampleTickMs)
                       < kInputStaleThresholdMs;
            controller.probes = pad.probe;
            controller.probeCount = runtime.config->probeCount;
            controller.healthy = healthy;
            controller.generation = pad.deviceGeneration;
            controller.sampleSerial = pad.sampleSerial;
        }
        bool focusActive = false;
        uint32_t focusGeneration = 0;
        g_inputWindowFocus.readInto(focusActive, focusGeneration);
        qinput::FocusView focus{focusActive, focusGeneration};
        const qinput::EvaluateResult result =
            runtime.evaluator->evaluate(keyboard, controller, focus);
        netLogInputStall(player == 0 ? "offline-p1"
                         : player == 1 ? "offline-p2"
                         : player == 2 ? "offline-p3" : "offline-p4",
                         result.controllerStallHeld, "pad");
        runtime.mask = static_cast<int>(
            qinput::logicalFromCompact(result.mask));
        runtime.physicalNeutralBeforeSocd =
            result.physicalNeutralBeforeSocd;

        if (player == 0) {
            static uint32_t s_logged = 0;
            static uint32_t s_lastStatus = 0xFFFFFFFFu;
            static int s_lastMask = -1;
            static int s_lastHealthy = -1;
            static int s_lastFocus = -1;
            const int healthyNow = controller.healthy ? 1 : 0;
            const int focusNow = focusActive ? 1 : 0;
            if ((pad.status != s_lastStatus || runtime.mask != s_lastMask
                 || healthyNow != s_lastHealthy || focusNow != s_lastFocus)
                && s_logged < 40u) {
                ++s_logged;
                QLOG("[input-offline] P1 pad read=%d status=%u healthy=%d "
                     "ageMs=%u gen=%u serial=%u probes=%u focus=%d "
                     "-> mask=%08X",
                     padRead ? 1 : 0, static_cast<unsigned>(pad.status),
                     healthyNow,
                     padRead ? static_cast<unsigned>(qinput::snapshotAgeMs(
                                   nowMs, pad.sampleTickMs))
                             : 0u,
                     static_cast<unsigned>(pad.deviceGeneration),
                     static_cast<unsigned>(pad.sampleSerial),
                     static_cast<unsigned>(runtime.config->probeCount),
                     focusNow, static_cast<unsigned>(runtime.mask));
            }
            s_lastStatus = pad.status;
            s_lastMask = runtime.mask;
            s_lastHealthy = healthyNow;
            s_lastFocus = focusNow;
        }
    }

    bool focusActive = false;
    uint32_t focusGeneration = 0;
    g_inputWindowFocus.readInto(focusActive, focusGeneration);
    const bool keyboardHealthy = keyboardRead
        && keyboardSnapshot.status
            == static_cast<uint32_t>(qinput::SnapshotStatus::Connected)
        && qinput::snapshotAgeMs(nowMs, keyboardSnapshot.sampleTickMs)
            < kInputStaleThresholdMs;
    qinput::KeyboardView trainingKeyboard{};
    trainingKeyboard.keyBits = keyboardSnapshot.keyBits;
    trainingKeyboard.healthy = keyboardHealthy;
    trainingKeyboard.generation = keyboardSnapshot.deviceGeneration;
    trainingKeyboard.sampleSerial = keyboardSnapshot.sampleSerial;
    qinput::ControllerView trainingController{};
    if (trainingPadRead) {
        trainingController.probes = trainingPadSnapshot.probe;
        trainingController.probeCount = g_inputLaunchConfig.probeCount;
        trainingController.healthy = trainingPadSnapshot.status
                == static_cast<uint32_t>(qinput::SnapshotStatus::Connected)
            && qinput::snapshotAgeMs(nowMs, trainingPadSnapshot.sampleTickMs)
                   < kInputStaleThresholdMs;
        trainingController.generation = trainingPadSnapshot.deviceGeneration;
        trainingController.sampleSerial = trainingPadSnapshot.sampleSerial;
    }
    qinput::FocusView trainingFocus{focusActive, focusGeneration};
    uint64_t enabledTraining = 0;
    for (const qinput::ModeControlDescriptor& descriptor
         : qinput::kModeControlDescriptors) {
        if (descriptor.domain == qinput::ModeControlDomain::Training)
            enabledTraining |= qinput::modeControlBit(descriptor.action);
    }
    const bool offlineReturnEligible =
        g_mode == RunMode::Observe && offlineManagedWanted()
        && !g_netActive && !g_replayActive && !g_replayRoleActive
        && !g_trainingLaunchRequested
        && !qgame::directBootUnattendedFrontendActive();
    if (offlineReturnEligible) {
        enabledTraining &= ~qinput::modeControlBit(
            qinput::ModeControlAction::TrainingReturnCharacterSelect);
        enabledTraining |= qinput::modeControlBit(
            qinput::ModeControlAction::OnlineReturnCharacterSelect);
    }
    const qinput::ModeControlEvaluation modeEvaluation =
        g_modeControlEvaluator.evaluate(
            trainingKeyboard, trainingController, trainingFocus,
            g_inputLaunchConfig.focusPolicy, enabledTraining);
    netOfflineReturnToCharacterSelectTick(
        offlineReturnEligible
        && (modeEvaluation.edges & qinput::modeControlBit(
                qinput::ModeControlAction::OnlineReturnCharacterSelect))
               != 0u);
    uint32_t heldTrainingActions = 0;
    const auto mapHeld = [&](qinput::ModeControlAction mode,
                             qtraining::TrainingAction action) {
        if ((modeEvaluation.held & qinput::modeControlBit(mode)) != 0)
            heldTrainingActions |= qtraining::trainingActionBit(action);
    };
    mapHeld(qinput::ModeControlAction::TrainingResetPosition,
            qtraining::TrainingAction::ResetPosition);
    mapHeld(qinput::ModeControlAction::TrainingSaveState,
            qtraining::TrainingAction::SaveState);
    mapHeld(qinput::ModeControlAction::TrainingLoadState,
            qtraining::TrainingAction::LoadState);
    mapHeld(qinput::ModeControlAction::TrainingRecordToggle,
            qtraining::TrainingAction::RecordStop);
    mapHeld(qinput::ModeControlAction::TrainingPlaybackToggle,
            qtraining::TrainingAction::PlayStop);
    mapHeld(qinput::ModeControlAction::TrainingPreviousSlot,
            qtraining::TrainingAction::PreviousSlot);
    mapHeld(qinput::ModeControlAction::TrainingNextSlot,
            qtraining::TrainingAction::NextSlot);
    mapHeld(qinput::ModeControlAction::TrainingPause,
            qtraining::TrainingAction::TogglePause);
    mapHeld(qinput::ModeControlAction::TrainingFrameStep,
            qtraining::TrainingAction::FrameStep);
    mapHeld(qinput::ModeControlAction::TrainingReturnCharacterSelect,
            qtraining::TrainingAction::ReturnToCharacterSelect);
    mapHeld(qinput::ModeControlAction::TrainingCycleStance,
            qtraining::TrainingAction::CycleDummyStance);
    mapHeld(qinput::ModeControlAction::TrainingCycleGuard,
            qtraining::TrainingAction::CycleGuardMode);
    mapHeld(qinput::ModeControlAction::TrainingRefill,
            qtraining::TrainingAction::RefillAll);
    mapHeld(qinput::ModeControlAction::TrainingChargeExFraction,
            qtraining::TrainingAction::ChargeExFraction);
    mapHeld(qinput::ModeControlAction::TrainingResetRoundStart,
            qtraining::TrainingAction::ResetPositionRoundStart);
    mapHeld(qinput::ModeControlAction::TrainingToggleHitboxes,
            qtraining::TrainingAction::ToggleHitboxes);
    trainingObserveOfflinePhysicalSnapshot(
        static_cast<uint32_t>(g_offlinePlayers[0].mask),
        static_cast<uint32_t>(g_offlinePlayers[1].mask),
        g_offlinePlayers[0].physicalNeutralBeforeSocd,
        heldTrainingActions);

    if ((modeEvaluation.edges & qinput::modeControlBit(
             qinput::ModeControlAction::OpenOverlayMenuPad)) != 0) {
        qinput::menuOverlayToggle();
    }

    static qinput::MenuNavigationForwarder s_menuNav;
    if (qinput::menuOverlayVisible()) {
        qinput::menuNavPublishLabels(g_inputLaunchConfig);
        const uint32_t menuMask =
            static_cast<uint32_t>(g_offlinePlayers[0].mask)
            | static_cast<uint32_t>(g_offlinePlayers[1].mask);
        qinput::menuSubmitNavigationMask(
            s_menuNav.step(menuMask, static_cast<uint64_t>(nowMs)));
    } else {
        s_menuNav.reset();
    }
}

int __fastcall hkNativeInputInit(void* hwnd, void* edxUnused) {
    (void)edxUnused;
    if (g_replayRoleActive)
        g_replayTakeoverInputWindow = static_cast<HWND>(hwnd);
    if (!managedInputRole()) {
        return g_origInputInit != nullptr
            ? g_origInputInit(hwnd, nullptr) : 0;
    }
    wr32(0x4AA4C8u, static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(hwnd)));
    bool preInitClean = rd32(0x4AA5DCu) == 0 && rd32(0x4AAA20u) == 0
        && rd32(0x4AAA24u) == 0 && rd32(0x4AAA28u) == 0;
    wr32(0x4AA6F8u, 0);
    std::memset(reinterpret_cast<void*>(kNativeKeyStateAddrManaged), 0,
                0x100);
    wr32(0x4AA80Cu, 0);
    wr32(0x4AA910u, 0);
    wr32(0x4AA914u, 0);
    for (uint32_t offset = 0; offset < 16; offset += 4)
        wr32(0x4AB758u + offset, 0);
    for (uint32_t offset = 0; offset < 16; offset += 4)
        wr32(0x4ECB20u + offset, 0);
    const bool fCommandPolicyReady =
        !g_netActive || netFCommandPolicyReady();
    bool bootstrapOk = preInitClean && fCommandPolicyReady;
    if (g_netActive && !fCommandPolicyReady) {
        QLOG("[fcommand-policy] Stage-B FAILED before DebugWindow/native "
             "network-mode consumption");
    }
    if (bootstrapOk && !g_inputSpectatorRole && !g_replayRoleActive) {
        g_inputEvaluator = new (std::nothrow) qinput::InputEvaluator();
        if (g_inputEvaluator != nullptr) {
            g_inputEvaluator->configure(&g_inputLaunchConfig);
        }
        g_inputKeyboardWorker =
            new (std::nothrow) qinput::KeyboardWorker();
        if (g_inputKeyboardWorker != nullptr) {
            qinput::KeyboardWorkerConfig workerConfig{};
            workerConfig.backend =
                qinput::createDirectInput8KeyboardBackend(hwnd);
            workerConfig.nowMs = &inputNowMs;
            if (!g_inputKeyboardWorker->start(workerConfig))
                bootstrapOk = false;
        } else {
            bootstrapOk = false;
        }
        if (bootstrapOk && g_inputLaunchConfig.probeCount > 0) {
            const DWORD startedMs = GetTickCount();
            qinput::SdlBootstrapResult sdl =
                qinput::SdlBootstrapResult::NotRequired;
            if (qinput::compiledControllerNeedsSdl(
                    g_inputLaunchConfig.probes,
                    g_inputLaunchConfig.probeCount,
                    g_inputLaunchConfig.probeBackend)) {
                sdl = qinput::sdlBootstrapOnMainThread();
            }
            const DWORD elapsedMs = GetTickCount() - startedMs;
            char sdlVersion[64] = "not required";
            if (sdl != qinput::SdlBootstrapResult::NotRequired)
                qinput::sdlVersionText(sdlVersion, sizeof(sdlVersion));
            QLOG("[input-v2] controller runtime: backend=%s sdl=%s "
                 "bootstrap=%d probes=%u enumeration=%lums "
                 "(main-thread SDL init; Stage-B stays pending until the "
                 "controller worker publishes Ready or a typed "
                 "Safe-Unavailable result)",
                 qinput::controllerBackendName(), sdlVersion,
                 static_cast<int>(sdl), g_inputLaunchConfig.probeCount,
                 static_cast<unsigned long>(elapsedMs));
            g_inputControllerWorker =
                new (std::nothrow) qinput::ControllerWorker();
            if (g_inputControllerWorker != nullptr) {
                qinput::ControllerWorkerConfig controllerConfig{};
                controllerConfig.backend = inputCreateBackendFor(
                    g_inputLaunchConfig.probes,
                    g_inputLaunchConfig.probeCount,
                    g_inputLaunchConfig.probeBackend, hwnd, 1u);
                controllerConfig.nowMs = &inputNowMs;
                controllerConfig.matcher = &g_inputLaunchConfig.device;
                controllerConfig.probes = g_inputLaunchConfig.probes;
                controllerConfig.probeCount =
                    g_inputLaunchConfig.probeCount;
                controllerConfig.mapping =
                    g_inputLaunchConfig.mappingLength > 0
                        ? g_inputLaunchConfig.mapping : nullptr;
                controllerConfig.mappingLength =
                    g_inputLaunchConfig.mappingLength;
                if (!g_inputControllerWorker->start(controllerConfig)) {
                    QLOG("[input-v2] controller worker FAILED to start; "
                         "controller input unavailable this session");
                } else {
                    constexpr DWORD kAttachBudgetMs = 3000;
                    const DWORD attachStartedMs = GetTickCount();
                    qinput::ControllerAttachState attach =
                        g_inputControllerWorker->attachState();
                    while (attach == qinput::ControllerAttachState::Starting
                           || attach
                               == qinput::ControllerAttachState::NoCandidate) {
                        if (GetTickCount() - attachStartedMs
                            >= kAttachBudgetMs) {
                            break;
                        }
                        Sleep(25);
                        attach = g_inputControllerWorker->attachState();
                    }
                    const DWORD attachMs = GetTickCount() - attachStartedMs;
                    const char* attachText = "unknown";
                    switch (attach) {
                        case qinput::ControllerAttachState::Attached:
                            attachText = "attached"; break;
                        case qinput::ControllerAttachState::Starting:
                            attachText = "still starting"; break;
                        case qinput::ControllerAttachState::NoCandidate:
                            attachText = "NO MATCHING DEVICE"; break;
                        case qinput::ControllerAttachState::Ambiguous:
                            attachText = "AMBIGUOUS - two or more "
                                         "indistinguishable devices"; break;
                        case qinput::ControllerAttachState::OpenFailed:
                            attachText = "OPEN FAILED"; break;
                        case qinput::ControllerAttachState::LayoutChanged:
                            attachText = "LAYOUT CHANGED"; break;
                        case qinput::ControllerAttachState::BackendUnavailable:
                            attachText = "BACKEND UNAVAILABLE"; break;
                    }
                    if (attach == qinput::ControllerAttachState::Attached) {
                        QLOG("[input-v2] controller ATTACHED after %lums - "
                             "the configured controller is live in the game "
                             "process",
                             static_cast<unsigned long>(attachMs));
                    } else {
                        QLOG("[input-v2] controller NOT ATTACHED after %lums: "
                             "%s. The game starts on keyboard input; the "
                             "controller stays neutral until it attaches. "
                             "candidates=%u",
                             static_cast<unsigned long>(attachMs), attachText,
                             g_inputControllerWorker->lastCandidateCount());
                    }
                }
            }
        }
    }
    if (bootstrapOk && hwnd != nullptr) {
        g_inputPrevWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(
            static_cast<HWND>(hwnd), GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&inputFocusWndProc)));
        if (g_inputPrevWndProc == nullptr) bootstrapOk = false;
        g_inputFocusGeneration =
            (g_inputFocusGeneration + 1u) & 0x7FFFFFFFu;
        g_inputWindowFocus.publish(netProcessOwnsForegroundWindow(),
                                   g_inputFocusGeneration);
    }
    wchar_t eventName[64];
    if (bootstrapOk) {
        qipc::ipcInputReadyEventName(eventName, 64,
                                     GetCurrentProcessId());
    } else {
        qipc::ipcInputFailedEventName(eventName, 64,
                                      GetCurrentProcessId());
    }
    HANDLE stageEvent =
        OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    if (stageEvent != nullptr) {
        SetEvent(stageEvent);
        CloseHandle(stageEvent);
    }
    g_inputRuntimeReady = bootstrapOk;
    bool seededFocus = false;
    uint32_t seededFocusGeneration = 0;
    g_inputWindowFocus.readInto(seededFocus, seededFocusGeneration);
    QLOG("[input-v2] managed input bootstrap %s hwnd=%p spectator=%d "
         "keyboardWorker=%d controllerWorker=%d preInitClean=%d "
         "seededFocus=%d",
         bootstrapOk ? "READY" : "FAILED", hwnd,
         g_inputSpectatorRole ? 1 : 0,
         g_inputKeyboardWorker != nullptr
             && g_inputKeyboardWorker->running() ? 1 : 0,
         g_inputControllerWorker != nullptr
             && g_inputControllerWorker->running() ? 1 : 0,
         preInitClean ? 1 : 0, seededFocus ? 1 : 0);
    return bootstrapOk ? 1 : 0;
}

int __cdecl hkNativePresenceProbe() {
    if (managedInputRole()) {
        ++g_nativeInputBypasses;
        for (uint32_t offset = 0; offset < 16; offset += 4)
            wr32(0x4AB758u + offset, 0);
        return 0;
    }
    if (qgame::replay::replayRuntimeActive())
        qgame::replay::replayNoteDeviceCall("native-presence-probe");
    return g_origPresenceProbe != nullptr ? g_origPresenceProbe() : 0;
}

int __fastcall hkNativeJoyLowState(int index, int out) {
    if (managedInputRole()) {
        ++g_nativeInputBypasses;
        return 0;
    }
    if (qgame::replay::replayRuntimeActive())
        qgame::replay::replayNoteDeviceCall("native-joy-low-state");
    return g_origJoyLow != nullptr ? g_origJoyLow(index, out) : 0;
}

int __cdecl hkNativeFullPoll() {
    if (managedInputRole()) {
        ++g_nativeInputBypasses;
        std::memset(reinterpret_cast<void*>(kNativeKeyStateAddrManaged), 0,
                    0x100);
        for (uint32_t offset = 0; offset < 16; offset += 4)
            wr32(0x4ECB20u + offset, 0);
        applyNativeDeveloperEdgePolicy();
        return 1;
    }
    if (qgame::replay::replayRuntimeActive())
        qgame::replay::replayNoteDeviceCall("native-full-poll");
    const int result = g_origFullPoll != nullptr ? g_origFullPoll() : 1;
    mirrorInteractiveOfflineDeveloperKeysFromManagedSnapshot();
    applyNativeDeveloperEdgePolicy();
    return result;
}

static UINT WINAPI stubJoyGetNumDevs() {
    if (qgame::replay::replayRuntimeActive())
        qgame::replay::replayNoteDeviceCall("joyGetNumDevs-stub");
    return 0;
}
static MMRESULT WINAPI stubJoyGetDevCapsA(UINT_PTR, LPJOYCAPSA, UINT) {
    if (qgame::replay::replayRuntimeActive())
        qgame::replay::replayNoteDeviceCall("joyGetDevCapsA-stub");
    return MMSYSERR_NODRIVER;
}
static MMRESULT WINAPI stubJoyGetPos(UINT, LPJOYINFO) {
    if (qgame::replay::replayRuntimeActive())
        qgame::replay::replayNoteDeviceCall("joyGetPos-stub");
    return MMSYSERR_NODRIVER;
}
static MMRESULT WINAPI stubJoyGetPosEx(UINT, LPJOYINFOEX) {
    if (qgame::replay::replayRuntimeActive())
        qgame::replay::replayNoteDeviceCall("joyGetPosEx-stub");
    return MMSYSERR_NODRIVER;
}

struct QohImportPatch {
    const char* module = nullptr;
    const char* function = nullptr;
    qplat::IatSlot slot{};
};
constexpr size_t kMaxQohImportPatches = 24;
static QohImportPatch g_qohImportPatches[kMaxQohImportPatches];
static size_t g_qohImportPatchCount = 0;

static bool patchQohImport(const char* module, const char* function,
                           void* replacement, void** originalOut) {
    if (originalOut != nullptr) *originalOut = nullptr;
    if (g_qohImportPatchCount >= kMaxQohImportPatches) {
        QLOG("[iat] REFUSING %s!%s: patch registry full (%zu)",
             module, function, kMaxQohImportPatches);
        return false;
    }
    void* host = reinterpret_cast<void*>(GetModuleHandleW(nullptr));
    if (host == nullptr) {
        QLOG("[iat] REFUSING %s!%s: no host module handle", module, function);
        return false;
    }
    qplat::IatSlot slot{};
    std::string error;
    if (!qplat::findImportSlot(host, module, function, slot, error)) {
        QLOG("[iat] REFUSING to intercept %s!%s: %s "
             "(fail-closed; a system-code detour is never an acceptable "
             "substitute)",
             module, function, error.c_str());
        return false;
    }
    if (!qplat::patchImportSlot(slot, replacement, error)) {
        QLOG("[iat] FAILED to patch %s!%s slot=%p: %s",
             module, function, static_cast<void*>(slot.slot), error.c_str());
        return false;
    }
    QohImportPatch& record = g_qohImportPatches[g_qohImportPatchCount++];
    record.module = module;
    record.function = function;
    record.slot = slot;
    if (originalOut != nullptr) *originalOut = slot.original;
    QLOG("[iat] patched %s!%s slot=%p original=%p -> %p",
         module, function, static_cast<void*>(slot.slot), slot.original,
         replacement);
    return true;
}

static void restoreQohImportPatches() {
    size_t restored = 0;
    size_t failed = 0;
    while (g_qohImportPatchCount > 0) {
        QohImportPatch& record = g_qohImportPatches[--g_qohImportPatchCount];
        std::string error;
        if (qplat::restoreImportSlot(record.slot, error)) {
            ++restored;
        } else {
            ++failed;
            QLOG("[iat] rollback FAILED for %s!%s: %s",
                 record.module ? record.module : "?",
                 record.function ? record.function : "?", error.c_str());
        }
        record = QohImportPatch{};
    }
    if (restored != 0 || failed != 0) {
        QLOG("[iat] rollback restored=%zu failed=%zu", restored, failed);
    }
}

static void patchWinmmImports() {
    int patched = 0;
    if (patchQohImport("winmm.dll", "joyGetNumDevs",
                       reinterpret_cast<void*>(&stubJoyGetNumDevs), nullptr))
        ++patched;
    if (patchQohImport("winmm.dll", "joyGetDevCapsA",
                       reinterpret_cast<void*>(&stubJoyGetDevCapsA), nullptr))
        ++patched;
    if (patchQohImport("winmm.dll", "joyGetPos",
                       reinterpret_cast<void*>(&stubJoyGetPos), nullptr))
        ++patched;
    if (patchQohImport("winmm.dll", "joyGetPosEx",
                       reinterpret_cast<void*>(&stubJoyGetPosEx), nullptr))
        ++patched;
    QLOG("[input-v2] WinMM joystick IAT guards installed (%d entries)",
         patched);
}

void installNativeDeveloperEdgeGuardOnce() {
    if (g_fullPollHooked) return;
    // PER-BUILD: the E8 at +1 calls sub_45D1A0, which is at the same address
    // in both builds, from a site that moved +0x20 in December.
    static const uint8_t kFullPollSigNovember[] = {
        0x56, 0xE8, 0x0A, 0x87, 0xFD, 0xFF,
        0xA1, 0x58, 0xB7, 0x4A, 0x00
    };
    static const uint8_t kFullPollSigDecember[] = {
        0x56, 0xE8, 0xEA, 0x86, 0xFD, 0xFF,
        0xA1, 0x58, 0xB7, 0x4A, 0x00
    };
    static_assert(sizeof(kFullPollSigNovember)
                      == sizeof(kFullPollSigDecember),
                  "both builds must be checked over the same window");
    const uint8_t* const kFullPollSig = seSelectPerBuildSignature(
        kFullPollSigNovember, kFullPollSigDecember);
    void* target = reinterpret_cast<void*>(g_fullInputPollFn);
    if (!g_fullInputPollFn
        || !se::verifyCodeBytes(g_fullInputPollFn, kFullPollSig,
                                sizeof(kFullPollSigNovember))) {
        QLOG("[input-v2] PROLOGUE MISMATCH at full poll %08X",
             static_cast<unsigned>(g_fullInputPollFn));
    } else if (MH_CreateHook(
                   target, reinterpret_cast<void*>(&hkNativeFullPoll),
                   reinterpret_cast<void**>(&g_origFullPoll)) == MH_OK
               && MH_QueueEnableHook(target) == MH_OK) {
        g_fullPollHooked = true;
        QLOG("[input-v2] native full-poll policy guard %08X enabled",
             static_cast<unsigned>(g_fullInputPollFn));
    } else {
        QLOG("[input-v2] FAILED to hook full poll %08X",
             static_cast<unsigned>(g_fullInputPollFn));
    }
}

void installManagedInputGuardsOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!g_inputInitHooked) {
        static const uint8_t kInputInitSig[] = {
            0x83, 0xEC, 0x14, 0x56, 0x33, 0xF6, 0x57, 0x56,
            0x68, 0xDC, 0xA5, 0x4A, 0x00, 0x8B, 0xF9, 0x68
        };
        void* t = reinterpret_cast<void*>(kNativeInputInitFn);
        if (!se::verifyCodeBytes(kNativeInputInitFn, kInputInitSig,
                                 sizeof(kInputInitSig))) {
            QLOG("[input-v2] PROLOGUE MISMATCH at input-init sub_45CD20");
        } else if (MH_CreateHook(
                       t, reinterpret_cast<void*>(&hkNativeInputInit),
                       reinterpret_cast<void**>(&g_origInputInit))
                       == MH_OK
                   && MH_QueueEnableHook(t) == MH_OK) {
            g_inputInitHooked = true;
            QLOG("[input-v2] guarded input-init hook on sub_45CD20 "
                 "enabled");
        } else {
            QLOG("[input-v2] FAILED to hook input-init sub_45CD20");
        }
    }
    if (!g_presenceProbeHooked) {
        // The E8 at +2 calls a function that did not move from a site that moved in December, so this signature is per build.
        static const uint8_t kPresenceSigNovember[] = {
            0x33, 0xC9, 0xE8, 0x79, 0x84, 0xFD, 0xFF, 0x48,
            0xB9, 0x01, 0x00, 0x00, 0x00, 0xF7, 0xD8, 0x1B
        };
        static const uint8_t kPresenceSigDecember[] = {
            0x33, 0xC9, 0xE8, 0x59, 0x84, 0xFD, 0xFF, 0x48,
            0xB9, 0x01, 0x00, 0x00, 0x00, 0xF7, 0xD8, 0x1B
        };
        static_assert(sizeof(kPresenceSigNovember)
                          == sizeof(kPresenceSigDecember),
                      "both builds must be checked over the same window");
        const uint8_t* const kPresenceSig = seSelectPerBuildSignature(
            kPresenceSigNovember, kPresenceSigDecember);
        void* t = reinterpret_cast<void*>(g_nativePresenceProbeFn);
        if (!g_nativePresenceProbeFn
            || !se::verifyCodeBytes(g_nativePresenceProbeFn, kPresenceSig,
                                 sizeof(kPresenceSigNovember))) {
            QLOG("[input-v2] PROLOGUE MISMATCH at presence probe %08X",
                 static_cast<unsigned>(g_nativePresenceProbeFn));
        } else if (MH_CreateHook(
                       t,
                       reinterpret_cast<void*>(&hkNativePresenceProbe),
                       reinterpret_cast<void**>(&g_origPresenceProbe))
                       == MH_OK
                   && MH_QueueEnableHook(t) == MH_OK) {
            g_presenceProbeHooked = true;
            QLOG("[input-v2] presence-probe guard on %08X enabled",
                 static_cast<unsigned>(g_nativePresenceProbeFn));
        } else {
            QLOG("[input-v2] FAILED to hook presence probe %08X",
                 static_cast<unsigned>(g_nativePresenceProbeFn));
        }
    }
    if (!g_joyLowHooked) {
        static const uint8_t kJoyLowSig[] = {
            0xA1, 0x14, 0xA9, 0x4A, 0x00, 0x56, 0x8B, 0xF1,
            0x57, 0x3B, 0xF0, 0x8B, 0xFA, 0x72, 0x05, 0x5F
        };
        void* t = reinterpret_cast<void*>(kNativeJoyLowStateFn);
        if (!se::verifyCodeBytes(kNativeJoyLowStateFn, kJoyLowSig,
                                 sizeof(kJoyLowSig))) {
            QLOG("[input-v2] PROLOGUE MISMATCH at joy state sub_45D100");
        } else if (MH_CreateHook(
                       t,
                       reinterpret_cast<void*>(&hkNativeJoyLowState),
                       reinterpret_cast<void**>(&g_origJoyLow)) == MH_OK
                   && MH_QueueEnableHook(t) == MH_OK) {
            g_joyLowHooked = true;
            QLOG("[input-v2] joystick-state guard on sub_45D100 enabled");
        } else {
            QLOG("[input-v2] FAILED to hook joy state sub_45D100");
        }
    }
    installNativeDeveloperEdgeGuardOnce();
    patchWinmmImports();
}

typedef int (__cdecl* PacingFn)();
static PacingFn      g_origPacing = nullptr;
static bool          g_pacingHooked = false;
static volatile int  g_pacingMode = 0;
static int           g_pacingRec[32];
static volatile int  g_pacingRecN = 0;
static volatile int  g_pacingReplayIdx = 0;
int __cdecl hkPacing() {
    if (g_pacingMode == 1) {
        int v = g_origPacing ? g_origPacing() : 1;
        if (g_pacingRecN < 32) g_pacingRec[g_pacingRecN++] = v;
        return v;
    }
    if (g_pacingMode == 2) {
        if (g_pacingReplayIdx < g_pacingRecN) return g_pacingRec[g_pacingReplayIdx++];
        return g_origPacing ? g_origPacing() : 1;
    }
    return g_origPacing ? g_origPacing() : 1;
}

static bool g_pacingReplayEnabled = false;

enum class ReplayTickMode { Natural, Deterministic, PacingShim };
static ReplayTickMode g_replayMode = ReplayTickMode::Natural;
static bool g_clockShimEnabled = true;
static bool g_doubleReplay = false;
static bool g_minimalDet = false;
struct TimingSpan { uintptr_t addr; size_t size; };
static const TimingSpan kTimingSpans[] = {
    { 0x4AA420, 0x40  },
    { 0x4ABA00, 0x520  },
};
constexpr int kPaceSlots = 32;
struct alignas(16) PaceSnap {
    uint32_t d28 = 0, dc4 = 0, d38 = 0, b20 = 0;
    uint8_t  timing[0x40 + 0x520];
    unsigned int fpControl = 0;
    alignas(16) uint8_t fxState[512] = {};
    bool fxValid = false;
};
static PaceSnap g_paceSlots[kPaceSlots];
static bool g_fpuStateShim = false;
static void fxSaveTo(void* buf)      { __asm { mov eax, buf } __asm { fxsave [eax] } }
static void fxRestoreFrom(void* buf) { __asm { mov eax, buf } __asm { fxrstor [eax] } }
struct alignas(16) NetFpuSlot {
    alignas(16) uint8_t state[512] = {};
    VirtualAudioState audio{};
    uint64_t audioFrame = 0;
    uint32_t layout = 0;
    uint32_t epoch = 0;
    uint32_t frame = 0;
    bool valid = false;
};
static NetFpuSlot g_netFpuSlots[kPoolSlots];
static uint32_t g_netFpuInitializedEpoch = 0;
inline void netResetFpuContract() {
    uint16_t cw = 0x027Fu;
    uint32_t mxcsr = 0x1F80u;
    __asm { fninit }
    __asm { fldcw cw }
    __asm { ldmxcsr mxcsr }
}
inline void netCaptureFpu(size_t slot, uint32_t epoch, uint32_t frame) {
    NetFpuSlot& s = g_netFpuSlots[slot % kPoolSlots];
    fxSaveTo(s.state);
    s.audio = g_virtualAudio;
    s.audioFrame = frame;
    s.layout = g_rb.layoutGeneration();
    s.epoch = epoch;
    s.frame = frame;
    s.valid = true;
}
inline bool netRestoreFpu(size_t slot, uint32_t epoch, uint32_t frame) {
    const NetFpuSlot& s = g_netFpuSlots[slot % kPoolSlots];
    if (!s.valid || s.layout != g_rb.layoutGeneration()
        || s.epoch != epoch || s.frame != frame)
        return false;
    g_virtualAudio = s.audio;
    fxRestoreFrom(const_cast<uint8_t*>(s.state));
    return true;
}
inline void netInvalidateFpu() {
    for (NetFpuSlot& s : g_netFpuSlots) s.valid = false;
}

static VirtualAudioState g_rematchTheaterAudio{};

static qnetplay::RematchTheaterObserved netRematchTheaterObserve() {
    qnetplay::RematchTheaterObserved o{};
    o.scene = rd32(kSceneStateAddr);
    o.sceneNext = rd32(0x61801Cu);
    o.formationRow = rd32(0x62369Cu);
    o.matchSub = static_cast<int32_t>(rd32(kMatchSubAddr));
    o.formationActive = rd32(0x6236A0u);
    o.loserSide = rd32(kMatchWinnerSlotAddr);
    o.vacancyMask = rd32(0x6236CCu);
    o.continueOwner = rd32(0x6236C4u);
    o.continueTimer = rd32(0x6236C8u);
    o.committedCpuMask = rd32(0x6236D4u);
    return o;
}

bool netOfflineRematchTheaterFlipped() { return g_offlineRematch.flipped; }

static void netOfflineRematchTick() {
    OfflineRematchTheaterState& t = g_offlineRematch;
    const qnetplay::RematchTheaterObserved o = netRematchTheaterObserve();
    if (o.scene != 2u) {
        if (t.flipped) {
            for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
                wr32(kRematchTheaterDwordAddrs[i], t.savedDwords[i]);
            QLOG("[rematch-offline] scene moved while flipped; restored");
        }
        t = OfflineRematchTheaterState{};
        return;
    }
    if (t.done) {
        if (o.matchSub <= 0) t = OfflineRematchTheaterState{};
        return;
    }
    if (t.nativeRestartArmed) {
        if (o.matchSub <= 0) {
            for (size_t i = 0; i < kRematchTheaterDwordCount; ++i) {
                if (kRematchTheaterDwordAddrs[i] == kMatchSubAddr) continue;
                wr32(kRematchTheaterDwordAddrs[i], t.savedDwords[i]);
            }
            wr32(0x6236A0u, 1u);
            wr32(0x626670u, 0u);
            wr32(0x626674u, 0u);
            t.flipped = false;
            t.done = true;
            QLOG("[rematch-offline] native RESTART armed the rematch "
                 "(matchSub=%d formation=%u cpuMask=%u)",
                 o.matchSub, rd32(0x62369Cu), rd32(0x6236CCu));
        }
        return;
    }
    if (!t.flipped) {
        if (!offlineManagedWanted() || g_netActive || g_replayRoleActive
            || qgame::directBootUnattendedFrontendActive())
            return;
        if (!qnetplay::rematchTheaterDoorwayEligible(o)) return;
        for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
            t.savedDwords[i] = rd32(kRematchTheaterDwordAddrs[i]);
        t.loserSide = o.loserSide & 1u;
        const qnetplay::RematchTheaterFlipPlan plan =
            qnetplay::rematchTheaterFlipPlan(o);
        wr32(0x62369Cu, plan.formationRow);
        wr32(kMatchSubAddr, static_cast<uint32_t>(plan.matchSub));
        wr32(0x61801Cu, plan.sceneNext);
        wr32(0x6236CCu, plan.vacancyMask);
        t.flipped = true;
        netPrepareOverlayHud();
        QLOG("[rematch-offline] prompt OPEN loser=P%u cpuMask=%u->%u "
             "formation=1->0 (native continue screen live)",
             t.loserSide + 1u, o.vacancyMask, plan.vacancyMask);
        return;
    }
    if (!qnetplay::rematchTheaterFlippedStateSane(o, t.loserSide)) {
        for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
            wr32(kRematchTheaterDwordAddrs[i], t.savedDwords[i]);
        t.done = true;
        t.flipped = false;
        QLOG("[rematch-offline] LEAKED EDGE fail-closed: scene=%u next=%u "
             "row=%u matchSub=%d owner=%d mask=%u timer=%u - restored",
             o.scene, o.sceneNext, o.formationRow, o.matchSub,
             static_cast<int32_t>(o.continueOwner), o.vacancyMask,
             o.continueTimer);
        return;
    }
    for (int p = 0; p < 2; ++p) {
        const uint32_t rising = t.raw[p] & ~t.prev[p];
        t.prev[p] = t.raw[p];
        if (!t.ready[p]
            && (rising & (qinput::kLogicalLeft | qinput::kLogicalRight))
                   != 0u) {
            wr32(kRematchTheaterCursorAddr,
                 rd32(kRematchTheaterCursorAddr) ^ 1u);
        }
        if (t.ready[p] || (rising & qinput::kLogicalButtonMask) == 0u)
            continue;
        t.ready[p] = true;
        t.choiceNo[p] = !qnetplay::rematchTheaterCursorIsYes(
            rd32(kRematchTheaterCursorAddr));
        QLOG("[rematch-offline] P%d confirmed %s (cursor=%u)", p + 1,
             t.choiceNo[p] ? "NO/iie" : "YES/hai",
             rd32(kRematchTheaterCursorAddr));
    }
    if (!(t.ready[0] && t.ready[1])) return;
    const bool declined = t.choiceNo[0] || t.choiceNo[1];
    if (declined) {
        for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
            wr32(kRematchTheaterDwordAddrs[i], t.savedDwords[i]);
        t.flipped = false;
        t.done = true;
        QLOG("[rematch-offline] verdict=DECLINED restored: formation=%u "
             "cpuMask=%u matchSub=%d - native VS exit owns the return",
             rd32(0x62369Cu), rd32(0x6236CCu),
             static_cast<int32_t>(rd32(kMatchSubAddr)));
        return;
    }
    wr32(kMatchSubAddr, 5u);
    t.nativeRestartArmed = true;
    QLOG("[rematch-offline] verdict=REMATCH - releasing the native arcade "
         "restart (matchSub -> 5 under the flip)");
}

static bool netRematchTheaterFlip() {
    using namespace qnetplay;
    RematchTheaterState& t = g_rematchTheater;
    t.flipped = false;
    t.regionsSnapshotValid = false;
    t.prevSampleMask = 0xFFFFFFFFu;
    t.fallbackCursorNo = false;
    t.nextTickQpcUs = 0;
    t.asyncTicks = 0;
    t.feedMask = 0;
    const RematchTheaterObserved o = netRematchTheaterObserve();
    if (!rematchTheaterDoorwayEligible(o)) {
        QLOG("[rematch-theater] flip REFUSED (doorway shape): scene=%u "
             "next=%u row=%u matchSub=%d formation=%u loser=%u mask=%08X "
             "owner=%08X timer=%u - freeze-frame fallback",
             o.scene, o.sceneNext, o.formationRow, o.matchSub,
             o.formationActive, o.loserSide, o.vacancyMask, o.continueOwner,
             o.continueTimer);
        return false;
    }
    if (!g_rbReady || g_rb.totalSize() == 0) {
        QLOG("[rematch-theater] flip REFUSED (savestate manager offline) - "
             "freeze-frame fallback");
        return false;
    }
    g_rematchTheaterRegions.resize(g_rb.totalSize());
    if (!g_rb.snapshotTo(g_rematchTheaterRegions.data())) {
        QLOG("[rematch-theater] flip REFUSED (doorway snapshot failed) - "
             "freeze-frame fallback");
        return false;
    }
    t.layoutGeneration = g_rb.layoutGeneration();
    t.regionsSnapshotValid = true;
    for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
        t.savedDwords[i] = rd32(kRematchTheaterDwordAddrs[i]);
    g_rematchTheaterAudio = g_virtualAudio;
    fxSaveTo(t.fpu);
    t.loserSide = o.loserSide & 1u;
    t.doorwayFrame = g_netSession != nullptr
        ? g_netSession->currentFrame() : 0u;
    const RematchTheaterFlipPlan plan = rematchTheaterFlipPlan(o);
    wr32(0x62369Cu, plan.formationRow);
    wr32(0x8719A4u, static_cast<uint32_t>(plan.matchSub));
    wr32(0x61801Cu, plan.sceneNext);
    wr32(0x6236CCu, plan.vacancyMask);
    t.flipped = true;
    QLOG("[rematch-theater] FLIPPED to Arcade continue: frame=%u loser=P%u "
         "row(saved)=%u next(saved)=%u timer=%u mask=%08X->%08X "
         "gateB4=%08X gateB8=%08X snapshot=%zuB",
         t.doorwayFrame, t.loserSide + 1u, t.savedDwords[0],
         t.savedDwords[1], o.continueTimer, o.vacancyMask, plan.vacancyMask,
         rd32(0x6236B4u), rd32(0x6236B8u), g_rematchTheaterRegions.size());
    return true;
}

// Restore order must not change: savestate regions, explicit dwords, then virtual audio and FPU/SSE.
static void netRematchTheaterRestore(const char* why) {
    RematchTheaterState& t = g_rematchTheater;
    if (!t.flipped) return;
    t.flipped = false;
    bool regionsRestored = false;
    if (t.regionsSnapshotValid && g_rbReady
        && t.layoutGeneration == g_rb.layoutGeneration()
        && g_rematchTheaterRegions.size() == g_rb.totalSize()) {
        regionsRestored = g_rb.loadFrom(g_rematchTheaterRegions.data());
    }
    for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
        wr32(kRematchTheaterDwordAddrs[i], t.savedDwords[i]);
    g_virtualAudio = g_rematchTheaterAudio;
    fxRestoreFrom(t.fpu);
    t.regionsSnapshotValid = false;
    QLOG("[rematch-theater] RESTORED doorway (%s): frame=%u asyncTicks=%u "
         "regions=%d matchSub=%d row=%u mask=%08X next=%u",
         why, t.doorwayFrame, t.asyncTicks,
         static_cast<int>(regionsRestored),
         static_cast<int32_t>(rd32(kMatchSubAddr)), rd32(0x62369Cu),
         rd32(0x6236CCu), rd32(0x61801Cu));
}

static bool netRematchTheaterGuard(const char* where) {
    if (!g_rematchTheater.flipped) return true;
    const qnetplay::RematchTheaterObserved o = netRematchTheaterObserve();
    if (qnetplay::rematchTheaterFlippedStateSane(o,
                                                 g_rematchTheater.loserSide))
        return true;
    QLOG("[rematch-theater] LEAKED EDGE (%s): scene=%u next=%u row=%u "
         "matchSub=%d formation=%u owner=%08X mask=%08X timer=%u",
         where, o.scene, o.sceneNext, o.formationRow, o.matchSub,
         o.formationActive, o.continueOwner, o.vacancyMask,
         o.continueTimer);
    netRematchTheaterRestore("leaked-edge fail-closed");
    netFailClosed(kNetInternalFailure,
                  g_netSession != nullptr ? g_netSession->currentFrame()
                                          : 0u,
                  "rematch theater leaked an edge into the native Arcade "
                  "machine", true, false);
    return false;
}

inline uint64_t netFnv1a(const void* bytes, size_t size,
                         uint64_t h = 1469598103934665603ull) {
    const uint8_t* p = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
uint64_t netCanonicalAudioManifestHash(bool includeCharacterVoices) {
    if (includeCharacterVoices) {
        if (g_audioManifestLegacyChecked) return g_audioManifestLegacyHash;
    } else if (g_audioManifestChecked) {
        return g_audioManifestHash;
    }
    if (includeCharacterVoices) g_audioManifestLegacyChecked = true;
    else g_audioManifestChecked = true;

    struct DurationEntry {
        std::string path;
        uint32_t frames = 0;
    };
    std::vector<DurationEntry> entries;
    entries.reserve(31);
    auto normalize = [](std::string value) {
        for (char& ch : value) {
            if (ch == '/') ch = '\\';
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        }
        return value;
    };
    size_t characterVoices = 0;
    WIN32_FIND_DATAA found{};
    HANDLE search = FindFirstFileA(".\\*", &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
                || std::strcmp(found.cFileName, ".") == 0
                || std::strcmp(found.cFileName, "..") == 0)
                continue;
            char path[MAX_PATH] = {};
            std::snprintf(
                path, sizeof(path), ".\\%s\\%s.wav",
                found.cFileName, found.cFileName);
            uint32_t frames = 0;
            if (!readWaveDurationFrames(path, frames)) continue;
            if (includeCharacterVoices) {
                std::string identity = std::string(found.cFileName)
                    + "\\" + found.cFileName + ".wav";
                entries.push_back({ normalize(identity), frames });
            }
            ++characterVoices;
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }

    static constexpr const char* kStatusSounds[] = {
        ".\\System\\wav\\Syskoe\\Round.wav",
        ".\\System\\wav\\Syskoe\\01.wav",
        ".\\System\\wav\\Syskoe\\02.wav",
        ".\\System\\wav\\Syskoe\\03.wav",
        ".\\System\\wav\\Syskoe\\04.wav",
        ".\\System\\wav\\Syskoe\\05.wav",
        ".\\System\\wav\\Syskoe\\DoubleKO.wav",
    };
    static constexpr int kStatusSoundIds[] = {
        162, 166, 167, 168, 169, 170, 171,
    };
    static_assert(
        std::size(kStatusSounds) == std::size(kStatusSoundIds),
        "system announcement manifest mapping drift");
    bool complete = true;
    for (size_t index = 0; index < std::size(kStatusSounds); ++index) {
        uint32_t frames = 0;
        const bool valid =
            readWaveDurationFrames(kStatusSounds[index], frames);
        if (valid) {
            entries.push_back({
                normalize(kStatusSounds[index] + 2), frames,
            });
            const int statusIndex =
                qgame::simulationStatusSoundIndex(kStatusSoundIds[index]);
            if (statusIndex >= 0) {
                g_statusSoundDurationFrames[
                    static_cast<size_t>(statusIndex)] = frames;
            }
        } else {
            QLOG("[presentation] sound file not readable: '%s' (sound id "
                 "%d). It will not play. This does NOT affect the match: the "
                 "simulation uses a canonical length for every status sound "
                 "and never reads this file.",
                 kStatusSounds[index], kStatusSoundIds[index]);
        }
        complete = valid && complete;
    }
    if (!complete) {
        QLOG("[presentation] canonical audio manifest invalid: %zu of %zu "
             "simulation-timing sounds present (characterVoices=%zu, %s)",
             entries.size(), std::size(kStatusSounds), characterVoices,
             includeCharacterVoices ? "hashed (pre-V2 replay)" : "not hashed");
        if (includeCharacterVoices) g_audioManifestLegacyHash = 0;
        else g_audioManifestHash = 0;
        return 0;
    }

    std::sort(
        entries.begin(), entries.end(),
        [](const DurationEntry& left, const DurationEntry& right) {
            return left.path < right.path;
        });
    // The V1 domain string must stay byte-exact: stored replay proofs are recomputed from it.
    static constexpr char kDomainV1[] =
        "QOH99_CANONICAL_AUDIO_DURATIONS_V1";
    static constexpr char kDomainV2[] =
        "QOH99_CANONICAL_AUDIO_DURATIONS_V2_STATUS_ONLY";
    const char* const domain =
        includeCharacterVoices ? kDomainV1 : kDomainV2;
    const size_t domainLength =
        (includeCharacterVoices ? sizeof(kDomainV1) : sizeof(kDomainV2)) - 1u;
    uint64_t hash = netFnv1a(domain, domainLength);
    const uint32_t count = static_cast<uint32_t>(entries.size());
    hash = netFnv1a(&count, sizeof(count), hash);
    for (const DurationEntry& entry : entries) {
        const uint32_t pathBytes =
            static_cast<uint32_t>(entry.path.size());
        hash = netFnv1a(&pathBytes, sizeof(pathBytes), hash);
        hash = netFnv1a(entry.path.data(), entry.path.size(), hash);
        hash = netFnv1a(&entry.frames, sizeof(entry.frames), hash);
    }
    const uint64_t resolved = hash != 0 ? hash : 0x514F483939415544ull;
    if (includeCharacterVoices) g_audioManifestLegacyHash = resolved;
    else g_audioManifestHash = resolved;
    QLOG("[presentation] canonical audio manifest%s=%016llX entries=%u "
         "(%zu simulation-timing + %zu character voices%s)",
         includeCharacterVoices ? "[pre-V2]" : "",
         static_cast<unsigned long long>(resolved), count,
         std::size(kStatusSounds), characterVoices,
         includeCharacterVoices ? ", hashed" : ", not hashed");
    return resolved;
}
uint64_t netCanonicalFpuHash(const NetFpuSlot& slot) {
    // Hash only FCW plus MXCSR control bits (mask sticky status bits 0..5), the stable FP environment TrialNetplay also fingerprints.
    uint16_t controlWord = 0;
    uint32_t mxcsr = 0;
    std::memcpy(&controlWord, slot.state, sizeof(controlWord));
    std::memcpy(&mxcsr, slot.state + 24, sizeof(mxcsr));
    mxcsr &= ~0x3Fu;
    uint8_t canonical[6] = {};
    std::memcpy(canonical, &controlWord, sizeof(controlWord));
    std::memcpy(canonical + 2, &mxcsr, sizeof(mxcsr));
    return netFnv1a(canonical, sizeof(canonical));
}
uint64_t netTaggedPreHash(size_t slot, uint32_t epoch, uint32_t frame) {
    const NetFpuSlot& fpu = g_netFpuSlots[slot % kPoolSlots];
    if (!fpu.valid || fpu.layout != g_rb.layoutGeneration()
        || fpu.epoch != epoch || fpu.frame != frame)
        return 0;
    uint64_t h = g_rb.hashSlot(slot, RollbackManager::Profile::Gameplay);
    const uint64_t fpuHash = netCanonicalFpuHash(fpu);
    h = netFnv1a(&fpuHash, sizeof(fpuHash), h);
    const VirtualAudioState audio =
        canonicalVirtualAudioForHash(fpu.audio, fpu.audioFrame);
    return netFnv1a(&audio, sizeof(audio), h);
}

uint64_t netLiveTaggedGameplayHash(uint64_t audioFrame) {
    uint64_t hash =
        g_rb.hashLive(nullptr, RollbackManager::Profile::Gameplay);
    NetFpuSlot fpu{};
    fxSaveTo(fpu.state);
    const uint64_t fpuHash = netCanonicalFpuHash(fpu);
    hash = netFnv1a(&fpuHash, sizeof(fpuHash), hash);
    const VirtualAudioState audio =
        canonicalVirtualAudioForHash(g_virtualAudio, audioFrame);
    return netFnv1a(&audio, sizeof(audio), hash);
}

static void netLogCompositeHashProbe(const char* tag, uint32_t cursor) {
    std::vector<RollbackManager::RegionHash> per;
    const uint64_t rb = g_rb.hashLive(&per, RollbackManager::Profile::Gameplay);
    NetFpuSlot fpu{};
    fxSaveTo(fpu.state);
    const uint64_t fpuHash = netCanonicalFpuHash(fpu);
    const VirtualAudioState audio =
        canonicalVirtualAudioForHash(g_virtualAudio, cursor);
    uint64_t audioSig = netFnv1a(&audio, sizeof(audio));
    QLOG("[ckhash] %s cursor=%u rb=%016llX fpu=%016llX audio=%016llX",
         tag, cursor,
         static_cast<unsigned long long>(rb),
         static_cast<unsigned long long>(fpuHash),
         static_cast<unsigned long long>(audioSig));
    for (const auto& rh : per) {
        QLOG("[ckhash] %s region=%s h=%016llX present=%d",
             tag, rh.name,
             static_cast<unsigned long long>(rh.hash),
             rh.present ? 1 : 0);
    }
}

// Checkpoint audio is region id 3: virtual-audio deadlines serialized as remainders relative to the capture cursor.

bool netRestoreCheckpointAudioRegion(const uint8_t* bytes, size_t byteCount,
                                     uint32_t recordCursor) {
    if (bytes == nullptr || byteCount < 4u) return false;
    uint32_t count = 0;
    std::memcpy(&count, bytes, 4u);
    if (count != g_virtualAudio.statusUntil.size()
        || byteCount != 4u + sizeof(uint64_t) * count) {
        return false;
    }
    g_virtualAudio = VirtualAudioState{};
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t remainder = 0;
        std::memcpy(&remainder, bytes + 4u + sizeof(uint64_t) * i,
                    sizeof(remainder));
        g_virtualAudio.statusUntil[i] =
            remainder == 0u
                ? 0u
                : remainder == kVirtualSoundLooping
                    ? kVirtualSoundLooping
                    : recordCursor + remainder;
    }
    return true;
}

static constexpr std::array<uintptr_t, 31> kNetFrontendFields = {
    0x6234ECu,
    0x61801Cu,
    0x4AB8C4u,
    0x4AB8D0u,
    0x4AA454u, 0x4AA458u, 0x4AA450u, 0x49EF00u,
    0x62369Cu, 0x6236A0u, 0x6236A8u, 0x6236B0u,
    0x6236CCu, 0x6236ECu, 0x6236F0u, 0x6236F4u,
    0x4A37B4u, 0x4A37B8u, 0x4A37BCu, 0x4A37C0u,
    0x4A37C4u, 0x4A37C8u, 0x4A37CCu, 0x4A37D0u,
    0x613F10u, 0x613F14u, 0x613F18u, 0x613F1Cu,
    0x8716D0u, 0x8716D4u, 0x8716E8u,
};

struct NetFrontendFieldOverride {
    size_t index = 0u;
    uint32_t value = 0u;
};

enum class AudioManifestRule {
    None = 0,
    StatusSoundsOnly,
    WithCharacterVoices,
};

uint64_t netFrontendFingerprint(bool includeReconciledSeeds,
                                uint64_t audioFrame,
                                AudioManifestRule audioRule =
                                    AudioManifestRule::None,
                                const NetFrontendFieldOverride* override_ =
                                    nullptr) {
    uint64_t hash = 1469598103934665603ull;
    if (audioRule != AudioManifestRule::None) {
        const uint64_t audioManifest = netCanonicalAudioManifestHash(
            audioRule == AudioManifestRule::WithCharacterVoices);
        if (audioManifest == 0) return 0;
        hash = netFnv1a(&audioManifest, sizeof(audioManifest), hash);
    }
    for (size_t index = 0u; index < kNetFrontendFields.size(); ++index) {
        const uint32_t address32 =
            static_cast<uint32_t>(kNetFrontendFields[index]);
        const uint32_t value =
            (override_ != nullptr && override_->index == index)
                ? override_->value
                : rd32(kNetFrontendFields[index]);
        hash = netFnv1a(&address32, sizeof(address32), hash);
        hash = netFnv1a(&value, sizeof(value), hash);
    }
    if (includeReconciledSeeds) {
        static constexpr std::array<uintptr_t, 3> kSeedFields = {
            0x4A9938u, 0x4A993Cu, 0x4ABB24u,
        };
        for (const uintptr_t address : kSeedFields) {
            const uint32_t address32 = static_cast<uint32_t>(address);
            const uint32_t value = rd32(address);
            hash = netFnv1a(&address32, sizeof(address32), hash);
            hash = netFnv1a(&value, sizeof(value), hash);
        }
    }
    const uint32_t phase = netPhaseTag();
    hash = netFnv1a(&phase, sizeof(phase), hash);
    NetFpuSlot fpu{};
    fxSaveTo(fpu.state);
    const uint64_t fpuHash = netCanonicalFpuHash(fpu);
    hash = netFnv1a(&fpuHash, sizeof(fpuHash), hash);
    if (audioRule != AudioManifestRule::None) {
        const VirtualAudioState audio =
            canonicalVirtualAudioForHash(g_virtualAudio, audioFrame);
        hash = netFnv1a(&audio, sizeof(audio), hash);
    } else {
        (void)audioFrame;
    }
    return hash != 0 ? hash : 0x514E50352D533000ull;
}
const char* audioManifestRuleName(const AudioManifestRule rule) {
    switch (rule) {
        case AudioManifestRule::None: return "current (no sound files)";
        case AudioManifestRule::StatusSoundsOnly: return "status sounds only";
        case AudioManifestRule::WithCharacterVoices:
            return "with character voices";
    }
    return "unknown";
}

void netExplainBootstrapFingerprintMismatch(const uint64_t recorded,
                                            const uint64_t observed) {
    if (recorded == 0u || recorded == observed) return;
    static constexpr AudioManifestRule kRules[] = {
        AudioManifestRule::None,
        AudioManifestRule::StatusSoundsOnly,
        AudioManifestRule::WithCharacterVoices,
    };
    for (const AudioManifestRule rule : kRules) {
        const char* const ruleName = audioManifestRuleName(rule);
        if (netFrontendFingerprint(false, g_ovrFrame, rule) == recorded) {
            QLOG("[replay.explain] the recorded bootstrap proof is reproduced "
                 "by this installation under the '%s' audio rule; nothing "
                 "else differs.", ruleName);
            return;
        }
        for (size_t index = 0u; index < kNetFrontendFields.size(); ++index) {
            const uint32_t current =
                rd32(kNetFrontendFields[index]);
            for (uint32_t candidate = 0u; candidate <= 0xFFFFu; ++candidate) {
                NetFrontendFieldOverride override_{ index, candidate };
                if (netFrontendFingerprint(false, g_ovrFrame, rule,
                                           &override_) != recorded) {
                    continue;
                }
                QLOG("[replay.explain] the bootstrap proof differs in EXACTLY "
                     "one frontend field: address=%08X recorded=%08X "
                     "now=%08X (audio rule='%s'). Everything else in the "
                     "proof is identical.",
                     static_cast<uint32_t>(kNetFrontendFields[index]),
                     candidate, current, ruleName);
                return;
            }
            for (unsigned bit = 0u; bit < 32u; ++bit) {
                NetFrontendFieldOverride override_{
                    index, current ^ (1u << bit) };
                if (netFrontendFingerprint(false, g_ovrFrame, rule,
                                           &override_) != recorded) {
                    continue;
                }
                QLOG("[replay.explain] the bootstrap proof differs in EXACTLY "
                     "one frontend field, by one bit: address=%08X bit=%u "
                     "recorded=%08X now=%08X (audio rule='%s').",
                     static_cast<uint32_t>(kNetFrontendFields[index]), bit,
                     override_.value, current, ruleName);
                return;
            }
        }
    }
    QLOG("[replay.explain] no single frontend field, under any of the three "
         "audio rules, reproduces the recorded bootstrap proof "
         "(recorded=%016llX now=%016llX). Two or more inputs moved, or an "
         "input outside the field table did - the FPU environment, the phase "
         "tag, or the virtual audio state.",
         static_cast<unsigned long long>(recorded),
         static_cast<unsigned long long>(observed));
}

uint64_t netFrontendStateFingerprint(uint64_t audioFrame = g_ovrFrame) {
    return netFrontendFingerprint(true, audioFrame);
}
uint64_t netBootstrapFingerprint() {
    return netFrontendFingerprint(false, g_ovrFrame);
}

bool netSpectatorFrontendTraceEnabled() {
    static int enabled = -1;
    if (enabled < 0) {
        char value[8] = {};
        enabled =
            GetEnvironmentVariableA(
                "QOH99_NP_TRACE_SPECTATOR_FRONTEND",
                value, sizeof(value)) != 0
            && value[0] == '1';
    }
    return enabled != 0;
}

void netTraceSpectatorFrontend(const char* label, uint32_t frame,
                               uint64_t hash) {
    if (!netSpectatorFrontendTraceEnabled()) return;
    NetFpuSlot fpu{};
    fxSaveTo(fpu.state);
    uint16_t controlWord = 0;
    uint32_t mxcsr = 0;
    std::memcpy(&controlWord, fpu.state, sizeof(controlWord));
    std::memcpy(&mxcsr, fpu.state + 24, sizeof(mxcsr));
    QLOG("[spectator-trace] %s frame=%u hash=%016llX phase=%08X "
         "fpu=%04X/%08X",
         label, frame, static_cast<unsigned long long>(hash),
         netPhaseTag(), controlWord, mxcsr);
    for (size_t first = 0; first < kNetFrontendFields.size();
         first += 8) {
        char line[512] = {};
        int used = std::snprintf(
            line, sizeof(line),
            "[spectator-trace] %s frame=%u fields", label, frame);
        const size_t end = (std::min)(
            first + 8, kNetFrontendFields.size());
        for (size_t i = first;
             i < end && used > 0
                 && static_cast<size_t>(used) < sizeof(line);
             ++i) {
            used += std::snprintf(
                line + used, sizeof(line) - static_cast<size_t>(used),
                " %08X=%08X",
                static_cast<unsigned>(kNetFrontendFields[i]),
                rd32(kNetFrontendFields[i]));
        }
        QLOG("%s", line);
    }
    QLOG("[spectator-trace] %s frame=%u audio="
         "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
         label, frame,
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[0]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[1]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[2]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[3]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[4]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[5]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[6]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[7]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[8]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[9]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[10]));
}

uint64_t netSpectatorLiveStateHash(uint64_t audioFrame) {
    if (!g_netGameplaySegmentActive || !rollbackEligible() || !g_rbReady)
        return netFrontendStateFingerprint(audioFrame);

    uint64_t hash =
        g_rb.hashLive(nullptr, RollbackManager::Profile::Gameplay);
    NetFpuSlot fpu{};
    fxSaveTo(fpu.state);
    const uint64_t fpuHash = netCanonicalFpuHash(fpu);
    hash = netFnv1a(&fpuHash, sizeof(fpuHash), hash);
    const VirtualAudioState audio =
        canonicalVirtualAudioForHash(g_virtualAudio, audioFrame);
    hash = netFnv1a(&audio, sizeof(audio), hash);
    return hash != 0 ? hash : 0x514F483939535045ull;
}

void netLogReplayStateComponents(const char* label, uint32_t frame) {
    uint64_t gameplay = 0u;
    if (g_rbReady && rollbackEligible())
        gameplay = g_rb.hashLive(
            nullptr, RollbackManager::Profile::Gameplay);
    NetFpuSlot fpu{};
    fxSaveTo(fpu.state);
    const uint64_t fpuHash = netCanonicalFpuHash(fpu);
    const VirtualAudioState canonicalAudio =
        canonicalVirtualAudioForHash(g_virtualAudio, frame);
    const uint64_t audioHash =
        netFnv1a(&canonicalAudio, sizeof(canonicalAudio));
    QLOG("[replay.components] %s frame=%u gameplay=%016llX "
         "fpu=%016llX audio=%016llX status="
         "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
         label ? label : "unknown", frame,
         static_cast<unsigned long long>(gameplay),
         static_cast<unsigned long long>(fpuHash),
         static_cast<unsigned long long>(audioHash),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[0]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[1]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[2]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[3]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[4]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[5]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[6]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[7]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[8]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[9]),
         static_cast<unsigned long long>(g_virtualAudio.statusUntil[10]));
}

// Profile +376 is the display-flags dword; netplay forces its full-colour bit, so it must be staged to keep QOHcnf.key unchanged.
constexpr std::array<size_t, 12> kQohSessionOverrideOffsets = {
    36u, 40u, 44u, 48u, 64u, 80u, 84u, 88u, 92u, 96u, 112u, 376u,
};
struct NetOriginalConfig {
    bool captured = false;
    uint32_t profile = 0;
    std::array<uint32_t, kQohSessionOverrideOffsets.size()> values{};
};
static NetOriginalConfig g_netOriginalConfig{};

constexpr std::array<size_t, 7> kTrainingConfigOffsets = {
    36u, 64u, 80u, 84u, 88u, 92u, 96u,
};
struct TrainingOriginalConfig {
    bool captured = false;
    uint32_t profile = 0;
    std::array<uint32_t, kTrainingConfigOffsets.size()> values{};
};
static TrainingOriginalConfig g_trainingOriginalConfig{};

bool trainingCaptureOriginalConfig(uint32_t active) {
    if (!g_trainingLaunchRequested
        || !RollbackManager::validQohActiveConfigPointer(active)
        || rd32(RollbackManager::kQohBindingPointerAddress)
            != RollbackManager::expectedQohBindingPointer(active)) {
        return false;
    }
    if (g_trainingOriginalConfig.captured)
        return g_trainingOriginalConfig.profile == active;
    g_trainingOriginalConfig.captured = true;
    g_trainingOriginalConfig.profile = active;
    for (size_t i = 0; i < kTrainingConfigOffsets.size(); ++i) {
        g_trainingOriginalConfig.values[i] =
            rd32(active + kTrainingConfigOffsets[i]);
    }
    QLOG("[training-config] captured profile %u before session overlays",
         static_cast<unsigned>(
             (active - RollbackManager::kQohConfigRecordBase)
             / RollbackManager::kQohConfigRecordStride));
    return true;
}

bool trainingWriteOriginalConfigValues() {
    if (!g_trainingOriginalConfig.captured
        || !RollbackManager::validQohActiveConfigPointer(
            g_trainingOriginalConfig.profile)) {
        return false;
    }
    for (size_t i = 0; i < kTrainingConfigOffsets.size(); ++i) {
        wr32(g_trainingOriginalConfig.profile + kTrainingConfigOffsets[i],
             g_trainingOriginalConfig.values[i]);
    }
    for (size_t i = 0; i < kTrainingConfigOffsets.size(); ++i) {
        if (rd32(g_trainingOriginalConfig.profile
                 + kTrainingConfigOffsets[i])
            != g_trainingOriginalConfig.values[i]) {
            return false;
        }
    }
    return true;
}

bool trainingStageOriginalConfigForSave(
    std::array<uint32_t, kTrainingConfigOffsets.size()>& runtime) {
    if (!g_trainingOriginalConfig.captured)
        return false;
    for (size_t i = 0; i < kTrainingConfigOffsets.size(); ++i) {
        runtime[i] = rd32(
            g_trainingOriginalConfig.profile + kTrainingConfigOffsets[i]);
    }
    if (trainingWriteOriginalConfigValues())
        return true;
    for (size_t i = 0; i < kTrainingConfigOffsets.size(); ++i) {
        wr32(g_trainingOriginalConfig.profile + kTrainingConfigOffsets[i],
             runtime[i]);
    }
    return false;
}

void trainingRestoreRuntimeConfigAfterSave(
    const std::array<uint32_t, kTrainingConfigOffsets.size()>& runtime) {
    if (!g_trainingOriginalConfig.captured)
        return;
    for (size_t i = 0; i < kTrainingConfigOffsets.size(); ++i) {
        wr32(g_trainingOriginalConfig.profile + kTrainingConfigOffsets[i],
             runtime[i]);
    }
}

void trainingRestoreOriginalConfigInMemory() {
    if (!g_trainingOriginalConfig.captured)
        return;
    if (trainingWriteOriginalConfigValues())
        QLOG("[training-config] restored local profile overlays");
    else
        QLOG("[training-config] WARNING: could not restore profile overlays");
    g_trainingOriginalConfig = TrainingOriginalConfig{};
}

using ConfigSaveFn = unsigned int (__fastcall*)(const char*, void*);
static ConfigSaveFn g_origConfigSave = nullptr;
static bool g_configSaveHooked = false;
static uintptr_t g_configSaveFn = 0;

bool netStoreOriginalConfig(uint32_t active, const char* capturePoint) {
    if (!RollbackManager::validQohActiveConfigPointer(active))
        return false;
    if (g_netOriginalConfig.captured)
        return g_netOriginalConfig.profile == active;
    g_netOriginalConfig.captured = true;
    g_netOriginalConfig.profile = active;
    for (size_t i = 0; i < kQohSessionOverrideOffsets.size(); ++i) {
        g_netOriginalConfig.values[i] =
            rd32(active + kQohSessionOverrideOffsets[i]);
    }
    QLOG("[config] captured local profile %u before runtime overrides "
         "(source=%s)",
         static_cast<unsigned>(
             (active - RollbackManager::kQohConfigRecordBase)
             / RollbackManager::kQohConfigRecordStride),
         capturePoint ? capturePoint : "runtime");
    return true;
}

bool netCaptureOriginalConfig(uint32_t active) {
    if (!RollbackManager::validQohActiveConfigPointer(active)
        || rd32(RollbackManager::kQohBindingPointerAddress)
        != RollbackManager::expectedQohBindingPointer(active)) {
        return false;
    }
    return netStoreOriginalConfig(active, "runtime");
}

bool netRefreshFCommandOriginalConfig(uint32_t active) {
    if (!g_netOriginalConfig.captured
        || g_netOriginalConfig.profile != active)
        return false;
    constexpr size_t kFirstFCommandOwnedIndex = 5u;
    static_assert(kQohSessionOverrideOffsets[5] == 80u
                  && kQohSessionOverrideOffsets[11] == 376u,
                  "FCommand-owned profile suffix changed");
    for (size_t i = kFirstFCommandOwnedIndex;
         i < kQohSessionOverrideOffsets.size(); ++i) {
        g_netOriginalConfig.values[i] =
            rd32(active + kQohSessionOverrideOffsets[i]);
    }
    QLOG("[config] refreshed FCommand-owned saved preferences after "
         "an unexpected parser re-entry");
    return true;
}

bool netWriteOriginalConfigValues() {
    if (!g_netOriginalConfig.captured
        || !RollbackManager::validQohActiveConfigPointer(
            g_netOriginalConfig.profile)) {
        return false;
    }
    for (size_t i = 0; i < kQohSessionOverrideOffsets.size(); ++i) {
        wr32(g_netOriginalConfig.profile + kQohSessionOverrideOffsets[i],
             g_netOriginalConfig.values[i]);
    }
    for (size_t i = 0; i < kQohSessionOverrideOffsets.size(); ++i) {
        if (rd32(g_netOriginalConfig.profile
                 + kQohSessionOverrideOffsets[i])
            != g_netOriginalConfig.values[i]) {
            return false;
        }
    }
    return true;
}

bool netStageOriginalConfigForSave(
    std::array<uint32_t, kQohSessionOverrideOffsets.size()>& runtime) {
    if (!g_netOriginalConfig.captured)
        return false;
    for (size_t i = 0; i < kQohSessionOverrideOffsets.size(); ++i) {
        runtime[i] = rd32(
            g_netOriginalConfig.profile + kQohSessionOverrideOffsets[i]);
    }
    if (netWriteOriginalConfigValues())
        return true;
    for (size_t i = 0; i < kQohSessionOverrideOffsets.size(); ++i) {
        wr32(g_netOriginalConfig.profile + kQohSessionOverrideOffsets[i],
             runtime[i]);
    }
    return false;
}

void netRestoreRuntimeConfigAfterSave(
    const std::array<uint32_t,
                     kQohSessionOverrideOffsets.size()>& runtime) {
    if (!g_netOriginalConfig.captured)
        return;
    for (size_t i = 0; i < kQohSessionOverrideOffsets.size(); ++i) {
        wr32(g_netOriginalConfig.profile + kQohSessionOverrideOffsets[i],
             runtime[i]);
    }
}

void netRestoreOriginalConfigInMemory() {
    if (!g_netOriginalConfig.captured)
        return;
    if (netWriteOriginalConfigValues())
        QLOG("[config] restored local profile runtime overrides");
    else
        QLOG("[config] WARNING: could not restore local profile overrides");
    g_netOriginalConfig = NetOriginalConfig{};
}

constexpr uintptr_t kFCommandParseAddress = 0x46ED90u;
constexpr uintptr_t kQohWaitTimeModeAddress = 0x4A749Cu;
constexpr uintptr_t kQohWaitPeriodAddress = 0x4A74A0u;
constexpr uintptr_t kQohNetworkModeAddress = 0x4AAA50u;
constexpr uintptr_t kQohPrintDecDamageAddress = 0x4AB768u;
constexpr uintptr_t kQohPrintMasterDamageAddress = 0x4AB76Cu;
constexpr uintptr_t kQohComVsAsobiAddress = 0x4AB770u;

using FCommandParseFn = unsigned int (__thiscall*)(const char*);
static FCommandParseFn g_origFCommandParse = nullptr;
static bool g_fCommandPolicyHooked = false;
static bool g_fCommandPolicyApplied = false;

struct NetFCommandPolicySession {
    bool baselineCaptured = false;
    bool normalizationFinalized = false;
    unsigned int parserCalls = 0;
    uint32_t profile = 0;
    qgame::FCommandRiskyState baseline{};
    qgame::FCommandBootPacing bootPacing{};
};
static NetFCommandPolicySession g_netFCommandPolicy{};

bool netReadFCommandRiskyState(
    uint32_t active, qgame::FCommandRiskyState& state) {
    if (!RollbackManager::validQohActiveConfigPointer(active)
        || rd32(RollbackManager::kQohBindingPointerAddress)
            != RollbackManager::expectedQohBindingPointer(active))
        return false;
    state.qohNetworkMode = rd32(kQohNetworkModeAddress);
    state.waitTimeMode = rd32(kQohWaitTimeModeAddress);
    state.waitPeriod = rd32(kQohWaitPeriodAddress);
    state.printDecDamage = rd32(kQohPrintDecDamageAddress);
    state.printMasterDamage = rd32(kQohPrintMasterDamageAddress);
    state.comVsAsobiVersion = rd32(kQohComVsAsobiAddress);
    state.extraFunctionFlags = rd32(active + 80u);
    state.waitCount = rd32(active + 84u);
    state.waitCounter = rd32(active + 88u);
    state.extraSkip = rd32(active + 92u);
    state.extraSkipCounter = rd32(active + 96u);
    state.debugWindow = rd32(active + 112u);
    state.displayFlags = rd32(active + 376u);
    return true;
}

bool netFCommandPolicyReady() {
    if (!g_fCommandPolicyHooked || g_origFCommandParse == nullptr
        || !g_fCommandPolicyApplied
        || !g_netFCommandPolicy.normalizationFinalized) {
        return false;
    }
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    if (active != g_netFCommandPolicy.profile
        || !RollbackManager::validQohActiveConfigPointer(active)
        || rd32(RollbackManager::kQohBindingPointerAddress)
            != RollbackManager::expectedQohBindingPointer(active)) {
        return false;
    }
    qgame::FCommandRiskyState state{};
    return netReadFCommandRiskyState(active, state)
        && qgame::netplayFCommandDefaultsApplied(
            state, g_netFCommandPolicy.bootPacing);
}

void netWriteFCommandRiskyState(
    uint32_t active, const qgame::FCommandRiskyState& state) {
    // Profile +376 also carries NoSound and Vsync: only bit 0 may be set, bits 1 and 2 stay the player's.
    wr32(kQohNetworkModeAddress, state.qohNetworkMode);
    wr32(kQohWaitTimeModeAddress, state.waitTimeMode);
    wr32(kQohWaitPeriodAddress, state.waitPeriod);
    wr32(kQohPrintDecDamageAddress, state.printDecDamage);
    wr32(kQohPrintMasterDamageAddress, state.printMasterDamage);
    wr32(kQohComVsAsobiAddress, state.comVsAsobiVersion);
    wr32(active + 80u, state.extraFunctionFlags);
    wr32(active + 84u, state.waitCount);
    wr32(active + 88u, state.waitCounter);
    wr32(active + 92u, state.extraSkip);
    wr32(active + 96u, state.extraSkipCounter);
    wr32(active + 112u, state.debugWindow);
    wr32(active + 376u, state.displayFlags);
}

void netResetFCommandPolicySession() {
    g_netFCommandPolicy = NetFCommandPolicySession{};
    g_fCommandPolicyApplied = false;
}

unsigned int __fastcall hkFCommandParse(const char* directory, void*) {
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    if (!g_netFCommandPolicy.baselineCaptured) {
        if (!netReadFCommandRiskyState(
                active, g_netFCommandPolicy.baseline)) {
            QLOG("[fcommand-policy] FAILED to capture the pre-parser profile");
        } else {
            g_netFCommandPolicy.baselineCaptured = true;
            g_netFCommandPolicy.profile = active;
            g_netFCommandPolicy.bootPacing.waitTimeMode =
                g_netFCommandPolicy.baseline.waitTimeMode;
            g_netFCommandPolicy.bootPacing.waitPeriod =
                g_netFCommandPolicy.baseline.waitPeriod;
        }
    } else if (g_netFCommandPolicy.normalizationFinalized) {
        if (active == g_netFCommandPolicy.profile) {
            netWriteFCommandRiskyState(
                active, g_netFCommandPolicy.baseline);
        } else {
            QLOG("[fcommand-policy] FAILED: active profile changed between "
                 "parser calls (%08X -> %08X)",
                 g_netFCommandPolicy.profile, active);
        }
        g_netFCommandPolicy.normalizationFinalized = false;
        g_fCommandPolicyApplied = false;
    }

    const unsigned int result =
        g_origFCommandParse ? g_origFCommandParse(directory) : 0u;
    ++g_netFCommandPolicy.parserCalls;

    if (result == 0u && g_netFCommandPolicy.parserCalls == 1u) {
        QLOG("[fcommand-policy] LocalConfig unavailable; awaiting the "
             "verified System fallback");
        return result;
    }

    const uint32_t parsedProfile =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    qgame::FCommandRiskyState parsed{};
    if (!g_netFCommandPolicy.baselineCaptured
        || parsedProfile != g_netFCommandPolicy.profile
        || !netReadFCommandRiskyState(parsedProfile, parsed)) {
        QLOG("[fcommand-policy] FAILED to capture parsed values "
             "(call=%u result=%u profile=%08X)",
             g_netFCommandPolicy.parserCalls, result, parsedProfile);
        g_fCommandPolicyApplied = false;
        return result;
    }
    const bool capturedForSave = !g_netOriginalConfig.captured
        ? netStoreOriginalConfig(
            parsedProfile, "post-FCommand parser")
        : netRefreshFCommandOriginalConfig(parsedProfile);
    if (!capturedForSave) {
        QLOG("[fcommand-policy] FAILED to preserve parsed profile values "
             "(call=%u profile=%08X)",
             g_netFCommandPolicy.parserCalls, parsedProfile);
        g_fCommandPolicyApplied = false;
        return result;
    }

    qgame::applyNetplayFCommandDefaults(
        parsed, g_netFCommandPolicy.bootPacing);
    netWriteFCommandRiskyState(parsedProfile, parsed);

    qgame::FCommandRiskyState verified{};
    g_fCommandPolicyApplied =
        netReadFCommandRiskyState(parsedProfile, verified)
        && qgame::netplayFCommandDefaultsApplied(
            verified, g_netFCommandPolicy.bootPacing);
    g_netFCommandPolicy.normalizationFinalized =
        g_fCommandPolicyApplied;
    QLOG("[fcommand-policy] netplay defaults %s after parser call=%u "
         "result=%u (native wait=%08X/%08X; files untouched)",
         g_fCommandPolicyApplied ? "applied" : "FAILED",
         g_netFCommandPolicy.parserCalls, result,
         g_netFCommandPolicy.bootPacing.waitTimeMode,
         g_netFCommandPolicy.bootPacing.waitPeriod);
    return result;
}

void netSealSpectatorArchive(
    qnetplay::spectator::TerminalReason reason);

unsigned int __fastcall hkConfigSave(const char* path, void* edx) {
    netRestoreOfflineInputBindings();
    // Latch before queuing so the pump's snapshot service cannot double-emit the final pacing ledger.
    const bool summaryAlreadyLogged = g_spectatorPacingSummaryLogged;
    g_spectatorPacingSummaryLogged = true;
    qgame::requestSpectatorViewerDetach();
    qgame::requestTestNetworkStatsSnapshot();
    if (!summaryAlreadyLogged)
        netLogPresentationSummary();

    if (g_netActive && !g_netHalted && !g_netSpectatorMode
        && qgame::netLinkEstablished()) {
        QLOG("[netplay] clean local quit during live match; notifying peer");
        qgame::netSendGoodbyeNow(kNetPeerRequested, g_netFrame);
    }

    const bool sidecarPublisherActive =
        g_spectatorServiceMode == 2u
        && qgame::spectator_publish::sidecarPublisherActive();
    if (!g_netSpectatorMode
        && !g_netSpectatorArchiveSealed
        && (sidecarPublisherActive || g_netSpectatorArchiveBegun)) {
        netSealSpectatorArchive(
            g_netHalted
                ? qnetplay::spectator::TerminalReason::ProtocolFailure
                : qnetplay::spectator::TerminalReason::SourceStopped);
    }
    if (sidecarPublisherActive) {
        qgame::spectator_publish::stopSidecarPublisher();
    }

    std::array<uint32_t, kQohSessionOverrideOffsets.size()> runtime{};
    const bool replayOwnsProfile = g_mode == RunMode::Replay;
    const bool staged = (g_netActive || replayOwnsProfile)
        && netStageOriginalConfigForSave(runtime);
    std::array<uint32_t, kTrainingConfigOffsets.size()> trainingRuntime{};
    const bool trainingStaged = g_trainingLaunchRequested
        && trainingStageOriginalConfigForSave(trainingRuntime);
    if ((g_netActive || replayOwnsProfile)
        && g_netOriginalConfig.captured && !staged) {
        QLOG("[config] WARNING: original profile could not be staged for save");
    } else if (staged) {
        QLOG("[config] staged local profile values for QOHcnf.key save");
    }
    if (g_trainingLaunchRequested
        && g_trainingOriginalConfig.captured && !trainingStaged) {
        QLOG("[training-config] WARNING: original profile could not be "
             "staged for save");
    } else if (trainingStaged) {
        QLOG("[training-config] staged local profile values for "
             "QOHcnf.key save");
    }

    unsigned int result = 0;
    __try {
        result = g_origConfigSave
            ? g_origConfigSave(path, edx)
            : 0u;
    } __finally {
        if (staged && g_netActive)
            netRestoreRuntimeConfigAfterSave(runtime);
        if (trainingStaged && g_trainingLaunchRequested)
            trainingRestoreRuntimeConfigAfterSave(trainingRuntime);
    }
    if (g_netActive) {
        if (qgame::qohConfigSaveSucceeded(staged, result)) {
            QLOG("[config] saved QOHcnf.key successfully with local profile "
                 "values preserved result=%u", result);
        } else {
            QLOG("[config] WARNING: QOHcnf.key save did not preserve local "
                 "profile result=%u staged=%u",
                 result, staged ? 1u : 0u);
        }
    }
    if (g_trainingLaunchRequested) {
        if (qgame::qohConfigSaveSucceeded(
                trainingStaged, result)) {
            QLOG("[training-config] saved QOHcnf.key successfully with "
                 "original local profile values result=%u", result);
        } else {
            QLOG("[training-config] WARNING: QOHcnf.key save did not "
                 "preserve original local profile values "
                 "result=%u staged=%u",
                 result, trainingStaged ? 1u : 0u);
        }
    }
    return result;
}

bool netReadActiveMatchConfig(qgame::NetReadyState& ready) {
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    if (!RollbackManager::validQohActiveConfigPointer(active)
        || rd32(RollbackManager::kQohBindingPointerAddress)
            != RollbackManager::expectedQohBindingPointer(active)) {
        return false;
    }
    ready.timerMode = rd32(active + 36u);
    ready.aiDifficulty = rd32(active + 40u);
    ready.lifeMultiplier = rd32(active + 44u);
    ready.roundsToWinMinusOne = rd32(active + 48u);
    return qgame::netReadyMatchConfigValid(ready);
}

void netApplyExtraFunctionPolicy() {
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    if (!RollbackManager::validQohActiveConfigPointer(active)) return;
    if (rd32(active + 80u) != 0u) {
        wr32(active + 80u, 0u);
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            QLOG("[human-match] disabled QOH ExtraFunction F-keys + joystick "
                 "poll for netplay (config+80 -> 0)");
        }
    }
}

bool netNormalizeLocalSchedulerConfig() {
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    if (!RollbackManager::validQohActiveConfigPointer(active)
        || rd32(RollbackManager::kQohBindingPointerAddress)
            != RollbackManager::expectedQohBindingPointer(active)) {
        return false;
    }
    if (!netCaptureOriginalConfig(active))
        return false;
    for (const size_t offset :
         RollbackManager::kQohNetNormalizedOffsets) {
        wr32(active + offset, 0u);
    }
    return RollbackManager::qohNetNormalizedConfigValid(
        reinterpret_cast<const uint8_t*>(
            static_cast<uintptr_t>(active)),
        RollbackManager::kQohConfigGameplayPrefixSize);
}

bool netLocalSchedulerConfigIsNormalized() {
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    return RollbackManager::validQohActiveConfigPointer(active)
        && qgame::netActiveConfigIdentityValid(
            g_netOriginalConfig.captured,
            active,
            g_netOriginalConfig.profile,
            rd32(RollbackManager::kQohBindingPointerAddress),
            RollbackManager::expectedQohBindingPointer(active))
        && RollbackManager::qohNetNormalizedConfigValid(
            reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(active)),
            RollbackManager::kQohConfigGameplayPrefixSize);
}

bool netApplyAuthoritativeMatchConfig(
    const qgame::NetReadyState& ready) {
    if (!qgame::netReadyMatchConfigValid(ready))
        return false;
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    if (!RollbackManager::validQohActiveConfigPointer(active)
        || rd32(RollbackManager::kQohBindingPointerAddress)
            != RollbackManager::expectedQohBindingPointer(active)) {
        return false;
    }
    if (!netCaptureOriginalConfig(active))
        return false;
    g_rb.setMatchRulesOverridden(true);
    wr32(active + 36u, ready.timerMode);
    wr32(active + 40u, ready.aiDifficulty);
    wr32(active + 44u, ready.lifeMultiplier);
    wr32(active + 48u, ready.roundsToWinMinusOne);
    return rd32(active + 36u) == ready.timerMode
        && rd32(active + 40u) == ready.aiDifficulty
        && rd32(active + 44u) == ready.lifeMultiplier
        && rd32(active + 48u) == ready.roundsToWinMinusOne;
}

std::vector<uint8_t> netSpectatorMetadata(const qgame::NetReadyState& ready,
                                          uint32_t initialPhase) {
    qnetplay::spectator::ReadyMetadata metadata{};
    metadata.seedGameplay = ready.seedGameplay;
    metadata.seedCosmetic = ready.seedCosmetic;
    metadata.selector = ready.selector;
    metadata.initialPhase = initialPhase;
    metadata.bootstrapHash = ready.bootstrapHash;
    metadata.timerMode = ready.timerMode;
    metadata.aiDifficulty = ready.aiDifficulty;
    metadata.lifeMultiplier = ready.lifeMultiplier;
    metadata.roundsToWinMinusOne = ready.roundsToWinMinusOne;
    for (int lane = 0; lane < 4; ++lane) {
        metadata.playerName[lane][0] = '\0';
        metadata.playerNameCodePage[lane] = 0;
        metadata.slotToSourceRoute[lane] = static_cast<uint8_t>(lane);
    }
    for (int lane = 0; lane < 2; ++lane) {
        std::memcpy(metadata.playerName[lane], g_netPlayerNames[lane],
                    sizeof(metadata.playerName[lane]));
        metadata.playerName[lane][sizeof(metadata.playerName[lane]) - 1] = '\0';
        metadata.playerNameCodePage[lane] = g_netPlayerNameCodePages[lane];
    }
    metadata.vsRow = qipc::kVsRow1v1;
    metadata.cpuMask = 0u;
    metadata.activeLaneMask = 0x3u;
    metadata.activeSlotMask = 0x3u;
    metadata.rosterEpochLow = 0u;
    std::vector<uint8_t> out;
    if (!qnetplay::spectator::encodeReadyMetadata(
            metadata, out)) {
        out.clear();
    }
    return out;
}

bool netParseSpectatorMetadata(
    const qnetplay::spectator::GameStartPayload& start,
    qgame::NetReadyState& ready, uint32_t& initialPhase) {
    qnetplay::spectator::ReadyMetadata metadata{};
    if (!qnetplay::spectator::decodeReadyMetadata(
            start.metadata, metadata)) {
        return false;
    }
    ready.seedGameplay = metadata.seedGameplay;
    ready.seedCosmetic = metadata.seedCosmetic;
    ready.selector = metadata.selector;
    ready.bootstrapHash = metadata.bootstrapHash;
    ready.timerMode = metadata.timerMode;
    ready.aiDifficulty = metadata.aiDifficulty;
    ready.lifeMultiplier = metadata.lifeMultiplier;
    ready.roundsToWinMinusOne =
        metadata.roundsToWinMinusOne;
    initialPhase = metadata.initialPhase;
    for (int lane = 0; lane < 2; ++lane) {
        std::memcpy(g_netPlayerNames[lane], metadata.playerName[lane],
                    sizeof(g_netPlayerNames[lane]));
        g_netPlayerNames[lane][sizeof(g_netPlayerNames[lane]) - 1] = '\0';
        g_netPlayerNameCodePages[lane] = metadata.playerNameCodePage[lane];
    }
    QLOG("[spectator] GameStart players='%s'(P1) vs '%s'(P2) vsRow=%u "
         "activeLanes=%02X activeSlots=%02X cpuMask=%08X route=[%u,%u,%u,%u]",
         g_netPlayerNames[0], g_netPlayerNames[1], metadata.vsRow,
         metadata.activeLaneMask, metadata.activeSlotMask, metadata.cpuMask,
         metadata.slotToSourceRoute[0], metadata.slotToSourceRoute[1],
         metadata.slotToSourceRoute[2], metadata.slotToSourceRoute[3]);
    if (metadata.activeLaneMask != 0x3u || metadata.activeSlotMask != 0x3u
        || metadata.cpuMask != 0u) {
        QLOG("[spectator] REJECT: this runtime plays back 1v1 all-human "
             "sessions only (lanes=%02X slots=%02X cpuMask=%08X)",
             metadata.activeLaneMask, metadata.activeSlotMask,
             metadata.cpuMask);
        return false;
    }
    return true;
}

void netBeginSpectatorArchive(const qgame::NetReadyState& hostReady) {
    if (g_spectatorServiceMode == 0) return;
    const bool sidecarMode = g_spectatorServiceMode == 2u;
    if (!g_netServeSpectators
        || (!sidecarMode && !qgame::spectatorSourceActive())
        || g_netSpectatorArchiveBegun || g_netSpectatorArchiveFailed)
        return;
    qnetplay::spectator::GameDescriptor descriptor{};
    descriptor.gameId = g_netSpectatorGameId;
    descriptor.simulationContract = qipc::kRuntimeCompatibilityId;
    if (!qgame::netAcceptedCompatibilityIdentity(
            descriptor.compatibilityIdentity)) {
        g_netSpectatorArchiveFailed = true;
        QLOG("[spectator-source] no accepted full compatibility identity; "
             "spectator service disabled for this session");
        return;
    }
    descriptor.firstTimelineFrame = 0;
    descriptor.playerOneDelay = static_cast<uint16_t>(
        g_netLocalCtrl == 0 ? g_netDelay : g_netRemoteDelay);
    descriptor.playerTwoDelay = static_cast<uint16_t>(
        g_netLocalCtrl == 1 ? g_netDelay : g_netRemoteDelay);
    descriptor.rollbackWindow = static_cast<uint16_t>(
        (std::max)(g_netRollback, g_netRemoteRollback));
    descriptor.metadata = netSpectatorMetadata(hostReady, netPhaseTag());
    if (descriptor.metadata.empty()) {
        g_netSpectatorArchiveFailed = true;
        QLOG("[spectator-source] deterministic metadata encoding failed; "
             "player session unaffected");
        return;
    }
    descriptor.initialStateHash = netSpectatorLiveStateHash();
    if (descriptor.initialStateHash == 0) {
        g_netSpectatorArchiveFailed = true;
        QLOG("[spectator-source] initial canonical state hash failed; "
             "player session unaffected");
        return;
    }
    if (sidecarMode) {
        qgame::spectator_publish::SidecarStartInfo info{};
        info.gameId = descriptor.gameId;
        info.simulationContract = descriptor.simulationContract;
        info.compatibilityIdentity = descriptor.compatibilityIdentity;
        info.initialStateHash = descriptor.initialStateHash;
        info.initialDigest = descriptor.initialDigest;
        info.firstTimelineFrame = descriptor.firstTimelineFrame;
        info.playerOneDelay = descriptor.playerOneDelay;
        info.playerTwoDelay = descriptor.playerTwoDelay;
        info.rollbackWindow = descriptor.rollbackWindow;
        if (qgame::spectator_publish::publishStart(
                info, descriptor.metadata.data(), descriptor.metadata.size())) {
            g_netSpectatorPostTailHash = descriptor.initialStateHash;
            QLOG("[spectator-sidecar] start published game=%llu phase=%08X "
                 "delay=[%u,%u] rollbackWindow=%u metadata=%zu",
                 static_cast<unsigned long long>(descriptor.gameId),
                 netPhaseTag(), descriptor.playerOneDelay,
                 descriptor.playerTwoDelay, descriptor.rollbackWindow,
                 descriptor.metadata.size());
        } else {
            g_netSpectatorArchiveFailed = true;
            QLOG("[spectator-sidecar] start publication refused; the player "
                 "session is unaffected");
        }
        return;
    }

    const qnetplay::spectator::ArchiveResult result =
        qgame::spectatorSourceBeginGame(descriptor);
    if (result == qnetplay::spectator::ArchiveResult::Ok
        || result == qnetplay::spectator::ArchiveResult::ExactDuplicate) {
        g_netSpectatorArchiveBegun = true;
        g_netSpectatorPostTailHash = descriptor.initialStateHash;
        QLOG("[spectator-source] archive begun game=%llu phase=%08X "
             "delay=[%u,%u] rollbackWindow=%u",
             static_cast<unsigned long long>(descriptor.gameId),
             netPhaseTag(), descriptor.playerOneDelay,
             descriptor.playerTwoDelay, descriptor.rollbackWindow);
    } else {
        g_netSpectatorArchiveFailed = true;
        QLOG("[spectator-source] begin failed result=%u; player session unaffected",
             static_cast<unsigned>(result));
    }
}

void netSealSpectatorArchive(qnetplay::spectator::TerminalReason reason) {
    if (g_spectatorServiceMode == 2u) {
        if (g_netSpectatorArchiveSealed) return;
        g_netSpectatorArchiveSealed = true;
        qgame::spectator_publish::publishSeal(
            static_cast<uint16_t>(reason), g_netSpectatorPostTailHash);
        return;
    }
    if (!g_netSpectatorArchiveBegun || g_netSpectatorArchiveSealed) return;
    const qnetplay::spectator::ArchiveResult result =
        qgame::spectatorSourceSealGame(
            g_netSpectatorGameId, reason,
            g_netSpectatorPostTailHash);
    if (result == qnetplay::spectator::ArchiveResult::Ok
        || result == qnetplay::spectator::ArchiveResult::ExactDuplicate) {
        g_netSpectatorArchiveSealed = true;
        QLOG("[spectator-source] SEALED records=%u reason=%u "
             "postTailHash=%016llX; handing the tail to the pump-thread drain",
             g_netSpectatorRecord, static_cast<unsigned>(reason),
             static_cast<unsigned long long>(g_netSpectatorPostTailHash));
        qgame::spectatorSourceBeginTailDrain(0);
    } else {
        QLOG("[spectator-source] seal failed result=%u; player session unaffected",
             static_cast<unsigned>(result));
    }
}
inline void captureFpuState(PaceSnap& p) {
    _controlfp_s(&p.fpControl, 0, 0);
    fxSaveTo(p.fxState); p.fxValid = true;
}
inline void restoreFpuState(const PaceSnap& p) {
    if (p.fxValid) { fxRestoreFrom(const_cast<uint8_t*>(p.fxState)); return; }
    unsigned int cur = 0;
    _controlfp_s(&cur, p.fpControl, _MCW_PC | _MCW_RC | _MCW_EM | _MCW_DN);
}
inline void capturePacingState(int slot) {
    PaceSnap& p = g_paceSlots[slot % kPaceSlots];
    p.d28 = rd32(0x4ABD28); p.dc4 = rd32(0x4ABDC4);
    p.d38 = rd32(0x4ABD38); p.b20 = rd32(0x4ABB20);
    size_t off = 0;
    for (const auto& s : kTimingSpans) {
        std::memcpy(p.timing + off, reinterpret_cast<const void*>(s.addr), s.size);
        off += s.size;
    }
    if (g_fpuStateShim) captureFpuState(p);
}
inline void restorePacingState(int slot) {
    const PaceSnap& p = g_paceSlots[slot % kPaceSlots];
    if (g_fpuStateShim) restoreFpuState(p);    // The FPU must be restored before anything else this tick.
    wr32(0x4ABD28, p.d28); wr32(0x4ABDC4, p.dc4);
    wr32(0x4ABD38, p.d38); wr32(0x4ABB20, p.b20);
    if (g_minimalDet) return;
    size_t off = 0;
    for (const auto& s : kTimingSpans) {
        std::memcpy(reinterpret_cast<void*>(s.addr), p.timing + off, s.size);
        off += s.size;
    }
}

static bool          g_clockHooked = false;
static volatile int  g_clockMode = 0;
static DWORD         g_clockThread = 0;
typedef DWORD (WINAPI* TimeGetTimeFn)(void);
typedef DWORD (WINAPI* GetTickCountFn)(void);
typedef BOOL  (WINAPI* QueryPerfCounterFn)(LARGE_INTEGER*);
static TimeGetTimeFn      g_origTGT = nullptr;
static GetTickCountFn     g_origGTC = nullptr;
static QueryPerfCounterFn g_origQPC = nullptr;
static volatile bool      g_netClockActive = false;
static volatile uint32_t  g_netClockFrame = 0;
static LONGLONG           g_netClockQpf = 1;
int64_t netUnhookedQpcNow() {
    LARGE_INTEGER sampled{};
    if (g_origQPC != nullptr)
        return g_origQPC(&sampled) ? sampled.QuadPart : 0;
    if (QueryPerformanceCounter(&sampled))
        return sampled.QuadPart;
    return 0;
}
inline DWORD netSyntheticMs() {
    return static_cast<DWORD>(
        (static_cast<uint64_t>(g_netClockFrame) * 10000ull) / 606ull);
}
inline void netClockBegin(uint32_t frame) {
    g_netClockFrame = frame;
    g_netClockActive = true;
}
inline void netClockOff() { g_netClockActive = false; }
template <typename T, int N> struct ClockRing {
    T v[N]; int n = 0, idx = 0;
    void beginRecord() { n = 0; }
    void beginReplay() { idx = 0; }
    void rec(T x) { if (n < N) v[n++] = x; }
    bool rep(T& o) { if (idx < n) { o = v[idx++]; return true; } if (n > 0) { o = v[n - 1]; return true; } return false; }
};
constexpr int kClockSlots = 32;
struct ClockFrameRec {
    ClockRing<DWORD, 384>    tgt, gtc;
    ClockRing<LONGLONG, 384> qpc;
};
static ClockFrameRec  g_clockSlots[kClockSlots];
static ClockFrameRec* g_clockActive = nullptr;
static uint64_t g_clockRecTotal = 0, g_clockRepTotal = 0, g_clockRepMiss = 0;
static volatile int g_inPacing = 0;
static volatile bool g_pacingFreeze = false;
inline bool onClockThread() { return GetCurrentThreadId() == g_clockThread; }
inline bool clockActive() { return onClockThread() && g_inPacing == 0; }
DWORD WINAPI hkTimeGetTime(void) {
    if ((g_netActive || g_replayRoleActive) && g_netClockActive
        && onClockThread()
        && g_inPacing == 0)
        return netSyntheticMs();
    if (clockActive() && g_clockActive) {
        if (g_clockMode == 1) { DWORD r = g_origTGT(); g_clockActive->tgt.rec(r); g_clockRecTotal++; return r; }
        if (g_clockMode == 2) { DWORD r; g_clockRepTotal++; if (g_clockActive->tgt.rep(r)) return r; g_clockRepMiss++; }
    }
    return g_origTGT ? g_origTGT() : ::timeGetTime();
}
DWORD WINAPI hkGetTickCount(void) {
    if ((g_netActive || g_replayRoleActive) && g_netClockActive
        && onClockThread()
        && g_inPacing == 0)
        return netSyntheticMs();
    if (clockActive() && g_clockActive) {
        if (g_clockMode == 1) { DWORD r = g_origGTC(); g_clockActive->gtc.rec(r); return r; }
        if (g_clockMode == 2) { DWORD r; if (g_clockActive->gtc.rep(r)) return r; }
    }
    return g_origGTC ? g_origGTC() : ::GetTickCount();
}
BOOL WINAPI hkQueryPerformanceCounter(LARGE_INTEGER* p) {
    if ((g_netActive || g_replayRoleActive) && g_netClockActive
        && onClockThread()
        && g_inPacing == 0 && p) {
        p->QuadPart = static_cast<LONGLONG>(
            (static_cast<uint64_t>(netSyntheticMs())
             * static_cast<uint64_t>(g_netClockQpf)) / 1000ull);
        return TRUE;
    }
    if (clockActive() && g_clockActive && p) {
        if (g_clockMode == 1) { BOOL b = g_origQPC(p); g_clockActive->qpc.rec(p->QuadPart); return b; }
        if (g_clockMode == 2) { LONGLONG q; if (g_clockActive->qpc.rep(q)) { p->QuadPart = q; return TRUE; } }
    }
    return g_origQPC ? g_origQPC(p) : ::QueryPerformanceCounter(p);
}

static bool netInstallQohClockImportPatches() {
    if (g_clockHooked) return true;
    LARGE_INTEGER syntheticFrequency{};
    if (QueryPerformanceFrequency(&syntheticFrequency)
        && syntheticFrequency.QuadPart > 0) {
        g_netClockQpf = syntheticFrequency.QuadPart;
    }
    void* originalTgt = nullptr;
    void* originalGtc = nullptr;
    void* originalQpc = nullptr;
    const bool tgt = patchQohImport(
        "winmm.dll", "timeGetTime",
        reinterpret_cast<void*>(&hkTimeGetTime), &originalTgt);
    const bool gtc = patchQohImport(
        "kernel32.dll", "GetTickCount",
        reinterpret_cast<void*>(&hkGetTickCount), &originalGtc);
    const bool qpc = patchQohImport(
        "kernel32.dll", "QueryPerformanceCounter",
        reinterpret_cast<void*>(&hkQueryPerformanceCounter), &originalQpc);
    if (tgt) g_origTGT = reinterpret_cast<TimeGetTimeFn>(originalTgt);
    if (gtc) g_origGTC = reinterpret_cast<GetTickCountFn>(originalGtc);
    if (qpc) g_origQPC = reinterpret_cast<QueryPerfCounterFn>(originalQpc);
    const int ok = (tgt ? 1 : 0) + (gtc ? 1 : 0) + (qpc ? 1 : 0);
    g_clockHooked = (ok == 3);
    QLOG("[clock_shim] QOH import-slot clock interception installed=%d/3 "
         "(winmm!timeGetTime=%d kernel32!GetTickCount=%d "
         "kernel32!QueryPerformanceCounter=%d) qpf=%lld -> %s",
         ok, tgt ? 1 : 0, gtc ? 1 : 0, qpc ? 1 : 0,
         static_cast<long long>(g_netClockQpf),
         g_clockHooked ? "ENABLED" : "FAIL-CLOSED");
    if (!g_clockHooked) {
        g_clockShimEnabled = false;
        QLOG("[clock_shim] refusing to detour system implementation code as a "
             "fallback; the caller must roll the launch back");
    }
    return g_clockHooked;
}
typedef BOOL  (__cdecl* Pacing4704E0Fn)();
typedef DWORD (__cdecl* Pacing4704A0Fn)();
typedef int   (__cdecl* Pacing470510Fn)();
static Pacing4704E0Fn g_origPace4704E0 = nullptr;
static Pacing4704A0Fn g_origPace4704A0 = nullptr;
static Pacing470510Fn g_origPace470510 = nullptr;
static bool           g_paceBracketHooked = false;
static volatile LONG  g_netGateOwnsTick = 0;
static volatile LONG  g_freezeGateCalls = 0;
BOOL __cdecl hkPace4704E0() {
    // Never answer a permanent 0: the game busy-waits on this gate inside the tick (while(!sub_4704E0())) and would hang.
    if ((g_netActive || g_replayRoleActive)
        && InterlockedCompareExchange(&g_netGateOwnsTick, 0, 0))
        return (InterlockedIncrement(&g_freezeGateCalls) == 1) ? 1 : 0;
    if (g_pacingFreeze) return InterlockedIncrement(&g_freezeGateCalls) == 1 ? 1 : 0;
    ++g_inPacing; BOOL r = g_origPace4704E0 ? g_origPace4704E0() : 1; --g_inPacing; return r;
}
DWORD __cdecl hkPace4704A0() {
    if ((g_netActive || g_replayRoleActive)
        && InterlockedCompareExchange(&g_netGateOwnsTick, 0, 0)) {
        const DWORD value = netSyntheticMs();
        *reinterpret_cast<volatile uint32_t*>(0x4AB8A0) = value;
        return value;
    }
    if (g_pacingFreeze) return *reinterpret_cast<volatile uint32_t*>(0x4AB8A0);
    ++g_inPacing; DWORD r = g_origPace4704A0 ? g_origPace4704A0() : 0; --g_inPacing; return r;
}
static bool g_freezeCatchup = false;
int __cdecl hkPace470510() {
    if (((g_netActive || g_replayRoleActive)
         && InterlockedCompareExchange(&g_netGateOwnsTick, 0, 0)) ||
        g_pacingFreeze || g_freezeCatchup) return 0;
    ++g_inPacing; int r = g_origPace470510 ? g_origPace470510() : 0; --g_inPacing; return r;
}
inline void clockBeginRecord(int slot) {
    g_clockActive = &g_clockSlots[slot % kClockSlots];
    g_clockActive->tgt.beginRecord(); g_clockActive->gtc.beginRecord(); g_clockActive->qpc.beginRecord();
    g_clockMode = 1;
}
inline void clockBeginReplay(int slot) {
    g_clockActive = &g_clockSlots[slot % kClockSlots];
    g_clockActive->tgt.beginReplay(); g_clockActive->gtc.beginReplay(); g_clockActive->qpc.beginReplay();
    g_clockMode = 2;
}
inline void clockOff() { g_clockMode = 0; g_clockActive = nullptr; }

// Every RNG draw funnels through this function; seeds 0x4A9938/0x4A993C are referenced nowhere else in .text.
typedef unsigned int (__cdecl* RngFn)();
static RngFn g_origRng   = nullptr;
static bool  g_rngHooked = false;
static DWORD g_frameThread = 0;

enum class CosmeticRngMode { Off, Trace, Whitelist };
static CosmeticRngMode g_cosRngMode = CosmeticRngMode::Off;

constexpr int kMaxRngSites = 128;
struct RngSite {
    uintptr_t ra = 0;
    uint32_t  truthN = 0, replayN = 0;
    uint64_t  totalN = 0;
    bool      whitelisted = false;
};
static RngSite g_rngSites[kMaxRngSites];
static int     g_rngSiteN = 0;

struct RngCounts { uint32_t game = 0, cos = 0, redirected = 0; };
static RngCounts g_rngTruthCnt, g_rngReplayCnt;
static volatile int g_rngPhase = 0;

static uint32_t g_privSeed = 0x1234567u;

constexpr int kRngRingCap   = 384;
constexpr int kRngRingSlots = 32;
struct RedirRing { uint32_t v[kRngRingCap]; int n = 0, idx = 0, miss = 0; };
static RedirRing  g_redirRings[kRngRingSlots];
static RedirRing* g_redirActive = nullptr;

static RngSite* rngSiteFor(uintptr_t ra) {
    for (int i = 0; i < g_rngSiteN; ++i)
        if (g_rngSites[i].ra == ra) return &g_rngSites[i];
    if (g_rngSiteN < kMaxRngSites) {
        RngSite* s = &g_rngSites[g_rngSiteN++];
        s->ra = ra;
        return s;
    }
    return nullptr;
}
inline void rngResetFrameCounts() {
    for (int i = 0; i < g_rngSiteN; ++i) { g_rngSites[i].truthN = 0; g_rngSites[i].replayN = 0; }
    g_rngTruthCnt = RngCounts{}; g_rngReplayCnt = RngCounts{};
}
static volatile bool g_rngCountThis = true;
inline void rngBeginTruth(int slot) {
    g_redirActive = &g_redirRings[slot % kRngRingSlots];
    g_redirActive->n = 0; g_redirActive->idx = 0; g_redirActive->miss = 0;
    g_rngCountThis = true;
    g_rngPhase = 1;
}
inline void rngBeginReplay(int slot, bool countIt) {
    g_redirActive = &g_redirRings[slot % kRngRingSlots];
    g_redirActive->idx = 0;
    g_rngCountThis = countIt;
    g_rngPhase = 2;
}
inline void rngPhaseOff() { g_rngPhase = 0; g_redirActive = nullptr; }

static volatile LONG g_rngOtherThreadDraws = 0;
static uintptr_t     g_rngOtherThreadSites[8];
static volatile LONG g_rngOtherThreadSiteN = 0;
static bool trainingCriticalRollOverride(uint32_t nativeSample,
                                         uint32_t& outSample);
static bool trainingCriticalContextArmed();
unsigned int __cdecl hkRngDraw();

/* Two MinHook detours on one native target cannot coexist: share this hook. */
bool requestTrainingCriticalRngHook() {
    if (g_rngHooked) return true;
    static const uint8_t kRngSig[] = {
        0xA1, 0x24, 0xBB, 0x4A, 0x00, 0x85, 0xC0, 0x75, 0x1A };
    if (!g_rngDrawFn
        || !se::verifyCodeBytes(g_rngDrawFn, kRngSig, sizeof(kRngSig))) {
        QLOG("[training] RNG prologue mismatch at profile %s 0x%08X - "
             "critical policy unavailable",
             runtime_profile::selectedProfileKey(),
             static_cast<unsigned>(g_rngDrawFn));
        return false;
    }
    void* target = reinterpret_cast<void*>(g_rngDrawFn);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hkRngDraw),
                      reinterpret_cast<void**>(&g_origRng)) != MH_OK
        || MH_QueueEnableHook(target) != MH_OK) {
        QLOG("[training] could not install the shared RNG hook at 0x%08X - "
             "critical policy unavailable",
             static_cast<unsigned>(g_rngDrawFn));
        return false;
    }
    g_rngHooked = true;
    QLOG("[training] shared RNG hook installed for critical policy at "
         "profile %s 0x%08X",
         runtime_profile::selectedProfileKey(),
         static_cast<unsigned>(g_rngDrawFn));
    return true;
}

unsigned int __cdecl hkRngDraw() {
    if (trainingCriticalContextArmed()) {
        const unsigned int nativeSample = g_origRng();
        uint32_t overridden = 0u;
        if (trainingCriticalRollOverride(
                static_cast<uint32_t>(nativeSample), overridden)) {
            return overridden;
        }
        return nativeSample;
    }
    if (g_cosRngMode == CosmeticRngMode::Off)
        return g_origRng();
    if (GetCurrentThreadId() != g_frameThread) {
        uintptr_t ra = reinterpret_cast<uintptr_t>(_ReturnAddress());
        InterlockedIncrement(&g_rngOtherThreadDraws);
        LONG n = g_rngOtherThreadSiteN;
        bool seen = false;
        for (LONG i = 0; i < n && i < 8; ++i) if (g_rngOtherThreadSites[i] == ra) { seen = true; break; }
        if (!seen && n < 8) { g_rngOtherThreadSites[n] = ra; InterlockedIncrement(&g_rngOtherThreadSiteN); }
        return g_origRng();
    }
    uintptr_t ra  = reinterpret_cast<uintptr_t>(_ReturnAddress());
    RngSite*  s   = rngSiteFor(ra);
    uint32_t  sel = rd32(0x4ABB24);
    int       ph  = g_rngPhase;
    bool      cnt = (ph == 1) || (ph == 2 && g_rngCountThis);
    if (s) { if (ph == 1) s->truthN++; else if (ph == 2 && cnt) s->replayN++; s->totalN++; }
    bool redirect = (g_cosRngMode == CosmeticRngMode::Whitelist) && s && s->whitelisted && sel == 0;
    if (redirect) {
        uint32_t v;
        if (ph == 2 && g_redirActive && g_redirActive->idx < g_redirActive->n) {
            v = g_redirActive->v[g_redirActive->idx++];
        } else {
            g_privSeed = 156683941u * g_privSeed + 1u;
            v = (g_privSeed >> 15) & 0x7FFF;
            if (ph == 2 && g_redirActive) g_redirActive->miss++;
            if (ph == 1 && g_redirActive && g_redirActive->n < kRngRingCap)
                g_redirActive->v[g_redirActive->n++] = v;
        }
        if (cnt) { if (ph == 1) g_rngTruthCnt.redirected++; else g_rngReplayCnt.redirected++; }
        return v;
    }
    if (cnt) {
        if (ph == 1) { if (sel == 0) g_rngTruthCnt.game++;  else g_rngTruthCnt.cos++; }
        if (ph == 2) { if (sel == 0) g_rngReplayCnt.game++; else g_rngReplayCnt.cos++; }
    }
    return g_origRng();
}

static bool g_histTrace = false;
static bool g_p56Trace = false;
typedef int (__fastcall* HistFn)(int, int);
static HistFn g_origHist = nullptr;
static bool g_histHooked = false;
static uint64_t g_histLogged = 0;
static volatile uint32_t g_histCallP2 = 0;
inline uint32_t histHash(int fighter) {
    uint32_t h = 2166136261u;
    for (int i = 216; i < 216 + 36 * 30; i += 4) h = (h ^ rd32(fighter + i)) * 16777619u;
    return h;
}
int __fastcall hkHistUpdate(int a1, int a2) {
    if (a1 == 0x626880) g_histCallP2++;
    if (!g_histTrace || !g_origHist) return g_origHist ? g_origHist(a1, a2) : 0;
    uint32_t b208 = rd32(a1 + 208), bE8 = rd32(a1 + 232), bAct = rd32(a1 + 56);
    uint32_t hBefore = histHash(a1);
    int r = g_origHist(a1, a2);
    uint32_t a208 = rd32(a1 + 208), aE8 = rd32(a1 + 232);
    bool reset = (b208 & 0x10000) != 0 && (a208 & 0x10000) == 0;
    static uint64_t s_ptrLog = 0;
    if (s_ptrLog < 24) { QLOG("[hist_ptr] a1=%08X a2=%08X reset=%d E8=%08X phase=%s",
                              (unsigned)a1, (unsigned)a2, (int)reset, aE8,
                              g_replayActive ? "replay" : "truth"); s_ptrLog++; }
    if ((a1 == 0x623E00 || a1 == 0x626880) && (reset || aE8 != bE8) && g_histLogged < 6000) {
        QLOG("[hist_trace] phase=%s frame=%llu K=%d fighter=%08X reset=%d act=%u "
             "in48=%d in52=%d in64=%d in72=%d in84=%d in88=%d 208:%08X->%08X "
             "E8:%08X->%08X hist:%08X->%08X",
             g_replayActive ? "replay" : "truth", (unsigned long long)g_ovrFrame,
             g_window, (unsigned)a1, (int)reset, bAct,
             (int)rd32(a1 + 48), (int)rd32(a1 + 52), (int)rd32(a1 + 64),
             (int)rd32(a1 + 72), (int)rd32(a1 + 84), (int)rd32(a1 + 88),
             b208, a208, bE8, aE8, hBefore, histHash(a1));
        g_histLogged++;
    }
    return r;
}

static bool g_b600Trace = false;
typedef int (__fastcall* B600Fn)(int, int, int);
static B600Fn g_origB600 = nullptr;
static bool g_b600Hooked = false;
struct B600Rec { uint64_t frame; uint8_t replay; int32_t a2; uint32_t before, after; };
constexpr int kB600Ring = 512;
static B600Rec g_b600Ring[kB600Ring] = {};
static volatile uint32_t g_b600Idx = 0;
int __fastcall hkB600(int thisp, int edx, int a2) {
    if (!g_b600Trace || !g_origB600 || (uint32_t)thisp != 0x626880)
        return g_origB600 ? g_origB600(thisp, edx, a2) : 0;
    uint32_t before = rd32(thisp + 56);
    int r = g_origB600(thisp, edx, a2);
    B600Rec& e = g_b600Ring[g_b600Idx++ % kB600Ring];
    e.frame = g_ovrFrame; e.replay = g_replayActive ? 1 : 0; e.a2 = a2;
    e.before = before; e.after = rd32(thisp + 56);
    return r;
}

static bool g_a0Trace = false;
static bool pageReadable(const void* p);
typedef int (__fastcall* A0Fn)(int, int, int);
static A0Fn g_origA0 = nullptr;
static bool g_a0Hooked = false;
struct A0Rec { uint64_t frame; uint8_t replay; uint32_t f52in, f48, f64out, f56out, f52out;
               uint32_t f16, f24, base, durAddr, durVal; };
constexpr int kA0Ring = 512;
static A0Rec g_a0Ring[kA0Ring] = {};
static volatile uint32_t g_a0Idx = 0;
int __fastcall hkA0(int a1, int edx, int a3) {
    if (!g_a0Trace || !g_origA0 || (uint32_t)a1 != 0x626880)
        return g_origA0 ? g_origA0(a1, edx, a3) : 0;
    uint32_t f48 = rd32(a1 + 48);
    int r = g_origA0(a1, edx, a3);
    A0Rec& e = g_a0Ring[g_a0Idx++ % kA0Ring];
    e.frame = g_ovrFrame; e.replay = g_replayActive ? 1 : 0;
    e.f48 = f48; e.f52in = 0; e.f52out = 0; e.f24 = 0; e.durAddr = 0; e.durVal = 0;
    e.f64out = rd32(a1 + 64); e.f56out = rd32(a1 + 56);
    uint32_t f16 = rd32(a1 + 16);
    e.f16 = f16;
    e.base = (f16 >= 0x10000 && pageReadable((void*)(uintptr_t)(f16 + 24 * f48)))
                 ? rd32(f16 + 24 * f48) : 0;
    return r;
}

static bool inputOverrideOptIn() {
    char buf[8] = {0};
    DWORD n = GetEnvironmentVariableA("QOH99_NP_INPUT_OVERRIDE", buf, sizeof(buf));
    return (n == 1 && buf[0] == '1') || g_inputTest || g_netActive
        || g_replayRoleActive || offlineManagedWanted()
        || g_unattendedHarnessLaunch;
}

void configureAudioV2FromEnv() {
    static bool done = false; if (done) return; done = true;
    char buf[64] = {0};
    if (GetEnvironmentVariableA("QOH99_NP_AUDIO_V2", buf, sizeof(buf)) && buf[0] == '0') {
        g_audioV2Enabled = false;
        QLOG("[audio-v2] DISABLED by env (legacy inline replay-audio policy)");
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_AUDIO_MUTE", buf, sizeof(buf)) && buf[0] == '1') {
        g_audioV2Muted = true;
        QLOG("[audio-v2] physical sink MUTED by env (intent/journals unaffected)");
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_AUDIO_HIDDEN_HORIZON", buf, sizeof(buf)) && buf[0]) {
        g_audioV2Horizons.hiddenForwardAudioHorizonFrames =
            qaudio::clampHiddenForwardHorizon(
                static_cast<uint32_t>(std::atoi(buf)));
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_AUDIO_SPECTATOR_HORIZON", buf, sizeof(buf)) && buf[0]) {
        g_audioV2Horizons.spectatorAudioHorizonFrames =
            qaudio::clampSpectatorHorizon(
                static_cast<uint32_t>(std::atoi(buf)));
    }
}

void installGameplayHooksOnce() {
    configureAudioV2FromEnv();
    if ((g_netActive || g_replayRoleActive || g_offlineManagedLaunch
         || g_trainingLaunchRequested)
        && !g_configSaveHooked) {
        void* target =
            reinterpret_cast<void*>(g_configSaveFn);
        if (MH_CreateHook(
                target, reinterpret_cast<void*>(&hkConfigSave),
                reinterpret_cast<void**>(&g_origConfigSave)) == MH_OK
            && MH_QueueEnableHook(target) == MH_OK) {
            g_configSaveHooked = true;
            QLOG("[config] preference-preserving save hook enabled "
                 "at sub_485390");
        } else {
            QLOG("[config] FAILED to install mandatory QOHcnf save hook");
        }
    }
    if (!g_readHooked) {
        if (!inputOverrideOptIn()) {
            g_ovrEnabled = false;
            QLOG("[input_override] method=sub_484BB0_hook DISABLED by default "
                 "(no role opted in; the historical frame-1 AV was a "
                 "mid-execution enable, not hook creation; set "
                 "QOH99_NP_INPUT_OVERRIDE=1 to force). "
                 "Self-test runs idle (deterministic) - see docs/runtime/input_injection.md");
        } else {
            static const uint8_t kReadSig[] = { 0xA1, 0x60, 0x68, 0x62, 0x00, 0x56 };
            void* t = reinterpret_cast<void*>(g_readLogicalInputFn);
            if (!g_readLogicalInputFn
                || !se::verifyCodeBytes(g_readLogicalInputFn, kReadSig,
                                        sizeof(kReadSig))) {
                QLOG("[input_override] PROLOGUE MISMATCH at profile %s 0x%08X - refusing to hook",
                     runtime_profile::selectedProfileKey(),
                     (unsigned)g_readLogicalInputFn);
                g_ovrEnabled = false;
            } else if (MH_CreateHook(t, reinterpret_cast<void*>(&hkInputRead),
                              reinterpret_cast<void**>(&g_origRead)) == MH_OK &&
                MH_QueueEnableHook(t) == MH_OK) {
                g_readHooked = true;
                QLOG("[input_override] method=profile-logical-read enabled at %s 0x%08X",
                     runtime_profile::selectedProfileKey(),
                     (unsigned)g_readLogicalInputFn);
            } else {
                QLOG("[input_override] FAILED to hook profile %s 0x%08X - override disabled",
                     runtime_profile::selectedProfileKey(),
                     (unsigned)g_readLogicalInputFn);
                g_ovrEnabled = false;
            }
        }
    }
    if (!g_paletteHooked) {
        static const uint8_t kPaletteSig[] = {
            0x8B, 0x44, 0x24, 0x04, 0x50, 0x33, 0xC0, 0x66,
            0x8B, 0x04, 0xCD, 0xC4, 0xF2, 0x66, 0x00
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45B590);
        if (!se::verifyCodeBytes(0x45B590, kPaletteSig,
                                 sizeof(kPaletteSig))) {
            QLOG("[presentation] PROLOGUE MISMATCH at palette sub_45B590");
        } else if (
            MH_CreateHook(t, reinterpret_cast<void*>(&hkPaletteApply),
                          reinterpret_cast<void**>(&g_origPaletteApply))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_paletteHooked = true;
            QLOG("[presentation] palette/FPU hook on sub_45B590 enabled");
        } else {
            QLOG("[presentation] FAILED to hook palette sub_45B590");
        }
    }
    if (!g_gameErrorWriteHooked) {
        static const uint8_t kGameErrorSig[] = {
            0x81, 0xEC, 0x04, 0x01, 0x00, 0x00, 0x53, 0x56,
            0x8B, 0x35, 0xC4, 0xA4, 0x4A, 0x00, 0x57, 0x85
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45C5C0);
        if (!se::verifyCodeBytes(0x45C5C0, kGameErrorSig,
                                 sizeof(kGameErrorSig))) {
            QLOG("[game-error] PROLOGUE MISMATCH at QOHerror writer "
                 "sub_45C5C0 - the game's failure reasons stay in "
                 "SYSTEM\\QOHerror.txt only");
        } else if (
            MH_CreateHook(t, reinterpret_cast<void*>(&hkGameErrorWrite),
                          reinterpret_cast<void**>(&g_origGameErrorWrite))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_gameErrorWriteHooked = true;
            QLOG("[game-error] QOHerror.txt writer hook on sub_45C5C0 enabled "
                 "(the game's own failure reasons are mirrored here)");
        } else {
            QLOG("[game-error] FAILED to hook sub_45C5C0");
        }
    }
    if (!g_soundLoadHooked) {
        static const uint8_t kSoundLoadSig[] = {
            0x81, 0xEC, 0xC4, 0x00, 0x00, 0x00, 0xA1, 0xB4,
            0xAA, 0x4A, 0x00, 0x53, 0x55, 0x33, 0xED, 0x56
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45F760);
        if (!se::verifyCodeBytes(
                0x45F760, kSoundLoadSig, sizeof(kSoundLoadSig))) {
            QLOG("[presentation] PROLOGUE MISMATCH at sound-load sub_45F760");
        } else if (
            MH_CreateHook(t, reinterpret_cast<void*>(&hkSoundLoad),
                          reinterpret_cast<void**>(&g_origSoundLoad))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_soundLoadHooked = true;
            QLOG("[presentation] canonical WAV-duration hook on sub_45F760 enabled");
        } else {
            QLOG("[presentation] FAILED to hook sound-load sub_45F760");
        }
    }
    if (!g_soundDuplicatePlayHooked) {
        static const uint8_t kSoundDuplicatePlaySig[] = {
            0x83, 0xEC, 0x08, 0xA1, 0xB4, 0xAA, 0x4A, 0x00,
            0x53, 0x55, 0x56, 0x85, 0xC0, 0x57, 0x89, 0x54
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45F9C0);
        if (!se::verifyCodeBytes(
                0x45F9C0, kSoundDuplicatePlaySig,
                sizeof(kSoundDuplicatePlaySig))) {
            QLOG("[presentation] PROLOGUE MISMATCH at sound sub_45F9C0");
        } else if (
            MH_CreateHook(
                t, reinterpret_cast<void*>(&hkSoundDuplicatePlay),
                reinterpret_cast<void**>(&g_origSoundDuplicatePlay))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_soundDuplicatePlayHooked = true;
            QLOG("[presentation] replay-safe sound hook on sub_45F9C0 enabled");
        } else {
            QLOG("[presentation] FAILED to hook sound sub_45F9C0");
        }
    }
    if (!g_soundDirectPlayHooked) {
        static const uint8_t kSoundDirectPlaySig[] = {
            0x56, 0x8B, 0xF1, 0x57, 0x8B, 0xFA, 0x8B, 0x04,
            0xB5, 0x14, 0xAD, 0x4A, 0x00, 0x85, 0xC0, 0x75
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45FB20);
        if (!se::verifyCodeBytes(
                0x45FB20, kSoundDirectPlaySig,
                sizeof(kSoundDirectPlaySig))) {
            QLOG("[presentation] PROLOGUE MISMATCH at sound sub_45FB20");
        } else if (
            MH_CreateHook(
                t, reinterpret_cast<void*>(&hkSoundDirectPlay),
                reinterpret_cast<void**>(&g_origSoundDirectPlay))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_soundDirectPlayHooked = true;
            QLOG("[presentation] replay-safe sound hook on sub_45FB20 enabled");
        } else {
            QLOG("[presentation] FAILED to hook sound sub_45FB20");
        }
    }
    if (!g_soundStopHooked) {
        static const uint8_t kSoundStopSig[] = {
            0x8B, 0xC2, 0x8B, 0x15, 0xB4, 0xAA, 0x4A, 0x00,
            0x85, 0xD2, 0x75, 0x03, 0x33, 0xC0, 0xC3
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45FB60);
        if (!se::verifyCodeBytes(
                0x45FB60, kSoundStopSig, sizeof(kSoundStopSig))) {
            QLOG("[presentation] PROLOGUE MISMATCH at sound sub_45FB60");
        } else if (
            MH_CreateHook(t, reinterpret_cast<void*>(&hkSoundStop),
                          reinterpret_cast<void**>(&g_origSoundStop))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_soundStopHooked = true;
            QLOG("[presentation] replay-safe sound-stop hook on sub_45FB60 enabled");
        } else {
            QLOG("[presentation] FAILED to hook sound-stop sub_45FB60");
        }
    }
    if (!g_soundStatusHooked) {
        static const uint8_t kSoundStatusSig[] = {
            0x51, 0x8B, 0xC2, 0x8B, 0x15, 0xB4, 0xAA, 0x4A,
            0x00, 0x85, 0xD2, 0x75, 0x04, 0x33, 0xC0
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45FBB0);
        if (!se::verifyCodeBytes(
                0x45FBB0, kSoundStatusSig, sizeof(kSoundStatusSig))) {
            QLOG("[presentation] PROLOGUE MISMATCH at sound sub_45FBB0");
        } else if (
            MH_CreateHook(t, reinterpret_cast<void*>(&hkSoundStatus),
                          reinterpret_cast<void**>(&g_origSoundStatus))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_soundStatusHooked = true;
            QLOG("[presentation] deterministic sound-status hook on sub_45FBB0 enabled");
        } else {
            QLOG("[presentation] FAILED to hook sound-status sub_45FBB0");
        }
    }
    if (g_audioV2Enabled && !g_soundUnloadHooked) {
        static const uint8_t kSoundUnloadSig[] = {
            0xA1, 0xB4, 0xAA, 0x4A, 0x00, 0x53, 0x85, 0xC0,
            0x8B, 0xD9, 0x75, 0x04, 0x33, 0xC0, 0x5B, 0xC3
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45F6D0);
        if (!se::verifyCodeBytes(
                0x45F6D0, kSoundUnloadSig, sizeof(kSoundUnloadSig))) {
            QLOG("[audio-v2] PROLOGUE MISMATCH at sound-unload sub_45F6D0");
        } else if (
            MH_CreateHook(t, reinterpret_cast<void*>(&hkSoundUnload),
                          reinterpret_cast<void**>(&g_origSoundUnload))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_soundUnloadHooked = true;
            QLOG("[audio-v2] journal sound-unload hook on sub_45F6D0 enabled");
        } else {
            QLOG("[audio-v2] FAILED to hook sound-unload sub_45F6D0");
        }
    }
    if (g_audioV2Enabled && !g_bgmCommandHooked) {
        static const uint8_t kBgmCommandSig[] = {
            0x81, 0xEC, 0x0C, 0x02, 0x00, 0x00, 0xA1, 0xE0,
            0x3D, 0x61, 0x00, 0x53, 0x55, 0x56, 0x8B, 0xF1
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x46FBC0);
        if (!se::verifyCodeBytes(
                0x46FBC0, kBgmCommandSig, sizeof(kBgmCommandSig))) {
            QLOG("[audio-v2] PROLOGUE MISMATCH at bgm-command sub_46FBC0");
        } else if (
            MH_CreateHook(t, reinterpret_cast<void*>(&hkBgmCommand),
                          reinterpret_cast<void**>(&g_origBgmCommand))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_bgmCommandHooked = true;
            QLOG("[audio-v2] BGM boundary-guard hook on sub_46FBC0 enabled");
        } else {
            QLOG("[audio-v2] FAILED to hook bgm-command sub_46FBC0");
        }
    }
    if (g_audioV2Enabled && !g_audioTeardownHooked) {
        static const uint8_t kAudioTeardownSig[] = {
            0x56, 0x57, 0xE8, 0x69, 0x0E, 0x00, 0x00, 0x33,
            0xFF, 0x33, 0xF6, 0x83, 0xBE, 0x00, 0xAB, 0x4A
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45F360);
        if (!se::verifyCodeBytes(
                0x45F360, kAudioTeardownSig, sizeof(kAudioTeardownSig))) {
            QLOG("[audio-v2] PROLOGUE MISMATCH at audio-teardown sub_45F360");
        } else if (
            MH_CreateHook(t, reinterpret_cast<void*>(&hkAudioTeardown),
                          reinterpret_cast<void**>(&g_origAudioTeardown))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_audioTeardownHooked = true;
            QLOG("[audio-v2] device-generation hook on sub_45F360 enabled");
        } else {
            QLOG("[audio-v2] FAILED to hook audio-teardown sub_45F360");
        }
    }
    if (g_audioV2Enabled && !g_audioInitHooked) {
        static const uint8_t kAudioInitSig[] = {
            0x56, 0x8B, 0xF1, 0x85, 0xF6, 0x75, 0x08, 0xFF,
            0x15, 0xCC, 0x91, 0x49, 0x00, 0x8B, 0xF0, 0x57
        };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45F3E0);
        if (!se::verifyCodeBytes(
                0x45F3E0, kAudioInitSig, sizeof(kAudioInitSig))) {
            QLOG("[audio-v2] PROLOGUE MISMATCH at audio-init sub_45F3E0");
        } else if (
            MH_CreateHook(t, reinterpret_cast<void*>(&hkAudioInit),
                          reinterpret_cast<void**>(&g_origAudioInit))
                == MH_OK
            && MH_QueueEnableHook(t) == MH_OK) {
            g_audioInitHooked = true;
            QLOG("[audio-v2] device-observation hook on sub_45F3E0 enabled");
        } else {
            QLOG("[audio-v2] FAILED to hook audio-init sub_45F3E0");
        }
    }
    if (g_audioV2Enabled && !g_audioOpsConfigured
        && g_soundLoadHooked && g_soundDuplicatePlayHooked
        && g_soundDirectPlayHooked && g_soundStopHooked
        && g_soundUnloadHooked && g_bgmCommandHooked) {
        qaudio::NativeAudioOps ops{};
        ops.load = &qohAudioOpLoad;
        ops.unload = &qohAudioOpUnload;
        ops.playDuplicate = &qohAudioOpPlayDuplicate;
        ops.playBase = &qohAudioOpPlayBase;
        ops.stop = &qohAudioOpStop;
        ops.bgm = &qohAudioOpBgm;
        ops.setCursor = &qohAudioOpSetCursor;
        qaudio::runtime().configure(ops);
        qaudio::runtime().setMuted(g_audioV2Muted);
        qaudio::runtime().setHorizons(g_audioV2Horizons);
        g_audioOpsConfigured = true;
        QLOG("[audio-v2] runtime configured (muted=%d hiddenHorizon=%u "
             "spectatorHorizon=%u)",
             g_audioV2Muted ? 1 : 0,
             g_audioV2Horizons.hiddenForwardAudioHorizonFrames,
             g_audioV2Horizons.spectatorAudioHorizonFrames);
    }
    if (g_histTrace && !g_histHooked) {
        static const uint8_t kHistSig[] = { 0x51, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xFA, 0x8B, 0xD9 };
        void* t = reinterpret_cast<void*>((uintptr_t)0x41A540);
        if (!se::verifyCodeBytes(0x41A540, kHistSig, sizeof(kHistSig))) {
            QLOG("[hist_trace] PROLOGUE MISMATCH at 0x41A540 - refusing to hook");
        } else if (MH_CreateHook(t, reinterpret_cast<void*>(&hkHistUpdate),
                          reinterpret_cast<void**>(&g_origHist)) == MH_OK &&
                   MH_QueueEnableHook(t) == MH_OK) {
            g_histHooked = true;
            QLOG("[hist_trace] history-update trace hook on sub_41A540 enabled");
        } else {
            QLOG("[hist_trace] FAILED to hook sub_41A540");
        }
    }
    if (g_b600Trace && !g_b600Hooked) {
        static const uint8_t kB600Sig[] = { 0x51, 0xA1, 0xB4, 0x37, 0x4A, 0x00, 0x53, 0x55, 0x56, 0x85, 0xC0 };
        void* t = reinterpret_cast<void*>((uintptr_t)0x41B600);
        if (!se::verifyCodeBytes(0x41B600, kB600Sig, sizeof(kB600Sig))) {
            QLOG("[b600_trace] PROLOGUE MISMATCH at 0x41B600 - refusing to hook");
        } else if (MH_CreateHook(t, reinterpret_cast<void*>(&hkB600),
                          reinterpret_cast<void**>(&g_origB600)) == MH_OK &&
                   MH_QueueEnableHook(t) == MH_OK) {
            g_b600Hooked = true;
            QLOG("[b600_trace] sub_41B600 P2 callsite trace enabled (record-only ring)");
        } else {
            QLOG("[b600_trace] FAILED to hook sub_41B600");
        }
    }
    if (g_a0Trace && !g_a0Hooked) {
        static const uint8_t kA0Sig[] = { 0x83, 0xEC, 0x28, 0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0xEA };
        void* t = reinterpret_cast<void*>((uintptr_t)0x411AE0);
        if (!se::verifyCodeBytes(0x411AE0, kA0Sig, sizeof(kA0Sig))) {
            QLOG("[a0_trace] PROLOGUE MISMATCH at 0x411AE0 - refusing to hook");
        } else if (MH_CreateHook(t, reinterpret_cast<void*>(&hkA0),
                          reinterpret_cast<void**>(&g_origA0)) == MH_OK &&
                   MH_QueueEnableHook(t) == MH_OK) {
            g_a0Hooked = true;
            QLOG("[a0_trace] sub_411AE0 P2 anim-advance trace enabled (record-only ring)");
        } else {
            QLOG("[a0_trace] FAILED to hook sub_411AE0");
        }
    }
    if (!g_pollHooked) {
        static const uint8_t kPollSig[] = { 0xA1, 0x20, 0xAA, 0x4A, 0x00, 0x85, 0xC0 };
        void* t = reinterpret_cast<void*>((uintptr_t)0x45D1A0);
        if (!se::verifyCodeBytes(0x45D1A0, kPollSig, sizeof(kPollSig))) {
            QLOG("[keystate] PROLOGUE MISMATCH at 0x45D1A0 - not hooking the poll");
        } else if (MH_CreateHook(t, reinterpret_cast<void*>(&hkPoll),
                                 reinterpret_cast<void**>(&g_origPoll)) == MH_OK &&
                   MH_QueueEnableHook(t) == MH_OK) {
            g_pollHooked = true;
            QLOG("[keystate] KeyState record/replay hook on sub_45D1A0 enabled "
                 "(deterministic keyboard across truth/replay ticks)");
        } else {
            QLOG("[keystate] FAILED to hook sub_45D1A0 - real-match input may be non-deterministic");
        }
    }
    if (((g_replayMode == ReplayTickMode::Deterministic
          && g_clockShimEnabled) || g_netActive) && !g_paceBracketHooked) {
        int pb = 0;
        void* p4E0 = reinterpret_cast<void*>((uintptr_t)0x4704E0);
        void* p4A0 = reinterpret_cast<void*>((uintptr_t)0x4704A0);
        void* p510 = reinterpret_cast<void*>((uintptr_t)0x470510);
        if (MH_CreateHook(p4E0, reinterpret_cast<void*>(&hkPace4704E0), reinterpret_cast<void**>(&g_origPace4704E0)) == MH_OK && MH_QueueEnableHook(p4E0) == MH_OK) pb++;
        if (MH_CreateHook(p4A0, reinterpret_cast<void*>(&hkPace4704A0), reinterpret_cast<void**>(&g_origPace4704A0)) == MH_OK && MH_QueueEnableHook(p4A0) == MH_OK) pb++;
        if (MH_CreateHook(p510, reinterpret_cast<void*>(&hkPace470510), reinterpret_cast<void**>(&g_origPace470510)) == MH_OK && MH_QueueEnableHook(p510) == MH_OK) pb++;
        g_paceBracketHooked = (pb == 3);
        QLOG("[clock_shim] pacing brackets queued=%d/3 "
             "(clock interception is an IAT patch, applied separately) "
             "thread=pending-first-dispatch -> %s",
             pb, g_paceBracketHooked ? "ENABLED" : "PARTIAL/DISABLED");
        if (!g_paceBracketHooked)
            g_clockShimEnabled = false;
    }
    if ((g_netActive || g_replayRoleActive) && !g_paceBracketHooked) {
        int pb = 0;
        void* p4E0 = reinterpret_cast<void*>((uintptr_t)0x4704E0);
        void* p4A0 = reinterpret_cast<void*>((uintptr_t)0x4704A0);
        void* p510 = reinterpret_cast<void*>((uintptr_t)0x470510);
        static const uint8_t kPace4E0Sig[] =
            { 0xA1, 0x9C, 0x74, 0x4A, 0x00, 0x85, 0xC0, 0x74 };
        static const uint8_t kPace4A0Sig[] =
            { 0xA1, 0x9C, 0x74, 0x4A, 0x00, 0x85, 0xC0, 0x74 };
        static const uint8_t kPace510Sig[] =
            { 0xA1, 0x9C, 0x74, 0x4A, 0x00, 0x8B, 0x0D, 0xA0 };
        const bool signaturesOk =
            se::verifyCodeBytes(0x4704E0, kPace4E0Sig, sizeof(kPace4E0Sig)) &&
            se::verifyCodeBytes(0x4704A0, kPace4A0Sig, sizeof(kPace4A0Sig)) &&
            se::verifyCodeBytes(0x470510, kPace510Sig, sizeof(kPace510Sig));
        if (!signaturesOk) {
            QLOG("[netplay] pacing-gate signature mismatch - refusing mandatory hooks");
        } else {
            if (MH_CreateHook(p4E0, reinterpret_cast<void*>(&hkPace4704E0), reinterpret_cast<void**>(&g_origPace4704E0)) == MH_OK && MH_QueueEnableHook(p4E0) == MH_OK) pb++;
            if (MH_CreateHook(p4A0, reinterpret_cast<void*>(&hkPace4704A0), reinterpret_cast<void**>(&g_origPace4704A0)) == MH_OK && MH_QueueEnableHook(p4A0) == MH_OK) pb++;
            if (MH_CreateHook(p510, reinterpret_cast<void*>(&hkPace470510), reinterpret_cast<void**>(&g_origPace470510)) == MH_OK && MH_QueueEnableHook(p510) == MH_OK) pb++;
        }
        g_paceBracketHooked = (pb == 3);
        QLOG("[netplay] pacing-gate brackets installed=%d/3 -> frame gate %s",
             pb, g_paceBracketHooked ? "ACTIVE" : "FAILED (sim would free-run!)");
    }
    if (g_pacingReplayEnabled && !g_pacingHooked) {
        void* t = reinterpret_cast<void*>((uintptr_t)0x4704E0);
        if (MH_CreateHook(t, reinterpret_cast<void*>(&hkPacing),
                          reinterpret_cast<void**>(&g_origPacing)) == MH_OK &&
            MH_QueueEnableHook(t) == MH_OK) {
            g_pacingHooked = true;
            QLOG("[replay] frame-pacing hook on sub_4704E0 enabled (record/replay the truth pacing verdict)");
        } else {
            QLOG("[replay] FAILED to hook sub_4704E0");
        }
    }
    if (g_cosRngMode != CosmeticRngMode::Off && !g_rngHooked) {
        g_frameThread = GetCurrentThreadId();
        static const uint8_t kRngSig[] = { 0xA1, 0x24, 0xBB, 0x4A, 0x00, 0x85, 0xC0, 0x75, 0x1A };
        void* t = reinterpret_cast<void*>(g_rngDrawFn);
        if (!g_rngDrawFn
            || !se::verifyCodeBytes(g_rngDrawFn, kRngSig, sizeof(kRngSig))) {
            QLOG("[rng_redirect] PROLOGUE MISMATCH at profile %s 0x%08X - RNG hook DISABLED",
                 runtime_profile::selectedProfileKey(), (unsigned)g_rngDrawFn);
            g_cosRngMode = CosmeticRngMode::Off;
        } else if (MH_CreateHook(t, reinterpret_cast<void*>(&hkRngDraw),
                                 reinterpret_cast<void**>(&g_origRng)) == MH_OK &&
                   MH_QueueEnableHook(t) == MH_OK) {
            g_rngHooked = true;
            QLOG("[rng_redirect] RNG hook installed at profile %s 0x%08X mode=%s whitelistedSites=%d",
                 runtime_profile::selectedProfileKey(), (unsigned)g_rngDrawFn,
                 g_cosRngMode == CosmeticRngMode::Trace ? "trace" : "whitelist",
                 [] { int n = 0; for (int i = 0; i < g_rngSiteN; ++i) if (g_rngSites[i].whitelisted) n++; return n; }());
        } else {
            QLOG("[rng_redirect] FAILED to hook RNG at profile %s 0x%08X",
                 runtime_profile::selectedProfileKey(), (unsigned)g_rngDrawFn);
            g_cosRngMode = CosmeticRngMode::Off;
        }
    }
}

void configureFromEnv() {
    static bool done = false; if (done) return; done = true;
    char buf[256] = {0};
    if (GetEnvironmentVariableA("QOH99_NP_TRACE_RESIDUAL", buf, sizeof(buf)) && buf[0] == '1') {
        g_traceResidual = true;
        QLOG("[trace] gameplay-residual tracing ENABLED");
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_PACING_REPLAY", buf, sizeof(buf)) && buf[0] == '1') {
        g_pacingReplayEnabled = true;
        g_replayMode = ReplayTickMode::PacingShim;
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_REPLAY_TICK", buf, sizeof(buf)) && buf[0]) {
        if (std::strstr(buf, "deterministic")) g_replayMode = ReplayTickMode::Deterministic;
        else if (std::strstr(buf, "pacingshim")) { g_replayMode = ReplayTickMode::PacingShim; g_pacingReplayEnabled = true; }
        else if (std::strstr(buf, "natural"))  g_replayMode = ReplayTickMode::Natural;
    }
    const char* mn = g_replayMode == ReplayTickMode::Deterministic ? "DeterministicFrameStep"
                   : g_replayMode == ReplayTickMode::PacingShim ? "PacingShimDiagnostic" : "NaturalSub428430";
    QLOG("[replay_tick] mode=%s", mn);
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_SFX_PASSTHROUGH", buf, sizeof(buf)) && buf[0] == '1') {
        g_sfxPassthrough = true;
        QLOG("[replay_tick] audio passthrough ENABLED "
             "(diagnostic: replay audio side effects will execute)");
    }
    configureAudioV2FromEnv();
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_CLOCK_SHIM", buf, sizeof(buf)) && buf[0] == '0') {
        g_clockShimEnabled = false;
        QLOG("[clock_shim] DISABLED by env (A/B: Deterministic mode without wall-clock record/replay)");
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_DOUBLE_REPLAY", buf, sizeof(buf)) && buf[0] == '1') {
        g_doubleReplay = true;
        QLOG("[replay2] double-replay self-consistency probe ENABLED");
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_MINIMAL_DET", buf, sizeof(buf)) && buf[0] == '1') {
        g_minimalDet = true;
        QLOG("[replay_tick] MINIMAL deterministic (4 pacing scalars only; no timing-block restore, no freeze)");
    }
    if (g_replayMode == ReplayTickMode::Deterministic)
        g_cosRngMode = CosmeticRngMode::Trace;
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_COSMETIC_RNG", buf, sizeof(buf)) && buf[0]) {
        if (std::strstr(buf, "trace"))          g_cosRngMode = CosmeticRngMode::Trace;
        else if (std::strstr(buf, "whitelist") ||
                 std::strstr(buf, "narrow")    ||
                 std::strstr(buf, "broad"))     g_cosRngMode = CosmeticRngMode::Whitelist;
        else                                    g_cosRngMode = CosmeticRngMode::Off;
    }
    QLOG("[rng_redirect] mode=%s",
         g_cosRngMode == CosmeticRngMode::Off ? "off" :
         g_cosRngMode == CosmeticRngMode::Trace ? "trace" : "whitelist");
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_DESYNC_DUMP", buf, sizeof(buf)) && buf[0] == '0') {
        g_desyncDumpEnabled = false;
        QLOG("[desync] full desync dump DISABLED by env");
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_FPU_SHIM", buf, sizeof(buf)) && buf[0] == '1') {
        g_fpuStateShim = true;
        QLOG("[fpu_shim] ENABLED by env (FPU control-word restore before replay)");
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_WIDE_DESYNC", buf, sizeof(buf)) && buf[0] == '1') {
        g_wideDesync = true;
        QLOG("[wide_desync] ENABLED: per-frame wide truth capture + full .data truth-vs-replay "
             "diff on every GameplaySync desync (feeder hunt; ~1ms/frame overhead)");
    }
    if (GetEnvironmentVariableA("QOH99_NP_FEEDER_RING", buf, sizeof(buf)) && buf[0] == '1') {
        g_feederRing = true; g_wideDesync = true;
        QLOG("[feeder_ring] ENABLED: full .data ring for K>1 feeder hunt (lazy-alloc)");
    }
    if (GetEnvironmentVariableA("QOH99_NP_WIDE_RESTORE", buf, sizeof(buf)) && buf[0]) {
        g_wideRestore = true; g_feederRing = true; g_wideDesync = true;
        unsigned lo = 0, hi = 0;
        if (sscanf_s(buf, "%x-%x", &lo, &hi) == 2) { g_wideRestoreLo = lo; g_wideRestoreHi = hi; }
        QLOG("[wide_restore] ENABLED range=%08X-%08X (0,0=full): overwrite non-saved .data at "
             "replay-entry with truth frame-startF image (feeder A/B)",
             (unsigned)g_wideRestoreLo, (unsigned)g_wideRestoreHi);
    }
    if (GetEnvironmentVariableA("QOH99_NP_HIST_TRACE", buf, sizeof(buf)) && buf[0] == '1') {
        g_histTrace = true;
        QLOG("[hist_trace] ENABLED: sub_41A540 fighter history-update trace (truth vs replay)");
    }
    if (GetEnvironmentVariableA("QOH99_NP_P56_TRACE", buf, sizeof(buf)) && buf[0] == '1') {
        g_p56Trace = true;
        QLOG("[p56_trace] ENABLED: P2+56 replay-window divergence timeline (no masking hook)");
    }
    if (GetEnvironmentVariableA("QOH99_NP_B600_TRACE", buf, sizeof(buf)) && buf[0] == '1') {
        g_b600Trace = true;
        QLOG("[b600_trace] ENABLED: sub_41B600 P2 callsite ring (post-reset +56 decrement)");
    }
    if (GetEnvironmentVariableA("QOH99_NP_A0_TRACE", buf, sizeof(buf)) && buf[0] == '1') {
        g_a0Trace = true;
        QLOG("[a0_trace] ENABLED: sub_411AE0 P2 anim-advance ring (+56=dur reset feeder)");
    }
    if (GetEnvironmentVariableA("QOH99_NP_FREEZE_CATCHUP", buf, sizeof(buf)) && buf[0] == '1') {
        g_freezeCatchup = true;
        QLOG("[freeze_catchup] ENABLED: sub_470510 catch-up forced 0 for TRUTH tick too "
             "(deterministic fixed-timestep; candidate K>1 fix)");
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_INPUT_TEST", buf, sizeof(buf)) && buf[0] == '1') {
        g_inputTest = true;
        QLOG("[input_test] input verification script ENABLED (P1 driven through the real "
             "sub_484BB0 path: 90-frame phases, dirs 0x10000..0x80000 + buttons)");
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_K", buf, sizeof(buf)) && buf[0]) {
        int k = std::atoi(buf);
        if (k >= 1 && k <= kMaxWindow) {
            g_windowPin = k; g_window = k;
            QLOG("[selftest] K PINNED to %d (env QOH99_NP_K)", k);
        }
    }
    char wl[512] = {0};
    if (GetEnvironmentVariableA("QOH99_NP_RNG_WHITELIST", wl, sizeof(wl)) && wl[0]) {
        char* p = wl;
        while (*p) {
            uintptr_t ra = (uintptr_t)std::strtoul(p, &p, 16);
            if (ra) { RngSite* s = rngSiteFor(ra); if (s) s->whitelisted = true; }
            while (*p == ',' || *p == ' ') ++p;
        }
        QLOG("[rng_redirect] whitelist from env: %d sites total", g_rngSiteN);
    }
    buf[0] = 0;
    if (GetEnvironmentVariableA("QOH99_NP_CANDIDATE", buf, sizeof(buf)) && buf[0]) {
        if (std::strstr(buf, "keystate"))   g_rb.addExtraRegion(0x4AA920, 0x100, "cand_keystate",     RegionRole::GameplayRequired);
        if (std::strstr(buf, "inputwords")) g_rb.addExtraRegion(0x66ECC4, 0x18,  "cand_inputwords",   RegionRole::GameplayRequired);
        if (std::strstr(buf, "inputbind"))  g_rb.addExtraRegion(0x626860, 0x4,   "cand_inputbindptr", RegionRole::GameplayRequired);
        if (std::strstr(buf, "diactive"))   g_rb.addExtraRegion(0x4AA6F8, 0x8,   "cand_diactive",     RegionRole::GameplayRequired);
        if (std::strstr(buf, "joydev"))     g_rb.addExtraRegion(0x4ECB20, 0x40,  "cand_joydev",       RegionRole::GameplayRequired);
        QLOG("[candidate] QOH99_NP_CANDIDATE='%s' applied (only NOT-already-saved regions are useful)", buf);
    }
    buf[0] = 0;
    bool feedersOn = !(GetEnvironmentVariableA("QOH99_NP_FEEDERS", buf, sizeof(buf)) && buf[0] == '0');
    if (feedersOn) {
        g_rb.addExtraRegion(0x8A5378, 0x10, "feeder_anim_ctr_8A5378", RegionRole::RenderCosmetic);
        QLOG("[feeder] render-anim counter added as RenderCosmetic (restored, not hashed)");
    }
}

// Per-fighter frame data: pointer at fighter+24, frameCount at fighter+28, size frameCount<<6.
static bool g_frameDataRegistered = false;
static bool rangeReadable(uintptr_t addr, size_t size) {
    if (!size) return false;
    uintptr_t end = addr + size;
    if (end < addr) return false;
    for (uintptr_t p = addr & ~(uintptr_t)0xFFF; p < end; p += 0x1000) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    }
    return true;
}

constexpr uintptr_t kAiBankTable       = 0x62D640u;
constexpr size_t    kAiBankCount       = 20u;
constexpr size_t    kAiBankStride      = 0x110u;
constexpr size_t    kAiBankPathBytes   = 0x104u;
constexpr size_t    kAiScriptPtrOffset = 0x104u;
constexpr size_t    kAiMetaCountOffset = 0x108u;
constexpr size_t    kAiMetaPtrOffset   = 0x10Cu;
constexpr size_t    kMaxFobBytes       = 16u * 1024u * 1024u;

static const char* const kAiScriptRegionNames[kAiBankCount] = {
    "ai_script_bank_00", "ai_script_bank_01",
    "ai_script_bank_02", "ai_script_bank_03",
    "ai_script_bank_04", "ai_script_bank_05",
    "ai_script_bank_06", "ai_script_bank_07",
    "ai_script_bank_08", "ai_script_bank_09",
    "ai_script_bank_10", "ai_script_bank_11",
    "ai_script_bank_12", "ai_script_bank_13",
    "ai_script_bank_14", "ai_script_bank_15",
    "ai_script_bank_16", "ai_script_bank_17",
    "ai_script_bank_18", "ai_script_bank_19",
};

struct FobImage {
    std::vector<uint8_t> bytes;
    uint32_t descriptorCount = 0;
    uint32_t scriptLength = 0;
    size_t scriptOffset = 0;
};

struct AiBankLayout {
    bool present = false;
    uintptr_t scriptPointer = 0;
    uintptr_t metadataPointer = 0;
    uint32_t metadataCount = 0;
    size_t scriptBytes = 0;
    char path[kAiBankPathBytes] = {};
};

static bool g_aiScriptsRegistered = false;
static AiBankLayout g_aiBankLayout[kAiBankCount] = {};
static bool g_aiBankFailureLogged[kAiBankCount] = {};
static uint64_t g_fobAssetFingerprint = 0;
static bool g_immutableAssetFingerprintReady = false;

inline uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

bool readExactFile(const char* path, std::vector<uint8_t>& bytes) {
    bytes.clear();
    HANDLE file = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER length{};
    const bool lengthOk = GetFileSizeEx(file, &length) != FALSE
        && length.QuadPart >= 0
        && static_cast<uint64_t>(length.QuadPart) <= kMaxFobBytes;
    if (!lengthOk) {
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(length.QuadPart));
    size_t complete = 0;
    while (complete < bytes.size()) {
        const size_t remaining = bytes.size() - complete;
        const DWORD request = static_cast<DWORD>(
            remaining > 0x100000u ? 0x100000u : remaining);
        DWORD got = 0;
        if (!ReadFile(file, bytes.data() + complete, request, &got, nullptr)
            || got == 0) {
            CloseHandle(file);
            bytes.clear();
            return false;
        }
        complete += got;
    }
    CloseHandle(file);
    return true;
}

bool parseFobExact(const char* path, FobImage& image, const char** why) {
    image = {};
    if (!path || !*path) {
        if (why) *why = "empty bank path";
        return false;
    }
    if (!readExactFile(path, image.bytes)) {
        if (why) *why = "cannot read original Fob";
        return false;
    }
    if (image.bytes.size() < 8u) {
        if (why) *why = "Fob shorter than fixed header";
        return false;
    }

    image.descriptorCount = readLe32(image.bytes.data());
    const uint64_t lengthOffset =
        4ull + 36ull * static_cast<uint64_t>(image.descriptorCount);
    if (lengthOffset + 4ull > image.bytes.size()) {
        if (why) *why = "Fob descriptor table exceeds file";
        return false;
    }
    image.scriptLength =
        readLe32(image.bytes.data() + static_cast<size_t>(lengthOffset));
    image.scriptOffset = static_cast<size_t>(lengthOffset + 4ull);
    const uint64_t exactFileSize =
        lengthOffset + 4ull + static_cast<uint64_t>(image.scriptLength);
    if (image.scriptLength == 0
        || exactFileSize != static_cast<uint64_t>(image.bytes.size())
        || static_cast<uint64_t>(image.scriptLength) + 2ull > kMaxFobBytes) {
        if (why) *why = "Fob has a non-exact script payload length";
        return false;
    }
    if (why) *why = nullptr;
    return true;
}

bool readAiBankPath(size_t bank, char (&path)[kAiBankPathBytes]) {
    std::memset(path, 0, sizeof(path));
    const uintptr_t record = kAiBankTable + bank * kAiBankStride;
    if (!rangeReadable(record, kAiBankStride)) return false;
    const char* source = reinterpret_cast<const char*>(record);
    const void* terminator = std::memchr(source, 0, kAiBankPathBytes);
    if (!terminator) return false;
    const size_t length =
        static_cast<const char*>(terminator) - source;
    if (length) std::memcpy(path, source, length);
    return true;
}

bool inspectAiBank(size_t bank, AiBankLayout& layout, FobImage* fob,
                   const char** why) {
    layout = {};
    const uintptr_t record = kAiBankTable + bank * kAiBankStride;
    if (!readAiBankPath(bank, layout.path)) {
        if (why) *why = "bank path is not terminated/readable";
        return false;
    }

    layout.scriptPointer = rd32(record + kAiScriptPtrOffset);
    layout.metadataCount = rd32(record + kAiMetaCountOffset);
    layout.metadataPointer = rd32(record + kAiMetaPtrOffset);
    if (!layout.path[0]) {
        if (layout.scriptPointer || layout.metadataPointer) {
            if (why) *why = "empty bank has a live owner";
            return false;
        }
        if (why) *why = nullptr;
        return true;
    }

    FobImage local;
    FobImage& parsed = fob ? *fob : local;
    if (!parseFobExact(layout.path, parsed, why)) return false;
    if (layout.metadataCount != parsed.descriptorCount) {
        if (why) *why = "loaded metadata count differs from Fob";
        return false;
    }
    const size_t metadataBytes =
        static_cast<size_t>(parsed.descriptorCount) * 36u;
    if (metadataBytes) {
        if (!layout.metadataPointer
            || !rangeReadable(layout.metadataPointer, metadataBytes)
            || std::memcmp(
                reinterpret_cast<const void*>(layout.metadataPointer),
                parsed.bytes.data() + 4u, metadataBytes) != 0) {
            if (why) *why = "loaded metadata bytes differ from Fob";
            return false;
        }
    } else if (layout.metadataPointer) {
        if (why) *why = "zero-descriptor Fob has metadata owner";
        return false;
    }

    layout.scriptBytes = static_cast<size_t>(parsed.scriptLength) + 2u;
    if (!layout.scriptPointer
        || !rangeReadable(layout.scriptPointer, layout.scriptBytes)) {
        if (why) *why = "script allocation is absent or truncated";
        return false;
    }
    const uint8_t* script =
        reinterpret_cast<const uint8_t*>(layout.scriptPointer);
    if (script[parsed.scriptLength] != 0
        || script[parsed.scriptLength + 1u] != 0) {
        if (why) *why = "script allocation lost its two sentinels";
        return false;
    }
    layout.present = true;
    if (why) *why = nullptr;
    return true;
}
struct FdSlot { uintptr_t base; const char* name; bool required; };
static const FdSlot kFdSlots[] = {
    { 0x623E00, "framedata_p1", true  }, { 0x626880, "framedata_p2", true  },
    { 0x4ECB40, "framedata_p3", false }, { 0x617A20, "framedata_p4", false },
    { 0x614060, "framedata_h1", false }, { 0x61465C, "framedata_h2", false },
    { 0x614C58, "framedata_h3", false }, { 0x615254, "framedata_h4", false },
};
constexpr size_t kFdSlotCount = sizeof(kFdSlots) / sizeof(kFdSlots[0]);
static size_t g_fdRegisteredSize[kFdSlotCount] = {};
static uintptr_t g_fdRegisteredPtr[kFdSlotCount] = {};
static bool   g_fdDisabled = false;

static const FdSlot kAssetDescriptorSlots[] = {
    { 0x623E00, "asset_p1",              true  },
    { 0x626880, "asset_p2",              true  },
    { 0x4ECB40, "asset_p3",              false },
    { 0x617A20, "asset_p4",              false },
    { 0x614060, "asset_h1",              false },
    { 0x61465C, "asset_h2",              false },
    { 0x614C58, "asset_h3",              false },
    { 0x615254, "asset_h4",              false },
    { 0x624EE0, "asset_effect_template", true  },
    { 0x8A3740, "asset_system_rina",     true  },
};
constexpr size_t kAssetDescriptorSlotCount =
    sizeof(kAssetDescriptorSlots) / sizeof(kAssetDescriptorSlots[0]);

struct DescriptorFamily {
    size_t pointerOffset;
    size_t countOffset;
    size_t stride;
};
static constexpr DescriptorFamily kDescriptorFamilies[] = {
    {  0u,  4u, 260u },
    {  8u, 12u,  12u },
    { 16u, 20u,  24u },
    { 24u, 28u,  64u },
    { 32u, 36u,   8u },
    { 40u, 44u,  24u },
};
constexpr size_t kDescriptorFamilyCount =
    sizeof(kDescriptorFamilies) / sizeof(kDescriptorFamilies[0]);
constexpr size_t kMaxDescriptorFamilyBytes = 64u * 1024u * 1024u;
static bool g_descriptorSlotPresent[kAssetDescriptorSlotCount] = {};
static uintptr_t
    g_descriptorRegisteredPtr[kAssetDescriptorSlotCount][kDescriptorFamilyCount] = {};
static uint32_t
    g_descriptorRegisteredCount[kAssetDescriptorSlotCount][kDescriptorFamilyCount] = {};

static bool fdSlotValid(const FdSlot& s, size_t* szOut) {
    if (!rangeReadable(s.base + 24, 8)) return false;
    uint32_t frameCount = rd32(s.base + 28);
    uint32_t fdPtr      = rd32(s.base + 24);
    size_t   sz         = (size_t)frameCount << 6;
    if (!(frameCount >= 1 && frameCount <= 0x2000 && fdPtr >= 0x10000 &&
          rangeReadable((uintptr_t)fdPtr, sz))) return false;
    if (szOut) *szOut = sz;
    return true;
}

bool registerFrameDataRegions() {
    if (g_frameDataRegistered) return true;
    char buf[8] = {0};
    if (GetEnvironmentVariableA("QOH99_NP_FRAMEDATA", buf, sizeof(buf)) && buf[0] == '0') {
        g_frameDataRegistered = true; g_fdDisabled = true;
        QLOG("[framedata] frame-data buffer save DISABLED via QOH99_NP_FRAMEDATA=0");
        return true;
    }
    for (size_t i = 0; i < kFdSlotCount; ++i)
        if (kFdSlots[i].required && !fdSlotValid(kFdSlots[i], nullptr)) return false;
    for (size_t i = 0; i < kFdSlotCount; ++i) {
        size_t sz = 0;
        if (fdSlotValid(kFdSlots[i], &sz)) {
            g_rb.addExtraPtrRegion(kFdSlots[i].base, 24, sz, kFdSlots[i].name,
                                   kFdSlots[i].required ? RegionRole::GameplayRequired
                                                        : RegionRole::GameplayCandidate);
            g_fdRegisteredSize[i] = sz;
            g_fdRegisteredPtr[i] = rd32(kFdSlots[i].base + 24);
        } else {
            g_fdRegisteredSize[i] = 0;
            g_fdRegisteredPtr[i] = 0;
            QLOG("[framedata] %s @%08X: descriptor not loaded - skipped (optional slot)",
                 kFdSlots[i].name, (unsigned)kFdSlots[i].base);
        }
    }
    g_frameDataRegistered = true;
    return true;
}

bool registerAiScriptRegions() {
    if (g_aiScriptsRegistered) return true;

    std::array<AiBankLayout, kAiBankCount> candidate{};
    std::array<FobImage, kAiBankCount> fobs{};
    size_t active = 0;
    for (size_t bank = 0; bank < kAiBankCount; ++bank) {
        const char* why = nullptr;
        if (!inspectAiBank(bank, candidate[bank], &fobs[bank], &why)) {
            if (!g_aiBankFailureLogged[bank]) {
                QLOG("[ai_script] bank=%zu registration deferred: %s",
                     bank, why ? why : "invalid bank");
                g_aiBankFailureLogged[bank] = true;
            }
            return false;
        }
        g_aiBankFailureLogged[bank] = false;
        if (candidate[bank].present) ++active;
    }
    if (active == 0) {
        QLOG("[ai_script] no active COM banks; registering exact empty layout");
    }

    static constexpr char kFobDomain[] = "QOH99_FOB_ASSETS_V1";
    uint64_t fobHash =
        netFnv1a(kFobDomain, sizeof(kFobDomain) - 1u);
    for (size_t bank = 0; bank < kAiBankCount; ++bank) {
        const uint32_t bankId = static_cast<uint32_t>(bank);
        const uint32_t present = candidate[bank].present ? 1u : 0u;
        fobHash = netFnv1a(&bankId, sizeof(bankId), fobHash);
        fobHash = netFnv1a(&present, sizeof(present), fobHash);
        if (!candidate[bank].present) continue;

        const uint32_t fileBytes =
            static_cast<uint32_t>(fobs[bank].bytes.size());
        fobHash = netFnv1a(&fileBytes, sizeof(fileBytes), fobHash);
        fobHash = netFnv1a(
            fobs[bank].bytes.data(), fobs[bank].bytes.size(), fobHash);

        const uint8_t* liveScript = reinterpret_cast<const uint8_t*>(
            candidate[bank].scriptPointer);
        const bool stillOriginal =
            std::memcmp(
                liveScript,
                fobs[bank].bytes.data() + fobs[bank].scriptOffset,
                fobs[bank].scriptLength) == 0;
        g_rb.addExternalOwnerPtrRegion(
            kAiBankTable + bank * kAiBankStride + kAiScriptPtrOffset,
            candidate[bank].scriptBytes, kAiScriptRegionNames[bank],
            RegionRole::GameplayRequired, static_cast<uint8_t>(bank));
        QLOG("[ai_script] bank=%zu owner=%08X child=%08X bytes=%zu "
             "descriptors=%u source=%s",
             bank,
             static_cast<unsigned>(
                 kAiBankTable + bank * kAiBankStride
                 + kAiScriptPtrOffset),
             static_cast<unsigned>(candidate[bank].scriptPointer),
             candidate[bank].scriptBytes,
             candidate[bank].metadataCount,
             stillOriginal ? "original" : "already-mutated");
    }

    for (size_t bank = 0; bank < kAiBankCount; ++bank)
        g_aiBankLayout[bank] = candidate[bank];
    g_fobAssetFingerprint = fobHash != 0
        ? fobHash : 0x514F483939464F42ull;
    g_aiScriptsRegistered = true;
    return true;
}

bool descriptorFamilyInfo(
    const FdSlot& slot, const DescriptorFamily& family,
    uintptr_t* pointerOut, uint32_t* countOut, size_t* bytesOut) {
    if (!rangeReadable(slot.base, 48u)) return false;
    const uintptr_t pointer = rd32(slot.base + family.pointerOffset);
    const uint32_t count = rd32(slot.base + family.countOffset);
    if (count > kMaxDescriptorFamilyBytes / family.stride) return false;
    const size_t bytes = static_cast<size_t>(count) * family.stride;
    if (bytes && (pointer < 0x10000u || !rangeReadable(pointer, bytes)))
        return false;
    if (pointerOut) *pointerOut = pointer;
    if (countOut) *countOut = count;
    if (bytesOut) *bytesOut = bytes;
    return true;
}

bool descriptorSlotHasAnyState(const FdSlot& slot) {
    if (!rangeReadable(slot.base, 48u)) return true;
    for (const DescriptorFamily& family : kDescriptorFamilies) {
        if (rd32(slot.base + family.pointerOffset)
            || rd32(slot.base + family.countOffset))
            return true;
    }
    return false;
}

bool computeImmutableAssetFingerprint(uint64_t* fingerprint) {
    if (!g_aiScriptsRegistered || g_fobAssetFingerprint == 0) return false;

    bool slotPresent[kAssetDescriptorSlotCount] = {};
    uintptr_t
        familyPointers[kAssetDescriptorSlotCount][kDescriptorFamilyCount] = {};
    uint32_t
        familyCounts[kAssetDescriptorSlotCount][kDescriptorFamilyCount] = {};

    static constexpr char kDescriptorDomain[] =
        "QOH99_DESCRIPTOR_ASSETS_V2";
    uint64_t hash = netFnv1a(
        kDescriptorDomain, sizeof(kDescriptorDomain) - 1u,
        g_fobAssetFingerprint);
    for (size_t slotIndex = 0;
         slotIndex < kAssetDescriptorSlotCount; ++slotIndex) {
        const FdSlot& slot = kAssetDescriptorSlots[slotIndex];
        size_t frameBytes = 0;
        const bool loaded = fdSlotValid(slot, &frameBytes);
        if (!loaded && (slot.required || descriptorSlotHasAnyState(slot)))
            return false;

        slotPresent[slotIndex] = loaded;
        const uint32_t logicalSlot = static_cast<uint32_t>(slotIndex);
        const uint32_t present = loaded ? 1u : 0u;
        hash = netFnv1a(&logicalSlot, sizeof(logicalSlot), hash);
        hash = netFnv1a(&present, sizeof(present), hash);
        if (!loaded) continue;

        for (size_t familyIndex = 0;
             familyIndex < kDescriptorFamilyCount; ++familyIndex) {
            uintptr_t pointer = 0;
            uint32_t count = 0;
            size_t bytes = 0;
            if (!descriptorFamilyInfo(
                    slot, kDescriptorFamilies[familyIndex],
                    &pointer, &count, &bytes)) {
                return false;
            }
            familyPointers[slotIndex][familyIndex] = pointer;
            familyCounts[slotIndex][familyIndex] = count;

            const uint32_t logicalFamily =
                static_cast<uint32_t>(familyIndex);
            const uint32_t byteCount = static_cast<uint32_t>(bytes);
            hash = netFnv1a(
                &logicalFamily, sizeof(logicalFamily), hash);
            hash = netFnv1a(&count, sizeof(count), hash);
            hash = netFnv1a(&byteCount, sizeof(byteCount), hash);
            if (bytes) {
                hash = netFnv1a(
                    reinterpret_cast<const void*>(pointer), bytes, hash);
            }
        }
    }

    for (size_t slotIndex = 0;
         slotIndex < kAssetDescriptorSlotCount; ++slotIndex) {
        g_descriptorSlotPresent[slotIndex] = slotPresent[slotIndex];
        for (size_t familyIndex = 0;
             familyIndex < kDescriptorFamilyCount; ++familyIndex) {
            g_descriptorRegisteredPtr[slotIndex][familyIndex] =
                familyPointers[slotIndex][familyIndex];
            g_descriptorRegisteredCount[slotIndex][familyIndex] =
                familyCounts[slotIndex][familyIndex];
        }
    }
    if (fingerprint) {
        *fingerprint = hash != 0
            ? hash : 0x514F483939415353ull;
    }
    return true;
}

bool registerImmutableAssetFingerprint() {
    if (g_immutableAssetFingerprintReady) return true;
    uint64_t fingerprint = 0;
    if (!computeImmutableAssetFingerprint(&fingerprint)) return false;
    if (!g_rb.setImmutableAssetFingerprint(fingerprint)) {
        QLOG("[assets] immutable fingerprint changed while rollback pool "
             "is live; refusing a mixed-layout generation");
        return false;
    }
    g_immutableAssetFingerprintReady = true;
    QLOG("[assets] immutable post-load fingerprint=%016llX "
         "(original Fob files + six descriptor families)",
         static_cast<unsigned long long>(fingerprint));
    return true;
}

bool aiScriptConfigChanged() {
    if (!g_aiScriptsRegistered) return false;
    for (size_t bank = 0; bank < kAiBankCount; ++bank) {
        char livePath[kAiBankPathBytes] = {};
        if (!readAiBankPath(bank, livePath)) return true;
        const uintptr_t record = kAiBankTable + bank * kAiBankStride;
        const uintptr_t script = rd32(record + kAiScriptPtrOffset);
        const uint32_t count = rd32(record + kAiMetaCountOffset);
        const uintptr_t metadata = rd32(record + kAiMetaPtrOffset);
        const AiBankLayout& saved = g_aiBankLayout[bank];
        if (!saved.present) {
            if (livePath[0] || script || metadata) return true;
            continue;
        }
        if (!livePath[0]
            || std::strcmp(livePath, saved.path) != 0
            || script != saved.scriptPointer
            || metadata != saved.metadataPointer
            || count != saved.metadataCount
            || !rangeReadable(script, saved.scriptBytes)) {
            return true;
        }
        const uint8_t* bytes =
            reinterpret_cast<const uint8_t*>(script);
        if (bytes[saved.scriptBytes - 2u] != 0
            || bytes[saved.scriptBytes - 1u] != 0) {
            return true;
        }
    }
    return false;
}

bool descriptorConfigChanged() {
    if (!g_immutableAssetFingerprintReady) return false;
    for (size_t slotIndex = 0;
         slotIndex < kAssetDescriptorSlotCount; ++slotIndex) {
        const FdSlot& slot = kAssetDescriptorSlots[slotIndex];
        size_t frameBytes = 0;
        const bool loaded = fdSlotValid(slot, &frameBytes);
        if (loaded != g_descriptorSlotPresent[slotIndex]) return true;
        if (!loaded) {
            if (descriptorSlotHasAnyState(slot)) return true;
            continue;
        }
        for (size_t familyIndex = 0;
             familyIndex < kDescriptorFamilyCount; ++familyIndex) {
            uintptr_t pointer = 0;
            uint32_t count = 0;
            size_t bytes = 0;
            if (!descriptorFamilyInfo(
                    slot, kDescriptorFamilies[familyIndex],
                    &pointer, &count, &bytes)
                || pointer
                    != g_descriptorRegisteredPtr[slotIndex][familyIndex]
                || count
                    != g_descriptorRegisteredCount[slotIndex][familyIndex]) {
                return true;
            }
        }
    }
    return false;
}

bool frameDataConfigChanged() {
    if (!g_frameDataRegistered || g_fdDisabled) return false;
    for (size_t i = 0; i < kFdSlotCount; ++i) {
        size_t sz = 0;
        bool valid = fdSlotValid(kFdSlots[i], &sz);
        if (g_fdRegisteredSize[i]) {
            if (!valid || sz != g_fdRegisteredSize[i]
                || rd32(kFdSlots[i].base + 24)
                    != g_fdRegisteredPtr[i]) {
                return true;
            }
        } else if (valid && !kFdSlots[i].required) {
            return true;
        }
    }
    return false;
}

bool gameplayLayoutConfigChanged() {
    return frameDataConfigChanged()
        || descriptorConfigChanged()
        || aiScriptConfigChanged();
}

void reregisterFrameData() {
    QLOG("[rollback] gameplay-layout boundary detected - re-registering "
         "descriptor/script children + rebuilding pool");
    g_frameDataRegistered = false;
    for (size_t i = 0; i < kFdSlotCount; ++i) {
        g_fdRegisteredSize[i] = 0;
        g_fdRegisteredPtr[i] = 0;
    }
    for (size_t i = 0; i < kAssetDescriptorSlotCount; ++i) {
        g_descriptorSlotPresent[i] = false;
        for (size_t family = 0;
             family < kDescriptorFamilyCount; ++family) {
            g_descriptorRegisteredPtr[i][family] = 0;
            g_descriptorRegisteredCount[i][family] = 0;
        }
    }
    g_aiScriptsRegistered = false;
    for (size_t bank = 0; bank < kAiBankCount; ++bank) {
        g_aiBankLayout[bank] = {};
        g_aiBankFailureLogged[bank] = false;
    }
    g_fobAssetFingerprint = 0;
    g_immutableAssetFingerprintReady = false;
    g_rb.clearExtraPtrRegions();
    g_rb.invalidateSlots();
    g_rbReady = false;
    if (g_netActive) {
        ++g_netEpoch;
        if (g_netEpoch == 0) ++g_netEpoch;
        netInvalidateFpu();
        g_netGameplaySegmentActive = false;
        g_spectatorGameplayEntryStartedMs = 0;
        g_spectatorGameplayEntryLastLogMs = 0;
        g_netTransitionBlocked = 0xFFFFFFFFu;
        if (g_audioV2Enabled) {
            qaudio::runtime().rotateEpoch(g_netEpoch);
        }
        QLOG("[netplay] rollback layout epoch advanced to %u", g_netEpoch);
    }
}

bool ensureRollback() {
    if (g_rbReady) return true;
    configureFromEnv();
    static uint64_t s_erStageMs = 0;
    const uint64_t erNow = GetTickCount64();
    const bool erLog = erNow - s_erStageMs >= 1000u;
    if (erLog) s_erStageMs = erNow;
    if (!registerFrameDataRegions()) {
        if (erLog) QLOG("[hooks] ensureRollback stage=frameData refused");
        return false;
    }
    if (!registerAiScriptRegions()) {
        if (erLog) QLOG("[hooks] ensureRollback stage=aiScript refused");
        return false;
    }
    if (!registerImmutableAssetFingerprint()) {
        if (erLog) QLOG("[hooks] ensureRollback stage=assetFingerprint refused");
        return false;
    }
    if (g_rb.init(kPoolSlots)) {
        g_expected.resize(g_rb.totalSize());
        g_rbReady = true;
        QLOG("[hooks] rollback online (stateSize=%zu, fixed=%zu ptr=%zu, "
             "assets=%016llX, maxWindow=%d)",
             g_rb.totalSize(), g_rb.fixedRegionCount(),
             g_rb.ptrRegionCount(),
             static_cast<unsigned long long>(
                 g_rb.immutableAssetFingerprint()),
             kMaxWindow);
    }
    return g_rbReady;
}

#include "training/TrainingGameIntegration.inl"

void traceArenaDiff(const char* region, const uint8_t* truthBlock,
                    const uint8_t* liveBlock, size_t n, size_t firstOff) {
    size_t lo = firstOff > 32 ? firstOff - 32 : 0;
    size_t hi = firstOff + 32 < n ? firstOff + 32 : n;
    char tbuf[256], rbuf[256]; int tp = 0, rp = 0;
    for (size_t i = lo; i < hi && tp < 240; ++i) {
        tp += std::snprintf(tbuf + tp, sizeof(tbuf) - tp, "%02X", truthBlock[i]);
        rp += std::snprintf(rbuf + rp, sizeof(rbuf) - rp, "%02X", liveBlock[i]);
    }
    QLOG("[arena4d4] region=%s firstOff=0x%zX window=[0x%zX..0x%zX]", region, firstOff, lo, hi);
    QLOG("[arena4d4]   truth =%s", tbuf);
    QLOG("[arena4d4]   replay=%s", rbuf);
    char ranges[512]; int rr = 0; size_t rs = 0; bool in = false; int cnt = 0;
    for (size_t i = 0; i <= n; ++i) {
        bool d = (i < n) && (truthBlock[i] != liveBlock[i]);
        if (d && !in) { in = true; rs = i; }
        else if (!d && in) {
            in = false;
            if (cnt < 24 && rr < 460)
                rr += std::snprintf(ranges + rr, sizeof(ranges) - rr, "[0x%zX+%zu]", rs, i - rs);
            cnt++;
        }
    }
    QLOG("[arena4d4]   diffRanges(%d)=%s", cnt, ranges);
}

void logSyncHashOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    std::vector<RollbackManager::RegionHash> per;
    uint64_t full = g_rb.hashLive(&per);
    QLOG("[sync] frame=%llu full=%016llX regions=%zu",
         (unsigned long long)g_frame, (unsigned long long)full, per.size());
    for (const auto& rh : per)
        QLOG("[sync]   region=%-22s size=%-7zu present=%d hash=%016llX",
             rh.name, rh.size, (int)rh.present, (unsigned long long)rh.hash);
}

void logRegionLayoutOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    size_t n = 0; const Region* r = saveRegions(n);
    for (size_t i = 0; i < n; ++i)
        QLOG("[save] region=%-22s addr=%08X size=%-7zu role=%s hash=%s",
             r[i].name, (unsigned)r[i].addr, r[i].size,
             inGameplayHash(r[i].role) ? "GameplayRequired" : "RenderCosmetic",
             inGameplayHash(r[i].role) ? "GameplaySync" : "RenderCosmetic");
    size_t pc = 0; const PtrRegion* pr = pointerRegions(pc);
    for (size_t i = 0; i < pc; ++i)
        QLOG("[save] region=%-22s ptr=%08X+%zu size=%-7zu role=%s "
             "activation=%08X",
             pr[i].name, (unsigned)pr[i].fighterBase,
             pr[i].ptrOffset, pr[i].size,
             pr[i].role == RegionRole::GameplayRequired
                 ? "GameplayRequired"
                 : "GameplayCandidate",
             static_cast<unsigned>(
                 pr[i].activationPointerAddress));
}

void doObserve() {
    static uint64_t s_lastTraceMs = 0;
    const uint64_t nowMs = GetTickCount64();
    if (s_lastTraceMs != 0 && nowMs - s_lastTraceMs < 5000u) return;
    s_lastTraceMs = nowMs;
    QLOG("frame=%llu scene=%d matchSub=%d sel=%u active=%08X rngG=%08X life=[%d,%d]",
         (unsigned long long)g_frame,
         (int)rd32(kSceneStateAddr), (int)rd32(kMatchSubAddr),
         rd32(kBlockSelector), rd32(kActiveBlockPtr), rd32(kRngSeedGameplay),
         netBattleLifeEffective(0), netBattleLifeEffective(1));
}

static volatile int  g_stPhase    = 0;
static volatile long g_stCrash    = 0;
static bool          g_stDisabled = false;
static int           g_stResult   = 0;
static uint64_t      g_okAtWindow = 0;
static uint32_t      g_selBefore = 0, g_selAfter = 0, g_actBefore = 0, g_actAfter = 0;
static RollbackManager::Diff g_rcDiff{};
static bool          g_rcOk = true;
static RollbackManager::Diff g_fdDiff{};
static bool          g_fdOk = true;
static RollbackManager::Diff g_stDiff{};

struct FighterDbg {
    uint32_t f56, f48, f52, f88, f1400;
    uint32_t rngG, rngC, rngSel;
    uint32_t inMerged, inP1, inP2;
    uint32_t keyHash;
    uint32_t camX, camPrev;
    uint32_t p1x, p2x, p1c1312;
    uint32_t roundTimer;
    uint32_t activeArena, selArena;
};
static FighterDbg g_truthDbg{}, g_replayDbg{};
static uint32_t   g_prevF56 = 0;
static uint32_t   g_rngSeedAtSave = 0;
inline void readFighterDbg(FighterDbg& d) {
    d.f56    = rd32(kFighterP1Addr + 56);
    d.f48    = rd32(kFighterP1Addr + 48);
    d.f52    = rd32(kFighterP1Addr + 52);
    d.f88    = rd32(kFighterP1Addr + 88);
    d.f1400  = rd32(kFighterP1Addr + 1400);
    d.rngG   = rd32(0x4A9938);
    d.rngC   = rd32(0x4A993C);
    d.rngSel = rd32(0x4ABB24);
    d.inMerged = rd32(0x66ECC4);
    d.inP1   = rd32(0x66ECD4);
    d.inP2   = rd32(0x66ECD8);
    d.camX     = rd32(0x626E80);
    d.camPrev  = rd32(0x62D630);
    d.p1x      = rd32(kFighterP1Addr + 136);
    d.p2x      = rd32(kFighterP2Addr + 136);
    d.p1c1312  = rd32(kFighterP1Addr + 1312);
    d.roundTimer  = rd32(0x6236C8);
    d.activeArena = rd32(kActiveBlockPtr);
    d.selArena    = rd32(kBlockSelector);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 0x100; i += 4) { h ^= rd32(0x4AA920 + i); h *= 16777619u; }
    d.keyHash = h;
}
inline int lcgSteps(uint32_t start, uint32_t end, int maxSteps = 100000) {
    uint32_t s = start;
    for (int i = 0; i <= maxSteps; ++i) {
        if (s == end) return i;
        s = 156683941u * s + 1u;
    }
    return -1;
}

static uint32_t fnv32(uintptr_t p, size_t n) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
    uint32_t h = 2166136261u; while (n--) { h ^= *b++; h *= 16777619u; } return h;
}
struct TickGate {
    uint32_t scene, sceneNext; int32_t matchSub;
    uint32_t active, selector, latch, frameCtr;
    uint32_t keyHash, menuIn, remoteIn, netSeq, poolHeadsHash;
    uint32_t f48, f52, f56, f64, f72, f84, f88, f1400;
    uint32_t c6236C8, c6236CC, c6236C4, c6236A8;
};
static TickGate g_gBeforeTruth{}, g_gAfterTruth{}, g_gAfterRestore{}, g_gAfterReplay{};
static uint32_t g_restoreDiffBytes = 0;
inline void captureGate(TickGate& g) {
    g.scene = rd32(kSceneStateAddr); g.sceneNext = rd32(kSceneNextAddr);
    g.matchSub = (int32_t)rd32(kMatchSubAddr);
    g.active = rd32(kActiveBlockPtr); g.selector = rd32(kBlockSelector);
    g.latch = rd32(0x6267D4); g.frameCtr = rd32(0x4AB778);
    g.keyHash = fnv32(kKeyStateAddr, 256);
    g.menuIn = rd32(0x66ECC4); g.remoteIn = rd32(0x66ECD4); g.netSeq = rd32(0x66ECCC);
    g.poolHeadsHash = fnv32(0x613E00, 0x20);
    const uintptr_t f = kFighterP1Addr;
    g.f48 = rd32(f + 48); g.f52 = rd32(f + 52); g.f56 = rd32(f + 56); g.f64 = rd32(f + 64);
    g.f72 = rd32(f + 72); g.f84 = rd32(f + 84); g.f88 = rd32(f + 88); g.f1400 = rd32(f + 1400);
    g.c6236C8 = rd32(0x6236C8); g.c6236CC = rd32(0x6236CC);
    g.c6236C4 = rd32(0x6236C4); g.c6236A8 = rd32(0x6236A8);
}
#define GDIF(field) if (a.field != b.field) \
    QLOG("[tick_gate_diff] field=%-10s truth=%08X replay=%08X", #field, a.field, b.field)
static void dumpGateDiff(const char* what, const TickGate& a, const TickGate& b) {
    QLOG("[tick_gate] %s (truth-vs-replay diffs below; none = replay reproduced truth)", what);
    GDIF(scene); GDIF(sceneNext); GDIF(matchSub); GDIF(active); GDIF(selector);
    GDIF(latch); GDIF(frameCtr); GDIF(keyHash); GDIF(menuIn); GDIF(remoteIn); GDIF(netSeq);
    GDIF(poolHeadsHash); GDIF(f48); GDIF(f52); GDIF(f56); GDIF(f64); GDIF(f72); GDIF(f84);
    GDIF(f88); GDIF(f1400); GDIF(c6236C8); GDIF(c6236CC); GDIF(c6236C4); GDIF(c6236A8);
}
#undef GDIF

inline int nextWindow(int w) { int n = w * 2; return n < kMaxWindow ? n : kMaxWindow; }

constexpr uintptr_t kWideBase = 0x499000, kWideEnd = 0x8A7000;
constexpr size_t    kWideSize = kWideEnd - kWideBase;
static std::vector<uint8_t> g_wideTruth, g_wideReplay;
static volatile bool g_wideWant = false, g_wideCaptured = false;

static bool ptrChildCovers(const PtrRegion& pr, uintptr_t addr) {
    const void* pp = reinterpret_cast<const void*>(pr.fighterBase + pr.ptrOffset);
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(pp, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    uintptr_t base = *static_cast<const volatile uint32_t*>(pp);
    return base && addr >= base && addr < base + pr.size;
}
static bool inSaveSet(uintptr_t addr) {
    if (g_rbReady) {
        for (const Region& r : g_rb.fixedRegions())
            if (addr >= r.addr && addr < r.addr + r.size) return true;
        for (const PtrRegion& pr : g_rb.pointerChildRegions())
            if (ptrChildCovers(pr, addr)) return true;
        return false;
    }
    size_t n = 0; const Region* r = saveRegions(n);
    for (size_t i = 0; i < n; ++i)
        if (addr >= r[i].addr && addr < r[i].addr + r[i].size) return true;
    size_t pc = 0; const PtrRegion* pr = pointerRegions(pc);
    for (size_t i = 0; i < pc; ++i)
        if (ptrChildCovers(pr[i], addr)) return true;
    return false;
}
static bool pageReadable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    return mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
static bool pageWritable(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    const DWORD kW = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return mbi.State == MEM_COMMIT && (mbi.Protect & kW) && !(mbi.Protect & PAGE_GUARD);
}
static void copyPageGuarded(uint8_t* dst, const void* src) {
    __try { std::memcpy(dst, src, 0x1000); }
    __except (EXCEPTION_EXECUTE_HANDLER) { std::memset(dst, 0, 0x1000); }
}
static void wideRead(uint8_t* dst) {
    for (size_t off = 0; off < kWideSize; off += 0x1000) {
        const void* p = reinterpret_cast<const void*>(kWideBase + off);
        if (pageReadable(p)) copyPageGuarded(dst + off, p);
        else                 std::memset(dst + off, 0, 0x1000);
    }
}
static void wideWrite(const uint8_t* src) {
    for (size_t off = 0; off < kWideSize; off += 0x1000) {
        void* p = reinterpret_cast<void*>(kWideBase + off);
        if (pageWritable(p)) std::memcpy(p, src + off, 0x1000);
    }
}
static void wideWriteRange(const uint8_t* src, uintptr_t lo, uintptr_t hi) {
    uintptr_t a = (lo && lo > kWideBase) ? (lo & ~0xFFFull) : kWideBase;
    uintptr_t b = (hi && hi <= kWideEnd) ? hi : kWideEnd;
    for (uintptr_t va = a; va < b; va += 0x1000) {
        void* p = reinterpret_cast<void*>(va);
        if (pageWritable(p)) std::memcpy(p, src + (va - kWideBase), 0x1000);
    }
}

static volatile uintptr_t g_faultAddr = 0;
static int sehFilter(unsigned code, EXCEPTION_POINTERS* ep) {
    switch (code) {
        case 0x80000001u:
        case 0x80000003u:
        case 0x80000004u:
        case 0x4001000Au:
        case 0x40010006u:
            return EXCEPTION_CONTINUE_SEARCH;
        default:
            g_faultAddr = ep && ep->ExceptionRecord
                        ? (uintptr_t)ep->ExceptionRecord->ExceptionAddress : 0;
            return EXCEPTION_EXECUTE_HANDLER;
    }
}

static bool selfTestGuarded(uint64_t f, int* rOut) {
    const int slots = kPoolSlots;
    const int win   = g_window;
    __try {
        g_ovrActive = true;
        const bool nonNatural = (g_replayMode != ReplayTickMode::Natural);
        g_stPhase = 1; g_ovrFrame = f;
        g_prevF56 = rd32(kFighterP1Addr + 56);
        if (g_traceResidual) captureGate(g_gBeforeTruth);
        uint32_t frameCtrBefore = rd32(0x4AB778);
        uint32_t p2StepsBefore = g_histCallP2;
        if (nonNatural) capturePacingState((int)(f % slots));
        if (g_wideDesync) {
            if (g_wideTruth.size() < kWideSize) { g_wideTruth.resize(kWideSize); g_wideReplay.resize(kWideSize); }
            wideRead(g_wideTruth.data());
        }
        g_stPhase = 2; g_rb.saveToSlot((size_t)(f % slots));
        if (g_feederRing) {
            if (g_wideRing.size() != (size_t)slots) {
                g_wideRing.resize(slots);
                for (auto& v : g_wideRing) v.assign(kWideSize, 0);
            }
            wideRead(g_wideRing[f % slots].data());
        }
        g_rngSeedAtSave = rd32(0x4A9938);
        if (g_desyncDumpEnabled) g_rb.hashLive(&g_hashBeforeTruth);
        if (g_pacingReplayEnabled) { g_pacingRecN = 0; g_pacingMode = 1; }
        const bool clockShim = (g_replayMode == ReplayTickMode::Deterministic && g_clockHooked && g_clockShimEnabled);
        if (clockShim) clockBeginRecord((int)(f % slots));
        if (g_rngHooked) { rngResetFrameCounts(); rngBeginTruth((int)(f % slots)); }
        if (audioHooksReady() && nonNatural)
            audioBeginRecord((int)(f % slots));
        if (g_pollHooked && nonNatural) ksBeginRecord((int)(f % slots));
        g_stPhase = 3; *rOut = g_orig();
        if (g_pollHooked) ksOff();
        if (audioHooksReady()) audioOff();
        if (g_rngHooked) rngPhaseOff();
        if (clockShim) clockOff();
        g_pacingMode = 0;
        uint32_t frameCtrAfterTruth = rd32(0x4AB778);
        bool truthAdvanced = (frameCtrAfterTruth != frameCtrBefore);
        g_lastTruthAdvanced = truthAdvanced; g_lastFrameCtrTruth = frameCtrAfterTruth;
        g_advSlot[f % slots] = truthAdvanced;
        g_advCount[f % slots] = frameCtrAfterTruth - frameCtrBefore;
        g_truthP2Steps[f % slots] = g_histCallP2 - p2StepsBefore;
        g_p2_56_truth[f % slots] = rd32(0x6268B8);
        g_p2_48_truth[f % slots] = rd32(0x626880 + 48);
        g_p2_52_truth[f % slots] = rd32(0x626880 + 52);
        g_p2_64_truth[f % slots] = rd32(0x626880 + 64);
        g_p2_16_truth[f % slots] = rd32(0x626880 + 16); g_p2_24_truth[f % slots] = rd32(0x626880 + 24);
        if (g_p56Trace) for (int gi = 0; gi < 8; ++gi) g_gT[gi][f % slots] = rd32(kGateAddr[gi]);
        static uint64_t s_advLogged = 0;
        if (nonNatural && (s_advLogged < 6 || (f % 600) == 0)) {
            QLOG("[advance_detect] phase=truth advanced=%d frameCtrBefore=%u frameCtrAfter=%u",
                 (int)truthAdvanced, frameCtrBefore, frameCtrAfterTruth);
            s_advLogged++;
        }
        if ((int)rd32(kMatchSubAddr) != 0) { g_ovrActive = false; g_stResult = 2; g_stPhase = 0; return true; }
        if (nonNatural && !truthAdvanced) {
            static uint64_t s_skip = 0;
            if (s_skip < 4) { QLOG("[selftest] skip frame=%llu reason=truth_no_advance", (unsigned long long)f); s_skip++; }
            g_ovrActive = false; g_stResult = 2; g_stPhase = 0; return true;
        }
        if (f >= (uint64_t)win) {
            const uint64_t startF = f - win + 1;
            g_lastSlot = (size_t)(startF % slots);
            g_stPhase = 4; g_rb.snapshotTo(g_expected.data());
            if (g_desyncDumpEnabled) g_rb.hashLive(&g_hashAfterTruth);
            readFighterDbg(g_truthDbg);
            if (g_traceResidual) captureGate(g_gAfterTruth);
            if (g_wideWant) wideRead(g_wideTruth.data());
            g_selBefore = rd32(kBlockSelector); g_actBefore = rd32(kActiveBlockPtr);
            g_stPhase = 5; g_rb.loadFromSlot((size_t)(startF % slots));
            if (g_wideDesync && g_wideReplay.size() >= kWideSize) wideRead(g_wideReplay.data());
            if (g_wideRestore && g_wideRing.size() == (size_t)slots)
                wideWriteRange(g_wideRing[startF % slots].data(), g_wideRestoreLo, g_wideRestoreHi);
            g_selAfter = rd32(kBlockSelector); g_actAfter = rd32(kActiveBlockPtr);
            {
                RollbackManager::Diff rd{};
                g_rb.verifyAgainstSlot((size_t)(startF % slots), &rd, RollbackManager::Profile::All);
                g_restoreDiffBytes = (uint32_t)rd.diffBytes;
                if (g_traceResidual) captureGate(g_gAfterRestore);
                if (g_desyncDumpEnabled) g_rb.hashLive(&g_hashAfterRestore);
            }
            static uint64_t s_rvLogged = 0;
            uint32_t rvActive = rd32(kActiveBlockPtr);
            if (s_rvLogged < 4 || (f % 600) == 0) {
                QLOG("[restore_validate] frame=%llu K=%d activeArena=%08X(%s) matchSub=%d sel=%u",
                     (unsigned long long)f, win, rvActive,
                     (rvActive == kBlock0Addr || rvActive == kBlock1Addr) ? "ok" : "BAD",
                     (int)rd32(kMatchSubAddr), rd32(kBlockSelector));
                s_rvLogged++;
            }
            static uint64_t s_replayLogged = 0;
            bool logReplay = (s_replayLogged < 4 || (f % 600) == 0);
            if (logReplay) {
                QLOG("[replay] begin from=%llu to=%llu K=%d",
                     (unsigned long long)startF, (unsigned long long)f, win);
                QLOG("[render_suppress] frame=%llu replay ticks not presented; "
                     "audio gated=%d palette retained=%d",
                     (unsigned long long)f, (int)audioHooksReady(),
                     (int)g_paletteHooked);
                s_replayLogged++;
            }
            g_replayActive = true;
            uint64_t audioBefore = g_audioSuppressed;
            if (g_p56Trace)
                for (int w = 0; w < win && w < (int)slots; ++w) g_winValid[w] = false;
            for (uint64_t i = startF; i <= f; ++i) {
                if (nonNatural && !g_advSlot[i % slots]) continue;
                g_stPhase = 6; g_ovrFrame = i;
                uint32_t rFrameCtrBefore = rd32(0x4AB778);
                uint32_t rP2StepsBefore = g_histCallP2;
                if (g_replayMode == ReplayTickMode::Deterministic) {
                    restorePacingState((int)(i % slots));
                    g_freezeGateCalls = 0;
                    g_pacingFreeze = !g_minimalDet;
                }
                if (g_pacingReplayEnabled) { g_pacingReplayIdx = 0; g_pacingMode = 2; }
                if (clockShim) clockBeginReplay((int)(i % slots));
                if (g_rngHooked) rngBeginReplay((int)(i % slots), i == f);
                if (audioHooksReady() && nonNatural)
                    audioBeginReplay((int)(i % slots));
                if (g_pollHooked && nonNatural) ksBeginReplay((int)(i % slots));
                g_stPhase = 7; g_orig();
                if (g_pollHooked) ksOff();
                if (audioHooksReady()) audioOff();
                if (g_rngHooked) rngPhaseOff();
                if (clockShim) clockOff();
                g_pacingFreeze = false;
                if (g_p56Trace) {
                    size_t wi = (size_t)(i - startF);
                    if (wi < (size_t)slots) {
                        g_winTruth56[wi] = g_p2_56_truth[i % slots];
                        g_winRep56[wi]   = rd32(0x6268B8);
                        g_winAct[wi]     = rd32(0x626880 + 364);
                        g_win208[wi]     = rd32(0x626880 + 208);
                        g_winP2x[wi]     = rd32(0x626880 + 136);
                        g_winT48[wi] = g_p2_16_truth[i % slots]; g_winR48[wi] = rd32(0x626880 + 16);
                        g_winT52[wi] = g_p2_24_truth[i % slots]; g_winR52[wi] = rd32(0x626880 + 24);
                        g_winT64[wi] = g_p2_64_truth[i % slots]; g_winR64[wi] = rd32(0x626880 + 64);
                        uint32_t p16 = rd32(0x626880 + 16), p24 = rd32(0x626880 + 24);
                        uint32_t p48 = rd32(0x626880 + 48), p64 = rd32(0x626880 + 64);
                        uint32_t vb = (p16 && pageReadable((void*)(p16 + 24*p48))) ? rd32(p16 + 24*p48) : 0;
                        uint32_t fd = ((p64 + vb) << 6) + p24;
                        g_winRfd[wi]  = fd;
                        g_winRdur[wi] = pageReadable((void*)(uintptr_t)(fd + 8)) ? rd32(fd + 8) : 0xBADBAD;
                        g_winRd[wi]   = (int32_t)(rd32(0x4AB778) - rFrameCtrBefore);
                        for (int gi = 0; gi < 8; ++gi) { g_gR[gi][wi] = rd32(kGateAddr[gi]); }
                        g_winValid[wi] = true;
                    }
                }
                g_pacingMode = 0;
                if (i == f) {
                    g_lastFrameCtrReplay = rd32(0x4AB778);
                    g_lastReplayAdvanced = (g_lastFrameCtrReplay != rFrameCtrBefore);
                }
                if (g_histTrace) {
                    uint32_t replaySteps = g_histCallP2 - rP2StepsBefore;
                    uint32_t truthSteps  = g_truthP2Steps[i % slots];
                    static uint64_t s_stepLog = 0;
                    if (replaySteps != truthSteps && s_stepLog < 400) {
                        QLOG("[replay_step] MISMATCH win=%d target=%llu i=%llu (off=%lld) "
                             "truthP2Steps=%u replayP2Steps=%u P2act=%u P2E8=%08X",
                             win, (unsigned long long)f, (unsigned long long)i,
                             (long long)(i - startF), truthSteps, replaySteps,
                             (unsigned)rd32(0x626880 + 56), (unsigned)rd32(0x626880 + 232));
                        s_stepLog++;
                    }
                }
                if (nonNatural && (s_advLogged < 12)) {
                    QLOG("[advance_detect] phase=replay advanced=%d frameCtrBefore=%u frameCtrAfter=%u clockRec=%llu clockRep=%llu miss=%llu",
                         (int)(rd32(0x4AB778) != rFrameCtrBefore), rFrameCtrBefore, rd32(0x4AB778),
                         (unsigned long long)g_clockRecTotal, (unsigned long long)g_clockRepTotal, (unsigned long long)g_clockRepMiss);
                    s_advLogged++;
                }
            }
            g_replayActive = false;
            if (logReplay) QLOG("[replay] end frames=%d audioSuppressed=%llu",
                                win, (unsigned long long)(
                                    g_audioSuppressed - audioBefore));
            g_stPhase = 8;
            if (g_desyncDumpEnabled) g_rb.hashLive(&g_hashAfterReplay);
            readFighterDbg(g_replayDbg);
            if (g_doubleReplay && win == 1) {
                static uint64_t s_r2 = 0, s_diff = 0;
                if (g_wideTruth.empty()) { g_wideTruth.resize(kWideSize); g_wideReplay.resize(kWideSize); }
                auto oneReplay = [&](uint8_t* snapBefore) {
                    g_rb.loadFromSlot((size_t)(startF % slots));
                    if (g_replayMode == ReplayTickMode::Deterministic) restorePacingState((int)(startF % slots));
                    if (snapBefore) wideRead(snapBefore);
                    g_replayActive = true;
                    if (clockShim) clockBeginReplay((int)(startF % slots));
                    if (g_rngHooked) rngBeginReplay((int)(startF % slots), false);
                    if (audioHooksReady())
                        audioBeginReplay((int)(startF % slots));
                    if (g_pollHooked) ksBeginReplay((int)(startF % slots));
                    g_freezeGateCalls = 0;
                    g_pacingFreeze = true;
                    g_orig();
                    g_pacingFreeze = false;
                    if (g_pollHooked) ksOff();
                    if (audioHooksReady()) audioOff();
                    if (g_rngHooked) rngPhaseOff();
                    if (clockShim) clockOff();
                    g_replayActive = false;
                    return rd32(0x4A9938);
                };
                uint32_t seedA = oneReplay(g_wideTruth.data());
                uint32_t seedB = oneReplay(g_wideReplay.data());
                if (s_r2 < 16) {
                    QLOG("[replay2] rngG S_f=%08X a=%08X(+%d) b=%08X(+%d) %s",
                         g_rngSeedAtSave, seedA, lcgSteps(g_rngSeedAtSave, seedA),
                         seedB, lcgSteps(g_rngSeedAtSave, seedB),
                         (seedA == seedB) ? "SELF-CONSISTENT" : "<<NONDETERMINISTIC(non-saved feeder)");
                    s_r2++;
                }
                if (seedA != seedB && s_diff < 4) {
                    s_diff++;
                    const uint8_t* A = g_wideTruth.data(); const uint8_t* B = g_wideReplay.data();
                    QLOG("[feeder] frame=%llu divergent replay; non-saved bytes replay_a left behind:", (unsigned long long)f);
                    size_t rs = 0; bool in = false; int reported = 0, total = 0;
                    for (size_t i = 0; i <= kWideSize; ++i) {
                        bool d = (i < kWideSize) && (A[i] != B[i]);
                        if (d && !in) { in = true; rs = i; }
                        else if (!d && in) {
                            in = false; uintptr_t va = kWideBase + rs; size_t len = i - rs;
                            if (!inSaveSet(va)) {
                                total++;
                                if (reported < 24) {
                                    QLOG("[feeder]   @%08X size=%zu A=%02X%02X%02X%02X B=%02X%02X%02X%02X",
                                         (unsigned)va, len, A[rs], rs+1<kWideSize?A[rs+1]:0, rs+2<kWideSize?A[rs+2]:0, rs+3<kWideSize?A[rs+3]:0,
                                         B[rs], rs+1<kWideSize?B[rs+1]:0, rs+2<kWideSize?B[rs+2]:0, rs+3<kWideSize?B[rs+3]:0);
                                    reported++;
                                }
                            }
                        }
                    }
                    QLOG("[feeder] frame=%llu total non-saved differing ranges=%d", (unsigned long long)f, total);
                }
                readFighterDbg(g_replayDbg);
            }
            if (g_traceResidual) captureGate(g_gAfterReplay);
            if (g_wideWant) {
                wideRead(g_wideReplay.data());
                wideWrite(g_wideTruth.data());
                g_wideCaptured = true; g_wideWant = false;
            }
            bool gpOk = g_rb.compareWith(g_expected.data(), &g_stDiff, RollbackManager::Profile::Gameplay);
            g_rcOk = g_rb.compareWith(g_expected.data(), &g_rcDiff, RollbackManager::Profile::Render);
            g_fdOk = g_rb.compareWith(g_expected.data(), &g_fdDiff, RollbackManager::Profile::All);
            g_stResult = gpOk ? 0 : 1;
            if (g_p56Trace && win > 1 && !gpOk) {
                static uint64_t s_tl = 0;
                if (s_tl < 6) {
                    int firstDiv = -1;
                    for (int wi = 0; wi < win; ++wi)
                        if (g_winValid[wi] && g_winRep56[wi] != g_winTruth56[wi]) { firstDiv = wi; break; }
                    QLOG("[replay_window] K=%d target=%llu saveFrame=%llu firstDivWindowIdx=%d firstDivFrame=%lld "
                         "| P2 state+174=%02X anim+48=%u frame+52=%u +64=%u +160=%08X +204=%08X +72=%08X +76=%08X",
                         win, (unsigned long long)f, (unsigned long long)startF, firstDiv,
                         firstDiv < 0 ? -1LL : (long long)(startF + firstDiv),
                         (unsigned)(rd32(0x626880 + 172) >> 16 & 0xFF), rd32(0x626880 + 48), rd32(0x626880 + 52),
                         rd32(0x626880 + 64), rd32(0x626880 + 160), rd32(0x626880 + 204),
                         rd32(0x626880 + 72), rd32(0x626880 + 76));
                    for (int wi = 0; wi < win; ++wi) {
                        if (!g_winValid[wi]) {
                            QLOG("[replay_window]   i=%lld SKIP(no-advance)", (long long)(startF + wi));
                            continue;
                        }
                        uint32_t p16 = g_winT48[wi], p24 = g_winT52[wi], p64 = g_winT64[wi];
                        uint32_t p48 = g_p2_48_truth[(startF + wi) % slots];
                        uint32_t vb  = (p16 && pageReadable((void*)(uintptr_t)(p16 + 24*p48))) ? rd32(p16 + 24*p48) : 0;
                        uint32_t tfd = ((p64 + vb) << 6) + p24;
                        uint32_t tdur = pageReadable((void*)(uintptr_t)(tfd + 8)) ? rd32(tfd + 8) : 0xBADBAD;
                        QLOG("[replay_window]   i=%lld +56 t=%u r=%u%s | dur t=%u r=%u%s | simAdv t=%d r=%d%s | fdAddr t=%08X r=%08X%s",
                             (long long)(startF + wi),
                             g_winTruth56[wi], g_winRep56[wi], (g_winRep56[wi]!=g_winTruth56[wi])?"<<":"  ",
                             tdur, g_winRdur[wi], (g_winRdur[wi]!=tdur)?"<<":"  ",
                             (int)g_advCount[(startF + wi) % slots], (int)g_winRd[wi],
                             ((int)g_advCount[(startF+wi)%slots] != (int)g_winRd[wi]) ? "<<STEPDIFF" : "  ",
                             tfd, g_winRfd[wi], (g_winRfd[wi]!=tfd)?"<<":"  ");
                        char gs[256]; int gn = 0;
                        for (int gi = 0; gi < 8; ++gi)
                            if (g_gT[gi][(startF+wi)%slots] != g_gR[gi][wi])
                                gn += std::snprintf(gs+gn, sizeof(gs)-gn, " %s(t=%08X r=%08X)",
                                                    kGateName[gi], g_gT[gi][(startF+wi)%slots], g_gR[gi][wi]);
                        if (gn) QLOG("[countdown_gate]   i=%lld GATES-DIFFER:%s", (long long)(startF+wi), gs);
                    }
                    if (g_b600Trace) {
                        for (int ri = 0; ri < kB600Ring; ++ri) {
                            const B600Rec& e = g_b600Ring[ri];
                            if (e.frame >= (uint64_t)startF && e.frame <= f && (e.before != e.after || e.a2))
                                QLOG("[b600_call]   frame=%llu phase=%s a2=%d +56 %u->%u %s",
                                     (unsigned long long)e.frame, e.replay ? "replay" : "truth",
                                     e.a2, e.before, e.after, (e.before != e.after) ? "DECREMENTED" : "");
                        }
                    }
                    if (g_a0Trace) {
                        for (int ri = 0; ri < kA0Ring; ++ri) {
                            const A0Rec& e = g_a0Ring[ri];
                            if (e.frame >= (uint64_t)startF && e.frame <= f)
                                QLOG("[a0_call]   frame=%llu phase=%s +48=%u +64out=%u +56out(dur)=%u"
                                     " | f16(animTbl)=%08X base(*animRec)=%u",
                                     (unsigned long long)e.frame, e.replay ? "replay" : "truth",
                                     e.f48, e.f64out, e.f56out, e.f16, e.base);
                        }
                    }
                    s_tl++;
                }
            }
            if (g_feederRing && win > 1 && !gpOk && g_wideRing.size() == (size_t)slots
                && g_wideReplay.size() >= kWideSize) {
                static uint64_t s_fr = 0;
                if (s_fr < 8) {
                    const uint8_t* R = g_wideReplay.data();
                    const uint8_t* T = g_wideRing[startF % slots].data();
                    QLOG("[feeder_k] frame=%llu win=%d startF=%llu - non-saved bytes stale after deep restore:",
                         (unsigned long long)f, win, (unsigned long long)startF);
                    size_t rs = 0; bool in = false; int reported = 0, total = 0;
                    for (size_t i = 0; i <= kWideSize; ++i) {
                        bool d = (i < kWideSize) && (R[i] != T[i]);
                        if (d && !in) { in = true; rs = i; }
                        else if (!d && in) {
                            in = false; uintptr_t va = kWideBase + rs; size_t len = i - rs;
                            if (!inSaveSet(va)) {
                                total++;
                                if (reported < 32) {
                                    QLOG("[feeder_k]   @%08X size=%zu replay=%02X%02X%02X%02X truth=%02X%02X%02X%02X",
                                         (unsigned)va, len, R[rs], rs+1<kWideSize?R[rs+1]:0, rs+2<kWideSize?R[rs+2]:0, rs+3<kWideSize?R[rs+3]:0,
                                         T[rs], rs+1<kWideSize?T[rs+1]:0, rs+2<kWideSize?T[rs+2]:0, rs+3<kWideSize?T[rs+3]:0);
                                    reported++;
                                }
                            }
                        }
                    }
                    QLOG("[feeder_k] frame=%llu total non-saved differing ranges=%d", (unsigned long long)f, total);
                    s_fr++;
                }
            }
            if (g_doubleReplay && win > 1 && !gpOk) {
                static uint64_t s_kdr = 0;
                auto replayWindowHash = [&]() -> uint32_t {
                    g_rb.loadFromSlot((size_t)(startF % slots));
                    for (uint64_t j = startF; j <= f; ++j) {
                        if (!g_advSlot[j % slots]) continue;
                        if (g_replayMode == ReplayTickMode::Deterministic) {
                            restorePacingState((int)(j % slots)); g_freezeGateCalls = 0; g_pacingFreeze = true;
                        }
                        if (clockShim) clockBeginReplay((int)(j % slots));
                        if (g_rngHooked) rngBeginReplay((int)(j % slots), false);
                        if (audioHooksReady())
                            audioBeginReplay((int)(j % slots));
                        if (g_pollHooked) ksBeginReplay((int)(j % slots));
                        g_replayActive = true; g_orig(); g_replayActive = false; g_pacingFreeze = false;
                        if (g_pollHooked) ksOff();
                        if (audioHooksReady()) audioOff();
                        if (g_rngHooked) rngPhaseOff(); if (clockShim) clockOff();
                    }
                    uint32_t h = 2166136261u;
                    for (int k = 216; k < 216 + 36 * 30; k += 4) h = (h ^ rd32(0x626880 + k)) * 16777619u;
                    return h;
                };
                uint32_t hA = replayWindowHash();
                uint32_t hB = replayWindowHash();
                if (s_kdr < 30) {
                    QLOG("[dblreplay_k] win=%d target=%llu P2histA=%08X P2histB=%08X %s",
                         win, (unsigned long long)f, hA, hB,
                         (hA == hB) ? "REPLAY-SELF-CONSISTENT(deterministic feeder)"
                                    : "REPLAY-NONDETERMINISTIC(race/uncaptured-clock/uninit)");
                    s_kdr++;
                }
            }
            if (!gpOk && g_desyncDumpEnabled && g_stDiff.region) {
                uintptr_t addr = 0; size_t size = 0;
                for (const Region& rgn2 : g_rb.fixedRegions())
                    if (std::strcmp(rgn2.name, g_stDiff.region) == 0) { addr = rgn2.addr; size = rgn2.size; }
                for (const PtrRegion& pr2 : g_rb.pointerChildRegions())
                    if (std::strcmp(pr2.name, g_stDiff.region) == 0) { addr = rd32(pr2.fighterBase + pr2.ptrOffset); size = pr2.size; }
                for (int bi = 0; bi < 16; ++bi)
                    g_replayDiffBytes[bi] = (addr && g_stDiff.offsetInRegion + bi < size)
                        ? *reinterpret_cast<volatile uint8_t*>(addr + g_stDiff.offsetInRegion + bi) : 0;
                if (addr && size && size <= 0x8000) {
                    if (g_replayRegionSnap.size() < size) g_replayRegionSnap.resize(0x8000);
                    std::memcpy(g_replayRegionSnap.data(), reinterpret_cast<const void*>(addr), size);
                    g_replayRegionSnapAddr = addr; g_replayRegionSnapSize = size;
                }
                uint32_t bk = rd32(0x623500 + 4);
                g_bkP2Ptr = bk;
                if (bk) {
                    static const int of[] = {48,52,56,64,72,84,88,136,140,156,170,1456,1460,1468,1500};
                    for (int i = 0; i < 15; ++i) g_bkP2Fields[i] = rd32(bk + of[i]);
                }
            }
            g_rb.loadFrom(g_expected.data());
        } else {
            g_stResult = 2;
        }
        g_stPhase = 0;
        g_ovrActive = false;
        g_pacingMode = 0;
        g_pacingFreeze = false;
        clockOff();
        audioOff();
        ksOff();
        return true;
    } __except (g_stCrash = GetExceptionCode(), sehFilter(g_stCrash, GetExceptionInformation())) {
        g_ovrActive = false;
        g_pacingMode = 0;
        g_pacingFreeze = false;
        g_replayActive = false;
        clockOff();
        audioOff();
        ksOff();
        return false;
    }
}

static void dumpDesyncFull(uint64_t f, int win) {
    static int s_dumps = 0;
    constexpr int kDumpQuota = 8, kDumpBinQuota = 3;
    if (!g_desyncDumpEnabled || s_dumps >= kDumpQuota) return;
    s_dumps++;
    const char* rgn = g_stDiff.region ? g_stDiff.region : "?";
    QLOG("[desync] frame=%llu k=%d replayIndex=%llu phase=gameplay matchSub=%d slot=%zu",
         (unsigned long long)f, win, (unsigned long long)f, (int)rd32(kMatchSubAddr), g_lastSlot);
    size_t bufOff = 0; bool isChild = false;
    if (g_rb.bufferOffsetOf(rgn, &bufOff, &isChild)) {
        uintptr_t addr = 0; size_t size = 0; RegionRole role = RegionRole::GameplayRequired;
        for (const Region& r : g_rb.fixedRegions())
            if (std::strcmp(r.name, rgn) == 0) { addr = r.addr; size = r.size; role = r.role; }
        for (const PtrRegion& pr : g_rb.pointerChildRegions())
            if (std::strcmp(pr.name, rgn) == 0) {
                addr = rd32(pr.fighterBase + pr.ptrOffset); size = pr.size; role = pr.role;
            }
        size_t doff = g_stDiff.offsetInRegion;
        char tb[25] = {0}, rb[25] = {0};
        for (int i = 0; i < 8 && doff + i < size; ++i) {
            std::snprintf(tb + 2*i, 3, "%02X", g_expected[bufOff + doff + i]);
            std::snprintf(rb + 2*i, 3, "%02X", g_replayDiffBytes[i]);
        }
        QLOG("[desync_region] name=%s role=%s addr=0x%08X rva=0x%08X size=0x%zX "
             "firstDiffOffset=0x%zX firstDiffVA=0x%08X firstDiffRVA=0x%08X diffBytes=%zu "
             "truthBytes=%s replayBytes=%s",
             rgn,
             inGameplayHash(role) ? "DeterministicGameplay" : "RenderCosmetic",
             (unsigned)addr, (unsigned)(addr ? addr - 0x400000 : 0), size,
             doff, (unsigned)(addr ? addr + doff : 0),
             (unsigned)(addr ? addr + doff - 0x400000 : 0), g_stDiff.diffBytes, tb, rb);
        if (addr == g_replayRegionSnapAddr && size == g_replayRegionSnapSize
            && size <= g_replayRegionSnap.size()) {
            int n = 0;
            for (size_t o = 0; o + 4 <= size && n < 48; o += 4) {
                uint32_t t, r2;
                std::memcpy(&t,  &g_expected[bufOff + o], 4);
                std::memcpy(&r2, &g_replayRegionSnap[o],  4);
                if (t != r2) {
                    QLOG("[region_diff]   %s+0x%zX VA=%08X truth=%08X replay=%08X",
                         rgn, o, (unsigned)(addr + o), t, r2);
                    n++;
                }
            }
            QLOG("[region_diff] %s diffs shown=%d (cap 48)", rgn, n);
        }
    }
    {
        uint32_t bkTruth = rd32(0x623500 + 4);
        QLOG("[backup_p2] dword_623500[1] ptr truth=%08X replay=%08X %s",
             bkTruth, g_bkP2Ptr, (bkTruth == g_bkP2Ptr) ? "PTR-MATCH" : "PTR-DIVERGE(pointer-table swap!)");
        if (bkTruth) {
            static const int of[] = {48,52,56,64,72,84,88,136,140,156,170,1456,1460,1468,1500};
            for (int i = 0; i < 15; ++i) {
                uint32_t t = rd32(bkTruth + of[i]);
                if (t != g_bkP2Fields[i])
                    QLOG("[backup_p2]   backup+%d truth=%08X replay=%08X DIVERGE", of[i], t, g_bkP2Fields[i]);
            }
        }
    }
    auto hashAt = [](const std::vector<RollbackManager::RegionHash>& v, size_t i) -> uint64_t {
        return i < v.size() ? v[i].hash : 0;
    };
    size_t idx = 0;
    for (const Region& r : g_rb.fixedRegions()) {
        QLOG("[region_table] index=%zu name=%s role=%s addr=0x%08X rva=0x%08X size=0x%zX "
             "includedInSave=1 includedInGameplayHash=%d safeToRestore=%d "
             "hashBeforeTruth=%016llX hashAfterTruth=%016llX hashAfterRestore=%016llX hashAfterReplay=%016llX",
             idx, r.name,
             inGameplayHash(r.role) ? "DeterministicGameplay" : "RenderCosmetic",
             (unsigned)r.addr, (unsigned)(r.addr - 0x400000), r.size,
             (int)inGameplayHash(r.role), (int)safeToRestore(r.role),
             (unsigned long long)hashAt(g_hashBeforeTruth, idx),
             (unsigned long long)hashAt(g_hashAfterTruth, idx),
             (unsigned long long)hashAt(g_hashAfterRestore, idx),
             (unsigned long long)hashAt(g_hashAfterReplay, idx));
        idx++;
    }
    for (const PtrRegion& pr : g_rb.pointerChildRegions()) {
        uintptr_t pointee = rd32(pr.fighterBase + pr.ptrOffset);
        QLOG("[pointer_region] index=%zu name=%s pointerAddress=0x%08X pointerRVA=0x%08X "
             "pointee=0x%08X pointeeRVA=0x%08X size=0x%zX includedInGameplayHash=%d "
             "hashBeforeTruth=%016llX hashAfterTruth=%016llX hashAfterRestore=%016llX hashAfterReplay=%016llX",
             idx, pr.name,
             (unsigned)(pr.fighterBase + pr.ptrOffset), (unsigned)(pr.fighterBase + pr.ptrOffset - 0x400000),
             (unsigned)pointee, (unsigned)(pointee > 0x400000 ? pointee - 0x400000 : 0), pr.size,
             (int)inGameplayHash(pr.role),
             (unsigned long long)hashAt(g_hashBeforeTruth, idx),
             (unsigned long long)hashAt(g_hashAfterTruth, idx),
             (unsigned long long)hashAt(g_hashAfterRestore, idx),
             (unsigned long long)hashAt(g_hashAfterReplay, idx));
        idx++;
    }
    QLOG("[rng_desync] gameSeedBefore=%08X gameSeedTruth=%08X gameSeedReplay=%08X "
         "cosSeedTruth=%08X cosSeedReplay=%08X selectorTruth=%u selectorReplay=%u "
         "gameDrawsTruth=%u gameDrawsReplay=%u cosDrawsTruth=%u cosDrawsReplay=%u "
         "redirectedTruth=%u redirectedReplay=%u",
         g_rngSeedAtSave, g_truthDbg.rngG, g_replayDbg.rngG,
         g_truthDbg.rngC, g_replayDbg.rngC, g_truthDbg.rngSel, g_replayDbg.rngSel,
         g_rngTruthCnt.game, g_rngReplayCnt.game, g_rngTruthCnt.cos, g_rngReplayCnt.cos,
         g_rngTruthCnt.redirected, g_rngReplayCnt.redirected);
    QLOG("[input_desync] p1MaskTruth=0x%08X p1MaskReplay=0x%08X p2MaskTruth=0x%08X p2MaskReplay=0x%08X "
         "mergedTruth=0x%08X mergedReplay=0x%08X keyHashTruth=%08X keyHashReplay=%08X",
         g_truthDbg.inP1, g_replayDbg.inP1, g_truthDbg.inP2, g_replayDbg.inP2,
         g_truthDbg.inMerged, g_replayDbg.inMerged, g_truthDbg.keyHash, g_replayDbg.keyHash);
    QLOG("[tick_desync] advanceTruth=%d advanceReplay=%d frameCtrTruth=%u frameCtrReplay=%u "
         "roundTimerTruth=%u roundTimerReplay=%u activeArenaTruth=%08X activeArenaReplay=%08X "
         "selectorTruth=%u selectorReplay=%u",
         (int)g_lastTruthAdvanced, (int)g_lastReplayAdvanced,
         g_lastFrameCtrTruth, g_lastFrameCtrReplay,
         g_truthDbg.roundTimer, g_replayDbg.roundTimer,
         g_truthDbg.activeArena, g_replayDbg.activeArena,
         g_truthDbg.selArena, g_replayDbg.selArena);
    QLOG("[build_profile] runtimeProfile=%s executableIdentity=exact "
         "codeAddressResolution=explicit-profile-entries prologueVerified=1",
         runtime_profile::selectedProfileKey());
    if (g_wideDesync && g_wideTruth.size() >= kWideSize && g_wideReplay.size() >= kWideSize) {
        const uint8_t* t = g_wideTruth.data(); const uint8_t* rp = g_wideReplay.data();
        size_t rs = 0; bool in = false; int reported = 0, total = 0;
        for (size_t i = 0; i <= kWideSize; ++i) {
            bool d = (i < kWideSize) && (t[i] != rp[i]);
            if (d && !in) { in = true; rs = i; }
            else if (!d && in) {
                in = false;
                uintptr_t va = kWideBase + rs; size_t len = i - rs;
                if (!inSaveSet(va)) {
                    total++;
                    if (reported < 24) {
                        QLOG("[wide_desync]   NON-SAVED @%08X (rva=%08X) size=%zu T=%02X%02X%02X%02X R=%02X%02X%02X%02X",
                             (unsigned)va, (unsigned)(va - 0x400000), len,
                             t[rs], rs+1<kWideSize?t[rs+1]:0, rs+2<kWideSize?t[rs+2]:0, rs+3<kWideSize?t[rs+3]:0,
                             rp[rs], rs+1<kWideSize?rp[rs+1]:0, rs+2<kWideSize?rp[rs+2]:0, rs+3<kWideSize?rp[rs+3]:0);
                        reported++;
                    }
                }
            }
        }
        QLOG("[wide_desync] frame=%llu: %d non-saved differing ranges (candidate feeders)",
             (unsigned long long)f, total);
    }
    if (g_sessionDir[0]) {
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "%s\\desync_dump.jsonl", g_sessionDir);
        if (FILE* jf = std::fopen(path, "a")) {
            std::fprintf(jf,
                "{\"frame\":%llu,\"k\":%d,\"slot\":%zu,\"region\":\"%s\",\"offset\":%zu,"
                "\"diffBytes\":%zu,\"restoreDiff\":%u,"
                "\"gameSeedBefore\":\"%08X\",\"gameSeedTruth\":\"%08X\",\"gameSeedReplay\":\"%08X\","
                "\"gameDrawsTruth\":%u,\"gameDrawsReplay\":%u,"
                "\"advanceTruth\":%d,\"advanceReplay\":%d,"
                "\"roundTimerTruth\":%u,\"roundTimerReplay\":%u,"
                "\"camXTruth\":%d,\"camXReplay\":%d}\n",
                (unsigned long long)f, win, g_lastSlot, rgn, g_stDiff.offsetInRegion,
                g_stDiff.diffBytes, g_restoreDiffBytes,
                g_rngSeedAtSave, g_truthDbg.rngG, g_replayDbg.rngG,
                g_rngTruthCnt.game, g_rngReplayCnt.game,
                (int)g_lastTruthAdvanced, (int)g_lastReplayAdvanced,
                g_truthDbg.roundTimer, g_replayDbg.roundTimer,
                (int)g_truthDbg.camX, (int)g_replayDbg.camX);
            std::fclose(jf);
        }
        if (s_dumps <= kDumpBinQuota) {
            const uint8_t* slotBytes = g_rb.slotData(g_lastSlot);
            if (slotBytes) {
                std::snprintf(path, sizeof(path), "%s\\desync_frame_%llu_slot_%zu.bin",
                              g_sessionDir, (unsigned long long)f, g_lastSlot);
                if (FILE* bf = std::fopen(path, "wb")) {
                    std::fwrite(slotBytes, 1, g_rb.totalSize(), bf);
                    std::fclose(bf);
                    QLOG("[desync] slot binary written: %s (%zu bytes)", path, g_rb.totalSize());
                }
            }
        }
    }
}

void netFailClosed(uint32_t code, uint32_t detail, const char* why,
                   bool notifyPeer = true, bool graceful = false) {
    if (g_netHalted) return;
    if (!graceful) qoh99::Log::get().issue();
    netRematchTheaterRestore("fail-closed");
    g_netHalted = true;
    g_netHaltCode = code;
    std::snprintf(g_netHaltReason, sizeof(g_netHaltReason), "%s",
                  why ? why : "unknown netplay failure");
    const auto terminalCode =
        static_cast<qnetplay::wire::TerminalErrorCode>(code);
    const char* codeName =
        qnetplay::wire::terminalErrorCodeName(terminalCode);
    const uint32_t reasonId =
        qnetplay::wire::terminalReasonId(g_netHaltReason);
    if (graceful) {
        QLOG("[netplay] opponent left the match "
             "(code=%s(%u) detail=%u reasonId=%08X frame=%u): %s",
             codeName, code, detail, reasonId, g_netFrame,
             g_netHaltReason);
    } else {
        QLOG("[netplay] TERMINAL code=%s(%u) detail=%u "
             "reasonId=%08X frame=%u: %s",
             codeName, code, detail, reasonId, g_netFrame,
             g_netHaltReason);
    }
    qgame::netShowTerminalStatus(code, detail, g_netHaltReason, graceful);
    netObserveRollbackHealth("terminal", true);
    if (qgame::replay::replayRecorderV2Active()) {
        (void)qgame::replay::replayRecordAbnormalEndV2(
            qreplay::TerminalReason::Disconnected,
            graceful
                ? qreplay::ReplayCloseProvenanceV2::PeerOrderly
                : qreplay::ReplayCloseProvenanceV2::ProcessObserved);
    }

    if (graceful && g_sessionDir[0]
        && std::strncmp(g_netHaltReason, "rematch", 7) == 0) {
        char endPath[MAX_PATH];
        std::snprintf(endPath, sizeof(endPath),
                      "%s\\netplay_end.txt", g_sessionDir);
        if (FILE* f = std::fopen(endPath, "w")) {
            std::fprintf(f, "reason=%s\n", g_netHaltReason);
            std::fclose(f);
        }
    }

    if (!graceful && g_sessionDir[0]) {
        char failurePath[MAX_PATH];
        std::snprintf(failurePath, sizeof(failurePath),
                      "%s\\netplay_failure.txt", g_sessionDir);
        if (FILE* f = std::fopen(failurePath, "w")) {
            const qnetplay::RollbackDiagnostics diag =
                g_netSession
                    ? g_netSession->diagnostics()
                    : qnetplay::RollbackDiagnostics{};
            const qnetplay::RollbackStats stats =
                g_netSession
                    ? g_netSession->stats()
                    : qnetplay::RollbackStats{};
            qgame::NetTransportDiagnostics link{};
            (void)qgame::netGetTransportDiagnostics(link);
            const uint64_t observedUs =
                g_netRollbackHealth.totalObservedWallUs();
            const double sessionForwardFps =
                observedUs == 0
                    ? 0.0
                    : static_cast<double>(stats.visibleAdvances)
                        * 1000000.0
                        / static_cast<double>(observedUs);
            const double sessionSpeedPercent =
                sessionForwardFps * 100.0 / qgame::kQohTargetFps;
            std::fprintf(f,
                         "format_version=2\n"
                         "code=%u\n"
                         "code_name=%s\n"
                         "detail=%u\n"
                         "reason_id=%08X\n"
                         "frame=%u\n"
                         "scene=%u\n"
                         "match_sub=%d\n"
                         "notified_peer=%u\n"
                         "reason=%s\n",
                         code, codeName, detail, reasonId,
                         g_netFrame, rd32(kSceneStateAddr),
                         static_cast<int32_t>(rd32(kMatchSubAddr)),
                         notifyPeer ? 1u : 0u, g_netHaltReason);
            std::fprintf(
                f,
                "rollback_valid=%u\nfrontier=%u\n"
                "input_delay=%u\nmax_rollback=%u\n"
                "speculative_depth=%u\nnext_remote_expected=%u\n"
                "session_remote_newest_present=%u\n"
                "session_remote_newest=%u\n"
                "session_remote_hole=%u\nremote_hole_span=%u\n"
                "next_confirmed=%u\npending_rollback=%u\n"
                "pending_rollback_from=%u\nreplay_active=%u\n"
                "replay_from=%u\nreplay_next=%u\nreplay_until=%u\n"
                "active_stall=%s\nconsecutive_stall_polls=%u\n"
                "forward_commits=%llu\npredicted_commits=%llu\n"
                "confirmed_frames=%llu\nrollbacks=%llu\n"
                "rollback_frames=%llu\nreplayed_frames=%llu\n"
                "stall_polls=%llu\nstall_episodes=%llu\n"
                "max_consecutive_stall_polls=%u\n"
                "mismatching_remote_inputs=%llu\n"
                "remote_out_of_order_inputs=%llu\n"
                "remote_hole_prefix_advances=%llu\n"
                "max_remote_hole_span=%u\n"
                "max_speculative_frames=%u\n"
                "health_observed_us=%llu\n"
                "health_runnable_us=%llu\n"
                "health_blocked_us=%llu\n"
                "health_scheduler_nonstall_us=%llu\n"
                "health_scheduler_stalled_us=%llu\n"
                "health_adapter_hold_us=%llu\n"
                "health_current_adapter_hold=%s\n"
                "health_no_progress_us=%llu\n"
                "session_forward_fps=%.3f\n"
                "session_speed_percent=%.2f\n",
                diag.valid ? 1u : 0u, diag.frontier,
                static_cast<unsigned>(diag.inputDelayFrames),
                static_cast<unsigned>(diag.maxRollbackFrames),
                diag.speculativeFrames, diag.nextRemoteExpected,
                diag.hasRemoteNewestActual ? 1u : 0u,
                diag.remoteNewestActual,
                diag.hasRemoteHole ? 1u : 0u, diag.remoteHoleSpan,
                diag.nextConfirmedFrame,
                diag.pendingRollback ? 1u : 0u,
                diag.pendingRollbackFrom,
                diag.replayInProgress ? 1u : 0u,
                diag.replayFrom, diag.nextReplayFrame,
                diag.replayUntilExclusive,
                qnetplay::stallReasonName(diag.activeStallReason),
                diag.consecutiveStallPolls,
                static_cast<unsigned long long>(
                    stats.visibleAdvances),
                static_cast<unsigned long long>(
                    stats.predictedAdvances),
                static_cast<unsigned long long>(
                    stats.confirmedFrames),
                static_cast<unsigned long long>(
                    stats.rollbackCount),
                static_cast<unsigned long long>(
                    stats.rollbackFrames),
                static_cast<unsigned long long>(
                    stats.replayedFrames),
                static_cast<unsigned long long>(
                    stats.stallPolls),
                static_cast<unsigned long long>(
                    stats.stallEpisodes),
                stats.maxConsecutiveStallPolls,
                static_cast<unsigned long long>(
                    stats.mismatchingRemoteInputs),
                static_cast<unsigned long long>(
                    stats.remoteOutOfOrderInputs),
                static_cast<unsigned long long>(
                    stats.remoteHolePrefixAdvances),
                stats.maxRemoteHoleSpan,
                stats.maxSpeculativeFrames,
                static_cast<unsigned long long>(observedUs),
                static_cast<unsigned long long>(
                    g_netRollbackHealth.totalRunnableWallUs()),
                static_cast<unsigned long long>(
                    g_netRollbackHealth.totalBlockedWallUs()),
                static_cast<unsigned long long>(
                    g_netRollbackHealth.totalActiveWallUs()),
                static_cast<unsigned long long>(
                    g_netRollbackHealth.totalStalledWallUs()),
                static_cast<unsigned long long>(
                    g_netRollbackHealth.totalAdapterHoldWallUs()),
                qgame::adapterHoldReasonName(
                    g_netRollbackHealth.currentAdapterHoldReason()),
                static_cast<unsigned long long>(
                    g_netRollbackHealth.currentNoProgressAgeUs()),
                sessionForwardFps, sessionSpeedPercent);
            for (size_t i = 0;
                 i < stats.stallPollsByReason.size(); ++i) {
                std::fprintf(
                    f, "stall_reason_%s_polls=%llu\n"
                       "stall_reason_%s_episodes=%llu\n"
                       "health_stall_reason_%s_us=%llu\n",
                    qnetplay::stallReasonName(
                        static_cast<qnetplay::StallReason>(i)),
                    static_cast<unsigned long long>(
                        stats.stallPollsByReason[i]),
                    qnetplay::stallReasonName(
                        static_cast<qnetplay::StallReason>(i)),
                    static_cast<unsigned long long>(
                        stats.stallEpisodesByReason[i]),
                    qnetplay::stallReasonName(
                        static_cast<qnetplay::StallReason>(i)),
                    static_cast<unsigned long long>(
                        g_netRollbackHealth
                            .totalStalledWallUsByReason()[i]));
            }
            for (size_t i = 0;
                 i < stats.remoteInputResults.size(); ++i) {
                std::fprintf(
                    f, "remote_input_%s=%llu\n",
                    qnetplay::remoteInputResultName(
                        static_cast<qnetplay::RemoteInputResult>(i)),
                    static_cast<unsigned long long>(
                        stats.remoteInputResults[i]));
            }
            for (size_t i = 0;
                 i < g_netRollbackHealth
                         .totalAdapterHoldWallUsByReason().size();
                 ++i) {
                std::fprintf(
                    f, "health_adapter_hold_%s_us=%llu\n",
                    qgame::adapterHoldReasonName(
                        static_cast<qgame::AdapterHoldReason>(i)),
                    static_cast<unsigned long long>(
                        g_netRollbackHealth
                            .totalAdapterHoldWallUsByReason()[i]));
            }
            std::fprintf(
                f,
                "transport_available=%u\nestablished=%u\nalive=%u\n"
                "last_valid_receive_age_ms=%llu\n"
                "last_new_remote_input_age_ms=%llu\n"
                "core_remote_contiguous_present=%u\n"
                "core_remote_contiguous=%u\n"
                "core_remote_newest_present=%u\n"
                "core_remote_newest=%u\ncore_reorder_span=%u\n"
                "peer_ack_present=%u\npeer_ack=%u\n"
                "peer_frontier_present=%u\npeer_frontier=%u\n"
                "peer_frontier_age_ms=%llu\n"
                "ingress_accepted=%u\ningress_rejected=%u\n"
                "last_ingress_present=%u\n"
                "last_ingress=%s(%u)\nlast_ingress_age_ms=%llu\n"
                "ping_samples=%u\nping_mean_us=%u\nping_p95_us=%u\n"
                "ping_jitter_us=%u\nping_loss_permille=%u\n"
                "cross_domain_drops=%u\nwrong_source_drops=%u\n"
                "receive_errors=%u\nsend_failures=%u\n"
                "last_socket_error=%d\n"
                "game_thread_stage=%s\n"
                "game_thread_frame=%u\n"
                "game_thread_stage_age_ms=%llu\n",
                link.available ? 1u : 0u,
                link.core.established ? 1u : 0u,
                link.core.alive ? 1u : 0u,
                static_cast<unsigned long long>(
                    link.core.lastValidReceiveAgeMs),
                static_cast<unsigned long long>(
                    link.core.lastNewRemoteInputAgeMs),
                link.core.hasRemoteContiguous ? 1u : 0u,
                link.core.remoteContiguousThrough,
                link.core.hasRemoteNewest ? 1u : 0u,
                link.core.remoteNewest,
                link.core.remoteReorderSpan,
                link.core.hasPeerAck ? 1u : 0u,
                link.core.peerAckThrough,
                link.core.hasPeerSimFrontier ? 1u : 0u,
                link.core.peerSimFrontier,
                static_cast<unsigned long long>(
                    link.core.peerFrontierAgeMs),
                link.ingressAccepted, link.ingressRejected,
                link.hasLastIngress ? 1u : 0u,
                qnetplay::ingressResultName(link.lastIngress),
                static_cast<unsigned>(link.lastIngress),
                static_cast<unsigned long long>(
                    link.lastIngressAgeMs),
                link.hasPing ? link.ping.samples : 0u,
                link.hasPing ? link.ping.meanMicros : 0u,
                link.hasPing ? link.ping.p95Micros : 0u,
                link.hasPing ? link.ping.jitterMicros : 0u,
                link.hasPing ? link.ping.lossPerMille : 0u,
                link.crossDomainDrops, link.wrongSourceDrops,
                link.receiveErrors, link.sendFailures,
                link.lastSocketError,
                qgame::netGameThreadStageName(
                    link.gameThreadStage),
                link.gameThreadFrame,
                static_cast<unsigned long long>(
                    link.gameThreadStageAgeMs));
            for (size_t i = 0;
                 i < link.ingressByResult.size(); ++i) {
                if (link.ingressByResult[i] == 0)
                    continue;
                std::fprintf(
                    f, "ingress_%s=%u\n",
                    qnetplay::ingressResultName(
                        static_cast<qnetplay::IngressResult>(i)),
                    link.ingressByResult[i]);
            }
            std::fclose(f);
        }
    }
    if (notifyPeer)
        qgame::netSetLinkError(
            code, detail, g_netHaltReason);
    InterlockedExchange(&g_netGateOwnsTick, 0);
    g_netCanonicalPoll = false;
    g_netPhysicalPoll = false;
    g_replayActive = false;
    netClockOff();
    if (!g_netSpectatorArchiveSealed
        && ((g_spectatorServiceMode == 2u
             && qgame::spectator_publish::sidecarPublisherActive())
            || g_netSpectatorArchiveBegun)) {
        netSealSpectatorArchive(
            qnetplay::spectator::TerminalReason::ProtocolFailure);
    }
}

constexpr uint64_t kNetTerminalGraceMs = 4000;

int netPostTerminalHold() {
    static uint64_t s_lastNoteMs = 0;
    static uint64_t s_holdStartMs = 0;
    static bool s_closeRequested = false;
    const uint64_t nowMs = GetTickCount64();
    if (s_holdStartMs == 0) s_holdStartMs = nowMs;
    if (s_lastNoteMs == 0 || nowMs - s_lastNoteMs >= 10000u) {
        s_lastNoteMs = nowMs;
        QLOG("[netplay] post-terminal hold active frame=%u reason=%s",
             g_netFrame, g_netHaltReason);
    }
    static bool s_graceLogged = false;
    if (!s_closeRequested && nowMs - s_holdStartMs >= kNetTerminalGraceMs) {
        if (!s_graceLogged) {
            s_graceLogged = true;
            QLOG("[netplay] post-terminal grace elapsed (%llu ms); closing game cleanly",
                 (unsigned long long)kNetTerminalGraceMs);
        }
        if (qgame::netRequestGameWindowClose())
            s_closeRequested = true;
    }
    Sleep(16);
    return kNetHoldFrameRet;
}

void netWriteConfirmedDesync(
    const NetFrameMeta& local,
    const qgame::NetConfirmedHash& peer);

bool netCheckLink(bool requireAlive) {
    qgame::NetLinkError error{};
    if (qgame::netGetLinkError(error) && error.active) {
        if (error.fromPeer
            && error.code == kNetConfirmedDesync) {
            const NetFrameMeta& local =
                g_netMeta[error.detail % kNetMetaSlots];
            if (local.valid && local.frame == error.detail) {
                qgame::NetConfirmedHash remote{};
                remote.layoutGeneration = local.layoutGeneration;
                remote.epoch = local.epoch;
                remote.frame = error.detail;
                netWriteConfirmedDesync(local, remote);
            }
        }
        if (error.fromPeer && error.code == kNetPeerRequested) {
            netFailClosed(kNetPeerRequested, error.detail,
                          "opponent left the match", false, true);
            return false;
        }
        char why[qnetplay::wire::kTerminalReasonBytes] = {};
        if (error.reason[0] != '\0') {
            std::snprintf(why, sizeof(why), "%s", error.reason);
        } else {
            std::snprintf(
                why, sizeof(why),
                "peer link terminated: %s(%u), detail=%u",
                qnetplay::wire::terminalErrorCodeName(
                    static_cast<qnetplay::wire::TerminalErrorCode>(
                        error.code)),
                error.code, error.detail);
        }
        netFailClosed(error.code ? error.code : kNetTransportFailure,
                      error.detail, why, false);
        return false;
    }
    if (requireAlive && !qgame::netPeerAlive()) {
        netFailClosed(kNetTimeout, g_netFrame,
                      "peer liveness timeout; simulation halted");
        return false;
    }
    return true;
}

bool netFlushLocalSubmissions() {
    if (!g_netSession) return false;
    qnetplay::TaggedInput input{};
    while (g_netSession->popLocalSubmission(input)) {
        if (!qgame::netSubmitLocalInput(input.frame, input.value)) {
            netFailClosed(kNetInternalFailure, input.frame,
                          "transport rejected sequential local input");
            return false;
        }
    }
    return true;
}

void netObserveRunState(const qgame::WorkKind kind,
                        const qgame::HoldCause hold) {
    if (!g_netSession) return;

    qgame::RunStateInputs in{};
    in.kind = kind;
    in.hold = hold;
    const uint32_t frontier = g_netSession->currentFrame();
    const qnetplay::RollbackDiagnostics diag = g_netSession->diagnostics();
    in.missingRemoteDepth =
        qnet::frameAfter(frontier, diag.nextRemoteExpected)
            ? qnet::forwardDistance(diag.nextRemoteExpected, frontier) : 0u;
    in.localMaxRollback = g_netRollback;
    in.recoverableDebtFrames = g_netCadenceDebt.frames();
    in.externalSuspension = false;
    in.sessionTerminated = false;

    const qgame::NetplayRunState next = qgame::classifyRunState(in);
    const uint64_t nowUs = netRealQpcUs();
    const bool changed = next != g_netRunState;
    if (!changed && g_netRunStateSampledUs != 0
        && nowUs - g_netRunStateSampledUs < 16500ull) {
        return;
    }

    constexpr uint64_t kMaxAttributionUs = 100000;
    if (g_netRunStateEnteredUs != 0 && nowUs > g_netRunStateEnteredUs) {
        const uint64_t delta = nowUs - g_netRunStateEnteredUs;
        if (delta <= kMaxAttributionUs) {
            g_netRunStateUs[static_cast<size_t>(g_netRunState)] += delta;
        } else {
            ++g_netRunStateDiscontinuities;
        }
    }
    g_netRunStateEnteredUs = nowUs;
    g_netRunStateSampledUs = nowUs;

    if (changed) {
        ++g_netRunStateTransitions;
        if (qgame::runStateIsNetworkEmergency(next)
            || qgame::runStateIsNetworkEmergency(g_netRunState)) {
            QLOG("[run-state] %s -> %s frame=%u missingDepth=%u R=%u debt=%u",
                 qgame::runStateName(g_netRunState), qgame::runStateName(next),
                 frontier, in.missingRemoteDepth, in.localMaxRollback,
                 in.recoverableDebtFrames);
            static uint64_t lastBudgetLogUs = 0;
            if (qgame::runStateIsNetworkEmergency(next)
                && (lastBudgetLogUs == 0
                    || nowUs - lastBudgetLogUs >= 2000000ull)) {
                lastBudgetLogUs = nowUs;
                const uint32_t rttMs =
                    qgame::netPeerHasRtt() ? qgame::netPeerRttMs() : 0u;
                const int32_t transit = static_cast<int32_t>(
                    ((rttMs / 2u) * 6060ull + 50000ull) / 100000ull);
                int32_t lead = 0;
                if (qgame::netPeerHasSimFrontier()) {
                    const uint32_t staleFrames = static_cast<uint32_t>(
                        ((qgame::netPeerFrontierAgeMs() + rttMs / 2u)
                         * 6060ull + 50000ull) / 100000ull);
                    lead = qnet::signedLead(
                        frontier,
                        qgame::netPeerSimFrontier() + staleFrames);
                }
                const int32_t budget =
                    static_cast<int32_t>(in.localMaxRollback);
                const int32_t headroom = budget - transit
                                       - (lead > 0 ? lead : 0);
                QLOG("[stall-cause] window=%d spent: transit=%d (rtt/2) "
                     "lead=%d -> headroom=%d%s slewPpm=%d frontendDelay=%u",
                     budget, transit, lead, headroom,
                     headroom <= 0
                         ? "  EXHAUSTED: every frame now waits for the exact "
                           "remote input - this is the freeze, and the lead is "
                           "what spent the budget, not the ping"
                         : "",
                     g_netPaceSlewPpm, g_netFrontendDelay);
            }
        }
        g_netRunState = next;
    }
    g_netHoldLedger.observe(next, nowUs, frontier, in.localMaxRollback);

    qgame::netPublishLocalPressure(
        g_netSession->localInputProducedThrough(),
        diag.nextConfirmedFrame,
        netSmoothedPredictionDepth(in.missingRemoteDepth),
        static_cast<uint8_t>(in.recoverableDebtFrames > 0xFFu
                                 ? 0xFFu : in.recoverableDebtFrames),
        static_cast<uint8_t>(next),
        g_netHoldLedger.generation());
}

static uint64_t g_netProducerLastUs = 0;

// Never call g_fullInputPollFn() off-cadence: it writes native key-state buffers that native edge detection reads.
static bool netProducerFreshSample(uint16_t& out) {
    if (!g_inputConfigPresent) return false;
    if (g_netAutomatchTest) return false;
    if (g_rematchPrompt.advancing()) return false;
    if (g_netDirectBootInputGate.awaitingNeutral()) return false;
    if (qgame::directBootNetworkActive()) return false;

    bool neutralBeforeSocd = true;
    const int sample = netManagedPhysicalCapture(neutralBeforeSocd);
    const uint16_t packed = packLogical(sample);
    if (!qinput::isCanonicalFightInput(packed)) return false;
    out = packed;
    return true;
}

bool netProduceLocalInputWhileStalled() {
    if (netInputProducerDisabled()) return false;
    if (!g_netSession) return false;
    if (g_sessionCaptureFrozen) return false;
    if (!qgame::inputProductionPermitted(
            rollbackEligible(),
            netLifecycleTransitionRequiresExactInput(),
            g_rematchPrompt.phase() != qnetplay::RematchPromptPhase::Idle,
            g_netSpectatorMode || g_replayActive)) {
        return false;
    }
    if (!g_netSession->hasCapturedWord()) return false;

    const uint64_t nowUs = netRealQpcUs();
    constexpr uint64_t kFramePeriodUs = 16500;
    if (g_netProducerLastUs != 0
        && nowUs - g_netProducerLastUs < kFramePeriodUs) {
        return false;
    }

    uint16_t word = g_netSession->lastCapturedWord();
    const bool fresh = netProducerFreshSample(word);

    const uint32_t lead = netProducerMaxLeadFrames();
    if (!g_netSession->produceLocalInputAhead(word, lead)) {
        return false;
    }
    if (fresh) ++g_netProducerFreshSamples; else ++g_netProducerHeldRepeats;
    g_netProducerLastUs = nowUs;
    ++g_netProducerAheadFrames;
    const uint32_t nowLead = g_netSession->localInputProducedThrough()
        - (g_netSession->currentFrame()
           + static_cast<uint32_t>(
                 g_netSession->config().inputDelayFrames));
    if (nowLead > g_netProducerAheadMaxLead)
        g_netProducerAheadMaxLead = nowLead;
    (void)netFlushLocalSubmissions();
    return true;
}

bool netDrainRemoteInputs() {
    if (!g_netSession) return false;
    uint16_t input = 0;
    while (qgame::netGetRemoteExact(g_netRemoteIngest, input)) {
        const qnetplay::RemoteInputResult result =
            g_netSession->receiveRemoteInput(g_netRemoteIngest, input);
        if (result != qnetplay::RemoteInputResult::Inserted
            && result != qnetplay::RemoteInputResult::Duplicate) {
            const uint32_t badFrame = g_netRemoteIngest;
            const bool protocolViolation =
                result == qnetplay::RemoteInputResult::Conflict
                || result == qnetplay::RemoteInputResult::InvalidValue;
            char why[160];
            std::snprintf(
                why, sizeof(why),
                "remote input rejected: %s(%u)%s",
                qnetplay::remoteInputResultName(result),
                static_cast<unsigned>(result),
                result == qnetplay::RemoteInputResult::InvalidValue
                    ? " (unknown action bits)" : "");
            netFailClosed(
                protocolViolation ? kNetProtocolViolation
                                  : kNetInternalFailure,
                badFrame, why);
            return false;
        }
        ++g_netRemoteIngest;
    }
    return true;
}

inline bool netRemoteActualThrough(uint32_t frame) {
    uint16_t value = 0;
    return g_netSession && g_netSession->getRemoteActual(frame, value)
        && g_netSession->hasRemoteContiguousThrough()
        && g_netSession->remoteContiguousThrough() >= frame;
}

constexpr uintptr_t kReplayResultCounterAddr = 0x6236C8u;
constexpr uintptr_t kReplayLogicalStageAddr = 0x4ECB1Cu;
constexpr uintptr_t kReplayAppliedStageAddr = 0x87D980u;
// These three VAs are stamped into the replay record: changing a tag is a wire change, not a refactor.
constexpr uintptr_t kReplayRoundActiveSite = static_cast<uintptr_t>(
    generated_model::nativeSymbolNovemberVa(
        generated_model::NativeSymbolId::SceneMatchDispatch));
constexpr uintptr_t kReplayRoundOutcomeSite = static_cast<uintptr_t>(
    generated_model::nativeSymbolNovemberVa(
        generated_model::NativeSymbolId::ReplayRoundOutcomeSite));
constexpr uintptr_t kReplayFinalResultSite = static_cast<uintptr_t>(
    generated_model::nativeSymbolNovemberVa(
        generated_model::NativeSymbolId::ReplayFinalResultSite));
static_assert(kReplayRoundActiveSite == 0x428430u
                  && kReplayRoundOutcomeSite == 0x40EC60u
                  && kReplayFinalResultSite == 0x4047C0u,
              "replay provenance tags are on the wire; their values must not "
              "move when their source table does");
constexpr uintptr_t kReplaySelectorCursorAddr = 0x870954u;
constexpr uintptr_t kReplaySelectorAlternateAddr = 0x87095Cu;
constexpr uintptr_t kReplaySelectorConfirmAddr = 0x8716D0u;
constexpr uintptr_t kReplaySelectorLaunchStateAddr = 0x8716E8u;
// dword_6236F0[slot] latches a side's knockout in sub_4047C0; only the main-menu VS confirm (sub_486CB0 case 0) clears it, so it survives rematches.
constexpr uintptr_t kReplaySessionKnockoutLatchAddr = 0x6236F0u;
constexpr uintptr_t kReplaySelectorStride = 0x460u;
constexpr std::array<uintptr_t, qreplay::kFighterSlots>
    kReplayFinalRosterAddrs{{
        0x623EAEu, 0x62692Eu, 0x4ECBEEu, 0x617ACEu
    }};

std::array<qreplay::FighterIdentityV2, qreplay::kFighterSlots>
netReplayRosterV2() {
    std::array<
        qreplay::FighterIdentityV2,
        qreplay::kFighterSlots> roster{};
    const qreplay::CharacterCatalog& catalog =
        qreplay::CharacterCatalog::forSeBuild();
    for (size_t slot = 0u; slot < roster.size(); ++slot) {
        qreplay::FighterIdentityV2 identity{};
        // Byte 0 is the final resolved native character identity; byte 1 is the palette nibble (index << 4), not the character-select cursor palette.
        const uint8_t character = rd8(kReplayFinalRosterAddrs[slot]);
        const uint8_t paletteRaw = rd8(kReplayFinalRosterAddrs[slot] + 1u);
        if (slot < 2u && catalog.isSelectable(character)
            && (paletteRaw & 0x0Fu) == 0u
            && (paletteRaw >> 4u) <= qreplay::kMaxPaletteIndex) {
            identity.nativeCharacterId = character;
            identity.paletteIndex =
                static_cast<uint8_t>(paletteRaw >> 4u);
        } else {
            identity.nativeCharacterId =
                qreplay::kInactiveCharacterId;
            identity.paletteIndex = 0u;
        }
        roster[slot] = identity;
    }
    roster[2].nativeCharacterId = qreplay::kInactiveCharacterId;
    roster[2].paletteIndex = 0u;
    roster[3].nativeCharacterId = qreplay::kInactiveCharacterId;
    roster[3].paletteIndex = 0u;
    return roster;
}

std::array<qreplay::SelectorProofV2, qreplay::kFighterSlots>
netReplaySelectorProofV2() {
    std::array<
        qreplay::SelectorProofV2,
        qreplay::kFighterSlots> proofs{};
    for (size_t slot = 0u; slot < 2u; ++slot) {
        qreplay::SelectorProofV2& proof = proofs[slot];
        proof.cursorCell = static_cast<uint8_t>(rd32(
            kReplaySelectorCursorAddr + kReplaySelectorStride * slot));
        proof.alternateFlag = static_cast<uint8_t>(rd32(
            kReplaySelectorAlternateAddr + kReplaySelectorStride * slot));
        proof.confirmState = static_cast<uint8_t>(rd32(
            kReplaySelectorConfirmAddr + sizeof(uint32_t) * slot));
    }
    return proofs;
}

qreplay::StageIdentityV2 netReplayStageV2() {
    qreplay::StageIdentityV2 stage{};
    const uint32_t logical = rd32(kReplayLogicalStageAddr);
    if (!qreplay::StageCatalogV2::forSeBuild().makeIdentity(
            logical, stage)) {
        return qreplay::StageIdentityV2{};
    }
    if (rd32(kReplayAppliedStageAddr) != stage.nativeStageId)
        return qreplay::StageIdentityV2{};
    return stage;
}

qreplay::GameLifecycleV2 netReplayLifecycleV2(
    const uint32_t layoutGeneration) {
    qreplay::GameLifecycleV2 lifecycle{};
    lifecycle.scene = rd32(kSceneStateAddr);
    lifecycle.requestedScene = rd32(kSceneNextAddr);
    lifecycle.matchSub =
        static_cast<int32_t>(rd32(kMatchSubAddr));
    lifecycle.sceneThreeState = rd32(addr::kScene3Substate);
    lifecycle.roundState = rd32(kRoundResultStateAddr);
    lifecycle.roundCounter = rd32(addr::kRoundCounter);
    lifecycle.resultCounter = rd32(kReplayResultCounterAddr);
    lifecycle.roundTallies[0] =
        static_cast<uint8_t>(rd32(kRoundWinsP1Addr));
    lifecycle.roundTallies[1] =
        static_cast<uint8_t>(rd32(kRoundWinsP2Addr));
    // dword_6236A8 resets to zero at every round intro and zero is also the P1 result, so it is a verdict only after the native matchSub 1 -> 2 commit.
    if (lifecycle.scene == 2u && lifecycle.matchSub >= 2
        && lifecycle.roundCounter == 5u) {
        const uint32_t nativeWinner = rd32(kMatchWinnerSlotAddr);
        const int32_t derivedWinner =
            lifecycle.roundTallies[0] > lifecycle.roundTallies[1]
                ? 0 : 1;
        if (nativeWinner <= 1u
            && static_cast<int32_t>(nativeWinner) == derivedWinner) {
            lifecycle.winnerSlot = derivedWinner;
        }
    }
    lifecycle.routeEpoch = layoutGeneration;
    lifecycle.rosterEpoch = layoutGeneration;
    lifecycle.layoutGeneration = layoutGeneration;
    return lifecycle;
}

qreplay::RoundTransitionProofV2 netReplayRoundTransitionV2(
    const qreplay::GameLifecycleV2& before,
    qreplay::GameLifecycleV2& after) {
    qreplay::RoundTransitionProofV2 proof{};
    proof.talliesBefore = before.roundTallies;
    proof.talliesAfter = after.roundTallies;
    proof.roundStateBefore = before.roundState;
    proof.roundStateAfter = after.roundState;
    proof.roundCounterBefore = before.roundCounter;
    proof.roundCounterAfter = after.roundCounter;
    proof.resultCounterBefore = before.resultCounter;
    proof.resultCounterAfter = after.resultCounter;

    const bool initialRoundBecameActive =
        before.scene == 2u && before.matchSub == -1
        && after.scene == 2u && after.matchSub == 0
        && after.roundCounter == 0u
        && after.roundTallies == std::array<uint8_t, 2>{};
    const bool laterRoundBecameActive =
        before.scene == 2u && before.matchSub == 0
        && after.scene == 2u && after.matchSub == 0
        && before.roundCounter < UINT32_MAX
        && after.roundCounter == before.roundCounter + 1u;
    if (initialRoundBecameActive
        || (laterRoundBecameActive
            && before.roundTallies == after.roundTallies)) {
        proof.flags = qreplay::RoundBecameActiveV2;
        proof.transitionSiteId =
            static_cast<uint32_t>(kReplayRoundActiveSite);
        proof.roundOrdinal = static_cast<uint8_t>(
            (std::min)(after.roundCounter + 1u, 255u));
        return proof;
    }

    int32_t tallyWinner = -1;
    if (before.roundTallies[0] != 0xFFu
        && after.roundTallies[0]
            == static_cast<uint8_t>(before.roundTallies[0] + 1u)
        && after.roundTallies[1] == before.roundTallies[1]) {
        tallyWinner = 0;
    } else if (before.roundTallies[1] != 0xFFu
               && after.roundTallies[1]
                   == static_cast<uint8_t>(
                       before.roundTallies[1] + 1u)
               && after.roundTallies[0] == before.roundTallies[0]) {
        tallyWinner = 1;
    }
    if (tallyWinner >= 0
        && before.scene == 2u && before.matchSub == 0
        && after.scene == 2u && after.matchSub == 0) {
        proof.flags = qreplay::RoundOutcomeCommittedV2;
        proof.transitionSiteId =
            static_cast<uint32_t>(kReplayRoundOutcomeSite);
        proof.roundOrdinal = static_cast<uint8_t>(
            (std::min)(before.roundCounter + 1u, 255u));
        proof.winnerSlot = tallyWinner;
        proof.winningSideMask =
            static_cast<uint8_t>(1u << tallyWinner);
        return proof;
    }

    if (before.scene == 2u && before.matchSub == 1
        && after.scene == 2u && after.matchSub == 2
        && after.roundCounter == 5u) {
        proof.flags = qreplay::FinalGameResultCommittedV2;
        proof.transitionSiteId =
            static_cast<uint32_t>(kReplayFinalResultSite);
        const uint32_t rounds =
            static_cast<uint32_t>(after.roundTallies[0])
            + static_cast<uint32_t>(after.roundTallies[1]);
        proof.roundOrdinal = static_cast<uint8_t>(
            (std::min)((std::max)(rounds, 1u), 255u));
        proof.winnerSlot = after.winnerSlot;
        if (proof.winnerSlot >= 0 && proof.winnerSlot < 2) {
            proof.winningSideMask =
                static_cast<uint8_t>(1u << proof.winnerSlot);
        }
        return proof;
    }
    return proof;
}

void netReplayMaybeStageFreshGameV2(const uint32_t sourceFrame) {
    if (!qgame::replay::replayRecorderV2Active()
        || sourceFrame == 0u
        || rd32(kSceneStateAddr) != 2u
        || static_cast<int32_t>(rd32(kMatchSubAddr)) != -1) {
        return;
    }
    const NetFrameMeta& launch =
        g_netMeta[(sourceFrame - 1u) % kNetMetaSlots];
    const bool launchEpochMatches = launch.epoch == g_netEpoch;
    const bool launchPrecedesRotatedRematchEpoch =
        g_netGameplaySegmentSeen && !g_rbReady
        && !g_netGameplayRefreshPending
        && launch.epoch != UINT32_MAX
        && launch.epoch + 1u == g_netEpoch;
    const bool instantRematchLaunch = g_netInstantRematchLaunch
        && launch.valid && launch.frame == sourceFrame - 1u
        && launch.frame >= g_netInstantRematchBoundary
        && launch.replayLifecycleBefore.scene == 2u
        && (launchEpochMatches || launch.epoch + 1u == g_netEpoch);
    if (instantRematchLaunch) {
        g_netInstantRematchLaunch = false;
        QLOG("[replay.record-v2] instant-rematch launch proof accepted "
             "frame=%u boundary=%u", sourceFrame,
             g_netInstantRematchBoundary);
    }
    if (!instantRematchLaunch
        && (!launch.valid || launch.frame != sourceFrame - 1u
            || (!launchEpochMatches
                && !launchPrecedesRotatedRematchEpoch)
            || launch.replayLifecycleBefore.scene != 3u)) {
        QLOG("[replay.record-v2] fresh-game boundary rejected frame=%u "
             "reason=missing-launch-proof previous=[valid=%u frame=%u "
             "epoch=%u currentEpoch=%u before=%u/%d after=%u/%d]",
             sourceFrame, launch.valid ? 1u : 0u, launch.frame,
             launch.epoch, g_netEpoch,
             launch.replayLifecycleBefore.scene,
             launch.replayLifecycleBefore.matchSub,
             launch.replayLifecycleAfter.scene,
             launch.replayLifecycleAfter.matchSub);
        return;
    }

    qgame::NetReadyState ready{};
    if (!netReadActiveMatchConfig(ready)) {
        QLOG("[replay.record-v2] fresh-game boundary rejected frame=%u "
             "reason=active-match-config-unavailable", sourceFrame);
        return;
    }
    qgame::replay::ReplayFreshGameCaptureV2 capture{};
    capture.launchSourceFrame = sourceFrame - 1u;
    capture.layoutGeneration = g_netEpoch;
    capture.configuration.timerMode = ready.timerMode;
    capture.configuration.aiDifficulty = ready.aiDifficulty;
    capture.configuration.lifeMultiplier =
        ready.lifeMultiplier;
    capture.configuration.roundsToWinMinusOne =
        ready.roundsToWinMinusOne;
    capture.rng.gameplay = rd32(0x4A9938u);
    capture.rng.cosmetic = rd32(0x4A993Cu);
    capture.rng.selector = rd32(0x4ABB24u);
    uint16_t liveX87 = 0u;
    __asm { fnstcw liveX87 }
    capture.fpu.x87ControlWord = liveX87;
    capture.fpu.mxcsr = _mm_getcsr() & ~0x3Fu;
    capture.initialPhase = netPhaseTag();
    capture.bootstrapFingerprint =
        netBootstrapFingerprint();
    capture.roster = netReplayRosterV2();
    if (instantRematchLaunch) {
        if (!g_netStagedSelectorProofValid) {
            QLOG("[replay.record-v2] fresh-game boundary rejected frame=%u "
                 "reason=instant-launch-selector-proof-unavailable",
                 sourceFrame);
            return;
        }
        capture.selectorProof = g_netStagedSelectorProof;
    } else {
        capture.selectorProof = launch.replaySelectorProofBefore;
    }
    const bool staged =
        qgame::replay::replayRecordStageFreshGameV2(capture);
    if (staged) {
        g_netStagedSelectorProof = capture.selectorProof;
        g_netStagedSelectorProofValid = true;
    }
    QLOG("[replay.record-v2] fresh-game boundary frame=%u launch=%u "
         "previous=[valid=%u frame=%u epoch=%u scene=%u sub=%d] "
         "roster=[%u/%u,%u/%u] selector=[%u/%u/%u,%u/%u/%u] "
         "config=[%u,%u,%u,%u] staged=%u",
         sourceFrame, capture.launchSourceFrame,
         launch.valid ? 1u : 0u, launch.frame, launch.epoch,
         launch.replayLifecycleBefore.scene,
         launch.replayLifecycleBefore.matchSub,
         capture.roster[0].nativeCharacterId,
         capture.roster[0].paletteIndex,
         capture.roster[1].nativeCharacterId,
         capture.roster[1].paletteIndex,
         capture.selectorProof[0].cursorCell,
         capture.selectorProof[0].alternateFlag,
         capture.selectorProof[0].confirmState,
         capture.selectorProof[1].cursorCell,
         capture.selectorProof[1].alternateFlag,
         capture.selectorProof[1].confirmState,
         capture.configuration.timerMode,
         capture.configuration.aiDifficulty,
         capture.configuration.lifeMultiplier,
         capture.configuration.roundsToWinMinusOne,
         staged ? 1u : 0u);
}

constexpr uint32_t kOfflineLayoutGeneration = 1u;

void netOfflineRecorderCommitV2() {
    if (!qgame::replay::replayRecorderV2Active()) return;
    if (g_netActive) return;

    static bool s_have = false;
    static uint32_t s_lastCounter = 0u;
    static uint32_t s_sourceFrame = 0u;
    static uint64_t s_preHash = 0u;
    static qreplay::GameLifecycleV2 s_before{};
    static bool s_staged = false;
    static bool s_opened = false;
    static qgame::replay::ReplayFreshGameCaptureV2 s_pendingCapture{};
    static bool s_havePendingCapture = false;
    static bool s_haveLive = false;
    static uint32_t s_liveScene = 0u;
    static int32_t s_liveSub = 0;
    static std::array<qreplay::SelectorProofV2, qreplay::kFighterSlots>
        s_charSelectProof{};
    static bool s_charSelectProofValid = false;

    const uint32_t liveScene = rd32(kSceneStateAddr);
    const int32_t liveSub = static_cast<int32_t>(rd32(kMatchSubAddr));

    if (liveScene == 3u) {
        s_charSelectProof = netReplaySelectorProofV2();
        s_charSelectProofValid = true;
    }
    if (liveScene != 2u) {
        s_staged = false;
        s_opened = false;
        s_havePendingCapture = false;
    }
    const bool enteredSetup = liveScene == 2u && liveSub == -1
        && !(s_haveLive && s_liveScene == 2u && s_liveSub == -1);

    static uint32_t s_traced = 0u;
    if (s_traced < 96u && s_haveLive
        && (liveScene != s_liveScene || liveSub != s_liveSub)) {
        ++s_traced;
        QLOG("[replay.record-v2] offline lifecycle %u/%d -> %u/%d "
             "round=%u tallies=%u/%u result=%u staged=%d frame=%u",
             s_liveScene, s_liveSub, liveScene, liveSub,
             rd32(addr::kRoundCounter),
             rd32(kRoundWinsP1Addr), rd32(kRoundWinsP2Addr),
             rd32(kReplayResultCounterAddr),
             s_staged ? 1 : 0, s_sourceFrame);
    }

    s_haveLive = true;
    s_liveScene = liveScene;
    s_liveSub = liveSub;
    if (enteredSetup) s_staged = false;

    if (enteredSetup && !s_havePendingCapture) {
        qgame::NetReadyState ready{};
        if (!netReadActiveMatchConfig(ready)) {
            QLOG("[replay.record-v2] offline fresh-game rejected frame=%u "
                 "reason=active-match-config-unavailable", s_sourceFrame);
        } else if (!s_charSelectProofValid) {
            QLOG("[replay.record-v2] offline fresh-game rejected "
                 "frame=%u reason=selector-proof-unavailable",
                 s_sourceFrame);
        } else {
            qgame::replay::ReplayFreshGameCaptureV2 capture{};
            capture.layoutGeneration = kOfflineLayoutGeneration;
            capture.configuration.timerMode = ready.timerMode;
            capture.configuration.aiDifficulty = ready.aiDifficulty;
            capture.configuration.lifeMultiplier = ready.lifeMultiplier;
            capture.configuration.roundsToWinMinusOne =
                ready.roundsToWinMinusOne;
            capture.rng.gameplay = rd32(0x4A9938u);
            capture.rng.cosmetic = rd32(0x4A993Cu);
            capture.rng.selector = rd32(0x4ABB24u);
            uint16_t liveX87 = 0u;
            __asm { fnstcw liveX87 }
            capture.fpu.x87ControlWord = liveX87;
            capture.fpu.mxcsr = _mm_getcsr() & ~0x3Fu;
            capture.initialPhase = netPhaseTag();
            capture.bootstrapFingerprint = netBootstrapFingerprint();
            capture.roster = netReplayRosterV2();
            capture.selectorProof = s_charSelectProof;
            s_pendingCapture = capture;
            s_havePendingCapture = true;
        }
    }

    if (s_havePendingCapture && !s_staged && s_have
        && s_before.scene == 2u && s_before.matchSub == -1) {
        s_pendingCapture.launchSourceFrame = s_sourceFrame;
        s_staged = qgame::replay::replayRecordStageFreshGameV2(
            s_pendingCapture);
        s_havePendingCapture = false;
        QLOG("[replay.record-v2] offline fresh-game staged=%d state=%u "
             "launch=%u firstTo=%u epoch=%u bootstrap=%016llX "
             "x87=%04X mxcsr=%08X chars=[%u,%u]",
             s_staged ? 1 : 0,
             static_cast<unsigned>(qgame::replay::replayRecordStateV2()),
             s_pendingCapture.launchSourceFrame,
             s_pendingCapture.configuration.roundsToWinMinusOne + 1u,
             s_pendingCapture.layoutGeneration,
             static_cast<unsigned long long>(
                 s_pendingCapture.bootstrapFingerprint),
             s_pendingCapture.fpu.x87ControlWord,
             s_pendingCapture.fpu.mxcsr,
             s_pendingCapture.roster[0].nativeCharacterId,
             s_pendingCapture.roster[1].nativeCharacterId);
    }

    const uint32_t counter = rd32(0x4AB778);
    if (!s_have) {
        s_have = true;
        s_lastCounter = counter;
        s_preHash = netSpectatorLiveStateHash();
        s_before = netReplayLifecycleV2(kOfflineLayoutGeneration);
        return;
    }

    qreplay::GameLifecycleV2 after =
        netReplayLifecycleV2(kOfflineLayoutGeneration);

    const bool stepped = counter != s_lastCounter;
    static uint32_t s_unsteppedRun = 0u;
    if (stepped) {
        s_unsteppedRun = 0u;
    } else if (after == s_before || s_unsteppedRun >= 240u) {
        return;
    } else {
        ++s_unsteppedRun;
    }
    s_lastCounter = counter;

    const uint64_t postHash = netSpectatorLiveStateHash();
    ++s_sourceFrame;
    qgame::replay::ReplayNativeFrameV2 frame{};
    frame.sourceFrame = s_sourceFrame;
    frame.core.inputDomain = qreplay::InputDomain::LogicalSourceV1;
    frame.core.activeLaneMask = 0x03u;
    frame.core.actions[0] =
        qinput::compactFromLogical(g_offlinePlayers[0].mask);
    frame.core.actions[1] =
        qinput::compactFromLogical(g_offlinePlayers[1].mask);
    frame.core.before = s_before;
    frame.core.after = after;
    frame.core.dispatcherResult = 0;
    frame.core.preStateHash = s_preHash;
    frame.core.postStateHash = postHash;
    frame.roundTransition =
        netReplayRoundTransitionV2(frame.core.before, frame.core.after);
    frame.activeFighterMask = 0x03u;
    frame.roster = netReplayRosterV2();
    frame.stage = netReplayStageV2();
    const bool published = qgame::replay::replayRecordConfirmedV2(frame);
    if (published && s_staged && !s_opened) {
        s_opened = true;
        QLOG("[replay.record-v2] offline recording publishing from frame=%u",
             s_sourceFrame);
    }

    s_preHash = postHash;
    s_before = frame.core.after;
}

NetFrameMeta netMetaBeforeTick(uint32_t frame, uint32_t phase,
                               uint64_t preHash) {
    NetFrameMeta meta{};
    meta.frame = frame;
    meta.layoutGeneration = preHash ? g_netEpoch : 0;
    meta.epoch = g_netEpoch;
    meta.phaseBefore = phase;
    meta.rngGameplay = rd32(0x4A9938);
    meta.rngCosmetic = rd32(0x4A993C);
    meta.rngSelector = rd32(0x4ABB24);
    // These wire fields carry QOH's effective life, not the guard gauge their names suggest.
    meta.hp0 = netBattleLifeEffective(0);
    meta.hp1 = netBattleLifeEffective(1);
    meta.preHash = preHash;
    if (preHash != 0) {
        const NetFpuSlot& fpu =
            g_netFpuSlots[frame % kPoolSlots];
        if (fpu.valid && fpu.layout == g_rb.layoutGeneration()
            && fpu.epoch == g_netEpoch && fpu.frame == frame) {
            std::memcpy(
                &meta.fpuControlWord, fpu.state,
                sizeof(meta.fpuControlWord));
            std::memcpy(
                &meta.fpuMxcsrRaw, fpu.state + 24,
                sizeof(meta.fpuMxcsrRaw));
            meta.fpuMxcsrCanonical =
                meta.fpuMxcsrRaw & ~0x3Fu;
            meta.fpuHash = netCanonicalFpuHash(fpu);
            const VirtualAudioState canonicalAudio =
                canonicalVirtualAudioForHash(
                    fpu.audio, fpu.audioFrame);
            meta.virtualAudioHash =
                netFnv1a(&canonicalAudio, sizeof(canonicalAudio));
            meta.virtualAudioStatusUntil =
                fpu.audio.statusUntil;
        }
    }
    meta.spectatorPreHash =
        preHash != 0 ? preHash : netFrontendStateFingerprint(frame);
    meta.replayLifecycleBefore =
        netReplayLifecycleV2(g_netEpoch);
    meta.replayRoster = netReplayRosterV2();
    meta.replaySelectorProofBefore = netReplaySelectorProofV2();
    meta.replayStage = netReplayStageV2();
    meta.replayActiveFighterMask = 0x03u;
    meta.valid = true;
    return meta;
}

bool netSavePreFrame(uint32_t frame, uint32_t phase, uint64_t& preHash) {
    const size_t slot = frame % kPoolSlots;
    const uint64_t saveStartUs = netRealQpcUs();
    const bool savedOk = g_rb.saveTaggedToSlot(slot, g_netEpoch, frame, phase);
    g_netSaveWorkUs.add(netRealQpcUs() - saveStartUs);
    if (!savedOk) {
        netFailClosed(kNetInternalFailure, frame,
                      "atomic pre-frame snapshot failed");
        return false;
    }
    netCaptureFpu(slot, g_netEpoch, frame);
    preHash = netTaggedPreHash(slot, g_netEpoch, frame);
    if (!preHash) {
        netFailClosed(kNetInternalFailure, frame,
                      "tagged gameplay/FPU pre-state hash failed");
        return false;
    }
    return true;
}

bool netRestorePreFrame(uint32_t frame, uint32_t phase) {
    const size_t slot = frame % kPoolSlots;
    const uint64_t loadStartUs = netRealQpcUs();
    const bool loadedOk =
        g_rb.loadTaggedFromSlot(slot, g_netEpoch, frame, phase)
        && netRestoreFpu(slot, g_netEpoch, frame);
    g_netLoadWorkUs.add(netRealQpcUs() - loadStartUs);
    if (!loadedOk) {
        netFailClosed(kNetInternalFailure, frame,
                      "tagged rollback restore failed");
        return false;
    }
    return true;
}

void netRememberOwnedTickCrashInput(
    const uint32_t frame, const qnetplay::CanonicalInputs& inputs) {
    qcrash::RecentInputSample sample{};
    sample.frame = frame;
    sample.playerOne = static_cast<uint32_t>(inputs.p1);
    sample.playerTwo = static_cast<uint32_t>(inputs.p2);
    g_ownedTickCrashInputs[g_ownedTickCrashInputNext
        % qcrash::kCrashRecentInputCapacity] = sample;
    ++g_ownedTickCrashInputNext;
    if (g_ownedTickCrashInputCount < qcrash::kCrashRecentInputCapacity)
        ++g_ownedTickCrashInputCount;
}

qcrash::OwnedTickCrashSnapshot netCaptureOwnedTickCrashSnapshot() {
    qcrash::OwnedTickCrashSnapshot snapshot{};
    snapshot.frame = static_cast<uint32_t>(g_ovrFrame);
    snapshot.canonicalFrame = g_netFrame;
    if (rangeReadable(kSceneStateAddr, sizeof(uint32_t)))
        snapshot.lifecycleState = rd32(kSceneStateAddr);
    if (g_netSession) {
        const qnetplay::RollbackDiagnostics diagnostics =
            g_netSession->diagnostics();
        snapshot.confirmedFrame = diagnostics.hasStableConfirmedThrough
            ? diagnostics.stableConfirmedThrough
            : diagnostics.nextConfirmedFrame;
        snapshot.rollbackOrigin = diagnostics.pendingRollback
            ? diagnostics.pendingRollbackFrom
            : (diagnostics.replayInProgress ? diagnostics.replayFrom : 0u);
        snapshot.rollbackDepth = static_cast<uint16_t>((std::min)(
            static_cast<uint32_t>(diagnostics.maxRollbackFrames),
            static_cast<uint32_t>(0xFFFFu)));
        snapshot.speculativeDepth = static_cast<uint16_t>((std::min)(
            diagnostics.speculativeFrames, static_cast<uint32_t>(0xFFFFu)));
        if (diagnostics.pendingRollback || diagnostics.replayInProgress) {
            qcrash::RecentRollbackDecision decision{};
            decision.frame = snapshot.frame;
            decision.origin = snapshot.rollbackOrigin;
            decision.depth = snapshot.rollbackDepth;
            decision.replay = diagnostics.replayInProgress ? 1u : 0u;
            decision.speculative = diagnostics.speculativeFrames != 0u
                ? 1u : 0u;
            g_ownedTickCrashRollback[g_ownedTickCrashRollbackNext
                % qcrash::kCrashRecentRollbackCapacity] = decision;
            ++g_ownedTickCrashRollbackNext;
            if (g_ownedTickCrashRollbackCount
                < qcrash::kCrashRecentRollbackCapacity) {
                ++g_ownedTickCrashRollbackCount;
            }
        }
    }
    const std::array<qreplay::FighterIdentityV2, qreplay::kFighterSlots>
        roster = netReplayRosterV2();
    snapshot.characterIds[0] = roster[0].nativeCharacterId;
    snapshot.characterIds[1] = roster[1].nativeCharacterId;
    for (uint32_t player = 0u; player < 2u; ++player) {
        const uintptr_t fighter = netActiveFighterAddress(player);
        if (fighter == 0u || !rangeReadable(
                fighter, qoh99::fighter_field::kSize)) {
            continue;
        }
        snapshot.actions[player] = static_cast<uint16_t>(rd32(
            fighter + qoh99::fighter_field::kActionState));
        snapshot.animationIndices[player] = static_cast<uint16_t>(rd32(
            fighter + qoh99::fighter_field::kActionFrame));
        snapshot.animationFrames[player] = static_cast<uint16_t>(rd32(
            fighter + qoh99::fighter_field::kAnimFrame));
    }
    snapshot.recentInputCount = g_ownedTickCrashInputCount;
    snapshot.recentRollbackCount = g_ownedTickCrashRollbackCount;
    for (uint8_t index = 0u; index < snapshot.recentInputCount; ++index) {
        const uint32_t source = (g_ownedTickCrashInputNext
            + qcrash::kCrashRecentInputCapacity - snapshot.recentInputCount
            + index) % qcrash::kCrashRecentInputCapacity;
        snapshot.recentInputs[index] = g_ownedTickCrashInputs[source];
    }
    for (uint8_t index = 0u; index < snapshot.recentRollbackCount; ++index) {
        const uint32_t source = (g_ownedTickCrashRollbackNext
            + qcrash::kCrashRecentRollbackCapacity
            - snapshot.recentRollbackCount + index)
            % qcrash::kCrashRecentRollbackCapacity;
        snapshot.recentRollback[index] = g_ownedTickCrashRollback[source];
    }
    return snapshot;
}

DWORD netInvokeOwnedTickWithSeh(FrameDispatchFn orig, int& result) {
    const qcrash::OwnedTickCrashSnapshot snapshot =
        netCaptureOwnedTickCrashSnapshot();
    qnetplay::compat::CompatibilityIdentityWire cachedIdentity{};
    const qnetplay::compat::CompatibilityIdentityWire* identity =
        qcrash::copyCachedCompatibilityIdentity(cachedIdentity)
            ? &cachedIdentity : nullptr;
    qcrash::CapturedCrashContext captured{};
    DWORD exceptionCode = 0u;
    __try {
        result = orig ? orig() : kNetHoldFrameRet;
    } __except ((exceptionCode = GetExceptionCode(),
                qcrash::captureExceptionContext(
                    GetExceptionInformation(), snapshot, identity, captured),
                EXCEPTION_EXECUTE_HANDLER)) {
    }
    if (exceptionCode != 0u)
        qcrash::reportCaughtOwnedTick(captured);
    return exceptionCode;
}

inline bool isNonDrawingDisposition(PresentationDisposition d) {
    return d == PresentationDisposition::SuppressHistorical
        || d == PresentationDisposition::HiddenNewForward
        || d == PresentationDisposition::CorrectedFinal;
}

inline bool isHiddenDisposition(PresentationDisposition d) {
    return d == PresentationDisposition::SuppressHistorical
        || d == PresentationDisposition::HiddenNewForward
        || d == PresentationDisposition::VisibleFastForward;
}

static void netCanonicalRematchTick(uint32_t frame) {
    if (!g_netActive || g_replayRoleActive) return;
    CanonicalRematchState& t = g_canonRematch;
    const uint32_t scene = rd32(kSceneStateAddr);
    if (scene != 2u) {
        if (t.active) {
            for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
                wr32(kRematchTheaterDwordAddrs[i], t.savedDwords[i]);
            QLOG("[rematch-canon] scene moved while active; restored");
        }
        t = CanonicalRematchState{};
        return;
    }
    if (!t.active) {
        if (t.done) {
            if (static_cast<int32_t>(rd32(kMatchSubAddr)) <= 0)
                t = CanonicalRematchState{};
            return;
        }
        const qnetplay::RematchTheaterObserved o = netRematchTheaterObserve();
        if (!qnetplay::rematchTheaterDoorwayEligible(o)) return;
        for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
            t.savedDwords[i] = rd32(kRematchTheaterDwordAddrs[i]);
        t.loserSide = o.loserSide & 1u;
        const qnetplay::RematchTheaterFlipPlan plan =
            qnetplay::rematchTheaterFlipPlan(o);
        wr32(0x62369Cu, plan.formationRow);
        wr32(kMatchSubAddr, static_cast<uint32_t>(plan.matchSub));
        wr32(0x61801Cu, plan.sceneNext);
        wr32(0x6236CCu, plan.vacancyMask);
        t.active = true;
        t.doorwayFrame = frame;
        t.prev[0] = g_netInject[0];
        t.prev[1] = g_netInject[1];
        QLOG("[rematch-canon] prompt OPEN frame=%u loser=P%u (canonical "
             "lockstep; no holds, no wire)", frame, t.loserSide + 1u);
        g_netInject[0] &= ~(qinput::kLogicalButtonMask
                            | qinput::kLogicalDirectionMask);
        g_netInject[1] &= ~(qinput::kLogicalButtonMask
                            | qinput::kLogicalDirectionMask);
        return;
    }
    for (int pIdx = 0; pIdx < 2; ++pIdx) {
        const uint32_t cur = g_netInject[pIdx];
        const uint32_t rising = cur & ~t.prev[pIdx];
        t.prev[pIdx] = cur;
        if (!t.ready[pIdx]
            && (rising & (qinput::kLogicalLeft | qinput::kLogicalRight))
                   != 0u) {
            wr32(kRematchTheaterCursorAddr,
                 rd32(kRematchTheaterCursorAddr) ^ 1u);
        }
        if (!t.ready[pIdx]
            && (rising & qinput::kLogicalButtonMask) != 0u) {
            t.ready[pIdx] = true;
            t.choiceNo[pIdx] = !qnetplay::rematchTheaterCursorIsYes(
                rd32(kRematchTheaterCursorAddr));
            QLOG("[rematch-canon] P%d locked %s frame=%u", pIdx + 1,
                 t.choiceNo[pIdx] ? "NO" : "YES", frame);
        }
    }
    g_netInject[0] &= ~(qinput::kLogicalButtonMask
                        | qinput::kLogicalDirectionMask);
    g_netInject[1] &= ~(qinput::kLogicalButtonMask
                        | qinput::kLogicalDirectionMask);
    const bool timedOut =
        frame - t.doorwayFrame >= kCanonRematchTimeoutFrames;
    if (!(t.ready[0] && t.ready[1]) && !timedOut) return;
    const bool declined = timedOut || t.choiceNo[0] || t.choiceNo[1];
    if (declined) {
        for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
            wr32(kRematchTheaterDwordAddrs[i], t.savedDwords[i]);
        QLOG("[rematch-canon] %s frame=%u -> character select "
             "(formation=%u cpuMask=%u)",
             timedOut ? "TIMEOUT" : "DECLINED", frame,
             rd32(0x62369Cu), rd32(0x6236CCu));
    } else {
        t.restartWritesPending = true;
        QLOG("[rematch-canon] REMATCH frame=%u -> instant restart armed "
             "(writes at the post-tick seam)", frame);
    }
    t.active = false;
    t.done = true;
}

void netCanonicalRematchPostTick(const uint32_t frame) {
    CanonicalRematchState& t = g_canonRematch;
    if (!t.restartWritesPending) return;
    t.restartWritesPending = false;
    for (size_t i = 0; i < kRematchTheaterDwordCount; ++i) {
        if (kRematchTheaterDwordAddrs[i] == kMatchSubAddr) continue;
        wr32(kRematchTheaterDwordAddrs[i], t.savedDwords[i]);
    }
    wr32(kMatchSubAddr, static_cast<uint32_t>(-1));
    wr32(0x626670u, 0u);
    wr32(0x626674u, 0u);
    wr32(0x8A5D20u, 0u);
    g_netInstantRematchLaunch = true;
    g_netInstantRematchBoundary = frame;
    QLOG("[rematch-canon] REMATCH frame=%u -> instant restart "
         "(post-tick launch shape; formation=%u cpuMask=%u tallies=0/0)",
         frame, rd32(0x62369Cu), rd32(0x6236CCu));
}

void netCanonicalRematchCancelForCharacterSelect() {
    if (g_canonRematch.active) {
        for (size_t i = 0; i < kRematchTheaterDwordCount; ++i)
            wr32(kRematchTheaterDwordAddrs[i],
                 g_canonRematch.savedDwords[i]);
    }
    g_canonRematch = CanonicalRematchState{};
}

qnetplay::CanonicalInputs netApplyCharacterSelectBackPolicy(
    const uint32_t frame,
    const qnetplay::CanonicalInputs& inputs) {
    qnetplay::CanonicalInputs filtered = inputs;
    const bool inCharacterSelect =
        rd32(kSceneStateAddr) == 3u
        && rd32(addr::kScene3Substate) == 2u;
    if (!inCharacterSelect) return filtered;

    uint16_t* const actions[2] = {&filtered.p1, &filtered.p2};
    const uintptr_t confirmations[2] = {
        addr::kCharSelConfirmP1,
        addr::kCharSelConfirmP2,
    };
    for (size_t slot = 0; slot < 2u; ++slot) {
        const uint32_t before =
            qinput::logicalFromCompact(*actions[slot]);
        const uint32_t confirmationState = rd32(confirmations[slot]);
        const uint32_t after =
            qgame::filterUnselectedCharacterSelectBack(
                true, confirmationState, before);
        *actions[slot] = qinput::compactFromLogical(after);
        if (after != before) {
            if (g_netCharSelectBackBlocked < 8u) {
                QLOG("[netplay-input] ignored B before character selection "
                     "frame=%u side=P%u confirmState=%u word=%08X->%08X",
                     frame, static_cast<unsigned>(slot + 1u),
                     confirmationState, before, after);
            }
            ++g_netCharSelectBackBlocked;
        }
    }
    return filtered;
}

bool netRunOwnedTick(FrameDispatchFn orig, uint32_t frame,
                     const qnetplay::CanonicalInputs& inputs,
                     bool replay, int& result, bool notifyPeer = true,
                     PresentationDisposition presentation =
                         PresentationDisposition::Normal) {
    const bool bootOwnedThisFrame = qgame::directBootNetworkActive();
    if (bootOwnedThisFrame) qgame::directBootNetworkTick();
    if (qgame::directBootNetworkFailed()) {
        netFailClosed(kNetInternalFailure, frame,
                      "deterministic DirectBoot failed", notifyPeer);
        return false;
    }
    g_netInject[0] =
        bootOwnedThisFrame ? 0 : unpackLogical(inputs.p1);
    g_netInject[1] =
        bootOwnedThisFrame ? 0 : unpackLogical(inputs.p2);
    const uint32_t rawCanonical[2] = {
        static_cast<uint32_t>(g_netInject[0]),
        static_cast<uint32_t>(g_netInject[1]),
    };
    if (!bootOwnedThisFrame) netCanonicalRematchTick(frame);
    if (!bootOwnedThisFrame) {
        const uint32_t scene = rd32(kSceneStateAddr);
        const int32_t matchSub = static_cast<int32_t>(rd32(kMatchSubAddr));
        const bool postGameplay = scene == 2u && matchSub != 0;
        const bool frontend = scene == 3u;
        for (int slot = 0; slot < 2; ++slot) {
            const uint32_t before = g_netInject[slot];
            const uint32_t allowed = g_netWinscreenCarry[slot].filter(
                postGameplay, frontend, rawCanonical[slot]);
            g_netInject[slot] = static_cast<int>(before & allowed);
            if (before != 0u && g_netInject[slot] == 0) {
                if (g_netWinscreenCarryBlocked < 4u) {
                    QLOG("[winscreen-carry] held confirm withheld from the "
                         "frontend frame=%u slot=P%d scene=%u matchSub=%d "
                         "word=%08X (release to press again)",
                         frame, slot + 1, scene, matchSub, before);
                }
                ++g_netWinscreenCarryBlocked;
            }
        }
    }
    g_ovrFrame = frame;
    netRememberOwnedTickCrashInput(frame, inputs);
    g_netCanonicalPoll = true;
    g_replayActive = replay;
    const bool audioV2Tick = g_audioV2Enabled && g_netActive;
    bool audioTickOpen = false;
    if (audioV2Tick) {
        // Coupled to the exact-input predicate: do not pin one without the other, or a rolled-back KO replays its voice and hit sounds.
        const bool rollbackCapableTick = rollbackEligible()
            && !netLifecycleTransitionRequiresExactInput();
        if (rollbackCapableTick) {
            using AudioDisp = qaudio::AudioTickDisposition;
            AudioDisp audioDisposition;
            if (g_netSpectatorMode) {
                audioDisposition = replay
                    ? AudioDisp::SpectatorConfirmedCatchup
                    : AudioDisp::SpectatorConfirmedVisible;
            } else if (replay) {
                audioDisposition = AudioDisp::HistoricalCorrection;
            } else if (presentation
                           == PresentationDisposition::HiddenNewForward) {
                audioDisposition = AudioDisp::ForwardHidden;
            } else {
                audioDisposition = AudioDisp::ForwardVisible;
            }
            qaudio::runtime().setDisposition(audioDisposition);
            audioTickOpen =
                qaudio::runtime().beginTick(g_netEpoch, g_netEpoch, frame);
        } else {
            qaudio::runtime().setDisposition(
                qaudio::AudioTickDisposition::ImmediateOffline);
        }
    }
    ScopedPresentationDisposition presentationScope(presentation);
    if (presentation == PresentationDisposition::SuppressHistorical)
        ++g_hiddenReplayNativeTicks;
    else if (presentation == PresentationDisposition::HiddenNewForward)
        ++g_hiddenForwardNativeTicks;
    else if (presentation == PresentationDisposition::CorrectedFinal)
        ++g_correctedFinalNativeTicks;
    netClockBegin(frame);
    InterlockedExchange(&g_freezeGateCalls, 0);
    const LONG ownedTickSerial =
        InterlockedIncrement(&g_netOwnedTickSerial);
    InterlockedExchange(&g_netGateOwnsTick, 1);

    result = kNetHoldFrameRet;
    const unsigned long presentationQueriesBefore =
        isNonDrawingDisposition(presentation)
            ? presentationDispositionQueryCount()
            : 0;
    const bool preserveNativeFpsAccounting =
        isHiddenDisposition(presentation);
    const qgame::QohNativeFpsAccounting nativeFpsAccountingBefore =
        preserveNativeFpsAccounting
            ? qgame::captureQohNativeFpsAccounting(
                  [](const uintptr_t address) {
                      return rd32(address);
                  })
            : qgame::QohNativeFpsAccounting{};
    const DWORD exceptionCode =
        netInvokeOwnedTickWithSeh(orig, result);
    if (preserveNativeFpsAccounting) {
        qgame::restoreQohNativeFpsAccounting(
            nativeFpsAccountingBefore,
            [](const uintptr_t address, const uint32_t value) {
                wr32(address, value);
            });
    }
    if (exceptionCode == 0 && !bootOwnedThisFrame)
        netCanonicalRematchPostTick(frame);
    if (exceptionCode == 0
        && !isHiddenDisposition(presentation)) {
        const LONG grantsBefore = InterlockedCompareExchange(
            &g_netPresentationSlotGrants, 0, 0);
        (void)netConsumePresentationSlot(ownedTickSerial);
        if (InterlockedCompareExchange(
                &g_netPresentationSlotGrants, 0, 0)
            > grantsBefore) {
            InterlockedIncrement(&g_netNoCommitFallbackGrants);
        }
    }
    const unsigned long presentationQueriesAfter =
        isNonDrawingDisposition(presentation)
            ? presentationDispositionQueryCount()
            : presentationQueriesBefore;

    InterlockedExchange(&g_netGateOwnsTick, 0);
    netClockOff();
    g_replayActive = false;
    g_netCanonicalPoll = false;
    g_netPhysicalPoll = false;
    if (audioV2Tick) {
        bool audioCommitFailed = false;
        if (audioTickOpen) {
            if (exceptionCode) {
                qaudio::runtime().abortTick();
            } else {
                audioCommitFailed = !qaudio::runtime().commitTick();
            }
        }
        qaudio::runtime().setDisposition(
            qaudio::AudioTickDisposition::ImmediateOffline);
        if (audioCommitFailed) {
            netFailClosed(kNetInternalFailure, frame,
                          "audio intent journal commit failed", notifyPeer);
            return false;
        }
    }
    if (exceptionCode) {
        char why[128];
        std::snprintf(why, sizeof(why),
                      "game tick raised SEH exception 0x%08lX",
                      static_cast<unsigned long>(exceptionCode));
        netFailClosed(kNetInternalFailure, frame, why, notifyPeer);
        return false;
    }
    if (isNonDrawingDisposition(presentation)) {
        if (presentationQueriesAfter == presentationQueriesBefore) {
            ++g_missingPresentationGateTicks;
            if (g_verifiedHiddenPresentationTicks == 0
                && presentationQueriesAfter == 0ul) {
                netFailClosed(
                    kNetInternalFailure, frame,
                    "compatible ddraw presentation gate not active",
                    notifyPeer);
                return false;
            }
            if (g_missingPresentationGateTicks <= 8u
                || (g_missingPresentationGateTicks % 300u) == 0u) {
                QLOG("[presentation-gate] hidden tick drew nothing frame=%u "
                     "(no disposition query; gate already proven this session, "
                     "verified=%llu missing=%llu) - legal non-rendering "
                     "lifecycle tick, nothing could leak",
                     frame,
                     static_cast<unsigned long long>(
                         g_verifiedHiddenPresentationTicks),
                     static_cast<unsigned long long>(
                         g_missingPresentationGateTicks));
            }
            return true;
        }
        ++g_verifiedHiddenPresentationTicks;
        if (g_verifiedHiddenPresentationTicks <= 8
            || (g_verifiedHiddenPresentationTicks % 300u) == 0u) {
            QLOG("[presentation-gate] hidden tick verified frame=%u "
                 "queryBefore=%lu queryAfter=%lu verified=%llu "
                 "wrapper normalVisiblePresents=%lu suppressedPresents=%lu "
                 "correctedFinalPresents=%lu",
                 frame, presentationQueriesBefore,
                 presentationQueriesAfter,
                 static_cast<unsigned long long>(
                     g_verifiedHiddenPresentationTicks),
                 static_cast<unsigned long>(
                     InterlockedCompareExchange(
                         &g_wrapperNormalVisiblePresents, 0, 0)),
                 static_cast<unsigned long>(
                     InterlockedCompareExchange(
                         &g_wrapperSuppressedPresents, 0, 0)),
                 static_cast<unsigned long>(
                     InterlockedCompareExchange(
                         &g_wrapperCorrectedFinalPresents, 0, 0)));
        }
    }
    return true;
}

static bool netRunRematchTheaterTick(FrameDispatchFn orig) {
    RematchTheaterState& t = g_rematchTheater;
    if (!t.flipped || g_netHalted) return false;
    const uint64_t nowUs = netRealQpcUs();
    if (t.nextTickQpcUs != 0
        && nowUs < static_cast<uint64_t>(t.nextTickQpcUs))
        return false;
    t.nextTickQpcUs = static_cast<int64_t>(nowUs + 16667u);
    if (!netRematchTheaterGuard("pre-tick")) return false;
    qnetplay::CanonicalInputs feed{};
    feed.frame = t.doorwayFrame;
    const uint16_t local =
        packLogical(static_cast<int>(t.feedMask));
    feed.p1 = g_netLocalCtrl == 0 ? local : static_cast<uint16_t>(0);
    feed.p2 = g_netLocalCtrl == 1 ? local : static_cast<uint16_t>(0);
    int result = kNetHoldFrameRet;
    if (!netRunOwnedTick(orig, t.doorwayFrame, feed, false,
                         result, true,
                         PresentationDisposition::Normal)) {
        return false;
    }
    ++t.asyncTicks;
    (void)netRematchTheaterGuard("post-tick");
    return true;
}

void netWriteConfirmedCheckpoint(const NetFrameMeta& meta) {
    if (!g_netAutomatchTest || meta.preHash == 0
        || (meta.frame % 30u) != 0u) {
        return;
    }
    QLOG("[confirmed-components] frame=%u layout=%u epoch=%u "
         "gameplayHash=%016llX "
         "fpuHash=%016llX fcw=%04X mxcsr=%08X/%08X "
         "audioHash=%016llX audio="
         "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
         meta.frame, meta.layoutGeneration, meta.epoch,
         static_cast<unsigned long long>(meta.preHash),
         static_cast<unsigned long long>(meta.fpuHash),
         static_cast<unsigned>(meta.fpuControlWord),
         static_cast<unsigned>(meta.fpuMxcsrRaw),
         static_cast<unsigned>(meta.fpuMxcsrCanonical),
         static_cast<unsigned long long>(meta.virtualAudioHash),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[0]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[1]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[2]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[3]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[4]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[5]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[6]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[7]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[8]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[9]),
         static_cast<unsigned long long>(
             meta.virtualAudioStatusUntil[10]));
}

void netWriteConfirmedDesync(const NetFrameMeta& local,
                             const qgame::NetConfirmedHash& peer) {
    QLOG_ISSUE("[netplay] *** CONFIRMED DESYNC frame=%u epoch=%u layout=%u ***",
         peer.frame, peer.epoch, peer.layoutGeneration);
    QLOG("[netplay] local hash=%016llX rng=%08X/%08X/%08X hp=[%d,%d]",
         static_cast<unsigned long long>(local.preHash),
         local.rngGameplay, local.rngCosmetic, local.rngSelector,
         local.hp0, local.hp1);
    QLOG("[netplay] local components fpu=%016llX fcw=%04X "
         "mxcsr=%08X/%08X audio=%016llX "
         "statusUntil=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
         "%llu,%llu,%llu",
         static_cast<unsigned long long>(local.fpuHash),
         static_cast<unsigned>(local.fpuControlWord),
         static_cast<unsigned>(local.fpuMxcsrRaw),
         static_cast<unsigned>(local.fpuMxcsrCanonical),
         static_cast<unsigned long long>(local.virtualAudioHash),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[0]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[1]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[2]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[3]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[4]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[5]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[6]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[7]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[8]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[9]),
         static_cast<unsigned long long>(
             local.virtualAudioStatusUntil[10]));
    QLOG("[netplay] peer  hash=%016llX rng=%08X/%08X/%08X hp=[%d,%d]",
         static_cast<unsigned long long>(peer.gameplayHash),
         peer.rngGameplay, peer.rngCosmetic, peer.rngSelector,
         peer.hp0, peer.hp1);
    if (!g_sessionDir[0]) return;
    const size_t slot = peer.frame % kPoolSlots;
    std::vector<RollbackManager::RegionHash> regions;
    const uint64_t slotHash = g_rb.hashSlot(
        slot, RollbackManager::Profile::Gameplay, &regions);
    const RollbackManager::SlotTag* slotTag = g_rb.slotTag(slot);
    QLOG("[netplay] confirmed snapshot slot=%zu hash=%016llX "
         "tag=%s/%u/%u/%u regions=%zu",
         slot, static_cast<unsigned long long>(slotHash),
         slotTag && slotTag->valid ? "valid" : "invalid",
         slotTag ? slotTag->epoch : 0,
         slotTag ? slotTag->frame : 0,
         slotTag ? slotTag->phase : 0,
         regions.size());
    for (size_t index = 0; index < regions.size(); ++index) {
        const RollbackManager::RegionHash& region = regions[index];
        QLOG("[confirmed-region] index=%zu name=%s size=%zu present=%d "
             "hash=%016llX",
             index, region.name, region.size, region.present ? 1 : 0,
             static_cast<unsigned long long>(region.hash));
    }
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\confirmed_desync.jsonl",
                  g_sessionDir);
    if (FILE* f = std::fopen(path, "a")) {
        std::fprintf(
            f,
            "{\"frame\":%u,\"epoch\":%u,\"layout\":%u,"
            "\"localHash\":\"%016llX\",\"peerHash\":\"%016llX\","
            "\"localRng\":[%u,%u,%u],\"peerRng\":[%u,%u,%u],"
            "\"localHp\":[%d,%d],\"peerHp\":[%d,%d],"
            "\"localFpuHash\":\"%016llX\",\"localFcw\":%u,"
            "\"localMxcsrRaw\":%u,\"localMxcsrCanonical\":%u,"
            "\"localVirtualAudioHash\":\"%016llX\","
            "\"localVirtualAudio\":[%llu,%llu,%llu,%llu,%llu,"
            "%llu,%llu,%llu,%llu,%llu,%llu]}\n",
            peer.frame, peer.epoch, peer.layoutGeneration,
            static_cast<unsigned long long>(local.preHash),
            static_cast<unsigned long long>(peer.gameplayHash),
            local.rngGameplay, local.rngCosmetic, local.rngSelector,
            peer.rngGameplay, peer.rngCosmetic, peer.rngSelector,
            local.hp0, local.hp1, peer.hp0, peer.hp1,
            static_cast<unsigned long long>(local.fpuHash),
            static_cast<unsigned>(local.fpuControlWord),
            static_cast<unsigned>(local.fpuMxcsrRaw),
            static_cast<unsigned>(local.fpuMxcsrCanonical),
            static_cast<unsigned long long>(
                local.virtualAudioHash),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[0]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[1]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[2]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[3]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[4]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[5]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[6]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[7]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[8]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[9]),
            static_cast<unsigned long long>(
                local.virtualAudioStatusUntil[10]));
        std::fclose(f);
    }
    std::snprintf(
        path, sizeof(path),
        "%s\\confirmed_desync_regions_frame_%u.jsonl",
        g_sessionDir, peer.frame);
    if (FILE* f = std::fopen(path, "w")) {
        for (size_t index = 0; index < regions.size(); ++index) {
            const RollbackManager::RegionHash& region = regions[index];
            std::fprintf(
                f,
                "{\"frame\":%u,\"epoch\":%u,\"slot\":%zu,"
                "\"slotHash\":\"%016llX\",\"index\":%zu,"
                "\"name\":\"%s\",\"size\":%zu,\"present\":%s,"
                "\"hash\":\"%016llX\"}\n",
                peer.frame, peer.epoch, slot,
                static_cast<unsigned long long>(slotHash),
                index, region.name, region.size,
                region.present ? "true" : "false",
                static_cast<unsigned long long>(region.hash));
        }
        std::fclose(f);
    }
    const uint8_t* slotBytes = g_rb.slotData(slot);
    if (slotBytes) {
        std::snprintf(
            path, sizeof(path),
            "%s\\confirmed_desync_frame_%u_epoch_%u_slot_%zu.bin",
            g_sessionDir, peer.frame, peer.epoch, slot);
        if (FILE* f = std::fopen(path, "wb")) {
            std::fwrite(slotBytes, 1, g_rb.totalSize(), f);
            std::fclose(f);
        }
    }
}

static bool g_recorderEnabled = false;
static uint32_t g_recorderGeneration = 0;
static uint64_t g_recorderPublished = 0;
static uint64_t g_recorderRollingDigest = 0;
static bool g_recorderDisabled = false;
static bool g_recorderOverflowReported = false;
static uint32_t g_recorderLastFrame = 0;
static bool g_recorderHasLastFrame = false;

static qreplay::DeterministicStart g_recorderStart{};
static bool g_recorderStartReady = false;
static const char* g_recorderStartSource = "none";

static uint32_t g_recorderMatchId = 0;
static uint32_t g_recorderFirstMatchId = 0;
static uint32_t g_recorderNextMatchId = 1;
static uint32_t g_recorderNextOrdinal = 0;
static bool g_recorderMatchSceneSeen = false;
static uint64_t g_recorderMatchPostTailHash = 0;

bool netRecorderArmed() {
    return g_recorderEnabled && !g_recorderDisabled
        && qgame::replay::replayRecorderActive()
        && !qgame::replay::replayRecorderV2Active();
}

void netRecorderCopyDisplayName(std::array<char, 32>& dst, const char* src,
                                size_t capacity) {
    dst.fill('\0');
    if (src == nullptr) return;
    const size_t limit = capacity < 20u ? capacity : 20u;
    for (size_t i = 0; i < limit; ++i) {
        const unsigned char byte = static_cast<unsigned char>(src[i]);
        if (byte == 0u || byte < 0x20u || byte == 0x7Fu) break;
        dst[i] = src[i];
    }
}

void netRecorderFillStartConfig(qreplay::DeterministicStart& start,
                                const qgame::NetReadyState& ready,
                                uint32_t initialPhase, bool named) {
    start = qreplay::DeterministicStart{};
    start.seedGameplay = ready.seedGameplay;
    start.seedCosmetic = ready.seedCosmetic;
    start.selector = ready.selector;
    start.initialPhase = initialPhase;
    start.bootstrapHash = ready.bootstrapHash;
    start.timerMode = ready.timerMode;
    start.aiDifficulty = ready.aiDifficulty;
    start.lifeMultiplier = ready.lifeMultiplier;
    start.roundsToWinMinusOne = ready.roundsToWinMinusOne;
    if (!named) return;
    start.playerOneNameCodePage = g_netPlayerNameCodePages[0];
    start.playerTwoNameCodePage = g_netPlayerNameCodePages[1];
    netRecorderCopyDisplayName(start.displayNames[0], g_netPlayerNames[0],
                               sizeof(g_netPlayerNames[0]));
    netRecorderCopyDisplayName(start.displayNames[1], g_netPlayerNames[1],
                               sizeof(g_netPlayerNames[1]));
}

void netRecorderCaptureNetplayStart(const qgame::NetReadyState& chosen) {
    if (!netRecorderArmed() || g_recorderStartReady) return;
    netRecorderFillStartConfig(g_recorderStart, chosen, netPhaseTag(), true);
    g_recorderStartReady = true;
    g_recorderStartSource = "netplay-go";
    QLOG("[recorder] deterministic start captured at the netplay GO barrier "
         "seeds=[%08X,%08X,%08X] bootstrap=%016llX phase=%08X "
         "config=[timer=%u ai=%u life=%u roundsMinusOne=%u] names=['%s','%s']",
         chosen.seedGameplay, chosen.seedCosmetic, chosen.selector,
         static_cast<unsigned long long>(chosen.bootstrapHash),
         g_recorderStart.initialPhase, chosen.timerMode, chosen.aiDifficulty,
         chosen.lifeMultiplier, chosen.roundsToWinMinusOne,
         g_recorderStart.displayNames[0].data(),
         g_recorderStart.displayNames[1].data());
}

bool netRecorderCaptureOfflineStart() {
    if (!netRecorderArmed()) return false;
    if (g_recorderStartReady) return true;
    qgame::NetReadyState ready{};
    ready.seedGameplay = rd32(0x4A9938);
    ready.seedCosmetic = rd32(0x4A993C);
    ready.selector = rd32(0x4ABB24);
    ready.bootstrapHash = netBootstrapFingerprint();
    if (!netReadActiveMatchConfig(ready)) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            QLOG("[recorder] offline deterministic start not yet available: "
                 "the active QOH match config is not one this build can state "
                 "- the recording waits for a boundary it can describe and the "
                 "session is unaffected");
        }
        return false;
    }
    netRecorderFillStartConfig(g_recorderStart, ready, netPhaseTag(), false);
    g_recorderStartReady = true;
    g_recorderStartSource = "offline-arm";
    QLOG("[recorder] deterministic start captured at the offline arm seam "
         "seeds=[%08X,%08X,%08X] bootstrap=%016llX phase=%08X "
         "config=[timer=%u ai=%u life=%u roundsMinusOne=%u]",
         ready.seedGameplay, ready.seedCosmetic, ready.selector,
         static_cast<unsigned long long>(ready.bootstrapHash),
         g_recorderStart.initialPhase, ready.timerMode, ready.aiDifficulty,
         ready.lifeMultiplier, ready.roundsToWinMinusOne);
    return true;
}

// `phaseTag` is netPhaseTag()'s packing: scene in the high half, match substate in the low half.
qreplay::LifecycleTag netLifecycleTagFromPhase(uint32_t phaseTag,
                                               uint32_t layoutGeneration) {
    qreplay::LifecycleTag tag{};
    tag.scene = (phaseTag >> 16) & 0xFFFFu;
    tag.matchState = phaseTag & 0xFFFFu;
    tag.layoutGeneration = layoutGeneration;
    return tag;
}

bool netRecorderCloseMatchSegment(qreplay::TerminalReason reason) {
    if (g_recorderMatchId == 0u || !g_recorderHasLastFrame) {
        g_recorderMatchId = 0u;
        g_recorderMatchSceneSeen = false;
        return true;
    }
    qreplay::MatchEndRecord record{};
    record.matchId = g_recorderMatchId;
    record.lastTimelineFrame = g_recorderLastFrame;
    record.winningSideMask = 0u;
    record.roundsSideZero = 0u;
    record.roundsSideOne = 0u;
    record.reason = reason;
    record.postTailStateHash = g_recorderMatchPostTailHash;
    record.rollingDigest = g_recorderRollingDigest;
    g_recorderMatchId = 0u;
    g_recorderMatchSceneSeen = false;
    return qgame::replay::replayRecordMatchEnd(record);
}

void netRecorderFailGeneration(uint32_t frame, const char* why) {
    if (g_recorderDisabled) return;
    g_recorderDisabled = true;
    if (!g_recorderOverflowReported) {
        g_recorderOverflowReported = true;
        (void)netRecorderCloseMatchSegment(
            qreplay::TerminalReason::RecorderOverflow);
        qgame::replay::replayRecordTerminal(
            static_cast<uint16_t>(qreplay::TerminalReason::RecorderOverflow),
            frame, 0u, g_recorderRollingDigest);
    }
    const qgame::replay::ReplayRecorderStats stats =
        qgame::replay::replayRecorderStats();
    QLOG_ISSUE("[recorder] recording generation %u DISABLED at frame=%u: %s "
         "(publishedHere=%llu published=%llu dropped=%llu highWater=%llu "
         "written=%llu start=%s matches=%u/%u) - the match is unaffected and "
         "the file will never be finalized",
         g_recorderGeneration, frame, why ? why : "?",
         static_cast<unsigned long long>(g_recorderPublished),
         static_cast<unsigned long long>(stats.published),
         static_cast<unsigned long long>(stats.dropped),
         static_cast<unsigned long long>(stats.queueHighWater),
         static_cast<unsigned long long>(stats.written),
         g_recorderStartSource, stats.matchesStarted, stats.matchesEnded);
}

bool netRecorderOpenMatchSegment(uint32_t frame, uint64_t preStateHash,
                                 const qreplay::LifecycleTag& before) {
    qreplay::MatchStartRecord record{};
    record.matchId = g_recorderNextMatchId;
    record.ordinal = g_recorderNextOrdinal;
    record.firstTimelineFrame = frame;
    record.start = g_recorderStart;
    record.start.firstTimelineFrame = frame;
    record.start.initialStateHash = preStateHash;
    record.start.initialDigest = g_recorderRollingDigest;
    if (g_recorderFirstMatchId == 0u) {
        record.dependsOnMatchId = 0u;
        record.independentBootstrap = true;
    } else {
        record.dependsOnMatchId = g_recorderFirstMatchId;
        record.independentBootstrap = false;
        record.start.seedGameplay = 0u;
        record.start.seedCosmetic = 0u;
        record.start.selector = 0u;
        record.start.initialPhase =
            ((before.scene & 0xFFFFu) << 16) | (before.matchState & 0xFFFFu);
    }
    for (size_t slot = 0; slot < qreplay::kFighterSlots; ++slot) {
        qreplay::FighterSlotDescriptor& descriptor = record.slots[slot];
        descriptor.ownership = slot < 2u
            ? qreplay::SlotOwnership::RecordedHuman
            : qreplay::SlotOwnership::Inactive;
        descriptor.side = static_cast<uint8_t>(slot & 1u);
        descriptor.identity.nativeCharacterId = qreplay::kInactiveCharacterId;
        descriptor.identity.paletteIndex = 0u;
        descriptor.selectorProofPresent = false;
        record.slotToSourceRoute[slot] = static_cast<uint8_t>(slot);
    }
    record.vsRow = qipc::kVsRow1v1;
    record.cpuMask = 0u;
    record.rosterEpoch = 0u;
    record.stageId = 0u;
    record.stageVerified = false;
    if (!qgame::replay::replayRecordMatchStart(record)) return false;
    g_recorderMatchId = record.matchId;
    g_recorderMatchSceneSeen = false;
    if (g_recorderFirstMatchId == 0u)
        g_recorderFirstMatchId = record.matchId;
    ++g_recorderNextMatchId;
    ++g_recorderNextOrdinal;
    return true;
}

bool netRecorderPublishFrame(uint32_t frame, uint16_t p1, uint16_t p2,
                             uint64_t preStateHash, uint64_t postStateHash,
                             const qreplay::LifecycleTag& before,
                             const qreplay::LifecycleTag& after,
                             int dispatcherResult, uint32_t routeEpoch) {
    if (!netRecorderArmed()) return false;
    if (!qgame::replay::replayRecordManifest()) {
        netRecorderFailGeneration(frame,
                                  "the session manifest could not be published");
        return false;
    }
    if (g_recorderHasLastFrame && frame == g_recorderLastFrame) {
        return true;
    }
    if (g_recorderHasLastFrame && frame != g_recorderLastFrame + 1u) {
        netRecorderFailGeneration(frame, "non-contiguous canonical timeline");
        return false;
    }
    if (!qgame::replay::replayRecorderSessionStartPublished()) {
        if (!g_recorderStartReady) {
            netRecorderFailGeneration(
                frame, "no proven deterministic start was captured");
            return false;
        }
        qreplay::DeterministicStart start = g_recorderStart;
        start.firstTimelineFrame = frame;
        start.initialStateHash = preStateHash;
        start.initialDigest = g_recorderRollingDigest;
        if (!qgame::replay::replayRecordSessionStart(start)) {
            netRecorderFailGeneration(
                frame, "the deterministic session start could not be published");
            return false;
        }
    }
    const bool inMatchBefore =
        before.scene == static_cast<uint32_t>(Scene::Match);
    if (g_recorderMatchId == 0u
        && (g_recorderFirstMatchId == 0u || inMatchBefore)) {
        if (!netRecorderOpenMatchSegment(frame, preStateHash, before)) {
            netRecorderFailGeneration(frame,
                                      "the match start could not be published");
            return false;
        }
    }
    if (inMatchBefore) g_recorderMatchSceneSeen = true;

    qreplay::ReplayFrameV1 out{};
    out.timelineFrame = frame;
    out.inputDomain = qreplay::InputDomain::LogicalSourceV1;
    out.activeLaneMask = 0x3u;
    out.actions[0] = p1;
    out.actions[1] = p2;
    out.actions[2] = 0u;
    out.actions[3] = 0u;
    out.routeEpoch = routeEpoch;
    out.preStateHash = preStateHash;
    out.postStateHash = postStateHash;
    out.lifecycleBefore = before;
    out.lifecycleAfter = after;
    out.dispatcherResult = dispatcherResult;
    g_recorderRollingDigest =
        qreplay::sealFrameDigest(g_recorderRollingDigest, out);
    if (!qgame::replay::replayRecordConfirmed(out)) {
        netRecorderFailGeneration(frame, "publication ring is full");
        return false;
    }
    g_recorderLastFrame = frame;
    g_recorderHasLastFrame = true;
    g_recorderMatchPostTailHash = postStateHash;
    ++g_recorderPublished;

    if (g_recorderMatchId != 0u && g_recorderMatchSceneSeen
        && after.scene != static_cast<uint32_t>(Scene::Match)) {
        if (!netRecorderCloseMatchSegment(
                qreplay::TerminalReason::Completed)) {
            netRecorderFailGeneration(frame,
                                      "the match end could not be published");
            return false;
        }
    }
    return true;
}

static void spectatorSetCheckpointCaptureAtPublish(
    uint32_t frame, const NetFrameMeta& meta) {
    if (!g_spectatorSetCheckpointArmed || g_spectatorSetCheckpointDone)
        return;
    if (!g_rbReady || g_rb.totalSize() == 0u) return;
    const size_t slot = frame % kPoolSlots;
    const NetFpuSlot& fpu = g_netFpuSlots[slot];
    if (!fpu.valid || fpu.frame != frame || fpu.epoch != meta.epoch
        || fpu.layout != g_rb.layoutGeneration()) {
        return;
    }
    const uint32_t logicalStage = rd32(kSetSeekStageAddr);
    if (!g_netStagedSelectorProofValid || logicalStage >= 28u) return;
    unsigned char bootDescriptor[kSetSeekBootBytes];
    packSetSeekBoot(bootDescriptor, logicalStage);
    static std::vector<uint8_t> snapshot;
    snapshot.resize(g_rb.totalSize());
    if (!g_rb.copySlotTo(slot, snapshot.data())) return;
    const VirtualAudioState audioCanonical =
        canonicalVirtualAudioForHash(fpu.audio, fpu.audioFrame);
    const uint32_t audioCount =
        static_cast<uint32_t>(audioCanonical.statusUntil.size());
    uint8_t audioRegion[4u + sizeof(uint64_t) * 32u];
    const size_t audioRegionBytes = 4u + sizeof(uint64_t) * audioCount;
    if (audioRegionBytes > sizeof(audioRegion)) return;
    std::memcpy(audioRegion, &audioCount, 4u);
    std::memcpy(audioRegion + 4u, audioCanonical.statusUntil.data(),
                sizeof(uint64_t) * audioCount);
    if (qgame::spectator_publish::publishStateCheckpoint(
            snapshot.data(), snapshot.size(),
            0x51524231u /* 'QRB1': one flat g_rb region snapshot */,
            g_rb.layoutGeneration(),
            meta.spectatorPreHash,
            bootDescriptor, sizeof(bootDescriptor),
            audioRegion, audioRegionBytes)) {
        g_spectatorSetCheckpointArmed = false;
        g_spectatorSetCheckpointDone = true;
        QLOG("[checkpoint] game %u canonical slot captured at record %u "
             "(%zu bytes, layoutGen=%u, hash=%016llX)",
             g_spectatorSetMatchOrdinal, frame, snapshot.size(),
             g_rb.layoutGeneration(),
             static_cast<unsigned long long>(meta.spectatorPreHash));
    }
}

bool netPublishAndVerifyConfirmed() {
    if (!g_netSession) return false;
    enum class EpochRelation {
        Current,
        Stale,
        Future,
        Inconsistent,
    };
    const auto serialAfter = [](uint32_t a, uint32_t b) {
        const uint32_t distance = a - b;
        return distance != 0 && distance < 0x80000000u;
    };
    const auto relationToCurrent =
        [&](uint32_t layoutGeneration, uint32_t epoch) {
            if (layoutGeneration == g_netEpoch && epoch == g_netEpoch)
                return EpochRelation::Current;
            const bool layoutStale =
                serialAfter(g_netEpoch, layoutGeneration);
            const bool epochStale = serialAfter(g_netEpoch, epoch);
            if (layoutStale && epochStale)
                return EpochRelation::Stale;
            const bool layoutFuture =
                serialAfter(layoutGeneration, g_netEpoch);
            const bool epochFuture = serialAfter(epoch, g_netEpoch);
            if (layoutFuture && epochFuture)
                return EpochRelation::Future;
            return EpochRelation::Inconsistent;
        };
    qnetplay::ConfirmedFrame confirmed{};
    while (g_netSession->popConfirmedFrame(confirmed)) {
        NetFrameMeta& meta = g_netMeta[confirmed.frame % kNetMetaSlots];
        if (!meta.valid || meta.frame != confirmed.frame
            || meta.preHash != confirmed.preStateHash) {
            netFailClosed(kNetInternalFailure, confirmed.frame,
                          "confirmed frame metadata was missing or stale");
            return false;
        }
        g_netConfirmedThrough = confirmed.frame;
        g_netHasConfirmed = true;

        if (qgame::replay::replayRecorderV2Active()) {
            qgame::replay::ReplayNativeFrameV2 replayFrame{};
            replayFrame.sourceFrame = confirmed.frame;
            replayFrame.core.inputDomain =
                qreplay::InputDomain::LogicalSourceV1;
            replayFrame.core.activeLaneMask = 0x03u;
            replayFrame.core.actions[0] = meta.canonicalActions[0];
            replayFrame.core.actions[1] = meta.canonicalActions[1];
            replayFrame.core.before =
                meta.replayLifecycleBefore;
            replayFrame.core.after =
                meta.replayLifecycleAfter;
            replayFrame.core.dispatcherResult =
                meta.replayDispatcherResult;
            replayFrame.core.preStateHash =
                meta.spectatorPreHash;
            replayFrame.core.postStateHash =
                meta.spectatorPostHash;
            replayFrame.roundTransition =
                meta.replayRoundTransition;
            replayFrame.activeFighterMask =
                meta.replayActiveFighterMask;
            replayFrame.roster = meta.replayRoster;
            replayFrame.stage = meta.replayStage;
            (void)qgame::replay::replayRecordConfirmedV2(
                replayFrame);
        }

        if (netRecorderArmed()) {
            netRecorderPublishFrame(
                confirmed.frame, meta.canonicalActions[0],
                meta.canonicalActions[1],
                meta.spectatorPreHash, meta.spectatorPostHash,
                netLifecycleTagFromPhase(meta.phaseBefore,
                                         meta.layoutGeneration),
                netLifecycleTagFromPhase(meta.phaseAfter,
                                         meta.layoutGeneration),
                0, meta.epoch);
        }

        if (g_spectatorServiceMode == 2u) {
            if (meta.spectatorPreHash == 0 || meta.spectatorPostHash == 0) {
                qgame::spectator_publish::publishFault(
                    qgame::spectator_publish::PublishDisableReason::
                        InvalidRecord);
                if (!g_spectatorSidecarDropLogged) {
                    g_spectatorSidecarDropLogged = true;
                    QLOG("[spectator-sidecar] missing pre/post proof frame=%u "
                         "record=%u; spectator service degrades, the player "
                         "session is unaffected",
                         confirmed.frame, g_spectatorSidecarRecord);
                }
            } else if (confirmed.frame != g_spectatorSidecarRecord) {
                qgame::spectator_publish::publishFault(
                    qgame::spectator_publish::PublishDisableReason::
                        InvalidRecord);
                if (!g_spectatorSidecarDropLogged) {
                    g_spectatorSidecarDropLogged = true;
                    QLOG("[spectator-sidecar] non-contiguous publication "
                         "record=%u frame=%u; spectator service degrades, the "
                         "player session is unaffected",
                         g_spectatorSidecarRecord, confirmed.frame);
                }
            } else {
                qnetplay::spectator::ConfirmedRecord sidecar{};
                sidecar.record = g_spectatorSidecarRecord;
                sidecar.timelineFrame = confirmed.frame;
                sidecar.playerOneInput = meta.canonicalActions[0];
                sidecar.playerTwoInput = meta.canonicalActions[1];
                sidecar.stateHash = meta.spectatorPreHash;
                spectatorSetCheckpointCaptureAtPublish(
                    confirmed.frame, meta);
                if (qgame::spectator_publish::publishConfirmed(sidecar)) {
                    ++g_spectatorSidecarRecord;
                    g_netSpectatorPostTailHash = meta.spectatorPostHash;
                    qgame::spectator_publish::publishTailProof(
                        g_spectatorSidecarRecord, meta.spectatorPostHash);
                } else if (!g_spectatorSidecarDropLogged) {
                    g_spectatorSidecarDropLogged = true;
                    QLOG("[spectator-sidecar] publication refused at frame=%u; "
                         "spectator service degrades, the player session is "
                         "unaffected", confirmed.frame);
                }
            }
        }

        if (g_netSpectatorArchiveBegun
            && !g_netSpectatorArchiveFailed) {
            if (confirmed.frame != g_netSpectatorRecord) {
                g_netSpectatorArchiveFailed = true;
                QLOG("[spectator-source] non-contiguous confirmed publication "
                     "record=%u frame=%u; player session unaffected",
                     g_netSpectatorRecord, confirmed.frame);
                netSealSpectatorArchive(
                    qnetplay::spectator::TerminalReason::ProtocolFailure);
            } else {
                qnetplay::spectator::ConfirmedRecord record{};
                record.record = g_netSpectatorRecord;
                record.timelineFrame = confirmed.frame;
                record.playerOneInput = meta.canonicalActions[0];
                record.playerTwoInput = meta.canonicalActions[1];
                record.stateHash = meta.spectatorPreHash;
                if (record.stateHash == 0 || meta.spectatorPostHash == 0) {
                    g_netSpectatorArchiveFailed = true;
                    QLOG("[spectator-source] missing pre/post proof frame=%u; "
                         "player session unaffected", confirmed.frame);
                    netSealSpectatorArchive(
                        qnetplay::spectator::TerminalReason::ProtocolFailure);
                    continue;
                }
                qnetplay::spectator::ConfirmedRecord stored{};
                const qnetplay::spectator::ArchiveResult archiveResult =
                    qgame::spectatorSourceAppendConfirmed(
                        g_netSpectatorGameId, record, &stored);
                if (archiveResult
                        == qnetplay::spectator::ArchiveResult::Ok
                    || archiveResult
                        == qnetplay::spectator::ArchiveResult::ExactDuplicate) {
                    ++g_netSpectatorRecord;
                    g_netSpectatorPostTailHash = meta.spectatorPostHash;
                } else {
                    g_netSpectatorArchiveFailed = true;
                    QLOG("[spectator-source] append failed result=%u frame=%u; "
                         "player session unaffected",
                         static_cast<unsigned>(archiveResult),
                         confirmed.frame);
                    netSealSpectatorArchive(
                        qnetplay::spectator::TerminalReason::ProtocolFailure);
                }
            }
        }

        if (meta.preHash && (confirmed.frame % 30u) == 0u) {
            const EpochRelation relation = relationToCurrent(
                meta.layoutGeneration, meta.epoch);
            if (relation == EpochRelation::Stale) {
                static uint64_t staleLocalHashes = 0;
                if (++staleLocalHashes <= 8u
                    || staleLocalHashes % 300u == 0u) {
                    QLOG("[netplay] suppressed newly confirmed stale local "
                         "hash layout=%u epoch=%u frame=%u current=%u",
                         meta.layoutGeneration, meta.epoch, meta.frame,
                         g_netEpoch);
                }
                continue;
            }
            if (relation != EpochRelation::Current) {
                netFailClosed(
                    kNetInternalFailure, confirmed.frame,
                    "local confirmed hash has inconsistent layout epoch");
                return false;
            }
            qgame::NetConfirmedHash hash{};
            hash.layoutGeneration = meta.layoutGeneration;
            hash.epoch = meta.epoch;
            hash.frame = meta.frame;
            hash.rngGameplay = meta.rngGameplay;
            hash.rngCosmetic = meta.rngCosmetic;
            hash.rngSelector = meta.rngSelector;
            hash.hp0 = meta.hp0;
            hash.hp1 = meta.hp1;
            hash.gameplayHash = meta.preHash;
            netWriteConfirmedCheckpoint(meta);
            if (!qgame::netSubmitConfirmedHash(hash)) {
                if (!netCheckLink(true))
                    return false;
                netFailClosed(kNetInternalFailure, confirmed.frame,
                              "transport rejected confirmed hash submission");
                return false;
            }
        }
    }

    qgame::NetConfirmedHash peer{};
    while (qgame::netTakeConfirmedHash(peer)) {
        const EpochRelation relation =
            relationToCurrent(peer.layoutGeneration, peer.epoch);
        if (relation == EpochRelation::Stale) {
            static uint64_t staleHashes = 0;
            if (++staleHashes <= 8u || staleHashes % 300u == 0u) {
                QLOG("[netplay] ignored delayed confirmed hash from "
                     "stale layout=%u epoch=%u frame=%u current=%u",
                     peer.layoutGeneration, peer.epoch, peer.frame,
                     g_netEpoch);
            }
            continue;
        }
        if (relation == EpochRelation::Inconsistent) {
            netFailClosed(kNetProtocolViolation, peer.frame,
                          "peer confirmed hash has inconsistent layout epoch");
            return false;
        }
        if (g_netPendingPeerHashes.size() >= 128u) {
            netFailClosed(kNetProtocolViolation, peer.frame,
                          "peer confirmed-hash queue exceeded its bound");
            return false;
        }
        g_netPendingPeerHashes.push_back(peer);
    }

    while (!g_netPendingPeerHashes.empty()) {
        const qgame::NetConfirmedHash& pending =
            g_netPendingPeerHashes.front();
        const EpochRelation relation = relationToCurrent(
            pending.layoutGeneration, pending.epoch);
        if (relation == EpochRelation::Stale) {
            g_netPendingPeerHashes.pop_front();
            continue;
        }
        if (relation == EpochRelation::Future)
            break;
        if (relation == EpochRelation::Inconsistent) {
            netFailClosed(kNetProtocolViolation, pending.frame,
                          "queued peer hash has inconsistent layout epoch");
            return false;
        }
        if (!g_netHasConfirmed || pending.frame > g_netConfirmedThrough)
            break;
        NetFrameMeta& local = g_netMeta[pending.frame % kNetMetaSlots];
        const bool match =
            local.valid
            && local.frame == pending.frame
            && local.layoutGeneration == pending.layoutGeneration
            && local.epoch == pending.epoch
            && local.preHash != 0
            && local.preHash == pending.gameplayHash
            && local.rngGameplay == pending.rngGameplay
            && local.rngCosmetic == pending.rngCosmetic
            && local.rngSelector == pending.rngSelector
            && local.hp0 == pending.hp0
            && local.hp1 == pending.hp1;
        if (!match) {
            netWriteConfirmedDesync(local, pending);
            netFailClosed(kNetConfirmedDesync, pending.frame,
                          "confirmed canonical state differs from peer");
            return false;
        }
        ++g_netConfirmedHashMatches;
        QLOG("[netplay] confirmed hash MATCH count=%llu layout=%u "
             "epoch=%u frame=%u hash=%016llX",
             static_cast<unsigned long long>(
                 g_netConfirmedHashMatches),
             pending.layoutGeneration, pending.epoch, pending.frame,
             static_cast<unsigned long long>(pending.gameplayHash));
        g_netPendingPeerHashes.pop_front();
    }
    return true;
}

struct NetLayoutDigestComponents {
    uint64_t audioManifest = 0;
    uint64_t state = 0;
    uint32_t phase = 0;
    uint16_t fpuControlWord = 0;
    uint32_t fpuMxcsr = 0;
    uint64_t fpu = 0;
    uint64_t virtualAudio = 0;
    uint64_t digest = 0;
};

void netLogVirtualAudioDeadlines(const char* const reason) {
    char text[320] = {};
    size_t written = 0;
    for (size_t i = 0; i < g_virtualAudio.statusUntil.size(); ++i) {
        if (written + 1u >= sizeof(text)) break;
        const uint64_t until = g_virtualAudio.statusUntil[i];
        char value[24] = {};
        if (until == kVirtualSoundLooping) {
            std::snprintf(value, sizeof(value), "loop");
        } else {
            std::snprintf(value, sizeof(value), "%llu",
                          static_cast<unsigned long long>(
                              until > g_ovrFrame ? until - g_ovrFrame : 0ull));
        }
        const int added = std::snprintf(
            text + written, sizeof(text) - written, "%s%d=%s",
            i == 0 ? "" : " ", qgame::kSimulationStatusSoundIds[i], value);
        if (added <= 0) break;
        written += static_cast<size_t>(added);
    }
    QLOG("[netplay] virtual audio deadlines (%s) frame=%llu remaining=[%s]",
         reason, static_cast<unsigned long long>(g_ovrFrame), text);
}

uint64_t netCurrentLayoutDigest(
    NetLayoutDigestComponents* components = nullptr,
    std::vector<RollbackManager::RegionHash>* perRegion = nullptr) {
    static constexpr char kDomain[] = "QOH99_POST_LOAD_LAYOUT_V2_NO_AUDIO";
    uint64_t digest = netFnv1a(kDomain, sizeof(kDomain) - 1u);
    const uint64_t state =
        g_rb.hashLive(perRegion, RollbackManager::Profile::Gameplay);
    digest = netFnv1a(&state, sizeof(state), digest);
    const uint32_t phase = netPhaseTag();
    digest = netFnv1a(&phase, sizeof(phase), digest);

    NetFpuSlot fpu{};
    fxSaveTo(fpu.state);
    uint16_t fpuControlWord = 0;
    uint32_t fpuMxcsr = 0;
    std::memcpy(
        &fpuControlWord, fpu.state, sizeof(fpuControlWord));
    std::memcpy(&fpuMxcsr, fpu.state + 24, sizeof(fpuMxcsr));
    const uint64_t fpuHash = netCanonicalFpuHash(fpu);
    digest = netFnv1a(&fpuHash, sizeof(fpuHash), digest);
    const uint64_t virtualAudioHash =
        netFnv1a(&g_virtualAudio, sizeof(g_virtualAudio));
    const uint64_t nonzeroDigest =
        digest != 0 ? digest : 0x514F4839394C4159ull;
    if (components) {
        components->audioManifest = netCanonicalAudioManifestHash(false);
        components->state = state;
        components->phase = phase;
        components->fpuControlWord = fpuControlWord;
        components->fpuMxcsr = fpuMxcsr;
        components->fpu = fpuHash;
        components->virtualAudio = virtualAudioHash;
        components->digest = nonzeroDigest;
    }
    return nonzeroDigest;
}

uintptr_t netRegionAddressByName(const char* name) {
    if (name == nullptr) return 0;
    for (const qoh99::Region& region : g_rb.fixedRegions()) {
        if (region.name != nullptr && std::strcmp(region.name, name) == 0)
            return region.addr;
    }
    return 0;
}

void netWriteLayoutDump(const char* reason,
                        const qgame::NetLayoutFingerprint* peer) {
    if (!g_sessionDir[0]) return;
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\layout_mismatch.txt",
                  g_sessionDir);
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) return;

    NetLayoutDigestComponents parts{};
    std::vector<RollbackManager::RegionHash> regions;
    const uint64_t digest = netCurrentLayoutDigest(&parts, &regions);

    std::fprintf(f, "# QOH99 layout dump\n");
    std::fprintf(f, "# reason=%s\n", reason ? reason : "unspecified");
    std::fprintf(f, "# Send this file to the other player and compare the two.\n");
    std::fprintf(f, "# Identical lines are agreement; any difference is a\n");
    std::fprintf(f, "# difference in the installed game, not in the network.\n");
    std::fprintf(f, "contract %s\n", QOH99_BUILD_CONTRACT_MARKER);
    std::fprintf(f, "digest %016llX\n",
                 static_cast<unsigned long long>(digest));
    std::fprintf(f, "component audioManifest %016llX\n",
                 static_cast<unsigned long long>(parts.audioManifest));
    std::fprintf(f, "component state %016llX\n",
                 static_cast<unsigned long long>(parts.state));
    std::fprintf(f, "component phase %08X\n", parts.phase);
    std::fprintf(f, "component fpu %016llX control=%04X mxcsr=%08X\n",
                 static_cast<unsigned long long>(parts.fpu),
                 parts.fpuControlWord, parts.fpuMxcsr);
    std::fprintf(f, "component virtualAudio %016llX\n",
                 static_cast<unsigned long long>(parts.virtualAudio));
    if (peer != nullptr) {
        std::fprintf(f, "peer digest %016llX\n",
                     static_cast<unsigned long long>(peer->digest));
        std::fprintf(f,
                     "peer components audio=%08X state=%08X phase=%08X "
                     "fpu=%08X mixer=%08X\n",
                     peer->audioManifest32, peer->state32, peer->phase32,
                     peer->fpu32, peer->virtualAudio32);
    }
    std::fprintf(f, "regions %zu\n", regions.size());
    for (size_t i = 0; i < regions.size(); ++i) {
        const RollbackManager::RegionHash& r = regions[i];
        std::fprintf(f, "region %-28s size=%-10zu present=%u hash=%016llX\n",
                     r.name ? r.name : "<unnamed>", r.size,
                     r.present ? 1u : 0u,
                     static_cast<unsigned long long>(r.hash));
        constexpr size_t kRawDumpLimit = 64;
        if (!r.present || r.size == 0) continue;
        const uintptr_t base = netRegionAddressByName(r.name);
        if (base == 0) continue;
        if (r.size <= kRawDumpLimit) {
            std::fprintf(f, "  bytes %-26s", r.name ? r.name : "<unnamed>");
            for (size_t off = 0; off < r.size; off += 4) {
                std::fprintf(f, " %08lX",
                             static_cast<unsigned long>(rd32(base + off)));
            }
            std::fprintf(f, "\n");
            continue;
        }
        constexpr size_t kChunkColumns = 32;
        size_t stride = (r.size + kChunkColumns - 1) / kChunkColumns;
        stride = (stride + 3u) & ~static_cast<size_t>(3u);
        if (stride < 4) stride = 4;
        std::fprintf(f, "  chunks %-25s stride=%zu",
                     r.name ? r.name : "<unnamed>", stride);
        for (size_t off = 0; off < r.size; off += stride) {
            const size_t span = (std::min)(stride, r.size - off);
            uint64_t chunk = 1469598103934665603ull;
            for (size_t i = 0; i < span; ++i) {
                chunk ^= static_cast<uint64_t>(rd8(base + off + i));
                chunk *= 1099511628211ull;
            }
            std::fprintf(f, " %08lX",
                         static_cast<unsigned long>(chunk & 0xFFFFFFFFull));
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    QLOG("[layout-dump] wrote %s (%zu regions)", path, regions.size());
}

void netLogLayoutDigest(const char* reason) {
    NetLayoutDigestComponents components{};
    std::vector<RollbackManager::RegionHash> regions;
    const uint64_t digest =
        netCurrentLayoutDigest(&components, &regions);
    QLOG("[layout-diagnostic] reason=%s digest=%016llX "
         "audioManifest=%016llX state=%016llX phase=%08X "
         "fpu=%016llX control=%04X mxcsr=%08X "
         "virtualAudio=%016llX regions=%zu",
         reason ? reason : "unspecified",
         static_cast<unsigned long long>(digest),
         static_cast<unsigned long long>(components.audioManifest),
         static_cast<unsigned long long>(components.state),
         components.phase,
         static_cast<unsigned long long>(components.fpu),
         components.fpuControlWord, components.fpuMxcsr,
         static_cast<unsigned long long>(components.virtualAudio),
         regions.size());
    for (size_t index = 0; index < regions.size(); ++index) {
        const RollbackManager::RegionHash& region = regions[index];
        QLOG("[layout-region] index=%zu name=%s size=%zu present=%u "
             "hash=%016llX",
             index, region.name ? region.name : "<unnamed>",
             region.size, region.present ? 1u : 0u,
             static_cast<unsigned long long>(region.hash));
    }
    const uint32_t activeConfig =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    const uint32_t bindingConfig =
        rd32(RollbackManager::kQohBindingPointerAddress);
    if (RollbackManager::validQohActiveConfigPointer(activeConfig)) {
        const uint32_t profile = static_cast<uint32_t>(
            (activeConfig - RollbackManager::kQohConfigRecordBase)
            / RollbackManager::kQohConfigRecordStride);
        RollbackManager::QohMatchConfigProof gameplay{};
        const bool proofValid = RollbackManager::qohMatchConfigProof(
            RollbackManager::kQohConfigRegionAddress,
            reinterpret_cast<const uint8_t*>(
                RollbackManager::kQohConfigRegionAddress),
            RollbackManager::kQohConfigRegionSize,
            gameplay);
        const uint8_t* activeBytes =
            reinterpret_cast<const uint8_t*>(
                static_cast<uintptr_t>(activeConfig));
        const uint64_t imageHash = netFnv1a(
            reinterpret_cast<const void*>(
                RollbackManager::kQohConfigImageAddress),
            RollbackManager::kQohConfigImageSize);
        const uint64_t sharedHeaderHash = netFnv1a(
            reinterpret_cast<const void*>(
                RollbackManager::kQohConfigSharedHeaderAddress),
            RollbackManager::kQohConfigSharedHeaderSize);
        const uint64_t sharedBlockHash = netFnv1a(
            reinterpret_cast<const void*>(
                RollbackManager::kQohConfigSharedBlockAddress),
            RollbackManager::kQohConfigSharedBlockSize);
        const uint64_t activePrefixHash = netFnv1a(
            activeBytes,
            RollbackManager::kQohConfigGameplayPrefixSize);
        const uint64_t pathHash =
            netFnv1a(activeBytes + 116u, 260u);
        const uint64_t bindingsHash = netFnv1a(
            activeBytes + RollbackManager::kQohConfigBindingOffset,
            RollbackManager::kQohConfigRecordStride
                - RollbackManager::kQohConfigBindingOffset);
        const uint64_t snapshotPrefixHash = netFnv1a(
            reinterpret_cast<const void*>(
                RollbackManager::kQohConfigSnapshotAddress),
            RollbackManager::kQohConfigGameplayPrefixSize);
        QLOG("[layout-config] valid=%u profile=%u active=%08X "
             "binding=%08X expectedBinding=%08X "
             "timerMode36=%u aiDifficulty40=%u "
             "lifeMultiplier44=%u roundsToWinMinusOne48=%u "
             "image=%016llX activePrefix=%016llX "
             "path=%016llX bindings=%016llX "
             "shared20=%016llX shared334=%016llX "
             "snapshotPrefix=%016llX "
             "local60=%u local64=%u local68=%u "
             "local84=%u local88=%u local92=%u local96=%u "
             "local108=%u local376=%08X",
             proofValid ? 1u : 0u, profile, activeConfig,
             bindingConfig,
             RollbackManager::expectedQohBindingPointer(activeConfig),
             gameplay.timerMode, gameplay.aiDifficulty,
             gameplay.lifeMultiplier,
             gameplay.roundsToWinMinusOne,
             static_cast<unsigned long long>(imageHash),
             static_cast<unsigned long long>(activePrefixHash),
             static_cast<unsigned long long>(pathHash),
             static_cast<unsigned long long>(bindingsHash),
             static_cast<unsigned long long>(sharedHeaderHash),
             static_cast<unsigned long long>(sharedBlockHash),
             static_cast<unsigned long long>(snapshotPrefixHash),
             RollbackManager::readCanonicalU32(activeBytes, 60u),
             RollbackManager::readCanonicalU32(activeBytes, 64u),
             RollbackManager::readCanonicalU32(activeBytes, 68u),
             RollbackManager::readCanonicalU32(activeBytes, 84u),
             RollbackManager::readCanonicalU32(activeBytes, 88u),
             RollbackManager::readCanonicalU32(activeBytes, 92u),
             RollbackManager::readCanonicalU32(activeBytes, 96u),
             RollbackManager::readCanonicalU32(activeBytes, 108u),
             RollbackManager::readCanonicalU32(activeBytes, 376u));
    } else {
        QLOG("[layout-config] valid=0 active=%08X binding=%08X",
             activeConfig, bindingConfig);
    }
    QLOG("[layout-audio] statusUntil="
         "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[0]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[1]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[2]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[3]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[4]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[5]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[6]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[7]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[8]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[9]),
         static_cast<unsigned long long>(
             g_virtualAudio.statusUntil[10]));
}

enum class NetLayoutEpochRelation {
    Current,
    Stale,
    Future,
    Inconsistent,
};

NetLayoutEpochRelation netClassifyLayoutEpoch(
    uint32_t layoutGeneration, uint32_t epoch,
    uint32_t currentGeneration, uint32_t currentEpoch) {
    if (layoutGeneration == currentGeneration && epoch == currentEpoch)
        return NetLayoutEpochRelation::Current;
    const auto serialAfter = [](uint32_t a, uint32_t b) {
        const uint32_t distance = a - b;
        return distance != 0 && distance < 0x80000000u;
    };
    if (serialAfter(currentGeneration, layoutGeneration)
        && serialAfter(currentEpoch, epoch)) {
        return NetLayoutEpochRelation::Stale;
    }
    if (serialAfter(layoutGeneration, currentGeneration)
        && serialAfter(epoch, currentEpoch)) {
        return NetLayoutEpochRelation::Future;
    }
    return NetLayoutEpochRelation::Inconsistent;
}

bool netApplyLayoutCommandHistoryTestMutation(uint32_t localGeneration) {
    if (g_netLayoutTestMutationGeneration == localGeneration) return true;

    char value[64] = {};
    const DWORD length = GetEnvironmentVariableA(
        "QOH99_NP_TEST_LAYOUT_COMMAND_HISTORY", value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) return true;

    static constexpr char kInactiveP4[] = "live-e2e:inactive-p4";
    static constexpr char kActiveP2[] = "live-e2e:active-p2";
    const bool mutateInactiveP4 = std::strcmp(value, kInactiveP4) == 0;
    const bool mutateActiveP2 = std::strcmp(value, kActiveP2) == 0;
    if (!mutateInactiveP4 && !mutateActiveP2) return true;

    // 0x62369C is the applied VS formation row: sub_432EE0 writes 1 for 1v1 rows, 2 for simultaneous 2v2 rows; 0 means no row applied yet.
    if (mutateInactiveP4
        && (rd32(addr::kVsFormationRow) > 1u
            || rd32(addr::kControlMask) != 0u)) {
        netFailClosed(
            kNetInternalFailure, localGeneration,
            "inactive-P4 layout test hook requires a two-human 1v1 match");
        return false;
    }
    const uintptr_t fighter = mutateInactiveP4
        ? static_cast<uintptr_t>(addr::kFighterP4)
        : static_cast<uintptr_t>(addr::kFighterP2);
    static constexpr size_t kCommandChildPointerOffset = 164u;
    static constexpr size_t kCommandHistoryBytes = 0x200u;
    static constexpr size_t kMutationByteOffset = 8u + 2u;
    if (!rangeReadable(
            fighter + kCommandChildPointerOffset, sizeof(uint32_t))) {
        netFailClosed(
            kNetInternalFailure, localGeneration,
            "layout command-history test owner is unreadable");
        return false;
    }
    const uintptr_t child = static_cast<uintptr_t>(
        rd32(fighter + kCommandChildPointerOffset));
    if (child == 0
        || !rangeReadable(child, kCommandHistoryBytes)
        || !pageWritable(reinterpret_cast<const void*>(
            child + kMutationByteOffset))) {
        netFailClosed(
            kNetInternalFailure, localGeneration,
            "layout command-history test child is unavailable");
        return false;
    }

    uint8_t* const byte = reinterpret_cast<uint8_t*>(
        child + kMutationByteOffset);
    const uint64_t rawBefore =
        netFnv1a(reinterpret_cast<const void*>(child), kCommandHistoryBytes);
    const uint8_t before = *byte;
    *byte = static_cast<uint8_t>(before ^ 0x01u);
    const uint8_t after = *byte;
    const uint64_t rawAfter =
        netFnv1a(reinterpret_cast<const void*>(child), kCommandHistoryBytes);
    if (before == after || rawBefore == rawAfter) {
        netFailClosed(
            kNetInternalFailure, localGeneration,
            "layout command-history test mutation did not change state");
        return false;
    }

    g_netLayoutTestMutationGeneration = localGeneration;
    QLOG("[test-hook] layout command-history mutation generation=%u "
         "target=%s fighter=%08X child=%08X offset=%zu "
         "before=%02X after=%02X rawBefore=%016llX rawAfter=%016llX",
         localGeneration,
         mutateInactiveP4 ? "inactive-p4" : "active-p2",
         static_cast<unsigned>(fighter),
         static_cast<unsigned>(child),
         kMutationByteOffset,
         static_cast<unsigned>(before), static_cast<unsigned>(after),
         static_cast<unsigned long long>(rawBefore),
         static_cast<unsigned long long>(rawAfter));
    return true;
}

bool netLayoutBarrierReady() {
    const uint32_t localGeneration = g_rb.layoutGeneration();
    const uint32_t generation = g_netEpoch;
    const uint32_t epoch = g_netEpoch;
    if (localGeneration == 0 || generation == 0 || epoch == 0) {
        netFailClosed(kNetInternalFailure, localGeneration,
                      "post-load layout barrier has an invalid generation");
        return false;
    }

    if (!netApplyLayoutCommandHistoryTestMutation(localGeneration))
        return false;
    const uint64_t currentDigest = netCurrentLayoutDigest();
    if (currentDigest == 0) {
        netFailClosed(kNetInvalidConfiguration, 0,
                      "canonical audio manifest is incomplete");
        return false;
    }
    if (g_netLayoutBarrier.layoutGeneration != localGeneration
        || g_netLayoutBarrier.epoch != epoch) {
        g_netLayoutBarrier = NetLayoutBarrierState{};
        g_netLayoutBarrier.layoutGeneration = localGeneration;
        g_netLayoutBarrier.epoch = epoch;
        g_netLayoutBarrier.digest = currentDigest;
        g_netLayoutBarrier.startedMs = GetTickCount64();
        netLogLayoutDigest("first-submission");
    } else if (g_netLayoutBarrier.digest != currentDigest) {
        netFailClosed(
            kNetInternalFailure,
            static_cast<uint32_t>(
                g_netLayoutBarrier.digest ^ currentDigest),
            "canonical gameplay state changed during layout rendezvous");
        return false;
    }

    if (!netPublishAndVerifyConfirmed()) return false;

    if (!g_netLayoutBarrier.submitted) {
        qgame::NetLayoutFingerprint local{};
        local.layoutGeneration = generation;
        local.epoch = epoch;
        local.digest = g_netLayoutBarrier.digest;
        {
            NetLayoutDigestComponents parts{};
            netCurrentLayoutDigest(&parts, nullptr);
            local.audioManifest32 =
                static_cast<uint32_t>(parts.audioManifest);
            local.state32 = static_cast<uint32_t>(parts.state);
            local.phase32 = parts.phase;
            local.fpu32 = static_cast<uint32_t>(parts.fpu);
            local.virtualAudio32 =
                static_cast<uint32_t>(parts.virtualAudio);
        }
        if (qgame::netSubmitLayoutFingerprint(local)) {
            g_netLayoutBarrier.submitted = true;
            QLOG("[netplay] layout rendezvous submitted generation=%u "
                 "epoch=%u localGeneration=%u digest=%016llX "
                 "assets=%016llX",
                 generation, epoch, localGeneration,
                 static_cast<unsigned long long>(local.digest),
                 static_cast<unsigned long long>(
                     g_rb.immutableAssetFingerprint()));
        }
    }

    qgame::NetLayoutFingerprint remote{};
    while (qgame::netPeekLayoutFingerprint(remote)) {
        const NetLayoutEpochRelation relation =
            netClassifyLayoutEpoch(
                remote.layoutGeneration, remote.epoch,
                generation, epoch);
        if (relation == NetLayoutEpochRelation::Future) {
            break;
        }
        qgame::NetLayoutFingerprint consumed{};
        if (!qgame::netTakeLayoutFingerprint(consumed)
            || consumed.layoutGeneration != remote.layoutGeneration
            || consumed.epoch != remote.epoch
            || consumed.digest != remote.digest) {
            netFailClosed(kNetInternalFailure, generation,
                          "layout fingerprint queue changed during consume");
            return false;
        }
        if (relation == NetLayoutEpochRelation::Stale) {
            QLOG("[netplay] ignored delayed stale layout rendezvous "
                 "generation=%u epoch=%u current=[%u,%u]",
                 remote.layoutGeneration, remote.epoch,
                 generation, epoch);
            continue;
        }
        if (relation == NetLayoutEpochRelation::Inconsistent
            || remote.digest != g_netLayoutBarrier.digest) {
            netLogLayoutDigest("peer-mismatch");

            qgame::NetLayoutFingerprint localPrint{};
            localPrint.digest = g_netLayoutBarrier.digest;
            {
                NetLayoutDigestComponents parts{};
                netCurrentLayoutDigest(&parts, nullptr);
                localPrint.audioManifest32 =
                    static_cast<uint32_t>(parts.audioManifest);
                localPrint.state32 = static_cast<uint32_t>(parts.state);
                localPrint.phase32 = parts.phase;
                localPrint.fpu32 = static_cast<uint32_t>(parts.fpu);
                localPrint.virtualAudio32 =
                    static_cast<uint32_t>(parts.virtualAudio);
            }
            const qgame::NetLayoutMismatchExplanation differing =
                qgame::netLayoutComponentMismatch(localPrint, remote);
            netWriteLayoutDump("peer-mismatch", &remote);

            QLOG("[netplay] layout rendezvous mismatch local=[%u,%u,%016llX] "
                 "peer=[%u,%u,%016llX] differing=%s",
                 generation, epoch,
                 static_cast<unsigned long long>(
                     g_netLayoutBarrier.digest),
                 remote.layoutGeneration, remote.epoch,
                 static_cast<unsigned long long>(remote.digest),
                 differing.component != nullptr ? differing.component
                                                : "a hashed memory region");
            QLOG("[netplay] layout components local=[audio=%08X state=%08X "
                 "phase=%08X fpu=%08X mixer=%08X] "
                 "peer=[audio=%08X state=%08X phase=%08X fpu=%08X mixer=%08X]",
                 localPrint.audioManifest32, localPrint.state32,
                 localPrint.phase32, localPrint.fpu32,
                 localPrint.virtualAudio32,
                 remote.audioManifest32, remote.state32, remote.phase32,
                 remote.fpu32, remote.virtualAudio32);

            if (localPrint.virtualAudio32 != remote.virtualAudio32
                && localPrint.audioManifest32 == remote.audioManifest32
                && localPrint.state32 == remote.state32
                && localPrint.phase32 == remote.phase32) {
                netLogVirtualAudioDeadlines("layout-mismatch");
            }

            char why[192];
            std::snprintf(why, sizeof(why), "%s (%s)",
                          differing.playerFacing,
                          differing.component != nullptr
                              ? differing.component
                              : "a hashed memory region");
            netFailClosed(
                kNetInvalidConfiguration,
                static_cast<uint32_t>(
                    remote.digest ^ g_netLayoutBarrier.digest),
                why);
            return false;
        }
        g_netLayoutBarrier.remoteMatched = true;
    }

    const bool acknowledged =
        g_netLayoutBarrier.submitted
        && qgame::netLayoutFingerprintAcknowledged(generation, epoch);
    if (g_netLayoutBarrier.remoteMatched && acknowledged) {
        QLOG("[netplay] layout rendezvous complete generation=%u "
             "epoch=%u localGeneration=%u digest=%016llX",
             generation, epoch, localGeneration,
             static_cast<unsigned long long>(
                 g_netLayoutBarrier.digest));
        return true;
    }

    if (GetTickCount64() - g_netLayoutBarrier.startedMs > 30000u) {
        netWriteLayoutDump("peer-timeout", nullptr);
        netFailClosed(kNetTimeout, generation,
                      "post-load layout rendezvous timed out");
    }
    return false;
}

bool netInitializeRollbackSession() {
    qnetplay::RollbackConfig config{};
    config.localPlayer = static_cast<uint8_t>(g_netLocalCtrl);
    config.inputDelayFrames = static_cast<uint8_t>(g_netDelay);
    config.maxRollbackFrames = static_cast<uint8_t>(g_netRollback);
    config.neutralInput = 0;
    config.firstFrame = 0;
    config.maxRemoteFutureFrames = static_cast<uint16_t>(
        (std::max)(32u, (g_netRemoteDelay + g_netRollback) * 2u + 16u));
    config.historyCapacity = kNetMetaSlots;
    g_netSession.reset(new (std::nothrow) qnetplay::RollbackSession(config));
    if (!g_netSession || !g_netSession->valid()) {
        netFailClosed(kNetInvalidConfiguration, g_netRollback,
                      "rollback session configuration/allocation failed");
        return false;
    }
    g_netRemoteIngest = 0;
    g_netFrame = 0;
    g_netEpoch = 1;
    netPaletteResetForSession();
    g_netTransitionBlocked = qgame::kNetNoFrame;
    g_netPendingPeerHashes.clear();
    g_netHasConfirmed = false;
    g_netConfirmedThrough = 0;
    g_netConfirmedHashMatches = 0;
    g_netGameplaySegmentActive = false;
    g_netGameplaySegmentSeen = false;
    g_netGameplayRefreshPending = false;
    g_netDelayDrain.reset();
    g_netLayoutBarrier = NetLayoutBarrierState{};
    g_netLayoutTestMutationGeneration = 0;
    g_netFpuInitializedEpoch = 0;
    netInvalidateFpu();
    for (NetFrameMeta& meta : g_netMeta) meta = NetFrameMeta{};
    netResetPacingTelemetry();
    return netFlushLocalSubmissions();
}

uint32_t netRefreshFrontendDelay() {
    constexpr int64_t kFrontendDelayRiseUs = 150000;
    constexpr int64_t kFrontendDelayDecayUs = 300000;
    constexpr uint32_t kFrontendDelayMinSamples = 12u;
    qgame::FrontendDelayLatch& latch = g_netFrontendDelayLatch;

    qgame::NetPingStats ping{};
    uint32_t rttMicros = 0;
    bool windowUsable = false;
    if (qgame::netPeerPingStats(ping)) {
        rttMicros = ping.p95Micros;
        windowUsable = ping.samples >= kFrontendDelayMinSamples;
    } else {
        rttMicros = qgame::netPeerRttMicros();
    }
    const uint32_t baseline = (std::max)(1u, static_cast<uint32_t>(g_netDelay));
    const uint32_t target = static_cast<uint32_t>(
        qgame::frontendDelayForRttMicros(rttMicros,
                                         static_cast<uint8_t>(g_netDelay)));
    const int64_t freq = g_qpcFreq > 0 ? g_qpcFreq : 10000000;
    const auto ticksFor = [freq](const int64_t micros) {
        return (freq * micros) / 1000000;
    };
    const uint32_t before = latch.frames;
    g_netFrontendDelay = latch.step(windowUsable, target, baseline,
                                    netUnhookedQpcNow(),
                                    ticksFor(kFrontendDelayRiseUs),
                                    ticksFor(kFrontendDelayDecayUs));
    if (latch.frames != before) {
        QLOG("[frontend-delay] %s to %u frames (rtt p95 %u us wants %u, "
             "%u samples; %lld ms per frame)",
             latch.frames > before ? "raised" : "eased", latch.frames,
             rttMicros, target, ping.samples,
             (long long)((latch.frames > before ? kFrontendDelayRiseUs
                                                : kFrontendDelayDecayUs)
                         / 1000));
    }
    return g_netFrontendDelay;
}

bool netProcessOwnsForegroundWindow() {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}

bool netRequestGameplayDelay(uint8_t requested, const char* source) {
    const uint8_t previous = static_cast<uint8_t>(g_netDelay);
    if (requested == previous) {
        QLOG("[netplay] gameplay-delay request from %s was already active "
             "frame=%u delay=%u",
             source, g_netSession ? g_netSession->currentFrame() : g_netFrame,
             previous);
        return false;
    }
    g_netDelay = requested;
    const bool gameplay = rollbackEligible();
    if (!gameplay)
        g_netGameplayDelayApplied = requested;
    const bool pending = g_netDelayDrain.request(
        requested,
        static_cast<uint8_t>(g_netGameplayDelayApplied),
        GetTickCount64(), gameplay);
    qgame::netSetLocalGameplayDelayStatus(
        requested,
        static_cast<uint8_t>(g_netGameplayDelayApplied),
        pending);
    QLOG("[netplay] gameplay-delay requested by %s frame=%u "
         "requested=%u->%u applied=%u "
         "(peer-local; %s confirmed-boundary drain)",
         source,
         g_netSession ? g_netSession->currentFrame() : g_netFrame,
         previous, requested, g_netGameplayDelayApplied,
         pending ? "starts/retargets" : gameplay ? "cancels" : "skips frontend");
    return true;
}

inline uint32_t netRouteRequiredCoverageFrames() {
    qgame::NetPingStats ping{};
    uint32_t rttMicros = 0;
    if (qgame::netPeerPingStats(ping) && ping.samples >= 8u)
        rttMicros = ping.p95Micros;
    else if (qgame::netPeerHasRtt())
        rttMicros = qgame::netPeerRttMicros();
    if (rttMicros == 0u) return 0u;
    return static_cast<uint32_t>(qnet::requiredCoverageForRoute(
        qnet::RouteRttMs{
            (rttMicros + 500u) / 1000u}));
}

inline uint32_t netRouteLiveFloorFrames() {
    qgame::NetPingStats ping{};
    uint32_t rttMicros = 0;
    if (qgame::netPeerPingStats(ping) && ping.samples >= 8u)
        rttMicros = ping.meanMicros;
    else if (qgame::netPeerHasRtt())
        rttMicros = qgame::netPeerRttMicros();
    if (rttMicros == 0u) return 0u;
    return static_cast<uint32_t>(qnet::oneWayFrames(
        qnet::RouteRttMs{(rttMicros + 500u) / 1000u}));
}

inline uint8_t netMinimumLocalDelayForRoute() {
    const uint32_t required = netRouteLiveFloorFrames();
    if (required == 0u) return qgame::kMinimumGameplayDelay;
    const uint32_t peerRollback = g_netRemoteRollback;
    const uint32_t floorFrames =
        required > peerRollback ? required - peerRollback : 0u;
    return static_cast<uint8_t>(
        floorFrames > qgame::kMaximumGameplayDelay
            ? qgame::kMaximumGameplayDelay
            : floorFrames);
}

inline uint8_t netMinimumLocalRollbackForRoute() {
    const uint32_t required = netRouteLiveFloorFrames();
    if (required == 0u) return qgame::kMinimumGameplayRollback;
    const uint32_t peerDelay = g_netRemoteDelay;
    const uint32_t floorFrames =
        required > peerDelay ? required - peerDelay : 0u;
    const uint32_t clamped =
        floorFrames < qgame::kMinimumGameplayRollback
            ? qgame::kMinimumGameplayRollback
            : floorFrames;
    return static_cast<uint8_t>(
        clamped > qgame::kMaximumGameplayRollback
            ? qgame::kMaximumGameplayRollback
            : clamped);
}

static uint32_t g_netRollbackRequestPending = 0;

bool netRequestGameplayRollback(uint8_t requested, const char* source) {
    if (!g_netSession) return false;
    const uint8_t previous = static_cast<uint8_t>(g_netRollback);
    const uint32_t frame = g_netSession->currentFrame();
    if (requested == previous) {
        QLOG("[netplay] rollback-window request from %s was already active "
             "frame=%u rollback=%u", source, frame, previous);
        return false;
    }
    const qnetplay::InputDelayChangeResult result =
        g_netSession->tryChangeMaxRollback(requested);
    if (result == qnetplay::InputDelayChangeResult::Invalid) {
        QLOG("[netplay] rollback-window request REFUSED from %s frame=%u "
             "requested=%u blocker=%s (history capacity cannot hold "
             "delay+rollback+2)", source, frame, requested,
             qnetplay::inputDelayChangeBlockerName(
                 g_netSession->maxRollbackChangeBlocker(requested)));
        return false;
    }
    if (result == qnetplay::InputDelayChangeResult::Deferred) {
        g_netRollbackRequestPending = requested;
        QLOG("[netplay] rollback-window request DEFERRED from %s frame=%u "
             "requested=%u applied=%u blocker=%s (lowering below live "
             "speculation; retried each opportunity)",
             source, frame, requested, previous,
             qnetplay::inputDelayChangeBlockerName(
                 g_netSession->maxRollbackChangeBlocker(requested)));
        return false;
    }
    g_netRollback = requested;
    g_netRollbackRequestPending = 0;
    QLOG("[netplay] rollback-window APPLIED by %s frame=%u %u->%u "
         "(peer-local, immediate; protects THIS peer's coverage)",
         source, frame, previous, requested);
    return true;
}

void netServiceDeferredRollbackRequest() {
    if (g_netRollbackRequestPending == 0u || !g_netSession) return;
    const uint8_t want =
        static_cast<uint8_t>(g_netRollbackRequestPending);
    if (want == static_cast<uint8_t>(g_netRollback)) {
        g_netRollbackRequestPending = 0;
        return;
    }
    if (g_netSession->tryChangeMaxRollback(want)
        == qnetplay::InputDelayChangeResult::Applied) {
        QLOG("[netplay] rollback-window APPLIED (deferred) frame=%u %u->%u",
             g_netSession->currentFrame(),
             static_cast<unsigned>(g_netRollback), want);
        g_netRollback = want;
        g_netRollbackRequestPending = 0;
    }
}

qinput::ModeControlEvaluation netEvaluateOnlineModeControls() {
    if (g_modeControlEvaluationIteration == g_inputOuterIteration)
        return g_modeControlEvaluation;
    netRefreshInputBundle();
    qinput::KeyboardView keyboard{};
    keyboard.keyBits = g_inputBundle.keyboard.keyBits;
    keyboard.healthy = bundleKeyboardHealthy();
    keyboard.generation = g_inputBundle.keyboard.deviceGeneration;
    keyboard.sampleSerial = g_inputBundle.keyboard.sampleSerial;
    qinput::ControllerView controller{};
    if (g_inputControllerWorker != nullptr && g_inputBundle.controllerRead) {
        controller.probes = g_inputBundle.controller.probe;
        controller.probeCount = g_inputLaunchConfig.probeCount;
        controller.healthy =
            g_inputBundle.controller.status
                == static_cast<uint32_t>(qinput::SnapshotStatus::Connected)
            && qinput::snapshotAgeMs(g_inputBundle.nowMs,
                                     g_inputBundle.controller.sampleTickMs)
                   < kInputStaleThresholdMs;
        controller.generation = g_inputBundle.controller.deviceGeneration;
        controller.sampleSerial = g_inputBundle.controller.sampleSerial;
    }
    qinput::FocusView focus{g_inputBundle.focusActive,
                            g_inputBundle.focusGeneration};
    uint64_t enabled = 0;
    if (g_mode == RunMode::Netplay && g_netActive && g_netRunning
        && !g_netSpectatorMode && !g_replayActive) {
        enabled |= qinput::modeControlBit(
            qinput::ModeControlAction::LeaveMatch);
        enabled |= qinput::modeControlBit(
            qinput::ModeControlAction::OnlineReturnCharacterSelect);
        if (qgame::runtimeDelayControlsAllowed(
                netRollbackGameplayPolicyActive(), rd32(kSceneStateAddr))
            && g_inputBundle.focusActive) {
            enabled |= qinput::modeControlBit(
                qinput::ModeControlAction::InputDelayDecrease);
            enabled |= qinput::modeControlBit(
                qinput::ModeControlAction::InputDelayIncrease);
            enabled |= qinput::modeControlBit(
                qinput::ModeControlAction::RollbackWindowDecrease);
            enabled |= qinput::modeControlBit(
                qinput::ModeControlAction::RollbackWindowIncrease);
        }
    }
    g_modeControlEvaluation = g_modeControlEvaluator.evaluate(
        keyboard, controller, focus, g_inputLaunchConfig.focusPolicy, enabled);
    g_modeControlEvaluationIteration = g_inputOuterIteration;
    return g_modeControlEvaluation;
}

void netPollGameplayDelayHotkeys() {
    ++g_inputOuterIteration;
    const qinput::ModeControlEvaluation controls =
        netEvaluateOnlineModeControls();

    const bool rollbackDown = (controls.edges & qinput::modeControlBit(
        qinput::ModeControlAction::RollbackWindowDecrease)) != 0;
    const bool rollbackUp = (controls.edges & qinput::modeControlBit(
        qinput::ModeControlAction::RollbackWindowIncrease)) != 0;
    const int rollbackDirection = rollbackDown == rollbackUp
        ? 0 : rollbackDown ? -1 : 1;
    if (rollbackDirection != 0) {
        const uint8_t previousRollback =
            static_cast<uint8_t>(g_netRollback);
        const uint8_t wantRollback =
            qgame::adjustedGameplayRollback(previousRollback,
                                            rollbackDirection);
        const uint8_t rollbackFloor = netMinimumLocalRollbackForRoute();
        if (rollbackDirection < 0 && wantRollback < rollbackFloor) {
            QLOG("[netplay] rollback-window hotkey REFUSED frame=%u %u->%u: "
                 "an input cannot cross this route in under %u frames and the "
                 "peer is only sending %u of delay, so this peer must keep at "
                 "least %u of rollback (setup target was %u)",
                 g_netSession ? g_netSession->currentFrame() : g_netFrame,
                 previousRollback, wantRollback,
                 netRouteLiveFloorFrames(), g_netRemoteDelay,
                 rollbackFloor, netRouteRequiredCoverageFrames());
        } else if (wantRollback == previousRollback) {
            QLOG("[netplay] rollback-window hotkey ignored at %s limit=%u",
                 rollbackDirection < 0 ? "minimum" : "maximum",
                 previousRollback);
        } else {
            netRequestGameplayRollback(
                 wantRollback, "configured mode control");
        }
    }
    netServiceDeferredRollbackRequest();

    const bool delayDown = (controls.edges & qinput::modeControlBit(
        qinput::ModeControlAction::InputDelayDecrease)) != 0;
    const bool delayUp = (controls.edges & qinput::modeControlBit(
        qinput::ModeControlAction::InputDelayIncrease)) != 0;
    const int direction = delayDown == delayUp ? 0 : delayDown ? -1 : 1;
    if (direction == 0) return;

    const uint8_t previous = static_cast<uint8_t>(g_netDelay);
    const uint8_t requested =
        qgame::adjustedGameplayDelay(previous, direction);
    const uint8_t delayFloor = netMinimumLocalDelayForRoute();
    if (direction < 0 && requested < delayFloor) {
        QLOG("[netplay] gameplay-delay hotkey REFUSED frame=%u %u->%u: "
             "an input cannot cross this route in under %u frames and the peer "
             "only has %u of rollback, so lowering this peer's delay would "
             "underbuffer the OPPONENT (floor=%u, setup target was %u)",
             g_netSession ? g_netSession->currentFrame() : g_netFrame,
             previous, requested, netRouteLiveFloorFrames(),
             g_netRemoteRollback, delayFloor,
             netRouteRequiredCoverageFrames());
        return;
    }
    if (requested == previous) {
        QLOG("[netplay] gameplay-delay hotkey ignored at %s limit=%u",
             direction < 0 ? "minimum" : "maximum", previous);
        return;
    }
    netRequestGameplayDelay(
        requested, "configured mode control");
}

void netPollTestGameplayDelayScript() {
    if (!g_netTestDelayScriptEnabled || !g_netSession
        || !netRollbackGameplayPolicyActive()
        || g_netTestDelayNextStep >= g_netTestDelayStepCount) {
        return;
    }
    const uint32_t frame = g_netSession->currentFrame();
    if (frame == g_netTestDelayLastFrame) return;
    g_netTestDelayLastFrame = frame;
    ++g_netTestDelayGameplayTicks;

    while (g_netTestDelayNextStep < g_netTestDelayStepCount
           && g_netTestDelaySteps[g_netTestDelayNextStep].gameplayTick
                  <= g_netTestDelayGameplayTicks) {
        const NetTestDelayStep step =
            g_netTestDelaySteps[g_netTestDelayNextStep++];
        QLOG("[test-delay] REQUEST local=P%d gameplayTick=%u frame=%u "
             "target=%u step=%zu/%zu",
             g_netLocalCtrl + 1, g_netTestDelayGameplayTicks, frame,
             step.delay, g_netTestDelayNextStep,
             g_netTestDelayStepCount);
        if (netRequestGameplayDelay(step.delay, "live-E2E delay script"))
            g_netTestDelayPendingTarget = step.delay;
    }
}

void netMarkTestGameplayDelayApplied(uint32_t applied) {
    if (!g_netTestDelayScriptEnabled
        || g_netTestDelayPendingTarget != applied) {
        return;
    }
    QLOG("[test-delay] APPLIED local=P%d gameplayTick=%u frame=%u "
         "delay=%u",
         g_netLocalCtrl + 1, g_netTestDelayGameplayTicks,
         g_netSession ? g_netSession->currentFrame() : g_netFrame,
         applied);
    g_netTestDelayPendingTarget = UINT32_MAX;
}

bool netApplyPhaseInputDelay() {
    if (!g_netSession) return false;
    const bool gameplay = rollbackEligible();
    if (!gameplay && g_netDelayDrain.active()) {
        QLOG("[netplay] gameplay-delay drain canceled by phase exit "
             "frame=%u target=%u retained=%u",
             g_netSession->currentFrame(), g_netDelayDrain.target(),
             g_netGameplayDelayApplied);
        g_netDelay = g_netGameplayDelayApplied;
        g_netDelayDrain.reset();
        qgame::netSetLocalGameplayDelayStatus(
            static_cast<uint8_t>(g_netDelay),
            static_cast<uint8_t>(g_netGameplayDelayApplied),
            false);
    }
    const uint32_t desired = gameplay
        ? g_netDelay
        : netRefreshFrontendDelay();
    const uint32_t applied =
        g_netSession->config().inputDelayFrames;
    if (desired == applied) {
        if (gameplay) {
            if (g_netDelayDrain.active()) {
                QLOG("[netplay] gameplay-delay drain APPLIED frame=%u "
                     "delay=%u waited=%llums stableThrough=%u",
                     g_netSession->currentFrame(), desired,
                     static_cast<unsigned long long>(
                         g_netDelayDrain.elapsedMs(GetTickCount64())),
                     g_netSession->stableConfirmedThrough());
                g_netDelayDrain.reset();
            }
            g_netGameplayDelayApplied = applied;
            qgame::netSetLocalGameplayDelayStatus(
                static_cast<uint8_t>(g_netDelay),
                static_cast<uint8_t>(g_netGameplayDelayApplied),
                false);
            netMarkTestGameplayDelayApplied(applied);
        }
        return true;
    }

    const qnetplay::InputDelayChangeResult change =
        g_netSession->tryChangeInputDelayAtConfirmedBoundary(
            static_cast<uint8_t>(desired));
    if (change == qnetplay::InputDelayChangeResult::Deferred) {
        const uint64_t now = GetTickCount64();
        if (g_netDeferredDelayTarget != desired
            || now - g_netDeferredDelayLastLogMs >= 1000u) {
            const qnetplay::InputDelayChangeBlocker blocker =
                g_netSession->inputDelayChangeBlocker(
                    static_cast<uint8_t>(desired));
            const qnetplay::RollbackDiagnostics diag =
                g_netSession->diagnostics();
            QLOG("[netplay] input-delay policy deferred frame=%u "
                 "phase=%s applied=%u desired=%u blocker=%s(%u) "
                 "confirmedNext=%u hasConfirmed=%u",
                 g_netSession->currentFrame(),
                 gameplay
                     ? "rollback-gameplay"
                     : "exact-frontend",
                 applied, desired,
                 qnetplay::inputDelayChangeBlockerName(blocker),
                 static_cast<unsigned>(blocker),
                 diag.nextConfirmedFrame,
                 diag.hasStableConfirmedThrough ? 1u : 0u);
            g_netDeferredDelayTarget = desired;
            g_netDeferredDelayLastLogMs = now;
        }
        if (gameplay) {
            if (g_netDelayDrain.active()
                && g_netDelayDrain.timedOut(now)) {
                const uint8_t canceled = g_netDelayDrain.target();
                const uint64_t waited =
                    g_netDelayDrain.elapsedMs(now);
                g_netDelay = g_netGameplayDelayApplied;
                g_netDelayDrain.reset();
                qgame::netSetLocalGameplayDelayStatus(
                    static_cast<uint8_t>(g_netDelay),
                    static_cast<uint8_t>(g_netGameplayDelayApplied),
                    false);
                QLOG("[netplay] gameplay-delay drain CANCELED frame=%u "
                     "target=%u retained=%u waited=%llums "
                     "stableThrough=%u",
                     g_netSession->currentFrame(), canceled,
                     g_netGameplayDelayApplied,
                     static_cast<unsigned long long>(waited),
                     g_netSession->stableConfirmedThrough());
                return true;
            }
            qgame::netSetLocalGameplayDelayStatus(
                static_cast<uint8_t>(g_netDelay),
                static_cast<uint8_t>(g_netGameplayDelayApplied),
                true);
        }
        return true;
    }
    if (change == qnetplay::InputDelayChangeResult::Invalid) {
        const qnetplay::InputDelayChangeBlocker blocker =
            g_netSession->inputDelayChangeBlocker(
                static_cast<uint8_t>(desired));
        QLOG("[netplay] input-delay policy INVALID frame=%u "
             "applied=%u desired=%u blocker=%s(%u)",
             g_netSession->currentFrame(), applied, desired,
             qnetplay::inputDelayChangeBlockerName(blocker),
             static_cast<unsigned>(blocker));
        return false;
    }
    g_netDeferredDelayTarget = UINT32_MAX;
    g_netDeferredDelayLastLogMs = 0;
    if (!netFlushLocalSubmissions()) return false;
    if (gameplay) {
        if (g_netDelayDrain.active()) {
            QLOG("[netplay] gameplay-delay drain APPLIED frame=%u "
                 "delay=%u waited=%llums stableThrough=%u",
                 g_netSession->currentFrame(), desired,
                 static_cast<unsigned long long>(
                     g_netDelayDrain.elapsedMs(GetTickCount64())),
                 g_netSession->stableConfirmedThrough());
            g_netDelayDrain.reset();
        }
        g_netGameplayDelayApplied = desired;
        qgame::netSetLocalGameplayDelayStatus(
            static_cast<uint8_t>(g_netDelay),
            static_cast<uint8_t>(g_netGameplayDelayApplied),
            false);
        netMarkTestGameplayDelayApplied(desired);
    }

    qgame::NetPingStats policyPing{};
    const bool hasPolicyPing = qgame::netPeerPingStats(policyPing);
    QLOG("[netplay] input-delay policy frame=%u phase=%s applied=%u->%u "
         "gameplayDelay=%u frontendDelay=%u "
         "pingUs=[mean=%u min=%u max=%u p95=%u jitter=%u] loss=%u/1000 "
         "samples=%u",
         g_netSession->currentFrame(),
         gameplay ? "rollback-gameplay" : "exact-frontend",
         applied, desired, g_netDelay, g_netFrontendDelay,
         hasPolicyPing ? policyPing.meanMicros : 0u,
         hasPolicyPing ? policyPing.minimumMicros : 0u,
         hasPolicyPing ? policyPing.maximumMicros : 0u,
         hasPolicyPing ? policyPing.p95Micros : 0u,
         hasPolicyPing ? policyPing.jitterMicros : 0u,
         hasPolicyPing ? policyPing.lossPerMille : 0u,
         hasPolicyPing ? policyPing.samples : 0u);
    return true;
}

struct NetReplayCorrectionResult {
    bool succeeded = false;
    bool committedBoundary = false;
    int dispatchResult = kNetHoldFrameRet;
};

struct AudioCorrectionScope {
    bool active = false;
    ~AudioCorrectionScope() {
        if (active) qaudio::runtime().abortCorrection();
    }
};

bool netCommitAudioCorrection(AudioCorrectionScope& scope, uint32_t frame) {
    if (!scope.active) return true;
    scope.active = false;
    if (!qaudio::runtime().commitCorrection()) {
        netFailClosed(kNetInternalFailure, frame,
                      "audio correction reconcile failed");
        return false;
    }
    static uint64_t s_audioCorrectionCommits = 0;
    ++s_audioCorrectionCommits;
    if (s_audioCorrectionCommits <= 4
        || (s_audioCorrectionCommits % 200u) == 0u) {
        char audioLedger[768];
        qaudio::runtime().formatLedger(audioLedger, sizeof(audioLedger));
        QLOG("[audio-v2] correction #%llu committed; %s",
             static_cast<unsigned long long>(s_audioCorrectionCommits),
             audioLedger);
    }
    return true;
}

void netInvalidateDiscardedTimeline(
    uint32_t firstFrame, uint32_t untilExclusive) {
    for (uint32_t frame = firstFrame;
         frame != untilExclusive; ++frame) {
        const size_t metaSlot = frame % kNetMetaSlots;
        const size_t stateSlot = frame % kPoolSlots;
        NetFrameMeta& meta = g_netMeta[metaSlot];
        if (meta.valid && meta.frame == frame
            && meta.epoch == g_netEpoch) {
            meta = NetFrameMeta{};
        }
        (void)g_rb.invalidateTaggedSlot(
            stateSlot, g_netEpoch, frame);
        NetFpuSlot& fpu = g_netFpuSlots[stateSlot];
        if (fpu.valid && fpu.layout == g_rb.layoutGeneration()
            && fpu.epoch == g_netEpoch && fpu.frame == frame) {
            fpu.valid = false;
        }
    }
}

bool netHoldReplayBeforeBoundary(
    const qnetplay::FrameAction& action, uint32_t frame,
    const char* reason) {
    if (!g_netSession
        || !g_netSession->finishRollbackBeforeBoundary()) {
        netFailClosed(
            kNetInternalFailure, frame,
            "predicted gameplay-boundary hold was rejected");
        return false;
    }
    netInvalidateDiscardedTimeline(
        frame, action.replayUntilExclusive);
    g_netTransitionBlocked = frame;
    QLOG("[netplay] held corrected gameplay boundary frame=%u "
         "from=%u oldFrontier=%u discarded=%u reason=%s",
         frame, action.rollbackFrom, action.replayUntilExclusive,
         action.replayUntilExclusive - frame,
         reason ? reason : "predicted");
    return netPublishAndVerifyConfirmed();
}

NetReplayCorrectionResult netReplayCorrection(
    FrameDispatchFn orig, const qnetplay::FrameAction& action) {
    NetReplayCorrectionResult result{};
    if (!g_netSession || !rollbackEligible()) {
        netFailClosed(kNetInternalFailure, action.rollbackFrom,
                      "rollback requested outside gameplay");
        return result;
    }
    const NetFrameMeta restoreMeta =
        g_netMeta[action.rollbackFrom % kNetMetaSlots];
    if (!restoreMeta.valid || restoreMeta.frame != action.rollbackFrom
        || restoreMeta.epoch != g_netEpoch || !restoreMeta.preHash) {
        netFailClosed(kNetInternalFailure, action.rollbackFrom,
                      "rollback origin metadata is unavailable");
        return result;
    }
    if (!g_netSession->beginRollback(action.rollbackFrom,
                                     action.replayUntilExclusive)) {
        netFailClosed(kNetInternalFailure, action.rollbackFrom,
                      "rollback transaction was rejected by the session");
        return result;
    }
    if (!netRestorePreFrame(action.rollbackFrom,
                            restoreMeta.phaseBefore)) {
        return result;
    }
    AudioCorrectionScope audioScope;
    if (g_audioV2Enabled && g_netActive) {
        audioScope.active = qaudio::runtime().beginCorrection(
            action.rollbackFrom, action.replayUntilExclusive);
        if (!audioScope.active) {
            netFailClosed(kNetInternalFailure, action.rollbackFrom,
                          "audio correction transaction was rejected");
            return result;
        }
    }

    for (uint32_t frame = action.rollbackFrom;
         frame != action.replayUntilExclusive; ++frame) {
        const NetFrameMeta old = g_netMeta[frame % kNetMetaSlots];
        const uint32_t phaseBefore = netPhaseTag();
        if (!old.valid || old.frame != frame || old.epoch != g_netEpoch
            || old.phaseBefore != phaseBefore || !rollbackEligible()) {
            netFailClosed(kNetInternalFailure, frame,
                          "rollback crossed a phase/layout boundary");
            return result;
        }
        qnetplay::CanonicalInputs inputs{};
        if (!g_netSession->inputsForFrame(frame, inputs)) {
            netFailClosed(kNetInternalFailure, frame,
                          "rollback replay inputs are unavailable");
            return result;
        }
        if (netLifecycleTransitionRequiresExactInput()
            && (inputs.remotePredicted || !netRemoteActualThrough(frame))) {
            if (!netHoldReplayBeforeBoundary(
                    action, frame, "exact-lifecycle-prefix"))
                return result;
            result.succeeded = true;
            return result;
        }

        netAutomatchPrepareCanonicalFrame(frame, true);
        netNormalizeHumanMatchAiVm(frame, true);
        uint64_t preHash = 0;
        if (!netSavePreFrame(frame, phaseBefore, preHash)) return result;
        NetFrameMeta updated = netMetaBeforeTick(frame, phaseBefore, preHash);
        const qnetplay::CanonicalInputs canonicalInputs =
            netApplyCharacterSelectBackPolicy(frame, inputs);
        updated.canonicalActions = {
            canonicalInputs.p1,
            canonicalInputs.p2,
        };
        int replayResult = kNetHoldFrameRet;
        const PresentationDisposition replayPresentation =
            qgame::rollbackCorrectionTickVisibility()
                    == qgame::NativeTickVisibility::Historical
                ? PresentationDisposition::SuppressHistorical
                : PresentationDisposition::Normal;
        qgame::netPublishGameThreadHeartbeat(
            qgame::NetTransportDiagnostics::GameThreadStage::NativeEnter,
            frame);
        if (!netRunOwnedTick(
                orig, frame, canonicalInputs, true, replayResult, true,
                replayPresentation))
            return result;
        qgame::netPublishGameThreadHeartbeat(
            qgame::NetTransportDiagnostics::GameThreadStage::NativeReturn,
            frame);
        netPrepareNextCanonicalBoundary(frame, true);
        updated.phaseAfter = netPhaseTag();
        updated.replayLifecycleAfter =
            netReplayLifecycleV2(
                updated.replayLifecycleBefore.layoutGeneration);
        updated.replayRoundTransition =
            netReplayRoundTransitionV2(
                updated.replayLifecycleBefore,
                updated.replayLifecycleAfter);
        updated.replayRoster = netReplayRosterV2();
        updated.replayStage = netReplayStageV2();
        updated.replayDispatcherResult = replayResult;
        updated.spectatorPostHash = netSpectatorLiveStateHash(
            static_cast<uint64_t>(frame) + 1u);
        if (updated.spectatorPostHash == 0) {
            netFailClosed(kNetInternalFailure, frame,
                          "rollback replay post-state hash failed");
            return result;
        }
        const bool gameplayAfter = rollbackEligible();
        if (!gameplayAfter
            && (inputs.remotePredicted
                || !netRemoteActualThrough(frame))) {
            netFailClosed(
                kNetInternalFailure, frame,
                "speculative rollback replay escaped exact lifecycle guard");
            return result;
        }
        if (!g_netSession->commitReplayFrame(inputs, preHash)) {
            netFailClosed(kNetInternalFailure, frame,
                          "rollback replay commit was rejected");
            return result;
        }
        qgame::netPublishGameThreadHeartbeat(
            qgame::NetTransportDiagnostics::GameThreadStage::CanonicalCommit,
            frame);
        g_netMeta[frame % kNetMetaSlots] = updated;

        if (!gameplayAfter) {
            const uint32_t nextFrame = frame + 1u;
            const uint32_t discarded =
                action.replayUntilExclusive - nextFrame;
            if (!g_netSession->finishRollbackAtBoundary()) {
                netFailClosed(
                    kNetInternalFailure, frame,
                    "corrected gameplay boundary commit was rejected");
                return result;
            }
            netInvalidateDiscardedTimeline(
                nextFrame, action.replayUntilExclusive);
            g_netTransitionBlocked = qgame::kNetNoFrame;
            if (g_netGameplaySegmentSeen)
                g_netGameplayRefreshPending = true;
            g_netGameplaySegmentActive = false;
            QLOG("[netplay] corrected gameplay boundary frame=%u "
                 "from=%u oldFrontier=%u discarded=%u result=%d",
                 frame, action.rollbackFrom,
                 action.replayUntilExclusive, discarded,
                 replayResult);
            if (!netCommitAudioCorrection(audioScope, frame))
                return result;
            if (!netPublishAndVerifyConfirmed())
                return result;
            result.succeeded = true;
            result.committedBoundary = true;
            result.dispatchResult = replayResult;
            return result;
        }
    }
    if (!g_netSession->finishRollback()) {
        netFailClosed(kNetInternalFailure, action.rollbackFrom,
                      "rollback transaction did not finish");
        return result;
    }
    const qnetplay::RollbackStats& stats = g_netSession->stats();
    if (stats.rollbackCount <= 8 || (stats.rollbackCount % 100) == 0) {
        QLOG("[netplay] rollback #%llu from=%u to=%u depth=%u "
             "maxDepth=%u replayed=%llu",
             static_cast<unsigned long long>(stats.rollbackCount),
             action.rollbackFrom, action.replayUntilExclusive,
             action.replayUntilExclusive - action.rollbackFrom,
             stats.maxRollbackDepth,
             static_cast<unsigned long long>(stats.replayedFrames));
    }
    if (!netCommitAudioCorrection(audioScope, action.rollbackFrom))
        return result;
    result.succeeded = netPublishAndVerifyConfirmed();
    return result;
}

struct NetAdvanceOutcome {
    int result = kNetHoldFrameRet;
    uint32_t phaseBefore = 0;
    bool committed = false;
};

void netRegulatePaceSlew() {
    const auto apply = [](const int32_t ppm) {
        if (ppm == g_netPaceSlewPpm || g_paceTicks <= 0) return;
        g_netPaceSlewPpm = ppm;
        const int32_t magnitude = ppm < 0 ? -ppm : ppm;
        if (magnitude > g_netPaceSlewPeakPpm)
            g_netPaceSlewPeakPpm = magnitude;
        ++g_netPaceSlewAdjustments;
        const int64_t adjusted =
            g_paceTicks + (g_paceTicks * ppm) / 1000000;
        g_paceDeadline.setFrameTicks(adjusted);
    };

    const bool rollbackGameplay =
        g_netRunning && !g_netSpectatorMode && g_netSession != nullptr
        && rollbackEligible();
    if (rollbackGameplay) {
        const uint32_t frame = g_netSession->currentFrame();
        if (!g_netRollbackBalanceHasLastFrame
            || frame != g_netRollbackBalanceLastFrame) {
            g_netRollbackBalanceLastFrame = frame;
            g_netRollbackBalanceHasLastFrame = true;
            if (g_netRollbackBalanceGameplayFrames != UINT32_MAX)
                ++g_netRollbackBalanceGameplayFrames;
        }
    }

    qgame::NetPeerPressure peer{};
    const bool hasPeerReport =
        rollbackGameplay && qgame::netPeerPressure(peer);
    qnetplay::RollbackDiagnostics diagnostics{};
    if (g_netSession) diagnostics = g_netSession->diagnostics();

    qgame::RollbackBalanceInput balanceInput{};
    balanceInput.enabled = !netRollbackBalanceDisabled();
    balanceInput.rollbackGameplay = rollbackGameplay;
    balanceInput.hasPeerReport = hasPeerReport;
    balanceInput.gameplayFramesObserved =
        g_netRollbackBalanceGameplayFrames;
    balanceInput.peerReportAgeMs = peer.ageMs;
    balanceInput.localPredictionDepth =
        netSmoothedPredictionDepth(diagnostics.speculativeFrames);
    balanceInput.localPredictionLimit = static_cast<uint16_t>(g_netRollback);
    balanceInput.peerPredictionDepth = peer.predictionDepth;
    balanceInput.peerPredictionLimit =
        static_cast<uint16_t>(g_netRemoteRollback);
    balanceInput.peerRunState = peer.runState;

    const qgame::RollbackBalanceSample sample =
        qgame::classifyRollbackBalance(balanceInput);
    if (!g_netRollbackBalanceHasLastReason
        || sample.reason != g_netRollbackBalanceLastReason) {
        QLOG("[rollback-balance] state=%s frame=%u warmup=%u/%u "
             "localDepth=%u/%u peerDepth=%u/%u peerState=%u ageMs=%llu "
             "killSwitch=%u",
             qgame::rollbackBalanceReasonName(sample.reason),
             g_netSession ? g_netSession->currentFrame() : 0u,
             g_netRollbackBalanceGameplayFrames,
             qgame::kRollbackBalanceWarmupFrames,
             static_cast<unsigned>(balanceInput.localPredictionDepth),
             static_cast<unsigned>(balanceInput.localPredictionLimit),
             static_cast<unsigned>(balanceInput.peerPredictionDepth),
             static_cast<unsigned>(balanceInput.peerPredictionLimit),
             static_cast<unsigned>(balanceInput.peerRunState),
             static_cast<unsigned long long>(balanceInput.peerReportAgeMs),
             netRollbackBalanceDisabled() ? 1u : 0u);
        g_netRollbackBalanceLastReason = sample.reason;
        g_netRollbackBalanceHasLastReason = true;
    }

    if (!sample.usable) {
        if (g_netPaceSlewPpm != 0) {
            const int32_t previous = g_netPaceSlewPpm;
            apply(0);
            QLOG("[rollback-balance] action=release reason=%s ppm=0 "
                 "was=%d frame=%u",
                 qgame::rollbackBalanceReasonName(sample.reason), previous,
                 g_netSession ? g_netSession->currentFrame() : 0u);
        }
        g_netPaceSlewIntegralPpm = 0;
        g_netPaceSlewCountdown = 0;
        return;
    }
    if (g_netPaceSlewCountdown > 0) {
        --g_netPaceSlewCountdown;
        return;
    }

    const int32_t deadband = netPaceSlewDeadbandFrames();
    const qgame::NetplayPaceCorrection correction =
        qgame::planTrailingPaceCorrection(
            sample.predictionDepthDelta, deadband, g_netPaceSlewPpm,
            g_netPaceSlewIntegralPpm);
    g_netPaceSlewIntegralPpm = correction.integralPpm;
    const int32_t ppm = correction.periodPpm;

    if (ppm != g_netPaceSlewPpm) {
        const int32_t previous = g_netPaceSlewPpm;
        apply(ppm);
        if (previous == 0 || ppm == 0
            || ppm == -qgame::kNetPaceCorrectionMaxPpm) {
            QLOG("[rollback-balance] action=%s ppm=%d was=%d "
                 "localDepth=%u peerDepth=%u depthDelta=%d deadband=%d "
                 "peerAgeMs=%llu peerState=%u frame=%u periodTicks=%lld "
                 "nominal=%lld%s",
                 ppm == 0 ? "released" : (ppm < previous ? "speeding"
                                                         : "easing"),
                 ppm, previous,
                 static_cast<unsigned>(balanceInput.localPredictionDepth),
                 static_cast<unsigned>(balanceInput.peerPredictionDepth),
                 sample.predictionDepthDelta, deadband,
                 static_cast<unsigned long long>(
                     balanceInput.peerReportAgeMs),
                 static_cast<unsigned>(balanceInput.peerRunState),
                 g_netSession->currentFrame(),
                 (long long)g_paceDeadline.frameTicks(),
                 (long long)g_paceTicks,
                 ppm == -qgame::kNetPaceCorrectionMaxPpm
                     ? " AT CAP - local clock cannot catch up faster"
                     : "");
        }
    }
    g_netPaceSlewCountdown = kNetPaceSlewIntervalOpportunities;
}

bool netAdvanceOneCanonicalFrame(FrameDispatchFn orig,
                                 const qnetplay::FrameAction& action,
                                 bool hidden,
                                 NetAdvanceOutcome& out) {
    const uint32_t frame = action.frame;
    const bool gameplayBefore = rollbackEligible();
    const uint32_t phaseBefore = netPhaseTag();
    out.phaseBefore = phaseBefore;
    const uint32_t lifecycleSceneBefore = rd32(kSceneStateAddr);
    const uint32_t lifecycleNextBefore = rd32(kSceneNextAddr);
    const int32_t lifecycleMatchSubBefore =
        static_cast<int32_t>(rd32(kMatchSubAddr));
    const int32_t lifecycleScene3SubBefore =
        static_cast<int32_t>(rd32(addr::kScene3Substate));
    const bool lifecycleDispatchEdge =
        lifecycleSceneBefore != g_netLifecycleLoggedScene
        || lifecycleNextBefore != g_netLifecycleLoggedNext
        || lifecycleMatchSubBefore != g_netLifecycleLoggedMatchSub
        || lifecycleScene3SubBefore != g_netLifecycleLoggedScene3Sub;
    if (lifecycleDispatchEdge) {
        QLOG("[lifecycle] dispatch-enter frame=%u scene=%u next=%u "
             "matchSub=%d scene3Sub=%d gameplay=%u ready=%u "
             "legacyBusy=%u ring=%u",
             frame, lifecycleSceneBefore, lifecycleNextBefore,
             lifecycleMatchSubBefore, lifecycleScene3SubBefore,
             gameplayBefore ? 1u : 0u, rd32(kNetReadyAddr),
             rd32(0x4AAA54u), rd32(0x66EE28u));
        g_netLifecycleLoggedScene = lifecycleSceneBefore;
        g_netLifecycleLoggedNext = lifecycleNextBefore;
        g_netLifecycleLoggedMatchSub = lifecycleMatchSubBefore;
        g_netLifecycleLoggedScene3Sub = lifecycleScene3SubBefore;
    }
    if (!gameplayBefore) {
        if (g_netGameplaySegmentActive) {
            g_netGameplaySegmentActive = false;
            if (g_netGameplaySegmentSeen)
                g_netGameplayRefreshPending = true;
        }
        if (action.inputs.remotePredicted || !netRemoteActualThrough(frame)) {
            ++g_netPacingExactWaits;
            netObserveRollbackHealth(
                "adapter-hold", false,
                qgame::AdapterHoldReason::ExactInput);
            netPaceSemanticHold(qgame::HoldCause::LifecycleBoundary);
            Sleep(1);
            return false;
        }
        if (lifecycleSceneBefore == 2u
            && lifecycleMatchSubBefore == -1
            && g_netGameplayRefreshPending && g_rbReady) {
            reregisterFrameData();
            g_netGameplayRefreshPending = false;
            QLOG("[netplay] rematch layout epoch rotated at confirmed "
                 "setup frame=%u epoch=%u", frame, g_netEpoch);
        }
    } else {
        if (netLifecycleTransitionRequiresExactInput()
            && (action.inputs.remotePredicted
                || !netRemoteActualThrough(frame))) {
            g_netTransitionBlocked = frame;
            ++g_netPacingExactWaits;
            netObserveRollbackHealth(
                "adapter-hold", false,
                qgame::AdapterHoldReason::ExactInput);
            netPaceSemanticHold(qgame::HoldCause::LifecycleBoundary);
            Sleep(1);
            return false;
        }
        if (!g_netGameplaySegmentActive
            && (presentationDispositionQueryCount() == 0u
                || InterlockedCompareExchange(
                       &g_netPresentationWaitCallbacks, 0, 0) == 0
                || InterlockedCompareExchange(
                       &g_netPresentationCommitCallbacks, 0, 0) == 0)) {
            static uint64_t s_presentationGateWaitStartMs = 0;
            const uint64_t gateNowMs = GetTickCount64();
            if (s_presentationGateWaitStartMs == 0)
                s_presentationGateWaitStartMs = gateNowMs;
            if (gateNowMs - s_presentationGateWaitStartMs < 2000u) {
                netObserveRollbackHealth(
                    "adapter-hold", false,
                    qgame::AdapterHoldReason::PresentationGate);
                netPaceSemanticHold();
                Sleep(1);
                return false;
            }
            char gateWhy[160];
            std::snprintf(
                gateWhy, sizeof(gateWhy),
                "compatible ddraw presentation gate/publish callback "
                "not active (queries=%lu wait=%ld commit=%ld)",
                presentationDispositionQueryCount(),
                InterlockedCompareExchange(
                    &g_netPresentationWaitCallbacks, 0, 0),
                InterlockedCompareExchange(
                    &g_netPresentationCommitCallbacks, 0, 0));
            netFailClosed(kNetInternalFailure, frame, gateWhy);
            return false;
        }
        if (!ensureRollback()) {
            netObserveRollbackHealth(
                "adapter-hold", false,
                qgame::AdapterHoldReason::RollbackSetup);
            netPaceSemanticHold();
            Sleep(1);
            return false;
        }
        if (!g_netGameplaySegmentActive) {
            if (!netCanonicalizeHumanGameStartTransients(frame)) {
                netFailClosed(
                    kNetInternalFailure, frame,
                    "human game-start transient canonicalization failed");
                return false;
            }
            const char* why = nullptr;
            if (!g_rb.validateLive(&why)) {
                netFailClosed(kNetInternalFailure, frame,
                              why ? why : "rollback live-state validation failed");
                return false;
            }
            netResetFpuContract();
            g_netFpuInitializedEpoch = g_netEpoch;
            if (!netLayoutBarrierReady()) {
                netObserveRollbackHealth(
                    "adapter-hold", false,
                    qgame::AdapterHoldReason::LayoutBarrier);
                netPaceSemanticHold();
                Sleep(1);
                return false;
            }
            netResetFramePacingTelemetry();
            g_netGameplaySegmentActive = true;
            g_netGameplaySegmentSeen = true;
        }
    }

    netObserveRollbackHealth(
        "adapter-runnable", false,
        qgame::AdapterHoldReason::None);
    netTraceProductionCharacterSelect("pre", frame, action.inputs);

    netReplayMaybeStageFreshGameV2(frame);

    netAutomatchPrepareCanonicalFrame(frame, false);
    netNormalizeHumanMatchAiVm(frame, false);
    uint64_t preHash = 0;
    if (gameplayBefore
        && !netSavePreFrame(frame, phaseBefore, preHash))
        return false;
    NetFrameMeta meta = netMetaBeforeTick(frame, phaseBefore, preHash);
    const qnetplay::CanonicalInputs canonicalInputs =
        netApplyCharacterSelectBackPolicy(frame, action.inputs);
    meta.canonicalActions = {
        canonicalInputs.p1,
        canonicalInputs.p2,
    };
    if (!gameplayBefore && frame < 4u) {
        netTraceSpectatorFrontend(
            "source-pre", frame, meta.spectatorPreHash);
    }
    const uint32_t nativeCounterBefore = rd32(0x4AB778u);
    const uint32_t arenaSelectorBefore = rd32(kBlockSelector);
    qgame::netPublishGameThreadHeartbeat(
        qgame::NetTransportDiagnostics::GameThreadStage::NativeEnter,
        frame);
    const uint64_t ownedTickStartedMs = GetTickCount64();
    if (!netRunOwnedTick(orig, frame, canonicalInputs, false, out.result, true,
                         hidden ? PresentationDisposition::HiddenNewForward
                                : PresentationDisposition::Normal))
        return false;
    qgame::netPublishGameThreadHeartbeat(
        qgame::NetTransportDiagnostics::GameThreadStage::NativeReturn,
        frame);
    const uint64_t ownedTickElapsedMs =
        GetTickCount64() - ownedTickStartedMs;
    if (!hidden)
        netObservePacing(
            frame, gameplayBefore, nativeCounterBefore,
            rd32(0x4AB778u), arenaSelectorBefore, rd32(kBlockSelector));
    netTraceProductionCharacterSelect("post", frame, canonicalInputs);
    netPrepareNextCanonicalBoundary(frame, false);
    meta.phaseAfter = netPhaseTag();
    meta.replayLifecycleAfter =
        netReplayLifecycleV2(
            meta.replayLifecycleBefore.layoutGeneration);
    meta.replayRoundTransition =
        netReplayRoundTransitionV2(
            meta.replayLifecycleBefore,
            meta.replayLifecycleAfter);
    meta.replayRoster = netReplayRosterV2();
    meta.replayStage = netReplayStageV2();
    meta.replayDispatcherResult = out.result;
    const bool gameplayAfter = rollbackEligible();
    const uint32_t lifecycleSceneAfter = rd32(kSceneStateAddr);
    const uint32_t lifecycleNextAfter = rd32(kSceneNextAddr);
    const int32_t lifecycleMatchSubAfter =
        static_cast<int32_t>(rd32(kMatchSubAddr));
    const int32_t lifecycleScene3SubAfter =
        static_cast<int32_t>(rd32(addr::kScene3Substate));
    if (out.result != kNetHoldFrameRet
        || meta.phaseAfter != phaseBefore
        || lifecycleSceneAfter != lifecycleSceneBefore
        || lifecycleNextAfter != lifecycleNextBefore
        || lifecycleMatchSubAfter != lifecycleMatchSubBefore
        || lifecycleScene3SubAfter != lifecycleScene3SubBefore
        || ownedTickElapsedMs >= 250u) {
        QLOG("[lifecycle] dispatch-return frame=%u result=%d elapsedMs=%llu "
             "scene=%u->%u next=%u->%u matchSub=%d->%d "
             "scene3Sub=%d->%d gameplay=%u->%u ready=%u "
             "legacyBusy=%u ring=%u gateCalls=%ld",
             frame, out.result,
             static_cast<unsigned long long>(ownedTickElapsedMs),
             lifecycleSceneBefore, lifecycleSceneAfter,
             lifecycleNextBefore, lifecycleNextAfter,
             lifecycleMatchSubBefore, lifecycleMatchSubAfter,
             lifecycleScene3SubBefore, lifecycleScene3SubAfter,
             gameplayBefore ? 1u : 0u, gameplayAfter ? 1u : 0u,
             rd32(kNetReadyAddr), rd32(0x4AAA54u),
             rd32(0x66EE28u),
             InterlockedCompareExchange(&g_freezeGateCalls, 0, 0));
    }
    meta.spectatorPostHash = netSpectatorLiveStateHash(
        static_cast<uint64_t>(frame) + 1u);
    if (!gameplayAfter && frame < 4u) {
        netTraceSpectatorFrontend(
            "source-post", frame, meta.spectatorPostHash);
    }
    if (meta.spectatorPostHash == 0) {
        if (gameplayBefore)
            (void)netRestorePreFrame(frame, phaseBefore);
        netFailClosed(kNetInternalFailure, frame,
                      "visible post-state hash failed");
        return false;
    }

    if (gameplayBefore && !gameplayAfter
        && (action.inputs.remotePredicted
            || !netRemoteActualThrough(frame))) {
        netFailClosed(
            kNetInternalFailure, frame,
            "speculative visible tick escaped exact lifecycle guard");
        return false;
    }

    if (!g_netSession->commitAdvance(action.inputs, preHash)) {
        if (gameplayBefore)
            (void)netRestorePreFrame(frame, phaseBefore);
        netFailClosed(kNetInternalFailure, frame,
                      "visible frame commit was rejected");
        return false;
    }
    qgame::netPublishGameThreadHeartbeat(
        qgame::NetTransportDiagnostics::GameThreadStage::CanonicalCommit,
        frame);
    g_netMeta[frame % kNetMetaSlots] = meta;
    g_netFrame = g_netSession->currentFrame();
    if (!gameplayAfter) {
        if (gameplayBefore && g_netGameplaySegmentSeen)
            g_netGameplayRefreshPending = true;
        g_netGameplaySegmentActive = false;
    }
    if (!netPublishAndVerifyConfirmed())
        return false;
    if (g_audioV2Enabled && g_netActive && !hidden) {
        uint32_t audioConfirmedThrough = frame;
        if (g_netSession && g_netSession->hasRemoteContiguousThrough()) {
            const uint32_t remote = g_netSession->remoteContiguousThrough();
            if (remote < audioConfirmedThrough)
                audioConfirmedThrough = remote;
        }
        qaudio::runtime().flushPending(audioConfirmedThrough);
        static bool s_audioFailClosedAnnounced = false;
        if (!s_audioFailClosedAnnounced
            && qaudio::runtime().failedClosed()) {
            s_audioFailClosedAnnounced = true;
            char audioLedger[768];
            qaudio::runtime().formatLedger(audioLedger, sizeof(audioLedger));
            QLOG_ISSUE("[audio-v2] FAILED CLOSED frame=%u - physical audio disabled "
                 "for this session; %s", frame, audioLedger);
        }
        if ((frame % 3600u) == 0u && frame != 0u) {
            char audioLedger[768];
            qaudio::runtime().formatLedger(audioLedger, sizeof(audioLedger));
            QLOG("%s", audioLedger);
        }
    }
    out.committed = true;
    return true;
}

int netRematchAdvanceLocalInput(uint32_t frame) {
    if (rd32(kSceneStateAddr) != 2u
        || rd32(addr::kRoundCounter) < 5u) {
        return 0;
    }
    const int32_t matchSub = static_cast<int32_t>(rd32(kMatchSubAddr));
    const bool tap = (frame % 18u) < 3u;
    if (matchSub == 2) {
        const uint32_t winner = rd32(kMatchWinnerSlotAddr);
        if (winner <= 1u
            && static_cast<uint32_t>(g_netLocalCtrl) == (winner ^ 1u)) {
            return tap ? 0x00000001 : 0;
        }
        return 0;
    }
    if (matchSub == 4 && g_netLocalCtrl == 0)
        return tap ? 0x00000001 : 0;
    return 0;
}

bool netPumpSessionEvents() {
    using qnetplay::wire::SessionEventPayload;
    using qnetplay::wire::SessionEventStage;
    if (!g_netActive || !g_netRunning || g_netSpectatorMode
        || g_netSession == nullptr || !g_inputConfigPresent
        || g_netHalted) {
        g_sessionCaptureFrozen = false;
        return false;
    }
    netRefreshInputBundle();
    const uint64_t nowMs = GetTickCount64();

    {
        const int32_t promptMatchSub =
            static_cast<int32_t>(rd32(kMatchSubAddr));
        if (g_rematchTheater.flipped) {
            if (!netRematchTheaterGuard("pump")) {
                g_sessionCaptureFrozen = true;
                return true;
            }
        } else {
            const bool postGame = rd32(kSceneStateAddr) == 2u
                && promptMatchSub == 5
                && rd32(addr::kRoundCounter) >= 5u
                && rd32(kMatchWinnerSlotAddr) <= 1u;
            if (!postGame) {
                if (g_rematchPrompt.active()) {
                    QLOG("[rematch] prompt closed frame=%u phase=%u "
                         "(native state moved on)",
                         g_netSession->currentFrame(),
                         static_cast<unsigned>(g_rematchPrompt.phase()));
                    netRematchTheaterRestore("prompt closed");
                    g_rematchPrompt.reset();
                    g_rematchEndRequested = false;
                }
            } else if (!g_rematchPrompt.active()) {
                if (true) {
                } else if (rollbackEligible()) {
                    QLOG("[rematch] REFUSED latch on rollback-eligible state "
                         "frame=%u", g_netSession->currentFrame());
                } else if (g_rematchPrompt.begin(
                               g_netEpoch, g_netSession->currentFrame(),
                               static_cast<uint8_t>(g_netLocalCtrl), nowMs)) {
                    g_rematchEndRequested = false;
                    QLOG("[rematch] prompt OPEN frame=%u epoch=%u side=P%d "
                         "matchSub=%d wins=[%u,%u]",
                         g_netSession->currentFrame(), g_netEpoch,
                         g_netLocalCtrl + 1, promptMatchSub,
                         g_netGameWins[0], g_netGameWins[1]);
                    (void)netRematchTheaterFlip();
                }
            }
        }
    }

    const qinput::ModeControlEvaluation controls =
        netEvaluateOnlineModeControls();
    const bool leaveRequested = (controls.edges & qinput::modeControlBit(
        qinput::ModeControlAction::LeaveMatch)) != 0;
    const bool returnRequested =
        (controls.edges & qinput::modeControlBit(
             qinput::ModeControlAction::OnlineReturnCharacterSelect)) != 0;
    auto sendStage = [&](SessionEventStage stage, uint32_t eventId,
                         qinput::SessionEventKind kind, uint32_t value,
                         uint32_t boundary) {
        SessionEventPayload payload{};
        payload.stage = stage;
        payload.kind = static_cast<uint8_t>(kind);
        payload.eventId = eventId;
        payload.value = value;
        payload.boundary = boundary;
        (void)qgame::netSubmitSessionEvent(payload);
    };
    auto sendReport = [&](uint32_t eventId,
                          qinput::SessionEventKind kind) {
        SessionEventPayload payload{};
        payload.stage = SessionEventStage::Report;
        payload.kind = static_cast<uint8_t>(kind);
        payload.eventId = eventId;
        payload.nextFrame = g_netSession->currentFrame();
        payload.assignedTailExclusive =
            g_netSession->assignedLocalTailExclusive();
        payload.oldestRestorable = g_netSession->oldestRestorableFrame();
        payload.spectatorFrontier = 0;
        payload.lifecycleKey = netPhaseTag();
        payload.flags = g_netDelayDrain.active() ? 0x01u : 0x00u;
        (void)qgame::netSubmitSessionEvent(payload);
    };
    auto localReport = [&]() {
        qinput::QuiesceReport report{};
        report.valid = true;
        report.nextFrame = g_netSession->currentFrame();
        report.assignedTailExclusive =
            g_netSession->assignedLocalTailExclusive();
        report.oldestRestorable = g_netSession->oldestRestorableFrame();
        report.spectatorFrontier = 0;
        report.lifecyclePhaseKey = netPhaseTag();
        report.delayTransitionPending = g_netDelayDrain.active();
        return report;
    };

    auto requestEvent = [&](qinput::SessionEventKind kind,
                            uint32_t value) {
        if (g_sessionIsHost) {
            if (g_sessionEvents.hostAcceptRequest(kind, value)) {
                const uint32_t id = g_sessionEvents.current().eventId;
                g_sessionCaptureFrozen = true;
                g_sessionReportSubmitted = false;
                g_sessionAtBoundarySent = false;
                g_sessionRemoteAtBoundary = false;
                g_sessionFillDone = false;
                sendStage(SessionEventStage::Quiesce, id, kind, value, 0);
                QLOG("[session-event] host accepted kind=%u value=%u id=%u",
                     static_cast<unsigned>(kind), value, id);
            }
        } else if (g_sessionClientRequestId == 0) {
            g_sessionClientRequestId =
                0x40000000u + (static_cast<uint32_t>(nowMs) | 1u);
            g_sessionClientRequestMs = nowMs;
            sendStage(SessionEventStage::Request, g_sessionClientRequestId,
                      kind, value, 0);
            QLOG("[session-event] client requested kind=%u value=%u",
                 static_cast<unsigned>(kind), value);
        }
    };
    if (leaveRequested) {
        if (g_rematchPrompt.deciding() && !g_rematchEndRequested) {
            (void)g_rematchPrompt.latchLocalLeave();
            QLOG("[rematch] local locked NO (Back) frame=%u - waiting for "
                 "the opponent", g_rematchPrompt.promptFrame());
        } else if (!g_rematchPrompt.deciding()) {
            requestEvent(qinput::SessionEventKind::TerminateSession,
                         kNetTerminateValueBack);
        }
    }
    bool returnAutomation = false;
    if (g_netTestF1ReturnFrame != 0u && g_sessionIsHost
        && g_netSession->currentFrame() >= g_netTestF1ReturnFrame
        && qgame::onlineReturnToCharacterSelectEligible(
               rd32(kSceneStateAddr))) {
        returnAutomation = true;
        g_netTestF1ReturnFrame = 0u;
        QLOG("[test-automatch] F1 return firing at frame %u",
             g_netSession->currentFrame());
    }
    if ((returnRequested || returnAutomation)
        && qgame::onlineReturnToCharacterSelectEligible(
               rd32(kSceneStateAddr))) {
        requestEvent(
            qinput::SessionEventKind::ReturnToCharacterSelect,
            g_netSession->currentFrame());
    }
    if (!g_sessionIsHost && g_sessionClientRequestId != 0
        && g_sessionEvents.current().state
               == qinput::SessionEventState::Idle
        && nowMs - g_sessionClientRequestMs > 3000u) {
        g_sessionClientRequestId = 0;
        qgame::netClearSessionEvent();
        if (g_rematchPrompt.active()) g_rematchEndRequested = false;
    }

    if (g_rematchPrompt.active()) {
        if (g_rematchPrompt.deciding() && !g_netAutomatchTest
            && !g_sessionEvents.paused()) {
            bool sampleNeutralBeforeSocd = true;
            const uint32_t sample = static_cast<uint32_t>(
                netManagedPhysicalCapture(sampleNeutralBeforeSocd));
            const uint32_t prev = g_rematchTheater.prevSampleMask;
            g_rematchTheater.prevSampleMask = sample;
            if (!g_rematchTheater.flipped
                && !g_rematchPrompt.localReady()
                && qnetplay::rematchTheaterFallbackCursorToggles(prev,
                                                                 sample)) {
                g_rematchTheater.fallbackCursorNo =
                    !g_rematchTheater.fallbackCursorNo;
            }
            if (qnetplay::rematchTheaterConfirmEdge(prev, sample)
                && !g_rematchPrompt.localReady()) {
                const bool yes = g_rematchTheater.flipped
                    ? qnetplay::rematchTheaterCursorIsYes(
                          rd32(kRematchTheaterCursorAddr))
                    : !g_rematchTheater.fallbackCursorNo;
                if (g_rematchPrompt.latchLocalConfirm(yes)) {
                    QLOG("[rematch] local CONFIRM %s frame=%u cursor=%u "
                         "theater=%d",
                         yes ? "YES" : "NO",
                         g_rematchPrompt.promptFrame(),
                         rd32(kRematchTheaterCursorAddr),
                         static_cast<int>(g_rematchTheater.flipped));
                }
            }
            g_rematchTheater.feedMask =
                qnetplay::rematchTheaterLocalFeed(
                    sample, g_rematchPrompt.localReady());
        } else {
            g_rematchTheater.feedMask = 0u;
        }

        if (g_netAutomatchTest && g_netRematchAutoAnswer != 0
            && g_rematchPrompt.deciding() && !g_rematchPrompt.localReady()
            && nowMs - g_rematchPrompt.startMs() >= 500u) {
            if (g_netRematchAutoAnswer == 2 && !g_rematchEndRequested) {
                if (g_rematchPrompt.latchLocalConfirm(false)) {
                    QLOG("[rematch] auto-answer DECLINE frame=%u",
                         g_rematchPrompt.promptFrame());
                }
            } else if (g_netRematchAutoAnswer == 1) {
                if (g_rematchPrompt.latchLocalConfirm(true)) {
                    QLOG("[rematch] auto-answer REMATCH frame=%u",
                         g_rematchPrompt.promptFrame());
                }
            }
        }

        qnetplay::wire::RematchChoicePayload inboundChoice{};
        while (qgame::netTakeRematchChoice(inboundChoice)) {
            const bool hadReady = g_rematchPrompt.remoteReady();
            const uint8_t hadChoice = g_rematchPrompt.remoteChoice();
            const bool hadSeen = g_rematchPrompt.remoteSeen();
            if (g_rematchPrompt.acceptRemote(inboundChoice)) {
                if (!hadSeen || hadReady != g_rematchPrompt.remoteReady()
                    || hadChoice != g_rematchPrompt.remoteChoice()) {
                    QLOG("[rematch] remote choice=%u ready=%u frame=%u",
                         inboundChoice.choice, inboundChoice.ready,
                         inboundChoice.promptFrame);
                }
            } else {
                QLOG("[rematch] dropped stale choice epoch=%u frame=%u "
                     "(local epoch=%u frame=%u)",
                     inboundChoice.epoch, inboundChoice.promptFrame,
                     g_rematchPrompt.epoch(),
                     g_rematchPrompt.promptFrame());
            }
        }

        if (g_rematchPrompt.shouldSend(nowMs)
            && qgame::netSubmitRematchChoice(
                   g_rematchPrompt.localAdvertisement())) {
            g_rematchPrompt.markSent(nowMs);
        }

        if (g_sessionIsHost && g_rematchPrompt.deciding()
            && g_rematchPrompt.agreementReached()
            && !g_sessionEvents.paused()
            && g_sessionEvents.hostAcceptRequest(
                   qinput::SessionEventKind::RematchCommit,
                   g_rematchPrompt.promptFrame())) {
            const uint32_t id = g_sessionEvents.current().eventId;
            g_sessionCaptureFrozen = true;
            g_sessionReportSubmitted = false;
            g_sessionAtBoundarySent = false;
            g_sessionRemoteAtBoundary = false;
            g_sessionFillDone = false;
            sendStage(SessionEventStage::Quiesce, id,
                      qinput::SessionEventKind::RematchCommit,
                      g_rematchPrompt.promptFrame(), 0);
            g_rematchPrompt.markCommitPending();
            netRematchTheaterRestore("commit entering ladder");
            QLOG("[rematch] AGREED: host opened RematchCommit id=%u "
                 "frame=%u", id, g_rematchPrompt.promptFrame());
        }

        if (g_sessionIsHost && g_rematchPrompt.deciding()
            && g_rematchPrompt.declineAgreed()
            && !g_sessionEvents.paused()
            && g_sessionEvents.hostAcceptRequest(
                   qinput::SessionEventKind::RematchDecline,
                   g_rematchPrompt.promptFrame())) {
            const uint32_t id = g_sessionEvents.current().eventId;
            g_sessionCaptureFrozen = true;
            g_sessionReportSubmitted = false;
            g_sessionAtBoundarySent = false;
            g_sessionRemoteAtBoundary = false;
            g_sessionFillDone = false;
            sendStage(SessionEventStage::Quiesce, id,
                      qinput::SessionEventKind::RematchDecline,
                      g_rematchPrompt.promptFrame(), 0);
            g_rematchPrompt.markCommitPending();
            netRematchTheaterRestore("decline entering ladder");
            QLOG("[rematch] DECLINE AGREED: host opened RematchDecline "
                 "id=%u frame=%u", id, g_rematchPrompt.promptFrame());
        }

        if (!g_rematchEndRequested
            && g_rematchPrompt.decisionTimedOut(nowMs)) {
            (void)g_rematchPrompt.latchLocalLeave();
            g_rematchEndRequested = true;
            QLOG("[rematch] decision TIMEOUT frame=%u",
                 g_rematchPrompt.promptFrame());
            netRematchTheaterRestore("timeout entering ladder");
            requestEvent(qinput::SessionEventKind::RematchDecline,
                         g_rematchPrompt.promptFrame());
        }
    }

    SessionEventPayload inbound{};
    while (qgame::netTakeSessionEvent(inbound)) {
        const qinput::SessionEventKind kind =
            static_cast<qinput::SessionEventKind>(inbound.kind);
        switch (inbound.stage) {
            case SessionEventStage::Request:
                if (g_sessionIsHost) {
                    if (kind == qinput::SessionEventKind::RematchCommit) {
                        QLOG("[rematch] refused relayed RematchCommit "
                             "request id=%u", inbound.eventId);
                        break;
                    }
                    if (g_sessionEvents.hostAcceptRequest(kind,
                                                          inbound.value)) {
                        const uint32_t id =
                            g_sessionEvents.current().eventId;
                        g_sessionCaptureFrozen = true;
                        g_sessionReportSubmitted = false;
                        g_sessionAtBoundarySent = false;
                        g_sessionRemoteAtBoundary = false;
                        g_sessionFillDone = false;
                        sendStage(SessionEventStage::Quiesce, id, kind,
                                  inbound.value, 0);
                        QLOG("[session-event] host accepted relayed "
                             "kind=%u value=%u id=%u",
                             inbound.kind, inbound.value, id);
                    }
                }
                break;
            case SessionEventStage::Quiesce:
                if (!g_sessionIsHost
                    && g_sessionEvents.peerBeginQuiesce(inbound.eventId,
                                                        kind,
                                                        inbound.value)) {
                    if (kind == qinput::SessionEventKind::RematchCommit) {
                        if (!g_rematchPrompt.active()
                            || inbound.value
                                   != g_rematchPrompt.promptFrame()) {
                            netFailClosed(kNetProtocolViolation,
                                          inbound.value,
                                          "rematch commit named an "
                                          "unknown prompt");
                            return true;
                        }
                        g_rematchPrompt.markCommitPending();
                    }
                    if (kind == qinput::SessionEventKind::RematchCommit
                        || kind == qinput::SessionEventKind::RematchDecline
                        || kind == qinput::SessionEventKind::
                                       ReturnToCharacterSelect) {
                        netRematchTheaterRestore("outcome quiesce");
                    }
                    g_sessionCaptureFrozen = true;
                    g_sessionReportSubmitted = false;
                    g_sessionAtBoundarySent = false;
                    g_sessionFillDone = false;
                    g_sessionClientRequestId = 0;
                    QLOG("[session-event] quiesce id=%u kind=%u value=%u",
                         inbound.eventId, inbound.kind, inbound.value);
                } else if (!g_sessionIsHost) {
                    static uint64_t s_quiesceRefusedMs = 0;
                    if (nowMs - s_quiesceRefusedMs >= 1000u) {
                        s_quiesceRefusedMs = nowMs;
                        QLOG("[session-event] quiesce id=%u kind=%u REFUSED "
                             "(state=%u currentId=%u lastApplied=%u)",
                             inbound.eventId, inbound.kind,
                             static_cast<unsigned>(
                                 g_sessionEvents.current().state),
                             g_sessionEvents.current().eventId,
                             g_sessionEvents.lastAppliedId());
                    }
                }
                break;
            case SessionEventStage::Report:
                if (g_sessionIsHost) {
                    qinput::QuiesceReport remote{};
                    remote.valid = true;
                    remote.nextFrame = inbound.nextFrame;
                    remote.assignedTailExclusive =
                        inbound.assignedTailExclusive;
                    remote.oldestRestorable = inbound.oldestRestorable;
                    remote.spectatorFrontier = inbound.spectatorFrontier;
                    remote.lifecyclePhaseKey = inbound.lifecycleKey;
                    remote.delayTransitionPending =
                        (inbound.flags & 0x01u) != 0;
                    g_sessionEvents.submitRemoteReport(remote);
                }
                break;
            case SessionEventStage::Prepare:
                if (!g_sessionIsHost) {
                    if (!g_sessionEvents.acceptPrepare(
                            inbound.eventId, inbound.boundary,
                            g_netSession->currentFrame())) {
                        if (g_sessionEvents.terminated()) {
                            netFailClosed(kNetProtocolViolation,
                                          inbound.boundary,
                                          "session-event boundary was "
                                          "already crossed");
                            return true;
                        }
                    }
                }
                break;
            case SessionEventStage::AtBoundary:
                if (g_sessionIsHost
                    && g_sessionEvents.current().eventId
                           == inbound.eventId) {
                    if (!g_sessionRemoteAtBoundary) {
                        QLOG("[session-event] peer at boundary id=%u kind=%u "
                             "reported=%u expected=%u",
                             inbound.eventId,
                             static_cast<unsigned>(kind),
                             inbound.boundary,
                             g_sessionEvents.current().boundary);
                    }
                    g_sessionRemoteAtBoundary = true;
                }
                break;
            case SessionEventStage::Commit:
                if (!g_sessionIsHost) {
                    if (g_sessionEvents.lastAppliedId()
                        >= inbound.eventId) {
                        sendStage(SessionEventStage::CommitAck,
                                  inbound.eventId, kind, inbound.value,
                                  inbound.boundary);
                        break;
                    }
                    (void)g_sessionEvents.commit(inbound.eventId);
                }
                break;
            case SessionEventStage::CommitAck:
                // Clear only the acknowledged event: a late CommitAck echo must not wipe the next event's Quiesce from the outbound slot.
                if (g_sessionIsHost
                    && inbound.eventId == g_sessionEvents.lastAppliedId()) {
                    qgame::netClearSessionEvent();
                }
                break;
            case SessionEventStage::Abort:
                (void)g_sessionEvents.abortBeforePrepare(inbound.eventId);
                if (g_sessionEvents.current().state
                    == qinput::SessionEventState::Idle) {
                    g_sessionCaptureFrozen = g_sessionEvents.paused();
                    qgame::netClearSessionEvent();
                }
                break;
        }
    }

    const qinput::SessionEvent& current = g_sessionEvents.current();
    switch (current.state) {
        case qinput::SessionEventState::Quiescing: {
            g_sessionCaptureFrozen = true;
            if (!g_sessionReportSubmitted) {
                g_sessionEvents.submitLocalReport(localReport());
                if (!g_sessionIsHost)
                    sendReport(current.eventId, current.kind);
                g_sessionReportSubmitted = true;
            }
            if (g_sessionIsHost) {
                uint32_t boundary = 0;
                if (g_sessionEvents.hostChooseBoundary(boundary)) {
                    sendStage(SessionEventStage::Prepare, current.eventId,
                              current.kind, current.value, boundary);
                    QLOG("[session-event] prepared id=%u boundary=%u",
                         current.eventId, boundary);
                } else if (g_sessionEvents.terminated()) {
                    netFailClosed(kNetProtocolViolation,
                                  g_netSession->currentFrame(),
                                  "session-event quiesce reports were "
                                  "incompatible");
                    return true;
                }
            }
            return true;
        }
        case qinput::SessionEventState::Prepared: {
            g_sessionCaptureFrozen = true;
            if (!g_sessionFillDone) {
                if (!g_netSession->fillLocalNeutralThrough(
                        current.boundary)) {
                    netFailClosed(kNetInternalFailure, current.boundary,
                                  "QuiesceNeutral fill failed");
                    return true;
                }
                (void)netFlushLocalSubmissions();
                g_sessionFillDone = true;
                QLOG("[session-event] drain to boundary=%u from=%u",
                     current.boundary, g_netSession->currentFrame());
            }
            const uint32_t frontier = g_netSession->currentFrame();
            if (qnet::frameAfter(frontier, current.boundary)) {
                QLOG("[session-event] boundary OVERSHOOT id=%u kind=%u "
                     "frontier=%u boundary=%u",
                     current.eventId,
                     static_cast<unsigned>(current.kind), frontier,
                     current.boundary);
                netFailClosed(
                    kNetInternalFailure, frontier,
                    "session-event drain crossed its prepared boundary");
                return true;
            }
            if (frontier == current.boundary) {
                if (g_netSession->hasPendingRollback()) return false;
                if (g_netSession->diagnostics().nextConfirmedFrame
                    == current.boundary) {
                    if (!g_sessionEvents.markAtBoundary(current.eventId)) {
                        netFailClosed(
                            kNetInternalFailure, frontier,
                            "session-event boundary could not be marked");
                        return true;
                    }
                    QLOG("[session-event] at boundary id=%u kind=%u side=%s "
                         "boundary=%u",
                         current.eventId,
                         static_cast<unsigned>(current.kind),
                         g_sessionIsHost ? "host" : "client",
                         current.boundary);
                    if (!g_sessionIsHost && !g_sessionAtBoundarySent) {
                        sendStage(SessionEventStage::AtBoundary,
                                  current.eventId, current.kind,
                                  current.value, current.boundary);
                        g_sessionAtBoundarySent = true;
                    }
                }
                return true;
            }
            return false;
        }
        case qinput::SessionEventState::AtBoundary: {
            g_sessionCaptureFrozen = true;
            if (g_sessionIsHost) {
                if (g_sessionRemoteAtBoundary) {
                    (void)g_sessionEvents.commit(current.eventId);
                    sendStage(SessionEventStage::Commit, current.eventId,
                              current.kind, current.value,
                              current.boundary);
                }
            } else if (!g_sessionAtBoundarySent) {
                sendStage(SessionEventStage::AtBoundary, current.eventId,
                          current.kind, current.value, current.boundary);
                g_sessionAtBoundarySent = true;
            }
            if (current.state != qinput::SessionEventState::Committed)
                return true;
            return true;
        }
        case qinput::SessionEventState::Committed: {
            const uint32_t eventId = current.eventId;
            const qinput::SessionEventKind kind = current.kind;
            const uint32_t value = current.value;
            const uint32_t boundary = current.boundary;
            if (g_sessionEvents.applyOnce(eventId)) {
                QLOG("[session-event] APPLIED id=%u kind=%u value=%u "
                     "boundary=%u",
                     eventId, static_cast<unsigned>(kind), value,
                     boundary);
                if (!g_sessionIsHost) {
                    sendStage(SessionEventStage::CommitAck, eventId, kind,
                              value, boundary);
                }
                if (kind == qinput::SessionEventKind::TerminateSession) {
                    netRematchTheaterRestore("committed terminate");
                    const char* reason = "session ended by committed Back";
                    if (value == kNetTerminateValueRematchDeclined)
                        reason = "rematch declined";
                    else if (value == kNetTerminateValueRematchTimeout)
                        reason = "rematch timed out";
                    netFailClosed(kNetPeerRequested, boundary, reason,
                                  true, true);
                    return true;
                }
                if (kind == qinput::SessionEventKind::RematchDecline) {
                    netRematchTheaterRestore("rematch declined");
                    g_rematchPrompt.markCommitted();
                    QLOG("[rematch] DECLINED committed: returning to "
                         "character select, session alive (boundary=%u "
                         "promptFrame=%u)", boundary, value);
                }
                if (kind == qinput::SessionEventKind::
                                ReturnToCharacterSelect) {
                    netRematchTheaterRestore("committed F1 return");
                    netCanonicalRematchCancelForCharacterSelect();
                    g_rematchPrompt.reset();
                    g_rematchEndRequested = false;
                    g_netInstantRematchLaunch = false;
                    g_netReturnToCharacterSelectPending = true;
                    g_netReturnToCharacterSelectBoundary = boundary;
                    QLOG("[netplay] F1 return committed at boundary=%u "
                         "requestedFrame=%u; returning both players to "
                         "character select", boundary, value);
                }
                if (kind == qinput::SessionEventKind::RematchCommit) {
                    netRematchTheaterRestore("rematch commit");
                    g_rematchPrompt.markCommitted();
                    if (g_spectatorServiceMode == 0u) {
                        wr32(kMatchSubAddr, static_cast<uint32_t>(-1));
                        wr32(0x626670u, 0u);
                        wr32(0x626674u, 0u);
                        wr32(0x8A5D20u, 0u);
                        g_netInstantRematchLaunch = true;
                        g_netInstantRematchBoundary = boundary;
                        QLOG("[rematch] APPLIED: INSTANT RESTART at "
                             "boundary=%u promptFrame=%u (formation=%u "
                             "cpuMask=%u tallies=0/0)",
                             boundary, value, rd32(0x62369Cu),
                             rd32(0x6236CCu));
                    } else {
                        QLOG("[rematch] APPLIED: resuming winner screen "
                             "boundary=%u promptFrame=%u (charselect route: "
                             "spectator service active)", boundary, value);
                    }
                }
                if (!g_sessionEvents.paused()) {
                    const uint32_t seedThrough =
                        boundary
                        + static_cast<uint32_t>(
                              g_netSession->config().inputDelayFrames);
                    (void)g_netSession->fillLocalNeutralThrough(
                        seedThrough);
                    (void)netFlushLocalSubmissions();
                    g_sessionCaptureFrozen = false;
                }
            }
            return g_sessionEvents.paused()
                || g_netReturnToCharacterSelectPending;
        }
        case qinput::SessionEventState::Failed:
            netFailClosed(kNetProtocolViolation,
                          g_netSession->currentFrame(),
                          "session-event protocol failed");
            return true;
        case qinput::SessionEventState::Idle:
        case qinput::SessionEventState::Requested:
        case qinput::SessionEventState::Applied:
            break;
    }

    if (g_sessionEvents.paused()) {
        g_sessionCaptureFrozen = true;
        return true;
    }
    if (g_netReturnToCharacterSelectPending) {
        g_sessionCaptureFrozen = true;
        return true;
    }
    if (g_rematchPrompt.deciding()
        || g_rematchPrompt.phase()
               == qnetplay::RematchPromptPhase::CommitPending) {
        g_sessionCaptureFrozen = true;
        return true;
    }
    g_sessionCaptureFrozen = false;
    return false;
}

void netFinalizeCommittedCharacterSelectReturn() {
    const uint32_t boundary = g_netReturnToCharacterSelectBoundary;
    bool replayClosed = true;
    if (qgame::replay::replayRecorderV2Active()) {
        replayClosed = qgame::replay::replayRecordAbnormalEndV2(
            qreplay::TerminalReason::ProcessExit,
            qreplay::ReplayCloseProvenanceV2::LocalOrderly);
    } else if (netRecorderArmed()) {
        replayClosed = netRecorderCloseMatchSegment(
            qreplay::TerminalReason::ProcessExit);
    }
    if (!replayClosed) {
        QLOG("[replay.record] F1 tail close was refused at boundary=%u; "
             "netplay return continues", boundary);
    } else {
        QLOG("[replay.record] F1 tail closed at boundary=%u", boundary);
    }

    if (!g_netSpectatorArchiveSealed
        && ((g_spectatorServiceMode == 2u
             && qgame::spectator_publish::sidecarPublisherActive())
            || g_netSpectatorArchiveBegun)) {
        if (!spectatorSetEventPublishF1Return()) {
            netSealSpectatorArchive(
                qnetplay::spectator::TerminalReason::SourceStopped);
        }
    }

    g_netReturnToCharacterSelectPending = false;
    g_netReturnToCharacterSelectBoundary = 0;
    g_sessionCaptureFrozen = false;
}

int doNetplay(FrameDispatchFn orig) {
    using namespace qgame;
    if (!g_netActive) return orig ? orig() : kNetHoldFrameRet;
    if (g_netHalted) {
        return netPostTerminalHold();
    }
    if (InterlockedCompareExchange(
            &g_netPacingClockFault, 0, 0) != 0) {
        netFailClosed(
            kNetInternalFailure, g_netFrame,
            "canonical presentation clock failed");
        return kNetHoldFrameRet;
    }
    ++g_netPacingDriverCalls;

    if (rd32(0x626860) == 0) return orig ? orig() : kNetHoldFrameRet;
    if (rd32(kNetIsNetworkAddr) != 0u) {
        netFailClosed(
            kNetInvalidConfiguration, rd32(kNetIsNetworkAddr),
            "M2 dual-slot input pipeline left QOH local mode");
        return kNetHoldFrameRet;
    }
    const bool pacingConfigReady = !g_netReadySnapped
        ? netNormalizeLocalSchedulerConfig()
        : netLocalSchedulerConfigIsNormalized();
    if (!pacingConfigReady) {
        netFailClosed(
            kNetInvalidConfiguration, 0,
            "local scheduler config could alter deterministic execution");
        return kNetHoldFrameRet;
    }
    netApplyExtraFunctionPolicy();
    if (!g_readHooked || !g_pollHooked || !g_paceBracketHooked
        || !g_clockHooked || !g_configSaveHooked
        || !g_fCommandPolicyHooked || !g_fCommandPolicyApplied
        || !playbackSideEffectHooksReady()) {
        netFailClosed(kNetInternalFailure, 0,
                      "mandatory deterministic gameplay hook disappeared");
        return kNetHoldFrameRet;
    }
    if (!netCheckLink(false)) return kNetHoldFrameRet;

    netPollGameplayDelayHotkeys();
    netPollTestGameplayDelayScript();

    if (!g_netRunning) {
        if (qgame::directBootNetworkActive()
            && !qgame::directBootNetworkAtStartBarrier()) {
            return orig ? orig() : kNetHoldFrameRet;
        }
        if (!g_netReadySnapped) {
            netResetFpuContract();
            g_netLocalReady.seedGameplay = rd32(0x4A9938);
            g_netLocalReady.seedCosmetic = rd32(0x4A993C);
            g_netLocalReady.selector = rd32(0x4ABB24);
            g_netLocalReady.bootstrapHash =
                netBootstrapFingerprint();
            if (g_netLocalReady.bootstrapHash == 0) {
                netFailClosed(kNetInvalidConfiguration, 0,
                              "canonical audio manifest is incomplete");
                return kNetHoldFrameRet;
            }
            if (!netReadActiveMatchConfig(g_netLocalReady)) {
                netFailClosed(kNetInvalidConfiguration, 0,
                              "active QOH match config is invalid");
                return kNetHoldFrameRet;
            }
            g_netReadySnapped = true;
        }
        if (!netSubmitLocalReady(g_netLocalReady)) {
            netFailClosed(kNetInternalFailure, 0,
                          "transport rejected immutable ready state");
            return kNetHoldFrameRet;
        }
        NetReadyState remote{};
        if (netLinkEstablished() && netReadyComplete()
            && netGetRemoteReady(remote)) {
            if (remote.bootstrapHash
                != g_netLocalReady.bootstrapHash) {
                netFailClosed(
                    kNetInvalidConfiguration,
                    static_cast<uint32_t>(
                        remote.bootstrapHash
                        ^ g_netLocalReady.bootstrapHash),
                    "deterministic frontend bootstrap fingerprint differs");
                return kNetHoldFrameRet;
            }
            const NetReadyState chosen =
                g_netLocalCtrl == 0 ? g_netLocalReady : remote;
            if (!netApplyAuthoritativeMatchConfig(chosen)) {
                netFailClosed(
                    kNetInvalidConfiguration, 0,
                    "cannot apply P1-authoritative match config");
                return kNetHoldFrameRet;
            }
            wr32(0x4A9938, chosen.seedGameplay);
            wr32(0x4A993C, chosen.seedCosmetic);
            wr32(0x4ABB24, chosen.selector);
            if (!netInitializeRollbackSession())
                return kNetHoldFrameRet;
            netRecorderCaptureNetplayStart(chosen);
            netBeginSpectatorArchive(chosen);
            g_netRunning = true;
            QLOG("[netplay] GO: reliable ready complete; host seeds "
                 "G=%08X C=%08X sel=%08X bootstrap=%016llX "
                 "timer=%u ai=%u life=%u roundsMinusOne=%u "
                 "local=P%d delay=%u rollback=%u",
                 chosen.seedGameplay, chosen.seedCosmetic, chosen.selector,
                 static_cast<unsigned long long>(
                     chosen.bootstrapHash),
                 chosen.timerMode, chosen.aiDifficulty,
                 chosen.lifeMultiplier,
                 chosen.roundsToWinMinusOne,
                 g_netLocalCtrl + 1, g_netDelay, g_netRollback);
        }
        netPaceSemanticHold();
        Sleep(1);
        return kNetHoldFrameRet;
    }

    qgame::netPublishGameThreadHeartbeat(
        qgame::NetTransportDiagnostics::GameThreadStage::DriverEnter,
        g_netSession ? g_netSession->currentFrame() : g_netFrame);
    netObserveRollbackHealth("driver");
    qgame::netPublishGameThreadHeartbeat(
        qgame::NetTransportDiagnostics::GameThreadStage::TransportDrain,
        g_netSession ? g_netSession->currentFrame() : g_netFrame);
    if (!netCheckLink(false) || !g_netSession
        || !netSetSimulationFrame(g_netSession->currentFrame())
        || !netDrainRemoteInputs()) {
        if (!g_netHalted)
            netFailClosed(kNetInternalFailure, g_netFrame,
                          "player transport/session update failed");
        return kNetHoldFrameRet;
    }
    if (!qgame::netPeerAlive()) {
        netFailClosed(kNetTimeout, g_netFrame,
                      "peer liveness timeout; simulation halted");
        return kNetHoldFrameRet;
    }
    qgame::netPublishGameThreadHeartbeat(
        qgame::NetTransportDiagnostics::GameThreadStage::Scheduler,
        g_netSession->currentFrame());

    const bool testPauseRequested =
        (g_netAutomatchHarnessEnabled || g_netTestPauseBarrierEnabled)
        && InterlockedCompareExchange(
               &g_netTestPauseRequest, 0, 0) != 0;
    if (testPauseRequested) {
        const LONG pauseFrame = InterlockedCompareExchange(
            &g_netTestPauseFrame, 0, 0);
        const uint32_t currentFrame = g_netSession->currentFrame();
        if (pauseFrame == 0
            || static_cast<uint32_t>(pauseFrame) <= currentFrame) {
            InterlockedExchange(
                &g_netTestPauseHeldFrame,
                static_cast<LONG>(currentFrame));
            if (!g_netHalted && !netPublishAndVerifyConfirmed())
                return kNetHoldFrameRet;
            bool tailSettled = true;
            if (pauseFrame != 0 && currentFrame != 0) {
                tailSettled = g_netHasConfirmed
                    && g_netConfirmedThrough + 1u == currentFrame;
                if (!tailSettled
                    && ++g_netTestPauseTailWaits >= kNetTestPauseTailWaitTicks) {
                    tailSettled = true;
                    QLOG("[netplay] test barrier frame=%u acknowledged with an "
                         "unsettled tail (confirmedThrough=%u hasConfirmed=%d)",
                         currentFrame, g_netConfirmedThrough,
                         g_netHasConfirmed ? 1 : 0);
                }
            }
            if (tailSettled)
                InterlockedExchange(&g_netTestPauseAcknowledged, 1);
            netObserveRollbackHealth(
                "adapter-hold", false,
                qgame::AdapterHoldReason::TestBarrier);
            netPaceSemanticHold();
            Sleep(1);
            return kNetHoldFrameRet;
        }
    }
    g_netTestPauseTailWaits = 0;
    InterlockedExchange(&g_netTestPauseAcknowledged, 0);

    if (!netApplyPhaseInputDelay()) {
        if (!g_netHalted) {
            netFailClosed(kNetInternalFailure, g_netFrame,
                          "phase input-delay transition failed");
        }
        return kNetHoldFrameRet;
    }
    if (g_netDelayDrain.active()
        && g_netSession->inputDelayChangeBlocker(
               static_cast<uint8_t>(g_netDelay))
            == qnetplay::InputDelayChangeBlocker::
                UnconfirmedBoundary) {
        if (!netPublishAndVerifyConfirmed())
            return kNetHoldFrameRet;
        netObserveRollbackHealth(
            "adapter-hold", false,
            qgame::AdapterHoldReason::DelayDrain);
        netPaceSemanticHold();
        Sleep(1);
        return kNetHoldFrameRet;
    }

    if (netPumpSessionEvents()) {
        if (!g_netHalted && !netPublishAndVerifyConfirmed())
            return kNetHoldFrameRet;
        if (!g_netHalted && g_netReturnToCharacterSelectPending) {
            netFinalizeCommittedCharacterSelectReturn();
            return 0;
        }
        netObserveRollbackHealth(
            "adapter-hold", false,
            qgame::AdapterHoldReason::SessionEvent);
        netPaceSemanticHold();
        if (!g_netHalted && g_rematchTheater.flipped
            && !g_sessionEvents.paused()
            && (g_rematchPrompt.deciding()
                || g_rematchPrompt.phase()
                       == qnetplay::RematchPromptPhase::CommitPending)) {
            (void)netRunRematchTheaterTick(orig);
        }
        Sleep(1);
        return kNetHoldFrameRet;
    }

    if (rollbackEligible()
        && (g_netGameplayRefreshPending || gameplayLayoutConfigChanged())) {
        reregisterFrameData();
        g_netGameplayRefreshPending = false;
        netObserveRollbackHealth(
            "adapter-hold", false,
            qgame::AdapterHoldReason::LayoutRefresh);
        netPaceSemanticHold();
        Sleep(1);
        return kNetHoldFrameRet;
    }

    if (!g_sessionCaptureFrozen && g_netSession->needsLocalCapture()) {
        int localNow = 0;
        bool physicalNeutralBeforeSocd = true;
        if (g_inputConfigPresent) {
            localNow =
                netManagedPhysicalCapture(physicalNeutralBeforeSocd);
        } else {
            DWORD pollException = 0;
            g_netPhysicalPoll = true;
            __try {
                (void)reinterpret_cast<PollFn>(g_fullInputPollFn)();
                localNow = g_origRead ? g_origRead(0) : 0;
            } __except (
                pollException = GetExceptionCode(),
                EXCEPTION_EXECUTE_HANDLER) {
            }
            g_netPhysicalPoll = false;
            if (pollException != 0) {
                char why[128];
                std::snprintf(
                    why, sizeof(why),
                    "physical input poll raised SEH exception 0x%08lX",
                    static_cast<unsigned long>(pollException));
                netFailClosed(kNetInternalFailure,
                              g_netSession->currentFrame(), why);
                return kNetHoldFrameRet;
            }
            physicalNeutralBeforeSocd = localNow == 0;
        }
        if (g_netAutomatchTest) {
            localNow = netAutomatchLocalInput(
                g_netSession->currentFrame());
            physicalNeutralBeforeSocd = localNow == 0;
        }
        if (g_rematchPrompt.advancing()) {
            localNow = netRematchAdvanceLocalInput(
                g_netSession->currentFrame());
            physicalNeutralBeforeSocd = localNow == 0;
        }

        const bool directBootActive =
            qgame::directBootNetworkActive();
        const bool productionPhysicalInput = !g_netAutomatchTest;
        const bool wasAwaitingNeutral =
            g_netDirectBootInputGate.awaitingNeutral();
        const uint32_t sampledLocalInput =
            static_cast<uint32_t>(localNow);
        uint32_t gateSample = sampledLocalInput;
        if (wasAwaitingNeutral && !physicalNeutralBeforeSocd
            && gateSample == 0u) {
            gateSample = qinput::kLogicalLeft | qinput::kLogicalRight;
        }
        localNow = static_cast<int>(g_netDirectBootInputGate.filter(
            directBootActive,
            productionPhysicalInput,
            gateSample));
        const bool awaitingNeutral =
            g_netDirectBootInputGate.awaitingNeutral();
        if (!wasAwaitingNeutral && awaitingNeutral) {
            g_netDirectBootLastBlockedInput = UINT32_MAX;
            QLOG("[netplay-input] DirectBoot capture gate engaged frame=%u "
                 "source=%s sampled=%08X submitted=00000000",
                 g_netSession->currentFrame(),
                 productionPhysicalInput ? "physical" : "test-automation",
                 sampledLocalInput);
        } else if (wasAwaitingNeutral && !awaitingNeutral) {
            g_netDirectBootLastBlockedInput = UINT32_MAX;
            QLOG("[netplay-input] DirectBoot capture gate released frame=%u "
                 "source=%s reason=%s",
                 g_netSession->currentFrame(),
                 productionPhysicalInput ? "physical" : "test-automation",
                 productionPhysicalInput
                     ? "neutral-hardware-sample"
                     : "exact-opt-in-automation");
        } else if (wasAwaitingNeutral && awaitingNeutral
                   && !directBootActive && productionPhysicalInput
                   && sampledLocalInput != 0u
                   && sampledLocalInput
                       != g_netDirectBootLastBlockedInput) {
            g_netDirectBootLastBlockedInput = sampledLocalInput;
            QLOG("[netplay-input] blocked held post-DirectBoot input frame=%u "
                 "sampled=%08X submitted=00000000; waiting for neutral",
                 g_netSession->currentFrame(), sampledLocalInput);
        }

        if (g_netAutomatchTest) {
            const bool combatInput =
                static_cast<int>(rd32(kSceneStateAddr)) == 2
                && static_cast<int>(rd32(kMatchSubAddr)) == 0;
            const bool shouldLog =
                (combatInput && g_netAutomatchCombatInputLogs < 16)
                || (!combatInput && g_netAutomatchInputLogs < 16)
                || (localNow != 0
                    && (combatInput
                        ? !g_netAutomatchCombatNonzeroLogged
                        : !g_netAutomatchFrontendNonzeroLogged))
                || (g_netSession->currentFrame() % 600u) == 0u;
            if (shouldLog) {
                QLOG("[test-automatch] input frame=%u local=P%d "
                     "scene=%d matchSub=%d combat=%d mask=%08X "
                     "result=%u round=%u wins=[%u,%u] finishTimers=%d/%d",
                     g_netSession->currentFrame(), g_netLocalCtrl + 1,
                     static_cast<int>(rd32(kSceneStateAddr)),
                     static_cast<int>(rd32(kMatchSubAddr)),
                     static_cast<int>(combatInput),
                     static_cast<unsigned>(localNow),
                     rd32(kRoundResultStateAddr), rd32(0x871950u),
                     rd32(0x625628u), rd32(0x62562Cu),
                     static_cast<int32_t>(rd32(0x4AA3D0u)),
                     static_cast<int32_t>(rd32(0x4AA408u)));
                if (combatInput)
                    ++g_netAutomatchCombatInputLogs;
                else
                    ++g_netAutomatchInputLogs;
                if (localNow != 0) {
                    if (combatInput)
                        g_netAutomatchCombatNonzeroLogged = true;
                    else
                        g_netAutomatchFrontendNonzeroLogged = true;
                }
            }
        }
        if (!g_netSession->captureLocalInput(
                g_netSession->currentFrame(), packLogical(localNow))
            || !netFlushLocalSubmissions()) {
            if (!g_netHalted)
                netFailClosed(kNetInternalFailure,
                              g_netSession->currentFrame(),
                              "local delayed-input capture failed");
            return kNetHoldFrameRet;
        }
    }
    netObserveInputProduction(
        g_netSession->localInputProducedThrough());
    g_netPredictionDepth.add(
        g_netSession->diagnostics().speculativeFrames);

    const bool sessionEventDrain =
        g_sessionCaptureFrozen
        && g_sessionEvents.current().state
               == qinput::SessionEventState::Prepared;
    qnetplay::FrameAction action = sessionEventDrain
        ? g_netSession->nextQuiesceDrainAction()
        : g_netSession->nextAction();
    netObserveRollbackHealth("action");
    netRegulatePaceSlew();
    if (action.kind == qnetplay::ActionKind::Rollback) {
        netObserveRollbackHealth(
            "adapter-hold", false,
            qgame::AdapterHoldReason::CorrectionWait);
        netObserveRunState(qgame::WorkKind::Correct, qgame::HoldCause::None);
        g_netHoldLedger.observeRecovery(
            qnet::frameAfter(action.rollbackFrom, action.frame)
                ? qnet::forwardDistance(action.frame, action.rollbackFrom)
                : 0u);
        const uint64_t correctionStartedUs = netRealQpcUs();
        const NetReplayCorrectionResult correction =
            netReplayCorrection(orig, action);
        const uint64_t correctionFinishedUs = netRealQpcUs();
        if (correctionFinishedUs >= correctionStartedUs)
            g_netPacingCorrectionWorkUs +=
                correctionFinishedUs - correctionStartedUs;
        ++g_netPacingCorrectionBatches;
        g_netFrame = g_netSession->currentFrame();
        if (correction.succeeded && correction.committedBoundary) {
            netObserveRollbackHealth(
                "adapter-runnable", false,
                qgame::AdapterHoldReason::None);
            return correction.dispatchResult;
        }
        netPaceSemanticHold();
        Sleep(1);
        return kNetHoldFrameRet;
    }
    if (action.kind == qnetplay::ActionKind::Stall) {
        ++g_netPacingSessionStalls;
        {
            const uint32_t frontierNow = g_netSession->currentFrame();
            const uint64_t nowUs = netRealQpcUs();
            if (frontierNow != g_netStallDeadlineFrame) {
                g_netStallDeadlineFrame = frontierNow;
                g_netStallDeadlineSinceUs = nowUs;
                g_netStallDeadlineWarned = false;
            } else if (g_netStallDeadlineSinceUs != 0) {
                const uint64_t heldUs = nowUs - g_netStallDeadlineSinceUs;
                if (!g_netStallDeadlineWarned
                    && heldUs >= kNetStallWarnUs) {
                    g_netStallDeadlineWarned = true;
                    QLOG("[progress-deadline] no canonical progress for %llu ms "
                         "at frame=%u (peer is alive on the wire but has not "
                         "produced input). Terminating at %llu ms.",
                         static_cast<unsigned long long>(heldUs / 1000ull),
                         frontierNow,
                         static_cast<unsigned long long>(kNetStallAbortUs / 1000ull));
                }
                if (heldUs >= kNetStallAbortUs) {
                    QLOG("[progress-deadline] ABORT: %llu ms without a single "
                         "canonical frame at frame=%u",
                         static_cast<unsigned long long>(heldUs / 1000ull),
                         frontierNow);
                    netFailClosed(kNetTimeout, frontierNow,
                                  "the other player's game stopped responding",
                                  true);
                    return kNetHoldFrameRet;
                }
            }
        }
        (void)netPublishAndVerifyConfirmed();
        netObserveRollbackHealth(
            "adapter-runnable", false,
            qgame::AdapterHoldReason::None);
        const qgame::HoldCause stallCause =
            action.stallReason == qnetplay::StallReason::PredictionLimit
                ? qgame::HoldCause::PredictionLimit
                : qgame::HoldCause::LocalInputMissing;
        netPaceSemanticHold(stallCause);
        netObserveRunState(qgame::WorkKind::Hold, stallCause);
        (void)netProduceLocalInputWhileStalled();
        Sleep(1);
        return kNetHoldFrameRet;
    }

    qgame::FrameDomains domains{};
    domains.simFrontier = g_netSession->currentFrame();
    domains.remoteActualPrefix =
        g_netSession->diagnostics().nextRemoteExpected;
    domains.localMaxRollback = static_cast<uint8_t>(g_netRollback);
    if (g_netFaHoldCooldown > 0)
        --g_netFaHoldCooldown;
    if (rollbackEligible() && g_netFaHoldCooldown == 0
        && qgame::netPeerHasSimFrontier()) {
        const uint64_t ageMs = qgame::netPeerFrontierAgeMs();
        if (ageMs <= kNetPeerFrontierFreshMs) {
            const uint32_t rttMs =
                qgame::netPeerHasRtt() ? qgame::netPeerRttMs() : 0u;
            const uint64_t staleMs = ageMs + rttMs / 2u;
            const uint32_t staleFrames = static_cast<uint32_t>(
                (staleMs * 6060ull + 50000ull) / 100000ull);
            domains.peerSimFrontier =
                qgame::netPeerSimFrontier() + staleFrames;
            domains.peerSampleFresh = true;
        }
    }
    qgame::OpportunityInputs opportunity{};
    opportunity.cadenceOpportunity = true;
    opportunity.localInputReady = true;
    opportunity.remoteActualAtFrontier =
        !action.inputs.remotePredicted
        && netRemoteActualThrough(action.frame);
    opportunity.lifecycleExactBoundary =
        netLifecycleTransitionRequiresExactInput();
    opportunity.sessionEventDrain = sessionEventDrain;
    opportunity.recoverableDebtFrames = g_netCadenceDebt.frames();
    opportunity.safeForwardHeadroom = 0;
    if (rollbackEligible() && !opportunity.lifecycleExactBoundary) {
        const uint32_t depth = qgame::predictionDepth(domains);
        const uint32_t ceiling =
            static_cast<uint32_t>(domains.localMaxRollback);
        if (ceiling > depth)
            if (ceiling > depth + 1u)
                opportunity.safeForwardHeadroom = ceiling - depth - 1u;
    }
    uint32_t roundIntroPhase = 0;
    uint32_t roundIntroTimer = 0;
    if (netReadRoundIntroPresentationState(
            roundIntroPhase, roundIntroTimer)
        && qgame::roundIntroRotationPresentationCritical(
            roundIntroPhase, roundIntroTimer)) {
        opportunity.safeForwardHeadroom = 0u;
    }
    const qgame::SchedulePlan plan =
        qgame::planSchedule(domains, opportunity, kNetSchedulePolicy);
    if (sessionEventDrain && plan.forwardFrames > 1u) {
        netFailClosed(
            kNetInternalFailure, g_netSession->currentFrame(),
            "session-event drain scheduled a multi-frame catch-up");
        return kNetHoldFrameRet;
    }
    if (plan.kind == qgame::WorkKind::Hold) {
        if (plan.hold == qgame::HoldCause::LifecycleBoundary) {
            ++g_netPacingLifecycleHolds;
        }
        netObserveRollbackHealth(
            "adapter-hold", false,
            netAdapterHoldReasonForHoldCause(plan.hold));
        netObserveRunState(plan.kind, plan.hold);
        netPaceSemanticHold(plan.hold);
        (void)netProduceLocalInputWhileStalled();
        Sleep(1);
        return kNetHoldFrameRet;
    }

    netObserveRunState(plan.kind, qgame::HoldCause::None);

    qnetplay::FrameAction frameAction = action;
    NetAdvanceOutcome out{};
    uint32_t executed = 0;
    uint32_t presentedThisBatch = 0;
    bool advanceFailed = false;
    if (plan.forwardFrames > 1u) {
        ++g_netCatchupBatches;
        if (g_netCadenceDebt.frames() == 0u)
            g_netFaHoldCooldown = kNetFaHoldCooldownOpportunities;
    }
    const uint64_t batchStartUs = netRealQpcUs();
    uint32_t lastIndex = plan.forwardFrames - 1u;
    for (uint32_t i = 0; i < plan.forwardFrames; ++i) {
        if (i != 0) {
            if (i < lastIndex
                && netRealQpcUs() - batchStartUs
                    >= kNetCatchupBatchBudgetUs) {
                ++g_netCatchupBudgetStops;
                lastIndex = i;
            }
            if (!g_sessionCaptureFrozen
                && g_netSession->needsLocalCapture()
                && (!g_netSession->captureLocalInput(
                        g_netSession->currentFrame(),
                        g_netSession->lastCapturedWord())
                    || !netFlushLocalSubmissions())) {
                if (!g_netHalted)
                    netFailClosed(kNetInternalFailure,
                                  g_netSession->currentFrame(),
                                  "catch-up repeat capture failed");
                return kNetHoldFrameRet;
            }
            frameAction = sessionEventDrain
                ? g_netSession->nextQuiesceDrainAction()
                : g_netSession->nextAction();
            if (frameAction.kind != qnetplay::ActionKind::Advance) {
                break;
            }
        }
        out = NetAdvanceOutcome{};
        const bool hiddenTick =
            qgame::playerCatchupTickVisibility(i, lastIndex)
                == qgame::NativeTickVisibility::HiddenNewForward;
        const uint64_t tickStartUs = hiddenTick ? netRealQpcUs() : 0u;
        if (!netAdvanceOneCanonicalFrame(
                orig, frameAction, hiddenTick, out)) {
            advanceFailed = true;
            break;
        }
        if (hiddenTick)
            g_netHiddenTickUs.add(netRealQpcUs() - tickStartUs);
        else
            ++presentedThisBatch;
        ++executed;
        if (i >= lastIndex)
            break;
        if (out.result != kNetHoldFrameRet)
            break;
    }
    if (executed == 0)
        return kNetHoldFrameRet;
    if (presentedThisBatch == 0)
        ++g_netStarvedOpportunities;
    if (executed == plan.forwardFrames) {
        g_netCadenceDebt.apply(plan);
    } else {
        if (executed > 1u) {
            qgame::SchedulePlan credited = plan;
            credited.debt = qgame::DebtDisposition::Consume;
            credited.debtConsumed = executed - 1u;
            g_netCadenceDebt.apply(credited);
        }
        (void)netConsumePresentationSlot(
            InterlockedCompareExchange(&g_netOwnedTickSerial, 0, 0));
    }
    if (g_lastPacePoll.rebased()) {
        qgame::applyRebasedFrames(g_netCadenceDebt,
                                  g_lastPacePoll.rebasedDebtFrames,
                                  g_netLastHoldCause);
        g_lastPacePoll = {};
    }
    g_netLastHoldCause = qgame::HoldCause::None;
    if (advanceFailed)
        return kNetHoldFrameRet;
    const uint32_t sceneBefore = out.phaseBefore >> 16;
    if (qgame::spectatorSourceStopsOnDispatchReturn(sceneBefore, out.result)) {
        netSealSpectatorArchive(
            qnetplay::spectator::TerminalReason::SourceStopped);
    }
    return out.result;
}

bool spectatorSavePreFrame(uint32_t frame, uint32_t phase,
                           uint64_t& preHash) {
    const size_t slot = frame % kPoolSlots;
    if (!g_rb.saveTaggedToSlot(slot, g_netEpoch, frame, phase)) {
        netFailClosed(kNetInternalFailure, frame,
                      "spectator pre-frame snapshot failed", false);
        return false;
    }
    netCaptureFpu(slot, g_netEpoch, frame);
    preHash = netTaggedPreHash(slot, g_netEpoch, frame);
    if (!preHash) {
        netFailClosed(kNetInternalFailure, frame,
                      "spectator canonical pre-state hash failed", false);
        return false;
    }
    return true;
}

void netSpectatorNameRecorderLanes() {
    if (!qgame::replay::replayRecorderV2Active()) return;
    std::array<std::string, 2> names{};
    for (size_t lane = 0u; lane < 2u; ++lane) {
        names[lane] = qgame::playerNameForUtf8Display(
            g_netPlayerNames[lane], g_netPlayerNameCodePages[lane]);
    }
    const bool named =
        qgame::replay::replayRecordSetSourceLaneNamesV2(names);
    QLOG("[replay.record-v2] spectator lanes named=%d p1='%s' p2='%s'",
         named ? 1 : 0, names[0].c_str(), names[1].c_str());
}

int doSpectator(FrameDispatchFn orig) {
    using namespace qnetplay::spectator;
    if (!g_netActive || !g_netSpectatorMode)
        return orig ? orig() : kNetHoldFrameRet;
    if (g_netHalted) {
        return netPostTerminalHold();
    }
    if (InterlockedCompareExchange(
            &g_netPacingClockFault, 0, 0) != 0) {
        netFailClosed(
            kNetInternalFailure, g_netFrame,
            "spectator canonical presentation clock failed", false);
        return kNetHoldFrameRet;
    }
    if (rd32(0x626860) == 0)
        return orig ? orig() : kNetHoldFrameRet;
    if (rd32(kNetIsNetworkAddr) != 0u) {
        netFailClosed(
            kNetInvalidConfiguration, rd32(kNetIsNetworkAddr),
            "spectator dual-slot playback left QOH local mode", false);
        return kNetHoldFrameRet;
    }
    const bool pacingConfigReady = !g_spectatorPlaybackStarted
        ? netNormalizeLocalSchedulerConfig()
        : netLocalSchedulerConfigIsNormalized();
    if (!pacingConfigReady) {
        netFailClosed(
            kNetInvalidConfiguration, 0,
            "spectator local scheduler config could alter deterministic execution",
            false);
        return kNetHoldFrameRet;
    }
    netApplyExtraFunctionPolicy();
    if (!g_readHooked || !g_pollHooked || !g_paceBracketHooked
        || !g_clockHooked || !g_configSaveHooked
        || !g_fCommandPolicyHooked || !g_fCommandPolicyApplied
        || !playbackSideEffectHooksReady()) {
        netFailClosed(kNetInternalFailure, 0,
                      "mandatory spectator playback hook disappeared", false);
        return kNetHoldFrameRet;
    }

    qgame::SpectatorViewerStatus status{};
    if (!qgame::spectatorViewerGetStatus(status) || !status.active) {
        netFailClosed(kNetTransportFailure, 0,
                      "spectator transport state is unavailable", false);
        return kNetHoldFrameRet;
    }
    if (status.phase == ViewerPhase::Rejected) {
        RejectPayload rejection{};
        (void)qgame::spectatorViewerGetRejection(rejection);
        char why[192];
        std::snprintf(why, sizeof(why),
                      "spectator source rejected this viewer: %s (reason=%u)",
                      spectatorRejectReasonText(rejection.reason),
                      static_cast<unsigned>(rejection.reason));
        netFailClosed(kNetProtocolViolation,
                      static_cast<uint32_t>(rejection.reason), why, false);
        return kNetHoldFrameRet;
    }
    if (status.phase == ViewerPhase::Failed) {
        char why[128];
        const bool routeLost =
            status.failure == IngressResult::RouteLost;
        std::snprintf(
            why, sizeof(why),
            routeLost
                ? "spectator relay route was closed (reason=%u)"
                : "spectator protocol failed (reason=%u)",
            static_cast<unsigned>(status.failure));
        netFailClosed(
            routeLost ? kNetTransportFailure : kNetProtocolViolation,
            static_cast<uint32_t>(status.failure), why, false);
        return kNetHoldFrameRet;
    }

    const uint64_t nowMs = GetTickCount64();
    if (!g_spectatorStartReceived && status.hasGameStart) {
        if (!qgame::spectatorViewerGetGameStart(g_spectatorGameStart)) {
            netFailClosed(kNetProtocolViolation, 0,
                          "spectator GameStart vanished", false);
            return kNetHoldFrameRet;
        }
        g_spectatorStartReceived = true;
        g_spectatorStartReceivedMs = nowMs;
    }
    if (!g_spectatorStartReceived) {
        switch (qgame::spectatorStartWaitVerdict(
            status.sawSourceContact, status.sourceContactAgeMs,
            nowMs - g_spectatorArmMs)) {
        case qgame::SpectatorStartWait::SourceGone:
            netFailClosed(
                kNetTimeout,
                static_cast<uint32_t>(status.sourceContactAgeMs),
                "the host stopped answering while we waited for the match "
                "to start",
                false);
            return kNetHoldFrameRet;
        case qgame::SpectatorStartWait::NeverAnswered:
            netFailClosed(
                kNetTransportFailure, 0,
                "the host never answered this spectator after the route was "
                "proved",
                false);
            return kNetHoldFrameRet;
        case qgame::SpectatorStartWait::Expired:
            netFailClosed(kNetTimeout, 0,
                          "spectator GameStart timed out", false);
            return kNetHoldFrameRet;
        case qgame::SpectatorStartWait::KeepWaiting:
        default:
            break;
        }
        spectatorJoinTelemetryTick(status, nowMs);
        Sleep(1);
        return kNetHoldFrameRet;
    }

    if (!g_spectatorPlaybackStarted) {
        qgame::spectatorPublishPlaybackWait(qgame::SpectatorWait::Joining,
                                            status.bufferedRecords > 0xFFFFu
                                                ? 0xFFFFu
                                                : static_cast<uint32_t>(
                                                      status.bufferedRecords),
                                            0u);
        spectatorJoinTelemetryTick(status, nowMs);
        qgame::NetReadyState ready{};
        uint32_t expectedPhase = 0;
        if (!netParseSpectatorMetadata(
                g_spectatorGameStart, ready, expectedPhase)
            || g_spectatorGameStart.simulationContract
                != qipc::kRuntimeCompatibilityId
            || g_spectatorGameStart.firstTimelineFrame != 0
            || g_spectatorGameStart.resumeRecord != 0
            || g_spectatorGameStart.initialStateHash == 0) {
            netFailClosed(kNetInvalidConfiguration, 0,
                          "spectator simulation contract/metadata mismatch",
                          false);
            return kNetHoldFrameRet;
        }

        if (g_spectatorWantsLatest
            && g_spectatorSkip == SpectatorSkipState::Off) {
            g_spectatorSkip = SpectatorSkipState::Waiting;
            g_spectatorSkipArmedMs = nowMs;
            QLOG("[set-seek] waiting for the source's newest checkpoint "
                 "(--latest); the from-zero walk keeps running as the "
                 "fallback");
        }
        if (g_spectatorSkip == SpectatorSkipState::Waiting) {
            qnetplay::spectator::Checkpoint checkpoint{};
            if (qgame::spectatorViewerTakeCheckpoint(checkpoint)) {
                const qnetplay::spectator::CheckpointRegion* state = nullptr;
                const qnetplay::spectator::CheckpointRegion* boot = nullptr;
                for (const auto& region : checkpoint.regions) {
                    if (region.id == 1u) state = &region;
                    if (region.id == 2u) boot = &region;
                }
                qgame::ReplayDirectBootConfig bootConfig{};
                qnetplay::spectator::SpectatorSetTimeline::BindSeed seed{};
                if (state != nullptr && boot != nullptr
                    && checkpoint.regionSchema == 0x51524231u
                    && unpackSetSeekBoot(boot->bytes.data(),
                                         boot->bytes.size(), bootConfig,
                                         seed)
                    && qgame::startReplayDirectBootToMatch(bootConfig)) {
                    g_spectatorSkipCheckpoint = std::move(checkpoint);
                    g_spectatorSkipBoot = bootConfig;
                    g_spectatorSkipSeed = seed;
                    g_spectatorSkipReady = ready;
                    g_spectatorSkip = SpectatorSkipState::Booting;
                    g_spectatorSkipHoldTicks = 0;
                    QLOG("[set-seek] checkpoint accepted: match %u record %u "
                         "event %u state=%zuB; booting its roster",
                         seed.openMatchId,
                         g_spectatorSkipCheckpoint.identity.recordCursor,
                         g_spectatorSkipCheckpoint.eventCursor,
                         state->bytes.size());
                } else {
                    QLOG("[set-seek] checkpoint refused (regions/descriptor/"
                         "boot); falling back to watching from the start");
                    g_spectatorSkip = SpectatorSkipState::Off;
                    g_spectatorWantsLatest = false;
                }
            } else if (((++g_spectatorSkipHoldTicks) % 300u) == 0u) {
                uint32_t chunksSeen = 0u;
                uint32_t chunkCount = 0u;
                const bool offered = qgame::spectatorViewerCheckpointProgress(
                    chunksSeen, chunkCount);
                QLOG("[set-seek] waiting: offer=%d chunks=%u/%u",
                     offered ? 1 : 0, chunksSeen, chunkCount);
            }
            if (g_spectatorSkip == SpectatorSkipState::Waiting
                && nowMs - g_spectatorSkipArmedMs
                       > qgame::kSpectatorSetSeekWaitMs) {
                QLOG("[set-seek] no checkpoint within %llums; watching "
                     "from the start instead",
                     static_cast<unsigned long long>(
                         qgame::kSpectatorSetSeekWaitMs));
                g_spectatorSkip = SpectatorSkipState::Off;
                g_spectatorWantsLatest = false;
            }
            if (g_spectatorSkip == SpectatorSkipState::Waiting) {
                if (qgame::directBootNetworkActive()
                    && !qgame::directBootNetworkAtStartBarrier()) {
                    qgame::directBootNetworkTick();
                }
                g_netInject[0] = g_netInject[1] = 0;
                return orig ? orig() : kNetHoldFrameRet;
            }
        }
        if (g_spectatorSkip == SpectatorSkipState::Booting) {
            if (qgame::directBootNetworkActive())
                qgame::directBootNetworkTick();
            if (qgame::directBootNetworkFailed()
                || ++g_spectatorSkipHoldTicks > 3600u) {
                netFailClosed(kNetInternalFailure, g_spectatorSkipHoldTicks,
                              "set-seek roster boot failed", false);
                return kNetHoldFrameRet;
            }
            if (!qgame::directBootNetworkActive()) {
                g_spectatorSkip = SpectatorSkipState::Restoring;
                g_spectatorSkipHoldTicks = 0;
            }
            g_netInject[0] = g_netInject[1] = 0;
            return orig ? orig() : kNetHoldFrameRet;
        }
        if (g_spectatorSkip == SpectatorSkipState::Restoring) {
            (void)ensureRollback();
            if (!g_rbReady || g_rb.totalSize() == 0) {
                if (++g_spectatorSkipHoldTicks > 600u) {
                    netFailClosed(kNetInternalFailure, 0,
                                  "set-seek layout never became ready",
                                  false);
                    return kNetHoldFrameRet;
                }
                g_netInject[0] = g_netInject[1] = 0;
                return orig ? orig() : kNetHoldFrameRet;
            }
            const qnetplay::spectator::CheckpointRegion* state = nullptr;
            for (const auto& region : g_spectatorSkipCheckpoint.regions)
                if (region.id == 1u) state = &region;
            if (state == nullptr
                || state->bytes.size() != g_rb.totalSize()) {
                netFailClosed(
                    kNetInvalidConfiguration,
                    state ? static_cast<uint32_t>(state->bytes.size()) : 0u,
                    "set-seek layout size mismatch", false);
                return kNetHoldFrameRet;
            }
            if (!g_rb.loadFromForeign(state->bytes.data())) {
                if (++g_spectatorSkipHoldTicks > 600u) {
                    netFailClosed(kNetInternalFailure,
                                  g_rb.layoutGeneration(),
                                  "set-seek state load kept failing", false);
                    return kNetHoldFrameRet;
                }
                if ((g_spectatorSkipHoldTicks % 120u) == 0u) {
                    QLOG("[set-seek] loadFrom not accepted yet "
                         "(layoutGen=%u totalSize=%zu, try %u)",
                         g_rb.layoutGeneration(), g_rb.totalSize(),
                         g_spectatorSkipHoldTicks);
                }
                g_netInject[0] = g_netInject[1] = 0;
                return orig ? orig() : kNetHoldFrameRet;
            }
            const qnetplay::spectator::CheckpointRegion* audio = nullptr;
            for (const auto& region : g_spectatorSkipCheckpoint.regions)
                if (region.id == 3u) audio = &region;
            if (!netRestoreCheckpointAudioRegion(
                    audio ? audio->bytes.data() : nullptr,
                    audio ? audio->bytes.size() : 0u,
                    g_spectatorSkipCheckpoint.identity.recordCursor)) {
                netFailClosed(kNetInvalidConfiguration, 3u,
                              "set-seek checkpoint audio region is absent "
                              "or malformed",
                              false);
                return kNetHoldFrameRet;
            }
            netResetFpuContract();
            const uint64_t restoredHash = netLiveTaggedGameplayHash(
                g_spectatorSkipCheckpoint.identity.recordCursor);
            if (g_spectatorSkipCheckpoint.stateHash == 0
                || restoredHash != g_spectatorSkipCheckpoint.stateHash) {
                netFailClosed(
                    kNetConfirmedDesync,
                    g_spectatorSkipCheckpoint.identity.recordCursor,
                    "set-seek restored state hash differs from checkpoint",
                    false);
                QLOG("[set-seek] restored=%016llX checkpoint=%016llX",
                     static_cast<unsigned long long>(restoredHash),
                     static_cast<unsigned long long>(
                         g_spectatorSkipCheckpoint.stateHash));
                netLogCompositeHashProbe(
                    "restore", g_spectatorSkipCheckpoint.identity.recordCursor);
                return kNetHoldFrameRet;
            }
            if (!qgame::spectatorViewerResumeAtCheckpoint(
                    g_spectatorSkipCheckpoint.identity.recordCursor,
                    g_spectatorSkipCheckpoint.eventCursor,
                    g_spectatorSkipCheckpoint.digestChainPrefix,
                    g_spectatorSkipSeed)) {
                netFailClosed(kNetInternalFailure, 1,
                              "set-seek session rebind refused", false);
                return kNetHoldFrameRet;
            }
            netResetFpuContract();
            if (!netApplyAuthoritativeMatchConfig(g_spectatorSkipReady)) {
                netFailClosed(kNetInvalidConfiguration, 0,
                              "set-seek cannot apply match config", false);
                return kNetHoldFrameRet;
            }
            g_netEpoch = 1;
            netPaletteResetForSession();
            g_netBootstrapTransientsEpoch = g_netEpoch;
            g_netFrame =
                g_spectatorSkipCheckpoint.identity.recordCursor;
            g_spectatorTimelineFrame =
                g_spectatorSkipCheckpoint.identity.recordCursor;
            g_spectatorSetEventsConsumed =
                g_spectatorSkipCheckpoint.eventCursor;
            g_spectatorReturnScanCursor =
                g_spectatorSkipCheckpoint.eventCursor;
            g_spectatorReturnPending = false;
            g_netGameplaySegmentActive = false;
            g_netGameplaySegmentSeen = false;
            g_netGameplayRefreshPending = false;
            g_netFpuInitializedEpoch = 0;
            netInvalidateFpu();
            g_spectatorPlaybackStarted = true;
            netSpectatorNameRecorderLanes();
            g_spectatorSkip = SpectatorSkipState::Off;
            QLOG("[set-seek] JOINED at record %u (event %u, match %u): the "
                 "restored state verifies against every record hash from "
                 "here on",
                 g_spectatorSkipCheckpoint.identity.recordCursor,
                 g_spectatorSkipCheckpoint.eventCursor,
                 g_spectatorSkipSeed.openMatchId);
            QLOG("[spectator] PLAY game=%llu checkpoint=%016llX record=%u "
                 "phase=%08X (set-seek join)",
                 static_cast<unsigned long long>(
                     g_spectatorSkipCheckpoint.identity.gameId),
                 static_cast<unsigned long long>(
                     g_spectatorSkipCheckpoint.identity.checkpointId),
                 g_spectatorSkipCheckpoint.identity.recordCursor,
                 netPhaseTag());
            // Never call orig() here: one native tick would advance the restored canonical pre-state with non-canonical inputs and fail the archive hash check.
            return kNetHoldFrameRet;
        }
        if (netPhaseTag() != expectedPhase
            || (qgame::directBootNetworkActive()
                && !qgame::directBootNetworkAtStartBarrier())) {
            if (nowMs - g_spectatorStartReceivedMs
                >= qgame::kSpectatorStartRendezvousTimeoutMs) {
                netFailClosed(
                    kNetTimeout, netPhaseTag(),
                    "spectator DirectBoot start rendezvous timed out",
                    false);
                return kNetHoldFrameRet;
            }
            g_netInject[0] = g_netInject[1] = 0;
            return orig ? orig() : kNetHoldFrameRet;
        }
        netResetFpuContract();
        const uint64_t localBootstrap =
            netBootstrapFingerprint();
        if (localBootstrap == 0) {
            netFailClosed(kNetInvalidConfiguration, 0,
                          "spectator canonical audio manifest is incomplete",
                          false);
            return kNetHoldFrameRet;
        }
        if (localBootstrap != ready.bootstrapHash) {
            netFailClosed(
                kNetConfirmedDesync,
                static_cast<uint32_t>(
                    localBootstrap ^ ready.bootstrapHash),
                "spectator frontend bootstrap fingerprint differs",
                false);
            return kNetHoldFrameRet;
        }
        if (!netApplyAuthoritativeMatchConfig(ready)) {
            netFailClosed(
                kNetInvalidConfiguration, 0,
                "spectator cannot apply authoritative match config",
                false);
            return kNetHoldFrameRet;
        }
        wr32(0x4A9938, ready.seedGameplay);
        wr32(0x4A993C, ready.seedCosmetic);
        wr32(0x4ABB24, ready.selector);
        const uint64_t localInitialState =
            netSpectatorLiveStateHash();
        if (localInitialState
            != g_spectatorGameStart.initialStateHash) {
            netFailClosed(
                kNetConfirmedDesync,
                static_cast<uint32_t>(
                    localInitialState
                    ^ g_spectatorGameStart.initialStateHash),
                "spectator initial canonical state differs from archive",
                false);
            return kNetHoldFrameRet;
        }
        g_netEpoch = 1;
        netPaletteResetForSession();
        g_netBootstrapTransientsEpoch = 0;
        g_netFrame = g_spectatorGameStart.firstTimelineFrame;
        g_spectatorTimelineFrame =
            g_spectatorGameStart.firstTimelineFrame;
        g_netGameplaySegmentActive = false;
        g_netGameplaySegmentSeen = false;
        g_netGameplayRefreshPending = false;
        g_netFpuInitializedEpoch = 0;
        netInvalidateFpu();
        g_spectatorPlaybackStarted = true;
        netSpectatorNameRecorderLanes();
        QLOG("[spectator] PLAY game=%llu seed=%08X/%08X/%08X "
             "phase=%08X delay=[%u,%u] rollback=%u",
             static_cast<unsigned long long>(
                 g_spectatorGameStart.gameId),
             ready.seedGameplay, ready.seedCosmetic, ready.selector,
             expectedPhase, g_spectatorGameStart.playerOneDelay,
             g_spectatorGameStart.playerTwoDelay,
             g_spectatorGameStart.rollbackWindow);
    }

    const bool sourceContactFresh =
        status.sawSourceContact
        && status.sourceContactAgeMs <= kSpectatorHeartbeatFreshMs;
    if (!status.linkAlive && !status.hasTerminal
        && status.phase == ViewerPhase::Streaming
        && !sourceContactFresh
        && nowMs - g_spectatorLastResumeMs >= 5000u) {
        g_spectatorLastResumeMs = nowMs;
        if (!qgame::spectatorViewerResume()) {
            netFailClosed(kNetTransportFailure, g_netFrame,
                          "spectator resume failed", false);
            return kNetHoldFrameRet;
        }
        QLOG("[spectator] resuming at playback=%u received=%u "
             "(no source contact for %llums)",
             status.playbackNextRecord, status.receivedNextRecord,
             static_cast<unsigned long long>(status.sourceContactAgeMs));
    }
    if (status.linkAlive || status.hasTerminal || sourceContactFresh) {
        g_spectatorLinkLostSinceMs = 0;
    } else if (g_spectatorPlaybackStarted) {
        if (g_spectatorLinkLostSinceMs == 0) {
            g_spectatorLinkLostSinceMs = nowMs;
        } else if (nowMs - g_spectatorLinkLostSinceMs >= 45000u) {
            netFailClosed(
                kNetTimeout, status.playbackNextRecord,
                "spectator source remained unavailable after resume attempts",
                false);
            return kNetHoldFrameRet;
        }
    }

    if (rollbackEligible()
        && (g_netGameplayRefreshPending || gameplayLayoutConfigChanged())) {
        if (g_spectatorLayoutHoldSinceMs == 0)
            g_spectatorLayoutHoldSinceMs = nowMs;
        if (++g_spectatorLayoutHoldStreak % 300u == 0u) {
            QLOG("[spectator] layout-refresh hold streak=%u held=%llums "
                 "(pending=%d)",
                 g_spectatorLayoutHoldStreak,
                 static_cast<unsigned long long>(
                     nowMs - g_spectatorLayoutHoldSinceMs),
                 g_netGameplayRefreshPending ? 1 : 0);
        }
        if (nowMs - g_spectatorLayoutHoldSinceMs
            >= qgame::kSpectatorLayoutHoldDeadlineMs) {
            netFailClosed(kNetInternalFailure, g_spectatorLayoutHoldStreak,
                          "spectator gameplay layout never settled", false);
            return kNetHoldFrameRet;
        }
        reregisterFrameData();
        g_netGameplayRefreshPending = false;
        ++g_netPacingSessionStalls;
        netPaceSemanticHold();
        Sleep(1);
        return kNetHoldFrameRet;
    }
    g_spectatorLayoutHoldSinceMs = 0;
    g_spectatorLayoutHoldStreak = 0;

    spectatorApplyPendingLocalCommands();
    spectatorTelemetryTick(status, nowMs);
    spectatorTryRegisterHotkeySink(nowMs);

    const bool sourceExhausted = qgame::spectatorSourceHoldsNothingMore(
        status.sourceFrontierKnown, status.sourceNextRecord,
        status.receivedNextRecord, status.sourceFrontierStaticMs);
    qgame::spectatorPublishPlaybackWait(
        sourceExhausted ? qgame::SpectatorWait::SourceIdle
        : status.awaitingEventIndex != 0 ? qgame::SpectatorWait::AwaitingEvent
                                         : qgame::SpectatorWait::Filling,
        static_cast<uint32_t>(status.bufferedRecords),
        static_cast<uint32_t>(
            (g_spectatorHasPrimed ? qgame::kSpectatorRebufferRecords
                                  : qgame::kSpectatorStartupBufferRecords)
            + g_spectatorCushionBonus));

    if (status.bufferedRecords == 0) {
        if (status.phase == ViewerPhase::Complete) {
            qgame::spectatorPublishPlaybackPresentation(
                false, false, false);
            if (!g_spectatorPacingSummaryLogged) {
                ArchiveTerminal terminal{};
                (void)qgame::spectatorViewerGetTerminal(terminal);
                QLOG("[spectator] archive complete records=%u reason=%u "
                     "finalHash=%016llX underruns=%llu elasticHolds=%llu",
                     terminal.nextRecord,
                     static_cast<unsigned>(terminal.reason),
                     static_cast<unsigned long long>(
                         terminal.postTailStateHash),
                     static_cast<unsigned long long>(g_spectatorUnderruns),
                     static_cast<unsigned long long>(
                         g_spectatorElasticHolds));
                netLogPresentationSummary();
                g_spectatorPacingSummaryLogged = true;
            }
            return 0;
        }
        if (g_spectatorBufferPrimed) {
            g_spectatorBufferPrimed = false;
            g_spectatorCatchingUp = false;
            g_spectatorCatchupStartedMs = 0;
            g_spectatorElasticCredit = 0;
            g_spectatorLastElasticLogMs = 0;
            g_spectatorCatchupCredit = 0;
            if (qgame::spectatorEmptyBufferIsUnderrun(
                    status.sourceFrontierKnown, status.sourceNextRecord,
                    status.receivedNextRecord)) {
                ++g_spectatorUnderruns;
                g_spectatorCushionBonus =
                    qgame::spectatorCushionBonus(g_spectatorUnderruns);
                spectatorLogFreezeDiagnostics(status, nowMs);
                QLOG("[spectator] playback underrun episode=%llu; rebuffer "
                     "target=%zu (cushion now +%zu records = %zu live "
                     "target)",
                     static_cast<unsigned long long>(g_spectatorUnderruns),
                     qgame::kSpectatorRebufferRecords
                         + g_spectatorCushionBonus,
                     g_spectatorCushionBonus,
                     qgame::spectatorLiveTarget(g_spectatorCushionBonus));
            } else {
                QLOG("[spectator] caught up to a held source at record=%u; "
                     "waiting for the match to advance (not an underrun, "
                     "cushion stays +%zu)",
                     status.receivedNextRecord, g_spectatorCushionBonus);
            }
        }
        qgame::spectatorPublishPlaybackPresentation(
            false, false, g_spectatorLocalPaused);
        ++g_netPacingSessionStalls;
        netPaceSemanticHold();
        Sleep(1);
        return kNetHoldFrameRet;
    }
    if (!g_spectatorBufferPrimed) {
        if (!qgame::spectatorPlaybackPrimed(
                status.bufferedRecords, status.hasTerminal,
                status.phase == ViewerPhase::Complete,
                g_spectatorHasPrimed, g_spectatorCushionBonus,
                sourceExhausted)) {
            qgame::spectatorPublishPlaybackPresentation(
                false, false, g_spectatorLocalPaused);
            ++g_netPacingSessionStalls;
            netPaceSemanticHold();
            Sleep(1);
            return kNetHoldFrameRet;
        }
        g_spectatorBufferPrimed = true;
        const bool recovered = g_spectatorHasPrimed;
        g_spectatorHasPrimed = true;
        g_spectatorElasticCredit = 0;
        g_spectatorLastElasticLogMs = 0;
        qgame::spectatorPublishPlaybackPresentation(
            true, false, g_spectatorLocalPaused);
        QLOG("[spectator] playback buffer primed records=%zu target=%zu "
             "recovery=%d sealed=%d complete=%d sourceIdle=%d",
             status.bufferedRecords,
             (recovered ? qgame::kSpectatorRebufferRecords
                        : qgame::kSpectatorStartupBufferRecords)
                 + g_spectatorCushionBonus,
             recovered ? 1 : 0,
             status.hasTerminal ? 1 : 0,
             status.phase == ViewerPhase::Complete ? 1 : 0,
             sourceExhausted ? 1 : 0);
    }

    if (g_spectatorFastForwardLatched
        && !qgame::spectatorJumpToLiveNeeded(status.bufferedRecords,
                                             g_spectatorCushionBonus)) {
        g_spectatorFastForwardLatched = false;
    }
    const bool fastForward =
        g_spectatorFastForwardHeld || g_spectatorFastForwardLatched;
    if (g_spectatorLocalPaused && g_spectatorPendingSteps == 0
        && !fastForward) {
        qgame::spectatorPublishPlaybackPresentation(true, false, true);
        netPaceSemanticHold();
        Sleep(1);
        return kNetHoldFrameRet;
    }

    const bool inGameplay = rollbackEligible();
    const bool wasCatchingUp = g_spectatorCatchingUp || fastForward;
    const qgame::SpectatorCatchupPlan catchupPlan =
        qgame::spectatorCatchupPlan(status.bufferedRecords, wasCatchingUp,
                                    g_spectatorCushionBonus, inGameplay,
                                    fastForward);
    g_spectatorCatchingUp = catchupPlan.catchingUp;
    g_spectatorCatchupSpeedPermille =
        catchupPlan.catchingUp ? catchupPlan.speedPermille : 1000u;
    qgame::spectatorPublishPlaybackPresentation(
        g_spectatorBufferPrimed, g_spectatorCatchingUp,
        g_spectatorLocalPaused);
    const qgame::SpectatorCatchupSlot catchupSlot =
        qgame::spectatorCatchupSlot(catchupPlan, g_spectatorCatchupCredit);
    g_spectatorCatchupCredit =
        catchupPlan.catchingUp ? catchupSlot.nextCredit : 0u;
    size_t tickBudget = catchupSlot.ticks;
    bool forceSingleStep = false;
    if (g_spectatorPendingSteps != 0) {
        tickBudget = 1u;
        --g_spectatorPendingSteps;
        forceSingleStep = true;
    } else if (fastForward && tickBudget < 2u
               && status.bufferedRecords > 1u) {
        tickBudget = 2u;
    }

    const bool elasticEligible =
        !catchupPlan.catchingUp && !fastForward
        && !forceSingleStep && !status.hasTerminal
        && g_spectatorSpeedPermille == qreplay::kSpeedNormal;
    if (elasticEligible) {
        const qgame::SpectatorElasticPlan elastic =
            qgame::spectatorElasticPlan(status.bufferedRecords,
                                        g_spectatorElasticCredit);
        g_spectatorElasticCredit = elastic.nextCredit;
        if (elastic.speedPermille < 1000u
            && (g_spectatorLastElasticLogMs == 0u
                || nowMs - g_spectatorLastElasticLogMs >= 1000u)) {
            g_spectatorLastElasticLogMs = nowMs;
            QLOG("[spectator] elastic low-water playback backlog=%zu "
                 "speed=%upm holds=%llu underruns=%llu",
                 status.bufferedRecords, elastic.speedPermille,
                 static_cast<unsigned long long>(g_spectatorElasticHolds),
                 static_cast<unsigned long long>(g_spectatorUnderruns));
        }
        if (!elastic.advance) {
            ++g_spectatorElasticHolds;
            if (!netPaceIdleSlot()) {
                netPaceSemanticHold();
                Sleep(1);
            }
            return kNetHoldFrameRet;
        }
    } else {
        g_spectatorElasticCredit = 0u;
        g_spectatorLastElasticLogMs = 0u;
    }
    if (catchupPlan.catchingUp && g_spectatorCatchupStartedMs == 0)
        g_spectatorCatchupStartedMs = nowMs;
    const uint64_t estimatedSlotsToLive = qgame::spectatorEstimatedSlotsToLive(
        status.bufferedRecords, catchupPlan.catchingUp, g_spectatorCushionBonus,
        inGameplay);
    const uint64_t catchupSliceMicros = inGameplay
        ? qgame::kSpectatorCatchupSliceMicros
        : qgame::kSpectatorCatchupMenuSliceMicros;
    const int64_t catchupSliceStartUs = netUnhookedQpcNow();
    const LONG batchGrantsBefore = InterlockedCompareExchange(
        &g_netPresentationSlotGrants, 0, 0);
    bool batchExecutedTick = false;
    bool drawClosingTick = false;
    int visibleResult = kNetHoldFrameRet;
    size_t executedTicks = 0;
    bool sliceTruncated = false;
    for (size_t tick = 0; tick < tickBudget; ++tick) {
        if (tick != 0 && g_qpcFreq > 0) {
            const int64_t elapsedTicks =
                netUnhookedQpcNow() - catchupSliceStartUs;
            const uint64_t elapsedUs = elapsedTicks > 0
                ? (static_cast<uint64_t>(elapsedTicks) * 1000000ull)
                      / static_cast<uint64_t>(g_qpcFreq)
                : 0ull;
            if (elapsedUs >= catchupSliceMicros) {
                sliceTruncated = true;
                ++g_spectatorCatchupSlicesTruncated;
                if (!qgame::spectatorCatchupTickDraws()) break;
                drawClosingTick = true;
            }
        }
        ConfirmedRecord record{};
        if (!qgame::spectatorViewerPeekConfirmed(record)) break;
        if (!g_spectatorReturnPending) {
            const uint32_t appliedEvents =
                qgame::spectatorViewerSetEventCount();
            while (g_spectatorReturnScanCursor < appliedEvents
                   && !g_spectatorReturnPending) {
                qnetplay::spectator::SetEvent setEvent{};
                if (!qgame::spectatorViewerSetEventAt(
                        g_spectatorReturnScanCursor, setEvent)) {
                    break;
                }
                ++g_spectatorReturnScanCursor;
                if (setEvent.kind
                    == qnetplay::spectator::SetEventKind::
                           ReturnedToCharacterSelect) {
                    g_spectatorReturnPending = true;
                    g_spectatorReturnAtRecord = setEvent.recordCursor;
                }
            }
        }
        if (g_spectatorReturnPending
            && static_cast<int32_t>(record.record
                                    - g_spectatorReturnAtRecord) >= 0) {
            g_spectatorReturnPending = false;
            if (record.record != g_spectatorReturnAtRecord) {
                QLOG("[spectator] F1 return event cursor=%u is behind "
                     "playback record=%u; not executed",
                     g_spectatorReturnAtRecord, record.record);
            } else if ((int)rd32(kSceneStateAddr) == 2) {
                QLOG("[spectator] F1 return executed at record %u: "
                     "returning this viewer to character select in "
                     "lockstep with the players", record.record);
                g_netGameplaySegmentActive = false;
                g_spectatorGameplayEntryStartedMs = 0;
                g_spectatorGameplayEntryLastLogMs = 0;
                return 0;
            } else {
                QLOG("[spectator] F1 return event at record %u skipped: "
                     "viewer scene=%u already left the match",
                     record.record, rd32(kSceneStateAddr));
            }
        }
        if (record.timelineFrame != g_spectatorTimelineFrame
            || record.record != status.playbackNextRecord + tick) {
            netFailClosed(kNetProtocolViolation, record.record,
                          "spectator timeline is not contiguous", false);
            return kNetHoldFrameRet;
        }

        const uint32_t frame = record.timelineFrame;
        const uint32_t phaseBefore = netPhaseTag();
        const bool gameplayBefore = rollbackEligible();
        netAutomatchPrepareCanonicalFrame(frame, false);
        netNormalizeHumanMatchAiVm(frame, false);
        uint64_t observedPreHash = 0;
        if (gameplayBefore) {
            if (!ensureRollback()) {
                static uint64_t s_erProbeMs = 0;
                if (nowMs - s_erProbeMs >= 1000u) {
                    s_erProbeMs = nowMs;
                    QLOG("[spectator] tick hold: ensureRollback false at "
                         "frame=%u", record.timelineFrame);
                }
                ++g_netPacingSessionStalls;
                netPaceSemanticHold();
                Sleep(1);
                return kNetHoldFrameRet;
            }
            if (!g_netGameplaySegmentActive) {
                if (g_spectatorGameplayEntryStartedMs == 0) {
                    g_spectatorGameplayEntryStartedMs = nowMs;
                    g_spectatorGameplayEntryLastLogMs = 0;
                    netResetFpuContract();
                    g_netFpuInitializedEpoch = g_netEpoch;
                }
                if (!netCanonicalizeHumanGameStartTransients(frame)) {
                    netFailClosed(
                        kNetInternalFailure, frame,
                        "spectator human game-start transient "
                        "canonicalization failed",
                        false);
                    return kNetHoldFrameRet;
                }
                const char* why = nullptr;
                if (!g_rb.validateLive(&why)) {
                    netFailClosed(
                        kNetInternalFailure, frame,
                        why ? why
                            : "spectator rollback state validation failed",
                        false);
                    return kNetHoldFrameRet;
                }
            }
            if (!spectatorSavePreFrame(
                    frame, phaseBefore, observedPreHash))
                return kNetHoldFrameRet;
        } else {
            g_netGameplaySegmentActive = false;
            g_spectatorGameplayEntryStartedMs = 0;
            g_spectatorGameplayEntryLastLogMs = 0;
            observedPreHash = netFrontendStateFingerprint(frame);
            if (record.record < 4u) {
                netTraceSpectatorFrontend(
                    "viewer-pre", frame, observedPreHash);
            }
        }

        const uint64_t entryElapsedMs =
            gameplayBefore && g_spectatorGameplayEntryStartedMs != 0
                ? nowMs - g_spectatorGameplayEntryStartedMs
                : 0;
        const qgame::SpectatorPreStateDecision preStateDecision =
            qgame::spectatorPreStateDecision(
                g_netGameplaySegmentActive,
                record.stateHash, observedPreHash, entryElapsedMs);
        if (gameplayBefore
            && preStateDecision
                == qgame::SpectatorPreStateDecision::RetryEntry) {
            if (g_spectatorGameplayEntryLastLogMs == 0
                || nowMs - g_spectatorGameplayEntryLastLogMs >= 1000u) {
                g_spectatorGameplayEntryLastLogMs = nowMs;
                QLOG("[spectator] gameplay-entry rendezvous waiting "
                     "frame=%u elapsed=%llums expected=%016llX "
                     "observed=%016llX localLayout=%016llX",
                     frame,
                     static_cast<unsigned long long>(entryElapsedMs),
                     static_cast<unsigned long long>(record.stateHash),
                     static_cast<unsigned long long>(observedPreHash),
                     static_cast<unsigned long long>(
                         netCurrentLayoutDigest()));
            }
            ++g_netPacingSessionStalls;
            netPaceSemanticHold();
            Sleep(1);
            return kNetHoldFrameRet;
        }
        if ((!gameplayBefore && record.stateHash != observedPreHash)
            || (gameplayBefore
                && preStateDecision
                    == qgame::SpectatorPreStateDecision::Reject)) {
            (void)qgame::spectatorViewerCommitConfirmed(
                record.record, observedPreHash, 0);
            netFailClosed(kNetConfirmedDesync, frame,
                          "spectator canonical pre-state differs from archive",
                          false);
            return kNetHoldFrameRet;
        }
        if (gameplayBefore && !g_netGameplaySegmentActive) {
            QLOG("[spectator] gameplay-entry rendezvous complete "
                 "frame=%u waited=%llums hash=%016llX",
                 frame,
                 static_cast<unsigned long long>(entryElapsedMs),
                 static_cast<unsigned long long>(observedPreHash));
            g_netGameplaySegmentActive = true;
            g_netGameplaySegmentSeen = true;
            g_spectatorGameplayEntryStartedMs = 0;
            g_spectatorGameplayEntryLastLogMs = 0;
        }

        netReplayMaybeStageFreshGameV2(frame);

        qnetplay::CanonicalInputs inputs{};
        inputs.frame = frame;
        inputs.p1 = record.playerOneInput;
        inputs.p2 = record.playerTwoInput;
        inputs.remotePredicted = false;
        NetFrameMeta meta =
            netMetaBeforeTick(frame, phaseBefore, observedPreHash);
        meta.canonicalActions[0] = record.playerOneInput;
        meta.canonicalActions[1] = record.playerTwoInput;
        int tickResult = kNetHoldFrameRet;
        const bool hiddenCatchup =
            qgame::spectatorCatchupExecutedTickVisibility(
                tick, tickBudget, drawClosingTick)
                == qgame::NativeTickVisibility::Historical;
        const uint32_t nativeCounterBefore = rd32(0x4AB778u);
        const uint32_t arenaSelectorBefore = rd32(kBlockSelector);
        const PresentationDisposition catchupDisposition =
            !hiddenCatchup ? PresentationDisposition::Normal
            : drawClosingTick
                ? PresentationDisposition::VisibleFastForward
                : PresentationDisposition::SuppressHistorical;
        if (!netRunOwnedTick(orig, frame, inputs, hiddenCatchup,
                tickResult, false, catchupDisposition))
            return kNetHoldFrameRet;
        if (!hiddenCatchup)
            netObservePacing(
                frame, gameplayBefore, nativeCounterBefore,
                rd32(0x4AB778u), arenaSelectorBefore, rd32(kBlockSelector));
        batchExecutedTick = true;
        netPrepareNextCanonicalBoundary(frame, false);
        meta.phaseAfter = netPhaseTag();
        meta.replayLifecycleAfter =
            netReplayLifecycleV2(
                meta.replayLifecycleBefore.layoutGeneration);
        meta.replayRoundTransition =
            netReplayRoundTransitionV2(
                meta.replayLifecycleBefore,
                meta.replayLifecycleAfter);
        meta.replayRoster = netReplayRosterV2();
        meta.replayStage = netReplayStageV2();
        meta.replayDispatcherResult = tickResult;
        meta.spectatorPostHash = netSpectatorLiveStateHash(
            static_cast<uint64_t>(frame) + 1u);
        if (!rollbackEligible() && record.record < 4u) {
            netTraceSpectatorFrontend(
                "viewer-post", frame, meta.spectatorPostHash);
        }
        if (meta.spectatorPostHash == 0) {
            netFailClosed(kNetInternalFailure, frame,
                          "spectator post-state hash failed", false);
            return kNetHoldFrameRet;
        }
        const IngressResult committed =
            qgame::spectatorViewerCommitConfirmed(
                record.record, observedPreHash,
                meta.spectatorPostHash);
        if (committed != IngressResult::Accepted) {
            const bool stateMismatch =
                committed == IngressResult::StateHashMismatch;
            netFailClosed(stateMismatch
                              ? kNetConfirmedDesync
                              : kNetProtocolViolation,
                          static_cast<uint32_t>(committed),
                          stateMismatch
                              ? "spectator applied state differs from archive"
                              : "spectator playback commit was rejected",
                          false);
            return kNetHoldFrameRet;
        }
        if (qgame::replay::replayRecorderV2Active()) {
            qgame::replay::ReplayNativeFrameV2 replayFrame{};
            replayFrame.sourceFrame = frame;
            replayFrame.core.inputDomain =
                qreplay::InputDomain::LogicalSourceV1;
            replayFrame.core.activeLaneMask = 0x03u;
            replayFrame.core.actions[0] = meta.canonicalActions[0];
            replayFrame.core.actions[1] = meta.canonicalActions[1];
            replayFrame.core.before = meta.replayLifecycleBefore;
            replayFrame.core.after = meta.replayLifecycleAfter;
            replayFrame.core.dispatcherResult = meta.replayDispatcherResult;
            replayFrame.core.preStateHash = meta.spectatorPreHash;
            replayFrame.core.postStateHash = meta.spectatorPostHash;
            replayFrame.roundTransition = meta.replayRoundTransition;
            replayFrame.activeFighterMask = meta.replayActiveFighterMask;
            replayFrame.roster = meta.replayRoster;
            replayFrame.stage = meta.replayStage;
            (void)qgame::replay::replayRecordConfirmedV2(replayFrame);
        }
        g_netMeta[frame % kNetMetaSlots] = meta;
        g_spectatorTimelineFrame = frame + 1u;
        g_netFrame = g_spectatorTimelineFrame;
        ++executedTicks;
        visibleResult = tickResult;
        if (g_audioV2Enabled && !hiddenCatchup)
            qaudio::runtime().flushPending(frame);

        const bool gameplayAfter = rollbackEligible();
        if (!gameplayAfter) {
            if (gameplayBefore && g_netGameplaySegmentSeen)
                g_netGameplayRefreshPending = true;
            g_netGameplaySegmentActive = false;
            g_spectatorGameplayEntryStartedMs = 0;
            g_spectatorGameplayEntryLastLogMs = 0;
        }
        if (qgame::spectatorCatchupMustYield(
                tickResult != kNetHoldFrameRet,
                gameplayBefore != gameplayAfter,
                meta.phaseAfter != phaseBefore,
                rd32(0x6234EC), rd32(0x61801C)))
            break;
        if (drawClosingTick) break;
    }
    if (batchExecutedTick
        && InterlockedCompareExchange(
               &g_netPresentationSlotGrants, 0, 0)
            == batchGrantsBefore) {
        (void)netConsumePresentationSlot(
            InterlockedCompareExchange(
                &g_netOwnedTickSerial, 0, 0));
        if (InterlockedCompareExchange(
                &g_netPresentationSlotGrants, 0, 0)
            > batchGrantsBefore) {
            InterlockedIncrement(
                &g_netSpectatorBatchFallbackGrants);
        }
    }
    const size_t receiveBacklog =
        status.receivedNextRecord >= status.playbackNextRecord
            ? static_cast<size_t>(status.receivedNextRecord
                                  - status.playbackNextRecord)
            : 0u;
    if (catchupPlan.catchingUp) {
        if (g_spectatorLastCatchupLogMs == 0
            || nowMs - g_spectatorLastCatchupLogMs >= 1000u) {
            g_spectatorLastCatchupLogMs = nowMs;
            QLOG("[spectator] catch-up receiveBacklog=%zu playbackBacklog=%zu "
                 "tickBudget=%zu executed=%zu sliceTruncated=%d "
                 "etaSlotsToLive=%llu liveTarget=%zu underruns=%llu "
                 "truncatedSlices=%llu fastForward=%d speed=%upm "
                 "plan=%upm phase=%s slice=%lluus",
                 receiveBacklog, status.bufferedRecords, tickBudget,
                 executedTicks, sliceTruncated ? 1 : 0,
                 static_cast<unsigned long long>(estimatedSlotsToLive),
                 qgame::kSpectatorLiveTargetRecords,
                 static_cast<unsigned long long>(g_spectatorUnderruns),
                 static_cast<unsigned long long>(
                     g_spectatorCatchupSlicesTruncated),
                 fastForward ? 1 : 0, g_spectatorSpeedPermille,
                 catchupPlan.speedPermille, inGameplay ? "gameplay" : "menu",
                 static_cast<unsigned long long>(catchupSliceMicros));
        }
    } else if (g_spectatorCatchupStartedMs != 0) {
        QLOG("[spectator] converged to live target after %llums "
             "(playbackBacklog=%zu liveTarget=%zu underruns=%llu "
             "truncatedSlices=%llu)",
             static_cast<unsigned long long>(
                 nowMs - g_spectatorCatchupStartedMs),
             status.bufferedRecords, qgame::kSpectatorLiveTargetRecords,
             static_cast<unsigned long long>(g_spectatorUnderruns),
             static_cast<unsigned long long>(
                 g_spectatorCatchupSlicesTruncated));
        g_spectatorCatchupStartedMs = 0;
        g_spectatorLastCatchupLogMs = 0;
        g_spectatorFastForwardLatched = false;
    }
    return visibleResult;
}

static bool g_replayHalted = false;
static uint64_t g_replayTicks = 0;
static uint64_t g_replayHiddenTicks = 0;
static uint64_t g_replayBufferingSlots = 0;
enum class ReplayBootstrapPhase : uint8_t {
    WaitingForStart = 0,
    DirectBoot,
    NativeSetup,
    Complete,
};
static ReplayBootstrapPhase g_replayBootstrapPhase =
    ReplayBootstrapPhase::WaitingForStart;
static qreplay::ConfirmedGameStartV2 g_replayGameStartV2{};
static uint64_t g_replayObservedBootstrapFingerprint = 0u;
static uint32_t g_replayObservedInitialPhase = 0u;

static qgame::NativeHudOfflineOwner nativeHudOfflineOwner() {
    qgame::NativeHudOwnerFacts facts{};
    facts.trainingSessionRequested = g_trainingLaunchRequested;
    facts.trainingLifecycle = g_trainingLifecycle;
    facts.replayRoleActive = g_replayRoleActive;
    facts.replayPlaybackActive =
        g_replayBootstrapPhase == ReplayBootstrapPhase::Complete
        && !g_replayHalted;
    facts.replayTakeoverActive = g_replayTakeoverInputRuntimeReady;
    facts.netplayActive = g_netActive;
    facts.spectatorRole = g_inputSpectatorRole;
    return qgame::nativeHudResolveOwner(facts);
}

static qgame::NativeHudEffectiveMasks nativeHudEffectiveMasksFor(
        qgame::NativeHudOfflineOwner owner) {
    return qgame::nativeHudEffectiveMasks(
        owner,
        owner == qgame::NativeHudOfflineOwner::Replay
            ? g_nativeHudPrefs.replay : g_nativeHudPrefs.training);
}

static uint32_t g_replayBootstrapSlots = 0u;
static std::array<uint32_t, 2> g_replayBootstrapSelectorConfirms{};
static uint32_t g_replayBootstrapSelectorLaunch = 0u;
static std::array<uint32_t, 2> g_replayBootstrapKnockoutLatches{};
static bool g_replayBootstrapSelectorLifecycleValid = false;
static bool g_replayClosePosted = false;
static LONG g_replayTerminalSerial = 0;
static uint32_t g_replayWinScreenTicks = 0u;

struct ReplayTakeoverNativeSnapshot {
    std::vector<uint8_t> regions;
    alignas(16) uint8_t fpu[512]{};
    VirtualAudioState virtualAudio{};
    uint32_t layoutGeneration = 0u;
    uint32_t netFrame = 0u;
    uint64_t overrideFrame = 0u;
    LONG terminalSerial = 0;
    LONG presentedSerial = 0;
    uint32_t fpuInitializedEpoch = 0u;
    bool gameplaySegmentActive = false;
    bool gameplaySegmentSeen = false;
    bool gameplayRefreshPending = false;
    bool valid = false;
};
static ReplayTakeoverNativeSnapshot g_replayTakeoverSnapshot;
static ReplayTakeoverNativeSnapshot g_replayStartSnapshot;
static qreplay::ReplayBootstrapObservationV2 g_replayStartObservation;

struct ReplaySeekSnapshot {
    ReplayTakeoverNativeSnapshot native;
    qaudio::QohAudioRuntime::ReplayCheckpoint audio;
    qreplay::ReplayPlaybackSessionV2::Checkpoint playback;
    uint64_t preStateHash = 0u;
    qreplay::GameLifecycleV2 lifecycle{};
};
static qreplay::ReplaySeekCache<ReplaySeekSnapshot> g_replaySeekCache;

static bool g_replayObserveBootstrapDiffered = false;
static uint64_t g_replayObserveFrames = 0u;
static uint64_t g_replayObservePreMismatch = 0u;
static uint64_t g_replayObservePostMismatch = 0u;
static uint32_t g_replayObserveFirstPostMatch = UINT32_MAX;
static uint32_t g_replayObserveLastPostMismatch = UINT32_MAX;

static bool replayProofObserveMode() {
    static const bool enabled = []() -> bool {
        char buf[32] = {};
        const DWORD n = GetEnvironmentVariableA(
            "QOH99_REPLAY_PROOF_OBSERVE", buf, sizeof(buf));
        const bool on = n != 0u && n < sizeof(buf)
            && std::strcmp(buf, "diagnose") == 0;
        if (on) {
            QLOG("[replay.observe] *** STATE PROOFS ARE IN OBSERVE MODE ***  "
                 "the recorded pre/post state hashes are MEASURED AND LOGGED "
                 "but NOT enforced; a 'verified complete' result from this "
                 "process does NOT include the byte-exact state proof. "
                 "Diagnostic builds only.");
        }
        return on;
    }();
    return enabled;
}

static void replayObserveFrame(uint32_t frame, uint64_t recordedPre,
                               uint64_t observedPre, uint64_t recordedPost,
                               uint64_t observedPost) {
    ++g_replayObserveFrames;
    const bool preBad = recordedPre != 0u && recordedPre != observedPre;
    const bool postBad = recordedPost != 0u && recordedPost != observedPost;
    if (preBad) ++g_replayObservePreMismatch;
    if (postBad) {
        ++g_replayObservePostMismatch;
        g_replayObserveLastPostMismatch = frame;
    } else if (g_replayObserveFirstPostMatch == UINT32_MAX) {
        g_replayObserveFirstPostMatch = frame;
    }
    const bool interesting =
        frame < 8u || (frame % 300u) == 0u
        || (!postBad && g_replayObserveFirstPostMatch == frame);
    if (interesting) {
        QLOG("[replay.observe] frame=%u pre=%s(%016llX/%016llX) "
             "post=%s(%016llX/%016llX) preMismatch=%llu postMismatch=%llu",
             frame, preBad ? "DIFF" : "same",
             static_cast<unsigned long long>(recordedPre),
             static_cast<unsigned long long>(observedPre),
             postBad ? "DIFF" : "same",
             static_cast<unsigned long long>(recordedPost),
             static_cast<unsigned long long>(observedPost),
             static_cast<unsigned long long>(g_replayObservePreMismatch),
             static_cast<unsigned long long>(g_replayObservePostMismatch));
    }
}

static uint64_t g_replayObserveLastPre = 0u;
static uint64_t g_replayObserveLifecycleDiff = 0u;
static uint64_t g_replayObserveDispatcherDiff = 0u;

static void replayObserveFrameV3(uint32_t frame, uint64_t recordedPre,
                                 uint64_t observedPre, uint64_t recordedPost,
                                 uint64_t observedPost, bool lifecycleBefore,
                                 bool lifecycleAfter, bool dispatcher) {
    ++g_replayObserveFrames;
    const bool preBad = recordedPre != observedPre;
    const bool postBad = recordedPost != observedPost;
    if (preBad) ++g_replayObservePreMismatch;
    if (postBad) {
        ++g_replayObservePostMismatch;
        g_replayObserveLastPostMismatch = frame;
    } else if (g_replayObserveFirstPostMatch == UINT32_MAX) {
        g_replayObserveFirstPostMatch = frame;
    }
    if (lifecycleBefore || lifecycleAfter) ++g_replayObserveLifecycleDiff;
    if (dispatcher) ++g_replayObserveDispatcherDiff;
    const bool interesting =
        frame < 4u || (frame % 600u) == 0u
        || (!postBad && g_replayObserveFirstPostMatch == frame)
        || lifecycleBefore || lifecycleAfter || dispatcher;
    if (interesting && g_replayObserveFrames < 40000u) {
        QLOG("[replay.observe] frame=%u pre=%s post=%s life=%d/%d disp=%d "
             "recPost=%016llX obsPost=%016llX preDiff=%llu postDiff=%llu "
             "lifeDiff=%llu dispDiff=%llu",
             frame, preBad ? "DIFF" : "same", postBad ? "DIFF" : "same",
             static_cast<int>(lifecycleBefore),
             static_cast<int>(lifecycleAfter), static_cast<int>(dispatcher),
             static_cast<unsigned long long>(recordedPost),
             static_cast<unsigned long long>(observedPost),
             static_cast<unsigned long long>(g_replayObservePreMismatch),
             static_cast<unsigned long long>(g_replayObservePostMismatch),
             static_cast<unsigned long long>(g_replayObserveLifecycleDiff),
             static_cast<unsigned long long>(g_replayObserveDispatcherDiff));
    }
}

static void netLogReplayObserveSummary(const char* where) {
    if (!replayProofObserveMode()) return;
    QLOG("[replay.observe] SUMMARY %s bootstrapDiffered=%d frames=%llu "
         "preMismatch=%llu postMismatch=%llu firstPostMatch=%d "
         "lastPostMismatch=%d lifecycleDiff=%llu dispatcherDiff=%llu",
         where ? where : "(end)",
         static_cast<int>(g_replayObserveBootstrapDiffered),
         static_cast<unsigned long long>(g_replayObserveFrames),
         static_cast<unsigned long long>(g_replayObservePreMismatch),
         static_cast<unsigned long long>(g_replayObservePostMismatch),
         g_replayObserveFirstPostMatch == UINT32_MAX
             ? -1
             : static_cast<int>(g_replayObserveFirstPostMatch),
         g_replayObserveLastPostMismatch == UINT32_MAX
             ? -1
             : static_cast<int>(g_replayObserveLastPostMismatch),
         static_cast<unsigned long long>(g_replayObserveLifecycleDiff),
         static_cast<unsigned long long>(g_replayObserveDispatcherDiff));
}

static void replayHalt(uint32_t frame, const char* why,
                       uint64_t expected, uint64_t actual) {
    if (g_replayHalted) return;
    g_replayHalted = true;
    QLOG("[replay] HALTED frame=%u reason=%s expected=%016llX actual=%016llX "
         "phase=%08X matchSub=%d scene=%u requestedScene=%u "
         "manifestDigest=%016llX matchOrdinal=%u speed=%upm "
         "ticks=%llu hidden=%llu",
         frame, why ? why : "?",
         static_cast<unsigned long long>(expected),
         static_cast<unsigned long long>(actual),
         netPhaseTag(), static_cast<int>(rd32(kMatchSubAddr)),
         rd32(kSceneStateAddr), rd32(0x61801C),
         static_cast<unsigned long long>(g_replayManifestDigest),
         g_replayMatchOrdinal, g_replaySpeedPermille,
         static_cast<unsigned long long>(g_replayTicks),
         static_cast<unsigned long long>(g_replayHiddenTicks));
    netLogAudioLedger();
    netLogPresentationSummary();
    netLogReplayObserveSummary(why);
}

static bool replayRunOneTick(FrameDispatchFn orig, uint32_t frame, bool hidden,
                             LONG ownedTickSerial, int& result,
                             bool takeover = false) {
    if (qgame::directBootNetworkActive()) qgame::directBootNetworkTick();
    if (qgame::directBootNetworkFailed()) {
        if (takeover) {
            (void)qgame::replay::replayTakeoverFailBranch();
            QLOG("[replay.takeover] branch DirectBoot failed frame=%u", frame);
        } else {
            replayHalt(frame, "deterministic DirectBoot failed", 0, 0);
        }
        return false;
    }
    g_ovrFrame = frame;
    const bool audioTick = g_audioV2Enabled
        && qgame::replay::replayPlaybackV3()
        && rollbackEligible()
        && !netLifecycleTransitionRequiresExactInput();
    bool audioTickOpen = false;
    if (audioTick) {
        qaudio::runtime().setDisposition(
            takeover
                ? qaudio::AudioTickDisposition::ForwardVisible
                : hidden
                    ? qaudio::AudioTickDisposition::SpectatorConfirmedCatchup
                    : qaudio::AudioTickDisposition::SpectatorConfirmedVisible);
        audioTickOpen = qaudio::runtime().beginTick(
            g_netEpoch, g_netEpoch, frame);
        if (!audioTickOpen) {
            qaudio::runtime().setDisposition(
                qaudio::AudioTickDisposition::ImmediateOffline);
            QLOG("[replay.v3] audio intent journal could not begin frame=%u",
                 frame);
            if (takeover)
                (void)qgame::replay::replayTakeoverFailBranch();
            else
                qgame::replay::replayFailPlaybackV2();
            return false;
        }
    } else if (g_audioV2Enabled) {
        qaudio::runtime().setDisposition(
            qaudio::AudioTickDisposition::ImmediateOffline);
    }
    ScopedPresentationDisposition presentationScope(
        hidden ? PresentationDisposition::SuppressHistorical
               : PresentationDisposition::Normal);
    if (hidden) ++g_hiddenReplayNativeTicks;
    netClockBegin(frame);
    InterlockedExchange(&g_freezeGateCalls, 0);
    InterlockedExchange(&g_netOwnedTickSerial, ownedTickSerial);
    InterlockedExchange(&g_netGateOwnsTick, 1);
    g_netCanonicalPoll = true;
    result = kNetHoldFrameRet;
    const DWORD exceptionCode = netInvokeOwnedTickWithSeh(orig, result);
    if (exceptionCode == 0 && !hidden)
        (void)netConsumePresentationSlot(ownedTickSerial);
    InterlockedExchange(&g_netGateOwnsTick, 0);
    netClockOff();
    g_netCanonicalPoll = false;
    bool audioCommitFailed = false;
    if (audioTickOpen) {
        if (exceptionCode != 0u)
            qaudio::runtime().abortTick();
        else
            audioCommitFailed = !qaudio::runtime().commitTick();
    }
    if (g_audioV2Enabled) {
        qaudio::runtime().setDisposition(
            qaudio::AudioTickDisposition::ImmediateOffline);
    }
    if (audioCommitFailed) {
        QLOG("[replay.v3] audio intent journal commit failed frame=%u",
             frame);
        if (takeover)
            (void)qgame::replay::replayTakeoverFailBranch();
        else
            qgame::replay::replayFailPlaybackV2();
        return false;
    }
    if (exceptionCode != 0) {
        char why[96];
        std::snprintf(why, sizeof(why),
                      "game tick raised SEH exception 0x%08lX",
                      static_cast<unsigned long>(exceptionCode));
        if (qgame::replay::replayPlaybackV3()) {
            QLOG("[replay.v3] %s frame=%u", why, frame);
            if (takeover)
                (void)qgame::replay::replayTakeoverFailBranch();
            else
                qgame::replay::replayFailPlaybackV2();
        } else {
            replayHalt(frame, why, 0, 0);
        }
        return false;
    }
    if (!takeover) ++g_replayTicks;
    if (hidden) ++g_replayHiddenTicks;
    return true;
}

static void replayTakeoverStopTransientSfx() {
    if (!g_soundStopHooked || g_origSoundStop == nullptr) return;
    for (int endpoint = 0;
         endpoint <= static_cast<int>(qaudio::kMaxDuplicateSlot); ++endpoint) {
        (void)callNativeSoundStopPreservingFpu(endpoint, 1);
    }
    for (int endpoint = 2;
         endpoint <= static_cast<int>(qaudio::kMaxSampleId); ++endpoint) {
        (void)callNativeSoundStopPreservingFpu(endpoint, 2);
    }
}

static bool replayTakeoverCaptureSnapshot(
    ReplayTakeoverNativeSnapshot& snapshot = g_replayTakeoverSnapshot,
    bool replayStart = false,
    qaudio::QohAudioRuntime::ReplayCheckpoint* checkpointAudio = nullptr) {
    snapshot.valid = false;
    if (qgame::directBootNetworkActive() || !rollbackEligible()
        || !ensureRollback()
        || g_rb.totalSize() == 0u) {
        QLOG("[replay.takeover] snapshot refused: gameplay layout unavailable");
        return false;
    }
    const char* why = nullptr;
    if (!g_rb.validateLive(
            &why,
            RollbackManager::LiveValidationPolicy::RawStructuralOnly)) {
        QLOG("[replay.takeover] snapshot refused: %s",
             why ? why : "layout validation failed");
        return false;
    }
    try {
        snapshot.regions.resize(g_rb.totalSize());
    } catch (const std::bad_alloc&) {
        QLOG("[replay.takeover] snapshot refused: allocation failed bytes=%zu",
             g_rb.totalSize());
        return false;
    }
    fxSaveTo(snapshot.fpu);
    if (!g_rb.snapshotTo(snapshot.regions.data())) {
        QLOG("[replay.takeover] snapshot refused: region capture failed");
        return false;
    }
    if (g_audioV2Enabled && !(checkpointAudio
            ? qaudio::runtime().captureReplayCheckpoint(*checkpointAudio)
            : replayStart ? qaudio::runtime().captureReplayStart()
                          : qaudio::runtime().beginReplayTakeover())) {
        QLOG("[replay.takeover] snapshot refused: audio transaction busy");
        return false;
    }
    snapshot.virtualAudio = g_virtualAudio;
    snapshot.layoutGeneration = g_rb.layoutGeneration();
    snapshot.netFrame = g_netFrame;
    snapshot.overrideFrame = g_ovrFrame;
    snapshot.terminalSerial = g_replayTerminalSerial;
    snapshot.presentedSerial = InterlockedCompareExchange(
        &g_replayPresentedTickSerial, 0, 0);
    snapshot.fpuInitializedEpoch = g_netFpuInitializedEpoch;
    snapshot.gameplaySegmentActive = g_netGameplaySegmentActive;
    snapshot.gameplaySegmentSeen = g_netGameplaySegmentSeen;
    snapshot.gameplayRefreshPending = g_netGameplayRefreshPending;
    snapshot.valid = true;
    if (!checkpointAudio) QLOG("[replay.takeover] native branch snapshot captured frame=%u "
         "layout=%u bytes=%zu", snapshot.netFrame,
         snapshot.layoutGeneration, snapshot.regions.size());
    return true;
}

static bool replayTakeoverRestoreSnapshot(
    ReplayTakeoverNativeSnapshot& snapshot = g_replayTakeoverSnapshot,
    bool replayStart = false,
    const qaudio::QohAudioRuntime::ReplayCheckpoint* checkpointAudio = nullptr) {
    if (!snapshot.valid || !g_rbReady
        || snapshot.layoutGeneration != g_rb.layoutGeneration()
        || snapshot.regions.size() != g_rb.totalSize()) {
        QLOG("[replay.takeover] restore refused: stale native snapshot");
        return false;
    }
    const char* why = nullptr;
    if (!g_rb.validateLive(
            &why,
            RollbackManager::LiveValidationPolicy::RawStructuralOnly)) {
        QLOG("[replay.takeover] restore refused before write: %s",
             why ? why : "layout validation failed");
        return false;
    }
    if (!g_rb.loadFrom(snapshot.regions.data())) {
        QLOG("[replay.takeover] restore failed: atomic region load refused");
        return false;
    }
    why = nullptr;
    if (!g_rb.validateLive(
            &why,
            RollbackManager::LiveValidationPolicy::RawStructuralOnly)) {
        QLOG("[replay.takeover] restore failed after write: %s",
             why ? why : "layout validation failed");
        return false;
    }

    replayTakeoverStopTransientSfx();
    if (g_audioV2Enabled
        && !(checkpointAudio ? qaudio::runtime().restoreReplayCheckpoint(*checkpointAudio)
            : replayStart ? qaudio::runtime().restoreReplayStart()
                          : qaudio::runtime().restoreAfterReplayTakeover())) {
        QLOG("[replay.takeover] restore failed: audio baseline unavailable");
        return false;
    }
    g_virtualAudio = snapshot.virtualAudio;
    g_netFrame = snapshot.netFrame;
    g_ovrFrame = snapshot.overrideFrame;
    g_replayTerminalSerial = snapshot.terminalSerial;
    InterlockedExchange(
        &g_replayPresentedTickSerial, snapshot.presentedSerial);
    g_netFpuInitializedEpoch = snapshot.fpuInitializedEpoch;
    g_netGameplaySegmentActive = snapshot.gameplaySegmentActive;
    g_netGameplaySegmentSeen = snapshot.gameplaySegmentSeen;
    g_netGameplayRefreshPending = snapshot.gameplayRefreshPending;
    InterlockedExchange(
        &g_presentationDisposition,
        static_cast<LONG>(PresentationDisposition::Normal));
    netClockOff();
    InterlockedExchange(&g_netGateOwnsTick, 0);
    fxRestoreFrom(snapshot.fpu);
    QLOG("[replay.takeover] native branch restored frame=%u layout=%u",
         snapshot.netFrame, snapshot.layoutGeneration);
    return true;
}

static int replayRunBootstrapTick(FrameDispatchFn orig) {
    ScopedPresentationDisposition presentationScope(
        PresentationDisposition::SuppressHistorical);
    g_ovrFrame = 0u;
    netClockBegin(0u);
    InterlockedExchange(&g_freezeGateCalls, 0);
    const LONG serial = InterlockedIncrement(&g_netOwnedTickSerial);
    InterlockedExchange(&g_netGateOwnsTick, 1);
    int result = kNetHoldFrameRet;
    const DWORD exceptionCode = netInvokeOwnedTickWithSeh(orig, result);
    InterlockedExchange(&g_netGateOwnsTick, 0);
    netClockOff();
    if (exceptionCode != 0u) {
        QLOG("[replay.v3] bootstrap native tick raised SEH 0x%08lX "
             "serial=%ld",
             static_cast<unsigned long>(exceptionCode), serial);
        qgame::replay::replayFailPlaybackV2();
        return kNetHoldFrameRet;
    }
    return result;
}

static void replayFailBootstrapV3(const char* why, uint64_t actual = 0u) {
    QLOG_ISSUE("[replay.v3] bootstrap FAILED: %s actual=%016llX scene=%u "
         "matchSub=%d phase=%08X",
         why ? why : "unknown",
         static_cast<unsigned long long>(actual),
         rd32(kSceneStateAddr),
         static_cast<int32_t>(rd32(kMatchSubAddr)), netPhaseTag());
    qreplay::ReplayBootstrapObservationV2 failed{};
    (void)qgame::replay::replayCompleteBootstrapV2(failed);
}

static bool replayArmRecordedDirectBootV3() {
    if (!qgame::replay::replayGameStartV2(g_replayGameStartV2))
        return false;
    const qreplay::GameManifestV2& manifest =
        g_replayGameStartV2.manifest;
    uint32_t logicalStage = qreplay::kNoLogicalStageIndexV2;
    if (manifest.bootstrapKind
            != qreplay::BootstrapKindV2::MatchActiveDirectBootV1
        || manifest.vsRow != 1u
        || !qreplay::StageCatalogV2::forSeBuild()
                .logicalStageIndexForNativeStageId(
                    manifest.stage.nativeStageId, logicalStage)) {
        replayFailBootstrapV3("unsupported recorded bootstrap topology");
        return false;
    }
    qgame::ReplayDirectBootConfig boot{};
    boot.vsRow = manifest.vsRow;
    boot.logicalStageIndex = logicalStage;
    for (size_t slot = 0u; slot < 2u; ++slot) {
        const qreplay::FighterSlotDescriptorV2& fighter =
            manifest.slots[slot];
        if (!fighter.selectorProofPresent) {
            replayFailBootstrapV3("recorded fighter lacks selector proof",
                                  slot);
            return false;
        }
        boot.slots[slot].cursorCell =
            fighter.selectorProof.cursorCell;
        boot.slots[slot].alternateFlag =
            fighter.selectorProof.alternateFlag;
        boot.slots[slot].paletteIndex =
            fighter.identity.paletteIndex;
        boot.slots[slot].confirmState =
            fighter.selectorProof.confirmState;
    }
    if (!qgame::startReplayDirectBootToMatch(boot)) {
        replayFailBootstrapV3("recorded DirectBoot configuration refused");
        return false;
    }
    g_replayBootstrapPhase = ReplayBootstrapPhase::DirectBoot;
    g_replayBootstrapSlots = 0u;
    return true;
}

static bool replayApplyRecordedStartV3() {
    const qreplay::DeterministicGameStartV2& start =
        g_replayGameStartV2.start;
    qgame::NetReadyState ready{};
    ready.seedGameplay = start.rng.gameplay;
    ready.seedCosmetic = start.rng.cosmetic;
    ready.selector = start.rng.selector;
    ready.timerMode = start.configuration.timerMode;
    ready.aiDifficulty = start.configuration.aiDifficulty;
    ready.lifeMultiplier = start.configuration.lifeMultiplier;
    ready.roundsToWinMinusOne =
        start.configuration.roundsToWinMinusOne;
    if (!netApplyAuthoritativeMatchConfig(ready)) return false;
    wr32(0x4A9938u, ready.seedGameplay);
    wr32(0x4A993Cu, ready.seedCosmetic);
    wr32(0x4ABB24u, ready.selector);
    uint16_t x87 = start.fpu.x87ControlWord;
    __asm { fldcw x87 }
    _mm_setcsr(start.fpu.mxcsr);
    g_netEpoch = g_replayGameStartV2.manifest.layoutGeneration;
    g_replayObservedInitialPhase = netPhaseTag();
    g_replayObservedBootstrapFingerprint = netBootstrapFingerprint();
    if (g_replayObservedBootstrapFingerprint
            != start.bootstrapFingerprint) {
        const uint32_t original0 = rd32(kReplaySelectorConfirmAddr);
        const uint32_t original1 = rd32(
            kReplaySelectorConfirmAddr + sizeof(uint32_t));
        const uint32_t originalLaunch =
            rd32(kReplaySelectorLaunchStateAddr);
        const uint32_t originalKnockout0 =
            rd32(kReplaySessionKnockoutLatchAddr);
        const uint32_t originalKnockout1 =
            rd32(kReplaySessionKnockoutLatchAddr + sizeof(uint32_t));
        bool resolved = false;
        for (uint32_t knockout = 0u; knockout < 4u && !resolved; ++knockout) {
            wr32(kReplaySessionKnockoutLatchAddr,
                 (knockout & 1u) != 0u ? 1u : 0u);
            wr32(kReplaySessionKnockoutLatchAddr + sizeof(uint32_t),
                 (knockout & 2u) != 0u ? 1u : 0u);
            for (uint32_t launch = 0u; launch <= 1u && !resolved; ++launch) {
                wr32(kReplaySelectorLaunchStateAddr, launch);
                for (uint32_t first = 1u; first <= 4u && !resolved; ++first) {
                    for (uint32_t second = 1u; second <= 4u; ++second) {
                        wr32(kReplaySelectorConfirmAddr, first);
                        wr32(
                            kReplaySelectorConfirmAddr + sizeof(uint32_t),
                            second);
                        const uint64_t candidate = netBootstrapFingerprint();
                        if (candidate == start.bootstrapFingerprint) {
                            g_replayObservedBootstrapFingerprint = candidate;
                            resolved = true;
                            QLOG("[replay.v3] bootstrap selector lifecycle "
                                 "normalized confirms=%u/%u launch=%u "
                                 "knockout=%u/%u from=%u/%u/%u/%u/%u",
                                 first, second, launch,
                                 (knockout & 1u) != 0u ? 1u : 0u,
                                 (knockout & 2u) != 0u ? 1u : 0u,
                                 original0, original1, originalLaunch,
                                 originalKnockout0, originalKnockout1);
                            break;
                        }
                    }
                }
            }
        }
        if (!resolved) {
            wr32(kReplaySelectorConfirmAddr, original0);
            wr32(kReplaySelectorConfirmAddr + sizeof(uint32_t), original1);
            wr32(kReplaySelectorLaunchStateAddr, originalLaunch);
            wr32(kReplaySessionKnockoutLatchAddr, originalKnockout0);
            wr32(kReplaySessionKnockoutLatchAddr + sizeof(uint32_t),
                 originalKnockout1);
            g_replayObservedBootstrapFingerprint =
                netBootstrapFingerprint();
        }
    }
    g_replayBootstrapSelectorConfirms[0] =
        rd32(kReplaySelectorConfirmAddr);
    g_replayBootstrapSelectorConfirms[1] =
        rd32(kReplaySelectorConfirmAddr + sizeof(uint32_t));
    g_replayBootstrapSelectorLaunch =
        rd32(kReplaySelectorLaunchStateAddr);
    g_replayBootstrapKnockoutLatches[0] =
        rd32(kReplaySessionKnockoutLatchAddr);
    g_replayBootstrapKnockoutLatches[1] =
        rd32(kReplaySessionKnockoutLatchAddr + sizeof(uint32_t));
    g_replayBootstrapSelectorLifecycleValid = true;
    return g_replayObservedBootstrapFingerprint != 0u;
}

static bool replaySelectRecordedStateHashAlgorithm(const uint64_t algorithmId) {
    using Algorithm = RollbackManager::HashAlgorithm;
    Algorithm algorithm = Algorithm::Fnv1a64;
    if (algorithmId == qreplay::kReplayStateHashAlgorithmIdV2) {
        algorithm = Algorithm::Fnv1a64;
    } else if (algorithmId == qreplay::kReplayStateHashAlgorithmIdBlock64V1) {
        algorithm = Algorithm::Block64;
    } else {
        return false;
    }
    g_rb.setHashAlgorithm(algorithm);
    QLOG("[replay.v3] state-proof primitive taken from the recording: %s "
         "(id=%016llX)",
         algorithm == Algorithm::Fnv1a64 ? "fnv1a64-le-v1" : "block64-le-v1",
         static_cast<unsigned long long>(algorithmId));
    return true;
}

static bool replayFinishBootstrapV3() {
    const qreplay::GameManifestV2& manifest =
        g_replayGameStartV2.manifest;
    const qreplay::DeterministicGameStartV2& start =
        g_replayGameStartV2.start;
    if (!replaySelectRecordedStateHashAlgorithm(
            manifest.contract.stateHashAlgorithmId)) {
        replayFailBootstrapV3(
            "recording uses a state-hash algorithm this build cannot compute");
        return false;
    }
    const auto roster = netReplayRosterV2();
    for (size_t slot = 0u; slot < qreplay::kFighterSlots; ++slot) {
        if (roster[slot] != manifest.slots[slot].identity) {
            replayFailBootstrapV3("native roster differs from GameStart",
                                  slot);
            return false;
        }
    }
    const qreplay::StageIdentityV2 stage = netReplayStageV2();
    if (stage != manifest.stage) {
        replayFailBootstrapV3("native applied stage differs from GameStart",
                              stage.nativeStageId);
        return false;
    }
    if (g_replayBootstrapSelectorLifecycleValid) {
        wr32(kReplaySelectorConfirmAddr,
             g_replayBootstrapSelectorConfirms[0]);
        wr32(kReplaySelectorConfirmAddr + sizeof(uint32_t),
             g_replayBootstrapSelectorConfirms[1]);
        wr32(kReplaySelectorLaunchStateAddr,
             g_replayBootstrapSelectorLaunch);
        wr32(kReplaySessionKnockoutLatchAddr,
             g_replayBootstrapKnockoutLatches[0]);
        wr32(kReplaySessionKnockoutLatchAddr + sizeof(uint32_t),
             g_replayBootstrapKnockoutLatches[1]);
    }
    if (!ensureRollback()) {
        replayFailBootstrapV3("rollback layout unavailable at frame zero");
        return false;
    }
    if (!netCanonicalizeHumanGameStartTransients(0u)) {
        replayFailBootstrapV3(
            "human game-start transient canonicalization failed");
        return false;
    }
    const char* layoutWhy = nullptr;
    if (!g_rb.validateLive(&layoutWhy)) {
        replayFailBootstrapV3(
            layoutWhy ? layoutWhy : "rollback layout validation failed");
        return false;
    }
    netResetFpuContract();
    g_netFpuInitializedEpoch = g_netEpoch;
    netAutomatchPrepareCanonicalFrame(0u, false);
    netNormalizeHumanMatchAiVm(0u, false);
    if (netLiveTaggedGameplayHash(0u) != start.frameZeroPreStateHash) {
        constexpr uintptr_t kSelectorRouteBase = 0x8716F0u;
        constexpr uintptr_t kSelectorLaunchCounter = 0x8718B8u;
        std::array<uint32_t, 4> originalRoutes{};
        for (size_t i = 0u; i < originalRoutes.size(); ++i)
            originalRoutes[i] = rd32(kSelectorRouteBase + 4u * i);
        const uint32_t originalCounter = rd32(kSelectorLaunchCounter);
        std::array<uint32_t, 4> candidate{{0u, 1u, 2u, 3u}};
        bool resolved = false;
        do {
            for (const uint32_t counter : {0u, 30u}) {
                for (size_t i = 0u; i < candidate.size(); ++i)
                    wr32(kSelectorRouteBase + 4u * i, candidate[i]);
                wr32(kSelectorLaunchCounter, counter);
                if (netLiveTaggedGameplayHash(0u)
                        == start.frameZeroPreStateHash) {
                    resolved = true;
                    QLOG("[replay.v3] frame-zero selector carry resolved "
                         "routes=%u/%u/%u/%u launchCounter=%u",
                         candidate[0], candidate[1], candidate[2],
                         candidate[3], counter);
                    break;
                }
            }
            if (resolved) break;
        } while (std::next_permutation(
            candidate.begin(), candidate.end()));
        if (!resolved) {
            for (size_t i = 0u; i < originalRoutes.size(); ++i)
                wr32(kSelectorRouteBase + 4u * i, originalRoutes[i]);
            wr32(kSelectorLaunchCounter, originalCounter);
        }
    }
    const uint32_t phase = netPhaseTag();
    uint64_t preHash = 0u;
    if (!netSavePreFrame(0u, phase, preHash)) {
        replayFailBootstrapV3("frame-zero state capture failed");
        return false;
    }
    qreplay::ReplayBootstrapObservationV2 observed{};
    observed.initialPhase = g_replayObservedInitialPhase;
    observed.bootstrapFingerprint =
        g_replayObservedBootstrapFingerprint;
    observed.frameZeroPreStateHash = preHash;
    observed.frameZeroLifecycle =
        netReplayLifecycleV2(manifest.layoutGeneration);
    if (observed.bootstrapFingerprint != start.bootstrapFingerprint
        && observed.initialPhase == start.initialPhase
        && observed.frameZeroPreStateHash == start.frameZeroPreStateHash) {
        static constexpr AudioManifestRule kOlderRules[] = {
            AudioManifestRule::StatusSoundsOnly,
            AudioManifestRule::WithCharacterVoices,
        };
        for (const AudioManifestRule rule : kOlderRules) {
            const uint64_t legacy =
                netFrontendFingerprint(false, g_ovrFrame, rule);
            if (legacy == 0u || legacy != start.bootstrapFingerprint) continue;
            QLOG("[replay.v3] bootstrap proof accepted under the older '%s' "
                 "audio rule (recorded=%016llX current-rule=%016llX "
                 "older-rule=%016llX). Sound files have since left the proof "
                 "entirely; frame-zero state matches exactly, so the "
                 "simulation is identical and only the proof's shape differs.",
                 audioManifestRuleName(rule),
                 static_cast<unsigned long long>(start.bootstrapFingerprint),
                 static_cast<unsigned long long>(
                     observed.bootstrapFingerprint),
                 static_cast<unsigned long long>(legacy));
            observed.bootstrapFingerprint = start.bootstrapFingerprint;
            break;
        }
    }
    if (observed.bootstrapFingerprint != start.bootstrapFingerprint
        && observed.initialPhase == start.initialPhase
        && observed.frameZeroPreStateHash == start.frameZeroPreStateHash) {
        QLOG("[replay.v3] bootstrap proof differs but frame-zero state is "
             "IDENTICAL (recorded=%016llX observed=%016llX frame0=%016llX). "
             "The recording starts from the same simulation state, so it "
             "plays; per-frame state verification stays binding and is the "
             "real check.",
             static_cast<unsigned long long>(start.bootstrapFingerprint),
             static_cast<unsigned long long>(observed.bootstrapFingerprint),
             static_cast<unsigned long long>(observed.frameZeroPreStateHash));
        observed.bootstrapFingerprint = start.bootstrapFingerprint;
    }
    if (replayProofObserveMode()
        && observed.frameZeroPreStateHash != start.frameZeroPreStateHash) {
        QLOG("[replay.observe] frame-zero proof differs recorded=%016llX "
             "observed=%016llX - continuing in observe mode",
             static_cast<unsigned long long>(start.frameZeroPreStateHash),
             static_cast<unsigned long long>(
                 observed.frameZeroPreStateHash));
        netLogReplayStateComponents("observe-bootstrap-mismatch", 0u);
        g_replayObserveBootstrapDiffered = true;
        observed.initialPhase = start.initialPhase;
        observed.bootstrapFingerprint = start.bootstrapFingerprint;
        observed.frameZeroPreStateHash = start.frameZeroPreStateHash;
    }
    if (!qgame::replay::replayCompleteBootstrapV2(observed)) {
        netLogReplayStateComponents("playback-bootstrap-mismatch", 0u);
        netLogLayoutDigest("replay-bootstrap-mismatch");
        if (observed.bootstrapFingerprint != start.bootstrapFingerprint) {
            netExplainBootstrapFingerprintMismatch(
                start.bootstrapFingerprint, observed.bootstrapFingerprint);
        }
        const qreplay::GameLifecycleV2& actualLifecycle =
            observed.frameZeroLifecycle;
        QLOG_ISSUE("[replay.v3] recorded bootstrap proof mismatch "
             "phase=%08X/%08X fingerprint=%016llX/%016llX "
             "frame0=%016llX/%016llX actualLifecycle="
             "[%u,%u,%d,%u,%u,%u,%u,%u/%u,%d]",
             start.initialPhase, observed.initialPhase,
             static_cast<unsigned long long>(
                 start.bootstrapFingerprint),
             static_cast<unsigned long long>(
                 observed.bootstrapFingerprint),
             static_cast<unsigned long long>(
                 start.frameZeroPreStateHash),
             static_cast<unsigned long long>(
                 observed.frameZeroPreStateHash),
             actualLifecycle.scene,
             actualLifecycle.requestedScene,
             actualLifecycle.matchSub,
             actualLifecycle.sceneThreeState,
             actualLifecycle.roundState,
             actualLifecycle.roundCounter,
             actualLifecycle.resultCounter,
             actualLifecycle.roundTallies[0],
             actualLifecycle.roundTallies[1],
             actualLifecycle.winnerSlot);
        return false;
    }
    g_netGameplaySegmentActive = true;
    g_netGameplaySegmentSeen = true;
    g_netFrame = 0u;
    g_replayBootstrapPhase = ReplayBootstrapPhase::Complete;
    g_replayStartObservation = observed;
    if (!replayTakeoverCaptureSnapshot(g_replayStartSnapshot, true)) {
        QLOG("[replay.v3] replay start snapshot unavailable");
        qgame::replay::replayFailPlaybackV2();
        return false;
    }
    QLOG("[replay.v3] bootstrap verified phase=%08X fingerprint=%016llX "
         "frame0=%016llX layout=%u stage=%u",
         phase,
         static_cast<unsigned long long>(
             g_replayObservedBootstrapFingerprint),
         static_cast<unsigned long long>(preHash),
         manifest.layoutGeneration, manifest.stage.nativeStageId);
    return true;
}

static int replayBeginNextNativeGameV3() {
    if (!qgame::replay::replayNextGamePendingV3()
        || !qgame::replay::replayBeginNextGameV3())
        return kNetHoldFrameRet;

    qgame::stopDirectBoot();
    replayTakeoverStopInputRuntime();
    replayReleaseCustomHitboxes();
    g_replayTakeoverSnapshot = ReplayTakeoverNativeSnapshot{};
    g_replayStartSnapshot = ReplayTakeoverNativeSnapshot{};
    g_replayStartObservation = qreplay::ReplayBootstrapObservationV2{};
    g_replaySeekCache.clear();
    g_replayTakeoverInputWindow = nullptr;
    netPaletteResetForSession();
    g_replayHalted = false;
    g_replayTicks = g_replayHiddenTicks = g_replayBufferingSlots = 0u;
    g_replayBootstrapPhase = ReplayBootstrapPhase::WaitingForStart;
    g_replayGameStartV2 = qreplay::ConfirmedGameStartV2{};
    g_replayObservedBootstrapFingerprint = 0u;
    g_replayObservedInitialPhase = 0u;
    g_replayBootstrapSlots = 0u;
    g_replayBootstrapSelectorConfirms = {};
    g_replayBootstrapSelectorLaunch = 0u;
    g_replayBootstrapKnockoutLatches = {};
    g_replayBootstrapSelectorLifecycleValid = false;
    g_replayClosePosted = false;
    g_replayTerminalSerial = 0;
    g_replayWinScreenTicks = 0u;
    g_replayObserveBootstrapDiffered = false;
    g_replayObserveFrames = g_replayObservePreMismatch = g_replayObservePostMismatch = 0u;
    g_replayObserveFirstPostMatch = g_replayObserveLastPostMismatch = UINT32_MAX;
    g_netEpoch = 1u;
    g_netBootstrapTransientsEpoch = 0u;
    g_netFpuInitializedEpoch = 0u;
    g_netFrame = 0u;
    g_ovrFrame = 0u;
    g_netInject[0] = g_netInject[1] = 0;
    g_netGameplaySegmentActive = g_netGameplaySegmentSeen = false;
    g_netGameplayRefreshPending = false;
    reregisterFrameData();
    resetCanonicalAudioSession();
    if (g_audioV2Enabled) qaudio::runtime().resetSession();
    wr32(0x49EF00u, 1u);
    wr32(0x4AA454u, 0u);
    wr32(0x8718B8u, 0u);
    wr32(0x8716E8u, 0u);
    wr32(0x4AA450u, 0u);
    QLOG("[replay.folder] native rollover: return=2; next file awaits fresh bootstrap");
    return 2;
}

static int doReplayV3(FrameDispatchFn orig) {
    const qgame::replay::ReplayTickPlan plan =
        qgame::replay::replayPlanOuterSlot();
    if (qgame::replay::replayComplete())
        replayReleaseCustomHitboxes();
    else
        replaySetCustomHitboxes(qgame::replay::replayCollisionDisplayRequested());

    if (g_replayTerminalSerial != 0
        && qgame::replay::replayAwaitingFinalPresentationV2()
        && InterlockedCompareExchange(
               &g_replayPresentedTickSerial, 0, 0)
            == g_replayTerminalSerial) {
        (void)qgame::replay::replayAcknowledgeFinalPresentationV2(
            static_cast<uint64_t>(g_replayTerminalSerial));
        g_replayTerminalSerial = 0;
    }

    if (qgame::replay::replayNextGamePendingV3())
        return replayBeginNextNativeGameV3();

    if (qgame::replay::replayExitRequested()) {
        replayReleaseCustomHitboxes();
        replayTakeoverStopInputRuntime();
        g_replayTakeoverSnapshot.valid = false;
        if (g_audioV2Enabled)
            qaudio::runtime().endReplayTakeover();
        if (!g_replayClosePosted
            && qgame::netRequestGameWindowClose()) {
            g_replayClosePosted = true;
        }
        Sleep(1);
        return kNetHoldFrameRet;
    }
    if (qgame::replay::replayComplete()) {
        if (qgame::replay::replayWinScreenPendingV3()) {
            bool bannerSettled = true;
            for (uintptr_t slide = 0u; slide < 9u; ++slide)
                bannerSettled = bannerSettled
                    && rd32(0x8A5628u + 20u * slide) == 0u;
            const auto action = qreplay::replayWinScreenAction(
                rd32(kSceneStateAddr), static_cast<int32_t>(rd32(kMatchSubAddr)),
                rd32(0x62369Cu), rd32(0x6236C8u), bannerSettled,
                g_replayWinScreenTicks);
            if (action == qreplay::ReplayWinScreenAction::Advance) {
                const LONG serial = InterlockedIncrement(&g_netOwnedTickSerial);
                int result = kNetHoldFrameRet;
                if (!replayRunOneTick(orig, g_netFrame, false, serial, result)) {
                    qgame::replay::replayCompleteWinScreenV3(false);
                    return kNetHoldFrameRet;
                }
                ++g_netFrame;
                ++g_replayWinScreenTicks;
                return result;
            }
            qgame::replay::replayCompleteWinScreenV3(
                action == qreplay::ReplayWinScreenAction::Hold);
        }
        replayTakeoverStopInputRuntime();
        g_replayTakeoverSnapshot.valid = false;
        if (g_audioV2Enabled)
            qaudio::runtime().endReplayTakeover();
        if (!qgame::replay::replayHeadlessV3()) trainingRepaintHeldFrame();
        Sleep(1);
        return kNetHoldFrameRet;
    }

    if (g_replayBootstrapPhase
            == ReplayBootstrapPhase::WaitingForStart) {
        if (!replayArmRecordedDirectBootV3()) {
            Sleep(1);
            return kNetHoldFrameRet;
        }
    }
    if (g_replayBootstrapPhase == ReplayBootstrapPhase::DirectBoot
        || g_replayBootstrapPhase == ReplayBootstrapPhase::NativeSetup) {
        if (++g_replayBootstrapSlots > 3600u) {
            replayFailBootstrapV3("recorded DirectBoot timed out");
            return kNetHoldFrameRet;
        }
        if (qgame::directBootNetworkActive())
            qgame::directBootNetworkTick();
        if (qgame::directBootNetworkFailed()) {
            replayFailBootstrapV3("recorded DirectBoot failed");
            return kNetHoldFrameRet;
        }
        if (g_replayBootstrapPhase == ReplayBootstrapPhase::DirectBoot
            && qgame::directBootNetworkComplete()) {
            if (rd32(kSceneStateAddr) != 2u
                || static_cast<int32_t>(rd32(kMatchSubAddr)) != -1) {
                replayFailBootstrapV3(
                    "DirectBoot did not reach the match setup boundary");
                return kNetHoldFrameRet;
            }
            if (!replayApplyRecordedStartV3()) {
                replayFailBootstrapV3(
                    "could not apply deterministic GameStart");
                return kNetHoldFrameRet;
            }
            g_replayBootstrapPhase = ReplayBootstrapPhase::NativeSetup;
        }
        (void)replayRunBootstrapTick(orig);
        if (g_replayBootstrapPhase == ReplayBootstrapPhase::NativeSetup
            && rollbackEligible()) {
            (void)replayFinishBootstrapV3();
        }
        return kNetHoldFrameRet;
    }

    if (qgame::replay::replayRewindRequested()) {
        const uint32_t target = qgame::replay::replayRewindTarget();
        const auto* entry = g_replaySeekCache.find(target);
        const uint32_t cursor = entry ? entry->nextRecord : 0u;
        auto* checkpoint = entry ? entry->state.get() : nullptr;
        bool restored = checkpoint
            ? replayTakeoverRestoreSnapshot(checkpoint->native, false, &checkpoint->audio)
            : replayTakeoverRestoreSnapshot(g_replayStartSnapshot, true);
        auto observed = g_replayStartObservation;
        if (restored) {
            observed.frameZeroPreStateHash = netLiveTaggedGameplayHash(g_netFrame);
            observed.frameZeroLifecycle = netReplayLifecycleV2(
                g_replayGameStartV2.manifest.layoutGeneration);
            if (checkpoint) restored = observed.frameZeroPreStateHash == checkpoint->preStateHash
                && observed.frameZeroLifecycle == checkpoint->lifecycle;
        }
        const bool ok = qgame::replay::replayCompleteRewind(
            restored, observed, checkpoint ? &checkpoint->playback : nullptr);
        g_replaySeekCache.discardAfter(cursor);
        QLOG("[replay.seek] restored=%u checkpoint=%u target=%u hiddenTicks=%u stride=%u bytes=%zu",
             ok ? 1u : 0u, cursor, target, target - cursor,
             g_replaySeekCache.stride(), g_replaySeekCache.bytes());
        return kNetHoldFrameRet;
    }

    if (qgame::replay::replayTakeoverNeedsSnapshot()) {
        const bool inputReady = replayTakeoverStartInputRuntime();
        const bool captured = inputReady && replayTakeoverCaptureSnapshot();
        (void)qgame::replay::replayTakeoverCompleteSnapshot(captured);
        if (!captured) {
            if (g_audioV2Enabled)
                qaudio::runtime().endReplayTakeover();
            replayTakeoverStopInputRuntime();
            g_replayTakeoverSnapshot.valid = false;
        }
        if (!captured || !netPaceIdleSlot()) {
            netPaceSemanticHold();
            Sleep(1);
        }
        return kNetHoldFrameRet;
    }
    if (qgame::replay::replayTakeoverRestorePending()) {
        const qreplay::ReplayTakeoverRestoreReason reason =
            qgame::replay::replayTakeoverRestoreReason();
        const bool restored = replayTakeoverRestoreSnapshot();
        const bool completed =
            qgame::replay::replayTakeoverCompleteRestore(restored);
        if (completed
            && reason == qreplay::ReplayTakeoverRestoreReason::Exit) {
            if (g_audioV2Enabled)
                qaudio::runtime().endReplayTakeover();
            replayTakeoverStopInputRuntime();
            g_replayTakeoverSnapshot.valid = false;
            g_replayTakeoverSnapshot.regions.clear();
        }
        const bool countdownRestarted = completed
            && reason != qreplay::ReplayTakeoverRestoreReason::Exit;
        if (!countdownRestarted || !netPaceIdleSlot()) {
            netPaceSemanticHold();
            Sleep(1);
        }
        return kNetHoldFrameRet;
    }

    if (plan.takeover && !plan.paused) {
        if (!rollbackEligible() || rd32(addr::kRoundCounter) == 5u) {
            QLOG("[replay.takeover] branch reached lifecycle boundary; "
                 "restoring original replay frame=%u", g_netFrame);
            (void)qgame::replay::replayTakeoverRequestSafetyExit();
            netPaceSemanticHold();
            Sleep(1);
            return kNetHoldFrameRet;
        }
        qreplay::ReplayTakeoverResolvedFrame branch{};
        if (!g_replayTakeoverInputRuntimeReady
            || !qgame::replay::replayTakeoverBeginFrame(
                replayTakeoverCaptureCompactInput(), branch)) {
            QLOG("[replay.takeover] branch input/resolve failed frame=%u",
                 g_netFrame);
            netPaceSemanticHold();
            Sleep(1);
            return kNetHoldFrameRet;
        }
        g_netFrame = branch.sourceFrame;
        netAutomatchPrepareCanonicalFrame(branch.sourceFrame, false);
        netNormalizeHumanMatchAiVm(branch.sourceFrame, false);
        const LONG serial = InterlockedIncrement(&g_netOwnedTickSerial);
        int tickResult = kNetHoldFrameRet;
        if (!replayRunOneTick(orig, branch.sourceFrame, false, serial,
                              tickResult, true)) {
            return kNetHoldFrameRet;
        }
        netPrepareNextCanonicalBoundary(branch.sourceFrame, false);
        g_netFrame = branch.sourceFrame + 1u;
        if (g_audioV2Enabled)
            qaudio::runtime().flushPending(branch.sourceFrame);
        (void)qgame::replay::replayTakeoverCommitFrame();
        g_netGameplaySegmentActive = rollbackEligible();
        return tickResult;
    }

    if (plan.paused || (!plan.present && plan.hiddenTicks == 0u)) {
        if (!qgame::replay::replayHeadlessV3()) trainingRepaintHeldFrame();
        const bool cadenceHold = plan.slowMotionHold
            || plan.takeoverCountdownHold;
        if (!cadenceHold || !netPaceIdleSlot()) {
            netPaceSemanticHold();
            Sleep(1);
        }
        return kNetHoldFrameRet;
    }
    const uint32_t totalTicks =
        plan.hiddenTicks + (plan.present ? 1u : 0u);
    int visibleResult = kNetHoldFrameRet;
    for (uint32_t tick = 0u; tick < totalTicks; ++tick) {
        qreplay::ConfirmedGameFrameV2 record{};
        bool finalFrame = false;
        bool headless = false;
        if (!qgame::replay::replayPeekFrameV2(
                record, finalFrame, headless)) {
            break;
        }
        bool hidden = !plan.present || tick + 1u < totalTicks;
        if (finalFrame && !headless) hidden = false;
        const uint32_t frame = record.replayFrame;
        const uint32_t simulationFrame = record.sourceFrame;
        g_netFrame = simulationFrame;
        const uint32_t phaseBefore = netPhaseTag();
        const bool gameplayBefore = rollbackEligible();
        netAutomatchPrepareCanonicalFrame(simulationFrame, false);
        netNormalizeHumanMatchAiVm(simulationFrame, false);
        uint64_t observedPreHash = 0u;
        if (gameplayBefore) {
            if (!ensureRollback()) break;
            if (!g_netGameplaySegmentActive) {
                const char* why = nullptr;
                if (!g_rb.validateLive(&why)) {
                    QLOG("[replay.v3] gameplay layout validation failed: %s",
                         why ? why : "unknown");
                    qgame::replay::replayFailPlaybackV2();
                    break;
                }
                netResetFpuContract();
                g_netFpuInitializedEpoch = g_netEpoch;
            }
            if (!netSavePreFrame(
                    simulationFrame, phaseBefore, observedPreHash)) {
                break;
            }
        } else {
            g_netGameplaySegmentActive = false;
            observedPreHash = netFrontendStateFingerprint(simulationFrame);
        }

        const LONG serial = InterlockedIncrement(&g_netOwnedTickSerial);
        qreplay::ReplayFrameBeginObservationV2 begin{};
        begin.replayFrame = frame;
        begin.preStateHash = observedPreHash;
        begin.lifecycleBefore = netReplayLifecycleV2(
            g_replayGameStartV2.manifest.layoutGeneration);
        begin.activeLaneMask = record.core.activeLaneMask;
        begin.finalPresentationSerial = finalFrame && !headless
            ? static_cast<uint64_t>(serial) : 0u;
        if (!headless && gameplayBefore && g_replaySeekCache.wants(frame)) {
            if (g_audioV2Enabled && simulationFrame != 0u) {
                if (qgame::replay::replayNavigationActive())
                    qaudio::runtime().flushPendingMuted(simulationFrame - 1u);
                else
                    qaudio::runtime().flushPending(simulationFrame - 1u);
            }
            try {
                auto checkpoint = std::make_unique<ReplaySeekSnapshot>();
                if (qgame::replay::replayCaptureCheckpoint(checkpoint->playback)
                    && replayTakeoverCaptureSnapshot(checkpoint->native, false, &checkpoint->audio)) {
                    checkpoint->preStateHash = begin.preStateHash;
                    checkpoint->lifecycle = begin.lifecycleBefore;
                    const size_t bytes = sizeof(ReplaySeekSnapshot) + checkpoint->native.regions.capacity();
                    (void)g_replaySeekCache.insert(frame, bytes, std::move(checkpoint));
                }
            } catch (const std::bad_alloc&) {
                QLOG("[replay.seek] checkpoint allocation unavailable frame=%u", frame);
            }
        }
        bool observeLifecycleBeforeDiff = false;
        if (replayProofObserveMode()) {
            observeLifecycleBeforeDiff =
                !(begin.lifecycleBefore == record.core.before);
            g_replayObserveLastPre = begin.preStateHash;
            begin.preStateHash = record.core.preStateHash;
            begin.lifecycleBefore = record.core.before;
        }
        if (!qgame::replay::replayBeginFrameV2(begin)) break;
        if (gameplayBefore && !g_netGameplaySegmentActive) {
            g_netGameplaySegmentActive = true;
            g_netGameplaySegmentSeen = true;
        }
        if (finalFrame && !headless)
            g_replayTerminalSerial = serial;

        int tickResult = kNetHoldFrameRet;
        if (!replayRunOneTick(
                orig, simulationFrame, hidden, serial, tickResult)) {
            break;
        }
        netPrepareNextCanonicalBoundary(simulationFrame, false);
        const uint64_t observedPostHash = netSpectatorLiveStateHash(
            static_cast<uint64_t>(simulationFrame) + 1u);
        qreplay::ReplayFrameCommitObservationV2 commit{};
        commit.replayFrame = frame;
        commit.postStateHash = observedPostHash;
        commit.lifecycleAfter = netReplayLifecycleV2(
            g_replayGameStartV2.manifest.layoutGeneration);
        commit.dispatcherResult = tickResult;
        if (observedPostHash != record.core.postStateHash
            && qgame::replay::replayVerificationModeV2()
                == qreplay::ReplayVerificationModeV2::ByteExact) {
            netLogReplayStateComponents(
                "playback-post-mismatch", simulationFrame);
            QLOG_ISSUE("[replay.v3] post proof mismatch frame=%u source=%u "
                 "expected=%016llX actual=%016llX actions=%04X/%04X "
                 "result=%d/%d lifecycle=%u/%d/%u/%u->%u/%d/%u/%u",
                 frame, simulationFrame,
                 static_cast<unsigned long long>(
                     record.core.postStateHash),
                 static_cast<unsigned long long>(observedPostHash),
                 record.core.actions[0], record.core.actions[1],
                 record.core.dispatcherResult, tickResult,
                 record.core.after.scene, record.core.after.matchSub,
                 record.core.after.roundState,
                 record.core.after.roundCounter,
                 commit.lifecycleAfter.scene,
                 commit.lifecycleAfter.matchSub,
                 commit.lifecycleAfter.roundState,
                 commit.lifecycleAfter.roundCounter);
        }
        if (replayProofObserveMode()) {
            replayObserveFrameV3(
                frame, record.core.preStateHash, g_replayObserveLastPre,
                record.core.postStateHash, observedPostHash,
                observeLifecycleBeforeDiff,
                !(commit.lifecycleAfter == record.core.after),
                commit.dispatcherResult != record.core.dispatcherResult);
            commit.postStateHash = record.core.postStateHash;
            commit.lifecycleAfter = record.core.after;
            commit.dispatcherResult = record.core.dispatcherResult;
        }
        if (!qgame::replay::replayCommitFrameV2(commit)) break;
        g_netFrame = simulationFrame + 1u;
        if (g_audioV2Enabled) {
            if (hidden && qgame::replay::replayNavigationActive())
                qaudio::runtime().flushPendingMuted(simulationFrame);
            else if (!hidden)
                qaudio::runtime().flushPending(simulationFrame);
        }
        visibleResult = tickResult;
        if (!rollbackEligible()) g_netGameplaySegmentActive = false;

        if (finalFrame && !headless
            && InterlockedCompareExchange(
                   &g_replayPresentedTickSerial, 0, 0) == serial) {
            (void)qgame::replay::replayAcknowledgeFinalPresentationV2(
                static_cast<uint64_t>(serial));
            g_replayTerminalSerial = 0;
        }
        if (finalFrame) {
            netLogReplayObserveSummary("final-frame");
            break;
        }
        if (qgame::spectatorCatchupMustYield(
                tickResult != kNetHoldFrameRet,
                gameplayBefore != rollbackEligible(),
                netPhaseTag() != phaseBefore,
                rd32(0x6234EC), rd32(0x61801C))) {
            break;
        }
    }
    return visibleResult;
}

int doReplay(FrameDispatchFn orig) {
    if (!g_replayRoleActive)
        return orig ? orig() : kNetHoldFrameRet;
    if (!qgame::replay::replayRuntimeActive())
        replayReleaseCustomHitboxes();
    if (g_replayHalted) {
        Sleep(1);
        return kNetHoldFrameRet;
    }
    if (!qgame::replay::replayRuntimeActive()) {
        replayHalt(0, "replay broker channel is not active", 0, 0);
        return kNetHoldFrameRet;
    }
    if (g_netActive || g_netSpectatorMode || g_inputConfigPresent) {
        replayHalt(0, "replay session acquired player/spectator/input state",
                   0, 0);
        return kNetHoldFrameRet;
    }
    if (rd32(kNetIsNetworkAddr) != 0u) {
        replayHalt(0, "replay left QOH local mode", 0,
                   rd32(kNetIsNetworkAddr));
        return kNetHoldFrameRet;
    }
    if (!g_readHooked || !g_pollHooked || !g_paceBracketHooked
        || !g_clockHooked || !g_configSaveHooked
        || !playbackSideEffectHooksReady()) {
        replayHalt(0, "mandatory replay playback hook disappeared", 0, 0);
        return kNetHoldFrameRet;
    }

    if (qgame::replay::replayPlaybackV3())
        return doReplayV3(orig);

    if (qgame::directBootNetworkActive()
        && !qgame::directBootNetworkAtStartBarrier()) {
        qgame::directBootNetworkTick();
        if (qgame::directBootNetworkFailed()) {
            replayHalt(0, "deterministic DirectBoot failed before the start "
                          "barrier", 0, 0);
            return kNetHoldFrameRet;
        }
        return orig ? orig() : kNetHoldFrameRet;
    }

    const qgame::replay::ReplayTickPlan plan =
        qgame::replay::replayPlanOuterSlot();
    replaySetCustomHitboxes(
        qgame::replay::replayCollisionDisplayRequested());
    if (plan.paused || (!plan.present && plan.hiddenTicks == 0u)) {
        trainingRepaintHeldFrame();
        netPaceSemanticHold();
        Sleep(1);
        return kNetHoldFrameRet;
    }
    const uint32_t totalTicks =
        plan.hiddenTicks + (plan.present ? 1u : 0u);
    int visibleResult = kNetHoldFrameRet;
    for (uint32_t tick = 0; tick < totalTicks; ++tick) {
        const bool hidden = !plan.present || (tick + 1u < totalTicks);
        qreplay::ReplayFrameV1 record{};
        if (!qgame::replay::replayNextFrame(record)) {
            ++g_replayBufferingSlots;
            if (g_replayBufferingSlots == 1u)
                netLogReplayObserveSummary("source-drained");
            if (g_replayBufferingSlots <= 8u
                || g_replayBufferingSlots % 300u == 0u) {
                QLOG("[replay] local replay buffering (slot #%llu) - holding "
                     "presentation; no prediction and no device access",
                     static_cast<unsigned long long>(g_replayBufferingSlots));
            }
            netPaceSemanticHold();
            Sleep(1);
            break;
        }
        const uint32_t frame = record.timelineFrame;
        const uint32_t phaseBefore = netPhaseTag();
        const bool gameplayBefore = rollbackEligible();
        netAutomatchPrepareCanonicalFrame(frame, false);
        netNormalizeHumanMatchAiVm(frame, false);

        uint64_t observedPreHash = 0;
        if (gameplayBefore) {
            if (!ensureRollback()) {
                netPaceSemanticHold();
                Sleep(1);
                break;
            }
            if (!g_netGameplaySegmentActive) {
                if (!netCanonicalizeHumanGameStartTransients(frame)) {
                    replayHalt(
                        frame,
                        "human game-start transient canonicalization failed",
                        0, 0);
                    return kNetHoldFrameRet;
                }
                const char* why = nullptr;
                if (!g_rb.validateLive(&why)) {
                    replayHalt(frame,
                               why ? why : "rollback state validation failed",
                               0, 0);
                    return kNetHoldFrameRet;
                }
                netResetFpuContract();
                g_netFpuInitializedEpoch = g_netEpoch;
                g_netGameplaySegmentActive = true;
                g_netGameplaySegmentSeen = true;
            }
            if (!spectatorSavePreFrame(frame, phaseBefore, observedPreHash))
                return kNetHoldFrameRet;
        } else {
            g_netGameplaySegmentActive = false;
            observedPreHash = netFrontendStateFingerprint(frame);
        }
        if (record.preStateHash != 0
            && record.preStateHash != observedPreHash
            && !replayProofObserveMode()) {
            qgame::replay::replayReportMismatch(
                frame, record.preStateHash, observedPreHash);
            replayHalt(frame, "canonical pre-state differs from the recording",
                       record.preStateHash, observedPreHash);
            return kNetHoldFrameRet;
        }

        int tickResult = kNetHoldFrameRet;
        const LONG ownedTickSerial =
            InterlockedIncrement(&g_netOwnedTickSerial);
        if (!replayRunOneTick(
                orig, frame, hidden, ownedTickSerial, tickResult))
            return kNetHoldFrameRet;

        netPrepareNextCanonicalBoundary(frame, false);
        const uint64_t observedPostHash = netSpectatorLiveStateHash(
            static_cast<uint64_t>(frame) + 1u);
        if (observedPostHash == 0) {
            replayHalt(frame, "post-state projection failed", 0, 0);
            return kNetHoldFrameRet;
        }
        if (replayProofObserveMode()) {
            replayObserveFrame(frame, record.preStateHash, observedPreHash,
                               record.postStateHash, observedPostHash);
            qgame::replay::replayCommitFrame(record, record.postStateHash);
            g_netFrame = frame + 1u;
            if (g_audioV2Enabled && !hidden)
                qaudio::runtime().flushPending(frame);
            visibleResult = tickResult;
            const bool gameplayAfterObserve = rollbackEligible();
            if (!gameplayAfterObserve) g_netGameplaySegmentActive = false;
            if (qgame::spectatorCatchupMustYield(
                    tickResult != kNetHoldFrameRet,
                    gameplayBefore != gameplayAfterObserve,
                    netPhaseTag() != phaseBefore,
                    rd32(0x6234EC), rd32(0x61801C)))
                break;
            continue;
        }
        if (record.postStateHash != 0
            && record.postStateHash != observedPostHash) {
            qgame::replay::replayReportMismatch(
                frame, record.postStateHash, observedPostHash);
            replayHalt(frame, "canonical post-state differs from the recording",
                       record.postStateHash, observedPostHash);
            return kNetHoldFrameRet;
        }
        qgame::replay::replayCommitFrame(record, observedPostHash);
        g_netFrame = frame + 1u;
        if (g_audioV2Enabled && !hidden)
            qaudio::runtime().flushPending(frame);
        visibleResult = tickResult;

        const bool gameplayAfter = rollbackEligible();
        if (!gameplayAfter) g_netGameplaySegmentActive = false;
        if (qgame::spectatorCatchupMustYield(
                tickResult != kNetHoldFrameRet,
                gameplayBefore != gameplayAfter,
                netPhaseTag() != phaseBefore,
                rd32(0x6234EC), rd32(0x61801C)))
            break;
    }
    return visibleResult;
}

int doSelfTest() {
    if (g_stDisabled) return g_orig();
    if (!ensureRollback()) return g_orig();
    if (!rollbackEligible()) {
        g_frame = 0; return g_orig();
    }
    if (gameplayLayoutConfigChanged()) {
        reregisterFrameData();
        g_frame = 0;
        return g_orig();
    }

    static bool validated = false;
    if (!validated) {
        const char* why = nullptr;
        bool ok = g_rb.validateLive(&why);
        QLOG("[selftest] boot-validate scene=%d matchSub=%d P1=%08X P2=%08X child1=%08X child2=%08X active=%08X sel=%u -> %s%s%s",
             (int)rd32(kSceneStateAddr), (int)rd32(kMatchSubAddr),
             rd32(kFighterP1Addr), rd32(kFighterP2Addr),
             rd32(kFighterP1Addr + 164), rd32(kFighterP2Addr + 164),
             rd32(kActiveBlockPtr), rd32(kBlockSelector),
             ok ? "OK" : "FAIL(", ok ? "" : (why ? why : "?"), ok ? "" : ")");
        if (!ok) { g_stDisabled = true; return g_orig(); }
        validated = true;
        logRegionLayoutOnce();
        logSyncHashOnce();
        installGameplayHooksOnce();
        const MH_STATUS applied = MH_ApplyQueued();
        QLOG("[selftest] deferred gameplay hook transaction applied status=%d",
             static_cast<int>(applied));
    }

    static const bool kWideEnable = false;
    if (kWideEnable && g_frame == 400 && !g_wideCaptured && g_wideTruth.empty()) {
        g_wideTruth.resize(kWideSize); g_wideReplay.resize(kWideSize); g_wideWant = true;
        QLOG("[widediff] armed at frame 400 (range 0x%X..0x%X, %zu KB)",
             (unsigned)kWideBase, (unsigned)kWideEnd, kWideSize / 1024);
    }

    const uint64_t f = g_frame;

    if ((f % 600) == 300) {
        static std::vector<uint8_t> sa, sb;
        static int probes = 0;
        if (sa.size() != g_rb.totalSize()) { sa.assign(g_rb.totalSize(), 0); sb.assign(g_rb.totalSize(), 0); }
        g_rb.snapshotTo(sa.data());
        g_rb.snapshotTo(sb.data());
        struct Ctx { int logged; } ctx{ 0 };
        size_t total = g_rb.diffBuffers(sa.data(), sb.data(),
            [](const char* region, size_t off, uintptr_t va, size_t len, void* user) {
                Ctx* c = static_cast<Ctx*>(user);
                if (c->logged < 12)
                    QLOG("[async_writer]   region='%s'+0x%zX va=%08X len=%zu", region, off, (unsigned)va, len);
                c->logged++;
            }, &ctx);
        if (total || probes < 3)
            QLOG("[async_writer] probe frame=%llu: %zu bytes written by non-sim threads (%d ranges)",
                 (unsigned long long)f, total, ctx.logged);
        probes++;
    }

    int r = 0;
    bool ok = selfTestGuarded(f, &r);
    if (g_inputTest) inputTestSample(f);

    if (g_wideCaptured) {
        g_wideCaptured = false;
        const uint8_t* t = g_wideTruth.data(); const uint8_t* rp = g_wideReplay.data();
        size_t rs = 0; bool in = false; int reported = 0, total = 0; size_t missBytes = 0;
        QLOG("[widediff] truth-vs-replay changed ranges NOT in save set (= missing feeders):");
        for (size_t i = 0; i <= kWideSize; ++i) {
            bool d = (i < kWideSize) && (t[i] != rp[i]);
            if (d && !in) { in = true; rs = i; }
            else if (!d && in) {
                in = false;
                uintptr_t va = kWideBase + rs; size_t len = i - rs;
                if (!inSaveSet(va)) {
                    total++; missBytes += len;
                    if (reported < 40) {
                        QLOG("[widediff]   MISSING @%08X size=%zu (first bytes T=%02X%02X%02X%02X R=%02X%02X%02X%02X)",
                             (unsigned)va, len, t[rs], rs+1<kWideSize?t[rs+1]:0, rs+2<kWideSize?t[rs+2]:0, rs+3<kWideSize?t[rs+3]:0,
                             rp[rs], rs+1<kWideSize?rp[rs+1]:0, rs+2<kWideSize?rp[rs+2]:0, rs+3<kWideSize?rp[rs+3]:0);
                        reported++;
                    }
                }
            }
        }
        QLOG("[widediff] DONE: %d unsaved changed ranges, %zu missing bytes (saved regions are deterministic)",
             total, missBytes);
        std::vector<uint8_t>().swap(g_wideTruth); std::vector<uint8_t>().swap(g_wideReplay);
    }

    if (!ok) {
        if (g_stPhase >= 4) g_rb.loadFrom(g_expected.data());
        int prev = g_window;
        if (g_window > 1) {
            g_window = g_window / 2; g_okAtWindow = 0;
            QLOG("[selftest] FAULT code=0x%08lX phase=%d frame=%llu region='%s' childPtr=%08X (recovered=%s) -> shrink K %d->%d",
                 (unsigned long)g_stCrash, g_stPhase, (unsigned long long)f,
                 g_stDiff.region ? g_stDiff.region : "?", rd32(kFighterP1Addr + 164),
                 g_stPhase >= 4 ? "truth" : "asis", prev, g_window);
        } else {
            g_stDisabled = true;
            QLOG("[selftest] FAULT code=0x%08lX phase=%d frame=%llu faultAddr=%08X (rva=0x%X) at K=1 -> DISABLED",
                 (unsigned long)g_stCrash, g_stPhase, (unsigned long long)f,
                 (unsigned)g_faultAddr, (unsigned)(g_faultAddr - 0x400000));
        }
        return g_orig();
    }

    if (g_stResult == 0 && f >= (uint64_t)g_window) {
        g_selftestOk++; g_okAtWindow++;
        if ((g_selftestOk == 1) || (f % 300) == 0) {
            if (gameplaySyncGreen(g_selftestBad))
                QLOG("[selftest] frame=%llu K=%d GameplaySync OK bad=0 (ok=%llu okAtK=%llu)",
                     (unsigned long long)f, g_window,
                     (unsigned long long)g_selftestOk, (unsigned long long)g_okAtWindow);
            else
                QLOG("[selftest] frame=%llu K=%d GameplaySync RESIDUAL ok=%llu bad=%llu okAtK=%llu "
                     "(nonzero bad => NOT green; see DESYNC lines for the exact region)",
                     (unsigned long long)f, g_window,
                     (unsigned long long)g_selftestOk, (unsigned long long)g_selftestBad,
                     (unsigned long long)g_okAtWindow);
            if (!g_rcOk)
                QLOG("[selftest] frame=%llu K=%d RenderCosmetic DIFF first='%s'+0x%zX diffBytes=%zu (allowed)",
                     (unsigned long long)f, g_window, g_rcDiff.region ? g_rcDiff.region : "?",
                     g_rcDiff.offsetInRegion, g_rcDiff.diffBytes);
            if (!g_fdOk)
                QLOG("[selftest] frame=%llu K=%d FullDebugWide DIFF first='%s'+0x%zX diffBytes=%zu (diagnostic)",
                     (unsigned long long)f, g_window, g_fdDiff.region ? g_fdDiff.region : "?",
                     g_fdDiff.offsetInRegion, g_fdDiff.diffBytes);
        }
        if (!g_windowPin && g_window < kMaxWindow && g_okAtWindow >= kGrowAfter) {
            int prev = g_window; g_window = nextWindow(g_window); g_okAtWindow = 0;
            QLOG("[selftest] K=%d GREEN over %llu frames -> grow K %d->%d",
                 prev, (unsigned long long)kGrowAfter, prev, g_window);
        }
    } else if (g_stResult == 1) {
        g_selftestBad++; g_okAtWindow = 0;
        if (g_selftestBad <= 40 || (g_selftestBad % 100) == 0) {
            QLOG("[selftest] frame=%llu K=%d GameplaySync DESYNC region='%s'+0x%zX diffBytes=%zu restoreDiff=%u sel:%u->%u active:%08X->%08X (ok=%llu bad=%llu)",
                 (unsigned long long)f, g_window, g_stDiff.region ? g_stDiff.region : "?",
                 g_stDiff.offsetInRegion, g_stDiff.diffBytes, g_restoreDiffBytes,
                 g_selBefore, g_selAfter, g_actBefore, g_actAfter,
                 (unsigned long long)g_selftestOk, (unsigned long long)g_selftestBad);
            dumpDesyncFull(f, g_window);
            if (g_traceResidual && g_selftestBad <= 12) {
                QLOG("[residual_trace] frame=%llu firstRegion='%s'+0x%zX restoreDiffBytes=%u",
                     (unsigned long long)f, g_stDiff.region ? g_stDiff.region : "?",
                     g_stDiff.offsetInRegion, g_restoreDiffBytes);
                auto absg = [](const char* tag, const TickGate& g) {
                    QLOG("[residual_abs] %-13s latch=%u sel=%u frameCtr=%u c6236C8=%u f56=%u active=%08X keyHash=%08X",
                         tag, g.latch, g.selector, g.frameCtr, g.c6236C8, g.f56, g.active, g.keyHash);
                };
                absg("before_truth", g_gBeforeTruth);
                absg("after_truth", g_gAfterTruth);
                absg("after_restore", g_gAfterRestore);
                absg("after_replay", g_gAfterReplay);
                dumpGateDiff("after_restore-vs-before_truth (restore fidelity; want NONE)",
                             g_gBeforeTruth, g_gAfterRestore);
            }
            {
                const char* rgn = g_stDiff.region ? g_stDiff.region : "";
                bool gameplayCarve = std::strncmp(rgn, "dw_", 3) == 0 ||
                                     std::strncmp(rgn, "actor_clone_pool", 16) == 0 ||
                                     std::strncmp(rgn, "fighterP3", 9) == 0;
                if (gameplayCarve && g_selftestBad <= 80) {
                    QLOG("[dbg-residual] frame=%llu region='%s'+0x%zX diffBytes=%zu matchSub=%d sel:%u->%u active:%08X->%08X frameCtr=%08X",
                         (unsigned long long)f, rgn, g_stDiff.offsetInRegion, g_stDiff.diffBytes,
                         (int)rd32(kMatchSubAddr), g_selBefore, g_selAfter, g_actBefore, g_actAfter, rd32(0x4AB778));
                    // Actor-pool free-list heads: sub_42DB00 uses &dword_613E00/&dword_613E20; sub_42DB70 uses dword_626680.
                    QLOG("[dbg-residual]   freelist heads 613E00=%08X 613E04=%08X 613E20=%08X 626680=%08X sessCtr(553DC0)=%08X",
                         rd32(0x613E00), rd32(0x613E04), rd32(0x613E20), rd32(0x626680), rd32(0x553DC0));
                    QLOG("[dbg-residual]   P1 truth : +56=%08X +48=%08X +52=%08X +88=%08X +1400=%08X prevF56=%08X",
                         g_truthDbg.f56, g_truthDbg.f48, g_truthDbg.f52, g_truthDbg.f88, g_truthDbg.f1400, g_prevF56);
                    QLOG("[dbg-residual]   P1 replay: +56=%08X +48=%08X +52=%08X +88=%08X +1400=%08X (diff56=%d diff48=%d)",
                         g_replayDbg.f56, g_replayDbg.f48, g_replayDbg.f52, g_replayDbg.f88, g_replayDbg.f1400,
                         (int)(g_truthDbg.f56 != g_replayDbg.f56), (int)(g_truthDbg.f48 != g_replayDbg.f48));
                    QLOG("[step_parity]   rngG S_f=%08X truth=%08X(+%d) replay=%08X(+%d) | rngC t=%08X r=%08X | sel t=%u r=%u",
                         g_rngSeedAtSave,
                         g_truthDbg.rngG,  lcgSteps(g_rngSeedAtSave, g_truthDbg.rngG),
                         g_replayDbg.rngG, lcgSteps(g_rngSeedAtSave, g_replayDbg.rngG),
                         g_truthDbg.rngC, g_replayDbg.rngC, g_truthDbg.rngSel, g_replayDbg.rngSel);
                    QLOG("[input_parity]  merged t=%08X r=%08X | P1 t=%08X r=%08X | P2 t=%08X r=%08X | keyHash t=%08X r=%08X %s",
                         g_truthDbg.inMerged, g_replayDbg.inMerged, g_truthDbg.inP1, g_replayDbg.inP1,
                         g_truthDbg.inP2, g_replayDbg.inP2, g_truthDbg.keyHash, g_replayDbg.keyHash,
                         (g_truthDbg.keyHash != g_replayDbg.keyHash || g_truthDbg.inMerged != g_replayDbg.inMerged) ? "<<INPUT_DIVERGE" : "");
                    QLOG("[cam] camX t=%d r=%d | camPrev t=%d r=%d | p1x t=%d r=%d | p2x t=%d r=%d | p1+1312 t=%d r=%d",
                         (int)g_truthDbg.camX, (int)g_replayDbg.camX,
                         (int)g_truthDbg.camPrev, (int)g_replayDbg.camPrev,
                         (int)g_truthDbg.p1x, (int)g_replayDbg.p1x,
                         (int)g_truthDbg.p2x, (int)g_replayDbg.p2x,
                         (int)g_truthDbg.p1c1312, (int)g_replayDbg.p1c1312);
                    if (g_rngHooked) {
                        QLOG("[rng_count] truth  game=%u cos=%u redirected=%u | replay game=%u cos=%u redirected=%u | ringMiss=%d | otherThreadDraws=%ld",
                             g_rngTruthCnt.game, g_rngTruthCnt.cos, g_rngTruthCnt.redirected,
                             g_rngReplayCnt.game, g_rngReplayCnt.cos, g_rngReplayCnt.redirected,
                             g_redirRings[(int)(f % (uint64_t)kPoolSlots) % kRngRingSlots].miss,
                             (long)g_rngOtherThreadDraws);
                        for (LONG oi = 0; oi < g_rngOtherThreadSiteN && oi < 8; ++oi)
                            QLOG("[rng_callsite] OTHER-THREAD ra=0x%08X profile=%s",
                                 (unsigned)g_rngOtherThreadSites[oi],
                                 runtime_profile::selectedProfileKey());
                        int logged = 0;
                        for (int si = 0; si < g_rngSiteN && logged < 16; ++si) {
                            const RngSite& rs = g_rngSites[si];
                            if (rs.truthN == rs.replayN) continue;
                            QLOG("[rng_callsite] ra=0x%08X profile=%s truthN=%u replayN=%u wl=%d total=%llu <<DIVERGENT",
                                 (unsigned)rs.ra, runtime_profile::selectedProfileKey(),
                                 rs.truthN, rs.replayN, (int)rs.whitelisted,
                                 (unsigned long long)rs.totalN);
                            logged++;
                        }
                    }
                }
            }
            if (g_selftestBad <= 3) {
                std::vector<RollbackManager::RegionHash> per;
                g_rb.hashLive(&per);
                for (const auto& rh : per)
                    QLOG("[desync_region] region=%-22s size=%-7zu present=%d hash=%016llX",
                         rh.name, rh.size, (int)rh.present, (unsigned long long)rh.hash);
                const char* rgn = g_stDiff.region ? g_stDiff.region : "";
                if (rgn[0] == 'b' && rgn[1] == 'l') {
                    bool b1 = (rgn[5] == '1');
                    const uint8_t* truth = g_expected.data() + (b1 ? kBlockSize : 0);
                    const uint8_t* live  = reinterpret_cast<const uint8_t*>(b1 ? kBlock1Addr : kBlock0Addr);
                    traceArenaDiff(rgn, truth, live, kBlockSize, g_stDiff.offsetInRegion);
                }
            }
        }
    }
    return r;
}

static void logFlowState(const char* why) {
    uintptr_t cfg = rd32(0x613DE0);
    // 0x6236B0 is the VS-match launch flag (set in sub_48CA20 case 4), not a demo/attract flag.
    QLOG("[flow] %s scene=%d next=%d matchSub=%d net(4AAA50)=%u pauseA(cfg+84)=%u pauseB(cfg+88)=%u "
         "frameCtr=%u vsLaunch(6236B0)=%u skip(6267D0)=%u replayFile=%u gFrame=%llu eligible=%d",
         why, (int)rd32(kSceneStateAddr), (int)rd32(0x61801C), (int)rd32(kMatchSubAddr),
         rd32(0x4AAA50), cfg ? rd32(cfg + 84) : 0, cfg ? rd32(cfg + 88) : 0,
         rd32(0x4AB778), rd32(0x6236B0), rd32(0x6267D0) & 0xFF, rd32(0x4AB794) & 0xFF,
         (unsigned long long)g_frame, (int)rollbackEligible());
}

// matchSub: 0 = gameplay; 1..4 = transition; 5 = win.
static void trackMatchLifecycle() {
    static int  s_prevSub   = -999;
    static int  s_rounds    = 0, s_roundsDone = 0;
    int sub = (int)rd32(kMatchSubAddr);
    int scene = (int)rd32(kSceneStateAddr);
    if (scene != 2) {
        if (s_prevSub != -999)
            QLOG("[fullmatch] LEFT match scene (scene=%d next=%d matchSub=%d roundNo=%d) "
                 "after rounds_done=%d | cumulative ok=%llu bad=%llu %s",
                 scene, (int)rd32(0x61801C), sub, (int)rd32(0x6275A8), s_roundsDone,
                 (unsigned long long)g_selftestOk, (unsigned long long)g_selftestBad,
                 gameplaySyncGreen(g_selftestBad) ? "GREEN" : "RESIDUAL");
        s_prevSub = -999; return;
    }
    if (sub == s_prevSub) return;
    if (sub == 0 && s_prevSub != 0) {
        s_rounds++;
        QLOG("[fullmatch] round %d ENTER gameplay (matchSub %d->0) | cumulative GameplaySync ok=%llu bad=%llu %s",
             s_rounds, s_prevSub, (unsigned long long)g_selftestOk, (unsigned long long)g_selftestBad,
             gameplaySyncGreen(g_selftestBad) ? "GREEN" : "RESIDUAL");
    } else if (s_prevSub == 0 && sub != 0) {
        s_roundsDone++;
        QLOG("[fullmatch] round %d EXIT gameplay (matchSub 0->%d) rounds_completed=%d | cumulative ok=%llu bad=%llu %s",
             s_rounds, sub, s_roundsDone,
             (unsigned long long)g_selftestOk, (unsigned long long)g_selftestBad,
             gameplaySyncGreen(g_selftestBad) ? "GREEN" : "RESIDUAL");
    } else {
        QLOG("[fullmatch] transition matchSub %d->%d (scene=2 next=%d roundNo=%d) ok=%llu bad=%llu",
             s_prevSub, sub, (int)rd32(0x61801C), (int)rd32(0x6275A8),
             (unsigned long long)g_selftestOk, (unsigned long long)g_selftestBad);
    }
    s_prevSub = sub;
}

// dword_613DE0 points at the 764-byte live settings profile. Match launch copies its first 0x17C bytes to 0x623520; the running match reads that copy.
static void logMatchSettingsSnapshotOnRoundStart() {
    static int s_prevMatchSub = -999;
    const int scene = static_cast<int>(rd32(kSceneStateAddr));
    const int matchSub = static_cast<int>(rd32(kMatchSubAddr));
    const bool enteredGameplay =
        scene == 2 && matchSub == 0 && s_prevMatchSub != 0;
    s_prevMatchSub = (scene == 2) ? matchSub : -999;
    if (!enteredGameplay) return;

    const uintptr_t cfg = rd32(0x613DE0);
    if (!cfg) {
        QLOG("[match-settings] round start, but no active config pointer "
             "(dword_613DE0=0) - nothing to read yet");
        return;
    }
    constexpr uintptr_t kActive = 0x623520;
    constexpr uintptr_t kActiveBytes = 0x17Cu;
    static_assert(376u + 4u <= kActiveBytes,
                  "the flags field must be inside the copied snapshot");

    const uint32_t reqDifficulty = rd32(cfg + 44);
    const uint32_t reqFrameLoopOwner = rd32(cfg + 64);
    const uint32_t reqHitFlash = rd32(cfg + 68);
    const uint32_t reqWaitCount = rd32(cfg + 84);
    const uint32_t reqExtraSkip = rd32(cfg + 92);
    const uint32_t reqScreenMode = rd32(cfg + 108);
    const uint32_t reqFlags = rd32(cfg + 376);

    const uint32_t actDifficulty = rd32(kActive + 44);
    const uint32_t actFrameLoopOwner = rd32(kActive + 64);
    const uint32_t actHitFlash = rd32(kActive + 68);
    const uint32_t actWaitCount = rd32(kActive + 84);
    const uint32_t actExtraSkip = rd32(kActive + 92);
    const uint32_t actScreenMode = rd32(kActive + 108);
    const uint32_t actFlags = rd32(kActive + 376);

    const uint32_t fx0 = rd32(0x623720);
    const uint32_t fx1 = rd32(0x623724);
    const uint32_t fx2 = rd32(0x623728);
    const uint32_t fx3 = rd32(0x62372C);
    const uint32_t fx4 = rd32(0x623730);

    QLOG("[match-settings] round start cfg=%08X | requested(profile) "
         "difficulty(+44)=%u frameLoopOwner(+64)=%u hitFlash(+68)=%u "
         "waitCount(+84)=%u extraSkip(+92)=%u screenMode(+108)=%u "
         "flags(+376)=0x%02X[16bit=%d nosound=%d vsync=%d] | "
         "active(match copy@623520) difficulty=%u frameLoopOwner=%u "
         "hitFlash=%u waitCount=%u extraSkip=%u screenMode=%u "
         "flags=0x%02X[16bit=%d nosound=%d vsync=%d] | "
         "screen_fx_state@623720=[%08X %08X %08X %08X %08X]",
         static_cast<unsigned>(cfg),
         reqDifficulty, reqFrameLoopOwner, reqHitFlash, reqWaitCount,
         reqExtraSkip, reqScreenMode, reqFlags & 0xFFu,
         (reqFlags & 1u) != 0, (reqFlags & 2u) != 0, (reqFlags & 4u) != 0,
         actDifficulty, actFrameLoopOwner, actHitFlash, actWaitCount,
         actExtraSkip, actScreenMode, actFlags & 0xFFu,
         (actFlags & 1u) != 0, (actFlags & 2u) != 0, (actFlags & 4u) != 0,
         fx0, fx1, fx2, fx3, fx4);
}

bool bindFrameDispatchThread() {
    const DWORD current = GetCurrentThreadId();
    if (g_clockThread == 0u) {
        g_clockThread = current;
        QLOG("[clock_shim] pinned real frame-dispatch thread=%lu",
             static_cast<unsigned long>(current));
    }
    return g_clockThread == current;
}

int __cdecl hkFrameDispatch() {
    if (!bindFrameDispatchThread()) {
        if (g_netActive) {
            netFailClosed(
                kNetInternalFailure, GetCurrentThreadId(),
                "frame dispatcher moved to another thread");
            return kNetHoldFrameRet;
        }
        return g_orig ? g_orig() : kNetHoldFrameRet;
    }
    netServiceTestPacingSummaryRequest();
    netAudioVolumeSweep();
    netRetryDeferredOverlayHud();
    netObserveLifecycleState();
    logMatchSettingsSnapshotOnRoundStart();
    g_frame++;
    g_dispatchN++;
    if (g_mode == RunMode::SelfTest) {
        trackMatchLifecycle();
        static int s_prevScene = -1;
        int scene = (int)rd32(kSceneStateAddr);
        if (scene != s_prevScene) { logFlowState(s_prevScene < 0 ? "first" : "scene-edge"); s_prevScene = scene; }
        else {
            static uint64_t s_lastFlowMs = 0;
            const uint64_t nowMs = GetTickCount64();
            if (s_lastFlowMs == 0 || nowMs - s_lastFlowMs >= 10000u) {
                s_lastFlowMs = nowMs;
                logFlowState("heartbeat");
            }
        }
    }
    if (g_mode != RunMode::Netplay && g_mode != RunMode::Spectator
        && g_mode != RunMode::Replay) {
        netOfflineFrameTick();
        qgame::directBootOfflineTick();
    }
    if (g_mode != RunMode::Replay) trainingOuterTick();
    else trainingReplayNativeHudTick();
    switch (g_mode) {
        case RunMode::SelfTest: return doSelfTest();
        case RunMode::Observe: {
            doObserve();
            if (g_offlineReturnToCharacterSelectPending) {
                g_offlineReturnToCharacterSelectPending = false;
                if (qgame::onlineReturnToCharacterSelectEligible(
                        rd32(kSceneStateAddr))) {
                    QLOG("[offline] F1 return taken; the match dispatcher "
                         "yields to character select");
                    return 0;
                }
                QLOG("[offline] F1 return dropped: the match had already "
                     "left scene 2 (scene=%u)", rd32(kSceneStateAddr));
            }
            const bool pacedGameplay =
                rd32(kSceneStateAddr) == 2u && rd32(kMatchSubAddr) == 0u;
            const uint32_t frameCounterBefore = rd32(0x4AB778u);
            const int result = g_orig();
            if (pacedGameplay
                && rd32(kSceneStateAddr) == 2u
                && rd32(kMatchSubAddr) == 0u
                && rd32(0x4AB778u) == frameCounterBefore) {
                offlinePollWaitBrief();
            }
            return result;
        }
        case RunMode::Netplay:  return doNetplay(g_orig);
        case RunMode::Spectator:return doSpectator(g_orig);
        case RunMode::Replay:   return doReplay(g_orig);
        default:                return g_orig();
    }
}

static FrameDispatchFn g_origScene1 = nullptr;
static FrameDispatchFn g_origScene3 = nullptr;
static uintptr_t g_scene1DispatchFn = 0;
static uintptr_t g_scene3DispatchFn = 0;
int hkScene1Dispatch() {
    if (!bindFrameDispatchThread()) {
        netFailClosed(
            kNetInternalFailure, GetCurrentThreadId(),
            "menu dispatcher moved to another thread");
        return kNetHoldFrameRet;
    }
    netServiceTestPacingSummaryRequest();
    netAudioVolumeSweep();
    netRetryDeferredOverlayHud();
    ++g_dispatchN;
    if (g_mode == RunMode::Netplay) return doNetplay(g_origScene1);
    if (g_mode == RunMode::Spectator) return doSpectator(g_origScene1);
    if (g_mode == RunMode::Replay) return doReplay(g_origScene1);
    if (g_mode == RunMode::Observe) netOfflineFrameTick();
    if (g_mode == RunMode::Observe || g_mode == RunMode::SelfTest)
        qgame::directBootOfflineTick();
    trainingOuterTick();
    return g_origScene1 ? g_origScene1() : 0;
}
int hkScene3Dispatch() {
    if (!bindFrameDispatchThread()) {
        netFailClosed(
            kNetInternalFailure, GetCurrentThreadId(),
            "character-select dispatcher moved to another thread");
        return kNetHoldFrameRet;
    }
    netServiceTestPacingSummaryRequest();
    netAudioVolumeSweep();
    netRetryDeferredOverlayHud();
    ++g_dispatchN;
    if (g_mode == RunMode::Netplay) return doNetplay(g_origScene3);
    if (g_mode == RunMode::Spectator) return doSpectator(g_origScene3);
    if (g_mode == RunMode::Replay) {
        trainingReplayNativeHudTick();
        return doReplay(g_origScene3);
    }
    if (g_mode == RunMode::Observe) netOfflineFrameTick();
    if (g_mode == RunMode::Observe || g_mode == RunMode::SelfTest)
        qgame::directBootOfflineTick();
    trainingOuterTick();
    return g_origScene3 ? g_origScene3() : 0;
}

void resetHookInstallState() {
    g_clockThread = 0;
    g_fCommandPolicyHooked = false;
    g_origFCommandParse = nullptr;
    netResetFCommandPolicySession();
    g_configSaveHooked = false;
    g_origConfigSave = nullptr;
    g_readHooked = false;
    g_pollHooked = false;
    g_paletteHooked = false;
    g_soundLoadHooked = false;
    g_soundDuplicatePlayHooked = false;
    g_soundDirectPlayHooked = false;
    g_soundStopHooked = false;
    g_soundStatusHooked = false;
    g_pacingHooked = false;
    g_clockHooked = false;
    g_paceBracketHooked = false;
    g_rngHooked = false;
    g_histHooked = false;
    g_b600Hooked = false;
    g_a0Hooked = false;
    g_scoreDrawHooked = false;
    g_timeDrawHooked  = false;
    g_origScoreDraw = nullptr;
    g_origTimeDraw  = nullptr;
}

void disarmNetplayState() {
    if (!g_netSpectatorArchiveSealed
        && ((g_spectatorServiceMode == 2u
             && qgame::spectator_publish::sidecarPublisherActive())
            || g_netSpectatorArchiveBegun)) {
        netSealSpectatorArchive(
            g_netHalted
                ? qnetplay::spectator::TerminalReason::ProtocolFailure
                : qnetplay::spectator::TerminalReason::SourceStopped);
    }
    if (g_spectatorServiceMode == 2u
        && qgame::spectator_publish::sidecarPublisherActive()) {
        qgame::spectator_publish::stopSidecarPublisher();
    }
    netRestoreOriginalConfigInMemory();
    g_netActive = false;
    g_netRunning = false;
    g_netSpectatorMode = false;
    g_netSession.reset();
    g_netPaceSlewPpm = 0;
    g_netPaceSlewIntegralPpm = 0;
    if (g_paceTicks > 0) g_paceDeadline.setFrameTicks(g_paceTicks);
    netResetPacingTelemetry();
    g_netLayoutBarrier = NetLayoutBarrierState{};
    g_netLayoutTestMutationGeneration = 0;
    g_netAutomatchTest = false;
    g_netAutomatchFrontendOnly = false;
    g_netAutomatchHarnessEnabled = false;
    InterlockedExchange(&g_netTestPauseRequest, 0);
    InterlockedExchange(&g_netTestPauseAcknowledged, 0);
    InterlockedExchange(&g_netTestPauseFrame, 0);
    InterlockedExchange(&g_netTestPauseHeldFrame, 0);
    g_netAutomatchWasGameplay = false;
    g_netAutomatchSegment = 0;
    g_netAutomatchSegmentStart = 0;
    g_netAutomatchRoundSeenMask = 0;
    g_netAutomatchLowLifeLoggedMask = 0;
    g_netAutomatchNativeKoLoggedMask = 0;
    g_netAutomatchNativeLatchLoggedMask = 0;
    g_netAutomatchForcedKoLoggedMask = 0;
    g_netTestDelayScriptEnabled = false;
    g_netTestDelayStepCount = 0;
    g_netTestDelayNextStep = 0;
    g_netTestDelayPendingTarget = UINT32_MAX;
    g_netDelayDrain.reset();
    g_netDirectBootInputGate.reset();
    g_netDirectBootLastBlockedInput = UINT32_MAX;
    netResetCharacterSelectTrace();
    g_netCanonicalPoll = false;
    g_replayActive = false;
    InterlockedExchange(
        &g_presentationDisposition,
        static_cast<LONG>(PresentationDisposition::Normal));
    resetCanonicalAudioSession();
    if (g_audioV2Enabled && g_audioOpsConfigured) {
        char audioLedger[768];
        qaudio::runtime().formatLedger(audioLedger, sizeof(audioLedger));
        QLOG("%s", audioLedger);
        qaudio::runtime().abortCorrection();
        qaudio::runtime().abortTick();
        qaudio::runtime().setDisposition(
            qaudio::AudioTickDisposition::ImmediateOffline);
    }
    netClockOff();
    InterlockedExchange(&g_netGateOwnsTick, 0);
    netPaceShutdown();
    offlinePollWaitShutdown();
    netReleaseWrapperPacingOwnership();
}

}

void setSessionAuthority(bool isHost) {
    g_sessionIsHost = isHost;
}

void setAudioVolumes(uint8_t musicVolume, uint8_t soundVolume) {
    g_musicVolume = musicVolume > 100 ? 100 : musicVolume;
    g_soundVolume = soundVolume > 100 ? 100 : soundVolume;
    QLOG("[audio] playback volumes music=%u sound=%u (buffer-level, "
         "presentation only)", g_musicVolume, g_soundVolume);
}

void setOfflineExtraInputConfig(uint32_t slot,
                                const qinput::InputLaunchConfig& config) {
    if (slot >= 3) return;
    g_offlineExtraConfigs[slot] = config;
    g_offlineExtraPresent[slot] = true;
    QLOG("[input-v2] offline player %u configuration accepted mode=%u "
         "probes=%u profileHash=%016llX",
         slot + 2u, static_cast<unsigned>(config.sourceMode),
         config.probeCount,
         static_cast<unsigned long long>(config.profileHash));
}

void setInputLaunchConfig(const qinput::InputLaunchConfig& config,
                          bool spectator) {
    g_inputLaunchConfig = config;
    g_inputConfigPresent = true;
    g_inputSpectatorRole = spectator;
    QLOG("[input-v2] launch configuration accepted mode=%u probes=%u "
         "spectator=%d profileHash=%016llX",
         static_cast<unsigned>(config.sourceMode), config.probeCount,
         spectator ? 1 : 0,
         static_cast<unsigned long long>(config.profileHash));
}

void setReplayTakeoverInputConfig(
        const qinput::InputLaunchConfig& config) {
    g_replayTakeoverInputConfig = config;
    g_replayTakeoverInputConfigPresent = true;
    QLOG("[replay.takeover] P1 input configuration accepted mode=%u probes=%u "
         "profileHash=%016llX",
         static_cast<unsigned>(config.sourceMode), config.probeCount,
         static_cast<unsigned long long>(config.profileHash));
}

void setModeControls(const qinput::CompiledModeControlsV1& controls) {
    g_modeControls = controls;
    g_modeControlEvaluator.configure(&g_modeControls);
    g_modeControlEvaluation = qinput::ModeControlEvaluation{};
    g_modeControlEvaluationIteration = ~0ull;
    QLOG("[mode-controls] schema=2 runtime actions=%u",
         static_cast<unsigned>(g_modeControls.count));
}

PresentationDisposition currentPresentationDisposition() {
    const LONG raw = InterlockedCompareExchange(
        &g_presentationDisposition, 0, 0);
    if (raw == static_cast<LONG>(
                   PresentationDisposition::SuppressHistorical))
        return PresentationDisposition::SuppressHistorical;
    // The wrapper hard-compares this value to 1, so HiddenNewForward must be exported as SuppressHistorical or the frame renders as fully visible.
    if (raw == static_cast<LONG>(
                   PresentationDisposition::HiddenNewForward))
        return PresentationDisposition::SuppressHistorical;
    if (raw == static_cast<LONG>(
                   PresentationDisposition::CorrectedFinal))
        return PresentationDisposition::CorrectedFinal;
    return PresentationDisposition::Normal;
}

static bool networkPacingOwner() {
    return g_netActive
        && (g_mode == RunMode::Netplay
            || g_mode == RunMode::Spectator);
}

bool ownsNetplayPacing() {
    const bool visibleReplayOwner = g_replayRoleActive
        && g_mode == RunMode::Replay
        && !qgame::replay::replayHeadlessV3();
    return (networkPacingOwner() || visibleReplayOwner)
        && g_wrapperPacingOwned;
}

bool ownsNetworkPacing() {
    return networkPacingOwner() && g_wrapperPacingOwned;
}

bool presentationClockStarted() {
    if (g_replayRoleActive) {
        return !qgame::replay::replayHeadlessV3()
            && g_replayBootstrapPhase
                == ReplayBootstrapPhase::Complete;
    }
    return qgame::deterministicPresentationClockStarted(
        g_netRunning, g_netSpectatorMode,
        g_spectatorPlaybackStarted);
}

bool tryAcquirePresentationSlot() {
    if (!ownsNetplayPacing())
        return true;
    if (!presentationClockStarted())
        return true;
    const LONG callbackCount =
        InterlockedIncrement(&g_netPresentationWaitCallbacks);
    if (callbackCount == 1) {
        QLOG("[pacing-owner] canonical presentation-slot callback active");
    }
    if (InterlockedCompareExchange(
            &g_netGateOwnsTick, 0, 0) == 0) {
        const LONG rejected =
            InterlockedIncrement(&g_netUnownedCommitCallbacks);
        netLatchPacingClockFault();
        if (rejected <= 8 || (rejected % 300) == 0) {
            QLOG_ISSUE("[pacing-owner] FAILED: canonical presentation-slot "
                 "callback rejected outside an owned frame-dispatch tick "
                 "count=%ld", rejected);
        }
        return false;
    }
    if (g_clockThread == 0u
        || GetCurrentThreadId() != g_clockThread) {
        netLatchPacingClockFault();
        QLOG_ISSUE("[pacing-owner] FAILED: presentation commit moved off "
             "frame-dispatch thread expected=%lu actual=%lu",
             static_cast<unsigned long>(g_clockThread),
             static_cast<unsigned long>(GetCurrentThreadId()));
        return false;
    }

    if (currentPresentationDisposition()
        == PresentationDisposition::SuppressHistorical) {
        InterlockedIncrement(&g_netHiddenCommitBypasses);
        return false;
    }
    const LONG ownedTickSerial = InterlockedCompareExchange(
        &g_netOwnedTickSerial, 0, 0);
    if (ownedTickSerial != 0
        && InterlockedCompareExchange(
               &g_netPacedTickSerial, 0, 0)
            == ownedTickSerial) {
        InterlockedIncrement(&g_netDuplicateCommitCallbacks);
        return false;
    }
    const LONG grantsBefore = InterlockedCompareExchange(
        &g_netPresentationSlotGrants, 0, 0);
    const bool acquired =
        netConsumePresentationSlot(ownedTickSerial);
    if (InterlockedCompareExchange(
            &g_netPresentationSlotGrants, 0, 0)
        > grantsBefore) {
        InterlockedIncrement(&g_netCommitBoundaryGrants);
        return acquired;
    }
    return false;
}

PresentationDisposition queryPresentationDisposition() {
    InterlockedIncrement(&g_presentationDispositionQueries);
    return currentPresentationDisposition();
}

void observePresentationCommit() {
    if (g_replayRoleActive) {
        if (currentPresentationDisposition()
                != PresentationDisposition::SuppressHistorical
            && InterlockedCompareExchange(
                   &g_netGateOwnsTick, 0, 0) != 0) {
            const LONG serial = InterlockedCompareExchange(
                &g_netOwnedTickSerial, 0, 0);
            if (serial != 0)
                InterlockedExchange(
                    &g_replayPresentedTickSerial, serial);
            if (ownsNetplayPacing() && presentationClockStarted()) {
                const int64_t nowTicks = netUnhookedQpcNow();
                if (nowTicks > 0)
                    g_paceDeadline.observePresentationCompleted(nowTicks);
                else
                    netLatchPacingClockFault();
            }
        }
        return;
    }
    if (!g_netActive)
        return;
    if (!presentationClockStarted()) {
        if (!ownsNetplayPacing())
            return;
        const ScopedPacingConsumer consumer;
        if (!consumer)
            return;
        const int64_t nowTicks = netUnhookedQpcNow();
        if (nowTicks <= 0) {
            netLatchPacingClockFault();
            QLOG_ISSUE("[pacing-owner] FAILED: pre-clock presentation timer "
                 "unavailable");
            return;
        }
        g_paceDeadline.anchorPresentationCompleted(nowTicks);
        static volatile LONG preClockAnchorLogged = 0;
        if (InterlockedCompareExchange(
                &preClockAnchorLogged, 1, 0) == 0) {
            QLOG("[pacing-owner] pre-GO/pre-playback presentation "
                 "anchor active");
        }
        return;
    }
    if (currentPresentationDisposition()
        == PresentationDisposition::SuppressHistorical)
        return;
    const ScopedPacingConsumer consumer;
    if (!consumer)
        return;
    if (g_clockThread == 0u
        || GetCurrentThreadId() != g_clockThread
        || InterlockedCompareExchange(
               &g_netGateOwnsTick, 0, 0) == 0) {
        netLatchPacingClockFault();
        QLOG_ISSUE("[pacing-owner] FAILED: post-publish callback moved "
             "outside the owned frame-dispatch tick");
        return;
    }
    const LONG ownedTickSerial = InterlockedCompareExchange(
        &g_netOwnedTickSerial, 0, 0);
    if (ownedTickSerial == 0
        || InterlockedCompareExchange(
               &g_netPacedTickSerial, 0, 0) != ownedTickSerial) {
        netLatchPacingClockFault();
        QLOG_ISSUE("[pacing-owner] FAILED: frame published without its "
             "canonical presentation slot");
        return;
    }
    if (InterlockedCompareExchange(
            &g_netCompletedTickSerial, 0, 0) == ownedTickSerial) {
        InterlockedIncrement(&g_netDuplicatePublishCallbacks);
        netLatchPacingClockFault();
        QLOG_ISSUE("[pacing-owner] FAILED: owned tick completed more than once "
             "serial=%ld", ownedTickSerial);
        return;
    }

    const int64_t nowTicks = netUnhookedQpcNow();
    if (nowTicks <= 0) {
        netLatchPacingClockFault();
        QLOG_ISSUE("[pacing-owner] FAILED: post-publish wall clock unavailable");
        return;
    }
    InterlockedExchange(
        &g_netCompletedTickSerial, ownedTickSerial);
    InterlockedExchange(
        &g_netPublishedTickSerial, ownedTickSerial);
    ++g_netPresentedThisWindow;
    const LONG publishCount =
        InterlockedIncrement(&g_netPresentationCommitCallbacks);
    qgame::netPublishGameThreadHeartbeat(
        qgame::NetTransportDiagnostics::GameThreadStage::
            PresentationCommit,
        static_cast<uint32_t>(g_ovrFrame));
    if (publishCount == 1) {
        QLOG("[pacing-owner] successful canonical post-publish callback "
             "active");
    }
    g_paceDeadline.observePresentationCompleted(nowTicks);
    const uint64_t nowUs = netQpcTicksToUs(nowTicks);
    g_netPresentationPacingWindow.observe(
        nowUs, netRollbackGameplayPolicyActive(), true);
    if (!g_netPresentationPacingWindow.reportDue(nowUs))
        return;

    const qgame::NetplayPacingReport report =
        g_netPresentationPacingWindow.finish(nowUs);
    QLOG("[pacing-present] frame=%u elapsedUs=%llu visible=%u "
         "visibleFps=%.2f gameplay=%u intervalUs=%u/%u/%u "
         "burst=%u late=%u",
         static_cast<uint32_t>(g_ovrFrame),
         static_cast<unsigned long long>(report.elapsedUs),
         report.visibleTicks, report.visibleFps(),
         report.gameplayTicks,
         report.minimumIntervalUs, report.p95IntervalUs,
         report.maximumIntervalUs, report.burstIntervals,
         report.lateIntervals);
}

void observePresentationFailure(const unsigned long reason) {
    if ((!g_netActive && !g_replayRoleActive)
        || !presentationClockStarted())
        return;
    const ScopedPacingConsumer consumer;
    if (!consumer)
        return;
    if (g_clockThread == 0u
        || GetCurrentThreadId() != g_clockThread
        || InterlockedCompareExchange(
               &g_netGateOwnsTick, 0, 0) == 0
        || currentPresentationDisposition()
            == PresentationDisposition::SuppressHistorical) {
        netLatchPacingClockFault();
        QLOG_ISSUE("[pacing-owner] FAILED: publish-failure callback moved "
             "outside a visible owned frame-dispatch tick");
        return;
    }
    const LONG ownedTickSerial = InterlockedCompareExchange(
        &g_netOwnedTickSerial, 0, 0);
    if (ownedTickSerial == 0
        || InterlockedCompareExchange(
               &g_netPacedTickSerial, 0, 0) != ownedTickSerial) {
        netLatchPacingClockFault();
        QLOG_ISSUE("[pacing-owner] FAILED: renderer failure reported without "
             "its canonical presentation slot");
        return;
    }
    if (InterlockedCompareExchange(
            &g_netCompletedTickSerial, 0, 0) == ownedTickSerial) {
        InterlockedIncrement(&g_netDuplicatePublishCallbacks);
        netLatchPacingClockFault();
        QLOG_ISSUE("[pacing-owner] FAILED: owned tick completion reported more "
             "than once serial=%ld", ownedTickSerial);
        return;
    }

    InterlockedExchange(
        &g_netCompletedTickSerial, ownedTickSerial);
    const unsigned long reasonIndex =
        reason <= 6u ? reason : 0u;
    InterlockedIncrement(
        &g_netPresentationFailureReasons[reasonIndex]);
    const LONG failureCount =
        InterlockedIncrement(&g_netPresentationFailureCallbacks);
    if (failureCount <= 8 || (failureCount % 300) == 0) {
        QLOG("[pacing-owner] canonical physical publish unavailable "
             "count=%ld serial=%ld reason=%lu",
             failureCount, ownedTickSerial, reasonIndex);
    }
}

unsigned long presentationDispositionQueryCount() {
    return static_cast<unsigned long>(
        InterlockedCompareExchange(
            &g_presentationDispositionQueries, 0, 0));
}

void reportWrapperPresentationCounters(
    unsigned long normalVisible,
    unsigned long suppressed,
    unsigned long correctedFinal) {
    InterlockedExchange(
        &g_wrapperNormalVisiblePresents,
        static_cast<LONG>(normalVisible));
    InterlockedExchange(
        &g_wrapperSuppressedPresents,
        static_cast<LONG>(suppressed));
    InterlockedExchange(
        &g_wrapperCorrectedFinalPresents,
        static_cast<LONG>(correctedFinal));
    const unsigned long total =
        normalVisible + suppressed + correctedFinal;
    if (total <= 8u || (total % 300u) == 0u) {
        QLOG("[presentation-wrapper] normalVisiblePresents=%lu "
             "suppressedPresents=%lu correctedFinalPresents=%lu "
             "dispositionQueries=%lu",
             normalVisible, suppressed, correctedFinal,
             presentationDispositionQueryCount());
    }
}

void armNetplay(int localPlayer, int delay, int rollback,
                int remoteDelay, int remoteRollback,
                const qnetplay::config_provenance::ConfigurationProvenanceWire&
                    configurationProvenance,
                bool serveSpectators,
                const char* localName, unsigned long localNameCodePage,
                const char* remoteName, unsigned long remoteNameCodePage) {
    netRestoreOriginalConfigInMemory();
    netResetFCommandPolicySession();
    g_hiddenReplayNativeTicks = 0;
    g_hiddenForwardNativeTicks = 0;
    g_netCadenceDebt.reset();
    g_netLastHoldCause = qgame::HoldCause::None;
    g_netRunState = qgame::NetplayRunState::Running;
    g_netHoldLedger.reset();
    g_netRunStateEnteredUs = 0;
    g_netRunStateSampledUs = 0;
    g_netRunStateTransitions = 0;
    for (uint64_t& slot : g_netRunStateUs) slot = 0;
    g_netRunStateDiscontinuities = 0;
    g_netPacingFrameAdvantageHolds = 0;
    g_netPacingLifecycleHolds = 0;
    g_netCatchupBatches = 0;
    g_netFaHoldCooldown = 0;
    g_netPaceSlewPpm = 0;
    g_netPaceSlewIntegralPpm = 0;
    g_netStallDeadlineFrame = 0;
    g_netStallDeadlineSinceUs = 0;
    g_netStallDeadlineWarned = false;
    g_netStarvedOpportunities = 0;
    g_netPresentedThisWindow = 0;
    g_netPaceSlewCountdown = 0;
    g_netPaceSlewAdjustments = 0;
    g_netPaceSlewPeakPpm = 0;
    g_netRollbackBalanceGameplayFrames = 0;
    g_netRollbackBalanceLastFrame = 0;
    g_netRollbackBalanceHasLastFrame = false;
    g_netRollbackBalanceLastReason =
        qgame::RollbackBalanceReason::OutsideRollbackGameplay;
    g_netRollbackBalanceHasLastReason = false;
    if (g_paceTicks > 0) g_paceDeadline.setFrameTicks(g_paceTicks);
    g_correctedFinalNativeTicks = 0;
    g_verifiedHiddenPresentationTicks = 0;
    g_missingPresentationGateTicks = 0;
    InterlockedExchange(&g_presentationDispositionQueries, 0);
    g_netActive    = true;
    g_netRunning   = false;
    g_netSpectatorMode = false;
    g_netLocalCtrl = (localPlayer == 2) ? 1 : 0;
    g_netPlayerNameCodePages[g_netLocalCtrl] =
        static_cast<uint32_t>(localNameCodePage);
    g_netPlayerNameCodePages[g_netLocalCtrl ^ 1] =
        static_cast<uint32_t>(remoteNameCodePage);
    qgame::storeCanonicalPlayerNameForCodePage(
        g_netPlayerNames[g_netLocalCtrl],
        localName ? localName : "Player",
        g_netPlayerNameCodePages[g_netLocalCtrl]);
    qgame::storeCanonicalPlayerNameForCodePage(
        g_netPlayerNames[g_netLocalCtrl ^ 1],
        remoteName ? remoteName : "Player",
        g_netPlayerNameCodePages[g_netLocalCtrl ^ 1]);
    netConfigureAutomatchTest();
    netConfigureTestDelayScript();
    constexpr int kDelayCap = static_cast<int>(qgame::kMaximumGameplayDelay);
    constexpr int kRollbackCap =
        static_cast<int>(qgame::kMaximumGameplayRollback);
    g_netDelay     = (delay < 0) ? 0
                    : (delay > kDelayCap ? (uint32_t)kDelayCap
                                         : (uint32_t)delay);
    g_netRollback  = (rollback < 0) ? 0
                    : (rollback > kRollbackCap ? (uint32_t)kRollbackCap
                                               : (uint32_t)rollback);
    g_netRemoteDelay = (remoteDelay < 0) ? 0
                       : (remoteDelay > kDelayCap ? (uint32_t)kDelayCap
                                                  : (uint32_t)remoteDelay);
    g_netRemoteRollback = (remoteRollback < 0) ? 0
                          : (remoteRollback > kRollbackCap
                                 ? (uint32_t)kRollbackCap
                                 : (uint32_t)remoteRollback);
    g_netFrontendDelay = (std::max)(1u, g_netDelay);
    g_netFrontendDelayLatch = qgame::FrontendDelayLatch{};
    g_netFrontendDelayLatch.frames = g_netFrontendDelay;
    g_netDeferredDelayTarget = UINT32_MAX;
    g_netDeferredDelayLastLogMs = 0;
    g_netGameplayDelayApplied = g_netDelay;
    g_netDelayHotkeys.reset();
    g_netDelayDrain.reset();
    g_netDirectBootInputGate.reset();
    g_netDirectBootLastBlockedInput = UINT32_MAX;
    netResetCharacterSelectTrace();
    qgame::netSetLocalGameplayDelayStatus(
        static_cast<uint8_t>(g_netDelay),
        static_cast<uint8_t>(g_netGameplayDelayApplied),
        false);
    QLOG("[config-provenance] v=1 role=runtime stage=%s committed=1 "
         "local_delay=%u local_rollback=%u "
         "local_delay_origin=%s local_rollback_origin=%s "
         "peer_delay=%u peer_rollback=%u "
         "peer_delay_origin=%s peer_rollback_origin=%s "
         "local_runtime_applied=1 peer_runtime_applied=0 "
         "peer_runtime_attestation=%s",
         qnetplay::config_provenance::stageName(
             qnetplay::config_provenance::Stage::RuntimeApplied),
         g_netDelay, g_netRollback,
         qnetplay::config_provenance::stageName(
             static_cast<qnetplay::config_provenance::Stage>(
                 configurationProvenance.localDelayOrigin)),
         qnetplay::config_provenance::stageName(
             static_cast<qnetplay::config_provenance::Stage>(
                 configurationProvenance.localRollbackOrigin)),
         g_netRemoteDelay, g_netRemoteRollback,
         qnetplay::config_provenance::stageName(
             static_cast<qnetplay::config_provenance::Stage>(
                 configurationProvenance.peerDelayOrigin)),
         qnetplay::config_provenance::stageName(
             static_cast<qnetplay::config_provenance::Stage>(
                 configurationProvenance.peerRollbackOrigin)),
         qnetplay::config_provenance::stageName(
             static_cast<qnetplay::config_provenance::Stage>(
                 configurationProvenance.peerRuntimeAttestation)));
    g_netServeSpectators = serveSpectators;
    g_netSpectatorArchiveBegun = false;
    g_netSpectatorArchiveFailed = false;
    g_netSpectatorArchiveSealed = false;
    g_netSpectatorGameId = 1;
    g_netSpectatorRecord = 0;
    g_netSpectatorPostTailHash = 0;
    g_spectatorSidecarRecord = 0;
    g_spectatorSidecarDropLogged = false;
    g_netLifecycleLoggedScene = 0xFFFFFFFFu;
    g_netLifecycleLoggedNext = 0xFFFFFFFFu;
    g_netLifecycleLoggedMatchSub = INT32_MIN;
    g_netLifecycleLoggedScene3Sub = INT32_MIN;
    g_netFrame     = 0;
    g_netInject[0] = g_netInject[1] = 0;
    g_netSession.reset();
    netResetPacingTelemetry();
    g_netEpoch = 1;
    netPaletteResetForSession();
    g_netBootstrapTransientsEpoch = 0;
    g_netRemoteIngest = 0;
    g_netTransitionBlocked = qgame::kNetNoFrame;
    g_netHalted = false;
    g_netHaltReason[0] = '\0';
    g_netPendingPeerHashes.clear();
    g_netHasConfirmed = false;
    g_netConfirmedThrough = 0;
    g_netConfirmedHashMatches = 0;
    g_netGameplaySegmentActive = false;
    g_netGameplaySegmentSeen = false;
    g_netGameplayRefreshPending = false;
    g_netLayoutBarrier = NetLayoutBarrierState{};
    g_netLayoutTestMutationGeneration = 0;
    g_netReadySnapped = false;
    g_netLocalReady = qgame::NetReadyState{};
    g_netFpuInitializedEpoch = 0;
    netInvalidateFpu();
    resetCanonicalAudioSession();
    if (g_audioV2Enabled) {
        qaudio::runtime().resetSession();
        qaudio::runtime().setMuted(g_audioV2Muted);
        qaudio::runtime().setHorizons(g_audioV2Horizons);
    }
    netResetSessionEventDriver();
    for (NetFrameMeta& meta : g_netMeta) meta = NetFrameMeta{};
    g_ovrEnabled   = true;
    netPaceInit();
    QLOG("[netplay] armed: %s(P1) vs %s(P2), local=P%d delay=%u, "
         "local rollback=%u "
         "peer delay=%u peer rollback=%u spectators=%d "
         "(both slots served by our input wrap); "
         "pacing owned here: %lld QPC ticks/frame "
         "(%.2f fps target, timer1ms=%u)",
         g_netPlayerNames[0], g_netPlayerNames[1],
         g_netLocalCtrl + 1, g_netDelay, g_netRollback,
         g_netRemoteDelay, g_netRemoteRollback,
         static_cast<int>(g_netServeSpectators),
         (long long)g_paceTicks,
         g_paceTicks ? (double)g_qpcFreq / (double)g_paceTicks : 0.0,
         g_timerPeriodActive ? 1u : 0u);
}

void setSpectatorWantsLatest(bool enabled) {
    g_spectatorWantsLatest = enabled;
    g_spectatorSkip = SpectatorSkipState::Off;
    if (enabled)
        QLOG("[set-seek] --latest requested: will join at the source's "
             "newest checkpoint when one arrives");
}

void armSpectator() {
    netRestoreOriginalConfigInMemory();
    netResetFCommandPolicySession();
    g_hiddenReplayNativeTicks = 0;
    g_hiddenForwardNativeTicks = 0;
    g_netCadenceDebt.reset();
    g_netLastHoldCause = qgame::HoldCause::None;
    g_netRunState = qgame::NetplayRunState::Running;
    g_netHoldLedger.reset();
    g_netRunStateEnteredUs = 0;
    g_netRunStateSampledUs = 0;
    g_netRunStateTransitions = 0;
    for (uint64_t& slot : g_netRunStateUs) slot = 0;
    g_netRunStateDiscontinuities = 0;
    g_netPacingFrameAdvantageHolds = 0;
    g_netPacingLifecycleHolds = 0;
    g_netCatchupBatches = 0;
    g_netFaHoldCooldown = 0;
    g_netPaceSlewPpm = 0;
    g_netPaceSlewIntegralPpm = 0;
    g_netStallDeadlineFrame = 0;
    g_netStallDeadlineSinceUs = 0;
    g_netStallDeadlineWarned = false;
    g_netStarvedOpportunities = 0;
    g_netPresentedThisWindow = 0;
    g_netPaceSlewCountdown = 0;
    g_netPaceSlewAdjustments = 0;
    g_netPaceSlewPeakPpm = 0;
    g_netRollbackBalanceGameplayFrames = 0;
    g_netRollbackBalanceLastFrame = 0;
    g_netRollbackBalanceHasLastFrame = false;
    g_netRollbackBalanceLastReason =
        qgame::RollbackBalanceReason::OutsideRollbackGameplay;
    g_netRollbackBalanceHasLastReason = false;
    if (g_paceTicks > 0) g_paceDeadline.setFrameTicks(g_paceTicks);
    g_correctedFinalNativeTicks = 0;
    g_verifiedHiddenPresentationTicks = 0;
    g_missingPresentationGateTicks = 0;
    InterlockedExchange(&g_presentationDispositionQueries, 0);
    g_netActive = true;
    g_netRunning = true;
    g_netSpectatorMode = true;
    g_netLocalCtrl = 0;
    netConfigureAutomatchTest();
    g_netTestDelayScriptEnabled = false;
    g_netTestDelayStepCount = 0;
    g_netTestDelayNextStep = 0;
    g_netTestDelayPendingTarget = UINT32_MAX;
    g_netDelay = g_netRollback = 0;
    g_netRemoteDelay = g_netRemoteRollback = 0;
    g_netPlayerNameCodePages[0] = GetACP();
    g_netPlayerNameCodePages[1] = GetACP();
    qgame::storeCanonicalPlayerName(g_netPlayerNames[0], "Player 1");
    qgame::storeCanonicalPlayerName(g_netPlayerNames[1], "Player 2");
    g_netDelayHotkeys.reset();
    g_netDelayDrain.reset();
    g_netDirectBootInputGate.reset();
    g_netDirectBootLastBlockedInput = UINT32_MAX;
    netResetCharacterSelectTrace();
    g_netServeSpectators = false;
    g_netFrame = 0;
    g_netInject[0] = g_netInject[1] = 0;
    g_netSession.reset();
    netResetPacingTelemetry();
    g_netEpoch = 1;
    netPaletteResetForSession();
    g_netBootstrapTransientsEpoch = 0;
    g_netHalted = false;
    g_netHaltReason[0] = '\0';
    g_netGameplaySegmentActive = false;
    g_netGameplaySegmentSeen = false;
    g_netGameplayRefreshPending = false;
    g_netLayoutBarrier = NetLayoutBarrierState{};
    g_netLayoutTestMutationGeneration = 0;
    g_netFpuInitializedEpoch = 0;
    netInvalidateFpu();
    resetCanonicalAudioSession();
    if (g_audioV2Enabled) {
        qaudio::runtime().resetSession();
        qaudio::runtime().setMuted(g_audioV2Muted);
        qaudio::runtime().setHorizons(g_audioV2Horizons);
    }
    for (NetFrameMeta& meta : g_netMeta) meta = NetFrameMeta{};
    g_spectatorStartReceived = false;
    g_spectatorPlaybackStarted = false;
    g_spectatorBufferPrimed = false;
    g_spectatorHasPrimed = false;
    g_spectatorPacingSummaryLogged = false;
    g_spectatorGameStart =
        qnetplay::spectator::GameStartPayload{};
    g_spectatorTimelineFrame = 0;
    g_spectatorLastResumeMs = 0;
    g_spectatorArmMs = GetTickCount64();
    g_spectatorStartReceivedMs = 0;
    g_spectatorGameplayEntryStartedMs = 0;
    g_spectatorGameplayEntryLastLogMs = 0;
    g_spectatorLinkLostSinceMs = 0;
    g_spectatorCatchingUp = false;
    g_spectatorCatchupStartedMs = 0;
    g_spectatorUnderruns = 0;
    g_spectatorCatchupSlicesTruncated = 0;
    g_spectatorLastCatchupLogMs = 0;
    g_spectatorElasticCredit = 0;
    g_spectatorElasticHolds = 0;
    g_spectatorLastElasticLogMs = 0;
    spectatorTelemetryReset();
    qgame::spectatorPublishPlaybackPresentation(false, false, false);
    g_spectatorLocalPaused = false;
    g_spectatorFastForwardHeld = false;
    g_spectatorFastForwardLatched = false;
    g_spectatorPendingSteps = 0;
    g_spectatorSpeedPermille = qreplay::kSpeedNormal;
    g_spectatorCatchupSpeedPermille = 1000u;
    g_spectatorSeekMatchRequest = 0;
    g_spectatorSeekLatestRequested = false;
    InterlockedExchange(&g_spectatorCommandHead, 0);
    InterlockedExchange(&g_spectatorCommandTail, 0);
    InterlockedExchange(&g_spectatorCommandDrops, 0);
    g_ovrEnabled = true;
    netPaceInit();
    QLOG("[spectator] armed receive-only confirmed playback timer1ms=%u "
         "liveTarget=%zu catchupEnter=%zu sliceBudget=%lluus",
         g_timerPeriodActive ? 1u : 0u,
         qgame::kSpectatorLiveTargetRecords,
         qgame::kSpectatorCatchupEnterRecords,
         static_cast<unsigned long long>(
             qgame::kSpectatorCatchupSliceMicros));
}

static void clearProfileHookTargets() {
    g_nativeDamageFn = 0;
    g_nativeReactionGateFn = 0;
    g_nativeHudScoreDrawFn = 0;
    g_nativeHudTimeDrawFn = 0;
    g_readLogicalInputFn = 0;
    g_rngDrawFn = 0;
    g_frameDispatchFn = 0;
    g_fullInputPollFn = 0;
    g_nativeKeyboardCallbackFn = 0;
    g_nativePresenceProbeFn = 0;
    g_configSaveFn = 0;
    g_scene1DispatchFn = 0;
    g_scene3DispatchFn = 0;
}

static bool resolveProfileHookTargets(std::string& error) {
    using FunctionId = generated_model::FunctionId;
    clearProfileHookTargets();
    const auto resolve = [&error](FunctionId id, uintptr_t& target,
                                  const char* label) {
        std::string detail;
        if (runtime_profile::resolveSelectedFunction(id, target, detail))
            return true;
        error = std::string("profile target '") + label + "' was not resolved: "
            + detail;
        return false;
    };
    return resolve(FunctionId::NativeDamage, g_nativeDamageFn, "native damage")
        && resolve(FunctionId::NativeReactionGate, g_nativeReactionGateFn,
                   "native reaction gate")
        && resolve(FunctionId::HudScoreDraw, g_nativeHudScoreDrawFn,
                   "HUD score draw")
        && resolve(FunctionId::HudTimeDraw, g_nativeHudTimeDrawFn,
                   "HUD time draw")
        && resolve(FunctionId::ReadLogicalInput, g_readLogicalInputFn,
                   "logical input read")
        && resolve(FunctionId::RngDraw, g_rngDrawFn, "RNG draw")
        && resolve(FunctionId::SceneMatchDispatch, g_frameDispatchFn,
                   "match dispatcher")
        && resolve(FunctionId::PollAllInputs, g_fullInputPollFn,
                   "full input poll")
        && resolve(FunctionId::NativeKeyboardCallback,
                   g_nativeKeyboardCallbackFn, "native keyboard callback")
        && resolve(FunctionId::NativePresenceProbe, g_nativePresenceProbeFn,
                   "native presence probe")
        && resolve(FunctionId::ConfigSave, g_configSaveFn, "config save")
        && resolve(FunctionId::SceneTitleMenu, g_scene1DispatchFn,
                   "title dispatcher")
        && resolve(FunctionId::SceneVsSetup, g_scene3DispatchFn,
                   "VS dispatcher");
}

static bool netVerifyHostImageOnce() {
    static bool s_verified = false;
    static bool s_result = false;
    if (s_verified) return s_result;
    s_verified = true;

    runtime_profile::clearSelectedProfile();
    clearProfileHookTargets();

    std::string error;
    const std::wstring hostPathW = qplat::exePathW();
    const std::string hostPath = qplat::narrow(hostPathW);
    qclient::RuntimeInstallationIdentity installation{};
    if (hostPath.empty()
        || !qclient::validateRuntimeInstallationForAdmission(
            hostPath, &installation, error)) {
        QLOG("[host-image] REJECTED executable/content admission: %s",
             error.empty() ? "could not determine executable path"
                           : error.c_str());
        runtime_profile::rejectSelectedProfile();
        return false;
    }
    const qclient::ExecutableIdentity& fileIdentity = installation.executable;

    void* host = reinterpret_cast<void*>(GetModuleHandleW(nullptr));
    qplat::PeImageInfo image{};
    if (!qplat::validateHostImage(host, image, error)) {
        QLOG("[host-image] REJECTED: %s (base=%p)", error.c_str(), host);
        runtime_profile::rejectSelectedProfile();
        return false;
    }
    const generated_model::BuildProfile* observed =
        runtime_profile::findProfile(fileIdentity.runtimeProfileId);
    const bool observedQualified =
        observed != nullptr
        && runtime_profile::profileAllowsHookInstall(*observed);
    const bool baseOk = image.base == 0x400000u;
    const bool machineOk = image.machine == 0x014Cu;
    const bool kindOk = !image.isDll;
    const bool profileKnown = observed != nullptr;
    const bool imageMatchesExactFile = profileKnown
        && image.machine == fileIdentity.machine;
    const bool sizeOk = image.sizeOfImage >= 0x00080000u
        && image.sizeOfImage <= 0x02000000u;
    const bool entryOk = image.entryPointRva != 0
        && image.entryPointRva < image.sizeOfImage;
    QLOG("[host-image] base=%08X sizeOfImage=%08X machine=%04X dll=%d "
         "subsystem=%u timestamp=%08X entryRva=%08X checksum=%08X "
         "detectedProfile=%s profileQualified=%d exactFileIdentity=%d",
         static_cast<unsigned>(image.base), image.sizeOfImage, image.machine,
         image.isDll ? 1 : 0, image.subsystem, image.timestamp,
         image.entryPointRva, image.checkSum,
         observed ? observed->runtimeProfileKey : "<unknown>",
         observedQualified ? 1 : 0, imageMatchesExactFile ? 1 : 0);
    if (!baseOk || !machineOk || !kindOk || !profileKnown || !sizeOk
        || !entryOk || !imageMatchesExactFile) {
        QLOG("[host-image] REJECTED: base=%d machine=%d exe=%d profile=%d "
             "size=%d entry=%d exactFile=%d - refusing to arm transport, input, or any "
             "fixed-address patch",
             baseOk ? 1 : 0, machineOk ? 1 : 0, kindOk ? 1 : 0,
             profileKnown ? 1 : 0, sizeOk ? 1 : 0, entryOk ? 1 : 0,
             imageMatchesExactFile ? 1 : 0);
        runtime_profile::rejectSelectedProfile();
        return false;
    }
    if (!runtime_profile::selectProfileForExactExecutableEvidence(
            fileIdentity.sha256.c_str(), fileIdentity.fileSize,
            fileIdentity.machine, error)) {
        QLOG("[host-image] REJECTED: %s", error.c_str());
        QLOG("[host-image] refusal detail: %s",
             runtime_profile::unsupportedRuntimeProfileDiagnostic(*observed)
                 .c_str());
        return false;
    }
    if (!resolveProfileHookTargets(error)) {
        QLOG("[host-image] REJECTED selected profile '%s': %s",
             runtime_profile::selectedProfileKey(), error.c_str());
        runtime_profile::rejectSelectedProfile();
        clearProfileHookTargets();
        return false;
    }
    const qplat::CompatInfo& compat = qplat::detectCompat();
    QLOG("[host-image] accepted profile '%s' (%s; content policy=%s) (compat: wine=%d "
         "version='%s' buildId='%s' hostOs='%s' proton=%d appId='%s' "
         "steamOverlay=%d steamInput=%d)",
         observed->runtimeProfileKey, observed->displayName,
         observed->simulationContentManifestPolicy, compat.wine ? 1 : 0,
         compat.wineVersion.c_str(), compat.wineBuildId.c_str(),
         compat.wineHostOs.c_str(), compat.proton ? 1 : 0,
         compat.protonAppId.c_str(), compat.steamOverlayPresent ? 1 : 0,
         compat.steamInputPresent ? 1 : 0);
    s_result = true;
    return true;
}

static void rollbackHookTransaction(const char* why) {
    QLOG("[hooks] rolling back hook transaction: %s", why);
    const MH_STATUS removed = MH_Uninitialize();
    restoreQohImportPatches();
    g_orig = nullptr;
    g_origScene1 = nullptr;
    g_origScene3 = nullptr;
    resetHookInstallState();
    QLOG("[hooks] rollback complete (MH_Uninitialize=%d, all IAT slots "
         "restored) - the game resumes on a byte-identical image",
         static_cast<int>(removed));
}

bool installHooks(RunMode mode) {
    if (mode == RunMode::Off) { QLOG("[hooks] mode=Off, not installing"); return true; }
    const bool deterministicNetworkMode =
        mode == RunMode::Netplay || mode == RunMode::Spectator;
    const bool replayMode = mode == RunMode::Replay;
    const bool deterministicTimelineMode =
        deterministicNetworkMode || replayMode;
    const bool explicitTrainingMode =
        mode == RunMode::Observe && g_trainingLaunchRequested;
    const bool selfTestOrProbeMode =
        mode == RunMode::SelfTest || g_unattendedHarnessLaunch;
    const qgame::NativeDeveloperKeyPolicy nativeDeveloperPolicy =
        qgame::nativeDeveloperKeyPolicy(
            selfTestOrProbeMode
                ? qgame::NativeDeveloperRuntimeMode::SelfTestOrProbe
                : qgame::NativeDeveloperRuntimeMode::OfflineInteractive,
            g_nativeDeveloperKeysAllowedByLaunch,
            explicitTrainingMode,
            replayMode,
            mode == RunMode::Spectator,
            mode == RunMode::Netplay);
    const bool interactiveOfflineDeveloperKeys =
        nativeDeveloperPolicy
            == qgame::NativeDeveloperKeyPolicy::RespectUserFCommand;
    QLOG("[mode-controls] nativeFKeys policy=%s mode=%s",
         nativeDeveloperPolicy
                 == qgame::NativeDeveloperKeyPolicy::ForceDisabled
             ? "ForceDisabled" : "RespectUserFCommand",
         mode == RunMode::SelfTest ? "SelfTest"
         : mode == RunMode::Netplay ? "Netplay"
         : mode == RunMode::Spectator ? "Spectator"
         : mode == RunMode::Replay ? "Replay"
         : explicitTrainingMode ? "Training"
         : selfTestOrProbeMode ? "Probe" : "OfflineInteractive");
    g_offlineManagedLaunch = mode == RunMode::Observe
        && g_inputConfigPresent && !g_inputSpectatorRole;
    const bool needsSceneHooks =
        deterministicTimelineMode || g_offlineManagedLaunch
        || explicitTrainingMode;

    if (!netVerifyHostImageOnce()) {
        QLOG("[hooks] refusing to install on an unverified host image");
        disarmNetplayState();
        return false;
    }

    if (deterministicTimelineMode
        && (g_qpcFreq <= 0 || g_paceTicks <= 0
            || netUnhookedQpcNow() <= 0)) {
        QLOG("[hooks] mandatory presentation clock preflight FAILED");
        disarmNetplayState();
        return false;
    }
    if (deterministicTimelineMode
        && !netAcquireWrapperPacingOwnership()) {
        QLOG("[hooks] mandatory single-owner pacing handoff FAILED");
        disarmNetplayState();
        return false;
    }

    MH_STATUS mi = MH_Initialize();
    if (mi != MH_OK && mi != MH_ERROR_ALREADY_INITIALIZED) {
        QLOG("[hooks] MH_Initialize FAILED (%d)", (int)mi);
        if (deterministicNetworkMode) disarmNetplayState();
        return false;
    }

    static const uint8_t kMatchSig[] = {
        0x83, 0xEC, 0x54, 0x53, 0x56, 0x57, 0xBF, 0x01
    };
    static const uint8_t kMenuSig[] = {
        0x53, 0x55, 0xBD, 0x01, 0x00, 0x00, 0x00, 0x33
    };
    static const uint8_t kCharSelectSig[] = {
        0x53, 0x56, 0xBB, 0x01, 0x00, 0x00, 0x00, 0x57
    };
    static const uint8_t kFullInputPollSigNovember[] = {
        0x56, 0xE8, 0x0A, 0x87, 0xFD, 0xFF,
        0xA1, 0x58, 0xB7, 0x4A, 0x00
    };
    static const uint8_t kFullInputPollSigDecember[] = {
        0x56, 0xE8, 0xEA, 0x86, 0xFD, 0xFF,
        0xA1, 0x58, 0xB7, 0x4A, 0x00
    };
    static_assert(sizeof(kFullInputPollSigNovember)
                      == sizeof(kFullInputPollSigDecember),
                  "both builds must be checked over the same window");
    const uint8_t* const kFullInputPollSig = seSelectPerBuildSignature(
        kFullInputPollSigNovember, kFullInputPollSigDecember);
    const size_t kFullInputPollSigSize = sizeof(kFullInputPollSigNovember);
    static const uint8_t kConfigSaveSig[] = {
        0x81, 0xEC, 0x04, 0x01, 0x00, 0x00,
        0x8B, 0xD1, 0x85, 0xD2
    };
    static const uint8_t kFCommandParseSigNovember[] = {
        0xB8, 0x98, 0x20, 0x00, 0x00,
        0xE8, 0xE6, 0x55, 0x02, 0x00
    };
    static const uint8_t kFCommandParseSigDecember[] = {
        0xB8, 0x98, 0x20, 0x00, 0x00,
        0xE8, 0x06, 0x56, 0x02, 0x00
    };
    const bool selectedDecember =
        runtime_profile::selectedProfileId()
        == qoh99::generated_model::RuntimeProfileId::Qoh99DecemberCandidate;
    const uint8_t* const kFCommandParseSig = selectedDecember
        ? kFCommandParseSigDecember : kFCommandParseSigNovember;
    const size_t kFCommandParseSigSize = sizeof(kFCommandParseSigNovember);
    static_assert(sizeof(kFCommandParseSigNovember)
                      == sizeof(kFCommandParseSigDecember),
                  "both builds must be checked over the same window");
    // sub_421EE0's pre-transition commit barrier: cmp [0x871950],5; the driver requires exact remote input when the indicator is 5, so the commit never runs speculatively.
    static const uint8_t kTransitionCommitSig[] = {
        0x83, 0x3D, 0x50, 0x19, 0x87, 0x00, 0x05, 0x75, 0x1F
    };
    struct MandatorySignature {
        bool required;
        const char* label;
        uintptr_t address;
        const uint8_t* expected;
        size_t size;
    };
    const bool timelineOrTraining =
        deterministicTimelineMode || explicitTrainingMode;
    const MandatorySignature kMandatory[] = {
        { true, "match dispatcher", g_frameDispatchFn, kMatchSig,
          sizeof(kMatchSig) },
        { deterministicNetworkMode, "FCommand parser", kFCommandParseAddress,
          kFCommandParseSig, kFCommandParseSigSize },
        { timelineOrTraining, "title dispatcher", g_scene1DispatchFn, kMenuSig,
          sizeof(kMenuSig) },
        { timelineOrTraining, "VS dispatcher", g_scene3DispatchFn,
          kCharSelectSig, sizeof(kCharSelectSig) },
        { timelineOrTraining, "config save", g_configSaveFn, kConfigSaveSig,
          sizeof(kConfigSaveSig) },
        { deterministicTimelineMode || selfTestOrProbeMode
              || interactiveOfflineDeveloperKeys,
          "full input poll", g_fullInputPollFn,
          kFullInputPollSig, kFullInputPollSigSize },
        { timelineOrTraining && mode == RunMode::Netplay && g_netRollback > 0,
          "pre-transition commit barrier", 0x425B20, kTransitionCommitSig,
          sizeof(kTransitionCommitSig) },
    };
    for (const MandatorySignature& check : kMandatory) {
        if (!check.required) continue;
        if (se::verifyCodeBytes(check.address, check.expected, check.size))
            continue;
        char wanted[80] = {0};
        char found[80] = {0};
        const uint8_t* const actual =
            reinterpret_cast<const uint8_t*>(check.address);
        for (size_t i = 0; i < check.size && i < 16u; ++i) {
            std::snprintf(wanted + i * 3, 4, "%02X ", check.expected[i]);
            if (check.address != 0)
                std::snprintf(found + i * 3, 4, "%02X ", actual[i]);
        }
        QLOG("[hooks] mandatory dispatcher signature mismatch - unsupported "
             "game build: site='%s' profile=%s address=%08X want=[%s] got=[%s]",
             check.label,
             selectedDecember ? "qoh99-december-candidate" : "qoh99-se1126",
             static_cast<unsigned>(check.address), wanted, found);
        MH_Uninitialize();
        disarmNetplayState();
        return false;
    }

    installNativeHudHooksOnce();

    if (deterministicTimelineMode) {
        installManagedInputGuardsOnce();
        if ((g_inputConfigPresent || replayMode)
            && (!g_inputInitHooked || !g_presenceProbeHooked
                || !g_joyLowHooked || !g_fullPollHooked)) {
            QLOG("[hooks] mandatory managed-input guards FAILED");
            rollbackHookTransaction("managed-input guard creation failed");
            disarmNetplayState();
            return false;
        }
    } else if (selfTestOrProbeMode || interactiveOfflineDeveloperKeys) {
        installNativeDeveloperEdgeGuardOnce();
        if (!g_fullPollHooked) {
            QLOG("[hooks] mandatory native developer-key full-poll guard "
                 "FAILED (diagnostic=%d interactiveOffline=%d)",
                 selfTestOrProbeMode ? 1 : 0,
                 interactiveOfflineDeveloperKeys ? 1 : 0);
            rollbackHookTransaction(
                "native developer-key full-poll guard creation failed");
            disarmNetplayState();
            return false;
        }
    }

    if (deterministicNetworkMode) {
        void* fCommandTarget =
            reinterpret_cast<void*>(kFCommandParseAddress);
        if (MH_CreateHook(
                fCommandTarget,
                reinterpret_cast<void*>(&hkFCommandParse),
                reinterpret_cast<void**>(&g_origFCommandParse)) == MH_OK
            && MH_QueueEnableHook(fCommandTarget) == MH_OK) {
            g_fCommandPolicyHooked = true;
            QLOG("[fcommand-policy] post-parser defaults hook queued at "
                 "sub_46ED90 (netplay/spectator only)");
        } else {
            QLOG("[fcommand-policy] mandatory parser hook creation FAILED");
            rollbackHookTransaction("FCommand parser hook creation failed");
            disarmNetplayState();
            return false;
        }
    }

    void* target = reinterpret_cast<void*>(g_frameDispatchFn);
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hkFrameDispatch),
                      reinterpret_cast<void**>(&g_orig)) != MH_OK) {
        QLOG("[hooks] MH_CreateHook(0x%08X) FAILED", (unsigned)g_frameDispatchFn);
        rollbackHookTransaction("frame dispatcher hook creation failed");
        disarmNetplayState();
        return false;
    }

    void* s1 = reinterpret_cast<void*>(g_scene1DispatchFn);
    void* s3 = reinterpret_cast<void*>(g_scene3DispatchFn);
    if (needsSceneHooks) {
        if (MH_CreateHook(s1, reinterpret_cast<void*>(&hkScene1Dispatch),
                          reinterpret_cast<void**>(&g_origScene1)) != MH_OK ||
            MH_CreateHook(s3, reinterpret_cast<void*>(&hkScene3Dispatch),
                          reinterpret_cast<void**>(&g_origScene3)) != MH_OK) {
            QLOG("[hooks] mandatory scene hook creation FAILED");
            rollbackHookTransaction("scene dispatcher hook creation failed");
            disarmNetplayState();
            return false;
        }
    }

    if (needsSceneHooks || g_unattendedHarnessLaunch) {
        installGameplayHooksOnce();
    }

    if (explicitTrainingMode && !installTrainingHooksOnce()) {
        QLOG("[hooks] mandatory explicit-training hook transaction FAILED "
             "- rolling back startup");
        rollbackHookTransaction("training hook creation failed");
        disarmNetplayState();
        return false;
    }
    if (replayMode) replayInstallCustomHitboxes();

    const bool needsClockInterception = deterministicTimelineMode
        || (g_replayMode == ReplayTickMode::Deterministic && g_clockShimEnabled);
    if (needsClockInterception) {
        if (!netInstallQohClockImportPatches() && deterministicTimelineMode) {
            QLOG("[hooks] mandatory deterministic clock interception FAILED");
            rollbackHookTransaction("QOH clock import patch failed");
            disarmNetplayState();
            return false;
        }
    }

    if (needsSceneHooks) {
        const bool gameplayHooksIncomplete = deterministicNetworkMode
            ? (!g_readHooked || !g_paceBracketHooked
               || !g_pollHooked || !g_clockHooked
               || !g_configSaveHooked || !g_fCommandPolicyHooked
               || g_origFCommandParse == nullptr
               || !playbackSideEffectHooksReady())
            : replayMode
                ? (!g_readHooked || !g_paceBracketHooked || !g_pollHooked
                   || !g_clockHooked || !g_configSaveHooked
                   || !playbackSideEffectHooksReady())
                : (!g_readHooked
                   || (explicitTrainingMode
                       && (!g_configSaveHooked
                           || !g_soundStopHooked
                           || g_origSoundStop == nullptr)));
        if (gameplayHooksIncomplete) {
            QLOG("[hooks] mandatory gameplay hooks incomplete "
                 "(input=%d pace=%d poll=%d clock=%d configSave=%d "
                 "fcommand=%d "
                 "palette=%d audio=%d soundStop=%d "
                 "rollback=%u) "
                 "- rolling back startup",
                 (int)g_readHooked, (int)g_paceBracketHooked,
                 (int)g_pollHooked, (int)g_clockHooked,
                 (int)g_configSaveHooked,
                 (int)g_fCommandPolicyHooked,
                 (int)g_paletteHooked, (int)audioHooksReady(),
                 (int)(g_soundStopHooked
                       && g_origSoundStop != nullptr),
                 g_netRollback);
            rollbackHookTransaction("mandatory gameplay hooks incomplete");
            disarmNetplayState();
            return false;
        }
    }

    bool queued = MH_QueueEnableHook(target) == MH_OK;
    if (queued && needsSceneHooks) {
        queued = MH_QueueEnableHook(s1) == MH_OK
            && MH_QueueEnableHook(s3) == MH_OK;
    }
    if (!queued) {
        QLOG("[hooks] mandatory hook queueing FAILED");
        rollbackHookTransaction("dispatcher queueing failed");
        disarmNetplayState();
        return false;
    }

    const DWORD applyStartedMs = GetTickCount();
    const MH_STATUS applyStatus = MH_ApplyQueued();
    const DWORD applyElapsedMs = GetTickCount() - applyStartedMs;
    if (applyStatus != MH_OK) {
        const char* what =
            applyStatus == MH_ERROR_NOT_INITIALIZED ? "MH_ERROR_NOT_INITIALIZED"
            : applyStatus == MH_ERROR_NOT_CREATED   ? "MH_ERROR_NOT_CREATED"
            : applyStatus == MH_ERROR_ENABLED       ? "MH_ERROR_ENABLED"
            : applyStatus == MH_ERROR_DISABLED      ? "MH_ERROR_DISABLED"
            : applyStatus == MH_ERROR_MEMORY_ALLOC  ? "MH_ERROR_MEMORY_ALLOC"
            : applyStatus == MH_ERROR_MEMORY_PROTECT? "MH_ERROR_MEMORY_PROTECT"
                                                    : "MH_ERROR_UNKNOWN";
        QLOG("[hooks] MH_ApplyQueued FAILED status=%d (%s) after %lums - "
             "thread freeze/patch transaction did not complete; restoring "
             "every IAT slot and removing every created hook",
             static_cast<int>(applyStatus), what,
             static_cast<unsigned long>(applyElapsedMs));
        rollbackHookTransaction("MH_ApplyQueued failed");
        disarmNetplayState();
        return false;
    }

    g_mode = mode;
    QLOG("[hooks] ONE queued transaction applied in %lums: frame dispatch "
         "0x%08X, mode=%s sceneHooks=%d offlineManaged=%d importPatches=%zu",
         static_cast<unsigned long>(applyElapsedMs),
         (unsigned)g_frameDispatchFn,
         mode == RunMode::SelfTest ? "SelfTest" :
         mode == RunMode::Netplay  ? "Netplay" :
         mode == RunMode::Spectator ? "Spectator" :
         mode == RunMode::Replay ? "Replay" : "Observe",
         needsSceneHooks ? 1 : 0,
         g_offlineManagedLaunch ? 1 : 0,
         g_qohImportPatchCount);
    if (deterministicTimelineMode)
        QLOG("[%s] mandatory scene dispatchers committed: menu 0x%08X + "
             "charselect 0x%08X + match 0x%08X",
             mode == RunMode::Spectator ? "spectator"
             : mode == RunMode::Replay  ? "replay" : "netplay",
             (unsigned)g_scene1DispatchFn, (unsigned)g_scene3DispatchFn,
             (unsigned)g_frameDispatchFn);
    return true;
}

void armTraining(bool enabled) {
    trainingArmInternal(enabled);
}

void enableOfflineVsScoreboard() {
    g_offlineVsScoreboard = true;
    QLOG("[offline-vs] scoreboard on: completed games per side are counted "
         "and shown as \"Wins: N\" on the name plates");
}

void setUnattendedHarnessLaunch(bool enabled) {
    g_unattendedHarnessLaunch = enabled;
    if (enabled)
        QLOG("[frontend_press] unattended harness launch: logical-read wrap "
             "commits at install time; press driver will play character "
             "select (2 confirm edges per side, then stage/proceed)");
}

void harnessFrontendPressTick() {
    frontendPressDriverTick();
    netPaletteExchangeTick();
}

void setNativeHudSuppression(bool hideScore, bool hideTimer) {
    g_hideNativeScore = hideScore;
    g_hideNativeTimer = hideTimer;
    const qgame::NativeHudVisibilityProfileV1 migrated =
        qgame::nativeHudProfileFromLegacy(hideScore, hideTimer);
    g_nativeHudPrefs.training = migrated;
    g_nativeHudPrefs.replay = migrated;
    ++g_nativeHudGeneration;
}

void setModularNativeHudEligible(bool eligible) {
    g_nativeHudModularEligible = eligible;
}

void getModularNativeHudStatus(uint32_t* owner, uint32_t* globalMask,
                               uint32_t* p1Mask, uint32_t* p2Mask,
                               uint32_t* unavailableGlobal,
                               uint32_t* unavailableP1,
                               uint32_t* unavailableP2) {
    const qgame::NativeHudOfflineOwner current = nativeHudOfflineOwner();
    const qgame::NativeHudEffectiveMasks masks =
        nativeHudEffectiveMasksFor(current);
    if (owner) *owner = static_cast<uint32_t>(current);
    if (globalMask) *globalMask = masks.globalMask;
    if (p1Mask) *p1Mask = masks.p1Mask;
    if (p2Mask) *p2Mask = masks.p2Mask;
    if (unavailableGlobal) *unavailableGlobal = masks.unavailableGlobalMask;
    if (unavailableP1) *unavailableP1 = masks.unavailableP1Mask;
    if (unavailableP2) *unavailableP2 = masks.unavailableP2Mask;
}

void getModularNativeHudRequested(uint32_t* owner, uint32_t* globalMask,
                                  uint32_t* p1Mask, uint32_t* p2Mask) {
    const qgame::NativeHudOfflineOwner current = nativeHudOfflineOwner();
    const qgame::NativeHudVisibilityProfileV1& profile =
        current == qgame::NativeHudOfflineOwner::Replay
            ? g_nativeHudPrefs.replay : g_nativeHudPrefs.training;
    if (owner) *owner = static_cast<uint32_t>(current);
    if (globalMask) *globalMask = profile.globalVisibleMask;
    if (p1Mask) *p1Mask = profile.p1VisibleMask;
    if (p2Mask) *p2Mask = profile.p2VisibleMask;
}

bool setModularNativeHudTrainingMasks(uint32_t globalMask, uint32_t p1Mask,
                                      uint32_t p2Mask) {
    if (!qgame::nativeHudGlobalMaskWellFormed(globalMask)
        || !qgame::nativeHudPlayerMaskWellFormed(p1Mask)
        || !qgame::nativeHudPlayerMaskWellFormed(p2Mask)) {
        return false;
    }
    g_nativeHudPrefs.training.globalVisibleMask = globalMask;
    g_nativeHudPrefs.training.p1VisibleMask = p1Mask;
    g_nativeHudPrefs.training.p2VisibleMask = p2Mask;
    ++g_nativeHudGeneration;
    return true;
}

void armReplay(unsigned long long manifestDigest,
               unsigned long speedPermille,
               unsigned long matchOrdinal) {
    replayTakeoverStopInputRuntime();
    g_replayTakeoverSnapshot = ReplayTakeoverNativeSnapshot{};
    g_replayStartSnapshot = ReplayTakeoverNativeSnapshot{};
    g_replayStartObservation = qreplay::ReplayBootstrapObservationV2{};
    g_replaySeekCache.clear();
    g_replayWinScreenTicks = 0u;
    if (g_audioV2Enabled)
        qaudio::runtime().endReplayTakeover();
    g_replayTakeoverInputWindow = nullptr;
    g_replayRoleActive = true;
    qgame::paletteClearReplayHold();
    g_replayManifestDigest = manifestDigest;
    g_replaySpeedPermille = static_cast<uint32_t>(speedPermille);
    g_replayMatchOrdinal = static_cast<uint32_t>(matchOrdinal);
    g_replayHalted = false;
    g_replayTicks = 0u;
    g_replayHiddenTicks = 0u;
    g_replayBufferingSlots = 0u;
    g_replayBootstrapPhase = ReplayBootstrapPhase::WaitingForStart;
    g_replayGameStartV2 = qreplay::ConfirmedGameStartV2{};
    g_replayObservedBootstrapFingerprint = 0u;
    g_replayObservedInitialPhase = 0u;
    g_replayBootstrapSlots = 0u;
    g_replayBootstrapSelectorConfirms = {};
    g_replayBootstrapSelectorLaunch = 0u;
    g_replayBootstrapKnockoutLatches = {};
    g_replayBootstrapSelectorLifecycleValid = false;
    g_replayClosePosted = false;
    g_replayTerminalSerial = 0;
    InterlockedExchange(&g_replayPresentedTickSerial, 0);
    g_netEpoch = 1;
    netPaletteResetForSession();
    g_netBootstrapTransientsEpoch = 0;
    g_netFrame = 0;
    g_netGameplaySegmentActive = false;
    g_netGameplaySegmentSeen = false;
    resetCanonicalAudioSession();
    if (g_audioV2Enabled) {
        qaudio::runtime().resetSession();
        qaudio::runtime().setMuted(g_audioV2Muted);
        qaudio::runtime().setHorizons(g_audioV2Horizons);
    }
    g_netActive = false;
    g_netRunning = false;
    g_netSpectatorMode = false;
    g_netServeSpectators = false;
    g_inputConfigPresent = false;
    g_offlineManagedLaunch = false;
    g_ovrEnabled = true;
    netPaceInit();
    QLOG("[replay] armed deterministic file playback manifestDigest=%016llX "
         "matchOrdinal=%lu speed=%lupm (no socket, no player identity; "
         "takeover input remains lazy)",
         manifestDigest, matchOrdinal, speedPermille);
}

void armRecorder(bool enabled, unsigned long channelGeneration) {
    g_recorderEnabled = enabled && channelGeneration != 0;
    g_recorderGeneration = static_cast<uint32_t>(channelGeneration);
    g_recorderPublished = 0;
    g_recorderRollingDigest = 0;
    g_recorderDisabled = false;
    g_recorderOverflowReported = false;
    g_recorderLastFrame = 0;
    g_recorderHasLastFrame = false;
    g_recorderStart = qreplay::DeterministicStart{};
    g_recorderStartReady = false;
    g_recorderStartSource = "none";
    g_recorderMatchId = 0;
    g_recorderFirstMatchId = 0;
    g_recorderNextMatchId = 1;
    g_recorderNextOrdinal = 0;
    g_recorderMatchSceneSeen = false;
    g_recorderMatchPostTailHash = 0;
    QLOG("[recorder] %s generation=%lu (publication is a bounded copy from the "
         "confirmed seam; any failure disables the recording and never alters "
         "the match)",
         g_recorderEnabled ? "armed" : "disabled", channelGeneration);
}

void setSpectatorServiceMode(unsigned char serviceMode) {
    g_spectatorServiceMode = serviceMode;
    QLOG("[spectator-source] service mode=%u (%s)", serviceMode,
         serviceMode == 0 ? "OFF: no socket, no archive"
         : serviceMode == 1 ? "in-process: game owns socket + archive"
                            : "sidecar: game publishes, caster owns fanout");
}

void spectatorSubmitPlaybackCommand(unsigned char command,
                                    unsigned long long argument) {
    const LONG head = InterlockedCompareExchange(&g_spectatorCommandHead, 0, 0);
    const LONG tail = InterlockedCompareExchange(&g_spectatorCommandTail, 0, 0);
    if (static_cast<uint32_t>(head - tail) >= kSpectatorCommandQueueDepth) {
        const LONG drops = InterlockedIncrement(&g_spectatorCommandDrops);
        if (drops <= 8 || drops % 100 == 0) {
            QLOG("[spectator] local command queue full; dropped %ld commands "
                 "(playback is unaffected)", drops);
        }
        return;
    }
    SpectatorCommandSlot& slot =
        g_spectatorCommands[static_cast<uint32_t>(head)
                            % kSpectatorCommandQueueDepth];
    slot.command = static_cast<qreplay::PlaybackCommand>(command);
    slot.argument = argument;
    InterlockedExchange(&g_spectatorCommandHead, head + 1);
}

void spectatorPlaybackStatus(SpectatorPlaybackReadout& out) {
    out = SpectatorPlaybackReadout{};
    qgame::SpectatorViewerStatus status{};
    if (!qgame::spectatorViewerGetStatus(status) || !status.active) return;
    out.active = true;
    out.paused = g_spectatorLocalPaused;
    out.fastForward =
        g_spectatorFastForwardHeld || g_spectatorFastForwardLatched;
    out.speedPermille = g_spectatorSpeedPermille;
    out.playbackBacklogRecords =
        static_cast<unsigned long>(status.bufferedRecords);
    out.receiveBacklogRecords =
        status.receivedNextRecord >= status.playbackNextRecord
            ? status.receivedNextRecord - status.playbackNextRecord
            : 0u;
    out.liveTargetRecords =
        static_cast<unsigned long>(qgame::kSpectatorLiveTargetRecords);
    out.catchingUp = g_spectatorCatchingUp;
    out.estimatedSlotsToLive =
        qgame::spectatorEstimatedSlotsToLive(status.bufferedRecords,
                                             g_spectatorCatchingUp,
                                             g_spectatorCushionBonus);
    out.currentMatchId = status.currentMatchId;
    out.sourceMatchId = status.sourceMatchId;
    out.behindLiveKnown = status.sourceFrontierKnown
        && status.sourceNextRecord >= status.playbackNextRecord;
    out.behindLiveRecords = out.behindLiveKnown
        ? static_cast<unsigned long long>(status.sourceNextRecord
                                          - status.playbackNextRecord)
        : static_cast<unsigned long long>(status.bufferedRecords);
    out.cushionBonusRecords =
        static_cast<unsigned long>(g_spectatorCushionBonus);
    out.catchupSpeedPermille = g_spectatorCatchupSpeedPermille;
    out.estimatedSecondsToLive =
        static_cast<double>(out.estimatedSlotsToLive) * 10.0 / 606.0;
    out.backlogSeconds =
        static_cast<double>(status.bufferedRecords) * 10.0 / 606.0;
    out.underruns = g_spectatorUnderruns;
    out.truncatedSlices = g_spectatorCatchupSlicesTruncated;
    out.sourceContactAgeMs = status.sourceContactAgeMs;
    out.sourceContactFresh = status.sawSourceContact
        && status.sourceContactAgeMs <= kSpectatorHeartbeatFreshMs;
    out.hasTerminal = status.hasTerminal;
    out.complete =
        status.phase == qnetplay::spectator::ViewerPhase::Complete;
    if (status.hasTerminal) {
        qnetplay::spectator::ArchiveTerminal terminal{};
        if (qgame::spectatorViewerGetTerminal(terminal))
            out.terminalReason = static_cast<unsigned>(terminal.reason);
    }
}

void setNativeDeveloperKeyLaunchPolicy(
    bool allowInteractiveOfflineKeys) {
    g_nativeDeveloperKeysAllowedByLaunch =
        allowInteractiveOfflineKeys;
}

void setNetplayExtraFunctionPolicy(bool disableExtraFunctions) {
    if (!disableExtraFunctions) {
        QLOG("[fcommand-policy] legacy request to allow extra functions "
             "ignored for live network roles");
    }
}

void setSessionDir(const char* dir) {
    if (dir && dir[0]) {
        std::strncpy(g_sessionDir, dir, sizeof(g_sessionDir) - 1);
        g_sessionDir[sizeof(g_sessionDir) - 1] = 0;
    }
}

void removeHooks() {
    // Never unhook at detach: the DLL stays pinned, and waiting in DLL_PROCESS_DETACH would deadlock under the loader lock.
    QLOG("[hooks] runtime removal ignored: hooks are process-lifetime");
}

}

