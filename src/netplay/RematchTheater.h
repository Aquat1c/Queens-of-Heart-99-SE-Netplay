#pragma once

#include <cstdint>

#include "input/InputActions.h"

namespace qnetplay {

struct RematchTheaterObserved {
    uint32_t scene = 0;            // 0x6234EC current scene (2 = match)
    uint32_t sceneNext = 0;        // 0x61801C requested scene
    uint32_t formationRow = 0;     // 0x62369C applied VS formation row
    int32_t matchSub = 0;          // 0x8719A4 scene-2 substate
    uint32_t formationActive = 0;  // 0x6236A0 formation-alive flag
    uint32_t loserSide = 0;        // 0x6236A8 native winner-screen verdict
    uint32_t vacancyMask = 0;      // 0x6236CC vacated/COM slot mask
    uint32_t continueOwner = 0;    // 0x6236C4 continue owner (-1 = none)
    uint32_t continueTimer = 0;    // 0x6236C8 postgame timer
    // 0x6236D4: scene-2's committed copy of the CPU slot mask, bit n = slot n is engine-driven.
    uint32_t committedCpuMask = 0;
};

constexpr uint32_t kRematchTheaterNoOwner = 0xFFFFFFFFu;

inline bool rematchTheaterHumanVersusHuman(const RematchTheaterObserved& o) {
    return o.formationActive == 1u && o.committedCpuMask == 0u;
}

inline bool rematchTheaterDoorwayEligible(const RematchTheaterObserved& o) {
    return o.scene == 2u
        && (o.sceneNext == 3u || o.sceneNext == 2u)
        && (o.formationRow == 1u || o.formationRow == 2u)
        && o.matchSub == 5
        && o.formationActive == 1u
        && o.loserSide <= 1u
        && o.vacancyMask == 0u
        && o.continueOwner == kRematchTheaterNoOwner
        && o.continueTimer > 60u
        && rematchTheaterHumanVersusHuman(o);
}

struct RematchTheaterFlipPlan {
    uint32_t formationRow = 0;
    int32_t matchSub = 0;
    uint32_t sceneNext = 0;
    uint32_t vacancyMask = 0;
};

inline RematchTheaterFlipPlan rematchTheaterFlipPlan(
    const RematchTheaterObserved& o) {
    RematchTheaterFlipPlan plan{};
    plan.formationRow = 0u;
    plan.matchSub = 2;
    plan.sceneNext = 2u;
    plan.vacancyMask = o.vacancyMask | (1u << (o.loserSide & 1u));
    return plan;
}

inline bool rematchTheaterFlippedStateSane(const RematchTheaterObserved& o,
                                           uint32_t flippedLoserSide) {
    return o.scene == 2u
        && o.sceneNext == 2u
        && o.formationRow == 0u
        && o.matchSub == 2
        && o.formationActive == 1u
        && o.continueOwner == kRematchTheaterNoOwner
        && ((o.vacancyMask >> (flippedLoserSide & 1u)) & 1u) == 1u
        && o.continueTimer > 60u;
}

inline uint32_t rematchTheaterLocalFeed(uint32_t sampledLogicalMask,
                                        bool locallyReady) {
    if (locallyReady) return 0u;
    return sampledLogicalMask & qinput::kLogicalDirectionMask;
}

// dword_8A5450: 0 = hai (YES, the default), 1 = iie (NO).
inline bool rematchTheaterCursorIsYes(uint32_t promptCursor) {
    return promptCursor == 0u;
}

inline bool rematchTheaterFallbackCursorToggles(uint32_t previousMask,
                                                uint32_t currentMask) {
    const uint32_t rising = currentMask & ~previousMask;
    const bool left = (rising & qinput::kLogicalLeft) != 0u;
    const bool right = (rising & qinput::kLogicalRight) != 0u;
    return left != right;
}

inline bool rematchTheaterConfirmEdge(uint32_t previousMask,
                                      uint32_t currentMask) {
    return (previousMask & qinput::kLogicalButtonMask) == 0u
        && (currentMask & qinput::kLogicalButtonMask) != 0u;
}

}
