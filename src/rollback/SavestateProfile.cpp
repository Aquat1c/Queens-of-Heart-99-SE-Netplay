#include "SavestateProfile.hpp"

namespace qoh99 { namespace netplay {

static bool overlaps(GameAddr a0, std::uint32_t asz, GameAddr b0, std::uint32_t bsz) {
    return a0 < b0 + bsz && b0 < a0 + asz;
}

SavestateProfile::SavestateProfile() {
    std::size_t n = 0;
    const RegionInfo* cat = GameplayRegionCatalog::regions(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (cat[i].relevance == GameplayRelevance::DeterministicGameplay)
            regions_.push_back({ cat[i].name, cat[i].address, cat[i].size,
                                 RegionRole::GameplayRequired, true, false });
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (cat[i].relevance != GameplayRelevance::RenderCosmetic)
            continue;
        bool nested = false;
        for (const auto& r : regions_)
            if (overlaps(cat[i].address, cat[i].size, r.addr, r.size))
                nested = true;
        if (!nested)
            regions_.push_back({ cat[i].name, cat[i].address, cat[i].size,
                                 RegionRole::RenderCosmetic, false, true });
    }
    std::size_t pn = 0;
    const PointerChildInfo* pc = GameplayRegionCatalog::pointerChildren(pn);
    for (std::size_t i = 0; i < pn; ++i) {
        if (pc[i].relevance != GameplayRelevance::DeterministicGameplay)
            continue;
        pointerRegions_.push_back({ pc[i].name, pc[i].pointerAddress, pc[i].pointeeSize,
                                    pc[i].nullable ? RegionRole::GameplayCandidate
                                                   : RegionRole::GameplayRequired,
                                    pc[i].nullable });
    }
}

std::size_t SavestateProfile::fixedBytes() const {
    std::size_t total = 0;
    for (const auto& r : regions_) total += r.size;
    return total;
}

int SavestateProfile::firstOverlap() const {
    for (std::size_t i = 0; i < regions_.size(); ++i)
        for (std::size_t j = i + 1; j < regions_.size(); ++j)
            if (overlaps(regions_[i].addr, regions_[i].size,
                         regions_[j].addr, regions_[j].size))
                return static_cast<int>(i);
    return -1;
}

bool SavestateProfile::save(const MemoryView& m, Savestate& out) const {
    out.fixed.resize(fixedBytes());
    out.children.clear();
    out.valid = false;
    std::size_t off = 0;
    for (const auto& r : regions_) {
        if (!m.read(r.addr, out.fixed.data() + off, r.size))
            return false;
        off += r.size;
    }
    for (const auto& p : pointerRegions_) {
        Savestate::Child c;
        c.base = m.get<GameAddr>(p.pointerAddr);
        if (c.base == kNullAddr) {
            if (!p.nullable) return false;
        } else {
            c.bytes.resize(p.pointeeSize);
            if (!m.read(c.base, c.bytes.data(), p.pointeeSize))
                return false;
        }
        out.children.push_back(std::move(c));
    }
    out.valid = true;
    return true;
}

bool SavestateProfile::restore(MemoryView& m, const Savestate& s) const {
    if (!s.valid || s.fixed.size() != fixedBytes() ||
        s.children.size() != pointerRegions_.size())
        return false;
    std::size_t off = 0;
    for (const auto& r : regions_) {
        if (!m.write(r.addr, s.fixed.data() + off, r.size))
            return false;
        off += r.size;
    }
    // Validate every child before writing any: a half-applied restore is undetectable corruption.
    for (std::size_t i = 0; i < pointerRegions_.size(); ++i) {
        const auto& p = pointerRegions_[i];
        const auto& c = s.children[i];
        const GameAddr cur = m.get<GameAddr>(p.pointerAddr);
        if (c.base == kNullAddr) {
            if (cur != kNullAddr && !p.nullable) return false;
            continue;
        }
        if (cur != c.base)
            return false;
    }
    for (std::size_t i = 0; i < pointerRegions_.size(); ++i) {
        const auto& c = s.children[i];
        if (c.base == kNullAddr)
            continue;
        if (!m.write(c.base, c.bytes.data(), c.bytes.size()))
            return false;
    }
    return true;
}

std::vector<RegionDiffEntry> SavestateProfile::diff(const Savestate& a, const Savestate& b,
                                                    bool gameplayOnly) const {
    std::vector<RegionDiffEntry> out;
    if (a.fixed.size() != b.fixed.size())
        return { { "size-mismatch", 0, 0 } };
    std::size_t off = 0;
    for (const auto& r : regions_) {
        if (!gameplayOnly || r.inGameplayHash) {
            std::uint32_t first = r.size, count = 0;
            for (std::uint32_t i = 0; i < r.size; ++i) {
                if (a.fixed[off + i] != b.fixed[off + i]) {
                    if (first == r.size) first = i;
                    ++count;
                }
            }
            if (count)
                out.push_back({ r.name, first, count });
        }
        off += r.size;
    }
    if (a.children.size() != b.children.size()) {
        out.push_back({ "child-count-mismatch", 0u,
                        static_cast<std::uint32_t>(
                            a.children.size() > b.children.size()
                                ? a.children.size() - b.children.size()
                                : b.children.size() - a.children.size()) });
    }
    for (std::size_t i = 0; i < pointerRegions_.size() &&
                            i < a.children.size() && i < b.children.size(); ++i) {
        const auto& p = pointerRegions_[i];
        const auto& ca = a.children[i];
        const auto& cb = b.children[i];
        if (ca.base != cb.base) {
            out.push_back({ p.name, 0, p.pointeeSize });
            continue;
        }
        std::uint32_t first = p.pointeeSize, count = 0;
        for (std::size_t k = 0; k < ca.bytes.size() && k < cb.bytes.size(); ++k) {
            if (ca.bytes[k] != cb.bytes[k]) {
                if (first == p.pointeeSize) first = static_cast<std::uint32_t>(k);
                ++count;
            }
        }
        if (count)
            out.push_back({ p.name, first, count });
    }
    return out;
}

}}
