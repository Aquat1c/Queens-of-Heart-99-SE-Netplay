#include "TestHarness.h"

#include "ReplayTestFixtures.h"

#include "client/replay/ReplayBroker.h"

#include <string>
#include <vector>

using namespace qreplay;
using namespace qclient::replay;
using namespace qclient::replay::test;

namespace {

RuntimeIdentity runtimeMatching(const FixtureOptions& options) {
    const SimulationContract contract = makeContract(options);
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

}

QNP_TEST(replay_broker_refuses_a_recording_that_never_finished) {
    TempDir dir(L"inc_play");
    QNP_REQUIRE(dir.valid());

    FixtureOptions options{};
    options.matchCount = 2u;
    options.framesPerMatch = 12u;
    options.finalize = false;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    const std::wstring path = dir.file(L"interrupted.q99r");
    QNP_REQUIRE(writeBytes(path, fixture.bytes));

    ReplayBroker broker;
    BrokerSelection selection;
    selection.path = path;
    const BrokerPrepareResult result =
        broker.prepare(selection, runtimeMatching(options));

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.parse.status == ParseStatus::MissingFooter);
    QNP_CHECK(result.detail.find("finalized") != std::string::npos);
}

QNP_TEST(replay_broker_refuses_a_torn_file) {
    TempDir dir(L"inc_torn");
    QNP_REQUIRE(dir.valid());

    FixtureOptions options{};
    options.matchCount = 2u;
    options.framesPerMatch = 12u;
    options.finalize = false;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    QNP_REQUIRE(fixture.chunkOffsets.size() >= 4u);

    const size_t finalChunkStart =
        static_cast<size_t>(fixture.chunkOffsets[fixture.chunkOffsets.size() - 2u]);
    const size_t finalChunkEnd = static_cast<size_t>(fixture.chunkOffsets.back());
    QNP_REQUIRE(finalChunkEnd == fixture.bytes.size());
    QNP_REQUIRE(finalChunkEnd - finalChunkStart > 8u);
    const size_t cut = finalChunkStart + 8u;
    std::vector<uint8_t> torn(fixture.bytes.begin(),
                              fixture.bytes.begin() + cut);
    const std::wstring path = dir.file(L"torn.q99r");
    QNP_REQUIRE(writeBytes(path, torn));

    ReplayBroker broker;
    BrokerSelection selection;
    selection.path = path;
    const BrokerPrepareResult result =
        broker.prepare(selection, runtimeMatching(options));

    QNP_CHECK(!result.ok);
    QNP_CHECK(!result.detail.empty());
}

QNP_TEST(replay_broker_refuses_a_sealed_file_whose_bytes_changed) {
    TempDir dir(L"inc_sealed");
    QNP_REQUIRE(dir.valid());

    FixtureOptions options{};
    options.matchCount = 1u;
    options.framesPerMatch = 8u;
    options.finalize = true;
    Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);

    QNP_REQUIRE(fixture.chunkOffsets.size() >= 4u);
    const size_t damage =
        static_cast<size_t>(fixture.chunkOffsets[1]) + 8u;
    QNP_REQUIRE(damage < fixture.bytes.size());
    fixture.bytes[damage] ^= 0x5Au;
    const std::wstring path = dir.file(L"sealed_damaged.q99r");
    QNP_REQUIRE(writeBytes(path, fixture.bytes));

    ReplayBroker broker;
    BrokerSelection selection;
    selection.path = path;
    const BrokerPrepareResult result =
        broker.prepare(selection, runtimeMatching(options));

    QNP_CHECK(!result.ok);
    QNP_CHECK(result.detail.find("finalized") != std::string::npos);
}

QNP_TEST(replay_broker_refuses_an_interrupted_file_with_no_playable_frames) {
    TempDir dir(L"inc_empty");
    QNP_REQUIRE(dir.valid());

    FixtureOptions options{};
    options.finalize = false;
    const Fixture fixture = makeFixture(options);
    QNP_REQUIRE(fixture.ok);
    QNP_REQUIRE(!fixture.chunkOffsets.empty());

    QNP_REQUIRE(fixture.chunkOffsets.size() >= 2u);
    const size_t cut = static_cast<size_t>(fixture.chunkOffsets[1]);
    std::vector<uint8_t> metadataOnly(fixture.bytes.begin(),
                                      fixture.bytes.begin() + cut);
    const std::wstring path = dir.file(L"metadata_only.q99r");
    QNP_REQUIRE(writeBytes(path, metadataOnly));

    ReplayBroker broker;
    BrokerSelection selection;
    selection.path = path;
    const BrokerPrepareResult result =
        broker.prepare(selection, runtimeMatching(options));

    QNP_CHECK(!result.ok);
    QNP_CHECK(!result.detail.empty());
}
