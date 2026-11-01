#include "TestFramework.hpp"
#include "qoh99/GameplayRegionCatalog.hpp"
#include "qoh99_game.h"
#include <string>
#include <cstring>

using namespace qoh99;

static const RegionInfo* findRegion(const char* name) {
    size_t n = 0; const RegionInfo* r = GameplayRegionCatalog::regions(n);
    for (size_t i = 0; i < n; ++i)
        if (std::strcmp(r[i].name, name) == 0) return &r[i];
    return nullptr;
}

TEST(AiRngDrawsAreGameplay) {
    const char* aiRegions[] = { "ai_script_vm", "ai_player_state" };
    for (const char* nm : aiRegions) {
        const RegionInfo* r = findRegion(nm);
        CHECK(r != nullptr);
        if (r) CHECK(r->relevance == GameplayRelevance::DeterministicGameplay);
    }
    const RegionInfo* aiVars = findRegion("dw_mid2b_ai_vars");
    CHECK(aiVars && aiVars->address == 0x6248E0
          && aiVars->address + aiVars->size == 0x6249E0
          && aiVars->relevance
              == GameplayRelevance::DeterministicGameplay);
    const RegionInfo* hi = findRegion("dw_hi");
    CHECK(hi && hi->address == 0x6275A8u &&
          0x62D640u == hi->address + hi->size &&
          hi->relevance == GameplayRelevance::DeterministicGameplay);
    CHECK(0x62D634u >= hi->address
          && 0x62D634u < hi->address + hi->size);
}

TEST(NoDefaultCosmeticRngRedirectForAi) {
    CHECK(findRegion("sound_script_vars") == nullptr);
    CHECK(findRegion("sound_bank_state") == nullptr);
    const uintptr_t aiSpots[] = { 0x6248E0, 0x62D640, 0x62EB80, 0x66EBA0, 0x8A5E00 };
    size_t n = 0; const RegionInfo* r = GameplayRegionCatalog::regions(n);
    for (size_t i = 0; i < n; ++i) {
        if (r[i].relevance != GameplayRelevance::RenderCosmetic) continue;
        for (uintptr_t a : aiSpots)
            if (a >= r[i].address && a < r[i].address + r[i].size)
                testfw::reportFail(__FILE__, __LINE__,
                    std::string("RenderCosmetic region ") + r[i].name + " covers AI state");
    }
}

TEST(AiBankTableIsExactExternalOwnerTable) {
    const RegionInfo* banks = findRegion("ai_script_banks");
    CHECK(banks != nullptr);
    if (banks) {
        CHECK_EQ((uint32_t)banks->address, 0x62D640u);
        CHECK_EQ(banks->size, (size_t)(20u * 0x110u));
        CHECK_EQ(
            (uint32_t)(banks->address + banks->size), 0x62EB80u);
        CHECK(banks->relevance == GameplayRelevance::ExternalResource);
    }
    for (uint32_t bank = 0; bank < 20; ++bank) {
        const uint32_t base = 0x62D640u + bank * 0x110u;
        CHECK_EQ(base + 0x104u, 0x62D744u + bank * 0x110u);
        CHECK_EQ(base + 0x108u, 0x62D748u + bank * 0x110u);
        CHECK_EQ(base + 0x10Cu, 0x62D74Cu + bank * 0x110u);
    }
}

TEST(AiVmCoversFullBoundedStackAndCursorOnly) {
    const RegionInfo* vm = findRegion("ai_script_vm");
    CHECK(vm != nullptr);
    if (vm) {
        CHECK_EQ((uint32_t)vm->address, 0x62EB80u);
        CHECK_EQ(vm->size, (size_t)0x40024u);
        CHECK_EQ((uint32_t)(vm->address + vm->size), 0x66EBA4u);
        CHECK(0x62EBA0u >= vm->address);
        CHECK_EQ(0x66EBA0u - 0x62EBA0u, 0x40000u);
    }
    CHECK(findRegion("ai_script_cursor") == nullptr);
    CHECK(findRegion("gap_after_data_working") == nullptr);

    size_t n = 0; const Region* pr = saveRegions(n);
    for (size_t i = 0; i < n; ++i) {
        bool covers = 0x66EBA4 >= pr[i].addr && 0x66EBA4 < pr[i].addr + pr[i].size;
        if (covers)
            testfw::reportFail(__FILE__, __LINE__,
                std::string("save region ") + pr[i].name + " covers callback ptr 0x66EBA4");
    }
}
