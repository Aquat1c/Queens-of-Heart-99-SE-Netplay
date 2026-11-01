#include "qoh99/RngSystem.hpp"

namespace qoh99 {

static_assert(RngSystem::kMul == 156683941u, "LCG multiplier from sub_4914D0");
static_assert(RngSystem::kInc == 1u, "LCG increment from sub_4914D0");

static_assert(RngSystem::step(0) == 1u, "step(0)");
static_assert(RngSystem::step(1) == 156683942u, "step(1)");
static_assert(RngSystem::drawValue(RngSystem::step(0)) == 0u, "draw after step(0)");

}
