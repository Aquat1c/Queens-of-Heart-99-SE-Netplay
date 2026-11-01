#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "TrainingContactPolicies.h"
#include "TrainingHitboxOverlay.h"
#include "TrainingStatusMessage.h"

namespace qtraining {

constexpr uint32_t kTrainingApiVersion = 1;
constexpr uint32_t kTrainingMacroSlotCount = 8;
constexpr uint32_t kTrainingStateSlotCount = 8;
constexpr uint32_t kTrainingMacroFrameCapacity = 3600;
constexpr uint32_t kTrainingCommandCapacity = 64;
constexpr uint32_t kTrainingTriggerMaximumDelaySteps = 240;
constexpr uint32_t kTrainingTriggerMaximumChancePercent = 100;
constexpr uint32_t kTrainingTriggerMaximumMoveIndex = 64;
constexpr uint32_t kTrainingTriggerMoveValueFlag = 0x80000000u;
static_assert(kTrainingTriggerMaximumMoveIndex <= 64u,
              "trigger move availability uses one uint64 mask");

constexpr bool trainingTriggerMoveIndexValid(uint32_t oneBasedIndex) {
    return oneBasedIndex >= 1u
        && oneBasedIndex <= kTrainingTriggerMaximumMoveIndex;
}

constexpr uint32_t encodeTrainingTriggerMoveValue(uint32_t oneBasedIndex) {
    return trainingTriggerMoveIndexValid(oneBasedIndex)
        ? kTrainingTriggerMoveValueFlag | oneBasedIndex : 0u;
}

constexpr bool isEncodedTrainingTriggerMoveValue(uint32_t value) {
    return (value & kTrainingTriggerMoveValueFlag) != 0u
        && (value & ~(kTrainingTriggerMoveValueFlag | 0xFFFFu)) == 0u
        && trainingTriggerMoveIndexValid(value & 0xFFFFu);
}

constexpr uint16_t decodeTrainingTriggerMoveIndex(uint32_t value) {
    return isEncodedTrainingTriggerMoveValue(value)
        ? static_cast<uint16_t>(value & 0xFFFFu) : 0u;
}

enum class TrainingAction : uint8_t {
    ResetPosition = 0,
    SaveState = 1,
    LoadState = 2,
    RecordStop = 3,
    PlayStop = 4,
    NextSlot = 5,
    PreviousSlot = 6,
    TogglePause = 7,
    FrameStep = 8,
    ReturnToCharacterSelect = 9,
    // QOH's own F-key edges must stay suppressed: each one mutates state the step invariants assert on.
    RefillAll = 10,
    ToggleHitboxes = 11,
    ResetPositionRoundStart = 12,
    CycleDummyStance = 13,
    CycleGuardMode = 14,
    ChargeExFraction = 15,
    Count = 16,
};

constexpr uint32_t trainingImageMutatingActionMask();

constexpr uint32_t kTrainingActionCount =
    static_cast<uint32_t>(TrainingAction::Count);
constexpr uint32_t kTrainingActionMask =
    (1u << kTrainingActionCount) - 1u;

constexpr uint32_t trainingActionBit(TrainingAction action) {
    return 1u << static_cast<uint32_t>(action);
}

constexpr uint32_t kTrainingLevelActionMask =
    trainingActionBit(TrainingAction::ChargeExFraction)
    | trainingActionBit(TrainingAction::RefillAll);

constexpr bool isCanonicalTrainingActions(uint32_t actions) {
    return (actions & ~kTrainingActionMask) == 0;
}

constexpr uint32_t trainingImageMutatingActionMask() {
    return trainingActionBit(TrainingAction::ResetPosition)
        | trainingActionBit(TrainingAction::ResetPositionRoundStart)
        | trainingActionBit(TrainingAction::LoadState);
}

enum class PositionPreset : uint32_t {
    FromModifier = 0,
    // ResetRoundStartNeutral depends on QOH's fixed anchors (P1 64, P2 192, camera 96), so these numbers must not be retuned.
    RoundStart = 1,
    LeftCorner = 2,
    Center = 3,
    RightCorner = 4,
    VeryClose = 5,
};

enum class GuardMode : uint32_t {
    Off = 0,
    Auto = 1,
    AlwaysStand = 2,
    AlwaysCrouch = 3,
    AutoHeuristic = 4,
    AlwaysAir = 5,
    AutoRecord = 6,
};

// Low three bits of fighter +156, as sub_47C8A0 switches on them: 1 stand (guard action 18), 2 crouch (19), 4 air (20); any other value requests no guard.
enum class PostureClass : uint32_t {
    NotGuardable = 0,
    Standing = 1,
    Crouching = 2,
    Air = 3,
    Unreadable = 4,
};

constexpr PostureClass classifyPostureNibble(uint8_t moveGateByte) {
    switch (moveGateByte & 0x07u) {
        case 1: return PostureClass::Standing;
        case 2: return PostureClass::Crouching;
        case 4: return PostureClass::Air;
        default: return PostureClass::NotGuardable;
    }
}

constexpr bool postureClassGrounded(PostureClass posture) {
    return posture == PostureClass::Standing
        || posture == PostureClass::Crouching;
}

constexpr bool postureClassAirborne(PostureClass posture) {
    return posture == PostureClass::Air;
}

// Native attack-record byte: bit 0 stand, bit 1 crouch, bit 2 air.
constexpr uint8_t kGuardPermitStand = 0x01;
constexpr uint8_t kGuardPermitCrouch = 0x02;
constexpr uint8_t kGuardPermitAir = 0x04;
constexpr uint8_t kGuardPermissionMask =
    kGuardPermitStand | kGuardPermitCrouch | kGuardPermitAir;

enum class GuardClassification : uint32_t {
    Unguardable = 0,
    Stand = 1,
    Crouch = 2,
    Either = 3,
    Air = 4,
    Invalid = 5,
};

enum class DummyStance : uint32_t {
    Stand = 0,
    Crouch = 1,
    Jump = 2,
};

enum class DummyTech : uint32_t {
    Off = 0,
    HoldGuard = 1,
};

enum class ResourceTrigger : uint32_t {
    Off = 0,
    AfterCombo = 1,
    AfterRecovery = 2,
    WhenNeutral = 3,
    OnResetOnly = 4,
};

// Append-only: commands and status snapshots carry the numeric event id across the wrapper boundary.
enum class TrainingTriggerEvent : uint32_t {
    OnHit = 0,
    AfterBlock = 1,
    AfterHitstun = 2,
    OnWakeup = 3,
    AfterAirRecovery = 4,
    AfterGroundRecovery = 5,
    // Action 73 is QOH99's native throw-tech result.
    AfterThrowTech = 6,
    GuardCancel = 7,
    Count = 8,
};

enum class TrainingActorPoolId : uint32_t {
    Effect = 0,   // 0x618160, the projectile/effect pool
    Clone = 1,    // 0x526FA0, the spawned-actor pool
    Helper = 2,   // the four striker/assist clone singletons
};

inline constexpr uint32_t kTrainingActorReportCapacity = 16u;
inline constexpr uint32_t kTrainingActorOwnerUnknown = 0xFFFFFFFFu;

constexpr uint32_t encodeTrainingActorSlot(TrainingActorPoolId pool,
                                           uint32_t slot) {
    return (static_cast<uint32_t>(pool) << 24u) | (slot & 0x00FFFFFFu);
}

constexpr TrainingActorPoolId decodeTrainingActorPool(uint32_t encoded) {
    return static_cast<TrainingActorPoolId>((encoded >> 24u) & 0xFFu);
}

constexpr uint32_t decodeTrainingActorSlotIndex(uint32_t encoded) {
    return encoded & 0x00FFFFFFu;
}

enum class TrainingWakeupReversalPhase : uint32_t {
    Idle = 0,
    WatchingKnockdown = 1,
    WaitingForSafeSuffix = 2,
    Priming = 3,
    AwaitingFirstResponse = 4,
    Released = 5,
    Completed = 6,
    Cancelled = 7,
};

constexpr uint32_t kTrainingTriggerEventCount =
    static_cast<uint32_t>(TrainingTriggerEvent::Count);

constexpr bool trainingTriggerEventValid(TrainingTriggerEvent event) {
    return static_cast<uint32_t>(event) < kTrainingTriggerEventCount;
}

// Append-only: the values are written to a session file read back by script.
enum class TriggerJournalKind : uint8_t {
    Latched = 0,
    Cleared = 1,
    Scheduled = 2,
    ChanceSkipped = 3,
    Serviced = 4,
    Outcome = 5,
    PrebufferPrepared = 6,
    PrebufferCleared = 7,
    Count = 8,
};

enum class TriggerJournalCause : uint8_t {
    None = 0,
    ContactHit = 1,
    ContactBlock = 2,
    ContactThrow = 3,
    ActionEntry = 4,
    NewCombatEpisode = 5,
    Superseded = 6,
    MutuallyExclusive = 7,
    Fired = 8,
    GenerationReset = 9,
    NotConfigured = 10,
    ResponseNotReady = 11,
    Count = 12,
};

constexpr const char* triggerJournalKindName(TriggerJournalKind kind) {
    switch (kind) {
        case TriggerJournalKind::Latched: return "latched";
        case TriggerJournalKind::Cleared: return "cleared";
        case TriggerJournalKind::Scheduled: return "scheduled";
        case TriggerJournalKind::ChanceSkipped: return "chance-skipped";
        case TriggerJournalKind::Serviced: return "serviced";
        case TriggerJournalKind::Outcome: return "outcome";
        case TriggerJournalKind::PrebufferPrepared: return "prebuf-prepared";
        case TriggerJournalKind::PrebufferCleared: return "prebuf-cleared";
        default: return "?";
    }
}

constexpr const char* triggerJournalCauseName(TriggerJournalCause cause) {
    switch (cause) {
        case TriggerJournalCause::None: return "-";
        case TriggerJournalCause::ContactHit: return "contact-hit";
        case TriggerJournalCause::ContactBlock: return "contact-block";
        case TriggerJournalCause::ContactThrow: return "contact-throw";
        case TriggerJournalCause::ActionEntry: return "action-entry";
        case TriggerJournalCause::NewCombatEpisode: return "new-episode";
        case TriggerJournalCause::Superseded: return "superseded";
        case TriggerJournalCause::MutuallyExclusive: return "mutually-excl";
        case TriggerJournalCause::Fired: return "fired";
        case TriggerJournalCause::GenerationReset: return "generation";
        case TriggerJournalCause::NotConfigured: return "not-configured";
        case TriggerJournalCause::ResponseNotReady: return "not-ready";
        default: return "?";
    }
}

struct TrainingTriggerJournalEntry {
    uint64_t stepSerial = 0;
    uint32_t detail = 0;
    uint16_t moveIndex = 0;
    TrainingTriggerEvent event = TrainingTriggerEvent::OnHit;
    TriggerJournalKind kind = TriggerJournalKind::Latched;
    TriggerJournalCause cause = TriggerJournalCause::None;
    uint8_t macroSlot = 0;
};

constexpr uint32_t kTrainingTriggerJournalCapacity = 128u;

constexpr uint8_t kTrainingTriggerRandomMacroSlot =
    static_cast<uint8_t>(kTrainingMacroSlotCount + 1u);

struct TrainingTriggerConfig {
    uint8_t macroSlot = 0;
    uint8_t chancePercent = 100;
    uint16_t delaySteps = 0;
    uint16_t moveIndex = 0;
};

constexpr bool trainingTriggerConfigValid(const TrainingTriggerConfig& config) {
    return config.macroSlot <= kTrainingTriggerRandomMacroSlot
        && config.moveIndex <= kTrainingTriggerMaximumMoveIndex
        && !(config.macroSlot != 0u && config.moveIndex != 0u)
        && config.chancePercent <= kTrainingTriggerMaximumChancePercent
        && config.delaySteps <= kTrainingTriggerMaximumDelaySteps;
}

constexpr int32_t kGuardGaugeFull = 100;
constexpr int32_t kExStocksFull = 9;
constexpr int32_t kExFractionMax = 53;

constexpr uint32_t kRecoveryDelayStepsMax = 240;

enum class ManualRecoveryMode : uint32_t {
    Off = 0,
    Earliest = 1,
    FixedDelay = 2,
    RandomDelay = 3,
    Late = 4,
};

enum class RecoveryPolicySlot : uint32_t {
    Air = 0,
    Ground = 1,
    Wall = 2,
    Terminal = 3,
    ThrowEscape = 4,
    Count = 5,
};

// Never renumbered; the eligibility bit mask is indexed by these.
enum class RecoveryBranch : uint32_t {
    Air22 = 0,
    Ground88 = 1,
    Wall66 = 2,
    TerminalForward66 = 3,
    TerminalBack44 = 4,
    ThrowEscape = 5,
    Count = 6,
};

constexpr uint32_t kRecoveryBranchCount =
    static_cast<uint32_t>(RecoveryBranch::Count);

constexpr uint32_t recoveryBranchBit(RecoveryBranch branch) {
    return 1u << static_cast<uint32_t>(branch);
}

enum class TerminalRecoveryChoice : uint32_t {
    Preserve = 0,
    Forward = 1,
    Back = 2,
    Random = 3,
};

enum class RecoveryState : uint32_t {
    Idle = 0,
    WaitEligible = 1,
    DelayEligibleSteps = 2,
    EmitPreNeutral = 3,
    EmitTap1 = 4,
    EmitNeutral = 5,
    EmitTap2 = 6,
    AwaitNativeTransition = 7,
    ObservingBuffer = 8,
    Cooldown = 9,
};

enum class RecoveryVerdict : uint32_t {
    None = 0,
    Succeeded = 1,
    MissedExpired = 2,
    Cancelled = 3,
    BufferedObserved = 4,
    AutomaticObserved = 5,
    CommitBlocked = 6,
};

enum class RecoveryEvaluatorReason : uint32_t {
    Qualified = 0,
    NotAttempted = 1,
    AddressUnresolved = 2,
    PrologueMismatch = 3,
    Count = 4,
};

// Neither reason may ever write QOH's native pause byte byte_62D580.
enum TrainingHoldReason : uint32_t {
    TrainingHoldNone = 0u,
    TrainingHoldUser = 1u << 0,
    TrainingHoldMenu = 1u << 1,
};

constexpr uint32_t kTrainingHoldReasonMask =
    TrainingHoldUser | TrainingHoldMenu;

constexpr bool isCanonicalTrainingHoldReasons(uint32_t reasons) {
    return (reasons & ~kTrainingHoldReasonMask) == 0u;
}

enum RecoveryStandDown : uint32_t {
    RecoveryStandDownNone = 0u,
    RecoveryStandDownNotReadable = 1u << 0,
    RecoveryStandDownLifecycleDown = 1u << 1,
    RecoveryStandDownSandboxInactive = 1u << 2,
    RecoveryStandDownMenuCaptured = 1u << 3,
    RecoveryStandDownOperationNeutral = 1u << 4,
    RecoveryStandDownMacroPlayback = 1u << 5,
    RecoveryStandDownMacroOwnsP1 = 1u << 6,
    RecoveryStandDownComControlled = 1u << 7,
    RecoveryStandDownDirectionSuppressed = 1u << 8,
    RecoveryStandDownKoLatch = 1u << 9,
};

constexpr uint32_t kRecoveryStandDownAvailabilityMask =
    RecoveryStandDownNotReadable | RecoveryStandDownLifecycleDown
    | RecoveryStandDownSandboxInactive | RecoveryStandDownMenuCaptured
    | RecoveryStandDownOperationNeutral | RecoveryStandDownMacroPlayback
    | RecoveryStandDownMacroOwnsP1;

constexpr uint32_t kRecoveryStandDownCommonGateMask =
    RecoveryStandDownComControlled | RecoveryStandDownDirectionSuppressed
    | RecoveryStandDownKoLatch;

constexpr uint32_t kRecoveryStandDownMask =
    kRecoveryStandDownAvailabilityMask | kRecoveryStandDownCommonGateMask;

constexpr bool isCanonicalRecoveryStandDown(uint32_t mask) {
    return (mask & ~kRecoveryStandDownMask) == 0u;
}

enum class TrainingRefillTarget : uint32_t {
    Both = 0,
    P1 = 1,
    P2 = 2,
};

enum class PlaybackMode : uint32_t {
    Once = 0,
    Loop = 1,
    Random = 2,
    RandomLoop = 3,
};

constexpr bool playbackModeRandom(PlaybackMode mode) {
    return mode == PlaybackMode::Random || mode == PlaybackMode::RandomLoop;
}
constexpr bool playbackModeLoops(PlaybackMode mode) {
    return mode == PlaybackMode::Loop || mode == PlaybackMode::RandomLoop;
}

enum class FacingPolicy : uint32_t {
    Relative = 0,
    Absolute = 1,
};

enum class MacroState : uint32_t {
    Idle = 0,
    RecordArmed = 1,
    Recording = 2,
    RecordStopping = 3,
    PlayArmed = 4,
    Playing = 5,
    PlayStopping = 6,
    PreRecord = 7,
};

enum class InputRearmState : uint32_t {
    Armed = 0,
    Captured = 1,
    AwaitNeutral = 2,
    NeutralSettle = 3,
};

enum class TrainingCommandKind : uint32_t {
    None = 0,
    ResetPosition = 1,
    SaveState = 2,
    LoadState = 3,
    ClearState = 4,
    ClearMacro = 5,
    ToggleRecord = 6,
    TogglePlayback = 7,
    StopMacro = 8,
    SelectMacroSlot = 9,
    SelectStateSlot = 10,
    SetGuardMode = 11,
    SetGuardDuringPlayback = 12,
    SetPlaybackMode = 13,
    SetFacingPolicy = 14,
    SetHitboxDisplay = 15,
    ReturnToCharacterSelect = 16,
    SetHpRegen = 17,
    SetStance = 18,
    SetTech = 19,
    RefillLife = 20,
    RefillGuardGauge = 21,
    RefillExMeter = 22,
    // RefillLife/GuardGauge/ExMeter (20..22) carry their target in value with slotOrSetting == 0; the kinds below carry target or policy slot in slotOrSetting and the setting in value.
    SetLifeValue = 23,          // slotOrSetting=target, value=life units (>= 1)
    SetExMeter = 24,            // slotOrSetting=target, value=stocks, flags=frac
    SetGuardGaugeValue = 25,    // slotOrSetting=target, value=0..100
    SetHealthPolicy = 26,       // slotOrSetting=target, value=ResourceTrigger
    SetExPolicy = 27,
    SetGuardGaugePolicy = 28,
    // Must never call sub_40AAD0, and never touches the guard gauge or EX meter.
    ResetRoundStartNeutral = 29,
    SetRecoveryPolicy = 30,        // slotOrSetting=RecoveryPolicySlot, value=ManualRecoveryMode.
    SetRecoveryTiming = 31,        // slotOrSetting=RecoveryPolicySlot, value=delaySteps, flags=randomMin | (randomMax << 16).
    SetTerminalRecoveryChoice = 32,// value=TerminalRecoveryChoice
    TogglePause = 33,
    FrameStep = 34,
    SetMeterChargeConfig = 35, // target=slotOrSetting, interval=value
    SetThrowTechChance = 36,
    // slotOrSetting=TrainingTriggerEvent. Value 0..8 selects Off or a recording; kTrainingTriggerMoveValueFlag|oneBasedMove selects a move catalog row. flags=delay low16 | chancePercent high16.
    SetTriggerConfig = 37,
    SetFrameAdvantageEnabled = 38,
    SetDebugOverlayEnabled = 39,
    SetStateChangeLogEnabled = 40,
    // slotOrSetting selects which mask (0 global, 1 P1, 2 P2), value is the new VISIBLE mask.
    SetNativeHudMask = 41,
    SetFrameTraceEnabled = 42,
    /* The slot goes in slotOrSetting and the policy value in value. */
    SetContactPolicy = 43,
    /* slotOrSetting 0 unpins (back to full), 1 pins with value = (stocks << 16) | fraction. */
    SetExRestoreTarget = 44,
    /* value = TrainingMacroAfterReset. */
    SetMacroAfterReset = 45,
    /* value = TrainingGuardTimingMode, flags = window in steps (1..600). */
    SetGuardTiming = 46,
    // 0 = P2 is the dummy (the default), 1 = P1 is.
    SetPlayerSwap = 47,
    BeginPreRecord = 48,
    Count = 49,
};

enum class TrainingMacroAfterReset : uint32_t {
    Off = 0,
    Play = 1,
    Record = 2,
};
constexpr uint32_t kTrainingMacroAfterResetMax =
    static_cast<uint32_t>(TrainingMacroAfterReset::Record);

enum class CommandIngressResult : uint32_t {
    Accepted = 0,
    NullCommand = 1,
    ApiVersionMismatch = 2,
    StructTooSmall = 3,
    StaleGeneration = 4,
    NonMonotonicId = 5,
    InvalidKind = 6,
    InvalidPayload = 7,
    QueueFull = 8,
};

enum class CommandAckResult : uint32_t {
    None = 0,
    Applied = 1,
    RejectedUnavailable = 2,
    RejectedUnsafeScene = 3,
    RejectedInvalidSlot = 4,
    RejectedInvalidSetting = 5,
    RejectedStaleLayout = 6,
    NativeValidationFailed = 7,
    SnapshotFailed = 8,
};

enum class TrainingDisableReason : uint32_t {
    None = 0,
    NotTrainingLaunch = 1,
    OnlineMode = 2,
    SpectatorMode = 3,
    SelfTestOrProbe = 4,
    UnsupportedBuild = 5,
    UnsupportedStepBoundary = 6,
    UnsafeScene = 7,
    LayoutInvalid = 8,
    InvariantFailure = 9,
    SnapshotFailure = 10,
};

enum class TrainingLifecycleState : uint32_t {
    Off = 0,
    LaunchRequested = 1,
    CharacterSelect = 2,
    MatchInitializing = 3,
    Active = 4,
    Reinitializing = 5,
    ReturningToCharacterSelect = 6,
    Failed = 7,
};

enum TrainingCapability : uint32_t {
    TrainingCapabilityNone = 0,
    TrainingCapabilityReset = 1u << 0,
    TrainingCapabilityEndlessRound = 1u << 1,
    TrainingCapabilityInfiniteTime = 1u << 2,
    TrainingCapabilityDummyGuard = 1u << 3,
    TrainingCapabilityMacros = 1u << 4,
    TrainingCapabilitySavestates = 1u << 5,
    TrainingCapabilityHitboxes = 1u << 6,
    TrainingCapabilityReturnToSelect = 1u << 7,
    TrainingCapabilityRefill = 1u << 8,
    TrainingCapabilityMeterCharge = 1u << 9,
    TrainingCapabilityTriggers = 1u << 10,
    TrainingCapabilityFrameAdvantage = 1u << 11,
    TrainingCapabilityThrowTech = 1u << 12,
    TrainingCapabilityFaultlessPolicy = 1u << 13,
    TrainingCapabilityParryPolicy = 1u << 14,
    TrainingCapabilityCriticalPolicy = 1u << 15,
    TrainingCapabilityReducePolicy = 1u << 16,
};

enum class StepAdmissionResult : uint32_t {
    None = 0,
    Admitted = 1,
    NativeReturnedZero = 2,
    NativeReturnedOne = 3,
    WrongScene = 4,
    WrongMatchSubstate = 5,
    WaitCountBlocked = 6,
    Paused = 7,
    Frozen = 8,
    ReplayOrSpectator = 9,
    InvariantFailure = 10,
};

// Stable wrapper-facing ABI prefix: all members are fixed-width with no compiler-dependent padding.
struct TrainingCommandV1 {
    uint32_t apiVersion = kTrainingApiVersion;
    uint32_t structSize = sizeof(TrainingCommandV1);
    uint32_t trainingGeneration = 0;
    uint32_t commandId = 0;
    uint32_t commandKind =
        static_cast<uint32_t>(TrainingCommandKind::None);
    uint32_t slotOrSetting = 0;
    uint32_t value = 0;
    uint32_t flags = 0;
};

// Immutable wrapper-facing ABI: booleans are u32 and serials are split into words so the layout is identical on x86 and x64.
struct TrainingStatusV1 {
    uint32_t apiVersion = kTrainingApiVersion;
    uint32_t structSize = sizeof(TrainingStatusV1);
    uint32_t explicitTrainingPresent = 0;
    uint32_t available = 0;
    uint32_t disableReason =
        static_cast<uint32_t>(TrainingDisableReason::NotTrainingLaunch);
    uint32_t lifecycleState =
        static_cast<uint32_t>(TrainingLifecycleState::Off);
    uint32_t inMatchReady = 0;
    uint32_t capabilityFlags = TrainingCapabilityNone;
    uint32_t trainingGeneration = 0;
    uint32_t realStepSerialLow = 0;
    uint32_t realStepSerialHigh = 0;

    uint32_t scene = 0;
    uint32_t matchSubstate = 0;
    uint32_t p1Character = 0;
    uint32_t p2Character = 0;
    uint32_t p1Facing = 0;
    uint32_t p2Facing = 0;
    uint32_t p1FinalLogicalMask = 0;
    uint32_t p2FinalLogicalMask = 0;

    uint32_t comMask = 0;
    uint32_t p1ComPrimary = 0;
    uint32_t p1ComMirror = 0;
    uint32_t p2ComPrimary = 0;
    uint32_t p2ComMirror = 0;

    uint32_t guardMode = static_cast<uint32_t>(GuardMode::Off);
    uint32_t guardPermission = 0;
    uint32_t guardClassification =
        static_cast<uint32_t>(GuardClassification::Unguardable);
    uint32_t guardPredictionMisses = 0;
    uint32_t guardDuringPlayback = 0;

    uint32_t endlessRoundRequested = 0;
    uint32_t endlessRoundEffective = 0;
    uint32_t infiniteTimeRequested = 0;
    uint32_t infiniteTimeEffective = 0;
    uint32_t extraFunctionsDisabled = 0;
    uint32_t waitCountZero = 0;
    uint32_t pauseNeutral = 0;
    uint32_t freezeNeutral = 0;

    uint32_t lastAdmissionResult =
        static_cast<uint32_t>(StepAdmissionResult::None);
    uint32_t lastNativeReturn = 0;
    uint32_t inputRearmState =
        static_cast<uint32_t>(InputRearmState::AwaitNeutral);
    uint32_t macroState = static_cast<uint32_t>(MacroState::Idle);
    uint32_t macroCursor = 0;
    uint32_t selectedMacroSlot = 0;
    uint32_t playbackMode = static_cast<uint32_t>(PlaybackMode::Once);
    uint32_t facingPolicy = static_cast<uint32_t>(FacingPolicy::Relative);
    uint32_t validMacroSlots = 0;
    uint32_t macroFrameCounts[kTrainingMacroSlotCount] = {};
    uint32_t macroFrameCapacity = kTrainingMacroFrameCapacity;
    uint32_t selectedStateSlot = 0;
    uint32_t validStateSlots = 0;
    uint32_t staleStateSlots = 0;
    uint32_t rollbackLayoutGeneration = 0;
    uint32_t characterTeamFingerprintLow = 0;
    uint32_t characterTeamFingerprintHigh = 0;

    // HitboxDisplayMode, not a bool: 0 off, 1 our overlay, 2 QOH's own.
    uint32_t hitboxRequested = 0;
    uint32_t hitboxEffective = 0;
    uint32_t menuCaptured = 0;
    uint32_t lastAcknowledgedCommandId = 0;
    uint32_t lastCommandResult =
        static_cast<uint32_t>(CommandAckResult::None);
    uint32_t commandQueueDepth = 0;
    uint32_t commandQueueCapacity = kTrainingCommandCapacity;
    uint32_t commandQueueDrops = 0;
    uint32_t stickyValidationErrors = 0;

    char disableText[96] = {};
    char statusText[96] = {};
    char resultText[96] = {};

    // Appended after the frozen v1 prefix (offsets >= 568). Append only; every field is u32 because the status seqlock copies the struct as uint32 words.
    uint32_t combatReadoutValid = 0;

    uint32_t dummyStance = static_cast<uint32_t>(DummyStance::Stand);
    uint32_t dummyTech = static_cast<uint32_t>(DummyTech::Off);

    uint32_t p1EffectiveLife = 0;
    uint32_t p2EffectiveLife = 0;
    uint32_t lifeMax = 0;
    uint32_t p1GuardGauge = 0;
    uint32_t p2GuardGauge = 0;
    uint32_t p1MeterStocks = 0;
    uint32_t p2MeterStocks = 0;
    uint32_t p1MeterFraction = 0;
    uint32_t p2MeterFraction = 0;

    uint32_t damageEventSerial = 0;
    uint32_t damageDeclinedCount = 0;
    uint32_t lastHitDamage = 0;         // int32 bit pattern, NOMINAL damage
    uint32_t lastHitPendingDelta = 0;   // int32 bit pattern
    uint32_t lastHitAttackerSlot = 0;
    uint32_t lastHitVictimSlot = 0;
    // RAW fatal-cause word: values outside 0..15 are legal and no decode table exists.
    uint32_t lastHitCause = 0;
    uint32_t lastHitAdmitted = 0;

    uint32_t p1ComboHitsTaken = 0;
    uint32_t p2ComboHitsTaken = 0;
    uint32_t p1ComboDamageTaken = 0;
    uint32_t p2ComboDamageTaken = 0;
    uint32_t p1LastComboHitsTaken = 0;
    uint32_t p2LastComboHitsTaken = 0;
    uint32_t p1LastComboDamageTaken = 0;
    uint32_t p2LastComboDamageTaken = 0;
    uint32_t p1MaxComboHitsDealt = 0;
    uint32_t p2MaxComboHitsDealt = 0;
    uint32_t p1MaxComboDamageDealt = 0;
    uint32_t p2MaxComboDamageDealt = 0;

    // Appended after the 696-byte combat tier (offsets >= 696).
    uint32_t p1LifeCurrent = 0;         // int32 bit pattern, life record +0
    uint32_t p1LifePendingDamage = 0;   // int32 bit pattern, life record +4
    uint32_t p1LifeReserve = 0;         // int32 bit pattern, life record +8
    uint32_t p1LifeSettleTimer = 0;     // int32 bit pattern, life record +12
    uint32_t p2LifeCurrent = 0;
    uint32_t p2LifePendingDamage = 0;
    uint32_t p2LifeReserve = 0;
    uint32_t p2LifeSettleTimer = 0;

    uint32_t fighterReadoutValid = 0;

    uint32_t p1ActionState = 0;         // RAW, fighter +48
    uint32_t p1ActionFrame = 0;         // RAW, fighter +52
    uint32_t p1ActionLockEnd = 0;       // RAW, fighter +56
    uint32_t p1AnimFrame = 0;           // RAW, fighter +64
    uint32_t p1NextAction = 0;          // RAW, fighter +72
    uint32_t p1TransitionFlag = 0;      // RAW, fighter +76
    uint32_t p1PrevAction = 0;          // RAW, fighter +80
    uint32_t p1MoveGateByte = 0;        // RAW byte, fighter +156, masked 0xFF
    uint32_t p1PositionX = 0;           // int32 bit pattern, fighter +136
    uint32_t p1PositionY = 0;           // int32 bit pattern, fighter +140
    uint32_t p2ActionState = 0;
    uint32_t p2ActionFrame = 0;
    uint32_t p2ActionLockEnd = 0;
    uint32_t p2AnimFrame = 0;
    uint32_t p2NextAction = 0;
    uint32_t p2TransitionFlag = 0;
    uint32_t p2PrevAction = 0;
    uint32_t p2MoveGateByte = 0;
    uint32_t p2PositionX = 0;
    uint32_t p2PositionY = 0;

    // Appended after the 812-byte fighter tier (offsets >= 812); gated on kTrainingStatusV1RecoveryTier.

    uint32_t holdReasons = TrainingHoldNone;

    uint32_t guardHeuristicPostureDivergences = 0;
    uint32_t guardHeuristicAmbiguousAttackers = 0;
    uint32_t guardHeuristicUnguardablePostures = 0;
    uint32_t p1PostureClass = static_cast<uint32_t>(PostureClass::Unreadable);
    uint32_t p2PostureClass = static_cast<uint32_t>(PostureClass::Unreadable);

    uint32_t resourcePolicyValid = 0;
    uint32_t healthTrigger = static_cast<uint32_t>(ResourceTrigger::Off);
    uint32_t exTrigger = static_cast<uint32_t>(ResourceTrigger::Off);
    uint32_t guardGaugeTrigger = static_cast<uint32_t>(ResourceTrigger::Off);
    uint32_t healthTriggerTarget =
        static_cast<uint32_t>(TrainingRefillTarget::Both);
    uint32_t exTriggerTarget =
        static_cast<uint32_t>(TrainingRefillTarget::Both);
    uint32_t guardGaugeTriggerTarget =
        static_cast<uint32_t>(TrainingRefillTarget::Both);
    uint32_t bothFightersNeutral = 0;
    uint32_t neutralHoldSteps = 0;
    uint32_t p1NeutralReasonMask = 0;
    uint32_t p2NeutralReasonMask = 0;
    uint32_t neutralGlobalReasonMask = 0;
    uint32_t p1ComboEpisodeState = 0;
    uint32_t p2ComboEpisodeState = 0;
    uint32_t p1RecoveryLock = 0;        // RAW, 0x6265DC + 44*slot
    uint32_t p2RecoveryLock = 0;
    uint32_t p1RecoveryPhaseLatch = 0;  // RAW, 0x6265D8 + 44*slot
    uint32_t p2RecoveryPhaseLatch = 0;
    uint32_t p1ComboHitsNative = 0;     // RAW, 0x6265D0 + 44*slot
    uint32_t p2ComboHitsNative = 0;
    uint32_t p1AuthoredFrameFlags = 0;  // RAW byte, fighter +158
    uint32_t p2AuthoredFrameFlags = 0;
    uint32_t p1TerminalReady = 0;       // RAW, fighter +1528
    uint32_t p2TerminalReady = 0;
    uint32_t p1DispatchAction = 0;      // RAW, fighter +84
    uint32_t p2DispatchAction = 0;
    uint32_t p1InitStatus = 0;          // RAW, fighter +88, int32 bit pattern
    uint32_t p2InitStatus = 0;
    uint32_t lifeSetMax = 0;
    uint32_t exStocksMax = 0;
    uint32_t guardGaugeMax = 0;
    uint32_t resourcePolicyFires = 0;
    uint32_t resourcePolicyRejects = 0;
    uint32_t roundStartNeutralMask = 0;
    uint32_t roundStartNeutralValid = 0;

    uint32_t recoveryReadoutValid = 0;
    uint32_t recoveryModeAir = static_cast<uint32_t>(ManualRecoveryMode::Off);
    uint32_t recoveryModeGround = static_cast<uint32_t>(ManualRecoveryMode::Off);
    uint32_t recoveryModeWall = static_cast<uint32_t>(ManualRecoveryMode::Off);
    uint32_t recoveryModeTerminal =
        static_cast<uint32_t>(ManualRecoveryMode::Off);
    uint32_t recoveryModeThrowEscape =
        static_cast<uint32_t>(ManualRecoveryMode::Off);
    uint32_t recoveryTerminalChoice =
        static_cast<uint32_t>(TerminalRecoveryChoice::Preserve);
    uint32_t recoveryDelaySteps = 0;
    uint32_t recoveryRandomMinSteps = 0;
    uint32_t recoveryRandomMaxSteps = 0;
    uint32_t recoveryState = static_cast<uint32_t>(RecoveryState::Idle);
    uint32_t recoveryBranch = static_cast<uint32_t>(RecoveryBranch::Air22);
    uint32_t recoveryEligibleMask = 0;
    uint32_t recoveryLastVerdict =
        static_cast<uint32_t>(RecoveryVerdict::None);
    uint32_t recoveryAttempts = 0;
    uint32_t recoverySucceeded = 0;
    uint32_t recoveryMissed = 0;
    uint32_t recoveryCancelled = 0;
    uint32_t recoveryBufferedObserved = 0;
    uint32_t recoveryAutomaticObserved = 0;
    uint32_t recoveryCommitBlocked = 0;
    uint32_t recoveryLastResultAction = 0;
    uint32_t recoveryStepsRemaining = 0;
    uint32_t recoveryContactTestUnknown = 0;

    // Wall-contact flags: RAW fighter +1390 & 3, world space.
    uint32_t p1WallFlags = 0;
    uint32_t p2WallFlags = 0;

    uint32_t recoveryEvaluatorReason =
        static_cast<uint32_t>(RecoveryEvaluatorReason::NotAttempted);
    uint32_t recoveryStandDownMask = RecoveryStandDownNone;
    uint32_t recoveryArmableMask = 0;
    uint32_t recoveryTapsEmitted = 0;

    uint32_t meterChargeTarget =
        static_cast<uint32_t>(TrainingRefillTarget::Both);
    uint32_t meterChargeInterval = 2u;

    uint32_t combatToolsReadoutValid = 0u;

    uint32_t throwTechChancePercent = 0u;
    uint32_t throwTechOpportunities = 0u;
    uint32_t throwTechNativeAccepted = 0u;
    uint32_t throwTechAutomaticAccepted = 0u;
    uint32_t throwTechResultObserved = 0u;

    uint32_t triggerMacroSlot[kTrainingTriggerEventCount] = {};
    uint32_t triggerDelaySteps[kTrainingTriggerEventCount] = {};
    uint32_t triggerChancePercent[kTrainingTriggerEventCount] = {};
    uint32_t triggerPendingCount = 0u;
    uint32_t triggerStartedCount = 0u;
    uint32_t triggerChanceSkippedCount = 0u;
    uint32_t triggerDroppedCount = 0u;
    uint32_t triggerLastEvent = 0u;
    uint32_t triggerLastMacroSlot = 0u;
    uint32_t triggerLastOutcome = 0u;

    uint32_t frameAdvantageEnabled = 0u;
    uint32_t frameAdvantageState = 0u;
    uint32_t frameAdvantageContactKind = 0u;
    uint32_t frameAdvantageAttackerOwner = 0u;
    uint32_t frameAdvantageDefenderOwner = 0u;
    uint32_t frameAdvantageValid = 0u;
    uint32_t frameAdvantageValue = 0u;
    uint32_t frameAdvantageAttackerReadyDelay = 0u;
    uint32_t frameAdvantageDefenderReadyDelay = 0u;
    uint32_t frameAdvantageCompletedCount = 0u;
    uint32_t frameAdvantageTradeCount = 0u;
    uint32_t frameAdvantageTimeoutCount = 0u;

    uint32_t triggerMoveCatalogId = 0u;
    uint32_t triggerMoveIndex[kTrainingTriggerEventCount] = {};
    uint32_t triggerLastMoveIndex = 0u;

    uint32_t debugOverlayEnabled = 0u;
    uint32_t debugStepSerialLow = 0u;
    uint32_t debugStepSerialHigh = 0u;
    uint32_t debugP1LogicalMask = 0u;
    uint32_t debugP2LogicalMask = 0u;
    uint32_t p1FreezeFrames = 0u;
    uint32_t p2FreezeFrames = 0u;
    uint32_t p1ResponseReady = 0u;
    uint32_t p2ResponseReady = 0u;
    uint32_t p2GuardCancelReady = 0u;
    uint32_t wakeupReversalPhase =
        static_cast<uint32_t>(TrainingWakeupReversalPhase::Idle);
    uint32_t wakeupReversalMoveIndex = 0u;
    uint32_t wakeupLockedStepsRemaining = 0u;
    uint32_t wakeupCommitIndex = 0u;
    uint32_t wakeupPrefixCursor = 0u;
    uint32_t wakeupLastOutcome = 0u;

    uint32_t stateChangeLogEnabled = 0u;
    uint32_t stateChangeLogRecords = 0u;
    uint32_t stateChangeLogFailed = 0u;
    uint32_t triggerSuppressionReason = 0u;

    uint32_t actorPoolValid = 0u;
    uint32_t actorPoolLiveCount = 0u;
    uint32_t actorPoolReportedCount = 0u;
    uint32_t actorPoolAction[kTrainingActorReportCapacity] = {};
    uint32_t actorPoolOwner[kTrainingActorReportCapacity] = {};
    // TrainingActorPoolId in the high byte, slot index in the low 24 bits.
    uint32_t actorPoolSlot[kTrainingActorReportCapacity] = {};

    uint32_t nativeHudOwner = 0u;
    uint32_t nativeHudGlobalMask = 0u;
    uint32_t nativeHudP1Mask = 0u;
    uint32_t nativeHudP2Mask = 0u;
    uint32_t nativeHudUnavailableGlobalMask = 0u;
    uint32_t nativeHudUnavailableP1Mask = 0u;
    uint32_t nativeHudUnavailableP2Mask = 0u;

    uint32_t nativeHudRequestedGlobalMask = 0u;
    uint32_t nativeHudRequestedP1Mask = 0u;
    uint32_t nativeHudRequestedP2Mask = 0u;

    uint32_t frameTraceEnabled = 0u;
    uint32_t frameTraceRows = 0u;
    uint32_t frameTraceFailed = 0u;
    uint32_t frameTraceCapped = 0u;

    uint32_t frameAdvantageGapValid = 0u;
    uint32_t frameAdvantageGapFrames = 0u;

    uint32_t faultlessPolicy = 0u;
    uint32_t parryPolicy = 0u;
    uint32_t criticalP1Policy = 0u;
    uint32_t criticalP2Policy = 0u;

    uint32_t faultlessAvailable = 0u;
    uint32_t parryAvailable = 0u;
    uint32_t criticalAvailable = 0u;

    uint32_t faultlessSupportedDummy = 0u;
    uint32_t parrySupportedDummy = 0u;
    uint32_t dummyFractionalMeter = 0u;

    uint32_t lastContactStepLow = 0u;
    uint32_t lastContactStepHigh = 0u;

    uint32_t lastFaultlessNativeMode = 0u;
    uint32_t lastFaultlessFinalMode = 0u;
    uint32_t lastFaultlessMeterBefore = 0u;
    uint32_t lastFaultlessReason = 0u;

    uint32_t lastParryNativeResult = 0u;
    uint32_t lastParryFinalResult = 0u;
    uint32_t lastParryReason = 0u;

    uint32_t lastCriticalAttackerSlot = 0u;
    uint32_t lastCriticalIncomingHit = 0u;
    uint32_t lastCriticalDivisor = 0u;
    uint32_t lastCriticalNativeSample = 0u;
    uint32_t lastCriticalFinalSample = 0u;
    uint32_t lastCriticalGuardCrushLatch = 0u;
    uint32_t lastCriticalNativeFlag = 0u;
    uint32_t lastCriticalFinalFlag = 0u;
    uint32_t lastCriticalDamageBefore = 0u;
    uint32_t lastCriticalDamageAfter = 0u;
    uint32_t lastCriticalReason = 0u;

    uint32_t exRestorePinned = 0u;
    uint32_t exRestoreStocks = 0u;
    uint32_t exRestoreFraction = 0u;

    uint32_t macroAfterReset = 0u;

    uint32_t reduceP1Policy = 0u;
    uint32_t reduceP2Policy = 0u;
    uint32_t reduceAvailable = 0u;
    uint32_t lastReduceVictimSlot = 0u;
    uint32_t lastReduceNativeVerdict = 0u;
    uint32_t lastReduceFinalVerdict = 0u;
    uint32_t lastReduceReason = 0u;

    uint32_t guardTimingMode = 0u;
    uint32_t guardTimingWindow = 0u;
    uint32_t guardTimingArmed = 0u;

    uint32_t playerSwap = 0u;

    uint32_t statusEventKind = 0u;
    uint32_t statusEventDetail = 0u;
    uint32_t statusEventSequence = 0u;
    uint32_t statusModeKind = 0u;
    uint32_t statusModeDetail = 0u;
};

// Frozen v1 prefix: fields below this offset never move. The provider copies a released tier, never an arbitrary caller-supplied length.
constexpr uint32_t kTrainingStatusV1BaseSize = 568;

// Every released status size, ascending, ending at sizeof(). Append only; never edit or remove an entry.
constexpr uint32_t kTrainingStatusV1SizeTiers[] = {
    568u, 696u, 812u, 1072u, 1080u, 1096u, 1104u,
    1300u, 1340u, 1404u, 1420u, 1624u, 1652u, 1664u, 1680u, 1688u, 1808u,
    1820u, 1824u, 1852u, 1864u, 1868u, sizeof(TrainingStatusV1)};

constexpr uint32_t kTrainingStatusV1CombatTier = 696u;
constexpr uint32_t kTrainingStatusV1FighterTier = 812u;
constexpr uint32_t kTrainingStatusV1RecoveryTier = 1072u;
constexpr uint32_t kTrainingStatusV1WallTier = 1080u;
constexpr uint32_t kTrainingStatusV1RecoveryDiagTier = 1096u;
constexpr uint32_t kTrainingStatusV1MeterChargeTier = 1104u;
constexpr uint32_t kTrainingStatusV1CombatToolsTier = 1300u;
constexpr uint32_t kTrainingStatusV1ActionTriggersTier = 1340u;
// Released tiers must be frozen literals: a tier written as sizeof() silently redefines itself on the next append.
constexpr uint32_t kTrainingStatusV1DebugOverlayTier = 1404u;
constexpr uint32_t kTrainingStatusV1StateLogTier = 1420u;
constexpr uint32_t kTrainingStatusV1ActorPoolTier = 1624u;
constexpr uint32_t kTrainingStatusV1NativeHudTier = 1652u;
constexpr uint32_t kTrainingStatusV1NativeHudRequestedTier = 1664u;
constexpr uint32_t kTrainingStatusV1FrameTraceTier = 1680u;
constexpr uint32_t kTrainingStatusV1FrameAdvantageGapTier = 1688u;
constexpr uint32_t kTrainingStatusV1ContactPolicyTier = 1808u;
constexpr uint32_t kTrainingStatusV1ExRestoreTargetTier = 1820u;
constexpr uint32_t kTrainingStatusV1MacroAfterResetTier = 1824u;
constexpr uint32_t kTrainingStatusV1ReducePolicyTier = 1852u;
constexpr uint32_t kTrainingStatusV1GuardTimingTier = 1864u;
constexpr uint32_t kTrainingStatusV1PlayerSwapTier = 1868u;
constexpr uint32_t kTrainingStatusV1StatusMessageTier =
    sizeof(TrainingStatusV1);

constexpr uint32_t kTrainingStatusV1TierCount =
    sizeof(kTrainingStatusV1SizeTiers)
    / sizeof(kTrainingStatusV1SizeTiers[0]);

constexpr bool trainingStatusTiersAscend() {
    for (uint32_t i = 1; i < kTrainingStatusV1TierCount; ++i) {
        if (kTrainingStatusV1SizeTiers[i]
            <= kTrainingStatusV1SizeTiers[i - 1]) {
            return false;
        }
    }
    return true;
}

constexpr uint32_t trainingStatusTierFor(uint32_t callerSize) {
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < kTrainingStatusV1TierCount; ++i) {
        if (kTrainingStatusV1SizeTiers[i] <= callerSize)
            chosen = kTrainingStatusV1SizeTiers[i];
    }
    return chosen;
}

constexpr bool isValidCommandKind(TrainingCommandKind kind) {
    return kind > TrainingCommandKind::None
        && kind < TrainingCommandKind::Count;
}

constexpr bool commandMayCoalesce(TrainingCommandKind kind) {
    switch (kind) {
        case TrainingCommandKind::SelectMacroSlot:
        case TrainingCommandKind::SelectStateSlot:
        case TrainingCommandKind::SetGuardMode:
        case TrainingCommandKind::SetGuardDuringPlayback:
        case TrainingCommandKind::SetPlaybackMode:
        case TrainingCommandKind::SetFacingPolicy:
        case TrainingCommandKind::SetHitboxDisplay:
        case TrainingCommandKind::SetStance:
        case TrainingCommandKind::SetTech:
        case TrainingCommandKind::SetHealthPolicy:
        case TrainingCommandKind::SetExPolicy:
        case TrainingCommandKind::SetGuardGaugePolicy:
        case TrainingCommandKind::SetRecoveryPolicy:
        case TrainingCommandKind::SetRecoveryTiming:
        case TrainingCommandKind::SetTerminalRecoveryChoice:
        case TrainingCommandKind::SetMeterChargeConfig:
        case TrainingCommandKind::SetThrowTechChance:
        case TrainingCommandKind::SetTriggerConfig:
        case TrainingCommandKind::SetFrameAdvantageEnabled:
        case TrainingCommandKind::SetDebugOverlayEnabled:
        case TrainingCommandKind::SetStateChangeLogEnabled:
        case TrainingCommandKind::SetNativeHudMask:
        case TrainingCommandKind::SetFrameTraceEnabled:
        case TrainingCommandKind::SetContactPolicy:
        case TrainingCommandKind::SetExRestoreTarget:
        case TrainingCommandKind::SetMacroAfterReset:
            return true;
        default:
            return false;
    }
}

// Admission must serialize these: an acknowledged image mutation must not be undone by a later command in the same native step.
constexpr bool commandOwnsNativeImageTransaction(
    TrainingCommandKind kind) {
    return kind == TrainingCommandKind::ResetPosition
        || kind == TrainingCommandKind::ResetRoundStartNeutral
        || kind == TrainingCommandKind::LoadState;
}

constexpr bool validCommandPayload(const TrainingCommandV1& command) {
    const auto kind =
        static_cast<TrainingCommandKind>(command.commandKind);
    switch (kind) {
        case TrainingCommandKind::ResetPosition:
            return command.value
                    <= static_cast<uint32_t>(PositionPreset::VeryClose);
        case TrainingCommandKind::SaveState:
        case TrainingCommandKind::LoadState:
        case TrainingCommandKind::ClearState:
        case TrainingCommandKind::SelectStateSlot:
            return command.slotOrSetting < kTrainingStateSlotCount;
        case TrainingCommandKind::ClearMacro:
        case TrainingCommandKind::SelectMacroSlot:
            return command.slotOrSetting < kTrainingMacroSlotCount;
        case TrainingCommandKind::SetGuardMode:
            return command.value
                    <= static_cast<uint32_t>(GuardMode::AutoRecord);
        case TrainingCommandKind::SetGuardDuringPlayback:
            return command.value <= 1;
        case TrainingCommandKind::SetHitboxDisplay:
            return command.value <= kHitboxDisplayModeMax;
        case TrainingCommandKind::SetHpRegen:
            return false;
        case TrainingCommandKind::SetStance:
            return command.slotOrSetting == 0u
                && command.value
                    <= static_cast<uint32_t>(DummyStance::Jump)
                && command.flags == 0u;
        case TrainingCommandKind::SetTech:
            return command.slotOrSetting == 0u
                && command.value
                    <= static_cast<uint32_t>(DummyTech::HoldGuard)
                && command.flags == 0u;
        case TrainingCommandKind::RefillLife:
        case TrainingCommandKind::RefillGuardGauge:
        case TrainingCommandKind::RefillExMeter:
            return command.slotOrSetting == 0u
                && command.value
                    <= static_cast<uint32_t>(TrainingRefillTarget::P2)
                && command.flags == 0u;
        case TrainingCommandKind::SetLifeValue:
            return command.slotOrSetting
                    <= static_cast<uint32_t>(TrainingRefillTarget::P2)
                && command.value >= 1u
                && command.flags == 0u;
        case TrainingCommandKind::SetExMeter:
            return command.slotOrSetting
                    <= static_cast<uint32_t>(TrainingRefillTarget::P2)
                && command.value <= static_cast<uint32_t>(kExStocksFull)
                && command.flags <= static_cast<uint32_t>(kExFractionMax)
                && !(command.value == static_cast<uint32_t>(kExStocksFull)
                     && command.flags != 0u);
        case TrainingCommandKind::SetGuardGaugeValue:
            return command.slotOrSetting
                    <= static_cast<uint32_t>(TrainingRefillTarget::P2)
                && command.value <= static_cast<uint32_t>(kGuardGaugeFull)
                && command.flags == 0u;
        case TrainingCommandKind::SetHealthPolicy:
        case TrainingCommandKind::SetExPolicy:
        case TrainingCommandKind::SetGuardGaugePolicy:
            return command.slotOrSetting
                    <= static_cast<uint32_t>(TrainingRefillTarget::P2)
                && command.value
                    <= static_cast<uint32_t>(ResourceTrigger::OnResetOnly)
                && command.flags == 0u;
        case TrainingCommandKind::ResetRoundStartNeutral:
            return command.slotOrSetting
                    <= static_cast<uint32_t>(TrainingRefillTarget::P2)
                && command.value == 0u
                && command.flags == 0u;
        case TrainingCommandKind::SetRecoveryPolicy:
            return command.slotOrSetting
                    < static_cast<uint32_t>(RecoveryPolicySlot::Count)
                && command.value
                    <= static_cast<uint32_t>(ManualRecoveryMode::Late)
                && command.flags == 0u;
        case TrainingCommandKind::SetRecoveryTiming: {
            const uint32_t randomMin = command.flags & 0xFFFFu;
            const uint32_t randomMax = (command.flags >> 16) & 0xFFFFu;
            return command.slotOrSetting == 0u
                && command.value <= kRecoveryDelayStepsMax
                && randomMin <= kRecoveryDelayStepsMax
                && randomMax <= kRecoveryDelayStepsMax
                && randomMin <= randomMax;
        }
        case TrainingCommandKind::SetTerminalRecoveryChoice:
            return command.slotOrSetting == 0u
                && command.value
                    <= static_cast<uint32_t>(TerminalRecoveryChoice::Random)
                && command.flags == 0u;
        case TrainingCommandKind::SetMeterChargeConfig:
            return command.slotOrSetting
                    <= static_cast<uint32_t>(TrainingRefillTarget::P2)
                && command.value >= 1u && command.value <= 60u
                && command.flags == 0u;
        case TrainingCommandKind::SetThrowTechChance:
            return command.slotOrSetting == 0u
                && command.value <= kTrainingTriggerMaximumChancePercent
                && command.flags == 0u;
        case TrainingCommandKind::SetTriggerConfig: {
            const uint32_t delay = command.flags & 0xFFFFu;
            const uint32_t chance = (command.flags >> 16u) & 0xFFFFu;
            return command.slotOrSetting < kTrainingTriggerEventCount
                && (command.value <= kTrainingTriggerRandomMacroSlot
                    || isEncodedTrainingTriggerMoveValue(command.value))
                && delay <= kTrainingTriggerMaximumDelaySteps
                && chance <= kTrainingTriggerMaximumChancePercent;
        }
        case TrainingCommandKind::SetFrameAdvantageEnabled:
        case TrainingCommandKind::SetDebugOverlayEnabled:
        case TrainingCommandKind::SetStateChangeLogEnabled:
        case TrainingCommandKind::SetFrameTraceEnabled:
            return command.slotOrSetting == 0u
                && command.value <= 1u
                && command.flags == 0u;
        case TrainingCommandKind::SetNativeHudMask:
            return command.slotOrSetting <= 2u && command.flags == 0u;
        case TrainingCommandKind::SetExRestoreTarget:
            if (command.flags != 0u) return false;
            if (command.slotOrSetting == 0u) return command.value == 0u;
            if (command.slotOrSetting != 1u) return false;
            {
                const int32_t stocks = static_cast<int32_t>(
                    (command.value >> 16) & 0xFFFFu);
                const int32_t fraction = static_cast<int32_t>(
                    command.value & 0xFFFFu);
                return stocks <= kExStocksFull
                    && fraction <= kExFractionMax
                    && !(stocks == kExStocksFull && fraction != 0);
            }
        case TrainingCommandKind::SetContactPolicy:
            return contactPolicyAssignmentValid(
                       command.slotOrSetting, command.value)
                && command.flags == 0u;
        case TrainingCommandKind::SetMacroAfterReset:
            return command.slotOrSetting == 0u
                && command.value <= kTrainingMacroAfterResetMax
                && command.flags == 0u;
        case TrainingCommandKind::SetGuardTiming:
            return command.slotOrSetting == 0u && command.value < 3u
                && command.flags >= 1u && command.flags <= 600u;
        case TrainingCommandKind::SetPlayerSwap:
            return command.slotOrSetting == 0u && command.value <= 1u
                && command.flags == 0u;
        case TrainingCommandKind::TogglePause:
        case TrainingCommandKind::FrameStep:
            return command.slotOrSetting == 0u
                && command.value == 0u
                && command.flags == 0u;
        case TrainingCommandKind::SetPlaybackMode:
            return command.value
                <= static_cast<uint32_t>(PlaybackMode::RandomLoop);
        case TrainingCommandKind::SetFacingPolicy:
            return command.value
                    <= static_cast<uint32_t>(FacingPolicy::Absolute);
        case TrainingCommandKind::ToggleRecord:
        case TrainingCommandKind::TogglePlayback:
        case TrainingCommandKind::StopMacro:
            return true;
        case TrainingCommandKind::BeginPreRecord:
            return command.slotOrSetting == 0u
                && command.value == 0u
                && command.flags == 0u;
        case TrainingCommandKind::ReturnToCharacterSelect:
            return command.slotOrSetting == 0u
                && command.value == 0u
                && command.flags == 0u;
        case TrainingCommandKind::None:
        case TrainingCommandKind::Count:
            return false;
    }
    return false;
}

enum class RefillVerdict : uint32_t {
    Apply = 0,
    Busy = 1,
    Corrupt = 2,
    Invalid = 3,
};

constexpr bool refillFitsInt32(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX;
}

constexpr uint32_t kTrainingRefillPlayerCount = 2;

constexpr bool isValidRefillTarget(TrainingRefillTarget target) {
    return target <= TrainingRefillTarget::P2;
}

// Slots 2/3 must never be written.
constexpr bool refillTargetIncludesPlayer(TrainingRefillTarget target,
                                          uint32_t player) {
    if (player >= kTrainingRefillPlayerCount) return false;
    switch (target) {
        case TrainingRefillTarget::Both:
            return true;
        case TrainingRefillTarget::P1:
            return player == 0u;
        case TrainingRefillTarget::P2:
            return player == 1u;
    }
    return false;
}

constexpr uint32_t refillTargetPlayerMask(TrainingRefillTarget target) {
    uint32_t mask = 0;
    for (uint32_t player = 0; player < kTrainingRefillPlayerCount; ++player) {
        if (refillTargetIncludesPlayer(target, player)) mask |= 1u << player;
    }
    return mask;
}

// maxLife = 9200 * (lifeLevel + 1), where lifeLevel is the active config record's life-level word; settlement clamps current to it.
constexpr int64_t kLifeUnitsPerLevel = 9200;
constexpr int64_t kLifeLevelClamp = 255;

struct LifeMaxPlan {
    bool valid = false;
    int64_t maxLife = 0;
};

constexpr LifeMaxPlan planLifeMax(int64_t lifeLevel) {
    LifeMaxPlan plan{};
    if (lifeLevel < 0 || lifeLevel > kLifeLevelClamp) return plan;
    const int64_t maxLife = kLifeUnitsPerLevel * (lifeLevel + 1);
    if (maxLife < kLifeUnitsPerLevel || !refillFitsInt32(maxLife))
        return plan;
    plan.valid = true;
    plan.maxLife = maxLife;
    return plan;
}

// The 16-byte life record: effective life is current + reserve - pendingDamage, and the settle timer counts frames until pending damage is subtracted from current.
struct LifeRecordImage {
    int64_t current = 0;
    int64_t pendingDamage = 0;
    int64_t reserve = 0;
    int64_t settleTimer = 0;
};

struct LifeRefillPlan {
    RefillVerdict verdict = RefillVerdict::Corrupt;
    int32_t current = 0;
    int32_t pendingDamage = 0;
    int32_t reserve = 0;
    int32_t settleTimer = 0;
};

constexpr LifeRefillPlan planLifeRefill(const LifeRecordImage& image,
                                        int64_t maxLife) {
    LifeRefillPlan plan{};
    if (maxLife < kLifeUnitsPerLevel || !refillFitsInt32(maxLife))
        return plan;
    if (image.current < 0 || image.pendingDamage < 0
        || image.reserve < 0 || image.settleTimer < 0) {
        return plan;
    }
    if (!refillFitsInt32(image.current)
        || !refillFitsInt32(image.pendingDamage)
        || !refillFitsInt32(image.reserve)
        || !refillFitsInt32(image.settleTimer)) {
        return plan;
    }
    if (image.current > maxLife) return plan;
    const int64_t total = image.current + image.reserve;
    const int64_t effective = total - image.pendingDamage;
    if (!refillFitsInt32(total) || !refillFitsInt32(effective)
        || effective < 0) {
        return plan;
    }
    const int64_t intermediate = maxLife + image.reserve;
    if (!refillFitsInt32(intermediate)) return plan;
    if (intermediate - image.pendingDamage < 0) return plan;

    plan.verdict = RefillVerdict::Apply;
    plan.current = static_cast<int32_t>(maxLife);
    plan.pendingDamage = 0;
    plan.reserve = 0;
    plan.settleTimer = 0;
    return plan;
}

// The SET write order is part of the plan: every prefix of it must leave effective life >= 1.
constexpr uint32_t kLifeSetStepCapacity = 5;
constexpr uint32_t kLifeRecordCurrentOffset = 0;
constexpr uint32_t kLifeRecordPendingOffset = 4;
constexpr uint32_t kLifeRecordReserveOffset = 8;
constexpr uint32_t kLifeRecordSettleTimerOffset = 12;

struct LifeSetStep {
    uint32_t offset = 0;
    int32_t value = 0;
};

struct LifeSetPlan {
    RefillVerdict verdict = RefillVerdict::Corrupt;
    uint32_t stepCount = 0;
    LifeSetStep steps[kLifeSetStepCapacity] = {};
    int32_t current = 0;
    int32_t pendingDamage = 0;
    int32_t reserve = 0;
    int32_t settleTimer = 0;
};

constexpr bool validLifeFixedPointSource(const LifeRecordImage& image,
                                         int64_t maxLife) {
    if (maxLife < kLifeUnitsPerLevel || !refillFitsInt32(maxLife))
        return false;
    if (image.current < 0 || image.pendingDamage < 0
        || image.reserve < 0 || image.settleTimer < 0) {
        return false;
    }
    if (!refillFitsInt32(image.current)
        || !refillFitsInt32(image.pendingDamage)
        || !refillFitsInt32(image.reserve)
        || !refillFitsInt32(image.settleTimer)
        || image.current > maxLife) {
        return false;
    }
    const int64_t total = image.current + image.reserve;
    const int64_t effective = total - image.pendingDamage;
    return refillFitsInt32(total) && refillFitsInt32(effective)
        && effective >= 0;
}

constexpr LifeSetPlan planLifeFixedPoint(const LifeRecordImage& image,
                                         int64_t maxLife,
                                         int64_t requested,
                                         int64_t minimum) {
    LifeSetPlan plan{};
    if (!validLifeFixedPointSource(image, maxLife)) return plan;
    const int64_t total = image.current + image.reserve;
    const int64_t effective = total - image.pendingDamage;
    if (effective < minimum) return plan;
    if (requested < minimum || requested > maxLife) {
        plan.verdict = RefillVerdict::Invalid;
        return plan;
    }
    const int64_t raised = image.current > requested ? image.current : requested;
    const int64_t intermediate = raised + image.reserve;
    if (!refillFitsInt32(raised) || !refillFitsInt32(intermediate)
        || intermediate - image.pendingDamage < minimum) {
        return plan;
    }

    plan.steps[0] = LifeSetStep{kLifeRecordCurrentOffset,
                                static_cast<int32_t>(raised)};
    plan.steps[1] = LifeSetStep{kLifeRecordPendingOffset, 0};
    plan.steps[2] = LifeSetStep{kLifeRecordReserveOffset, 0};
    plan.steps[3] = LifeSetStep{kLifeRecordSettleTimerOffset, 0};
    plan.stepCount = 4;
    if (raised != requested) {
        plan.steps[4] = LifeSetStep{kLifeRecordCurrentOffset,
                                    static_cast<int32_t>(requested)};
        plan.stepCount = 5;
    }
    plan.verdict = RefillVerdict::Apply;
    plan.current = static_cast<int32_t>(requested);
    plan.pendingDamage = 0;
    plan.reserve = 0;
    plan.settleTimer = 0;
    return plan;
}

constexpr LifeSetPlan planLifeSet(const LifeRecordImage& image,
                                  int64_t maxLife, int64_t requested) {
    return planLifeFixedPoint(image, maxLife, requested, 1);
}

constexpr LifeSetPlan planLifeReset(const LifeRecordImage& image,
                                    int64_t maxLife, bool restore) {
    if (!validLifeFixedPointSource(image, maxLife)) return LifeSetPlan{};
    const int64_t effective =
        image.current + image.reserve - image.pendingDamage;
    LifeSetPlan plan = planLifeFixedPoint(
        image, maxLife, restore ? maxLife : effective, 0);
    if (plan.verdict == RefillVerdict::Invalid)
        plan.verdict = RefillVerdict::Corrupt;
    return plan;
}

struct GuardGaugeRefillPlan {
    RefillVerdict verdict = RefillVerdict::Corrupt;
    int32_t writeValue = 0;
};

constexpr GuardGaugeRefillPlan planGuardGaugeRefill(int64_t observed) {
    GuardGaugeRefillPlan plan{};
    if (observed < 0 || observed > kGuardGaugeFull) return plan;
    plan.verdict = RefillVerdict::Apply;
    plan.writeValue = kGuardGaugeFull;
    return plan;
}

struct GuardGaugeSetPlan {
    RefillVerdict verdict = RefillVerdict::Corrupt;
    int32_t writeValue = 0;
};

constexpr GuardGaugeSetPlan planGuardGaugeSet(int64_t observed,
                                              int64_t requested) {
    GuardGaugeSetPlan plan{};
    if (observed < 0 || observed > kGuardGaugeFull) return plan;
    if (requested < 0 || requested > kGuardGaugeFull) {
        plan.verdict = RefillVerdict::Invalid;
        return plan;
    }
    plan.verdict = RefillVerdict::Apply;
    plan.writeValue = static_cast<int32_t>(requested);
    return plan;
}

// EX record: completed stocks 0..9, fraction 0..53, four per-slot pending lanes, a spend-animation state. Canonical full is 9 stocks with fraction 0.
constexpr int32_t kExFractionFull = 0;
constexpr uint32_t kExPendingLaneCount = 4;
constexpr int32_t kExSpendStateSingle = -1;
constexpr int32_t kExSpendStateMulti = -2;

struct ExRefillPlan {
    RefillVerdict verdict = RefillVerdict::Corrupt;
    int32_t stocks = 0;
    int32_t fraction = 0;
};

struct ExResetPlan {
    RefillVerdict verdict = RefillVerdict::Corrupt;
    int32_t stocks = 0;
    int32_t fraction = 0;
};

// Never plan a write to the pending lanes: EX is granted first and debited afterwards, so zeroing a lane makes a granted super free.
struct ExRestoreTarget {
    bool pinned = false;
    int32_t stocks = kExStocksFull;
    int32_t fraction = kExFractionFull;
};

constexpr bool exRestoreTargetValid(const ExRestoreTarget& target) {
    return target.stocks >= 0 && target.stocks <= kExStocksFull
        && target.fraction >= 0 && target.fraction <= kExFractionMax
        && !(target.stocks == kExStocksFull && target.fraction != 0);
}

constexpr ExResetPlan planExReset(
        int64_t stocks, int64_t fraction, bool restore,
        const ExRestoreTarget& target = ExRestoreTarget{}) {
    ExResetPlan plan{};
    if (stocks < 0 || stocks > kExStocksFull
        || fraction < 0 || fraction > kExFractionMax) {
        return plan;
    }
    const bool useTarget = restore && target.pinned
        && exRestoreTargetValid(target);
    plan.verdict = RefillVerdict::Apply;
    plan.stocks = restore
        ? (useTarget ? target.stocks : kExStocksFull)
        : static_cast<int32_t>(stocks);
    plan.fraction = restore
        ? (useTarget ? target.fraction : kExFractionFull)
        : static_cast<int32_t>(fraction);
    return plan;
}

constexpr ExRefillPlan planExRefill(
        int64_t state, int64_t stocks, int64_t fraction,
        const int64_t (&pendingLanes)[kExPendingLaneCount],
        const ExRestoreTarget& target = ExRestoreTarget{}) {
    ExRefillPlan plan{};
    if (state == kExSpendStateSingle || state == kExSpendStateMulti) {
        plan.verdict = RefillVerdict::Busy;
        return plan;
    }
    for (uint32_t lane = 0; lane < kExPendingLaneCount; ++lane) {
        if (pendingLanes[lane] != 0) {
            plan.verdict = RefillVerdict::Busy;
            return plan;
        }
    }
    if (stocks < 0 || stocks > kExStocksFull) return plan;
    if (fraction < 0 || fraction > kExFractionMax) return plan;
    plan.verdict = RefillVerdict::Apply;
    const bool usable = target.pinned && exRestoreTargetValid(target);
    plan.stocks = usable ? target.stocks : kExStocksFull;
    plan.fraction = usable ? target.fraction : kExFractionFull;
    return plan;
}

struct ExSetPlan {
    RefillVerdict verdict = RefillVerdict::Corrupt;
    int32_t stocks = 0;
    int32_t fraction = 0;
};

constexpr ExSetPlan planExSet(
        int64_t state, int64_t stocks, int64_t fraction,
        const int64_t (&pendingLanes)[kExPendingLaneCount],
        int64_t requestedStocks, int64_t requestedFraction) {
    ExSetPlan plan{};
    if (state == kExSpendStateSingle || state == kExSpendStateMulti) {
        plan.verdict = RefillVerdict::Busy;
        return plan;
    }
    for (uint32_t lane = 0; lane < kExPendingLaneCount; ++lane) {
        if (pendingLanes[lane] != 0) {
            plan.verdict = RefillVerdict::Busy;
            return plan;
        }
    }
    if (stocks < 0 || stocks > kExStocksFull) return plan;
    if (fraction < 0 || fraction > kExFractionMax) return plan;
    if (requestedStocks < 0 || requestedStocks > kExStocksFull
        || requestedFraction < 0 || requestedFraction > kExFractionMax
        || (requestedStocks == kExStocksFull
            && requestedFraction != kExFractionFull)) {
        plan.verdict = RefillVerdict::Invalid;
        return plan;
    }
    plan.verdict = RefillVerdict::Apply;
    plan.stocks = static_cast<int32_t>(requestedStocks);
    plan.fraction = static_cast<int32_t>(requestedFraction);
    return plan;
}

struct ExFractionChargePlan {
    RefillVerdict verdict = RefillVerdict::Corrupt;
    bool write = false;
    int32_t stocks = 0;
    int32_t fraction = 0;
};

constexpr ExFractionChargePlan planExFractionCharge(
        int64_t state, int64_t stocks, int64_t fraction,
        const int64_t (&pendingLanes)[kExPendingLaneCount]) {
    ExFractionChargePlan plan{};
    if (state == kExSpendStateSingle || state == kExSpendStateMulti) {
        plan.verdict = RefillVerdict::Busy;
        return plan;
    }
    for (uint32_t lane = 0; lane < kExPendingLaneCount; ++lane) {
        if (pendingLanes[lane] != 0) {
            plan.verdict = RefillVerdict::Busy;
            return plan;
        }
    }
    if (stocks < 0 || stocks > kExStocksFull
        || fraction < 0 || fraction > kExFractionMax
        || (stocks == kExStocksFull && fraction != 0)) {
        return plan;
    }
    plan.verdict = RefillVerdict::Apply;
    plan.stocks = static_cast<int32_t>(stocks);
    plan.fraction = static_cast<int32_t>(fraction);
    plan.write = true;
    if (stocks == kExStocksFull) {
        plan.stocks = 0;
        plan.fraction = kExFractionFull;
    } else if (fraction == kExFractionMax) {
        plan.stocks = static_cast<int32_t>(stocks + 1);
        plan.fraction = kExFractionFull;
    } else {
        plan.fraction = static_cast<int32_t>(fraction + 1);
    }
    return plan;
}

struct MeterChargeCadencePlan {
    uint32_t nextCounter = 0u;
    bool fire = false;
};

constexpr MeterChargeCadencePlan planMeterChargeCadence(
        uint32_t counter, bool held, bool admitted, uint32_t interval) {
    MeterChargeCadencePlan plan{};
    if (!held || interval < 1u || interval > 60u) return plan;
    if (!admitted) {
        plan.nextCounter = counter < interval ? counter : 0u;
        return plan;
    }
    plan.nextCounter = counter + 1u;
    if (plan.nextCounter >= interval) {
        plan.nextCounter = 0u;
        plan.fire = true;
    }
    return plan;
}

static_assert(sizeof(TrainingCommandV1) == 32,
              "training command ABI changed");
static_assert(sizeof(TrainingStatusV1)
                  == 1104u
                     + ((25u + 3u * kTrainingTriggerEventCount)
                        * sizeof(uint32_t))
                     + ((2u + kTrainingTriggerEventCount)
                        * sizeof(uint32_t))
                     + 16u * sizeof(uint32_t)
                     + 4u * sizeof(uint32_t)
                     + (3u + 3u * kTrainingActorReportCapacity)
                           * sizeof(uint32_t)
                     + 7u * sizeof(uint32_t)
                     + 3u * sizeof(uint32_t)
                     + 4u * sizeof(uint32_t)
                     + 2u * sizeof(uint32_t)
                     + 30u * sizeof(uint32_t)
                     + 3u * sizeof(uint32_t)
                     + 1u * sizeof(uint32_t)
                     + 7u * sizeof(uint32_t)
                     + 3u * sizeof(uint32_t)
                     + 1u * sizeof(uint32_t)
                     + 5u * sizeof(uint32_t),
              "training action-trigger status ABI changed");
static_assert(offsetof(TrainingCommandV1, commandKind) == 16
                  && offsetof(TrainingCommandV1, flags) == 28,
              "training command ABI offsets changed");
static_assert(offsetof(TrainingStatusV1, explicitTrainingPresent) == 8
                  && offsetof(TrainingStatusV1, p1FinalLogicalMask) == 68
                  && offsetof(TrainingStatusV1, macroFrameCounts) == 184
                  && offsetof(TrainingStatusV1, disableText) == 280
                  && offsetof(TrainingStatusV1, resultText) == 472
                  && offsetof(TrainingStatusV1, combatReadoutValid)
                         == kTrainingStatusV1BaseSize
                  && offsetof(TrainingStatusV1, lastHitCause) == 640
                  && offsetof(TrainingStatusV1, p2MaxComboDamageDealt) == 692,
              "training status ABI offsets changed");
static_assert(offsetof(TrainingStatusV1, p1LifeCurrent)
                      == kTrainingStatusV1CombatTier
                  && offsetof(TrainingStatusV1, p2LifeSettleTimer) == 724
                  && offsetof(TrainingStatusV1, fighterReadoutValid) == 728
                  && offsetof(TrainingStatusV1, p1ActionState) == 732
                  && offsetof(TrainingStatusV1, p2ActionState) == 772
                  && offsetof(TrainingStatusV1, p2PositionY) == 808,
              "training status fighter-tier offsets changed");
static_assert(offsetof(TrainingStatusV1, holdReasons)
                      == kTrainingStatusV1FighterTier
                  && offsetof(TrainingStatusV1,
                              guardHeuristicPostureDivergences) == 816
                  && offsetof(TrainingStatusV1, p1PostureClass) == 828
                  && offsetof(TrainingStatusV1, resourcePolicyValid) == 836
                  && offsetof(TrainingStatusV1, bothFightersNeutral) == 864
                  && offsetof(TrainingStatusV1, lifeSetMax) == 948
                  && offsetof(TrainingStatusV1, recoveryReadoutValid) == 976
                  && offsetof(TrainingStatusV1, recoveryState) == 1016
                  && offsetof(TrainingStatusV1, recoveryContactTestUnknown)
                         == 1068
                  && offsetof(TrainingStatusV1, p1WallFlags)
                         == kTrainingStatusV1RecoveryTier,
              "training status recovery-tier offsets changed");
static_assert(offsetof(TrainingStatusV1, p1WallFlags) == 1072
                  && offsetof(TrainingStatusV1, p2WallFlags) == 1076,
              "training status wall-tier offsets changed");
static_assert(offsetof(TrainingStatusV1, recoveryEvaluatorReason)
                      == kTrainingStatusV1WallTier
                  && offsetof(TrainingStatusV1, recoveryStandDownMask) == 1084
                  && offsetof(TrainingStatusV1, recoveryArmableMask) == 1088
                  && offsetof(TrainingStatusV1, recoveryTapsEmitted) == 1092
                  && offsetof(TrainingStatusV1, meterChargeTarget)
                         == kTrainingStatusV1RecoveryDiagTier,
              "training status recovery-diagnostic-tier offsets changed");
static_assert(offsetof(TrainingStatusV1, meterChargeTarget) == 1096
                  && offsetof(TrainingStatusV1, meterChargeInterval) == 1100
                  && offsetof(TrainingStatusV1, combatToolsReadoutValid)
                         == kTrainingStatusV1MeterChargeTier,
              "training status meter-charge-tier offsets changed");
static_assert(offsetof(TrainingStatusV1, combatToolsReadoutValid) == 1104
                  && offsetof(TrainingStatusV1, throwTechChancePercent) == 1108
                  && offsetof(TrainingStatusV1, triggerMacroSlot) == 1128
                  && offsetof(TrainingStatusV1, triggerPendingCount)
                         == 1128u
                            + 3u * kTrainingTriggerEventCount
                                  * sizeof(uint32_t)
                  && offsetof(TrainingStatusV1, frameAdvantageEnabled)
                         == 1156u
                            + 3u * kTrainingTriggerEventCount
                                  * sizeof(uint32_t)
                  && offsetof(TrainingStatusV1, triggerMoveCatalogId)
                         == kTrainingStatusV1CombatToolsTier
                  && offsetof(TrainingStatusV1, triggerMoveIndex)
                         == kTrainingStatusV1CombatToolsTier
                            + sizeof(uint32_t)
                  && offsetof(TrainingStatusV1, triggerLastMoveIndex)
                         == kTrainingStatusV1CombatToolsTier
                            + (1u + kTrainingTriggerEventCount)
                                  * sizeof(uint32_t)
                  && offsetof(TrainingStatusV1, debugOverlayEnabled)
                         == kTrainingStatusV1ActionTriggersTier
                  && offsetof(TrainingStatusV1, wakeupLastOutcome)
                         == kTrainingStatusV1ActionTriggersTier
                            + 15u * sizeof(uint32_t)
                  && offsetof(TrainingStatusV1, stateChangeLogEnabled)
                         == kTrainingStatusV1DebugOverlayTier
                  && offsetof(TrainingStatusV1, triggerSuppressionReason)
                         == kTrainingStatusV1DebugOverlayTier
                            + 3u * sizeof(uint32_t)
                  && offsetof(TrainingStatusV1, actorPoolValid)
                         == kTrainingStatusV1StateLogTier
                  && offsetof(TrainingStatusV1, actorPoolAction)
                         == kTrainingStatusV1StateLogTier
                            + 3u * sizeof(uint32_t)
                  && offsetof(TrainingStatusV1, nativeHudOwner)
                         == kTrainingStatusV1ActorPoolTier
                  && offsetof(TrainingStatusV1, nativeHudRequestedGlobalMask)
                         == kTrainingStatusV1NativeHudTier
                  && offsetof(TrainingStatusV1, frameTraceEnabled)
                         == kTrainingStatusV1NativeHudRequestedTier
                  && offsetof(TrainingStatusV1, frameTraceCapped)
                         == kTrainingStatusV1NativeHudRequestedTier
                            + 3u * sizeof(uint32_t)
                  && offsetof(TrainingStatusV1, frameAdvantageGapValid)
                         == kTrainingStatusV1FrameTraceTier
                  && offsetof(TrainingStatusV1, faultlessPolicy)
                         == kTrainingStatusV1FrameAdvantageGapTier
                  && offsetof(TrainingStatusV1, exRestorePinned)
                         == kTrainingStatusV1ContactPolicyTier
                  && offsetof(TrainingStatusV1, macroAfterReset)
                         == kTrainingStatusV1ExRestoreTargetTier
                  && offsetof(TrainingStatusV1, reduceP1Policy)
                         == kTrainingStatusV1MacroAfterResetTier
                  && offsetof(TrainingStatusV1, guardTimingMode)
                         == kTrainingStatusV1ReducePolicyTier
                  && offsetof(TrainingStatusV1, statusEventKind)
                         == kTrainingStatusV1PlayerSwapTier
                  && offsetof(TrainingStatusV1, playerSwap)
                         == kTrainingStatusV1GuardTimingTier
                  && sizeof(TrainingStatusV1)
                         == kTrainingStatusV1StatusMessageTier,
              "training status combat-tools-tier offsets changed");
static_assert(kTrainingStatusV1CombatTier < kTrainingStatusV1FighterTier
                  && kTrainingStatusV1FighterTier
                         < kTrainingStatusV1RecoveryTier
                  && kTrainingStatusV1RecoveryTier
                         < kTrainingStatusV1WallTier
                  && kTrainingStatusV1WallTier
                         < kTrainingStatusV1RecoveryDiagTier
                  && kTrainingStatusV1RecoveryDiagTier
                         < kTrainingStatusV1MeterChargeTier
                  && kTrainingStatusV1MeterChargeTier
                         < kTrainingStatusV1CombatToolsTier
                  && kTrainingStatusV1CombatToolsTier
                         < kTrainingStatusV1ActionTriggersTier
                  && kTrainingStatusV1ActionTriggersTier
                         < kTrainingStatusV1DebugOverlayTier
                  && kTrainingStatusV1SizeTiers[1]
                         == kTrainingStatusV1CombatTier
                  && kTrainingStatusV1SizeTiers[2]
                         == kTrainingStatusV1FighterTier
                  && kTrainingStatusV1SizeTiers[3]
                         == kTrainingStatusV1RecoveryTier
                  && kTrainingStatusV1SizeTiers[4]
                         == kTrainingStatusV1WallTier
                  && kTrainingStatusV1SizeTiers[5]
                         == kTrainingStatusV1RecoveryDiagTier
                  && kTrainingStatusV1SizeTiers[6]
                         == kTrainingStatusV1MeterChargeTier
                  && kTrainingStatusV1SizeTiers[7]
                         == kTrainingStatusV1CombatToolsTier
                  && kTrainingStatusV1SizeTiers[8]
                         == kTrainingStatusV1ActionTriggersTier
                  && kTrainingStatusV1SizeTiers[9]
                         == kTrainingStatusV1DebugOverlayTier
                  && kTrainingStatusV1SizeTiers[10]
                         == kTrainingStatusV1StateLogTier
                  && kTrainingStatusV1SizeTiers[11]
                         == kTrainingStatusV1ActorPoolTier
                  && kTrainingStatusV1SizeTiers[12]
                         == kTrainingStatusV1NativeHudTier
                  && kTrainingStatusV1SizeTiers[13]
                         == kTrainingStatusV1NativeHudRequestedTier
                  && kTrainingStatusV1SizeTiers[14]
                         == kTrainingStatusV1FrameTraceTier,
              "named status tiers must be entries in the released tier table");
static_assert(sizeof(TrainingStatusV1) % sizeof(uint32_t) == 0,
              "training status must stay word-copyable by the publisher");
static_assert(kTrainingStatusV1SizeTiers[0] == kTrainingStatusV1BaseSize,
              "the first frozen status tier is the frozen v1 prefix");
static_assert(kTrainingStatusV1SizeTiers[kTrainingStatusV1TierCount - 1]
                  == sizeof(TrainingStatusV1),
              "a status growth must append its new size to the tier table");
static_assert(trainingStatusTiersAscend(),
              "frozen status tiers must ascend and never repeat");
static_assert(std::is_standard_layout<TrainingCommandV1>::value
                  && std::is_trivially_copyable<TrainingCommandV1>::value,
              "training command must remain POD");
static_assert(std::is_standard_layout<TrainingStatusV1>::value
                  && std::is_trivially_copyable<TrainingStatusV1>::value,
              "training status must remain POD");

}
