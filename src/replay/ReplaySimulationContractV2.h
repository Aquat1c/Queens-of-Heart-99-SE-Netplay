#pragma once

#include "ReplayTypes.h"

#include <array>
#include <cstdint>

namespace qreplay {

size_t qualifiedExecutableCountV2();
const std::array<uint8_t, 32>& qualifiedExecutableSha256V2(size_t index);

bool isQualifiedExecutableSha256V2(
    const std::array<uint8_t, 32>& executableSha256);

// November/1126: qoh99.exe, 704512 bytes, COFF timestamp 0x3A19A8A9.
const std::array<uint8_t, 32>& supportedSeExecutableSha256V2();

constexpr uint64_t kSeNativeBuildIdentityV2 = 0xA79002592953E8D9ull;

constexpr uint64_t nativeBuildIdentityFromSha256V2(
    const std::array<uint8_t, 32>& executableSha256) {
    uint64_t identity = 0u;
    for (size_t i = 0; i < 8u; ++i)
        identity = (identity << 8u) | executableSha256[i];
    return identity;
}

constexpr uint64_t replayContractDomainIdV2(const char* text) {
    uint64_t hash = 1469598103934665603ull;
    while (text != nullptr && *text != '\0') {
        hash ^= static_cast<uint8_t>(*text++);
        hash *= 1099511628211ull;
    }
    return hash;
}

constexpr uint64_t kReplayStateRegionSchemaIdV2 =
    replayContractDomainIdV2(
        "qoh99-se-state-region-schema-v2:"
        "gameplay+frontend+fpu+virtual-audio");
constexpr uint64_t kReplayStateHashProjectionIdV2 =
    replayContractDomainIdV2(
        "qoh99-se-state-hash-projection-v2:"
        "frontend-or-gameplay+fpu+virtual-audio");
constexpr uint64_t kReplayStateHashAlgorithmIdV2 =
    replayContractDomainIdV2("qoh99-replay-state-hash:fnv1a64-le-v1");
constexpr uint64_t kReplayStateHashAlgorithmIdBlock64V1 =
    replayContractDomainIdV2("qoh99-replay-state-hash:block64-le-v1");

// Must name the primitive RollbackManager::kHashAlgorithmCurrent computes, or new recordings misdeclare their own hashes.
constexpr uint64_t kReplayStateHashAlgorithmIdCurrent =
    kReplayStateHashAlgorithmIdBlock64V1;

// Never remove an entry: doing so makes every recording that used it unwatchable.
constexpr bool replayStateHashAlgorithmSupportedV2(const uint64_t id) {
    return id == kReplayStateHashAlgorithmIdV2
        || id == kReplayStateHashAlgorithmIdBlock64V1;
}

inline bool replayContractAgreesForPlaybackV2(
    const SimulationContractV2& recorded,
    const SimulationContractV2& local) {
    if (!replayStateHashAlgorithmSupportedV2(recorded.stateHashAlgorithmId))
        return false;
    SimulationContractV2 normalized = recorded;
    normalized.stateHashAlgorithmId = local.stateHashAlgorithmId;
    return normalized == local;
}
constexpr uint64_t kReplayBootstrapSchemaIdV2 =
    replayContractDomainIdV2(
        "qoh99-se-match-active-direct-boot-v1");
constexpr uint64_t kReplayEventSchemaIdV2 =
    replayContractDomainIdV2(
        "qoh99-se-replay-events-v2:"
        "game-start+round-start+round-end+game-result");
constexpr uint64_t kReplayTerminalResultSchemaIdV2 =
    replayContractDomainIdV2(
        "qoh99-se-replay-terminal-result-v2:"
        "exclusive-end+presentation-ack");
constexpr uint32_t kReplayCompactActionDomainVersionV2 = 1u;

bool isSupportedSeExecutableSha256V2(
    const std::array<uint8_t, 32>& executableSha256);

SimulationContractV2 makeSeSimulationContractV2(
    uint64_t runtimeSimulationId,
    const std::array<uint8_t, 32>& executableSha256,
    const std::array<uint8_t, 32>& contentManifestRoot);

}
