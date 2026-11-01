#include "ToySimulation.h"

#include <algorithm>

namespace qnetplay::soak {

namespace {

constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x100000001B3ull;

void hashU32(uint64_t& hash, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        hash ^= static_cast<uint8_t>(value >> shift);
        hash *= kFnvPrime;
    }
}

uint64_t splitMix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31u);
}

uint32_t distance(const uint32_t left, const uint32_t right) {
    return left > right ? left - right : right - left;
}

}

bool ToyState::operator==(const ToyState& other) const {
    return frame == other.frame
        && rng == other.rng
        && position == other.position
        && velocity == other.velocity
        && health == other.health
        && meter == other.meter
        && combo == other.combo
        && arena == other.arena;
}

ToySimulation::ToySimulation(const uint32_t seed) {
    reset(seed);
}

void ToySimulation::reset(const uint32_t seed) {
    _state = {};
    _state.rng = seed != 0u ? seed : 1u;
    _state.position = {900u, 3100u};
    _state.health = {10000u, 10000u};
    for (size_t index = 0u; index < _state.arena.size(); ++index) {
        _state.arena[index] =
            nextRng(_state.rng + static_cast<uint32_t>(index * 17u));
    }
}

uint32_t ToySimulation::nextRng(uint32_t value) {
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    return value != 0u ? value : 0xA341316Cu;
}

void ToySimulation::applyMovement(ToyState& state,
                                  const size_t player,
                                  const uint16_t input) {
    uint32_t velocity = state.velocity[player];
    if ((input & 0x0001u) != 0u) {
        velocity = velocity > 2u ? velocity - 2u : 0u;
    }
    if ((input & 0x0002u) != 0u) {
        velocity = std::min<uint32_t>(velocity + 2u, 18u);
    }
    if ((input & 0x0004u) != 0u) {
        velocity = std::min<uint32_t>(velocity + 1u, 18u);
    }
    if ((input & 0x0008u) != 0u) {
        velocity /= 2u;
    }

    const bool movesLeft = (input & 0x0001u) != 0u
        && (input & 0x0002u) == 0u;
    if (movesLeft) {
        state.position[player] =
            state.position[player] > velocity
            ? state.position[player] - velocity
            : 0u;
    } else {
        state.position[player] =
            std::min<uint32_t>(4095u,
                               state.position[player] + velocity);
    }
    state.velocity[player] = velocity;
}

void ToySimulation::step(const uint16_t p1, const uint16_t p2) {
    const std::array<uint16_t, 2> inputs{p1, p2};
    applyMovement(_state, 0u, p1);
    applyMovement(_state, 1u, p2);

    _state.rng = nextRng(_state.rng
        ^ (static_cast<uint32_t>(p1) << 16u)
        ^ static_cast<uint32_t>(p2)
        ^ (_state.frame * 0x9E3779B9u));

    for (size_t attacker = 0u; attacker < 2u; ++attacker) {
        const size_t defender = attacker ^ 1u;
        const uint16_t input = inputs[attacker];
        const bool attack = (input & 0x0030u) != 0u;
        const uint32_t reach = 180u + ((input >> 6u) & 0x3u) * 40u;
        if (attack
            && distance(_state.position[attacker],
                        _state.position[defender]) <= reach) {
            const uint32_t damage =
                7u + ((_state.rng >> (attacker * 7u)) & 0x1Fu)
                + (_state.combo[attacker] & 7u);
            _state.health[defender] =
                _state.health[defender] > damage
                ? _state.health[defender] - damage
                : 0u;
            ++_state.combo[attacker];
            _state.combo[defender] = 0u;
            _state.meter[attacker] =
                std::min<uint32_t>(9999u,
                    _state.meter[attacker] + damage * 3u);
        } else {
            _state.combo[attacker] = 0u;
            _state.meter[attacker] +=
                static_cast<uint32_t>((input >> 8u) & 0x3u);
            _state.meter[attacker] %= 10000u;
        }
    }

    const size_t arenaIndex = static_cast<size_t>(
        (_state.frame + (_state.rng >> 24u)) % _state.arena.size());
    _state.arena[arenaIndex] = nextRng(
        _state.arena[arenaIndex]
        + _state.rng
        + static_cast<uint32_t>(p1)
        + (static_cast<uint32_t>(p2) << 1u)
        + _state.health[0]
        + (_state.health[1] << 3u));
    ++_state.frame;
}

uint64_t ToySimulation::hash() const {
    uint64_t hash = kFnvOffset;
    hashU32(hash, _state.frame);
    hashU32(hash, _state.rng);
    for (const uint32_t value : _state.position) hashU32(hash, value);
    for (const uint32_t value : _state.velocity) hashU32(hash, value);
    for (const uint32_t value : _state.health) hashU32(hash, value);
    for (const uint32_t value : _state.meter) hashU32(hash, value);
    for (const uint32_t value : _state.combo) hashU32(hash, value);
    for (const uint32_t value : _state.arena) hashU32(hash, value);
    return hash;
}

uint64_t deterministicWord(const uint64_t inputSeed,
                           const uint8_t canonicalPlayer,
                           const uint32_t sourceFrame) {
    return splitMix64(inputSeed
        ^ (static_cast<uint64_t>(canonicalPlayer + 1u)
            * 0xD1B54A32D192ED03ull)
        ^ (static_cast<uint64_t>(sourceFrame)
            * 0x9E3779B97F4A7C15ull));
}

uint16_t delayedScriptedInput(const uint64_t inputSeed,
                              const uint8_t canonicalPlayer,
                              const uint32_t inputFrame,
                              const uint8_t localDelay,
                              const uint32_t firstFrame,
                              const uint16_t neutralInput) {
    const uint32_t elapsed = inputFrame - firstFrame;
    if (elapsed < static_cast<uint32_t>(localDelay)) {
        return neutralInput;
    }
    const uint32_t sourceFrame =
        inputFrame - static_cast<uint32_t>(localDelay);
    const uint64_t word =
        deterministicWord(inputSeed, canonicalPlayer, sourceFrame);

    uint16_t input = static_cast<uint16_t>(
        (word & 0x000Fu)
        | ((word & 0x00F0u) << 4));
    if ((input & 0x0C00u) == 0x0C00u)
        input &= static_cast<uint16_t>(~0x0400u);
    if ((input & 0x0300u) == 0x0300u)
        input &= static_cast<uint16_t>(~0x0100u);
    if (((sourceFrame / 47u) + canonicalPlayer) % 5u == 0u)
        input ^= 0x0001u;
    return input;
}

}
