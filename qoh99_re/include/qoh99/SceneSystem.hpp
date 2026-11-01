#pragma once
#include <cstddef>
#include <cstdint>
#include "AddressMap.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

enum class Scene : int {
    Transition = 0, TitleMenu = 1, Match = 2, VsSetup = 3,
    OpeningMovie = 4, Exit = 255
};

enum class Scene3Sub : int {
    Setup = 0, VsList = 1, CharSelect = 2, Confirm = 3
};

enum class MatchSub : int {
    RoundIntro = -1, Gameplay = 0,
    Unknown1 = 1, Unknown2 = 2, Unknown3 = 3, Unknown4 = 4,
    RoundWin = 5
};

enum class GamePhase {
    Boot, Menu, VsList, CharSelect, Stage3Other,
    MatchIntro,
    MatchGameplay,
    MatchOther,
    MatchRoundEnd,
    OpeningMovie,
    Transition, Unknown
};

enum class InputPolicy { NoInput, MenuInput, FullControl };

struct PhasePolicy {
    InputPolicy input;
    bool rollbackAllowed;
    bool saveAllowed;
};

// dword_62369C holds the applied formation: 0 = no VS row applied (arcade/story), 1 = two-fighter (VS rows 0, 1), 2 = four-fighter 2v2 (VS rows 2..5).
enum class VsFormation : int {
    NotSelected = 0,
    TwoFighter  = 1,
    FourFighter = 2
};

struct VsRow {
    int          row;
    VsFormation  formation;
    std::uint8_t humanSlotMask;
    std::uint8_t cpuMask;
    std::uint8_t route[4];
    int          requiredSelectorConfirms;
    const char*  description;
};

class VsFormationModel {
public:
    static constexpr std::size_t kRowCount        = 6;
    static constexpr int         kFighterSlots    = 4;
    static constexpr int         kSourceLanes     = 4;
    static constexpr int         kSelectorDone    = 4;
    static constexpr int         kSideLeft        = 0;
    static constexpr int         kSideRight       = 1;
    static constexpr int         kSideSlots[2][2] = { { 0, 2 }, { 1, 3 } };

    static const VsRow* rows(std::size_t& count);
    static const VsRow* row(int r);

    static VsFormation formationOf(int r);
    static bool isFourFighter(VsFormation f) { return f == VsFormation::FourFighter; }
    static bool isFourFighter(int r) { return isFourFighter(formationOf(r)); }
    static int  fighterCount(VsFormation f) {
        return f == VsFormation::FourFighter ? 4 : (f == VsFormation::TwoFighter ? 2 : 0);
    }

    static int  sideOfSlot(int slot) { return slot & 1; }
    static bool areTeammates(int a, int b) {
        return a != b && sideOfSlot(a) == sideOfSlot(b);
    }

    static bool isHumanSlot(int r, int slot);
    static bool isCpuSlot(int r, int slot);
    // Fighter slot to logical input source is dword_4A37C4[fighter+172]; row 3 is the non-identity case [0, 2, 1, 3].
    static int  sourceForSlot(int r, int slot);
    static bool hasIdentityRoute(int r);

    // The four-fighter formation waits until all of dword_8716D0/D4/D8/DC equal 4.
    static int  requiredSelectorConfirms(VsFormation f) {
        return f == VsFormation::FourFighter ? 4
             : (f == VsFormation::TwoFighter ? 2 : 0);
    }
    static int  requiredSelectorConfirms(int r) {
        return requiredSelectorConfirms(formationOf(r));
    }
    static bool launchReady(VsFormation f, const int confirmState[4]);
    static bool launchReady(int r, const int confirmState[4]) {
        return launchReady(formationOf(r), confirmState);
    }
};

class MatchFlowModel {
public:
    static int cpuMaskAfterGameEnd(int formation, int cpuMask, int loserSlot);

    static bool postGameHandlerYieldsToSceneDispatcher(int formation);

    static constexpr int kSceneAfterGame       = 3;
    static constexpr int kScene3SubAfterGame   = 3;

    struct RematchState {
        bool applied      = false;
        int  formation    = 0;
        int  vsApplied    = 0;
        int  cpuMask      = 0;
        int  scene3Sub    = 3;
    };
    static RematchState rematchCharacterSelect(int vsRow, bool vsListEntryFlag,
                                               int cpuMaskBefore);

    static bool launchGateFires(int formation, int vsApplied,
                                const int confirmState[4], int latch);
    static int  launchGateRequiredSelectors(int formation, int vsApplied);
    static constexpr int kLaunchLatchTarget = 30;

    static constexpr int kGameOverRoundCounter = 5;
    struct RoundEnd {
        int  roundCounter = 0;
        bool gameOver     = false;
    };
    static RoundEnd advanceRound(int roundCounter, int winnerRoundWins,
                                 int roundsToWinMinusOne);
};

class SceneSystem {
public:
    explicit SceneSystem(MemoryView& m) : m_(m) {}

    Scene scene() const        { return static_cast<Scene>(m_.get<int>(addr::kSceneCurrent)); }
    Scene sceneNext() const    { return static_cast<Scene>(m_.get<int>(addr::kSceneNext)); }
    Scene3Sub scene3Sub() const{ return static_cast<Scene3Sub>(m_.get<int>(addr::kScene3Substate)); }
    MatchSub matchSub() const  { return static_cast<MatchSub>(m_.get<int>(addr::kMatchSubstate)); }
    int mainMenuSub() const    { return m_.get<int>(addr::kMainMenuSubstate); }
    int mainMenuCursor() const { return m_.get<int>(addr::kMainMenuCursor); }

    int vsListRow() const      { return m_.get<int>(addr::kScene3Cursor); }
    VsFormation vsFormation() const {
        return static_cast<VsFormation>(m_.get<int>(addr::kVsFormationRow));
    }
    bool inFourFighterMode() const {
        return vsFormation() == VsFormation::FourFighter;
    }
    // dword_6236CC: bit i set => fighter slot i is native CPU (fighter+1400).
    int cpuMask() const        { return m_.get<int>(addr::kControlMask); }

    bool inMatch() const       { return scene() == Scene::Match; }
    bool inGameplay() const    { return inMatch() && matchSub() == MatchSub::Gameplay; }

    GamePhase phase() const;

    static GamePhase classify(int scene, int scene3Sub, int matchSub);
    static PhasePolicy policyFor(GamePhase p);

private:
    MemoryView& m_;
};

class DirectBootController {
public:
    enum class Step { AtMenu, AtVsList, AtCharSelect, Launched, Idle };

    explicit DirectBootController(MemoryView& m, int vsRow = 1)
        : m_(m), scenes_(m), vsRow_(vsRow) {}

    static bool supportsRow(int vsRow) {
        return VsFormationModel::formationOf(vsRow) == VsFormation::TwoFighter;
    }
    bool supported() const { return supportsRow(vsRow_); }
    int vsRow() const { return vsRow_; }

    Step tick();
    bool done() const { return step_ == Step::Launched; }

private:
    MemoryView& m_;
    SceneSystem scenes_;
    int vsRow_;
    Step step_ = Step::Idle;
    int confirmCooldown_ = 0;
};

}
