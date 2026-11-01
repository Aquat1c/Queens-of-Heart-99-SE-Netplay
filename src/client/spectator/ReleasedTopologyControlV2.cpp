#include "client/spectator/ReleasedTopologyControlV2.h"

#include "client/spectator/ResilientTopologyClient.h"
#include "spectator/SpectatorRelayAdapter.h"
#include "spectator/TopologyRelayAdapter.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace qclient::spectator::topology::controlv2 {
namespace {

using qclient::spectator::topology::client::TopologyControlV2Direction;
using qclient::spectator::topology::client::kQtaMacBytes;

bool grantFromWire(const qnet::SpectatorGrantWire& wire,
                   qnetplay::spectator::SpectatorGrant& grant) {
    for (const uint8_t byte : wire.reserved)
        if (byte != 0u) return false;
    if (wire.grantId == 0u || wire.viewerId == 0u || wire.sessionId == 0u
        || wire.expiryUnix == 0u || wire.sourceGeneration == 0u
        || (wire.macLow == 0u && wire.macHigh == 0u)
        || (wire.route != qnet::SPECTATE_ROUTE_DIRECT
            && wire.route != qnet::SPECTATE_ROUTE_PUNCHED
            && wire.route != qnet::SPECTATE_ROUTE_RELAY)) {
        return false;
    }
    grant = {};
    grant.grantId = wire.grantId;
    grant.viewerId = wire.viewerId;
    grant.sessionId = wire.sessionId;
    grant.expiryUnix = wire.expiryUnix;
    grant.sourceGeneration = wire.sourceGeneration;
    grant.resumeRecord = wire.resumeRecord;
    grant.resumeDigest = wire.resumeDigest;
    grant.route = wire.route;
    grant.macLow = wire.macLow;
    grant.macHigh = wire.macHigh;
    return true;
}

bool makeCanonical(const TopologyControlV2Direction direction,
                   const void* const record, const size_t bytesWithoutTag,
                   std::vector<uint8_t>& out) {
    return client::makeTopologyControlV2CanonicalInput(
        direction, static_cast<const uint8_t*>(record), bytesWithoutTag, out);
}

void wipeTag(std::array<uint8_t, kQtaMacBytes>& tag) {
    volatile uint8_t* cursor = tag.data();
    for (size_t index = 0u; index < tag.size(); ++index) cursor[index] = 0u;
}

bool allZero(const uint8_t* const bytes, const size_t count) {
    if (bytes == nullptr || count == 0u) return true;
    uint8_t aggregate = 0u;
    for (size_t index = 0u; index < count; ++index) aggregate |= bytes[index];
    return aggregate == 0u;
}

bool equalConstantTime(const uint8_t* const left, const uint8_t* const right,
                       const size_t count) {
    if (left == nullptr || right == nullptr) return false;
    uint8_t difference = 0u;
    for (size_t index = 0u; index < count; ++index)
        difference |= static_cast<uint8_t>(left[index] ^ right[index]);
    return difference == 0u;
}

}

void wipeControlBuffer(std::vector<uint8_t>& bytes) {
    if (!bytes.empty()) {
        volatile uint8_t* cursor = bytes.data();
        size_t remaining = bytes.size();
        while (remaining-- != 0u) *cursor++ = 0u;
    }
    bytes.clear();
}

bool endpointBindingToWire(const EndpointBinding& binding,
                           qnet::SpectateTopologyEndpointBindingWire& out) {
    out = {};
    if (!binding.validForAuthority()) return false;
    out.localIpv4BE = binding.local.ipv4BE;
    out.localPort = binding.local.port;
    out.advertisedRouteKind = static_cast<uint8_t>(binding.advertised.kind);
    out.advertisedIpv4BE = binding.advertised.ipv4BE;
    out.advertisedPort = binding.advertised.port;
    out.advertisedRouteGeneration = binding.advertised.generation;
    out.advertisedRouteProved = binding.advertised.routeProved ? 1u : 0u;
    out.localProvenance = static_cast<uint8_t>(binding.localProvenance);
    out.advertisedProvenance =
        static_cast<uint8_t>(binding.advertisedProvenance);
    out.bindingGeneration = binding.generation;
    return true;
}

bool infoFromEnrollment(const uint64_t requestNonce,
                        const TopologyEnrollment& enrollment,
                        qnet::SpectateTopologyInfoV2& out) {
    using qclient::spectator::topology::EndpointProvenance;
    out = {};
    const bool qspDirect = enrollment.qspProvenance
        == EndpointProvenance::ProvenDirectQsp;
    const bool qspPunched = enrollment.qspProvenance
        == EndpointProvenance::ProvenPunchedQsp;
    const bool qspRelay = enrollment.qspProvenance
        == EndpointProvenance::ProvenRelayQsp;
    const bool virtualQspEndpoint = enrollment.qspEndpoint.ipv4BE
        == qnetplay::spectator::kRelayVirtualIpv4BE;
    if (qspRelay != virtualQspEndpoint
        || qspRelay != enrollment.relayQsrOffer.present) {
        out = {};
        return false;
    }
    if (requestNonce == 0u || !enrollment.granted
        || !qnetplay::spectator::isValidIdentity(enrollment.session)
        || !qnetplay::compat::isValidCompatibilityIdentity(
               enrollment.compatibility)
        || enrollment.streamRootIdentity == 0u
        || enrollment.sourceGeneration == 0u
        || enrollment.authorityGeneration == 0u
        || enrollment.rootNodeId
               == qnetplay::spectator::resilient::kInvalidNodeId
        || enrollment.nodeId == qnetplay::spectator::resilient::kInvalidNodeId
        || enrollment.rootNodeId == enrollment.nodeId
        || enrollment.expiresMs == 0u || enrollment.bindingGeneration == 0u
        || enrollment.viewerId == 0u || enrollment.viewerGeneration == 0u
        || !enrollment.qspEndpoint.valid()
        || !(qspDirect || qspPunched || qspRelay)
        || enrollment.qspRouteGeneration == 0u || !enrollment.limits.valid()
        || !endpointBindingToWire(enrollment.rootQsp, out.rootQsp)
        || !endpointBindingToWire(enrollment.rootQsr, out.rootQsr)
        || out.rootQsr.advertisedRouteKind
               != static_cast<uint8_t>(
                   qnetplay::spectator::resilient::RouteKind::Direct)
        || out.rootQsp.advertisedRouteProved != 1u
        || out.rootQsr.advertisedRouteProved != 0u
        || out.rootQsr.advertisedProvenance
               != static_cast<uint8_t>(
                   EndpointProvenance::PublishedDirectCandidate)
        || out.rootQsp.advertisedPort == out.rootQsr.advertisedPort
        || out.rootQsp.localPort == out.rootQsr.localPort) {
        out = {};
        return false;
    }
    uint8_t credentialOr = 0u;
    for (const uint8_t byte : enrollment.credential) credentialOr |= byte;
    uint8_t signingKeyOr = 0u;
    for (const uint8_t byte : enrollment.assignmentSigningPublicKey)
        signingKeyOr |= byte;
    if (credentialOr == 0u || signingKeyOr == 0u
        || enrollment.assignmentSigningPublicKey.size()
            != qnet::kSpectateTopologyAuthorityPublicKeyBytes) {
        out = {};
        return false;
    }

    out.schemaVersion = qnet::kSpectateTopologyControlSchemaVersion;
    out.requiredFeatures = qnet::kSpectateTopologyControlRequiredFeatures;
    out.state = qnet::SPECTATE_TOPOLOGY_GRANTED;
    out.qspRoute = qspRelay
        ? qnet::SPECTATE_ROUTE_RELAY
        : (qspDirect ? qnet::SPECTATE_ROUTE_DIRECT
                     : qnet::SPECTATE_ROUTE_PUNCHED);
    out.qspProvenance = static_cast<uint8_t>(enrollment.qspProvenance);
    out.rootQspRouteKind = out.rootQsp.advertisedRouteKind;
    out.rootQspRouteProved = out.rootQsp.advertisedRouteProved;
    out.rootQsrRouteKind = out.rootQsr.advertisedRouteKind;
    out.rootQsrRouteProved = out.rootQsr.advertisedRouteProved;
    out.requestNonce = requestNonce;
    out.sessionId = enrollment.session.sessionId;
    out.sessionEpoch = enrollment.session.epoch;
    out.viewerId = enrollment.viewerId;
    out.viewerGeneration = enrollment.viewerGeneration;
    out.confirmedCursor = enrollment.confirmedCursor;
    out.streamRootIdentity = enrollment.streamRootIdentity;
    out.compatibilityIdentity = enrollment.compatibility;
    out.authorityGeneration = enrollment.authorityGeneration;
    out.sourceGeneration = enrollment.sourceGeneration;
    out.rootNodeId = enrollment.rootNodeId;
    out.childNodeId = enrollment.nodeId;
    out.expiryMs = enrollment.expiresMs;
    out.bindingGeneration = enrollment.bindingGeneration;
    out.qspIpv4BE = enrollment.qspEndpoint.ipv4BE;
    out.qspPort = enrollment.qspEndpoint.port;
    out.qspRouteGeneration = enrollment.qspRouteGeneration;
    out.qtaCursor.nextRecord = enrollment.qtaCursor.nextRecord;
    out.qtaCursor.nextEvent = enrollment.qtaCursor.nextEvent;
    out.qtaCursor.digest = enrollment.qtaCursor.digest;
    out.qtaLimits.maxTopologyNodes =
        static_cast<uint64_t>(enrollment.limits.maxTopologyNodes);
    out.qtaLimits.maxActiveDepth = enrollment.limits.maxActiveDepth;
    out.qtaLimits.maxChildrenPerRoot =
        static_cast<uint64_t>(enrollment.limits.maxChildrenPerRoot);
    out.qtaLimits.maxChildrenPerRelay =
        static_cast<uint64_t>(enrollment.limits.maxChildrenPerRelay);
    out.qtaLimits.maxStandbyParents =
        static_cast<uint64_t>(enrollment.limits.maxStandbyParents);
    out.qtaLimits.standbyLeaseMs = enrollment.limits.standbyLeaseMs;
    std::memcpy(out.authorityAssignmentPublicKey,
                enrollment.assignmentSigningPublicKey.data(),
                sizeof(out.authorityAssignmentPublicKey));
    std::memcpy(out.topologyCredential, enrollment.credential.data(),
                sizeof(out.topologyCredential));
    if (qspRelay
        && !qnetplay::spectator::writeRelayQsrOffer(enrollment.relayQsrOffer,
                                                    out)) {
        out = {};
        return false;
    }
    return true;
}

bool signInfo(qnet::SpectateTopologyInfoV2& info,
              const qnetplay::spectator::SpectatorGrant& grant) {
    std::vector<uint8_t> canonical;
    std::array<uint8_t, kQtaMacBytes> tag{};
    const bool ready = makeCanonical(
            TopologyControlV2Direction::Response, &info,
            offsetof(qnet::SpectateTopologyInfoV2, responseTag), canonical)
        && client::makeTopologyControlV2Tag(
            grant.macLow, grant.macHigh, canonical.data(), canonical.size(),
            tag);
    wipeControlBuffer(canonical);
    if (!ready) {
        wipeTag(tag);
        return false;
    }
    std::memcpy(info.responseTag, tag.data(), tag.size());
    wipeTag(tag);
    return true;
}

bool verifyProofTag(const qnet::SpectateTopologyProofV2& proof,
                    const qnetplay::spectator::SpectatorGrant& grant) {
    std::vector<uint8_t> canonical;
    std::array<uint8_t, kQtaMacBytes> tag{};
    std::memcpy(tag.data(), proof.requestTag, tag.size());
    const bool accepted = makeCanonical(
            TopologyControlV2Direction::Request, &proof,
            offsetof(qnet::SpectateTopologyProofV2, requestTag), canonical)
        && client::verifyTopologyControlV2Tag(
            grant.macLow, grant.macHigh, canonical.data(), canonical.size(),
            tag);
    wipeControlBuffer(canonical);
    wipeTag(tag);
    return accepted;
}

bool validateProof(const qnet::SpectateTopologyProofV2& proof,
                   const qnet::ClientIntent& intent,
                   const qnet::SpectateInfo& answer,
                   const qnet::SpectateTopologyChallengeV2& challenge,
                   qnetplay::spectator::SpectatorGrant& grant) {
    grant = {};
    if (proof.schemaVersion != qnet::kSpectateTopologyControlSchemaVersion
        || proof.requiredFeatures
               != qnet::kSpectateTopologyControlRequiredFeatures
        || proof.requestNonce == 0u || proof.viewerId != intent.viewerId
        || proof.viewerGeneration != intent.resumeRecord
        || proof.qspRoute != intent.spectatorRoute
        || proof.qspRouteGeneration == 0u || proof.qspIpv4BE == 0u
        || proof.qspPort == 0u || proof.sessionEpoch == 0u
        || proof.discoveryGeneration == 0u || proof.streamRootIdentity == 0u
        || proof.reservedRoute[0] != 0u || proof.reservedRoute[1] != 0u
        || proof.reservedRoute[2] != 0u || proof.reservedEndpoint != 0u
        || allZero(proof.peerChallenge, sizeof(proof.peerChallenge))
        || !qnetplay::compat::isValidCompatibilityIdentity(
               proof.compatibilityIdentity)
        || !grantFromWire(proof.grant, grant)
        || grant.viewerId != intent.viewerId
        || grant.sessionId != answer.sessionId
        || proof.discoveryGeneration != grant.sourceGeneration
        || grant.route != proof.qspRoute
        || (proof.qspRoute != qnet::SPECTATE_ROUTE_DIRECT
            && proof.qspRoute != qnet::SPECTATE_ROUTE_PUNCHED
            && proof.qspRoute != qnet::SPECTATE_ROUTE_RELAY)
        || challenge.schemaVersion != qnet::kSpectateTopologyControlSchemaVersion
        || challenge.requiredFeatures
               != qnet::kSpectateTopologyControlRequiredFeatures
        || challenge.sessionId != grant.sessionId
        || challenge.viewerId != proof.viewerId
        || challenge.viewerGeneration != proof.viewerGeneration
        || challenge.reserved != 0u
        || allZero(challenge.peerChallenge, sizeof(challenge.peerChallenge))
        || !equalConstantTime(proof.peerChallenge, challenge.peerChallenge,
                              sizeof(proof.peerChallenge))) {
        grant = {};
        return false;
    }
    return verifyProofTag(proof, grant);
}

}
