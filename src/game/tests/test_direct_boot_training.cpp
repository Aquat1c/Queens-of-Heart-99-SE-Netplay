#include "../../netplay/tests/TestHarness.h"

#include "../DirectBoot.h"
#include "../../ipc/IpcConfig.h"

namespace {

qgame::directboot::TrainingSelectionSample row1Sample() {
    qgame::directboot::TrainingSelectionSample sample{};
    sample.scene = 3;
    sample.sceneSubstate = 2;
    sample.sceneNext = 3;
    sample.matchMode = 1;
    sample.controlMask = 0;
    sample.p1State = 1;
    sample.p2State = 1;
    sample.p1InputSource = 0;
    sample.p2InputSource = 1;
    sample.p2Route = 1;
    sample.stageSelectionActive = false;
    return sample;
}

qgame::directboot::TrainingSameCallConfirmSample sameCallConfirmSample() {
    qgame::directboot::TrainingSameCallConfirmSample sample{};
    sample.explicitTraining = true;
    sample.scene = 3;
    sample.sceneSubstate = 2;
    sample.selectorPlayer = 0;
    sample.matchMode = 1;
    sample.controlMask = 0;
    sample.p1StateBefore = 2;
    sample.p1StateAfter = 4;
    sample.p2StateBefore = 0;
    sample.p2StateAfter = 1;
    sample.p1InputSource = 0;
    sample.p2InputSource = 0;
    sample.p2Route = 1;
    return sample;
}

qgame::directboot::TrainingFrontendInputGateSample frontendGateSample() {
    qgame::directboot::TrainingFrontendInputGateSample sample{};
    sample.explicitTraining = true;
    sample.managedRuntimeArmed = true;
    sample.menuCaptured = false;
    sample.physicalNeutralBeforeSocd = true;
    sample.dispatchSerial = 1;
    sample.captureGeneration = 0;
    return sample;
}

qgame::directboot::TrainingStageInputGateSample stageGateSample() {
    qgame::directboot::TrainingStageInputGateSample sample{};
    sample.explicitTraining = true;
    sample.managedRuntimeArmed = true;
    sample.inCharacterSelect = true;
    sample.stageSelectionActive = true;
    sample.menuCaptured = false;
    sample.actionButtonsNeutral = false;
    sample.dispatchSerial = 1;
    return sample;
}

QNP_TEST(TrainingReturnedStageResetIsLimitedToCompletedTrainingReturn) {
    using namespace qgame::directboot;

    TrainingReturnedStageResetDecision decision =
        decideTrainingReturnedStageReset(true, true, 3u, 2u);
    QNP_REQUIRE(decision.apply);
    QNP_CHECK(decision.selectionKind == 2u);
    QNP_CHECK(decision.selectionValue == 0u);

    QNP_CHECK(!decideTrainingReturnedStageReset(true, false, 3u, 2u).apply);
    QNP_CHECK(!decideTrainingReturnedStageReset(false, true, 3u, 2u).apply);
    QNP_CHECK(!decideTrainingReturnedStageReset(false, false, 3u, 2u).apply);

    QNP_CHECK(!decideTrainingReturnedStageReset(true, true, 2u, 0u).apply);
    QNP_CHECK(!decideTrainingReturnedStageReset(true, true, 3u, 1u).apply);
}

}

QNP_TEST(TrainingIpcRoleIsExplicitAndNarrow) {
    qipc::IpcConfig config{};
    config.mode = qipc::MODE_OBSERVE;
    config.isHost = 1;
    config.localPlayer = 1;
    config.directBoot = 1;
    config.vsRow = qipc::kVsRow1v1;

    QNP_CHECK(qipc::validateTrainingLaunch(config) == nullptr);

    config.trainingEnabled = 1;
    QNP_CHECK(qipc::validateTrainingLaunch(config) == nullptr);

    config.mode = qipc::MODE_NETPLAY;
    QNP_CHECK(qipc::validateTrainingLaunch(config) != nullptr);
    config.mode = qipc::MODE_OBSERVE;
    config.vsRow = qipc::kVsRow1vCpu;
    QNP_CHECK(qipc::validateTrainingLaunch(config) != nullptr);
    config.vsRow = qipc::kVsRow1v1;
    config.offlineExtraPresent[0] = 1;
    QNP_CHECK(qipc::validateTrainingLaunch(config) != nullptr);
    config.offlineExtraPresent[0] = 0;
    config.trainingEnabled = 2;
    QNP_CHECK(qipc::validateTrainingLaunch(config) != nullptr);

    QNP_CHECK(qipc::kIpcVersion == 25);
    QNP_CHECK(sizeof(qipc::IpcConfig) == 14965);
}

QNP_TEST(TrainingLiveE2EEnvironmentGateRequiresExactOne) {
    using qgame::directboot::trainingLiveE2EEnvironmentValueExact;
    QNP_CHECK(trainingLiveE2EEnvironmentValueExact("1", 1));
    QNP_CHECK(!trainingLiveE2EEnvironmentValueExact(nullptr, 1));
    QNP_CHECK(!trainingLiveE2EEnvironmentValueExact("", 0));
    QNP_CHECK(!trainingLiveE2EEnvironmentValueExact("0", 1));
    QNP_CHECK(!trainingLiveE2EEnvironmentValueExact("1 ", 2));
    QNP_CHECK(!trainingLiveE2EEnvironmentValueExact("true", 4));
}

QNP_TEST(TrainingFrontendGateRequiresTwoUniquePhysicalNeutralDispatches) {
    using namespace qgame::directboot;
    TrainingFrontendInputGatePolicy policy;
    TrainingFrontendInputGateSample sample = frontendGateSample();

    TrainingFrontendInputGateDecision decision = policy.advance(sample);
    QNP_CHECK(!decision.allowInput);
    QNP_CHECK(decision.state
              == TrainingFrontendInputGateState::Rearming);
    QNP_CHECK(decision.neutralDispatches == 1);

    decision = policy.advance(sample);
    QNP_CHECK(!decision.allowInput);
    QNP_CHECK(decision.neutralDispatches == 1);

    ++sample.dispatchSerial;
    decision = policy.advance(sample);
    QNP_CHECK(decision.allowInput);
    QNP_CHECK(decision.state
              == TrainingFrontendInputGateState::Armed);
    QNP_CHECK(decision.neutralDispatches == 2);
}

QNP_TEST(TrainingFrontendGateBlocksUnreadyAndPhysicallyRearmsAfterCapture) {
    using namespace qgame::directboot;
    TrainingFrontendInputGatePolicy policy;
    TrainingFrontendInputGateSample sample = frontendGateSample();
    sample.managedRuntimeArmed = false;
    QNP_CHECK(!policy.advance(sample).allowInput);

    sample.managedRuntimeArmed = true;
    ++sample.dispatchSerial;
    QNP_CHECK(!policy.advance(sample).allowInput);
    ++sample.dispatchSerial;
    QNP_REQUIRE(policy.advance(sample).allowInput);

    sample.menuCaptured = true;
    ++sample.captureGeneration;
    ++sample.dispatchSerial;
    QNP_CHECK(!policy.advance(sample).allowInput);

    sample.menuCaptured = false;
    sample.physicalNeutralBeforeSocd = false;
    ++sample.captureGeneration;
    ++sample.dispatchSerial;
    TrainingFrontendInputGateDecision decision = policy.advance(sample);
    QNP_CHECK(!decision.allowInput);
    QNP_CHECK(decision.neutralDispatches == 0);
    sample.physicalNeutralBeforeSocd = true;
    ++sample.dispatchSerial;
    QNP_CHECK(!policy.advance(sample).allowInput);
    ++sample.dispatchSerial;
    QNP_CHECK(policy.advance(sample).allowInput);
}

QNP_TEST(TrainingStageGateRejectsCarriedConfirmUntilTwoNeutralDispatches) {
    using namespace qgame::directboot;
    TrainingStageInputGatePolicy policy;
    TrainingStageInputGateSample sample = stageGateSample();

    TrainingStageInputGateDecision decision = policy.advance(sample);
    QNP_CHECK(!decision.allowInput);
    QNP_CHECK(decision.state == TrainingStageInputGateState::Rearming);
    QNP_CHECK(decision.neutralDispatches == 0u);

    QNP_CHECK(!policy.advance(sample).allowInput);
    sample.actionButtonsNeutral = true;
    ++sample.dispatchSerial;
    decision = policy.advance(sample);
    QNP_CHECK(!decision.allowInput);
    QNP_CHECK(decision.neutralDispatches == 1u);
    ++sample.dispatchSerial;
    decision = policy.advance(sample);
    QNP_CHECK(decision.allowInput);
    QNP_CHECK(decision.state == TrainingStageInputGateState::Armed);
}

QNP_TEST(TrainingStageGateIsInertOutsideTrainingAndRearmsOnReentry) {
    using namespace qgame::directboot;
    TrainingStageInputGatePolicy policy;
    TrainingStageInputGateSample sample = stageGateSample();
    sample.actionButtonsNeutral = true;
    QNP_CHECK(!policy.advance(sample).allowInput);
    ++sample.dispatchSerial;
    QNP_REQUIRE(policy.advance(sample).allowInput);

    sample.stageSelectionActive = false;
    ++sample.dispatchSerial;
    TrainingStageInputGateDecision decision = policy.advance(sample);
    QNP_CHECK(!decision.allowInput);
    QNP_CHECK(decision.state == TrainingStageInputGateState::Inactive);

    sample.stageSelectionActive = true;
    ++sample.dispatchSerial;
    decision = policy.advance(sample);
    QNP_CHECK(!decision.allowInput);
    QNP_CHECK(decision.neutralDispatches == 1u);

    sample.explicitTraining = false;
    ++sample.dispatchSerial;
    decision = policy.advance(sample);
    QNP_CHECK(!decision.allowInput);
    QNP_CHECK(decision.state == TrainingStageInputGateState::Inactive);
}

QNP_TEST(TrainingStageGateCaptureRequiresFreshNeutralRelease) {
    using namespace qgame::directboot;
    TrainingStageInputGatePolicy policy;
    TrainingStageInputGateSample sample = stageGateSample();
    sample.actionButtonsNeutral = true;
    QNP_CHECK(!policy.advance(sample).allowInput);
    ++sample.dispatchSerial;
    QNP_REQUIRE(policy.advance(sample).allowInput);

    sample.menuCaptured = true;
    ++sample.dispatchSerial;
    QNP_CHECK(!policy.advance(sample).allowInput);
    sample.menuCaptured = false;
    sample.actionButtonsNeutral = false;
    ++sample.dispatchSerial;
    QNP_CHECK(!policy.advance(sample).allowInput);
    sample.actionButtonsNeutral = true;
    ++sample.dispatchSerial;
    QNP_CHECK(!policy.advance(sample).allowInput);
    ++sample.dispatchSerial;
    QNP_CHECK(policy.advance(sample).allowInput);
}

QNP_TEST(TrainingSameCallConfirmHoldsOnlyQualifiedP1Handoff) {
    using namespace qgame::directboot;
    TrainingSameCallConfirmSample sample = sameCallConfirmSample();
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::HoldP2Inactive);

    sample.p2StateBefore = 1;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::HoldP2Inactive);
}

QNP_TEST(TrainingSameCallConfirmIsInertOutsideExactTransition) {
    using namespace qgame::directboot;
    TrainingSameCallConfirmSample sample = sameCallConfirmSample();

    sample.explicitTraining = false;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::None);
    sample.explicitTraining = true;
    sample.selectorPlayer = 1;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::None);
    sample.selectorPlayer = 0;
    sample.p1StateAfter = 1;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::None);
    sample.p1StateAfter = 4;
    sample.sceneSubstate = 1;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::None);
}

QNP_TEST(TrainingSameCallConfirmFailsClosedOnNativeShapeDrift) {
    using namespace qgame::directboot;
    TrainingSameCallConfirmSample sample = sameCallConfirmSample();

    sample.p2StateAfter = 2;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::FailClosed);
    sample = sameCallConfirmSample();
    sample.p2InputSource = 1;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::FailClosed);
    sample = sameCallConfirmSample();
    sample.controlMask = 2;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::FailClosed);
    sample = sameCallConfirmSample();
    sample.matchMode = 2;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::FailClosed);
    sample = sameCallConfirmSample();
    sample.p2StateBefore = 2;
    QNP_CHECK(decideTrainingSameCallConfirm(sample)
              == TrainingSameCallConfirmAction::FailClosed);
}

QNP_TEST(TrainingSelectorInitializesNativeRow1Overlay) {
    using namespace qgame::directboot;
    TrainingSelectionPolicy policy;
    TrainingSelectionSample sample = row1Sample();

    const TrainingSelectionDecision decision = policy.advance(sample);
    QNP_CHECK(decision.failure == TrainingSelectionFailure::None);
    QNP_CHECK(decision.writeP2State && decision.p2State == 0);
    QNP_CHECK(decision.writeP2InputSource
              && decision.p2InputSource == 0);
    QNP_CHECK(decision.writeP2Route && decision.p2Route == 1);
    QNP_CHECK(decision.resetP2SelectorOwner);
    QNP_CHECK(policy.phase() == TrainingSelectionPhase::SelectingP1);
}

QNP_TEST(TrainingSelectorHeldConfirmCannotDoublePick) {
    using namespace qgame::directboot;
    TrainingSelectionPolicy policy;
    TrainingSelectionSample sample = row1Sample();
    (void)policy.advance(sample);

    sample.p1State = 4;
    sample.p2State = 1;
    sample.p2InputSource = 0;
    sample.p1LogicalMask = 1;
    TrainingSelectionDecision decision = policy.advance(sample);
    QNP_CHECK(policy.phase()
              == TrainingSelectionPhase::ReleasingP1Buttons);
    QNP_CHECK(decision.writeP2State && decision.p2State == 0);

    for (int i = 0; i < 4; ++i) {
        decision = policy.advance(sample);
        QNP_CHECK(decision.writeP2State && decision.p2State == 0);
        QNP_CHECK(policy.phase()
                  == TrainingSelectionPhase::ReleasingP1Buttons);
    }

    sample.p1LogicalMask = 0;
    decision = policy.advance(sample);
    QNP_CHECK(decision.writeP2State && decision.p2State == 1);
    QNP_CHECK(policy.phase() == TrainingSelectionPhase::SelectingP2);
}

QNP_TEST(TrainingSelectorRequiresTwoNeutralSelectionSteps) {
    using namespace qgame::directboot;
    TrainingSelectionPolicy policy;
    TrainingSelectionSample sample = row1Sample();
    (void)policy.advance(sample);
    sample.p1State = 4;
    sample.p2InputSource = 0;
    sample.p1LogicalMask = 0;
    (void)policy.advance(sample);

    TrainingSelectionDecision decision = policy.advance(sample);
    QNP_CHECK(decision.writeP2State && decision.p2State == 0);
    QNP_CHECK(policy.phase()
              == TrainingSelectionPhase::ReleasingP1Buttons);
    decision = policy.advance(sample);
    QNP_CHECK(decision.writeP2State && decision.p2State == 1);
    QNP_CHECK(policy.phase() == TrainingSelectionPhase::SelectingP2);
}

QNP_TEST(TrainingSelectorIsolatesP2SourceOnlyDuringStageSelection) {
    using namespace qgame::directboot;
    TrainingSelectionPolicy policy;
    TrainingSelectionSample sample = row1Sample();
    (void)policy.advance(sample);
    sample.p1State = 4;
    sample.p2InputSource = 0;
    (void)policy.advance(sample);
    (void)policy.advance(sample);
    (void)policy.advance(sample);
    QNP_REQUIRE(policy.phase() == TrainingSelectionPhase::SelectingP2);

    sample.p2State = 4;
    TrainingSelectionDecision decision = policy.advance(sample);
    QNP_CHECK(policy.phase()
              == TrainingSelectionPhase::P2ConfirmedSettling);
    QNP_CHECK(decision.writeP2InputSource
              && decision.p2InputSource == 0);

    for (int i = 0; i < 30; ++i) {
        decision = policy.advance(sample);
        QNP_CHECK(decision.failure == TrainingSelectionFailure::None);
        QNP_CHECK(decision.writeP2InputSource
                  && decision.p2InputSource == 0);
    }

    sample.stageSelectionActive = true;
    decision = policy.advance(sample);
    QNP_CHECK(decision.failure == TrainingSelectionFailure::None);
    QNP_CHECK(decision.writeP2InputSource
              && decision.p2InputSource == 1);

    sample.stageSelectionActive = false;
    sample.p2InputSource = 1;
    decision = policy.advance(sample);
    QNP_CHECK(decision.writeP2InputSource
              && decision.p2InputSource == 0);

    sample.stageSelectionActive = true;
    decision = policy.advance(sample);
    QNP_CHECK(decision.writeP2InputSource
              && decision.p2InputSource == 1);
    sample.sceneNext = 2;
    sample.p2InputSource = 1;
    decision = policy.advance(sample);
    QNP_CHECK(decision.completed);
    QNP_CHECK(!decision.writeP2InputSource);
    QNP_CHECK(policy.phase() == TrainingSelectionPhase::Complete);
}

QNP_TEST(TrainingSelectorCancelBackToP1RearmsHandoff) {
    using namespace qgame::directboot;
    TrainingSelectionPolicy policy;
    TrainingSelectionSample sample = row1Sample();
    (void)policy.advance(sample);
    sample.p1State = 4;
    sample.p2InputSource = 0;
    (void)policy.advance(sample);
    (void)policy.advance(sample);
    (void)policy.advance(sample);
    QNP_REQUIRE(policy.phase() == TrainingSelectionPhase::SelectingP2);

    sample.p1State = 3;
    sample.p2State = 0;
    TrainingSelectionDecision decision = policy.advance(sample);
    QNP_CHECK(decision.writeP2State && decision.p2State == 0);
    QNP_CHECK(policy.phase() == TrainingSelectionPhase::SelectingP1);

    sample.p1State = 4;
    sample.p2State = 1;
    sample.p1LogicalMask = 1;
    decision = policy.advance(sample);
    QNP_CHECK(policy.phase()
              == TrainingSelectionPhase::ReleasingP1Buttons);
    QNP_CHECK(decision.writeP2State && decision.p2State == 0);
}

QNP_TEST(TrainingSelectorFailsClosedWithoutNativeNormalization) {
    using namespace qgame::directboot;
    TrainingSelectionPolicy policy;
    TrainingSelectionSample sample = row1Sample();
    (void)policy.advance(sample);
    sample.p1State = 4;
    sample.p2InputSource = 0;
    (void)policy.advance(sample);
    (void)policy.advance(sample);
    (void)policy.advance(sample);
    sample.p2State = 4;
    (void)policy.advance(sample);

    sample.sceneNext = 2;
    sample.p2InputSource = 0;
    const TrainingSelectionDecision decision = policy.advance(sample);
    QNP_CHECK(decision.failure
              == TrainingSelectionFailure::NativeP2NormalizationMissing);
    QNP_CHECK(policy.phase() == TrainingSelectionPhase::Failed);
}
