#include "TestFramework.hpp"
#include "qoh99/AddressMap.hpp"
#include "qoh99/Fighter.hpp"
#include <cctype>
#include <string>

using namespace qoh99;

TEST(addressmap_entries_exist_and_lookup) {
    std::size_t n = 0;
    const AddressEntry* e = AddressMap::entries(n);
    CHECK(e != nullptr);
    CHECK_EQ(n, 105u);

    const AddressEntry* scene = AddressMap::find("sceneCurrent");
    CHECK(scene && scene->va == 0x6234EC);
    const AddressEntry* byVa = AddressMap::findByVa(0x8719A4);
    CHECK(byVa && std::string(byVa->name) == "matchSubstate");
    CHECK(AddressMap::find("noSuchSymbol") == nullptr);
    const AddressEntry* host = AddressMap::find("netIsHost");
    CHECK(host && host->va == addr::kNetIsHost);
}

TEST(addressmap_spot_values) {
    CHECK_EQ(addr::kSceneCurrent, 0x6234ECu);
    CHECK_EQ(addr::kSceneNext, 0x61801Cu);
    CHECK_EQ(addr::kMatchSubstate, 0x8719A4u);
    CHECK_EQ(addr::kFighterP1, 0x623E00u);
    CHECK_EQ(addr::kFighterP2, 0x626880u);
    CHECK_EQ(addr::kArenaBlock0, 0x4ED140u);
    CHECK_EQ(addr::kArenaBlock1, 0x50A010u);
    CHECK_EQ(addr::kArenaActivePtr, 0x6234E8u);
    CHECK_EQ(addr::kBattleLifeArray, 0x6254E0u);
    CHECK_EQ(addr::kBattleLifeStride, 16u);
    CHECK_EQ(addr::kExGaugeRecords, 0x6267E0u);
    CHECK_EQ(addr::kExGaugeStocks, 0x6267E4u);
    CHECK_EQ(addr::kExGaugeSpendTimer, 0x6267E8u);
    CHECK_EQ(addr::kExGaugePendingCosts, 0x6267ECu);
    CHECK_EQ(addr::kExGaugeFraction, 0x6267FCu);
    CHECK_EQ(addr::kExGaugeRecordStride, 0x20u);
    CHECK_EQ(addr::kExGaugeStorageRecordCount, 4u);
    CHECK_EQ(addr::kExGaugeActiveOwnerCount, 2u);
    CHECK_EQ(addr::kExGaugePendingCostLanes, 4u);
    CHECK_EQ(addr::kExGaugeStockMax, 9u);
    CHECK_EQ(addr::kExGaugeFractionMax, 53u);
    CHECK_EQ(addr::kScene3ConfirmFlag, 0x4AA450u);
    CHECK_EQ(addr::kVsListEntryFlag, 0x49EF00u);
    CHECK_EQ(addr::kFighterSize, 0x5FCu);
    CHECK_EQ(addr::kArenaBlockSize, 0x1CED0u);
    CHECK_EQ(addr::kVsFormationRow, 0x62369Cu);
    CHECK_EQ(addr::kOpeningMovieFilename, 0x4AB794u);
}

TEST(addressmap_ex_gauge_compound_owner_record) {
    const AddressEntry* e = AddressMap::find("exGaugeRecords");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK_EQ(e->va, 0x6267E0u);
    CHECK_EQ(e->size, 0x80u);
    CHECK(std::string(e->origName) == "dword_6267E0");
    CHECK(e->cls == MemClass::MutableGameplay);
    CHECK(e->conf == Confidence::StaticAnalysis);

    CHECK_EQ(addr::kExGaugeStocks, 0x6267E4u);
    CHECK_EQ(addr::kExGaugeStocks + addr::kExGaugeRecordStride, 0x626804u);
    CHECK_EQ(addr::kExGaugeFraction, 0x6267FCu);
    CHECK_EQ(addr::kExGaugeFraction + addr::kExGaugeRecordStride, 0x62681Cu);
}

TEST(fighter_ex_owner_is_distinct_from_player_index_and_com_ownership) {
    constexpr GameAddr base = 0x100000u;
    BufferMemoryView memory;
    CHECK(memory.set<std::uint8_t>(
        base + fighter_field::kExGaugeOwner, 1u));
    CHECK(memory.set<std::uint8_t>(
        base + fighter_field::kPlayerIndex, 3u));
    CHECK(memory.set<int>(
        base + fighter_field::kComOwnership, 1));

    Fighter fighter(memory, base);
    CHECK_EQ(fighter.exGaugeOwner(), 1u);
    CHECK_EQ(fighter.playerIndex(), 3u);
    CHECK(fighter.comOwned());
}

TEST(addressmap_opening_movie_has_no_replay_semantics) {
    const AddressEntry* e = AddressMap::find("openingMovieFilename");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK_EQ(e->va, 0x4AB794u);
    CHECK_EQ(e->size, 1u);
    CHECK(std::string(e->origName) == "byte_4AB794");
    CHECK(e->cls == MemClass::ControlFlow);
    CHECK(e->conf == Confidence::RuntimeVerified);
    CHECK(AddressMap::findByVa(0x4AB794) == e);

    const std::string evidence = e->evidence;
    CHECK(evidence.find("Opening.avi") != std::string::npos);

    auto lower = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    CHECK(lower(e->name).find("replay") == std::string::npos);
    CHECK(lower(evidence).find("replay") == std::string::npos);
    CHECK(AddressMap::find("replayFilename") == nullptr);
    CHECK(AddressMap::findExact("replayFilename") == nullptr);
}

TEST(addressmap_no_entry_claims_native_replay) {
    std::size_t n = 0;
    const AddressEntry* e = AddressMap::entries(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::string blob = std::string(e[i].name) + '|' + e[i].origName + '|'
                         + e[i].evidence;
        for (char& c : blob)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (blob.find("replay") != std::string::npos)
            testfw::reportFail(__FILE__, __LINE__,
                std::string("address entry still claims replay semantics: ")
                + e[i].name);
    }
}

TEST(addressmap_vs_formation_row_replaces_match_mode) {
    const AddressEntry* e = AddressMap::findExact("vsFormationRow");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK_EQ(e->va, 0x62369Cu);
    CHECK_EQ(e->size, 4u);
    CHECK(std::string(e->origName) == "dword_62369C");
    CHECK(e->cls == MemClass::MutableGameplay);

    CHECK(AddressMap::findExact("matchMode") == nullptr);
    CHECK(AddressMap::findExact("teamSize") == nullptr);
    CHECK(AddressMap::find("matchMode") == e);
    CHECK(AddressMap::find("teamSize") == e);
    CHECK(AddressMap::find("noSuchLegacyName") == nullptr);

    std::size_t n = 0;
    const LegacyNameAlias* aliases = AddressMap::legacyNames(n);
    CHECK(aliases != nullptr);
    CHECK(n >= 2u);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(aliases[i].legacyName && aliases[i].legacyName[0] != '\0');
        CHECK(aliases[i].reason && aliases[i].reason[0] != '\0');
        CHECK(AddressMap::findExact(aliases[i].currentName) != nullptr);
        CHECK(std::string(aliases[i].legacyName) != "replayFilename");
    }
}

TEST(addressmap_all_entries_within_image_and_evidenced) {
    std::size_t n = 0;
    const AddressEntry* e = AddressMap::entries(n);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(e[i].va >= 0x400000u && e[i].va < 0x900000u);
        CHECK(e[i].evidence && e[i].evidence[0] != '\0');
        CHECK(e[i].name && e[i].name[0] != '\0');
    }
}

TEST(addressmap_va_uniqueness) {
    std::size_t n = 0;
    const AddressEntry* e = AddressMap::entries(n);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j)
            if (e[i].va == e[j].va)
                testfw::reportFail(__FILE__, __LINE__,
                    std::string("duplicate VA: ") + e[i].name + " vs " + e[j].name);
}

TEST(addressmap_memclass_distinctions) {
    CHECK(AddressMap::find("frameTimeRing")->cls == MemClass::WallClockPacing);
    CHECK(AddressMap::find("ddraw2Ptr")->cls == MemClass::UnsafeOSPointer);
    CHECK(AddressMap::find("spriteSurfaceTable")->cls == MemClass::UnsafeOSPointer);
    CHECK(AddressMap::find("chatCallbackPtr")->cls == MemClass::UnsafeOSPointer);
    CHECK(AddressMap::find("logicalMaskTable")->cls == MemClass::ReadOnlyTable);
    CHECK(AddressMap::find("healthArray")->cls == MemClass::MutableGameplay);
    CHECK(AddressMap::find("hudTextBuffer")->cls == MemClass::RenderCosmetic);
    CHECK(AddressMap::find("netConfigBlock")->cls == MemClass::ControlFlow);
    CHECK(AddressMap::find("netConfigBlock20")->cls == MemClass::ControlFlow);
}
