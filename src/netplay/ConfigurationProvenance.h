#pragma once

#include <cstddef>
#include <cstdint>

namespace qnetplay::config_provenance {

constexpr uint16_t kConfigurationProvenanceSchemaVersion = 1u;

enum class Stage : uint8_t {
    Unknown = 0u,
    Default = 1u,
    ProvisionalRecommendation = 2u,
    LocallySelected = 3u,
    CasterAdvertised = 4u,
    SessionCommitted = 5u,
    IpcDelivered = 6u,
    RuntimeApplied = 7u,
    PeerAdvertised = 8u,
    PeerRuntimeAttested = 9u,
};

enum class TraceLane : uint8_t {
    Local,
    Peer,
};

inline const char* stageName(const Stage stage) {
    switch (stage) {
        case Stage::Default:                  return "default";
        case Stage::ProvisionalRecommendation:return "provisional-recommendation";
        case Stage::LocallySelected:          return "locally-selected";
        case Stage::CasterAdvertised:         return "caster-advertised";
        case Stage::SessionCommitted:         return "session-committed";
        case Stage::IpcDelivered:             return "ipc-delivered";
        case Stage::RuntimeApplied:           return "runtime-applied";
        case Stage::PeerAdvertised:           return "peer-advertised";
        case Stage::PeerRuntimeAttested:      return "peer-runtime-attested";
        case Stage::Unknown:
        default:                              return "unknown";
    }
}

inline bool isValidTraceTransition(const TraceLane lane, const Stage from,
                                   const Stage to) {
    if (from == to) return false;
    switch (to) {
        case Stage::Default:
            return lane == TraceLane::Local && from == Stage::Unknown;
        case Stage::ProvisionalRecommendation:
            return lane == TraceLane::Local
                && (from == Stage::Unknown || from == Stage::Default);
        case Stage::LocallySelected:
            return lane == TraceLane::Local
                && (from == Stage::Unknown || from == Stage::Default
                    || from == Stage::ProvisionalRecommendation);
        case Stage::CasterAdvertised:
        case Stage::PeerAdvertised:
            return lane == TraceLane::Peer && from == Stage::Unknown;
        case Stage::SessionCommitted:
            return (lane == TraceLane::Local
                    && (from == Stage::Default
                        || from == Stage::LocallySelected))
                || (lane == TraceLane::Peer
                    && (from == Stage::CasterAdvertised
                        || from == Stage::PeerAdvertised));
        case Stage::IpcDelivered:
            return from == Stage::SessionCommitted;
        case Stage::RuntimeApplied:
            return lane == TraceLane::Local && from == Stage::IpcDelivered;
        case Stage::PeerRuntimeAttested:
            return lane == TraceLane::Peer && from == Stage::IpcDelivered;
        case Stage::Unknown:
        default:
            return false;
    }
}

class Trace {
public:
    explicit Trace(const TraceLane lane) : lane_(lane) {}

    bool advance(const Stage next) {
        if (!isValidTraceTransition(lane_, current_, next)) return false;
        current_ = next;
        return true;
    }

    Stage current() const { return current_; }

private:
    TraceLane lane_;
    Stage current_ = Stage::Unknown;
};

inline bool isLocalLaunchOrigin(const Stage stage) {
    return stage == Stage::Default || stage == Stage::LocallySelected;
}

inline bool isPeerLaunchOrigin(const Stage stage) {
    return stage == Stage::CasterAdvertised || stage == Stage::PeerAdvertised;
}

#pragma pack(push, 1)
struct ConfigurationProvenanceWire {
    uint16_t schemaVersion = kConfigurationProvenanceSchemaVersion;
    uint8_t localDelayOrigin = static_cast<uint8_t>(Stage::Unknown);
    uint8_t localRollbackOrigin = static_cast<uint8_t>(Stage::Unknown);
    uint8_t peerDelayOrigin = static_cast<uint8_t>(Stage::Unknown);
    uint8_t peerRollbackOrigin = static_cast<uint8_t>(Stage::Unknown);
    uint8_t peerRuntimeAttestation = static_cast<uint8_t>(Stage::Unknown);
    uint8_t reserved[3]{};
};
#pragma pack(pop)

static_assert(sizeof(ConfigurationProvenanceWire) == 10u,
              "Configuration provenance IPC payload must stay compact");

inline ConfigurationProvenanceWire makeCommittedWire(
    const Stage localDelayOrigin, const Stage localRollbackOrigin,
    const Stage peerDelayOrigin, const Stage peerRollbackOrigin) {
    ConfigurationProvenanceWire wire{};
    wire.localDelayOrigin = static_cast<uint8_t>(localDelayOrigin);
    wire.localRollbackOrigin = static_cast<uint8_t>(localRollbackOrigin);
    wire.peerDelayOrigin = static_cast<uint8_t>(peerDelayOrigin);
    wire.peerRollbackOrigin = static_cast<uint8_t>(peerRollbackOrigin);
    wire.peerRuntimeAttestation = static_cast<uint8_t>(Stage::Unknown);
    return wire;
}

inline const char* validateCommittedWire(
    const ConfigurationProvenanceWire& wire) {
    if (wire.schemaVersion != kConfigurationProvenanceSchemaVersion)
        return "configuration provenance schema version mismatch";
    if (wire.reserved[0] != 0u || wire.reserved[1] != 0u
        || wire.reserved[2] != 0u)
        return "configuration provenance reserved bytes are nonzero";
    if (!isLocalLaunchOrigin(static_cast<Stage>(wire.localDelayOrigin))
        || !isLocalLaunchOrigin(static_cast<Stage>(wire.localRollbackOrigin)))
        return "configuration provenance local origin is not committed";
    if (!isPeerLaunchOrigin(static_cast<Stage>(wire.peerDelayOrigin))
        || !isPeerLaunchOrigin(static_cast<Stage>(wire.peerRollbackOrigin)))
        return "configuration provenance peer origin is not an advertisement";
    if (static_cast<Stage>(wire.peerRuntimeAttestation) != Stage::Unknown)
        return "peer runtime attestation is unavailable for this IPC schema";
    return nullptr;
}

}
