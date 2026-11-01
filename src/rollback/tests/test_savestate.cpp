#include "TestFramework.hpp"
#include "qoh99/AddressMap.hpp"
#include "qoh99/TimingSystem.hpp"
#include "rollback/SavestateProfile.hpp"
#include <array>
#include <cstring>
#include <vector>

using namespace qoh99;
using namespace qoh99::netplay;

TEST(savestate_regions_derived_from_catalog_and_disjoint) {
    SavestateProfile p;
    CHECK_EQ(p.firstOverlap(), -1);
    CHECK(p.fixedBytes() > 0x60000);
    std::size_t n = 0;
    const RegionInfo* cat = GameplayRegionCatalog::regions(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (cat[i].relevance != GameplayRelevance::DeterministicGameplay)
            continue;
        bool found = false;
        for (const auto& r : p.regions())
            if (std::string(r.name) == cat[i].name)
                found = true;
        if (!found)
            testfw::reportFail(__FILE__, __LINE__,
                std::string("catalog gameplay region not captured: ") + cat[i].name);
    }
    for (const auto& r : p.regions()) {
        const RegionInfo* ci = GameplayRegionCatalog::find(r.name);
        CHECK(ci != nullptr);
        CHECK(ci->relevance == GameplayRelevance::DeterministicGameplay ||
              ci->relevance == GameplayRelevance::RenderCosmetic);
    }
}

TEST(savestate_never_covers_wallclock_or_os_pointers) {
    SavestateProfile p;
    std::size_t n = 0;
    const AddressEntry* e = AddressMap::entries(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (e[i].cls != MemClass::WallClockPacing && e[i].cls != MemClass::UnsafeOSPointer)
            continue;
        for (const auto& r : p.regions()) {
            bool covered = e[i].va >= r.addr && e[i].va < r.addr + r.size;
            if (covered)
                testfw::reportFail(__FILE__, __LINE__,
                    std::string("region ") + r.name + " covers unsafe global " + e[i].name);
        }
    }
    std::size_t wn = 0;
    const TimingSystem::WallClockRegion* w = TimingSystem::wallClockRegions(wn);
    for (std::size_t i = 0; i < wn; ++i)
        for (const auto& r : p.regions())
            CHECK(!(w[i].base >= r.addr && w[i].base < r.addr + r.size));
}

TEST(savestate_never_restores_full_data_section) {
    SavestateProfile p;
    CHECK(p.fixedBytes() < 0x180000);
    for (const auto& r : p.regions()) {
        CHECK(r.size <= 0x40024);
        if (r.size == 0x40024)
            CHECK(std::string(r.name) == "ai_script_vm");
    }
}

TEST(savestate_save_restore_roundtrip) {
    BufferMemoryView mem;
    SavestateProfile p;

    mem.set<std::uint32_t>(addr::kRngSeedGameplay, 0x1111);
    mem.set<int>(addr::kMatchSubstate, 0);
    mem.set<std::uint32_t>(addr::kFighterP1 + 48, 0xAAAA);
    mem.set<std::uint32_t>(addr::kArenaBlock0 + 100, 0xBBBB);
    const GameAddr child1 = 0x700000, child2 = 0x701000;
    mem.set<GameAddr>(addr::kFighterP1 + 164, child1);
    mem.set<GameAddr>(addr::kFighterP2 + 164, child2);
    mem.set<std::uint32_t>(child1 + 8, 0xC0FFEE);

    Savestate s;
    CHECK(p.save(mem, s));
    CHECK(s.valid);

    mem.set<std::uint32_t>(addr::kRngSeedGameplay, 0x9999);
    mem.set<int>(addr::kMatchSubstate, 5);
    mem.set<std::uint32_t>(addr::kFighterP1 + 48, 0xDEAD);
    mem.set<std::uint32_t>(addr::kArenaBlock0 + 100, 0xDEAD);
    mem.set<std::uint32_t>(child1 + 8, 0xDEAD);

    CHECK(p.restore(mem, s));
    CHECK_EQ(mem.get<std::uint32_t>(addr::kRngSeedGameplay), 0x1111u);
    CHECK_EQ(mem.get<int>(addr::kMatchSubstate), 0);
    CHECK_EQ(mem.get<std::uint32_t>(addr::kFighterP1 + 48), 0xAAAAu);
    CHECK_EQ(mem.get<std::uint32_t>(addr::kArenaBlock0 + 100), 0xBBBBu);
    CHECK_EQ(mem.get<std::uint32_t>(child1 + 8), 0xC0FFEEu);
}

TEST(savestate_round_intro_rotation_restores_coherent_historical_state) {
    BufferMemoryView mem;
    SavestateProfile profile;
    mem.set<GameAddr>(addr::kFighterP1 + 164, 0x700000);
    mem.set<GameAddr>(addr::kFighterP2 + 164, 0x701000);

    const RegionInfo* phaseRegion =
        GameplayRegionCatalog::find("round_intro_state");
    const RegionInfo* controlRegion =
        GameplayRegionCatalog::find("round_intro_control");
    const RegionInfo* rotationRegion =
        GameplayRegionCatalog::find("round_intro_rotation");
    CHECK(phaseRegion != nullptr);
    CHECK(controlRegion != nullptr);
    CHECK(rotationRegion != nullptr);
    if (!phaseRegion || !controlRegion || !rotationRegion) return;

    const GameAddr primaryScale = 0x49B530u;
    const GameAddr secondaryScale = controlRegion->address;
    const GameAddr timer = controlRegion->address + 0x8u;
    mem.set<std::uint32_t>(phaseRegion->address, 5u);
    mem.set<double>(primaryScale, 2.25);
    mem.set<double>(secondaryScale, 3.0);
    mem.set<std::uint32_t>(timer, 110u);
    mem.set<std::int32_t>(rotationRegion->address, -240);

    Savestate frameR;
    CHECK(profile.save(mem, frameR));
    CHECK(frameR.valid);

    mem.set<std::uint32_t>(phaseRegion->address, 6u);
    mem.set<double>(primaryScale, 0.0);
    mem.set<double>(secondaryScale, 0.0);
    mem.set<std::uint32_t>(timer, 10u);
    mem.set<std::int32_t>(rotationRegion->address, 77);
    CHECK(profile.restore(mem, frameR));
    CHECK_EQ(mem.get<std::uint32_t>(phaseRegion->address), 5u);
    CHECK_EQ(mem.get<double>(primaryScale), 2.25);
    CHECK_EQ(mem.get<double>(secondaryScale), 3.0);
    CHECK_EQ(mem.get<std::uint32_t>(timer), 110u);
    CHECK_EQ(mem.get<std::int32_t>(rotationRegion->address), -240);

    using IntroSample = std::array<std::uint64_t, 5>;
    const auto advanceIntro = [&]() {
        std::vector<IntroSample> sequence;
        for (std::size_t step = 0; step < 5u; ++step) {
            const std::uint32_t phase =
                mem.get<std::uint32_t>(phaseRegion->address);
            std::uint32_t countdown = mem.get<std::uint32_t>(timer);
            if (phase == 5u && countdown > 60u) {
                std::int32_t angle =
                    mem.get<std::int32_t>(rotationRegion->address) - 24;
                if (angle < -360) angle += 360;
                mem.set<std::int32_t>(rotationRegion->address, angle);
                mem.set<double>(
                    primaryScale, mem.get<double>(primaryScale) - 0.025);
                mem.set<std::uint32_t>(timer, --countdown);
            }
            const double primary = mem.get<double>(primaryScale);
            const double secondary = mem.get<double>(secondaryScale);
            std::uint64_t primaryBits = 0;
            std::uint64_t secondaryBits = 0;
            std::memcpy(&primaryBits, &primary, sizeof(primaryBits));
            std::memcpy(&secondaryBits, &secondary, sizeof(secondaryBits));
            sequence.push_back({
                phase,
                mem.get<std::uint32_t>(timer),
                static_cast<std::uint32_t>(
                    mem.get<std::int32_t>(rotationRegion->address)),
                primaryBits,
                secondaryBits,
            });
        }
        return sequence;
    };

    const std::vector<IntroSample> first = advanceIntro();
    CHECK(profile.restore(mem, frameR));
    const std::vector<IntroSample> reconstructed = advanceIntro();
    CHECK(first == reconstructed);

    CHECK(profile.restore(mem, frameR));
    Savestate angleA;
    Savestate angleB;
    CHECK(profile.save(mem, angleA));
    mem.set<std::int32_t>(rotationRegion->address, -264);
    CHECK(profile.save(mem, angleB));
    CHECK(profile.diff(angleA, angleB, true).empty());
    const std::vector<RegionDiffEntry> renderDiff =
        profile.diff(angleA, angleB, false);
    CHECK_EQ(renderDiff.size(), 1u);
    CHECK(std::string(renderDiff[0].region) == "round_intro_rotation");
    CHECK_EQ(renderDiff[0].offset, 0u);
    CHECK(renderDiff[0].count > 0u);
}

TEST(savestate_pointer_child_reassignment_fails_loudly) {
    BufferMemoryView mem;
    SavestateProfile p;
    mem.set<GameAddr>(addr::kFighterP1 + 164, 0x700000);
    mem.set<GameAddr>(addr::kFighterP2 + 164, 0x701000);

    Savestate s;
    CHECK(p.save(mem, s));
    s.children[0].base = 0x7FF000;
    CHECK(!p.restore(mem, s));
}

TEST(savestate_diff_reports_region_and_offset) {
    BufferMemoryView mem;
    SavestateProfile p;
    mem.set<GameAddr>(addr::kFighterP1 + 164, 0x700000);
    mem.set<GameAddr>(addr::kFighterP2 + 164, 0x701000);

    Savestate a, b;
    CHECK(p.save(mem, a));
    mem.set<std::uint8_t>(addr::kRngSeedGameplay + 1, 0x7F);
    CHECK(p.save(mem, b));

    auto d = p.diff(a, b, true);
    CHECK_EQ(d.size(), 1u);
    CHECK(std::string(d[0].region) == "rng");
    CHECK_EQ(d[0].offset, 9u);
    CHECK_EQ(d[0].count, 1u);
}

TEST(savestate_aux_actor_pool_is_captured_and_restored_exactly) {
    BufferMemoryView mem;
    SavestateProfile p;

    mem.set<GameAddr>(addr::kFighterP1 + 164, 0x700000);
    mem.set<GameAddr>(addr::kFighterP2 + 164, 0x701000);

    const GameAddr pool = 0x8737CCu;
    const std::uint32_t poolSize = 0x959Cu;
    mem.set<std::uint8_t>(pool, 0xAB);
    mem.set<std::uint8_t>(pool + 0x59Cu, 0xCD);
    mem.set<std::uint8_t>(pool + poolSize - 1u, 0xEF);

    Savestate s;
    CHECK(p.save(mem, s));
    CHECK(s.valid);

    mem.set<std::uint8_t>(pool, 0x00);
    mem.set<std::uint8_t>(pool + 0x59Cu, 0x00);
    mem.set<std::uint8_t>(pool + poolSize - 1u, 0x00);

    CHECK(p.restore(mem, s));
    CHECK_EQ(mem.get<std::uint8_t>(pool), 0xABu);
    CHECK_EQ(mem.get<std::uint8_t>(pool + 0x59Cu), 0xCDu);
    CHECK_EQ(mem.get<std::uint8_t>(pool + poolSize - 1u), 0xEFu);
}

TEST(savestate_captures_the_aux_actor_pool_exactly_once) {
    SavestateProfile p;
    std::size_t count = 0;
    for (const auto& r : p.regions())
        if (std::string(r.name) == "match_aux_actor_pool_slots_05_29")
            ++count;
    CHECK_EQ(count, 1u);
}
