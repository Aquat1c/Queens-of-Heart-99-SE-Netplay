#pragma once
#include "ArenaFrame.hpp"

namespace qoh99 {

struct EntityDescriptor {
    static constexpr std::uint32_t kStride = arena_field::kEntityStride;
    static constexpr std::uint32_t kTransformOff =
        arena_field::kEntityTransform - arena_field::kEntityArray;
    GameAddr base;
};

class RenderEntityList {
public:
    explicit RenderEntityList(MemoryView& m) : m_(m), arena_(m) {}

    int count() const { return arena_.entityCount(); }
    EntityDescriptor at(int i) const { return { arena_.entityAt(i) }; }
    double transformOf(int i) const {
        return m_.get<double>(arena_.entityAt(i) + EntityDescriptor::kTransformOff);
    }

private:
    MemoryView& m_;
    ArenaFrame arena_;
};

class EntitySystem {
public:
    explicit EntitySystem(MemoryView& m) : list_(m) {}
    RenderEntityList& list() { return list_; }
private:
    RenderEntityList list_;
};

}
