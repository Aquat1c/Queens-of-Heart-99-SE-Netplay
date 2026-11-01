#pragma once

#include <cstdint>

namespace qgame {

constexpr uint8_t frontendDelayForRttMicros(
    const uint32_t rttMicros,
    const uint8_t gameplayDelay,
    const uint8_t maximumDelay = 15u) {
    const uint64_t scaled = static_cast<uint64_t>(rttMicros) * 606ull;
    const uint32_t oneWayFrames =
        static_cast<uint32_t>((scaled + 19999999ull) / 20000000ull);
    const uint32_t safetyFrames = oneWayFrames >= 6u ? 2u : 1u;
    const uint32_t measured = oneWayFrames + safetyFrames;
    const uint32_t atLeastGameplay =
        measured > gameplayDelay ? measured : gameplayDelay;
    return static_cast<uint8_t>(
        atLeastGameplay > maximumDelay ? maximumDelay : atLeastGameplay);
}

constexpr uint8_t frontendDelayForRtt(
    const uint32_t rttMs,
    const uint8_t gameplayDelay,
    const uint8_t maximumDelay = 15u) {
    return frontendDelayForRttMicros(rttMs * 1000u, gameplayDelay,
                                     maximumDelay);
}

static_assert(frontendDelayForRttMicros(0u, 0u) == 1u, "zero RTT");
static_assert(frontendDelayForRttMicros(400u, 0u) == 2u,
              "0.4 ms LAN: one covered frame + one safety frame");
static_assert(frontendDelayForRttMicros(400u, 0u)
                  == frontendDelayForRttMicros(900u, 0u),
              "sub-millisecond jitter must not move the frontend delay");
static_assert(frontendDelayForRttMicros(47000u, 0u) == 3u,
              "a REAL 47 ms RTT still buys its two covered frames");
static_assert(frontendDelayForRttMicros(33000u, 0u) == 2u, "33 ms RTT");
static_assert(frontendDelayForRtt(33u, 0u) == 2u, "ms wrapper agrees");
static_assert(frontendDelayForRttMicros(198000u, 0u) == 8u, "198 ms RTT");

struct FrontendDelayLatch {
    uint32_t frames = 1u;
    int64_t riseDeadline = 0;
    int64_t decayDeadline = 0;

    constexpr uint32_t step(const bool windowUsable, const uint32_t target,
                            const uint32_t baseline, const int64_t now,
                            const int64_t riseInterval,
                            const int64_t decayInterval) {
        if (!windowUsable) {
            if (frames < baseline) frames = baseline;
            riseDeadline = 0;
            decayDeadline = 0;
            return frames;
        }
        if (target > frames) {
            if (riseDeadline == 0) {
                riseDeadline = now + riseInterval;
            } else if (now >= riseDeadline) {
                ++frames;
                riseDeadline = now + riseInterval;
            }
            decayDeadline = now + decayInterval;
            return frames;
        }
        riseDeadline = 0;
        if (target == frames) {
            decayDeadline = now + decayInterval;
            return frames;
        }
        if (decayDeadline == 0) {
            decayDeadline = now + decayInterval;
        } else if (now >= decayDeadline) {
            const uint32_t floor = target > 1u ? target : 1u;
            if (frames > floor) --frames;
            decayDeadline = now + decayInterval;
        }
        return frames;
    }
};

}
