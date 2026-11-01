#include "SyncHash.hpp"

namespace qoh99 { namespace netplay {

bool SyncHash::includes(bool inGameplayHash, bool inRenderHash, HashProfile p) const {
    switch (p) {
        case HashProfile::GameplaySync:   return inGameplayHash;
        case HashProfile::RenderCosmetic: return inRenderHash;
        case HashProfile::FullDebugWide:  return true;
    }
    return false;
}

std::vector<RegionHash> SyncHash::regionHashes(const Savestate& s, HashProfile p) const {
    std::vector<RegionHash> out;
    std::size_t off = 0;
    for (const auto& r : profile_.regions()) {
        if (includes(r.inGameplayHash, r.inRenderHash, p) &&
            off + r.size <= s.fixed.size())
            out.push_back({ r.name, RegionHasher::fnv1a(s.fixed.data() + off, r.size) });
        off += r.size;
    }
    const auto& prs = profile_.pointerRegions();
    for (std::size_t i = 0; i < prs.size() && i < s.children.size(); ++i) {
        if (!includes(true, false, p))
            continue;
        const auto& c = s.children[i];
        // Hash child contents and presence, never the address: addresses differ between processes.
        const std::uint8_t present = c.base != 0u ? 1u : 0u;
        std::uint64_t h = RegionHasher::fnv1a(&present, sizeof(present));
        h = RegionHasher::fnv1a(c.bytes.data(), c.bytes.size(), h);
        out.push_back({ prs[i].name, h });
    }
    return out;
}

std::uint64_t SyncHash::combined(const Savestate& s, HashProfile p) const {
    std::uint64_t h = RegionHasher::kFnvOffset;
    for (const auto& rh : regionHashes(s, p))
        h = RegionHasher::fnv1a(&rh.hash, sizeof(rh.hash), h);
    return h;
}

SyncHash::FirstDiff SyncHash::firstDiff(const Savestate& a, const Savestate& b,
                                        HashProfile p) const {
    FirstDiff fd;
    bool gameplayOnly = (p == HashProfile::GameplaySync);
    auto entries = profile_.diff(a, b, gameplayOnly);
    if (p == HashProfile::RenderCosmetic) {
        auto all = profile_.diff(a, b, false);
        entries.clear();
        for (const auto& e : all)
            for (const auto& r : profile_.regions())
                if (r.name == e.region && r.inRenderHash)
                    entries.push_back(e);
    }
    if (!entries.empty()) {
        fd.differs = true;
        fd.region = entries.front().region;
        fd.offset = entries.front().offset;
    }
    return fd;
}

}}
