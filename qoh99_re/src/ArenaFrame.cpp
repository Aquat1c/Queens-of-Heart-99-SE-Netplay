#include "qoh99/ArenaFrame.hpp"

namespace qoh99 {

static_assert(ArenaFrame::kBlock1 == ArenaFrame::kBlock0 + ArenaFrame::kBlockSize,
              "block1 = block0 + 0x1CED0 (118480)");
static_assert(ArenaFrame::kBlockSize == 118480u, "0x1CED0 = 118480");
static_assert(arena_field::kEntityArray + arena_field::kEntityStride <
              ArenaFrame::kBlockSize, "entity array inside the block");
static_assert(arena_field::kEntityCount < ArenaFrame::kBlockSize,
              "count field inside the block");
static_assert(arena_field::kEntityTransform - arena_field::kEntityArray == 60,
              "per-entity transform offset");

}
