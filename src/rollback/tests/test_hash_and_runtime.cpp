#include "TestFramework.hpp"
#include "rollback/RollbackRuntime.hpp"
#include "rollback/SyncHash.hpp"

using namespace qoh99;
using namespace qoh99::netplay;

static void stageBaseline(BufferMemoryView& mem) {
    mem.set<GameAddr>(addr::kFighterP1 + 164, 0x700000);
    mem.set<GameAddr>(addr::kFighterP2 + 164, 0x701000);
    mem.set<std::uint32_t>(addr::kRngSeedGameplay, 42);
    mem.set<int>(addr::kSceneCurrent, 2);
    mem.set<int>(addr::kMatchSubstate, 0);
}

TEST(hash_identical_states_zero_diff) {
    BufferMemoryView mem;
    stageBaseline(mem);
    SavestateProfile p;
    SyncHash h(p);
    Savestate a, b;
    CHECK(p.save(mem, a));
    CHECK(p.save(mem, b));
    CHECK_EQ(h.combined(a, HashProfile::GameplaySync), h.combined(b, HashProfile::GameplaySync));
    CHECK(!h.firstDiff(a, b, HashProfile::GameplaySync).differs);
    CHECK(!h.firstDiff(a, b, HashProfile::FullDebugWide).differs);
}

TEST(hash_gameplay_change_detected_with_region_and_offset) {
    BufferMemoryView mem;
    stageBaseline(mem);
    SavestateProfile p;
    SyncHash h(p);
    Savestate a, b;
    CHECK(p.save(mem, a));
    mem.set<std::uint32_t>(addr::kHealthArray, 55);
    CHECK(p.save(mem, b));

    auto fd = h.firstDiff(a, b, HashProfile::GameplaySync);
    CHECK(fd.differs);
    CHECK(std::string(fd.region) == "dw_mid2b_tail");
    CHECK_EQ(fd.offset, addr::kHealthArray - 0x6254E0u);
    CHECK(h.combined(a, HashProfile::GameplaySync) != h.combined(b, HashProfile::GameplaySync));
}

TEST(hash_excludes_opening_drive_cursor_but_tracks_exact_rng_selector) {
    BufferMemoryView mem;
    stageBaseline(mem);
    SavestateProfile p;
    SyncHash h(p);
    Savestate baseline, driveChanged, selectorChanged;
    CHECK(p.save(mem, baseline));

    mem.set<std::uint32_t>(0x4ABB28u, 26u);
    CHECK(p.save(mem, driveChanged));
    CHECK(!h.firstDiff(
        baseline, driveChanged, HashProfile::GameplaySync).differs);
    CHECK(!h.firstDiff(
        baseline, driveChanged, HashProfile::FullDebugWide).differs);

    mem.set<std::uint32_t>(addr::kRngSelector, 1u);
    CHECK(p.save(mem, selectorChanged));
    const auto diff =
        h.firstDiff(baseline, selectorChanged, HashProfile::GameplaySync);
    CHECK(diff.differs);
    CHECK(std::string(diff.region) == "rng_selector");
    CHECK_EQ(diff.offset, 0u);
}

TEST(hash_cosmetic_change_ignored_by_gameplay_sync) {
    BufferMemoryView mem;
    stageBaseline(mem);
    SavestateProfile p;
    SyncHash h(p);
    Savestate a, b;
    CHECK(p.save(mem, a));
    mem.set<std::uint32_t>(0x87D100, 0x1234);
    CHECK(p.save(mem, b));

    CHECK(!h.firstDiff(a, b, HashProfile::GameplaySync).differs);
    CHECK(h.firstDiff(a, b, HashProfile::RenderCosmetic).differs);
    CHECK(h.firstDiff(a, b, HashProfile::FullDebugWide).differs);
    CHECK_EQ(h.combined(a, HashProfile::GameplaySync),
             h.combined(b, HashProfile::GameplaySync));
}

TEST(hash_child_pointer_and_bytes_included) {
    BufferMemoryView mem;
    stageBaseline(mem);
    SavestateProfile p;
    SyncHash h(p);
    Savestate a, b;
    CHECK(p.save(mem, a));
    mem.set<std::uint32_t>(0x700000 + 268, 3);
    CHECK(p.save(mem, b));
    auto fd = h.firstDiff(a, b, HashProfile::GameplaySync);
    CHECK(fd.differs);
    CHECK(std::string(fd.region) == "cmd_p1");
    CHECK_EQ(fd.offset, 268u);
}

namespace {
struct SimGame {
    BufferMemoryView& mem;
    void tick() {
        auto s = mem.get<std::uint32_t>(addr::kRngSeedGameplay);
        mem.set<std::uint32_t>(addr::kRngSeedGameplay, s * 156683941u + 1u);
        auto c = mem.get<std::uint32_t>(addr::kFighterP1 + 48);
        auto in = mem.get<std::uint32_t>(addr::kNetRemoteInput);
        mem.set<std::uint32_t>(addr::kFighterP1 + 48, c + 1 + in);
    }
};
}

TEST(rollback_runtime_k_frame_replay_is_deterministic) {
    BufferMemoryView mem;
    stageBaseline(mem);
    SimGame game{ mem };

    auto feed = [&](std::uint32_t frame) {
        mem.set<std::uint32_t>(addr::kNetRemoteInput,
                               InputSystem::aiInput(frame, 1).mask & 0xF);
    };
    RollbackRuntime rt(mem, [&] { game.tick(); }, feed);

    for (std::uint32_t f = 0; f < 5; ++f) {
        CHECK(rt.saveFrame(f));
        feed(f);
        game.tick();
    }
    Savestate truth;
    CHECK(rt.profile().save(mem, truth));

    CHECK_EQ(rt.rollbackAndReplay(2, 3), 3);
    Savestate replayed;
    CHECK(rt.profile().save(mem, replayed));
    SyncHash h(rt.profile());
    auto fd = h.firstDiff(truth, replayed, HashProfile::GameplaySync);
    if (fd.differs)
        testfw::reportFail(__FILE__, __LINE__,
            std::string("replay diverged in ") + fd.region);
}

TEST(rollback_runtime_respects_phase_gates) {
    BufferMemoryView mem;
    stageBaseline(mem);
    mem.set<int>(addr::kMatchSubstate, 3);
    RollbackRuntime rt(mem, [] {}, nullptr);
    CHECK(!rt.saveFrame(0));
    CHECK_EQ(rt.rollbackAndReplay(0, 1), 0);

    mem.set<int>(addr::kMatchSubstate, 0);
    CHECK(rt.saveFrame(1));
}

TEST(rollback_runtime_stops_on_gameplay_exit) {
    BufferMemoryView mem;
    stageBaseline(mem);
    int ticks = 0;
    RollbackRuntime rt(mem, [&] {
        if (++ticks == 2)
            mem.set<int>(addr::kMatchSubstate, 5);
    }, nullptr);
    CHECK(rt.saveFrame(0));
    int replayed = rt.rollbackAndReplay(0, 4);
    CHECK_EQ(replayed, 2);
    CHECK(!rt.render().active());
    CHECK(!rt.audio().active());
}

TEST(rollback_suppressors_are_scoped) {
    RenderSuppressor r;
    CHECK(!r.active());
    {
        RenderSuppressor::Scope s(r);
        CHECK(r.active());
    }
    CHECK(!r.active());
}

TEST(RenderSuppressedDuringReplay) {
    BufferMemoryView mem;
    stageBaseline(mem);
    RollbackRuntime* rtp = nullptr;
    bool sawRenderSuppressed = false, sawAudioSuppressed = false;
    RollbackRuntime rt(mem, [&] {
        if (rtp && rtp->render().active()) sawRenderSuppressed = true;
        if (rtp && rtp->audio().active())  sawAudioSuppressed = true;
    }, nullptr);
    rtp = &rt;
    CHECK(rt.saveFrame(0));
    CHECK(!rt.render().active());
    CHECK_EQ(rt.rollbackAndReplay(0, 3), 3);
    CHECK(sawRenderSuppressed);
    CHECK(sawAudioSuppressed);
    CHECK(!rt.render().active());
    CHECK(!rt.audio().active());
}

TEST(rollback_input_override_gating) {
    InputOverride ov;
    ov.setOverride(0, { input_mask::kBtn1 });
    CHECK_EQ(ov.filter(0, 0xAA), 0xAAu);
    ov.setActive(true);
    CHECK_EQ(ov.filter(0, 0xAA), input_mask::kBtn1);
    CHECK_EQ(ov.filter(1, 0xBB), 0u);
    ov.setActive(false);
    CHECK_EQ(ov.filter(0, 0xCC), 0xCCu);
}
