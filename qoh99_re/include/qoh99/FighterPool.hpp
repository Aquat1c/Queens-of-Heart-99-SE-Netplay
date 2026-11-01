#pragma once
#include "Fighter.hpp"

namespace qoh99 {

class FighterSlotTable {
public:
    explicit FighterSlotTable(MemoryView& m) : m_(m) {}

    GameAddr slotPtr(int slot) const {
        return m_.get<GameAddr>(addr::kFighterPtrTable + 4 * static_cast<GameAddr>(slot));
    }
    Fighter fighter(int slot) { return Fighter(m_, slotPtr(slot)); }

    static GameAddr staticFighter(int player) {
        switch (player) {
            case 0: return addr::kFighterP1;
            case 1: return addr::kFighterP2;
            case 2: return addr::kFighterP3;
            case 3: return addr::kFighterP4;
            default: return kNullAddr;
        }
    }
    static GameAddr helperClone(int player) {
        switch (player) {
            case 0: return addr::kHelperCloneP1;
            case 1: return addr::kHelperCloneP2;
            case 2: return addr::kHelperCloneP3;
            case 3: return addr::kHelperCloneP4;
            default: return kNullAddr;
        }
    }

    bool validate1v1() const {
        return slotPtr(0) == addr::kFighterP1 && slotPtr(1) == addr::kFighterP2;
    }

private:
    MemoryView& m_;
};

class ActorPool {
public:
    ActorPool(MemoryView& m, GameAddr base, int count)
        : m_(m), base_(base), count_(count) {}

    int count() const { return count_; }
    GameAddr objectAt(int i) const {
        return base_ + static_cast<GameAddr>(i) * fighter_field::kSize;
    }
    Fighter object(int i) { return Fighter(m_, objectAt(i)); }
    GameAddr begin() const { return base_; }
    GameAddr end() const { return objectAt(count_); }

    static ActorPool effectPool(MemoryView& m) { return ActorPool(m, addr::kEffectPool, 30); }
    static ActorPool clonePool(MemoryView& m)  { return ActorPool(m, addr::kActorClonePool, 120); }
    static ActorPool shadowPool(MemoryView& m) { return ActorPool(m, addr::kShadowPool, 16); }

private:
    MemoryView& m_;
    GameAddr base_;
    int count_;
};

}
