#include "client/spectator/ResilientTopologyClientRuntime.h"

#include "client/spectator/ResilientSpectatorNodeRuntime.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "net/SecureRandom.h"
#include "spectator/ResilientSpectatorProtocol.h"
#include "spectator/SpectatorRelayAdapter.h"

namespace qclient::spectator::topology::client {
using qnet::SpectateTopologyCursorWire;
using qnet::SpectateTopologyChallengeV2;
using qnet::SpectateTopologyEndpointBindingWire;
using qnet::SpectateTopologyInfoV2;
using qnet::SpectateTopologyProofV2;
using qnet::SpectateTopologyQtaLimitsWire;
using qnet::SpectatorGrantWire;
using qnet::kSpectateTopologyControlRequiredFeatures;
using qnet::kSpectateTopologyControlSchemaVersion;

namespace {

constexpr uint8_t kTopologyControlType =
    qnetplay::spectator::resilient::wire::kTopologyQtaEnvelopeControlType;
constexpr size_t kTopologyEnvelopePrefixBytes =
    qnetplay::spectator::resilient::wire::kTopologyQtaEnvelopePrefixBytes;
static_assert(kTopologyEnvelopePrefixBytes == 52u,
              "the QTA envelope prefix is a frozen wire shape");
constexpr char kTopologyCredentialDomain[] = "qoh99-topology-qsr-v1";
constexpr uint64_t kTopologyControlSetupTimeoutMs = 10000u;
constexpr uint64_t kTopologyControlResponseTimeoutMs = 1000u;
constexpr uint32_t kTopologyControlMaxRestarts = 4u;
constexpr uint64_t kTopologyInitialLivenessWindowMs = 5000u;
constexpr uint32_t kRelayDataBindingMaxAttempts = 5u;
constexpr uint64_t kRelayDataBindingRetryMs = 250u;
constexpr uint32_t kRelayReadyMaxAttempts = 5u;
constexpr uint64_t kRelayReadyRetryMs = 250u;

uint64_t addSaturating(const uint64_t left, const uint64_t right) {
    return left > (std::numeric_limits<uint64_t>::max)() - right
        ? (std::numeric_limits<uint64_t>::max)() : left + right;
}

void wipeBytes(void* bytes, size_t count) {
    volatile uint8_t* cursor = static_cast<volatile uint8_t*>(bytes);
    while (cursor != nullptr && count != 0u) {
        *cursor++ = 0u;
        --count;
    }
}

void wipeVector(std::vector<uint8_t>& bytes) {
    if (!bytes.empty()) wipeBytes(bytes.data(), bytes.size());
    bytes.clear();
}

bool allZero(const uint8_t* bytes, const size_t count) {
    if (bytes == nullptr || count == 0u) return true;
    uint8_t aggregate = 0u;
    for (size_t index = 0u; index < count; ++index) aggregate |= bytes[index];
    return aggregate == 0u;
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

bool routeKindValid(const uint8_t kind) {
    return kind == static_cast<uint8_t>(model::RouteKind::Direct)
        || kind == static_cast<uint8_t>(model::RouteKind::Punched);
}

bool endpointBindingWireValid(const SpectateTopologyEndpointBindingWire& binding) {
    return binding.localPort != 0u
        && routeKindValid(binding.advertisedRouteKind)
        && binding.advertisedIpv4BE != 0u
        && binding.advertisedPort != 0u
        && binding.advertisedRouteGeneration != 0u
        && binding.advertisedRouteProved <= 1u
        && binding.localProvenance >= static_cast<uint8_t>(EndpointProvenance::LocalBind)
        && binding.localProvenance
            <= static_cast<uint8_t>(EndpointProvenance::ProvenPunchedQsp)
        && binding.advertisedProvenance
            >= static_cast<uint8_t>(EndpointProvenance::LocalBind)
        && binding.advertisedProvenance
            <= static_cast<uint8_t>(EndpointProvenance::ProvenPunchedQsp)
        && binding.bindingGeneration != 0u
        && binding.advertisedRouteGeneration == binding.bindingGeneration;
}

bool routeEqualsWire(const model::RouteDescriptor& route,
                     const SpectateTopologyEndpointBindingWire& binding) {
    return route.kind == static_cast<model::RouteKind>(
                             binding.advertisedRouteKind)
        && route.ipv4BE == binding.advertisedIpv4BE
        && route.port == binding.advertisedPort
        && route.generation == binding.advertisedRouteGeneration
        && route.routeProved == (binding.advertisedRouteProved != 0u);
}

bool sameRoute(const model::RouteDescriptor& left,
               const model::RouteDescriptor& right) {
    return left.kind == right.kind && left.ipv4BE == right.ipv4BE
        && left.port == right.port && left.generation == right.generation
        && left.routeProved == right.routeProved;
}

model::RouteDescriptor routeFromWire(
    const SpectateTopologyEndpointBindingWire& binding) {
    model::RouteDescriptor route{};
    route.kind = static_cast<model::RouteKind>(binding.advertisedRouteKind);
    route.ipv4BE = binding.advertisedIpv4BE;
    route.port = binding.advertisedPort;
    route.generation = binding.advertisedRouteGeneration;
    route.routeProved = binding.advertisedRouteProved != 0u;
    return route;
}

bool streamEquals(const model::StreamCompatibilityIdentity& stream,
                  const qnetplay::compat::CompatibilityIdentityWire& compatibility,
                  const uint64_t sessionId, const uint64_t epoch,
                  const uint64_t rootIdentity) {
    return stream.setOrSessionId == sessionId && stream.gameEpoch == epoch
        && stream.streamRootIdentity == rootIdentity
        && qnetplay::compat::semanticallyEqual(stream.compatibility,
                                                compatibility);
}

bool sameLeaseBinding(const model::LeaseBinding& binding,
                      const model::ParentLease& lease) {
    return binding.childId == lease.childId && binding.parentId == lease.parentId
        && binding.parentSourceGeneration == lease.parentSourceGeneration
        && binding.routeGeneration == lease.routeGeneration
        && binding.expiryMs == lease.expiryMs
        && binding.replayNonce == lease.replayNonce
        && binding.macLow == lease.macLow && binding.macHigh == lease.macHigh
        && binding.childBinding == lease.childBinding;
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

bool sameParentLease(const model::ParentLease& left,
                     const model::ParentLease& right) {
    if (!streamEquals(left.stream, right.stream.compatibility,
                      right.stream.setOrSessionId, right.stream.gameEpoch,
                      right.stream.streamRootIdentity)
        || left.childId != right.childId || left.parentId != right.parentId
        || left.childBinding != right.childBinding
        || left.parentDepth != right.parentDepth
        || left.parentSourceGeneration != right.parentSourceGeneration
        || left.routeGeneration != right.routeGeneration
        || left.routeCount != right.routeCount
        || left.cacheRange.firstCursor != right.cacheRange.firstCursor
        || left.cacheRange.nextCursor != right.cacheRange.nextCursor
        || left.latestVerifiedCursor != right.latestVerifiedCursor
        || left.childCapacity != right.childCapacity
        || left.expiryMs != right.expiryMs
        || left.replayNonce != right.replayNonce
        || left.ancestryCount != right.ancestryCount
        || left.macLow != right.macLow || left.macHigh != right.macHigh) {
        return false;
    }
    for (size_t index = 0u; index < left.routeCount; ++index) {
        const model::RouteDescriptor& a = left.routes[index];
        const model::RouteDescriptor& b = right.routes[index];
        if (a.kind != b.kind || a.ipv4BE != b.ipv4BE || a.port != b.port
            || a.generation != b.generation
            || a.routeProved != b.routeProved) {
            return false;
        }
    }
    for (size_t index = 0u; index < left.ancestryCount; ++index)
        if (left.ancestry[index] != right.ancestry[index]) return false;
    return true;
}

bool hasExactDerivedChildLeaseChain(
    const model::StreamCompatibilityIdentity& stream,
    const uint64_t authorityGeneration, const model::NodeId rootNodeId,
    const model::NodeId relayNodeId, const model::NodeId childNodeId,
    const model::LeaseBinding& relayLease,
    const model::ParentLease& childLease,
    const model::RouteDescriptor& relayDataRoute,
    const model::RouteDescriptor& childDataRoute,
    const uint8_t maxActiveDepth,
    const model::ParentLease* const exactRelayLease = nullptr) {
    if (maxActiveDepth == 0u || authorityGeneration == 0u
        || rootNodeId == model::kInvalidNodeId
        || relayNodeId == model::kInvalidNodeId
        || childNodeId == model::kInvalidNodeId
        || rootNodeId == relayNodeId || rootNodeId == childNodeId
        || relayNodeId == childNodeId || !(childLease.stream == stream)
        || relayLease.childId != relayNodeId
        || relayLease.parentId == model::kInvalidNodeId
        || relayLease.parentId == relayNodeId
        || childLease.childId != childNodeId
        || childLease.parentId != relayNodeId
        || !relayLease.childBinding.valid()
        || !childLease.childBinding.valid()
        || relayLease.childBinding.authorityGeneration != authorityGeneration
        || childLease.childBinding.authorityGeneration != authorityGeneration
        || relayLease.parentSourceGeneration == 0u
        || relayLease.parentSourceGeneration
            != childLease.parentSourceGeneration
        || relayLease.routeGeneration == 0u || childLease.routeGeneration == 0u
        || childLease.routeCount != 1u || !childLease.routes[0].valid()
        || childLease.routes[0].generation != childLease.routeGeneration
        || !relayDataRoute.valid() || !relayDataRoute.routeProved
        || !childDataRoute.valid() || !childDataRoute.routeProved
        || !sameRoute(childLease.routes[0], relayDataRoute)
        || childLease.routeGeneration != relayDataRoute.generation
        || !sameRoute(childLease.childBinding.childQspRoute, childDataRoute)
        || relayLease.replayNonce == 0u || childLease.replayNonce == 0u
        || relayLease.expiryMs == 0u || childLease.expiryMs == 0u
        || childLease.expiryMs > relayLease.expiryMs
        || childLease.cacheRange.firstCursor > childLease.cacheRange.nextCursor
        || !childLease.cacheRange.containsBoundary(
            childLease.latestVerifiedCursor)
        || childLease.latestVerifiedCursor
            < childLease.childBinding.childRequestedCursor.nextRecord) {
        return false;
    }

    const uint16_t childParentDepth = childLease.parentDepth;
    if (childParentDepth + 1u > maxActiveDepth) {
        return false;
    }

    const size_t childAncestryCount = childLease.ancestryCount;
    if (childAncestryCount == 0u
        || childAncestryCount > childLease.ancestry.size()
        || childAncestryCount
            != static_cast<size_t>(childLease.parentDepth) + 1u
        || childLease.ancestry[0] != rootNodeId
        || childLease.ancestry[childAncestryCount - 1u] != relayNodeId) {
        return false;
    }

    for (size_t index = 0u; index < childAncestryCount; ++index) {
        const model::NodeId ancestor = childLease.ancestry[index];
        if (ancestor == model::kInvalidNodeId || ancestor == childNodeId
            || (index + 1u < childAncestryCount
                && ancestor == relayNodeId)) {
            return false;
        }
        for (size_t previous = 0u; previous < index; ++previous) {
            if (childLease.ancestry[previous] == ancestor) return false;
        }
    }

    if (exactRelayLease != nullptr) {
        const size_t relayAncestryCount = exactRelayLease->ancestryCount;
        if (!(exactRelayLease->stream == stream)
            || !sameLeaseBinding(relayLease, *exactRelayLease)
            || exactRelayLease->childId != relayNodeId
            || exactRelayLease->parentId == model::kInvalidNodeId
            || exactRelayLease->parentId == relayNodeId
            || exactRelayLease->parentDepth == (std::numeric_limits<uint8_t>::max)()
            || static_cast<uint16_t>(exactRelayLease->parentDepth) + 1u
                != childParentDepth
            || relayAncestryCount == 0u
            || relayAncestryCount > exactRelayLease->ancestry.size()
            || relayAncestryCount
                != static_cast<size_t>(exactRelayLease->parentDepth) + 1u
            || childAncestryCount != relayAncestryCount + 1u
            || exactRelayLease->ancestry[0] != rootNodeId
            || exactRelayLease->ancestry[relayAncestryCount - 1u]
                != exactRelayLease->parentId) {
            return false;
        }
        for (size_t index = 0u; index < relayAncestryCount; ++index) {
            if (exactRelayLease->ancestry[index] != childLease.ancestry[index]
                || exactRelayLease->ancestry[index] == relayNodeId) {
                return false;
            }
        }
    }
    return true;
}

bool hasExactDerivedRelayChildActivationLeaseChain(
    const qnetplay::spectator::resilient::wire::RelayChildActivation&
        activation,
    const uint8_t maxActiveDepth,
    const model::ParentLease* const exactRelayLease = nullptr) {
    const model::ParentLease& childLease = activation.childLease;
    return hasExactDerivedChildLeaseChain(
               activation.stream, activation.authorityGeneration,
               activation.rootNodeId, activation.relayNodeId,
               activation.childNodeId, activation.relayLease, childLease,
               activation.relayDataRoute, activation.childDataRoute,
               maxActiveDepth, exactRelayLease)
        && activation.relayCacheRange.firstCursor
            <= activation.relayCacheRange.nextCursor
        && activation.relayCacheRange.containsBoundary(
            activation.commonCursor.nextRecord)
        && childLease.cacheRange.firstCursor
            == activation.relayCacheRange.firstCursor
        && childLease.cacheRange.nextCursor
            == activation.relayCacheRange.nextCursor
        && childLease.cacheRange.containsBoundary(
            activation.commonCursor.nextRecord)
        && childLease.latestVerifiedCursor
            == activation.commonCursor.nextRecord
        && childLease.childBinding.childRequestedCursor.nextRecord
            == activation.commonCursor.nextRecord
        && childLease.childBinding.childRequestedCursor.nextEvent
            == activation.commonCursor.nextEvent
        && childLease.childBinding.childRequestedCursor.digest
            == activation.commonCursor.digest
        && activation.activationNonce != 0u
        && activation.activationDeadlineMs != 0u
        && activation.activationDeadlineMs <= childLease.expiryMs
        && activation.activationDeadlineMs <= activation.relayLease.expiryMs;
}

bool hasExactAttachedRelayLineage(const model::ParentLease& lease,
                                  const model::NodeId rootNodeId,
                                  const model::NodeId localNodeId,
                                  const uint8_t maxActiveDepth) {
    const size_t ancestryCount = lease.ancestryCount;
    if (maxActiveDepth == 0u || rootNodeId == model::kInvalidNodeId
        || localNodeId == model::kInvalidNodeId
        || lease.childId != localNodeId
        || lease.parentId == model::kInvalidNodeId
        || lease.parentId == localNodeId
        || static_cast<uint16_t>(lease.parentDepth) + 1u
            >= maxActiveDepth
        || ancestryCount == 0u || ancestryCount > lease.ancestry.size()
        || ancestryCount != static_cast<size_t>(lease.parentDepth) + 1u
        || lease.ancestry[0] != rootNodeId
        || lease.ancestry[ancestryCount - 1u] != lease.parentId) {
        return false;
    }
    for (size_t index = 0u; index < lease.ancestry.size(); ++index) {
        const model::NodeId ancestor = lease.ancestry[index];
        if (index >= ancestryCount) {
            if (ancestor != model::kInvalidNodeId) return false;
            continue;
        }
        if (ancestor == model::kInvalidNodeId || ancestor == localNodeId
            || (index + 1u < ancestryCount && ancestor == lease.parentId)) {
            return false;
        }
        for (size_t previous = 0u; previous < index; ++previous) {
            if (lease.ancestry[previous] == ancestor) return false;
        }
    }
    return true;
}

bool sameRelayChildActivation(
    const qnetplay::spectator::resilient::wire::RelayChildActivation& left,
    const qnetplay::spectator::resilient::wire::RelayChildActivation& right) {
    return left.stream == right.stream
        && left.authorityGeneration == right.authorityGeneration
        && left.rootNodeId == right.rootNodeId
        && left.relayNodeId == right.relayNodeId
        && left.childNodeId == right.childNodeId
        && sameLeaseBinding(left.relayLease, right.relayLease)
        && left.relayBindingRequestNonce == right.relayBindingRequestNonce
        && sameRoute(left.relayDataRoute, right.relayDataRoute)
        && sameRoute(left.relayControlRoute, right.relayControlRoute)
        && sameParentLease(left.childLease, right.childLease)
        && sameRoute(left.childDataRoute, right.childDataRoute)
        && sameRoute(left.childControlRoute, right.childControlRoute)
        && left.relayCacheRange.firstCursor
            == right.relayCacheRange.firstCursor
        && left.relayCacheRange.nextCursor
            == right.relayCacheRange.nextCursor
        && left.commonCursor.nextRecord == right.commonCursor.nextRecord
        && left.commonCursor.nextEvent == right.commonCursor.nextEvent
        && left.commonCursor.digest == right.commonCursor.digest
        && left.activationNonce == right.activationNonce
        && left.activationDeadlineMs == right.activationDeadlineMs
        && left.hostSignature == right.hostSignature;
}

bool matchesRelayChildActivationRetirement(
    const qnetplay::spectator::resilient::wire::
        RelayChildActivationRetirement& retirement,
    const qnetplay::spectator::resilient::wire::RelayChildActivation&
        activation) {
    return retirement.stream == activation.stream
        && retirement.authorityGeneration == activation.authorityGeneration
        && retirement.rootNodeId == activation.rootNodeId
        && retirement.relayNodeId == activation.relayNodeId
        && retirement.childNodeId == activation.childNodeId
        && sameLeaseBinding(retirement.relayLease, activation.relayLease)
        && sameLeaseBinding(retirement.childLease,
                            leaseBindingOf(activation.childLease))
        && retirement.relayBindingRequestNonce
            == activation.relayBindingRequestNonce
        && sameRoute(retirement.relayDataRoute, activation.relayDataRoute)
        && sameRoute(retirement.relayControlRoute,
                     activation.relayControlRoute)
        && sameRoute(retirement.childDataRoute, activation.childDataRoute)
        && sameRoute(retirement.childControlRoute,
                     activation.childControlRoute)
        && retirement.relayCacheRange.firstCursor
            == activation.relayCacheRange.firstCursor
        && retirement.relayCacheRange.nextCursor
            == activation.relayCacheRange.nextCursor
        && retirement.commonCursor.nextRecord == activation.commonCursor.nextRecord
        && retirement.commonCursor.nextEvent == activation.commonCursor.nextEvent
        && retirement.commonCursor.digest == activation.commonCursor.digest
        && retirement.activationNonce == activation.activationNonce
        && retirement.activationDeadlineMs == activation.activationDeadlineMs;
}

qnetplay::spectator::resilient::wire::RelayChildActivationRetirement
retirementBindingOf(
    const qnetplay::spectator::resilient::wire::RelayChildActivation&
        activation) {
    using qnetplay::spectator::resilient::wire::RelayChildActivationRetirement;

    RelayChildActivationRetirement retirement{};
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
    return retirement;
}

bool sameRelayChildActivationRetirementBinding(
    const qnetplay::spectator::resilient::wire::
        RelayChildActivationRetirement& left,
    const qnetplay::spectator::resilient::wire::
        RelayChildActivationRetirement& right) {
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
        && left.relayCacheRange.firstCursor
            == right.relayCacheRange.firstCursor
        && left.relayCacheRange.nextCursor
            == right.relayCacheRange.nextCursor
        && left.commonCursor.nextRecord == right.commonCursor.nextRecord
        && left.commonCursor.nextEvent == right.commonCursor.nextEvent
        && left.commonCursor.digest == right.commonCursor.digest
        && left.activationNonce == right.activationNonce
        && left.activationDeadlineMs == right.activationDeadlineMs;
}

bool sameClientPlayerRootRuntimeReceipt(
    const qnetplay::spectator::resilient::wire::ClientPlayerRootRuntimeReceipt&
        left,
    const qnetplay::spectator::resilient::wire::ClientPlayerRootRuntimeReceipt&
        right) {
    return left.processId == right.processId
        && left.publisherGeneration == right.publisherGeneration
        && left.publisherNonce == right.publisherNonce
        && left.playerSlot == right.playerSlot
        && left.runtimeStage == right.runtimeStage
        && left.delay == right.delay && left.rollback == right.rollback
        && left.remoteDelay == right.remoteDelay
        && left.remoteRollback == right.remoteRollback
        && std::memcmp(&left.provenance, &right.provenance,
                       sizeof(left.provenance)) == 0;
}

bool sameClientPlayerRootBinding(
    const qnetplay::spectator::resilient::wire::ClientPlayerRootBinding& left,
    const qnetplay::spectator::resilient::wire::ClientPlayerRootBinding&
        right) {
    return left.stream == right.stream
        && left.gameStartFingerprint == right.gameStartFingerprint
        && left.clientRootNodeId == right.clientRootNodeId
        && left.remotePlayerRole == right.remotePlayerRole
        && std::memcmp(&left.configurationProvenance,
                       &right.configurationProvenance,
                       sizeof(left.configurationProvenance)) == 0
        && sameClientPlayerRootRuntimeReceipt(left.runtimeReceipt,
                                              right.runtimeReceipt)
        && left.sourceGeneration == right.sourceGeneration
        && left.qspRouteGeneration == right.qspRouteGeneration
        && left.qsrBindingGeneration == right.qsrBindingGeneration
        && sameRoute(left.dataRoute, right.dataRoute)
        && sameRoute(left.qsrRoute, right.qsrRoute)
        && left.cacheRange.firstCursor == right.cacheRange.firstCursor
        && left.cacheRange.nextCursor == right.cacheRange.nextCursor
        && left.cursor.nextRecord == right.cursor.nextRecord
        && left.cursor.nextEvent == right.cursor.nextEvent
        && left.cursor.digest == right.cursor.digest
        && left.childCapacity == right.childCapacity
        && left.expiresMs == right.expiresMs;
}

bool sameClientPlayerRootActivation(
    const qnetplay::spectator::resilient::wire::ClientPlayerRootActivation&
        left,
    const qnetplay::spectator::resilient::wire::ClientPlayerRootActivation&
        right) {
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

bool hasExactClientPlayerRootActivationLeaseChain(
    const qnetplay::spectator::resilient::wire::ClientPlayerRootActivation&
        activation) {
    const auto& binding = activation.binding;
    const model::ParentLease& lease = activation.targetLease;
    return activation.authorityGeneration != 0u
        && activation.hostRootNodeId != model::kInvalidNodeId
        && binding.clientRootNodeId != model::kInvalidNodeId
        && binding.clientRootNodeId != activation.hostRootNodeId
        && activation.targetViewerNodeId != model::kInvalidNodeId
        && activation.targetViewerNodeId != activation.hostRootNodeId
        && activation.targetViewerNodeId != binding.clientRootNodeId
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
        && sameRoute(activation.targetDataRoute,
                     lease.childBinding.childQspRoute)
        && activation.targetQsrRoute.valid()
        && activation.targetQsrRoute.routeProved
        && activation.targetQsrRoute.kind == model::RouteKind::Direct
        && activation.targetQsrRoute.generation
            == lease.childBinding.childEndpointBindingGeneration
        && !binding.dataRoute.sameEndpoint(activation.targetDataRoute)
        && !binding.dataRoute.sameEndpoint(activation.targetQsrRoute)
        && !binding.qsrRoute.sameEndpoint(activation.targetDataRoute)
        && !binding.qsrRoute.sameEndpoint(activation.targetQsrRoute);
}

bool hasPairedActivationControlType(const uint8_t* const bytes,
                                    const size_t length) {
    using qnetplay::spectator::resilient::wire::ControlMessageType;
    if (bytes == nullptr
        || length < qnetplay::spectator::resilient::wire::
            kResilientControlHeaderBytes
        || !qnetplay::spectator::resilient::wire::hasResilientControlMagic(
            bytes, length)) {
        return false;
    }
    const uint8_t rawType = bytes[6u];
    return rawType == static_cast<uint8_t>(
                          ControlMessageType::RelayChildActivation)
        || rawType == static_cast<uint8_t>(
                          ControlMessageType::RelayChildActivationRetirement)
        || rawType == static_cast<uint8_t>(
                          ControlMessageType::RelayChildActivationCommit)
        || rawType == static_cast<uint8_t>(
                          ControlMessageType::ClientPlayerRootActivation)
        || rawType == static_cast<uint8_t>(
                          ControlMessageType::
                              ClientPlayerRootActivationRetirement)
        || rawType == static_cast<uint8_t>(
                          ControlMessageType::ClientPlayerRootActivationCommit);
}

SpectatorGrantWire grantToWire(const qnetplay::spectator::SpectatorGrant& grant) {
    SpectatorGrantWire wire{};
    wire.grantId = grant.grantId;
    wire.viewerId = grant.viewerId;
    wire.sessionId = grant.sessionId;
    wire.expiryUnix = grant.expiryUnix;
    wire.sourceGeneration = grant.sourceGeneration;
    wire.resumeRecord = grant.resumeRecord;
    wire.resumeDigest = grant.resumeDigest;
    wire.route = grant.route;
    wire.macLow = grant.macLow;
    wire.macHigh = grant.macHigh;
    return wire;
}

bool copyInfoBodyWithoutTag(const SpectateTopologyInfoV2& info,
                            std::vector<uint8_t>& canonical) {
    return makeTopologyControlV2CanonicalInput(
        TopologyControlV2Direction::Response,
        reinterpret_cast<const uint8_t*>(&info),
        offsetof(SpectateTopologyInfoV2, responseTag), canonical);
}

bool copyProofBodyWithoutTag(const SpectateTopologyProofV2& proof,
                             std::vector<uint8_t>& canonical) {
    return makeTopologyControlV2CanonicalInput(
        TopologyControlV2Direction::Request,
        reinterpret_cast<const uint8_t*>(&proof),
        offsetof(SpectateTopologyProofV2, requestTag), canonical);
}

bool endpointValid(const qnet::Endpoint& endpoint) {
    return endpoint.sa.sin_family == AF_INET
        && endpoint.sa.sin_addr.s_addr != 0u && endpoint.sa.sin_port != 0u;
}

}

struct ReleasedTopologyClientRuntime::TranscriptFacts {
    uint64_t sessionId = 0u;
    uint64_t sessionEpoch = 0u;
    uint64_t spectatorCookieLow = 0u;
    uint64_t spectatorCookieHigh = 0u;
    qnetplay::compat::CompatibilityIdentityWire compatibility{};
    uint64_t streamRootIdentity = 0u;
    uint64_t authorityGeneration = 0u;
    uint64_t rootNodeId = model::kInvalidNodeId;
    SpectateTopologyEndpointBindingWire rootQsp{};
    SpectateTopologyEndpointBindingWire rootQsr{};
    uint64_t sourceGeneration = 0u;
    uint64_t childNodeId = model::kInvalidNodeId;
    uint64_t viewerId = 0u;
    uint32_t viewerGeneration = 0u;
    uint32_t qspIpv4BE = 0u;
    uint16_t qspPort = 0u;
    uint8_t qspProvenance = 0u;
    uint64_t qspRouteGeneration = 0u;
    uint64_t bindingGeneration = 0u;
    uint32_t confirmedCursor = 0u;
    uint64_t expiryMs = 0u;
    SpectateTopologyCursorWire cursor{};
    SpectateTopologyQtaLimitsWire limits{};
    TopologyAuthorityPublicBlob assignmentSigningPublicKey{};
};

ReleasedTopologyClientRuntime::ReleasedTopologyClientRuntime() = default;

class ReleasedTopologyClientRuntime::TranscriptBuilder final
    : public QtaMacInputBuilder {
public:
    explicit TranscriptBuilder(const TranscriptFacts& facts) : _facts(facts) {}

    bool build(const uint64_t authorityGeneration, const uint64_t nodeId,
               const uint64_t sequence, const uint8_t* qsrPayload,
               const size_t qsrPayloadBytes,
               std::vector<uint8_t>& out) const override {
        wipeVector(out);
        if (authorityGeneration != _facts.authorityGeneration
            || nodeId != _facts.childNodeId || sequence == 0u
            || qsrPayload == nullptr || qsrPayloadBytes == 0u
            || qsrPayloadBytes
                > qnetplay::spectator::resilient::wire::kMaxResilientControlDatagramBytes) {
            return false;
        }
        try {
            out.reserve(512u + qsrPayloadBytes);
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (!appendBytes(out, reinterpret_cast<const uint8_t*>(
                             kTopologyCredentialDomain),
                         sizeof(kTopologyCredentialDomain) - 1u)) {
            return false;
        }
        appendU64(out, _facts.sessionId);
        appendU64(out, _facts.sessionEpoch);
        appendU64(out, _facts.spectatorCookieLow);
        appendU64(out, _facts.spectatorCookieHigh);
        if (!appendCompatibility(out, _facts.compatibility)) return false;
        appendU64(out, _facts.streamRootIdentity);
        appendU64(out, _facts.authorityGeneration);
        appendU64(out, _facts.rootNodeId);
        appendEndpoint(_facts.rootQsp, out);
        appendEndpoint(_facts.rootQsr, out);
        appendU64(out, _facts.sourceGeneration);
        appendU64(out, _facts.childNodeId);
        appendU64(out, _facts.viewerId);
        appendU32(out, _facts.viewerGeneration);
        appendU32(out, _facts.qspIpv4BE);
        appendU16(out, _facts.qspPort);
        appendU8(out, _facts.qspProvenance);
        appendU64(out, _facts.qspRouteGeneration);
        appendU64(out, _facts.bindingGeneration);
        appendU32(out, _facts.confirmedCursor);
        appendU64(out, _facts.expiryMs);
        appendU64(out, sequence);
        appendU32(out, _facts.cursor.nextRecord);
        appendU32(out, _facts.cursor.nextEvent);
        appendU64(out, _facts.cursor.digest);
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
        return appendBytes(out, qsrPayload, qsrPayloadBytes);
    }

private:
    static void appendEndpoint(const SpectateTopologyEndpointBindingWire& value,
                               std::vector<uint8_t>& out) {
        appendU32(out, value.localIpv4BE);
        appendU16(out, value.localPort);
        appendU8(out, value.advertisedRouteKind);
        appendU32(out, value.advertisedIpv4BE);
        appendU16(out, value.advertisedPort);
        appendU64(out, value.advertisedRouteGeneration);
        appendU8(out, value.advertisedRouteProved);
        appendU8(out, value.localProvenance);
        appendU8(out, value.advertisedProvenance);
        appendU64(out, value.bindingGeneration);
    }

    const TranscriptFacts& _facts;
};

const char* releasedTopologyClientStateName(const ReleasedTopologyClientState state) {
    switch (state) {
    case ReleasedTopologyClientState::Disabled: return "disabled";
    case ReleasedTopologyClientState::WaitingForInitAck:
        return "waiting-for-init-ack";
    case ReleasedTopologyClientState::ControlConnecting:
        return "control-connecting";
    case ReleasedTopologyClientState::AwaitingHostVersion:
        return "awaiting-host-version";
    case ReleasedTopologyClientState::AwaitingControlChallenge:
        return "awaiting-control-challenge";
    case ReleasedTopologyClientState::AwaitingEnrollmentResponse:
        return "awaiting-enrollment-response";
    case ReleasedTopologyClientState::AwaitingRootOffer:
        return "awaiting-root-offer";
    case ReleasedTopologyClientState::AwaitingReceipt: return "awaiting-receipt";
    case ReleasedTopologyClientState::Attached: return "attached";
    case ReleasedTopologyClientState::Unavailable: return "unavailable";
    }
    return "unknown";
}

const char* releasedTopologyRelayBindingStateName(
    const ReleasedTopologyRelayBindingState state) {
    switch (state) {
    case ReleasedTopologyRelayBindingState::Inactive: return "inactive";
    case ReleasedTopologyRelayBindingState::Pending: return "pending";
    case ReleasedTopologyRelayBindingState::Ready: return "ready";
    case ReleasedTopologyRelayBindingState::Unavailable: return "unavailable";
    }
    return "unknown";
}

ReleasedTopologyClientRuntime::~ReleasedTopologyClientRuntime() {
    stop();
}

bool ReleasedTopologyClientRuntime::validateConfig(
    const ReleasedTopologyClientConfig& config) const {
    const bool direct = config.qspRoute == qnet::SPECTATE_ROUTE_DIRECT
        && config.qspProvenance == EndpointProvenance::ProvenDirectQsp
        && !config.relayQsrPermitted;
    const bool punched = config.qspRoute == qnet::SPECTATE_ROUTE_PUNCHED
        && config.qspProvenance == EndpointProvenance::ProvenPunchedQsp
        && !config.relayQsrPermitted;
    const bool relay = config.qspRoute == qnet::SPECTATE_ROUTE_RELAY
        && config.qspProvenance == EndpointProvenance::ProvenRelayQsp
        && config.relayQsrPermitted;
    const qnetplay::spectator::SpectatorGrant& grant = config.grant;
    return config.enabled && endpointValid(config.hostControl)
        && config.qspEndpoint.valid() && config.rootQspEndpoint.valid()
        && (direct || punched || relay) && config.qspRouteGeneration != 0u
        && config.viewerId != 0u && config.viewerGeneration != 0u
        && config.sessionId != 0u && config.sessionEpoch != 0u
        && (config.spectatorCookieLow != 0u || config.spectatorCookieHigh != 0u)
        && config.streamRootIdentity != 0u
        && qnetplay::compat::isValidCompatibilityIdentity(config.compatibility)
        && grant.grantId != 0u && grant.viewerId == config.viewerId
        && grant.sessionId == config.sessionId && grant.expiryUnix != 0u
        && grant.sourceGeneration != 0u && grant.route == config.qspRoute
        && (grant.macLow != 0u || grant.macHigh != 0u);
}

bool ReleasedTopologyClientRuntime::start(
    const ReleasedTopologyClientConfig& config, const uint64_t nowMs) {
    stop();
    if (!config.enabled) return true;
    if (!validateConfig(config)) {
        _state = ReleasedTopologyClientState::Unavailable;
        _reason = "invalid-topology-bootstrap";
        return false;
    }
    bool degraded = false;
    const uint64_t correlation = qnet::secureRandom64(degraded);
    std::array<uint8_t, kQtaMacBytes> proofKey{};
    const bool keyReady = correlation != 0u && !degraded
        && deriveTopologyControlV2ProofKey(config.grant.macLow,
                                            config.grant.macHigh, proofKey);
    if (!keyReady) {
        wipeBytes(proofKey.data(), proofKey.size());
        _state = ReleasedTopologyClientState::Unavailable;
        _reason = "topology-rng-or-proof-key-unavailable";
        return false;
    }
    ClientBootstrap bootstrap{};
    bootstrap.enabled = true;
    bootstrap.viewerId = config.viewerId;
    bootstrap.viewerGeneration = config.viewerGeneration;
    bootstrap.sessionId = config.sessionId;
    bootstrap.correlation = correlation;
    bootstrap.enrollmentProofKey = proofKey.data();
    bootstrap.enrollmentProofKeyBytes = proofKey.size();
    bootstrap.qta.magic = kTopologyControlMagic;
    bootstrap.qta.version = kTopologyControlVersion;
    bootstrap.qta.type = kTopologyControlType;
    bootstrap.qta.maxPayloadBytes =
        qnetplay::spectator::resilient::wire::kMaxResilientControlDatagramBytes;
    const bool started = _client.start(bootstrap, nowMs);
    wipeBytes(proofKey.data(), proofKey.size());
    if (!started) {
        _lastTranscriptFailure = _client.failure();
        _client.stop();
        _state = ReleasedTopologyClientState::Unavailable;
        _reason = "topology-transcript-bootstrap-rejected";
        return false;
    }
    _sessionId = config.sessionId;
    _sessionEpoch = config.sessionEpoch;
    _viewerId = config.viewerId;
    _viewerGeneration = config.viewerGeneration;
    _correlation = correlation;
    _spectatorCookieLow = config.spectatorCookieLow;
    _spectatorCookieHigh = config.spectatorCookieHigh;
    _streamRootIdentity = config.streamRootIdentity;
    _qspRouteGeneration = config.qspRouteGeneration;
    _qspRoute = config.qspRoute;
    _qspProvenance = config.qspProvenance;
    _qspEndpoint = config.qspEndpoint;
    _rootQspEndpoint = config.rootQspEndpoint;
    _hostControl = config.hostControl;
    _compatibility = config.compatibility;
    _grant = config.grant;
    _relayQsrPermitted = config.relayQsrPermitted;
    _relayQsrOffer = {};
    _state = ReleasedTopologyClientState::WaitingForInitAck;
    _reason = "waiting-for-init-ack";
    return true;
}

bool ReleasedTopologyClientRuntime::beginControl(const uint64_t nowMs) {
    wipeBytes(_peerChallenge, sizeof(_peerChallenge));
    _responseDeadlineMs = 0u;
    if (!_controlSocket.open(0u)) return false;
    _controlLink.startClient(_controlSocket, _hostControl, nowMs);
    if (_controlDeadlineMs == 0u)
        _controlDeadlineMs = addSaturating(nowMs, kTopologyControlSetupTimeoutMs);
    _state = ReleasedTopologyClientState::ControlConnecting;
    _reason = "control-connecting";
    return true;
}

bool ReleasedTopologyClientRuntime::restartControl(const uint64_t nowMs) {
    if (_controlDeadlineMs == 0u || nowMs >= _controlDeadlineMs
        || _controlRestartCount >= kTopologyControlMaxRestarts) {
        return false;
    }
    bool degraded = false;
    const uint64_t freshCorrelation = qnet::secureRandom64(degraded);
    if (degraded || freshCorrelation == 0u || freshCorrelation == _correlation
        || !_client.restartEnrollment(freshCorrelation, nowMs)) {
        return false;
    }
    _correlation = freshCorrelation;
    _controlLink.close();
    _controlSocket.close();
    ++_controlRestartCount;
    return beginControl(nowMs);
}

bool ReleasedTopologyClientRuntime::onQspInitAckTransmitted(
    const uint64_t nowMs) {
    if (_state != ReleasedTopologyClientState::WaitingForInitAck
        || !_client.onQspInitAckTransmitted(nowMs)) {
        _lastTranscriptFailure = _client.failure();
        failSoft("init-ack-transition-rejected");
        return false;
    }
    if (!beginControl(nowMs)) {
        failSoft("topology-control-socket-unavailable");
        return false;
    }
    return true;
}

bool ReleasedTopologyClientRuntime::updateVerifiedQspFrontier(
    const uint32_t nextRecord, const uint32_t nextEvent, const uint64_t digest) {
    if (_state == ReleasedTopologyClientState::Disabled
        || _state == ReleasedTopologyClientState::Unavailable) {
        return false;
    }
    if (_qspFrontierObserved
        && (nextRecord < _qspFrontier.nextRecord
            || nextEvent < _qspFrontier.nextEvent
            || (nextRecord == _qspFrontier.nextRecord
                && nextEvent == _qspFrontier.nextEvent
                && digest != _qspFrontier.digest))) {
        return false;
    }
    _qspFrontier.nextRecord = nextRecord;
    _qspFrontier.nextEvent = nextEvent;
    _qspFrontier.digest = digest;
    _qspFrontierObserved = true;
    return true;
}

void ReleasedTopologyClientRuntime::serviceControl(const uint64_t nowMs) {
    if (_state != ReleasedTopologyClientState::ControlConnecting
        && _state != ReleasedTopologyClientState::AwaitingHostVersion
        && _state != ReleasedTopologyClientState::AwaitingControlChallenge
        && _state != ReleasedTopologyClientState::AwaitingEnrollmentResponse) {
        return;
    }
    if (_state == ReleasedTopologyClientState::AwaitingEnrollmentResponse
        && _responseDeadlineMs != 0u && nowMs >= _responseDeadlineMs) {
        if (_client.state() == ClientState::AwaitingEnrollment
            && restartControl(nowMs)) {
            return;
        }
        failSoft("topology-control-response-timed-out");
        return;
    }
    if (_controlDeadlineMs != 0u && nowMs >= _controlDeadlineMs) {
        failSoft("topology-control-setup-timed-out");
        return;
    }
    _controlLink.poll(0);
    if (!_controlLink.isOpen()) {
        const ClientState transcript = _client.state();
        if ((transcript == ClientState::EnrollmentPending
             || transcript == ClientState::AwaitingEnrollment)
            && restartControl(nowMs)) {
            return;
        }
        failSoft("topology-control-link-closed");
        return;
    }
    if (_state == ReleasedTopologyClientState::ControlConnecting
        && _controlLink.established()) {
        _state = ReleasedTopologyClientState::AwaitingHostVersion;
        _reason = "awaiting-host-version";
    }
    for (size_t budget = 0u; budget < 8u; ++budget) {
        qnet::ControlLink::Received rx{};
        const qnet::ControlLink::Rs result = _controlLink.recvMsgEx(rx, 0);
        if (result == qnet::ControlLink::Rs::Timeout) break;
        if (result == qnet::ControlLink::Rs::Closed) {
            const ClientState transcript = _client.state();
            if ((transcript == ClientState::EnrollmentPending
                 || transcript == ClientState::AwaitingEnrollment)
                && restartControl(nowMs)) {
                return;
            }
            failSoft("topology-control-link-closed");
            return;
        }
        if (result != qnet::ControlLink::Rs::Msg) {
            failSoft("topology-control-receive-failed");
            return;
        }
        if (rx.type == qnet::MSG_PING) {
            (void)_controlLink.sendMsg(qnet::MSG_PONG, rx.body.data(),
                                       rx.body.size());
            continue;
        }
        if (rx.type == qnet::MSG_ERROR_TEXT) {
            failSoft("topology-control-host-rejected");
            return;
        }
        if (_state == ReleasedTopologyClientState::AwaitingHostVersion) {
            qnet::VersionConfig version{};
            if (rx.type != qnet::MSG_VERSION_CONFIG
                || rx.body.size() != sizeof(version)) {
                failSoft("topology-control-version-malformed");
                return;
            }
            std::memcpy(&version, rx.body.data(), sizeof(version));
            if (version.protocolVersion != qnet::kProtocolVersion) {
                failSoft("topology-control-version-mismatch");
                return;
            }
            qnet::VersionConfig local{qnet::kProtocolVersion};
            qnet::ClientIntent intent{};
            intent.intent = qnet::INTENT_SPECTATE;
            intent.spectatorRoute = _qspRoute;
            intent.spectatorCapabilities =
                qnet::kSpectateCapabilityReleasedTopologyV2Mask;
            intent.sessionId = static_cast<uint32_t>(_sessionId);
            intent.keyLow = _spectatorCookieLow;
            intent.keyHigh = _spectatorCookieHigh;
            intent.viewerId = _viewerId;
            intent.resumeRecord = _viewerGeneration;
            if (_sessionId > (std::numeric_limits<uint32_t>::max)()
                || !_controlLink.sendMsg(qnet::MSG_VERSION_CONFIG, &local,
                                         sizeof(local))
                || !_controlLink.sendMsg(qnet::MSG_CLIENT_INTENT, &intent,
                                         sizeof(intent))) {
                failSoft("topology-control-send-failed");
                return;
            }
            _state = ReleasedTopologyClientState::AwaitingControlChallenge;
            _reason = "awaiting-control-challenge";
            continue;
        }
        if (_state == ReleasedTopologyClientState::AwaitingControlChallenge) {
            SpectateTopologyChallengeV2 challenge{};
            if (rx.type != qnet::MSG_SPECTATE_TOPOLOGY_CHALLENGE_V2
                || rx.body.size() != sizeof(challenge)) {
                failSoft("topology-control-challenge-malformed");
                return;
            }
            std::memcpy(&challenge, rx.body.data(), sizeof(challenge));
            const bool valid = challenge.schemaVersion
                    == kSpectateTopologyControlSchemaVersion
                && challenge.requiredFeatures
                    == kSpectateTopologyControlRequiredFeatures
                && challenge.sessionId == _sessionId
                && challenge.viewerId == _viewerId
                && challenge.viewerGeneration == _viewerGeneration
                && challenge.reserved == 0u
                && !allZero(challenge.peerChallenge,
                            sizeof(challenge.peerChallenge));
            if (!valid) {
                wipeBytes(&challenge, sizeof(challenge));
                failSoft("topology-control-challenge-rejected");
                return;
            }
            std::memcpy(_peerChallenge, challenge.peerChallenge,
                        sizeof(_peerChallenge));
            wipeBytes(&challenge, sizeof(challenge));
            _state = ReleasedTopologyClientState::AwaitingEnrollmentResponse;
            _reason = "awaiting-enrollment-response";
            continue;
        }
        if (_state == ReleasedTopologyClientState::AwaitingEnrollmentResponse) {
            SpectateTopologyInfoV2 info{};
            if (rx.type != qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2
                || rx.body.size() != sizeof(info)) {
                failSoft("topology-control-response-malformed");
                return;
            }
            std::memcpy(&info, rx.body.data(), sizeof(info));
            const bool accepted = acceptEnrollmentInfo(info, nowMs);
            wipeBytes(&info, sizeof(info));
            if (!accepted) return;
            _responseDeadlineMs = 0u;
            _controlLink.close();
            _controlSocket.close();
            return;
        }
        failSoft("topology-control-state-rejected");
        return;
    }
}

bool ReleasedTopologyClientRuntime::sendEnrollmentProof(
    const ClientAction& action, const uint64_t nowMs) {
    if (action.correlation != _correlation
        || allZero(_peerChallenge, sizeof(_peerChallenge))) {
        (void)_client.completeAction(action.actionId, false, nowMs);
        return false;
    }
    SpectateTopologyProofV2 proof{};
    proof.schemaVersion = kSpectateTopologyControlSchemaVersion;
    proof.requiredFeatures = kSpectateTopologyControlRequiredFeatures;
    proof.requestNonce = action.correlation;
    std::memcpy(proof.peerChallenge, _peerChallenge,
                sizeof(proof.peerChallenge));
    proof.viewerId = _viewerId;
    proof.viewerGeneration = _viewerGeneration;
    proof.qspRoute = _qspRoute;
    proof.qspRouteGeneration = _qspRouteGeneration;
    proof.qspIpv4BE = _qspEndpoint.ipv4BE;
    proof.qspPort = _qspEndpoint.port;
    proof.sessionEpoch = _sessionEpoch;
    proof.discoveryGeneration = _grant.sourceGeneration;
    proof.streamRootIdentity = _streamRootIdentity;
    proof.compatibilityIdentity = _compatibility;
    proof.grant = grantToWire(_grant);
    std::vector<uint8_t> canonical;
    std::array<uint8_t, kQtaMacBytes> tag{};
    const bool tagged = copyProofBodyWithoutTag(proof, canonical)
        && _client.makeEnrollmentRequestTag(action.actionId, canonical.data(),
                                             canonical.size(), tag);
    wipeVector(canonical);
    if (!tagged) {
        wipeBytes(tag.data(), tag.size());
        (void)_client.completeAction(action.actionId, false, nowMs);
        return false;
    }
    std::copy_n(tag.data(), tag.size(), proof.requestTag);
    wipeBytes(tag.data(), tag.size());
    const bool sent = _controlLink.sendMsg(qnet::MSG_SPECTATE_TOPOLOGY_PROOF_V2,
                                           &proof, sizeof(proof));
    wipeBytes(&proof, sizeof(proof));
    const bool completed = _client.completeAction(action.actionId, sent, nowMs);
    if (sent && completed && _responseDeadlineMs == 0u) {
        _responseDeadlineMs = addSaturating(
            nowMs, kTopologyControlResponseTimeoutMs);
    }
    return completed;
}

bool ReleasedTopologyClientRuntime::acceptEnrollmentInfo(
    const SpectateTopologyInfoV2& info, const uint64_t nowMs) {
    std::vector<uint8_t> canonical;
    std::array<uint8_t, kQtaMacBytes> tag{};
    std::copy_n(info.responseTag, tag.size(), tag.data());
    const bool authenticationValid = copyInfoBodyWithoutTag(info, canonical)
        && verifyTopologyControlV2Tag(_grant.macLow, _grant.macHigh,
                                      canonical.data(), canonical.size(), tag);
    wipeVector(canonical);
    wipeBytes(tag.data(), tag.size());
    if (!authenticationValid) {
        failSoft("topology-response-authentication-rejected");
        return false;
    }
    if (info.requestNonce != _correlation) {
        return false;
    }
    EnrollmentResponse response{};
    response.granted = info.state == qnet::SPECTATE_TOPOLOGY_GRANTED;
    response.correlation = info.requestNonce;
    response.viewerId = info.viewerId;
    response.viewerGeneration = info.viewerGeneration;
    response.sessionId = info.sessionId;
    response.authorityGeneration = info.authorityGeneration;
    response.nodeId = info.childNodeId;
    response.expiresAtMs = info.expiryMs;
    response.qspRouteGeneration = info.qspRouteGeneration;
    response.bindingGeneration = info.bindingGeneration;
    std::copy_n(info.topologyCredential, response.qtaCredential.size(),
                response.qtaCredential.data());
    std::copy_n(info.responseTag, response.authenticationTag.size(),
                response.authenticationTag.data());
    std::vector<uint8_t> moduleCanonical;
    const bool canonicalReady = copyInfoBodyWithoutTag(info, moduleCanonical);
    if (!canonicalReady) {
        wipeBytes(&response, sizeof(response));
        failSoft("topology-response-canonicalization-failed");
        return false;
    }
    const bool unavailable = !response.granted;
    const bool coreAccepted = _client.acceptEnrollmentResponse(
        response, moduleCanonical.data(), moduleCanonical.size(), nowMs);
    wipeVector(moduleCanonical);
    wipeBytes(&response, sizeof(response));
    if (!coreAccepted) {
        _lastTranscriptFailure = _client.failure();
        failSoft(unavailable ? "topology-unavailable"
                             : "topology-response-transcript-rejected");
        return false;
    }

    const bool recognizedSchema =
        info.schemaVersion == kSpectateTopologyControlSchemaVersion
        && info.requiredFeatures == kSpectateTopologyControlRequiredFeatures;
    const bool exactViewer = info.requestNonce == _correlation
        && info.viewerId == _viewerId
        && info.viewerGeneration == _viewerGeneration;
    const bool exactIdentity = info.sessionId == _sessionId
        && info.sessionEpoch == _sessionEpoch
        && info.streamRootIdentity == _streamRootIdentity
        && qnetplay::compat::semanticallyEqual(info.compatibilityIdentity,
                                                _compatibility);
    const bool relayClient = _qspRoute == qnet::SPECTATE_ROUTE_RELAY;
    const bool exactQspEndpoint = relayClient
        ? (info.qspIpv4BE == qnetplay::spectator::kRelayVirtualIpv4BE
           && info.qspPort != 0u)
        : (info.qspIpv4BE == _qspEndpoint.ipv4BE
           && info.qspPort == _qspEndpoint.port);
    const bool exactQsp = exactQspEndpoint
        && info.qspRouteGeneration == _qspRouteGeneration
        && info.qspRoute == _qspRoute
        && info.qspProvenance == static_cast<uint8_t>(_qspProvenance);
    const bool validRootBindings = endpointBindingWireValid(info.rootQsp)
        && endpointBindingWireValid(info.rootQsr)
        && info.rootQspRouteKind == info.rootQsp.advertisedRouteKind
        && info.rootQspRouteProved == info.rootQsp.advertisedRouteProved
        && info.rootQsrRouteKind == info.rootQsr.advertisedRouteKind
        && info.rootQsrRouteProved == info.rootQsr.advertisedRouteProved
        && (relayClient
            || (info.rootQsp.advertisedIpv4BE == _rootQspEndpoint.ipv4BE
                && info.rootQsp.advertisedPort == _rootQspEndpoint.port))
        && info.rootQsp.advertisedRouteProved == 1u
        && info.rootQsp.advertisedRouteGeneration
            == info.rootQsp.bindingGeneration
        && info.rootQsr.advertisedRouteKind
            == static_cast<uint8_t>(model::RouteKind::Direct)
        && info.rootQsr.advertisedRouteProved == 0u
        && info.rootQsr.advertisedProvenance
            == static_cast<uint8_t>(EndpointProvenance::PublishedDirectCandidate)
        && info.rootQsp.advertisedPort != info.rootQsr.advertisedPort
        && info.rootQsp.localPort != info.rootQsr.localPort;
    const bool validAuthority = info.authorityGeneration != 0u
        && info.sourceGeneration == _grant.sourceGeneration
        && info.rootNodeId != model::kInvalidNodeId
        && info.childNodeId != model::kInvalidNodeId
        && info.rootNodeId != info.childNodeId && info.expiryMs != 0u
        && info.bindingGeneration != 0u
        && info.qtaLimits.maxTopologyNodes != 0u
        && info.qtaLimits.maxActiveDepth != 0u
        && info.qtaLimits.maxChildrenPerRoot != 0u
        && info.qtaLimits.maxChildrenPerRelay != 0u
        && info.qtaLimits.maxStandbyParents != 0u
        && info.qtaLimits.standbyLeaseMs != 0u
        && !allZero(info.topologyCredential, sizeof(info.topologyCredential))
        && (unavailable || !allZero(
            info.authorityAssignmentPublicKey,
            sizeof(info.authorityAssignmentPublicKey)));
    qnetplay::spectator::RelayQsrOffer relayOffer{};
    const bool relayBlockValid =
        qnetplay::spectator::relayQsrOfferFromInfo(info, relayOffer)
        && (!relayOffer.present || _relayQsrPermitted);
    if (!recognizedSchema || info.reservedState != 0u || info.reservedQsp != 0u
        || !exactViewer || !exactIdentity || !exactQsp || !validRootBindings
        || !validAuthority || !relayBlockValid) {
        failSoft("topology-response-binding-rejected");
        return false;
    }
    _relayQsrOffer = relayOffer;
    _assignmentVerifier.reset();
    if (!unavailable && !_assignmentVerifier.initialize(
            info.authorityAssignmentPublicKey,
            sizeof(info.authorityAssignmentPublicKey))) {
        failSoft("topology-response-host-attestation-rejected");
        return false;
    }
    try {
        _facts = std::make_unique<TranscriptFacts>();
        _facts->sessionId = info.sessionId;
        _facts->sessionEpoch = info.sessionEpoch;
        _facts->spectatorCookieLow = _spectatorCookieLow;
        _facts->spectatorCookieHigh = _spectatorCookieHigh;
        _facts->compatibility = info.compatibilityIdentity;
        _facts->streamRootIdentity = info.streamRootIdentity;
        _facts->authorityGeneration = info.authorityGeneration;
        _facts->rootNodeId = info.rootNodeId;
        _facts->rootQsp = info.rootQsp;
        _facts->rootQsr = info.rootQsr;
        _facts->sourceGeneration = info.sourceGeneration;
        _facts->childNodeId = info.childNodeId;
        _facts->viewerId = info.viewerId;
        _facts->viewerGeneration = info.viewerGeneration;
        _facts->qspIpv4BE = info.qspIpv4BE;
        _facts->qspPort = info.qspPort;
        _facts->qspProvenance = info.qspProvenance;
        _facts->qspRouteGeneration = info.qspRouteGeneration;
        _facts->bindingGeneration = info.bindingGeneration;
        _facts->confirmedCursor = info.confirmedCursor;
        _facts->expiryMs = info.expiryMs;
        _facts->cursor = info.qtaCursor;
        _facts->limits = info.qtaLimits;
        std::copy_n(info.authorityAssignmentPublicKey,
                    _facts->assignmentSigningPublicKey.size(),
                    _facts->assignmentSigningPublicKey.data());
    } catch (const std::bad_alloc&) {
        failSoft("topology-response-resource-exhausted");
        return false;
    }
    wipeBytes(&_grant, sizeof(_grant));
    if (!setupQsrBootstrap(nowMs)) {
        failSoft(_qspRoute == qnet::SPECTATE_ROUTE_PUNCHED
                     ? "punched-qsr-route-not-runtime-proven"
                     : "topology-qsr-bootstrap-unavailable");
        return false;
    }
    _state = ReleasedTopologyClientState::AwaitingRootOffer;
    _reason = "awaiting-root-offer";
    _controlDeadlineMs = 0u;
    _credentialDeadlineMs = addSaturating(nowMs, info.qtaLimits.standbyLeaseMs);
    return true;
}

bool ReleasedTopologyClientRuntime::setupQsrBootstrap(const uint64_t nowMs) {
    if (_facts != nullptr && _relayQsrPermitted) {
        return _relayQsrOffer.present && setupRelayQsrBootstrap(nowMs);
    }
    if (_facts == nullptr || _qspRoute != qnet::SPECTATE_ROUTE_DIRECT
        || _qspProvenance != EndpointProvenance::ProvenDirectQsp
        || !_qsrSocket.open(0u) || _qsrSocket.boundPort() == 0u
        || _qsrSocket.boundPort() == _qspEndpoint.port) {
        return false;
    }
    model::StreamCompatibilityIdentity stream{};
    if (!model::makeStreamCompatibilityIdentity(
            _facts->compatibility, _facts->sessionId, _facts->sessionEpoch,
            _facts->streamRootIdentity, stream)) {
        return false;
    }
    model::NodeAdvertisement advertisement{};
    advertisement.nodeId = _facts->childNodeId;
    advertisement.kind = model::NodeKind::SpectatorRelay;
    advertisement.stream = stream;
    advertisement.route.kind = _qspProvenance
            == EndpointProvenance::ProvenPunchedQsp
        ? model::RouteKind::Punched : model::RouteKind::Direct;
    advertisement.route.ipv4BE = _qspEndpoint.ipv4BE;
    advertisement.route.port = _qspEndpoint.port;
    advertisement.route.generation = _facts->qspRouteGeneration;
    advertisement.route.routeProved = true;
    advertisement.controlRoute.kind = model::RouteKind::Direct;
    advertisement.controlRoute.ipv4BE = _qspEndpoint.ipv4BE;
    advertisement.controlRoute.port = _qsrSocket.boundPort();
    advertisement.controlRoute.generation = _facts->qspRouteGeneration;
    advertisement.controlRoute.routeProved = true;
    advertisement.activeParent = model::kInvalidNodeId;
    advertisement.depth = 0u;
    advertisement.capabilityModelRevision = model::kResilientCapabilityModelRevision;
    advertisement.capabilities = model::kCapabilityRelayCapableSpectator
        | model::kCapabilityTopologyLeaf;
    advertisement.childCapacity = 0u;
    advertisement.childCount = 0u;
    advertisement.uploadCapacity = 1u;
    advertisement.uploadInUse = 0u;
    advertisement.cacheFirstCursor = _facts->cursor.nextRecord;
    advertisement.cacheNextCursor = _facts->cursor.nextRecord;
    advertisement.sourceGeneration = _facts->sourceGeneration;
    advertisement.routeGeneration = _facts->qspRouteGeneration;
    advertisement.lastProgressMs = nowMs;
    advertisement.leaseExpiryMs = _facts->expiryMs;
    advertisement.reachable = true;
    advertisement.draining = false;
    qnetplay::spectator::resilient::wire::ControlDatagram datagram{};
    datagram.type = qnetplay::spectator::resilient::wire::ControlMessageType::NodeAdvertisement;
    datagram.nodeAdvertisement = advertisement;
    std::vector<uint8_t> payload;
    const bool encoded = qnetplay::spectator::resilient::wire::encodeControlDatagram(
        datagram, payload);
    const bool queued = encoded && _client.setBootstrapQsrPayload(
        payload.data(), payload.size(), nowMs);
    wipeVector(payload);
    return queued;
}

bool ReleasedTopologyClientRuntime::setupRelayQsrBootstrap(
    const uint64_t nowMs) {
    if (_facts == nullptr || !_relayQsrOffer.present) return false;
    if (_relayQsrOffer.relayIpv4BE == 0u || _relayQsrOffer.relayPort == 0u
        || _relayQsrOffer.grant.nodeId != _facts->childNodeId
        || _relayQsrOffer.grant.sessionId != _sessionId) {
        return false;
    }
    if (!_qsrSocket.open(0u) || _qsrSocket.boundPort() == 0u
        || _qsrSocket.boundPort() == _qspEndpoint.port) {
        return false;
    }
    _relayQsrEndpoint = {};
    _relayQsrEndpoint.sa.sin_family = AF_INET;
    _relayQsrEndpoint.sa.sin_addr.s_addr = _relayQsrOffer.relayIpv4BE;
    _relayQsrEndpoint.sa.sin_port = htons(_relayQsrOffer.relayPort);
    _relayQsrActive = true;
    _nextRelayQsrHelloMs = nowMs;

    model::StreamCompatibilityIdentity stream{};
    if (!model::makeStreamCompatibilityIdentity(
            _facts->compatibility, _facts->sessionId, _facts->sessionEpoch,
            _facts->streamRootIdentity, stream)) {
        return false;
    }
    model::NodeAdvertisement advertisement{};
    advertisement.nodeId = _facts->childNodeId;
    advertisement.kind = model::NodeKind::SpectatorRelay;
    advertisement.stream = stream;
    advertisement.route.kind = model::RouteKind::TrafficRelay;
    advertisement.route.ipv4BE = _facts->qspIpv4BE;
    advertisement.route.port = _facts->qspPort;
    advertisement.route.generation = _facts->qspRouteGeneration;
    advertisement.route.routeProved = true;
    advertisement.controlRoute.kind = model::RouteKind::TrafficRelay;
    advertisement.controlRoute.ipv4BE = _relayQsrOffer.relayIpv4BE;
    advertisement.controlRoute.port = _relayQsrOffer.relayPort;
    advertisement.controlRoute.generation = _facts->qspRouteGeneration;
    advertisement.controlRoute.routeProved = false;
    advertisement.activeParent = model::kInvalidNodeId;
    advertisement.depth = 0u;
    advertisement.capabilityModelRevision =
        model::kResilientCapabilityModelRevision;
    advertisement.capabilities = model::kCapabilityRelayCapableSpectator
        | model::kCapabilityTopologyLeaf;
    advertisement.childCapacity = 0u;
    advertisement.childCount = 0u;
    advertisement.uploadCapacity = 1u;
    advertisement.uploadInUse = 0u;
    advertisement.cacheFirstCursor = _facts->cursor.nextRecord;
    advertisement.cacheNextCursor = _facts->cursor.nextRecord;
    advertisement.sourceGeneration = _facts->sourceGeneration;
    advertisement.routeGeneration = _facts->qspRouteGeneration;
    advertisement.lastProgressMs = nowMs;
    advertisement.leaseExpiryMs = _facts->expiryMs;
    advertisement.reachable = true;
    advertisement.draining = false;
    qnetplay::spectator::resilient::wire::ControlDatagram datagram{};
    datagram.type =
        qnetplay::spectator::resilient::wire::ControlMessageType::
            NodeAdvertisement;
    datagram.nodeAdvertisement = advertisement;
    std::vector<uint8_t> payload;
    const bool encoded =
        qnetplay::spectator::resilient::wire::encodeControlDatagram(
            datagram, payload);
    const bool queued = encoded && _client.setBootstrapQsrPayload(
        payload.data(), payload.size(), nowMs);
    wipeVector(payload);
    return queued;
}

void ReleasedTopologyClientRuntime::serviceRelayQsrAdmission(
    const uint64_t nowMs) {
    if (!_relayQsrActive || !_qsrSocket.isOpen()) return;
    std::vector<uint8_t> record;
    if (!_relayQsrAdapter.enabled()) {
        if (nowMs < _nextRelayQsrHelloMs) return;
        _nextRelayQsrHelloMs =
            nowMs + qnetplay::spectator::kTopologyRelayRegistrationRetryMs;
        qnet::relay::TopologyHello hello{};
        hello.room = _relayQsrOffer.room;
        hello.party = qnet::relay::TopologyParty::Node;
        hello.admission = qnet::relay::TopologyAdmission::Grant;
        hello.sessionId = _relayQsrOffer.grant.sessionId;
        hello.grantId = _relayQsrOffer.grant.grantId;
        hello.nodeId = _relayQsrOffer.grant.nodeId;
        hello.expiryUnix = _relayQsrOffer.grant.expiryUnix;
        hello.authorityGeneration =
            _relayQsrOffer.grant.authorityGeneration;
        hello.macLow = _relayQsrOffer.grant.macLow;
        hello.macHigh = _relayQsrOffer.grant.macHigh;
        if (qnet::relay::encodeTopologyHello(hello, record)) {
            (void)_qsrSocket.sendTo(_relayQsrEndpoint, record.data(),
                                    record.size());
        }
        wipeVector(record);
        return;
    }
    if (_relayQsrAdapter.encodeRegistration(nowMs, record)) {
        (void)_qsrSocket.sendTo(_relayQsrEndpoint, record.data(),
                                record.size());
    }
    wipeVector(record);
}

bool ReleasedTopologyClientRuntime::acceptRelayQsrWelcome(
    const uint8_t* const bytes, const size_t length, const uint64_t nowMs) {
    if (!_relayQsrActive || _relayQsrAdapter.enabled()) return false;
    qnet::relay::TopologyWelcome welcome{};
    if (qnet::relay::decodeTopologyWelcome(bytes, length, welcome)
        != qnet::relay::DecodeError::None) {
        return false;
    }
    if (welcome.party != qnet::relay::TopologyParty::Node
        || welcome.nodeId != _relayQsrOffer.grant.nodeId
        || welcome.sessionId != _relayQsrOffer.grant.sessionId
        || welcome.authorityGeneration
            != _relayQsrOffer.grant.authorityGeneration
        || !(welcome.room == _relayQsrOffer.room)) {
        return false;
    }
    if (welcome.status != qnet::relay::TopologyStatus::Accepted
        && welcome.status != qnet::relay::TopologyStatus::Waiting) {
        failSoft("topology-relay-qsr-admission-refused");
        return true;
    }
    qnetplay::spectator::TopologyRelayBinding binding{};
    if (!qnetplay::spectator::relayBindingFromOffer(
            _relayQsrOffer, welcome.registrationToken, binding)
        || !_relayQsrAdapter.configure(binding)) {
        failSoft("topology-relay-qsr-binding-rejected");
        return true;
    }
    std::vector<uint8_t> record;
    if (_relayQsrAdapter.encodeRegistration(nowMs, record, true)) {
        (void)_qsrSocket.sendTo(_relayQsrEndpoint, record.data(),
                                record.size());
    }
    wipeVector(record);
    return true;
}

bool ReleasedTopologyClientRuntime::sendQsrDatagram(
    const std::vector<uint8_t>& envelope) {
    if (envelope.empty() || !_qsrSocket.isOpen() || _facts == nullptr)
        return false;
    if (_relayQsrActive) {
        if (!_relayQsrAdapter.enabled()) return false;
        std::vector<uint8_t> framed;
        if (!_relayQsrAdapter.encodeOutbound(envelope.data(), envelope.size(),
                                             framed)) {
            return false;
        }
        const bool sent = _qsrSocket.sendTo(_relayQsrEndpoint, framed.data(),
                                            framed.size());
        wipeVector(framed);
        return sent;
    }
    qnet::Endpoint destination{};
    destination.sa.sin_addr.s_addr = _facts->rootQsr.advertisedIpv4BE;
    destination.sa.sin_port = htons(_facts->rootQsr.advertisedPort);
    return _qsrSocket.sendTo(destination, envelope.data(), envelope.size());
}

bool ReleasedTopologyClientRuntime::sendQsrAction(const ClientAction& action,
                                                  const uint64_t nowMs) {
    if (_facts == nullptr || !_qsrSocket.isOpen()) return false;
    TranscriptBuilder transcript(*_facts);
    std::vector<uint8_t> envelope;
    const bool encoded = _client.makeQtaEnvelope(action, transcript, envelope);
    const bool sent = encoded && sendQsrDatagram(envelope);
    wipeVector(envelope);
    return _client.completeAction(action.actionId, sent, nowMs);
}

void ReleasedTopologyClientRuntime::serviceActions(const uint64_t nowMs) {
    if (_state != ReleasedTopologyClientState::AwaitingEnrollmentResponse
        && _state != ReleasedTopologyClientState::AwaitingRootOffer
        && _state != ReleasedTopologyClientState::AwaitingReceipt
        && _state != ReleasedTopologyClientState::Attached) {
        return;
    }
    ClientAction action{};
    if (!_client.poll(nowMs, action)) {
        if (_client.state() == ClientState::Failed) {
            _lastTranscriptFailure = _client.failure();
            failSoft(clientFailureName(_lastTranscriptFailure));
        }
        return;
    }
    bool completed = false;
    if (action.kind == ClientActionKind::SendEnrollmentRequest) {
        completed = sendEnrollmentProof(action, nowMs);
    } else if (action.kind == ClientActionKind::SendBootstrapQsr
               || action.kind == ClientActionKind::SendRedemptionQsr
               || action.kind == ClientActionKind::SendActivationReceiptQsr
               || action.kind
                   == ClientActionKind::SendAttachedRelayActivationReceiptQsr) {
        completed = sendQsrAction(action, nowMs);
    }
    if (!completed && _client.state() == ClientState::Failed) {
        _lastTranscriptFailure = _client.failure();
        failSoft(clientFailureName(_lastTranscriptFailure));
    }
}

bool ReleasedTopologyClientRuntime::acceptRootOffer(const model::ParentLease& lease,
                                                     const uint64_t nowMs) {
    if (_pendingClientPlayerRootActivation.has_value()) return true;
    model::ParentLeaseChildBinding expectedChildBinding{};
    if (_facts != nullptr) {
        expectedChildBinding.authorityGeneration = _facts->authorityGeneration;
        expectedChildBinding.childViewerId = _facts->viewerId;
        expectedChildBinding.childViewerGeneration = _facts->viewerGeneration;
        const bool qspDirect = _facts->qspProvenance
            == static_cast<uint8_t>(EndpointProvenance::ProvenDirectQsp);
        const bool qspPunched = _facts->qspProvenance
            == static_cast<uint8_t>(EndpointProvenance::ProvenPunchedQsp);
        const bool qspRelay = _facts->qspProvenance
            == static_cast<uint8_t>(EndpointProvenance::ProvenRelayQsp);
        if (qspDirect || qspPunched || qspRelay) {
            expectedChildBinding.childQspRoute.kind = qspRelay
                ? model::RouteKind::TrafficRelay
                : (qspPunched ? model::RouteKind::Punched
                              : model::RouteKind::Direct);
            expectedChildBinding.childQspRoute.ipv4BE = _facts->qspIpv4BE;
            expectedChildBinding.childQspRoute.port = _facts->qspPort;
            expectedChildBinding.childQspRoute.generation =
                _facts->qspRouteGeneration;
            expectedChildBinding.childQspRoute.routeProved = true;
        }
        expectedChildBinding.childEndpointBindingGeneration =
            _facts->bindingGeneration;
        expectedChildBinding.childRequestedCursor = {
            _facts->cursor.nextRecord, _facts->cursor.nextEvent,
            _facts->cursor.digest};
    }
    if (_facts == nullptr || _offeredLease != nullptr
        || !streamEquals(lease.stream, _facts->compatibility, _facts->sessionId,
                         _facts->sessionEpoch, _facts->streamRootIdentity)
        || lease.childId != _facts->childNodeId
        || !expectedChildBinding.valid()
        || lease.childBinding != expectedChildBinding
        || lease.parentId != _facts->rootNodeId || lease.parentDepth != 0u
        || lease.parentSourceGeneration != _facts->sourceGeneration
        || lease.routeGeneration != _facts->rootQsp.bindingGeneration
        || lease.routeCount != 1u || !routeEqualsWire(lease.routes[0],
                                                       _facts->rootQsp)
        || lease.cacheRange.firstCursor > lease.cacheRange.nextCursor
        || !lease.cacheRange.containsBoundary(
            expectedChildBinding.childRequestedCursor.nextRecord)
        || lease.latestVerifiedCursor
            < expectedChildBinding.childRequestedCursor.nextRecord
        || !_qspFrontierObserved
        || _qspFrontier.nextRecord < lease.cacheRange.firstCursor
        || _qspFrontier.nextRecord > lease.cacheRange.nextCursor
        || lease.latestVerifiedCursor != lease.cacheRange.nextCursor
        || lease.latestVerifiedCursor < _facts->cursor.nextRecord
        || lease.childCapacity == 0u || lease.expiryMs == 0u
        || lease.replayNonce == 0u || lease.ancestryCount != 1u
        || lease.ancestry[0] != _facts->rootNodeId) {
        return false;
    }
    if (!_client.acceptRootOffer(lease.replayNonce, nowMs)) return false;
    clearPendingRelayChildActivation();
    clearPendingClientPlayerRootActivation();
    qnetplay::spectator::resilient::wire::ControlDatagram redemption{};
    redemption.type = qnetplay::spectator::resilient::wire::ControlMessageType::ParentLease;
    redemption.parentLease = lease;
    std::vector<uint8_t> payload;
    const bool encoded = qnetplay::spectator::resilient::wire::encodeControlDatagram(
        redemption, payload);
    const bool queued = encoded && _client.setRedemptionQsrPayload(
        payload.data(), payload.size(), lease.replayNonce, nowMs);
    wipeVector(payload);
    if (!queued) return false;
    try {
        _offeredLease = std::make_unique<model::ParentLease>(lease);
    } catch (const std::bad_alloc&) {
        return false;
    }
    _state = ReleasedTopologyClientState::AwaitingReceipt;
    _reason = "awaiting-receipt";
    _offerDeadlineMs = _credentialDeadlineMs;
    return true;
}

bool ReleasedTopologyClientRuntime::acceptParentCommit(
    const qnetplay::spectator::resilient::wire::ParentCommit& commit,
    const uint64_t nowMs) {
    if (_facts == nullptr || _offeredLease == nullptr
        || !streamEquals(commit.stream, _facts->compatibility, _facts->sessionId,
                         _facts->sessionEpoch, _facts->streamRootIdentity)
        || commit.childId != _offeredLease->childId
        || commit.parentId != _offeredLease->parentId
        || !sameLeaseBinding(commit.lease, *_offeredLease)
        || commit.sourceGeneration != _offeredLease->parentSourceGeneration
        || commit.routeGeneration != _offeredLease->routeGeneration
        || _offerDeadlineMs == 0u || nowMs >= _offerDeadlineMs
        || !_qspFrontierObserved
        || commit.exactNextCursor != _qspFrontier.nextRecord
        || commit.exactNextEvent != _qspFrontier.nextEvent
        || commit.overlapCursor != commit.exactNextCursor
        || commit.overlapEvent != commit.exactNextEvent
        || commit.overlapDigest != _qspFrontier.digest) {
        return false;
    }

    ReleasedTopologyAttachmentSnapshot snapshot{};
    snapshot.stream = _offeredLease->stream;
    snapshot.authorityGeneration = _facts->authorityGeneration;
    snapshot.rootNodeId = _offeredLease->parentId;
    snapshot.childNodeId = _offeredLease->childId;
    snapshot.parentSourceGeneration = _offeredLease->parentSourceGeneration;
    snapshot.parentRouteGeneration = _offeredLease->routeGeneration;
    snapshot.parentDataRoute = _offeredLease->routes[0];
    snapshot.childBinding = _offeredLease->childBinding;
    snapshot.committedCursor = _qspFrontier;
    snapshot.credentialDeadlineMs = _credentialDeadlineMs;
    snapshot.qtaLimits = _facts->limits;
    if (!snapshot.valid()) return false;

    std::unique_ptr<model::ParentLease> attachedLease;
    try {
        attachedLease = std::make_unique<model::ParentLease>(*_offeredLease);
    } catch (const std::bad_alloc&) {
        return false;
    }
    const size_t maximumChildRedemptions = static_cast<size_t>(
        _facts->limits.maxChildrenPerRelay);
    std::shared_ptr<qclient::spectator::resilient::QtaAuthorityContext>
        qtaAuthority = maximumChildRedemptions == 0u
        ? nullptr
        : qclient::spectator::resilient::QtaAuthorityContext::create(
              _facts->authorityGeneration, _credentialDeadlineMs,
              maximumChildRedemptions);
    if (qtaAuthority == nullptr) {
        wipeBytes(attachedLease.get(), sizeof(*attachedLease));
        return false;
    }
    if (!_client.acceptReceipt(_offeredLease->replayNonce, nowMs)) {
        qtaAuthority->revoke();
        wipeBytes(attachedLease.get(), sizeof(*attachedLease));
        return false;
    }
    _attachedLease = std::move(attachedLease);
    _qtaAuthority = std::move(qtaAuthority);
    _attachmentSnapshot = snapshot;
    wipeBytes(_offeredLease.get(), sizeof(*_offeredLease));
    _offeredLease.reset();
    _state = ReleasedTopologyClientState::Attached;
    _reason = "attached";
    const uint64_t initialWindow = (std::min)(
        kTopologyInitialLivenessWindowMs,
        static_cast<uint64_t>(
            qnetplay::spectator::resilient::wire::
                kTopologyAuthorityStateMaxLivenessWindowMs));
    _livenessDeadlineMs = _credentialDeadlineMs == 0u
        ? addSaturating(nowMs, initialWindow)
        : (std::min)(_credentialDeadlineMs,
                     addSaturating(nowMs, initialWindow));
    return true;
}

bool ReleasedTopologyClientRuntime::makeRelayDataBindingRequest(
    qnetplay::spectator::resilient::wire::RelayDataBindingRequest& out) const {
    out = {};
    if (_state != ReleasedTopologyClientState::Attached || _facts == nullptr
        || _attachedLease == nullptr || _relayDataRequestNonce == 0u) {
        return false;
    }
    out.stream = _attachedLease->stream;
    out.authorityGeneration = _facts->authorityGeneration;
    out.rootNodeId = _facts->rootNodeId;
    out.childNodeId = _attachedLease->childId;
    out.lease = leaseBindingOf(*_attachedLease);
    out.requestNonce = _relayDataRequestNonce;
    return out.stream.valid() && out.authorityGeneration != 0u
        && out.rootNodeId != model::kInvalidNodeId
        && out.childNodeId != model::kInvalidNodeId
        && out.requestNonce != 0u;
}

bool ReleasedTopologyClientRuntime::sendRelayDataBinding(
    const uint64_t nowMs) {
    if (_state != ReleasedTopologyClientState::Attached || _facts == nullptr
        || !_relayDataBindingPending || !_relayDataSocket.isOpen()
        || _relayDataAttemptCount >= kRelayDataBindingMaxAttempts) {
        return false;
    }
    qnetplay::spectator::resilient::wire::RelayDataBindingRequest request{};
    qnetplay::spectator::resilient::wire::ControlDatagram datagram{};
    std::vector<uint8_t> inner;
    std::vector<uint8_t> envelope;
    datagram.type = qnetplay::spectator::resilient::wire::
        ControlMessageType::RelayDataBindingRequest;
    const bool requestReady = makeRelayDataBindingRequest(request);
    datagram.relayDataBindingRequest = request;
    const bool encoded = requestReady
        && qnetplay::spectator::resilient::wire::encodeControlDatagram(
            datagram, inner);
    TranscriptBuilder transcript(*_facts);
    const bool wrapped = encoded && _client.makeAttachedQtaEnvelope(
        inner.data(), inner.size(), transcript, envelope);
    qnet::Endpoint destination{};
    destination.sa.sin_addr.s_addr = _facts->rootQsr.advertisedIpv4BE;
    destination.sa.sin_port = htons(_facts->rootQsr.advertisedPort);
    const bool sent = wrapped && _relayDataSocket.sendTo(
        destination, envelope.data(), envelope.size());
    wipeVector(inner);
    wipeVector(envelope);
    if (!wrapped) {
        clearRelayDataBinding(true);
        return false;
    }
    ++_relayDataAttemptCount;
    _relayDataNextAttemptMs = addSaturating(nowMs, kRelayDataBindingRetryMs);
    return sent;
}

void ReleasedTopologyClientRuntime::serviceRelayDataBinding(
    const uint64_t nowMs) {
    if (!_relayDataBindingPending) return;
    if (_state != ReleasedTopologyClientState::Attached
        || _credentialDeadlineMs == 0u || nowMs >= _credentialDeadlineMs
        || !_relayDataSocket.isOpen()) {
        clearRelayDataBinding(true);
        return;
    }
    if (_relayDataAttemptCount >= kRelayDataBindingMaxAttempts) {
        if (_relayDataNextAttemptMs != 0u
            && nowMs >= _relayDataNextAttemptMs) {
            clearRelayDataBinding(true);
        }
        return;
    }
    if (nowMs < _relayDataNextAttemptMs) return;
    (void)sendRelayDataBinding(nowMs);
}

bool ReleasedTopologyClientRuntime::beginRelayDataBinding(
    uintptr_t& ownedSocket, const uint64_t nowMs) {
    if (_state != ReleasedTopologyClientState::Attached || _facts == nullptr
        || _attachedLease == nullptr || _qtaAuthority == nullptr
        || _credentialDeadlineMs == 0u || nowMs >= _credentialDeadlineMs
        || _relayDataBindingPending || _relayDataBindingSnapshot.valid()
        || _relayDataSocket.isOpen()
        || _relayDataSocketGeneration
            == (std::numeric_limits<uint64_t>::max)()
        || ownedSocket == kInvalidReleasedTopologyRelaySocket) {
        return false;
    }
    const SOCKET suppliedSocket = static_cast<SOCKET>(ownedSocket);
    if (suppliedSocket == INVALID_SOCKET) return false;
    const bool adopted = _relayDataSocket.adopt(suppliedSocket);
    // `adopt` takes the handle before configuring nonblocking mode, so the caller must relinquish it even when that fails.
    ownedSocket = kInvalidReleasedTopologyRelaySocket;
    (void)advanceRelayDataSocketGeneration();
    bool degraded = false;
    const uint64_t requestNonce = qnet::secureRandom64(degraded);
    const uint16_t localPort = _relayDataSocket.boundPort();
    if (!adopted || degraded || requestNonce == 0u || localPort == 0u
        || localPort == _qsrSocket.boundPort()
        || localPort == _qspEndpoint.port) {
        clearRelayDataBinding(true);
        return false;
    }
    _relayDataLocalPort = localPort;
    _relayDataRequestNonce = requestNonce;
    _relayDataNextAttemptMs = nowMs;
    _relayDataAttemptCount = 0u;
    _relayDataBindingPending = true;
    _relayDataBindingUnavailable = false;
    (void)sendRelayDataBinding(nowMs);
    return _relayDataBindingPending;
}

bool ReleasedTopologyClientRuntime::acceptRelayDataBindingAck(
    const qnetplay::spectator::resilient::wire::RelayDataBindingAck& ack,
    const uint64_t nowMs) {
    if (_state != ReleasedTopologyClientState::Attached || _facts == nullptr
        || _attachedLease == nullptr || !_relayDataBindingPending
        || !_relayDataSocket.isOpen() || _relayDataRequestNonce == 0u
        || _credentialDeadlineMs == 0u || nowMs >= _credentialDeadlineMs
        || ack.stream != _attachedLease->stream
        || ack.authorityGeneration != _facts->authorityGeneration
        || ack.rootNodeId != _facts->rootNodeId
        || ack.childNodeId != _attachedLease->childId
        || !sameLeaseBinding(ack.lease, *_attachedLease)
        || ack.requestNonce != _relayDataRequestNonce
        || !ack.observedDataRoute.valid()
        || ack.observedDataRoute.kind != model::RouteKind::Direct
        || !ack.observedDataRoute.routeProved
        || !ack.observedControlRoute.valid()
        || ack.observedControlRoute.kind != model::RouteKind::Direct
        || !ack.observedControlRoute.routeProved
        || ack.observedDataRoute.sameEndpoint(ack.observedControlRoute)
        || ack.observedDataRoute.port == _qspEndpoint.port
        || ack.observedDataRoute.generation == 0u
        || ack.observedDataRoute.generation
            != ack.observedControlRoute.generation
        || ack.observedDataRoute.generation
            != _attachedLease->childBinding.childEndpointBindingGeneration) {
        return false;
    }
    ReleasedTopologyRelayBindingSnapshot snapshot{};
    snapshot.stream = ack.stream;
    snapshot.authorityGeneration = ack.authorityGeneration;
    snapshot.rootNodeId = ack.rootNodeId;
    snapshot.childNodeId = ack.childNodeId;
    snapshot.observedDataRoute = ack.observedDataRoute;
    snapshot.observedControlRoute = ack.observedControlRoute;
    snapshot.credentialDeadlineMs = _credentialDeadlineMs;
    if (!snapshot.valid()) return false;
    _relayDataBindingSnapshot = snapshot;
    _relayDataBindingPending = false;
    _relayDataNextAttemptMs = 0u;
    return true;
}

bool ReleasedTopologyClientRuntime::makeRelayReady(
    qnetplay::spectator::resilient::wire::RelayReady& out) const {
    out = {};
    if (_state != ReleasedTopologyClientState::Attached || _facts == nullptr
        || _attachedLease == nullptr || _relayNode == nullptr
        || _relayCanonicalIngress == nullptr || !_relayNode->running()
        || !_relayDataBindingSnapshot.valid()
        || _relayDataRequestNonce == 0u || !_relayDataSocket.isOpen()) {
        return false;
    }
    const model::ResilientRelayNode* const relay = _relayNode->relayNode();
    if (relay == nullptr) return false;

    model::ResilientLimits limits{};
    limits.maxTopologyNodes = static_cast<size_t>(_facts->limits.maxTopologyNodes);
    limits.maxActiveDepth = _facts->limits.maxActiveDepth;
    limits.maxChildrenPerRoot = static_cast<size_t>(
        _facts->limits.maxChildrenPerRoot);
    limits.maxChildrenPerRelay = static_cast<size_t>(
        _facts->limits.maxChildrenPerRelay);
    limits.maxStandbyParents = static_cast<size_t>(
        _facts->limits.maxStandbyParents);
    limits.standbyLeaseMs = _facts->limits.standbyLeaseMs;
    const model::VerifiedRelayCache& cache = relay->cache();
    const model::VerifiedCacheRange range = cache.range();
    const model::VerifiedChainCursor cursor = cache.frontier();
    if (!limits.valid() || range.nextCursor < range.firstCursor
        || static_cast<size_t>(range.nextCursor - range.firstCursor)
            < limits.requiredCacheRecords()
        || cursor.nextRecord != range.nextCursor
        || cursor.digest == 0u
        || !range.containsBoundary(_attachedLease->childBinding
                                       .childRequestedCursor.nextRecord)) {
        return false;
    }

    out.stream = _attachedLease->stream;
    out.authorityGeneration = _facts->authorityGeneration;
    out.rootNodeId = _facts->rootNodeId;
    out.relayNodeId = _attachedLease->childId;
    out.relayLease = leaseBindingOf(*_attachedLease);
    out.bindingRequestNonce = _relayDataRequestNonce;
    out.observedDataRoute = _relayDataBindingSnapshot.observedDataRoute;
    out.observedControlRoute = _relayDataBindingSnapshot.observedControlRoute;
    out.cacheRange = range;
    out.cacheCursor = cursor;
    return out.stream.valid() && out.authorityGeneration != 0u
        && out.rootNodeId != model::kInvalidNodeId
        && out.relayNodeId != model::kInvalidNodeId
        && out.rootNodeId != out.relayNodeId
        && out.bindingRequestNonce != 0u
        && out.observedDataRoute.valid()
        && out.observedControlRoute.valid();
}

bool ReleasedTopologyClientRuntime::sendRelayReady(const uint64_t nowMs) {
    if (!relayAuthorityActive(nowMs) || _facts == nullptr
        || !_qsrSocket.isOpen()
        || _relayReadyAttemptCount >= kRelayReadyMaxAttempts) {
        return false;
    }
    qnetplay::spectator::resilient::wire::RelayReady ready{};
    qnetplay::spectator::resilient::wire::ControlDatagram datagram{};
    std::vector<uint8_t> inner;
    std::vector<uint8_t> envelope;
    const bool readyBuilt = makeRelayReady(ready);
    datagram.type = qnetplay::spectator::resilient::wire::
        ControlMessageType::RelayReady;
    datagram.relayReady = ready;
    const bool encoded = readyBuilt
        && qnetplay::spectator::resilient::wire::encodeControlDatagram(
            datagram, inner);
    TranscriptBuilder transcript(*_facts);
    const bool wrapped = encoded && _client.makeAttachedQtaEnvelope(
        inner.data(), inner.size(), transcript, envelope);
    const bool sent = wrapped && sendQsrDatagram(envelope);
    wipeVector(inner);
    wipeVector(envelope);
    if (!wrapped) {
        clearRelayDataBinding(true);
        return false;
    }
    ++_relayReadyAttemptCount;
    _relayReadyNextAttemptMs = addSaturating(nowMs, kRelayReadyRetryMs);
    return sent;
}

void ReleasedTopologyClientRuntime::serviceRelayReady(const uint64_t nowMs) {
    if (!relayAuthorityActive(nowMs) || _relayNode == nullptr
        || !_relayNode->running()) {
        return;
    }
    qnetplay::spectator::resilient::wire::RelayReady ready{};
    if (!makeRelayReady(ready)) return;

    if (ready.cacheCursor.nextRecord != _lastRelayReadyCursor.nextRecord
        || ready.cacheCursor.nextEvent != _lastRelayReadyCursor.nextEvent
        || ready.cacheCursor.digest != _lastRelayReadyCursor.digest) {
        _lastRelayReadyCursor = ready.cacheCursor;
        _relayReadyAttemptCount = 0u;
        _relayReadyNextAttemptMs = nowMs;
    }
    if (_relayReadyAttemptCount >= kRelayReadyMaxAttempts
        || nowMs < _relayReadyNextAttemptMs) {
        return;
    }
    (void)sendRelayReady(nowMs);
}

void ReleasedTopologyClientRuntime::clearPendingRelayChildActivation() {
    if (!_pendingRelayChildActivation.has_value()) return;
    (void)_client.cancelActivationReceiptQsrPayload(
        _pendingRelayChildActivation->activation.activationNonce);
    wipeBytes(&_pendingRelayChildActivation->activation,
              sizeof(_pendingRelayChildActivation->activation));
    _pendingRelayChildActivation.reset();
}

void ReleasedTopologyClientRuntime::retirePendingRelayChildActivation() {
    if (!_pendingRelayChildActivation.has_value()) return;
    if (_retiredRelayChildActivation.has_value()) {
        wipeBytes(&_retiredRelayChildActivation->activation,
                  sizeof(_retiredRelayChildActivation->activation));
        _retiredRelayChildActivation.reset();
    }
    _retiredRelayChildActivation.emplace();
    _retiredRelayChildActivation->activation =
        _pendingRelayChildActivation->activation;
    clearPendingRelayChildActivation();
}

void ReleasedTopologyClientRuntime::clearPendingClientPlayerRootActivation() {
    if (!_pendingClientPlayerRootActivation.has_value()) return;
    (void)_client.cancelActivationReceiptQsrPayload(
        _pendingClientPlayerRootActivation->activation.activationNonce);
    wipeBytes(&_pendingClientPlayerRootActivation->activation,
              sizeof(_pendingClientPlayerRootActivation->activation));
    _pendingClientPlayerRootActivation.reset();
}

void ReleasedTopologyClientRuntime::retirePendingClientPlayerRootActivation() {
    if (!_pendingClientPlayerRootActivation.has_value()) return;
    if (_retiredClientPlayerRootActivation.has_value()) {
        wipeBytes(&_retiredClientPlayerRootActivation->activation,
                  sizeof(_retiredClientPlayerRootActivation->activation));
        _retiredClientPlayerRootActivation.reset();
    }
    _retiredClientPlayerRootActivation.emplace();
    _retiredClientPlayerRootActivation->activation =
        _pendingClientPlayerRootActivation->activation;
    clearPendingClientPlayerRootActivation();
}

void ReleasedTopologyClientRuntime::retainCompletedParentRouteActivation(
    const qnetplay::spectator::resilient::wire::RelayChildActivation&
        activation) {
    if (_completedParentRouteActivation.has_value()) {
        wipeBytes(&*_completedParentRouteActivation,
                  sizeof(*_completedParentRouteActivation));
        _completedParentRouteActivation.reset();
    }
    _completedParentRouteActivation.emplace(retirementBindingOf(activation));
}

void ReleasedTopologyClientRuntime::retainCompletedClientPlayerRootActivation(
    const qnetplay::spectator::resilient::wire::ClientPlayerRootActivation&
        activation) {
    if (_completedClientPlayerRootActivation.has_value()) {
        wipeBytes(&*_completedClientPlayerRootActivation,
                  sizeof(*_completedClientPlayerRootActivation));
        _completedClientPlayerRootActivation.reset();
    }
    _completedClientPlayerRootActivation.emplace(activation);
}

void ReleasedTopologyClientRuntime::clearPendingAttachedRelayActivation() {
    if (!_pendingAttachedRelayActivation.has_value()) return;
    (void)_client.cancelAttachedRelayActivationReceiptQsrPayload(
        _pendingAttachedRelayActivation->activation.activationNonce);
    wipeBytes(&_pendingAttachedRelayActivation->activation,
              sizeof(_pendingAttachedRelayActivation->activation));
    _pendingAttachedRelayActivation.reset();
}

void ReleasedTopologyClientRuntime::retirePendingAttachedRelayActivation() {
    if (!_pendingAttachedRelayActivation.has_value()) return;
    if (_retiredAttachedRelayActivation.has_value()) {
        wipeBytes(&_retiredAttachedRelayActivation->activation,
                  sizeof(_retiredAttachedRelayActivation->activation));
        _retiredAttachedRelayActivation.reset();
    }
    _retiredAttachedRelayActivation.emplace();
    _retiredAttachedRelayActivation->activation =
        _pendingAttachedRelayActivation->activation;
    clearPendingAttachedRelayActivation();
}

void ReleasedTopologyClientRuntime::retireCommittedAttachedRelayChild(
    const uint64_t nowMs) {
    using qclient::spectator::resilient::NodeRuntimeResult;

    if (!_pendingAttachedRelayActivation.has_value()
        || !_pendingAttachedRelayActivation->committed || _relayNode == nullptr
        || !_relayNode->running()) {
        return;
    }
    const auto& activation = _pendingAttachedRelayActivation->activation;
    const model::LeaseBinding childLease = leaseBindingOf(activation.childLease);
    pruneRetiredRelayChildren();
    const auto found = std::find_if(
        _relayChildren.begin(), _relayChildren.end(),
        [&activation, &childLease](const RelayChildBinding& candidate) {
            return candidate.childNodeId == activation.childNodeId
                && sameLeaseBinding(candidate.childLease, childLease)
                && sameRoute(candidate.route,
                             activation.childLease.childBinding.childQspRoute)
                && candidate.endpointBindingGeneration
                    == activation.childLease.childBinding
                           .childEndpointBindingGeneration
                && candidate.endpointToken
                    == static_cast<qnetplay::spectator::EndpointId>(
                        activation.childLease.childBinding.childViewerId);
        });
    if (found == _relayChildren.end() || !found->retirement.has_value()) return;

    const NodeRuntimeResult result = _relayNode->retireQtaVerifiedChild(
        std::move(*found->retirement), nowMs);
    if (result == NodeRuntimeResult::Accepted) {
        _relayChildren.erase(found);
    } else {
        pruneRetiredRelayChildren();
    }
}

bool ReleasedTopologyClientRuntime::activationLocalDeadline(
    const qnetplay::spectator::resilient::wire::RelayChildActivation&
        activation,
    const uint64_t acceptedAtMs, uint64_t& out) const {
    out = 0u;
    if (_facts == nullptr || _credentialDeadlineMs == 0u
        || activation.activationDeadlineMs == 0u
        || _facts->expiryMs == 0u
        || activation.activationDeadlineMs > _facts->expiryMs) {
        return false;
    }
    const uint64_t hostRestrictionMs =
        _facts->expiryMs - activation.activationDeadlineMs;
    if (hostRestrictionMs >= _credentialDeadlineMs) return false;
    const uint64_t localDeadline = _credentialDeadlineMs - hostRestrictionMs;
    if (localDeadline <= acceptedAtMs) return false;
    out = localDeadline;
    return true;
}

bool ReleasedTopologyClientRuntime::activationLocalDeadline(
    const qnetplay::spectator::resilient::wire::ClientPlayerRootActivation&
        activation,
    const uint64_t acceptedAtMs, uint64_t& out) const {
    out = 0u;
    if (_facts == nullptr || _credentialDeadlineMs == 0u
        || activation.activationDeadlineMs == 0u
        || _facts->expiryMs == 0u
        || activation.activationDeadlineMs > _facts->expiryMs) {
        return false;
    }
    const uint64_t hostRestrictionMs =
        _facts->expiryMs - activation.activationDeadlineMs;
    if (hostRestrictionMs >= _credentialDeadlineMs) return false;
    const uint64_t localDeadline = _credentialDeadlineMs - hostRestrictionMs;
    if (localDeadline <= acceptedAtMs) return false;
    out = localDeadline;
    return true;
}

void ReleasedTopologyClientRuntime::servicePairedActivations(
    const uint64_t nowMs) {
    if (_pendingRelayChildActivation.has_value()
        && (_pendingRelayChildActivation->localDeadlineMs == 0u
            || nowMs >= _pendingRelayChildActivation->localDeadlineMs)) {
        retirePendingRelayChildActivation();
    }
    if (_pendingClientPlayerRootActivation.has_value()
        && (_pendingClientPlayerRootActivation->localDeadlineMs == 0u
            || nowMs
                >= _pendingClientPlayerRootActivation->localDeadlineMs)) {
        retirePendingClientPlayerRootActivation();
    }
    if (_pendingAttachedRelayActivation.has_value()
        && (_pendingAttachedRelayActivation->localDeadlineMs == 0u
            || nowMs >= _pendingAttachedRelayActivation->localDeadlineMs)) {
        retireCommittedAttachedRelayChild(nowMs);
        retirePendingAttachedRelayActivation();
    }
}

void ReleasedTopologyClientRuntime::clearRelayDataBinding(
    const bool unavailable) {
    retirePendingAttachedRelayActivation();
    const bool retiredSocket = _relayDataSocket.isOpen();
    if (_relayNode) _relayNode->stop();
    _relayNode.reset();
    _relayCanonicalIngress.reset();
    _relayViewer = nullptr;
    _relayDataSocket.close();
    if (retiredSocket) (void)advanceRelayDataSocketGeneration();
    _relayDataLocalPort = 0u;
    _relayDataRequestNonce = 0u;
    _relayDataNextAttemptMs = 0u;
    _relayDataAttemptCount = 0u;
    _relayDataBindingPending = false;
    _relayDataBindingUnavailable = unavailable;
    _relayDataBindingSnapshot = {};
    _relayChildren.clear();
    _lastRelayReadyCursor = {};
    _relayReadyNextAttemptMs = 0u;
    _relayReadyAttemptCount = 0u;
}

bool ReleasedTopologyClientRuntime::advanceRelayDataSocketGeneration() {
    if (_relayDataSocketGeneration == (std::numeric_limits<uint64_t>::max)())
        return false;
    ++_relayDataSocketGeneration;
    return true;
}

bool ReleasedTopologyClientRuntime::relayAuthorityActive(
    const uint64_t nowMs) const {
    return _state == ReleasedTopologyClientState::Attached
        && _facts != nullptr && _attachedLease != nullptr
        && _qtaAuthority != nullptr && _credentialDeadlineMs != 0u
        && nowMs < _credentialDeadlineMs
        && _qtaAuthority->activeFor(_facts->authorityGeneration, nowMs);
}

bool ReleasedTopologyClientRuntime::makeRelayNodeConfig(
    const qnetplay::spectator::SpectatorViewer& verifiedViewer,
    qclient::spectator::resilient::ResilientSpectatorNodeRuntimeConfig& out)
    const {
    using qclient::spectator::resilient::NodeRuntimeIngressMode;
    out = {};
    if (_facts == nullptr || _attachedLease == nullptr
        || !_relayDataBindingSnapshot.valid() || !_relayDataSocket.isOpen()
        || _relayDataLocalPort == 0u
        || _relayDataSocket.boundPort() != _relayDataLocalPort) {
        return false;
    }
    qnetplay::spectator::GameStartPayload start{};
    model::StreamCompatibilityIdentity stream{};
    if (!verifiedViewer.gameStart(start) || start.gameId == 0u
        || start.simulationContract == 0u || start.initialDigest == 0u
        || !qnetplay::compat::semanticallyEqual(
            start.compatibilityIdentity, _facts->compatibility)
        || !model::makeStreamCompatibilityIdentity(
            _facts->compatibility, _facts->sessionId, _facts->sessionEpoch,
            qnetplay::spectator::gameStartFingerprint(start), stream)
        || stream != _attachedLease->stream
        || _relayDataBindingSnapshot.stream != stream
        || _relayDataBindingSnapshot.authorityGeneration
            != _facts->authorityGeneration
        || _relayDataBindingSnapshot.rootNodeId != _facts->rootNodeId
        || _relayDataBindingSnapshot.childNodeId != _attachedLease->childId
        || _relayDataBindingSnapshot.observedDataRoute.generation
            != _attachedLease->childBinding.childEndpointBindingGeneration
        || !hasExactAttachedRelayLineage(
            *_attachedLease, _facts->rootNodeId, _facts->childNodeId,
            _facts->limits.maxActiveDepth)
        || _facts->limits.maxTopologyNodes
            > (std::numeric_limits<size_t>::max)()
        || _facts->limits.maxChildrenPerRoot
            > (std::numeric_limits<size_t>::max)()
        || _facts->limits.maxChildrenPerRelay
            > (std::numeric_limits<size_t>::max)()
        || _facts->limits.maxStandbyParents
            > (std::numeric_limits<size_t>::max)()
        || _facts->limits.maxChildrenPerRelay
            > (std::numeric_limits<uint32_t>::max)()
        || _facts->limits.maxChildrenPerRoot
            > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }

    model::ResilientLimits limits{};
    limits.maxTopologyNodes = static_cast<size_t>(
        _facts->limits.maxTopologyNodes);
    limits.maxActiveDepth = _facts->limits.maxActiveDepth;
    limits.maxChildrenPerRoot = static_cast<size_t>(
        _facts->limits.maxChildrenPerRoot);
    limits.maxChildrenPerRelay = static_cast<size_t>(
        _facts->limits.maxChildrenPerRelay);
    limits.maxStandbyParents = static_cast<size_t>(
        _facts->limits.maxStandbyParents);
    limits.standbyLeaseMs = _facts->limits.standbyLeaseMs;
    if (!limits.valid()) return false;

    out.stream = stream;
    out.qspIdentity = qnetplay::spectator::SessionIdentity{
        _facts->sessionId, _facts->sessionEpoch,
        qnetplay::spectator::SessionCookie{
            _facts->spectatorCookieLow, _facts->spectatorCookieHigh}};
    out.simulationContract = start.simulationContract;
    out.compatibilityIdentity = _facts->compatibility;
    out.gameId = start.gameId;
    out.initialDigest = start.initialDigest;
    out.sourceGeneration = _attachedLease->parentSourceGeneration;
    out.authorityGeneration = _facts->authorityGeneration;
    out.initialParentGeneration = _attachedLease->parentSourceGeneration;
    out.initialRouteGeneration = _attachedLease->routeGeneration;
    out.limits = limits;
    out.ingressMode = NodeRuntimeIngressMode::CanonicalHostAttested;
    out.productionRelay = true;

    model::NodeAdvertisement& local = out.localAdvertisement;
    local.nodeId = _attachedLease->childId;
    local.kind = model::NodeKind::SpectatorRelay;
    local.stream = stream;
    local.route = _relayDataBindingSnapshot.observedDataRoute;
    local.controlRoute = _relayDataBindingSnapshot.observedControlRoute;
    local.capabilityModelRevision = model::kResilientCapabilityModelRevision;
    local.capabilities = model::kCapabilityRelayCapableSpectator;
    local.childCapacity = static_cast<uint32_t>(limits.maxChildrenPerRelay);
    local.uploadCapacity = local.childCapacity;
    local.cacheFirstCursor = 0u;
    local.cacheNextCursor = 0u;
    local.sourceGeneration = out.sourceGeneration;
    local.routeGeneration = local.route.generation;
    local.lastProgressMs = 1u;
    local.reachable = true;

    model::NodeAdvertisement& parent = out.initialParentAdvertisement;
    parent.nodeId = _attachedLease->parentId;
    parent.kind = _attachedLease->parentId == _facts->rootNodeId
        ? model::NodeKind::HostRoot : model::NodeKind::SpectatorRelay;
    parent.stream = stream;
    parent.route = _attachedLease->routes[0];
    parent.controlRoute = routeFromWire(_facts->rootQsr);
    parent.capabilityModelRevision = model::kResilientCapabilityModelRevision;
    parent.childCapacity = static_cast<uint32_t>(limits.maxChildrenPerRoot);
    parent.uploadCapacity = parent.childCapacity;
    parent.cacheFirstCursor = 0u;
    parent.cacheNextCursor = 0u;
    parent.sourceGeneration = out.sourceGeneration;
    parent.routeGeneration = _attachedLease->routeGeneration;
    parent.lastProgressMs = 1u;
    parent.reachable = true;

    out.initialParentDepth = _attachedLease->parentDepth;
    out.initialParentAncestryCount = static_cast<uint8_t>(
        _attachedLease->ancestryCount - 1u);
    for (size_t index = 0u;
         index < static_cast<size_t>(out.initialParentAncestryCount);
         ++index) {
        out.initialParentAncestry[index] = _attachedLease->ancestry[index];
    }
    return true;
}

void ReleasedTopologyClientRuntime::serviceRelayNode(const uint64_t nowMs) {
    if (!_relayNode) return;
    if (!relayAuthorityActive(nowMs)) {
        clearRelayDataBinding(true);
        return;
    }
    _relayNode->tick(nowMs);
    if (!_relayNode->running()) {
        clearRelayDataBinding(true);
        return;
    }
    pruneRetiredRelayChildren();
    serviceRelayReady(nowMs);
}

void ReleasedTopologyClientRuntime::pruneRetiredRelayChildren() {
    if (_relayChildren.empty()) return;
    if (_relayNode == nullptr || !_relayNode->running()) {
        _relayChildren.clear();
        return;
    }
    _relayChildren.erase(std::remove_if(
        _relayChildren.begin(), _relayChildren.end(),
        [this](const RelayChildBinding& binding) {
            return !_relayNode->hasActiveChild(binding.childNodeId);
        }), _relayChildren.end());
}

bool ReleasedTopologyClientRuntime::acceptAuthorityState(
    const qnetplay::spectator::resilient::wire::AuthorityState& state,
    const uint64_t nowMs) {
    using qnetplay::spectator::resilient::wire::AuthorityStateKind;
    if (_facts == nullptr || !_qsrSocket.isOpen()
        || !streamEquals(state.stream, _facts->compatibility,
                         _facts->sessionId, _facts->sessionEpoch,
                         _facts->streamRootIdentity)
        || state.authorityGeneration != _facts->authorityGeneration
        || state.rootNodeId != _facts->rootNodeId
        || state.childNodeId != _facts->childNodeId
        || state.viewerId != _facts->viewerId
        || state.viewerGeneration != _facts->viewerGeneration
        || state.qspRouteGeneration != _facts->qspRouteGeneration
        || state.qsrBindingGeneration != _facts->bindingGeneration
        || state.qsrIpv4BE != _qspEndpoint.ipv4BE
        || state.qsrPort != _qsrSocket.boundPort()
        || state.authorityExpiryMs != _facts->expiryMs
        || state.eventTimeMs == 0u || state.sequence == 0u
        || state.sequence <= _lastAuthorityStateSequence) {
        return true;
    }
    if (state.kind == AuthorityStateKind::Liveness) {
        if (_state != ReleasedTopologyClientState::Attached
            || state.livenessWindowMs == 0u
            || state.livenessWindowMs
                > qnetplay::spectator::resilient::wire::
                    kTopologyAuthorityStateMaxLivenessWindowMs) {
            return true;
        }
        const uint64_t proposed = addSaturating(nowMs, state.livenessWindowMs);
        _livenessDeadlineMs = _credentialDeadlineMs == 0u
            ? proposed : (std::min)(_credentialDeadlineMs, proposed);
        _lastAuthorityStateSequence = state.sequence;
        return true;
    }
    if (state.kind
        == qnetplay::spectator::resilient::wire::AuthorityStateKind::Revocation) {
        _lastAuthorityStateSequence = state.sequence;
        failSoft("topology-authority-revoked");
        return true;
    }
    return true;
}

bool ReleasedTopologyClientRuntime::acceptRelayChildActivation(
    const qnetplay::spectator::resilient::wire::RelayChildActivation&
        activation,
    const uint64_t nowMs) {
    using qnetplay::spectator::resilient::wire::ControlDatagram;
    using qnetplay::spectator::resilient::wire::ControlMessageType;
    using qnetplay::spectator::resilient::wire::RelayChildActivationReceipt;
    using qnetplay::spectator::resilient::wire::RelayChildActivationReceiptRole;

    if (_state != ReleasedTopologyClientState::AwaitingRootOffer
        || _facts == nullptr || _offeredLease != nullptr
        || _attachedLease != nullptr || !_qsrSocket.isOpen()
        || !_assignmentVerifier.ready() || _credentialDeadlineMs == 0u
        || nowMs >= _credentialDeadlineMs) {
        return true;
    }

    if (_pendingClientPlayerRootActivation.has_value()) return true;

    std::vector<uint8_t> attestationInput;
    const bool hostAttested = qnetplay::spectator::resilient::wire::
            encodeRelayChildActivationAttestationInput(
                activation, attestationInput)
        && _assignmentVerifier.verify(attestationInput.data(),
                                      attestationInput.size(),
                                      activation.hostSignature);
    wipeVector(attestationInput);
    if (!hostAttested) return true;

    uint64_t localActivationDeadlineMs = 0u;
    if (!activationLocalDeadline(activation, nowMs, localActivationDeadlineMs))
        return true;

    if (_pendingRelayChildActivation.has_value()) {
        return sameRelayChildActivation(
            _pendingRelayChildActivation->activation, activation);
    }
    if (_retiredRelayChildActivation.has_value()
        && sameRelayChildActivation(
            _retiredRelayChildActivation->activation, activation)) {
        return true;
    }

    model::ParentLeaseChildBinding expectedChildBinding{};
    const bool qspDirect = _facts->qspProvenance
        == static_cast<uint8_t>(EndpointProvenance::ProvenDirectQsp);
    const bool qspPunched = _facts->qspProvenance
        == static_cast<uint8_t>(EndpointProvenance::ProvenPunchedQsp);
    const bool qspRelay = _facts->qspProvenance
        == static_cast<uint8_t>(EndpointProvenance::ProvenRelayQsp);
    if (qspDirect || qspPunched || qspRelay) {
        expectedChildBinding.childQspRoute.kind = qspRelay
            ? model::RouteKind::TrafficRelay
            : (qspPunched ? model::RouteKind::Punched
                          : model::RouteKind::Direct);
        expectedChildBinding.childQspRoute.ipv4BE = _facts->qspIpv4BE;
        expectedChildBinding.childQspRoute.port = _facts->qspPort;
        expectedChildBinding.childQspRoute.generation =
            _facts->qspRouteGeneration;
        expectedChildBinding.childQspRoute.routeProved = true;
    }
    expectedChildBinding.authorityGeneration = _facts->authorityGeneration;
    expectedChildBinding.childViewerId = _facts->viewerId;
    expectedChildBinding.childViewerGeneration = _facts->viewerGeneration;
    expectedChildBinding.childEndpointBindingGeneration =
        _facts->bindingGeneration;
    expectedChildBinding.childRequestedCursor = {
        _facts->cursor.nextRecord, _facts->cursor.nextEvent,
        _facts->cursor.digest};

    const model::ParentLease& childLease = activation.childLease;
    const model::LeaseBinding childLeaseBinding = leaseBindingOf(childLease);
    const bool exactLeaseChain =
        hasExactDerivedRelayChildActivationLeaseChain(
            activation, _facts->limits.maxActiveDepth);
    const bool exactChild = streamEquals(
            activation.stream, _facts->compatibility, _facts->sessionId,
            _facts->sessionEpoch, _facts->streamRootIdentity)
        && activation.authorityGeneration == _facts->authorityGeneration
        && activation.rootNodeId == _facts->rootNodeId
        && activation.relayNodeId != model::kInvalidNodeId
        && activation.relayNodeId != activation.rootNodeId
        && activation.childNodeId == _facts->childNodeId
        && activation.childNodeId != activation.rootNodeId
        && activation.childNodeId != activation.relayNodeId
        && expectedChildBinding.valid()
        && childLease.stream == activation.stream
        && childLease.childId == activation.childNodeId
        && childLease.parentId == activation.relayNodeId
        && childLease.childBinding == expectedChildBinding
        && childLease.parentSourceGeneration == _facts->sourceGeneration
        && childLease.routeGeneration == activation.relayDataRoute.generation
        && childLease.routeCount == 1u
        && sameRoute(childLease.routes[0], activation.relayDataRoute)
        && childLease.cacheRange.firstCursor
            == activation.relayCacheRange.firstCursor
        && childLease.cacheRange.nextCursor
            == activation.relayCacheRange.nextCursor
        && childLease.latestVerifiedCursor
            == activation.commonCursor.nextRecord
        && sameRoute(activation.childDataRoute,
                     expectedChildBinding.childQspRoute)
        && activation.childControlRoute.valid()
        && activation.childControlRoute.routeProved
        && activation.childControlRoute.kind == model::RouteKind::Direct
        && activation.childControlRoute.port == _qsrSocket.boundPort()
        && activation.childControlRoute.generation == _facts->bindingGeneration
        && activation.commonCursor.nextRecord
            == expectedChildBinding.childRequestedCursor.nextRecord
        && activation.commonCursor.nextEvent
            == expectedChildBinding.childRequestedCursor.nextEvent
        && activation.commonCursor.digest
            == expectedChildBinding.childRequestedCursor.digest
        && _qspFrontierObserved
        && activation.commonCursor.nextRecord == _qspFrontier.nextRecord
        && activation.commonCursor.nextEvent == _qspFrontier.nextEvent
        && activation.commonCursor.digest == _qspFrontier.digest
        && exactLeaseChain
        && activation.relayBindingRequestNonce != 0u
        && activation.relayDataRoute.valid()
        && activation.relayDataRoute.routeProved
        && activation.relayDataRoute.kind == model::RouteKind::Direct
        && activation.relayDataRoute.generation
            == activation.relayLease.childBinding
                   .childEndpointBindingGeneration
        && childLeaseBinding.childId == activation.childNodeId
        && childLeaseBinding.parentId == activation.relayNodeId
        && childLeaseBinding.childBinding == expectedChildBinding
        && activation.relayCacheRange.nextCursor
            == activation.commonCursor.nextRecord;
    if (!exactChild) return true;

    RelayChildActivationReceipt receipt{};
    ControlDatagram receiptDatagram{};
    receiptDatagram.type = ControlMessageType::RelayChildActivationReceipt;
    const bool receiptBuilt = qnetplay::spectator::resilient::wire::
            makeRelayChildActivationReceipt(
                activation, RelayChildActivationReceiptRole::Child, receipt);
    receiptDatagram.relayChildActivationReceipt = receipt;
    std::vector<uint8_t> receiptBytes;
    const bool receiptEncoded = receiptBuilt
        && qnetplay::spectator::resilient::wire::encodeControlDatagram(
            receiptDatagram, receiptBytes);
    if (!receiptEncoded) {
        wipeVector(receiptBytes);
        return true;
    }

    _pendingRelayChildActivation.emplace();
    _pendingRelayChildActivation->activation = activation;
    _pendingRelayChildActivation->localDeadlineMs = localActivationDeadlineMs;
    const bool queued = _client.setActivationReceiptQsrPayload(
        receiptBytes.data(), receiptBytes.size(), activation.activationNonce,
        nowMs);
    wipeVector(receiptBytes);
    if (!queued) clearPendingRelayChildActivation();
    return true;
}

bool ReleasedTopologyClientRuntime::acceptRelayChildActivationRetirement(
    const qnetplay::spectator::resilient::wire::
        RelayChildActivationRetirement& retirement,
    const uint64_t nowMs) {
    if (_state == ReleasedTopologyClientState::AwaitingRootOffer
        && _pendingRelayChildActivation.has_value()
        && matchesRelayChildActivationRetirement(
            retirement, _pendingRelayChildActivation->activation)) {
        retirePendingRelayChildActivation();
    } else if (_state == ReleasedTopologyClientState::Attached
               && _pendingAttachedRelayActivation.has_value()
               && matchesRelayChildActivationRetirement(
                   retirement,
                   _pendingAttachedRelayActivation->activation)) {
        retireCommittedAttachedRelayChild(nowMs);
        retirePendingAttachedRelayActivation();
    } else if (_state == ReleasedTopologyClientState::Attached
               && _completedParentRouteActivation.has_value()
               && sameRelayChildActivationRetirementBinding(
                   retirement, *_completedParentRouteActivation)) {
        failSoft("topology-parent-route-activation-retired");
    }
    return true;
}

bool ReleasedTopologyClientRuntime::acceptClientPlayerRootActivation(
    const qnetplay::spectator::resilient::wire::ClientPlayerRootActivation&
        activation,
    const uint64_t nowMs) {
    using qnetplay::spectator::resilient::wire::ClientPlayerRootActivationReceipt;
    using qnetplay::spectator::resilient::wire::ControlDatagram;
    using qnetplay::spectator::resilient::wire::ControlMessageType;

    if (_state != ReleasedTopologyClientState::AwaitingRootOffer
        || _facts == nullptr || _offeredLease != nullptr
        || _attachedLease != nullptr || !_qsrSocket.isOpen()
        || !_assignmentVerifier.ready() || _credentialDeadlineMs == 0u
        || nowMs >= _credentialDeadlineMs) {
        return true;
    }

    std::vector<uint8_t> attestationInput;
    const bool hostAttested = qnetplay::spectator::resilient::wire::
            encodeClientPlayerRootActivationAttestationInput(
                activation, attestationInput)
        && _assignmentVerifier.verify(attestationInput.data(),
                                      attestationInput.size(),
                                      activation.hostSignature);
    wipeVector(attestationInput);
    if (!hostAttested) return true;

    uint64_t localActivationDeadlineMs = 0u;
    if (!activationLocalDeadline(activation, nowMs, localActivationDeadlineMs))
        return true;

    if (_pendingRelayChildActivation.has_value()) return true;
    if (_pendingClientPlayerRootActivation.has_value()) {
        return true;
    }
    if (_retiredClientPlayerRootActivation.has_value()
        && sameClientPlayerRootActivation(
            _retiredClientPlayerRootActivation->activation, activation)) {
        return true;
    }

    model::ParentLeaseChildBinding expectedTargetBinding{};
    const bool qspDirect = _facts->qspProvenance
        == static_cast<uint8_t>(EndpointProvenance::ProvenDirectQsp);
    const bool qspPunched = _facts->qspProvenance
        == static_cast<uint8_t>(EndpointProvenance::ProvenPunchedQsp);
    if (qspDirect || qspPunched) {
        expectedTargetBinding.childQspRoute.kind = qspPunched
            ? model::RouteKind::Punched : model::RouteKind::Direct;
        expectedTargetBinding.childQspRoute.ipv4BE = _facts->qspIpv4BE;
        expectedTargetBinding.childQspRoute.port = _facts->qspPort;
        expectedTargetBinding.childQspRoute.generation =
            _facts->qspRouteGeneration;
        expectedTargetBinding.childQspRoute.routeProved = true;
    }
    expectedTargetBinding.authorityGeneration = _facts->authorityGeneration;
    expectedTargetBinding.childViewerId = _facts->viewerId;
    expectedTargetBinding.childViewerGeneration = _facts->viewerGeneration;
    expectedTargetBinding.childEndpointBindingGeneration =
        _facts->bindingGeneration;
    expectedTargetBinding.childRequestedCursor = {
        _facts->cursor.nextRecord, _facts->cursor.nextEvent,
        _facts->cursor.digest};

    const auto& binding = activation.binding;
    const model::ParentLease& lease = activation.targetLease;
    const RouteEndpoint existingParent{
        _facts->rootQsp.advertisedIpv4BE, _facts->rootQsp.advertisedPort};
    const RouteEndpoint candidateParent{binding.dataRoute.ipv4BE,
                                        binding.dataRoute.port};
    const bool exactTarget = expectedTargetBinding.valid()
        && hasExactClientPlayerRootActivationLeaseChain(activation)
        && streamEquals(binding.stream, _facts->compatibility,
                        _facts->sessionId, _facts->sessionEpoch,
                        _facts->streamRootIdentity)
        && binding.gameStartFingerprint == _facts->streamRootIdentity
        && activation.authorityGeneration == _facts->authorityGeneration
        && activation.hostRootNodeId == _facts->rootNodeId
        && activation.targetViewerNodeId == _facts->childNodeId
        && binding.sourceGeneration == _facts->sourceGeneration
        && binding.cursor.nextRecord == _facts->cursor.nextRecord
        && binding.cursor.nextEvent == _facts->cursor.nextEvent
        && binding.cursor.digest == _facts->cursor.digest
        && lease.childBinding == expectedTargetBinding
        && lease.parentSourceGeneration == _facts->sourceGeneration
        && lease.childBinding.childRequestedCursor.nextRecord
            == binding.cursor.nextRecord
        && lease.childBinding.childRequestedCursor.nextEvent
            == binding.cursor.nextEvent
        && lease.childBinding.childRequestedCursor.digest
            == binding.cursor.digest
        && sameRoute(activation.targetDataRoute,
                     expectedTargetBinding.childQspRoute)
        && activation.targetQsrRoute.port == _qsrSocket.boundPort()
        && activation.targetQsrRoute.generation == _facts->bindingGeneration
        && _qspFrontierObserved
        && binding.cursor.nextRecord == _qspFrontier.nextRecord
        && binding.cursor.nextEvent == _qspFrontier.nextEvent
        && binding.cursor.digest == _qspFrontier.digest
        && existingParent.valid() && candidateParent.valid()
        && existingParent != candidateParent
        && !binding.dataRoute.sameEndpoint(routeFromWire(_facts->rootQsp))
        && activation.activationDeadlineMs <= _facts->expiryMs;
    if (!exactTarget) return true;

    ClientPlayerRootActivationReceipt receipt{};
    ControlDatagram receiptDatagram{};
    receiptDatagram.type = ControlMessageType::ClientPlayerRootActivationReceipt;
    const bool receiptBuilt = qnetplay::spectator::resilient::wire::
            makeClientPlayerRootActivationReceipt(activation, receipt);
    receiptDatagram.clientPlayerRootActivationReceipt = receipt;
    std::vector<uint8_t> receiptBytes;
    const bool receiptEncoded = receiptBuilt
        && qnetplay::spectator::resilient::wire::encodeControlDatagram(
            receiptDatagram, receiptBytes);
    if (!receiptEncoded) {
        wipeVector(receiptBytes);
        return true;
    }

    _pendingClientPlayerRootActivation.emplace();
    _pendingClientPlayerRootActivation->activation = activation;
    _pendingClientPlayerRootActivation->localDeadlineMs =
        localActivationDeadlineMs;
    const bool queued = _client.setActivationReceiptQsrPayload(
        receiptBytes.data(), receiptBytes.size(), activation.activationNonce,
        nowMs);
    wipeVector(receiptBytes);
    if (!queued) clearPendingClientPlayerRootActivation();
    return true;
}

bool ReleasedTopologyClientRuntime::acceptClientPlayerRootActivationCommit(
    const qnetplay::spectator::resilient::wire::ClientPlayerRootActivationCommit&
        commit,
    const uint64_t nowMs) {
    if (_state != ReleasedTopologyClientState::AwaitingRootOffer
        || !_pendingClientPlayerRootActivation.has_value()
        || !sameClientPlayerRootActivation(
            _pendingClientPlayerRootActivation->activation,
            commit.activation)) {
        return true;
    }
    if (!_assignmentVerifier.ready()
        || !qnetplay::spectator::resilient::wire::
            validClientPlayerRootActivationCommit(commit)) {
        return true;
    }
    std::vector<uint8_t> attestationInput;
    const bool hostAttested = qnetplay::spectator::resilient::wire::
            encodeClientPlayerRootActivationCommitAttestationInput(
                commit, attestationInput)
        && _assignmentVerifier.verify(attestationInput.data(),
                                      attestationInput.size(),
                                      commit.hostSignature);
    wipeVector(attestationInput);
    if (!hostAttested) return true;
    if (_pendingClientPlayerRootActivation->localDeadlineMs == 0u
        || nowMs >= _pendingClientPlayerRootActivation->localDeadlineMs) {
        retirePendingClientPlayerRootActivation();
        return true;
    }
    if (_pendingClientPlayerRootActivation->committed) return true;
    if (!_client.acceptPairedActivationCommit(
            commit.activation.activationNonce, nowMs)) {
        return true;
    }
    _pendingClientPlayerRootActivation->committed = true;
    return true;
}

bool ReleasedTopologyClientRuntime::acceptClientPlayerRootActivationRetirement(
    const qnetplay::spectator::resilient::wire::
        ClientPlayerRootActivationRetirement& retirement,
    const uint64_t ) {
    if (!_assignmentVerifier.ready()
        || !qnetplay::spectator::resilient::wire::
            validClientPlayerRootActivationRetirement(retirement)) {
        return true;
    }
    std::vector<uint8_t> attestationInput;
    const bool hostAttested = qnetplay::spectator::resilient::wire::
            encodeClientPlayerRootActivationRetirementAttestationInput(
                retirement, attestationInput)
        && _assignmentVerifier.verify(attestationInput.data(),
                                      attestationInput.size(),
                                      retirement.hostSignature);
    wipeVector(attestationInput);
    if (!hostAttested) return true;

    if (_state == ReleasedTopologyClientState::AwaitingRootOffer
        && _pendingClientPlayerRootActivation.has_value()
        && sameClientPlayerRootActivation(
            retirement.activation,
            _pendingClientPlayerRootActivation->activation)) {
        retirePendingClientPlayerRootActivation();
    } else if (_state == ReleasedTopologyClientState::Attached
               && _completedClientPlayerRootActivation.has_value()
               && sameClientPlayerRootActivation(
                   retirement.activation,
                   *_completedClientPlayerRootActivation)) {
        failSoft("topology-client-player-root-activation-retired");
    }
    return true;
}

bool ReleasedTopologyClientRuntime::acceptAttachedRelayActivation(
    const qnetplay::spectator::resilient::wire::RelayChildActivation&
        activation,
    const uint64_t nowMs) {
    using qnetplay::spectator::resilient::wire::ControlDatagram;
    using qnetplay::spectator::resilient::wire::ControlMessageType;
    using qnetplay::spectator::resilient::wire::RelayChildActivationReceipt;
    using qnetplay::spectator::resilient::wire::RelayChildActivationReceiptRole;

    if (_state != ReleasedTopologyClientState::Attached || _facts == nullptr
        || _attachedLease == nullptr || !_qsrSocket.isOpen()
        || !_assignmentVerifier.ready() || !_relayDataBindingSnapshot.valid()
        || _relayDataRequestNonce == 0u || !relayAuthorityActive(nowMs)) {
        return true;
    }
    std::vector<uint8_t> attestationInput;
    const bool hostAttested = qnetplay::spectator::resilient::wire::
            encodeRelayChildActivationAttestationInput(
                activation, attestationInput)
        && _assignmentVerifier.verify(attestationInput.data(),
                                      attestationInput.size(),
                                      activation.hostSignature);
    wipeVector(attestationInput);
    if (!hostAttested) return true;

    uint64_t localActivationDeadlineMs = 0u;
    if (!activationLocalDeadline(activation, nowMs, localActivationDeadlineMs))
        return true;
    if (_pendingAttachedRelayActivation.has_value()) {
        return sameRelayChildActivation(
            _pendingAttachedRelayActivation->activation, activation);
    }
    if (_retiredAttachedRelayActivation.has_value()
        && sameRelayChildActivation(
            _retiredAttachedRelayActivation->activation, activation)) {
        return true;
    }

    qnetplay::spectator::resilient::wire::RelayReady ready{};
    if (!makeRelayReady(ready)) return true;
    const model::ParentLease& childLease = activation.childLease;
    const model::ParentLeaseChildBinding& childBinding =
        childLease.childBinding;
    const model::LeaseBinding childLeaseBinding = leaseBindingOf(childLease);
    const bool exactLeaseChain =
        hasExactDerivedRelayChildActivationLeaseChain(
            activation, _facts->limits.maxActiveDepth, _attachedLease.get());
    const bool exact = activation.stream == _attachedLease->stream
        && activation.authorityGeneration == _facts->authorityGeneration
        && activation.rootNodeId == _facts->rootNodeId
        && activation.relayNodeId == _attachedLease->childId
        && activation.childNodeId != model::kInvalidNodeId
        && activation.childNodeId != activation.rootNodeId
        && activation.childNodeId != activation.relayNodeId
        && sameLeaseBinding(activation.relayLease, *_attachedLease)
        && activation.relayBindingRequestNonce == _relayDataRequestNonce
        && sameRoute(activation.relayDataRoute, ready.observedDataRoute)
        && sameRoute(activation.relayControlRoute,
                     ready.observedControlRoute)
        && activation.relayCacheRange.firstCursor
            == ready.cacheRange.firstCursor
        && activation.relayCacheRange.nextCursor == ready.cacheRange.nextCursor
        && activation.commonCursor.nextRecord == ready.cacheCursor.nextRecord
        && activation.commonCursor.nextEvent == ready.cacheCursor.nextEvent
        && activation.commonCursor.digest == ready.cacheCursor.digest
        && exactLeaseChain
        && childLease.stream == _attachedLease->stream
        && childLease.childId == activation.childNodeId
        && childLease.parentId == activation.relayNodeId
        && childLease.parentSourceGeneration
            == _attachedLease->parentSourceGeneration
        && childLease.routeGeneration == ready.observedDataRoute.generation
        && childLease.routeCount == 1u
        && sameRoute(childLease.routes[0], ready.observedDataRoute)
        && childLease.cacheRange.firstCursor
            == activation.relayCacheRange.firstCursor
        && childLease.cacheRange.nextCursor
            == activation.relayCacheRange.nextCursor
        && childLease.latestVerifiedCursor == activation.commonCursor.nextRecord
        && childLease.replayNonce != 0u && childLease.expiryMs != 0u
        && childBinding.valid()
        && childBinding.authorityGeneration == _facts->authorityGeneration
        && sameRoute(activation.childDataRoute,
                     childBinding.childQspRoute)
        && activation.childDataRoute.valid()
        && activation.childDataRoute.routeProved
        && activation.childControlRoute.valid()
        && activation.childControlRoute.routeProved
        && activation.childControlRoute.kind == model::RouteKind::Direct
        && activation.childControlRoute.generation
            == childBinding.childEndpointBindingGeneration
        && !activation.childControlRoute.sameEndpoint(
            activation.childDataRoute)
        && childLeaseBinding.childId == activation.childNodeId
        && childLeaseBinding.parentId == activation.relayNodeId
        && childLeaseBinding.childBinding == childBinding
        && activation.activationNonce != 0u
        && activation.activationDeadlineMs != 0u;
    if (!exact) return true;

    pruneRetiredRelayChildren();
    const size_t maximumChildren = static_cast<size_t>(
        _facts->limits.maxChildrenPerRelay);
    const qnetplay::spectator::EndpointId endpointToken =
        static_cast<qnetplay::spectator::EndpointId>(
            childBinding.childViewerId);
    if (maximumChildren == 0u || _relayChildren.size() >= maximumChildren
        || endpointToken == 0u) {
        return true;
    }
    const auto duplicate = std::find_if(
        _relayChildren.begin(), _relayChildren.end(),
        [&childLease, &childBinding, endpointToken](
            const RelayChildBinding& existing) {
            return existing.childNodeId == childLease.childId
                || existing.replayNonce == childLease.replayNonce
                || existing.endpointToken == endpointToken
                || (existing.route.ipv4BE == childBinding.childQspRoute.ipv4BE
                    && existing.route.port == childBinding.childQspRoute.port);
        });
    if (duplicate != _relayChildren.end()) return true;

    RelayChildActivationReceipt receipt{};
    ControlDatagram datagram{};
    datagram.type = ControlMessageType::RelayChildActivationReceipt;
    const bool receiptBuilt = qnetplay::spectator::resilient::wire::
            makeRelayChildActivationReceipt(
                activation, RelayChildActivationReceiptRole::Relay, receipt);
    datagram.relayChildActivationReceipt = receipt;
    std::vector<uint8_t> receiptBytes;
    const bool receiptEncoded = receiptBuilt
        && qnetplay::spectator::resilient::wire::encodeControlDatagram(
            datagram, receiptBytes);
    if (!receiptEncoded) {
        wipeVector(receiptBytes);
        return true;
    }

    _pendingAttachedRelayActivation.emplace();
    _pendingAttachedRelayActivation->activation = activation;
    _pendingAttachedRelayActivation->localDeadlineMs = localActivationDeadlineMs;
    const bool queued = _client.setAttachedRelayActivationReceiptQsrPayload(
        receiptBytes.data(), receiptBytes.size(), activation.activationNonce,
        nowMs);
    wipeVector(receiptBytes);
    if (!queued) clearPendingAttachedRelayActivation();
    return true;
}

bool ReleasedTopologyClientRuntime::acceptRelayChildActivationCommit(
    const qnetplay::spectator::resilient::wire::RelayChildActivationCommit&
        commit,
    const uint64_t nowMs) {
    using qclient::spectator::resilient::NodeRuntimeResult;
    using qclient::spectator::resilient::QtaVerifiedParentLease;

    const auto hostCommitAttested = [this, &commit]() {
        if (!_assignmentVerifier.ready()
            || !qnetplay::spectator::resilient::wire::
                validRelayChildActivationCommit(commit)) {
            return false;
        }
        std::vector<uint8_t> attestationInput;
        const bool attested = qnetplay::spectator::resilient::wire::
                encodeRelayChildActivationCommitAttestationInput(
                    commit, attestationInput)
            && _assignmentVerifier.verify(attestationInput.data(),
                                          attestationInput.size(),
                                          commit.hostSignature);
        wipeVector(attestationInput);
        return attested;
    };

    if (_state == ReleasedTopologyClientState::AwaitingRootOffer) {
        if (!_pendingRelayChildActivation.has_value()
            || !sameRelayChildActivation(
                _pendingRelayChildActivation->activation,
                commit.activation)) {
            return true;
        }
        if (!hostCommitAttested()) return true;
        if (_pendingRelayChildActivation->localDeadlineMs == 0u
            || nowMs >= _pendingRelayChildActivation->localDeadlineMs) {
            retirePendingRelayChildActivation();
            return true;
        }
        if (!_client.acceptPairedActivationCommit(
                commit.activation.activationNonce, nowMs)) {
            return true;
        }
        _pendingRelayChildActivation->committed = true;
        return true;
    }

    if (_state != ReleasedTopologyClientState::Attached
        || !_pendingAttachedRelayActivation.has_value()
        || !sameRelayChildActivation(
            _pendingAttachedRelayActivation->activation,
            commit.activation)) {
        return true;
    }
    if (!hostCommitAttested()) return true;
    if (_pendingAttachedRelayActivation->localDeadlineMs == 0u
        || nowMs >= _pendingAttachedRelayActivation->localDeadlineMs) {
        retireCommittedAttachedRelayChild(nowMs);
        retirePendingAttachedRelayActivation();
        return true;
    }
    if (_pendingAttachedRelayActivation->committed) return true;

    const auto& activation = _pendingAttachedRelayActivation->activation;
    if (_facts == nullptr || _attachedLease == nullptr
        || _qtaAuthority == nullptr || _relayNode == nullptr
        || _relayCanonicalIngress == nullptr || !_relayNode->running()
        || !_relayDataBindingSnapshot.valid()
        || !relayAuthorityActive(nowMs)
        || activation.stream != _attachedLease->stream
        || activation.authorityGeneration != _facts->authorityGeneration
        || activation.rootNodeId != _facts->rootNodeId
        || activation.relayNodeId != _attachedLease->childId
        || !sameLeaseBinding(activation.relayLease, *_attachedLease)
        || activation.relayBindingRequestNonce == 0u
        || activation.relayBindingRequestNonce != _relayDataRequestNonce
        || !sameRoute(activation.relayDataRoute,
                      _relayDataBindingSnapshot.observedDataRoute)
        || !sameRoute(activation.relayControlRoute,
                      _relayDataBindingSnapshot.observedControlRoute)) {
        return true;
    }

    const model::ParentLease& childLease = activation.childLease;
    const model::ParentLeaseChildBinding& childBinding = childLease.childBinding;
    const size_t maximumChildren = static_cast<size_t>(
        _facts->limits.maxChildrenPerRelay);
    if (childLease.childId != activation.childNodeId
        || childLease.parentId != activation.relayNodeId
        || !childBinding.valid() || maximumChildren == 0u) {
        return true;
    }
    const qnetplay::spectator::EndpointId endpointToken =
        static_cast<qnetplay::spectator::EndpointId>(
            childBinding.childViewerId);
    if (endpointToken == 0u || !childBinding.childQspRoute.valid()
        || !childBinding.childQspRoute.routeProved
        || childBinding.childQspRoute.sameEndpoint(
            _relayDataBindingSnapshot.observedDataRoute)) {
        return true;
    }

    pruneRetiredRelayChildren();
    if (_relayChildren.size() >= maximumChildren) return true;
    const auto duplicate = std::find_if(
        _relayChildren.begin(), _relayChildren.end(),
        [&childLease, &childBinding, endpointToken](
            const RelayChildBinding& existing) {
            return existing.childNodeId == childLease.childId
                || existing.replayNonce == childLease.replayNonce
                || existing.endpointToken == endpointToken
                || (existing.route.ipv4BE == childBinding.childQspRoute.ipv4BE
                    && existing.route.port == childBinding.childQspRoute.port);
        });
    if (duplicate != _relayChildren.end()) return true;

    try {
        _relayChildren.reserve(_relayChildren.size() + 1u);
    } catch (const std::bad_alloc&) {
        return true;
    }
    QtaVerifiedParentLease verified(childLease, _qtaAuthority);
    std::optional<qclient::spectator::resilient::QtaVerifiedChildRetirement>
        retirementCapability;
    if (_relayNode->registerQtaVerifiedChild(std::move(verified), nowMs,
                                             retirementCapability)
            != NodeRuntimeResult::Accepted
        || !retirementCapability.has_value()) {
        return true;
    }
    RelayChildBinding installed{};
    installed.childNodeId = childLease.childId;
    installed.route = childBinding.childQspRoute;
    installed.endpointBindingGeneration =
        childBinding.childEndpointBindingGeneration;
    installed.endpointToken = endpointToken;
    installed.replayNonce = childLease.replayNonce;
    installed.childLease = leaseBindingOf(childLease);
    installed.retirement = std::move(retirementCapability);
    _relayChildren.push_back(std::move(installed));
    _pendingAttachedRelayActivation->committed = true;
    return true;
}

bool ReleasedTopologyClientRuntime::acceptRelayChildAssignment(
    const qnetplay::spectator::resilient::wire::RelayChildAssignment&
        assignment,
    const uint64_t nowMs) {
    using qclient::spectator::resilient::NodeRuntimeResult;
    using qclient::spectator::resilient::QtaVerifiedParentLease;

    if (_state != ReleasedTopologyClientState::Attached || _facts == nullptr
        || _attachedLease == nullptr || _qtaAuthority == nullptr
        || _relayNode == nullptr || _relayCanonicalIngress == nullptr
        || !_relayNode->running() || !_relayDataBindingSnapshot.valid()
        || !_assignmentVerifier.ready() || !relayAuthorityActive(nowMs)) {
        return true;
    }
    std::vector<uint8_t> attestationInput;
    const bool hostAttested = qnetplay::spectator::resilient::wire::
            encodeRelayChildAssignmentAttestationInput(
                assignment, attestationInput)
        && _assignmentVerifier.verify(attestationInput.data(),
                                      attestationInput.size(),
                                      assignment.hostSignature);
    wipeVector(attestationInput);
    if (!hostAttested) return true;
    const model::ParentLease& childLease = assignment.childLease;
    const model::ParentLeaseChildBinding& childBinding =
        childLease.childBinding;
    const size_t maximumChildren = static_cast<size_t>(
        _facts->limits.maxChildrenPerRelay);
    pruneRetiredRelayChildren();
    if (assignment.stream != _attachedLease->stream
        || assignment.authorityGeneration != _facts->authorityGeneration
        || assignment.rootNodeId != _facts->rootNodeId
        || assignment.relayNodeId != _attachedLease->childId
        || assignment.childNodeId == model::kInvalidNodeId
        || assignment.childNodeId == assignment.relayNodeId
        || !sameLeaseBinding(assignment.relayLease, *_attachedLease)
        || assignment.bindingRequestNonce == 0u
        || assignment.bindingRequestNonce != _relayDataRequestNonce
        || !sameRoute(assignment.observedDataRoute,
                      _relayDataBindingSnapshot.observedDataRoute)
        || !sameRoute(assignment.observedControlRoute,
                      _relayDataBindingSnapshot.observedControlRoute)
        || !hasExactDerivedChildLeaseChain(
            assignment.stream, assignment.authorityGeneration,
            assignment.rootNodeId, assignment.relayNodeId,
            assignment.childNodeId, assignment.relayLease, childLease,
            assignment.observedDataRoute, childBinding.childQspRoute,
            _facts->limits.maxActiveDepth, _attachedLease.get())
        || childLease.stream != _attachedLease->stream
        || childLease.childId != assignment.childNodeId
        || childLease.parentId != assignment.relayNodeId
        || childLease.parentSourceGeneration
            != _attachedLease->parentSourceGeneration
        || childLease.routeGeneration
            != _relayDataBindingSnapshot.observedDataRoute.generation
        || childLease.routeCount != 1u
        || !sameRoute(childLease.routes[0],
                      _relayDataBindingSnapshot.observedDataRoute)
        || !childBinding.valid()
        || childBinding.authorityGeneration != _facts->authorityGeneration
        || childLease.replayNonce == 0u || childLease.expiryMs == 0u
        || maximumChildren == 0u || _relayChildren.size() >= maximumChildren) {
        return true;
    }

    const qnetplay::spectator::EndpointId endpointToken =
        static_cast<qnetplay::spectator::EndpointId>(
            childBinding.childViewerId);
    if (endpointToken == 0u
        || !childBinding.childQspRoute.valid()
        || !childBinding.childQspRoute.routeProved
        || childBinding.childQspRoute.sameEndpoint(
            _relayDataBindingSnapshot.observedDataRoute)) {
        return true;
    }
    const auto duplicate = std::find_if(
        _relayChildren.begin(), _relayChildren.end(),
        [&childLease, &childBinding, endpointToken](
            const RelayChildBinding& existing) {
            return existing.childNodeId == childLease.childId
                || existing.replayNonce == childLease.replayNonce
                || existing.endpointToken == endpointToken
                || (existing.route.ipv4BE == childBinding.childQspRoute.ipv4BE
                    && existing.route.port == childBinding.childQspRoute.port);
        });
    if (duplicate != _relayChildren.end()) return true;

    try {
        _relayChildren.reserve(_relayChildren.size() + 1u);
    } catch (const std::bad_alloc&) {
        return true;
    }
    QtaVerifiedParentLease verified(childLease, _qtaAuthority);
    std::optional<qclient::spectator::resilient::QtaVerifiedChildRetirement>
        retirementCapability;
    if (_relayNode->registerQtaVerifiedChild(std::move(verified), nowMs,
                                             retirementCapability)
            != NodeRuntimeResult::Accepted
        || !retirementCapability.has_value()) {
        return true;
    }
    RelayChildBinding installed{};
    installed.childNodeId = childLease.childId;
    installed.route = childBinding.childQspRoute;
    installed.endpointBindingGeneration =
        childBinding.childEndpointBindingGeneration;
    installed.endpointToken = endpointToken;
    installed.replayNonce = childLease.replayNonce;
    installed.childLease = leaseBindingOf(childLease);
    installed.retirement = std::move(retirementCapability);
    _relayChildren.push_back(std::move(installed));
    return true;
}

bool ReleasedTopologyClientRuntime::acceptRelayChildRetirement(
    const qnetplay::spectator::resilient::wire::RelayChildRetirement&
        retirement,
    const uint64_t nowMs) {
    using qclient::spectator::resilient::NodeRuntimeResult;

    if (_state != ReleasedTopologyClientState::Attached || _facts == nullptr
        || _attachedLease == nullptr || _relayNode == nullptr
        || !_relayNode->running() || !_relayDataBindingSnapshot.valid()) {
        return true;
    }
    pruneRetiredRelayChildren();
    if (retirement.stream != _attachedLease->stream
        || retirement.authorityGeneration != _facts->authorityGeneration
        || retirement.rootNodeId != _facts->rootNodeId
        || retirement.relayNodeId != _attachedLease->childId
        || retirement.childNodeId == model::kInvalidNodeId
        || retirement.childNodeId == retirement.relayNodeId
        || !sameLeaseBinding(retirement.relayLease, *_attachedLease)
        || retirement.bindingRequestNonce == 0u
        || retirement.bindingRequestNonce != _relayDataRequestNonce) {
        return true;
    }
    const auto found = std::find_if(
        _relayChildren.begin(), _relayChildren.end(),
        [&retirement](const RelayChildBinding& candidate) {
            return candidate.childNodeId == retirement.childNodeId;
        });
    if (found == _relayChildren.end()
        || !sameLeaseBinding(retirement.childLease, found->childLease)
        || found->replayNonce != retirement.childLease.replayNonce
        || !found->retirement.has_value()) {
        return true;
    }

    const NodeRuntimeResult result = _relayNode->retireQtaVerifiedChild(
        std::move(*found->retirement), nowMs);
    if (result == NodeRuntimeResult::Accepted) {
        _relayChildren.erase(found);
    } else {
        pruneRetiredRelayChildren();
    }
    return true;
}

bool ReleasedTopologyClientRuntime::acceptQsrControl(const uint8_t* bytes,
                                                      const size_t length,
                                                      const uint64_t nowMs) {
    if (_facts == nullptr) return false;
    TranscriptBuilder transcript(*_facts);
    QtaEnvelope envelope{};
    const QtaIngressResult ingress = _client.acceptQtaEnvelope(
        bytes, length, transcript, envelope);
    if (ingress == QtaIngressResult::ReplayRejected) {
        wipeVector(envelope.qsrPayload);
        return true;
    }
    if (ingress != QtaIngressResult::Accepted) {
        wipeVector(envelope.qsrPayload);
        return _state == ReleasedTopologyClientState::Attached;
    }
    qnetplay::spectator::resilient::wire::ControlDatagram datagram{};
    const bool pairedActivationControl = hasPairedActivationControlType(
        envelope.qsrPayload.data(), envelope.qsrPayload.size());
    const auto decoded = qnetplay::spectator::resilient::wire::decodeControlDatagram(
        envelope.qsrPayload.data(), envelope.qsrPayload.size(), datagram);
    wipeVector(envelope.qsrPayload);
    if (decoded != qnetplay::spectator::resilient::wire::DecodeResult::Ok) {
        return _state == ReleasedTopologyClientState::Attached
            || pairedActivationControl;
    }
    switch (datagram.type) {
    case qnetplay::spectator::resilient::wire::ControlMessageType::ParentLease:
        if (_state == ReleasedTopologyClientState::AwaitingRootOffer)
            return acceptRootOffer(datagram.parentLease, nowMs);
        return _state == ReleasedTopologyClientState::AwaitingReceipt
            && _offeredLease != nullptr
            && sameParentLease(datagram.parentLease, *_offeredLease);
    case qnetplay::spectator::resilient::wire::ControlMessageType::ParentCommit:
        return _state == ReleasedTopologyClientState::AwaitingReceipt
            && acceptParentCommit(datagram.parentCommit, nowMs);
    case qnetplay::spectator::resilient::wire::ControlMessageType::AuthorityState:
        return acceptAuthorityState(datagram.authorityState, nowMs);
    case qnetplay::spectator::resilient::wire::ControlMessageType::
        RelayDataBindingAck:
        return _state == ReleasedTopologyClientState::Attached
            && (!_relayDataBindingPending
                || acceptRelayDataBindingAck(datagram.relayDataBindingAck,
                                              nowMs));
    case qnetplay::spectator::resilient::wire::ControlMessageType::
        RelayChildAssignment:
        return _state == ReleasedTopologyClientState::Attached
            && acceptRelayChildAssignment(datagram.relayChildAssignment,
                                          nowMs);
    case qnetplay::spectator::resilient::wire::ControlMessageType::
        RelayChildRetirement:
        return _state == ReleasedTopologyClientState::Attached
            && acceptRelayChildRetirement(datagram.relayChildRetirement,
                                          nowMs);
    case qnetplay::spectator::resilient::wire::ControlMessageType::
        RelayChildActivation:
        if (_state == ReleasedTopologyClientState::AwaitingRootOffer) {
            return acceptRelayChildActivation(datagram.relayChildActivation,
                                              nowMs);
        }
        if (_state == ReleasedTopologyClientState::Attached) {
            return acceptAttachedRelayActivation(datagram.relayChildActivation,
                                                 nowMs);
        }
        return true;
    case qnetplay::spectator::resilient::wire::ControlMessageType::
        RelayChildActivationRetirement:
        return acceptRelayChildActivationRetirement(
            datagram.relayChildActivationRetirement, nowMs);
    case qnetplay::spectator::resilient::wire::ControlMessageType::
        RelayChildActivationCommit:
        return acceptRelayChildActivationCommit(
            datagram.relayChildActivationCommit, nowMs);
    case qnetplay::spectator::resilient::wire::ControlMessageType::
        ClientPlayerRootActivation:
        return acceptClientPlayerRootActivation(
            datagram.clientPlayerRootActivation, nowMs);
    case qnetplay::spectator::resilient::wire::ControlMessageType::
        ClientPlayerRootActivationCommit:
        return acceptClientPlayerRootActivationCommit(
            datagram.clientPlayerRootActivationCommit, nowMs);
    case qnetplay::spectator::resilient::wire::ControlMessageType::
        ClientPlayerRootActivationRetirement:
        return acceptClientPlayerRootActivationRetirement(
            datagram.clientPlayerRootActivationRetirement, nowMs);
    default:
        return _state == ReleasedTopologyClientState::Attached;
    }
}

void ReleasedTopologyClientRuntime::serviceQsr(const uint64_t nowMs) {
    if ((_state != ReleasedTopologyClientState::AwaitingRootOffer
         && _state != ReleasedTopologyClientState::AwaitingReceipt
         && _state != ReleasedTopologyClientState::Attached)
        || _facts == nullptr || !_qsrSocket.isOpen()) {
        return;
    }
    const model::RouteDescriptor hostRoute = routeFromWire(_facts->rootQsr);
    const uint32_t expectedIpv4 = _relayQsrActive
        ? static_cast<uint32_t>(_relayQsrEndpoint.sa.sin_addr.s_addr)
        : hostRoute.ipv4BE;
    const uint16_t expectedPort = _relayQsrActive
        ? ntohs(_relayQsrEndpoint.sa.sin_port)
        : hostRoute.port;
    std::array<uint8_t, qnet::relay::kTopologyFrameHeaderSize
                            + kTopologyEnvelopePrefixBytes
                            + qnetplay::spectator::resilient::wire::kMaxResilientControlDatagramBytes>
        buffer{};
    std::vector<uint8_t> unwrapped;
    for (size_t budget = 0u; budget < 32u; ++budget) {
        qnet::Endpoint from{};
        const int received = _qsrSocket.recvFrom(buffer.data(), buffer.size(), from);
        if (received == 0) break;
        if (received < 0) {
            failSoft("topology-qsr-receive-failed");
            return;
        }
        if (from.sa.sin_addr.s_addr != expectedIpv4
            || ntohs(from.sa.sin_port) != expectedPort) {
            continue;
        }
        const uint8_t* payload = buffer.data();
        size_t payloadLength = static_cast<size_t>(received);
        if (_relayQsrActive) {
            if (acceptRelayQsrWelcome(buffer.data(),
                                      static_cast<size_t>(received), nowMs)) {
                if (_state == ReleasedTopologyClientState::Unavailable
                    || _state == ReleasedTopologyClientState::Disabled) {
                    return;
                }
                continue;
            }
            const qnetplay::spectator::TopologyRelayIngress ingress =
                _relayQsrAdapter.decodeInbound(
                    true, buffer.data(), static_cast<size_t>(received),
                    unwrapped);
            if (ingress
                == qnetplay::spectator::TopologyRelayIngress::Detached) {
                failSoft("topology-relay-qsr-detached");
                return;
            }
            if (ingress
                != qnetplay::spectator::TopologyRelayIngress::Data) {
                continue;
            }
            payload = unwrapped.data();
            payloadLength = unwrapped.size();
        }
        if (!acceptQsrControl(payload, payloadLength, nowMs)) {
            failSoft("topology-qsr-control-rejected");
            return;
        }
        if (_state == ReleasedTopologyClientState::Unavailable
            || _state == ReleasedTopologyClientState::Disabled) {
            return;
        }
    }
}

void ReleasedTopologyClientRuntime::service(const uint64_t nowMs) {
    if (_credentialDeadlineMs != 0u && nowMs >= _credentialDeadlineMs
        && _state != ReleasedTopologyClientState::Disabled
        && _state != ReleasedTopologyClientState::Unavailable) {
        failSoft("topology-credential-expired");
        return;
    }
    servicePairedActivations(nowMs);
    serviceControl(nowMs);
    serviceRelayQsrAdmission(nowMs);
    if (_state == ReleasedTopologyClientState::Unavailable
        || _state == ReleasedTopologyClientState::Disabled) {
        return;
    }
    if (_state == ReleasedTopologyClientState::Attached) {
        if (_livenessDeadlineMs != 0u && nowMs >= _livenessDeadlineMs) {
            failSoft("topology-authority-liveness-timed-out");
            return;
        }
        serviceActions(nowMs);
        if (_state == ReleasedTopologyClientState::Unavailable
            || _state == ReleasedTopologyClientState::Disabled) {
            return;
        }
        serviceQsr(nowMs);
        if (_state == ReleasedTopologyClientState::Unavailable
            || _state == ReleasedTopologyClientState::Disabled) {
            return;
        }
        serviceRelayDataBinding(nowMs);
        if (_state == ReleasedTopologyClientState::Attached)
            serviceRelayNode(nowMs);
        return;
    }
    serviceActions(nowMs);
    if (_state == ReleasedTopologyClientState::Unavailable) return;
    serviceQsr(nowMs);
}

void ReleasedTopologyClientRuntime::clearSensitiveState() {
    clearPendingRelayChildActivation();
    clearPendingClientPlayerRootActivation();
    clearPendingAttachedRelayActivation();
    _client.stop();
    _controlLink.close();
    _controlSocket.close();
    _qsrSocket.close();
    _relayQsrAdapter.reset();
    _relayQsrActive = false;
    _relayQsrEndpoint = {};
    _nextRelayQsrHelloMs = 0u;
    wipeBytes(&_relayQsrOffer, sizeof(_relayQsrOffer));
    _relayQsrOffer = {};
    if (_retiredRelayChildActivation.has_value()) {
        wipeBytes(&_retiredRelayChildActivation->activation,
                  sizeof(_retiredRelayChildActivation->activation));
        _retiredRelayChildActivation.reset();
    }
    if (_retiredClientPlayerRootActivation.has_value()) {
        wipeBytes(&_retiredClientPlayerRootActivation->activation,
                  sizeof(_retiredClientPlayerRootActivation->activation));
        _retiredClientPlayerRootActivation.reset();
    }
    if (_completedParentRouteActivation.has_value()) {
        wipeBytes(&*_completedParentRouteActivation,
                  sizeof(*_completedParentRouteActivation));
        _completedParentRouteActivation.reset();
    }
    if (_completedClientPlayerRootActivation.has_value()) {
        wipeBytes(&*_completedClientPlayerRootActivation,
                  sizeof(*_completedClientPlayerRootActivation));
        _completedClientPlayerRootActivation.reset();
    }
    if (_retiredAttachedRelayActivation.has_value()) {
        wipeBytes(&_retiredAttachedRelayActivation->activation,
                  sizeof(_retiredAttachedRelayActivation->activation));
        _retiredAttachedRelayActivation.reset();
    }
    clearRelayDataBinding();
    _assignmentVerifier.reset();
    wipeBytes(&_grant, sizeof(_grant));
    if (_facts) {
        wipeBytes(_facts.get(), sizeof(*_facts));
        _facts.reset();
    }
    if (_offeredLease) {
        wipeBytes(_offeredLease.get(), sizeof(*_offeredLease));
        _offeredLease.reset();
    }
    if (_attachedLease) {
        wipeBytes(_attachedLease.get(), sizeof(*_attachedLease));
        _attachedLease.reset();
    }
    if (_qtaAuthority) {
        _qtaAuthority->revoke();
        _qtaAuthority.reset();
    }
    _attachmentSnapshot = {};
    _sessionId = 0u;
    _sessionEpoch = 0u;
    _viewerId = 0u;
    _viewerGeneration = 0u;
    _correlation = 0u;
    _spectatorCookieLow = 0u;
    _spectatorCookieHigh = 0u;
    _streamRootIdentity = 0u;
    _qspRouteGeneration = 0u;
    wipeBytes(_peerChallenge, sizeof(_peerChallenge));
    _controlDeadlineMs = 0u;
    _responseDeadlineMs = 0u;
    _controlRestartCount = 0u;
    _credentialDeadlineMs = 0u;
    _livenessDeadlineMs = 0u;
    _lastAuthorityStateSequence = 0u;
    _offerDeadlineMs = 0u;
    _qspFrontier = {};
    _qspFrontierObserved = false;
    _qspRoute = qnet::SPECTATE_ROUTE_NONE;
    _qspProvenance = EndpointProvenance::Unknown;
    _qspEndpoint = {};
    _rootQspEndpoint = {};
    _hostControl = {};
    _compatibility = {};
}

void ReleasedTopologyClientRuntime::failSoft(const char* const reason) {
    _lastTranscriptFailure = _client.failure();
    clearSensitiveState();
    _state = ReleasedTopologyClientState::Unavailable;
    _reason = reason != nullptr ? reason : "topology-unavailable";
}

void ReleasedTopologyClientRuntime::stop() {
    clearSensitiveState();
    _lastTranscriptFailure = ClientFailure::None;
    _state = ReleasedTopologyClientState::Disabled;
    _reason = "disabled";
}

ReleasedTopologyClientStatus ReleasedTopologyClientRuntime::status() const {
    ReleasedTopologyClientStatus out{};
    out.state = _state;
    out.transcriptState = _client.state();
    out.transcriptFailure = _lastTranscriptFailure;
    out.reason = _reason.c_str();
    return out;
}

bool ReleasedTopologyClientRuntime::attachmentSnapshot(
    ReleasedTopologyAttachmentSnapshot& out) const {
    out = {};
    if (_state != ReleasedTopologyClientState::Attached
        || !_attachmentSnapshot.valid()) {
        return false;
    }
    out = _attachmentSnapshot;
    return true;
}

bool ReleasedTopologyClientRuntime::parentRouteActivation(
    ReleasedTopologyParentRouteActivation& out, const uint64_t nowMs) const {
    out = {};
    if (_state == ReleasedTopologyClientState::AwaitingRootOffer
        && _facts != nullptr
        && _pendingClientPlayerRootActivation.has_value()
        && _pendingClientPlayerRootActivation->committed
        && _pendingClientPlayerRootActivation->localDeadlineMs != 0u
        && nowMs < _pendingClientPlayerRootActivation->localDeadlineMs) {
        const auto& activation =
            _pendingClientPlayerRootActivation->activation;
        const RouteEndpoint activeParent{
            _facts->rootQsp.advertisedIpv4BE,
            _facts->rootQsp.advertisedPort};
        const RouteEndpoint candidate{activation.binding.dataRoute.ipv4BE,
                                      activation.binding.dataRoute.port};
        if (activeParent.valid() && candidate.valid()
            && activeParent != candidate
            && hasExactClientPlayerRootActivationLeaseChain(activation)
            && streamEquals(activation.binding.stream,
                            _facts->compatibility, _facts->sessionId,
                            _facts->sessionEpoch, _facts->streamRootIdentity)
            && activation.authorityGeneration == _facts->authorityGeneration
            && activation.hostRootNodeId == _facts->rootNodeId
            && activation.targetViewerNodeId == _facts->childNodeId
            && activation.targetLease.parentSourceGeneration
                == _facts->sourceGeneration
            && activation.targetLease.routeGeneration
                == activation.binding.dataRoute.generation) {
            out.currentParentEndpoint = activeParent;
            out.candidateDataEndpoint = candidate;
            out.sourceGeneration =
                activation.targetLease.parentSourceGeneration;
            out.routeGeneration = activation.targetLease.routeGeneration;
            out.commonCursor = activation.binding.cursor;
            out.localDeadlineMs =
                _pendingClientPlayerRootActivation->localDeadlineMs;
            out.activationNonce = activation.activationNonce;
            if (out.valid()) return true;
            out = {};
        }
        return false;
    }
    if (_state != ReleasedTopologyClientState::AwaitingRootOffer
        || _facts == nullptr || !_pendingRelayChildActivation.has_value()
        || !_pendingRelayChildActivation->committed
        || _pendingRelayChildActivation->localDeadlineMs == 0u
        || nowMs >= _pendingRelayChildActivation->localDeadlineMs) {
        return false;
    }
    const qnetplay::spectator::resilient::wire::RelayChildActivation&
        activation = _pendingRelayChildActivation->activation;
    const RouteEndpoint activeParent{
        _facts->rootQsp.advertisedIpv4BE, _facts->rootQsp.advertisedPort};
    const RouteEndpoint candidate{
        activation.relayDataRoute.ipv4BE, activation.relayDataRoute.port};
    if (!activeParent.valid() || !candidate.valid()
        || activeParent == candidate
        || activation.authorityGeneration != _facts->authorityGeneration
        || activation.rootNodeId != _facts->rootNodeId
        || activation.childNodeId != _facts->childNodeId
        || activation.childLease.parentSourceGeneration
            != _facts->sourceGeneration
        || activation.childLease.routeGeneration
            != activation.relayDataRoute.generation) {
        return false;
    }
    out.currentParentEndpoint = activeParent;
    out.candidateDataEndpoint = candidate;
    out.sourceGeneration = activation.childLease.parentSourceGeneration;
    out.routeGeneration = activation.childLease.routeGeneration;
    out.commonCursor = activation.commonCursor;
    out.localDeadlineMs = _pendingRelayChildActivation->localDeadlineMs;
    out.activationNonce = activation.activationNonce;
    if (!out.valid()) {
        out = {};
        return false;
    }
    return true;
}

bool ReleasedTopologyClientRuntime::completeParentRouteActivationAfterIngress(
    const uint64_t activationNonce, const RouteEndpoint& acceptedParent,
    const uint32_t nextRecord, const uint32_t nextEvent, const uint64_t digest,
    const uint64_t nowMs) {
    if (_state == ReleasedTopologyClientState::AwaitingRootOffer
        && _facts != nullptr
        && _pendingClientPlayerRootActivation.has_value()
        && _pendingClientPlayerRootActivation->committed
        && activationNonce != 0u
        && activationNonce
            == _pendingClientPlayerRootActivation->activation.activationNonce
        && _pendingClientPlayerRootActivation->localDeadlineMs != 0u
        && nowMs < _pendingClientPlayerRootActivation->localDeadlineMs
        && _qspFrontierObserved) {
        const auto& activation =
            _pendingClientPlayerRootActivation->activation;
        const auto& binding = activation.binding;
        const model::ParentLease& lease = activation.targetLease;
        const RouteEndpoint expectedParent{binding.dataRoute.ipv4BE,
                                           binding.dataRoute.port};
        const RouteEndpoint originalParent{
            _facts->rootQsp.advertisedIpv4BE,
            _facts->rootQsp.advertisedPort};

        model::ParentLeaseChildBinding expectedTargetBinding{};
        const bool qspDirect = _facts->qspProvenance
            == static_cast<uint8_t>(EndpointProvenance::ProvenDirectQsp);
        const bool qspPunched = _facts->qspProvenance
            == static_cast<uint8_t>(EndpointProvenance::ProvenPunchedQsp);
        if (qspDirect || qspPunched) {
            expectedTargetBinding.childQspRoute.kind = qspPunched
                ? model::RouteKind::Punched : model::RouteKind::Direct;
            expectedTargetBinding.childQspRoute.ipv4BE = _facts->qspIpv4BE;
            expectedTargetBinding.childQspRoute.port = _facts->qspPort;
            expectedTargetBinding.childQspRoute.generation =
                _facts->qspRouteGeneration;
            expectedTargetBinding.childQspRoute.routeProved = true;
        }
        expectedTargetBinding.authorityGeneration =
            _facts->authorityGeneration;
        expectedTargetBinding.childViewerId = _facts->viewerId;
        expectedTargetBinding.childViewerGeneration = _facts->viewerGeneration;
        expectedTargetBinding.childEndpointBindingGeneration =
            _facts->bindingGeneration;
        expectedTargetBinding.childRequestedCursor = {
            _facts->cursor.nextRecord, _facts->cursor.nextEvent,
            _facts->cursor.digest};

        if (!expectedParent.valid() || !originalParent.valid()
            || expectedParent == originalParent
            || acceptedParent != expectedParent
            || !expectedTargetBinding.valid()
            || !hasExactClientPlayerRootActivationLeaseChain(activation)
            || !streamEquals(binding.stream, _facts->compatibility,
                             _facts->sessionId, _facts->sessionEpoch,
                             _facts->streamRootIdentity)
            || binding.gameStartFingerprint != _facts->streamRootIdentity
            || activation.authorityGeneration != _facts->authorityGeneration
            || activation.hostRootNodeId != _facts->rootNodeId
            || activation.targetViewerNodeId != _facts->childNodeId
            || binding.sourceGeneration != _facts->sourceGeneration
            || binding.cursor.nextRecord != _facts->cursor.nextRecord
            || binding.cursor.nextEvent != _facts->cursor.nextEvent
            || binding.cursor.digest != _facts->cursor.digest
            || lease.childBinding != expectedTargetBinding
            || lease.parentSourceGeneration != _facts->sourceGeneration
            || lease.routeGeneration != binding.dataRoute.generation
            || lease.routeCount != 1u
            || !sameRoute(lease.routes[0], binding.dataRoute)
            || lease.childId != _facts->childNodeId
            || lease.parentId != binding.clientRootNodeId
            || lease.latestVerifiedCursor != binding.cursor.nextRecord
            || !sameRoute(activation.targetDataRoute,
                          expectedTargetBinding.childQspRoute)
            || activation.targetQsrRoute.port != _qsrSocket.boundPort()
            || activation.targetQsrRoute.generation != _facts->bindingGeneration
            || binding.cursor.nextRecord != _qspFrontier.nextRecord
            || binding.cursor.nextEvent != _qspFrontier.nextEvent
            || binding.cursor.digest != _qspFrontier.digest
            || activation.activationDeadlineMs > _facts->expiryMs) {
            return false;
        }

        if (nextRecord < binding.cursor.nextRecord
            || nextEvent < binding.cursor.nextEvent
            || (nextRecord == binding.cursor.nextRecord
                && nextEvent == binding.cursor.nextEvent
                && digest != binding.cursor.digest)
            || nextRecord < _qspFrontier.nextRecord
            || nextEvent < _qspFrontier.nextEvent
            || (nextRecord == _qspFrontier.nextRecord
                && nextEvent == _qspFrontier.nextEvent
                && digest != _qspFrontier.digest)) {
            return false;
        }
        const model::VerifiedChainCursor committedCursor{
            nextRecord, nextEvent, digest};

        ReleasedTopologyAttachmentSnapshot snapshot{};
        snapshot.stream = lease.stream;
        snapshot.authorityGeneration = _facts->authorityGeneration;
        snapshot.rootNodeId = activation.hostRootNodeId;
        snapshot.childNodeId = lease.childId;
        snapshot.parentSourceGeneration = lease.parentSourceGeneration;
        snapshot.parentRouteGeneration = lease.routeGeneration;
        snapshot.parentDataRoute = lease.routes[0];
        snapshot.childBinding = lease.childBinding;
        snapshot.committedCursor = committedCursor;
        snapshot.credentialDeadlineMs = _credentialDeadlineMs;
        snapshot.qtaLimits = _facts->limits;
        if (!snapshot.valid()) return false;

        std::unique_ptr<model::ParentLease> attachedLease;
        try {
            attachedLease = std::make_unique<model::ParentLease>(lease);
        } catch (const std::bad_alloc&) {
            return false;
        }
        const size_t maximumChildRedemptions = static_cast<size_t>(
            _facts->limits.maxChildrenPerRelay);
        std::shared_ptr<qclient::spectator::resilient::QtaAuthorityContext>
            qtaAuthority = maximumChildRedemptions == 0u
            ? nullptr
            : qclient::spectator::resilient::QtaAuthorityContext::create(
                  _facts->authorityGeneration, _credentialDeadlineMs,
                  maximumChildRedemptions);
        if (qtaAuthority == nullptr
            || !_client.acceptPairedActivationCompletion(activationNonce,
                                                          nowMs)) {
            if (qtaAuthority != nullptr) qtaAuthority->revoke();
            wipeBytes(attachedLease.get(), sizeof(*attachedLease));
            return false;
        }

        _qspFrontier = committedCursor;
        _qspFrontierObserved = true;
        _attachedLease = std::move(attachedLease);
        _qtaAuthority = std::move(qtaAuthority);
        _attachmentSnapshot = snapshot;
        retainCompletedClientPlayerRootActivation(activation);
        clearPendingClientPlayerRootActivation();
        _state = ReleasedTopologyClientState::Attached;
        _reason = "attached-via-client-player-root-activation";
        const uint64_t initialWindow = (std::min)(
            kTopologyInitialLivenessWindowMs,
            static_cast<uint64_t>(qnetplay::spectator::resilient::wire::
                kTopologyAuthorityStateMaxLivenessWindowMs));
        _livenessDeadlineMs = _credentialDeadlineMs == 0u
            ? addSaturating(nowMs, initialWindow)
            : (std::min)(_credentialDeadlineMs,
                         addSaturating(nowMs, initialWindow));
        return true;
    }

    if (_state != ReleasedTopologyClientState::AwaitingRootOffer
        || _facts == nullptr || !_pendingRelayChildActivation.has_value()
        || !_pendingRelayChildActivation->committed
        || activationNonce == 0u
        || activationNonce
            != _pendingRelayChildActivation->activation.activationNonce
        || _pendingRelayChildActivation->localDeadlineMs == 0u
        || nowMs >= _pendingRelayChildActivation->localDeadlineMs
        || !_qspFrontierObserved) {
        return false;
    }

    const qnetplay::spectator::resilient::wire::RelayChildActivation&
        activation = _pendingRelayChildActivation->activation;
    const model::ParentLease& childLease = activation.childLease;
    const RouteEndpoint expectedParent{
        activation.relayDataRoute.ipv4BE, activation.relayDataRoute.port};
    const RouteEndpoint originalParent{
        _facts->rootQsp.advertisedIpv4BE, _facts->rootQsp.advertisedPort};
    if (!expectedParent.valid() || !originalParent.valid()
        || expectedParent == originalParent || acceptedParent != expectedParent
        || !streamEquals(activation.stream, _facts->compatibility,
                         _facts->sessionId, _facts->sessionEpoch,
                         _facts->streamRootIdentity)
        || activation.authorityGeneration != _facts->authorityGeneration
        || activation.rootNodeId != _facts->rootNodeId
        || activation.childNodeId != _facts->childNodeId
        || !hasExactDerivedRelayChildActivationLeaseChain(
            activation, _facts->limits.maxActiveDepth)
        || childLease.stream != activation.stream
        || childLease.childId != activation.childNodeId
        || childLease.parentId != activation.relayNodeId
        || childLease.parentSourceGeneration != _facts->sourceGeneration
        || childLease.routeGeneration != activation.relayDataRoute.generation
        || childLease.routeCount != 1u
        || !sameRoute(childLease.routes[0], activation.relayDataRoute)
        || childLease.latestVerifiedCursor
            != activation.commonCursor.nextRecord
        || childLease.childBinding.authorityGeneration
            != _facts->authorityGeneration
        || childLease.childBinding.childViewerId != _facts->viewerId
        || childLease.childBinding.childViewerGeneration
            != _facts->viewerGeneration
        || childLease.childBinding.childRequestedCursor.nextRecord
            != activation.commonCursor.nextRecord
        || childLease.childBinding.childRequestedCursor.nextEvent
            != activation.commonCursor.nextEvent
        || childLease.childBinding.childRequestedCursor.digest
            != activation.commonCursor.digest
        || childLease.replayNonce == 0u || childLease.expiryMs == 0u) {
        return false;
    }

    if (nextRecord < activation.commonCursor.nextRecord
        || nextEvent < activation.commonCursor.nextEvent
        || (nextRecord == activation.commonCursor.nextRecord
            && nextEvent == activation.commonCursor.nextEvent
            && digest != activation.commonCursor.digest)
        || nextRecord < _qspFrontier.nextRecord
        || nextEvent < _qspFrontier.nextEvent
        || (nextRecord == _qspFrontier.nextRecord
            && nextEvent == _qspFrontier.nextEvent
            && digest != _qspFrontier.digest)) {
        return false;
    }
    const model::VerifiedChainCursor committedCursor{
        nextRecord, nextEvent, digest};

    ReleasedTopologyAttachmentSnapshot snapshot{};
    snapshot.stream = childLease.stream;
    snapshot.authorityGeneration = _facts->authorityGeneration;
    snapshot.rootNodeId = activation.rootNodeId;
    snapshot.childNodeId = childLease.childId;
    snapshot.parentSourceGeneration = childLease.parentSourceGeneration;
    snapshot.parentRouteGeneration = childLease.routeGeneration;
    snapshot.parentDataRoute = childLease.routes[0];
    snapshot.childBinding = childLease.childBinding;
    snapshot.committedCursor = committedCursor;
    snapshot.credentialDeadlineMs = _credentialDeadlineMs;
    snapshot.qtaLimits = _facts->limits;
    if (!snapshot.valid()) return false;

    std::unique_ptr<model::ParentLease> attachedLease;
    try {
        attachedLease = std::make_unique<model::ParentLease>(childLease);
    } catch (const std::bad_alloc&) {
        return false;
    }
    const size_t maximumChildRedemptions = static_cast<size_t>(
        _facts->limits.maxChildrenPerRelay);
    std::shared_ptr<qclient::spectator::resilient::QtaAuthorityContext>
        qtaAuthority = maximumChildRedemptions == 0u
        ? nullptr
        : qclient::spectator::resilient::QtaAuthorityContext::create(
              _facts->authorityGeneration, _credentialDeadlineMs,
              maximumChildRedemptions);
    if (qtaAuthority == nullptr
        || !_client.acceptPairedActivationCompletion(activationNonce, nowMs)) {
        if (qtaAuthority != nullptr) qtaAuthority->revoke();
        wipeBytes(attachedLease.get(), sizeof(*attachedLease));
        return false;
    }

    _qspFrontier = committedCursor;
    _qspFrontierObserved = true;
    _attachedLease = std::move(attachedLease);
    _qtaAuthority = std::move(qtaAuthority);
    _attachmentSnapshot = snapshot;
    retainCompletedParentRouteActivation(activation);
    clearPendingRelayChildActivation();
    _state = ReleasedTopologyClientState::Attached;
    _reason = "attached-via-paired-activation";
    const uint64_t initialWindow = (std::min)(
        kTopologyInitialLivenessWindowMs,
        static_cast<uint64_t>(qnetplay::spectator::resilient::wire::
            kTopologyAuthorityStateMaxLivenessWindowMs));
    _livenessDeadlineMs = _credentialDeadlineMs == 0u
        ? addSaturating(nowMs, initialWindow)
        : (std::min)(_credentialDeadlineMs,
                     addSaturating(nowMs, initialWindow));
    return true;
}

bool ReleasedTopologyClientRuntime::relayDataBindingSnapshot(
    ReleasedTopologyRelayBindingSnapshot& out) const {
    out = {};
    if (relayDataBindingState() != ReleasedTopologyRelayBindingState::Ready
        || !_relayDataBindingSnapshot.valid()) {
        return false;
    }
    out = _relayDataBindingSnapshot;
    return true;
}

ReleasedTopologyRelayBindingState
ReleasedTopologyClientRuntime::relayDataBindingState() const {
    if (_state == ReleasedTopologyClientState::Disabled
        || _state == ReleasedTopologyClientState::Unavailable
        || _relayDataBindingUnavailable) {
        return ReleasedTopologyRelayBindingState::Unavailable;
    }
    if (_relayDataBindingPending && _relayDataSocket.isOpen())
        return ReleasedTopologyRelayBindingState::Pending;
    if (_state == ReleasedTopologyClientState::Attached
        && _relayDataBindingSnapshot.valid() && _relayDataSocket.isOpen()
        && _relayDataLocalPort != 0u
        && _relayDataSocket.boundPort() == _relayDataLocalPort) {
        return ReleasedTopologyRelayBindingState::Ready;
    }
    return ReleasedTopologyRelayBindingState::Inactive;
}

ReleasedTopologyRelaySocketBorrow
ReleasedTopologyClientRuntime::borrowedRelayDataSocket() const {
    ReleasedTopologyRelaySocketBorrow out{};
    out.generation = _relayDataSocketGeneration;
    if (relayDataBindingState() == ReleasedTopologyRelayBindingState::Ready) {
        out.socket = static_cast<uintptr_t>(_relayDataSocket.raw());
    }
    return out;
}

bool ReleasedTopologyClientRuntime::startRelayNode(
    const qnetplay::spectator::SpectatorViewer& verifiedViewer,
    const uint64_t nowMs) {
    using qclient::spectator::resilient::NodeRuntimeResult;
    using qclient::spectator::resilient::QtaVerifiedCanonicalIngress;
    using qclient::spectator::resilient::ResilientSpectatorNodeRuntime;

    if (relayDataBindingState() != ReleasedTopologyRelayBindingState::Ready)
        return false;
    if (!relayAuthorityActive(nowMs)) {
        clearRelayDataBinding(true);
        return false;
    }
    if (_relayNode != nullptr) {
        return _relayViewer == &verifiedViewer && _relayNode->running()
            && _relayCanonicalIngress != nullptr;
    }
    if (_relayViewer != nullptr && _relayViewer != &verifiedViewer)
        return false;

    qclient::spectator::resilient::ResilientSpectatorNodeRuntimeConfig config{};
    if (!makeRelayNodeConfig(verifiedViewer, config)) {
        clearRelayDataBinding(true);
        return false;
    }
    std::unique_ptr<ResilientSpectatorNodeRuntime> node(
        new (std::nothrow) ResilientSpectatorNodeRuntime());
    if (!node || !node->start(config)) {
        clearRelayDataBinding(true);
        return false;
    }
    std::unique_ptr<QtaVerifiedCanonicalIngress> ingress(
        new (std::nothrow) QtaVerifiedCanonicalIngress(
            _qtaAuthority, config, verifiedViewer));
    if (!ingress) {
        node->stop();
        clearRelayDataBinding(true);
        return false;
    }
    if (node->projectCanonicalVerifiedPrefix(*ingress, verifiedViewer, nowMs)
        != NodeRuntimeResult::Accepted) {
        node->stop();
        clearRelayDataBinding(true);
        return false;
    }
    _relayViewer = &verifiedViewer;
    _relayCanonicalIngress = std::move(ingress);
    _relayNode = std::move(node);
    serviceRelayReady(nowMs);
    return true;
}

qclient::spectator::resilient::NodeRuntimeResult
ReleasedTopologyClientRuntime::projectRelayViewer(
    const qnetplay::spectator::SpectatorViewer& verifiedViewer,
    const uint64_t nowMs) {
    using qclient::spectator::resilient::NodeRuntimeResult;
    if (_relayNode == nullptr || _relayCanonicalIngress == nullptr
        || _relayViewer != &verifiedViewer) {
        return NodeRuntimeResult::NotRunning;
    }
    if (!relayAuthorityActive(nowMs)) {
        clearRelayDataBinding(true);
        return NodeRuntimeResult::NotRunning;
    }
    const NodeRuntimeResult result = _relayNode->projectCanonicalVerifiedPrefix(
        *_relayCanonicalIngress, verifiedViewer, nowMs);
    if (!_relayNode->running()) clearRelayDataBinding(true);
    return result;
}

qclient::spectator::resilient::NodeRuntimeResult
ReleasedTopologyClientRuntime::ingestRelayChildDatagram(
    const RouteEndpoint& observedSource, const uint8_t* const bytes,
    const size_t length, const uint64_t nowMs) {
    using qclient::spectator::resilient::NodeRuntimeChildIngress;
    using qclient::spectator::resilient::NodeRuntimeResult;
    if (_relayNode == nullptr || _relayCanonicalIngress == nullptr)
        return NodeRuntimeResult::NotRunning;
    if (!relayAuthorityActive(nowMs)) {
        clearRelayDataBinding(true);
        return NodeRuntimeResult::NotRunning;
    }
    if (!observedSource.valid() || bytes == nullptr || length == 0u)
        return NodeRuntimeResult::ChildRejected;
    const RelayChildBinding* const binding = relayChildForSource(observedSource);
    if (binding == nullptr) return NodeRuntimeResult::ChildRejected;
    const NodeRuntimeChildIngress ingress(
        binding->route, binding->endpointBindingGeneration,
        binding->endpointToken);
    return _relayNode->onChildDatagram(ingress, bytes, length, nowMs);
}

qclient::spectator::resilient::NodeRuntimeResult
ReleasedTopologyClientRuntime::ingestRelayChildDatagram(
    const RouteEndpoint& observedSource,
    const qnetplay::spectator::EndpointId endpointToken,
    const uint8_t* const bytes, const size_t length, const uint64_t nowMs) {
    using qclient::spectator::resilient::NodeRuntimeResult;
    (void)observedSource;
    (void)endpointToken;
    (void)bytes;
    (void)length;
    (void)nowMs;
    return NodeRuntimeResult::ChildRejected;
}

const ReleasedTopologyClientRuntime::RelayChildBinding*
ReleasedTopologyClientRuntime::relayChildForSource(
    const RouteEndpoint& observedSource) const {
    const auto found = std::find_if(
        _relayChildren.begin(), _relayChildren.end(),
        [this, &observedSource](const RelayChildBinding& candidate) {
            return routeMatchesEndpoint(candidate.route, observedSource);
        });
    return found == _relayChildren.end() ? nullptr : &*found;
}

const ReleasedTopologyClientRuntime::RelayChildBinding*
ReleasedTopologyClientRuntime::relayChildForEndpoint(
    const qnetplay::spectator::EndpointId endpointToken) const {
    const auto found = std::find_if(
        _relayChildren.begin(), _relayChildren.end(),
        [endpointToken](const RelayChildBinding& candidate) {
            return candidate.endpointToken == endpointToken;
        });
    return found == _relayChildren.end() ? nullptr : &*found;
}

bool ReleasedTopologyClientRuntime::routeMatchesEndpoint(
    const model::RouteDescriptor& route, const RouteEndpoint& endpoint) const {
    return route.valid() && route.routeProved && endpoint.valid()
        && route.ipv4BE == endpoint.ipv4BE && route.port == endpoint.port;
}

std::vector<ReleasedTopologyRelayDatagram>
ReleasedTopologyClientRuntime::pollRelayChildDatagrams(
    const uint64_t nowMs, const size_t packetBudget) {
    std::vector<ReleasedTopologyRelayDatagram> out;
    if (_relayNode == nullptr || _relayCanonicalIngress == nullptr)
        return out;
    if (!relayAuthorityActive(nowMs)) {
        clearRelayDataBinding(true);
        return out;
    }
    const std::vector<qclient::spectator::resilient::NodeRuntimeDownstreamDatagram>
        packets = _relayNode->pollChildOutbound(nowMs, packetBudget);
    try {
        out.reserve(packets.size());
        for (const auto& packet : packets) {
            const RelayChildBinding* const binding =
                relayChildForEndpoint(packet.destination);
            if (binding == nullptr || !routeMatchesEndpoint(
                                       binding->route,
                                       RouteEndpoint{binding->route.ipv4BE,
                                                     binding->route.port})
                || packet.bytes.empty()) {
                continue;
            }
            ReleasedTopologyRelayDatagram translated{};
            translated.destination = RouteEndpoint{binding->route.ipv4BE,
                                                   binding->route.port};
            translated.bytes = packet.bytes;
            out.push_back(std::move(translated));
        }
    } catch (const std::bad_alloc&) {
        out.clear();
    }
    return out;
}

std::vector<qclient::spectator::resilient::NodeRuntimeDownstreamDatagram>
ReleasedTopologyClientRuntime::pollRelayChildOutbound(
    const uint64_t nowMs, const size_t packetBudget) {
    if (_relayNode == nullptr || _relayCanonicalIngress == nullptr)
        return {};
    if (!relayAuthorityActive(nowMs)) {
        clearRelayDataBinding(true);
        return {};
    }
    return _relayNode->pollChildOutbound(nowMs, packetBudget);
}

bool ReleasedTopologyClientRuntime::relayNodeRunning() const {
    return _relayNode != nullptr && _relayNode->running();
}

void ReleasedTopologyClientRuntime::stopRelayNode() {
    clearRelayDataBinding(true);
}

}
