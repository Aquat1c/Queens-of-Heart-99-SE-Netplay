#include "qoh99/TimingSystem.hpp"

namespace qoh99 {

static_assert(TimingSystem::kMsgPumpVa == 0x491760,
              "message pump VA (hooking it crashes — keep the warning honest)");

}
