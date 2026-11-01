#include "TestHarness.h"

#include "game/NetplayRunState.h"

using namespace qgame;

namespace {

RunStateInputs advancing() {
    RunStateInputs in;
    in.kind = WorkKind::Advance;
    in.hold = HoldCause::None;
    in.missingRemoteDepth = 0;
    in.localMaxRollback = 12;
    in.warningMargin = 2;
    return in;
}

RunStateInputs holding(const HoldCause cause) {
    RunStateInputs in = advancing();
    in.kind = WorkKind::Hold;
    in.hold = cause;
    return in;
}

}

QNP_TEST(HealthyAdvanceIsRunning) {
    QNP_CHECK(classifyRunState(advancing()) == NetplayRunState::Running);
}

QNP_TEST(PredictionLimitHoldIsTheNetworkEmergency) {
    const NetplayRunState s = classifyRunState(holding(HoldCause::PredictionLimit));
    QNP_CHECK(s == NetplayRunState::EmergencyNetworkHold);
    QNP_CHECK(runStateStopsCanonicalTime(s));
    QNP_CHECK(runStateIsNetworkEmergency(s));
}

QNP_TEST(LifecycleBarrierStopsTimeButIsNotAnEmergency) {
    const NetplayRunState s = classifyRunState(holding(HoldCause::LifecycleBoundary));
    QNP_CHECK(s == NetplayRunState::ExactLifecycleBarrier);
    QNP_CHECK(runStateStopsCanonicalTime(s));
    QNP_CHECK(!runStateIsNetworkEmergency(s));
}

QNP_TEST(LocalInputMissingIsPressureNotAnEmergency) {
    const NetplayRunState s = classifyRunState(holding(HoldCause::LocalInputMissing));
    QNP_CHECK(s == NetplayRunState::PredictionPressure);
    QNP_CHECK(!runStateIsNetworkEmergency(s));
}

QNP_TEST(RetryPollDoesNotChangeTheReportedState) {
    RunStateInputs in = holding(HoldCause::NotCadence);
    QNP_CHECK(classifyRunState(in) == NetplayRunState::Running);
    in.recoverableDebtFrames = 3;
    QNP_CHECK(classifyRunState(in) == NetplayRunState::RecoveryCatchUp);
}

QNP_TEST(CorrectionIsWorkNotAStall) {
    RunStateInputs in = advancing();
    in.kind = WorkKind::Correct;
    const NetplayRunState s = classifyRunState(in);
    QNP_CHECK(s == NetplayRunState::CorrectionCatchUp);
    QNP_CHECK(!runStateStopsCanonicalTime(s));
}

QNP_TEST(PressureBandOpensAtTheWarningMargin) {
    RunStateInputs in = advancing();
    in.missingRemoteDepth = 9;
    QNP_CHECK(classifyRunState(in) == NetplayRunState::Running);
    in.missingRemoteDepth = 10;
    QNP_CHECK(classifyRunState(in) == NetplayRunState::PredictionPressure);
    QNP_CHECK(!runStateStopsCanonicalTime(classifyRunState(in)));
}

QNP_TEST(ZeroMarginCollapsesTheBandWithoutBreakingTheTrigger) {
    RunStateInputs in = advancing();
    in.warningMargin = 0;
    in.missingRemoteDepth = 11;
    QNP_CHECK(classifyRunState(in) == NetplayRunState::Running);
    in.missingRemoteDepth = 12;
    QNP_CHECK(classifyRunState(in) == NetplayRunState::PredictionPressure);
}

QNP_TEST(PressureOutranksRecoveryCatchUp) {
    RunStateInputs in = advancing();
    in.recoverableDebtFrames = 4;
    QNP_CHECK(classifyRunState(in) == NetplayRunState::RecoveryCatchUp);
    in.missingRemoteDepth = 11;
    QNP_CHECK(classifyRunState(in) == NetplayRunState::PredictionPressure);
}

QNP_TEST(SuspensionAndTerminationOutrankEveryNetworkState) {
    RunStateInputs in = holding(HoldCause::PredictionLimit);
    in.externalSuspension = true;
    QNP_CHECK(classifyRunState(in) == NetplayRunState::ExternalSuspension);
    QNP_CHECK(!runStateIsNetworkEmergency(classifyRunState(in)));
    in.sessionTerminated = true;
    QNP_CHECK(classifyRunState(in) == NetplayRunState::Terminated);
}

QNP_TEST(NativeFreezeIsNotAnEmergency) {
    for (const bool frozen : {false, true}) {
        RunStateInputs run = advancing();
        run.nativeFreezeActive = frozen;
        QNP_CHECK(classifyRunState(run) == NetplayRunState::Running);

        RunStateInputs pressure = advancing();
        pressure.nativeFreezeActive = frozen;
        pressure.missingRemoteDepth = 11;
        QNP_CHECK(classifyRunState(pressure) == NetplayRunState::PredictionPressure);
    }
}

QNP_TEST(ClassifierNeverReturnsHoldNegotiating) {
    for (const HoldCause cause : {HoldCause::None, HoldCause::NotCadence,
                                  HoldCause::LocalInputMissing,
                                  HoldCause::PredictionLimit,
                                  HoldCause::LifecycleBoundary}) {
        QNP_CHECK(classifyRunState(holding(cause)) != NetplayRunState::HoldNegotiating);
    }
}

QNP_TEST(LedgerOpensAndClosesOneEpisodePerContiguousHold) {
    HoldEpisodeLedger led;
    QNP_CHECK(led.episodes() == 0 && !led.active());

    led.observe(NetplayRunState::EmergencyNetworkHold, 1000, 500, 12);
    QNP_CHECK(led.active());
    QNP_CHECK(led.episodes() == 1);
    QNP_CHECK(led.current().generation == 1);
    QNP_CHECK(led.current().firstMissingFrame == 500);
    QNP_CHECK(led.current().localMaxRollback == 12);

    led.observe(NetplayRunState::EmergencyNetworkHold, 2000, 500, 12);
    led.observe(NetplayRunState::EmergencyNetworkHold, 3000, 500, 12);
    QNP_CHECK(led.current().opportunitiesHeld == 3);
    QNP_CHECK(led.heldOpportunities() == 3);

    led.observe(NetplayRunState::Running, 4000, 0, 12);
    QNP_CHECK(!led.active());
    QNP_CHECK(led.episodes() == 1);
    QNP_CHECK(led.totalHeldUs() == 3000);
}

QNP_TEST(LedgerIgnoresLifecycleBarriersAndSuspensions) {
    HoldEpisodeLedger led;
    for (const NetplayRunState s : {NetplayRunState::ExactLifecycleBarrier,
                                    NetplayRunState::ExternalSuspension,
                                    NetplayRunState::PredictionPressure,
                                    NetplayRunState::CorrectionCatchUp,
                                    NetplayRunState::RecoveryCatchUp}) {
        led.observe(s, 1000, 0, 12);
    }
    QNP_CHECK(led.episodes() == 0);
    QNP_CHECK(led.totalHeldUs() == 0);
    QNP_CHECK(led.generation() == 0);
}

QNP_TEST(LedgerKeepsTheWorstEpisodeNotTheLast) {
    HoldEpisodeLedger led;
    led.observe(NetplayRunState::EmergencyNetworkHold, 1000, 10, 12);
    led.observe(NetplayRunState::EmergencyNetworkHold, 9000, 10, 12);
    led.observe(NetplayRunState::Running, 9500, 0, 12);
    led.observe(NetplayRunState::EmergencyNetworkHold, 20000, 77, 12);
    led.observe(NetplayRunState::EmergencyNetworkHold, 20100, 77, 12);
    led.observe(NetplayRunState::Running, 20200, 0, 12);

    QNP_CHECK(led.episodes() == 2);
    QNP_CHECK(led.worst().durationUs() == 8500);
    QNP_CHECK(led.worst().firstMissingFrame == 10);
    QNP_CHECK(led.totalHeldUs() == 8700);
}

QNP_TEST(RecoveryDepthAttachesOnlyToTheFirstCorrectionAfterAHold) {
    HoldEpisodeLedger led;
    led.observe(NetplayRunState::EmergencyNetworkHold, 1000, 10, 12);
    led.observe(NetplayRunState::Running, 2000, 0, 12);
    led.observeRecovery(7);
    QNP_CHECK(led.deepestRecovery() == 7);
    QNP_CHECK(led.worst().recoveryDepth == 7);

    led.observeRecovery(30);
    QNP_CHECK(led.deepestRecovery() == 7);
    QNP_CHECK(led.worst().recoveryDepth == 7);
}

QNP_TEST(RecoveryBeforeAnyHoldIsIgnored) {
    HoldEpisodeLedger led;
    led.observeRecovery(9);
    QNP_CHECK(led.deepestRecovery() == 0);
    QNP_CHECK(led.episodes() == 0);
}

QNP_TEST(LedgerIsBoundedAcrossManyEpisodes) {
    HoldEpisodeLedger led;
    uint64_t t = 0;
    for (int i = 0; i < 5000; ++i) {
        led.observe(NetplayRunState::EmergencyNetworkHold, t, static_cast<uint32_t>(i), 12);
        t += 100;
        led.observe(NetplayRunState::Running, t, 0, 12);
        t += 100;
    }
    QNP_CHECK(led.episodes() == 5000);
    QNP_CHECK(led.generation() == 5000);
    QNP_CHECK(sizeof(HoldEpisodeLedger) < 256);
}

QNP_TEST(LedgerResetClearsEverything) {
    HoldEpisodeLedger led;
    led.observe(NetplayRunState::EmergencyNetworkHold, 1000, 10, 12);
    led.reset();
    QNP_CHECK(!led.active());
    QNP_CHECK(led.episodes() == 0);
    QNP_CHECK(led.generation() == 0);
    QNP_CHECK(led.totalHeldUs() == 0);
    QNP_CHECK(led.heldOpportunities() == 0);
}
