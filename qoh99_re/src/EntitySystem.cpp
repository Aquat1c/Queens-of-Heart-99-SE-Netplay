#include "qoh99/EntitySystem.hpp"

namespace qoh99 {

static_assert(EntityDescriptor::kStride == 104, "render-entity stride");
static_assert(EntityDescriptor::kTransformOff == 60, "transform at +60");

}
