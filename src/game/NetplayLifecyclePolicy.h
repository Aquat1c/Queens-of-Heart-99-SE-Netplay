#pragma once

#include "../input/InputActions.h"

#include <cstdint>

namespace qgame {

// QOH keeps the round-result sequence inside scene 2 / matchSubstate 0.
constexpr bool lifecycleTransitionRequiresExactInput(
    const uint32_t roundResultState,
    const uint32_t roundCounter,
    const uint32_t stageTimer) {
    if (roundCounter == 5u) return true;
    return roundResultState == 8u && stageTimer <= 1u;
}

constexpr bool rollbackGameplayPolicyActive(
    const bool rollbackEligible,
    const uint32_t roundResultState,
    const uint32_t roundCounter,
    const uint32_t stageTimer) {
    return rollbackEligible
        && !lifecycleTransitionRequiresExactInput(
            roundResultState, roundCounter, stageTimer);
}

constexpr bool inputProductionPermitted(
    const bool rollbackEligible,
    const bool lifecycleExactBarrier,
    const bool promptActive,
    const bool spectatorOrReplay) {
    if (spectatorOrReplay) return false;
    if (!rollbackEligible) return false;
    if (lifecycleExactBarrier) return false;
    if (promptActive) return false;
    return true;
}

constexpr uint32_t filterUnselectedCharacterSelectBack(
    const bool inCharacterSelect,
    const uint32_t localConfirmationState,
    const uint32_t input) {
    return inCharacterSelect && localConfirmationState <= 1u
        ? input & ~qinput::kLogicalB
        : input;
}

constexpr bool onlineReturnToCharacterSelectEligible(
    const uint32_t scene) {
    return scene == 2u;
}

class NetworkDirectBootInputGate {
public:
    uint32_t filter(
        const bool directBootActive,
        const bool productionPhysicalInput,
        const uint32_t input) {
        if (directBootActive) {
            awaitingNeutral_ = true;
            return 0u;
        }
        if (!productionPhysicalInput) {
            awaitingNeutral_ = false;
            return input;
        }
        if (awaitingNeutral_) {
            if (input == 0u)
                awaitingNeutral_ = false;
            return 0u;
        }
        return input;
    }

    void reset() {
        awaitingNeutral_ = false;
    }

    bool awaitingNeutral() const {
        return awaitingNeutral_;
    }

private:
    bool awaitingNeutral_ = false;
};

class WinscreenCarryGate {
public:
    uint32_t filter(const bool postGameplay,
                    const bool frontend,
                    const uint32_t input) {
        if (!postGameplay && !frontend) {
            armed_ = false;
            return input;
        }
        if (armed_) {
            if (input == 0u) armed_ = false;
            return 0u;
        }
        if (postGameplay && input != 0u) armed_ = true;
        return input;
    }

    void reset() { armed_ = false; }
    bool armed() const { return armed_; }

private:
    bool armed_ = false;
};

}
