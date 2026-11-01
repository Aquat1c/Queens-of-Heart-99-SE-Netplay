#include "ReplaySimulationContractV2.h"

#include "CharacterCatalog.h"

#include <algorithm>

namespace qreplay {
namespace {

constexpr std::array<uint8_t, 32> kSupportedSeExecutableSha256{{
    0xA7u, 0x90u, 0x02u, 0x59u, 0x29u, 0x53u, 0xE8u, 0xD9u,
    0xACu, 0xE3u, 0xE3u, 0x63u, 0xB3u, 0x11u, 0x5Cu, 0xE4u,
    0x45u, 0x2Cu, 0x1Au, 0x07u, 0x89u, 0xEBu, 0x5Bu, 0x17u,
    0xC9u, 0xBCu, 0xB0u, 0x32u, 0xAFu, 0x27u, 0xF1u, 0xF6u,
}};

constexpr std::array<uint8_t, 32> kDecemberExecutableSha256{{
    0xC4u, 0x1Eu, 0x75u, 0x5Eu, 0x66u, 0x09u, 0xB3u, 0x97u,
    0x49u, 0x71u, 0x6Fu, 0xEEu, 0x02u, 0xD3u, 0xA3u, 0x93u,
    0xEFu, 0x8Au, 0x11u, 0xF9u, 0xD8u, 0xBFu, 0x84u, 0x41u,
    0x9Au, 0xA9u, 0x13u, 0x1Bu, 0x5Eu, 0x91u, 0xEBu, 0x63u,
}};

constexpr std::array<const std::array<uint8_t, 32>*, 2>
    kQualifiedExecutables{{
        &kSupportedSeExecutableSha256,
        &kDecemberExecutableSha256,
    }};

bool allZero(const std::array<uint8_t, 32>& value) {
    return std::all_of(
        value.begin(), value.end(),
        [](const uint8_t byte) { return byte == 0u; });
}

}

size_t qualifiedExecutableCountV2() {
    return kQualifiedExecutables.size();
}

const std::array<uint8_t, 32>& qualifiedExecutableSha256V2(
    const size_t index) {
    if (index >= kQualifiedExecutables.size())
        return kSupportedSeExecutableSha256;
    return *kQualifiedExecutables[index];
}

bool isQualifiedExecutableSha256V2(
    const std::array<uint8_t, 32>& executableSha256) {
    for (const std::array<uint8_t, 32>* qualified : kQualifiedExecutables) {
        if (executableSha256 == *qualified) return true;
    }
    return false;
}

const std::array<uint8_t, 32>& supportedSeExecutableSha256V2() {
    return kSupportedSeExecutableSha256;
}

bool isSupportedSeExecutableSha256V2(
    const std::array<uint8_t, 32>& executableSha256) {
    return executableSha256 == kSupportedSeExecutableSha256;
}

SimulationContractV2 makeSeSimulationContractV2(
    const uint64_t runtimeSimulationId,
    const std::array<uint8_t, 32>& executableSha256,
    const std::array<uint8_t, 32>& contentManifestRoot) {
    SimulationContractV2 contract{};
    if (runtimeSimulationId == 0u
        || !isQualifiedExecutableSha256V2(executableSha256)
        || allZero(contentManifestRoot)) {
        return contract;
    }

    contract.executableSha256 = executableSha256;
    contract.nativeBuildIdentity =
        nativeBuildIdentityFromSha256V2(executableSha256);
    contract.runtimeSimulationId = runtimeSimulationId;
    contract.stateRegionSchemaFingerprint =
        kReplayStateRegionSchemaIdV2;
    contract.stateHashProjectionId =
        kReplayStateHashProjectionIdV2;
    contract.stateHashAlgorithmId =
        kReplayStateHashAlgorithmIdCurrent;
    contract.contentManifestRoot = contentManifestRoot;
    contract.bootstrapSchemaId = kReplayBootstrapSchemaIdV2;
    contract.eventSchemaId = kReplayEventSchemaIdV2;
    contract.characterCatalogId =
        CharacterCatalog::forSeBuild().catalogId();
    contract.terminalResultSchemaId =
        kReplayTerminalResultSchemaIdV2;
    contract.compactActionDomainVersion =
        kReplayCompactActionDomainVersionV2;
    return contract;
}

}
