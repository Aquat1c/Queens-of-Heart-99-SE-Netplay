#pragma once
#include <cstdint>
#include "qoh99_registry.h"

namespace qoh99 {

enum class QohPhase {
    Unknown, ProcessBoot, MainMenu, ModeSelect, CharacterSelect, StageSelect,
    Loading, CharaIntro, PreRoundMovementOnly, RoundActive, RoundHitstopOrFreeze,
    RoundUnknownSub, RoundKO, RoundTransition, MatchOver, RetryMenu, ReplayOrDemo, Exiting
};

enum class QohNetplayState {
    PreInitial, Initial, MenuNavigation, CharacterSelect, StageSelect, Loading,
    CharaIntro, InGame, Skippable, RetryMenu, ReplayMenu
};

inline const char* phaseName(QohPhase p) {
    switch (p) {
        case QohPhase::ProcessBoot:          return "ProcessBoot";
        case QohPhase::MainMenu:             return "MainMenu";
        case QohPhase::ModeSelect:           return "ModeSelect";
        case QohPhase::CharacterSelect:      return "CharacterSelect";
        case QohPhase::StageSelect:          return "StageSelect";
        case QohPhase::Loading:              return "Loading";
        case QohPhase::CharaIntro:           return "CharaIntro";
        case QohPhase::PreRoundMovementOnly: return "PreRoundMovementOnly";
        case QohPhase::RoundActive:          return "RoundActive";
        case QohPhase::RoundHitstopOrFreeze: return "RoundHitstopOrFreeze";
        case QohPhase::RoundUnknownSub:      return "RoundUnknownSub";
        case QohPhase::RoundKO:              return "RoundKO";
        case QohPhase::RoundTransition:      return "RoundTransition";
        case QohPhase::MatchOver:            return "MatchOver";
        case QohPhase::RetryMenu:            return "RetryMenu";
        case QohPhase::ReplayOrDemo:         return "ReplayOrDemo";
        case QohPhase::Exiting:              return "Exiting";
        default:                             return "Unknown";
    }
}

struct QohStateSnapshot {
    int scene;
    int matchSub;
    int s3sub;
    int mainMenuSub;
};

inline QohPhase detectPhase(const QohStateSnapshot& s) {
    switch (s.scene) {
        case 1:
            return (s.mainMenuSub == 2) ? QohPhase::MainMenu : QohPhase::ProcessBoot;
        case 3:
            if (s.s3sub == 1) return QohPhase::ModeSelect;
            if (s.s3sub == 2) return QohPhase::CharacterSelect;
            return QohPhase::Loading;
        case 2:
            if (s.matchSub == -1) return QohPhase::CharaIntro;
            if (s.matchSub == 0)  return QohPhase::RoundActive;
            if (s.matchSub == 5)  return QohPhase::RoundKO;
            if (s.matchSub >= 1 && s.matchSub <= 4)
                return QohPhase::RoundUnknownSub;
            return QohPhase::Unknown;
        case 4:  return QohPhase::ReplayOrDemo;
        case 255: return QohPhase::Exiting;
        default:  return QohPhase::Unknown;
    }
}

inline QohNetplayState toNetplayState(QohPhase p) {
    switch (p) {
        case QohPhase::ProcessBoot:          return QohNetplayState::Initial;
        case QohPhase::MainMenu:
        case QohPhase::ModeSelect:           return QohNetplayState::MenuNavigation;
        case QohPhase::CharacterSelect:      return QohNetplayState::CharacterSelect;
        case QohPhase::StageSelect:          return QohNetplayState::StageSelect;
        case QohPhase::Loading:              return QohNetplayState::Loading;
        case QohPhase::CharaIntro:           return QohNetplayState::CharaIntro;
        case QohPhase::PreRoundMovementOnly:
        case QohPhase::RoundActive:
        case QohPhase::RoundHitstopOrFreeze: return QohNetplayState::InGame;
        case QohPhase::RoundUnknownSub:
        case QohPhase::RoundKO:
        case QohPhase::RoundTransition:
        case QohPhase::MatchOver:            return QohNetplayState::Skippable;
        case QohPhase::RetryMenu:            return QohNetplayState::RetryMenu;
        case QohPhase::ReplayOrDemo:         return QohNetplayState::ReplayMenu;
        default:                             return QohNetplayState::PreInitial;
    }
}

enum class InputSyncPolicy {
    LocalOnly, BlockingLockstep, InputDelayNoRollback, Rollback, NoInput,
    SkipSafeOnly, UnknownNoRollback
};

enum class InputMaskPolicy { None, DirectionsOnly, FullGameplay, SystemOnly, Unknown };

struct PhasePolicy {
    InputSyncPolicy sync;
    InputMaskPolicy mask;
    bool rollbackAllowed;
    bool rngMustSync;
    bool remoteMayStall;
};

inline PhasePolicy policyFor(QohPhase p) {
    switch (p) {
        case QohPhase::MainMenu:
        case QohPhase::ModeSelect:           return { InputSyncPolicy::LocalOnly,         InputMaskPolicy::Unknown,       false, false, true  };
        case QohPhase::CharacterSelect:
        case QohPhase::StageSelect:          return { InputSyncPolicy::BlockingLockstep,  InputMaskPolicy::Unknown,       false, false, true  };
        case QohPhase::Loading:              return { InputSyncPolicy::NoInput,           InputMaskPolicy::None,          false, true,  true  };
        case QohPhase::CharaIntro:           return { InputSyncPolicy::NoInput,           InputMaskPolicy::None,          false, true,  false };
        case QohPhase::PreRoundMovementOnly: return { InputSyncPolicy::BlockingLockstep,  InputMaskPolicy::DirectionsOnly,false, true,  false };
        case QohPhase::RoundActive:
        case QohPhase::RoundHitstopOrFreeze: return { InputSyncPolicy::Rollback,          InputMaskPolicy::FullGameplay,  true,  true,  false };
        case QohPhase::RoundUnknownSub:      return { InputSyncPolicy::UnknownNoRollback, InputMaskPolicy::Unknown,       false, true,  false };
        case QohPhase::RoundKO:
        case QohPhase::RoundTransition:
        case QohPhase::MatchOver:            return { InputSyncPolicy::SkipSafeOnly,      InputMaskPolicy::SystemOnly,    false, false, false };
        case QohPhase::RetryMenu:            return { InputSyncPolicy::SkipSafeOnly,      InputMaskPolicy::Unknown,       false, false, true  };
        default:                             return { InputSyncPolicy::UnknownNoRollback, InputMaskPolicy::None,          false, false, true  };
    }
}

inline QohStateSnapshot readLiveSnapshot() {
    auto rd = [](uintptr_t a){ return *reinterpret_cast<volatile int32_t*>(a); };
    return { rd(0x6234EC), rd(0x8719A4), rd(0x4AA454), rd(0x4AB8C4) };
}

// Per-fighter control-lock (fighter+1400): sub_47BF90 records zero input while nonzero.
inline int readControlLock(uintptr_t fighterBase) {
    return *reinterpret_cast<volatile int32_t*>(fighterBase + 1400);
}
inline bool inputRecordedForFighter(uintptr_t fighterBase) {
    return readControlLock(fighterBase) == 0;
}

}
