#include "netplay/tests/TestHarness.h"

#include "input/InputDisplayPublisher.h"
#include "training/TrainingCommandQueue.h"
#include "training/TrainingDummy.h"
#include "training/TrainingGuardTiming.h"
#include "training/TrainingHold.h"
#include "training/TrainingInput.h"
#include "training/TrainingMacros.h"
#include "training/TrainingPositions.h"
#include "training/TrainingRecovery.h"
#include "training/TrainingResources.h"
#include "training/TrainingStatusPublisher.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

using namespace qtraining;

namespace {

TrainingCommandV1 command(uint32_t generation, uint32_t id,
                          TrainingCommandKind kind) {
    TrainingCommandV1 result{};
    result.trainingGeneration = generation;
    result.commandId = id;
    result.commandKind = static_cast<uint32_t>(kind);
    return result;
}

struct LegacyTrainingStatusV1 {
    uint32_t apiVersion;
    uint32_t structSize;
    uint32_t explicitTrainingPresent;
    uint32_t available;
    uint32_t disableReason;
    uint32_t lifecycleState;
    uint32_t inMatchReady;
    uint32_t capabilityFlags;
    uint32_t trainingGeneration;
    uint32_t realStepSerialLow;
    uint32_t realStepSerialHigh;

    uint32_t scene;
    uint32_t matchSubstate;
    uint32_t p1Character;
    uint32_t p2Character;
    uint32_t p1Facing;
    uint32_t p2Facing;
    uint32_t p1FinalLogicalMask;
    uint32_t p2FinalLogicalMask;

    uint32_t comMask;
    uint32_t p1ComPrimary;
    uint32_t p1ComMirror;
    uint32_t p2ComPrimary;
    uint32_t p2ComMirror;

    uint32_t guardMode;
    uint32_t guardPermission;
    uint32_t guardClassification;
    uint32_t guardPredictionMisses;
    uint32_t guardDuringPlayback;

    uint32_t endlessRoundRequested;
    uint32_t endlessRoundEffective;
    uint32_t infiniteTimeRequested;
    uint32_t infiniteTimeEffective;
    uint32_t extraFunctionsDisabled;
    uint32_t waitCountZero;
    uint32_t pauseNeutral;
    uint32_t freezeNeutral;

    uint32_t lastAdmissionResult;
    uint32_t lastNativeReturn;
    uint32_t inputRearmState;
    uint32_t macroState;
    uint32_t macroCursor;
    uint32_t selectedMacroSlot;
    uint32_t playbackMode;
    uint32_t facingPolicy;
    uint32_t validMacroSlots;
    uint32_t macroFrameCounts[kTrainingMacroSlotCount];
    uint32_t macroFrameCapacity;
    uint32_t selectedStateSlot;
    uint32_t validStateSlots;
    uint32_t staleStateSlots;
    uint32_t rollbackLayoutGeneration;
    uint32_t characterTeamFingerprintLow;
    uint32_t characterTeamFingerprintHigh;

    uint32_t hitboxRequested;
    uint32_t hitboxEffective;
    uint32_t menuCaptured;
    uint32_t lastAcknowledgedCommandId;
    uint32_t lastCommandResult;
    uint32_t commandQueueDepth;
    uint32_t commandQueueCapacity;
    uint32_t commandQueueDrops;
    uint32_t stickyValidationErrors;

    char disableText[96];
    char statusText[96];
    char resultText[96];
};

struct CombatTierTrainingStatusV1 {
    LegacyTrainingStatusV1 frozen;

    uint32_t combatReadoutValid;
    uint32_t dummyStance;
    uint32_t dummyTech;
    uint32_t p1EffectiveLife;
    uint32_t p2EffectiveLife;
    uint32_t lifeMax;
    uint32_t p1GuardGauge;
    uint32_t p2GuardGauge;
    uint32_t p1MeterStocks;
    uint32_t p2MeterStocks;
    uint32_t p1MeterFraction;
    uint32_t p2MeterFraction;
    uint32_t damageEventSerial;
    uint32_t damageDeclinedCount;
    uint32_t lastHitDamage;
    uint32_t lastHitPendingDelta;
    uint32_t lastHitAttackerSlot;
    uint32_t lastHitVictimSlot;
    uint32_t lastHitCause;
    uint32_t lastHitAdmitted;
    uint32_t p1ComboHitsTaken;
    uint32_t p2ComboHitsTaken;
    uint32_t p1ComboDamageTaken;
    uint32_t p2ComboDamageTaken;
    uint32_t p1LastComboHitsTaken;
    uint32_t p2LastComboHitsTaken;
    uint32_t p1LastComboDamageTaken;
    uint32_t p2LastComboDamageTaken;
    uint32_t p1MaxComboHitsDealt;
    uint32_t p2MaxComboHitsDealt;
    uint32_t p1MaxComboDamageDealt;
    uint32_t p2MaxComboDamageDealt;
};

struct FighterTierTrainingStatusV1 {
    CombatTierTrainingStatusV1 combat;

    uint32_t p1LifeCurrent;
    uint32_t p1LifePendingDamage;
    uint32_t p1LifeReserve;
    uint32_t p1LifeSettleTimer;
    uint32_t p2LifeCurrent;
    uint32_t p2LifePendingDamage;
    uint32_t p2LifeReserve;
    uint32_t p2LifeSettleTimer;

    uint32_t fighterReadoutValid;

    uint32_t p1ActionState;
    uint32_t p1ActionFrame;
    uint32_t p1ActionLockEnd;
    uint32_t p1AnimFrame;
    uint32_t p1NextAction;
    uint32_t p1TransitionFlag;
    uint32_t p1PrevAction;
    uint32_t p1MoveGateByte;
    uint32_t p1PositionX;
    uint32_t p1PositionY;
    uint32_t p2ActionState;
    uint32_t p2ActionFrame;
    uint32_t p2ActionLockEnd;
    uint32_t p2AnimFrame;
    uint32_t p2NextAction;
    uint32_t p2TransitionFlag;
    uint32_t p2PrevAction;
    uint32_t p2MoveGateByte;
    uint32_t p2PositionX;
    uint32_t p2PositionY;
};

RecoveryObservation recoveryAvailable() {
    RecoveryObservation observation{};
    observation.readable = true;
    observation.lifecycleAvailable = true;
    observation.sandboxActive = true;
    observation.slot = 1;
    observation.commandChild = 0x00401000u;
    observation.facing = 0;
    return observation;
}

RecoveryObservation recoveryEligibleFor(RecoveryBranch branch) {
    RecoveryObservation observation = recoveryAvailable();
    observation.postureNibble = 1;
    switch (branch) {
        case RecoveryBranch::Air22:
            observation.action = 25;
            observation.permission = kRecoveryPermitAir;
            break;
        case RecoveryBranch::Ground88:
            observation.action = 25;
            observation.permission = kRecoveryPermitGround;
            break;
        case RecoveryBranch::Wall66:
            observation.action = 37;
            observation.permission = kRecoveryPermitGround;
            observation.wallFlags = 2;
            break;
        case RecoveryBranch::TerminalForward66:
            observation.action = 25;
            observation.terminalReady = true;
            observation.directionRingHead = kRecoveryRingForward;
            break;
        case RecoveryBranch::TerminalBack44:
            observation.action = 25;
            observation.terminalReady = true;
            observation.directionRingHead = kRecoveryRingBack;
            break;
        case RecoveryBranch::ThrowEscape:
            observation.action = 25;
            observation.terminalReady = true;
            observation.buttonRingHead = kRecoveryButtonRingThrow;
            break;
        default:
            break;
    }
    return observation;
}

RecoverySettings recoveryArmed(RecoveryBranch branch,
                               ManualRecoveryMode mode
                                   = ManualRecoveryMode::Earliest) {
    RecoverySettings settings{};
    settings.evaluatorQualified = true;
    switch (branch) {
        case RecoveryBranch::Air22: settings.air = mode; break;
        case RecoveryBranch::Ground88: settings.ground = mode; break;
        case RecoveryBranch::Wall66: settings.wall = mode; break;
        case RecoveryBranch::TerminalForward66:
            settings.terminal = mode;
            settings.terminalChoice = TerminalRecoveryChoice::Forward;
            break;
        case RecoveryBranch::TerminalBack44:
            settings.terminal = mode;
            settings.terminalChoice = TerminalRecoveryChoice::Back;
            break;
        case RecoveryBranch::ThrowEscape:
            settings.throwEscape = mode;
            break;
        default:
            break;
    }
    return settings;
}

const char* recoveryBranchName(RecoveryBranch branch) {
    switch (branch) {
        case RecoveryBranch::Air22: return "Air22";
        case RecoveryBranch::Ground88: return "Ground88";
        case RecoveryBranch::Wall66: return "Wall66";
        case RecoveryBranch::TerminalForward66: return "Terminal66";
        case RecoveryBranch::TerminalBack44: return "Terminal44";
        case RecoveryBranch::ThrowEscape: return "ThrowEscape";
        default: return "?";
    }
}

void recoveryEmitsNothing(const char* label, RecoveryBranch branch,
                          const RecoveryObservation& observation) {
    TrainingRecoveryController controller;
    controller.reset(observation.trainingGeneration);
    uint64_t serial = 0;
    for (uint32_t step = 0; step < 8u; ++step) {
        const RecoveryEmission emission =
            controller.advance(++serial, recoveryArmed(branch), observation);
        if (emission.active || emission.directions != 0
            || emission.buttons != 0 || emission.ownsButtons) {
            std::printf("    [recovery] %s/%s emitted %08X/%08X on step %u\n",
                        recoveryBranchName(branch), label,
                        emission.directions, emission.buttons, step);
        }
        QNP_CHECK(!emission.active);
        QNP_CHECK(emission.directions == 0u);
        QNP_CHECK(emission.buttons == 0u);
        QNP_CHECK(!emission.ownsButtons);
    }
    QNP_CHECK(controller.counters().attempts == 0u);
}

FighterNeutralObservation neutralFighter(uint8_t slot) {
    FighterNeutralObservation observation{};
    observation.readable = true;
    observation.slot = slot;
    observation.fatalCause = -1;
    observation.postureNibble = 1;
    return observation;
}

NeutralGlobalObservation neutralGlobals() {
    NeutralGlobalObservation observation{};
    observation.readable = true;
    return observation;
}

uint32_t logicalFromDisplayNibble(uint32_t nibble) {
    uint32_t mask = 0;
    if (nibble & qinput::kInputDisplayNibbleUp) mask |= qinput::kLogicalUp;
    if (nibble & qinput::kInputDisplayNibbleDown) mask |= qinput::kLogicalDown;
    if (nibble & qinput::kInputDisplayNibbleLeft) mask |= qinput::kLogicalLeft;
    if (nibble & qinput::kInputDisplayNibbleRight)
        mask |= qinput::kLogicalRight;
    return mask;
}

struct DisplayNibbleCase {
    uint32_t nibble;
    qinput::InputDisplayDirection expected;
};

const DisplayNibbleCase kDisplayNibbleTable[16] = {
    {0x0u, qinput::InputDisplayDirection::Neutral},
    {0x1u, qinput::InputDisplayDirection::Up},
    {0x2u, qinput::InputDisplayDirection::Down},
    {0x3u, qinput::InputDisplayDirection::Neutral},
    {0x4u, qinput::InputDisplayDirection::Left},
    {0x5u, qinput::InputDisplayDirection::UpLeft},
    {0x6u, qinput::InputDisplayDirection::DownLeft},
    {0x7u, qinput::InputDisplayDirection::Left},
    {0x8u, qinput::InputDisplayDirection::Right},
    {0x9u, qinput::InputDisplayDirection::UpRight},
    {0xAu, qinput::InputDisplayDirection::DownRight},
    {0xBu, qinput::InputDisplayDirection::Right},
    {0xCu, qinput::InputDisplayDirection::Neutral},
    {0xDu, qinput::InputDisplayDirection::Up},
    {0xEu, qinput::InputDisplayDirection::Down},
    {0xFu, qinput::InputDisplayDirection::Neutral},
};

uint32_t distinctLogicalMask(uint32_t index) {
    return (index & 0xFu) | (((index >> 4) & 0xFu) << 16);
}

qinput::InputDisplayStep trainingDisplayStep(uint32_t generation,
                                             uint64_t serial,
                                             uint32_t lane,
                                             uint32_t mask) {
    qinput::InputDisplayStep step{};
    step.mode = qinput::InputDisplayMode::Training;
    step.generation = generation;
    step.stepSerial = serial;
    step.drivenLane = lane;
    step.laneActiveMask = 1u << lane;
    step.laneLogicalMask[lane] = mask;
    return step;
}

qinput::InputDisplayStep replayDisplayStep(uint32_t generation,
                                           uint64_t serial,
                                           uint32_t laneActiveMask,
                                           uint32_t p1Mask,
                                           uint32_t p2Mask) {
    qinput::InputDisplayStep step{};
    step.mode = qinput::InputDisplayMode::ReplayPlayback;
    step.generation = generation;
    step.stepSerial = serial;
    step.drivenLane = qinput::kInputDisplayNoLane;
    step.laneActiveMask = laneActiveMask;
    step.laneLogicalMask[0] = p1Mask;
    step.laneLogicalMask[1] = p2Mask;
    return step;
}

TrainingActionResult pollInput(TrainingInput& input, uint64_t& pollSerial,
                               uint32_t heldActions = 0,
                               uint32_t p1Mask = 0,
                               bool fightNeutral = true) {
    TrainingActionPoll poll{};
    poll.lifecycleAvailable = true;
    poll.relevantPhysicalNeutralBeforeSocd = fightNeutral;
    poll.heldTrainingActions = heldActions;
    poll.p1PhysicalMask = p1Mask;
    return input.pollActions(++pollSerial, poll);
}

TrainingActionResult armInput(TrainingInput& input, uint64_t& serial) {
    pollInput(input, serial);
    return pollInput(input, serial);
}

DummyPostureResult posture(GuardMode mode, DummyStance stance,
                           DummyTech tech, uint8_t facing,
                           PostureClass defenderPosture
                               = PostureClass::NotGuardable) {
    GuardDecisionInput guardInput{};
    guardInput.mode = mode;
    guardInput.permission = kGuardPermitStand | kGuardPermitCrouch;
    guardInput.defenderFacing = facing;
    guardInput.defenderPosture = defenderPosture;
    guardInput.autoPredictionQualified = false;
    const GuardDecision guard = chooseGuardDirections(guardInput);

    DummyPostureInput input{};
    input.guardMode = mode;
    input.stance = stance;
    input.tech = tech;
    input.guarding = guard.guarding;
    input.guardDirections = guard.logicalDirections;
    input.defenderFacing = facing;
    return chooseDummyPosture(input);
}

struct LadderCase {
    const char* name = "";
    bool lifecycleAvailable = true;
    bool menuCaptured = false;
    bool operationNeutral = false;
    bool armed = true;
    bool macroOverrideP1 = false;
    bool macroOverrideP2 = false;
    bool macroPlaybackActive = false;
    bool dummyOwnsDirection = true;
    bool guardDuringPlayback = false;
    uint32_t macroP1Mask = 0;
    uint32_t macroP2Mask = 0;
    uint32_t p1PhysicalMask = 0;
    uint32_t p2PhysicalMask = 0;
    GuardMode guardMode = GuardMode::Off;
    DummyStance stance = DummyStance::Stand;
    DummyTech tech = DummyTech::Off;
    uint8_t facing = 0;
    qinput::SocdPolicy horizontalSocd = qinput::SocdPolicy::Native;
    qinput::SocdPolicy verticalSocd = qinput::SocdPolicy::Native;
    uint32_t expectedP1 = 0;
    uint32_t expectedP2 = 0;
};

LadderCase ladderBaseline(const char* name) {
    LadderCase entry{};
    entry.name = name;
    return entry;
}

}

QNP_TEST(TrainingAbiIsFixedPod) {
    QNP_CHECK(kTrainingApiVersion == 1);
    QNP_CHECK(sizeof(TrainingCommandV1) == 32);
    QNP_CHECK(sizeof(TrainingStatusV1) == 1888);
    QNP_CHECK(kTrainingStatusV1BaseSize == 568);
    QNP_CHECK(kTrainingStatusV1CombatTier == 696);
    QNP_CHECK(kTrainingStatusV1FighterTier == 812);
    QNP_CHECK(kTrainingStatusV1RecoveryTier == 1072);
    QNP_CHECK(kTrainingStatusV1WallTier == 1080);
    QNP_CHECK(kTrainingStatusV1RecoveryDiagTier == 1096);
    QNP_CHECK(kTrainingStatusV1MeterChargeTier == 1104);
    QNP_CHECK(kTrainingStatusV1CombatToolsTier == 1300);
    QNP_CHECK(kTrainingStatusV1ActionTriggersTier == 1340);
    QNP_CHECK(kTrainingStatusV1DebugOverlayTier == 1404);
    QNP_CHECK(kTrainingStatusV1StateLogTier == 1420);
    QNP_CHECK(kTrainingStatusV1ActorPoolTier == 1624);
    QNP_CHECK(kTrainingStatusV1NativeHudTier == 1652);
    QNP_CHECK(kTrainingStatusV1NativeHudRequestedTier == 1664);
    QNP_CHECK(kTrainingStatusV1FrameTraceTier == 1680);
    QNP_CHECK(kTrainingStatusV1FrameAdvantageGapTier == 1688);
    QNP_CHECK(kTrainingStatusV1ContactPolicyTier == 1808);
    QNP_CHECK(kTrainingStatusV1ExRestoreTargetTier == 1820);
    QNP_CHECK(kTrainingStatusV1MacroAfterResetTier == 1824);
    QNP_CHECK(kTrainingStatusV1ReducePolicyTier == 1852);
    QNP_CHECK(kTrainingStatusV1GuardTimingTier == 1864u);
    QNP_CHECK(kTrainingStatusV1PlayerSwapTier == 1868u);
    QNP_CHECK(sizeof(TrainingStatusV1)
              == kTrainingStatusV1StatusMessageTier);
    QNP_CHECK(kTrainingStatusV1WallTier < kTrainingStatusV1RecoveryDiagTier);
    QNP_CHECK(kTrainingStatusV1TierCount == 23u);
    QNP_CHECK(kTrainingStatusV1SizeTiers[0] == kTrainingStatusV1BaseSize);
    QNP_CHECK(kTrainingStatusV1SizeTiers[1] == kTrainingStatusV1CombatTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[2] == kTrainingStatusV1FighterTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[3] == kTrainingStatusV1RecoveryTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[4] == kTrainingStatusV1WallTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[5]
              == kTrainingStatusV1RecoveryDiagTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[6]
              == kTrainingStatusV1MeterChargeTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[7]
              == kTrainingStatusV1CombatToolsTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[8]
              == kTrainingStatusV1ActionTriggersTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[9]
              == kTrainingStatusV1DebugOverlayTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[10]
              == kTrainingStatusV1StateLogTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[11]
              == kTrainingStatusV1ActorPoolTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[12]
              == kTrainingStatusV1NativeHudTier);
    QNP_CHECK(kTrainingStatusV1SizeTiers[13]
              == kTrainingStatusV1NativeHudRequestedTier);
    QNP_CHECK(trainingStatusTiersAscend());
    QNP_CHECK(offsetof(TrainingStatusV1, p1LifeCurrent)
              == kTrainingStatusV1CombatTier);
    QNP_CHECK(offsetof(TrainingStatusV1, fighterReadoutValid) == 728);
    QNP_CHECK(offsetof(TrainingStatusV1, p1ActionState) == 732);
    QNP_CHECK(offsetof(TrainingStatusV1, p2PositionY) == 808);
    QNP_CHECK(sizeof(TrainingStatusV1) > kTrainingStatusV1BaseSize);
    QNP_CHECK(sizeof(LegacyTrainingStatusV1)
              == kTrainingStatusV1BaseSize);
    QNP_CHECK(std::is_standard_layout<TrainingCommandV1>::value);
    QNP_CHECK(std::is_trivially_copyable<TrainingCommandV1>::value);
    QNP_CHECK(std::is_standard_layout<TrainingStatusV1>::value);
    QNP_CHECK(std::is_trivially_copyable<TrainingStatusV1>::value);
    QNP_CHECK(sizeof(TrainingStatusV1) % sizeof(uint32_t) == 0);

    QNP_CHECK(offsetof(TrainingStatusV1, explicitTrainingPresent) == 8);
    QNP_CHECK(offsetof(TrainingStatusV1, p1FinalLogicalMask) == 68);
    QNP_CHECK(offsetof(TrainingStatusV1, macroFrameCounts) == 184);
    QNP_CHECK(offsetof(TrainingStatusV1, disableText) == 280);
    QNP_CHECK(offsetof(TrainingStatusV1, resultText) == 472);

    QNP_CHECK(offsetof(TrainingStatusV1, combatReadoutValid)
              == kTrainingStatusV1BaseSize);
    QNP_CHECK(offsetof(TrainingStatusV1, dummyStance) == 572);
    QNP_CHECK(offsetof(TrainingStatusV1, dummyTech) == 576);
    QNP_CHECK(offsetof(TrainingStatusV1, p1EffectiveLife) == 580);
    QNP_CHECK(offsetof(TrainingStatusV1, p2EffectiveLife) == 584);
    QNP_CHECK(offsetof(TrainingStatusV1, lifeMax) == 588);
    QNP_CHECK(offsetof(TrainingStatusV1, p1GuardGauge) == 592);
    QNP_CHECK(offsetof(TrainingStatusV1, p2GuardGauge) == 596);
    QNP_CHECK(offsetof(TrainingStatusV1, p1MeterStocks) == 600);
    QNP_CHECK(offsetof(TrainingStatusV1, p2MeterStocks) == 604);
    QNP_CHECK(offsetof(TrainingStatusV1, p1MeterFraction) == 608);
    QNP_CHECK(offsetof(TrainingStatusV1, p2MeterFraction) == 612);
    QNP_CHECK(offsetof(TrainingStatusV1, damageEventSerial) == 616);
    QNP_CHECK(offsetof(TrainingStatusV1, damageDeclinedCount) == 620);
    QNP_CHECK(offsetof(TrainingStatusV1, lastHitDamage) == 624);
    QNP_CHECK(offsetof(TrainingStatusV1, lastHitPendingDelta) == 628);
    QNP_CHECK(offsetof(TrainingStatusV1, lastHitAttackerSlot) == 632);
    QNP_CHECK(offsetof(TrainingStatusV1, lastHitVictimSlot) == 636);
    QNP_CHECK(offsetof(TrainingStatusV1, lastHitCause) == 640);
    QNP_CHECK(offsetof(TrainingStatusV1, lastHitAdmitted) == 644);
    QNP_CHECK(offsetof(TrainingStatusV1, p1ComboHitsTaken) == 648);
    QNP_CHECK(offsetof(TrainingStatusV1, p2ComboHitsTaken) == 652);
    QNP_CHECK(offsetof(TrainingStatusV1, p1ComboDamageTaken) == 656);
    QNP_CHECK(offsetof(TrainingStatusV1, p2ComboDamageTaken) == 660);
    QNP_CHECK(offsetof(TrainingStatusV1, p1LastComboHitsTaken) == 664);
    QNP_CHECK(offsetof(TrainingStatusV1, p2LastComboHitsTaken) == 668);
    QNP_CHECK(offsetof(TrainingStatusV1, p1LastComboDamageTaken) == 672);
    QNP_CHECK(offsetof(TrainingStatusV1, p2LastComboDamageTaken) == 676);
    QNP_CHECK(offsetof(TrainingStatusV1, p1MaxComboHitsDealt) == 680);
    QNP_CHECK(offsetof(TrainingStatusV1, p2MaxComboHitsDealt) == 684);
    QNP_CHECK(offsetof(TrainingStatusV1, p1MaxComboDamageDealt) == 688);
    QNP_CHECK(offsetof(TrainingStatusV1, p2MaxComboDamageDealt) == 692);

    QNP_CHECK(offsetof(TrainingStatusV1, holdReasons)
              == kTrainingStatusV1FighterTier);
    QNP_CHECK(offsetof(TrainingStatusV1, guardHeuristicPostureDivergences)
              == 816);
    QNP_CHECK(offsetof(TrainingStatusV1, guardHeuristicAmbiguousAttackers)
              == 820);
    QNP_CHECK(offsetof(TrainingStatusV1, guardHeuristicUnguardablePostures)
              == 824);
    QNP_CHECK(offsetof(TrainingStatusV1, p1PostureClass) == 828);
    QNP_CHECK(offsetof(TrainingStatusV1, p2PostureClass) == 832);
    QNP_CHECK(offsetof(TrainingStatusV1, resourcePolicyValid) == 836);
    QNP_CHECK(offsetof(TrainingStatusV1, healthTrigger) == 840);
    QNP_CHECK(offsetof(TrainingStatusV1, guardGaugeTriggerTarget) == 860);
    QNP_CHECK(offsetof(TrainingStatusV1, bothFightersNeutral) == 864);
    QNP_CHECK(offsetof(TrainingStatusV1, neutralHoldSteps) == 868);
    QNP_CHECK(offsetof(TrainingStatusV1, p1NeutralReasonMask) == 872);
    QNP_CHECK(offsetof(TrainingStatusV1, neutralGlobalReasonMask) == 880);
    QNP_CHECK(offsetof(TrainingStatusV1, p1ComboEpisodeState) == 884);
    QNP_CHECK(offsetof(TrainingStatusV1, p1RecoveryLock) == 892);
    QNP_CHECK(offsetof(TrainingStatusV1, p1AuthoredFrameFlags) == 916);
    QNP_CHECK(offsetof(TrainingStatusV1, p2InitStatus) == 944);
    QNP_CHECK(offsetof(TrainingStatusV1, lifeSetMax) == 948);
    QNP_CHECK(offsetof(TrainingStatusV1, exStocksMax) == 952);
    QNP_CHECK(offsetof(TrainingStatusV1, guardGaugeMax) == 956);
    QNP_CHECK(offsetof(TrainingStatusV1, roundStartNeutralMask) == 968);
    QNP_CHECK(offsetof(TrainingStatusV1, roundStartNeutralValid) == 972);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryReadoutValid) == 976);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryModeAir) == 980);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryModeThrowEscape) == 996);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryTerminalChoice) == 1000);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryState) == 1016);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryEligibleMask) == 1024);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryLastVerdict) == 1028);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryLastResultAction) == 1060);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryStepsRemaining) == 1064);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryContactTestUnknown) == 1068);
    QNP_CHECK((kTrainingStatusV1RecoveryTier - kTrainingStatusV1FighterTier)
                  / sizeof(uint32_t) == 65u);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryContactTestUnknown)
                  + sizeof(uint32_t) == kTrainingStatusV1RecoveryTier);

    QNP_CHECK(offsetof(TrainingStatusV1, p1WallFlags)
              == kTrainingStatusV1RecoveryTier);
    QNP_CHECK(offsetof(TrainingStatusV1, p2WallFlags) == 1076);
    QNP_CHECK(offsetof(TrainingStatusV1, p2WallFlags) + sizeof(uint32_t)
              == kTrainingStatusV1WallTier);

    QNP_CHECK(offsetof(TrainingStatusV1, recoveryEvaluatorReason)
              == kTrainingStatusV1WallTier);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryStandDownMask) == 1084);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryArmableMask) == 1088);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryTapsEmitted) == 1092);
    QNP_CHECK(offsetof(TrainingStatusV1, recoveryTapsEmitted)
                  + sizeof(uint32_t) == kTrainingStatusV1RecoveryDiagTier);
    QNP_CHECK((kTrainingStatusV1RecoveryDiagTier - kTrainingStatusV1WallTier)
                  / sizeof(uint32_t) == 4u);

    QNP_CHECK(static_cast<uint32_t>(TrainingTriggerEvent::Count) == 8u);
    QNP_CHECK(kTrainingTriggerEventCount == 8u);
    QNP_CHECK(offsetof(TrainingStatusV1, combatToolsReadoutValid)
              == kTrainingStatusV1MeterChargeTier);
    QNP_CHECK(offsetof(TrainingStatusV1, throwTechChancePercent) == 1108u);
    QNP_CHECK(offsetof(TrainingStatusV1, throwTechOpportunities) == 1112u);
    QNP_CHECK(offsetof(TrainingStatusV1, throwTechNativeAccepted) == 1116u);
    QNP_CHECK(offsetof(TrainingStatusV1, throwTechAutomaticAccepted) == 1120u);
    QNP_CHECK(offsetof(TrainingStatusV1, throwTechResultObserved) == 1124u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerMacroSlot) == 1128u);
    QNP_CHECK(sizeof(TrainingStatusV1{}.triggerMacroSlot) == 8u * 4u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerDelaySteps) == 1160u);
    QNP_CHECK(sizeof(TrainingStatusV1{}.triggerDelaySteps) == 8u * 4u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerChancePercent) == 1192u);
    QNP_CHECK(sizeof(TrainingStatusV1{}.triggerChancePercent) == 8u * 4u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerPendingCount) == 1224u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerStartedCount) == 1228u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerChanceSkippedCount) == 1232u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerDroppedCount) == 1236u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerLastEvent) == 1240u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerLastMacroSlot) == 1244u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerLastOutcome) == 1248u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageEnabled) == 1252u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageState) == 1256u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageContactKind) == 1260u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageAttackerOwner) == 1264u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageDefenderOwner) == 1268u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageValid) == 1272u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageValue) == 1276u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageAttackerReadyDelay)
              == 1280u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageDefenderReadyDelay)
              == 1284u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageCompletedCount) == 1288u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageTradeCount) == 1292u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageTimeoutCount) == 1296u);
    QNP_CHECK(offsetof(TrainingStatusV1, frameAdvantageTimeoutCount)
                  + sizeof(uint32_t)
              == kTrainingStatusV1CombatToolsTier);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerMoveCatalogId)
              == kTrainingStatusV1CombatToolsTier);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerMoveIndex) == 1304u);
    QNP_CHECK(sizeof(TrainingStatusV1{}.triggerMoveIndex) == 8u * 4u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerLastMoveIndex) == 1336u);
    QNP_CHECK(offsetof(TrainingStatusV1, triggerLastMoveIndex)
                  + sizeof(uint32_t)
              == kTrainingStatusV1ActionTriggersTier);

    QNP_CHECK(kTrainingActionCount == 16);
    QNP_CHECK(trainingActionBit(TrainingAction::PreviousSlot) == (1u << 6));
    QNP_CHECK(trainingActionBit(TrainingAction::TogglePause) == (1u << 7));
    QNP_CHECK(trainingActionBit(TrainingAction::FrameStep) == (1u << 8));
    QNP_CHECK(trainingActionBit(TrainingAction::ReturnToCharacterSelect)
              == (1u << 9));
    QNP_CHECK(trainingActionBit(TrainingAction::RefillAll) == (1u << 10));
    QNP_CHECK(trainingActionBit(TrainingAction::ToggleHitboxes) == (1u << 11));
    QNP_CHECK(trainingActionBit(TrainingAction::ResetPositionRoundStart)
              == (1u << 12));
    QNP_CHECK(trainingActionBit(TrainingAction::CycleDummyStance)
              == (1u << 13));
    QNP_CHECK(trainingActionBit(TrainingAction::CycleGuardMode) == (1u << 14));
    QNP_CHECK(trainingActionBit(TrainingAction::ChargeExFraction)
              == (1u << 15));
    QNP_CHECK(kTrainingActionMask == 0xFFFFu);
    QNP_CHECK(trainingImageMutatingActionMask()
              == (trainingActionBit(TrainingAction::ResetPosition)
                  | trainingActionBit(TrainingAction::ResetPositionRoundStart)
                  | trainingActionBit(TrainingAction::LoadState)));
    QNP_CHECK((trainingImageMutatingActionMask()
               & (trainingActionBit(TrainingAction::RefillAll)
                  | trainingActionBit(TrainingAction::ToggleHitboxes)
                  | trainingActionBit(TrainingAction::CycleDummyStance)
                  | trainingActionBit(TrainingAction::CycleGuardMode))) == 0u);
    QNP_CHECK((trainingImageMutatingActionMask()
               & trainingActionBit(
                   TrainingAction::ReturnToCharacterSelect)) == 0u);
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::ReturnToCharacterSelect));

    QNP_CHECK(TrainingCapabilityRefill == (1u << 8));
    QNP_CHECK(TrainingCapabilityDummyGuard == (1u << 3));
    QNP_CHECK((TrainingCapabilityRefill
               & (TrainingCapabilityReturnToSelect
                  | TrainingCapabilityHitboxes)) == 0u);
}

QNP_TEST(TrainingStatusGrowthKeepsStaleWrapperReadingEveryFrozenField) {
    TrainingStatusV1 modern{};
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&modern);
    for (uint32_t offset = 0; offset < kTrainingStatusV1BaseSize; ++offset)
        bytes[offset] = static_cast<unsigned char>(1u + (offset * 37u) % 251u);
    std::memset(bytes + kTrainingStatusV1BaseSize, 0xEE,
                sizeof(modern) - kTrainingStatusV1BaseSize);
    modern.apiVersion = kTrainingApiVersion;
    modern.structSize = kTrainingStatusV1BaseSize;

    LegacyTrainingStatusV1 legacy;
    std::memset(&legacy, 0xCD, sizeof(legacy));
    const uint32_t outSize = sizeof(LegacyTrainingStatusV1);
    QNP_REQUIRE(outSize >= kTrainingStatusV1BaseSize);
    const uint32_t copied = trainingStatusTierFor(outSize);
    QNP_CHECK(copied == kTrainingStatusV1BaseSize);
    std::memcpy(&legacy, &modern, copied);

    QNP_CHECK(legacy.apiVersion == kTrainingApiVersion);
    QNP_CHECK(legacy.structSize == kTrainingStatusV1BaseSize);
    QNP_CHECK(legacy.structSize >= sizeof(LegacyTrainingStatusV1));

#define QNP_FROZEN_FIELD(field)                                        \
    QNP_CHECK(offsetof(TrainingStatusV1, field)                        \
              == offsetof(LegacyTrainingStatusV1, field));             \
    QNP_CHECK(modern.field == legacy.field)
#define QNP_FROZEN_ARRAY(field)                                        \
    QNP_CHECK(offsetof(TrainingStatusV1, field)                        \
              == offsetof(LegacyTrainingStatusV1, field));             \
    QNP_CHECK(sizeof(modern.field) == sizeof(legacy.field));           \
    QNP_CHECK(std::memcmp(modern.field, legacy.field,                  \
                          sizeof(legacy.field)) == 0)

    QNP_FROZEN_FIELD(apiVersion);
    QNP_FROZEN_FIELD(structSize);
    QNP_FROZEN_FIELD(explicitTrainingPresent);
    QNP_FROZEN_FIELD(available);
    QNP_FROZEN_FIELD(disableReason);
    QNP_FROZEN_FIELD(lifecycleState);
    QNP_FROZEN_FIELD(inMatchReady);
    QNP_FROZEN_FIELD(capabilityFlags);
    QNP_FROZEN_FIELD(trainingGeneration);
    QNP_FROZEN_FIELD(realStepSerialLow);
    QNP_FROZEN_FIELD(realStepSerialHigh);
    QNP_FROZEN_FIELD(scene);
    QNP_FROZEN_FIELD(matchSubstate);
    QNP_FROZEN_FIELD(p1Character);
    QNP_FROZEN_FIELD(p2Character);
    QNP_FROZEN_FIELD(p1Facing);
    QNP_FROZEN_FIELD(p2Facing);
    QNP_FROZEN_FIELD(p1FinalLogicalMask);
    QNP_FROZEN_FIELD(p2FinalLogicalMask);
    QNP_FROZEN_FIELD(comMask);
    QNP_FROZEN_FIELD(p1ComPrimary);
    QNP_FROZEN_FIELD(p1ComMirror);
    QNP_FROZEN_FIELD(p2ComPrimary);
    QNP_FROZEN_FIELD(p2ComMirror);
    QNP_FROZEN_FIELD(guardMode);
    QNP_FROZEN_FIELD(guardPermission);
    QNP_FROZEN_FIELD(guardClassification);
    QNP_FROZEN_FIELD(guardPredictionMisses);
    QNP_FROZEN_FIELD(guardDuringPlayback);
    QNP_FROZEN_FIELD(endlessRoundRequested);
    QNP_FROZEN_FIELD(endlessRoundEffective);
    QNP_FROZEN_FIELD(infiniteTimeRequested);
    QNP_FROZEN_FIELD(infiniteTimeEffective);
    QNP_FROZEN_FIELD(extraFunctionsDisabled);
    QNP_FROZEN_FIELD(waitCountZero);
    QNP_FROZEN_FIELD(pauseNeutral);
    QNP_FROZEN_FIELD(freezeNeutral);
    QNP_FROZEN_FIELD(lastAdmissionResult);
    QNP_FROZEN_FIELD(lastNativeReturn);
    QNP_FROZEN_FIELD(inputRearmState);
    QNP_FROZEN_FIELD(macroState);
    QNP_FROZEN_FIELD(macroCursor);
    QNP_FROZEN_FIELD(selectedMacroSlot);
    QNP_FROZEN_FIELD(playbackMode);
    QNP_FROZEN_FIELD(facingPolicy);
    QNP_FROZEN_FIELD(validMacroSlots);
    QNP_FROZEN_ARRAY(macroFrameCounts);
    QNP_FROZEN_FIELD(macroFrameCapacity);
    QNP_FROZEN_FIELD(selectedStateSlot);
    QNP_FROZEN_FIELD(validStateSlots);
    QNP_FROZEN_FIELD(staleStateSlots);
    QNP_FROZEN_FIELD(rollbackLayoutGeneration);
    QNP_FROZEN_FIELD(characterTeamFingerprintLow);
    QNP_FROZEN_FIELD(characterTeamFingerprintHigh);
    QNP_FROZEN_FIELD(hitboxRequested);
    QNP_FROZEN_FIELD(hitboxEffective);
    QNP_FROZEN_FIELD(menuCaptured);
    QNP_FROZEN_FIELD(lastAcknowledgedCommandId);
    QNP_FROZEN_FIELD(lastCommandResult);
    QNP_FROZEN_FIELD(commandQueueDepth);
    QNP_FROZEN_FIELD(commandQueueCapacity);
    QNP_FROZEN_FIELD(commandQueueDrops);
    QNP_FROZEN_FIELD(stickyValidationErrors);
    QNP_FROZEN_ARRAY(disableText);
    QNP_FROZEN_ARRAY(statusText);
    QNP_FROZEN_ARRAY(resultText);

#undef QNP_FROZEN_FIELD
#undef QNP_FROZEN_ARRAY

    QNP_CHECK(std::memcmp(&legacy, &modern,
                          kTrainingStatusV1BaseSize) == 0);
    QNP_CHECK(bytes[kTrainingStatusV1BaseSize] == 0xEE);
    QNP_CHECK(modern.combatReadoutValid == 0xEEEEEEEEu);
    QNP_CHECK(modern.p2MaxComboDamageDealt == 0xEEEEEEEEu);
}

QNP_TEST(TrainingStatusGrowthLeavesNewWrapperOnStaleDllReadingZeroedTail) {
    LegacyTrainingStatusV1 stale;
    std::memset(&stale, 0, sizeof(stale));
    stale.apiVersion = kTrainingApiVersion;
    stale.structSize = sizeof(LegacyTrainingStatusV1);
    stale.available = 1u;
    stale.p1FinalLogicalMask = qinput::kLogicalRight | qinput::kLogicalA;
    stale.macroFrameCounts[3] = 120u;
    std::memcpy(stale.resultText, "stale", 6);

    TrainingStatusV1 modern{};
    std::memset(&modern, 0, sizeof(modern));
    std::memcpy(&modern, &stale, stale.structSize);

    QNP_CHECK(modern.apiVersion == kTrainingApiVersion);
    QNP_CHECK(modern.structSize >= kTrainingStatusV1BaseSize);
    QNP_CHECK(modern.structSize <= sizeof(TrainingStatusV1));
    QNP_CHECK(modern.structSize < sizeof(TrainingStatusV1));

    QNP_CHECK(modern.available == 1u);
    QNP_CHECK(modern.p1FinalLogicalMask
              == (qinput::kLogicalRight | qinput::kLogicalA));
    QNP_CHECK(modern.macroFrameCounts[3] == 120u);
    QNP_CHECK(std::strcmp(modern.resultText, "stale") == 0);

    QNP_CHECK(modern.combatReadoutValid == 0u);
    QNP_CHECK(modern.dummyStance
              == static_cast<uint32_t>(DummyStance::Stand));
    QNP_CHECK(modern.dummyTech
              == static_cast<uint32_t>(DummyTech::Off));
    QNP_CHECK(modern.lifeMax == 0u);
    QNP_CHECK(modern.p1GuardGauge == 0u && modern.p2GuardGauge == 0u);
    QNP_CHECK(modern.p1MeterStocks == 0u && modern.p2MeterStocks == 0u);
    QNP_CHECK(modern.damageEventSerial == 0u);
    QNP_CHECK(modern.damageDeclinedCount == 0u);
    QNP_CHECK(modern.lastHitCause == 0u);
    QNP_CHECK(modern.lastHitAdmitted == 0u);
    QNP_CHECK(modern.p1MaxComboDamageDealt == 0u);
    QNP_CHECK(modern.p2MaxComboDamageDealt == 0u);
    const unsigned char* bytes =
        reinterpret_cast<const unsigned char*>(&modern);
    for (uint32_t offset = kTrainingStatusV1BaseSize;
         offset < sizeof(modern); ++offset) {
        QNP_CHECK(bytes[offset] == 0u);
    }
}

QNP_TEST(TrainingStatusSizeNegotiationPicksTheLargestFrozenTier) {
    QNP_CHECK(kTrainingStatusV1TierCount >= 2u);
    QNP_CHECK(trainingStatusTiersAscend());
    QNP_CHECK(kTrainingStatusV1SizeTiers[0] == kTrainingStatusV1BaseSize);
    QNP_CHECK(kTrainingStatusV1SizeTiers[kTrainingStatusV1TierCount - 1]
              == sizeof(TrainingStatusV1));

    QNP_CHECK(trainingStatusTierFor(0u) == 0u);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1BaseSize - 1u) == 0u);

    for (uint32_t i = 0; i < kTrainingStatusV1TierCount; ++i) {
        const uint32_t tier = kTrainingStatusV1SizeTiers[i];
        QNP_CHECK(trainingStatusTierFor(tier) == tier);
        QNP_CHECK(trainingStatusTierFor(tier + 1u) == tier);
        if (i + 1 < kTrainingStatusV1TierCount) {
            QNP_CHECK(trainingStatusTierFor(
                          kTrainingStatusV1SizeTiers[i + 1] - 1u) == tier);
        }
    }

    QNP_CHECK(trainingStatusTierFor(
                  static_cast<uint32_t>(sizeof(TrainingStatusV1)) + 4096u)
              == sizeof(TrainingStatusV1));
    for (uint32_t i = 0; i < kTrainingStatusV1TierCount; ++i)
        QNP_CHECK(kTrainingStatusV1SizeTiers[i] <= sizeof(TrainingStatusV1));
}

QNP_TEST(TrainingCommandPayloadAndCoalescingContract) {
    TrainingCommandV1 c =
        command(1, 1, TrainingCommandKind::ClearMacro);
    c.slotOrSetting = 7;
    QNP_CHECK(validCommandPayload(c));
    c.slotOrSetting = 8;
    QNP_CHECK(!validCommandPayload(c));

    c = command(
        1, 2, TrainingCommandKind::ReturnToCharacterSelect);
    QNP_CHECK(validCommandPayload(c));
    c.value = 1;
    QNP_CHECK(!validCommandPayload(c));
    c.value = 0;
    c.slotOrSetting = 1;
    QNP_CHECK(!validCommandPayload(c));
    c.slotOrSetting = 0;
    c.flags = 1;
    QNP_CHECK(!validCommandPayload(c));

    QNP_CHECK(commandMayCoalesce(TrainingCommandKind::SetGuardMode));
    QNP_CHECK(commandMayCoalesce(TrainingCommandKind::SelectMacroSlot));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::ResetPosition));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::SaveState));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::LoadState));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::ClearMacro));
    QNP_CHECK(!commandMayCoalesce(
        TrainingCommandKind::ReturnToCharacterSelect));
    QNP_CHECK(commandMayCoalesce(
        TrainingCommandKind::SetThrowTechChance));
    QNP_CHECK(commandMayCoalesce(TrainingCommandKind::SetTriggerConfig));
    QNP_CHECK(commandMayCoalesce(
        TrainingCommandKind::SetFrameAdvantageEnabled));
}

QNP_TEST(TrainingCommandCoalescingIsExhaustivelyClassified) {
    for (uint32_t raw =
             static_cast<uint32_t>(TrainingCommandKind::None);
         raw <= static_cast<uint32_t>(TrainingCommandKind::Count);
         ++raw) {
        const auto kind = static_cast<TrainingCommandKind>(raw);
        bool expected = false;
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
                expected = true;
                break;
            default:
                expected = false;
                break;
        }
        QNP_CHECK(commandMayCoalesce(kind) == expected);
    }
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::RefillLife));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::RefillGuardGauge));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::RefillExMeter));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::SetHpRegen));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::SetLifeValue));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::SetExMeter));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::SetGuardGaugeValue));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::ResetRoundStartNeutral));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::TogglePause));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::FrameStep));
}

QNP_TEST(TrainingAppendedCommandKindsAreNumberedAndValidated) {
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetHpRegen) == 17u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetStance) == 18u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetTech) == 19u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::RefillLife) == 20u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::RefillGuardGauge) == 21u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::RefillExMeter) == 22u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetLifeValue) == 23u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetExMeter) == 24u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetGuardGaugeValue) == 25u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetHealthPolicy) == 26u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetExPolicy) == 27u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetGuardGaugePolicy) == 28u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::ResetRoundStartNeutral) == 29u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetRecoveryPolicy) == 30u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetRecoveryTiming) == 31u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetTerminalRecoveryChoice) == 32u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::TogglePause) == 33u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::FrameStep) == 34u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetMeterChargeConfig) == 35u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetThrowTechChance) == 36u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetTriggerConfig) == 37u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetFrameAdvantageEnabled) == 38u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetDebugOverlayEnabled) == 39u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetStateChangeLogEnabled) == 40u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetNativeHudMask) == 41u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetFrameTraceEnabled) == 42u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetContactPolicy) == 43u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetExRestoreTarget) == 44u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetMacroAfterReset) == 45u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetGuardTiming) == 46u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::SetPlayerSwap) == 47u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::BeginPreRecord) == 48u);
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::Count) == 49u);
    {
        TrainingCommandV1 c = command(1u, 1u,
                                      TrainingCommandKind::SetMacroAfterReset);
        c.value = static_cast<uint32_t>(TrainingMacroAfterReset::Record);
        QNP_CHECK(validCommandPayload(c));
        c.value = kTrainingMacroAfterResetMax + 1u;
        QNP_CHECK(!validCommandPayload(c));
        c.value = 1u;
        c.slotOrSetting = 1u;
        QNP_CHECK(!validCommandPayload(c));
        c.slotOrSetting = 0u;
        c.flags = 1u;
        QNP_CHECK(!validCommandPayload(c));
    }
    QNP_CHECK(isValidCommandKind(
        TrainingCommandKind::SetFrameTraceEnabled));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::SetContactPolicy));
    {
        TrainingCommandV1 c = command(1u, 1u,
                                      TrainingCommandKind::SetContactPolicy);
        c.slotOrSetting = static_cast<uint32_t>(
            TrainingContactPolicySlot::ParryDummy);
        c.value = static_cast<uint32_t>(TrainingParryPolicy::Never);
        QNP_CHECK(validCommandPayload(c));
        c.value = static_cast<uint32_t>(TrainingCriticalPolicy::ForceRandomRoll);
        QNP_CHECK(!validCommandPayload(c));
        c.slotOrSetting = static_cast<uint32_t>(
            TrainingContactPolicySlot::CriticalP2Attacks);
        QNP_CHECK(validCommandPayload(c));
        c.slotOrSetting = 9u;
        QNP_CHECK(!validCommandPayload(c));
        c.slotOrSetting = static_cast<uint32_t>(
            TrainingContactPolicySlot::CriticalP1Attacks);
        c.value = 0u;
        c.flags = 1u;
        QNP_CHECK(!validCommandPayload(c));
    }
    {
        TrainingCommandV1 traceProbe{};
        traceProbe.commandKind = static_cast<uint32_t>(
            TrainingCommandKind::SetFrameTraceEnabled);
        traceProbe.value = 1u;
        QNP_CHECK(validCommandPayload(traceProbe));
        traceProbe.value = 2u;
        QNP_CHECK(!validCommandPayload(traceProbe));
        traceProbe.value = 1u;
        traceProbe.slotOrSetting = 1u;
        QNP_CHECK(!validCommandPayload(traceProbe));
        traceProbe.slotOrSetting = 0u;
        traceProbe.flags = 1u;
        QNP_CHECK(!validCommandPayload(traceProbe));
    }
    QNP_CHECK(isValidCommandKind(
        TrainingCommandKind::SetDebugOverlayEnabled));
    QNP_CHECK(isValidCommandKind(
        TrainingCommandKind::SetStateChangeLogEnabled));
    {
        TrainingCommandV1 probe{};
        probe.commandKind = static_cast<uint32_t>(
            TrainingCommandKind::SetStateChangeLogEnabled);
        probe.value = 1u;
        QNP_CHECK(validCommandPayload(probe));
        probe.value = 2u;
        QNP_CHECK(!validCommandPayload(probe));
        probe.value = 1u;
        probe.flags = 1u;
        QNP_CHECK(!validCommandPayload(probe));
    }
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::SetStance));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::SetTech));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::RefillLife));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::RefillGuardGauge));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::RefillExMeter));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::SetLifeValue));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::FrameStep));
    QNP_CHECK(!isValidCommandKind(TrainingCommandKind::Count));
    QNP_CHECK(!isValidCommandKind(TrainingCommandKind::None));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::SetMeterChargeConfig));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::SetThrowTechChance));
    QNP_CHECK(isValidCommandKind(TrainingCommandKind::SetTriggerConfig));
    QNP_CHECK(isValidCommandKind(
        TrainingCommandKind::SetFrameAdvantageEnabled));
    QNP_CHECK(!isValidCommandKind(static_cast<TrainingCommandKind>(99)));

    for (uint32_t value = 0; value <= 3; ++value) {
        TrainingCommandV1 retired =
            command(1, 1, TrainingCommandKind::SetHpRegen);
        retired.value = value;
        QNP_CHECK(!validCommandPayload(retired));
    }
    TrainingCommandV1 retiredZero =
        command(1, 1, TrainingCommandKind::SetHpRegen);
    QNP_CHECK(!validCommandPayload(retiredZero));

    for (uint32_t value = 0; value <= 4; ++value) {
        TrainingCommandV1 stance =
            command(1, 1, TrainingCommandKind::SetStance);
        stance.value = value;
        QNP_CHECK(validCommandPayload(stance)
                  == (value
                      <= static_cast<uint32_t>(DummyStance::Jump)));
        TrainingCommandV1 tech =
            command(1, 1, TrainingCommandKind::SetTech);
        tech.value = value;
        QNP_CHECK(validCommandPayload(tech)
                  == (value
                      <= static_cast<uint32_t>(DummyTech::HoldGuard)));
    }
    TrainingCommandV1 directional =
        command(1, 1, TrainingCommandKind::SetTech);
    directional.value = 2;
    QNP_CHECK(!validCommandPayload(directional));

    for (const TrainingCommandKind kind :
         {TrainingCommandKind::SetStance, TrainingCommandKind::SetTech}) {
        TrainingCommandV1 c = command(1, 1, kind);
        QNP_CHECK(validCommandPayload(c));
        c.slotOrSetting = 1;
        QNP_CHECK(!validCommandPayload(c));
        c.slotOrSetting = 0;
        c.flags = 1;
        QNP_CHECK(!validCommandPayload(c));
    }

    for (const TrainingCommandKind kind :
         {TrainingCommandKind::RefillLife,
          TrainingCommandKind::RefillGuardGauge,
          TrainingCommandKind::RefillExMeter}) {
        for (uint32_t value = 0; value <= 4; ++value) {
            TrainingCommandV1 c = command(1, 1, kind);
            c.value = value;
            QNP_CHECK(validCommandPayload(c)
                      == (value
                          <= static_cast<uint32_t>(
                              TrainingRefillTarget::P2)));
        }
        TrainingCommandV1 c = command(1, 1, kind);
        QNP_CHECK(validCommandPayload(c));
        QNP_CHECK(static_cast<TrainingRefillTarget>(c.value)
                  == TrainingRefillTarget::Both);
        c.slotOrSetting = 1;
        QNP_CHECK(!validCommandPayload(c));
        c.slotOrSetting = 0;
        c.flags = 0x80000000u;
        QNP_CHECK(!validCommandPayload(c));
    }

    TrainingCommandV1 throwTech =
        command(1, 1, TrainingCommandKind::SetThrowTechChance);
    throwTech.value = 0u;
    QNP_CHECK(validCommandPayload(throwTech));
    throwTech.value = kTrainingTriggerMaximumChancePercent;
    QNP_CHECK(validCommandPayload(throwTech));
    throwTech.value = kTrainingTriggerMaximumChancePercent + 1u;
    QNP_CHECK(!validCommandPayload(throwTech));
    throwTech.value = 50u;
    throwTech.slotOrSetting = 1u;
    QNP_CHECK(!validCommandPayload(throwTech));
    throwTech.slotOrSetting = 0u;
    throwTech.flags = 1u;
    QNP_CHECK(!validCommandPayload(throwTech));

    for (uint32_t event = 0u; event < kTrainingTriggerEventCount; ++event) {
        TrainingCommandV1 trigger =
            command(1, 1, TrainingCommandKind::SetTriggerConfig);
        trigger.slotOrSetting = event;
        trigger.value = kTrainingMacroSlotCount;
        trigger.flags = kTrainingTriggerMaximumDelaySteps
            | (kTrainingTriggerMaximumChancePercent << 16u);
        QNP_CHECK(validCommandPayload(trigger));
    }
    TrainingCommandV1 trigger =
        command(1, 1, TrainingCommandKind::SetTriggerConfig);
    trigger.slotOrSetting = kTrainingTriggerEventCount;
    QNP_CHECK(!validCommandPayload(trigger));
    trigger.slotOrSetting = 0u;
    trigger.value = kTrainingTriggerRandomMacroSlot;
    QNP_CHECK(validCommandPayload(trigger));
    trigger.value = kTrainingTriggerRandomMacroSlot + 1u;
    QNP_CHECK(!validCommandPayload(trigger));
    trigger.value = 0u;
    trigger.flags = kTrainingTriggerMaximumDelaySteps + 1u;
    QNP_CHECK(!validCommandPayload(trigger));
    trigger.flags =
        (kTrainingTriggerMaximumChancePercent + 1u) << 16u;
    QNP_CHECK(!validCommandPayload(trigger));
    trigger.flags = kTrainingTriggerMaximumDelaySteps
        | (kTrainingTriggerMaximumChancePercent << 16u);
    QNP_CHECK(validCommandPayload(trigger));
    trigger.value = encodeTrainingTriggerMoveValue(1u);
    QNP_CHECK(validCommandPayload(trigger));
    trigger.value = encodeTrainingTriggerMoveValue(
        kTrainingTriggerMaximumMoveIndex);
    QNP_CHECK(validCommandPayload(trigger));
    trigger.value = kTrainingTriggerMoveValueFlag;
    QNP_CHECK(!validCommandPayload(trigger));
    trigger.value = kTrainingTriggerMoveValueFlag
        | (kTrainingTriggerMaximumMoveIndex + 1u);
    QNP_CHECK(!validCommandPayload(trigger));
    trigger.value = kTrainingTriggerMoveValueFlag | (1u << 20u) | 1u;
    QNP_CHECK(!validCommandPayload(trigger));

    TrainingTriggerConfig config{};
    QNP_CHECK(trainingTriggerConfigValid(config));
    config.macroSlot = static_cast<uint8_t>(kTrainingMacroSlotCount);
    config.chancePercent =
        static_cast<uint8_t>(kTrainingTriggerMaximumChancePercent);
    config.delaySteps =
        static_cast<uint16_t>(kTrainingTriggerMaximumDelaySteps);
    QNP_CHECK(trainingTriggerConfigValid(config));
    config.macroSlot = kTrainingTriggerRandomMacroSlot;
    QNP_CHECK(trainingTriggerConfigValid(config));
    config.macroSlot =
        static_cast<uint8_t>(kTrainingTriggerRandomMacroSlot + 1u);
    QNP_CHECK(!trainingTriggerConfigValid(config));
    config.macroSlot = 0u;
    config.moveIndex = static_cast<uint16_t>(
        kTrainingTriggerMaximumMoveIndex);
    QNP_CHECK(trainingTriggerConfigValid(config));
    config.macroSlot = 1u;
    QNP_CHECK(!trainingTriggerConfigValid(config));
    config.macroSlot = 0u;
    config.moveIndex = static_cast<uint16_t>(
        kTrainingTriggerMaximumMoveIndex + 1u);
    QNP_CHECK(!trainingTriggerConfigValid(config));
    config.moveIndex = 0u;
    config.chancePercent = static_cast<uint8_t>(
        kTrainingTriggerMaximumChancePercent + 1u);
    QNP_CHECK(!trainingTriggerConfigValid(config));
    config.chancePercent = 0u;
    config.delaySteps = static_cast<uint16_t>(
        kTrainingTriggerMaximumDelaySteps + 1u);
    QNP_CHECK(!trainingTriggerConfigValid(config));

    TrainingCommandV1 advantage =
        command(1, 1, TrainingCommandKind::SetFrameAdvantageEnabled);
    QNP_CHECK(validCommandPayload(advantage));
    advantage.value = 1u;
    QNP_CHECK(validCommandPayload(advantage));
    advantage.value = 2u;
    QNP_CHECK(!validCommandPayload(advantage));
    advantage.value = 1u;
    advantage.slotOrSetting = 1u;
    QNP_CHECK(!validCommandPayload(advantage));
    advantage.slotOrSetting = 0u;
    advantage.flags = 1u;
    QNP_CHECK(!validCommandPayload(advantage));
}

QNP_TEST(TrainingRefillTargetNeverAddressesSlotsTwoOrThree) {
    QNP_CHECK(kTrainingRefillPlayerCount == 2u);
    QNP_CHECK(static_cast<uint32_t>(TrainingRefillTarget::Both) == 0u);
    QNP_CHECK(static_cast<uint32_t>(TrainingRefillTarget::P1) == 1u);
    QNP_CHECK(static_cast<uint32_t>(TrainingRefillTarget::P2) == 2u);
    QNP_CHECK(isValidRefillTarget(TrainingRefillTarget::Both));
    QNP_CHECK(isValidRefillTarget(TrainingRefillTarget::P1));
    QNP_CHECK(isValidRefillTarget(TrainingRefillTarget::P2));
    QNP_CHECK(!isValidRefillTarget(
        static_cast<TrainingRefillTarget>(3)));
    QNP_CHECK(!isValidRefillTarget(
        static_cast<TrainingRefillTarget>(0xFFFFFFFFu)));

    QNP_CHECK(refillTargetIncludesPlayer(
        TrainingRefillTarget::Both, 0u));
    QNP_CHECK(refillTargetIncludesPlayer(
        TrainingRefillTarget::Both, 1u));
    QNP_CHECK(refillTargetIncludesPlayer(TrainingRefillTarget::P1, 0u));
    QNP_CHECK(!refillTargetIncludesPlayer(TrainingRefillTarget::P1, 1u));
    QNP_CHECK(!refillTargetIncludesPlayer(TrainingRefillTarget::P2, 0u));
    QNP_CHECK(refillTargetIncludesPlayer(TrainingRefillTarget::P2, 1u));

    for (uint32_t raw = 0; raw <= 4u; ++raw) {
        const auto target = static_cast<TrainingRefillTarget>(raw);
        for (uint32_t player = 2u; player < 8u; ++player)
            QNP_CHECK(!refillTargetIncludesPlayer(target, player));
        QNP_CHECK(!refillTargetIncludesPlayer(target, 0xFFFFFFFFu));
    }
    QNP_CHECK(!refillTargetIncludesPlayer(
        static_cast<TrainingRefillTarget>(3), 0u));
    QNP_CHECK(!refillTargetIncludesPlayer(
        static_cast<TrainingRefillTarget>(3), 1u));
}

QNP_TEST(TrainingCommandIngressRejectsRetiredAndUnknownKinds) {
    TrainingCommandQueue queue;
    queue.publishGeneration(3);

    TrainingCommandV1 retired =
        command(3, 1, TrainingCommandKind::SetHpRegen);
    retired.value = 1;
    QNP_CHECK(queue.tryEnqueue(&retired, sizeof(retired))
              == CommandIngressResult::InvalidPayload);
    retired.value = 0;
    QNP_CHECK(queue.tryEnqueue(&retired, sizeof(retired))
              == CommandIngressResult::InvalidPayload);

    TrainingCommandV1 future = command(3, 2, TrainingCommandKind::Count);
    QNP_CHECK(queue.tryEnqueue(&future, sizeof(future))
              == CommandIngressResult::InvalidKind);
    future.commandKind =
        static_cast<uint32_t>(TrainingCommandKind::Count) + 1u;
    QNP_CHECK(queue.tryEnqueue(&future, sizeof(future))
              == CommandIngressResult::InvalidKind);

    TrainingCommandV1 badTarget =
        command(3, 3, TrainingCommandKind::RefillExMeter);
    badTarget.value = 3;
    QNP_CHECK(queue.tryEnqueue(&badTarget, sizeof(badTarget))
              == CommandIngressResult::InvalidPayload);

    TrainingCommandV1 first =
        command(3, 4, TrainingCommandKind::RefillLife);
    first.value = static_cast<uint32_t>(TrainingRefillTarget::Both);
    QNP_CHECK(queue.tryEnqueue(&first, sizeof(first))
              == CommandIngressResult::Accepted);
    TrainingCommandV1 second = first;
    second.commandId = 5;
    QNP_CHECK(queue.tryEnqueue(&second, sizeof(second))
              == CommandIngressResult::Accepted);

    TrainingCommandV1 stance =
        command(3, 6, TrainingCommandKind::SetStance);
    stance.value = static_cast<uint32_t>(DummyStance::Crouch);
    QNP_CHECK(queue.tryEnqueue(&stance, sizeof(stance))
              == CommandIngressResult::Accepted);

    TrainingCommandV1 out{};
    QNP_REQUIRE(queue.tryDequeue(out));
    QNP_CHECK(out.commandId == 4);
    QNP_CHECK(out.commandKind
              == static_cast<uint32_t>(TrainingCommandKind::RefillLife));
    QNP_REQUIRE(queue.tryDequeue(out));
    QNP_CHECK(out.commandId == 5);
    QNP_CHECK(out.commandKind
              == static_cast<uint32_t>(TrainingCommandKind::RefillLife));
    QNP_REQUIRE(queue.tryDequeue(out));
    QNP_CHECK(out.commandId == 6);
    QNP_CHECK(out.value
              == static_cast<uint32_t>(DummyStance::Crouch));
    QNP_CHECK(!queue.tryDequeue(out));
    QNP_CHECK(queue.rejectedCommands() == 5);
}

QNP_TEST(TrainingNativeImageTransactionsAreExhaustivelyClassified) {
    for (uint32_t raw =
             static_cast<uint32_t>(TrainingCommandKind::None);
         raw <= static_cast<uint32_t>(TrainingCommandKind::Count);
         ++raw) {
        const auto kind = static_cast<TrainingCommandKind>(raw);
        const bool expected =
            kind == TrainingCommandKind::ResetPosition
            || kind == TrainingCommandKind::ResetRoundStartNeutral
            || kind == TrainingCommandKind::LoadState;
        QNP_CHECK(commandOwnsNativeImageTransaction(kind) == expected);
    }
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::SaveState));
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::SetHitboxDisplay));
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::ReturnToCharacterSelect));
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::SetLifeValue));
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::SetExMeter));
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::SetGuardGaugeValue));
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::TogglePause));
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::FrameStep));
}

QNP_TEST(TrainingCommandQueueBoundsOrderingAndGeneration) {
    TrainingCommandQueue queue;
    queue.publishGeneration(9);
    QNP_CHECK(queue.publishedGeneration() == 9);

    TrainingCommandV1 stale =
        command(8, 1, TrainingCommandKind::ToggleRecord);
    QNP_CHECK(queue.tryEnqueue(&stale, sizeof(stale))
              == CommandIngressResult::StaleGeneration);

    for (uint32_t i = 1; i <= kTrainingCommandCapacity; ++i) {
        TrainingCommandV1 c =
            command(9, i, TrainingCommandKind::ToggleRecord);
        QNP_CHECK(queue.tryEnqueue(&c, sizeof(c))
                  == CommandIngressResult::Accepted);
    }
    QNP_CHECK(queue.approximateDepth() == kTrainingCommandCapacity);
    TrainingCommandV1 overflow =
        command(9, kTrainingCommandCapacity + 1,
                TrainingCommandKind::ToggleRecord);
    QNP_CHECK(queue.tryEnqueue(&overflow, sizeof(overflow))
              == CommandIngressResult::QueueFull);
    QNP_CHECK(queue.queueFullDrops() == 1);

    TrainingCommandV1 out{};
    for (uint32_t i = 1; i <= kTrainingCommandCapacity; ++i) {
        QNP_REQUIRE(queue.tryDequeue(out));
        QNP_CHECK(out.commandId == i);
    }
    QNP_CHECK(!queue.tryDequeue(out));

    QNP_CHECK(queue.tryEnqueue(&overflow, sizeof(overflow))
              == CommandIngressResult::Accepted);
    QNP_REQUIRE(queue.tryDequeue(out));
    QNP_CHECK(out.commandId == kTrainingCommandCapacity + 1);

    TrainingCommandV1 duplicate = overflow;
    QNP_CHECK(queue.tryEnqueue(&duplicate, sizeof(duplicate))
              == CommandIngressResult::NonMonotonicId);

    queue.publishGeneration(10);
    TrainingCommandV1 fresh =
        command(10, 1, TrainingCommandKind::TogglePlayback);
    QNP_CHECK(queue.tryEnqueue(&fresh, sizeof(fresh))
              == CommandIngressResult::Accepted);
}

QNP_TEST(TrainingCommandQueueRejectsMalformedAbi) {
    TrainingCommandQueue queue;
    queue.publishGeneration(1);
    TrainingCommandV1 c =
        command(1, 1, TrainingCommandKind::ToggleRecord);
    c.apiVersion = 99;
    QNP_CHECK(queue.tryEnqueue(&c, sizeof(c))
              == CommandIngressResult::ApiVersionMismatch);
    c.apiVersion = kTrainingApiVersion;
    c.structSize = sizeof(c) - 1;
    QNP_CHECK(queue.tryEnqueue(&c, sizeof(c))
              == CommandIngressResult::StructTooSmall);
    c.structSize = sizeof(c);
    c.commandKind = 0xFFFFFFFFu;
    QNP_CHECK(queue.tryEnqueue(&c, sizeof(c))
              == CommandIngressResult::InvalidKind);
    QNP_CHECK(queue.rejectedCommands() == 3);
}

QNP_TEST(TrainingCommandQueueDropsStaleEntriesAtDrain) {
    TrainingCommandQueue queue;
    queue.publishGeneration(1);
    TrainingCommandV1 old =
        command(1, 1, TrainingCommandKind::ToggleRecord);
    QNP_REQUIRE(queue.tryEnqueue(&old, sizeof(old))
                == CommandIngressResult::Accepted);
    queue.publishGeneration(2);
    TrainingCommandV1 current =
        command(2, 1, TrainingCommandKind::TogglePlayback);
    QNP_REQUIRE(queue.tryEnqueue(&current, sizeof(current))
                == CommandIngressResult::Accepted);
    TrainingCommandV1 out{};
    QNP_REQUIRE(queue.tryDequeue(out));
    QNP_CHECK(out.trainingGeneration == 2);
    QNP_CHECK(out.commandKind
              == static_cast<uint32_t>(
                  TrainingCommandKind::TogglePlayback));
    QNP_CHECK(!queue.tryDequeue(out));
    QNP_CHECK(queue.rejectedCommands() == 1);
}

QNP_TEST(TrainingCommandQueueConcurrentSpscStress) {
    constexpr uint32_t kCount = 20000;
    TrainingCommandQueue queue;
    queue.publishGeneration(44);
    std::atomic<uint32_t> failures{0};
    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        for (uint32_t id = 1; id <= kCount; ++id) {
            TrainingCommandV1 c =
                command(44, id, TrainingCommandKind::ToggleRecord);
            for (;;) {
                const CommandIngressResult result =
                    queue.tryEnqueue(&c, sizeof(c));
                if (result == CommandIngressResult::Accepted) break;
                if (result != CommandIngressResult::QueueFull) {
                    failures.fetch_add(1);
                    break;
                }
                std::this_thread::yield();
            }
        }
        producerDone.store(true);
    });
    uint32_t expected = 1;
    while (expected <= kCount) {
        TrainingCommandV1 out{};
        if (!queue.tryDequeue(out)) {
            if (producerDone.load() && queue.approximateDepth() == 0)
                break;
            std::this_thread::yield();
            continue;
        }
        if (out.commandId != expected) failures.fetch_add(1);
        ++expected;
    }
    producer.join();
    QNP_CHECK(failures.load() == 0);
    QNP_CHECK(expected == kCount + 1);
}

QNP_TEST(TrainingStatusPublisherCopiesWholeSnapshot) {
    TrainingStatusPublisher publisher;
    TrainingStatusV1 initial{};
    QNP_REQUIRE(publisher.read(initial));
    QNP_CHECK(initial.apiVersion == kTrainingApiVersion);

    TrainingStatusV1 expected{};
    expected.trainingGeneration = 77;
    expected.realStepSerialLow = 1234;
    expected.available = 1;
    expected.capabilityFlags =
        TrainingCapabilityReset | TrainingCapabilityMacros;
    expected.p1FinalLogicalMask =
        qinput::kLogicalRight | qinput::kLogicalA;
    expected.macroFrameCounts[7] = 3600;
    expected.commandQueueDepth = 13;
    std::memset(expected.statusText, 'x', sizeof(expected.statusText));
    publisher.publish(expected);

    TrainingStatusV1 actual{};
    QNP_REQUIRE(publisher.read(actual));
    QNP_CHECK(actual.trainingGeneration == 77);
    QNP_CHECK(actual.realStepSerialLow == 1234);
    QNP_CHECK(actual.p1FinalLogicalMask == expected.p1FinalLogicalMask);
    QNP_CHECK(actual.macroFrameCounts[7] == 3600);
    QNP_CHECK(actual.commandQueueCapacity == kTrainingCommandCapacity);
    QNP_CHECK(actual.statusText[95] == '\0');
}

QNP_TEST(TrainingStatusPublisherConcurrentStress) {
    constexpr uint32_t kCount = 10000;
    TrainingStatusPublisher publisher;
    std::atomic<bool> writerDone{false};
    std::atomic<uint32_t> failures{0};
    std::thread writer([&] {
        for (uint32_t value = 1; value <= kCount; ++value) {
            TrainingStatusV1 status{};
            status.trainingGeneration = value;
            status.realStepSerialLow = value;
            status.realStepSerialHigh = ~value;
            status.macroFrameCounts[0] = value * 3;
            publisher.publish(status);
        }
        writerDone.store(true);
    });
    do {
        TrainingStatusV1 status{};
        if (!publisher.read(status)) continue;
        const uint32_t value = status.trainingGeneration;
        if (value != 0
            && (status.realStepSerialLow != value
                || status.realStepSerialHigh != ~value
                || status.macroFrameCounts[0] != value * 3)) {
            failures.fetch_add(1);
        }
    } while (!writerDone.load());
    writer.join();
    TrainingStatusV1 final{};
    QNP_REQUIRE(publisher.read(final));
    QNP_CHECK(final.trainingGeneration == kCount);
    QNP_CHECK(failures.load() == 0);
}

QNP_TEST(TrainingInputAnOperationKeepsTheControlsYouAreHolding) {
    TrainingInput input;
    input.reset();
    uint64_t poll = 0;
    QNP_REQUIRE(armInput(input, poll).rearmState == InputRearmState::Armed);

    uint64_t serial = 0;
    TrainingInputStep step{};
    step.lifecycleAvailable = true;
    step.p1PhysicalMask = qinput::kLogicalLeft | qinput::kLogicalA;
    TrainingInputResult result = input.advance(++serial, step);
    QNP_CHECK(result.p1Mask == (qinput::kLogicalLeft | qinput::kLogicalA));

    input.resumeAfterOperation(0u);
    QNP_CHECK(pollInput(input, poll, 0, step.p1PhysicalMask, false).rearmState
              == InputRearmState::Armed);
    result = input.advance(++serial, step);
    QNP_CHECK(result.p1Mask == (qinput::kLogicalLeft | qinput::kLogicalA));

    input.resumeAfterOperation(qinput::kLogicalLeft);
    result = input.advance(++serial, step);
    QNP_CHECK(result.p1Mask == qinput::kLogicalA);
    result = input.advance(++serial, step);
    QNP_CHECK(result.p1Mask == qinput::kLogicalA);
    step.p1PhysicalMask = qinput::kLogicalA;
    result = input.advance(++serial, step);
    QNP_CHECK(result.p1Mask == qinput::kLogicalA);
    step.p1PhysicalMask = qinput::kLogicalLeft | qinput::kLogicalA;
    result = input.advance(++serial, step);
    QNP_CHECK(result.p1Mask == (qinput::kLogicalLeft | qinput::kLogicalA));

    input.resumeAfterOperation(qinput::kLogicalDown);
    step.p1PhysicalMask =
        qinput::kLogicalLeft | qinput::kLogicalDown | qinput::kLogicalA;
    result = input.advance(++serial, step);
    QNP_CHECK(result.p1Mask == (qinput::kLogicalLeft | qinput::kLogicalA));
}

QNP_TEST(TrainingInputCaptureReleaseRearmAndEdges) {
    TrainingInput input;
    input.reset();
    uint64_t poll = 0;
    TrainingActionResult actions = armInput(input, poll);
    QNP_CHECK(actions.rearmState == InputRearmState::Armed);

    const uint32_t reset = trainingActionBit(TrainingAction::ResetPosition);
    actions = pollInput(input, poll, reset, qinput::kLogicalRight, false);
    QNP_CHECK(actions.actionEdges == reset);
    QNP_CHECK(actions.resetModifier == qinput::kLogicalRight);

    TrainingActionPoll same{};
    same.lifecycleAvailable = true;
    same.heldTrainingActions = reset;
    same.p1PhysicalMask = qinput::kLogicalRight;
    const TrainingActionResult duplicate = input.pollActions(poll, same);
    QNP_CHECK(duplicate.duplicatePoll);
    QNP_CHECK(duplicate.actionEdges == 0);

    for (int frame = 0; frame < 120; ++frame) {
        actions = pollInput(input, poll, reset, qinput::kLogicalRight, false);
        QNP_CHECK(actions.actionEdges == 0);
    }
    actions = pollInput(input, poll, reset, 0, true);
    QNP_CHECK(actions.rearmState == InputRearmState::Armed);
    actions = pollInput(input, poll, 0, 0, true);
    QNP_CHECK(actions.actionEdges == 0);
    actions = pollInput(input, poll, reset, 0, true);
    QNP_CHECK(actions.actionEdges == reset);
    QNP_CHECK(actions.resetModifier == 0);

    const uint32_t step = trainingActionBit(TrainingAction::FrameStep);
    actions = pollInput(input, poll, 0, 0, true);
    uint32_t fired = 0;
    for (int frame = 0; frame < 60; ++frame) {
        actions = pollInput(input, poll, step, 0, true);
        if (actions.actionEdges & step) ++fired;
    }
    QNP_CHECK(fired == 1);

    TrainingActionPoll captured{};
    captured.lifecycleAvailable = true;
    captured.menuCaptured = true;
    captured.heldTrainingActions = step;
    actions = input.pollActions(++poll, captured);
    QNP_CHECK(actions.rearmState == InputRearmState::Captured);
    actions = pollInput(input, poll, step, 0, true);
    QNP_CHECK(actions.rearmState == InputRearmState::NeutralSettle);
    actions = pollInput(input, poll, step, 0, true);
    QNP_CHECK(actions.rearmState == InputRearmState::Armed);
    actions = pollInput(input, poll, step, 0, true);
    QNP_CHECK(actions.actionEdges == 0);
    actions = pollInput(input, poll, 0, 0, true);
    actions = pollInput(input, poll, step, 0, true);
    QNP_CHECK(actions.actionEdges == step);

    uint64_t stepSerial = 0;
    TrainingInputStep composed{};
    composed.lifecycleAvailable = true;
    composed.p1PhysicalMask = qinput::kLogicalRight;
    TrainingInputResult result = input.advance(++stepSerial, composed);
    QNP_CHECK(result.p1Mask == qinput::kLogicalRight);
    const TrainingInputResult repeat = input.advance(stepSerial, composed);
    QNP_CHECK(repeat.duplicateStep);
    QNP_CHECK(repeat.p1Mask == result.p1Mask);

    composed.operationNeutral = true;
    result = input.advance(++stepSerial, composed);
    QNP_CHECK(result.neutralized);
}

QNP_TEST(TrainingInputPrecedenceAndSocd) {
    TrainingInput input;
    input.reset();
    uint64_t serial = 0;
    armInput(input, serial);

    TrainingInputStep step{};
    step.lifecycleAvailable = true;
    step.macroOverrideP1 = true;
    step.macroP1Mask = 0;
    step.macroOverrideP2 = true;
    step.p1PhysicalMask =
        qinput::kLogicalRight | qinput::kLogicalA;
    step.macroP2Mask = step.p1PhysicalMask;
    step.p2PhysicalMask = qinput::kLogicalB;
    TrainingInputResult result = input.advance(++serial, step);
    QNP_CHECK(result.p1Mask == 0);
    QNP_CHECK(result.p2Mask
              == (qinput::kLogicalRight | qinput::kLogicalA));

    step.macroOverrideP1 = false;
    step.macroOverrideP2 = true;
    step.macroPlaybackActive = true;
    step.macroP2Mask = qinput::kLogicalB | qinput::kLogicalRight;
    step.dummyDirections =
        qinput::kLogicalLeft | qinput::kLogicalDown;
    step.dummyOwnedAxes = qinput::kLogicalDirectionMask;
    step.guardDuringPlayback = false;
    result = input.advance(++serial, step);
    QNP_CHECK(result.p2Mask
              == (qinput::kLogicalB | qinput::kLogicalRight));
    QNP_CHECK(result.postureOwnedAxes == 0u);

    step.guardDuringPlayback = true;
    step.horizontalSocd = qinput::SocdPolicy::Neutral;
    result = input.advance(++serial, step);
    QNP_CHECK(result.p2Mask
              == (qinput::kLogicalB | qinput::kLogicalRight
                  | qinput::kLogicalDown));
    QNP_CHECK(result.postureLane == kTrainingDrivenLaneP2);
    QNP_CHECK(result.postureOwnedAxes == kDummyVerticalAxis);

    step.macroP2Mask = qinput::kLogicalB;
    result = input.advance(++serial, step);
    QNP_CHECK(result.p2Mask
              == (qinput::kLogicalB | qinput::kLogicalLeft
                  | qinput::kLogicalDown));
    QNP_CHECK(result.postureLane == kTrainingDrivenLaneP2);
    QNP_CHECK(result.postureOwnedAxes == qinput::kLogicalDirectionMask);

    const uint32_t beforeP2 = result.p2Mask;
    step.operationNeutral = true;
    result = input.advance(++serial, step);
    QNP_CHECK(result.neutralized);
    TrainingActionPoll poll{};
    poll.lifecycleAvailable = true;
    poll.operationNeutral = true;
    poll.relevantPhysicalNeutralBeforeSocd = false;
    QNP_CHECK(input.pollActions(++serial, poll).rearmState
              == InputRearmState::Armed);
    step.operationNeutral = false;
    result = input.advance(++serial, step);
    QNP_CHECK(!result.neutralized);
    QNP_CHECK(result.p2Mask == beforeP2);

    input.reset();
    result = input.advance(++serial, step);
    QNP_CHECK(result.p2Mask != beforeP2);
}

QNP_TEST(TrainingDummyPostureReferenceTable) {
    const uint32_t left = qinput::kLogicalLeft;
    const uint32_t down = qinput::kLogicalDown;
    const uint32_t up = qinput::kLogicalUp;
    const uint32_t both = qinput::kLogicalDirectionMask;

    DummyPostureResult result =
        posture(GuardMode::Off, DummyStance::Stand, DummyTech::Off, 0);
    QNP_CHECK(result.valid);
    QNP_CHECK(result.directions == 0u);
    QNP_CHECK(result.ownedAxes == 0u);
    QNP_CHECK(result.horizontalSource == DummyPostureSource::None);
    QNP_CHECK(result.verticalSource == DummyPostureSource::None);

    result = posture(GuardMode::Off, DummyStance::Crouch,
                     DummyTech::Off, 0);
    QNP_CHECK(result.directions == down);
    QNP_CHECK(result.ownedAxes == kDummyVerticalAxis);
    QNP_CHECK(result.horizontalSource == DummyPostureSource::None);
    QNP_CHECK(result.verticalSource == DummyPostureSource::Stance);

    result = posture(GuardMode::Off, DummyStance::Jump,
                     DummyTech::Off, 0);
    QNP_CHECK(result.directions == up);
    QNP_CHECK(result.ownedAxes == kDummyVerticalAxis);
    QNP_CHECK(result.verticalSource == DummyPostureSource::Stance);

    for (const DummyStance stance :
         {DummyStance::Stand, DummyStance::Crouch, DummyStance::Jump}) {
        const DummyPostureResult retired =
            posture(GuardMode::Off, stance, DummyTech::HoldGuard, 0);
        const DummyPostureResult off =
            posture(GuardMode::Off, stance, DummyTech::Off, 0);
        QNP_CHECK(retired.valid && off.valid);
        QNP_CHECK(retired.directions == off.directions);
        QNP_CHECK(retired.ownedAxes == off.ownedAxes);
        QNP_CHECK(retired.horizontalSource == DummyPostureSource::None);
        QNP_CHECK(retired.horizontalSource == off.horizontalSource);
        QNP_CHECK(retired.verticalSource == off.verticalSource);
        QNP_CHECK(retired.horizontalSource != DummyPostureSource::Tech);
        QNP_CHECK(retired.verticalSource != DummyPostureSource::Tech);
    }
    result = posture(GuardMode::Off, DummyStance::Crouch,
                     DummyTech::HoldGuard, 0);
    QNP_CHECK(result.directions == down);
    QNP_CHECK(result.ownedAxes == kDummyVerticalAxis);
    QNP_CHECK(result.verticalSource == DummyPostureSource::Stance);

    result = posture(GuardMode::Off, DummyStance::Jump,
                     DummyTech::HoldGuard, 0);
    QNP_CHECK(result.directions == up);
    QNP_CHECK(result.ownedAxes == kDummyVerticalAxis);

    result = posture(GuardMode::AlwaysStand, DummyStance::Jump,
                     DummyTech::HoldGuard, 0);
    QNP_CHECK(result.directions == left);
    QNP_CHECK(result.ownedAxes == both);
    QNP_CHECK(result.horizontalSource == DummyPostureSource::Guard);
    QNP_CHECK(result.verticalSource == DummyPostureSource::Guard);

    result = posture(GuardMode::AlwaysCrouch, DummyStance::Jump,
                     DummyTech::HoldGuard, 0);
    QNP_CHECK(result.directions == (left | down));
    QNP_CHECK(result.ownedAxes == both);
    QNP_CHECK(result.verticalSource == DummyPostureSource::Guard);

    result = posture(GuardMode::AlwaysStand, DummyStance::Crouch,
                     DummyTech::Off, 0);
    QNP_CHECK(result.directions == left);
    QNP_CHECK(result.ownedAxes == both);

    result = posture(GuardMode::Auto, DummyStance::Crouch,
                     DummyTech::Off, 0);
    QNP_CHECK(result.valid);
    QNP_CHECK(result.directions == down);
    QNP_CHECK(result.ownedAxes == kDummyVerticalAxis);

    result = posture(GuardMode::Off, DummyStance::Stand,
                     DummyTech::HoldGuard, 1);
    QNP_CHECK(result.directions == 0u);
    QNP_CHECK(result.ownedAxes == 0u);
    result = posture(GuardMode::AlwaysCrouch, DummyStance::Stand,
                     DummyTech::Off, 1);
    QNP_CHECK(result.directions
              == (qinput::kLogicalRight | qinput::kLogicalDown));
}

QNP_TEST(TrainingDummyGuardModeOwnsEveryAxisItAsserts) {
    const uint32_t downBack =
        qinput::kLogicalLeft | qinput::kLogicalDown;
    for (const DummyStance stance :
         {DummyStance::Stand, DummyStance::Crouch, DummyStance::Jump}) {
        for (const DummyTech tech :
             {DummyTech::Off, DummyTech::HoldGuard}) {
            DummyPostureInput guardFirst{};
            guardFirst.guardMode = GuardMode::AlwaysCrouch;
            guardFirst.guarding = true;
            guardFirst.guardDirections = downBack;
            guardFirst.defenderFacing = 0;
            guardFirst.stance = stance;
            guardFirst.tech = tech;
            DummyPostureInput stanceFirst{};
            stanceFirst.stance = stance;
            stanceFirst.tech = tech;
            stanceFirst.defenderFacing = 0;
            stanceFirst.guardMode = GuardMode::AlwaysCrouch;
            stanceFirst.guarding = true;
            stanceFirst.guardDirections = downBack;

            const DummyPostureResult a = chooseDummyPosture(guardFirst);
            const DummyPostureResult b = chooseDummyPosture(stanceFirst);
            QNP_CHECK(a.valid && b.valid);
            QNP_CHECK(a.directions == b.directions);
            QNP_CHECK(a.ownedAxes == b.ownedAxes);
            QNP_CHECK(a.horizontalSource == b.horizontalSource);
            QNP_CHECK(a.verticalSource == b.verticalSource);
            QNP_CHECK(a.directions == downBack);
            QNP_CHECK(a.ownedAxes == qinput::kLogicalDirectionMask);
            QNP_CHECK(a.verticalSource == DummyPostureSource::Guard);
            QNP_CHECK(a.horizontalSource == DummyPostureSource::Guard);
        }
    }
    QNP_CHECK(posture(GuardMode::Off, DummyStance::Crouch,
                      DummyTech::Off, 0).directions
              == qinput::kLogicalDown);
    QNP_CHECK(posture(GuardMode::Off, DummyStance::Jump,
                      DummyTech::Off, 0).directions
              == qinput::kLogicalUp);
    QNP_CHECK(posture(GuardMode::Off, DummyStance::Stand,
                      DummyTech::Off, 0).ownedAxes == 0u);
}

QNP_TEST(TrainingDummyPostureNeverEmitsButtonsOrUnownedBits) {
    uint32_t observed = 0;
    for (uint32_t mode = 0; mode <= 5u; ++mode) {
        for (uint32_t stanceRaw = 0; stanceRaw <= 2u; ++stanceRaw) {
            for (uint32_t techRaw = 0; techRaw <= 1u; ++techRaw) {
                for (uint8_t facing = 0; facing <= 1u; ++facing) {
                    for (const PostureClass posed :
                             {PostureClass::Standing, PostureClass::Air}) {
                    const DummyPostureResult result = posture(
                        static_cast<GuardMode>(mode),
                        static_cast<DummyStance>(stanceRaw),
                        static_cast<DummyTech>(techRaw), facing, posed);
                    ++observed;
                    QNP_CHECK(result.valid);
                    QNP_CHECK((result.directions
                               & ~qinput::kLogicalDirectionMask) == 0u);
                    QNP_CHECK((result.directions
                               & qinput::kLogicalButtonMask) == 0u);
                    QNP_CHECK((result.directions
                               & ~result.ownedAxes) == 0u);
                    QNP_CHECK(isCanonicalDummyOwnedAxes(
                        result.ownedAxes));
                    }
                }
            }
        }
    }
    QNP_CHECK(observed == 144u);

    QNP_CHECK(isCanonicalDummyOwnedAxes(0u));
    QNP_CHECK(isCanonicalDummyOwnedAxes(kDummyHorizontalAxis));
    QNP_CHECK(isCanonicalDummyOwnedAxes(kDummyVerticalAxis));
    QNP_CHECK(isCanonicalDummyOwnedAxes(
        qinput::kLogicalDirectionMask));
    QNP_CHECK(!isCanonicalDummyOwnedAxes(qinput::kLogicalLeft));
    QNP_CHECK(!isCanonicalDummyOwnedAxes(qinput::kLogicalRight));
    QNP_CHECK(!isCanonicalDummyOwnedAxes(qinput::kLogicalUp));
    QNP_CHECK(!isCanonicalDummyOwnedAxes(qinput::kLogicalDown));
    QNP_CHECK(!isCanonicalDummyOwnedAxes(qinput::kLogicalA));
    QNP_CHECK(!isCanonicalDummyOwnedAxes(
        qinput::kLogicalDirectionMask | qinput::kLogicalA));
}

QNP_TEST(GuardAutoRecordReadsTheRecordAndBlocksTheLanding) {
    const uint32_t away = qinput::kLogicalLeft;
    const uint32_t down = qinput::kLogicalDown;
    const uint32_t up = qinput::kLogicalUp;

    auto base = [&](PostureClass defenderPosture) {
        GuardDecisionInput input{};
        input.mode = GuardMode::AutoRecord;
        input.defenderFacing = 0;
        input.defenderPosture = defenderPosture;
        input.recordEvidenceValid = true;
        return input;
    };

    {
        GuardDecisionInput input = base(PostureClass::Standing);
        input.activeRecordCount = 0;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.valid);
        QNP_CHECK(d.guarding);
        QNP_CHECK(d.logicalDirections == (away | down));
        QNP_CHECK(!d.recordForcedStandGuard);
        QNP_CHECK(!d.recordMixupUnblockable);
    }

    {
        GuardDecisionInput input = base(PostureClass::Standing);
        input.activeRecordCount = 1;
        input.activeLegalPostureMask = kGuardPermitCrouch;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.logicalDirections == (away | down));
        QNP_CHECK(!d.recordForcedStandGuard);
    }

    {
        GuardDecisionInput input = base(PostureClass::Standing);
        input.activeRecordCount = 1;
        input.activeLegalPostureMask = kGuardPermitStand;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.guarding);
        QNP_CHECK(d.logicalDirections == away);
        QNP_CHECK(d.recordForcedStandGuard);
    }

    {
        GuardDecisionInput input = base(PostureClass::Crouching);
        input.activeRecordCount = 1;
        input.activeLegalPostureMask =
            static_cast<uint8_t>(kGuardPermitStand | kGuardPermitCrouch);
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.logicalDirections == (away | down));
        QNP_CHECK(!d.recordForcedStandGuard);
    }

    {
        GuardDecisionInput input = base(PostureClass::Air);
        input.activeRecordCount = 1;
        input.activeLegalPostureMask = kGuardPermitAir;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.guarding);
        QNP_CHECK(d.logicalDirections == away);
        QNP_CHECK((d.logicalDirections & up) == 0u);
        QNP_CHECK((d.logicalDirections & down) == 0u);
    }

    {
        GuardDecisionInput air = base(PostureClass::Air);
        air.activeRecordCount = 1;
        air.activeLegalPostureMask = kGuardPermitAir;
        const GuardDecision inAir = chooseGuardDirections(air);
        QNP_CHECK(inAir.logicalDirections == away);

        GuardDecisionInput landed = base(PostureClass::Standing);
        landed.activeRecordCount = 1;
        landed.activeLegalPostureMask = kGuardPermitCrouch;
        const GuardDecision onGround = chooseGuardDirections(landed);
        QNP_CHECK(onGround.guarding);
        QNP_CHECK(onGround.logicalDirections == (away | down));
        QNP_CHECK((inAir.logicalDirections & away) != 0u);
        QNP_CHECK((onGround.logicalDirections & away) != 0u);
    }

    {
        GuardDecisionInput input = base(PostureClass::Standing);
        input.activeRecordCount = 2;
        input.activeLegalPostureMask = 0u;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.guarding);
        QNP_CHECK(d.recordMixupUnblockable);
        QNP_CHECK(d.logicalDirections == (away | down));
    }

    {
        GuardDecisionInput input = base(PostureClass::Standing);
        input.activeRecordCount = 1;
        input.activeLegalPostureMask = kGuardPermitAir;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.recordMixupUnblockable);
        QNP_CHECK(d.logicalDirections == (away | down));
    }

    {
        GuardDecisionInput input = base(PostureClass::Standing);
        input.recordEvidenceValid = false;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.guarding);
        QNP_CHECK(d.recordEvidenceMissing);
        QNP_CHECK(d.logicalDirections == (away | down));
    }

    {
        GuardDecisionInput input = base(PostureClass::Unreadable);
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(!d.valid);
        QNP_CHECK(!d.guarding);
    }
}

QNP_TEST(GuardAutoRecordAvoidsCapturesByPostureNotByGuarding) {
    const uint32_t away = qinput::kLogicalLeft;
    const uint32_t down = qinput::kLogicalDown;

    auto base = []() {
        GuardDecisionInput input{};
        input.mode = GuardMode::AutoRecord;
        input.defenderFacing = 0;
        input.defenderPosture = PostureClass::Standing;
        input.recordEvidenceValid = true;
        return input;
    };

    {
        GuardDecisionInput input = base();
        input.captureRecordCount = 1;
        input.captureAvoidPostureMask = kGuardPermitStand;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.guarding);
        QNP_CHECK(d.logicalDirections == away);
        QNP_CHECK(d.captureAvoidanceApplied);
    }

    {
        GuardDecisionInput input = base();
        input.captureRecordCount = 1;
        input.captureAvoidPostureMask =
            static_cast<uint8_t>(kGuardPermitStand | kGuardPermitCrouch);
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.logicalDirections == (away | down));
        QNP_CHECK(d.captureAvoidanceApplied);
    }

    {
        GuardDecisionInput input = base();
        input.activeRecordCount = 1;
        input.activeLegalPostureMask =
            static_cast<uint8_t>(kGuardPermitStand | kGuardPermitCrouch);
        input.captureRecordCount = 1;
        input.captureAvoidPostureMask = kGuardPermitStand;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.logicalDirections == away);
        QNP_CHECK(d.recordForcedStandGuard);
        QNP_CHECK(d.captureAvoidanceApplied);
    }

    {
        GuardDecisionInput input = base();
        input.activeRecordCount = 1;
        input.activeLegalPostureMask = kGuardPermitCrouch;
        input.captureRecordCount = 1;
        input.captureAvoidPostureMask = kGuardPermitStand;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.recordMixupUnblockable);
        QNP_CHECK(d.logicalDirections == (away | down));
    }

    {
        GuardDecisionInput input = base();
        input.defenderPosture = PostureClass::Air;
        input.captureRecordCount = 1;
        input.captureAvoidPostureMask = kGuardPermitStand;
        const GuardDecision d = chooseGuardDirections(input);
        QNP_CHECK(d.logicalDirections == away);
    }
}

QNP_TEST(TrainingDummyPostureFailsClosedToContributingNothing) {
    auto failed = [](const DummyPostureResult& result) {
        return !result.valid && result.directions == 0u
            && result.ownedAxes == 0u
            && result.horizontalSource == DummyPostureSource::None
            && result.verticalSource == DummyPostureSource::None;
    };
    DummyPostureInput input{};
    input.defenderFacing = 2;
    QNP_CHECK(failed(chooseDummyPosture(input)));

    input = DummyPostureInput{};
    input.stance = static_cast<DummyStance>(3);
    QNP_CHECK(failed(chooseDummyPosture(input)));

    input = DummyPostureInput{};
    input.tech = static_cast<DummyTech>(2);
    QNP_CHECK(failed(chooseDummyPosture(input)));
    QNP_CHECK(dummyTechValid(DummyTech::Off));
    QNP_CHECK(dummyTechValid(DummyTech::HoldGuard));
    QNP_CHECK(!dummyTechValid(static_cast<DummyTech>(2)));
    QNP_CHECK(!dummyTechValid(static_cast<DummyTech>(99)));
    QNP_CHECK(dummyTechReleaseQualified(DummyTech::Off, false));
    QNP_CHECK(!dummyTechReleaseQualified(DummyTech::HoldGuard, false));
    QNP_CHECK(!dummyTechReleaseQualified(
        static_cast<DummyTech>(2), false));
    QNP_CHECK(dummyTechReleaseQualified(
        static_cast<DummyTech>(2), true));
    QNP_CHECK(dummyStanceValid(DummyStance::Stand));
    QNP_CHECK(dummyStanceValid(DummyStance::Crouch));
    QNP_CHECK(dummyStanceValid(DummyStance::Jump));
    QNP_CHECK(!dummyStanceValid(static_cast<DummyStance>(3)));
    QNP_CHECK(!dummyStanceValid(static_cast<DummyStance>(99)));

    input = DummyPostureInput{};
    input.guarding = true;
    input.guardDirections = qinput::kLogicalLeft | qinput::kLogicalA;
    QNP_CHECK(failed(chooseDummyPosture(input)));

    input = DummyPostureInput{};
    input.guarding = true;
    input.guardDirections = 0;
    QNP_CHECK(failed(chooseDummyPosture(input)));

    input = DummyPostureInput{};
    input.guardMode = static_cast<GuardMode>(99);
    QNP_CHECK(failed(chooseDummyPosture(input)));
}

QNP_TEST(TrainingInputPrecedenceLadderTruthTable) {
    const uint32_t up = qinput::kLogicalUp;
    const uint32_t down = qinput::kLogicalDown;
    const uint32_t left = qinput::kLogicalLeft;
    const uint32_t right = qinput::kLogicalRight;
    const uint32_t a = qinput::kLogicalA;
    const uint32_t b = qinput::kLogicalB;
    const uint32_t c = qinput::kLogicalC;

    std::vector<LadderCase> cases;
    auto add = [&cases](const LadderCase& entry) {
        cases.push_back(entry);
    };

    {
        LadderCase entry = ladderBaseline("lifecycle unavailable");
        entry.lifecycleAvailable = false;
        entry.p1PhysicalMask = right | a;
        entry.p2PhysicalMask = b;
        entry.guardMode = GuardMode::AlwaysCrouch;
        entry.stance = DummyStance::Jump;
        entry.tech = DummyTech::HoldGuard;
        add(entry);
    }
    {
        LadderCase entry = ladderBaseline("Insert menu captured keeps guard");
        entry.menuCaptured = true;
        entry.p1PhysicalMask = right | a;
        entry.p2PhysicalMask = b;
        entry.guardMode = GuardMode::AlwaysCrouch;
        entry.stance = DummyStance::Crouch;
        entry.tech = DummyTech::HoldGuard;
        entry.expectedP2 = left | down;
        add(entry);
    }
    {
        LadderCase entry = ladderBaseline("rearm not Armed keeps stance");
        entry.armed = false;
        entry.p1PhysicalMask = right | a;
        entry.p2PhysicalMask = b;
        entry.stance = DummyStance::Crouch;
        entry.tech = DummyTech::HoldGuard;
        entry.expectedP2 = down;
        add(entry);
    }
    {
        LadderCase entry = ladderBaseline("operation neutral");
        entry.operationNeutral = true;
        entry.p1PhysicalMask = right | a;
        entry.p2PhysicalMask = b;
        entry.guardMode = GuardMode::AlwaysStand;
        entry.stance = DummyStance::Crouch;
        entry.tech = DummyTech::HoldGuard;
        add(entry);
    }

    {
        LadderCase entry = ladderBaseline("macro recording routes P1 to P2");
        entry.macroOverrideP1 = true;
        entry.macroOverrideP2 = true;
        entry.macroP1Mask = 0;
        entry.macroP2Mask = right | a;
        entry.p1PhysicalMask = right | a;
        entry.dummyOwnsDirection = false;
        entry.guardMode = GuardMode::AlwaysCrouch;
        entry.stance = DummyStance::Crouch;
        entry.tech = DummyTech::HoldGuard;
        entry.expectedP1 = 0;
        entry.expectedP2 = right | a;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("macro armed/stopping neutral frame");
        entry.macroOverrideP2 = true;
        entry.macroP2Mask = 0;
        entry.p2PhysicalMask = right | b;
        entry.dummyOwnsDirection = false;
        entry.guardMode = GuardMode::AlwaysCrouch;
        entry.expectedP2 = 0;
        add(entry);
    }

    {
        LadderCase entry =
            ladderBaseline("playback without keep-guarding ignores posture");
        entry.macroOverrideP2 = true;
        entry.macroPlaybackActive = true;
        entry.macroP2Mask = right | b;
        entry.guardDuringPlayback = false;
        entry.p1PhysicalMask = right | a;
        entry.guardMode = GuardMode::AlwaysCrouch;
        entry.expectedP1 = right | a;
        entry.expectedP2 = right | b;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("playback gate also covers stance");
        entry.macroOverrideP2 = true;
        entry.macroPlaybackActive = true;
        entry.macroP2Mask = right | a;
        entry.guardDuringPlayback = false;
        entry.stance = DummyStance::Crouch;
        entry.tech = DummyTech::HoldGuard;
        entry.expectedP2 = right | a;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("playback + tech yields the recorded axis, "
                           "Neutral SOCD");
        entry.macroOverrideP2 = true;
        entry.macroPlaybackActive = true;
        entry.macroP2Mask = right | b;
        entry.guardDuringPlayback = true;
        entry.tech = DummyTech::HoldGuard;
        entry.horizontalSocd = qinput::SocdPolicy::Neutral;
        entry.expectedP2 = right | b;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("playback + tech yields the recorded axis, "
                           "Native SOCD");
        entry.macroOverrideP2 = true;
        entry.macroPlaybackActive = true;
        entry.macroP2Mask = right | b;
        entry.guardDuringPlayback = true;
        entry.tech = DummyTech::HoldGuard;
        entry.horizontalSocd = qinput::SocdPolicy::Native;
        entry.expectedP2 = right | b;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("playback + stand guard fills the free axis");
        entry.macroOverrideP2 = true;
        entry.macroPlaybackActive = true;
        entry.macroP2Mask = b;
        entry.guardDuringPlayback = true;
        entry.guardMode = GuardMode::AlwaysStand;
        entry.horizontalSocd = qinput::SocdPolicy::Neutral;
        entry.expectedP2 = left | b;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("playback + crouch guard, UpPriority SOCD");
        entry.macroOverrideP2 = true;
        entry.macroPlaybackActive = true;
        entry.macroP2Mask = right | up | b;
        entry.guardDuringPlayback = true;
        entry.guardMode = GuardMode::AlwaysCrouch;
        entry.horizontalSocd = qinput::SocdPolicy::UpPriority;
        entry.verticalSocd = qinput::SocdPolicy::UpPriority;
        entry.expectedP2 = right | up | b;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("playback + stand guard keeps a recorded crouch");
        entry.macroOverrideP2 = true;
        entry.macroPlaybackActive = true;
        entry.macroP2Mask = down | c;
        entry.guardDuringPlayback = true;
        entry.guardMode = GuardMode::AlwaysStand;
        entry.expectedP2 = left | down | c;
        add(entry);
    }

    {
        LadderCase entry = ladderBaseline("stance crouch, no guard");
        entry.p1PhysicalMask = right | a;
        entry.p2PhysicalMask = b;
        entry.stance = DummyStance::Crouch;
        entry.expectedP1 = right | a;
        entry.expectedP2 = down | b;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("stance jump, retired tech contributes nothing");
        entry.p2PhysicalMask = b;
        entry.stance = DummyStance::Jump;
        entry.tech = DummyTech::HoldGuard;
        entry.expectedP2 = up | b;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("unowned horizontal axis survives");
        entry.p2PhysicalMask = right | a;
        entry.stance = DummyStance::Crouch;
        entry.expectedP2 = right | down | a;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("stand guard clears a held crouch");
        entry.p2PhysicalMask = down | b;
        entry.guardMode = GuardMode::AlwaysStand;
        entry.stance = DummyStance::Crouch;
        entry.expectedP2 = left | b;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("crouch guard suppresses stance jump");
        entry.p2PhysicalMask = up | right | c;
        entry.guardMode = GuardMode::AlwaysCrouch;
        entry.stance = DummyStance::Jump;
        entry.tech = DummyTech::HoldGuard;
        entry.expectedP2 = left | down | c;
        add(entry);
    }
    {
        LadderCase entry = ladderBaseline("facing 1 flips away");
        entry.facing = 1;
        entry.guardMode = GuardMode::AlwaysStand;
        entry.expectedP2 = right;
        add(entry);
    }
    {
        LadderCase entry = ladderBaseline("inert Auto lets stance through");
        entry.p2PhysicalMask = right | b;
        entry.guardMode = GuardMode::Auto;
        entry.stance = DummyStance::Crouch;
        entry.expectedP2 = right | down | b;
        add(entry);
    }
    {
        LadderCase entry = ladderBaseline("posture never touches P1");
        entry.p1PhysicalMask = left | a;
        entry.p2PhysicalMask = right;
        entry.guardMode = GuardMode::AlwaysStand;
        entry.expectedP1 = left | a;
        entry.expectedP2 = left;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("no posture opinion leaves both lanes alone");
        entry.p1PhysicalMask = right | a;
        entry.p2PhysicalMask = left | down | b;
        entry.expectedP1 = right | a;
        entry.expectedP2 = left | down | b;
        add(entry);
    }
    {
        LadderCase entry =
            ladderBaseline("zeroed pair leaves P2 physical intact");
        entry.p2PhysicalMask = right | a;
        entry.dummyOwnsDirection = false;
        entry.stance = DummyStance::Crouch;
        entry.tech = DummyTech::HoldGuard;
        entry.expectedP2 = right | a;
        add(entry);
    }

    QNP_CHECK(cases.size() == 23u);
    for (const LadderCase& entry : cases) {
        TrainingInput input;
        input.reset();
        uint64_t serial = 0;
        if (entry.armed) {
            armInput(input, serial);
            QNP_CHECK(input.rearmState() == InputRearmState::Armed);
        } else {
            QNP_CHECK(input.rearmState()
                      == InputRearmState::AwaitNeutral);
        }

        const DummyPostureResult resolved =
            posture(entry.guardMode, entry.stance, entry.tech,
                    entry.facing);
        QNP_CHECK(resolved.valid);

        qtraining::TrainingInputStep step{};
        step.lifecycleAvailable = entry.lifecycleAvailable;
        step.menuCaptured = entry.menuCaptured;
        step.operationNeutral = entry.operationNeutral;
        step.p1PhysicalMask = entry.p1PhysicalMask;
        step.p2PhysicalMask = entry.p2PhysicalMask;
        step.macroOverrideP1 = entry.macroOverrideP1;
        step.macroOverrideP2 = entry.macroOverrideP2;
        step.macroPlaybackActive = entry.macroPlaybackActive;
        step.macroP1Mask = entry.macroP1Mask;
        step.macroP2Mask = entry.macroP2Mask;
        step.dummyDirections =
            entry.dummyOwnsDirection ? resolved.directions : 0u;
        step.dummyOwnedAxes =
            entry.dummyOwnsDirection ? resolved.ownedAxes : 0u;
        step.guardDuringPlayback = entry.guardDuringPlayback;
        step.horizontalSocd = entry.horizontalSocd;
        step.verticalSocd = entry.verticalSocd;

        const TrainingInputResult result = input.advance(++serial, step);
        const bool valid = result.valid;
        const bool p1Matches = result.p1Mask == entry.expectedP1;
        const bool p2Matches = result.p2Mask == entry.expectedP2;
        if (!valid || !p1Matches || !p2Matches) {
            std::printf(
                "    [ladder] %s: p1=%08X want %08X, p2=%08X want %08X\n",
                entry.name, result.p1Mask, entry.expectedP1,
                result.p2Mask, entry.expectedP2);
        }
        QNP_CHECK(valid);
        QNP_CHECK(p1Matches);
        QNP_CHECK(p2Matches);
        QNP_CHECK(result.neutralized
                  == (entry.expectedP1 == 0u && entry.expectedP2 == 0u));
        QNP_CHECK(qinput::isCanonicalLogicalFightInput(result.p1Mask));
        QNP_CHECK(qinput::isCanonicalLogicalFightInput(result.p2Mask));
        QNP_CHECK(qinput::logicalFromCompact(
                      qinput::compactFromLogical(result.p1Mask))
                  == result.p1Mask);
        QNP_CHECK(qinput::logicalFromCompact(
                      qinput::compactFromLogical(result.p2Mask))
                  == result.p2Mask);
    }
}

QNP_TEST(TrainingPlaybackKeepGuardingOnlyAddsToTheRecording) {
    const uint32_t buttons = qinput::kLogicalA | qinput::kLogicalC;
    uint32_t observed = 0;
    for (uint8_t facing = 0; facing <= 1; ++facing) {
        for (uint32_t modeRaw = 0; modeRaw <= 3u; ++modeRaw) {
            for (uint32_t stanceRaw = 0; stanceRaw <= 2u; ++stanceRaw) {
                for (uint32_t techRaw = 0; techRaw <= 1u; ++techRaw) {
                    for (uint32_t recorded = 0; recorded <= 15u; ++recorded) {
                        const uint32_t recordedMask =
                            ((recorded & 1u) ? qinput::kLogicalUp : 0u)
                            | ((recorded & 2u) ? qinput::kLogicalDown : 0u)
                            | ((recorded & 4u) ? qinput::kLogicalLeft : 0u)
                            | ((recorded & 8u) ? qinput::kLogicalRight : 0u);
                        const DummyPostureResult resolved = posture(
                            static_cast<GuardMode>(modeRaw),
                            static_cast<DummyStance>(stanceRaw),
                            static_cast<DummyTech>(techRaw), facing);
                        QNP_CHECK(resolved.valid);

                        TrainingInput input;
                        input.reset();
                        uint64_t serial = 0;
                        armInput(input, serial);

                        TrainingInputStep step{};
                        step.lifecycleAvailable = true;
                        step.macroOverrideP2 = true;
                        step.macroPlaybackActive = true;
                        step.macroP2Mask = recordedMask | buttons;
                        step.guardDuringPlayback = true;
                        step.dummyDirections = resolved.directions;
                        step.dummyOwnedAxes = resolved.ownedAxes;
                        step.horizontalSocd = qinput::SocdPolicy::Native;
                        step.verticalSocd = qinput::SocdPolicy::Native;

                        const TrainingInputResult result =
                            input.advance(++serial, step);
                        ++observed;
                        QNP_CHECK(result.valid);
                        QNP_CHECK((result.p2Mask & step.macroP2Mask)
                                  == step.macroP2Mask);
                        QNP_CHECK((result.p2Mask
                                   & ~(step.macroP2Mask
                                       | resolved.directions)) == 0u);
                        const bool recordedHorizontalPair =
                            (recordedMask & kDummyHorizontalAxis)
                                == kDummyHorizontalAxis;
                        const bool recordedVerticalPair =
                            (recordedMask & kDummyVerticalAxis)
                                == kDummyVerticalAxis;
                        if (!recordedHorizontalPair) {
                            QNP_CHECK((result.p2Mask & kDummyHorizontalAxis)
                                      != kDummyHorizontalAxis);
                        }
                        if (!recordedVerticalPair) {
                            QNP_CHECK((result.p2Mask & kDummyVerticalAxis)
                                      != kDummyVerticalAxis);
                        }
                    }
                }
            }
        }
    }
    QNP_CHECK(observed == 2u * 4u * 3u * 2u * 16u);
}

QNP_TEST(TrainingInputRejectsNoncanonicalMasks) {
    TrainingInput input;
    input.reset();
    TrainingInputStep step{};
    step.lifecycleAvailable = true;
    step.p1PhysicalMask = 0x10;
    const TrainingInputResult result = input.advance(1, step);
    QNP_CHECK(!result.valid);
    QNP_CHECK(result.neutralized);

    step.p1PhysicalMask = 0;
    step.horizontalSocd = static_cast<qinput::SocdPolicy>(99);
    const TrainingInputResult badSocd = input.advance(2, step);
    QNP_CHECK(!badSocd.valid);

    TrainingActionPoll poll{};
    poll.lifecycleAvailable = true;
    poll.heldTrainingActions = ~kTrainingActionMask;
    QNP_CHECK(!input.pollActions(3, poll).valid);
    poll.heldTrainingActions = 0;
    poll.p1PhysicalMask = 0x10;
    QNP_CHECK(!input.pollActions(4, poll).valid);
}

QNP_TEST(TrainingInputRejectsMalformedDummyPosturePairs) {
    uint64_t serial = 0;
    auto rejects = [&serial](uint32_t directions, uint32_t ownedAxes) {
        TrainingInput input;
        input.reset();
        uint64_t poll = 0;
        armInput(input, poll);
        TrainingInputStep step{};
        step.lifecycleAvailable = true;
        step.p2PhysicalMask = qinput::kLogicalRight | qinput::kLogicalA;
        step.dummyDirections = directions;
        step.dummyOwnedAxes = ownedAxes;
        const TrainingInputResult result =
            input.advance(++serial, step);
        return !result.valid && result.neutralized
            && result.p1Mask == 0u && result.p2Mask == 0u;
    };

    QNP_CHECK(rejects(qinput::kLogicalA,
                      qinput::kLogicalDirectionMask));
    QNP_CHECK(rejects(qinput::kLogicalLeft | qinput::kLogicalD,
                      qinput::kLogicalDirectionMask));
    QNP_CHECK(rejects(0x100000u, qinput::kLogicalDirectionMask));
    QNP_CHECK(rejects(qinput::kLogicalDown, kDummyHorizontalAxis));
    QNP_CHECK(rejects(qinput::kLogicalLeft, kDummyVerticalAxis));
    QNP_CHECK(rejects(qinput::kLogicalLeft | qinput::kLogicalDown,
                      kDummyVerticalAxis));
    QNP_CHECK(rejects(qinput::kLogicalLeft, qinput::kLogicalLeft));
    QNP_CHECK(rejects(qinput::kLogicalDown, qinput::kLogicalDown));
    QNP_CHECK(rejects(0u, qinput::kLogicalUp));
    QNP_CHECK(rejects(0u, qinput::kLogicalDirectionMask
                              | qinput::kLogicalB));
    TrainingInput owning;
    owning.reset();
    uint64_t poll = 0;
    armInput(owning, poll);
    TrainingInputStep step{};
    step.lifecycleAvailable = true;
    step.p2PhysicalMask = qinput::kLogicalDown | qinput::kLogicalB;
    step.dummyDirections = 0u;
    step.dummyOwnedAxes = kDummyVerticalAxis;
    const TrainingInputResult result = owning.advance(1, step);
    QNP_CHECK(result.valid);
    QNP_CHECK(result.p2Mask == qinput::kLogicalB);
}

static bool pressRecordThroughPlacement(qtraining::TrainingMacros& macros) {
    if (!macros.requestRecordToggle()) return false;
    if (macros.state() != qtraining::MacroState::PreRecord) return true;
    return macros.requestRecordToggle();
}

QNP_TEST(TrainingMacroRecordStopAndOncePlayback) {
    TrainingMacros macros;
    macros.reset();
    QNP_REQUIRE(pressRecordThroughPlacement(macros));
    MacroStepInput input{};
    MacroStepResult result = macros.advance(1, input);
    QNP_CHECK(result.state == MacroState::Recording);
    QNP_CHECK(result.overrideP1 && result.overrideP2);

    input.recordSourceMask =
        qinput::kLogicalRight | qinput::kLogicalA;
    result = macros.advance(2, input);
    QNP_CHECK(result.recordFrameRequested);
    QNP_CHECK(result.p2Mask == input.recordSourceMask);
    QNP_CHECK(macros.selectedFrameCount() == 0);
    MacroStepResult duplicate = macros.advance(2, input);
    QNP_CHECK(duplicate.duplicateStep);
    QNP_CHECK(!duplicate.recordFrameRequested);
    QNP_CHECK(macros.selectedFrameCount() == 0);
    MacroRecordCommitResult commit = macros.commitRecordedFrame(
        2, input.recordSourceMask | qinput::kLogicalDown, 0);
    QNP_CHECK(commit.committed);
    QNP_CHECK(macros.selectedFrameCount() == 1);

    input.recordSourceMask = qinput::kLogicalLeft | qinput::kLogicalB;
    result = macros.advance(3, input);
    QNP_CHECK(result.recordFrameRequested);
    commit = macros.commitRecordedFrame(3, input.recordSourceMask, 1);
    QNP_CHECK(commit.committed);
    QNP_CHECK(macros.selectedFrameCount() == 2);
    QNP_REQUIRE(pressRecordThroughPlacement(macros));
    result = macros.advance(4, input);
    QNP_CHECK(result.overrideP1 && result.overrideP2);
    QNP_CHECK(result.p1Mask == 0 && result.p2Mask == 0);
    QNP_CHECK(result.state == MacroState::Idle);

    QNP_REQUIRE(macros.requestPlaybackToggle());
    input = MacroStepInput{};
    input.playbackFacing = 1;
    result = macros.advance(5, input);
    QNP_CHECK(result.state == MacroState::Playing);
    QNP_CHECK(result.overrideP2 && result.p2Mask == 0);
    result = macros.advance(6, input);
    QNP_CHECK(result.playbackFrame);
    QNP_CHECK(result.p2Mask
              == (qinput::kLogicalLeft | qinput::kLogicalDown
                  | qinput::kLogicalA));
    result = macros.advance(7, input);
    QNP_CHECK(result.p2Mask
              == (qinput::kLogicalLeft | qinput::kLogicalB));
    QNP_CHECK(result.state == MacroState::PlayStopping);
    result = macros.advance(8, input);
    QNP_CHECK(result.overrideP2 && result.p2Mask == 0);
    QNP_CHECK(!result.playbackFrame);
    QNP_CHECK(result.state == MacroState::Idle);
}

QNP_TEST(TrainingMacroGeneratedResponseIsBoundedRelativeAndIsolated) {
    TrainingMacros macros;
    macros.reset();
    QNP_REQUIRE(macros.selectSlot(3u));
    QNP_REQUIRE(macros.setFacingPolicy(FacingPolicy::Absolute));

    const uint32_t sequence[] = {
        0u,
        qinput::kLogicalDown,
        qinput::kLogicalDown | qinput::kLogicalRight,
        qinput::kLogicalRight | qinput::kLogicalA,
        0u,
    };
    constexpr uint32_t sequenceCount =
        static_cast<uint32_t>(sizeof(sequence) / sizeof(sequence[0]));
    QNP_REQUIRE(macros.requestTriggeredSequence(
        sequence, sequenceCount));
    QNP_CHECK(macros.selectedSlot() == 3u);
    QNP_CHECK(macros.selectedFrameCount() == 0u);

    MacroStepInput input{};
    input.playbackFacing = 1u;
    for (uint32_t index = 0u;
         index < sequenceCount; ++index) {
        const MacroStepResult result = macros.advance(index + 1u, input);
        QNP_CHECK(result.overrideP2);
        QNP_CHECK(result.playbackFrame);
        QNP_CHECK(result.generatedPlayback);
        QNP_CHECK(result.p2Mask
                  == TrainingMacros::remapFacingRelative(
                         sequence[index], 0u, input.playbackFacing));
    }
    QNP_CHECK(macros.state() == MacroState::PlayStopping);
    const MacroStepResult release = macros.advance(6u, input);
    QNP_CHECK(release.overrideP2);
    QNP_CHECK(!release.generatedPlayback);
    QNP_CHECK(release.p2Mask == 0u);
    QNP_CHECK(release.state == MacroState::Idle);
    QNP_CHECK(macros.selectedSlot() == 3u);

    QNP_REQUIRE(macros.requestTriggeredSequence(
        sequence, sequenceCount, true));
    const MacroStepResult skippedNeutral = macros.advance(7u, input);
    QNP_CHECK(skippedNeutral.playbackFrame);
    QNP_CHECK(skippedNeutral.p2Mask == qinput::kLogicalDown);
    QNP_REQUIRE(macros.requestStop());
    const MacroStepResult windowClosed = macros.advance(8u, input);
    QNP_CHECK(windowClosed.overrideP2);
    QNP_CHECK(!windowClosed.playbackFrame);
    QNP_CHECK(windowClosed.p2Mask == 0u);
    QNP_CHECK(windowClosed.state == MacroState::Idle);
    const MacroStepResult afterStop = macros.advance(9u, input);
    QNP_CHECK(!afterStop.overrideP2);
    QNP_CHECK(afterStop.p2Mask == 0u);

    QNP_CHECK(!macros.requestTriggeredSequence(nullptr, 1u));
    const uint32_t invalid[] = {0x10u};
    QNP_CHECK(!macros.requestTriggeredSequence(invalid, 1u));
    uint32_t oversized[TrainingMacros::kTriggeredSequenceCapacity + 1u] = {};
    QNP_CHECK(!macros.requestTriggeredSequence(
        oversized, TrainingMacros::kTriggeredSequenceCapacity + 1u));
}

QNP_TEST(BufferedReversalHoldsItsCommitAcrossTwoTicks) {
    TrainingMacros macros;
    macros.reset();
    const uint32_t sequence[] = {
        0u,
        qinput::kLogicalDown,
        qinput::kLogicalDown | qinput::kLogicalA,
        0u,
    };
    QNP_REQUIRE(macros.requestBufferedReversalSequence(sequence, 4u));
    QNP_REQUIRE(macros.bufferedReversalCommitIndex() == 2u);

    MacroStepInput input{};
    input.playbackFacing = 1u;
    uint64_t step = 0u;
    for (uint32_t index = 0u; index < 2u; ++index) {
        const MacroStepResult played = macros.advance(++step, input);
        QNP_CHECK(played.playbackFrame);
        QNP_CHECK(played.p2Mask
                  == TrainingMacros::remapFacingRelative(
                         sequence[index], 0u, input.playbackFacing));
    }
    QNP_REQUIRE(macros.bufferedReversalAwaitingRelease());

    const MacroStepResult withheld = macros.advance(++step, input);
    QNP_CHECK(!withheld.playbackFrame);
    QNP_CHECK(withheld.p2Mask == 0u);

    QNP_REQUIRE(macros.releaseBufferedReversal());
    const uint32_t commitMask = TrainingMacros::remapFacingRelative(
        sequence[2], 0u, input.playbackFacing);

    const MacroStepResult first = macros.advance(++step, input);
    QNP_CHECK(first.playbackFrame);
    QNP_CHECK(first.p2Mask == commitMask);
    QNP_CHECK((first.p2Mask & qinput::kLogicalButtonMask) != 0u);

    const MacroStepResult second = macros.advance(++step, input);
    QNP_CHECK(second.playbackFrame);
    QNP_CHECK(second.p2Mask == commitMask);
    QNP_CHECK((second.p2Mask & qinput::kLogicalButtonMask) != 0u);

    const MacroStepResult tail = macros.advance(++step, input);
    QNP_CHECK(tail.playbackFrame);
    QNP_CHECK((tail.p2Mask & qinput::kLogicalButtonMask) == 0u);
}

QNP_TEST(TrainingMacroBufferedReversalWithholdsTheActivationEdge) {
    TrainingMacros macros;
    macros.reset();
    const uint32_t sequence[] = {
        0u,
        qinput::kLogicalRight,
        qinput::kLogicalDown,
        qinput::kLogicalDown | qinput::kLogicalRight,
        qinput::kLogicalRight | qinput::kLogicalC,
        0u,
    };
    constexpr uint32_t count =
        static_cast<uint32_t>(sizeof(sequence) / sizeof(sequence[0]));
    QNP_REQUIRE(macros.requestBufferedReversalSequence(sequence, count));
    QNP_CHECK(macros.bufferedReversalActive());
    QNP_CHECK(macros.bufferedReversalCommitIndex() == 4u);

    MacroStepInput input{};
    input.playbackFacing = 1u;
    for (uint32_t index = 0u; index < 4u; ++index) {
        const MacroStepResult result = macros.advance(index + 1u, input);
        QNP_CHECK(result.playbackFrame);
        QNP_CHECK(result.generatedPlayback);
        QNP_CHECK(result.p2Mask
                  == TrainingMacros::remapFacingRelative(
                         sequence[index], 0u, input.playbackFacing));
    }
    QNP_CHECK(macros.bufferedReversalAwaitingRelease());
    const MacroStepResult withheld = macros.advance(5u, input);
    QNP_CHECK(withheld.overrideP2);
    QNP_CHECK(!withheld.playbackFrame);
    QNP_CHECK(withheld.generatedPlayback);
    QNP_CHECK(withheld.p2Mask == 0u);
    QNP_CHECK(macros.cursor() == 4u);

    QNP_REQUIRE(macros.releaseBufferedReversal());
    const MacroStepResult activation = macros.advance(6u, input);
    QNP_CHECK(activation.playbackFrame);
    QNP_CHECK((activation.p2Mask & qinput::kLogicalButtonMask)
              == qinput::kLogicalC);
    const MacroStepResult held = macros.advance(7u, input);
    QNP_CHECK(held.playbackFrame);
    QNP_CHECK((held.p2Mask & qinput::kLogicalButtonMask)
              == qinput::kLogicalC);
    const MacroStepResult release = macros.advance(8u, input);
    QNP_CHECK(release.playbackFrame);
    QNP_CHECK(release.p2Mask == 0u);
    QNP_CHECK(release.state == MacroState::PlayStopping);
    const MacroStepResult stopped = macros.advance(9u, input);
    QNP_CHECK(stopped.state == MacroState::Idle);
    QNP_CHECK(!macros.bufferedReversalActive());

    QNP_CHECK(!macros.requestBufferedReversalSequence(nullptr, count));
    const uint32_t noRelease[] = {0u, qinput::kLogicalA};
    QNP_CHECK(!macros.requestBufferedReversalSequence(noRelease, 2u));
    const uint32_t allNeutral[] = {0u, 0u};
    QNP_CHECK(!macros.requestBufferedReversalSequence(allNeutral, 2u));
}

QNP_TEST(TrainingMacroBufferedReversalAcceptsADirectionHoldTail) {
    TrainingMacros macros;
    macros.reset();
    const uint32_t sequence[] = {
        0u,
        qinput::kLogicalDown,
        qinput::kLogicalDown | qinput::kLogicalLeft,
        qinput::kLogicalLeft | qinput::kLogicalA,
        qinput::kLogicalLeft,
        0u,
    };
    QNP_REQUIRE(macros.requestBufferedReversalSequence(sequence, 6u));
    QNP_CHECK(macros.bufferedReversalCommitIndex() == 3u);

    MacroStepInput input{};
    input.playbackFacing = 0u;
    for (uint32_t index = 0u; index < 3u; ++index) {
        const MacroStepResult result = macros.advance(index + 1u, input);
        QNP_CHECK(result.playbackFrame);
        QNP_CHECK(result.p2Mask == sequence[index]);
    }
    QNP_CHECK(macros.bufferedReversalAwaitingRelease());
    const MacroStepResult withheld = macros.advance(4u, input);
    QNP_CHECK(!withheld.playbackFrame);
    QNP_CHECK(withheld.p2Mask == 0u);

    QNP_REQUIRE(macros.releaseBufferedReversal());
    const MacroStepResult activation = macros.advance(5u, input);
    QNP_CHECK(activation.playbackFrame);
    QNP_CHECK(activation.p2Mask
              == (qinput::kLogicalLeft | qinput::kLogicalA));
    const MacroStepResult commitHeld = macros.advance(6u, input);
    QNP_CHECK(commitHeld.playbackFrame);
    QNP_CHECK(commitHeld.p2Mask
              == (qinput::kLogicalLeft | qinput::kLogicalA));
    const MacroStepResult hold = macros.advance(7u, input);
    QNP_CHECK(hold.playbackFrame);
    QNP_CHECK(hold.p2Mask == qinput::kLogicalLeft);
    const MacroStepResult release = macros.advance(8u, input);
    QNP_CHECK(release.p2Mask == 0u);
    QNP_CHECK(release.state == MacroState::PlayStopping);

    macros.reset();
    const uint32_t twoPresses[] = {
        0u, qinput::kLogicalDown | qinput::kLogicalA, qinput::kLogicalDown,
        qinput::kLogicalA, 0u };
    QNP_CHECK(macros.requestBufferedReversalSequence(twoPresses, 5u));
    QNP_CHECK(macros.bufferedReversalCommitIndex() == 3u);
}

QNP_TEST(TrainingMacroInvalidStopStillOwnsNeutral) {
    TrainingMacros macros;
    macros.reset();
    QNP_REQUIRE(pressRecordThroughPlacement(macros));
    MacroStepInput input{};
    macros.advance(1, input);
    QNP_REQUIRE(macros.requestStop());
    input.recordSourceMask = 0x10;
    MacroStepResult result = macros.advance(2, input);
    QNP_CHECK(!result.valid);
    QNP_CHECK(result.overrideP1 && result.overrideP2);
    QNP_CHECK(result.p1Mask == 0 && result.p2Mask == 0);

    macros.reset();
    QNP_REQUIRE(pressRecordThroughPlacement(macros));
    input = MacroStepInput{};
    macros.advance(1, input);
    input.recordSourceMask = qinput::kLogicalA;
    MacroStepResult record = macros.advance(2, input);
    QNP_REQUIRE(record.recordFrameRequested);
    QNP_REQUIRE(macros.commitRecordedFrame(
        2, qinput::kLogicalA, 0).committed);
    QNP_REQUIRE(macros.requestStop());
    macros.advance(3, input);
    QNP_REQUIRE(macros.requestPlaybackToggle());
    macros.advance(4, input);
    input.playbackFacing = 99;
    QNP_REQUIRE(macros.requestStop());
    result = macros.advance(5, input);
    QNP_CHECK(!result.valid);
    QNP_CHECK(result.overrideP2);
    QNP_CHECK(result.p2Mask == 0);
}

QNP_TEST(TrainingMacroRecordStartsWithADirectionAlreadyHeld) {
    TrainingMacros macros;
    macros.reset();
    QNP_REQUIRE(pressRecordThroughPlacement(macros));

    MacroStepInput input{};
    input.recordSourceMask = qinput::kLogicalLeft | qinput::kLogicalA;
    MacroStepResult result = macros.advance(1, input);
    QNP_CHECK(result.state == MacroState::Recording);
    QNP_CHECK(result.overrideP1 && result.overrideP2);
    QNP_CHECK(result.p2Mask == 0);
    QNP_CHECK(!result.recordFrameRequested);

    result = macros.advance(2, input);
    QNP_CHECK(result.recordFrameRequested);
    QNP_CHECK(result.p2Mask == input.recordSourceMask);
    QNP_CHECK(macros.commitRecordedFrame(2, input.recordSourceMask, 0)
                  .committed);
    QNP_CHECK(macros.selectedFrameCount() == 1u);

    QNP_REQUIRE(pressRecordThroughPlacement(macros));
    QNP_REQUIRE(macros.advance(3, input).state == MacroState::Idle);
    QNP_REQUIRE(macros.requestPlaybackToggle());
    result = macros.advance(4, input);
    QNP_CHECK(result.state == MacroState::Playing);
    QNP_CHECK(result.p2Mask == 0);
    result = macros.advance(5, input);
    QNP_CHECK(result.playbackFrame);
    QNP_CHECK(result.p2Mask == (qinput::kLogicalLeft | qinput::kLogicalA));
}

QNP_TEST(TrainingMacroCaptureAndRearmPauseAllCursors) {
    TrainingMacros macros;
    macros.reset();
    QNP_REQUIRE(pressRecordThroughPlacement(macros));

    MacroStepInput input{};
    input.paused = true;
    MacroStepResult result = macros.advance(1, input);
    QNP_CHECK(result.state == MacroState::RecordArmed);
    QNP_CHECK(result.overrideP1 && result.overrideP2);
    QNP_CHECK(!result.recordFrameRequested);

    input.paused = false;
    result = macros.advance(2, input);
    QNP_REQUIRE(result.state == MacroState::Recording);
    const uint32_t recordedMask =
        qinput::kLogicalRight | qinput::kLogicalA;
    input.recordSourceMask = recordedMask;
    input.paused = true;
    result = macros.advance(3, input);
    QNP_CHECK(result.state == MacroState::Recording);
    QNP_CHECK(result.overrideP1 && result.overrideP2);
    QNP_CHECK(!result.recordFrameRequested);
    QNP_CHECK(macros.selectedFrameCount() == 0);
    QNP_CHECK(macros.cursor() == 0);

    input.paused = false;
    result = macros.advance(4, input);
    QNP_REQUIRE(result.recordFrameRequested);
    QNP_REQUIRE(macros.commitRecordedFrame(
        4, input.recordSourceMask, 0).committed);
    QNP_CHECK(macros.selectedFrameCount() == 1);
    QNP_REQUIRE(macros.requestStop());
    result = macros.advance(5, input);
    QNP_REQUIRE(result.state == MacroState::Idle);

    QNP_REQUIRE(macros.requestPlaybackToggle());
    input = MacroStepInput{};
    result = macros.advance(6, input);
    QNP_REQUIRE(result.state == MacroState::Playing);
    QNP_CHECK(macros.cursor() == 0);
    input.paused = true;
    result = macros.advance(7, input);
    QNP_CHECK(result.state == MacroState::Playing);
    QNP_CHECK(result.overrideP2);
    QNP_CHECK(!result.playbackFrame);
    QNP_CHECK(macros.cursor() == 0);
    input.paused = false;
    result = macros.advance(8, input);
    QNP_CHECK(result.playbackFrame);
    QNP_CHECK(result.p2Mask == recordedMask);
    QNP_CHECK(macros.cursor() == 1);
}

QNP_TEST(TrainingMacroLoopAndCapacity) {
    TrainingMacros macros;
    macros.reset();
    QNP_REQUIRE(pressRecordThroughPlacement(macros));
    MacroStepInput input{};
    macros.advance(1, input);
    input.recordSourceMask = qinput::kLogicalA;
    uint64_t serial = 2;
    MacroStepResult result{};
    for (uint32_t i = 0; i < kTrainingMacroFrameCapacity; ++i) {
        result = macros.advance(serial++, input);
        QNP_CHECK(result.valid);
        QNP_CHECK(result.recordFrameRequested);
        const MacroRecordCommitResult commit =
            macros.commitRecordedFrame(serial - 1,
                                       input.recordSourceMask, 0);
        QNP_CHECK(commit.committed);
        result.capacityReached = commit.capacityReached;
        result.state = commit.state;
    }
    QNP_CHECK(result.capacityReached);
    QNP_CHECK(result.state == MacroState::RecordStopping);
    QNP_CHECK(macros.selectedFrameCount()
              == kTrainingMacroFrameCapacity);
    result = macros.advance(serial++, input);
    QNP_CHECK(result.state == MacroState::Idle);

    macros.setPlaybackMode(PlaybackMode::Loop);
    QNP_REQUIRE(macros.requestPlaybackToggle());
    macros.advance(serial++, input);
    for (uint32_t i = 0; i < kTrainingMacroFrameCapacity + 1; ++i)
        result = macros.advance(serial++, input);
    QNP_CHECK(result.state == MacroState::Playing);
    QNP_CHECK(result.p2Mask == qinput::kLogicalA);
}

QNP_TEST(TrainingMacroFacingConversion) {
    const uint32_t buttons = qinput::kLogicalA | qinput::kLogicalD;
    QNP_CHECK(TrainingMacros::remapFacingRelative(
                  buttons | qinput::kLogicalRight, 0, 1)
              == (buttons | qinput::kLogicalLeft));
    QNP_CHECK(TrainingMacros::remapFacingRelative(
                  buttons | qinput::kLogicalLeft, 1, 0)
              == (buttons | qinput::kLogicalRight));
    QNP_CHECK(TrainingMacros::remapFacingRelative(
                  qinput::kLogicalLeft | qinput::kLogicalRight, 0, 1)
              == (qinput::kLogicalLeft | qinput::kLogicalRight));
    QNP_CHECK(TrainingMacros::remapFacingRelative(0x10, 0, 1) == 0);
    TrainingMacros macros;
    macros.reset();
    QNP_CHECK(!macros.setPlaybackMode(
        static_cast<PlaybackMode>(99)));
    QNP_CHECK(!macros.setFacingPolicy(
        static_cast<FacingPolicy>(99)));
}

QNP_TEST(TrainingGuardClassificationAndDirections) {
    QNP_CHECK(guardModeReleaseQualified(GuardMode::Off, false));
    QNP_CHECK(guardModeReleaseQualified(
        GuardMode::AlwaysStand, false));
    QNP_CHECK(guardModeReleaseQualified(
        GuardMode::AlwaysCrouch, false));
    QNP_CHECK(!guardModeReleaseQualified(GuardMode::Auto, false));
    QNP_CHECK(guardModeReleaseQualified(GuardMode::Auto, true));
    QNP_CHECK(!guardModeReleaseQualified(
        static_cast<GuardMode>(99), true));

    QNP_CHECK(classifyGuardPermission(0)
              == GuardClassification::Unguardable);
    QNP_CHECK(classifyGuardPermission(kGuardPermitStand)
              == GuardClassification::Stand);
    QNP_CHECK(classifyGuardPermission(kGuardPermitCrouch)
              == GuardClassification::Crouch);
    QNP_CHECK(classifyGuardPermission(
                  kGuardPermitStand | kGuardPermitCrouch)
              == GuardClassification::Either);
    QNP_CHECK(classifyGuardPermission(kGuardPermitAir)
              == GuardClassification::Air);
    QNP_CHECK(classifyGuardPermission(0x80)
              == GuardClassification::Invalid);

    GuardDecisionInput input{};
    input.mode = GuardMode::Auto;
    input.threatPresent = true;
    input.autoPredictionQualified = true;
    input.permission = kGuardPermitStand;
    input.defenderFacing = 0;
    GuardDecision decision = chooseGuardDirections(input);
    QNP_CHECK(decision.guarding);
    QNP_CHECK(decision.logicalDirections == qinput::kLogicalLeft);

    input.permission = kGuardPermitCrouch;
    input.defenderFacing = 1;
    decision = chooseGuardDirections(input);
    QNP_CHECK(decision.logicalDirections
              == (qinput::kLogicalRight | qinput::kLogicalDown));

    input.defenderAirborne = true;
    decision = chooseGuardDirections(input);
    QNP_CHECK(!decision.guarding);
    input.permission = kGuardPermitAir;
    decision = chooseGuardDirections(input);
    QNP_CHECK(decision.logicalDirections == qinput::kLogicalRight);

    input.autoPredictionQualified = false;
    decision = chooseGuardDirections(input);
    QNP_CHECK(!decision.guarding);
    input.mode = static_cast<GuardMode>(99);
    decision = chooseGuardDirections(input);
    QNP_CHECK(!decision.valid);
}

QNP_TEST(TrainingLifeMaxPlanClampsAndFailsClosed) {
    QNP_CHECK(planLifeMax(0).valid);
    QNP_CHECK(planLifeMax(0).maxLife == kLifeUnitsPerLevel);
    QNP_CHECK(planLifeMax(0).maxLife == 9200);
    QNP_CHECK(planLifeMax(1).maxLife == 18400);
    QNP_CHECK(planLifeMax(kLifeLevelClamp).valid);
    QNP_CHECK(planLifeMax(kLifeLevelClamp).maxLife
              == kLifeUnitsPerLevel * (kLifeLevelClamp + 1));

    QNP_CHECK(!planLifeMax(-1).valid);
    QNP_CHECK(planLifeMax(-1).maxLife == 0);
    QNP_CHECK(!planLifeMax(kLifeLevelClamp + 1).valid);
    QNP_CHECK(planLifeMax(kLifeLevelClamp + 1).maxLife == 0);
    QNP_CHECK(!planLifeMax(INT32_MAX).valid);
    QNP_CHECK(!planLifeMax(INT64_MAX).valid);
    QNP_CHECK(!planLifeMax(INT64_MIN).valid);
    for (int64_t level = 0; level <= kLifeLevelClamp; ++level) {
        const LifeMaxPlan plan = planLifeMax(level);
        QNP_CHECK(plan.valid);
        QNP_CHECK(refillFitsInt32(plan.maxLife));
        QNP_CHECK(plan.maxLife >= kLifeUnitsPerLevel);
    }
}

QNP_TEST(TrainingLifeRefillPlanWritesTheFixedPointOrNothing) {
    const int64_t maxLife = 9200;
    auto corrupt = [](const LifeRefillPlan& plan) {
        return plan.verdict == RefillVerdict::Corrupt
            && plan.current == 0 && plan.pendingDamage == 0
            && plan.reserve == 0 && plan.settleTimer == 0;
    };
    auto full = [maxLife](const LifeRefillPlan& plan) {
        return plan.verdict == RefillVerdict::Apply
            && plan.current == static_cast<int32_t>(maxLife)
            && plan.pendingDamage == 0
            && plan.reserve == 0 && plan.settleTimer == 0;
    };

    LifeRecordImage image{};
    QNP_CHECK(full(planLifeRefill(image, maxLife)));

    image = LifeRecordImage{};
    image.current = maxLife;
    const LifeRefillPlan once = planLifeRefill(image, maxLife);
    QNP_CHECK(full(once));
    LifeRecordImage settled{};
    settled.current = once.current;
    settled.pendingDamage = once.pendingDamage;
    settled.reserve = once.reserve;
    settled.settleTimer = once.settleTimer;
    const LifeRefillPlan twice = planLifeRefill(settled, maxLife);
    QNP_CHECK(full(twice));
    QNP_CHECK(twice.current == once.current);
    QNP_CHECK(twice.pendingDamage == once.pendingDamage);
    QNP_CHECK(twice.reserve == once.reserve);
    QNP_CHECK(twice.settleTimer == once.settleTimer);

    image = LifeRecordImage{};
    image.current = maxLife + 1;
    QNP_CHECK(corrupt(planLifeRefill(image, maxLife)));

    image = LifeRecordImage{};
    image.current = 5000;
    image.pendingDamage = 1200;
    image.settleTimer = 40;
    QNP_CHECK(full(planLifeRefill(image, maxLife)));

    image = LifeRecordImage{};
    image.current = 3800;
    QNP_CHECK(full(planLifeRefill(image, maxLife)));

    image = LifeRecordImage{};
    image.current = 100;
    image.reserve = 9100;
    QNP_CHECK(full(planLifeRefill(image, maxLife)));

    for (int component = 0; component < 4; ++component) {
        LifeRecordImage negative{};
        negative.current = 1000;
        if (component == 0) negative.current = -1;
        if (component == 1) negative.pendingDamage = -1;
        if (component == 2) negative.reserve = -1;
        if (component == 3) negative.settleTimer = -1;
        QNP_CHECK(corrupt(planLifeRefill(negative, maxLife)));
    }

    image = LifeRecordImage{};
    image.current = 100;
    image.pendingDamage = 500;
    QNP_CHECK(corrupt(planLifeRefill(image, maxLife)));
    image.reserve = 400;
    QNP_CHECK(full(planLifeRefill(image, maxLife)));

    image = LifeRecordImage{};
    image.current = 2000000000;
    image.reserve = 2000000000;
    QNP_CHECK(corrupt(planLifeRefill(image, 2100000000)));

    image = LifeRecordImage{};
    image.reserve = INT32_MAX;
    QNP_CHECK(corrupt(planLifeRefill(image, maxLife)));

    image = LifeRecordImage{};
    image.current = static_cast<int64_t>(INT32_MAX) + 1;
    QNP_CHECK(corrupt(planLifeRefill(image, maxLife)));
    image = LifeRecordImage{};
    image.settleTimer = static_cast<int64_t>(INT32_MAX) + 1;
    QNP_CHECK(corrupt(planLifeRefill(image, maxLife)));

    QNP_CHECK(corrupt(planLifeRefill(LifeRecordImage{}, 0)));
    QNP_CHECK(corrupt(planLifeRefill(LifeRecordImage{}, 9199)));
    QNP_CHECK(corrupt(planLifeRefill(LifeRecordImage{}, -1)));
    QNP_CHECK(corrupt(planLifeRefill(
        LifeRecordImage{}, static_cast<int64_t>(INT32_MAX) + 1)));
    QNP_CHECK(corrupt(planLifeRefill(LifeRecordImage{}, INT64_MAX)));

    for (int64_t current = -2; current <= 3; ++current) {
        for (int64_t pending = -2; pending <= 3; ++pending) {
            for (int64_t reserve = -2; reserve <= 3; ++reserve) {
                LifeRecordImage sweep{};
                sweep.current = current;
                sweep.pendingDamage = pending;
                sweep.reserve = reserve;
                const LifeRefillPlan plan =
                    planLifeRefill(sweep, maxLife);
                QNP_CHECK(plan.verdict != RefillVerdict::Busy);
                if (plan.verdict == RefillVerdict::Apply) {
                    QNP_CHECK(plan.current
                              == static_cast<int32_t>(maxLife));
                    QNP_CHECK(plan.pendingDamage == 0);
                    QNP_CHECK(plan.reserve == 0);
                    QNP_CHECK(plan.settleTimer == 0);
                } else {
                    QNP_CHECK(corrupt(plan));
                }
            }
        }
    }
}

QNP_TEST(TrainingGuardGaugeRefillClampsToTheStableRange) {
    QNP_CHECK(kGuardGaugeFull == 100);
    for (int64_t observed = 0; observed <= kGuardGaugeFull; ++observed) {
        const GuardGaugeRefillPlan plan =
            planGuardGaugeRefill(observed);
        QNP_CHECK(plan.verdict == RefillVerdict::Apply);
        QNP_CHECK(plan.writeValue == kGuardGaugeFull);
    }
    const int64_t outside[] = {
        -1, -100, kGuardGaugeFull + 1, 101, 1000,
        INT32_MIN, INT32_MAX, INT64_MIN, INT64_MAX,
    };
    for (const int64_t observed : outside) {
        const GuardGaugeRefillPlan plan =
            planGuardGaugeRefill(observed);
        QNP_CHECK(plan.verdict == RefillVerdict::Corrupt);
        QNP_CHECK(plan.writeValue == 0);
    }
    for (int64_t observed = -3; observed <= 103; ++observed) {
        QNP_CHECK(planGuardGaugeRefill(observed).verdict
                  != RefillVerdict::Busy);
    }
}

QNP_TEST(TrainingExRefillRefusesWhileAnActivationIsInFlight) {
    const int64_t idle[kExPendingLaneCount] = {0, 0, 0, 0};
    QNP_CHECK(kExStocksFull == 9);
    QNP_CHECK(kExFractionMax == 53);
    QNP_CHECK(kExFractionFull == 0);
    QNP_CHECK(kExPendingLaneCount == 4u);
    QNP_CHECK(kExSpendStateSingle == -1);
    QNP_CHECK(kExSpendStateMulti == -2);

    auto applies = [](const ExRefillPlan& plan) {
        return plan.verdict == RefillVerdict::Apply
            && plan.stocks == kExStocksFull
            && plan.fraction == kExFractionFull;
    };
    auto refuses = [](const ExRefillPlan& plan, RefillVerdict verdict) {
        return plan.verdict == verdict
            && plan.stocks == 0 && plan.fraction == 0;
    };

    for (int64_t stocks = 0; stocks <= kExStocksFull; ++stocks) {
        for (int64_t fraction = 0; fraction <= kExFractionMax;
             ++fraction) {
            QNP_CHECK(applies(
                planExRefill(0, stocks, fraction, idle)));
        }
    }
    QNP_CHECK(applies(planExRefill(0, kExStocksFull, 0, idle)));

    QNP_CHECK(applies(planExRefill(1, 4, 20, idle)));
    QNP_CHECK(applies(planExRefill(2, 4, 20, idle)));

    QNP_CHECK(refuses(planExRefill(kExSpendStateSingle, 4, 20, idle),
                      RefillVerdict::Busy));
    QNP_CHECK(refuses(planExRefill(kExSpendStateMulti, 4, 20, idle),
                      RefillVerdict::Busy));
    for (uint32_t lane = 0; lane < kExPendingLaneCount; ++lane) {
        for (const int64_t reserved : {-1, -3, -9, 1, 5}) {
            int64_t lanes[kExPendingLaneCount] = {0, 0, 0, 0};
            lanes[lane] = reserved;
            QNP_CHECK(refuses(planExRefill(0, 4, 20, lanes),
                              RefillVerdict::Busy));
        }
    }

    QNP_CHECK(refuses(planExRefill(0, kExStocksFull + 1, 0, idle),
                      RefillVerdict::Corrupt));
    QNP_CHECK(refuses(planExRefill(0, -1, 0, idle),
                      RefillVerdict::Corrupt));
    QNP_CHECK(refuses(planExRefill(0, 4, kExFractionMax + 1, idle),
                      RefillVerdict::Corrupt));
    QNP_CHECK(refuses(planExRefill(0, 4, -1, idle),
                      RefillVerdict::Corrupt));
    QNP_CHECK(refuses(planExRefill(0, INT64_MAX, 0, idle),
                      RefillVerdict::Corrupt));
    QNP_CHECK(refuses(planExRefill(0, INT64_MIN, 0, idle),
                      RefillVerdict::Corrupt));
    QNP_CHECK(refuses(planExRefill(0, 4, INT64_MAX, idle),
                      RefillVerdict::Corrupt));

    QNP_CHECK(refuses(
        planExRefill(kExSpendStateSingle, kExStocksFull + 1, 0, idle),
        RefillVerdict::Busy));
    QNP_CHECK(refuses(
        planExRefill(kExSpendStateMulti, -5, kExFractionMax + 9, idle),
        RefillVerdict::Busy));
    {
        int64_t lanes[kExPendingLaneCount] = {0, 0, -3, 0};
        QNP_CHECK(refuses(planExRefill(0, 99, 99, lanes),
                          RefillVerdict::Busy));
    }

    for (int64_t state = -4; state <= 4; ++state) {
        for (int64_t stocks = -1; stocks <= kExStocksFull + 1; ++stocks) {
            for (int64_t fraction = -1;
                 fraction <= kExFractionMax + 1; ++fraction) {
                const ExRefillPlan plan =
                    planExRefill(state, stocks, fraction, idle);
                if (plan.verdict == RefillVerdict::Apply) {
                    QNP_CHECK(plan.stocks == kExStocksFull);
                    QNP_CHECK(plan.fraction == kExFractionFull);
                } else {
                    QNP_CHECK(plan.stocks == 0);
                    QNP_CHECK(plan.fraction == 0);
                }
                const bool inFlight =
                    state == kExSpendStateSingle
                    || state == kExSpendStateMulti;
                QNP_CHECK((plan.verdict == RefillVerdict::Busy)
                          == inFlight);
            }
        }
    }
}

QNP_TEST(TrainingExResetPreservesOrRestoresVisibleMeterWithoutABusyState) {
    const ExResetPlan preserved = planExReset(3, 27, false);
    QNP_REQUIRE(preserved.verdict == RefillVerdict::Apply);
    QNP_CHECK(preserved.stocks == 3);
    QNP_CHECK(preserved.fraction == 27);

    const ExResetPlan full = planExReset(3, 27, true);
    QNP_REQUIRE(full.verdict == RefillVerdict::Apply);
    QNP_CHECK(full.stocks == kExStocksFull);
    QNP_CHECK(full.fraction == kExFractionFull);

    ExRestoreTarget pinned{};
    pinned.pinned = true;
    pinned.stocks = 2;
    pinned.fraction = 19;
    const ExResetPlan restored = planExReset(8, 40, true, pinned);
    QNP_REQUIRE(restored.verdict == RefillVerdict::Apply);
    QNP_CHECK(restored.stocks == 2);
    QNP_CHECK(restored.fraction == 19);

    pinned.stocks = kExStocksFull + 1;
    const ExResetPlan badPin = planExReset(4, 20, true, pinned);
    QNP_REQUIRE(badPin.verdict == RefillVerdict::Apply);
    QNP_CHECK(badPin.stocks == kExStocksFull);
    QNP_CHECK(badPin.fraction == kExFractionFull);

    pinned.stocks = kExStocksFull;
    pinned.fraction = 1;
    QNP_CHECK(!exRestoreTargetValid(pinned));
    const ExResetPlan noncanonicalPin =
        planExReset(4, 20, true, pinned);
    QNP_REQUIRE(noncanonicalPin.verdict == RefillVerdict::Apply);
    QNP_CHECK(noncanonicalPin.stocks == kExStocksFull);
    QNP_CHECK(noncanonicalPin.fraction == kExFractionFull);

    TrainingCommandV1 target{};
    target.commandKind = static_cast<uint32_t>(
        TrainingCommandKind::SetExRestoreTarget);
    target.slotOrSetting = 1u;
    target.value = (static_cast<uint32_t>(kExStocksFull) << 16u) | 1u;
    QNP_CHECK(!validCommandPayload(target));
    target.value = static_cast<uint32_t>(kExStocksFull) << 16u;
    QNP_CHECK(validCommandPayload(target));

    QNP_CHECK(planExReset(-1, 0, false).verdict
              == RefillVerdict::Corrupt);
    QNP_CHECK(planExReset(kExStocksFull + 1, 0, false).verdict
              == RefillVerdict::Corrupt);
    QNP_CHECK(planExReset(0, -1, false).verdict
              == RefillVerdict::Corrupt);
    QNP_CHECK(planExReset(0, kExFractionMax + 1, false).verdict
              == RefillVerdict::Corrupt);
}

QNP_TEST(TrainingGuardTelemetryMatchesMissesAndDuplicates) {
    GuardPredictionTelemetry telemetry;
    telemetry.reset();
    QNP_REQUIRE(telemetry.recordPrediction(
        10, 100, kGuardPermitStand, qinput::kLogicalLeft));
    QNP_CHECK(telemetry.recordExactContact(
                  10, 100, kGuardPermitStand,
                  qinput::kLogicalLeft, true)
              == GuardContactResult::Match);
    QNP_CHECK(telemetry.recordExactContact(
                  10, 100, kGuardPermitStand,
                  qinput::kLogicalLeft, true)
              == GuardContactResult::Duplicate);

    QNP_REQUIRE(telemetry.recordPrediction(
        11, 101, kGuardPermitCrouch,
        qinput::kLogicalLeft | qinput::kLogicalDown));
    QNP_CHECK(telemetry.recordExactContact(
                  11, 101, kGuardPermitStand,
                  qinput::kLogicalLeft | qinput::kLogicalDown, true)
              == GuardContactResult::PermissionMismatch);
    QNP_CHECK(telemetry.recordExactContact(
                  12, 102, kGuardPermitStand,
                  qinput::kLogicalLeft, true)
              == GuardContactResult::MissingPrediction);
    QNP_REQUIRE(telemetry.recordPrediction(
        13, 103, kGuardPermitStand, qinput::kLogicalLeft));
    QNP_CHECK(telemetry.recordExactContact(
                  13, 103, kGuardPermitStand,
                  qinput::kLogicalRight, true)
              == GuardContactResult::DirectionMismatch);
    const GuardTelemetryCounters& counters = telemetry.counters();
    QNP_CHECK(counters.predictions == 3);
    QNP_CHECK(counters.contacts == 5);
    QNP_CHECK(counters.matches == 1);
    QNP_CHECK(counters.duplicateContacts == 1);
    QNP_CHECK(counters.permissionMismatches == 1);
    QNP_CHECK(counters.directionMismatches == 1);
    QNP_CHECK(counters.missingPredictions == 1);
    QNP_CHECK(counters.firstActiveMisses == 3);
}

QNP_TEST(TrainingResetModifierTruthTable) {
    for (uint32_t bits = 0; bits < 16; ++bits) {
        uint32_t logical = 0;
        if (bits & 1) logical |= qinput::kLogicalUp;
        if (bits & 2) logical |= qinput::kLogicalDown;
        if (bits & 4) logical |= qinput::kLogicalLeft;
        if (bits & 8) logical |= qinput::kLogicalRight;
        const ResetModifierResult result =
            positionPresetFromResetModifier(logical);
        const bool shouldBeValid =
            bits == 0 || bits == 1 || bits == 2 || bits == 4 || bits == 8;
        QNP_CHECK(result.valid == shouldBeValid);
    }
    QNP_CHECK(!positionPresetFromResetModifier(qinput::kLogicalA).valid);
}

QNP_TEST(TrainingPositionPlansAsymmetricRectangles) {
    PositionMathConfig config{};
    config.worldRight = 400;
    config.cameraMax = 336;
    config.nativeP1X = 80;
    config.nativeP2X = 208;
    config.nativeCameraX = 112;
    config.p1Bounds = FighterHorizontalBounds{-20, 30};
    config.p2Bounds = FighterHorizontalBounds{-15, 25};

    PositionPlan round =
        planPositionPreset(PositionPreset::RoundStart, config);
    QNP_REQUIRE(round.valid);
    QNP_CHECK(round.p1X == 80 && round.p2X == 208);
    QNP_CHECK(round.cameraX == 112);

    PositionPlan left =
        planPositionPreset(PositionPreset::LeftCorner, config);
    QNP_REQUIRE(left.valid);
    QNP_CHECK(left.p2X + config.p2Bounds.left == config.worldLeft);
    QNP_CHECK(left.p2Facing == 0 && left.p1Facing == 1);

    PositionPlan right =
        planPositionPreset(PositionPreset::RightCorner, config);
    QNP_REQUIRE(right.valid);
    QNP_CHECK(right.p2X + config.p2Bounds.right == config.worldRight);
    QNP_CHECK(right.p1Facing == 0 && right.p2Facing == 1);

    PositionPlan center =
        planPositionPreset(PositionPreset::Center, config);
    QNP_REQUIRE(center.valid);
    const int32_t outerLeft = center.p1X + config.p1Bounds.left;
    const int32_t outerRight = center.p2X + config.p2Bounds.right;
    QNP_CHECK((outerLeft + outerRight) / 2
              == (config.worldLeft + config.worldRight) / 2);
    QNP_CHECK(center.p2X - center.p1X
              == config.p1Bounds.right - config.p2Bounds.left
                     + config.closeContactGap);
    QNP_CHECK(center.p2X - center.p1X == center.anchorSeparation);
    QNP_CHECK(center.anchorSeparation < config.anchorSeparation);
    QNP_CHECK(center.p1X + config.p1Bounds.right
              == center.p2X + config.p2Bounds.left);
    QNP_CHECK(right.anchorSeparation == center.anchorSeparation);
    QNP_CHECK(left.anchorSeparation == center.anchorSeparation);
    QNP_CHECK(round.anchorSeparation == config.anchorSeparation);
}

QNP_TEST(TrainingPositionPlannerAcceptsOverlapEngineResolves) {
    PositionMathConfig config{};
    config.nativeP1X = 100;
    config.nativeP2X = 120;
    config.nativeCameraX = 88;
    config.p1Bounds = FighterHorizontalBounds{-20, 20};
    config.p2Bounds = FighterHorizontalBounds{-20, 20};
    PositionPlan plan =
        planPositionPreset(PositionPreset::RoundStart, config);
    QNP_REQUIRE(plan.valid);
    QNP_CHECK(plan.error == PositionPlanError::None);
    QNP_CHECK(plan.p1X + config.p1Bounds.right
              > plan.p2X + config.p2Bounds.left);

    PositionMathConfig spaced = config;
    spaced.minimumEdgeGap = 8;
    PositionPlan gapped =
        planPositionPreset(PositionPreset::RoundStart, spaced);
    QNP_CHECK(!gapped.valid);
    QNP_CHECK(gapped.error
              == PositionPlanError::EdgeGapNotSatisfied);

    QNP_CHECK(!planPositionPreset(
                   PositionPreset::FromModifier, config).valid);
    QNP_CHECK(!planPositionPreset(
                   static_cast<PositionPreset>(99), config).valid);
}

QNP_TEST(TrainingPositionPlannerRejectsCrossedBoxes) {
    PositionMathConfig config{};
    config.worldRight = 511;
    config.anchorSeparation = 8;
    config.p1Bounds = FighterHorizontalBounds{100, 140};
    config.p2Bounds = FighterHorizontalBounds{-140, -100};
    PositionPlan plan{};
    plan.p1X = 200;
    plan.p2X = 208;
    plan.p1Facing = 0;
    plan.p2Facing = 1;
    plan.cameraX = 100;
    QNP_CHECK(validatePositionPlan(plan, config)
              == PositionPlanError::FightersCrossed);
}

QNP_TEST(TrainingPositionPlansRealCharacterZeroGeometry) {
    PositionMathConfig config{};
    config.worldLeft = 0;
    config.worldRight = 511;
    config.cameraMin = 0;
    config.cameraMax = 192;
    config.cameraBias = 32;
    config.anchorSeparation = 128;
    config.minimumEdgeGap = 0;
    config.nativeP1X = 64;
    config.nativeP2X = 192;
    config.nativeCameraX = 96;
    config.p1Bounds = FighterHorizontalBounds{105, 135};
    config.p2Bounds = FighterHorizontalBounds{120, 150};

    const PositionPlan round =
        planPositionPreset(PositionPreset::RoundStart, config);
    QNP_REQUIRE(round.valid);
    QNP_CHECK(round.p1X == 64 && round.p2X == 192);
    QNP_CHECK(round.cameraX == 96);

    const PositionPlan center =
        planPositionPreset(PositionPreset::Center, config);
    QNP_REQUIRE(center.valid);
    QNP_CHECK(center.p1X + config.p1Bounds.left >= config.worldLeft);
    QNP_CHECK(center.p2X + config.p2Bounds.right <= config.worldRight);
    QNP_CHECK(center.p1X + config.p1Bounds.right
              == center.p2X + config.p2Bounds.left);
    QNP_CHECK(center.p2X - center.p1X == 15);
    QNP_CHECK(center.p2X - center.p1X < config.anchorSeparation);
    QNP_CHECK(center.cameraX
              == (center.p1X + center.p2X) / 2 - config.cameraBias);
    QNP_CHECK(center.cameraX != config.nativeCameraX);

    const PositionPlan right =
        planPositionPreset(PositionPreset::RightCorner, config);
    QNP_REQUIRE(right.valid);
    QNP_CHECK(right.p2X + config.p2Bounds.right == config.worldRight);
    QNP_CHECK(right.p1X + config.p1Bounds.right
              == right.p2X + config.p2Bounds.left);
    QNP_CHECK(right.p2X - right.p1X == 15);

    PositionMathConfig flipped = config;
    flipped.p1Bounds = FighterHorizontalBounds{120, 150};
    flipped.p2Bounds = FighterHorizontalBounds{105, 135};
    const PositionPlan left =
        planPositionPreset(PositionPreset::LeftCorner, flipped);
    QNP_REQUIRE(left.valid);
    QNP_CHECK(left.p2X + flipped.p2Bounds.left == flipped.worldLeft);
    QNP_CHECK(left.p1Facing == 1 && left.p2Facing == 0);
    QNP_CHECK(left.p2X + flipped.p2Bounds.right
              == left.p1X + flipped.p1Bounds.left);
    QNP_CHECK(left.p1X - left.p2X == 15);

    QNP_CHECK(round.anchorSeparation == 128);
    QNP_CHECK(center.anchorSeparation == 15);
    QNP_CHECK(right.anchorSeparation == 15);
    QNP_CHECK(left.anchorSeparation == 15);

    QNP_CHECK(round.p1X == config.nativeP1X);
    QNP_CHECK(round.p2X == config.nativeP2X);
    QNP_CHECK(round.cameraX == config.nativeCameraX);
    QNP_CHECK(round.p2X - round.p1X == 128);
    QNP_CHECK(round.p1Facing == 0 && round.p2Facing == 1);
    PositionMathConfig wide = config;
    wide.closeContactGap = 24;
    const PositionPlan roundWide =
        planPositionPreset(PositionPreset::RoundStart, wide);
    QNP_REQUIRE(roundWide.valid);
    QNP_CHECK(roundWide.p1X == 64 && roundWide.p2X == 192);
    QNP_CHECK(roundWide.cameraX == 96);
    QNP_CHECK(roundWide.anchorSeparation == 128);
    const PositionPlan centerWide =
        planPositionPreset(PositionPreset::Center, wide);
    QNP_REQUIRE(centerWide.valid);
    QNP_CHECK(centerWide.anchorSeparation == 15 + 24);
    QNP_CHECK(centerWide.p2X + wide.p2Bounds.left
                  - (centerWide.p1X + wide.p1Bounds.right) == 24);

    QNP_CHECK(positionPresetFromResetModifier(qinput::kLogicalDown).preset
              == PositionPreset::Center);
    QNP_CHECK(positionPresetFromResetModifier(qinput::kLogicalUp).preset
              == PositionPreset::VeryClose);
    QNP_CHECK(positionPresetFromResetModifier(qinput::kLogicalLeft).preset
              == PositionPreset::LeftCorner);
    QNP_CHECK(positionPresetFromResetModifier(qinput::kLogicalRight).preset
              == PositionPreset::RightCorner);
    QNP_CHECK(positionPresetFromResetModifier(0).preset
              == PositionPreset::RoundStart);
    const PositionPlan veryClose =
        planPositionPreset(PositionPreset::VeryClose, config);
    QNP_REQUIRE(veryClose.valid);
    QNP_CHECK(veryClose.p1X == center.p1X && veryClose.p2X == center.p2X);
    QNP_CHECK(veryClose.cameraX == center.cameraX);
    QNP_CHECK(veryClose.anchorSeparation == center.anchorSeparation);
}

QNP_TEST(TrainingPositionVeryClosePlacesBoxesEdgeToEdge) {
    PositionMathConfig config{};
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
    config.p1Bounds = FighterHorizontalBounds{105, 135};
    config.p2Bounds = FighterHorizontalBounds{120, 150};

    const PositionPlan close =
        planPositionPreset(PositionPreset::VeryClose, config);
    QNP_REQUIRE(close.valid);
    QNP_CHECK(close.error == PositionPlanError::None);
    QNP_CHECK(close.p1X + config.p1Bounds.right
              == close.p2X + config.p2Bounds.left);
    QNP_CHECK(close.anchorSeparation == close.p2X - close.p1X);
    QNP_CHECK(close.anchorSeparation < config.anchorSeparation);
    QNP_CHECK(close.p1Facing == 0 && close.p2Facing == 1);
    const int32_t outerLeft = close.p1X + config.p1Bounds.left;
    const int32_t outerRight = close.p2X + config.p2Bounds.right;
    QNP_CHECK((outerLeft + outerRight) / 2
              == (config.worldLeft + config.worldRight) / 2);
    QNP_CHECK(close.cameraX
              == (close.p1X + close.p2X) / 2 - config.cameraBias);

    PositionMathConfig gapped = config;
    gapped.closeContactGap = 6;
    const PositionPlan spaced =
        planPositionPreset(PositionPreset::VeryClose, gapped);
    QNP_REQUIRE(spaced.valid);
    QNP_CHECK(spaced.p2X + gapped.p2Bounds.left
              - (spaced.p1X + gapped.p1Bounds.right) == 6);

    PositionMathConfig inverted = config;
    inverted.p1Bounds = FighterHorizontalBounds{-40, -20};
    inverted.p2Bounds = FighterHorizontalBounds{20, 40};
    const PositionPlan degenerate =
        planPositionPreset(PositionPreset::VeryClose, inverted);
    QNP_REQUIRE(degenerate.valid);
    QNP_CHECK(degenerate.anchorSeparation >= 1);
}

QNP_TEST(InputDisplayDirectionMapIsTotalAndSocdCancelsPerAxis) {
    QNP_CHECK(qinput::kInputDisplayDirectionCount == 9u);
    QNP_CHECK(qinput::kInputDisplayCompassCount == 8u);

    bool produced[qinput::kInputDisplayDirectionCount] = {};
    for (const DisplayNibbleCase& entry : kDisplayNibbleTable) {
        const uint32_t mask = logicalFromDisplayNibble(entry.nibble);
        QNP_CHECK(qinput::inputDisplayDirectionNibble(mask) == entry.nibble);
        const qinput::InputDisplayDirection direction =
            qinput::inputDisplayDirection(mask);
        if (direction != entry.expected) {
            std::printf("    [display] nibble %X -> %s, want %s\n",
                        entry.nibble,
                        qinput::inputDisplayDirectionName(direction),
                        qinput::inputDisplayDirectionName(entry.expected));
        }
        QNP_CHECK(direction == entry.expected);
        QNP_CHECK(qinput::inputDisplayDirectionFromNibble(entry.nibble)
                  == entry.expected);
        QNP_CHECK(static_cast<uint32_t>(direction)
                  < qinput::kInputDisplayDirectionCount);
        produced[static_cast<uint32_t>(direction)] = true;

        QNP_CHECK(qinput::inputDisplayDirection(mask | qinput::kLogicalA
                                                | qinput::kLogicalD)
                  == entry.expected);
        QNP_CHECK(qinput::inputDisplayDirection(mask | 0x0FF00u)
                  == entry.expected);
    }
    for (uint32_t i = 0; i < qinput::kInputDisplayDirectionCount; ++i)
        QNP_CHECK(produced[i]);

    const qinput::InputDisplayDirection points[8] = {
        qinput::InputDisplayDirection::Right,
        qinput::InputDisplayDirection::DownRight,
        qinput::InputDisplayDirection::Down,
        qinput::InputDisplayDirection::DownLeft,
        qinput::InputDisplayDirection::Left,
        qinput::InputDisplayDirection::UpLeft,
        qinput::InputDisplayDirection::Up,
        qinput::InputDisplayDirection::UpRight,
    };
    bool seenIndex[8] = {};
    for (uint32_t i = 0; i < 8u; ++i) {
        QNP_CHECK(points[i] != qinput::InputDisplayDirection::Neutral);
        const uint32_t compass = qinput::inputDisplayCompassIndex(points[i]);
        QNP_REQUIRE(compass < 8u);
        QNP_CHECK(!seenIndex[compass]);
        seenIndex[compass] = true;
        QNP_CHECK(compass == i);
        for (uint32_t j = i + 1u; j < 8u; ++j)
            QNP_CHECK(points[i] != points[j]);
    }
    QNP_CHECK(qinput::inputDisplayCompassIndex(
                  qinput::InputDisplayDirection::Neutral)
              == qinput::kInputDisplayNoCompass);
    QNP_CHECK(qinput::inputDisplayCompassIndex(
                  qinput::InputDisplayDirection::Count)
              == qinput::kInputDisplayNoCompass);

    uint32_t diagonals = 0;
    for (uint32_t raw = 0; raw < qinput::kInputDisplayDirectionCount; ++raw) {
        if (qinput::inputDisplayDirectionIsDiagonal(
                static_cast<qinput::InputDisplayDirection>(raw))) {
            ++diagonals;
        }
    }
    QNP_CHECK(diagonals == 4u);
    QNP_CHECK(qinput::inputDisplayDirectionIsDiagonal(
        qinput::InputDisplayDirection::UpLeft));
    QNP_CHECK(qinput::inputDisplayDirectionIsDiagonal(
        qinput::InputDisplayDirection::UpRight));
    QNP_CHECK(qinput::inputDisplayDirectionIsDiagonal(
        qinput::InputDisplayDirection::DownLeft));
    QNP_CHECK(qinput::inputDisplayDirectionIsDiagonal(
        qinput::InputDisplayDirection::DownRight));
    QNP_CHECK(!qinput::inputDisplayDirectionIsDiagonal(
        qinput::InputDisplayDirection::Up));
    QNP_CHECK(!qinput::inputDisplayDirectionIsDiagonal(
        qinput::InputDisplayDirection::Neutral));

    QNP_CHECK(qinput::inputDisplayDirection(qinput::kLogicalUp
                                            | qinput::kLogicalDown)
              == qinput::InputDisplayDirection::Neutral);
    QNP_CHECK(qinput::inputDisplayDirection(qinput::kLogicalLeft
                                            | qinput::kLogicalRight)
              == qinput::InputDisplayDirection::Neutral);
    QNP_CHECK(qinput::inputDisplayDirection(qinput::kLogicalUp
                                            | qinput::kLogicalDown
                                            | qinput::kLogicalLeft)
              == qinput::InputDisplayDirection::Left);
    QNP_CHECK(qinput::inputDisplayDirection(qinput::kLogicalLeft
                                            | qinput::kLogicalRight
                                            | qinput::kLogicalDown)
              == qinput::InputDisplayDirection::Down);
    QNP_CHECK(qinput::inputDisplayDirection(qinput::kLogicalDirectionMask)
              == qinput::InputDisplayDirection::Neutral);

    QNP_CHECK(qinput::kInputDisplayButtonCount == 4u);
    const uint32_t chord = qinput::kLogicalB | qinput::kLogicalD
        | qinput::kLogicalLeft;
    QNP_CHECK(!qinput::inputDisplayButtonPressed(
        chord, qinput::kInputDisplayButtonOrder[0]));
    QNP_CHECK(qinput::inputDisplayButtonPressed(
        chord, qinput::kInputDisplayButtonOrder[1]));
    QNP_CHECK(!qinput::inputDisplayButtonPressed(
        chord, qinput::kInputDisplayButtonOrder[2]));
    QNP_CHECK(qinput::inputDisplayButtonPressed(
        chord, qinput::kInputDisplayButtonOrder[3]));
    for (uint32_t i = 0; i < qinput::kInputDisplayButtonCount; ++i) {
        QNP_CHECK(qinput::inputDisplayButtonPressed(
            qinput::logicalBit(qinput::kInputDisplayButtonOrder[i]),
            qinput::kInputDisplayButtonOrder[i]));
        QNP_CHECK(!qinput::inputDisplayButtonPressed(
            qinput::kLogicalDirectionMask,
            qinput::kInputDisplayButtonOrder[i]));
    }
}

QNP_TEST(InputDisplayHistoryExtendsSaturatesAndWrapsInOrder) {
    qinput::InputDisplayLaneV1 lane{};
    QNP_CHECK(qinput::inputDisplayLaneStructurallyValid(lane));
    QNP_CHECK(lane.entryCount == 0u);
    QNP_CHECK(qinput::inputDisplayLaneSlotAt(lane, 0u)
              == qinput::kInputDisplayNoLane);

    const uint32_t first = qinput::kLogicalRight | qinput::kLogicalA;
    QNP_CHECK(qinput::inputDisplayLaneAppend(lane, first)
              == qinput::InputDisplayAppend::Started);
    QNP_CHECK(lane.entryCount == 1u);
    QNP_CHECK(lane.head == 0u);
    QNP_CHECK(lane.currentMask == first);
    QNP_REQUIRE(qinput::inputDisplayLaneSlotAt(lane, 0u) == 0u);
    QNP_CHECK(lane.entries[0].logicalMask == first);
    QNP_CHECK(lane.entries[0].heldSteps == 1u);

    for (uint32_t held = 2u; held <= 60u; ++held) {
        QNP_CHECK(qinput::inputDisplayLaneAppend(lane, first)
                  == qinput::InputDisplayAppend::Extended);
        QNP_CHECK(lane.entryCount == 1u);
        QNP_CHECK(lane.head == 0u);
        QNP_CHECK(lane.entries[0].heldSteps == held);
    }

    const uint32_t second = qinput::kLogicalRight | qinput::kLogicalDown;
    QNP_CHECK(qinput::inputDisplayLaneAppend(lane, second)
              == qinput::InputDisplayAppend::Started);
    QNP_CHECK(lane.entryCount == 2u);
    QNP_CHECK(lane.head == 1u);
    QNP_CHECK(lane.currentMask == second);
    QNP_REQUIRE(qinput::inputDisplayLaneSlotAt(lane, 0u) == 1u);
    QNP_REQUIRE(qinput::inputDisplayLaneSlotAt(lane, 1u) == 0u);
    QNP_CHECK(lane.entries[1].heldSteps == 1u);
    QNP_CHECK(lane.entries[0].logicalMask == first);
    QNP_CHECK(lane.entries[0].heldSteps == 60u);
    QNP_CHECK(qinput::inputDisplayLaneSlotAt(lane, 2u)
              == qinput::kInputDisplayNoLane);

    qinput::InputDisplayLaneV1 held{};
    for (uint32_t step = 1u; step <= qinput::kInputDisplayHoldCap + 500u;
         ++step) {
        const qinput::InputDisplayAppend outcome =
            qinput::inputDisplayLaneAppend(held, qinput::kLogicalDown);
        QNP_CHECK(outcome == (step == 1u
                                  ? qinput::InputDisplayAppend::Started
                                  : qinput::InputDisplayAppend::Extended));
        const uint32_t expected =
            step < qinput::kInputDisplayHoldCap ? step
                                                : qinput::kInputDisplayHoldCap;
        QNP_CHECK(held.entries[held.head].heldSteps == expected);
        QNP_CHECK(held.entryCount == 1u);
    }
    QNP_CHECK(held.entries[held.head].heldSteps
              == qinput::kInputDisplayHoldCap);

    qinput::InputDisplayLaneV1 ring{};
    const uint32_t written = qinput::kInputDisplayHistoryCapacity + 8u;
    for (uint32_t i = 0; i < written; ++i) {
        QNP_CHECK(qinput::inputDisplayLaneAppend(ring, distinctLogicalMask(i))
                  == qinput::InputDisplayAppend::Started);
    }
    QNP_CHECK(ring.entryCount == qinput::kInputDisplayHistoryCapacity);
    QNP_CHECK(qinput::inputDisplayLaneStructurallyValid(ring));
    QNP_CHECK(ring.currentMask == distinctLogicalMask(written - 1u));
    for (uint32_t ordinal = 0;
         ordinal < qinput::kInputDisplayHistoryCapacity; ++ordinal) {
        const uint32_t slot = qinput::inputDisplayLaneSlotAt(ring, ordinal);
        QNP_REQUIRE(slot < qinput::kInputDisplayHistoryCapacity);
        QNP_CHECK(ring.entries[slot].logicalMask
                  == distinctLogicalMask(written - 1u - ordinal));
        QNP_CHECK(ring.entries[slot].heldSteps == 1u);
        QNP_CHECK(qinput::inputDisplayOlderSlot(slot)
                  == qinput::inputDisplayLaneSlotAt(ring, ordinal + 1u)
                     || ordinal + 1u
                            == qinput::kInputDisplayHistoryCapacity);
    }
    QNP_CHECK(qinput::inputDisplayLaneSlotAt(
                  ring, qinput::kInputDisplayHistoryCapacity)
              == qinput::kInputDisplayNoLane);
    uint32_t copies = 0;
    for (uint32_t i = 0; i < qinput::kInputDisplayHistoryCapacity; ++i) {
        if (ring.entries[i].logicalMask == distinctLogicalMask(written - 1u))
            ++copies;
    }
    QNP_CHECK(copies == 1u);

    qinput::InputDisplayLaneV1 before = ring;
    QNP_CHECK(qinput::inputDisplayLaneAppend(ring, 0x00000100u)
              == qinput::InputDisplayAppend::Rejected);
    QNP_CHECK(std::memcmp(&before, &ring, sizeof(ring)) == 0);
    QNP_CHECK(qinput::inputDisplayLaneAppend(
                  ring, qinput::kLogicalFightMask | 0x80000000u)
              == qinput::InputDisplayAppend::Rejected);
    QNP_CHECK(std::memcmp(&before, &ring, sizeof(ring)) == 0);

    qinput::InputDisplayLaneV1 corrupt{};
    corrupt.head = qinput::kInputDisplayHistoryCapacity;
    before = corrupt;
    QNP_CHECK(!qinput::inputDisplayLaneStructurallyValid(corrupt));
    QNP_CHECK(qinput::inputDisplayLaneAppend(corrupt, qinput::kLogicalA)
              == qinput::InputDisplayAppend::Rejected);
    QNP_CHECK(std::memcmp(&before, &corrupt, sizeof(corrupt)) == 0);

    qinput::InputDisplayLaneV1 overfull{};
    overfull.entryCount = qinput::kInputDisplayHistoryCapacity + 1u;
    before = overfull;
    QNP_CHECK(!qinput::inputDisplayLaneStructurallyValid(overfull));
    QNP_CHECK(qinput::inputDisplayLaneAppend(overfull, qinput::kLogicalA)
              == qinput::InputDisplayAppend::Rejected);
    QNP_CHECK(std::memcmp(&before, &overfull, sizeof(overfull)) == 0);
    QNP_CHECK(qinput::inputDisplayLaneSlotAt(overfull, 0u)
              == qinput::kInputDisplayNoLane);

    qinput::inputDisplayLaneClearHistory(ring);
    QNP_CHECK(ring.entryCount == 0u);
    QNP_CHECK(ring.head == 0u);
    QNP_CHECK(ring.currentMask == 0u);
    for (uint32_t i = 0; i < qinput::kInputDisplayHistoryCapacity; ++i) {
        QNP_CHECK(ring.entries[i].logicalMask == 0u);
        QNP_CHECK(ring.entries[i].heldSteps == 0u);
    }
}

QNP_TEST(InputDisplayRepeatedStepSerialNeverAdvancesHistory) {
    qinput::InputDisplayComposer composer;
    composer.reset(4u);

    QNP_CHECK(composer.publishStep(
                  trainingDisplayStep(4u, 1u, 0u, qinput::kLogicalRight))
              == qinput::InputDisplayPublishResult::Published);
    QNP_CHECK(composer.pending().lanes[0].entryCount == 1u);
    QNP_CHECK(composer.pending().lanes[0].entries[0].heldSteps == 1u);

    qinput::InputDisplaySnapshotV1 frozen{};
    QNP_REQUIRE(composer.read(frozen));
    QNP_CHECK(qinput::inputDisplaySnapshotRenderable(frozen));

    for (uint32_t attempt = 0; attempt < 100u; ++attempt) {
        QNP_CHECK(composer.publishStep(
                      trainingDisplayStep(4u, 1u, 0u,
                                          attempt & 1u
                                              ? qinput::kLogicalLeft
                                              : qinput::kLogicalRight))
                  == qinput::InputDisplayPublishResult
                         ::RejectedNonMonotonicSerial);
        QNP_CHECK(composer.pending().lanes[0].entryCount == 1u);
        QNP_CHECK(composer.pending().lanes[0].entries[0].heldSteps == 1u);
        QNP_CHECK(composer.pending().lanes[0].currentMask
                  == qinput::kLogicalRight);
    }
    qinput::InputDisplaySnapshotV1 after{};
    QNP_REQUIRE(composer.read(after));
    QNP_CHECK(std::memcmp(&frozen, &after, sizeof(after)) == 0);

    QNP_CHECK(composer.publishStep(
                  trainingDisplayStep(4u, 0u, 0u, qinput::kLogicalLeft))
              == qinput::InputDisplayPublishResult
                     ::RejectedNonMonotonicSerial);
    QNP_CHECK(composer.pending().lanes[0].currentMask
              == qinput::kLogicalRight);

    QNP_CHECK(composer.publishStep(
                  trainingDisplayStep(4u, 2u, 0u, qinput::kLogicalRight))
              == qinput::InputDisplayPublishResult::Published);
    QNP_CHECK(composer.pending().lanes[0].entryCount == 1u);
    QNP_CHECK(composer.pending().lanes[0].entries[0].heldSteps == 2u);

    QNP_CHECK(composer.publishStep(
                  trainingDisplayStep(4u, 9000u, 0u, qinput::kLogicalLeft))
              == qinput::InputDisplayPublishResult::Published);
    QNP_CHECK(composer.pending().lanes[0].entryCount == 2u);

    const uint64_t high = (1ull << 33) | 7ull;
    QNP_CHECK(composer.publishStep(
                  trainingDisplayStep(4u, high, 0u, qinput::kLogicalLeft))
              == qinput::InputDisplayPublishResult::Published);
    qinput::InputDisplaySnapshotV1 wide{};
    QNP_REQUIRE(composer.read(wide));
    QNP_CHECK(wide.stepSerialLow == static_cast<uint32_t>(high & 0xFFFFFFFFu));
    QNP_CHECK(wide.stepSerialHigh == 2u);
    QNP_CHECK(composer.publishStep(
                  trainingDisplayStep(4u, high, 0u, qinput::kLogicalRight))
              == qinput::InputDisplayPublishResult
                     ::RejectedNonMonotonicSerial);
    QNP_CHECK(composer.publishStep(
                  trainingDisplayStep(4u, 1ull << 33, 0u,
                                      qinput::kLogicalRight))
              == qinput::InputDisplayPublishResult
                     ::RejectedNonMonotonicSerial);
}

QNP_TEST(InputDisplayModeGatingIsFailClosedAndHasOnlyTwoProducers) {
    QNP_CHECK(static_cast<uint32_t>(qinput::InputDisplayMode::None) == 0u);
    QNP_CHECK(static_cast<uint32_t>(qinput::InputDisplayMode::Training) == 1u);
    QNP_CHECK(static_cast<uint32_t>(qinput::InputDisplayMode::ReplayPlayback)
              == 2u);
    QNP_CHECK(static_cast<uint32_t>(qinput::InputDisplayMode::Count) == 3u);
    for (uint32_t raw = 0; raw < 64u; ++raw) {
        const bool publishable = qinput::isPublishableInputDisplayMode(
            static_cast<qinput::InputDisplayMode>(raw));
        QNP_CHECK(publishable == (raw == 1u || raw == 2u));
    }
    QNP_CHECK(!qinput::isPublishableInputDisplayMode(
        static_cast<qinput::InputDisplayMode>(0xFFFFFFFFu)));

    qinput::InputDisplaySnapshotV1 blank{};
    std::memset(&blank, 0, sizeof(blank));
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(blank));
    qinput::InputDisplaySnapshotV1 defaulted{};
    QNP_CHECK(defaulted.mode
              == static_cast<uint32_t>(qinput::InputDisplayMode::None));
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(defaulted));

    qinput::InputDisplayComposer composer;
    composer.reset(11u);
    qinput::InputDisplaySnapshotV1 idle{};
    QNP_REQUIRE(composer.read(idle));
    QNP_CHECK(idle.mode
              == static_cast<uint32_t>(qinput::InputDisplayMode::None));
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(idle));

    for (uint32_t raw : {0u, 3u, 4u, 0xFFFFFFFFu}) {
        qinput::InputDisplayStep step =
            trainingDisplayStep(11u, 1u, 0u, qinput::kLogicalRight);
        step.mode = static_cast<qinput::InputDisplayMode>(raw);
        QNP_CHECK(composer.publishStep(step)
                  == qinput::InputDisplayPublishResult::RejectedMode);
    }
    qinput::InputDisplaySnapshotV1 stillIdle{};
    QNP_REQUIRE(composer.read(stillIdle));
    QNP_CHECK(std::memcmp(&idle, &stillIdle, sizeof(idle)) == 0);

    qinput::InputDisplayStep empty =
        trainingDisplayStep(11u, 1u, 0u, qinput::kLogicalRight);
    empty.laneActiveMask = 0u;
    QNP_CHECK(composer.publishStep(empty)
              == qinput::InputDisplayPublishResult::RejectedLaneMask);
    qinput::InputDisplayStep beyond =
        trainingDisplayStep(11u, 1u, 0u, qinput::kLogicalRight);
    beyond.laneActiveMask = 1u << qinput::kInputDisplayLaneCount;
    QNP_CHECK(composer.publishStep(beyond)
              == qinput::InputDisplayPublishResult::RejectedLaneMask);

    qinput::InputDisplayStep bogus =
        trainingDisplayStep(11u, 1u, 0u, 0x00000100u);
    QNP_CHECK(composer.publishStep(bogus)
              == qinput::InputDisplayPublishResult::RejectedNonCanonicalMask);

    QNP_CHECK(composer.publishStep(
                  trainingDisplayStep(11u, 1u, 0u, qinput::kLogicalRight))
              == qinput::InputDisplayPublishResult::Published);
    qinput::InputDisplaySnapshotV1 live{};
    QNP_REQUIRE(composer.read(live));
    QNP_CHECK(qinput::inputDisplaySnapshotRenderable(live));

    for (uint32_t raw : {0u, 3u, 4u, 0xFFFFFFFFu}) {
        qinput::InputDisplaySnapshotV1 tampered = live;
        tampered.mode = raw;
        QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(tampered));
    }
    qinput::InputDisplaySnapshotV1 wrongVersion = live;
    wrongVersion.apiVersion = qinput::kInputDisplayApiVersion + 1u;
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(wrongVersion));
    qinput::InputDisplaySnapshotV1 shortStruct = live;
    shortStruct.structSize = qinput::kInputDisplaySnapshotV1BaseSize - 4u;
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(shortStruct));
    qinput::InputDisplaySnapshotV1 hugeStruct = live;
    hugeStruct.structSize =
        static_cast<uint32_t>(sizeof(qinput::InputDisplaySnapshotV1)) + 4u;
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(hugeStruct));
    qinput::InputDisplaySnapshotV1 corruptLane = live;
    corruptLane.lanes[2].head = qinput::kInputDisplayHistoryCapacity;
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(corruptLane));
    qinput::InputDisplaySnapshotV1 corruptMask = live;
    corruptMask.lanes[0].entries[0].logicalMask = 0x00000100u;
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(corruptMask));
    qinput::InputDisplaySnapshotV1 overHeld = live;
    overHeld.lanes[0].entries[0].heldSteps = qinput::kInputDisplayHoldCap + 1u;
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(overHeld));

    composer.publishIdle();
    qinput::InputDisplaySnapshotV1 parked{};
    QNP_REQUIRE(composer.read(parked));
    QNP_CHECK(parked.mode
              == static_cast<uint32_t>(qinput::InputDisplayMode::None));
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(parked));
    QNP_CHECK(parked.lanes[0].active == 0u);
    QNP_CHECK(parked.lanes[0].currentMask == 0u);
    QNP_CHECK(composer.pending().lanes[0].entryCount == 1u);
    QNP_CHECK(composer.publishStep(
                  trainingDisplayStep(11u, 2u, 0u, qinput::kLogicalRight))
              == qinput::InputDisplayPublishResult::Published);
    QNP_CHECK(composer.pending().lanes[0].entryCount == 1u);
    QNP_CHECK(composer.pending().lanes[0].entries[0].heldSteps == 2u);

    composer.reset(12u);
    QNP_CHECK(composer.pending().lanes[0].entryCount == 0u);
    QNP_CHECK(composer.pending().lanes[0].active == 0u);
    qinput::InputDisplaySnapshotV1 restarted{};
    QNP_REQUIRE(composer.read(restarted));
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(restarted));
    QNP_CHECK(restarted.producerGeneration == 12u);
}

QNP_TEST(DummySettingsApplyToTheFighterYouAreNotHolding) {
    QNP_CHECK(trainingPolicyLane(false, false) == kTrainingDrivenLaneP2);
    QNP_CHECK(trainingPolicyLane(false, true) == kTrainingDrivenLaneP1);
    QNP_CHECK(trainingPolicyLane(true, false) == kTrainingDrivenLaneP1);
    QNP_CHECK(trainingPolicyLane(true, true) == kTrainingDrivenLaneP2);
    for (int driving = 0; driving < 2; ++driving) {
        for (int swapped = 0; swapped < 2; ++swapped) {
            const bool d = driving != 0;
            const bool s = swapped != 0;
            QNP_CHECK(trainingPolicyLane(d, s) != trainingDrivenLane(d, s));
        }
    }

    const uint32_t pressed = qinput::kLogicalRight | qinput::kLogicalA;
    TrainingInputStep step{};
    step.lifecycleAvailable = true;
    step.humanDrivesDummyLane = true;
    step.macroOverrideP1 = true;
    step.macroP1Mask = 0u;
    step.macroOverrideP2 = true;
    step.macroP2Mask = pressed;
    step.dummyDirections = qinput::kLogicalLeft;
    step.dummyOwnedAxes = kDummyHorizontalAxis;
    {
        TrainingInput input;
        input.reset();
        uint64_t serial = 0;
        armInput(input, serial);
        const TrainingInputResult result = input.advance(++serial, step);
        QNP_REQUIRE(result.valid);
        QNP_CHECK(result.p1Mask == qinput::kLogicalLeft);
        QNP_CHECK(result.p2Mask == pressed);
        QNP_CHECK(result.drivenLane == kTrainingDrivenLaneP2);
    }

    {
        TrainingInputStep swapped = step;
        swapped.dummyIsP1 = true;
        swapped.macroOverrideP1 = true;
        swapped.macroP1Mask = pressed;
        swapped.macroOverrideP2 = true;
        swapped.macroP2Mask = 0u;
        TrainingInput input;
        input.reset();
        uint64_t serial = 0;
        armInput(input, serial);
        const TrainingInputResult result = input.advance(++serial, swapped);
        QNP_REQUIRE(result.valid);
        QNP_CHECK(result.p1Mask == pressed);
        QNP_CHECK(result.p2Mask == qinput::kLogicalLeft);
        QNP_CHECK(result.drivenLane == kTrainingDrivenLaneP1);
    }

    {
        TrainingInputStep idle{};
        idle.lifecycleAvailable = true;
        idle.p1PhysicalMask = pressed;
        idle.dummyDirections = qinput::kLogicalLeft;
        idle.dummyOwnedAxes = kDummyHorizontalAxis;
        TrainingInput input;
        input.reset();
        uint64_t serial = 0;
        armInput(input, serial);
        const TrainingInputResult result = input.advance(++serial, idle);
        QNP_REQUIRE(result.valid);
        QNP_CHECK(result.p1Mask == pressed);
        QNP_CHECK(result.p2Mask == qinput::kLogicalLeft);
        QNP_CHECK(result.drivenLane == kTrainingDrivenLaneP1);
    }
}

QNP_TEST(InputDisplayTrainingLaneFollowsMacroRecordRouting) {
    QNP_CHECK(kTrainingDrivenLaneP1 == 0u);
    QNP_CHECK(kTrainingDrivenLaneP2 == 1u);
    QNP_CHECK(trainingDrivenLane(false) == kTrainingDrivenLaneP1);
    QNP_CHECK(trainingDrivenLane(true) == kTrainingDrivenLaneP2);

    TrainingInputStep routed{};
    QNP_CHECK(trainingDrivenLane(routed) == kTrainingDrivenLaneP1);
    routed.macroOverrideP2 = true;
    routed.macroPlaybackActive = true;
    QNP_CHECK(trainingDrivenLane(routed) == kTrainingDrivenLaneP1);
    routed.macroOverrideP1 = true;
    QNP_CHECK(trainingDrivenLane(routed) == kTrainingDrivenLaneP1);
    routed.humanDrivesDummyLane = true;
    QNP_CHECK(trainingDrivenLane(routed) == kTrainingDrivenLaneP2);
    routed.dummyIsP1 = true;
    QNP_CHECK(trainingDrivenLane(routed) == kTrainingDrivenLaneP1);
    routed.humanDrivesDummyLane = false;
    QNP_CHECK(trainingDrivenLane(routed) == kTrainingDrivenLaneP2);

    {
        TrainingInput input;
        input.reset();
        uint64_t serial = 0;
        TrainingInputStep step{};
        step.lifecycleAvailable = true;
        step.macroOverrideP1 = true;
        step.humanDrivesDummyLane = true;
        const TrainingInputResult unarmed = input.advance(++serial, step);
        QNP_CHECK(unarmed.neutralized);
        QNP_CHECK(unarmed.drivenLane == kTrainingDrivenLaneP2);
    }
    {
        TrainingInput input;
        input.reset();
        uint64_t serial = 0;
        armInput(input, serial);
        TrainingInputStep step{};
        step.lifecycleAvailable = true;
        step.p1PhysicalMask = qinput::kLogicalRight;
        TrainingInputResult result = input.advance(++serial, step);
        QNP_CHECK(result.valid);
        QNP_CHECK(result.drivenLane == kTrainingDrivenLaneP1);

        TrainingInputStep other = step;
        other.macroOverrideP1 = true;
        other.humanDrivesDummyLane = true;
        const TrainingInputResult duplicate = input.advance(serial, other);
        QNP_CHECK(duplicate.duplicateStep);
        QNP_CHECK(duplicate.drivenLane == kTrainingDrivenLaneP1);

        step.macroOverrideP1 = true;
        step.humanDrivesDummyLane = true;
        step.macroP1Mask = 0;
        step.macroOverrideP2 = true;
        step.macroP2Mask = step.p1PhysicalMask;
        result = input.advance(++serial, step);
        QNP_CHECK(result.valid);
        QNP_CHECK(result.p1Mask == 0u);
        QNP_CHECK(result.p2Mask == qinput::kLogicalRight);
        QNP_CHECK(result.drivenLane == kTrainingDrivenLaneP2);

        TrainingInputStep bad = step;
        bad.p2PhysicalMask = 0x00000100u;
        const TrainingInputResult rejected = input.advance(++serial, bad);
        QNP_CHECK(!rejected.valid);
        QNP_CHECK(rejected.drivenLane == kTrainingDrivenLaneP2);

        const TrainingInputResult backwards = input.advance(1u, step);
        QNP_CHECK(!backwards.valid);
        QNP_CHECK(backwards.drivenLane == kTrainingDrivenLaneP2);
    }

    qinput::InputDisplayComposer composer;
    composer.reset(3u);
    for (uint32_t serial = 1; serial <= 5u; ++serial) {
        QNP_CHECK(composer.publishStep(trainingDisplayStep(
                      3u, serial, kTrainingDrivenLaneP1,
                      distinctLogicalMask(serial)))
                  == qinput::InputDisplayPublishResult::Published);
    }
    QNP_CHECK(composer.pending().lanes[0].entryCount == 5u);
    QNP_CHECK(composer.pending().lanes[0].active == 1u);
    QNP_CHECK(composer.pending().lanes[1].active == 0u);

    QNP_CHECK(composer.publishStep(trainingDisplayStep(
                  3u, 6u, kTrainingDrivenLaneP2, qinput::kLogicalA))
              == qinput::InputDisplayPublishResult::Published);
    QNP_CHECK(composer.pending().drivenLane == kTrainingDrivenLaneP2);
    QNP_CHECK(composer.pending().lanes[0].entryCount == 0u);
    QNP_CHECK(composer.pending().lanes[0].active == 0u);
    QNP_CHECK(composer.pending().lanes[0].currentMask == 0u);
    QNP_CHECK(composer.pending().lanes[1].entryCount == 1u);
    QNP_CHECK(composer.pending().lanes[1].currentMask == qinput::kLogicalA);

    QNP_CHECK(composer.publishStep(trainingDisplayStep(
                  9u, 7u, kTrainingDrivenLaneP2, qinput::kLogicalA))
              == qinput::InputDisplayPublishResult::Published);
    QNP_CHECK(composer.pending().lanes[1].entryCount == 1u);
    QNP_CHECK(composer.pending().lanes[1].entries[
                  composer.pending().lanes[1].head].heldSteps == 1u);

    QNP_CHECK(composer.publishStep(
                  replayDisplayStep(9u, 8u, 0x3u, qinput::kLogicalLeft,
                                    qinput::kLogicalRight))
              == qinput::InputDisplayPublishResult::Published);
    QNP_CHECK(composer.pending().drivenLane == qinput::kInputDisplayNoLane);
    QNP_CHECK(composer.pending().lanes[0].entryCount == 1u);
    QNP_CHECK(composer.pending().lanes[1].entryCount == 1u);
    QNP_CHECK(composer.pending().lanes[0].currentMask == qinput::kLogicalLeft);
    QNP_CHECK(composer.pending().lanes[1].currentMask
              == qinput::kLogicalRight);
    QNP_CHECK(composer.pending().lanes[0].active == 1u);
    QNP_CHECK(composer.pending().lanes[1].active == 1u);
    QNP_CHECK(composer.pending().lanes[2].active == 0u);
    QNP_CHECK(composer.pending().lanes[3].active == 0u);

    qinput::InputDisplayComposer strict;
    strict.reset(1u);
    qinput::InputDisplayStep mismatched =
        trainingDisplayStep(1u, 1u, 0u, qinput::kLogicalRight);
    mismatched.laneActiveMask = 1u << 1;
    QNP_CHECK(strict.publishStep(mismatched)
              == qinput::InputDisplayPublishResult::RejectedDrivenLane);
    qinput::InputDisplayStep both =
        trainingDisplayStep(1u, 1u, 0u, qinput::kLogicalRight);
    both.laneActiveMask = 0x3u;
    both.laneLogicalMask[1] = qinput::kLogicalLeft;
    QNP_CHECK(strict.publishStep(both)
              == qinput::InputDisplayPublishResult::RejectedDrivenLane);
    qinput::InputDisplayStep driverless =
        trainingDisplayStep(1u, 1u, 0u, qinput::kLogicalRight);
    driverless.drivenLane = qinput::kInputDisplayNoLane;
    QNP_CHECK(strict.publishStep(driverless)
              == qinput::InputDisplayPublishResult::RejectedDrivenLane);
    qinput::InputDisplayStep outOfRange =
        trainingDisplayStep(1u, 1u, 0u, qinput::kLogicalRight);
    outOfRange.drivenLane = qinput::kInputDisplayLaneCount;
    outOfRange.laneActiveMask = 1u;
    QNP_CHECK(strict.publishStep(outOfRange)
              == qinput::InputDisplayPublishResult::RejectedDrivenLane);
    qinput::InputDisplayStep claimed =
        replayDisplayStep(1u, 1u, 0x3u, 0u, 0u);
    claimed.drivenLane = 0u;
    QNP_CHECK(strict.publishStep(claimed)
              == qinput::InputDisplayPublishResult::RejectedDrivenLane);
    qinput::InputDisplaySnapshotV1 untouched{};
    QNP_REQUIRE(strict.read(untouched));
    QNP_CHECK(!qinput::inputDisplaySnapshotRenderable(untouched));
}

QNP_TEST(InputDisplaySnapshotAbiAndTierNegotiation) {
    QNP_CHECK(qinput::kInputDisplayApiVersion == 1u);
    QNP_CHECK(qinput::kInputDisplayLaneCount == 4u);
    QNP_CHECK(qinput::kInputDisplayHistoryCapacity == 32u);
    QNP_CHECK(qinput::kInputDisplayHoldCap == 999u);
    QNP_CHECK(qinput::kInputDisplayNoLane == 0xFFFFFFFFu);
    QNP_CHECK(qinput::kInputDisplayNoCompass == 0xFFFFFFFFu);
    QNP_CHECK(qinput::kInputDisplayNibbleUp == 0x1u);
    QNP_CHECK(qinput::kInputDisplayNibbleDown == 0x2u);
    QNP_CHECK(qinput::kInputDisplayNibbleLeft == 0x4u);
    QNP_CHECK(qinput::kInputDisplayNibbleRight == 0x8u);
    QNP_CHECK(qinput::kInputDisplayNibbleMask == 0xFu);

    QNP_CHECK(sizeof(qinput::InputDisplayEntryV1) == 8u);
    QNP_CHECK(sizeof(qinput::InputDisplayLaneV1) == 272u);
    QNP_CHECK(sizeof(qinput::InputDisplaySnapshotV1) == 1128u);
    QNP_CHECK(offsetof(qinput::InputDisplayEntryV1, heldSteps) == 4u);
    QNP_CHECK(offsetof(qinput::InputDisplayLaneV1, active) == 0u);
    QNP_CHECK(offsetof(qinput::InputDisplayLaneV1, currentMask) == 4u);
    QNP_CHECK(offsetof(qinput::InputDisplayLaneV1, entryCount) == 8u);
    QNP_CHECK(offsetof(qinput::InputDisplayLaneV1, head) == 12u);
    QNP_CHECK(offsetof(qinput::InputDisplayLaneV1, entries) == 16u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, apiVersion) == 0u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, structSize) == 4u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, mode) == 8u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, producerGeneration)
              == 12u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, stepSerialLow) == 16u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, stepSerialHigh) == 20u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, drivenLane) == 24u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, laneCount) == 28u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, historyCapacity)
              == 32u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, holdCap) == 36u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, lanes) == 40u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, lanes)
                  + 1u * sizeof(qinput::InputDisplayLaneV1)
              == 312u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, lanes)
                  + 3u * sizeof(qinput::InputDisplayLaneV1)
              == 856u);
    QNP_CHECK(offsetof(qinput::InputDisplaySnapshotV1, lanes)
                  + qinput::kInputDisplayLaneCount
                        * sizeof(qinput::InputDisplayLaneV1)
              == sizeof(qinput::InputDisplaySnapshotV1));

    QNP_CHECK(sizeof(qinput::InputDisplaySnapshotV1) % sizeof(uint32_t) == 0u);
    QNP_CHECK(std::is_standard_layout<qinput::InputDisplaySnapshotV1>::value);
    QNP_CHECK(
        std::is_trivially_copyable<qinput::InputDisplaySnapshotV1>::value);
    QNP_CHECK(std::is_standard_layout<qinput::InputDisplayLaneV1>::value);
    QNP_CHECK(std::is_trivially_copyable<qinput::InputDisplayLaneV1>::value);

    QNP_CHECK(qinput::kInputDisplaySnapshotV1TierCount == 1u);
    QNP_CHECK(qinput::kInputDisplaySnapshotV1BaseSize == 1128u);
    QNP_CHECK(qinput::kInputDisplaySnapshotV1SizeTiers[0]
              == qinput::kInputDisplaySnapshotV1BaseSize);
    QNP_CHECK(qinput::kInputDisplaySnapshotV1SizeTiers[
                  qinput::kInputDisplaySnapshotV1TierCount - 1u]
              == sizeof(qinput::InputDisplaySnapshotV1));
    QNP_CHECK(qinput::inputDisplayTiersAscend());
    QNP_CHECK(qinput::inputDisplayTierFor(0u) == 0u);
    QNP_CHECK(qinput::inputDisplayTierFor(
                  qinput::kInputDisplaySnapshotV1BaseSize - 1u) == 0u);
    for (uint32_t i = 0; i < qinput::kInputDisplaySnapshotV1TierCount; ++i) {
        const uint32_t tier = qinput::kInputDisplaySnapshotV1SizeTiers[i];
        QNP_CHECK(qinput::inputDisplayTierFor(tier) == tier);
        QNP_CHECK(qinput::inputDisplayTierFor(tier + 1u) == tier);
        QNP_CHECK(tier <= sizeof(qinput::InputDisplaySnapshotV1));
    }
    QNP_CHECK(qinput::inputDisplayTierFor(
                  static_cast<uint32_t>(
                      sizeof(qinput::InputDisplaySnapshotV1)) + 4096u)
              == sizeof(qinput::InputDisplaySnapshotV1));
}

QNP_TEST(InputDisplayPublisherStampsItsHeaderAndNeverTears) {
    qinput::InputDisplayPublisher publisher;
    qinput::InputDisplaySnapshotV1 initial{};
    QNP_REQUIRE(publisher.read(initial));
    QNP_CHECK(initial.apiVersion == qinput::kInputDisplayApiVersion);
    QNP_CHECK(initial.mode
              == static_cast<uint32_t>(qinput::InputDisplayMode::None));

    qinput::InputDisplaySnapshotV1 lying{};
    lying.apiVersion = 0xDEADu;
    lying.structSize = 4u;
    lying.laneCount = 77u;
    lying.historyCapacity = 3u;
    lying.holdCap = 1u;
    lying.mode = static_cast<uint32_t>(qinput::InputDisplayMode::Training);
    lying.drivenLane = 1u;
    lying.producerGeneration = 42u;
    lying.lanes[1].active = 1u;
    lying.lanes[1].entryCount = 1u;
    lying.lanes[1].currentMask = qinput::kLogicalC;
    lying.lanes[1].entries[0].logicalMask = qinput::kLogicalC;
    lying.lanes[1].entries[0].heldSteps = 5u;
    publisher.publish(lying);

    qinput::InputDisplaySnapshotV1 stamped{};
    QNP_REQUIRE(publisher.read(stamped));
    QNP_CHECK(stamped.apiVersion == qinput::kInputDisplayApiVersion);
    QNP_CHECK(stamped.structSize == sizeof(qinput::InputDisplaySnapshotV1));
    QNP_CHECK(stamped.laneCount == qinput::kInputDisplayLaneCount);
    QNP_CHECK(stamped.historyCapacity == qinput::kInputDisplayHistoryCapacity);
    QNP_CHECK(stamped.holdCap == qinput::kInputDisplayHoldCap);
    QNP_CHECK(stamped.producerGeneration == 42u);
    QNP_CHECK(stamped.drivenLane == 1u);
    QNP_CHECK(stamped.lanes[1].entries[0].heldSteps == 5u);
    QNP_CHECK(qinput::inputDisplaySnapshotRenderable(stamped));

    constexpr uint32_t kCount = 4000;
    std::atomic<bool> writerDone{false};
    std::atomic<uint32_t> failures{0};
    std::thread writer([&] {
        for (uint32_t value = 1; value <= kCount; ++value) {
            qinput::InputDisplaySnapshotV1 snapshot{};
            snapshot.mode =
                static_cast<uint32_t>(qinput::InputDisplayMode::Training);
            snapshot.producerGeneration = value;
            snapshot.stepSerialLow = value;
            snapshot.stepSerialHigh = ~value;
            snapshot.drivenLane = value & 1u;
            snapshot.lanes[0].entries[0].heldSteps = value * 3u;
            snapshot.lanes[3].entries[
                qinput::kInputDisplayHistoryCapacity - 1u].heldSteps =
                    value * 5u;
            publisher.publish(snapshot);
        }
        writerDone.store(true);
    });
    do {
        qinput::InputDisplaySnapshotV1 snapshot{};
        if (!publisher.read(snapshot)) continue;
        const uint32_t value = snapshot.producerGeneration;
        if (value != 0u && value != 42u
            && (snapshot.stepSerialLow != value
                || snapshot.stepSerialHigh != ~value
                || snapshot.drivenLane != (value & 1u)
                || snapshot.lanes[0].entries[0].heldSteps != value * 3u
                || snapshot.lanes[3].entries[
                       qinput::kInputDisplayHistoryCapacity - 1u].heldSteps
                       != value * 5u)) {
            failures.fetch_add(1);
        }
    } while (!writerDone.load());
    writer.join();
    qinput::InputDisplaySnapshotV1 last{};
    QNP_REQUIRE(publisher.read(last));
    QNP_CHECK(last.producerGeneration == kCount);
    QNP_CHECK(failures.load() == 0u);
}

QNP_TEST(TrainingReturnToSelectHotkeyIsRisingEdgeAndNeverEntersTheFightWire) {
    const uint32_t f1 =
        trainingActionBit(TrainingAction::ReturnToCharacterSelect);
    QNP_CHECK(f1 == (1u << 9));
    QNP_CHECK(kTrainingActionCount == 16u);
    QNP_CHECK(kTrainingActionMask == 0xFFFFu);
    for (uint32_t ordinal = 0; ordinal < 9u; ++ordinal) {
        QNP_CHECK(trainingActionBit(static_cast<TrainingAction>(ordinal))
                  == (1u << ordinal));
    }
    QNP_CHECK(isCanonicalTrainingActions(f1));
    QNP_CHECK(isCanonicalTrainingActions(kTrainingActionMask));
    QNP_CHECK(isCanonicalTrainingActions(1u << 15));
    QNP_CHECK(!isCanonicalTrainingActions(1u << 16));

    QNP_CHECK((trainingImageMutatingActionMask() & f1) == 0u);
    QNP_CHECK(trainingImageMutatingActionMask()
              == (trainingActionBit(TrainingAction::ResetPosition)
                  | trainingActionBit(TrainingAction::ResetPositionRoundStart)
                  | trainingActionBit(TrainingAction::LoadState)));
    QNP_CHECK(!commandOwnsNativeImageTransaction(
        TrainingCommandKind::ReturnToCharacterSelect));
    QNP_CHECK(!commandMayCoalesce(
        TrainingCommandKind::ReturnToCharacterSelect));
    QNP_CHECK(isValidCommandKind(
        TrainingCommandKind::ReturnToCharacterSelect));
    QNP_CHECK(static_cast<uint32_t>(
                  TrainingCommandKind::ReturnToCharacterSelect) == 16u);
    TrainingCommandV1 exitCommand =
        command(1, 1, TrainingCommandKind::ReturnToCharacterSelect);
    QNP_CHECK(validCommandPayload(exitCommand));
    exitCommand.value = 1;
    QNP_CHECK(!validCommandPayload(exitCommand));

    TrainingInput input;
    input.reset();
    uint64_t poll = 0;
    QNP_CHECK(armInput(input, poll).rearmState == InputRearmState::Armed);

    TrainingActionResult actions = pollInput(input, poll, f1, 0, true);
    QNP_CHECK(actions.valid);
    QNP_CHECK(actions.actionEdges == f1);
    QNP_CHECK(actions.resetModifier == 0u);

    TrainingActionPoll same{};
    same.lifecycleAvailable = true;
    same.relevantPhysicalNeutralBeforeSocd = true;
    same.heldTrainingActions = f1;
    const TrainingActionResult duplicate = input.pollActions(poll, same);
    QNP_CHECK(duplicate.duplicatePoll);
    QNP_CHECK(duplicate.actionEdges == 0u);

    uint32_t fired = 0;
    for (uint32_t frame = 0; frame < 180u; ++frame) {
        actions = pollInput(input, poll, f1, 0, true);
        if (actions.actionEdges & f1) ++fired;
    }
    QNP_CHECK(fired == 0u);

    actions = pollInput(input, poll, 0, 0, true);
    QNP_CHECK(actions.actionEdges == 0u);
    actions = pollInput(input, poll, f1, 0, true);
    QNP_CHECK(actions.actionEdges == f1);

    TrainingActionPoll captured{};
    captured.lifecycleAvailable = true;
    captured.menuCaptured = true;
    captured.heldTrainingActions = f1;
    actions = input.pollActions(++poll, captured);
    QNP_CHECK(actions.rearmState == InputRearmState::Captured);
    QNP_CHECK(actions.actionEdges == 0u);
    actions = pollInput(input, poll, f1, 0, true);
    actions = pollInput(input, poll, f1, 0, true);
    QNP_CHECK(actions.rearmState == InputRearmState::Armed);
    actions = pollInput(input, poll, f1, 0, true);
    QNP_CHECK(actions.actionEdges == 0u);

    actions = pollInput(input, poll, 0, 0, true);
    const uint32_t save = trainingActionBit(TrainingAction::SaveState);
    actions = pollInput(input, poll, f1 | save, 0, true);
    QNP_CHECK(actions.actionEdges == (f1 | save));

    uint64_t serial = 0;
    TrainingInput held;
    held.reset();
    armInput(held, serial);
    QNP_CHECK(pollInput(held, serial, f1, 0, true).actionEdges == f1);
    TrainingInputStep step{};
    step.lifecycleAvailable = true;
    TrainingInputResult composed = held.advance(++serial, step);
    QNP_CHECK(composed.valid);
    QNP_CHECK(composed.p1Mask == 0u);
    QNP_CHECK(composed.p2Mask == 0u);
    QNP_CHECK(composed.neutralized);

    step.p1PhysicalMask = qinput::kLogicalRight | qinput::kLogicalA;
    step.p2PhysicalMask = qinput::kLogicalDown;
    composed = held.advance(++serial, step);
    QNP_CHECK(composed.valid);
    QNP_CHECK(composed.p1Mask == (qinput::kLogicalRight | qinput::kLogicalA));
    QNP_CHECK(composed.p2Mask == qinput::kLogicalDown);
    QNP_CHECK(qinput::isCanonicalLogicalFightInput(composed.p1Mask));
    QNP_CHECK(qinput::isCanonicalLogicalFightInput(composed.p2Mask));
    QNP_CHECK(qinput::logicalFromCompact(
                  qinput::compactFromLogical(composed.p1Mask))
              == composed.p1Mask);
    QNP_CHECK(qinput::logicalFromCompact(
                  qinput::compactFromLogical(composed.p2Mask))
              == composed.p2Mask);
    QNP_CHECK((composed.p1Mask & ~qinput::kLogicalFightMask) == 0u);
    QNP_CHECK((composed.p2Mask & ~qinput::kLogicalFightMask) == 0u);

    QNP_CHECK(qinput::inputDisplayDirection(composed.p1Mask)
              == qinput::InputDisplayDirection::Right);
    qinput::InputDisplayComposer display;
    display.reset(1u);
    QNP_CHECK(display.publishStep(trainingDisplayStep(
                  1u, 1u, composed.drivenLane, composed.p1Mask))
              == qinput::InputDisplayPublishResult::Published);
    QNP_CHECK(display.pending().lanes[0].currentMask == composed.p1Mask);
}

QNP_TEST(TrainingPresentationToggleIsARearmSafeRisingEdge) {
    const uint32_t debug =
        trainingActionBit(TrainingAction::ToggleHitboxes);

    TrainingInput input;
    input.reset();
    uint64_t poll = 0u;
    QNP_REQUIRE(armInput(input, poll).rearmState == InputRearmState::Armed);
    QNP_CHECK(pollInput(input, poll, debug, 0u, true).actionEdges == debug);
    QNP_CHECK(pollInput(input, poll, debug, 0u, true).actionEdges == 0u);
    QNP_CHECK(pollInput(input, poll, 0u, 0u, true).actionEdges == 0u);
    QNP_CHECK(pollInput(input, poll, debug, 0u, true).actionEdges == debug);

    TrainingActionPoll captured{};
    captured.lifecycleAvailable = true;
    captured.menuCaptured = true;
    captured.heldTrainingActions = debug;
    const TrainingActionResult capture = input.pollActions(++poll, captured);
    QNP_CHECK(capture.rearmState == InputRearmState::Captured);
    QNP_CHECK(capture.actionEdges == 0u);
    QNP_CHECK(pollInput(input, poll, debug, 0u, true).actionEdges == 0u);
}

QNP_TEST(TrainingCombatTierWrapperStillReadsEveryCombatFieldFrom812) {
    QNP_CHECK(sizeof(CombatTierTrainingStatusV1)
              == kTrainingStatusV1CombatTier);
    QNP_CHECK(offsetof(CombatTierTrainingStatusV1, frozen) == 0u);
    QNP_CHECK(offsetof(CombatTierTrainingStatusV1, combatReadoutValid)
              == kTrainingStatusV1BaseSize);
    QNP_CHECK(offsetof(CombatTierTrainingStatusV1, p2MaxComboDamageDealt)
              == 692u);

    TrainingStatusV1 modern{};
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&modern);
    for (uint32_t offset = 0; offset < kTrainingStatusV1CombatTier; ++offset)
        bytes[offset] = static_cast<unsigned char>(3u + (offset * 53u) % 241u);
    std::memset(bytes + kTrainingStatusV1CombatTier, 0xEE,
                sizeof(modern) - kTrainingStatusV1CombatTier);
    modern.apiVersion = kTrainingApiVersion;

    const uint32_t outSize = sizeof(CombatTierTrainingStatusV1);
    const uint32_t copied = trainingStatusTierFor(outSize);
    QNP_CHECK(copied == kTrainingStatusV1CombatTier);
    QNP_CHECK(copied > kTrainingStatusV1BaseSize);
    modern.structSize = copied;

    CombatTierTrainingStatusV1 combat;
    std::memset(&combat, 0xCD, sizeof(combat));
    std::memcpy(&combat, &modern, copied);

    QNP_CHECK(combat.frozen.apiVersion == kTrainingApiVersion);
    QNP_CHECK(combat.frozen.structSize == kTrainingStatusV1CombatTier);
    QNP_CHECK(combat.frozen.structSize >= sizeof(CombatTierTrainingStatusV1));

#define QNP_COMBAT_FIELD(field)                                        \
    QNP_CHECK(offsetof(TrainingStatusV1, field)                        \
              == offsetof(CombatTierTrainingStatusV1, field));         \
    QNP_CHECK(modern.field == combat.field)

    QNP_COMBAT_FIELD(combatReadoutValid);
    QNP_COMBAT_FIELD(dummyStance);
    QNP_COMBAT_FIELD(dummyTech);
    QNP_COMBAT_FIELD(p1EffectiveLife);
    QNP_COMBAT_FIELD(p2EffectiveLife);
    QNP_COMBAT_FIELD(lifeMax);
    QNP_COMBAT_FIELD(p1GuardGauge);
    QNP_COMBAT_FIELD(p2GuardGauge);
    QNP_COMBAT_FIELD(p1MeterStocks);
    QNP_COMBAT_FIELD(p2MeterStocks);
    QNP_COMBAT_FIELD(p1MeterFraction);
    QNP_COMBAT_FIELD(p2MeterFraction);
    QNP_COMBAT_FIELD(damageEventSerial);
    QNP_COMBAT_FIELD(damageDeclinedCount);
    QNP_COMBAT_FIELD(lastHitDamage);
    QNP_COMBAT_FIELD(lastHitPendingDelta);
    QNP_COMBAT_FIELD(lastHitAttackerSlot);
    QNP_COMBAT_FIELD(lastHitVictimSlot);
    QNP_COMBAT_FIELD(lastHitCause);
    QNP_COMBAT_FIELD(lastHitAdmitted);
    QNP_COMBAT_FIELD(p1ComboHitsTaken);
    QNP_COMBAT_FIELD(p2ComboHitsTaken);
    QNP_COMBAT_FIELD(p1ComboDamageTaken);
    QNP_COMBAT_FIELD(p2ComboDamageTaken);
    QNP_COMBAT_FIELD(p1LastComboHitsTaken);
    QNP_COMBAT_FIELD(p2LastComboHitsTaken);
    QNP_COMBAT_FIELD(p1LastComboDamageTaken);
    QNP_COMBAT_FIELD(p2LastComboDamageTaken);
    QNP_COMBAT_FIELD(p1MaxComboHitsDealt);
    QNP_COMBAT_FIELD(p2MaxComboHitsDealt);
    QNP_COMBAT_FIELD(p1MaxComboDamageDealt);
    QNP_COMBAT_FIELD(p2MaxComboDamageDealt);

#undef QNP_COMBAT_FIELD

    QNP_CHECK(modern.available == combat.frozen.available);
    QNP_CHECK(modern.p1FinalLogicalMask == combat.frozen.p1FinalLogicalMask);
    QNP_CHECK(std::memcmp(modern.macroFrameCounts,
                          combat.frozen.macroFrameCounts,
                          sizeof(combat.frozen.macroFrameCounts)) == 0);
    QNP_CHECK(std::memcmp(modern.resultText, combat.frozen.resultText,
                          sizeof(combat.frozen.resultText)) == 0);

    QNP_CHECK(std::memcmp(&combat, &modern, kTrainingStatusV1CombatTier) == 0);
    QNP_CHECK(bytes[kTrainingStatusV1CombatTier] == 0xEE);
    QNP_CHECK(modern.p1LifeCurrent == 0xEEEEEEEEu);
    QNP_CHECK(modern.fighterReadoutValid == 0xEEEEEEEEu);
    QNP_CHECK(modern.p2PositionY == 0xEEEEEEEEu);

    CombatTierTrainingStatusV1 stale;
    std::memset(&stale, 0, sizeof(stale));
    stale.frozen.apiVersion = kTrainingApiVersion;
    stale.frozen.structSize = sizeof(CombatTierTrainingStatusV1);
    stale.frozen.available = 1u;
    stale.combatReadoutValid = 1u;
    stale.lifeMax = 9200u;
    stale.p1EffectiveLife = 4321u;
    stale.p2MaxComboDamageDealt = 777u;

    TrainingStatusV1 consumer{};
    std::memset(&consumer, 0, sizeof(consumer));
    std::memcpy(&consumer, &stale, stale.frozen.structSize);
    QNP_CHECK(consumer.structSize == kTrainingStatusV1CombatTier);
    QNP_CHECK(consumer.structSize >= kTrainingStatusV1CombatTier);
    QNP_CHECK(consumer.structSize < kTrainingStatusV1FighterTier);
    QNP_CHECK(consumer.combatReadoutValid == 1u);
    QNP_CHECK(consumer.lifeMax == 9200u);
    QNP_CHECK(consumer.p1EffectiveLife == 4321u);
    QNP_CHECK(consumer.p2MaxComboDamageDealt == 777u);
    QNP_CHECK(consumer.fighterReadoutValid == 0u);
    const unsigned char* tail =
        reinterpret_cast<const unsigned char*>(&consumer);
    for (uint32_t offset = kTrainingStatusV1CombatTier;
         offset < sizeof(consumer); ++offset) {
        QNP_CHECK(tail[offset] == 0u);
    }
}

namespace {
struct RingBuilder {
    DirectionRingSnapshot ring{};
    uint8_t count = 0u;
    RingBuilder& push(uint32_t value, uint32_t dwell) {
        ring.head = static_cast<uint8_t>(count & 31u);
        ring.values[ring.head] = value;
        ring.dwell[ring.head] = dwell;
        ++count;
        return *this;
    }
};
}

QNP_TEST(TrainingTerminalRollPredictionMirrorsTheNativeCheck) {
    constexpr uint32_t back = kRecoveryRingBack;
    constexpr uint32_t forward = kRecoveryRingForward;
    constexpr uint32_t downBack = 0x60000u;

    RingBuilder bounce;
    bounce.push(downBack, 40).push(back, 3).push(0u, 1);
    QNP_CHECK(terminalRecoveryWouldMatch(bounce.ring, back));
    QNP_CHECK(!terminalRecoveryWouldMatch(bounce.ring, downBack));
    QNP_CHECK(!terminalRecoveryWouldMatch(bounce.ring, 0u));

    RingBuilder taps;
    taps.push(back, 2).push(0u, 0).push(0x20000u, 0).push(0u, 0)
        .push(0x20000u, 1);
    QNP_CHECK(terminalRecoveryWouldMatch(taps.ring, back));

    RingBuilder stale;
    stale.push(back, 6).push(0u, 4);
    QNP_CHECK(!terminalRecoveryWouldMatch(stale.ring, back));

    RingBuilder posture;
    posture.push(back, 2).push(downBack, 1).push(back, 1).push(downBack, 1);
    QNP_CHECK(!terminalRecoveryWouldMatch(posture.ring, back));

    RingBuilder held;
    held.push(back, 1).push(0u, 1).push(back, 2);
    QNP_CHECK(terminalRecoveryWouldMatch(held.ring, back));
    RingBuilder aged;
    aged.push(back, 1).push(0u, 1).push(back, 7);
    QNP_CHECK(!terminalRecoveryWouldMatch(aged.ring, back));

    RingBuilder front;
    front.push(forward, 1).push(0u, 1);
    QNP_CHECK(terminalRecoveryWouldMatch(front.ring, forward));
    QNP_CHECK(!terminalRecoveryWouldMatch(front.ring, back));
}

QNP_TEST(TrainingRecoveryEligibilityMatchesEveryStateMapRow) {
    const RecoveryBranch branches[] = {
        RecoveryBranch::Air22,
        RecoveryBranch::Ground88,
        RecoveryBranch::Wall66,
        RecoveryBranch::TerminalForward66,
        RecoveryBranch::TerminalBack44,
    };
    QNP_CHECK(kRecoveryBranchCount == 6u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryBranch::ThrowEscape) == 5u);

    for (const RecoveryBranch branch : branches) {
        const RecoveryObservation eligible = recoveryEligibleFor(branch);
        const uint32_t mask = recoveryEligibleMask(eligible);
        if (mask != recoveryBranchBit(branch)) {
            std::printf("    [recovery] %s eligible mask %02X, want %02X\n",
                        recoveryBranchName(branch), mask,
                        recoveryBranchBit(branch));
        }
        QNP_CHECK(mask == recoveryBranchBit(branch));
    }

    for (const RecoveryBranch branch : branches) {
        RecoveryObservation observation = recoveryEligibleFor(branch);
        observation.readable = false;
        QNP_CHECK(recoveryEligibleMask(observation) == 0u);
        QNP_CHECK(recoveryArmableMask(observation) == 0u);
        recoveryEmitsNothing("unreadable", branch, observation);

        observation = recoveryEligibleFor(branch);
        observation.comControlled = true;
        QNP_CHECK(recoveryEligibleMask(observation) == 0u);
        recoveryEmitsNothing("COM owned", branch, observation);

        observation = recoveryEligibleFor(branch);
        observation.directionSuppressed = true;
        QNP_CHECK(recoveryEligibleMask(observation) == 0u);
        recoveryEmitsNothing("direction suppressed", branch, observation);

        observation = recoveryEligibleFor(branch);
        observation.koLatch = true;
        QNP_CHECK(recoveryEligibleMask(observation) == 0u);
        recoveryEmitsNothing("KO latched", branch, observation);

        observation = recoveryEligibleFor(branch);
        observation.action = 99;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) == 0u);
        recoveryEmitsNothing("wrong action", branch, observation);

        observation = recoveryEligibleFor(branch);
        observation.postureNibble = 3;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) == 0u);
        recoveryEmitsNothing("wrong posture", branch, observation);
    }

    const RecoveryBranch manual[] = {
        RecoveryBranch::Air22,
        RecoveryBranch::Ground88,
        RecoveryBranch::Wall66,
    };
    for (const RecoveryBranch branch : manual) {
        RecoveryObservation observation = recoveryEligibleFor(branch);
        observation.permission = 0;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) == 0u);
        recoveryEmitsNothing("no permission bit", branch, observation);

        observation = recoveryEligibleFor(branch);
        observation.permission =
            branch == RecoveryBranch::Air22 ? kRecoveryPermitGround
                                            : kRecoveryPermitAir;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) == 0u);

        observation = recoveryEligibleFor(branch);
        observation.recoveryLock = 1;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) == 0u);
        recoveryEmitsNothing("lock set", branch, observation);

        observation = recoveryEligibleFor(branch);
        observation.postureNibble = 4;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) != 0u);
    }

    const RecoveryBranch terminalFamily[] = {
        RecoveryBranch::TerminalForward66,
        RecoveryBranch::TerminalBack44,
    };
    for (const RecoveryBranch branch : terminalFamily) {
        RecoveryObservation observation = recoveryEligibleFor(branch);
        observation.recoveryLock = 0x7Fu;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) != 0u);
        observation = recoveryEligibleFor(branch);
        observation.permission = 0;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) != 0u);
        observation = recoveryEligibleFor(branch);
        observation.terminalReady = false;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) == 0u);
        recoveryEmitsNothing("terminal not ready", branch, observation);
        observation = recoveryEligibleFor(branch);
        observation.postureNibble = 4;
        QNP_CHECK((recoveryEligibleMask(observation)
                   & recoveryBranchBit(branch)) == 0u);
    }

    for (const uint16_t side : {uint16_t{0}, uint16_t{1}, uint16_t{2},
                                uint16_t{3}}) {
        RecoveryObservation wall =
            recoveryEligibleFor(RecoveryBranch::Wall66);
        wall.wallFlags = side;
        QNP_CHECK((recoveryEligibleMask(wall)
                   & recoveryBranchBit(RecoveryBranch::Wall66)) != 0u);
    }
    {
        RecoveryObservation wall =
            recoveryEligibleFor(RecoveryBranch::Wall66);
        wall = recoveryEligibleFor(RecoveryBranch::Wall66);
        wall.action = 35;
        QNP_CHECK((recoveryEligibleMask(wall)
                   & recoveryBranchBit(RecoveryBranch::Wall66)) == 0u);
        QNP_CHECK((recoveryEligibleMask(wall)
                   & recoveryBranchBit(RecoveryBranch::Ground88)) != 0u);
    }

    for (const uint32_t action : {25u, 38u, 56u, 57u, 58u}) {
        RecoveryObservation air = recoveryEligibleFor(RecoveryBranch::Air22);
        air.action = action;
        QNP_CHECK((recoveryEligibleMask(air)
                   & recoveryBranchBit(RecoveryBranch::Air22)) != 0u);
        RecoveryObservation ground =
            recoveryEligibleFor(RecoveryBranch::Ground88);
        ground.action = action;
        QNP_CHECK((recoveryEligibleMask(ground)
                   & recoveryBranchBit(RecoveryBranch::Ground88)) != 0u);
    }
    {
        RecoveryObservation air = recoveryEligibleFor(RecoveryBranch::Air22);
        air.action = 35;
        QNP_CHECK((recoveryEligibleMask(air)
                   & recoveryBranchBit(RecoveryBranch::Air22)) == 0u);
        RecoveryObservation ground =
            recoveryEligibleFor(RecoveryBranch::Ground88);
        ground.action = 35;
        QNP_CHECK((recoveryEligibleMask(ground)
                   & recoveryBranchBit(RecoveryBranch::Ground88)) != 0u);
        ground.action = 200;
        QNP_CHECK((recoveryEligibleMask(ground)
                   & recoveryBranchBit(RecoveryBranch::Ground88)) == 0u);
        RecoveryObservation terminal =
            recoveryEligibleFor(RecoveryBranch::TerminalForward66);
        terminal.action = 512;
        QNP_CHECK((recoveryEligibleMask(terminal)
                   & recoveryBranchBit(
                       RecoveryBranch::TerminalForward66)) != 0u);
    }

    {
        RecoveryObservation forward =
            recoveryEligibleFor(RecoveryBranch::TerminalForward66);
        forward.directionRingHead = kRecoveryRingBack;
        QNP_CHECK((recoveryEligibleMask(forward)
                   & recoveryBranchBit(
                       RecoveryBranch::TerminalForward66)) == 0u);
        QNP_CHECK((recoveryEligibleMask(forward)
                   & recoveryBranchBit(
                       RecoveryBranch::TerminalBack44)) != 0u);
        forward.directionRingHead = 0;
        QNP_CHECK((recoveryEligibleMask(forward)
                   & (recoveryBranchBit(RecoveryBranch::TerminalForward66)
                      | recoveryBranchBit(
                          RecoveryBranch::TerminalBack44))) == 0u);

    }

    {
        const RecoveryObservation forward =
            recoveryEligibleFor(RecoveryBranch::TerminalForward66);
        const uint32_t armable = recoveryArmableMask(forward);
        QNP_CHECK(armable
                  == (recoveryBranchBit(RecoveryBranch::TerminalForward66)
                      | recoveryBranchBit(RecoveryBranch::TerminalBack44)));
        QNP_CHECK(recoveryEligibleMask(forward)
                  == recoveryBranchBit(RecoveryBranch::TerminalForward66));
        QNP_CHECK((armable & recoveryEligibleMask(forward))
                  == recoveryEligibleMask(forward));
    }

    QNP_CHECK(nativeRecoveryReleaseQualified(ManualRecoveryMode::Off, false));
    for (const ManualRecoveryMode mode :
         {ManualRecoveryMode::Earliest, ManualRecoveryMode::FixedDelay,
          ManualRecoveryMode::RandomDelay, ManualRecoveryMode::Late}) {
        QNP_CHECK(!nativeRecoveryReleaseQualified(mode, false));
        QNP_CHECK(nativeRecoveryReleaseQualified(mode, true));
    }
    QNP_CHECK(!nativeRecoveryReleaseQualified(
        static_cast<ManualRecoveryMode>(99), false));
    for (const RecoveryBranch branch : branches) {
        RecoverySettings unqualified = recoveryArmed(branch);
        unqualified.evaluatorQualified = false;
        TrainingRecoveryController controller;
        controller.reset(0);
        uint64_t serial = 0;
        for (uint32_t step = 0; step < 6u; ++step) {
            const RecoveryEmission emission = controller.advance(
                ++serial, unqualified, recoveryEligibleFor(branch));
            QNP_CHECK(!emission.active);
            QNP_CHECK(emission.directions == 0u);
        }
        QNP_CHECK(controller.counters().attempts == 0u);
    }
}

QNP_TEST(TrainingLegacyThrowEscapeIsAlwaysInert) {
    const uint32_t throwBit =
        recoveryBranchBit(RecoveryBranch::ThrowEscape);

    for (uint32_t action = 0u; action <= 1024u; ++action) {
        QNP_CHECK(!recoveryBranchActionEligible(
            RecoveryBranch::ThrowEscape, action));
        QNP_CHECK(!recoveryBranchAcceptsResult(
            RecoveryBranch::ThrowEscape, action));
    }
    QNP_CHECK(!recoveryBranchActionEligible(
        RecoveryBranch::ThrowEscape, 0xFFFFFFFFu));
    QNP_CHECK(!recoveryBranchAcceptsResult(
        RecoveryBranch::ThrowEscape, 0xFFFFFFFFu));
    QNP_CHECK(recoveryBranchWindow(RecoveryBranch::ThrowEscape) == 0u);
    QNP_CHECK(recoveryEmissionStepCount(RecoveryBranch::ThrowEscape) == 0u);
    for (uint32_t index = 0u; index < 4u; ++index) {
        const RecoveryStepMask step = recoveryEmissionStep(
            RecoveryBranch::ThrowEscape, index, 0u);
        QNP_CHECK(step.directions == 0u);
        QNP_CHECK(step.buttons == 0u);
    }
    const RecoveryStepMask await = recoveryAwaitMask(
        RecoveryBranch::ThrowEscape, 0u);
    QNP_CHECK(await.directions == 0u);
    QNP_CHECK(await.buttons == 0u);
    QNP_CHECK(!isRecoveryResultAction(70u));
    QNP_CHECK(!isRecoveryResultAction(73u));

    const uint32_t actions[] = {
        0u, 25u, 35u, 37u, 38u, 58u, 70u, 73u, 199u, 200u,
        0xFFFFFFFFu};
    const uint8_t permissions[] = {0u, 4u, 8u, 12u};
    const uint8_t postures[] = {0u, 1u, 2u, 4u, 9u, 15u};
    const uint32_t directionHeads[] = {
        0u, kRecoveryRingForward, kRecoveryRingBack};
    const uint32_t buttonHeads[] = {
        0u, kRecoveryButtonRingThrow,
        kRecoveryButtonRingThrow | qinput::kLogicalD};

    for (const uint32_t action : actions) {
        for (const uint8_t permission : permissions) {
            for (const uint8_t posture : postures) {
                for (const uint32_t lock : {0u, 1u}) {
                    for (const bool ready : {false, true}) {
                        for (const uint32_t directionHead : directionHeads) {
                            for (const uint32_t buttonHead : buttonHeads) {
                                RecoveryObservation observation =
                                    recoveryAvailable();
                                observation.action = action;
                                observation.permission = permission;
                                observation.postureNibble = posture;
                                observation.recoveryLock = lock;
                                observation.terminalReady = ready;
                                observation.directionRingHead = directionHead;
                                observation.buttonRingHead = buttonHead;
                                QNP_CHECK((recoveryEligibleMask(observation)
                                           & throwBit) == 0u);
                                QNP_CHECK((recoveryArmableMask(observation)
                                           & throwBit) == 0u);
                            }
                        }
                    }
                }
            }
        }
    }

    for (const ManualRecoveryMode mode :
         {ManualRecoveryMode::Off, ManualRecoveryMode::Earliest,
          ManualRecoveryMode::FixedDelay, ManualRecoveryMode::RandomDelay,
          ManualRecoveryMode::Late}) {
        RecoverySettings settings{};
        settings.throwEscape = mode;
        settings.evaluatorQualified = true;
        TrainingRecoveryController controller;
        controller.reset(0);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::ThrowEscape);
        uint64_t serial = 0u;
        for (uint32_t step = 0u; step < 16u; ++step) {
            observation.action = (step & 1u) == 0u ? 25u : 200u;
            observation.directionRingHead =
                (step & 2u) == 0u ? kRecoveryRingBack
                                  : kRecoveryRingForward;
            observation.buttonRingHead =
                (step & 4u) == 0u ? kRecoveryButtonRingThrow
                                  : kRecoveryButtonRingThrow
                                        | qinput::kLogicalD;
            const RecoveryEmission emission =
                controller.advance(++serial, settings, observation);
            QNP_CHECK(emission.valid);
            QNP_CHECK(!emission.active);
            QNP_CHECK(emission.directions == 0u);
            QNP_CHECK(emission.buttons == 0u);
            QNP_CHECK(!emission.ownsButtons);
            QNP_CHECK((emission.eligibleMask & throwBit) == 0u);
            QNP_CHECK((emission.armableMask & throwBit) == 0u);
        }
        QNP_CHECK(controller.counters().attempts == 0u);
        QNP_CHECK(controller.counters().succeeded == 0u);
        QNP_CHECK(controller.counters().missed == 0u);
        QNP_CHECK(controller.counters().cancelled == 0u);
    }
}

QNP_TEST(TrainingRecoveryEvaluatorReasonIsTotalAndNeverInventsAResult) {
    using qtraining::RecoveryEvaluatorReason;
    using qtraining::recoveryEvaluatorReasonFor;

    QNP_CHECK(recoveryEvaluatorReasonFor(true, true, true)
              == RecoveryEvaluatorReason::Qualified);
    QNP_CHECK(recoveryEvaluatorReasonFor(true, false, false)
              == RecoveryEvaluatorReason::AddressUnresolved);
    QNP_CHECK(recoveryEvaluatorReasonFor(true, true, false)
              == RecoveryEvaluatorReason::PrologueMismatch);

    for (const bool resolved : {false, true}) {
        for (const bool matched : {false, true}) {
            QNP_CHECK(recoveryEvaluatorReasonFor(false, resolved, matched)
                      == RecoveryEvaluatorReason::NotAttempted);
        }
    }

    QNP_CHECK(recoveryEvaluatorReasonFor(true, false, true)
              == RecoveryEvaluatorReason::AddressUnresolved);

    QNP_CHECK(static_cast<uint32_t>(RecoveryEvaluatorReason::Count) == 4u);
}

QNP_TEST(TrainingRecoveryUnqualifiedEvaluatorLeavesEveryModeBehavingAsOff) {
    const RecoveryBranch branches[] = {
        RecoveryBranch::Air22,
        RecoveryBranch::Ground88,
        RecoveryBranch::Wall66,
        RecoveryBranch::TerminalForward66,
        RecoveryBranch::TerminalBack44,
    };

    RecoverySettings everything{};
    everything.air = ManualRecoveryMode::Earliest;
    everything.ground = ManualRecoveryMode::Earliest;
    everything.wall = ManualRecoveryMode::Earliest;
    everything.terminal = ManualRecoveryMode::Earliest;
    everything.throwEscape = ManualRecoveryMode::Earliest;
    everything.terminalChoice = TerminalRecoveryChoice::Random;
    everything.evaluatorQualified = false;

    TrainingRecoveryController controller;
    controller.reset(0);
    uint64_t serial = 0;
    for (uint32_t round = 0; round < 5u; ++round) {
        for (const RecoveryBranch branch : branches) {
            const RecoveryEmission emission = controller.advance(
                ++serial, everything, recoveryEligibleFor(branch));
            QNP_CHECK(emission.valid);
            QNP_CHECK(!emission.duplicateStep);
            QNP_CHECK(!emission.active);
            QNP_CHECK(emission.directions == 0u);
            QNP_CHECK(emission.buttons == 0u);
            QNP_CHECK(!emission.ownsButtons);
            QNP_CHECK(emission.state == RecoveryState::Idle
                      || emission.state == RecoveryState::WaitEligible);
            QNP_CHECK(emission.state != RecoveryState::DelayEligibleSteps);
            QNP_CHECK(emission.state != RecoveryState::EmitPreNeutral);
            QNP_CHECK(emission.state != RecoveryState::EmitTap1);
            QNP_CHECK(emission.state != RecoveryState::EmitNeutral);
            QNP_CHECK(emission.state != RecoveryState::EmitTap2);
            QNP_CHECK(emission.state != RecoveryState::AwaitNativeTransition);
            QNP_CHECK(emission.state != RecoveryState::ObservingBuffer);
            QNP_CHECK(emission.state != RecoveryState::Cooldown);
        }
    }
    QNP_CHECK(serial == 25u);

    const RecoveryCounters& counters = controller.counters();
    QNP_CHECK(counters.attempts == 0u);
    QNP_CHECK(counters.succeeded == 0u);
    QNP_CHECK(counters.missed == 0u);
    QNP_CHECK(counters.cancelled == 0u);
    QNP_CHECK(counters.commitBlocked == 0u);

    everything.evaluatorQualified = true;
    TrainingRecoveryController released;
    released.reset(0);
    const RecoveryEmission armed = released.advance(
        1, everything, recoveryEligibleFor(RecoveryBranch::Air22));
    QNP_CHECK(armed.active);
    QNP_CHECK(released.counters().attempts == 1u);
}

QNP_TEST(TrainingRecoveryEmitsTapNeutralTapAcrossDistinctAdmittedSteps) {
    TrainingRecoveryController controller;
    controller.reset(0);
    const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Air22);
    observation.directionRingHead = 0;

    uint64_t serial = 0;
    RecoveryEmission tap1 = controller.advance(++serial, settings, observation);
    QNP_CHECK(tap1.valid);
    QNP_CHECK(tap1.active);
    QNP_CHECK(tap1.directions == qinput::kLogicalDown);
    QNP_CHECK(tap1.buttons == 0u);
    QNP_CHECK(!tap1.ownsButtons);
    QNP_CHECK(tap1.state == RecoveryState::EmitTap1);
    QNP_CHECK(tap1.branch == RecoveryBranch::Air22);
    QNP_CHECK(controller.counters().attempts == 1u);

    const RecoveryEmission repeat =
        controller.advance(serial, settings, observation);
    QNP_CHECK(repeat.duplicateStep);
    QNP_CHECK(repeat.directions == tap1.directions);
    QNP_CHECK(repeat.state == tap1.state);

    const RecoveryEmission neutral =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(neutral.active);
    QNP_CHECK(neutral.directions == 0u);
    QNP_CHECK(neutral.state == RecoveryState::EmitNeutral);

    const RecoveryEmission tap2 =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(tap2.active);
    QNP_CHECK(tap2.directions == qinput::kLogicalDown);
    QNP_CHECK(tap2.state == RecoveryState::EmitTap2);

    QNP_CHECK(serial == 3u);

    for (uint32_t step = 0; step < kRecoveryAwaitCapSteps; ++step) {
        const RecoveryEmission await =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(await.active);
        QNP_CHECK(await.directions == 0u);
        QNP_CHECK(await.state == RecoveryState::AwaitNativeTransition);
    }
    const RecoveryEmission buffered =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(!buffered.active);
    QNP_CHECK(buffered.state == RecoveryState::ObservingBuffer);

    TrainingRecoveryController held;
    held.reset(0);
    RecoveryObservation holding =
        recoveryEligibleFor(RecoveryBranch::Air22);
    holding.directionRingHead = qinput::kLogicalDown;
    uint64_t heldSerial = 0;
    const RecoveryEmission pre =
        held.advance(++heldSerial, settings, holding);
    QNP_CHECK(pre.active);
    QNP_CHECK(pre.directions == 0u);
    QNP_CHECK(pre.state == RecoveryState::EmitPreNeutral);
    QNP_CHECK(held.advance(++heldSerial, settings, holding).directions
              == qinput::kLogicalDown);
    QNP_CHECK(held.advance(++heldSerial, settings, holding).directions == 0u);
    QNP_CHECK(held.advance(++heldSerial, settings, holding).directions
              == qinput::kLogicalDown);

    QNP_CHECK(recoveryEmissionStepCount(RecoveryBranch::Air22) == 4u);
    QNP_CHECK(recoveryEmissionStepCount(RecoveryBranch::Ground88) == 4u);
    QNP_CHECK(recoveryEmissionStepCount(RecoveryBranch::Wall66) == 4u);
    for (uint8_t facing = 0; facing <= 1u; ++facing) {
        const uint32_t forward =
            qinput::applyNativeFacing(qinput::kLogicalRight, facing);
        const uint32_t back =
            qinput::applyNativeFacing(qinput::kLogicalLeft, facing);
        struct SequenceRow {
            RecoveryBranch branch;
            uint32_t tap;
        };
        const SequenceRow rows[] = {
            {RecoveryBranch::Air22, qinput::kLogicalDown},
            {RecoveryBranch::Ground88, qinput::kLogicalUp},
            {RecoveryBranch::Wall66, forward},
            {RecoveryBranch::TerminalForward66, forward},
            {RecoveryBranch::TerminalBack44, back},
        };
        for (const SequenceRow& row : rows) {
            QNP_CHECK(recoveryEmissionStep(row.branch, 0, facing).directions
                      == 0u);
            QNP_CHECK(recoveryEmissionStep(row.branch, 1, facing).directions
                      == row.tap);
            QNP_CHECK(recoveryEmissionStep(row.branch, 2, facing).directions
                      == 0u);
            QNP_CHECK(recoveryEmissionStep(row.branch, 3, facing).directions
                      == row.tap);
            for (uint32_t index = 0; index < 4u; ++index) {
                QNP_CHECK(recoveryEmissionStep(row.branch, index,
                                               facing).buttons == 0u);
            }
        }
    }
}

QNP_TEST(TrainingRecoveryGround88LeavesUpReleasedAfterTapTwo) {
    QNP_CHECK(recoveryAwaitMask(RecoveryBranch::Ground88, 0).directions == 0u);
    QNP_CHECK(recoveryAwaitMask(RecoveryBranch::Ground88, 0).buttons == 0u);
    QNP_CHECK(recoveryAwaitMask(RecoveryBranch::Air22, 1).directions == 0u);
    QNP_CHECK(recoveryAwaitMask(RecoveryBranch::Wall66, 1).directions == 0u);
    QNP_CHECK(recoveryAwaitMask(RecoveryBranch::TerminalForward66, 0).directions
              == qinput::kLogicalRight);
    QNP_CHECK(recoveryAwaitMask(RecoveryBranch::TerminalBack44, 1).directions
              == qinput::kLogicalRight);
    TrainingRecoveryController controller;
    controller.reset(0);
    const RecoverySettings settings = recoveryArmed(RecoveryBranch::Ground88);
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Ground88);
    observation.directionRingHead = 0;

    uint64_t serial = 0;
    uint32_t upSteps = 0;
    bool sawTap2 = false;
    for (uint32_t step = 0; step < 12u; ++step) {
        const RecoveryEmission emission =
            controller.advance(++serial, settings, observation);
        if (emission.directions & qinput::kLogicalUp) ++upSteps;
        if (emission.state == RecoveryState::EmitTap2) {
            QNP_CHECK(emission.directions == qinput::kLogicalUp);
            sawTap2 = true;
            continue;
        }
        if (sawTap2) {
            QNP_CHECK((emission.directions & qinput::kLogicalUp) == 0u);
            QNP_CHECK(emission.directions == 0u);
        }
    }
    QNP_CHECK(sawTap2);
    QNP_CHECK(upSteps == 2u);
}

QNP_TEST(TrainingRecoveryWall66IsFacingNormalizedInBothCorners) {
    for (uint8_t facing = 0; facing <= 1u; ++facing) {
        const uint32_t expected =
            facing == 0 ? qinput::kLogicalRight : qinput::kLogicalLeft;
        for (const uint16_t wallSide : {uint16_t{1}, uint16_t{2}}) {
            TrainingRecoveryController controller;
            controller.reset(0);
            const RecoverySettings settings =
                recoveryArmed(RecoveryBranch::Wall66);
            RecoveryObservation observation =
                recoveryEligibleFor(RecoveryBranch::Wall66);
            observation.facing = facing;
            observation.wallFlags = wallSide;
            observation.directionRingHead = 0;

            uint64_t serial = 0;
            const RecoveryEmission tap1 =
                controller.advance(++serial, settings, observation);
            const RecoveryEmission gap =
                controller.advance(++serial, settings, observation);
            const RecoveryEmission tap2 =
                controller.advance(++serial, settings, observation);
            if (tap1.directions != expected || tap2.directions != expected) {
                std::printf(
                    "    [recovery] wall facing=%u side=%u taps %08X/%08X,"
                    " want %08X\n",
                    facing, wallSide, tap1.directions, tap2.directions,
                    expected);
            }
            QNP_CHECK(tap1.state == RecoveryState::EmitTap1);
            QNP_CHECK(tap1.directions == expected);
            QNP_CHECK(gap.directions == 0u);
            QNP_CHECK(tap2.state == RecoveryState::EmitTap2);
            QNP_CHECK(tap2.directions == expected);
            QNP_CHECK((tap1.directions & qinput::kLogicalDirectionMask)
                      == tap1.directions);
        }
    }

    TrainingRecoveryController flipping;
    flipping.reset(0);
    const RecoverySettings settings = recoveryArmed(RecoveryBranch::Wall66);
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Wall66);
    observation.directionRingHead = 0;
    uint64_t serial = 0;
    QNP_CHECK(flipping.advance(++serial, settings, observation).directions
              == qinput::kLogicalRight);
    observation.facing = 1;
    QNP_CHECK(flipping.advance(++serial, settings, observation).directions
              == 0u);
    const RecoveryEmission flipped =
        flipping.advance(++serial, settings, observation);
    QNP_CHECK(flipped.state == RecoveryState::EmitTap2);
    QNP_CHECK(flipped.directions == qinput::kLogicalLeft);
    QNP_CHECK(flipping.counters().cancelled == 0u);
}

QNP_TEST(TrainingRecoveryGenerationChangeCancelsTheAttempt) {
    struct CancelCase {
        const char* name;
        uint32_t action;
        uint8_t slot;
        uint32_t commandChild;
        uint8_t posture;
        bool comControlled;
    };
    const CancelCase cases[] = {
        {"action left the eligible set", 0u, 1u, 0x00401000u, 1u, false},
        {"defender slot changed", 25u, 0u, 0x00401000u, 1u, false},
        {"command history child replaced", 25u, 1u, 0x00500000u, 1u, false},
        {"posture left the manual gate", 25u, 1u, 0x00401000u, 3u, false},
        {"the COM took the fighter", 25u, 1u, 0x00401000u, 1u, true},
    };
    for (const CancelCase& entry : cases) {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        observation.directionRingHead = 0;
        uint64_t serial = 0;
        const RecoveryEmission tap1 =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(tap1.active);
        QNP_CHECK(tap1.directions == qinput::kLogicalDown);

        observation.action = entry.action;
        observation.slot = entry.slot;
        observation.commandChild = entry.commandChild;
        observation.postureNibble = entry.posture;
        observation.comControlled = entry.comControlled;
        const RecoveryEmission cancelled =
            controller.advance(++serial, settings, observation);
        if (cancelled.active || cancelled.directions != 0u) {
            std::printf("    [recovery] %s still emitted %08X\n",
                        entry.name, cancelled.directions);
        }
        QNP_CHECK(!cancelled.active);
        QNP_CHECK(cancelled.directions == 0u);
        QNP_CHECK(cancelled.lastVerdict == RecoveryVerdict::Cancelled);
        QNP_CHECK(cancelled.verdictIsNew);
        QNP_CHECK(controller.counters().cancelled == 1u);
        QNP_CHECK(controller.counters().succeeded == 0u);
    }

    {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        observation.directionRingHead = 0;
        uint64_t serial = 0;
        QNP_CHECK(controller.advance(++serial, settings, observation).directions
                  == qinput::kLogicalDown);
        observation.recoveryLock = 1;
        const RecoveryEmission suspended =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(suspended.state == RecoveryState::ObservingBuffer);
        QNP_CHECK(!suspended.active);
        QNP_CHECK(controller.counters().cancelled == 0u);
        QNP_CHECK(controller.counters().missed == 0u);
        QNP_CHECK(suspended.lastVerdict == RecoveryVerdict::None);
        QNP_CHECK(suspended.stepsRemaining > 0u);
        for (uint32_t step = 0; step < kRecoveryWindowAir - 3u; ++step) {
            const RecoveryEmission parked =
                controller.advance(++serial, settings, observation);
            QNP_CHECK(parked.state == RecoveryState::ObservingBuffer);
            QNP_CHECK(!parked.active);
            QNP_CHECK(controller.counters().missed == 0u);
        }
        observation.recoveryLock = 0;
        observation.action = 31;
        const RecoveryEmission late =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(late.lastVerdict == RecoveryVerdict::BufferedObserved);
        QNP_CHECK(controller.counters().bufferedObserved == 1u);
        QNP_CHECK(controller.counters().missed == 0u);
    }

    struct OwnershipCase {
        const char* name;
        bool lifecycleAvailable;
        bool sandboxActive;
        bool menuCaptured;
        bool operationNeutral;
        bool macroPlaybackActive;
        bool macroOverrideP1;
    };
    const OwnershipCase ownership[] = {
        {"lifecycle unavailable", false, true, false, false, false, false},
        {"sandbox revoked", true, false, false, false, false, false},
        {"menu captured", true, true, true, false, false, false},
        {"operation neutral", true, true, false, true, false, false},
        {"macro playback", true, true, false, false, true, false},
        {"macro record routing", true, true, false, false, false, true},
    };
    for (const OwnershipCase& entry : ownership) {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        observation.directionRingHead = 0;
        uint64_t serial = 0;
        QNP_CHECK(controller.advance(++serial, settings, observation).active);
        observation.lifecycleAvailable = entry.lifecycleAvailable;
        observation.sandboxActive = entry.sandboxActive;
        observation.menuCaptured = entry.menuCaptured;
        observation.operationNeutral = entry.operationNeutral;
        observation.macroPlaybackActive = entry.macroPlaybackActive;
        observation.macroOverrideP1 = entry.macroOverrideP1;
        const RecoveryEmission lost =
            controller.advance(++serial, settings, observation);
        if (lost.active) std::printf("    [recovery] %s\n", entry.name);
        QNP_CHECK(!lost.active);
        QNP_CHECK(lost.directions == 0u);
        QNP_CHECK(lost.lastVerdict == RecoveryVerdict::Cancelled);
        QNP_CHECK(controller.counters().cancelled == 1u);
    }

    {
        TrainingRecoveryController controller;
        controller.reset(1);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        observation.trainingGeneration = 1;
        observation.directionRingHead = 0;
        uint64_t serial = 0;
        QNP_CHECK(controller.advance(++serial, settings, observation).active);
        QNP_CHECK(controller.counters().attempts == 1u);
        observation.trainingGeneration = 2;
        const RecoveryEmission regenerated =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(controller.counters().attempts == 1u);
        QNP_CHECK(controller.rngState() == recoverySeedFor(2));
        QNP_CHECK(regenerated.state == RecoveryState::EmitTap1);
    }

    {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        const RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        QNP_CHECK(controller.advance(10u, settings, observation).valid);
        const RecoveryEmission backwards =
            controller.advance(9u, settings, observation);
        QNP_CHECK(!backwards.valid);
        QNP_CHECK(!backwards.active);
        QNP_CHECK(backwards.lastVerdict == RecoveryVerdict::Cancelled);
    }
}

QNP_TEST(TrainingRecoveryLockNeverSuspendsTheTerminalFamily) {
    QNP_CHECK(recoveryBranchConsultsManualLock(RecoveryBranch::Air22));
    QNP_CHECK(recoveryBranchConsultsManualLock(RecoveryBranch::Ground88));
    QNP_CHECK(recoveryBranchConsultsManualLock(RecoveryBranch::Wall66));
    QNP_CHECK(!recoveryBranchConsultsManualLock(
        RecoveryBranch::TerminalForward66));
    QNP_CHECK(!recoveryBranchConsultsManualLock(RecoveryBranch::TerminalBack44));

    const RecoveryBranch exempt[] = {
        RecoveryBranch::TerminalForward66,
        RecoveryBranch::TerminalBack44,
    };
    for (RecoveryBranch branch : exempt) {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(branch);
        RecoveryObservation observation = recoveryEligibleFor(branch);
        observation.recoveryLock = 1;
        QNP_CHECK((recoveryArmableMask(observation)
                   & recoveryBranchBit(branch)) != 0u);

        uint64_t serial = 0;
        const uint32_t steps = recoveryEmissionStepCount(branch);
        uint32_t emitted = 0;
        for (uint32_t step = 0; step < steps; ++step) {
            const RecoveryEmission out =
                controller.advance(++serial, settings, observation);
            if (out.active) ++emitted;
            QNP_CHECK(out.lastVerdict != RecoveryVerdict::Cancelled);
        }
        if (emitted == 0u) {
            std::printf("    [recovery] %s emitted nothing under the lock\n",
                        recoveryBranchName(branch));
        }
        QNP_CHECK(emitted == steps);
        QNP_CHECK(controller.counters().attempts == 1u);
        QNP_CHECK(controller.counters().cancelled == 0u);
    }

    {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        observation.directionRingHead = 0;
        uint64_t serial = 0;
        QNP_CHECK(controller.advance(++serial, settings, observation).active);
        observation.recoveryLock = 1;
        const RecoveryEmission suspended =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(suspended.state == RecoveryState::ObservingBuffer);
        QNP_CHECK(!suspended.active);
    }
}

QNP_TEST(TrainingRecoveryClaimsAResultItsOwnTapsProduced) {
    TrainingRecoveryController controller;
    controller.reset(0);
    const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Air22);
    observation.directionRingHead = qinput::kLogicalDown;

    uint64_t serial = 0;
    const RecoveryEmission preNeutral =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(preNeutral.state == RecoveryState::EmitPreNeutral);
    QNP_CHECK(preNeutral.directions == 0u);
    const RecoveryEmission tap1 =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(tap1.state == RecoveryState::EmitTap1);
    QNP_CHECK(tap1.directions == qinput::kLogicalDown);

    observation.action = 31;
    const RecoveryEmission early =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(early.lastVerdict == RecoveryVerdict::Succeeded);
    QNP_CHECK(early.verdictIsNew);
    QNP_CHECK(controller.counters().succeeded == 1u);
    QNP_CHECK(controller.counters().automaticObserved == 0u);
    QNP_CHECK(controller.counters().cancelled == 0u);
    QNP_CHECK(controller.counters().lastResultAction == 31u);
    QNP_CHECK(!early.active);

    {
        TrainingRecoveryController other;
        other.reset(0);
        const RecoverySettings delayed =
            recoveryArmed(RecoveryBranch::Air22, ManualRecoveryMode::FixedDelay);
        RecoverySettings withDelay = delayed;
        withDelay.delaySteps = 6;
        RecoveryObservation obs = recoveryEligibleFor(RecoveryBranch::Air22);
        obs.directionRingHead = 0;
        uint64_t step = 0;
        const RecoveryEmission waiting =
            other.advance(++step, withDelay, obs);
        QNP_CHECK(waiting.state == RecoveryState::DelayEligibleSteps);
        obs.action = 31;
        const RecoveryEmission automatic = other.advance(++step, withDelay, obs);
        QNP_CHECK(other.counters().automaticObserved == 1u);
        QNP_CHECK(other.counters().succeeded == 0u);
        QNP_CHECK(automatic.lastVerdict == RecoveryVerdict::Cancelled);
        QNP_CHECK(other.counters().cancelled == 1u);
    }
}

QNP_TEST(TrainingRecoveryTimelineResetKeepsTheObservationCounters) {
    TrainingRecoveryController controller;
    controller.reset(3);
    const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Air22);
    observation.trainingGeneration = 3;
    observation.directionRingHead = 0;

    uint64_t serial = 0;
    QNP_CHECK(controller.advance(++serial, settings, observation).active);
    QNP_CHECK(controller.counters().attempts == 1u);
    const uint64_t rngBefore = controller.rngState();

    controller.resetTimeline();
    QNP_CHECK(controller.counters().attempts == 1u);
    QNP_CHECK(controller.rngState() == rngBefore);

    const RecoveryEmission afterLoad =
        controller.advance(serial - 1u, settings, observation);
    QNP_CHECK(afterLoad.valid);
    QNP_CHECK(afterLoad.state == RecoveryState::EmitTap1);
    QNP_CHECK(controller.counters().attempts == 2u);

    controller.reset(4);
    QNP_CHECK(controller.counters().attempts == 0u);
    QNP_CHECK(controller.rngState() == recoverySeedFor(4));
}

QNP_TEST(TrainingRecoveryTimeoutEndsMissedAndNeverForcesTheResultAction) {
    TrainingRecoveryController controller;
    controller.reset(0);
    const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Air22);
    observation.directionRingHead = 0;

    uint64_t serial = 0;
    bool sawMiss = false;
    for (uint32_t step = 0; step < kRecoveryWindowAir + 8u; ++step) {
        const RecoveryEmission emission =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(emission.valid);
        QNP_CHECK((emission.directions & ~qinput::kLogicalDirectionMask) == 0u);
        QNP_CHECK((emission.buttons & ~qinput::kLogicalButtonMask) == 0u);
        if (emission.lastVerdict == RecoveryVerdict::MissedExpired
            && emission.verdictIsNew) {
            sawMiss = true;
            QNP_CHECK(!emission.active);
            QNP_CHECK(emission.directions == 0u);
            break;
        }
    }
    QNP_CHECK(sawMiss);
    QNP_CHECK(controller.counters().missed == 1u);
    QNP_CHECK(controller.counters().succeeded == 0u);
    QNP_CHECK(controller.counters().bufferedObserved == 0u);
    QNP_CHECK(controller.counters().automaticObserved == 0u);
    QNP_CHECK(controller.counters().lastResultAction == 0u);
    QNP_CHECK(controller.counters().attempts == 1u);

    QNP_CHECK(recoveryBranchAcceptsResult(RecoveryBranch::Air22, 31));
    QNP_CHECK(!recoveryBranchAcceptsResult(RecoveryBranch::Air22, 33));
    QNP_CHECK(recoveryBranchAcceptsResult(RecoveryBranch::Ground88, 33));
    QNP_CHECK(recoveryBranchAcceptsResult(RecoveryBranch::Wall66, 33));
    QNP_CHECK(recoveryBranchAcceptsResult(RecoveryBranch::Wall66, 34));
    QNP_CHECK(recoveryBranchAcceptsResult(
        RecoveryBranch::TerminalForward66, 13));
    QNP_CHECK(recoveryBranchAcceptsResult(RecoveryBranch::TerminalBack44, 14));
    QNP_CHECK(!recoveryBranchAcceptsResult(RecoveryBranch::ThrowEscape, 70));
    QNP_CHECK(!recoveryBranchAcceptsResult(RecoveryBranch::ThrowEscape, 73));
    QNP_CHECK(!recoveryBranchAcceptsResult(RecoveryBranch::ThrowEscape, 33));
    for (const uint32_t action : {13u, 14u, 31u, 33u, 34u})
        QNP_CHECK(isRecoveryResultAction(action));
    for (const uint32_t action : {0u, 25u, 35u, 37u, 38u, 56u, 70u, 73u})
        QNP_CHECK(!isRecoveryResultAction(action));
}

QNP_TEST(TrainingRecoveryRetriesWhileTheBranchStaysArmableAfterAMiss) {
    TrainingRecoveryController controller;
    controller.reset(0);
    const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Air22);
    observation.directionRingHead = 0;

    uint64_t serial = 0;
    bool sawMiss = false;
    for (uint32_t step = 0; step < kRecoveryWindowAir + 8u; ++step) {
        const RecoveryEmission emission =
            controller.advance(++serial, settings, observation);
        if (emission.lastVerdict == RecoveryVerdict::MissedExpired
            && emission.verdictIsNew) {
            sawMiss = true;
            break;
        }
    }
    QNP_REQUIRE(sawMiss);
    QNP_CHECK(controller.counters().attempts == 1u);

    for (uint32_t step = 0; step + 1u < kRecoveryRetryGapSteps; ++step) {
        const RecoveryEmission emission =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(!emission.active);
    }
    QNP_CHECK(controller.counters().attempts == 1u);
    bool rearmed = false;
    for (uint32_t step = 0; step < 4u; ++step) {
        const RecoveryEmission emission =
            controller.advance(++serial, settings, observation);
        if (emission.active && emission.directions != 0u) {
            rearmed = true;
            break;
        }
    }
    QNP_CHECK(rearmed);
    QNP_CHECK(controller.counters().attempts == 2u);

    RecoveryObservation notArmable = observation;
    notArmable.permission = 0;
    for (uint32_t step = 0; step < kRecoveryWindowAir + 8u; ++step)
        (void)controller.advance(++serial, settings, notArmable);
    const uint32_t before = controller.counters().attempts;
    const RecoveryEmission edge =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(controller.counters().attempts == before + 1u);
    (void)edge;
}

QNP_TEST(TrainingRecoveryANewHitIsANewEpisodeWithoutAnEdge) {
    TrainingRecoveryController controller;
    controller.reset(0);
    const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Air22);
    observation.directionRingHead = 0;
    observation.hitEpoch = 1u;

    uint64_t serial = 0;
    bool sawMiss = false;
    for (uint32_t step = 0; step < kRecoveryWindowAir + 8u; ++step) {
        const RecoveryEmission emission =
            controller.advance(++serial, settings, observation);
        if (emission.lastVerdict == RecoveryVerdict::MissedExpired
            && emission.verdictIsNew) {
            sawMiss = true;
            break;
        }
    }
    QNP_REQUIRE(sawMiss);
    QNP_CHECK(controller.counters().attempts == 1u);

    observation.hitEpoch = 2u;
    const RecoveryEmission rearmed =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(controller.counters().attempts == 2u);
    QNP_CHECK(rearmed.active);

    TrainingRecoveryController legacy;
    legacy.reset(0);
    RecoveryObservation unknownEpoch =
        recoveryEligibleFor(RecoveryBranch::Air22);
    unknownEpoch.directionRingHead = 0;
    serial = 0;
    for (uint32_t step = 0; step < 3u; ++step)
        (void)legacy.advance(++serial, settings, unknownEpoch);
    QNP_CHECK(legacy.counters().attempts == 1u);
}

QNP_TEST(TrainingRecoveryEvaluatorReasonIsATotalFunctionOverItsThreeChecks) {
    QNP_CHECK(recoveryEvaluatorReasonFor(false, false, false)
              == RecoveryEvaluatorReason::NotAttempted);
    QNP_CHECK(recoveryEvaluatorReasonFor(false, true, true)
              == RecoveryEvaluatorReason::NotAttempted);
    QNP_CHECK(recoveryEvaluatorReasonFor(true, false, false)
              == RecoveryEvaluatorReason::AddressUnresolved);
    QNP_CHECK(recoveryEvaluatorReasonFor(true, false, true)
              == RecoveryEvaluatorReason::AddressUnresolved);
    QNP_CHECK(recoveryEvaluatorReasonFor(true, true, false)
              == RecoveryEvaluatorReason::PrologueMismatch);
    QNP_CHECK(recoveryEvaluatorReasonFor(true, true, true)
              == RecoveryEvaluatorReason::Qualified);

    QNP_CHECK(nativeRecoveryReleaseQualified(ManualRecoveryMode::Off, false));
    QNP_CHECK(!nativeRecoveryReleaseQualified(
        ManualRecoveryMode::Earliest, false));
    const RecoverySettings shipped{};
    QNP_CHECK(!shipped.evaluatorQualified);
    QNP_CHECK(recoveryEvaluatorReasonFor(shipped.evaluatorQualified,
                                         shipped.evaluatorQualified,
                                         shipped.evaluatorQualified)
              == RecoveryEvaluatorReason::NotAttempted);
}

QNP_TEST(TrainingRecoveryStandDownNamesEveryTermThatZeroesTheMasks) {
    const RecoveryObservation clean =
        recoveryEligibleFor(RecoveryBranch::Air22);
    QNP_CHECK(recoveryStandDownMask(clean) == RecoveryStandDownNone);
    QNP_CHECK(isCanonicalRecoveryStandDown(recoveryStandDownMask(clean)));

    struct Case {
        uint32_t bit;
        bool RecoveryObservation::*flag;
        bool value;
    };
    const Case cases[] = {
        { RecoveryStandDownNotReadable,
          &RecoveryObservation::readable, false },
        { RecoveryStandDownLifecycleDown,
          &RecoveryObservation::lifecycleAvailable, false },
        { RecoveryStandDownSandboxInactive,
          &RecoveryObservation::sandboxActive, false },
        { RecoveryStandDownMenuCaptured,
          &RecoveryObservation::menuCaptured, true },
        { RecoveryStandDownOperationNeutral,
          &RecoveryObservation::operationNeutral, true },
        { RecoveryStandDownMacroPlayback,
          &RecoveryObservation::macroPlaybackActive, true },
        { RecoveryStandDownMacroOwnsP1,
          &RecoveryObservation::macroOverrideP1, true },
        { RecoveryStandDownComControlled,
          &RecoveryObservation::comControlled, true },
        { RecoveryStandDownDirectionSuppressed,
          &RecoveryObservation::directionSuppressed, true },
        { RecoveryStandDownKoLatch,
          &RecoveryObservation::koLatch, true },
    };
    uint32_t seen = 0;
    for (const Case& item : cases) {
        RecoveryObservation broken = clean;
        broken.*(item.flag) = item.value;
        QNP_CHECK(recoveryStandDownMask(broken) == item.bit);
        seen |= item.bit;

        const bool commonGate =
            (item.bit & kRecoveryStandDownCommonGateMask) != 0u;
        if (commonGate || item.bit == RecoveryStandDownNotReadable) {
            QNP_CHECK(recoveryArmableMask(broken) == 0u);
            QNP_CHECK(recoveryEligibleMask(broken) == 0u);
        } else {
            QNP_CHECK(recoveryArmableMask(broken)
                      == recoveryArmableMask(clean));
        }
    }
    QNP_CHECK(seen == kRecoveryStandDownMask);
    QNP_CHECK((kRecoveryStandDownAvailabilityMask
               & kRecoveryStandDownCommonGateMask) == 0u);
    QNP_CHECK((kRecoveryStandDownAvailabilityMask
               | kRecoveryStandDownCommonGateMask)
              == kRecoveryStandDownMask);

    RecoveryObservation several = clean;
    several.menuCaptured = true;
    several.koLatch = true;
    several.comControlled = true;
    QNP_CHECK(recoveryStandDownMask(several)
              == (RecoveryStandDownMenuCaptured | RecoveryStandDownKoLatch
                  | RecoveryStandDownComControlled));

    {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        RecoveryObservation held = clean;
        held.menuCaptured = true;
        const RecoveryEmission standDown = controller.advance(1, settings, held);
        QNP_CHECK(standDown.standDownMask == RecoveryStandDownMenuCaptured);
        QNP_CHECK(standDown.eligibleMask == 0u);
        QNP_CHECK(standDown.armableMask == 0u);
        QNP_CHECK(standDown.state == RecoveryState::Idle);
        QNP_CHECK(!standDown.active);

        const RecoveryEmission running = controller.advance(2, settings, clean);
        QNP_CHECK(running.standDownMask == RecoveryStandDownNone);
        QNP_CHECK(running.armableMask != 0u);

        RecoveryObservation ko = clean;
        ko.koLatch = true;
        const RecoveryEmission blocked = controller.advance(3, settings, ko);
        QNP_CHECK(blocked.standDownMask == RecoveryStandDownKoLatch);
        QNP_CHECK(blocked.eligibleMask == 0u);
        QNP_CHECK(blocked.armableMask == 0u);
    }
}

QNP_TEST(TrainingRecoveryPublishesTheLatchedTapCountBesideTheVerdict) {
    TrainingRecoveryController controller;
    controller.reset(0);
    RecoverySettings settings =
        recoveryArmed(RecoveryBranch::Air22, ManualRecoveryMode::FixedDelay);
    settings.delaySteps = 2;
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Air22);
    observation.directionRingHead = 0;

    uint64_t serial = 0;
    RecoveryEmission emission = controller.advance(++serial, settings,
                                                   observation);
    QNP_CHECK(emission.state == RecoveryState::DelayEligibleSteps);
    QNP_CHECK(emission.tapsEmitted == 0u);
    emission = controller.advance(++serial, settings, observation);
    QNP_CHECK(emission.state == RecoveryState::DelayEligibleSteps);
    QNP_CHECK(emission.tapsEmitted == 0u);
    emission = controller.advance(++serial, settings, observation);
    QNP_CHECK(emission.state == RecoveryState::EmitTap1);
    QNP_CHECK(emission.tapsEmitted == 1u);
    emission = controller.advance(++serial, settings, observation);
    QNP_CHECK(emission.state == RecoveryState::EmitNeutral);
    QNP_CHECK(emission.tapsEmitted == 1u);
    emission = controller.advance(++serial, settings, observation);
    QNP_CHECK(emission.state == RecoveryState::EmitTap2);
    QNP_CHECK(emission.tapsEmitted == 2u);

    bool sawMiss = false;
    for (uint32_t step = 0; step < kRecoveryWindowAir + 8u; ++step) {
        emission = controller.advance(++serial, settings, observation);
        if (emission.lastVerdict == RecoveryVerdict::MissedExpired
            && emission.verdictIsNew) {
            sawMiss = true;
            QNP_CHECK(emission.tapsEmitted == 2u);
            break;
        }
        QNP_CHECK(emission.tapsEmitted == 2u);
    }
    QNP_CHECK(sawMiss);
    emission = controller.advance(++serial, settings, observation);
    QNP_CHECK(emission.lastVerdict == RecoveryVerdict::MissedExpired);
    QNP_CHECK(emission.tapsEmitted == 2u);

    {
        TrainingRecoveryController fresh;
        fresh.reset(0);
        RecoverySettings delayed =
            recoveryArmed(RecoveryBranch::Air22, ManualRecoveryMode::FixedDelay);
        delayed.delaySteps = 4;
        uint64_t freshSerial = 0;
        const RecoveryEmission armed =
            fresh.advance(++freshSerial, delayed, observation);
        QNP_CHECK(armed.state == RecoveryState::DelayEligibleSteps);
        QNP_CHECK(armed.tapsEmitted == 0u);
    }

    controller.resetTimeline();
    const RecoveryEmission afterReset =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(afterReset.lastVerdict == RecoveryVerdict::None);
    QNP_CHECK(afterReset.tapsEmitted == 0u);
}

QNP_TEST(TrainingRecoveryPublishesArmableBesideEligible) {
    TrainingRecoveryController controller;
    controller.reset(0);
    const RecoverySettings settings{};
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::TerminalForward66);

    const RecoveryEmission aligned = controller.advance(1, settings,
                                                        observation);
    QNP_CHECK(aligned.eligibleMask == recoveryEligibleMask(observation));
    QNP_CHECK(aligned.armableMask == recoveryArmableMask(observation));
    QNP_CHECK(aligned.armableMask != 0u);

    observation.directionRingHead = 0;
    const RecoveryEmission adrift = controller.advance(2, settings,
                                                       observation);
    QNP_CHECK((adrift.eligibleMask
               & recoveryBranchBit(RecoveryBranch::TerminalForward66)) == 0u);
    QNP_CHECK((adrift.armableMask
               & recoveryBranchBit(RecoveryBranch::TerminalForward66)) != 0u);
    QNP_CHECK(adrift.eligibleMask != adrift.armableMask);
    QNP_CHECK((adrift.eligibleMask & adrift.armableMask)
              == adrift.eligibleMask);

    RecoveryObservation held = observation;
    held.menuCaptured = true;
    const RecoveryEmission standDown = controller.advance(3, settings, held);
    QNP_CHECK(standDown.eligibleMask == 0u);
    QNP_CHECK(standDown.armableMask == 0u);
    QNP_CHECK(standDown.standDownMask == RecoveryStandDownMenuCaptured);
}

QNP_TEST(TrainingRecoveryNeverRequestsAResultActionDirectly) {
    const RecoveryBranch liveBranches[] = {
        RecoveryBranch::Air22,
        RecoveryBranch::Ground88,
        RecoveryBranch::Wall66,
        RecoveryBranch::TerminalForward66,
        RecoveryBranch::TerminalBack44,
    };
    for (const RecoveryBranch branch : liveBranches) {
        for (uint32_t index = 0; index < 8u; ++index) {
            for (uint8_t facing = 0; facing <= 1u; ++facing) {
                const RecoveryStepMask step =
                    recoveryEmissionStep(branch, index, facing);
                const RecoveryStepMask await =
                    recoveryAwaitMask(branch, facing);
                for (const RecoveryStepMask& mask : {step, await}) {
                    QNP_CHECK((mask.directions
                               & ~qinput::kLogicalDirectionMask) == 0u);
                    QNP_CHECK((mask.buttons
                               & ~qinput::kLogicalButtonMask) == 0u);
                    const uint32_t combined = mask.directions | mask.buttons;
                    QNP_CHECK(
                        qinput::isCanonicalLogicalFightInput(combined));
                    QNP_CHECK(qinput::logicalFromCompact(
                                  qinput::compactFromLogical(combined))
                              == combined);
                    for (const uint32_t forbidden : {31u, 33u, 70u, 73u}) {
                        QNP_CHECK(mask.directions != forbidden);
                        QNP_CHECK(mask.buttons != forbidden);
                        QNP_CHECK(combined != forbidden);
                    }
                }
            }
        }
    }
    QNP_CHECK(isCanonicalDummyOwnedButtons(0u));
    QNP_CHECK(isCanonicalDummyOwnedButtons(qinput::kLogicalButtonMask));
    QNP_CHECK(!isCanonicalDummyOwnedButtons(qinput::kLogicalC));
    QNP_CHECK(!isCanonicalDummyOwnedButtons(
        qinput::kLogicalC | qinput::kLogicalD));
    for (uint8_t facing = 0; facing <= 1u; ++facing) {
        for (const RecoveryBranch branch : liveBranches) {
            for (uint32_t index = 0; index < 4u; ++index) {
                QNP_CHECK(recoveryEmissionStep(branch, index,
                                               facing).buttons == 0u);
            }
        }
    }
    DummyPostureInput input{};
    input.recoveryActive = true;
    input.recoveryDirections = qinput::kLogicalLeft;
    input.recoveryButtons = qinput::kLogicalC;
    input.recoveryOwnsButtons = false;
    QNP_CHECK(!chooseDummyPosture(input).valid);
    input.recoveryOwnsButtons = true;
    const DummyPostureResult escape = chooseDummyPosture(input);
    QNP_CHECK(escape.valid);
    QNP_CHECK(escape.buttons == qinput::kLogicalC);
    QNP_CHECK(escape.ownedButtons == qinput::kLogicalButtonMask);
    QNP_CHECK(escape.ownedAxes
              == (kDummyHorizontalAxis | kDummyVerticalAxis));
    QNP_CHECK(escape.horizontalSource == DummyPostureSource::Recovery);
    QNP_CHECK(escape.verticalSource == DummyPostureSource::Recovery);
    QNP_CHECK(static_cast<uint32_t>(DummyPostureSource::Recovery) == 4u);
    input.recoveryDirections = qinput::kLogicalLeft | 0x100000u;
    QNP_CHECK(!chooseDummyPosture(input).valid);
    input.recoveryDirections = qinput::kLogicalLeft;
    input.recoveryButtons = 0x10u;
    QNP_CHECK(!chooseDummyPosture(input).valid);
}

QNP_TEST(TrainingRecoveryOutranksGuardAndStanceOnItsOwnSteps) {
    for (uint32_t modeRaw = 0; modeRaw <= 4u; ++modeRaw) {
        for (uint32_t stanceRaw = 0; stanceRaw <= 2u; ++stanceRaw) {
            DummyPostureInput input{};
            input.guardMode = static_cast<GuardMode>(modeRaw);
            input.stance = static_cast<DummyStance>(stanceRaw);
            input.guarding = true;
            input.guardDirections =
                qinput::kLogicalLeft | qinput::kLogicalDown;
            input.recoveryActive = true;
            input.recoveryDirections = 0;
            const DummyPostureResult result = chooseDummyPosture(input);
            QNP_CHECK(result.valid);
            QNP_CHECK(result.directions == 0u);
            QNP_CHECK(result.ownedAxes
                      == (kDummyHorizontalAxis | kDummyVerticalAxis));
            QNP_CHECK(result.horizontalSource == DummyPostureSource::Recovery);
            QNP_CHECK(result.verticalSource == DummyPostureSource::Recovery);
            QNP_CHECK(result.buttons == 0u);
            QNP_CHECK(result.ownedButtons == 0u);

            input.recoveryDirections = qinput::kLogicalDown;
            const DummyPostureResult tap = chooseDummyPosture(input);
            QNP_CHECK(tap.valid);
            QNP_CHECK(tap.directions == qinput::kLogicalDown);
        }
    }
    TrainingInput input;
    input.reset();
    uint64_t serial = 0;
    armInput(input, serial);
    TrainingInputStep step{};
    step.lifecycleAvailable = true;
    step.dummyExclusive = true;
    step.macroPlaybackActive = true;
    step.macroOverrideP2 = true;
    step.dummyOwnedAxes = kDummyHorizontalAxis | kDummyVerticalAxis;
    step.dummyDirections = qinput::kLogicalDown;
    QNP_CHECK(!input.advance(++serial, step).valid);
    step.macroPlaybackActive = false;
    step.macroOverrideP2 = false;
    step.dummyOwnedAxes = kDummyVerticalAxis;
    QNP_CHECK(!input.advance(++serial, step).valid);
    step.dummyOwnedAxes = kDummyHorizontalAxis | kDummyVerticalAxis;
    const TrainingInputResult ok = input.advance(++serial, step);
    QNP_CHECK(ok.valid);
    QNP_CHECK(ok.p2Mask == qinput::kLogicalDown);
    QNP_CHECK(ok.p1Mask == 0u);
    step.p2PhysicalMask = qinput::kLogicalD | qinput::kLogicalRight;
    step.dummyButtons = qinput::kLogicalC;
    step.dummyOwnedButtons = qinput::kLogicalButtonMask;
    step.dummyDirections = qinput::kLogicalLeft;
    const TrainingInputResult ownedButtons = input.advance(++serial, step);
    QNP_CHECK(ownedButtons.valid);
    QNP_CHECK(ownedButtons.p2Mask
              == (qinput::kLogicalLeft | qinput::kLogicalC));
}

QNP_TEST(TrainingRecoveryClassifiesAutomaticSuccessAndBufferedSeparately) {
    {
        TrainingRecoveryController controller;
        controller.reset(0);
        RecoverySettings off{};
        off.evaluatorQualified = true;
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        uint64_t serial = 0;
        for (uint32_t step = 0; step < 4u; ++step) {
            const RecoveryEmission emission =
                controller.advance(++serial, off, observation);
            QNP_CHECK(!emission.active);
            QNP_CHECK(emission.directions == 0u);
            QNP_CHECK(emission.eligibleMask
                      == recoveryBranchBit(RecoveryBranch::Air22));
        }
        observation.action = 31;
        const RecoveryEmission automatic =
            controller.advance(++serial, off, observation);
        QNP_CHECK(automatic.lastVerdict == RecoveryVerdict::AutomaticObserved);
        QNP_CHECK(automatic.verdictIsNew);
        QNP_CHECK(controller.counters().automaticObserved == 1u);
        QNP_CHECK(controller.counters().lastResultAction == 31u);
        QNP_CHECK(controller.counters().attempts == 0u);
        QNP_CHECK(controller.counters().succeeded == 0u);
        QNP_CHECK(!automatic.active);
    }

    {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        observation.directionRingHead = 0;
        uint64_t serial = 0;
        for (uint32_t step = 0; step < 3u; ++step)
            (void)controller.advance(++serial, settings, observation);
        observation.action = 31;
        const RecoveryEmission success =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(success.lastVerdict == RecoveryVerdict::Succeeded);
        QNP_CHECK(controller.counters().succeeded == 1u);
        QNP_CHECK(controller.counters().automaticObserved == 0u);
        QNP_CHECK(controller.counters().bufferedObserved == 0u);
        QNP_CHECK(controller.counters().lastResultAction == 31u);
    }

    {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        observation.directionRingHead = 0;
        uint64_t serial = 0;
        for (uint32_t step = 0; step < 3u + kRecoveryAwaitCapSteps + 1u;
             ++step) {
            (void)controller.advance(++serial, settings, observation);
        }
        observation.action = 31;
        const RecoveryEmission buffered =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(buffered.lastVerdict == RecoveryVerdict::BufferedObserved);
        QNP_CHECK(controller.counters().bufferedObserved == 1u);
        QNP_CHECK(controller.counters().succeeded == 0u);
        QNP_CHECK(controller.counters().automaticObserved == 0u);
    }

    {
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Air22);
        observation.directionRingHead = 0;
        uint64_t serial = 0;
        for (uint32_t step = 0; step < 3u; ++step)
            (void)controller.advance(++serial, settings, observation);
        observation.transitionFlag = 1;
        observation.nextAction = 31;
        observation.koLatch = true;
        const RecoveryEmission blocked =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(controller.counters().commitBlocked == 1u);
        QNP_CHECK(controller.counters().missed == 0u);
        QNP_CHECK(controller.counters().succeeded == 0u);
        QNP_CHECK(blocked.lastVerdict == RecoveryVerdict::Cancelled);
        QNP_CHECK(controller.counters().cancelled == 1u);
        QNP_CHECK(!blocked.active);
    }

    QNP_CHECK(static_cast<uint32_t>(RecoveryVerdict::None) == 0u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryVerdict::Succeeded) == 1u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryVerdict::MissedExpired) == 2u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryVerdict::Cancelled) == 3u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryVerdict::BufferedObserved) == 4u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryVerdict::AutomaticObserved) == 5u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryVerdict::CommitBlocked) == 6u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryState::Idle) == 0u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryState::EmitPreNeutral) == 3u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryState::AwaitNativeTransition) == 7u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryState::ObservingBuffer) == 8u);
}

QNP_TEST(TrainingRecoveryDelayIsDeterministicAndCountsEligibleSteps) {
    TrainingRecoveryController controller;
    controller.reset(0);
    RecoverySettings settings = recoveryArmed(RecoveryBranch::Air22,
                                              ManualRecoveryMode::FixedDelay);
    settings.delaySteps = 3;
    RecoveryObservation observation =
        recoveryEligibleFor(RecoveryBranch::Air22);
    observation.directionRingHead = 0;

    uint64_t serial = 0;
    for (uint32_t step = 0; step < 3u; ++step) {
        const RecoveryEmission delayed =
            controller.advance(++serial, settings, observation);
        QNP_CHECK(delayed.state == RecoveryState::DelayEligibleSteps);
        QNP_CHECK(!delayed.active);
        QNP_CHECK(delayed.directions == 0u);
        QNP_CHECK(delayed.stepsRemaining == 2u - step);
    }
    const RecoveryEmission tap1 =
        controller.advance(++serial, settings, observation);
    QNP_CHECK(tap1.state == RecoveryState::EmitTap1);
    QNP_CHECK(tap1.directions == qinput::kLogicalDown);
    QNP_CHECK(serial == 4u);

    {
        TrainingRecoveryController earliest;
        earliest.reset(0);
        uint64_t earliestSerial = 0;
        const RecoveryEmission first = earliest.advance(
            ++earliestSerial, recoveryArmed(RecoveryBranch::Air22),
            observation);
        QNP_CHECK(first.state == RecoveryState::EmitTap1);
        QNP_CHECK(first.directions == qinput::kLogicalDown);
    }
    {
        TrainingRecoveryController single;
        single.reset(0);
        RecoverySettings one = recoveryArmed(RecoveryBranch::Air22,
                                             ManualRecoveryMode::FixedDelay);
        one.delaySteps = 1;
        uint64_t singleSerial = 0;
        QNP_CHECK(single.advance(++singleSerial, one, observation).state
                  == RecoveryState::DelayEligibleSteps);
        QNP_CHECK(single.advance(++singleSerial, one, observation).state
                  == RecoveryState::EmitTap1);
    }

    RecoverySettings random = recoveryArmed(RecoveryBranch::Air22,
                                            ManualRecoveryMode::RandomDelay);
    random.randomMinSteps = 2;
    random.randomMaxSteps = 9;
    TrainingRecoveryController a;
    TrainingRecoveryController b;
    a.reset(7);
    b.reset(7);
    QNP_CHECK(a.rngState() == b.rngState());
    QNP_CHECK(a.rngState() == recoverySeedFor(7));
    QNP_CHECK(recoverySeedFor(7) != recoverySeedFor(8));
    RecoveryObservation randomObservation =
        recoveryEligibleFor(RecoveryBranch::Air22);
    randomObservation.trainingGeneration = 7;
    randomObservation.directionRingHead = 0;
    uint64_t serialA = 0;
    uint64_t serialB = 0;
    for (uint32_t step = 0; step < 12u; ++step) {
        const RecoveryEmission left =
            a.advance(++serialA, random, randomObservation);
        const RecoveryEmission right =
            b.advance(++serialB, random, randomObservation);
        QNP_CHECK(left.directions == right.directions);
        QNP_CHECK(left.state == right.state);
        QNP_CHECK(left.stepsRemaining == right.stepsRemaining);
    }
    QNP_CHECK(a.rngState() == b.rngState());
    TrainingRecoveryController restored;
    restored.reset(7);
    restored.restoreRngState(a.rngState());
    QNP_CHECK(restored.rngState() == a.rngState());
    uint64_t rng = recoverySeedFor(3);
    for (uint32_t draw = 0; draw < 64u; ++draw) {
        const uint32_t value =
            2u + static_cast<uint32_t>(recoverySplitMix64(rng) % 8u);
        QNP_CHECK(value >= 2u && value <= 9u);
    }
    QNP_CHECK(kRecoveryDelayStepsMax == 240u);
    QNP_CHECK(kRecoveryWindowAir == 30u);
    QNP_CHECK(kRecoveryWindowGround == 20u);
    QNP_CHECK(kRecoveryWindowTerminal == 10u);
    QNP_CHECK(recoveryBranchWindow(RecoveryBranch::Air22)
              == kRecoveryWindowAir);
    QNP_CHECK(recoveryBranchWindow(RecoveryBranch::Ground88)
              == kRecoveryWindowGround);
    QNP_CHECK(recoveryBranchWindow(RecoveryBranch::Wall66)
              == kRecoveryWindowGround);
    QNP_CHECK(recoveryBranchWindow(RecoveryBranch::TerminalForward66)
              == kRecoveryWindowTerminal);
    QNP_CHECK(kRecoveryRingScanCap == 16u);
    QNP_CHECK(kRecoveryAwaitCapSteps < kRecoveryRingScanCap);
}

QNP_TEST(TrainingRecoveryCommandPayloadsAreRangeValidated) {
    QNP_CHECK(static_cast<uint32_t>(RecoveryPolicySlot::Count) == 5u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryPolicySlot::Air) == 0u);
    QNP_CHECK(static_cast<uint32_t>(RecoveryPolicySlot::ThrowEscape) == 4u);
    for (uint32_t slot = 0; slot <= 5u; ++slot) {
        for (uint32_t mode = 0; mode <= 5u; ++mode) {
            TrainingCommandV1 c =
                command(1, 1, TrainingCommandKind::SetRecoveryPolicy);
            c.slotOrSetting = slot;
            c.value = mode;
            QNP_CHECK(validCommandPayload(c) == (slot < 5u && mode <= 4u));
        }
    }
    for (uint32_t choice = 0; choice <= 4u; ++choice) {
        TrainingCommandV1 c =
            command(1, 1, TrainingCommandKind::SetTerminalRecoveryChoice);
        c.value = choice;
        QNP_CHECK(validCommandPayload(c) == (choice <= 3u));
    }
    {
        TrainingCommandV1 c =
            command(1, 1, TrainingCommandKind::SetRecoveryTiming);
        c.value = 12;
        c.flags = 4u | (9u << 16);
        QNP_CHECK(validCommandPayload(c));
        c.value = kRecoveryDelayStepsMax;
        QNP_CHECK(validCommandPayload(c));
        c.value = kRecoveryDelayStepsMax + 1u;
        QNP_CHECK(!validCommandPayload(c));
        c.value = 0;
        c.flags = 9u | (4u << 16);
        QNP_CHECK(!validCommandPayload(c));
        c.flags = 4u | (4u << 16);
        QNP_CHECK(validCommandPayload(c));
        c.flags = (kRecoveryDelayStepsMax + 1u) << 16;
        QNP_CHECK(!validCommandPayload(c));
        c.flags = 0;
        c.slotOrSetting = 1;
        QNP_CHECK(!validCommandPayload(c));
    }
    for (const TrainingCommandKind kind :
         {TrainingCommandKind::TogglePause, TrainingCommandKind::FrameStep}) {
        TrainingCommandV1 c = command(1, 1, kind);
        QNP_CHECK(validCommandPayload(c));
        c.value = 1;
        QNP_CHECK(!validCommandPayload(c));
        c.value = 0;
        c.slotOrSetting = 1;
        QNP_CHECK(!validCommandPayload(c));
        c.slotOrSetting = 0;
        c.flags = 1;
        QNP_CHECK(!validCommandPayload(c));
    }
    for (uint32_t target = 0; target <= 3u; ++target) {
        TrainingCommandV1 c =
            command(1, 1, TrainingCommandKind::ResetRoundStartNeutral);
        c.slotOrSetting = target;
        QNP_CHECK(validCommandPayload(c) == (target <= 2u));
    }
}

QNP_TEST(TrainingNeutralPredicateIsAnAllowListAndFailsClosed) {
    QNP_CHECK(evaluateFighterNeutral(neutralFighter(0)) == TrainingNeutralOk);
    QNP_CHECK(evaluateFighterNeutral(neutralFighter(1)) == TrainingNeutralOk);
    QNP_CHECK(evaluateNeutralGlobals(neutralGlobals()) == TrainingNeutralOk);

    FighterNeutralObservation unreadable{};
    unreadable.readable = false;
    QNP_CHECK(evaluateFighterNeutral(unreadable) == NeutralFighterUnreadable);
    NeutralGlobalObservation unreadableGlobals{};
    QNP_CHECK(evaluateNeutralGlobals(unreadableGlobals)
              == NeutralFighterUnreadable);

    struct ClauseCase {
        const char* name;
        uint32_t expected;
        void (*apply)(FighterNeutralObservation&);
    };
    const ClauseCase clauses[] = {
        {"slot 2 is not addressable", NeutralSlotOutOfRange,
         [](FighterNeutralObservation& o) { o.slot = 2; }},
        {"an alternate form is participating", NeutralAlternateFormActive,
         [](FighterNeutralObservation& o) { o.alternateForm = 1; }},
        {"action is not a free-movement id", NeutralActionNotFreeMovement,
         [](FighterNeutralObservation& o) { o.action = 25; }},
        {"the dispatcher is not on a free-movement id",
         NeutralDispatchNotFreeMovement,
         [](FighterNeutralObservation& o) { o.dispatchAction = 25; }},
        {"init status is not running", NeutralInitNotRunning,
         [](FighterNeutralObservation& o) { o.initStatus = -1; }},
        {"a transition is pending", NeutralTransitionPending,
         [](FighterNeutralObservation& o) { o.transitionFlag = 1; }},
        {"the action lock word is set", NeutralActionLockSet,
         [](FighterNeutralObservation& o) { o.actionLockEnd = 4; }},
        {"posture is not grounded", NeutralPostureNotGrounded,
         [](FighterNeutralObservation& o) { o.postureNibble = 4; }},
        {"the terminal marker is latched", NeutralTerminalLatched,
         [](FighterNeutralObservation& o) { o.terminalReady = 1; }},
        {"a round result is latched", NeutralRoundResultLatched,
         [](FighterNeutralObservation& o) { o.roundEnd = 1; }},
        {"a fatal cause is recorded", NeutralFatalRecorded,
         [](FighterNeutralObservation& o) { o.fatalCause = 0; }},
        {"the COM owns the fighter", NeutralComOwned,
         [](FighterNeutralObservation& o) { o.comOwnership = 1; }},
        {"the manual recovery lock is set", NeutralRecoveryLockSet,
         [](FighterNeutralObservation& o) { o.recoveryLock = 1; }},
        {"the recovery phase latch is set", NeutralRecoveryPhaseLatch,
         [](FighterNeutralObservation& o) { o.recoveryPhaseLatch = 2; }},
        {"a combo is being counted", NeutralComboActive,
         [](FighterNeutralObservation& o) { o.comboHits = 3; }},
        {"damage has not finished settling", NeutralDamageUnsettled,
         [](FighterNeutralObservation& o) { o.lifePendingDamage = 40; }},
        {"reserve is still being transferred", NeutralDamageUnsettled,
         [](FighterNeutralObservation& o) { o.lifeReserve = 100; }},
        {"the settle timer is still running", NeutralDamageUnsettled,
         [](FighterNeutralObservation& o) { o.lifeSettleTimer = 6; }},
    };
    for (const ClauseCase& clause : clauses) {
        FighterNeutralObservation observation = neutralFighter(1);
        clause.apply(observation);
        const uint32_t reasons = evaluateFighterNeutral(observation);
        if (reasons != clause.expected) {
            std::printf("    [neutral] %s -> %08X, want %08X\n",
                        clause.name, reasons, clause.expected);
        }
        QNP_CHECK(reasons == clause.expected);
        QNP_CHECK(reasons != TrainingNeutralOk);
    }

    {
        FighterNeutralObservation observation = neutralFighter(0);
        observation.fatalCause = -1;
        QNP_CHECK(evaluateFighterNeutral(observation) == TrainingNeutralOk);
        observation.fatalCause = 0;
        QNP_CHECK(evaluateFighterNeutral(observation) == NeutralFatalRecorded);
    }
    for (const uint8_t posture : {uint8_t{1}, uint8_t{2}}) {
        FighterNeutralObservation observation = neutralFighter(0);
        observation.postureNibble = posture;
        QNP_CHECK(evaluateFighterNeutral(observation) == TrainingNeutralOk);
    }
    for (const uint8_t posture : {uint8_t{0}, uint8_t{3}, uint8_t{4},
                                  uint8_t{5}, uint8_t{6}, uint8_t{7}}) {
        FighterNeutralObservation observation = neutralFighter(0);
        observation.postureNibble = posture;
        QNP_CHECK(evaluateFighterNeutral(observation)
                  == NeutralPostureNotGrounded);
    }

    {
        FighterNeutralObservation observation = neutralFighter(1);
        observation.action = 25;
        observation.comboHits = 2;
        observation.lifePendingDamage = 300;
        QNP_CHECK(evaluateFighterNeutral(observation)
                  == (NeutralActionNotFreeMovement | NeutralComboActive
                      | NeutralDamageUnsettled));
    }

    {
        NeutralGlobalObservation globals = neutralGlobals();
        globals.roundResultState = 1;
        QNP_CHECK(evaluateNeutralGlobals(globals) == NeutralRoundResultState);
        globals = neutralGlobals();
        globals.fatalHitDisableFlag = 1;
        QNP_CHECK(evaluateNeutralGlobals(globals) == NeutralFatalDisableFlag);
    }

    for (const uint32_t action : {18u, 19u, 20u, 21u, 22u, 23u, 24u,
                                  25u, 31u, 33u, 34u, 35u, 36u, 37u, 38u,
                                  56u, 57u, 58u, 70u, 73u,
                                  200u, 208u, 210u, 250u, 255u, 999u}) {
        QNP_CHECK(trainingReactionOrRecoveryAction(action));
    }
    for (const uint32_t action : {0u, 1u, 12u, 13u, 14u, 15u, 26u, 27u, 28u,
                                  30u, 32u, 39u, 48u, 49u, 55u, 59u, 69u, 71u,
                                  72u, 199u}) {
        QNP_CHECK(!trainingReactionOrRecoveryAction(action));
    }
    {
        FighterNeutralObservation observation = neutralFighter(0);
        observation.action = 1;
        QNP_CHECK(!trainingReactionOrRecoveryAction(1u));
        QNP_CHECK(evaluateFighterNeutral(observation)
                  == NeutralActionNotFreeMovement);
    }
}

QNP_TEST(TrainingActionFamiliesFollowTheActionStateCatalog) {
    ComboEpisodeObservation quiet{};
    quiet.readable = true;
    quiet.damageSettled = true;
    quiet.recoveryLocksClear = true;

    QNP_CHECK(!trainingReactionOrRecoveryAction(13u));
    QNP_CHECK(!trainingReactionOrRecoveryAction(14u));
    {
        ComboEpisodeMachine machine;
        machine.reset();
        ComboEpisodeObservation hit = quiet;
        hit.admittedDamageSerial = 1;
        QNP_CHECK(!machine.advance(hit, 2u));
        hit.admittedDamageSerial = 2;
        QNP_CHECK(!machine.advance(hit, 2u));
        QNP_CHECK(machine.state() == ComboEpisodeState::InEpisode);

        ComboEpisodeObservation dashing = quiet;
        dashing.admittedDamageSerial = 2;
        dashing.inReactionAction = trainingReactionOrRecoveryAction(13u);
        QNP_CHECK(!dashing.inReactionAction);
        QNP_CHECK(!machine.advance(dashing, 2u));
        QNP_CHECK(machine.state() == ComboEpisodeState::Settling);
        QNP_CHECK(machine.advance(dashing, 2u));
        QNP_CHECK(machine.state() == ComboEpisodeState::Idle);
    }
    {
        RecoveryExitTrigger exit;
        exit.reset();
        QNP_CHECK(!exit.advance(trainingReactionOrRecoveryAction(13u), true));
        QNP_CHECK(!exit.advance(trainingReactionOrRecoveryAction(0u), true));
    }

    QNP_CHECK(trainingReactionOrRecoveryAction(200u));
    QNP_CHECK(trainingReactionOrRecoveryAction(208u));
    QNP_CHECK(trainingReactionOrRecoveryAction(210u));
    QNP_CHECK(trainingReactionOrRecoveryAction(250u));
    QNP_CHECK(trainingReactionOrRecoveryAction(255u));
    QNP_CHECK(trainingReactionOrRecoveryAction(73u));
    QNP_CHECK(!trainingReactionOrRecoveryAction(199u));
    {
        ComboEpisodeMachine machine;
        machine.reset();
        ComboEpisodeObservation hit = quiet;
        hit.admittedDamageSerial = 1;
        QNP_CHECK(!machine.advance(hit, 2u));
        hit.admittedDamageSerial = 2;
        QNP_CHECK(!machine.advance(hit, 2u));
        QNP_CHECK(machine.state() == ComboEpisodeState::InEpisode);

        ComboEpisodeObservation thrown = quiet;
        thrown.admittedDamageSerial = 2;
        thrown.inReactionAction = trainingReactionOrRecoveryAction(210u);
        QNP_CHECK(thrown.inReactionAction);
        for (uint32_t step = 0; step < 60u; ++step) {
            QNP_CHECK(!machine.advance(thrown, 2u));
            QNP_CHECK(machine.state() == ComboEpisodeState::InEpisode);
        }
    }
    {
        RecoveryExitTrigger exit;
        exit.reset();
        QNP_CHECK(!exit.advance(
            trainingReactionOrRecoveryAction(210u), false));
        QNP_CHECK(!exit.advance(
            trainingReactionOrRecoveryAction(73u), false));
        QNP_CHECK(exit.advance(
            trainingReactionOrRecoveryAction(0u), true));
    }

    for (const uint32_t action : {0u, 26u, 27u, 28u}) {
        QNP_CHECK(trainingFreeMovementAction(action));
        FighterNeutralObservation observation = neutralFighter(0);
        observation.action = action;
        observation.dispatchAction = action;
        QNP_CHECK(evaluateFighterNeutral(observation) == TrainingNeutralOk);
    }
    for (const uint32_t action : {1u, 13u, 18u, 19u, 21u, 22u, 23u, 25u, 210u}) {
        QNP_CHECK(!trainingFreeMovementAction(action));
        FighterNeutralObservation byAction = neutralFighter(0);
        byAction.action = action;
        QNP_CHECK(evaluateFighterNeutral(byAction)
                  == NeutralActionNotFreeMovement);
        FighterNeutralObservation byDispatch = neutralFighter(0);
        byDispatch.dispatchAction = action;
        QNP_CHECK(evaluateFighterNeutral(byDispatch)
                  == NeutralDispatchNotFreeMovement);
    }
    {
        FighterNeutralObservation crouching = neutralFighter(0);
        crouching.postureNibble = 2;
        crouching.action = 28;
        crouching.dispatchAction = 28;
        QNP_CHECK(evaluateFighterNeutral(crouching) == TrainingNeutralOk);
        FighterNeutralObservation blocking = neutralFighter(0);
        blocking.postureNibble = 2;
        blocking.action = 19;
        blocking.dispatchAction = 19;
        QNP_CHECK(evaluateFighterNeutral(blocking)
                  == (NeutralActionNotFreeMovement
                      | NeutralDispatchNotFreeMovement));
    }
    {
        FighterNeutralObservation mixed = neutralFighter(0);
        mixed.action = 26;
        mixed.dispatchAction = 0;
        QNP_CHECK(evaluateFighterNeutral(mixed) == TrainingNeutralOk);
    }
    {
        RoundStartNeutralObservation reset{};
        reset.readable = true;
        reset.action = 26;
        reset.dispatchAction = 26;
        QNP_CHECK((evaluateRoundStartNeutral(reset)
                   & (RoundStartNeutralAction | RoundStartNeutralDispatch))
                  == 0u);
        reset.action = 0;
        reset.dispatchAction = 0;
        QNP_CHECK((evaluateRoundStartNeutral(reset)
                   & (RoundStartNeutralAction | RoundStartNeutralDispatch))
                  == (RoundStartNeutralAction | RoundStartNeutralDispatch));
    }
}

QNP_TEST(TrainingWhenNeutralRequiresBothSidesAndStaysReady) {
    auto bothNeutral = [](const FighterNeutralObservation& p1,
                          const FighterNeutralObservation& p2,
                          const NeutralGlobalObservation& globals) {
        return evaluateFighterNeutral(p1) == TrainingNeutralOk
            && evaluateFighterNeutral(p2) == TrainingNeutralOk
            && evaluateNeutralGlobals(globals) == TrainingNeutralOk;
    };

    const FighterNeutralObservation idleP1 = neutralFighter(0);
    const FighterNeutralObservation idleP2 = neutralFighter(1);
    QNP_CHECK(bothNeutral(idleP1, idleP2, neutralGlobals()));

    FighterNeutralObservation attacking = neutralFighter(0);
    attacking.action = 1;
    FighterNeutralObservation reacting = neutralFighter(1);
    reacting.action = 25;
    reacting.comboHits = 4;
    reacting.lifePendingDamage = 900;
    QNP_CHECK(!bothNeutral(attacking, reacting, neutralGlobals()));
    QNP_CHECK(!bothNeutral(idleP1, reacting, neutralGlobals()));
    QNP_CHECK(!bothNeutral(attacking, idleP2, neutralGlobals()));
    FighterNeutralObservation unreadable{};
    QNP_CHECK(!bothNeutral(idleP1, unreadable, neutralGlobals()));
    QNP_CHECK(!bothNeutral(unreadable, idleP2, neutralGlobals()));
    NeutralGlobalObservation unreadableGlobals{};
    QNP_CHECK(!bothNeutral(idleP1, idleP2, unreadableGlobals));

    QNP_CHECK(kTrainingNeutralHoldSteps == 2u);
    NeutralTrigger trigger;
    trigger.reset();
    QNP_CHECK(!trigger.advance(true));
    QNP_CHECK(trigger.neutral());
    QNP_CHECK(trigger.advance(true));
    for (uint32_t step = 0; step < 120u; ++step)
        QNP_CHECK(trigger.advance(true));
    QNP_CHECK(!trigger.advance(false));
    QNP_CHECK(!trigger.neutral());
    QNP_CHECK(trigger.holdSteps() == 0u);
    QNP_CHECK(!trigger.advance(true));
    QNP_CHECK(trigger.advance(true));
    trigger.reset();
    for (uint32_t step = 0; step < 20u; ++step) {
        QNP_CHECK(!trigger.advance(true));
        QNP_CHECK(!trigger.advance(false));
    }
}

QNP_TEST(TrainingSettledFreeMovementFallbackRestoresEveryResourceAfterElevenSteps) {
    FighterNeutralObservation p1 = neutralFighter(0);
    FighterNeutralObservation p2 = neutralFighter(1);
    p1.actionLockEnd = 1;
    p1.postureNibble = 0x09;
    p1.terminalReady = 1;
    p2.actionLockEnd = 2;
    p2.postureNibble = 0x09;
    p2.terminalReady = 1;
    QNP_CHECK(evaluateFighterNeutral(p1) != TrainingNeutralOk);
    QNP_CHECK(evaluateFighterNeutral(p2) != TrainingNeutralOk);
    for (const uint32_t action : {0u, 26u, 27u, 28u}) {
        p1.action = action;
        QNP_CHECK(trainingExactIdleAction(p1));
    }
    for (const uint32_t action : {1u, 13u, 18u, 21u, 210u}) {
        p1.action = action;
        QNP_CHECK(!trainingExactIdleAction(p1));
    }
    p1.action = 26u;
    p2.action = 28u;
    QNP_CHECK(trainingExactIdleAction(p1));
    QNP_CHECK(trainingExactIdleAction(p2));
    FighterNeutralObservation unreadable = p2;
    unreadable.readable = false;
    QNP_CHECK(!trainingExactIdleAction(unreadable));

    IdleStateTrigger trigger;
    trigger.reset();
    ResourcePolicySet restoreAll{};
    for (ResourcePolicySettings* policy : {
             &restoreAll.health, &restoreAll.ex, &restoreAll.guardGauge}) {
        policy->trigger = ResourceTrigger::WhenNeutral;
        policy->target = TrainingRefillTarget::Both;
    }
    ResourceTriggerSignals signals{};
    signals.neutralReady = false;
    for (uint32_t step = 0; step < 10u; ++step) {
        signals.exactIdleReady = trigger.advance(true);
        QNP_CHECK(!signals.exactIdleReady);
        QNP_CHECK(resolveHealthPolicyFire(restoreAll.health, signals) == 0u);
        QNP_CHECK(resolvePolicyFire(restoreAll.ex, signals) == 0u);
        QNP_CHECK(resolvePolicyFire(restoreAll.guardGauge, signals) == 0u);
    }
    QNP_CHECK(trigger.holdSteps() == 10u);
    signals.exactIdleReady = trigger.advance(true);
    QNP_CHECK(signals.exactIdleReady);
    QNP_CHECK(resolveHealthPolicyFire(restoreAll.health, signals) == 0x3u);
    QNP_CHECK(resolvePolicyFire(restoreAll.ex, signals) == 0x3u);
    QNP_CHECK(resolvePolicyFire(restoreAll.guardGauge, signals) == 0x3u);
    for (uint32_t step = 0; step < 60u; ++step) {
        signals.exactIdleReady = trigger.advance(true);
        QNP_CHECK(resolveHealthPolicyFire(restoreAll.health, signals) == 0x3u);
        QNP_CHECK(resolvePolicyFire(restoreAll.ex, signals) == 0x3u);
        QNP_CHECK(resolvePolicyFire(restoreAll.guardGauge, signals) == 0x3u);
    }
    QNP_CHECK(!trigger.advance(false));
    QNP_CHECK(trigger.holdSteps() == 0u);
    p2.action = 1u;
    QNP_CHECK(!trainingExactIdleAction(p2));
}

QNP_TEST(TrainingComboEpisodeAndRecoveryExitTriggers) {
    ComboEpisodeMachine machine;
    machine.reset();
    QNP_CHECK(machine.state() == ComboEpisodeState::Idle);

    ComboEpisodeObservation quiet{};
    quiet.readable = true;
    quiet.damageSettled = true;
    quiet.recoveryLocksClear = true;

    ComboEpisodeObservation unreadable = quiet;
    unreadable.readable = false;
    for (uint32_t step = 0; step < 100u; ++step)
        QNP_CHECK(!machine.advance(unreadable, 4u));
    QNP_CHECK(machine.state() == ComboEpisodeState::Idle);

    QNP_CHECK(!machine.advance(quiet, 4u));
    QNP_CHECK(machine.state() == ComboEpisodeState::Idle);

    ComboEpisodeObservation hit = quiet;
    hit.admittedDamageSerial = 1;
    QNP_CHECK(!machine.advance(hit, 4u));
    QNP_CHECK(machine.state() == ComboEpisodeState::InEpisode);

    ComboEpisodeObservation reacting = quiet;
    reacting.admittedDamageSerial = 1;
    reacting.inReactionAction = true;
    QNP_CHECK(!machine.advance(reacting, 4u));

    ComboEpisodeObservation settling = quiet;
    settling.admittedDamageSerial = 1;
    QNP_CHECK(!machine.advance(settling, 4u));
    QNP_CHECK(machine.state() == ComboEpisodeState::Settling);
    QNP_CHECK(!machine.advance(settling, 4u));
    QNP_CHECK(!machine.advance(settling, 4u));
    QNP_CHECK(machine.advance(settling, 4u));
    QNP_CHECK(machine.state() == ComboEpisodeState::Idle);
    QNP_CHECK(!machine.advance(settling, 4u));

    machine.reset();
    ComboEpisodeObservation open = quiet;
    open.admittedDamageSerial = 5;
    QNP_CHECK(!machine.advance(open, 3u));
    QNP_CHECK(!machine.advance(open, 3u));
    ComboEpisodeObservation stillCounting = open;
    stillCounting.nativeComboHits = 2;
    QNP_CHECK(!machine.advance(stillCounting, 3u));
    QNP_CHECK(machine.state() == ComboEpisodeState::InEpisode);
    QNP_CHECK(!machine.advance(open, 3u));
    QNP_CHECK(!machine.advance(open, 3u));
    QNP_CHECK(machine.advance(open, 3u));

    machine.reset();
    ComboEpisodeObservation baseline = quiet;
    baseline.admittedDamageSerial = 9;
    QNP_CHECK(!machine.advance(baseline, 2u));
    QNP_CHECK(machine.state() == ComboEpisodeState::Idle);
    ComboEpisodeObservation locked = quiet;
    locked.admittedDamageSerial = 10;
    QNP_CHECK(!machine.advance(locked, 2u));
    QNP_CHECK(machine.state() == ComboEpisodeState::InEpisode);
    locked.recoveryLocksClear = false;
    for (uint32_t step = 0; step < 30u; ++step)
        QNP_CHECK(!machine.advance(locked, 2u));
    locked.recoveryLocksClear = true;
    QNP_CHECK(!machine.advance(locked, 2u));
    QNP_CHECK(machine.advance(locked, 2u));

    RecoveryExitTrigger exit;
    exit.reset();
    QNP_CHECK(!exit.advance(false, true));
    QNP_CHECK(!exit.advance(true, true));
    QNP_CHECK(!exit.advance(true, true));
    QNP_CHECK(exit.advance(false, true));
    QNP_CHECK(!exit.advance(false, true));
    exit.reset();
    QNP_CHECK(!exit.advance(true, true));
    QNP_CHECK(!exit.advance(false, false));
    QNP_CHECK(!exit.advance(false, true));
}

QNP_TEST(TrainingResourcePolicyFiringIntersectsTriggerAndTarget) {
    QNP_CHECK(refillTargetPlayerMask(TrainingRefillTarget::Both) == 0x3u);
    QNP_CHECK(refillTargetPlayerMask(TrainingRefillTarget::P1) == 0x1u);
    QNP_CHECK(refillTargetPlayerMask(TrainingRefillTarget::P2) == 0x2u);
    QNP_CHECK(refillTargetPlayerMask(
        static_cast<TrainingRefillTarget>(3)) == 0u);
    QNP_CHECK(refillTargetPlayerMask(
        static_cast<TrainingRefillTarget>(0xFFFFFFFFu)) == 0u);

    ResourceTriggerSignals none{};
    for (uint32_t triggerRaw = 0; triggerRaw <= 4u; ++triggerRaw) {
        for (uint32_t targetRaw = 0; targetRaw <= 2u; ++targetRaw) {
            ResourcePolicySettings policy{};
            policy.trigger = static_cast<ResourceTrigger>(triggerRaw);
            policy.target = static_cast<TrainingRefillTarget>(targetRaw);
            QNP_CHECK(resolvePolicyFire(policy, none) == 0u);
        }
    }

    ResourceTriggerSignals everything{};
    everything.neutralReady = true;
    everything.exactIdleReady = true;
    everything.resetAccepted = true;
    everything.comboEpisodeClosed[0] = true;
    everything.comboEpisodeClosed[1] = true;
    everything.recoveryExited[0] = true;
    everything.recoveryExited[1] = true;
    {
        ResourcePolicySettings off{};
        off.trigger = ResourceTrigger::Off;
        off.target = TrainingRefillTarget::Both;
        QNP_CHECK(resolvePolicyFire(off, everything) == 0u);
        QNP_CHECK(resolveHealthPolicyFire(off, everything) == 0u);
    }

    for (uint32_t targetRaw = 0; targetRaw <= 2u; ++targetRaw) {
        ResourcePolicySettings policy{};
        policy.trigger = ResourceTrigger::WhenNeutral;
        policy.target = static_cast<TrainingRefillTarget>(targetRaw);
        ResourceTriggerSignals neutral{};
        neutral.neutralReady = true;
        QNP_CHECK(resolvePolicyFire(policy, neutral)
                  == refillTargetPlayerMask(policy.target));
    }

    for (uint32_t targetRaw = 0; targetRaw <= 2u; ++targetRaw) {
        ResourcePolicySettings policy{};
        policy.trigger = ResourceTrigger::WhenNeutral;
        policy.target = static_cast<TrainingRefillTarget>(targetRaw);
        ResourceTriggerSignals reset{};
        reset.resetAccepted = true;
        const uint32_t expected = refillTargetPlayerMask(policy.target);
        QNP_CHECK(resolvePolicyFire(policy, reset) == expected);
        QNP_CHECK(resolveHealthPolicyFire(policy, reset) == expected);
    }

    struct PerSideCase {
        ResourceTrigger trigger;
        bool p1;
        bool p2;
        TrainingRefillTarget target;
        uint32_t expected;
    };
    const PerSideCase perSide[] = {
        {ResourceTrigger::AfterCombo, true, false,
         TrainingRefillTarget::Both, 0x1u},
        {ResourceTrigger::AfterCombo, false, true,
         TrainingRefillTarget::Both, 0x2u},
        {ResourceTrigger::AfterCombo, true, true,
         TrainingRefillTarget::Both, 0x3u},
        {ResourceTrigger::AfterCombo, false, true,
         TrainingRefillTarget::P1, 0x0u},
        {ResourceTrigger::AfterCombo, true, true,
         TrainingRefillTarget::P2, 0x2u},
        {ResourceTrigger::AfterRecovery, false, true,
         TrainingRefillTarget::Both, 0x2u},
        {ResourceTrigger::AfterRecovery, true, false,
         TrainingRefillTarget::P2, 0x0u},
    };
    for (const PerSideCase& entry : perSide) {
        ResourcePolicySettings policy{};
        policy.trigger = entry.trigger;
        policy.target = entry.target;
        ResourceTriggerSignals signals{};
        if (entry.trigger == ResourceTrigger::AfterCombo) {
            signals.comboEpisodeClosed[0] = entry.p1;
            signals.comboEpisodeClosed[1] = entry.p2;
        } else {
            signals.recoveryExited[0] = entry.p1;
            signals.recoveryExited[1] = entry.p2;
        }
        QNP_CHECK(resolvePolicyFire(policy, signals) == entry.expected);
        ResourcePolicySettings crossed = policy;
        crossed.trigger =
            entry.trigger == ResourceTrigger::AfterCombo
                ? ResourceTrigger::AfterRecovery
                : ResourceTrigger::AfterCombo;
        QNP_CHECK(resolvePolicyFire(crossed, signals) == 0u);
    }

    {
        ResourcePolicySettings policy{};
        policy.trigger = ResourceTrigger::OnResetOnly;
        policy.target = TrainingRefillTarget::Both;
        ResourceTriggerSignals reset{};
        reset.resetAccepted = true;
        QNP_CHECK(resolvePolicyFire(policy, reset) == 0x3u);
        ResourceTriggerSignals neutral{};
        neutral.neutralReady = true;
        QNP_CHECK(resolvePolicyFire(policy, neutral) == 0u);
    }

    {
        ResourcePolicySettings policy{};
        policy.trigger = ResourceTrigger::WhenNeutral;
        policy.target = static_cast<TrainingRefillTarget>(7);
        ResourceTriggerSignals neutral{};
        neutral.neutralReady = true;
        QNP_CHECK(resolvePolicyFire(policy, neutral) == 0u);
    }
    {
        ResourcePolicySettings policy{};
        policy.trigger = static_cast<ResourceTrigger>(99);
        policy.target = TrainingRefillTarget::Both;
        QNP_CHECK(resolvePolicyFire(policy, everything) == 0u);
    }

    ResourcePolicySet policies{};
    policies.health.trigger = ResourceTrigger::WhenNeutral;
    policies.health.target = TrainingRefillTarget::Both;
    policies.ex.trigger = ResourceTrigger::AfterCombo;
    policies.ex.target = TrainingRefillTarget::P2;
    policies.guardGauge.trigger = ResourceTrigger::Off;
    ResourceTriggerSignals neutralOnly{};
    neutralOnly.neutralReady = true;
    QNP_CHECK(resolvePolicyFire(policies.health, neutralOnly) == 0x3u);
    QNP_CHECK(resolvePolicyFire(policies.ex, neutralOnly) == 0u);
    QNP_CHECK(resolvePolicyFire(policies.guardGauge, neutralOnly) == 0u);
}

QNP_TEST(TrainingRoundStartNeutralReportsPerClauseAndEscalatesThree) {
    RoundStartNeutralObservation clean{};
    clean.readable = true;
    clean.fatalCause = -1;
    clean.postureNibble = 1;
    clean.facing = 1;
    clean.expectedFacing = 1;
    clean.positionMatchesPlan = true;
    clean.framePointerInRange = true;
    QNP_CHECK(evaluateRoundStartNeutral(clean)
              == RoundStartNeutralAllBits);
    QNP_CHECK(RoundStartNeutralAllBits == 0x3FFFu);

    RoundStartNeutralObservation unreadable = clean;
    unreadable.readable = false;
    QNP_CHECK(evaluateRoundStartNeutral(unreadable) == 0u);
    QNP_CHECK((evaluateRoundStartNeutral(unreadable)
               & trainingRoundStartNeutralEscalatingBits()) == 0u);

    struct BitCase {
        const char* name;
        uint32_t bit;
        void (*apply)(RoundStartNeutralObservation&);
    };
    const BitCase bits[] = {
        {"action", RoundStartNeutralAction,
         [](RoundStartNeutralObservation& o) { o.action = 25; }},
        {"dispatch", RoundStartNeutralDispatch,
         [](RoundStartNeutralObservation& o) { o.dispatchAction = 25; }},
        {"init", RoundStartNeutralInit,
         [](RoundStartNeutralObservation& o) { o.initStatus = 1; }},
        {"no pending transition", RoundStartNeutralNoPending,
         [](RoundStartNeutralObservation& o) { o.transitionFlag = 1; }},
        {"grounded", RoundStartNeutralGrounded,
         [](RoundStartNeutralObservation& o) { o.postureNibble = 4; }},
        {"no terminal", RoundStartNeutralNoTerminal,
         [](RoundStartNeutralObservation& o) { o.terminalReady = 1; }},
        {"no wall", RoundStartNeutralNoWall,
         [](RoundStartNeutralObservation& o) { o.wallFlags = 2; }},
        {"facing", RoundStartNeutralFacing,
         [](RoundStartNeutralObservation& o) { o.facing = 0; }},
        {"no result", RoundStartNeutralNoResult,
         [](RoundStartNeutralObservation& o) { o.roundLost = 1; }},
        {"fatal idle", RoundStartNeutralFatalIdle,
         [](RoundStartNeutralObservation& o) { o.fatalCause = 3; }},
        {"no COM", RoundStartNeutralNoCom,
         [](RoundStartNeutralObservation& o) { o.comOwnership = 1; }},
        {"locks clear", RoundStartNeutralLocksClear,
         [](RoundStartNeutralObservation& o) { o.recoveryLock = 1; }},
        {"position", RoundStartNeutralPosition,
         [](RoundStartNeutralObservation& o) { o.positionMatchesPlan = false; }},
        {"frame pointer", RoundStartNeutralFrameOk,
         [](RoundStartNeutralObservation& o) {
             o.framePointerInRange = false;
         }},
    };
    for (const BitCase& entry : bits) {
        RoundStartNeutralObservation observation = clean;
        entry.apply(observation);
        const uint32_t mask = evaluateRoundStartNeutral(observation);
        if (mask != (RoundStartNeutralAllBits & ~entry.bit)) {
            std::printf("    [round-start] %s -> %04X\n", entry.name, mask);
        }
        QNP_CHECK(mask == (RoundStartNeutralAllBits & ~entry.bit));
    }

    QNP_CHECK(trainingRoundStartNeutralEscalatingBits()
              == (RoundStartNeutralNoResult | RoundStartNeutralNoCom
                  | RoundStartNeutralFrameOk));
    QNP_CHECK((trainingRoundStartNeutralEscalatingBits()
               & (RoundStartNeutralNoWall | RoundStartNeutralAction
                  | RoundStartNeutralPosition
                  | RoundStartNeutralLocksClear)) == 0u);
}

QNP_TEST(TrainingLifeSetIsAnOrderedTransactionSafeAtEveryPrefix) {
    QNP_CHECK(static_cast<uint32_t>(RefillVerdict::Apply) == 0u);
    QNP_CHECK(static_cast<uint32_t>(RefillVerdict::Busy) == 1u);
    QNP_CHECK(static_cast<uint32_t>(RefillVerdict::Corrupt) == 2u);
    QNP_CHECK(static_cast<uint32_t>(RefillVerdict::Invalid) == 3u);

    const int64_t maxLife = kLifeUnitsPerLevel;
    QNP_CHECK(planLifeMax(0).valid && planLifeMax(0).maxLife == maxLife);

    LifeRecordImage full{};
    full.current = maxLife;

    auto prefixesStayAlive = [](const LifeRecordImage& image,
                                const LifeSetPlan& plan) {
        int64_t current = image.current;
        int64_t pending = image.pendingDamage;
        int64_t reserve = image.reserve;
        for (uint32_t step = 0; step < plan.stepCount; ++step) {
            switch (plan.steps[step].offset) {
                case kLifeRecordCurrentOffset:
                    current = plan.steps[step].value;
                    break;
                case kLifeRecordPendingOffset:
                    pending = plan.steps[step].value;
                    break;
                case kLifeRecordReserveOffset:
                    reserve = plan.steps[step].value;
                    break;
                default:
                    break;
            }
            if (current + reserve - pending < 1) return false;
        }
        return current == plan.current && pending == plan.pendingDamage
            && reserve == plan.reserve;
    };

    struct SetCase {
        const char* name;
        int64_t current;
        int64_t pending;
        int64_t reserve;
        int64_t timer;
        int64_t requested;
        RefillVerdict verdict;
        uint32_t stepCount;
    };
    const SetCase cases[] = {
        {"raise from a chip of health", 100, 0, 0, 0, maxLife,
         RefillVerdict::Apply, 4},
        {"raise to the exact maximum", 1, 0, 0, 0, maxLife,
         RefillVerdict::Apply, 4},
        {"cut from full", maxLife, 0, 0, 0, 500,
         RefillVerdict::Apply, 5},
        {"cut to the minimum legal value", maxLife, 0, 0, 0, 1,
         RefillVerdict::Apply, 5},
        {"set with damage still settling", 4000, 300, 200, 12, 6000,
         RefillVerdict::Apply, 4},
        {"cut with damage still settling", 8000, 300, 200, 12, 60,
         RefillVerdict::Apply, 5},
        {"zero", maxLife, 0, 0, 0, 0, RefillVerdict::Invalid, 0},
        {"negative", maxLife, 0, 0, 0, -1, RefillVerdict::Invalid, 0},
        {"one above the maximum", maxLife, 0, 0, 0, maxLife + 1,
         RefillVerdict::Invalid, 0},
        {"absurdly large", maxLife, 0, 0, 0, INT32_MAX,
         RefillVerdict::Invalid, 0},
        {"negative current", -1, 0, 0, 0, 500, RefillVerdict::Corrupt, 0},
        {"negative pending", 100, -5, 0, 0, 500, RefillVerdict::Corrupt, 0},
        {"negative reserve", 100, 0, -5, 0, 500, RefillVerdict::Corrupt, 0},
        {"negative settle timer", 100, 0, 0, -1, 500,
         RefillVerdict::Corrupt, 0},
        {"current above the maximum", maxLife + 1, 0, 0, 0, 500,
         RefillVerdict::Corrupt, 0},
        {"already effectively dead", 100, 500, 0, 0, 500,
         RefillVerdict::Corrupt, 0},
        {"exactly zero effective life", 100, 100, 0, 0, 500,
         RefillVerdict::Corrupt, 0},
        {"current beyond int32", int64_t{INT32_MAX} + 1, 0, 0, 0, 500,
         RefillVerdict::Corrupt, 0},
        {"pending beyond int32", 100, int64_t{INT32_MAX} + 1, 0, 0, 500,
         RefillVerdict::Corrupt, 0},
        {"reserve beyond int32", 100, 0, int64_t{INT32_MAX} + 1, 0, 500,
         RefillVerdict::Corrupt, 0},
    };
    for (const SetCase& entry : cases) {
        LifeRecordImage image{};
        image.current = entry.current;
        image.pendingDamage = entry.pending;
        image.reserve = entry.reserve;
        image.settleTimer = entry.timer;
        const LifeSetPlan plan = planLifeSet(image, maxLife, entry.requested);
        if (plan.verdict != entry.verdict) {
            std::printf("    [lifeSet] %s -> verdict %u, want %u\n",
                        entry.name, static_cast<unsigned>(plan.verdict),
                        static_cast<unsigned>(entry.verdict));
        }
        QNP_CHECK(plan.verdict == entry.verdict);
        if (entry.verdict != RefillVerdict::Apply) {
            QNP_CHECK(plan.stepCount == 0u);
            continue;
        }
        QNP_CHECK(plan.stepCount == entry.stepCount);
        QNP_CHECK(plan.stepCount <= kLifeSetStepCapacity);
        QNP_CHECK(plan.current == static_cast<int32_t>(entry.requested));
        QNP_CHECK(plan.pendingDamage == 0);
        QNP_CHECK(plan.reserve == 0);
        QNP_CHECK(plan.settleTimer == 0);
        QNP_CHECK(prefixesStayAlive(image, plan));
        QNP_CHECK(plan.steps[0].offset == kLifeRecordCurrentOffset);
        QNP_CHECK(plan.steps[0].value >= entry.current);
        QNP_CHECK(plan.steps[0].value >= entry.requested);
        QNP_CHECK(plan.steps[1].offset == kLifeRecordPendingOffset);
        QNP_CHECK(plan.steps[2].offset == kLifeRecordReserveOffset);
        QNP_CHECK(plan.steps[3].offset == kLifeRecordSettleTimerOffset);
        if (plan.stepCount == 5u) {
            QNP_CHECK(plan.steps[4].offset == kLifeRecordCurrentOffset);
            QNP_CHECK(plan.steps[4].value
                      == static_cast<int32_t>(entry.requested));
        }
    }

    LifeRecordImage sane{};
    sane.current = 100;
    QNP_CHECK(planLifeSet(sane, 0, 50).verdict == RefillVerdict::Corrupt);
    QNP_CHECK(planLifeSet(sane, kLifeUnitsPerLevel - 1, 50).verdict
              == RefillVerdict::Corrupt);
    QNP_CHECK(planLifeSet(sane, int64_t{INT32_MAX} + 1, 50).verdict
              == RefillVerdict::Corrupt);
    QNP_CHECK(!planLifeMax(-1).valid);
    QNP_CHECK(!planLifeMax(kLifeLevelClamp + 1).valid);
    QNP_CHECK(planLifeMax(kLifeLevelClamp).valid);
    QNP_CHECK(planLifeMax(3).maxLife == kLifeUnitsPerLevel * 4);

    TrainingCommandV1 zero =
        command(1, 1, TrainingCommandKind::SetLifeValue);
    zero.value = 0;
    QNP_CHECK(!validCommandPayload(zero));
    zero.value = 1;
    QNP_CHECK(validCommandPayload(zero));
    zero.slotOrSetting = static_cast<uint32_t>(TrainingRefillTarget::P2);
    QNP_CHECK(validCommandPayload(zero));
    zero.slotOrSetting = 3;
    QNP_CHECK(!validCommandPayload(zero));
    zero.slotOrSetting = 0;
    zero.flags = 1;
    QNP_CHECK(!validCommandPayload(zero));
}

QNP_TEST(TrainingLifeResetPreservesEffectiveZeroOrRestoresFull) {
    const int64_t maxLife = kLifeUnitsPerLevel;
    LifeRecordImage image{};
    image.current = 100;
    image.pendingDamage = 100;
    image.settleTimer = 7;

    const LifeSetPlan preserved = planLifeReset(image, maxLife, false);
    QNP_REQUIRE(preserved.verdict == RefillVerdict::Apply);
    QNP_CHECK(preserved.current == 0);
    QNP_CHECK(preserved.pendingDamage == 0);
    QNP_CHECK(preserved.reserve == 0);
    QNP_CHECK(preserved.settleTimer == 0);

    int64_t current = image.current;
    int64_t pending = image.pendingDamage;
    int64_t reserve = image.reserve;
    for (uint32_t step = 0; step < preserved.stepCount; ++step) {
        switch (preserved.steps[step].offset) {
            case kLifeRecordCurrentOffset:
                current = preserved.steps[step].value;
                break;
            case kLifeRecordPendingOffset:
                pending = preserved.steps[step].value;
                break;
            case kLifeRecordReserveOffset:
                reserve = preserved.steps[step].value;
                break;
            default:
                break;
        }
        QNP_CHECK(current + reserve - pending >= 0);
    }
    QNP_CHECK(current == 0);
    QNP_CHECK(pending == 0);
    QNP_CHECK(reserve == 0);

    image.current = 4000;
    image.pendingDamage = 300;
    image.reserve = 200;
    image.settleTimer = 12;
    const LifeSetPlan stable = planLifeReset(image, maxLife, false);
    QNP_REQUIRE(stable.verdict == RefillVerdict::Apply);
    QNP_CHECK(stable.current == 3900);
    QNP_CHECK(stable.pendingDamage == 0);
    QNP_CHECK(stable.reserve == 0);
    QNP_CHECK(stable.settleTimer == 0);

    const LifeSetPlan restored = planLifeReset(image, maxLife, true);
    QNP_REQUIRE(restored.verdict == RefillVerdict::Apply);
    QNP_CHECK(restored.current == maxLife);
    QNP_CHECK(restored.pendingDamage == 0);
    QNP_CHECK(restored.reserve == 0);
    QNP_CHECK(restored.settleTimer == 0);

    image.pendingDamage = maxLife;
    image.reserve = 0;
    QNP_CHECK(planLifeReset(image, maxLife, false).verdict
              == RefillVerdict::Corrupt);
}

QNP_TEST(TrainingGuardGaugeAndExSetsRefuseOutOfRangeAndInFlight) {
    QNP_CHECK(kGuardGaugeFull == 100);
    for (const int64_t requested : {int64_t{0}, int64_t{1}, int64_t{50},
                                    int64_t{99}, int64_t{100}}) {
        const GuardGaugeSetPlan plan = planGuardGaugeSet(37, requested);
        QNP_CHECK(plan.verdict == RefillVerdict::Apply);
        QNP_CHECK(plan.writeValue == static_cast<int32_t>(requested));
    }
    QNP_CHECK(planGuardGaugeSet(100, 0).verdict == RefillVerdict::Apply);
    QNP_CHECK(planGuardGaugeSet(0, 101).verdict == RefillVerdict::Invalid);
    QNP_CHECK(planGuardGaugeSet(0, -1).verdict == RefillVerdict::Invalid);
    QNP_CHECK(planGuardGaugeSet(0, INT32_MAX).verdict
              == RefillVerdict::Invalid);
    QNP_CHECK(planGuardGaugeSet(-1, 50).verdict == RefillVerdict::Corrupt);
    QNP_CHECK(planGuardGaugeSet(101, 50).verdict == RefillVerdict::Corrupt);
    QNP_CHECK(planGuardGaugeSet(101, 999).verdict == RefillVerdict::Corrupt);
    QNP_CHECK(planGuardGaugeRefill(0).verdict == RefillVerdict::Apply);
    QNP_CHECK(planGuardGaugeRefill(0).writeValue == kGuardGaugeFull);
    QNP_CHECK(planGuardGaugeRefill(-1).verdict == RefillVerdict::Corrupt);
    QNP_CHECK(planGuardGaugeRefill(101).verdict == RefillVerdict::Corrupt);

    const int64_t idleLanes[kExPendingLaneCount] = {0, 0, 0, 0};
    QNP_CHECK(kExStocksFull == 9);
    QNP_CHECK(kExFractionMax == 53);
    QNP_CHECK(kExFractionFull == 0);

    for (int64_t stocks = 0; stocks <= kExStocksFull; ++stocks) {
        const int64_t fraction = stocks == kExStocksFull ? 0 : 17;
        const ExSetPlan plan =
            planExSet(0, 4, 20, idleLanes, stocks, fraction);
        QNP_CHECK(plan.verdict == RefillVerdict::Apply);
        QNP_CHECK(plan.stocks == static_cast<int32_t>(stocks));
        QNP_CHECK(plan.fraction == static_cast<int32_t>(fraction));
    }
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, 0, 0).verdict
              == RefillVerdict::Apply);
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, 0, kExFractionMax).verdict
              == RefillVerdict::Apply);
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, 10, 0).verdict
              == RefillVerdict::Invalid);
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, -1, 0).verdict
              == RefillVerdict::Invalid);
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, 3, 54).verdict
              == RefillVerdict::Invalid);
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, 3, -1).verdict
              == RefillVerdict::Invalid);
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, INT32_MAX, 0).verdict
              == RefillVerdict::Invalid);
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, kExStocksFull, 1).verdict
              == RefillVerdict::Invalid);
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, kExStocksFull, kExFractionMax)
                  .verdict == RefillVerdict::Invalid);
    QNP_CHECK(planExSet(0, 4, 20, idleLanes, kExStocksFull, 0).verdict
              == RefillVerdict::Apply);

    QNP_CHECK(planExSet(0, 10, 0, idleLanes, 3, 0).verdict
              == RefillVerdict::Corrupt);
    QNP_CHECK(planExSet(0, -1, 0, idleLanes, 3, 0).verdict
              == RefillVerdict::Corrupt);
    QNP_CHECK(planExSet(0, 4, 54, idleLanes, 3, 0).verdict
              == RefillVerdict::Corrupt);
    QNP_CHECK(planExSet(0, 4, -1, idleLanes, 3, 0).verdict
              == RefillVerdict::Corrupt);

    for (const int64_t state : {int64_t{kExSpendStateSingle},
                                int64_t{kExSpendStateMulti}}) {
        QNP_CHECK(planExSet(state, 4, 20, idleLanes, 3, 0).verdict
                  == RefillVerdict::Busy);
        QNP_CHECK(planExRefill(state, 4, 20, idleLanes).verdict
                  == RefillVerdict::Busy);
        QNP_CHECK(planExSet(state, 99, 99, idleLanes, 42, 42).verdict
                  == RefillVerdict::Busy);
    }
    for (uint32_t lane = 0; lane < kExPendingLaneCount; ++lane) {
        int64_t lanes[kExPendingLaneCount] = {0, 0, 0, 0};
        lanes[lane] = -1;
        QNP_CHECK(planExSet(0, 4, 20, lanes, 3, 0).verdict
                  == RefillVerdict::Busy);
        QNP_CHECK(planExRefill(0, 4, 20, lanes).verdict
                  == RefillVerdict::Busy);
        lanes[lane] = 2;
        QNP_CHECK(planExSet(0, 4, 20, lanes, 3, 0).verdict
                  == RefillVerdict::Busy);
    }
    QNP_CHECK(planExSet(1, 4, 20, idleLanes, 3, 0).verdict
              == RefillVerdict::Apply);
    QNP_CHECK(planExRefill(1, 4, 20, idleLanes).verdict
              == RefillVerdict::Apply);

    const ExRefillPlan refill = planExRefill(0, 2, 30, idleLanes);
    QNP_CHECK(refill.verdict == RefillVerdict::Apply);
    QNP_CHECK(refill.stocks == kExStocksFull);
    QNP_CHECK(refill.fraction == kExFractionFull);

    for (uint32_t stocks = 0; stocks <= 10u; ++stocks) {
        for (const uint32_t fraction : {0u, 1u, 53u, 54u}) {
            TrainingCommandV1 c =
                command(1, 1, TrainingCommandKind::SetExMeter);
            c.value = stocks;
            c.flags = fraction;
            const bool expected =
                stocks <= 9u && fraction <= 53u
                && !(stocks == 9u && fraction != 0u);
            QNP_CHECK(validCommandPayload(c) == expected);
        }
    }
    for (uint32_t value = 0; value <= 101u; value += 25u) {
        TrainingCommandV1 c =
            command(1, 1, TrainingCommandKind::SetGuardGaugeValue);
        c.value = value;
        QNP_CHECK(validCommandPayload(c) == (value <= 100u));
    }
    for (const TrainingCommandKind kind :
         {TrainingCommandKind::SetHealthPolicy,
          TrainingCommandKind::SetExPolicy,
          TrainingCommandKind::SetGuardGaugePolicy}) {
        for (uint32_t target = 0; target <= 3u; ++target) {
            for (uint32_t trigger = 0; trigger <= 5u; ++trigger) {
                TrainingCommandV1 c = command(1, 1, kind);
                c.slotOrSetting = target;
                c.value = trigger;
                QNP_CHECK(validCommandPayload(c)
                          == (target <= 2u && trigger <= 4u));
            }
        }
    }
}

QNP_TEST(TrainingFractionChargePlannerCarriesIntoTheNextStock) {
    int64_t lanes[kExPendingLaneCount] = {};
    for (int fraction = 0; fraction < kExFractionMax; ++fraction) {
        const ExFractionChargePlan plan =
            planExFractionCharge(0, 3, fraction, lanes);
        QNP_CHECK(plan.verdict == RefillVerdict::Apply);
        QNP_CHECK(plan.write);
        QNP_CHECK(plan.stocks == 3);
        QNP_CHECK(plan.fraction == fraction + 1);
    }
    {
        const ExFractionChargePlan fullFraction =
            planExFractionCharge(0, 3, kExFractionMax, lanes);
        QNP_CHECK(fullFraction.verdict == RefillVerdict::Apply);
        QNP_CHECK(fullFraction.write);
        QNP_CHECK(fullFraction.stocks == 4);
        QNP_CHECK(fullFraction.fraction == kExFractionFull);
    }
    {
        const ExFractionChargePlan fullStocks =
            planExFractionCharge(0, kExStocksFull, 0, lanes);
        QNP_CHECK(fullStocks.verdict == RefillVerdict::Apply);
        QNP_CHECK(fullStocks.write);
        QNP_CHECK(fullStocks.stocks == 0);
        QNP_CHECK(fullStocks.fraction == kExFractionFull);
    }
    QNP_CHECK(planExFractionCharge(
        kExSpendStateSingle, 3, 4, lanes).verdict == RefillVerdict::Busy);
    QNP_CHECK(planExFractionCharge(
        kExSpendStateMulti, 3, 4, lanes).verdict == RefillVerdict::Busy);
    lanes[2] = 1;
    QNP_CHECK(planExFractionCharge(
        0, 3, 4, lanes).verdict == RefillVerdict::Busy);
    lanes[2] = 0;
    QNP_CHECK(planExFractionCharge(
        0, 10, 0, lanes).verdict == RefillVerdict::Corrupt);
    QNP_CHECK(planExFractionCharge(
        0, 9, 1, lanes).verdict == RefillVerdict::Corrupt);
    QNP_CHECK(planExFractionCharge(
        0, 3, 54, lanes).verdict == RefillVerdict::Corrupt);
}

QNP_TEST(TrainingGuardAfterATechHopIsDecidedForTheLandingFrame) {
    const uint32_t left = qinput::kLogicalLeft;
    const uint32_t down = qinput::kLogicalDown;
    const uint32_t up = qinput::kLogicalUp;
    const auto decide = [](GuardMode mode, PostureClass posture,
                           bool techResult, uint32_t attackersGrounded) {
        GuardDecisionInput input{};
        input.mode = mode;
        input.defenderFacing = 0u;
        input.defenderPosture = posture;
        input.defenderAirborne = posture == PostureClass::Air;
        input.defenderInTechResult = techResult;
        input.attackerReadableCount = 1u;
        input.attackerGroundedCount = attackersGrounded;
        return chooseGuardDirections(input);
    };

    for (const GuardMode mode : {GuardMode::AutoHeuristic,
                                 GuardMode::AutoRecord,
                                 GuardMode::AlwaysCrouch}) {
        const GuardDecision jump =
            decide(mode, PostureClass::Air, false, 1u);
        QNP_CHECK(jump.guarding);
        QNP_CHECK(jump.logicalDirections == left);
        const GuardDecision tech =
            decide(mode, PostureClass::Air, true, 1u);
        QNP_CHECK(tech.guarding);
        QNP_CHECK(tech.logicalDirections == (left | down));
        QNP_CHECK(!tech.postureDivergedFromY);
    }
    QNP_CHECK(decide(GuardMode::AutoRecord, PostureClass::NotGuardable,
                     true, 1u).logicalDirections == (left | down));

    QNP_CHECK(decide(GuardMode::AutoHeuristic, PostureClass::Air, true, 0u)
                  .logicalDirections == left);
    QNP_CHECK(decide(GuardMode::AlwaysStand, PostureClass::Air, true, 1u)
                  .logicalDirections == left);
    QNP_CHECK(decide(GuardMode::AlwaysAir, PostureClass::Air, true, 1u)
                  .logicalDirections == (left | up));

    QNP_CHECK(decide(GuardMode::AutoRecord, PostureClass::Standing, true, 1u)
                  .logicalDirections == (left | down));
    QNP_CHECK(!decide(GuardMode::AutoHeuristic, PostureClass::Unreadable,
                      true, 1u).valid);
}

QNP_TEST(TrainingAutoHeuristicGuardsByAttackerPostureAndAutoStaysUnreleased) {
    QNP_CHECK(static_cast<uint32_t>(GuardMode::Auto) == 1u);
    QNP_CHECK(static_cast<uint32_t>(GuardMode::AutoHeuristic) == 4u);
    QNP_CHECK(!guardModeReleaseQualified(GuardMode::Auto, false));
    QNP_CHECK(guardModeReleaseQualified(GuardMode::Auto, true));
    QNP_CHECK(guardModeReleaseQualified(GuardMode::Off, false));
    QNP_CHECK(guardModeReleaseQualified(GuardMode::AlwaysStand, false));
    QNP_CHECK(guardModeReleaseQualified(GuardMode::AlwaysCrouch, false));
    QNP_CHECK(guardModeReleaseQualified(GuardMode::AutoHeuristic, false));
    QNP_CHECK(guardModeReleaseQualified(GuardMode::AlwaysAir, false));
    QNP_CHECK(guardModeReleaseQualified(GuardMode::AutoRecord, false));
    QNP_CHECK(!guardModeReleaseQualified(static_cast<GuardMode>(7), false));
    QNP_CHECK(!guardModeReleaseQualified(static_cast<GuardMode>(99), true));

    for (const bool threat : {false, true}) {
        GuardDecisionInput input{};
        input.mode = GuardMode::Auto;
        input.permission = kGuardPermitStand | kGuardPermitCrouch;
        input.threatPresent = threat;
        input.autoPredictionQualified = false;
        const GuardDecision decision = chooseGuardDirections(input);
        QNP_CHECK(decision.valid);
        QNP_CHECK(!decision.guarding);
        QNP_CHECK(decision.logicalDirections == 0u);
    }
    {
        GuardDecisionInput input{};
        input.mode = GuardMode::Auto;
        input.permission = kGuardPermitStand | kGuardPermitCrouch;
        input.autoPredictionQualified = true;
        input.threatPresent = false;
        QNP_CHECK(!chooseGuardDirections(input).guarding);
    }

    const uint32_t awayFacing0 = qinput::kLogicalLeft;
    const uint32_t awayFacing1 = qinput::kLogicalRight;
    struct HeuristicCase {
        const char* name;
        PostureClass defender;
        uint32_t readable;
        uint32_t grounded;
        uint8_t facing;
        uint32_t expected;
    };
    const HeuristicCase cases[] = {
        {"grounded attacker blocks low", PostureClass::Standing,
         1, 1, 0, awayFacing0 | qinput::kLogicalDown},
        {"grounded attacker, crouching defender", PostureClass::Crouching,
         1, 1, 0, awayFacing0 | qinput::kLogicalDown},
        {"grounded attacker, facing 1", PostureClass::Standing,
         1, 1, 1, awayFacing1 | qinput::kLogicalDown},
        {"airborne attacker blocks high", PostureClass::Standing,
         1, 0, 0, awayFacing0},
        {"airborne attacker, facing 1", PostureClass::Crouching,
         1, 0, 1, awayFacing1},
        {"airborne defender never adds Down", PostureClass::Air,
         1, 1, 0, awayFacing0},
        {"airborne defender, airborne attacker", PostureClass::Air,
         1, 0, 1, awayFacing1},
        {"unguardable defender posture", PostureClass::NotGuardable,
         1, 1, 0, awayFacing0},
        {"ambiguous attackers take the superset", PostureClass::Standing,
         2, 1, 0, awayFacing0 | qinput::kLogicalDown},
        {"no readable attacker still guards", PostureClass::Standing,
         0, 0, 0, awayFacing0 | qinput::kLogicalDown},
    };
    for (const HeuristicCase& entry : cases) {
        GuardDecisionInput input{};
        input.mode = GuardMode::AutoHeuristic;
        input.permission = kGuardPermitStand | kGuardPermitCrouch;
        input.defenderFacing = entry.facing;
        input.defenderPosture = entry.defender;
        input.attackerReadableCount = entry.readable;
        input.attackerGroundedCount = entry.grounded;
        const GuardDecision decision = chooseGuardDirections(input);
        if (!decision.valid || decision.logicalDirections != entry.expected) {
            std::printf("    [heuristic] %s -> %08X, want %08X\n",
                        entry.name, decision.logicalDirections,
                        entry.expected);
        }
        QNP_CHECK(decision.valid);
        QNP_CHECK(decision.guarding);
        QNP_CHECK(decision.logicalDirections == entry.expected);
        QNP_CHECK((decision.logicalDirections
                   & ~qinput::kLogicalDirectionMask) == 0u);
    }

    {
        GuardDecisionInput input{};
        input.mode = GuardMode::AutoHeuristic;
        input.permission = kGuardPermitStand | kGuardPermitCrouch;
        input.defenderPosture = PostureClass::Unreadable;
        input.attackerReadableCount = 1;
        input.attackerGroundedCount = 1;
        const GuardDecision decision = chooseGuardDirections(input);
        QNP_CHECK(!decision.valid);
        QNP_CHECK(!decision.guarding);
        QNP_CHECK(decision.logicalDirections == 0u);
    }

    {
        GuardDecisionInput input{};
        input.mode = GuardMode::AutoHeuristic;
        input.permission = kGuardPermitStand;
        input.defenderPosture = PostureClass::Air;
        input.defenderAirborne = false;
        input.attackerReadableCount = 1;
        input.attackerGroundedCount = 1;
        QNP_CHECK(chooseGuardDirections(input).postureDivergedFromY);
        input.defenderAirborne = true;
        QNP_CHECK(!chooseGuardDirections(input).postureDivergedFromY);
        input.defenderPosture = PostureClass::Standing;
        QNP_CHECK(chooseGuardDirections(input).postureDivergedFromY);

        input.defenderPosture = PostureClass::Standing;
        input.defenderAirborne = false;
        input.attackerReadableCount = 2;
        input.attackerGroundedCount = 1;
        QNP_CHECK(chooseGuardDirections(input).attackerPostureAmbiguous);
        input.attackerGroundedCount = 2;
        QNP_CHECK(!chooseGuardDirections(input).attackerPostureAmbiguous);
        input.attackerGroundedCount = 0;
        QNP_CHECK(!chooseGuardDirections(input).attackerPostureAmbiguous);

        input.attackerReadableCount = 1;
        input.attackerGroundedCount = 1;
        input.defenderPosture = PostureClass::NotGuardable;
        QNP_CHECK(chooseGuardDirections(input).defenderPostureUnguardable);
        input.defenderPosture = PostureClass::Standing;
        QNP_CHECK(!chooseGuardDirections(input).defenderPostureUnguardable);
    }

    for (const GuardMode mode : {GuardMode::Off, GuardMode::AlwaysStand,
                                 GuardMode::AlwaysCrouch,
                                 GuardMode::AlwaysAir}) {
        GuardDecisionInput input{};
        input.mode = mode;
        input.permission = kGuardPermitStand | kGuardPermitCrouch;
        input.defenderPosture = PostureClass::Unreadable;
        input.attackerReadableCount = 5;
        input.attackerGroundedCount = 3;
        const GuardDecision decision = chooseGuardDirections(input);
        QNP_CHECK(decision.valid);
        QNP_CHECK(!decision.postureDivergedFromY);
        QNP_CHECK(!decision.attackerPostureAmbiguous);
        QNP_CHECK(decision.guarding == (mode != GuardMode::Off));
    }

    QNP_CHECK(classifyPostureNibble(1) == PostureClass::Standing);
    QNP_CHECK(classifyPostureNibble(2) == PostureClass::Crouching);
    QNP_CHECK(classifyPostureNibble(4) == PostureClass::Air);
    for (const uint8_t nibble : {uint8_t{0}, uint8_t{3}, uint8_t{5},
                                 uint8_t{6}, uint8_t{7}}) {
        QNP_CHECK(classifyPostureNibble(nibble) == PostureClass::NotGuardable);
    }
    for (uint32_t high = 0; high < 32u; ++high) {
        const uint8_t byte = static_cast<uint8_t>((high << 3) | 2u);
        QNP_CHECK(classifyPostureNibble(byte) == PostureClass::Crouching);
    }
    QNP_CHECK(postureClassGrounded(PostureClass::Standing));
    QNP_CHECK(postureClassGrounded(PostureClass::Crouching));
    QNP_CHECK(!postureClassGrounded(PostureClass::Air));
    QNP_CHECK(!postureClassGrounded(PostureClass::NotGuardable));
    QNP_CHECK(!postureClassGrounded(PostureClass::Unreadable));
    for (uint32_t byte = 0; byte < 256u; ++byte) {
        QNP_CHECK(classifyPostureNibble(static_cast<uint8_t>(byte))
                  != PostureClass::Unreadable);
    }

    GuardMode mode = GuardMode::Off;
    uint32_t visitedAuto = 0;
    for (uint32_t step = 0; step < 32u; ++step) {
        mode = nextReleasedGuardMode(mode);
        if (mode == GuardMode::Auto) ++visitedAuto;
        QNP_CHECK(guardModeReleaseQualified(mode, false));
    }
    QNP_CHECK(visitedAuto == 0u);
    QNP_CHECK(nextReleasedGuardMode(GuardMode::Off) == GuardMode::AlwaysStand);
    QNP_CHECK(nextReleasedGuardMode(GuardMode::AlwaysStand)
              == GuardMode::AlwaysCrouch);
    QNP_CHECK(nextReleasedGuardMode(GuardMode::AlwaysCrouch)
              == GuardMode::AutoHeuristic);
    QNP_CHECK(nextReleasedGuardMode(GuardMode::AutoHeuristic)
              == GuardMode::AlwaysAir);
    QNP_CHECK(nextReleasedGuardMode(GuardMode::AlwaysAir)
              == GuardMode::AutoRecord);
    QNP_CHECK(nextReleasedGuardMode(GuardMode::AutoRecord) == GuardMode::Off);
    QNP_CHECK(guardModeReleaseQualified(
        nextReleasedGuardMode(GuardMode::Auto), false));

    for (uint32_t value = 0; value <= 8u; ++value) {
        TrainingCommandV1 c = command(1, 1, TrainingCommandKind::SetGuardMode);
        c.value = value;
        QNP_CHECK(validCommandPayload(c) == (value <= 6u));
    }
}

QNP_TEST(TrainingGuardOverrideBlocksInTheAirAndSplitsLowHigh) {
    const uint32_t away0 = qinput::kLogicalLeft;
    const uint32_t away1 = qinput::kLogicalRight;

    QNP_CHECK(static_cast<uint32_t>(GuardMode::AlwaysAir) == 5u);

    auto decide = [](GuardMode mode, uint8_t facing,
                     PostureClass defender) {
        GuardDecisionInput input{};
        input.mode = mode;
        input.permission = kGuardPermitStand | kGuardPermitCrouch;
        input.defenderFacing = facing;
        input.defenderPosture = defender;
        input.attackerReadableCount = 1;
        input.attackerGroundedCount = 1;
        return chooseGuardDirections(input);
    };

    for (const GuardMode mode : {GuardMode::AlwaysStand,
                                 GuardMode::AlwaysCrouch,
                                 GuardMode::AutoHeuristic,
                                 GuardMode::AlwaysAir}) {
        for (uint8_t facing = 0; facing <= 1u; ++facing) {
            const uint32_t away = facing == 0 ? away0 : away1;
            const GuardDecision decision =
                decide(mode, facing, PostureClass::Air);
            if (!decision.valid || !decision.guarding
                || (decision.logicalDirections & away) == 0u
                || (decision.logicalDirections & qinput::kLogicalDown) != 0u) {
                std::printf("    [air guard] mode %u facing %u -> %08X\n",
                            static_cast<unsigned>(mode),
                            static_cast<unsigned>(facing),
                            decision.logicalDirections);
            }
            QNP_CHECK(decision.valid);
            QNP_CHECK(decision.guarding);
            QNP_CHECK((decision.logicalDirections & away) != 0u);
            QNP_CHECK((decision.logicalDirections
                       & (facing == 0 ? away1 : away0)) == 0u);
            QNP_CHECK((decision.logicalDirections
                       & qinput::kLogicalDown) == 0u);
            QNP_CHECK((decision.logicalDirections
                       & ~qinput::kLogicalDirectionMask) == 0u);
        }
    }

    struct SplitCase {
        const char* name;
        GuardMode mode;
        PostureClass defender;
        uint8_t facing;
        uint32_t expected;
    };
    const SplitCase splits[] = {
        {"stand, grounded", GuardMode::AlwaysStand,
         PostureClass::Standing, 0, away0},
        {"stand, crouching", GuardMode::AlwaysStand,
         PostureClass::Crouching, 1, away1},
        {"stand, airborne", GuardMode::AlwaysStand,
         PostureClass::Air, 0, away0},
        {"stand, unreadable", GuardMode::AlwaysStand,
         PostureClass::Unreadable, 1, away1},

        {"crouch, standing", GuardMode::AlwaysCrouch,
         PostureClass::Standing, 0, away0 | qinput::kLogicalDown},
        {"crouch, crouching", GuardMode::AlwaysCrouch,
         PostureClass::Crouching, 1, away1 | qinput::kLogicalDown},
        {"crouch, unguardable nibble", GuardMode::AlwaysCrouch,
         PostureClass::NotGuardable, 0, away0 | qinput::kLogicalDown},
        {"crouch, unreadable", GuardMode::AlwaysCrouch,
         PostureClass::Unreadable, 1, away1 | qinput::kLogicalDown},
        {"crouch, airborne blocks high", GuardMode::AlwaysCrouch,
         PostureClass::Air, 0, away0},
        {"crouch, airborne facing 1", GuardMode::AlwaysCrouch,
         PostureClass::Air, 1, away1},

        {"air, standing", GuardMode::AlwaysAir,
         PostureClass::Standing, 0, away0 | qinput::kLogicalUp},
        {"air, crouching", GuardMode::AlwaysAir,
         PostureClass::Crouching, 1, away1 | qinput::kLogicalUp},
        {"air, airborne", GuardMode::AlwaysAir,
         PostureClass::Air, 0, away0 | qinput::kLogicalUp},
        {"air, unguardable nibble", GuardMode::AlwaysAir,
         PostureClass::NotGuardable, 1, away1 | qinput::kLogicalUp},
        {"air, unreadable", GuardMode::AlwaysAir,
         PostureClass::Unreadable, 0, away0 | qinput::kLogicalUp},

        {"heuristic, grounded defender blocks low", GuardMode::AutoHeuristic,
         PostureClass::Standing, 0, away0 | qinput::kLogicalDown},
        {"heuristic, airborne defender blocks high", GuardMode::AutoHeuristic,
         PostureClass::Air, 1, away1},
    };
    for (const SplitCase& entry : splits) {
        const GuardDecision decision =
            decide(entry.mode, entry.facing, entry.defender);
        if (!decision.valid
            || decision.logicalDirections != entry.expected) {
            std::printf("    [split] %s -> %08X, want %08X\n",
                        entry.name, decision.logicalDirections,
                        entry.expected);
        }
        QNP_CHECK(decision.valid);
        QNP_CHECK(decision.guarding);
        QNP_CHECK(decision.logicalDirections == entry.expected);
        QNP_CHECK((decision.logicalDirections
                   & ~qinput::kLogicalDirectionMask) == 0u);
    }

    for (uint8_t facing = 0; facing <= 1u; ++facing) {
        const uint32_t fixed =
            decide(GuardMode::AlwaysCrouch, facing,
                   PostureClass::Air).logicalDirections;
        const uint32_t heuristic =
            decide(GuardMode::AutoHeuristic, facing,
                   PostureClass::Air).logicalDirections;
        QNP_CHECK(fixed == heuristic);
        QNP_CHECK(fixed == (facing == 0 ? away0 : away1));
    }

    for (uint8_t facing = 0; facing <= 1u; ++facing) {
        const uint32_t away = facing == 0 ? away0 : away1;
        for (const PostureClass posed : {PostureClass::Standing,
                                         PostureClass::Air}) {
            for (const DummyStance stance : {DummyStance::Stand,
                                             DummyStance::Crouch,
                                             DummyStance::Jump}) {
                const DummyPostureResult result = posture(
                    GuardMode::AlwaysAir, stance, DummyTech::Off, facing,
                    posed);
                QNP_CHECK(result.valid);
                QNP_CHECK(result.directions == (away | qinput::kLogicalUp));
                QNP_CHECK(result.ownedAxes == qinput::kLogicalDirectionMask);
                QNP_CHECK(result.horizontalSource == DummyPostureSource::Guard);
                QNP_CHECK(result.verticalSource == DummyPostureSource::Guard);
                QNP_CHECK(result.buttons == 0u);
                QNP_CHECK(result.ownedButtons == 0u);
            }
        }
        const DummyPostureResult airborneCrouch = posture(
            GuardMode::AlwaysCrouch, DummyStance::Crouch, DummyTech::Off,
            facing, PostureClass::Air);
        QNP_CHECK(airborneCrouch.valid);
        QNP_CHECK(airborneCrouch.directions == away);
        QNP_CHECK(airborneCrouch.ownedAxes == qinput::kLogicalDirectionMask);
        QNP_CHECK(airborneCrouch.verticalSource == DummyPostureSource::Guard);
    }

    QNP_CHECK(classifyGuardPermission(kGuardPermitAir)
              == GuardClassification::Air);
    QNP_CHECK(classifyGuardPermission(kGuardPermitStand)
              == GuardClassification::Stand);
    QNP_CHECK(classifyGuardPermission(kGuardPermitCrouch)
              == GuardClassification::Crouch);

    QNP_CHECK(postureClassAirborne(PostureClass::Air));
    QNP_CHECK(!postureClassAirborne(PostureClass::Standing));
    QNP_CHECK(!postureClassAirborne(PostureClass::Crouching));
    QNP_CHECK(!postureClassAirborne(PostureClass::NotGuardable));
    QNP_CHECK(!postureClassAirborne(PostureClass::Unreadable));
    QNP_CHECK(!postureClassGrounded(PostureClass::NotGuardable));
    QNP_CHECK(!postureClassAirborne(PostureClass::NotGuardable));
    QNP_CHECK(!postureClassGrounded(PostureClass::Unreadable));
    QNP_CHECK(!postureClassAirborne(PostureClass::Unreadable));
}

QNP_TEST(TrainingPostureNibbleMasksStayDistinctPerNativeConsumer) {
    QNP_CHECK(classifyPostureNibble(1) == PostureClass::Standing);
    QNP_CHECK(classifyPostureNibble(2) == PostureClass::Crouching);
    QNP_CHECK(classifyPostureNibble(4) == PostureClass::Air);
    QNP_CHECK(classifyPostureNibble(9) == PostureClass::Standing);
    QNP_CHECK(classifyPostureNibble(0x0A) == PostureClass::Crouching);
    QNP_CHECK(classifyPostureNibble(0x0C) == PostureClass::Air);
    for (const uint8_t nibble : {uint8_t{0}, uint8_t{3}, uint8_t{5},
                                 uint8_t{6}, uint8_t{7}, uint8_t{0x0B},
                                 uint8_t{0x0D}, uint8_t{0x0E},
                                 uint8_t{0x0F}}) {
        QNP_CHECK(classifyPostureNibble(nibble) == PostureClass::NotGuardable);
    }

    QNP_CHECK(recoveryPostureAdmitsManual(1));
    QNP_CHECK(recoveryPostureAdmitsManual(2));
    QNP_CHECK(recoveryPostureAdmitsManual(4));
    QNP_CHECK(!recoveryPostureAdmitsManual(9));
    QNP_CHECK(!recoveryPostureAdmitsManual(0x0A));
    QNP_CHECK(!recoveryPostureAdmitsManual(0x0C));
    for (const uint8_t nibble : {uint8_t{0}, uint8_t{3}, uint8_t{5},
                                 uint8_t{6}, uint8_t{7}, uint8_t{8},
                                 uint8_t{0x0B}, uint8_t{0x0D},
                                 uint8_t{0x0E}, uint8_t{0x0F}}) {
        QNP_CHECK(!recoveryPostureAdmitsManual(nibble));
    }
    for (uint32_t high = 0; high < 16u; ++high) {
        const uint8_t byte = static_cast<uint8_t>((high << 4) | 4u);
        QNP_CHECK(recoveryPostureAdmitsManual(byte));
    }

    QNP_CHECK(recoveryPostureAdmitsTerminal(1));
    QNP_CHECK(recoveryPostureAdmitsTerminal(2));
    QNP_CHECK(!recoveryPostureAdmitsTerminal(4));
    QNP_CHECK(!recoveryPostureAdmitsTerminal(0x0C));
    QNP_CHECK(recoveryPostureAdmitsTerminal(9));
    QNP_CHECK(recoveryPostureAdmitsTerminal(0x0A));
    for (const uint8_t nibble : {uint8_t{0}, uint8_t{3}, uint8_t{5},
                                 uint8_t{6}, uint8_t{7}, uint8_t{0x0B},
                                 uint8_t{0x0D}, uint8_t{0x0E},
                                 uint8_t{0x0F}}) {
        QNP_CHECK(!recoveryPostureAdmitsTerminal(nibble));
    }
    for (uint32_t high = 0; high < 32u; ++high) {
        const uint8_t byte = static_cast<uint8_t>((high << 3) | 2u);
        QNP_CHECK(recoveryPostureAdmitsTerminal(byte));
    }

    QNP_CHECK(recoveryPostureAdmitsManual(9)
              != recoveryPostureAdmitsTerminal(9));
    QNP_CHECK(recoveryPostureAdmitsManual(0x0A)
              != recoveryPostureAdmitsTerminal(0x0A));
    QNP_CHECK((classifyPostureNibble(9) == PostureClass::Standing)
              == recoveryPostureAdmitsTerminal(9));
    QNP_CHECK((classifyPostureNibble(9) != PostureClass::NotGuardable)
              != recoveryPostureAdmitsManual(9));
    QNP_CHECK(classifyPostureNibble(0x0C) == PostureClass::Air);
    QNP_CHECK(!recoveryPostureAdmitsManual(0x0C));
}

QNP_TEST(TrainingHoldComposesMenuAndUserPauseWithoutLosingEither) {
    QNP_CHECK(TrainingHoldNone == 0u);
    QNP_CHECK(TrainingHoldUser == 1u);
    QNP_CHECK(TrainingHoldMenu == 2u);
    QNP_CHECK(kTrainingHoldReasonMask == 3u);
    QNP_CHECK(isCanonicalTrainingHoldReasons(TrainingHoldNone));
    QNP_CHECK(isCanonicalTrainingHoldReasons(kTrainingHoldReasonMask));
    QNP_CHECK(!isCanonicalTrainingHoldReasons(4u));
    QNP_CHECK(!isCanonicalTrainingHoldReasons(0xFFFFFFFFu));

    TrainingHold hold;
    hold.reset();
    QNP_CHECK(!hold.held());
    QNP_CHECK(hold.reasons() == TrainingHoldNone);
    QNP_CHECK(hold.admit().admit);
    QNP_CHECK(!hold.admit().consumedCredit);

    hold.reset();
    hold.toggleUserHold();
    QNP_CHECK(hold.held());
    QNP_CHECK(hold.reasons() == TrainingHoldUser);
    QNP_CHECK(!hold.admit().admit);
    hold.setMenuCaptured(true);
    QNP_CHECK(hold.reasons() == (TrainingHoldUser | TrainingHoldMenu));
    QNP_CHECK(!hold.admit().admit);
    hold.setMenuCaptured(false);
    QNP_CHECK(hold.reasons() == TrainingHoldUser);
    QNP_CHECK(hold.held());
    QNP_CHECK(!hold.admit().admit);
    hold.toggleUserHold();
    QNP_CHECK(!hold.held());
    QNP_CHECK(hold.admit().admit);

    hold.reset();
    QNP_CHECK(hold.admit().admit);
    hold.setMenuCaptured(true);
    QNP_CHECK(hold.reasons() == TrainingHoldMenu);
    QNP_CHECK(hold.held());
    QNP_CHECK(!hold.admit().admit);
    hold.setMenuCaptured(false);
    QNP_CHECK(hold.reasons() == TrainingHoldNone);
    QNP_CHECK(!hold.held());
    QNP_CHECK(hold.admit().admit);

    hold.reset();
    hold.setMenuCaptured(true);
    hold.toggleUserHold();
    QNP_CHECK(hold.reasons() == (TrainingHoldUser | TrainingHoldMenu));
    hold.setMenuCaptured(false);
    QNP_CHECK(hold.reasons() == TrainingHoldUser);
    QNP_CHECK(!hold.admit().admit);

    hold.reset();
    for (uint32_t frame = 0; frame < 100u; ++frame) hold.setMenuCaptured(true);
    QNP_CHECK(hold.reasons() == TrainingHoldMenu);
    for (uint32_t frame = 0; frame < 100u; ++frame) hold.setMenuCaptured(false);
    QNP_CHECK(hold.reasons() == TrainingHoldNone);

    hold.reset();
    hold.requestFrameStep();
    QNP_CHECK(hold.reasons() == TrainingHoldUser);
    QNP_CHECK(hold.frameStepCredits() == 1u);
    hold.setMenuCaptured(true);
    QNP_CHECK(hold.frameStepCredits() == 1u);
    hold.setMenuCaptured(false);
    QNP_CHECK(hold.frameStepCredits() == 1u);
    hold.setMenuCaptured(true);
    QNP_CHECK(hold.frameStepCredits() == 1u);
    TrainingHoldAdmission stepped = hold.admit();
    QNP_CHECK(stepped.admit);
    QNP_CHECK(stepped.consumedCredit);
    QNP_CHECK(stepped.reasons == (TrainingHoldUser | TrainingHoldMenu));
    QNP_CHECK(hold.frameStepCredits() == 0u);
    QNP_CHECK(!hold.admit().admit);

    hold.reset();
    hold.setMenuCaptured(true);
    QNP_CHECK(!hold.admit().admit);
    hold.requestFrameStep();
    QNP_CHECK(hold.reasons() == TrainingHoldMenu);
    QNP_CHECK(hold.admit().consumedCredit);
    QNP_CHECK(!hold.admit().admit);
    hold.setMenuCaptured(false);
    QNP_CHECK(hold.reasons() == TrainingHoldNone);
    QNP_CHECK(hold.admit().admit);

    hold.reset();
    for (uint32_t press = 0; press < 5u; ++press) {
        hold.requestFrameStep();
        QNP_CHECK(hold.heldFor(TrainingHoldUser));
    }
    QNP_CHECK(hold.frameStepCredits() == 1u);
    QNP_CHECK(hold.admit().consumedCredit);
    QNP_CHECK(!hold.admit().admit);

    hold.reset();
    hold.requestFrameStep();
    QNP_CHECK(hold.frameStepCredits() == 1u);
    hold.toggleUserHold();
    QNP_CHECK(!hold.heldFor(TrainingHoldUser));
    QNP_CHECK(hold.frameStepCredits() == 0u);
    QNP_CHECK(hold.admit().admit);
    QNP_CHECK(!hold.admit().consumedCredit);

    hold.reset();
    for (uint32_t frame = 0; frame < 1000u; ++frame) {
        const TrainingHoldAdmission admission = hold.admit();
        QNP_CHECK(admission.admit);
        QNP_CHECK(!admission.consumedCredit);
        QNP_CHECK(admission.reasons == TrainingHoldNone);
    }

    const char* both =
        trainingHoldStatusText(TrainingHoldUser | TrainingHoldMenu);
    const char* menuOnly = trainingHoldStatusText(TrainingHoldMenu);
    const char* userOnly = trainingHoldStatusText(TrainingHoldUser);
    const char* running = trainingHoldStatusText(TrainingHoldNone);
    QNP_CHECK(running[0] == '\0');
    QNP_CHECK(both[0] != '\0' && menuOnly[0] != '\0' && userOnly[0] != '\0');
    QNP_CHECK(std::strcmp(both, menuOnly) != 0);
    QNP_CHECK(std::strcmp(both, userOnly) != 0);
    QNP_CHECK(std::strcmp(menuOnly, userOnly) != 0);
    QNP_CHECK(std::strstr(both, "still paused") != nullptr);
    QNP_CHECK(std::strstr(userOnly, "step one frame") != nullptr);
    QNP_CHECK(std::strstr(menuOnly, "press 9") == nullptr);

    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::TogglePause));
    QNP_CHECK(!commandMayCoalesce(TrainingCommandKind::FrameStep));
}

QNP_TEST(TrainingHeldFrameSimulatesNothingButIsStillPresentEligible) {
    TrainingHold hold;

    hold.reset();
    for (uint32_t frame = 0; frame < 100u; ++frame) {
        const TrainingHoldAdmission running = hold.admit();
        QNP_CHECK(running.admit);
        QNP_CHECK(!running.presentHeldFrame);
        QNP_CHECK(!running.consumedCredit);
    }

    hold.reset();
    hold.toggleUserHold();
    for (uint32_t frame = 0; frame < 100u; ++frame) {
        const TrainingHoldAdmission user = hold.admit();
        QNP_CHECK(!user.admit);
        QNP_CHECK(user.presentHeldFrame);
        QNP_CHECK(!user.consumedCredit);
        QNP_CHECK(user.reasons == TrainingHoldUser);
    }
    hold.reset();
    hold.setMenuCaptured(true);
    for (uint32_t frame = 0; frame < 100u; ++frame) {
        const TrainingHoldAdmission menu = hold.admit();
        QNP_CHECK(!menu.admit);
        QNP_CHECK(menu.presentHeldFrame);
        QNP_CHECK(menu.reasons == TrainingHoldMenu);
    }
    hold.toggleUserHold();
    for (uint32_t frame = 0; frame < 100u; ++frame) {
        const TrainingHoldAdmission both2 = hold.admit();
        QNP_CHECK(!both2.admit);
        QNP_CHECK(both2.presentHeldFrame);
        QNP_CHECK(both2.reasons == (TrainingHoldUser | TrainingHoldMenu));
    }

    hold.reset();
    hold.setMenuCaptured(true);
    const TrainingHoldAdmission beforeStep = hold.admit();
    QNP_CHECK(!beforeStep.admit);
    QNP_CHECK(beforeStep.presentHeldFrame);

    hold.requestFrameStep();
    const TrainingHoldAdmission stepped = hold.admit();
    QNP_CHECK(stepped.admit);
    QNP_CHECK(stepped.consumedCredit);
    QNP_CHECK(!stepped.presentHeldFrame);
    QNP_CHECK(stepped.reasons == TrainingHoldMenu);

    const TrainingHoldAdmission afterStep = hold.admit();
    QNP_CHECK(!afterStep.admit);
    QNP_CHECK(afterStep.presentHeldFrame);
    QNP_CHECK(!afterStep.consumedCredit);
    hold.setMenuCaptured(false);
    QNP_CHECK(hold.reasons() == TrainingHoldNone);
    QNP_CHECK(hold.admit().admit);

    hold.reset();
    hold.toggleUserHold();
    QNP_CHECK(hold.admit().presentHeldFrame);
    hold.setMenuCaptured(true);
    QNP_CHECK(hold.admit().presentHeldFrame);
    hold.setMenuCaptured(false);
    QNP_CHECK(hold.reasons() == TrainingHoldUser);
    QNP_CHECK(hold.admit().presentHeldFrame);
    QNP_CHECK(hold.frameStepCredits() == 0u);
    hold.toggleUserHold();
    const TrainingHoldAdmission resumed = hold.admit();
    QNP_CHECK(resumed.admit);
    QNP_CHECK(!resumed.presentHeldFrame);
}

#if 0
QNP_TEST(TrainingFunctionKeysBindOurCommandsAndNothingBindsF5) {
    struct Binding {
        uint8_t dik;
        TrainingAction action;
        const char* label;
    };
    const Binding expected[] = {
        {0x02u, TrainingAction::ResetPosition, "1"},
        {0x03u, TrainingAction::SaveState, "2"},
        {0x04u, TrainingAction::LoadState, "3"},
        {0x05u, TrainingAction::RecordStop, "4"},
        {0x06u, TrainingAction::PlayStop, "5"},
        {0x07u, TrainingAction::PreviousSlot, "6"},
        {0x08u, TrainingAction::NextSlot, "7"},
        {0x09u, TrainingAction::TogglePause, "8"},
        {0x0Au, TrainingAction::FrameStep, "9"},
        {0x3Bu, TrainingAction::ReturnToCharacterSelect, "F1"},
        {0x3Cu, TrainingAction::CycleDummyStance, "F2"},
        {0x3Du, TrainingAction::CycleGuardMode, "F3"},
        {0x3Eu, TrainingAction::RefillAll, "F4"},
        {0x40u, TrainingAction::ResetPositionRoundStart, "F6"},
        {0x41u, TrainingAction::ToggleHitboxes, "F7"},
        {0x42u, TrainingAction::TogglePause, "F8"},
        {0x43u, TrainingAction::FrameStep, "F9"},
    };
    const uint32_t expectedCount =
        sizeof(expected) / sizeof(expected[0]);
    QNP_REQUIRE(kTrainingHotkeyCount == expectedCount);
    for (uint32_t i = 0; i < expectedCount; ++i) {
        if (kTrainingHotkeys[i].dik != expected[i].dik
            || kTrainingHotkeys[i].action != expected[i].action) {
            std::printf("    [hotkey] slot %u is DIK %02X, expected %s\n",
                        i, kTrainingHotkeys[i].dik, expected[i].label);
        }
        QNP_CHECK(kTrainingHotkeys[i].dik == expected[i].dik);
        QNP_CHECK(kTrainingHotkeys[i].action == expected[i].action);
    }
    for (uint32_t digit = 0; digit < 9u; ++digit) {
        QNP_CHECK(kTrainingHotkeys[digit].dik
                  == static_cast<uint8_t>(0x02u + digit));
        QNP_CHECK(static_cast<uint32_t>(kTrainingHotkeys[digit].action)
                  < 9u);
    }
    QNP_CHECK(trainingHotkeysCoverEveryAction());
    for (uint32_t i = 0; i < kTrainingHotkeyCount; ++i) {
        for (uint32_t j = i + 1u; j < kTrainingHotkeyCount; ++j)
            QNP_CHECK(kTrainingHotkeys[i].dik != kTrainingHotkeys[j].dik);
    }

    const uint8_t kF5 = 0x3Fu;
    const uint8_t kF10 = 0x44u;
    const uint8_t kF11 = 0x57u;
    const uint8_t kF12 = 0x58u;
    const uint8_t kDelete = 0xD3u;
    for (uint32_t i = 0; i < kTrainingHotkeyCount; ++i) {
        QNP_CHECK(kTrainingHotkeys[i].dik != kF5);
        QNP_CHECK(kTrainingHotkeys[i].dik != kF10);
        QNP_CHECK(kTrainingHotkeys[i].dik != kF11);
        QNP_CHECK(kTrainingHotkeys[i].dik != kF12);
        QNP_CHECK(kTrainingHotkeys[i].dik != kDelete);
    }
    QNP_CHECK(trainingHotkeysAvoidForbiddenKeys());
    const uint32_t forbiddenCount =
        sizeof(kTrainingForbiddenHotkeys) / sizeof(kTrainingForbiddenHotkeys[0]);
    QNP_CHECK(forbiddenCount == 5u);
    bool sawF5 = false;
    bool sawF12 = false;
    bool sawDelete = false;
    for (uint32_t i = 0; i < forbiddenCount; ++i) {
        if (kTrainingForbiddenHotkeys[i] == kF5) sawF5 = true;
        if (kTrainingForbiddenHotkeys[i] == kF12) sawF12 = true;
        if (kTrainingForbiddenHotkeys[i] == kDelete) sawDelete = true;
    }
    QNP_CHECK(sawF5);
    QNP_CHECK(sawF12);
    QNP_CHECK(sawDelete);

    for (const TrainingCommandKind kind :
         {TrainingCommandKind::RefillLife,
          TrainingCommandKind::RefillGuardGauge,
          TrainingCommandKind::RefillExMeter}) {
        QNP_CHECK(isValidCommandKind(kind));
        QNP_CHECK(!commandMayCoalesce(kind));
        QNP_CHECK(!commandOwnsNativeImageTransaction(kind));
    }
    QNP_CHECK(commandMayCoalesce(TrainingCommandKind::SetHitboxDisplay));
    {
        TrainingCommandV1 reset =
            command(1, 1, TrainingCommandKind::ResetPosition);
        reset.value = static_cast<uint32_t>(PositionPreset::RoundStart);
        QNP_CHECK(validCommandPayload(reset));
        QNP_CHECK(commandOwnsNativeImageTransaction(
            TrainingCommandKind::ResetPosition));
    }
    QNP_CHECK(nextDummyStance(DummyStance::Stand) == DummyStance::Crouch);
    QNP_CHECK(nextDummyStance(DummyStance::Crouch) == DummyStance::Jump);
    QNP_CHECK(nextDummyStance(DummyStance::Jump) == DummyStance::Stand);
    QNP_CHECK(dummyStanceValid(nextDummyStance(
        static_cast<DummyStance>(99))));
    {
        DummyStance stance = DummyStance::Stand;
        for (uint32_t press = 0; press < 30u; ++press) {
            stance = nextDummyStance(stance);
            QNP_CHECK(dummyStanceValid(stance));
        }
        QNP_CHECK(stance == DummyStance::Stand);
    }

    const uint32_t newActions[] = {
        trainingActionBit(TrainingAction::RefillAll),
        trainingActionBit(TrainingAction::ToggleHitboxes),
        trainingActionBit(TrainingAction::ResetPositionRoundStart),
        trainingActionBit(TrainingAction::CycleDummyStance),
        trainingActionBit(TrainingAction::CycleGuardMode),
    };
    for (const uint32_t bit : newActions) {
        TrainingInput input;
        input.reset();
        uint64_t poll = 0;
        QNP_REQUIRE(armInput(input, poll).rearmState
                    == InputRearmState::Armed);

        TrainingActionResult actions = pollInput(input, poll, bit, 0, true);
        QNP_CHECK(actions.valid);
        QNP_CHECK(actions.actionEdges == bit);
        QNP_CHECK(actions.resetModifier == 0u);

        TrainingActionPoll same{};
        same.lifecycleAvailable = true;
        same.relevantPhysicalNeutralBeforeSocd = true;
        same.heldTrainingActions = bit;
        const TrainingActionResult duplicate = input.pollActions(poll, same);
        QNP_CHECK(duplicate.duplicatePoll);
        QNP_CHECK(duplicate.actionEdges == 0u);

        uint32_t fired = 0;
        for (uint32_t frame = 0; frame < 180u; ++frame) {
            actions = pollInput(input, poll, bit, 0, true);
            if (actions.actionEdges & bit) ++fired;
        }
        QNP_CHECK(fired == 0u);

        QNP_CHECK(pollInput(input, poll, 0, 0, true).actionEdges == 0u);
        QNP_CHECK(pollInput(input, poll, bit, 0, true).actionEdges == bit);

        TrainingActionPoll captured{};
        captured.lifecycleAvailable = true;
        captured.menuCaptured = true;
        captured.heldTrainingActions = bit;
        actions = input.pollActions(++poll, captured);
        QNP_CHECK(actions.rearmState == InputRearmState::Captured);
        QNP_CHECK(actions.actionEdges == 0u);
        (void)pollInput(input, poll, bit, 0, true);
        actions = pollInput(input, poll, bit, 0, true);
        QNP_CHECK(actions.rearmState == InputRearmState::Armed);
        QNP_CHECK(pollInput(input, poll, bit, 0, true).actionEdges == 0u);
    }

    {
        TrainingInput input;
        input.reset();
        uint64_t poll = 0;
        armInput(input, poll);
        uint32_t all = 0;
        for (const uint32_t bit : newActions) all |= bit;
        const TrainingActionResult actions =
            pollInput(input, poll, all, 0, true);
        QNP_CHECK(actions.actionEdges == all);
        QNP_CHECK(isCanonicalTrainingActions(actions.actionEdges));
    }
    {
        TrainingInput input;
        input.reset();
        uint64_t poll = 0;
        armInput(input, poll);
        const uint32_t pause = trainingActionBit(TrainingAction::TogglePause);
        const TrainingActionResult actions =
            pollInput(input, poll, pause, 0, true);
        QNP_CHECK(actions.actionEdges == pause);
    }

    for (const uint32_t bit : newActions) {
        TrainingInput input;
        input.reset();
        uint64_t serial = 0;
        armInput(input, serial);
        QNP_CHECK(pollInput(input, serial, bit, 0, true).actionEdges == bit);

        TrainingInputStep step{};
        step.lifecycleAvailable = true;
        step.p1PhysicalMask = qinput::kLogicalRight | qinput::kLogicalA;
        step.p2PhysicalMask = qinput::kLogicalDown;
        const TrainingInputResult composed = input.advance(++serial, step);
        QNP_CHECK(composed.valid);
        QNP_CHECK(composed.p1Mask
                  == (qinput::kLogicalRight | qinput::kLogicalA));
        QNP_CHECK(composed.p2Mask == qinput::kLogicalDown);
        QNP_CHECK((composed.p1Mask & bit) == 0u);
        QNP_CHECK((composed.p2Mask & bit) == 0u);
        QNP_CHECK(qinput::isCanonicalLogicalFightInput(composed.p1Mask));
        QNP_CHECK(qinput::isCanonicalLogicalFightInput(composed.p2Mask));
        QNP_CHECK((composed.p1Mask & ~qinput::kLogicalFightMask) == 0u);
        QNP_CHECK((composed.p2Mask & ~qinput::kLogicalFightMask) == 0u);
        QNP_CHECK(qinput::logicalFromCompact(
                      qinput::compactFromLogical(composed.p1Mask))
                  == composed.p1Mask);
        QNP_CHECK(qinput::logicalFromCompact(
                      qinput::compactFromLogical(composed.p2Mask))
                  == composed.p2Mask);
    }
    {
        TrainingInput input;
        input.reset();
        uint64_t serial = 0;
        armInput(input, serial);
        (void)pollInput(input, serial, kTrainingActionMask, 0, true);
        TrainingInputStep step{};
        step.lifecycleAvailable = true;
        const TrainingInputResult composed = input.advance(++serial, step);
        QNP_CHECK(composed.valid);
        QNP_CHECK(composed.p1Mask == 0u);
        QNP_CHECK(composed.p2Mask == 0u);
        QNP_CHECK(composed.neutralized);
    }
    {
        TrainingInput input;
        input.reset();
        uint64_t poll = 0;
        TrainingActionPoll held{};
        held.lifecycleAvailable = true;
        held.relevantPhysicalNeutralBeforeSocd = true;
        held.heldTrainingActions = kTrainingActionMask;
        (void)input.pollActions(++poll, held);
        const TrainingActionResult armed = input.pollActions(++poll, held);
        QNP_CHECK(armed.rearmState == InputRearmState::Armed);
    }
}

#endif

QNP_TEST(TrainingLevelActionsAreChargeAndHeldRefillOnly) {
    QNP_CHECK(kTrainingActionCount == 16u);
    QNP_CHECK(kTrainingLevelActionMask
              == (trainingActionBit(TrainingAction::ChargeExFraction)
                  | trainingActionBit(TrainingAction::RefillAll)));
    QNP_CHECK((trainingImageMutatingActionMask()
               & trainingActionBit(TrainingAction::RefillAll)) == 0u);
    QNP_CHECK((trainingImageMutatingActionMask()
               & kTrainingLevelActionMask) == 0u);
    QNP_CHECK(isValidCommandKind(
        TrainingCommandKind::SetMeterChargeConfig));
    QNP_CHECK(commandMayCoalesce(
        TrainingCommandKind::SetMeterChargeConfig));
}

QNP_TEST(TrainingFractionChargeCadenceCountsOnlyAdmittedSteps) {
    MeterChargeCadencePlan cadence{};

    cadence = planMeterChargeCadence(0u, true, true, 2u);
    QNP_CHECK(cadence.nextCounter == 1u);
    QNP_CHECK(!cadence.fire);
    cadence = planMeterChargeCadence(cadence.nextCounter, true, false, 2u);
    QNP_CHECK(cadence.nextCounter == 1u);
    QNP_CHECK(!cadence.fire);
    cadence = planMeterChargeCadence(cadence.nextCounter, true, true, 2u);
    QNP_CHECK(cadence.nextCounter == 0u);
    QNP_CHECK(cadence.fire);

    uint32_t counter = 0u;
    for (uint32_t admitted = 1u; admitted <= 8u; ++admitted) {
        cadence = planMeterChargeCadence(counter, true, true, 2u);
        counter = cadence.nextCounter;
        QNP_CHECK(cadence.fire == ((admitted % 2u) == 0u));
    }

    cadence = planMeterChargeCadence(1u, false, false, 2u);
    QNP_CHECK(cadence.nextCounter == 0u);
    QNP_CHECK(!cadence.fire);
    cadence = planMeterChargeCadence(cadence.nextCounter, true, true, 2u);
    QNP_CHECK(cadence.nextCounter == 1u);
    QNP_CHECK(!cadence.fire);

    cadence = planMeterChargeCadence(59u, true, true, 0u);
    QNP_CHECK(cadence.nextCounter == 0u);
    QNP_CHECK(!cadence.fire);
    cadence = planMeterChargeCadence(61u, true, true, 61u);
    QNP_CHECK(cadence.nextCounter == 0u);
    QNP_CHECK(!cadence.fire);
}

QNP_TEST(TrainingFighterTierWrapperStillReadsEveryFighterFieldFrom1072) {
    QNP_CHECK(sizeof(FighterTierTrainingStatusV1)
              == kTrainingStatusV1FighterTier);
    QNP_CHECK(offsetof(FighterTierTrainingStatusV1, combat) == 0u);
    QNP_CHECK(offsetof(FighterTierTrainingStatusV1, p1LifeCurrent)
              == kTrainingStatusV1CombatTier);
    QNP_CHECK(offsetof(FighterTierTrainingStatusV1, fighterReadoutValid)
              == 728u);
    QNP_CHECK(offsetof(FighterTierTrainingStatusV1, p2PositionY) == 808u);

    TrainingStatusV1 modern{};
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&modern);
    for (uint32_t offset = 0; offset < kTrainingStatusV1FighterTier; ++offset)
        bytes[offset] = static_cast<unsigned char>(7u + (offset * 71u) % 239u);
    std::memset(bytes + kTrainingStatusV1FighterTier, 0xEE,
                sizeof(modern) - kTrainingStatusV1FighterTier);
    modern.apiVersion = kTrainingApiVersion;

    const uint32_t outSize = sizeof(FighterTierTrainingStatusV1);
    const uint32_t copied = trainingStatusTierFor(outSize);
    QNP_CHECK(copied == kTrainingStatusV1FighterTier);
    QNP_CHECK(copied > kTrainingStatusV1CombatTier);
    modern.structSize = copied;

    FighterTierTrainingStatusV1 fighter;
    std::memset(&fighter, 0xCD, sizeof(fighter));
    std::memcpy(&fighter, &modern, copied);

    QNP_CHECK(fighter.combat.frozen.apiVersion == kTrainingApiVersion);
    QNP_CHECK(fighter.combat.frozen.structSize
              == kTrainingStatusV1FighterTier);
    QNP_CHECK(fighter.combat.frozen.structSize >= sizeof(fighter));

#define QNP_FIGHTER_FIELD(field)                                       \
    QNP_CHECK(offsetof(TrainingStatusV1, field)                        \
              == offsetof(FighterTierTrainingStatusV1, field));        \
    QNP_CHECK(modern.field == fighter.field)

    QNP_FIGHTER_FIELD(p1LifeCurrent);
    QNP_FIGHTER_FIELD(p1LifePendingDamage);
    QNP_FIGHTER_FIELD(p1LifeReserve);
    QNP_FIGHTER_FIELD(p1LifeSettleTimer);
    QNP_FIGHTER_FIELD(p2LifeCurrent);
    QNP_FIGHTER_FIELD(p2LifePendingDamage);
    QNP_FIGHTER_FIELD(p2LifeReserve);
    QNP_FIGHTER_FIELD(p2LifeSettleTimer);
    QNP_FIGHTER_FIELD(fighterReadoutValid);
    QNP_FIGHTER_FIELD(p1ActionState);
    QNP_FIGHTER_FIELD(p1ActionFrame);
    QNP_FIGHTER_FIELD(p1ActionLockEnd);
    QNP_FIGHTER_FIELD(p1AnimFrame);
    QNP_FIGHTER_FIELD(p1NextAction);
    QNP_FIGHTER_FIELD(p1TransitionFlag);
    QNP_FIGHTER_FIELD(p1PrevAction);
    QNP_FIGHTER_FIELD(p1MoveGateByte);
    QNP_FIGHTER_FIELD(p1PositionX);
    QNP_FIGHTER_FIELD(p1PositionY);
    QNP_FIGHTER_FIELD(p2ActionState);
    QNP_FIGHTER_FIELD(p2ActionFrame);
    QNP_FIGHTER_FIELD(p2ActionLockEnd);
    QNP_FIGHTER_FIELD(p2AnimFrame);
    QNP_FIGHTER_FIELD(p2NextAction);
    QNP_FIGHTER_FIELD(p2TransitionFlag);
    QNP_FIGHTER_FIELD(p2PrevAction);
    QNP_FIGHTER_FIELD(p2MoveGateByte);
    QNP_FIGHTER_FIELD(p2PositionX);
    QNP_FIGHTER_FIELD(p2PositionY);

#undef QNP_FIGHTER_FIELD

    QNP_CHECK(modern.available == fighter.combat.frozen.available);
    QNP_CHECK(modern.p1FinalLogicalMask
              == fighter.combat.frozen.p1FinalLogicalMask);
    QNP_CHECK(modern.combatReadoutValid == fighter.combat.combatReadoutValid);
    QNP_CHECK(modern.p2MaxComboDamageDealt
              == fighter.combat.p2MaxComboDamageDealt);
    QNP_CHECK(std::memcmp(modern.resultText,
                          fighter.combat.frozen.resultText,
                          sizeof(fighter.combat.frozen.resultText)) == 0);

    QNP_CHECK(std::memcmp(&fighter, &modern,
                          kTrainingStatusV1FighterTier) == 0);
    QNP_CHECK(bytes[kTrainingStatusV1FighterTier] == 0xEE);
    QNP_CHECK(modern.holdReasons == 0xEEEEEEEEu);
    QNP_CHECK(modern.resourcePolicyValid == 0xEEEEEEEEu);
    QNP_CHECK(modern.recoveryReadoutValid == 0xEEEEEEEEu);
    QNP_CHECK(modern.recoveryContactTestUnknown == 0xEEEEEEEEu);

    FighterTierTrainingStatusV1 stale;
    std::memset(&stale, 0, sizeof(stale));
    stale.combat.frozen.apiVersion = kTrainingApiVersion;
    stale.combat.frozen.structSize = sizeof(FighterTierTrainingStatusV1);
    stale.combat.frozen.available = 1u;
    stale.combat.combatReadoutValid = 1u;
    stale.combat.p1EffectiveLife = 4321u;
    stale.fighterReadoutValid = 1u;
    stale.p1ActionState = 25u;
    stale.p2MoveGateByte = 4u;
    stale.p2PositionY = 0x0000BEEFu;

    TrainingStatusV1 consumer{};
    std::memset(&consumer, 0, sizeof(consumer));
    std::memcpy(&consumer, &stale, stale.combat.frozen.structSize);
    QNP_CHECK(consumer.structSize == kTrainingStatusV1FighterTier);
    QNP_CHECK(consumer.structSize >= kTrainingStatusV1FighterTier);
    QNP_CHECK(consumer.structSize < kTrainingStatusV1RecoveryTier);
    QNP_CHECK(consumer.fighterReadoutValid == 1u);
    QNP_CHECK(consumer.p1ActionState == 25u);
    QNP_CHECK(consumer.p2MoveGateByte == 4u);
    QNP_CHECK(consumer.p2PositionY == 0x0000BEEFu);
    QNP_CHECK(consumer.combatReadoutValid == 1u);
    QNP_CHECK(consumer.p1EffectiveLife == 4321u);
    QNP_CHECK(consumer.resourcePolicyValid == 0u);
    QNP_CHECK(consumer.recoveryReadoutValid == 0u);
    QNP_CHECK(consumer.holdReasons == TrainingHoldNone);
    const unsigned char* tail =
        reinterpret_cast<const unsigned char*>(&consumer);
    for (uint32_t offset = kTrainingStatusV1FighterTier;
         offset < sizeof(consumer); ++offset) {
        QNP_CHECK(tail[offset] == 0u);
    }
    QNP_CHECK(consumer.structSize < sizeof(TrainingStatusV1));
    QNP_CHECK(!(consumer.structSize >= kTrainingStatusV1RecoveryTier));
    QNP_CHECK(consumer.structSize >= kTrainingStatusV1CombatTier);

    QNP_CHECK(trainingStatusTierFor(0) == 0u);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1BaseSize - 1u) == 0u);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1BaseSize)
              == kTrainingStatusV1BaseSize);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1CombatTier - 1u)
              == kTrainingStatusV1BaseSize);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1FighterTier - 1u)
              == kTrainingStatusV1CombatTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1RecoveryTier - 1u)
              == kTrainingStatusV1FighterTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1RecoveryTier)
              == kTrainingStatusV1RecoveryTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1WallTier - 1u)
              == kTrainingStatusV1RecoveryTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1WallTier)
              == kTrainingStatusV1WallTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1RecoveryDiagTier - 1u)
              == kTrainingStatusV1WallTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1RecoveryDiagTier)
              == kTrainingStatusV1RecoveryDiagTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1CombatToolsTier - 1u)
              == kTrainingStatusV1MeterChargeTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1CombatToolsTier)
              == kTrainingStatusV1CombatToolsTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1ActionTriggersTier - 1u)
              == kTrainingStatusV1CombatToolsTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1ActionTriggersTier)
              == kTrainingStatusV1ActionTriggersTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1DebugOverlayTier - 1u)
              == kTrainingStatusV1ActionTriggersTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1DebugOverlayTier)
              == kTrainingStatusV1DebugOverlayTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1StateLogTier - 1u)
              == kTrainingStatusV1DebugOverlayTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1StateLogTier)
              == kTrainingStatusV1StateLogTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1ActorPoolTier - 1u)
              == kTrainingStatusV1StateLogTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1ActorPoolTier)
              == kTrainingStatusV1ActorPoolTier);
    QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1NativeHudTier - 1u)
              == kTrainingStatusV1ActorPoolTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1NativeHudRequestedTier - 1u)
              == kTrainingStatusV1NativeHudTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1FrameTraceTier - 1u)
              == kTrainingStatusV1NativeHudRequestedTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1FrameAdvantageGapTier - 1u)
              == kTrainingStatusV1FrameTraceTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1ContactPolicyTier - 1u)
              == kTrainingStatusV1FrameAdvantageGapTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1ExRestoreTargetTier - 1u)
              == kTrainingStatusV1ContactPolicyTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1MacroAfterResetTier - 1u)
              == kTrainingStatusV1ExRestoreTargetTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1ReducePolicyTier - 1u)
              == kTrainingStatusV1MacroAfterResetTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1GuardTimingTier - 1u)
              == kTrainingStatusV1ReducePolicyTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1PlayerSwapTier - 1u)
              == kTrainingStatusV1GuardTimingTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1StatusMessageTier - 1u)
              == kTrainingStatusV1PlayerSwapTier);
    QNP_CHECK(trainingStatusTierFor(
                  kTrainingStatusV1StatusMessageTier + 4096u)
              == kTrainingStatusV1StatusMessageTier);
    for (uint32_t i = 0; i < kTrainingStatusV1TierCount; ++i) {
        QNP_CHECK(trainingStatusTierFor(kTrainingStatusV1SizeTiers[i])
                  == kTrainingStatusV1SizeTiers[i]);
        QNP_CHECK(kTrainingStatusV1SizeTiers[i] % sizeof(uint32_t) == 0u);
    }
}

QNP_TEST(TrainingStatusPublisherCarriesTheWholeRecoveryTier) {
    TrainingStatusPublisher publisher;
    TrainingStatusV1 source{};
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&source);
    for (uint32_t offset = 0; offset < sizeof(source); ++offset)
        bytes[offset] = static_cast<unsigned char>(11u + (offset * 29u) % 233u);
    source.apiVersion = kTrainingApiVersion;
    source.structSize = sizeof(source);
    source.holdReasons = TrainingHoldUser | TrainingHoldMenu;
    source.recoveryReadoutValid = 1u;
    source.recoveryEligibleMask =
        recoveryBranchBit(RecoveryBranch::Wall66);
    source.recoveryLastVerdict =
        static_cast<uint32_t>(RecoveryVerdict::BufferedObserved);
    source.resourcePolicyValid = 1u;
    source.bothFightersNeutral = 1u;
    source.recoveryContactTestUnknown = 1u;
    publisher.publish(source);

    TrainingStatusV1 out{};
    std::memset(&out, 0xCD, sizeof(out));
    QNP_CHECK(publisher.read(out));
    TrainingStatusV1 expected = source;
    expected.apiVersion = kTrainingApiVersion;
    expected.structSize = sizeof(TrainingStatusV1);
    expected.disableText[sizeof(expected.disableText) - 1] = '\0';
    expected.statusText[sizeof(expected.statusText) - 1] = '\0';
    expected.resultText[sizeof(expected.resultText) - 1] = '\0';
    QNP_CHECK(std::memcmp(&out, &expected, sizeof(expected)) == 0);
    QNP_CHECK(std::memcmp(
                  reinterpret_cast<const unsigned char*>(&out)
                      + kTrainingStatusV1FighterTier,
                  reinterpret_cast<const unsigned char*>(&expected)
                      + kTrainingStatusV1FighterTier,
                  sizeof(expected) - kTrainingStatusV1FighterTier) == 0);
    QNP_CHECK(out.recoveryContactTestUnknown == 1u);
    QNP_CHECK(out.holdReasons == (TrainingHoldUser | TrainingHoldMenu));
    QNP_CHECK(out.recoveryEligibleMask
              == recoveryBranchBit(RecoveryBranch::Wall66));
}

namespace {

struct NativeDirectionRing {
    uint32_t entry[32] = {};
    uint32_t hold[32] = {};
    uint8_t head = 0;
    uint32_t inserts = 0;
    uint32_t records = 0;

    static uint32_t normalized(uint32_t logicalWorld, uint8_t facing,
                               bool comControlled, uint32_t comMask,
                               bool directionSuppressed) {
        uint32_t value = comControlled
            ? comMask
            : qinput::applyNativeFacing(logicalWorld, facing);
        if (directionSuppressed) value = 0u;
        return value & qinput::kLogicalDirectionMask;
    }

    bool record(uint32_t logicalWorld, uint8_t facing = 0,
                bool comControlled = false, uint32_t comMask = 0,
                bool directionSuppressed = false) {
        ++records;
        const uint32_t value = normalized(logicalWorld, facing, comControlled,
                                          comMask, directionSuppressed);
        if (value == entry[head]) {
            ++hold[head];
            return false;
        }
        head = static_cast<uint8_t>((head + 1u) & 0x1Fu);
        entry[head] = value;
        hold[head] = 0;
        ++inserts;
        return true;
    }

    uint32_t headEntry() const { return entry[head]; }

    bool evaluatorWouldRun(bool pendingLatch = false) const {
        return hold[head] == 0u || pendingLatch;
    }
};

struct NativeMatch {
    bool accepted = false;
    uint32_t spent = 0;
    uint32_t scanned = 0;
};

NativeMatch nativeMatchMode0(const NativeDirectionRing& ring,
                             const uint32_t* pattern, uint32_t window) {
    NativeMatch out{};
    if (window == 0) return out;
    uint32_t spent = 0;
    int scan = 15;
    uint8_t cursor = ring.head;
    uint32_t index = 0;
    for (;;) {
        ++out.scanned;
        const uint32_t held = ring.hold[cursor];
        if (held == 0) ++spent;
        spent += held;
        if (pattern[index] == ring.entry[cursor]) {
            ++index;
            if (pattern[index] == 255u) {
                out.accepted = true;
                out.spent = spent;
                return out;
            }
        }
        cursor = static_cast<uint8_t>((cursor - 1u) & 0x1Fu);
        if (--scan < 0 || spent >= window) break;
    }
    out.spent = spent;
    return out;
}

NativeMatch nativeMatchMode1(const NativeDirectionRing& ring,
                             const uint32_t* pattern, uint32_t window) {
    NativeMatch out{};
    uint32_t spent = 0;
    int scan = 15;
    uint8_t cursor = ring.head;
    uint32_t index = 0;
    for (;;) {
        ++out.scanned;
        spent += ring.hold[cursor];
        if (spent >= window) break;
        if (pattern[index] == ring.entry[cursor]) {
            ++index;
            if (pattern[index] == 255u) {
                out.accepted = true;
                out.spent = spent;
                return out;
            }
        }
        cursor = static_cast<uint8_t>((cursor - 1u) & 0x1Fu);
        if (--scan < 0) break;
    }
    out.spent = spent;
    return out;
}

const uint32_t kNativeAirPattern[] = {0x20000u, 0u, 0x20000u, 255u};
const uint32_t kNativeGroundPattern[] = {0x10000u, 0u, 0x10000u, 255u};
const uint32_t kNativeWallPattern[] = {0x80000u, 0u, 0x80000u, 255u};
const uint32_t kNativeTerminalForwardPattern[] =
    {0x80000u, 0u, 0x80000u, 255u};
const uint32_t kNativeTerminalBackPattern[] =
    {0x40000u, 0u, 0x40000u, 255u};

struct NativeFighterExtras {
    uint8_t jumpByte = 0;
    uint8_t moveId = 0;
    uint32_t jumpLatch = 0;
};

struct NativeRecoveryOutcome {
    bool terminalWrote = false;
    uint32_t terminalAction = 0;
    bool manualWrote = false;
    uint32_t manualAction = 0;
    uint32_t nextAction = 0;
    bool transitionFlag = false;
};

NativeRecoveryOutcome nativeManualRecoveryRequest(
    const RecoveryObservation& observation, const NativeDirectionRing& ring,
    const NativeFighterExtras& extras = NativeFighterExtras{}) {
    NativeRecoveryOutcome out{};
    const uint32_t action = observation.action;
    const uint8_t v20 = observation.permission;
    const uint8_t v21 = extras.jumpByte;
    const uint8_t nibble = static_cast<uint8_t>(observation.postureNibble
                                                & 0x0Fu);
    const uint8_t posture7 = static_cast<uint8_t>(nibble & 0x07u);
    auto write = [&out](uint32_t value) {
        out.nextAction = value;
        out.transitionFlag = true;
    };

    const bool terminalActionOk =
        !(action != 56 && action != 57 && action != 58 && action != 38
          && action != 25 && action != 35 && action != 37 && action < 200);
    if (terminalActionOk && observation.terminalReady
        && posture7 != 0 && posture7 <= 2) {
        const uint32_t head = ring.headEntry();
        if (head == 0x40000u) {
            if (nativeMatchMode1(ring, kNativeTerminalBackPattern, 10u)
                    .accepted) {
                out.terminalWrote = true;
                out.terminalAction = 14;
                write(14u);
            }
        } else if (head == 0x80000u) {
            if (nativeMatchMode1(ring, kNativeTerminalForwardPattern, 10u)
                    .accepted) {
                out.terminalWrote = true;
                out.terminalAction = 13;
                write(13u);
            }
        }
    }

    if (nibble != 1 && nibble != 2 && nibble != 4) return out;

    const bool lockClear = observation.recoveryLock == 0;
    bool fellToLabel59 = false;
    if ((v20 & 8u) != 0 && lockClear) {
        const uint32_t air = action;
        if (air == 56 || air == 57 || air == 58 || air == 38 || air == 25) {
            if (nativeMatchMode0(ring, kNativeAirPattern, 30u).accepted) {
                out.manualWrote = true;
                out.manualAction = 31;
                write(31u);
            }
            return out;
        }
        fellToLabel59 = true;
    } else if ((v20 & 4u) == 0 || !lockClear) {
        fellToLabel59 = true;
    } else {
        if (static_cast<int32_t>(action) >= 200) return out;
        if (action == 56 || action == 57 || action == 58 || action == 38
            || action == 25 || action == 35) {
            if (nativeMatchMode0(ring, kNativeGroundPattern, 20u).accepted) {
                out.manualWrote = true;
                out.manualAction = 33;
                write(33u);
            }
            return out;
        }
        if (action != 37) {
            fellToLabel59 = true;
        } else {
            if (nativeMatchMode0(ring, kNativeWallPattern, 20u).accepted) {
                out.manualWrote = true;
                out.manualAction = 33;
                write(33u);
            }
            return out;
        }
    }
    if (!fellToLabel59) return out;

    if ((v21 & 0xF0u) == 0) return out;
    if ((v20 & 3u) == 0 && (v20 & 8u) == 0) return out;
    if (extras.moveId == 12
        && (action == 160 || action == 163 || action == 166)) {
        return out;
    }
    const uint32_t upBack[] = {0x50000u, 0u, 255u};
    const uint32_t upForward[] = {0x90000u, 0u, 255u};
    const uint32_t up[] = {0x10000u, 0u, 255u};
    uint32_t jumpAction = 0;
    if (nativeMatchMode0(ring, upBack, 20u).accepted) jumpAction = 16;
    else if (nativeMatchMode0(ring, upForward, 20u).accepted) jumpAction = 15;
    else if (nativeMatchMode0(ring, up, 20u).accepted) jumpAction = 17;
    if (jumpAction != 0 && extras.jumpLatch != 1u) {
        out.manualWrote = true;
        out.manualAction = jumpAction;
        write(jumpAction);
    }
    return out;
}

uint32_t nativeConsultedBranchMask(const RecoveryObservation& observation) {
    uint32_t mask = 0;
    const uint32_t action = observation.action;
    const uint8_t nibble = static_cast<uint8_t>(observation.postureNibble
                                                & 0x0Fu);
    const uint8_t posture7 = static_cast<uint8_t>(nibble & 0x07u);

    const bool terminalActionOk =
        !(action != 56 && action != 57 && action != 58 && action != 38
          && action != 25 && action != 35 && action != 37 && action < 200);
    if (terminalActionOk && observation.terminalReady
        && posture7 != 0 && posture7 <= 2) {
        mask |= recoveryBranchBit(RecoveryBranch::TerminalForward66)
            | recoveryBranchBit(RecoveryBranch::TerminalBack44);
    }

    if (nibble != 1 && nibble != 2 && nibble != 4) return mask;

    const bool lockClear = observation.recoveryLock == 0;
    if ((observation.permission & 8u) != 0 && lockClear) {
        if (action == 56 || action == 57 || action == 58 || action == 38
            || action == 25) {
            mask |= recoveryBranchBit(RecoveryBranch::Air22);
        }
        return mask;
    }
    if ((observation.permission & 4u) == 0 || !lockClear) return mask;
    if (static_cast<int32_t>(action) >= 200) return mask;
    if (action == 56 || action == 57 || action == 58 || action == 38
        || action == 25 || action == 35) {
        mask |= recoveryBranchBit(RecoveryBranch::Ground88);
    } else if (action == 37) {
        mask |= recoveryBranchBit(RecoveryBranch::Wall66);
    }
    return mask;
}

enum class RecoveryStartRing { Neutral = 0, GuardHoldingBack, CrouchHoldingDown };

const char* startRingName(RecoveryStartRing ring) {
    switch (ring) {
        case RecoveryStartRing::Neutral: return "neutral";
        case RecoveryStartRing::GuardHoldingBack: return "guard-back";
        case RecoveryStartRing::CrouchHoldingDown: return "crouch-down";
    }
    return "?";
}

void seedStartRing(NativeDirectionRing& ring, RecoveryStartRing which,
                   uint8_t facing) {
    switch (which) {
        case RecoveryStartRing::Neutral:
            for (uint32_t step = 0; step < 10u; ++step) ring.record(0u, facing);
            return;
        case RecoveryStartRing::GuardHoldingBack:
            for (uint32_t step = 0; step < 10u; ++step) {
                ring.record(qinput::applyNativeFacing(qinput::kLogicalLeft,
                                                      facing), facing);
            }
            return;
        case RecoveryStartRing::CrouchHoldingDown:
            for (uint32_t step = 0; step < 10u; ++step) {
                ring.record(qinput::kLogicalDown, facing);
            }
            return;
    }
}

}

QNP_TEST(TrainingRecoveryEligibleSetsAreExactlyTheNativeSwitchArms) {
    std::vector<uint32_t> actions;
    for (uint32_t action = 0; action <= 70u; ++action) actions.push_back(action);
    for (const uint32_t action : {160u, 199u, 200u, 201u, 255u, 512u, 1023u})
        actions.push_back(action);

    uint32_t compared = 0;
    uint32_t wallNarrowed = 0;
    for (const uint32_t action : actions) {
        for (uint32_t permission = 0; permission < 16u; ++permission) {
            for (uint32_t nibble = 0; nibble < 16u; ++nibble) {
                for (const uint32_t lock : {0u, 0x7Fu}) {
                    for (uint32_t ready = 0; ready < 2u; ++ready) {
                        RecoveryObservation observation = recoveryAvailable();
                        observation.action = action;
                        observation.permission =
                            static_cast<uint8_t>(permission);
                        observation.postureNibble =
                            static_cast<uint8_t>(nibble);
                        observation.recoveryLock = lock;
                        observation.terminalReady = ready != 0;
                        observation.wallFlags = 2;

                        const uint32_t armable =
                            recoveryArmableMask(observation);
                        const uint32_t native =
                            nativeConsultedBranchMask(observation);
                        ++compared;
                        if (armable != native) {
                            std::printf(
                                "    [recovery] action=%u perm=%u nibble=%u"
                                " lock=%u ready=%u armable=%02X native=%02X\n",
                                action, permission, nibble, lock, ready,
                                armable, native);
                        }
                        QNP_CHECK(armable == native);

                        observation.wallFlags = 0;
                        const uint32_t unlatched =
                            recoveryArmableMask(observation);
                        if (unlatched != native) {
                            std::printf(
                                "    [recovery] wall-unlatched divergence"
                                " action=%u perm=%u nibble=%u lock=%u"
                                " armable=%02X native=%02X\n",
                                action, permission, nibble, lock,
                                unlatched, native);
                        }
                        QNP_CHECK(unlatched == native);
                        if ((native & recoveryBranchBit(RecoveryBranch::Wall66))
                            != 0u) {
                            ++wallNarrowed;
                        }
                    }
                }
            }
        }
    }
    QNP_CHECK(compared == actions.size() * 16u * 16u * 2u * 2u);
    QNP_CHECK(wallNarrowed > 0u);

    for (uint32_t action = 0; action < 260u; ++action) {
        const bool air = action == 25 || action == 38 || action == 56
            || action == 57 || action == 58;
        const bool ground = air || action == 35;
        QNP_CHECK(recoveryAirEligibleAction(action) == air);
        QNP_CHECK(recoveryGroundEligibleAction(action) == ground);
        QNP_CHECK(recoveryWallEligibleAction(action) == (action == 37));
        QNP_CHECK(recoveryTerminalEligibleAction(action)
                  == (ground || action == 37 || action >= 200));
    }
    QNP_CHECK(!recoveryAirEligibleAction(35));
    QNP_CHECK(recoveryGroundEligibleAction(35));
    QNP_CHECK(!recoveryGroundEligibleAction(37));
    QNP_CHECK(recoveryWallEligibleAction(37));
    for (const uint32_t thrown : {200u, 201u, 255u, 512u, 1023u}) {
        QNP_CHECK(!recoveryAirEligibleAction(thrown));
        QNP_CHECK(!recoveryGroundEligibleAction(thrown));
        QNP_CHECK(!recoveryWallEligibleAction(thrown));
        QNP_CHECK(recoveryTerminalEligibleAction(thrown));
    }
}

QNP_TEST(TrainingRecoveryEmissionIsAcceptedByTheNativeMatcher) {
    struct BranchRow {
        RecoveryBranch branch;
        uint32_t expected;
        const uint32_t* pattern;
        uint32_t window;
    };
    const BranchRow rows[] = {
        {RecoveryBranch::Air22, 31u, kNativeAirPattern, 30u},
        {RecoveryBranch::Ground88, 33u, kNativeGroundPattern, 20u},
        {RecoveryBranch::Wall66, 33u, kNativeWallPattern, 20u},
    };

    for (const BranchRow& row : rows) {
        for (uint8_t facing = 0; facing <= 1u; ++facing) {
            for (const RecoveryStartRing start :
                     {RecoveryStartRing::Neutral,
                      RecoveryStartRing::GuardHoldingBack,
                      RecoveryStartRing::CrouchHoldingDown}) {
                NativeDirectionRing ring;
                seedStartRing(ring, start, facing);
                const uint32_t seededInserts = ring.inserts;

                TrainingRecoveryController controller;
                controller.reset(0);
                const RecoverySettings settings = recoveryArmed(row.branch);
                RecoveryObservation observation =
                    recoveryEligibleFor(row.branch);
                observation.facing = facing;

                bool accepted = false;
                uint32_t acceptedOn = 0;
                uint32_t tapsBefore = 0;
                uint32_t recordedTaps = 0;
                uint64_t serial = 0;
                for (uint32_t step = 0; step < row.window; ++step) {
                    observation.directionRingHead = ring.headEntry();
                    const RecoveryEmission emission =
                        controller.advance(++serial, settings, observation);
                    QNP_REQUIRE(emission.valid);
                    const uint32_t world =
                        emission.active ? emission.directions : 0u;
                    if (world != 0u) ++recordedTaps;
                    ring.record(world, facing);

                    if (row.branch == RecoveryBranch::Wall66 && world != 0u) {
                        QNP_CHECK(world == (facing == 0 ? qinput::kLogicalRight
                                                        : qinput::kLogicalLeft));
                        QNP_CHECK(ring.headEntry() == 0x80000u);
                    }

                    if (!ring.evaluatorWouldRun()) continue;
                    const NativeRecoveryOutcome outcome =
                        nativeManualRecoveryRequest(observation, ring);
                    QNP_CHECK(!outcome.terminalWrote);
                    if (outcome.manualWrote && !accepted) {
                        accepted = true;
                        acceptedOn = step;
                        tapsBefore = recordedTaps;
                        QNP_CHECK(outcome.manualAction == row.expected);
                        QNP_CHECK(outcome.nextAction == row.expected);
                        QNP_CHECK(outcome.transitionFlag);
                    }
                }

                if (!accepted) {
                    std::printf(
                        "    [recovery] %s facing=%u start=%s NEVER accepted"
                        " (inserts %u -> %u)\n",
                        recoveryBranchName(row.branch), facing,
                        startRingName(start), seededInserts, ring.inserts);
                }
                QNP_CHECK(accepted);
                QNP_CHECK(acceptedOn < row.window);
                QNP_CHECK(tapsBefore > 0u);
                QNP_CHECK(recoveryBranchAcceptsResult(row.branch, row.expected));

                const NativeMatch air =
                    nativeMatchMode0(ring, kNativeAirPattern, 30u);
                const NativeMatch ground =
                    nativeMatchMode0(ring, kNativeGroundPattern, 20u);
                const NativeMatch wall =
                    nativeMatchMode0(ring, kNativeWallPattern, 20u);
                QNP_CHECK(air.accepted
                          == (row.branch == RecoveryBranch::Air22));
                QNP_CHECK(ground.accepted
                          == (row.branch == RecoveryBranch::Ground88));
                QNP_CHECK(wall.accepted
                          == (row.branch == RecoveryBranch::Wall66));

                if (row.branch == RecoveryBranch::Air22
                    && start == RecoveryStartRing::CrouchHoldingDown) {
                    QNP_CHECK(tapsBefore == 1u);
                    QNP_CHECK(acceptedOn == 1u);
                } else {
                    QNP_CHECK(tapsBefore == 2u);
                }
            }
        }
    }

    for (uint8_t facing = 0; facing <= 1u; ++facing) {
        struct TerminalRow {
            RecoveryBranch branch;
            uint32_t expected;
            uint32_t head;
        };
        const TerminalRow terminals[] = {
            {RecoveryBranch::TerminalForward66, 13u, kRecoveryRingForward},
            {RecoveryBranch::TerminalBack44, 14u, kRecoveryRingBack},
        };
        for (const TerminalRow& row : terminals) {
            NativeDirectionRing ring;
            seedStartRing(ring, RecoveryStartRing::Neutral, facing);
            TrainingRecoveryController controller;
            controller.reset(0);
            const RecoverySettings settings = recoveryArmed(row.branch);
            RecoveryObservation observation = recoveryEligibleFor(row.branch);
            observation.facing = facing;

            bool accepted = false;
            uint64_t serial = 0;
            for (uint32_t step = 0; step < kRecoveryWindowTerminal; ++step) {
                observation.directionRingHead = ring.headEntry();
                const RecoveryEmission emission =
                    controller.advance(++serial, settings, observation);
                QNP_REQUIRE(emission.valid);
                ring.record(emission.active ? emission.directions : 0u, facing);
                if (!ring.evaluatorWouldRun()) continue;
                const NativeRecoveryOutcome outcome =
                    nativeManualRecoveryRequest(observation, ring);
                if (outcome.terminalWrote && !accepted) {
                    accepted = true;
                    QNP_CHECK(outcome.terminalAction == row.expected);
                    QNP_CHECK(ring.headEntry() == row.head);
                }
            }
            if (!accepted) {
                std::printf("    [recovery] %s facing=%u terminal NOT accepted"
                            " (head %08X)\n",
                            recoveryBranchName(row.branch), facing,
                            ring.headEntry());
            }
            QNP_CHECK(accepted);
            QNP_CHECK(recoveryBranchAcceptsResult(row.branch, row.expected));
        }
    }

}

QNP_TEST(TrainingRecoveryHeldDirectionCanNeverMatch) {
    for (const uint32_t held : {qinput::kLogicalDown, qinput::kLogicalUp,
                                qinput::kLogicalLeft, qinput::kLogicalRight}) {
        NativeDirectionRing ring;
        for (uint32_t step = 0; step < 60u; ++step) {
            const bool inserted = ring.record(held, 0);
            QNP_CHECK(inserted == (step == 0));
            QNP_CHECK(ring.evaluatorWouldRun() == (step == 0));
            QNP_CHECK(!nativeMatchMode0(ring, kNativeAirPattern, 30u).accepted);
            QNP_CHECK(!nativeMatchMode0(ring, kNativeGroundPattern, 20u)
                           .accepted);
            QNP_CHECK(!nativeMatchMode0(ring, kNativeWallPattern, 20u)
                           .accepted);
        }
        QNP_CHECK(ring.records == 60u);
        QNP_CHECK(ring.inserts == 1u);
        QNP_CHECK(ring.hold[ring.head] == 59u);
    }

    const RecoveryBranch liveBranches[] = {
        RecoveryBranch::Air22,
        RecoveryBranch::Ground88,
        RecoveryBranch::Wall66,
        RecoveryBranch::TerminalForward66,
        RecoveryBranch::TerminalBack44,
    };
    for (const RecoveryBranch branch : liveBranches) {
        for (uint8_t facing = 0; facing <= 1u; ++facing) {
            for (const RecoveryStartRing start :
                     {RecoveryStartRing::Neutral,
                      RecoveryStartRing::GuardHoldingBack,
                      RecoveryStartRing::CrouchHoldingDown}) {
                NativeDirectionRing ring;
                seedStartRing(ring, start, facing);
                const uint32_t before = ring.inserts;

                TrainingRecoveryController controller;
                controller.reset(0);
                const RecoverySettings settings = recoveryArmed(branch);
                RecoveryObservation observation = recoveryEligibleFor(branch);
                observation.facing = facing;

                uint32_t previous = ring.headEntry();
                uint32_t emitted = 0;
                uint32_t taps = 0;
                uint64_t serial = 0;
                const uint32_t stepCount = recoveryEmissionStepCount(branch);
                for (uint32_t step = 0; step < stepCount + 1u; ++step) {
                    observation.directionRingHead = ring.headEntry();
                    const RecoveryEmission emission =
                        controller.advance(++serial, settings, observation);
                    QNP_REQUIRE(emission.valid);
                    const bool emitting =
                        emission.state == RecoveryState::EmitPreNeutral
                        || emission.state == RecoveryState::EmitTap1
                        || emission.state == RecoveryState::EmitNeutral
                        || emission.state == RecoveryState::EmitTap2;
                    if (!emitting) continue;
                    ++emitted;
                    const uint32_t world = emission.directions;
                    if (world != 0u && world == previous) {
                        std::printf(
                            "    [recovery] %s facing=%u start=%s HELD"
                            " %08X across two emitted steps\n",
                            recoveryBranchName(branch), facing,
                            startRingName(start), world);
                    }
                    QNP_CHECK(world == 0u || world != previous);
                    previous = world;
                    if (world != 0u) ++taps;
                    ring.record(world, facing);
                }

                QNP_CHECK(taps == 2u);
                QNP_CHECK(ring.inserts - before >= 3u);
                QNP_CHECK(ring.inserts - before <= 4u);
                QNP_CHECK(ring.evaluatorWouldRun());
            }
        }
    }

    for (uint8_t facing = 0; facing <= 1u; ++facing) {
        for (const RecoveryBranch branch :
                 {RecoveryBranch::Air22, RecoveryBranch::Ground88,
                  RecoveryBranch::Wall66}) {
            QNP_CHECK(recoveryAwaitMask(branch, facing).directions == 0u);
            QNP_CHECK(recoveryAwaitMask(branch, facing).buttons == 0u);
        }
        QNP_CHECK(recoveryAwaitMask(RecoveryBranch::TerminalForward66, facing)
                      .directions != 0u);
        QNP_CHECK(recoveryAwaitMask(RecoveryBranch::TerminalBack44, facing)
                      .directions != 0u);
    }
}

QNP_TEST(TrainingRecoveryWindowBudgetsMatchTheMatcher) {
    struct BudgetRow {
        RecoveryBranch branch;
        uint32_t window;
        const uint32_t* pattern;
    };
    const BudgetRow rows[] = {
        {RecoveryBranch::Air22, kRecoveryWindowAir, kNativeAirPattern},
        {RecoveryBranch::Ground88, kRecoveryWindowGround, kNativeGroundPattern},
        {RecoveryBranch::Wall66, kRecoveryWindowGround, kNativeWallPattern},
    };

    for (const BudgetRow& row : rows) {
        QNP_CHECK(recoveryBranchWindow(row.branch) == row.window);

        NativeDirectionRing ring;
        seedStartRing(ring, RecoveryStartRing::Neutral, 0);
        TrainingRecoveryController controller;
        controller.reset(0);
        const RecoverySettings settings = recoveryArmed(row.branch);
        RecoveryObservation observation = recoveryEligibleFor(row.branch);

        uint64_t serial = 0;
        uint64_t lastTapSerial = 0;
        uint64_t lastAcceptSerial = 0;
        uint64_t expirySerial = 0;
        uint64_t lastLiveSerial = 0;
        for (uint32_t step = 0; step < row.window + 8u; ++step) {
            observation.directionRingHead = ring.headEntry();
            const RecoveryEmission emission =
                controller.advance(++serial, settings, observation);
            QNP_REQUIRE(emission.valid);
            const uint32_t world = emission.active ? emission.directions : 0u;
            if (world != 0u) lastTapSerial = serial;
            ring.record(world, 0);

            if (nativeMatchMode0(ring, row.pattern, row.window).accepted)
                lastAcceptSerial = serial;
            if (emission.lastVerdict == RecoveryVerdict::MissedExpired
                && emission.verdictIsNew && expirySerial == 0) {
                expirySerial = serial;
            }
            if (expirySerial == 0
                && (emission.state == RecoveryState::AwaitNativeTransition
                    || emission.state == RecoveryState::ObservingBuffer)) {
                lastLiveSerial = serial;
            }
        }

        QNP_REQUIRE(lastTapSerial != 0);
        QNP_REQUIRE(lastAcceptSerial != 0);
        QNP_REQUIRE(expirySerial != 0);
        const uint64_t acceptSpan = lastAcceptSerial - lastTapSerial;
        const uint64_t expirySpan = expirySerial - lastTapSerial;
        std::printf(
            "    [recovery] %s window=%u last accept +%llu, controller expires"
            " +%llu, last claimed live +%llu\n",
            recoveryBranchName(row.branch), row.window,
            static_cast<unsigned long long>(acceptSpan),
            static_cast<unsigned long long>(expirySpan),
            static_cast<unsigned long long>(lastLiveSerial - lastTapSerial));

        QNP_CHECK(expirySpan == row.window);
        QNP_CHECK(acceptSpan == row.window - 2u);
        QNP_CHECK(lastLiveSerial == lastAcceptSerial + 1u);
    }

    {
        NativeDirectionRing ring;
        seedStartRing(ring, RecoveryStartRing::Neutral, 0);
        ring.record(qinput::kLogicalUp, 0);
        ring.record(0u, 0);
        ring.record(qinput::kLogicalUp, 0);
        RecoveryObservation observation =
            recoveryEligibleFor(RecoveryBranch::Ground88);
        NativeFighterExtras extras;
        QNP_CHECK(!nativeManualRecoveryRequest(observation, ring, extras)
                       .manualWrote
                  || nativeManualRecoveryRequest(observation, ring, extras)
                         .manualAction == 33u);
        extras.jumpByte = 0x10;
        const uint32_t up[] = {0x10000u, 0u, 255u};
        QNP_CHECK(nativeMatchMode0(ring, up, kRecoveryWindowGround).accepted);
        uint32_t retiredAfter = 0;
        for (uint32_t step = 1; step <= 64u; ++step) {
            ring.record(0u, 0);
            if (!nativeMatchMode0(ring, up, kRecoveryWindowGround).accepted) {
                retiredAfter = step;
                break;
            }
        }
        std::printf("    [recovery] 88 spill retired after %u neutral steps\n",
                    retiredAfter);
        QNP_CHECK(retiredAfter > 0u);
        QNP_CHECK(retiredAfter <= kRecoveryWindowGround);
    }

    QNP_CHECK(recoveryBranchWindow(RecoveryBranch::TerminalBack44)
              == kRecoveryWindowTerminal);
    QNP_CHECK(recoveryBranchWindow(RecoveryBranch::ThrowEscape) == 0u);
}

QNP_TEST(TrainingGuardTimingAfterBeingHitArmsForTheWindow) {
    using namespace qtraining;
    TrainingGuardTimingConfig config{};
    config.mode = TrainingGuardTimingMode::AfterBeingHit;
    config.windowSteps = 3u;
    TrainingGuardTimingState state{};

    TrainingGuardTimingStep free{};
    free.dummyNeutral = true;
    TrainingGuardTimingStep stunned{};

    QNP_CHECK(!guardTimingAdvance(config, state, free));
    TrainingGuardTimingStep blocked{};
    blocked.dummyBlocked = true;
    QNP_CHECK(!guardTimingAdvance(config, state, blocked));

    TrainingGuardTimingStep hit{};
    hit.dummyWasHit = true;
    QNP_CHECK(guardTimingAdvance(config, state, hit));
    QNP_CHECK(guardTimingAdvance(config, state, free));
    QNP_CHECK(guardTimingAdvance(config, state, free));
    QNP_CHECK(guardTimingAdvance(config, state, free));
    QNP_CHECK(!guardTimingAdvance(config, state, free));

    QNP_CHECK(guardTimingAdvance(config, state, hit));
    QNP_CHECK(guardTimingAdvance(config, state, stunned));
    QNP_CHECK(guardTimingAdvance(config, state, stunned));
    QNP_CHECK(guardTimingAdvance(config, state, stunned));
    QNP_CHECK(guardTimingAdvance(config, state, stunned));
    QNP_CHECK(guardTimingAdvance(config, state, free));
    QNP_CHECK(guardTimingAdvance(config, state, free));
    QNP_CHECK(guardTimingAdvance(config, state, free));
    QNP_CHECK(!guardTimingAdvance(config, state, free));

    QNP_CHECK(guardTimingAdvance(config, state, hit));
    QNP_CHECK(guardTimingAdvance(config, state, blocked));
    QNP_CHECK(guardTimingAdvance(config, state, free));
    QNP_CHECK(guardTimingAdvance(config, state, free));
    QNP_CHECK(guardTimingAdvance(config, state, free));
    QNP_CHECK(!guardTimingAdvance(config, state, free));
}

QNP_TEST(TrainingGuardTimingUntilFirstBlockSpendsAndRearms) {
    using namespace qtraining;
    TrainingGuardTimingConfig config{};
    config.mode = TrainingGuardTimingMode::UntilFirstBlock;
    config.windowSteps = 2u;
    TrainingGuardTimingState state{};

    TrainingGuardTimingStep quiet{};
    TrainingGuardTimingStep blocked{};
    blocked.dummyBlocked = true;
    TrainingGuardTimingStep neutral{};
    neutral.dummyNeutral = true;

    QNP_CHECK(guardTimingAdvance(config, state, quiet));
    QNP_CHECK(guardTimingAdvance(config, state, blocked));
    QNP_CHECK(!guardTimingAdvance(config, state, quiet));
    QNP_CHECK(!guardTimingAdvance(config, state, neutral));
    QNP_CHECK(!guardTimingAdvance(config, state, quiet));
    QNP_CHECK(!guardTimingAdvance(config, state, neutral));
    QNP_CHECK(guardTimingAdvance(config, state, neutral));
    QNP_CHECK(guardTimingAdvance(config, state, quiet));
}

QNP_TEST(TrainingGuardTimingAlwaysIsTransparent) {
    using namespace qtraining;
    TrainingGuardTimingConfig config{};
    TrainingGuardTimingState state{};
    state.blockSpent = true;
    state.armedCountdown = 5u;
    TrainingGuardTimingStep step{};
    step.dummyWasHit = true;
    step.dummyBlocked = true;
    QNP_CHECK(guardTimingAdvance(config, state, step));
    QNP_CHECK(!state.blockSpent);
    QNP_CHECK(state.armedCountdown == 0u);
    QNP_CHECK(guardTimingModeValid(0u) && guardTimingModeValid(2u)
              && !guardTimingModeValid(3u));
    QNP_CHECK(guardTimingWindowValid(1u) && guardTimingWindowValid(600u)
              && !guardTimingWindowValid(0u)
              && !guardTimingWindowValid(601u));
}

QNP_TEST(TrainingStatusMessagesClassifyEventsAndModes) {
    QNP_CHECK(statusEventValid(
        static_cast<uint32_t>(StatusEvent::RoundReset)));
    QNP_CHECK(!statusEventValid(
        static_cast<uint32_t>(StatusEvent::Count)));
    QNP_CHECK(statusModeValid(static_cast<uint32_t>(StatusMode::Playing)));
    QNP_CHECK(!statusModeValid(static_cast<uint32_t>(StatusMode::Count)));

    QNP_CHECK(statusEventTone(StatusEvent::StateSaved) == StatusTone::Good);
    QNP_CHECK(statusEventTone(StatusEvent::MacroRecorded)
              == StatusTone::Good);
    QNP_CHECK(statusEventTone(StatusEvent::StateLoaded)
              == StatusTone::Neutral);

    QNP_CHECK(statusModeTone(StatusMode::PreRecord) == StatusTone::Warning);
    QNP_CHECK(statusModeTone(StatusMode::Recording) == StatusTone::Active);
    QNP_CHECK(statusModeTone(StatusMode::Playing) == StatusTone::Active);
    QNP_CHECK(statusModeTone(StatusMode::None) == StatusTone::Neutral);

    QNP_CHECK(statusEventHasSlot(StatusEvent::StateSaved));
    QNP_CHECK(statusEventHasSlot(StatusEvent::MacroSlotSelected));
    QNP_CHECK(!statusEventHasSlot(StatusEvent::PositionReset));
    QNP_CHECK(!statusEventHasSlot(StatusEvent::MacroRecorded));
}
