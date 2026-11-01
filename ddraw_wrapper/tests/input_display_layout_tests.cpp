#include "../input_display_layout.h"

#include <cstdio>

namespace layout = qoh99_input_display_layout;

namespace {

int g_failures = 0;
int g_checks = 0;

void reportFailure(const char* file, int line, const char* what) {
    ++g_failures;
    std::printf("FAIL %s:%d  %s\n", file, line, what);
}

void check(bool condition, const char* file, int line, const char* what) {
    ++g_checks;
    if (!condition) reportFailure(file, line, what);
}

void checkNear(float actual, float expected, float tolerance, const char* file,
               int line, const char* what) {
    ++g_checks;
    const float diff = actual - expected;
    const float magnitude = (diff < 0.0f) ? -diff : diff;
    if (magnitude > tolerance) {
        ++g_failures;
        std::printf("FAIL %s:%d  %s (got %.4f, expected %.4f +/- %.4f)\n", file,
                    line, what, static_cast<double>(actual),
                    static_cast<double>(expected),
                    static_cast<double>(tolerance));
    }
}

void checkEqInt(int actual, int expected, const char* file, int line,
                const char* what) {
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("FAIL %s:%d  %s (got %d, expected %d)\n", file, line, what,
                    actual, expected);
    }
}

#define CHECK(cond) check((cond), __FILE__, __LINE__, #cond)
#define CHECK_NEAR(a, b, tol) checkNear((a), (b), (tol), __FILE__, __LINE__, \
                                        #a " ~= " #b)
#define CHECK_EQ_INT(a, b) checkEqInt((a), (b), __FILE__, __LINE__, #a " == " #b)

const int kHistoryCapacity = 32;

struct Resolution {
    int w;
    int h;
    const char* name;
};

const Resolution kFourThree[] = {
    {640, 480, "640x480"},     {800, 600, "800x600"},
    {960, 720, "960x720"},     {1024, 768, "1024x768"},
    {1280, 960, "1280x960"},   {1440, 1080, "1440x1080"},
    {1600, 1200, "1600x1200"}, {2048, 1536, "2048x1536"},
    {2880, 2160, "2880x2160"}, {4096, 3072, "4096x3072"},
};
const int kFourThreeCount =
    static_cast<int>(sizeof(kFourThree) / sizeof(kFourThree[0]));

void testLiveBoxIsSeparatedFromHistory() {
    for (int i = 0; i < kFourThreeCount; ++i) {
        const layout::Metrics m = layout::compute(
            0, 0, kFourThree[i].w, kFourThree[i].h, kHistoryCapacity);
        CHECK(m.renderable);
        CHECK_NEAR(m.liveBottom - m.liveTop, m.cell, 0.001f);
        CHECK(m.liveGap >= layout::kLiveGapMinPx);
        CHECK_NEAR(m.historyTop - m.liveBottom, m.liveGap, 0.001f);
        CHECK_NEAR(m.liveGap, layout::floorFloat(m.liveGap), 0.0001f);
        CHECK_NEAR(m.liveTop, m.bandTop, 0.001f);
        CHECK_NEAR(m.historyBottom, m.bandBottom, 0.001f);
    }
}

void testEveryVisibleRowFitsInsideTheHistoryBox() {
    for (int i = 0; i < kFourThreeCount; ++i) {
        const layout::Metrics m = layout::compute(
            0, 0, kFourThree[i].w, kFourThree[i].h, kHistoryCapacity);
        CHECK(m.renderable);
        CHECK(m.visibleRows > 0);
        for (int row = 0; row < m.visibleRows; ++row) {
            if (!layout::historyRowFits(m, row)) {
                std::printf("FAIL %s: row %d of %d does not fit "
                            "(top %.3f + cell %.3f > bottom %.3f)\n",
                            kFourThree[i].name, row, m.visibleRows,
                            layout::historyRowTop(m, row), m.cell,
                            m.historyBottom);
                ++g_failures;
            }
            ++g_checks;
        }
        if (m.visibleRows < kHistoryCapacity) {
            CHECK(!layout::historyRowFits(m, m.visibleRows));
        }
    }
}

void testStackNeverEscapesTheBand() {
    for (int i = 0; i < kFourThreeCount; ++i) {
        const layout::Metrics m = layout::compute(
            0, 0, kFourThree[i].w, kFourThree[i].h, kHistoryCapacity);
        CHECK(m.renderable);
        const float used = m.cell + m.liveGap
            + m.cell * static_cast<float>(m.visibleRows);
        CHECK(used <= (m.bandBottom - m.bandTop) + 0.001f);
    }
}

void testRowCountAccountsForTheGap() {
    for (int i = 0; i < kFourThreeCount; ++i) {
        const layout::Metrics m = layout::compute(
            0, 0, kFourThree[i].w, kFourThree[i].h, kHistoryCapacity);
        const int preSplitCount =
            static_cast<int>((m.bandBottom - m.bandTop) / m.cell) - 1;
        CHECK(m.visibleRows <= preSplitCount);
        CHECK_EQ_INT(m.visibleRows, 12);
    }
}

void testRowCountIsStableAcrossEveryFourThreeResolution() {
    int first = -1;
    for (int i = 0; i < kFourThreeCount; ++i) {
        const layout::Metrics m = layout::compute(
            0, 0, kFourThree[i].w, kFourThree[i].h, kHistoryCapacity);
        CHECK(m.renderable);
        if (first < 0) {
            first = m.visibleRows;
        } else if (m.visibleRows != first) {
            std::printf("FAIL %s: visibleRows %d, but %s gave %d\n",
                        kFourThree[i].name, m.visibleRows, kFourThree[0].name,
                        first);
            ++g_failures;
        }
        ++g_checks;
        const float exact = (m.historyBottom - m.historyTop) / m.cell;
        const float fractional = exact - layout::floorFloat(exact);
        CHECK(fractional > 0.02f);
    }
}

void testLiveAndHistoryShareTheirColumns() {
    for (int i = 0; i < kFourThreeCount; ++i) {
        const layout::Metrics m = layout::compute(
            0, 0, kFourThree[i].w, kFourThree[i].h, kHistoryCapacity);
        for (unsigned lane = 0; lane < 4u; ++lane) {
            const layout::LanePanelX p = layout::lanePanelX(m, lane);
            CHECK_NEAR(p.right - p.left, m.panelWidth, 0.001f);
            CHECK(m.panelWidth
                  >= 2.0f * m.inset
                      + static_cast<float>(layout::kGlyphColumns) * m.cell
                      + m.durationColumn - 0.001f);
        }
    }
}

void testLanesNeverOverlap() {
    for (int i = 0; i < kFourThreeCount; ++i) {
        const layout::Metrics m = layout::compute(
            0, 0, kFourThree[i].w, kFourThree[i].h, kHistoryCapacity);
        CHECK(m.renderable);
        const layout::LanePanelX l0 = layout::lanePanelX(m, 0u);
        const layout::LanePanelX l1 = layout::lanePanelX(m, 1u);
        CHECK(!l0.rightAligned);
        CHECK(l1.rightAligned);
        CHECK(l0.right <= l1.left);
        CHECK(l0.left >= m.imageLeft);
        CHECK(l1.right <= m.imageLeft + m.imageWidth + 0.001f);
    }
}

void testPanelsFollowTheDestinationOffset() {
    const layout::Metrics origin =
        layout::compute(0, 0, 1280, 960, kHistoryCapacity);
    const layout::Metrics offset =
        layout::compute(320, 60, 1280, 960, kHistoryCapacity);
    CHECK(origin.renderable && offset.renderable);
    CHECK_NEAR(offset.bandTop - origin.bandTop, 60.0f, 0.001f);
    CHECK_NEAR(offset.historyTop - origin.historyTop, 60.0f, 0.001f);
    CHECK_NEAR(offset.liveBottom - origin.liveBottom, 60.0f, 0.001f);
    const layout::LanePanelX a = layout::lanePanelX(origin, 1u);
    const layout::LanePanelX b = layout::lanePanelX(offset, 1u);
    CHECK_NEAR(b.left - a.left, 320.0f, 0.001f);
    CHECK_EQ_INT(offset.visibleRows, origin.visibleRows);
}

void testDegenerateDestinationsRefuseToRender() {
    const layout::Metrics zero = layout::compute(0, 0, 0, 0, kHistoryCapacity);
    CHECK(!zero.renderable);
    CHECK_EQ_INT(zero.visibleRows, 0);

    const layout::Metrics negative =
        layout::compute(0, 0, -1920, -1080, kHistoryCapacity);
    CHECK(!negative.renderable);
    CHECK_EQ_INT(negative.visibleRows, 0);

    const layout::Metrics narrow =
        layout::compute(0, 0, 100, 480, kHistoryCapacity);
    CHECK(!narrow.renderable);
    CHECK_EQ_INT(narrow.visibleRows, 0);

    const layout::Metrics justWide =
        layout::compute(0, 0, 160, 480, kHistoryCapacity);
    CHECK(justWide.renderable);
    CHECK(justWide.visibleRows > 0);

    const layout::Metrics squat =
        layout::compute(0, 0, 1920, 30, kHistoryCapacity);
    CHECK(!squat.renderable);
    CHECK_EQ_INT(squat.visibleRows, 0);

    const layout::Metrics justTall =
        layout::compute(0, 0, 1920, 40, kHistoryCapacity);
    CHECK(justTall.renderable);
    CHECK_EQ_INT(justTall.visibleRows, 1);
    CHECK(layout::historyRowFits(justTall, 0));
}

void testNonFourThreeDestinationsStayCoherent() {
    const Resolution odd[] = {
        {1920, 1080, "1920x1080"},
        {2560, 1080, "2560x1080"},
        {1080, 1920, "1080x1920 portrait"},
        {3840, 2160, "3840x2160"},
    };
    for (int i = 0; i < 4; ++i) {
        const layout::Metrics m =
            layout::compute(0, 0, odd[i].w, odd[i].h, kHistoryCapacity);
        if (!m.renderable) {
            CHECK_EQ_INT(m.visibleRows, 0);
            continue;
        }
        CHECK(m.cell > 0.0f);
        CHECK(m.historyTop > m.liveBottom);
        for (int row = 0; row < m.visibleRows; ++row) {
            CHECK(layout::historyRowFits(m, row));
        }
        const layout::LanePanelX l0 = layout::lanePanelX(m, 0u);
        const layout::LanePanelX l1 = layout::lanePanelX(m, 1u);
        CHECK(l0.right <= l1.left);
    }
}

void testScaleClampHolds() {
    CHECK_NEAR(layout::computeScale(640, 480), 1.0f, 0.0001f);
    CHECK_NEAR(layout::computeScale(1280, 960), 2.0f, 0.0001f);
    CHECK_NEAR(layout::computeScale(1920, 480), 1.0f, 0.0001f);
    CHECK_NEAR(layout::computeScale(640, 1920), 1.0f, 0.0001f);
    CHECK(layout::computeScale(16, 12) >= layout::kScaleMin);
    CHECK(layout::computeScale(65536, 49152) <= layout::kScaleMax);
    CHECK_NEAR(layout::computeScale(0, 0), 1.0f, 0.0001f);
}

void testHistoryCapacityIsAHardCeiling() {
    const layout::Metrics tiny = layout::compute(0, 0, 4096, 3072, 4);
    CHECK(tiny.renderable);
    CHECK(tiny.visibleRows <= 4);
    const layout::Metrics none = layout::compute(0, 0, 1280, 960, 0);
    CHECK_EQ_INT(none.visibleRows, 0);
}

void testFloorFloatMatchesTruncationTowardsNegativeInfinity() {
    CHECK_NEAR(layout::floorFloat(0.0f), 0.0f, 0.0001f);
    CHECK_NEAR(layout::floorFloat(3.0f), 3.0f, 0.0001f);
    CHECK_NEAR(layout::floorFloat(3.9f), 3.0f, 0.0001f);
    CHECK_NEAR(layout::floorFloat(12.999f), 12.0f, 0.0001f);
    CHECK_NEAR(layout::floorFloat(-0.5f), -1.0f, 0.0001f);
    CHECK_NEAR(layout::floorFloat(-3.0f), -3.0f, 0.0001f);
    CHECK_NEAR(layout::floorFloat(-3.1f), -4.0f, 0.0001f);
}

struct TestCase {
    const char* name;
    void (*fn)();
};

const TestCase kTests[] = {
    {"live box is separated from history", testLiveBoxIsSeparatedFromHistory},
    {"every visible row fits inside the history box",
     testEveryVisibleRowFitsInsideTheHistoryBox},
    {"stack never escapes the band", testStackNeverEscapesTheBand},
    {"row count accounts for the gap", testRowCountAccountsForTheGap},
    {"row count is stable across every 4:3 resolution",
     testRowCountIsStableAcrossEveryFourThreeResolution},
    {"live and history share their columns",
     testLiveAndHistoryShareTheirColumns},
    {"lanes never overlap", testLanesNeverOverlap},
    {"panels follow the destination offset",
     testPanelsFollowTheDestinationOffset},
    {"degenerate destinations refuse to render",
     testDegenerateDestinationsRefuseToRender},
    {"non-4:3 destinations stay coherent",
     testNonFourThreeDestinationsStayCoherent},
    {"scale clamp holds", testScaleClampHolds},
    {"history capacity is a hard ceiling", testHistoryCapacityIsAHardCeiling},
    {"floorFloat matches truncation towards negative infinity",
     testFloorFloatMatchesTruncationTowardsNegativeInfinity},
};

}

int main() {
    const int testCount = static_cast<int>(sizeof(kTests) / sizeof(kTests[0]));
    int passedTests = 0;
    for (int i = 0; i < testCount; ++i) {
        const int before = g_failures;
        kTests[i].fn();
        if (g_failures == before) {
            ++passedTests;
            std::printf("ok   %s\n", kTests[i].name);
        } else {
            std::printf("FAIL %s\n", kTests[i].name);
        }
    }
    std::printf("\ninput_display_layout: %d/%d tests passed (%d checks, "
                "%d failures)\n",
                passedTests, testCount, g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
