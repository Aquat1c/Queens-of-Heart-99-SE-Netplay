#pragma once
#include "AddressMap.hpp"
#include "GameplayRelevance.hpp"
#include "Types.hpp"

namespace qoh99 {

struct RegionInfo {
    const char* name;
    GameAddr    address;
    std::uint32_t size;
    GameplayRelevance relevance;
    Confidence  confidence;
    const char* evidence;
};

struct PointerChildInfo {
    const char* name;
    GameAddr    pointerAddress;
    std::uint32_t pointeeSize;
    GameplayRelevance relevance;
    bool        nullable;
    Confidence  confidence;
    const char* evidence;
};

class GameplayRegionCatalog {
public:
    static const RegionInfo* regions(std::size_t& count);
    static const PointerChildInfo* pointerChildren(std::size_t& count);
    static const RegionInfo* find(const char* name);

    static int firstOverlapAmongClassified();
};

}
