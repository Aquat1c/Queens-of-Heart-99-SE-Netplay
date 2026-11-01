#include "training/TrainingActionCatalog.h"

#include <cstddef>

#include "input/InputActions.h"

namespace qtraining {
namespace {

#include "training/generated/TrainingActionCatalogData.inc"

constexpr std::uint32_t kSemanticMask = 0x0Fu;
constexpr std::uint32_t kExitMask = 0x07u;
constexpr std::uint32_t kExitShift = 4u;
constexpr std::uint32_t kFlagsShift = 8u;

constexpr std::uint64_t kNativeResourceMask = 0x3Fu;
constexpr std::uint64_t kNativeSourceActionMask = 0xFFu;
constexpr std::uint64_t kNativeTriggerMask = 0x07u;
constexpr std::uint64_t kNativeEffectMask = 0x07u;
constexpr std::uint64_t kNativeContactMask = 0x7Fu;
constexpr std::uint64_t kNativeByteMask = 0xFFu;
constexpr std::uint32_t kNativeSourceActionShift = 6u;
constexpr std::uint32_t kNativeTriggerShift = 14u;
constexpr std::uint32_t kNativeEffectShift = 17u;
constexpr std::uint32_t kNativeContactShift = 20u;
constexpr std::uint32_t kNativeTriggerFrameShift = 27u;
constexpr std::uint32_t kNativeTargetActionShift = 35u;
constexpr std::uint32_t kNativeTargetFrameShift = 43u;
constexpr std::uint32_t kNativeResetTimerShift = 51u;
constexpr std::uint32_t kNativeClearMotionShift = 52u;
constexpr std::uint32_t kNativeCaptureShift = 53u;
constexpr std::uint32_t kNativeNoByte = 0xFFu;

static_assert(kGeneratedResourceCount == kTrainingActionResourceCount,
              "generated action resource count drifted");
static_assert(kGeneratedStateCount == kTrainingActionStateCount,
              "generated action state count drifted");
static_assert(kGeneratedAuthoredStateCount ==
                  kTrainingActionAuthoredStateCount,
              "generated authored-state count drifted");
static_assert(kGeneratedDistinctAuthoredIdCount ==
                  kTrainingActionDistinctAuthoredIdCount,
              "generated authored-ID count drifted");
static_assert(kGeneratedProfilesClassifyEquivalently ==
                  kTrainingActionProfilesClassifyEquivalently,
              "generated profile evidence is not equivalent");
static_assert(kGeneratedNativeMoveBehaviorRuleCount ==
                  kTrainingNativeMoveBehaviorRuleCount,
              "generated native move behavior count drifted");
static_assert(kGeneratedNativeMoveBehaviorSourceStateCount ==
                  kTrainingNativeMoveBehaviorSourceStateCount,
              "generated native move behavior source count drifted");
static_assert(kGeneratedNativeMoveBehaviorProfilesClassifyEquivalently ==
                  kTrainingNativeMoveBehaviorProfilesClassifyEquivalently,
              "generated native move behavior profile evidence differs");
static_assert(kGeneratedTrainingMoveRecordCount ==
                  kTrainingMoveCatalogRecordCount,
              "generated playable move count drifted");
static_assert(kGeneratedTrainingMoveStepCount ==
                  kTrainingMoveCatalogStepCount,
              "generated playable move step count drifted");
static_assert(kGeneratedTrainingMoveExcludedCount ==
                  kTrainingMoveCatalogExcludedRowCount,
              "generated playable move exclusion count drifted");
static_assert(kGeneratedTrainingMoveMaximumPerCharacter ==
                  kTrainingMoveMaximumPerCharacter,
              "generated maximum per-character move count drifted");
static_assert(kGeneratedTrainingMoveMaximumSequenceSteps ==
                  kTrainingMoveMaximumSequenceSteps,
              "generated maximum move sequence length drifted");

constexpr std::uint32_t resourceIndex(std::uint8_t characterId) noexcept {
    if (characterId < kTrainingActionResourceCount) return characterId;
    if (characterId == 35u) return 24u;
    if (characterId == 36u) return 25u;
    return kTrainingActionResourceCount;
}

struct DecodedNativeMoveRule {
    std::uint8_t resource = 0;
    std::uint8_t sourceAction = 0;
    TrainingNativeMoveTrigger trigger = TrainingNativeMoveTrigger::AnyContact;
    TrainingNativeMoveEffect effect = TrainingNativeMoveEffect::None;
    std::uint8_t contactMask = 0;
    std::uint8_t triggerFrame = kNativeNoByte;
    std::uint8_t targetAction = kNativeNoByte;
    std::uint8_t targetFrame = kNativeNoByte;
    std::uint8_t flags = TrainingNativeMoveFlagNone;
};

constexpr DecodedNativeMoveRule decodeNativeMoveRule(
    std::uint64_t packed) noexcept {
    DecodedNativeMoveRule result{};
    result.resource = static_cast<std::uint8_t>(packed & kNativeResourceMask);
    result.sourceAction = static_cast<std::uint8_t>(
        (packed >> kNativeSourceActionShift) & kNativeSourceActionMask);
    result.trigger = static_cast<TrainingNativeMoveTrigger>(
        (packed >> kNativeTriggerShift) & kNativeTriggerMask);
    result.effect = static_cast<TrainingNativeMoveEffect>(
        (packed >> kNativeEffectShift) & kNativeEffectMask);
    result.contactMask = static_cast<std::uint8_t>(
        (packed >> kNativeContactShift) & kNativeContactMask);
    result.triggerFrame = static_cast<std::uint8_t>(
        (packed >> kNativeTriggerFrameShift) & kNativeByteMask);
    result.targetAction = static_cast<std::uint8_t>(
        (packed >> kNativeTargetActionShift) & kNativeByteMask);
    result.targetFrame = static_cast<std::uint8_t>(
        (packed >> kNativeTargetFrameShift) & kNativeByteMask);
    if (((packed >> kNativeResetTimerShift) & 1u) != 0)
        result.flags |= TrainingNativeMoveFlagResetsFrameTimer;
    if (((packed >> kNativeClearMotionShift) & 1u) != 0)
        result.flags |= TrainingNativeMoveFlagClearsHorizontalMotion;
    if (((packed >> kNativeCaptureShift) & 1u) != 0)
        result.flags |= TrainingNativeMoveFlagCaptureSequence;
    return result;
}

constexpr bool validContact(TrainingNativeContactKind contact) noexcept {
    switch (contact) {
        case TrainingNativeContactKind::None:
        case TrainingNativeContactKind::Hit:
        case TrainingNativeContactKind::Clash:
        case TrainingNativeContactKind::Guard:
        case TrainingNativeContactKind::AlternateActorMode:
            return true;
    }
    return false;
}

constexpr bool triggerMatches(TrainingNativeMoveTrigger trigger,
                              TrainingNativeContactKind contact) noexcept {
    switch (trigger) {
        case TrainingNativeMoveTrigger::AnyContact:
            return contact == TrainingNativeContactKind::Hit ||
                   contact == TrainingNativeContactKind::Clash ||
                   contact == TrainingNativeContactKind::Guard;
        case TrainingNativeMoveTrigger::Hit:
            return contact == TrainingNativeContactKind::Hit;
        case TrainingNativeMoveTrigger::GuardOrClash:
            return contact == TrainingNativeContactKind::Guard ||
                   contact == TrainingNativeContactKind::Clash;
        case TrainingNativeMoveTrigger::NoHit:
            return contact == TrainingNativeContactKind::None ||
                   contact == TrainingNativeContactKind::Guard ||
                   contact == TrainingNativeContactKind::Clash;
        case TrainingNativeMoveTrigger::AlternateActorMode:
            return contact == TrainingNativeContactKind::AlternateActorMode;
    }
    return false;
}

constexpr std::uint32_t unpackRangeBegin(std::uint16_t range) noexcept {
    return range & 0xFFu;
}

constexpr std::uint32_t unpackRangeCount(std::uint16_t range) noexcept {
    return range >> 8u;
}

}

TrainingActionMeta trainingActionMeta(std::uint8_t characterId,
                                      std::uint32_t action48) noexcept {
    const std::uint32_t resource = resourceIndex(characterId);
    if (resource >= kTrainingActionResourceCount ||
        action48 >= kTrainingActionStateCount) {
        return {};
    }

    const std::uint32_t packed = kTrainingActionPacked[resource][action48];
    TrainingActionMeta result{};
    result.semantic = static_cast<TrainingActionSemantic>(
        packed & kSemanticMask);
    result.exitClass = static_cast<TrainingActionExitClass>(
        (packed >> kExitShift) & kExitMask);
    result.flags = static_cast<std::uint16_t>(packed >> kFlagsShift);
    return result;
}

bool trainingActionAllowsConventionalFrameAdvantage(
    std::uint8_t characterId, std::uint32_t action48) noexcept {
    const TrainingActionMeta meta = trainingActionMeta(characterId, action48);
    if (!meta.has(TrainingActionFlagAuthored) || action48 == 71u ||
        action48 == 72u || action48 == 73u || action48 >= 200u) {
        return false;
    }

    switch (meta.semantic) {
        case TrainingActionSemantic::Unused:
        case TrainingActionSemantic::UnknownOrScripted:
        case TrainingActionSemantic::Throw:
        case TrainingActionSemantic::ThrowTechCounterpart:
        case TrainingActionSemantic::ThrowTech:
        case TrainingActionSemantic::ThrowReception:
        case TrainingActionSemantic::CinematicOrSystem:
            return false;
        case TrainingActionSemantic::Neutral:
        case TrainingActionSemantic::MovementOrTurn:
        case TrainingActionSemantic::Attack:
        case TrainingActionSemantic::Guard:
        case TrainingActionSemantic::HitReaction:
        case TrainingActionSemantic::KnockdownOrWall:
        case TrainingActionSemantic::Recovery:
            return true;
    }
    return false;
}

std::uint32_t trainingNativeMoveBehaviorRuleCount(
    std::uint8_t characterId, std::uint32_t sourceAction48) noexcept {
    const std::uint32_t resource = resourceIndex(characterId);
    if (resource >= kTrainingActionResourceCount ||
        sourceAction48 >= kTrainingActionStateCount) {
        return 0;
    }

    const std::uint16_t range = kTrainingNativeMoveBehaviorResourceRanges[resource];
    const std::uint32_t begin = unpackRangeBegin(range);
    const std::uint32_t end = begin + unpackRangeCount(range);
    std::uint32_t count = 0;
    for (std::uint32_t index = begin; index < end; ++index) {
        const DecodedNativeMoveRule rule = decodeNativeMoveRule(
            kTrainingNativeMoveBehaviorPacked[index]);
        if (rule.sourceAction == sourceAction48) ++count;
    }
    return count;
}

std::uint8_t trainingMoveSelectableCharacterId(
    std::uint8_t runtimeCharacterId) noexcept {
    if (runtimeCharacterId < kTrainingMoveSelectableCharacterCount)
        return runtimeCharacterId;
    switch (runtimeCharacterId) {
        case 34u: return 4u;
        case 35u: return 1u;
        case 36u: return 25u;
        default: return kTrainingMoveInvalidCharacter;
    }
}

std::uint16_t trainingMoveCount(
    std::uint8_t runtimeCharacterId) noexcept {
    const std::uint8_t character =
        trainingMoveSelectableCharacterId(runtimeCharacterId);
    if (character == kTrainingMoveInvalidCharacter) return 0u;
    const std::uint16_t begin = kTrainingMoveCharacterOffsets[character];
    const std::uint16_t end = kTrainingMoveCharacterOffsets[character + 1u];
    return static_cast<std::uint16_t>(end - begin);
}

TrainingMoveView trainingMove(std::uint8_t runtimeCharacterId,
                              std::uint16_t moveIndex) noexcept {
    TrainingMoveView result{};
    const std::uint8_t character =
        trainingMoveSelectableCharacterId(runtimeCharacterId);
    if (character == kTrainingMoveInvalidCharacter || moveIndex == 0u)
        return result;

    const std::uint16_t begin = kTrainingMoveCharacterOffsets[character];
    const std::uint16_t end = kTrainingMoveCharacterOffsets[character + 1u];
    const std::uint32_t recordIndex = static_cast<std::uint32_t>(begin)
        + static_cast<std::uint32_t>(moveIndex - 1u);
    if (recordIndex >= end || recordIndex >= kGeneratedTrainingMoveRecordCount)
        return result;

    const GeneratedTrainingMoveRecord& record =
        kTrainingMoveRecords[recordIndex];
    const std::uint32_t stepEnd = static_cast<std::uint32_t>(record.stepBegin)
        + static_cast<std::uint32_t>(record.stepCount);
    if (record.stepCount == 0u
        || record.stepCount > kTrainingMoveMaximumSequenceSteps
        || stepEnd > kGeneratedTrainingMoveStepCount
        || record.category
            > static_cast<std::uint8_t>(TrainingMoveCategory::PartnerCommand)
        || record.context
            > static_cast<std::uint8_t>(TrainingMoveContext::FollowUp)) {
        return result;
    }

    result.characterId = character;
    result.moveIndex = moveIndex;
    result.actionId = record.actionId;
    result.category = static_cast<TrainingMoveCategory>(record.category);
    result.context = static_cast<TrainingMoveContext>(record.context);
    result.flags = record.flags;
    result.relativeSteps = kTrainingMoveRelativeSteps + record.stepBegin;
    result.stepCount = record.stepCount;
    result.input = record.input;
    result.label = record.label;
    result.actorResource = record.actorResource;
    return result;
}

bool copyTrainingMoveSequence(std::uint8_t runtimeCharacterId,
                              std::uint16_t moveIndex,
                              TrainingMoveSequence& out) noexcept {
    out = TrainingMoveSequence{};
    const TrainingMoveView move = trainingMove(runtimeCharacterId, moveIndex);
    if (!move.valid()) return false;

    constexpr std::uint32_t kDirections = qinput::kLogicalDirectionMask;
    constexpr std::uint32_t kButtons = qinput::kLogicalButtonMask;
    std::uint8_t written = 0u;
    for (std::uint8_t index = 0u; index < move.stepCount; ++index) {
        if (written >= kTrainingMoveMaximumSequenceSteps) {
            out = TrainingMoveSequence{};
            for (std::uint8_t raw = 0u; raw < move.stepCount; ++raw)
                out.relativeMasks[raw] = move.relativeSteps[raw];
            out.stepCount = move.stepCount;
            return true;
        }
        const std::uint32_t step = move.relativeSteps[index];
        out.relativeMasks[written++] = step;

        const bool presses = (step & kButtons) != 0u;
        const bool last = index + 1u >= move.stepCount;
        if (!presses || last) continue;
        const std::uint32_t next = move.relativeSteps[index + 1u];
        const bool nextKeepsDirection = (next & kButtons) == 0u
            && (next & kDirections) == (step & kDirections);
        if (nextKeepsDirection) continue;
        if (written >= kTrainingMoveMaximumSequenceSteps) {
            out = TrainingMoveSequence{};
            for (std::uint8_t raw = 0u; raw < move.stepCount; ++raw)
                out.relativeMasks[raw] = move.relativeSteps[raw];
            out.stepCount = move.stepCount;
            return true;
        }
        out.relativeMasks[written++] = step & kDirections;
    }
    out.stepCount = written;
    return true;
}

bool trainingMoveDirectTriggerEligible(
    const TrainingMoveView& move) noexcept {
    if (!move.valid()
        || move.has(TrainingMoveFlagFollowUp)
        || move.has(TrainingMoveFlagRepeatedButton)) {
        return false;
    }
    switch (move.category) {
        case TrainingMoveCategory::Movement:
        case TrainingMoveCategory::Normal:
        case TrainingMoveCategory::CommandNormal:
        case TrainingMoveCategory::Throw:
        case TrainingMoveCategory::Special:
        case TrainingMoveCategory::Super:
        case TrainingMoveCategory::PartnerCommand:
            break;
        case TrainingMoveCategory::Neutral:
        case TrainingMoveCategory::FollowUp:
        case TrainingMoveCategory::Defensive:
            return false;
    }
    if (move.has(TrainingMoveFlagModeDependent)) {
        if (move.has(TrainingMoveFlagPartnerActor)) return true;

        if (move.category == TrainingMoveCategory::PartnerCommand) {
            return move.input.size() >= 5u
                && move.input.substr(0u, 5u) == "214+D";
        }
        return false;
    }
    return true;
}

bool trainingMoveGuardCancelTriggerEligible(
    const TrainingMoveView& move) noexcept {
    if (!trainingMoveDirectTriggerEligible(move)
        || !move.has(TrainingMoveFlagGuardCancelEligible)) {
        return false;
    }
    return move.category == TrainingMoveCategory::Special
        || move.category == TrainingMoveCategory::Super
        || move.category == TrainingMoveCategory::PartnerCommand;
}

bool trainingMoveTriggerEligible(const TrainingMoveView& move,
                                 TrainingTriggerEvent event) noexcept {
    if (!trainingTriggerEventValid(event)
        || !trainingMoveDirectTriggerEligible(move)) {
        return false;
    }
    if (event == TrainingTriggerEvent::GuardCancel)
        return trainingMoveGuardCancelTriggerEligible(move);

    switch (event) {
        case TrainingTriggerEvent::AfterAirRecovery:
            return move.context == TrainingMoveContext::Air
                || move.context == TrainingMoveContext::Either;
        case TrainingTriggerEvent::OnWakeup:
        case TrainingTriggerEvent::AfterGroundRecovery:
            return move.context == TrainingMoveContext::Ground
                || move.context == TrainingMoveContext::Close
                || move.context == TrainingMoveContext::Either;
        case TrainingTriggerEvent::OnHit:
        case TrainingTriggerEvent::AfterBlock:
        case TrainingTriggerEvent::AfterHitstun:
        case TrainingTriggerEvent::AfterThrowTech:
            return true;
        case TrainingTriggerEvent::GuardCancel:
        case TrainingTriggerEvent::Count:
            break;
    }
    return false;
}

bool trainingMoveContextAllowsPosture(const TrainingMoveView& move,
                                      bool airborne) noexcept {
    if (!move.valid()) return false;
    switch (move.context) {
        case TrainingMoveContext::Air:
            return airborne;
        case TrainingMoveContext::Ground:
        case TrainingMoveContext::Close:
            return !airborne;
        case TrainingMoveContext::Either:
            return true;
        case TrainingMoveContext::Guard:
        case TrainingMoveContext::FollowUp:
            return false;
    }
    return false;
}

std::string_view trainingMoveCatalogFingerprint() noexcept {
    return kGeneratedTrainingMoveCatalogSha256;
}

std::uint32_t trainingMoveCatalogId() noexcept {
    std::uint32_t result = 0u;
    for (std::uint32_t index = 0u; index < 8u; ++index) {
        const char ch = kGeneratedTrainingMoveCatalogSha256[index];
        const std::uint32_t nibble = ch >= '0' && ch <= '9'
            ? static_cast<std::uint32_t>(ch - '0')
            : ch >= 'A' && ch <= 'F'
                ? static_cast<std::uint32_t>(ch - 'A' + 10)
                : 0xFFFFFFFFu;
        if (nibble > 0x0Fu) return 0u;
        result = (result << 4u) | nibble;
    }
    return result;
}

TrainingNativeMoveTransition trainingNativeMoveTransition(
    std::uint8_t characterId,
    std::uint32_t sourceAction48,
    std::uint32_t sourceLocalFrame,
    TrainingNativeContactKind contact,
    std::uint32_t observedAction48,
    std::uint32_t observedLocalFrame) noexcept {
    const std::uint32_t resource = resourceIndex(characterId);
    if (resource >= kTrainingActionResourceCount ||
        sourceAction48 >= kTrainingActionStateCount ||
        observedAction48 >= kTrainingActionStateCount ||
        !validContact(contact)) {
        return {};
    }

    const std::uint16_t range = kTrainingNativeMoveBehaviorResourceRanges[resource];
    const std::uint32_t begin = unpackRangeBegin(range);
    const std::uint32_t end = begin + unpackRangeCount(range);
    TrainingNativeMoveTransition matched{};
    std::uint32_t matchCount = 0;

    for (std::uint32_t index = begin; index < end; ++index) {
        const DecodedNativeMoveRule rule = decodeNativeMoveRule(
            kTrainingNativeMoveBehaviorPacked[index]);
        if (rule.resource != resource || rule.sourceAction != sourceAction48 ||
            !triggerMatches(rule.trigger, contact)) {
            continue;
        }
        if (rule.triggerFrame != kNativeNoByte &&
            sourceLocalFrame != rule.triggerFrame) {
            continue;
        }

        bool transitionMatches = false;
        switch (rule.effect) {
            case TrainingNativeMoveEffect::RedirectAnimationFrame:
                transitionMatches = observedAction48 == sourceAction48 &&
                    rule.targetFrame != kNativeNoByte &&
                    observedLocalFrame == rule.targetFrame;
                break;
            case TrainingNativeMoveEffect::ChangeAction:
            case TrainingNativeMoveEffect::ReplaceActionOnEntry:
                transitionMatches = rule.targetAction != kNativeNoByte &&
                    observedAction48 == rule.targetAction;
                break;
            case TrainingNativeMoveEffect::EndAction:
                transitionMatches = observedAction48 != sourceAction48;
                break;
            case TrainingNativeMoveEffect::UpdateCharacterState:
                transitionMatches = observedAction48 == sourceAction48;
                break;
            case TrainingNativeMoveEffect::None:
                break;
        }
        if (!transitionMatches) continue;

        ++matchCount;
        if (matchCount > 1u) return {};

        matched.trigger = rule.trigger;
        matched.effect = rule.effect;
        matched.flags = rule.flags;
        matched.nativeContactMask = rule.contactMask;
        if (rule.effect == TrainingNativeMoveEffect::RedirectAnimationFrame) {
            matched.targetAction = sourceAction48;
        } else if (rule.targetAction != kNativeNoByte) {
            matched.targetAction = rule.targetAction;
        }
        if (rule.targetFrame != kNativeNoByte)
            matched.targetLocalFrame = rule.targetFrame;

        if (rule.effect == TrainingNativeMoveEffect::UpdateCharacterState) {
            matched.proof = TrainingNativeTransitionProof::ProvenEffectOnly;
            continue;
        }

        const std::uint32_t policyTarget =
            matched.targetAction == kTrainingNativeMoveUnknownValue
                ? observedAction48
                : matched.targetAction;
        const bool excluded =
            (rule.flags & TrainingNativeMoveFlagCaptureSequence) != 0 ||
            !trainingActionAllowsConventionalFrameAdvantage(
                characterId, sourceAction48) ||
            !trainingActionAllowsConventionalFrameAdvantage(
                characterId, policyTarget);
        if (excluded) {
            matched.flags |=
                TrainingNativeMoveFlagConventionalFrameAdvantageExcluded;
            matched.proof =
                TrainingNativeTransitionProof::ProvenExcludedContinuation;
        } else {
            matched.proof =
                TrainingNativeTransitionProof::ProvenAutomaticContinuation;
        }
    }

    return matched;
}

TrainingDispatchFamily trainingDispatchFamily(std::uint32_t action84) noexcept {
    switch (action84) {
        case 0:
            return TrainingDispatchFamily::Neutral;

        case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        case 9: case 40: case 41: case 72: case 73: case 151: case 154:
            return TrainingDispatchFamily::GenericAction;

        case 10: case 11: case 12:
            return TrainingDispatchFamily::RunningNormal;
        case 13: case 14:
            return TrainingDispatchFamily::GroundDash;
        case 15: case 16: case 17:
            return TrainingDispatchFamily::Jump;
        case 18: case 19: case 20:
            return TrainingDispatchFamily::Guard;
        case 21: case 22: case 23:
            return TrainingDispatchFamily::GroundHitReaction;
        case 24:
            return TrainingDispatchFamily::AirGuardCrush;
        case 25: case 38: case 56: case 57: case 58:
            return TrainingDispatchFamily::AirborneReaction;
        case 26: case 27:
            return TrainingDispatchFamily::Walk;
        case 28:
            return TrainingDispatchFamily::Crouch;
        case 29: case 30:
            return TrainingDispatchFamily::Scripted29Or30;
        case 31: case 33: case 34:
            return TrainingDispatchFamily::RecoveryResult;
        case 35:
            return TrainingDispatchFamily::HardKnockdown;
        case 36:
            return TrainingDispatchFamily::Scripted36;
        case 37:
            return TrainingDispatchFamily::WallState;
        case 70:
            return TrainingDispatchFamily::Thrower;
        case 91:
            return TrainingDispatchFamily::RecursiveSystem;
        case 92: case 215:
            return TrainingDispatchFamily::Scripted92Or215;
        case 96: case 97: case 98:
            return TrainingDispatchFamily::Scripted96To98;
        case 150: case 153: case 156:
            return TrainingDispatchFamily::Special150Family;
        case 152: case 155: case 158: case 167: case 168: case 169:
            return TrainingDispatchFamily::Special152Family;
        case 157:
            return TrainingDispatchFamily::Special157;
        case 160:
            return TrainingDispatchFamily::Special160;
        case 161: case 162: case 163:
            return TrainingDispatchFamily::Special161Family;
        case 165:
            return TrainingDispatchFamily::Special165;
        case 166:
            return TrainingDispatchFamily::Special166;

        case 200: case 202: case 204: case 206: case 210: case 212:
        case 214: case 216: case 218: case 220: case 222: case 224:
        case 226: case 228: case 230: case 231: case 232: case 234:
        case 235: case 236: case 237: case 238: case 239: case 240:
        case 241: case 242: case 243: case 244: case 246: case 250:
            return TrainingDispatchFamily::ThrowReception;
        case 208:
            return TrainingDispatchFamily::ThrowReception208;
        default:
            return TrainingDispatchFamily::Unmapped;
    }
}

}
