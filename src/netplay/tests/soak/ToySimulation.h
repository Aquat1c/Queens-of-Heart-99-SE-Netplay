#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace qnetplay::soak {

struct ToyState {
    uint32_t frame = 0;
    uint32_t rng = 0;
    std::array<uint32_t, 2> position{};
    std::array<uint32_t, 2> velocity{};
    std::array<uint32_t, 2> health{};
    std::array<uint32_t, 2> meter{};
    std::array<uint32_t, 2> combo{};
    std::array<uint32_t, 32> arena{};

    bool operator==(const ToyState& other) const;
    bool operator!=(const ToyState& other) const {
        return !(*this == other);
    }
};

class ToySimulation {
public:
    explicit ToySimulation(uint32_t seed = 0x514F4839u);

    void reset(uint32_t seed = 0x514F4839u);
    void restore(const ToyState& state) { _state = state; }
    const ToyState& state() const { return _state; }

    void step(uint16_t p1, uint16_t p2);
    uint64_t hash() const;

private:
    ToyState _state{};

    static uint32_t nextRng(uint32_t value);
    static void applyMovement(ToyState& state,
                              size_t player,
                              uint16_t input);
};

uint64_t deterministicWord(uint64_t inputSeed,
                           uint8_t canonicalPlayer,
                           uint32_t sourceFrame);

uint16_t delayedScriptedInput(uint64_t inputSeed,
                              uint8_t canonicalPlayer,
                              uint32_t inputFrame,
                              uint8_t localDelay,
                              uint32_t firstFrame = 0,
                              uint16_t neutralInput = 0);

}
