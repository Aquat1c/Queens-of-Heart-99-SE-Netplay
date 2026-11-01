#include "../../netplay/tests/TestHarness.h"

#include "../ReplayCompatibilityIdentityV3.h"

#include "../../content/SimulationContentIdentity.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {

using qnetplay::compat::CompatibilityIdentity;
using qnetplay::compat::IdentityMismatch;
using qreplay::ReplayCompatibilityIdentityDecisionStatusV3;
using qreplay::ReplayCompatibilityIdentityDecodeStatusV3;
using qreplay::ReplayCompatibilityIdentityEnvelopeV3;
using qreplay::ReplayCompatibilityIdentityStatusV3;

void fillDigest(uint8_t* const destination, const size_t count,
                const uint8_t seed) {
    for (size_t index = 0u; index < count; ++index)
        destination[index] = static_cast<uint8_t>(seed + index);
}

CompatibilityIdentity sampleIdentity() {
    CompatibilityIdentity identity{};
    identity.schemaVersion = qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = 10u;
    (void)qnetplay::compat::setProfileId(identity, "qoh99-se1126");
    fillDigest(identity.runtimeBuild.executableSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x11u);
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.manifestFlags = 1u;
    fillDigest(identity.simulationContent.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x41u);
    fillDigest(identity.simulationContent.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x81u);
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.stateSchemaId = 0xA1B2C3D4E5F60718ull;
    identity.modSimulationId = 0x1122334455667788ull;
    identity.timingProfileId = 0x8877665544332211ull;
    return identity;
}

ReplayCompatibilityIdentityEnvelopeV3 sampleEnvelope() {
    ReplayCompatibilityIdentityEnvelopeV3 envelope{};
    envelope.status = ReplayCompatibilityIdentityStatusV3::Attested;
    envelope.identity = sampleIdentity();
    return envelope;
}

}

QNP_TEST(ReplayCompatibilityIdentityV3RoundTripsCompleteAttestation) {
    const ReplayCompatibilityIdentityEnvelopeV3 original = sampleEnvelope();
    std::string error;
    QNP_REQUIRE(qreplay::validateReplayCompatibilityIdentityEnvelopeV3(
        original, error));
    QNP_CHECK(error.empty());

    std::vector<uint8_t> first;
    QNP_REQUIRE(qreplay::encodeReplayCompatibilityIdentityEnvelopeV3(
        original, first, error));
    QNP_CHECK(first.size()
              == qreplay::kReplayCompatibilityIdentityEnvelopeBytesV3);
    QNP_CHECK(first.size() >= 4u && first[0] == 'R' && first[1] == 'C'
              && first[2] == 'I' && first[3] == '3');

    ReplayCompatibilityIdentityEnvelopeV3 decoded{};
    QNP_CHECK(qreplay::decodeReplayCompatibilityIdentityEnvelopeV3(
        first.data(), first.size(), decoded, error)
              == ReplayCompatibilityIdentityDecodeStatusV3::Ok);
    QNP_CHECK(error.empty());
    QNP_CHECK(decoded.status == ReplayCompatibilityIdentityStatusV3::Attested);
    QNP_CHECK(qnetplay::compat::semanticallyEqual(
        original.identity, decoded.identity));

    std::vector<uint8_t> second;
    QNP_REQUIRE(qreplay::encodeReplayCompatibilityIdentityEnvelopeV3(
        decoded, second, error));
    QNP_CHECK(first == second);
    const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
        decoded, original.identity);
    QNP_CHECK(decision.status
              == ReplayCompatibilityIdentityDecisionStatusV3::Exact);
    QNP_CHECK(decision.accepted());
}

QNP_TEST(ReplayCompatibilityIdentityV3PlaysASupersededStateSchema) {
    ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    recorded.identity.stateSchemaId = qnetplay::compat::kStateSchemaV1;
    qnetplay::compat::CompatibilityIdentity runtime = sampleEnvelope().identity;
    runtime.stateSchemaId = qnetplay::compat::kStateSchemaCurrent;

    const auto decision =
        qreplay::evaluateReplayCompatibilityIdentityV3(recorded, runtime);
    QNP_CHECK(decision.status == ReplayCompatibilityIdentityDecisionStatusV3::
                                     SupersededStateSchema);
    QNP_CHECK(decision.accepted());
    QNP_CHECK(!decision.stateProofsAreBinding());
}

QNP_TEST(ReplayCompatibilityIdentityV3RefusesAnUnknownStateSchema) {
    ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    recorded.identity.stateSchemaId = 0xDEADBEEFCAFEF00Dull;
    qnetplay::compat::CompatibilityIdentity runtime = sampleEnvelope().identity;
    runtime.stateSchemaId = qnetplay::compat::kStateSchemaCurrent;

    const auto decision =
        qreplay::evaluateReplayCompatibilityIdentityV3(recorded, runtime);
    QNP_CHECK(decision.status
              == ReplayCompatibilityIdentityDecisionStatusV3::IdentityMismatch);
    QNP_CHECK(!decision.accepted());
}

QNP_TEST(ReplayCompatibilityIdentityV3StillRefusesASupersededSchemaWithOtherDrift) {
    ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    recorded.identity.stateSchemaId = qnetplay::compat::kStateSchemaV1;
    recorded.identity.runtimeBuild.executableSha256[0] ^= 0x5Au;
    qnetplay::compat::CompatibilityIdentity runtime = sampleEnvelope().identity;
    runtime.stateSchemaId = qnetplay::compat::kStateSchemaCurrent;

    const auto decision =
        qreplay::evaluateReplayCompatibilityIdentityV3(recorded, runtime);
    QNP_CHECK(!decision.accepted());
}

QNP_TEST(ReplayCompatibilityIdentityV3TreatsPeTimestampAsForensicMetadata) {
    ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    recorded.identity.runtimeBuild.peTimestamp = 0u;
    std::string error;
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(qreplay::encodeReplayCompatibilityIdentityEnvelopeV3(
        recorded, encoded, error));

    ReplayCompatibilityIdentityEnvelopeV3 decoded{};
    QNP_REQUIRE(qreplay::decodeReplayCompatibilityIdentityEnvelopeV3(
        encoded.data(), encoded.size(), decoded, error)
                == ReplayCompatibilityIdentityDecodeStatusV3::Ok);
    QNP_CHECK(decoded.identity.runtimeBuild.peTimestamp == 0u);

    CompatibilityIdentity runtime = sampleIdentity();
    runtime.runtimeBuild.peTimestamp ^= 0xFFFFFFFFu;
    const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
        decoded, runtime);
    QNP_CHECK(decision.status
              == ReplayCompatibilityIdentityDecisionStatusV3::Exact);
    QNP_CHECK(decision.mismatch == IdentityMismatch::None);
    QNP_CHECK(decision.accepted());
}

QNP_TEST(ReplayCompatibilityIdentityV3RejectsWrongSimulationContentRoot) {
    const ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    CompatibilityIdentity runtime = sampleIdentity();
    runtime.simulationContent.simulationRootSha256[0] ^= 0x5Au;
    QNP_REQUIRE(qnetplay::compat::isValidCompatibilityIdentity(runtime));

    const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
        recorded, runtime);
    QNP_CHECK(decision.status
              == ReplayCompatibilityIdentityDecisionStatusV3::IdentityMismatch);
    QNP_CHECK(decision.mismatch == IdentityMismatch::SimulationRoot);
    QNP_CHECK(!decision.accepted());
}

QNP_TEST(ReplayCompatibilityIdentityV3MarksLegacyAsExplicitlyUnattested) {
    const ReplayCompatibilityIdentityEnvelopeV3 legacy =
        qreplay::makeLegacyUnattestedReplayCompatibilityIdentityV3();
    QNP_CHECK(legacy.status
              == ReplayCompatibilityIdentityStatusV3::LegacyUnattested);
    QNP_CHECK(legacy.legacyUnattested());
    QNP_CHECK(!legacy.attested());

    std::string error;
    QNP_CHECK(!qreplay::validateReplayCompatibilityIdentityEnvelopeV3(
        legacy, error));
    QNP_CHECK(error.find("legacy") != std::string::npos);

    const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
        legacy, sampleIdentity());
    QNP_CHECK(decision.status
              == ReplayCompatibilityIdentityDecisionStatusV3::LegacyUnattested);
    QNP_CHECK(!decision.accepted());

    std::vector<uint8_t> untouched{0xA5u};
    QNP_CHECK(!qreplay::encodeReplayCompatibilityIdentityEnvelopeV3(
        legacy, untouched, error));
    QNP_CHECK(untouched.size() == 1u && untouched[0] == 0xA5u);

    ReplayCompatibilityIdentityEnvelopeV3 decoded = legacy;
    QNP_CHECK(qreplay::decodeReplayCompatibilityIdentityEnvelopeV3(
        nullptr, 0u, decoded, error)
              == ReplayCompatibilityIdentityDecodeStatusV3::NullInput);
    QNP_CHECK(decoded.status == ReplayCompatibilityIdentityStatusV3::Invalid);
}

QNP_TEST(ReplayCompatibilityIdentityV3PlaysAPreNarrowingContentRoot) {
    const ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    CompatibilityIdentity runtime = sampleIdentity();
    fillDigest(runtime.simulationContent.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xC1u);
    fillDigest(runtime.simulationContent.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xD1u);
    runtime.simulationContent.manifestSchemaVersion =
        qoh99::content::kSimulationContentSchemaVersion;
    QNP_REQUIRE(runtime.simulationContent.manifestSchemaVersion
                != recorded.identity.simulationContent.manifestSchemaVersion);
    QNP_REQUIRE(qnetplay::compat::isValidCompatibilityIdentity(runtime));

    QNP_CHECK(qreplay::evaluateReplayCompatibilityIdentityV3(recorded, runtime)
                  .status
              == ReplayCompatibilityIdentityDecisionStatusV3::IdentityMismatch);

    qreplay::LegacyContentRootsV3 legacy{};
    fillDigest(legacy.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x41u);
    fillDigest(legacy.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x81u);
    legacy.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;
    legacy.valid = true;

    const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
        recorded, runtime, &legacy);
    QNP_CHECK(decision.status == ReplayCompatibilityIdentityDecisionStatusV3::
                                     LegacyContentRootMembership);
    QNP_CHECK(decision.accepted());
    QNP_CHECK(decision.stateProofsAreBinding());
}

QNP_TEST(ReplayCompatibilityIdentityV3LegacyRootsRescueNothingElse) {
    const ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    qreplay::LegacyContentRootsV3 legacy{};
    fillDigest(legacy.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x41u);
    fillDigest(legacy.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x81u);
    legacy.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;
    legacy.valid = true;

    {
        CompatibilityIdentity runtime = sampleIdentity();
        fillDigest(runtime.simulationContent.simulationRootSha256,
                   qnetplay::compat::kCompatibilitySha256Bytes, 0xC1u);
        qreplay::LegacyContentRootsV3 differentChr = legacy;
        differentChr.simulationRootSha256[3] ^= 0x5Au;
        const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
            recorded, runtime, &differentChr);
        QNP_CHECK(decision.status == ReplayCompatibilityIdentityDecisionStatusV3
                                         ::IdentityMismatch);
        QNP_CHECK(!decision.accepted());
    }

    {
        CompatibilityIdentity runtime = sampleIdentity();
        fillDigest(runtime.simulationContent.simulationRootSha256,
                   qnetplay::compat::kCompatibilitySha256Bytes, 0xC1u);
        runtime.runtimeBuild.executableSha256[0] ^= 0x5Au;
        const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
            recorded, runtime, &legacy);
        QNP_CHECK(decision.status == ReplayCompatibilityIdentityDecisionStatusV3
                                         ::IdentityMismatch);
        QNP_CHECK(!decision.accepted());
    }

    {
        CompatibilityIdentity runtime = sampleIdentity();
        fillDigest(runtime.simulationContent.simulationRootSha256,
                   qnetplay::compat::kCompatibilitySha256Bytes, 0xC1u);
        qreplay::LegacyContentRootsV3 unavailable = legacy;
        unavailable.valid = false;
        const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
            recorded, runtime, &unavailable);
        QNP_CHECK(decision.status == ReplayCompatibilityIdentityDecisionStatusV3
                                         ::IdentityMismatch);
        QNP_CHECK(!decision.accepted());
    }
}

QNP_TEST(ReplayCompatibilityIdentityV3PlaysNewRootsRecordedUnderTheOldSchema) {
    ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    recorded.identity.simulationContent.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;

    CompatibilityIdentity runtime = sampleIdentity();
    runtime.simulationContent.manifestSchemaVersion =
        qoh99::content::kSimulationContentSchemaVersion;
    QNP_REQUIRE(qnetplay::compat::isValidCompatibilityIdentity(runtime));

    qreplay::LegacyContentRootsV3 legacy{};
    fillDigest(legacy.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xE1u);
    fillDigest(legacy.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xF1u);
    legacy.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;
    legacy.valid = true;

    const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
        recorded, runtime, &legacy);
    QNP_CHECK(decision.status == ReplayCompatibilityIdentityDecisionStatusV3::
                                     LegacyContentRootMembership);
    QNP_CHECK(decision.accepted());
    QNP_CHECK(decision.stateProofsAreBinding());
}

QNP_TEST(ReplayCompatibilityIdentityV3RefusesAnUnknownManifestSchema) {
    ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    recorded.identity.simulationContent.manifestSchemaVersion = 977u;
    CompatibilityIdentity runtime = sampleIdentity();
    runtime.simulationContent.manifestSchemaVersion =
        qoh99::content::kSimulationContentSchemaVersion;

    qreplay::LegacyContentRootsV3 legacy{};
    fillDigest(legacy.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xE1u);
    fillDigest(legacy.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xF1u);
    legacy.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;
    legacy.valid = true;

    const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
        recorded, runtime, &legacy);
    QNP_CHECK(decision.status
              == ReplayCompatibilityIdentityDecisionStatusV3::IdentityMismatch);
    QNP_CHECK(!decision.accepted());
}

QNP_TEST(ReplayCompatibilityIdentityV3PlaysARecordingNeedingBothAllowances) {
    ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    recorded.identity.simulationContent.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;
    recorded.identity.stateSchemaId = qnetplay::compat::kStateSchemaV1;

    CompatibilityIdentity runtime = sampleIdentity();
    runtime.simulationContent.manifestSchemaVersion =
        qoh99::content::kSimulationContentSchemaVersion;
    runtime.stateSchemaId = qnetplay::compat::kStateSchemaCurrent;
    fillDigest(runtime.simulationContent.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xC1u);
    fillDigest(runtime.simulationContent.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xD1u);
    QNP_REQUIRE(qnetplay::compat::isValidCompatibilityIdentity(runtime));

    qreplay::LegacyContentRootsV3 legacy{};
    fillDigest(legacy.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x41u);
    fillDigest(legacy.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x81u);
    legacy.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;
    legacy.valid = true;

    const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
        recorded, runtime, &legacy);
    QNP_CHECK(decision.accepted());
    QNP_CHECK(decision.status == ReplayCompatibilityIdentityDecisionStatusV3::
                                     SupersededStateSchema);
    QNP_CHECK(!decision.stateProofsAreBinding());
}

QNP_TEST(ReplayCompatibilityIdentityV3ComposedAllowancesStillRefuseRealDrift) {
    ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    recorded.identity.simulationContent.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;
    recorded.identity.stateSchemaId = qnetplay::compat::kStateSchemaV1;
    recorded.identity.runtimeBuild.executableSha256[7] ^= 0x5Au;

    CompatibilityIdentity runtime = sampleIdentity();
    runtime.simulationContent.manifestSchemaVersion =
        qoh99::content::kSimulationContentSchemaVersion;
    runtime.stateSchemaId = qnetplay::compat::kStateSchemaCurrent;
    fillDigest(runtime.simulationContent.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xC1u);
    fillDigest(runtime.simulationContent.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0xD1u);

    qreplay::LegacyContentRootsV3 legacy{};
    fillDigest(legacy.simulationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x41u);
    fillDigest(legacy.validationRootSha256,
               qnetplay::compat::kCompatibilitySha256Bytes, 0x81u);
    legacy.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;
    legacy.valid = true;

    const auto decision = qreplay::evaluateReplayCompatibilityIdentityV3(
        recorded, runtime, &legacy);
    QNP_CHECK(decision.status
              == ReplayCompatibilityIdentityDecisionStatusV3::IdentityMismatch);
    QNP_CHECK(!decision.accepted());
}

QNP_TEST(ReplayCompatibilityIdentityV3PlaysAV2SupersededStateSchema) {
    ReplayCompatibilityIdentityEnvelopeV3 recorded = sampleEnvelope();
    recorded.identity.stateSchemaId = qnetplay::compat::kStateSchemaV2;
    qnetplay::compat::CompatibilityIdentity runtime = sampleEnvelope().identity;
    runtime.stateSchemaId = qnetplay::compat::kStateSchemaCurrent;

    const auto decision =
        qreplay::evaluateReplayCompatibilityIdentityV3(recorded, runtime);
    QNP_CHECK(decision.status
              == ReplayCompatibilityIdentityDecisionStatusV3::SupersededStateSchema);
    QNP_CHECK(decision.accepted());
    QNP_CHECK(!decision.stateProofsAreBinding());
}

QNP_TEST(ReplayCompatibilityIdentityV3StateSchemaVersioning) {
    QNP_CHECK(qnetplay::compat::kStateSchemaCurrent
              == qnetplay::compat::kStateSchemaV5);
    QNP_CHECK(qnetplay::compat::isSupersededStateSchema(
        qnetplay::compat::kStateSchemaV1));
    QNP_CHECK(qnetplay::compat::isSupersededStateSchema(
        qnetplay::compat::kStateSchemaV2));
    QNP_CHECK(qnetplay::compat::isSupersededStateSchema(
        qnetplay::compat::kStateSchemaV3));
    QNP_CHECK(qnetplay::compat::isSupersededStateSchema(
        qnetplay::compat::kStateSchemaV4));
    QNP_CHECK(!qnetplay::compat::isSupersededStateSchema(
        qnetplay::compat::kStateSchemaV5));
    QNP_CHECK(qnetplay::compat::kStateSchemaV4
              != qnetplay::compat::kStateSchemaV3);
    QNP_CHECK(qnetplay::compat::kStateSchemaV5
              != qnetplay::compat::kStateSchemaV4);
}

