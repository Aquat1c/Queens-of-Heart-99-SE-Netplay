#include "qoh99/HudSystem.hpp"

namespace qoh99 {

static_assert(addr::kHudTimerDigits >= 0x8A5C00 && addr::kHudTimerDigits < 0x8A5E00,
              "timer digits inside hud_counters region");
static_assert(addr::kHudComboDigits >= 0x8A5C00 && addr::kHudComboDigits < 0x8A5E00,
              "combo digits inside hud_counters region");
static_assert(addr::kHudDisplayA >= 0x87D000 && addr::kHudDisplayA < 0x881000,
              "hudDisplayA inside split hud_display band");

}
