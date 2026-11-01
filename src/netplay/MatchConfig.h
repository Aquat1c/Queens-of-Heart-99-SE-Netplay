#pragma once

#include <cstdint>

namespace qnetplay {

constexpr uint32_t kMaxTimerMode = 2u;
constexpr uint32_t kMaxAiDifficulty = 4u;
constexpr uint32_t kSupportedLifeMultiplier = 0u;
constexpr uint32_t kMaxRoundsToWinMinusOne = 2u;

constexpr bool isSupportedQohMatchConfig(
    uint32_t timerMode,
    uint32_t aiDifficulty,
    uint32_t lifeMultiplier,
    uint32_t roundsToWinMinusOne) {
    return timerMode <= kMaxTimerMode
        && aiDifficulty <= kMaxAiDifficulty
        && lifeMultiplier == kSupportedLifeMultiplier
        && roundsToWinMinusOne <= kMaxRoundsToWinMinusOne;
}

}
