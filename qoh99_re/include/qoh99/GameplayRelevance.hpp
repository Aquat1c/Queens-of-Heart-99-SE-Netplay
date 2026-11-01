#pragma once
#include <cstddef>
#include <cstdint>
#include "Confidence.hpp"

namespace qoh99 {

enum class GameplayRelevance {
    DeterministicGameplay,
    RenderCosmetic,
    WallClockPacing,
    ExternalResource,
    Unknown
};

inline const char* toString(GameplayRelevance r) {
    switch (r) {
        case GameplayRelevance::DeterministicGameplay: return "DeterministicGameplay";
        case GameplayRelevance::RenderCosmetic:        return "RenderCosmetic";
        case GameplayRelevance::WallClockPacing:       return "WallClockPacing";
        case GameplayRelevance::ExternalResource:      return "ExternalResource";
        default:                                       return "Unknown";
    }
}

struct FieldInfo {
    const char*  name;
    std::uintptr_t address;
    std::size_t  size;
    Confidence   confidence;
    GameplayRelevance relevance;
    const char*  evidence;
};

}
