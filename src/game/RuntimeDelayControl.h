#pragma once

#include <cstdint>

namespace qgame {

constexpr uint8_t kMinimumGameplayDelay = 0;
constexpr uint8_t kMaximumGameplayDelay = 15;
constexpr uint64_t kRuntimeDelayDrainTimeoutMs = 5000;

constexpr uint8_t kMinimumGameplayRollback = 2;
// Must not exceed GameHooks' savestate pool: kPoolSlots = kMaxWindow + 1 = 31 (static-asserted there).
constexpr uint8_t kMaximumGameplayRollback = 30;

constexpr bool runtimeDelayControlsAllowed(bool rollbackGameplay,
                                          uint32_t scene) {
    return rollbackGameplay || scene == 3u;
}

struct RuntimeDelayKeySample {
    bool playerNetplay = false;
    bool focused = false;
    bool rollbackGameplay = false;
    bool minusDown = false;
    bool equalsDown = false;
    bool bracketLeftDown = false;
    bool bracketRightDown = false;
    bool shiftDown = false;
    bool controlDown = false;
    bool altDown = false;
};

class RuntimeDelayHotkeys {
public:
    int poll(const RuntimeDelayKeySample& sample) {
        const bool minusEdge = sample.minusDown && !minusWasDown_;
        const bool equalsEdge = sample.equalsDown && !equalsWasDown_;
        minusWasDown_ = sample.minusDown;
        equalsWasDown_ = sample.equalsDown;

        const bool enabled =
            sample.playerNetplay
            && sample.focused
            && sample.rollbackGameplay
            && !sample.shiftDown
            && !sample.controlDown
            && !sample.altDown;
        if (!enabled || minusEdge == equalsEdge)
            return 0;
        return minusEdge ? -1 : 1;
    }

    int pollRollback(const RuntimeDelayKeySample& sample) {
        const bool leftEdge =
            sample.bracketLeftDown && !bracketLeftWasDown_;
        const bool rightEdge =
            sample.bracketRightDown && !bracketRightWasDown_;
        bracketLeftWasDown_ = sample.bracketLeftDown;
        bracketRightWasDown_ = sample.bracketRightDown;

        const bool enabled =
            sample.playerNetplay
            && sample.focused
            && sample.rollbackGameplay
            && !sample.shiftDown
            && !sample.controlDown
            && !sample.altDown;
        if (!enabled || leftEdge == rightEdge)
            return 0;
        return leftEdge ? -1 : 1;
    }

    void reset() {
        minusWasDown_ = false;
        equalsWasDown_ = false;
        bracketLeftWasDown_ = false;
        bracketRightWasDown_ = false;
    }

private:
    bool minusWasDown_ = false;
    bool equalsWasDown_ = false;
    bool bracketLeftWasDown_ = false;
    bool bracketRightWasDown_ = false;
};

class RuntimeDelayDrain {
public:
    bool request(uint8_t target, uint8_t applied, uint64_t nowMs,
                 bool gameplay = true) {
        if (!gameplay || target == applied) {
            reset();
            return false;
        }
        if (!active_)
            startedMs_ = nowMs;
        active_ = true;
        target_ = target;
        return true;
    }

    bool active() const { return active_; }
    uint8_t target() const { return target_; }
    uint64_t elapsedMs(uint64_t nowMs) const {
        return active_ && nowMs >= startedMs_ ? nowMs - startedMs_ : 0;
    }
    bool timedOut(uint64_t nowMs) const {
        return active_
            && elapsedMs(nowMs) >= kRuntimeDelayDrainTimeoutMs;
    }

    void reset() {
        active_ = false;
        target_ = 0;
        startedMs_ = 0;
    }

private:
    bool active_ = false;
    uint8_t target_ = 0;
    uint64_t startedMs_ = 0;
};

constexpr uint8_t adjustedGameplayRollback(uint8_t current, int direction) {
    return direction < 0
        ? (current > kMinimumGameplayRollback
               ? static_cast<uint8_t>(current - 1u)
               : kMinimumGameplayRollback)
        : direction > 0
            ? (current < kMaximumGameplayRollback
                   ? static_cast<uint8_t>(current + 1u)
                   : kMaximumGameplayRollback)
            : current;
}

constexpr uint8_t adjustedGameplayDelay(uint8_t current, int direction) {
    return direction < 0
        ? (current > kMinimumGameplayDelay
               ? static_cast<uint8_t>(current - 1u)
               : kMinimumGameplayDelay)
        : direction > 0
            ? (current < kMaximumGameplayDelay
                   ? static_cast<uint8_t>(current + 1u)
                   : kMaximumGameplayDelay)
            : current;
}

}
