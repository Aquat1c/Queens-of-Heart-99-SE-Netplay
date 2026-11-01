// Must stay at file scope: including inside this unit's unnamed namespace defines distinct types and breaks linking.
}
}

#include "input/InputDisplayPublisher.h"
#include "input/InputDisplayOverlayBridge.h"

#include "training/TrainingGuardTiming.h"
#include "training/TrainingHold.h"
#include "training/TrainingActionCatalog.h"
#include "training/TrainingCombatTools.h"
#include "training/TrainingRecovery.h"
#include "training/TrainingResources.h"
#include "training/NativeRecoveryQualification.h"
#include "training/TrainingHitboxOverlay.h"
#include "training/TrainingHitboxPublisher.h"
#include "training/TrainingHitboxOverlayBridge.h"

namespace qoh99 {
namespace {

using qtraining::CommandAckResult;
using qtraining::DummyStance;
using qtraining::DummyTech;
using qtraining::FacingPolicy;
using qtraining::GuardClassification;
using qtraining::GuardMode;
using qtraining::InputRearmState;
using qtraining::MacroState;
using qtraining::ManualRecoveryMode;
using qtraining::PlaybackMode;
using qtraining::PositionPreset;
using qtraining::PostureClass;
using qtraining::RecoveryPolicySlot;
using qtraining::RefillVerdict;
using qtraining::ResourceTrigger;
using qtraining::StepAdmissionResult;
using qtraining::TerminalRecoveryChoice;
using qtraining::TrainingTriggerEvent;
using qtraining::TrainingRefillTarget;
using qtraining::TrainingCapability;
using qtraining::TrainingCommandKind;
using qtraining::TrainingCommandV1;
using qtraining::TrainingDisableReason;
using qtraining::TrainingLifecycleState;
using qtraining::TrainingStatusV1;

constexpr uintptr_t kTrainingMatchScopeFn = 0x421EE0u;
constexpr uintptr_t kTrainingStepGateFn = 0x41A0E0u;
constexpr uintptr_t kTrainingResetFn = 0x40AAD0u;
constexpr uintptr_t kTrainingDamageFn = 0x42D760u;
constexpr uintptr_t kTrainingSelectorConfirmFn = 0x434410u;
constexpr uintptr_t kTrainingThreatNowFn = 0x4428B0u;
constexpr uintptr_t kTrainingThreatNextFn = 0x442AB0u;
constexpr uintptr_t kTrainingCollisionRectFn = 0x462A10u;
constexpr uintptr_t kTrainingFacingFlipFn = 0x463510u;
constexpr uintptr_t kTrainingWorldClampFn = 0x464920u;
constexpr uintptr_t kTrainingPreBoxRenderFn = 0x419C70u;

constexpr uintptr_t kTrainingComMaskAddr = 0x6236CCu;
constexpr uintptr_t kTrainingMatchModeAddr = 0x62369Cu;
constexpr uintptr_t kTrainingScene3SubstateAddr = 0x4AA454u;
constexpr uintptr_t kTrainingP1InputSourceAddr = 0x4A37C4u;
constexpr uintptr_t kTrainingP2InputSourceAddr = 0x4A37C8u;
constexpr uintptr_t kTrainingP2RouteAddr = 0x613F14u;
constexpr uintptr_t kTrainingP1SelectStateAddr = 0x8716D0u;
constexpr uintptr_t kTrainingP2SelectStateAddr = 0x8716D4u;
constexpr uintptr_t kTrainingStageSubphaseAddr = 0x8716E8u;
constexpr uintptr_t kTrainingModeAddr = 0x4AAA50u;
constexpr uintptr_t kTrainingVsLaunchAddr = 0x6236B0u;
constexpr uintptr_t kTrainingCopiedConfigAddr = 0x623520u;
constexpr uintptr_t kTrainingPauseAddr = 0x62D580u;
constexpr uintptr_t kTrainingFreezeAddr = 0x6267D0u;
constexpr uintptr_t kTrainingHitboxAddr = 0x4AA424u;
constexpr uintptr_t kTrainingCameraAddr = 0x626E80u;
constexpr uintptr_t kTrainingCameraYAddr = 0x626E84u;
constexpr uintptr_t kTrainingCameraPreviousAddr = 0x62D630u;
constexpr uintptr_t kTrainingCameraParityAddr = 0x4AB754u;
constexpr uintptr_t kTrainingRoundProgressAddr = 0x871950u;
constexpr uintptr_t kTrainingRoundCounterAddr = 0x8719A4u;
constexpr uintptr_t kTrainingOutcomeP1Addr = 0x625620u;
constexpr uintptr_t kTrainingOutcomeP2Addr = 0x625624u;
constexpr uintptr_t kTrainingWinsP1Addr = 0x625628u;
constexpr uintptr_t kTrainingWinsP2Addr = 0x62562Cu;
constexpr uintptr_t kTrainingResultProgressP1Addr = 0x625640u;
constexpr uintptr_t kTrainingResultProgressP2Addr = 0x625644u;

// Per-slot combat record at 0x6265C0, 44-byte stride: +0 combo bitmask, +4 guard-crush latch, +8 guard gauge 0..100, +16 combo hit count.
constexpr uintptr_t kTrainingCombatRecordBase =
    static_cast<uintptr_t>(qoh99::generated_model::nativeSymbolNovemberVa(
        qoh99::generated_model::NativeSymbolId::CombatRecordBase));
static_assert(kTrainingCombatRecordBase == 0x6265C0u,
              "the catalogued combat-record base moved away from dword_6265C0");
constexpr uintptr_t kTrainingCombatRecordStride = 44u;
constexpr uint32_t kTrainingCombatRecordSlotCount = 4u;
constexpr uintptr_t kTrainingGuardGaugeOffset = 8u;
constexpr uintptr_t kTrainingComboHitsOffset = 16u;
constexpr uintptr_t kTrainingRecoveryPhaseLatchOffset = 24u;
constexpr uintptr_t kTrainingRecoveryLockOffset = 28u;
static_assert(kTrainingCombatRecordBase + kTrainingRecoveryPhaseLatchOffset
                      == 0x6265D8u
                  && kTrainingCombatRecordBase + kTrainingRecoveryLockOffset
                      == 0x6265DCu
                  && kTrainingCombatRecordBase + kTrainingComboHitsOffset
                      == 0x6265D0u,
              "per-slot combat record offsets disagree with the executable");
constexpr uintptr_t kTrainingCombatRecordBlockEnd =
    kTrainingCombatRecordBase
    + kTrainingCombatRecordStride * kTrainingCombatRecordSlotCount;
// AddressMap's "health" for 0x6265C8 is really the guard gauge; lethal life is dword_6254E0.
static_assert(kGuardGaugeBase
                  == kTrainingCombatRecordBase + kTrainingGuardGaugeOffset
              && addr::kGuardGaugeStride == kTrainingCombatRecordStride,
              "guard gauge is field +8 of the per-slot combat record");

// EX record 0x6267E0 + 0x20 * owner: +0x00 anim state, +0x04 stocks 0..9, +0x08 anim timer, +0x0C + 4*slot pending reservation (never write), +0x1C progress 0..53.
constexpr uintptr_t kTrainingExStateOffset = 0x00u;
constexpr uintptr_t kTrainingExStocksOffset = 0x04u;
constexpr uintptr_t kTrainingExTimerOffset = 0x08u;
constexpr uintptr_t kTrainingExPendingOffset = 0x0Cu;
constexpr uintptr_t kTrainingExFractionOffset = 0x1Cu;
constexpr uintptr_t kTrainingExRecordBlockEnd =
    static_cast<uintptr_t>(addr::kExGaugeRecords)
    + static_cast<uintptr_t>(addr::kExGaugeRecordStride)
        * addr::kExGaugeStorageRecordCount;
static_assert(addr::kExGaugeStocks - addr::kExGaugeRecords
                  == kTrainingExStocksOffset
              && addr::kExGaugeSpendTimer - addr::kExGaugeRecords
                  == kTrainingExTimerOffset
              && addr::kExGaugePendingCosts - addr::kExGaugeRecords
                  == kTrainingExPendingOffset
              && addr::kExGaugeFraction - addr::kExGaugeRecords
                  == kTrainingExFractionOffset,
              "EX record field offsets disagree with the RE address map");

constexpr uintptr_t kTrainingTeamComboBase = 0x8A5C80u;
constexpr uintptr_t kTrainingTeamComboStride = 80u;
constexpr uintptr_t kTrainingTeamComboDamageOffset = 12u;
constexpr uintptr_t kTrainingTeamLastComboHitsOffset = 40u;
constexpr uintptr_t kTrainingTeamLastComboDamageOffset = 52u;
constexpr uintptr_t kTrainingMatchRecordBase = 0x625648u;
constexpr uintptr_t kTrainingMatchRecordStride = 20u;
constexpr uintptr_t kTrainingMatchMaxComboHitsOffset = 0u;
constexpr uintptr_t kTrainingMatchMaxComboDamageOffset = 8u;

// EX owner side; +172 is the slot. Different numbers: do not index one array with the other.
constexpr uintptr_t kTrainingFighterExOwnerOffset = 171u;

constexpr uintptr_t kTrainingFighterComOffset = 1400u;
constexpr uintptr_t kTrainingFighterFacingSnapshotOffset = 1404u;
constexpr uintptr_t kTrainingFighterPreviousXOffset = 1312u;
constexpr uintptr_t kTrainingFighterFatalCauseOffset = 1376u;
constexpr uintptr_t kTrainingFighterFatalAttackerOffset = 1380u;
constexpr uintptr_t kTrainingFighterFloorModeOffset = 1386u;
constexpr uintptr_t kTrainingFighterCharacterOffset = 174u;
constexpr uintptr_t kTrainingFighterFacingOffset = 170u;
constexpr uintptr_t kTrainingFighterXOffset = 136u;
constexpr uintptr_t kTrainingFighterYOffset = 140u;
constexpr uintptr_t kTrainingFighterVerticalVelocityOffset = 98u;
constexpr uintptr_t kTrainingFighterVerticalAccelerationOffset = 102u;
constexpr uintptr_t kTrainingFighterVerticalJerkOffset = 106u;
constexpr uintptr_t kTrainingFighterVerticalTweenOffset = 114u;
constexpr uintptr_t kTrainingFighterVerticalTweenDeltaOffset = 118u;

constexpr uintptr_t kTrainingFighterActionStateOffset = 48u;
constexpr uintptr_t kTrainingFighterActionFrameOffset = 52u;
constexpr uintptr_t kTrainingFighterActionLockEndOffset = 56u;
constexpr uintptr_t kTrainingFighterAnimFrameOffset = 64u;
constexpr uintptr_t kTrainingFighterNextActionOffset = 72u;
constexpr uintptr_t kTrainingFighterTransitionFlagOffset = 76u;
constexpr uintptr_t kTrainingFighterPrevActionOffset = 80u;
constexpr uintptr_t kTrainingFighterForcedActionOffset = 92u;
constexpr uintptr_t kTrainingFighterMoveGateOffset = 156u;
constexpr uintptr_t kTrainingFighterDispatchActionOffset = 84u;
constexpr uintptr_t kTrainingFighterInitStatusOffset = 88u;
constexpr uintptr_t kTrainingFighterCommandChildOffset = 164u;
constexpr uintptr_t kTrainingFighterGuardClassOffset = 157u;
constexpr uintptr_t kTrainingFighterAuthoredFlagsOffset = 158u;
// Native contact-result bits read by the command parser: hit=0x10, clash/paired=0x20, guard=0x40.
constexpr uintptr_t kTrainingFighterContactFlagsOffset = 160u;
// bit 0 = left wall, bit 1 = right wall, WORLD space (sub_464920).
constexpr uintptr_t kTrainingFighterWallFlagsOffset = 1390u;
// bit 0 set when the action's frame next-word reached 0xFFFE.
constexpr uintptr_t kTrainingFighterTerminalReadyOffset = 1528u;
// +1432 points at the 64-byte authored frame record currently displayed, not the one +52 loads next.
constexpr uintptr_t kTrainingFighterFrameRecordPtrOffset = 1432u;
constexpr uintptr_t kTrainingChrFrameRecordSize = 64u;
constexpr uintptr_t kTrainingChrFrameSpriteOffset = 0u;
constexpr uintptr_t kTrainingChrFrameDurationOffset = 8u;
constexpr uintptr_t kTrainingChrFrameActivityOffset = 14u;
static_assert(kTrainingFighterFrameRecordPtrOffset + 4u <= addr::kFighterSize,
              "the CHR frame-record pointer must lie inside the fighter");
static_assert(kTrainingChrFrameSpriteOffset + 2u <= kTrainingChrFrameRecordSize
                  && kTrainingChrFrameDurationOffset + 4u
                      <= kTrainingChrFrameRecordSize
                  && kTrainingChrFrameActivityOffset + 2u
                      <= kTrainingChrFrameRecordSize,
              "frame-trace reads must lie inside the 64-byte CHR record");
constexpr uintptr_t kTrainingFighterThrowTechLatchOffset = 1472u;
constexpr uintptr_t kTrainingFighterPairedActorOffset = 1436u;
constexpr uintptr_t kTrainingFighterThrowLinkScratchOffset = 152u;
constexpr uintptr_t kTrainingFighterThrowHoldOffset = 1320u;
constexpr uintptr_t kTrainingFighterThrowStruggleOffset = 1340u;
constexpr uintptr_t kTrainingFighterThrowStruggleMaxOffset = 1344u;
constexpr uintptr_t kTrainingFighterThrowScratchOffset = 1428u;

constexpr uintptr_t kTrainingFighterFreezeOffset = 1500u;
// +1456 == 1 is the mirror-bind state sub_47CAE0 ORs the partner's +158/+160 through.
constexpr uintptr_t kTrainingFighterAlternateFormOffset = 1456u;
constexpr uintptr_t kTrainingFighterActionTableOffset = 16u;
constexpr uintptr_t kTrainingFighterFrameTableOffset = 24u;
constexpr uintptr_t kTrainingFighterFrameCountOffset = 28u;
constexpr uintptr_t kTrainingFighterCurrentFrameOffset = 1432u;
constexpr uintptr_t kTrainingActionEntryStride = 24u;
constexpr uintptr_t kTrainingFighterFrameStride = 64u;
constexpr uintptr_t kTrainingFrameDurationOffset = 8u;
constexpr uintptr_t kTrainingFrameNextOffset = 12u;
constexpr uintptr_t kTrainingFramePermissionOffset = 14u;
constexpr uintptr_t kTrainingFrameMotionFlagsOffset = 16u;
constexpr uintptr_t kTrainingFrameVerticalVelocityOffset = 36u;
constexpr uintptr_t kTrainingFrameVerticalAccelerationOffset = 40u;
// fighter+1368 is both KO latch and round-lost word; it blocks every transition commit.
static_assert(kFighterRoundLostOffset == 1368u,
              "the KO latch and the round-lost word are the same field");
static_assert(kTrainingFighterTerminalReadyOffset + 4u <= addr::kFighterSize
                  && kTrainingFighterAlternateFormOffset + 4u
                      <= addr::kFighterSize
                  && kTrainingFighterThrowTechLatchOffset + 4u
                      <= addr::kFighterSize
                  && kTrainingFighterPairedActorOffset + 4u
                      <= addr::kFighterSize
                  && kTrainingFighterFreezeOffset + 4u
                      <= addr::kFighterSize
                  && kTrainingFighterWallFlagsOffset + 2u
                      <= addr::kFighterSize,
              "recovery observation fields must lie inside the fighter object");

// Command-history child (sub_47BF70). Directions: 32 dwords at +8, hold counts at +136, head byte at +264 (mod 32). Buttons: 16 dwords at +268, hold counts at +332, head byte at +396 (mod 16). +432 == 1 forces the recorded direction to zero.
constexpr uintptr_t kTrainingCommandDirDwellOffset = 136u;
constexpr uintptr_t kTrainingCommandButtonHoldOffset = 332u;
constexpr uintptr_t kTrainingCommandButtonCountOffset = 397u;
constexpr uint32_t kTrainingCommandButtonCount = 4u;
constexpr uintptr_t kTrainingCommandDirRingOffset = 8u;
constexpr uintptr_t kTrainingCommandDirHeadOffset = 264u;
constexpr uintptr_t kTrainingCommandButtonRingOffset = 268u;
constexpr uintptr_t kTrainingCommandButtonHeadOffset = 396u;
constexpr uintptr_t kTrainingCommandDirectionKillOffset = 432u;
constexpr uintptr_t kTrainingCommandChildSize = 436u;
constexpr uint32_t kTrainingCommandDirRingMask = 31u;
constexpr uint32_t kTrainingCommandButtonRingMask = 15u;
static_assert(kTrainingFighterMoveGateOffset + 4u <= addr::kFighterSize
                  && kTrainingFighterForcedActionOffset + 4u
                      <= addr::kFighterSize
                  && kTrainingFighterContactFlagsOffset + 4u
                      <= addr::kFighterSize
                  && kTrainingFighterXOffset + 4u <= addr::kFighterSize
                  && kTrainingFighterYOffset + 4u <= addr::kFighterSize,
              "fighter readout fields must lie inside the validated object");

constexpr uint32_t kTrainingBaseCapabilities =
    qtraining::TrainingCapabilityReset
    | qtraining::TrainingCapabilityEndlessRound
    | qtraining::TrainingCapabilityInfiniteTime
    | qtraining::TrainingCapabilityDummyGuard
    | qtraining::TrainingCapabilityMacros
    | qtraining::TrainingCapabilitySavestates
    | qtraining::TrainingCapabilityHitboxes
    | qtraining::TrainingCapabilityReturnToSelect
    | qtraining::TrainingCapabilityRefill
    | qtraining::TrainingCapabilityMeterCharge;
constexpr uint32_t kTrainingCommandsPerStep = 8u;

using TrainingMatchScopeFn = int (__cdecl*)();
using TrainingStepGateFn = int (__cdecl*)();
using TrainingDamageFn = int (__fastcall*)(int, int, int, int);
using TrainingSelectorConfirmFn = int (__fastcall*)(int);
using TrainingThreatFn = int (__fastcall*)(int, int*, uint32_t*);
using TrainingCollisionRectFn = int (__fastcall*)(int, int, int*);
using TrainingFacingFlipFn = int (__fastcall*)(int, int);
using TrainingWorldClampFn = int (__fastcall*)(int, int, int);
// sub_42A440 takes ECX/EDX and differs between builds: always call the profile-qualified original.
using TrainingThrowInstallFn = int (__fastcall*)(int, int);
using TrainingThrowTechFn = int (__thiscall*)(int*, int);
using TrainingGuardContactFn = int (__fastcall*)(int, int, int, int);
using TrainingHitContactFn = int (__fastcall*)(
    int, int, int, int, uint32_t*, int*);
using TrainingNaturalActionTerminalFn = int (__thiscall*)(int*);
using RegisterTrainingUiProviderFn = void (__stdcall*)(
    DWORD,
    EfzDx9TrainingStatusProviderV1,
    EfzDx9TrainingCommandEnqueueV1,
    EfzDx9TrainingMenuCaptureV1,
    void*);

static TrainingMatchScopeFn g_trainingOrigMatchScope = nullptr;
static TrainingStepGateFn g_trainingOrigStepGate = nullptr;
static TrainingDamageFn g_trainingOrigDamage = nullptr;
static TrainingSelectorConfirmFn g_trainingOrigSelectorConfirm = nullptr;
static TrainingThrowInstallFn g_trainingOrigThrowInstall = nullptr;
static TrainingThrowTechFn g_trainingOrigThrowTech = nullptr;
static TrainingGuardContactFn g_trainingOrigGuardContact = nullptr;
static TrainingHitContactFn g_trainingOrigHitContact = nullptr;
static TrainingNaturalActionTerminalFn
    g_trainingOrigNaturalActionTerminal = nullptr;
static uintptr_t g_trainingThrowInstallFn = 0u;
static uintptr_t g_trainingThrowTechFn = 0u;
static uintptr_t g_trainingGuardContactFn = 0u;
static uintptr_t g_trainingHitContactFn = 0u;
static uintptr_t g_trainingNaturalActionTerminalFn = 0u;
static bool g_trainingThrowTechHookReady = false;
static bool g_trainingContactHooksReady = false;

static uintptr_t g_trainingCriticalScalerFn = 0u;
static uintptr_t g_trainingGuardActionRequestFn = 0u;
static uintptr_t g_trainingFaultlessDetectorFn = 0u;
static uintptr_t g_trainingReduceDetectorFn = 0u;

using TrainingCriticalScalerFn =
    int(__fastcall*)(int attacker, int defender, int* damage,
                     uint32_t* criticalFlag);
// The native ABI is __thiscall; the x86 MinHook detour is __fastcall with an unused EDX parameter.
using TrainingGuardActionRequestFn =
    int(__fastcall*)(int fighter, int edx, char mode);
using TrainingFaultlessDetectorFn = int(__fastcall*)(int fighter, int edx);
// Real __fastcall: mode in ECX, victim actor in EDX (sub_47D510).
using TrainingReduceDetectorFn = int(__fastcall*)(int mode, int actor);

static TrainingCriticalScalerFn g_trainingOrigCriticalScaler = nullptr;
static TrainingGuardActionRequestFn g_trainingOrigGuardActionRequest = nullptr;
static TrainingFaultlessDetectorFn g_trainingOrigFaultlessDetector = nullptr;
static TrainingReduceDetectorFn g_trainingOrigReduceDetector = nullptr;

static bool g_trainingCriticalHookReady = false;
static bool g_trainingParryHookReady = false;
static bool g_trainingFaultlessHookReady = false;
static bool g_trainingReduceHookReady = false;

static qtraining::TrainingContactPolicySet g_trainingContactPolicyRequested{};
static qtraining::TrainingContactPolicySet g_trainingContactPolicyLatched{};

struct TrainingCriticalContext {
    uint32_t depth = 0u;
    bool consumed = false;
    qtraining::CriticalRollOverride override =
        qtraining::CriticalRollOverride::Native;
    uint32_t attackerSlot = 0u;
    uint32_t incomingHit = 0u;
    uint32_t divisor = 0u;
    bool guardCrushLatch = false;
    uint32_t nativeSample = 0u;
    uint32_t finalSample = 0u;
};
static thread_local TrainingCriticalContext t_trainingCriticalContext{};

constexpr uintptr_t kTrainingComboCountBase = 0x6265D0u;
constexpr uintptr_t kTrainingGuardCrushLatchBase = 0x6265C4u;

struct TrainingLastCriticalRecord {
    bool valid = false;
    uint32_t attackerSlot = 0u;
    uint32_t incomingHit = 0u;
    uint32_t divisor = 0u;
    uint32_t nativeSample = 0u;
    uint32_t finalSample = 0u;
    uint32_t guardCrushLatch = 0u;
    uint32_t finalFlag = 0u;
    uint32_t damageBefore = 0u;
    uint32_t damageAfter = 0u;
    uint32_t policy = 0u;
};
struct TrainingLastFaultlessRecord {
    bool valid = false;
    uint32_t nativeMode = 0u;
    uint32_t finalMode = 0u;
    uint32_t policy = 0u;
};
struct TrainingLastParryRecord {
    bool valid = false;
    uint32_t nativeResult = 0u;
    uint32_t finalResult = 0u;
    uint32_t policy = 0u;
};
struct TrainingLastReduceRecord {
    bool valid = false;
    uint32_t victimSlot = 0u;
    uint32_t nativeVerdict = 0u;
    uint32_t finalVerdict = 0u;
    uint32_t policy = 0u;
};
static qtraining::ExRestoreTarget g_trainingExRestoreTarget{};

static TrainingLastCriticalRecord g_trainingLastCritical{};
static TrainingLastFaultlessRecord g_trainingLastFaultless{};
static TrainingLastParryRecord g_trainingLastParry{};
static TrainingLastReduceRecord g_trainingLastReduce{};
static uint32_t trainingCapabilityFlags() {
    uint32_t flags = kTrainingBaseCapabilities;
    if (g_trainingContactHooksReady) {
        flags |= qtraining::TrainingCapabilityTriggers
            | qtraining::TrainingCapabilityFrameAdvantage;
    }
    if (g_trainingThrowTechHookReady)
        flags |= qtraining::TrainingCapabilityThrowTech;
    if (g_trainingFaultlessHookReady)
        flags |= qtraining::TrainingCapabilityFaultlessPolicy;
    if (g_trainingParryHookReady)
        flags |= qtraining::TrainingCapabilityParryPolicy;
    if (g_trainingCriticalHookReady)
        flags |= qtraining::TrainingCapabilityCriticalPolicy;
    if (g_trainingReduceHookReady)
        flags |= qtraining::TrainingCapabilityReducePolicy;
    return flags;
}
static bool g_trainingHooksInstalled = false;
static bool g_trainingHooksSupported = false;
static bool g_trainingGuardScannerProloguesMatch = false;
static bool g_trainingUiRegistered = false;
static uint64_t g_trainingUiRetryMs = 0;

static qtraining::TrainingCommandQueue g_trainingCommands;
static qtraining::TrainingStatusPublisher g_trainingStatusPublisher;
static qtraining::TrainingInput g_trainingInput;
static qtraining::TrainingMacros g_trainingMacros;
static qtraining::GuardPredictionTelemetry g_trainingGuardTelemetry;

struct TrainingPhysicalSnapshot {
    uint32_t p1Mask = 0;
    uint32_t p2Mask = 0;
    bool p1NeutralBeforeSocd = false;
    uint32_t heldActions = 0;
    uint64_t dispatchSerial = 0;
};
static TrainingPhysicalSnapshot g_trainingPhysical{};

static constexpr uint32_t kTrainingNoDamageDescription = 0xFFFFFFFFu;

struct TrainingDamageEvent {
    uint32_t admittedSerial = 0;
    uint32_t declinedSerial = 0;
    uint32_t anomalySerial = 0;
    int32_t damage = 0;
    int32_t pendingDelta = 0;
    uint32_t attackerSlot = 0;
    uint32_t victimSlot =
        kTrainingNoDamageDescription;
    uint32_t cause = 0;
    uint32_t admitted = 0;
};
static TrainingDamageEvent g_trainingDamage{};

static void trainingClearLastDamageEvent() {
    g_trainingDamage.damage = 0;
    g_trainingDamage.pendingDelta = 0;
    g_trainingDamage.attackerSlot = 0;
    g_trainingDamage.victimSlot = kTrainingNoDamageDescription;
    g_trainingDamage.cause = 0;
    g_trainingDamage.admitted = 0u;
}

struct alignas(16) TrainingFpuImage {
    alignas(16) uint8_t bytes[512] = {};
    bool valid = false;
};

struct TrainingStateSlot {
    std::vector<uint8_t> bytes;
    TrainingFpuImage fpu{};
    uint32_t generation = 0;
    uint32_t layoutGeneration = 0;
    uint64_t characterFingerprint = 0;
    uint64_t stepSerial = 0;
    // Training-local splitmix64 state; never draw from sub_4914B0, which the simulation consumes.
    uint64_t recoveryRng = 0;
    uint32_t scene = 0;
    uint32_t matchSubstate = 0;
    bool valid = false;
    bool stale = false;
};
static std::array<TrainingStateSlot,
                  qtraining::kTrainingStateSlotCount> g_trainingStates{};
static TrainingStateSlot g_trainingScratch{};

struct TrainingInvariantBaseline {
    uint32_t resultState = 0;
    uint32_t roundProgress = 0;
    uint32_t roundCounter = 0;
    uint32_t outcomeP1 = 0;
    uint32_t outcomeP2 = 0;
    uint32_t winsP1 = 0;
    uint32_t winsP2 = 0;
    uint32_t resultProgressP1 = 0;
    uint32_t resultProgressP2 = 0;
    bool valid = false;
};
static TrainingInvariantBaseline g_trainingBaseline{};

static uint32_t g_trainingGeneration = 1u;
static uint64_t g_trainingStepSerial = 0;
static TrainingRefillTarget g_trainingMeterChargeTarget =
    TrainingRefillTarget::Both;
static uint32_t g_trainingMeterChargeInterval = 2u;
static uint32_t g_trainingMeterChargeCounter = 0u;
static uint64_t g_trainingPollSerial = 0;
static qtraining::TrainingHold g_trainingHold;
static uint32_t g_trainingSelectedStateSlot = 0;
static GuardMode g_trainingGuardMode = GuardMode::Off;
static bool g_trainingPlayerSwap = false;
static bool g_trainingPolicyFollowsHuman = false;
static qtraining::TrainingGuardTimingConfig g_trainingGuardTimingConfig{};
static qtraining::TrainingGuardTimingState g_trainingGuardTimingState{};
static bool g_trainingGuardTimingArmed = true;
static bool g_trainingGuardTimingPendingHit = false;
static bool g_trainingGuardTimingPendingBlock = false;
static qtraining::GuardDecision g_trainingLastGuardDecision{};
static uint32_t g_trainingLastEmittedDummyDirections = 0u;
static uint32_t g_trainingLastEmittedOwnedAxes = 0u;
static DummyStance g_trainingDummyStance = DummyStance::Stand;
static DummyTech g_trainingDummyTech = DummyTech::Off;
static bool g_trainingGuardDuringPlayback = false;
static uint8_t g_trainingGuardPermission = 0;
static GuardClassification g_trainingGuardClassification =
    GuardClassification::Unguardable;

static uint32_t g_trainingGuardPostureDivergences = 0u;
static uint32_t g_trainingGuardAmbiguousAttackers = 0u;
static uint32_t g_trainingGuardUnguardablePostures = 0u;

static qtraining::TrainingRecoveryController g_trainingRecovery;
static qtraining::RecoverySettings g_trainingRecoverySettings;
static qtraining::RecoveryEmission g_trainingRecoveryEmission{};
static uint32_t g_trainingRecoveryReadoutValid = 0u;
static uint32_t g_trainingRecoveryLockSample = 0u;
static uint8_t g_trainingRecoveryArmable = 0u;

static qtraining::TrainingRecoveryController g_trainingRecoveryP1;
static qtraining::RecoveryEmission g_trainingRecoveryEmissionP1{};

static uint32_t g_trainingHitEpoch[2] = {};

static qtraining::TrainingMacroAfterReset g_trainingMacroAfterReset =
    qtraining::TrainingMacroAfterReset::Off;
static qtraining::TrainingMacroAfterReset g_trainingMacroAutoStartPending =
    qtraining::TrainingMacroAfterReset::Off;

static void trainingArmMacroAfterReset() {
    g_trainingMacroAutoStartPending = g_trainingMacroAfterReset;
}

static bool g_trainingRecoveryEvaluatorQualified = false;
static qtraining::RecoveryEvaluatorReason g_trainingRecoveryEvaluatorReason =
    qtraining::RecoveryEvaluatorReason::NotAttempted;
static uint32_t g_trainingRecoveryEvaluatorCompared = 0u;

static qtraining::TrainingTriggerController g_trainingTriggers;
static qtraining::FrameAdvantageTracker g_trainingFrameAdvantage;
constexpr uint32_t kTrainingContactCapacity = 64u;
static qtraining::CombatContactEvent
    g_trainingContacts[kTrainingContactCapacity] = {};
static uint32_t g_trainingContactCount = 0u;
static uint64_t g_trainingContactSerial = 0u;
static uint32_t g_trainingContactDrops = 0u;
static uint32_t g_trainingCombatToolsReadoutValid = 0u;
static bool g_trainingGuardCancelResponsePlayback = false;
static bool g_trainingDebugOverlayEnabled = false;

static void trainingStateLogSetEnabled(bool enabled);
static bool trainingStateLogRecording();
static bool trainingStateLogFailed();
static uint32_t trainingStateLogRecordCount();
static void trainingStateLogObserve(
    const qtraining::CombatFrameSample& sample,
    const qtraining::TrainingTriggerDiagnostics& diagnostics);

struct TrainingResponseWatch {
    bool active = false;
    uint32_t event = 0u;
    uint16_t moveIndex = 0u;
    uint8_t macroSlot = 0u;
    uint16_t expectedAction = qtraining::kTrainingMoveUnknownAction;
    char input[24] = {};
    uint64_t startStep = 0u;
    uint32_t startAction = 0u;
    uint64_t actorToken = 0u;
    uint32_t generation = 0u;
    bool prebuffered = false;
    uint32_t guardYieldSteps = 0u;
    uint32_t guardYieldBase = 0u;
};
static TrainingResponseWatch g_trainingResponseWatch{};

static uint32_t g_trainingGuardYieldSteps = 0u;

static void trainingResponseWatchArm(uint32_t event, uint16_t moveIndex,
                                     uint8_t macroSlot,
                                     uint16_t expectedAction,
                                     const char* input, std::size_t inputLen,
                                     const qtraining::CombatFrameSample& sample,
                                     bool prebuffered);
static void trainingResponseWatchObserve(
    const qtraining::CombatFrameSample& sample);
static void trainingResponseWatchCancel(const char* reason);

static void trainingFrameTraceSetEnabled(bool enabled);
static bool trainingFrameTraceRecording();
static bool trainingFrameTraceFailed();
static bool trainingFrameTraceCapped();
static uint32_t trainingFrameTraceRowCount();
static void trainingFrameTraceObserve();

struct TrainingWakeupReversalRuntime {
    bool active = false;
    bool released = false;
    bool releasedEarly = false;
    qtraining::TrainingTriggerEvent event =
        qtraining::TrainingTriggerEvent::OnWakeup;
    uintptr_t fighter = 0u;
    uint8_t character = 0xFFu;
    uint16_t moveIndex = 0u;
    uint32_t generation = 0u;
    uint64_t actorToken = 0u;
    uint64_t actionEpoch = 0u;
    uint32_t resourceAction = 0u;
    uint32_t dispatchAction = 0u;
    uint32_t commitIndex = 0u;
    uint32_t lockedStepsRemaining = 0u;
    uint64_t predictedStartStep = 0u;
    int32_t appliedLead = 0;
    bool episodeAttempted = false;
    uint64_t episodeToken = 0u;
    qtraining::TrainingWakeupReversalPhase phase =
        qtraining::TrainingWakeupReversalPhase::Idle;
    qtraining::TriggerRequestOutcome lastOutcome =
        qtraining::TriggerRequestOutcome::None;
};
static TrainingWakeupReversalRuntime g_trainingWakeupReversal{};
static bool g_trainingWakeupEpisodeOpen = false;
static bool g_trainingWakeupEpisodePrimed = false;

struct TrainingNaturalTerminalPulse {
    bool valid = false;
    uintptr_t fighter = 0u;
    uint8_t character = 0xFFu;
    uint32_t resourceAction = 0u;
    uint32_t dispatchAction = 0u;
    uint64_t actorToken = 0u;
    uint64_t actionEpoch = 0u;
    uint64_t stepSerial = 0u;
};
struct TrainingNaturalRecoveryExpectation {
    bool active = false;
    uintptr_t fighter = 0u;
    uint8_t character = 0xFFu;
    uint32_t resourceAction = 0u;
    uint32_t dispatchAction = 0u;
    uint32_t lastLocalFrame = 0u;
    uint64_t actorToken = 0u;
    uint64_t actionEpoch = 0u;
    qtraining::CombatContactKind contactKind =
        qtraining::CombatContactKind::None;
};

struct TrainingCombatActorIdentity {
    bool valid = false;
    uintptr_t fighter = 0u;
    uint8_t character = 0xFFu;
    uint32_t resourceAction = 0u;
    uint32_t dispatchAction = 0u;
    uint32_t localFrame = 0u;
    uint32_t handlerLifecycle = 0u;
    uint64_t actorToken = 0u;
    uint64_t actionEpoch = 0u;
};

struct TrainingPreHandlerAction {
    bool valid = false;
    uintptr_t fighter = 0u;
    uint8_t character = 0xFFu;
    uint32_t resourceAction = 0u;
    uint32_t dispatchAction = 0u;
    uint32_t localFrame = 0u;
    uint64_t actorToken = 0u;
    uint64_t actionEpoch = 0u;
    uint64_t stepSerial = 0u;
};
static TrainingNaturalTerminalPulse g_trainingNaturalTerminal[2] = {};
static TrainingNaturalRecoveryExpectation
    g_trainingNaturalExpectation[2] = {};
static TrainingCombatActorIdentity g_trainingCombatIdentity[2] = {};
static TrainingPreHandlerAction g_trainingPreHandlerAction[2] = {};
static uint64_t g_trainingCombatActorTokenSequence = 0u;
static uint64_t g_trainingCombatActionEpochSequence = 0u;

struct TrainingThrowReceptionObservation {
    bool valid = false;
    uintptr_t fighter = 0u;
    bool inExactReception = false;
};
static TrainingThrowReceptionObservation
    g_trainingThrowReceptionObservation[2] = {};

struct TrainingPendingThrowContact {
    bool valid = false;
    uintptr_t attacker = 0u;
    uintptr_t victim = 0u;
    uint64_t stepSerial = 0u;
};
static TrainingPendingThrowContact g_trainingPendingThrowContact[2] = {};

struct TrainingThrowTechTelemetry {
    uint32_t chancePercent = 0u;
    uint32_t opportunities = 0u;
    uint32_t nativeAccepted = 0u;
    uint32_t automaticAccepted = 0u;
    uint32_t resultObserved = 0u;
    uint64_t lastDecisionStep = 0u;
    uintptr_t lastDecisionAttacker = 0u;
    uintptr_t lastDecisionFighter = 0u;
    bool haveDecision = false;
    bool automaticDecision = false;
    bool nativeAcceptedDecision = false;
    uint32_t previousDummyAction = 0u;
    bool haveDummyAction = false;
};
static TrainingThrowTechTelemetry g_trainingThrowTech{};

static void trainingResetRecoverySettings() {
    g_trainingRecoverySettings = qtraining::RecoverySettings{};
    g_trainingRecoverySettings.evaluatorQualified =
        g_trainingRecoveryEvaluatorQualified;
}

static void trainingResetCombatToolSettings() {
    if (g_trainingWakeupReversal.active)
        (void)g_trainingMacros.requestStop();
    g_trainingTriggers.reset();
    g_trainingTriggers.resetTimeline(g_trainingGeneration);
    g_trainingFrameAdvantage.reset();
    g_trainingFrameAdvantage.resetTimeline(g_trainingGeneration);
    g_trainingThrowTech = TrainingThrowTechTelemetry{};
    g_trainingGuardCancelResponsePlayback = false;
    g_trainingDebugOverlayEnabled = false;
    g_trainingWakeupReversal = TrainingWakeupReversalRuntime{};
}

static void trainingResetCombatToolTimeline(
        bool preserveObservationCounters = false) {
    if (g_trainingWakeupReversal.active)
        (void)g_trainingMacros.requestStop();
    g_trainingTriggers.resetTimeline(
        g_trainingGeneration, preserveObservationCounters);
    g_trainingFrameAdvantage.resetTimeline(
        g_trainingGeneration, preserveObservationCounters);
    g_trainingContactCount = 0u;
    g_trainingContactDrops = 0u;
    g_trainingCombatToolsReadoutValid = 0u;
    g_trainingGuardCancelResponsePlayback = false;
    g_trainingWakeupReversal = TrainingWakeupReversalRuntime{};
    g_trainingNaturalTerminal[0] = TrainingNaturalTerminalPulse{};
    g_trainingNaturalTerminal[1] = TrainingNaturalTerminalPulse{};
    g_trainingNaturalExpectation[0] =
        TrainingNaturalRecoveryExpectation{};
    g_trainingNaturalExpectation[1] =
        TrainingNaturalRecoveryExpectation{};
    g_trainingCombatIdentity[0] = TrainingCombatActorIdentity{};
    g_trainingCombatIdentity[1] = TrainingCombatActorIdentity{};
    g_trainingPreHandlerAction[0] = TrainingPreHandlerAction{};
    g_trainingPreHandlerAction[1] = TrainingPreHandlerAction{};
    g_trainingThrowReceptionObservation[0] =
        TrainingThrowReceptionObservation{};
    g_trainingThrowReceptionObservation[1] =
        TrainingThrowReceptionObservation{};
    g_trainingPendingThrowContact[0] = TrainingPendingThrowContact{};
    g_trainingPendingThrowContact[1] = TrainingPendingThrowContact{};
    g_trainingCombatActorTokenSequence = 0u;
    g_trainingCombatActionEpochSequence = 0u;
    g_trainingThrowTech.haveDecision = false;
    g_trainingThrowTech.automaticDecision = false;
    g_trainingThrowTech.lastDecisionStep = 0u;
    g_trainingThrowTech.lastDecisionAttacker = 0u;
    g_trainingThrowTech.lastDecisionFighter = 0u;
    g_trainingThrowTech.nativeAcceptedDecision = false;
    g_trainingThrowTech.haveDummyAction = false;
    g_trainingThrowTech.previousDummyAction = 0u;
}

static qtraining::ResourcePolicySet g_trainingResourcePolicies{};

static bool trainingRestoreAllPresetActive() {
    const qtraining::ResourcePolicySettings* const policies[] = {
        &g_trainingResourcePolicies.health,
        &g_trainingResourcePolicies.ex,
        &g_trainingResourcePolicies.guardGauge,
    };
    for (const qtraining::ResourcePolicySettings* policy : policies) {
        if (policy->trigger != qtraining::ResourceTrigger::WhenNeutral
            || policy->target != qtraining::TrainingRefillTarget::Both) {
            return false;
        }
    }
    return true;
}

static qtraining::NeutralTrigger g_trainingNeutralTrigger;
static qtraining::IdleStateTrigger g_trainingExactIdleTrigger;
static qtraining::ComboEpisodeMachine
    g_trainingComboEpisode[qtraining::kTrainingRefillPlayerCount];
static qtraining::RecoveryExitTrigger
    g_trainingRecoveryExit[qtraining::kTrainingRefillPlayerCount];
static uint64_t g_trainingResourcePolicyStepSerial = 0u;
static uint32_t g_trainingResourcePolicyFires = 0u;
static uint32_t g_trainingResourcePolicyRejects = 0u;
static uint32_t g_trainingResourcePolicyValid = 0u;
static uint32_t g_trainingBothFightersNeutral = 0u;
static uint32_t g_trainingNeutralHoldSteps = 0u;
static uint32_t g_trainingNeutralReasonMask[
    qtraining::kTrainingRefillPlayerCount] = {};
static uint32_t g_trainingNeutralGlobalReasonMask = 0u;
static uint32_t g_trainingRecoveryLockReadout[
    qtraining::kTrainingRefillPlayerCount] = {};
static uint32_t g_trainingRecoveryPhaseReadout[
    qtraining::kTrainingRefillPlayerCount] = {};
static uint32_t g_trainingComboHitsReadout[
    qtraining::kTrainingRefillPlayerCount] = {};
static uint32_t g_trainingAuthoredFlagsReadout[
    qtraining::kTrainingRefillPlayerCount] = {};
static uint32_t g_trainingWallFlagsReadout[
    qtraining::kTrainingRefillPlayerCount] = {};
static uint32_t g_trainingTerminalReadyReadout[
    qtraining::kTrainingRefillPlayerCount] = {};
static uint32_t g_trainingDispatchActionReadout[
    qtraining::kTrainingRefillPlayerCount] = {};
static uint32_t g_trainingInitStatusReadout[
    qtraining::kTrainingRefillPlayerCount] = {};

static bool g_trainingRoundStartNeutralPending = false;
static uint32_t g_trainingRoundStartNeutralTargetMask = 0u;
static uint64_t g_trainingRoundStartNeutralStepSerial = 0u;
static uint32_t g_trainingRoundStartNeutralMask = 0u;
static uint32_t g_trainingRoundStartNeutralValid = 0u;
static bool g_trainingLastPositionPlanValid = false;
static int32_t g_trainingLastPositionPlanX[
    qtraining::kTrainingRefillPlayerCount] = {};
static uint8_t g_trainingLastPositionPlanFacing[
    qtraining::kTrainingRefillPlayerCount] = {};

constexpr uint32_t kTrainingRoundCharacterActorCapacity = 4u;
constexpr uint32_t kTrainingRoundCharacterRegionCapacity = 8u;
constexpr uint32_t kTrainingRoundCharacterFrameCountMax = 0x2000u;
constexpr size_t kTrainingRoundCharacterCommandSize = 0x200u;
constexpr uintptr_t kTrainingRoundCharacterSidecarBase = 0x625680u;
constexpr size_t kTrainingRoundCharacterSidecarSize = 0x3D0u;
static_assert(kTrainingCommandDirectionKillOffset + sizeof(uint32_t)
                  <= kTrainingRoundCharacterCommandSize,
              "direction-kill latch must fit the captured command child");

struct TrainingRoundCharacterRegion {
    uintptr_t address = 0u;
    std::vector<uint8_t> bytes;
};

struct TrainingRoundCharacterState {
    bool valid = false;
    uint32_t generation = 0u;
    uintptr_t active[qtraining::kTrainingRefillPlayerCount] = {};
    uintptr_t alternate[qtraining::kTrainingRefillPlayerCount] = {};
    uintptr_t actorAddress[kTrainingRoundCharacterActorCapacity] = {};
    uint8_t actorBytes[kTrainingRoundCharacterActorCapacity][addr::kFighterSize]
        = {};
    uint32_t actorCount = 0u;
    uint8_t sidecarBytes[qtraining::kTrainingRefillPlayerCount]
                        [kTrainingRoundCharacterSidecarSize] = {};
    TrainingRoundCharacterRegion
        regions[kTrainingRoundCharacterRegionCapacity] = {};
    uint32_t regionCount = 0u;
};

static TrainingRoundCharacterState g_trainingRoundCharacterState{};
static uint32_t g_trainingStatusEventKind = 0u;
static uint32_t g_trainingStatusEventDetail = 0u;
static uint32_t g_trainingStatusEventSequence = 0u;

static void trainingRaiseStatusEvent(qtraining::StatusEvent event,
                                     uint32_t detail) {
    g_trainingStatusEventKind = static_cast<uint32_t>(event);
    g_trainingStatusEventDetail = detail;
    ++g_trainingStatusEventSequence;
}

static uint32_t g_trainingFinalInput[2] = {};
struct TrainingGuardAwayAtRead {
    bool armed = false;
    uint8_t lane = 0u;
    uint32_t awayBit = 0u;
};
static TrainingGuardAwayAtRead g_trainingGuardAwayRead{};
static uint32_t g_trainingGuardTurnedAtRead = 0u;
static uint64_t g_trainingGuardTurnedAtReadStep = 0u;
static uint32_t g_trainingLastComposedInput[2] = {};
static volatile LONG g_trainingMenuCaptured = 0;
static volatile LONG g_trainingUiGeneration = 0;
static volatile uint32_t g_trainingLiveE2ELogicalMask = 0u;
static bool g_trainingLiveE2EEnabled = false;

struct TrainingLiveE2EMailboxV1 {
    volatile uint32_t commandKind = 0u;
    volatile uint32_t slotOrSetting = 0u;
    volatile uint32_t value = 0u;
    volatile uint32_t flags = 0u;
    volatile uint32_t gameplayP1Mask = 0u;
    volatile uint32_t requestId = 0u;

    volatile uint32_t ingressRequestId = 0u;
    volatile uint32_t ingressResult = 0u;
    volatile uint32_t acknowledgedCommandId = 0u;
    volatile uint32_t acknowledgedResult = 0u;
    volatile uint32_t trainingGeneration = 0u;
    volatile uint32_t lifecycleState = 0u;
    volatile uint32_t finalP1Mask = 0u;
    volatile uint32_t finalP2Mask = 0u;
    volatile uint32_t macroState = 0u;
    volatile uint32_t macroCursor = 0u;
    volatile uint32_t selectedMacroFrameCount = 0u;
    volatile uint32_t hitboxEffective = 0u;
    volatile uint32_t stepSerialLow = 0u;
    volatile uint32_t stepSerialHigh = 0u;
    volatile uint32_t ingressGeneration = 0u;
    volatile uint32_t lethalDamageRequestId = 0u;
    volatile uint32_t lethalDamageAckId = 0u;
    volatile uint32_t lethalDamageAckResult = 0u;
    volatile uint32_t lethalDamageAmount = 0u;
    volatile uint32_t lethalDamageDetourCount = 0u;
    volatile uint32_t lethalDamageGeneration = 0u;
    volatile uint32_t lethalDamageVictimSlot = 0u;
};
static TrainingLiveE2EMailboxV1 g_trainingLiveE2EMailbox{};
static_assert(sizeof(TrainingLiveE2EMailboxV1) == 112u,
              "live training E2E mailbox ABI changed");
static_assert(offsetof(TrainingLiveE2EMailboxV1, requestId) == 20u
                  && offsetof(TrainingLiveE2EMailboxV1, finalP1Mask) == 48u
                  && offsetof(TrainingLiveE2EMailboxV1,
                              lethalDamageRequestId) == 84u
                  && offsetof(TrainingLiveE2EMailboxV1,
                              lethalDamageVictimSlot) == 108u,
              "live training E2E mailbox offsets changed");

static volatile LONG g_trainingFrontendInputArmed = 0;
static volatile LONG g_trainingFrontendCaptureGeneration = 0;
static qgame::directboot::TrainingFrontendInputGatePolicy
    g_trainingFrontendInputGate;
static volatile LONG g_trainingStageInputArmed = 0;
static qgame::directboot::TrainingStageInputGatePolicy
    g_trainingStageInputGate;
static bool g_trainingStepPending = false;
static bool g_trainingStepFailure = false;
static bool g_trainingMutationFailed = false;
static bool g_trainingReturningToSelect = false;
static bool g_trainingOperationNeutral = false;
static bool g_trainingOperationNeutralForStep = false;
static qtraining::HitboxDisplayMode g_trainingHitboxMode =
    qtraining::HitboxDisplayMode::Off;
static qtraining::HitboxDisplayMode g_trainingHitboxRenderer =
    qtraining::kHitboxDefaultRenderer;
static bool g_trainingHitboxOverlayVisible = false;
static void trainingPublishHitboxSnapshot(bool fromRenderPhase = false);
static bool g_trainingHitboxOriginalCaptured = false;
static uint8_t g_trainingHitboxOriginal = 0;
static uint32_t g_trainingLastAckId = 0;
static CommandAckResult g_trainingLastAckResult =
    CommandAckResult::None;
static StepAdmissionResult g_trainingLastAdmission =
    StepAdmissionResult::None;
static uint32_t g_trainingLastNativeReturn = 0;
static uint32_t g_trainingStickyErrors = 0;
static TrainingDisableReason g_trainingDisableReason =
    TrainingDisableReason::NotTrainingLaunch;
static TrainingLifecycleState g_trainingLifecycle =
    TrainingLifecycleState::Off;
static bool g_trainingHadActiveMatch = false;
static uint32_t g_trainingLastScene = UINT32_MAX;
static uint32_t g_trainingLastMatchSub = UINT32_MAX;
static char g_trainingDisableText[96] = {};
static char g_trainingStatusText[96] = {};
static char g_trainingResultText[96] = {};

static bool trainingLiveE2EEnvironmentEnabled() {
    char value[8] = {};
    const DWORD length = GetEnvironmentVariableA(
        "QOH99_TRAINING_LIVE_E2E", value,
        static_cast<DWORD>(sizeof(value)));
    return qgame::directboot::trainingLiveE2EEnvironmentValueExact(
        value, static_cast<uint32_t>(length));
}

static bool trainingLiveE2EOfflineControlEnabled() {
    return g_trainingLiveE2EEnabled
        && g_trainingLaunchRequested
        && !g_netActive
        && g_mode == RunMode::Observe;
}

static void trainingLiveE2EClearRequestControls() {
    g_trainingLiveE2EMailbox.requestId = 0u;
    MemoryBarrier();
    g_trainingLiveE2EMailbox.commandKind = 0u;
    g_trainingLiveE2EMailbox.slotOrSetting = 0u;
    g_trainingLiveE2EMailbox.value = 0u;
    g_trainingLiveE2EMailbox.flags = 0u;
}

static void trainingLiveE2EClearDamageRequest() {
    g_trainingLiveE2EMailbox.lethalDamageRequestId = 0u;
}

static void trainingLiveE2EResetMailbox() {
    trainingLiveE2EClearRequestControls();
    trainingLiveE2EClearDamageRequest();
    g_trainingLiveE2EMailbox.gameplayP1Mask = 0u;
    g_trainingLiveE2EMailbox.ingressRequestId = 0u;
    g_trainingLiveE2EMailbox.ingressResult = 0u;
    g_trainingLiveE2EMailbox.acknowledgedCommandId = 0u;
    g_trainingLiveE2EMailbox.acknowledgedResult = 0u;
    g_trainingLiveE2EMailbox.trainingGeneration = 0u;
    g_trainingLiveE2EMailbox.lifecycleState = 0u;
    g_trainingLiveE2EMailbox.finalP1Mask = 0u;
    g_trainingLiveE2EMailbox.finalP2Mask = 0u;
    g_trainingLiveE2EMailbox.macroState = 0u;
    g_trainingLiveE2EMailbox.macroCursor = 0u;
    g_trainingLiveE2EMailbox.selectedMacroFrameCount = 0u;
    g_trainingLiveE2EMailbox.hitboxEffective = 0u;
    g_trainingLiveE2EMailbox.stepSerialLow = 0u;
    g_trainingLiveE2EMailbox.stepSerialHigh = 0u;
    g_trainingLiveE2EMailbox.ingressGeneration = 0u;
    g_trainingLiveE2EMailbox.lethalDamageAckId = 0u;
    g_trainingLiveE2EMailbox.lethalDamageAckResult = 0u;
    g_trainingLiveE2EMailbox.lethalDamageAmount = 0u;
    g_trainingLiveE2EMailbox.lethalDamageDetourCount = 0u;
    g_trainingLiveE2EMailbox.lethalDamageGeneration = 0u;
    g_trainingLiveE2EMailbox.lethalDamageVictimSlot = 0u;
}

static void trainingLiveE2EPollCommand(
    uint32_t scene, uint32_t matchSub) {
    const bool gameplay =
        scene == 2u && matchSub == 0u
        && !g_trainingReturningToSelect;
    if (!trainingLiveE2EOfflineControlEnabled() || !gameplay) {
        trainingLiveE2EClearRequestControls();
        trainingLiveE2EClearDamageRequest();
        g_trainingLiveE2EMailbox.gameplayP1Mask = 0u;
        return;
    }

    const uint32_t requestId =
        g_trainingLiveE2EMailbox.requestId;
    if (requestId == 0u
        || (requestId
                == g_trainingLiveE2EMailbox.ingressRequestId
            && g_trainingGeneration
                == g_trainingLiveE2EMailbox.ingressGeneration)) {
        return;
    }

    MemoryBarrier();
    TrainingCommandV1 command{};
    command.trainingGeneration = g_trainingGeneration;
    command.commandId = requestId;
    command.commandKind =
        g_trainingLiveE2EMailbox.commandKind;
    command.slotOrSetting =
        g_trainingLiveE2EMailbox.slotOrSetting;
    command.value = g_trainingLiveE2EMailbox.value;
    command.flags = g_trainingLiveE2EMailbox.flags;
    const qtraining::CommandIngressResult result =
        g_trainingCommands.tryEnqueue(
            &command, sizeof(command));

    g_trainingLiveE2EMailbox.ingressResult =
        static_cast<uint32_t>(result);
    g_trainingLiveE2EMailbox.ingressGeneration =
        g_trainingGeneration;
    MemoryBarrier();
    g_trainingLiveE2EMailbox.ingressRequestId = requestId;
    MemoryBarrier();
    trainingLiveE2EClearRequestControls();
    QLOG("[training-live-e2e] request=%u kind=%u ingress=%u "
         "generation=%u",
         requestId, command.commandKind,
         static_cast<unsigned>(result),
         command.trainingGeneration);
}

static void trainingLiveE2EMirrorStatus(
    uint32_t scene, uint32_t matchSub) {
    if (!trainingLiveE2EOfflineControlEnabled()) {
        trainingLiveE2EResetMailbox();
        return;
    }
    g_trainingLiveE2EMailbox.acknowledgedCommandId =
        g_trainingLastAckId;
    g_trainingLiveE2EMailbox.acknowledgedResult =
        static_cast<uint32_t>(g_trainingLastAckResult);
    g_trainingLiveE2EMailbox.trainingGeneration =
        g_trainingGeneration;
    g_trainingLiveE2EMailbox.lifecycleState =
        static_cast<uint32_t>(g_trainingLifecycle);
    const uint64_t serial = g_trainingStepSerial;
    g_trainingLiveE2EMailbox.stepSerialLow =
        static_cast<uint32_t>(serial);
    g_trainingLiveE2EMailbox.stepSerialHigh =
        static_cast<uint32_t>(serial >> 32u);

    if (scene != 2u || matchSub != 0u) {
        g_trainingLiveE2EMailbox.finalP1Mask = 0u;
        g_trainingLiveE2EMailbox.finalP2Mask = 0u;
        g_trainingLiveE2EMailbox.macroState = 0u;
        g_trainingLiveE2EMailbox.macroCursor = 0u;
        g_trainingLiveE2EMailbox.selectedMacroFrameCount = 0u;
        g_trainingLiveE2EMailbox.hitboxEffective = 0u;
        return;
    }
    g_trainingLiveE2EMailbox.finalP1Mask =
        g_trainingLastComposedInput[0];
    g_trainingLiveE2EMailbox.finalP2Mask =
        g_trainingLastComposedInput[1];
    g_trainingLiveE2EMailbox.macroState =
        static_cast<uint32_t>(g_trainingMacros.state());
    g_trainingLiveE2EMailbox.macroCursor =
        g_trainingMacros.cursor();
    g_trainingLiveE2EMailbox.selectedMacroFrameCount =
        g_trainingMacros.selectedFrameCount();
    g_trainingLiveE2EMailbox.hitboxEffective =
        rd8(kTrainingHitboxAddr) != 0u ? 1u : 0u;
}

static void trainingResetFrontendInputGate() {
    g_trainingFrontendInputGate.reset();
    InterlockedExchange(&g_trainingFrontendInputArmed, 0);
    g_trainingStageInputGate.reset();
    InterlockedExchange(&g_trainingStageInputArmed, 0);
    InterlockedExchange(
        &g_trainingFrontendCaptureGeneration, 0);
    g_trainingLiveE2ELogicalMask = 0u;
}

static void trainingCopyText(char (&dst)[96], const char* text) {
    std::strncpy(dst, text ? text : "", sizeof(dst) - 1u);
    dst[sizeof(dst) - 1u] = '\0';
}

static uint32_t trainingDummySlot() {
    return g_trainingPlayerSwap ? 0u : 1u;
}
static uint32_t trainingHumanSlot() {
    return g_trainingPlayerSwap ? 1u : 0u;
}

static uint32_t trainingPolicySlot() {
    return g_trainingPolicyFollowsHuman
        ? trainingHumanSlot() : trainingDummySlot();
}
static uint32_t trainingPolicyPressureSlot() {
    return g_trainingPolicyFollowsHuman
        ? trainingDummySlot() : trainingHumanSlot();
}

static uintptr_t trainingFighter(uint32_t player, bool mirror) {
    if (player >= 4u) return 0;
    const uintptr_t table = mirror
        ? addr::kFighterBackupTable : addr::kFighterPtrTable;
    const uintptr_t fighter = rd32(table + player * 4u);
    return rangeReadable(fighter, addr::kFighterSize) ? fighter : 0;
}

static void trainingInvalidateRoundCharacterState() {
    g_trainingRoundCharacterState.valid = false;
    g_trainingRoundCharacterState.generation = 0u;
    g_trainingRoundCharacterState.actorCount = 0u;
    g_trainingRoundCharacterState.regionCount = 0u;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        g_trainingRoundCharacterState.active[player] = 0u;
        g_trainingRoundCharacterState.alternate[player] = 0u;
    }
    for (TrainingRoundCharacterRegion& region
         : g_trainingRoundCharacterState.regions) {
        region.address = 0u;
        region.bytes.clear();
    }
}

static bool trainingRoundCharacterCaptureReady() {
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        const uintptr_t fighter = trainingFighter(player, false);
        if (!fighter
            || rd8(fighter + kTrainingFighterCharacterOffset) == 0xFFu
            || rd32(fighter + kTrainingFighterActionStateOffset) != 0u) {
            return false;
        }
    }
    return true;
}

static bool trainingCaptureRoundCharacterRegion(
        uintptr_t address, size_t size) {
    for (uint32_t index = 0;
         index < g_trainingRoundCharacterState.regionCount; ++index) {
        const TrainingRoundCharacterRegion& saved =
            g_trainingRoundCharacterState.regions[index];
        if (saved.address == address)
            return saved.bytes.size() == size;
    }
    if (g_trainingRoundCharacterState.regionCount
            >= kTrainingRoundCharacterRegionCapacity
        || !rangeReadable(address, size)) {
        return false;
    }
    TrainingRoundCharacterRegion& saved =
        g_trainingRoundCharacterState.regions[
            g_trainingRoundCharacterState.regionCount];
    try {
        saved.bytes.resize(size);
    } catch (const std::bad_alloc&) {
        return false;
    }
    saved.address = address;
    std::memcpy(saved.bytes.data(), reinterpret_cast<const void*>(address), size);
    ++g_trainingRoundCharacterState.regionCount;
    return true;
}

static bool trainingCaptureRoundCharacterActor(uintptr_t actor) {
    for (uint32_t index = 0;
         index < g_trainingRoundCharacterState.actorCount; ++index) {
        if (g_trainingRoundCharacterState.actorAddress[index] == actor)
            return true;
    }
    if (g_trainingRoundCharacterState.actorCount
            >= kTrainingRoundCharacterActorCapacity
        || !rangeReadable(actor, addr::kFighterSize)) {
        return false;
    }
    const uint32_t index = g_trainingRoundCharacterState.actorCount;
    g_trainingRoundCharacterState.actorAddress[index] = actor;
    std::memcpy(g_trainingRoundCharacterState.actorBytes[index],
                reinterpret_cast<const void*>(actor), addr::kFighterSize);
    ++g_trainingRoundCharacterState.actorCount;

    if (rd8(actor + kTrainingFighterCharacterOffset) == 0xFFu) return true;

    const uintptr_t command = static_cast<uintptr_t>(
        rd32(actor + kTrainingFighterCommandChildOffset));
    if (!trainingCaptureRoundCharacterRegion(
            command, kTrainingRoundCharacterCommandSize))
        return false;
    for (uint32_t region = 0;
         region < g_trainingRoundCharacterState.regionCount; ++region) {
        TrainingRoundCharacterRegion& saved =
            g_trainingRoundCharacterState.regions[region];
        if (saved.address != command) continue;
        std::memset(saved.bytes.data() + kTrainingCommandDirectionKillOffset,
                    0, sizeof(uint32_t));
        break;
    }

    const uint32_t frameCount =
        rd32(actor + kTrainingFighterFrameCountOffset);
    const uintptr_t frameData = static_cast<uintptr_t>(
        rd32(actor + kTrainingFighterFrameTableOffset));
    if (frameCount == 0u
        || frameCount > kTrainingRoundCharacterFrameCountMax) {
        return false;
    }
    const size_t frameBytes = static_cast<size_t>(frameCount) << 6u;
    return trainingCaptureRoundCharacterRegion(frameData, frameBytes);
}

static bool trainingCaptureRoundCharacterState() {
    if (g_trainingRoundCharacterState.valid) return true;
    if (!trainingRoundCharacterCaptureReady()) return true;

    trainingInvalidateRoundCharacterState();
    g_trainingRoundCharacterState.generation = g_trainingGeneration;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        const uintptr_t active = trainingFighter(player, false);
        const uintptr_t alternate = trainingFighter(player, true);
        if (!active || !alternate) {
            trainingInvalidateRoundCharacterState();
            return false;
        }
        g_trainingRoundCharacterState.active[player] = active;
        g_trainingRoundCharacterState.alternate[player] = alternate;
        if (!trainingCaptureRoundCharacterActor(active)
            || !trainingCaptureRoundCharacterActor(alternate)) {
            trainingInvalidateRoundCharacterState();
            return false;
        }
    }
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        const uintptr_t sidecar = kTrainingRoundCharacterSidecarBase
            + static_cast<uintptr_t>(player)
                * kTrainingRoundCharacterSidecarSize;
        std::memcpy(g_trainingRoundCharacterState.sidecarBytes[player],
                    reinterpret_cast<const void*>(sidecar),
                    kTrainingRoundCharacterSidecarSize);
    }
    g_trainingRoundCharacterState.valid = true;
    QLOG("[training-reset] captured round idle character image "
         "actors=%u children=%u generation=%u",
         g_trainingRoundCharacterState.actorCount,
         g_trainingRoundCharacterState.regionCount,
         g_trainingRoundCharacterState.generation);
    return true;
}

static bool trainingRoundCharacterStateRestorable() {
    const TrainingRoundCharacterState& saved =
        g_trainingRoundCharacterState;
    if (!saved.valid || saved.generation != g_trainingGeneration)
        return false;

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        const uintptr_t active = rd32(addr::kFighterPtrTable + player * 4u);
        const uintptr_t alternate =
            rd32(addr::kFighterBackupTable + player * 4u);
        const bool originalOrder = active == saved.active[player]
            && alternate == saved.alternate[player];
        const bool swappedOrder = active == saved.alternate[player]
            && alternate == saved.active[player];
        if (!originalOrder && !swappedOrder) return false;
    }
    return true;
}

static void trainingRestoreRoundCharacterActors() {
    const TrainingRoundCharacterState& saved =
        g_trainingRoundCharacterState;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        wr32(addr::kFighterPtrTable + player * 4u,
             static_cast<uint32_t>(saved.active[player]));
        wr32(addr::kFighterBackupTable + player * 4u,
             static_cast<uint32_t>(saved.alternate[player]));
    }
    for (uint32_t index = 0; index < saved.actorCount; ++index) {
        std::memcpy(reinterpret_cast<void*>(saved.actorAddress[index]),
                    saved.actorBytes[index], addr::kFighterSize);
    }
}

static void trainingRestoreRoundCharacterRegions() {
    const TrainingRoundCharacterState& saved =
        g_trainingRoundCharacterState;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        const uintptr_t sidecar = kTrainingRoundCharacterSidecarBase
            + static_cast<uintptr_t>(player)
                * kTrainingRoundCharacterSidecarSize;
        std::memcpy(reinterpret_cast<void*>(sidecar),
                    saved.sidecarBytes[player],
                    kTrainingRoundCharacterSidecarSize);
    }
    for (uint32_t index = 0; index < saved.regionCount; ++index) {
        const TrainingRoundCharacterRegion& region = saved.regions[index];
        std::memcpy(reinterpret_cast<void*>(region.address),
                    region.bytes.data(), region.bytes.size());
    }
}

static uintptr_t trainingAlternateFormActor(uint32_t player) {
    const uintptr_t actor = trainingFighter(player, true);
    if (!actor) return 0;
    return rd8(actor + kTrainingFighterCharacterOffset) != 0xFFu
        ? actor : 0;
}

static bool trainingActorOwner(uintptr_t actor, uint8_t& owner) {
    owner = 0u;
    if (!rangeReadable(actor, kTrainingFighterExOwnerOffset + 1u))
        return false;
    const uint8_t value = rd8(actor + kTrainingFighterExOwnerOffset);
    if (value > 1u) return false;
    owner = value;
    return true;
}

static uint64_t trainingNextNonzeroSequence(uint64_t& sequence) {
    ++sequence;
    if (sequence == 0u) ++sequence;
    return sequence;
}

static bool trainingCombatFrameReadable(uintptr_t fighter) {
    if (!fighter) return false;
    const uintptr_t frame = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterCurrentFrameOffset));
    return frame != 0u
        && rangeReadable(frame, kTrainingFighterFrameStride);
}

static TrainingCombatActorIdentity& trainingObserveCombatIdentity(
        uint8_t owner, uintptr_t fighter) {
    TrainingCombatActorIdentity& identity =
        g_trainingCombatIdentity[owner];
    const uint8_t character =
        rd8(fighter + kTrainingFighterCharacterOffset);
    const uint32_t resourceAction =
        rd32(fighter + kTrainingFighterActionStateOffset);
    const uint32_t dispatchAction =
        rd32(fighter + kTrainingFighterDispatchActionOffset);
    const uint32_t localFrame =
        rd32(fighter + kTrainingFighterAnimFrameOffset);
    const uint32_t handlerLifecycle =
        rd32(fighter + kTrainingFighterInitStatusOffset);

    const bool actorChanged = !identity.valid
        || identity.fighter != fighter
        || identity.character != character;
    if (actorChanged) {
        identity = TrainingCombatActorIdentity{};
        identity.valid = true;
        identity.fighter = fighter;
        identity.character = character;
        identity.actorToken = trainingNextNonzeroSequence(
            g_trainingCombatActorTokenSequence);
        identity.actionEpoch = trainingNextNonzeroSequence(
            g_trainingCombatActionEpochSequence);
    } else {
        const qtraining::TrainingActionMeta meta =
            qtraining::trainingActionMeta(character, resourceAction);
        const bool knownInternalCycle =
            meta.has(qtraining::TrainingActionFlagLoop)
            || meta.has(qtraining::TrainingActionFlagLanding);
        const bool actionChanged =
            identity.resourceAction != resourceAction
            || identity.dispatchAction != dispatchAction;
        const bool sameIdHandlerRestart = !actionChanged
            && handlerLifecycle == UINT32_MAX
            && identity.handlerLifecycle != UINT32_MAX;
        const bool nonLoopingFrameRestart = !actionChanged
            && localFrame < identity.localFrame
            && !knownInternalCycle;
        if (actionChanged || sameIdHandlerRestart
            || nonLoopingFrameRestart) {
            identity.actionEpoch = trainingNextNonzeroSequence(
                g_trainingCombatActionEpochSequence);
        }
    }

    identity.resourceAction = resourceAction;
    identity.dispatchAction = dispatchAction;
    identity.localFrame = localFrame;
    identity.handlerLifecycle = handlerLifecycle;
    return identity;
}

static bool trainingCombatExactThrowReception(
        uint32_t action,
        qtraining::TrainingDispatchFamily dispatch) {
    return qtraining::combatActionIsThrowReception(action)
        && (dispatch == qtraining::TrainingDispatchFamily::ThrowReception
            || dispatch
                == qtraining::TrainingDispatchFamily::ThrowReception208);
}

static bool trainingCombatDefensiveLock(
        uint32_t action,
        const qtraining::TrainingActionMeta& meta,
        qtraining::TrainingDispatchFamily dispatch) {
    if (trainingCombatExactThrowReception(action, dispatch)
        || qtraining::combatActionIsThrowTechResult(action)) {
        return true;
    }
    switch (dispatch) {
        case qtraining::TrainingDispatchFamily::Guard:
        case qtraining::TrainingDispatchFamily::GroundHitReaction:
        case qtraining::TrainingDispatchFamily::AirGuardCrush:
        case qtraining::TrainingDispatchFamily::AirborneReaction:
        case qtraining::TrainingDispatchFamily::RecoveryResult:
        case qtraining::TrainingDispatchFamily::HardKnockdown:
        case qtraining::TrainingDispatchFamily::Scripted36:
        case qtraining::TrainingDispatchFamily::WallState:
            return true;
        default:
            break;
    }
    return meta.semantic == qtraining::TrainingActionSemantic::Guard
        || meta.semantic == qtraining::TrainingActionSemantic::HitReaction
        || meta.semantic ==
            qtraining::TrainingActionSemantic::KnockdownOrWall
        || meta.semantic == qtraining::TrainingActionSemantic::Recovery;
}

static bool trainingCombatSemanticPathKnown(
        uint32_t action,
        const qtraining::TrainingActionMeta& meta,
        qtraining::TrainingDispatchFamily dispatch) {
    if (trainingCombatExactThrowReception(action, dispatch)
        || qtraining::combatActionIsThrowTechResult(action)) {
        return true;
    }
    if (dispatch == qtraining::TrainingDispatchFamily::RecursiveSystem
        || dispatch == qtraining::TrainingDispatchFamily::Scripted92Or215
        || dispatch == qtraining::TrainingDispatchFamily::Scripted96To98
        || dispatch == qtraining::TrainingDispatchFamily::ThrowReception
        || dispatch == qtraining::TrainingDispatchFamily::ThrowReception208) {
        return false;
    }
    switch (meta.semantic) {
        case qtraining::TrainingActionSemantic::Unused:
        case qtraining::TrainingActionSemantic::UnknownOrScripted:
        case qtraining::TrainingActionSemantic::Throw:
        case qtraining::TrainingActionSemantic::ThrowTechCounterpart:
        case qtraining::TrainingActionSemantic::ThrowTech:
        case qtraining::TrainingActionSemantic::ThrowReception:
        case qtraining::TrainingActionSemantic::CinematicOrSystem:
            return false;
        default:
            return true;
    }
}

static bool trainingCombatTransitionLooksCancelled(
        const qtraining::TrainingActionMeta& meta) {
    return meta.semantic == qtraining::TrainingActionSemantic::Neutral
        || meta.semantic ==
            qtraining::TrainingActionSemantic::MovementOrTurn
        || meta.semantic == qtraining::TrainingActionSemantic::Attack;
}

static bool trainingCombatExcludedFromConventionalAdvantage(
        const qtraining::TrainingActionMeta& meta) {
    return meta.has(qtraining::TrainingActionFlagBroadThrowDeny)
        || meta.semantic == qtraining::TrainingActionSemantic::Throw
        || meta.semantic ==
            qtraining::TrainingActionSemantic::ThrowTechCounterpart
        || meta.semantic == qtraining::TrainingActionSemantic::ThrowTech
        || meta.semantic == qtraining::TrainingActionSemantic::ThrowReception
        || meta.semantic ==
            qtraining::TrainingActionSemantic::CinematicOrSystem;
}

static qtraining::TrainingNativeContactKind trainingNativeContactKind(
        qtraining::CombatContactKind kind) {
    switch (kind) {
        case qtraining::CombatContactKind::Hit:
            return qtraining::TrainingNativeContactKind::Hit;
        case qtraining::CombatContactKind::Block:
            return qtraining::TrainingNativeContactKind::Guard;
        case qtraining::CombatContactKind::Throw:
            return qtraining::TrainingNativeContactKind::None;
        case qtraining::CombatContactKind::None:
            return qtraining::TrainingNativeContactKind::None;
    }
    return qtraining::TrainingNativeContactKind::None;
}

static bool trainingExactThrowTechCounterpartTransition(
        uint8_t attackerOwner,
        const TrainingNaturalRecoveryExpectation& expectation,
        uint32_t action, uint32_t dispatchAction) {
    if (expectation.contactKind != qtraining::CombatContactKind::Throw
        || !qtraining::combatThrowTechCounterpartContinues(
            expectation.resourceAction, expectation.dispatchAction,
            action, dispatchAction)) {
        return false;
    }

    const uintptr_t attacker = trainingFighter(attackerOwner, false);
    const uintptr_t victim = trainingFighter(attackerOwner ^ 1u, false);
    return attacker && victim && attacker == expectation.fighter
        && rd32(attacker + kTrainingFighterPairedActorOffset) == victim
        && rd32(victim + kTrainingFighterActionStateOffset) == 73u
        && rd32(victim + kTrainingFighterDispatchActionOffset) == 73u;
}

static uint8_t trainingCombinedAuthoredPermission(
        uintptr_t fighter, uint8_t owner) {
    uint8_t permission = static_cast<uint8_t>(
        rd32(fighter + kTrainingFighterAuthoredFlagsOffset) & 0x0Fu);
    if (rd32(fighter + kTrainingFighterAlternateFormOffset) == 1u) {
        const uintptr_t partner = trainingAlternateFormActor(owner);
        if (partner) {
            permission |= static_cast<uint8_t>(
                rd32(partner + kTrainingFighterAuthoredFlagsOffset)
                & 0x0Fu);
        }
    }
    return permission;
}

static bool trainingNativeResponseReady(uintptr_t fighter, uint8_t owner) {
    if (fighter == 0u || owner >= 2u) return false;
    if (rd32(fighter + kFighterRoundLostOffset) != 0u) return false;
    if (rd32(fighter + kTrainingFighterFreezeOffset) != 0u) return false;
    const uint8_t permission =
        trainingCombinedAuthoredPermission(fighter, owner);
    if ((permission & 0x01u) == 0u) return false;
    const uint32_t action =
        rd32(fighter + kTrainingFighterActionStateOffset);
    if (qtraining::combatActionCannotCommitCommand(action)) return false;
    return !qtraining::combatActionIsNativeReactionFamily(action)
        || (permission & 0x0Cu) == 0u;
}

struct TrainingActorPoolSource {
    uintptr_t base;
    uint32_t count;
    qtraining::TrainingActorPoolId pool;
};

static bool trainingActorSlotLive(uintptr_t actor) {
    if (!actor || !rangeReadable(actor, addr::kFighterSize)) return false;
    const uint8_t character = rd8(actor + kTrainingFighterCharacterOffset);
    if (character == 0xFFu) return false;
    if (character > 36u) return false;
    if (rd32(actor + kTrainingFighterActionStateOffset) > 255u) return false;
    return rd8(actor + kFighterPlayerSlotOffset) <= 3u;
}

static void trainingObserveActorPools(qtraining::TrainingStatusV1& status) {
    status.actorPoolValid = 0u;
    status.actorPoolLiveCount = 0u;
    status.actorPoolReportedCount = 0u;
    if (!g_trainingDebugOverlayEnabled) return;

    const TrainingActorPoolSource sources[] = {
        { addr::kEffectPool, 30u, qtraining::TrainingActorPoolId::Effect },
        { addr::kActorClonePool, 120u, qtraining::TrainingActorPoolId::Clone },
        { addr::kHelperCloneP1, 1u, qtraining::TrainingActorPoolId::Helper },
        { addr::kHelperCloneP2, 1u, qtraining::TrainingActorPoolId::Helper },
        { addr::kHelperCloneP3, 1u, qtraining::TrainingActorPoolId::Helper },
        { addr::kHelperCloneP4, 1u, qtraining::TrainingActorPoolId::Helper },
    };

    uint32_t reported = 0u;
    uint32_t live = 0u;
    uint32_t helperIndex = 0u;
    for (const TrainingActorPoolSource& source : sources) {
        for (uint32_t slot = 0u; slot < source.count; ++slot) {
            const uintptr_t actor =
                source.base + static_cast<uintptr_t>(slot) * addr::kFighterSize;
            if (!trainingActorSlotLive(actor)) continue;
            ++live;
            if (reported >= qtraining::kTrainingActorReportCapacity) continue;
            const uint32_t index =
                source.pool == qtraining::TrainingActorPoolId::Helper
                    ? helperIndex : slot;
            status.actorPoolAction[reported] =
                rd32(actor + kTrainingFighterActionStateOffset);
            uint8_t owner = 0u;
            status.actorPoolOwner[reported] = trainingActorOwner(actor, owner)
                ? owner : qtraining::kTrainingActorOwnerUnknown;
            status.actorPoolSlot[reported] =
                qtraining::encodeTrainingActorSlot(source.pool, index);
            ++reported;
        }
        if (source.pool == qtraining::TrainingActorPoolId::Helper)
            ++helperIndex;
    }
    status.actorPoolLiveCount = live;
    status.actorPoolReportedCount = reported;
    status.actorPoolValid = 1u;
}

static bool trainingNativeTerminalCommandWindow(uintptr_t fighter) {
    if (fighter == 0u) return false;
    const uint32_t action =
        rd32(fighter + kTrainingFighterActionStateOffset);
    return qtraining::combatActionIsNativeReactionFamily(action)
        && (rd32(fighter + kTrainingFighterTerminalReadyOffset) & 0x01u) != 0u;
}

static bool trainingNativeGuardCancelReady(
        uintptr_t fighter, uint8_t owner) {
    if (fighter == 0u || owner >= 2u) return false;
    const uint32_t action =
        rd32(fighter + kTrainingFighterActionStateOffset);
    return qtraining::combatActionIsGuard(action)
        && rd32(fighter + kFighterRoundLostOffset) == 0u
        && rd32(fighter + kTrainingFighterFreezeOffset) == 0u
        && (trainingCombinedAuthoredPermission(fighter, owner) & 0x04u)
               != 0u;
}

static uint16_t trainingReadFrameU16(uintptr_t address) {
    return static_cast<uint16_t>(rd8(address))
        | static_cast<uint16_t>(
              static_cast<uint16_t>(rd8(address + 1u)) << 8u);
}

constexpr uint32_t kTrainingCommandRingWindowFrames = 16u;

static bool trainingPadReversalSequenceWithGuard(
        const qtraining::TrainingMoveSequence& source,
        uint32_t sourceCommitIndex, uint32_t pad,
        qtraining::TrainingMoveSequence& padded,
        uint32_t& paddedCommitIndex) {
    padded = qtraining::TrainingMoveSequence{};
    paddedCommitIndex = 0u;
    if (pad == 0u) {
        padded = source;
        paddedCommitIndex = sourceCommitIndex;
        return true;
    }
    if (sourceCommitIndex == 0u
        || sourceCommitIndex >= source.stepCount
        || source.stepCount > qtraining::TrainingMacros::
                                  kTriggeredSequenceCapacity - pad) {
        return false;
    }
    uint32_t out = 0u;
    for (uint32_t index = 0u; index < sourceCommitIndex; ++index)
        padded.relativeMasks[out++] = source.relativeMasks[index];
    for (uint32_t held = 0u; held < pad; ++held)
        padded.relativeMasks[out++] = qinput::kLogicalLeft;
    paddedCommitIndex = out;
    for (uint32_t index = sourceCommitIndex; index < source.stepCount; ++index)
        padded.relativeMasks[out++] = source.relativeMasks[index];
    padded.stepCount = out;
    return true;
}

static uint32_t trainingReversalGuardPad(uint32_t remaining,
                                         uint32_t commitIndex) {
    if (remaining <= commitIndex + 1u) return 0u;
    const uint32_t wanted = remaining - commitIndex - 1u;
    if (commitIndex + 2u >= kTrainingCommandRingWindowFrames) return 0u;
    const uint32_t budget =
        kTrainingCommandRingWindowFrames - commitIndex - 2u;
    return wanted < budget ? wanted : budget;
}

static bool trainingWakeupSequenceCommitIndex(
        const qtraining::TrainingMoveSequence& sequence,
        uint32_t& commitIndex) {
    commitIndex = 0u;
    if (sequence.stepCount < 2u
        || sequence.stepCount
               > qtraining::TrainingMacros::kTriggeredSequenceCapacity
        || sequence.relativeMasks[0] != 0u) {
        return false;
    }
    bool haveCommit = false;
    for (uint32_t index = 0u; index < sequence.stepCount; ++index) {
        const uint32_t mask = sequence.relativeMasks[index];
        if (!qinput::isCanonicalLogicalFightInput(mask)) return false;
        if ((mask & qinput::kLogicalButtonMask) != 0u) {
            haveCommit = true;
            commitIndex = index;
        }
    }
    if (!haveCommit || commitIndex == 0u
        || commitIndex + 1u >= sequence.stepCount) {
        return false;
    }
    for (uint32_t index = commitIndex + 1u;
         index < sequence.stepCount; ++index) {
        if ((sequence.relativeMasks[index] & qinput::kLogicalButtonMask)
            != 0u) {
            return false;
        }
    }
    return true;
}

constexpr uint32_t kTrainingParsePressLead = 1u;
constexpr uint32_t kTrainingResponseLookaheadTickCap = 240u;

static bool trainingTicksUntilResponseReady(
        uintptr_t fighter, uint8_t owner, uint32_t& ticks) {
    ticks = 0u;
    if (!fighter || owner >= 2u) return false;
    if (trainingNativeResponseReady(fighter, owner)) return true;

    if (rd32(fighter + kFighterRoundLostOffset) != 0u
        || rd32(fighter + kTrainingFighterFreezeOffset) != 0u
        || rd32(fighter + kTrainingFighterTransitionFlagOffset) != 0u
        || rd32(fighter + kTrainingFighterForcedActionOffset) != UINT32_MAX
        || rd32(fighter + kTrainingFighterAlternateFormOffset) == 1u) {
        return false;
    }

    const uint32_t action =
        rd32(fighter + kTrainingFighterActionStateOffset);
    if (qtraining::combatActionCannotCommitCommand(action)) return false;
    const bool reactionFamily =
        qtraining::combatActionIsNativeReactionFamily(action);

    const uintptr_t actionTable = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterActionTableOffset));
    const uintptr_t frameTable = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterFrameTableOffset));
    if (!actionTable || !frameTable) return false;
    const uintptr_t actionEntry = actionTable
        + static_cast<uintptr_t>(action) * kTrainingActionEntryStride;
    if (actionEntry < actionTable
        || !rangeReadable(actionEntry, kTrainingActionEntryStride)) {
        return false;
    }
    const int32_t frameBase = static_cast<int32_t>(rd32(actionEntry));
    const int32_t frameCount = static_cast<int32_t>(rd32(actionEntry + 4u));
    if (frameBase < 0 || frameCount <= 0 || frameCount > 256) return false;

    const uint32_t currentRemaining =
        rd32(fighter + kTrainingFighterActionLockEndOffset);
    if (currentRemaining == 0u || currentRemaining >= 0xFFFFu) return false;

    uint32_t elapsed = currentRemaining;
    uint32_t cursor = rd32(fighter + kTrainingFighterActionFrameOffset);
    uint8_t visited[256] = {};
    while (cursor != 0xFFFEu) {
        if (cursor >= static_cast<uint32_t>(frameCount)
            || visited[cursor] != 0u
            || elapsed > kTrainingResponseLookaheadTickCap) {
            return false;
        }
        visited[cursor] = 1u;
        const uint64_t absolute = static_cast<uint64_t>(frameBase) + cursor;
        if (absolute
            > (static_cast<uint64_t>(UINTPTR_MAX) - frameTable)
                  / kTrainingFighterFrameStride) {
            return false;
        }
        const uintptr_t frame = frameTable
            + static_cast<uintptr_t>(absolute) * kTrainingFighterFrameStride;
        if (frame < frameTable
            || !rangeReadable(frame, kTrainingFighterFrameStride)) {
            return false;
        }
        const uint8_t permission = static_cast<uint8_t>(
            rd8(frame + kTrainingFramePermissionOffset) & 0x0Fu);
        const uint8_t motionFlags =
            rd8(frame + kTrainingFrameMotionFlagsOffset);
        if ((motionFlags & 0x04u) != 0u) return false;
        const bool open = (permission & 0x01u) != 0u
            && (!reactionFamily || (permission & 0x0Cu) == 0u);
        if (open) {
            ticks = elapsed;
            return true;
        }
        const uint32_t duration = rd32(frame + kTrainingFrameDurationOffset);
        if (duration == 0u || duration >= 0xFFFFu
            || elapsed > kTrainingResponseLookaheadTickCap - duration) {
            return false;
        }
        elapsed += duration;
        const uint16_t next =
            trainingReadFrameU16(frame + kTrainingFrameNextOffset);
        if (next == 0xFFFEu) {
            return false;
        }
        if (next == 0xFFFFu) {
            if (cursor + 1u >= static_cast<uint32_t>(frameCount))
                return false;
            ++cursor;
        } else {
            cursor = next;
        }
    }
    return false;
}

static const char* g_trainingSuffixRefusal = "none";

static bool trainingWakeupLockedStepsRemaining(
        uintptr_t fighter, uint32_t& remaining) {
    remaining = 0u;
    g_trainingSuffixRefusal = "none";
    if (!fighter || rd32(fighter + kTrainingFighterFreezeOffset) != 0u
        || rd32(fighter + kFighterRoundLostOffset) != 0u
        || rd32(fighter + kTrainingFighterTransitionFlagOffset) != 0u
        || rd32(fighter + kTrainingFighterForcedActionOffset) != UINT32_MAX
        || static_cast<int32_t>(
               rd32(fighter + kTrainingFighterYOffset)) != 0
        || rd8(fighter + kTrainingFighterFloorModeOffset) != 0u
        || trainingReadFrameU16(
               fighter + kTrainingFighterVerticalVelocityOffset) != 0u
        || trainingReadFrameU16(
               fighter + kTrainingFighterVerticalAccelerationOffset) != 0u
        || trainingReadFrameU16(
               fighter + kTrainingFighterVerticalJerkOffset) != 0u
        || trainingReadFrameU16(
               fighter + kTrainingFighterVerticalTweenOffset) != 0u
        || trainingReadFrameU16(
               fighter + kTrainingFighterVerticalTweenDeltaOffset) != 0u)
        { g_trainingSuffixRefusal = "not-a-locked-action"; return false; }
    const uint32_t action =
        rd32(fighter + kTrainingFighterActionStateOffset);
    const bool guardReaction = qtraining::combatActionIsGuard(action)
        && action != 20u;
    const bool hitReaction = qtraining::combatActionIsGroundHit(action);
    if (!qtraining::combatActionIsNaturalWakeupPath(action)
        && !guardReaction && !hitReaction) {
        { g_trainingSuffixRefusal = "com-or-permission"; return false; }
    }
    const uint8_t permissionRefuseMask = guardReaction ? 0x09u : 0x0Du;
    if (rd32(fighter + kTrainingFighterComOffset) != 0u
        || (trainingCombinedAuthoredPermission(fighter, 1u)
            & permissionRefuseMask) != 0u) {
        { g_trainingSuffixRefusal = "command-child"; return false; }
    }
    const uintptr_t commandChild = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterCommandChildOffset));
    if (!commandChild
        || !rangeReadable(commandChild, kTrainingCommandChildSize)
        || rd32(commandChild + kTrainingCommandDirectionKillOffset) != 0u) {
        { g_trainingSuffixRefusal = "tables"; return false; }
    }

    const uintptr_t actionTable = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterActionTableOffset));
    const uintptr_t frameTable = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterFrameTableOffset));
    if (!actionTable || !frameTable) return false;
    const uintptr_t actionEntry = actionTable
        + static_cast<uintptr_t>(action) * kTrainingActionEntryStride;
    if (actionEntry < actionTable
        || !rangeReadable(actionEntry, kTrainingActionEntryStride)) {
        { g_trainingSuffixRefusal = "frame-bounds"; return false; }
    }
    const int32_t frameBase = static_cast<int32_t>(rd32(actionEntry));
    const int32_t frameCount = static_cast<int32_t>(rd32(actionEntry + 4u));
    if (frameBase < 0 || frameCount <= 0 || frameCount > 256)
        { g_trainingSuffixRefusal = "anim-out-of-range"; return false; }

    const uint32_t currentLocal =
        rd32(fighter + kTrainingFighterAnimFrameOffset);
    if (currentLocal >= static_cast<uint32_t>(frameCount)) return false;
    const uint64_t currentAbsolute = static_cast<uint64_t>(frameBase)
        + currentLocal;
    if (currentAbsolute > UINT32_MAX) return false;
    if (currentAbsolute
        > (static_cast<uint64_t>(UINTPTR_MAX) - frameTable)
              / kTrainingFighterFrameStride) {
        { g_trainingSuffixRefusal = "addressing"; return false; }
    }
    const uintptr_t currentFrame = frameTable
        + static_cast<uintptr_t>(currentAbsolute)
              * kTrainingFighterFrameStride;
    if (currentFrame < frameTable
        || !rangeReadable(currentFrame, kTrainingFighterFrameStride)
        || static_cast<uintptr_t>(
               rd32(fighter + kTrainingFighterCurrentFrameOffset))
               != currentFrame
         || (rd8(currentFrame + kTrainingFrameMotionFlagsOffset) & 0x04u)
               != 0u
         || (rd32(fighter + kTrainingFighterAuthoredFlagsOffset) & 0x0Fu)
                != (rd8(currentFrame + kTrainingFramePermissionOffset)
                    & 0x0Fu)) {
        { g_trainingSuffixRefusal = "current-frame"; return false; }
    }

    uint32_t total = rd32(fighter + kTrainingFighterActionLockEndOffset);
    if (total == 0u || total >= 0xFFFFu
        || total > qtraining::kTrainingMacroFrameCapacity) {
        { g_trainingSuffixRefusal = "lock-empty-or-sentinel"; return false; }
    }
    uint32_t cursor = rd32(fighter + kTrainingFighterActionFrameOffset);
    uint8_t visited[256] = {};
    while (cursor != 0xFFFEu) {
        if (cursor >= static_cast<uint32_t>(frameCount)
            || visited[cursor] != 0u) {
            { g_trainingSuffixRefusal = "chain-bounds"; return false; }
        }
        visited[cursor] = 1u;
        const uint64_t absolute = static_cast<uint64_t>(frameBase) + cursor;
        if (absolute > UINT32_MAX) return false;
        if (absolute
            > (static_cast<uint64_t>(UINTPTR_MAX) - frameTable)
                  / kTrainingFighterFrameStride) {
            { g_trainingSuffixRefusal = "frame-unreadable"; return false; }
        }
        const uintptr_t frame = frameTable
            + static_cast<uintptr_t>(absolute)
                  * kTrainingFighterFrameStride;
        if (frame < frameTable
            || !rangeReadable(frame, kTrainingFighterFrameStride)) {
            { g_trainingSuffixRefusal = "frame-refused"; return false; }
        }
        const uint8_t permission =
            rd8(frame + kTrainingFramePermissionOffset);
        const uint8_t motionFlags =
            rd8(frame + kTrainingFrameMotionFlagsOffset);
        const bool initializesVerticalMotion = (motionFlags & 0x02u) != 0u;
        if ((permission & permissionRefuseMask) != 0u
            || (motionFlags & 0x04u) != 0u
            || (initializesVerticalMotion
                && (trainingReadFrameU16(
                        frame + kTrainingFrameVerticalVelocityOffset) != 0u
                    || trainingReadFrameU16(
                        frame + kTrainingFrameVerticalAccelerationOffset)
                           != 0u))) {
            { g_trainingSuffixRefusal = "duration"; return false; }
        }
        const uint32_t duration =
            rd32(frame + kTrainingFrameDurationOffset);
        if (duration == 0u || duration >= 0xFFFFu
            || duration > qtraining::kTrainingMacroFrameCapacity
            || total > qtraining::kTrainingMacroFrameCapacity - duration) {
            { g_trainingSuffixRefusal = "link-bounds"; return false; }
        }
        total += duration;
        const uint16_t next = trainingReadFrameU16(
            frame + kTrainingFrameNextOffset);
        if (next == 0xFFFEu) {
            cursor = 0xFFFEu;
        } else if (next == 0xFFFFu) {
            if (cursor + 1u >= static_cast<uint32_t>(frameCount))
                { g_trainingSuffixRefusal = "other"; return false; }
            ++cursor;
        } else {
            cursor = next;
        }
    }
    remaining = total;
    return true;
}

static bool trainingStepsUntilDummyCanAct(
        uintptr_t fighter, uint32_t& remaining, bool& gateBased) {
    remaining = 0u;
    gateBased = false;
    uint32_t gateTicks = 0u;
    if (trainingTicksUntilResponseReady(fighter, 1u, gateTicks)) {
        if (gateTicks == 0u) return false;
        remaining = gateTicks;
        gateBased = true;
        return true;
    }
    return trainingWakeupLockedStepsRemaining(fighter, remaining);
}

static bool trainingWakeupActivationStillReady(uintptr_t fighter) {
    if (!fighter
        || rd32(fighter + kFighterRoundLostOffset) != 0u
        || rd32(fighter + kTrainingFighterFreezeOffset) != 0u
        || rd32(fighter + kTrainingFighterComOffset) != 0u
        || rd32(fighter + kTrainingFighterForcedActionOffset) != UINT32_MAX
        || !trainingNativeResponseReady(fighter, 1u)) {
        return false;
    }
    const uintptr_t commandChild = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterCommandChildOffset));
    return commandChild != 0u
        && rangeReadable(commandChild, kTrainingCommandChildSize)
        && rd32(commandChild + kTrainingCommandDirectionKillOffset) == 0u;
}

static void trainingReversalTraceCloseWindow();
static void trainingRevokeSeededMotion(uintptr_t fighter);

static void trainingCancelWakeupReversal(
        qtraining::TriggerRequestOutcome outcome,
        const char* reason) {
    if (!g_trainingWakeupReversal.active) return;
    (void)g_trainingMacros.requestStop();
    g_trainingWakeupReversal.active = false;
    g_trainingWakeupReversal.released = false;
    g_trainingWakeupReversal.releasedEarly = false;
    g_trainingWakeupReversal.phase =
        qtraining::TrainingWakeupReversalPhase::Cancelled;
    g_trainingWakeupReversal.lastOutcome = outcome;
    if (!g_trainingWakeupReversal.released) {
        const uintptr_t seeded =
            trainingFighter(trainingDummySlot(), false);
        if (seeded) trainingRevokeSeededMotion(seeded);
    }
    trainingReversalTraceCloseWindow();
    QLOG("[training-wakeup] prebuffer cancelled: %s move=%u step=%llu",
         reason ? reason : "unknown",
         static_cast<unsigned>(g_trainingWakeupReversal.moveIndex),
         static_cast<unsigned long long>(g_trainingStepSerial));
}

static uint64_t trainingCharacterFingerprint() {
    uint32_t words[10] = {
        rd32(0x62369Cu), rd32(0x6236A0u),
        rd32(0x6236B4u), rd32(kTrainingComMaskAddr),
        0u, 0u, 0u, 0u, 0u, 0u,
    };
    for (uint32_t player = 0; player < 4u; ++player) {
        const uintptr_t fighter = trainingFighter(player, false);
        words[4u + player] = fighter
            ? (static_cast<uint32_t>(
                   rd8(fighter + kTrainingFighterCharacterOffset))
               | (static_cast<uint32_t>(
                      rd8(fighter + kFighterPlayerSlotOffset)) << 8u))
            : 0xFFFFFFFFu;
    }
    words[8] = rd32(addr::kFighterPtrTable);
    words[9] = rd32(addr::kFighterPtrTable + 4u);
    return netFnv1a(words, sizeof(words));
}

static void trainingInvalidateStates(bool stale) {
    for (TrainingStateSlot& slot : g_trainingStates) {
        if (stale) {
            if (slot.valid) slot.stale = true;
        } else {
            slot.stale = false;
        }
        slot.valid = false;
    }
    g_trainingScratch.valid = false;
}

static bool trainingSoundStopReady() {
    return g_soundStopHooked && g_origSoundStop != nullptr;
}

static bool trainingStopTransientSfx() {
    if (!trainingSoundStopReady()) return false;
    for (int endpoint = 0; endpoint < 64; ++endpoint)
        (void)callNativeSoundStopPreservingFpu(endpoint, 1);
    for (int sample = 2; sample <= 255; ++sample)
        (void)callNativeSoundStopPreservingFpu(sample, 2);
    return true;
}

constexpr uint32_t kTrainingRefillLife = 1u << 0;
constexpr uint32_t kTrainingRefillGuardGauge = 1u << 1;
constexpr uint32_t kTrainingRefillExMeter = 1u << 2;
constexpr uint32_t kTrainingRefillNativeF4 =
    kTrainingRefillLife | kTrainingRefillExMeter;
static_assert((kTrainingRefillNativeF4 & kTrainingRefillGuardGauge) == 0u,
              "QOH's own F4 refills life and EX meter, never the guard gauge");
static uint32_t g_trainingRefillRequest = 0u;

static bool trainingApplyConfigOverlay() {
    if (!g_trainingLaunchRequested) return false;
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    if (!trainingCaptureOriginalConfig(active)) return false;
    wr32(active + 36u, 2u);
    wr32(active + 64u, 0u);
    wr32(active + 80u, 0u);
    wr32(active + 84u, 0u);
    wr32(active + 88u, 0u);
    wr32(active + 92u, 0u);
    wr32(active + 96u, 0u);
    if (rangeReadable(kTrainingCopiedConfigAddr, 100u)) {
        wr32(kTrainingCopiedConfigAddr + 36u, 2u);
        wr32(kTrainingCopiedConfigAddr + 64u, 0u);
        wr32(kTrainingCopiedConfigAddr + 80u, 0u);
        wr32(kTrainingCopiedConfigAddr + 84u, 0u);
        wr32(kTrainingCopiedConfigAddr + 88u, 0u);
        wr32(kTrainingCopiedConfigAddr + 92u, 0u);
        wr32(kTrainingCopiedConfigAddr + 96u, 0u);
    }
    wr8(kTrainingPauseAddr, 0u);
    wr8(kTrainingFreezeAddr, 0u);
    return rd32(active + 36u) == 2u
        && rd32(active + 64u) == 0u
        && rd32(active + 80u) == 0u
        && rd32(active + 84u) == 0u
        && rd32(active + 88u) == 0u
        && rd32(active + 92u) == 0u
        && rd32(active + 96u) == 0u
        && rd8(kTrainingPauseAddr) == 0u
        && rd8(kTrainingFreezeAddr) == 0u;
}

static void trainingClearNativeDeveloperEdges() {
    clearOwnedNativeDeveloperEdges();
    wr32(kNativeFatalHitDisableAddr, 0u);
}

static bool trainingRepairHumanOwnership() {
    bool repaired = false;
    const uint32_t comMask = rd32(kTrainingComMaskAddr);
    if ((comMask & 3u) != 0u) {
        wr32(kTrainingComMaskAddr, comMask & ~3u);
        repaired = true;
    }
    for (uint32_t player = 0; player < 2u; ++player) {
        for (int mirror = 0; mirror < 2; ++mirror) {
            const uintptr_t fighter =
                trainingFighter(player, mirror != 0);
            if (!fighter) return false;
            if (rd32(fighter + kTrainingFighterComOffset) != 0u) {
                wr32(fighter + kTrainingFighterComOffset, 0u);
                repaired = true;
            }
        }
    }
    if (repaired) {
        ++g_trainingStickyErrors;
        QLOG("[training] repaired stale native COM ownership");
    }
    return (rd32(kTrainingComMaskAddr) & 3u) == 0u;
}

static bool trainingCaptureBaseline() {
    if (rd32(kRoundResultStateAddr) != 0u
        || rd32(kTrainingRoundCounterAddr) != 0u) {
        return false;
    }
    g_trainingBaseline.resultState = rd32(kRoundResultStateAddr);
    g_trainingBaseline.roundProgress =
        rd32(kTrainingRoundProgressAddr);
    g_trainingBaseline.roundCounter =
        rd32(kTrainingRoundCounterAddr);
    g_trainingBaseline.outcomeP1 =
        rd32(kTrainingOutcomeP1Addr);
    g_trainingBaseline.outcomeP2 =
        rd32(kTrainingOutcomeP2Addr);
    g_trainingBaseline.winsP1 = rd32(kTrainingWinsP1Addr);
    g_trainingBaseline.winsP2 = rd32(kTrainingWinsP2Addr);
    g_trainingBaseline.resultProgressP1 =
        rd32(kTrainingResultProgressP1Addr);
    g_trainingBaseline.resultProgressP2 =
        rd32(kTrainingResultProgressP2Addr);
    g_trainingBaseline.valid = true;
    return true;
}

static bool trainingFighterResultFieldsNeutral() {
    for (uint32_t player = 0; player < 2u; ++player) {
        for (int mirror = 0; mirror < 2; ++mirror) {
            const uintptr_t fighter =
                trainingFighter(player, mirror != 0);
            if (!fighter
                || rd32(fighter + kFighterRoundEndOffset) != 0u
                || rd32(fighter + kFighterRoundLostOffset) != 0u
                || rd32(fighter + kTrainingFighterComOffset) != 0u) {
                return false;
            }
        }
    }
    return true;
}

static bool trainingInvariantsHold() {
    return g_trainingBaseline.valid
        && rd32(kSceneStateAddr) == 2u
        && rd32(kMatchSubAddr) == 0u
        && rd32(kRoundResultStateAddr)
            == g_trainingBaseline.resultState
        && rd32(kTrainingRoundProgressAddr)
            == g_trainingBaseline.roundProgress
        && rd32(kTrainingRoundCounterAddr)
            == g_trainingBaseline.roundCounter
        && rd32(kTrainingOutcomeP1Addr)
            == g_trainingBaseline.outcomeP1
        && rd32(kTrainingOutcomeP2Addr)
            == g_trainingBaseline.outcomeP2
        && rd32(kTrainingWinsP1Addr) == g_trainingBaseline.winsP1
        && rd32(kTrainingWinsP2Addr) == g_trainingBaseline.winsP2
        && rd32(kTrainingResultProgressP1Addr)
            == g_trainingBaseline.resultProgressP1
        && rd32(kTrainingResultProgressP2Addr)
            == g_trainingBaseline.resultProgressP2
        && rd32(kNativeFatalHitDisableAddr) == 0u
        && (rd32(kTrainingComMaskAddr) & 3u) == 0u
        && trainingFighterResultFieldsNeutral();
}

static bool trainingPrepareRollbackLayout() {
    if (g_rbReady && gameplayLayoutConfigChanged()) {
        const uint32_t priorLayout = g_rb.layoutGeneration();
        reregisterFrameData();
        trainingInvalidateStates(true);
        QLOG("[training-state] layout changed from generation %u; "
             "all external slots invalidated", priorLayout);
    }
    if (!ensureRollback()) return false;
    const size_t bytes = g_rb.totalSize();
    if (bytes == 0u) return false;
    if (g_trainingScratch.bytes.size() != bytes) {
        try {
            g_trainingScratch.bytes.assign(bytes, 0u);
            for (TrainingStateSlot& slot : g_trainingStates)
                slot.bytes.assign(bytes, 0u);
        } catch (const std::bad_alloc&) {
            g_trainingScratch.bytes.clear();
            for (TrainingStateSlot& slot : g_trainingStates)
                slot.bytes.clear();
            return false;
        }
        trainingInvalidateStates(true);
    }
    return true;
}

static bool trainingCaptureSnapshot(TrainingStateSlot& slot) {
    if (!trainingPrepareRollbackLayout()
        || slot.bytes.size() != g_rb.totalSize()) {
        return false;
    }
    const char* why = nullptr;
    if (!g_rb.validateLive(
            &why,
            RollbackManager::LiveValidationPolicy::LocalTrainingSnapshot)
        || !g_rb.snapshotTo(slot.bytes.data())) {
        QLOG("[training-state] snapshot rejected: %s",
             why ? why : "copy failed");
        return false;
    }
    fxSaveTo(slot.fpu.bytes);
    slot.fpu.valid = true;
    slot.generation = g_trainingGeneration;
    slot.layoutGeneration = g_rb.layoutGeneration();
    slot.characterFingerprint = trainingCharacterFingerprint();
    slot.stepSerial = g_trainingStepSerial;
    slot.recoveryRng = g_trainingRecovery.rngState();
    slot.scene = rd32(kSceneStateAddr);
    slot.matchSubstate = rd32(kMatchSubAddr);
    slot.valid = true;
    slot.stale = false;
    return true;
}

static bool trainingSlotCompatible(const TrainingStateSlot& slot) {
    return slot.valid && !slot.stale && slot.fpu.valid
        && slot.generation == g_trainingGeneration
        && slot.layoutGeneration == g_rb.layoutGeneration()
        && slot.bytes.size() == g_rb.totalSize()
        && slot.characterFingerprint == trainingCharacterFingerprint()
        && slot.scene == 2u && slot.matchSubstate == 0u
        && rd32(kSceneStateAddr) == 2u
        && rd32(kMatchSubAddr) == 0u;
}

static void trainingResetTimelineMachines(
    bool preserveObservationCounters = false) {
    if (preserveObservationCounters) {
        g_trainingRecovery.resetTimeline();
        g_trainingRecoveryP1.resetTimeline();
    } else {
        g_trainingRecovery.reset(g_trainingGeneration);
        g_trainingRecoveryP1.reset(g_trainingGeneration);
    }
    g_trainingRecoveryEmission = qtraining::RecoveryEmission{};
    g_trainingRecoveryEmissionP1 = qtraining::RecoveryEmission{};
    trainingResetCombatToolTimeline(preserveObservationCounters);
    g_trainingNeutralTrigger.reset();
    g_trainingExactIdleTrigger.reset();
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        g_trainingComboEpisode[player].reset();
        g_trainingRecoveryExit[player].reset();
        g_trainingNeutralReasonMask[player] = 0u;
    }
    g_trainingNeutralGlobalReasonMask = 0u;
    g_trainingBothFightersNeutral = 0u;
    g_trainingNeutralHoldSteps = 0u;
    g_trainingResourcePolicyValid = 0u;
    g_trainingRoundStartNeutralPending = false;
    g_trainingRoundStartNeutralValid = 0u;
    g_trainingRoundStartNeutralMask = 0u;
    g_trainingRoundStartNeutralStepSerial = 0u;
    g_trainingRoundStartNeutralTargetMask = 0u;
}

static bool trainingFinishRestoredImage(
    const TrainingFpuImage& restoredFpu, const char* imageName) {
    trainingResetTimelineMachines(true);
    (void)g_trainingMacros.requestStop();
    const char* rawWhy = nullptr;
    const bool rawValid = g_rb.validateLive(
        &rawWhy, RollbackManager::LiveValidationPolicy::RawStructuralOnly);
    bool configValid = false;
    bool ownershipValid = false;
    bool postwriteValid = false;
    bool invariantValid = false;
    const char* postwriteWhy = nullptr;
    if (rawValid) {
        configValid = trainingApplyConfigOverlay();
        wr8(kTrainingHitboxAddr,
            qtraining::hitboxVanillaFlagValue(g_trainingHitboxMode));
        trainingClearNativeDeveloperEdges();
        ownershipValid = trainingRepairHumanOwnership();
        postwriteValid = g_rb.validateLive(
            &postwriteWhy,
            RollbackManager::LiveValidationPolicy::LocalTrainingSnapshot);
        invariantValid = trainingInvariantsHold();
    }
    const bool audioValid = trainingStopTransientSfx();
    const bool valid = rawValid && configValid && ownershipValid
        && postwriteValid && invariantValid && audioValid;
    if (!valid) {
        QLOG("[training-state] %s restore validation failed "
             "raw=%d(%s) config=%d ownership=%d postwrite=%d(%s) "
             "invariant=%d audio=%d",
             imageName ? imageName : "unknown",
             rawValid ? 1 : 0,
             rawWhy ? rawWhy : "ok",
             configValid ? 1 : 0,
             ownershipValid ? 1 : 0,
             postwriteValid ? 1 : 0,
             postwriteWhy ? postwriteWhy : "ok",
             invariantValid ? 1 : 0,
             audioValid ? 1 : 0);
    }
    fxRestoreFrom(const_cast<uint8_t*>(restoredFpu.bytes));
    return valid;
}

static bool trainingRestoreSnapshot(const TrainingStateSlot& slot) {
    if (!trainingPrepareRollbackLayout()
        || !trainingSlotCompatible(slot)
        || !g_rb.loadFrom(slot.bytes.data())) {
        return false;
    }
    const bool finished =
        trainingFinishRestoredImage(slot.fpu, "savestate");
    g_trainingRecovery.restoreRngState(slot.recoveryRng);
    return finished;
}

static void trainingFailMutation(TrainingDisableReason reason,
                                 const char* text) {
    g_trainingMutationFailed = true;
    g_trainingStepFailure = true;
    g_trainingFinalInput[0] = g_trainingFinalInput[1] = 0u;
    ++g_trainingStickyErrors;
    g_trainingDisableReason = reason;
    trainingCopyText(g_trainingDisableText, text);
    trainingCopyText(g_trainingResultText, text);
    QLOG("[training] FAIL-CLOSED reason=%u: %s",
         static_cast<unsigned>(reason), text ? text : "");
}

constexpr uint32_t kTrainingWorldClampIterationCap = 1024u;

struct TrainingNativeCollisionRectangle {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = 0;
    int32_t bottom = 0;
};

static bool trainingCheckedScaledPointer(
    uintptr_t base, uint64_t index, uintptr_t stride,
    size_t readableBytes, uintptr_t& result) {
    if (!base || !stride) return false;
    const uint64_t maxPointer =
        static_cast<uint64_t>((std::numeric_limits<uintptr_t>::max)());
    const uint64_t base64 = static_cast<uint64_t>(base);
    if (base64 > maxPointer
        || index > (maxPointer - base64) / stride) {
        return false;
    }
    result = base + static_cast<uintptr_t>(index * stride);
    return rangeReadable(result, readableBytes);
}

static bool trainingCurrentCollisionRectangle(
    uintptr_t fighter, TrainingNativeCollisionRectangle& rectangle) {
    if (!rangeReadable(fighter, addr::kFighterSize)) return false;

    const uintptr_t actionTable = rd32(fighter + 16u);
    const uint32_t actionIndex = rd32(fighter + 48u);
    const uintptr_t frameBase = rd32(fighter + 24u);
    uintptr_t action = 0;
    if (actionIndex > 4095u
        || !rangeReadable(actionTable, 24u)
        || !trainingCheckedScaledPointer(
            actionTable, actionIndex, 24u, 24u, action)) {
        return false;
    }

    const uint64_t frameIndex =
        static_cast<uint64_t>(rd32(action))
        + static_cast<uint64_t>(rd32(fighter + 64u));
    uintptr_t frame = 0;
    if (frameIndex > 0xFFFFu
        || !rangeReadable(frameBase, 64u)
        || !trainingCheckedScaledPointer(
            frameBase, frameIndex, 64u, 64u, frame)
        || rd8(frame + 24u) == 0xFFu) {
        return false;
    }

    const uint64_t collisionIndex =
        static_cast<uint64_t>(rd32(action + 8u))
        + static_cast<uint64_t>(rd8(frame + 23u));
    const uintptr_t collisionTable = rd32(fighter + 32u);
    uintptr_t collisionRecord = 0;
    if (collisionIndex > static_cast<uint64_t>(INT_MAX)
        || !rangeReadable(collisionTable, 8u)
        || !trainingCheckedScaledPointer(
            collisionTable, collisionIndex, 8u, 8u,
            collisionRecord)) {
        return false;
    }

    int nativeRectangle[4] = {};
    const auto collision =
        reinterpret_cast<TrainingCollisionRectFn>(
            kTrainingCollisionRectFn);
    if (collision(static_cast<int>(fighter),
                  static_cast<int>(collisionIndex),
                  nativeRectangle) != 1) {
        return false;
    }
    if (nativeRectangle[0] > nativeRectangle[2]
        || nativeRectangle[1] > nativeRectangle[3]) {
        return false;
    }
    rectangle.left = nativeRectangle[0];
    rectangle.top = nativeRectangle[1];
    rectangle.right = nativeRectangle[2];
    rectangle.bottom = nativeRectangle[3];
    return true;
}

static bool trainingCurrentHorizontalBounds(
    uintptr_t fighter, qtraining::FighterHorizontalBounds& bounds) {
    TrainingNativeCollisionRectangle rectangle{};
    if (!trainingCurrentCollisionRectangle(fighter, rectangle))
        return false;
    const int32_t x =
        static_cast<int32_t>(rd32(
            fighter + kTrainingFighterXOffset));
    const int64_t left =
        static_cast<int64_t>(rectangle.left) - x;
    const int64_t right =
        static_cast<int64_t>(rectangle.right) - x;
    if (left < INT_MIN || left > INT_MAX
        || right < INT_MIN || right > INT_MAX
        || left > right) {
        return false;
    }
    bounds.left = static_cast<int32_t>(left);
    bounds.right = static_cast<int32_t>(right);
    return true;
}

static bool trainingSetFacing(uintptr_t fighter, uint8_t desired) {
    if (!rangeReadable(fighter, addr::kFighterSize)
        || desired > 1u) {
        return false;
    }
    const uint8_t current =
        rd8(fighter + kTrainingFighterFacingOffset);
    if (current > 1u) return false;
    if (current != desired) {
        TrainingNativeCollisionRectangle preflight{};
        if (!trainingCurrentCollisionRectangle(fighter, preflight))
            return false;
        const auto flip = reinterpret_cast<TrainingFacingFlipFn>(
            kTrainingFacingFlipFn);
        if (flip(0, static_cast<int>(fighter)) != 1)
            return false;
    }
    return rd8(fighter + kTrainingFighterFacingOffset) == desired
        && rd32(fighter + kTrainingFighterFacingSnapshotOffset)
            == desired;
}

static bool trainingWritePosition(uintptr_t fighter,
                                  int32_t x, int32_t y) {
    if (!rangeReadable(fighter, addr::kFighterSize))
        return false;
    const uint8_t facing =
        rd8(fighter + kTrainingFighterFacingOffset);
    if (facing > 1u) return false;
    wr32(fighter + kTrainingFighterXOffset,
         static_cast<uint32_t>(x));
    wr32(fighter + kTrainingFighterYOffset,
         static_cast<uint32_t>(y));
    wr32(fighter + kTrainingFighterPreviousXOffset,
         static_cast<uint32_t>(x));
    wr32(fighter + kTrainingFighterFacingSnapshotOffset,
         facing);
    return static_cast<int32_t>(rd32(
               fighter + kTrainingFighterXOffset)) == x
        && static_cast<int32_t>(rd32(
               fighter + kTrainingFighterYOffset)) == y
        && static_cast<int32_t>(rd32(
               fighter + kTrainingFighterPreviousXOffset)) == x
        && rd32(fighter + kTrainingFighterFacingSnapshotOffset)
            == facing;
}

static bool trainingClampFighterToWorld(
    uintptr_t fighter, bool& moved) {
    moved = false;
    TrainingNativeCollisionRectangle preflight{};
    if (!trainingCurrentCollisionRectangle(fighter, preflight))
        return false;

    const auto clamp = reinterpret_cast<TrainingWorldClampFn>(
        kTrainingWorldClampFn);
    for (uint32_t iteration = 0;
         iteration < kTrainingWorldClampIterationCap;
         ++iteration) {
        const int32_t before =
            static_cast<int32_t>(rd32(
                fighter + kTrainingFighterXOffset));
        const int nativeResult =
            clamp(static_cast<int>(fighter), 0, 2);
        const int32_t after =
            static_cast<int32_t>(rd32(
                fighter + kTrainingFighterXOffset));
        if (nativeResult == 0) {
            if (after != before) return false;
            TrainingNativeCollisionRectangle finalRectangle{};
            return trainingCurrentCollisionRectangle(
                       fighter, finalRectangle)
                && finalRectangle.left >= 0
                && finalRectangle.right <= 511;
        }
        if (nativeResult != 1 && nativeResult != 2)
            return false;
        const int64_t expected =
            static_cast<int64_t>(before)
            + (nativeResult == 1 ? 1 : -1);
        if (expected < INT_MIN || expected > INT_MAX
            || after != static_cast<int32_t>(expected)) {
            return false;
        }
        moved = true;
    }
    return false;
}

static bool trainingFighterPositionMatches(
    uintptr_t fighter, int32_t x, int32_t y, uint8_t facing,
    TrainingNativeCollisionRectangle& rectangle) {
    return rangeReadable(fighter, addr::kFighterSize)
        && static_cast<int32_t>(rd32(
               fighter + kTrainingFighterXOffset)) == x
        && static_cast<int32_t>(rd32(
               fighter + kTrainingFighterYOffset)) == y
        && static_cast<int32_t>(rd32(
               fighter + kTrainingFighterPreviousXOffset)) == x
        && rd8(fighter + kTrainingFighterFacingOffset) == facing
        && rd32(fighter + kTrainingFighterFacingSnapshotOffset)
            == facing
        && trainingCurrentCollisionRectangle(fighter, rectangle)
        && rectangle.left >= 0 && rectangle.right <= 511;
}

static bool trainingCollisionPairSeparated(
    const TrainingNativeCollisionRectangle& p1,
    const TrainingNativeCollisionRectangle& p2,
    bool p1IsLeft, int32_t minimumEdgeGap) {
    const TrainingNativeCollisionRectangle& left = p1IsLeft ? p1 : p2;
    const TrainingNativeCollisionRectangle& right = p1IsLeft ? p2 : p1;
    if (static_cast<int64_t>(left.left) > right.left
        || static_cast<int64_t>(left.right) > right.right) {
        return false;
    }
    return minimumEdgeGap <= 0
        || static_cast<int64_t>(left.right)
               + minimumEdgeGap <= right.left;
}

static bool trainingPositionCommitMatches(
    const qtraining::PositionPlan& plan,
    const qtraining::PositionMathConfig& config,
    uintptr_t p1, uintptr_t p2,
    uintptr_t p1Alt, uintptr_t p2Alt) {
    TrainingNativeCollisionRectangle p1Rectangle{};
    TrainingNativeCollisionRectangle p2Rectangle{};
    TrainingNativeCollisionRectangle p1AltRectangle{};
    TrainingNativeCollisionRectangle p2AltRectangle{};
    if (!trainingFighterPositionMatches(
            p1, plan.p1X, 0, plan.p1Facing, p1Rectangle)
        || !trainingFighterPositionMatches(
            p2, plan.p2X, 0, plan.p2Facing, p2Rectangle)
        || (p1Alt && !trainingFighterPositionMatches(
            p1Alt, plan.p1X, 0, plan.p1Facing,
            p1AltRectangle))
        || (p2Alt && !trainingFighterPositionMatches(
            p2Alt, plan.p2X, 0, plan.p2Facing,
            p2AltRectangle))) {
        return false;
    }

    qtraining::PositionMathConfig actualConfig = config;
    if (!trainingCurrentHorizontalBounds(
            p1, actualConfig.p1Bounds)
        || !trainingCurrentHorizontalBounds(
            p2, actualConfig.p2Bounds)
        || qtraining::validatePositionPlan(plan, actualConfig)
            != qtraining::PositionPlanError::None) {
        return false;
    }
    const int64_t separation =
        static_cast<int64_t>(plan.p2X) - plan.p1X;
    const bool p1IsLeft = separation >= 0;
    const int64_t absoluteSeparation =
        p1IsLeft ? separation : -separation;
    if (absoluteSeparation != plan.anchorSeparation
        || !trainingCollisionPairSeparated(
            p1Rectangle, p2Rectangle, p1IsLeft,
            config.minimumEdgeGap)
        || (p1Alt && p2Alt
            && !trainingCollisionPairSeparated(
                p1AltRectangle, p2AltRectangle, p1IsLeft,
                config.minimumEdgeGap))) {
        return false;
    }

    return static_cast<int32_t>(rd32(kTrainingCameraAddr))
            == plan.cameraX
        && static_cast<int32_t>(rd32(
               kTrainingCameraPreviousAddr)) == plan.cameraX
        && rd32(kTrainingCameraParityAddr) == 0u
        && static_cast<int32_t>(rd32(kTrainingCameraYAddr)) == 0
        && rd32(kRoundResultStateAddr) == 0u;
}

static qtraining::PositionMathConfig
trainingDefaultPositionMathConfig() {
    qtraining::PositionMathConfig config{};
    config.worldLeft = 0;
    config.worldRight = 511;
    config.cameraMin = 0;
    config.cameraMax = 192;
    config.cameraBias = 32;
    config.anchorSeparation = 128;
    config.closeContactGap = 0;
    config.minimumEdgeGap = 0;
    config.nativeP1X = 64;
    config.nativeP2X = 192;
    config.nativeCameraX = 96;
    return config;
}

static void trainingPresetFacing(PositionPreset preset,
                                 uint8_t& p1Facing,
                                 uint8_t& p2Facing) {
    p1Facing = preset == PositionPreset::LeftCorner ? 1u : 0u;
    p2Facing = preset == PositionPreset::LeftCorner ? 0u : 1u;
}

static bool trainingResetPairToPreset(PositionPreset preset);

static bool trainingApplyPositionPreset(PositionPreset preset) {
    const uintptr_t p1 = trainingFighter(0u, false);
    const uintptr_t p2 = trainingFighter(1u, false);
    const uintptr_t p1Alt = trainingAlternateFormActor(0u);
    const uintptr_t p2Alt = trainingAlternateFormActor(1u);
    if (!p1 || !p2) return false;

    uint8_t p1Facing = 0u;
    uint8_t p2Facing = 1u;
    trainingPresetFacing(preset, p1Facing, p2Facing);
    TrainingNativeCollisionRectangle preflight{};
    if (!trainingCurrentCollisionRectangle(p1, preflight)
        || !trainingCurrentCollisionRectangle(p2, preflight)
        || (p1Alt && !trainingCurrentCollisionRectangle(p1Alt, preflight))
        || (p2Alt && !trainingCurrentCollisionRectangle(p2Alt, preflight))
        || !trainingSetFacing(p1, p1Facing)
        || !trainingSetFacing(p2, p2Facing)
        || (p1Alt && !trainingSetFacing(p1Alt, p1Facing))
        || (p2Alt && !trainingSetFacing(p2Alt, p2Facing))) {
        return false;
    }

    qtraining::PositionMathConfig config =
        trainingDefaultPositionMathConfig();
    if (!trainingCurrentHorizontalBounds(p1, config.p1Bounds)
        || !trainingCurrentHorizontalBounds(p2, config.p2Bounds)) {
        return false;
    }
    const qtraining::PositionPlan plan =
        qtraining::planPositionPreset(preset, config);
    if (!plan.valid
        || plan.p1Facing != p1Facing
        || plan.p2Facing != p2Facing) {
        return false;
    }

    if (!trainingWritePosition(p1, plan.p1X, 0)
        || !trainingWritePosition(p2, plan.p2X, 0)
        || (p1Alt && !trainingWritePosition(p1Alt, plan.p1X, 0))
        || (p2Alt && !trainingWritePosition(p2Alt, plan.p2X, 0))) {
        return false;
    }

    const uintptr_t fighters[] = {
        p1, p2, p1Alt, p2Alt,
    };
    for (uintptr_t fighter : fighters) {
        if (!fighter) continue;
        bool nativeMoved = false;
        if (!trainingClampFighterToWorld(fighter, nativeMoved)
            || nativeMoved) {
            return false;
        }
    }

    wr32(kTrainingCameraAddr,
         static_cast<uint32_t>(plan.cameraX));
    wr32(kTrainingCameraPreviousAddr,
         static_cast<uint32_t>(plan.cameraX));
    wr32(kTrainingCameraParityAddr, 0u);
    wr32(kTrainingCameraYAddr, 0u);

    if (!trainingPositionCommitMatches(
            plan, config, p1, p2, p1Alt, p2Alt)
        || !trainingRepairHumanOwnership()) {
        return false;
    }
    g_trainingLastPositionPlanX[0] = plan.p1X;
    g_trainingLastPositionPlanX[1] = plan.p2X;
    g_trainingLastPositionPlanFacing[0] = plan.p1Facing;
    g_trainingLastPositionPlanFacing[1] = plan.p2Facing;
    g_trainingLastPositionPlanValid = true;
    trainingStopTransientSfx();
    return true;
}

static PostureClass trainingReadPostureClass(uintptr_t fighter) {
    if (!fighter) return PostureClass::Unreadable;
    return qtraining::classifyPostureNibble(
        static_cast<uint8_t>(
            rd32(fighter + kTrainingFighterMoveGateOffset) & 0xFFu));
}

constexpr uintptr_t kTrainingActorActionTableOffset = 16u;
constexpr uintptr_t kTrainingActorFrameTableOffset = 24u;
constexpr uintptr_t kTrainingActorAttackTableOffset = 40u;
constexpr uintptr_t kTrainingActionEntrySize = 24u;
constexpr uintptr_t kTrainingActionEntryFrameBase = 0u;
constexpr uintptr_t kTrainingActionEntryAttackBase = 16u;
constexpr uintptr_t kTrainingChrFrameRecordStartOffset = 31u;
constexpr uintptr_t kTrainingChrFrameRecordCountOffset = 32u;
constexpr uintptr_t kTrainingAttackRecordSize = 24u;
constexpr uintptr_t kTrainingAttackRecordPermissionOffset = 17u;
constexpr uintptr_t kTrainingAttackRecordSelectorOffset = 18u;
constexpr uint8_t kTrainingAttackRecordOrdinarySelectorMax = 12u;
// Type 15 is the capture handshake (sub_464AA0): the victim escapes unless (raw+17 & victim+156) == 0. Types 13/14 are attack-versus-attack.
constexpr uint8_t kTrainingAttackRecordCaptureSelector = 15u;
// u16 next = frame[+12]: 0xFFFE terminates, 0xFFFF advances linearly, and any other value jumps to that local frame index.
constexpr uintptr_t kTrainingChrFrameNextOffset = 12u;
constexpr uint16_t kTrainingChrFrameNextTerminal = 0xFFFEu;
constexpr uint16_t kTrainingChrFrameNextLinear = 0xFFFFu;
static_assert(kTrainingChrFrameNextOffset + 2u <= kTrainingChrFrameRecordSize,
              "the frame link must lie inside the CHR frame");
// Frame byte +32: 0 clears the attacker's collision flags, 254 is no-collision, 255 is a one-record probe; never iterate 254 or 255 records.
constexpr uint8_t kTrainingFrameNoCollisionSentinel = 254u;
constexpr uint8_t kTrainingFrameProbeSentinel = 255u;
constexpr uint32_t kTrainingMaxScannedAttackRecords = 32u;
static_assert(kTrainingActorActionTableOffset + 4u <= addr::kFighterSize
                  && kTrainingActorFrameTableOffset + 4u <= addr::kFighterSize
                  && kTrainingActorAttackTableOffset + 4u
                      <= addr::kFighterSize,
              "attack-record table pointers must lie inside the fighter");
static_assert(kTrainingChrFrameRecordStartOffset + 1u
                      <= kTrainingChrFrameRecordSize
                  && kTrainingChrFrameRecordCountOffset + 1u
                      <= kTrainingChrFrameRecordSize,
              "frame record-index bytes must lie inside the CHR frame");
static_assert(kTrainingAttackRecordPermissionOffset + 1u
                      <= kTrainingAttackRecordSize
                  && kTrainingAttackRecordSelectorOffset + 1u
                      <= kTrainingAttackRecordSize,
              "attack-record reads must lie inside the 24-byte record");

struct TrainingGuardEvidenceReadout {
    bool scanned = false;
    bool valid = false;
    bool fromLookahead = false;
    bool held = false;
    uint32_t recordCount = 0u;
    uint8_t legalPostureMask = 0u;
};
static TrainingGuardEvidenceReadout g_trainingGuardEvidence{};

constexpr uint32_t kTrainingGuardPostureHoldSteps = 10u;

struct TrainingGuardPostureHold {
    uint8_t mask = 0u;
    uint8_t captureMask = 0u;
    uint32_t captureCount = 0u;
    uint32_t stepsRemaining = 0u;
};
static TrainingGuardPostureHold g_trainingGuardHold{};

struct TrainingActiveGuardEvidence {
    bool valid = false;
    uint32_t recordCount = 0u;
    uint8_t legalPostureMask = 0u;
    bool fromLookahead = false;
    uint32_t captureCount = 0u;
    uint8_t captureAvoidMask = 0u;
};

constexpr uint32_t kTrainingGuardLookaheadTicks = 32u;
constexpr uint32_t kTrainingGuardLookaheadMaxFrameTicks = 240u;
static bool g_trainingGuardTraceEnabled = false;
constexpr uint32_t kTrainingGuardFrameDurationSentinel = 0xFFFFu;
constexpr uint32_t kTrainingGuardSentinelNominalTicks = 2u;
constexpr uint32_t kTrainingGuardLookaheadFrames = 24u;

enum class TrainingGuardLookaheadStop : uint8_t {
    Found = 0,
    BudgetSpent = 1,
    FrameUnreadable = 2,
    Terminal = 3,
    BadDuration = 4,
    StepsExhausted = 5,
};

static const char* trainingGuardLookaheadStopName(
        TrainingGuardLookaheadStop stop) {
    switch (stop) {
        case TrainingGuardLookaheadStop::Found: return "found";
        case TrainingGuardLookaheadStop::BudgetSpent: return "budget";
        case TrainingGuardLookaheadStop::FrameUnreadable: return "unreadable";
        case TrainingGuardLookaheadStop::Terminal: return "terminal";
        case TrainingGuardLookaheadStop::BadDuration: return "duration";
        case TrainingGuardLookaheadStop::StepsExhausted: return "steps";
    }
    return "?";
}

static TrainingGuardLookaheadStop g_trainingGuardLookaheadStop =
    TrainingGuardLookaheadStop::StepsExhausted;
static uint32_t g_trainingGuardLookaheadTicksSeen = 0u;

static bool trainingScanUpcomingGuardRecords(
        uintptr_t frameTable, uintptr_t attackTable, uint32_t frameBase,
        uint32_t attackBase, uint32_t localFrame, uint32_t ticksAlreadyOwed,
        uint8_t& outLegalPostureMask, uint32_t& outCaptureCount,
        uint8_t& outCaptureAvoidMask) {
    g_trainingGuardLookaheadStop = TrainingGuardLookaheadStop::StepsExhausted;
    g_trainingGuardLookaheadTicksSeen = ticksAlreadyOwed;
    uint32_t ticks =
        ticksAlreadyOwed >= kTrainingGuardFrameDurationSentinel
            ? kTrainingGuardSentinelNominalTicks
            : ticksAlreadyOwed;
    uint32_t cursor = localFrame;
    for (uint32_t step = 0u; step < kTrainingGuardLookaheadFrames; ++step) {
        if (ticks >= kTrainingGuardLookaheadTicks) {
            g_trainingGuardLookaheadStop =
                TrainingGuardLookaheadStop::BudgetSpent;
            g_trainingGuardLookaheadTicksSeen = ticks;
            return false;
        }
        const uintptr_t frame =
            frameTable
            + kTrainingChrFrameRecordSize
                  * (static_cast<uintptr_t>(frameBase) + cursor);
        if (!rangeReadable(frame, kTrainingChrFrameRecordSize)) {
            g_trainingGuardLookaheadStop =
                TrainingGuardLookaheadStop::FrameUnreadable;
            return false;
        }
        const uint16_t link =
            rd16(frame + kTrainingChrFrameNextOffset);
        const uint32_t duration =
            rd32(frame + kTrainingChrFrameDurationOffset);
        if (step != 0u) {
            const uint8_t recordCount =
                rd8(frame + kTrainingChrFrameRecordCountOffset);
            if (recordCount != 0u
                && recordCount != kTrainingFrameNoCollisionSentinel
                && recordCount != kTrainingFrameProbeSentinel
                && recordCount <= kTrainingMaxScannedAttackRecords) {
                const uint32_t recordStart =
                    attackBase
                    + rd8(frame + kTrainingChrFrameRecordStartOffset);
                uint8_t mask = 0u;
                uint32_t counted = 0u;
                uint8_t avoid = 0u;
                uint32_t captured = 0u;
                for (uint32_t index = 0u; index < recordCount; ++index) {
                    const uintptr_t record =
                        attackTable
                        + kTrainingAttackRecordSize
                              * (static_cast<uintptr_t>(recordStart) + index);
                    if (!rangeReadable(record, kTrainingAttackRecordSize))
                        return false;
                    const uint8_t selector =
                        rd8(record + kTrainingAttackRecordSelectorOffset);
                    const uint8_t raw =
                        rd8(record + kTrainingAttackRecordPermissionOffset)
                        & qtraining::kGuardPermissionMask;
                    if (selector
                        == kTrainingAttackRecordCaptureSelector) {
                        avoid = captured == 0u
                            ? raw
                            : static_cast<uint8_t>(avoid & raw);
                        ++captured;
                        continue;
                    }
                    if (selector
                        > kTrainingAttackRecordOrdinarySelectorMax) {
                        continue;
                    }
                    mask = counted == 0u
                        ? raw
                        : static_cast<uint8_t>(mask & raw);
                    ++counted;
                }
                if (counted != 0u || captured != 0u) {
                    outLegalPostureMask =
                        counted != 0u ? mask : qtraining::kGuardPermissionMask;
                    outCaptureCount = captured;
                    outCaptureAvoidMask = avoid;
                    g_trainingGuardLookaheadStop =
                        TrainingGuardLookaheadStop::Found;
                    g_trainingGuardLookaheadTicksSeen = ticks;
                    return true;
                }
            }
        }
        if (link == kTrainingChrFrameNextTerminal) {
            g_trainingGuardLookaheadStop =
                TrainingGuardLookaheadStop::Terminal;
            return false;
        }
        if (duration == 0u
            || (duration > kTrainingGuardLookaheadMaxFrameTicks
                && duration != kTrainingGuardFrameDurationSentinel)) {
            g_trainingGuardLookaheadStop =
                TrainingGuardLookaheadStop::BadDuration;
            g_trainingGuardLookaheadTicksSeen = duration;
            return false;
        }
        ticks += duration == kTrainingGuardFrameDurationSentinel
            ? kTrainingGuardSentinelNominalTicks
            : duration;
        const uint32_t next =
            link == kTrainingChrFrameNextLinear ? cursor + 1u : link;
        if (next <= cursor) return false;
        cursor = next;
    }
    return false;
}

static void trainingAccumulateActiveGuardRecords(
        uintptr_t actor, TrainingActiveGuardEvidence& out) {
    if (actor == 0u || !trainingCombatFrameReadable(actor)) return;
    const uintptr_t actionTable = rd32(actor + kTrainingActorActionTableOffset);
    const uintptr_t frameTable = rd32(actor + kTrainingActorFrameTableOffset);
    const uintptr_t attackTable = rd32(actor + kTrainingActorAttackTableOffset);
    if (actionTable == 0u || frameTable == 0u || attackTable == 0u) return;
    const uint32_t action = rd32(actor + kTrainingFighterActionStateOffset);
    if (action > 255u) {
        out.valid = false;
        return;
    }

    const uintptr_t actionEntry =
        actionTable + kTrainingActionEntrySize * action;
    if (!rangeReadable(actionEntry, kTrainingActionEntrySize)) {
        out.valid = false;
        return;
    }
    const uint32_t frameBase =
        rd32(actionEntry + kTrainingActionEntryFrameBase);
    const uint32_t attackBase =
        rd32(actionEntry + kTrainingActionEntryAttackBase);
    const uint32_t localFrame = rd32(actor + kTrainingFighterAnimFrameOffset);
    const uintptr_t frame =
        frameTable
        + kTrainingChrFrameRecordSize
              * (static_cast<uintptr_t>(frameBase) + localFrame);
    if (!rangeReadable(frame, kTrainingChrFrameRecordSize)) {
        out.valid = false;
        return;
    }
    const uint32_t parked =
        rd32(actor + kTrainingFighterFrameRecordPtrOffset);
    if (parked != 0u && static_cast<uintptr_t>(parked) != frame) {
        out.valid = false;
        return;
    }
    const uint8_t recordCount =
        rd8(frame + kTrainingChrFrameRecordCountOffset);
    if (recordCount == 0u
        || recordCount == kTrainingFrameNoCollisionSentinel
        || recordCount == kTrainingFrameProbeSentinel) {
        uint8_t upcoming = 0u;
        uint32_t upcomingCaptures = 0u;
        uint8_t upcomingAvoid = 0u;
        if (trainingScanUpcomingGuardRecords(
                frameTable, attackTable, frameBase, attackBase, localFrame,
                rd32(actor + kTrainingFighterActionLockEndOffset), upcoming,
                upcomingCaptures, upcomingAvoid)) {
            out.legalPostureMask = out.recordCount == 0u
                ? upcoming
                : static_cast<uint8_t>(out.legalPostureMask & upcoming);
            ++out.recordCount;
            if (upcomingCaptures != 0u) {
                out.captureAvoidMask = out.captureCount == 0u
                    ? upcomingAvoid
                    : static_cast<uint8_t>(
                          out.captureAvoidMask & upcomingAvoid);
                out.captureCount += upcomingCaptures;
            }
            out.fromLookahead = true;
        }
        return;
    }
    if (recordCount > kTrainingMaxScannedAttackRecords) {
        out.valid = false;
        return;
    }
    uint8_t aheadMask = 0u;
    uint32_t aheadCaptures = 0u;
    uint8_t aheadAvoid = 0u;
    const bool haveAhead = trainingScanUpcomingGuardRecords(
        frameTable, attackTable, frameBase, attackBase, localFrame,
        rd32(actor + kTrainingFighterActionLockEndOffset), aheadMask,
        aheadCaptures, aheadAvoid);
    const uint32_t recordStart =
        attackBase + rd8(frame + kTrainingChrFrameRecordStartOffset);
    for (uint32_t index = 0u; index < recordCount; ++index) {
        const uintptr_t record =
            attackTable
            + kTrainingAttackRecordSize
                  * (static_cast<uintptr_t>(recordStart) + index);
        if (!rangeReadable(record, kTrainingAttackRecordSize)) {
            out.valid = false;
            return;
        }
        const uint8_t selector =
            rd8(record + kTrainingAttackRecordSelectorOffset);
        const uint8_t raw =
            rd8(record + kTrainingAttackRecordPermissionOffset);
        if (selector == kTrainingAttackRecordCaptureSelector) {
            const uint8_t avoid = raw & qtraining::kGuardPermissionMask;
            out.captureAvoidMask = out.captureCount == 0u
                ? avoid
                : static_cast<uint8_t>(out.captureAvoidMask & avoid);
            ++out.captureCount;
            continue;
        }
        if (selector > kTrainingAttackRecordOrdinarySelectorMax) continue;
        const uint8_t legal = raw & qtraining::kGuardPermissionMask;
        out.legalPostureMask = out.recordCount == 0u
            ? legal
            : static_cast<uint8_t>(out.legalPostureMask & legal);
        ++out.recordCount;
    }

    const uint8_t nowMask = out.legalPostureMask;
    if (haveAhead && aheadMask != 0u) {
        const uint8_t merged =
            static_cast<uint8_t>(out.legalPostureMask & aheadMask);
        if (out.recordCount == 0u) {
            out.legalPostureMask = aheadMask;
            ++out.recordCount;
        } else if (merged != 0u) {
            out.legalPostureMask = merged;
        } else {
            out.legalPostureMask = aheadMask;
        }
        out.fromLookahead = true;
    }
    if (g_trainingGuardTraceEnabled)
    QLOG("[training-guard] attacker action=%lu frame=%lu lock=%lu recs=%u "
         "now=%02X ahead=%02X(%s@%lu)%s chose=%02X captures=%lu",
         static_cast<unsigned long>(action),
         static_cast<unsigned long>(localFrame),
         static_cast<unsigned long>(
             rd32(actor + kTrainingFighterActionLockEndOffset)),
         static_cast<unsigned>(recordCount),
         static_cast<unsigned>(nowMask),
         static_cast<unsigned>(haveAhead ? aheadMask : 0u),
         trainingGuardLookaheadStopName(g_trainingGuardLookaheadStop),
         static_cast<unsigned long>(g_trainingGuardLookaheadTicksSeen),
         (haveAhead && aheadMask != 0u && aheadMask != nowMask)
             ? " TRANSITION" : "",
         static_cast<unsigned>(out.legalPostureMask),
         static_cast<unsigned long>(out.captureCount));

    if (haveAhead && aheadCaptures != 0u) {
        out.captureAvoidMask = out.captureCount == 0u
            ? aheadAvoid
            : static_cast<uint8_t>(out.captureAvoidMask & aheadAvoid);
        out.captureCount += aheadCaptures;
        out.fromLookahead = true;
    }
}

static qtraining::GuardDecision trainingGuardDecision(
    uint8_t defenderFacing) {
    qtraining::GuardDecisionInput input{};
    const GuardMode effectiveGuardMode = g_trainingGuardTimingArmed
        ? g_trainingGuardMode : GuardMode::Off;
    input.mode = effectiveGuardMode;
    const uintptr_t defender = trainingFighter(trainingPolicySlot(), false);
    if (!defender) {
        input.defenderFacing = 0xFFu;
        return qtraining::chooseGuardDirections(input);
    }
    input.defenderFacing = defenderFacing;
    input.defenderAirborne =
        static_cast<int32_t>(
            rd32(defender + kTrainingFighterYOffset)) != 0;
    input.autoPredictionQualified = false;

    input.defenderPosture = trainingReadPostureClass(defender);
    input.defenderInTechResult = qtraining::combatActionIsGroundRecovery(
        rd32(defender + kTrainingFighterActionStateOffset));
    const uintptr_t attackers[] = {
        trainingFighter(trainingPolicyPressureSlot(), false),
        trainingAlternateFormActor(trainingPolicyPressureSlot()),
    };
    for (uintptr_t attacker : attackers) {
        if (!attacker) continue;
        const PostureClass posture = trainingReadPostureClass(attacker);
        if (posture == PostureClass::Unreadable) continue;
        ++input.attackerReadableCount;
        if (qtraining::postureClassGrounded(posture))
            ++input.attackerGroundedCount;
    }

    if (effectiveGuardMode == GuardMode::AutoRecord) {
        TrainingActiveGuardEvidence evidence{};
        evidence.valid = true;
        for (uintptr_t attacker : attackers) {
            if (!attacker) continue;
            trainingAccumulateActiveGuardRecords(attacker, evidence);
            if (!evidence.valid) break;
        }
        bool held = false;
        if (evidence.valid
            && (evidence.recordCount != 0u || evidence.captureCount != 0u)) {
            g_trainingGuardHold.mask = evidence.legalPostureMask;
            g_trainingGuardHold.captureMask = evidence.captureAvoidMask;
            g_trainingGuardHold.captureCount = evidence.captureCount;
            g_trainingGuardHold.stepsRemaining =
                kTrainingGuardPostureHoldSteps;
        } else if (g_trainingGuardHold.stepsRemaining != 0u) {
            --g_trainingGuardHold.stepsRemaining;
            evidence.valid = true;
            evidence.recordCount = 1u;
            evidence.legalPostureMask = g_trainingGuardHold.mask;
            evidence.captureCount = g_trainingGuardHold.captureCount;
            evidence.captureAvoidMask = g_trainingGuardHold.captureMask;
            held = true;
        }
        input.recordEvidenceValid = evidence.valid;
        input.activeRecordCount = evidence.recordCount;
        input.activeLegalPostureMask = evidence.legalPostureMask;
        input.captureRecordCount = evidence.captureCount;
        input.captureAvoidPostureMask = evidence.captureAvoidMask;
        g_trainingGuardEvidence.held = held;
        g_trainingGuardEvidence.scanned = true;
        g_trainingGuardEvidence.valid = evidence.valid;
        g_trainingGuardEvidence.fromLookahead = evidence.fromLookahead;
        g_trainingGuardEvidence.recordCount = evidence.recordCount;
        g_trainingGuardEvidence.legalPostureMask = evidence.legalPostureMask;
    } else {
        g_trainingGuardEvidence = TrainingGuardEvidenceReadout{};
        g_trainingGuardHold = TrainingGuardPostureHold{};
    }

    if (effectiveGuardMode == GuardMode::AlwaysStand) {
        input.permission = qtraining::kGuardPermitStand;
    } else if (effectiveGuardMode == GuardMode::AlwaysCrouch) {
        input.permission = qtraining::kGuardPermitCrouch;
    } else if (effectiveGuardMode == GuardMode::AutoRecord
               && input.recordEvidenceValid
               && input.activeRecordCount != 0u) {
        input.permission = input.activeLegalPostureMask;
    } else if (effectiveGuardMode == GuardMode::AlwaysAir) {
        input.permission = qtraining::kGuardPermitAir;
    }

    g_trainingGuardPermission = input.permission;
    g_trainingGuardClassification =
        qtraining::classifyGuardPermission(input.permission);
    const qtraining::GuardDecision decision =
        qtraining::chooseGuardDirections(input);
    if (decision.postureDivergedFromY)
        ++g_trainingGuardPostureDivergences;
    if (decision.attackerPostureAmbiguous)
        ++g_trainingGuardAmbiguousAttackers;
    if (decision.defenderPostureUnguardable)
        ++g_trainingGuardUnguardablePostures;
    return decision;
}

static bool trainingSampleRecoveryObservation(
        uint8_t owner,
        uint8_t defenderFacing, bool menuCaptured, bool operationNeutral,
        bool macroPlaybackActive, bool macroOverrideP1,
        qtraining::RecoveryObservation& out) {
    out = qtraining::RecoveryObservation{};
    if (owner >= 2u) return false;
    out.trainingGeneration = g_trainingGeneration;
    out.hitEpoch = g_trainingHitEpoch[owner];
    out.lifecycleAvailable = true;
    out.sandboxActive =
        g_trainingLaunchRequested
        && g_trainingHooksSupported
        && g_trainingHooksInstalled
        && !g_trainingMutationFailed
        && !g_netActive
        && g_mode == RunMode::Observe;
    out.menuCaptured = menuCaptured;
    out.operationNeutral = operationNeutral;
    out.macroPlaybackActive = macroPlaybackActive;
    out.macroOverrideP1 = macroOverrideP1;

    const uintptr_t defender = trainingFighter(owner, false);
    if (!defender) return false;

    const uint32_t slot = rd8(defender + kFighterPlayerSlotOffset);
    if (slot >= qtraining::kTrainingRefillPlayerCount) return false;

    out.slot = static_cast<uint8_t>(slot);
    out.facing = defenderFacing;
    out.action = rd32(defender + kTrainingFighterActionStateOffset);
    out.nextAction = rd32(defender + kTrainingFighterNextActionOffset);
    out.transitionFlag =
        rd32(defender + kTrainingFighterTransitionFlagOffset);
    out.committedAction =
        rd32(defender + kTrainingFighterDispatchActionOffset);
    out.initStatus = static_cast<int32_t>(
        rd32(defender + kTrainingFighterInitStatusOffset));
    out.animFrame = rd32(defender + kTrainingFighterAnimFrameOffset);
    out.postureNibble = static_cast<uint8_t>(
        rd32(defender + kTrainingFighterMoveGateOffset) & 0x0Fu);
    // +158 is a WORD write of a nibble (sub_411AE0).
    uint8_t permission = static_cast<uint8_t>(
        rd32(defender + kTrainingFighterAuthoredFlagsOffset) & 0x0Fu);
    const uint32_t alternateForm =
        rd32(defender + kTrainingFighterAlternateFormOffset);
    if (alternateForm == 1u) {
        const uintptr_t partner = trainingAlternateFormActor(owner);
        if (partner) {
            permission |= static_cast<uint8_t>(
                rd32(partner + kTrainingFighterAuthoredFlagsOffset)
                & 0x0Fu);
        }
    }
    out.permission = permission;
    out.wallFlags = static_cast<uint16_t>(
        rd32(defender + kTrainingFighterWallFlagsOffset) & 3u);
    out.terminalReady =
        (rd32(defender + kTrainingFighterTerminalReadyOffset) & 1u) != 0u;
    out.koLatch = rd32(defender + kFighterRoundLostOffset) != 0u;
    out.comControlled =
        rd32(defender + kTrainingFighterComOffset) != 0u;

    const uintptr_t record = kTrainingCombatRecordBase
        + static_cast<uintptr_t>(slot) * kTrainingCombatRecordStride;
    if (!rangeReadable(record, kTrainingCombatRecordStride)) return false;
    out.recoveryLock = rd32(record + kTrainingRecoveryLockOffset);
    out.recoveryPhaseLatch =
        rd32(record + kTrainingRecoveryPhaseLatchOffset);

    const uintptr_t child =
        rd32(defender + kTrainingFighterCommandChildOffset);
    if (!rangeReadable(child, kTrainingCommandChildSize)) return false;
    out.commandChild = static_cast<uint32_t>(child);
    out.directionSuppressed =
        rd32(child + kTrainingCommandDirectionKillOffset) == 1u;
    const uint32_t directionHead =
        rd8(child + kTrainingCommandDirHeadOffset)
        & kTrainingCommandDirRingMask;
    const uint32_t buttonHead =
        rd8(child + kTrainingCommandButtonHeadOffset)
        & kTrainingCommandButtonRingMask;
    out.directionRingHead = rd32(
        child + kTrainingCommandDirRingOffset + 4u * directionHead);
    out.buttonRingHead = rd32(
        child + kTrainingCommandButtonRingOffset + 4u * buttonHead);

    out.readable = true;
    return true;
}

static bool trainingReadDirectionRing(uintptr_t fighter,
                                      qtraining::DirectionRingSnapshot& out) {
    if (fighter == 0u) return false;
    const uintptr_t child =
        rd32(fighter + kTrainingFighterCommandChildOffset);
    if (!rangeReadable(child, kTrainingCommandChildSize)) return false;
    for (uint32_t i = 0u; i < qtraining::kRecoveryDirectionRingSize; ++i) {
        out.values[i] =
            rd32(child + kTrainingCommandDirRingOffset + 4u * i);
        out.dwell[i] =
            rd32(child + kTrainingCommandDirDwellOffset + 4u * i);
    }
    out.head = static_cast<uint8_t>(
        rd8(child + kTrainingCommandDirHeadOffset)
        & (qtraining::kRecoveryDirectionRingSize - 1u));
    return true;
}

static uint32_t g_trainingGuardRollAvoidedSteps = 0u;

static void trainingLogDirectionRing(const char* what, uint32_t fromAction,
                                     uint32_t toAction,
                                     const qtraining::DirectionRingSnapshot& ring) {
    char text[160];
    size_t used = 0u;
    for (uint32_t back = 0u; back < 6u && used < sizeof(text); ++back) {
        const uint32_t index = (ring.head - back)
            & (qtraining::kRecoveryDirectionRingSize - 1u);
        const int wrote = std::snprintf(
            text + used, sizeof(text) - used, "%s%05lX:%lu",
            back == 0u ? "" : " ",
            static_cast<unsigned long>(ring.values[index]),
            static_cast<unsigned long>(ring.dwell[index]));
        if (wrote < 0) break;
        used += static_cast<size_t>(wrote);
    }
    QLOG("[training-guard] %s action %lu->%lu step=%llu ring(head first, "
         "value:dwell)=%s",
         what, static_cast<unsigned long>(fromAction),
         static_cast<unsigned long>(toAction),
         static_cast<unsigned long long>(g_trainingStepSerial), text);
}

static void trainingGuardAvoidTerminalRoll(qtraining::GuardDecision& guard) {
    const uintptr_t defender = trainingFighter(trainingPolicySlot(), false);
    const uint32_t action = defender != 0u
        ? rd32(defender + kTrainingFighterActionStateOffset) : 0xFFFFFFFFu;

    static uintptr_t s_previousDefender = 0u;
    static uint32_t s_previousAction = 0xFFFFFFFFu;
    if (defender != 0u && defender == s_previousDefender
        && qtraining::recoveryTerminalEligibleAction(s_previousAction)
        && (action == 13u || action == 14u)
        && g_trainingRecoverySettings.terminalChoice
               == TerminalRecoveryChoice::Preserve) {
        qtraining::DirectionRingSnapshot ring{};
        if (trainingReadDirectionRing(defender, ring))
            trainingLogDirectionRing("native terminal ROLL (not configured)",
                                     s_previousAction, action, ring);
    }
    s_previousDefender = defender;
    s_previousAction = action;

    if (!guard.valid || !guard.guarding || defender == 0u) return;
    constexpr uint32_t kHorizontal =
        qinput::kLogicalLeft | qinput::kLogicalRight;
    constexpr uint32_t kVertical = qinput::kLogicalUp | qinput::kLogicalDown;
    const uint32_t directions = guard.logicalDirections;
    if ((directions & kVertical) != 0u || (directions & kHorizontal) == 0u)
        return;
    if (!qtraining::recoveryTerminalEligibleAction(action)) return;
    qtraining::DirectionRingSnapshot ring{};
    if (!trainingReadDirectionRing(defender, ring)) return;
    if (!qtraining::terminalRecoveryWouldMatch(
            ring, qtraining::kRecoveryRingBack)) {
        return;
    }
    guard.logicalDirections = directions | qinput::kLogicalDown;
    if (g_trainingGuardRollAvoidedSteps++ < 64u) {
        trainingLogDirectionRing(
            "held down-back: plain back would have spelled the terminal roll",
            action, action, ring);
    }
}

static qtraining::DummyPostureResult trainingDummyPosture(
    const qtraining::GuardDecision& guard, uint8_t defenderFacing,
    const qtraining::RecoveryEmission& recovery) {
    qtraining::DummyPostureInput input{};
    input.guardMode = g_trainingGuardTimingArmed
        ? g_trainingGuardMode : GuardMode::Off;
    input.stance = g_trainingDummyStance;
    input.tech = g_trainingDummyTech;
    input.guarding = guard.guarding;
    input.guardDirections = guard.logicalDirections;
    input.defenderFacing = defenderFacing;
    input.recoveryActive = recovery.valid && recovery.active;
    input.recoveryDirections =
        input.recoveryActive ? recovery.directions : 0u;
    input.recoveryButtons =
        input.recoveryActive ? recovery.buttons : 0u;
    input.recoveryOwnsButtons =
        input.recoveryActive && recovery.ownsButtons;
    return qtraining::chooseDummyPosture(input);
}

static CommandAckResult trainingSaveState(uint32_t slotIndex) {
    if (slotIndex >= g_trainingStates.size())
        return CommandAckResult::RejectedInvalidSlot;
    return trainingCaptureSnapshot(g_trainingStates[slotIndex])
        ? CommandAckResult::Applied
        : CommandAckResult::SnapshotFailed;
}

static bool trainingRestoreScratchFailSafe();

static bool trainingRestoreScratchOrFailClosed(const char* failureText) {
    if (trainingRestoreScratchFailSafe()) return true;
    trainingFailMutation(
        TrainingDisableReason::SnapshotFailure, failureText);
    return false;
}

static CommandAckResult trainingLoadState(uint32_t slotIndex) {
    if (slotIndex >= g_trainingStates.size())
        return CommandAckResult::RejectedInvalidSlot;
    TrainingStateSlot& slot = g_trainingStates[slotIndex];
    if (!slot.valid || slot.stale)
        return CommandAckResult::RejectedStaleLayout;
    if (!trainingRestoreSnapshot(slot)) {
        (void)trainingRestoreScratchOrFailClosed(
            "Savestate load failed and stable scratch restore failed");
        return CommandAckResult::SnapshotFailed;
    }
    g_trainingInput.resumeAfterOperation(0u);
    (void)g_trainingMacros.requestStop();
    g_trainingOperationNeutral = true;
    trainingArmMacroAfterReset();
    trainingClearLastDamageEvent();
    return CommandAckResult::Applied;
}

static CommandAckResult trainingBeginReturnToCharacterSelect() {
    if (g_trainingReturningToSelect)
        return CommandAckResult::Applied;
    if (!g_trainingLaunchRequested
        || !g_trainingHooksSupported
        || !g_trainingHooksInstalled
        || g_netActive
        || g_mode != RunMode::Observe) {
        return CommandAckResult::RejectedUnavailable;
    }
    if (rd32(kSceneStateAddr) != 2u
        || rd32(kMatchSubAddr) != 0u
        || g_trainingStepPending) {
        return CommandAckResult::RejectedUnsafeScene;
    }

    g_trainingReturningToSelect = true;
    g_trainingLifecycle =
        TrainingLifecycleState::ReturningToCharacterSelect;
    g_trainingFinalInput[0] = g_trainingFinalInput[1] = 0u;
    g_trainingLastComposedInput[0] =
        g_trainingLastComposedInput[1] = 0u;
    qinput::inputDisplay().publishIdle();
    g_trainingInput.reset();
    g_trainingMacros.reset();
    trainingInvalidateStates(false);
    trainingInvalidateRoundCharacterState();
    g_trainingHitboxMode = qtraining::HitboxDisplayMode::Off;
    g_trainingHitboxOverlayVisible = false;
    qtraining::trainingHitboxes().publishHidden();
    wr8(kTrainingHitboxAddr, 0u);
    g_trainingRefillRequest = 0u;
    g_trainingMeterChargeCounter = 0u;
    trainingResetTimelineMachines();
    g_trainingRecoveryReadoutValid = 0u;
    g_trainingLastPositionPlanValid = false;
    g_trainingHold.reset();
    trainingClearLastDamageEvent();
    trainingStopTransientSfx();
    trainingRestoreOriginalConfigInMemory();
    trainingResetFrontendInputGate();
    qgame::rearmTrainingCharacterSelect();
    QLOG("[training] return-to-character-select armed; "
         "native match return/scene cleanup owns transition");
    return CommandAckResult::Applied;
}

static CommandAckResult trainingRefillEnvironmentReady(
        TrainingRefillTarget target) {
    if (!qtraining::isValidRefillTarget(target))
        return CommandAckResult::RejectedInvalidSetting;
    if (!g_trainingLaunchRequested
        || !g_trainingHooksSupported
        || !g_trainingHooksInstalled
        || g_trainingMutationFailed
        || g_netActive
        || g_mode != RunMode::Observe) {
        return CommandAckResult::RejectedUnavailable;
    }
    if (rd32(kSceneStateAddr) != 2u || rd32(kMatchSubAddr) != 0u)
        return CommandAckResult::RejectedUnsafeScene;
    return CommandAckResult::Applied;
}

struct TrainingLifeComponents {
    int32_t current = 0;
    int32_t pendingDamage = 0;
    int32_t reserve = 0;
    int32_t settleTimer = 0;
};

static bool trainingLifeRecordAddress(uint32_t player,
                                      uintptr_t& outLife) {
    // Never write slots 2/3: round adjudication dereferences off_4A37BC, null in a 1v1, without a check in its slot-2 death branch.
    if (player >= qtraining::kTrainingRefillPlayerCount) return false;
    const uintptr_t life = netBattleLifeAddress(player);
    if (!rangeReadable(life, 16u)) return false;
    outLife = life;
    return true;
}

static TrainingLifeComponents trainingReadLifeComponents(
        uintptr_t life) {
    TrainingLifeComponents components{};
    components.current = static_cast<int32_t>(
        rd32(life + kBattleLifeCurrentOffset));
    components.pendingDamage = static_cast<int32_t>(
        rd32(life + kBattleLifeDamageOffset));
    components.reserve = static_cast<int32_t>(
        rd32(life + kBattleLifeReserveOffset));
    components.settleTimer = static_cast<int32_t>(
        rd32(life + kBattleLifeTimerOffset));
    return components;
}

// Write current first: every intermediate effective life stays at or above the starting value, so a fault cannot leave a lethal state.
static void trainingWriteLifeComponents(
        uintptr_t life, const TrainingLifeComponents& components) {
    wr32(life + kBattleLifeCurrentOffset,
         static_cast<uint32_t>(components.current));
    wr32(life + kBattleLifeDamageOffset,
         static_cast<uint32_t>(components.pendingDamage));
    wr32(life + kBattleLifeReserveOffset,
         static_cast<uint32_t>(components.reserve));
    wr32(life + kBattleLifeTimerOffset,
         static_cast<uint32_t>(components.settleTimer));
}

static bool trainingLifeComponentsMatch(
        uintptr_t life, const TrainingLifeComponents& expected) {
    const TrainingLifeComponents live =
        trainingReadLifeComponents(life);
    return live.current == expected.current
        && live.pendingDamage == expected.pendingDamage
        && live.reserve == expected.reserve
        && live.settleTimer == expected.settleTimer;
}

// maxLife = 9200 * (activeConfig+44 + 1), where dword_613DE0 is the active config record, not the 0x623520 match copy.
static bool trainingNativeMaxLife(int64_t& outMaxLife) {
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    if (!RollbackManager::validQohActiveConfigPointer(active)
        || !rangeReadable(active, 92u)) {
        return false;
    }
    const int64_t lifeLevel =
        static_cast<int32_t>(rd32(active + 44u));
    const qtraining::LifeMaxPlan plan =
        qtraining::planLifeMax(lifeLevel);
    if (!plan.valid) return false;
    outMaxLife = plan.maxLife;
    return true;
}

static CommandAckResult trainingApplyLifeRefill(
        TrainingRefillTarget target, bool* outChanged = nullptr) {
    if (outChanged) *outChanged = false;
    const CommandAckResult ready =
        trainingRefillEnvironmentReady(target);
    if (ready != CommandAckResult::Applied) return ready;

    int64_t maxLife = 0;
    if (!trainingNativeMaxLife(maxLife)) {
        QLOG("[training-refill] life rejected: active config life level "
             "unreadable or outside the modelled range");
        return CommandAckResult::NativeValidationFailed;
    }
    const uint32_t activeConfig =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    if (rangeReadable(kTrainingCopiedConfigAddr, 100u)
        && rd32(kTrainingCopiedConfigAddr + 44u)
            != rd32(activeConfig + 44u)) {
        QLOG("[training-refill] config+44 diverged active=%u copy=%u",
             rd32(activeConfig + 44u),
             rd32(kTrainingCopiedConfigAddr + 44u));
    }

    uintptr_t life[qtraining::kTrainingRefillPlayerCount] = {};
    TrainingLifeComponents before[
        qtraining::kTrainingRefillPlayerCount] = {};
    TrainingLifeComponents full[
        qtraining::kTrainingRefillPlayerCount] = {};
    bool needsWrite[qtraining::kTrainingRefillPlayerCount] = {};
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!qtraining::refillTargetIncludesPlayer(target, player))
            continue;
        if (!trainingLifeRecordAddress(player, life[player]))
            return CommandAckResult::NativeValidationFailed;
        before[player] = trainingReadLifeComponents(life[player]);
        qtraining::LifeRecordImage image{};
        image.current = before[player].current;
        image.pendingDamage = before[player].pendingDamage;
        image.reserve = before[player].reserve;
        image.settleTimer = before[player].settleTimer;
        const qtraining::LifeRefillPlan plan =
            qtraining::planLifeRefill(image, maxLife);
        if (plan.verdict != RefillVerdict::Apply) {
            QLOG("[training-refill] life rejected p%u max=%d "
                 "record=%d/%d/%d/%d",
                 player + 1u, static_cast<int>(maxLife),
                 before[player].current, before[player].pendingDamage,
                 before[player].reserve, before[player].settleTimer);
            return CommandAckResult::NativeValidationFailed;
        }
        full[player].current = plan.current;
        full[player].pendingDamage = plan.pendingDamage;
        full[player].reserve = plan.reserve;
        full[player].settleTimer = plan.settleTimer;
        needsWrite[player] =
            before[player].current != full[player].current
            || before[player].pendingDamage != full[player].pendingDamage
            || before[player].reserve != full[player].reserve
            || before[player].settleTimer != full[player].settleTimer;
    }

    bool changed = false;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!qtraining::refillTargetIncludesPlayer(target, player))
            continue;
        if (!needsWrite[player]) continue;
        trainingWriteLifeComponents(life[player], full[player]);
        if (trainingLifeComponentsMatch(life[player], full[player])) {
            changed = true;
            continue;
        }
        for (uint32_t undo = 0; undo <= player; ++undo) {
            if (!qtraining::refillTargetIncludesPlayer(target, undo))
                continue;
            trainingWriteLifeComponents(life[undo], before[undo]);
            if (trainingLifeComponentsMatch(life[undo], before[undo]))
                continue;
            trainingFailMutation(
                TrainingDisableReason::InvariantFailure,
                "Life refill could not restore the life record");
            return CommandAckResult::NativeValidationFailed;
        }
        return CommandAckResult::NativeValidationFailed;
    }

    if (outChanged) *outChanged = changed;
    if (changed) {
        QLOG("[training-refill] life target=%u max=%d "
             "p1=%d/%d/%d p2=%d/%d/%d",
             static_cast<unsigned>(target),
             static_cast<int>(maxLife),
             before[0].current, before[0].pendingDamage, before[0].reserve,
             before[1].current, before[1].pendingDamage, before[1].reserve);
    }
    return CommandAckResult::Applied;
}

static bool trainingGuardGaugeAddress(uint32_t player,
                                      uint32_t& outSlot,
                                      uintptr_t& outGauge) {
    if (player >= qtraining::kTrainingRefillPlayerCount) return false;
    const uintptr_t fighter = trainingFighter(player, false);
    if (!fighter) return false;
    const uint32_t slot = rd8(fighter + kFighterPlayerSlotOffset);
    if (slot >= kTrainingCombatRecordSlotCount) return false;
    const uint64_t gauge =
        static_cast<uint64_t>(kGuardGaugeBase)
        + static_cast<uint64_t>(addr::kGuardGaugeStride) * slot;
    if (gauge < kTrainingCombatRecordBase
        || gauge + 4u > kTrainingCombatRecordBlockEnd) {
        return false;
    }
    const uintptr_t address = static_cast<uintptr_t>(gauge);
    if (!rangeReadable(address, 4u)) return false;
    outSlot = slot;
    outGauge = address;
    return true;
}

static CommandAckResult trainingApplyGuardGaugeRefill(
        TrainingRefillTarget target, bool* outChanged = nullptr) {
    if (outChanged) *outChanged = false;
    const CommandAckResult ready =
        trainingRefillEnvironmentReady(target);
    if (ready != CommandAckResult::Applied) return ready;

    uint32_t slot[qtraining::kTrainingRefillPlayerCount] = {};
    uintptr_t gauge[qtraining::kTrainingRefillPlayerCount] = {};
    bool targeted[qtraining::kTrainingRefillPlayerCount] = {};
    bool needsWrite[qtraining::kTrainingRefillPlayerCount] = {};
    int32_t writeValue[qtraining::kTrainingRefillPlayerCount] = {};
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!qtraining::refillTargetIncludesPlayer(target, player))
            continue;
        if (!trainingGuardGaugeAddress(player, slot[player],
                                       gauge[player])) {
            return CommandAckResult::NativeValidationFailed;
        }
        for (uint32_t earlier = 0; earlier < player; ++earlier) {
            if (targeted[earlier] && slot[earlier] == slot[player])
                return CommandAckResult::NativeValidationFailed;
        }
        const int64_t observed =
            static_cast<int32_t>(rd32(gauge[player]));
        const qtraining::GuardGaugeRefillPlan plan =
            qtraining::planGuardGaugeRefill(observed);
        if (plan.verdict != RefillVerdict::Apply) {
            QLOG("[training-refill] guard gauge rejected p%u slot=%u "
                 "observed=%lld",
                 player + 1u, slot[player],
                 static_cast<long long>(observed));
            return CommandAckResult::NativeValidationFailed;
        }
        targeted[player] = true;
        writeValue[player] = plan.writeValue;
        needsWrite[player] = observed != plan.writeValue;
    }

    bool changed = false;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!targeted[player] || !needsWrite[player]) continue;
        wr32(gauge[player], static_cast<uint32_t>(writeValue[player]));
        if (rd32(gauge[player])
                == static_cast<uint32_t>(writeValue[player])) {
            changed = true;
            continue;
        }
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Guard gauge refill write did not take");
        return CommandAckResult::NativeValidationFailed;
    }
    if (outChanged) *outChanged = changed;
    if (changed) {
        QLOG("[training-refill] guard gauge target=%u value=%d",
             static_cast<unsigned>(target),
             qtraining::kGuardGaugeFull);
    }
    return CommandAckResult::Applied;
}

static bool trainingExRecordAddress(uint32_t player,
                                    uint32_t& outOwner,
                                    uintptr_t& outRecord) {
    if (player >= qtraining::kTrainingRefillPlayerCount) return false;
    const uintptr_t fighter = trainingFighter(player, false);
    if (!fighter) return false;
    const uint32_t owner =
        rd8(fighter + kTrainingFighterExOwnerOffset);
    if (owner >= addr::kExGaugeStorageRecordCount) return false;
    const uint64_t record =
        static_cast<uint64_t>(addr::kExGaugeRecords)
        + static_cast<uint64_t>(addr::kExGaugeRecordStride) * owner;
    if (record < addr::kExGaugeRecords
        || record + addr::kExGaugeRecordStride
            > kTrainingExRecordBlockEnd) {
        return false;
    }
    const uintptr_t address = static_cast<uintptr_t>(record);
    if (!rangeReadable(address, addr::kExGaugeRecordStride))
        return false;
    outOwner = owner;
    outRecord = address;
    return true;
}

static CommandAckResult trainingApplyExMeterRefill(
        TrainingRefillTarget target, bool* outChanged = nullptr) {
    if (outChanged) *outChanged = false;
    const CommandAckResult ready =
        trainingRefillEnvironmentReady(target);
    if (ready != CommandAckResult::Applied) return ready;

    uint32_t owner[qtraining::kTrainingRefillPlayerCount] = {};
    uintptr_t record[qtraining::kTrainingRefillPlayerCount] = {};
    bool targeted[qtraining::kTrainingRefillPlayerCount] = {};
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!qtraining::refillTargetIncludesPlayer(target, player))
            continue;
        if (!trainingExRecordAddress(player, owner[player],
                                     record[player])) {
            return CommandAckResult::NativeValidationFailed;
        }
        bool duplicate = false;
        for (uint32_t earlier = 0; earlier < player; ++earlier) {
            if (targeted[earlier] && owner[earlier] == owner[player])
                duplicate = true;
        }
        targeted[player] = !duplicate;
    }

    int32_t stocks[qtraining::kTrainingRefillPlayerCount] = {};
    int32_t fraction[qtraining::kTrainingRefillPlayerCount] = {};
    bool needsWrite[qtraining::kTrainingRefillPlayerCount] = {};
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!targeted[player]) continue;
        const int64_t state = static_cast<int32_t>(
            rd32(record[player] + kTrainingExStateOffset));
        const int64_t observedStocks = static_cast<int32_t>(
            rd32(record[player] + kTrainingExStocksOffset));
        const int64_t observedFraction = static_cast<int32_t>(
            rd32(record[player] + kTrainingExFractionOffset));
        int64_t lanes[qtraining::kExPendingLaneCount] = {};
        for (uint32_t lane = 0;
             lane < qtraining::kExPendingLaneCount; ++lane) {
            lanes[lane] = static_cast<int32_t>(
                rd32(record[player] + kTrainingExPendingOffset
                     + 4u * lane));
        }
        const qtraining::ExRefillPlan plan =
            qtraining::planExRefill(state, observedStocks,
                                    observedFraction, lanes,
                                    g_trainingExRestoreTarget);
        if (plan.verdict == RefillVerdict::Busy) {
            QLOG("[training-refill] EX busy owner=%u state=%lld "
                 "lanes=%lld/%lld/%lld/%lld",
                 owner[player], static_cast<long long>(state),
                 static_cast<long long>(lanes[0]),
                 static_cast<long long>(lanes[1]),
                 static_cast<long long>(lanes[2]),
                 static_cast<long long>(lanes[3]));
            return CommandAckResult::RejectedUnavailable;
        }
        if (plan.verdict != RefillVerdict::Apply) {
            QLOG("[training-refill] EX rejected owner=%u stocks=%lld "
                 "fraction=%lld",
                 owner[player],
                 static_cast<long long>(observedStocks),
                 static_cast<long long>(observedFraction));
            return CommandAckResult::NativeValidationFailed;
        }
        stocks[player] = plan.stocks;
        fraction[player] = plan.fraction;
        needsWrite[player] = observedStocks != plan.stocks
            || observedFraction != plan.fraction;
    }

    bool changed = false;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!targeted[player] || !needsWrite[player]) continue;
        wr32(record[player] + kTrainingExStocksOffset,
             static_cast<uint32_t>(stocks[player]));
        wr32(record[player] + kTrainingExFractionOffset,
             static_cast<uint32_t>(fraction[player]));
        if (rd32(record[player] + kTrainingExStocksOffset)
                == static_cast<uint32_t>(stocks[player])
            && rd32(record[player] + kTrainingExFractionOffset)
                == static_cast<uint32_t>(fraction[player])) {
            changed = true;
            continue;
        }
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "EX meter refill write did not take");
        return CommandAckResult::NativeValidationFailed;
    }
    if (outChanged) *outChanged = changed;
    if (changed) {
        QLOG("[training-refill] EX target=%u stocks=%d fraction=%d",
             static_cast<unsigned>(target),
             qtraining::kExStocksFull, qtraining::kExFractionFull);
    }
    return CommandAckResult::Applied;
}

static bool trainingChargeExFraction(TrainingRefillTarget target) {
    const CommandAckResult ready =
        trainingRefillEnvironmentReady(target);
    if (ready != CommandAckResult::Applied) {
        return true;
    }

    uint32_t owner[qtraining::kTrainingRefillPlayerCount] = {};
    uintptr_t record[qtraining::kTrainingRefillPlayerCount] = {};
    bool targeted[qtraining::kTrainingRefillPlayerCount] = {};
    qtraining::ExFractionChargePlan
        plan[qtraining::kTrainingRefillPlayerCount] = {};

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!qtraining::refillTargetIncludesPlayer(target, player))
            continue;
        if (!trainingExRecordAddress(player, owner[player],
                                     record[player])) {
            trainingFailMutation(
                TrainingDisableReason::InvariantFailure,
                "EX fraction charge record was unavailable");
            return false;
        }
        bool duplicate = false;
        for (uint32_t earlier = 0; earlier < player; ++earlier) {
            if (targeted[earlier] && owner[earlier] == owner[player])
                duplicate = true;
        }
        targeted[player] = !duplicate;
    }

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!targeted[player]) continue;
        const int64_t state = static_cast<int32_t>(
            rd32(record[player] + kTrainingExStateOffset));
        const int64_t stocks = static_cast<int32_t>(
            rd32(record[player] + kTrainingExStocksOffset));
        const int64_t fraction = static_cast<int32_t>(
            rd32(record[player] + kTrainingExFractionOffset));
        int64_t lanes[qtraining::kExPendingLaneCount] = {};
        for (uint32_t lane = 0;
             lane < qtraining::kExPendingLaneCount; ++lane) {
            lanes[lane] = static_cast<int32_t>(
                rd32(record[player] + kTrainingExPendingOffset
                     + 4u * lane));
        }
        plan[player] = qtraining::planExFractionCharge(
            state, stocks, fraction, lanes);
        if (plan[player].verdict == RefillVerdict::Busy) {
            continue;
        }
        if (plan[player].verdict != RefillVerdict::Apply) {
            QLOG("[training-charge] EX corrupt owner=%u stocks=%lld "
                 "fraction=%lld",
                 owner[player], static_cast<long long>(stocks),
                 static_cast<long long>(fraction));
            trainingFailMutation(
                TrainingDisableReason::InvariantFailure,
                "EX fraction charge observed a corrupt meter record");
            return false;
        }
    }

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!targeted[player] || !plan[player].write) continue;
        wr32(record[player] + kTrainingExStocksOffset,
             static_cast<uint32_t>(plan[player].stocks));
        wr32(record[player] + kTrainingExFractionOffset,
             static_cast<uint32_t>(plan[player].fraction));
        if (rd32(record[player] + kTrainingExFractionOffset)
                == static_cast<uint32_t>(plan[player].fraction)
            && rd32(record[player] + kTrainingExStocksOffset)
                == static_cast<uint32_t>(plan[player].stocks)) {
            continue;
        }
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "EX fraction charge postcondition failed");
        return false;
    }

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!targeted[player] || !plan[player].write) continue;
        g_trainingExRestoreTarget.pinned = true;
        g_trainingExRestoreTarget.stocks = plan[player].stocks;
        g_trainingExRestoreTarget.fraction = plan[player].fraction;
        break;
    }
    return true;
}

static bool trainingConsumeRefillRequests() {
    const uint32_t requested = g_trainingRefillRequest;
    g_trainingRefillRequest = 0u;
    if (requested == 0u) return true;

    uint32_t applied = 0u;
    uint32_t rejected = 0u;
    if ((requested & kTrainingRefillLife) != 0u) {
        if (trainingApplyLifeRefill(TrainingRefillTarget::Both)
            == CommandAckResult::Applied) {
            applied |= kTrainingRefillLife;
        } else {
            rejected |= kTrainingRefillLife;
        }
    }
    if ((requested & kTrainingRefillGuardGauge) != 0u
        && !g_trainingMutationFailed) {
        if (trainingApplyGuardGaugeRefill(TrainingRefillTarget::Both)
            == CommandAckResult::Applied) {
            applied |= kTrainingRefillGuardGauge;
        } else {
            rejected |= kTrainingRefillGuardGauge;
        }
    }
    if ((requested & kTrainingRefillExMeter) != 0u
        && !g_trainingMutationFailed) {
        if (trainingApplyExMeterRefill(TrainingRefillTarget::Both)
            == CommandAckResult::Applied) {
            applied |= kTrainingRefillExMeter;
        } else {
            rejected |= kTrainingRefillExMeter;
        }
    }

    if (rejected != 0u) {
        ++g_trainingStickyErrors;
        trainingCopyText(
            g_trainingResultText,
            applied != 0u ? "Refill partly rejected"
                          : "Refill rejected");
    } else {
        trainingCopyText(g_trainingResultText, "Refilled");
    }
    return !g_trainingMutationFailed;
}

static_assert(kBattleLifeCurrentOffset
                      == qtraining::kLifeRecordCurrentOffset
                  && kBattleLifeDamageOffset
                      == qtraining::kLifeRecordPendingOffset
                  && kBattleLifeReserveOffset
                      == qtraining::kLifeRecordReserveOffset
                  && kBattleLifeTimerOffset
                      == qtraining::kLifeRecordSettleTimerOffset,
              "the life SET plan's offsets must be this build's life record");

static CommandAckResult trainingApplyLifeSet(
        TrainingRefillTarget target, int64_t requested) {
    const CommandAckResult ready =
        trainingRefillEnvironmentReady(target);
    if (ready != CommandAckResult::Applied) return ready;

    int64_t maxLife = 0;
    if (!trainingNativeMaxLife(maxLife)) {
        QLOG("[training-set] life rejected: active config life level "
             "unreadable or outside the modelled range");
        return CommandAckResult::NativeValidationFailed;
    }

    uintptr_t life[qtraining::kTrainingRefillPlayerCount] = {};
    TrainingLifeComponents before[
        qtraining::kTrainingRefillPlayerCount] = {};
    qtraining::LifeSetPlan plan[
        qtraining::kTrainingRefillPlayerCount] = {};
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!qtraining::refillTargetIncludesPlayer(target, player))
            continue;
        if (!trainingLifeRecordAddress(player, life[player]))
            return CommandAckResult::NativeValidationFailed;
        before[player] = trainingReadLifeComponents(life[player]);
        qtraining::LifeRecordImage image{};
        image.current = before[player].current;
        image.pendingDamage = before[player].pendingDamage;
        image.reserve = before[player].reserve;
        image.settleTimer = before[player].settleTimer;
        plan[player] = qtraining::planLifeSet(image, maxLife, requested);
        if (plan[player].verdict == RefillVerdict::Invalid) {
            QLOG("[training-set] life value %lld outside 1..%lld",
                 static_cast<long long>(requested),
                 static_cast<long long>(maxLife));
            return CommandAckResult::RejectedInvalidSetting;
        }
        if (plan[player].verdict != RefillVerdict::Apply) {
            QLOG("[training-set] life rejected p%u max=%d "
                 "record=%d/%d/%d/%d",
                 player + 1u, static_cast<int>(maxLife),
                 before[player].current, before[player].pendingDamage,
                 before[player].reserve, before[player].settleTimer);
            return CommandAckResult::NativeValidationFailed;
        }
    }

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!qtraining::refillTargetIncludesPlayer(target, player))
            continue;
        for (uint32_t step = 0; step < plan[player].stepCount; ++step) {
            wr32(life[player] + plan[player].steps[step].offset,
                 static_cast<uint32_t>(plan[player].steps[step].value));
        }
        TrainingLifeComponents expected{};
        expected.current = plan[player].current;
        expected.pendingDamage = plan[player].pendingDamage;
        expected.reserve = plan[player].reserve;
        expected.settleTimer = plan[player].settleTimer;
        if (trainingLifeComponentsMatch(life[player], expected))
            continue;
        for (uint32_t undo = 0; undo <= player; ++undo) {
            if (!qtraining::refillTargetIncludesPlayer(target, undo))
                continue;
            trainingWriteLifeComponents(life[undo], before[undo]);
            if (trainingLifeComponentsMatch(life[undo], before[undo]))
                continue;
            trainingFailMutation(
                TrainingDisableReason::InvariantFailure,
                "Life set could not restore the life record");
            return CommandAckResult::NativeValidationFailed;
        }
        return CommandAckResult::NativeValidationFailed;
    }

    QLOG("[training-set] life target=%u value=%lld max=%lld",
         static_cast<unsigned>(target),
         static_cast<long long>(requested),
         static_cast<long long>(maxLife));
    return CommandAckResult::Applied;
}

static CommandAckResult trainingApplyGuardGaugeSet(
        TrainingRefillTarget target, int64_t requested) {
    const CommandAckResult ready =
        trainingRefillEnvironmentReady(target);
    if (ready != CommandAckResult::Applied) return ready;

    uint32_t slot[qtraining::kTrainingRefillPlayerCount] = {};
    uintptr_t gauge[qtraining::kTrainingRefillPlayerCount] = {};
    bool targeted[qtraining::kTrainingRefillPlayerCount] = {};
    int32_t writeValue[qtraining::kTrainingRefillPlayerCount] = {};
    int32_t before[qtraining::kTrainingRefillPlayerCount] = {};
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!qtraining::refillTargetIncludesPlayer(target, player))
            continue;
        if (!trainingGuardGaugeAddress(player, slot[player],
                                       gauge[player])) {
            return CommandAckResult::NativeValidationFailed;
        }
        for (uint32_t earlier = 0; earlier < player; ++earlier) {
            if (targeted[earlier] && slot[earlier] == slot[player])
                return CommandAckResult::NativeValidationFailed;
        }
        before[player] = static_cast<int32_t>(rd32(gauge[player]));
        const qtraining::GuardGaugeSetPlan plan =
            qtraining::planGuardGaugeSet(before[player], requested);
        if (plan.verdict == RefillVerdict::Invalid)
            return CommandAckResult::RejectedInvalidSetting;
        if (plan.verdict != RefillVerdict::Apply) {
            QLOG("[training-set] guard gauge rejected p%u slot=%u "
                 "observed=%d",
                 player + 1u, slot[player], before[player]);
            return CommandAckResult::NativeValidationFailed;
        }
        targeted[player] = true;
        writeValue[player] = plan.writeValue;
    }

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!targeted[player]) continue;
        wr32(gauge[player], static_cast<uint32_t>(writeValue[player]));
        if (rd32(gauge[player])
            == static_cast<uint32_t>(writeValue[player])) {
            continue;
        }
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Guard gauge set write did not take");
        return CommandAckResult::NativeValidationFailed;
    }
    QLOG("[training-set] guard gauge target=%u value=%lld",
         static_cast<unsigned>(target),
         static_cast<long long>(requested));
    return CommandAckResult::Applied;
}

static CommandAckResult trainingApplyExSet(
        TrainingRefillTarget target, int64_t requestedStocks,
        int64_t requestedFraction) {
    const CommandAckResult ready =
        trainingRefillEnvironmentReady(target);
    if (ready != CommandAckResult::Applied) return ready;

    uint32_t owner[qtraining::kTrainingRefillPlayerCount] = {};
    uintptr_t record[qtraining::kTrainingRefillPlayerCount] = {};
    bool targeted[qtraining::kTrainingRefillPlayerCount] = {};
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!qtraining::refillTargetIncludesPlayer(target, player))
            continue;
        if (!trainingExRecordAddress(player, owner[player],
                                     record[player])) {
            return CommandAckResult::NativeValidationFailed;
        }
        bool duplicate = false;
        for (uint32_t earlier = 0; earlier < player; ++earlier) {
            if (targeted[earlier] && owner[earlier] == owner[player])
                duplicate = true;
        }
        targeted[player] = !duplicate;
    }

    int32_t stocks[qtraining::kTrainingRefillPlayerCount] = {};
    int32_t fraction[qtraining::kTrainingRefillPlayerCount] = {};
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!targeted[player]) continue;
        const int64_t state = static_cast<int32_t>(
            rd32(record[player] + kTrainingExStateOffset));
        const int64_t observedStocks = static_cast<int32_t>(
            rd32(record[player] + kTrainingExStocksOffset));
        const int64_t observedFraction = static_cast<int32_t>(
            rd32(record[player] + kTrainingExFractionOffset));
        int64_t lanes[qtraining::kExPendingLaneCount] = {};
        for (uint32_t lane = 0;
             lane < qtraining::kExPendingLaneCount; ++lane) {
            lanes[lane] = static_cast<int32_t>(
                rd32(record[player] + kTrainingExPendingOffset
                     + 4u * lane));
        }
        const qtraining::ExSetPlan plan = qtraining::planExSet(
            state, observedStocks, observedFraction, lanes,
            requestedStocks, requestedFraction);
        if (plan.verdict == RefillVerdict::Busy) {
            QLOG("[training-set] EX busy owner=%u state=%lld "
                 "lanes=%lld/%lld/%lld/%lld",
                 owner[player], static_cast<long long>(state),
                 static_cast<long long>(lanes[0]),
                 static_cast<long long>(lanes[1]),
                 static_cast<long long>(lanes[2]),
                 static_cast<long long>(lanes[3]));
            return CommandAckResult::RejectedUnavailable;
        }
        if (plan.verdict == RefillVerdict::Invalid) {
            QLOG("[training-set] EX value %lld/%lld rejected",
                 static_cast<long long>(requestedStocks),
                 static_cast<long long>(requestedFraction));
            return CommandAckResult::RejectedInvalidSetting;
        }
        if (plan.verdict != RefillVerdict::Apply) {
            QLOG("[training-set] EX rejected owner=%u stocks=%lld "
                 "fraction=%lld",
                 owner[player],
                 static_cast<long long>(observedStocks),
                 static_cast<long long>(observedFraction));
            return CommandAckResult::NativeValidationFailed;
        }
        stocks[player] = plan.stocks;
        fraction[player] = plan.fraction;
    }

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!targeted[player]) continue;
        wr32(record[player] + kTrainingExStocksOffset,
             static_cast<uint32_t>(stocks[player]));
        wr32(record[player] + kTrainingExFractionOffset,
             static_cast<uint32_t>(fraction[player]));
        if (rd32(record[player] + kTrainingExStocksOffset)
                == static_cast<uint32_t>(stocks[player])
            && rd32(record[player] + kTrainingExFractionOffset)
                == static_cast<uint32_t>(fraction[player])) {
            continue;
        }
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "EX meter set write did not take");
        return CommandAckResult::NativeValidationFailed;
    }
    QLOG("[training-set] EX target=%u stocks=%lld fraction=%lld",
         static_cast<unsigned>(target),
         static_cast<long long>(requestedStocks),
         static_cast<long long>(requestedFraction));
    return CommandAckResult::Applied;
}

struct TrainingResetResourceState {
    uintptr_t lifeAddress[qtraining::kTrainingRefillPlayerCount] = {};
    qtraining::LifeSetPlan life[qtraining::kTrainingRefillPlayerCount] = {};

    bool guardUsed[kTrainingCombatRecordSlotCount] = {};
    bool guardRestore[kTrainingCombatRecordSlotCount] = {};
    uintptr_t guardAddress[kTrainingCombatRecordSlotCount] = {};
    int32_t guardValue[kTrainingCombatRecordSlotCount] = {};

    bool exUsed[addr::kExGaugeStorageRecordCount] = {};
    bool exRestore[addr::kExGaugeStorageRecordCount] = {};
    uintptr_t exAddress[addr::kExGaugeStorageRecordCount] = {};
    int32_t exStocks[addr::kExGaugeStorageRecordCount] = {};
    int32_t exFraction[addr::kExGaugeStorageRecordCount] = {};
};

static bool trainingCaptureResetResourceState(
        TrainingResetResourceState& state) {
    qtraining::ResourceTriggerSignals signals{};
    signals.resetAccepted = true;
    const uint32_t healthRestore = qtraining::resolveHealthPolicyFire(
        g_trainingResourcePolicies.health, signals);
    const uint32_t guardRestore = qtraining::resolvePolicyFire(
        g_trainingResourcePolicies.guardGauge, signals);
    const uint32_t exRestore = qtraining::resolvePolicyFire(
        g_trainingResourcePolicies.ex, signals);

    int64_t maxLife = 0;
    if (!trainingNativeMaxLife(maxLife)) return false;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!trainingLifeRecordAddress(player, state.lifeAddress[player]))
            return false;
        const TrainingLifeComponents observed =
            trainingReadLifeComponents(state.lifeAddress[player]);
        qtraining::LifeRecordImage image{};
        image.current = observed.current;
        image.pendingDamage = observed.pendingDamage;
        image.reserve = observed.reserve;
        image.settleTimer = observed.settleTimer;
        state.life[player] = qtraining::planLifeReset(
            image, maxLife, (healthRestore & (1u << player)) != 0u);
        if (state.life[player].verdict != RefillVerdict::Apply)
            return false;

        uint32_t guardSlot = 0u;
        uintptr_t guardAddress = 0u;
        if (!trainingGuardGaugeAddress(
                player, guardSlot, guardAddress)) {
            return false;
        }
        if (!state.guardUsed[guardSlot]) {
            const int32_t observedGuard =
                static_cast<int32_t>(rd32(guardAddress));
            if (qtraining::planGuardGaugeRefill(observedGuard).verdict
                    != RefillVerdict::Apply) {
                return false;
            }
            state.guardUsed[guardSlot] = true;
            state.guardAddress[guardSlot] = guardAddress;
            state.guardValue[guardSlot] = observedGuard;
        }
        state.guardRestore[guardSlot] =
            state.guardRestore[guardSlot]
            || (guardRestore & (1u << player)) != 0u;

        uint32_t exOwner = 0u;
        uintptr_t exAddress = 0u;
        if (!trainingExRecordAddress(player, exOwner, exAddress))
            return false;
        if (!state.exUsed[exOwner]) {
            state.exUsed[exOwner] = true;
            state.exAddress[exOwner] = exAddress;
            state.exStocks[exOwner] = static_cast<int32_t>(
                rd32(exAddress + kTrainingExStocksOffset));
            state.exFraction[exOwner] = static_cast<int32_t>(
                rd32(exAddress + kTrainingExFractionOffset));
        }
        state.exRestore[exOwner] = state.exRestore[exOwner]
            || (exRestore & (1u << player)) != 0u;
    }

    for (uint32_t slot = 0;
         slot < kTrainingCombatRecordSlotCount; ++slot) {
        if (state.guardUsed[slot] && state.guardRestore[slot])
            state.guardValue[slot] = qtraining::kGuardGaugeFull;
    }
    for (uint32_t owner = 0;
         owner < addr::kExGaugeStorageRecordCount; ++owner) {
        if (!state.exUsed[owner]) continue;
        const qtraining::ExResetPlan plan = qtraining::planExReset(
            state.exStocks[owner], state.exFraction[owner],
            state.exRestore[owner], g_trainingExRestoreTarget);
        if (plan.verdict != RefillVerdict::Apply) return false;
        state.exStocks[owner] = plan.stocks;
        state.exFraction[owner] = plan.fraction;
    }
    return true;
}

static bool trainingApplyResetResourceState(
        const TrainingResetResourceState& state) {
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        const qtraining::LifeSetPlan& plan = state.life[player];
        for (uint32_t step = 0; step < plan.stepCount; ++step) {
            wr32(state.lifeAddress[player] + plan.steps[step].offset,
                 static_cast<uint32_t>(plan.steps[step].value));
        }
    }
    for (uint32_t slot = 0;
         slot < kTrainingCombatRecordSlotCount; ++slot) {
        if (!state.guardUsed[slot]) continue;
        wr32(state.guardAddress[slot],
             static_cast<uint32_t>(state.guardValue[slot]));
    }
    for (uint32_t owner = 0;
         owner < addr::kExGaugeStorageRecordCount; ++owner) {
        if (!state.exUsed[owner]) continue;
        const uintptr_t record = state.exAddress[owner];
        wr32(record + kTrainingExStateOffset, 0u);
        wr32(record + kTrainingExTimerOffset, 0u);
        for (uint32_t lane = 0;
             lane < qtraining::kExPendingLaneCount; ++lane) {
            wr32(record + kTrainingExPendingOffset + 4u * lane, 0u);
        }
        wr32(record + kTrainingExStocksOffset,
             static_cast<uint32_t>(state.exStocks[owner]));
        wr32(record + kTrainingExFractionOffset,
             static_cast<uint32_t>(state.exFraction[owner]));
    }

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        TrainingLifeComponents expected{};
        expected.current = state.life[player].current;
        expected.pendingDamage = state.life[player].pendingDamage;
        expected.reserve = state.life[player].reserve;
        expected.settleTimer = state.life[player].settleTimer;
        if (!trainingLifeComponentsMatch(
                state.lifeAddress[player], expected)) {
            return false;
        }
    }
    for (uint32_t slot = 0;
         slot < kTrainingCombatRecordSlotCount; ++slot) {
        if (state.guardUsed[slot]
            && static_cast<int32_t>(rd32(state.guardAddress[slot]))
                != state.guardValue[slot]) {
            return false;
        }
    }
    for (uint32_t owner = 0;
         owner < addr::kExGaugeStorageRecordCount; ++owner) {
        if (!state.exUsed[owner]) continue;
        const uintptr_t record = state.exAddress[owner];
        if (rd32(record + kTrainingExStateOffset) != 0u
            || rd32(record + kTrainingExTimerOffset) != 0u
            || static_cast<int32_t>(
                   rd32(record + kTrainingExStocksOffset))
                != state.exStocks[owner]
            || static_cast<int32_t>(
                   rd32(record + kTrainingExFractionOffset))
                != state.exFraction[owner]) {
            return false;
        }
        for (uint32_t lane = 0;
             lane < qtraining::kExPendingLaneCount; ++lane) {
            if (rd32(record + kTrainingExPendingOffset + 4u * lane) != 0u)
                return false;
        }
    }
    return true;
}

constexpr uintptr_t kTrainingForcedActionApplyAddr = 0x42A2A0u;
using TrainingForcedActionApplyFn = int (__fastcall*)(uintptr_t, int);
static bool g_trainingForcedActionApplyQualified = false;

static bool trainingQualifyForcedActionApply() {
    static const uint8_t kSignature[] = {
        0x53, 0x56, 0x8B, 0xF1, 0x8B, 0xDA, 0x8B,
        0x46, 0x5C, 0x83, 0xF8, 0xFF, 0x0F, 0x84};
    g_trainingForcedActionApplyQualified = se::verifyCodeBytes(
        kTrainingForcedActionApplyAddr, kSignature, sizeof(kSignature));
    if (!g_trainingForcedActionApplyQualified) {
        QLOG("[training] forced-action applier sub_42A2A0 prologue mismatch; "
             "engine action requests unavailable");
    }
    return g_trainingForcedActionApplyQualified;
}

static CommandAckResult trainingApplyRoundStartNeutral(
        TrainingRefillTarget target) {
    const CommandAckResult ready =
        trainingRefillEnvironmentReady(target);
    if (ready != CommandAckResult::Applied) return ready;

    if (!trainingResetPairToPreset(PositionPreset::RoundStart)
        || !trainingInvariantsHold()) {
        (void)trainingRestoreScratchOrFailClosed(
            "Round-start neutral failed and stable scratch restore failed");
        return CommandAckResult::NativeValidationFailed;
    }

    trainingResetTimelineMachines(true);
    g_trainingRoundStartNeutralPending = true;
    g_trainingRoundStartNeutralStepSerial = g_trainingStepSerial;
    g_trainingRoundStartNeutralTargetMask =
        qtraining::refillTargetPlayerMask(target);
    g_trainingRoundStartNeutralValid = 0u;
    g_trainingRoundStartNeutralMask = 0u;

    g_trainingInput.resumeAfterOperation(0u);
    (void)g_trainingMacros.requestStop();
    g_trainingOperationNeutral = true;
    QLOG("[training] round-start neutral target=%u restored=%02X",
         static_cast<unsigned>(target),
         qtraining::refillTargetPlayerMask(target));
    return CommandAckResult::Applied;
}

constexpr uintptr_t kTrainingComboTeardownAddr = 0x42A0A0u;
using TrainingComboTeardownFn = int (__fastcall*)(uintptr_t, int);

static void trainingClearNativeComboState(uintptr_t fighter) {
    reinterpret_cast<TrainingComboTeardownFn>(
        kTrainingComboTeardownAddr)(fighter, 1);
}

static bool trainingClearComboReadout() {
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        trainingClearNativeComboState(trainingFighter(player, false));
        const uintptr_t alternate = trainingAlternateFormActor(player);
        if (alternate) trainingClearNativeComboState(alternate);
    }
    for (uint32_t slot = 0; slot < kTrainingCombatRecordSlotCount; ++slot) {
        const uintptr_t record =
            kTrainingCombatRecordBase + kTrainingCombatRecordStride * slot;
        if (rd32(record + kTrainingComboHitsOffset) != 0u) return false;
    }
    return true;
}

constexpr uintptr_t kTrainingActorUnlinkAddr = 0x42DB00u;
using TrainingActorUnlinkFn = void (__fastcall*)(void*);
constexpr uintptr_t kTrainingCharacterAuxActorBase = 0x8737CCu;
constexpr uint32_t kTrainingCharacterAuxActorCount = 25u;
constexpr uint8_t kTrainingKaedeAuxActorCharacter = 34u;
constexpr uint32_t kTrainingKaedeAuxActorAction = 120u;
constexpr uint8_t kTrainingObj1AuxActorCharacter = 100u;
constexpr uint32_t kTrainingObj1AuxActorAction = 50u;

static uint32_t trainingDespawnCharacterAuxActors() {
    uint32_t despawned = 0u;
    for (uint32_t slot = 0;
         slot < kTrainingCharacterAuxActorCount; ++slot) {
        const uintptr_t actor = kTrainingCharacterAuxActorBase
            + static_cast<uintptr_t>(slot) * addr::kFighterSize;
        const uint8_t character =
            rd8(actor + kTrainingFighterCharacterOffset);
        const uint32_t action =
            rd32(actor + kTrainingFighterActionStateOffset);
        const bool fighterDerived =
            (character == kTrainingKaedeAuxActorCharacter
             && action == kTrainingKaedeAuxActorAction)
            || (character == kTrainingObj1AuxActorCharacter
                && action == kTrainingObj1AuxActorAction);
        if (!fighterDerived) continue;
        wr8(actor + kTrainingFighterCharacterOffset, 0xFFu);
        ++despawned;
    }
    return despawned;
}

static uint32_t trainingDespawnTransientActors() {
    const auto unlink =
        reinterpret_cast<TrainingActorUnlinkFn>(kTrainingActorUnlinkAddr);
    struct Pool { uintptr_t base; uint32_t count; };
    const Pool pools[] = {
        { addr::kEffectPool, 30u },
        { addr::kActorClonePool, 120u },
    };
    uint32_t despawned = 0u;
    for (const Pool& pool : pools) {
        for (uint32_t slot = 0; slot < pool.count; ++slot) {
            const uintptr_t actor =
                pool.base + static_cast<uintptr_t>(slot) * addr::kFighterSize;
            if (rd8(actor + kTrainingFighterCharacterOffset) == 0xFFu) continue;
            unlink(reinterpret_cast<void*>(actor));
            wr8(actor + kTrainingFighterCharacterOffset, 0xFFu);
            ++despawned;
        }
    }
    despawned += trainingDespawnCharacterAuxActors();
    return despawned;
}

static bool trainingResetPairToPreset(PositionPreset preset) {
    if (!trainingRoundCharacterStateRestorable()) return false;
    TrainingResetResourceState resources{};
    if (!trainingCaptureResetResourceState(resources)) return false;

    const uint32_t despawned = trainingDespawnTransientActors();
    trainingRestoreRoundCharacterActors();
    const bool comboCleared = trainingClearComboReadout();
    trainingRestoreRoundCharacterRegions();
    if (!comboCleared) return false;

    if (!trainingApplyPositionPreset(preset)
        || !trainingApplyResetResourceState(resources)) {
        return false;
    }

    if (despawned != 0u) {
        QLOG("[training] reset despawned %u spawned actor(s)", despawned);
    }
    return true;
}

constexpr uint32_t kTrainingStunWalkMaxFrames = 32u;
constexpr uint32_t kTrainingStunWalkMaxTicks = 240u;

static const char* g_trainingStunWalkRefusal = "none";
static uint32_t g_trainingPrimeClosest = 0xFFFFFFFFu;
static const char* g_trainingPrimeClosestReason = "none";

static bool trainingStunRemainingSteps(uintptr_t fighter,
                                       uint32_t& remaining) {
    remaining = 0u;
    g_trainingStunWalkRefusal = "none";
    if (!fighter || !rangeReadable(fighter, addr::kFighterSize)) {
        g_trainingStunWalkRefusal = "unreadable";
        return false;
    }
    const uint32_t action =
        rd32(fighter + kTrainingFighterActionStateOffset);
    if (!qtraining::combatActionIsGuard(action)
        && !qtraining::combatActionIsGroundHit(action)) {
        g_trainingStunWalkRefusal = "not-stun";
        return false;
    }
    if (rd32(fighter + kTrainingFighterFreezeOffset) != 0u) {
        g_trainingStunWalkRefusal = "hitstop";
        return false;
    }

    const uintptr_t actionTable = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterActionTableOffset));
    const uintptr_t frameTable = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterFrameTableOffset));
    if (!actionTable || !frameTable) {
        g_trainingStunWalkRefusal = "tables";
        return false;
    }
    const uintptr_t actionEntry = actionTable
        + static_cast<uintptr_t>(action) * kTrainingActionEntryStride;
    if (actionEntry < actionTable
        || !rangeReadable(actionEntry, kTrainingActionEntryStride)) {
        g_trainingStunWalkRefusal = "action-entry";
        return false;
    }
    const int32_t frameBase = static_cast<int32_t>(rd32(actionEntry));
    const int32_t frameCount = static_cast<int32_t>(rd32(actionEntry + 4u));
    if (frameBase < 0 || frameCount <= 0 || frameCount > 256) {
        g_trainingStunWalkRefusal = "frame-bounds";
        return false;
    }

    uint32_t total = rd32(fighter + kTrainingFighterActionLockEndOffset);
    if (total >= kTrainingGuardFrameDurationSentinel) {
        g_trainingStunWalkRefusal = "current-sentinel";
        return false;
    }
    uint32_t cursor = rd32(fighter + kTrainingFighterActionFrameOffset);
    uint8_t visited[256] = {};
    uint32_t steps = 0u;
    while (cursor != 0xFFFEu) {
        if (++steps > kTrainingStunWalkMaxFrames) {
            g_trainingStunWalkRefusal = "too-many-frames";
            return false;
        }
        if (cursor >= static_cast<uint32_t>(frameCount)) {
            g_trainingStunWalkRefusal = "cursor-bounds";
            return false;
        }
        if (visited[cursor] != 0u) {
            g_trainingStunWalkRefusal = "loop";
            return false;
        }
        visited[cursor] = 1u;
        const uint64_t absolute = static_cast<uint64_t>(frameBase) + cursor;
        if (absolute > (static_cast<uint64_t>(UINTPTR_MAX) - frameTable)
                           / kTrainingFighterFrameStride) {
            g_trainingStunWalkRefusal = "addressing";
            return false;
        }
        const uintptr_t frame = frameTable
            + static_cast<uintptr_t>(absolute) * kTrainingFighterFrameStride;
        if (frame < frameTable
            || !rangeReadable(frame, kTrainingFighterFrameStride)) {
            g_trainingStunWalkRefusal = "frame-unreadable";
            return false;
        }
        const uint32_t duration = rd32(frame + kTrainingFrameDurationOffset);
        if (duration >= kTrainingGuardFrameDurationSentinel) {
            g_trainingStunWalkRefusal = "sentinel";
            return false;
        }
        if (duration == 0u || total > kTrainingStunWalkMaxTicks - duration) {
            g_trainingStunWalkRefusal = "duration";
            return false;
        }
        total += duration;
        const uint16_t next =
            trainingReadFrameU16(frame + kTrainingFrameNextOffset);
        if (next == 0xFFFEu) break;
        if (next == 0xFFFFu) {
            if (cursor + 1u >= static_cast<uint32_t>(frameCount)) {
                g_trainingStunWalkRefusal = "past-last-frame";
                return false;
            }
            ++cursor;
        } else {
            cursor = next;
        }
    }
    remaining = total;
    static uint32_t lastAction = 0xFFFFFFFFu;
    static uint32_t lastClass = 0xFFFFFFFFu;
    static uint32_t lastRemaining = 0xFFFFFFFFu;
    const uint32_t guardClass =
        rd8(fighter + kTrainingFighterGuardClassOffset);
    if (g_trainingGuardTraceEnabled
        && (action != lastAction || guardClass != lastClass
            || remaining != lastRemaining)) {
        lastAction = action;
        lastClass = guardClass;
        lastRemaining = remaining;
        QLOG("[training-stun] action=%lu class=%u lock=%lu frame=%lu "
             "remaining=%lu step=%llu",
             static_cast<unsigned long>(action),
             static_cast<unsigned>(guardClass),
             static_cast<unsigned long>(
                 rd32(fighter + kTrainingFighterActionLockEndOffset)),
             static_cast<unsigned long>(
                 rd32(fighter + kTrainingFighterAnimFrameOffset)),
             static_cast<unsigned long>(remaining),
             static_cast<unsigned long long>(g_trainingStepSerial));
    }
    return true;
}

constexpr uint32_t kTrainingSeedMaxEntries = 8u;
constexpr uint32_t kTrainingSeedBarrierDwell = 64u;

static bool trainingSeedMotionIntoRing(
        uintptr_t fighter, const uint32_t* relativeMasks, uint32_t stepCount,
        uint32_t commitIndex) {
    if (!fighter || relativeMasks == nullptr || commitIndex == 0u
        || commitIndex > stepCount) {
        return false;
    }
    const uintptr_t child = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterCommandChildOffset));
    if (!child || !rangeReadable(child, kTrainingCommandChildSize))
        return false;
    if (rd32(child + kTrainingCommandDirectionKillOffset) != 0u) return false;

    uint32_t motion[kTrainingSeedMaxEntries] = {};
    uint32_t count = 0u;
    uint32_t previous = 0xFFFFFFFFu;
    for (uint32_t index = 0u; index < commitIndex; ++index) {
        const uint32_t direction =
            relativeMasks[index] & qinput::kLogicalDirectionMask;
        if (direction == 0u || direction == previous) continue;
        if (count >= kTrainingSeedMaxEntries) return false;
        motion[count++] = direction;
        previous = direction;
    }
    if (count == 0u) return false;

    const uint32_t head =
        rd8(child + kTrainingCommandDirHeadOffset)
        & kTrainingCommandDirRingMask;
    for (uint32_t index = 0u; index < count; ++index) {
        const uint32_t back = count - index;
        const uint32_t slot = (head - back) & kTrainingCommandDirRingMask;
        wr32(child + kTrainingCommandDirRingOffset + 4u * slot, motion[index]);
        wr32(child + kTrainingCommandDirDwellOffset + 4u * slot, 1u);
    }
    const uint32_t barrier =
        (head - (count + 1u)) & kTrainingCommandDirRingMask;
    wr32(child + kTrainingCommandDirRingOffset + 4u * barrier, 0u);
    wr32(child + kTrainingCommandDirDwellOffset + 4u * barrier,
         kTrainingSeedBarrierDwell);
    if (g_trainingGuardTraceEnabled) {
        char seeded[96];
        int used = 0;
        for (uint32_t index = 0u; index < count; ++index) {
            const int wrote = std::snprintf(
                seeded + used, sizeof(seeded) - static_cast<size_t>(used),
                "%s%05X", index != 0u ? "," : "",
                static_cast<unsigned>(motion[index]));
            if (wrote <= 0
                || static_cast<size_t>(used + wrote) >= sizeof(seeded)) {
                break;
            }
            used += wrote;
        }
        QLOG("[training-seed] %u entr%s [%s] behind head=%lu (head holds "
             "%05lX, untouched) step=%llu",
             static_cast<unsigned>(count), count == 1u ? "y" : "ies", seeded,
             static_cast<unsigned long>(head),
             static_cast<unsigned long>(
                 rd32(child + kTrainingCommandDirRingOffset + 4u * head)),
             static_cast<unsigned long long>(g_trainingStepSerial));
    }
    return true;
}

static void trainingRevokeSeededMotion(uintptr_t fighter) {
    if (!fighter || !rangeReadable(fighter, addr::kFighterSize)) return;
    const uintptr_t child = static_cast<uintptr_t>(
        rd32(fighter + kTrainingFighterCommandChildOffset));
    if (!child || !rangeReadable(child, kTrainingCommandChildSize)) return;
    const uint32_t head =
        rd8(child + kTrainingCommandDirHeadOffset)
        & kTrainingCommandDirRingMask;
    // Behind the head, never the head: that slot is the guard direction
    // sub_47C770 reads at contact, and taking it would drop the block.
    const uint32_t slot = (head - 1u) & kTrainingCommandDirRingMask;
    wr32(child + kTrainingCommandDirRingOffset + 4u * slot, 0u);
    wr32(child + kTrainingCommandDirDwellOffset + 4u * slot,
         kTrainingSeedBarrierDwell);
    if (g_trainingGuardTraceEnabled) {
        QLOG("[training-seed] revoked: barrier stamped behind head=%lu "
             "(head holds %05lX, untouched) step=%llu",
             static_cast<unsigned long>(head),
             static_cast<unsigned long>(
                 rd32(child + kTrainingCommandDirRingOffset + 4u * head)),
             static_cast<unsigned long long>(g_trainingStepSerial));
    }
}

static const char* g_trainingEngineRequestRefusal = "none";

static bool trainingRequestFighterAction(uintptr_t fighter, uint32_t action,
                                         uint8_t owner) {
    g_trainingEngineRequestRefusal = "none";
    if (!fighter || action == 0u
        || action == qtraining::kTrainingMoveUnknownAction) {
        g_trainingEngineRequestRefusal = "no-action-id";
        return false;
    }
    if (!rangeReadable(fighter, addr::kFighterSize)) {
        g_trainingEngineRequestRefusal = "unreadable";
        return false;
    }
    if (!trainingNativeResponseReady(fighter, owner)) {
        g_trainingEngineRequestRefusal = "not-ready";
        return false;
    }
    const uint32_t liveAction =
        rd32(fighter + kTrainingFighterActionStateOffset);
    if (qtraining::combatActionIsGuard(liveAction)
        || qtraining::combatActionIsGroundHit(liveAction)
        || qtraining::combatActionIsNativeReactionFamily(liveAction)
        || qtraining::combatActionCannotCommitCommand(liveAction)) {
        g_trainingEngineRequestRefusal = "still-in-stun";
        return false;
    }
    if (rd32(fighter + kTrainingFighterForcedActionOffset) != UINT32_MAX) {
        g_trainingEngineRequestRefusal = "forced-queued";
        return false;
    }
    if (rd32(fighter + kTrainingFighterFreezeOffset) != 0u) {
        g_trainingEngineRequestRefusal = "hitstop";
        return false;
    }

    if (!g_trainingForcedActionApplyQualified) {
        g_trainingEngineRequestRefusal = "applier-unqualified";
        return false;
    }
    wr32(fighter + kTrainingFighterForcedActionOffset, action);
    if (rd32(fighter + kTrainingFighterForcedActionOffset) != action) {
        g_trainingEngineRequestRefusal = "queue-rejected";
        return false;
    }
    reinterpret_cast<TrainingForcedActionApplyFn>(
        kTrainingForcedActionApplyAddr)(fighter, 0);
    if (rd32(fighter + kTrainingFighterForcedActionOffset) != UINT32_MAX) {
        g_trainingEngineRequestRefusal = "applier-did-not-run";
        return false;
    }
    return true;
}

static bool trainingSampleFighterNeutral(
        uint32_t player, qtraining::FighterNeutralObservation& out) {
    out = qtraining::FighterNeutralObservation{};
    if (player >= qtraining::kTrainingRefillPlayerCount) return false;
    const uintptr_t fighter = trainingFighter(player, false);
    if (!fighter) return false;
    const uint32_t slot = rd8(fighter + kFighterPlayerSlotOffset);
    out.slot = static_cast<uint8_t>(slot);
    out.alternateForm =
        rd32(fighter + kTrainingFighterAlternateFormOffset);
    out.action = rd32(fighter + kTrainingFighterActionStateOffset);
    out.dispatchAction =
        rd32(fighter + kTrainingFighterDispatchActionOffset);
    out.initStatus = static_cast<int32_t>(
        rd32(fighter + kTrainingFighterInitStatusOffset));
    out.transitionFlag =
        rd32(fighter + kTrainingFighterTransitionFlagOffset);
    out.actionLockEnd =
        rd32(fighter + kTrainingFighterActionLockEndOffset);
    out.postureNibble = static_cast<uint8_t>(
        rd32(fighter + kTrainingFighterMoveGateOffset) & 0xFFu);
    out.terminalReady =
        rd32(fighter + kTrainingFighterTerminalReadyOffset);
    out.roundEnd = rd32(fighter + kFighterRoundEndOffset);
    out.roundLost = rd32(fighter + kFighterRoundLostOffset);
    out.fatalCause = static_cast<int32_t>(
        rd32(fighter + kTrainingFighterFatalCauseOffset));
    out.comOwnership = rd32(fighter + kTrainingFighterComOffset);

    if (slot < qtraining::kTrainingRefillPlayerCount) {
        const uintptr_t record = kTrainingCombatRecordBase
            + static_cast<uintptr_t>(slot) * kTrainingCombatRecordStride;
        if (!rangeReadable(record, kTrainingCombatRecordStride))
            return false;
        out.recoveryLock = rd32(record + kTrainingRecoveryLockOffset);
        out.recoveryPhaseLatch =
            rd32(record + kTrainingRecoveryPhaseLatchOffset);
        out.comboHits = rd32(record + kTrainingComboHitsOffset);
    }

    uintptr_t life = 0;
    if (!trainingLifeRecordAddress(player, life)) return false;
    const TrainingLifeComponents components =
        trainingReadLifeComponents(life);
    out.lifePendingDamage = components.pendingDamage;
    out.lifeReserve = components.reserve;
    out.lifeSettleTimer = components.settleTimer;

    out.readable = true;
    return true;
}

static void trainingSampleNeutralGlobals(
        qtraining::NeutralGlobalObservation& out) {
    out = qtraining::NeutralGlobalObservation{};
    out.roundResultState = rd32(kRoundResultStateAddr);
    out.fatalHitDisableFlag = rd32(kNativeFatalHitDisableAddr);
    out.readable = true;
}

static void trainingEvaluateRoundStartNeutral() {
    if (!g_trainingRoundStartNeutralPending) return;
    if (g_trainingStepSerial <= g_trainingRoundStartNeutralStepSerial) return;
    g_trainingRoundStartNeutralPending = false;

    uint32_t combined = qtraining::RoundStartNeutralAllBits;
    bool evaluated = false;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if ((g_trainingRoundStartNeutralTargetMask & (1u << player)) == 0u)
            continue;
        const uintptr_t fighter = trainingFighter(player, false);
        qtraining::RoundStartNeutralObservation observation{};
        if (fighter && g_trainingLastPositionPlanValid) {
            observation.readable = true;
            observation.action =
                rd32(fighter + kTrainingFighterActionStateOffset);
            observation.dispatchAction =
                rd32(fighter + kTrainingFighterDispatchActionOffset);
            observation.initStatus = static_cast<int32_t>(
                rd32(fighter + kTrainingFighterInitStatusOffset));
            observation.transitionFlag =
                rd32(fighter + kTrainingFighterTransitionFlagOffset);
            observation.postureNibble = static_cast<uint8_t>(
                rd32(fighter + kTrainingFighterMoveGateOffset) & 0xFFu);
            observation.terminalReady =
                rd32(fighter + kTrainingFighterTerminalReadyOffset);
            observation.wallFlags =
                rd32(fighter + kTrainingFighterWallFlagsOffset) & 3u;
            observation.facing =
                rd8(fighter + kTrainingFighterFacingOffset);
            observation.expectedFacing =
                g_trainingLastPositionPlanFacing[player];
            observation.roundEnd =
                rd32(fighter + kFighterRoundEndOffset);
            observation.roundLost =
                rd32(fighter + kFighterRoundLostOffset);
            observation.fatalCause = static_cast<int32_t>(
                rd32(fighter + kTrainingFighterFatalCauseOffset));
            observation.comOwnership =
                rd32(fighter + kTrainingFighterComOffset);
            const uint32_t slot =
                rd8(fighter + kFighterPlayerSlotOffset);
            if (slot < qtraining::kTrainingRefillPlayerCount) {
                const uintptr_t record = kTrainingCombatRecordBase
                    + static_cast<uintptr_t>(slot)
                        * kTrainingCombatRecordStride;
                if (rangeReadable(record, kTrainingCombatRecordStride)) {
                    observation.recoveryLock =
                        rd32(record + kTrainingRecoveryLockOffset);
                    observation.recoveryPhaseLatch =
                        rd32(record + kTrainingRecoveryPhaseLatchOffset);
                }
            }
            observation.positionMatchesPlan =
                static_cast<int32_t>(
                    rd32(fighter + kTrainingFighterXOffset))
                        == g_trainingLastPositionPlanX[player]
                && static_cast<int32_t>(
                    rd32(fighter + kTrainingFighterYOffset)) == 0;
            const uintptr_t frameBase =
                rd32(fighter + kTrainingFighterFrameTableOffset);
            const uintptr_t frame =
                rd32(fighter + kTrainingFighterCurrentFrameOffset);
            observation.framePointerInRange =
                frameBase != 0 && frame >= frameBase
                && ((frame - frameBase) % kTrainingFighterFrameStride) == 0
                && rangeReadable(frame, kTrainingFighterFrameStride);
        }
        combined &= qtraining::evaluateRoundStartNeutral(observation);
        evaluated = true;
    }

    if (!evaluated) return;
    g_trainingRoundStartNeutralMask = combined;
    g_trainingRoundStartNeutralValid = 1u;

    const uint32_t escalating =
        qtraining::trainingRoundStartNeutralEscalatingBits();
    if ((combined & escalating) != escalating) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Round-start neutral left a round-result, COM or frame invariant");
        return;
    }
    if (combined != qtraining::RoundStartNeutralAllBits) {
        char text[96] = {};
        std::snprintf(text, sizeof(text),
                      "Round-start neutral: reported mask %04X",
                      combined);
        trainingCopyText(g_trainingResultText, text);
    }
}

static TrainingRefillTarget trainingTargetFromPlayerMask(uint32_t mask) {
    if ((mask & 3u) == 3u) return TrainingRefillTarget::Both;
    return (mask & 1u) != 0u
        ? TrainingRefillTarget::P1 : TrainingRefillTarget::P2;
}

static bool trainingConsumeResourcePolicies() {
    if (g_trainingStepSerial == g_trainingResourcePolicyStepSerial)
        return true;
    g_trainingResourcePolicyStepSerial = g_trainingStepSerial;

    trainingEvaluateRoundStartNeutral();
    if (g_trainingMutationFailed) return false;

    qtraining::FighterNeutralObservation fighters[
        qtraining::kTrainingRefillPlayerCount] = {};
    qtraining::NeutralGlobalObservation globals{};
    trainingSampleNeutralGlobals(globals);
    bool allReadable = true;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        if (!trainingSampleFighterNeutral(player, fighters[player]))
            allReadable = false;
    }

    uint32_t reasons[qtraining::kTrainingRefillPlayerCount] = {};
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        reasons[player] =
            qtraining::evaluateFighterNeutral(fighters[player]);
        g_trainingNeutralReasonMask[player] = reasons[player];
        g_trainingRecoveryLockReadout[player] =
            fighters[player].recoveryLock;
        g_trainingRecoveryPhaseReadout[player] =
            fighters[player].recoveryPhaseLatch;
        g_trainingComboHitsReadout[player] = fighters[player].comboHits;
        g_trainingDispatchActionReadout[player] =
            fighters[player].dispatchAction;
        g_trainingInitStatusReadout[player] = static_cast<uint32_t>(
            fighters[player].initStatus);
        g_trainingTerminalReadyReadout[player] =
            fighters[player].terminalReady;
    }
    const uint32_t globalReasons =
        qtraining::evaluateNeutralGlobals(globals);
    g_trainingNeutralGlobalReasonMask = globalReasons;
    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        const uintptr_t fighter = trainingFighter(player, false);
        g_trainingAuthoredFlagsReadout[player] = fighter
            ? (rd32(fighter + kTrainingFighterAuthoredFlagsOffset) & 0x0Fu)
            : 0u;
        g_trainingWallFlagsReadout[player] = fighter
            ? (rd32(fighter + kTrainingFighterWallFlagsOffset) & 3u)
            : 0u;
    }
    g_trainingResourcePolicyValid = allReadable ? 1u : 0u;

    const bool bothNeutral = allReadable
        && globalReasons == qtraining::TrainingNeutralOk
        && reasons[0] == qtraining::TrainingNeutralOk
        && reasons[1] == qtraining::TrainingNeutralOk;
    qtraining::ResourceTriggerSignals signals{};
    signals.neutralReady = g_trainingNeutralTrigger.advance(bothNeutral);
    const bool bothSettledNeutral = allReadable
        && qtraining::trainingExactIdleAction(fighters[0])
        && qtraining::trainingExactIdleAction(fighters[1]);
    signals.exactIdleReady =
        g_trainingExactIdleTrigger.advance(bothSettledNeutral);
    g_trainingBothFightersNeutral = bothNeutral ? 1u : 0u;
    g_trainingNeutralHoldSteps = g_trainingNeutralTrigger.holdSteps();

    for (uint32_t player = 0;
         player < qtraining::kTrainingRefillPlayerCount; ++player) {
        const bool inReaction = fighters[player].readable
            && qtraining::trainingReactionOrRecoveryAction(
                   fighters[player].action);
        const bool locksClear = fighters[player].readable
            && fighters[player].recoveryLock == 0u
            && fighters[player].recoveryPhaseLatch == 0u;
        qtraining::ComboEpisodeObservation episode{};
        episode.readable = fighters[player].readable;
        episode.inReactionAction = inReaction;
        episode.nativeComboHits = fighters[player].comboHits;
        episode.damageSettled =
            fighters[player].lifePendingDamage == 0
            && fighters[player].lifeReserve == 0
            && fighters[player].lifeSettleTimer == 0;
        episode.recoveryLocksClear = locksClear;
        episode.admittedDamageSerial = g_trainingDamage.admittedSerial;
        signals.comboEpisodeClosed[player] =
            g_trainingComboEpisode[player].advance(
                episode, qtraining::kTrainingComboSettleSteps);
        signals.recoveryExited[player] = fighters[player].readable
            && g_trainingRecoveryExit[player].advance(inReaction, locksClear);
    }

    const uint32_t healthMask = qtraining::resolveHealthPolicyFire(
        g_trainingResourcePolicies.health, signals);
    const uint32_t guardMask = qtraining::resolvePolicyFire(
        g_trainingResourcePolicies.guardGauge, signals);
    const uint32_t exMask = qtraining::resolvePolicyFire(
        g_trainingResourcePolicies.ex, signals);
    if ((healthMask | guardMask | exMask) == 0u) return true;

    if (!allReadable) {
        ++g_trainingResourcePolicyRejects;
        return true;
    }

    static uint32_t loggedGeneration[3] = {};
    auto run = [&](uint32_t index, uint32_t mask, const char* what,
                   CommandAckResult (*apply)(TrainingRefillTarget, bool*)) {
        if (mask == 0u || g_trainingMutationFailed) return;
        bool changed = false;
        const CommandAckResult result =
            apply(trainingTargetFromPlayerMask(mask), &changed);
        if (result == CommandAckResult::Applied) {
            if (changed) ++g_trainingResourcePolicyFires;
            return;
        }
        ++g_trainingResourcePolicyRejects;
        if (loggedGeneration[index] != g_trainingGeneration) {
            loggedGeneration[index] = g_trainingGeneration;
            QLOG("[training-policy] %s rejected mask=%02X result=%u",
                 what, mask, static_cast<unsigned>(result));
        }
    };
    run(0u, healthMask, "health", &trainingApplyLifeRefill);
    run(1u, guardMask, "guard gauge", &trainingApplyGuardGaugeRefill);
    run(2u, exMask, "EX meter", &trainingApplyExMeterRefill);

    return !g_trainingMutationFailed;
}

static CommandAckResult trainingApplyCommand(
    const TrainingCommandV1& command) {
    if (command.trainingGeneration != g_trainingGeneration)
        return CommandAckResult::RejectedUnavailable;
    const TrainingCommandKind kind =
        static_cast<TrainingCommandKind>(command.commandKind);
    if (!qtraining::validCommandPayload(command))
        return CommandAckResult::RejectedInvalidSetting;

    switch (kind) {
        case TrainingCommandKind::ResetPosition: {
            PositionPreset preset =
                static_cast<PositionPreset>(command.value);
            if (preset == PositionPreset::FromModifier) {
                const auto parsed =
                    qtraining::positionPresetFromResetModifier(
                        g_trainingPhysical.p1Mask
                        & qinput::kLogicalDirectionMask);
                if (!parsed.valid)
                    return CommandAckResult::RejectedInvalidSetting;
                preset = parsed.preset;
            }
            if (!trainingResetPairToPreset(preset)
                || !trainingInvariantsHold())
            {
                (void)trainingRestoreScratchOrFailClosed(
                    "Position reset failed and stable scratch restore failed");
                return CommandAckResult::NativeValidationFailed;
            }
            trainingResetTimelineMachines(true);
            g_trainingInput.resumeAfterOperation(0u);
            (void)g_trainingMacros.requestStop();
            g_trainingOperationNeutral = true;
            trainingArmMacroAfterReset();
            g_trainingRoundStartNeutralPending = true;
            g_trainingRoundStartNeutralStepSerial = g_trainingStepSerial;
            g_trainingRoundStartNeutralTargetMask = 3u;
            g_trainingRoundStartNeutralValid = 0u;
            g_trainingRoundStartNeutralMask = 0u;
            trainingRaiseStatusEvent(qtraining::StatusEvent::PositionReset,
                                     static_cast<uint32_t>(preset));
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SaveState: {
            const CommandAckResult result =
                trainingSaveState(command.slotOrSetting);
            if (result == CommandAckResult::Applied) {
                trainingRaiseStatusEvent(qtraining::StatusEvent::StateSaved,
                                         command.slotOrSetting + 1u);
            }
            return result;
        }
        case TrainingCommandKind::LoadState: {
            const CommandAckResult result =
                trainingLoadState(command.slotOrSetting);
            if (result == CommandAckResult::Applied) {
                trainingRaiseStatusEvent(qtraining::StatusEvent::StateLoaded,
                                         command.slotOrSetting + 1u);
            }
            return result;
        }
        case TrainingCommandKind::ClearState:
            if (command.slotOrSetting >= g_trainingStates.size())
                return CommandAckResult::RejectedInvalidSlot;
            g_trainingStates[command.slotOrSetting].valid = false;
            g_trainingStates[command.slotOrSetting].stale = false;
            trainingRaiseStatusEvent(qtraining::StatusEvent::StateCleared,
                                     command.slotOrSetting + 1u);
            return CommandAckResult::Applied;
        case TrainingCommandKind::ClearMacro:
            if (!g_trainingMacros.selectSlot(command.slotOrSetting)
                || !g_trainingMacros.clearSelectedSlot()) {
                return CommandAckResult::RejectedInvalidSlot;
            }
            trainingRaiseStatusEvent(qtraining::StatusEvent::MacroCleared,
                                     command.slotOrSetting + 1u);
            return CommandAckResult::Applied;
        case TrainingCommandKind::ToggleRecord: {
            const bool wasRecording =
                g_trainingMacros.state() == MacroState::Recording;
            if (!g_trainingMacros.requestRecordToggle())
                return CommandAckResult::RejectedUnavailable;
            if (wasRecording) {
                trainingRaiseStatusEvent(
                    qtraining::StatusEvent::MacroRecorded,
                    g_trainingMacros.selectedFrameCount());
            }
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::BeginPreRecord:
            return g_trainingMacros.requestPreRecord()
                ? CommandAckResult::Applied
                : CommandAckResult::RejectedUnavailable;
        case TrainingCommandKind::TogglePlayback:
            return g_trainingMacros.requestPlaybackToggle()
                ? CommandAckResult::Applied
                : CommandAckResult::RejectedUnavailable;
        case TrainingCommandKind::StopMacro: {
            const bool wasRecording =
                g_trainingMacros.state() == MacroState::Recording;
            if (!g_trainingMacros.requestStop())
                return CommandAckResult::RejectedUnavailable;
            trainingRaiseStatusEvent(
                wasRecording ? qtraining::StatusEvent::MacroRecorded
                             : qtraining::StatusEvent::MacroStopped,
                wasRecording ? g_trainingMacros.selectedFrameCount()
                             : g_trainingMacros.selectedSlot() + 1u);
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SelectMacroSlot:
            if (!g_trainingMacros.selectSlot(command.slotOrSetting))
                return CommandAckResult::RejectedInvalidSlot;
            trainingRaiseStatusEvent(
                qtraining::StatusEvent::MacroSlotSelected,
                command.slotOrSetting + 1u);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SelectStateSlot:
            if (command.slotOrSetting >= g_trainingStates.size())
                return CommandAckResult::RejectedInvalidSlot;
            g_trainingSelectedStateSlot = command.slotOrSetting;
            trainingRaiseStatusEvent(
                qtraining::StatusEvent::StateSlotSelected,
                command.slotOrSetting + 1u);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetGuardMode: {
            const GuardMode requested =
                static_cast<GuardMode>(command.value);
            if (!qtraining::guardModeReleaseQualified(
                    requested, false)) {
                return CommandAckResult::RejectedUnavailable;
            }
            g_trainingGuardMode = requested;
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SetGuardDuringPlayback:
            g_trainingGuardDuringPlayback = command.value != 0u;
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetPlaybackMode:
            return g_trainingMacros.setPlaybackMode(
                       static_cast<PlaybackMode>(command.value))
                ? CommandAckResult::Applied
                : CommandAckResult::RejectedInvalidSetting;
        case TrainingCommandKind::SetFacingPolicy:
            return g_trainingMacros.setFacingPolicy(
                       static_cast<FacingPolicy>(command.value))
                ? CommandAckResult::Applied
                : CommandAckResult::RejectedInvalidSetting;
        case TrainingCommandKind::SetHitboxDisplay:
            g_trainingHitboxMode =
                qtraining::hitboxDisplayModeFrom(command.value);
            if (g_trainingHitboxMode != qtraining::HitboxDisplayMode::Off)
                g_trainingHitboxRenderer = g_trainingHitboxMode;
            wr8(kTrainingHitboxAddr,
                qtraining::hitboxVanillaFlagValue(g_trainingHitboxMode));
            trainingPublishHitboxSnapshot();
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetHpRegen:
            return CommandAckResult::RejectedUnavailable;
        case TrainingCommandKind::SetStance: {
            const DummyStance requested =
                static_cast<DummyStance>(command.value);
            if (!qtraining::dummyStanceValid(requested))
                return CommandAckResult::RejectedInvalidSetting;
            g_trainingDummyStance = requested;
            QLOG("[training] dummy stance=%u", command.value);
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SetTech: {
            const DummyTech requested =
                static_cast<DummyTech>(command.value);
            if (!qtraining::dummyTechReleaseQualified(requested, false))
                return CommandAckResult::RejectedUnavailable;
            g_trainingDummyTech = requested;
            QLOG("[training] dummy auto-tech=%u", command.value);
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::RefillLife:
            return trainingApplyLifeRefill(
                static_cast<TrainingRefillTarget>(command.value));
        case TrainingCommandKind::RefillGuardGauge:
            return trainingApplyGuardGaugeRefill(
                static_cast<TrainingRefillTarget>(command.value));
        case TrainingCommandKind::RefillExMeter:
            return trainingApplyExMeterRefill(
                static_cast<TrainingRefillTarget>(command.value));
        case TrainingCommandKind::SetLifeValue:
            return trainingApplyLifeSet(
                static_cast<TrainingRefillTarget>(command.slotOrSetting),
                static_cast<int64_t>(command.value));
        case TrainingCommandKind::SetExMeter:
            return trainingApplyExSet(
                static_cast<TrainingRefillTarget>(command.slotOrSetting),
                static_cast<int64_t>(command.value),
                static_cast<int64_t>(command.flags));
        case TrainingCommandKind::SetGuardGaugeValue:
            return trainingApplyGuardGaugeSet(
                static_cast<TrainingRefillTarget>(command.slotOrSetting),
                static_cast<int64_t>(command.value));
        case TrainingCommandKind::SetHealthPolicy:
            g_trainingResourcePolicies.health.trigger =
                static_cast<ResourceTrigger>(command.value);
            g_trainingResourcePolicies.health.target =
                static_cast<TrainingRefillTarget>(command.slotOrSetting);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetExPolicy:
            g_trainingResourcePolicies.ex.trigger =
                static_cast<ResourceTrigger>(command.value);
            g_trainingResourcePolicies.ex.target =
                static_cast<TrainingRefillTarget>(command.slotOrSetting);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetGuardGaugePolicy:
            g_trainingResourcePolicies.guardGauge.trigger =
                static_cast<ResourceTrigger>(command.value);
            g_trainingResourcePolicies.guardGauge.target =
                static_cast<TrainingRefillTarget>(command.slotOrSetting);
            return CommandAckResult::Applied;
        case TrainingCommandKind::ResetRoundStartNeutral: {
            const CommandAckResult result = trainingApplyRoundStartNeutral(
                static_cast<TrainingRefillTarget>(command.slotOrSetting));
            if (result == CommandAckResult::Applied) {
                trainingRaiseStatusEvent(qtraining::StatusEvent::RoundReset, 0u);
            }
            return result;
        }
        case TrainingCommandKind::SetRecoveryPolicy: {
            const ManualRecoveryMode mode =
                static_cast<ManualRecoveryMode>(command.value);
            if (!qtraining::nativeRecoveryReleaseQualified(
                    mode, g_trainingRecoverySettings.evaluatorQualified)) {
                return CommandAckResult::RejectedUnavailable;
            }
            switch (static_cast<RecoveryPolicySlot>(command.slotOrSetting)) {
                case RecoveryPolicySlot::Air:
                    g_trainingRecoverySettings.air = mode;
                    break;
                case RecoveryPolicySlot::Ground:
                    g_trainingRecoverySettings.ground = mode;
                    break;
                case RecoveryPolicySlot::Wall:
                    g_trainingRecoverySettings.wall = mode;
                    break;
                case RecoveryPolicySlot::Terminal:
                    g_trainingRecoverySettings.terminal = mode;
                    break;
                case RecoveryPolicySlot::ThrowEscape:
                    if (mode != ManualRecoveryMode::Off)
                        return CommandAckResult::RejectedUnavailable;
                    g_trainingRecoverySettings.throwEscape =
                        ManualRecoveryMode::Off;
                    break;
                default:
                    return CommandAckResult::RejectedInvalidSetting;
            }
            QLOG("[training] recovery policy slot=%u mode=%u",
                 command.slotOrSetting, command.value);
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SetRecoveryTiming:
            g_trainingRecoverySettings.delaySteps = command.value;
            g_trainingRecoverySettings.randomMinSteps =
                command.flags & 0xFFFFu;
            g_trainingRecoverySettings.randomMaxSteps =
                (command.flags >> 16u) & 0xFFFFu;
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetTerminalRecoveryChoice:
            g_trainingRecoverySettings.terminalChoice =
                static_cast<TerminalRecoveryChoice>(command.value);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetThrowTechChance:
            if (!g_trainingThrowTechHookReady)
                return CommandAckResult::RejectedUnavailable;
            g_trainingThrowTech.chancePercent = command.value;
            QLOG("[training] throw-tech chance=%u", command.value);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetTriggerConfig: {
            if (!g_trainingContactHooksReady)
                return CommandAckResult::RejectedUnavailable;
            qtraining::TrainingTriggerConfig config{};
            const TrainingTriggerEvent event =
                static_cast<TrainingTriggerEvent>(command.slotOrSetting);
            if (qtraining::isEncodedTrainingTriggerMoveValue(command.value)) {
                config.moveIndex =
                    qtraining::decodeTrainingTriggerMoveIndex(command.value);
                const uintptr_t dummy =
                    trainingFighter(trainingDummySlot(), false);
                if (!dummy) return CommandAckResult::RejectedUnsafeScene;
                const qtraining::TrainingMoveView move = qtraining::trainingMove(
                    rd8(dummy + kTrainingFighterCharacterOffset),
                    config.moveIndex);
                if (!qtraining::trainingMoveTriggerEligible(move, event))
                    return CommandAckResult::RejectedInvalidSetting;
            } else {
                config.macroSlot = static_cast<uint8_t>(command.value);
            }
            config.delaySteps =
                static_cast<uint16_t>(command.flags & 0xFFFFu);
            config.chancePercent =
                static_cast<uint8_t>((command.flags >> 16u) & 0xFFFFu);
            if (!g_trainingTriggers.setConfig(
                    event, config)) {
                return CommandAckResult::RejectedInvalidSetting;
            }
            QLOG("[training-trigger] config event=%u recording=%u move=%u "
                 "delay=%u chance=%u",
                 command.slotOrSetting,
                 static_cast<unsigned>(config.macroSlot),
                 static_cast<unsigned>(config.moveIndex),
                 static_cast<unsigned>(config.delaySteps),
                 static_cast<unsigned>(config.chancePercent));
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SetFrameAdvantageEnabled:
            if (!g_trainingContactHooksReady)
                return CommandAckResult::RejectedUnavailable;
            g_trainingFrameAdvantage.setEnabled(command.value != 0u);
            QLOG("[training] frame advantage=%u", command.value);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetDebugOverlayEnabled:
            g_trainingDebugOverlayEnabled = command.value != 0u;
            QLOG("[training] debug overlay=%u", command.value);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetExRestoreTarget: {
            if (command.slotOrSetting == 0u) {
                g_trainingExRestoreTarget = qtraining::ExRestoreTarget{};
                QLOG("[training-refill] EX restore level unpinned (full)");
                return CommandAckResult::Applied;
            }
            qtraining::ExRestoreTarget next{};
            next.pinned = true;
            next.stocks =
                static_cast<int32_t>((command.value >> 16) & 0xFFFFu);
            next.fraction = static_cast<int32_t>(command.value & 0xFFFFu);
            if (!qtraining::exRestoreTargetValid(next)) {
                return CommandAckResult::RejectedInvalidSetting;
            }
            g_trainingExRestoreTarget = next;
            QLOG("[training-refill] EX restore level pinned to %d + %d",
                 next.stocks, next.fraction);
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SetMacroAfterReset:
            g_trainingMacroAfterReset =
                static_cast<qtraining::TrainingMacroAfterReset>(
                    command.value);
            QLOG("[training-macro] after reset/load: %u "
                 "(0 off, 1 play, 2 record)", command.value);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetGuardTiming: {
            if (!qtraining::guardTimingModeValid(command.value)
                || !qtraining::guardTimingWindowValid(command.flags)) {
                return CommandAckResult::RejectedInvalidSetting;
            }
            g_trainingGuardTimingConfig.mode =
                static_cast<qtraining::TrainingGuardTimingMode>(
                    command.value);
            g_trainingGuardTimingConfig.windowSteps = command.flags;
            g_trainingGuardTimingState =
                qtraining::TrainingGuardTimingState{};
            g_trainingGuardTimingArmed = true;
            QLOG("[training-guard] timing mode %u window %u steps "
                 "(0 always, 1 after being hit, 2 until first block)",
                 command.value, command.flags);
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SetPlayerSwap: {
            const bool requested = command.value != 0u;
            if (requested == g_trainingPlayerSwap)
                return CommandAckResult::Applied;
            if (g_trainingMacros.state() != MacroState::Idle)
                return CommandAckResult::RejectedUnsafeScene;
            g_trainingPlayerSwap = requested;
            g_trainingGuardTimingState = qtraining::TrainingGuardTimingState{};
            g_trainingGuardTimingArmed = true;
            g_trainingGuardTimingPendingHit = false;
            g_trainingGuardTimingPendingBlock = false;
            g_trainingPolicyFollowsHuman = false;
            g_trainingRecovery.reset(g_trainingGeneration);
            g_trainingRecoveryP1.reset(g_trainingGeneration);
            QLOG("[training-swap] dummy is now P%u; your device drives P%u "
                 "(guard timing and both recovery controllers restarted)",
                 static_cast<unsigned>(trainingDummySlot()) + 1u,
                 static_cast<unsigned>(trainingHumanSlot()) + 1u);
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SetContactPolicy: {
            const auto slot = static_cast<qtraining::TrainingContactPolicySlot>(
                command.slotOrSetting);
            const bool available =
                slot == qtraining::TrainingContactPolicySlot::FaultlessDummy
                    ? g_trainingFaultlessHookReady
                : slot == qtraining::TrainingContactPolicySlot::ParryDummy
                    ? g_trainingParryHookReady
                : (slot == qtraining::TrainingContactPolicySlot::ReduceP1Hit
                   || slot
                          == qtraining::TrainingContactPolicySlot::
                                 ReduceP2Hit)
                    ? g_trainingReduceHookReady
                    : g_trainingCriticalHookReady;
            if (!available) return CommandAckResult::RejectedUnavailable;
            if (!qtraining::applyContactPolicyAssignment(
                    g_trainingContactPolicyRequested, command.slotOrSetting,
                    command.value)) {
                return CommandAckResult::RejectedInvalidSetting;
            }
            QLOG("[training-contact] policy slot %u <- %u",
                 command.slotOrSetting, command.value);
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SetNativeHudMask: {
            uint32_t owner = 0u, globalMask = 0u, p1 = 0u, p2 = 0u;
            getModularNativeHudRequested(&owner, &globalMask, &p1, &p2);
            uint32_t next[3] = {globalMask, p1, p2};
            next[command.slotOrSetting] = command.value;
            if (!setModularNativeHudTrainingMasks(next[0], next[1], next[2]))
                return CommandAckResult::RejectedInvalidSetting;
            QLOG("[native-hud] training mask %u <- %08X",
                 command.slotOrSetting, command.value);
            return CommandAckResult::Applied;
        }
        case TrainingCommandKind::SetStateChangeLogEnabled:
            trainingStateLogSetEnabled(command.value != 0u);
            QLOG("[training] state-change log=%u open=%u failed=%u",
                 command.value,
                 trainingStateLogRecording() ? 1u : 0u,
                 trainingStateLogFailed() ? 1u : 0u);
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetFrameTraceEnabled:
            trainingFrameTraceSetEnabled(command.value != 0u);
            QLOG("[training] frame trace=%u open=%u failed=%u",
                 command.value,
                 trainingFrameTraceRecording() ? 1u : 0u,
                 trainingFrameTraceFailed() ? 1u : 0u);
            return CommandAckResult::Applied;
        case TrainingCommandKind::TogglePause:
            g_trainingHold.toggleUserHold();
            trainingCopyText(
                g_trainingResultText,
                g_trainingHold.heldFor(qtraining::TrainingHoldUser)
                    ? "Paused" : "Resumed");
            return CommandAckResult::Applied;
        case TrainingCommandKind::FrameStep:
            g_trainingHold.requestFrameStep();
            trainingCopyText(g_trainingResultText, "Frame step");
            return CommandAckResult::Applied;
        case TrainingCommandKind::SetMeterChargeConfig:
            g_trainingMeterChargeTarget =
                static_cast<TrainingRefillTarget>(command.slotOrSetting);
            g_trainingMeterChargeInterval = command.value;
            g_trainingMeterChargeCounter = 0u;
            QLOG("[training-charge] config target=%u interval=%u",
                 command.slotOrSetting, command.value);
            return CommandAckResult::Applied;
        case TrainingCommandKind::ReturnToCharacterSelect:
            return trainingBeginReturnToCharacterSelect();
        case TrainingCommandKind::None:
        case TrainingCommandKind::Count:
            return CommandAckResult::RejectedInvalidSetting;
    }
    return CommandAckResult::RejectedInvalidSetting;
}

static void trainingAcknowledgeCommand(
    const TrainingCommandV1& command,
    CommandAckResult result,
    const char* rejectionContext = nullptr) {
    g_trainingLastAckId = command.commandId;
    g_trainingLastAckResult = result;
    std::snprintf(
        g_trainingResultText, sizeof(g_trainingResultText),
        "Command %u: result %u",
        command.commandId, static_cast<unsigned>(result));
    if (result != CommandAckResult::Applied) {
        ++g_trainingStickyErrors;
        QLOG("[training-command] id=%u kind=%u rejected=%u%s%s",
             command.commandId, command.commandKind,
             static_cast<unsigned>(result),
             rejectionContext ? " context=" : "",
             rejectionContext ? rejectionContext : "");
    }
}

static bool trainingDrainCommands(
    bool& recoverySnapshotReady,
    bool& imageTransactionConsumed) {
    TrainingCommandV1 command{};
    for (uint32_t count = 0;
         count < kTrainingCommandsPerStep
              && g_trainingCommands.tryDequeue(command);
         ++count) {
        if (!recoverySnapshotReady) {
            if (!trainingCaptureSnapshot(g_trainingScratch)) {
                trainingAcknowledgeCommand(
                    command, CommandAckResult::SnapshotFailed,
                    "pre-command recovery snapshot failed");
                trainingFailMutation(
                    TrainingDisableReason::SnapshotFailure,
                    "Mandatory pre-command recovery snapshot failed");
                return false;
            }
            recoverySnapshotReady = true;
        }
        const CommandAckResult result =
            trainingApplyCommand(command);
        trainingAcknowledgeCommand(command, result);
        if (qtraining::commandOwnsNativeImageTransaction(
                static_cast<TrainingCommandKind>(
                    command.commandKind))) {
            imageTransactionConsumed = true;
            break;
        }
        if (g_trainingReturningToSelect
            || g_trainingMutationFailed) {
            break;
        }
    }
    return !g_trainingMutationFailed;
}

static bool trainingDrainFailedRecoveryCommands() {
    TrainingCommandV1 command{};
    bool returnApplied = false;
    for (uint32_t count = 0;
         count < kTrainingCommandsPerStep
             && g_trainingCommands.tryDequeue(command);
         ++count) {
        CommandAckResult result =
            CommandAckResult::RejectedUnavailable;
        const char* context = "failed-state mutation blocked";
        if (command.trainingGeneration != g_trainingGeneration) {
            context = "stale failed-state generation";
        } else if (!qtraining::validCommandPayload(command)) {
            result = CommandAckResult::RejectedInvalidSetting;
            context = "invalid failed-state payload";
        } else if (static_cast<TrainingCommandKind>(
                       command.commandKind)
                   == TrainingCommandKind::ReturnToCharacterSelect) {
            result = trainingBeginReturnToCharacterSelect();
            context = result == CommandAckResult::Applied
                ? nullptr : "failed-state return rejected";
        }
        trainingAcknowledgeCommand(command, result, context);
        if (result == CommandAckResult::Applied) {
            returnApplied = true;
            break;
        }
    }
    return returnApplied;
}

static bool trainingApplyActionEdges(
    const qtraining::TrainingActionResult& actions,
    bool imageTransactionConsumed) {
    const uint32_t edges = actions.actionEdges;
    if (edges == 0u) return true;
    if (InterlockedCompareExchange(&g_trainingMenuCaptured, 0, 0) != 0)
        return true;

    auto fired = [&](qtraining::TrainingAction action) {
        return (edges & qtraining::trainingActionBit(action)) != 0u;
    };
    auto note = [&](const char* text) {
        trainingCopyText(g_trainingResultText, text);
    };
    auto reject = [&](const char* text) {
        ++g_trainingStickyErrors;
        trainingCopyText(g_trainingResultText, text);
    };

    if (fired(qtraining::TrainingAction::ResetPosition)) {
        if (imageTransactionConsumed
            && !trainingCaptureSnapshot(g_trainingScratch)) {
            trainingFailMutation(
                TrainingDisableReason::SnapshotFailure,
                "Reset hotkey could not capture post-command recovery image");
            return false;
        }
        const auto parsed = qtraining::positionPresetFromResetModifier(
            actions.resetModifier);
        if (!parsed.valid) {
            reject("Reset rejected: use Left, Right, Up, Down, or no direction");
            return true;
        }
        if (!trainingResetPairToPreset(parsed.preset)
            || !trainingInvariantsHold()) {
            if (!trainingRestoreScratchOrFailClosed(
                    "Reset hotkey failed and stable scratch restore failed")) {
                return false;
            }
            reject("Reset rejected by native position validation");
            return true;
        }
        trainingResetTimelineMachines(true);
        g_trainingInput.resumeAfterOperation(actions.resetModifier);
        (void)g_trainingMacros.requestStop();
        g_trainingOperationNeutral = true;
        trainingArmMacroAfterReset();
        g_trainingRoundStartNeutralPending = true;
        g_trainingRoundStartNeutralStepSerial = g_trainingStepSerial;
        g_trainingRoundStartNeutralTargetMask = 3u;
        g_trainingRoundStartNeutralValid = 0u;
        g_trainingRoundStartNeutralMask = 0u;
        trainingRaiseStatusEvent(qtraining::StatusEvent::PositionReset,
                                 static_cast<uint32_t>(parsed.preset));
        note("Position reset: idle character state restored");
        return true;
    }

    if (fired(qtraining::TrainingAction::ReturnToCharacterSelect)) {
        const CommandAckResult result =
            trainingBeginReturnToCharacterSelect();
        if (result == CommandAckResult::Applied) {
            note("Returning to character select; recording discarded");
            return true;
        }
        reject("Return to character select rejected: not in a live match");
    }

    if (fired(qtraining::TrainingAction::NextSlot)
        != fired(qtraining::TrainingAction::PreviousSlot)) {
        static_assert(qtraining::kTrainingStateSlotCount
                          == qtraining::kTrainingMacroSlotCount,
                      "one slot number drives both selectors");
        const uint32_t count = qtraining::kTrainingStateSlotCount;
        const uint32_t slot =
            fired(qtraining::TrainingAction::NextSlot)
                ? (g_trainingSelectedStateSlot + 1u) % count
                : (g_trainingSelectedStateSlot + count - 1u) % count;
        g_trainingSelectedStateSlot = slot;
        (void)g_trainingMacros.selectSlot(slot);
        trainingRaiseStatusEvent(qtraining::StatusEvent::StateSlotSelected,
                                 slot + 1u);
        char text[96] = {};
        std::snprintf(text, sizeof(text), "Slot %u selected", slot + 1u);
        note(text);
    }

    if (fired(qtraining::TrainingAction::RecordStop)) {
        if (g_trainingMacros.requestRecordToggle())
            note(g_trainingMacros.state() == MacroState::PreRecord
                     ? "Pre-record: move the dummy, then Record again"
                     : "Recording toggled");
        else
            reject("Recording rejected: macro runtime busy");
    }
    if (fired(qtraining::TrainingAction::PlayStop)) {
        const bool wasPlacing =
            g_trainingMacros.state() == MacroState::PreRecord;
        if (g_trainingMacros.requestPlaybackToggle())
            note(wasPlacing ? "Pre-record cancelled"
                            : "Playback toggled");
        else
            reject("Playback rejected: macro runtime busy");
    }

    if (fired(qtraining::TrainingAction::TogglePause)) {
        g_trainingHold.toggleUserHold();
        note(g_trainingHold.heldFor(qtraining::TrainingHoldUser)
                 ? "Paused" : "Resumed");
    }
    if (fired(qtraining::TrainingAction::FrameStep)) {
        g_trainingHold.requestFrameStep();
        note("Frame step");
    }

    if (fired(qtraining::TrainingAction::RefillAll)) {
        const bool restoreAllOn = trainingRestoreAllPresetActive();
        const qtraining::ResourceTrigger trigger = restoreAllOn
            ? qtraining::ResourceTrigger::Off
            : qtraining::ResourceTrigger::WhenNeutral;
        if (!restoreAllOn)
            g_trainingExRestoreTarget = qtraining::ExRestoreTarget{};
        qtraining::ResourcePolicySettings* const policies[] = {
            &g_trainingResourcePolicies.health,
            &g_trainingResourcePolicies.ex,
            &g_trainingResourcePolicies.guardGauge,
        };
        for (qtraining::ResourcePolicySettings* policy : policies) {
            policy->trigger = trigger;
            policy->target = qtraining::TrainingRefillTarget::Both;
        }
        note(restoreAllOn ? "Restore all off"
                          : "Restore all ON (HP, full meter and guard gauge)");
    }
    if (fired(qtraining::TrainingAction::ToggleHitboxes)) {
        g_trainingHitboxMode = qtraining::hitboxToggled(
            g_trainingHitboxMode, g_trainingHitboxRenderer);
        wr8(kTrainingHitboxAddr,
            qtraining::hitboxVanillaFlagValue(g_trainingHitboxMode));
        trainingPublishHitboxSnapshot();
        note(g_trainingHitboxMode == qtraining::HitboxDisplayMode::Off
                 ? "Hitbox display off"
                 : (g_trainingHitboxMode
                        == qtraining::HitboxDisplayMode::Vanilla
                        ? "Hitbox display ON (game's own)"
                        : "Hitbox display ON"));
    }
    if (fired(qtraining::TrainingAction::CycleDummyStance)) {
        g_trainingDummyStance =
            qtraining::nextDummyStance(g_trainingDummyStance);
        char text[96] = {};
        std::snprintf(text, sizeof(text), "Dummy stance %u",
                      static_cast<unsigned>(g_trainingDummyStance));
        note(text);
    }
    if (fired(qtraining::TrainingAction::CycleGuardMode)) {
        g_trainingGuardMode =
            qtraining::nextReleasedGuardMode(g_trainingGuardMode);
        char text[96] = {};
        std::snprintf(text, sizeof(text), "Guard mode %u",
                      static_cast<unsigned>(g_trainingGuardMode));
        note(text);
    }

    if (fired(qtraining::TrainingAction::SaveState)) {
        const CommandAckResult result =
            trainingSaveState(g_trainingSelectedStateSlot);
        if (result == CommandAckResult::Applied) {
            trainingRaiseStatusEvent(qtraining::StatusEvent::StateSaved,
                                     g_trainingSelectedStateSlot + 1u);
            note("Save state written");
        }
        else
            reject("Save state rejected");
    }

    if (imageTransactionConsumed) return true;

    if (fired(qtraining::TrainingAction::LoadState)) {
        const CommandAckResult result =
            trainingLoadState(g_trainingSelectedStateSlot);
        if (result == CommandAckResult::Applied) {
            trainingRaiseStatusEvent(qtraining::StatusEvent::StateLoaded,
                                     g_trainingSelectedStateSlot + 1u);
            note("Save state loaded");
            return true;
        }
        if (g_trainingMutationFailed) return false;
        reject("Load state rejected: slot empty or from another match");
        return true;
    }

    if (fired(qtraining::TrainingAction::ResetPositionRoundStart)) {
        if (!trainingResetPairToPreset(PositionPreset::RoundStart)
            || !trainingInvariantsHold()) {
            if (!trainingRestoreScratchOrFailClosed(
                    "Round-start reset failed and stable scratch restore "
                    "failed")) {
                return false;
            }
            reject("Round-start reset rejected by native position validation");
            return true;
        }
        trainingResetTimelineMachines(true);
        g_trainingInput.resumeAfterOperation(0u);
        (void)g_trainingMacros.requestStop();
        g_trainingOperationNeutral = true;
        trainingArmMacroAfterReset();
        g_trainingRoundStartNeutralPending = true;
        g_trainingRoundStartNeutralStepSerial = g_trainingStepSerial;
        g_trainingRoundStartNeutralTargetMask = 3u;
        trainingRaiseStatusEvent(
            qtraining::StatusEvent::PositionReset,
            static_cast<uint32_t>(PositionPreset::RoundStart));
        note("Round-start position reset applied");
    }
    return true;
}

typedef BOOL(__stdcall* TrainingPresentHeldFrameFn)(void);
static TrainingPresentHeldFrameFn g_trainingHeldFramePresenter = nullptr;
static bool g_trainingHeldFramePresenterResolved = false;
static uint32_t g_trainingHeldFrameRepaints = 0u;
static uint32_t g_trainingHeldFrameRepaintMisses = 0u;

static void trainingRepaintHeldFrame() {
    if (!g_trainingHeldFramePresenterResolved) {
        g_trainingHeldFramePresenterResolved = true;
        const HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
        g_trainingHeldFramePresenter =
            wrapper != nullptr
                ? reinterpret_cast<TrainingPresentHeldFrameFn>(
                      GetProcAddress(wrapper,
                                     "EFZDDRAW_PresentHeldFrame"))
                : nullptr;
        QLOG("[training-hold] held-frame repaint entry point: "
             "wrapper=%p fn=%p",
             reinterpret_cast<void*>(wrapper),
             reinterpret_cast<void*>(g_trainingHeldFramePresenter));
    }
    if (g_trainingHeldFramePresenter == nullptr) return;
    ++g_trainingHeldFrameRepaints;
    if (!g_trainingHeldFramePresenter())
        ++g_trainingHeldFrameRepaintMisses;
    if (g_trainingHeldFrameRepaints == 1u
        || (g_trainingHeldFrameRepaints % 600u) == 0u) {
        QLOG("[training-hold] held-frame repaints=%u declined=%u",
             g_trainingHeldFrameRepaints,
             g_trainingHeldFrameRepaintMisses);
    }
}

struct TrainingReversalParseProbe {
    bool active = false;
    uint32_t ticks = 0u;
    uint32_t generation = 0u;
    uintptr_t fighter = 0u;
};
static TrainingReversalParseProbe g_trainingReversalParseProbe{};
constexpr uint32_t kTrainingReversalParseProbeTicks = 3u;

static void trainingArmReversalParseProbe(uintptr_t fighter) {
    g_trainingReversalParseProbe.active = true;
    g_trainingReversalParseProbe.ticks = 0u;
    g_trainingReversalParseProbe.generation = g_trainingGeneration;
    g_trainingReversalParseProbe.fighter = fighter;
}

static void trainingObserveReversalParse() {
    TrainingReversalParseProbe& probe = g_trainingReversalParseProbe;
    if (!probe.active) return;
    const uintptr_t fighter = probe.fighter;
    if (probe.generation != g_trainingGeneration || !fighter
        || trainingFighter(trainingDummySlot(), false) != fighter
        || !rangeReadable(fighter, addr::kFighterSize)) {
        probe = TrainingReversalParseProbe{};
        return;
    }
    ++probe.ticks;
    QLOG("[training-parse] +%u action=%u next=%d trans=%u disp=%u prev=%u "
         "perm=%02X lock=%u frame=%u anim=%u forced=%d step=%llu",
         static_cast<unsigned>(probe.ticks),
         static_cast<unsigned>(
             rd32(fighter + kTrainingFighterActionStateOffset)),
         static_cast<int>(rd32(fighter + kTrainingFighterNextActionOffset)),
         static_cast<unsigned>(
             rd32(fighter + kTrainingFighterTransitionFlagOffset)),
         static_cast<unsigned>(
             rd32(fighter + kTrainingFighterDispatchActionOffset)),
         static_cast<unsigned>(
             rd32(fighter + kTrainingFighterPrevActionOffset)),
         static_cast<unsigned>(trainingCombinedAuthoredPermission(fighter, 1u)),
         static_cast<unsigned>(
             rd32(fighter + kTrainingFighterActionLockEndOffset)),
         static_cast<unsigned>(
             rd32(fighter + kTrainingFighterActionFrameOffset)),
         static_cast<unsigned>(
             rd32(fighter + kTrainingFighterAnimFrameOffset)),
         static_cast<int>(rd32(fighter + kTrainingFighterForcedActionOffset)),
         static_cast<unsigned long long>(g_trainingStepSerial));
    if (probe.ticks >= kTrainingReversalParseProbeTicks)
        probe = TrainingReversalParseProbe{};
}

constexpr int32_t kTrainingChainLastTickIsActionable = 1;

constexpr uint32_t kTrainingReversalLeadSlots = 64u;
constexpr int32_t kTrainingReversalLeadMax = 2;

struct TrainingReversalLeadSlot {
    bool used = false;
    uint8_t character = 0xFFu;
    uint16_t moveIndex = 0u;
    int32_t correction = 0;
    uint32_t samples = 0u;
};
static TrainingReversalLeadSlot
    g_trainingReversalLead[kTrainingReversalLeadSlots] = {};

static TrainingReversalLeadSlot* trainingReversalLeadSlot(
        uint8_t character, uint16_t moveIndex, bool create) {
    TrainingReversalLeadSlot* free = nullptr;
    for (uint32_t i = 0u; i < kTrainingReversalLeadSlots; ++i) {
        TrainingReversalLeadSlot& slot = g_trainingReversalLead[i];
        if (slot.used && slot.character == character
            && slot.moveIndex == moveIndex) {
            return &slot;
        }
        if (!slot.used && free == nullptr) free = &slot;
    }
    if (!create || free == nullptr) return nullptr;
    free->used = true;
    free->character = character;
    free->moveIndex = moveIndex;
    free->correction = 0;
    free->samples = 0u;
    return free;
}

static int32_t trainingReversalLead(uint8_t character, uint16_t moveIndex) {
    const TrainingReversalLeadSlot* slot =
        trainingReversalLeadSlot(character, moveIndex, false);
    return slot != nullptr ? slot->correction : 0;
}

static void trainingReversalLeadObserve(uint8_t character, uint16_t moveIndex,
                                        int32_t delta) {
    if (delta == 0) {
        TrainingReversalLeadSlot* slot =
            trainingReversalLeadSlot(character, moveIndex, false);
        if (slot != nullptr) ++slot->samples;
        return;
    }
    TrainingReversalLeadSlot* slot =
        trainingReversalLeadSlot(character, moveIndex, true);
    if (slot == nullptr) return;
    int32_t corrected = slot->correction + delta;
    if (corrected > kTrainingReversalLeadMax)
        corrected = kTrainingReversalLeadMax;
    if (corrected < -kTrainingReversalLeadMax)
        corrected = -kTrainingReversalLeadMax;
    const int32_t previous = slot->correction;
    slot->correction = corrected;
    ++slot->samples;
    QLOG("[training-reversal] lead corrected character=%u move=%u "
         "delta=%+d lead=%+d (was %+d) samples=%u step=%llu",
         static_cast<unsigned>(character), static_cast<unsigned>(moveIndex),
         static_cast<int>(delta), static_cast<int>(corrected),
         static_cast<int>(previous), static_cast<unsigned>(slot->samples),
         static_cast<unsigned long long>(g_trainingStepSerial));
}

static void trainingReversalLeadDecay(uint8_t character, uint16_t moveIndex) {
    TrainingReversalLeadSlot* slot =
        trainingReversalLeadSlot(character, moveIndex, false);
    if (slot == nullptr || slot->correction == 0) return;
    const int32_t previous = slot->correction;
    slot->correction += slot->correction > 0 ? -1 : 1;
    QLOG("[training-reversal] lead decayed character=%u move=%u %+d -> %+d "
         "(no prime reached this episode) step=%llu",
         static_cast<unsigned>(character), static_cast<unsigned>(moveIndex),
         static_cast<int>(previous), static_cast<int>(slot->correction),
         static_cast<unsigned long long>(g_trainingStepSerial));
}

static void trainingReversalLeadReset() {
    for (uint32_t i = 0u; i < kTrainingReversalLeadSlots; ++i)
        g_trainingReversalLead[i] = TrainingReversalLeadSlot{};
}

static const char* trainingTriggerEventName(uint32_t event);

struct TrainingTriggerJournalFile {
    std::FILE* file = nullptr;
    bool failed = false;
    bool capped = false;
    uint32_t records = 0u;
    uint32_t dropped = 0u;
};
static TrainingTriggerJournalFile g_trainingTriggerJournal{};
constexpr uint32_t kTrainingTriggerJournalRecordCap = 200000u;

static void trainingTriggerJournalClose() {
    if (g_trainingTriggerJournal.file != nullptr) {
        std::fclose(g_trainingTriggerJournal.file);
        g_trainingTriggerJournal.file = nullptr;
    }
}

static void trainingTriggerJournalOpen() {
    if (g_trainingTriggerJournal.file != nullptr
        || g_trainingTriggerJournal.failed) {
        return;
    }
    if (g_sessionDir[0] == 0) {
        g_trainingTriggerJournal.failed = true;
        return;
    }
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\trigger_journal.log", g_sessionDir);
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (needed <= 0) {
        g_trainingTriggerJournal.failed = true;
        return;
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            &wide[0], needed) != needed) {
        g_trainingTriggerJournal.failed = true;
        return;
    }
    g_trainingTriggerJournal.file = _wfopen(wide.c_str(), L"a");
    if (g_trainingTriggerJournal.file == nullptr) {
        g_trainingTriggerJournal.failed = true;
        QLOG("[training-trigger] could not open %s", path);
        return;
    }
    std::fprintf(g_trainingTriggerJournal.file,
                 "# trigger journal; one line per controller decision.\n"
                 "# step | event | kind | cause | move | slot | detail\n");
    std::fflush(g_trainingTriggerJournal.file);
    QLOG("[training-trigger] journal recording to %s", path);
}

static void trainingDrainTriggerJournal() {
    qtraining::TrainingTriggerJournalEntry entries[
        qtraining::kTrainingTriggerJournalCapacity];
    if (g_trainingTriggerJournal.capped) {
        (void)g_trainingTriggers.drainJournal(nullptr, 0u);
        return;
    }
    trainingTriggerJournalOpen();
    if (g_trainingTriggerJournal.file == nullptr) {
        (void)g_trainingTriggers.drainJournal(nullptr, 0u);
        return;
    }
    const uint32_t taken = g_trainingTriggers.drainJournal(
        entries, qtraining::kTrainingTriggerJournalCapacity);
    if (taken == 0u) {
        const uint32_t dropped = g_trainingTriggers.journalDropped();
        if (dropped != g_trainingTriggerJournal.dropped) {
            g_trainingTriggerJournal.dropped = dropped;
            std::fprintf(g_trainingTriggerJournal.file,
                         "# %u decision(s) dropped: the ring filled inside a "
                         "single step\n", dropped);
        }
        return;
    }
    for (uint32_t i = 0u; i < taken; ++i) {
        const qtraining::TrainingTriggerJournalEntry& entry = entries[i];
        std::fprintf(
            g_trainingTriggerJournal.file,
            "step=%llu %-18s %-16s %-14s move=%u slot=%u detail=%u\n",
            static_cast<unsigned long long>(entry.stepSerial),
            trainingTriggerEventName(static_cast<uint32_t>(entry.event)),
            qtraining::triggerJournalKindName(entry.kind),
            qtraining::triggerJournalCauseName(entry.cause),
            static_cast<unsigned>(entry.moveIndex),
            static_cast<unsigned>(entry.macroSlot),
            static_cast<unsigned>(entry.detail));
        ++g_trainingTriggerJournal.records;
    }
    std::fflush(g_trainingTriggerJournal.file);
    if (g_trainingTriggerJournal.records >= kTrainingTriggerJournalRecordCap) {
        g_trainingTriggerJournal.capped = true;
        QLOG("[training-trigger] journal record cap %u reached; "
             "capture stopped", kTrainingTriggerJournalRecordCap);
        trainingTriggerJournalClose();
    }
}

struct TrainingReversalTraceFile {
    std::FILE* file = nullptr;
    bool failed = false;
    bool capped = false;
    bool windowOpen = false;
    uint32_t rows = 0u;
    uint32_t tailSteps = 0u;
    uint32_t attempt = 0u;
    uint64_t predictedStartStep = 0u;
    uint32_t predictedAction = 0u;
};
static TrainingReversalTraceFile g_trainingReversalTrace{};
constexpr uint32_t kTrainingReversalTraceRowCap = 20000u;
constexpr uint32_t kTrainingReversalTraceTailSteps = 6u;

static void trainingReversalTraceClose() {
    if (g_trainingReversalTrace.file != nullptr) {
        std::fclose(g_trainingReversalTrace.file);
        g_trainingReversalTrace.file = nullptr;
    }
}

static void trainingReversalTraceOpen() {
    if (g_trainingReversalTrace.file != nullptr
        || g_trainingReversalTrace.failed) {
        return;
    }
    if (g_sessionDir[0] == 0) {
        g_trainingReversalTrace.failed = true;
        return;
    }
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\reversal_trace.csv", g_sessionDir);
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (needed <= 0) {
        g_trainingReversalTrace.failed = true;
        return;
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            &wide[0], needed) != needed) {
        g_trainingReversalTrace.failed = true;
        return;
    }
    g_trainingReversalTrace.file = _wfopen(wide.c_str(), L"a");
    if (g_trainingReversalTrace.file == nullptr) {
        g_trainingReversalTrace.failed = true;
        QLOG("[training-reversal] could not open %s", path);
        return;
    }
    std::fprintf(g_trainingReversalTrace.file,
                 "attempt,step,phase,event,character,move,"
                 "action,actionFrame,lock,anim,next,trans,prev,disp,init,"
                 "forced,posture,perm,term,freeze,dur,link,"
                 "cursor,commit,physicalP2,recoveryLock,armable,worldY,floor,"
                 "dirHead,dir0,dwell0,dir1,dwell1,dir2,dwell2,dir3,dwell3,"
                 "btnHead,btnMask,btn0,btn1,btn2,btn3,dirKill,"
                 "predictedStart,predictedAction\n");
    std::fflush(g_trainingReversalTrace.file);
    QLOG("[training-reversal] recording to %s", path);
}

static void trainingReversalTraceOpenWindow(uint64_t predictedStartStep,
                                            uint32_t predictedAction) {
    if (g_trainingReversalTrace.capped) return;
    trainingReversalTraceOpen();
    if (g_trainingReversalTrace.file == nullptr) return;
    if (!g_trainingReversalTrace.windowOpen) ++g_trainingReversalTrace.attempt;
    g_trainingReversalTrace.windowOpen = true;
    g_trainingReversalTrace.tailSteps = 0u;
    g_trainingReversalTrace.predictedStartStep = predictedStartStep;
    g_trainingReversalTrace.predictedAction = predictedAction;
}

static void trainingReversalTraceCloseWindow() {
    if (!g_trainingReversalTrace.windowOpen) return;
    if (g_trainingReversalTrace.tailSteps == 0u)
        g_trainingReversalTrace.tailSteps = kTrainingReversalTraceTailSteps;
}

static void trainingReversalTraceRow(const char* phase) {
    TrainingReversalTraceFile& trace = g_trainingReversalTrace;
    if (!trace.windowOpen || trace.file == nullptr || trace.capped) return;
    const uintptr_t f = trainingFighter(trainingDummySlot(), false);
    if (!f || !rangeReadable(f, addr::kFighterSize)) return;
    if (trace.rows >= kTrainingReversalTraceRowCap) {
        trace.capped = true;
        trace.windowOpen = false;
        QLOG("[training-reversal] row cap %u reached; capture stopped",
             kTrainingReversalTraceRowCap);
        trainingReversalTraceClose();
        return;
    }

    const uintptr_t frame = static_cast<uintptr_t>(
        rd32(f + kTrainingFighterCurrentFrameOffset));
    uint32_t duration = 0u;
    uint32_t link = 0u;
    if (frame && rangeReadable(frame, kTrainingFighterFrameStride)) {
        duration = rd32(frame + kTrainingFrameDurationOffset);
        link = trainingReadFrameU16(frame + kTrainingFrameNextOffset);
    }

    const uintptr_t child = static_cast<uintptr_t>(
        rd32(f + kTrainingFighterCommandChildOffset));
    uint32_t dirHead = 0u;
    uint32_t btnHead = 0u;
    uint32_t btnMask = 0u;
    uint32_t dirKill = 0u;
    uint32_t dirEntry[4] = {};
    uint32_t dirDwell[4] = {};
    uint32_t btnCount[kTrainingCommandButtonCount] = {};
    if (child && rangeReadable(child, kTrainingCommandChildSize)) {
        dirHead = rd8(child + kTrainingCommandDirHeadOffset);
        btnHead = rd8(child + kTrainingCommandButtonHeadOffset);
        dirKill = rd32(child + kTrainingCommandDirectionKillOffset);
        btnMask = rd32(child + kTrainingCommandButtonRingOffset
                       + 4u * (btnHead & kTrainingCommandButtonRingMask));
        for (uint32_t back = 0u; back < 4u; ++back) {
            const uint32_t slot =
                (dirHead - back) & kTrainingCommandDirRingMask;
            dirEntry[back] =
                rd32(child + kTrainingCommandDirRingOffset + 4u * slot);
            dirDwell[back] =
                rd32(child + kTrainingCommandDirDwellOffset + 4u * slot);
        }
        for (uint32_t b = 0u; b < kTrainingCommandButtonCount; ++b)
            btnCount[b] = rd8(child + kTrainingCommandButtonCountOffset + b);
    }

    std::fprintf(
        trace.file,
        "%u,%llu,%s,%u,%u,%u,"
        "%u,%u,%u,%u,%d,%u,%u,%u,%d,"
        "%d,%u,%02X,%u,%u,%u,%u,"
        "%u,%u,%05X,%u,%02X,%d,%u,"
        "%u,%05X,%u,%05X,%u,%05X,%u,%05X,%u,"
        "%u,%05X,%u,%u,%u,%u,%u,"
        "%llu,%u\n",
        trace.attempt,
        static_cast<unsigned long long>(g_trainingStepSerial), phase,
        static_cast<unsigned>(g_trainingWakeupReversal.event),
        static_cast<unsigned>(g_trainingWakeupReversal.character),
        static_cast<unsigned>(g_trainingWakeupReversal.moveIndex),
        static_cast<unsigned>(rd32(f + kTrainingFighterActionStateOffset)),
        static_cast<unsigned>(rd32(f + kTrainingFighterActionFrameOffset)),
        static_cast<unsigned>(rd32(f + kTrainingFighterActionLockEndOffset)),
        static_cast<unsigned>(rd32(f + kTrainingFighterAnimFrameOffset)),
        static_cast<int>(rd32(f + kTrainingFighterNextActionOffset)),
        static_cast<unsigned>(rd32(f + kTrainingFighterTransitionFlagOffset)),
        static_cast<unsigned>(rd32(f + kTrainingFighterPrevActionOffset)),
        static_cast<unsigned>(rd32(f + kTrainingFighterDispatchActionOffset)),
        static_cast<int>(rd32(f + kTrainingFighterInitStatusOffset)),
        static_cast<int>(rd32(f + kTrainingFighterForcedActionOffset)),
        static_cast<unsigned>(rd8(f + kTrainingFighterMoveGateOffset) & 0x0Fu),
        static_cast<unsigned>(trainingCombinedAuthoredPermission(f, 1u)),
        static_cast<unsigned>(
            rd32(f + kTrainingFighterTerminalReadyOffset) & 0x01u),
        static_cast<unsigned>(rd32(f + kTrainingFighterFreezeOffset)),
        static_cast<unsigned>(duration), static_cast<unsigned>(link),
        static_cast<unsigned>(g_trainingMacros.cursor()),
        static_cast<unsigned>(g_trainingWakeupReversal.commitIndex),
        static_cast<unsigned>(g_trainingPhysical.p2Mask),
        static_cast<unsigned>(g_trainingRecoveryLockSample),
        static_cast<unsigned>(g_trainingRecoveryArmable),
        static_cast<int>(rd32(f + kTrainingFighterYOffset)),
        static_cast<unsigned>(rd8(f + kTrainingFighterFloorModeOffset)),
        static_cast<unsigned>(dirHead),
        static_cast<unsigned>(dirEntry[0]), static_cast<unsigned>(dirDwell[0]),
        static_cast<unsigned>(dirEntry[1]), static_cast<unsigned>(dirDwell[1]),
        static_cast<unsigned>(dirEntry[2]), static_cast<unsigned>(dirDwell[2]),
        static_cast<unsigned>(dirEntry[3]), static_cast<unsigned>(dirDwell[3]),
        static_cast<unsigned>(btnHead), static_cast<unsigned>(btnMask),
        static_cast<unsigned>(btnCount[0]), static_cast<unsigned>(btnCount[1]),
        static_cast<unsigned>(btnCount[2]), static_cast<unsigned>(btnCount[3]),
        static_cast<unsigned>(dirKill),
        static_cast<unsigned long long>(trace.predictedStartStep),
        static_cast<unsigned>(trace.predictedAction));
    ++trace.rows;
    if ((trace.rows & 0x1Fu) == 0u) std::fflush(trace.file);

    if (trace.tailSteps > 0u && --trace.tailSteps == 0u) {
        trace.windowOpen = false;
        std::fflush(trace.file);
    }
}

static bool trainingAdmitPausedFrame() {
    const qtraining::TrainingHoldAdmission admission =
        g_trainingHold.admit();
    if (admission.admit) return true;
    trainingCopyText(
        g_trainingStatusText,
        qtraining::trainingHoldStatusText(admission.reasons));
    if (admission.presentHeldFrame)
        trainingRepaintHeldFrame();
    return false;
}

static bool trainingComposeInputForStep() {
    const uintptr_t p2 = trainingFighter(trainingDummySlot(), false);
    if (!p2) return false;
    const uint8_t p2Facing =
        rd8(p2 + kTrainingFighterFacingOffset);
    if (p2Facing > 1u
        || !qinput::isCanonicalLogicalFightInput(
               g_trainingPhysical.p1Mask)
        || !qinput::isCanonicalLogicalFightInput(
               g_trainingPhysical.p2Mask)) {
        return false;
    }

    if (g_trainingWakeupReversal.active
        && !g_trainingWakeupReversal.released) {
        const qtraining::TrainingTriggerConfig config =
            g_trainingTriggers.config(g_trainingWakeupReversal.event);
        uint32_t remaining = 0u;
        bool releaseGateBased = false;
        const uint32_t cursor = g_trainingMacros.cursor();
        const bool configurationMatches = config.macroSlot == 0u
            && config.moveIndex == g_trainingWakeupReversal.moveIndex
            && config.delaySteps == 0u
            && config.chancePercent != 0u;
        const bool timingMatches =
            cursor <= g_trainingWakeupReversal.commitIndex
            && g_trainingWakeupReversal.character
                   == rd8(p2 + kTrainingFighterCharacterOffset)
            && g_trainingWakeupReversal.resourceAction
                   == rd32(p2 + kTrainingFighterActionStateOffset)
            && g_trainingWakeupReversal.dispatchAction
                   == rd32(p2 + kTrainingFighterDispatchActionOffset)
            && g_trainingCombatIdentity[1].valid
            && g_trainingCombatIdentity[1].fighter == p2
            && g_trainingCombatIdentity[1].character
                   == g_trainingWakeupReversal.character
            && g_trainingCombatIdentity[1].actorToken
                   == g_trainingWakeupReversal.actorToken
            && g_trainingCombatIdentity[1].actionEpoch
                   == g_trainingWakeupReversal.actionEpoch
            && !g_trainingRecoveryEmission.active
            && (qtraining::TrainingTriggerController::prebufferRestoresGuard(
                    g_trainingWakeupReversal.event)
                    ? trainingStunRemainingSteps(p2, remaining)
                    : trainingStepsUntilDummyCanAct(
                          p2, remaining, releaseGateBased))
            && static_cast<int64_t>(remaining) + static_cast<int64_t>(cursor)
                == static_cast<int64_t>(
                       g_trainingWakeupReversal.commitIndex) + 1
                       + kTrainingChainLastTickIsActionable
                       + g_trainingWakeupReversal.appliedLead;
        if (!configurationMatches
            || g_trainingWakeupReversal.generation
                   != g_trainingGeneration
            || g_trainingWakeupReversal.fighter != p2
            || !g_trainingMacros.bufferedReversalActive()
            || !timingMatches) {
            trainingCancelWakeupReversal(
                configurationMatches
                    ? qtraining::TriggerRequestOutcome::DroppedWindowClosed
                    : qtraining::TriggerRequestOutcome::
                          DroppedConfigurationChanged,
                configurationMatches
                    ? "native wakeup suffix changed"
                    : "trigger configuration changed");
        } else {
            g_trainingWakeupReversal.lockedStepsRemaining = remaining;
            if (cursor == g_trainingWakeupReversal.commitIndex
                && static_cast<int64_t>(remaining)
                       == 1 + kTrainingChainLastTickIsActionable
                              + g_trainingWakeupReversal.appliedLead
                && g_trainingMacros.releaseBufferedReversal()) {
                g_trainingWakeupReversal.released = true;
                g_trainingWakeupReversal.releasedEarly = true;
                g_trainingWakeupReversal.phase =
                    qtraining::TrainingWakeupReversalPhase::Released;
                g_trainingWakeupReversal.lastOutcome =
                    qtraining::TriggerRequestOutcome::Started;
                g_trainingTriggers.markEventAnswered(
                    g_trainingWakeupReversal.event, g_trainingStepSerial);
                trainingArmReversalParseProbe(p2);
                QLOG("[training-wakeup] activation released on the last "
                     "locked step move=%u character=%u basis=%s forced=%d "
                     "lock=%u perm=%02X action=%u step=%llu",
                     static_cast<unsigned>(
                         g_trainingWakeupReversal.moveIndex),
                     static_cast<unsigned>(
                         g_trainingWakeupReversal.character),
                     releaseGateBased ? "gate" : "terminal",
                     static_cast<int>(
                         rd32(p2 + kTrainingFighterForcedActionOffset)),
                     static_cast<unsigned>(
                         rd32(p2 + kTrainingFighterActionLockEndOffset)),
                     static_cast<unsigned>(
                         trainingCombinedAuthoredPermission(p2, 1u)),
                     static_cast<unsigned>(
                         rd32(p2 + kTrainingFighterActionStateOffset)),
                     static_cast<unsigned long long>(g_trainingStepSerial));
            } else {
                g_trainingWakeupReversal.phase =
                    cursor == g_trainingWakeupReversal.commitIndex
                        ? qtraining::TrainingWakeupReversalPhase::
                              AwaitingFirstResponse
                        : qtraining::TrainingWakeupReversalPhase::Priming;
            }
        }
    }
    if (g_trainingWakeupReversal.active
        && g_trainingWakeupReversal.released
        && !g_trainingMacros.bufferedReversalActive()) {
        const uint32_t liveAction =
            rd32(p2 + kTrainingFighterActionStateOffset);
        if (qtraining::combatActionIsNativeReactionFamily(liveAction)
            || qtraining::combatActionCannotCommitCommand(liveAction)) {
            g_trainingWakeupReversal.episodeAttempted = true;
            QLOG("[training-wakeup] prefix aimed at a chain link: still in "
                 "action=%u after the commit; this episode falls back to the "
                 "late path move=%u step=%llu",
                 static_cast<unsigned>(liveAction),
                 static_cast<unsigned>(g_trainingWakeupReversal.moveIndex),
                 static_cast<unsigned long long>(g_trainingStepSerial));
            (void)g_trainingMacros.requestStop();
            trainingCancelWakeupReversal(
                qtraining::TriggerRequestOutcome::DroppedWindowClosed,
                "prefix aimed at a knockdown chain link");
        }
    }

    if (g_trainingWakeupReversal.active
        && g_trainingWakeupReversal.released) {
        const bool activationPending =
            g_trainingMacros.cursor()
                <= g_trainingWakeupReversal.commitIndex;
        const bool identityMatches = !activationPending
            || (g_trainingCombatIdentity[1].valid
                && g_trainingCombatIdentity[1].fighter == p2
                && g_trainingCombatIdentity[1].character
                       == g_trainingWakeupReversal.character
                && g_trainingCombatIdentity[1].actorToken
                       == g_trainingWakeupReversal.actorToken
                && g_trainingCombatIdentity[1].actionEpoch
                       == g_trainingWakeupReversal.actionEpoch
                && rd32(p2 + kTrainingFighterActionStateOffset)
                       == g_trainingWakeupReversal.resourceAction
                && rd32(p2 + kTrainingFighterDispatchActionOffset)
                       == g_trainingWakeupReversal.dispatchAction);
        if (!g_trainingMacros.bufferedReversalActive()
            || g_trainingWakeupReversal.generation != g_trainingGeneration
            || g_trainingWakeupReversal.fighter != p2
            || g_trainingWakeupReversal.character
                   != rd8(p2 + kTrainingFighterCharacterOffset)
            || !identityMatches
            || (activationPending
                && !g_trainingWakeupReversal.releasedEarly
                && !trainingWakeupActivationStillReady(p2))) {
            trainingCancelWakeupReversal(
                qtraining::TriggerRequestOutcome::DroppedWindowClosed,
                "first response parser gate changed");
        }
    }

    if (g_trainingGuardCancelResponsePlayback) {
        if (g_trainingMacros.state() != MacroState::Playing) {
            g_trainingGuardCancelResponsePlayback = false;
        } else if (!trainingNativeGuardCancelReady(p2, 1u)) {
            (void)g_trainingMacros.requestStop();
            g_trainingGuardCancelResponsePlayback = false;
            QLOG("[training-trigger] guard-cancel response stopped: native "
                 "window closed step=%llu",
                 static_cast<unsigned long long>(g_trainingStepSerial));
        }
    }

    const bool menuCaptured =
        InterlockedCompareExchange(
            &g_trainingMenuCaptured, 0, 0) != 0;
    const bool operationNeutral = g_trainingOperationNeutral;
    g_trainingOperationNeutralForStep = operationNeutral;
    const InputRearmState inputRearm =
        g_trainingInput.rearmState();

    qtraining::MacroStepInput macroInput{};
    macroInput.recordSourceMask = g_trainingPhysical.p1Mask;
    macroInput.playbackFacing = p2Facing;
    macroInput.paused =
        menuCaptured || operationNeutral
        || inputRearm != InputRearmState::Armed;
    const qtraining::MacroStepResult macro =
        g_trainingMacros.advance(g_trainingStepSerial, macroInput);
    if (!macro.valid) return false;
    if (g_trainingMacroAutoStartPending
            != qtraining::TrainingMacroAfterReset::Off
        && !macroInput.paused
        && macro.state == MacroState::Idle) {
        const qtraining::TrainingMacroAfterReset pending =
            g_trainingMacroAutoStartPending;
        g_trainingMacroAutoStartPending =
            qtraining::TrainingMacroAfterReset::Off;
        const bool started =
            pending == qtraining::TrainingMacroAfterReset::Play
                ? g_trainingMacros.requestPlaybackToggle()
                : g_trainingMacros.requestRecording();
        QLOG("[training-macro] auto %s after reset/load: %s step=%llu",
             pending == qtraining::TrainingMacroAfterReset::Play
                 ? "playback" : "recording",
             started ? "armed" : "refused (slot empty or runtime busy)",
             static_cast<unsigned long long>(g_trainingStepSerial));
    }
    if (g_trainingWakeupReversal.released
        && g_trainingWakeupReversal.active
        && macro.state != MacroState::Playing) {
        g_trainingWakeupReversal.active = false;
        g_trainingWakeupReversal.phase =
            qtraining::TrainingWakeupReversalPhase::Completed;
        g_trainingWakeupReversal.lastOutcome =
            qtraining::TriggerRequestOutcome::Started;
    }
    if (g_trainingGuardCancelResponsePlayback
        && macro.state != MacroState::Playing) {
        g_trainingGuardCancelResponsePlayback = false;
    }

    const bool macroNeutralOwnership =
        macro.overrideP2
        && !macro.recordFrameRequested
        && !macro.playbackFrame;
    const bool macroRecordOwnership = macro.overrideP1;

    uint8_t policyFacing = p2Facing;
    bool policyFollowsHuman = false;
    if (macroRecordOwnership) {
        const uintptr_t self = trainingFighter(trainingHumanSlot(), false);
        const uint8_t selfFacing =
            self ? rd8(self + kTrainingFighterFacingOffset) : 0xFFu;
        if (selfFacing <= 1u) {
            policyFollowsHuman = true;
            policyFacing = selfFacing;
        }
    }
    if (policyFollowsHuman != g_trainingPolicyFollowsHuman) {
        g_trainingPolicyFollowsHuman = policyFollowsHuman;
        g_trainingGuardTimingState = qtraining::TrainingGuardTimingState{};
        g_trainingGuardTimingArmed = true;
        g_trainingGuardTimingPendingHit = false;
        g_trainingGuardTimingPendingBlock = false;
        QLOG("[training-dummy] settings now act on P%u "
             "(%s; guard timing restarted)",
             static_cast<unsigned>(trainingPolicySlot()) + 1u,
             policyFollowsHuman
                 ? "recording, so your own fighter is the one standing there"
                 : "the dummy");
    }

    qtraining::RecoveryObservation recoveryObservation{};
    (void)trainingSampleRecoveryObservation(
        static_cast<uint8_t>(trainingDummySlot()), p2Facing,
        menuCaptured, operationNeutral,
        macro.playbackFrame,
        macroRecordOwnership || macroNeutralOwnership,
        recoveryObservation);
    const qtraining::RecoveryEmission recovery =
        g_trainingRecovery.advance(
            g_trainingStepSerial, g_trainingRecoverySettings,
            recoveryObservation);
    g_trainingRecoveryEmission = recovery;
    g_trainingRecoveryLockSample = recoveryObservation.recoveryLock;
    g_trainingRecoveryArmable = static_cast<uint8_t>(
        qtraining::recoveryArmableMask(recoveryObservation));
    g_trainingRecoveryReadoutValid =
        recoveryObservation.readable ? 1u : 0u;
    if (!recovery.valid) return false;

    uint32_t policyArmableMask = 0u;
    if (macroRecordOwnership) {
        qtraining::RecoveryObservation selfObservation{};
        (void)trainingSampleRecoveryObservation(
            static_cast<uint8_t>(trainingHumanSlot()),
            policyFollowsHuman ? policyFacing : 0u, menuCaptured,
            operationNeutral, false, false, selfObservation);
        g_trainingRecoveryEmissionP1 =
            g_trainingRecoveryP1.advance(
                g_trainingStepSerial, g_trainingRecoverySettings,
                selfObservation);
        if (policyFollowsHuman) {
            policyArmableMask =
                qtraining::recoveryArmableMask(selfObservation);
        }
    }

    {
        qtraining::TrainingGuardTimingStep timingStep{};
        timingStep.dummyWasHit = g_trainingGuardTimingPendingHit;
        timingStep.dummyBlocked = g_trainingGuardTimingPendingBlock;
        g_trainingGuardTimingPendingHit = false;
        g_trainingGuardTimingPendingBlock = false;
        const uintptr_t defender =
            trainingFighter(trainingPolicySlot(), false);
        const uint32_t defenderAction = defender != 0u
            ? rd32(defender + kTrainingFighterActionStateOffset)
            : 0xFFFFFFFFu;
        timingStep.dummyNeutral = defenderAction == 0u
            || defenderAction == 26u
            || defenderAction == 27u || defenderAction == 28u;
        g_trainingGuardTimingArmed = qtraining::guardTimingAdvance(
            g_trainingGuardTimingConfig, g_trainingGuardTimingState,
            timingStep);
    }
    qtraining::GuardDecision guard = trainingGuardDecision(policyFacing);
    trainingGuardAvoidTerminalRoll(guard);
    g_trainingLastGuardDecision = guard;
    if (!guard.valid) return false;
    const qtraining::RecoveryEmission& policyRecovery =
        policyFollowsHuman ? g_trainingRecoveryEmissionP1 : recovery;
    /* sub_47CAE0's air pattern is {0x20000, 0, 0x20000}: a crouch guard that blinks is an air tech. */
    const bool policyStandsDownForRecovery =
        policyArmableMask != 0u && !policyRecovery.active;
    const qtraining::DummyPostureResult posture =
        trainingDummyPosture(guard, policyFacing, policyRecovery);
    if (!posture.valid) return false;

    qtraining::TrainingInputStep input{};
    input.lifecycleAvailable = true;
    input.menuCaptured = menuCaptured;
    input.operationNeutral = operationNeutral;
    const bool swap = g_trainingPlayerSwap;
    input.dummyIsP1 = swap;
    input.p1PhysicalMask =
        swap ? g_trainingPhysical.p2Mask : g_trainingPhysical.p1Mask;
    input.p2PhysicalMask =
        swap ? g_trainingPhysical.p1Mask : g_trainingPhysical.p2Mask;
    input.humanDrivesDummyLane = macroRecordOwnership;
    input.macroOverrideP1 = swap ? macro.overrideP2 : macro.overrideP1;
    input.macroOverrideP2 = swap ? macro.overrideP1 : macro.overrideP2;
    input.macroPlaybackActive = macro.playbackFrame;
    input.macroP1Mask = swap ? macro.p2Mask : macro.p1Mask;
    input.macroP2Mask = swap ? macro.p1Mask : macro.p2Mask;
    const bool dummyOwnsDirection = macroRecordOwnership
        ? (policyFollowsHuman && !policyStandsDownForRecovery)
        : !macroNeutralOwnership;
    const bool releaseGuardForResponse =
        dummyOwnsDirection && guard.guarding
        && g_trainingWakeupReversal.active
        && qtraining::TrainingTriggerController::prebufferRestoresGuard(
               g_trainingWakeupReversal.event)
        && g_trainingWakeupReversal.lockedStepsRemaining <= 1u;
    input.dummyDirections = dummyOwnsDirection
        ? (releaseGuardForResponse ? 0u : posture.directions)
        : 0u;
    g_trainingLastEmittedDummyDirections = input.dummyDirections;
    g_trainingLastEmittedOwnedAxes =
        dummyOwnsDirection ? posture.ownedAxes : 0u;
    input.dummyOwnedAxes = dummyOwnsDirection
        ? (releaseGuardForResponse ? 0u : posture.ownedAxes)
        : 0u;
    if (g_trainingGuardMode != GuardMode::Off
        && (!dummyOwnsDirection
            || (macro.playbackFrame && !g_trainingGuardDuringPlayback))) {
        ++g_trainingGuardYieldSteps;
    }
    input.dummyButtons =
        dummyOwnsDirection ? posture.buttons : 0u;
    input.dummyOwnedButtons =
        dummyOwnsDirection ? posture.ownedButtons : 0u;
    input.dummyExclusive = dummyOwnsDirection && policyRecovery.active;
    input.guardDuringPlayback =
        g_trainingGuardDuringPlayback && !macro.generatedPlayback;
    input.horizontalSocd =
        g_inputConfigPresent
            ? g_inputLaunchConfig.horizontalSocd
            : qinput::SocdPolicy::Native;
    input.verticalSocd =
        g_inputConfigPresent
            ? g_inputLaunchConfig.verticalSocd
            : qinput::SocdPolicy::Native;

    const qtraining::TrainingInputResult composed =
        g_trainingInput.advance(g_trainingStepSerial, input);
    if (!composed.valid) return false;
    if (macro.recordFrameRequested) {
        const qtraining::MacroRecordCommitResult committed =
            g_trainingMacros.commitRecordedFrame(
                g_trainingStepSerial,
                swap ? composed.p1Mask : composed.p2Mask, p2Facing);
        if (!committed.valid) return false;
    }
    g_trainingGuardAwayRead = {};
    if (guard.guarding
        && posture.horizontalSource == qtraining::DummyPostureSource::Guard
        && (composed.postureOwnedAxes & qtraining::kDummyHorizontalAxis)
               != 0u) {
        const uint32_t lane =
            composed.postureLane == qtraining::kTrainingDrivenLaneP1 ? 0u : 1u;
        const uint32_t horizontal =
            (lane == 0u ? composed.p1Mask : composed.p2Mask)
            & (qinput::kLogicalLeft | qinput::kLogicalRight);
        if (horizontal == qinput::kLogicalLeft
            || horizontal == qinput::kLogicalRight) {
            g_trainingGuardAwayRead.armed = true;
            g_trainingGuardAwayRead.lane = static_cast<uint8_t>(lane);
            g_trainingGuardAwayRead.awayBit = horizontal;
        }
    }
    g_trainingFinalInput[0] = composed.p1Mask;
    g_trainingFinalInput[1] = composed.p2Mask;
    g_trainingLastComposedInput[0] = composed.p1Mask;
    g_trainingLastComposedInput[1] = composed.p2Mask;

    qinput::InputDisplayStep display{};
    display.mode = qinput::InputDisplayMode::Training;
    display.generation = g_trainingGeneration;
    display.stepSerial = g_trainingStepSerial;
    display.drivenLane = qtraining::trainingDrivenLane(
        macroRecordOwnership, g_trainingPlayerSwap);
    display.laneActiveMask = 1u << display.drivenLane;
    display.laneLogicalMask[0] = composed.p1Mask;
    display.laneLogicalMask[1] = composed.p2Mask;
    (void)qinput::inputDisplay().publishStep(display);

    g_trainingOperationNeutral = false;
    return true;
}

static bool trainingRestoreScratchFailSafe() {
    if (!g_trainingScratch.valid
        || !g_trainingScratch.fpu.valid
        || g_trainingScratch.generation != g_trainingGeneration
        || g_trainingScratch.layoutGeneration
            != g_rb.layoutGeneration()
        || g_trainingScratch.bytes.size() != g_rb.totalSize()
        || !g_rb.loadFrom(g_trainingScratch.bytes.data())) {
        return false;
    }
    const bool finished = trainingFinishRestoredImage(
        g_trainingScratch.fpu, "scratch");
    g_trainingRecovery.restoreRngState(g_trainingScratch.recoveryRng);
    return finished;
}

static bool trainingLiveE2EApplyLethalDamageProbe() {
    if (!trainingLiveE2EOfflineControlEnabled())
        return true;
    const uint32_t requestId =
        g_trainingLiveE2EMailbox.lethalDamageRequestId;
    if (requestId == 0u
        || (requestId
                == g_trainingLiveE2EMailbox.lethalDamageAckId
            && g_trainingGeneration
                == g_trainingLiveE2EMailbox.lethalDamageGeneration)) {
        return true;
    }

    constexpr int64_t kMaximumProbeDamage = 1000000;
    constexpr uint32_t kProbeApplied = 1u;
    constexpr uint32_t kProbeRejected = 2u;
    g_trainingLiveE2EMailbox.lethalDamageAmount = 0u;
    g_trainingLiveE2EMailbox.lethalDamageVictimSlot = 1u;

    auto publish = [&](uint32_t result, uint32_t amount) {
        g_trainingLiveE2EMailbox.lethalDamageAckResult = result;
        g_trainingLiveE2EMailbox.lethalDamageAmount = amount;
        g_trainingLiveE2EMailbox.lethalDamageGeneration =
            g_trainingGeneration;
        MemoryBarrier();
        g_trainingLiveE2EMailbox.lethalDamageAckId = requestId;
        MemoryBarrier();
        trainingLiveE2EClearDamageRequest();
    };
    auto failAndRestore = [&](const char* why) {
        publish(kProbeRejected, 0u);
        g_trainingStepPending = false;
        const bool restored = trainingRestoreScratchFailSafe();
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            restored ? why
                     : "Live lethal probe failed and scratch restore failed");
        return false;
    };

    if (!g_trainingStepPending
        || rd32(kSceneStateAddr) != 2u
        || rd32(kMatchSubAddr) != 0u
        || !g_trainingHooksInstalled
        || g_trainingOrigDamage == nullptr) {
        return failAndRestore(
            "Live lethal probe was outside its qualified step");
    }

    const uintptr_t victim = trainingFighter(trainingDummySlot(), false);
    const uintptr_t mirror = trainingFighter(trainingDummySlot(), true);
    if (!victim || !mirror
        || rd8(victim + kFighterPlayerSlotOffset) != 1u) {
        return failAndRestore(
            "Live lethal probe victim/mirror validation failed");
    }
    const uintptr_t life = netBattleLifeAddress(1u);
    if (!rangeReadable(life, 16u)) {
        return failAndRestore(
            "Live lethal probe life record was invalid");
    }
    const int64_t current = static_cast<int32_t>(
        rd32(life + kBattleLifeCurrentOffset));
    const int64_t reserve = static_cast<int32_t>(
        rd32(life + kBattleLifeReserveOffset));
    const int64_t pending = static_cast<int32_t>(
        rd32(life + kBattleLifeDamageOffset));
    const int64_t effective = current + reserve - pending;
    if (effective < 0 || effective >= kMaximumProbeDamage) {
        return failAndRestore(
            "Live lethal probe damage bound was invalid");
    }
    const uint32_t damage =
        static_cast<uint32_t>(effective + 1);
    const uint32_t activeFatalCause =
        rd32(victim + kTrainingFighterFatalCauseOffset);
    const uint32_t activeFatalAttacker =
        rd32(victim + kTrainingFighterFatalAttackerOffset);
    const uint32_t mirrorFatalCause =
        rd32(mirror + kTrainingFighterFatalCauseOffset);
    const uint32_t mirrorFatalAttacker =
        rd32(mirror + kTrainingFighterFatalAttackerOffset);
    const uint32_t detourCountBefore =
        g_trainingLiveE2EMailbox.lethalDamageDetourCount;

    const auto damageFn =
        reinterpret_cast<TrainingDamageFn>(kTrainingDamageFn);
    (void)damageFn(
        0, static_cast<int>(victim), 0,
        static_cast<int>(damage));

    const int64_t postCurrent = static_cast<int32_t>(
        rd32(life + kBattleLifeCurrentOffset));
    const int64_t postReserve = static_cast<int32_t>(
        rd32(life + kBattleLifeReserveOffset));
    const int64_t postPending = static_cast<int32_t>(
        rd32(life + kBattleLifeDamageOffset));
    const bool detourObserved =
        g_trainingLiveE2EMailbox.lethalDamageDetourCount
            == detourCountBefore + 1u;
    const bool fatalRestored =
        rd32(victim + kTrainingFighterFatalCauseOffset)
                == activeFatalCause
        && rd32(victim + kTrainingFighterFatalAttackerOffset)
                == activeFatalAttacker
        && rd32(mirror + kTrainingFighterFatalCauseOffset)
                == mirrorFatalCause
        && rd32(mirror + kTrainingFighterFatalAttackerOffset)
                == mirrorFatalAttacker;
    if (!detourObserved
        || g_trainingMutationFailed
        || postCurrent + postReserve - postPending != 0
        || !fatalRestored
        || !trainingInvariantsHold()) {
        return failAndRestore(
            "Live lethal probe detour postcondition failed");
    }

    publish(kProbeApplied, damage);
    QLOG("[training-live-e2e] lethal damage probe applied "
         "request=%u victim=1 damage=%u detours=%u",
         requestId, damage,
         g_trainingLiveE2EMailbox.lethalDamageDetourCount);
    return true;
}

static void trainingAdvanceCombatToolsPostStep();

static bool trainingFinalizePendingStep() {
    if (!g_trainingStepPending) return !g_trainingMutationFailed;
    const bool invariantFailure =
        g_trainingStepFailure || !trainingInvariantsHold();
    g_trainingStepPending = false;
    g_trainingOperationNeutralForStep = false;
    g_trainingFinalInput[0] = g_trainingFinalInput[1] = 0u;
    if (!invariantFailure) {
        trainingAdvanceCombatToolsPostStep();
        g_trainingScratch.valid = false;
        trainingCopyText(g_trainingStatusText,
                         "Active: native step validated");
        return true;
    }

    const bool restored = trainingRestoreScratchFailSafe();
    g_trainingMutationFailed = true;
    g_trainingDisableReason =
        TrainingDisableReason::InvariantFailure;
    ++g_trainingStickyErrors;
    trainingCopyText(
        g_trainingDisableText,
        restored
            ? "Training invariant failed; stable pre-step state restored"
            : "Training invariant failed and scratch restore failed");
    trainingCopyText(g_trainingResultText,
                     g_trainingDisableText);
    QLOG("[training] post-step invariant FAILED; scratchRestored=%d "
         "scene=%u sub=%u result=%u progress=%u "
         "outcome=%u/%u wins=%u/%u",
         restored ? 1 : 0, rd32(kSceneStateAddr),
         rd32(kMatchSubAddr), rd32(kRoundResultStateAddr),
         rd32(kTrainingRoundProgressAddr),
         rd32(kTrainingOutcomeP1Addr),
         rd32(kTrainingOutcomeP2Addr),
         rd32(kTrainingWinsP1Addr), rd32(kTrainingWinsP2Addr));
    return false;
}

static bool trainingQualifiedGameplayGate() {
    return g_trainingLaunchRequested
        && g_trainingHooksInstalled
        && !g_trainingMutationFailed
        && !g_netActive
        && g_mode == RunMode::Observe
        && rd32(kTrainingModeAddr) == 0u
        && rd32(kSceneStateAddr) == 2u
        && rd32(kMatchSubAddr) == 0u
        && rd32(kTrainingVsLaunchAddr) != 1u
        && rd8(kTrainingPauseAddr) == 0u
        && rd8(kTrainingFreezeAddr) == 0u;
}

static bool trainingCombatDetourActive() {
    return g_trainingStepPending && trainingQualifiedGameplayGate();
}

static void trainingRecordContact(
        qtraining::CombatContactKind kind, uintptr_t attacker,
        uintptr_t victim, bool completedAdmittedStep = false) {
    if (!g_trainingContactHooksReady
        || (completedAdmittedStep
                ? !trainingQualifiedGameplayGate()
                : !trainingCombatDetourActive())) {
        return;
    }
    if (kind == qtraining::CombatContactKind::Hit
        || kind == qtraining::CombatContactKind::Block) {
        const uintptr_t defender =
            trainingFighter(trainingPolicySlot(), false);
        if (defender != 0u && victim == defender) {
            if (kind == qtraining::CombatContactKind::Hit)
                g_trainingGuardTimingPendingHit = true;
            else
                g_trainingGuardTimingPendingBlock = true;
        }
    }
    uint8_t attackerOwner = 0u;
    uint8_t victimOwner = 0u;
    if (!trainingActorOwner(attacker, attackerOwner)
        || !trainingActorOwner(victim, victimOwner)
        || attackerOwner == victimOwner) {
        return;
    }
    if (g_trainingContactCount >= kTrainingContactCapacity) {
        ++g_trainingContactDrops;
        return;
    }

    qtraining::CombatContactEvent& event =
        g_trainingContacts[g_trainingContactCount++];
    event = qtraining::CombatContactEvent{};
    ++g_trainingContactSerial;
    if (g_trainingContactSerial == 0u) ++g_trainingContactSerial;
    event.serial = g_trainingContactSerial;
    event.stepSerial = g_trainingStepSerial;
    event.kind = kind;
    event.attackerOwner = attackerOwner;
    event.victimOwner = victimOwner;
    if (kind == qtraining::CombatContactKind::Hit && victimOwner < 2u) {
        if (++g_trainingHitEpoch[victimOwner] == 0u)
            ++g_trainingHitEpoch[victimOwner];
    }
    const uintptr_t attackerPrimary =
        trainingFighter(attackerOwner, false);
    const uintptr_t victimPrimary = trainingFighter(victimOwner, false);
    event.attackerIsPrimary =
        attackerPrimary != 0u && attackerPrimary == attacker;
    event.victimIsPrimary =
        victimPrimary != 0u && victimPrimary == victim;

    if (attackerPrimary) {
        TrainingCombatActorIdentity& identity =
            trainingObserveCombatIdentity(attackerOwner, attackerPrimary);
        event.attackerActorToken = identity.actorToken;
        event.attackerActionEpoch = identity.actionEpoch;
    }
    if (event.victimIsPrimary) {
        TrainingCombatActorIdentity& identity =
            trainingObserveCombatIdentity(victimOwner, victimPrimary);
        event.victimActorToken = identity.actorToken;
        event.victimActionEpoch = identity.actionEpoch;
    }

    const bool throwContact =
        kind == qtraining::CombatContactKind::Throw;

    const bool armoredHit = kind == qtraining::CombatContactKind::Hit
        && (rd32(victim + kTrainingFighterMoveGateOffset) & 0x20u) != 0u;
    bool excludedEpisode = false;
    if (!throwContact && event.attackerIsPrimary) {
        const uint8_t character =
            rd8(attacker + kTrainingFighterCharacterOffset);
        const uint32_t action =
            rd32(attacker + kTrainingFighterActionStateOffset);
        excludedEpisode =
            !qtraining::trainingActionAllowsConventionalFrameAdvantage(
                character, action)
            || trainingCombatExcludedFromConventionalAdvantage(
                qtraining::trainingActionMeta(character, action));
    }
    if (!throwContact && event.victimIsPrimary) {
        excludedEpisode = excludedEpisode
            || trainingCombatExcludedFromConventionalAdvantage(
                qtraining::trainingActionMeta(
                    rd8(victim + kTrainingFighterCharacterOffset),
                    rd32(victim + kTrainingFighterActionStateOffset)));
    }
    event.defenderLockEligible = event.victimIsPrimary
        && !armoredHit && !excludedEpisode;

    TrainingNaturalRecoveryExpectation& expectation =
        g_trainingNaturalExpectation[attackerOwner];
    if (event.attackerIsPrimary) {
        const TrainingCombatActorIdentity& identity =
            g_trainingCombatIdentity[attackerOwner];
        expectation.active = true;
        expectation.fighter = attacker;
        expectation.character = identity.character;
        expectation.resourceAction = identity.resourceAction;
        expectation.dispatchAction = identity.dispatchAction;
        expectation.lastLocalFrame = identity.localFrame;
        expectation.actorToken = identity.actorToken;
        expectation.actionEpoch = identity.actionEpoch;
        expectation.contactKind = kind;
    } else {
        expectation = TrainingNaturalRecoveryExpectation{};
    }
}

static bool trainingThrowContactAlreadyRecorded(
        uint8_t attackerOwner, uint8_t victimOwner) {
    for (uint32_t index = 0u; index < g_trainingContactCount; ++index) {
        const qtraining::CombatContactEvent& event =
            g_trainingContacts[index];
        if (event.stepSerial == g_trainingStepSerial
            && event.kind == qtraining::CombatContactKind::Throw
            && event.attackerOwner == attackerOwner
            && event.victimOwner == victimOwner) {
            return true;
        }
    }
    return false;
}

static void trainingRecordThrowContactOnce(
        uintptr_t attacker, uintptr_t victim,
        bool completedAdmittedStep = false,
        bool allowThrowTechResult = false) {
    uint8_t attackerOwner = 0u;
    uint8_t victimOwner = 0u;
    if (!trainingActorOwner(attacker, attackerOwner)
        || !trainingActorOwner(victim, victimOwner)
        || attackerOwner == victimOwner
        || trainingFighter(attackerOwner, false) != attacker
        || trainingFighter(victimOwner, false) != victim
        || rd32(attacker + kTrainingFighterPairedActorOffset) != victim) {
        return;
    }
    const uint32_t attackerAction =
        rd32(attacker + kTrainingFighterActionStateOffset);
    const uint32_t victimAction =
        rd32(victim + kTrainingFighterActionStateOffset);
    const bool exactThrower = attackerAction == 70u;
    const bool exactTechCounterpart = allowThrowTechResult
        && attackerAction == 72u
        && qtraining::combatActionIsThrowTechResult(victimAction);
    if (!exactThrower && !exactTechCounterpart) return;
    const bool validVictimPath = trainingCombatExactThrowReception(
        victimAction,
        qtraining::trainingDispatchFamily(
            rd32(victim + kTrainingFighterDispatchActionOffset)))
        || (allowThrowTechResult
            && qtraining::combatActionIsThrowTechResult(victimAction));
    if (!validVictimPath) return;
    if (trainingThrowContactAlreadyRecorded(attackerOwner, victimOwner))
        return;
    trainingRecordContact(
        qtraining::CombatContactKind::Throw, attacker, victim,
        completedAdmittedStep);
}

static void trainingLatchPendingInstalledThrowContact(
        uintptr_t attacker, uintptr_t victim) {
    uint8_t attackerOwner = 0u;
    uint8_t victimOwner = 0u;
    if (!trainingActorOwner(attacker, attackerOwner)
        || !trainingActorOwner(victim, victimOwner)
        || attackerOwner == victimOwner
        || trainingFighter(attackerOwner, false) != attacker
        || trainingFighter(victimOwner, false) != victim
        || rd32(attacker + kTrainingFighterNextActionOffset) != 70u
        || rd32(attacker + kTrainingFighterTransitionFlagOffset) != 1u
        || rd32(attacker + kTrainingFighterPairedActorOffset) != victim
        || rd32(victim + kTrainingFighterNextActionOffset) != 200u
        || rd32(victim + kTrainingFighterDispatchActionOffset) != 200u
        || rd32(victim + kTrainingFighterInitStatusOffset) != UINT32_MAX
        || rd32(victim + kTrainingFighterForcedActionOffset) != UINT32_MAX) {
        return;
    }
    const TrainingThrowReceptionObservation& before =
        g_trainingThrowReceptionObservation[victimOwner];
    if (!before.valid || before.fighter != victim
        || before.inExactReception) {
        return;
    }
    TrainingPendingThrowContact& pending =
        g_trainingPendingThrowContact[victimOwner];
    pending.valid = true;
    pending.attacker = attacker;
    pending.victim = victim;
    pending.stepSerial = g_trainingStepSerial;
}

static void trainingFlushPendingThrowContacts() {
    for (TrainingPendingThrowContact& pending :
         g_trainingPendingThrowContact) {
        if (!pending.valid) continue;
        const TrainingPendingThrowContact current = pending;
        pending = TrainingPendingThrowContact{};
        if (current.stepSerial != g_trainingStepSerial) continue;
        trainingRecordThrowContactOnce(
            current.attacker, current.victim, true, true);
    }
}

static void trainingPrimeThrowReceptionObservations() {
    for (uint8_t owner = 0u; owner < 2u; ++owner) {
        const uintptr_t fighter = trainingFighter(owner, false);
        TrainingThrowReceptionObservation& observation =
            g_trainingThrowReceptionObservation[owner];
        if (!fighter || !trainingCombatFrameReadable(fighter)) {
            observation = TrainingThrowReceptionObservation{};
            continue;
        }
        observation.valid = true;
        observation.fighter = fighter;
        observation.inExactReception = trainingCombatExactThrowReception(
            rd32(fighter + kTrainingFighterActionStateOffset),
            qtraining::trainingDispatchFamily(
                rd32(fighter + kTrainingFighterDispatchActionOffset)));
    }
}

static void trainingRecordThrowReceptionEntries() {
    for (uint8_t victimOwner = 0u; victimOwner < 2u; ++victimOwner) {
        const uintptr_t victim = trainingFighter(victimOwner, false);
        TrainingThrowReceptionObservation& previous =
            g_trainingThrowReceptionObservation[victimOwner];
        if (!victim || !trainingCombatFrameReadable(victim)) {
            previous = TrainingThrowReceptionObservation{};
            continue;
        }

        const bool inExactReception = trainingCombatExactThrowReception(
            rd32(victim + kTrainingFighterActionStateOffset),
            qtraining::trainingDispatchFamily(
                rd32(victim + kTrainingFighterDispatchActionOffset)));
        const bool entered = previous.valid
            && previous.fighter == victim
            && !previous.inExactReception
            && inExactReception;
        previous.valid = true;
        previous.fighter = victim;
        previous.inExactReception = inExactReception;
        if (!entered) continue;

        const uint8_t attackerOwner = static_cast<uint8_t>(victimOwner ^ 1u);
        const uintptr_t attacker = trainingFighter(attackerOwner, false);
        if (attacker) {
            trainingRecordThrowContactOnce(
                attacker, victim, true);
        }
    }
}

static int __fastcall hkTrainingGuardContact(
        int attacker, int defender, int a3, int a4) {
    const int result = g_trainingOrigGuardContact
        ? g_trainingOrigGuardContact(attacker, defender, a3, a4) : 0;
    if (result == 1) {
        trainingRecordContact(
            qtraining::CombatContactKind::Block,
            static_cast<uintptr_t>(attacker),
            static_cast<uintptr_t>(defender));
    }
    return result;
}

static int __fastcall hkTrainingHitContact(
        int attacker, int defender, int a3, int a4,
        uint32_t* outFlags, int* outEffect) {
    const int result = g_trainingOrigHitContact
        ? g_trainingOrigHitContact(
              attacker, defender, a3, a4, outFlags, outEffect) : 0;
    if (result > 0) {
        trainingRecordContact(
            qtraining::CombatContactKind::Hit,
            static_cast<uintptr_t>(attacker),
            static_cast<uintptr_t>(defender));
    }
    return result;
}

static int __fastcall hkTrainingNaturalActionTerminal(
        int* fighter, void* ) {
    const uintptr_t actor = reinterpret_cast<uintptr_t>(fighter);
    const bool readable = rangeReadable(actor, addr::kFighterSize);
    const uint32_t before = readable
        ? rd32(actor + kTrainingFighterInitStatusOffset) : 0u;
    uint8_t owner = 0u;
    const bool capture = g_trainingContactHooksReady
        && trainingCombatDetourActive() && readable
        && trainingActorOwner(actor, owner)
        && trainingFighter(owner, false) == actor;
    TrainingCombatActorIdentity identity{};
    if (capture) {
        identity = trainingObserveCombatIdentity(owner, actor);
        TrainingPreHandlerAction& pre =
            g_trainingPreHandlerAction[owner];
        pre.valid = true;
        pre.fighter = actor;
        pre.character = identity.character;
        pre.resourceAction = identity.resourceAction;
        pre.dispatchAction = identity.dispatchAction;
        pre.localFrame = identity.localFrame;
        pre.actorToken = identity.actorToken;
        pre.actionEpoch = identity.actionEpoch;
        pre.stepSerial = g_trainingStepSerial;
    }
    const int result = g_trainingOrigNaturalActionTerminal
        ? g_trainingOrigNaturalActionTerminal(fighter) : 0;
    if (!capture || before == 1u
        || rd32(actor + kTrainingFighterInitStatusOffset) != 1u) {
        return result;
    }
    TrainingNaturalTerminalPulse& pulse =
        g_trainingNaturalTerminal[owner];
    pulse.valid = true;
    pulse.fighter = actor;
    pulse.character = identity.character;
    pulse.resourceAction = identity.resourceAction;
    pulse.dispatchAction = identity.dispatchAction;
    pulse.actorToken = identity.actorToken;
    pulse.actionEpoch = identity.actionEpoch;
    pulse.stepSerial = g_trainingStepSerial;
    return result;
}

static uint64_t trainingCombatMix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31u);
}

static bool trainingThrowLifecycleAction(uintptr_t fighter) {
    if (!fighter) return true;
    const auto isLifecycleAction = [](uint32_t action) {
        return action >= 91u && action <= 98u;
    };
    return isLifecycleAction(
               rd32(fighter + kTrainingFighterActionStateOffset))
        || isLifecycleAction(
               rd32(fighter + kTrainingFighterDispatchActionOffset));
}

static bool trainingAutomaticThrowTechAllowed(
        uintptr_t attacker, uintptr_t victim) {
    if (!g_trainingThrowTechHookReady || !trainingCombatDetourActive()
        || g_trainingLifecycle != TrainingLifecycleState::Active
        || g_trainingOperationNeutralForStep
        || g_trainingRoundStartNeutralPending
        || rd32(kRoundResultStateAddr) != 0u
        || !rangeReadable(attacker, addr::kFighterSize)
        || !rangeReadable(victim, addr::kFighterSize)
        || rd32(attacker + kFighterRoundLostOffset) != 0u
        || rd32(victim + kFighterRoundLostOffset) != 0u
        || trainingThrowLifecycleAction(attacker)
        || trainingThrowLifecycleAction(victim)) {
        return false;
    }
    uint8_t attackerOwner = 0u;
    uint8_t victimOwner = 0u;
    return trainingActorOwner(attacker, attackerOwner)
        && trainingActorOwner(victim, victimOwner)
        && attackerOwner != victimOwner
        && victimOwner == trainingPolicySlot()
        && trainingFighter(attackerOwner, false) == attacker
        && trainingFighter(victimOwner, false) == victim;
}

static int __fastcall hkTrainingThrowInstall(
        int attackerValue, int victimValue) {
    const uintptr_t attacker = static_cast<uintptr_t>(attackerValue);
    const uintptr_t victim = static_cast<uintptr_t>(victimValue);
    const bool preReadable = rangeReadable(attacker, addr::kFighterSize)
        && rangeReadable(victim, addr::kFighterSize);
    const bool exactPrecondition = preReadable
        && rd32(attacker + kTrainingFighterNextActionOffset) == 70u
        && rd32(attacker + kTrainingFighterPrevActionOffset) != 70u;

    const int result = g_trainingOrigThrowInstall
        ? g_trainingOrigThrowInstall(attackerValue, victimValue) : 0;
    if (!exactPrecondition || !g_trainingThrowTechHookReady
        || !trainingCombatDetourActive()
        || !rangeReadable(attacker, addr::kFighterSize)
        || !rangeReadable(victim, addr::kFighterSize)) {
        return result;
    }

    const bool exactInstalledPair =
        rd32(attacker + kTrainingFighterNextActionOffset) == 70u
        && rd32(attacker + kTrainingFighterTransitionFlagOffset) == 1u
        && rd32(attacker + kTrainingFighterPairedActorOffset) == victim
        && rd32(victim + kTrainingFighterNextActionOffset) == 200u
        && rd32(victim + kTrainingFighterDispatchActionOffset) == 200u
        && rd32(victim + kTrainingFighterInitStatusOffset) == UINT32_MAX
        && rd32(victim + kTrainingFighterForcedActionOffset) == UINT32_MAX;
    if (!exactInstalledPair) return result;

    uint8_t attackerOwner = 0u;
    uint8_t victimOwner = 0u;
    if (!trainingActorOwner(attacker, attackerOwner)
        || !trainingActorOwner(victim, victimOwner)
        || attackerOwner == victimOwner
        || trainingFighter(attackerOwner, false) != attacker
        || trainingFighter(victimOwner, false) != victim) {
        return result;
    }

    trainingLatchPendingInstalledThrowContact(attacker, victim);

    if (!trainingAutomaticThrowTechAllowed(attacker, victim)) return result;

    TrainingThrowTechTelemetry& telemetry = g_trainingThrowTech;
    const bool sameOpportunity = telemetry.haveDecision
        && telemetry.lastDecisionStep == g_trainingStepSerial
        && telemetry.lastDecisionAttacker == attacker
        && telemetry.lastDecisionFighter == victim;
    if (!sameOpportunity) {
        telemetry.haveDecision = true;
        telemetry.lastDecisionStep = g_trainingStepSerial;
        telemetry.lastDecisionAttacker = attacker;
        telemetry.lastDecisionFighter = victim;
        telemetry.automaticDecision = false;
        telemetry.nativeAcceptedDecision = false;
        ++telemetry.opportunities;
        if (telemetry.chancePercent != 0u) {
            const uint64_t token = trainingCombatMix64(
                (static_cast<uint64_t>(g_trainingGeneration) << 32u)
                ^ g_trainingStepSerial
                ^ static_cast<uint64_t>(attacker)
                ^ (static_cast<uint64_t>(telemetry.opportunities)
                   * 0xD6E8FEB86659FD93ull));
            telemetry.automaticDecision =
                telemetry.chancePercent >= 100u
                || token % 100u < telemetry.chancePercent;
            if (telemetry.automaticDecision)
                ++telemetry.automaticAccepted;
        }
        QLOG("[training-throw-tech] opportunity step=%llu chance=%u "
             "automatic=%d attacker=%08X victim=%08X",
             static_cast<unsigned long long>(g_trainingStepSerial),
             telemetry.chancePercent,
             telemetry.automaticDecision ? 1 : 0,
             static_cast<unsigned>(attacker),
             static_cast<unsigned>(victim));
    }

    if (telemetry.automaticDecision) {
        wr32(victim + kTrainingFighterThrowTechLatchOffset,
             rd32(victim + kTrainingFighterThrowTechLatchOffset) | 1u);
    }
    return result;
}

static int __fastcall hkTrainingThrowTech(
        int* fighter, void* , int evalFlags) {
    const int nativeResult = g_trainingOrigThrowTech
        ? g_trainingOrigThrowTech(fighter, evalFlags) : 0;
    const uintptr_t actor = reinterpret_cast<uintptr_t>(fighter);
    if (!g_trainingThrowTechHookReady || !trainingCombatDetourActive()
        || (static_cast<uint32_t>(evalFlags) & 0xFFFF0000u) != 0u
        || !rangeReadable(actor, addr::kFighterSize)) {
        return nativeResult;
    }
    uint8_t owner = 0u;
    if (!trainingActorOwner(actor, owner)
        || trainingFighter(owner, false) != actor) {
        return nativeResult;
    }

    if (owner != trainingPolicySlot()) return nativeResult;

    TrainingThrowTechTelemetry& telemetry = g_trainingThrowTech;
    const bool sameOpportunity = telemetry.haveDecision
        && telemetry.lastDecisionStep == g_trainingStepSerial
        && telemetry.lastDecisionFighter == actor;
    if (sameOpportunity && nativeResult == 1
        && !telemetry.nativeAcceptedDecision) {
        telemetry.nativeAcceptedDecision = true;
        ++telemetry.nativeAccepted;
    }
    return nativeResult;
}

static void trainingAdvanceCombatToolsPostStep() {
    if (!g_trainingContactHooksReady) {
        g_trainingContactCount = 0u;
        g_trainingCombatToolsReadoutValid = 0u;
        return;
    }

    trainingFlushPendingThrowContacts();
    trainingRecordThrowReceptionEntries();

    if (g_trainingContactDrops != 0u) {
        const uint32_t dropped = g_trainingContactDrops;
        trainingResetCombatToolTimeline(true);
        QLOG("[training-contact] discarded incomplete boundary; "
             "dropped=%u step=%llu",
             dropped,
             static_cast<unsigned long long>(g_trainingStepSerial));
    }

    qtraining::CombatFrameSample sample{};
    sample.trainingGeneration = g_trainingGeneration;
    sample.stepSerial = g_trainingStepSerial;
    bool readable = true;
    bool roundValid = rd32(kRoundResultStateAddr) == 0u;
    for (uint8_t owner = 0u; owner < 2u; ++owner) {
        const uintptr_t fighter = trainingFighter(owner, false);
        if (!fighter || !trainingCombatFrameReadable(fighter)) {
            readable = false;
            continue;
        }
        TrainingCombatActorIdentity& identity =
            trainingObserveCombatIdentity(owner, fighter);
        if (identity.character > 36u) readable = false;
        const uint32_t action =
            rd32(fighter + kTrainingFighterActionStateOffset);
        const uint32_t dispatch =
            rd32(fighter + kTrainingFighterDispatchActionOffset);
        const qtraining::TrainingActionMeta actionMeta =
            qtraining::trainingActionMeta(identity.character, action);
        const qtraining::TrainingDispatchFamily dispatchFamily =
            qtraining::trainingDispatchFamily(dispatch);
        const bool responseReady =
            trainingNativeResponseReady(fighter, owner);
        qtraining::PrimaryRecoveryDisposition recoveryDisposition =
            qtraining::PrimaryRecoveryDisposition::Pending;
        TrainingNaturalRecoveryExpectation& expectation =
            g_trainingNaturalExpectation[owner];
        const TrainingNaturalTerminalPulse& pulse =
            g_trainingNaturalTerminal[owner];
        if (expectation.active) {
            const bool exactTerminal = pulse.valid
                && pulse.stepSerial == g_trainingStepSerial
                && pulse.fighter == expectation.fighter
                && pulse.character == expectation.character
                && pulse.resourceAction == expectation.resourceAction
                && pulse.dispatchAction == expectation.dispatchAction
                && pulse.actorToken == expectation.actorToken
                && pulse.actionEpoch == expectation.actionEpoch;
            if (expectation.fighter != fighter
                || expectation.actorToken != identity.actorToken) {
                recoveryDisposition =
                    qtraining::PrimaryRecoveryDisposition::ActorChanged;
                expectation = TrainingNaturalRecoveryExpectation{};
            } else if (exactTerminal) {
                recoveryDisposition =
                    qtraining::PrimaryRecoveryDisposition::Ready;
                expectation = TrainingNaturalRecoveryExpectation{};
            } else {
                const bool forcedTransition =
                    rd32(fighter + kTrainingFighterForcedActionOffset)
                        != UINT32_MAX;
                const bool actionChanged =
                    expectation.actionEpoch != identity.actionEpoch
                    || expectation.resourceAction != action
                    || expectation.dispatchAction != dispatch;
                if (forcedTransition) {
                    recoveryDisposition =
                        qtraining::PrimaryRecoveryDisposition::Interrupted;
                    expectation = TrainingNaturalRecoveryExpectation{};
                } else if (actionChanged) {
                    uint32_t sourceAction = expectation.resourceAction;
                    uint32_t sourceLocalFrame = expectation.lastLocalFrame;
                    const TrainingPreHandlerAction& pre =
                        g_trainingPreHandlerAction[owner];
                    if (pre.valid
                        && pre.stepSerial == g_trainingStepSerial
                        && pre.fighter == expectation.fighter
                        && pre.character == expectation.character
                        && pre.actorToken == expectation.actorToken
                        && pre.actionEpoch == expectation.actionEpoch) {
                        sourceAction = pre.resourceAction;
                        sourceLocalFrame = pre.localFrame;
                    }
                    const qtraining::TrainingNativeMoveTransition transition =
                        qtraining::trainingNativeMoveTransition(
                            expectation.character, sourceAction,
                            sourceLocalFrame,
                            trainingNativeContactKind(
                                expectation.contactKind),
                            action, identity.localFrame);
                    const bool throwTechCounterpart =
                        trainingExactThrowTechCounterpartTransition(
                            owner, expectation, action, dispatch);
                    const bool expectedThrowPath =
                        expectation.contactKind
                                == qtraining::CombatContactKind::Throw
                        && qtraining::combatThrowAttackerTupleIsHold(
                            expectation.resourceAction,
                            expectation.dispatchAction);
                    const bool currentThrowPath =
                        qtraining::combatThrowAttackerTupleIsHold(
                            action, dispatch);
                    if (throwTechCounterpart
                        || (transition.allowsConventionalFrameAdvantage()
                            && transition.effect
                                != qtraining::TrainingNativeMoveEffect::
                                    EndAction)) {
                        expectation.resourceAction = action;
                        expectation.dispatchAction = dispatch;
                        expectation.lastLocalFrame = identity.localFrame;
                        expectation.actionEpoch = identity.actionEpoch;
                        recoveryDisposition =
                            qtraining::PrimaryRecoveryDisposition::Continued;
                    } else if (expectedThrowPath && !currentThrowPath) {
                        recoveryDisposition =
                            qtraining::PrimaryRecoveryDisposition::Ready;
                        expectation = TrainingNaturalRecoveryExpectation{};
                    } else if (
                        transition.proof
                            == qtraining::TrainingNativeTransitionProof::
                                Unproven
                        && actionMeta.semantic
                            == qtraining::TrainingActionSemantic::Attack
                        && !trainingCombatExcludedFromConventionalAdvantage(
                            actionMeta)) {
                        expectation.resourceAction = action;
                        expectation.dispatchAction = dispatch;
                        expectation.lastLocalFrame = identity.localFrame;
                        expectation.actionEpoch = identity.actionEpoch;
                        recoveryDisposition =
                            qtraining::PrimaryRecoveryDisposition::Rechained;
                    } else {
                        const bool cancelled =
                            transition.proof
                                == qtraining::TrainingNativeTransitionProof::
                                    Unproven
                            && trainingCombatTransitionLooksCancelled(
                                actionMeta);
                        recoveryDisposition = cancelled
                            ? qtraining::PrimaryRecoveryDisposition::Cancelled
                            : qtraining::PrimaryRecoveryDisposition::
                                Interrupted;
                        expectation = TrainingNaturalRecoveryExpectation{};
                    }
                } else {
                    expectation.lastLocalFrame = identity.localFrame;
                }
            }
        }
        sample.owners[owner].action = action;
        sample.owners[owner].responseReady = responseReady;
        sample.owners[owner].naturalReady = recoveryDisposition
            == qtraining::PrimaryRecoveryDisposition::Ready;
        sample.owners[owner].primaryRecovery = recoveryDisposition;
        sample.owners[owner].actorToken = identity.actorToken;
        sample.owners[owner].actionEpoch = identity.actionEpoch;
        sample.owners[owner].defensiveLock =
            trainingCombatDefensiveLock(
                action, actionMeta, dispatchFamily);
        sample.owners[owner].semanticPathValid =
            trainingCombatSemanticPathKnown(
                action, actionMeta, dispatchFamily);
        sample.owners[owner].guardCancelReady =
            trainingNativeGuardCancelReady(fighter, owner);
        if (rd32(fighter + kFighterRoundLostOffset) != 0u)
            roundValid = false;
    }
    sample.readable = readable;
    sample.roundValid = roundValid;
    g_trainingCombatToolsReadoutValid = readable && roundValid ? 1u : 0u;

    if (readable) {
        const uint32_t dummyAction = sample.owners[1].action;
        if (g_trainingThrowTech.haveDummyAction
            && g_trainingThrowTech.previousDummyAction != 73u
            && dummyAction == 73u) {
            ++g_trainingThrowTech.resultObserved;
        }
        g_trainingThrowTech.previousDummyAction = dummyAction;
        g_trainingThrowTech.haveDummyAction = true;
    }

    uint8_t validMacroMask = 0u;
    for (uint32_t slot = 0u;
         slot < qtraining::kTrainingMacroSlotCount; ++slot) {
        if (g_trainingMacros.slot(slot).frameCount != 0u)
            validMacroMask |= static_cast<uint8_t>(1u << slot);
    }
    const uintptr_t dummy = trainingFighter(trainingDummySlot(), false);
    const uint8_t dummyCharacter = dummy
        ? rd8(dummy + kTrainingFighterCharacterOffset) : 0xFFu;
    const PostureClass dummyPosture = trainingReadPostureClass(dummy);
    const bool dummyPostureKnown = qtraining::postureClassGrounded(dummyPosture)
        || dummyPosture == PostureClass::Air;
    const bool dummyAirborne = dummyPosture == PostureClass::Air;
    uint64_t validMoveMask = 0u;
    if (dummyPostureKnown) {
        const uint16_t moveCount = qtraining::trainingMoveCount(dummyCharacter);
        for (uint16_t moveIndex = 1u;
             moveIndex <= moveCount
                 && moveIndex <= qtraining::kTrainingTriggerMaximumMoveIndex;
             ++moveIndex) {
            const qtraining::TrainingMoveView move =
                qtraining::trainingMove(dummyCharacter, moveIndex);
            if (qtraining::trainingMoveDirectTriggerEligible(move)
                && qtraining::trainingMoveContextAllowsPosture(
                    move, dummyAirborne)) {
                validMoveMask |= uint64_t{1u} << (moveIndex - 1u);
            }
        }
    }
    if (g_trainingWakeupReversal.active) {
        for (uint32_t index = 0u; index < g_trainingContactCount; ++index) {
            if (g_trainingContacts[index].victimOwner
                    == qtraining::kCombatDummyOwner) {
                trainingCancelWakeupReversal(
                    qtraining::TriggerRequestOutcome::DroppedSuperseded,
                    "new combat episode");
                break;
            }
        }
    }
    const bool wakeupReleaseAvailable =
        g_trainingWakeupReversal.active
        && !g_trainingWakeupReversal.released
        && g_trainingMacros.bufferedReversalAwaitingRelease()
        && sample.owners[qtraining::kCombatDummyOwner].responseReady;
    const bool wakeupReleasedEarly =
        g_trainingWakeupReversal.active
        && g_trainingWakeupReversal.released
        && g_trainingWakeupReversal.releasedEarly;
    const bool macroIdle = g_trainingMacros.state() == MacroState::Idle
        || wakeupReleaseAvailable || wakeupReleasedEarly;
    const qtraining::TrainingTriggerStepResult trigger =
        g_trainingTriggers.advance(
            sample, g_trainingContacts, g_trainingContactCount,
            macroIdle, validMacroMask, validMoveMask,
            wakeupReleaseAvailable || wakeupReleasedEarly,
            g_trainingWakeupReversal.active
                && !g_trainingWakeupReversal.released);
    if (trigger.requestMacroStart) {
        if (g_trainingWakeupReversal.active)
            trainingCancelWakeupReversal(
                qtraining::TriggerRequestOutcome::DroppedCollision,
                "recorded response won the wakeup boundary");
        if (!g_trainingMacros.requestTriggeredPlayback(
                trigger.macroSlotIndex)) {
            QLOG("[training-trigger] playback start raced with macro state "
                 "event=%u slot=%u step=%llu",
                 static_cast<unsigned>(trigger.event),
                 static_cast<unsigned>(trigger.macroSlotIndex + 1u),
                 static_cast<unsigned long long>(g_trainingStepSerial));
        } else {
            g_trainingGuardCancelResponsePlayback =
                trigger.event == qtraining::TrainingTriggerEvent::GuardCancel;
            trainingResponseWatchArm(
                static_cast<uint32_t>(trigger.event), 0u,
                static_cast<uint8_t>(trigger.macroSlotIndex + 1u),
                qtraining::kTrainingMoveUnknownAction, nullptr, 0u, sample,
                false);
        }
    }
    if (trigger.requestMoveStart) {
        const qtraining::TrainingMoveView move = qtraining::trainingMove(
            dummyCharacter, trigger.moveIndex);
        qtraining::TrainingMoveSequence sequence{};
        const bool sequenceCopied = qtraining::copyTrainingMoveSequence(
            dummyCharacter, trigger.moveIndex, sequence);
        const uint32_t lastComposed = g_trainingLastComposedInput[1];
        const uint8_t dummyFacingRaw =
            dummy ? rd8(dummy + kTrainingFighterFacingOffset) : 0u;
        const uint32_t firstPressed = sequenceCopied && sequence.stepCount > 1u
            ? qtraining::TrainingMacros::remapFacingRelative(
                  sequence.relativeMasks[1], 0u,
                  dummyFacingRaw <= 1u ? dummyFacingRaw : 0u)
            : 0u;
        const bool inputAlreadyNeutral = lastComposed == 0u;
        const bool leadingNeutralRedundant = inputAlreadyNeutral
            || ((lastComposed & qinput::kLogicalButtonMask) == 0u
                && (lastComposed & qinput::kLogicalDirectionMask)
                       != (firstPressed & qinput::kLogicalDirectionMask));
        const bool releaseBufferedWakeup = wakeupReleaseAvailable
            && trigger.event == g_trainingWakeupReversal.event
            && trigger.moveIndex == g_trainingWakeupReversal.moveIndex;
        const bool alreadyReleasedEarly = wakeupReleasedEarly
            && trigger.event == g_trainingWakeupReversal.event
            && trigger.moveIndex == g_trainingWakeupReversal.moveIndex;
        if (alreadyReleasedEarly) {
            g_trainingWakeupReversal.lastOutcome =
                qtraining::TriggerRequestOutcome::Started;
            QLOG("[training-wakeup] first-response activation already "
                 "released move=%u character=%u step=%llu",
                 static_cast<unsigned>(trigger.moveIndex),
                 static_cast<unsigned>(dummyCharacter),
                 static_cast<unsigned long long>(g_trainingStepSerial));
            trainingResponseWatchArm(
                static_cast<uint32_t>(trigger.event), trigger.moveIndex, 0u,
                move.actionId, move.input.data(), move.input.size(), sample,
                true);
        } else if (releaseBufferedWakeup) {
            if (!g_trainingMacros.releaseBufferedReversal()) {
                trainingCancelWakeupReversal(
                    qtraining::TriggerRequestOutcome::DroppedBusy,
                    "activation release was no longer aligned");
            } else {
                g_trainingWakeupReversal.released = true;
                g_trainingWakeupReversal.actorToken =
                    sample.owners[qtraining::kCombatDummyOwner].actorToken;
                g_trainingWakeupReversal.actionEpoch =
                    sample.owners[qtraining::kCombatDummyOwner].actionEpoch;
                g_trainingWakeupReversal.resourceAction =
                    rd32(dummy + kTrainingFighterActionStateOffset);
                g_trainingWakeupReversal.dispatchAction =
                    rd32(dummy + kTrainingFighterDispatchActionOffset);
                g_trainingWakeupReversal.phase =
                    qtraining::TrainingWakeupReversalPhase::Released;
                g_trainingWakeupReversal.lastOutcome =
                    qtraining::TriggerRequestOutcome::Started;
                QLOG("[training-wakeup] first-response activation released "
                     "move=%u character=%u step=%llu",
                     static_cast<unsigned>(trigger.moveIndex),
                     static_cast<unsigned>(dummyCharacter),
                     static_cast<unsigned long long>(g_trainingStepSerial));
                trainingResponseWatchArm(
                    static_cast<uint32_t>(trigger.event), trigger.moveIndex,
                    0u, move.actionId, move.input.data(), move.input.size(),
                    sample, true);
            }
        } else if (uint32_t commitIndexForSequence = 0u;
                   qtraining::trainingMoveTriggerEligible(move, trigger.event)
                   && qtraining::trainingMoveContextAllowsPosture(
                          move, dummyAirborne)
                   && sequenceCopied
                   && trainingWakeupSequenceCommitIndex(
                          sequence, commitIndexForSequence)
                   && trainingSeedMotionIntoRing(
                          dummy, sequence.relativeMasks, sequence.stepCount,
                          commitIndexForSequence)
                   && g_trainingMacros.requestTriggeredSequence(
                          sequence.relativeMasks + commitIndexForSequence,
                          sequence.stepCount - commitIndexForSequence,
                          true)) {
            g_trainingGuardCancelResponsePlayback =
                trigger.event == qtraining::TrainingTriggerEvent::GuardCancel;
            QLOG("[training-trigger] move start (ring-seeded) event=%u "
                 "move=%u input='%.*s' action=%u character=%u step=%llu",
                 static_cast<unsigned>(trigger.event),
                 static_cast<unsigned>(trigger.moveIndex),
                 static_cast<int>(move.input.size()), move.input.data(),
                 static_cast<unsigned>(move.actionId),
                 static_cast<unsigned>(dummyCharacter),
                 static_cast<unsigned long long>(g_trainingStepSerial));
            trainingResponseWatchArm(
                static_cast<uint32_t>(trigger.event), trigger.moveIndex, 0u,
                move.actionId, move.input.data(), move.input.size(), sample,
                true);
        } else if (!qtraining::trainingMoveTriggerEligible(move, trigger.event)
            || !qtraining::trainingMoveContextAllowsPosture(
                move, dummyAirborne)
            || !sequenceCopied
            || !g_trainingMacros.requestTriggeredSequence(
                sequence.relativeMasks, sequence.stepCount,
                leadingNeutralRedundant)) {
            QLOG("[training-trigger] move start refused after selection "
                 "event=%u move=%u character=%u posture=%u step=%llu",
                 static_cast<unsigned>(trigger.event),
                 static_cast<unsigned>(trigger.moveIndex),
                 static_cast<unsigned>(dummyCharacter),
                 static_cast<unsigned>(dummyPosture),
                 static_cast<unsigned long long>(g_trainingStepSerial));
        } else {
            g_trainingGuardCancelResponsePlayback =
                trigger.event == qtraining::TrainingTriggerEvent::GuardCancel;
            QLOG("[training-trigger] move start (INPUT fallback: %s, "
                 "closest-prime=%lu/%s) event=%u move=%u input='%.*s' "
                 "character=%u steps=%u neutral-skip=%u",
                 g_trainingEngineRequestRefusal,
                 static_cast<unsigned long>(g_trainingPrimeClosest),
                 g_trainingPrimeClosestReason,
                 static_cast<unsigned>(trigger.event),
                 static_cast<unsigned>(trigger.moveIndex),
                 static_cast<int>(move.input.size()), move.input.data(),
                 static_cast<unsigned>(dummyCharacter),
                 static_cast<unsigned>(sequence.stepCount),
                leadingNeutralRedundant ? 1u : 0u);
            trainingResponseWatchArm(
                static_cast<uint32_t>(trigger.event), trigger.moveIndex, 0u,
                move.actionId, move.input.data(), move.input.size(), sample,
                false);
        }
    }

    if (g_trainingWakeupReversal.active
        && !g_trainingWakeupReversal.released
        && sample.owners[qtraining::kCombatDummyOwner].responseReady) {
        const qtraining::TrainingTriggerStatus& triggerStatus =
            g_trainingTriggers.status();
        if (triggerStatus.lastEvent
                == qtraining::TrainingTriggerEvent::OnWakeup
            && triggerStatus.lastEventStep == g_trainingStepSerial
            && triggerStatus.lastOutcome
                != qtraining::TriggerRequestOutcome::Started) {
            trainingCancelWakeupReversal(
                triggerStatus.lastOutcome,
                "wakeup trigger did not select the activation");
        }
    }

    {
        const uint32_t dummyActionNow =
            sample.owners[qtraining::kCombatDummyOwner].action;
        const bool wakeupFamily =
            qtraining::combatActionIsNaturalWakeupPath(
                sample.owners[qtraining::kCombatDummyOwner].action);
        const bool guardFamily =
            qtraining::combatActionIsGuard(dummyActionNow)
            && dummyActionNow != 20u;
        const bool hitFamily =
            qtraining::combatActionIsGroundHit(dummyActionNow);
        const bool primeable = wakeupFamily || guardFamily || hitFamily;
        const qtraining::TrainingTriggerEvent primeEvent = wakeupFamily
            ? qtraining::TrainingTriggerEvent::OnWakeup
            : guardFamily
                ? qtraining::TrainingTriggerEvent::AfterBlock
                : qtraining::TrainingTriggerEvent::AfterHitstun;
        if (!primeable) {
            if (g_trainingWakeupEpisodeOpen
                && !g_trainingWakeupEpisodePrimed) {
                trainingReversalLeadDecay(
                    dummyCharacter,
                    static_cast<uint16_t>(
                        g_trainingTriggers
                            .config(qtraining::TrainingTriggerEvent::OnWakeup)
                            .moveIndex));
            }
            g_trainingWakeupEpisodeOpen = false;
            g_trainingWakeupEpisodePrimed = false;
            g_trainingWakeupReversal.episodeAttempted = false;
        } else {
            g_trainingWakeupEpisodeOpen = true;
        }
        if (!g_trainingWakeupReversal.active
            && !g_trainingWakeupReversal.episodeAttempted
            && g_trainingMacros.state() == MacroState::Idle
            && dummy != 0u
            && primeable) {
            const qtraining::TrainingTriggerConfig wakeupConfig =
                g_trainingTriggers.config(primeEvent);
            const qtraining::TrainingMoveView wakeupMove =
                qtraining::trainingMove(dummyCharacter, wakeupConfig.moveIndex);
            qtraining::TrainingMoveSequence wakeupSequence{};
            uint32_t commitIndex = 0u;
            uint32_t remaining = 0u;
            const bool eligible = wakeupConfig.macroSlot == 0u
                && wakeupConfig.moveIndex != 0u
                && wakeupConfig.delaySteps == 0u
                && wakeupConfig.chancePercent != 0u
                && g_trainingTriggers.prebufferApproved(
                    primeEvent, wakeupConfig.moveIndex)
                && !g_trainingRecoveryEmission.active
                && qtraining::trainingMoveTriggerEligible(
                    wakeupMove, primeEvent)
                && qtraining::trainingMoveContextAllowsPosture(
                    wakeupMove, false)
                && qtraining::copyTrainingMoveSequence(
                    dummyCharacter, wakeupConfig.moveIndex, wakeupSequence)
                && trainingWakeupSequenceCommitIndex(
                    wakeupSequence, commitIndex);
            if (eligible) {
                g_trainingWakeupReversal.phase =
                    qtraining::TrainingWakeupReversalPhase::
                        WaitingForSafeSuffix;
                g_trainingWakeupReversal.event = primeEvent;
                g_trainingWakeupReversal.moveIndex = wakeupConfig.moveIndex;
                g_trainingWakeupReversal.commitIndex = commitIndex;
                bool primeGateBased = false;
                const bool stunEvent =
                    qtraining::TrainingTriggerController::
                        prebufferRestoresGuard(primeEvent);
                const bool walked = stunEvent
                    ? trainingStunRemainingSteps(dummy, remaining)
                    : trainingStepsUntilDummyCanAct(
                          dummy, remaining, primeGateBased);
                static uint64_t declinedEpoch = 0u;
                static uint64_t declinedToken = 0u;

                const uint64_t epoch =
                    sample.owners[qtraining::kCombatDummyOwner].actionEpoch;
                const uint64_t token =
                    sample.owners[qtraining::kCombatDummyOwner].actorToken;
                const bool newEpisode =
                    epoch != declinedEpoch || token != declinedToken;
                if (newEpisode) {
                    g_trainingPrimeClosest = 0xFFFFFFFFu;
                    g_trainingPrimeClosestReason = "none";
                }
                const uint32_t approach = walked ? remaining : 0xFFFFFFFEu;
                if (approach <= g_trainingPrimeClosest) {
                    g_trainingPrimeClosest = approach;
                    g_trainingPrimeClosestReason = walked
                        ? "aligned-elsewhere"
                        : (stunEvent ? g_trainingStunWalkRefusal
                                     : g_trainingSuffixRefusal);
                }
                if ((!walked || remaining != commitIndex + 1u) && newEpisode) {
                    declinedEpoch = epoch;
                    declinedToken = token;
                    QLOG("[training-wakeup] not primed: move=%u '%.*s' "
                         "walk=%s locked-remaining=%u need=%u action=%u "
                         "step=%llu (response will use the late path)",
                         static_cast<unsigned>(wakeupConfig.moveIndex),
                         static_cast<int>(wakeupMove.input.size()),
                         wakeupMove.input.data(),
                         walked ? (stunEvent ? "stun" : (primeGateBased
                                       ? "gate" : "terminal"))
                                : (stunEvent ? g_trainingStunWalkRefusal
                                             : g_trainingSuffixRefusal),
                         static_cast<unsigned>(remaining),
                         static_cast<unsigned>(commitIndex + 1u),
                         static_cast<unsigned>(
                             sample.owners[qtraining::kCombatDummyOwner]
                                 .action),
                         static_cast<unsigned long long>(g_trainingStepSerial));
                }
                if (walked) {
                    g_trainingWakeupReversal.lockedStepsRemaining = remaining;
                    const int32_t lead = trainingReversalLead(
                        dummyCharacter, static_cast<uint16_t>(
                            wakeupConfig.moveIndex));
                    const int64_t basePrefix =
                        static_cast<int64_t>(commitIndex) + 1
                        + kTrainingChainLastTickIsActionable + lead;
                    const int64_t padWanted =
                        static_cast<int64_t>(remaining) - basePrefix;
                    uint32_t guardPad = 0u;
                    if (stunEvent && padWanted > 0) {
                        const uint32_t budget =
                            trainingReversalGuardPad(remaining, commitIndex);
                        guardPad = padWanted
                                       < static_cast<int64_t>(budget)
                                   ? static_cast<uint32_t>(padWanted)
                                   : budget;
                    }
                    qtraining::TrainingMoveSequence primedSequence{};
                    uint32_t primedCommitIndex = 0u;
                    const bool padded = trainingPadReversalSequenceWithGuard(
                        wakeupSequence, commitIndex, guardPad,
                        primedSequence, primedCommitIndex);
                    if (padded) commitIndex = primedCommitIndex;
                    const int64_t wantRemaining =
                        static_cast<int64_t>(commitIndex) + 1
                        + kTrainingChainLastTickIsActionable + lead;
                    g_trainingWakeupReversal.appliedLead = lead;
                    if (padded && wantRemaining > 0
                        && static_cast<int64_t>(remaining) == wantRemaining
                        && g_trainingMacros.requestBufferedReversalSequence(
                            primedSequence.relativeMasks,
                            primedSequence.stepCount)) {
                        g_trainingWakeupReversal.active = true;
                        g_trainingWakeupReversal.released = false;
                        g_trainingWakeupReversal.releasedEarly = false;
                        g_trainingWakeupReversal.fighter = dummy;
                        g_trainingWakeupReversal.character = dummyCharacter;
                        g_trainingWakeupReversal.generation =
                            g_trainingGeneration;
                        g_trainingWakeupReversal.actorToken =
                            sample.owners[qtraining::kCombatDummyOwner]
                                .actorToken;
                        g_trainingWakeupReversal.actionEpoch =
                            sample.owners[qtraining::kCombatDummyOwner]
                                .actionEpoch;
                        g_trainingWakeupReversal.resourceAction =
                            rd32(dummy + kTrainingFighterActionStateOffset);
                        g_trainingWakeupReversal.dispatchAction =
                            rd32(dummy + kTrainingFighterDispatchActionOffset);
                        g_trainingWakeupReversal.phase =
                            qtraining::TrainingWakeupReversalPhase::Priming;
                        g_trainingWakeupReversal.lastOutcome =
                            qtraining::TriggerRequestOutcome::None;
                        g_trainingWakeupEpisodePrimed = true;
                        g_trainingWakeupReversal.predictedStartStep =
                            g_trainingStepSerial + remaining
                            - kTrainingChainLastTickIsActionable;
                        trainingReversalTraceOpenWindow(
                            g_trainingWakeupReversal.predictedStartStep,
                            wakeupMove.actionId);
                        QLOG("[training-wakeup] prebuffer started event=%u "
                             "move=%u input='%.*s' character=%u commit=%u "
                             "step=%llu",
                             static_cast<unsigned>(primeEvent),
                             static_cast<unsigned>(wakeupConfig.moveIndex),
                             static_cast<int>(wakeupMove.input.size()),
                             wakeupMove.input.data(),
                             static_cast<unsigned>(dummyCharacter),
                             static_cast<unsigned>(commitIndex),
                             static_cast<unsigned long long>(
                                 g_trainingStepSerial));
                    }
                }
            } else if (wakeupFamily) {
                g_trainingWakeupReversal.phase =
                    qtraining::TrainingWakeupReversalPhase::WatchingKnockdown;
                g_trainingWakeupReversal.moveIndex = 0u;
                g_trainingWakeupReversal.commitIndex = 0u;
                g_trainingWakeupReversal.lockedStepsRemaining = 0u;
            }
        }
    }
    g_trainingFrameAdvantage.advance(
        sample, g_trainingContacts, g_trainingContactCount);

    trainingObserveReversalParse();
    trainingReversalTraceRow(
        g_trainingWakeupReversal.active
            ? (g_trainingWakeupReversal.released ? "released" : "priming")
            : "tail");
    trainingResponseWatchObserve(sample);

    trainingDrainTriggerJournal();
    trainingStateLogObserve(sample, g_trainingTriggers.diagnostics());
    trainingFrameTraceObserve();

    g_trainingContactCount = 0u;
    g_trainingNaturalTerminal[0] = TrainingNaturalTerminalPulse{};
    g_trainingNaturalTerminal[1] = TrainingNaturalTerminalPulse{};
    g_trainingPreHandlerAction[0] = TrainingPreHandlerAction{};
    g_trainingPreHandlerAction[1] = TrainingPreHandlerAction{};
}

static int trainingHoldFailureFrame(int nativeResult) {
    if (nativeResult != 0
        && rd32(kSceneStateAddr) == 2u
        && rd32(kMatchSubAddr) == 0u) {
        trainingRepaintHeldFrame();
    }
    g_trainingFinalInput[0] = g_trainingFinalInput[1] = 0u;
    return nativeResult == 0 ? 0 : 1;
}

static const char* trainingTriggerEventName(uint32_t event) {
    switch (static_cast<qtraining::TrainingTriggerEvent>(event)) {
        case qtraining::TrainingTriggerEvent::OnHit: return "OnHit";
        case qtraining::TrainingTriggerEvent::AfterBlock: return "AfterBlock";
        case qtraining::TrainingTriggerEvent::AfterHitstun:
            return "AfterHitstun";
        case qtraining::TrainingTriggerEvent::OnWakeup: return "OnWakeup";
        case qtraining::TrainingTriggerEvent::AfterAirRecovery:
            return "AfterAirRecovery";
        case qtraining::TrainingTriggerEvent::AfterGroundRecovery:
            return "AfterGroundRecovery";
        case qtraining::TrainingTriggerEvent::AfterThrowTech:
            return "AfterThrowTech";
        case qtraining::TrainingTriggerEvent::GuardCancel:
            return "GuardCancel";
        default: return "?";
    }
}

static const char* trainingSuppressionReasonName(uint32_t reason) {
    switch (static_cast<qtraining::TriggerSuppressionReason>(reason)) {
        case qtraining::TriggerSuppressionReason::None:
            return "responding";
        case qtraining::TriggerSuppressionReason::NoEventArmed:
            return "no-episode-armed";
        case qtraining::TriggerSuppressionReason::EventArmedNotConfigured:
            return "episode-armed-but-no-response-configured";
        case qtraining::TriggerSuppressionReason::WaitingForResponseReady:
            return "waiting-for-native-response-permission";
        case qtraining::TriggerSuppressionReason::WaitingForDelay:
            return "waiting-out-configured-delay";
        case qtraining::TriggerSuppressionReason::WaitingForGuardCancelWindow:
            return "guard-cancel-window-shut";
        case qtraining::TriggerSuppressionReason::WaitingForWakeupRelease:
            return "prebuffered-reversal-holding-commit";
        case qtraining::TriggerSuppressionReason::BlockedMacroBusy:
            return "playback-owner-busy";
        case qtraining::TriggerSuppressionReason::BlockedMoveUnavailable:
            return "selected-move-unavailable";
        case qtraining::TriggerSuppressionReason::BlockedEmptySlot:
            return "selected-recording-empty";
        case qtraining::TriggerSuppressionReason::ChanceSkipped:
            return "chance-declined";
        case qtraining::TriggerSuppressionReason::TimelineNotMeasurable:
            return "timeline-not-measurable";
        default: return "?";
    }
}

constexpr uint32_t kTrainingResponseWatchSteps = 30u;

static const char* trainingResponseActionVerdict(uint16_t expected,
                                                 uint32_t actual) {
    if (expected == qtraining::kTrainingMoveUnknownAction) return "unverified";
    return static_cast<uint32_t>(expected) == actual ? "MATCH" : "MISMATCH";
}

static void trainingResponseWatchCancel(const char* reason) {
    if (!g_trainingResponseWatch.active) return;
    QLOG("[training-response] %s %s move=%u '%s' abandoned: %s step=%llu",
         trainingTriggerEventName(g_trainingResponseWatch.event),
         g_trainingResponseWatch.macroSlot != 0u ? "recording" : "command",
         static_cast<unsigned>(g_trainingResponseWatch.macroSlot != 0u
                                   ? g_trainingResponseWatch.macroSlot
                                   : g_trainingResponseWatch.moveIndex),
         g_trainingResponseWatch.input,
         reason ? reason : "unknown",
         static_cast<unsigned long long>(g_trainingStepSerial));
    g_trainingResponseWatch = TrainingResponseWatch{};
}

static void trainingResponseWatchArm(uint32_t event, uint16_t moveIndex,
                                     uint8_t macroSlot,
                                     uint16_t expectedAction,
                                     const char* input, std::size_t inputLen,
                                     const qtraining::CombatFrameSample& sample,
                                     bool prebuffered) {
    if (g_trainingResponseWatch.active)
        trainingResponseWatchCancel("superseded by a new response");

    TrainingResponseWatch& watch = g_trainingResponseWatch;
    watch = TrainingResponseWatch{};
    watch.active = true;
    watch.event = event;
    watch.moveIndex = moveIndex;
    watch.macroSlot = macroSlot;
    watch.expectedAction = expectedAction;
    if (input != nullptr && inputLen != 0u) {
        const std::size_t copy =
            inputLen < sizeof(watch.input) - 1u ? inputLen
                                                : sizeof(watch.input) - 1u;
        std::memcpy(watch.input, input, copy);
        watch.input[copy] = '\0';
    } else {
        std::snprintf(watch.input, sizeof(watch.input), "-");
    }
    watch.startStep = g_trainingStepSerial;
    watch.startAction = sample.owners[qtraining::kCombatDummyOwner].action;
    watch.actorToken = sample.owners[qtraining::kCombatDummyOwner].actorToken;
    watch.generation = g_trainingGeneration;
    watch.prebuffered = prebuffered;
    watch.guardYieldBase = g_trainingGuardYieldSteps;
}

static void trainingResponseWatchObserve(
        const qtraining::CombatFrameSample& sample) {
    TrainingResponseWatch& watch = g_trainingResponseWatch;
    if (!watch.active) return;

    if (watch.generation != g_trainingGeneration || !sample.readable) {
        trainingResponseWatchCancel("timeline replaced");
        return;
    }
    const qtraining::CombatOwnerSample& dummy =
        sample.owners[qtraining::kCombatDummyOwner];
    if (dummy.actorToken != 0u && watch.actorToken != 0u
        && dummy.actorToken != watch.actorToken) {
        trainingResponseWatchCancel("dummy actor replaced");
        return;
    }

    watch.guardYieldSteps =
        g_trainingGuardYieldSteps - watch.guardYieldBase;

    const uint64_t elapsed = g_trainingStepSerial >= watch.startStep
        ? g_trainingStepSerial - watch.startStep : 0u;

    const bool transientMovement =
        watch.expectedAction != qtraining::kTrainingMoveUnknownAction
        && (dummy.action == 0u || dummy.action == 26u
            || dummy.action == 27u || dummy.action == 28u)
        && dummy.action != watch.expectedAction;
    if (dummy.action != watch.startAction && !transientMovement) {
        QLOG("[training-response] %s %s=%u '%s' -> action=%u expected=%d %s "
             "latency=%llu prebuffered=%u guard-yielded=%u step=%llu",
             trainingTriggerEventName(watch.event),
             watch.macroSlot != 0u ? "recording" : "move",
             static_cast<unsigned>(watch.macroSlot != 0u ? watch.macroSlot
                                                        : watch.moveIndex),
             watch.input,
             static_cast<unsigned>(dummy.action),
             watch.expectedAction == qtraining::kTrainingMoveUnknownAction
                 ? -1 : static_cast<int>(watch.expectedAction),
             trainingResponseActionVerdict(watch.expectedAction, dummy.action),
             static_cast<unsigned long long>(elapsed),
             watch.prebuffered ? 1u : 0u,
             static_cast<unsigned>(watch.guardYieldSteps),
             static_cast<unsigned long long>(g_trainingStepSerial));
        if (watch.prebuffered
            && g_trainingWakeupReversal.predictedStartStep != 0u
            && g_trainingWakeupReversal.moveIndex == watch.moveIndex
            && dummy.action == watch.expectedAction) {
            const int64_t delta =
                static_cast<int64_t>(g_trainingStepSerial)
                - static_cast<int64_t>(
                      g_trainingWakeupReversal.predictedStartStep);
            if (delta > -static_cast<int64_t>(kTrainingReversalLeadMax)
                && delta < static_cast<int64_t>(kTrainingReversalLeadMax)) {
                trainingReversalLeadObserve(
                    g_trainingWakeupReversal.character, watch.moveIndex,
                    static_cast<int32_t>(delta));
            }
        }
        g_trainingWakeupReversal.predictedStartStep = 0u;
        trainingReversalTraceCloseWindow();
        watch = TrainingResponseWatch{};
        return;
    }

    if (elapsed < kTrainingResponseWatchSteps) return;

    const uintptr_t fighter = trainingFighter(trainingDummySlot(), false);
    const unsigned permission = fighter != 0u
        ? trainingCombinedAuthoredPermission(fighter, 1u) : 0u;
    QLOG("[training-response] %s %s=%u '%s' -> NO ACTION in %u steps "
         "(still action=%u permission=%02X responseReady=%u reason=%s) "
         "prebuffered=%u guard-yielded=%u step=%llu",
         trainingTriggerEventName(watch.event),
         watch.macroSlot != 0u ? "recording" : "move",
         static_cast<unsigned>(watch.macroSlot != 0u ? watch.macroSlot
                                                     : watch.moveIndex),
         watch.input,
         static_cast<unsigned>(kTrainingResponseWatchSteps),
         static_cast<unsigned>(dummy.action),
         permission,
         dummy.responseReady ? 1u : 0u,
         trainingSuppressionReasonName(
             static_cast<uint32_t>(g_trainingTriggers.diagnostics().reason)),
         watch.prebuffered ? 1u : 0u,
         static_cast<unsigned>(watch.guardYieldSteps),
         static_cast<unsigned long long>(g_trainingStepSerial));
    watch = TrainingResponseWatch{};
}

struct TrainingStateChangeLogFile {
    std::FILE* file = nullptr;
    bool requested = true;
    bool failed = false;
    uint32_t records = 0u;

    bool haveSnapshot = false;
    uint32_t generation = 0u;
    uint32_t action[2] = {};
    uint32_t dispatch[2] = {};
    bool responseReady[2] = {};
    uint8_t permission[2] = {};
    bool terminalWindow[2] = {};
    uint8_t armedMask = 0u;
    uint8_t reason = 0xFFu;
    bool havePending = false;
    uint8_t pendingEvent = 0u;
    uint8_t pendingSlot = 0u;
    uint16_t pendingMove = 0u;
    uint32_t wakeupPhase = 0u;
    uint32_t guardMode = 0xFFFFFFFFu;
    uint32_t guardDirections = 0u;
    bool guardValid = false;
    bool guarding = false;
    bool guardEvidenceMissing = false;
    bool guardUnguardablePosture = false;
    bool guardMixupUnblockable = false;
    bool guardForcedStand = false;
    bool guardCaptureAvoid = false;
    uint32_t guardYieldSteps = 0u;
    uint32_t guardTurnedAtRead = 0u;
    uint8_t guardLegalMask = 0u;
    uint32_t guardEmitted = 0u;
    uint32_t guardOwnedAxes = 0u;
};

static TrainingStateChangeLogFile g_trainingStateLog{};

static void trainingStateLogClose() {
    if (g_trainingStateLog.file != nullptr) {
        std::fclose(g_trainingStateLog.file);
        g_trainingStateLog.file = nullptr;
    }
    g_trainingStateLog.haveSnapshot = false;
}

static void trainingStateLogOpen() {
    if (g_trainingStateLog.file != nullptr || g_trainingStateLog.failed) return;
    if (g_sessionDir[0] == '\0') {
        g_trainingStateLog.failed = true;
        return;
    }
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\training_state.log", g_sessionDir);
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (needed <= 0) {
        g_trainingStateLog.failed = true;
        return;
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            &wide[0], needed) != needed) {
        g_trainingStateLog.failed = true;
        return;
    }
    g_trainingStateLog.file = _wfopen(wide.c_str(), L"a");
    if (g_trainingStateLog.file == nullptr) {
        g_trainingStateLog.failed = true;
        QLOG("[training-statelog] could not open %s", path);
        return;
    }
    g_trainingStateLog.haveSnapshot = false;
    std::fprintf(g_trainingStateLog.file,
                 "# training state-change log; one line per CHANGE.\n"
                 "# fields: step | side 48:from>to 84:from>to rdy perm term"
                 " | armed episodes | queued response | why nothing is "
                 "pressed\n"
                 "# perm = effective authored +158: 0x01 basic action open,"
                 " 0x0C reaction lock. QOH needs 0x01 SET and, in a reaction"
                 " state, 0x0C CLEAR.\n");
    std::fflush(g_trainingStateLog.file);
    QLOG("[training-statelog] recording to %s", path);
}

static void trainingStateLogSetEnabled(bool enabled) {
    g_trainingGuardTraceEnabled = enabled;
    if (g_trainingStateLog.requested == enabled) return;
    g_trainingStateLog.requested = enabled;
    if (enabled) {
        g_trainingStateLog.failed = false;
        trainingStateLogOpen();
    } else {
        trainingStateLogClose();
    }
}

static bool trainingStateLogRecording() {
    return g_trainingStateLog.file != nullptr;
}

static bool trainingStateLogFailed() { return g_trainingStateLog.failed; }

static uint32_t trainingStateLogRecordCount() {
    return g_trainingStateLog.records;
}

static void trainingStateLogAppendEventNames(
        uint8_t mask, char* out, size_t size) {
    out[0] = '\0';
    if (mask == 0u) {
        std::snprintf(out, size, "-");
        return;
    }
    size_t used = 0u;
    for (uint32_t event = 0u;
         event < qtraining::kTrainingTriggerEventCount; ++event) {
        if ((mask & static_cast<uint8_t>(1u << event)) == 0u) continue;
        const char* name = trainingTriggerEventName(event);
        const int written = std::snprintf(
            out + used, used < size ? size - used : 0u,
            used == 0u ? "%s" : ",%s", name);
        if (written <= 0) break;
        used += static_cast<size_t>(written);
        if (used >= size) break;
    }
}

static void trainingStateLogObserve(
        const qtraining::CombatFrameSample& sample,
        const qtraining::TrainingTriggerDiagnostics& diagnostics) {
    if (g_trainingStateLog.file == nullptr && g_trainingStateLog.requested
        && !g_trainingStateLog.failed) {
        trainingStateLogOpen();
        g_trainingGuardTraceEnabled = g_trainingStateLog.file != nullptr;
    }
    if (g_trainingStateLog.file == nullptr) return;

    uint32_t dispatch[2] = {};
    uint8_t permission[2] = {};
    bool terminalWindow[2] = {};
    for (uint8_t owner = 0u; owner < 2u; ++owner) {
        const uintptr_t fighter = trainingFighter(owner, false);
        if (fighter == 0u || !trainingCombatFrameReadable(fighter)) continue;
        dispatch[owner] = rd32(fighter + kTrainingFighterDispatchActionOffset);
        permission[owner] = trainingCombinedAuthoredPermission(fighter, owner);
        terminalWindow[owner] = trainingNativeTerminalCommandWindow(fighter);
    }

    const bool generationChanged =
        g_trainingStateLog.haveSnapshot
        && g_trainingStateLog.generation != sample.trainingGeneration;
    if (generationChanged) {
        std::fprintf(g_trainingStateLog.file,
                     "-- training generation %lu\n",
                     static_cast<unsigned long>(sample.trainingGeneration));
        g_trainingStateLog.haveSnapshot = false;
    }

    const qtraining::GuardDecision& guard = g_trainingLastGuardDecision;
    const uint32_t guardMode = static_cast<uint32_t>(g_trainingGuardMode);
    const uint8_t reason = static_cast<uint8_t>(diagnostics.reason);
    bool changed = !g_trainingStateLog.haveSnapshot
        || g_trainingStateLog.guardMode != guardMode
        || g_trainingStateLog.guardDirections != guard.logicalDirections
        || g_trainingStateLog.guardValid != guard.valid
        || g_trainingStateLog.guarding != guard.guarding
        || g_trainingStateLog.guardEvidenceMissing
               != guard.recordEvidenceMissing
        || g_trainingStateLog.guardUnguardablePosture
               != guard.defenderPostureUnguardable
        || g_trainingStateLog.guardMixupUnblockable
               != guard.recordMixupUnblockable
        || g_trainingStateLog.guardForcedStand != guard.recordForcedStandGuard
        || g_trainingStateLog.guardLegalMask != guard.legalPostureMask
        || g_trainingStateLog.guardEmitted
               != g_trainingLastEmittedDummyDirections
        || g_trainingStateLog.guardOwnedAxes != g_trainingLastEmittedOwnedAxes
        || g_trainingStateLog.guardCaptureAvoid != guard.captureAvoidanceApplied
        || g_trainingStateLog.guardYieldSteps != g_trainingGuardYieldSteps
        || g_trainingStateLog.guardTurnedAtRead != g_trainingGuardTurnedAtRead
        || g_trainingStateLog.armedMask != diagnostics.armedEventMask
        || g_trainingStateLog.reason != reason
        || g_trainingStateLog.havePending != diagnostics.havePending
        || g_trainingStateLog.wakeupPhase
               != static_cast<uint32_t>(g_trainingWakeupReversal.phase);
    if (diagnostics.havePending) {
        changed = changed
            || g_trainingStateLog.pendingEvent
                   != static_cast<uint8_t>(diagnostics.pendingEvent)
            || g_trainingStateLog.pendingSlot != diagnostics.pendingMacroSlot
            || g_trainingStateLog.pendingMove != diagnostics.pendingMoveIndex;
    }
    for (uint8_t owner = 0u; owner < 2u; ++owner) {
        changed = changed
            || g_trainingStateLog.action[owner] != sample.owners[owner].action
            || g_trainingStateLog.dispatch[owner] != dispatch[owner]
            || g_trainingStateLog.permission[owner] != permission[owner]
            || g_trainingStateLog.terminalWindow[owner]
                   != terminalWindow[owner]
            || g_trainingStateLog.responseReady[owner]
                   != sample.owners[owner].responseReady;
    }
    if (!changed) return;

    char armed[192];
    trainingStateLogAppendEventNames(
        diagnostics.armedEventMask, armed, sizeof(armed));

    char queued[128];
    if (!diagnostics.havePending) {
        std::snprintf(queued, sizeof(queued), "-");
    } else if (diagnostics.pendingMacroSlot != 0u) {
        std::snprintf(queued, sizeof(queued), "%s recording=%u due=%llu",
                      trainingTriggerEventName(
                          static_cast<uint32_t>(diagnostics.pendingEvent)),
                      static_cast<unsigned>(diagnostics.pendingMacroSlot),
                      static_cast<unsigned long long>(
                          diagnostics.pendingDueStep));
    } else {
        std::snprintf(queued, sizeof(queued), "%s move=%u due=%llu",
                      trainingTriggerEventName(
                          static_cast<uint32_t>(diagnostics.pendingEvent)),
                      static_cast<unsigned>(diagnostics.pendingMoveIndex),
                      static_cast<unsigned long long>(
                          diagnostics.pendingDueStep));
    }

    const bool first = !g_trainingStateLog.haveSnapshot;
    char sides[2][128];
    for (uint8_t owner = 0u; owner < 2u; ++owner) {
        const uint32_t previousAction = first
            ? sample.owners[owner].action : g_trainingStateLog.action[owner];
        const uint32_t previousDispatch = first
            ? dispatch[owner] : g_trainingStateLog.dispatch[owner];
        std::snprintf(sides[owner], sizeof(sides[owner]),
                      "P%u 48:%lu>%lu 84:%lu>%lu rdy=%u perm=%02X term=%u",
                      static_cast<unsigned>(owner + 1u),
                      static_cast<unsigned long>(previousAction),
                      static_cast<unsigned long>(sample.owners[owner].action),
                      static_cast<unsigned long>(previousDispatch),
                      static_cast<unsigned long>(dispatch[owner]),
                      sample.owners[owner].responseReady ? 1u : 0u,
                      static_cast<unsigned>(permission[owner]),
                      terminalWindow[owner] ? 1u : 0u);
    }

    char guardText[176];
    std::snprintf(guardText, sizeof(guardText),
                  "guard=%lu%s dir=%05lX emit=%05lX axes=%05lX legal=%02X "
                  "rec=%lu/%lu yield=%lu turned=%lu%s%s%s%s%s",
                  static_cast<unsigned long>(guardMode),
                  guard.guarding ? "" : " NOT-GUARDING",
                  static_cast<unsigned long>(guard.logicalDirections),
                  static_cast<unsigned long>(
                      g_trainingLastEmittedDummyDirections),
                  static_cast<unsigned long>(g_trainingLastEmittedOwnedAxes),
                  static_cast<unsigned>(guard.legalPostureMask),
                  static_cast<unsigned long>(guard.activeRecordCount),
                  static_cast<unsigned long>(guard.captureRecordCount),
                  static_cast<unsigned long>(g_trainingGuardYieldSteps),
                  static_cast<unsigned long>(g_trainingGuardTurnedAtRead),
                  guard.valid ? "" : " invalid",
                  guard.recordEvidenceMissing ? " no-record-evidence" : "",
                  guard.defenderPostureUnguardable ? " posture-unguardable" : "",
                  guard.recordMixupUnblockable ? " MIXUP-UNBLOCKABLE" : "",
                  guard.recordForcedStandGuard ? " forced-stand" : "");
    std::fprintf(g_trainingStateLog.file,
                 "step=%llu %s | %s | armed=%s | queued=%s | wakeup=%lu "
                 "| %s | why=%s\n",
                 static_cast<unsigned long long>(sample.stepSerial),
                 sides[0], sides[1], armed, queued,
                 static_cast<unsigned long>(g_trainingWakeupReversal.phase),
                 guardText,
                 trainingSuppressionReasonName(reason));
    ++g_trainingStateLog.records;
    std::fflush(g_trainingStateLog.file);

    g_trainingStateLog.haveSnapshot = true;
    g_trainingStateLog.guardMode = guardMode;
    g_trainingStateLog.guardDirections = guard.logicalDirections;
    g_trainingStateLog.guardValid = guard.valid;
    g_trainingStateLog.guarding = guard.guarding;
    g_trainingStateLog.guardEvidenceMissing = guard.recordEvidenceMissing;
    g_trainingStateLog.guardUnguardablePosture =
        guard.defenderPostureUnguardable;
    g_trainingStateLog.guardMixupUnblockable = guard.recordMixupUnblockable;
    g_trainingStateLog.guardForcedStand = guard.recordForcedStandGuard;
    g_trainingStateLog.guardCaptureAvoid = guard.captureAvoidanceApplied;
    g_trainingStateLog.guardYieldSteps = g_trainingGuardYieldSteps;
    g_trainingStateLog.guardTurnedAtRead = g_trainingGuardTurnedAtRead;
    g_trainingStateLog.guardLegalMask = guard.legalPostureMask;
    g_trainingStateLog.guardEmitted = g_trainingLastEmittedDummyDirections;
    g_trainingStateLog.guardOwnedAxes = g_trainingLastEmittedOwnedAxes;
    g_trainingStateLog.generation = sample.trainingGeneration;
    g_trainingStateLog.armedMask = diagnostics.armedEventMask;
    g_trainingStateLog.reason = reason;
    g_trainingStateLog.havePending = diagnostics.havePending;
    g_trainingStateLog.pendingEvent =
        static_cast<uint8_t>(diagnostics.pendingEvent);
    g_trainingStateLog.pendingSlot = diagnostics.pendingMacroSlot;
    g_trainingStateLog.pendingMove = diagnostics.pendingMoveIndex;
    g_trainingStateLog.wakeupPhase =
        static_cast<uint32_t>(g_trainingWakeupReversal.phase);
    for (uint8_t owner = 0u; owner < 2u; ++owner) {
        g_trainingStateLog.action[owner] = sample.owners[owner].action;
        g_trainingStateLog.dispatch[owner] = dispatch[owner];
        g_trainingStateLog.responseReady[owner] =
            sample.owners[owner].responseReady;
        g_trainingStateLog.permission[owner] = permission[owner];
        g_trainingStateLog.terminalWindow[owner] = terminalWindow[owner];
    }
}

// cursor 65534 (0xFFFE) is the authored terminal sentinel, not a frame index.
constexpr uint32_t kTrainingFrameTraceRowCap = 100000u;

struct TrainingFrameTraceFile {
    std::FILE* file = nullptr;
    bool requested = false;
    bool failed = false;
    bool capped = false;
    uint32_t rows = 0u;
    uint64_t tick = 0u;
};

static TrainingFrameTraceFile g_trainingFrameTrace{};

static void trainingFrameTraceClose() {
    if (g_trainingFrameTrace.file != nullptr) {
        std::fclose(g_trainingFrameTrace.file);
        g_trainingFrameTrace.file = nullptr;
    }
}

static void trainingFrameTraceOpen() {
    if (g_trainingFrameTrace.file != nullptr
        || g_trainingFrameTrace.failed) {
        return;
    }
    if (g_sessionDir[0] == '\0') {
        g_trainingFrameTrace.failed = true;
        return;
    }
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\frametrace.csv", g_sessionDir);
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, nullptr, 0);
    if (needed <= 0) {
        g_trainingFrameTrace.failed = true;
        return;
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            &wide[0], needed) != needed) {
        g_trainingFrameTrace.failed = true;
        return;
    }
    g_trainingFrameTrace.file = _wfopen(wide.c_str(), L"a");
    if (g_trainingFrameTrace.file == nullptr) {
        g_trainingFrameTrace.failed = true;
        QLOG("[training-frametrace] could not open %s", path);
        return;
    }
    std::fprintf(g_trainingFrameTrace.file,
                 "tick,player,action,cursor,hold,localFrame,duration,"
                 "activity,sprite,guardScan,guardValid,guardSrc,"
                 "guardRecords,guardLegal\n");
    std::fflush(g_trainingFrameTrace.file);
    QLOG("[training-frametrace] recording to %s", path);
}

static void trainingFrameTraceSetEnabled(bool enabled) {
    if (g_trainingFrameTrace.requested == enabled) return;
    g_trainingFrameTrace.requested = enabled;
    if (enabled) {
        g_trainingFrameTrace.failed = false;
        g_trainingFrameTrace.capped = false;
        g_trainingFrameTrace.rows = 0u;
        trainingFrameTraceOpen();
    } else {
        trainingFrameTraceClose();
    }
}

static bool trainingFrameTraceRecording() {
    return g_trainingFrameTrace.file != nullptr
        && !g_trainingFrameTrace.capped;
}

static bool trainingFrameTraceFailed() {
    return g_trainingFrameTrace.failed;
}

static bool trainingFrameTraceCapped() {
    return g_trainingFrameTrace.capped;
}

static uint32_t trainingFrameTraceRowCount() {
    return g_trainingFrameTrace.rows;
}

static void trainingFrameTraceObserve() {
    if (g_trainingFrameTrace.file == nullptr
        || g_trainingFrameTrace.capped) {
        return;
    }
    const uint64_t tick = g_trainingFrameTrace.tick++;
    for (uint8_t owner = 0u; owner < 2u; ++owner) {
        if (g_trainingFrameTrace.rows >= kTrainingFrameTraceRowCap) {
            g_trainingFrameTrace.capped = true;
            std::fflush(g_trainingFrameTrace.file);
            QLOG("[training-frametrace] row cap %u reached; capture stopped",
                 kTrainingFrameTraceRowCap);
            return;
        }
        const uintptr_t fighter = trainingFighter(owner, false);
        if (fighter == 0u || !trainingCombatFrameReadable(fighter)) continue;
        const uint32_t action =
            rd32(fighter + kTrainingFighterActionStateOffset);
        const uint32_t cursor =
            rd32(fighter + kTrainingFighterActionFrameOffset);
        const uint32_t hold =
            rd32(fighter + kTrainingFighterActionLockEndOffset);
        const uint32_t localFrame =
            rd32(fighter + kTrainingFighterAnimFrameOffset);
        const uint32_t framePtr =
            rd32(fighter + kTrainingFighterFrameRecordPtrOffset);
        uint32_t duration = 0u;
        uint32_t activity = 0u;
        uint32_t sprite = 0u;
        if (framePtr != 0u
            && rangeReadable(static_cast<uintptr_t>(framePtr),
                             kTrainingChrFrameRecordSize)) {
            duration = rd32(framePtr + kTrainingChrFrameDurationOffset);
            activity = rd16(framePtr + kTrainingChrFrameActivityOffset);
            sprite = rd16(framePtr + kTrainingChrFrameSpriteOffset);
        }
        std::fprintf(g_trainingFrameTrace.file,
                     "%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
                     static_cast<unsigned long long>(tick),
                     static_cast<unsigned>(owner),
                     static_cast<unsigned>(action),
                     static_cast<unsigned>(cursor),
                     static_cast<unsigned>(hold),
                     static_cast<unsigned>(localFrame),
                     static_cast<unsigned>(duration),
                     static_cast<unsigned>(activity),
                     static_cast<unsigned>(sprite),
                     g_trainingGuardEvidence.scanned ? 1u : 0u,
                     g_trainingGuardEvidence.valid ? 1u : 0u,
                     g_trainingGuardEvidence.held
                         ? 3u
                         : (g_trainingGuardEvidence.recordCount == 0u
                                ? 0u
                                : (g_trainingGuardEvidence.fromLookahead
                                       ? 2u : 1u)),
                     static_cast<unsigned>(
                         g_trainingGuardEvidence.recordCount),
                     static_cast<unsigned>(
                         g_trainingGuardEvidence.legalPostureMask));
        ++g_trainingFrameTrace.rows;
    }
    std::fflush(g_trainingFrameTrace.file);
}

static int trainingAdmitAfterNativeGate(int nativeResult) {
    g_trainingLastNativeReturn =
        static_cast<uint32_t>(nativeResult);
    if (!g_trainingLaunchRequested)
        return nativeResult;
    if (g_trainingMutationFailed) {
        const bool returning =
            trainingDrainFailedRecoveryCommands();
        g_trainingLastAdmission =
            StepAdmissionResult::InvariantFailure;
        return trainingHoldFailureFrame(
            nativeResult == 0 || returning ? 0 : 1);
    }
    if (nativeResult == 0) {
        g_trainingLastAdmission =
            StepAdmissionResult::NativeReturnedZero;
        return nativeResult;
    }
    if (nativeResult == 1) {
        g_trainingLastAdmission =
            StepAdmissionResult::NativeReturnedOne;
        return nativeResult;
    }
    if (nativeResult != 2)
        return nativeResult;
    if (rd32(kSceneStateAddr) != 2u) {
        g_trainingLastAdmission =
            StepAdmissionResult::WrongScene;
        return nativeResult;
    }
    if (rd32(kMatchSubAddr) != 0u) {
        g_trainingLastAdmission =
            StepAdmissionResult::WrongMatchSubstate;
        return nativeResult;
    }
    if (!trainingQualifiedGameplayGate()) {
        g_trainingLastAdmission =
            rd8(kTrainingFreezeAddr) != 0u
                ? StepAdmissionResult::Frozen
                : (rd8(kTrainingPauseAddr) != 0u
                    ? StepAdmissionResult::Paused
                    : StepAdmissionResult::WaitCountBlocked);
        return nativeResult;
    }

    if (!trainingApplyConfigOverlay()
        || !trainingRepairHumanOwnership()) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Training config or human ownership invariant failed");
        return trainingHoldFailureFrame(1);
    }
    if (!g_trainingBaseline.valid
        && !trainingCaptureBaseline()) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Training round did not enter a clean no-result baseline");
        return trainingHoldFailureFrame(1);
    }
    if (!trainingInvariantsHold()) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Training pre-step result invariant failed");
        return trainingHoldFailureFrame(1);
    }
    if (!trainingCaptureRoundCharacterState()) {
        trainingFailMutation(
            TrainingDisableReason::SnapshotFailure,
            "Training round idle character capture failed");
        return trainingHoldFailureFrame(1);
    }
    const bool characterCapturePending =
        !g_trainingRoundCharacterState.valid;
    ++g_trainingPollSerial;
    qtraining::TrainingActionPoll poll{};
    poll.lifecycleAvailable = true;
    const bool menuCaptured =
        InterlockedCompareExchange(&g_trainingMenuCaptured, 0, 0) != 0;
    g_trainingHold.setMenuCaptured(menuCaptured);
    poll.menuCaptured = menuCaptured;
    poll.operationNeutral =
        g_trainingOperationNeutral || characterCapturePending;
    poll.relevantPhysicalNeutralBeforeSocd =
        g_trainingPhysical.p1NeutralBeforeSocd;
    poll.heldTrainingActions = g_trainingPhysical.heldActions;
    poll.p1PhysicalMask = g_trainingPhysical.p1Mask;
    const qtraining::TrainingActionResult actions =
        g_trainingInput.pollActions(g_trainingPollSerial, poll);
    if (!actions.valid) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Training hotkey poll rejected a non-canonical snapshot");
        return trainingHoldFailureFrame(1);
    }
    if ((actions.heldLevelActions
         & qtraining::trainingActionBit(
             qtraining::TrainingAction::ChargeExFraction)) == 0u) {
        g_trainingMeterChargeCounter = 0u;
    }

    const bool actionsMayMutate =
        (actions.actionEdges
         & qtraining::trainingImageMutatingActionMask()) != 0u;
    bool recoverySnapshotReady = false;
    if (actionsMayMutate
        && !trainingCaptureSnapshot(g_trainingScratch)) {
        g_trainingDisableReason =
            TrainingDisableReason::SnapshotFailure;
        trainingFailMutation(
            TrainingDisableReason::SnapshotFailure,
            "Mandatory pre-step training snapshot failed");
        return trainingHoldFailureFrame(1);
    }
    recoverySnapshotReady = actionsMayMutate;

    g_trainingContactPolicyLatched = g_trainingContactPolicyRequested;

    g_trainingOperationNeutral = characterCapturePending;
    bool imageTransactionConsumed = false;
    if (!trainingDrainCommands(
            recoverySnapshotReady, imageTransactionConsumed)) {
        g_trainingLastAdmission =
            StepAdmissionResult::InvariantFailure;
        return trainingHoldFailureFrame(1);
    }
    if (g_trainingReturningToSelect) {
        g_trainingScratch.valid = false;
        g_trainingLastAdmission =
            StepAdmissionResult::NativeReturnedZero;
        return 0;
    }
    if (g_trainingMutationFailed) {
        g_trainingLastAdmission =
            StepAdmissionResult::InvariantFailure;
        return trainingHoldFailureFrame(1);
    }

    if (!trainingApplyActionEdges(actions, imageTransactionConsumed)
        || g_trainingMutationFailed) {
        g_trainingLastAdmission =
            StepAdmissionResult::InvariantFailure;
        return trainingHoldFailureFrame(1);
    }
    if (g_trainingReturningToSelect) {
        g_trainingScratch.valid = false;
        g_trainingLastAdmission =
            StepAdmissionResult::NativeReturnedZero;
        return 0;
    }
    if (!trainingConsumeRefillRequests()
        || g_trainingMutationFailed) {
        g_trainingLastAdmission =
            StepAdmissionResult::InvariantFailure;
        return trainingHoldFailureFrame(1);
    }
    if (!trainingConsumeResourcePolicies()
        || g_trainingMutationFailed) {
        g_trainingLastAdmission =
            StepAdmissionResult::InvariantFailure;
        return trainingHoldFailureFrame(1);
    }
    if (!trainingApplyConfigOverlay()
        || !trainingRepairHumanOwnership()
        || !trainingInvariantsHold()) {
        (void)trainingRestoreScratchFailSafe();
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Training command postcondition failed");
        return trainingHoldFailureFrame(1);
    }
    if (!trainingCaptureSnapshot(g_trainingScratch)) {
        trainingFailMutation(
            TrainingDisableReason::SnapshotFailure,
            "Post-command pre-step snapshot failed");
        return trainingHoldFailureFrame(1);
    }

    if (!trainingAdmitPausedFrame()) {
        g_trainingFinalInput[0] = g_trainingFinalInput[1] = 0u;
        g_trainingLastAdmission = StepAdmissionResult::Paused;
        return 1;
    }

    trainingPrimeThrowReceptionObservations();

    ++g_trainingStepSerial;
    if (g_trainingStepSerial == 0u) ++g_trainingStepSerial;
    const bool refillHeld = !g_trainingOperationNeutral
        && (actions.heldLevelActions
            & qtraining::trainingActionBit(
                qtraining::TrainingAction::RefillAll)) != 0u;
    const bool chargeHeld = !refillHeld
        && !g_trainingOperationNeutral
        && (actions.heldLevelActions
            & qtraining::trainingActionBit(
                qtraining::TrainingAction::ChargeExFraction)) != 0u;
    if (refillHeld) {
        const bool healMode = trainingRestoreAllPresetActive();
        const CommandAckResult held = healMode
            ? trainingApplyExMeterRefill(g_trainingMeterChargeTarget)
            : trainingApplyExSet(g_trainingMeterChargeTarget, 0, 0);
        static bool lastHealMode = false;
        static CommandAckResult lastHeld = CommandAckResult::Applied;
        static bool haveHeld = false;
        if (!haveHeld || healMode != lastHealMode || held != lastHeld) {
            haveHeld = true;
            lastHealMode = healMode;
            lastHeld = held;
            QLOG("[training-refill] held F4 -> %s meter (heal-mode=%u "
                 "target=%u result=%u) step=%llu",
                 healMode ? "FULL" : "ZERO",
                 healMode ? 1u : 0u,
                 static_cast<unsigned>(g_trainingMeterChargeTarget),
                 static_cast<unsigned>(held),
                 static_cast<unsigned long long>(g_trainingStepSerial));
        }
    }
    const qtraining::MeterChargeCadencePlan chargeCadence =
        qtraining::planMeterChargeCadence(
            g_trainingMeterChargeCounter, chargeHeld, true,
            g_trainingMeterChargeInterval);
    g_trainingMeterChargeCounter = chargeCadence.nextCounter;
    if (chargeCadence.fire
        && !trainingChargeExFraction(g_trainingMeterChargeTarget)) {
        (void)trainingRestoreScratchFailSafe();
        return trainingHoldFailureFrame(1);
    }
    if (!trainingComposeInputForStep()) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Training input or macro composition failed");
        (void)trainingRestoreScratchFailSafe();
        return trainingHoldFailureFrame(1);
    }
    g_trainingStepFailure = false;
    g_trainingStepPending = true;
    if (!trainingLiveE2EApplyLethalDamageProbe()) {
        g_trainingLastAdmission =
            StepAdmissionResult::InvariantFailure;
        return trainingHoldFailureFrame(1);
    }
    g_trainingLastAdmission = StepAdmissionResult::Admitted;
    if (g_trainingStepSerial == 1u
        || (g_trainingStepSerial % 600u) == 0u) {
        QLOG("[training-step] admitted serial=%llu layout=%u "
             "stateBytes=%zu input=%05X/%05X",
             static_cast<unsigned long long>(
                 g_trainingStepSerial),
             g_rb.layoutGeneration(), g_rb.totalSize(),
             g_trainingFinalInput[0],
             g_trainingFinalInput[1]);
    }
    return nativeResult;
}

static int __cdecl hkTrainingStepGate() {
    if (g_trainingReturningToSelect) {
        g_trainingFinalInput[0] = 0u;
        g_trainingFinalInput[1] = 0u;
        g_trainingLastNativeReturn = 0u;
        g_trainingLastAdmission =
            StepAdmissionResult::NativeReturnedZero;
        return 0;
    }
    if (g_trainingStepPending)
        (void)trainingFinalizePendingStep();
    if (g_trainingLaunchRequested) {
        (void)trainingApplyConfigOverlay();
        trainingClearNativeDeveloperEdges();
        wr8(kTrainingHitboxAddr,
            qtraining::hitboxVanillaFlagValue(g_trainingHitboxMode));
    }
    const int nativeResult =
        g_trainingOrigStepGate ? g_trainingOrigStepGate() : 1;
    if (g_trainingLaunchRequested) {
        trainingClearNativeDeveloperEdges();
    }
    return trainingAdmitAfterNativeGate(nativeResult);
}

static int __cdecl hkTrainingMatchScope() {
    const int result =
        g_trainingOrigMatchScope ? g_trainingOrigMatchScope() : 1;
    if (g_trainingLaunchRequested)
        (void)trainingFinalizePendingStep();
    return result;
}

static int __fastcall hkTrainingSelectorConfirm(int selectorPlayer) {
    qgame::directboot::TrainingSameCallConfirmSample sample{};
    sample.explicitTraining = g_trainingLaunchRequested;
    if (sample.explicitTraining) {
        sample.scene = rd32(kSceneStateAddr);
        sample.sceneSubstate =
            rd32(kTrainingScene3SubstateAddr);
        sample.selectorPlayer =
            static_cast<uint32_t>(selectorPlayer);
        sample.matchMode = rd32(kTrainingMatchModeAddr);
        sample.controlMask = rd32(kTrainingComMaskAddr);
        sample.p1StateBefore =
            rd32(kTrainingP1SelectStateAddr);
        sample.p2StateBefore =
            rd32(kTrainingP2SelectStateAddr);
        sample.p1InputSource =
            rd32(kTrainingP1InputSourceAddr);
        sample.p2InputSource =
            rd32(kTrainingP2InputSourceAddr);
        sample.p2Route = rd32(kTrainingP2RouteAddr);
    }

    const int result =
        g_trainingOrigSelectorConfirm(selectorPlayer);
    if (!sample.explicitTraining)
        return result;

    sample.p1StateAfter =
        rd32(kTrainingP1SelectStateAddr);
    sample.p2StateAfter =
        rd32(kTrainingP2SelectStateAddr);
    const qgame::directboot::TrainingSameCallConfirmAction action =
        qgame::directboot::decideTrainingSameCallConfirm(sample);
    if (action
        == qgame::directboot::TrainingSameCallConfirmAction::None) {
        return result;
    }

    wr32(kTrainingP2SelectStateAddr, 0u);
    if (action
            == qgame::directboot::TrainingSameCallConfirmAction::FailClosed
        || rd32(kTrainingP2SelectStateAddr) != 0u) {
        QLOG("[training-select] same-call handoff invariant FAILED "
             "player=%d scene=%u/%u mode=%u ctrl=%u "
             "state=%u->%u/%u->%u source=%u/%u route2=%u",
             selectorPlayer, sample.scene, sample.sceneSubstate,
             sample.matchMode, sample.controlMask,
             sample.p1StateBefore, sample.p1StateAfter,
             sample.p2StateBefore, sample.p2StateAfter,
             sample.p1InputSource, sample.p2InputSource,
             sample.p2Route);
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Native P1-to-P2 selector handoff invariant failed");
        return result;
    }

    QLOG("[training-select] held P2 inactive after native "
         "same-call handoff state=%u->%u/%u->%u",
         sample.p1StateBefore, sample.p1StateAfter,
         sample.p2StateBefore, sample.p2StateAfter);
    return result;
}

static int __fastcall hkTrainingDamage(
    int attackerSlot, int victimActor, int cause, int damage) {
    if (!g_trainingLaunchRequested
        || !g_trainingStepPending
        || g_trainingMutationFailed
        || rd32(kSceneStateAddr) != 2u
        || rd32(kMatchSubAddr) != 0u) {
        return g_trainingOrigDamage
            ? g_trainingOrigDamage(
                  attackerSlot, victimActor, cause, damage)
            : 0;
    }
    if (trainingLiveE2EOfflineControlEnabled()) {
        g_trainingLiveE2EMailbox.lethalDamageDetourCount =
            g_trainingLiveE2EMailbox.lethalDamageDetourCount + 1u;
    }

    const uintptr_t victim =
        static_cast<uint32_t>(victimActor);
    if (!rangeReadable(victim, addr::kFighterSize)) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Damage victim pointer was invalid");
        return 0;
    }
    const uint32_t slot =
        rd8(victim + kFighterPlayerSlotOffset);
    const uintptr_t active =
        slot < 4u ? trainingFighter(slot, false) : 0u;
    const uintptr_t mirror =
        slot < 4u ? trainingFighter(slot, true) : 0u;
    if (slot >= 4u || !active || !mirror) {
        QLOG("[training-damage] invalid native target "
             "victim=%08X slot=%u active=%08X mirror=%08X",
             static_cast<unsigned>(victim), slot,
             static_cast<unsigned>(active),
             static_cast<unsigned>(mirror));
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Damage target table pointer validation failed");
        return 0;
    }

    const uintptr_t life = netBattleLifeAddress(slot);
    if (!rangeReadable(life, 16u)) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Damage life record validation failed");
        return 0;
    }
    const int64_t current = static_cast<int32_t>(
        rd32(life + kBattleLifeCurrentOffset));
    const int64_t reserve = static_cast<int32_t>(
        rd32(life + kBattleLifeReserveOffset));
    const int64_t pending = static_cast<int32_t>(
        rd32(life + kBattleLifeDamageOffset));
    const int64_t pendingAfter =
        pending + static_cast<int64_t>(damage);
    const int64_t total = current + reserve;
    const int64_t preEffective = total - pending;
    const int64_t postEffective = total - pendingAfter;
    const auto fitsI32 = [](int64_t value) {
        return value >= INT_MIN && value <= INT_MAX;
    };
    if (!fitsI32(pendingAfter) || !fitsI32(total)
        || !fitsI32(preEffective) || !fitsI32(postEffective)
        || preEffective < 0) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Damage arithmetic preflight failed");
        return 0;
    }

    const uint32_t activeFatalCause =
        rd32(active + kTrainingFighterFatalCauseOffset);
    const uint32_t activeFatalAttacker =
        rd32(active + kTrainingFighterFatalAttackerOffset);
    const uint32_t mirrorFatalCause =
        rd32(mirror + kTrainingFighterFatalCauseOffset);
    const uint32_t mirrorFatalAttacker =
        rd32(mirror + kTrainingFighterFatalAttackerOffset);

    const int result = g_trainingOrigDamage
        ? g_trainingOrigDamage(
              attackerSlot, victimActor, cause, damage)
        : 0;

    const int64_t nativeCurrent = static_cast<int32_t>(
        rd32(life + kBattleLifeCurrentOffset));
    const int64_t nativeReserve = static_cast<int32_t>(
        rd32(life + kBattleLifeReserveOffset));
    const int64_t nativePending = static_cast<int32_t>(
        rd32(life + kBattleLifeDamageOffset));
    const int64_t nativeTotal = nativeCurrent + nativeReserve;
    const int64_t effective = nativeTotal - nativePending;
    if (!fitsI32(nativeTotal) || !fitsI32(effective)) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Native damage result overflowed");
        return result;
    }

    const int64_t observedDelta = nativePending - pending;
    const bool bodyAdmitted = result == 0;
    const bool deltaMatches =
        observedDelta == static_cast<int64_t>(damage);
    g_trainingDamage.damage = damage;
    g_trainingDamage.pendingDelta =
        fitsI32(observedDelta)
            ? static_cast<int32_t>(observedDelta) : 0;
    g_trainingDamage.attackerSlot =
        static_cast<uint32_t>(attackerSlot);
    g_trainingDamage.victimSlot = slot;
    g_trainingDamage.cause = static_cast<uint32_t>(cause);
    if (bodyAdmitted && deltaMatches) {
        g_trainingDamage.admitted = 1u;
        ++g_trainingDamage.admittedSerial;
    } else if (!bodyAdmitted) {
        g_trainingDamage.admitted = 0u;
        ++g_trainingDamage.declinedSerial;
    } else {
        g_trainingDamage.admitted = 0u;
        ++g_trainingDamage.anomalySerial;
        ++g_trainingStickyErrors;
        QLOG("[training-damage] pending delta mismatch slot=%u "
             "damage=%d delta=%lld return=%d",
             slot, damage,
             static_cast<long long>(observedDelta), result);
    }

    if (effective < 0) {
        if (nativeTotal < 0 || nativeTotal > INT_MAX) {
            trainingFailMutation(
                TrainingDisableReason::InvariantFailure,
                "Damage floor could not represent zero life");
            return result;
        }
        wr32(life + kBattleLifeDamageOffset,
             static_cast<uint32_t>(nativeTotal));
        g_trainingDamage.pendingDelta =
            static_cast<int32_t>(preEffective);
        wr32(active + kTrainingFighterFatalCauseOffset,
             activeFatalCause);
        wr32(active + kTrainingFighterFatalAttackerOffset,
             activeFatalAttacker);
        wr32(mirror + kTrainingFighterFatalCauseOffset,
             mirrorFatalCause);
        wr32(mirror + kTrainingFighterFatalAttackerOffset,
             mirrorFatalAttacker);
    }
    return result;
}

static TrainingDisableReason trainingCurrentDisableReason() {
    if (!g_trainingLaunchRequested)
        return TrainingDisableReason::NotTrainingLaunch;
    if (g_netActive)
        return g_netSpectatorMode
            ? TrainingDisableReason::SpectatorMode
            : TrainingDisableReason::OnlineMode;
    if (g_mode == RunMode::SelfTest)
        return TrainingDisableReason::SelfTestOrProbe;
    if (!g_trainingHooksSupported
        || !g_trainingHooksInstalled) {
        return TrainingDisableReason::UnsupportedStepBoundary;
    }
    if (g_trainingMutationFailed)
        return g_trainingDisableReason;
    return TrainingDisableReason::None;
}

struct TrainingCombatReadout {
    uint32_t effectiveLife = 0;
    uint32_t lifeCurrent = 0;
    uint32_t lifePendingDamage = 0;
    uint32_t lifeReserve = 0;
    uint32_t lifeSettleTimer = 0;
    uint32_t guardGauge = 0;
    uint32_t meterStocks = 0;
    uint32_t meterFraction = 0;
    uint32_t comboHitsTaken = 0;
    uint32_t comboDamageTaken = 0;
    uint32_t lastComboHitsTaken = 0;
    uint32_t lastComboDamageTaken = 0;
    uint32_t maxComboHitsDealt = 0;
    uint32_t maxComboDamageDealt = 0;
};

static bool trainingReadCombatReadout(
        uint32_t player, TrainingCombatReadout& out) {
    if (player >= qtraining::kTrainingRefillPlayerCount) return false;
    const uintptr_t fighter = trainingFighter(player, false);
    if (!fighter) return false;
    const uint32_t slot = rd8(fighter + kFighterPlayerSlotOffset);
    const uint32_t side =
        rd8(fighter + kTrainingFighterExOwnerOffset);
    if (slot >= kTrainingCombatRecordSlotCount
        || side >= addr::kExGaugeActiveOwnerCount) {
        return false;
    }
    const uintptr_t record = kTrainingCombatRecordBase
        + static_cast<uintptr_t>(slot) * kTrainingCombatRecordStride;
    const uintptr_t exRecord =
        static_cast<uintptr_t>(addr::kExGaugeRecords)
        + static_cast<uintptr_t>(side) * addr::kExGaugeRecordStride;
    const uintptr_t team = kTrainingTeamComboBase
        + static_cast<uintptr_t>(side) * kTrainingTeamComboStride;
    const uintptr_t match = kTrainingMatchRecordBase
        + static_cast<uintptr_t>(side) * kTrainingMatchRecordStride;
    uintptr_t life = 0;
    if (!rangeReadable(record, kTrainingCombatRecordStride)
        || !rangeReadable(exRecord, addr::kExGaugeRecordStride)
        || !rangeReadable(team, kTrainingTeamComboStride)
        || !rangeReadable(match, kTrainingMatchRecordStride)
        || !trainingLifeRecordAddress(player, life)) {
        return false;
    }

    const TrainingLifeComponents components =
        trainingReadLifeComponents(life);
    const int64_t effective =
        static_cast<int64_t>(components.current)
        + components.reserve - components.pendingDamage;
    if (!qtraining::refillFitsInt32(effective)) return false;
    out.effectiveLife =
        static_cast<uint32_t>(static_cast<int32_t>(effective));
    out.lifeCurrent =
        static_cast<uint32_t>(components.current);
    out.lifePendingDamage =
        static_cast<uint32_t>(components.pendingDamage);
    out.lifeReserve =
        static_cast<uint32_t>(components.reserve);
    out.lifeSettleTimer =
        static_cast<uint32_t>(components.settleTimer);
    out.guardGauge = rd32(record + kTrainingGuardGaugeOffset);
    out.meterStocks = rd32(exRecord + kTrainingExStocksOffset);
    out.meterFraction = rd32(exRecord + kTrainingExFractionOffset);
    out.comboHitsTaken = rd32(record + kTrainingComboHitsOffset);
    out.comboDamageTaken =
        rd32(team + kTrainingTeamComboDamageOffset);
    out.lastComboHitsTaken =
        rd32(team + kTrainingTeamLastComboHitsOffset);
    out.lastComboDamageTaken =
        rd32(team + kTrainingTeamLastComboDamageOffset);
    out.maxComboHitsDealt =
        rd32(match + kTrainingMatchMaxComboHitsOffset);
    out.maxComboDamageDealt =
        rd32(match + kTrainingMatchMaxComboDamageOffset);
    return true;
}

struct TrainingFighterReadout {
    uint32_t actionState = 0;
    uint32_t actionFrame = 0;
    uint32_t actionLockEnd = 0;
    uint32_t animFrame = 0;
    uint32_t nextAction = 0;
    uint32_t transitionFlag = 0;
    uint32_t prevAction = 0;
    uint32_t moveGateByte = 0;
    uint32_t positionX = 0;
    uint32_t positionY = 0;
};

static bool trainingReadFighterReadout(
        uint32_t player, TrainingFighterReadout& out) {
    if (player >= qtraining::kTrainingRefillPlayerCount) return false;
    const uintptr_t fighter = trainingFighter(player, false);
    if (!fighter) return false;
    out.actionState =
        rd32(fighter + kTrainingFighterActionStateOffset);
    out.actionFrame =
        rd32(fighter + kTrainingFighterActionFrameOffset);
    out.actionLockEnd =
        rd32(fighter + kTrainingFighterActionLockEndOffset);
    out.animFrame =
        rd32(fighter + kTrainingFighterAnimFrameOffset);
    out.nextAction =
        rd32(fighter + kTrainingFighterNextActionOffset);
    out.transitionFlag =
        rd32(fighter + kTrainingFighterTransitionFlagOffset);
    out.prevAction =
        rd32(fighter + kTrainingFighterPrevActionOffset);
    out.moveGateByte =
        rd32(fighter + kTrainingFighterMoveGateOffset) & 0xFFu;
    out.positionX = rd32(fighter + kTrainingFighterXOffset);
    out.positionY = rd32(fighter + kTrainingFighterYOffset);
    return true;
}

static void trainingPublishStatus() {
    TrainingStatusV1 status{};
    status.explicitTrainingPresent =
        g_trainingLaunchRequested ? 1u : 0u;
    status.disableReason =
        static_cast<uint32_t>(trainingCurrentDisableReason());
    status.available =
        g_trainingLaunchRequested
        && status.disableReason
            == static_cast<uint32_t>(
                TrainingDisableReason::None)
            ? 1u : 0u;
    status.lifecycleState =
        static_cast<uint32_t>(g_trainingLifecycle);
    status.scene = rd32(kSceneStateAddr);
    status.matchSubstate = rd32(kMatchSubAddr);
    status.inMatchReady =
        status.available && status.scene == 2u
        && status.matchSubstate == 0u
        && g_trainingBaseline.valid ? 1u : 0u;
    status.capabilityFlags =
        g_trainingHooksInstalled
            ? trainingCapabilityFlags() : 0u;
    if (!trainingSoundStopReady()) {
        status.capabilityFlags &=
            ~(qtraining::TrainingCapabilityReset
              | qtraining::TrainingCapabilitySavestates);
    }
    status.trainingGeneration = g_trainingGeneration;
    status.realStepSerialLow =
        static_cast<uint32_t>(g_trainingStepSerial);
    status.realStepSerialHigh =
        static_cast<uint32_t>(g_trainingStepSerial >> 32u);

    const uintptr_t p1 = trainingFighter(0u, false);
    const uintptr_t p2 = trainingFighter(1u, false);
    const uintptr_t p1Mirror = trainingFighter(0u, true);
    const uintptr_t p2Mirror = trainingFighter(1u, true);
    if (p1) {
        status.p1Character =
            rd8(p1 + kTrainingFighterCharacterOffset);
        status.p1Facing =
            rd8(p1 + kTrainingFighterFacingOffset);
        status.p1ComPrimary =
            rd32(p1 + kTrainingFighterComOffset);
    }
    if (p2) {
        status.p2Character =
            rd8(p2 + kTrainingFighterCharacterOffset);
        status.p2Facing =
            rd8(p2 + kTrainingFighterFacingOffset);
        status.p2ComPrimary =
            rd32(p2 + kTrainingFighterComOffset);
    }
    if (p1Mirror)
        status.p1ComMirror =
            rd32(p1Mirror + kTrainingFighterComOffset);
    if (p2Mirror)
        status.p2ComMirror =
            rd32(p2Mirror + kTrainingFighterComOffset);
    status.p1FinalLogicalMask = g_trainingFinalInput[0];
    status.p2FinalLogicalMask = g_trainingFinalInput[1];
    status.comMask = rd32(kTrainingComMaskAddr);

    status.guardMode =
        static_cast<uint32_t>(g_trainingGuardMode);
    status.guardPermission = g_trainingGuardPermission;
    status.guardClassification =
        static_cast<uint32_t>(g_trainingGuardClassification);
    status.guardPredictionMisses =
        g_trainingGuardTelemetry.counters().firstActiveMisses;
    status.guardDuringPlayback =
        g_trainingGuardDuringPlayback ? 1u : 0u;
    status.dummyStance =
        static_cast<uint32_t>(g_trainingDummyStance);
    status.dummyTech =
        static_cast<uint32_t>(g_trainingDummyTech);
    status.meterChargeTarget =
        static_cast<uint32_t>(g_trainingMeterChargeTarget);
    status.meterChargeInterval = g_trainingMeterChargeInterval;

    TrainingCombatReadout combat[
        qtraining::kTrainingRefillPlayerCount] = {};
    TrainingFighterReadout fighters[
        qtraining::kTrainingRefillPlayerCount] = {};
    int64_t lifeMax = 0;
    if (status.inMatchReady
        && trainingReadCombatReadout(0u, combat[0])
        && trainingReadCombatReadout(1u, combat[1])
        && trainingNativeMaxLife(lifeMax)) {
        status.combatReadoutValid = 1u;
        status.lifeMax = static_cast<uint32_t>(lifeMax);
        status.p1EffectiveLife = combat[0].effectiveLife;
        status.p2EffectiveLife = combat[1].effectiveLife;
        status.p1GuardGauge = combat[0].guardGauge;
        status.p2GuardGauge = combat[1].guardGauge;
        status.p1MeterStocks = combat[0].meterStocks;
        status.p2MeterStocks = combat[1].meterStocks;
        status.p1MeterFraction = combat[0].meterFraction;
        status.p2MeterFraction = combat[1].meterFraction;
        status.p1ComboHitsTaken = combat[0].comboHitsTaken;
        status.p2ComboHitsTaken = combat[1].comboHitsTaken;
        status.p1ComboDamageTaken = combat[0].comboDamageTaken;
        status.p2ComboDamageTaken = combat[1].comboDamageTaken;
        status.p1LastComboHitsTaken =
            combat[0].lastComboHitsTaken;
        status.p2LastComboHitsTaken =
            combat[1].lastComboHitsTaken;
        status.p1LastComboDamageTaken =
            combat[0].lastComboDamageTaken;
        status.p2LastComboDamageTaken =
            combat[1].lastComboDamageTaken;
        status.p1MaxComboHitsDealt = combat[0].maxComboHitsDealt;
        status.p2MaxComboHitsDealt = combat[1].maxComboHitsDealt;
        status.p1MaxComboDamageDealt =
            combat[0].maxComboDamageDealt;
        status.p2MaxComboDamageDealt =
            combat[1].maxComboDamageDealt;
        status.p1LifeCurrent = combat[0].lifeCurrent;
        status.p1LifePendingDamage = combat[0].lifePendingDamage;
        status.p1LifeReserve = combat[0].lifeReserve;
        status.p1LifeSettleTimer = combat[0].lifeSettleTimer;
        status.p2LifeCurrent = combat[1].lifeCurrent;
        status.p2LifePendingDamage = combat[1].lifePendingDamage;
        status.p2LifeReserve = combat[1].lifeReserve;
        status.p2LifeSettleTimer = combat[1].lifeSettleTimer;
    }

    if (status.inMatchReady
        && trainingReadFighterReadout(0u, fighters[0])
        && trainingReadFighterReadout(1u, fighters[1])) {
        status.fighterReadoutValid = 1u;
        status.p1ActionState = fighters[0].actionState;
        status.p1ActionFrame = fighters[0].actionFrame;
        status.p1ActionLockEnd = fighters[0].actionLockEnd;
        status.p1AnimFrame = fighters[0].animFrame;
        status.p1NextAction = fighters[0].nextAction;
        status.p1TransitionFlag = fighters[0].transitionFlag;
        status.p1PrevAction = fighters[0].prevAction;
        status.p1MoveGateByte = fighters[0].moveGateByte;
        status.p1PositionX = fighters[0].positionX;
        status.p1PositionY = fighters[0].positionY;
        status.p2ActionState = fighters[1].actionState;
        status.p2ActionFrame = fighters[1].actionFrame;
        status.p2ActionLockEnd = fighters[1].actionLockEnd;
        status.p2AnimFrame = fighters[1].animFrame;
        status.p2NextAction = fighters[1].nextAction;
        status.p2TransitionFlag = fighters[1].transitionFlag;
        status.p2PrevAction = fighters[1].prevAction;
        status.p2MoveGateByte = fighters[1].moveGateByte;
        status.p2PositionX = fighters[1].positionX;
        status.p2PositionY = fighters[1].positionY;
        status.p1PostureClass = static_cast<uint32_t>(
            qtraining::classifyPostureNibble(
                static_cast<uint8_t>(fighters[0].moveGateByte)));
        status.p2PostureClass = static_cast<uint32_t>(
            qtraining::classifyPostureNibble(
                static_cast<uint8_t>(fighters[1].moveGateByte)));
    }

    status.holdReasons = g_trainingHold.reasons();

    status.guardHeuristicPostureDivergences =
        g_trainingGuardPostureDivergences;
    status.guardHeuristicAmbiguousAttackers =
        g_trainingGuardAmbiguousAttackers;
    status.guardHeuristicUnguardablePostures =
        g_trainingGuardUnguardablePostures;

    status.healthTrigger = static_cast<uint32_t>(
        g_trainingResourcePolicies.health.trigger);
    status.exTrigger = static_cast<uint32_t>(
        g_trainingResourcePolicies.ex.trigger);
    status.guardGaugeTrigger = static_cast<uint32_t>(
        g_trainingResourcePolicies.guardGauge.trigger);
    status.healthTriggerTarget = static_cast<uint32_t>(
        g_trainingResourcePolicies.health.target);
    status.exTriggerTarget = static_cast<uint32_t>(
        g_trainingResourcePolicies.ex.target);
    status.guardGaugeTriggerTarget = static_cast<uint32_t>(
        g_trainingResourcePolicies.guardGauge.target);
    status.resourcePolicyFires = g_trainingResourcePolicyFires;
    status.resourcePolicyRejects = g_trainingResourcePolicyRejects;
    status.roundStartNeutralMask = g_trainingRoundStartNeutralMask;
    status.roundStartNeutralValid = g_trainingRoundStartNeutralValid;
    if (status.inMatchReady && g_trainingResourcePolicyValid != 0u) {
        status.resourcePolicyValid = 1u;
        status.bothFightersNeutral = g_trainingBothFightersNeutral;
        status.neutralHoldSteps = g_trainingNeutralHoldSteps;
        status.p1NeutralReasonMask = g_trainingNeutralReasonMask[0];
        status.p2NeutralReasonMask = g_trainingNeutralReasonMask[1];
        status.neutralGlobalReasonMask =
            g_trainingNeutralGlobalReasonMask;
        status.p1ComboEpisodeState = static_cast<uint32_t>(
            g_trainingComboEpisode[0].state());
        status.p2ComboEpisodeState = static_cast<uint32_t>(
            g_trainingComboEpisode[1].state());
        status.p1RecoveryLock = g_trainingRecoveryLockReadout[0];
        status.p2RecoveryLock = g_trainingRecoveryLockReadout[1];
        status.p1RecoveryPhaseLatch = g_trainingRecoveryPhaseReadout[0];
        status.p2RecoveryPhaseLatch = g_trainingRecoveryPhaseReadout[1];
        status.p1ComboHitsNative = g_trainingComboHitsReadout[0];
        status.p2ComboHitsNative = g_trainingComboHitsReadout[1];
        status.p1AuthoredFrameFlags = g_trainingAuthoredFlagsReadout[0];
        status.p2AuthoredFrameFlags = g_trainingAuthoredFlagsReadout[1];
        status.p1WallFlags = g_trainingWallFlagsReadout[0];
        status.p2WallFlags = g_trainingWallFlagsReadout[1];
        status.p1TerminalReady = g_trainingTerminalReadyReadout[0];
        status.p2TerminalReady = g_trainingTerminalReadyReadout[1];
        status.p1DispatchAction = g_trainingDispatchActionReadout[0];
        status.p2DispatchAction = g_trainingDispatchActionReadout[1];
        status.p1InitStatus = g_trainingInitStatusReadout[0];
        status.p2InitStatus = g_trainingInitStatusReadout[1];
        status.lifeSetMax = status.combatReadoutValid != 0u
            ? status.lifeMax : 0u;
        status.exStocksMax =
            static_cast<uint32_t>(qtraining::kExStocksFull);
        status.guardGaugeMax =
            static_cast<uint32_t>(qtraining::kGuardGaugeFull);
    }

    status.recoveryModeAir =
        static_cast<uint32_t>(g_trainingRecoverySettings.air);
    status.recoveryModeGround =
        static_cast<uint32_t>(g_trainingRecoverySettings.ground);
    status.recoveryModeWall =
        static_cast<uint32_t>(g_trainingRecoverySettings.wall);
    status.recoveryModeTerminal =
        static_cast<uint32_t>(g_trainingRecoverySettings.terminal);
    status.recoveryModeThrowEscape =
        static_cast<uint32_t>(g_trainingRecoverySettings.throwEscape);
    status.recoveryTerminalChoice = static_cast<uint32_t>(
        g_trainingRecoverySettings.terminalChoice);
    status.recoveryDelaySteps =
        g_trainingRecoverySettings.delaySteps;
    status.recoveryRandomMinSteps =
        g_trainingRecoverySettings.randomMinSteps;
    status.recoveryRandomMaxSteps =
        g_trainingRecoverySettings.randomMaxSteps;
    status.recoveryEvaluatorReason =
        static_cast<uint32_t>(g_trainingRecoveryEvaluatorReason);
    {
        const qtraining::RecoveryCounters& counters =
            g_trainingRecovery.counters();
        status.recoveryAttempts = counters.attempts;
        status.recoverySucceeded = counters.succeeded;
        status.recoveryMissed = counters.missed;
        status.recoveryCancelled = counters.cancelled;
        status.recoveryBufferedObserved = counters.bufferedObserved;
        status.recoveryAutomaticObserved = counters.automaticObserved;
        status.recoveryCommitBlocked = counters.commitBlocked;
        status.recoveryLastResultAction = counters.lastResultAction;
    }
    status.recoveryLastVerdict = static_cast<uint32_t>(
        g_trainingRecoveryEmission.lastVerdict);
    status.recoveryStandDownMask =
        g_trainingRecoveryEmission.standDownMask;
    status.recoveryTapsEmitted =
        g_trainingRecoveryEmission.tapsEmitted;
    if (status.inMatchReady && g_trainingRecoveryReadoutValid != 0u) {
        status.recoveryReadoutValid = 1u;
        status.recoveryState = static_cast<uint32_t>(
            g_trainingRecoveryEmission.state);
        status.recoveryBranch = static_cast<uint32_t>(
            g_trainingRecoveryEmission.branch);
        status.recoveryEligibleMask =
            g_trainingRecoveryEmission.eligibleMask;
        status.recoveryArmableMask =
            g_trainingRecoveryEmission.armableMask;
        status.recoveryStepsRemaining =
            g_trainingRecoveryEmission.stepsRemaining;
        status.recoveryContactTestUnknown =
            g_trainingRecoveryEmission.contactTestUnknown ? 1u : 0u;
    }

    status.combatToolsReadoutValid =
        status.inMatchReady && g_trainingCombatToolsReadoutValid != 0u
        && g_trainingContactHooksReady ? 1u : 0u;
    status.throwTechChancePercent = g_trainingThrowTech.chancePercent;
    status.throwTechOpportunities = g_trainingThrowTech.opportunities;
    status.throwTechNativeAccepted = g_trainingThrowTech.nativeAccepted;
    status.throwTechAutomaticAccepted =
        g_trainingThrowTech.automaticAccepted;
    status.throwTechResultObserved = g_trainingThrowTech.resultObserved;
    status.triggerMoveCatalogId = qtraining::trainingMoveCatalogId();
    for (uint32_t event = 0u;
         event < qtraining::kTrainingTriggerEventCount; ++event) {
        const qtraining::TrainingTriggerConfig config =
            g_trainingTriggers.config(
                static_cast<TrainingTriggerEvent>(event));
        status.triggerMacroSlot[event] = config.macroSlot;
        status.triggerMoveIndex[event] = config.moveIndex;
        status.triggerDelaySteps[event] = config.delaySteps;
        status.triggerChancePercent[event] = config.chancePercent;
    }
    const qtraining::TrainingTriggerStatus& triggerStatus =
        g_trainingTriggers.status();
    status.triggerPendingCount = triggerStatus.pendingCount;
    status.triggerStartedCount = triggerStatus.startedCount;
    status.triggerChanceSkippedCount =
        triggerStatus.chanceSkippedCount;
    status.triggerDroppedCount = triggerStatus.droppedCount;
    status.triggerLastEvent =
        static_cast<uint32_t>(triggerStatus.lastEvent);
    status.triggerLastMacroSlot = triggerStatus.lastMacroSlot;
    status.triggerLastMoveIndex = triggerStatus.lastMoveIndex;
    status.triggerLastOutcome =
        static_cast<uint32_t>(triggerStatus.lastOutcome);

    const qtraining::FrameAdvantageStatus& advantageStatus =
        g_trainingFrameAdvantage.status();
    status.frameAdvantageEnabled =
        g_trainingFrameAdvantage.enabled() ? 1u : 0u;
    status.frameAdvantageState =
        static_cast<uint32_t>(advantageStatus.state);
    status.frameAdvantageContactKind =
        static_cast<uint32_t>(advantageStatus.contactKind);
    status.frameAdvantageAttackerOwner = advantageStatus.attackerOwner;
    status.frameAdvantageDefenderOwner = advantageStatus.defenderOwner;
    status.frameAdvantageValid = advantageStatus.advantageValid ? 1u : 0u;
    status.frameAdvantageValue =
        static_cast<uint32_t>(advantageStatus.advantageFrames);
    status.frameAdvantageGapValid = advantageStatus.gapValid ? 1u : 0u;
    status.frameAdvantageGapFrames = advantageStatus.gapFrames;
    status.frameAdvantageAttackerReadyDelay =
        static_cast<uint32_t>(advantageStatus.attackerReadyDelay);
    status.frameAdvantageDefenderReadyDelay =
        static_cast<uint32_t>(advantageStatus.defenderReadyDelay);
    status.frameAdvantageCompletedCount = advantageStatus.completedCount;
    status.frameAdvantageTradeCount = advantageStatus.tradeCount;
    status.frameAdvantageTimeoutCount = advantageStatus.timeoutCount;

    status.debugOverlayEnabled =
        g_trainingDebugOverlayEnabled ? 1u : 0u;
    status.debugStepSerialLow = static_cast<uint32_t>(g_trainingStepSerial);
    status.debugStepSerialHigh =
        static_cast<uint32_t>(g_trainingStepSerial >> 32u);
    status.debugP1LogicalMask = g_trainingLastComposedInput[0];
    status.debugP2LogicalMask = g_trainingLastComposedInput[1];
    if (status.inMatchReady && p1 && p2) {
        status.p1FreezeFrames =
            rd32(p1 + kTrainingFighterFreezeOffset);
        status.p2FreezeFrames =
            rd32(p2 + kTrainingFighterFreezeOffset);
        status.p1ResponseReady =
            trainingNativeResponseReady(p1, 0u) ? 1u : 0u;
        status.p2ResponseReady =
            trainingNativeResponseReady(p2, 1u) ? 1u : 0u;
        status.p2GuardCancelReady =
            trainingNativeGuardCancelReady(p2, 1u) ? 1u : 0u;
    }
    status.wakeupReversalPhase = static_cast<uint32_t>(
        g_trainingWakeupReversal.phase);
    status.wakeupReversalMoveIndex =
        g_trainingWakeupReversal.moveIndex;
    status.wakeupLockedStepsRemaining =
        g_trainingWakeupReversal.lockedStepsRemaining;
    status.wakeupCommitIndex =
        g_trainingWakeupReversal.commitIndex;
    status.wakeupPrefixCursor =
        g_trainingMacros.bufferedReversalActive()
            ? g_trainingMacros.cursor() : 0u;
    status.wakeupLastOutcome = static_cast<uint32_t>(
        g_trainingWakeupReversal.lastOutcome);

    getModularNativeHudRequested(nullptr,
                                 &status.nativeHudRequestedGlobalMask,
                                 &status.nativeHudRequestedP1Mask,
                                 &status.nativeHudRequestedP2Mask);
    getModularNativeHudStatus(&status.nativeHudOwner,
                              &status.nativeHudGlobalMask,
                              &status.nativeHudP1Mask,
                              &status.nativeHudP2Mask,
                              &status.nativeHudUnavailableGlobalMask,
                              &status.nativeHudUnavailableP1Mask,
                              &status.nativeHudUnavailableP2Mask);
    status.stateChangeLogEnabled = trainingStateLogRecording() ? 1u : 0u;
    status.stateChangeLogRecords = trainingStateLogRecordCount();
    status.stateChangeLogFailed = trainingStateLogFailed() ? 1u : 0u;
    status.frameTraceEnabled = trainingFrameTraceRecording() ? 1u : 0u;
    status.frameTraceRows = trainingFrameTraceRowCount();
    status.frameTraceFailed = trainingFrameTraceFailed() ? 1u : 0u;
    status.frameTraceCapped = trainingFrameTraceCapped() ? 1u : 0u;
    status.triggerSuppressionReason = static_cast<uint32_t>(
        g_trainingTriggers.diagnostics().reason);

    status.faultlessPolicy =
        static_cast<uint32_t>(g_trainingContactPolicyRequested.faultless);
    status.parryPolicy =
        static_cast<uint32_t>(g_trainingContactPolicyRequested.parry);
    status.criticalP1Policy =
        static_cast<uint32_t>(g_trainingContactPolicyRequested.criticalP1);
    status.criticalP2Policy =
        static_cast<uint32_t>(g_trainingContactPolicyRequested.criticalP2);
    status.faultlessAvailable = g_trainingFaultlessHookReady ? 1u : 0u;
    status.parryAvailable = g_trainingParryHookReady ? 1u : 0u;
    status.criticalAvailable = g_trainingCriticalHookReady ? 1u : 0u;
    {
        const uintptr_t dummy = trainingFighter(trainingDummySlot(), false);
        const uint32_t actor = dummy != 0u ? rd8(dummy + 174u) : 0xFFu;
        status.faultlessSupportedDummy =
            qtraining::faultlessSupportedActor(actor) ? 1u : 0u;
        status.parrySupportedDummy =
            qtraining::parrySupportedActor(actor) ? 1u : 0u;
        uintptr_t exRecord = 0u;
        uint32_t exOwner = 0u;
        if (dummy != 0u
            && trainingExRecordAddress(1u, exOwner, exRecord)
            && exRecord != 0u) {
            status.dummyFractionalMeter = static_cast<uint32_t>(
                static_cast<int32_t>(
                    rd32(exRecord + kTrainingExFractionOffset)));
        }
    }
    if (g_trainingLastFaultless.valid) {
        status.lastFaultlessNativeMode = g_trainingLastFaultless.nativeMode;
        status.lastFaultlessFinalMode = g_trainingLastFaultless.finalMode;
        status.lastFaultlessReason = g_trainingLastFaultless.policy;
    }
    if (g_trainingLastParry.valid) {
        status.lastParryNativeResult = g_trainingLastParry.nativeResult;
        status.lastParryFinalResult = g_trainingLastParry.finalResult;
        status.lastParryReason = g_trainingLastParry.policy;
    }
    status.reduceP1Policy =
        static_cast<uint32_t>(g_trainingContactPolicyRequested.reduceP1);
    status.reduceP2Policy =
        static_cast<uint32_t>(g_trainingContactPolicyRequested.reduceP2);
    status.reduceAvailable = g_trainingReduceHookReady ? 1u : 0u;
    status.guardTimingMode =
        static_cast<uint32_t>(g_trainingGuardTimingConfig.mode);
    status.guardTimingWindow = g_trainingGuardTimingConfig.windowSteps;
    status.guardTimingArmed = g_trainingGuardTimingArmed ? 1u : 0u;
    status.playerSwap = g_trainingPlayerSwap ? 1u : 0u;
    if (g_trainingLastReduce.valid) {
        status.lastReduceVictimSlot = g_trainingLastReduce.victimSlot;
        status.lastReduceNativeVerdict = g_trainingLastReduce.nativeVerdict;
        status.lastReduceFinalVerdict = g_trainingLastReduce.finalVerdict;
        status.lastReduceReason = g_trainingLastReduce.policy;
    }
    status.exRestorePinned = g_trainingExRestoreTarget.pinned ? 1u : 0u;
    status.exRestoreStocks =
        static_cast<uint32_t>(g_trainingExRestoreTarget.stocks);
    status.exRestoreFraction =
        static_cast<uint32_t>(g_trainingExRestoreTarget.fraction);
    status.macroAfterReset =
        static_cast<uint32_t>(g_trainingMacroAfterReset);
    if (g_trainingLastCritical.valid) {
        status.lastCriticalAttackerSlot = g_trainingLastCritical.attackerSlot;
        status.lastCriticalIncomingHit = g_trainingLastCritical.incomingHit;
        status.lastCriticalDivisor = g_trainingLastCritical.divisor;
        status.lastCriticalNativeSample = g_trainingLastCritical.nativeSample;
        status.lastCriticalFinalSample = g_trainingLastCritical.finalSample;
        status.lastCriticalGuardCrushLatch =
            g_trainingLastCritical.guardCrushLatch;
        status.lastCriticalFinalFlag = g_trainingLastCritical.finalFlag;
        status.lastCriticalDamageBefore = g_trainingLastCritical.damageBefore;
        status.lastCriticalDamageAfter = g_trainingLastCritical.damageAfter;
        status.lastCriticalReason = g_trainingLastCritical.policy;
    }
    if (status.inMatchReady) trainingObserveActorPools(status);

    status.damageEventSerial = g_trainingDamage.admittedSerial;
    status.damageDeclinedCount = g_trainingDamage.declinedSerial;
    status.lastHitDamage =
        static_cast<uint32_t>(g_trainingDamage.damage);
    status.lastHitPendingDelta =
        static_cast<uint32_t>(g_trainingDamage.pendingDelta);
    status.lastHitAttackerSlot = g_trainingDamage.attackerSlot;
    status.lastHitVictimSlot = g_trainingDamage.victimSlot;
    status.lastHitCause = g_trainingDamage.cause;
    status.lastHitAdmitted = g_trainingDamage.admitted;

    status.endlessRoundRequested = 1u;
    status.endlessRoundEffective =
        status.inMatchReady != 0u
        && trainingInvariantsHold()
            ? 1u : 0u;
    status.infiniteTimeRequested = 1u;
    const uint32_t active =
        rd32(RollbackManager::kQohActiveConfigPointerAddress);
    const bool activeValid =
        RollbackManager::validQohActiveConfigPointer(active);
    status.infiniteTimeEffective =
        activeValid && rd32(active + 36u) == 2u
        && rd32(kTrainingCopiedConfigAddr + 36u) == 2u
            ? 1u : 0u;
    status.extraFunctionsDisabled =
        activeValid && rd32(active + 80u) == 0u ? 1u : 0u;
    status.waitCountZero =
        activeValid && rd32(active + 84u) == 0u
        && rd32(active + 88u) == 0u ? 1u : 0u;
    status.pauseNeutral =
        rd8(kTrainingPauseAddr) == 0u ? 1u : 0u;
    status.freezeNeutral =
        rd8(kTrainingFreezeAddr) == 0u ? 1u : 0u;

    status.lastAdmissionResult =
        static_cast<uint32_t>(g_trainingLastAdmission);
    status.lastNativeReturn = g_trainingLastNativeReturn;
    status.inputRearmState =
        static_cast<uint32_t>(g_trainingInput.rearmState());
    status.macroState =
        static_cast<uint32_t>(g_trainingMacros.state());
    switch (g_trainingMacros.state()) {
        case MacroState::PreRecord:
            status.statusModeKind =
                static_cast<uint32_t>(qtraining::StatusMode::PreRecord);
            status.statusModeDetail = g_trainingMacros.selectedSlot() + 1u;
            break;
        case MacroState::RecordArmed:
        case MacroState::Recording:
            status.statusModeKind =
                static_cast<uint32_t>(qtraining::StatusMode::Recording);
            status.statusModeDetail = g_trainingMacros.selectedSlot() + 1u;
            break;
        case MacroState::PlayArmed:
        case MacroState::Playing:
            status.statusModeKind =
                static_cast<uint32_t>(qtraining::StatusMode::Playing);
            status.statusModeDetail =
                g_trainingMacros.activePlaybackSlot() + 1u;
            break;
        default:
            status.statusModeKind =
                static_cast<uint32_t>(qtraining::StatusMode::None);
            status.statusModeDetail = 0u;
            break;
    }
    status.statusEventKind = g_trainingStatusEventKind;
    status.statusEventDetail = g_trainingStatusEventDetail;
    status.statusEventSequence = g_trainingStatusEventSequence;
    status.macroCursor = g_trainingMacros.cursor();
    status.selectedMacroSlot =
        g_trainingMacros.selectedSlot();
    status.playbackMode =
        static_cast<uint32_t>(g_trainingMacros.playbackMode());
    status.facingPolicy =
        static_cast<uint32_t>(g_trainingMacros.facingPolicy());
    for (uint32_t slot = 0;
         slot < qtraining::kTrainingMacroSlotCount; ++slot) {
        const uint32_t count =
            g_trainingMacros.slot(slot).frameCount;
        status.macroFrameCounts[slot] = count;
        if (count != 0u)
            status.validMacroSlots |= 1u << slot;
    }
    status.selectedStateSlot = g_trainingSelectedStateSlot;
    for (uint32_t slot = 0;
         slot < qtraining::kTrainingStateSlotCount; ++slot) {
        if (g_trainingStates[slot].valid)
            status.validStateSlots |= 1u << slot;
        if (g_trainingStates[slot].stale)
            status.staleStateSlots |= 1u << slot;
    }
    status.rollbackLayoutGeneration =
        g_rbReady ? g_rb.layoutGeneration() : 0u;
    const uint64_t fingerprint =
        (p1 && p2) ? trainingCharacterFingerprint() : 0u;
    status.characterTeamFingerprintLow =
        static_cast<uint32_t>(fingerprint);
    status.characterTeamFingerprintHigh =
        static_cast<uint32_t>(fingerprint >> 32u);

    status.hitboxRequested =
        static_cast<uint32_t>(g_trainingHitboxMode);
    status.hitboxEffective =
        qtraining::hitboxModeWantsVanillaFlag(g_trainingHitboxMode)
            ? (rd8(kTrainingHitboxAddr) != 0u ? 1u : 0u)
            : (qtraining::hitboxModeWantsCustomOverlay(g_trainingHitboxMode)
                   ? (qtraining::hitboxOverlayRegistered()
                      && g_trainingHitboxOverlayVisible ? 1u : 0u)
                   : 0u);
    status.menuCaptured =
        InterlockedCompareExchange(
            &g_trainingMenuCaptured, 0, 0) != 0
            ? 1u : 0u;
    status.lastAcknowledgedCommandId = g_trainingLastAckId;
    status.lastCommandResult =
        static_cast<uint32_t>(g_trainingLastAckResult);
    status.commandQueueDepth =
        g_trainingCommands.approximateDepth();
    status.commandQueueDrops =
        g_trainingCommands.queueFullDrops();
    status.stickyValidationErrors = g_trainingStickyErrors;
    trainingCopyText(status.disableText,
                     g_trainingDisableText);
    trainingCopyText(status.statusText,
                     g_trainingStatusText);
    trainingCopyText(status.resultText,
                     g_trainingResultText);
    g_trainingStatusPublisher.publish(status);
}

static void trainingPublishNativeHudOnlyStatus() {
    TrainingStatusV1 status{};
    status.structSize = static_cast<uint32_t>(sizeof(status));
    getModularNativeHudRequested(nullptr,
                                 &status.nativeHudRequestedGlobalMask,
                                 &status.nativeHudRequestedP1Mask,
                                 &status.nativeHudRequestedP2Mask);
    getModularNativeHudStatus(&status.nativeHudOwner,
                              &status.nativeHudGlobalMask,
                              &status.nativeHudP1Mask,
                              &status.nativeHudP2Mask,
                              &status.nativeHudUnavailableGlobalMask,
                              &status.nativeHudUnavailableP1Mask,
                              &status.nativeHudUnavailableP2Mask);
    g_trainingStatusPublisher.publish(status);
}

static BOOL __stdcall trainingStatusProvider(
    Qoh99TrainingStatusV1* outStatus,
    DWORD outSize, void*) {
    if (!outStatus
        || outSize < qtraining::kTrainingStatusV1BaseSize) {
        return FALSE;
    }
    TrainingStatusV1 status{};
    if (!g_trainingStatusPublisher.read(status))
        return FALSE;
    static_assert(sizeof(status) == sizeof(*outStatus),
                  "training wrapper status ABI mismatch");
    static_assert(
        offsetof(TrainingStatusV1, p1FinalLogicalMask)
            == offsetof(Qoh99TrainingStatusV1,
                        p1FinalLogicalMask),
        "training wrapper status offset mismatch");
    static_assert(sizeof(status) >= qtraining::kTrainingStatusV1BaseSize,
                  "training status prefix cannot exceed the struct");
    const uint32_t copied =
        qtraining::trainingStatusTierFor(
            static_cast<uint32_t>(outSize));
    if (copied == 0u || copied > sizeof(status)) return FALSE;
    std::memcpy(outStatus, &status, copied);
    outStatus->structSize = copied;
    return TRUE;
}

static BOOL __stdcall trainingCommandEnqueue(
    const Qoh99TrainingCommandV1* command,
    DWORD commandSize, void*) {
    if (!command
        || commandSize < sizeof(*command)) {
        return FALSE;
    }
    static_assert(sizeof(TrainingCommandV1)
                      == sizeof(*command),
                  "training wrapper command ABI mismatch");
    TrainingCommandV1 incoming{};
    std::memcpy(&incoming, command, sizeof(incoming));
    return g_trainingCommands.tryEnqueue(
               &incoming, commandSize)
            == qtraining::CommandIngressResult::Accepted
        ? TRUE : FALSE;
}

static BOOL __stdcall trainingMenuCapture(
    DWORD generation, BOOL captured, void*) {
    if (generation != static_cast<DWORD>(
            InterlockedCompareExchange(
                &g_trainingUiGeneration, 0, 0))) {
        return FALSE;
    }
    const LONG next = captured ? 1 : 0;
    const LONG previous =
        InterlockedExchange(&g_trainingMenuCaptured, next);
    if (previous != next) {
        InterlockedIncrement(
            &g_trainingFrontendCaptureGeneration);
        InterlockedExchange(
            &g_trainingFrontendInputArmed, 0);
        g_trainingLiveE2ELogicalMask = 0u;
    }
    return TRUE;
}

static void trainingTryRegisterUi(bool hudOnly = false) {
    if ((!g_trainingLaunchRequested && !hudOnly)
        || g_trainingUiRegistered) {
        return;
    }
    const uint64_t now = GetTickCount64();
    if (g_trainingUiRetryMs != 0u
        && now - g_trainingUiRetryMs < 500u) {
        return;
    }
    g_trainingUiRetryMs = now;
    HMODULE wrapper = GetModuleHandleW(L"ddraw.dll");
    if (!wrapper) return;
    const auto registerProvider =
        reinterpret_cast<RegisterTrainingUiProviderFn>(
            GetProcAddress(
                wrapper,
                "EFZDX9_RegisterTrainingUiProvider"));
    if (!registerProvider) return;
    if (hudOnly) {
        trainingPublishNativeHudOnlyStatus();
    } else {
        trainingPublishStatus();
    }
    registerProvider(
        qtraining::kTrainingApiVersion,
        &trainingStatusProvider,
        &trainingCommandEnqueue,
        &trainingMenuCapture,
        nullptr);
    g_trainingUiRegistered = true;
    QLOG("[training-ui] provider registered api=%u "
         "status=%zu prefix=%u command=%zu",
         qtraining::kTrainingApiVersion,
         sizeof(TrainingStatusV1),
         qtraining::kTrainingStatusV1BaseSize,
         sizeof(TrainingCommandV1));
}

static constexpr uintptr_t kTrainingFighterTableAddr = 0x4A37B4u;
static constexpr uintptr_t kTrainingPartnerTableAddr = 0x623500u;
static constexpr uintptr_t kTrainingClonePoolAddr = 0x526FA0u;
static constexpr uint32_t kTrainingClonePoolCount = 120u;
static constexpr uint32_t kTrainingActorStride =
    static_cast<uint32_t>(addr::kFighterSize);
static constexpr uint32_t kTrainingFighterSlots = 4u;
static constexpr uint32_t kTrainingPartnerPresentOffset = 1456u;
static constexpr uintptr_t kTrainingCameraGroundAddr = 0x87D97Cu;

struct TrainingHitboxCategory {
    qtraining::HitboxBoxSource source;
    uint32_t offsetByte;
    uint32_t countByte;
    bool attackArray;
};

// Secondary (+0x1D/+0x1E) resolves out of the same 8-byte array as body and hurt, indexed from action+8 (sub_467EA0).
static const TrainingHitboxCategory kTrainingHitboxCategories[] = {
    {qtraining::HitboxBoxSource::Body,      23u, 24u, false},
    {qtraining::HitboxBoxSource::Hurt,      25u, 26u, false},
    {qtraining::HitboxBoxSource::Secondary, 29u, 30u, false},
    {qtraining::HitboxBoxSource::Attack,    31u, 32u, true},
};

static bool trainingReadHitboxCamera(qtraining::HitboxCameraV1& camera) {
    if (!rangeReadable(kTrainingCameraAddr, 8u)
        || !rangeReadable(kTrainingCameraGroundAddr, 4u)) {
        return false;
    }
    camera.scrollX = static_cast<int32_t>(rd32(kTrainingCameraAddr));
    camera.scrollY = static_cast<int32_t>(rd32(kTrainingCameraYAddr));
    float ground = 0.0f;
    const uint32_t rawGround = rd32(kTrainingCameraGroundAddr);
    std::memcpy(&ground, &rawGround, sizeof(ground));
    if (!(ground > -4096.0f && ground < 4096.0f)) return false;
    camera.groundOffset = static_cast<int32_t>(ground);
    return true;
}

static void trainingReadActorBoxes(uintptr_t actor,
                                   const qtraining::HitboxCameraV1& camera,
                                   uint8_t owner,
                                   bool clashingNow,
                                   qtraining::HitboxSnapshotV1& snapshot) {
    if (!actor || !rangeReadable(actor, kTrainingActorStride)) return;

    const uintptr_t actionTable = rd32(actor + 16u);
    const uintptr_t frameBase = rd32(actor + 24u);
    uintptr_t action = 0;
    if (!rangeReadable(actionTable, 24u)
        || !trainingCheckedScaledPointer(
               actionTable, rd32(actor + 48u), 24u, 24u, action)) {
        return;
    }
    const uint64_t frameIndex =
        static_cast<uint64_t>(rd32(action))
        + static_cast<uint64_t>(rd32(actor + 64u));
    uintptr_t frame = 0;
    if (frameIndex > 0xFFFFu
        || !rangeReadable(frameBase, 64u)
        || !trainingCheckedScaledPointer(
               frameBase, frameIndex, 64u, 64u, frame)) {
        return;
    }

    const int32_t actorX = static_cast<int32_t>(rd32(actor + 136u));
    const int32_t actorY = static_cast<int32_t>(rd32(actor + 140u));
    const bool facingLeft = rd8(actor + 170u) != 0u;

    const bool attackLatched =
        rangeReadable(actor + 160u, 2u)
        && qtraining::hitboxAttackActive(
               static_cast<uint16_t>(rd16(actor + 160u)));

    for (const TrainingHitboxCategory& category : kTrainingHitboxCategories) {
        const uint8_t authored = rd8(frame + category.countByte);
        const uint8_t records =
            qtraining::hitboxRecordsToDrawFor(category.source, authored);
        if (records == 0u) continue;

        const uintptr_t recordArray =
            rd32(actor + (category.attackArray ? 40u : 32u));
        const uint32_t stride = category.attackArray ? 24u : 8u;
        const uint64_t first =
            static_cast<uint64_t>(
                rd32(action + (category.attackArray ? 16u : 8u)))
            + static_cast<uint64_t>(rd8(frame + category.offsetByte));
        if (!rangeReadable(recordArray, stride)) continue;

        for (uint8_t i = 0; i < records; ++i) {
            uintptr_t record = 0;
            if (!trainingCheckedScaledPointer(
                    recordArray, first + i, stride, stride, record)) {
                break;
            }
            const int16_t left = static_cast<int16_t>(rd16(record + 0u));
            const int16_t top = static_cast<int16_t>(rd16(record + 2u));
            const int16_t right = static_cast<int16_t>(rd16(record + 4u));
            const int16_t bottom = static_cast<int16_t>(rd16(record + 6u));

            const uint8_t selector =
                category.attackArray ? rd8(record + 18u) : 0u;
            const qtraining::HitboxBoxKind kind =
                qtraining::boxKindForSource(
                    category.source, selector, attackLatched);

            const qtraining::HitboxRect world =
                qtraining::hitboxResolveRect(
                    actorX, actorY, left, top, right, bottom, facingLeft);
            const qtraining::HitboxRect screen =
                qtraining::hitboxToScreen(world, camera);
            if (clashingNow && kind == qtraining::HitboxBoxKind::Clash)
                qtraining::hitboxNoteClashPoint(snapshot, screen);
            (void)qtraining::hitboxSnapshotAppend(
                snapshot, screen, kind, owner);
        }
    }
}

static uint32_t g_trainingHitboxLastFrame = 0xFFFFFFFFu;

static bool g_trainingHitboxRenderPhaseHooked = false;
static bool g_replayHitboxOwner = false;
static bool g_replayHitboxesEnabled = false;
static bool g_replayHitboxSnapshotValid = false;
static qtraining::HitboxSnapshotV1 g_replayHitboxSnapshot{};
static void replayReleaseCustomHitboxes();

static void trainingHitboxDropHeldFrame() {
    g_trainingHitboxLastFrame = 0xFFFFFFFFu;
}

static void trainingPublishHitboxSnapshot(bool fromRenderPhase) {
    if (g_replayRoleActive && !qreplay::replayHitboxPresentationAllowed(
            rd32(kSceneStateAddr), static_cast<int32_t>(rd32(kMatchSubAddr)))) {
        replayReleaseCustomHitboxes();
        return;
    }
    if (g_replayHitboxOwner && currentPresentationDisposition()
            == PresentationDisposition::SuppressHistorical) return;
    const qtraining::HitboxDisplayMode mode = g_replayHitboxOwner
        ? qtraining::HitboxDisplayMode::Custom : g_trainingHitboxMode;
    if (!qtraining::hitboxModeWantsCustomOverlay(mode)) {
        if (g_trainingHitboxOverlayVisible) {
            qtraining::trainingHitboxes().publishHidden();
            g_trainingHitboxOverlayVisible = false;
        }
        trainingHitboxDropHeldFrame();
        return;
    }

    if (g_trainingHitboxRenderPhaseHooked && !fromRenderPhase) return;

    if (!fromRenderPhase) {
        const uint32_t frameCounter = rd32(0x4AB778u);
        if (frameCounter == g_trainingHitboxLastFrame) return;
        g_trainingHitboxLastFrame = frameCounter;
    }

    qtraining::HitboxCameraV1 camera{};
    if (!trainingReadHitboxCamera(camera)) {
        if (g_replayHitboxOwner) g_replayHitboxSnapshotValid = false;
        if (g_trainingHitboxOverlayVisible) {
            qtraining::trainingHitboxes().publishHidden();
            g_trainingHitboxOverlayVisible = false;
        }
        trainingHitboxDropHeldFrame();
        return;
    }

    qtraining::HitboxSnapshotV1 snapshot{};
    snapshot.mode = static_cast<uint32_t>(mode);
    snapshot.visible = 1u;
    snapshot.frameSerial = g_trainingGeneration;

    if (rangeReadable(kTrainingFighterTableAddr, 4u * kTrainingFighterSlots)) {
        for (uint32_t slot = 0; slot < kTrainingFighterSlots; ++slot) {
            const uintptr_t actor =
                rd32(kTrainingFighterTableAddr + 4u * slot);
            if (!actor) continue;
            const uint8_t owner = static_cast<uint8_t>(slot);

            bool clashingNow = false;
            if (rangeReadable(actor + 160u, 2u)) {
                const uint16_t contact =
                    static_cast<uint16_t>(rd16(actor + 160u));
                const uint32_t bit = 1u << slot;
                clashingNow = qtraining::hitboxContactClash(contact);
                if (clashingNow) snapshot.clashMask |= bit;
                if (qtraining::hitboxContactHit(contact))
                    snapshot.hitMask |= bit;
                if (qtraining::hitboxContactGuard(contact))
                    snapshot.guardMask |= bit;
                if (qtraining::hitboxAttackActive(contact))
                    snapshot.activeMask |= bit;
            }

            trainingReadActorBoxes(actor, camera, owner, clashingNow, snapshot);

            if (rangeReadable(actor + kTrainingPartnerPresentOffset, 4u)
                && rd32(actor + kTrainingPartnerPresentOffset) != 0u
                && rangeReadable(kTrainingPartnerTableAddr
                                     + 4u * slot, 4u)) {
                const uintptr_t partner =
                    rd32(kTrainingPartnerTableAddr + 4u * slot);
                trainingReadActorBoxes(partner, camera, owner, clashingNow,
                                       snapshot);
            }
        }
    }

    for (uint32_t i = 0; i < kTrainingClonePoolCount; ++i) {
        const uintptr_t actor = kTrainingClonePoolAddr + kTrainingActorStride * i;
        if (!rangeReadable(actor + 174u, 1u)) break;
        // byte +174 is the type id; -1 is the free-slot marker.
        if (rd8(actor + 174u) == 0xFFu) continue;
        trainingReadActorBoxes(actor, camera, qtraining::kHitboxOwnerPool,
                               false, snapshot);
    }

    if (g_replayHitboxOwner) {
        g_replayHitboxSnapshot = snapshot;
        g_replayHitboxSnapshotValid = true;
        snapshot.visible = g_replayHitboxesEnabled ? 1u : 0u;
    }
    qtraining::trainingHitboxes().publish(snapshot);
    g_trainingHitboxOverlayVisible = snapshot.visible != 0u;
}

static void trainingTryRegisterHitboxOverlay() {
    if (!g_trainingLaunchRequested
        || qtraining::hitboxOverlayRegistered()) {
        return;
    }
    trainingPublishHitboxSnapshot();
    if (!qtraining::hitboxTryRegisterOverlay()) return;
    QLOG("[training-ui] hitbox overlay provider registered api=%u "
         "snapshot=%zu boxes=%u virtual=%dx%d",
         qtraining::kHitboxApiVersion,
         sizeof(qtraining::HitboxSnapshotV1),
         qtraining::kHitboxSnapshotMaxBoxes,
         static_cast<int>(qtraining::kHitboxVirtualWidth),
         static_cast<int>(qtraining::kHitboxVirtualHeight));
}

using TrainingPreBoxRenderFn = int (*)();
static TrainingPreBoxRenderFn g_trainingOrigPreBoxRender = nullptr;
static void trainingRemoveCreatedHook(uintptr_t address);

static int hkTrainingPreBoxRender() {
    const int result = g_trainingOrigPreBoxRender
        ? g_trainingOrigPreBoxRender()
        : 0;
    trainingPublishHitboxSnapshot(true);
    return result;
}

static void trainingInstallRenderPhaseHook() {
    if (g_trainingHitboxRenderPhaseHooked) return;
    static const uint8_t kPreBoxRenderSig[] = {
        0x8B, 0x0D, 0xD8, 0x67, 0x62, 0x00, 0x83, 0xEC,
        0x14, 0x53, 0x33, 0xDB, 0x3B, 0xCB,
    };
    void* target = reinterpret_cast<void*>(kTrainingPreBoxRenderFn);
    if (!se::verifyCodeBytes(kTrainingPreBoxRenderFn, kPreBoxRenderSig,
                             sizeof(kPreBoxRenderSig))) {
        QLOG("[training-ui] box-overlay render phase unavailable: prologue "
             "mismatch at sub_419C70 - overlay falls back to the outer tick "
             "and will trail the picture by one frame");
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hkTrainingPreBoxRender),
                      reinterpret_cast<void**>(&g_trainingOrigPreBoxRender))
            != MH_OK
        || MH_QueueEnableHook(target) != MH_OK) {
        g_trainingOrigPreBoxRender = nullptr;
        trainingRemoveCreatedHook(kTrainingPreBoxRenderFn);
        QLOG("[training-ui] box-overlay render phase could not be hooked - "
             "overlay falls back to the outer tick");
        return;
    }
    g_trainingHitboxRenderPhaseHooked = true;
    trainingHitboxDropHeldFrame();
    QLOG("[training-ui] box overlay reads at the game's own draw phase "
         "(sub_419C70); geometry matches the presented frame exactly");
}

static void replayInstallCustomHitboxes() {
    g_replayHitboxOwner = true;
    trainingInstallRenderPhaseHook();
}

static void replaySetCustomHitboxes(bool enabled) {
    if (rd8(kTrainingHitboxAddr) != 0u) wr8(kTrainingHitboxAddr, 0u);
    if (!qreplay::replayHitboxPresentationAllowed(
            rd32(kSceneStateAddr), static_cast<int32_t>(rd32(kMatchSubAddr)))) {
        replayReleaseCustomHitboxes();
        return;
    }
    if (!g_replayHitboxOwner) replayInstallCustomHitboxes();
    const bool changed = enabled != g_replayHitboxesEnabled;
    g_replayHitboxesEnabled = enabled;
    if (rd32(kSceneStateAddr) != 2u) {
        g_replayHitboxSnapshotValid = false;
        if (g_trainingHitboxOverlayVisible) {
            qtraining::trainingHitboxes().publishHidden();
            g_trainingHitboxOverlayVisible = false;
        }
    } else if (changed) {
        if (enabled && g_replayHitboxSnapshotValid) {
            qtraining::trainingHitboxes().publish(g_replayHitboxSnapshot);
            g_trainingHitboxOverlayVisible = true;
        } else {
            qtraining::trainingHitboxes().publishHidden();
            g_trainingHitboxOverlayVisible = false;
        }
    }
    qtraining::hitboxTryRegisterOverlay();
}

static void replayReleaseCustomHitboxes() {
    if (rd8(kTrainingHitboxAddr) != 0u) wr8(kTrainingHitboxAddr, 0u);
    g_replayHitboxOwner = false;
    g_replayHitboxesEnabled = false;
    g_replayHitboxSnapshotValid = false;
    g_trainingHitboxOverlayVisible = false;
    qtraining::trainingHitboxes().publishHidden();
    qtraining::hitboxReleaseOverlay();
    trainingHitboxDropHeldFrame();
}

static void trainingTryRegisterInputDisplay() {
    if (!g_trainingLaunchRequested
        || qinput::inputDisplayOverlayRegistered()) {
        return;
    }
    if (!qinput::inputDisplayTryRegisterOverlay()) return;
    QLOG("[training-ui] input display provider registered api=%u "
         "snapshot=%zu lanes=%u history=%u",
         qinput::kInputDisplayApiVersion,
         sizeof(qinput::InputDisplaySnapshotV1),
         qinput::kInputDisplayLaneCount,
         qinput::kInputDisplayHistoryCapacity);
}

static bool trainingVerifyNativeSeams() {
    static const uint8_t kMatchScopeSig[] = {
        0x81, 0xEC, 0x3C, 0x03, 0x00, 0x00,
        0xA1, 0x50, 0xAA, 0x4A, 0x00,
    };
    static const uint8_t kStepGateSig[] = {
        0xA1, 0x50, 0xAA, 0x4A, 0x00, 0x53,
        0xBB, 0x01, 0x00, 0x00, 0x00, 0x56,
    };
    static const uint8_t kDamageSig[] = {
        0x56, 0x8B, 0xF2, 0x33, 0xC0, 0x57, 0x8A,
        0x86, 0xAC, 0x00, 0x00, 0x00, 0x8B, 0xF9,
    };
    static const uint8_t kSelectorConfirmSig[] = {
        0x83, 0xEC, 0x1C, 0x53, 0x55, 0x56, 0x8B, 0xF1,
        0x33, 0xD2, 0x57, 0x8D, 0x04, 0xF5,
        0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t kResetSig[] = {
        0x53, 0x55, 0x56, 0x57, 0xE8, 0x17, 0x3A,
        0x05, 0x00, 0xA3, 0x1C, 0xA4, 0x4A, 0x00,
    };
    static const uint8_t kCollisionSig[] = {
        0x8A, 0x81, 0xAA, 0x00, 0x00, 0x00, 0x56,
        0x84, 0xC0, 0x8B, 0x41, 0x20, 0x57,
    };
    static const uint8_t kFacingSig[] = {
        0x83, 0xEC, 0x20, 0x53, 0x56, 0x8B, 0xF2,
        0x57, 0x8A, 0x9E, 0xAA, 0x00, 0x00, 0x00,
    };
    static const uint8_t kClampSig[] = {
        0x83, 0xEC, 0x20, 0x8B, 0xC2, 0x53, 0x55,
        0x56, 0x8B, 0xF1, 0x57, 0x8B, 0x4E, 0x30,
    };
    static const uint8_t kComboTeardownSig[] = {
        0x8B, 0xC1, 0x53, 0x33, 0xC9, 0x56, 0x8A,
        0x88, 0xAC, 0x00, 0x00, 0x00, 0x57, 0x8B,
    };
    static const uint8_t kActorUnlinkSig[] = {
        0x53, 0x56, 0x57, 0xBA, 0x00, 0x3E, 0x61,
        0x00, 0x8B, 0x02, 0x8B, 0xF2, 0x85, 0xC0,
    };
    return se::verifyCodeBytes(
               kTrainingMatchScopeFn,
               kMatchScopeSig, sizeof(kMatchScopeSig))
        && se::verifyCodeBytes(
               kTrainingStepGateFn,
               kStepGateSig, sizeof(kStepGateSig))
        && se::verifyCodeBytes(
               kTrainingDamageFn,
               kDamageSig, sizeof(kDamageSig))
        && se::verifyCodeBytes(
               kTrainingSelectorConfirmFn,
               kSelectorConfirmSig,
               sizeof(kSelectorConfirmSig))
        && se::verifyCodeBytes(
               kTrainingResetFn,
               kResetSig, sizeof(kResetSig))
        && se::verifyCodeBytes(
               kTrainingCollisionRectFn,
               kCollisionSig, sizeof(kCollisionSig))
        && se::verifyCodeBytes(
               kTrainingFacingFlipFn,
               kFacingSig, sizeof(kFacingSig))
        && se::verifyCodeBytes(
               kTrainingWorldClampFn,
               kClampSig, sizeof(kClampSig))
        && se::verifyCodeBytes(
               kTrainingComboTeardownAddr,
               kComboTeardownSig, sizeof(kComboTeardownSig))
        && se::verifyCodeBytes(
               kTrainingActorUnlinkAddr,
               kActorUnlinkSig, sizeof(kActorUnlinkSig));
}

static bool trainingVerifyGuardScanners() {
    static const uint8_t kThreatNowSig[] = {
        0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56, 0x57,
        0x89, 0x54, 0x24, 0x18, 0x89, 0x4C, 0x24, 0x10,
    };
    static const uint8_t kThreatNextSig[] = {
        0x83, 0xEC, 0x08, 0x8B, 0x02, 0x53, 0x55,
        0x83, 0xF8, 0xFF, 0x89, 0x4C, 0x24, 0x08,
    };
    return se::verifyCodeBytes(
               kTrainingThreatNowFn,
               kThreatNowSig, sizeof(kThreatNowSig))
        && se::verifyCodeBytes(
               kTrainingThreatNextFn,
               kThreatNextSig, sizeof(kThreatNextSig));
}

static void trainingRemoveCreatedHook(uintptr_t address) {
    void* target = reinterpret_cast<void*>(address);
    (void)MH_DisableHook(target);
    (void)MH_RemoveHook(target);
}

static bool trainingReadImageBytes(void* , uint32_t va,
                                   uint8_t* out, uint32_t count) {
    if (out == nullptr || count == 0) return false;
    const uintptr_t address = static_cast<uintptr_t>(va);
    if (!rangeReadable(address, count)) return false;
    std::memcpy(out, reinterpret_cast<const void*>(address), count);
    return true;
}

static bool trainingResolveQualifiedHook(
        generated_model::FunctionId functionId,
        generated_model::NativeSymbolId symbolId,
        const char* label, uintptr_t& outVa) {
    outVa = 0u;
    const generated_model::BuildProfile* profile =
        runtime_profile::selectedProfile();
    if (profile == nullptr) return false;
    std::string error;
    if (!runtime_profile::resolveSelectedFunction(
            functionId, outVa, error) || outVa == 0u) {
        QLOG("[training] %s address resolution failed: %s",
             label, error.c_str());
        return false;
    }
    const generated_model::NativeSymbol* symbol =
        generated_model::findNativeSymbol(symbolId);
    if (symbol == nullptr || symbol->functionId != functionId
        || generated_model::resolveNativeSymbolVa(profile->id, symbolId)
               != static_cast<uint32_t>(outVa)) {
        QLOG("[training] %s catalog/profile disagreement at 0x%08X",
             label, static_cast<unsigned>(outVa));
        return false;
    }
    const generated_model::PrologueSignature* signature = nullptr;
    switch (profile->id) {
        case generated_model::BuildId::Game2000:
            signature = &symbol->decemberPrologue;
            break;
        case generated_model::BuildId::Se2006:
            signature = &symbol->novemberPrologue;
            break;
    }
    if (signature == nullptr
        || signature->status
               != generated_model::PrologueSignatureStatus::Curated
        || signature->bytes == nullptr || signature->size == 0u
        || !rangeReadable(outVa, signature->size)
        || std::memcmp(reinterpret_cast<const void*>(outVa),
                       signature->bytes, signature->size) != 0) {
        QLOG("[training] %s prologue qualification failed at 0x%08X",
             label, static_cast<unsigned>(outVa));
        outVa = 0u;
        return false;
    }
    return true;
}

static bool trainingResolveQualifiedNativeHook(
        generated_model::NativeSymbolId symbolId,
        const char* label, uintptr_t& outVa) {
    outVa = 0u;
    const generated_model::BuildProfile* profile =
        runtime_profile::selectedProfile();
    const generated_model::NativeSymbol* symbol =
        generated_model::findNativeSymbol(symbolId);
    if (profile == nullptr || symbol == nullptr
        || symbol->kind
               != generated_model::NativeSymbolKind::FunctionEntry
        || symbol->shiftClass
               != generated_model::NativeShiftClass::SemanticallyEdited
        || symbol->runtimeUse
               != generated_model::NativeRuntimeUse::SeamResolved) {
        QLOG("[training] %s catalog qualification refused", label);
        return false;
    }
    outVa = static_cast<uintptr_t>(
        generated_model::resolveNativeSymbolVa(profile->id, symbolId));
    const generated_model::PrologueSignature* signature = nullptr;
    switch (profile->id) {
        case generated_model::BuildId::Game2000:
            signature = &symbol->decemberPrologue;
            break;
        case generated_model::BuildId::Se2006:
            signature = &symbol->novemberPrologue;
            break;
    }
    if (outVa == 0u || signature == nullptr
        || signature->status
               != generated_model::PrologueSignatureStatus::Curated
        || signature->bytes == nullptr || signature->size == 0u
        || !rangeReadable(outVa, signature->size)
        || std::memcmp(reinterpret_cast<const void*>(outVa),
                       signature->bytes, signature->size) != 0) {
        QLOG("[training] %s prologue qualification failed at 0x%08X",
             label, static_cast<unsigned>(outVa));
        outVa = 0u;
        return false;
    }
    return true;
}

static void trainingQualifyRecoveryEvaluator() {
    const qoh99::recovery::Report report =
        qoh99::recovery::qualifyRecoveryEvaluator(&trainingReadImageBytes,
                                                  nullptr);
    g_trainingRecoveryEvaluatorQualified =
        report.addressesResolved && report.prologuesMatched;
    g_trainingRecoveryEvaluatorReason = qtraining::recoveryEvaluatorReasonFor(
        true, report.addressesResolved, report.prologuesMatched);
    g_trainingRecoveryEvaluatorCompared = report.comparedPrologues;
    QLOG("[training] recovery evaluator qualification: profile=%s resolved=%d "
         "prologues=%d compared=%u reason=%u failure=%u (%s) at %s 0x%08X",
         runtime_profile::selectedProfileKey(),
         report.addressesResolved ? 1 : 0,
         report.prologuesMatched ? 1 : 0,
         report.comparedPrologues,
         static_cast<unsigned>(g_trainingRecoveryEvaluatorReason),
         static_cast<unsigned>(report.failure),
         qoh99::recovery::qualificationFailureText(report.failure),
         report.failedLabel != nullptr ? report.failedLabel : "-",
         static_cast<unsigned>(report.failedVa));
}

static bool trainingContactPolicyActive() {
    return g_trainingLaunchRequested
        && g_trainingLifecycle == TrainingLifecycleState::Active
        && !g_trainingMutationFailed
        && onClockThread();
}

static bool trainingGuardCrushLatchSet(uintptr_t defender) {
    if (defender == 0u) return false;
    const uint32_t slot = rd8(defender + 172u);
    if (slot > 1u) return false;
    return rd32(kTrainingGuardCrushLatchBase + 44u * slot) == 1u;
}

static void trainingClearGuardCrushLatch(uintptr_t defender) {
    if (defender == 0u) return;
    const uint32_t slot = rd8(defender + 172u);
    if (slot > 1u) return;
    wr32(kTrainingGuardCrushLatchBase + 44u * slot, 0u);
}

static uint32_t trainingDefenderComboHits(uintptr_t defender) {
    if (defender == 0u) return 0u;
    const uint32_t slot = rd8(defender + 172u);
    if (slot > 1u) return 0u;
    return rd32(kTrainingComboCountBase + 44u * slot);
}

static int __fastcall hkTrainingCriticalScaler(
    int attacker, int defender, int* damage, uint32_t* criticalFlag) {
    if (!trainingContactPolicyActive() || g_trainingOrigCriticalScaler == nullptr) {
        return g_trainingOrigCriticalScaler
            ? g_trainingOrigCriticalScaler(attacker, defender, damage,
                                           criticalFlag)
            : 0;
    }

    static bool loggedCriticalEntry = false;
    if (!loggedCriticalEntry) {
        loggedCriticalEntry = true;
        QLOG("[training-contact] critical scaler reached: rngHooked=%d",
             g_rngHooked ? 1 : 0);
    }

    const auto attackerAddr = static_cast<uintptr_t>(attacker);
    const auto defenderAddr = static_cast<uintptr_t>(defender);
    const uint32_t attackerSlot = attackerAddr != 0u
        ? rd8(attackerAddr + 172u) : 0u;
    const qtraining::TrainingCriticalPolicy policy =
        qtraining::criticalPolicyForAttackerSlot(
            g_trainingContactPolicyLatched, attackerSlot);
    const bool latch = trainingGuardCrushLatchSet(defenderAddr);

    // Clear before the original runs: the native integer scale cannot be undone afterwards.
    if (qtraining::criticalPolicyClearsGuardCrushLatch(policy, latch)) {
        trainingClearGuardCrushLatch(defenderAddr);
    }

    TrainingCriticalContext& ctx = t_trainingCriticalContext;
    const TrainingCriticalContext saved = ctx;
    ctx.depth += 1u;
    ctx.consumed = false;
    ctx.override = qtraining::criticalRollOverrideFor(policy, latch);
    ctx.attackerSlot = attackerSlot;
    ctx.incomingHit = trainingDefenderComboHits(defenderAddr) + 1u;
    ctx.divisor = qtraining::criticalDivisorForIncomingHit(ctx.incomingHit);
    ctx.guardCrushLatch = latch;
    ctx.nativeSample = 0u;
    ctx.finalSample = 0u;

    const int damageBefore = damage != nullptr ? *damage : 0;
    const int result =
        g_trainingOrigCriticalScaler(attacker, defender, damage, criticalFlag);

    if (ctx.consumed) {
        g_trainingLastCritical.valid = true;
        g_trainingLastCritical.attackerSlot = attackerSlot;
        g_trainingLastCritical.incomingHit = ctx.incomingHit;
        g_trainingLastCritical.divisor = ctx.divisor;
        g_trainingLastCritical.nativeSample = ctx.nativeSample;
        g_trainingLastCritical.finalSample = ctx.finalSample;
        g_trainingLastCritical.guardCrushLatch = latch ? 1u : 0u;
        g_trainingLastCritical.finalFlag =
            criticalFlag != nullptr ? *criticalFlag : 0u;
        g_trainingLastCritical.damageBefore =
            static_cast<uint32_t>(damageBefore);
        g_trainingLastCritical.damageAfter =
            damage != nullptr ? static_cast<uint32_t>(*damage) : 0u;
        g_trainingLastCritical.policy = static_cast<uint32_t>(policy);
    }
    ctx = saved;
    return result;
}

static bool trainingCriticalContextArmed() {
    const TrainingCriticalContext& ctx = t_trainingCriticalContext;
    return ctx.depth != 0u && !ctx.consumed;
}

static bool trainingCriticalRollOverride(uint32_t nativeSample,
                                         uint32_t& outSample) {
    TrainingCriticalContext& ctx = t_trainingCriticalContext;
    if (ctx.depth == 0u || ctx.consumed) return false;
    ctx.consumed = true;
    ctx.nativeSample = nativeSample;
    ctx.finalSample =
        qtraining::criticalSampleForOverride(ctx.override, nativeSample);
    outSample = ctx.finalSample;
    return ctx.finalSample != nativeSample;
}

static int __fastcall hkTrainingFaultlessDetector(int fighter, int edx) {
    const int nativeResult = g_trainingOrigFaultlessDetector != nullptr
        ? g_trainingOrigFaultlessDetector(fighter, edx) : 0;
    static bool loggedEntry = false;
    if (!loggedEntry) {
        loggedEntry = true;
        QLOG("[training-contact] Faultless detector reached: slot=%u actor=%u "
             "active=%d", rd8(static_cast<uintptr_t>(fighter) + 172u),
             rd8(static_cast<uintptr_t>(fighter) + 174u),
             trainingContactPolicyActive() ? 1 : 0);
    }
    if (!trainingContactPolicyActive()) return nativeResult;
    const auto addr = static_cast<uintptr_t>(fighter);
    if (addr == 0u || rd8(addr + 172u) != 1u) return nativeResult;

    const uint32_t actor = rd8(addr + 174u);
    const qtraining::TrainingFaultlessPolicy policy =
        g_trainingContactPolicyLatched.faultless;
    int finalResult = nativeResult;
    switch (policy) {
        case qtraining::TrainingFaultlessPolicy::Never:
            finalResult = 0;
            break;
        case qtraining::TrainingFaultlessPolicy::AlwaysWhenLegal:
            finalResult = static_cast<int>(
                qtraining::faultlessFamilyModeForActor(actor));
            break;
        case qtraining::TrainingFaultlessPolicy::NativeInput:
        default:
            break;
    }
    g_trainingLastFaultless.valid = true;
    g_trainingLastFaultless.nativeMode = static_cast<uint32_t>(nativeResult);
    g_trainingLastFaultless.finalMode = static_cast<uint32_t>(finalResult);
    g_trainingLastFaultless.policy = static_cast<uint32_t>(policy);
    return finalResult;
}

static int __fastcall hkTrainingReduceDetector(int mode, int actor) {
    const int nativeVerdict = g_trainingOrigReduceDetector != nullptr
        ? g_trainingOrigReduceDetector(mode, actor) : 0;
    if (mode != 1) return nativeVerdict;
    static bool loggedEntry = false;
    if (!loggedEntry) {
        loggedEntry = true;
        QLOG("[training-contact] Reduce detector reached: victim=%u "
             "native=%d active=%d",
             rd8(static_cast<uintptr_t>(actor) + 172u), nativeVerdict,
             trainingContactPolicyActive() ? 1 : 0);
    }
    if (!trainingContactPolicyActive()) return nativeVerdict;
    const auto addr = static_cast<uintptr_t>(actor);
    if (addr == 0u) return nativeVerdict;
    const uint32_t victimSlot = rd8(addr + 172u);
    if (victimSlot > 1u) return nativeVerdict;

    const qtraining::TrainingReducePolicy policy =
        qtraining::reducePolicyForVictimSlot(
            g_trainingContactPolicyLatched, victimSlot);
    const int finalVerdict =
        qtraining::reduceVerdictForPolicy(policy, nativeVerdict);
    g_trainingLastReduce.valid = true;
    g_trainingLastReduce.victimSlot = victimSlot;
    g_trainingLastReduce.nativeVerdict =
        static_cast<uint32_t>(nativeVerdict);
    g_trainingLastReduce.finalVerdict = static_cast<uint32_t>(finalVerdict);
    g_trainingLastReduce.policy = static_cast<uint32_t>(policy);
    return finalVerdict;
}

static int __fastcall hkTrainingGuardActionRequest(
    int fighter, int edx, char mode) {
    const auto addr = static_cast<uintptr_t>(fighter);
    const uint32_t priorRequest = addr != 0u ? rd32(addr + 72u) : 0u;
    const uint32_t priorPending = addr != 0u ? rd32(addr + 76u) : 0u;

    const int nativeResult = g_trainingOrigGuardActionRequest != nullptr
        ? g_trainingOrigGuardActionRequest(fighter, edx, mode) : 0;

    if ((static_cast<unsigned char>(mode) & 1u) != 0u) {
        static bool loggedParryEntry = false;
        if (!loggedParryEntry) {
            loggedParryEntry = true;
            QLOG("[training-contact] parry matcher reached: slot=%u actor=%u "
                 "native=%d active=%d", rd8(addr + 172u), rd8(addr + 174u),
                 nativeResult, trainingContactPolicyActive() ? 1 : 0);
        }
    }
    if (!trainingContactPolicyActive()) return nativeResult;
    if ((static_cast<unsigned char>(mode) & 1u) == 0u) return nativeResult;
    if (addr == 0u || rd8(addr + 172u) != 1u) return nativeResult;

    const qtraining::TrainingParryPolicy policy =
        g_trainingContactPolicyLatched.parry;
    int finalResult = nativeResult;
    switch (policy) {
        case qtraining::TrainingParryPolicy::AlwaysWhenLegal:
            wr32(addr + 72u, qtraining::kParryMatcherActionRequest);
            wr32(addr + 76u, qtraining::kParryMatcherPendingFlag);
            finalResult =
                static_cast<int>(qtraining::kParryMatcherAcceptedResult);
            break;
        case qtraining::TrainingParryPolicy::Never:
            if (static_cast<uint32_t>(nativeResult)
                == qtraining::kParryMatcherAcceptedResult) {
                wr32(addr + 72u, priorRequest);
                wr32(addr + 76u, priorPending);
                finalResult = priorPending != 0u ? 1 : 0;
            }
            break;
        case qtraining::TrainingParryPolicy::NativeInput:
        default:
            break;
    }
    g_trainingLastParry.valid = true;
    g_trainingLastParry.nativeResult = static_cast<uint32_t>(nativeResult);
    g_trainingLastParry.finalResult = static_cast<uint32_t>(finalResult);
    g_trainingLastParry.policy = static_cast<uint32_t>(policy);
    return finalResult;
}

static void trainingInstallContactPolicyHooks() {
    uintptr_t resolverVa = 0u;
    const bool resolverOk = trainingResolveQualifiedHook(
        generated_model::FunctionId::AttackContactResolver,
        generated_model::NativeSymbolId::AttackContactResolver,
        "attack contact resolver", resolverVa);

    if (resolverOk
        && trainingResolveQualifiedHook(
               generated_model::FunctionId::FaultlessButtonDetector,
               generated_model::NativeSymbolId::FaultlessButtonDetector,
               "faultless button detector", g_trainingFaultlessDetectorFn)
        && MH_CreateHook(
               reinterpret_cast<void*>(g_trainingFaultlessDetectorFn),
               reinterpret_cast<void*>(&hkTrainingFaultlessDetector),
               reinterpret_cast<void**>(&g_trainingOrigFaultlessDetector))
           == MH_OK
        && MH_QueueEnableHook(
               reinterpret_cast<void*>(g_trainingFaultlessDetectorFn))
           == MH_OK) {
        g_trainingFaultlessHookReady = true;
    } else {
        g_trainingFaultlessDetectorFn = 0u;
        QLOG("[training] Faultless policy unavailable on this build");
    }

    if (resolverOk
        && trainingResolveQualifiedHook(
               generated_model::FunctionId::GuardActionRequest,
               generated_model::NativeSymbolId::GuardActionRequest,
               "guard action request", g_trainingGuardActionRequestFn)
        && MH_CreateHook(
               reinterpret_cast<void*>(g_trainingGuardActionRequestFn),
               reinterpret_cast<void*>(&hkTrainingGuardActionRequest),
               reinterpret_cast<void**>(&g_trainingOrigGuardActionRequest))
           == MH_OK
        && MH_QueueEnableHook(
               reinterpret_cast<void*>(g_trainingGuardActionRequestFn))
           == MH_OK) {
        g_trainingParryHookReady = true;
    } else {
        g_trainingGuardActionRequestFn = 0u;
        QLOG("[training] Parry policy unavailable on this build");
    }

    if (trainingResolveQualifiedHook(
            generated_model::FunctionId::DamageCriticalScaler,
            generated_model::NativeSymbolId::DamageCriticalScaler,
            "damage critical scaler", g_trainingCriticalScalerFn)
        && requestTrainingCriticalRngHook()
        && MH_CreateHook(
               reinterpret_cast<void*>(g_trainingCriticalScalerFn),
               reinterpret_cast<void*>(&hkTrainingCriticalScaler),
               reinterpret_cast<void**>(&g_trainingOrigCriticalScaler))
           == MH_OK
        && MH_QueueEnableHook(
               reinterpret_cast<void*>(g_trainingCriticalScalerFn)) == MH_OK) {
        g_trainingCriticalHookReady = true;
    } else {
        g_trainingCriticalScalerFn = 0u;
        QLOG("[training] Critical policy unavailable on this build");
    }

    if (trainingResolveQualifiedHook(
            generated_model::FunctionId::ReduceMashDetector,
            generated_model::NativeSymbolId::ReduceMashDetector,
            "reduce mash detector", g_trainingReduceDetectorFn)
        && MH_CreateHook(
               reinterpret_cast<void*>(g_trainingReduceDetectorFn),
               reinterpret_cast<void*>(&hkTrainingReduceDetector),
               reinterpret_cast<void**>(&g_trainingOrigReduceDetector))
           == MH_OK
        && MH_QueueEnableHook(
               reinterpret_cast<void*>(g_trainingReduceDetectorFn))
           == MH_OK) {
        g_trainingReduceHookReady = true;
    } else {
        g_trainingReduceDetectorFn = 0u;
        QLOG("[training] Reduce policy unavailable on this build");
    }

    if (MH_ApplyQueued() != MH_OK) {
        g_trainingFaultlessHookReady = false;
        g_trainingParryHookReady = false;
        g_trainingCriticalHookReady = false;
        g_trainingReduceHookReady = false;
        QLOG("[training] contact-policy hook apply FAILED; all four "
             "capabilities stay unavailable and behaviour stays native");
    }
    QLOG("[training] contact policy: faultless=%d parry=%d critical=%d "
         "reduce=%d",
         g_trainingFaultlessHookReady ? 1 : 0,
         g_trainingParryHookReady ? 1 : 0,
         g_trainingCriticalHookReady ? 1 : 0,
         g_trainingReduceHookReady ? 1 : 0);
}

static bool installTrainingHooksOnce() {
    if (!g_trainingLaunchRequested)
        return true;
    if (g_trainingHooksInstalled)
        return true;
    g_trainingHooksSupported = trainingVerifyNativeSeams();
    g_trainingGuardScannerProloguesMatch =
        trainingVerifyGuardScanners();
    if (!g_trainingHooksSupported) {
        g_trainingDisableReason =
            TrainingDisableReason::UnsupportedStepBoundary;
        trainingCopyText(
            g_trainingDisableText,
            "Unsupported training boundary or native helper");
        QLOG("[training] native seam qualification FAILED; "
             "all mutation disabled");
        trainingPublishStatus();
        return false;
    }

    const bool combatHooksQualified =
        trainingResolveQualifiedNativeHook(
            generated_model::NativeSymbolId::EditedActorTransition,
            "throw-pair installer", g_trainingThrowInstallFn)
        && trainingResolveQualifiedHook(
            generated_model::FunctionId::ThrowTechEvaluator,
            generated_model::NativeSymbolId::ThrowTechEvaluator,
            "throw-tech evaluator", g_trainingThrowTechFn)
        && trainingResolveQualifiedHook(
            generated_model::FunctionId::GuardContactResolver,
            generated_model::NativeSymbolId::GuardContactResolver,
            "guard contact resolver", g_trainingGuardContactFn)
        && trainingResolveQualifiedHook(
            generated_model::FunctionId::HitContactResolver,
            generated_model::NativeSymbolId::HitContactResolver,
            "hit contact resolver", g_trainingHitContactFn)
        && trainingResolveQualifiedHook(
            generated_model::FunctionId::NaturalActionTerminal,
            generated_model::NativeSymbolId::NaturalActionTerminal,
            "natural action completion", g_trainingNaturalActionTerminalFn);
    if (!combatHooksQualified) {
        g_trainingThrowInstallFn = 0u;
        g_trainingThrowTechFn = 0u;
        g_trainingGuardContactFn = 0u;
        g_trainingHitContactFn = 0u;
        g_trainingNaturalActionTerminalFn = 0u;
        g_trainingHooksSupported = false;
        g_trainingDisableReason =
            TrainingDisableReason::UnsupportedStepBoundary;
        trainingCopyText(
            g_trainingDisableText,
            "Training combat helpers do not match this game build");
        QLOG("[training] combat helper qualification FAILED; "
             "all mutation disabled");
        trainingPublishStatus();
        return false;
    }

    bool matchCreated = false;
    bool stepCreated = false;
    bool damageCreated = false;
    bool selectorConfirmCreated = false;
    bool throwInstallCreated = false;
    bool throwTechCreated = false;
    bool guardContactCreated = false;
    bool hitContactCreated = false;
    bool naturalTerminalCreated = false;
    if (MH_CreateHook(
            reinterpret_cast<void*>(kTrainingMatchScopeFn),
            reinterpret_cast<void*>(&hkTrainingMatchScope),
            reinterpret_cast<void**>(
                &g_trainingOrigMatchScope)) == MH_OK) {
        matchCreated = true;
    }
    if (matchCreated
        && MH_CreateHook(
            reinterpret_cast<void*>(kTrainingStepGateFn),
            reinterpret_cast<void*>(&hkTrainingStepGate),
            reinterpret_cast<void**>(
                &g_trainingOrigStepGate)) == MH_OK) {
        stepCreated = true;
    }
    if (stepCreated
        && MH_CreateHook(
            reinterpret_cast<void*>(kTrainingDamageFn),
            reinterpret_cast<void*>(&hkTrainingDamage),
            reinterpret_cast<void**>(
                &g_trainingOrigDamage)) == MH_OK) {
        damageCreated = true;
    }
    if (damageCreated
        && MH_CreateHook(
            reinterpret_cast<void*>(kTrainingSelectorConfirmFn),
            reinterpret_cast<void*>(&hkTrainingSelectorConfirm),
            reinterpret_cast<void**>(
                &g_trainingOrigSelectorConfirm)) == MH_OK) {
        selectorConfirmCreated = true;
    }
    if (selectorConfirmCreated
        && MH_CreateHook(
            reinterpret_cast<void*>(g_trainingThrowInstallFn),
            reinterpret_cast<void*>(&hkTrainingThrowInstall),
            reinterpret_cast<void**>(
                &g_trainingOrigThrowInstall)) == MH_OK) {
        throwInstallCreated = true;
    }
    if (throwInstallCreated
        && MH_CreateHook(
            reinterpret_cast<void*>(g_trainingThrowTechFn),
            reinterpret_cast<void*>(&hkTrainingThrowTech),
            reinterpret_cast<void**>(
                &g_trainingOrigThrowTech)) == MH_OK) {
        throwTechCreated = true;
    }
    if (throwTechCreated
        && MH_CreateHook(
            reinterpret_cast<void*>(g_trainingGuardContactFn),
            reinterpret_cast<void*>(&hkTrainingGuardContact),
            reinterpret_cast<void**>(
                &g_trainingOrigGuardContact)) == MH_OK) {
        guardContactCreated = true;
    }
    if (guardContactCreated
        && MH_CreateHook(
            reinterpret_cast<void*>(g_trainingHitContactFn),
            reinterpret_cast<void*>(&hkTrainingHitContact),
            reinterpret_cast<void**>(
                &g_trainingOrigHitContact)) == MH_OK) {
        hitContactCreated = true;
    }
    if (hitContactCreated
        && MH_CreateHook(
            reinterpret_cast<void*>(g_trainingNaturalActionTerminalFn),
            reinterpret_cast<void*>(&hkTrainingNaturalActionTerminal),
            reinterpret_cast<void**>(
                &g_trainingOrigNaturalActionTerminal)) == MH_OK) {
        naturalTerminalCreated = true;
    }
    // Queue, never enable: installHooks applies the whole hook set with one MH_ApplyQueued while the main thread is suspended.
    bool enabled = naturalTerminalCreated;
    if (enabled) {
        enabled =
            MH_QueueEnableHook(
                reinterpret_cast<void*>(
                    kTrainingMatchScopeFn)) == MH_OK
            && MH_QueueEnableHook(
                reinterpret_cast<void*>(
                    kTrainingStepGateFn)) == MH_OK
            && MH_QueueEnableHook(
                reinterpret_cast<void*>(
                    kTrainingDamageFn)) == MH_OK
            && MH_QueueEnableHook(
                reinterpret_cast<void*>(
                    kTrainingSelectorConfirmFn)) == MH_OK
            && MH_QueueEnableHook(
                reinterpret_cast<void*>(
                    g_trainingThrowInstallFn)) == MH_OK
            && MH_QueueEnableHook(
                reinterpret_cast<void*>(
                    g_trainingThrowTechFn)) == MH_OK
            && MH_QueueEnableHook(
                reinterpret_cast<void*>(
                    g_trainingGuardContactFn)) == MH_OK
            && MH_QueueEnableHook(
                reinterpret_cast<void*>(
                    g_trainingHitContactFn)) == MH_OK
            && MH_QueueEnableHook(
                reinterpret_cast<void*>(
                    g_trainingNaturalActionTerminalFn)) == MH_OK;
    }
    if (!enabled) {
        if (naturalTerminalCreated)
            trainingRemoveCreatedHook(g_trainingNaturalActionTerminalFn);
        if (hitContactCreated)
            trainingRemoveCreatedHook(g_trainingHitContactFn);
        if (guardContactCreated)
            trainingRemoveCreatedHook(g_trainingGuardContactFn);
        if (throwTechCreated)
            trainingRemoveCreatedHook(g_trainingThrowTechFn);
        if (throwInstallCreated)
            trainingRemoveCreatedHook(g_trainingThrowInstallFn);
        if (selectorConfirmCreated)
            trainingRemoveCreatedHook(kTrainingSelectorConfirmFn);
        if (damageCreated)
            trainingRemoveCreatedHook(kTrainingDamageFn);
        if (stepCreated)
            trainingRemoveCreatedHook(kTrainingStepGateFn);
        if (matchCreated)
            trainingRemoveCreatedHook(kTrainingMatchScopeFn);
        g_trainingOrigMatchScope = nullptr;
        g_trainingOrigStepGate = nullptr;
        g_trainingOrigDamage = nullptr;
        g_trainingOrigSelectorConfirm = nullptr;
        g_trainingOrigThrowInstall = nullptr;
        g_trainingOrigThrowTech = nullptr;
        g_trainingOrigGuardContact = nullptr;
        g_trainingOrigHitContact = nullptr;
        g_trainingOrigNaturalActionTerminal = nullptr;
        g_trainingThrowTechHookReady = false;
        g_trainingContactHooksReady = false;
        g_trainingHooksSupported = false;
        g_trainingDisableReason =
            TrainingDisableReason::UnsupportedStepBoundary;
        trainingCopyText(
            g_trainingDisableText,
            "Training hooks could not be installed transactionally");
        QLOG("[training] native hook transaction FAILED");
        trainingPublishStatus();
        return false;
    }
    g_trainingThrowTechHookReady = true;
    g_trainingContactHooksReady = true;
    g_trainingHooksInstalled = true;
    g_trainingDisableReason = TrainingDisableReason::None;
    g_trainingLifecycle =
        TrainingLifecycleState::LaunchRequested;
    trainingQualifyRecoveryEvaluator();
    g_trainingRecoverySettings.evaluatorQualified =
        g_trainingRecoveryEvaluatorQualified;
    trainingQualifyForcedActionApply();
    trainingCopyText(
        g_trainingStatusText,
        "Training hooks ready; choose both characters");
    QLOG("[training] native hooks qualified and installed "
         "scope=%08X gate=%08X damage=%08X selector=%08X "
         "throwInstall=%08X throwTech=%08X block=%08X hit=%08X "
         "naturalEnd=%08X "
         "guardScannerPrologues=%d autoGuardQualified=0 "
         "recoveryEvaluatorQualified=%d forcedActionApply=%d "
         "capabilities=%08X",
         static_cast<unsigned>(kTrainingMatchScopeFn),
         static_cast<unsigned>(kTrainingStepGateFn),
         static_cast<unsigned>(kTrainingDamageFn),
         static_cast<unsigned>(kTrainingSelectorConfirmFn),
         static_cast<unsigned>(g_trainingThrowInstallFn),
         static_cast<unsigned>(g_trainingThrowTechFn),
         static_cast<unsigned>(g_trainingGuardContactFn),
         static_cast<unsigned>(g_trainingHitContactFn),
         static_cast<unsigned>(g_trainingNaturalActionTerminalFn),
         g_trainingGuardScannerProloguesMatch ? 1 : 0,
         g_trainingRecoveryEvaluatorQualified ? 1 : 0,
         g_trainingForcedActionApplyQualified ? 1 : 0,
         trainingCapabilityFlags());

    trainingInstallContactPolicyHooks();
    trainingInstallRenderPhaseHook();

    trainingPublishStatus();
    trainingTryRegisterUi();
    trainingTryRegisterInputDisplay();
    trainingTryRegisterHitboxOverlay();
    return true;
}

static void trainingResetPlayerSettings() {
    g_trainingSelectedStateSlot = 0u;
    g_trainingGuardMode = GuardMode::Off;
    g_trainingGuardTimingConfig = qtraining::TrainingGuardTimingConfig{};
    g_trainingGuardTimingState = qtraining::TrainingGuardTimingState{};
    g_trainingGuardTimingArmed = true;
    g_trainingGuardTimingPendingHit = false;
    g_trainingGuardTimingPendingBlock = false;
    g_trainingPolicyFollowsHuman = false;
    g_trainingGuardDuringPlayback = false;
    g_trainingDummyStance = DummyStance::Stand;
    g_trainingDummyTech = DummyTech::Off;
    g_trainingHitboxMode = qtraining::HitboxDisplayMode::Off;
    g_trainingHitboxRenderer = qtraining::kHitboxDefaultRenderer;
    g_trainingMeterChargeTarget = TrainingRefillTarget::Both;
    g_trainingMeterChargeInterval = 2u;
    g_trainingMeterChargeCounter = 0u;
    g_trainingRefillRequest = 0u;
    g_trainingExRestoreTarget = qtraining::ExRestoreTarget{};
    g_trainingContactPolicyRequested = qtraining::TrainingContactPolicySet{};
    g_trainingContactPolicyLatched = qtraining::TrainingContactPolicySet{};
    g_trainingResourcePolicies = qtraining::ResourcePolicySet{};
    g_trainingMacroAfterReset = qtraining::TrainingMacroAfterReset::Off;
    g_trainingMacroAutoStartPending =
        qtraining::TrainingMacroAfterReset::Off;
    trainingResetRecoverySettings();
    trainingResetCombatToolSettings();
}

static void trainingBeginNewGeneration() {
    trainingReversalLeadReset();
    ++g_trainingGeneration;
    if (g_trainingGeneration == 0u)
        ++g_trainingGeneration;
    g_trainingCommands.publishGeneration(
        g_trainingGeneration);
    InterlockedExchange(
        &g_trainingUiGeneration,
        static_cast<LONG>(g_trainingGeneration));
    InterlockedExchange(&g_trainingMenuCaptured, 0);
    trainingResetFrontendInputGate();
    g_trainingInput.reset();
    g_trainingMacros.reset();
    g_trainingPlayerSwap = false;
    g_trainingPolicyFollowsHuman = false;
    qinput::inputDisplay().reset(g_trainingGeneration);
    g_trainingGuardTelemetry.reset();
    trainingInvalidateStates(false);
    trainingInvalidateRoundCharacterState();
    g_trainingScratch = TrainingStateSlot{};
    g_trainingBaseline = TrainingInvariantBaseline{};
    g_trainingStepSerial = 0;
    g_trainingFinalInput[0] = 0u;
    g_trainingFinalInput[1] = 0u;
    g_trainingStepPending = false;
    g_trainingStepFailure = false;
    g_trainingMutationFailed = false;
    g_trainingOperationNeutral = false;
    g_trainingPollSerial = 0;
    g_trainingHold.reset();
    g_trainingReturningToSelect = false;
    trainingResetPlayerSettings();
    trainingResetTimelineMachines();
    g_trainingResourcePolicyStepSerial = 0u;
    g_trainingResourcePolicyFires = 0u;
    g_trainingResourcePolicyRejects = 0u;
    g_trainingRecoveryReadoutValid = 0u;
    g_trainingLastPositionPlanValid = false;
    g_trainingGuardPostureDivergences = 0u;
    g_trainingGuardAmbiguousAttackers = 0u;
    g_trainingGuardUnguardablePostures = 0u;
    g_trainingDamage = TrainingDamageEvent{};
    g_trainingDisableReason = g_trainingHooksInstalled
        ? TrainingDisableReason::None
        : TrainingDisableReason::UnsupportedStepBoundary;
    g_trainingDisableText[0] = '\0';
    g_trainingResultText[0] = '\0';
}

static bool trainingStateProbeEnabled() {
    static int cached = -1;
    if (cached < 0) {
        char value[8] = {};
        const DWORD n = GetEnvironmentVariableA(
            "QOH99_TRAINING_STATE_PROBE", value, sizeof(value));
        cached = (n > 0 && n < sizeof(value) && value[0] == '1') ? 1 : 0;
    }
    return cached != 0;
}

static void trainingStateProbeTick() {
    if (!trainingStateProbeEnabled()) return;

    struct Sample {
        uint32_t actionState;
        uint32_t actionFrame;
        uint32_t actionLockEnd;
        uint32_t nextAction;
        uint32_t transitionFlag;
        uint32_t prevAction;
        uint32_t moveGate;
        uint32_t comOwnership;
        bool valid;
    };
    static Sample previous[2] = {};

    for (uint32_t player = 0; player < 2u; ++player) {
        const uintptr_t fighter = trainingFighter(player, false);
        if (!fighter) continue;
        Sample now{};
        now.actionState =
            rd32(fighter + kTrainingFighterActionStateOffset);
        now.actionFrame =
            rd32(fighter + kTrainingFighterActionFrameOffset);
        now.actionLockEnd =
            rd32(fighter + kTrainingFighterActionLockEndOffset);
        now.nextAction =
            rd32(fighter + kTrainingFighterNextActionOffset);
        now.transitionFlag =
            rd32(fighter + kTrainingFighterTransitionFlagOffset);
        now.prevAction =
            rd32(fighter + kTrainingFighterPrevActionOffset);
        now.moveGate =
            rd32(fighter + kTrainingFighterMoveGateOffset) & 0xFFu;
        now.comOwnership   = rd32(fighter + kTrainingFighterComOffset);
        now.valid = true;

        const Sample& was = previous[player];
        const bool changed =
            !was.valid
            || was.actionState != now.actionState
            || was.actionLockEnd != now.actionLockEnd
            || was.nextAction != now.nextAction
            || was.transitionFlag != now.transitionFlag
            || was.prevAction != now.prevAction
            || (was.moveGate & 7u) != (now.moveGate & 7u)
            || was.comOwnership != now.comOwnership;
        if (!changed) continue;

        QLOG("[state-probe] p%u action=%u frame=%u lockEnd=%u next=%u "
             "transition=%u prev=%u moveGate=%02X (&7=%u) com=%u",
             player + 1u, now.actionState, now.actionFrame, now.actionLockEnd,
             now.nextAction, now.transitionFlag, now.prevAction,
             now.moveGate, now.moveGate & 7u, now.comOwnership);
        previous[player] = now;
    }
}

static void trainingReplayNativeHudTick() {
    trainingTryRegisterUi(true);
    trainingPublishNativeHudOnlyStatus();

    TrainingCommandV1 command{};
    for (uint32_t count = 0;
         count < kTrainingCommandsPerStep
              && g_trainingCommands.tryDequeue(command);
         ++count) {
        if (static_cast<TrainingCommandKind>(command.commandKind)
                != TrainingCommandKind::SetNativeHudMask) {
            trainingAcknowledgeCommand(
                command, CommandAckResult::RejectedUnavailable,
                "only the native HUD is controllable during replay playback");
            continue;
        }
        uint32_t owner = 0u, globalMask = 0u, p1 = 0u, p2 = 0u;
        getModularNativeHudRequested(&owner, &globalMask, &p1, &p2);
        uint32_t next[3] = {globalMask, p1, p2};
        if (command.slotOrSetting > 2u) {
            trainingAcknowledgeCommand(
                command, CommandAckResult::RejectedInvalidSetting);
            continue;
        }
        next[command.slotOrSetting] = command.value;
        const bool applied =
            setModularNativeHudTrainingMasks(next[0], next[1], next[2]);
        trainingAcknowledgeCommand(
            command,
            applied ? CommandAckResult::Applied
                    : CommandAckResult::RejectedInvalidSetting);
        if (applied) {
            QLOG("[native-hud] replay mask %u <- %08X",
                 command.slotOrSetting, command.value);
        }
    }
}

static void trainingOuterTick() {
    trainingStateProbeTick();
    if (!g_trainingLaunchRequested) return;
    if (qgame::directBootOfflineFailed()
        && !g_trainingMutationFailed) {
        trainingFailMutation(
            TrainingDisableReason::InvariantFailure,
            "Training DirectBoot failed before selector completion");
    }
    trainingTryRegisterUi();
    trainingTryRegisterInputDisplay();
    trainingTryRegisterHitboxOverlay();
    trainingPublishHitboxSnapshot();
    const uint32_t scene = rd32(kSceneStateAddr);
    const uint32_t matchSub = rd32(kMatchSubAddr);
    if (scene != 3u
        || rd32(kTrainingScene3SubstateAddr) != 2u) {
        g_trainingLiveE2ELogicalMask = 0u;
    }
    if (!g_trainingHitboxOriginalCaptured) {
        g_trainingHitboxOriginal =
            rd8(kTrainingHitboxAddr);
        g_trainingHitboxOriginalCaptured = true;
        wr8(kTrainingHitboxAddr, 0u);
    }

    if (g_trainingStepPending
        && (scene != 2u || matchSub != 0u)) {
        (void)trainingFinalizePendingStep();
    }

    if (g_trainingHadActiveMatch
        && g_trainingLastScene == 2u && g_trainingLastMatchSub == 0u
        && (scene != 2u || matchSub != 0u)) {
        trainingInvalidateRoundCharacterState();
        g_trainingBaseline = TrainingInvariantBaseline{};
        g_trainingLastPositionPlanValid = false;
        trainingResetTimelineMachines(true);
        (void)g_trainingMacros.requestStop();
    }

    if (!g_trainingReturningToSelect) {
        (void)trainingApplyConfigOverlay();
        trainingClearNativeDeveloperEdges();
    }
    if (scene == 2u && matchSub == 0u) {
        if (!g_trainingHadActiveMatch) {
            g_trainingHadActiveMatch = true;
            g_trainingLifecycle =
                TrainingLifecycleState::Active;
            g_trainingBaseline =
                TrainingInvariantBaseline{};
            trainingCopyText(
                g_trainingStatusText,
                "Training match active; initializing rollback layout");
            QLOG("[training] lifecycle ACTIVE scene=2 sub=0 "
                 "generation=%u", g_trainingGeneration);
        }
        wr8(kTrainingHitboxAddr,
            qtraining::hitboxVanillaFlagValue(g_trainingHitboxMode));
    } else if (scene == 2u) {
        g_trainingLifecycle =
            TrainingLifecycleState::MatchInitializing;
        wr8(kTrainingHitboxAddr, 0u);
    } else if (scene == 3u) {
        if (g_trainingHadActiveMatch
            || g_trainingReturningToSelect) {
            trainingBeginNewGeneration();
            g_trainingHadActiveMatch = false;
            QLOG("[training] lifecycle CHARACTER_SELECT; "
                 "new generation=%u", g_trainingGeneration);
        }
        g_trainingLifecycle =
            TrainingLifecycleState::CharacterSelect;
        wr8(kTrainingHitboxAddr, 0u);
        trainingCopyText(
            g_trainingStatusText,
            "Choose P1, release buttons, then choose P2");
    } else {
        g_trainingLifecycle =
            TrainingLifecycleState::LaunchRequested;
        wr8(kTrainingHitboxAddr, 0u);
    }
    if (g_trainingMutationFailed)
        g_trainingLifecycle =
            TrainingLifecycleState::Failed;

    if (scene != g_trainingLastScene
        || matchSub != g_trainingLastMatchSub) {
        QLOG("[training] phase scene=%u sub=%u lifecycle=%u",
             scene, matchSub,
             static_cast<unsigned>(g_trainingLifecycle));
        g_trainingLastScene = scene;
        g_trainingLastMatchSub = matchSub;
    }
    trainingLiveE2EPollCommand(scene, matchSub);
    trainingLiveE2EMirrorStatus(scene, matchSub);
    trainingPublishStatus();
}

static bool trainingInCharacterSelectFrontend() {
    return rd32(kSceneStateAddr) == 3u
        && rd32(kTrainingScene3SubstateAddr) == 2u;
}

static bool trainingInStageSelectionFrontend() {
    return trainingInCharacterSelectFrontend()
        && rd32(kTrainingStageSubphaseAddr) == 1u;
}

bool trainingOverrideActive(int player) {
    if (!g_trainingLaunchRequested
        || player < 0 || player > 1) {
        return false;
    }
    if (g_trainingReturningToSelect)
        return false;
    if (player == 1 && trainingInStageSelectionFrontend()) {
        return true;
    }
    if (player == 0 && trainingInStageSelectionFrontend()) {
        const bool armed =
            InterlockedCompareExchange(
                &g_trainingStageInputArmed, 0, 0) != 0;
        return !armed || g_trainingLiveE2EEnabled;
    }
    if (player == 0 && trainingInCharacterSelectFrontend()) {
        const bool armed =
            InterlockedCompareExchange(
                &g_trainingFrontendInputArmed, 0, 0) != 0;
        return !armed || g_trainingLiveE2EEnabled;
    }
    if (InterlockedCompareExchange(
            &g_trainingMenuCaptured, 0, 0) != 0) {
        return true;
    }
    return g_trainingStepPending
        && !g_trainingMutationFailed
        && rd32(kSceneStateAddr) == 2u
        && rd32(kMatchSubAddr) == 0u;
}

/* Input is normalized by the facing byte +170 as it stands at the read (sub_47C420, before sub_484B90). */
static uint32_t trainingGuardAwayAtRead(int player, uint32_t mask) {
    const TrainingGuardAwayAtRead& arm = g_trainingGuardAwayRead;
    if (!arm.armed || player != static_cast<int>(arm.lane)) return mask;
    constexpr uint32_t kHorizontal =
        qinput::kLogicalLeft | qinput::kLogicalRight;
    if ((mask & kHorizontal) != arm.awayBit) return mask;
    const uintptr_t fighter =
        trainingFighter(static_cast<uint32_t>(player), false);
    if (fighter == 0u) return mask;
    const uint8_t facing = rd8(fighter + kTrainingFighterFacingOffset);
    if (facing > 1u) return mask;
    const uint32_t away =
        facing == 0u ? qinput::kLogicalLeft : qinput::kLogicalRight;
    if (away != arm.awayBit
        && g_trainingGuardTurnedAtReadStep != g_trainingStepSerial) {
        g_trainingGuardTurnedAtReadStep = g_trainingStepSerial;
        ++g_trainingGuardTurnedAtRead;
    }
    return (mask & ~kHorizontal) | away;
}

int trainingOverrideMask(int player) {
    if (player == 1 && trainingInStageSelectionFrontend())
        return 0;
    if (player == 0 && trainingInStageSelectionFrontend()) {
        const bool armed =
            InterlockedCompareExchange(
                &g_trainingStageInputArmed, 0, 0) != 0;
        return armed && g_trainingLiveE2EEnabled
            ? static_cast<int>(
                  g_trainingLiveE2ELogicalMask & 0xF000Fu)
            : 0;
    }
    if (player == 0 && trainingInCharacterSelectFrontend()) {
        const bool armed =
            InterlockedCompareExchange(
                &g_trainingFrontendInputArmed, 0, 0) != 0;
        return armed && g_trainingLiveE2EEnabled
            ? static_cast<int>(
                  g_trainingLiveE2ELogicalMask & 1u)
            : 0;
    }
    if (player < 0 || player > 1) return 0;
    return static_cast<int>(
        trainingGuardAwayAtRead(player, g_trainingFinalInput[player]));
}

void trainingObserveOfflinePhysicalSnapshot(
    uint32_t p1Mask, uint32_t p2Mask,
    bool p1PhysicalNeutralBeforeSocd,
    uint32_t heldTrainingActions) {
    if (!g_trainingLaunchRequested) return;
    const bool liveGameplayInput =
        trainingLiveE2EOfflineControlEnabled()
        && rd32(kSceneStateAddr) == 2u
        && rd32(kMatchSubAddr) == 0u;
    if (!liveGameplayInput)
        g_trainingLiveE2EMailbox.gameplayP1Mask = 0u;
    const uint32_t requestedLiveMask =
        liveGameplayInput
            ? g_trainingLiveE2EMailbox.gameplayP1Mask
            : 0u;
    const uint32_t effectiveP1Mask =
        liveGameplayInput
            ? (qinput::isCanonicalLogicalFightInput(
                   requestedLiveMask)
                ? requestedLiveMask : 0u)
            : p1Mask;
    const bool effectiveP1PhysicalNeutralBeforeSocd =
        liveGameplayInput
            ? requestedLiveMask == 0u
            : p1PhysicalNeutralBeforeSocd;
    g_trainingPhysical.p1Mask = effectiveP1Mask;
    g_trainingPhysical.p2Mask = p2Mask;
    g_trainingPhysical.p1NeutralBeforeSocd =
        effectiveP1PhysicalNeutralBeforeSocd;
    g_trainingPhysical.heldActions =
        liveGameplayInput
            ? 0u
            : (heldTrainingActions & qtraining::kTrainingActionMask);
    ++g_trainingPhysical.dispatchSerial;

    qgame::directboot::TrainingFrontendInputGateSample sample{};
    sample.explicitTraining = true;
    sample.managedRuntimeArmed = g_offlineManagedArmed;
    sample.menuCaptured =
        InterlockedCompareExchange(
            &g_trainingMenuCaptured, 0, 0) != 0;
    sample.physicalNeutralBeforeSocd =
        p1PhysicalNeutralBeforeSocd;
    if (liveGameplayInput) {
        sample.physicalNeutralBeforeSocd =
            effectiveP1PhysicalNeutralBeforeSocd;
    }
    sample.dispatchSerial = g_trainingPhysical.dispatchSerial;
    sample.captureGeneration = static_cast<uint32_t>(
        InterlockedCompareExchange(
            &g_trainingFrontendCaptureGeneration, 0, 0));
    const qgame::directboot::TrainingFrontendInputGateState before =
        g_trainingFrontendInputGate.state();
    const qgame::directboot::TrainingFrontendInputGateDecision decision =
        g_trainingFrontendInputGate.advance(sample);
    InterlockedExchange(
        &g_trainingFrontendInputArmed,
        decision.allowInput ? 1 : 0);
    if (!decision.allowInput)
        g_trainingLiveE2ELogicalMask = 0u;
    if (decision.state != before) {
        QLOG("[training-input] frontend gate %u -> %u "
             "managed=%u captured=%u physicalNeutral=%u settle=%u",
             static_cast<unsigned>(before),
             static_cast<unsigned>(decision.state),
             sample.managedRuntimeArmed ? 1u : 0u,
             sample.menuCaptured ? 1u : 0u,
             sample.physicalNeutralBeforeSocd ? 1u : 0u,
             decision.neutralDispatches);
    }

    qgame::directboot::TrainingStageInputGateSample stageSample{};
    stageSample.explicitTraining = true;
    stageSample.managedRuntimeArmed = g_offlineManagedArmed;
    stageSample.inCharacterSelect =
        trainingInCharacterSelectFrontend();
    stageSample.stageSelectionActive =
        rd32(kTrainingStageSubphaseAddr) == 1u;
    stageSample.menuCaptured = sample.menuCaptured;
    stageSample.actionButtonsNeutral =
        (effectiveP1Mask & 0x0Fu) == 0u;
    stageSample.dispatchSerial = g_trainingPhysical.dispatchSerial;
    const qgame::directboot::TrainingStageInputGateState stageBefore =
        g_trainingStageInputGate.state();
    const qgame::directboot::TrainingStageInputGateDecision stageDecision =
        g_trainingStageInputGate.advance(stageSample);
    InterlockedExchange(
        &g_trainingStageInputArmed,
        stageDecision.allowInput ? 1 : 0);
    if (!stageDecision.allowInput
        && stageSample.stageSelectionActive) {
        g_trainingLiveE2ELogicalMask = 0u;
    }
    if (stageDecision.state != stageBefore) {
        QLOG("[training-input] stage gate %u -> %u active=%u "
             "buttonsNeutral=%u captured=%u settle=%u",
             static_cast<unsigned>(stageBefore),
             static_cast<unsigned>(stageDecision.state),
             stageSample.stageSelectionActive ? 1u : 0u,
             stageSample.actionButtonsNeutral ? 1u : 0u,
             stageSample.menuCaptured ? 1u : 0u,
             stageDecision.neutralDispatches);
    }
}

static void trainingArmInternal(bool enabled) {
    if (!enabled) {
        if (g_trainingLaunchRequested) {
            (void)trainingFinalizePendingStep();
            trainingRestoreOriginalConfigInMemory();
            if (g_trainingHitboxOriginalCaptured)
                wr8(kTrainingHitboxAddr,
                    g_trainingHitboxOriginal);
        }
        g_trainingLaunchRequested = false;
        g_trainingLifecycle = TrainingLifecycleState::Off;
        g_trainingDisableReason =
            TrainingDisableReason::NotTrainingLaunch;
        g_trainingFinalInput[0] =
            g_trainingFinalInput[1] = 0u;
        qinput::inputDisplay().publishIdle();
        (void)qinput::inputDisplayReleaseOverlay();
        InterlockedExchange(&g_trainingMenuCaptured, 0);
        InterlockedExchange(&g_trainingUiGeneration, 0);
        g_trainingLiveE2EEnabled = false;
        trainingLiveE2EResetMailbox();
        trainingResetFrontendInputGate();
        g_trainingMeterChargeCounter = 0u;
        trainingInvalidateRoundCharacterState();
        trainingResetCombatToolSettings();
        trainingResetCombatToolTimeline();
        return;
    }

    g_trainingLaunchRequested = true;
    g_trainingLiveE2EEnabled =
        trainingLiveE2EEnvironmentEnabled();
    trainingLiveE2EResetMailbox();
    trainingResetFrontendInputGate();
    g_trainingGeneration = 1u;
    g_trainingCommands.publishGeneration(
        g_trainingGeneration);
    InterlockedExchange(
        &g_trainingUiGeneration,
        static_cast<LONG>(g_trainingGeneration));
    g_trainingInput.reset();
    g_trainingMacros.reset();
    qinput::inputDisplay().reset(g_trainingGeneration);
    g_trainingGuardTelemetry.reset();
    trainingInvalidateStates(false);
    trainingInvalidateRoundCharacterState();
    g_trainingBaseline = TrainingInvariantBaseline{};
    g_trainingPhysical = TrainingPhysicalSnapshot{};
    g_trainingGuardPermission = 0u;
    g_trainingGuardClassification =
        GuardClassification::Unguardable;
    g_trainingFinalInput[0] =
        g_trainingFinalInput[1] = 0u;
    g_trainingLastComposedInput[0] =
        g_trainingLastComposedInput[1] = 0u;
    g_trainingStepPending = false;
    g_trainingStepFailure = false;
    g_trainingMutationFailed = false;
    g_trainingReturningToSelect = false;
    g_trainingOperationNeutral = false;
    g_trainingMeterChargeTarget = TrainingRefillTarget::Both;
    g_trainingMeterChargeInterval = 2u;
    g_trainingMeterChargeCounter = 0u;
    g_trainingPollSerial = 0;
    g_trainingHold.reset();
    g_trainingHitboxOriginalCaptured = false;
    trainingResetPlayerSettings();
    trainingResetTimelineMachines();
    g_trainingResourcePolicyStepSerial = 0u;
    g_trainingResourcePolicyFires = 0u;
    g_trainingResourcePolicyRejects = 0u;
    g_trainingRecoveryReadoutValid = 0u;
    g_trainingLastPositionPlanValid = false;
    g_trainingGuardPostureDivergences = 0u;
    g_trainingGuardAmbiguousAttackers = 0u;
    g_trainingGuardUnguardablePostures = 0u;
    g_trainingDamage = TrainingDamageEvent{};
    g_trainingLastAckId = 0u;
    g_trainingLastAckResult = CommandAckResult::None;
    g_trainingLastAdmission = StepAdmissionResult::None;
    g_trainingLastNativeReturn = 0u;
    g_trainingStickyErrors = 0u;
    g_trainingDisableReason =
        TrainingDisableReason::UnsupportedStepBoundary;
    g_trainingLifecycle =
        TrainingLifecycleState::LaunchRequested;
    g_trainingHadActiveMatch = false;
    g_trainingLastScene = UINT32_MAX;
    g_trainingLastMatchSub = UINT32_MAX;
    g_trainingDisableText[0] = '\0';
    trainingCopyText(
        g_trainingStatusText,
        "Explicit training launch requested");
    g_trainingResultText[0] = '\0';
    InterlockedExchange(&g_trainingMenuCaptured, 0);
    trainingPublishStatus();
    QLOG("[training] explicit offline training armed "
         "generation=%u liveE2E=%u",
         g_trainingGeneration,
         g_trainingLiveE2EEnabled ? 1u : 0u);
}
