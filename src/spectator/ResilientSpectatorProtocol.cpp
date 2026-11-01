#include "spectator/ResilientSpectatorProtocol.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "replay/ReplayIntegrity.h"

namespace qnetplay::spectator::resilient::wire {
namespace {

bool validNodeKind(const NodeKind kind) {
    return static_cast<uint8_t>(kind)
        <= static_cast<uint8_t>(NodeKind::TrafficRelay);
}

bool validRouteKind(const RouteKind kind) {
    return static_cast<uint8_t>(kind)
        <= static_cast<uint8_t>(RouteKind::RootFallback);
}

bool validControlMessageType(const ControlMessageType type) {
    return static_cast<uint8_t>(type)
        >= static_cast<uint8_t>(ControlMessageType::NodeAdvertisement)
        && static_cast<uint8_t>(type)
            <= static_cast<uint8_t>(
                ControlMessageType::ClientPlayerRootActivationRetirement);
}

bool validAuthorityStateKind(const AuthorityStateKind kind) {
    return kind == AuthorityStateKind::Liveness
        || kind == AuthorityStateKind::Revocation;
}

bool validAuthorityStateReason(const AuthorityStateReason reason) {
    return static_cast<uint8_t>(reason)
        <= static_cast<uint8_t>(AuthorityStateReason::ViewerDetached);
}

bool validRelayChildRetirementReason(
    const RelayChildRetirementReason reason) {
    return reason == RelayChildRetirementReason::AuthorityRevoked
        || reason == RelayChildRetirementReason::ChildDetached
        || reason == RelayChildRetirementReason::ChildBindingReplaced
        || reason == RelayChildRetirementReason::LeaseExpired
        || reason == RelayChildRetirementReason::ServiceStopping;
}

bool validRelayChildActivationReceiptRole(
    const RelayChildActivationReceiptRole role) {
    return role == RelayChildActivationReceiptRole::Relay
        || role == RelayChildActivationReceiptRole::Child;
}

bool validClientPlayerRootRole(const ClientPlayerRootRole role) {
    return role == ClientPlayerRootRole::RemotePlayerOne
        || role == ClientPlayerRootRole::RemotePlayerTwo;
}

bool validClientPlayerRootRetirementReason(
    const ClientPlayerRootRetirementReason reason) {
    return reason == ClientPlayerRootRetirementReason::AuthorityRevoked
        || reason == ClientPlayerRootRetirementReason::ClientRootExpired
        || reason == ClientPlayerRootRetirementReason::ClientRootBindingReplaced
        || reason == ClientPlayerRootRetirementReason::TargetViewerDetached
        || reason == ClientPlayerRootRetirementReason::LeaseExpired
        || reason == ClientPlayerRootRetirementReason::ServiceStopping;
}

bool validBoolByte(const uint8_t value, bool& out) {
    if (value > 1u) return false;
    out = value != 0u;
    return true;
}

bool checkedU32(const size_t value, uint32_t& out) {
    if (value > (std::numeric_limits<uint32_t>::max)()) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

class Writer {
public:
    bool u8(const uint8_t value) { return byte(value); }
    bool u16(const uint16_t value) {
        return byte(static_cast<uint8_t>(value))
            && byte(static_cast<uint8_t>(value >> 8u));
    }
    bool u32(const uint32_t value) {
        for (unsigned shift = 0u; shift < 32u; shift += 8u)
            if (!byte(static_cast<uint8_t>(value >> shift))) return false;
        return true;
    }
    bool u64(const uint64_t value) {
        for (unsigned shift = 0u; shift < 64u; shift += 8u)
            if (!byte(static_cast<uint8_t>(value >> shift))) return false;
        return true;
    }
    bool bytes(const uint8_t* data, const size_t count) {
        if ((data == nullptr && count != 0u)
            || _bytes.size() + count > kMaxResilientControlDatagramBytes) {
            return false;
        }
        if (count == 0u) return true;
        try {
            _bytes.insert(_bytes.end(), data, data + count);
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    }
    bool reserve(const size_t count) {
        if (count > kMaxResilientControlDatagramBytes) return false;
        try {
            _bytes.reserve(count);
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    }
    const std::vector<uint8_t>& bytes() const { return _bytes; }
    std::vector<uint8_t>& bytes() { return _bytes; }

private:
    bool byte(const uint8_t value) {
        if (_bytes.size() >= kMaxResilientControlDatagramBytes) return false;
        try {
            _bytes.push_back(value);
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    }

    std::vector<uint8_t> _bytes;
};

class Reader {
public:
    Reader(const uint8_t* bytes, const size_t length)
        : _bytes(bytes), _length(length) {}

    bool u8(uint8_t& out) {
        if (_offset >= _length) return false;
        out = _bytes[_offset++];
        return true;
    }
    bool u16(uint16_t& out) {
        uint8_t low = 0, high = 0;
        if (!u8(low) || !u8(high)) return false;
        out = static_cast<uint16_t>(low)
            | (static_cast<uint16_t>(high) << 8u);
        return true;
    }
    bool u32(uint32_t& out) {
        uint32_t value = 0;
        for (unsigned shift = 0u; shift < 32u; shift += 8u) {
            uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<uint32_t>(byte) << shift;
        }
        out = value;
        return true;
    }
    bool u64(uint64_t& out) {
        uint64_t value = 0;
        for (unsigned shift = 0u; shift < 64u; shift += 8u) {
            uint8_t byte = 0;
            if (!u8(byte)) return false;
            value |= static_cast<uint64_t>(byte) << shift;
        }
        out = value;
        return true;
    }
    bool bytes(uint8_t* out, const size_t count) {
        if ((out == nullptr && count != 0u) || count > remaining()) return false;
        std::copy_n(_bytes + _offset, count, out);
        _offset += count;
        return true;
    }
    size_t remaining() const { return _length - _offset; }

private:
    const uint8_t* _bytes = nullptr;
    size_t _length = 0;
    size_t _offset = 0;
};

void writeLe16(uint8_t* out, const uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8u);
}

void writeLe32(uint8_t* out, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        out[shift / 8u] = static_cast<uint8_t>(value >> shift);
}

uint16_t readLe16(const uint8_t* in) {
    return static_cast<uint16_t>(in[0])
        | (static_cast<uint16_t>(in[1]) << 8u);
}

uint32_t readLe32(const uint8_t* in) {
    uint32_t value = 0;
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        value |= static_cast<uint32_t>(in[shift / 8u]) << shift;
    return value;
}

bool writeCompatibilityIdentity(
    Writer& writer, const qnetplay::compat::CompatibilityIdentityWire& identity) {
    using namespace qnetplay::compat;
    return writer.u32(identity.schemaVersion)
        && writer.u32(identity.protocolVersion)
        && writer.bytes(reinterpret_cast<const uint8_t*>(
                            identity.runtimeBuild.profileId),
                        kCompatibilityProfileIdBytes)
        && writer.bytes(identity.runtimeBuild.executableSha256,
                        kCompatibilitySha256Bytes)
        && writer.u64(identity.runtimeBuild.executableSize)
        && writer.u32(identity.runtimeBuild.peTimestamp)
        && writer.u32(identity.simulationContent.manifestSchemaVersion)
        && writer.u32(identity.simulationContent.manifestFlags)
        && writer.bytes(identity.simulationContent.simulationRootSha256,
                        kCompatibilitySha256Bytes)
        && writer.bytes(identity.simulationContent.validationRootSha256,
                        kCompatibilitySha256Bytes)
        && writer.u32(identity.simulationContent.entryCount)
        && writer.u32(identity.simulationContent.requiredEntryCount)
        && writer.u64(identity.stateSchemaId)
        && writer.u64(identity.modSimulationId)
        && writer.u64(identity.timingProfileId)
        && writer.u32(identity.reserved);
}

bool readCompatibilityIdentity(
    Reader& reader, qnetplay::compat::CompatibilityIdentityWire& out) {
    using namespace qnetplay::compat;
    CompatibilityIdentityWire decoded{};
    if (!reader.u32(decoded.schemaVersion)
        || !reader.u32(decoded.protocolVersion)
        || !reader.bytes(reinterpret_cast<uint8_t*>(
                             decoded.runtimeBuild.profileId),
                         kCompatibilityProfileIdBytes)
        || !reader.bytes(decoded.runtimeBuild.executableSha256,
                         kCompatibilitySha256Bytes)
        || !reader.u64(decoded.runtimeBuild.executableSize)
        || !reader.u32(decoded.runtimeBuild.peTimestamp)
        || !reader.u32(decoded.simulationContent.manifestSchemaVersion)
        || !reader.u32(decoded.simulationContent.manifestFlags)
        || !reader.bytes(decoded.simulationContent.simulationRootSha256,
                         kCompatibilitySha256Bytes)
        || !reader.bytes(decoded.simulationContent.validationRootSha256,
                         kCompatibilitySha256Bytes)
        || !reader.u32(decoded.simulationContent.entryCount)
        || !reader.u32(decoded.simulationContent.requiredEntryCount)
        || !reader.u64(decoded.stateSchemaId)
        || !reader.u64(decoded.modSimulationId)
        || !reader.u64(decoded.timingProfileId)
        || !reader.u32(decoded.reserved)
        || !isValidCompatibilityIdentity(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool writeConfigurationProvenance(
    Writer& writer,
    const qnetplay::config_provenance::ConfigurationProvenanceWire&
        provenance) {
    using qnetplay::config_provenance::validateCommittedWire;
    return validateCommittedWire(provenance) == nullptr
        && writer.u16(provenance.schemaVersion)
        && writer.u8(provenance.localDelayOrigin)
        && writer.u8(provenance.localRollbackOrigin)
        && writer.u8(provenance.peerDelayOrigin)
        && writer.u8(provenance.peerRollbackOrigin)
        && writer.u8(provenance.peerRuntimeAttestation)
        && writer.bytes(provenance.reserved, sizeof(provenance.reserved));
}

bool readConfigurationProvenance(
    Reader& reader,
    qnetplay::config_provenance::ConfigurationProvenanceWire& out) {
    qnetplay::config_provenance::ConfigurationProvenanceWire decoded{};
    if (!reader.u16(decoded.schemaVersion)
        || !reader.u8(decoded.localDelayOrigin)
        || !reader.u8(decoded.localRollbackOrigin)
        || !reader.u8(decoded.peerDelayOrigin)
        || !reader.u8(decoded.peerRollbackOrigin)
        || !reader.u8(decoded.peerRuntimeAttestation)
        || !reader.bytes(decoded.reserved, sizeof(decoded.reserved))
        || qnetplay::config_provenance::validateCommittedWire(decoded)
            != nullptr) {
        return false;
    }
    out = decoded;
    return true;
}

bool writeStream(Writer& writer, const StreamCompatibilityIdentity& stream) {
    return stream.valid()
        && writer.u64(stream.setOrSessionId)
        && writer.u64(stream.gameEpoch)
        && writeCompatibilityIdentity(writer, stream.compatibility)
        && writer.u64(stream.streamRootIdentity);
}

bool readStream(Reader& reader, StreamCompatibilityIdentity& out) {
    StreamCompatibilityIdentity decoded{};
    if (!reader.u64(decoded.setOrSessionId)
        || !reader.u64(decoded.gameEpoch)
        || !readCompatibilityIdentity(reader, decoded.compatibility)
        || !reader.u64(decoded.streamRootIdentity)
        || !decoded.valid()) {
        return false;
    }
    out = decoded;
    return true;
}

bool isCanonicalUnusedRoute(const RouteDescriptor& route) {
    return route.kind == RouteKind::Direct
        && route.ipv4BE == 0u && route.port == 0u
        && route.generation == 0u && !route.routeProved;
}

bool writeRoute(Writer& writer, const RouteDescriptor& route) {
    return validRouteKind(route.kind)
        && writer.u8(static_cast<uint8_t>(route.kind))
        && writer.u32(route.ipv4BE)
        && writer.u16(route.port)
        && writer.u64(route.generation)
        && writer.u8(route.routeProved ? 1u : 0u);
}

bool readRoute(Reader& reader, RouteDescriptor& out) {
    uint8_t rawKind = 0, rawProved = 0;
    RouteDescriptor decoded{};
    if (!reader.u8(rawKind)
        || rawKind > static_cast<uint8_t>(RouteKind::RootFallback)
        || !reader.u32(decoded.ipv4BE)
        || !reader.u16(decoded.port)
        || !reader.u64(decoded.generation)
        || !reader.u8(rawProved)
        || !validBoolByte(rawProved, decoded.routeProved)) {
        return false;
    }
    decoded.kind = static_cast<RouteKind>(rawKind);
    out = decoded;
    return true;
}

bool writeParentLeaseChildBinding(Writer& writer,
                                  const ParentLeaseChildBinding& binding) {
    return binding.valid()
        && writer.u64(binding.authorityGeneration)
        && writer.u64(binding.childViewerId)
        && writer.u32(binding.childViewerGeneration)
        && writeRoute(writer, binding.childQspRoute)
        && writer.u64(binding.childEndpointBindingGeneration)
        && writer.u32(binding.childRequestedCursor.nextRecord)
        && writer.u32(binding.childRequestedCursor.nextEvent)
        && writer.u64(binding.childRequestedCursor.digest);
}

bool readParentLeaseChildBinding(Reader& reader,
                                 ParentLeaseChildBinding& out) {
    ParentLeaseChildBinding decoded{};
    if (!reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.childViewerId)
        || !reader.u32(decoded.childViewerGeneration)
        || !readRoute(reader, decoded.childQspRoute)
        || !reader.u64(decoded.childEndpointBindingGeneration)
        || !reader.u32(decoded.childRequestedCursor.nextRecord)
        || !reader.u32(decoded.childRequestedCursor.nextEvent)
        || !reader.u64(decoded.childRequestedCursor.digest)
        || !decoded.valid()) {
        return false;
    }
    out = decoded;
    return true;
}

bool validAdvertisedTopology(const NodeAdvertisement& advertisement) {
    if (advertisement.ancestryCount > kMaxTopologyAncestry) return false;
    if (advertisement.activeParent == kInvalidNodeId) {
        if (advertisement.depth != 0u || advertisement.ancestryCount != 0u)
            return false;
    } else {
        if (advertisement.activeParent == advertisement.nodeId
            || advertisement.depth == 0u
            || advertisement.depth != advertisement.ancestryCount) {
            return false;
        }
    }
    for (size_t index = 0u; index < advertisement.ancestry.size(); ++index) {
        const NodeId ancestor = advertisement.ancestry[index];
        if (index >= advertisement.ancestryCount) {
            if (ancestor != kInvalidNodeId) return false;
            continue;
        }
        if (ancestor == kInvalidNodeId || ancestor == advertisement.nodeId)
            return false;
        for (size_t prior = 0u; prior < index; ++prior)
            if (advertisement.ancestry[prior] == ancestor) return false;
    }
    if (advertisement.activeParent != kInvalidNodeId
        && advertisement.ancestry[advertisement.ancestryCount - 1u]
            != advertisement.activeParent) {
        return false;
    }
    return (advertisement.kind != NodeKind::HostRoot
            && advertisement.kind != NodeKind::ClientRoot)
        || advertisement.activeParent == kInvalidNodeId;
}

bool validNodeAdvertisement(const NodeAdvertisement& advertisement) {
    uint32_t ignored = 0;
    const bool hasDedicatedControlRoute = advertisement.controlRoute.valid();
    return advertisement.nodeId != kInvalidNodeId
        && validNodeKind(advertisement.kind)
        && advertisement.stream.valid()
        && advertisement.route.valid()
        && validRouteKind(advertisement.route.kind)
        && (hasDedicatedControlRoute
                ? validRouteKind(advertisement.controlRoute.kind)
                    && advertisement.controlRoute.generation
                        == advertisement.routeGeneration
                : isCanonicalUnusedRoute(advertisement.controlRoute))
        && validAdvertisedTopology(advertisement)
        && advertisement.capabilityModelRevision
            == kResilientCapabilityModelRevision
        && checkedU32(advertisement.childCapacity, ignored)
        && checkedU32(advertisement.childCount, ignored)
        && advertisement.childCount <= advertisement.childCapacity
        && advertisement.uploadInUse <= advertisement.uploadCapacity
        && advertisement.sourceGeneration != 0u
        && advertisement.routeGeneration != 0u
        && advertisement.route.generation == advertisement.routeGeneration
        && advertisement.cacheFirstCursor <= advertisement.cacheNextCursor;
}

bool writeNodeAdvertisement(Writer& writer,
                            const NodeAdvertisement& advertisement) {
    uint32_t childCapacity = 0, childCount = 0;
    if (!validNodeAdvertisement(advertisement)
        || !checkedU32(advertisement.childCapacity, childCapacity)
        || !checkedU32(advertisement.childCount, childCount)) {
        return false;
    }
    if (!writer.u64(advertisement.nodeId)
        || !writer.u8(static_cast<uint8_t>(advertisement.kind))
        || !writeStream(writer, advertisement.stream)
        || !writeRoute(writer, advertisement.route)
        || !writeRoute(writer, advertisement.controlRoute)
        || !writer.u64(advertisement.activeParent)
        || !writer.u8(advertisement.depth)
        || !writer.u8(advertisement.ancestryCount)) {
        return false;
    }
    for (const NodeId ancestor : advertisement.ancestry)
        if (!writer.u64(ancestor)) return false;
    return writer.u16(advertisement.capabilityModelRevision)
        && writer.u32(advertisement.capabilities)
        && writer.u32(childCapacity)
        && writer.u32(childCount)
        && writer.u32(advertisement.uploadCapacity)
        && writer.u32(advertisement.uploadInUse)
        && writer.u32(advertisement.rttMs)
        && writer.u32(advertisement.lossPerMille)
        && writer.u32(advertisement.cacheFirstCursor)
        && writer.u32(advertisement.cacheNextCursor)
        && writer.u64(advertisement.sourceGeneration)
        && writer.u64(advertisement.routeGeneration)
        && writer.u64(advertisement.lastProgressMs)
        && writer.u64(advertisement.leaseExpiryMs)
        && writer.u8(advertisement.reachable ? 1u : 0u)
        && writer.u8(advertisement.draining ? 1u : 0u);
}

bool readNodeAdvertisement(Reader& reader, NodeAdvertisement& out) {
    uint8_t rawKind = 0, rawReachable = 0, rawDraining = 0;
    uint32_t childCapacity = 0, childCount = 0;
    NodeAdvertisement decoded{};
    if (!reader.u64(decoded.nodeId)
        || !reader.u8(rawKind)
        || rawKind > static_cast<uint8_t>(NodeKind::TrafficRelay)
        || !readStream(reader, decoded.stream)
        || !readRoute(reader, decoded.route)
        || !readRoute(reader, decoded.controlRoute)
        || !reader.u64(decoded.activeParent)
        || !reader.u8(decoded.depth)
        || !reader.u8(decoded.ancestryCount)) {
        return false;
    }
    for (NodeId& ancestor : decoded.ancestry)
        if (!reader.u64(ancestor)) return false;
    if (!reader.u16(decoded.capabilityModelRevision)
        || !reader.u32(decoded.capabilities)
        || !reader.u32(childCapacity)
        || !reader.u32(childCount)
        || !reader.u32(decoded.uploadCapacity)
        || !reader.u32(decoded.uploadInUse)
        || !reader.u32(decoded.rttMs)
        || !reader.u32(decoded.lossPerMille)
        || !reader.u32(decoded.cacheFirstCursor)
        || !reader.u32(decoded.cacheNextCursor)
        || !reader.u64(decoded.sourceGeneration)
        || !reader.u64(decoded.routeGeneration)
        || !reader.u64(decoded.lastProgressMs)
        || !reader.u64(decoded.leaseExpiryMs)
        || !reader.u8(rawReachable)
        || !reader.u8(rawDraining)
        || !validBoolByte(rawReachable, decoded.reachable)
        || !validBoolByte(rawDraining, decoded.draining)) {
        return false;
    }
    decoded.kind = static_cast<NodeKind>(rawKind);
    decoded.childCapacity = childCapacity;
    decoded.childCount = childCount;
    if (!validNodeAdvertisement(decoded)) return false;
    out = decoded;
    return true;
}

bool validParentLease(const ParentLease& lease) {
    if (!lease.stream.valid() || lease.childId == kInvalidNodeId
        || lease.parentId == kInvalidNodeId || lease.childId == lease.parentId
        || !lease.childBinding.valid()
        || lease.parentSourceGeneration == 0u || lease.routeGeneration == 0u
        || lease.routeCount == 0u || lease.routeCount > kMaxLeaseRoutes
        || lease.cacheRange.firstCursor > lease.cacheRange.nextCursor
        || lease.latestVerifiedCursor != lease.cacheRange.nextCursor
        || lease.childCapacity == 0u || lease.expiryMs == 0u
        || lease.replayNonce == 0u || lease.ancestryCount > kMaxLeaseAncestry) {
        return false;
    }
    for (size_t index = 0; index < lease.routes.size(); ++index) {
        const RouteDescriptor& route = lease.routes[index];
        if (index < lease.routeCount) {
            if (!validRouteKind(route.kind) || !route.valid()
                || route.generation != lease.routeGeneration) {
                return false;
            }
        } else if (!isCanonicalUnusedRoute(route)) {
            return false;
        }
    }
    for (size_t index = 0; index < lease.ancestry.size(); ++index) {
        const NodeId ancestor = lease.ancestry[index];
        if (index >= lease.ancestryCount) {
            if (ancestor != kInvalidNodeId) return false;
            continue;
        }
        if (ancestor == kInvalidNodeId || ancestor == lease.childId) return false;
        for (size_t prior = 0; prior < index; ++prior)
            if (lease.ancestry[prior] == ancestor) return false;
    }
    return true;
}

bool writeParentLease(Writer& writer, const ParentLease& lease) {
    if (!validParentLease(lease)) return false;
    if (!writeStream(writer, lease.stream)
        || !writer.u64(lease.childId)
        || !writer.u64(lease.parentId)
        || !writeParentLeaseChildBinding(writer, lease.childBinding)
        || !writer.u8(lease.parentDepth)
        || !writer.u64(lease.parentSourceGeneration)
        || !writer.u64(lease.routeGeneration)
        || !writer.u8(lease.routeCount)) {
        return false;
    }
    for (const RouteDescriptor& route : lease.routes)
        if (!writeRoute(writer, route)) return false;
    if (!writer.u32(lease.cacheRange.firstCursor)
        || !writer.u32(lease.cacheRange.nextCursor)
        || !writer.u32(lease.latestVerifiedCursor)
        || !writer.u32(lease.childCapacity)
        || !writer.u64(lease.expiryMs)
        || !writer.u64(lease.replayNonce)
        || !writer.u8(lease.ancestryCount)) {
        return false;
    }
    for (const NodeId ancestor : lease.ancestry)
        if (!writer.u64(ancestor)) return false;
    return writer.u64(lease.macLow) && writer.u64(lease.macHigh);
}

bool readParentLease(Reader& reader, ParentLease& out) {
    ParentLease decoded{};
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.childId)
        || !reader.u64(decoded.parentId)
        || !readParentLeaseChildBinding(reader, decoded.childBinding)
        || !reader.u8(decoded.parentDepth)
        || !reader.u64(decoded.parentSourceGeneration)
        || !reader.u64(decoded.routeGeneration)
        || !reader.u8(decoded.routeCount)) {
        return false;
    }
    for (RouteDescriptor& route : decoded.routes)
        if (!readRoute(reader, route)) return false;
    if (!reader.u32(decoded.cacheRange.firstCursor)
        || !reader.u32(decoded.cacheRange.nextCursor)
        || !reader.u32(decoded.latestVerifiedCursor)
        || !reader.u32(decoded.childCapacity)
        || !reader.u64(decoded.expiryMs)
        || !reader.u64(decoded.replayNonce)
        || !reader.u8(decoded.ancestryCount)) {
        return false;
    }
    for (NodeId& ancestor : decoded.ancestry)
        if (!reader.u64(ancestor)) return false;
    if (!reader.u64(decoded.macLow) || !reader.u64(decoded.macHigh)
        || !validParentLease(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validLeaseBinding(const LeaseBinding& binding) {
    return binding.childId != kInvalidNodeId
        && binding.parentId != kInvalidNodeId
        && binding.childId != binding.parentId
        && binding.childBinding.valid()
        && binding.parentSourceGeneration != 0u
        && binding.routeGeneration != 0u
        && binding.expiryMs != 0u
        && binding.replayNonce != 0u;
}

bool writeLeaseBinding(Writer& writer, const LeaseBinding& binding) {
    return validLeaseBinding(binding)
        && writer.u64(binding.childId)
        && writer.u64(binding.parentId)
        && writer.u64(binding.parentSourceGeneration)
        && writer.u64(binding.routeGeneration)
        && writer.u64(binding.expiryMs)
        && writer.u64(binding.replayNonce)
        && writer.u64(binding.macLow)
        && writer.u64(binding.macHigh)
        && writeParentLeaseChildBinding(writer, binding.childBinding);
}

bool readLeaseBinding(Reader& reader, LeaseBinding& out) {
    LeaseBinding decoded{};
    if (!reader.u64(decoded.childId)
        || !reader.u64(decoded.parentId)
        || !reader.u64(decoded.parentSourceGeneration)
        || !reader.u64(decoded.routeGeneration)
        || !reader.u64(decoded.expiryMs)
        || !reader.u64(decoded.replayNonce)
        || !reader.u64(decoded.macLow)
        || !reader.u64(decoded.macHigh)
        || !readParentLeaseChildBinding(reader, decoded.childBinding)
        || !validLeaseBinding(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

static bool validCheckpointIdentity(const uint32_t checkpointCursor,
                                    const uint32_t checkpointEvent,
                                    const uint64_t checkpointDigest,
                                    const uint32_t boundaryCursor,
                                    const uint32_t boundaryEvent) {
    if (checkpointDigest == 0u)
        return checkpointCursor == 0u && checkpointEvent == 0u;
    return checkpointCursor <= boundaryCursor
        && checkpointEvent <= boundaryEvent;
}

bool validContinuityRequest(const ContinuityRequest& request) {
    return request.stream.valid() && validLeaseBinding(request.lease)
        && request.gameId != 0u
        && request.expectedSourceGeneration != 0u
        && request.expectedSourceGeneration
            == request.lease.parentSourceGeneration
        && request.parentGeneration != 0u
        && request.overlapCursor <= request.exactNextCursor
        && request.overlapEvent <= request.exactNextEvent
        && validCheckpointIdentity(request.checkpointCursor,
                                   request.checkpointEvent,
                                   request.checkpointDigest,
                                   request.overlapCursor,
                                   request.overlapEvent);
}

bool writeContinuityRequest(Writer& writer, const ContinuityRequest& request) {
    return validContinuityRequest(request)
        && writeStream(writer, request.stream)
        && writeLeaseBinding(writer, request.lease)
        && writer.u64(request.gameId)
        && writer.u64(request.expectedSourceGeneration)
        && writer.u64(request.parentGeneration)
        && writer.u32(request.exactNextCursor)
        && writer.u32(request.exactNextEvent)
        && writer.u32(request.overlapCursor)
        && writer.u32(request.overlapEvent)
        && writer.u64(request.overlapDigest)
        && writer.u32(request.checkpointCursor)
        && writer.u32(request.checkpointEvent)
        && writer.u64(request.checkpointDigest);
}

bool readContinuityRequest(Reader& reader, ContinuityRequest& out) {
    ContinuityRequest decoded{};
    if (!readStream(reader, decoded.stream)
        || !readLeaseBinding(reader, decoded.lease)
        || !reader.u64(decoded.gameId)
        || !reader.u64(decoded.expectedSourceGeneration)
        || !reader.u64(decoded.parentGeneration)
        || !reader.u32(decoded.exactNextCursor)
        || !reader.u32(decoded.exactNextEvent)
        || !reader.u32(decoded.overlapCursor)
        || !reader.u32(decoded.overlapEvent)
        || !reader.u64(decoded.overlapDigest)
        || !reader.u32(decoded.checkpointCursor)
        || !reader.u32(decoded.checkpointEvent)
        || !reader.u64(decoded.checkpointDigest)
        || !validContinuityRequest(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validContinuityProof(const ContinuityProof& proof) {
    return proof.stream.valid() && validLeaseBinding(proof.lease)
        && proof.gameId != 0u
        && proof.sourceGeneration != 0u
        && proof.sourceGeneration == proof.lease.parentSourceGeneration
        && proof.parentGeneration != 0u
        && proof.cacheFirstCursor <= proof.cacheNextCursor
        && proof.cacheFirstEvent <= proof.cacheNextEvent
        && proof.overlapCursor <= proof.cacheNextCursor
        && proof.overlapEvent <= proof.cacheNextEvent
        && (!proof.available || (proof.overlapCursor >= proof.cacheFirstCursor
                                 && proof.overlapEvent
                                     >= proof.cacheFirstEvent))
        && validCheckpointIdentity(proof.checkpointCursor,
                                   proof.checkpointEvent,
                                   proof.checkpointDigest,
                                   proof.cacheNextCursor,
                                   proof.cacheNextEvent)
        && (proof.available || proof.checkpointDigest == 0u);
}

bool writeContinuityProof(Writer& writer, const ContinuityProof& proof) {
    return validContinuityProof(proof)
        && writeStream(writer, proof.stream)
        && writeLeaseBinding(writer, proof.lease)
        && writer.u64(proof.gameId)
        && writer.u64(proof.sourceGeneration)
        && writer.u64(proof.parentGeneration)
        && writer.u32(proof.cacheFirstCursor)
        && writer.u32(proof.cacheNextCursor)
        && writer.u32(proof.cacheFirstEvent)
        && writer.u32(proof.cacheNextEvent)
        && writer.u32(proof.overlapCursor)
        && writer.u32(proof.overlapEvent)
        && writer.u64(proof.overlapDigest)
        && writer.u32(proof.checkpointCursor)
        && writer.u32(proof.checkpointEvent)
        && writer.u64(proof.checkpointDigest)
        && writer.u8(proof.available ? 1u : 0u);
}

bool readContinuityProof(Reader& reader, ContinuityProof& out) {
    uint8_t rawAvailable = 0;
    ContinuityProof decoded{};
    if (!readStream(reader, decoded.stream)
        || !readLeaseBinding(reader, decoded.lease)
        || !reader.u64(decoded.gameId)
        || !reader.u64(decoded.sourceGeneration)
        || !reader.u64(decoded.parentGeneration)
        || !reader.u32(decoded.cacheFirstCursor)
        || !reader.u32(decoded.cacheNextCursor)
        || !reader.u32(decoded.cacheFirstEvent)
        || !reader.u32(decoded.cacheNextEvent)
        || !reader.u32(decoded.overlapCursor)
        || !reader.u32(decoded.overlapEvent)
        || !reader.u64(decoded.overlapDigest)
        || !reader.u32(decoded.checkpointCursor)
        || !reader.u32(decoded.checkpointEvent)
        || !reader.u64(decoded.checkpointDigest)
        || !reader.u8(rawAvailable)
        || !validBoolByte(rawAvailable, decoded.available)
        || !validContinuityProof(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validParentCommit(const ParentCommit& commit) {
    return commit.stream.valid() && commit.childId != kInvalidNodeId
        && commit.parentId != kInvalidNodeId && commit.childId != commit.parentId
        && validLeaseBinding(commit.lease)
        && commit.childId == commit.lease.childId
        && commit.parentId == commit.lease.parentId
        && commit.sourceGeneration != 0u && commit.routeGeneration != 0u
        && commit.sourceGeneration == commit.lease.parentSourceGeneration
        && commit.routeGeneration == commit.lease.routeGeneration
        && commit.overlapCursor <= commit.exactNextCursor
        && commit.overlapEvent <= commit.exactNextEvent;
}

bool writeParentCommit(Writer& writer, const ParentCommit& commit) {
    return validParentCommit(commit)
        && writeStream(writer, commit.stream)
        && writer.u64(commit.childId)
        && writer.u64(commit.parentId)
        && writeLeaseBinding(writer, commit.lease)
        && writer.u64(commit.sourceGeneration)
        && writer.u64(commit.routeGeneration)
        && writer.u32(commit.exactNextCursor)
        && writer.u32(commit.exactNextEvent)
        && writer.u32(commit.overlapCursor)
        && writer.u32(commit.overlapEvent)
        && writer.u64(commit.overlapDigest);
}

bool readParentCommit(Reader& reader, ParentCommit& out) {
    ParentCommit decoded{};
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.childId)
        || !reader.u64(decoded.parentId)
        || !readLeaseBinding(reader, decoded.lease)
        || !reader.u64(decoded.sourceGeneration)
        || !reader.u64(decoded.routeGeneration)
        || !reader.u32(decoded.exactNextCursor)
        || !reader.u32(decoded.exactNextEvent)
        || !reader.u32(decoded.overlapCursor)
        || !reader.u32(decoded.overlapEvent)
        || !reader.u64(decoded.overlapDigest)
        || !validParentCommit(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool containsOnlyUniqueNonzero(const std::vector<NodeId>& values,
                               const NodeId disallowed) {
    for (size_t index = 0; index < values.size(); ++index) {
        if (values[index] == kInvalidNodeId || values[index] == disallowed)
            return false;
        for (size_t prior = 0; prior < index; ++prior)
            if (values[prior] == values[index]) return false;
    }
    return true;
}

bool validBoundDrainNotice(const BoundDrainNotice& bound) {
    const DrainNotice& notice = bound.notice;
    return bound.stream.valid() && bound.nodeId != kInvalidNodeId
        && notice.shutdownDeadlineMs != 0u
        && notice.lastAvailableCursor <= notice.cacheEndCursor
        && notice.sourceGeneration != 0u && notice.routeGeneration != 0u
        && notice.recommendedReplacementParents.size()
            <= kMaxDrainNoticeNodeIds
        && notice.activeChildIds.size() <= kMaxDrainNoticeNodeIds
        && containsOnlyUniqueNonzero(notice.recommendedReplacementParents,
                                     bound.nodeId)
        && containsOnlyUniqueNonzero(notice.activeChildIds, bound.nodeId);
}

bool writeNodeList(Writer& writer, const std::vector<NodeId>& values) {
    uint16_t count = 0;
    if (values.size() > (std::numeric_limits<uint16_t>::max)()) return false;
    count = static_cast<uint16_t>(values.size());
    if (!writer.u16(count)) return false;
    for (const NodeId value : values)
        if (!writer.u64(value)) return false;
    return true;
}

bool readNodeList(Reader& reader, std::vector<NodeId>& out) {
    uint16_t count = 0;
    if (!reader.u16(count) || count > kMaxDrainNoticeNodeIds
        || reader.remaining() < static_cast<size_t>(count) * sizeof(NodeId)) {
        return false;
    }
    std::vector<NodeId> decoded;
    try {
        decoded.reserve(count);
        for (uint16_t index = 0; index < count; ++index) {
            NodeId value = kInvalidNodeId;
            if (!reader.u64(value)) return false;
            decoded.push_back(value);
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

bool writeBoundDrainNotice(Writer& writer, const BoundDrainNotice& bound) {
    const DrainNotice& notice = bound.notice;
    return validBoundDrainNotice(bound)
        && writeStream(writer, bound.stream)
        && writer.u64(bound.nodeId)
        && writer.u64(notice.shutdownDeadlineMs)
        && writer.u32(notice.lastAvailableCursor)
        && writer.u32(notice.cacheEndCursor)
        && writer.u64(notice.sourceGeneration)
        && writer.u64(notice.routeGeneration)
        && writeNodeList(writer, notice.recommendedReplacementParents)
        && writeNodeList(writer, notice.activeChildIds);
}

bool readBoundDrainNotice(Reader& reader, BoundDrainNotice& out) {
    BoundDrainNotice decoded{};
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.nodeId)
        || !reader.u64(decoded.notice.shutdownDeadlineMs)
        || !reader.u32(decoded.notice.lastAvailableCursor)
        || !reader.u32(decoded.notice.cacheEndCursor)
        || !reader.u64(decoded.notice.sourceGeneration)
        || !reader.u64(decoded.notice.routeGeneration)
        || !readNodeList(reader,
                         decoded.notice.recommendedReplacementParents)
        || !readNodeList(reader, decoded.notice.activeChildIds)
        || !validBoundDrainNotice(decoded)) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

bool validAuthorityState(const AuthorityState& state) {
    if (!state.stream.valid() || state.authorityGeneration == 0u
        || state.rootNodeId == kInvalidNodeId
        || state.childNodeId == kInvalidNodeId
        || state.rootNodeId == state.childNodeId || state.viewerId == 0u
        || state.viewerGeneration == 0u || state.qspRouteGeneration == 0u
        || state.qsrIpv4BE == 0u || state.qsrPort == 0u
        || state.qsrBindingGeneration == 0u || state.sequence == 0u
        || state.authorityExpiryMs == 0u || state.eventTimeMs == 0u
        || !validAuthorityStateKind(state.kind)
        || !validAuthorityStateReason(state.reason)) {
        return false;
    }
    if (state.kind == AuthorityStateKind::Liveness) {
        return state.reason == AuthorityStateReason::None
            && state.livenessWindowMs != 0u
            && state.livenessWindowMs
                <= kTopologyAuthorityStateMaxLivenessWindowMs;
    }
    return state.reason != AuthorityStateReason::None
        && state.livenessWindowMs == 0u;
}

bool writeAuthorityState(Writer& writer, const AuthorityState& state) {
    return validAuthorityState(state)
        && writeStream(writer, state.stream)
        && writer.u64(state.authorityGeneration)
        && writer.u64(state.rootNodeId)
        && writer.u64(state.childNodeId)
        && writer.u64(state.viewerId)
        && writer.u32(state.viewerGeneration)
        && writer.u64(state.qspRouteGeneration)
        && writer.u32(state.qsrIpv4BE)
        && writer.u16(state.qsrPort)
        && writer.u64(state.qsrBindingGeneration)
        && writer.u8(static_cast<uint8_t>(state.kind))
        && writer.u8(static_cast<uint8_t>(state.reason))
        && writer.u64(state.sequence)
        && writer.u64(state.authorityExpiryMs)
        && writer.u64(state.eventTimeMs)
        && writer.u32(state.livenessWindowMs);
}

bool readAuthorityState(Reader& reader, AuthorityState& out) {
    AuthorityState decoded{};
    uint8_t rawKind = 0u;
    uint8_t rawReason = 0u;
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.rootNodeId)
        || !reader.u64(decoded.childNodeId)
        || !reader.u64(decoded.viewerId)
        || !reader.u32(decoded.viewerGeneration)
        || !reader.u64(decoded.qspRouteGeneration)
        || !reader.u32(decoded.qsrIpv4BE)
        || !reader.u16(decoded.qsrPort)
        || !reader.u64(decoded.qsrBindingGeneration)
        || !reader.u8(rawKind) || !reader.u8(rawReason)
        || !reader.u64(decoded.sequence)
        || !reader.u64(decoded.authorityExpiryMs)
        || !reader.u64(decoded.eventTimeMs)
        || !reader.u32(decoded.livenessWindowMs)) {
        return false;
    }
    decoded.kind = static_cast<AuthorityStateKind>(rawKind);
    decoded.reason = static_cast<AuthorityStateReason>(rawReason);
    if (!validAuthorityState(decoded)) return false;
    out = decoded;
    return true;
}

bool sameRoute(const RouteDescriptor& left, const RouteDescriptor& right) {
    return left.kind == right.kind && left.ipv4BE == right.ipv4BE
        && left.port == right.port && left.generation == right.generation
        && left.routeProved == right.routeProved;
}

bool validRelayDataRoute(const RouteDescriptor& route) {
    return route.valid() && route.routeProved
        && route.kind == RouteKind::Direct;
}

bool validRelayDataBindingRequest(const RelayDataBindingRequest& request) {
    return request.stream.valid() && request.authorityGeneration != 0u
        && request.rootNodeId != kInvalidNodeId
        && request.childNodeId != kInvalidNodeId
        && request.rootNodeId != request.childNodeId
        && validLeaseBinding(request.lease)
        && request.lease.childId == request.childNodeId
        && request.lease.childBinding.authorityGeneration
            == request.authorityGeneration
        && request.requestNonce != 0u;
}

bool validRelayDataBindingAck(const RelayDataBindingAck& ack) {
    const RelayDataBindingRequest request{
        ack.stream, ack.authorityGeneration, ack.rootNodeId, ack.childNodeId,
        ack.lease, ack.requestNonce};
    return validRelayDataBindingRequest(request)
        && validRelayDataRoute(ack.observedDataRoute)
        && validRelayDataRoute(ack.observedControlRoute)
        && !ack.observedDataRoute.sameEndpoint(ack.observedControlRoute)
        && ack.observedDataRoute.generation
            == ack.observedControlRoute.generation;
}

bool writeRelayDataBindingRequest(Writer& writer,
                                  const RelayDataBindingRequest& request) {
    return validRelayDataBindingRequest(request)
        && writeStream(writer, request.stream)
        && writer.u64(request.authorityGeneration)
        && writer.u64(request.rootNodeId)
        && writer.u64(request.childNodeId)
        && writeLeaseBinding(writer, request.lease)
        && writer.u64(request.requestNonce);
}

bool readRelayDataBindingRequest(Reader& reader,
                                 RelayDataBindingRequest& out) {
    RelayDataBindingRequest decoded{};
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.rootNodeId)
        || !reader.u64(decoded.childNodeId)
        || !readLeaseBinding(reader, decoded.lease)
        || !reader.u64(decoded.requestNonce)
        || !validRelayDataBindingRequest(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool writeRelayDataBindingAck(Writer& writer,
                              const RelayDataBindingAck& ack) {
    return validRelayDataBindingAck(ack)
        && writeStream(writer, ack.stream)
        && writer.u64(ack.authorityGeneration)
        && writer.u64(ack.rootNodeId)
        && writer.u64(ack.childNodeId)
        && writeLeaseBinding(writer, ack.lease)
        && writer.u64(ack.requestNonce)
        && writeRoute(writer, ack.observedDataRoute)
        && writeRoute(writer, ack.observedControlRoute);
}

bool readRelayDataBindingAck(Reader& reader, RelayDataBindingAck& out) {
    RelayDataBindingAck decoded{};
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.rootNodeId)
        || !reader.u64(decoded.childNodeId)
        || !readLeaseBinding(reader, decoded.lease)
        || !reader.u64(decoded.requestNonce)
        || !readRoute(reader, decoded.observedDataRoute)
        || !readRoute(reader, decoded.observedControlRoute)
        || !validRelayDataBindingAck(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validRelayReady(const RelayReady& ready) {
    const RelayDataBindingRequest binding{
        ready.stream, ready.authorityGeneration, ready.rootNodeId,
        ready.relayNodeId, ready.relayLease, ready.bindingRequestNonce};
    return validRelayDataBindingRequest(binding)
        && validRelayDataRoute(ready.observedDataRoute)
        && validRelayDataRoute(ready.observedControlRoute)
        && !ready.observedDataRoute.sameEndpoint(ready.observedControlRoute)
        && ready.observedDataRoute.generation
            == ready.observedControlRoute.generation
        && ready.cacheRange.firstCursor <= ready.cacheRange.nextCursor
        && ready.cacheRange.nextCursor == ready.cacheCursor.nextRecord;
}

bool writeRelayReady(Writer& writer, const RelayReady& ready) {
    return validRelayReady(ready)
        && writeStream(writer, ready.stream)
        && writer.u64(ready.authorityGeneration)
        && writer.u64(ready.rootNodeId)
        && writer.u64(ready.relayNodeId)
        && writeLeaseBinding(writer, ready.relayLease)
        && writer.u64(ready.bindingRequestNonce)
        && writeRoute(writer, ready.observedDataRoute)
        && writeRoute(writer, ready.observedControlRoute)
        && writer.u32(ready.cacheRange.firstCursor)
        && writer.u32(ready.cacheRange.nextCursor)
        && writer.u32(ready.cacheCursor.nextRecord)
        && writer.u32(ready.cacheCursor.nextEvent)
        && writer.u64(ready.cacheCursor.digest);
}

bool readRelayReady(Reader& reader, RelayReady& out) {
    RelayReady decoded{};
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.rootNodeId)
        || !reader.u64(decoded.relayNodeId)
        || !readLeaseBinding(reader, decoded.relayLease)
        || !reader.u64(decoded.bindingRequestNonce)
        || !readRoute(reader, decoded.observedDataRoute)
        || !readRoute(reader, decoded.observedControlRoute)
        || !reader.u32(decoded.cacheRange.firstCursor)
        || !reader.u32(decoded.cacheRange.nextCursor)
        || !reader.u32(decoded.cacheCursor.nextRecord)
        || !reader.u32(decoded.cacheCursor.nextEvent)
        || !reader.u64(decoded.cacheCursor.digest)
        || !validRelayReady(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validClientDataRootAssertion(const ClientDataRootAssertion& assertion) {
    return assertion.stream.valid()
        && assertion.gameStartFingerprint != 0u
        && assertion.authorityGeneration != 0u
        && assertion.clientRootNodeId != kInvalidNodeId
        && assertion.qspRouteGeneration != 0u
        && assertion.qsrBindingGeneration != 0u
        && assertion.sourceGeneration != 0u
        && validRelayDataRoute(assertion.dataRoute)
        && validRelayDataRoute(assertion.qsrRoute)
        && !assertion.dataRoute.sameEndpoint(assertion.qsrRoute)
        && assertion.dataRoute.generation == assertion.qsrBindingGeneration
        && assertion.qsrRoute.generation == assertion.qsrBindingGeneration
        && assertion.cacheRange.firstCursor <= assertion.cacheRange.nextCursor
        && assertion.cacheRange.nextCursor == assertion.cursor.nextRecord
        && assertion.childCapacity != 0u;
}

bool writeClientDataRootAssertion(Writer& writer,
                                  const ClientDataRootAssertion& assertion) {
    return validClientDataRootAssertion(assertion)
        && writeStream(writer, assertion.stream)
        && writer.u64(assertion.gameStartFingerprint)
        && writer.u64(assertion.authorityGeneration)
        && writer.u64(assertion.clientRootNodeId)
        && writer.u64(assertion.qspRouteGeneration)
        && writer.u64(assertion.qsrBindingGeneration)
        && writer.u64(assertion.sourceGeneration)
        && writeRoute(writer, assertion.dataRoute)
        && writeRoute(writer, assertion.qsrRoute)
        && writer.u32(assertion.cacheRange.firstCursor)
        && writer.u32(assertion.cacheRange.nextCursor)
        && writer.u32(assertion.cursor.nextRecord)
        && writer.u32(assertion.cursor.nextEvent)
        && writer.u64(assertion.cursor.digest)
        && writer.u32(assertion.childCapacity);
}

bool readClientDataRootAssertion(Reader& reader,
                                 ClientDataRootAssertion& out) {
    ClientDataRootAssertion decoded{};
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.gameStartFingerprint)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.clientRootNodeId)
        || !reader.u64(decoded.qspRouteGeneration)
        || !reader.u64(decoded.qsrBindingGeneration)
        || !reader.u64(decoded.sourceGeneration)
        || !readRoute(reader, decoded.dataRoute)
        || !readRoute(reader, decoded.qsrRoute)
        || !reader.u32(decoded.cacheRange.firstCursor)
        || !reader.u32(decoded.cacheRange.nextCursor)
        || !reader.u32(decoded.cursor.nextRecord)
        || !reader.u32(decoded.cursor.nextEvent)
        || !reader.u64(decoded.cursor.digest)
        || !reader.u32(decoded.childCapacity)
        || !validClientDataRootAssertion(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool sameConfigurationProvenance(
    const qnetplay::config_provenance::ConfigurationProvenanceWire& left,
    const qnetplay::config_provenance::ConfigurationProvenanceWire& right) {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

bool validClientPlayerRootRoute(const RouteDescriptor& route) {
    return route.valid() && route.routeProved
        && (route.kind == RouteKind::Direct || route.kind == RouteKind::Punched);
}

bool validClientPlayerRootRuntimeReceipt(
    const ClientPlayerRootRuntimeReceipt& receipt,
    const ClientPlayerRootRole role,
    const qnetplay::config_provenance::ConfigurationProvenanceWire&
        expectedProvenance) {
    using qnetplay::config_provenance::Stage;
    return validClientPlayerRootRole(role)
        && receipt.processId != 0u
        && receipt.publisherGeneration != 0u
        && receipt.publisherNonce != 0u
        && receipt.playerSlot == static_cast<uint8_t>(role)
        && receipt.runtimeStage == Stage::RuntimeApplied
        && qnetplay::config_provenance::validateCommittedWire(
               receipt.provenance) == nullptr
        && sameConfigurationProvenance(receipt.provenance,
                                       expectedProvenance);
}

bool writeClientPlayerRootRuntimeReceipt(
    Writer& writer, const ClientPlayerRootRuntimeReceipt& receipt,
    const ClientPlayerRootRole role,
    const qnetplay::config_provenance::ConfigurationProvenanceWire&
        expectedProvenance) {
    return validClientPlayerRootRuntimeReceipt(receipt, role,
                                               expectedProvenance)
        && writer.u32(receipt.processId)
        && writer.u32(receipt.publisherGeneration)
        && writer.u64(receipt.publisherNonce)
        && writer.u8(receipt.playerSlot)
        && writer.u8(static_cast<uint8_t>(receipt.runtimeStage))
        && writer.u8(receipt.delay)
        && writer.u8(receipt.rollback)
        && writer.u8(receipt.remoteDelay)
        && writer.u8(receipt.remoteRollback)
        && writeConfigurationProvenance(writer, receipt.provenance);
}

bool readClientPlayerRootRuntimeReceipt(
    Reader& reader, ClientPlayerRootRuntimeReceipt& out,
    const ClientPlayerRootRole role,
    const qnetplay::config_provenance::ConfigurationProvenanceWire&
        expectedProvenance) {
    ClientPlayerRootRuntimeReceipt decoded{};
    uint8_t rawStage = 0u;
    if (!reader.u32(decoded.processId)
        || !reader.u32(decoded.publisherGeneration)
        || !reader.u64(decoded.publisherNonce)
        || !reader.u8(decoded.playerSlot)
        || !reader.u8(rawStage)
        || !reader.u8(decoded.delay)
        || !reader.u8(decoded.rollback)
        || !reader.u8(decoded.remoteDelay)
        || !reader.u8(decoded.remoteRollback)
        || !readConfigurationProvenance(reader, decoded.provenance)) {
        return false;
    }
    decoded.runtimeStage =
        static_cast<qnetplay::config_provenance::Stage>(rawStage);
    if (!validClientPlayerRootRuntimeReceipt(decoded, role,
                                              expectedProvenance)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validClientPlayerRootBinding(const ClientPlayerRootBinding& binding) {
    return binding.stream.valid()
        && binding.gameStartFingerprint != 0u
        && binding.gameStartFingerprint == binding.stream.streamRootIdentity
        && binding.clientRootNodeId != kInvalidNodeId
        && validClientPlayerRootRole(binding.remotePlayerRole)
        && qnetplay::config_provenance::validateCommittedWire(
               binding.configurationProvenance) == nullptr
        && validClientPlayerRootRuntimeReceipt(
            binding.runtimeReceipt, binding.remotePlayerRole,
            binding.configurationProvenance)
        && binding.sourceGeneration != 0u
        && binding.qspRouteGeneration != 0u
        && binding.qsrBindingGeneration != 0u
        && validClientPlayerRootRoute(binding.dataRoute)
        && validClientPlayerRootRoute(binding.qsrRoute)
        && !binding.dataRoute.sameEndpoint(binding.qsrRoute)
        && binding.dataRoute.generation == binding.qspRouteGeneration
        && binding.qsrRoute.generation == binding.qsrBindingGeneration
        && binding.cacheRange.firstCursor <= binding.cacheRange.nextCursor
        && binding.cacheRange.nextCursor == binding.cursor.nextRecord
        && binding.cursor.digest != 0u
        && binding.childCapacity != 0u
        && binding.expiresMs != 0u;
}

bool writeClientPlayerRootBinding(Writer& writer,
                                  const ClientPlayerRootBinding& binding) {
    return validClientPlayerRootBinding(binding)
        && writeStream(writer, binding.stream)
        && writer.u64(binding.gameStartFingerprint)
        && writer.u64(binding.clientRootNodeId)
        && writer.u8(static_cast<uint8_t>(binding.remotePlayerRole))
        && writeConfigurationProvenance(writer,
                                        binding.configurationProvenance)
        && writeClientPlayerRootRuntimeReceipt(
            writer, binding.runtimeReceipt, binding.remotePlayerRole,
            binding.configurationProvenance)
        && writer.u64(binding.sourceGeneration)
        && writer.u64(binding.qspRouteGeneration)
        && writer.u64(binding.qsrBindingGeneration)
        && writeRoute(writer, binding.dataRoute)
        && writeRoute(writer, binding.qsrRoute)
        && writer.u32(binding.cacheRange.firstCursor)
        && writer.u32(binding.cacheRange.nextCursor)
        && writer.u32(binding.cursor.nextRecord)
        && writer.u32(binding.cursor.nextEvent)
        && writer.u64(binding.cursor.digest)
        && writer.u32(binding.childCapacity)
        && writer.u64(binding.expiresMs);
}

bool readClientPlayerRootBinding(Reader& reader,
                                 ClientPlayerRootBinding& out) {
    ClientPlayerRootBinding decoded{};
    uint8_t rawRole = 0u;
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.gameStartFingerprint)
        || !reader.u64(decoded.clientRootNodeId)
        || !reader.u8(rawRole)) {
        return false;
    }
    decoded.remotePlayerRole = static_cast<ClientPlayerRootRole>(rawRole);
    if (!validClientPlayerRootRole(decoded.remotePlayerRole)
        || !readConfigurationProvenance(reader,
                                        decoded.configurationProvenance)
        || !readClientPlayerRootRuntimeReceipt(
            reader, decoded.runtimeReceipt, decoded.remotePlayerRole,
            decoded.configurationProvenance)
        || !reader.u64(decoded.sourceGeneration)
        || !reader.u64(decoded.qspRouteGeneration)
        || !reader.u64(decoded.qsrBindingGeneration)
        || !readRoute(reader, decoded.dataRoute)
        || !readRoute(reader, decoded.qsrRoute)
        || !reader.u32(decoded.cacheRange.firstCursor)
        || !reader.u32(decoded.cacheRange.nextCursor)
        || !reader.u32(decoded.cursor.nextRecord)
        || !reader.u32(decoded.cursor.nextEvent)
        || !reader.u64(decoded.cursor.digest)
        || !reader.u32(decoded.childCapacity)
        || !reader.u64(decoded.expiresMs)
        || !validClientPlayerRootBinding(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool nonzeroClientPlayerRootSignature(const uint8_t* bytes,
                                      const size_t byteCount) {
    if (bytes == nullptr || byteCount == 0u) return false;
    return std::any_of(bytes, bytes + byteCount,
                       [](const uint8_t byte) { return byte != 0u; });
}

bool validClientPlayerRootGrantCore(const ClientPlayerRootGrant& grant) {
    return grant.authorityGeneration != 0u
        && grant.hostRootNodeId != kInvalidNodeId
        && grant.grantNonce != 0u
        && validClientPlayerRootBinding(grant.binding)
        && grant.binding.clientRootNodeId != grant.hostRootNodeId;
}

bool validClientPlayerRootGrant(const ClientPlayerRootGrant& grant) {
    return validClientPlayerRootGrantCore(grant)
        && nonzeroClientPlayerRootSignature(grant.hostSignature.data(),
                                            grant.hostSignature.size());
}

bool writeClientPlayerRootGrantUnsigned(Writer& writer,
                                        const ClientPlayerRootGrant& grant) {
    return validClientPlayerRootGrantCore(grant)
        && writer.u64(grant.authorityGeneration)
        && writer.u64(grant.hostRootNodeId)
        && writer.u64(grant.grantNonce)
        && writeClientPlayerRootBinding(writer, grant.binding);
}

bool writeClientPlayerRootGrant(Writer& writer,
                                const ClientPlayerRootGrant& grant) {
    return validClientPlayerRootGrant(grant)
        && writeClientPlayerRootGrantUnsigned(writer, grant)
        && writer.bytes(grant.hostSignature.data(), grant.hostSignature.size());
}

bool readClientPlayerRootGrant(Reader& reader, ClientPlayerRootGrant& out) {
    ClientPlayerRootGrant decoded{};
    if (!reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.hostRootNodeId)
        || !reader.u64(decoded.grantNonce)
        || !readClientPlayerRootBinding(reader, decoded.binding)
        || !reader.bytes(decoded.hostSignature.data(),
                         decoded.hostSignature.size())
        || !validClientPlayerRootGrant(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validClientPlayerRootAssertion(
    const ClientPlayerRootAssertion& assertion) {
    return assertion.authorityGeneration != 0u
        && assertion.hostRootNodeId != kInvalidNodeId
        && assertion.grantNonce != 0u
        && validClientPlayerRootBinding(assertion.binding)
        && assertion.binding.clientRootNodeId != assertion.hostRootNodeId;
}

bool writeClientPlayerRootAssertion(Writer& writer,
                                    const ClientPlayerRootAssertion& assertion) {
    return validClientPlayerRootAssertion(assertion)
        && writer.u64(assertion.authorityGeneration)
        && writer.u64(assertion.hostRootNodeId)
        && writer.u64(assertion.grantNonce)
        && writeClientPlayerRootBinding(writer, assertion.binding);
}

bool readClientPlayerRootAssertion(Reader& reader,
                                   ClientPlayerRootAssertion& out) {
    ClientPlayerRootAssertion decoded{};
    if (!reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.hostRootNodeId)
        || !reader.u64(decoded.grantNonce)
        || !readClientPlayerRootBinding(reader, decoded.binding)
        || !validClientPlayerRootAssertion(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validClientPlayerRootActivationCore(
    const ClientPlayerRootActivation& activation) {
    const ParentLease& lease = activation.targetLease;
    const ClientPlayerRootBinding& binding = activation.binding;
    return activation.authorityGeneration != 0u
        && activation.hostRootNodeId != kInvalidNodeId
        && validClientPlayerRootBinding(binding)
        && binding.clientRootNodeId != activation.hostRootNodeId
        && activation.targetViewerNodeId != kInvalidNodeId
        && activation.targetViewerNodeId != activation.hostRootNodeId
        && activation.targetViewerNodeId != binding.clientRootNodeId
        && validParentLease(lease)
        && lease.stream == binding.stream
        && lease.childId == activation.targetViewerNodeId
        && lease.parentId == binding.clientRootNodeId
        && lease.parentDepth == 0u
        && lease.parentSourceGeneration == binding.sourceGeneration
        && lease.routeGeneration == binding.dataRoute.generation
        && lease.routeCount == 1u
        && sameRoute(lease.routes[0], binding.dataRoute)
        && lease.cacheRange.firstCursor == binding.cacheRange.firstCursor
        && lease.cacheRange.nextCursor == binding.cacheRange.nextCursor
        && lease.latestVerifiedCursor == binding.cursor.nextRecord
        && lease.childBinding.authorityGeneration
            == activation.authorityGeneration
        && lease.childBinding.childRequestedCursor.nextRecord
            == binding.cursor.nextRecord
        && lease.childBinding.childRequestedCursor.nextEvent
            == binding.cursor.nextEvent
        && lease.childBinding.childRequestedCursor.digest
            == binding.cursor.digest
        && lease.expiryMs == activation.activationDeadlineMs
        && activation.activationDeadlineMs <= binding.expiresMs
        && activation.activationNonce != 0u
        && activation.activationDeadlineMs != 0u
        && lease.ancestryCount == 1u
        && lease.ancestry[0] == binding.clientRootNodeId
        && validClientPlayerRootRoute(activation.targetDataRoute)
        && sameRoute(activation.targetDataRoute,
                     lease.childBinding.childQspRoute)
        && activation.targetQsrRoute.valid()
        && activation.targetQsrRoute.routeProved
        && activation.targetQsrRoute.kind == RouteKind::Direct
        && activation.targetQsrRoute.generation
            == lease.childBinding.childEndpointBindingGeneration
        && !activation.targetDataRoute.sameEndpoint(activation.targetQsrRoute)
        && !binding.dataRoute.sameEndpoint(activation.targetDataRoute)
        && !binding.dataRoute.sameEndpoint(activation.targetQsrRoute)
        && !binding.qsrRoute.sameEndpoint(activation.targetDataRoute)
        && !binding.qsrRoute.sameEndpoint(activation.targetQsrRoute);
}

bool validClientPlayerRootActivation(
    const ClientPlayerRootActivation& activation) {
    return validClientPlayerRootActivationCore(activation)
        && nonzeroClientPlayerRootSignature(activation.hostSignature.data(),
                                            activation.hostSignature.size());
}

bool writeClientPlayerRootActivationUnsigned(
    Writer& writer, const ClientPlayerRootActivation& activation) {
    return validClientPlayerRootActivationCore(activation)
        && writer.u64(activation.authorityGeneration)
        && writer.u64(activation.hostRootNodeId)
        && writeClientPlayerRootBinding(writer, activation.binding)
        && writer.u64(activation.targetViewerNodeId)
        && writeParentLease(writer, activation.targetLease)
        && writeRoute(writer, activation.targetDataRoute)
        && writeRoute(writer, activation.targetQsrRoute)
        && writer.u64(activation.activationNonce)
        && writer.u64(activation.activationDeadlineMs);
}

bool writeClientPlayerRootActivation(Writer& writer,
                                     const ClientPlayerRootActivation& activation) {
    return validClientPlayerRootActivation(activation)
        && writeClientPlayerRootActivationUnsigned(writer, activation)
        && writer.bytes(activation.hostSignature.data(),
                        activation.hostSignature.size());
}

bool readClientPlayerRootActivation(Reader& reader,
                                    ClientPlayerRootActivation& out) {
    ClientPlayerRootActivation decoded{};
    if (!reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.hostRootNodeId)
        || !readClientPlayerRootBinding(reader, decoded.binding)
        || !reader.u64(decoded.targetViewerNodeId)
        || !readParentLease(reader, decoded.targetLease)
        || !readRoute(reader, decoded.targetDataRoute)
        || !readRoute(reader, decoded.targetQsrRoute)
        || !reader.u64(decoded.activationNonce)
        || !reader.u64(decoded.activationDeadlineMs)
        || !reader.bytes(decoded.hostSignature.data(),
                         decoded.hostSignature.size())
        || !validClientPlayerRootActivation(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validClientPlayerRootActivationReceipt(
    const ClientPlayerRootActivationReceipt& receipt) {
    return validClientPlayerRootActivation(receipt.activation);
}

bool writeClientPlayerRootActivationReceipt(
    Writer& writer, const ClientPlayerRootActivationReceipt& receipt) {
    return validClientPlayerRootActivationReceipt(receipt)
        && writeClientPlayerRootActivation(writer, receipt.activation);
}

bool readClientPlayerRootActivationReceipt(
    Reader& reader, ClientPlayerRootActivationReceipt& out) {
    ClientPlayerRootActivationReceipt decoded{};
    if (!readClientPlayerRootActivation(reader, decoded.activation)
        || !validClientPlayerRootActivationReceipt(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validClientPlayerRootActivationCommitImpl(
    const ClientPlayerRootActivationCommit& commit) {
    return validClientPlayerRootActivation(commit.activation)
        && nonzeroClientPlayerRootSignature(commit.hostSignature.data(),
                                            commit.hostSignature.size());
}

bool writeClientPlayerRootActivationCommit(
    Writer& writer, const ClientPlayerRootActivationCommit& commit) {
    return validClientPlayerRootActivationCommitImpl(commit)
        && writeClientPlayerRootActivation(writer, commit.activation)
        && writer.bytes(commit.hostSignature.data(), commit.hostSignature.size());
}

bool readClientPlayerRootActivationCommit(
    Reader& reader, ClientPlayerRootActivationCommit& out) {
    ClientPlayerRootActivationCommit decoded{};
    if (!readClientPlayerRootActivation(reader, decoded.activation)
        || !reader.bytes(decoded.hostSignature.data(),
                         decoded.hostSignature.size())
        || !validClientPlayerRootActivationCommitImpl(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validClientPlayerRootActivationRetirementImpl(
    const ClientPlayerRootActivationRetirement& retirement) {
    return validClientPlayerRootActivation(retirement.activation)
        && validClientPlayerRootRetirementReason(retirement.reason)
        && nonzeroClientPlayerRootSignature(retirement.hostSignature.data(),
                                            retirement.hostSignature.size());
}

bool writeClientPlayerRootActivationRetirement(
    Writer& writer, const ClientPlayerRootActivationRetirement& retirement) {
    return validClientPlayerRootActivationRetirementImpl(retirement)
        && writeClientPlayerRootActivation(writer, retirement.activation)
        && writer.u8(static_cast<uint8_t>(retirement.reason))
        && writer.bytes(retirement.hostSignature.data(),
                        retirement.hostSignature.size());
}

bool readClientPlayerRootActivationRetirement(
    Reader& reader, ClientPlayerRootActivationRetirement& out) {
    ClientPlayerRootActivationRetirement decoded{};
    uint8_t rawReason = 0u;
    if (!readClientPlayerRootActivation(reader, decoded.activation)
        || !reader.u8(rawReason)
        || !reader.bytes(decoded.hostSignature.data(),
                         decoded.hostSignature.size())) {
        return false;
    }
    decoded.reason = static_cast<ClientPlayerRootRetirementReason>(rawReason);
    if (!validClientPlayerRootActivationRetirementImpl(decoded)) return false;
    out = decoded;
    return true;
}

bool validRelayChildAssignmentCore(const RelayChildAssignment& assignment) {
    const RelayDataBindingRequest binding{
        assignment.stream, assignment.authorityGeneration,
        assignment.rootNodeId, assignment.relayNodeId,
        assignment.relayLease, assignment.bindingRequestNonce};
    const ParentLease& childLease = assignment.childLease;
    return validRelayDataBindingRequest(binding)
        && assignment.childNodeId != kInvalidNodeId
        && assignment.childNodeId != assignment.rootNodeId
        && assignment.childNodeId != assignment.relayNodeId
        && validRelayDataRoute(assignment.observedDataRoute)
        && validRelayDataRoute(assignment.observedControlRoute)
        && !assignment.observedDataRoute.sameEndpoint(
            assignment.observedControlRoute)
        && assignment.observedDataRoute.generation
            == assignment.observedControlRoute.generation
        && validParentLease(childLease)
        && (childLease.macLow != 0u || childLease.macHigh != 0u)
        && childLease.stream == assignment.stream
        && childLease.childId == assignment.childNodeId
        && childLease.parentId == assignment.relayNodeId
        && childLease.childBinding.authorityGeneration
            == assignment.authorityGeneration
        && childLease.parentDepth != 0u
        && childLease.parentDepth < childLease.ancestry.size()
        && childLease.ancestryCount
            == static_cast<uint8_t>(childLease.parentDepth + 1u)
        && childLease.parentSourceGeneration
            == assignment.relayLease.parentSourceGeneration
        && childLease.routeGeneration == assignment.observedDataRoute.generation
        && childLease.routeCount == 1u
        && sameRoute(childLease.routes[0], assignment.observedDataRoute)
        && childLease.cacheRange.containsBoundary(
            childLease.childBinding.childRequestedCursor.nextRecord)
        && childLease.expiryMs <= assignment.relayLease.expiryMs
        && childLease.ancestryCount >= 2u
        && childLease.ancestry[0] == assignment.rootNodeId
        && childLease.ancestry[childLease.ancestryCount - 1u]
            == assignment.relayNodeId;
}

bool nonzeroRelayChildAssignmentSignature(
    const std::array<uint8_t, kRelayChildAssignmentHostSignatureBytes>&
        signature) {
    return std::any_of(signature.begin(), signature.end(),
                       [](const uint8_t byte) { return byte != 0u; });
}

bool validRelayChildAssignment(const RelayChildAssignment& assignment) {
    return validRelayChildAssignmentCore(assignment)
        && nonzeroRelayChildAssignmentSignature(assignment.hostSignature);
}

bool writeRelayChildAssignmentUnsigned(
    Writer& writer, const RelayChildAssignment& assignment) {
    return validRelayChildAssignmentCore(assignment)
        && writeStream(writer, assignment.stream)
        && writer.u64(assignment.authorityGeneration)
        && writer.u64(assignment.rootNodeId)
        && writer.u64(assignment.relayNodeId)
        && writer.u64(assignment.childNodeId)
        && writeLeaseBinding(writer, assignment.relayLease)
        && writer.u64(assignment.bindingRequestNonce)
        && writeRoute(writer, assignment.observedDataRoute)
        && writeRoute(writer, assignment.observedControlRoute)
        && writeParentLease(writer, assignment.childLease);
}

bool writeRelayChildAssignment(Writer& writer,
                               const RelayChildAssignment& assignment) {
    return validRelayChildAssignment(assignment)
        && writeRelayChildAssignmentUnsigned(writer, assignment)
        && writer.bytes(assignment.hostSignature.data(),
                        assignment.hostSignature.size());
}

bool readRelayChildAssignment(Reader& reader, RelayChildAssignment& out) {
    RelayChildAssignment decoded{};
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.rootNodeId)
        || !reader.u64(decoded.relayNodeId)
        || !reader.u64(decoded.childNodeId)
        || !readLeaseBinding(reader, decoded.relayLease)
        || !reader.u64(decoded.bindingRequestNonce)
        || !readRoute(reader, decoded.observedDataRoute)
        || !readRoute(reader, decoded.observedControlRoute)
        || !readParentLease(reader, decoded.childLease)
        || !reader.bytes(decoded.hostSignature.data(),
                         decoded.hostSignature.size())
        || !validRelayChildAssignment(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validRelayChildRetirement(const RelayChildRetirement& retirement) {
    const RelayDataBindingRequest binding{
        retirement.stream, retirement.authorityGeneration,
        retirement.rootNodeId, retirement.relayNodeId,
        retirement.relayLease, retirement.bindingRequestNonce};
    const LeaseBinding& childLease = retirement.childLease;
    return validRelayDataBindingRequest(binding)
        && retirement.childNodeId != kInvalidNodeId
        && retirement.childNodeId != retirement.rootNodeId
        && retirement.childNodeId != retirement.relayNodeId
        && validLeaseBinding(childLease)
        && (childLease.macLow != 0u || childLease.macHigh != 0u)
        && childLease.childId == retirement.childNodeId
        && childLease.parentId == retirement.relayNodeId
        && childLease.parentSourceGeneration
            == retirement.relayLease.parentSourceGeneration
        && childLease.expiryMs <= retirement.relayLease.expiryMs
        && childLease.childBinding.authorityGeneration
            == retirement.authorityGeneration
        && validRelayChildRetirementReason(retirement.reason);
}

bool writeRelayChildRetirement(Writer& writer,
                               const RelayChildRetirement& retirement) {
    return validRelayChildRetirement(retirement)
        && writeStream(writer, retirement.stream)
        && writer.u64(retirement.authorityGeneration)
        && writer.u64(retirement.rootNodeId)
        && writer.u64(retirement.relayNodeId)
        && writer.u64(retirement.childNodeId)
        && writeLeaseBinding(writer, retirement.relayLease)
        && writer.u64(retirement.bindingRequestNonce)
        && writeLeaseBinding(writer, retirement.childLease)
        && writer.u8(static_cast<uint8_t>(retirement.reason));
}

bool readRelayChildRetirement(Reader& reader, RelayChildRetirement& out) {
    RelayChildRetirement decoded{};
    uint8_t rawReason = 0u;
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.rootNodeId)
        || !reader.u64(decoded.relayNodeId)
        || !reader.u64(decoded.childNodeId)
        || !readLeaseBinding(reader, decoded.relayLease)
        || !reader.u64(decoded.bindingRequestNonce)
        || !readLeaseBinding(reader, decoded.childLease)
        || !reader.u8(rawReason)) {
        return false;
    }
    decoded.reason = static_cast<RelayChildRetirementReason>(rawReason);
    if (!validRelayChildRetirement(decoded)) return false;
    out = decoded;
    return true;
}

LeaseBinding leaseBindingFor(const ParentLease& lease) {
    LeaseBinding binding{};
    binding.childId = lease.childId;
    binding.parentId = lease.parentId;
    binding.parentSourceGeneration = lease.parentSourceGeneration;
    binding.routeGeneration = lease.routeGeneration;
    binding.expiryMs = lease.expiryMs;
    binding.replayNonce = lease.replayNonce;
    binding.macLow = lease.macLow;
    binding.macHigh = lease.macHigh;
    binding.childBinding = lease.childBinding;
    return binding;
}

bool sameLeaseBinding(const LeaseBinding& left, const LeaseBinding& right) {
    return left.childId == right.childId && left.parentId == right.parentId
        && left.parentSourceGeneration == right.parentSourceGeneration
        && left.routeGeneration == right.routeGeneration
        && left.expiryMs == right.expiryMs
        && left.replayNonce == right.replayNonce
        && left.macLow == right.macLow && left.macHigh == right.macHigh
        && left.childBinding == right.childBinding;
}

bool validActivationChildDataRoute(const RouteDescriptor& route) {
    return route.valid() && route.routeProved
        && (route.kind == RouteKind::Direct || route.kind == RouteKind::Punched);
}

bool activationRoutesAreDistinct(const RouteDescriptor& relayData,
                                 const RouteDescriptor& relayControl,
                                 const RouteDescriptor& childData,
                                 const RouteDescriptor& childControl) {
    return !relayData.sameEndpoint(relayControl)
        && !relayData.sameEndpoint(childData)
        && !relayData.sameEndpoint(childControl)
        && !relayControl.sameEndpoint(childData)
        && !relayControl.sameEndpoint(childControl)
        && !childData.sameEndpoint(childControl);
}

bool validRelayChildActivationCore(const RelayChildActivation& activation) {
    const RelayDataBindingRequest relayBinding{
        activation.stream, activation.authorityGeneration,
        activation.rootNodeId, activation.relayNodeId,
        activation.relayLease, activation.relayBindingRequestNonce};
    const ParentLease& childLease = activation.childLease;
    return validRelayDataBindingRequest(relayBinding)
        && activation.childNodeId != kInvalidNodeId
        && activation.childNodeId != activation.rootNodeId
        && activation.childNodeId != activation.relayNodeId
        && validRelayDataRoute(activation.relayDataRoute)
        && validRelayDataRoute(activation.relayControlRoute)
        && validActivationChildDataRoute(activation.childDataRoute)
        && validRelayDataRoute(activation.childControlRoute)
        && activationRoutesAreDistinct(
            activation.relayDataRoute, activation.relayControlRoute,
            activation.childDataRoute, activation.childControlRoute)
        && activation.relayDataRoute.generation
            == activation.relayLease.childBinding.childEndpointBindingGeneration
        && activation.relayControlRoute.generation
            == activation.relayLease.childBinding.childEndpointBindingGeneration
        && validParentLease(childLease)
        && (childLease.macLow != 0u || childLease.macHigh != 0u)
        && childLease.stream == activation.stream
        && childLease.childId == activation.childNodeId
        && childLease.parentId == activation.relayNodeId
        && childLease.childBinding.authorityGeneration
            == activation.authorityGeneration
        && sameRoute(childLease.childBinding.childQspRoute,
                     activation.childDataRoute)
        && activation.childControlRoute.generation
            == childLease.childBinding.childEndpointBindingGeneration
        && childLease.parentDepth != 0u
        && childLease.parentDepth < childLease.ancestry.size()
        && childLease.ancestryCount
            == static_cast<uint8_t>(childLease.parentDepth + 1u)
        && childLease.parentSourceGeneration
            == activation.relayLease.parentSourceGeneration
        && childLease.routeGeneration == activation.relayDataRoute.generation
        && childLease.routeCount == 1u
        && sameRoute(childLease.routes[0], activation.relayDataRoute)
        && childLease.cacheRange.firstCursor
            == activation.relayCacheRange.firstCursor
        && childLease.cacheRange.nextCursor
            == activation.relayCacheRange.nextCursor
        && activation.relayCacheRange.containsBoundary(
            activation.commonCursor.nextRecord)
        && activation.relayCacheRange.nextCursor
            == activation.commonCursor.nextRecord
        && childLease.latestVerifiedCursor == activation.commonCursor.nextRecord
        && childLease.childBinding.childRequestedCursor.nextRecord
            == activation.commonCursor.nextRecord
        && childLease.childBinding.childRequestedCursor.nextEvent
            == activation.commonCursor.nextEvent
        && childLease.childBinding.childRequestedCursor.digest
            == activation.commonCursor.digest
        && childLease.expiryMs == activation.activationDeadlineMs
        && activation.activationDeadlineMs <= activation.relayLease.expiryMs
        && activation.activationNonce != 0u
        && activation.activationDeadlineMs != 0u
        && childLease.ancestryCount >= 2u
        && childLease.ancestry[0] == activation.rootNodeId
        && childLease.ancestry[childLease.ancestryCount - 1u]
            == activation.relayNodeId;
}

bool nonzeroRelayChildActivationSignature(
    const std::array<uint8_t, kRelayChildActivationHostSignatureBytes>&
        signature) {
    return std::any_of(signature.begin(), signature.end(),
                       [](const uint8_t byte) { return byte != 0u; });
}

bool validRelayChildActivation(const RelayChildActivation& activation) {
    return validRelayChildActivationCore(activation)
        && nonzeroRelayChildActivationSignature(activation.hostSignature);
}

bool writeRelayChildActivationUnsigned(
    Writer& writer, const RelayChildActivation& activation) {
    return validRelayChildActivationCore(activation)
        && writeStream(writer, activation.stream)
        && writer.u64(activation.authorityGeneration)
        && writer.u64(activation.rootNodeId)
        && writer.u64(activation.relayNodeId)
        && writer.u64(activation.childNodeId)
        && writeLeaseBinding(writer, activation.relayLease)
        && writer.u64(activation.relayBindingRequestNonce)
        && writeRoute(writer, activation.relayDataRoute)
        && writeRoute(writer, activation.relayControlRoute)
        && writeParentLease(writer, activation.childLease)
        && writeRoute(writer, activation.childDataRoute)
        && writeRoute(writer, activation.childControlRoute)
        && writer.u32(activation.relayCacheRange.firstCursor)
        && writer.u32(activation.relayCacheRange.nextCursor)
        && writer.u32(activation.commonCursor.nextRecord)
        && writer.u32(activation.commonCursor.nextEvent)
        && writer.u64(activation.commonCursor.digest)
        && writer.u64(activation.activationNonce)
        && writer.u64(activation.activationDeadlineMs);
}

bool writeRelayChildActivation(Writer& writer,
                               const RelayChildActivation& activation) {
    return validRelayChildActivation(activation)
        && writeRelayChildActivationUnsigned(writer, activation)
        && writer.bytes(activation.hostSignature.data(),
                        activation.hostSignature.size());
}

bool readRelayChildActivation(Reader& reader, RelayChildActivation& out) {
    RelayChildActivation decoded{};
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.rootNodeId)
        || !reader.u64(decoded.relayNodeId)
        || !reader.u64(decoded.childNodeId)
        || !readLeaseBinding(reader, decoded.relayLease)
        || !reader.u64(decoded.relayBindingRequestNonce)
        || !readRoute(reader, decoded.relayDataRoute)
        || !readRoute(reader, decoded.relayControlRoute)
        || !readParentLease(reader, decoded.childLease)
        || !readRoute(reader, decoded.childDataRoute)
        || !readRoute(reader, decoded.childControlRoute)
        || !reader.u32(decoded.relayCacheRange.firstCursor)
        || !reader.u32(decoded.relayCacheRange.nextCursor)
        || !reader.u32(decoded.commonCursor.nextRecord)
        || !reader.u32(decoded.commonCursor.nextEvent)
        || !reader.u64(decoded.commonCursor.digest)
        || !reader.u64(decoded.activationNonce)
        || !reader.u64(decoded.activationDeadlineMs)
        || !reader.bytes(decoded.hostSignature.data(),
                         decoded.hostSignature.size())
        || !validRelayChildActivation(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool nonzeroRelayChildActivationCommitSignature(
    const std::array<uint8_t, kRelayChildActivationCommitHostSignatureBytes>&
        signature) {
    return std::any_of(signature.begin(), signature.end(),
                       [](const uint8_t byte) { return byte != 0u; });
}

bool validRelayChildActivationCommitImpl(
    const RelayChildActivationCommit& commit) {
    return validRelayChildActivation(commit.activation)
        && nonzeroRelayChildActivationCommitSignature(commit.hostSignature);
}

bool writeRelayChildActivationCommit(
    Writer& writer, const RelayChildActivationCommit& commit) {
    return validRelayChildActivationCommitImpl(commit)
        && writeRelayChildActivation(writer, commit.activation)
        && writer.bytes(commit.hostSignature.data(), commit.hostSignature.size());
}

bool readRelayChildActivationCommit(Reader& reader,
                                    RelayChildActivationCommit& out) {
    RelayChildActivationCommit decoded{};
    if (!readRelayChildActivation(reader, decoded.activation)
        || !reader.bytes(decoded.hostSignature.data(),
                         decoded.hostSignature.size())
        || !validRelayChildActivationCommitImpl(decoded)) {
        return false;
    }
    out = decoded;
    return true;
}

bool validRelayChildActivationReceipt(
    const RelayChildActivationReceipt& receipt) {
    const RelayDataBindingRequest relayBinding{
        receipt.stream, receipt.authorityGeneration, receipt.rootNodeId,
        receipt.relayNodeId, receipt.relayLease,
        receipt.relayBindingRequestNonce};
    return validRelayDataBindingRequest(relayBinding)
        && receipt.childNodeId != kInvalidNodeId
        && receipt.childNodeId != receipt.rootNodeId
        && receipt.childNodeId != receipt.relayNodeId
        && validLeaseBinding(receipt.childLease)
        && (receipt.childLease.macLow != 0u || receipt.childLease.macHigh != 0u)
        && receipt.childLease.childId == receipt.childNodeId
        && receipt.childLease.parentId == receipt.relayNodeId
        && receipt.childLease.parentSourceGeneration
            == receipt.relayLease.parentSourceGeneration
        && receipt.childLease.childBinding.authorityGeneration
            == receipt.authorityGeneration
        && validRelayDataRoute(receipt.relayDataRoute)
        && validRelayDataRoute(receipt.relayControlRoute)
        && validActivationChildDataRoute(receipt.childDataRoute)
        && validRelayDataRoute(receipt.childControlRoute)
        && activationRoutesAreDistinct(
            receipt.relayDataRoute, receipt.relayControlRoute,
            receipt.childDataRoute, receipt.childControlRoute)
        && receipt.relayDataRoute.generation
            == receipt.relayLease.childBinding.childEndpointBindingGeneration
        && receipt.relayControlRoute.generation
            == receipt.relayLease.childBinding.childEndpointBindingGeneration
        && sameRoute(receipt.childLease.childBinding.childQspRoute,
                     receipt.childDataRoute)
        && receipt.childControlRoute.generation
            == receipt.childLease.childBinding.childEndpointBindingGeneration
        && receipt.relayCacheRange.firstCursor
            <= receipt.relayCacheRange.nextCursor
        && receipt.relayCacheRange.containsBoundary(
            receipt.commonCursor.nextRecord)
        && receipt.relayCacheRange.nextCursor
            == receipt.commonCursor.nextRecord
        && receipt.childLease.childBinding.childRequestedCursor.nextRecord
            == receipt.commonCursor.nextRecord
        && receipt.childLease.childBinding.childRequestedCursor.nextEvent
            == receipt.commonCursor.nextEvent
        && receipt.childLease.childBinding.childRequestedCursor.digest
            == receipt.commonCursor.digest
        && receipt.childLease.expiryMs == receipt.activationDeadlineMs
        && receipt.activationDeadlineMs <= receipt.relayLease.expiryMs
        && receipt.activationNonce != 0u
        && receipt.activationDeadlineMs != 0u
        && validRelayChildActivationReceiptRole(receipt.role);
}

bool writeRelayChildActivationReceipt(
    Writer& writer, const RelayChildActivationReceipt& receipt) {
    return validRelayChildActivationReceipt(receipt)
        && writeStream(writer, receipt.stream)
        && writer.u64(receipt.authorityGeneration)
        && writer.u64(receipt.rootNodeId)
        && writer.u64(receipt.relayNodeId)
        && writer.u64(receipt.childNodeId)
        && writeLeaseBinding(writer, receipt.relayLease)
        && writeLeaseBinding(writer, receipt.childLease)
        && writer.u64(receipt.relayBindingRequestNonce)
        && writeRoute(writer, receipt.relayDataRoute)
        && writeRoute(writer, receipt.relayControlRoute)
        && writeRoute(writer, receipt.childDataRoute)
        && writeRoute(writer, receipt.childControlRoute)
        && writer.u32(receipt.relayCacheRange.firstCursor)
        && writer.u32(receipt.relayCacheRange.nextCursor)
        && writer.u32(receipt.commonCursor.nextRecord)
        && writer.u32(receipt.commonCursor.nextEvent)
        && writer.u64(receipt.commonCursor.digest)
        && writer.u64(receipt.activationNonce)
        && writer.u64(receipt.activationDeadlineMs)
        && writer.u8(static_cast<uint8_t>(receipt.role));
}

bool readRelayChildActivationReceipt(
    Reader& reader, RelayChildActivationReceipt& out) {
    RelayChildActivationReceipt decoded{};
    uint8_t rawRole = 0u;
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.rootNodeId)
        || !reader.u64(decoded.relayNodeId)
        || !reader.u64(decoded.childNodeId)
        || !readLeaseBinding(reader, decoded.relayLease)
        || !readLeaseBinding(reader, decoded.childLease)
        || !reader.u64(decoded.relayBindingRequestNonce)
        || !readRoute(reader, decoded.relayDataRoute)
        || !readRoute(reader, decoded.relayControlRoute)
        || !readRoute(reader, decoded.childDataRoute)
        || !readRoute(reader, decoded.childControlRoute)
        || !reader.u32(decoded.relayCacheRange.firstCursor)
        || !reader.u32(decoded.relayCacheRange.nextCursor)
        || !reader.u32(decoded.commonCursor.nextRecord)
        || !reader.u32(decoded.commonCursor.nextEvent)
        || !reader.u64(decoded.commonCursor.digest)
        || !reader.u64(decoded.activationNonce)
        || !reader.u64(decoded.activationDeadlineMs)
        || !reader.u8(rawRole)) {
        return false;
    }
    decoded.role = static_cast<RelayChildActivationReceiptRole>(rawRole);
    if (!validRelayChildActivationReceipt(decoded)) return false;
    out = decoded;
    return true;
}

bool validRelayChildActivationRetirement(
    const RelayChildActivationRetirement& retirement) {
    RelayChildActivationReceipt comparable{};
    comparable.stream = retirement.stream;
    comparable.authorityGeneration = retirement.authorityGeneration;
    comparable.rootNodeId = retirement.rootNodeId;
    comparable.relayNodeId = retirement.relayNodeId;
    comparable.childNodeId = retirement.childNodeId;
    comparable.relayLease = retirement.relayLease;
    comparable.childLease = retirement.childLease;
    comparable.relayBindingRequestNonce = retirement.relayBindingRequestNonce;
    comparable.relayDataRoute = retirement.relayDataRoute;
    comparable.relayControlRoute = retirement.relayControlRoute;
    comparable.childDataRoute = retirement.childDataRoute;
    comparable.childControlRoute = retirement.childControlRoute;
    comparable.relayCacheRange = retirement.relayCacheRange;
    comparable.commonCursor = retirement.commonCursor;
    comparable.activationNonce = retirement.activationNonce;
    comparable.activationDeadlineMs = retirement.activationDeadlineMs;
    comparable.role = RelayChildActivationReceiptRole::Relay;
    return validRelayChildActivationReceipt(comparable)
        && validRelayChildRetirementReason(retirement.reason);
}

bool writeRelayChildActivationRetirement(
    Writer& writer, const RelayChildActivationRetirement& retirement) {
    return validRelayChildActivationRetirement(retirement)
        && writeStream(writer, retirement.stream)
        && writer.u64(retirement.authorityGeneration)
        && writer.u64(retirement.rootNodeId)
        && writer.u64(retirement.relayNodeId)
        && writer.u64(retirement.childNodeId)
        && writeLeaseBinding(writer, retirement.relayLease)
        && writeLeaseBinding(writer, retirement.childLease)
        && writer.u64(retirement.relayBindingRequestNonce)
        && writeRoute(writer, retirement.relayDataRoute)
        && writeRoute(writer, retirement.relayControlRoute)
        && writeRoute(writer, retirement.childDataRoute)
        && writeRoute(writer, retirement.childControlRoute)
        && writer.u32(retirement.relayCacheRange.firstCursor)
        && writer.u32(retirement.relayCacheRange.nextCursor)
        && writer.u32(retirement.commonCursor.nextRecord)
        && writer.u32(retirement.commonCursor.nextEvent)
        && writer.u64(retirement.commonCursor.digest)
        && writer.u64(retirement.activationNonce)
        && writer.u64(retirement.activationDeadlineMs)
        && writer.u8(static_cast<uint8_t>(retirement.reason));
}

bool readRelayChildActivationRetirement(
    Reader& reader, RelayChildActivationRetirement& out) {
    RelayChildActivationRetirement decoded{};
    uint8_t rawReason = 0u;
    if (!readStream(reader, decoded.stream)
        || !reader.u64(decoded.authorityGeneration)
        || !reader.u64(decoded.rootNodeId)
        || !reader.u64(decoded.relayNodeId)
        || !reader.u64(decoded.childNodeId)
        || !readLeaseBinding(reader, decoded.relayLease)
        || !readLeaseBinding(reader, decoded.childLease)
        || !reader.u64(decoded.relayBindingRequestNonce)
        || !readRoute(reader, decoded.relayDataRoute)
        || !readRoute(reader, decoded.relayControlRoute)
        || !readRoute(reader, decoded.childDataRoute)
        || !readRoute(reader, decoded.childControlRoute)
        || !reader.u32(decoded.relayCacheRange.firstCursor)
        || !reader.u32(decoded.relayCacheRange.nextCursor)
        || !reader.u32(decoded.commonCursor.nextRecord)
        || !reader.u32(decoded.commonCursor.nextEvent)
        || !reader.u64(decoded.commonCursor.digest)
        || !reader.u64(decoded.activationNonce)
        || !reader.u64(decoded.activationDeadlineMs)
        || !reader.u8(rawReason)) {
        return false;
    }
    decoded.reason = static_cast<RelayChildRetirementReason>(rawReason);
    if (!validRelayChildActivationRetirement(decoded)) return false;
    out = decoded;
    return true;
}

bool writePayload(Writer& writer, const ControlDatagram& datagram) {
    switch (datagram.type) {
    case ControlMessageType::NodeAdvertisement:
        return writeNodeAdvertisement(writer, datagram.nodeAdvertisement);
    case ControlMessageType::ParentLease:
        return writeParentLease(writer, datagram.parentLease);
    case ControlMessageType::ContinuityRequest:
        return writeContinuityRequest(writer, datagram.continuityRequest);
    case ControlMessageType::ContinuityProof:
        return writeContinuityProof(writer, datagram.continuityProof);
    case ControlMessageType::ParentCommit:
        return writeParentCommit(writer, datagram.parentCommit);
    case ControlMessageType::DrainNotice:
        return writeBoundDrainNotice(writer, datagram.drainNotice);
    case ControlMessageType::AuthorityState:
        return writeAuthorityState(writer, datagram.authorityState);
    case ControlMessageType::RelayDataBindingRequest:
        return writeRelayDataBindingRequest(writer,
                                            datagram.relayDataBindingRequest);
    case ControlMessageType::RelayDataBindingAck:
        return writeRelayDataBindingAck(writer, datagram.relayDataBindingAck);
    case ControlMessageType::RelayReady:
        return writeRelayReady(writer, datagram.relayReady);
    case ControlMessageType::RelayChildAssignment:
        return writeRelayChildAssignment(writer, datagram.relayChildAssignment);
    case ControlMessageType::RelayChildRetirement:
        return writeRelayChildRetirement(writer, datagram.relayChildRetirement);
    case ControlMessageType::RelayChildActivation:
        return writeRelayChildActivation(writer, datagram.relayChildActivation);
    case ControlMessageType::RelayChildActivationReceipt:
        return writeRelayChildActivationReceipt(
            writer, datagram.relayChildActivationReceipt);
    case ControlMessageType::RelayChildActivationRetirement:
        return writeRelayChildActivationRetirement(
            writer, datagram.relayChildActivationRetirement);
    case ControlMessageType::RelayChildActivationCommit:
        return writeRelayChildActivationCommit(
            writer, datagram.relayChildActivationCommit);
    case ControlMessageType::ClientDataRootAssertion:
        return writeClientDataRootAssertion(
            writer, datagram.clientDataRootAssertion);
    case ControlMessageType::ClientPlayerRootGrant:
        return writeClientPlayerRootGrant(writer, datagram.clientPlayerRootGrant);
    case ControlMessageType::ClientPlayerRootAssertion:
        return writeClientPlayerRootAssertion(
            writer, datagram.clientPlayerRootAssertion);
    case ControlMessageType::ClientPlayerRootActivation:
        return writeClientPlayerRootActivation(
            writer, datagram.clientPlayerRootActivation);
    case ControlMessageType::ClientPlayerRootActivationReceipt:
        return writeClientPlayerRootActivationReceipt(
            writer, datagram.clientPlayerRootActivationReceipt);
    case ControlMessageType::ClientPlayerRootActivationCommit:
        return writeClientPlayerRootActivationCommit(
            writer, datagram.clientPlayerRootActivationCommit);
    case ControlMessageType::ClientPlayerRootActivationRetirement:
        return writeClientPlayerRootActivationRetirement(
            writer, datagram.clientPlayerRootActivationRetirement);
    }
    return false;
}

bool readPayload(Reader& reader, ControlDatagram& out) {
    switch (out.type) {
    case ControlMessageType::NodeAdvertisement:
        return readNodeAdvertisement(reader, out.nodeAdvertisement);
    case ControlMessageType::ParentLease:
        return readParentLease(reader, out.parentLease);
    case ControlMessageType::ContinuityRequest:
        return readContinuityRequest(reader, out.continuityRequest);
    case ControlMessageType::ContinuityProof:
        return readContinuityProof(reader, out.continuityProof);
    case ControlMessageType::ParentCommit:
        return readParentCommit(reader, out.parentCommit);
    case ControlMessageType::DrainNotice:
        return readBoundDrainNotice(reader, out.drainNotice);
    case ControlMessageType::AuthorityState:
        return readAuthorityState(reader, out.authorityState);
    case ControlMessageType::RelayDataBindingRequest:
        return readRelayDataBindingRequest(reader,
                                           out.relayDataBindingRequest);
    case ControlMessageType::RelayDataBindingAck:
        return readRelayDataBindingAck(reader, out.relayDataBindingAck);
    case ControlMessageType::RelayReady:
        return readRelayReady(reader, out.relayReady);
    case ControlMessageType::RelayChildAssignment:
        return readRelayChildAssignment(reader, out.relayChildAssignment);
    case ControlMessageType::RelayChildRetirement:
        return readRelayChildRetirement(reader, out.relayChildRetirement);
    case ControlMessageType::RelayChildActivation:
        return readRelayChildActivation(reader, out.relayChildActivation);
    case ControlMessageType::RelayChildActivationReceipt:
        return readRelayChildActivationReceipt(
            reader, out.relayChildActivationReceipt);
    case ControlMessageType::RelayChildActivationRetirement:
        return readRelayChildActivationRetirement(
            reader, out.relayChildActivationRetirement);
    case ControlMessageType::RelayChildActivationCommit:
        return readRelayChildActivationCommit(
            reader, out.relayChildActivationCommit);
    case ControlMessageType::ClientDataRootAssertion:
        return readClientDataRootAssertion(
            reader, out.clientDataRootAssertion);
    case ControlMessageType::ClientPlayerRootGrant:
        return readClientPlayerRootGrant(reader, out.clientPlayerRootGrant);
    case ControlMessageType::ClientPlayerRootAssertion:
        return readClientPlayerRootAssertion(
            reader, out.clientPlayerRootAssertion);
    case ControlMessageType::ClientPlayerRootActivation:
        return readClientPlayerRootActivation(
            reader, out.clientPlayerRootActivation);
    case ControlMessageType::ClientPlayerRootActivationReceipt:
        return readClientPlayerRootActivationReceipt(
            reader, out.clientPlayerRootActivationReceipt);
    case ControlMessageType::ClientPlayerRootActivationCommit:
        return readClientPlayerRootActivationCommit(
            reader, out.clientPlayerRootActivationCommit);
    case ControlMessageType::ClientPlayerRootActivationRetirement:
        return readClientPlayerRootActivationRetirement(
            reader, out.clientPlayerRootActivationRetirement);
    }
    return false;
}

}

bool validRelayChildActivationCommit(
    const RelayChildActivationCommit& commit) {
    return validRelayChildActivationCommitImpl(commit);
}

bool encodeRelayChildAssignmentAttestationInput(
    const RelayChildAssignment& assignment, std::vector<uint8_t>& out) {
    out.clear();
    if (!validRelayChildAssignmentCore(assignment)) return false;
    static constexpr char kAssignmentAttestationDomain[] =
        "qoh99-qsr-relay-child-assignment-v1";
    Writer writer;
    if (!writer.reserve(512u)
        || !writer.bytes(
            reinterpret_cast<const uint8_t*>(kAssignmentAttestationDomain),
            sizeof(kAssignmentAttestationDomain) - 1u)
        || !writer.u16(kResilientControlVersion)
        || !writeRelayChildAssignmentUnsigned(writer, assignment)) {
        return false;
    }
    out = std::move(writer.bytes());
    return !out.empty();
}

bool encodeRelayChildActivationAttestationInput(
    const RelayChildActivation& activation, std::vector<uint8_t>& out) {
    out.clear();
    if (!validRelayChildActivationCore(activation)) return false;
    static constexpr char kActivationAttestationDomain[] =
        "qoh99-qsr-relay-child-activation-v1";
    Writer writer;
    if (!writer.reserve(768u)
        || !writer.bytes(
            reinterpret_cast<const uint8_t*>(kActivationAttestationDomain),
            sizeof(kActivationAttestationDomain) - 1u)
        || !writer.u16(kResilientControlVersion)
        || !writeRelayChildActivationUnsigned(writer, activation)) {
        return false;
    }
    out = std::move(writer.bytes());
    return !out.empty();
}

bool encodeRelayChildActivationCommitAttestationInput(
    const RelayChildActivationCommit& commit, std::vector<uint8_t>& out) {
    out.clear();
    if (!validRelayChildActivation(commit.activation)) return false;
    static constexpr char kActivationCommitAttestationDomain[] =
        "qoh99-qsr-relay-child-activation-commit-v1";
    Writer writer;
    if (!writer.reserve(896u)
        || !writer.bytes(reinterpret_cast<const uint8_t*>(
                             kActivationCommitAttestationDomain),
                         sizeof(kActivationCommitAttestationDomain) - 1u)
        || !writer.u16(kResilientControlVersion)
        || !writeRelayChildActivation(writer, commit.activation)) {
        return false;
    }
    out = std::move(writer.bytes());
    return !out.empty();
}

bool makeRelayChildActivationReceipt(
    const RelayChildActivation& activation,
    const RelayChildActivationReceiptRole role,
    RelayChildActivationReceipt& out) {
    out = {};
    if (!validRelayChildActivationCore(activation)
        || !validRelayChildActivationReceiptRole(role)) {
        return false;
    }
    RelayChildActivationReceipt receipt{};
    receipt.stream = activation.stream;
    receipt.authorityGeneration = activation.authorityGeneration;
    receipt.rootNodeId = activation.rootNodeId;
    receipt.relayNodeId = activation.relayNodeId;
    receipt.childNodeId = activation.childNodeId;
    receipt.relayLease = activation.relayLease;
    receipt.childLease = leaseBindingFor(activation.childLease);
    receipt.relayBindingRequestNonce = activation.relayBindingRequestNonce;
    receipt.relayDataRoute = activation.relayDataRoute;
    receipt.relayControlRoute = activation.relayControlRoute;
    receipt.childDataRoute = activation.childDataRoute;
    receipt.childControlRoute = activation.childControlRoute;
    receipt.relayCacheRange = activation.relayCacheRange;
    receipt.commonCursor = activation.commonCursor;
    receipt.activationNonce = activation.activationNonce;
    receipt.activationDeadlineMs = activation.activationDeadlineMs;
    receipt.role = role;
    if (!validRelayChildActivationReceipt(receipt)) return false;
    out = receipt;
    return true;
}

bool encodeClientPlayerRootGrantAttestationInput(
    const ClientPlayerRootGrant& grant, std::vector<uint8_t>& out) {
    out.clear();
    if (!validClientPlayerRootGrantCore(grant)) return false;
    static constexpr char kClientPlayerRootGrantDomain[] =
        "qoh99-qsr-client-player-root-grant-v1";
    Writer writer;
    if (!writer.reserve(768u)
        || !writer.bytes(reinterpret_cast<const uint8_t*>(
                             kClientPlayerRootGrantDomain),
                         sizeof(kClientPlayerRootGrantDomain) - 1u)
        || !writer.u16(kResilientControlVersion)
        || !writeClientPlayerRootGrantUnsigned(writer, grant)) {
        return false;
    }
    out = std::move(writer.bytes());
    return !out.empty();
}

bool encodeClientPlayerRootActivationAttestationInput(
    const ClientPlayerRootActivation& activation,
    std::vector<uint8_t>& out) {
    out.clear();
    if (!validClientPlayerRootActivationCore(activation)) return false;
    static constexpr char kClientPlayerRootActivationDomain[] =
        "qoh99-qsr-client-player-root-activation-v1";
    Writer writer;
    if (!writer.reserve(1024u)
        || !writer.bytes(reinterpret_cast<const uint8_t*>(
                             kClientPlayerRootActivationDomain),
                         sizeof(kClientPlayerRootActivationDomain) - 1u)
        || !writer.u16(kResilientControlVersion)
        || !writeClientPlayerRootActivationUnsigned(writer, activation)) {
        return false;
    }
    out = std::move(writer.bytes());
    return !out.empty();
}

bool encodeClientPlayerRootActivationCommitAttestationInput(
    const ClientPlayerRootActivationCommit& commit,
    std::vector<uint8_t>& out) {
    out.clear();
    if (!validClientPlayerRootActivation(commit.activation)) return false;
    static constexpr char kClientPlayerRootActivationCommitDomain[] =
        "qoh99-qsr-client-player-root-activation-commit-v1";
    Writer writer;
    if (!writer.reserve(1152u)
        || !writer.bytes(reinterpret_cast<const uint8_t*>(
                             kClientPlayerRootActivationCommitDomain),
                         sizeof(kClientPlayerRootActivationCommitDomain) - 1u)
        || !writer.u16(kResilientControlVersion)
        || !writeClientPlayerRootActivation(writer, commit.activation)) {
        return false;
    }
    out = std::move(writer.bytes());
    return !out.empty();
}

bool encodeClientPlayerRootActivationRetirementAttestationInput(
    const ClientPlayerRootActivationRetirement& retirement,
    std::vector<uint8_t>& out) {
    out.clear();
    if (!validClientPlayerRootActivation(retirement.activation)
        || !validClientPlayerRootRetirementReason(retirement.reason)) {
        return false;
    }
    static constexpr char kClientPlayerRootActivationRetirementDomain[] =
        "qoh99-qsr-client-player-root-activation-retirement-v1";
    Writer writer;
    if (!writer.reserve(1152u)
        || !writer.bytes(reinterpret_cast<const uint8_t*>(
                             kClientPlayerRootActivationRetirementDomain),
                         sizeof(kClientPlayerRootActivationRetirementDomain)
                             - 1u)
        || !writer.u16(kResilientControlVersion)
        || !writeClientPlayerRootActivation(writer, retirement.activation)
        || !writer.u8(static_cast<uint8_t>(retirement.reason))) {
        return false;
    }
    out = std::move(writer.bytes());
    return !out.empty();
}

bool validClientPlayerRootActivationCommit(
    const ClientPlayerRootActivationCommit& commit) {
    return validClientPlayerRootActivationCommitImpl(commit);
}

bool validClientPlayerRootActivationRetirement(
    const ClientPlayerRootActivationRetirement& retirement) {
    return validClientPlayerRootActivationRetirementImpl(retirement);
}

bool makeClientPlayerRootActivationReceipt(
    const ClientPlayerRootActivation& activation,
    ClientPlayerRootActivationReceipt& out) {
    out = {};
    if (!validClientPlayerRootActivation(activation)) return false;
    out.activation = activation;
    return true;
}

bool hasResilientControlMagic(const uint8_t* const bytes,
                              const size_t length) {
    return bytes != nullptr && length >= sizeof(uint32_t)
        && readLe32(bytes) == kResilientControlMagic;
}

bool encodeControlDatagram(const ControlDatagram& datagram,
                           std::vector<uint8_t>& out) {
    out.clear();
    if (!validControlMessageType(datagram.type)) return false;

    Writer payload;
    if (!payload.reserve(kMaxResilientControlDatagramBytes
                         - kResilientControlHeaderBytes)
        || !writePayload(payload, datagram)
        || payload.bytes().size()
            > kMaxResilientControlDatagramBytes
                - kResilientControlHeaderBytes
        || payload.bytes().size() > (std::numeric_limits<uint16_t>::max)()) {
        return false;
    }

    try {
        out.assign(kResilientControlHeaderBytes + payload.bytes().size(), 0u);
    } catch (const std::bad_alloc&) {
        out.clear();
        return false;
    }
    writeLe32(out.data(), kResilientControlMagic);
    writeLe16(out.data() + 4u, kResilientControlVersion);
    out[6] = static_cast<uint8_t>(datagram.type);
    out[7] = 0u;
    writeLe16(out.data() + 8u,
              static_cast<uint16_t>(payload.bytes().size()));
    writeLe16(out.data() + 10u, 0u);
    // Bytes [12,16) are zero while calculating the packet CRC32C.
    std::copy(payload.bytes().begin(), payload.bytes().end(),
              out.begin() + static_cast<std::ptrdiff_t>(
                  kResilientControlHeaderBytes));
    writeLe32(out.data() + 12u, qreplay::crc32c(out.data(), out.size()));
    return true;
}

DecodeResult decodeControlDatagram(const uint8_t* const bytes,
                                   const size_t length,
                                   ControlDatagram& out) {
    out = {};
    if (bytes == nullptr) return DecodeResult::NullInput;
    if (length < kResilientControlHeaderBytes) return DecodeResult::TooSmall;
    if (length > kMaxResilientControlDatagramBytes)
        return DecodeResult::TooLarge;
    if (readLe32(bytes) != kResilientControlMagic)
        return DecodeResult::WrongMagic;
    if (readLe16(bytes + 4u) != kResilientControlVersion)
        return DecodeResult::UnsupportedVersion;
    const uint8_t rawType = bytes[6];
    if (rawType < static_cast<uint8_t>(ControlMessageType::NodeAdvertisement)
        || rawType > static_cast<uint8_t>(
            ControlMessageType::ClientPlayerRootActivationRetirement)) {
        return DecodeResult::UnknownMessageType;
    }
    if (bytes[7] != 0u || readLe16(bytes + 10u) != 0u)
        return DecodeResult::HeaderReservedNonZero;
    const size_t payloadLength = readLe16(bytes + 8u);
    if (payloadLength != length - kResilientControlHeaderBytes)
        return DecodeResult::LengthMismatch;
    const uint32_t expectedChecksum = readLe32(bytes + 12u);
    std::array<uint8_t, kResilientControlHeaderBytes> header{};
    std::copy_n(bytes, header.size(), header.data());
    writeLe32(header.data() + 12u, 0u);
    uint32_t actualChecksum = qreplay::crc32c(header.data(), header.size());
    actualChecksum = qreplay::crc32c(
        bytes + kResilientControlHeaderBytes, payloadLength, actualChecksum);
    if (actualChecksum != expectedChecksum) return DecodeResult::ChecksumMismatch;

    ControlDatagram decoded{};
    decoded.type = static_cast<ControlMessageType>(rawType);
    Reader reader(bytes + kResilientControlHeaderBytes, payloadLength);
    if (!readPayload(reader, decoded) || reader.remaining() != 0u)
        return DecodeResult::MalformedPayload;
    out = std::move(decoded);
    return DecodeResult::Ok;
}

const char* decodeResultName(const DecodeResult result) {
    switch (result) {
    case DecodeResult::Ok: return "ok";
    case DecodeResult::NullInput: return "null-input";
    case DecodeResult::TooSmall: return "too-small";
    case DecodeResult::TooLarge: return "too-large";
    case DecodeResult::WrongMagic: return "wrong-magic";
    case DecodeResult::UnsupportedVersion: return "unsupported-version";
    case DecodeResult::UnknownMessageType: return "unknown-message-type";
    case DecodeResult::HeaderReservedNonZero: return "reserved-header";
    case DecodeResult::LengthMismatch: return "length-mismatch";
    case DecodeResult::ChecksumMismatch: return "checksum-mismatch";
    case DecodeResult::MalformedPayload: return "malformed-payload";
    }
    return "unknown";
}

}
