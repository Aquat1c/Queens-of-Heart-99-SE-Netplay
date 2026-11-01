#include "TestHarness.h"

#include "netplay/RematchTheater.h"

using namespace qnetplay;

namespace {

RematchTheaterObserved doorway() {
    RematchTheaterObserved o{};
    o.scene = 2u;
    o.sceneNext = 3u;
    o.formationRow = 1u;
    o.matchSub = 5;
    o.formationActive = 1u;
    o.loserSide = 1u;
    o.vacancyMask = 0u;
    o.continueOwner = kRematchTheaterNoOwner;
    o.continueTimer = 61u;
    return o;
}

RematchTheaterObserved flipped() {
    RematchTheaterObserved o = doorway();
    const RematchTheaterFlipPlan plan = rematchTheaterFlipPlan(doorway());
    o.formationRow = plan.formationRow;
    o.matchSub = plan.matchSub;
    o.sceneNext = plan.sceneNext;
    o.vacancyMask = plan.vacancyMask;
    return o;
}

}

QNP_TEST(RematchTheaterDoorwayEligibility) {
    QNP_CHECK(rematchTheaterDoorwayEligible(doorway()));
    {
        RematchTheaterObserved o = doorway();
        o.formationRow = 2u;
        QNP_CHECK(rematchTheaterDoorwayEligible(o));
        o.sceneNext = 2u;
        QNP_CHECK(rematchTheaterDoorwayEligible(o));
    }
    {
        RematchTheaterObserved o = doorway();
        o.scene = 3u;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }
    {
        RematchTheaterObserved o = doorway();
        o.matchSub = 2;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }
    {
        RematchTheaterObserved o = doorway();
        o.formationRow = 0u;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }
    {
        RematchTheaterObserved o = doorway();
        o.formationActive = 0u;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }
    {
        RematchTheaterObserved o = doorway();
        o.loserSide = 2u;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }
    {
        RematchTheaterObserved o = doorway();
        o.vacancyMask = 1u;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }
    {
        RematchTheaterObserved o = doorway();
        o.continueOwner = 0u;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }
    {
        RematchTheaterObserved o = doorway();
        o.continueTimer = 60u;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }
    {
        RematchTheaterObserved o = doorway();
        o.sceneNext = 1u;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }
}

QNP_TEST(RematchTheaterFlipPlanWrites) {
    const RematchTheaterFlipPlan plan = rematchTheaterFlipPlan(doorway());
    QNP_CHECK(plan.formationRow == 0u);
    QNP_CHECK(plan.matchSub == 2);
    QNP_CHECK(plan.sceneNext == 2u);
    QNP_CHECK(plan.vacancyMask == (1u << 1));
    {
        RematchTheaterObserved o = doorway();
        o.loserSide = 0u;
        QNP_CHECK(rematchTheaterFlipPlan(o).vacancyMask == 1u);
    }
    QNP_CHECK(rematchTheaterFlippedStateSane(flipped(),
                                             doorway().loserSide));
    QNP_CHECK(rematchTheaterDoorwayEligible(doorway()));
}

QNP_TEST(RematchTheaterLeakedEdgeGuard) {
    const uint32_t loser = doorway().loserSide;
    QNP_CHECK(rematchTheaterFlippedStateSane(flipped(), loser));
    {
        RematchTheaterObserved o = flipped();
        o.matchSub = 5;
        QNP_CHECK(!rematchTheaterFlippedStateSane(o, loser));
    }
    {
        RematchTheaterObserved o = flipped();
        o.matchSub = -1;
        o.formationActive = 0u;
        QNP_CHECK(!rematchTheaterFlippedStateSane(o, loser));
    }
    {
        RematchTheaterObserved o = flipped();
        o.matchSub = 3;
        QNP_CHECK(!rematchTheaterFlippedStateSane(o, loser));
    }
    {
        RematchTheaterObserved o = flipped();
        o.continueOwner = 0u;
        QNP_CHECK(!rematchTheaterFlippedStateSane(o, loser));
    }
    {
        RematchTheaterObserved o = flipped();
        o.vacancyMask = 0u;
        QNP_CHECK(!rematchTheaterFlippedStateSane(o, loser));
    }
    {
        RematchTheaterObserved o = flipped();
        o.formationActive = 0u;
        QNP_CHECK(!rematchTheaterFlippedStateSane(o, loser));
    }
    {
        RematchTheaterObserved o = flipped();
        o.sceneNext = 3u;
        QNP_CHECK(!rematchTheaterFlippedStateSane(o, loser));
    }
    {
        RematchTheaterObserved o = flipped();
        o.scene = 3u;
        QNP_CHECK(!rematchTheaterFlippedStateSane(o, loser));
    }
    {
        RematchTheaterObserved o = flipped();
        o.formationRow = 1u;
        QNP_CHECK(!rematchTheaterFlippedStateSane(o, loser));
    }
}

QNP_TEST(RematchTheaterSimFeedMasking) {
    using namespace qinput;
    const uint32_t held = kLogicalLeft | kLogicalDown | kLogicalA
        | kLogicalB | kLogicalC | kLogicalD;
    QNP_CHECK(rematchTheaterLocalFeed(held, false)
              == (kLogicalLeft | kLogicalDown));
    QNP_CHECK((rematchTheaterLocalFeed(held, false)
               & kLogicalButtonMask) == 0u);
    QNP_CHECK(rematchTheaterLocalFeed(held, true) == 0u);
    QNP_CHECK(rematchTheaterLocalFeed(0u, false) == 0u);
}

QNP_TEST(RematchTheaterCursorAndConfirmMapping) {
    using namespace qinput;
    QNP_CHECK(rematchTheaterCursorIsYes(0u));
    QNP_CHECK(!rematchTheaterCursorIsYes(1u));
    QNP_CHECK(rematchTheaterConfirmEdge(0u, kLogicalA));
    QNP_CHECK(rematchTheaterConfirmEdge(kLogicalLeft,
                                        kLogicalLeft | kLogicalD));
    QNP_CHECK(!rematchTheaterConfirmEdge(0u, kLogicalRight));
    QNP_CHECK(!rematchTheaterConfirmEdge(kLogicalA, kLogicalA));
    QNP_CHECK(!rematchTheaterConfirmEdge(kLogicalA,
                                         kLogicalA | kLogicalB));
    QNP_CHECK(!rematchTheaterConfirmEdge(0u, 0u));
    QNP_CHECK(rematchTheaterFallbackCursorToggles(0u, kLogicalLeft));
    QNP_CHECK(rematchTheaterFallbackCursorToggles(0u, kLogicalRight));
    QNP_CHECK(!rematchTheaterFallbackCursorToggles(
        0u, kLogicalLeft | kLogicalRight));
    QNP_CHECK(!rematchTheaterFallbackCursorToggles(kLogicalLeft,
                                                   kLogicalLeft));
    QNP_CHECK(!rematchTheaterFallbackCursorToggles(kLogicalLeft, 0u));
    QNP_CHECK(rematchTheaterFallbackCursorToggles(
        kLogicalUp, kLogicalUp | kLogicalRight));
}

QNP_TEST(RematchTheaterRefusesVersusCpu) {
    RematchTheaterObserved cpu = doorway();
    cpu.committedCpuMask = 2u;
    QNP_CHECK(!rematchTheaterHumanVersusHuman(cpu));
    QNP_CHECK(!rematchTheaterDoorwayEligible(cpu));

    for (const uint32_t mask : {1u, 2u, 3u, 4u, 8u, 10u, 14u}) {
        RematchTheaterObserved o = doorway();
        o.committedCpuMask = mask;
        QNP_CHECK(!rematchTheaterDoorwayEligible(o));
    }

    RematchTheaterObserved leak = doorway();
    leak.vacancyMask = 0u;
    leak.formationRow = 1u;
    leak.formationActive = 1u;
    leak.committedCpuMask = 2u;
    QNP_CHECK(!rematchTheaterDoorwayEligible(leak));
    leak.committedCpuMask = 0u;
    QNP_CHECK(rematchTheaterDoorwayEligible(leak));

    QNP_CHECK(rematchTheaterHumanVersusHuman(doorway()));
    QNP_CHECK(rematchTheaterDoorwayEligible(doorway()));

    RematchTheaterObserved noFormation = doorway();
    noFormation.formationActive = 0u;
    QNP_CHECK(!rematchTheaterHumanVersusHuman(noFormation));
}
