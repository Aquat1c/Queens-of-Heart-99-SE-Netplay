#pragma once
#include <cstdint>
#include <vector>
#include "qoh99/GameplayRegionCatalog.hpp"
#include "qoh99/MemoryView.hpp"

namespace qoh99 { namespace netplay {

enum class RegionRole {
    GameplayRequired,
    GameplayCandidate,
    RenderCosmetic,
    DebugOnly
};

struct SaveRegion {
    const char* name;
    GameAddr    addr;
    std::uint32_t size;
    RegionRole  role;
    bool        inGameplayHash;
    bool        inRenderHash;
};

struct PointerRegion {
    const char* name;
    GameAddr    pointerAddr;
    std::uint32_t pointeeSize;
    RegionRole  role;
    bool        nullable;
};

struct Savestate {
    std::vector<std::uint8_t> fixed;
    struct Child { GameAddr base; std::vector<std::uint8_t> bytes; };
    std::vector<Child> children;
    bool valid = false;
};

struct RegionDiffEntry {
    const char* region;
    std::uint32_t offset;
    std::uint32_t count;
};

class SavestateProfile {
public:
    SavestateProfile();

    const std::vector<SaveRegion>& regions() const { return regions_; }
    const std::vector<PointerRegion>& pointerRegions() const { return pointerRegions_; }

    std::size_t fixedBytes() const;
    int firstOverlap() const;

    bool save(const MemoryView& m, Savestate& out) const;
    bool restore(MemoryView& m, const Savestate& s) const;

    std::vector<RegionDiffEntry> diff(const Savestate& a, const Savestate& b,
                                      bool gameplayOnly) const;

private:
    std::vector<SaveRegion> regions_;
    std::vector<PointerRegion> pointerRegions_;
};

}}
