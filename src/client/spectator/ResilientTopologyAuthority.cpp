#include "client/spectator/ResilientTopologyAuthority.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "net/SecureRandom.h"
#include "net/Sockets.h"
#include "spectator/SpectatorGrant.h"
#include "spectator/SpectatorRelayAdapter.h"
#include "spectator/TopologyGrant.h"

namespace qclient::spectator::topology {
namespace {

constexpr char kTopologyCredentialDomain[] = "qoh99-topology-qsr-v1";
// This transcript deliberately omits SessionIdentity.cookie: no player/session secret may enter topology state.
constexpr char kClientPlayerRootCredentialDomain[] =
    "qoh99-topology-client-player-root-qta-v1";
constexpr uint8_t kTopologyControlType =
    wire::kTopologyQtaEnvelopeControlType;
constexpr size_t kTopologyEnvelopePrefixBytes = 4u + 2u + 1u + 1u + 2u
    + 2u + 8u + 8u + 8u + kTopologyMacBytes;
static_assert(kTopologyEnvelopePrefixBytes
                  == wire::kTopologyQtaEnvelopePrefixBytes,
              "the QTA envelope prefix must match the canonical wire shape");
constexpr size_t kMaxTopologyControlBytes =
    kTopologyEnvelopePrefixBytes
    + wire::kMaxResilientControlDatagramBytes;
constexpr uint64_t kTopologyAuthorityLivenessIntervalMs = 1000u;
constexpr uint32_t kTopologyAuthorityLivenessWindowMs = 5000u;

bool equalConstantTime(const uint8_t* left, const uint8_t* right,
                       const size_t count) {
    if (left == nullptr || right == nullptr) return false;
    uint8_t difference = 0u;
    for (size_t index = 0; index < count; ++index)
        difference |= static_cast<uint8_t>(left[index] ^ right[index]);
    return difference == 0u;
}

bool allZero(const uint8_t* bytes, const size_t count) {
    if (bytes == nullptr) return true;
    uint8_t value = 0u;
    for (size_t index = 0; index < count; ++index) value |= bytes[index];
    return value == 0u;
}

void wipeVector(std::vector<uint8_t>& bytes) {
    std::fill(bytes.begin(), bytes.end(), uint8_t{0u});
    bytes.clear();
}

void appendU8(std::vector<uint8_t>& out, const uint8_t value) {
    out.push_back(value);
}

void appendU16(std::vector<uint8_t>& out, const uint16_t value) {
    appendU8(out, static_cast<uint8_t>(value));
    appendU8(out, static_cast<uint8_t>(value >> 8u));
}

void appendU32(std::vector<uint8_t>& out, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        appendU8(out, static_cast<uint8_t>(value >> shift));
}

void appendU64(std::vector<uint8_t>& out, const uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        appendU8(out, static_cast<uint8_t>(value >> shift));
}

bool appendBytes(std::vector<uint8_t>& out, const uint8_t* bytes,
                 const size_t count) {
    if (bytes == nullptr && count != 0u) return false;
    try {
        out.insert(out.end(), bytes, bytes + count);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void writeLe16(uint8_t* out, const uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8u);
}

void writeLe32(uint8_t* out, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        out[shift / 8u] = static_cast<uint8_t>(value >> shift);
}

void writeLe64(uint8_t* out, const uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        out[shift / 8u] = static_cast<uint8_t>(value >> shift);
}

uint16_t readLe16(const uint8_t* in) {
    return static_cast<uint16_t>(in[0])
        | (static_cast<uint16_t>(in[1]) << 8u);
}

uint32_t readLe32(const uint8_t* in) {
    uint32_t value = 0u;
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        value |= static_cast<uint32_t>(in[shift / 8u]) << shift;
    return value;
}

uint64_t readLe64(const uint8_t* in) {
    uint64_t value = 0u;
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        value |= static_cast<uint64_t>(in[shift / 8u]) << shift;
    return value;
}

bool appendCompatibility(
    std::vector<uint8_t>& out,
    const qnetplay::compat::CompatibilityIdentityWire& identity) {
    using namespace qnetplay::compat;
    appendU32(out, identity.schemaVersion);
    appendU32(out, identity.protocolVersion);
    return appendBytes(out, reinterpret_cast<const uint8_t*>(
                               identity.runtimeBuild.profileId),
                       kCompatibilityProfileIdBytes)
        && appendBytes(out, identity.runtimeBuild.executableSha256,
                       kCompatibilitySha256Bytes)
        && (appendU64(out, identity.runtimeBuild.executableSize), true)
        && (appendU32(out, identity.runtimeBuild.peTimestamp), true)
        && (appendU32(out, identity.simulationContent.manifestSchemaVersion),
            true)
        && (appendU32(out, identity.simulationContent.manifestFlags), true)
        && appendBytes(out, identity.simulationContent.simulationRootSha256,
                       kCompatibilitySha256Bytes)
        && appendBytes(out, identity.simulationContent.validationRootSha256,
                       kCompatibilitySha256Bytes)
        && (appendU32(out, identity.simulationContent.entryCount), true)
        && (appendU32(out, identity.simulationContent.requiredEntryCount),
            true)
        && (appendU64(out, identity.stateSchemaId), true)
        && (appendU64(out, identity.modSimulationId), true)
        && (appendU64(out, identity.timingProfileId), true)
        && (appendU32(out, identity.reserved), true);
}

bool appendConfigurationProvenance(
    std::vector<uint8_t>& out,
    const qnetplay::config_provenance::ConfigurationProvenanceWire&
        provenance) {
    appendU16(out, provenance.schemaVersion);
    appendU8(out, provenance.localDelayOrigin);
    appendU8(out, provenance.localRollbackOrigin);
    appendU8(out, provenance.peerDelayOrigin);
    appendU8(out, provenance.peerRollbackOrigin);
    appendU8(out, provenance.peerRuntimeAttestation);
    return appendBytes(out, provenance.reserved, sizeof(provenance.reserved));
}

void appendRoute(std::vector<uint8_t>& out,
                 const model::RouteDescriptor& route) {
    appendU8(out, static_cast<uint8_t>(route.kind));
    appendU32(out, route.ipv4BE);
    appendU16(out, route.port);
    appendU64(out, route.generation);
    appendU8(out, route.routeProved ? 1u : 0u);
}

void appendEndpointBinding(std::vector<uint8_t>& out,
                           const EndpointBinding& binding) {
    appendU32(out, binding.local.ipv4BE);
    appendU16(out, binding.local.port);
    appendRoute(out, binding.advertised);
    appendU8(out, static_cast<uint8_t>(binding.localProvenance));
    appendU8(out, static_cast<uint8_t>(binding.advertisedProvenance));
    appendU64(out, binding.generation);
}

bool appendClientPlayerRootBinding(
    std::vector<uint8_t>& out,
    const wire::ClientPlayerRootBinding& binding) {
    appendU64(out, binding.stream.setOrSessionId);
    appendU64(out, binding.stream.gameEpoch);
    if (!appendCompatibility(out, binding.stream.compatibility)) return false;
    appendU64(out, binding.stream.streamRootIdentity);
    appendU64(out, binding.gameStartFingerprint);
    appendU64(out, binding.clientRootNodeId);
    appendU8(out, static_cast<uint8_t>(binding.remotePlayerRole));
    if (!appendConfigurationProvenance(out,
                                       binding.configurationProvenance)) {
        return false;
    }
    appendU32(out, binding.runtimeReceipt.processId);
    appendU32(out, binding.runtimeReceipt.publisherGeneration);
    appendU64(out, binding.runtimeReceipt.publisherNonce);
    appendU8(out, binding.runtimeReceipt.playerSlot);
    appendU8(out, static_cast<uint8_t>(binding.runtimeReceipt.runtimeStage));
    appendU8(out, binding.runtimeReceipt.delay);
    appendU8(out, binding.runtimeReceipt.rollback);
    appendU8(out, binding.runtimeReceipt.remoteDelay);
    appendU8(out, binding.runtimeReceipt.remoteRollback);
    if (!appendConfigurationProvenance(out,
                                       binding.runtimeReceipt.provenance)) {
        return false;
    }
    appendU64(out, binding.sourceGeneration);
    appendU64(out, binding.qspRouteGeneration);
    appendU64(out, binding.qsrBindingGeneration);
    appendRoute(out, binding.dataRoute);
    appendRoute(out, binding.qsrRoute);
    appendU32(out, binding.cacheRange.firstCursor);
    appendU32(out, binding.cacheRange.nextCursor);
    appendU32(out, binding.cursor.nextRecord);
    appendU32(out, binding.cursor.nextEvent);
    appendU64(out, binding.cursor.digest);
    appendU32(out, binding.childCapacity);
    appendU64(out, binding.expiresMs);
    return true;
}

bool validProvenance(const EndpointProvenance provenance) {
    return provenance == EndpointProvenance::LocalBind
        || provenance == EndpointProvenance::PublishedDirectCandidate
        || provenance == EndpointProvenance::ProvenDirectQsp
        || provenance == EndpointProvenance::ProvenPunchedQsp;
}

bool validRouteProvenance(const EndpointProvenance provenance) {
    return provenance == EndpointProvenance::ProvenDirectQsp
        || provenance == EndpointProvenance::ProvenPunchedQsp
        || provenance == EndpointProvenance::ProvenRelayQsp;
}

bool sameCompatibility(
    const qnetplay::compat::CompatibilityIdentityWire& left,
    const qnetplay::compat::CompatibilityIdentityWire& right) {
    return qnetplay::compat::semanticallyEqual(left, right);
}

bool sameFullCompatibility(
    const qnetplay::compat::CompatibilityIdentityWire& left,
    const qnetplay::compat::CompatibilityIdentityWire& right) {
    using namespace qnetplay::compat;
    return left.schemaVersion == right.schemaVersion
        && left.protocolVersion == right.protocolVersion
        && std::memcmp(left.runtimeBuild.profileId, right.runtimeBuild.profileId,
                       kCompatibilityProfileIdBytes) == 0
        && std::memcmp(left.runtimeBuild.executableSha256,
                       right.runtimeBuild.executableSha256,
                       kCompatibilitySha256Bytes) == 0
        && left.runtimeBuild.executableSize == right.runtimeBuild.executableSize
        && left.runtimeBuild.peTimestamp == right.runtimeBuild.peTimestamp
        && left.simulationContent.manifestSchemaVersion
            == right.simulationContent.manifestSchemaVersion
        && left.simulationContent.manifestFlags
            == right.simulationContent.manifestFlags
        && std::memcmp(left.simulationContent.simulationRootSha256,
                       right.simulationContent.simulationRootSha256,
                       kCompatibilitySha256Bytes) == 0
        && std::memcmp(left.simulationContent.validationRootSha256,
                       right.simulationContent.validationRootSha256,
                       kCompatibilitySha256Bytes) == 0
        && left.simulationContent.entryCount
            == right.simulationContent.entryCount
        && left.simulationContent.requiredEntryCount
            == right.simulationContent.requiredEntryCount
        && left.stateSchemaId == right.stateSchemaId
        && left.modSimulationId == right.modSimulationId
        && left.timingProfileId == right.timingProfileId
        && left.reserved == right.reserved;
}

bool sameConfigurationProvenance(
    const qnetplay::config_provenance::ConfigurationProvenanceWire& left,
    const qnetplay::config_provenance::ConfigurationProvenanceWire& right) {
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

bool sameFullStream(const model::StreamCompatibilityIdentity& left,
                    const model::StreamCompatibilityIdentity& right) {
    return left.setOrSessionId == right.setOrSessionId
        && left.gameEpoch == right.gameEpoch
        && sameFullCompatibility(left.compatibility, right.compatibility)
        && left.streamRootIdentity == right.streamRootIdentity;
}

bool sameRoute(const model::RouteDescriptor& left,
               const model::RouteDescriptor& right) {
    return left.kind == right.kind && left.ipv4BE == right.ipv4BE
        && left.port == right.port && left.generation == right.generation
        && left.routeProved == right.routeProved;
}

bool sameCacheRange(const model::VerifiedCacheRange& left,
                    const model::VerifiedCacheRange& right) {
    return left.firstCursor == right.firstCursor
        && left.nextCursor == right.nextCursor;
}

bool sameCursor(const model::VerifiedChainCursor& left,
                const model::VerifiedChainCursor& right) {
    return left.nextRecord == right.nextRecord
        && left.nextEvent == right.nextEvent && left.digest == right.digest;
}

bool validClientPlayerRootRoute(const model::RouteDescriptor& route) {
    return route.valid() && route.routeProved
        && (route.kind == model::RouteKind::Direct
            || route.kind == model::RouteKind::Punched);
}

RouteEndpoint endpointFromRoute(const model::RouteDescriptor& route) {
    return RouteEndpoint{route.ipv4BE, route.port};
}

bool sameAncestry(
    const std::array<model::NodeId, model::kMaxLeaseAncestry>& left,
    const uint8_t leftCount,
    const std::array<model::NodeId, model::kMaxLeaseAncestry>& right,
    const uint8_t rightCount) {
    return leftCount == rightCount
        && leftCount <= left.size()
        && rightCount <= right.size()
        && std::equal(left.begin(), left.begin() + leftCount, right.begin());
}

bool sameClientDataRootDescriptor(
    const ClientDataRootDescriptor& left,
    const ClientDataRootDescriptor& right) {
    return left.registered == right.registered
        && left.nodeId == right.nodeId && left.stream == right.stream
        && left.gameStartFingerprint == right.gameStartFingerprint
        && left.authorityGeneration == right.authorityGeneration
        && left.qspRouteGeneration == right.qspRouteGeneration
        && left.qsrBindingGeneration == right.qsrBindingGeneration
        && left.sourceGeneration == right.sourceGeneration
        && sameRoute(left.dataRoute, right.dataRoute)
        && sameRoute(left.qsrRoute, right.qsrRoute)
        && sameCacheRange(left.cacheRange, right.cacheRange)
        && sameCursor(left.cursor, right.cursor)
        && left.childCapacity == right.childCapacity
        && left.expiresMs == right.expiresMs;
}

bool sameClientPlayerRootRuntimeReceipt(
    const wire::ClientPlayerRootRuntimeReceipt& left,
    const wire::ClientPlayerRootRuntimeReceipt& right) {
    return left.processId == right.processId
        && left.publisherGeneration == right.publisherGeneration
        && left.publisherNonce == right.publisherNonce
        && left.playerSlot == right.playerSlot
        && left.runtimeStage == right.runtimeStage
        && left.delay == right.delay && left.rollback == right.rollback
        && left.remoteDelay == right.remoteDelay
        && left.remoteRollback == right.remoteRollback
        && sameConfigurationProvenance(left.provenance, right.provenance);
}

bool sameClientPlayerRootBinding(const wire::ClientPlayerRootBinding& left,
                                 const wire::ClientPlayerRootBinding& right) {
    return sameFullStream(left.stream, right.stream)
        && left.gameStartFingerprint == right.gameStartFingerprint
        && left.clientRootNodeId == right.clientRootNodeId
        && left.remotePlayerRole == right.remotePlayerRole
        && sameConfigurationProvenance(left.configurationProvenance,
                                       right.configurationProvenance)
        && sameClientPlayerRootRuntimeReceipt(left.runtimeReceipt,
                                              right.runtimeReceipt)
        && left.sourceGeneration == right.sourceGeneration
        && left.qspRouteGeneration == right.qspRouteGeneration
        && left.qsrBindingGeneration == right.qsrBindingGeneration
        && sameRoute(left.dataRoute, right.dataRoute)
        && sameRoute(left.qsrRoute, right.qsrRoute)
        && sameCacheRange(left.cacheRange, right.cacheRange)
        && sameCursor(left.cursor, right.cursor)
        && left.childCapacity == right.childCapacity
        && left.expiresMs == right.expiresMs;
}

bool sameParentLease(const model::ParentLease& left,
                     const model::ParentLease& right) {
    if (!sameFullStream(left.stream, right.stream)
        || left.childId != right.childId || left.parentId != right.parentId
        || !(left.childBinding == right.childBinding)
        || left.parentDepth != right.parentDepth
        || left.parentSourceGeneration != right.parentSourceGeneration
        || left.routeGeneration != right.routeGeneration
        || left.routeCount != right.routeCount
        || !sameCacheRange(left.cacheRange, right.cacheRange)
        || left.latestVerifiedCursor != right.latestVerifiedCursor
        || left.childCapacity != right.childCapacity
        || left.expiryMs != right.expiryMs
        || left.replayNonce != right.replayNonce
        || left.ancestryCount != right.ancestryCount
        || left.macLow != right.macLow || left.macHigh != right.macHigh) {
        return false;
    }
    for (size_t index = 0u; index < left.routes.size(); ++index)
        if (!sameRoute(left.routes[index], right.routes[index])) return false;
    return left.ancestry == right.ancestry;
}

bool sameClientPlayerRootActivation(
    const wire::ClientPlayerRootActivation& left,
    const wire::ClientPlayerRootActivation& right) {
    return left.authorityGeneration == right.authorityGeneration
        && left.hostRootNodeId == right.hostRootNodeId
        && sameClientPlayerRootBinding(left.binding, right.binding)
        && left.targetViewerNodeId == right.targetViewerNodeId
        && sameParentLease(left.targetLease, right.targetLease)
        && sameRoute(left.targetDataRoute, right.targetDataRoute)
        && sameRoute(left.targetQsrRoute, right.targetQsrRoute)
        && left.activationNonce == right.activationNonce
        && left.activationDeadlineMs == right.activationDeadlineMs
        && left.hostSignature == right.hostSignature;
}

bool sameClientPlayerRootActivationCommit(
    const wire::ClientPlayerRootActivationCommit& left,
    const wire::ClientPlayerRootActivationCommit& right) {
    return sameClientPlayerRootActivation(left.activation, right.activation)
        && left.hostSignature == right.hostSignature;
}

bool sameClientPlayerRootActivationRetirement(
    const wire::ClientPlayerRootActivationRetirement& left,
    const wire::ClientPlayerRootActivationRetirement& right) {
    return sameClientPlayerRootActivation(left.activation, right.activation)
        && left.reason == right.reason && left.hostSignature == right.hostSignature;
}

bool sameEndpoint(const RouteEndpoint& left, const RouteEndpoint& right) {
    return left == right;
}

model::LeaseBinding leaseBindingOf(const model::ParentLease& lease) {
    model::LeaseBinding binding{};
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

bool sameLeaseBinding(const model::LeaseBinding& left,
                      const model::LeaseBinding& right) {
    return left.childId == right.childId && left.parentId == right.parentId
        && left.parentSourceGeneration == right.parentSourceGeneration
        && left.routeGeneration == right.routeGeneration
        && left.expiryMs == right.expiryMs
        && left.replayNonce == right.replayNonce
        && left.macLow == right.macLow && left.macHigh == right.macHigh
        && left.childBinding == right.childBinding;
}

bool sameRelayChildRetirement(
    const wire::RelayChildRetirement& left,
    const wire::RelayChildRetirement& right) {
    return left.stream == right.stream
        && left.authorityGeneration == right.authorityGeneration
        && left.rootNodeId == right.rootNodeId
        && left.relayNodeId == right.relayNodeId
        && left.childNodeId == right.childNodeId
        && sameLeaseBinding(left.relayLease, right.relayLease)
        && left.bindingRequestNonce == right.bindingRequestNonce
        && sameLeaseBinding(left.childLease, right.childLease)
        && left.reason == right.reason;
}

bool sameRelayChildActivation(
    const wire::RelayChildActivation& left,
    const wire::RelayChildActivation& right) {
    return left.stream == right.stream
        && left.authorityGeneration == right.authorityGeneration
        && left.rootNodeId == right.rootNodeId
        && left.relayNodeId == right.relayNodeId
        && left.childNodeId == right.childNodeId
        && sameLeaseBinding(left.relayLease, right.relayLease)
        && left.relayBindingRequestNonce == right.relayBindingRequestNonce
        && sameRoute(left.relayDataRoute, right.relayDataRoute)
        && sameRoute(left.relayControlRoute, right.relayControlRoute)
        && sameLeaseBinding(leaseBindingOf(left.childLease),
                            leaseBindingOf(right.childLease))
        && sameRoute(left.childDataRoute, right.childDataRoute)
        && sameRoute(left.childControlRoute, right.childControlRoute)
        && sameCacheRange(left.relayCacheRange, right.relayCacheRange)
        && sameCursor(left.commonCursor, right.commonCursor)
        && left.activationNonce == right.activationNonce
        && left.activationDeadlineMs == right.activationDeadlineMs
        && left.hostSignature == right.hostSignature;
}

bool sameRelayChildActivationCommit(
    const wire::RelayChildActivationCommit& left,
    const wire::RelayChildActivationCommit& right) {
    return sameRelayChildActivation(left.activation, right.activation)
        && left.hostSignature == right.hostSignature;
}

bool receiptMatchesActivation(
    const wire::RelayChildActivationReceipt& receipt,
    const wire::RelayChildActivation& activation) {
    return receipt.stream == activation.stream
        && receipt.authorityGeneration == activation.authorityGeneration
        && receipt.rootNodeId == activation.rootNodeId
        && receipt.relayNodeId == activation.relayNodeId
        && receipt.childNodeId == activation.childNodeId
        && sameLeaseBinding(receipt.relayLease, activation.relayLease)
        && sameLeaseBinding(receipt.childLease,
                            leaseBindingOf(activation.childLease))
        && receipt.relayBindingRequestNonce
            == activation.relayBindingRequestNonce
        && sameRoute(receipt.relayDataRoute, activation.relayDataRoute)
        && sameRoute(receipt.relayControlRoute, activation.relayControlRoute)
        && sameRoute(receipt.childDataRoute, activation.childDataRoute)
        && sameRoute(receipt.childControlRoute, activation.childControlRoute)
        && sameCacheRange(receipt.relayCacheRange,
                          activation.relayCacheRange)
        && sameCursor(receipt.commonCursor, activation.commonCursor)
        && receipt.activationNonce == activation.activationNonce
        && receipt.activationDeadlineMs == activation.activationDeadlineMs;
}

bool sameRelayChildActivationRetirement(
    const wire::RelayChildActivationRetirement& left,
    const wire::RelayChildActivationRetirement& right) {
    return left.stream == right.stream
        && left.authorityGeneration == right.authorityGeneration
        && left.rootNodeId == right.rootNodeId
        && left.relayNodeId == right.relayNodeId
        && left.childNodeId == right.childNodeId
        && sameLeaseBinding(left.relayLease, right.relayLease)
        && sameLeaseBinding(left.childLease, right.childLease)
        && left.relayBindingRequestNonce == right.relayBindingRequestNonce
        && sameRoute(left.relayDataRoute, right.relayDataRoute)
        && sameRoute(left.relayControlRoute, right.relayControlRoute)
        && sameRoute(left.childDataRoute, right.childDataRoute)
        && sameRoute(left.childControlRoute, right.childControlRoute)
        && sameCacheRange(left.relayCacheRange, right.relayCacheRange)
        && sameCursor(left.commonCursor, right.commonCursor)
        && left.activationNonce == right.activationNonce
        && left.activationDeadlineMs == right.activationDeadlineMs
        && left.reason == right.reason;
}

bool validEnvelopeHeader(const uint8_t* bytes, const size_t length,
                         size_t& qsrLength) {
    if (bytes == nullptr || length < kTopologyEnvelopePrefixBytes
        || length > kMaxTopologyControlBytes
        || readLe32(bytes) != kTopologyControlMagic
        || readLe16(bytes + 4u) != kTopologyControlVersion
        || bytes[6] != kTopologyControlType || bytes[7] != 0u
        || readLe16(bytes + 10u) != 0u) {
        return false;
    }
    qsrLength = readLe16(bytes + 8u);
    return qsrLength > 0u
        && qsrLength <= wire::kMaxResilientControlDatagramBytes
        && qsrLength + kTopologyEnvelopePrefixBytes == length;
}

uint64_t nextNonzero(const uint64_t now, const uint64_t span) {
    if (now > (std::numeric_limits<uint64_t>::max)() - span)
        return (std::numeric_limits<uint64_t>::max)();
    return now + span;
}

}

bool hasTopologyControlEnvelopeMagic(const uint8_t* bytes,
                                     const size_t length) {
    return bytes != nullptr && length >= sizeof(uint32_t)
        && readLe32(bytes) == kTopologyControlMagic;
}

bool reserveTopologyControlSequence(uint64_t& next, uint64_t& reserved) {
    reserved = 0u;
    if (next == 0u) return false;
    reserved = next;
    if (next == (std::numeric_limits<uint64_t>::max)()) {
        next = 0u;
    } else {
        ++next;
    }
    return true;
}

bool EndpointBinding::validForAuthority() const {
    return local.port != 0u && advertised.valid()
        && advertised.generation == generation
        && validProvenance(localProvenance)
        && validProvenance(advertisedProvenance)
        && advertised.kind != model::RouteKind::TrafficRelay;
}

bool TopologyAuthorityFacts::complete() const {
    const bool signingKeyPresent = std::any_of(
        assignmentSigningPublicKey.begin(), assignmentSigningPublicKey.end(),
        [](const uint8_t byte) { return byte != 0u; });
    return qnetplay::spectator::isValidIdentity(session)
        && qnetplay::compat::isValidCompatibilityIdentity(compatibility)
        && streamRootIdentity != 0u
        && gameStartFingerprint != 0u
        && gameStartFingerprint == streamRootIdentity
        && sourceGeneration != 0u
        && authorityGeneration != 0u && rootNodeId != model::kInvalidNodeId
        && rootQsp.validForAuthority() && rootQsr.validForAuthority()
        && rootQsp.local.port != rootQsr.local.port
        && rootQsp.advertised.port != rootQsr.advertised.port
        && limits.valid() && signingKeyPresent && createdMs != 0u
        && expiresMs > createdMs
        && !revoked;
}

const char* topologyControlResultName(const TopologyControlResult result) {
    switch (result) {
    case TopologyControlResult::Accepted: return "accepted";
    case TopologyControlResult::NotReady: return "not-ready";
    case TopologyControlResult::Malformed: return "malformed";
    case TopologyControlResult::UnknownNode: return "unknown-node";
    case TopologyControlResult::AuthorityMismatch: return "authority-mismatch";
    case TopologyControlResult::Expired: return "expired";
    case TopologyControlResult::EndpointMismatch: return "endpoint-mismatch";
    case TopologyControlResult::Replay: return "replay";
    case TopologyControlResult::AuthenticationRejected:
        return "authentication-rejected";
    case TopologyControlResult::BindingRejected: return "binding-rejected";
    }
    return "unknown";
}

TopologyAuthority::~TopologyAuthority() {
    revoke();
}

bool TopologyAuthority::drawNonzero(uint64_t& out) {
    out = 0u;
    for (unsigned attempt = 0u; attempt < 8u; ++attempt) {
        bool degraded = false;
        const uint64_t value = qnet::secureRandom64(degraded);
        if (degraded || value == 0u) return false;
        out = value;
        return true;
    }
    return false;
}

bool TopologyAuthority::drawCredential(
    std::array<uint8_t, kTopologyCredentialBytes>& out) {
    out.fill(0u);
    bool degraded = false;
    if (!qnet::secureRandomBytes(out.data(), out.size(), degraded)
        || degraded || allZero(out.data(), out.size())) {
        out.fill(0u);
        return false;
    }
    return true;
}

bool TopologyAuthority::nodeIdInUse(const model::NodeId nodeId) const {
    if (nodeId == model::kInvalidNodeId || nodeId == _facts.rootNodeId)
        return true;
    const auto active = std::find_if(
        _enrollments.begin(), _enrollments.end(), [nodeId](const auto& item) {
            return item.nodeId == nodeId;
        });
    if (active != _enrollments.end()) return true;
    const auto clientRoot = std::find_if(
        _clientPlayerRoots.begin(), _clientPlayerRoots.end(),
        [nodeId](const ClientPlayerRootRecord& item) {
            return item.binding.clientRootNodeId == nodeId;
        });
    if (clientRoot != _clientPlayerRoots.end()) return true;
    return std::find(_retiredNodeIds.begin(), _retiredNodeIds.end(), nodeId)
        != _retiredNodeIds.end();
}

bool TopologyAuthority::initialize(
    const qnetplay::spectator::SessionIdentity& session,
    const qnetplay::compat::CompatibilityIdentityWire& compatibility,
    const uint64_t streamRootIdentity,
    const TopologyActivationRequest& request,
    const EndpointBinding& qsrBinding, const uint64_t nowMs,
    std::string& error) {
    revoke();
    error.clear();
    if (!request.requested || !qnetplay::spectator::isValidIdentity(session)
        || !qnetplay::compat::isValidCompatibilityIdentity(compatibility)
        || streamRootIdentity == 0u || request.sourceGeneration == 0u
        || !request.rootQsp.validForAuthority()
        || !request.rootQsp.advertised.routeProved
        || !qsrBinding.validForAuthority() || !request.limits.valid()
        || request.rootQsp.advertised.kind != model::RouteKind::Direct
        || qsrBinding.advertised.kind != model::RouteKind::Direct
        || nowMs == 0u) {
        error = "topology authority facts were incomplete";
        return false;
    }

    uint64_t authorityGeneration = 0u;
    uint64_t rootNodeId = 0u;
    if (!drawNonzero(authorityGeneration) || !drawNonzero(rootNodeId)
        || !drawCredential(_leaseKey) || !_assignmentSigner.generate()
        || !_assignmentSigner.publicBlob(_facts.assignmentSigningPublicKey)) {
        revoke();
        error = "the secure random provider could not initialize topology authority";
        return false;
    }

    _facts.session = session;
    _facts.compatibility = compatibility;
    _facts.streamRootIdentity = streamRootIdentity;
    _facts.gameStartFingerprint = streamRootIdentity;
    _facts.sourceGeneration = request.sourceGeneration;
    _facts.authorityGeneration = authorityGeneration;
    _facts.rootNodeId = rootNodeId;
    _facts.rootQsp = request.rootQsp;
    _facts.rootQsr = qsrBinding;
    _facts.limits = request.limits;
    _facts.createdMs = nowMs;
    _facts.expiresMs = nextNonzero(nowMs, 6ull * 60ull * 60ull * 1000ull);
    _facts.revoked = false;
    if (!_facts.complete()) {
        revoke();
        error = "topology authority rejected incomplete generated facts";
        return false;
    }
    return true;
}

void TopologyAuthority::revoke() {
    for (EnrollmentRecord& enrollment : _enrollments) {
        enrollment.credential.fill(0u);
        enrollment.attached = false;
        enrollment.attachmentOwner = AttachmentOwner::None;
        enrollment.attachedLease = {};
        enrollment.attachedDepth = 0u;
        enrollment.attachedAncestry = {};
        enrollment.attachedAncestryCount = 0u;
        enrollment.relayDataEndpoint = {};
        enrollment.relayDataBound = false;
        enrollment.relayDataRequestNonce = 0u;
        enrollment.relayDataBindingGeneration = 0u;
    }
    _enrollments.clear();
    _retiredNodeIds.clear();
    _offers.clear();
    _relayAssignments.clear();
    _relayActivations.clear();
    _relayActivationStages.clear();
    _clientDataRoots.clear();
    for (ClientPlayerRootRecord& root : _clientPlayerRoots)
        root.credential.fill(0u);
    _clientPlayerRoots.clear();
    _clientPlayerRootActivations.clear();
    for (PendingRelayChildRetirement& pending : _pendingRelayChildRetirements)
        pending.qtaRecord.credential.fill(0u);
    _pendingRelayChildRetirements.clear();
    for (PendingRelayChildActivationRetirement& pending
         : _pendingRelayChildActivationRetirements) {
        pending.qtaRecord.credential.fill(0u);
    }
    _pendingRelayChildActivationRetirements.clear();
    for (PendingRelayChildActivationCommit& pending
         : _pendingRelayChildActivationCommits) {
        pending.qtaRecord.credential.fill(0u);
    }
    _pendingRelayChildActivationCommits.clear();
    for (PendingClientPlayerRootActivationCommit& pending
         : _pendingClientPlayerRootActivationCommits) {
        pending.qtaRecord.credential.fill(0u);
    }
    _pendingClientPlayerRootActivationCommits.clear();
    for (PendingClientPlayerRootActivationRetirement& pending
         : _pendingClientPlayerRootActivationRetirements) {
        pending.qtaRecord.credential.fill(0u);
    }
    _pendingClientPlayerRootActivationRetirements.clear();
    _leaseKey.fill(0u);
    _assignmentSigner.reset();
    _verifiedCursor = {};
    _facts = {};
    _facts.revoked = true;
}

bool TopologyAuthority::ready() const {
    return _facts.complete() && _assignmentSigner.ready()
        && !allZero(_leaseKey.data(), _leaseKey.size());
}

TopologyAuthority::EnrollmentRecord* TopologyAuthority::findMutable(
    const model::NodeId nodeId) {
    const auto found = std::find_if(
        _enrollments.begin(), _enrollments.end(), [nodeId](const auto& item) {
            return item.nodeId == nodeId;
        });
    return found == _enrollments.end() ? nullptr : &*found;
}

const TopologyAuthority::EnrollmentRecord* TopologyAuthority::find(
    const model::NodeId nodeId) const {
    const auto found = std::find_if(
        _enrollments.begin(), _enrollments.end(), [nodeId](const auto& item) {
            return item.nodeId == nodeId;
        });
    return found == _enrollments.end() ? nullptr : &*found;
}

const TopologyAuthority::EnrollmentRecord* TopologyAuthority::findByViewer(
    const uint64_t viewerId) const {
    const auto found = std::find_if(
        _enrollments.begin(), _enrollments.end(), [viewerId](const auto& item) {
            return item.viewerId == viewerId;
        });
    return found == _enrollments.end() ? nullptr : &*found;
}

const TopologyAuthority::EnrollmentRecord*
TopologyAuthority::findByQspEndpoint(
    const RouteEndpoint& endpoint) const {
    const auto found = std::find_if(
        _enrollments.begin(), _enrollments.end(), [&endpoint](const auto& item) {
            return item.qspEndpoint == endpoint;
        });
    return found == _enrollments.end() ? nullptr : &*found;
}

const TopologyAuthority::EnrollmentRecord*
TopologyAuthority::findByQsrEndpoint(
    const RouteEndpoint& endpoint) const {
    const auto found = std::find_if(
        _enrollments.begin(), _enrollments.end(), [&endpoint](const auto& item) {
            return item.qsrBound && item.qsrEndpoint == endpoint;
        });
    return found == _enrollments.end() ? nullptr : &*found;
}

const TopologyAuthority::EnrollmentRecord*
TopologyAuthority::findByRelayDataEndpoint(
    const RouteEndpoint& endpoint) const {
    const auto found = std::find_if(
        _enrollments.begin(), _enrollments.end(), [&endpoint](const auto& item) {
            return item.relayDataBound && item.relayDataEndpoint == endpoint;
        });
    return found == _enrollments.end() ? nullptr : &*found;
}

TopologyAuthority::ClientPlayerRootRecord*
TopologyAuthority::findClientPlayerRootMutable(const model::NodeId nodeId) {
    const auto found = std::find_if(
        _clientPlayerRoots.begin(), _clientPlayerRoots.end(),
        [nodeId](const ClientPlayerRootRecord& item) {
            return item.binding.clientRootNodeId == nodeId;
        });
    return found == _clientPlayerRoots.end() ? nullptr : &*found;
}

const TopologyAuthority::ClientPlayerRootRecord*
TopologyAuthority::findClientPlayerRoot(const model::NodeId nodeId) const {
    const auto found = std::find_if(
        _clientPlayerRoots.begin(), _clientPlayerRoots.end(),
        [nodeId](const ClientPlayerRootRecord& item) {
            return item.binding.clientRootNodeId == nodeId;
        });
    return found == _clientPlayerRoots.end() ? nullptr : &*found;
}

bool TopologyAuthority::endpointConflictsWithTopologyBinding(
    const RouteEndpoint& endpoint, const model::NodeId exemptNodeId) const {
    if (!endpoint.valid()) return true;
    const auto endpointFor = [](const EndpointBinding& binding) {
        return RouteEndpoint{binding.advertised.ipv4BE,
                             binding.advertised.port};
    };
    const RouteEndpoint rootQsp = endpointFor(_facts.rootQsp);
    const RouteEndpoint rootQsr = endpointFor(_facts.rootQsr);
    if ((rootQsp.valid() && endpoint == rootQsp)
        || (rootQsr.valid() && endpoint == rootQsr)) {
        return true;
    }
    const bool spectatorConflict = std::any_of(
        _enrollments.begin(), _enrollments.end(),
        [&endpoint, exemptNodeId](const EnrollmentRecord& item) {
        if (item.nodeId != exemptNodeId && item.qspEndpoint == endpoint)
            return true;
        return (item.qsrBound && item.qsrEndpoint == endpoint)
            || (item.relayDataBound && item.relayDataEndpoint == endpoint);
    });
    return spectatorConflict
        || endpointConflictsWithClientPlayerRootBinding(endpoint,
                                                        exemptNodeId);
}

bool TopologyAuthority::endpointConflictsWithClientPlayerRootBinding(
    const RouteEndpoint& endpoint, const model::NodeId exemptNodeId) const {
    if (!endpoint.valid()) return true;
    return std::any_of(
        _clientPlayerRoots.begin(), _clientPlayerRoots.end(),
        [&endpoint, exemptNodeId](const ClientPlayerRootRecord& item) {
            if (item.binding.clientRootNodeId == exemptNodeId) return false;
            return endpoint == endpointFromRoute(item.binding.dataRoute)
                || endpoint == endpointFromRoute(item.binding.qsrRoute);
        });
}

bool TopologyAuthority::populateEnrollment(const EnrollmentRecord& record,
                                           TopologyEnrollment& out) const {
    out = {};
    if (!ready() || record.nodeId == model::kInvalidNodeId
        || !record.qspEndpoint.valid()) {
        return false;
    }
    out.granted = true;
    out.nodeId = record.nodeId;
    out.rootNodeId = _facts.rootNodeId;
    out.authorityGeneration = _facts.authorityGeneration;
    out.expiresMs = record.expiryMs;
    out.bindingGeneration = record.bindingGeneration;
    out.rootQsp = _facts.rootQsp;
    out.rootQsr = _facts.rootQsr;
    out.session = _facts.session;
    out.compatibility = _facts.compatibility;
    out.streamRootIdentity = _facts.streamRootIdentity;
    out.sourceGeneration = _facts.sourceGeneration;
    out.viewerId = record.viewerId;
    out.viewerGeneration = record.viewerGeneration;
    out.qspEndpoint = record.qspEndpoint;
    out.qspProvenance = record.qspProvenance;
    out.qspRouteGeneration = record.routeGeneration;
    out.confirmedCursor = record.confirmedCursor;
    out.qtaCursor = record.qtaCursor;
    out.limits = _facts.limits;
    out.assignmentSigningPublicKey = _facts.assignmentSigningPublicKey;
    out.credential = record.credential;
    return true;
}

bool TopologyAuthority::validBindingProof(
    const TopologyViewerProof& proof) const {
    return proof.viewerId != 0u && proof.viewerGeneration != 0u
        && proof.endpoint.valid() && proof.routeGeneration != 0u
        && proof.relayCapable
        && validRouteProvenance(proof.provenance);
}

bool TopologyAuthority::validProof(const TopologyViewerProof& proof) const {
    return validBindingProof(proof) && proof.qspStreaming;
}

void TopologyAuthority::expire(
    const uint64_t nowMs,
    std::vector<model::NodeId>* const detachedNodes) {
    if (!ready()) return;
    const auto expiredOffer = std::remove_if(
        _offers.begin(), _offers.end(), [nowMs](const OfferReservation& offer) {
            return offer.expiryMs <= nowMs;
        });
    _offers.erase(expiredOffer, _offers.end());
    const auto expiredStage = std::remove_if(
        _relayActivationStages.begin(), _relayActivationStages.end(),
        [nowMs](const RelayActivationStage& stage) {
            return stage.expiryMs == 0u || stage.expiryMs <= nowMs;
        });
    _relayActivationStages.erase(expiredStage,
                                 _relayActivationStages.end());
    for (auto activation = _relayActivations.begin();
         activation != _relayActivations.end();) {
        if (activation->activation.activationDeadlineMs > nowMs) {
            ++activation;
            continue;
        }
        discardRelayChildActivationCommits(activation->activation);
        rollbackRelayChildActivationAdjacency(*activation);
        if (!queueRelayChildActivationRetirement(
                *activation, wire::RelayChildRetirementReason::LeaseExpired)) {
            ++activation;
            continue;
        }
        activation = _relayActivations.erase(activation);
    }
    for (auto activation = _clientPlayerRootActivations.begin();
         activation != _clientPlayerRootActivations.end();) {
        if (activation->activation.activationDeadlineMs > nowMs) {
            ++activation;
            continue;
        }
        discardClientPlayerRootActivationCommits(activation->activation);
        if (!queueClientPlayerRootActivationRetirement(
                *activation,
                wire::ClientPlayerRootRetirementReason::LeaseExpired)) {
            ++activation;
            continue;
        }
        activation = _clientPlayerRootActivations.erase(activation);
    }
    for (auto assignment = _relayAssignments.begin();
         assignment != _relayAssignments.end();) {
        if (assignment->childLease.expiryMs > nowMs) {
            ++assignment;
            continue;
        }
        const RelayAssignmentReservation expired = *assignment;
        if (!queueRelayChildRetirement(
                expired, wire::RelayChildRetirementReason::LeaseExpired)) {
            ++assignment;
            continue;
        }
        assignment = _relayAssignments.erase(assignment);
    }

    for (size_t index = 0u; index < _clientPlayerRoots.size();) {
        if (_clientPlayerRoots[index].binding.expiresMs > nowMs) {
            ++index;
            continue;
        }
        const model::NodeId nodeId =
            _clientPlayerRoots[index].binding.clientRootNodeId;
        if (!releaseClientPlayerRootRecord(
                nodeId,
                wire::ClientPlayerRootRetirementReason::ClientRootExpired)) {
            ++index;
            continue;
        }
    }

    for (EnrollmentRecord& enrollment : _enrollments) {
        if (enrollment.expiryMs > nowMs) continue;
        releaseClientDataRootForNode(enrollment.nodeId);
        const bool rootOwnedAttachment = enrollment.attached
            && enrollment.attachmentOwner == AttachmentOwner::Root;
        if (rootOwnedAttachment && detachedNodes == nullptr) continue;
        if (rootOwnedAttachment)
            detachedNodes->push_back(enrollment.nodeId);
        if (std::find(_retiredNodeIds.begin(), _retiredNodeIds.end(),
                      enrollment.nodeId)
            == _retiredNodeIds.end()) {
            _retiredNodeIds.push_back(enrollment.nodeId);
        }
        if (!releaseRelayAssignmentsForNode(
                enrollment.nodeId,
                wire::RelayChildRetirementReason::LeaseExpired)) {
            continue;
        }
        enrollment.credential.fill(0u);
        enrollment.attached = false;
        enrollment.attachmentOwner = AttachmentOwner::None;
        enrollment.attachedLease = {};
        enrollment.attachedDepth = 0u;
        enrollment.attachedAncestry = {};
        enrollment.attachedAncestryCount = 0u;
        enrollment.relayDataEndpoint = {};
        enrollment.relayDataBound = false;
        enrollment.relayDataRequestNonce = 0u;
        enrollment.relayDataBindingGeneration = 0u;
        enrollment.relayReady = false;
        enrollment.relayReadyCacheRange = {};
        enrollment.relayReadyCursor = {};
    }
    const auto expiredEnrollment = std::remove_if(
        _enrollments.begin(), _enrollments.end(),
        [nowMs, detachedNodes](const auto& item) {
            return item.expiryMs <= nowMs
                && (!item.attached || detachedNodes != nullptr)
                && allZero(item.credential.data(), item.credential.size());
        });
    _enrollments.erase(expiredEnrollment, _enrollments.end());
}

bool TopologyAuthority::serviceExpiry(
    const uint64_t nowMs, std::vector<model::NodeId>& detachedNodes) {
    detachedNodes.clear();
    if (!ready()) return false;
    if (nowMs >= _facts.expiresMs) {
        for (const EnrollmentRecord& enrollment : _enrollments) {
            if (enrollment.attached
                && enrollment.attachmentOwner == AttachmentOwner::Root) {
                detachedNodes.push_back(enrollment.nodeId);
            }
        }
        revoke();
        return true;
    }
    expire(nowMs, &detachedNodes);
    return false;
}

bool TopologyAuthority::collectAuthorityState(
    const wire::AuthorityStateKind kind,
    const wire::AuthorityStateReason reason, const uint64_t nowMs,
    std::vector<AuthorityStateRecipient>& out, const model::NodeId onlyChild) {
    out.clear();
    if (!ready() || nowMs == 0u) return false;
    if ((kind != wire::AuthorityStateKind::Liveness
         && kind != wire::AuthorityStateKind::Revocation)
        || (kind == wire::AuthorityStateKind::Liveness
         && reason != wire::AuthorityStateReason::None)
        || (kind == wire::AuthorityStateKind::Revocation
            && reason == wire::AuthorityStateReason::None)) {
        return false;
    }

    model::StreamCompatibilityIdentity stream{};
    if (!model::makeStreamCompatibilityIdentity(
            _facts.compatibility, _facts.session.sessionId,
            _facts.session.epoch, _facts.streamRootIdentity, stream)) {
        return false;
    }
    try {
        out.reserve(_enrollments.size());
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (EnrollmentRecord& record : _enrollments) {
        if (onlyChild != model::kInvalidNodeId && record.nodeId != onlyChild)
            continue;
        if (!record.qspStreaming || !record.qsrBound
            || !record.qsrEndpoint.valid() || record.qsrEndpoint.port
                == record.qspEndpoint.port) {
            continue;
        }
        if (kind == wire::AuthorityStateKind::Liveness) {
            if (!record.attached || nowMs >= record.expiryMs
                || nowMs >= _facts.expiresMs
                || (record.lastLivenessSentMs != 0u
                    && nowMs - record.lastLivenessSentMs
                        < kTopologyAuthorityLivenessIntervalMs)) {
                continue;
            }
        }
        if (record.nextAuthorityStateSequence == 0u) return false;

        wire::AuthorityState state{};
        state.stream = stream;
        state.authorityGeneration = _facts.authorityGeneration;
        state.rootNodeId = _facts.rootNodeId;
        state.childNodeId = record.nodeId;
        state.viewerId = record.viewerId;
        state.viewerGeneration = record.viewerGeneration;
        state.qspRouteGeneration = record.routeGeneration;
        state.qsrIpv4BE = record.qsrEndpoint.ipv4BE;
        state.qsrPort = record.qsrEndpoint.port;
        state.qsrBindingGeneration = record.bindingGeneration;
        state.kind = kind;
        state.reason = reason;
        state.sequence = record.nextAuthorityStateSequence;
        state.authorityExpiryMs = record.expiryMs;
        state.eventTimeMs = nowMs;
        if (kind == wire::AuthorityStateKind::Liveness) {
            const uint64_t recordRemaining = record.expiryMs - nowMs;
            const uint64_t authorityRemaining = _facts.expiresMs - nowMs;
            const uint64_t bounded = (std::min)(
                static_cast<uint64_t>(kTopologyAuthorityLivenessWindowMs),
                (std::min)(recordRemaining, authorityRemaining));
            if (bounded == 0u) continue;
            state.livenessWindowMs = static_cast<uint32_t>(bounded);
            record.lastLivenessSentMs = nowMs;
        }
        // Sequence exhaustion terminates this state stream: never wrap a replay-protection counter.
        if (record.nextAuthorityStateSequence
            == (std::numeric_limits<uint64_t>::max)()) {
            record.nextAuthorityStateSequence = 0u;
        } else {
            ++record.nextAuthorityStateSequence;
        }

        AuthorityStateRecipient recipient{};
        recipient.qsrRoute.kind = model::RouteKind::Direct;
        recipient.qsrRoute.ipv4BE = record.qsrEndpoint.ipv4BE;
        recipient.qsrRoute.port = record.qsrEndpoint.port;
        recipient.qsrRoute.generation = record.routeGeneration;
        recipient.qsrRoute.routeProved = true;
        recipient.state = state;
        try {
            out.push_back(std::move(recipient));
        } catch (const std::bad_alloc&) {
            out.clear();
            return false;
        }
    }
    return true;
}

bool TopologyAuthority::enroll(const TopologyViewerProof& proof,
                                const uint64_t nowMs,
                                TopologyEnrollment& out,
                                std::string& error) {
    out = {};
    error.clear();
    if (!ready() || nowMs >= _facts.expiresMs) {
        error = "topology authority is not ready";
        return false;
    }
    expire(nowMs);
    if (!validProof(proof)) {
        error = "relay topology enrollment requires a proven streaming QSP route";
        return false;
    }
    // NodeIds must never be recycled for the lifetime of an authority.
    if (const EnrollmentRecord* existing = findByViewer(proof.viewerId)) {
        if (existing->viewerGeneration == proof.viewerGeneration
            && existing->qspEndpoint == proof.endpoint
            && existing->qspProvenance == proof.provenance
            && existing->routeGeneration == proof.routeGeneration
            && existing->qspStreaming && existing->expiryMs > nowMs
            && populateEnrollment(*existing, out)) {
            return true;
        }
        error = "topology enrollment is unavailable for this viewer";
        return false;
    }
    if (endpointConflictsWithTopologyBinding(proof.endpoint)
        || _enrollments.size() + _clientPlayerRoots.size()
               + _retiredNodeIds.size() + 1u
               >= _facts.limits.maxTopologyNodes) {
        error = "topology enrollment is unavailable for this viewer";
        return false;
    }

    model::NodeId nodeId = model::kInvalidNodeId;
    for (unsigned attempt = 0u; attempt < 16u; ++attempt) {
        if (!drawNonzero(nodeId)) {
            error = "the secure random provider could not allocate a topology NodeId";
            return false;
        }
        if (!nodeIdInUse(nodeId)) break;
        nodeId = model::kInvalidNodeId;
    }
    if (nodeId == model::kInvalidNodeId) {
        error = "the topology NodeId namespace could not allocate a unique node";
        return false;
    }

    EnrollmentRecord record{};
    record.nodeId = nodeId;
    record.viewerId = proof.viewerId;
    record.viewerGeneration = proof.viewerGeneration;
    record.qspEndpoint = proof.endpoint;
    record.qspProvenance = proof.provenance;
    record.routeGeneration = proof.routeGeneration;
    record.bindingGeneration = 1u;
    record.confirmedCursor = proof.confirmedCursor;
    record.qtaCursor = _verifiedCursor;
    record.expiryMs = nextNonzero(nowMs, _facts.limits.standbyLeaseMs);
    record.qspStreaming = true;
    if (!drawCredential(record.credential)) {
        error = "the secure random provider could not issue a topology credential";
        return false;
    }
    try {
        _enrollments.push_back(record);
    } catch (const std::bad_alloc&) {
        error = "topology enrollment storage was unavailable";
        return false;
    }

    return populateEnrollment(record, out);
}

bool TopologyAuthority::detachViewer(const uint64_t viewerId,
                                     ViewerLifecycleResult& out) {
    out = {};
    const auto found = std::find_if(
        _enrollments.begin(), _enrollments.end(), [viewerId](const auto& item) {
            return item.viewerId == viewerId;
        });
    if (found == _enrollments.end()) return false;
    ViewerLifecycleResult lifecycle{};
    lifecycle.nodeId = found->nodeId;
    lifecycle.releasedAttachment = found->attached;
    if (lifecycle.releasedAttachment)
        lifecycle.releasedAttachmentOwner = found->attachmentOwner;
    if (!releaseRelayAssignmentsForNode(
            found->nodeId,
            wire::RelayChildRetirementReason::ChildDetached)) {
        return false;
    }
    releaseClientDataRootForNode(found->nodeId);
    if (std::find(_retiredNodeIds.begin(), _retiredNodeIds.end(),
                  found->nodeId)
        == _retiredNodeIds.end()) {
        _retiredNodeIds.push_back(found->nodeId);
    }
    found->credential.fill(0u);
    found->attached = false;
    found->attachmentOwner = AttachmentOwner::None;
    found->attachedLease = {};
    found->attachedDepth = 0u;
    found->attachedAncestry = {};
    found->attachedAncestryCount = 0u;
    found->relayDataEndpoint = {};
    found->relayDataBound = false;
    found->relayDataRequestNonce = 0u;
    found->relayDataBindingGeneration = 0u;
    found->relayReady = false;
    found->relayReadyCacheRange = {};
    found->relayReadyCursor = {};
    _offers.erase(std::remove_if(_offers.begin(), _offers.end(),
                                 [nodeId = found->nodeId](const auto& offer) {
                                     return offer.childId == nodeId;
                                 }),
                  _offers.end());
    _enrollments.erase(found);
    out = lifecycle;
    return true;
}

bool TopologyAuthority::detachViewer(const uint64_t viewerId,
                                     model::NodeId& detachedNodeId) {
    ViewerLifecycleResult lifecycle{};
    if (!detachViewer(viewerId, lifecycle)) {
        detachedNodeId = model::kInvalidNodeId;
        return false;
    }
    detachedNodeId = lifecycle.nodeId;
    return true;
}

bool TopologyAuthority::beginViewerRebind(const TopologyViewerProof& proof,
                                          const uint64_t nowMs,
                                          ViewerLifecycleResult& out,
                                          std::string& error) {
    out = {};
    error.clear();
    if (!ready() || !validBindingProof(proof)) {
        error = "the replacement QSP route was not proved";
        return false;
    }
    expire(nowMs);
    EnrollmentRecord* record = nullptr;
    for (EnrollmentRecord& item : _enrollments) {
        if (item.viewerId == proof.viewerId) {
            record = &item;
            break;
        }
    }
    if (record == nullptr || proof.viewerGeneration <= record->viewerGeneration
        || proof.routeGeneration <= record->routeGeneration) {
        error = "the replacement QSP route generation was stale";
        return false;
    }
    if (endpointConflictsWithTopologyBinding(proof.endpoint,
                                             record->nodeId)) {
        error = "the replacement QSP route collides with a topology endpoint";
        return false;
    }
    ViewerLifecycleResult lifecycle{};
    lifecycle.nodeId = record->nodeId;
    lifecycle.releasedAttachment = record->attached;
    if (lifecycle.releasedAttachment)
        lifecycle.releasedAttachmentOwner = record->attachmentOwner;
    if (!releaseRelayAssignmentsForNode(
            record->nodeId,
            wire::RelayChildRetirementReason::ChildBindingReplaced)) {
        error = "the prior relay child bindings could not be retired";
        return false;
    }
    releaseClientDataRootForNode(record->nodeId);
    record->viewerGeneration = proof.viewerGeneration;
    record->qspEndpoint = proof.endpoint;
    record->qspProvenance = proof.provenance;
    record->qsrEndpoint = {};
    record->qsrBound = false;
    record->relayDataEndpoint = {};
    record->relayDataBound = false;
    record->relayDataRequestNonce = 0u;
    record->relayDataBindingGeneration = 0u;
    record->relayReady = false;
    record->relayReadyCacheRange = {};
    record->relayReadyCursor = {};
    record->routeGeneration = proof.routeGeneration;
    ++record->bindingGeneration;
    record->confirmedCursor = proof.confirmedCursor;
    record->qtaCursor = _verifiedCursor;
    record->lastInboundSequence = 0u;
    record->qspStreaming = false;
    record->attached = false;
    record->attachmentOwner = AttachmentOwner::None;
    record->attachedLease = {};
    record->attachedDepth = 0u;
    record->attachedAncestry = {};
    record->attachedAncestryCount = 0u;
    record->expiryMs = nextNonzero(nowMs, _facts.limits.standbyLeaseMs);
    _offers.erase(std::remove_if(_offers.begin(), _offers.end(),
                                 [nodeId = record->nodeId](const auto& offer) {
                                     return offer.childId == nodeId;
                                 }),
                  _offers.end());
    out = lifecycle;
    return true;
}

bool TopologyAuthority::beginViewerRebind(const TopologyViewerProof& proof,
                                          const uint64_t nowMs,
                                          std::string& error) {
    ViewerLifecycleResult ignored{};
    return beginViewerRebind(proof, nowMs, ignored, error);
}

bool TopologyAuthority::refreshViewerBinding(const TopologyViewerProof& proof,
                                             const uint64_t nowMs,
                                             ViewerLifecycleResult& out,
                                             std::string& error) {
    out = {};
    error.clear();
    if (!ready() || !validProof(proof)) {
        error = "the replacement QSP route was not proved";
        return false;
    }
    expire(nowMs);
    EnrollmentRecord* record = nullptr;
    for (EnrollmentRecord& item : _enrollments) {
        if (item.viewerId == proof.viewerId) {
            record = &item;
            break;
        }
    }
    if (record == nullptr || proof.viewerGeneration < record->viewerGeneration
        || proof.routeGeneration < record->routeGeneration) {
        error = "the replacement QSP route generation was stale";
        return false;
    }
    const bool sameBinding = proof.viewerGeneration == record->viewerGeneration
        && proof.routeGeneration == record->routeGeneration
        && proof.endpoint == record->qspEndpoint
        && proof.provenance == record->qspProvenance;
    if (sameBinding) {
        if (endpointConflictsWithTopologyBinding(proof.endpoint,
                                                 record->nodeId)) {
            error = "the replacement QSP route collides with a topology endpoint";
            return false;
        }
        if (record->qspStreaming) {
            error = "the replacement QSP route generation was stale";
            return false;
        }
        record->qspStreaming = true;
        record->confirmedCursor = proof.confirmedCursor;
        record->expiryMs = nextNonzero(nowMs, _facts.limits.standbyLeaseMs);
        return true;
    }
    if (proof.viewerGeneration <= record->viewerGeneration
        || proof.routeGeneration <= record->routeGeneration) {
        error = "the replacement QSP route generation was stale";
        return false;
    }
    if (endpointConflictsWithTopologyBinding(proof.endpoint,
                                             record->nodeId)) {
        error = "the replacement QSP route collides with a topology endpoint";
        return false;
    }
    ViewerLifecycleResult lifecycle{};
    lifecycle.nodeId = record->nodeId;
    lifecycle.releasedAttachment = record->attached;
    if (lifecycle.releasedAttachment)
        lifecycle.releasedAttachmentOwner = record->attachmentOwner;
    if (!releaseRelayAssignmentsForNode(
            record->nodeId,
            wire::RelayChildRetirementReason::ChildBindingReplaced)) {
        error = "the prior relay child bindings could not be retired";
        return false;
    }
    releaseClientDataRootForNode(record->nodeId);
    record->viewerGeneration = proof.viewerGeneration;
    record->qspEndpoint = proof.endpoint;
    record->qspProvenance = proof.provenance;
    record->qsrEndpoint = {};
    record->qsrBound = false;
    record->relayDataEndpoint = {};
    record->relayDataBound = false;
    record->relayDataRequestNonce = 0u;
    record->relayDataBindingGeneration = 0u;
    record->relayReady = false;
    record->relayReadyCacheRange = {};
    record->relayReadyCursor = {};
    record->routeGeneration = proof.routeGeneration;
    ++record->bindingGeneration;
    record->confirmedCursor = proof.confirmedCursor;
    record->qtaCursor = _verifiedCursor;
    record->lastInboundSequence = 0u;
    record->qspStreaming = true;
    record->attached = false;
    record->attachmentOwner = AttachmentOwner::None;
    record->attachedLease = {};
    record->attachedDepth = 0u;
    record->attachedAncestry = {};
    record->attachedAncestryCount = 0u;
    record->expiryMs = nextNonzero(nowMs, _facts.limits.standbyLeaseMs);
    _offers.erase(std::remove_if(_offers.begin(), _offers.end(),
                                 [nodeId = record->nodeId](const auto& offer) {
                                     return offer.childId == nodeId;
                                 }),
                  _offers.end());
    out = lifecycle;
    return true;
}

bool TopologyAuthority::refreshViewerBinding(const TopologyViewerProof& proof,
                                             const uint64_t nowMs,
                                             std::string& error) {
    ViewerLifecycleResult ignored{};
    return refreshViewerBinding(proof, nowMs, ignored, error);
}

bool TopologyAuthority::writeEnvelopeMacInput(
    const EnrollmentRecord& record, const uint64_t sequence,
    const uint8_t* qsrBytes, const size_t qsrLength,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!ready() || qsrBytes == nullptr || qsrLength == 0u
        || qsrLength > wire::kMaxResilientControlDatagramBytes) {
        return false;
    }
    try {
        out.reserve(512u + qsrLength);
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (!appendBytes(out, reinterpret_cast<const uint8_t*>(kTopologyCredentialDomain),
                     sizeof(kTopologyCredentialDomain) - 1u)) {
        return false;
    }
    appendU64(out, _facts.session.sessionId);
    appendU64(out, _facts.session.epoch);
    appendU64(out, _facts.session.cookie.low);
    appendU64(out, _facts.session.cookie.high);
    if (!appendCompatibility(out, _facts.compatibility)) return false;
    appendU64(out, _facts.streamRootIdentity);
    appendU64(out, _facts.authorityGeneration);
    appendU64(out, _facts.rootNodeId);
    appendEndpointBinding(out, _facts.rootQsp);
    appendEndpointBinding(out, _facts.rootQsr);
    appendU64(out, _facts.sourceGeneration);
    appendU64(out, record.nodeId);
    appendU64(out, record.viewerId);
    appendU32(out, record.viewerGeneration);
    appendU32(out, record.qspEndpoint.ipv4BE);
    appendU16(out, record.qspEndpoint.port);
    appendU8(out, static_cast<uint8_t>(record.qspProvenance));
    appendU64(out, record.routeGeneration);
    appendU64(out, record.bindingGeneration);
    appendU32(out, record.confirmedCursor);
    appendU64(out, record.expiryMs);
    appendU64(out, sequence);
    appendU32(out, record.qtaCursor.nextRecord);
    appendU32(out, record.qtaCursor.nextEvent);
    appendU64(out, record.qtaCursor.digest);
    appendU8(out, 1u);
    appendU64(out, _facts.limits.maxTopologyNodes);
    appendU8(out, _facts.limits.maxActiveDepth);
    appendU64(out, _facts.limits.maxChildrenPerRoot);
    appendU64(out, _facts.limits.maxChildrenPerRelay);
    appendU64(out, _facts.limits.maxStandbyParents);
    appendU64(out, _facts.limits.standbyLeaseMs);
    if (!appendBytes(out, _facts.assignmentSigningPublicKey.data(),
                     _facts.assignmentSigningPublicKey.size())) {
        return false;
    }
    return appendBytes(out, qsrBytes, qsrLength);
}

bool TopologyAuthority::encodeControl(const TopologyEnrollment& enrollment,
                                      const uint64_t sequence,
                                      const uint8_t* qsrBytes,
                                      const size_t qsrLength,
                                      std::vector<uint8_t>& out) const {
    out.clear();
    const EnrollmentRecord* record = find(enrollment.nodeId);
    if (!ready() || record == nullptr || !enrollment.granted
        || enrollment.authorityGeneration != _facts.authorityGeneration
        || sequence == 0u || qsrBytes == nullptr || qsrLength == 0u
        || qsrLength > wire::kMaxResilientControlDatagramBytes
        || !equalConstantTime(record->credential.data(),
                              enrollment.credential.data(),
                              record->credential.size())) {
        return false;
    }
    return encodeControlForRecord(*record, sequence, qsrBytes, qsrLength,
                                  out);
}

bool TopologyAuthority::encodeControlForRecord(
    const EnrollmentRecord& record, const uint64_t sequence,
    const uint8_t* qsrBytes, const size_t qsrLength,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!ready() || record.nodeId == model::kInvalidNodeId
        || allZero(record.credential.data(), record.credential.size())
        || sequence == 0u || qsrBytes == nullptr || qsrLength == 0u
        || qsrLength > wire::kMaxResilientControlDatagramBytes) {
        return false;
    }
    std::vector<uint8_t> macInput;
    if (!writeEnvelopeMacInput(record, sequence, qsrBytes, qsrLength,
                               macInput)) {
        return false;
    }
    std::array<uint8_t, 32> mac{};
    qnetplay::spectator::hmacSha256(
        record.credential.data(), record.credential.size(), macInput.data(),
        macInput.size(), mac);
    try {
        out.assign(kTopologyEnvelopePrefixBytes + qsrLength, 0u);
    } catch (const std::bad_alloc&) {
        return false;
    }
    writeLe32(out.data(), kTopologyControlMagic);
    writeLe16(out.data() + 4u, kTopologyControlVersion);
    out[6] = kTopologyControlType;
    out[7] = 0u;
    writeLe16(out.data() + 8u, static_cast<uint16_t>(qsrLength));
    writeLe16(out.data() + 10u, 0u);
    writeLe64(out.data() + 12u, _facts.authorityGeneration);
    writeLe64(out.data() + 20u, record.nodeId);
    writeLe64(out.data() + 28u, sequence);
    std::copy_n(mac.data(), kTopologyMacBytes, out.data() + 36u);
    std::copy_n(qsrBytes, qsrLength,
                out.data() + static_cast<std::ptrdiff_t>(
                                 kTopologyEnvelopePrefixBytes));
    return true;
}

TopologyControlResult TopologyAuthority::decodeControl(
    const RouteEndpoint& from, const uint8_t* bytes, const size_t length,
    const uint64_t nowMs, TopologyControlEnvelope& out) {
    out = {};
    if (!ready()) return TopologyControlResult::NotReady;
    if (nowMs >= _facts.expiresMs) return TopologyControlResult::Expired;
    size_t qsrLength = 0u;
    if (!validEnvelopeHeader(bytes, length, qsrLength))
        return TopologyControlResult::Malformed;
    const uint64_t authorityGeneration = readLe64(bytes + 12u);
    const model::NodeId nodeId = readLe64(bytes + 20u);
    const uint64_t sequence = readLe64(bytes + 28u);
    if (authorityGeneration != _facts.authorityGeneration)
        return TopologyControlResult::AuthorityMismatch;
    EnrollmentRecord* record = findMutable(nodeId);
    if (record == nullptr) return TopologyControlResult::UnknownNode;
    if (nowMs >= record->expiryMs) return TopologyControlResult::Expired;
    if (!record->qspStreaming) return TopologyControlResult::BindingRejected;
    const uint8_t* const qsr = bytes + kTopologyEnvelopePrefixBytes;

    std::vector<uint8_t> macInput;
    if (!writeEnvelopeMacInput(*record, sequence, qsr, qsrLength, macInput))
        return TopologyControlResult::AuthenticationRejected;
    std::array<uint8_t, 32> expected{};
    qnetplay::spectator::hmacSha256(
        record->credential.data(), record->credential.size(), macInput.data(),
        macInput.size(), expected);
    if (!equalConstantTime(bytes + 36u, expected.data(), kTopologyMacBytes))
        return TopologyControlResult::AuthenticationRejected;

    wire::ControlDatagram datagram{};
    if (wire::decodeControlDatagram(qsr, qsrLength, datagram)
        != wire::DecodeResult::Ok) {
        return TopologyControlResult::Malformed;
    }
    if (sequence == 0u || sequence <= record->lastInboundSequence)
        return TopologyControlResult::Replay;

    const bool relayRecord = relayProvenance(record->qspProvenance);
    const bool virtualIngress =
        from.ipv4BE == qnetplay::spectator::kRelayVirtualIpv4BE;

    if (datagram.type == wire::ControlMessageType::RelayDataBindingRequest) {
        if (relayRecord || virtualIngress)
            return TopologyControlResult::BindingRejected;
        if (!from.valid() || from.port == record->qspEndpoint.port
            || !record->qsrBound
            || from.port == record->qsrEndpoint.port) {
            return TopologyControlResult::EndpointMismatch;
        }
        if (!acceptRelayDataBinding(*record,
                                    datagram.relayDataBindingRequest,
                                    from, nowMs)) {
            return TopologyControlResult::BindingRejected;
        }
        try {
            out.qsr.assign(qsr, qsr + qsrLength);
        } catch (const std::bad_alloc&) {
            return TopologyControlResult::Malformed;
        }
        record->lastInboundSequence = sequence;
        out.authorityGeneration = authorityGeneration;
        out.nodeId = nodeId;
        out.sequence = sequence;
        std::copy_n(bytes + 36u, kTopologyMacBytes, out.mac.data());
        return TopologyControlResult::Accepted;
    }

    if (relayRecord != virtualIngress)
        return TopologyControlResult::EndpointMismatch;
    if (!from.valid() || from.port == record->qspEndpoint.port)
        return TopologyControlResult::EndpointMismatch;
    if (record->qsrBound && !sameEndpoint(from, record->qsrEndpoint))
        return TopologyControlResult::EndpointMismatch;

    std::vector<uint8_t> normalizedQsr;
    const uint8_t* admittedQsr = qsr;
    size_t admittedQsrLength = qsrLength;
    if (datagram.type == wire::ControlMessageType::NodeAdvertisement) {
        const bool relayRouteClaim =
            datagram.nodeAdvertisement.route.kind
                == model::RouteKind::TrafficRelay
            || datagram.nodeAdvertisement.controlRoute.kind
                == model::RouteKind::TrafficRelay;
        if (relayRouteClaim != relayRecord)
            return TopologyControlResult::BindingRejected;
        model::NodeAdvertisement expectedAdvertisement{};
        if (!makeChildAdvertisement(*record, from, expectedAdvertisement))
            return TopologyControlResult::BindingRejected;
        datagram.nodeAdvertisement.controlRoute =
            expectedAdvertisement.controlRoute;
        if (!wire::encodeControlDatagram(datagram, normalizedQsr))
            return TopologyControlResult::Malformed;
        admittedQsr = normalizedQsr.data();
        admittedQsrLength = normalizedQsr.size();
    }
    if (!verifyControlDatagram(*record, datagram, from, nowMs))
        return TopologyControlResult::BindingRejected;
    if (datagram.type == wire::ControlMessageType::RelayReady
        && !acceptRelayReady(*record, datagram.relayReady, nowMs)) {
        return TopologyControlResult::BindingRejected;
    }
    if (datagram.type == wire::ControlMessageType::RelayChildActivationReceipt
        && !acceptRelayChildActivationReceipt(
            *record, datagram.relayChildActivationReceipt, nowMs)) {
        return TopologyControlResult::BindingRejected;
    }
    if (datagram.type
            == wire::ControlMessageType::ClientPlayerRootActivationReceipt
        && !acceptClientPlayerRootActivationReceipt(
            *record, datagram.clientPlayerRootActivationReceipt, nowMs)) {
        return TopologyControlResult::BindingRejected;
    }

    try {
        out.qsr.assign(admittedQsr, admittedQsr + admittedQsrLength);
    } catch (const std::bad_alloc&) {
        return TopologyControlResult::Malformed;
    }
    if (datagram.type == wire::ControlMessageType::ClientDataRootAssertion) {
        ClientDataRootDescriptor registered{};
        if (!registerClientDataRoot(datagram.clientDataRootAssertion, nowMs,
                                    registered)) {
            out = {};
            return TopologyControlResult::BindingRejected;
        }
    }
    if (!record->qsrBound) {
        const EnrollmentRecord* const existing = findByQsrEndpoint(from);
        if (existing != nullptr && existing->nodeId != record->nodeId)
            return TopologyControlResult::BindingRejected;
        record->qsrEndpoint = from;
        record->qsrBound = true;
    }
    record->lastInboundSequence = sequence;
    out.authorityGeneration = authorityGeneration;
    out.nodeId = nodeId;
    out.sequence = sequence;
    std::copy_n(bytes + 36u, kTopologyMacBytes, out.mac.data());
    return TopologyControlResult::Accepted;
}

bool TopologyAuthority::verifyControlDatagram(
    const EnrollmentRecord& record, const wire::ControlDatagram& datagram,
    const RouteEndpoint& from, const uint64_t nowMs) const {
    if (!this->ready() || nowMs >= _facts.expiresMs || nowMs >= record.expiryMs
        || !record.qspStreaming || !from.valid()
        || from.port == record.qspEndpoint.port
        || (record.qsrBound && from != record.qsrEndpoint)) {
        return false;
    }
    switch (datagram.type) {
    case wire::ControlMessageType::NodeAdvertisement: {
        model::NodeAdvertisement expected{};
        const RouteEndpoint qsrEndpoint = record.qsrBound
            ? record.qsrEndpoint : from;
        const EnrollmentRecord* const existing = findByQsrEndpoint(from);
        return (existing == nullptr || existing->nodeId == record.nodeId)
            && makeChildAdvertisement(record, qsrEndpoint, expected)
            && datagram.nodeAdvertisement.nodeId == expected.nodeId
            && datagram.nodeAdvertisement.kind == expected.kind
            && datagram.nodeAdvertisement.stream == expected.stream
            && sameRoute(datagram.nodeAdvertisement.route, expected.route)
            && sameRoute(datagram.nodeAdvertisement.controlRoute,
                         expected.controlRoute)
            && datagram.nodeAdvertisement.activeParent
                == model::kInvalidNodeId
            && datagram.nodeAdvertisement.depth == 0u
            && datagram.nodeAdvertisement.ancestryCount == 0u
            && datagram.nodeAdvertisement.capabilityModelRevision
                == expected.capabilityModelRevision
            && datagram.nodeAdvertisement.capabilities == expected.capabilities
            && datagram.nodeAdvertisement.childCapacity == 0u
            && datagram.nodeAdvertisement.childCount == 0u
            && datagram.nodeAdvertisement.sourceGeneration
                == expected.sourceGeneration
            && datagram.nodeAdvertisement.routeGeneration
                == expected.routeGeneration;
    }
    case wire::ControlMessageType::ParentLease: {
        if (!record.qsrBound) return false;
        const model::ParentLease& lease = datagram.parentLease;
        model::ParentLeaseChildBinding expectedChildBinding{};
        if (!makeParentLeaseChildBinding(record, expectedChildBinding))
            return false;
        const auto offer = std::find_if(
            _offers.begin(), _offers.end(), [&lease, &record](const auto& item) {
                return item.childId == record.nodeId
                    && item.nonce == lease.replayNonce;
            });
        return lease.stream.setOrSessionId == _facts.session.sessionId
            && lease.stream.gameEpoch == _facts.session.epoch
            && sameCompatibility(lease.stream.compatibility, _facts.compatibility)
            && lease.stream.streamRootIdentity == _facts.streamRootIdentity
            && lease.childId == record.nodeId
            && lease.childBinding == expectedChildBinding
            && lease.parentId == _facts.rootNodeId
            && lease.parentDepth == 0u
            && lease.parentSourceGeneration == _facts.sourceGeneration
            && lease.routeGeneration == _facts.rootQsp.generation
            && lease.routeCount == 1u
            && sameRoute(lease.routes[0], _facts.rootQsp.advertised)
            && lease.cacheRange.firstCursor
                <= lease.cacheRange.nextCursor
            && lease.cacheRange.containsBoundary(
                expectedChildBinding.childRequestedCursor.nextRecord)
            && lease.latestVerifiedCursor == lease.cacheRange.nextCursor
            && lease.childCapacity != 0u
            && lease.expiryMs > nowMs && offer != _offers.end()
            && offer->expiryMs == lease.expiryMs
            && offer->cursor.nextRecord == lease.latestVerifiedCursor
            && lease.ancestryCount == 1u
            && lease.ancestry[0] == _facts.rootNodeId;
    }
    case wire::ControlMessageType::RelayReady:
        return relayReadyMatches(record, datagram.relayReady, nowMs);
    case wire::ControlMessageType::ClientDataRootAssertion:
        return record.qsrBound
            && clientDataRootAssertionMatches(
                record, datagram.clientDataRootAssertion, nowMs);
    case wire::ControlMessageType::RelayChildActivationReceipt: {
        if (!record.qsrBound) return false;
        const auto activation = std::find_if(
            _relayActivations.begin(), _relayActivations.end(),
            [&datagram](const RelayActivationReservation& reservation) {
                return reservation.activation.activationNonce
                    == datagram.relayChildActivationReceipt.activationNonce;
            });
        return activation != _relayActivations.end()
            && activationReceiptMatches(
                *activation, record, datagram.relayChildActivationReceipt,
                nowMs);
    }
    case wire::ControlMessageType::ClientPlayerRootActivationReceipt: {
        if (!record.qsrBound) return false;
        const auto activation = std::find_if(
            _clientPlayerRootActivations.begin(),
            _clientPlayerRootActivations.end(),
            [&datagram](const ClientPlayerRootActivationReservation& value) {
                return value.activation.activationNonce
                    == datagram.clientPlayerRootActivationReceipt
                           .activation.activationNonce;
            });
        return activation != _clientPlayerRootActivations.end()
            && clientPlayerRootActivationReceiptMatches(
                *activation, record,
                datagram.clientPlayerRootActivationReceipt, nowMs);
    }
    default:
        return false;
    }
}

bool TopologyAuthority::authorize(const RouteEndpoint& from,
                                  const wire::ControlDatagram& datagram,
                                  const uint64_t nowMs) const {
    model::NodeId nodeId = model::kInvalidNodeId;
    switch (datagram.type) {
    case wire::ControlMessageType::NodeAdvertisement:
        nodeId = datagram.nodeAdvertisement.nodeId;
        break;
    case wire::ControlMessageType::ParentLease:
        nodeId = datagram.parentLease.childId;
        break;
    default:
        return false;
    }
    const EnrollmentRecord* record = find(nodeId);
    return record != nullptr
        && verifyControlDatagram(*record, datagram, from, nowMs);
}

bool TopologyAuthority::parentLeaseChildBinding(
    const model::NodeId childId,
    model::ParentLeaseChildBinding& out) const {
    out = {};
    const EnrollmentRecord* const record = find(childId);
    return ready() && record != nullptr && record->qspStreaming
        && record->qsrBound && record->expiryMs > _facts.createdMs
        && makeParentLeaseChildBinding(*record, out);
}

bool TopologyAuthority::reserveRootOffer(const model::NodeId childId,
                                         const uint64_t nowMs,
                                         uint64_t& nonce,
                                         uint64_t& expiryMs) {
    nonce = 0u;
    expiryMs = 0u;
    if (!ready() || nowMs >= _facts.expiresMs) return false;
    expire(nowMs);
    EnrollmentRecord* record = findMutable(childId);
    if (record == nullptr || record->attached || nowMs >= record->expiryMs)
        return false;
    model::ParentLeaseChildBinding childBinding{};
    if (!record->qsrBound
        || !makeParentLeaseChildBinding(*record, childBinding)) {
        return false;
    }
    if (std::any_of(_offers.begin(), _offers.end(),
                    [childId](const OfferReservation& offer) {
                        return offer.childId == childId && !offer.redeemed;
                    })
        || std::any_of(_relayActivationStages.begin(),
                       _relayActivationStages.end(),
                       [childId](const RelayActivationStage& stage) {
                           return stage.childNodeId == childId;
                       })
        || std::any_of(_relayAssignments.begin(), _relayAssignments.end(),
                       [childId](const RelayAssignmentReservation& assignment) {
                           return assignment.childNodeId == childId;
                       })
        || std::any_of(_relayActivations.begin(), _relayActivations.end(),
                       [childId](const RelayActivationReservation& activation) {
                           return activation.activation.childNodeId == childId;
                       })) {
        return false;
    }
    const size_t attached = static_cast<size_t>(std::count_if(
        _enrollments.begin(), _enrollments.end(), [](const auto& item) {
            return item.attached
                && item.attachmentOwner == AttachmentOwner::Root;
        }));
    if (attached + reservedCount() >= _facts.limits.maxChildrenPerRoot)
        return false;
    for (unsigned attempt = 0u; attempt < 16u; ++attempt) {
        if (!drawNonzero(nonce)) return false;
        const bool duplicate = std::any_of(
            _offers.begin(), _offers.end(), [nonce](const auto& offer) {
                return offer.nonce == nonce;
            });
        if (!duplicate) break;
        nonce = 0u;
    }
    if (nonce == 0u) return false;
    expiryMs = (std::min)(nextNonzero(nowMs, _facts.limits.standbyLeaseMs),
                          (std::min)(record->expiryMs, _facts.expiresMs));
    if (expiryMs <= nowMs) {
        expiryMs = 0u;
        return false;
    }
    try {
        _offers.push_back(OfferReservation{childId, nonce, expiryMs,
                                           _verifiedCursor, false});
    } catch (const std::bad_alloc&) {
        nonce = 0u;
        expiryMs = 0u;
        return false;
    }
    return true;
}

bool TopologyAuthority::confirmRedemption(const model::ParentLease& lease,
                                          const uint64_t nowMs) {
    EnrollmentRecord* record = findMutable(lease.childId);
    if (record == nullptr || record->attached || nowMs >= record->expiryMs)
        return false;
    const auto found = std::find_if(
        _offers.begin(), _offers.end(), [&lease](const OfferReservation& offer) {
            return offer.childId == lease.childId
                && offer.nonce == lease.replayNonce && !offer.redeemed;
        });
    if (found == _offers.end() || found->expiryMs != lease.expiryMs
        || found->expiryMs <= nowMs) {
        return false;
    }
    if (lease.parentId != _facts.rootNodeId || lease.parentDepth != 0u
        || lease.ancestryCount != 1u
        || lease.ancestry[0] != _facts.rootNodeId) {
        return false;
    }
    found->redeemed = true;
    record->attached = true;
    record->attachmentOwner = AttachmentOwner::Root;
    record->attachedLease = leaseBindingOf(lease);
    record->attachedDepth = 1u;
    record->attachedAncestry = lease.ancestry;
    record->attachedAncestryCount = lease.ancestryCount;
    return true;
}

bool TopologyAuthority::rollbackRedemption(const model::ParentLease& lease) {
    EnrollmentRecord* record = findMutable(lease.childId);
    if (record == nullptr || !record->attached
        || record->attachmentOwner != AttachmentOwner::Root) {
        return false;
    }
    const auto found = std::find_if(
        _offers.begin(), _offers.end(), [&lease](const OfferReservation& offer) {
            return offer.childId == lease.childId
                && offer.nonce == lease.replayNonce && offer.redeemed
                && offer.expiryMs == lease.expiryMs;
        });
    if (found == _offers.end()) return false;
    if (!releaseRelayAssignmentsForNode(
            record->nodeId,
            wire::RelayChildRetirementReason::ChildBindingReplaced)) {
        return false;
    }
    releaseClientDataRootForNode(record->nodeId);
    record->attached = false;
    record->attachmentOwner = AttachmentOwner::None;
    record->attachedLease = {};
    record->attachedDepth = 0u;
    record->attachedAncestry = {};
    record->attachedAncestryCount = 0u;
    record->relayDataEndpoint = {};
    record->relayDataBound = false;
    record->relayDataRequestNonce = 0u;
    record->relayDataBindingGeneration = 0u;
    record->relayReady = false;
    record->relayReadyCacheRange = {};
    record->relayReadyCursor = {};
    _offers.erase(found);
    return true;
}

void TopologyAuthority::releaseOffer(const model::NodeId childId,
                                     const uint64_t nonce) {
    _offers.erase(std::remove_if(_offers.begin(), _offers.end(),
                                 [childId, nonce](const auto& offer) {
                                     return offer.childId == childId
                                         && offer.nonce == nonce;
                                 }),
                  _offers.end());
}

void TopologyAuthority::setVerifiedCursor(
    const model::VerifiedChainCursor& cursor) {
    if (!ready()) return;
    if (!sameCursor(_verifiedCursor, cursor)) {
        releaseAllClientDataRoots();
        (void)releaseAllClientPlayerRootActivations(
            wire::ClientPlayerRootRetirementReason::ClientRootBindingReplaced);
        for (ClientPlayerRootRecord& root : _clientPlayerRoots)
            root.asserted = false;
    }
    _verifiedCursor = cursor;
    for (EnrollmentRecord& record : _enrollments) {
        if (!record.relayReady
            || sameCursor(record.relayReadyCursor, _verifiedCursor)) {
            continue;
        }
        record.relayReady = false;
        record.relayReadyCacheRange = {};
        record.relayReadyCursor = {};
    }
}

uint32_t TopologyAuthority::requiredCursor(const model::NodeId childId) const {
    const EnrollmentRecord* record = find(childId);
    return record == nullptr ? 0u : record->qtaCursor.nextRecord;
}

bool TopologyAuthority::enrollmentForEndpoint(
    const RouteEndpoint& endpoint, TopologyEnrollment& out) const {
    out = {};
    const EnrollmentRecord* record = findByQspEndpoint(endpoint);
    if (record == nullptr || !record->qspStreaming || !ready()) return false;
    return populateEnrollment(*record, out);
}

bool TopologyAuthority::enrollmentForQsrEndpoint(
    const RouteEndpoint& endpoint, TopologyEnrollment& out) const {
    out = {};
    const EnrollmentRecord* record = findByQsrEndpoint(endpoint);
    if (record == nullptr || !record->qspStreaming || !ready()) return false;
    return populateEnrollment(*record, out);
}

bool TopologyAuthority::enrollmentForNode(const model::NodeId nodeId,
                                          TopologyEnrollment& out) const {
    out = {};
    const EnrollmentRecord* record = find(nodeId);
    if (record == nullptr || !ready()) return false;
    return populateEnrollment(*record, out);
}

bool TopologyAuthority::nodeForViewer(const uint64_t viewerId,
                                      model::NodeId& out) const {
    out = model::kInvalidNodeId;
    const EnrollmentRecord* record = findByViewer(viewerId);
    if (record == nullptr || !ready()) return false;
    out = record->nodeId;
    return out != model::kInvalidNodeId;
}

bool TopologyAuthority::isAttached(const model::NodeId nodeId) const {
    const EnrollmentRecord* record = find(nodeId);
    return record != nullptr && record->attached;
}

size_t TopologyAuthority::enrolledCount() const {
    return _enrollments.size();
}

size_t TopologyAuthority::attachedCount() const {
    return static_cast<size_t>(std::count_if(
        _enrollments.begin(), _enrollments.end(), [](const auto& item) {
            return item.attached;
        }));
}

size_t TopologyAuthority::reservedCount() const {
    return static_cast<size_t>(std::count_if(
        _offers.begin(), _offers.end(), [](const auto& item) {
            return !item.redeemed;
        }));
}

bool TopologyAuthority::registerClientDataRoot(
    const wire::ClientDataRootAssertion& assertion, const uint64_t nowMs,
    ClientDataRootDescriptor& out) {
    out = {};
    if (!ready() || nowMs == 0u || nowMs >= _facts.expiresMs) return false;
    const EnrollmentRecord* const record = find(assertion.clientRootNodeId);
    if (record == nullptr
        || !clientDataRootAssertionMatches(*record, assertion, nowMs)) {
        return false;
    }

    ClientDataRootDescriptor descriptor{};
    descriptor.registered = true;
    descriptor.nodeId = record->nodeId;
    descriptor.stream = assertion.stream;
    descriptor.gameStartFingerprint = assertion.gameStartFingerprint;
    descriptor.authorityGeneration = assertion.authorityGeneration;
    descriptor.qspRouteGeneration = assertion.qspRouteGeneration;
    descriptor.qsrBindingGeneration = assertion.qsrBindingGeneration;
    descriptor.sourceGeneration = assertion.sourceGeneration;
    descriptor.dataRoute = assertion.dataRoute;
    descriptor.qsrRoute = assertion.qsrRoute;
    descriptor.cacheRange = assertion.cacheRange;
    descriptor.cursor = assertion.cursor;
    descriptor.childCapacity = assertion.childCapacity;
    descriptor.expiresMs = record->expiryMs;

    const auto existing = std::find_if(
        _clientDataRoots.begin(), _clientDataRoots.end(),
        [nodeId = record->nodeId](const ClientDataRootRecord& value) {
            return value.descriptor.nodeId == nodeId;
        });
    if (existing != _clientDataRoots.end()) {
        if (!sameClientDataRootDescriptor(existing->descriptor, descriptor))
            return false;
        out = existing->descriptor;
        return true;
    }

    const size_t maxClientRoots = _facts.limits.maxTopologyNodes > 0u
        ? _facts.limits.maxTopologyNodes - 1u : 0u;
    if (maxClientRoots == 0u || _clientDataRoots.size() >= maxClientRoots)
        return false;
    try {
        _clientDataRoots.push_back(ClientDataRootRecord{descriptor});
    } catch (const std::bad_alloc&) {
        return false;
    }
    out = descriptor;
    return true;
}

bool TopologyAuthority::clientDataRootForNode(
    const model::NodeId nodeId, const uint64_t nowMs,
    ClientDataRootDescriptor& out) const {
    out = {};
    if (!ready() || nowMs == 0u || nowMs >= _facts.expiresMs
        || nodeId == model::kInvalidNodeId) {
        return false;
    }
    const auto found = std::find_if(
        _clientDataRoots.begin(), _clientDataRoots.end(),
        [nodeId](const ClientDataRootRecord& value) {
            return value.descriptor.nodeId == nodeId;
        });
    if (found == _clientDataRoots.end()
        || !found->descriptor.registered
        || nowMs >= found->descriptor.expiresMs) {
        return false;
    }
    const EnrollmentRecord* const record = find(nodeId);
    if (record == nullptr
        || found->descriptor.expiresMs != record->expiryMs) {
        return false;
    }
    wire::ClientDataRootAssertion assertion{};
    assertion.stream = found->descriptor.stream;
    assertion.gameStartFingerprint = found->descriptor.gameStartFingerprint;
    assertion.authorityGeneration = found->descriptor.authorityGeneration;
    assertion.clientRootNodeId = found->descriptor.nodeId;
    assertion.qspRouteGeneration = found->descriptor.qspRouteGeneration;
    assertion.qsrBindingGeneration = found->descriptor.qsrBindingGeneration;
    assertion.sourceGeneration = found->descriptor.sourceGeneration;
    assertion.dataRoute = found->descriptor.dataRoute;
    assertion.qsrRoute = found->descriptor.qsrRoute;
    assertion.cacheRange = found->descriptor.cacheRange;
    assertion.cursor = found->descriptor.cursor;
    assertion.childCapacity = found->descriptor.childCapacity;
    if (!clientDataRootAssertionMatches(*record, assertion, nowMs))
        return false;
    out = found->descriptor;
    return true;
}

size_t TopologyAuthority::clientDataRootCount() const {
    return _clientDataRoots.size();
}

bool TopologyAuthority::validClientPlayerRootClaim(
    const ClientPlayerRootClaim& claim, const uint64_t nowMs) const {
    using qnetplay::config_provenance::Stage;
    if (!ready() || nowMs == 0u || nowMs >= _facts.expiresMs
        || claim.sessionId != _facts.session.sessionId
        || claim.sessionEpoch != _facts.session.epoch
        || (claim.remotePlayerRole
                != wire::ClientPlayerRootRole::RemotePlayerOne
            && claim.remotePlayerRole
                != wire::ClientPlayerRootRole::RemotePlayerTwo)
        || !sameFullCompatibility(claim.compatibility, _facts.compatibility)
        || qnetplay::config_provenance::validateCommittedWire(
               claim.configurationProvenance) != nullptr
        || claim.streamRootIdentity != _facts.streamRootIdentity
        || claim.gameStartFingerprint != _facts.gameStartFingerprint
        || claim.publisherGeneration == 0u || claim.publisherNonce == 0u
        || claim.runtimeReceipt.processId == 0u
        || claim.runtimeReceipt.publisherGeneration
            != claim.publisherGeneration
        || claim.runtimeReceipt.publisherNonce != claim.publisherNonce
        || claim.runtimeReceipt.playerSlot
            != static_cast<uint8_t>(claim.remotePlayerRole)
        || claim.runtimeReceipt.runtimeStage != Stage::RuntimeApplied
        || qnetplay::config_provenance::validateCommittedWire(
               claim.runtimeReceipt.provenance) != nullptr
        || !sameConfigurationProvenance(claim.runtimeReceipt.provenance,
                                        claim.configurationProvenance)
        || claim.sourceGeneration != _facts.sourceGeneration
        || claim.qspRouteGeneration == 0u
        || claim.qsrBindingGeneration == 0u
        || !validClientPlayerRootRoute(claim.dataRoute)
        || !validClientPlayerRootRoute(claim.qsrRoute)
        || claim.dataRoute.generation != claim.qspRouteGeneration
        || claim.qsrRoute.generation != claim.qsrBindingGeneration
        || claim.dataRoute.sameEndpoint(claim.qsrRoute)
        || claim.cacheRange.firstCursor > claim.cacheRange.nextCursor
        || claim.cacheRange.nextCursor != claim.cursor.nextRecord
        || claim.cursor.digest == 0u
        || !sameCursor(claim.cursor, _verifiedCursor)
        || !claim.cacheRange.containsBoundary(_verifiedCursor.nextRecord)
        || claim.childCapacity == 0u
        || static_cast<size_t>(claim.childCapacity)
            > _facts.limits.maxChildrenPerRoot
        || claim.expiresMs <= nowMs || claim.expiresMs > _facts.expiresMs) {
        return false;
    }
    return true;
}

bool TopologyAuthority::makeClientPlayerRootBinding(
    const ClientPlayerRootClaim& claim, const model::NodeId nodeId,
    wire::ClientPlayerRootBinding& out) const {
    out = {};
    if (!ready() || nodeId == model::kInvalidNodeId) return false;
    model::StreamCompatibilityIdentity stream{};
    if (!model::makeStreamCompatibilityIdentity(
            claim.compatibility, claim.sessionId, claim.sessionEpoch,
            claim.streamRootIdentity, stream)) {
        return false;
    }
    out.stream = stream;
    out.gameStartFingerprint = claim.gameStartFingerprint;
    out.clientRootNodeId = nodeId;
    out.remotePlayerRole = claim.remotePlayerRole;
    out.configurationProvenance = claim.configurationProvenance;
    out.runtimeReceipt = claim.runtimeReceipt;
    out.sourceGeneration = claim.sourceGeneration;
    out.qspRouteGeneration = claim.qspRouteGeneration;
    out.qsrBindingGeneration = claim.qsrBindingGeneration;
    out.dataRoute = claim.dataRoute;
    out.qsrRoute = claim.qsrRoute;
    out.cacheRange = claim.cacheRange;
    out.cursor = claim.cursor;
    out.childCapacity = claim.childCapacity;
    out.expiresMs = claim.expiresMs;
    return out.stream.valid()
        && out.gameStartFingerprint == out.stream.streamRootIdentity
        && out.dataRoute.generation == out.qspRouteGeneration
        && out.qsrRoute.generation == out.qsrBindingGeneration;
}

bool TopologyAuthority::populateClientPlayerRootEnrollment(
    const ClientPlayerRootRecord& record,
    ClientPlayerRootEnrollment& out) const {
    out = {};
    if (!ready()
        || record.binding.clientRootNodeId == model::kInvalidNodeId
        || record.grantNonce == 0u
        || allZero(record.credential.data(), record.credential.size())
        || record.grant.authorityGeneration != _facts.authorityGeneration
        || record.grant.hostRootNodeId != _facts.rootNodeId
        || record.grant.grantNonce != record.grantNonce
        || !sameClientPlayerRootBinding(record.grant.binding, record.binding)) {
        return false;
    }
    out.granted = true;
    out.clientRootNodeId = record.binding.clientRootNodeId;
    out.hostRootNodeId = _facts.rootNodeId;
    out.authorityGeneration = _facts.authorityGeneration;
    out.expiresMs = record.binding.expiresMs;
    out.grant = record.grant;
    out.credential = record.credential;
    out.rootQsp = _facts.rootQsp;
    out.rootQsr = _facts.rootQsr;
    out.hostSourceGeneration = _facts.sourceGeneration;
    out.assignmentSigningPublicKey = _facts.assignmentSigningPublicKey;
    return true;
}

bool TopologyAuthority::enrollClientPlayerRoot(
    const ClientPlayerRootClaim& claim, const uint64_t nowMs,
    ClientPlayerRootEnrollment& out, std::string& error) {
    out = {};
    error.clear();
    if (!ready() || nowMs == 0u || nowMs >= _facts.expiresMs) {
        error = "client-player-root authority is not ready";
        return false;
    }
    expire(nowMs);
    if (!validClientPlayerRootClaim(claim, nowMs)) {
        error = "client-player-root claim did not match current authority facts";
        return false;
    }

    const auto sameRole = std::find_if(
        _clientPlayerRoots.begin(), _clientPlayerRoots.end(),
        [&claim](const ClientPlayerRootRecord& value) {
            return value.binding.remotePlayerRole == claim.remotePlayerRole;
        });
    if (sameRole != _clientPlayerRoots.end()) {
        wire::ClientPlayerRootBinding expected{};
        if (makeClientPlayerRootBinding(claim,
                                        sameRole->binding.clientRootNodeId,
                                        expected)
            && sameClientPlayerRootBinding(expected, sameRole->binding)
            && sameRole->binding.expiresMs > nowMs
            && populateClientPlayerRootEnrollment(*sameRole, out)) {
            return true;
        }
        if (!releaseClientPlayerRootRecord(
                sameRole->binding.clientRootNodeId,
                wire::ClientPlayerRootRetirementReason::ClientRootBindingReplaced)) {
            error = "the prior client-player-root activation could not be retired";
            return false;
        }
    }

    const RouteEndpoint dataEndpoint = endpointFromRoute(claim.dataRoute);
    const RouteEndpoint qsrEndpoint = endpointFromRoute(claim.qsrRoute);
    if (endpointConflictsWithTopologyBinding(dataEndpoint)
        || endpointConflictsWithTopologyBinding(qsrEndpoint)) {
        error = "client-player-root routes collide with live topology bindings";
        return false;
    }
    if (_enrollments.size() + _clientPlayerRoots.size()
            + _retiredNodeIds.size() + 1u
        >= _facts.limits.maxTopologyNodes) {
        error = "client-player-root topology capacity is unavailable";
        return false;
    }

    model::NodeId nodeId = model::kInvalidNodeId;
    for (unsigned attempt = 0u; attempt < 16u; ++attempt) {
        if (!drawNonzero(nodeId)) {
            error = "the secure random provider could not allocate a client-player-root NodeId";
            return false;
        }
        if (!nodeIdInUse(nodeId)) break;
        nodeId = model::kInvalidNodeId;
    }
    if (nodeId == model::kInvalidNodeId) {
        error = "the client-player-root NodeId namespace could not allocate a unique node";
        return false;
    }

    uint64_t grantNonce = 0u;
    for (unsigned attempt = 0u; attempt < 16u; ++attempt) {
        if (!drawNonzero(grantNonce)) {
            error = "the secure random provider could not allocate a client-player-root grant";
            return false;
        }
        const bool duplicate = std::any_of(
            _clientPlayerRoots.begin(), _clientPlayerRoots.end(),
            [grantNonce](const ClientPlayerRootRecord& value) {
                return value.grantNonce == grantNonce;
            });
        if (!duplicate) break;
        grantNonce = 0u;
    }
    if (grantNonce == 0u) {
        error = "the client-player-root grant namespace was exhausted";
        return false;
    }

    ClientPlayerRootRecord record{};
    if (!makeClientPlayerRootBinding(claim, nodeId, record.binding)
        || !drawCredential(record.credential)) {
        record.credential.fill(0u);
        error = "the client-player-root binding could not be created";
        return false;
    }
    record.grantNonce = grantNonce;
    record.grant.authorityGeneration = _facts.authorityGeneration;
    record.grant.hostRootNodeId = _facts.rootNodeId;
    record.grant.grantNonce = grantNonce;
    record.grant.binding = record.binding;
    std::vector<uint8_t> attestationInput;
    const bool signedGrant = wire::encodeClientPlayerRootGrantAttestationInput(
            record.grant, attestationInput)
        && _assignmentSigner.sign(attestationInput.data(),
                                  attestationInput.size(),
                                  record.grant.hostSignature);
    wipeVector(attestationInput);
    if (!signedGrant) {
        record.credential.fill(0u);
        record.grant.hostSignature.fill(0u);
        error = "the client-player-root grant could not be attested";
        return false;
    }
    try {
        _clientPlayerRoots.push_back(record);
    } catch (const std::bad_alloc&) {
        record.credential.fill(0u);
        record.grant.hostSignature.fill(0u);
        error = "client-player-root enrollment storage was unavailable";
        return false;
    }
    if (!populateClientPlayerRootEnrollment(_clientPlayerRoots.back(), out)) {
        _clientPlayerRoots.back().credential.fill(0u);
        _clientPlayerRoots.pop_back();
        error = "the client-player-root enrollment response was incomplete";
        return false;
    }
    return true;
}

bool writeClientPlayerRootEnvelopeMacInputFromFacts(
    const TopologyAuthorityFacts& facts, const uint64_t grantNonce,
    const wire::ClientPlayerRootBinding& binding, const uint64_t sequence,
    const uint8_t* qsrBytes, const size_t qsrLength,
    std::vector<uint8_t>& out) {
    out.clear();
    if (sequence == 0u || qsrBytes == nullptr || qsrLength == 0u
        || qsrLength > wire::kMaxResilientControlDatagramBytes
        || binding.clientRootNodeId == model::kInvalidNodeId
        || grantNonce == 0u) {
        return false;
    }
    try {
        out.reserve(768u + qsrLength);
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (!appendBytes(out, reinterpret_cast<const uint8_t*>(
                         kClientPlayerRootCredentialDomain),
                     sizeof(kClientPlayerRootCredentialDomain) - 1u)) {
        return false;
    }
    appendU64(out, facts.session.sessionId);
    appendU64(out, facts.session.epoch);
    if (!appendCompatibility(out, facts.compatibility)) return false;
    appendU64(out, facts.streamRootIdentity);
    appendU64(out, facts.gameStartFingerprint);
    appendU64(out, facts.authorityGeneration);
    appendU64(out, facts.rootNodeId);
    appendEndpointBinding(out, facts.rootQsp);
    appendEndpointBinding(out, facts.rootQsr);
    appendU64(out, facts.sourceGeneration);
    if (!appendBytes(out, facts.assignmentSigningPublicKey.data(),
                     facts.assignmentSigningPublicKey.size())) {
        return false;
    }
    appendU64(out, grantNonce);
    if (!appendClientPlayerRootBinding(out, binding)) return false;
    appendU64(out, sequence);
    return appendBytes(out, qsrBytes, qsrLength);
}

bool encodeClientPlayerRootEnvelopeFromFacts(
    const TopologyAuthorityFacts& facts, const uint64_t grantNonce,
    const wire::ClientPlayerRootBinding& binding,
    const std::array<uint8_t, kTopologyCredentialBytes>& credential,
    const uint64_t sequence, const uint8_t* qsrBytes, const size_t qsrLength,
    std::vector<uint8_t>& out) {
    out.clear();
    if (allZero(credential.data(), credential.size())) return false;
    std::vector<uint8_t> macInput;
    if (!writeClientPlayerRootEnvelopeMacInputFromFacts(
            facts, grantNonce, binding, sequence, qsrBytes, qsrLength,
            macInput)) {
        return false;
    }
    std::array<uint8_t, 32> mac{};
    qnetplay::spectator::hmacSha256(credential.data(), credential.size(),
                                    macInput.data(), macInput.size(), mac);
    wipeVector(macInput);
    try {
        out.assign(kTopologyEnvelopePrefixBytes + qsrLength, 0u);
    } catch (const std::bad_alloc&) {
        return false;
    }
    writeLe32(out.data(), kTopologyControlMagic);
    writeLe16(out.data() + 4u, kTopologyControlVersion);
    out[6] = kTopologyControlType;
    out[7] = 0u;
    writeLe16(out.data() + 8u, static_cast<uint16_t>(qsrLength));
    writeLe16(out.data() + 10u, 0u);
    writeLe64(out.data() + 12u, facts.authorityGeneration);
    writeLe64(out.data() + 20u, binding.clientRootNodeId);
    writeLe64(out.data() + 28u, sequence);
    std::copy_n(mac.data(), kTopologyMacBytes, out.data() + 36u);
    std::copy_n(qsrBytes, qsrLength,
                out.data() + static_cast<std::ptrdiff_t>(
                                 kTopologyEnvelopePrefixBytes));
    mac.fill(0u);
    return true;
}

bool decodeClientPlayerRootEnvelopeFromFacts(
    const TopologyAuthorityFacts& facts, const uint64_t grantNonce,
    const wire::ClientPlayerRootBinding& binding,
    const std::array<uint8_t, kTopologyCredentialBytes>& credential,
    const uint8_t* bytes, const size_t length,
    const uint64_t minSequenceExclusive, const uint8_t*& outPayload,
    size_t& outPayloadLength, uint64_t& outSequence) {
    outPayload = nullptr;
    outPayloadLength = 0u;
    outSequence = 0u;
    size_t qsrLength = 0u;
    if (allZero(credential.data(), credential.size())
        || !validEnvelopeHeader(bytes, length, qsrLength)) {
        return false;
    }
    const uint64_t authorityGeneration = readLe64(bytes + 12u);
    const model::NodeId nodeId = readLe64(bytes + 20u);
    const uint64_t sequence = readLe64(bytes + 28u);
    if (authorityGeneration != facts.authorityGeneration
        || nodeId != binding.clientRootNodeId || sequence == 0u
        || sequence <= minSequenceExclusive) {
        return false;
    }
    const uint8_t* const qsr = bytes + kTopologyEnvelopePrefixBytes;
    std::vector<uint8_t> macInput;
    if (!writeClientPlayerRootEnvelopeMacInputFromFacts(
            facts, grantNonce, binding, sequence, qsr, qsrLength, macInput)) {
        return false;
    }
    std::array<uint8_t, 32> expected{};
    qnetplay::spectator::hmacSha256(credential.data(), credential.size(),
                                    macInput.data(), macInput.size(),
                                    expected);
    wipeVector(macInput);
    const bool authentic = equalConstantTime(bytes + 36u, expected.data(),
                                             kTopologyMacBytes);
    expected.fill(0u);
    if (!authentic) return false;
    outPayload = qsr;
    outPayloadLength = qsrLength;
    outSequence = sequence;
    return true;
}

bool TopologyAuthority::writeClientPlayerRootEnvelopeMacInput(
    const ClientPlayerRootRecord& record, const uint64_t sequence,
    const uint8_t* qsrBytes, const size_t qsrLength,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!ready()
        || allZero(record.credential.data(), record.credential.size())) {
        return false;
    }
    return writeClientPlayerRootEnvelopeMacInputFromFacts(
        _facts, record.grantNonce, record.binding, sequence, qsrBytes,
        qsrLength, out);
}

bool TopologyAuthority::encodeClientPlayerRootControlForRecord(
    const ClientPlayerRootRecord& record, const uint64_t sequence,
    const uint8_t* qsrBytes, const size_t qsrLength,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!ready() || sequence == 0u || qsrBytes == nullptr || qsrLength == 0u
        || qsrLength > wire::kMaxResilientControlDatagramBytes
        || record.binding.clientRootNodeId == model::kInvalidNodeId
        || allZero(record.credential.data(), record.credential.size())) {
        return false;
    }
    std::vector<uint8_t> macInput;
    if (!writeClientPlayerRootEnvelopeMacInput(record, sequence, qsrBytes,
                                                qsrLength, macInput)) {
        return false;
    }
    std::array<uint8_t, 32> mac{};
    qnetplay::spectator::hmacSha256(
        record.credential.data(), record.credential.size(), macInput.data(),
        macInput.size(), mac);
    try {
        out.assign(kTopologyEnvelopePrefixBytes + qsrLength, 0u);
    } catch (const std::bad_alloc&) {
        return false;
    }
    writeLe32(out.data(), kTopologyControlMagic);
    writeLe16(out.data() + 4u, kTopologyControlVersion);
    out[6] = kTopologyControlType;
    out[7] = 0u;
    writeLe16(out.data() + 8u, static_cast<uint16_t>(qsrLength));
    writeLe16(out.data() + 10u, 0u);
    writeLe64(out.data() + 12u, _facts.authorityGeneration);
    writeLe64(out.data() + 20u, record.binding.clientRootNodeId);
    writeLe64(out.data() + 28u, sequence);
    std::copy_n(mac.data(), kTopologyMacBytes, out.data() + 36u);
    std::copy_n(qsrBytes, qsrLength,
                out.data() + static_cast<std::ptrdiff_t>(
                                 kTopologyEnvelopePrefixBytes));
    return true;
}

bool TopologyAuthority::clientPlayerRootAssertionMatches(
    const ClientPlayerRootRecord& record,
    const wire::ClientPlayerRootAssertion& assertion,
    const RouteEndpoint& from, const uint64_t nowMs) const {
    return ready() && nowMs != 0u && nowMs < _facts.expiresMs
        && nowMs < record.binding.expiresMs
        && from.valid() && from == endpointFromRoute(record.binding.qsrRoute)
        && assertion.authorityGeneration == _facts.authorityGeneration
        && assertion.hostRootNodeId == _facts.rootNodeId
        && assertion.grantNonce == record.grantNonce
        && sameClientPlayerRootBinding(assertion.binding, record.binding)
        && sameCursor(record.binding.cursor, _verifiedCursor)
        && record.binding.cacheRange.containsBoundary(
            _verifiedCursor.nextRecord);
}

bool TopologyAuthority::acceptClientPlayerRootAssertion(
    ClientPlayerRootRecord& record,
    const wire::ClientPlayerRootAssertion& assertion,
    const RouteEndpoint& from, const uint64_t nowMs) {
    if (!clientPlayerRootAssertionMatches(record, assertion, from, nowMs))
        return false;
    record.asserted = true;
    return true;
}

bool TopologyAuthority::encodeClientPlayerRootControl(
    const ClientPlayerRootEnrollment& enrollment, const uint64_t sequence,
    const uint8_t* qsrBytes, const size_t qsrLength,
    std::vector<uint8_t>& out) const {
    out.clear();
    const ClientPlayerRootRecord* const record =
        findClientPlayerRoot(enrollment.clientRootNodeId);
    if (!ready() || record == nullptr || !enrollment.granted
        || enrollment.hostRootNodeId != _facts.rootNodeId
        || enrollment.authorityGeneration != _facts.authorityGeneration
        || enrollment.expiresMs != record->binding.expiresMs
        || enrollment.grant.grantNonce != record->grantNonce
        || !sameClientPlayerRootBinding(enrollment.grant.binding,
                                        record->binding)
        || enrollment.grant.hostSignature != record->grant.hostSignature
        || sequence == 0u || qsrBytes == nullptr || qsrLength == 0u
        || qsrLength > wire::kMaxResilientControlDatagramBytes
        || !equalConstantTime(record->credential.data(),
                              enrollment.credential.data(),
                              record->credential.size())) {
        return false;
    }
    wire::ControlDatagram decoded{};
    if (wire::decodeControlDatagram(qsrBytes, qsrLength, decoded)
            != wire::DecodeResult::Ok
        || decoded.type != wire::ControlMessageType::ClientPlayerRootAssertion
        || decoded.clientPlayerRootAssertion.authorityGeneration
            != _facts.authorityGeneration
        || decoded.clientPlayerRootAssertion.hostRootNodeId
            != _facts.rootNodeId
        || decoded.clientPlayerRootAssertion.grantNonce != record->grantNonce
        || !sameClientPlayerRootBinding(
            decoded.clientPlayerRootAssertion.binding, record->binding)) {
        return false;
    }
    return encodeClientPlayerRootControlForRecord(*record, sequence, qsrBytes,
                                                  qsrLength, out);
}

TopologyControlResult TopologyAuthority::decodeClientPlayerRootControl(
    const RouteEndpoint& from, const uint8_t* bytes, const size_t length,
    const uint64_t nowMs, TopologyControlEnvelope& out) {
    out = {};
    if (!ready()) return TopologyControlResult::NotReady;
    if (nowMs == 0u || nowMs >= _facts.expiresMs)
        return TopologyControlResult::Expired;
    size_t qsrLength = 0u;
    if (!validEnvelopeHeader(bytes, length, qsrLength))
        return TopologyControlResult::Malformed;
    const uint64_t authorityGeneration = readLe64(bytes + 12u);
    const model::NodeId nodeId = readLe64(bytes + 20u);
    const uint64_t sequence = readLe64(bytes + 28u);
    if (authorityGeneration != _facts.authorityGeneration)
        return TopologyControlResult::AuthorityMismatch;
    ClientPlayerRootRecord* const record = findClientPlayerRootMutable(nodeId);
    if (record == nullptr) return TopologyControlResult::UnknownNode;
    if (nowMs >= record->binding.expiresMs)
        return TopologyControlResult::Expired;
    if (!from.valid() || from != endpointFromRoute(record->binding.qsrRoute))
        return TopologyControlResult::EndpointMismatch;
    if (sequence == 0u || sequence <= record->lastInboundSequence)
        return TopologyControlResult::Replay;
    const uint8_t* const qsr = bytes + kTopologyEnvelopePrefixBytes;
    std::vector<uint8_t> macInput;
    if (!writeClientPlayerRootEnvelopeMacInput(*record, sequence, qsr,
                                               qsrLength, macInput)) {
        return TopologyControlResult::AuthenticationRejected;
    }
    std::array<uint8_t, 32> expected{};
    qnetplay::spectator::hmacSha256(
        record->credential.data(), record->credential.size(), macInput.data(),
        macInput.size(), expected);
    if (!equalConstantTime(bytes + 36u, expected.data(), kTopologyMacBytes))
        return TopologyControlResult::AuthenticationRejected;
    wire::ControlDatagram datagram{};
    if (wire::decodeControlDatagram(qsr, qsrLength, datagram)
            != wire::DecodeResult::Ok) {
        return TopologyControlResult::Malformed;
    }
    if (datagram.type != wire::ControlMessageType::ClientPlayerRootAssertion
        || !acceptClientPlayerRootAssertion(
            *record, datagram.clientPlayerRootAssertion, from, nowMs)) {
        return TopologyControlResult::BindingRejected;
    }
    try {
        out.qsr.assign(qsr, qsr + qsrLength);
    } catch (const std::bad_alloc&) {
        return TopologyControlResult::Malformed;
    }
    record->lastInboundSequence = sequence;
    out.authorityGeneration = authorityGeneration;
    out.nodeId = nodeId;
    out.sequence = sequence;
    std::copy_n(bytes + 36u, kTopologyMacBytes, out.mac.data());
    return TopologyControlResult::Accepted;
}

bool TopologyAuthority::clientPlayerRootForNode(
    const model::NodeId nodeId, const uint64_t nowMs,
    ClientPlayerRootDescriptor& out) const {
    out = {};
    const ClientPlayerRootRecord* const record = findClientPlayerRoot(nodeId);
    if (!ready() || nowMs == 0u || nowMs >= _facts.expiresMs
        || record == nullptr || nowMs >= record->binding.expiresMs
        || !sameCursor(record->binding.cursor, _verifiedCursor)
        || !record->binding.cacheRange.containsBoundary(
            _verifiedCursor.nextRecord)) {
        return false;
    }
    out.enrolled = true;
    out.asserted = record->asserted;
    out.grantNonce = record->grantNonce;
    out.binding = record->binding;
    return true;
}

size_t TopologyAuthority::clientPlayerRootCount() const {
    return _clientPlayerRoots.size();
}

std::string TopologyAuthority::diagnostics() const {
    if (!ready()) return "topology=disabled";
    return "topology=ready authority="
        + std::to_string(_facts.authorityGeneration)
        + " root=" + std::to_string(_facts.rootNodeId)
        + " enrolled=" + std::to_string(_enrollments.size())
        + " client-data-roots=" + std::to_string(_clientDataRoots.size())
        + " client-player-roots="
        + std::to_string(_clientPlayerRoots.size())
        + " reserved=" + std::to_string(reservedCount());
}

bool TopologyAuthority::makeChildAdvertisement(
    const EnrollmentRecord& record, const RouteEndpoint& qsrEndpoint,
    model::NodeAdvertisement& out) const {
    out = {};
    const bool relayRecord = relayProvenance(record.qspProvenance);
    const bool virtualQsr =
        qsrEndpoint.ipv4BE == qnetplay::spectator::kRelayVirtualIpv4BE;
    if (!ready() || !qsrEndpoint.valid()
        || qsrEndpoint.port == record.qspEndpoint.port
        || relayRecord != virtualQsr) {
        return false;
    }
    model::StreamCompatibilityIdentity stream{};
    if (!model::makeStreamCompatibilityIdentity(
            _facts.compatibility, _facts.session.sessionId,
            _facts.session.epoch, _facts.streamRootIdentity, stream)) {
        return false;
    }
    out.nodeId = record.nodeId;
    out.kind = model::NodeKind::SpectatorRelay;
    out.stream = stream;
    out.route.kind = relayRecord
        ? model::RouteKind::TrafficRelay
        : (record.qspProvenance == EndpointProvenance::ProvenPunchedQsp
               ? model::RouteKind::Punched : model::RouteKind::Direct);
    out.route.ipv4BE = record.qspEndpoint.ipv4BE;
    out.route.port = record.qspEndpoint.port;
    out.route.generation = record.routeGeneration;
    out.route.routeProved = true;
    out.controlRoute.kind = relayRecord ? model::RouteKind::TrafficRelay
                                        : model::RouteKind::Direct;
    out.controlRoute.ipv4BE = qsrEndpoint.ipv4BE;
    out.controlRoute.port = qsrEndpoint.port;
    out.controlRoute.generation = record.routeGeneration;
    out.controlRoute.routeProved = true;
    out.capabilityModelRevision = model::kResilientCapabilityModelRevision;
    out.capabilities = model::kCapabilityRelayCapableSpectator
        | model::kCapabilityTopologyLeaf;
    out.childCapacity = 0u;
    out.childCount = 0u;
    out.uploadCapacity = 1u;
    out.uploadInUse = 0u;
    out.cacheFirstCursor = record.qtaCursor.nextRecord;
    out.cacheNextCursor = record.qtaCursor.nextRecord;
    out.sourceGeneration = _facts.sourceGeneration;
    out.routeGeneration = record.routeGeneration;
    out.lastProgressMs = _facts.createdMs;
    out.leaseExpiryMs = record.expiryMs;
    out.reachable = true;
    out.draining = false;
    return true;
}

bool TopologyAuthority::makeParentLeaseChildBinding(
    const EnrollmentRecord& record,
    model::ParentLeaseChildBinding& out) const {
    out = {};
    if (!ready() || record.nodeId == model::kInvalidNodeId
        || record.viewerId == 0u || record.viewerGeneration == 0u
        || !record.qspEndpoint.valid() || !validRouteProvenance(record.qspProvenance)
        || record.routeGeneration == 0u || record.bindingGeneration == 0u) {
        return false;
    }
    out.authorityGeneration = _facts.authorityGeneration;
    out.childViewerId = record.viewerId;
    out.childViewerGeneration = record.viewerGeneration;
    out.childQspRoute.kind =
        record.qspProvenance == EndpointProvenance::ProvenRelayQsp
            ? model::RouteKind::TrafficRelay
            : (record.qspProvenance == EndpointProvenance::ProvenPunchedQsp
                   ? model::RouteKind::Punched
                   : model::RouteKind::Direct);
    out.childQspRoute.ipv4BE = record.qspEndpoint.ipv4BE;
    out.childQspRoute.port = record.qspEndpoint.port;
    out.childQspRoute.generation = record.routeGeneration;
    out.childQspRoute.routeProved = true;
    out.childEndpointBindingGeneration = record.bindingGeneration;
    out.childRequestedCursor = record.qtaCursor;
    return out.valid();
}

bool TopologyAuthority::acceptRelayDataBinding(
    EnrollmentRecord& record,
    const wire::RelayDataBindingRequest& request,
    const RouteEndpoint& observedDataEndpoint, const uint64_t nowMs) {
    model::StreamCompatibilityIdentity stream{};
    if (!ready() || nowMs >= _facts.expiresMs || nowMs >= record.expiryMs
        || !record.qspStreaming || !record.qsrBound || !record.attached
        || !record.qspEndpoint.valid() || !record.qsrEndpoint.valid()
        || !observedDataEndpoint.valid()
        || (!record.relayDataBound
            && endpointConflictsWithTopologyBinding(observedDataEndpoint,
                                                    record.nodeId))
        || observedDataEndpoint.port == record.qspEndpoint.port
        || observedDataEndpoint.port == record.qsrEndpoint.port
        || (observedDataEndpoint.ipv4BE
                == _facts.rootQsp.advertised.ipv4BE
            && observedDataEndpoint.port == _facts.rootQsp.advertised.port)
        || (observedDataEndpoint.ipv4BE
                == _facts.rootQsr.advertised.ipv4BE
            && observedDataEndpoint.port == _facts.rootQsr.advertised.port)
        || !model::makeStreamCompatibilityIdentity(
            _facts.compatibility, _facts.session.sessionId,
            _facts.session.epoch, _facts.streamRootIdentity, stream)
        || request.stream != stream
        || request.authorityGeneration != _facts.authorityGeneration
        || request.rootNodeId != _facts.rootNodeId
        || request.childNodeId != record.nodeId
        || request.requestNonce == 0u
        || !sameLeaseBinding(request.lease, record.attachedLease)
        || request.lease.childBinding.authorityGeneration
            != _facts.authorityGeneration) {
        return false;
    }

    const EnrollmentRecord* const qspCollision =
        findByQspEndpoint(observedDataEndpoint);
    const EnrollmentRecord* const qsrCollision =
        findByQsrEndpoint(observedDataEndpoint);
    const EnrollmentRecord* const dataCollision =
        findByRelayDataEndpoint(observedDataEndpoint);
    if ((qspCollision != nullptr && qspCollision->nodeId != record.nodeId)
        || (qsrCollision != nullptr && qsrCollision->nodeId != record.nodeId)
        || (dataCollision != nullptr && dataCollision->nodeId != record.nodeId)) {
        return false;
    }

    if (record.relayDataBound) {
        return record.relayDataEndpoint == observedDataEndpoint
            && record.relayDataRequestNonce == request.requestNonce
            && record.relayDataBindingGeneration == record.bindingGeneration;
    }

    if (record.bindingGeneration == 0u
        || (record.attachmentOwner != AttachmentOwner::RelayActivation
            && !releaseRelayAssignmentsForNode(
                record.nodeId,
                wire::RelayChildRetirementReason::ChildBindingReplaced))) {
        return false;
    }
    releaseClientDataRootForNode(record.nodeId);
    record.relayDataEndpoint = observedDataEndpoint;
    record.relayDataBound = true;
    record.relayDataRequestNonce = request.requestNonce;
    record.relayDataBindingGeneration = record.bindingGeneration;
    record.relayReady = false;
    record.relayReadyCacheRange = {};
    record.relayReadyCursor = {};
    return true;
}

bool TopologyAuthority::makeRelayDataBindingAck(
    const EnrollmentRecord& record, wire::RelayDataBindingAck& out) const {
    out = {};
    model::StreamCompatibilityIdentity stream{};
    if (!ready() || !record.attached || !record.relayDataBound
        || !record.qsrBound || !record.relayDataEndpoint.valid()
        || !record.qsrEndpoint.valid()
        || record.relayDataEndpoint.port == record.qspEndpoint.port
        || record.relayDataEndpoint.port == record.qsrEndpoint.port
        || record.relayDataRequestNonce == 0u
        || record.relayDataBindingGeneration == 0u
        || !model::makeStreamCompatibilityIdentity(
            _facts.compatibility, _facts.session.sessionId,
            _facts.session.epoch, _facts.streamRootIdentity, stream)) {
        return false;
    }
    out.stream = stream;
    out.authorityGeneration = _facts.authorityGeneration;
    out.rootNodeId = _facts.rootNodeId;
    out.childNodeId = record.nodeId;
    out.lease = record.attachedLease;
    out.requestNonce = record.relayDataRequestNonce;
    out.observedDataRoute.kind = model::RouteKind::Direct;
    out.observedDataRoute.ipv4BE = record.relayDataEndpoint.ipv4BE;
    out.observedDataRoute.port = record.relayDataEndpoint.port;
    out.observedDataRoute.generation = record.relayDataBindingGeneration;
    out.observedDataRoute.routeProved = true;
    out.observedControlRoute.kind = model::RouteKind::Direct;
    out.observedControlRoute.ipv4BE = record.qsrEndpoint.ipv4BE;
    out.observedControlRoute.port = record.qsrEndpoint.port;
    out.observedControlRoute.generation = record.relayDataBindingGeneration;
    out.observedControlRoute.routeProved = true;
    return true;
}

bool TopologyAuthority::relayDataBindingAck(
    const model::NodeId nodeId,
    const wire::RelayDataBindingRequest& request,
    wire::RelayDataBindingAck& out) const {
    out = {};
    const EnrollmentRecord* const record = find(nodeId);
    if (record == nullptr || request.childNodeId != nodeId
        || request.requestNonce != record->relayDataRequestNonce
        || !sameLeaseBinding(request.lease, record->attachedLease)
        || !makeRelayDataBindingAck(*record, out)) {
        out = {};
        return false;
    }
    const bool exact = request.stream == out.stream
        && request.authorityGeneration == out.authorityGeneration
        && request.rootNodeId == out.rootNodeId
        && request.childNodeId == out.childNodeId;
    if (!exact) out = {};
    return exact;
}

bool TopologyAuthority::relayReadyMatches(
    const EnrollmentRecord& record, const wire::RelayReady& ready,
    const uint64_t nowMs) const {
    wire::RelayDataBindingAck expected{};
    if (!this->ready() || nowMs >= _facts.expiresMs || nowMs >= record.expiryMs
        || !record.qspStreaming || !record.qsrBound || !record.attached
        || !record.relayDataBound
        || !makeRelayDataBindingAck(record, expected)) {
        return false;
    }
    return ready.stream == expected.stream
        && ready.authorityGeneration == expected.authorityGeneration
        && ready.rootNodeId == expected.rootNodeId
        && ready.relayNodeId == expected.childNodeId
        && sameLeaseBinding(ready.relayLease, expected.lease)
        && ready.bindingRequestNonce == expected.requestNonce
        && sameRoute(ready.observedDataRoute, expected.observedDataRoute)
        && sameRoute(ready.observedControlRoute,
                     expected.observedControlRoute)
        && ready.cacheRange.firstCursor <= ready.cacheRange.nextCursor
        && ready.cacheRange.containsBoundary(record.qtaCursor.nextRecord)
        && ready.cacheRange.containsBoundary(_verifiedCursor.nextRecord)
        && ready.cacheRange.nextCursor == _verifiedCursor.nextRecord
        && sameCursor(ready.cacheCursor, _verifiedCursor);
}

bool TopologyAuthority::acceptRelayReady(
    EnrollmentRecord& record, const wire::RelayReady& ready,
    const uint64_t nowMs) {
    if (!relayReadyMatches(record, ready, nowMs)) return false;
    record.relayReady = true;
    record.relayReadyCacheRange = ready.cacheRange;
    record.relayReadyCursor = ready.cacheCursor;
    return true;
}

bool TopologyAuthority::clientDataRootAssertionMatches(
    const EnrollmentRecord& record,
    const wire::ClientDataRootAssertion& assertion,
    const uint64_t nowMs) const {
    wire::RelayDataBindingAck expectedBinding{};
    model::StreamCompatibilityIdentity expectedStream{};
    if (!ready() || nowMs == 0u || nowMs >= _facts.expiresMs
        || nowMs >= record.expiryMs || !record.qspStreaming
        || !record.attached || !record.qsrBound || !record.relayDataBound
        || !record.relayReady || !record.qsrEndpoint.valid()
        || !record.relayDataEndpoint.valid()
        || !model::makeStreamCompatibilityIdentity(
            _facts.compatibility, _facts.session.sessionId,
            _facts.session.epoch, _facts.streamRootIdentity, expectedStream)
        || !makeRelayDataBindingAck(record, expectedBinding)) {
        return false;
    }

    return sameFullStream(assertion.stream, expectedStream)
        && assertion.gameStartFingerprint == _facts.gameStartFingerprint
        && assertion.authorityGeneration == _facts.authorityGeneration
        && assertion.clientRootNodeId == record.nodeId
        && assertion.qspRouteGeneration == record.routeGeneration
        && assertion.qsrBindingGeneration == record.bindingGeneration
        && assertion.sourceGeneration == _facts.sourceGeneration
        && sameRoute(assertion.dataRoute, expectedBinding.observedDataRoute)
        && sameRoute(assertion.qsrRoute, expectedBinding.observedControlRoute)
        && sameCacheRange(assertion.cacheRange, record.relayReadyCacheRange)
        && sameCursor(assertion.cursor, record.relayReadyCursor)
        && sameCursor(assertion.cursor, _verifiedCursor)
        && assertion.childCapacity != 0u
        && assertion.childCapacity <= _facts.limits.maxChildrenPerRoot;
}

void TopologyAuthority::releaseClientDataRootForNode(
    const model::NodeId nodeId) {
    _clientDataRoots.erase(
        std::remove_if(_clientDataRoots.begin(), _clientDataRoots.end(),
                       [nodeId](const ClientDataRootRecord& value) {
                           return value.descriptor.nodeId == nodeId;
                       }),
        _clientDataRoots.end());
}

void TopologyAuthority::releaseAllClientDataRoots() {
    _clientDataRoots.clear();
}

bool TopologyAuthority::relayAssignmentNonceInUse(const uint64_t nonce) const {
    if (nonce == 0u) return true;
    const bool offerCollision = std::any_of(
        _offers.begin(), _offers.end(), [nonce](const OfferReservation& offer) {
            return offer.nonce == nonce;
        });
    const bool assignmentCollision = std::any_of(
        _relayAssignments.begin(), _relayAssignments.end(),
        [nonce](const RelayAssignmentReservation& assignment) {
            return assignment.childLease.replayNonce == nonce;
        });
    const bool activationCollision = std::any_of(
        _relayActivations.begin(), _relayActivations.end(),
        [nonce](const RelayActivationReservation& activation) {
            return activation.activation.activationNonce == nonce
                || activation.activation.childLease.replayNonce == nonce;
        });
    const bool clientRootActivationCollision = std::any_of(
        _clientPlayerRootActivations.begin(),
        _clientPlayerRootActivations.end(),
        [nonce](const ClientPlayerRootActivationReservation& activation) {
            return activation.activation.activationNonce == nonce
                || activation.activation.targetLease.replayNonce == nonce;
        });
    return offerCollision || assignmentCollision || activationCollision
        || clientRootActivationCollision;
}

bool TopologyAuthority::relayActivationNonceInUse(const uint64_t nonce) const {
    return relayAssignmentNonceInUse(nonce);
}

bool TopologyAuthority::activationMatchesCurrentBindings(
    const RelayActivationReservation& reservation,
    const EnrollmentRecord& relay, const EnrollmentRecord& child,
    const uint64_t nowMs) const {
    const wire::RelayChildActivation& activation = reservation.activation;
    wire::RelayChildActivationReceipt structuralReceipt{};
    if (!ready() || nowMs >= _facts.expiresMs
        || nowMs >= activation.activationDeadlineMs
        || relay.nodeId != activation.relayNodeId
        || child.nodeId != activation.childNodeId
        || relay.nodeId == child.nodeId
        || nowMs >= relay.expiryMs || nowMs >= child.expiryMs
        || !relay.qspStreaming || !relay.qsrBound || !relay.attached
        || relay.attachmentOwner == AttachmentOwner::None
        || !relay.relayDataBound || !relay.relayReady
        || relay.attachedDepth == 0u
        || relay.attachedDepth >= _facts.limits.maxActiveDepth
        || relay.attachedAncestryCount == 0u
        || relay.attachedAncestryCount >= relay.attachedAncestry.size()
        || !child.qspStreaming || !child.qsrBound || child.attached
        || !wire::makeRelayChildActivationReceipt(
            activation, wire::RelayChildActivationReceiptRole::Relay,
            structuralReceipt)) {
        return false;
    }
    wire::RelayDataBindingAck relayBinding{};
    model::ParentLeaseChildBinding childBinding{};
    if (!makeRelayDataBindingAck(relay, relayBinding)
        || !makeParentLeaseChildBinding(child, childBinding)) {
        return false;
    }
    model::RouteDescriptor childControlRoute{};
    childControlRoute.kind = model::RouteKind::Direct;
    childControlRoute.ipv4BE = child.qsrEndpoint.ipv4BE;
    childControlRoute.port = child.qsrEndpoint.port;
    childControlRoute.generation = child.bindingGeneration;
    childControlRoute.routeProved = true;
    std::array<model::NodeId, model::kMaxLeaseAncestry> childAncestry =
        relay.attachedAncestry;
    childAncestry[relay.attachedAncestryCount] = relay.nodeId;
    const uint8_t childAncestryCount = static_cast<uint8_t>(
        relay.attachedAncestryCount + 1u);
    return activation.stream == relayBinding.stream
        && activation.authorityGeneration == _facts.authorityGeneration
        && activation.rootNodeId == _facts.rootNodeId
        && sameLeaseBinding(activation.relayLease, relayBinding.lease)
        && activation.relayBindingRequestNonce == relayBinding.requestNonce
        && sameRoute(activation.relayDataRoute,
                     relayBinding.observedDataRoute)
        && sameRoute(activation.relayControlRoute,
                     relayBinding.observedControlRoute)
        && sameRoute(activation.childDataRoute,
                     childBinding.childQspRoute)
        && sameRoute(activation.childControlRoute, childControlRoute)
        && activation.childLease.childBinding == childBinding
        && activation.childLease.parentDepth == relay.attachedDepth
        && sameAncestry(activation.childLease.ancestry,
                        activation.childLease.ancestryCount, childAncestry,
                        childAncestryCount)
        && sameCursor(activation.commonCursor, relay.relayReadyCursor)
        && sameCacheRange(activation.relayCacheRange,
                          relay.relayReadyCacheRange)
        && sameCursor(activation.commonCursor, _verifiedCursor)
        && activation.activationDeadlineMs <= relay.expiryMs
        && activation.activationDeadlineMs <= child.expiryMs
        && activation.activationDeadlineMs <= relay.attachedLease.expiryMs;
}

bool TopologyAuthority::activationReceiptMatches(
    const RelayActivationReservation& reservation,
    const EnrollmentRecord& sender,
    const wire::RelayChildActivationReceipt& receipt,
    const uint64_t nowMs) const {
    const wire::RelayChildActivation& activation = reservation.activation;
    const EnrollmentRecord* const relay = find(activation.relayNodeId);
    const EnrollmentRecord* const child = find(activation.childNodeId);
    if (relay == nullptr || child == nullptr
        || !activationMatchesCurrentBindings(reservation, *relay, *child,
                                             nowMs)
        || !receiptMatchesActivation(receipt, activation)) {
        return false;
    }
    if (receipt.role == wire::RelayChildActivationReceiptRole::Relay) {
        return sender.nodeId == activation.relayNodeId
            && sender.qsrBound
            && sender.qsrEndpoint.ipv4BE == activation.relayControlRoute.ipv4BE
            && sender.qsrEndpoint.port == activation.relayControlRoute.port;
    }
    if (receipt.role == wire::RelayChildActivationReceiptRole::Child) {
        return sender.nodeId == activation.childNodeId
            && sender.qsrBound
            && sender.qsrEndpoint.ipv4BE == activation.childControlRoute.ipv4BE
            && sender.qsrEndpoint.port == activation.childControlRoute.port;
    }
    return false;
}

bool TopologyAuthority::relayActivationStageMatchesCurrentBindings(
    const RelayActivationStage& stage, const EnrollmentRecord& relay,
    const EnrollmentRecord& child, const uint64_t nowMs,
    const bool requireChildQsrBound) const {
    if (!ready() || nowMs >= _facts.expiresMs || stage.expiryMs <= nowMs
        || stage.relayNodeId == model::kInvalidNodeId
        || stage.childNodeId == model::kInvalidNodeId
        || stage.relayNodeId == stage.childNodeId
        || relay.nodeId != stage.relayNodeId
        || child.nodeId != stage.childNodeId
        || nowMs >= relay.expiryMs || nowMs >= child.expiryMs
        || !relay.qspStreaming || !relay.qsrBound || !relay.attached
        || !relay.relayDataBound || !relay.relayReady
        || !child.qspStreaming || child.qsrBound != requireChildQsrBound
        || child.attached) {
        return false;
    }
    return sameLeaseBinding(relay.attachedLease, stage.relayLease)
        && relay.qsrEndpoint == stage.relayQsrEndpoint
        && relay.relayDataEndpoint == stage.relayDataEndpoint
        && relay.bindingGeneration == stage.relayBindingGeneration
        && relay.relayDataBindingGeneration == stage.relayBindingGeneration
        && relay.relayDataRequestNonce == stage.relayDataRequestNonce
        && sameCacheRange(relay.relayReadyCacheRange,
                          stage.relayReadyCacheRange)
        && sameCursor(relay.relayReadyCursor, stage.relayReadyCursor)
        && child.viewerId == stage.childViewerId
        && child.viewerGeneration == stage.childViewerGeneration
        && child.qspEndpoint == stage.childQspEndpoint
        && child.qspProvenance == stage.childQspProvenance
        && child.routeGeneration == stage.childRouteGeneration
        && child.bindingGeneration == stage.childBindingGeneration
        && sameCursor(child.qtaCursor, stage.childQtaCursor)
        && stage.expiryMs <= relay.expiryMs
        && stage.expiryMs <= child.expiryMs
        && stage.expiryMs <= relay.attachedLease.expiryMs;
}

void TopologyAuthority::discardRelayActivationStagesForNode(
    const model::NodeId nodeId) {
    if (nodeId == model::kInvalidNodeId) return;
    const auto retained = std::remove_if(
        _relayActivationStages.begin(), _relayActivationStages.end(),
        [nodeId](const RelayActivationStage& stage) {
            return stage.relayNodeId == nodeId || stage.childNodeId == nodeId;
        });
    _relayActivationStages.erase(retained, _relayActivationStages.end());
}

void TopologyAuthority::discardAllRelayActivationStages() {
    _relayActivationStages.clear();
}

bool TopologyAuthority::stageRelayChildActivation(
    const model::NodeId relayNodeId, const model::NodeId childNodeId,
    const uint64_t nowMs) {
    if (!ready() || nowMs >= _facts.expiresMs
        || relayNodeId == model::kInvalidNodeId
        || childNodeId == model::kInvalidNodeId
        || relayNodeId == childNodeId || relayNodeId == _facts.rootNodeId
        || childNodeId == _facts.rootNodeId) {
        return false;
    }
    expire(nowMs);

    EnrollmentRecord* const relay = findMutable(relayNodeId);
    EnrollmentRecord* const child = findMutable(childNodeId);
    if (relay == nullptr || child == nullptr) return false;

    const auto existing = std::find_if(
        _relayActivationStages.begin(), _relayActivationStages.end(),
        [relayNodeId, childNodeId](const RelayActivationStage& stage) {
            return stage.relayNodeId == relayNodeId
                || stage.childNodeId == childNodeId
                || stage.relayNodeId == childNodeId
                || stage.childNodeId == relayNodeId;
        });
    if (existing != _relayActivationStages.end()) {
        if (existing->relayNodeId == relayNodeId
            && existing->childNodeId == childNodeId
            && relayActivationStageMatchesCurrentBindings(
                *existing, *relay, *child, nowMs, false)) {
            return true;
        }
        _relayActivationStages.erase(existing);
    }

    if (child->qsrBound || child->attached || !child->qspStreaming
        || !relay->qspStreaming || !relay->qsrBound || !relay->attached
        || !relay->relayDataBound || !relay->relayReady
        || std::any_of(_offers.begin(), _offers.end(),
                       [childNodeId](const OfferReservation& offer) {
                           return offer.childId == childNodeId
                               && !offer.redeemed;
                       })
        || std::any_of(_relayAssignments.begin(), _relayAssignments.end(),
                       [relayNodeId, childNodeId](
                           const RelayAssignmentReservation& item) {
                           return item.relayNodeId == relayNodeId
                               || item.childNodeId == childNodeId;
                       })
        || std::any_of(_relayActivations.begin(), _relayActivations.end(),
                       [relayNodeId, childNodeId](
                           const RelayActivationReservation& item) {
                           return item.activation.relayNodeId == relayNodeId
                               || item.activation.childNodeId == childNodeId;
                       })) {
        return false;
    }

    const size_t activeChildren = static_cast<size_t>(std::count_if(
        _relayAssignments.begin(), _relayAssignments.end(),
        [relayNodeId](const RelayAssignmentReservation& item) {
            return item.relayNodeId == relayNodeId;
        })) + static_cast<size_t>(std::count_if(
        _relayActivations.begin(), _relayActivations.end(),
        [relayNodeId](const RelayActivationReservation& item) {
            return item.activation.relayNodeId == relayNodeId;
        }));
    if (activeChildren >= _facts.limits.maxChildrenPerRelay) return false;

    RelayActivationStage stage{};
    stage.relayNodeId = relayNodeId;
    stage.childNodeId = childNodeId;
    stage.relayLease = relay->attachedLease;
    stage.relayQsrEndpoint = relay->qsrEndpoint;
    stage.relayDataEndpoint = relay->relayDataEndpoint;
    stage.relayBindingGeneration = relay->bindingGeneration;
    stage.relayDataRequestNonce = relay->relayDataRequestNonce;
    stage.relayReadyCacheRange = relay->relayReadyCacheRange;
    stage.relayReadyCursor = relay->relayReadyCursor;
    stage.childViewerId = child->viewerId;
    stage.childViewerGeneration = child->viewerGeneration;
    stage.childQspEndpoint = child->qspEndpoint;
    stage.childQspProvenance = child->qspProvenance;
    stage.childRouteGeneration = child->routeGeneration;
    stage.childBindingGeneration = child->bindingGeneration;
    stage.childQtaCursor = child->qtaCursor;
    stage.expiryMs = nextNonzero(nowMs, _facts.limits.standbyLeaseMs);
    stage.expiryMs = (std::min)(stage.expiryMs, relay->expiryMs);
    stage.expiryMs = (std::min)(stage.expiryMs, child->expiryMs);
    stage.expiryMs = (std::min)(stage.expiryMs, relay->attachedLease.expiryMs);
    stage.expiryMs = (std::min)(stage.expiryMs, _facts.expiresMs);
    if (!relayActivationStageMatchesCurrentBindings(stage, *relay, *child,
                                                    nowMs, false)
        || _relayActivationStages.size() >= _facts.limits.maxTopologyNodes) {
        return false;
    }
    try {
        _relayActivationStages.push_back(stage);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

StagedRelayChildActivationResult
TopologyAuthority::consumeStagedRelayChildActivation(
    const model::NodeId childNodeId, const uint64_t nowMs,
    model::NodeId& outRelayNodeId) {
    outRelayNodeId = model::kInvalidNodeId;
    if (childNodeId == model::kInvalidNodeId || !ready()
        || nowMs >= _facts.expiresMs) {
        return StagedRelayChildActivationResult::None;
    }
    expire(nowMs);
    const auto found = std::find_if(
        _relayActivationStages.begin(), _relayActivationStages.end(),
        [childNodeId](const RelayActivationStage& stage) {
            return stage.childNodeId == childNodeId;
        });
    if (found == _relayActivationStages.end()) {
        return StagedRelayChildActivationResult::None;
    }
    const RelayActivationStage stage = *found;
    _relayActivationStages.erase(found);

    const EnrollmentRecord* const relay = find(stage.relayNodeId);
    const EnrollmentRecord* const child = find(stage.childNodeId);
    if (relay == nullptr || child == nullptr
        || !relayActivationStageMatchesCurrentBindings(
            stage, *relay, *child, nowMs, true)
        || std::any_of(_offers.begin(), _offers.end(),
                       [childNodeId](const OfferReservation& offer) {
                           return offer.childId == childNodeId
                               && !offer.redeemed;
                       })) {
        return StagedRelayChildActivationResult::Cancelled;
    }
    outRelayNodeId = stage.relayNodeId;
    return StagedRelayChildActivationResult::Ready;
}

bool TopologyAuthority::acceptRelayChildActivationReceipt(
    EnrollmentRecord& sender,
    const wire::RelayChildActivationReceipt& receipt,
    const uint64_t nowMs) {
    const auto found = std::find_if(
        _relayActivations.begin(), _relayActivations.end(),
        [&receipt](const RelayActivationReservation& reservation) {
            return reservation.activation.activationNonce
                == receipt.activationNonce;
        });
    if (found == _relayActivations.end()
        || !activationReceiptMatches(*found, sender, receipt, nowMs)) {
        return false;
    }
    if (receipt.role == wire::RelayChildActivationReceiptRole::Relay) {
        found->relayReceipt = true;
    } else if (receipt.role == wire::RelayChildActivationReceiptRole::Child) {
        found->childReceipt = true;
    } else {
        return false;
    }
    return (!found->relayReceipt || !found->childReceipt)
        || (ensureRelayChildActivationCommit(*found)
            && queueRelayChildActivationCommit(*found));
}

bool TopologyAuthority::ensureRelayChildActivationCommit(
    RelayActivationReservation& reservation) {
    if (reservation.commitCreated) {
        return wire::validRelayChildActivationCommit(reservation.commit)
            && sameRelayChildActivation(reservation.commit.activation,
                                        reservation.activation);
    }
    if (!ready() || !reservation.relayReceipt || !reservation.childReceipt) {
        return false;
    }
    wire::RelayChildActivationCommit commit{};
    commit.activation = reservation.activation;
    std::vector<uint8_t> attestationInput;
    const bool signedCommit = wire::encodeRelayChildActivationCommitAttestationInput(
            commit, attestationInput)
        && _assignmentSigner.sign(attestationInput.data(),
                                  attestationInput.size(),
                                  commit.hostSignature);
    wipeVector(attestationInput);
    if (!signedCommit || !wire::validRelayChildActivationCommit(commit)) {
        commit.hostSignature.fill(0u);
        return false;
    }
    reservation.commit = commit;
    reservation.commitCreated = true;
    return true;
}

bool TopologyAuthority::queueRelayChildActivationCommit(
    const RelayActivationReservation& reservation) {
    if (!ready() || !reservation.relayReceipt || !reservation.childReceipt
        || !reservation.commitCreated
        || !wire::validRelayChildActivationCommit(reservation.commit)
        || !sameRelayChildActivation(reservation.commit.activation,
                                     reservation.activation)) {
        return false;
    }
    const size_t maxPending = _facts.limits.maxTopologyNodes
            <= (std::numeric_limits<size_t>::max)() / 2u
        ? _facts.limits.maxTopologyNodes * 2u
        : (std::numeric_limits<size_t>::max)();
    const auto enqueue = [this, &reservation, maxPending](
                             const EnrollmentRecord* record,
                             const model::RouteDescriptor& expectedRoute) {
        if (record == nullptr || !record->qsrBound
            || !record->qsrEndpoint.valid()) {
            return false;
        }
        model::RouteDescriptor route{};
        route.kind = model::RouteKind::Direct;
        route.ipv4BE = record->qsrEndpoint.ipv4BE;
        route.port = record->qsrEndpoint.port;
        route.generation = record->bindingGeneration;
        route.routeProved = true;
        if (!sameRoute(route, expectedRoute)) return false;
        const auto duplicate = std::find_if(
            _pendingRelayChildActivationCommits.begin(),
            _pendingRelayChildActivationCommits.end(),
            [&route, &reservation](const PendingRelayChildActivationCommit& value) {
                return sameRoute(value.recipient.qsrRoute, route)
                    && sameRelayChildActivationCommit(
                        value.recipient.commit, reservation.commit);
            });
        if (duplicate != _pendingRelayChildActivationCommits.end()) return true;
        if (_pendingRelayChildActivationCommits.size() >= maxPending)
            return false;
        PendingRelayChildActivationCommit pending{};
        pending.recipient.qsrRoute = route;
        pending.recipient.commit = reservation.commit;
        pending.qtaRecord = *record;
        try {
            _pendingRelayChildActivationCommits.push_back(std::move(pending));
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    };
    return enqueue(find(reservation.activation.relayNodeId),
                   reservation.activation.relayControlRoute)
        && enqueue(find(reservation.activation.childNodeId),
                   reservation.activation.childControlRoute);
}

void TopologyAuthority::discardRelayChildActivationCommits(
    const wire::RelayChildActivation& activation) {
    const auto retained = std::remove_if(
        _pendingRelayChildActivationCommits.begin(),
        _pendingRelayChildActivationCommits.end(),
        [&activation](PendingRelayChildActivationCommit& pending) {
            if (!sameRelayChildActivation(pending.recipient.commit.activation,
                                          activation)) {
                return false;
            }
            pending.qtaRecord.credential.fill(0u);
            pending.recipient.commit.hostSignature.fill(0u);
            return true;
        });
    _pendingRelayChildActivationCommits.erase(
        retained, _pendingRelayChildActivationCommits.end());
}

bool TopologyAuthority::establishRelayChildActivationAdjacency(
    RelayActivationReservation& reservation, const uint64_t nowMs) {
    const wire::RelayChildActivation& activation = reservation.activation;
    if (!ready() || reservation.adjacencyAttached || !reservation.relayReceipt
        || !reservation.childReceipt || !reservation.commitCreated
        || !reservation.relayCommitSentLocally
        || !reservation.childCommitSentLocally
        || !wire::validRelayChildActivationCommit(reservation.commit)
        || !sameRelayChildActivation(reservation.commit.activation, activation)) {
        return false;
    }

    EnrollmentRecord* const relay = findMutable(activation.relayNodeId);
    EnrollmentRecord* const child = findMutable(activation.childNodeId);
    if (relay == nullptr || child == nullptr
        || !activationMatchesCurrentBindings(reservation, *relay, *child,
                                             nowMs)) {
        return false;
    }

    const model::ParentLease& childLease = activation.childLease;
    if (childLease.parentDepth != relay->attachedDepth
        || childLease.parentDepth == (std::numeric_limits<uint8_t>::max)()
        || childLease.ancestryCount == 0u
        || childLease.ancestryCount != relay->attachedAncestryCount + 1u
        || childLease.ancestryCount > childLease.ancestry.size()
        || childLease.ancestry[childLease.ancestryCount - 1u]
            != relay->nodeId
        || childLease.routeCount != 1u
        || !sameRoute(childLease.routes[0], activation.relayDataRoute)) {
        return false;
    }
    std::array<model::NodeId, model::kMaxLeaseAncestry> expectedAncestry =
        relay->attachedAncestry;
    expectedAncestry[relay->attachedAncestryCount] = relay->nodeId;
    if (!sameAncestry(childLease.ancestry, childLease.ancestryCount,
                      expectedAncestry,
                      static_cast<uint8_t>(relay->attachedAncestryCount + 1u))) {
        return false;
    }

    model::ParentLeaseExpectation expectation{};
    expectation.stream = activation.stream;
    expectation.expectedChildId = activation.childNodeId;
    expectation.expectedParentId = activation.relayNodeId;
    expectation.expectedChildBinding = childLease.childBinding;
    expectation.nowMs = nowMs;
    expectation.minimumSourceGeneration = _facts.sourceGeneration;
    expectation.minimumRouteGeneration = activation.relayDataRoute.generation;
    expectation.requiredCursor = activation.commonCursor.nextRecord;
    expectation.maxActiveDepth = _facts.limits.maxActiveDepth;
    expectation.parentHasCapacity = true;
    expectation.loopFree = true;
    model::ParentLeaseRegistry verifier(_leaseKey);
    if (verifier.verifyContinuity(childLease, expectation)
        != model::ParentLeaseVerdict::Accepted) {
        return false;
    }

    child->attached = true;
    child->attachmentOwner = AttachmentOwner::RelayActivation;
    child->attachedLease = leaseBindingOf(childLease);
    child->attachedDepth = static_cast<uint8_t>(childLease.parentDepth + 1u);
    child->attachedAncestry = childLease.ancestry;
    child->attachedAncestryCount = childLease.ancestryCount;
    reservation.adjacencyAttached = true;
    return true;
}

void TopologyAuthority::rollbackRelayChildActivationAdjacency(
    RelayActivationReservation& reservation) {
    if (!reservation.adjacencyAttached) return;
    EnrollmentRecord* const child =
        findMutable(reservation.activation.childNodeId);
    const model::LeaseBinding expectedLease =
        leaseBindingOf(reservation.activation.childLease);
    if (child != nullptr && child->attached
        && child->attachmentOwner == AttachmentOwner::RelayActivation
        && sameLeaseBinding(child->attachedLease, expectedLease)) {
        releaseClientDataRootForNode(child->nodeId);
        child->attached = false;
        child->attachmentOwner = AttachmentOwner::None;
        child->attachedLease = {};
        child->attachedDepth = 0u;
        child->attachedAncestry = {};
        child->attachedAncestryCount = 0u;
        child->relayDataEndpoint = {};
        child->relayDataBound = false;
        child->relayDataRequestNonce = 0u;
        child->relayDataBindingGeneration = 0u;
        child->relayReady = false;
        child->relayReadyCacheRange = {};
        child->relayReadyCursor = {};
    }
    reservation.adjacencyAttached = false;
}

bool TopologyAuthority::queueRelayChildActivationRetirement(
    const RelayActivationReservation& reservation,
    const wire::RelayChildRetirementReason reason) {
    const wire::RelayChildActivation& activation = reservation.activation;
    wire::RelayChildActivationReceipt structuralReceipt{};
    if (!ready() || activation.relayNodeId == model::kInvalidNodeId
        || activation.childNodeId == model::kInvalidNodeId
        || !wire::makeRelayChildActivationReceipt(
            activation, wire::RelayChildActivationReceiptRole::Relay,
            structuralReceipt)) {
        return false;
    }
    wire::RelayChildActivationRetirement retirement{};
    retirement.stream = activation.stream;
    retirement.authorityGeneration = activation.authorityGeneration;
    retirement.rootNodeId = activation.rootNodeId;
    retirement.relayNodeId = activation.relayNodeId;
    retirement.childNodeId = activation.childNodeId;
    retirement.relayLease = activation.relayLease;
    retirement.childLease = leaseBindingOf(activation.childLease);
    retirement.relayBindingRequestNonce = activation.relayBindingRequestNonce;
    retirement.relayDataRoute = activation.relayDataRoute;
    retirement.relayControlRoute = activation.relayControlRoute;
    retirement.childDataRoute = activation.childDataRoute;
    retirement.childControlRoute = activation.childControlRoute;
    retirement.relayCacheRange = activation.relayCacheRange;
    retirement.commonCursor = activation.commonCursor;
    retirement.activationNonce = activation.activationNonce;
    retirement.activationDeadlineMs = activation.activationDeadlineMs;
    retirement.reason = reason;

    const size_t maxPending = _facts.limits.maxTopologyNodes
            <= (std::numeric_limits<size_t>::max)() / 2u
        ? _facts.limits.maxTopologyNodes * 2u
        : (std::numeric_limits<size_t>::max)();
    const auto enqueue = [this, &retirement, maxPending](
                             const EnrollmentRecord* record,
                             const model::RouteDescriptor& expectedRoute) {
        if (record == nullptr || !record->qsrBound
            || !record->qsrEndpoint.valid()) {
            return true;
        }
        model::RouteDescriptor route{};
        route.kind = model::RouteKind::Direct;
        route.ipv4BE = record->qsrEndpoint.ipv4BE;
        route.port = record->qsrEndpoint.port;
        route.generation = record->bindingGeneration;
        route.routeProved = true;
        if (!sameRoute(route, expectedRoute)) return true;
        const auto duplicate = std::find_if(
            _pendingRelayChildActivationRetirements.begin(),
            _pendingRelayChildActivationRetirements.end(),
            [&route, &retirement](
                const PendingRelayChildActivationRetirement& value) {
                return sameRoute(value.recipient.qsrRoute, route)
                    && sameRelayChildActivationRetirement(
                        value.recipient.retirement, retirement);
            });
        if (duplicate != _pendingRelayChildActivationRetirements.end())
            return true;
        if (_pendingRelayChildActivationRetirements.size() >= maxPending)
            return false;
        PendingRelayChildActivationRetirement pending{};
        pending.recipient.qsrRoute = route;
        pending.recipient.retirement = retirement;
        pending.qtaRecord = *record;
        try {
            _pendingRelayChildActivationRetirements.push_back(
                std::move(pending));
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    };
    return enqueue(find(activation.relayNodeId), activation.relayControlRoute)
        && enqueue(find(activation.childNodeId), activation.childControlRoute);
}

bool TopologyAuthority::releaseRelayChildActivationsForNode(
    const model::NodeId nodeId,
    const wire::RelayChildRetirementReason reason) {
    if (nodeId == model::kInvalidNodeId) return false;
    for (RelayActivationReservation& activation : _relayActivations) {
        if (activation.activation.relayNodeId != nodeId
            && activation.activation.childNodeId != nodeId) {
            continue;
        }
        discardRelayChildActivationCommits(activation.activation);
        rollbackRelayChildActivationAdjacency(activation);
        if (!queueRelayChildActivationRetirement(activation, reason))
            return false;
    }
    _relayActivations.erase(std::remove_if(
        _relayActivations.begin(), _relayActivations.end(),
        [nodeId](const RelayActivationReservation& activation) {
            return activation.activation.relayNodeId == nodeId
                || activation.activation.childNodeId == nodeId;
        }), _relayActivations.end());
    return true;
}

bool TopologyAuthority::releaseAllRelayChildActivations(
    const wire::RelayChildRetirementReason reason) {
    for (RelayActivationReservation& activation : _relayActivations) {
        discardRelayChildActivationCommits(activation.activation);
        rollbackRelayChildActivationAdjacency(activation);
        if (!queueRelayChildActivationRetirement(activation, reason))
            return false;
    }
    _relayActivations.clear();
    return true;
}

bool TopologyAuthority::clientPlayerRootActivationMatchesCurrentBindings(
    const ClientPlayerRootActivationReservation& reservation,
    const ClientPlayerRootRecord& root, const EnrollmentRecord& target,
    const uint64_t nowMs) const {
    const wire::ClientPlayerRootActivation& activation = reservation.activation;
    if (!ready() || nowMs == 0u || nowMs >= _facts.expiresMs
        || nowMs >= activation.activationDeadlineMs
        || !root.asserted || nowMs >= root.binding.expiresMs
        || nowMs >= target.expiryMs || !target.qspStreaming || !target.qsrBound
        || target.attached
        || activation.authorityGeneration != _facts.authorityGeneration
        || activation.hostRootNodeId != _facts.rootNodeId
        || activation.targetViewerNodeId != target.nodeId
        || !sameClientPlayerRootBinding(activation.binding, root.binding)
        || !sameCursor(root.binding.cursor, _verifiedCursor)
        || !root.binding.cacheRange.containsBoundary(_verifiedCursor.nextRecord)
        || activation.activationDeadlineMs > root.binding.expiresMs
        || activation.activationDeadlineMs > target.expiryMs
        || activation.activationDeadlineMs > _facts.expiresMs) {
        return false;
    }
    model::ParentLeaseChildBinding childBinding{};
    if (!makeParentLeaseChildBinding(target, childBinding)) return false;
    model::RouteDescriptor targetQsrRoute{};
    targetQsrRoute.kind = model::RouteKind::Direct;
    targetQsrRoute.ipv4BE = target.qsrEndpoint.ipv4BE;
    targetQsrRoute.port = target.qsrEndpoint.port;
    targetQsrRoute.generation = target.bindingGeneration;
    targetQsrRoute.routeProved = true;
    const model::ParentLease& lease = activation.targetLease;
    if (!targetQsrRoute.valid()
        || !sameRoute(activation.targetDataRoute,
                      childBinding.childQspRoute)
        || !sameRoute(activation.targetQsrRoute, targetQsrRoute)
        || lease.stream != root.binding.stream
        || lease.childId != target.nodeId
        || lease.parentId != root.binding.clientRootNodeId
        || lease.childBinding != childBinding
        || lease.parentDepth != 0u
        || lease.parentSourceGeneration != root.binding.sourceGeneration
        || lease.routeGeneration != root.binding.dataRoute.generation
        || lease.routeCount != 1u
        || !sameRoute(lease.routes[0], root.binding.dataRoute)
        || !sameCacheRange(lease.cacheRange, root.binding.cacheRange)
        || lease.latestVerifiedCursor != root.binding.cursor.nextRecord
        || lease.childCapacity != root.binding.childCapacity
        || lease.expiryMs != activation.activationDeadlineMs
        || lease.replayNonce == 0u || lease.ancestryCount != 1u
        || lease.ancestry[0] != root.binding.clientRootNodeId) {
        return false;
    }
    model::ParentLeaseExpectation expectation{};
    expectation.stream = root.binding.stream;
    expectation.expectedChildId = target.nodeId;
    expectation.expectedParentId = root.binding.clientRootNodeId;
    expectation.expectedChildBinding = childBinding;
    expectation.nowMs = nowMs;
    expectation.minimumSourceGeneration = root.binding.sourceGeneration;
    expectation.minimumRouteGeneration = root.binding.dataRoute.generation;
    expectation.requiredCursor = root.binding.cursor.nextRecord;
    expectation.maxActiveDepth = _facts.limits.maxActiveDepth;
    expectation.parentHasCapacity = true;
    expectation.loopFree = true;
    model::ParentLeaseRegistry verifier(_leaseKey);
    return verifier.verifyContinuity(lease, expectation)
        == model::ParentLeaseVerdict::Accepted;
}

bool TopologyAuthority::clientPlayerRootActivationReceiptMatches(
    const ClientPlayerRootActivationReservation& reservation,
    const EnrollmentRecord& target,
    const wire::ClientPlayerRootActivationReceipt& receipt,
    const uint64_t nowMs) const {
    const ClientPlayerRootRecord* const root = findClientPlayerRoot(
        reservation.activation.binding.clientRootNodeId);
    return root != nullptr
        && sameClientPlayerRootActivation(receipt.activation,
                                          reservation.activation)
        && clientPlayerRootActivationMatchesCurrentBindings(
            reservation, *root, target, nowMs);
}

bool TopologyAuthority::acceptClientPlayerRootActivationReceipt(
    EnrollmentRecord& target,
    const wire::ClientPlayerRootActivationReceipt& receipt,
    const uint64_t nowMs) {
    const auto found = std::find_if(
        _clientPlayerRootActivations.begin(),
        _clientPlayerRootActivations.end(),
        [&receipt](const ClientPlayerRootActivationReservation& value) {
            return value.activation.activationNonce
                == receipt.activation.activationNonce;
        });
    if (found == _clientPlayerRootActivations.end()
        || !clientPlayerRootActivationReceiptMatches(*found, target, receipt,
                                                     nowMs)) {
        return false;
    }
    found->receiptAccepted = true;
    return ensureClientPlayerRootActivationCommit(*found)
        && queueClientPlayerRootActivationCommit(*found);
}

bool TopologyAuthority::ensureClientPlayerRootActivationCommit(
    ClientPlayerRootActivationReservation& reservation) {
    if (reservation.commitCreated) {
        return wire::validClientPlayerRootActivationCommit(reservation.commit)
            && sameClientPlayerRootActivation(reservation.commit.activation,
                                              reservation.activation);
    }
    if (!ready() || !reservation.receiptAccepted) return false;
    wire::ClientPlayerRootActivationCommit commit{};
    commit.activation = reservation.activation;
    std::vector<uint8_t> attestationInput;
    const bool signedCommit =
        wire::encodeClientPlayerRootActivationCommitAttestationInput(
            commit, attestationInput)
        && _assignmentSigner.sign(attestationInput.data(),
                                  attestationInput.size(),
                                  commit.hostSignature);
    wipeVector(attestationInput);
    if (!signedCommit || !wire::validClientPlayerRootActivationCommit(commit)) {
        commit.hostSignature.fill(0u);
        return false;
    }
    reservation.commit = commit;
    reservation.commitCreated = true;
    return true;
}

bool TopologyAuthority::queueClientPlayerRootActivationCommit(
    const ClientPlayerRootActivationReservation& reservation) {
    if (!ready() || !reservation.receiptAccepted || !reservation.commitCreated
        || !wire::validClientPlayerRootActivationCommit(reservation.commit)
        || !sameClientPlayerRootActivation(reservation.commit.activation,
                                           reservation.activation)) {
        return false;
    }
    const EnrollmentRecord* const target =
        find(reservation.activation.targetViewerNodeId);
    if (target == nullptr || !target->qsrBound || !target->qsrEndpoint.valid())
        return false;
    model::RouteDescriptor route{};
    route.kind = model::RouteKind::Direct;
    route.ipv4BE = target->qsrEndpoint.ipv4BE;
    route.port = target->qsrEndpoint.port;
    route.generation = target->bindingGeneration;
    route.routeProved = true;
    if (!sameRoute(route, reservation.activation.targetQsrRoute)) return false;
    const auto duplicate = std::find_if(
        _pendingClientPlayerRootActivationCommits.begin(),
        _pendingClientPlayerRootActivationCommits.end(),
        [&route, &reservation](
            const PendingClientPlayerRootActivationCommit& value) {
            return sameRoute(value.recipient.qsrRoute, route)
                && sameClientPlayerRootActivationCommit(
                    value.recipient.commit, reservation.commit);
        });
    if (duplicate != _pendingClientPlayerRootActivationCommits.end())
        return true;
    if (_pendingClientPlayerRootActivationCommits.size()
        >= _facts.limits.maxTopologyNodes) {
        return false;
    }
    PendingClientPlayerRootActivationCommit pending{};
    pending.recipient.qsrRoute = route;
    pending.recipient.commit = reservation.commit;
    pending.qtaRecord = *target;
    try {
        _pendingClientPlayerRootActivationCommits.push_back(std::move(pending));
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void TopologyAuthority::discardClientPlayerRootActivationCommits(
    const wire::ClientPlayerRootActivation& activation) {
    const auto retained = std::remove_if(
        _pendingClientPlayerRootActivationCommits.begin(),
        _pendingClientPlayerRootActivationCommits.end(),
        [&activation](PendingClientPlayerRootActivationCommit& pending) {
            if (!sameClientPlayerRootActivation(
                    pending.recipient.commit.activation, activation)) {
                return false;
            }
            pending.qtaRecord.credential.fill(0u);
            pending.recipient.commit.hostSignature.fill(0u);
            return true;
        });
    _pendingClientPlayerRootActivationCommits.erase(
        retained, _pendingClientPlayerRootActivationCommits.end());
}

bool TopologyAuthority::queueClientPlayerRootActivationRetirement(
    const ClientPlayerRootActivationReservation& reservation,
    const wire::ClientPlayerRootRetirementReason reason) {
    if (!ready()) return false;
    wire::ClientPlayerRootActivationRetirement retirement{};
    retirement.activation = reservation.activation;
    retirement.reason = reason;
    std::vector<uint8_t> attestationInput;
    const bool signedRetirement =
        wire::encodeClientPlayerRootActivationRetirementAttestationInput(
            retirement, attestationInput)
        && _assignmentSigner.sign(attestationInput.data(),
                                  attestationInput.size(),
                                  retirement.hostSignature);
    wipeVector(attestationInput);
    if (!signedRetirement
        || !wire::validClientPlayerRootActivationRetirement(retirement)) {
        retirement.hostSignature.fill(0u);
        return false;
    }
    const EnrollmentRecord* const target =
        find(reservation.activation.targetViewerNodeId);
    if (target == nullptr || !target->qsrBound || !target->qsrEndpoint.valid())
        return true;
    model::RouteDescriptor route{};
    route.kind = model::RouteKind::Direct;
    route.ipv4BE = target->qsrEndpoint.ipv4BE;
    route.port = target->qsrEndpoint.port;
    route.generation = target->bindingGeneration;
    route.routeProved = true;
    if (!sameRoute(route, reservation.activation.targetQsrRoute)) return true;
    const auto duplicate = std::find_if(
        _pendingClientPlayerRootActivationRetirements.begin(),
        _pendingClientPlayerRootActivationRetirements.end(),
        [&route, &retirement](
            const PendingClientPlayerRootActivationRetirement& value) {
            return sameRoute(value.recipient.qsrRoute, route)
                && sameClientPlayerRootActivationRetirement(
                    value.recipient.retirement, retirement);
        });
    if (duplicate != _pendingClientPlayerRootActivationRetirements.end())
        return true;
    if (_pendingClientPlayerRootActivationRetirements.size()
        >= _facts.limits.maxTopologyNodes) {
        return false;
    }
    PendingClientPlayerRootActivationRetirement pending{};
    pending.recipient.qsrRoute = route;
    pending.recipient.retirement = retirement;
    pending.qtaRecord = *target;
    try {
        _pendingClientPlayerRootActivationRetirements.push_back(
            std::move(pending));
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

bool TopologyAuthority::releaseClientPlayerRootActivationsForNode(
    const model::NodeId nodeId,
    const wire::ClientPlayerRootRetirementReason reason) {
    if (nodeId == model::kInvalidNodeId) return false;
    for (ClientPlayerRootActivationReservation& activation
         : _clientPlayerRootActivations) {
        if (activation.activation.binding.clientRootNodeId != nodeId
            && activation.activation.targetViewerNodeId != nodeId) {
            continue;
        }
        discardClientPlayerRootActivationCommits(activation.activation);
        if (!queueClientPlayerRootActivationRetirement(activation, reason))
            return false;
    }
    _clientPlayerRootActivations.erase(
        std::remove_if(_clientPlayerRootActivations.begin(),
                       _clientPlayerRootActivations.end(),
                       [nodeId](
                           const ClientPlayerRootActivationReservation& value) {
                           return value.activation.binding.clientRootNodeId
                                      == nodeId
                               || value.activation.targetViewerNodeId == nodeId;
                       }),
        _clientPlayerRootActivations.end());
    return true;
}

bool TopologyAuthority::releaseAllClientPlayerRootActivations(
    const wire::ClientPlayerRootRetirementReason reason) {
    for (ClientPlayerRootActivationReservation& activation
         : _clientPlayerRootActivations) {
        discardClientPlayerRootActivationCommits(activation.activation);
        if (!queueClientPlayerRootActivationRetirement(activation, reason))
            return false;
    }
    _clientPlayerRootActivations.clear();
    return true;
}

bool TopologyAuthority::releaseClientPlayerRootRecord(
    const model::NodeId nodeId,
    const wire::ClientPlayerRootRetirementReason reason) {
    ClientPlayerRootRecord* const record = findClientPlayerRootMutable(nodeId);
    if (record == nullptr) return false;
    if (!releaseClientPlayerRootActivationsForNode(nodeId, reason)) return false;
    const auto found = std::find_if(
        _clientPlayerRoots.begin(), _clientPlayerRoots.end(),
        [nodeId](const ClientPlayerRootRecord& value) {
            return value.binding.clientRootNodeId == nodeId;
        });
    if (found == _clientPlayerRoots.end()) return false;
    if (std::find(_retiredNodeIds.begin(), _retiredNodeIds.end(), nodeId)
        == _retiredNodeIds.end()) {
        try {
            _retiredNodeIds.push_back(nodeId);
        } catch (const std::bad_alloc&) {
            return false;
        }
    }
    found->credential.fill(0u);
    found->grant.hostSignature.fill(0u);
    _clientPlayerRoots.erase(found);
    return true;
}

void TopologyAuthority::releaseAllClientPlayerRoots() {
    for (ClientPlayerRootRecord& record : _clientPlayerRoots) {
        record.credential.fill(0u);
        record.grant.hostSignature.fill(0u);
        record.asserted = false;
    }
    _clientPlayerRoots.clear();
}

bool TopologyAuthority::queueRelayChildRetirement(
    const RelayAssignmentReservation& assignment,
    const wire::RelayChildRetirementReason reason) {
    if (!ready() || assignment.relayNodeId == model::kInvalidNodeId
        || assignment.childNodeId == model::kInvalidNodeId
        || assignment.bindingRequestNonce == 0u) {
        return false;
    }

    const EnrollmentRecord* const relay = find(assignment.relayNodeId);
    wire::RelayDataBindingAck binding{};
    if (relay == nullptr || !makeRelayDataBindingAck(*relay, binding)
        || binding.requestNonce != assignment.bindingRequestNonce
        || !sameLeaseBinding(binding.lease, relay->attachedLease)
        || binding.observedControlRoute.kind != model::RouteKind::Direct
        || !binding.observedControlRoute.valid()) {
        return true;
    }

    wire::RelayChildRetirement retirement{};
    retirement.stream = assignment.childLease.stream;
    retirement.authorityGeneration = _facts.authorityGeneration;
    retirement.rootNodeId = _facts.rootNodeId;
    retirement.relayNodeId = assignment.relayNodeId;
    retirement.childNodeId = assignment.childNodeId;
    retirement.relayLease = binding.lease;
    retirement.bindingRequestNonce = assignment.bindingRequestNonce;
    retirement.childLease = leaseBindingOf(assignment.childLease);
    retirement.reason = reason;
    if (retirement.stream != binding.stream) return false;

    const auto duplicate = std::find_if(
        _pendingRelayChildRetirements.begin(),
        _pendingRelayChildRetirements.end(),
        [&binding, &retirement](const PendingRelayChildRetirement& pending) {
            const wire::RelayChildRetirement& existing =
                pending.recipient.retirement;
            return sameRoute(pending.recipient.qsrRoute,
                             binding.observedControlRoute)
                && existing.relayNodeId == retirement.relayNodeId
                && existing.childNodeId == retirement.childNodeId
                && sameLeaseBinding(existing.relayLease, retirement.relayLease)
                && existing.bindingRequestNonce
                    == retirement.bindingRequestNonce
                && sameLeaseBinding(existing.childLease,
                                    retirement.childLease);
        });
    if (duplicate != _pendingRelayChildRetirements.end()) return true;

    if (_pendingRelayChildRetirements.size()
        >= _facts.limits.maxTopologyNodes) {
        return false;
    }
    PendingRelayChildRetirement pending{};
    pending.recipient.qsrRoute = binding.observedControlRoute;
    pending.recipient.retirement = retirement;
    pending.qtaRecord = *relay;
    try {
        _pendingRelayChildRetirements.push_back(std::move(pending));
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

bool TopologyAuthority::releaseRelayAssignmentsForNode(
    const model::NodeId nodeId,
    const wire::RelayChildRetirementReason reason) {
    if (nodeId == model::kInvalidNodeId) {
        return false;
    }
    discardRelayActivationStagesForNode(nodeId);
    if (!releaseRelayChildActivationsForNode(nodeId, reason)) {
        return false;
    }
    wire::ClientPlayerRootRetirementReason clientRootReason =
        wire::ClientPlayerRootRetirementReason::TargetViewerDetached;
    switch (reason) {
    case wire::RelayChildRetirementReason::LeaseExpired:
        clientRootReason =
            wire::ClientPlayerRootRetirementReason::LeaseExpired;
        break;
    case wire::RelayChildRetirementReason::ServiceStopping:
        clientRootReason =
            wire::ClientPlayerRootRetirementReason::ServiceStopping;
        break;
    case wire::RelayChildRetirementReason::ChildBindingReplaced:
    case wire::RelayChildRetirementReason::ChildDetached:
    default:
        clientRootReason =
            wire::ClientPlayerRootRetirementReason::TargetViewerDetached;
        break;
    }
    if (!releaseClientPlayerRootActivationsForNode(nodeId, clientRootReason))
        return false;
    for (const RelayAssignmentReservation& assignment : _relayAssignments) {
        if (assignment.relayNodeId != nodeId
            && assignment.childNodeId != nodeId) {
            continue;
        }
        if (!queueRelayChildRetirement(assignment, reason)) return false;
    }
    _relayAssignments.erase(std::remove_if(
        _relayAssignments.begin(), _relayAssignments.end(),
        [nodeId](const RelayAssignmentReservation& assignment) {
            return assignment.relayNodeId == nodeId
                || assignment.childNodeId == nodeId;
        }), _relayAssignments.end());
    return true;
}

bool TopologyAuthority::releaseAllRelayAssignments(
    const wire::RelayChildRetirementReason reason) {
    discardAllRelayActivationStages();
    if (!releaseAllRelayChildActivations(reason)) return false;
    for (const RelayAssignmentReservation& assignment : _relayAssignments) {
        if (!queueRelayChildRetirement(assignment, reason)) return false;
    }
    _relayAssignments.clear();
    return true;
}

bool TopologyAuthority::nextRelayChildRetirement(
    RelayChildRetirementRecipient& out) const {
    out = {};
    if (_pendingRelayChildRetirements.empty()) return false;
    out = _pendingRelayChildRetirements.front().recipient;
    return true;
}

bool TopologyAuthority::encodeRelayChildRetirementControl(
    const RelayChildRetirementRecipient& recipient, const uint64_t sequence,
    const uint8_t* qsrBytes, const size_t qsrLength,
    std::vector<uint8_t>& out) const {
    out.clear();
    const auto pending = std::find_if(
        _pendingRelayChildRetirements.begin(),
        _pendingRelayChildRetirements.end(),
        [&recipient](const PendingRelayChildRetirement& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameRelayChildRetirement(value.recipient.retirement,
                                            recipient.retirement);
        });
    if (pending == _pendingRelayChildRetirements.end()
        || recipient.qsrRoute.kind != model::RouteKind::Direct
        || !recipient.qsrRoute.valid()
        || pending->qtaRecord.qsrEndpoint.ipv4BE
            != recipient.qsrRoute.ipv4BE
        || pending->qtaRecord.qsrEndpoint.port != recipient.qsrRoute.port) {
        return false;
    }
    wire::ControlDatagram decoded{};
    if (wire::decodeControlDatagram(qsrBytes, qsrLength, decoded)
            != wire::DecodeResult::Ok
        || decoded.type != wire::ControlMessageType::RelayChildRetirement
        || !sameRelayChildRetirement(decoded.relayChildRetirement,
                                     recipient.retirement)) {
        return false;
    }
    return encodeControlForRecord(pending->qtaRecord, sequence, qsrBytes,
                                  qsrLength, out);
}

bool TopologyAuthority::acknowledgeRelayChildRetirement(
    const RelayChildRetirementRecipient& recipient) {
    const auto found = std::find_if(
        _pendingRelayChildRetirements.begin(),
        _pendingRelayChildRetirements.end(),
        [&recipient](const PendingRelayChildRetirement& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameRelayChildRetirement(value.recipient.retirement,
                                            recipient.retirement);
        });
    if (found == _pendingRelayChildRetirements.end()) return false;
    found->qtaRecord.credential.fill(0u);
    _pendingRelayChildRetirements.erase(found);
    return true;
}

bool TopologyAuthority::nextRelayChildActivationRetirement(
    RelayChildActivationRetirementRecipient& out) const {
    out = {};
    if (_pendingRelayChildActivationRetirements.empty()) return false;
    out = _pendingRelayChildActivationRetirements.front().recipient;
    return true;
}

bool TopologyAuthority::encodeRelayChildActivationRetirementControl(
    const RelayChildActivationRetirementRecipient& recipient,
    const uint64_t sequence, const uint8_t* qsrBytes,
    const size_t qsrLength, std::vector<uint8_t>& out) const {
    out.clear();
    const auto pending = std::find_if(
        _pendingRelayChildActivationRetirements.begin(),
        _pendingRelayChildActivationRetirements.end(),
        [&recipient](const PendingRelayChildActivationRetirement& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameRelayChildActivationRetirement(
                    value.recipient.retirement, recipient.retirement);
        });
    if (pending == _pendingRelayChildActivationRetirements.end()
        || recipient.qsrRoute.kind != model::RouteKind::Direct
        || !recipient.qsrRoute.valid()
        || pending->qtaRecord.qsrEndpoint.ipv4BE
            != recipient.qsrRoute.ipv4BE
        || pending->qtaRecord.qsrEndpoint.port != recipient.qsrRoute.port) {
        return false;
    }
    wire::ControlDatagram decoded{};
    if (wire::decodeControlDatagram(qsrBytes, qsrLength, decoded)
            != wire::DecodeResult::Ok
        || decoded.type
            != wire::ControlMessageType::RelayChildActivationRetirement
        || !sameRelayChildActivationRetirement(
            decoded.relayChildActivationRetirement,
            recipient.retirement)) {
        return false;
    }
    return encodeControlForRecord(pending->qtaRecord, sequence, qsrBytes,
                                  qsrLength, out);
}

bool TopologyAuthority::acknowledgeRelayChildActivationRetirement(
    const RelayChildActivationRetirementRecipient& recipient) {
    const auto found = std::find_if(
        _pendingRelayChildActivationRetirements.begin(),
        _pendingRelayChildActivationRetirements.end(),
        [&recipient](const PendingRelayChildActivationRetirement& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameRelayChildActivationRetirement(
                    value.recipient.retirement, recipient.retirement);
        });
    if (found == _pendingRelayChildActivationRetirements.end()) return false;
    found->qtaRecord.credential.fill(0u);
    _pendingRelayChildActivationRetirements.erase(found);
    return true;
}

bool TopologyAuthority::nextRelayChildActivationCommit(
    RelayChildActivationCommitRecipient& out) const {
    out = {};
    if (_pendingRelayChildActivationCommits.empty()) return false;
    out = _pendingRelayChildActivationCommits.front().recipient;
    return true;
}

bool TopologyAuthority::encodeRelayChildActivationCommitControl(
    const RelayChildActivationCommitRecipient& recipient,
    const uint64_t sequence, const uint8_t* qsrBytes,
    const size_t qsrLength, std::vector<uint8_t>& out) const {
    out.clear();
    const auto pending = std::find_if(
        _pendingRelayChildActivationCommits.begin(),
        _pendingRelayChildActivationCommits.end(),
        [&recipient](const PendingRelayChildActivationCommit& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameRelayChildActivationCommit(value.recipient.commit,
                                                  recipient.commit);
        });
    if (pending == _pendingRelayChildActivationCommits.end()
        || recipient.qsrRoute.kind != model::RouteKind::Direct
        || !recipient.qsrRoute.valid()
        || pending->qtaRecord.qsrEndpoint.ipv4BE
            != recipient.qsrRoute.ipv4BE
        || pending->qtaRecord.qsrEndpoint.port != recipient.qsrRoute.port) {
        return false;
    }
    wire::ControlDatagram decoded{};
    if (wire::decodeControlDatagram(qsrBytes, qsrLength, decoded)
            != wire::DecodeResult::Ok
        || decoded.type != wire::ControlMessageType::RelayChildActivationCommit
        || !sameRelayChildActivationCommit(decoded.relayChildActivationCommit,
                                           recipient.commit)) {
        return false;
    }
    return encodeControlForRecord(pending->qtaRecord, sequence, qsrBytes,
                                  qsrLength, out);
}

bool TopologyAuthority::markRelayChildActivationCommitSent(
    const RelayChildActivationCommitRecipient& recipient,
    const uint64_t nowMs) {
    if (!ready() || nowMs == 0u) return false;
    expire(nowMs);
    const auto found = std::find_if(
        _pendingRelayChildActivationCommits.begin(),
        _pendingRelayChildActivationCommits.end(),
        [&recipient](const PendingRelayChildActivationCommit& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameRelayChildActivationCommit(value.recipient.commit,
                                                  recipient.commit);
        });
    if (found == _pendingRelayChildActivationCommits.end()) return false;

    const auto activation = std::find_if(
        _relayActivations.begin(), _relayActivations.end(),
        [&recipient](const RelayActivationReservation& value) {
            return sameRelayChildActivation(value.activation,
                                             recipient.commit.activation);
        });
    if (activation == _relayActivations.end() || activation->adjacencyAttached
        || !activation->relayReceipt || !activation->childReceipt
        || !activation->commitCreated
        || !sameRelayChildActivationCommit(activation->commit,
                                            recipient.commit)) {
        return false;
    }

    const bool relayRecipient = sameRoute(
        recipient.qsrRoute, activation->activation.relayControlRoute);
    const bool childRecipient = sameRoute(
        recipient.qsrRoute, activation->activation.childControlRoute);
    if (relayRecipient == childRecipient) return false;

    const EnrollmentRecord* const relay =
        find(activation->activation.relayNodeId);
    const EnrollmentRecord* const child =
        find(activation->activation.childNodeId);
    if (relay == nullptr || child == nullptr
        || !activationMatchesCurrentBindings(*activation, *relay, *child,
                                             nowMs)) {
        return false;
    }

    if (relayRecipient) {
        activation->relayCommitSentLocally = true;
    } else {
        activation->childCommitSentLocally = true;
    }
    if (activation->relayCommitSentLocally
        && activation->childCommitSentLocally
        && !establishRelayChildActivationAdjacency(*activation, nowMs)) {
        return false;
    }

    found->qtaRecord.credential.fill(0u);
    found->recipient.commit.hostSignature.fill(0u);
    _pendingRelayChildActivationCommits.erase(found);
    return true;
}

bool TopologyAuthority::nextClientPlayerRootActivationCommit(
    ClientPlayerRootActivationCommitRecipient& out) const {
    out = {};
    if (_pendingClientPlayerRootActivationCommits.empty()) return false;
    out = _pendingClientPlayerRootActivationCommits.front().recipient;
    return true;
}

bool TopologyAuthority::encodeClientPlayerRootActivationCommitControl(
    const ClientPlayerRootActivationCommitRecipient& recipient,
    const uint64_t sequence, const uint8_t* qsrBytes,
    const size_t qsrLength, std::vector<uint8_t>& out) const {
    out.clear();
    const auto pending = std::find_if(
        _pendingClientPlayerRootActivationCommits.begin(),
        _pendingClientPlayerRootActivationCommits.end(),
        [&recipient](const PendingClientPlayerRootActivationCommit& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameClientPlayerRootActivationCommit(
                    value.recipient.commit, recipient.commit);
        });
    if (pending == _pendingClientPlayerRootActivationCommits.end()
        || recipient.qsrRoute.kind != model::RouteKind::Direct
        || !recipient.qsrRoute.valid()
        || pending->qtaRecord.qsrEndpoint.ipv4BE
            != recipient.qsrRoute.ipv4BE
        || pending->qtaRecord.qsrEndpoint.port != recipient.qsrRoute.port) {
        return false;
    }
    wire::ControlDatagram decoded{};
    if (wire::decodeControlDatagram(qsrBytes, qsrLength, decoded)
            != wire::DecodeResult::Ok
        || decoded.type
            != wire::ControlMessageType::ClientPlayerRootActivationCommit
        || !sameClientPlayerRootActivationCommit(
            decoded.clientPlayerRootActivationCommit, recipient.commit)) {
        return false;
    }
    return encodeControlForRecord(pending->qtaRecord, sequence, qsrBytes,
                                  qsrLength, out);
}

bool TopologyAuthority::acknowledgeClientPlayerRootActivationCommit(
    const ClientPlayerRootActivationCommitRecipient& recipient) {
    const auto found = std::find_if(
        _pendingClientPlayerRootActivationCommits.begin(),
        _pendingClientPlayerRootActivationCommits.end(),
        [&recipient](const PendingClientPlayerRootActivationCommit& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameClientPlayerRootActivationCommit(
                    value.recipient.commit, recipient.commit);
        });
    if (found == _pendingClientPlayerRootActivationCommits.end()) return false;
    found->qtaRecord.credential.fill(0u);
    found->recipient.commit.hostSignature.fill(0u);
    _pendingClientPlayerRootActivationCommits.erase(found);
    return true;
}

bool TopologyAuthority::nextClientPlayerRootActivationRetirement(
    ClientPlayerRootActivationRetirementRecipient& out) const {
    out = {};
    if (_pendingClientPlayerRootActivationRetirements.empty()) return false;
    out = _pendingClientPlayerRootActivationRetirements.front().recipient;
    return true;
}

bool TopologyAuthority::encodeClientPlayerRootControlToRoot(
    const model::NodeId clientRootNodeId, const uint64_t sequence,
    const uint8_t* qsrBytes, const size_t qsrLength,
    std::vector<uint8_t>& out, model::RouteDescriptor& outQsrRoute) const {
    out.clear();
    outQsrRoute = {};
    const ClientPlayerRootRecord* const record =
        findClientPlayerRoot(clientRootNodeId);
    if (record == nullptr
        || !encodeClientPlayerRootControlForRecord(*record, sequence,
                                                   qsrBytes, qsrLength, out)) {
        return false;
    }
    outQsrRoute = record->binding.qsrRoute;
    return outQsrRoute.valid();
}

bool TopologyAuthority::assertedClientPlayerRootNode(
    const uint64_t nowMs, model::NodeId& out) const {
    out = model::kInvalidNodeId;
    if (!ready()) return false;
    for (const ClientPlayerRootRecord& record : _clientPlayerRoots) {
        if (record.asserted && nowMs < record.binding.expiresMs) {
            out = record.binding.clientRootNodeId;
            return true;
        }
    }
    return false;
}

bool TopologyAuthority::clientPlayerRootGrantForNode(
    const model::NodeId clientRootNodeId,
    wire::ClientPlayerRootGrant& out) const {
    out = {};
    const ClientPlayerRootRecord* const record =
        findClientPlayerRoot(clientRootNodeId);
    if (record == nullptr || record->grantNonce == 0u) return false;
    out = record->grant;
    return true;
}

bool TopologyAuthority::encodeClientPlayerRootActivationRetirementControl(
    const ClientPlayerRootActivationRetirementRecipient& recipient,
    const uint64_t sequence, const uint8_t* qsrBytes,
    const size_t qsrLength, std::vector<uint8_t>& out) const {
    out.clear();
    const auto pending = std::find_if(
        _pendingClientPlayerRootActivationRetirements.begin(),
        _pendingClientPlayerRootActivationRetirements.end(),
        [&recipient](
            const PendingClientPlayerRootActivationRetirement& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameClientPlayerRootActivationRetirement(
                    value.recipient.retirement, recipient.retirement);
        });
    if (pending == _pendingClientPlayerRootActivationRetirements.end()
        || recipient.qsrRoute.kind != model::RouteKind::Direct
        || !recipient.qsrRoute.valid()
        || pending->qtaRecord.qsrEndpoint.ipv4BE
            != recipient.qsrRoute.ipv4BE
        || pending->qtaRecord.qsrEndpoint.port != recipient.qsrRoute.port) {
        return false;
    }
    wire::ControlDatagram decoded{};
    if (wire::decodeControlDatagram(qsrBytes, qsrLength, decoded)
            != wire::DecodeResult::Ok
        || decoded.type
            != wire::ControlMessageType::ClientPlayerRootActivationRetirement
        || !sameClientPlayerRootActivationRetirement(
            decoded.clientPlayerRootActivationRetirement,
            recipient.retirement)) {
        return false;
    }
    return encodeControlForRecord(pending->qtaRecord, sequence, qsrBytes,
                                  qsrLength, out);
}

bool TopologyAuthority::acknowledgeClientPlayerRootActivationRetirement(
    const ClientPlayerRootActivationRetirementRecipient& recipient) {
    const auto found = std::find_if(
        _pendingClientPlayerRootActivationRetirements.begin(),
        _pendingClientPlayerRootActivationRetirements.end(),
        [&recipient](
            const PendingClientPlayerRootActivationRetirement& value) {
            return sameRoute(value.recipient.qsrRoute, recipient.qsrRoute)
                && sameClientPlayerRootActivationRetirement(
                    value.recipient.retirement, recipient.retirement);
        });
    if (found == _pendingClientPlayerRootActivationRetirements.end())
        return false;
    found->qtaRecord.credential.fill(0u);
    found->recipient.retirement.hostSignature.fill(0u);
    _pendingClientPlayerRootActivationRetirements.erase(found);
    return true;
}

bool TopologyAuthority::queueRelayChildRetirementsForServiceStop() {
    if (!ready()) return false;
    return releaseAllRelayAssignments(
        wire::RelayChildRetirementReason::ServiceStopping);
}

bool TopologyAuthority::issueRelayChildAssignment(
    const model::NodeId relayNodeId, const model::NodeId childNodeId,
    const uint64_t nowMs, wire::RelayChildAssignment& out,
    model::RouteDescriptor& outRelayQsrRoute) {
    out = {};
    outRelayQsrRoute = {};
    if (!ready() || nowMs >= _facts.expiresMs
        || relayNodeId == model::kInvalidNodeId
        || childNodeId == model::kInvalidNodeId
        || relayNodeId == childNodeId || relayNodeId == _facts.rootNodeId
        || childNodeId == _facts.rootNodeId) {
        return false;
    }
    expire(nowMs);
    EnrollmentRecord* const relay = findMutable(relayNodeId);
    EnrollmentRecord* const child = findMutable(childNodeId);
    if (relay == nullptr || child == nullptr || nowMs >= relay->expiryMs
        || nowMs >= child->expiryMs || !relay->qspStreaming
        || !relay->qsrBound || !relay->attached
        || relay->attachmentOwner == AttachmentOwner::None
        || relay->attachedDepth == 0u
        || relay->attachedDepth >= _facts.limits.maxActiveDepth
        || relay->attachedAncestryCount == 0u
        || relay->attachedAncestryCount >= relay->attachedAncestry.size()
        || !relay->relayDataBound
        || !relay->relayReady || !child->qspStreaming || !child->qsrBound
        || child->attached
        || endpointConflictsWithTopologyBinding(child->qspEndpoint,
                                                child->nodeId)) {
        return false;
    }
    if (!sameCursor(relay->relayReadyCursor, _verifiedCursor)
        || !relay->relayReadyCacheRange.containsBoundary(
            _verifiedCursor.nextRecord)) {
        return false;
    }
    if (std::any_of(_offers.begin(), _offers.end(),
                    [childNodeId](const OfferReservation& offer) {
                        return offer.childId == childNodeId && !offer.redeemed;
                    })
        || std::any_of(_relayAssignments.begin(), _relayAssignments.end(),
                       [childNodeId](const RelayAssignmentReservation& item) {
                           return item.childNodeId == childNodeId;
                       })
        || std::any_of(_relayActivations.begin(), _relayActivations.end(),
                       [relayNodeId, childNodeId](
                           const RelayActivationReservation& item) {
                           return item.activation.relayNodeId == relayNodeId
                               || item.activation.childNodeId == childNodeId;
                       })) {
        return false;
    }
    const size_t activeChildren = static_cast<size_t>(std::count_if(
        _relayAssignments.begin(), _relayAssignments.end(),
        [relayNodeId](const RelayAssignmentReservation& item) {
            return item.relayNodeId == relayNodeId;
        })) + static_cast<size_t>(std::count_if(
            _relayActivations.begin(), _relayActivations.end(),
            [relayNodeId](const RelayActivationReservation& item) {
                return item.activation.relayNodeId == relayNodeId;
            }));
    if (activeChildren >= _facts.limits.maxChildrenPerRelay
        || _facts.limits.maxChildrenPerRelay
            > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }

    model::ParentLeaseChildBinding childBinding{};
    wire::RelayDataBindingAck relayBinding{};
    if (!makeParentLeaseChildBinding(*child, childBinding)
        || !makeRelayDataBindingAck(*relay, relayBinding)
        || !sameCursor(childBinding.childRequestedCursor,
                       relay->relayReadyCursor)
        || !relay->relayReadyCacheRange.containsBoundary(
            childBinding.childRequestedCursor.nextRecord)) {
        return false;
    }

    uint64_t nonce = 0u;
    for (unsigned attempt = 0u; attempt < 16u; ++attempt) {
        if (!drawNonzero(nonce)) return false;
        if (!relayAssignmentNonceInUse(nonce)) break;
        nonce = 0u;
    }
    if (nonce == 0u) return false;
    uint64_t expiryMs = nextNonzero(nowMs, _facts.limits.standbyLeaseMs);
    expiryMs = (std::min)(expiryMs, relay->expiryMs);
    expiryMs = (std::min)(expiryMs, child->expiryMs);
    expiryMs = (std::min)(expiryMs, relay->attachedLease.expiryMs);
    expiryMs = (std::min)(expiryMs, _facts.expiresMs);
    if (expiryMs <= nowMs) return false;

    model::ParentLease childLease{};
    childLease.stream = relayBinding.stream;
    childLease.childId = childNodeId;
    childLease.parentId = relayNodeId;
    childLease.childBinding = childBinding;
    childLease.parentDepth = relay->attachedDepth;
    childLease.parentSourceGeneration = _facts.sourceGeneration;
    childLease.routeGeneration = relayBinding.observedDataRoute.generation;
    childLease.routes[0] = relayBinding.observedDataRoute;
    childLease.routeCount = 1u;
    childLease.cacheRange = relay->relayReadyCacheRange;
    childLease.latestVerifiedCursor = relay->relayReadyCursor.nextRecord;
    childLease.childCapacity = static_cast<uint32_t>(
        _facts.limits.maxChildrenPerRelay - activeChildren);
    childLease.expiryMs = expiryMs;
    childLease.replayNonce = nonce;
    childLease.ancestry = relay->attachedAncestry;
    childLease.ancestry[relay->attachedAncestryCount] = relayNodeId;
    childLease.ancestryCount = static_cast<uint8_t>(
        relay->attachedAncestryCount + 1u);
    model::ParentLeaseRegistry signer(_leaseKey);
    if (!signer.issue(childLease)) return false;

    wire::RelayChildAssignment assignment{};
    assignment.stream = relayBinding.stream;
    assignment.authorityGeneration = _facts.authorityGeneration;
    assignment.rootNodeId = _facts.rootNodeId;
    assignment.relayNodeId = relayNodeId;
    assignment.childNodeId = childNodeId;
    assignment.relayLease = relay->attachedLease;
    assignment.bindingRequestNonce = relay->relayDataRequestNonce;
    assignment.observedDataRoute = relayBinding.observedDataRoute;
    assignment.observedControlRoute = relayBinding.observedControlRoute;
    assignment.childLease = childLease;
    std::vector<uint8_t> attestationInput;
    const bool signedAssignment = wire::encodeRelayChildAssignmentAttestationInput(
            assignment, attestationInput)
        && _assignmentSigner.sign(attestationInput.data(),
                                  attestationInput.size(),
                                  assignment.hostSignature);
    wipeVector(attestationInput);
    if (!signedAssignment) {
        assignment.hostSignature.fill(0u);
        return false;
    }
    try {
        _relayAssignments.push_back(RelayAssignmentReservation{
            relayNodeId, childNodeId, relay->relayDataRequestNonce, childLease});
    } catch (const std::bad_alloc&) {
        return false;
    }
    out = assignment;
    outRelayQsrRoute = relayBinding.observedControlRoute;
    return true;
}

bool TopologyAuthority::issueRelayChildActivation(
    const model::NodeId relayNodeId, const model::NodeId childNodeId,
    const uint64_t nowMs, wire::RelayChildActivation& out,
    model::RouteDescriptor& outRelayQsrRoute,
    model::RouteDescriptor& outChildQsrRoute) {
    out = {};
    outRelayQsrRoute = {};
    outChildQsrRoute = {};
    if (!ready() || nowMs >= _facts.expiresMs
        || relayNodeId == model::kInvalidNodeId
        || childNodeId == model::kInvalidNodeId
        || relayNodeId == childNodeId || relayNodeId == _facts.rootNodeId
        || childNodeId == _facts.rootNodeId) {
        return false;
    }
    expire(nowMs);

    auto existing = std::find_if(
        _relayActivations.begin(), _relayActivations.end(),
        [relayNodeId, childNodeId](const RelayActivationReservation& value) {
            return value.activation.relayNodeId == relayNodeId
                && value.activation.childNodeId == childNodeId;
        });
    if (existing != _relayActivations.end()) {
        if (existing->adjacencyAttached) return false;
        const EnrollmentRecord* const relay = find(relayNodeId);
        const EnrollmentRecord* const child = find(childNodeId);
        if (relay != nullptr && child != nullptr
            && activationMatchesCurrentBindings(*existing, *relay, *child,
                                                nowMs)) {
            out = existing->activation;
            outRelayQsrRoute = out.relayControlRoute;
            outChildQsrRoute = out.childControlRoute;
            return true;
        }
        discardRelayChildActivationCommits(existing->activation);
        rollbackRelayChildActivationAdjacency(*existing);
        if (!queueRelayChildActivationRetirement(
                *existing,
                wire::RelayChildRetirementReason::ChildBindingReplaced)) {
            return false;
        }
        _relayActivations.erase(existing);
    }

    EnrollmentRecord* const relay = findMutable(relayNodeId);
    EnrollmentRecord* const child = findMutable(childNodeId);
    if (relay == nullptr || child == nullptr || nowMs >= relay->expiryMs
        || nowMs >= child->expiryMs || !relay->qspStreaming
        || !relay->qsrBound || !relay->attached
        || relay->attachmentOwner == AttachmentOwner::None
        || relay->attachedDepth == 0u
        || relay->attachedDepth >= _facts.limits.maxActiveDepth
        || relay->attachedAncestryCount == 0u
        || relay->attachedAncestryCount >= relay->attachedAncestry.size()
        || !relay->relayDataBound
        || !relay->relayReady || !child->qspStreaming || !child->qsrBound
        || child->attached
        || endpointConflictsWithTopologyBinding(child->qspEndpoint,
                                                child->nodeId)) {
        return false;
    }
    if (!sameCursor(relay->relayReadyCursor, _verifiedCursor)
        || !relay->relayReadyCacheRange.containsBoundary(
            _verifiedCursor.nextRecord)) {
        return false;
    }
    if (std::any_of(_offers.begin(), _offers.end(),
                    [childNodeId](const OfferReservation& offer) {
                        return offer.childId == childNodeId && !offer.redeemed;
                    })
        || std::any_of(_relayActivationStages.begin(),
                       _relayActivationStages.end(),
                       [relayNodeId, childNodeId](
                           const RelayActivationStage& stage) {
                           return stage.relayNodeId == relayNodeId
                               || stage.childNodeId == childNodeId
                               || stage.relayNodeId == childNodeId
                               || stage.childNodeId == relayNodeId;
                       })
        || std::any_of(_relayAssignments.begin(), _relayAssignments.end(),
                       [relayNodeId, childNodeId](
                           const RelayAssignmentReservation& item) {
                           return item.relayNodeId == relayNodeId
                               || item.childNodeId == childNodeId;
                       })
        || std::any_of(_relayActivations.begin(), _relayActivations.end(),
                       [relayNodeId, childNodeId](
                           const RelayActivationReservation& item) {
                           return item.activation.relayNodeId == relayNodeId
                               || item.activation.childNodeId == childNodeId;
                       })) {
        return false;
    }
    const size_t activeChildren = static_cast<size_t>(std::count_if(
        _relayAssignments.begin(), _relayAssignments.end(),
        [relayNodeId](const RelayAssignmentReservation& item) {
            return item.relayNodeId == relayNodeId;
        })) + static_cast<size_t>(std::count_if(
            _relayActivations.begin(), _relayActivations.end(),
            [relayNodeId](const RelayActivationReservation& item) {
                return item.activation.relayNodeId == relayNodeId;
            }));
    if (activeChildren >= _facts.limits.maxChildrenPerRelay
        || _facts.limits.maxChildrenPerRelay
            > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }

    model::ParentLeaseChildBinding childBinding{};
    wire::RelayDataBindingAck relayBinding{};
    if (!makeParentLeaseChildBinding(*child, childBinding)
        || !makeRelayDataBindingAck(*relay, relayBinding)
        || !sameCursor(childBinding.childRequestedCursor,
                       relay->relayReadyCursor)
        || !relay->relayReadyCacheRange.containsBoundary(
            childBinding.childRequestedCursor.nextRecord)) {
        return false;
    }
    model::RouteDescriptor childControlRoute{};
    childControlRoute.kind = model::RouteKind::Direct;
    childControlRoute.ipv4BE = child->qsrEndpoint.ipv4BE;
    childControlRoute.port = child->qsrEndpoint.port;
    childControlRoute.generation = child->bindingGeneration;
    childControlRoute.routeProved = true;
    if (!childControlRoute.valid()
        || childControlRoute.sameEndpoint(childBinding.childQspRoute)
        || childControlRoute.sameEndpoint(relayBinding.observedDataRoute)
        || childControlRoute.sameEndpoint(relayBinding.observedControlRoute)) {
        return false;
    }

    uint64_t leaseNonce = 0u;
    for (unsigned attempt = 0u; attempt < 16u; ++attempt) {
        if (!drawNonzero(leaseNonce)) return false;
        if (!relayAssignmentNonceInUse(leaseNonce)) break;
        leaseNonce = 0u;
    }
    if (leaseNonce == 0u) return false;
    uint64_t activationNonce = 0u;
    for (unsigned attempt = 0u; attempt < 16u; ++attempt) {
        if (!drawNonzero(activationNonce)) return false;
        if (activationNonce != leaseNonce
            && !relayActivationNonceInUse(activationNonce)) {
            break;
        }
        activationNonce = 0u;
    }
    if (activationNonce == 0u) return false;

    uint64_t expiryMs = nextNonzero(nowMs, _facts.limits.standbyLeaseMs);
    expiryMs = (std::min)(expiryMs, relay->expiryMs);
    expiryMs = (std::min)(expiryMs, child->expiryMs);
    expiryMs = (std::min)(expiryMs, relay->attachedLease.expiryMs);
    expiryMs = (std::min)(expiryMs, _facts.expiresMs);
    if (expiryMs <= nowMs) return false;

    model::ParentLease childLease{};
    childLease.stream = relayBinding.stream;
    childLease.childId = childNodeId;
    childLease.parentId = relayNodeId;
    childLease.childBinding = childBinding;
    childLease.parentDepth = relay->attachedDepth;
    childLease.parentSourceGeneration = _facts.sourceGeneration;
    childLease.routeGeneration = relayBinding.observedDataRoute.generation;
    childLease.routes[0] = relayBinding.observedDataRoute;
    childLease.routeCount = 1u;
    childLease.cacheRange = relay->relayReadyCacheRange;
    childLease.latestVerifiedCursor = relay->relayReadyCursor.nextRecord;
    childLease.childCapacity = static_cast<uint32_t>(
        _facts.limits.maxChildrenPerRelay - activeChildren);
    childLease.expiryMs = expiryMs;
    childLease.replayNonce = leaseNonce;
    childLease.ancestry = relay->attachedAncestry;
    childLease.ancestry[relay->attachedAncestryCount] = relayNodeId;
    childLease.ancestryCount = static_cast<uint8_t>(
        relay->attachedAncestryCount + 1u);
    model::ParentLeaseRegistry signer(_leaseKey);
    if (!signer.issue(childLease)) return false;

    wire::RelayChildActivation activation{};
    activation.stream = relayBinding.stream;
    activation.authorityGeneration = _facts.authorityGeneration;
    activation.rootNodeId = _facts.rootNodeId;
    activation.relayNodeId = relayNodeId;
    activation.childNodeId = childNodeId;
    activation.relayLease = relay->attachedLease;
    activation.relayBindingRequestNonce = relay->relayDataRequestNonce;
    activation.relayDataRoute = relayBinding.observedDataRoute;
    activation.relayControlRoute = relayBinding.observedControlRoute;
    activation.childLease = childLease;
    activation.childDataRoute = childBinding.childQspRoute;
    activation.childControlRoute = childControlRoute;
    activation.relayCacheRange = relay->relayReadyCacheRange;
    activation.commonCursor = relay->relayReadyCursor;
    activation.activationNonce = activationNonce;
    activation.activationDeadlineMs = expiryMs;
    std::vector<uint8_t> attestationInput;
    const bool signedActivation =
        wire::encodeRelayChildActivationAttestationInput(
            activation, attestationInput)
        && _assignmentSigner.sign(attestationInput.data(),
                                  attestationInput.size(),
                                  activation.hostSignature);
    wipeVector(attestationInput);
    if (!signedActivation) {
        activation.hostSignature.fill(0u);
        return false;
    }
    try {
        RelayActivationReservation reservation{};
        reservation.activation = activation;
        _relayActivations.push_back(std::move(reservation));
    } catch (const std::bad_alloc&) {
        return false;
    }
    out = activation;
    outRelayQsrRoute = activation.relayControlRoute;
    outChildQsrRoute = activation.childControlRoute;
    return true;
}

bool TopologyAuthority::issueClientPlayerRootActivation(
    const model::NodeId clientRootNodeId,
    const model::NodeId targetViewerNodeId, const uint64_t nowMs,
    wire::ClientPlayerRootActivation& out,
    model::RouteDescriptor& outTargetQsrRoute) {
    out = {};
    outTargetQsrRoute = {};
    if (!ready() || nowMs == 0u || nowMs >= _facts.expiresMs
        || clientRootNodeId == model::kInvalidNodeId
        || targetViewerNodeId == model::kInvalidNodeId
        || clientRootNodeId == targetViewerNodeId
        || clientRootNodeId == _facts.rootNodeId
        || targetViewerNodeId == _facts.rootNodeId) {
        return false;
    }
    expire(nowMs);

    for (auto activation = _clientPlayerRootActivations.begin();
         activation != _clientPlayerRootActivations.end();) {
        const bool touchesTarget = activation->activation.targetViewerNodeId
            == targetViewerNodeId;
        const bool samePair = touchesTarget
            && activation->activation.binding.clientRootNodeId
                == clientRootNodeId;
        if (!touchesTarget && activation->activation.binding.clientRootNodeId
                != clientRootNodeId) {
            ++activation;
            continue;
        }
        const ClientPlayerRootRecord* const root = findClientPlayerRoot(
            activation->activation.binding.clientRootNodeId);
        const EnrollmentRecord* const target = find(
            activation->activation.targetViewerNodeId);
        if (samePair && root != nullptr && target != nullptr
            && clientPlayerRootActivationMatchesCurrentBindings(
                *activation, *root, *target, nowMs)) {
            out = activation->activation;
            outTargetQsrRoute = out.targetQsrRoute;
            return true;
        }
        discardClientPlayerRootActivationCommits(activation->activation);
        if (!queueClientPlayerRootActivationRetirement(
                *activation,
                wire::ClientPlayerRootRetirementReason::ClientRootBindingReplaced)) {
            return false;
        }
        activation = _clientPlayerRootActivations.erase(activation);
    }

    ClientPlayerRootRecord* const root =
        findClientPlayerRootMutable(clientRootNodeId);
    EnrollmentRecord* const target = findMutable(targetViewerNodeId);
    if (root == nullptr || target == nullptr || !root->asserted
        || nowMs >= root->binding.expiresMs || nowMs >= target->expiryMs
        || !target->qspStreaming || !target->qsrBound || target->attached
        || !sameCursor(root->binding.cursor, _verifiedCursor)
        || !root->binding.cacheRange.containsBoundary(
            _verifiedCursor.nextRecord)
        || endpointConflictsWithTopologyBinding(target->qspEndpoint,
                                                target->nodeId)) {
        return false;
    }
    if (std::any_of(_offers.begin(), _offers.end(),
                    [targetViewerNodeId](const OfferReservation& offer) {
                        return offer.childId == targetViewerNodeId
                            && !offer.redeemed;
                    })
        || std::any_of(_relayActivationStages.begin(),
                       _relayActivationStages.end(),
                       [targetViewerNodeId](const RelayActivationStage& stage) {
                           return stage.relayNodeId == targetViewerNodeId
                               || stage.childNodeId == targetViewerNodeId;
                       })
        || std::any_of(_relayAssignments.begin(), _relayAssignments.end(),
                       [targetViewerNodeId](
                           const RelayAssignmentReservation& assignment) {
                           return assignment.relayNodeId == targetViewerNodeId
                               || assignment.childNodeId == targetViewerNodeId;
                       })
        || std::any_of(_relayActivations.begin(), _relayActivations.end(),
                       [targetViewerNodeId](
                           const RelayActivationReservation& activation) {
                           return activation.activation.relayNodeId
                                      == targetViewerNodeId
                               || activation.activation.childNodeId
                                      == targetViewerNodeId;
                       })) {
        return false;
    }
    const size_t rootChildren = static_cast<size_t>(std::count_if(
        _clientPlayerRootActivations.begin(),
        _clientPlayerRootActivations.end(),
        [clientRootNodeId](const ClientPlayerRootActivationReservation& item) {
            return item.activation.binding.clientRootNodeId == clientRootNodeId;
        }));
    if (rootChildren >= root->binding.childCapacity) return false;

    model::ParentLeaseChildBinding childBinding{};
    if (!makeParentLeaseChildBinding(*target, childBinding)) return false;
    model::RouteDescriptor targetQsrRoute{};
    targetQsrRoute.kind = model::RouteKind::Direct;
    targetQsrRoute.ipv4BE = target->qsrEndpoint.ipv4BE;
    targetQsrRoute.port = target->qsrEndpoint.port;
    targetQsrRoute.generation = target->bindingGeneration;
    targetQsrRoute.routeProved = true;
    if (!targetQsrRoute.valid()
        || targetQsrRoute.sameEndpoint(childBinding.childQspRoute)
        || targetQsrRoute.sameEndpoint(root->binding.dataRoute)
        || targetQsrRoute.sameEndpoint(root->binding.qsrRoute)
        || childBinding.childQspRoute.sameEndpoint(root->binding.dataRoute)
        || childBinding.childQspRoute.sameEndpoint(root->binding.qsrRoute)) {
        return false;
    }

    uint64_t leaseNonce = 0u;
    for (unsigned attempt = 0u; attempt < 16u; ++attempt) {
        if (!drawNonzero(leaseNonce)) return false;
        if (!relayAssignmentNonceInUse(leaseNonce)) break;
        leaseNonce = 0u;
    }
    if (leaseNonce == 0u) return false;
    uint64_t activationNonce = 0u;
    for (unsigned attempt = 0u; attempt < 16u; ++attempt) {
        if (!drawNonzero(activationNonce)) return false;
        if (activationNonce != leaseNonce
            && !relayActivationNonceInUse(activationNonce)) {
            break;
        }
        activationNonce = 0u;
    }
    if (activationNonce == 0u) return false;

    uint64_t expiryMs = nextNonzero(nowMs, _facts.limits.standbyLeaseMs);
    expiryMs = (std::min)(expiryMs, root->binding.expiresMs);
    expiryMs = (std::min)(expiryMs, target->expiryMs);
    expiryMs = (std::min)(expiryMs, _facts.expiresMs);
    if (expiryMs <= nowMs) return false;

    model::ParentLease lease{};
    lease.stream = root->binding.stream;
    lease.childId = targetViewerNodeId;
    lease.parentId = clientRootNodeId;
    lease.childBinding = childBinding;
    lease.parentDepth = 0u;
    lease.parentSourceGeneration = root->binding.sourceGeneration;
    lease.routeGeneration = root->binding.dataRoute.generation;
    lease.routes[0] = root->binding.dataRoute;
    lease.routeCount = 1u;
    lease.cacheRange = root->binding.cacheRange;
    lease.latestVerifiedCursor = root->binding.cursor.nextRecord;
    lease.childCapacity = root->binding.childCapacity;
    lease.expiryMs = expiryMs;
    lease.replayNonce = leaseNonce;
    lease.ancestry[0] = clientRootNodeId;
    lease.ancestryCount = 1u;
    model::ParentLeaseRegistry signer(_leaseKey);
    if (!signer.issue(lease)) return false;

    wire::ClientPlayerRootActivation activation{};
    activation.authorityGeneration = _facts.authorityGeneration;
    activation.hostRootNodeId = _facts.rootNodeId;
    activation.binding = root->binding;
    activation.targetViewerNodeId = targetViewerNodeId;
    activation.targetLease = lease;
    activation.targetDataRoute = childBinding.childQspRoute;
    activation.targetQsrRoute = targetQsrRoute;
    activation.activationNonce = activationNonce;
    activation.activationDeadlineMs = expiryMs;
    std::vector<uint8_t> attestationInput;
    const bool signedActivation =
        wire::encodeClientPlayerRootActivationAttestationInput(
            activation, attestationInput)
        && _assignmentSigner.sign(attestationInput.data(),
                                  attestationInput.size(),
                                  activation.hostSignature);
    wipeVector(attestationInput);
    if (!signedActivation) {
        activation.hostSignature.fill(0u);
        return false;
    }
    ClientPlayerRootActivationReservation reservation{};
    reservation.activation = activation;
    if (!clientPlayerRootActivationMatchesCurrentBindings(reservation, *root,
                                                           *target, nowMs)) {
        activation.hostSignature.fill(0u);
        return false;
    }
    try {
        _clientPlayerRootActivations.push_back(std::move(reservation));
    } catch (const std::bad_alloc&) {
        activation.hostSignature.fill(0u);
        return false;
    }
    out = activation;
    outTargetQsrRoute = targetQsrRoute;
    return true;
}

bool TopologyAuthority::childAdvertisement(const model::NodeId nodeId,
                                            model::NodeAdvertisement& out) const {
    out = {};
    const EnrollmentRecord* record = find(nodeId);
    if (!ready() || record == nullptr || !record->qsrBound) return false;
    return makeChildAdvertisement(*record, record->qsrEndpoint, out);
}

namespace {

constexpr uint64_t kTopologyRelayLaneConnectDeadlineMs = 4'000u;
constexpr uint64_t kTopologyRelayLaneWelcomeDeadlineMs = 4'000u;
constexpr uint64_t kTopologyRelayLaneInitialBackoffMs = 500u;
constexpr uint64_t kTopologyRelayLaneMaximumBackoffMs = 8'000u;
constexpr uint32_t kTopologyRelayLaneMaximumAttempts = 10u;
constexpr uint64_t kTopologyRelayGrantLifetimeSeconds = 120u;
constexpr uint16_t kFirstTopologyRelayQsrVirtualPort = 0x8000u;

enum class TopologyRelayLaneState : uint8_t {
    Disabled = 0,
    Backoff = 1,
    Connecting = 2,
    AwaitingWelcome = 3,
    Active = 4,
    Terminal = 5,
};

}

struct TopologyQsrControl::Impl {
    qnet::UdpSock socket{};
    EndpointBinding binding{};
    bool forceServiceFailure = false;
    TopologyAuthority* authority = nullptr;
    resilient::ResilientSpectatorRootRuntime* root = nullptr;
    struct OutboundSequence {
        model::NodeId nodeId = model::kInvalidNodeId;
        uint64_t next = 1u;
    };
    std::vector<OutboundSequence> outbound{};

    TopologyRelayLaneConfig relayConfig{};
    qnet::Endpoint relayEndpoint{};
    SOCKET relayConnectingSocket = INVALID_SOCKET;
    qnet::TcpConn relayControl;
    TopologyRelayLaneState relayState = TopologyRelayLaneState::Disabled;
    uint32_t relayAttempts = 0;
    uint64_t relayNextAttemptMs = 0;
    uint64_t relayAttemptDeadlineMs = 0;
    uint64_t relayBackoffMs = kTopologyRelayLaneInitialBackoffMs;
    std::vector<uint8_t> relayHello;
    qnetplay::spectator::TopologyGrantKey relayGrantKey{};
    qnetplay::spectator::TopologyRelayAuthorityAdapter relayAdapter;
    struct RelayNodePort {
        model::NodeId nodeId = model::kInvalidNodeId;
        uint16_t port = 0;
    };
    std::vector<RelayNodePort> relayNodePorts;
    uint16_t nextRelayQsrVirtualPort = kFirstTopologyRelayQsrVirtualPort;

    static uint64_t relayDeadlineAfter(const uint64_t nowMs,
                                       const uint64_t deltaMs) {
        const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
        return deltaMs > maximum - nowMs ? maximum : nowMs + deltaMs;
    }

    bool relayLaneActive() const {
        return relayState == TopologyRelayLaneState::Active
            && relayAdapter.enabled();
    }

    uint16_t relayQsrVirtualPort(const model::NodeId nodeId) {
        if (nodeId == model::kInvalidNodeId || nodeId == 0u) return 0u;
        for (const RelayNodePort& entry : relayNodePorts) {
            if (entry.nodeId == nodeId) return entry.port;
        }
        if (nextRelayQsrVirtualPort == 0u) return 0u;
        RelayNodePort fresh{};
        fresh.nodeId = nodeId;
        fresh.port = nextRelayQsrVirtualPort;
        try {
            relayNodePorts.push_back(fresh);
        } catch (const std::bad_alloc&) {
            return 0u;
        }
        // Deliberate wrap-to-zero: the allocator jams shut after 32767 relayed enrollments instead of reusing a live port.
        ++nextRelayQsrVirtualPort;
        return fresh.port;
    }

    void closePendingRelaySocket() {
        if (relayConnectingSocket == INVALID_SOCKET) return;
        closesocket(relayConnectingSocket);
        relayConnectingSocket = INVALID_SOCKET;
    }

    void wipeRelayGrantKey() {
        volatile uint8_t* bytes = relayGrantKey.data();
        for (size_t index = 0; index < relayGrantKey.size(); ++index)
            bytes[index] = 0u;
    }

    void resetRelayLane(const uint64_t nowMs, const bool terminal) {
        closePendingRelaySocket();
        relayControl.close();
        relayAdapter.reset();
        wipeRelayGrantKey();
        relayAttemptDeadlineMs = 0;
        if (terminal || relayState == TopologyRelayLaneState::Disabled) {
            relayState = relayConfig.enabled
                ? TopologyRelayLaneState::Terminal
                : TopologyRelayLaneState::Disabled;
            relayNextAttemptMs = 0;
            return;
        }
        if (relayAttempts >= kTopologyRelayLaneMaximumAttempts) {
            relayState = TopologyRelayLaneState::Terminal;
            relayNextAttemptMs = 0;
            return;
        }
        relayState = TopologyRelayLaneState::Backoff;
        relayNextAttemptMs = relayDeadlineAfter(nowMs, relayBackoffMs);
        relayBackoffMs = (std::min)(relayBackoffMs * 2u,
                                    kTopologyRelayLaneMaximumBackoffMs);
    }

    void beginRelayLaneConnect(const uint64_t nowMs) {
        if (relayAttempts >= kTopologyRelayLaneMaximumAttempts) {
            resetRelayLane(nowMs, true);
            return;
        }
        ++relayAttempts;
        closePendingRelaySocket();
        relayConnectingSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (relayConnectingSocket == INVALID_SOCKET) {
            resetRelayLane(nowMs, false);
            return;
        }
        u_long nonBlocking = 1;
        if (ioctlsocket(relayConnectingSocket, FIONBIO, &nonBlocking) != 0) {
            resetRelayLane(nowMs, false);
            return;
        }
        const int connected = ::connect(
            relayConnectingSocket,
            reinterpret_cast<const sockaddr*>(&relayEndpoint.sa),
            sizeof(relayEndpoint.sa));
        if (connected == 0) {
            finishRelayLaneConnect(nowMs);
            return;
        }
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS
            && error != WSAEALREADY) {
            resetRelayLane(nowMs, false);
            return;
        }
        relayState = TopologyRelayLaneState::Connecting;
        relayAttemptDeadlineMs =
            relayDeadlineAfter(nowMs, kTopologyRelayLaneConnectDeadlineMs);
    }

    void finishRelayLaneConnect(const uint64_t nowMs) {
        if (relayConnectingSocket == INVALID_SOCKET) {
            resetRelayLane(nowMs, false);
            return;
        }
        const SOCKET connected = relayConnectingSocket;
        relayConnectingSocket = INVALID_SOCKET;
        BOOL noDelay = TRUE;
        BOOL keepAlive = TRUE;
        (void)setsockopt(connected, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&noDelay),
                         sizeof(noDelay));
        (void)setsockopt(connected, SOL_SOCKET, SO_KEEPALIVE,
                         reinterpret_cast<const char*>(&keepAlive),
                         sizeof(keepAlive));
        relayControl = qnet::TcpConn(connected);
        if (!relayControl.sendMsg(qnet::relay::kTcpEnvelopeType,
                                  relayHello.data(), relayHello.size())) {
            resetRelayLane(nowMs, false);
            return;
        }
        relayState = TopologyRelayLaneState::AwaitingWelcome;
        relayAttemptDeadlineMs =
            relayDeadlineAfter(nowMs, kTopologyRelayLaneWelcomeDeadlineMs);
    }

    void pollRelayLaneConnect(const uint64_t nowMs) {
        if (relayConnectingSocket == INVALID_SOCKET) {
            resetRelayLane(nowMs, false);
            return;
        }
        fd_set writable;
        fd_set exceptional;
        FD_ZERO(&writable);
        FD_ZERO(&exceptional);
        FD_SET(relayConnectingSocket, &writable);
        FD_SET(relayConnectingSocket, &exceptional);
        timeval noWait{};
        const int selected =
            select(0, nullptr, &writable, &exceptional, &noWait);
        if (selected == SOCKET_ERROR) {
            resetRelayLane(nowMs, false);
            return;
        }
        if (selected == 0) {
            if (nowMs >= relayAttemptDeadlineMs) resetRelayLane(nowMs, false);
            return;
        }
        int socketError = 0;
        int socketErrorBytes = sizeof(socketError);
        if (getsockopt(relayConnectingSocket, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socketError),
                       &socketErrorBytes) != 0
            || socketError != 0
            || FD_ISSET(relayConnectingSocket, &exceptional)) {
            resetRelayLane(nowMs, false);
            return;
        }
        finishRelayLaneConnect(nowMs);
    }

    bool acceptRelayLaneWelcome(const qnet::relay::TopologyWelcome& welcome) {
        if (welcome.party != qnet::relay::TopologyParty::Authority
            || !(welcome.room == relayConfig.room)
            || welcome.sessionId != relayConfig.sessionId
            || welcome.authorityGeneration == 0u
            || !qnet::relay::isValidId(welcome.registrationToken)
            || welcome.grantKey.size() != qnet::relay::kGrantKeyBytes
            || welcome.grantKey.size() != relayGrantKey.size()) {
            return false;
        }
        qnetplay::spectator::TopologyRelayAuthorityBinding binding{};
        binding.enabled = true;
        binding.room = relayConfig.room;
        binding.token = welcome.registrationToken;
        binding.authorityGeneration = welcome.authorityGeneration;
        if (!relayAdapter.configure(binding)) return false;
        std::copy(welcome.grantKey.begin(), welcome.grantKey.end(),
                  relayGrantKey.begin());
        relayState = TopologyRelayLaneState::Active;
        relayAttemptDeadlineMs = 0;
        relayNextAttemptMs = 0;
        relayAttempts = 0;
        relayBackoffMs = kTopologyRelayLaneInitialBackoffMs;
        return true;
    }

    void pollRelayLaneWelcome(const uint64_t nowMs) {
        qnet::TcpConn::Received received;
        const qnet::TcpConn::Rs result = relayControl.recvMsgEx(received, 0);
        if (result == qnet::TcpConn::Rs::Timeout) {
            if (nowMs >= relayAttemptDeadlineMs) resetRelayLane(nowMs, false);
            return;
        }
        if (result == qnet::TcpConn::Rs::Closed
            || result == qnet::TcpConn::Rs::Error) {
            resetRelayLane(nowMs, false);
            return;
        }
        if (result == qnet::TcpConn::Rs::Corrupt
            || received.type != qnet::relay::kTcpEnvelopeType) {
            resetRelayLane(nowMs, true);
            return;
        }
        qnet::relay::TopologyWelcome welcome{};
        if (qnet::relay::decodeTopologyWelcome(
                received.body.data(), received.body.size(), welcome)
            != qnet::relay::DecodeError::None) {
            resetRelayLane(nowMs, true);
            return;
        }
        if (welcome.status == qnet::relay::TopologyStatus::Accepted) {
            if (!acceptRelayLaneWelcome(welcome)) resetRelayLane(nowMs, true);
            else {
                std::vector<uint8_t> registration;
                if (relayAdapter.encodeRegistration(nowMs, registration,
                                                    true)) {
                    (void)socket.sendTo(relayEndpoint, registration.data(),
                                        registration.size());
                }
            }
            return;
        }
        if (welcome.status == qnet::relay::TopologyStatus::RateLimited
            || welcome.status
                == qnet::relay::TopologyStatus::AuthorityUnavailable) {
            resetRelayLane(nowMs, false);
            return;
        }
        resetRelayLane(nowMs, true);
    }

    void pollActiveRelayLane(const uint64_t nowMs) {
        qnet::TcpConn::Received received;
        const qnet::TcpConn::Rs result = relayControl.recvMsgEx(received, 0);
        if (result == qnet::TcpConn::Rs::Closed
            || result == qnet::TcpConn::Rs::Error) {
            resetRelayLane(nowMs, false);
            return;
        }
        if (result == qnet::TcpConn::Rs::Msg
            || result == qnet::TcpConn::Rs::Corrupt) {
            resetRelayLane(nowMs, true);
            return;
        }
        std::vector<uint8_t> registration;
        if (relayAdapter.encodeRegistration(nowMs, registration)) {
            (void)socket.sendTo(relayEndpoint, registration.data(),
                                registration.size());
        }
    }

    void serviceRelayLane(const uint64_t nowMs) {
        switch (relayState) {
        case TopologyRelayLaneState::Disabled:
        case TopologyRelayLaneState::Terminal:
            return;
        case TopologyRelayLaneState::Backoff:
            if (nowMs >= relayNextAttemptMs) beginRelayLaneConnect(nowMs);
            return;
        case TopologyRelayLaneState::Connecting:
            pollRelayLaneConnect(nowMs);
            return;
        case TopologyRelayLaneState::AwaitingWelcome:
            pollRelayLaneWelcome(nowMs);
            return;
        case TopologyRelayLaneState::Active:
            pollActiveRelayLane(nowMs);
            return;
        }
    }

    void closeRelayLane() {
        if (relayLaneActive() && socket.isOpen()) {
            std::vector<uint8_t> detach;
            if (relayAdapter.encodeDetach(
                    0u, qnet::relay::TopologyDetachReason::ServiceStopping,
                    detach)) {
                (void)socket.sendTo(relayEndpoint, detach.data(),
                                    detach.size());
            }
        }
        closePendingRelaySocket();
        relayControl.close();
        relayAdapter.reset();
        wipeRelayGrantKey();
        relayConfig = {};
        relayEndpoint = {};
        relayHello.clear();
        relayState = TopologyRelayLaneState::Disabled;
        relayAttempts = 0;
        relayNextAttemptMs = 0;
        relayAttemptDeadlineMs = 0;
        relayBackoffMs = kTopologyRelayLaneInitialBackoffMs;
        relayNodePorts.clear();
        nextRelayQsrVirtualPort = kFirstTopologyRelayQsrVirtualPort;
    }
};

TopologyQsrControl::TopologyQsrControl() : _impl(new Impl()) {}

TopologyQsrControl::~TopologyQsrControl() {
    close();
}

bool TopologyQsrControl::open(const TopologyActivationRequest& request,
                              const EndpointBinding& rootQsp,
                              EndpointBinding& qsrBinding,
                              std::string& error) {
    qsrBinding = {};
    error.clear();
    close();
    if (!_impl) _impl.reset(new (std::nothrow) Impl());
    if (!_impl || !request.requested || !rootQsp.validForAuthority()
        || !rootQsp.advertised.routeProved) {
        error = "topology QSR configuration was incomplete";
        return false;
    }
    if (request.forceQsrBindFailure) {
        error = "topology QSR bind was forced to fail by the focused test seam";
        return false;
    }
    if (!qnet::netInit() || !_impl->socket.open(0u)
        || _impl->socket.boundPort() == 0u) {
        error = "the topology QSR UDP control socket could not be bound";
        _impl->socket.close();
        return false;
    }
    EndpointBinding binding{};
    binding.local = RouteEndpoint{0u, _impl->socket.boundPort()};
    binding.localProvenance = EndpointProvenance::LocalBind;
    binding.advertised = rootQsp.advertised;
    binding.advertised.port = _impl->socket.boundPort();
    binding.advertised.generation = rootQsp.generation;
    binding.advertised.routeProved = false;
    binding.advertisedProvenance = EndpointProvenance::PublishedDirectCandidate;
    binding.generation = rootQsp.generation;
    if (!binding.validForAuthority()) {
        error = "the topology QSR endpoint could not be advertised exactly";
        _impl->socket.close();
        return false;
    }
    _impl->binding = binding;
    _impl->forceServiceFailure = request.forceQsrServiceFailure;
    qsrBinding = binding;
    return true;
}

bool TopologyQsrControl::configureRelayLane(
    const TopologyRelayLaneConfig& config, std::string& warning) {
    warning.clear();
    if (!_impl) return false;
    _impl->closeRelayLane();
    if (!config.enabled) return true;
    if (!_impl->socket.isOpen() || !config.relay.valid()
        || !qnet::relay::isValidId(config.room)
        || !qnet::relay::isValidId(config.authorityToken)
        || config.sessionId == 0u) {
        warning =
            "topology relay lane disabled because its binding is incomplete";
        return false;
    }
    qnet::relay::TopologyHello hello{};
    hello.room = config.room;
    hello.party = qnet::relay::TopologyParty::Authority;
    hello.admission = qnet::relay::TopologyAdmission::None;
    hello.authorityToken = config.authorityToken;
    hello.sessionId = config.sessionId;
    if (!qnet::relay::encodeTopologyHello(hello, _impl->relayHello)) {
        warning = "topology relay lane enrollment could not be encoded";
        return false;
    }
    _impl->relayConfig = config;
    _impl->relayEndpoint = {};
    _impl->relayEndpoint.sa.sin_family = AF_INET;
    _impl->relayEndpoint.sa.sin_addr.s_addr = config.relay.ipv4BE;
    _impl->relayEndpoint.sa.sin_port = htons(config.relay.port);
    _impl->relayState = TopologyRelayLaneState::Backoff;
    _impl->relayNextAttemptMs = 0;
    _impl->relayAttempts = 0;
    _impl->relayBackoffMs = kTopologyRelayLaneInitialBackoffMs;
    return true;
}

bool TopologyQsrControl::relayLaneReady() const {
    return _impl != nullptr && _impl->relayLaneActive();
}

bool TopologyQsrControl::issueRelayQsrOffer(
    const model::NodeId nodeId, const uint64_t sessionId,
    const uint64_t nowUnix, qnetplay::spectator::RelayQsrOffer& out) {
    out = {};
    if (!_impl || !_impl->relayLaneActive() || nodeId == model::kInvalidNodeId
        || nodeId == 0u || sessionId == 0u || nowUnix == 0u
        || sessionId != _impl->relayConfig.sessionId) {
        return false;
    }
    if (_impl->relayQsrVirtualPort(nodeId) == 0u) return false;
    bool degraded = false;
    const uint64_t grantId = qnet::secureRandom64(degraded);
    if (degraded || grantId == 0u) return false;
    qnetplay::spectator::TopologyGrant grant{};
    grant.grantId = grantId;
    grant.nodeId = nodeId;
    grant.sessionId = sessionId;
    grant.expiryUnix = nowUnix + kTopologyRelayGrantLifetimeSeconds;
    grant.authorityGeneration =
        _impl->relayAdapter.binding().authorityGeneration;
    if (!qnetplay::spectator::issueTopologyGrant(_impl->relayGrantKey, grant)
        || (grant.macLow == 0u && grant.macHigh == 0u)) {
        return false;
    }
    out.present = true;
    out.relayIpv4BE = _impl->relayConfig.relay.ipv4BE;
    out.relayPort = _impl->relayConfig.relay.port;
    out.room = _impl->relayConfig.room;
    out.grant = grant;
    return true;
}

void TopologyQsrControl::attach(
    TopologyAuthority& authority,
    resilient::ResilientSpectatorRootRuntime& root) {
    if (!_impl || !_impl->socket.isOpen()) return;
    _impl->authority = &authority;
    _impl->root = &root;
}

void TopologyQsrControl::detach() {
    if (!_impl) return;
    if (_impl->socket.isOpen() && _impl->authority != nullptr
        && _impl->authority->ready()) {
        (void)_impl->authority->queueRelayChildRetirementsForServiceStop();
        (void)flushRelayChildRetirements();
        (void)flushRelayChildActivationRetirements();
    }
    _impl->authority = nullptr;
    _impl->root = nullptr;
    _impl->outbound.clear();
}

void TopologyQsrControl::close() {
    if (!_impl) return;
    detach();
    _impl->closeRelayLane();
    _impl->socket.close();
    _impl->binding = {};
}

bool TopologyQsrControl::open() const {
    return _impl != nullptr && _impl->socket.isOpen();
}

const EndpointBinding& TopologyQsrControl::binding() const {
    static const EndpointBinding empty{};
    return _impl ? _impl->binding : empty;
}

bool TopologyQsrControl::service(const uint64_t nowMs) {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr
        || _impl->root == nullptr) {
        return false;
    }
    if (_impl->authority->facts().revoked) return false;
    if (_impl->forceServiceFailure) return false;
    (void)flushRelayChildRetirements();
    (void)flushRelayChildActivationRetirements();
    (void)flushRelayChildActivationCommits(nowMs);
    (void)flushClientPlayerRootActivationCommits(nowMs);
    (void)flushClientPlayerRootActivationRetirements();
    _impl->serviceRelayLane(nowMs);
    std::array<uint8_t,
               kMaxTopologyControlBytes
                   + qnet::relay::kTopologyFrameHeaderSize> bytes{};
    std::vector<uint8_t> unwrapped;
    for (size_t budget = 0u; budget < 32u; ++budget) {
        qnet::Endpoint peer{};
        const int received = _impl->socket.recvFrom(bytes.data(), bytes.size(), peer);
        if (received == 0) break;
        if (received < 0) return false;
        RouteEndpoint from{peer.sa.sin_addr.s_addr,
                           ntohs(peer.sa.sin_port)};
        const uint8_t* datagram = bytes.data();
        size_t datagramLength = static_cast<size_t>(received);
        if (from.ipv4BE == qnetplay::spectator::kRelayVirtualIpv4BE) continue;
        const bool fromRelayLane = _impl->relayLaneActive()
            && peer.sa.sin_addr.s_addr
                == _impl->relayEndpoint.sa.sin_addr.s_addr
            && peer.sa.sin_port == _impl->relayEndpoint.sa.sin_port;
        if (fromRelayLane) {
            uint64_t frameNodeId = 0u;
            const qnetplay::spectator::TopologyRelayIngress ingress =
                _impl->relayAdapter.decodeInbound(true, datagram,
                                                  datagramLength, unwrapped,
                                                  frameNodeId);
            if (ingress == qnetplay::spectator::TopologyRelayIngress::Data) {
                const uint16_t virtualPort =
                    _impl->relayQsrVirtualPort(frameNodeId);
                if (virtualPort == 0u) continue;
                from = RouteEndpoint{
                    qnetplay::spectator::kRelayVirtualIpv4BE, virtualPort};
                datagram = unwrapped.data();
                datagramLength = unwrapped.size();
            } else if (ingress
                       == qnetplay::spectator::TopologyRelayIngress::Detached) {
                if (frameNodeId == 0u) _impl->resetRelayLane(nowMs, false);
                continue;
            } else {
                continue;
            }
        }
        TopologyControlEnvelope envelope{};
        const TopologyControlResult decodedResult =
            _impl->authority->decodeControl(
                from, datagram, datagramLength, nowMs,
                envelope);
        if (decodedResult == TopologyControlResult::UnknownNode) {
            TopologyControlEnvelope rootEnvelope{};
            if (_impl->authority->decodeClientPlayerRootControl(
                    from, datagram, datagramLength, nowMs,
                    rootEnvelope)
                == TopologyControlResult::Accepted) {
                wire::ClientPlayerRootGrant grant{};
                if (_impl->authority->clientPlayerRootGrantForNode(
                        rootEnvelope.nodeId, grant)) {
                    wire::ControlDatagram ack{};
                    ack.type =
                        wire::ControlMessageType::ClientPlayerRootGrant;
                    ack.clientPlayerRootGrant = grant;
                    std::vector<uint8_t> ackBytes;
                    if (wire::encodeControlDatagram(ack, ackBytes)) {
                        (void)sendClientRootControl(rootEnvelope.nodeId,
                                                    ackBytes.data(),
                                                    ackBytes.size());
                    }
                }
            }
            continue;
        }
        if (decodedResult != TopologyControlResult::Accepted) continue;
        wire::ControlDatagram decoded{};
        if (wire::decodeControlDatagram(envelope.qsr.data(),
                                        envelope.qsr.size(), decoded)
            != wire::DecodeResult::Ok) {
            continue;
        }
        if (decoded.type
            == wire::ControlMessageType::RelayDataBindingRequest) {
            wire::RelayDataBindingAck ack{};
            wire::ControlDatagram response{};
            response.type = wire::ControlMessageType::RelayDataBindingAck;
            if (!_impl->authority->relayDataBindingAck(
                    envelope.nodeId, decoded.relayDataBindingRequest, ack)) {
                continue;
            }
            response.relayDataBindingAck = ack;
            std::vector<uint8_t> responseBytes;
            if (!wire::encodeControlDatagram(response, responseBytes))
                continue;
            (void)sendControl(ack.observedControlRoute,
                              responseBytes.data(), responseBytes.size());
            continue;
        }
        if (decoded.type == wire::ControlMessageType::RelayReady) {
            continue;
        }
        if (decoded.type
            == wire::ControlMessageType::RelayChildActivationReceipt) {
            continue;
        }
        if (decoded.type == wire::ControlMessageType::NodeAdvertisement) {
            model::NodeId stagedRelayNodeId = model::kInvalidNodeId;
            const StagedRelayChildActivationResult staged =
                _impl->authority->consumeStagedRelayChildActivation(
                    envelope.nodeId, nowMs, stagedRelayNodeId);
            if (staged != StagedRelayChildActivationResult::None) {
                if (staged == StagedRelayChildActivationResult::Ready) {
                    (void)issueRelayChildActivation(stagedRelayNodeId,
                                                    envelope.nodeId, nowMs);
                }
                continue;
            }
        }
        std::vector<uint8_t> normalizedQsr;
        const uint8_t* rootBytes = envelope.qsr.data();
        size_t rootLength = envelope.qsr.size();
        if (decoded.type == wire::ControlMessageType::NodeAdvertisement) {
            const bool relayRouteClaim =
                decoded.nodeAdvertisement.route.kind
                    == model::RouteKind::TrafficRelay
                || decoded.nodeAdvertisement.controlRoute.kind
                    == model::RouteKind::TrafficRelay;
            const bool relayIngressDatagram =
                from.ipv4BE == qnetplay::spectator::kRelayVirtualIpv4BE;
            if (relayRouteClaim != relayIngressDatagram) continue;
            model::NodeAdvertisement expected{};
            if (!_impl->authority->childAdvertisement(envelope.nodeId,
                                                       expected)) {
                continue;
            }
            decoded.nodeAdvertisement.controlRoute = expected.controlRoute;
            if (!wire::encodeControlDatagram(decoded, normalizedQsr))
                continue;
            rootBytes = normalizedQsr.data();
            rootLength = normalizedQsr.size();
        }
        if (!_impl->authority->authorize(from, decoded, nowMs)) continue;
        const resilient::ControlIngressResult result = _impl->root->onDatagram(
            from, rootBytes, rootLength, nowMs);
        if (result == resilient::ControlIngressResult::AcceptedAdvertisement) {
            uint64_t nonce = 0u;
            uint64_t expiry = 0u;
            if (_impl->authority->reserveRootOffer(envelope.nodeId, nowMs,
                                                    nonce, expiry)) {
                if (!_impl->root->issueAndSendParentOffer(
                        envelope.nodeId,
                        _impl->authority->requiredCursor(envelope.nodeId),
                        expiry, nonce)) {
                    _impl->authority->releaseOffer(envelope.nodeId, nonce);
                }
            } else {
                (void)_impl->root->resendParentOffer(envelope.nodeId);
            }
        } else if (result
                   == resilient::ControlIngressResult::AcceptedLeaseRedemption) {
            if (!_impl->authority->confirmRedemption(decoded.parentLease,
                                                      nowMs)) {
                (void)_impl->root->detachChild(decoded.parentLease.childId);
            } else if (!_impl->root->sendParentCommit(decoded.parentLease)) {
                (void)_impl->authority->rollbackRedemption(decoded.parentLease);
                (void)_impl->root->detachChild(decoded.parentLease.childId);
            }
        } else if (result == resilient::ControlIngressResult::LeaseRejected
                   && _impl->authority->isAttached(
                       decoded.parentLease.childId)) {
            (void)_impl->root->sendParentCommit(decoded.parentLease);
        }
    }
    (void)flushRelayChildRetirements();
    (void)flushRelayChildActivationRetirements();
    (void)flushRelayChildActivationCommits(nowMs);
    return true;
}

bool TopologyQsrControl::flushRelayChildRetirements() {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr
        || !_impl->authority->ready()) {
        return false;
    }
    for (size_t budget = 0u; budget < 32u; ++budget) {
        RelayChildRetirementRecipient recipient{};
        if (!_impl->authority->nextRelayChildRetirement(recipient))
            return true;
        if (recipient.qsrRoute.kind != model::RouteKind::Direct
            || !recipient.qsrRoute.valid()) {
            return false;
        }

        wire::ControlDatagram datagram{};
        datagram.type = wire::ControlMessageType::RelayChildRetirement;
        datagram.relayChildRetirement = recipient.retirement;
        std::vector<uint8_t> inner;
        if (!wire::encodeControlDatagram(datagram, inner)) return false;

        auto sequence = std::find_if(
            _impl->outbound.begin(), _impl->outbound.end(),
            [nodeId = recipient.retirement.relayNodeId](
                const Impl::OutboundSequence& value) {
                return value.nodeId == nodeId;
            });
        if (sequence == _impl->outbound.end()) {
            try {
                _impl->outbound.push_back(Impl::OutboundSequence{
                    recipient.retirement.relayNodeId, 1u});
            } catch (const std::bad_alloc&) {
                return false;
            }
            sequence = _impl->outbound.end() - 1;
        }
        uint64_t outboundSequence = 0u;
        if (!reserveTopologyControlSequence(sequence->next, outboundSequence))
            return false;
        std::vector<uint8_t> wrapped;
        if (!_impl->authority->encodeRelayChildRetirementControl(
                recipient, outboundSequence, inner.data(), inner.size(),
                wrapped)) {
            sequence->next = outboundSequence;
            return false;
        }
        qnet::Endpoint destination{};
        destination.sa.sin_addr.s_addr = recipient.qsrRoute.ipv4BE;
        destination.sa.sin_port = htons(recipient.qsrRoute.port);
        if (!_impl->socket.sendTo(destination, wrapped.data(), wrapped.size()))
            return false;
        if (!_impl->authority->acknowledgeRelayChildRetirement(recipient))
            return false;
    }
    return true;
}

bool TopologyQsrControl::flushRelayChildActivationRetirements() {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr
        || !_impl->authority->ready()) {
        return false;
    }
    for (size_t budget = 0u; budget < 32u; ++budget) {
        RelayChildActivationRetirementRecipient recipient{};
        if (!_impl->authority->nextRelayChildActivationRetirement(recipient))
            return true;
        if (recipient.qsrRoute.kind != model::RouteKind::Direct
            || !recipient.qsrRoute.valid()) {
            return false;
        }
        const wire::RelayChildActivationRetirement& retirement =
            recipient.retirement;
        model::NodeId recipientNodeId = model::kInvalidNodeId;
        if (sameRoute(recipient.qsrRoute, retirement.relayControlRoute)) {
            recipientNodeId = retirement.relayNodeId;
        } else if (sameRoute(recipient.qsrRoute,
                             retirement.childControlRoute)) {
            recipientNodeId = retirement.childNodeId;
        }
        if (recipientNodeId == model::kInvalidNodeId) return false;

        wire::ControlDatagram datagram{};
        datagram.type = wire::ControlMessageType::RelayChildActivationRetirement;
        datagram.relayChildActivationRetirement = retirement;
        std::vector<uint8_t> inner;
        if (!wire::encodeControlDatagram(datagram, inner)) return false;

        auto sequence = std::find_if(
            _impl->outbound.begin(), _impl->outbound.end(),
            [recipientNodeId](const Impl::OutboundSequence& value) {
                return value.nodeId == recipientNodeId;
            });
        if (sequence == _impl->outbound.end()) {
            try {
                _impl->outbound.push_back(
                    Impl::OutboundSequence{recipientNodeId, 1u});
            } catch (const std::bad_alloc&) {
                return false;
            }
            sequence = _impl->outbound.end() - 1;
        }
        uint64_t outboundSequence = 0u;
        if (!reserveTopologyControlSequence(sequence->next, outboundSequence))
            return false;
        std::vector<uint8_t> wrapped;
        if (!_impl->authority->encodeRelayChildActivationRetirementControl(
                recipient, outboundSequence, inner.data(), inner.size(),
                wrapped)) {
            sequence->next = outboundSequence;
            return false;
        }
        qnet::Endpoint destination{};
        destination.sa.sin_addr.s_addr = recipient.qsrRoute.ipv4BE;
        destination.sa.sin_port = htons(recipient.qsrRoute.port);
        if (!_impl->socket.sendTo(destination, wrapped.data(), wrapped.size()))
            return false;
        if (!_impl->authority->acknowledgeRelayChildActivationRetirement(
                recipient)) {
            return false;
        }
    }
    return true;
}

bool TopologyQsrControl::flushRelayChildActivationCommits(const uint64_t nowMs) {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr
        || !_impl->authority->ready()) {
        return false;
    }
    for (size_t budget = 0u; budget < 32u; ++budget) {
        RelayChildActivationCommitRecipient recipient{};
        if (!_impl->authority->nextRelayChildActivationCommit(recipient))
            return true;
        if (recipient.qsrRoute.kind != model::RouteKind::Direct
            || !recipient.qsrRoute.valid()) {
            return false;
        }
        const wire::RelayChildActivation& activation = recipient.commit.activation;
        model::NodeId recipientNodeId = model::kInvalidNodeId;
        if (sameRoute(recipient.qsrRoute, activation.relayControlRoute)) {
            recipientNodeId = activation.relayNodeId;
        } else if (sameRoute(recipient.qsrRoute,
                             activation.childControlRoute)) {
            recipientNodeId = activation.childNodeId;
        }
        if (recipientNodeId == model::kInvalidNodeId) return false;

        wire::ControlDatagram datagram{};
        datagram.type = wire::ControlMessageType::RelayChildActivationCommit;
        datagram.relayChildActivationCommit = recipient.commit;
        std::vector<uint8_t> inner;
        if (!wire::encodeControlDatagram(datagram, inner)) return false;

        auto sequence = std::find_if(
            _impl->outbound.begin(), _impl->outbound.end(),
            [recipientNodeId](const Impl::OutboundSequence& value) {
                return value.nodeId == recipientNodeId;
            });
        if (sequence == _impl->outbound.end()) {
            try {
                _impl->outbound.push_back(
                    Impl::OutboundSequence{recipientNodeId, 1u});
            } catch (const std::bad_alloc&) {
                return false;
            }
            sequence = _impl->outbound.end() - 1;
        }
        uint64_t outboundSequence = 0u;
        if (!reserveTopologyControlSequence(sequence->next, outboundSequence))
            return false;
        std::vector<uint8_t> wrapped;
        if (!_impl->authority->encodeRelayChildActivationCommitControl(
                recipient, outboundSequence, inner.data(), inner.size(),
                wrapped)) {
            sequence->next = outboundSequence;
            return false;
        }
        qnet::Endpoint destination{};
        destination.sa.sin_addr.s_addr = recipient.qsrRoute.ipv4BE;
        destination.sa.sin_port = htons(recipient.qsrRoute.port);
        if (!_impl->socket.sendTo(destination, wrapped.data(), wrapped.size()))
            return false;
        if (!_impl->authority->markRelayChildActivationCommitSent(
                recipient, nowMs)) {
            return false;
        }
    }
    return true;
}

void TopologyQsrControl::forgetNode(const model::NodeId nodeId) {
    if (!_impl || nodeId == model::kInvalidNodeId) return;
    _impl->outbound.erase(std::remove_if(
        _impl->outbound.begin(), _impl->outbound.end(),
        [nodeId](const Impl::OutboundSequence& value) {
            return value.nodeId == nodeId;
        }), _impl->outbound.end());
}

bool TopologyQsrControl::sendAuthorityState(
    const wire::AuthorityStateKind kind,
    const wire::AuthorityStateReason reason, const uint64_t nowMs,
    const model::NodeId onlyChild) {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr) {
        return false;
    }
    std::vector<AuthorityStateRecipient> recipients;
    if (!_impl->authority->collectAuthorityState(kind, reason, nowMs,
                                                  recipients, onlyChild)) {
        return false;
    }
    bool allSent = true;
    for (const AuthorityStateRecipient& recipient : recipients) {
        wire::ControlDatagram datagram{};
        datagram.type = wire::ControlMessageType::AuthorityState;
        datagram.authorityState = recipient.state;
        std::vector<uint8_t> bytes;
        const bool encoded = wire::encodeControlDatagram(datagram, bytes);
        const bool sent = encoded && sendControl(recipient.qsrRoute,
                                                 bytes.data(), bytes.size());
        allSent = allSent && sent;
    }
    return allSent;
}

bool TopologyQsrControl::issueRelayChildAssignment(
    const model::NodeId relayNodeId, const model::NodeId childNodeId,
    const uint64_t nowMs) {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr) {
        return false;
    }
    wire::RelayChildAssignment assignment{};
    model::RouteDescriptor relayQsrRoute{};
    if (!_impl->authority->issueRelayChildAssignment(
            relayNodeId, childNodeId, nowMs, assignment, relayQsrRoute)) {
        return false;
    }
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::RelayChildAssignment;
    datagram.relayChildAssignment = assignment;
    std::vector<uint8_t> bytes;
    return wire::encodeControlDatagram(datagram, bytes)
        && sendControl(relayQsrRoute, bytes.data(), bytes.size());
}

bool TopologyQsrControl::issueRelayChildActivation(
    const model::NodeId relayNodeId, const model::NodeId childNodeId,
    const uint64_t nowMs) {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr) {
        return false;
    }
    if (_impl->authority->stageRelayChildActivation(relayNodeId, childNodeId,
                                                     nowMs)) {
        return true;
    }
    wire::RelayChildActivation activation{};
    model::RouteDescriptor relayQsrRoute{};
    model::RouteDescriptor childQsrRoute{};
    if (!_impl->authority->issueRelayChildActivation(
            relayNodeId, childNodeId, nowMs, activation, relayQsrRoute,
            childQsrRoute)) {
        return false;
    }
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::RelayChildActivation;
    datagram.relayChildActivation = activation;
    std::vector<uint8_t> bytes;
    if (!wire::encodeControlDatagram(datagram, bytes)) return false;

    const bool relaySent = sendControl(relayQsrRoute, bytes.data(), bytes.size());
    const bool childSent = sendControl(childQsrRoute, bytes.data(), bytes.size());
    return relaySent && childSent;
}

bool TopologyQsrControl::sendClientRootControl(
    const model::NodeId clientRootNodeId, const uint8_t* bytes,
    const size_t length) {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr
        || clientRootNodeId == model::kInvalidNodeId || bytes == nullptr
        || length == 0u) {
        return false;
    }
    auto sequence = std::find_if(
        _impl->outbound.begin(), _impl->outbound.end(),
        [clientRootNodeId](const Impl::OutboundSequence& value) {
            return value.nodeId == clientRootNodeId;
        });
    if (sequence == _impl->outbound.end()) {
        try {
            _impl->outbound.push_back(
                Impl::OutboundSequence{clientRootNodeId, 1u});
        } catch (const std::bad_alloc&) {
            return false;
        }
        sequence = _impl->outbound.end() - 1;
    }
    uint64_t outboundSequence = 0u;
    if (!reserveTopologyControlSequence(sequence->next, outboundSequence))
        return false;
    std::vector<uint8_t> wrapped;
    model::RouteDescriptor route{};
    if (!_impl->authority->encodeClientPlayerRootControlToRoot(
            clientRootNodeId, outboundSequence, bytes, length, wrapped,
            route)) {
        sequence->next = outboundSequence;
        return false;
    }
    qnet::Endpoint destination{};
    destination.sa.sin_addr.s_addr = route.ipv4BE;
    destination.sa.sin_port = htons(route.port);
    return _impl->socket.sendTo(destination, wrapped.data(), wrapped.size());
}

bool TopologyQsrControl::issueClientPlayerRootActivation(
    const model::NodeId clientRootNodeId,
    const model::NodeId targetViewerNodeId, const uint64_t nowMs) {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr) {
        return false;
    }
    wire::ClientPlayerRootActivation activation{};
    model::RouteDescriptor targetQsrRoute{};
    if (!_impl->authority->issueClientPlayerRootActivation(
            clientRootNodeId, targetViewerNodeId, nowMs, activation,
            targetQsrRoute)) {
        return false;
    }
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::ClientPlayerRootActivation;
    datagram.clientPlayerRootActivation = activation;
    std::vector<uint8_t> bytes;
    if (!wire::encodeControlDatagram(datagram, bytes)) return false;
    const bool viewerSent = sendControl(targetQsrRoute, bytes.data(),
                                        bytes.size());
    const bool rootSent = sendClientRootControl(clientRootNodeId,
                                                bytes.data(), bytes.size());
    return viewerSent && rootSent;
}

bool TopologyQsrControl::flushClientPlayerRootActivationCommits(
    const uint64_t nowMs) {
    (void)nowMs;
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr
        || !_impl->authority->ready()) {
        return false;
    }
    for (size_t budget = 0u; budget < 32u; ++budget) {
        ClientPlayerRootActivationCommitRecipient recipient{};
        if (!_impl->authority->nextClientPlayerRootActivationCommit(recipient))
            return true;
        if (recipient.qsrRoute.kind != model::RouteKind::Direct
            || !recipient.qsrRoute.valid()) {
            return false;
        }
        wire::ControlDatagram datagram{};
        datagram.type =
            wire::ControlMessageType::ClientPlayerRootActivationCommit;
        datagram.clientPlayerRootActivationCommit = recipient.commit;
        std::vector<uint8_t> inner;
        if (!wire::encodeControlDatagram(datagram, inner)) return false;

        const model::NodeId viewerNodeId =
            recipient.commit.activation.targetViewerNodeId;
        auto sequence = std::find_if(
            _impl->outbound.begin(), _impl->outbound.end(),
            [viewerNodeId](const Impl::OutboundSequence& value) {
                return value.nodeId == viewerNodeId;
            });
        if (sequence == _impl->outbound.end()) {
            try {
                _impl->outbound.push_back(
                    Impl::OutboundSequence{viewerNodeId, 1u});
            } catch (const std::bad_alloc&) {
                return false;
            }
            sequence = _impl->outbound.end() - 1;
        }
        uint64_t outboundSequence = 0u;
        if (!reserveTopologyControlSequence(sequence->next, outboundSequence))
            return false;
        std::vector<uint8_t> wrapped;
        if (!_impl->authority->encodeClientPlayerRootActivationCommitControl(
                recipient, outboundSequence, inner.data(), inner.size(),
                wrapped)) {
            sequence->next = outboundSequence;
            return false;
        }
        qnet::Endpoint destination{};
        destination.sa.sin_addr.s_addr = recipient.qsrRoute.ipv4BE;
        destination.sa.sin_port = htons(recipient.qsrRoute.port);
        if (!_impl->socket.sendTo(destination, wrapped.data(), wrapped.size()))
            return false;
        if (!sendClientRootControl(
                recipient.commit.activation.binding.clientRootNodeId,
                inner.data(), inner.size())) {
            return false;
        }
        if (!_impl->authority->acknowledgeClientPlayerRootActivationCommit(
                recipient)) {
            return false;
        }
    }
    return true;
}

bool TopologyQsrControl::flushClientPlayerRootActivationRetirements() {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr
        || !_impl->authority->ready()) {
        return false;
    }
    for (size_t budget = 0u; budget < 32u; ++budget) {
        ClientPlayerRootActivationRetirementRecipient recipient{};
        if (!_impl->authority->nextClientPlayerRootActivationRetirement(
                recipient)) {
            return true;
        }
        if (recipient.qsrRoute.kind != model::RouteKind::Direct
            || !recipient.qsrRoute.valid()) {
            return false;
        }
        wire::ControlDatagram datagram{};
        datagram.type =
            wire::ControlMessageType::ClientPlayerRootActivationRetirement;
        datagram.clientPlayerRootActivationRetirement = recipient.retirement;
        std::vector<uint8_t> inner;
        if (!wire::encodeControlDatagram(datagram, inner)) return false;

        const model::NodeId viewerNodeId =
            recipient.retirement.activation.targetViewerNodeId;
        auto sequence = std::find_if(
            _impl->outbound.begin(), _impl->outbound.end(),
            [viewerNodeId](const Impl::OutboundSequence& value) {
                return value.nodeId == viewerNodeId;
            });
        if (sequence == _impl->outbound.end()) {
            try {
                _impl->outbound.push_back(
                    Impl::OutboundSequence{viewerNodeId, 1u});
            } catch (const std::bad_alloc&) {
                return false;
            }
            sequence = _impl->outbound.end() - 1;
        }
        uint64_t outboundSequence = 0u;
        if (!reserveTopologyControlSequence(sequence->next, outboundSequence))
            return false;
        std::vector<uint8_t> wrapped;
        if (!_impl->authority
                 ->encodeClientPlayerRootActivationRetirementControl(
                     recipient, outboundSequence, inner.data(), inner.size(),
                     wrapped)) {
            sequence->next = outboundSequence;
            return false;
        }
        qnet::Endpoint destination{};
        destination.sa.sin_addr.s_addr = recipient.qsrRoute.ipv4BE;
        destination.sa.sin_port = htons(recipient.qsrRoute.port);
        if (!_impl->socket.sendTo(destination, wrapped.data(), wrapped.size()))
            return false;
        (void)sendClientRootControl(
            recipient.retirement.activation.binding.clientRootNodeId,
            inner.data(), inner.size());
        if (!_impl->authority->acknowledgeClientPlayerRootActivationRetirement(
                recipient)) {
            return false;
        }
    }
    return true;
}

bool TopologyQsrControl::sendControl(const model::RouteDescriptor& to,
                                     const uint8_t* bytes,
                                     const size_t length) {
    if (!_impl || !_impl->socket.isOpen() || _impl->authority == nullptr
        || bytes == nullptr || length == 0u || !to.valid()) {
        return false;
    }
    const RouteEndpoint endpoint{to.ipv4BE, to.port};
    TopologyEnrollment enrollment{};
    if (!_impl->authority->enrollmentForQsrEndpoint(endpoint, enrollment))
        return false;
    auto sequence = std::find_if(
        _impl->outbound.begin(), _impl->outbound.end(),
        [nodeId = enrollment.nodeId](const Impl::OutboundSequence& value) {
            return value.nodeId == nodeId;
        });
    if (sequence == _impl->outbound.end()) {
        try {
            _impl->outbound.push_back(Impl::OutboundSequence{enrollment.nodeId, 1u});
        } catch (const std::bad_alloc&) {
            return false;
        }
        sequence = _impl->outbound.end() - 1;
    }
    uint64_t outboundSequence = 0u;
    if (!reserveTopologyControlSequence(sequence->next, outboundSequence))
        return false;
    std::vector<uint8_t> wrapped;
    if (!_impl->authority->encodeControl(enrollment, outboundSequence, bytes,
                                         length, wrapped)) {
        sequence->next = outboundSequence;
        return false;
    }
    if (endpoint.ipv4BE == qnetplay::spectator::kRelayVirtualIpv4BE) {
        if (!_impl->relayLaneActive()
            || _impl->relayQsrVirtualPort(enrollment.nodeId)
                != endpoint.port) {
            return false;
        }
        std::vector<uint8_t> framed;
        if (!_impl->relayAdapter.encodeOutbound(enrollment.nodeId,
                                                wrapped.data(),
                                                wrapped.size(), framed)) {
            return false;
        }
        return _impl->socket.sendTo(_impl->relayEndpoint, framed.data(),
                                    framed.size());
    }
    qnet::Endpoint destination{};
    destination.sa.sin_addr.s_addr = endpoint.ipv4BE;
    destination.sa.sin_port = htons(endpoint.port);
    return _impl->socket.sendTo(destination, wrapped.data(), wrapped.size());
}

}
