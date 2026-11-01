#include <cstdio>
#include <cstring>

#include "client/spectator/ReleasedClientRootControl.h"

namespace {

namespace clientroot = qclient::spectator::topology::clientroot;

int g_failures = 0;

#define CHECK_TRUE(expression)                                                \
    do {                                                                      \
        if (!(expression)) {                                                  \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                         #expression);                                        \
            ++g_failures;                                                     \
        }                                                                     \
    } while (false)

constexpr uint64_t kSessionId = 0x51525354u;
constexpr uint64_t kSessionEpoch = 0x00A1B2C3D4E5F607ull;

void fillToken(uint8_t (&token)[qnet::kClientRootTokenBytes],
               const uint8_t seed) {
    for (size_t index = 0u; index < sizeof(token); ++index)
        token[index] = static_cast<uint8_t>(seed + index);
}

qnet::ClientRootClaim makeValidClaimWire() {
    qnet::ClientRootClaim wire{};
    wire.schemaVersion = 1u;
    wire.remotePlayerRole = 2u;
    wire.playerSlot = 1u;
    wire.requestNonce = 0xC0FFEE01u;
    wire.sessionId = static_cast<uint32_t>(kSessionId);
    wire.sessionEpoch = kSessionEpoch;
    wire.processId = 4242u;
    wire.publisherGeneration = 7u;
    wire.publisherNonce = 0xAB12CD34EF56ull;
    wire.runtimeStage = 7u;
    wire.delay = 0u;
    wire.rollback = 7u;
    wire.remoteDelay = 0u;
    wire.remoteRollback = 15u;
    wire.streamRootIdentity = 0x5354524D524F4F54ull;
    wire.gameStartFingerprint = 0x47414D4553544152ull;
    wire.sourceGeneration = 3u;
    wire.qspRouteGeneration = 2u;
    wire.qsrBindingGeneration = 1u;
    wire.dataRoute.kind = 0u;
    wire.dataRoute.routeProved = 1u;
    wire.dataRoute.port = 41001u;
    wire.dataRoute.ipv4BE = 0x0100007Fu;
    wire.dataRoute.generation = 1u;
    wire.qsrRoute.kind = 0u;
    wire.qsrRoute.routeProved = 1u;
    wire.qsrRoute.port = 41002u;
    wire.qsrRoute.ipv4BE = 0x0100007Fu;
    wire.qsrRoute.generation = 1u;
    wire.cacheFirstCursor = 0u;
    wire.cacheNextCursor = 704u;
    wire.cursorNextRecord = 704u;
    wire.cursorNextEvent = 12u;
    wire.cursorDigest = 0x44494745535431ull;
    wire.childCapacity = 4u;
    wire.expiresMs = 90000u;
    return wire;
}

void testTagSchemeRoundTripAndDomains() {
    uint8_t token[qnet::kClientRootTokenBytes];
    fillToken(token, 0x10u);
    const uint8_t body[64] = {1u, 2u, 3u, 4u, 5u};
    std::array<uint8_t, 16u> tag{};
    CHECK_TRUE(clientroot::makeControlTag(token, sizeof(token),
                                          clientroot::Direction::Claim, body,
                                          sizeof(body), tag));
    CHECK_TRUE(clientroot::verifyControlTag(token, sizeof(token),
                                            clientroot::Direction::Claim,
                                            body, sizeof(body), tag));
    CHECK_TRUE(!clientroot::verifyControlTag(
        token, sizeof(token), clientroot::Direction::Enrollment, body,
        sizeof(body), tag));
    uint8_t otherToken[qnet::kClientRootTokenBytes];
    fillToken(otherToken, 0x60u);
    CHECK_TRUE(!clientroot::verifyControlTag(
        otherToken, sizeof(otherToken), clientroot::Direction::Claim, body,
        sizeof(body), tag));
    uint8_t tampered[64];
    std::memcpy(tampered, body, sizeof(body));
    tampered[10] ^= 0x01u;
    CHECK_TRUE(!clientroot::verifyControlTag(
        token, sizeof(token), clientroot::Direction::Claim, tampered,
        sizeof(tampered), tag));
    CHECK_TRUE(!clientroot::verifyControlTag(
        token, sizeof(token), clientroot::Direction::Claim, body,
        sizeof(body) - 1u, tag));
    std::array<uint8_t, 16u> refused{};
    CHECK_TRUE(!clientroot::makeControlTag(token, sizeof(token) - 1u,
                                           clientroot::Direction::Claim,
                                           body, sizeof(body), refused));
    CHECK_TRUE(!clientroot::makeControlTag(token, sizeof(token),
                                           clientroot::Direction::Claim,
                                           body, 0u, refused));
}

void testClaimMappingValid() {
    const qnet::ClientRootClaim wire = makeValidClaimWire();
    qclient::spectator::topology::ClientPlayerRootClaim claim{};
    CHECK_TRUE(clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                         claim));
    CHECK_TRUE(claim.sessionId == kSessionId);
    CHECK_TRUE(claim.sessionEpoch == kSessionEpoch);
    CHECK_TRUE(claim.remotePlayerRole
               == qnetplay::spectator::resilient::wire::ClientPlayerRootRole::
                   RemotePlayerTwo);
    CHECK_TRUE(claim.publisherGeneration == 7u);
    CHECK_TRUE(claim.publisherNonce == 0xAB12CD34EF56ull);
    CHECK_TRUE(claim.runtimeReceipt.processId == 4242u);
    CHECK_TRUE(claim.runtimeReceipt.playerSlot == 1u);
    CHECK_TRUE(claim.runtimeReceipt.runtimeStage
               == qnetplay::config_provenance::Stage::RuntimeApplied);
    CHECK_TRUE(claim.runtimeReceipt.rollback == 7u);
    CHECK_TRUE(claim.runtimeReceipt.remoteRollback == 15u);
    CHECK_TRUE(claim.streamRootIdentity == 0x5354524D524F4F54ull);
    CHECK_TRUE(claim.gameStartFingerprint == 0x47414D4553544152ull);
    CHECK_TRUE(claim.sourceGeneration == 3u);
    CHECK_TRUE(claim.dataRoute.port == 41001u);
    CHECK_TRUE(claim.dataRoute.routeProved);
    CHECK_TRUE(claim.qsrRoute.port == 41002u);
    CHECK_TRUE(claim.cacheRange.firstCursor == 0u);
    CHECK_TRUE(claim.cacheRange.nextCursor == 704u);
    CHECK_TRUE(claim.cursor.nextRecord == 704u);
    CHECK_TRUE(claim.cursor.nextEvent == 12u);
    CHECK_TRUE(claim.cursor.digest == 0x44494745535431ull);
    CHECK_TRUE(claim.childCapacity == 4u);
    CHECK_TRUE(claim.expiresMs == 90000u);
}

void testClaimMappingNegatives() {
    qclient::spectator::topology::ClientPlayerRootClaim claim{};

    qnet::ClientRootClaim wire = makeValidClaimWire();
    wire.schemaVersion = 2u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    CHECK_TRUE(!clientroot::claimFromWire(wire, 1u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.remotePlayerRole = 3u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 3u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.sessionEpoch = kSessionEpoch + 1u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.runtimeStage = 6u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.cursorDigest = 0u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.requestNonce = 0u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.qsrRoute.port = wire.dataRoute.port;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.dataRoute.kind = 9u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.dataRoute.routeProved = 2u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.cursorNextRecord = wire.cacheNextCursor + 1u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));

    wire = makeValidClaimWire();
    wire.cacheNextCursor = 0u;
    wire.cursorNextRecord = 1u;
    CHECK_TRUE(!clientroot::claimFromWire(wire, 2u, kSessionId, kSessionEpoch,
                                          claim));
}

void testEnrollmentSerializerBounds() {
    qclient::spectator::topology::ClientPlayerRootEnrollment enrollment{};
    enrollment.granted = true;
    enrollment.clientRootNodeId = 0x11u;
    enrollment.hostRootNodeId = 0x22u;
    enrollment.authorityGeneration = 5u;
    enrollment.expiresMs = 120000u;
    for (size_t index = 0u; index < enrollment.credential.size(); ++index)
        enrollment.credential[index] = static_cast<uint8_t>(0x80u + index);
    const auto fillBinding =
        [](qclient::spectator::topology::EndpointBinding& binding,
           const uint16_t port) {
            binding.local.ipv4BE = 0x0100007Fu;
            binding.local.port = port;
            binding.advertised.kind =
                qnetplay::spectator::resilient::RouteKind::Direct;
            binding.advertised.ipv4BE = 0x0100007Fu;
            binding.advertised.port = port;
            binding.advertised.generation = 2u;
            binding.advertised.routeProved = true;
            binding.localProvenance =
                qclient::spectator::topology::EndpointProvenance::LocalBind;
            binding.advertisedProvenance = qclient::spectator::topology::
                EndpointProvenance::ProvenDirectQsp;
            binding.generation = 2u;
        };
    fillBinding(enrollment.rootQsp, 42001u);
    fillBinding(enrollment.rootQsr, 42002u);
    enrollment.hostSourceGeneration = 6u;
    for (size_t index = 0u;
         index < enrollment.assignmentSigningPublicKey.size(); ++index) {
        enrollment.assignmentSigningPublicKey[index] =
            static_cast<uint8_t>(index + 1u);
    }

    const uint8_t blob[48] = {0xAAu, 0xBBu, 0xCCu};
    qnet::ClientRootEnrollment wire{};
    CHECK_TRUE(clientroot::enrollmentToWire(0xC0FFEE01u, enrollment, blob,
                                            sizeof(blob), wire));
    CHECK_TRUE(wire.granted == 1u);
    CHECK_TRUE(wire.requestNonce == 0xC0FFEE01u);
    CHECK_TRUE(wire.clientRootNodeId == 0x11u);
    CHECK_TRUE(wire.grantLength == sizeof(blob));
    CHECK_TRUE(wire.topologyCredential[0] == 0x80u);
    CHECK_TRUE(wire.rootQsr.advertisedPort == 42002u);
    CHECK_TRUE(wire.rootQsp.advertisedPort == 42001u);
    CHECK_TRUE(wire.hostSourceGeneration == 6u);
    CHECK_TRUE(wire.authorityPublicKey[0] == 1u);

    std::array<uint8_t, qnet::kClientRootCredentialBytes> credential{};
    std::vector<uint8_t> grantBlob;
    CHECK_TRUE(clientroot::enrollmentFromWire(wire, 0xC0FFEE01u, credential,
                                              grantBlob));
    CHECK_TRUE(credential[1] == 0x81u);
    CHECK_TRUE(grantBlob.size() == sizeof(blob));
    CHECK_TRUE(grantBlob[2] == 0xCCu);

    CHECK_TRUE(!clientroot::enrollmentFromWire(wire, 0xC0FFEE02u, credential,
                                               grantBlob));
    qnet::ClientRootEnrollment refused = wire;
    refused.granted = 0u;
    CHECK_TRUE(!clientroot::enrollmentFromWire(refused, 0xC0FFEE01u,
                                               credential, grantBlob));
    qnet::ClientRootEnrollment collided = wire;
    collided.hostRootNodeId = collided.clientRootNodeId;
    CHECK_TRUE(!clientroot::enrollmentFromWire(collided, 0xC0FFEE01u,
                                               credential, grantBlob));
    qnet::ClientRootEnrollment oversized{};
    std::vector<uint8_t> huge(qnet::kClientRootGrantBlobMax + 1u, 0x55u);
    CHECK_TRUE(!clientroot::enrollmentToWire(0xC0FFEE01u, enrollment,
                                             huge.data(), huge.size(),
                                             oversized));
    qclient::spectator::topology::ClientPlayerRootEnrollment ungranted =
        enrollment;
    ungranted.granted = false;
    CHECK_TRUE(!clientroot::enrollmentToWire(0xC0FFEE01u, ungranted, blob,
                                             sizeof(blob), oversized));
    qclient::spectator::topology::ClientPlayerRootEnrollment missingQsr =
        enrollment;
    missingQsr.rootQsr = {};
    CHECK_TRUE(!clientroot::enrollmentToWire(0xC0FFEE01u, missingQsr, blob,
                                             sizeof(blob), oversized));
    qnet::ClientRootEnrollment strippedWire = wire;
    strippedWire.rootQsr = {};
    CHECK_TRUE(!clientroot::enrollmentFromWire(strippedWire, 0xC0FFEE01u,
                                               credential, grantBlob));
}

}

int main() {
    testTagSchemeRoundTripAndDomains();
    testClaimMappingValid();
    testClaimMappingNegatives();
    testEnrollmentSerializerBounds();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d client-root control check(s) failed\n",
                     g_failures);
        return 1;
    }
    std::printf("released client-root control checks passed\n");
    return 0;
}
