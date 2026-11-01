#pragma once

#include <cstdint>

namespace qreplay {

struct ReplayNavigation {
    struct Slot {
        uint32_t hiddenTicks = 0u;
        bool present = false;
    };

    bool active = false;
    bool restorePending = false;
    uint32_t target = 0u;

    void reset(bool autoStart) {
        active = !autoStart;
        restorePending = false;
        target = 0u;
    }

    bool request(uint32_t requested, uint32_t total) {
        if (requested >= total) return false;
        active = true;
        restorePending = true;
        target = requested;
        return true;
    }

    void restored() { restorePending = false; }

    bool committed(uint32_t nextRecord) {
        if (!active || restorePending || nextRecord <= target) return false;
        active = false;
        return true;
    }

    Slot plan(uint32_t nextRecord) const {
        if (!active || restorePending || nextRecord > target) return {};
        const uint32_t remaining = target - nextRecord;
        return { remaining < 16u ? remaining : 16u, remaining < 16u };
    }

    static uint32_t previousFrame(uint32_t nextRecord) {
        return nextRecord > 1u ? nextRecord - 2u : 0u;
    }
};

}
