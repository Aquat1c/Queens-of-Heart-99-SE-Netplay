#include "TestFramework.hpp"
#include "qoh99/AddressMap.hpp"
#include "qoh99/GameplayRegionCatalog.hpp"
#include "qoh99/generated/GameplayRegions.hpp"
#include "qoh99/FighterPool.hpp"
#include "qoh99/HudSystem.hpp"
#include "qoh99/TimingSystem.hpp"

using namespace qoh99;

TEST(effect_pool_covers_all_thirty_match_loop_slots) {
    BufferMemoryView memory;
    const ActorPool pool = ActorPool::effectPool(memory);
    CHECK_EQ(pool.count(), 30);
    CHECK_EQ(
        pool.end(),
        addr::kEffectPool + 30u * fighter_field::kSize);
    CHECK_EQ(pool.end(), 0x6234E8u);
}

namespace {

struct ExpectedRegion {
    const char* name;
    GameAddr address;
    std::uint32_t size;
    GameplayRelevance relevance;
};

const RegionInfo* checkRegion(const ExpectedRegion& expected) {
    const RegionInfo* actual = GameplayRegionCatalog::find(expected.name);
    CHECK(actual != nullptr);
    if (actual) {
        CHECK_EQ(actual->address, expected.address);
        CHECK_EQ(actual->size, expected.size);
        CHECK(actual->relevance == expected.relevance);
    }
    return actual;
}

template <std::size_t N>
void checkExactTiling(const ExpectedRegion (&expected)[N]) {
    const RegionInfo* previous = nullptr;
    for (const ExpectedRegion& row : expected) {
        const RegionInfo* actual = checkRegion(row);
        if (previous && actual)
            CHECK_EQ(previous->address + previous->size, actual->address);
        previous = actual;
    }
}

}

TEST(catalog_has_all_five_relevance_classes) {
    std::size_t n = 0;
    const RegionInfo* r = GameplayRegionCatalog::regions(n);
    int counts[5] = { 0, 0, 0, 0, 0 };
    for (std::size_t i = 0; i < n; ++i)
        ++counts[static_cast<int>(r[i].relevance)];
    CHECK_EQ(n, 97u);
    CHECK_EQ(counts[static_cast<int>(GameplayRelevance::DeterministicGameplay)], 45);
    CHECK_EQ(counts[static_cast<int>(GameplayRelevance::RenderCosmetic)], 18);
    CHECK_EQ(counts[static_cast<int>(GameplayRelevance::WallClockPacing)], 5);
    CHECK_EQ(counts[static_cast<int>(GameplayRelevance::ExternalResource)], 23);
    CHECK_EQ(counts[static_cast<int>(GameplayRelevance::Unknown)], 6);
}

TEST(catalog_rng_selector_excludes_adjacent_boot_and_pacing_state) {
    const ExpectedRegion tile[] = {
        { "frame_time_ring_index", 0x4ABB20u, 0x4u,
          GameplayRelevance::WallClockPacing },
        { "rng_selector", 0x4ABB24u, 0x4u,
          GameplayRelevance::DeterministicGameplay },
        { "opening_drive_cursor", 0x4ABB28u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "unused_combat_zero", 0x4ABB2Cu, 0x4u,
          GameplayRelevance::Unknown },
    };
    checkExactTiling(tile);

    CHECK(GameplayRegionCatalog::find("rng_selector")->address
          == addr::kRngSelector);
    CHECK(GameplayRegionCatalog::find("rng_selector")->size == 4u);
}

TEST(catalog_classified_regions_disjoint) {
    CHECK_EQ(GameplayRegionCatalog::firstOverlapAmongClassified(), -1);
}

TEST(catalog_gameplay_regions_never_cover_unsafe_globals) {
    std::size_t n = 0, an = 0;
    const RegionInfo* r = GameplayRegionCatalog::regions(n);
    const AddressEntry* e = AddressMap::entries(an);
    for (std::size_t i = 0; i < n; ++i) {
        if (r[i].relevance != GameplayRelevance::DeterministicGameplay)
            continue;
        for (std::size_t k = 0; k < an; ++k) {
            if (e[k].cls != MemClass::WallClockPacing && e[k].cls != MemClass::UnsafeOSPointer)
                continue;
            if (e[k].va >= r[i].address && e[k].va < r[i].address + r[i].size)
                testfw::reportFail(__FILE__, __LINE__,
                    std::string("gameplay region ") + r[i].name +
                    " covers unsafe global " + e[k].name);
        }
    }
}

TEST(catalog_gameplay_hashed_globals_are_in_deterministic_regions) {
    const GameAddr mustBeGameplay[] = {
        addr::kFighterP1, addr::kFighterP2, addr::kHealthArray, addr::kScoreArray,
        addr::kRngSeedGameplay, addr::kRngSelector, addr::kMatchSubstate,
        addr::kActorClonePool, addr::kEffectPool, addr::kShadowPool,
        addr::kHudComboDigits,
    };
    std::size_t n = 0;
    const RegionInfo* r = GameplayRegionCatalog::regions(n);
    for (GameAddr va : mustBeGameplay) {
        bool covered = false;
        for (std::size_t i = 0; i < n; ++i)
            if (r[i].relevance == GameplayRelevance::DeterministicGameplay &&
                va >= r[i].address && va < r[i].address + r[i].size)
                covered = true;
        if (!covered) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "gameplay global 0x%X not in a gameplay region", va);
            testfw::reportFail(__FILE__, __LINE__, buf);
        }
    }
}

TEST(catalog_arena_and_camera_are_saved_but_cosmetic) {
    const GameAddr savedCosmetic[] = {
        addr::kArenaBlock0, addr::kArenaBlock1, addr::kArenaActivePtr,
        addr::kArenaSelector, addr::kCameraZoomA,
    };
    std::size_t n = 0;
    const RegionInfo* r = GameplayRegionCatalog::regions(n);
    for (GameAddr va : savedCosmetic) {
        const RegionInfo* hit = nullptr;
        for (std::size_t i = 0; i < n; ++i)
            if (va >= r[i].address && va < r[i].address + r[i].size)
                hit = &r[i];
        if (!hit) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "0x%X not covered by any captured region", va);
            testfw::reportFail(__FILE__, __LINE__, buf);
        } else {
            CHECK(hit->relevance == GameplayRelevance::RenderCosmetic);
        }
    }
}

TEST(catalog_wallclock_and_external_classified_correctly) {
    std::size_t wn = 0;
    const TimingSystem::WallClockRegion* w = TimingSystem::wallClockRegions(wn);
    std::size_t n = 0;
    const RegionInfo* r = GameplayRegionCatalog::regions(n);
    for (std::size_t i = 0; i < wn; ++i) {
        bool found = false;
        for (std::size_t k = 0; k < n; ++k)
            if (r[k].relevance == GameplayRelevance::WallClockPacing &&
                w[i].base >= r[k].address && w[i].base < r[k].address + r[k].size)
                found = true;
        CHECK(found);
    }
    const RegionInfo* chat = GameplayRegionCatalog::find("chat_callback_ptr");
    CHECK(chat && chat->relevance == GameplayRelevance::ExternalResource);
    CHECK(chat->address == addr::kChatCallbackPtr);

    const AddressEntry* appActive = AddressMap::find("appActiveFlag");
    CHECK(appActive && appActive->cls == MemClass::UnsafeOSPointer);
}

TEST(catalog_hud_and_window_span_is_exactly_tiled) {
    const ExpectedRegion tile[] = {
        { "script_bank_selection", 0x623900u, 0x40u,
          GameplayRelevance::DeterministicGameplay },
        { "hud_text", 0x623940u, 0x104u,
          GameplayRelevance::RenderCosmetic },
        { "dw_mid2b_fighter", 0x623A44u, 0x9BCu,
          GameplayRelevance::DeterministicGameplay },
        { "window_primary", 0x624400u, 0x4C8u,
          GameplayRelevance::ExternalResource },
        { "window_adjacent_gameplay", 0x6248C8u, 0x4u,
          GameplayRelevance::DeterministicGameplay },
        { "window_audio_mode", 0x6248CCu, 0x4u,
          GameplayRelevance::ExternalResource },
        { "gap_after_primary_window", 0x6248D0u, 0x10u,
          GameplayRelevance::Unknown },
        { "dw_mid2b_ai_vars", 0x6248E0u, 0x100u,
          GameplayRelevance::DeterministicGameplay },
        { "net_message_cursor", 0x6249E0u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "gap_before_secondary_window", 0x6249E4u, 0x1Cu,
          GameplayRelevance::Unknown },
        { "window_secondary", 0x624A00u, 0x4C8u,
          GameplayRelevance::ExternalResource },
        { "gap_after_secondary_window", 0x624EC8u, 0x8u,
          GameplayRelevance::Unknown },
        { "dw_mid2b_tail_head", 0x624ED0u, 0x60Cu,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_object_id_6254DC", 0x6254DCu, 0x4u,
          GameplayRelevance::ExternalResource },
        { "dw_mid2b_tail", 0x6254E0u, 0x12E0u,
          GameplayRelevance::DeterministicGameplay },
    };
    checkExactTiling(tile);
    const RegionInfo* tail = GameplayRegionCatalog::find("dw_mid2b_tail");
    CHECK(tail && tail->address + tail->size == 0x6267C0u);
}

TEST(catalog_renderer_ids_are_external_holes_in_gameplay_spans) {
    const ExpectedRegion highDataTile[] = {
        { "dw_hi_head", 0x626800u, 0xCA0u,
          GameplayRelevance::DeterministicGameplay },
        { "ddraw_error_text", 0x6274A0u, 0x30u,
          GameplayRelevance::ExternalResource },
        { "gap_before_renderer_id_6275A4", 0x6274D0u, 0xD0u,
          GameplayRelevance::Unknown },
        { "dw_hi_gameplay_gate", 0x6275A0u, 0x4u,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_6275A4", 0x6275A4u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "dw_hi", 0x6275A8u, 0x6098u,
          GameplayRelevance::DeterministicGameplay },
    };
    checkExactTiling(highDataTile);

    const ExpectedRegion matchSubTile[] = {
        { "matchsub_gameplay_00", 0x8716C0u, 0x40u,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_871700", 0x871700u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "matchsub_gameplay_01", 0x871704u, 0x1A0u,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_8718A4", 0x8718A4u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "matchsub_gameplay_02", 0x8718A8u, 0x4u,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_8718AC", 0x8718ACu, 0x4u,
          GameplayRelevance::ExternalResource },
        { "matchsub_gameplay_03", 0x8718B0u, 0x4u,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_8718B4", 0x8718B4u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "matchsub_gameplay_04", 0x8718B8u, 0x8u,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_8718C0", 0x8718C0u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "matchsub_gameplay_05", 0x8718C4u, 0x48u,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_87190C", 0x87190Cu, 0x4u,
          GameplayRelevance::ExternalResource },
        { "matchsub_gameplay_06", 0x871910u, 0xCu,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_87191C", 0x87191Cu, 0x4u,
          GameplayRelevance::ExternalResource },
        { "matchsub_gameplay_07", 0x871920u, 0x34u,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_871954", 0x871954u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "matchsub_gameplay_08", 0x871958u, 0x1168u,
          GameplayRelevance::DeterministicGameplay },
    };
    checkExactTiling(matchSubTile);

    const ExpectedRegion hudCounterTile[] = {
        { "hud_counters_a", 0x8A5C00u, 0x68u,
          GameplayRelevance::DeterministicGameplay },
        { "chat_callback_ptr", 0x8A5C68u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "hud_counters_b_head", 0x8A5C6Cu, 0xCu,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_surface_id_8A5C78", 0x8A5C78u, 0x4u,
          GameplayRelevance::ExternalResource },
        { "hud_counters_b", 0x8A5C7Cu, 0x184u,
          GameplayRelevance::DeterministicGameplay },
        { "ai_player_state", 0x8A5E00u, 0x8B0u,
          GameplayRelevance::DeterministicGameplay },
    };
    checkExactTiling(hudCounterTile);
}

TEST(catalog_ai_banks_and_full_vm_end_before_network_callbacks) {
    const ExpectedRegion aiTile[] = {
        { "ai_script_banks", 0x62D640u, 0x1540u,
          GameplayRelevance::ExternalResource },
        { "ai_script_vm", 0x62EB80u, 0x40024u,
          GameplayRelevance::DeterministicGameplay },
        { "net_callback_ptrs", 0x66EBA4u, 0x2A0u,
          GameplayRelevance::ExternalResource },
    };
    checkExactTiling(aiTile);
    CHECK(GameplayRegionCatalog::find("ai_script_cursor") == nullptr);
    CHECK(GameplayRegionCatalog::find("gap_after_data_working") == nullptr);
    CHECK(GameplayRegionCatalog::find("matchsub_block") == nullptr);
}

TEST(catalog_pointer_children_are_command_history_objects) {
    std::size_t n = 0;
    const PointerChildInfo* c = GameplayRegionCatalog::pointerChildren(n);
    CHECK_EQ(n, 4u);
    CHECK_EQ(c[0].pointerAddress, addr::kFighterP1 + 164u);
    CHECK_EQ(c[1].pointerAddress, addr::kFighterP2 + 164u);
    CHECK(!c[0].nullable);
    CHECK(c[2].nullable);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK_EQ(c[i].pointeeSize, 0x200u);
        CHECK(c[i].relevance == GameplayRelevance::DeterministicGameplay);
    }
}

TEST(catalog_evidence_present_everywhere) {
    std::size_t n = 0;
    const RegionInfo* r = GameplayRegionCatalog::regions(n);
    for (std::size_t i = 0; i < n; ++i) {
        CHECK(r[i].evidence && r[i].evidence[0]);
        if (r[i].relevance == GameplayRelevance::Unknown)
            CHECK(r[i].confidence == Confidence::Unknown);
    }
}

TEST(catalog_agrees_with_hud_feeder_regions) {
    std::size_t n = 0;
    const HudSystem::FeederRegion* f = HudSystem::feederRegions(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::string(f[i].name) == "hud_display_head"
            || std::string(f[i].name) == "hud_display_mid"
            || std::string(f[i].name) == "hud_display_tail") {
            const RegionInfo* r =
                GameplayRegionCatalog::find(f[i].name);
            CHECK(r && r->address == f[i].base);
            CHECK(r && r->size == f[i].size);
            CHECK(r && r->relevance == f[i].relevance);
        }
    }
}

TEST(catalog_hashes_native_finish_state_inside_cosmetic_hud_band) {
    const ExpectedRegion tile[] = {
        { "hud_display_head", 0x87D000u, 0x37C4u,
          GameplayRelevance::RenderCosmetic },
        { "round_intro_state", 0x8807C4u, 0x4u,
          GameplayRelevance::DeterministicGameplay },
        { "hud_display_mid", 0x8807C8u, 0x1C4u,
          GameplayRelevance::RenderCosmetic },
        { "finish_fx_state", 0x88098Cu, 0x4u,
          GameplayRelevance::DeterministicGameplay },
        { "hud_display_tail", 0x880990u, 0x670u,
          GameplayRelevance::RenderCosmetic },
    };
    checkExactTiling(tile);
    const RegionInfo* state =
        GameplayRegionCatalog::find("finish_fx_state");
    CHECK(state && state->address + state->size == 0x880990u);
}

TEST(catalog_hashes_round_announcement_control_state) {
    const RegionInfo* audio =
        GameplayRegionCatalog::find("round_banner_audio_state");
    CHECK(audio && audio->address == 0x4AA400u);
    CHECK(audio && audio->size == 0x4u);
    CHECK(audio && audio->relevance
        == GameplayRelevance::DeterministicGameplay);

    const RegionInfo* rotation =
        GameplayRegionCatalog::find("round_intro_rotation");
    CHECK(rotation && rotation->address == 0x4AA43Cu);
    CHECK(rotation && rotation->size == 0x4u);
    CHECK(rotation && rotation->relevance
        == GameplayRelevance::RenderCosmetic);

    const generated_model::GameplayRegion* generatedRotation = nullptr;
    for (const auto& region : generated_model::kGameplayRegions) {
        if (std::string(region.name) == "round_intro_rotation")
            generatedRotation = &region;
    }
    CHECK(generatedRotation != nullptr);
    if (generatedRotation) {
        CHECK(generatedRotation->semantic
              == generated_model::RegionSemantic::Render);
        CHECK(generatedRotation->safety
              == generated_model::RegionSafety::PlainBytes);
        CHECK(generatedRotation->policy.capture);
        CHECK(generatedRotation->policy.restore);
        CHECK(!generatedRotation->policy.gameplayHash);
        CHECK(generatedRotation->policy.renderHash);
    }

    std::size_t regionCount = 0;
    const RegionInfo* regions =
        GameplayRegionCatalog::regions(regionCount);
    for (GameAddr address = 0x4AA43Cu; address < 0x4AA440u; ++address) {
        std::size_t covering = 0;
        for (std::size_t i = 0; i < regionCount; ++i) {
            if (address >= regions[i].address
                && address < regions[i].address + regions[i].size) {
                ++covering;
                CHECK(std::string(regions[i].name)
                      == "round_intro_rotation");
            }
        }
        CHECK_EQ(covering, 1u);
    }
    for (const GameAddr adjacent : { 0x4AA438u, 0x4AA440u }) {
        std::size_t covering = 0;
        for (std::size_t i = 0; i < regionCount; ++i)
            if (adjacent >= regions[i].address
                && adjacent < regions[i].address + regions[i].size)
                ++covering;
        CHECK_EQ(covering, 0u);
    }

    const RegionInfo* fps =
        GameplayRegionCatalog::find("fps_counter_digit");
    CHECK(fps && fps->address == 0x49B520u);
    CHECK(fps && fps->size == 0x10u);
    CHECK(fps && fps->relevance
        == GameplayRelevance::RenderCosmetic);

    const RegionInfo* visual =
        GameplayRegionCatalog::find("camera_zoom_visual");
    CHECK(visual && visual->address == 0x49B530u);
    CHECK(visual && visual->size == 0x8u);
    CHECK(visual && visual->relevance
        == GameplayRelevance::RenderCosmetic);

    const RegionInfo* timer =
        GameplayRegionCatalog::find("round_intro_control");
    CHECK(timer && timer->address == 0x49B538u);
    CHECK(timer && timer->size == 0xCu);
    CHECK(timer && timer->relevance
        == GameplayRelevance::DeterministicGameplay);

    const RegionInfo* finish =
        GameplayRegionCatalog::find("finish_banner_descriptor");
    CHECK(finish && finish->address == 0x49B548u);
    CHECK(finish && finish->size == 0x18u);
    CHECK(finish && finish->relevance
        == GameplayRelevance::RenderCosmetic);
}

TEST(catalog_aux_actor_pool_slots_05_29_exact_region_and_placement) {
    const RegionInfo* r =
        GameplayRegionCatalog::find("match_aux_actor_pool_slots_05_29");
    CHECK(r != nullptr);
    CHECK(r && r->address == 0x8737CCu);
    CHECK(r && r->size == 0x959Cu);
    CHECK(r && r->size == 25u * 0x5FCu);
    CHECK(r && r->address + r->size == 0x87CD68u);
    CHECK(r && r->relevance == GameplayRelevance::DeterministicGameplay);

    std::size_t n = 0;
    const RegionInfo* all = GameplayRegionCatalog::regions(n);
    std::size_t auxIdx = n, tailIdx = n, ringIdx = n;
    for (std::size_t i = 0; i < n; ++i) {
        const std::string name = all[i].name;
        if (name == "match_aux_actor_pool_slots_05_29") auxIdx = i;
        if (name == "actor_clone_pool_tail") tailIdx = i;
        if (name == "frame_time_ring") ringIdx = i;
    }
    CHECK(tailIdx < auxIdx);
    CHECK(auxIdx < ringIdx);
}

TEST(catalog_rina_crash_actors_exact_region_and_relevance) {
    const RegionInfo* r = GameplayRegionCatalog::find("rina_crash_actors");
    CHECK(r != nullptr);
    CHECK(r && r->address == 0x8A3740u);
    CHECK(r && r->size == 0x17F0u);
    CHECK(r && r->size == 4u * 0x5FCu);
    CHECK(r && r->address + r->size == 0x8A4F30u);
    CHECK(r && r->relevance == GameplayRelevance::DeterministicGameplay);
    CHECK(r && r->address + 1u * 0x5FCu == 0x8A3D3Cu);
    CHECK(r && r->address + 2u * 0x5FCu == 0x8A4338u);
    CHECK(r && r->address + 3u * 0x5FCu == 0x8A4934u);
}

TEST(catalog_rina_crash_actors_do_not_overlap_classified_regions) {
    std::size_t n = 0;
    const RegionInfo* all = GameplayRegionCatalog::regions(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::string(all[i].name) == "rina_crash_actors") continue;
        const GameAddr a0 = all[i].address;
        const GameAddr a1 = a0 + all[i].size;
        CHECK(a1 <= 0x8A3740u || a0 >= 0x8A4F30u);
    }
    CHECK_EQ(GameplayRegionCatalog::firstOverlapAmongClassified(), -1);
}

TEST(catalog_aux_actor_pool_does_not_overlap_classified_regions) {
    std::size_t n = 0;
    const RegionInfo* all = GameplayRegionCatalog::regions(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (std::string(all[i].name) == "match_aux_actor_pool_slots_05_29")
            continue;
        const GameAddr a0 = all[i].address;
        const GameAddr a1 = a0 + all[i].size;
        CHECK(a1 <= 0x8737CCu || a0 >= 0x87CD68u);
    }
    CHECK_EQ(GameplayRegionCatalog::firstOverlapAmongClassified(), -1);
}
