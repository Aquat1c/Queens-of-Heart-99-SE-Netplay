#include "TestHarness.h"

#include "net/RouteSelection.h"

using namespace qnet;

namespace {
RouteCandidate direct(RouteLabel label, bool validated, int p95us, int lossPerMille = 0,
                      bool sameObservedPublic = false) {
    RouteCandidate c;
    c.label = label;
    c.validated = validated;
    c.p95Micros = p95us;
    c.lossPerMille = lossPerMille;
    c.sameObservedPublicAddr = sameObservedPublic;
    return c;
}
const RouteSelectionConfig kCfg{};
}

QNP_TEST(PublicDirectFromSharedObservedAddressIsLabeledPossibleHairpin) {
    QNP_CHECK(resolveLabel(direct(RouteLabel::PublicDirect, true, 60000, 0, true))
              == RouteLabel::PublicDirectPossibleHairpin);
    QNP_CHECK(resolveLabel(direct(RouteLabel::PublicDirect, true, 60000, 0, false))
              == RouteLabel::PublicDirect);
    QNP_CHECK(resolveLabel(direct(RouteLabel::LanDirect, true, 1000)) == RouteLabel::LanDirect);
}

QNP_TEST(SelectsHealthyPrivateDirectOverPublicHairpinAndRelay) {
    RouteCandidate cands[3] = {
        direct(RouteLabel::LanDirect, true, 1000),
        direct(RouteLabel::PublicDirect, true, 60000, 0, true),
        direct(RouteLabel::Relay, true, 30000),
    };
    const RouteSelection s = selectRoute(cands, 3, false, kCfg);
    QNP_CHECK(s.index == 0 && s.label == RouteLabel::LanDirect);
}

QNP_TEST(FallsBackToRelayOnlyWhenDirectFailsAndDeadlineExpired) {
    RouteCandidate cands[3] = {
        direct(RouteLabel::LanDirect, false, 1000),
        direct(RouteLabel::PublicDirect, false, 60000),
        direct(RouteLabel::Relay, true, 30000),
    };
    QNP_CHECK(selectRoute(cands, 3, false, kCfg).index == -1);
    const RouteSelection s = selectRoute(cands, 3, true, kCfg);
    QNP_CHECK(s.index == 2 && s.label == RouteLabel::Relay);
}

QNP_TEST(ValidatedDirectAlwaysBeatsRelay) {
    RouteCandidate cands[2] = {
        direct(RouteLabel::PublicDirect, true, 40000),
        direct(RouteLabel::Relay, true, 20000),
    };
    QNP_CHECK(selectRoute(cands, 2, true, kCfg).label == RouteLabel::PublicDirect);
    QNP_CHECK(selectRoute(cands, 2, false, kCfg).label == RouteLabel::PublicDirect);
}

QNP_TEST(PrefersLanOnTieButBetterTailWins) {
    RouteCandidate tied[2] = {
        direct(RouteLabel::PublicDirect, true, 30000),
        direct(RouteLabel::LanDirect, true, 31000),
    };
    QNP_CHECK(selectRoute(tied, 2, false, kCfg).label == RouteLabel::LanDirect);

    RouteCandidate notTied[2] = {
        direct(RouteLabel::LanDirect, true, 50000),
        direct(RouteLabel::PublicDirect, true, 10000),
    };
    QNP_CHECK(selectRoute(notTied, 2, false, kCfg).label == RouteLabel::PublicDirect);
}

QNP_TEST(LowerLossBreaksASameLabelTie) {
    RouteCandidate cands[2] = {
        direct(RouteLabel::PublicDirect, true, 30000, 120),
        direct(RouteLabel::PublicDirect, true, 30500, 10),
    };
    QNP_CHECK(selectRoute(cands, 2, false, kCfg).index == 1);
}

QNP_TEST(RouteCommitMatchesOnlyIdenticalSessionAndKind) {
    QNP_CHECK(routeCommitMatches(42u, 1u, 42u, 1u));
    QNP_CHECK(routeCommitMatches(42u, 2u, 42u, 2u));
    QNP_CHECK(!routeCommitMatches(42u, 1u, 43u, 1u));
    QNP_CHECK(!routeCommitMatches(42u, 1u, 42u, 2u));
    QNP_CHECK(!routeCommitMatches(42u, 0u, 42u, 0u));
    QNP_CHECK(!routeCommitMatches(42u, 3u, 42u, 3u));
}

QNP_TEST(NoRouteWhenNothingValidatedAndDeadlineOpen) {
    RouteCandidate cands[2] = {
        direct(RouteLabel::LanDirect, false, 1000),
        direct(RouteLabel::Relay, false, 30000),
    };
    QNP_CHECK(selectRoute(cands, 2, false, kCfg).index == -1);
    QNP_CHECK(selectRoute(cands, 2, true, kCfg).index == -1);
}
