#include "client/spectator/ReleasedClientRootControl.h"

#include <algorithm>
#include <cstring>

#include "spectator/SpectatorGrant.h"

namespace qclient::spectator::topology::clientroot {
namespace {

constexpr char kClaimTagDomain[] = "qoh99.spectator.clientroot.claim.v1";
constexpr char kEnrollmentTagDomain[] =
    "qoh99.spectator.clientroot.enrollment.v1";

void wipeBytes(uint8_t* bytes, const size_t length) {
    if (bytes == nullptr || length == 0u) return;
    volatile uint8_t* volatileBytes = bytes;
    for (size_t index = 0u; index < length; ++index) volatileBytes[index] = 0u;
}

bool equalConstantTime(const uint8_t* left, const uint8_t* right,
                       const size_t length) {
    if (left == nullptr || right == nullptr) return false;
    uint8_t difference = 0u;
    for (size_t index = 0u; index < length; ++index)
        difference |= static_cast<uint8_t>(left[index] ^ right[index]);
    return difference == 0u;
}

const char* tagDomain(const Direction direction) {
    return direction == Direction::Claim ? kClaimTagDomain
                                         : kEnrollmentTagDomain;
}

bool validRouteKind(const uint8_t kind) {
    return kind <= static_cast<uint8_t>(model::RouteKind::RootFallback);
}

// Local mirror of the controlv2 endpoint-binding serializer: keep the field mapping and validity rules identical.
bool bindingToWire(const EndpointBinding& binding,
                   qnet::SpectateTopologyEndpointBindingWire& out) {
    out = {};
    const auto validProvenance = [](const EndpointProvenance provenance) {
        return provenance == EndpointProvenance::LocalBind
            || provenance == EndpointProvenance::PublishedDirectCandidate
            || provenance == EndpointProvenance::ProvenDirectQsp
            || provenance == EndpointProvenance::ProvenPunchedQsp;
    };
    if (binding.local.port == 0u || !binding.advertised.valid()
        || binding.advertised.generation != binding.generation
        || !validProvenance(binding.localProvenance)
        || !validProvenance(binding.advertisedProvenance)
        || binding.advertised.kind == model::RouteKind::TrafficRelay) {
        return false;
    }
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

bool routeFromWire(const qnet::ClientRootRouteWire& wire,
                   model::RouteDescriptor& out) {
    out = {};
    if (!validRouteKind(wire.kind) || wire.routeProved > 1u
        || wire.ipv4BE == 0u || wire.port == 0u || wire.generation == 0u) {
        return false;
    }
    out.kind = static_cast<model::RouteKind>(wire.kind);
    out.ipv4BE = wire.ipv4BE;
    out.port = wire.port;
    out.generation = wire.generation;
    out.routeProved = wire.routeProved == 1u;
    return true;
}

}

void wipeControlBuffer(std::vector<uint8_t>& bytes) {
    wipeBytes(bytes.data(), bytes.size());
    bytes.clear();
}

bool makeControlTag(const uint8_t* token, const size_t tokenBytes,
                    const Direction direction,
                    const uint8_t* packedBodyWithoutTag,
                    const size_t packedBodyWithoutTagBytes,
                    std::array<uint8_t, 16u>& out) {
    out.fill(0u);
    if (token == nullptr || tokenBytes != qnet::kClientRootTokenBytes
        || packedBodyWithoutTag == nullptr
        || packedBodyWithoutTagBytes == 0u) {
        return false;
    }
    const char* const domain = tagDomain(direction);
    std::array<uint8_t, 32u> subkey{};
    qnetplay::spectator::hmacSha256(
        token, tokenBytes, reinterpret_cast<const uint8_t*>(domain),
        std::strlen(domain), subkey);
    std::array<uint8_t, 32u> digest{};
    qnetplay::spectator::hmacSha256(subkey.data(), subkey.size(),
                                    packedBodyWithoutTag,
                                    packedBodyWithoutTagBytes, digest);
    std::copy_n(digest.data(), out.size(), out.data());
    wipeBytes(digest.data(), digest.size());
    wipeBytes(subkey.data(), subkey.size());
    return true;
}

bool verifyControlTag(const uint8_t* token, const size_t tokenBytes,
                      const Direction direction,
                      const uint8_t* packedBodyWithoutTag,
                      const size_t packedBodyWithoutTagBytes,
                      const std::array<uint8_t, 16u>& expected) {
    std::array<uint8_t, 16u> actual{};
    const bool tagged = makeControlTag(token, tokenBytes, direction,
                                       packedBodyWithoutTag,
                                       packedBodyWithoutTagBytes, actual);
    const bool matched = tagged && equalConstantTime(actual.data(),
                                                     expected.data(),
                                                     actual.size());
    wipeBytes(actual.data(), actual.size());
    return matched;
}

bool claimFromWire(const qnet::ClientRootClaim& wire,
                   const uint8_t expectedRemotePlayerRole,
                   const uint64_t sessionId, const uint64_t sessionEpoch,
                   ClientPlayerRootClaim& out) {
    out = {};
    const bool roleValid = wire.remotePlayerRole
            == static_cast<uint8_t>(
                qnetplay::spectator::resilient::wire::ClientPlayerRootRole::
                    RemotePlayerOne)
        || wire.remotePlayerRole
            == static_cast<uint8_t>(
                qnetplay::spectator::resilient::wire::ClientPlayerRootRole::
                    RemotePlayerTwo);
    if (wire.schemaVersion != 1u || !roleValid
        || wire.remotePlayerRole != expectedRemotePlayerRole
        || wire.requestNonce == 0u || sessionId == 0u || sessionEpoch == 0u
        || wire.sessionEpoch != sessionEpoch || wire.processId == 0u
        || wire.publisherGeneration == 0u || wire.publisherNonce == 0u
        || wire.runtimeStage
            != static_cast<uint8_t>(
                qnetplay::config_provenance::Stage::RuntimeApplied)
        || wire.streamRootIdentity == 0u || wire.gameStartFingerprint == 0u
        || wire.sourceGeneration == 0u || wire.qspRouteGeneration == 0u
        || wire.qsrBindingGeneration == 0u
        || wire.cursorDigest == 0u || wire.childCapacity == 0u
        || wire.expiresMs == 0u
        || wire.cacheNextCursor < wire.cacheFirstCursor
        || wire.cursorNextRecord < wire.cacheFirstCursor
        || wire.cursorNextRecord > wire.cacheNextCursor) {
        return false;
    }
    if (!routeFromWire(wire.dataRoute, out.dataRoute)
        || !routeFromWire(wire.qsrRoute, out.qsrRoute)) {
        out = {};
        return false;
    }
    if (out.dataRoute.ipv4BE == out.qsrRoute.ipv4BE
        && out.dataRoute.port == out.qsrRoute.port) {
        out = {};
        return false;
    }
    out.sessionId = sessionId;
    out.sessionEpoch = sessionEpoch;
    out.remotePlayerRole =
        static_cast<qnetplay::spectator::resilient::wire::ClientPlayerRootRole>(
            wire.remotePlayerRole);
    out.compatibility = wire.compatibilityIdentity;
    out.configurationProvenance = wire.provenance;
    out.streamRootIdentity = wire.streamRootIdentity;
    out.gameStartFingerprint = wire.gameStartFingerprint;
    out.publisherGeneration = wire.publisherGeneration;
    out.publisherNonce = wire.publisherNonce;
    out.runtimeReceipt.processId = wire.processId;
    out.runtimeReceipt.publisherGeneration = wire.publisherGeneration;
    out.runtimeReceipt.publisherNonce = wire.publisherNonce;
    out.runtimeReceipt.playerSlot = wire.playerSlot;
    out.runtimeReceipt.runtimeStage =
        qnetplay::config_provenance::Stage::RuntimeApplied;
    out.runtimeReceipt.delay = wire.delay;
    out.runtimeReceipt.rollback = wire.rollback;
    out.runtimeReceipt.remoteDelay = wire.remoteDelay;
    out.runtimeReceipt.remoteRollback = wire.remoteRollback;
    out.runtimeReceipt.provenance = wire.provenance;
    out.sourceGeneration = wire.sourceGeneration;
    out.qspRouteGeneration = wire.qspRouteGeneration;
    out.qsrBindingGeneration = wire.qsrBindingGeneration;
    out.cacheRange.firstCursor = wire.cacheFirstCursor;
    out.cacheRange.nextCursor = wire.cacheNextCursor;
    out.cursor.nextRecord = wire.cursorNextRecord;
    out.cursor.nextEvent = wire.cursorNextEvent;
    out.cursor.digest = wire.cursorDigest;
    out.childCapacity = wire.childCapacity;
    out.expiresMs = wire.expiresMs;
    return true;
}

bool enrollmentToWire(const uint64_t requestNonce,
                      const ClientPlayerRootEnrollment& enrollment,
                      const uint8_t* grantBlob, const size_t grantBlobBytes,
                      qnet::ClientRootEnrollment& out) {
    out = {};
    if (requestNonce == 0u || !enrollment.granted
        || enrollment.clientRootNodeId == model::kInvalidNodeId
        || enrollment.hostRootNodeId == model::kInvalidNodeId
        || enrollment.authorityGeneration == 0u || enrollment.expiresMs == 0u
        || grantBlob == nullptr || grantBlobBytes == 0u
        || grantBlobBytes > qnet::kClientRootGrantBlobMax) {
        return false;
    }
    if (!bindingToWire(enrollment.rootQsp, out.rootQsp)
        || !bindingToWire(enrollment.rootQsr, out.rootQsr)
        || enrollment.hostSourceGeneration == 0u) {
        out = {};
        return false;
    }
    out.schemaVersion = 1u;
    out.granted = 1u;
    out.requestNonce = requestNonce;
    out.clientRootNodeId = enrollment.clientRootNodeId;
    out.hostRootNodeId = enrollment.hostRootNodeId;
    out.authorityGeneration = enrollment.authorityGeneration;
    out.expiresMs = enrollment.expiresMs;
    out.hostSourceGeneration = enrollment.hostSourceGeneration;
    static_assert(sizeof(out.authorityPublicKey)
                      == std::tuple_size<
                          decltype(enrollment.assignmentSigningPublicKey)>::value,
                  "authority public key width divergence");
    std::memcpy(out.authorityPublicKey,
                enrollment.assignmentSigningPublicKey.data(),
                sizeof(out.authorityPublicKey));
    static_assert(sizeof(out.topologyCredential)
                      == std::tuple_size<decltype(enrollment.credential)>::value,
                  "client-root credential width divergence");
    std::memcpy(out.topologyCredential, enrollment.credential.data(),
                sizeof(out.topologyCredential));
    out.grantLength = static_cast<uint16_t>(grantBlobBytes);
    std::memcpy(out.grantBlob, grantBlob, grantBlobBytes);
    return true;
}

bool enrollmentFromWire(
    const qnet::ClientRootEnrollment& wire,
    const uint64_t expectedRequestNonce,
    std::array<uint8_t, qnet::kClientRootCredentialBytes>& credential,
    std::vector<uint8_t>& grantBlob) {
    credential.fill(0u);
    grantBlob.clear();
    if (wire.schemaVersion != 1u || wire.granted != 1u
        || expectedRequestNonce == 0u
        || wire.requestNonce != expectedRequestNonce
        || wire.clientRootNodeId == model::kInvalidNodeId
        || wire.hostRootNodeId == model::kInvalidNodeId
        || wire.clientRootNodeId == wire.hostRootNodeId
        || wire.authorityGeneration == 0u || wire.expiresMs == 0u
        || wire.hostSourceGeneration == 0u
        || wire.rootQsr.advertisedIpv4BE == 0u
        || wire.rootQsr.advertisedPort == 0u
        || wire.rootQsr.bindingGeneration == 0u
        || wire.grantLength == 0u
        || wire.grantLength > qnet::kClientRootGrantBlobMax) {
        return false;
    }
    std::copy_n(wire.topologyCredential, credential.size(),
                credential.data());
    grantBlob.assign(wire.grantBlob, wire.grantBlob + wire.grantLength);
    return true;
}

}
