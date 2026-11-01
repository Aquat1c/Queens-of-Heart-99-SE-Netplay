#include "TestFramework.hpp"
#include "qoh99/GameplayRegionCatalog.hpp"
#include "qoh99/Fighter.hpp"
#include "qoh99_game.h"
#include "RollbackManager.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace qoh99;

TEST(SavestateRegionsMatchGameplayCatalog) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    std::set<std::string> prodSet;
    for (size_t i = 0; i < pn; ++i)
        prodSet.insert(std::string(pr[i].name) + ":" +
                       std::to_string(pr[i].addr) + ":" + std::to_string(pr[i].size));

    size_t cn = 0; const RegionInfo* cr = GameplayRegionCatalog::regions(cn);
    std::set<std::string> catSet;
    int captured = 0;
    for (size_t i = 0; i < cn; ++i) {
        if (cr[i].relevance != GameplayRelevance::DeterministicGameplay &&
            cr[i].relevance != GameplayRelevance::RenderCosmetic)
            continue;
        ++captured;
        catSet.insert(std::string(cr[i].name) + ":" +
                      std::to_string(cr[i].address) + ":" + std::to_string(cr[i].size));
    }
    CHECK_EQ((int)pn, captured);
    CHECK(prodSet == catSet);
}

TEST(GameplayHashMatchesCatalog) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    for (size_t i = 0; i < pn; ++i) {
        const RegionInfo* c = GameplayRegionCatalog::find(pr[i].name);
        CHECK(c != nullptr);
        if (!c) continue;
        bool prodGameplay = inGameplayHash(pr[i].role);
        bool catGameplay = (c->relevance == GameplayRelevance::DeterministicGameplay);
        if (prodGameplay != catGameplay)
            testfw::reportFail(__FILE__, __LINE__,
                std::string("hash-class mismatch for ") + pr[i].name);
    }
}

TEST(NativeHudSuppressionTargetsAreNeverInGameplayHash) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    const uintptr_t kBlocks[] = { 0x4ED140u, 0x50A010u };
    const size_t    kOffs[]   = { 86640u, 87252u, 100404u, 100412u, 113604u };
    for (uintptr_t b : kBlocks) {
        const Region* block = nullptr;
        for (size_t i = 0; i < pn; ++i)
            if (pr[i].addr == b) block = &pr[i];
        CHECK(block != nullptr);
        if (block) CHECK(block->size == 0x1CED0u);
        for (size_t o : kOffs) {
            const Region* hit = nullptr;
            for (size_t i = 0; i < pn; ++i)
                if (b + o >= pr[i].addr && b + o < pr[i].addr + pr[i].size)
                    hit = &pr[i];
            CHECK(hit != nullptr);
            if (hit) { CHECK(!inGameplayHash(hit->role));
                       CHECK(inRenderHash(hit->role)); }
        }
    }
    CHECK(113604u + 36u < 0x1CED0u);
    bool timeTableCovered = false;
    for (uintptr_t a = 0x49EC58u; a < 0x49ED50u; a += 4u)
        for (size_t i = 0; i < pn; ++i)
            if (a >= pr[i].addr && a < pr[i].addr + pr[i].size)
                timeTableCovered = true;
    CHECK(!timeTableCovered);
    for (size_t i = 0; i < pn; ++i)
        if (0x553DC0u >= pr[i].addr && 0x553DC0u < pr[i].addr + pr[i].size)
            CHECK(!inGameplayHash(pr[i].role));
    bool maskInGameplayHash = false;
    for (size_t i = 0; i < pn; ++i)
        if (0x6236CCu >= pr[i].addr && 0x6236CCu < pr[i].addr + pr[i].size
            && inGameplayHash(pr[i].role))
            maskInGameplayHash = true;
    CHECK(maskInGameplayHash);
}

TEST(CosmeticRegionsExcludedFromGameplayHash) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    int cosmetic = 0;
    for (size_t i = 0; i < pn; ++i) {
        const RegionInfo* c = GameplayRegionCatalog::find(pr[i].name);
        if (c && c->relevance == GameplayRelevance::RenderCosmetic) {
            ++cosmetic;
            CHECK(!inGameplayHash(pr[i].role));
            CHECK(inRenderHash(pr[i].role));
            CHECK(safeToRestore(pr[i].role));
        }
    }
    CHECK(cosmetic >= 8);
}

TEST(ScreenEffectStateIsAnExactCosmeticCarve) {
    const RegionInfo* fx =
        GameplayRegionCatalog::find("screen_fx_state");
    const RegionInfo* gameplay =
        GameplayRegionCatalog::find("dw_mid2a");
    CHECK(fx != nullptr);
    CHECK(gameplay != nullptr);
    if (!fx || !gameplay) return;

    CHECK_EQ(fx->address, 0x623720u);
    CHECK_EQ(fx->size, 0x14u);
    CHECK(fx->relevance == GameplayRelevance::RenderCosmetic);
    CHECK_EQ(gameplay->address, 0x623734u);
    CHECK_EQ(gameplay->size, 0x1CCu);
    CHECK(gameplay->relevance
          == GameplayRelevance::DeterministicGameplay);
    CHECK_EQ(fx->address + static_cast<GameAddr>(fx->size),
             gameplay->address);
    CHECK_EQ(gameplay->address
                 + static_cast<GameAddr>(gameplay->size),
             0x623900u);

    size_t count = 0;
    const Region* saved = saveRegions(count);
    bool sawFx = false;
    bool sawGameplay = false;
    for (size_t i = 0; i < count; ++i) {
        if (std::string(saved[i].name) == "screen_fx_state") {
            sawFx = true;
            CHECK(!inGameplayHash(saved[i].role));
            CHECK(inRenderHash(saved[i].role));
        }
        if (std::string(saved[i].name) == "dw_mid2a") {
            sawGameplay = true;
            CHECK(inGameplayHash(saved[i].role));
        }
    }
    CHECK(sawFx);
    CHECK(sawGameplay);
}

TEST(RoundBannerAudioSubstateIsRollbackGameplay) {
    const RegionInfo* banner =
        GameplayRegionCatalog::find("round_banner_audio_state");
    CHECK(banner != nullptr);
    if (!banner) return;
    CHECK_EQ(banner->address, 0x4AA400u);
    CHECK_EQ(banner->size, 0x4u);
    CHECK(banner->relevance
          == GameplayRelevance::DeterministicGameplay);

    size_t count = 0;
    const Region* saved = saveRegions(count);
    bool captured = false;
    const GameAddr deadScratch[] = {
        0x4AA3F8u,
        0x4AA3FCu,
        0x4AA404u,
    };
    for (size_t i = 0; i < count; ++i) {
        if (std::string(saved[i].name)
            == "round_banner_audio_state") {
            captured = true;
            CHECK(safeToRestore(saved[i].role));
            CHECK(inGameplayHash(saved[i].role));
            CHECK(!inRenderHash(saved[i].role));
        }
        for (const GameAddr scratch : deadScratch) {
            CHECK(!(scratch >= saved[i].addr
                    && scratch < saved[i].addr + saved[i].size));
        }
    }
    CHECK(captured);
}

TEST(RoundBannerControlStateIsRollbackGameplay) {
    const RegionInfo* fps =
        GameplayRegionCatalog::find("fps_counter_digit");
    const RegionInfo* visual =
        GameplayRegionCatalog::find("camera_zoom_visual");
    const RegionInfo* camera =
        GameplayRegionCatalog::find("round_intro_control");
    const RegionInfo* phase =
        GameplayRegionCatalog::find("round_intro_state");
    CHECK(fps != nullptr);
    CHECK(visual != nullptr);
    CHECK(camera != nullptr);
    CHECK(phase != nullptr);
    if (!fps || !visual || !camera || !phase) return;

    CHECK_EQ(fps->address, 0x49B520u);
    CHECK_EQ(fps->size, 0x10u);
    CHECK(fps->relevance == GameplayRelevance::RenderCosmetic);
    CHECK_EQ(visual->address, 0x49B530u);
    CHECK_EQ(visual->size, 0x8u);
    CHECK(visual->relevance == GameplayRelevance::RenderCosmetic);
    CHECK_EQ(camera->address, 0x49B538u);
    CHECK_EQ(camera->size, 0xCu);
    CHECK(camera->address <= 0x49B540u);
    CHECK(camera->address + camera->size > 0x49B540u);
    CHECK(camera->relevance
          == GameplayRelevance::DeterministicGameplay);
    CHECK_EQ(phase->address, 0x8807C4u);
    CHECK_EQ(phase->size, 0x4u);
    CHECK(phase->relevance
          == GameplayRelevance::DeterministicGameplay);

    size_t count = 0;
    const Region* saved = saveRegions(count);
    bool timerCaptured = false;
    bool fpsCapturedAsCosmetic = false;
    bool phaseCaptured = false;
    for (size_t i = 0; i < count; ++i) {
        if (0x49B540u >= saved[i].addr
            && 0x49B540u < saved[i].addr + saved[i].size) {
            timerCaptured = true;
            CHECK(inGameplayHash(saved[i].role));
        }
        if (std::string(saved[i].name) == "fps_counter_digit") {
            fpsCapturedAsCosmetic = true;
            CHECK(!inGameplayHash(saved[i].role));
            CHECK(inRenderHash(saved[i].role));
        }
        if (std::string(saved[i].name) == "round_intro_state") {
            phaseCaptured = true;
            CHECK(inGameplayHash(saved[i].role));
            CHECK(safeToRestore(saved[i].role));
        }
    }
    CHECK(timerCaptured);
    CHECK(fpsCapturedAsCosmetic);
    CHECK(phaseCaptured);
}

TEST(RoundIntroRotationIsExactRenderCosmeticRollbackState) {
    const RegionInfo* rotation =
        GameplayRegionCatalog::find("round_intro_rotation");
    CHECK(rotation != nullptr);
    if (!rotation) return;
    CHECK_EQ(rotation->address, 0x4AA43Cu);
    CHECK_EQ(rotation->size, 0x4u);
    CHECK(rotation->relevance == GameplayRelevance::RenderCosmetic);

    size_t count = 0;
    const Region* saved = saveRegions(count);
    size_t captured = 0;
    for (size_t i = 0; i < count; ++i) {
        if (std::string(saved[i].name) != "round_intro_rotation")
            continue;
        ++captured;
        CHECK_EQ(saved[i].addr, 0x4AA43Cu);
        CHECK_EQ(saved[i].size, 0x4u);
        CHECK(saved[i].role == RegionRole::RenderCosmetic);
        CHECK(safeToRestore(saved[i].role));
        CHECK(!inGameplayHash(saved[i].role));
        CHECK(inRenderHash(saved[i].role));
    }
    CHECK_EQ(captured, 1u);
}

TEST(UnsafeRegionsNeverRestored) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    for (size_t i = 0; i < pn; ++i) {
        const RegionInfo* c = GameplayRegionCatalog::find(pr[i].name);
        CHECK(c != nullptr);
        if (c) {
            CHECK(c->relevance != GameplayRelevance::WallClockPacing);
            CHECK(c->relevance != GameplayRelevance::ExternalResource);
            CHECK(c->relevance != GameplayRelevance::Unknown);
        }
        CHECK(safeToRestore(pr[i].role));
    }
    CHECK(!safeToRestore(RegionRole::WallClockPacing));
    CHECK(!safeToRestore(RegionRole::OSPointerUnsafe));
    CHECK(!safeToRestore(RegionRole::DebugOnly));
}

TEST(PointerRegionsDerivedFromCatalog) {
    size_t pn = 0; const PtrRegion* pr = pointerRegions(pn);
    size_t cn = 0; const PointerChildInfo* pc = GameplayRegionCatalog::pointerChildren(cn);
    CHECK_EQ(pn, cn);
    for (size_t i = 0; i < pn && i < cn; ++i) {
        CHECK_EQ(pr[i].fighterBase + pr[i].ptrOffset, (uintptr_t)pc[i].pointerAddress);
        CHECK_EQ(pr[i].size, (size_t)pc[i].pointeeSize);
        CHECK(inGameplayHash(pr[i].role));
        CHECK_EQ(
            pr[i].activationPointerAddress,
            pc[i].nullable
                ? kFighterTableAddr + i * sizeof(uint32_t)
                : uintptr_t{0});
    }
}

TEST(NullableTeamCommandChildrenFollowActiveFighterSlots) {
    size_t count = 0;
    const PtrRegion* regions = pointerRegions(count);
    CHECK_EQ(count, size_t{4});
    if (count != 4) return;

    CHECK(pointerChildParticipates(regions[0], 0u));
    CHECK(pointerChildParticipates(regions[1], 0u));
    CHECK(!pointerChildParticipates(regions[2], 0u));
    CHECK(!pointerChildParticipates(regions[3], 0u));
    CHECK(pointerChildParticipates(
        regions[2], static_cast<uint32_t>(addr::kFighterP3)));
    CHECK(pointerChildParticipates(
        regions[3], static_cast<uint32_t>(addr::kFighterP4)));
}

TEST(SaveSetNeverCoversFullDataSection) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    size_t total = 0;
    for (size_t i = 0; i < pn; ++i) {
        total += pr[i].size;
        CHECK(pr[i].size <= 0x40024u);
        if (pr[i].size == 0x40024u)
            CHECK(std::string(pr[i].name) == "ai_script_vm");
    }
    CHECK(total < 0x180000u);
}

TEST(GameplaySyncCannotBeOkWithBadNonzero) {
    CHECK(gameplaySyncGreen(0));
    CHECK(!gameplaySyncGreen(1));
    CHECK(!gameplaySyncGreen(1623));
    for (unsigned long long b = 1; b < 5000; b += 137)
        CHECK(!gameplaySyncGreen(b));
}

TEST(ActorClonePoolEndsAt120Objects) {
    const RegionInfo* pool = GameplayRegionCatalog::find("actor_clone_pool");
    CHECK(pool != nullptr);
    if (pool) {
        CHECK_EQ(pool->size, 120u * 0x5FCu);
        CHECK_EQ(pool->size, 0x2CE20u);
        CHECK(pool->relevance == GameplayRelevance::DeterministicGameplay);
    }
}

TEST(EffectActorCanonicalIdentityCoversAllThirtySlots) {
    CHECK_EQ(RollbackManager::kQohEffectActorCount, 30u);
    const RegionInfo* working = GameplayRegionCatalog::find("dw_lo2");
    CHECK(working != nullptr);
    if (working) {
        const GameAddr lastActor =
            addr::kEffectPool
            + static_cast<GameAddr>(
                (RollbackManager::kQohEffectActorCount - 1u)
                * fighter_field::kSize);
        CHECK(lastActor + 0x59Cu
              <= working->address + working->size);
        CHECK(lastActor + fighter_field::kSize
              > working->address + working->size);
        CHECK_EQ(
            addr::kEffectPool
                + static_cast<GameAddr>(
                    RollbackManager::kQohEffectActorCount
                    * fighter_field::kSize),
            0x6234E8u);
    }
}

TEST(ActorClonePoolTailNotGameplayHashed) {
    const GameAddr tail = 0x526FA0 + 0x2CE20;
    CHECK_EQ(tail, 0x553DC0u);
    size_t n = 0; const RegionInfo* r = GameplayRegionCatalog::regions(n);
    for (size_t i = 0; i < n; ++i) {
        if (r[i].relevance != GameplayRelevance::DeterministicGameplay) continue;
        bool covers = tail >= r[i].address && tail < r[i].address + r[i].size;
        if (covers)
            testfw::reportFail(__FILE__, __LINE__,
                std::string("gameplay region ") + r[i].name + " still covers the pool tail 0x553DC0");
    }
    const RegionInfo* t = GameplayRegionCatalog::find("actor_clone_pool_tail");
    CHECK(t && t->address == tail && t->relevance == GameplayRelevance::RenderCosmetic);
    size_t pn = 0; const Region* pr = saveRegions(pn);
    for (size_t i = 0; i < pn; ++i)
        if (tail >= pr[i].addr && tail < pr[i].addr + pr[i].size) {
            CHECK(!inGameplayHash(pr[i].role));
            CHECK(inRenderHash(pr[i].role));
        }
}

TEST(FighterPlus56ClassificationDocumented) {
    CHECK_EQ(fighter_field::kActionLockEnd, 56u);
    const GameAddr p1_plus56 = addr::kFighterP1 + 56;
    CHECK_EQ(p1_plus56, 0x623E38u);
    size_t n = 0; const RegionInfo* r = GameplayRegionCatalog::regions(n);
    const RegionInfo* hit = nullptr;
    for (size_t i = 0; i < n; ++i)
        if (p1_plus56 >= r[i].address && p1_plus56 < r[i].address + r[i].size) hit = &r[i];
    CHECK(hit != nullptr);
    if (hit) {
        CHECK(hit->relevance == GameplayRelevance::DeterministicGameplay);
        CHECK(std::string(hit->name) == "dw_mid2b_fighter");
    }
}

TEST(WindowObjectsAreExplicitExternalCarves) {
    struct Expected {
        const char* name;
        GameAddr address;
        std::size_t size;
        GameplayRelevance relevance;
    };
    const Expected expected[] = {
        { "script_bank_selection", 0x623900, 0x40,
          GameplayRelevance::DeterministicGameplay },
        { "hud_text", 0x623940, 0x104,
          GameplayRelevance::RenderCosmetic },
        { "dw_mid2b_fighter", 0x623A44, 0x9BC,
          GameplayRelevance::DeterministicGameplay },
        { "window_primary", 0x624400, 0x4C8,
          GameplayRelevance::ExternalResource },
        { "window_adjacent_gameplay", 0x6248C8, 0x4,
          GameplayRelevance::DeterministicGameplay },
        { "window_audio_mode", 0x6248CC, 0x4,
          GameplayRelevance::ExternalResource },
        { "gap_after_primary_window", 0x6248D0, 0x10,
          GameplayRelevance::Unknown },
        { "dw_mid2b_ai_vars", 0x6248E0, 0x100,
          GameplayRelevance::DeterministicGameplay },
        { "net_message_cursor", 0x6249E0, 0x4,
          GameplayRelevance::ExternalResource },
        { "gap_before_secondary_window", 0x6249E4, 0x1C,
          GameplayRelevance::Unknown },
        { "window_secondary", 0x624A00, 0x4C8,
          GameplayRelevance::ExternalResource },
        { "gap_after_secondary_window", 0x624EC8, 0x8,
          GameplayRelevance::Unknown },
        { "dw_mid2b_tail_head", 0x624ED0, 0x60C,
          GameplayRelevance::DeterministicGameplay },
        { "renderer_object_id_6254DC", 0x6254DC, 0x4,
          GameplayRelevance::ExternalResource },
        { "dw_mid2b_tail", 0x6254E0, 0x12E0,
          GameplayRelevance::DeterministicGameplay },
    };
    GameAddr cursor = 0x623900;
    for (const Expected& item : expected) {
        const RegionInfo* region = GameplayRegionCatalog::find(item.name);
        CHECK(region != nullptr);
        if (!region) continue;
        CHECK_EQ(region->address, item.address);
        CHECK_EQ(region->size, item.size);
        CHECK(region->relevance == item.relevance);
        CHECK_EQ(region->address, cursor);
        cursor = region->address + static_cast<GameAddr>(region->size);
    }
    CHECK_EQ(cursor, 0x6267C0u);

    size_t count = 0;
    const Region* saved = saveRegions(count);
    const GameAddr externalStart[] = {
        0x624400, 0x6248CC, 0x6249E0, 0x624A00, 0x6254DC
    };
    const GameAddr externalEnd[] = {
        0x6248C8, 0x6248D0, 0x6249E4, 0x624EC8, 0x6254E0
    };
    for (size_t i = 0; i < count; ++i) {
        const GameAddr lo = static_cast<GameAddr>(saved[i].addr);
        const GameAddr hi = lo + static_cast<GameAddr>(saved[i].size);
        for (size_t external = 0; external < 5; ++external)
            CHECK(!(lo < externalEnd[external]
                    && externalStart[external] < hi));
    }

    CHECK(addr::kFighterP1 >= 0x623A44
          && addr::kFighterP1 < 0x624400);
    const RegionInfo* aiVars =
        GameplayRegionCatalog::find("dw_mid2b_ai_vars");
    CHECK(aiVars && aiVars->address <= 0x6248E0u
          && 0x6249E0u == aiVars->address + aiVars->size);
    CHECK(addr::kHealthArray >= 0x6254E0
          && addr::kHealthArray < 0x6267C0);
}

TEST(RendererIdsInsideGameplaySpansArePreserveLive) {
    const GameAddr ids[] = {
        0x6254DCu, 0x6275A4u,
        0x871700u, 0x8718A4u, 0x8718ACu, 0x8718B4u,
        0x8718C0u, 0x87190Cu, 0x87191Cu, 0x871954u,
        0x8A5C78u,
    };
    size_t catalogCount = 0;
    const RegionInfo* catalog =
        GameplayRegionCatalog::regions(catalogCount);
    size_t savedCount = 0;
    const Region* saved = saveRegions(savedCount);
    for (const GameAddr id : ids) {
        const RegionInfo* owner = nullptr;
        for (size_t i = 0; i < catalogCount; ++i)
            if (id >= catalog[i].address
                && id < catalog[i].address + catalog[i].size)
                owner = &catalog[i];
        CHECK(owner != nullptr);
        if (owner)
            CHECK(owner->relevance
                  == GameplayRelevance::ExternalResource);
        for (size_t i = 0; i < savedCount; ++i)
            CHECK(!(id >= saved[i].addr
                    && id < saved[i].addr + saved[i].size));
    }
}

TEST(ActorInteriorPointerCanonicalizationKeepsRelativeIdentity) {
    const uint32_t a = RollbackManager::canonicalActorInteriorPointer(
        0x710000u + 17u * 64u, 0x710000u, 100u, 64u);
    const uint32_t b = RollbackManager::canonicalActorInteriorPointer(
        0x930000u + 17u * 64u, 0x930000u, 100u, 64u);
    const uint32_t different =
        RollbackManager::canonicalActorInteriorPointer(
            0x930000u + 18u * 64u, 0x930000u, 100u, 64u);
    CHECK_EQ(a, b);
    CHECK(a != different);
    CHECK_EQ(
        RollbackManager::canonicalActorInteriorPointer(
            0x12345678u, 0x710000u, 100u, 64u),
        0x12345678u);
    CHECK_EQ(
        RollbackManager::canonicalActorInteriorPointer(
            0u, 0x710000u, 100u, 64u),
        0u);
    CHECK_EQ(
        RollbackManager::canonicalActorInteriorPointer(
            0x710000u + 100u * 64u,
            0x710000u, 100u, 64u),
        0x710000u + 100u * 64u);
    CHECK_EQ(
        RollbackManager::canonicalActorInteriorPointer(
            0x710040u, 0x710000u, 100u, 0u),
        0x710040u);
}

namespace {
std::vector<uint8_t> syntheticQohConfigRegion(
    uint32_t profile, uint8_t localFill) {
    std::vector<uint8_t> bytes(
        RollbackManager::kQohConfigRegionSize, 0x5Au);
    const size_t imageOffset =
        RollbackManager::kQohConfigImageAddress
        - RollbackManager::kQohConfigRegionAddress;
    const size_t localProfilesEnd =
        RollbackManager::kQohConfigSharedHeaderAddress
        - RollbackManager::kQohConfigRegionAddress;
    std::fill(
        bytes.begin() + imageOffset,
        bytes.begin() + localProfilesEnd,
        localFill);
    const size_t localGap =
        RollbackManager::kQohConfigSharedHeaderAddress
        + RollbackManager::kQohConfigSharedHeaderSize
        - RollbackManager::kQohConfigRegionAddress;
    const size_t sharedBlock =
        RollbackManager::kQohConfigSharedBlockAddress
        - RollbackManager::kQohConfigRegionAddress;
    std::fill(
        bytes.begin() + localGap,
        bytes.begin() + sharedBlock,
        localFill);
    const uint32_t active =
        static_cast<uint32_t>(RollbackManager::kQohConfigRecordBase)
        + profile * static_cast<uint32_t>(
            RollbackManager::kQohConfigRecordStride);
    RollbackManager::writeCanonicalU32(
        bytes.data(),
        RollbackManager::kQohActiveConfigPointerAddress
            - RollbackManager::kQohConfigRegionAddress,
        active);
    const size_t record =
        static_cast<size_t>(
            active - RollbackManager::kQohConfigRegionAddress);
    RollbackManager::writeCanonicalU32(
        bytes.data(), record + 36u, 2u);
    RollbackManager::writeCanonicalU32(
        bytes.data(), record + 40u, 1u);
    RollbackManager::writeCanonicalU32(
        bytes.data(), record + 44u, 0u);
    RollbackManager::writeCanonicalU32(
        bytes.data(), record + 48u, 2u);
    return bytes;
}

std::vector<uint8_t> syntheticQohConfigSnapshot(uint8_t localFill) {
    std::vector<uint8_t> bytes(
        RollbackManager::kQohConfigSnapshotRegionSize, 0x7Bu);
    std::fill(
        bytes.begin(),
        bytes.begin()
            + RollbackManager::kQohConfigGameplayPrefixSize,
        localFill);
    RollbackManager::writeCanonicalU32(bytes.data(), 36u, 2u);
    RollbackManager::writeCanonicalU32(bytes.data(), 40u, 1u);
    RollbackManager::writeCanonicalU32(bytes.data(), 44u, 0u);
    RollbackManager::writeCanonicalU32(bytes.data(), 48u, 2u);
    return bytes;
}
}

TEST(QohConfigPointersAcceptEveryLocalProfileButRejectInvalidOwners) {
    for (uint32_t profile = 0;
         profile < RollbackManager::kQohConfigRecordCount;
         ++profile) {
        const uint32_t active =
            static_cast<uint32_t>(
                RollbackManager::kQohConfigRecordBase)
            + profile * static_cast<uint32_t>(
                RollbackManager::kQohConfigRecordStride);
        CHECK(RollbackManager::validQohActiveConfigPointer(active));
        CHECK_EQ(
            RollbackManager::expectedQohBindingPointer(active),
            active + RollbackManager::kQohConfigBindingOffset);
    }
    CHECK(!RollbackManager::validQohActiveConfigPointer(0u));
    CHECK(!RollbackManager::validQohActiveConfigPointer(
        static_cast<uint32_t>(
            RollbackManager::kQohConfigRecordBase + 1u)));
    CHECK(!RollbackManager::validQohActiveConfigPointer(
        static_cast<uint32_t>(
            RollbackManager::kQohConfigRecordBase
            + RollbackManager::kQohConfigRecordCount
                * RollbackManager::kQohConfigRecordStride)));
    CHECK_EQ(RollbackManager::expectedQohBindingPointer(0u), 0u);
}

TEST(QohPeerLocalProfilesPathsAndBindingsHaveOneCanonicalIdentity) {
    const std::vector<uint8_t> rawLeft =
        syntheticQohConfigRegion(0u, 0x11u);
    const std::vector<uint8_t> rawRight =
        syntheticQohConfigRegion(4u, 0xE2u);
    std::vector<uint8_t> left = rawLeft;
    std::vector<uint8_t> right = rawRight;
    CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
        RollbackManager::kQohConfigRegionAddress,
        left.data(), left.size()));
    CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
        RollbackManager::kQohConfigRegionAddress,
        right.data(), right.size()));
    CHECK(left == right);

    const size_t leftBinding =
        static_cast<size_t>(
            RollbackManager::kQohConfigRecordBase
            - RollbackManager::kQohConfigRegionAddress)
        + RollbackManager::kQohConfigBindingOffset;
    const size_t rightBinding =
        static_cast<size_t>(
            RollbackManager::kQohConfigRecordBase
            - RollbackManager::kQohConfigRegionAddress)
        + 4u * RollbackManager::kQohConfigRecordStride
        + RollbackManager::kQohConfigBindingOffset;
    CHECK_EQ(rawLeft[leftBinding], 0x11u);
    CHECK_EQ(rawRight[rightBinding], 0xE2u);
}

TEST(QohConfigCanonicalizationStillProvesEveryGameplayField) {
    const std::vector<uint8_t> raw =
        syntheticQohConfigRegion(2u, 0x42u);
    std::vector<uint8_t> canonical = raw;
    CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
        RollbackManager::kQohConfigRegionAddress,
        canonical.data(), canonical.size()));
    const size_t record =
        static_cast<size_t>(
            RollbackManager::kQohConfigRecordBase
            - RollbackManager::kQohConfigRegionAddress)
        + 2u * RollbackManager::kQohConfigRecordStride;
    const size_t gameplayOffsets[] = { 36u, 40u, 44u, 48u };
    for (const size_t field : gameplayOffsets) {
        std::vector<uint8_t> changed = raw;
        RollbackManager::writeCanonicalU32(
            changed.data(), record + field,
            RollbackManager::readCanonicalU32(
                changed.data(), record + field) + 1u);
        CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
            RollbackManager::kQohConfigRegionAddress,
            changed.data(), changed.size()));
        CHECK(changed != canonical);
    }

    std::vector<uint8_t> changed = raw;
    changed[0x100u] ^= 0x80u;
    CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
        RollbackManager::kQohConfigRegionAddress,
        changed.data(), changed.size()));
    CHECK(changed != canonical);
}

TEST(QohVanillaConfigPersistenceBlocksAreExcludedFromGameplayIdentity) {
    const std::vector<uint8_t> raw =
        syntheticQohConfigRegion(1u, 0x39u);
    std::vector<uint8_t> canonical = raw;
    CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
        RollbackManager::kQohConfigRegionAddress,
        canonical.data(), canonical.size()));

    const uintptr_t sharedAddresses[] = {
        RollbackManager::kQohConfigSharedHeaderAddress,
        RollbackManager::kQohConfigSharedHeaderAddress
            + RollbackManager::kQohConfigSharedHeaderSize - 1u,
        RollbackManager::kQohConfigSharedBlockAddress,
        RollbackManager::kQohConfigSharedBlockTailAddress - 1u,
    };
    for (const uintptr_t address : sharedAddresses) {
        std::vector<uint8_t> changed = raw;
        changed[address
                - RollbackManager::kQohConfigRegionAddress] ^= 0x40u;
        CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
            RollbackManager::kQohConfigRegionAddress,
            changed.data(), changed.size()));
        CHECK(changed == canonical);
    }

    CHECK_EQ(
        RollbackManager::kQohConfigSharedBlockAddress
            + RollbackManager::kQohConfigSharedBlockSize,
        RollbackManager::kQohConfigImageAddress
            + RollbackManager::kQohConfigImageSize);
    CHECK_EQ(
        RollbackManager::kQohConfigSharedBlockTailSize,
        size_t{0xCu});

    size_t regionCount = 0;
    const Region* regions = saveRegions(regionCount);
    bool foundTailOwner = false;
    for (size_t i = 0; i < regionCount; ++i) {
        if (RollbackManager::kQohConfigSharedBlockTailAddress
                >= regions[i].addr
            && RollbackManager::kQohConfigSharedBlockTailAddress
                < regions[i].addr + regions[i].size) {
            foundTailOwner = true;
            CHECK(!inGameplayHash(regions[i].role));
        }
    }
    CHECK(foundTailOwner);
}

TEST(QohSchedulerLocalFieldsRequireCanonicalZero) {
    std::vector<uint8_t> record(
        RollbackManager::kQohConfigGameplayPrefixSize, 0u);
    CHECK(RollbackManager::qohNetNormalizedConfigValid(
        record.data(), record.size()));
    CHECK(
        [&] {
            std::vector<uint8_t> withBgm = record;
            RollbackManager::writeCanonicalU32(
                withBgm.data(), 60u, 3u);
            return RollbackManager::qohNetNormalizedConfigValid(
                withBgm.data(), withBgm.size());
        }());
    for (const size_t offset :
         RollbackManager::kQohNetNormalizedOffsets) {
        std::vector<uint8_t> changed = record;
        RollbackManager::writeCanonicalU32(
            changed.data(), offset, 1u);
        CHECK(!RollbackManager::qohNetNormalizedConfigValid(
            changed.data(), changed.size()));
    }
    CHECK(!RollbackManager::qohNetNormalizedConfigValid(
        record.data(), 96u));
}

TEST(QohMatchSnapshotCanonicalizationDropsOnlyPeerLocalPrefix) {
    std::vector<uint8_t> left =
        syntheticQohConfigSnapshot(0x10u);
    std::vector<uint8_t> right =
        syntheticQohConfigSnapshot(0xF0u);
    CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
        RollbackManager::kQohConfigSnapshotAddress,
        left.data(), left.size()));
    CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
        RollbackManager::kQohConfigSnapshotAddress,
        right.data(), right.size()));
    CHECK(left == right);

    std::vector<uint8_t> gameplayChanged =
        syntheticQohConfigSnapshot(0x70u);
    RollbackManager::writeCanonicalU32(
        gameplayChanged.data(), 48u, 3u);
    CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
        RollbackManager::kQohConfigSnapshotAddress,
        gameplayChanged.data(), gameplayChanged.size()));
    CHECK(gameplayChanged != left);

    std::vector<uint8_t> tailChanged =
        syntheticQohConfigSnapshot(0x40u);
    tailChanged[RollbackManager::kQohConfigGameplayPrefixSize] ^= 1u;
    CHECK(RollbackManager::canonicalizeQohPeerLocalConfigForHash(
        RollbackManager::kQohConfigSnapshotAddress,
        tailChanged.data(), tailChanged.size()));
    CHECK(tailChanged != left);
}

TEST(AiScriptHeapCanonicalizationUsesBankAndOffset) {
    const RollbackManager::ScriptRange left[] = {
        { 0x710000u, 0x8000u, 3u },
        { 0x720000u, 0x7000u, 7u },
    };
    const RollbackManager::ScriptRange right[] = {
        { 0x910000u, 0x8000u, 3u },
        { 0xA20000u, 0x7000u, 7u },
    };
    uint32_t leftBank = 0, leftOffset = 0;
    uint32_t rightBank = 0, rightOffset = 0;
    CHECK(RollbackManager::canonicalScriptPointer(
        0x710234u, left, 2, leftBank, leftOffset));
    CHECK(RollbackManager::canonicalScriptPointer(
        0x910234u, right, 2, rightBank, rightOffset));
    CHECK_EQ(leftBank, rightBank);
    CHECK_EQ(leftOffset, rightOffset);
    CHECK_EQ(leftBank, 3u);
    CHECK_EQ(leftOffset, 0x234u);

    uint32_t bank = 0, offset = 0;
    CHECK(RollbackManager::canonicalScriptPointer(
        0xA20040u, right, 2, bank, offset));
    CHECK_EQ(bank, 7u);
    CHECK_EQ(offset, 0x40u);
    CHECK(!RollbackManager::canonicalScriptPointer(
        0x12345678u, right, 2, bank, offset));

    CHECK(RollbackManager::canonicalScriptPointer(
        0x910000u, right, 2, bank, offset));
    CHECK_EQ(offset, 0u);
    CHECK(RollbackManager::canonicalScriptPointer(
        0x917FFFu, right, 2, bank, offset));
    CHECK_EQ(offset, 0x7FFFu);
    CHECK(!RollbackManager::canonicalScriptPointer(
        0x918000u, right, 2, bank, offset));

    CHECK(RollbackManager::canonicalScriptPointerForBank(
        0x910234u, right, 2, 3u, offset));
    CHECK_EQ(offset, 0x234u);
    CHECK(!RollbackManager::canonicalScriptPointerForBank(
        0xA20040u, right, 2, 3u, offset));
    CHECK(RollbackManager::canonicalScriptPointerForBank(
        0xA20040u, right, 2, 7u, offset));
    CHECK_EQ(offset, 0x40u);
}

TEST(SnapshotPresenceMarkersAreExactlyBinary) {
    CHECK(RollbackManager::validPresenceMarker(0u));
    CHECK(RollbackManager::validPresenceMarker(1u));
    CHECK(!RollbackManager::validPresenceMarker(2u));
    CHECK(!RollbackManager::validPresenceMarker(0xFFFFFFFFu));
}

TEST(SnapshotSizeArithmeticRejectsOverflow) {
    const size_t maximum = (std::numeric_limits<size_t>::max)();
    size_t result = 0;

    CHECK(RollbackManager::checkedSizeAdd(
        maximum - 4u, 4u, result));
    CHECK_EQ(result, maximum);
    result = 0x1234u;
    CHECK(!RollbackManager::checkedSizeAdd(
        maximum - 3u, 4u, result));
    CHECK_EQ(result, 0x1234u);

    const size_t quotient = maximum / 31u;
    CHECK(RollbackManager::checkedSizeMultiply(
        31u, quotient, result));
    CHECK_EQ(result, maximum - (maximum % 31u));
    result = 0x5678u;
    CHECK(!RollbackManager::checkedSizeMultiply(
        31u, quotient + 1u, result));
    CHECK_EQ(result, 0x5678u);

    CHECK(RollbackManager::checkedSizeMultiply(
        0u, maximum, result));
    CHECK_EQ(result, 0u);
    CHECK_EQ(RollbackManager::maxSnapshotPoolBytes(),
             size_t{512u} * 1024u * 1024u);
}

TEST(AddressSpanArithmeticRejectsWrapAndNonRanges) {
    const uintptr_t maximum =
        (std::numeric_limits<uintptr_t>::max)();
    uintptr_t end = 0;

    CHECK(RollbackManager::checkedAddressEnd(1u, 1u, end));
    CHECK_EQ(end, uintptr_t{2u});
    CHECK(RollbackManager::checkedAddressEnd(
        maximum - 1u, 1u, end));
    CHECK_EQ(end, maximum);

    end = uintptr_t{0x1234u};
    CHECK(!RollbackManager::checkedAddressEnd(0u, 1u, end));
    CHECK_EQ(end, uintptr_t{0x1234u});
    CHECK(!RollbackManager::checkedAddressEnd(1u, 0u, end));
    CHECK_EQ(end, uintptr_t{0x1234u});
    CHECK(!RollbackManager::checkedAddressEnd(maximum, 1u, end));
    CHECK_EQ(end, uintptr_t{0x1234u});
    CHECK(!RollbackManager::checkedAddressEnd(
        maximum - 1u, 2u, end));
    CHECK_EQ(end, uintptr_t{0x1234u});
}

TEST(AiVmBoundsRejectCursorAliasAndOddFetches) {
    CHECK(RollbackManager::validAiVmDepth(0u));
    CHECK(RollbackManager::validAiVmDepth(0xFFFFu));
    CHECK(!RollbackManager::validAiVmDepth(0x10000u));
    CHECK(!RollbackManager::validAiVmDepth(0xFFFFFFFFu));

    CHECK(RollbackManager::validAiVmCursor(0u, 2u));
    CHECK(RollbackManager::validAiVmCursor(0x7Eu, 0x80u));
    CHECK(!RollbackManager::validAiVmCursor(1u, 0x80u));
    CHECK(!RollbackManager::validAiVmCursor(0x7Fu, 0x80u));
    CHECK(!RollbackManager::validAiVmCursor(0x80u, 0x80u));
    CHECK(!RollbackManager::validAiVmCursor(0u, 1u));

    const RollbackManager::ScriptRange ranges[] = {
        { 0x710000u, 0x80u, 3u },
        { 0x720000u, 0x40u, 7u },
    };
    CHECK(RollbackManager::validAiVmSelection(
        0u, 0u, 0u, 0u, ranges, 2));
    CHECK(!RollbackManager::validAiVmSelection(
        1u, 0u, 0u, 0u, ranges, 2));
    CHECK(RollbackManager::validAiVmSelection(
        0xFFFFu, 3u, 0x710000u, 0x7Eu, ranges, 2));
    CHECK(!RollbackManager::validAiVmSelection(
        0x10000u, 3u, 0x710000u, 0x7Eu, ranges, 2));
    CHECK(!RollbackManager::validAiVmSelection(
        0u, 7u, 0x710000u, 0u, ranges, 2));
    CHECK(!RollbackManager::validAiVmSelection(
        0u, 3u, 0x710002u, 0u, ranges, 2));
    CHECK(!RollbackManager::validAiVmSelection(
        0u, 20u, 0x710000u, 0u, ranges, 2));
    CHECK(!RollbackManager::validAiVmSelection(
        0u, 3u, 0x710000u, 1u, ranges, 2));
}

TEST(DescriptorIdentitySeparatesFamilyShapeAndContent) {
    const uint64_t baseline =
        RollbackManager::descriptorIdentityFingerprint(
            3u, 100u, 6400u, 0x123456789ABCDEF0ull);
    CHECK(baseline !=
          RollbackManager::descriptorIdentityFingerprint(
              4u, 100u, 6400u, 0x123456789ABCDEF0ull));
    CHECK(baseline !=
          RollbackManager::descriptorIdentityFingerprint(
              3u, 101u, 6400u, 0x123456789ABCDEF0ull));
    CHECK(baseline !=
          RollbackManager::descriptorIdentityFingerprint(
              3u, 100u, 6408u, 0x123456789ABCDEF0ull));
    CHECK(baseline !=
          RollbackManager::descriptorIdentityFingerprint(
              3u, 100u, 6400u, 0x123456789ABCDEEFull));
}

TEST(EveryNativeFreezeRequestCounterIsGameplayHashed) {
    const GameAddr counters[] = {
        0x6248C8,
        0x613F04,
        0x6177D0,
        0x6177D4,
        0x6275B4,
        0x6275AC,
        0x6275B0,
    };
    const GameAddr arbiter[] = {
        0x626E7C,
        0x618020,
    };

    size_t count = 0;
    const RegionInfo* regions = GameplayRegionCatalog::regions(count);
    const auto relevanceAt = [&](GameAddr address) {
        for (size_t i = 0; i < count; ++i) {
            const GameAddr lo = regions[i].address;
            const GameAddr hi = lo + static_cast<GameAddr>(regions[i].size);
            if (address >= lo && address + 4u <= hi) return regions[i].relevance;
        }
        return GameplayRelevance::Unknown;
    };

    for (const GameAddr address : counters) {
        CHECK(relevanceAt(address)
              == GameplayRelevance::DeterministicGameplay);
    }
    for (const GameAddr address : arbiter) {
        CHECK(relevanceAt(address)
              == GameplayRelevance::DeterministicGameplay);
    }

    const RegionInfo* head = GameplayRegionCatalog::find("arena_mirror");
    const RegionInfo* mid = GameplayRegionCatalog::find("freeze_requests_23");
    const RegionInfo* tail = GameplayRegionCatalog::find("arena_mirror_tail");
    CHECK(head != nullptr && mid != nullptr && tail != nullptr);
    if (head && mid && tail) {
        CHECK_EQ(head->address, 0x6177C0u);
        CHECK_EQ(head->address + static_cast<GameAddr>(head->size),
                 mid->address);
        CHECK_EQ(mid->address + static_cast<GameAddr>(mid->size),
                 tail->address);
        CHECK_EQ(tail->address + static_cast<GameAddr>(tail->size),
                 0x617800u);
        CHECK(head->relevance == GameplayRelevance::RenderCosmetic);
        CHECK(tail->relevance == GameplayRelevance::RenderCosmetic);
    }

    size_t savedCount = 0;
    const Region* saved = saveRegions(savedCount);
    for (const GameAddr address : counters) {
        bool hashed = false;
        for (size_t i = 0; i < savedCount; ++i) {
            const GameAddr lo = static_cast<GameAddr>(saved[i].addr);
            const GameAddr hi = lo + static_cast<GameAddr>(saved[i].size);
            if (address >= lo && address + 4u <= hi
                && inGameplayHash(saved[i].role)) {
                hashed = true;
                break;
            }
        }
        CHECK(hashed);
    }
}

TEST(AuxActorPoolIsACatalogDrivenGameplayRegionInProduction) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    int found = 0;
    for (size_t i = 0; i < pn; ++i) {
        if (std::string(pr[i].name) == "match_aux_actor_pool_slots_05_29") {
            ++found;
            CHECK_EQ(pr[i].addr, 0x8737CCu);
            CHECK_EQ(pr[i].size, 0x959Cu);
            CHECK(pr[i].role == RegionRole::GameplayRequired);
            CHECK(inGameplayHash(pr[i].role));
            CHECK(!inRenderHash(pr[i].role));
            CHECK(safeToRestore(pr[i].role));
        }
    }
    CHECK_EQ(found, 1);
}

TEST(AuxActorPoolRegionIsNeverACandidateOrDebugRegion) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    for (size_t i = 0; i < pn; ++i)
        if (std::string(pr[i].name) == "match_aux_actor_pool_slots_05_29") {
            CHECK(pr[i].role == RegionRole::GameplayRequired);
            CHECK(pr[i].role != RegionRole::GameplayCandidate);
        }
}

TEST(ProductionSavedRegionCountTracksExtendedCatalog) {
    size_t pn = 0; saveRegions(pn);
    CHECK_EQ(pn, 63u);
}

TEST(RinaCrashActorsAreACatalogDrivenGameplayRegionInProduction) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    int found = 0;
    for (size_t i = 0; i < pn; ++i) {
        if (std::string(pr[i].name) == "rina_crash_actors") {
            ++found;
            CHECK_EQ(pr[i].addr, 0x8A3740u);
            CHECK_EQ(pr[i].size, 0x17F0u);
            CHECK_EQ(pr[i].size, 4u * 0x5FCu);
            CHECK(pr[i].role == RegionRole::GameplayRequired);
            CHECK(pr[i].role != RegionRole::GameplayCandidate);
            CHECK(inGameplayHash(pr[i].role));
            CHECK(!inRenderHash(pr[i].role));
            CHECK(safeToRestore(pr[i].role));
        }
    }
    CHECK_EQ(found, 1);
}

TEST(RinaCrashActorsDoNotOverlapAnySavedRegion) {
    size_t pn = 0; const Region* pr = saveRegions(pn);
    for (size_t i = 0; i < pn; ++i) {
        if (std::string(pr[i].name) == "rina_crash_actors") continue;
        const uintptr_t aLo = 0x8A3740u, aHi = 0x8A3740u + 0x17F0u;
        const uintptr_t bLo = pr[i].addr, bHi = pr[i].addr + pr[i].size;
        CHECK(aHi <= bLo || bHi <= aLo);
    }
}

namespace {
std::string readRollbackManagerSource() {
    const std::string here = __FILE__;
    const std::size_t at = here.find("src");
    if (at == std::string::npos) return std::string{};
    std::ifstream file(here.substr(0, at) + "src/RollbackManager.cpp",
                       std::ios::binary);
    const std::istreambuf_iterator<char> begin(file);
    const std::istreambuf_iterator<char> end;
    std::string text(begin, end);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

std::string withoutSpaces(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
        if (c != ' ' && c != '\n' && c != '\t') out.push_back(c);
    return out;
}
}

TEST(RinaCrashActorsCanonicalizeEverySlotNotJustTheRoot) {
    const std::string rollback = readRollbackManagerSource();
    CHECK(!rollback.empty());
    const std::string compact = withoutSpaces(rollback);
    CHECK(compact.find("addPool(rina->addr,4u);") != std::string::npos);
    CHECK(compact.find("rina->size!=4u*addr::kFighterSize")
          != std::string::npos);
    CHECK(compact.find("\"rina_crash_actors\"") != std::string::npos);
    CHECK(compact.find("0x624EE0u,};") != std::string::npos);
    CHECK(compact.find("0x8A3740u,")
          != std::string::npos);
}

TEST(RinaCrashActorsSnapshotByteDelta) {
    const size_t regionBytes = 0x17F0u;
    CHECK_EQ(regionBytes, 6128u);
    CHECK_EQ(31u * regionBytes, 189968u);
    CHECK(31u * regionBytes < RollbackManager::maxSnapshotPoolBytes());
}

TEST(RoundIntroRotationSnapshotByteDelta) {
    const size_t regionBytes = 0x4u;
    CHECK_EQ(regionBytes, 4u);
    CHECK_EQ(31u * regionBytes, 124u);
}

TEST(AuxActorPoolSnapshotByteDelta) {
    const size_t regionBytes = 0x959Cu;
    CHECK_EQ(regionBytes, 38300u);
    CHECK_EQ(31u * regionBytes, 1187300u);
    CHECK(31u * regionBytes < RollbackManager::maxSnapshotPoolBytes());
}
