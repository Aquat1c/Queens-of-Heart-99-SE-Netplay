#pragma once
#include <cstdint>

namespace qoh99 {

enum class ProbeMode : uint8_t {
    None            = 0,
    InputMap        = 1,
    FighterWrites   = 2,
    FighterPointers = 3,
    FighterSize     = 4,
    Intro           = 5,
    Blocks          = 6,
    Timer           = 7,
    Rng             = 8,
    Helpers         = 9,
    All             = 255
};

void probeInit(ProbeMode mode, const char* sessionDir);

void probeFrameTick();

bool probeActive();

}
