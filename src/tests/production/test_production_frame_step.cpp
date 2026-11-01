#include "TestFramework.hpp"

namespace {

enum class Mode { Natural, Deterministic, PacingShim };
inline bool nonNatural(Mode m) { return m != Mode::Natural; }

bool shouldCompareFrame(Mode m, bool truthAdvanced) {
    if (nonNatural(m) && !truthAdvanced) return false;
    return true;
}

bool replayAdvances(Mode m, bool truthAdvanced, bool pacingStateRestored) {
    if (!truthAdvanced) return false;
    if (m == Mode::Deterministic) return pacingStateRestored;
    return false;
}

}

TEST(ReplayCompareOnlyWhenTruthAdvanced) {
    CHECK(!shouldCompareFrame(Mode::Deterministic, false));
    CHECK( shouldCompareFrame(Mode::Deterministic, true));
    CHECK(!shouldCompareFrame(Mode::PacingShim,    false));
    CHECK( shouldCompareFrame(Mode::PacingShim,    true));
    CHECK(shouldCompareFrame(Mode::Natural, false));
    CHECK(shouldCompareFrame(Mode::Natural, true));
}

TEST(ReplayMustAdvanceWhenTruthAdvanced) {
    CHECK(replayAdvances(Mode::Deterministic, true,  true));
    CHECK(!replayAdvances(Mode::Deterministic, true, false));
    CHECK(!replayAdvances(Mode::Deterministic, false, true));
}
