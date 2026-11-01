#include "TestFramework.hpp"
#include "qoh99/AddressMap.hpp"
#include "qoh99/generated/AddressConstants.hpp"
#include "qoh99/generated/BuildProfiles.hpp"
#include "qoh99/generated/FunctionCatalog.hpp"
#include "qoh99/generated/GameplayRegions.hpp"
#include <string>

using namespace qoh99;

TEST(generated_model_exact_coverage) {
    CHECK_EQ(generated_model::kDataSymbolCount, 105u);
    CHECK_EQ(generated_model::kSizeConstantCount, 14u);
    CHECK_EQ(generated_model::kFunctionSymbolCount, 44u);
    CHECK_EQ(generated_model::kGameplayRegionCount, 97u);
    CHECK_EQ(generated_model::kPointerChildCount, 4u);
    CHECK_EQ(generated_model::kBuildCount, 2u);

    bool foundHost = false;
    bool foundExGauge = false;
    for (const auto& symbol : generated_model::kDataSymbols) {
        if (std::string(symbol.cxxName) == "kNetIsHost") {
            foundHost = true;
            CHECK_EQ(symbol.va, 0x66EE00u);
            CHECK_EQ(symbol.size, 4u);
        }
        if (std::string(symbol.cxxName) == "kExGaugeRecords") {
            foundExGauge = true;
            CHECK_EQ(symbol.va, 0x6267E0u);
            CHECK_EQ(symbol.size, 0x80u);
            CHECK(std::string(symbol.memoryClass) == "MutableGameplay");
            CHECK(std::string(symbol.confidence) == "StaticAnalysis");
        }
    }
    CHECK(foundHost);
    CHECK(foundExGauge);
    CHECK_EQ(generated_model::size::kExGaugeRecordStride, 0x20u);
    CHECK_EQ(generated_model::size::kExGaugeStockMax, 9u);
    CHECK_EQ(generated_model::size::kExGaugeFractionMax, 53u);
}

TEST(generated_model_build_identity_api) {
    const auto& runtime = generated_model::supportedRuntimeBuild();
    CHECK(runtime.id == generated_model::BuildId::Se2006);
    CHECK_EQ(runtime.binarySize, 704512ull);
    CHECK_EQ(runtime.coffTimestamp, 0x3A19A8A9u);
    CHECK(std::string(runtime.binarySha256) ==
          "A79002592953E8D9ACE3E363B3115CE4452C1A0789EB5B17C9BCB032AF27F1F6");

    const auto* found = generated_model::findBuildBySha256(
        "a79002592953e8d9ace3e363b3115ce4452c1a0789eb5b17c9bcb032af27f1f6");
    CHECK(found != nullptr);
    CHECK(found && found->id == generated_model::BuildId::Se2006);
    CHECK(generated_model::findBuildBySha256("not-a-build") == nullptr);

    CHECK(runtime.runtimeProfileId
          == generated_model::RuntimeProfileId::Qoh99Se1126);
    CHECK(std::string(runtime.runtimeProfileKey) == "qoh99-se1126");
    CHECK(runtime.runtimeQualification
          == generated_model::RuntimeQualification::Qualified);
    CHECK(!runtime.executableIdentityEstablishesSimulationContent);
    CHECK(std::string(runtime.simulationContentManifestPolicy)
          == "requires-future-manifest-attestation");

    const auto* candidate = generated_model::findBuildByRuntimeProfileId(
        generated_model::RuntimeProfileId::Qoh99DecemberCandidate);
    CHECK(candidate != nullptr);
    CHECK(candidate && std::string(candidate->runtimeProfileKey)
                       == "qoh99-december-candidate");
    CHECK(candidate && candidate->runtimeQualification
                           == generated_model::RuntimeQualification::Qualified);
    CHECK(candidate && !candidate->executableIdentityEstablishesSimulationContent);
}

TEST(generated_model_build_content_preconditions) {
    const auto* november = generated_model::findBuildByRuntimeProfileId(
        generated_model::RuntimeProfileId::Qoh99Se1126);
    const auto* december = generated_model::findBuildByRuntimeProfileId(
        generated_model::RuntimeProfileId::Qoh99DecemberCandidate);
    CHECK(november != nullptr);
    CHECK(december != nullptr);
    if (!november || !december) return;

    CHECK_EQ(november->contentPreconditions.chrHeaderRevisionVa, 0x004A71E0u);
    CHECK_EQ(december->contentPreconditions.chrHeaderRevisionVa, 0x004A71E0u);
    CHECK_EQ(november->contentPreconditions.chrHeaderRevision, 5u);
    CHECK_EQ(december->contentPreconditions.chrHeaderRevision, 6u);

    CHECK(std::string(november->contentPreconditions.simulationProbePath)
          .empty());
    CHECK(std::string(november->contentPreconditions.simulationProbeSha256)
          .empty());
    CHECK(std::string(december->contentPreconditions.simulationProbePath)
          == "yosie/yosie.chr");
    CHECK(std::string(december->contentPreconditions.simulationProbeSha256)
          == "A4AD728F7E37856138B30DC8D9CD842DF142540E8EB5F8106743F26327E9F9E2");
}

TEST(generated_model_both_builds_are_runnable) {
    CHECK_EQ(generated_model::runtimeQualifiedBuildCount(), 2u);
    CHECK(generated_model::buildIsRuntimeQualified(
        generated_model::BuildId::Se2006));
    CHECK(generated_model::buildIsRuntimeQualified(
        generated_model::BuildId::Game2000));
    CHECK(generated_model::buildIsRuntimeQualified(
        generated_model::kSupportedRuntimeBuild));
    for (const auto& build : generated_model::kBuilds) {
        const bool qualified = build.runtimeQualification
            == generated_model::RuntimeQualification::Qualified;
        CHECK_EQ(generated_model::runtimeProfileAllowsHookInstall(build),
                 qualified);
        CHECK(qualified);
    }
}

TEST(generated_model_per_build_function_addresses) {
    const generated_model::FunctionSymbol* inputRead = nullptr;
    const generated_model::FunctionSymbol* dispatch = nullptr;
    for (const auto& fn : generated_model::kFunctions) {
        if (std::string(fn.cxxName) == "kReadLogicalInput")
            inputRead = &fn;
        if (std::string(fn.cxxName) == "kSceneMatchDispatch")
            dispatch = &fn;
    }
    CHECK(inputRead != nullptr);
    CHECK(inputRead && inputRead->game2000Va == 0x484BB0u);
    CHECK(inputRead && inputRead->se2006Va == 0x484B90u);
    CHECK(generated_model::resolveFunctionVa(
              generated_model::BuildId::Game2000,
              generated_model::FunctionId::ReadLogicalInput)
          == 0x484BB0u);
    CHECK(generated_model::resolveFunctionVa(
              generated_model::BuildId::Se2006,
              generated_model::FunctionId::ReadLogicalInput)
          == 0x484B90u);
    CHECK(dispatch != nullptr);
    CHECK(dispatch && dispatch->game2000Va == 0x428430u);
    CHECK(dispatch && dispatch->se2006Va == 0x428430u);

    const auto* configSave = generated_model::findFunction(
        generated_model::FunctionId::ConfigSave);
    CHECK(configSave != nullptr);
    CHECK(configSave && configSave->game2000Va == 0x4853B0u);
    CHECK(configSave && configSave->se2006Va == 0x485390u);
}

TEST(generated_model_corrected_alias_semantics) {
    CHECK_EQ(addr::kVsFormationRow, 0x62369Cu);
    CHECK_EQ(addr::kMatchMode, addr::kVsFormationRow);
    CHECK_EQ(addr::kTeamSize, addr::kVsFormationRow);
    CHECK_EQ(addr::kNetSyncPosXRing, 0x66EE10u);
    CHECK_EQ(addr::kNetSyncPosYRing, 0x66EE18u);
    CHECK_EQ(addr::kNetSyncPosP1, addr::kNetSyncPosXRing);
    CHECK_EQ(addr::kNetSyncPosP2, addr::kNetSyncPosYRing);
}

TEST(generated_model_scene4_is_opening_movie_not_replay) {
    CHECK_EQ(generated_model::addr::kOpeningMovieFilename, 0x4AB794u);
    CHECK_EQ(qoh99::addr::kOpeningMovieFilename,
             generated_model::addr::kOpeningMovieFilename);

    bool found = false;
    for (const auto& symbol : generated_model::kDataSymbols) {
        std::string blob = std::string(symbol.cxxName) + '|' + symbol.name + '|'
                         + symbol.originalName;
        for (char& c : blob)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (blob.find("replay") != std::string::npos)
            testfw::reportFail(__FILE__, __LINE__,
                std::string("generated data symbol still named for replay: ")
                + symbol.cxxName);
        if (std::string(symbol.cxxName) == "kOpeningMovieFilename") {
            found = true;
            CHECK(std::string(symbol.name) == "openingMovieFilename");
            CHECK(std::string(symbol.originalName) == "byte_4AB794");
            CHECK_EQ(symbol.va, 0x4AB794u);
            CHECK_EQ(symbol.size, 1u);
        }
    }
    CHECK(found);
}

TEST(generated_model_vs_formation_symbol) {
    bool found = false;
    for (const auto& symbol : generated_model::kDataSymbols) {
        if (std::string(symbol.cxxName) != "kVsFormationRow")
            continue;
        found = true;
        CHECK(std::string(symbol.name) == "vsFormationRow");
        CHECK(std::string(symbol.originalName) == "dword_62369C");
        CHECK_EQ(symbol.va, 0x62369Cu);
        CHECK_EQ(symbol.size, 4u);
        CHECK(std::string(symbol.memoryClass) == "MutableGameplay");
        CHECK(std::string(symbol.name) != "matchMode");
    }
    CHECK(found);
    for (const auto& symbol : generated_model::kDataSymbols)
        CHECK(std::string(symbol.cxxName) != "kMatchMode");
}

TEST(generated_model_aux_actor_pool_region) {
    CHECK_EQ(generated_model::kGameplayRegionCount, 97u);
    std::size_t auxIdx = generated_model::kGameplayRegionCount, tailIdx = 0u, ringIdx = 0u;
    for (std::size_t i = 0; i < generated_model::kGameplayRegionCount; ++i) {
        const auto& r = generated_model::kGameplayRegions[i];
        const std::string name = r.name;
        if (name == "match_aux_actor_pool_slots_05_29") {
            auxIdx = i;
            CHECK_EQ(r.address, 0x8737CCu);
            CHECK_EQ(r.size, 0x959Cu);
            CHECK(std::string(r.legacyRelevance) == "DeterministicGameplay");
            CHECK(r.policy.capture);
            CHECK(r.policy.restore);
            CHECK(r.policy.gameplayHash);
            CHECK(!r.policy.renderHash);
        } else if (name == "actor_clone_pool_tail") {
            tailIdx = i;
        } else if (name == "frame_time_ring") {
            ringIdx = i;
        }
    }
    CHECK(auxIdx < generated_model::kGameplayRegionCount);
    CHECK_EQ(tailIdx + 1u, auxIdx);
    CHECK_EQ(auxIdx + 1u, ringIdx);
}

TEST(generated_model_round_intro_rotation_region) {
    std::size_t audioIdx = generated_model::kGameplayRegionCount;
    std::size_t rotationIdx = generated_model::kGameplayRegionCount;
    std::size_t fighterIdx = generated_model::kGameplayRegionCount;
    for (std::size_t i = 0; i < generated_model::kGameplayRegionCount; ++i) {
        const auto& r = generated_model::kGameplayRegions[i];
        const std::string name = r.name;
        if (name == "round_banner_audio_state") {
            audioIdx = i;
        } else if (name == "round_intro_rotation") {
            rotationIdx = i;
            CHECK_EQ(r.address, 0x4AA43Cu);
            CHECK_EQ(r.size, 0x4u);
            CHECK(std::string(r.legacyRelevance) == "RenderCosmetic");
            CHECK(r.semantic == generated_model::RegionSemantic::Render);
            CHECK(r.safety == generated_model::RegionSafety::PlainBytes);
            CHECK(r.policy.capture);
            CHECK(r.policy.restore);
            CHECK(!r.policy.gameplayHash);
            CHECK(r.policy.renderHash);
            CHECK(std::string(r.confidence) == "StaticAnalysis");
        } else if (name == "fighter_ptr_table") {
            fighterIdx = i;
        }
    }
    CHECK(rotationIdx < generated_model::kGameplayRegionCount);
    CHECK_EQ(audioIdx + 1u, rotationIdx);
    CHECK_EQ(rotationIdx + 1u, fighterIdx);
}
