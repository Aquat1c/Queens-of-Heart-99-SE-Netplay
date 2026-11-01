#pragma once

#include "../ResilientSpectatorDistribution.h"

namespace qnetplay::spectator::resilient::test {

inline StreamCompatibilityIdentity syntheticStreamIdentityForTests(
    const uint64_t seed) {
    qnetplay::compat::CompatibilityIdentityWire compatibility{};
    compatibility.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    compatibility.protocolVersion = 1u;
    (void)qnetplay::compat::setProfileId(compatibility, "qoh99-se1126");
    for (size_t index = 0; index < qnetplay::compat::kCompatibilitySha256Bytes;
         ++index) {
        const uint8_t value = static_cast<uint8_t>((seed + index * 29u) | 1u);
        compatibility.runtimeBuild.executableSha256[index] = value;
        compatibility.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(value ^ 0x5Au);
        compatibility.simulationContent.validationRootSha256[index] =
            static_cast<uint8_t>(value ^ 0xA5u);
    }
    compatibility.runtimeBuild.executableSize = 704512u + (seed & 0xFFu);
    compatibility.runtimeBuild.peTimestamp =
        static_cast<uint32_t>(0x3A19A8A9u ^ seed);
    compatibility.simulationContent.manifestSchemaVersion = 1u;
    compatibility.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    compatibility.simulationContent.entryCount = 1u;
    compatibility.simulationContent.requiredEntryCount = 1u;
    compatibility.stateSchemaId = 0x5354415445000001ull ^ seed;
    compatibility.modSimulationId = 0x4D4F4453494D0001ull ^ seed;
    compatibility.timingProfileId = 0x54494D494E470001ull ^ seed;

    StreamCompatibilityIdentity out{};
    (void)makeStreamCompatibilityIdentity(
        compatibility, 0x5345540000000001ull ^ seed,
        0x45504F4348000001ull ^ seed,
        0x524F4F5400000001ull ^ seed, out);
    return out;
}

}
