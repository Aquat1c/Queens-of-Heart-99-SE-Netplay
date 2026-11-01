#include "TestHarness.h"

#include "replay/ReplaySimulationContractV2.h"

using namespace qreplay;

QNP_TEST(ReplaySimulationContractV2BindsTheAuditedSeBuild) {
    std::array<uint8_t, 32> content{};
    content.fill(0x5Au);
    const SimulationContractV2 contract =
        makeSeSimulationContractV2(
            0x1020304050607080ull,
            supportedSeExecutableSha256V2(), content);

    QNP_CHECK(isValidSimulationContractV2(contract));
    QNP_CHECK(
        contract.executableSha256
        == supportedSeExecutableSha256V2());
    QNP_CHECK(
        contract.nativeBuildIdentity
        == kSeNativeBuildIdentityV2);
    QNP_CHECK(
        contract.stateRegionSchemaFingerprint
        == kReplayStateRegionSchemaIdV2);
    QNP_CHECK(
        contract.stateHashProjectionId
        == kReplayStateHashProjectionIdV2);
    QNP_CHECK(
        contract.stateHashAlgorithmId
        == kReplayStateHashAlgorithmIdCurrent);
    QNP_CHECK(replayStateHashAlgorithmSupportedV2(
        contract.stateHashAlgorithmId));
    QNP_CHECK(
        contract.bootstrapSchemaId
        == kReplayBootstrapSchemaIdV2);
    QNP_CHECK(
        contract.eventSchemaId == kReplayEventSchemaIdV2);
    QNP_CHECK(
        contract.terminalResultSchemaId
        == kReplayTerminalResultSchemaIdV2);
    QNP_CHECK(
        contract.compactActionDomainVersion
        == kReplayCompactActionDomainVersionV2);
}

QNP_TEST(ReplaySimulationContractV2RejectsInventedIdentity) {
    std::array<uint8_t, 32> content{};
    content.fill(0x33u);

    std::array<uint8_t, 32> wrongExecutable =
        supportedSeExecutableSha256V2();
    wrongExecutable[31] ^= 1u;
    QNP_CHECK(!isSupportedSeExecutableSha256V2(wrongExecutable));
    QNP_CHECK(!isQualifiedExecutableSha256V2(wrongExecutable));
    QNP_CHECK(!isValidSimulationContractV2(
        makeSeSimulationContractV2(
            1u, wrongExecutable, content)));

    std::array<uint8_t, 32> zeroContent{};
    QNP_CHECK(!isValidSimulationContractV2(
        makeSeSimulationContractV2(
            1u, supportedSeExecutableSha256V2(),
            zeroContent)));
    QNP_CHECK(!isValidSimulationContractV2(
        makeSeSimulationContractV2(
            0u, supportedSeExecutableSha256V2(), content)));
}

QNP_TEST(ReplaySimulationContractV2CoversEveryQualifiedBuild) {
    std::array<uint8_t, 32> content{};
    content.fill(0x5Au);

    QNP_REQUIRE(qualifiedExecutableCountV2() >= 2u);
    for (size_t i = 0; i < qualifiedExecutableCountV2(); ++i) {
        const std::array<uint8_t, 32>& exe = qualifiedExecutableSha256V2(i);
        QNP_CHECK(isQualifiedExecutableSha256V2(exe));
        const SimulationContractV2 contract =
            makeSeSimulationContractV2(0x1020304050607080ull, exe, content);
        QNP_CHECK(isValidSimulationContractV2(contract));
        QNP_CHECK(contract.executableSha256 == exe);
        QNP_CHECK(contract.nativeBuildIdentity
                  == nativeBuildIdentityFromSha256V2(exe));
    }

    for (size_t i = 0; i < qualifiedExecutableCountV2(); ++i) {
        for (size_t j = i + 1u; j < qualifiedExecutableCountV2(); ++j) {
            QNP_CHECK(qualifiedExecutableSha256V2(i)
                      != qualifiedExecutableSha256V2(j));
            QNP_CHECK(
                nativeBuildIdentityFromSha256V2(qualifiedExecutableSha256V2(i))
                != nativeBuildIdentityFromSha256V2(
                       qualifiedExecutableSha256V2(j)));
        }
    }
}

QNP_TEST(ReplaySimulationContractV2KeepsNovembersPublishedBuildIdentity) {
    QNP_CHECK(nativeBuildIdentityFromSha256V2(supportedSeExecutableSha256V2())
              == kSeNativeBuildIdentityV2);

    std::array<uint8_t, 32> content{};
    content.fill(0x5Au);
    const SimulationContractV2 november = makeSeSimulationContractV2(
        0x1020304050607080ull, supportedSeExecutableSha256V2(), content);
    QNP_CHECK(november.nativeBuildIdentity == kSeNativeBuildIdentityV2);
}
