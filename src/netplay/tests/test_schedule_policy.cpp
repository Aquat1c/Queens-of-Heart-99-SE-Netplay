#include "TestHarness.h"

#include "game/NetplaySchedulePolicy.h"

using namespace qgame;

namespace {

FrameDomains domains(uint32_t simFrontier, uint32_t remotePrefix,
                     uint8_t maxRollback) {
    FrameDomains d;
    d.simFrontier = simFrontier;
    d.remoteActualPrefix = remotePrefix;
    d.localMaxRollback = maxRollback;
    return d;
}

OpportunityInputs advanceable() {
    OpportunityInputs in;
    in.cadenceOpportunity = true;
    in.localInputReady = true;
    in.remoteActualAtFrontier = true;
    in.safeForwardHeadroom = 8;
    return in;
}

const SchedulePolicyConfig kCfg{};

}

QNP_TEST(RetryPollHoldsWithoutDebt) {
    OpportunityInputs in = advanceable();
    in.cadenceOpportunity = false;
    const SchedulePlan p = planSchedule(domains(100, 100, 8), in, kCfg);
    QNP_CHECK(p.kind == WorkKind::Hold);
    QNP_CHECK(p.hold == HoldCause::NotCadence);
    QNP_CHECK(p.forwardFrames == 0 && p.debt == DebtDisposition::None);
}

QNP_TEST(PendingCorrectionRunsFirstAndOwesNoForwardDebt) {
    OpportunityInputs in = advanceable();
    in.pendingCorrection = true;
    const SchedulePlan p = planSchedule(domains(108, 100, 8), in, kCfg);
    QNP_CHECK(p.kind == WorkKind::Correct);
    QNP_CHECK(p.debt == DebtDisposition::None && p.forwardFrames == 0);
}

QNP_TEST(LocalInputMissingIsNotARecoverableNetworkWait) {
    OpportunityInputs in = advanceable();
    in.localInputReady = false;
    const SchedulePlan p = planSchedule(domains(100, 100, 8), in, kCfg);
    QNP_CHECK(p.kind == WorkKind::Hold && p.hold == HoldCause::LocalInputMissing);
    QNP_CHECK(p.debt == DebtDisposition::None);
}

QNP_TEST(EightSpeculativeFramesLegalNinthPredictionLimitStalls) {
    OpportunityInputs in = advanceable();
    in.remoteActualAtFrontier = false;

    const SchedulePlan a = planSchedule(domains(107, 100, 8), in, kCfg);
    QNP_CHECK(a.kind == WorkKind::Advance && a.forwardFrames == 1);

    const SchedulePlan b = planSchedule(domains(108, 100, 8), in, kCfg);
    QNP_CHECK(b.kind == WorkKind::Hold && b.hold == HoldCause::PredictionLimit);
    QNP_CHECK(b.debt == DebtDisposition::CreateOne);
    QNP_CHECK(predictionDepth(domains(108, 100, 8)) == 8);
}

QNP_TEST(IncreasingMaxRollbackPreventsTheCapHoldWithoutForcingACorrection) {
    OpportunityInputs in = advanceable();
    in.remoteActualAtFrontier = false;
    QNP_CHECK(planSchedule(domains(108, 100, 8), in, kCfg).kind == WorkKind::Hold);
    QNP_CHECK(planSchedule(domains(108, 100, 12), in, kCfg).kind == WorkKind::Advance);
}

QNP_TEST(ShortPredictionHoldAccruesOneOpportunityAndRepays) {
    CadenceDebt debt;
    OpportunityInputs hold = advanceable();
    hold.remoteActualAtFrontier = false;
    const SchedulePlan h = planSchedule(domains(108, 100, 8), hold, kCfg);
    debt.apply(h);
    QNP_CHECK(debt.frames() == 1);

    OpportunityInputs go = advanceable();
    go.recoverableDebtFrames = debt.frames();
    const SchedulePlan g = planSchedule(domains(108, 108, 8), go, kCfg);
    QNP_CHECK(g.kind == WorkKind::Advance);
    QNP_CHECK(g.forwardFrames == 2);
    QNP_CHECK(g.visibleIndex == 1);
    QNP_CHECK(g.debt == DebtDisposition::Consume && g.debtConsumed == 1);
    debt.apply(g);
    QNP_CHECK(debt.frames() == 0);
}

QNP_TEST(CorrectionReplayDoesNotReduceForwardDebt) {
    CadenceDebt debt;
    OpportunityInputs hold = advanceable();
    hold.remoteActualAtFrontier = false;
    debt.apply(planSchedule(domains(108, 100, 8), hold, kCfg));
    debt.apply(planSchedule(domains(108, 100, 8), hold, kCfg));
    QNP_CHECK(debt.frames() == 2);
    OpportunityInputs corr = advanceable();
    corr.pendingCorrection = true;
    debt.apply(planSchedule(domains(108, 100, 8), corr, kCfg));
    QNP_CHECK(debt.frames() == 2);
}

QNP_TEST(ExternalSuspendDiscardsDebtAndDebtIsBounded) {
    CadenceDebt debt;
    OpportunityInputs hold = advanceable();
    hold.remoteActualAtFrontier = false;
    for (int i = 0; i < 50; ++i)
        debt.apply(planSchedule(domains(108, 100, 8), hold, kCfg));
    QNP_CHECK(debt.frames() == CadenceDebt::kMaxFrames);
    QNP_CHECK(debt.saturated());
    debt.discardExternal();
    QNP_CHECK(debt.frames() == 0);
}

QNP_TEST(CatchupIsBoundedByCapAndHeadroom) {
    OpportunityInputs go = advanceable();
    go.recoverableDebtFrames = 5;
    go.safeForwardHeadroom = 3;
    QNP_CHECK(planSchedule(domains(108, 108, 8), go, kCfg).forwardFrames == 2);
    SchedulePolicyConfig wide{};
    wide.maxCatchupExtraPerOpportunity = 4;
    const SchedulePlan p = planSchedule(domains(108, 108, 8), go, wide);
    QNP_CHECK(p.forwardFrames == 4);
    QNP_CHECK(p.debtConsumed == 3);
}

QNP_TEST(SessionEventDrainCannotCatchUpPastItsExactBoundary) {
    OpportunityInputs drain = advanceable();
    drain.sessionEventDrain = true;
    drain.recoverableDebtFrames = CadenceDebt::kMaxFrames;
    drain.safeForwardHeadroom = 8u;

    FrameDomains trailing = domains(100u, 100u, 8u);
    trailing.peerSimFrontier = 108u;
    trailing.peerSampleFresh = true;
    const SchedulePlan plan = planSchedule(trailing, drain, kCfg);
    QNP_CHECK(plan.kind == WorkKind::Advance);
    QNP_CHECK(plan.forwardFrames == 1u);
    QNP_CHECK(plan.visibleIndex == 0u);
    QNP_CHECK(plan.debt == DebtDisposition::None);
    QNP_CHECK(plan.debtConsumed == 0u);
}

QNP_TEST(LeadingPeerIsNeverHeldToChaseIt) {
    FrameDomains d = domains(108, 108, 8);
    d.peerSimFrontier = 105;
    d.peerSampleFresh = true;
    const SchedulePlan p = planSchedule(d, advanceable(), kCfg);
    QNP_CHECK(p.kind == WorkKind::Advance);
    QNP_CHECK(p.forwardFrames == 1);
    QNP_CHECK(p.debt == DebtDisposition::None);
    QNP_CHECK(p.frameAdvantageValid && p.frameAdvantage == 3);

    d.peerSimFrontier = 60;
    const SchedulePlan far = planSchedule(d, advanceable(), kCfg);
    QNP_CHECK(far.kind == WorkKind::Advance);

    FrameDomains atCeiling = domains(108, 100, 8);
    atCeiling.peerSimFrontier = 60;
    atCeiling.peerSampleFresh = true;
    OpportunityInputs noActual = advanceable();
    noActual.remoteActualAtFrontier = false;
    const SchedulePlan held = planSchedule(atCeiling, noActual, kCfg);
    QNP_CHECK(held.kind == WorkKind::Hold);
    QNP_CHECK(held.hold == HoldCause::PredictionLimit);
}

QNP_TEST(TrailingPeerGetsBoundedCatchupOnlyWithLegalRoom) {
    FrameDomains d = domains(108, 108, 8);
    d.peerSimFrontier = 113;
    d.peerSampleFresh = true;
    OpportunityInputs go = advanceable();
    go.safeForwardHeadroom = 8;
    SchedulePolicyConfig wide{};
    wide.maxCatchupExtraPerOpportunity = 4;
    const SchedulePlan p = planSchedule(d, go, wide);
    QNP_CHECK(p.kind == WorkKind::Advance);
    QNP_CHECK(p.frameAdvantage == -5);
    QNP_CHECK(p.forwardFrames == 4 && p.debtConsumed == 3);
    go.safeForwardHeadroom = 1;
    QNP_CHECK(planSchedule(d, go, wide).forwardFrames == 2);
}

QNP_TEST(WithinDeadbandNormalSingleVisibleAdvance) {
    FrameDomains d = domains(108, 108, 8);
    d.peerSimFrontier = 107;
    d.peerSampleFresh = true;
    const SchedulePlan p = planSchedule(d, advanceable(), kCfg);
    QNP_CHECK(p.kind == WorkKind::Advance && p.forwardFrames == 1);
    QNP_CHECK(p.debt == DebtDisposition::None);
}

QNP_TEST(FrameAdvantageIgnoredWithoutAFreshPeerSample) {
    FrameDomains d = domains(108, 108, 8);
    d.peerSimFrontier = 90;
    d.peerSampleFresh = false;
    const SchedulePlan p = planSchedule(d, advanceable(), kCfg);
    QNP_CHECK(!p.frameAdvantageValid && p.frameAdvantage == 0);
    QNP_CHECK(p.kind == WorkKind::Advance && p.forwardFrames == 1);
}

QNP_TEST(PredictionDepthIsWrapSafe) {
    QNP_CHECK(predictionDepth(domains(100, 100, 8)) == 0);
    QNP_CHECK(predictionDepth(domains(100, 105, 8)) == 0);
    QNP_CHECK(predictionDepth(domains(105, 100, 8)) == 5);
    QNP_CHECK(predictionDepth(domains(3u, 0xFFFFFFFEu, 8)) == 5);
}

#include "game/NetplayPacingDeadline.h"
#include "game/NetplayPresentationPolicy.h"

QNP_TEST(RoundIntroRotationPresentationCriticalWindowIsExact) {
    QNP_CHECK(roundIntroRotationPresentationCritical(6u, 0u));
    QNP_CHECK(roundIntroRotationPresentationCritical(6u, 120u));
    QNP_CHECK(roundIntroRotationPresentationCritical(5u, 120u));
    QNP_CHECK(roundIntroRotationPresentationCritical(5u, 61u));
    QNP_CHECK(!roundIntroRotationPresentationCritical(5u, 60u));
    QNP_CHECK(!roundIntroRotationPresentationCritical(5u, 0u));
    QNP_CHECK(!roundIntroRotationPresentationCritical(4u, 999u));
    QNP_CHECK(!roundIntroRotationPresentationCritical(7u, 999u));
}

QNP_TEST(RoundIntroRotationWindowDefersCatchupWithoutConsumingDebt) {
    CadenceDebt debt;
    debt.createBounded(3u);

    OpportunityInputs critical = advanceable();
    critical.recoverableDebtFrames = debt.frames();
    if (roundIntroRotationPresentationCritical(5u, 61u))
        critical.safeForwardHeadroom = 0u;

    SchedulePolicyConfig wide{};
    wide.maxCatchupExtraPerOpportunity = 4u;
    const SchedulePlan guarded =
        planSchedule(domains(108, 108, 8), critical, wide);
    QNP_CHECK(guarded.kind == WorkKind::Advance);
    QNP_CHECK(guarded.forwardFrames == 1u);
    QNP_CHECK(guarded.visibleIndex == 0u);
    QNP_CHECK(guarded.debt == DebtDisposition::None);
    QNP_CHECK(guarded.debtConsumed == 0u);
    debt.apply(guarded);
    QNP_CHECK(debt.frames() == 3u);

    OpportunityInputs resumed = advanceable();
    resumed.recoverableDebtFrames = debt.frames();
    if (roundIntroRotationPresentationCritical(5u, 60u))
        resumed.safeForwardHeadroom = 0u;
    const SchedulePlan catchup =
        planSchedule(domains(108, 108, 8), resumed, wide);
    QNP_CHECK(catchup.forwardFrames == 4u);
    QNP_CHECK(catchup.visibleIndex == 3u);
    QNP_CHECK(catchup.debt == DebtDisposition::Consume);
    QNP_CHECK(catchup.debtConsumed == 3u);
    debt.apply(catchup);
    QNP_CHECK(debt.frames() == 0u);

    QNP_CHECK(rollbackCorrectionTickVisibility()
              == NativeTickVisibility::Historical);
    QNP_CHECK(spectatorCatchupTickVisibility(0u, 2u)
              == NativeTickVisibility::Historical);
    QNP_CHECK(spectatorCatchupExecutedTickVisibility(0u, 8u, true)
              == NativeTickVisibility::VisibleCanonical);
}

QNP_TEST(DeadlineRebaseReportsDiscardedWholeFrames) {
    NetplayPacingDeadline dl;
    dl.configure(1000);
    QNP_CHECK(dl.poll(0).ready());
    const PacingDeadlinePoll p = dl.poll(4500);
    QNP_CHECK(p.ready() && p.rebased());
    QNP_CHECK(p.rebasedDebtFrames == 3);
    QNP_CHECK(dl.idealDeadline() == 5500);
}

QNP_TEST(DeadlineSubFrameLatenessKeepsSmoothingNoDebtReport) {
    NetplayPacingDeadline dl;
    dl.configure(1000);
    QNP_CHECK(dl.poll(0).ready());
    const PacingDeadlinePoll p = dl.poll(1400);
    QNP_CHECK(p.ready() && !p.rebased());
    QNP_CHECK(p.rebasedDebtFrames == 0);
    QNP_CHECK(dl.idealDeadline() == 2000);
}

QNP_TEST(RebasedFramesBecomeDebtOnlyForNetworkWaits) {
    CadenceDebt debt;
    applyRebasedFrames(debt, 3, HoldCause::PredictionLimit);
    QNP_CHECK(debt.frames() == 3);
    applyRebasedFrames(debt, 2, HoldCause::LocalInputMissing);
    QNP_CHECK(debt.frames() == 3);
    applyRebasedFrames(debt, 5, HoldCause::LifecycleBoundary);
    QNP_CHECK(debt.frames() == 3);
    applyRebasedFrames(debt, 50, HoldCause::PredictionLimit);
    QNP_CHECK(debt.frames() == CadenceDebt::kMaxFrames);
    applyRebasedFrames(debt, -4, HoldCause::PredictionLimit);
    QNP_CHECK(debt.frames() == CadenceDebt::kMaxFrames);
}

QNP_TEST(CadenceDebtCreateBoundedSaturates) {
    CadenceDebt debt;
    debt.createBounded(5);
    QNP_CHECK(debt.frames() == 5);
    debt.createBounded(9);
    QNP_CHECK(debt.frames() == CadenceDebt::kMaxFrames && debt.saturated());
    debt.discardExternal();
    QNP_CHECK(debt.frames() == 0);
}

QNP_TEST(PlayerCatchupVisibilityMarksTheLastEXECUTEDTickVisible) {
    QNP_CHECK(playerCatchupTickVisibility(0, 0) == NativeTickVisibility::VisibleCanonical);
    QNP_CHECK(playerCatchupTickVisibility(0, 1) == NativeTickVisibility::HiddenNewForward);
    QNP_CHECK(playerCatchupTickVisibility(1, 1) == NativeTickVisibility::VisibleCanonical);

    for (size_t planned = 1; planned <= 4; ++planned) {
        for (size_t lastIndex = 0; lastIndex < planned; ++lastIndex) {
            size_t visible = 0;
            for (size_t i = 0; i <= lastIndex; ++i) {
                if (playerCatchupTickVisibility(i, lastIndex)
                    == NativeTickVisibility::VisibleCanonical)
                    ++visible;
            }
            QNP_CHECK(visible == 1);
            QNP_CHECK(playerCatchupTickVisibility(lastIndex, lastIndex)
                      == NativeTickVisibility::VisibleCanonical);
        }
    }

    QNP_CHECK(rollbackCorrectionTickVisibility() == NativeTickVisibility::Historical);
    QNP_CHECK(spectatorCatchupTickVisibility(0, 2) == NativeTickVisibility::Historical);
    for (size_t planned = 3; planned <= 24; ++planned) {
        for (size_t closing = 1; closing + 1 < planned; ++closing) {
            QNP_CHECK(spectatorCatchupExecutedTickVisibility(
                          closing, planned, true)
                      == NativeTickVisibility::VisibleCanonical);
        }
    }
    QNP_CHECK(playerCatchupTickVisibility(0, 1) != rollbackCorrectionTickVisibility());
}

QNP_TEST(PresentationContractRejectsSimulatingWithoutDrawing) {
    QNP_CHECK(opportunityStarvedPresentation(1, 0));
    QNP_CHECK(opportunityStarvedPresentation(62, 0));
    QNP_CHECK(!opportunityStarvedPresentation(1, 1));
    QNP_CHECK(!opportunityStarvedPresentation(4, 1));
    QNP_CHECK(!opportunityStarvedPresentation(0, 0));
}
