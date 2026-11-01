#pragma once
#include "AddressMap.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

class RngSystem {
public:
    explicit RngSystem(MemoryView& m) : m_(m) {}

    static constexpr std::uint32_t kMul = 156683941u;
    static constexpr std::uint32_t kInc = 1u;

    static constexpr std::uint32_t step(std::uint32_t seed) { return seed * kMul + kInc; }
    static constexpr std::uint32_t drawValue(std::uint32_t steppedSeed) {
        return (steppedSeed >> 15) & 0x7FFF;
    }

    std::uint32_t gameplaySeed() const { return m_.get<std::uint32_t>(addr::kRngSeedGameplay); }
    std::uint32_t cosmeticSeed() const { return m_.get<std::uint32_t>(addr::kRngSeedCosmetic); }
    std::uint32_t selector() const     { return m_.get<std::uint32_t>(addr::kRngSelector); }

    std::uint32_t predictNextDraw() const {
        std::uint32_t s = selector() ? cosmeticSeed() : gameplaySeed();
        return drawValue(step(s));
    }

    bool srandBoth(std::uint32_t seed) {
        return m_.set<std::uint32_t>(addr::kRngSeedGameplay, seed) &&
               m_.set<std::uint32_t>(addr::kRngSeedCosmetic, seed);
    }

    struct Snapshot { std::uint32_t gameplay, cosmetic, selector; };
    Snapshot save() const { return { gameplaySeed(), cosmeticSeed(), selector() }; }
    bool restore(const Snapshot& s) {
        return m_.set(addr::kRngSeedGameplay, s.gameplay) &&
               m_.set(addr::kRngSeedCosmetic, s.cosmetic) &&
               m_.set(addr::kRngSelector, s.selector);
    }

private:
    MemoryView& m_;
};

}
