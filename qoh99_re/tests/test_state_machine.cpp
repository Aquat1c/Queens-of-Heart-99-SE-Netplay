#include "TestFramework.hpp"
#include "qoh99/MatchSystem.hpp"
#include "qoh99/SceneSystem.hpp"

using namespace qoh99;

TEST(statemachine_boot_sequence_classification) {
    CHECK(SceneSystem::classify(1, 0, 0) == GamePhase::Menu);
    CHECK(SceneSystem::classify(3, 1, 0) == GamePhase::VsList);
    CHECK(SceneSystem::classify(3, 2, 0) == GamePhase::CharSelect);
    CHECK(SceneSystem::classify(2, 2, -1) == GamePhase::MatchIntro);
    CHECK(SceneSystem::classify(2, 2, 0) == GamePhase::MatchGameplay);
    CHECK(SceneSystem::classify(2, 2, 5) == GamePhase::MatchRoundEnd);
    CHECK(SceneSystem::classify(0, 0, 0) == GamePhase::Transition);
    CHECK(SceneSystem::classify(4, 0, 0) == GamePhase::OpeningMovie);
    CHECK(static_cast<int>(Scene::OpeningMovie) == 4);
    CHECK(SceneSystem::classify(255, 0, 0) == GamePhase::Transition);
}

TEST(statemachine_matchsub_1_to_4_are_unknown) {
    for (int sub = 1; sub <= 4; ++sub) {
        GamePhase p = SceneSystem::classify(2, 0, sub);
        CHECK(p == GamePhase::MatchOther);
        PhasePolicy pol = SceneSystem::policyFor(p);
        CHECK(!pol.rollbackAllowed);
        CHECK(!pol.saveAllowed);
    }
}

TEST(statemachine_policy_rollback_only_in_gameplay) {
    const GamePhase all[] = {
        GamePhase::Boot, GamePhase::Menu, GamePhase::VsList, GamePhase::CharSelect,
        GamePhase::Stage3Other, GamePhase::MatchIntro, GamePhase::MatchGameplay,
        GamePhase::MatchOther, GamePhase::MatchRoundEnd, GamePhase::OpeningMovie,
        GamePhase::Transition, GamePhase::Unknown
    };
    int allowed = 0;
    for (GamePhase p : all)
        if (SceneSystem::policyFor(p).rollbackAllowed)
            ++allowed;
    CHECK_EQ(allowed, 1);
    CHECK(SceneSystem::policyFor(GamePhase::MatchGameplay).rollbackAllowed);
}

TEST(statemachine_intro_is_nocontrol) {
    PhasePolicy p = SceneSystem::policyFor(GamePhase::MatchIntro);
    CHECK(p.input == InputPolicy::NoInput);
    CHECK(!p.rollbackAllowed);
    CHECK(p.saveAllowed);
}

TEST(statemachine_gameplay_exit_transition_fact) {
    CHECK(MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::RoundWin));
    CHECK(MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::Unknown1));
    CHECK(!MatchSystem::exitsGameplay(MatchSub::Gameplay, MatchSub::Gameplay));
    CHECK(!MatchSystem::exitsGameplay(MatchSub::RoundIntro, MatchSub::Gameplay));
}

TEST(statemachine_direct_boot_sequence) {
    BufferMemoryView mem;
    DirectBootController boot(mem, 1);

    mem.set<int>(addr::kSceneCurrent, 1);
    mem.set<int>(addr::kMainMenuSubstate, 2);
    CHECK(boot.tick() == DirectBootController::Step::AtMenu);
    CHECK_EQ(mem.get<int>(addr::kSceneNext), 3);
    CHECK_EQ(mem.get<int>(addr::kVsListEntryFlag), 1);
    CHECK_EQ(mem.get<int>(addr::kVsFormationRow), 1);
    CHECK_EQ(addr::kMatchMode, addr::kVsFormationRow);
    CHECK_EQ(addr::kTeamSize, addr::kVsFormationRow);
    CHECK(DirectBootController::supportsRow(1));
    CHECK(boot.supported());

    mem.set<int>(addr::kSceneCurrent, 3);
    mem.set<int>(addr::kScene3Substate, 1);
    CHECK(boot.tick() == DirectBootController::Step::AtVsList);
    CHECK_EQ(mem.get<int>(addr::kScene3Cursor), 1);
    CHECK_EQ(mem.get<int>(addr::kScene3ConfirmFlag), 1);

    mem.set<int>(addr::kScene3Substate, 2);
    mem.set<int>(addr::kScene3ConfirmFlag, 0);
    CHECK(boot.tick() == DirectBootController::Step::AtCharSelect);
    CHECK_EQ(mem.get<int>(addr::kP1CharCursor), 0);
    CHECK_EQ(mem.get<int>(addr::kP2CharCursor), 0);
    CHECK_EQ(mem.get<int>(addr::kScene3ConfirmFlag), 0);
    CHECK_EQ(mem.get<int>(addr::kCharSelConfirmP1), 1);
    CHECK_EQ(mem.get<int>(addr::kCharSelConfirmP2), 1);

    mem.set<int>(addr::kSceneCurrent, 2);
    CHECK(boot.tick() == DirectBootController::Step::Launched);
    CHECK(boot.done());
}

TEST(statemachine_scene_reads_via_view) {
    BufferMemoryView mem;
    mem.set<int>(addr::kSceneCurrent, 2);
    mem.set<int>(addr::kMatchSubstate, 0);
    SceneSystem s(mem);
    CHECK(s.inMatch());
    CHECK(s.inGameplay());
    CHECK(s.phase() == GamePhase::MatchGameplay);
    mem.set<int>(addr::kMatchSubstate, 5);
    CHECK(!s.inGameplay());
}
