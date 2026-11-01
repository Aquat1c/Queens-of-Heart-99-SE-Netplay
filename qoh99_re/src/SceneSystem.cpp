#include "qoh99/SceneSystem.hpp"

namespace qoh99 {

GamePhase SceneSystem::phase() const {
    return classify(static_cast<int>(scene()),
                    static_cast<int>(scene3Sub()),
                    static_cast<int>(matchSub()));
}

GamePhase SceneSystem::classify(int scene, int scene3Sub, int matchSub) {
    switch (scene) {
        case 0:  return GamePhase::Transition;
        case 1:  return GamePhase::Menu;
        case 2:
            switch (matchSub) {
                case -1: return GamePhase::MatchIntro;
                case 0:  return GamePhase::MatchGameplay;
                case 5:  return GamePhase::MatchRoundEnd;
                case 1: case 2: case 3: case 4:
                         return GamePhase::MatchOther;
                default: return GamePhase::Unknown;
            }
        case 3:
            switch (scene3Sub) {
                case 1:  return GamePhase::VsList;
                case 2:  return GamePhase::CharSelect;
                default: return GamePhase::Stage3Other;
            }
        case 4:  return GamePhase::OpeningMovie;
        case 255:return GamePhase::Transition;
        default: return GamePhase::Unknown;
    }
}

PhasePolicy SceneSystem::policyFor(GamePhase p) {
    switch (p) {
        case GamePhase::MatchGameplay:
            // This is the only phase where rollback may run.
            return { InputPolicy::FullControl, true, true };
        case GamePhase::MatchIntro:
            return { InputPolicy::NoInput, false, true };
        case GamePhase::MatchOther:
            // matchSub 1..4 are unmapped, so never roll back through them.
            return { InputPolicy::NoInput, false, false };
        case GamePhase::MatchRoundEnd:
            return { InputPolicy::NoInput, false, false };
        case GamePhase::Menu:
        case GamePhase::VsList:
        case GamePhase::CharSelect:
        case GamePhase::Stage3Other:
            return { InputPolicy::MenuInput, false, false };
        case GamePhase::OpeningMovie:
            return { InputPolicy::NoInput, false, false };
        default:
            return { InputPolicy::NoInput, false, false };
    }
}

static const VsRow kVsRows[VsFormationModel::kRowCount] = {
    { 0, VsFormation::TwoFighter,  0x1, 0x2, { 0, 1, 2, 3 }, 2,
      "1P vs CPU: only slot 0 is human; identity route after the transition" },
    { 1, VsFormation::TwoFighter,  0x3, 0x0, { 0, 1, 2, 3 }, 2,
      "1P vs 2P: slots 0 and 1 are human, no CPU" },
    { 2, VsFormation::FourFighter, 0x1, 0xE, { 0, 1, 2, 3 }, 4,
      "2v2 with one human: slot 0 human, slots 1/2/3 CPU" },
    { 3, VsFormation::FourFighter, 0x5, 0xA, { 0, 2, 1, 3 }, 4,
      "2v2 cooperative: slots 0 and 2 are human teammates; NON-IDENTITY route" },
    { 4, VsFormation::FourFighter, 0x3, 0xC, { 0, 1, 2, 3 }, 4,
      "2v2 opposing humans with CPU partners: slots 0 and 1 human" },
    { 5, VsFormation::FourFighter, 0xF, 0x0, { 0, 1, 2, 3 }, 4,
      "four-human 2v2: all four slots human, identity route, no CPU" },
};

const VsRow* VsFormationModel::rows(std::size_t& count) {
    count = kRowCount;
    return kVsRows;
}

const VsRow* VsFormationModel::row(int r) {
    if (r < 0 || static_cast<std::size_t>(r) >= kRowCount)
        return nullptr;
    return &kVsRows[r];
}

VsFormation VsFormationModel::formationOf(int r) {
    const VsRow* d = row(r);
    return d ? d->formation : VsFormation::NotSelected;
}

bool VsFormationModel::isHumanSlot(int r, int slot) {
    const VsRow* d = row(r);
    if (!d || slot < 0 || slot >= kFighterSlots)
        return false;
    return (d->humanSlotMask & (1u << slot)) != 0;
}

bool VsFormationModel::isCpuSlot(int r, int slot) {
    const VsRow* d = row(r);
    if (!d || slot < 0 || slot >= kFighterSlots)
        return false;
    return (d->cpuMask & (1u << slot)) != 0;
}

int VsFormationModel::sourceForSlot(int r, int slot) {
    const VsRow* d = row(r);
    if (!d || slot < 0 || slot >= kFighterSlots)
        return -1;
    return static_cast<int>(d->route[slot]);
}

bool VsFormationModel::hasIdentityRoute(int r) {
    const VsRow* d = row(r);
    if (!d)
        return false;
    for (int slot = 0; slot < kFighterSlots; ++slot)
        if (static_cast<int>(d->route[slot]) != slot)
            return false;
    return true;
}

bool VsFormationModel::launchReady(VsFormation f, const int confirmState[4]) {
    const int required = requiredSelectorConfirms(f);
    if (required == 0 || !confirmState)
        return false;
    for (int i = 0; i < required; ++i)
        if (confirmState[i] != kSelectorDone)
            return false;
    return true;
}

int MatchFlowModel::cpuMaskAfterGameEnd(int formation, int cpuMask,
                                        int loserSlot) {
    if (loserSlot < 0 || loserSlot >= VsFormationModel::kFighterSlots)
        return cpuMask;
    if (formation != 0)
        return cpuMask;
    return cpuMask | (1 << loserSlot);
}

bool MatchFlowModel::postGameHandlerYieldsToSceneDispatcher(int formation) {
    return formation == 1 || formation == 2;
}

MatchFlowModel::RematchState MatchFlowModel::rematchCharacterSelect(
    int vsRow, bool vsListEntryFlag, int cpuMaskBefore) {
    RematchState state;
    state.cpuMask = cpuMaskBefore;
    const VsRow* row = VsFormationModel::row(vsRow);
    if (!vsListEntryFlag || !row) {
        return state;
    }
    state.applied   = true;
    state.formation = static_cast<int>(row->formation);
    state.vsApplied = 1;
    state.cpuMask   = 0;
    state.scene3Sub = 2;
    return state;
}

int MatchFlowModel::launchGateRequiredSelectors(int formation, int vsApplied) {
    if (formation == 2) return 4;
    if (formation == 1) return 2;
    if (formation != 0) return 0;
    return vsApplied ? 2 : 1;
}

bool MatchFlowModel::launchGateFires(int formation, int vsApplied,
                                     const int confirmState[4], int latch) {
    if (!confirmState || latch != kLaunchLatchTarget)
        return false;
    if (formation == 2)
        return confirmState[0] == 4 && confirmState[1] == 4
            && confirmState[2] == 4 && confirmState[3] == 4;
    if (formation == 1)
        return confirmState[0] == 4 && confirmState[1] == 4;
    if (formation != 0)
        return false;
    if (vsApplied)
        return confirmState[0] == 3 && confirmState[1] == 3;
    return confirmState[0] == 3 || confirmState[1] == 3;
}

MatchFlowModel::RoundEnd MatchFlowModel::advanceRound(
    int roundCounter, int winnerRoundWins, int roundsToWinMinusOne) {
    RoundEnd end;
    end.roundCounter = roundCounter + 1;
    if (winnerRoundWins == roundsToWinMinusOne + 1)
        end.roundCounter = kGameOverRoundCounter;
    end.gameOver = end.roundCounter == kGameOverRoundCounter;
    return end;
}

DirectBootController::Step DirectBootController::tick() {
    const Scene sc = scenes_.scene();
    if (sc == Scene::Match) {
        step_ = Step::Launched;
        return step_;
    }
    if (sc == Scene::TitleMenu && scenes_.mainMenuSub() == 2) {
        m_.set<int>(addr::kSceneNext, 3);
        m_.set<int>(addr::kVsListEntryFlag, 1);
        m_.set<int>(addr::kVsFormationRow, 1);
        m_.set<int>(addr::kControlMask, 2);
        step_ = Step::AtMenu;
        return step_;
    }
    if (sc == Scene::VsSetup) {
        const Scene3Sub s3 = scenes_.scene3Sub();
        if (s3 == Scene3Sub::VsList) {
            m_.set<int>(addr::kScene3Cursor, vsRow_);
            m_.set<int>(addr::kScene3ConfirmFlag, 1);
            step_ = Step::AtVsList;
            return step_;
        }
        if (s3 == Scene3Sub::CharSelect) {
            m_.set<int>(addr::kP1CharCursor, 0);
            m_.set<int>(addr::kP2CharCursor, 0);
            if (--confirmCooldown_ <= 0) {
                const auto advanceConfirm = [this](GameAddr address) {
                    const int value = m_.get<int>(address);
                    const int next = value <= 0 ? 1 : (value == 1 ? 2 : 4);
                    if (value < 4)
                        m_.set<int>(address, next);
                };
                advanceConfirm(addr::kCharSelConfirmP1);
                advanceConfirm(addr::kCharSelConfirmP2);
                confirmCooldown_ = 20;
            }
            step_ = Step::AtCharSelect;
            return step_;
        }
    }
    return step_;
}

}
