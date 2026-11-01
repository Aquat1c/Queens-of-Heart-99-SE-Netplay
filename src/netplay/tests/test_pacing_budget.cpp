#include "TestHarness.h"

#include "net/PacingBudget.h"

using namespace qnet;

QNP_TEST(OneWayFramesCeilsHalfRttAtCanonicalRate) {
    QNP_CHECK(oneWayFrames(RouteRttMs{0}) == 0);
    QNP_CHECK(oneWayFrames(RouteRttMs{120}) == 4);
    QNP_CHECK(oneWayFrames(RouteRttMs{250}) == 8);
    QNP_CHECK(oneWayFrames(RouteRttMs{266}) == 9);
    QNP_CHECK(oneWayFrames(RouteRttMs{280}) == 9);
    QNP_CHECK(oneWayFrames(RouteRttMs{1}) == 1);
    QNP_CHECK(oneWayFrames(RouteRttMs{16}) == 1);
}

QNP_TEST(RequiredCoverageAddsSafetyMargin) {
    QNP_CHECK(requiredCoverage(RouteRttMs{280}, 2) == 11);
    QNP_CHECK(requiredCoverage(RouteRttMs{250}, 2) == 10);
    QNP_CHECK(requiredCoverage(RouteRttMs{120}, 1) == 5);
    const DirectionalRequirement sym = symmetricRequirement(RouteRttMs{280}, 2);
    QNP_CHECK(sym.hostFrames == 11 && sym.clientFrames == 11);
}

QNP_TEST(SafetyMarginGrowsWithTheRouteSoJustFitCannotHappenOnLongPaths) {

    QNP_CHECK(safetyMarginFramesForOneWay(0) == 2);
    QNP_CHECK(safetyMarginFramesForOneWay(3) == 2);
    QNP_CHECK(safetyMarginFramesForOneWay(5) == 2);
    QNP_CHECK(requiredCoverageForRoute(RouteRttMs{78}) == 5);
    QNP_CHECK(requiredCoverageForRoute(RouteRttMs{120}) == 6);

    QNP_CHECK(safetyMarginFramesForOneWay(9) == 3);
    QNP_CHECK(safetyMarginFramesForOneWay(12) == 4);
    QNP_CHECK(requiredCoverageForRoute(RouteRttMs{284}) == 12);
    const DirectionalRequirement route =
        symmetricRequirementForRoute(RouteRttMs{284});
    QNP_CHECK(route.hostFrames == 12 && route.clientFrames == 12);
    QNP_CHECK(classify(11, route.clientFrames)
              == Classification::Underbuffered);
    QNP_CHECK(classify(12, route.hostFrames) == Classification::Marginal);
    QNP_CHECK(classify(11, requiredCoverageForRoute(RouteRttMs{78}))
              == Classification::FullSpeed);

    for (int oneWay = 0; oneWay <= 30; ++oneWay)
        QNP_CHECK(safetyMarginFramesForOneWay(oneWay)
                  >= kDefaultSafetyMarginFrames);
}

QNP_TEST(DirectionalBudgetUsesSenderDelayPlusReceiverRollback) {
    const DirectionalBudget sym = directionalBudget(2, 8, 2, 8);
    QNP_CHECK(sym.hostReceive == 10 && sym.clientReceive == 10);

    const DirectionalBudget asym = directionalBudget(1, 8, 4, 8);
    QNP_CHECK(asym.hostReceive == 12 && asym.clientReceive == 9);

    QNP_CHECK(directionalBudget(5, 6, 5, 6).hostReceive == 11);
    QNP_CHECK(directionalBudget(2, 9, 2, 9).hostReceive == 11);
}

QNP_TEST(ClassifyMatchesBriefExamplesAt280ms) {
    const int required = requiredCoverage(RouteRttMs{280}, 2);
    QNP_CHECK(classify(directionalBudget(2, 8, 2, 8).hostReceive, required)
              == Classification::Underbuffered);
    QNP_CHECK(classify(directionalBudget(2, 9, 2, 9).hostReceive, required)
              == Classification::Marginal);
    QNP_CHECK(classify(directionalBudget(2, 11, 2, 11).hostReceive, required)
              == Classification::FullSpeed);
    QNP_CHECK(classify(directionalBudget(5, 6, 5, 6).hostReceive, required)
              == Classification::Marginal);
}

QNP_TEST(LowLatencyPresetSpendsRollbackBeforeDelay) {
    const DirectionalRequirement req{11, 11};
    const PeerAuthorization full{15, 15};
    const Allocation a = recommend(req, full, full, Preset::LowLatency);
    QNP_CHECK(a.hostMaxRollback == 12 && a.clientMaxRollback == 12);
    QNP_CHECK(a.hostDelay == 0 && a.clientDelay == 0);
    const DirectionalBudget b = directionalBudget(
        a.hostDelay, a.hostMaxRollback, a.clientDelay, a.clientMaxRollback);
    QNP_CHECK(b.hostReceive == 12 && b.clientReceive == 12);
    QNP_CHECK(classify(b.hostReceive, 11) == Classification::FullSpeed);
}

QNP_TEST(SmootherPresetPrefersDelayAndBackfillsAfterRollbackCap) {
    const DirectionalRequirement req{11, 11};
    const Allocation s = recommend(req, PeerAuthorization{15, 15},
                                   PeerAuthorization{15, 15}, Preset::Smoother);
    QNP_CHECK(s.hostMaxRollback == 6 && s.clientMaxRollback == 6);
    QNP_CHECK(s.hostDelay == 6 && s.clientDelay == 6);
    QNP_CHECK(directionalBudget(s.hostDelay, s.hostMaxRollback,
                                s.clientDelay, s.clientMaxRollback).hostReceive == 12);

    const Allocation capped = recommend(req, PeerAuthorization{4, 15},
                                        PeerAuthorization{4, 15}, Preset::Smoother);
    QNP_CHECK(capped.hostMaxRollback == 4 && capped.clientMaxRollback == 4);
    QNP_CHECK(capped.hostDelay == 8 && capped.clientDelay == 8);
    const DirectionalBudget cb = directionalBudget(
        capped.hostDelay, capped.hostMaxRollback,
        capped.clientDelay, capped.clientMaxRollback);
    QNP_CHECK(cb.hostReceive == 12 && cb.clientReceive == 12);
}

QNP_TEST(TheOldCeilingForcedInputDelayThatThirtyMakesOptional) {
    const int required = requiredCoverageForRoute(RouteRttMs{463});
    QNP_CHECK(required == 20);
    const DirectionalRequirement req{required, required};

    const Allocation old = recommend(req, PeerAuthorization{15, 15},
                                     PeerAuthorization{15, 15},
                                     Preset::LowLatency);
    QNP_CHECK(old.hostMaxRollback == 15 && old.clientMaxRollback == 15);
    QNP_CHECK(old.hostDelay == 6 && old.clientDelay == 6);
    QNP_CHECK(old.hostMaxRollback < required + 1);

    const PeerAuthorization full{kCapabilityRollback, kCapabilityDelay};
    const Allocation now = recommend(req, full, full, Preset::LowLatency);
    QNP_CHECK(now.hostDelay == 0 && now.clientDelay == 0);
    QNP_CHECK(now.hostMaxRollback == 21 && now.clientMaxRollback == 21);
    const DirectionalBudget budget = directionalBudget(
        now.hostDelay, now.hostMaxRollback,
        now.clientDelay, now.clientMaxRollback);
    QNP_CHECK(classify(budget.hostReceive, required)
              == Classification::FullSpeed);
    QNP_CHECK(classify(budget.clientReceive, required)
              == Classification::FullSpeed);

    const int longFloor = oneWayFrames(RouteRttMs{600});
    QNP_CHECK(longFloor == 19);
    QNP_CHECK(enforcedOwnRollback(longFloor, 0,
                                  0, 15) == 15);
    QNP_CHECK(enforcedOwnRollback(longFloor, 0,
                                  0,
                                  kCapabilityRollback) == 20);
}

QNP_TEST(ThePhysicalFloorIsWellBelowTheRecommendationTarget) {
    const int physicalFloor = oneWayFrames(RouteRttMs{400});
    const int recommendationTarget = requiredCoverageForRoute(RouteRttMs{463});
    QNP_CHECK(physicalFloor == 13);
    QNP_CHECK(recommendationTarget == 20);
    QNP_CHECK(physicalFloor < recommendationTarget);
    const int floored = enforcedOwnRollback(physicalFloor, 7,
                                            7,
                                            kCapabilityRollback);
    QNP_CHECK(floored == 7);
    const int alone = enforcedOwnRollback(physicalFloor, 0,
                                          0,
                                          kCapabilityRollback);
    QNP_CHECK(alone == 14);
}

QNP_TEST(AsymmetricRequirementsSolvedIndependently) {
    const DirectionalRequirement req{12, 9};
    const PeerAuthorization full{15, 15};
    const Allocation a = recommend(req, full, full, Preset::LowLatency);
    QNP_CHECK(a.hostMaxRollback == 13 && a.clientMaxRollback == 10);
    QNP_CHECK(a.hostDelay == 0 && a.clientDelay == 0);
    const DirectionalBudget b = directionalBudget(
        a.hostDelay, a.hostMaxRollback, a.clientDelay, a.clientMaxRollback);
    QNP_CHECK(b.hostReceive == 13 && b.clientReceive == 10);
}

QNP_TEST(IndependentAuthorizationsAreHonoured) {
    const DirectionalRequirement req{11, 11};
    const Allocation a = recommend(req, PeerAuthorization{8, 15},
                                   PeerAuthorization{15, 15}, Preset::LowLatency);
    QNP_CHECK(a.hostMaxRollback == 8);
    QNP_CHECK(a.clientDelay == 4);
    QNP_CHECK(a.clientMaxRollback == 12);
    QNP_CHECK(a.hostDelay == 0);
    const DirectionalBudget b = directionalBudget(
        a.hostDelay, a.hostMaxRollback, a.clientDelay, a.clientMaxRollback);
    QNP_CHECK(b.hostReceive == 12 && b.clientReceive == 12);
}

QNP_TEST(UnderAuthorizedCeilingsProduceHonestDeficit) {
    const DirectionalRequirement req{11, 11};
    const PeerAuthorization capped{4, 4};
    const Allocation a = recommend(req, capped, capped, Preset::LowLatency);
    QNP_CHECK(a.hostMaxRollback == 4 && a.clientMaxRollback == 4);
    QNP_CHECK(a.hostDelay == 4 && a.clientDelay == 4);
    const DirectionalBudget b = directionalBudget(
        a.hostDelay, a.hostMaxRollback, a.clientDelay, a.clientMaxRollback);
    QNP_CHECK(b.hostReceive == 8 && b.clientReceive == 8);
    QNP_CHECK(classify(b.hostReceive, req.hostFrames) == Classification::Underbuffered);
    QNP_CHECK(a.hostMaxRollback <= 4 && a.clientMaxRollback <= 4);
    QNP_CHECK(a.hostDelay <= 4 && a.clientDelay <= 4);
}

QNP_TEST(RaisingMaxRollbackRaisesBudgetWithoutForcingCorrections) {
    const int required = requiredCoverage(RouteRttMs{280}, 2);
    QNP_CHECK(directionalBudget(2, 8, 2, 8).hostReceive < required);
    QNP_CHECK(directionalBudget(2, 9, 2, 8).hostReceive == required);
    QNP_CHECK(directionalBudget(2, 12, 2, 8).hostReceive > required);
}

QNP_TEST(SessionFloorRaisesOwnRollbackToFullSpeedTarget) {
    const int lanOneWay = oneWayFrames(RouteRttMs{3});
    QNP_CHECK(lanOneWay == 1);
    QNP_CHECK(enforcedOwnRollback(lanOneWay, 0, 0, 15) == 2);
    QNP_CHECK(enforcedOwnRollback(lanOneWay, 1, 2, 15) == 2);
    QNP_CHECK(enforcedOwnRollback(3, 2, 0, 15) == 2);
    QNP_CHECK(enforcedOwnRollback(3, 4, 0, 15) == 0);
}

QNP_TEST(SessionFloorIsRaiseOnlyAndAuthCapped) {
    QNP_CHECK(enforcedOwnRollback(3, 0, 8, 15) == 8);
    QNP_CHECK(enforcedOwnRollback(11, 2, 0, 4) == 4);
    QNP_CHECK(enforcedOwnRollback(-1, -1, 0, 15) == 1);
    QNP_CHECK(enforcedOwnRollback(3, 0, 0, -1) == 0);
}

QNP_TEST(SessionFloorComposesToFullSpeedBothDirections) {
    const int required = 3;
    const int hostRb = enforcedOwnRollback(required, 0, 0, 15);
    const int clientRb = enforcedOwnRollback(required, 0, 0, 15);
    const DirectionalBudget b = directionalBudget(0, hostRb, 0, clientRb);
    QNP_CHECK(classify(b.hostReceive, required) == Classification::FullSpeed);
    QNP_CHECK(classify(b.clientReceive, required) == Classification::FullSpeed);
}

QNP_TEST(DelayFloorCoversWhatThePeerRollbackCeilingCannot) {
    QNP_CHECK(enforcedOwnDelay(20, 15, 0, 15) == 6);
    QNP_CHECK(enforcedOwnDelay(5, 15, 0, 15) == 0);
    QNP_CHECK(enforcedOwnDelay(14, 15, 0, 15) == 0);
    QNP_CHECK(enforcedOwnDelay(20, 15, 9, 15) == 9);
    QNP_CHECK(enforcedOwnDelay(30, 15, 0, 15) == 15);
    QNP_CHECK(enforcedOwnDelay(-1, -1, 0, 15) == 1);
    QNP_CHECK(enforcedOwnDelay(20, 15, 0, -1) == 0);
}

QNP_TEST(BothFloorsComposeToFullSpeedOnALongRoute) {
    const int required = 20;
    const int hostDelay = enforcedOwnDelay(required, 15, 0, 15);
    const int clientDelay = enforcedOwnDelay(required, 15, 0, 15);
    const int hostRb = enforcedOwnRollback(required, clientDelay, 0, 15);
    const int clientRb = enforcedOwnRollback(required, hostDelay, 0, 15);
    QNP_CHECK(hostDelay == 6 && clientDelay == 6);
    QNP_CHECK(hostRb == 15 && clientRb == 15);
    const DirectionalBudget b =
        directionalBudget(hostDelay, hostRb, clientDelay, clientRb);
    QNP_CHECK(classify(b.hostReceive, required) == Classification::FullSpeed);
    QNP_CHECK(classify(b.clientReceive, required) == Classification::FullSpeed);
}
