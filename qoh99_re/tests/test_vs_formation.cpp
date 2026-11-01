#include "TestFramework.hpp"
#include "qoh99/SceneSystem.hpp"
#include <initializer_list>
#include <string>

using namespace qoh99;

namespace {

struct ExpectedRow {
    int          row;
    VsFormation  formation;
    std::uint8_t humanSlotMask;
    std::uint8_t cpuMask;
    int          route[4];
    int          confirms;
};

const ExpectedRow kExpected[] = {
    { 0, VsFormation::TwoFighter,  0x1, 0x2, { 0, 1, 2, 3 }, 2 },
    { 1, VsFormation::TwoFighter,  0x3, 0x0, { 0, 1, 2, 3 }, 2 },
    { 2, VsFormation::FourFighter, 0x1, 0xE, { 0, 1, 2, 3 }, 4 },
    { 3, VsFormation::FourFighter, 0x5, 0xA, { 0, 2, 1, 3 }, 4 },
    { 4, VsFormation::FourFighter, 0x3, 0xC, { 0, 1, 2, 3 }, 4 },
    { 5, VsFormation::FourFighter, 0xF, 0x0, { 0, 1, 2, 3 }, 4 },
};

}

TEST(vsformation_six_rows_exact_ownership_and_route) {
    std::size_t n = 0;
    const VsRow* rows = VsFormationModel::rows(n);
    CHECK(rows != nullptr);
    CHECK_EQ(n, 6u);
    CHECK_EQ(VsFormationModel::kRowCount, 6u);

    for (const ExpectedRow& want : kExpected) {
        const VsRow* got = VsFormationModel::row(want.row);
        if (!got) {
            testfw::reportFail(__FILE__, __LINE__,
                "missing VS row " + std::to_string(want.row));
            continue;
        }
        CHECK_EQ(got->row, want.row);
        CHECK(got->formation == want.formation);
        CHECK_EQ(got->humanSlotMask, want.humanSlotMask);
        CHECK_EQ(got->cpuMask, want.cpuMask);
        CHECK_EQ(got->requiredSelectorConfirms, want.confirms);
        CHECK_EQ(VsFormationModel::requiredSelectorConfirms(want.row), want.confirms);
        CHECK(VsFormationModel::formationOf(want.row) == want.formation);
        for (int slot = 0; slot < 4; ++slot) {
            CHECK_EQ(static_cast<int>(got->route[slot]), want.route[slot]);
            CHECK_EQ(VsFormationModel::sourceForSlot(want.row, slot), want.route[slot]);
            const bool human = (want.humanSlotMask & (1u << slot)) != 0;
            const bool cpu = (want.cpuMask & (1u << slot)) != 0;
            CHECK(VsFormationModel::isHumanSlot(want.row, slot) == human);
            CHECK(VsFormationModel::isCpuSlot(want.row, slot) == cpu);
        }
        CHECK(got->description && got->description[0] != '\0');
    }

    CHECK(VsFormationModel::row(-1) == nullptr);
    CHECK(VsFormationModel::row(6) == nullptr);
    CHECK(VsFormationModel::formationOf(6) == VsFormation::NotSelected);
    CHECK_EQ(VsFormationModel::sourceForSlot(6, 0), -1);
    CHECK_EQ(VsFormationModel::sourceForSlot(0, 4), -1);
}

TEST(vsformation_two_versus_four_fighter_split) {
    CHECK_EQ(static_cast<int>(VsFormation::NotSelected), 0);
    CHECK_EQ(static_cast<int>(VsFormation::TwoFighter), 1);
    CHECK_EQ(static_cast<int>(VsFormation::FourFighter), 2);
    CHECK_EQ(addr::kVsFormationRow, 0x62369Cu);

    for (int row = 0; row <= 1; ++row) {
        CHECK(VsFormationModel::formationOf(row) == VsFormation::TwoFighter);
        CHECK(!VsFormationModel::isFourFighter(row));
        CHECK_EQ(VsFormationModel::fighterCount(VsFormationModel::formationOf(row)), 2);
    }
    for (int row = 2; row <= 5; ++row) {
        CHECK(VsFormationModel::formationOf(row) == VsFormation::FourFighter);
        CHECK(VsFormationModel::isFourFighter(row));
        CHECK_EQ(VsFormationModel::fighterCount(VsFormationModel::formationOf(row)), 4);
    }
}

TEST(vsformation_sides_are_slots_0_2_versus_1_3) {
    CHECK_EQ(VsFormationModel::kSideSlots[0][0], 0);
    CHECK_EQ(VsFormationModel::kSideSlots[0][1], 2);
    CHECK_EQ(VsFormationModel::kSideSlots[1][0], 1);
    CHECK_EQ(VsFormationModel::kSideSlots[1][1], 3);

    CHECK_EQ(VsFormationModel::sideOfSlot(0), VsFormationModel::kSideLeft);
    CHECK_EQ(VsFormationModel::sideOfSlot(2), VsFormationModel::kSideLeft);
    CHECK_EQ(VsFormationModel::sideOfSlot(1), VsFormationModel::kSideRight);
    CHECK_EQ(VsFormationModel::sideOfSlot(3), VsFormationModel::kSideRight);

    CHECK(VsFormationModel::areTeammates(0, 2));
    CHECK(VsFormationModel::areTeammates(1, 3));
    CHECK(!VsFormationModel::areTeammates(0, 1));
    CHECK(!VsFormationModel::areTeammates(2, 3));
    CHECK(!VsFormationModel::areTeammates(0, 0));
}

TEST(vsformation_row3_route_is_non_identity) {
    const VsRow* row3 = VsFormationModel::row(3);
    CHECK(row3 != nullptr);
    CHECK(!VsFormationModel::hasIdentityRoute(3));
    CHECK_EQ(VsFormationModel::sourceForSlot(3, 0), 0);
    CHECK_EQ(VsFormationModel::sourceForSlot(3, 1), 2);
    CHECK_EQ(VsFormationModel::sourceForSlot(3, 2), 1);
    CHECK_EQ(VsFormationModel::sourceForSlot(3, 3), 3);
    CHECK(VsFormationModel::isHumanSlot(3, 0));
    CHECK(VsFormationModel::isHumanSlot(3, 2));
    CHECK(VsFormationModel::areTeammates(0, 2));

    for (int row : { 0, 1, 2, 4, 5 })
        CHECK(VsFormationModel::hasIdentityRoute(row));
}

TEST(vsformation_four_fighter_launch_needs_all_four_selectors) {
    const int none[4]  = { 0, 0, 0, 0 };
    const int two[4]   = { 4, 4, 0, 0 };
    const int three[4] = { 4, 4, 4, 0 };
    const int four[4]  = { 4, 4, 4, 4 };

    CHECK_EQ(VsFormationModel::requiredSelectorConfirms(VsFormation::TwoFighter), 2);
    CHECK(VsFormationModel::launchReady(1, two));
    CHECK(VsFormationModel::launchReady(1, four));
    CHECK(!VsFormationModel::launchReady(1, none));

    CHECK_EQ(VsFormationModel::requiredSelectorConfirms(VsFormation::FourFighter), 4);
    for (int row = 2; row <= 5; ++row) {
        CHECK_EQ(VsFormationModel::requiredSelectorConfirms(row), 4);
        CHECK(!VsFormationModel::launchReady(row, two));
        CHECK(!VsFormationModel::launchReady(row, three));
        CHECK(VsFormationModel::launchReady(row, four));
    }

    CHECK_EQ(VsFormationModel::requiredSelectorConfirms(VsFormation::NotSelected), 0);
    CHECK(!VsFormationModel::launchReady(VsFormation::NotSelected, four));
    CHECK(!VsFormationModel::launchReady(VsFormation::TwoFighter, nullptr));
}

TEST(vsformation_direct_boot_scope_is_two_fighter_only) {
    CHECK(DirectBootController::supportsRow(0));
    CHECK(DirectBootController::supportsRow(1));
    for (int row = 2; row <= 5; ++row)
        CHECK(!DirectBootController::supportsRow(row));
}

TEST(vsformation_runtime_view_reads_formation_not_match_mode) {
    BufferMemoryView mem;
    SceneSystem scenes(mem);

    mem.set<int>(addr::kVsFormationRow, 0);
    CHECK(scenes.vsFormation() == VsFormation::NotSelected);
    CHECK(!scenes.inFourFighterMode());

    mem.set<int>(addr::kVsFormationRow, 1);
    CHECK(scenes.vsFormation() == VsFormation::TwoFighter);
    CHECK(!scenes.inFourFighterMode());

    mem.set<int>(addr::kVsFormationRow, 2);
    CHECK(scenes.vsFormation() == VsFormation::FourFighter);
    CHECK(scenes.inFourFighterMode());

    mem.set<int>(addr::kScene3Cursor, 5);
    CHECK_EQ(scenes.vsListRow(), 5);
    mem.set<int>(addr::kControlMask, 0xA);
    CHECK_EQ(scenes.cpuMask(), 0xA);
}
