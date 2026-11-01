#pragma once

#include <cstddef>
#include <cstdint>

#include "../content/SimulationContentIdentity.h"

namespace qnetplay::compat {

constexpr uint32_t kCompatibilityIdentitySchemaVersion = 1u;
constexpr size_t kCompatibilityProfileIdBytes = 32u;
constexpr size_t kCompatibilitySha256Bytes = 32u;

// Canonical wire size: fields are written in order as little-endian; the packed image is valid only in same-ABI records that assert this size.
constexpr size_t kCompatibilityIdentityWireBytes = 192u;

#pragma pack(push, 1)
struct RuntimeBuildIdentity {
    char profileId[kCompatibilityProfileIdBytes]{};
    uint8_t executableSha256[kCompatibilitySha256Bytes]{};
    uint64_t executableSize = 0u;
    uint32_t peTimestamp = 0u;
};

struct SimulationContentIdentity {
    uint32_t manifestSchemaVersion = 0u;
    uint32_t manifestFlags = 0u;
    uint8_t simulationRootSha256[kCompatibilitySha256Bytes]{};
    uint8_t validationRootSha256[kCompatibilitySha256Bytes]{};
    uint32_t entryCount = 0u;
    uint32_t requiredEntryCount = 0u;
};

struct CompatibilityIdentity {
    uint32_t schemaVersion = 0u;
    uint32_t protocolVersion = 0u;
    RuntimeBuildIdentity runtimeBuild{};
    SimulationContentIdentity simulationContent{};
    uint64_t stateSchemaId = 0u;
    uint64_t modSimulationId = 0u;
    uint64_t timingProfileId = 0u;
    uint32_t reserved = 0u;
};
#pragma pack(pop)

static_assert(sizeof(RuntimeBuildIdentity) == 76u,
              "compatibility build identity has a fixed wire layout");
static_assert(sizeof(SimulationContentIdentity) == 80u,
              "compatibility content identity has a fixed wire layout");
static_assert(sizeof(CompatibilityIdentity) == kCompatibilityIdentityWireBytes,
              "compatibility identity has a fixed wire layout");

using CompatibilityIdentityWire = CompatibilityIdentity;

constexpr uint64_t compatibilityDomainId(const char* text) {
    uint64_t value = 1469598103934665603ull;
    while (text != nullptr && *text != '\0') {
        value ^= static_cast<uint8_t>(*text++);
        value *= 1099511628211ull;
    }
    return value;
}

constexpr uint64_t kStateSchemaV1 =
    compatibilityDomainId("qoh99-se-state-schema-content-identity-v1");
constexpr uint64_t kStateSchemaV2 =
    compatibilityDomainId("qoh99-se-state-schema-content-identity-v2");
constexpr uint64_t kStateSchemaV3 =
    compatibilityDomainId("qoh99-se-state-schema-content-identity-v3");
constexpr uint64_t kStateSchemaV4 =
    compatibilityDomainId("qoh99-se-state-schema-content-identity-v4");
constexpr uint64_t kStateSchemaV5 =
    compatibilityDomainId("qoh99-se-state-schema-content-identity-v5");
constexpr uint64_t kStateSchemaCurrent = kStateSchemaV5;

inline bool isSupersededStateSchema(const uint64_t id) {
    return id == kStateSchemaV1 || id == kStateSchemaV2
        || id == kStateSchemaV3 || id == kStateSchemaV4;
}

enum class IdentityMismatch : uint8_t {
    None = 0,
    LocalIdentityInvalid,
    PeerIdentityInvalid,
    IdentitySchemaVersion,
    ProtocolVersion,
    ProfileId,
    ExecutableSha256,
    ExecutableSize,
    ManifestSchemaVersion,
    SimulationRoot,
    ValidationRoot,
    EntryCount,
    RequiredEntryCount,
    ManifestFlags,
    StateSchema,
    ModSimulation,
    TimingProfile,
};

inline bool allZero(const uint8_t* const bytes, const size_t count) {
    if (bytes == nullptr && count != 0u) return false;
    for (size_t i = 0u; i < count; ++i)
        if (bytes[i] != 0u) return false;
    return true;
}

inline bool equalBytes(const uint8_t* const left,
                       const uint8_t* const right,
                       const size_t count) {
    if ((left == nullptr || right == nullptr) && count != 0u) return false;
    for (size_t i = 0u; i < count; ++i)
        if (left[i] != right[i]) return false;
    return true;
}

inline bool validProfileCharacter(const char value) {
    return (value >= 'a' && value <= 'z')
        || (value >= '0' && value <= '9') || value == '-';
}

inline bool isValidProfileId(
    const char (&profileId)[kCompatibilityProfileIdBytes]) {
    bool terminated = false;
    size_t count = 0u;
    for (size_t i = 0u; i < kCompatibilityProfileIdBytes; ++i) {
        const char value = profileId[i];
        if (terminated) {
            if (value != '\0') return false;
            continue;
        }
        if (value == '\0') {
            terminated = true;
            continue;
        }
        if (!validProfileCharacter(value)) return false;
        ++count;
    }
    return terminated && count != 0u;
}

inline bool setProfileId(
    char (&destination)[kCompatibilityProfileIdBytes], const char* source) {
    if (source == nullptr || *source == '\0') return false;

    char canonical[kCompatibilityProfileIdBytes]{};
    size_t count = 0u;
    while (source[count] != '\0') {
        if (count + 1u >= kCompatibilityProfileIdBytes
            || !validProfileCharacter(source[count])) {
            return false;
        }
        canonical[count] = source[count];
        ++count;
    }
    if (count == 0u) return false;
    for (size_t i = 0u; i < kCompatibilityProfileIdBytes; ++i)
        destination[i] = canonical[i];
    return true;
}

inline bool setProfileId(CompatibilityIdentity& identity,
                         const char* const source) {
    return setProfileId(identity.runtimeBuild.profileId, source);
}

inline bool hasAdmissibleSimulationManifestFlags(
    const uint32_t manifestFlags) {
    using namespace qoh99::content;
    constexpr uint32_t kComplete = ManifestFlagComplete;
    constexpr uint32_t kKnownFlags = ManifestFlagComplete
        | ManifestFlagMissingRequired
        | ManifestFlagUnreadableRequired
        | ManifestFlagDuplicateNormalizedPath
        | ManifestFlagExtraSimulationResource
        | ManifestFlagUnknownProfile
        | ManifestFlagRootMismatch
        | ManifestFlagNativeValidationMismatch
        | ManifestFlagExtraScanIncomplete
        | ManifestFlagUnreviewedPotentiallyRelevantExtra;
    constexpr uint32_t kFailureFlags = kKnownFlags & ~kComplete;
    return (manifestFlags & kComplete) != 0u
        && (manifestFlags & kFailureFlags) == 0u
        && (manifestFlags & ~kKnownFlags) == 0u;
}

inline bool isStructurallyValidCompatibilityIdentity(
    const CompatibilityIdentity& identity) {
    return identity.schemaVersion != 0u
        && identity.protocolVersion != 0u
        && isValidProfileId(identity.runtimeBuild.profileId)
        && !allZero(identity.runtimeBuild.executableSha256,
                    kCompatibilitySha256Bytes)
        && identity.runtimeBuild.executableSize != 0u
        && identity.simulationContent.manifestSchemaVersion != 0u
        && hasAdmissibleSimulationManifestFlags(
               identity.simulationContent.manifestFlags)
        && !allZero(identity.simulationContent.simulationRootSha256,
                    kCompatibilitySha256Bytes)
        && !allZero(identity.simulationContent.validationRootSha256,
                    kCompatibilitySha256Bytes)
        && identity.simulationContent.entryCount != 0u
        && identity.simulationContent.requiredEntryCount != 0u
        && identity.simulationContent.requiredEntryCount
               <= identity.simulationContent.entryCount
        && identity.stateSchemaId != 0u
        && identity.modSimulationId != 0u
        && identity.timingProfileId != 0u
        && identity.reserved == 0u;
}

inline bool isValidCompatibilityIdentity(
    const CompatibilityIdentity& identity) {
    return identity.schemaVersion == kCompatibilityIdentitySchemaVersion
        && isStructurallyValidCompatibilityIdentity(identity);
}

inline IdentityMismatch compatibilityMismatch(
    const CompatibilityIdentity& local,
    const CompatibilityIdentity& peer) {
    if (!isStructurallyValidCompatibilityIdentity(local))
        return IdentityMismatch::LocalIdentityInvalid;
    if (!isStructurallyValidCompatibilityIdentity(peer))
        return IdentityMismatch::PeerIdentityInvalid;
    if (local.schemaVersion != peer.schemaVersion)
        return IdentityMismatch::IdentitySchemaVersion;
    if (local.protocolVersion != peer.protocolVersion)
        return IdentityMismatch::ProtocolVersion;
    if (!equalBytes(
            reinterpret_cast<const uint8_t*>(local.runtimeBuild.profileId),
            reinterpret_cast<const uint8_t*>(peer.runtimeBuild.profileId),
            kCompatibilityProfileIdBytes)) {
        return IdentityMismatch::ProfileId;
    }
    if (!equalBytes(local.runtimeBuild.executableSha256,
                    peer.runtimeBuild.executableSha256,
                    kCompatibilitySha256Bytes)) {
        return IdentityMismatch::ExecutableSha256;
    }
    if (local.runtimeBuild.executableSize
        != peer.runtimeBuild.executableSize) {
        return IdentityMismatch::ExecutableSize;
    }
    if (local.simulationContent.manifestSchemaVersion
        != peer.simulationContent.manifestSchemaVersion) {
        return IdentityMismatch::ManifestSchemaVersion;
    }
    if (!equalBytes(local.simulationContent.simulationRootSha256,
                    peer.simulationContent.simulationRootSha256,
                    kCompatibilitySha256Bytes)) {
        return IdentityMismatch::SimulationRoot;
    }
    if (!equalBytes(local.simulationContent.validationRootSha256,
                    peer.simulationContent.validationRootSha256,
                    kCompatibilitySha256Bytes)) {
        return IdentityMismatch::ValidationRoot;
    }
    if (local.simulationContent.entryCount
        != peer.simulationContent.entryCount) {
        return IdentityMismatch::EntryCount;
    }
    if (local.simulationContent.requiredEntryCount
        != peer.simulationContent.requiredEntryCount) {
        return IdentityMismatch::RequiredEntryCount;
    }
    if (local.simulationContent.manifestFlags
        != peer.simulationContent.manifestFlags) {
        return IdentityMismatch::ManifestFlags;
    }
    if (local.stateSchemaId != peer.stateSchemaId)
        return IdentityMismatch::StateSchema;
    if (local.modSimulationId != peer.modSimulationId)
        return IdentityMismatch::ModSimulation;
    if (local.timingProfileId != peer.timingProfileId)
        return IdentityMismatch::TimingProfile;
    return IdentityMismatch::None;
}

inline bool semanticallyEqual(const CompatibilityIdentity& local,
                              const CompatibilityIdentity& peer) {
    return compatibilityMismatch(local, peer) == IdentityMismatch::None;
}

inline const char* identityMismatchReason(const IdentityMismatch mismatch) {
    switch (mismatch) {
        case IdentityMismatch::None: return "compatible";
        case IdentityMismatch::LocalIdentityInvalid:
            return "local compatibility identity is incomplete or invalid";
        case IdentityMismatch::PeerIdentityInvalid:
            return "peer compatibility identity is incomplete or invalid";
        case IdentityMismatch::IdentitySchemaVersion:
            return "compatibility identity schema mismatch";
        case IdentityMismatch::ProtocolVersion:
            return "compatibility protocol version mismatch";
        case IdentityMismatch::ProfileId: return "runtime profile mismatch";
        case IdentityMismatch::ExecutableSha256:
            return "executable SHA-256 mismatch";
        case IdentityMismatch::ExecutableSize:
            return "executable size mismatch";
        case IdentityMismatch::ManifestSchemaVersion:
            return "simulation-content manifest schema mismatch";
        case IdentityMismatch::SimulationRoot:
            return "simulation-content root mismatch";
        case IdentityMismatch::ValidationRoot:
            return "native-validation content root mismatch";
        case IdentityMismatch::EntryCount:
            return "simulation-content entry-count mismatch";
        case IdentityMismatch::RequiredEntryCount:
            return "required simulation-content entry-count mismatch";
        case IdentityMismatch::ManifestFlags:
            return "simulation-content manifest flags mismatch";
        case IdentityMismatch::StateSchema: return "state schema mismatch";
        case IdentityMismatch::ModSimulation:
            return "mod simulation identity mismatch";
        case IdentityMismatch::TimingProfile:
            return "timing profile mismatch";
    }
    return "unknown compatibility identity mismatch";
}

inline const char* identityMismatchPlayerText(
    const IdentityMismatch mismatch) {
    switch (mismatch) {
        case IdentityMismatch::None:
            return "everything matches";

        case IdentityMismatch::LocalIdentityInvalid:
            return "this copy of the game could not be identified. Check "
                   "that the game folder is complete and that the path in "
                   "Settings points at it";
        case IdentityMismatch::PeerIdentityInvalid:
            return "the other side's copy of the game could not be "
                   "identified. They should check that their game folder is "
                   "complete";

        case IdentityMismatch::IdentitySchemaVersion:
        case IdentityMismatch::ProtocolVersion:
            return "the two sides are running different versions of netplay. "
                   "Both need the same qoh99caster.exe and "
                   "qoh99_netplay.dll - whoever has the older one should "
                   "update";
        case IdentityMismatch::StateSchema:
            return "this netplay build tracks the game's state differently "
                   "than the other side's does. Both need the same netplay "
                   "version";
        case IdentityMismatch::ModSimulation:
            return "the two sides are running different gameplay "
                   "modifications. Both need the same set, or neither";
        case IdentityMismatch::TimingProfile:
            return "the two sides disagree about the game's frame timing, "
                   "which comes from the netplay build. Both need the same "
                   "netplay version";
        case IdentityMismatch::ManifestSchemaVersion:
            return "the two sides check their game files differently, which "
                   "means their netplay versions differ. Both need the same "
                   "netplay version";

        case IdentityMismatch::ProfileId:
            return "the two sides are running different editions of the game "
                   "(the November and December builds cannot play each "
                   "other). Both need the same edition";
        case IdentityMismatch::ExecutableSha256:
        case IdentityMismatch::ExecutableSize:
            return "the two sides are running different game executables. "
                   "Both need the same qoh99.exe - one of them has a "
                   "patched, repacked or different-revision copy";

        case IdentityMismatch::SimulationRoot:
        case IdentityMismatch::EntryCount:
        case IdentityMismatch::RequiredEntryCount:
        case IdentityMismatch::ManifestFlags:
            return "the two sides have different character data. Compare the "
                   "character folders - a .chr file that differs changes "
                   "hitboxes and frame data, so the two sides would not stay "
                   "in sync. Other files, including sound, do not matter";
        case IdentityMismatch::ValidationRoot:
            return "the two sides have different character data that the "
                   "game checks at startup. Compare the character folders";
    }
    return "the two sides could not agree on what they are running";
}

}
