#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "training/TrainingTypes.h"

namespace qtraining {

enum class TrainingActionSemantic : std::uint8_t {
    Unused = 0,
    UnknownOrScripted,
    Neutral,
    MovementOrTurn,
    Attack,
    Guard,
    HitReaction,
    KnockdownOrWall,
    Recovery,
    Throw,
    ThrowTechCounterpart,
    ThrowTech,
    ThrowReception,
    CinematicOrSystem,
};

enum class TrainingActionExitClass : std::uint8_t {
    Unused = 0,
    UnknownOrScripted,
    Terminal,
    Loop,
    LandingOrRuntime,
};

enum TrainingActionFlag : std::uint16_t {
    TrainingActionFlagNone = 0,
    TrainingActionFlagAuthored = 1u << 0,
    TrainingActionFlagTerminal = 1u << 1,
    TrainingActionFlagLoop = 1u << 2,
    TrainingActionFlagExplicitLoop = 1u << 3,
    TrainingActionFlagLanding = 1u << 4,
    TrainingActionFlagAttack = 1u << 5,
    TrainingActionFlagCatalogMove = 1u << 6,

    TrainingActionFlagBroadThrowDeny = 1u << 7,

    TrainingActionFlagGroundedRuntimeLanding = 1u << 8,
    TrainingActionFlagTimingDependsOnLanding = 1u << 9,
    TrainingActionFlagActiveUntilLanding = 1u << 10,
};

struct TrainingActionMeta {
    TrainingActionSemantic semantic = TrainingActionSemantic::Unused;
    TrainingActionExitClass exitClass = TrainingActionExitClass::Unused;
    std::uint16_t flags = TrainingActionFlagNone;

    constexpr bool has(TrainingActionFlag flag) const noexcept {
        return (flags & static_cast<std::uint16_t>(flag)) != 0;
    }
};

enum class TrainingNativeContactKind : std::uint8_t {
    None = 0x00,
    Hit = 0x10,
    Clash = 0x20,
    Guard = 0x40,
    AlternateActorMode = 0x80,
};

enum class TrainingNativeMoveTrigger : std::uint8_t {
    AnyContact = 0,
    Hit,
    GuardOrClash,
    NoHit,
    AlternateActorMode,
};

enum class TrainingNativeMoveEffect : std::uint8_t {
    None = 0,
    RedirectAnimationFrame,
    ChangeAction,
    ReplaceActionOnEntry,
    EndAction,
    UpdateCharacterState,
};

enum class TrainingNativeTransitionProof : std::uint8_t {
    Unproven = 0,
    ProvenEffectOnly,
    ProvenAutomaticContinuation,
    ProvenExcludedContinuation,
};

enum TrainingNativeMoveFlag : std::uint8_t {
    TrainingNativeMoveFlagNone = 0,
    TrainingNativeMoveFlagResetsFrameTimer = 1u << 0,
    TrainingNativeMoveFlagClearsHorizontalMotion = 1u << 1,
    TrainingNativeMoveFlagCaptureSequence = 1u << 2,
    TrainingNativeMoveFlagConventionalFrameAdvantageExcluded = 1u << 3,
};

inline constexpr std::uint32_t kTrainingNativeMoveUnknownValue = 0xFFFFFFFFu;

struct TrainingNativeMoveTransition {
    TrainingNativeTransitionProof proof =
        TrainingNativeTransitionProof::Unproven;
    TrainingNativeMoveTrigger trigger = TrainingNativeMoveTrigger::AnyContact;
    TrainingNativeMoveEffect effect = TrainingNativeMoveEffect::None;
    std::uint8_t flags = TrainingNativeMoveFlagNone;
    std::uint16_t nativeContactMask = 0;
    std::uint32_t targetAction = kTrainingNativeMoveUnknownValue;
    std::uint32_t targetLocalFrame = kTrainingNativeMoveUnknownValue;

    constexpr bool provenAutomaticContinuation() const noexcept {
        return proof == TrainingNativeTransitionProof::ProvenAutomaticContinuation ||
               proof == TrainingNativeTransitionProof::ProvenExcludedContinuation;
    }

    constexpr bool allowsConventionalFrameAdvantage() const noexcept {
        return proof == TrainingNativeTransitionProof::ProvenAutomaticContinuation;
    }

    constexpr bool has(TrainingNativeMoveFlag flag) const noexcept {
        return (flags & static_cast<std::uint8_t>(flag)) != 0;
    }
};

// Dispatch families for the actor value at +84 (the sub_4017F0 switch), which is independent of the CHR action at +48.
enum class TrainingDispatchFamily : std::uint8_t {
    Unmapped = 0,
    Neutral,
    GenericAction,
    RunningNormal,
    GroundDash,
    Jump,
    Guard,
    GroundHitReaction,
    AirGuardCrush,
    AirborneReaction,
    Walk,
    Crouch,
    Scripted29Or30,
    RecoveryResult,
    HardKnockdown,
    Scripted36,
    WallState,
    Thrower,
    RecursiveSystem,
    Scripted92Or215,
    Scripted96To98,
    Special150Family,
    Special152Family,
    Special157,
    Special160,
    Special161Family,
    Special165,
    Special166,
    ThrowReception,
    ThrowReception208,
};

enum class TrainingMoveCategory : std::uint8_t {
    Neutral = 0,
    Movement,
    Normal,
    CommandNormal,
    Throw,
    Special,
    Super,
    FollowUp,
    Defensive,
    PartnerCommand,
};

enum class TrainingMoveContext : std::uint8_t {
    Ground = 0,
    Air,
    Either,
    Close,
    Guard,
    FollowUp,
};

enum TrainingMoveFlag : std::uint16_t {
    TrainingMoveFlagNone = 0,
    TrainingMoveFlagAirNotation = 1u << 0,
    TrainingMoveFlagCloseQualifier = 1u << 1,
    TrainingMoveFlagPartnerActor = 1u << 2,
    TrainingMoveFlagMultiButton = 1u << 3,
    TrainingMoveFlagModeDependent = 1u << 4,
    TrainingMoveFlagFollowUp = 1u << 5,
    TrainingMoveFlagRepeatedButton = 1u << 6,
    TrainingMoveFlagGuardCancelEligible = 1u << 7,
    TrainingMoveFlagActionUnknown = 1u << 8,
};

inline constexpr std::uint16_t kTrainingMoveUnknownAction = 0xFFFFu;
inline constexpr std::uint8_t kTrainingMoveInvalidCharacter = 0xFFu;
inline constexpr std::uint32_t kTrainingMoveCatalogSchemaVersion = 1u;
inline constexpr std::uint32_t kTrainingMoveSelectableCharacterCount = 34u;
inline constexpr std::uint32_t kTrainingMoveCatalogRecordCount = 1159u;
inline constexpr std::uint32_t kTrainingMoveCatalogStepCount = 4685u;
inline constexpr std::uint32_t kTrainingMoveCatalogExcludedRowCount = 39u;
inline constexpr std::uint32_t kTrainingMoveMaximumPerCharacter = 54u;
inline constexpr std::uint32_t kTrainingMoveMaximumSequenceSteps = 9u;
inline constexpr bool kTrainingMoveProfilesClassifyEquivalently = true;
static_assert(kTrainingMoveMaximumPerCharacter
                  <= kTrainingTriggerMaximumMoveIndex,
              "generated move ordinals exceed the training command ABI");

struct TrainingMoveView {
    std::uint8_t characterId = kTrainingMoveInvalidCharacter;
    std::uint16_t moveIndex = 0;
    std::uint16_t actionId = kTrainingMoveUnknownAction;
    TrainingMoveCategory category = TrainingMoveCategory::Neutral;
    TrainingMoveContext context = TrainingMoveContext::Ground;
    std::uint16_t flags = TrainingMoveFlagNone;
    const std::uint32_t* relativeSteps = nullptr;
    std::uint8_t stepCount = 0;
    std::string_view input;
    std::string_view label;
    std::string_view actorResource;

    constexpr bool valid() const noexcept {
        return characterId != kTrainingMoveInvalidCharacter
            && moveIndex != 0 && relativeSteps != nullptr
            && stepCount != 0;
    }

    constexpr bool has(TrainingMoveFlag flag) const noexcept {
        return (flags & static_cast<std::uint16_t>(flag)) != 0;
    }
};

/* QOH parses a tap on its release frame and a hold on the second held frame, using the direction ring head at that moment; copyTrainingMoveSequence() therefore holds the direction one step past each button. */
struct TrainingMoveSequence {
    std::uint32_t relativeMasks[kTrainingMoveMaximumSequenceSteps] = {};
    std::uint8_t stepCount = 0;
};

inline constexpr std::uint32_t kTrainingActionResourceCount = 35;
inline constexpr std::uint32_t kTrainingActionStateCount = 256;
inline constexpr std::uint32_t kTrainingActionCatalogCellCount =
    kTrainingActionResourceCount * kTrainingActionStateCount;
inline constexpr std::uint32_t kTrainingActionAuthoredStateCount = 3826;
inline constexpr std::uint32_t kTrainingActionDistinctAuthoredIdCount = 175;
inline constexpr std::uint32_t kTrainingDispatchCaseCount = 101;
inline constexpr std::uint32_t kTrainingNativeMoveBehaviorRuleCount = 113;
inline constexpr std::uint32_t kTrainingNativeMoveBehaviorSourceStateCount = 69;

inline constexpr bool kTrainingActionProfilesClassifyEquivalently = true;
inline constexpr bool
    kTrainingNativeMoveBehaviorProfilesClassifyEquivalently = true;
inline constexpr char kTrainingActionNovemberProfileKey[] = "qoh99-se1126";
inline constexpr char kTrainingActionDecemberProfileKey[] = "qoh99-withse";

// characterId 0..33 = selectable order, 34 = EDIFEL, 35 = KANAKO resource, 36 = C_SERIKA resource.
TrainingActionMeta trainingActionMeta(std::uint8_t characterId,
                                      std::uint32_t action48) noexcept;

bool trainingActionAllowsConventionalFrameAdvantage(
    std::uint8_t characterId, std::uint32_t action48) noexcept;

TrainingNativeMoveTransition trainingNativeMoveTransition(
    std::uint8_t characterId,
    std::uint32_t sourceAction48,
    std::uint32_t sourceLocalFrame,
    TrainingNativeContactKind contact,
    std::uint32_t observedAction48,
    std::uint32_t observedLocalFrame) noexcept;

std::uint32_t trainingNativeMoveBehaviorRuleCount(
    std::uint8_t characterId, std::uint32_t sourceAction48) noexcept;

std::uint8_t trainingMoveSelectableCharacterId(
    std::uint8_t runtimeCharacterId) noexcept;

std::uint16_t trainingMoveCount(std::uint8_t runtimeCharacterId) noexcept;

TrainingMoveView trainingMove(std::uint8_t runtimeCharacterId,
                              std::uint16_t moveIndex) noexcept;
bool copyTrainingMoveSequence(std::uint8_t runtimeCharacterId,
                              std::uint16_t moveIndex,
                              TrainingMoveSequence& out) noexcept;

bool trainingMoveDirectTriggerEligible(
    const TrainingMoveView& move) noexcept;
bool trainingMoveGuardCancelTriggerEligible(
    const TrainingMoveView& move) noexcept;
bool trainingMoveTriggerEligible(const TrainingMoveView& move,
                                 TrainingTriggerEvent event) noexcept;
bool trainingMoveContextAllowsPosture(const TrainingMoveView& move,
                                      bool airborne) noexcept;

std::string_view trainingMoveCatalogFingerprint() noexcept;
// ABI handshake: first eight hex digits of the full fingerprint, compared before sending a move ordinal.
std::uint32_t trainingMoveCatalogId() noexcept;

TrainingDispatchFamily trainingDispatchFamily(std::uint32_t action84) noexcept;

}
