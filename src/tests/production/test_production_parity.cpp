#include "TestFramework.hpp"
#include "qoh99/GameplayRegionCatalog.hpp"
#include "SavestateProfile.hpp"
#include "SyncHash.hpp"
#include "qoh99_game.h"
#include <set>
#include <string>
#include <cstdint>

using namespace qoh99;

TEST(LegacyRollbackManagerParity) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    netplay::SavestateProfile prof;
    const auto& nr = prof.regions();
    std::set<std::string> prodSet, netSet;
    for (size_t i = 0; i < pn; ++i)
        prodSet.insert(std::string(pr[i].name) + ":" + std::to_string(pr[i].addr) +
                       ":" + std::to_string(pr[i].size));
    for (const auto& r : nr)
        netSet.insert(std::string(r.name) + ":" + std::to_string(r.addr) +
                      ":" + std::to_string(r.size));
    CHECK_EQ(pn, nr.size());
    CHECK(prodSet == netSet);
}

TEST(NetplaySavestateProfileMatchesCatalog) {
    netplay::SavestateProfile prof;
    size_t cn = 0; const RegionInfo* cr = GameplayRegionCatalog::regions(cn);
    int captured = 0;
    for (size_t i = 0; i < cn; ++i)
        if (cr[i].relevance == GameplayRelevance::DeterministicGameplay ||
            cr[i].relevance == GameplayRelevance::RenderCosmetic)
            ++captured;
    CHECK_EQ((size_t)captured, prof.regions().size());
    for (size_t i = 0; i < cn; ++i) {
        if (cr[i].relevance != GameplayRelevance::DeterministicGameplay &&
            cr[i].relevance != GameplayRelevance::RenderCosmetic) continue;
        bool found = false;
        for (const auto& r : prof.regions())
            if (std::string(r.name) == cr[i].name) {
                found = true;
                CHECK_EQ((uint32_t)r.addr, (uint32_t)cr[i].address);
                CHECK_EQ((size_t)r.size, cr[i].size);
            }
        CHECK(found);
    }
}

TEST(PointerChildParity) {
    size_t pc = 0; const PtrRegion* pp = pointerRegions(pc);
    netplay::SavestateProfile prof;
    CHECK_EQ(pc, prof.pointerRegions().size());
    for (size_t i = 0; i < pc; ++i) {
        bool found = false;
        for (const auto& npr : prof.pointerRegions())
            if (std::string(npr.name) == pp[i].name) {
                found = true;
                CHECK_EQ((size_t)npr.pointeeSize, pp[i].size);
            }
        CHECK(found);
    }
}

TEST(NetplaySyncHashMatchesProductionHash) {
    const uint8_t bytes[] = { 0x51, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x13, 0x37 };
    auto refFnv = [](const void* data, size_t n, uint64_t h) {
        const uint8_t* b = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        return h;
    };
    const uint64_t seed = 1469598103934665603ull;
    CHECK_EQ(netplay::RegionHasher::fnv1a(bytes, sizeof(bytes), seed),
             refFnv(bytes, sizeof(bytes), seed));
    CHECK_EQ(netplay::RegionHasher::fnv1a(bytes, sizeof(bytes)),
             refFnv(bytes, sizeof(bytes), 14695981039346656037ull));
}

TEST(DesyncDumpIncludesAllRegions) {
    size_t pn = 0; (void)saveRegions(pn);
    size_t pc = 0; (void)pointerRegions(pc);
    size_t cn = 0; const RegionInfo* cr = GameplayRegionCatalog::regions(cn);
    int captured = 0;
    for (size_t i = 0; i < cn; ++i)
        if (cr[i].relevance == GameplayRelevance::DeterministicGameplay ||
            cr[i].relevance == GameplayRelevance::RenderCosmetic) ++captured;
    size_t kn = 0; (void)GameplayRegionCatalog::pointerChildren(kn);
    CHECK_EQ((size_t)captured, pn);
    CHECK_EQ(kn, pc);
}
