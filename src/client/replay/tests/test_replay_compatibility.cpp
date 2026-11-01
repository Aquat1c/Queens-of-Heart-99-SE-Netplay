#include "TestHarness.h"

#include "ReplayTestFixtures.h"

#include "client/replay/ReplayCompatibility.h"

#include <string>
#include <vector>

using namespace qreplay;
using namespace qclient::replay;
using namespace qclient::replay::test;

namespace {

RuntimeIdentity runtimeFor(const SimulationContract& contract) {
    RuntimeIdentity runtime{};
    runtime.known = true;
    runtime.executableSha256 = contract.executableSha256;
    runtime.runtimeSimulationId = contract.runtimeSimulationId;
    runtime.stateRegionFingerprint = contract.stateRegionFingerprint;
    runtime.stateHashProjectionId = contract.stateHashProjectionId;
    runtime.stateHashAlgorithmId = contract.stateHashAlgorithmId;
    runtime.bootstrapCodecVersion = contract.bootstrapCodecVersion;
    runtime.eventSchemaVersion = contract.eventSchemaVersion;
    runtime.contentManifestRoot = contract.contentManifestRoot;
    runtime.characterCatalogId = contract.characterCatalogId;
    runtime.fpuControlWord = contract.fpuControlWord;
    runtime.mxcsr = contract.mxcsr;
    runtime.supportsFourPlayer = true;
    runtime.supportsCpuFighters = true;
    runtime.supportsPortableCheckpoint = true;
    runtime.supportsEffectiveFighterDomain = true;
    return runtime;
}

ReplayIdentitySnapshot goodFile() {
    FixtureOptions options{};
    ReplayIdentitySnapshot file{};
    file.contract = makeContract(options);
    file.prefix.containerMajor = kContainerVersionMajor;
    file.prefix.minimumReaderMajor = kContainerReaderMajor;
    file.structurallyValid = true;
    file.hasFooter = true;
    file.hasSessionStart = true;
    file.digestVerified = true;
    file.activeFighterCount = 2u;
    return file;
}

}

QNP_TEST(replay_compatibility_exact_requires_every_identity) {
    const ReplayIdentitySnapshot file = goodFile();
    const RuntimeIdentity runtime = runtimeFor(file.contract);
    const CompatibilityResult exact = evaluate(file, runtime);
    QNP_CHECK(exact.state == CompatibilityState::Exact);
    QNP_CHECK(exact.watchable);
    QNP_CHECK(exact.reason == CompatibilityReason::None);

    struct Case {
        const char* name;
        CompatibilityReason reason;
        CompatibilityState state;
    };
    const Case cases[] = {
        { "executable", CompatibilityReason::ExecutableMismatch,
          CompatibilityState::ContentMismatch },
        { "content", CompatibilityReason::ContentManifestMismatch,
          CompatibilityState::ContentMismatch },
        { "catalog", CompatibilityReason::CharacterCatalogMismatch,
          CompatibilityState::ContentMismatch },
        { "runtime", CompatibilityReason::RuntimeSimulationMismatch,
          CompatibilityState::Unsupported },
        { "state region", CompatibilityReason::StateRegionMismatch,
          CompatibilityState::Unsupported },
        { "projection", CompatibilityReason::StateHashProjectionMismatch,
          CompatibilityState::Unsupported },
        { "bootstrap", CompatibilityReason::BootstrapSchemaMismatch,
          CompatibilityState::Unsupported },
        { "events", CompatibilityReason::EventSchemaMismatch,
          CompatibilityState::Unsupported },
        { "fpu", CompatibilityReason::StateHashProjectionMismatch,
          CompatibilityState::Unsupported },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        RuntimeIdentity broken = runtime;
        switch (i) {
            case 0: broken.executableSha256[0] ^= 0x01u; break;
            case 1: broken.contentManifestRoot[31] ^= 0x01u; break;
            case 2: broken.characterCatalogId ^= 1ull; break;
            case 3: broken.runtimeSimulationId ^= 1ull; break;
            case 4: broken.stateRegionFingerprint ^= 1ull; break;
            case 5: broken.stateHashProjectionId ^= 1ull; break;
            case 6: broken.bootstrapCodecVersion ^= 1ull; break;
            case 7: broken.eventSchemaVersion ^= 1ull; break;
            case 8: broken.fpuControlWord ^= 1u; break;
            default: break;
        }
        const CompatibilityResult result = evaluate(file, broken);
        QNP_CHECK(result.state != CompatibilityState::Exact);
        QNP_CHECK(!result.watchable);
        QNP_CHECK(result.state == cases[i].state);
        QNP_CHECK(result.reason == cases[i].reason);
        QNP_CHECK(!result.detail.empty());
    }
}

QNP_TEST(replay_compatibility_reports_simulation_verified_only_when_recorded) {
    ReplayIdentitySnapshot file = goodFile();
    const RuntimeIdentity runtime = runtimeFor(file.contract);
    QNP_CHECK(evaluate(file, runtime).state == CompatibilityState::Exact);
    file.simulationVerified = true;
    const CompatibilityResult verified = evaluate(file, runtime);
    QNP_CHECK(verified.state == CompatibilityState::SimulationVerified);
    QNP_CHECK(verified.watchable);
}

QNP_TEST(replay_compatibility_builtin_allowlist_is_empty) {
    QNP_CHECK(CompatibilityAllowlist::builtin().size() == 0u);
}

QNP_TEST(replay_compatibility_version_numbers_never_imply_compatibility) {
    const ReplayIdentitySnapshot file = goodFile();
    RuntimeIdentity runtime = runtimeFor(file.contract);
    runtime.runtimeSimulationId = file.contract.runtimeSimulationId + 1u;
    runtime.bootstrapCodecVersion = file.contract.bootstrapCodecVersion + 1u;
    runtime.eventSchemaVersion = file.contract.eventSchemaVersion + 1u;
    const CompatibilityResult result = evaluate(file, runtime);
    QNP_CHECK(result.state == CompatibilityState::Unsupported);
    QNP_CHECK(!result.watchable);
}

QNP_TEST(replay_compatibility_allowlist_requires_equal_content) {
    const ReplayIdentitySnapshot file = goodFile();
    RuntimeIdentity runtime = runtimeFor(file.contract);
    runtime.runtimeSimulationId = file.contract.runtimeSimulationId + 1u;
    runtime.stateRegionFingerprint = file.contract.stateRegionFingerprint + 1u;

    CompatibilityAllowEntry entry{};
    entry.fileRuntimeSimulationId = file.contract.runtimeSimulationId;
    entry.runtimeRuntimeSimulationId = runtime.runtimeSimulationId;
    entry.fileStateRegionFingerprint = file.contract.stateRegionFingerprint;
    entry.runtimeStateRegionFingerprint = runtime.stateRegionFingerprint;
    entry.fileBootstrapCodecVersion = file.contract.bootstrapCodecVersion;
    entry.runtimeBootstrapCodecVersion = runtime.bootstrapCodecVersion;
    entry.fileEventSchemaVersion = file.contract.eventSchemaVersion;
    entry.runtimeEventSchemaVersion = runtime.eventSchemaVersion;
    entry.rationale = "decoder retained and tested";
    const CompatibilityAllowlist allowlist({ entry });

    const CompatibilityResult ok = evaluate(file, runtime, allowlist);
    QNP_CHECK(ok.state == CompatibilityState::Compatible);
    QNP_CHECK(ok.watchable);
    QNP_CHECK(ok.detail.find("decoder retained") != std::string::npos);

    RuntimeIdentity moddedContent = runtime;
    moddedContent.contentManifestRoot[0] ^= 0x01u;
    const CompatibilityResult mismatch =
        evaluate(file, moddedContent, allowlist);
    QNP_CHECK(mismatch.state == CompatibilityState::ContentMismatch);
    QNP_CHECK(!mismatch.watchable);

    RuntimeIdentity other = runtime;
    other.runtimeSimulationId += 1u;
    QNP_CHECK(evaluate(file, other, allowlist).state
              == CompatibilityState::Unsupported);
}

QNP_TEST(replay_compatibility_structural_states_dominate) {
    const RuntimeIdentity runtime = runtimeFor(goodFile().contract);

    {
        ReplayIdentitySnapshot file = goodFile();
        file.structurallyValid = false;
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Corrupt);
        QNP_CHECK(result.reason == CompatibilityReason::StructuralCorruption);
    }
    {
        ReplayIdentitySnapshot file = goodFile();
        file.activelyRecording = true;
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Recording);
        QNP_CHECK(!result.watchable);
    }
    {
        ReplayIdentitySnapshot file = goodFile();
        file.prefix.minimumReaderMajor =
            static_cast<uint16_t>(kContainerReaderMajor + 1u);
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Unsupported);
        QNP_CHECK(result.reason == CompatibilityReason::ContainerMajorTooNew);
    }
    {
        ReplayIdentitySnapshot file = goodFile();
        file.hasFooter = false;
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Incomplete);
        QNP_CHECK(result.reason == CompatibilityReason::MissingFooter);
    }
    {
        ReplayIdentitySnapshot file = goodFile();
        file.hasFooter = false;
        file.recoverable = true;
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Incomplete);
        QNP_CHECK(!result.watchable);
    }
    {
        ReplayIdentitySnapshot file = goodFile();
        RuntimeIdentity unknown{};
        unknown.known = false;
        const CompatibilityResult result = evaluate(file, unknown);
        QNP_CHECK(result.state == CompatibilityState::FileIntact);
        QNP_CHECK(!result.watchable);
    }
}

QNP_TEST(replay_compatibility_gates_unqualified_features) {
    const ReplayIdentitySnapshot base = goodFile();
    RuntimeIdentity limited = runtimeFor(base.contract);
    limited.supportsFourPlayer = false;
    limited.supportsCpuFighters = false;
    limited.supportsEffectiveFighterDomain = false;
    limited.supportsPortableCheckpoint = false;

    {
        ReplayIdentitySnapshot file = base;
        file.prefix.featureFlags |= kFeatureFourPlayer;
        const CompatibilityResult result = evaluate(file, limited);
        QNP_CHECK(result.state == CompatibilityState::Unsupported);
        QNP_CHECK(result.reason
                  == CompatibilityReason::UnsupportedTeamConfiguration);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.activeFighterCount = 4u;
        QNP_CHECK(evaluate(file, limited).reason
                  == CompatibilityReason::UnsupportedTeamConfiguration);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.usesNonIdentityRoute = true;
        QNP_CHECK(evaluate(file, limited).reason
                  == CompatibilityReason::UnsupportedTeamConfiguration);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.cpuContract = CpuContract::NativeAiDeterministicV1;
        QNP_CHECK(evaluate(file, limited).reason
                  == CompatibilityReason::UnsupportedCpuContract);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.inputDomain = InputDomain::EffectiveFighterV1;
        QNP_CHECK(evaluate(file, limited).reason
                  == CompatibilityReason::UnsupportedInputDomain);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.hasUnsupportedCheckpoint = true;
        const CompatibilityResult result = evaluate(file, limited);
        QNP_CHECK(result.state == CompatibilityState::Unsupported);
        QNP_CHECK(result.reason
                  == CompatibilityReason::UnsupportedCheckpointCodec);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.hasUnsupportedCheckpoint = true;
        file.checkpointIgnorable = true;
        const CompatibilityResult result =
            evaluate(file, runtimeFor(base.contract));
        QNP_CHECK(result.state == CompatibilityState::Compatible);
        QNP_CHECK(result.watchable);
        QNP_CHECK(result.detail.find("checkpoint") != std::string::npos);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.hasSessionStart = false;
        QNP_CHECK(evaluate(file, runtimeFor(base.contract)).state
                  == CompatibilityState::Unsupported);
    }
}

QNP_TEST(replay_compatibility_is_pure) {
    const ReplayIdentitySnapshot file = goodFile();
    const RuntimeIdentity runtime = runtimeFor(file.contract);
    const CompatibilityResult first = evaluate(file, runtime);
    for (int i = 0; i < 32; ++i) {
        const CompatibilityResult again = evaluate(file, runtime);
        QNP_CHECK(again.state == first.state);
        QNP_CHECK(again.reason == first.reason);
        QNP_CHECK(again.watchable == first.watchable);
        QNP_CHECK(again.detail == first.detail);
    }
}

QNP_TEST(replay_content_manifest_root_is_order_independent) {
    std::vector<ContentFileEntry> a;
    std::vector<ContentFileEntry> b;
    const char* names[] = { "data/chr01.dat", "qoh99.exe", "data/stage/a.bin" };
    for (size_t i = 0; i < 3u; ++i) {
        ContentFileEntry entry{};
        entry.relativePath = names[i];
        entry.sizeBytes = 100u + i;
        entry.sha256 = digestFromSeed(i + 1u);
        a.push_back(entry);
    }
    b.assign(a.rbegin(), a.rend());
    const std::array<uint8_t, 32> rootA = ContentManifestBuilder::computeRoot(a);
    const std::array<uint8_t, 32> rootB = ContentManifestBuilder::computeRoot(b);
    QNP_CHECK(rootA == rootB);

    std::vector<ContentFileEntry> c = a;
    c[0].sizeBytes += 1u;
    QNP_CHECK(ContentManifestBuilder::computeRoot(c) != rootA);
    c = a;
    c[1].sha256[0] ^= 0x01u;
    QNP_CHECK(ContentManifestBuilder::computeRoot(c) != rootA);
    c = a;
    c[2].relativePath += "x";
    QNP_CHECK(ContentManifestBuilder::computeRoot(c) != rootA);
    c = a;
    c.pop_back();
    QNP_CHECK(ContentManifestBuilder::computeRoot(c) != rootA);
}

QNP_TEST(replay_content_policy_excludes_local_outputs) {
    ContentManifestPolicy policy{};
    QNP_CHECK(policy.excludes(L"logs\\session.log", L"session.log"));
    QNP_CHECK(policy.excludes(L"replays\\a.q99r", L"a.q99r"));
    QNP_CHECK(policy.excludes(L"a.q99r.part", L"a.q99r.part"));
    QNP_CHECK(policy.excludes(L"qoh99_netplay.dll", L"qoh99_netplay.dll"));
    QNP_CHECK(policy.excludes(L"DDRAW.DLL", L"DDRAW.DLL"));
    QNP_CHECK(policy.excludes(L"Profiles\\pad.json", L"pad.json"));
    QNP_CHECK(policy.excludes(L"System\\QOHcnf.key", L"QOHcnf.key"));
    QNP_CHECK(policy.excludes(
        L"ddraw_wrapper.ini", L"ddraw_wrapper.ini"));
    QNP_CHECK(policy.excludes(L"qoh99caster.ini", L"qoh99caster.ini"));
    QNP_CHECK(policy.excludes(L"qoh99caster-controls.json",
                              L"qoh99caster-controls.json"));
    QNP_CHECK(policy.excludes(L"manifest.sha256", L"manifest.sha256"));
    QNP_CHECK(policy.excludes(L"launch_wine.sh", L"launch_wine.sh"));
    QNP_CHECK(policy.excludes(L"README.md", L"README.md"));
    QNP_CHECK(policy.excludes(L"README.txt", L"README.txt"));
    QNP_CHECK(policy.excludes(L"ddraw_wrapper.ini.bak-visible",
                              L"ddraw_wrapper.ini.bak-visible"));
    QNP_CHECK(policy.excludes(
        L"qoh99relay.exe.retired-superseded-by-caster-relay-verb",
        L"qoh99relay.exe.retired-superseded-by-caster-relay-verb"));
    QNP_CHECK(policy.excludes(L"qoh99.exe Netplay.lnk",
                              L"qoh99.exe Netplay.lnk"));
    QNP_CHECK(policy.excludes(L"docs\\WINE_PROTON_COMPATIBILITY.md",
                              L"WINE_PROTON_COMPATIBILITY.md"));
    QNP_CHECK(policy.excludes(L"licenses\\SDL3-LICENSE.txt",
                              L"SDL3-LICENSE.txt"));
    QNP_CHECK(!policy.excludes(L"qoh99.exe", L"qoh99.exe"));
    QNP_CHECK(!policy.excludes(L"data\\chr\\akari.dat", L"akari.dat"));
    QNP_CHECK(!policy.excludes(L"FCommand.txt", L"FCommand.txt"));
    QNP_CHECK(!policy.excludes(L"Manual\\index.htm", L"index.htm"));
    QNP_CHECK(!policy.excludes(L"Game Setup ReadMe - updated!.txt",
                               L"Game Setup ReadMe - updated!.txt"));
}

QNP_TEST(replay_content_manifest_scans_a_directory_and_uses_its_cache) {
    TempDir dir(L"content");
    QNP_REQUIRE(dir.valid());
    QNP_REQUIRE(writeBytes(dir.file(L"qoh99.exe"),
                           std::vector<uint8_t>(64u, 0xAAu)));
    QNP_REQUIRE(::CreateDirectoryW((dir.path + L"\\data").c_str(), nullptr) != 0);
    QNP_REQUIRE(writeBytes(dir.path + L"\\data\\chr.dat",
                           std::vector<uint8_t>(32u, 0x5Au)));
    QNP_REQUIRE(::CreateDirectoryW((dir.path + L"\\logs").c_str(), nullptr) != 0);
    QNP_REQUIRE(writeBytes(dir.path + L"\\logs\\x.log",
                           std::vector<uint8_t>(16u, 0x11u)));

    ContentManifestPolicy policy{};
    ContentHashCache cache;
    const ContentManifest first =
        ContentManifestBuilder::build(dir.path, policy, &cache);
    QNP_CHECK(first.complete);
    QNP_CHECK(first.entries.size() == 2u);
    QNP_CHECK(first.filesFromCache == 0u);
    QNP_CHECK(cache.size() == 2u);

    const ContentManifest second =
        ContentManifestBuilder::build(dir.path, policy, &cache);
    QNP_CHECK(second.complete);
    QNP_CHECK(second.root == first.root);
    QNP_CHECK(second.filesFromCache == 2u);

    QNP_REQUIRE(writeBytes(dir.path + L"\\data\\chr.dat",
                           std::vector<uint8_t>(48u, 0x5Bu)));
    const ContentManifest third =
        ContentManifestBuilder::build(dir.path, policy, &cache);
    QNP_CHECK(third.root != first.root);

    TempDir cacheDir(L"contentcache");
    QNP_REQUIRE(cacheDir.valid());
    const std::wstring cachePath = cacheDir.file(L"content.cache");
    QNP_CHECK(cache.save(cachePath));
    ContentHashCache reloaded;
    QNP_CHECK(reloaded.load(cachePath));
    QNP_CHECK(reloaded.size() == cache.size());
    const ContentManifest fourth =
        ContentManifestBuilder::build(dir.path, policy, &reloaded);
    QNP_CHECK(fourth.root == third.root);

    std::vector<uint8_t> corrupt(64u, 0xFFu);
    QNP_REQUIRE(writeBytes(cachePath, corrupt));
    ContentHashCache broken;
    QNP_CHECK(!broken.load(cachePath));
    QNP_CHECK(broken.size() == 0u);
}

QNP_TEST(content_hash_cache_is_excluded_from_the_manifest_it_accelerates) {
    TempDir dir(L"cacheexclusion");
    QNP_REQUIRE(dir.valid());
    QNP_REQUIRE(writeBytes(dir.file(L"qoh99.exe"),
                           std::vector<uint8_t>(64u, 0xAAu)));
    QNP_REQUIRE(writeBytes(dir.file(L"chr.dat"),
                           std::vector<uint8_t>(32u, 0x5Au)));

    ContentManifestPolicy policy{};
    ContentHashCache cache;
    const ContentManifest before =
        ContentManifestBuilder::build(dir.path, policy, &cache);
    QNP_REQUIRE(before.complete);
    QNP_CHECK(before.entries.size() == 2u);

    const std::wstring cachePath =
        ContentHashCache::pathForGameDirectory(dir.path);
    QNP_REQUIRE(::CreateDirectoryW((dir.path + L"\\.cache").c_str(), nullptr)
                != 0);
    QNP_REQUIRE(cache.save(cachePath));
    QNP_REQUIRE(::GetFileAttributesW(cachePath.c_str())
                != INVALID_FILE_ATTRIBUTES);

    const ContentManifest after =
        ContentManifestBuilder::build(dir.path, policy, &cache);
    QNP_CHECK(after.complete);
    QNP_CHECK(after.entries.size() == before.entries.size());
    QNP_CHECK(after.root == before.root);

    QNP_CHECK(policy.excludes(L".cache\\qoh99-content-hashes.bin",
                              L"qoh99-content-hashes.bin"));
    QNP_CHECK(policy.excludes(L".cache\\qoh99-content-hashes.bin.tmp",
                              L"qoh99-content-hashes.bin.tmp"));
}

QNP_TEST(replay_content_manifest_can_be_cancelled) {
    TempDir dir(L"cancel");
    QNP_REQUIRE(dir.valid());
    for (int i = 0; i < 8; ++i) {
        wchar_t name[32];
        _snwprintf_s(name, 32, _TRUNCATE, L"file%02d.dat", i);
        QNP_REQUIRE(writeBytes(dir.file(name), std::vector<uint8_t>(16u, 0x22u)));
    }
    ContentManifestPolicy policy{};
    volatile bool cancelled = true;
    const ContentManifest manifest =
        ContentManifestBuilder::build(dir.path, policy, nullptr, &cancelled);
    QNP_CHECK(!manifest.complete);
    QNP_CHECK(manifest.entries.empty());
}

QNP_TEST(replay_compatibility_rejects_an_interrupted_recording) {
    const RuntimeIdentity runtime = runtimeFor(goodFile().contract);

    {
        ReplayIdentitySnapshot file = goodFile();
        file.hasFooter = false;
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Incomplete);
        QNP_CHECK(result.reason == CompatibilityReason::MissingFooter);
        QNP_CHECK(!result.watchable);
        QNP_CHECK(result.detail.find("finalized footer")
                  != std::string::npos);
    }
    {
        ReplayIdentitySnapshot file = goodFile();
        file.hasFooter = false;
        file.recoverable = true;
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Incomplete);
        QNP_CHECK(!result.watchable);
    }
    {
        const CompatibilityResult result = evaluate(goodFile(), runtime);
        QNP_CHECK(result.state == CompatibilityState::Exact);
        QNP_CHECK(result.watchable);
    }
}

QNP_TEST(replay_compatibility_reports_identity_before_completeness) {
    const ReplayIdentitySnapshot base = goodFile();
    const RuntimeIdentity runtime = runtimeFor(base.contract);

    {
        ReplayIdentitySnapshot file = base;
        file.hasFooter = false;
        file.contract.contentManifestRoot = digestFromSeed(0xD1FFu);
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::ContentMismatch);
        QNP_CHECK(result.reason
                  == CompatibilityReason::ContentManifestMismatch);
        QNP_CHECK(!result.watchable);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.hasFooter = false;
        file.contract.stateRegionFingerprint = 0xBADF00Dull;
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Unsupported);
        QNP_CHECK(result.reason == CompatibilityReason::StateRegionMismatch);
        QNP_CHECK(!result.watchable);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.hasFooter = false;
        file.structurallyValid = false;
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Corrupt);
        QNP_CHECK(!result.watchable);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.hasFooter = false;
        file.activelyRecording = true;
        const CompatibilityResult result = evaluate(file, runtime);
        QNP_CHECK(result.state == CompatibilityState::Recording);
        QNP_CHECK(!result.watchable);
    }
    {
        ReplayIdentitySnapshot file = base;
        file.hasFooter = false;
        RuntimeIdentity unknown{};
        unknown.known = false;
        const CompatibilityResult result = evaluate(file, unknown);
        QNP_CHECK(result.state == CompatibilityState::FileIntact);
        QNP_CHECK(!result.watchable);
    }
}
