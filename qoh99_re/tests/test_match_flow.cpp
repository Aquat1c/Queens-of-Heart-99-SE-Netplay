#include "TestFramework.hpp"
#include "qoh99/SceneSystem.hpp"
#include "qoh99/MemoryView.hpp"

using namespace qoh99;

TEST(matchflow_launch_gate_formation_one_requires_both_selectors) {
    const int p1Only[4] = { 4, 1, 0, 0 };
    const int both[4]   = { 4, 4, 0, 0 };

    CHECK_EQ(MatchFlowModel::launchGateRequiredSelectors(1, 1), 2);
    CHECK(!MatchFlowModel::launchGateFires(
        1, 1, p1Only, MatchFlowModel::kLaunchLatchTarget));
    CHECK(MatchFlowModel::launchGateFires(
        1, 1, both, MatchFlowModel::kLaunchLatchTarget));
    for (int latch = 0; latch < MatchFlowModel::kLaunchLatchTarget; ++latch)
        CHECK(!MatchFlowModel::launchGateFires(1, 1, both, latch));

    CHECK(MatchFlowModel::launchGateRequiredSelectors(1,
                                                      1) == 2);
}

TEST(matchflow_launch_gate_formation_zero_is_the_auto_assign_branch) {
    const int p1Only[4] = { 3, 0, 0, 0 };
    const int p2Only[4] = { 0, 3, 0, 0 };
    const int both[4]   = { 3, 3, 0, 0 };

    CHECK_EQ(MatchFlowModel::launchGateRequiredSelectors(0, 0), 1);
    CHECK(MatchFlowModel::launchGateFires(
        0, 0, p1Only, MatchFlowModel::kLaunchLatchTarget));
    CHECK(MatchFlowModel::launchGateFires(
        0, 0, p2Only, MatchFlowModel::kLaunchLatchTarget));

    CHECK_EQ(MatchFlowModel::launchGateRequiredSelectors(0, 1), 2);
    CHECK(!MatchFlowModel::launchGateFires(
        0, 1, p1Only, MatchFlowModel::kLaunchLatchTarget));
    CHECK(MatchFlowModel::launchGateFires(
        0, 1, both, MatchFlowModel::kLaunchLatchTarget));

    const int confirmedFour[4] = { 4, 4, 0, 0 };
    CHECK(!MatchFlowModel::launchGateFires(
        0, 0, confirmedFour, MatchFlowModel::kLaunchLatchTarget));
}

TEST(matchflow_launch_gate_four_fighter_still_needs_all_four) {
    const int three[4] = { 4, 4, 4, 0 };
    const int four[4]  = { 4, 4, 4, 4 };
    CHECK_EQ(MatchFlowModel::launchGateRequiredSelectors(2, 1), 4);
    CHECK(!MatchFlowModel::launchGateFires(
        2, 1, three, MatchFlowModel::kLaunchLatchTarget));
    CHECK(MatchFlowModel::launchGateFires(
        2, 1, four, MatchFlowModel::kLaunchLatchTarget));
    CHECK_EQ(MatchFlowModel::launchGateRequiredSelectors(7, 1), 0);
    CHECK(!MatchFlowModel::launchGateFires(
        7, 1, four, MatchFlowModel::kLaunchLatchTarget));
}

TEST(matchflow_loser_bit_is_or_ed_only_when_no_formation_is_applied) {
    for (int loser = 0; loser < 2; ++loser) {
        CHECK_EQ(MatchFlowModel::cpuMaskAfterGameEnd(0, 0, loser),
                 1 << loser);
        CHECK_EQ(MatchFlowModel::cpuMaskAfterGameEnd(1, 0, loser), 0);
        CHECK_EQ(MatchFlowModel::cpuMaskAfterGameEnd(2, 0, loser), 0);
        CHECK_EQ(MatchFlowModel::cpuMaskAfterGameEnd(1, 2, loser), 2);
    }
    CHECK_EQ(MatchFlowModel::cpuMaskAfterGameEnd(0, 0, -1), 0);
    CHECK_EQ(MatchFlowModel::cpuMaskAfterGameEnd(0, 0, 4), 0);
}

TEST(matchflow_rematch_reapplies_the_row_and_zeroes_the_cpu_mask) {
    CHECK_EQ(MatchFlowModel::kSceneAfterGame, 3);
    CHECK_EQ(MatchFlowModel::kScene3SubAfterGame, 3);

    for (int loser = 0; loser < 2; ++loser) {
        const int afterGame =
            MatchFlowModel::cpuMaskAfterGameEnd(1, 0, loser);
        const MatchFlowModel::RematchState rematch =
            MatchFlowModel::rematchCharacterSelect(
                1, true, afterGame);
        CHECK(rematch.applied);
        CHECK_EQ(rematch.formation, 1);
        CHECK_EQ(rematch.vsApplied, 1);
        CHECK_EQ(rematch.cpuMask, 0);
        CHECK_EQ(rematch.scene3Sub, 2);
    }

    const MatchFlowModel::RematchState vsCpu =
        MatchFlowModel::rematchCharacterSelect(0, true, 2);
    CHECK(vsCpu.applied);
    CHECK_EQ(vsCpu.formation, 1);
    CHECK_EQ(vsCpu.cpuMask, 0);
    const VsRow* row0 = VsFormationModel::row(0);
    CHECK(row0 != nullptr);
    CHECK_EQ(static_cast<int>(row0->cpuMask), 2);

    const MatchFlowModel::RematchState noEntry =
        MatchFlowModel::rematchCharacterSelect(1, false, 3);
    CHECK(!noEntry.applied);
    CHECK_EQ(noEntry.cpuMask, 3);
    CHECK(!MatchFlowModel::rematchCharacterSelect(6, true, 0).applied);
}

TEST(matchflow_round_advance_is_formation_independent_and_bounded) {
    const int roundsToWinMinusOne = 1;
    int counter = 0;
    int wins = 0;
    int rounds = 0;
    while (rounds < 16) {
        ++rounds;
        ++wins;
        const MatchFlowModel::RoundEnd end =
            MatchFlowModel::advanceRound(counter, wins, roundsToWinMinusOne);
        counter = end.roundCounter;
        if (end.gameOver) break;
    }
    CHECK_EQ(rounds, 2);
    CHECK_EQ(counter, MatchFlowModel::kGameOverRoundCounter);

    counter = 0;
    rounds = 0;
    while (rounds < 16) {
        ++rounds;
        const MatchFlowModel::RoundEnd end =
            MatchFlowModel::advanceRound(counter, 0, 99);
        counter = end.roundCounter;
        if (end.gameOver) break;
    }
    CHECK_EQ(rounds, MatchFlowModel::kGameOverRoundCounter);
}

TEST(matchflow_completed_vs_game_returns_to_character_select_and_relaunches) {
    BufferMemoryView mem;
    SceneSystem scenes(mem);

    const int vsRow = 1;
    const VsRow* row = VsFormationModel::row(vsRow);
    CHECK(row != nullptr);

    mem.set<int>(addr::kSceneCurrent, 2);
    mem.set<int>(addr::kVsFormationRow, 1);
    mem.set<int>(addr::kControlMask, 0);
    mem.set<int>(addr::kRoundCounter, 0);
    CHECK(scenes.vsFormation() == VsFormation::TwoFighter);

    int counter = 0;
    int wins = 0;
    int rounds = 0;
    bool gameOver = false;
    for (int step = 0; step < 32 && !gameOver; ++step) {
        ++rounds;
        ++wins;
        const MatchFlowModel::RoundEnd end =
            MatchFlowModel::advanceRound(counter, wins, 1);
        counter = end.roundCounter;
        gameOver = end.gameOver;
        mem.set<int>(addr::kRoundCounter, counter);
        CHECK_EQ(mem.get<int>(addr::kSceneCurrent), 2);
    }
    CHECK(gameOver);
    CHECK_EQ(rounds, 2);

    const int formation = mem.get<int>(addr::kVsFormationRow);
    mem.set<int>(addr::kControlMask,
                 MatchFlowModel::cpuMaskAfterGameEnd(
                     formation, mem.get<int>(addr::kControlMask),
                     1));
    CHECK_EQ(mem.get<int>(addr::kControlMask), 0);

    CHECK(MatchFlowModel::postGameHandlerYieldsToSceneDispatcher(formation));
    mem.set<int>(addr::kSceneNext, MatchFlowModel::kSceneAfterGame);
    mem.set<int>(addr::kSceneCurrent, MatchFlowModel::kSceneAfterGame);
    mem.set<int>(addr::kScene3Substate, MatchFlowModel::kScene3SubAfterGame);
    CHECK(scenes.phase() == GamePhase::Stage3Other);

    const MatchFlowModel::RematchState rematch =
        MatchFlowModel::rematchCharacterSelect(
            vsRow, mem.get<int>(addr::kVsListEntryFlag) != 0,
            mem.get<int>(addr::kControlMask));
    CHECK(!rematch.applied);
    mem.set<int>(addr::kVsListEntryFlag, 1);
    const MatchFlowModel::RematchState applied =
        MatchFlowModel::rematchCharacterSelect(
            vsRow, true, mem.get<int>(addr::kControlMask));
    CHECK(applied.applied);
    mem.set<int>(addr::kVsFormationRow, applied.formation);
    mem.set<int>(addr::kControlMask, applied.cpuMask);
    mem.set<int>(addr::kScene3Substate, applied.scene3Sub);
    CHECK(scenes.phase() == GamePhase::CharSelect);
    CHECK_EQ(mem.get<int>(addr::kVsFormationRow), 1);
    CHECK_EQ(mem.get<int>(addr::kControlMask), 0);

    int confirm[4] = { 1, 1, 0, 0 };
    CHECK(!MatchFlowModel::launchGateFires(
        1, 1, confirm, MatchFlowModel::kLaunchLatchTarget));
    confirm[0] = 4;
    confirm[1] = 4;
    CHECK(MatchFlowModel::launchGateFires(
        1, 1, confirm, MatchFlowModel::kLaunchLatchTarget));
}

TEST(matchflow_formation_zero_control_takes_the_arcade_continue_loop) {
    CHECK(!MatchFlowModel::postGameHandlerYieldsToSceneDispatcher(0));
    CHECK(MatchFlowModel::postGameHandlerYieldsToSceneDispatcher(1));
    CHECK(MatchFlowModel::postGameHandlerYieldsToSceneDispatcher(2));
    CHECK(!MatchFlowModel::postGameHandlerYieldsToSceneDispatcher(3));

    int mask = 0;
    mask = MatchFlowModel::cpuMaskAfterGameEnd(0, mask, 0);
    mask = MatchFlowModel::cpuMaskAfterGameEnd(0, mask, 1);
    CHECK_EQ(mask, 3);
    int vsMask = 0;
    vsMask = MatchFlowModel::cpuMaskAfterGameEnd(1, vsMask, 0);
    vsMask = MatchFlowModel::cpuMaskAfterGameEnd(1, vsMask, 1);
    CHECK_EQ(vsMask, 0);
}
