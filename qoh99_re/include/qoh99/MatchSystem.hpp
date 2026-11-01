#pragma once
#include "AddressMap.hpp"
#include "MemoryView.hpp"
#include "SceneSystem.hpp"

namespace qoh99 {

class RoundState {
public:
    explicit RoundState(MemoryView& m) : m_(m) {}
    int roundCounter() const { return m_.get<int>(addr::kRoundCounter); }
    int winsOf(int player) const {
        return m_.get<int>(addr::kRoundWinTable + 4u * static_cast<GameAddr>(player));
    }
    int roundsToWin() const {
        GameAddr cfg = m_.get<GameAddr>(addr::kMatchConfigPtr);
        return cfg ? m_.get<int>(cfg + 48) : 0;
    }
private:
    MemoryView& m_;
};

class HitstopState {
public:
    static constexpr Confidence kConfidence = Confidence::Unknown;
};

class DamageSystemModel {
public:
    explicit DamageSystemModel(MemoryView& m) : m_(m) {}
    // sub_42D740: the strict-negative result is QOH's native KO predicate.
    int health(int player) const {
        const GameAddr base = addr::kBattleLifeArray
            + addr::kBattleLifeStride * static_cast<GameAddr>(player);
        return m_.get<int>(base)
            + m_.get<int>(base + 8)
            - m_.get<int>(base + 4);
    }
    int guardGauge(int player) const {
        return m_.get<int>(addr::kGuardGaugeArray
            + addr::kGuardGaugeStride * static_cast<GameAddr>(player));
    }
    int score(int player) const {
        return m_.get<int>(addr::kScoreArray + 4u * static_cast<GameAddr>(player));
    }
private:
    MemoryView& m_;
};

class CollisionSystemModel {
public:
    static constexpr Confidence kConfidence = Confidence::Unknown;
};

class GameplayTick {
public:
    static constexpr GameAddr kDispatchVa = fnaddr::kSceneMatchDispatch;
    static constexpr GameAddr kGameplayVa = fnaddr::kGameplayTick;
};

class MatchSystem {
public:
    explicit MatchSystem(MemoryView& m)
        : m_(m), scenes_(m), round_(m), damage_(m) {}

    SceneSystem& scenes() { return scenes_; }
    RoundState& round() { return round_; }
    DamageSystemModel& damage() { return damage_; }

    bool inGameplay() const { return scenes_.inGameplay(); }

    // The matchSub 0->x transition is not re-enterable: re-simulating the frame that flips 0->5 crashes the winscreen.
    static bool exitsGameplay(MatchSub before, MatchSub after) {
        return before == MatchSub::Gameplay && after != MatchSub::Gameplay;
    }

private:
    MemoryView& m_;
    SceneSystem scenes_;
    RoundState round_;
    DamageSystemModel damage_;
};

}
