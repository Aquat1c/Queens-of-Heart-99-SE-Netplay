#include "qoh99/GameplayRegionCatalog.hpp"
#include <cstring>

namespace qoh99 {

using C = Confidence;
using R = GameplayRelevance;

// Rows are validated against tools/qoh99_annotations.py REGION_CATALOG; keep the literal { "name", 0xADDR, 0xSIZE, GameplayRelevance::X } shape.
static const RegionInfo kRegions[] = {
    { "fighterP3_slot", 0x4ECB40, 0x5FC, GameplayRelevance::DeterministicGameplay,
      C::RuntimeVerified, "P3/striker static fighter slot (0x5FC), just below the arena" },
    // Exact pool extent = 120 x 0x5FC = 0x2CE20 (sub_42DBE0).
    { "round_banner_state", 0x526EE0, 0xC0, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "round-win counters + announcer/banner controller + petal params "
                          "(sub_429730/sub_429810/sub_4295D0; tick reads 526F50/526F28 per frame)" },
    { "actor_clone_pool", 0x526FA0, 0x2CE20, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "sub_42DDB0/sub_42DBE0 pool: exactly 120 x 0x5FC objects (0x2CE20)" },
    { "dw_lo1", 0x613000, 0x47C0, GameplayRelevance::DeterministicGameplay,
      C::RuntimeVerified, ".data working 0x613000..0x6177C0 (config/effect pool head)" },
    { "dw_lo2", 0x617800, 0xBCE0, GameplayRelevance::DeterministicGameplay,
      C::RuntimeVerified, ".data working 0x617800..0x6234E0 (effect pool 0x618160)" },
    { "dw_mid", 0x623520, 0x1D0, GameplayRelevance::DeterministicGameplay,
      C::RuntimeVerified, ".data working 0x623520..0x6236F0 (config snapshot)" },
    { "screen_fx_state", 0x623720, 0x14, GameplayRelevance::RenderCosmetic,
      C::StaticAnalysis, "screen flash/fade presentation state; local profile +68 may differ" },
    // Do not widen the cosmetic carve: 0x623734 begins the gameplay priority
    // permutation, and the following petal state consumes gameplay RNG.
    { "dw_mid2a", 0x623734, 0x1CC, GameplayRelevance::DeterministicGameplay,
      C::RuntimeVerified, ".data working 0x623734..0x623900 (fighter table region)" },
    { "script_bank_selection", 0x623900, 0x40,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "AI script-bank IDs dword_623920[4] + gameplay countdown dword_623930" },
    { "dw_mid2b_fighter", 0x623A44, 0x9BC,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      ".data working after the exact 260-byte HUD buffer through P1 fighter" },
    { "window_primary", 0x624400, 0x4C8,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "ClassName window/GDI/IME object: sub_4909E0/490AE0/490D50/490ED0; "
      "HINSTANCE +116, HWND +120, HDCs +156/+160, HBITMAP +164, "
      "HPALETTE +1192, HIMC +1196, OS client coordinates +1208" },
    { "window_adjacent_gameplay", 0x6248C8, 0x4,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "gameplay freeze/update gate dword_6248C8; decremented and consumed by "
      "match transition/update branches" },
    { "window_audio_mode", 0x6248CC, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "voice/audio resource-mode scalar dword_6248CC; preserve live" },
    { "gap_after_primary_window", 0x6248D0, 0x10,
      GameplayRelevance::Unknown, C::Unknown,
      "unreferenced padding between primary window/audio state and AI vars" },
    { "dw_mid2b_ai_vars", 0x6248E0, 0x100,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "exact 64-DWORD AI-script situation/global array; sub_461550/sub_44CFF0" },
    { "net_message_cursor", 0x6249E0, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "malloc-backed NetMessage.Txt parser cursor: sub_46E650 assigns it, "
      "sub_46E200 advances/dereferences it, owner is freed without clearing" },
    { "gap_before_secondary_window", 0x6249E4, 0x1C,
      GameplayRelevance::Unknown, C::Unknown,
      "unreferenced padding between the 64-DWORD AI array/transient parser "
      "cursor and byte_624A00; no declaration or xref in either build" },
    { "window_secondary", 0x624A00, 0x4C8,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "secondary byte_624A00 window/GDI/IME object with the same RE-verified "
      "handle layout; constructed by WinMain and destroyed by its WndProc" },
    { "gap_after_secondary_window", 0x624EC8, 0x8,
      GameplayRelevance::Unknown, C::Unknown,
      "unreferenced padding after the exact secondary window object" },
    { "dw_mid2b_tail_head", 0x624ED0, 0x60C,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "fighter/template state before renderer object-ID slot dword_6254DC" },
    { "renderer_object_id_6254DC", 0x6254DC, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "sub_459250 renderer object-table index; preserve live across rollback/"
      "display-resource generations" },
    { "dw_mid2b_tail", 0x6254E0, 0x12E0,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "fighter/template tables, health and score through 0x6267C0" },
    { "dw_hi_head", 0x626800, 0xCA0,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "P2 fighter and gameplay state through 0x6274A0" },
    { "ddraw_error_text", 0x6274A0, 0x30,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "DirectDraw/startup diagnostic MessageBox text buffer; preserve live" },
    { "gap_before_renderer_id_6275A4", 0x6274D0, 0xD0,
      GameplayRelevance::Unknown, C::Unknown,
      "unreferenced padding before gameplay gate dword_6275A0" },
    { "dw_hi_gameplay_gate", 0x6275A0, 0x4,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "dword_6275A0 gameplay mode/gate used by fighter and actor logic" },
    { "renderer_surface_id_6275A4", 0x6275A4, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "sub_44FD90 DirectDraw surface-table index; preserve live" },
    { "dw_hi", 0x6275A8, 0x6098,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "shadow pool and gameplay state through dword_62D634" },
    { "ai_script_banks", 0x62D640, 0x1540,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "20 bank records: name[260], mutable-script owner +0x104, metadata count "
      "+0x108, metadata owner +0x10C; sub_460900/sub_460B50" },
    { "frame_counter", 0x4AB778, 0x10, GameplayRelevance::WallClockPacing,
      C::StaticAnalysis, "FPS statistic dword_4AB778 (++ per frame; sampled+reset each "
                          "wall-clock second by sub_4294F0; no sim readers)" },
    { "rng", 0x4A9930, 0x20, GameplayRelevance::DeterministicGameplay,
      C::RuntimeVerified, "LCG seeds 4A9938/4A993C" },
    { "camera_subpixel", 0x4AB754, 0x4, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "camera pan half-pixel accumulator dword_4AB754 (sub_463C50 only)" },
    { "finish_fx_seq",   0x4AA3C8, 0x20, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "KO finish sequencer: zoom dbl_4AA3C8, timer dword_4AA3D0, "
                          "dbl_4AA3E0 + scalars (sub_40EF30); gates fighter updates" },
    { "finish_fx_timer", 0x4AA408, 0x4,  GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "KO finish secondary timer dword_4AA408 (sub_40EF30)" },
    { "target_loop_fighter160_backup", 0x4AA3E8, 0x10, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "per-target scratch backup of fighter+160 (sub_421EE0 target loop "
                          "L33819 write / L34331 restore); deep-K stale-feeder" },
    { "round_banner_audio_state", 0x4AA400, 0x4,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "dword_4AA400 persistent round-announcer/audio substate 0..3 "
      "(sub_40D820 states 2/4/5/6); gates sounds 162 and 166+roundCounter" },
    { "round_intro_rotation", 0x4AA43C, 0x4,
      GameplayRelevance::RenderCosmetic, C::StaticAnalysis,
      "sub_40D820 decrements/wraps dword_4AA43C by 24 during rotating segment "
      "and copies it into banner descriptor rotation" },
    { "fighter_ptr_table", 0x4A37B4, 0x10, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "off_4A37B4[4] active-fighter ptrs; tag-swap sites "
                          "L30362/L36276/L22707/L19714; pointees all static+saved" },
    { "frame_time_ring_index", 0x4ABB20, 0x4,
      GameplayRelevance::WallClockPacing, C::StaticAnalysis,
      "timeGetTime pacing-ring index read by sub_4910C0 (SE sub_4910A0)" },
    { "rng_selector", 0x4ABB24, 0x4,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "dword_4ABB24 alone selects gameplay versus cosmetic RNG seed" },
    { "opening_drive_cursor", 0x4ABB28, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "GetDriveTypeA A:..Z: cursor used only by opening.avi discovery" },
    { "unused_combat_zero", 0x4ABB2C, 0x4,
      GameplayRelevance::Unknown, C::Unknown,
      "zero-only writes in sub_492480; no reader in either build" },
    // matchSub contains eight renderer surface-table IDs that must not be restored, because a display reset can recycle those indices without a layout generation change.
    { "matchsub_gameplay_00", 0x8716C0, 0x40,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "match/character-select state before renderer ID 0x871700" },
    { "renderer_surface_id_871700", 0x871700, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "sub_42C880/sub_44FD90 renderer surface-table index; preserve live" },
    { "matchsub_gameplay_01", 0x871704, 0x1A0,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "match state between renderer IDs" },
    { "renderer_surface_id_8718A4", 0x8718A4, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "renderer surface-table index; preserve live" },
    { "matchsub_gameplay_02", 0x8718A8, 0x4,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "match state between renderer IDs" },
    { "renderer_surface_id_8718AC", 0x8718AC, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "renderer surface-table index; preserve live" },
    { "matchsub_gameplay_03", 0x8718B0, 0x4,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "match state between renderer IDs" },
    { "renderer_surface_id_8718B4", 0x8718B4, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "renderer surface-table index; preserve live" },
    { "matchsub_gameplay_04", 0x8718B8, 0x8,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "match state between renderer IDs" },
    { "renderer_surface_id_8718C0", 0x8718C0, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "renderer surface-table index; preserve live" },
    { "matchsub_gameplay_05", 0x8718C4, 0x48,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "match state between renderer IDs" },
    { "renderer_surface_id_87190C", 0x87190C, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "renderer surface-table index; preserve live" },
    { "matchsub_gameplay_06", 0x871910, 0xC,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "match state between renderer IDs" },
    { "renderer_surface_id_87191C", 0x87191C, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "renderer surface-table index; preserve live" },
    { "matchsub_gameplay_07", 0x871920, 0x34,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "match state including round counter through 0x871954" },
    { "renderer_surface_id_871954", 0x871954, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "renderer surface-table index; preserve live" },
    { "matchsub_gameplay_08", 0x871958, 0x1168,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "remaining matchSub/character-select/round state through 0x872AC0" },
    { "ai_script_vm", 0x62EB80, 0x40024, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "AI-script VM: stack idx dword_62EB80, bank dword_62EB84, "
                          "script base dword_62EB88, complete 65,536-DWORD stack "
                          "0x62EBA0..0x66EBA0, and cursor dword_66EBA0; ends before "
                          "network callback pointer 0x66EBA4" },
    /* Four 0x5FC actors at 0x8A3740, one per player slot, armed by dword_6178E0[slot]; their tick draws gameplay RNG, so they are deterministic state. */
    { "rina_crash_actors", 0x8A3740, 0x17F0,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "four 0x5FC rina_cra.chr actors, one per player slot: loaded at "
      "sub_42C3A0 :18941 (+3 qmemcpy copies :18945-18947), armed from "
      "dword_6178E0[slot] by sub_41EBF0 :31185, ticked per player by "
      "sub_421EE0 :35523 through sub_411AE0/sub_412290 (RNG-drawing)" },
    { "ai_player_state", 0x8A5E00, 0x8B0, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "per-player AI pool 0x8A5E00..0x8A5FE0 + AI command queues "
                          "v4=0x8A5FE0+436*playerIdx (queue idx v4[12], masks v4[45+i]; "
                          "sub_44CFF0 writes, sub_47C440 consumes as COM input)" },
    // This split skips chatCallbackPtr @0x8A5C68, an external function pointer that must never be restored.
    { "hud_counters_a", 0x8A5C00, 0x68, GameplayRelevance::DeterministicGameplay,
      C::RuntimeVerified, "digit pool head (before chat callback ptr)" },
    { "hud_counters_b_head", 0x8A5C6C, 0xC,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "HUD/gameplay counters before renderer surface ID 0x8A5C78" },
    { "renderer_surface_id_8A5C78", 0x8A5C78, 0x4,
      GameplayRelevance::ExternalResource, C::StaticAnalysis,
      "sub_42C880/sub_44FD90 renderer surface-table index; preserve live" },
    { "hud_counters_b", 0x8A5C7C, 0x184,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "timer/combo digits 8A5CA0/8A5CA8 — combo count affects damage" },

    { "block0", 0x4ED140, 0x1CED0, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "arena block 0 = per-frame render list (rebuilt from gameplay)" },
    { "block1", 0x50A010, 0x1CED0, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "arena block 1 (= block0 + 0x1CED0)" },
    { "arena_mirror", 0x6177C0, 0x10, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "arena mirror/free-list ptr 6177CC (double-buffer bookkeeping)" },
    { "freeze_requests_23", 0x6177D0, 0x8,
      GameplayRelevance::DeterministicGameplay, C::RuntimeVerified,
      "global freeze request counters 6177D0/6177D4 (sub_41D690 types 2/3)" },
    { "arena_mirror_tail", 0x6177D8, 0x28, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "read-only tail of the arena bookkeeping carve" },
    { "arena_active", 0x6234E0, 0x20, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "arena active ptr 6234E8 (double-buffer bookkeeping)" },
    { "backup_fighter_ptrs", 0x623500, 0x10, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "dword_623500[4] backup fighter ptr table (init 0x614060/61465C/"
                          "614C58/615254, tag-swap partner of "
                          "off_4A37B4[])" },
    { "arena_head", 0x623510, 0x10, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "arena head 623510 (double-buffer bookkeeping)" },
    { "arena_sel", 0x6236F0, 0x30, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "arena selector 623700 (double-buffer bookkeeping)" },
    { "hud_text", 0x623940, 0x104, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "exact CHAR byte_623940[260] wsprintfA HUD text buffer" },
    { "arena_latch", 0x6267C0, 0x40, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "arena swap latch 6267D4 (double-buffer bookkeeping); NOTE: this legacy carve also contains side-0 EX state, see docs/gameplay/ex_gauge.md" },
    { "fps_counter_digit", 0x49B520, 0x10,
      GameplayRelevance::RenderCosmetic, C::StaticAnalysis,
      "dword_49B520 is the FPS ones-digit sprite coordinate derived from "
      "GetTickCount-backed sub_4294F0; peer-local render output" },
    { "camera_zoom_visual", 0x49B530, 0x8,
      GameplayRelevance::RenderCosmetic, C::StaticAnalysis,
      "dbl_49B530 only scales emitted round-announcement render descriptors" },
    { "round_intro_control", 0x49B538, 0xC,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "dbl_49B538 threshold and dword_49B540 timer advance the sub_40D820 "
      "audio/announcement FSM and gate normal gameplay" },
    { "finish_banner_descriptor", 0x49B548, 0x18,
      GameplayRelevance::RenderCosmetic, C::StaticAnalysis,
      "six-DWORD sprite descriptor mutated by the native finish/result renderer" },
    { "hud_display_head", 0x87D000, 0x37C4,
      GameplayRelevance::RenderCosmetic, C::RuntimeVerified,
      "hit/combo display feeders before native round-intro state" },
    { "round_intro_state", 0x8807C4, 0x4,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "sub_40D820 round-intro/announcement FSM; gates normal gameplay and "
      "KO checks in sub_421EE0" },
    { "hud_display_mid", 0x8807C8, 0x1C4,
      GameplayRelevance::RenderCosmetic, C::RuntimeVerified,
      "hit/combo display feeders between round-intro and finish states" },
    { "finish_fx_state", 0x88098C, 0x4,
      GameplayRelevance::DeterministicGameplay, C::StaticAnalysis,
      "native KO/result FSM dword_88098C; gates normal fighter updates and "
      "round/rematch progression in sub_40EF30/sub_421EE0" },
    { "hud_display_tail", 0x880990, 0x670,
      GameplayRelevance::RenderCosmetic, C::RuntimeVerified,
      "hit/combo display feeders after native finish/result state" },
    { "actor_clone_pool_tail", 0x553DC0, 0x1E0, GameplayRelevance::RenderCosmetic,
      C::RuntimeVerified, "past-pool tail incl. monotonic counter dword_553DC0 (not gameplay)" },
    { "match_aux_actor_pool_slots_05_29", 0x8737CC, 0x959C, GameplayRelevance::DeterministicGameplay,
      C::StaticAnalysis, "shared match auxiliary actor pool slots 5..29 (25 x 0x5FC); stage-11 spawn schedules gameplay RNG" },

    { "frame_time_ring", 0x4ABA2C, 0x40, GameplayRelevance::WallClockPacing,
      C::RuntimeVerified, "timeGetTime pacing ring (wide-diff residual)" },
    { "pacing_scratch_a", 0x4ABD30, 0x40, GameplayRelevance::WallClockPacing,
      C::RuntimeVerified, "pacing scratch (wide-diff residual)" },
    { "pacing_scratch_b", 0x4ABDC4, 0x40, GameplayRelevance::WallClockPacing,
      C::RuntimeVerified, "pacing scratch (wide-diff residual)" },

    { "chat_callback_ptr", 0x8A5C68, 0x4, GameplayRelevance::ExternalResource,
      C::StaticAnalysis, "externally-set chat callback (injected DLL)" },
    { "ddraw_ptrs", 0x66FACC, 0x4, GameplayRelevance::ExternalResource,
      C::StaticAnalysis, "IDirectDraw2*" },
    { "sprite_surfaces", 0x68BC00, 0x400, GameplayRelevance::ExternalResource,
      C::RuntimeVerified, "load-once DDraw surface table (full-restore froze the game)" },
    { "dsound_buffers", 0x4AAAE4, 0x4, GameplayRelevance::ExternalResource,
      C::StaticAnalysis, "IDirectSoundBuffer*" },
    { "net_callback_ptrs", 0x66EBA4, 0x2A0, GameplayRelevance::ExternalResource,
      C::StaticAnalysis, "netplay fn-ptr block 66EBA4..66EE44 area holds dispatch/callback ptrs "
                          "(66ECC0/66EE04/66EE08); input ring inside is gameplay but the block is "
                          "pointer-laden — consumers must use the fine-grained AddressMap entries" },

    { "gap_after_clone_pool", 0x553FA0, 0xBF060, GameplayRelevance::Unknown,
      C::Unknown, "0x553FA0..0x613000: no xref evidence yet" },
};

static const PointerChildInfo kChildren[] = {
    { "cmd_p1", 0x623E00 + 164, 0x200, GameplayRelevance::DeterministicGameplay, false,
      C::StaticAnalysis, "sub_47BF90 rings; input history IS gameplay state" },
    { "cmd_p2", 0x626880 + 164, 0x200, GameplayRelevance::DeterministicGameplay, false,
      C::StaticAnalysis, "sub_47BF90 rings" },
    { "cmd_p3", 0x4ECB40 + 164, 0x200, GameplayRelevance::DeterministicGameplay, true,
      C::StaticAnalysis, "team modes only" },
    { "cmd_p4", 0x617A20 + 164, 0x200, GameplayRelevance::DeterministicGameplay, true,
      C::StaticAnalysis, "team modes only" },
};

const RegionInfo* GameplayRegionCatalog::regions(std::size_t& count) {
    count = sizeof(kRegions) / sizeof(kRegions[0]);
    return kRegions;
}

const PointerChildInfo* GameplayRegionCatalog::pointerChildren(std::size_t& count) {
    count = sizeof(kChildren) / sizeof(kChildren[0]);
    return kChildren;
}

const RegionInfo* GameplayRegionCatalog::find(const char* name) {
    for (const auto& r : kRegions)
        if (std::strcmp(r.name, name) == 0)
            return &r;
    return nullptr;
}

int GameplayRegionCatalog::firstOverlapAmongClassified() {
    std::size_t n = sizeof(kRegions) / sizeof(kRegions[0]);
    for (std::size_t i = 0; i < n; ++i) {
        if (kRegions[i].relevance != R::DeterministicGameplay &&
            kRegions[i].relevance != R::RenderCosmetic)
            continue;
        for (std::size_t j = i + 1; j < n; ++j) {
            if (kRegions[j].relevance != R::DeterministicGameplay &&
                kRegions[j].relevance != R::RenderCosmetic)
                continue;
            GameAddr a0 = kRegions[i].address, a1 = a0 + kRegions[i].size;
            GameAddr b0 = kRegions[j].address, b1 = b0 + kRegions[j].size;
            if (a0 < b1 && b0 < a1)
                return static_cast<int>(i);
        }
    }
    return -1;
}

}
