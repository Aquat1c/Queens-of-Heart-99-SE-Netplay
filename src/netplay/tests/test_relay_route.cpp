#include "TestHarness.h"

#include "../../net/RelayRoute.h"

QNP_TEST(RelayRouteHonorsDirectGraceAndCommitsImmutably) {
    qnet::relay::RouteSelector selector;
    QNP_CHECK(selector.start(1000));
    QNP_CHECK(!selector.start(1000));
    QNP_CHECK(selector.shouldProbeDirect());
    QNP_CHECK(!selector.shouldProbeRelay(2499));
    QNP_CHECK(selector.update(2499, false, true)
              == qnet::relay::SelectedRoute::None);
    QNP_CHECK(selector.shouldProbeRelay(2500));
    QNP_CHECK(selector.update(2500, false, true)
              == qnet::relay::SelectedRoute::Relay);
    QNP_CHECK(selector.update(2501, true, true)
              == qnet::relay::SelectedRoute::Relay);
}

QNP_TEST(RelayRoutePrefersDirectWhenBothEstablishTogether) {
    qnet::relay::RouteSelector selector;
    QNP_CHECK(selector.start(50));
    QNP_CHECK(selector.update(1550, true, true)
              == qnet::relay::SelectedRoute::Direct);
    QNP_CHECK(!selector.expired(50000));
}

QNP_TEST(RelayRouteForceRelayNeverProbesOrCommitsDirect) {
    qnet::relay::RouteSelector selector(true);
    QNP_CHECK(selector.start(0));
    QNP_CHECK(!selector.shouldProbeDirect());
    QNP_CHECK(selector.shouldProbeRelay(0));
    QNP_CHECK(selector.update(0, true, true)
              == qnet::relay::SelectedRoute::Relay);
    QNP_CHECK(selector.update(1, true, false)
              == qnet::relay::SelectedRoute::Relay);
}

QNP_TEST(RelayRouteDeadlineAndClockRegressionAreSafe) {
    qnet::relay::RouteSelector invalid(false, 1500, 1500);
    QNP_CHECK(!invalid.start(10));

    qnet::relay::RouteSelector selector;
    QNP_CHECK(selector.start(5000));
    QNP_CHECK(selector.update(4999, true, true)
              == qnet::relay::SelectedRoute::None);
    QNP_CHECK(!selector.expired(4999));
    QNP_CHECK(!selector.expired(16999));
    QNP_CHECK(selector.expired(17000));
}
