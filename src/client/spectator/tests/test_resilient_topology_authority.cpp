#include "../ResilientTopologyAuthority.h"

#include "../../../net/Sockets.h"
#include "../../../netplay/WireProtocol.h"
#include "../../../spectator/ResilientSpectatorProtocol.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

namespace topology = qclient::spectator::topology;
namespace resilient = qclient::spectator::resilient;
namespace model = qnetplay::spectator::resilient;
namespace wire = qnetplay::spectator::resilient::wire;
namespace compat = qnetplay::compat;

using qclient::spectator::RouteEndpoint;
using qnetplay::spectator::SessionCookie;
using qnetplay::spectator::SessionIdentity;

constexpr uint64_t kSessionId = 0x1122334455667788ull;
constexpr uint64_t kEpoch = 0x8877665544332211ull;
constexpr uint64_t kStreamRootIdentity = 0xAABBCCDDEEFF0011ull;
constexpr uint64_t kSourceGeneration = 17u;
constexpr uint64_t kNowMs = 1000u;
constexpr uint64_t kRootRouteGeneration = 9u;
constexpr uint64_t kInitialQtaDigest = 0xAABBCCDDEEFF1122ull;

constexpr size_t kAuthorityGenerationOffset = 12u;
constexpr size_t kNodeIdOffset = 20u;
constexpr size_t kMacOffset = 36u;
constexpr size_t kTopologyEnvelopePrefixBytes = 52u;

int g_failures = 0;

#define CHECK_TRUE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::fprintf(stderr, "%s:%d CHECK failed: %s\n",              \
                         __FILE__, __LINE__, #expr);                        \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

#define CHECK_EQ(left, right)                                                \
    do {                                                                     \
        const auto checkLeft = (left);                                       \
        const auto checkRight = (right);                                     \
        if (!(checkLeft == checkRight)) {                                    \
            std::fprintf(stderr, "%s:%d CHECK_EQ failed: %s != %s\n",     \
                         __FILE__, __LINE__, #left, #right);                \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

compat::CompatibilityIdentityWire compatibilityIdentity() {
    compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion = compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    if (!compat::setProfileId(identity, "qoh99-se1126")) return {};
    for (size_t index = 0u; index < compat::kCompatibilitySha256Bytes;
         ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(0x10u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(0x50u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<uint8_t>(0x90u + index);
    }
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.stateSchemaId = 0x5354415445000001ull;
    identity.modSimulationId = 0x4D4F4453494D0001ull;
    identity.timingProfileId = 0x54494D494E470001ull;
    return identity;
}

SessionIdentity sessionIdentity(const uint64_t sessionId = kSessionId,
                                const uint64_t epoch = kEpoch) {
    return SessionIdentity{
        sessionId,
        epoch,
        SessionCookie{0x1020304050607080ull, 0x8090A0B0C0D0E0F0ull},
    };
}

topology::EndpointBinding endpointBinding(const uint16_t localPort,
                                          const uint16_t advertisedPort,
                                          const uint64_t generation) {
    topology::EndpointBinding binding{};
    binding.local = RouteEndpoint{0u, localPort};
    binding.advertised.kind = model::RouteKind::Direct;
    binding.advertised.ipv4BE = 0x0100007Fu;
    binding.advertised.port = advertisedPort;
    binding.advertised.generation = generation;
    binding.advertised.routeProved = true;
    binding.localProvenance = topology::EndpointProvenance::LocalBind;
    binding.advertisedProvenance =
        topology::EndpointProvenance::PublishedDirectCandidate;
    binding.generation = generation;
    return binding;
}

topology::TopologyActivationRequest activationRequest() {
    topology::TopologyActivationRequest request{};
    request.requested = true;
    request.rootQsp = endpointBinding(43001u, 43001u, kRootRouteGeneration);
    request.sourceGeneration = kSourceGeneration;
    return request;
}

topology::EndpointBinding qsrBinding() {
    return endpointBinding(43002u, 43002u, kRootRouteGeneration);
}

topology::TopologyViewerProof viewerProof(const uint64_t viewerId,
                                          const uint16_t port,
                                          const uint32_t generation = 1u,
                                          const uint64_t routeGeneration = 3u,
                                          const uint32_t cursor = 24u) {
    topology::TopologyViewerProof proof{};
    proof.viewerId = viewerId;
    proof.viewerGeneration = generation;
    proof.endpoint = RouteEndpoint{0x0100007Fu, port};
    proof.provenance = topology::EndpointProvenance::ProvenPunchedQsp;
    proof.routeGeneration = routeGeneration;
    proof.confirmedCursor = cursor;
    proof.qspStreaming = true;
    proof.relayCapable = true;
    return proof;
}

bool initializeAuthority(topology::TopologyAuthority& authority,
                         topology::TopologyActivationRequest request =
                             activationRequest(),
                         const SessionIdentity& session = sessionIdentity()) {
    std::string error;
    const bool initialized = authority.initialize(
        session, compatibilityIdentity(), kStreamRootIdentity, request,
        qsrBinding(), kNowMs, error);
    if (initialized) {
        authority.setVerifiedCursor(
            model::VerifiedChainCursor{0u, 0u, kInitialQtaDigest});
    }
    return initialized;
}

bool encodeAdvertisement(const model::NodeAdvertisement& advertisement,
                         std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::NodeAdvertisement;
    datagram.nodeAdvertisement = advertisement;
    return wire::encodeControlDatagram(datagram, out);
}

bool makeInitialQsrAdvertisement(
    const topology::TopologyEnrollment& enrollment,
    const RouteEndpoint& qsrEndpoint, model::NodeAdvertisement& out) {
    out = {};
    if (!enrollment.granted || enrollment.nodeId == model::kInvalidNodeId
        || !qsrEndpoint.valid()
        || qsrEndpoint.port == enrollment.qspEndpoint.port
        || !model::makeStreamCompatibilityIdentity(
            enrollment.compatibility, enrollment.session.sessionId,
            enrollment.session.epoch, enrollment.streamRootIdentity,
            out.stream)) {
        return false;
    }
    out.nodeId = enrollment.nodeId;
    out.kind = model::NodeKind::SpectatorRelay;
    out.route.kind = enrollment.qspProvenance
            == topology::EndpointProvenance::ProvenPunchedQsp
        ? model::RouteKind::Punched : model::RouteKind::Direct;
    out.route.ipv4BE = enrollment.qspEndpoint.ipv4BE;
    out.route.port = enrollment.qspEndpoint.port;
    out.route.generation = enrollment.qspRouteGeneration;
    out.route.routeProved = true;
    out.controlRoute.kind = model::RouteKind::Direct;
    out.controlRoute.ipv4BE = qsrEndpoint.ipv4BE;
    out.controlRoute.port = qsrEndpoint.port;
    out.controlRoute.generation = enrollment.qspRouteGeneration;
    out.controlRoute.routeProved = true;
    out.capabilityModelRevision = model::kResilientCapabilityModelRevision;
    out.capabilities = model::kCapabilityRelayCapableSpectator
        | model::kCapabilityTopologyLeaf;
    out.childCapacity = 0u;
    out.childCount = 0u;
    out.uploadCapacity = 1u;
    out.uploadInUse = 0u;
    out.cacheFirstCursor = enrollment.qtaCursor.nextRecord;
    out.cacheNextCursor = enrollment.qtaCursor.nextRecord;
    out.sourceGeneration = enrollment.sourceGeneration;
    out.routeGeneration = enrollment.qspRouteGeneration;
    out.lastProgressMs = kNowMs;
    out.leaseExpiryMs = enrollment.expiresMs;
    out.reachable = true;
    out.draining = false;
    return true;
}

bool bindDistinctQsr(
    topology::TopologyAuthority& authority,
    const topology::TopologyEnrollment& enrollment,
    const RouteEndpoint& qsrEndpoint, const uint64_t sequence,
    const uint64_t nowMs, topology::TopologyControlEnvelope* decoded = nullptr,
    model::NodeAdvertisement* advertisement = nullptr) {
    model::NodeAdvertisement local{};
    std::vector<uint8_t> qsr;
    std::vector<uint8_t> envelope;
    topology::TopologyControlEnvelope received{};
    if (!makeInitialQsrAdvertisement(enrollment, qsrEndpoint, local)
        || !encodeAdvertisement(local, qsr)
        || !authority.encodeControl(enrollment, sequence, qsr.data(), qsr.size(),
                                    envelope)
        || authority.decodeControl(qsrEndpoint, envelope.data(), envelope.size(),
                                   nowMs, received)
            != topology::TopologyControlResult::Accepted) {
        return false;
    }
    if (decoded != nullptr) *decoded = received;
    if (advertisement != nullptr) *advertisement = local;
    return true;
}

model::ParentLease leaseFor(const topology::TopologyAuthority& authority,
                            const topology::TopologyEnrollment& enrollment,
                            const uint64_t nonce, const uint64_t expiryMs) {
    model::ParentLease lease{};
    const topology::TopologyAuthorityFacts& facts = authority.facts();
    (void)model::makeStreamCompatibilityIdentity(
        facts.compatibility, facts.session.sessionId, facts.session.epoch,
        facts.streamRootIdentity, lease.stream);
    lease.childId = enrollment.nodeId;
    lease.parentId = facts.rootNodeId;
    (void)authority.parentLeaseChildBinding(enrollment.nodeId,
                                            lease.childBinding);
    lease.parentDepth = 0u;
    lease.parentSourceGeneration = facts.sourceGeneration;
    lease.routeGeneration = facts.rootQsp.generation;
    lease.routes[0] = facts.rootQsp.advertised;
    lease.routeCount = 1u;
    lease.cacheRange = model::VerifiedCacheRange{0u, 50u};
    lease.latestVerifiedCursor = lease.cacheRange.nextCursor;
    lease.childCapacity = 1u;
    lease.expiryMs = expiryMs;
    lease.replayNonce = nonce;
    lease.ancestry[0] = facts.rootNodeId;
    lease.ancestryCount = 1u;
    model::ParentLeaseRegistry signer(authority.leaseKeyForRuntime());
    if (!signer.issue(lease)) return {};
    return lease;
}

model::LeaseBinding bindingFor(const model::ParentLease& lease) {
    return model::LeaseBinding{
        lease.childId, lease.parentId, lease.parentSourceGeneration,
        lease.routeGeneration, lease.expiryMs, lease.replayNonce,
        lease.macLow, lease.macHigh, lease.childBinding};
}

wire::RelayDataBindingRequest relayDataBindingRequest(
    const topology::TopologyAuthority& authority,
    const topology::TopologyEnrollment& enrollment,
    const model::ParentLease& lease, const uint64_t requestNonce) {
    wire::RelayDataBindingRequest request{};
    const topology::TopologyAuthorityFacts& facts = authority.facts();
    (void)model::makeStreamCompatibilityIdentity(
        facts.compatibility, facts.session.sessionId, facts.session.epoch,
        facts.streamRootIdentity, request.stream);
    request.authorityGeneration = facts.authorityGeneration;
    request.rootNodeId = facts.rootNodeId;
    request.childNodeId = enrollment.nodeId;
    request.lease = bindingFor(lease);
    request.requestNonce = requestNonce;
    return request;
}

bool encodeRelayDataBindingRequest(
    const wire::RelayDataBindingRequest& request, std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::RelayDataBindingRequest;
    datagram.relayDataBindingRequest = request;
    return wire::encodeControlDatagram(datagram, out);
}

wire::RelayReady relayReady(
    const wire::RelayDataBindingRequest& request,
    const wire::RelayDataBindingAck& ack,
    const model::VerifiedCacheRange& cacheRange,
    const model::VerifiedChainCursor& cacheCursor) {
    wire::RelayReady ready{};
    ready.stream = request.stream;
    ready.authorityGeneration = request.authorityGeneration;
    ready.rootNodeId = request.rootNodeId;
    ready.relayNodeId = request.childNodeId;
    ready.relayLease = request.lease;
    ready.bindingRequestNonce = request.requestNonce;
    ready.observedDataRoute = ack.observedDataRoute;
    ready.observedControlRoute = ack.observedControlRoute;
    ready.cacheRange = cacheRange;
    ready.cacheCursor = cacheCursor;
    return ready;
}

bool encodeRelayReady(const wire::RelayReady& ready,
                      std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::RelayReady;
    datagram.relayReady = ready;
    return wire::encodeControlDatagram(datagram, out);
}

wire::ClientDataRootAssertion clientDataRootAssertion(
    const topology::TopologyAuthority& authority,
    const topology::TopologyEnrollment& enrollment,
    const wire::RelayDataBindingAck& binding,
    const wire::RelayReady& ready, const uint32_t childCapacity) {
    wire::ClientDataRootAssertion assertion{};
    assertion.stream = ready.stream;
    assertion.gameStartFingerprint = authority.facts().gameStartFingerprint;
    assertion.authorityGeneration = authority.facts().authorityGeneration;
    assertion.clientRootNodeId = enrollment.nodeId;
    assertion.qspRouteGeneration = enrollment.qspRouteGeneration;
    assertion.qsrBindingGeneration = enrollment.bindingGeneration;
    assertion.sourceGeneration = authority.facts().sourceGeneration;
    assertion.dataRoute = binding.observedDataRoute;
    assertion.qsrRoute = binding.observedControlRoute;
    assertion.cacheRange = ready.cacheRange;
    assertion.cursor = ready.cacheCursor;
    assertion.childCapacity = childCapacity;
    return assertion;
}

bool encodeClientDataRootAssertion(
    const wire::ClientDataRootAssertion& assertion,
    std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::ClientDataRootAssertion;
    datagram.clientDataRootAssertion = assertion;
    return wire::encodeControlDatagram(datagram, out);
}

bool encodeRelayChildActivationReceipt(
    const wire::RelayChildActivationReceipt& receipt,
    std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::RelayChildActivationReceipt;
    datagram.relayChildActivationReceipt = receipt;
    return wire::encodeControlDatagram(datagram, out);
}

bool encodeRelayChildActivationRetirement(
    const wire::RelayChildActivationRetirement& retirement,
    std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::RelayChildActivationRetirement;
    datagram.relayChildActivationRetirement = retirement;
    return wire::encodeControlDatagram(datagram, out);
}

bool encodeRelayChildActivationCommit(
    const wire::RelayChildActivationCommit& commit,
    std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::RelayChildActivationCommit;
    datagram.relayChildActivationCommit = commit;
    return wire::encodeControlDatagram(datagram, out);
}

topology::ClientPlayerRootClaim clientPlayerRootClaim(
    const topology::TopologyAuthority& authority,
    const wire::ClientPlayerRootRole role, const uint16_t dataPort,
    const uint16_t qsrPort, const uint64_t sourceGeneration =
        kSourceGeneration) {
    using qnetplay::config_provenance::Stage;
    const topology::TopologyAuthorityFacts& facts = authority.facts();
    topology::ClientPlayerRootClaim claim{};
    claim.sessionId = facts.session.sessionId;
    claim.sessionEpoch = facts.session.epoch;
    claim.remotePlayerRole = role;
    claim.compatibility = facts.compatibility;
    claim.configurationProvenance =
        qnetplay::config_provenance::makeCommittedWire(
            Stage::Default, Stage::LocallySelected, Stage::CasterAdvertised,
            Stage::PeerAdvertised);
    claim.streamRootIdentity = facts.streamRootIdentity;
    claim.gameStartFingerprint = facts.gameStartFingerprint;
    claim.publisherGeneration = 5u;
    claim.publisherNonce = 0xC11E170000000001ull
        + static_cast<uint64_t>(role);
    claim.runtimeReceipt.processId = 4242u;
    claim.runtimeReceipt.publisherGeneration = claim.publisherGeneration;
    claim.runtimeReceipt.publisherNonce = claim.publisherNonce;
    claim.runtimeReceipt.playerSlot = static_cast<uint8_t>(role);
    claim.runtimeReceipt.runtimeStage = Stage::RuntimeApplied;
    claim.runtimeReceipt.delay = 3u;
    claim.runtimeReceipt.rollback = 7u;
    claim.runtimeReceipt.remoteDelay = 4u;
    claim.runtimeReceipt.remoteRollback = 6u;
    claim.runtimeReceipt.provenance = claim.configurationProvenance;
    claim.sourceGeneration = sourceGeneration;
    claim.qspRouteGeneration = 21u;
    claim.qsrBindingGeneration = 22u;
    claim.dataRoute = {model::RouteKind::Direct, 0x0100007Fu, dataPort,
                       claim.qspRouteGeneration, true};
    claim.qsrRoute = {model::RouteKind::Direct, 0x0100007Fu, qsrPort,
                      claim.qsrBindingGeneration, true};
    claim.cacheRange = {0u, 0u};
    claim.cursor = {0u, 0u, kInitialQtaDigest};
    claim.childCapacity = 1u;
    claim.expiresMs = kNowMs + 5000u;
    return claim;
}

bool encodeClientPlayerRootAssertion(
    const wire::ClientPlayerRootAssertion& assertion,
    std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::ClientPlayerRootAssertion;
    datagram.clientPlayerRootAssertion = assertion;
    return wire::encodeControlDatagram(datagram, out);
}

bool encodeClientPlayerRootActivationReceipt(
    const wire::ClientPlayerRootActivationReceipt& receipt,
    std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type =
        wire::ControlMessageType::ClientPlayerRootActivationReceipt;
    datagram.clientPlayerRootActivationReceipt = receipt;
    return wire::encodeControlDatagram(datagram, out);
}

bool encodeClientPlayerRootActivationCommit(
    const wire::ClientPlayerRootActivationCommit& commit,
    std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type =
        wire::ControlMessageType::ClientPlayerRootActivationCommit;
    datagram.clientPlayerRootActivationCommit = commit;
    return wire::encodeControlDatagram(datagram, out);
}

bool encodeClientPlayerRootActivationRetirement(
    const wire::ClientPlayerRootActivationRetirement& retirement,
    std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type =
        wire::ControlMessageType::ClientPlayerRootActivationRetirement;
    datagram.clientPlayerRootActivationRetirement = retirement;
    return wire::encodeControlDatagram(datagram, out);
}

void testAuthorityExportsOnlyExactBoundChildLeaseFacts() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    const topology::TopologyViewerProof proof =
        viewerProof(0xA010000000000001ull, 45010u, 4u, 13u, 61u);
    topology::TopologyEnrollment enrollment{};
    std::string error;
    CHECK_TRUE(authority.enroll(proof, kNowMs + 1u, enrollment, error));
    if (!enrollment.granted) return;

    model::ParentLeaseChildBinding binding{};
    CHECK_TRUE(!authority.parentLeaseChildBinding(enrollment.nodeId, binding));

    const RouteEndpoint qsrEndpoint{0x0100007Fu, 46010u};
    CHECK_TRUE(bindDistinctQsr(authority, enrollment, qsrEndpoint, 1u,
                               kNowMs + 2u));
    CHECK_TRUE(authority.parentLeaseChildBinding(enrollment.nodeId, binding));
    CHECK_TRUE(binding.valid());
    CHECK_EQ(binding.authorityGeneration,
             authority.facts().authorityGeneration);
    CHECK_EQ(binding.childViewerId, proof.viewerId);
    CHECK_EQ(binding.childViewerGeneration, proof.viewerGeneration);
    CHECK_EQ(binding.childQspRoute.kind, model::RouteKind::Punched);
    CHECK_EQ(binding.childQspRoute.ipv4BE, proof.endpoint.ipv4BE);
    CHECK_EQ(binding.childQspRoute.port, proof.endpoint.port);
    CHECK_EQ(binding.childQspRoute.generation, proof.routeGeneration);
    CHECK_TRUE(binding.childQspRoute.routeProved);
    CHECK_EQ(binding.childEndpointBindingGeneration,
             enrollment.bindingGeneration);
    CHECK_EQ(binding.childRequestedCursor.nextRecord,
             enrollment.qtaCursor.nextRecord);
    CHECK_EQ(binding.childRequestedCursor.nextEvent,
             enrollment.qtaCursor.nextEvent);
    CHECK_EQ(binding.childRequestedCursor.digest,
             enrollment.qtaCursor.digest);

    model::NodeAdvertisement advertisement{};
    CHECK_TRUE(authority.childAdvertisement(enrollment.nodeId, advertisement));
    CHECK_TRUE(binding.childQspRoute.sameEndpoint(advertisement.route));
    CHECK_TRUE(!binding.childQspRoute.sameEndpoint(
        advertisement.resolvedControlRoute()));

    model::ParentLeaseChildBinding absent{};
    CHECK_TRUE(!authority.parentLeaseChildBinding(
        enrollment.nodeId + 1u, absent));
    CHECK_TRUE(!absent.valid());
}

void testAuthorityRejectsLeaseBindingWithoutVerifiedCacheGenesis() {
    topology::TopologyAuthority authority{};
    std::string error;
    CHECK_TRUE(authority.initialize(sessionIdentity(), compatibilityIdentity(),
                                    kStreamRootIdentity, activationRequest(),
                                    qsrBinding(), kNowMs, error));
    if (!authority.ready()) return;

    const topology::TopologyViewerProof proof =
        viewerProof(0xA020000000000001ull, 45020u, 1u, 14u, 62u);
    topology::TopologyEnrollment enrollment{};
    CHECK_TRUE(authority.enroll(proof, kNowMs + 1u, enrollment, error));
    if (!enrollment.granted) return;
    CHECK_TRUE(bindDistinctQsr(authority, enrollment,
                               RouteEndpoint{0x0100007Fu, 46020u}, 1u,
                               kNowMs + 2u));

    model::ParentLeaseChildBinding binding{};
    CHECK_TRUE(!authority.parentLeaseChildBinding(enrollment.nodeId, binding));
    CHECK_TRUE(!binding.valid());

    authority.setVerifiedCursor(
        model::VerifiedChainCursor{0u, 0u, kInitialQtaDigest});
    CHECK_TRUE(!authority.parentLeaseChildBinding(enrollment.nodeId, binding));
}

void testAuthorityAllocatesBoundUniqueNodeIds() {
    topology::TopologyAuthority authority{};
    CHECK_EQ(authority.diagnostics(), std::string("topology=disabled"));
    CHECK_TRUE(initializeAuthority(authority));
    CHECK_TRUE(authority.ready());
    if (!authority.ready()) return;

    const topology::TopologyAuthorityFacts& facts = authority.facts();
    CHECK_TRUE(facts.complete());
    CHECK_TRUE(facts.rootNodeId != model::kInvalidNodeId);
    CHECK_EQ(facts.sourceGeneration, kSourceGeneration);
    CHECK_EQ(facts.rootQsp.generation, kRootRouteGeneration);
    CHECK_EQ(facts.rootQsr.generation, kRootRouteGeneration);
    authority.setVerifiedCursor(model::VerifiedChainCursor{12u, 3u,
                                                            0xAABBCCDDu});

    const topology::TopologyViewerProof firstProof =
        viewerProof(0xA100000000000001ull, 45101u, 1u, 3u, 24u);
    const topology::TopologyViewerProof secondProof =
        viewerProof(0xA100000000000002ull, 45102u, 1u, 4u, 25u);
    const RouteEndpoint firstQsr{0x0100007Fu, 46101u};
    topology::TopologyEnrollment first{};
    topology::TopologyEnrollment second{};
    std::string error;
    CHECK_TRUE(authority.enroll(firstProof, kNowMs + 1u, first, error));
    CHECK_TRUE(authority.enroll(secondProof, kNowMs + 1u, second, error));
    CHECK_TRUE(first.granted);
    CHECK_TRUE(second.granted);
    CHECK_TRUE(first.nodeId != model::kInvalidNodeId);
    CHECK_TRUE(first.nodeId != facts.rootNodeId);
    CHECK_TRUE(second.nodeId != facts.rootNodeId);
    CHECK_TRUE(first.nodeId != second.nodeId);
    CHECK_EQ(first.rootNodeId, facts.rootNodeId);
    CHECK_EQ(second.rootNodeId, facts.rootNodeId);
    CHECK_EQ(first.authorityGeneration, facts.authorityGeneration);
    CHECK_EQ(first.bindingGeneration, 1u);
    CHECK_EQ(first.session.sessionId, facts.session.sessionId);
    CHECK_EQ(first.session.epoch, facts.session.epoch);
    CHECK_EQ(first.session.cookie.low, facts.session.cookie.low);
    CHECK_EQ(first.session.cookie.high, facts.session.cookie.high);
    CHECK_TRUE(compat::semanticallyEqual(first.compatibility,
                                         facts.compatibility));
    CHECK_EQ(first.streamRootIdentity, facts.streamRootIdentity);
    CHECK_EQ(first.sourceGeneration, facts.sourceGeneration);
    CHECK_EQ(first.viewerId, firstProof.viewerId);
    CHECK_EQ(first.viewerGeneration, firstProof.viewerGeneration);
    CHECK_EQ(first.qspEndpoint, firstProof.endpoint);
    CHECK_EQ(first.qspRouteGeneration, firstProof.routeGeneration);
    CHECK_EQ(first.confirmedCursor, firstProof.confirmedCursor);
    CHECK_EQ(first.qtaCursor.nextRecord, 12u);
    CHECK_EQ(first.qtaCursor.nextEvent, 3u);
    CHECK_EQ(first.qtaCursor.digest, 0xAABBCCDDu);
    CHECK_TRUE((first.credential
                != std::array<uint8_t,
                              topology::kTopologyCredentialBytes>{}));
    CHECK_EQ(authority.enrolledCount(), static_cast<size_t>(2u));
    CHECK_EQ(authority.requiredCursor(first.nodeId), first.qtaCursor.nextRecord);

    topology::TopologyEnrollment byEndpoint{};
    topology::TopologyEnrollment byNode{};
    CHECK_TRUE(authority.enrollmentForEndpoint(firstProof.endpoint, byEndpoint));
    CHECK_TRUE(authority.enrollmentForNode(first.nodeId, byNode));
    CHECK_EQ(byEndpoint.nodeId, first.nodeId);
    CHECK_EQ(byNode.nodeId, first.nodeId);
    CHECK_EQ(byNode.rootNodeId, facts.rootNodeId);
    CHECK_TRUE(std::memcmp(byNode.credential.data(), first.credential.data(),
                           first.credential.size())
               == 0);

    model::NodeAdvertisement advertisement{};
    CHECK_TRUE(!authority.childAdvertisement(first.nodeId, advertisement));
    CHECK_TRUE(!authority.enrollmentForQsrEndpoint(firstQsr, byEndpoint));
    CHECK_TRUE(bindDistinctQsr(authority, first, firstQsr, 1u, kNowMs + 2u));
    CHECK_TRUE(!bindDistinctQsr(authority, second, firstQsr, 1u,
                                kNowMs + 2u));
    CHECK_TRUE(authority.enrollmentForQsrEndpoint(firstQsr, byEndpoint));
    CHECK_EQ(byEndpoint.nodeId, first.nodeId);
    CHECK_TRUE(authority.childAdvertisement(first.nodeId, advertisement));
    CHECK_EQ(advertisement.nodeId, first.nodeId);
    CHECK_EQ(advertisement.kind, model::NodeKind::SpectatorRelay);
    CHECK_EQ(advertisement.route.kind, model::RouteKind::Punched);
    CHECK_EQ(advertisement.route.ipv4BE, firstProof.endpoint.ipv4BE);
    CHECK_EQ(advertisement.route.port, firstProof.endpoint.port);
    CHECK_EQ(advertisement.route.generation, firstProof.routeGeneration);
    CHECK_TRUE(advertisement.route.routeProved);
    CHECK_EQ(advertisement.controlRoute.kind, model::RouteKind::Direct);
    CHECK_EQ(advertisement.controlRoute.ipv4BE, firstQsr.ipv4BE);
    CHECK_EQ(advertisement.controlRoute.port, firstQsr.port);
    CHECK_EQ(advertisement.controlRoute.generation,
             firstProof.routeGeneration);
    CHECK_TRUE(advertisement.controlRoute.routeProved);
    CHECK_EQ(advertisement.childCapacity, static_cast<size_t>(0u));
    CHECK_EQ(advertisement.childCount, static_cast<size_t>(0u));
    CHECK_TRUE((advertisement.capabilities & model::kCapabilityTopologyLeaf)
               != 0u);
    CHECK_EQ(advertisement.sourceGeneration, facts.sourceGeneration);
    CHECK_EQ(advertisement.routeGeneration, firstProof.routeGeneration);

    model::ResilientTopologyCoordinator coordinator(advertisement.stream,
                                                     facts.limits);
    model::NodeAdvertisement root = advertisement;
    root.nodeId = facts.rootNodeId;
    root.kind = model::NodeKind::HostRoot;
    root.route = facts.rootQsp.advertised;
    root.controlRoute = facts.rootQsr.advertised;
    root.routeGeneration = facts.rootQsp.generation;
    root.capabilities = 0u;
    root.childCapacity = 1u;
    root.reachable = false;
    model::NodeAdvertisement downstream = advertisement;
    downstream.nodeId = second.nodeId;
    ++downstream.route.port;
    ++downstream.controlRoute.port;
    downstream.capabilities = model::kCapabilityRelayCapableSpectator;
    CHECK_EQ(coordinator.registerNode(root),
             model::NodeRegistrationResult::Accepted);
    CHECK_EQ(coordinator.registerNode(advertisement),
             model::NodeRegistrationResult::Accepted);
    CHECK_EQ(coordinator.registerNode(downstream),
             model::NodeRegistrationResult::Accepted);
    model::ParentSelectionRequest request{};
    request.childId = downstream.nodeId;
    request.requiredCursor = advertisement.cacheNextCursor;
    request.minimumSourceGeneration = advertisement.sourceGeneration;
    request.minimumRouteGeneration = advertisement.routeGeneration;
    request.maxOffers = 1u;
    CHECK_TRUE(coordinator.selectStandbyParents(request).empty());
    CHECK_EQ(coordinator.attachChild(advertisement.nodeId, downstream.nodeId),
             model::TopologyAttachResult::ParentAtCapacity);

    const std::string diagnostics = authority.diagnostics();
    CHECK_TRUE(diagnostics.find("topology=ready authority=") != std::string::npos);
    CHECK_TRUE(diagnostics.find("root=") != std::string::npos);
    CHECK_TRUE(diagnostics.find("enrolled=2") != std::string::npos);
    CHECK_TRUE(diagnostics.find("reserved=0") != std::string::npos);
    CHECK_TRUE(diagnostics.find("credential") == std::string::npos);
}

void testAuthorityRejectsIncompleteOrCollidingRootFacts() {
    std::string error;
    topology::TopologyAuthority authority{};

    topology::TopologyActivationRequest unproved = activationRequest();
    unproved.rootQsp.advertised.routeProved = false;
    CHECK_TRUE(!authority.initialize(sessionIdentity(), compatibilityIdentity(),
                                     kStreamRootIdentity, unproved,
                                     qsrBinding(), kNowMs, error));
    CHECK_TRUE(!authority.ready());

    topology::TopologyActivationRequest missingEndpoint = activationRequest();
    missingEndpoint.rootQsp.advertised.port = 0u;
    CHECK_TRUE(!authority.initialize(sessionIdentity(), compatibilityIdentity(),
                                     kStreamRootIdentity, missingEndpoint,
                                     qsrBinding(), kNowMs, error));
    CHECK_TRUE(!authority.ready());

    topology::EndpointBinding collidingQsr = qsrBinding();
    collidingQsr.local.port = missingEndpoint.rootQsp.local.port;
    collidingQsr.advertised.port = missingEndpoint.rootQsp.advertised.port;
    topology::TopologyActivationRequest valid = activationRequest();
    collidingQsr.local.port = valid.rootQsp.local.port;
    collidingQsr.advertised.port = valid.rootQsp.advertised.port;
    CHECK_TRUE(!authority.initialize(sessionIdentity(), compatibilityIdentity(),
                                     kStreamRootIdentity, valid, collidingQsr,
                                     kNowMs, error));
    CHECK_TRUE(!authority.ready());
}

void testAuthorityRejectsUnprovenDuplicateAndStaleBindings() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    topology::TopologyViewerProof unproven =
        viewerProof(0xA200000000000001ull, 45201u);
    unproven.provenance =
        topology::EndpointProvenance::PublishedDirectCandidate;
    topology::TopologyEnrollment rejected{};
    std::string error;
    CHECK_TRUE(!authority.enroll(unproven, kNowMs + 1u, rejected, error));
    CHECK_TRUE(!rejected.granted);
    CHECK_EQ(rejected.nodeId, model::kInvalidNodeId);
    CHECK_EQ(authority.enrolledCount(), static_cast<size_t>(0u));

    const topology::TopologyViewerProof original =
        viewerProof(0xA200000000000002ull, 45202u, 1u, 5u, 31u);
    topology::TopologyEnrollment enrollment{};
    CHECK_TRUE(authority.enroll(original, kNowMs + 1u, enrollment, error));
    CHECK_TRUE(enrollment.granted);

    topology::TopologyViewerProof duplicateViewer = original;
    duplicateViewer.endpoint.port = 45203u;
    duplicateViewer.viewerGeneration = 2u;
    duplicateViewer.routeGeneration = 6u;
    CHECK_TRUE(!authority.enroll(duplicateViewer, kNowMs + 2u, rejected, error));

    topology::TopologyViewerProof duplicateEndpoint =
        viewerProof(0xA200000000000003ull, original.endpoint.port, 1u, 6u);
    CHECK_TRUE(!authority.enroll(duplicateEndpoint, kNowMs + 2u, rejected, error));
    CHECK_EQ(authority.enrolledCount(), static_cast<size_t>(1u));

    topology::TopologyViewerProof stale = original;
    stale.endpoint.port = 45204u;
    stale.viewerGeneration = original.viewerGeneration + 1u;
    CHECK_TRUE(!authority.refreshViewerBinding(stale, kNowMs + 3u, error));

    topology::TopologyViewerProof replacement = original;
    replacement.endpoint.port = 45204u;
    replacement.viewerGeneration = original.viewerGeneration + 1u;
    replacement.routeGeneration = original.routeGeneration + 1u;
    replacement.confirmedCursor = 77u;
    CHECK_TRUE(authority.refreshViewerBinding(replacement, kNowMs + 4u, error));
    CHECK_EQ(authority.requiredCursor(enrollment.nodeId),
             enrollment.qtaCursor.nextRecord);

    topology::TopologyEnrollment refreshed{};
    CHECK_TRUE(authority.enrollmentForNode(enrollment.nodeId, refreshed));
    CHECK_EQ(refreshed.bindingGeneration, 2u);
    CHECK_TRUE(!authority.enrollmentForEndpoint(original.endpoint, rejected));
    CHECK_TRUE(authority.enrollmentForEndpoint(replacement.endpoint, rejected));
    CHECK_EQ(rejected.nodeId, enrollment.nodeId);
}

void testAuthorityRejectsCrossLaneEndpointAliases() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    std::string error;
    topology::TopologyEnrollment rejected{};
    CHECK_TRUE(!authority.enroll(
        viewerProof(0xA350000000000001ull, 43001u), kNowMs + 1u,
        rejected, error));
    CHECK_TRUE(!authority.enroll(
        viewerProof(0xA350000000000002ull, 43002u), kNowMs + 1u,
        rejected, error));

    topology::TopologyEnrollment first{};
    const topology::TopologyViewerProof firstProof =
        viewerProof(0xA350000000000003ull, 43503u, 1u, 9u);
    CHECK_TRUE(authority.enroll(firstProof, kNowMs + 2u, first, error));
    if (!first.granted) return;
    const RouteEndpoint firstQsr{0x0100007Fu, 44503u};
    CHECK_TRUE(bindDistinctQsr(authority, first, firstQsr, 1u,
                               kNowMs + 3u));

    CHECK_TRUE(!authority.enroll(
        viewerProof(0xA350000000000004ull, firstQsr.port), kNowMs + 4u,
        rejected, error));

    topology::TopologyEnrollment second{};
    const topology::TopologyViewerProof secondProof =
        viewerProof(0xA350000000000005ull, 43505u, 1u, 11u);
    CHECK_TRUE(authority.enroll(secondProof, kNowMs + 4u, second, error));
    if (!second.granted) return;

    topology::TopologyViewerProof collidingRebind = secondProof;
    collidingRebind.viewerGeneration = 2u;
    collidingRebind.routeGeneration = 12u;
    collidingRebind.endpoint = firstQsr;
    CHECK_TRUE(!authority.beginViewerRebind(collidingRebind, kNowMs + 5u,
                                            error));

    collidingRebind.endpoint = RouteEndpoint{0x0100007Fu, 43001u};
    CHECK_TRUE(!authority.refreshViewerBinding(collidingRebind, kNowMs + 5u,
                                               error));

    topology::TopologyEnrollment preserved{};
    CHECK_TRUE(authority.enrollmentForNode(second.nodeId, preserved));
    CHECK_EQ(preserved.qspEndpoint.port, secondProof.endpoint.port);
    CHECK_EQ(preserved.viewerGeneration, secondProof.viewerGeneration);
    CHECK_EQ(preserved.qspRouteGeneration, secondProof.routeGeneration);
}

void testAuthorityRebindPreservesNodeAndWaitsForQspFinalize() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    const topology::TopologyViewerProof original =
        viewerProof(0xA250000000000001ull, 45251u, 1u, 10u, 33u);
    topology::TopologyEnrollment enrollment{};
    std::string error;
    CHECK_TRUE(authority.enroll(original, kNowMs + 1u, enrollment, error));
    if (!enrollment.granted) return;

    const RouteEndpoint initialQsr{0x0100007Fu, 46251u};
    model::NodeAdvertisement initialAdvertisement{};
    topology::TopologyControlEnvelope decoded{};
    CHECK_TRUE(bindDistinctQsr(authority, enrollment, initialQsr, 1u,
                               kNowMs + 2u, &decoded, &initialAdvertisement));
    wire::ControlDatagram initialDatagram{};
    initialDatagram.type = wire::ControlMessageType::NodeAdvertisement;
    initialDatagram.nodeAdvertisement = initialAdvertisement;
    CHECK_TRUE(authority.authorize(initialQsr, initialDatagram,
                                   kNowMs + 2u));

    topology::TopologyViewerProof pending = original;
    pending.endpoint.port = 45252u;
    ++pending.viewerGeneration;
    ++pending.routeGeneration;
    pending.confirmedCursor = 44u;
    pending.qspStreaming = false;
    CHECK_TRUE(authority.beginViewerRebind(pending, kNowMs + 3u, error));

    topology::TopologyEnrollment rebind{};
    CHECK_TRUE(authority.enrollmentForNode(enrollment.nodeId, rebind));
    CHECK_EQ(rebind.nodeId, enrollment.nodeId);
    CHECK_EQ(rebind.bindingGeneration, 2u);
    CHECK_EQ(authority.requiredCursor(enrollment.nodeId),
             rebind.qtaCursor.nextRecord);
    topology::TopologyEnrollment oldEndpoint{};
    CHECK_TRUE(!authority.enrollmentForEndpoint(original.endpoint, oldEndpoint));
    CHECK_TRUE(!authority.enrollmentForQsrEndpoint(initialQsr, oldEndpoint));

    const RouteEndpoint pendingQsrEndpoint{0x0100007Fu, 46252u};
    model::NodeAdvertisement pendingAdvertisement{};
    std::vector<uint8_t> pendingQsr;
    CHECK_TRUE(!authority.childAdvertisement(enrollment.nodeId,
                                              pendingAdvertisement));
    CHECK_TRUE(makeInitialQsrAdvertisement(rebind, pendingQsrEndpoint,
                                            pendingAdvertisement));
    CHECK_EQ(pendingAdvertisement.route.port, pending.endpoint.port);
    CHECK_EQ(pendingAdvertisement.controlRoute.port, pendingQsrEndpoint.port);
    CHECK_EQ(pendingAdvertisement.routeGeneration, pending.routeGeneration);
    CHECK_TRUE(encodeAdvertisement(pendingAdvertisement, pendingQsr));
    std::vector<uint8_t> pendingControl;
    CHECK_TRUE(authority.encodeControl(enrollment, 1u, pendingQsr.data(),
                                       pendingQsr.size(), pendingControl));
    CHECK_EQ(authority.decodeControl(pendingQsrEndpoint, pendingControl.data(),
                                     pendingControl.size(), kNowMs + 4u,
                                     decoded),
             topology::TopologyControlResult::BindingRejected);
    wire::ControlDatagram pendingDatagram{};
    pendingDatagram.type = wire::ControlMessageType::NodeAdvertisement;
    pendingDatagram.nodeAdvertisement = pendingAdvertisement;
    CHECK_TRUE(!authority.authorize(pendingQsrEndpoint, pendingDatagram,
                                    kNowMs + 4u));

    CHECK_TRUE(!authority.refreshViewerBinding(pending, kNowMs + 4u, error));
    pending.qspStreaming = true;
    CHECK_TRUE(authority.refreshViewerBinding(pending, kNowMs + 5u, error));
    CHECK_TRUE(authority.enrollmentForNode(enrollment.nodeId, rebind));
    CHECK_EQ(rebind.nodeId, enrollment.nodeId);
    CHECK_EQ(rebind.bindingGeneration, 2u);
    CHECK_EQ(authority.decodeControl(pendingQsrEndpoint, pendingControl.data(),
                                     pendingControl.size(), kNowMs + 5u,
                                     decoded),
             topology::TopologyControlResult::AuthenticationRejected);
    std::vector<uint8_t> finalizedControl;
    CHECK_TRUE(authority.encodeControl(enrollment, 1u, pendingQsr.data(),
                                       pendingQsr.size(), finalizedControl));
    CHECK_EQ(authority.decodeControl(pendingQsrEndpoint, finalizedControl.data(),
                                     finalizedControl.size(), kNowMs + 5u,
                                     decoded),
             topology::TopologyControlResult::Accepted);
    CHECK_TRUE(authority.authorize(pendingQsrEndpoint, pendingDatagram,
                                    kNowMs + 5u));
    model::NodeAdvertisement reboundAdvertisement{};
    CHECK_TRUE(authority.childAdvertisement(enrollment.nodeId,
                                            reboundAdvertisement));
    CHECK_EQ(reboundAdvertisement.route.port, pending.endpoint.port);
    CHECK_EQ(reboundAdvertisement.controlRoute.port, pendingQsrEndpoint.port);

    CHECK_TRUE(!authority.beginViewerRebind(pending, kNowMs + 6u, error));
    topology::TopologyViewerProof wrongViewer = pending;
    wrongViewer.viewerId ^= 0x100u;
    ++wrongViewer.viewerGeneration;
    ++wrongViewer.routeGeneration;
    CHECK_TRUE(!authority.beginViewerRebind(wrongViewer, kNowMs + 6u, error));
}

void testAuthorityRestartDropsPriorSessionNodesAndControls() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    const topology::TopologyViewerProof original =
        viewerProof(0xA260000000000001ull, 45261u, 1u, 11u, 35u);
    topology::TopologyEnrollment oldEnrollment{};
    std::string error;
    CHECK_TRUE(authority.enroll(original, kNowMs + 1u, oldEnrollment, error));
    if (!oldEnrollment.granted) return;
    const uint64_t oldAuthorityGeneration = authority.facts().authorityGeneration;
    const model::NodeId oldRootNodeId = authority.facts().rootNodeId;

    const RouteEndpoint oldQsrEndpoint{0x0100007Fu, 46261u};
    model::NodeAdvertisement oldAdvertisement{};
    std::vector<uint8_t> oldQsr;
    std::vector<uint8_t> oldControl;
    CHECK_TRUE(makeInitialQsrAdvertisement(oldEnrollment, oldQsrEndpoint,
                                            oldAdvertisement));
    CHECK_TRUE(encodeAdvertisement(oldAdvertisement, oldQsr));
    CHECK_TRUE(authority.encodeControl(oldEnrollment, 1u, oldQsr.data(),
                                       oldQsr.size(), oldControl));
    topology::TopologyControlEnvelope oldDecoded{};
    CHECK_EQ(authority.decodeControl(oldQsrEndpoint, oldControl.data(),
                                     oldControl.size(), kNowMs + 2u,
                                     oldDecoded),
             topology::TopologyControlResult::Accepted);

    const SessionIdentity restartedSession =
        sessionIdentity(kSessionId + 1u, kEpoch + 1u);
    CHECK_TRUE(authority.initialize(restartedSession, compatibilityIdentity(),
                                    kStreamRootIdentity + 1u,
                                    activationRequest(), qsrBinding(),
                                    kNowMs + 10u, error));
    CHECK_TRUE(authority.ready());
    if (!authority.ready()) return;
    const topology::TopologyAuthorityFacts& restarted = authority.facts();
    CHECK_EQ(restarted.session.sessionId, restartedSession.sessionId);
    CHECK_EQ(restarted.session.epoch, restartedSession.epoch);
    CHECK_TRUE(restarted.authorityGeneration != oldAuthorityGeneration);
    CHECK_TRUE(restarted.rootNodeId != oldRootNodeId);
    CHECK_EQ(authority.enrolledCount(), static_cast<size_t>(0u));

    topology::TopologyControlEnvelope decoded{};
    CHECK_EQ(authority.decodeControl(oldQsrEndpoint, oldControl.data(),
                                     oldControl.size(), kNowMs + 11u, decoded),
             topology::TopologyControlResult::AuthorityMismatch);
    topology::TopologyEnrollment absent{};
    CHECK_TRUE(!authority.enrollmentForNode(oldEnrollment.nodeId, absent));
    std::vector<uint8_t> shouldNotEncode;
    CHECK_TRUE(!authority.encodeControl(oldEnrollment, 2u, oldQsr.data(),
                                        oldQsr.size(), shouldNotEncode));

    topology::TopologyEnrollment fresh{};
    CHECK_TRUE(authority.enroll(original, kNowMs + 11u, fresh, error));
    CHECK_TRUE(fresh.granted);
    CHECK_TRUE(fresh.nodeId != oldEnrollment.nodeId);
    CHECK_TRUE(fresh.authorityGeneration != oldEnrollment.authorityGeneration);
}

void testAuthorityRequiresBoundAuthenticatedMonotonicControls() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    const topology::TopologyViewerProof proof =
        viewerProof(0xA300000000000001ull, 45301u, 1u, 7u, 41u);
    topology::TopologyEnrollment enrollment{};
    std::string error;
    CHECK_TRUE(authority.enroll(proof, kNowMs + 1u, enrollment, error));
    if (!enrollment.granted) return;

    const RouteEndpoint qsrEndpoint{0x0100007Fu, 46301u};
    model::NodeAdvertisement advertisement{};
    CHECK_TRUE(!authority.childAdvertisement(enrollment.nodeId, advertisement));
    CHECK_TRUE(makeInitialQsrAdvertisement(enrollment, qsrEndpoint,
                                            advertisement));
    std::vector<uint8_t> qsr;
    CHECK_TRUE(encodeAdvertisement(advertisement, qsr));
    if (qsr.empty()) return;

    std::vector<uint8_t> first;
    CHECK_TRUE(authority.encodeControl(enrollment, 1u, qsr.data(), qsr.size(),
                                       first));
    topology::TopologyControlEnvelope decoded{};
    CHECK_EQ(authority.decodeControl(proof.endpoint, first.data(), first.size(),
                                     kNowMs + 2u, decoded),
             topology::TopologyControlResult::EndpointMismatch);
    CHECK_EQ(authority.decodeControl(qsrEndpoint, first.data(), first.size(),
                                     kNowMs + 2u, decoded),
             topology::TopologyControlResult::Accepted);
    CHECK_EQ(decoded.nodeId, enrollment.nodeId);
    CHECK_EQ(decoded.authorityGeneration, authority.facts().authorityGeneration);
    CHECK_EQ(decoded.sequence, 1u);
    CHECK_EQ(decoded.qsr, qsr);

    wire::ControlDatagram inner{};
    CHECK_EQ(wire::decodeControlDatagram(decoded.qsr.data(), decoded.qsr.size(),
                                         inner), wire::DecodeResult::Ok);
    CHECK_TRUE(authority.authorize(qsrEndpoint, inner, kNowMs + 2u));
    authority.setVerifiedCursor(model::VerifiedChainCursor{88u, 9u,
                                                            0xCCDD0011u});
    model::NodeAdvertisement frozenAdvertisement{};
    CHECK_TRUE(authority.childAdvertisement(enrollment.nodeId,
                                            frozenAdvertisement));
    CHECK_EQ(frozenAdvertisement.cacheFirstCursor,
             enrollment.qtaCursor.nextRecord);
    CHECK_EQ(frozenAdvertisement.cacheNextCursor,
             enrollment.qtaCursor.nextRecord);
    CHECK_EQ(authority.decodeControl(qsrEndpoint, first.data(), first.size(),
                                     kNowMs + 2u, decoded),
             topology::TopologyControlResult::Replay);

    std::vector<uint8_t> second;
    CHECK_TRUE(authority.encodeControl(enrollment, 2u, qsr.data(), qsr.size(),
                                       second));
    RouteEndpoint wrongEndpoint = qsrEndpoint;
    ++wrongEndpoint.port;
    CHECK_EQ(authority.decodeControl(wrongEndpoint, second.data(), second.size(),
                                     kNowMs + 3u, decoded),
             topology::TopologyControlResult::EndpointMismatch);
    CHECK_EQ(authority.decodeControl(qsrEndpoint, second.data(), second.size(),
                                     kNowMs + 3u, decoded),
             topology::TopologyControlResult::Accepted);

    std::vector<uint8_t> tampered;
    CHECK_TRUE(authority.encodeControl(enrollment, 3u, qsr.data(), qsr.size(),
                                       tampered));
    CHECK_TRUE(tampered.size() > kMacOffset);
    if (tampered.size() > kMacOffset) {
        tampered[kMacOffset] ^= 0x80u;
        CHECK_EQ(authority.decodeControl(qsrEndpoint, tampered.data(),
                                         tampered.size(), kNowMs + 4u, decoded),
                 topology::TopologyControlResult::AuthenticationRejected);
    }

    std::vector<uint8_t> unknown;
    CHECK_TRUE(authority.encodeControl(enrollment, 4u, qsr.data(), qsr.size(),
                                       unknown));
    CHECK_TRUE(unknown.size() > kNodeIdOffset);
    if (unknown.size() > kNodeIdOffset) {
        unknown[kNodeIdOffset] ^= 0x01u;
        CHECK_EQ(authority.decodeControl(qsrEndpoint, unknown.data(),
                                         unknown.size(), kNowMs + 5u, decoded),
                 topology::TopologyControlResult::UnknownNode);
    }

    std::vector<uint8_t> wrongAuthority;
    CHECK_TRUE(authority.encodeControl(enrollment, 4u, qsr.data(), qsr.size(),
                                       wrongAuthority));
    CHECK_TRUE(wrongAuthority.size() > kAuthorityGenerationOffset);
    if (wrongAuthority.size() > kAuthorityGenerationOffset) {
        wrongAuthority[kAuthorityGenerationOffset] ^= 0x01u;
        CHECK_EQ(authority.decodeControl(qsrEndpoint, wrongAuthority.data(),
                                         wrongAuthority.size(), kNowMs + 5u,
                                         decoded),
                 topology::TopologyControlResult::AuthorityMismatch);
    }
}

void testAuthorityStagesFirstQsrAdmissionUntilInnerValidation() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    const topology::TopologyViewerProof proof =
        viewerProof(0xA350000000000001ull, 45351u, 1u, 8u, 42u);
    topology::TopologyEnrollment enrollment{};
    std::string error;
    CHECK_TRUE(authority.enroll(proof, kNowMs + 1u, enrollment, error));
    if (!enrollment.granted) return;

    const RouteEndpoint rejectedQsr{0x0100007Fu, 46351u};
    const RouteEndpoint admittedQsr{0x0100007Fu, 46352u};
    topology::TopologyControlEnvelope decoded{};
    topology::TopologyEnrollment lookup{};

    const std::array<uint8_t, 1u> malformedQta{{0xA5u}};
    std::vector<uint8_t> malformedEnvelope;
    CHECK_TRUE(authority.encodeControl(enrollment, 1u, malformedQta.data(),
                                       malformedQta.size(), malformedEnvelope));
    CHECK_EQ(authority.decodeControl(rejectedQsr, malformedEnvelope.data(),
                                     malformedEnvelope.size(), kNowMs + 2u,
                                     decoded),
             topology::TopologyControlResult::Malformed);
    CHECK_TRUE(!authority.enrollmentForQsrEndpoint(rejectedQsr, lookup));

    model::NodeAdvertisement unauthorized{};
    std::vector<uint8_t> unauthorizedQta;
    std::vector<uint8_t> unauthorizedEnvelope;
    CHECK_TRUE(makeInitialQsrAdvertisement(enrollment, rejectedQsr,
                                            unauthorized));
    unauthorized.stream.setOrSessionId ^= 0x01u;
    CHECK_TRUE(encodeAdvertisement(unauthorized, unauthorizedQta));
    CHECK_TRUE(authority.encodeControl(enrollment, 1u, unauthorizedQta.data(),
                                       unauthorizedQta.size(),
                                       unauthorizedEnvelope));
    CHECK_EQ(authority.decodeControl(rejectedQsr,
                                     unauthorizedEnvelope.data(),
                                     unauthorizedEnvelope.size(),
                                     kNowMs + 3u, decoded),
             topology::TopologyControlResult::BindingRejected);
    CHECK_TRUE(!authority.enrollmentForQsrEndpoint(rejectedQsr, lookup));
    model::NodeAdvertisement unbound{};
    CHECK_TRUE(!authority.childAdvertisement(enrollment.nodeId, unbound));

    model::NodeAdvertisement admitted{};
    std::vector<uint8_t> admittedQta;
    std::vector<uint8_t> admittedEnvelope;
    CHECK_TRUE(makeInitialQsrAdvertisement(enrollment, admittedQsr, admitted));
    CHECK_TRUE(encodeAdvertisement(admitted, admittedQta));
    CHECK_TRUE(authority.encodeControl(enrollment, 1u, admittedQta.data(),
                                       admittedQta.size(), admittedEnvelope));
    CHECK_EQ(authority.decodeControl(admittedQsr, admittedEnvelope.data(),
                                     admittedEnvelope.size(), kNowMs + 4u,
                                     decoded),
             topology::TopologyControlResult::Accepted);
    CHECK_TRUE(authority.enrollmentForQsrEndpoint(admittedQsr, lookup));
    CHECK_EQ(lookup.nodeId, enrollment.nodeId);
    CHECK_TRUE(!authority.enrollmentForQsrEndpoint(rejectedQsr, lookup));
}

void testAuthorityMakesRootLeasesOneTimeAndQsrFailureFailSoft() {
    topology::TopologyActivationRequest request = activationRequest();
    request.limits.maxChildrenPerRoot = 1u;
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority, request));
    if (!authority.ready()) return;

    const topology::TopologyViewerProof proof =
        viewerProof(0xA400000000000001ull, 45401u, 1u, 8u, 50u);
    topology::TopologyEnrollment enrollment{};
    std::string error;
    CHECK_TRUE(authority.enroll(proof, kNowMs + 1u, enrollment, error));
    if (!enrollment.granted) return;

    const topology::TopologyViewerProof queuedProof =
        viewerProof(0xA400000000000002ull, 45402u, 1u, 9u, 51u);
    topology::TopologyEnrollment queuedEnrollment{};
    CHECK_TRUE(authority.enroll(queuedProof, kNowMs + 1u, queuedEnrollment,
                                error));
    if (!queuedEnrollment.granted) return;

    const RouteEndpoint qsrEndpoint{0x0100007Fu, 46401u};
    const RouteEndpoint queuedQsrEndpoint{0x0100007Fu, 46402u};
    CHECK_TRUE(bindDistinctQsr(authority, enrollment, qsrEndpoint, 1u,
                               kNowMs + 2u));
    CHECK_TRUE(bindDistinctQsr(authority, queuedEnrollment, queuedQsrEndpoint,
                               1u, kNowMs + 2u));

    authority.setVerifiedCursor(model::VerifiedChainCursor{50u, 4u,
                                                            0xD15EA5E5u});
    uint64_t nonce = 0u;
    uint64_t expiryMs = 0u;
    CHECK_TRUE(authority.reserveRootOffer(enrollment.nodeId, kNowMs + 2u,
                                          nonce, expiryMs));
    CHECK_TRUE(nonce != 0u);
    CHECK_TRUE(expiryMs > kNowMs + 2u);
    CHECK_TRUE(expiryMs <= enrollment.expiresMs);
    CHECK_EQ(authority.reservedCount(), static_cast<size_t>(1u));
    const uint64_t firstNonce = nonce;
    const uint64_t firstExpiryMs = expiryMs;
    uint64_t duplicateNonce = 0u;
    uint64_t duplicateExpiryMs = 0u;
    CHECK_TRUE(!authority.reserveRootOffer(enrollment.nodeId, kNowMs + 2u,
                                           duplicateNonce, duplicateExpiryMs));
    CHECK_TRUE(!authority.reserveRootOffer(queuedEnrollment.nodeId,
                                           kNowMs + 2u, duplicateNonce,
                                           duplicateExpiryMs));

    const model::ParentLease lease = leaseFor(authority, enrollment, firstNonce,
                                              firstExpiryMs);
    wire::ControlDatagram leaseDatagram{};
    leaseDatagram.type = wire::ControlMessageType::ParentLease;
    leaseDatagram.parentLease = lease;
    CHECK_TRUE(authority.authorize(qsrEndpoint, leaseDatagram, kNowMs + 3u));

    model::ParentLease wrongNonce = lease;
    wrongNonce.replayNonce ^= 0x01u;
    leaseDatagram.parentLease = wrongNonce;
    CHECK_TRUE(!authority.authorize(qsrEndpoint, leaseDatagram,
                                    kNowMs + 3u));
    CHECK_TRUE(!authority.confirmRedemption(wrongNonce, kNowMs + 3u));

    model::ParentLease wrongExpiry = lease;
    ++wrongExpiry.expiryMs;
    leaseDatagram.parentLease = wrongExpiry;
    CHECK_TRUE(!authority.authorize(qsrEndpoint, leaseDatagram,
                                    kNowMs + 3u));
    CHECK_TRUE(!authority.confirmRedemption(wrongExpiry, kNowMs + 3u));

    model::ParentLease wrongIdentity = lease;
    wrongIdentity.stream.compatibility.timingProfileId ^= 0x01u;
    leaseDatagram.parentLease = wrongIdentity;
    CHECK_TRUE(!authority.authorize(qsrEndpoint, leaseDatagram,
                                    kNowMs + 3u));

    model::ParentLease wrongEpoch = lease;
    ++wrongEpoch.stream.gameEpoch;
    leaseDatagram.parentLease = wrongEpoch;
    CHECK_TRUE(!authority.authorize(qsrEndpoint, leaseDatagram,
                                    kNowMs + 3u));

    CHECK_TRUE(authority.confirmRedemption(lease, kNowMs + 3u));
    CHECK_TRUE(authority.isAttached(enrollment.nodeId));
    CHECK_EQ(authority.reservedCount(), static_cast<size_t>(0u));
    CHECK_TRUE(!authority.confirmRedemption(lease, kNowMs + 3u));
    CHECK_TRUE(!authority.reserveRootOffer(enrollment.nodeId, kNowMs + 3u,
                                           duplicateNonce, duplicateExpiryMs));

    topology::TopologyAuthority::ViewerLifecycleResult detached{};
    CHECK_TRUE(authority.detachViewer(proof.viewerId, detached));
    CHECK_EQ(detached.nodeId, enrollment.nodeId);
    CHECK_TRUE(detached.releasedAttachment);
    CHECK_EQ(detached.releasedAttachmentOwner,
             topology::TopologyAuthority::AttachmentOwner::Root);
    CHECK_TRUE(detached.requiresGenericRootDetach());
    CHECK_TRUE(!authority.isAttached(enrollment.nodeId));
    topology::TopologyEnrollment removed{};
    CHECK_TRUE(!authority.enrollmentForNode(enrollment.nodeId, removed));

    uint64_t freshNonce = 0u;
    uint64_t freshExpiryMs = 0u;
    CHECK_TRUE(authority.reserveRootOffer(queuedEnrollment.nodeId, kNowMs + 4u,
                                          freshNonce, freshExpiryMs));
    const model::ParentLease freshLease =
        leaseFor(authority, queuedEnrollment, freshNonce, freshExpiryMs);
    leaseDatagram.parentLease = freshLease;
    CHECK_TRUE(authority.authorize(queuedQsrEndpoint, leaseDatagram,
                                   kNowMs + 4u));
    CHECK_TRUE(authority.confirmRedemption(freshLease, kNowMs + 4u));
    CHECK_TRUE(authority.isAttached(queuedEnrollment.nodeId));

    std::vector<model::NodeId> expiredNodes;
    CHECK_TRUE(!authority.serviceExpiry(queuedEnrollment.expiresMs,
                                        expiredNodes));
    CHECK_EQ(expiredNodes.size(), static_cast<size_t>(1u));
    if (!expiredNodes.empty())
        CHECK_EQ(expiredNodes[0], queuedEnrollment.nodeId);
    CHECK_EQ(authority.enrolledCount(), static_cast<size_t>(0u));
    CHECK_TRUE(!authority.isAttached(queuedEnrollment.nodeId));

    topology::TopologyQsrControl qsr{};
    topology::TopologyActivationRequest failedRequest = activationRequest();
    failedRequest.forceQsrBindFailure = true;
    topology::EndpointBinding failedBinding{};
    CHECK_TRUE(!qsr.open(failedRequest, failedRequest.rootQsp, failedBinding,
                         error));
    CHECK_TRUE(!qsr.open());
    CHECK_TRUE(!failedBinding.validForAuthority());
    CHECK_TRUE(!qsr.binding().validForAuthority());
    CHECK_TRUE(error.find("forced to fail") != std::string::npos);
}

void testViewerLifecycleResultsPreserveRootAttachmentOwnership() {
    enum class LifecycleOperation : uint8_t {
        Detach = 1u,
        BeginRebind = 2u,
        RefreshBinding = 3u,
    };

    const auto exercise = [](const LifecycleOperation operation,
                             const uint64_t viewerId,
                             const uint16_t qspPort,
                             const uint16_t qsrPort) {
        topology::TopologyAuthority authority{};
        topology::TopologyActivationRequest request = activationRequest();
        request.limits.maxChildrenPerRoot = 1u;
        CHECK_TRUE(initializeAuthority(authority, request));
        if (!authority.ready()) return;

        const topology::TopologyViewerProof proof = viewerProof(
            viewerId, qspPort, 1u, 80u + static_cast<uint64_t>(operation),
            60u + static_cast<uint32_t>(operation));
        topology::TopologyEnrollment enrollment{};
        std::string error;
        CHECK_TRUE(authority.enroll(proof, kNowMs + 1u, enrollment, error));
        if (!enrollment.granted) return;
        CHECK_TRUE(bindDistinctQsr(
            authority, enrollment, RouteEndpoint{0x0100007Fu, qsrPort}, 1u,
            kNowMs + 2u));

        uint64_t offerNonce = 0u;
        uint64_t offerExpiry = 0u;
        CHECK_TRUE(authority.reserveRootOffer(enrollment.nodeId, kNowMs + 2u,
                                              offerNonce, offerExpiry));
        const model::ParentLease lease =
            leaseFor(authority, enrollment, offerNonce, offerExpiry);
        CHECK_TRUE(authority.confirmRedemption(lease, kNowMs + 3u));
        CHECK_TRUE(authority.isAttached(enrollment.nodeId));

        topology::TopologyAuthority::ViewerLifecycleResult lifecycle{};
        switch (operation) {
        case LifecycleOperation::Detach:
            CHECK_TRUE(authority.detachViewer(proof.viewerId, lifecycle));
            break;
        case LifecycleOperation::BeginRebind: {
            topology::TopologyViewerProof replacement = proof;
            replacement.endpoint.port = static_cast<uint16_t>(qspPort + 100u);
            ++replacement.viewerGeneration;
            ++replacement.routeGeneration;
            replacement.qspStreaming = false;
            CHECK_TRUE(authority.beginViewerRebind(
                replacement, kNowMs + 4u, lifecycle, error));
            break;
        }
        case LifecycleOperation::RefreshBinding: {
            topology::TopologyViewerProof replacement = proof;
            replacement.endpoint.port = static_cast<uint16_t>(qspPort + 100u);
            ++replacement.viewerGeneration;
            ++replacement.routeGeneration;
            replacement.qspStreaming = true;
            CHECK_TRUE(authority.refreshViewerBinding(
                replacement, kNowMs + 4u, lifecycle, error));
            break;
        }
        }

        CHECK_EQ(lifecycle.nodeId, enrollment.nodeId);
        CHECK_TRUE(lifecycle.releasedAttachment);
        CHECK_EQ(lifecycle.releasedAttachmentOwner,
                 topology::TopologyAuthority::AttachmentOwner::Root);
        CHECK_TRUE(lifecycle.requiresGenericRootDetach());
        CHECK_TRUE(!authority.isAttached(enrollment.nodeId));
    };

    exercise(LifecycleOperation::Detach, 0xA410000000000001ull, 45411u,
             46411u);
    exercise(LifecycleOperation::BeginRebind, 0xA410000000000002ull, 45412u,
             46412u);
    exercise(LifecycleOperation::RefreshBinding, 0xA410000000000003ull,
             45413u, 46413u);
}

void testAuthorityStateBindsOnlyCurrentAttachedRecipient() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    const topology::TopologyViewerProof proof =
        viewerProof(0xA500000000000001ull, 45501u, 1u, 13u, 63u);
    topology::TopologyEnrollment enrollment{};
    std::string error;
    CHECK_TRUE(authority.enroll(proof, kNowMs + 1u, enrollment, error));
    if (!enrollment.granted) return;

    const RouteEndpoint qsrEndpoint{0x0100007Fu, 46501u};
    CHECK_TRUE(bindDistinctQsr(authority, enrollment, qsrEndpoint, 1u,
                               kNowMs + 2u));
    authority.setVerifiedCursor(model::VerifiedChainCursor{50u, 7u,
                                                            0xD15EA5E6u});
    uint64_t nonce = 0u;
    uint64_t expiryMs = 0u;
    CHECK_TRUE(authority.reserveRootOffer(enrollment.nodeId, kNowMs + 2u,
                                          nonce, expiryMs));
    const model::ParentLease lease = leaseFor(authority, enrollment, nonce,
                                              expiryMs);
    wire::ControlDatagram leaseDatagram{};
    leaseDatagram.type = wire::ControlMessageType::ParentLease;
    leaseDatagram.parentLease = lease;
    CHECK_TRUE(authority.authorize(qsrEndpoint, leaseDatagram, kNowMs + 3u));
    CHECK_TRUE(authority.confirmRedemption(lease, kNowMs + 3u));

    std::vector<topology::AuthorityStateRecipient> recipients;
    CHECK_TRUE(authority.collectAuthorityState(
        wire::AuthorityStateKind::Liveness, wire::AuthorityStateReason::None,
        kNowMs + 4u, recipients));
    CHECK_EQ(recipients.size(), static_cast<size_t>(1u));
    if (recipients.empty()) return;
    const wire::AuthorityState first = recipients.front().state;
    const topology::TopologyAuthorityFacts& facts = authority.facts();
    CHECK_EQ(first.stream.setOrSessionId, kSessionId);
    CHECK_EQ(first.stream.gameEpoch, kEpoch);
    CHECK_TRUE(compat::semanticallyEqual(first.stream.compatibility,
                                         compatibilityIdentity()));
    CHECK_EQ(first.stream.streamRootIdentity, kStreamRootIdentity);
    CHECK_EQ(first.authorityGeneration, facts.authorityGeneration);
    CHECK_EQ(first.rootNodeId, facts.rootNodeId);
    CHECK_EQ(first.childNodeId, enrollment.nodeId);
    CHECK_EQ(first.viewerId, proof.viewerId);
    CHECK_EQ(first.viewerGeneration, proof.viewerGeneration);
    CHECK_EQ(first.qspRouteGeneration, proof.routeGeneration);
    CHECK_EQ(first.qsrIpv4BE, qsrEndpoint.ipv4BE);
    CHECK_EQ(first.qsrPort, qsrEndpoint.port);
    CHECK_EQ(first.qsrBindingGeneration, enrollment.bindingGeneration);
    CHECK_EQ(first.kind, wire::AuthorityStateKind::Liveness);
    CHECK_EQ(first.reason, wire::AuthorityStateReason::None);
    CHECK_TRUE(first.sequence != 0u);
    CHECK_EQ(first.authorityExpiryMs, enrollment.expiresMs);
    CHECK_EQ(first.eventTimeMs, kNowMs + 4u);
    CHECK_TRUE(first.livenessWindowMs != 0u);
    CHECK_TRUE(first.livenessWindowMs
               <= wire::kTopologyAuthorityStateMaxLivenessWindowMs);

    wire::ControlDatagram encoded{};
    encoded.type = wire::ControlMessageType::AuthorityState;
    encoded.authorityState = first;
    std::vector<uint8_t> bytes;
    CHECK_TRUE(wire::encodeControlDatagram(encoded, bytes));
    wire::ControlDatagram decoded{};
    CHECK_EQ(wire::decodeControlDatagram(bytes.data(), bytes.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type, wire::ControlMessageType::AuthorityState);
    CHECK_EQ(decoded.authorityState.sequence, first.sequence);

    recipients.clear();
    CHECK_TRUE(authority.collectAuthorityState(
        wire::AuthorityStateKind::Liveness, wire::AuthorityStateReason::None,
        kNowMs + 5u, recipients));
    CHECK_TRUE(recipients.empty());

    recipients.clear();
    CHECK_TRUE(authority.collectAuthorityState(
        wire::AuthorityStateKind::Revocation,
        wire::AuthorityStateReason::RouteReplaced, kNowMs + 6u, recipients,
        enrollment.nodeId));
    CHECK_EQ(recipients.size(), static_cast<size_t>(1u));
    if (!recipients.empty()) {
        const wire::AuthorityState& revoked = recipients.front().state;
        CHECK_EQ(revoked.kind, wire::AuthorityStateKind::Revocation);
        CHECK_EQ(revoked.reason, wire::AuthorityStateReason::RouteReplaced);
        CHECK_TRUE(revoked.sequence > first.sequence);
        CHECK_EQ(revoked.livenessWindowMs, 0u);
    }
}

void testRelayDataBindingRequiresExactAttachedHostObservedRoutes() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;
    authority.setVerifiedCursor(
        model::VerifiedChainCursor{50u, 4u, 0xDADA55AA00112233ull});

    const topology::TopologyViewerProof firstProof =
        viewerProof(0xA600000000000001ull, 45601u, 2u, 21u, 65u);
    const RouteEndpoint firstQsr{0x0100007Fu, 46601u};
    const RouteEndpoint firstData{0x0100007Fu, 47601u};
    topology::TopologyEnrollment first{};
    std::string error;
    CHECK_TRUE(authority.enroll(firstProof, kNowMs + 1u, first, error));
    if (!first.granted) return;
    CHECK_TRUE(bindDistinctQsr(authority, first, firstQsr, 1u, kNowMs + 2u));

    uint64_t firstOfferNonce = 0u;
    uint64_t firstOfferExpiry = 0u;
    CHECK_TRUE(authority.reserveRootOffer(first.nodeId, kNowMs + 2u,
                                          firstOfferNonce, firstOfferExpiry));
    const model::ParentLease firstLease =
        leaseFor(authority, first, firstOfferNonce, firstOfferExpiry);
    CHECK_EQ(firstLease.childId, first.nodeId);
    CHECK_TRUE(firstLease.macLow != 0u || firstLease.macHigh != 0u);
    CHECK_TRUE(authority.confirmRedemption(firstLease, kNowMs + 3u));

    const wire::RelayDataBindingRequest firstRequest =
        relayDataBindingRequest(authority, first, firstLease,
                                0x6100000000000001ull);
    std::vector<uint8_t> firstInner;
    CHECK_TRUE(encodeRelayDataBindingRequest(firstRequest, firstInner));
    if (firstInner.empty()) return;

    wire::ControlDatagram decodedInner{};
    CHECK_EQ(wire::decodeControlDatagram(firstInner.data(), firstInner.size(),
                                         decodedInner),
             wire::DecodeResult::Ok);
    CHECK_EQ(decodedInner.type, wire::ControlMessageType::RelayDataBindingRequest);
    CHECK_EQ(decodedInner.relayDataBindingRequest.requestNonce,
             firstRequest.requestNonce);
    std::vector<uint8_t> oldVersion = firstInner;
    CHECK_TRUE(oldVersion.size() >= wire::kResilientControlHeaderBytes);
    if (oldVersion.size() >= wire::kResilientControlHeaderBytes) {
        oldVersion[4u] = 5u;
        oldVersion[5u] = 0u;
        CHECK_EQ(wire::decodeControlDatagram(oldVersion.data(),
                                             oldVersion.size(), decodedInner),
                 wire::DecodeResult::UnsupportedVersion);
    }

    wire::RelayDataBindingAck firstAck{};
    CHECK_TRUE(!authority.relayDataBindingAck(first.nodeId, firstRequest,
                                              firstAck));

    std::vector<uint8_t> firstEnvelope;
    CHECK_TRUE(authority.encodeControl(first, 2u, firstInner.data(),
                                       firstInner.size(), firstEnvelope));
    topology::TopologyControlEnvelope decodedEnvelope{};
    CHECK_EQ(authority.decodeControl(firstProof.endpoint, firstEnvelope.data(),
                                     firstEnvelope.size(), kNowMs + 4u,
                                     decodedEnvelope),
             topology::TopologyControlResult::EndpointMismatch);
    CHECK_EQ(authority.decodeControl(firstQsr, firstEnvelope.data(),
                                     firstEnvelope.size(), kNowMs + 4u,
                                     decodedEnvelope),
             topology::TopologyControlResult::EndpointMismatch);
    const RouteEndpoint rootQsp{
        authority.facts().rootQsp.advertised.ipv4BE,
        authority.facts().rootQsp.advertised.port};
    const RouteEndpoint rootQsr{
        authority.facts().rootQsr.advertised.ipv4BE,
        authority.facts().rootQsr.advertised.port};
    CHECK_EQ(authority.decodeControl(rootQsp, firstEnvelope.data(),
                                     firstEnvelope.size(), kNowMs + 4u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);
    CHECK_EQ(authority.decodeControl(rootQsr, firstEnvelope.data(),
                                     firstEnvelope.size(), kNowMs + 4u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);
    CHECK_EQ(authority.decodeControl(firstData, firstEnvelope.data(),
                                     firstEnvelope.size(), kNowMs + 4u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    CHECK_EQ(decodedEnvelope.nodeId, first.nodeId);
    CHECK_EQ(decodedEnvelope.sequence, 2u);

    CHECK_TRUE(authority.relayDataBindingAck(first.nodeId, firstRequest,
                                              firstAck));
    CHECK_EQ(firstAck.requestNonce, firstRequest.requestNonce);
    CHECK_EQ(firstAck.lease.childId, firstLease.childId);
    CHECK_EQ(firstAck.lease.macLow, firstLease.macLow);
    CHECK_EQ(firstAck.observedDataRoute.kind, model::RouteKind::Direct);
    CHECK_EQ(firstAck.observedDataRoute.ipv4BE, firstData.ipv4BE);
    CHECK_EQ(firstAck.observedDataRoute.port, firstData.port);
    CHECK_TRUE(firstAck.observedDataRoute.routeProved);
    CHECK_EQ(firstAck.observedControlRoute.kind, model::RouteKind::Direct);
    CHECK_EQ(firstAck.observedControlRoute.ipv4BE, firstQsr.ipv4BE);
    CHECK_EQ(firstAck.observedControlRoute.port, firstQsr.port);
    CHECK_TRUE(firstAck.observedControlRoute.routeProved);
    CHECK_TRUE(!firstAck.observedDataRoute.sameEndpoint(
        firstAck.observedControlRoute));
    CHECK_EQ(firstAck.observedDataRoute.generation,
             firstAck.observedControlRoute.generation);

    wire::ControlDatagram ackDatagram{};
    ackDatagram.type = wire::ControlMessageType::RelayDataBindingAck;
    ackDatagram.relayDataBindingAck = firstAck;
    std::vector<uint8_t> ackBytes;
    CHECK_TRUE(wire::encodeControlDatagram(ackDatagram, ackBytes));
    wire::ControlDatagram decodedAck{};
    CHECK_EQ(wire::decodeControlDatagram(ackBytes.data(), ackBytes.size(),
                                         decodedAck), wire::DecodeResult::Ok);
    CHECK_EQ(decodedAck.type, wire::ControlMessageType::RelayDataBindingAck);
    CHECK_EQ(decodedAck.relayDataBindingAck.requestNonce,
             firstRequest.requestNonce);

    CHECK_EQ(authority.decodeControl(firstData, firstEnvelope.data(),
                                     firstEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Replay);
    std::vector<uint8_t> firstRetry;
    CHECK_TRUE(authority.encodeControl(first, 3u, firstInner.data(),
                                       firstInner.size(), firstRetry));
    CHECK_EQ(authority.decodeControl(firstData, firstRetry.data(),
                                     firstRetry.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    const RouteEndpoint wrongData{0x0100007Fu, 47602u};
    std::vector<uint8_t> wrongSource;
    CHECK_TRUE(authority.encodeControl(first, 4u, firstInner.data(),
                                       firstInner.size(), wrongSource));
    CHECK_EQ(authority.decodeControl(wrongData, wrongSource.data(),
                                     wrongSource.size(), kNowMs + 6u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);
    wire::RelayDataBindingRequest wrongNonce = firstRequest;
    ++wrongNonce.requestNonce;
    std::vector<uint8_t> wrongNonceInner;
    std::vector<uint8_t> wrongNonceEnvelope;
    CHECK_TRUE(encodeRelayDataBindingRequest(wrongNonce, wrongNonceInner));
    CHECK_TRUE(authority.encodeControl(first, 4u, wrongNonceInner.data(),
                                       wrongNonceInner.size(),
                                       wrongNonceEnvelope));
    CHECK_EQ(authority.decodeControl(firstData, wrongNonceEnvelope.data(),
                                     wrongNonceEnvelope.size(), kNowMs + 6u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);
    wire::RelayDataBindingRequest staleLease = firstRequest;
    ++staleLease.lease.childBinding.childEndpointBindingGeneration;
    std::vector<uint8_t> staleLeaseInner;
    std::vector<uint8_t> staleLeaseEnvelope;
    CHECK_TRUE(encodeRelayDataBindingRequest(staleLease, staleLeaseInner));
    CHECK_TRUE(authority.encodeControl(first, 4u, staleLeaseInner.data(),
                                       staleLeaseInner.size(),
                                       staleLeaseEnvelope));
    CHECK_EQ(authority.decodeControl(firstData, staleLeaseEnvelope.data(),
                                     staleLeaseEnvelope.size(), kNowMs + 6u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);
    wire::RelayDataBindingRequest wrongStream = firstRequest;
    wrongStream.stream.streamRootIdentity ^= 0x01u;
    std::vector<uint8_t> wrongStreamInner;
    std::vector<uint8_t> wrongStreamEnvelope;
    CHECK_TRUE(encodeRelayDataBindingRequest(wrongStream, wrongStreamInner));
    CHECK_TRUE(authority.encodeControl(first, 4u, wrongStreamInner.data(),
                                       wrongStreamInner.size(),
                                       wrongStreamEnvelope));
    CHECK_EQ(authority.decodeControl(firstData, wrongStreamEnvelope.data(),
                                     wrongStreamEnvelope.size(), kNowMs + 6u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);
    std::vector<uint8_t> exactRetry;
    CHECK_TRUE(authority.encodeControl(first, 4u, firstInner.data(),
                                       firstInner.size(), exactRetry));
    CHECK_EQ(authority.decodeControl(firstData, exactRetry.data(),
                                     exactRetry.size(), kNowMs + 6u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    const topology::TopologyViewerProof secondProof =
        viewerProof(0xA600000000000002ull, 45602u, 3u, 22u, 66u);
    const RouteEndpoint secondQsr{0x0100007Fu, 46602u};
    const RouteEndpoint secondData{0x0100007Fu, 47603u};
    topology::TopologyEnrollment second{};
    CHECK_TRUE(authority.enroll(secondProof, kNowMs + 7u, second, error));
    if (!second.granted) return;
    CHECK_TRUE(bindDistinctQsr(authority, second, secondQsr, 1u,
                               kNowMs + 8u));
    uint64_t secondOfferNonce = 0u;
    uint64_t secondOfferExpiry = 0u;
    CHECK_TRUE(authority.reserveRootOffer(second.nodeId, kNowMs + 8u,
                                          secondOfferNonce,
                                          secondOfferExpiry));
    const model::ParentLease secondLease =
        leaseFor(authority, second, secondOfferNonce, secondOfferExpiry);
    CHECK_TRUE(authority.confirmRedemption(secondLease, kNowMs + 9u));
    const wire::RelayDataBindingRequest secondRequest =
        relayDataBindingRequest(authority, second, secondLease,
                                0x6100000000000002ull);
    std::vector<uint8_t> secondInner;
    std::vector<uint8_t> secondCollision;
    CHECK_TRUE(encodeRelayDataBindingRequest(secondRequest, secondInner));
    CHECK_TRUE(authority.encodeControl(second, 2u, secondInner.data(),
                                       secondInner.size(), secondCollision));
    CHECK_EQ(authority.decodeControl(firstData, secondCollision.data(),
                                     secondCollision.size(), kNowMs + 10u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);
    CHECK_EQ(authority.decodeControl(secondData, secondCollision.data(),
                                     secondCollision.size(), kNowMs + 10u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    const topology::TopologyViewerProof thirdProof =
        viewerProof(0xA600000000000003ull, 45603u, 4u, 23u, 67u);
    const RouteEndpoint thirdQsr{0x0100007Fu, 46603u};
    const RouteEndpoint thirdData{0x0100007Fu, 47604u};
    topology::TopologyEnrollment third{};
    CHECK_TRUE(authority.enroll(thirdProof, kNowMs + 11u, third, error));
    if (!third.granted) return;
    CHECK_TRUE(bindDistinctQsr(authority, third, thirdQsr, 1u,
                               kNowMs + 12u));
    uint64_t thirdOfferNonce = 0u;
    uint64_t thirdOfferExpiry = 0u;
    CHECK_TRUE(authority.reserveRootOffer(third.nodeId, kNowMs + 12u,
                                          thirdOfferNonce, thirdOfferExpiry));
    const model::ParentLease thirdLease =
        leaseFor(authority, third, thirdOfferNonce, thirdOfferExpiry);
    const wire::RelayDataBindingRequest thirdRequest =
        relayDataBindingRequest(authority, third, thirdLease,
                                0x6100000000000003ull);
    std::vector<uint8_t> thirdInner;
    std::vector<uint8_t> thirdEnvelope;
    CHECK_TRUE(encodeRelayDataBindingRequest(thirdRequest, thirdInner));
    CHECK_TRUE(authority.encodeControl(third, 2u, thirdInner.data(),
                                       thirdInner.size(), thirdEnvelope));
    CHECK_EQ(authority.decodeControl(thirdData, thirdEnvelope.data(),
                                     thirdEnvelope.size(), kNowMs + 13u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);
}

void testRelayReadyRequiresHostBindingAndExplicitAssignment() {
    CHECK_TRUE(qnet::netInit());
    qnet::UdpSock relayQsrReceiver{};
    CHECK_TRUE(relayQsrReceiver.open(0u));
    if (!relayQsrReceiver.isOpen()) return;
    topology::TopologyAuthority authority{};
    topology::TopologyActivationRequest request = activationRequest();
    request.limits.maxChildrenPerRelay = 2u;
    CHECK_TRUE(initializeAuthority(authority, request));
    if (!authority.ready()) return;

    const model::VerifiedChainCursor currentCursor{
        50u, 4u, 0xBAADF00D00112233ull};
    authority.setVerifiedCursor(currentCursor);
    const model::VerifiedCacheRange currentRange{
        currentCursor.nextRecord, currentCursor.nextRecord};

    const topology::TopologyViewerProof relayProof =
        viewerProof(0xA700000000000001ull, 45701u, 2u, 31u, 71u);
    const topology::TopologyViewerProof childProof =
        viewerProof(0xA700000000000002ull, 45702u, 3u, 32u, 72u);
    const topology::TopologyViewerProof overflowProof =
        viewerProof(0xA700000000000003ull, 45703u, 4u, 33u, 73u);
    const topology::TopologyViewerProof thirdProof =
        viewerProof(0xA700000000000004ull, 45704u, 5u, 34u, 74u);
    const RouteEndpoint relayQsr{0x0100007Fu, relayQsrReceiver.boundPort()};
    const RouteEndpoint relayData{0x0100007Fu, 47701u};
    const RouteEndpoint childQsr{0x0100007Fu, 46702u};
    const RouteEndpoint overflowQsr{0x0100007Fu, 46703u};
    const RouteEndpoint thirdQsr{0x0100007Fu, 46704u};
    topology::TopologyEnrollment relay{};
    topology::TopologyEnrollment child{};
    topology::TopologyEnrollment overflow{};
    topology::TopologyEnrollment third{};
    std::string error;
    CHECK_TRUE(authority.enroll(relayProof, kNowMs + 1u, relay, error));
    CHECK_TRUE(authority.enroll(childProof, kNowMs + 1u, child, error));
    CHECK_TRUE(authority.enroll(overflowProof, kNowMs + 1u, overflow, error));
    CHECK_TRUE(authority.enroll(thirdProof, kNowMs + 1u, third, error));
    if (!relay.granted || !child.granted || !overflow.granted || !third.granted)
        return;
    CHECK_TRUE(bindDistinctQsr(authority, relay, relayQsr, 1u, kNowMs + 2u));
    CHECK_TRUE(bindDistinctQsr(authority, child, childQsr, 1u, kNowMs + 2u));
    CHECK_TRUE(bindDistinctQsr(authority, overflow, overflowQsr, 1u,
                               kNowMs + 2u));
    CHECK_TRUE(bindDistinctQsr(authority, third, thirdQsr, 1u,
                               kNowMs + 2u));

    uint64_t offerNonce = 0u;
    uint64_t offerExpiry = 0u;
    CHECK_TRUE(authority.reserveRootOffer(relay.nodeId, kNowMs + 2u,
                                          offerNonce, offerExpiry));
    const model::ParentLease relayLease =
        leaseFor(authority, relay, offerNonce, offerExpiry);
    CHECK_TRUE(authority.confirmRedemption(relayLease, kNowMs + 3u));

    const wire::RelayDataBindingRequest bindingRequest =
        relayDataBindingRequest(authority, relay, relayLease,
                                0x7100000000000001ull);
    std::vector<uint8_t> bindingBytes;
    std::vector<uint8_t> bindingEnvelope;
    CHECK_TRUE(encodeRelayDataBindingRequest(bindingRequest, bindingBytes));
    CHECK_TRUE(authority.encodeControl(relay, 2u, bindingBytes.data(),
                                       bindingBytes.size(), bindingEnvelope));
    topology::TopologyControlEnvelope decodedEnvelope{};
    CHECK_EQ(authority.decodeControl(relayData, bindingEnvelope.data(),
                                     bindingEnvelope.size(), kNowMs + 4u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    wire::RelayDataBindingAck bindingAck{};
    CHECK_TRUE(authority.relayDataBindingAck(relay.nodeId, bindingRequest,
                                              bindingAck));

    const wire::RelayReady correct = relayReady(
        bindingRequest, bindingAck, currentRange, currentCursor);
    std::vector<uint8_t> readyBytes;
    CHECK_TRUE(encodeRelayReady(correct, readyBytes));
    if (readyBytes.empty()) return;

    std::vector<uint8_t> oldVersion = readyBytes;
    CHECK_TRUE(oldVersion.size() >= wire::kResilientControlHeaderBytes);
    if (oldVersion.size() >= wire::kResilientControlHeaderBytes) {
        oldVersion[4u] = 5u;
        oldVersion[5u] = 0u;
        wire::ControlDatagram ignored{};
        CHECK_EQ(wire::decodeControlDatagram(oldVersion.data(),
                                             oldVersion.size(), ignored),
                 wire::DecodeResult::UnsupportedVersion);
    }

    wire::RelayReady wrongParent = correct;
    wrongParent.rootNodeId ^= 0x100u;
    wrongParent.relayLease.parentId = wrongParent.rootNodeId;
    std::vector<uint8_t> wrongParentBytes;
    std::vector<uint8_t> wrongParentEnvelope;
    CHECK_TRUE(encodeRelayReady(wrongParent, wrongParentBytes));
    CHECK_TRUE(authority.encodeControl(relay, 3u, wrongParentBytes.data(),
                                       wrongParentBytes.size(),
                                       wrongParentEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, wrongParentEnvelope.data(),
                                     wrongParentEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);

    wire::RelayReady wrongChild = correct;
    wrongChild.relayNodeId = child.nodeId;
    wrongChild.relayLease.childId = child.nodeId;
    std::vector<uint8_t> wrongChildBytes;
    std::vector<uint8_t> wrongChildEnvelope;
    CHECK_TRUE(encodeRelayReady(wrongChild, wrongChildBytes));
    CHECK_TRUE(authority.encodeControl(relay, 3u, wrongChildBytes.data(),
                                       wrongChildBytes.size(),
                                       wrongChildEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, wrongChildEnvelope.data(),
                                     wrongChildEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);

    wire::RelayReady wrongRoute = correct;
    ++wrongRoute.observedDataRoute.port;
    std::vector<uint8_t> wrongRouteBytes;
    std::vector<uint8_t> wrongRouteEnvelope;
    CHECK_TRUE(encodeRelayReady(wrongRoute, wrongRouteBytes));
    CHECK_TRUE(authority.encodeControl(relay, 3u, wrongRouteBytes.data(),
                                       wrongRouteBytes.size(),
                                       wrongRouteEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, wrongRouteEnvelope.data(),
                                     wrongRouteEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);

    wire::RelayReady wrongBinding = correct;
    ++wrongBinding.bindingRequestNonce;
    std::vector<uint8_t> wrongBindingBytes;
    std::vector<uint8_t> wrongBindingEnvelope;
    CHECK_TRUE(encodeRelayReady(wrongBinding, wrongBindingBytes));
    CHECK_TRUE(authority.encodeControl(relay, 3u, wrongBindingBytes.data(),
                                       wrongBindingBytes.size(),
                                       wrongBindingEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, wrongBindingEnvelope.data(),
                                     wrongBindingEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);

    wire::RelayReady wrongCache = correct;
    ++wrongCache.cacheCursor.nextRecord;
    ++wrongCache.cacheCursor.nextEvent;
    wrongCache.cacheCursor.digest ^= 0x1u;
    wrongCache.cacheRange = model::VerifiedCacheRange{
        wrongCache.cacheCursor.nextRecord, wrongCache.cacheCursor.nextRecord};
    std::vector<uint8_t> wrongCacheBytes;
    std::vector<uint8_t> wrongCacheEnvelope;
    CHECK_TRUE(encodeRelayReady(wrongCache, wrongCacheBytes));
    CHECK_TRUE(authority.encodeControl(relay, 3u, wrongCacheBytes.data(),
                                       wrongCacheBytes.size(),
                                       wrongCacheEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, wrongCacheEnvelope.data(),
                                     wrongCacheEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);

    std::vector<uint8_t> readyEnvelope;
    CHECK_TRUE(authority.encodeControl(relay, 3u, readyBytes.data(),
                                       readyBytes.size(), readyEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, readyEnvelope.data(),
                                     readyEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    CHECK_EQ(authority.decodeControl(relayQsr, readyEnvelope.data(),
                                     readyEnvelope.size(), kNowMs + 6u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Replay);
    std::vector<uint8_t> readyRetry;
    CHECK_TRUE(authority.encodeControl(relay, 4u, readyBytes.data(),
                                       readyBytes.size(), readyRetry));
    CHECK_EQ(authority.decodeControl(relayQsr, readyRetry.data(),
                                     readyRetry.size(), kNowMs + 6u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    wire::RelayChildAssignment assignment{};
    model::RouteDescriptor relayControlRoute{};
    CHECK_TRUE(authority.issueRelayChildAssignment(
        relay.nodeId, child.nodeId, kNowMs + 7u, assignment,
        relayControlRoute));
    CHECK_EQ(assignment.relayNodeId, relay.nodeId);
    CHECK_EQ(assignment.childNodeId, child.nodeId);
    CHECK_EQ(assignment.relayLease.replayNonce,
             bindingRequest.lease.replayNonce);
    CHECK_EQ(assignment.bindingRequestNonce, bindingRequest.requestNonce);
    CHECK_EQ(assignment.observedDataRoute.port, relayData.port);
    CHECK_EQ(relayControlRoute.port, relayQsr.port);
    CHECK_EQ(assignment.childLease.parentId, relay.nodeId);
    CHECK_EQ(assignment.childLease.childId, child.nodeId);
    CHECK_EQ(assignment.childLease.ancestryCount, static_cast<uint8_t>(2u));
    CHECK_EQ(assignment.childLease.ancestry[0], authority.facts().rootNodeId);
    CHECK_EQ(assignment.childLease.ancestry[1], relay.nodeId);
    CHECK_EQ(assignment.childLease.childCapacity, 2u);
    topology::TopologyAuthoritySignatureVerifier assignmentVerifier{};
    std::vector<uint8_t> assignmentAttestationInput;
    CHECK_TRUE(assignmentVerifier.initialize(
        authority.facts().assignmentSigningPublicKey));
    CHECK_TRUE(wire::encodeRelayChildAssignmentAttestationInput(
        assignment, assignmentAttestationInput));
    CHECK_TRUE(assignmentVerifier.verify(assignmentAttestationInput.data(),
                                         assignmentAttestationInput.size(),
                                         assignment.hostSignature));
    wire::RelayChildAssignment forgedAssignment = assignment;
    ++forgedAssignment.bindingRequestNonce;
    assignmentAttestationInput.clear();
    CHECK_TRUE(wire::encodeRelayChildAssignmentAttestationInput(
        forgedAssignment, assignmentAttestationInput));
    CHECK_TRUE(!assignmentVerifier.verify(assignmentAttestationInput.data(),
                                          assignmentAttestationInput.size(),
                                          forgedAssignment.hostSignature));
    CHECK_TRUE(!authority.isAttached(child.nodeId));
    CHECK_TRUE(!authority.reserveRootOffer(child.nodeId, kNowMs + 7u,
                                           offerNonce, offerExpiry));

    wire::ControlDatagram assignmentDatagram{};
    assignmentDatagram.type = wire::ControlMessageType::RelayChildAssignment;
    assignmentDatagram.relayChildAssignment = assignment;
    std::vector<uint8_t> assignmentBytes;
    CHECK_TRUE(wire::encodeControlDatagram(assignmentDatagram, assignmentBytes));
    wire::ControlDatagram decodedAssignment{};
    CHECK_EQ(wire::decodeControlDatagram(assignmentBytes.data(),
                                         assignmentBytes.size(),
                                         decodedAssignment),
             wire::DecodeResult::Ok);
    CHECK_EQ(decodedAssignment.type,
             wire::ControlMessageType::RelayChildAssignment);
    CHECK_EQ(decodedAssignment.relayChildAssignment.childLease.replayNonce,
             assignment.childLease.replayNonce);

    model::ParentLeaseChildBinding expectedChildBinding{};
    CHECK_TRUE(authority.parentLeaseChildBinding(child.nodeId,
                                                 expectedChildBinding));
    model::ParentLeaseExpectation expectation{};
    expectation.stream = assignment.childLease.stream;
    expectation.expectedChildId = child.nodeId;
    expectation.expectedParentId = relay.nodeId;
    expectation.expectedChildBinding = expectedChildBinding;
    expectation.nowMs = kNowMs + 7u;
    expectation.minimumSourceGeneration = authority.facts().sourceGeneration;
    expectation.minimumRouteGeneration = assignment.observedDataRoute.generation;
    expectation.requiredCursor = currentCursor.nextRecord;
    expectation.maxActiveDepth = authority.facts().limits.maxActiveDepth;
    expectation.parentHasCapacity = true;
    expectation.loopFree = true;
    model::ParentLeaseRegistry verifier(authority.leaseKeyForRuntime());
    CHECK_EQ(verifier.verifyContinuity(assignment.childLease, expectation),
             model::ParentLeaseVerdict::Accepted);

    wire::RelayChildAssignment overflowAssignment{};
    model::RouteDescriptor overflowControlRoute{};
    CHECK_TRUE(authority.issueRelayChildAssignment(
        relay.nodeId, overflow.nodeId, kNowMs + 7u, overflowAssignment,
        overflowControlRoute));
    CHECK_EQ(overflowAssignment.childLease.childCapacity, 1u);
    wire::RelayChildAssignment thirdAssignment{};
    model::RouteDescriptor thirdControlRoute{};
    CHECK_TRUE(!authority.issueRelayChildAssignment(
        relay.nodeId, third.nodeId, kNowMs + 7u, thirdAssignment,
        thirdControlRoute));

    model::NodeId detachedChild = model::kInvalidNodeId;
    CHECK_TRUE(authority.detachViewer(childProof.viewerId, detachedChild));
    CHECK_EQ(detachedChild, child.nodeId);
    topology::RelayChildRetirementRecipient queued{};
    CHECK_TRUE(authority.nextRelayChildRetirement(queued));
    CHECK_EQ(queued.qsrRoute.kind, model::RouteKind::Direct);
    CHECK_EQ(queued.qsrRoute.ipv4BE, relayQsr.ipv4BE);
    CHECK_EQ(queued.qsrRoute.port, relayQsr.port);
    CHECK_EQ(queued.retirement.relayNodeId, relay.nodeId);
    CHECK_EQ(queued.retirement.childNodeId, child.nodeId);
    CHECK_EQ(queued.retirement.bindingRequestNonce,
             assignment.bindingRequestNonce);
    CHECK_EQ(queued.retirement.reason,
             wire::RelayChildRetirementReason::ChildDetached);
    CHECK_EQ(queued.retirement.childLease.replayNonce,
             assignment.childLease.replayNonce);

    topology::TopologyQsrControl qsrControl{};
    topology::EndpointBinding qsrControlBinding{};
    topology::TopologyActivationRequest controlRequest = activationRequest();
    CHECK_TRUE(qsrControl.open(controlRequest, controlRequest.rootQsp,
                               qsrControlBinding, error));
    resilient::ResilientSpectatorRootRuntime inertRoot{};
    qsrControl.attach(authority, inertRoot);
    CHECK_TRUE(qsrControl.flushRelayChildRetirements());
    CHECK_TRUE(relayQsrReceiver.waitReadable(500));
    std::array<uint8_t, 2048u> retirementPacket{};
    qnet::Endpoint retirementSource{};
    const int receivedRetirement = relayQsrReceiver.recvFrom(
        retirementPacket.data(), retirementPacket.size(), retirementSource);
    CHECK_TRUE(receivedRetirement
               > static_cast<int>(kTopologyEnvelopePrefixBytes));
    if (receivedRetirement > static_cast<int>(kTopologyEnvelopePrefixBytes)) {
        CHECK_EQ(ntohs(retirementSource.sa.sin_port),
                 qsrControl.binding().local.port);
        wire::ControlDatagram received{};
        CHECK_EQ(wire::decodeControlDatagram(
                     retirementPacket.data() + kTopologyEnvelopePrefixBytes,
                     static_cast<size_t>(receivedRetirement)
                         - kTopologyEnvelopePrefixBytes,
                     received),
                 wire::DecodeResult::Ok);
        CHECK_EQ(received.type, wire::ControlMessageType::RelayChildRetirement);
        CHECK_EQ(received.relayChildRetirement.relayNodeId, relay.nodeId);
        CHECK_EQ(received.relayChildRetirement.childNodeId, child.nodeId);
        CHECK_EQ(received.relayChildRetirement.reason,
                 wire::RelayChildRetirementReason::ChildDetached);
    }
    CHECK_TRUE(!authority.nextRelayChildRetirement(queued));

    const topology::TopologyViewerProof reboundRelayProof = viewerProof(
        relayProof.viewerId, 45711u, relayProof.viewerGeneration + 1u,
        relayProof.routeGeneration + 1u, relayProof.confirmedCursor + 1u);
    CHECK_TRUE(authority.beginViewerRebind(reboundRelayProof, kNowMs + 9u,
                                           error));
    topology::TopologyEnrollment oldRelayLookup{};
    CHECK_TRUE(!authority.enrollmentForQsrEndpoint(relayQsr, oldRelayLookup));
    CHECK_TRUE(authority.nextRelayChildRetirement(queued));
    CHECK_EQ(queued.qsrRoute.ipv4BE, relayQsr.ipv4BE);
    CHECK_EQ(queued.qsrRoute.port, relayQsr.port);
    CHECK_EQ(queued.retirement.relayNodeId, relay.nodeId);
    CHECK_EQ(queued.retirement.childNodeId, overflow.nodeId);
    CHECK_EQ(queued.retirement.reason,
             wire::RelayChildRetirementReason::ChildBindingReplaced);
    CHECK_EQ(queued.retirement.bindingRequestNonce,
             overflowAssignment.bindingRequestNonce);
    CHECK_TRUE(qsrControl.flushRelayChildRetirements());
    CHECK_TRUE(relayQsrReceiver.waitReadable(500));
    const int receivedRebindRetirement = relayQsrReceiver.recvFrom(
        retirementPacket.data(), retirementPacket.size(), retirementSource);
    CHECK_TRUE(receivedRebindRetirement
               > static_cast<int>(kTopologyEnvelopePrefixBytes));
    if (receivedRebindRetirement > static_cast<int>(kTopologyEnvelopePrefixBytes)) {
        wire::ControlDatagram received{};
        CHECK_EQ(wire::decodeControlDatagram(
                     retirementPacket.data() + kTopologyEnvelopePrefixBytes,
                     static_cast<size_t>(receivedRebindRetirement)
                         - kTopologyEnvelopePrefixBytes,
                     received),
                 wire::DecodeResult::Ok);
        CHECK_EQ(received.type, wire::ControlMessageType::RelayChildRetirement);
        CHECK_EQ(received.relayChildRetirement.relayNodeId, relay.nodeId);
        CHECK_EQ(received.relayChildRetirement.childNodeId, overflow.nodeId);
        CHECK_EQ(received.relayChildRetirement.reason,
                 wire::RelayChildRetirementReason::ChildBindingReplaced);
    }
    CHECK_TRUE(!authority.nextRelayChildRetirement(queued));

    wire::RelayChildAssignment expiredAssignment{};
    model::RouteDescriptor expiredControlRoute{};
    CHECK_TRUE(!authority.issueRelayChildAssignment(
        relay.nodeId, overflow.nodeId, relayLease.expiryMs,
        expiredAssignment, expiredControlRoute));
}

void testStagedRelayChildActivationDefersGenericRootAdmission() {
    topology::TopologyAuthority authority{};
    topology::TopologyActivationRequest request = activationRequest();
    request.limits.maxChildrenPerRelay = 2u;
    CHECK_TRUE(initializeAuthority(authority, request));
    if (!authority.ready()) return;

    const model::VerifiedChainCursor currentCursor{
        60u, 9u, 0x57A6E00000112233ull};
    const model::VerifiedCacheRange currentRange{
        currentCursor.nextRecord, currentCursor.nextRecord};
    authority.setVerifiedCursor(currentCursor);

    const topology::TopologyViewerProof relayProof = viewerProof(
        0xD300000000000001ull, 48001u, 2u, 51u, 90u);
    const topology::TopologyViewerProof cancelledChildProof = viewerProof(
        0xD300000000000002ull, 48002u, 3u, 52u, 91u);
    const topology::TopologyViewerProof stagedChildProof = viewerProof(
        0xD300000000000003ull, 48003u, 4u, 53u, 92u);
    const RouteEndpoint relayQsr{0x0100007Fu, 49001u};
    const RouteEndpoint relayData{0x0100007Fu, 50001u};
    const RouteEndpoint cancelledChildQsr{0x0100007Fu, 49002u};
    const RouteEndpoint stagedChildQsr{0x0100007Fu, 49003u};
    topology::TopologyEnrollment relay{};
    topology::TopologyEnrollment cancelledChild{};
    topology::TopologyEnrollment stagedChild{};
    std::string error;
    CHECK_TRUE(authority.enroll(relayProof, kNowMs + 1u, relay, error));
    CHECK_TRUE(authority.enroll(cancelledChildProof, kNowMs + 1u,
                                cancelledChild, error));
    CHECK_TRUE(authority.enroll(stagedChildProof, kNowMs + 1u,
                                stagedChild, error));
    if (!relay.granted || !cancelledChild.granted || !stagedChild.granted)
        return;
    CHECK_TRUE(bindDistinctQsr(authority, relay, relayQsr, 1u,
                               kNowMs + 2u));

    uint64_t rootOfferNonce = 0u;
    uint64_t rootOfferExpiry = 0u;
    CHECK_TRUE(authority.reserveRootOffer(relay.nodeId, kNowMs + 2u,
                                          rootOfferNonce, rootOfferExpiry));
    const model::ParentLease relayLease =
        leaseFor(authority, relay, rootOfferNonce, rootOfferExpiry);
    CHECK_TRUE(authority.confirmRedemption(relayLease, kNowMs + 3u));

    const wire::RelayDataBindingRequest dataRequest =
        relayDataBindingRequest(authority, relay, relayLease,
                                0xD300000000000101ull);
    std::vector<uint8_t> requestBytes;
    std::vector<uint8_t> requestEnvelope;
    CHECK_TRUE(encodeRelayDataBindingRequest(dataRequest, requestBytes));
    CHECK_TRUE(authority.encodeControl(relay, 2u, requestBytes.data(),
                                       requestBytes.size(), requestEnvelope));
    topology::TopologyControlEnvelope decodedEnvelope{};
    CHECK_EQ(authority.decodeControl(relayData, requestEnvelope.data(),
                                     requestEnvelope.size(), kNowMs + 4u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    wire::RelayDataBindingAck dataAck{};
    CHECK_TRUE(authority.relayDataBindingAck(relay.nodeId, dataRequest,
                                              dataAck));
    const wire::RelayReady ready = relayReady(
        dataRequest, dataAck, currentRange, currentCursor);
    std::vector<uint8_t> readyBytes;
    std::vector<uint8_t> readyEnvelope;
    CHECK_TRUE(encodeRelayReady(ready, readyBytes));
    CHECK_TRUE(authority.encodeControl(relay, 3u, readyBytes.data(),
                                       readyBytes.size(), readyEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, readyEnvelope.data(),
                                     readyEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    CHECK_TRUE(authority.stageRelayChildActivation(
        relay.nodeId, cancelledChild.nodeId, kNowMs + 6u));
    CHECK_TRUE(authority.stageRelayChildActivation(
        relay.nodeId, cancelledChild.nodeId, kNowMs + 7u));
    CHECK_TRUE(!authority.reserveRootOffer(cancelledChild.nodeId,
                                           kNowMs + 7u,
                                           rootOfferNonce, rootOfferExpiry));

    const topology::TopologyViewerProof reboundCancelled = viewerProof(
        cancelledChildProof.viewerId, 48102u,
        cancelledChildProof.viewerGeneration + 1u,
        cancelledChildProof.routeGeneration + 1u,
        cancelledChildProof.confirmedCursor + 1u);
    CHECK_TRUE(authority.beginViewerRebind(reboundCancelled, kNowMs + 8u,
                                           error));
    CHECK_TRUE(authority.refreshViewerBinding(reboundCancelled, kNowMs + 9u,
                                              error));
    topology::TopologyEnrollment reboundCancelledEnrollment{};
    CHECK_TRUE(authority.enrollmentForNode(cancelledChild.nodeId,
                                           reboundCancelledEnrollment));
    CHECK_TRUE(bindDistinctQsr(authority, reboundCancelledEnrollment,
                               cancelledChildQsr, 1u, kNowMs + 10u));
    model::NodeId stagedRelayNodeId = model::kInvalidNodeId;
    CHECK_EQ(authority.consumeStagedRelayChildActivation(
                 cancelledChild.nodeId, kNowMs + 10u, stagedRelayNodeId),
             topology::StagedRelayChildActivationResult::None);
    CHECK_TRUE(authority.reserveRootOffer(cancelledChild.nodeId,
                                          kNowMs + 10u,
                                          rootOfferNonce, rootOfferExpiry));
    authority.releaseOffer(cancelledChild.nodeId, rootOfferNonce);

    CHECK_TRUE(authority.stageRelayChildActivation(
        relay.nodeId, stagedChild.nodeId, kNowMs + 11u));
    CHECK_TRUE(bindDistinctQsr(authority, stagedChild, stagedChildQsr, 1u,
                               kNowMs + 12u));
    CHECK_TRUE(!authority.reserveRootOffer(stagedChild.nodeId, kNowMs + 12u,
                                           rootOfferNonce, rootOfferExpiry));
    CHECK_EQ(authority.consumeStagedRelayChildActivation(
                 stagedChild.nodeId, kNowMs + 12u, stagedRelayNodeId),
             topology::StagedRelayChildActivationResult::Ready);
    CHECK_EQ(stagedRelayNodeId, relay.nodeId);
    CHECK_EQ(authority.consumeStagedRelayChildActivation(
                 stagedChild.nodeId, kNowMs + 12u, stagedRelayNodeId),
             topology::StagedRelayChildActivationResult::None);

    wire::RelayChildActivation activation{};
    model::RouteDescriptor relayControlRoute{};
    model::RouteDescriptor childControlRoute{};
    CHECK_TRUE(authority.issueRelayChildActivation(
        relay.nodeId, stagedChild.nodeId, kNowMs + 12u, activation,
        relayControlRoute, childControlRoute));
    CHECK_EQ(activation.relayNodeId, relay.nodeId);
    CHECK_EQ(activation.childNodeId, stagedChild.nodeId);
    CHECK_EQ(activation.relayControlRoute.port, relayQsr.port);
    CHECK_EQ(activation.childControlRoute.port, stagedChildQsr.port);
    CHECK_TRUE(!authority.reserveRootOffer(stagedChild.nodeId, kNowMs + 13u,
                                           rootOfferNonce, rootOfferExpiry));
}

void testPairedRelayChildActivationRequiresBothExactQtaRecipients() {
    topology::TopologyAuthority authority{};
    topology::TopologyActivationRequest request = activationRequest();
    request.limits.maxChildrenPerRelay = 2u;
    CHECK_TRUE(initializeAuthority(authority, request));
    if (!authority.ready()) return;

    const model::VerifiedChainCursor currentCursor{
        50u, 7u, 0xA17A7E0000112233ull};
    const model::VerifiedCacheRange currentRange{
        currentCursor.nextRecord, currentCursor.nextRecord};
    authority.setVerifiedCursor(currentCursor);

    const topology::TopologyViewerProof relayProof = viewerProof(
        0xD400000000000001ull, 48101u, 2u, 41u, 80u);
    const topology::TopologyViewerProof childProof = viewerProof(
        0xD400000000000002ull, 48102u, 3u, 42u, 81u);
    const RouteEndpoint relayQsr{0x0100007Fu, 49101u};
    const RouteEndpoint relayData{0x0100007Fu, 50101u};
    const RouteEndpoint childQsr{0x0100007Fu, 49102u};
    topology::TopologyEnrollment relay{};
    topology::TopologyEnrollment child{};
    std::string error;
    CHECK_TRUE(authority.enroll(relayProof, kNowMs + 1u, relay, error));
    CHECK_TRUE(authority.enroll(childProof, kNowMs + 1u, child, error));
    if (!relay.granted || !child.granted) return;
    CHECK_TRUE(bindDistinctQsr(authority, relay, relayQsr, 1u, kNowMs + 2u));
    CHECK_TRUE(bindDistinctQsr(authority, child, childQsr, 1u, kNowMs + 2u));

    uint64_t rootOfferNonce = 0u;
    uint64_t rootOfferExpiry = 0u;
    CHECK_TRUE(authority.reserveRootOffer(relay.nodeId, kNowMs + 2u,
                                          rootOfferNonce, rootOfferExpiry));
    const model::ParentLease relayLease =
        leaseFor(authority, relay, rootOfferNonce, rootOfferExpiry);
    CHECK_TRUE(authority.confirmRedemption(relayLease, kNowMs + 3u));

    const wire::RelayDataBindingRequest dataRequest =
        relayDataBindingRequest(authority, relay, relayLease,
                                0xD400000000000101ull);
    std::vector<uint8_t> requestBytes;
    std::vector<uint8_t> requestEnvelope;
    CHECK_TRUE(encodeRelayDataBindingRequest(dataRequest, requestBytes));
    CHECK_TRUE(authority.encodeControl(relay, 2u, requestBytes.data(),
                                       requestBytes.size(), requestEnvelope));
    topology::TopologyControlEnvelope decodedEnvelope{};
    CHECK_EQ(authority.decodeControl(relayData, requestEnvelope.data(),
                                     requestEnvelope.size(), kNowMs + 4u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    wire::RelayDataBindingAck dataAck{};
    CHECK_TRUE(authority.relayDataBindingAck(relay.nodeId, dataRequest,
                                              dataAck));
    const wire::RelayReady ready = relayReady(
        dataRequest, dataAck, currentRange, currentCursor);
    std::vector<uint8_t> readyBytes;
    std::vector<uint8_t> readyEnvelope;
    CHECK_TRUE(encodeRelayReady(ready, readyBytes));
    CHECK_TRUE(authority.encodeControl(relay, 3u, readyBytes.data(),
                                       readyBytes.size(), readyEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, readyEnvelope.data(),
                                     readyEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    wire::RelayChildActivation activation{};
    model::RouteDescriptor relayControlRoute{};
    model::RouteDescriptor childControlRoute{};
    CHECK_TRUE(authority.issueRelayChildActivation(
        relay.nodeId, child.nodeId, kNowMs + 6u, activation,
        relayControlRoute, childControlRoute));
    CHECK_EQ(activation.relayNodeId, relay.nodeId);
    CHECK_EQ(activation.childNodeId, child.nodeId);
    CHECK_EQ(activation.relayBindingRequestNonce, dataRequest.requestNonce);
    CHECK_EQ(activation.relayDataRoute.port, relayData.port);
    CHECK_EQ(activation.relayControlRoute.port, relayQsr.port);
    CHECK_EQ(activation.childDataRoute.port, childProof.endpoint.port);
    CHECK_EQ(activation.childControlRoute.port, childQsr.port);
    CHECK_EQ(activation.commonCursor.nextRecord, currentCursor.nextRecord);
    CHECK_EQ(activation.commonCursor.nextEvent, currentCursor.nextEvent);
    CHECK_EQ(activation.commonCursor.digest, currentCursor.digest);
    CHECK_EQ(activation.childLease.childId, child.nodeId);
    CHECK_EQ(activation.childLease.parentId, relay.nodeId);
    CHECK_EQ(activation.childLease.expiryMs, activation.activationDeadlineMs);
    CHECK_TRUE(activation.activationNonce != 0u);
    CHECK_EQ(relayControlRoute.port, relayQsr.port);
    CHECK_EQ(childControlRoute.port, childQsr.port);
    CHECK_TRUE(!authority.isAttached(child.nodeId));

    topology::TopologyAuthoritySignatureVerifier verifier{};
    std::vector<uint8_t> attestationInput;
    CHECK_TRUE(verifier.initialize(authority.facts().assignmentSigningPublicKey));
    CHECK_TRUE(wire::encodeRelayChildActivationAttestationInput(
        activation, attestationInput));
    CHECK_TRUE(verifier.verify(attestationInput.data(), attestationInput.size(),
                               activation.hostSignature));
    wire::RelayChildActivation forgedActivation = activation;
    ++forgedActivation.activationNonce;
    attestationInput.clear();
    CHECK_TRUE(wire::encodeRelayChildActivationAttestationInput(
        forgedActivation, attestationInput));
    CHECK_TRUE(!verifier.verify(attestationInput.data(),
                                attestationInput.size(),
                                forgedActivation.hostSignature));

    wire::RelayChildActivation retry{};
    model::RouteDescriptor retryRelayRoute{};
    model::RouteDescriptor retryChildRoute{};
    CHECK_TRUE(authority.issueRelayChildActivation(
        relay.nodeId, child.nodeId, kNowMs + 7u, retry, retryRelayRoute,
        retryChildRoute));
    CHECK_EQ(retry.activationNonce, activation.activationNonce);
    CHECK_EQ(retry.activationDeadlineMs, activation.activationDeadlineMs);
    CHECK_EQ(retryRelayRoute.port, relayControlRoute.port);
    CHECK_EQ(retryChildRoute.port, childControlRoute.port);
    CHECK_TRUE(!authority.reserveRootOffer(child.nodeId, kNowMs + 7u,
                                           rootOfferNonce, rootOfferExpiry));

    wire::RelayChildActivationReceipt childReceipt{};
    CHECK_TRUE(wire::makeRelayChildActivationReceipt(
        activation, wire::RelayChildActivationReceiptRole::Child,
        childReceipt));
    std::vector<uint8_t> receiptBytes;
    std::vector<uint8_t> receiptEnvelope;

    wire::RelayChildActivationReceipt wrongRole = childReceipt;
    wrongRole.role = wire::RelayChildActivationReceiptRole::Relay;
    CHECK_TRUE(encodeRelayChildActivationReceipt(wrongRole, receiptBytes));
    CHECK_TRUE(authority.encodeControl(child, 2u, receiptBytes.data(),
                                       receiptBytes.size(), receiptEnvelope));
    CHECK_EQ(authority.decodeControl(childQsr, receiptEnvelope.data(),
                                     receiptEnvelope.size(), kNowMs + 8u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);

    wire::RelayChildActivationReceipt wrongRoute = childReceipt;
    ++wrongRoute.relayDataRoute.port;
    receiptBytes.clear();
    receiptEnvelope.clear();
    CHECK_TRUE(encodeRelayChildActivationReceipt(wrongRoute, receiptBytes));
    CHECK_TRUE(authority.encodeControl(child, 2u, receiptBytes.data(),
                                       receiptBytes.size(), receiptEnvelope));
    CHECK_EQ(authority.decodeControl(childQsr, receiptEnvelope.data(),
                                     receiptEnvelope.size(), kNowMs + 8u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);

    receiptBytes.clear();
    receiptEnvelope.clear();
    CHECK_TRUE(encodeRelayChildActivationReceipt(childReceipt, receiptBytes));
    CHECK_TRUE(authority.encodeControl(child, 2u, receiptBytes.data(),
                                       receiptBytes.size(), receiptEnvelope));
    CHECK_EQ(authority.decodeControl(childQsr, receiptEnvelope.data(),
                                     receiptEnvelope.size(), kNowMs + 8u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    CHECK_TRUE(authority.encodeControl(child, 3u, receiptBytes.data(),
                                       receiptBytes.size(), receiptEnvelope));
    CHECK_EQ(authority.decodeControl(childQsr, receiptEnvelope.data(),
                                      receiptEnvelope.size(), kNowMs + 9u,
                                      decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    CHECK_TRUE(!authority.isAttached(child.nodeId));
    topology::RelayChildActivationCommitRecipient pendingCommit{};
    CHECK_TRUE(!authority.nextRelayChildActivationCommit(pendingCommit));

    wire::RelayChildActivationReceipt relayReceipt{};
    CHECK_TRUE(wire::makeRelayChildActivationReceipt(
        activation, wire::RelayChildActivationReceiptRole::Relay,
        relayReceipt));
    receiptBytes.clear();
    receiptEnvelope.clear();
    CHECK_TRUE(encodeRelayChildActivationReceipt(relayReceipt, receiptBytes));
    CHECK_TRUE(authority.encodeControl(relay, 4u, receiptBytes.data(),
                                       receiptBytes.size(), receiptEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, receiptEnvelope.data(),
                                      receiptEnvelope.size(), kNowMs + 9u,
                                      decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    pendingCommit = {};
    CHECK_TRUE(authority.nextRelayChildActivationCommit(pendingCommit));
    CHECK_EQ(pendingCommit.qsrRoute.kind, model::RouteKind::Direct);
    CHECK_EQ(pendingCommit.qsrRoute.ipv4BE, relayQsr.ipv4BE);
    CHECK_EQ(pendingCommit.qsrRoute.port, relayQsr.port);
    CHECK_EQ(pendingCommit.commit.activation.activationNonce,
             activation.activationNonce);
    CHECK_EQ(pendingCommit.commit.activation.activationDeadlineMs,
             activation.activationDeadlineMs);
    CHECK_TRUE(wire::validRelayChildActivationCommit(pendingCommit.commit));
    std::vector<uint8_t> commitAttestationInput;
    CHECK_TRUE(wire::encodeRelayChildActivationCommitAttestationInput(
        pendingCommit.commit, commitAttestationInput));
    CHECK_TRUE(verifier.verify(commitAttestationInput.data(),
                               commitAttestationInput.size(),
                               pendingCommit.commit.hostSignature));
    wire::RelayChildActivationCommit forgedCommit = pendingCommit.commit;
    ++forgedCommit.activation.activationNonce;
    commitAttestationInput.clear();
    CHECK_TRUE(wire::encodeRelayChildActivationCommitAttestationInput(
        forgedCommit, commitAttestationInput));
    CHECK_TRUE(!verifier.verify(commitAttestationInput.data(),
                                commitAttestationInput.size(),
                                forgedCommit.hostSignature));

    std::vector<uint8_t> commitBytes;
    CHECK_TRUE(encodeRelayChildActivationCommit(pendingCommit.commit,
                                                 commitBytes));
    std::vector<uint8_t> wrappedCommit;
    CHECK_TRUE(authority.encodeRelayChildActivationCommitControl(
        pendingCommit, 98u, commitBytes.data(), commitBytes.size(),
        wrappedCommit));
    CHECK_TRUE(wrappedCommit.size() > kTopologyEnvelopePrefixBytes);
    if (wrappedCommit.size() > kTopologyEnvelopePrefixBytes) {
        wire::ControlDatagram decodedCommit{};
        CHECK_EQ(wire::decodeControlDatagram(
                     wrappedCommit.data() + kTopologyEnvelopePrefixBytes,
                     wrappedCommit.size() - kTopologyEnvelopePrefixBytes,
                     decodedCommit),
                 wire::DecodeResult::Ok);
        CHECK_EQ(decodedCommit.type,
                 wire::ControlMessageType::RelayChildActivationCommit);
        CHECK_EQ(decodedCommit.relayChildActivationCommit.activation
                     .activationNonce,
                 activation.activationNonce);
    }
    CHECK_TRUE(authority.markRelayChildActivationCommitSent(
        pendingCommit, kNowMs + 9u));
    CHECK_TRUE(!authority.isAttached(child.nodeId));

    pendingCommit = {};
    CHECK_TRUE(authority.nextRelayChildActivationCommit(pendingCommit));
    CHECK_EQ(pendingCommit.qsrRoute.kind, model::RouteKind::Direct);
    CHECK_EQ(pendingCommit.qsrRoute.ipv4BE, childQsr.ipv4BE);
    CHECK_EQ(pendingCommit.qsrRoute.port, childQsr.port);
    CHECK_EQ(pendingCommit.commit.activation.activationNonce,
             activation.activationNonce);
    const topology::RelayChildActivationCommitRecipient staleChildCommit =
        pendingCommit;

    const topology::TopologyViewerProof reboundChild = viewerProof(
        childProof.viewerId, 48112u, childProof.viewerGeneration + 1u,
        childProof.routeGeneration + 1u, childProof.confirmedCursor + 1u);
    CHECK_TRUE(authority.beginViewerRebind(reboundChild, kNowMs + 10u, error));
    topology::TopologyEnrollment oldChildLookup{};
    CHECK_TRUE(!authority.enrollmentForQsrEndpoint(childQsr, oldChildLookup));
    pendingCommit = {};
    CHECK_TRUE(!authority.nextRelayChildActivationCommit(pendingCommit));
    CHECK_TRUE(!authority.markRelayChildActivationCommitSent(
        staleChildCommit, kNowMs + 11u));
    CHECK_TRUE(!authority.isAttached(child.nodeId));

    topology::RelayChildActivationRetirementRecipient queued{};
    CHECK_TRUE(authority.nextRelayChildActivationRetirement(queued));
    CHECK_EQ(queued.qsrRoute.kind, model::RouteKind::Direct);
    CHECK_EQ(queued.qsrRoute.ipv4BE, relayQsr.ipv4BE);
    CHECK_EQ(queued.qsrRoute.port, relayQsr.port);
    CHECK_EQ(queued.retirement.relayNodeId, relay.nodeId);
    CHECK_EQ(queued.retirement.childNodeId, child.nodeId);
    CHECK_EQ(queued.retirement.activationNonce, activation.activationNonce);
    CHECK_EQ(queued.retirement.activationDeadlineMs,
             activation.activationDeadlineMs);
    CHECK_EQ(queued.retirement.reason,
             wire::RelayChildRetirementReason::ChildBindingReplaced);
    CHECK_EQ(queued.retirement.relayDataRoute.port, relayData.port);
    CHECK_EQ(queued.retirement.childDataRoute.port, childProof.endpoint.port);

    std::vector<uint8_t> retirementBytes;
    CHECK_TRUE(encodeRelayChildActivationRetirement(queued.retirement,
                                                     retirementBytes));
    std::vector<uint8_t> wrappedRetirement;
    CHECK_TRUE(authority.encodeRelayChildActivationRetirementControl(
        queued, 100u, retirementBytes.data(), retirementBytes.size(),
        wrappedRetirement));
    CHECK_TRUE(wrappedRetirement.size() > kTopologyEnvelopePrefixBytes);
    if (wrappedRetirement.size() > kTopologyEnvelopePrefixBytes) {
        wire::ControlDatagram decodedRetirement{};
        CHECK_EQ(wire::decodeControlDatagram(
                     wrappedRetirement.data() + kTopologyEnvelopePrefixBytes,
                     wrappedRetirement.size() - kTopologyEnvelopePrefixBytes,
                     decodedRetirement),
                 wire::DecodeResult::Ok);
        CHECK_EQ(decodedRetirement.type,
                 wire::ControlMessageType::RelayChildActivationRetirement);
        CHECK_EQ(decodedRetirement.relayChildActivationRetirement.activationNonce,
                 activation.activationNonce);
    }
    std::vector<uint8_t> malformedRetirement = retirementBytes;
    if (!malformedRetirement.empty()) malformedRetirement.back() = 0u;
    std::vector<uint8_t> rejectedWrapped;
    CHECK_TRUE(!authority.encodeRelayChildActivationRetirementControl(
        queued, 101u, malformedRetirement.data(), malformedRetirement.size(),
        rejectedWrapped));
    CHECK_TRUE(rejectedWrapped.empty());
    CHECK_TRUE(authority.acknowledgeRelayChildActivationRetirement(queued));

    queued = {};
    CHECK_TRUE(authority.nextRelayChildActivationRetirement(queued));
    CHECK_EQ(queued.qsrRoute.kind, model::RouteKind::Direct);
    CHECK_EQ(queued.qsrRoute.ipv4BE, childQsr.ipv4BE);
    CHECK_EQ(queued.qsrRoute.port, childQsr.port);
    CHECK_EQ(queued.retirement.relayNodeId, relay.nodeId);
    CHECK_EQ(queued.retirement.childNodeId, child.nodeId);
    CHECK_EQ(queued.retirement.activationNonce, activation.activationNonce);
    CHECK_EQ(queued.retirement.reason,
             wire::RelayChildRetirementReason::ChildBindingReplaced);
    retirementBytes.clear();
    CHECK_TRUE(encodeRelayChildActivationRetirement(queued.retirement,
                                                     retirementBytes));
    wrappedRetirement.clear();
    CHECK_TRUE(authority.encodeRelayChildActivationRetirementControl(
        queued, 102u, retirementBytes.data(), retirementBytes.size(),
        wrappedRetirement));
    CHECK_TRUE(authority.acknowledgeRelayChildActivationRetirement(queued));
    CHECK_TRUE(!authority.nextRelayChildActivationRetirement(queued));

    CHECK_TRUE(!authority.issueRelayChildActivation(
        relay.nodeId, child.nodeId, kNowMs + 10u, retry, retryRelayRoute,
        retryChildRoute));

    CHECK_TRUE(authority.refreshViewerBinding(reboundChild, kNowMs + 11u,
                                              error));
    topology::TopologyEnrollment reboundEnrollment{};
    CHECK_TRUE(authority.enrollmentForNode(child.nodeId, reboundEnrollment));
    const RouteEndpoint reboundChildQsr{0x0100007Fu, 49112u};
    CHECK_TRUE(bindDistinctQsr(authority, reboundEnrollment, reboundChildQsr,
                               1u, kNowMs + 12u));
    CHECK_TRUE(authority.enrollmentForNode(child.nodeId, reboundEnrollment));
    wire::RelayChildActivation expiringActivation{};
    CHECK_TRUE(authority.issueRelayChildActivation(
        relay.nodeId, child.nodeId, kNowMs + 13u, expiringActivation,
        retryRelayRoute, retryChildRoute));
    std::vector<model::NodeId> detachedNodes;
    CHECK_TRUE(!authority.serviceExpiry(expiringActivation.activationDeadlineMs,
                                        detachedNodes));
    queued = {};
    CHECK_TRUE(authority.nextRelayChildActivationRetirement(queued));
    CHECK_EQ(queued.qsrRoute.port, relayQsr.port);
    CHECK_EQ(queued.retirement.activationNonce,
             expiringActivation.activationNonce);
    CHECK_EQ(queued.retirement.reason,
             wire::RelayChildRetirementReason::LeaseExpired);
    CHECK_TRUE(authority.acknowledgeRelayChildActivationRetirement(queued));
    queued = {};
    CHECK_TRUE(authority.nextRelayChildActivationRetirement(queued));
    CHECK_EQ(queued.qsrRoute.port, reboundChildQsr.port);
    CHECK_EQ(queued.retirement.activationNonce,
             expiringActivation.activationNonce);
    CHECK_EQ(queued.retirement.reason,
             wire::RelayChildRetirementReason::LeaseExpired);
    CHECK_TRUE(authority.acknowledgeRelayChildActivationRetirement(queued));
    CHECK_TRUE(!authority.nextRelayChildActivationRetirement(queued));
}

void testFinalCommitEstablishesAuthorityAdjacencyAndGatesGrandchild() {
    topology::TopologyAuthority authority{};
    topology::TopologyActivationRequest request = activationRequest();
    request.limits.maxActiveDepth = 4u;
    request.limits.maxChildrenPerRelay = 3u;
    CHECK_TRUE(initializeAuthority(authority, request));
    if (!authority.ready()) return;

    const model::VerifiedChainCursor currentCursor{
        70u, 11u, 0xA217A7E000112233ull};
    const model::VerifiedCacheRange currentRange{
        currentCursor.nextRecord, currentCursor.nextRecord};
    authority.setVerifiedCursor(currentCursor);

    const topology::TopologyViewerProof childProof = viewerProof(
        0xD500000000000002ull, 48202u, 3u, 62u, 102u);
    const topology::TopologyViewerProof grandchildProof = viewerProof(
        0xD500000000000003ull, 48203u, 4u, 63u, 103u);
    const topology::TopologyViewerProof relayProof = viewerProof(
        0xD500000000000001ull, 48201u, 2u, 61u, 101u);
    const RouteEndpoint relayQsr{0x0100007Fu, 49201u};
    const RouteEndpoint childQsr{0x0100007Fu, 49202u};
    const RouteEndpoint grandchildQsr{0x0100007Fu, 49203u};
    const RouteEndpoint relayData{0x0100007Fu, 50201u};
    const RouteEndpoint childRelayData{0x0100007Fu, 50202u};
    topology::TopologyEnrollment relay{};
    topology::TopologyEnrollment child{};
    topology::TopologyEnrollment grandchild{};
    std::string error;
    CHECK_TRUE(authority.enroll(childProof, kNowMs + 1u, child, error));
    CHECK_TRUE(authority.enroll(grandchildProof, kNowMs + 2u, grandchild,
                                error));
    CHECK_TRUE(authority.enroll(relayProof, kNowMs + 101u, relay, error));
    if (!relay.granted || !child.granted || !grandchild.granted) return;
    CHECK_TRUE(bindDistinctQsr(authority, child, childQsr, 1u,
                               kNowMs + 102u));
    CHECK_TRUE(bindDistinctQsr(authority, grandchild, grandchildQsr, 1u,
                               kNowMs + 102u));
    CHECK_TRUE(bindDistinctQsr(authority, relay, relayQsr, 1u,
                               kNowMs + 102u));

    uint64_t rootOfferNonce = 0u;
    uint64_t rootOfferExpiry = 0u;
    CHECK_TRUE(authority.reserveRootOffer(relay.nodeId, kNowMs + 102u,
                                           rootOfferNonce, rootOfferExpiry));
    const model::ParentLease relayLease =
        leaseFor(authority, relay, rootOfferNonce, rootOfferExpiry);
    CHECK_TRUE(authority.confirmRedemption(relayLease, kNowMs + 103u));

    const wire::RelayDataBindingRequest relayBindingRequest =
        ::relayDataBindingRequest(authority, relay, relayLease,
                                  0xD500000000000101ull);
    std::vector<uint8_t> relayBindingBytes;
    std::vector<uint8_t> relayBindingEnvelope;
    topology::TopologyControlEnvelope decodedEnvelope{};
    CHECK_TRUE(encodeRelayDataBindingRequest(relayBindingRequest,
                                              relayBindingBytes));
    CHECK_TRUE(authority.encodeControl(
        relay, 2u, relayBindingBytes.data(), relayBindingBytes.size(),
        relayBindingEnvelope));
    CHECK_EQ(authority.decodeControl(relayData, relayBindingEnvelope.data(),
                                     relayBindingEnvelope.size(),
                                     kNowMs + 104u, decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    wire::RelayDataBindingAck relayBindingAck{};
    CHECK_TRUE(authority.relayDataBindingAck(relay.nodeId, relayBindingRequest,
                                              relayBindingAck));
    const wire::RelayReady relayReadyMessage = relayReady(
        relayBindingRequest, relayBindingAck, currentRange, currentCursor);
    std::vector<uint8_t> relayReadyBytes;
    std::vector<uint8_t> relayReadyEnvelope;
    CHECK_TRUE(encodeRelayReady(relayReadyMessage, relayReadyBytes));
    CHECK_TRUE(authority.encodeControl(relay, 3u, relayReadyBytes.data(),
                                       relayReadyBytes.size(),
                                       relayReadyEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, relayReadyEnvelope.data(),
                                     relayReadyEnvelope.size(),
                                     kNowMs + 105u, decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    wire::RelayChildActivation activation{};
    model::RouteDescriptor relayControlRoute{};
    model::RouteDescriptor childControlRoute{};
    CHECK_TRUE(authority.issueRelayChildActivation(
        relay.nodeId, child.nodeId, kNowMs + 106u, activation,
        relayControlRoute, childControlRoute));
    CHECK_EQ(activation.childLease.parentDepth, static_cast<uint8_t>(1u));
    CHECK_EQ(activation.childLease.ancestryCount, static_cast<uint8_t>(2u));
    CHECK_EQ(activation.childLease.ancestry[0], authority.facts().rootNodeId);
    CHECK_EQ(activation.childLease.ancestry[1], relay.nodeId);
    CHECK_TRUE(!authority.isAttached(child.nodeId));

    wire::RelayChildActivation prematureGrandchild{};
    model::RouteDescriptor prematureRelayRoute{};
    model::RouteDescriptor prematureChildRoute{};
    CHECK_TRUE(!authority.issueRelayChildActivation(
        child.nodeId, grandchild.nodeId, kNowMs + 106u,
        prematureGrandchild, prematureRelayRoute, prematureChildRoute));

    wire::RelayChildActivationReceipt childReceipt{};
    wire::RelayChildActivationReceipt relayReceipt{};
    CHECK_TRUE(wire::makeRelayChildActivationReceipt(
        activation, wire::RelayChildActivationReceiptRole::Child,
        childReceipt));
    CHECK_TRUE(wire::makeRelayChildActivationReceipt(
        activation, wire::RelayChildActivationReceiptRole::Relay,
        relayReceipt));
    std::vector<uint8_t> receiptBytes;
    std::vector<uint8_t> receiptEnvelope;
    CHECK_TRUE(encodeRelayChildActivationReceipt(childReceipt, receiptBytes));
    CHECK_TRUE(authority.encodeControl(child, 2u, receiptBytes.data(),
                                       receiptBytes.size(), receiptEnvelope));
    CHECK_EQ(authority.decodeControl(childQsr, receiptEnvelope.data(),
                                     receiptEnvelope.size(), kNowMs + 107u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    receiptBytes.clear();
    receiptEnvelope.clear();
    CHECK_TRUE(encodeRelayChildActivationReceipt(relayReceipt, receiptBytes));
    CHECK_TRUE(authority.encodeControl(relay, 4u, receiptBytes.data(),
                                       receiptBytes.size(), receiptEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, receiptEnvelope.data(),
                                     receiptEnvelope.size(), kNowMs + 108u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    topology::RelayChildActivationCommitRecipient pendingCommit{};
    CHECK_TRUE(authority.nextRelayChildActivationCommit(pendingCommit));
    CHECK_EQ(pendingCommit.qsrRoute.port, relayQsr.port);
    topology::RelayChildActivationCommitRecipient forgedCommit = pendingCommit;
    forgedCommit.commit.hostSignature[0] ^= 0x80u;
    CHECK_TRUE(!authority.markRelayChildActivationCommitSent(
        forgedCommit, kNowMs + 108u));
    CHECK_TRUE(!authority.isAttached(child.nodeId));
    CHECK_TRUE(authority.markRelayChildActivationCommitSent(
        pendingCommit, kNowMs + 108u));
    CHECK_TRUE(!authority.isAttached(child.nodeId));

    pendingCommit = {};
    CHECK_TRUE(authority.nextRelayChildActivationCommit(pendingCommit));
    CHECK_EQ(pendingCommit.qsrRoute.port, childQsr.port);
    CHECK_TRUE(authority.markRelayChildActivationCommitSent(
        pendingCommit, kNowMs + 109u));
    CHECK_TRUE(authority.isAttached(child.nodeId));
    pendingCommit = {};
    CHECK_TRUE(!authority.nextRelayChildActivationCommit(pendingCommit));

    CHECK_TRUE(!authority.issueRelayChildActivation(
        child.nodeId, grandchild.nodeId, kNowMs + 109u,
        prematureGrandchild, prematureRelayRoute, prematureChildRoute));
    const wire::RelayDataBindingRequest childBindingRequest =
        ::relayDataBindingRequest(authority, child, activation.childLease,
                                  0xD500000000000102ull);
    std::vector<uint8_t> childBindingBytes;
    std::vector<uint8_t> childBindingEnvelope;
    CHECK_TRUE(encodeRelayDataBindingRequest(childBindingRequest,
                                              childBindingBytes));
    CHECK_TRUE(authority.encodeControl(
        child, 3u, childBindingBytes.data(), childBindingBytes.size(),
        childBindingEnvelope));
    CHECK_EQ(authority.decodeControl(childRelayData, childBindingEnvelope.data(),
                                     childBindingEnvelope.size(),
                                     kNowMs + 110u, decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    wire::RelayDataBindingAck childBindingAck{};
    CHECK_TRUE(authority.relayDataBindingAck(child.nodeId, childBindingRequest,
                                              childBindingAck));
    const wire::RelayReady childReady = relayReady(
        childBindingRequest, childBindingAck, currentRange, currentCursor);
    std::vector<uint8_t> childReadyBytes;
    std::vector<uint8_t> childReadyEnvelope;
    CHECK_TRUE(encodeRelayReady(childReady, childReadyBytes));
    CHECK_TRUE(authority.encodeControl(child, 4u, childReadyBytes.data(),
                                       childReadyBytes.size(),
                                       childReadyEnvelope));
    CHECK_EQ(authority.decodeControl(childQsr, childReadyEnvelope.data(),
                                     childReadyEnvelope.size(),
                                     kNowMs + 111u, decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    wire::RelayChildActivation grandchildActivation{};
    model::RouteDescriptor grandchildRelayRoute{};
    model::RouteDescriptor grandchildChildRoute{};
    CHECK_TRUE(authority.issueRelayChildActivation(
        child.nodeId, grandchild.nodeId, kNowMs + 112u,
        grandchildActivation, grandchildRelayRoute, grandchildChildRoute));
    CHECK_EQ(grandchildActivation.childLease.parentDepth,
             static_cast<uint8_t>(2u));
    CHECK_EQ(grandchildActivation.childLease.ancestryCount,
             static_cast<uint8_t>(3u));
    CHECK_EQ(grandchildActivation.childLease.ancestry[0],
             authority.facts().rootNodeId);
    CHECK_EQ(grandchildActivation.childLease.ancestry[1], relay.nodeId);
    CHECK_EQ(grandchildActivation.childLease.ancestry[2], child.nodeId);

    model::ParentLeaseChildBinding rootOwnedRelayBefore{};
    CHECK_TRUE(authority.parentLeaseChildBinding(relay.nodeId,
                                                 rootOwnedRelayBefore));
    topology::TopologyViewerProof reboundChild = viewerProof(
        childProof.viewerId, 48212u, childProof.viewerGeneration + 1u,
        childProof.routeGeneration + 1u, childProof.confirmedCursor + 1u);
    reboundChild.qspStreaming = false;
    topology::TopologyAuthority::ViewerLifecycleResult relayRebind{};
    CHECK_TRUE(authority.beginViewerRebind(
        reboundChild, kNowMs + 113u, relayRebind, error));
    CHECK_EQ(relayRebind.nodeId, child.nodeId);
    CHECK_TRUE(relayRebind.releasedAttachment);
    CHECK_EQ(relayRebind.releasedAttachmentOwner,
             topology::TopologyAuthority::AttachmentOwner::RelayActivation);
    CHECK_TRUE(!relayRebind.requiresGenericRootDetach());
    CHECK_TRUE(!authority.isAttached(child.nodeId));
    CHECK_TRUE(authority.isAttached(relay.nodeId));
    model::ParentLeaseChildBinding rootOwnedRelayAfter{};
    CHECK_TRUE(authority.parentLeaseChildBinding(relay.nodeId,
                                                 rootOwnedRelayAfter));
    CHECK_EQ(rootOwnedRelayAfter, rootOwnedRelayBefore);

    topology::TopologyViewerProof refreshedChild = reboundChild;
    refreshedChild.qspStreaming = true;
    topology::TopologyAuthority::ViewerLifecycleResult refresh{};
    CHECK_TRUE(authority.refreshViewerBinding(
        refreshedChild, kNowMs + 114u, refresh, error));
    CHECK_TRUE(!refresh.releasedAttachment);
    CHECK_TRUE(!refresh.requiresGenericRootDetach());
    topology::TopologyEnrollment reboundEnrollment{};
    CHECK_TRUE(authority.enrollmentForNode(child.nodeId, reboundEnrollment));
    const RouteEndpoint reboundChildQsr{0x0100007Fu, 49212u};
    CHECK_TRUE(bindDistinctQsr(authority, reboundEnrollment, reboundChildQsr,
                               1u, kNowMs + 115u));

    wire::RelayChildActivation replacementActivation{};
    model::RouteDescriptor replacementRelayControlRoute{};
    model::RouteDescriptor replacementChildControlRoute{};
    CHECK_TRUE(authority.issueRelayChildActivation(
        relay.nodeId, child.nodeId, kNowMs + 116u, replacementActivation,
        replacementRelayControlRoute, replacementChildControlRoute));
    wire::RelayChildActivationReceipt replacementChildReceipt{};
    wire::RelayChildActivationReceipt replacementRelayReceipt{};
    CHECK_TRUE(wire::makeRelayChildActivationReceipt(
        replacementActivation, wire::RelayChildActivationReceiptRole::Child,
        replacementChildReceipt));
    CHECK_TRUE(wire::makeRelayChildActivationReceipt(
        replacementActivation, wire::RelayChildActivationReceiptRole::Relay,
        replacementRelayReceipt));
    receiptBytes.clear();
    receiptEnvelope.clear();
    CHECK_TRUE(encodeRelayChildActivationReceipt(replacementChildReceipt,
                                                  receiptBytes));
    CHECK_TRUE(authority.encodeControl(reboundEnrollment, 2u,
                                       receiptBytes.data(), receiptBytes.size(),
                                       receiptEnvelope));
    CHECK_EQ(authority.decodeControl(reboundChildQsr, receiptEnvelope.data(),
                                     receiptEnvelope.size(), kNowMs + 117u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    receiptBytes.clear();
    receiptEnvelope.clear();
    CHECK_TRUE(encodeRelayChildActivationReceipt(replacementRelayReceipt,
                                                  receiptBytes));
    CHECK_TRUE(authority.encodeControl(relay, 5u, receiptBytes.data(),
                                       receiptBytes.size(), receiptEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, receiptEnvelope.data(),
                                     receiptEnvelope.size(), kNowMs + 117u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    for (uint64_t offset = 0u; offset < 2u; ++offset) {
        pendingCommit = {};
        CHECK_TRUE(authority.nextRelayChildActivationCommit(pendingCommit));
        CHECK_TRUE(authority.markRelayChildActivationCommitSent(
            pendingCommit, kNowMs + 118u + offset));
    }
    pendingCommit = {};
    CHECK_TRUE(!authority.nextRelayChildActivationCommit(pendingCommit));
    CHECK_TRUE(authority.isAttached(child.nodeId));

    topology::TopologyAuthority::ViewerLifecycleResult relayDetach{};
    CHECK_TRUE(authority.detachViewer(childProof.viewerId, relayDetach));
    CHECK_EQ(relayDetach.nodeId, child.nodeId);
    CHECK_TRUE(relayDetach.releasedAttachment);
    CHECK_EQ(relayDetach.releasedAttachmentOwner,
             topology::TopologyAuthority::AttachmentOwner::RelayActivation);
    CHECK_TRUE(!relayDetach.requiresGenericRootDetach());
    CHECK_TRUE(authority.isAttached(relay.nodeId));
    model::ParentLeaseChildBinding rootOwnedRelayAfterDetach{};
    CHECK_TRUE(authority.parentLeaseChildBinding(relay.nodeId,
                                                 rootOwnedRelayAfterDetach));
    CHECK_EQ(rootOwnedRelayAfterDetach, rootOwnedRelayBefore);
}

void testClientDataRootRequiresExactAuthenticatedImmutablePrefix() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    const model::VerifiedChainCursor currentCursor{
        50u, 4u, 0xC11E17D00D112233ull};
    const model::VerifiedCacheRange currentRange{
        currentCursor.nextRecord, currentCursor.nextRecord};
    authority.setVerifiedCursor(currentCursor);

    const topology::TopologyViewerProof relayProof =
        viewerProof(0xA900000000000001ull, 45901u, 2u, 41u, 50u);
    const RouteEndpoint relayQsr{0x0100007Fu, 46901u};
    const RouteEndpoint relayData{0x0100007Fu, 47901u};
    topology::TopologyEnrollment relay{};
    std::string error;
    CHECK_TRUE(authority.enroll(relayProof, kNowMs + 1u, relay, error));
    if (!relay.granted) return;
    CHECK_TRUE(bindDistinctQsr(authority, relay, relayQsr, 1u,
                               kNowMs + 2u));

    uint64_t offerNonce = 0u;
    uint64_t offerExpiry = 0u;
    CHECK_TRUE(authority.reserveRootOffer(relay.nodeId, kNowMs + 2u,
                                          offerNonce, offerExpiry));
    const model::ParentLease relayLease =
        leaseFor(authority, relay, offerNonce, offerExpiry);
    CHECK_TRUE(authority.confirmRedemption(relayLease, kNowMs + 3u));

    const wire::RelayDataBindingRequest bindingRequest =
        relayDataBindingRequest(authority, relay, relayLease,
                                0xA900000000000101ull);
    std::vector<uint8_t> bindingBytes;
    std::vector<uint8_t> bindingEnvelope;
    CHECK_TRUE(encodeRelayDataBindingRequest(bindingRequest, bindingBytes));
    CHECK_TRUE(authority.encodeControl(relay, 2u, bindingBytes.data(),
                                       bindingBytes.size(), bindingEnvelope));
    topology::TopologyControlEnvelope decodedEnvelope{};
    CHECK_EQ(authority.decodeControl(relayData, bindingEnvelope.data(),
                                     bindingEnvelope.size(), kNowMs + 4u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    wire::RelayDataBindingAck bindingAck{};
    CHECK_TRUE(authority.relayDataBindingAck(relay.nodeId, bindingRequest,
                                              bindingAck));

    const wire::RelayReady ready = relayReady(
        bindingRequest, bindingAck, currentRange, currentCursor);
    wire::ClientDataRootAssertion assertion = clientDataRootAssertion(
        authority, relay, bindingAck, ready, 3u);
    std::vector<uint8_t> assertionBytes;
    std::vector<uint8_t> assertionEnvelope;
    CHECK_TRUE(encodeClientDataRootAssertion(assertion, assertionBytes));

    CHECK_TRUE(authority.encodeControl(relay, 3u, assertionBytes.data(),
                                       assertionBytes.size(), assertionEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, assertionEnvelope.data(),
                                     assertionEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);
    CHECK_EQ(authority.clientDataRootCount(), 0u);

    std::vector<uint8_t> readyBytes;
    std::vector<uint8_t> readyEnvelope;
    CHECK_TRUE(encodeRelayReady(ready, readyBytes));
    CHECK_TRUE(authority.encodeControl(relay, 3u, readyBytes.data(),
                                       readyBytes.size(), readyEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, readyEnvelope.data(),
                                     readyEnvelope.size(), kNowMs + 5u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    CHECK_TRUE(authority.encodeControl(relay, 4u, assertionBytes.data(),
                                       assertionBytes.size(), assertionEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, assertionEnvelope.data(),
                                     assertionEnvelope.size(), kNowMs + 6u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    CHECK_EQ(authority.clientDataRootCount(), 1u);
    topology::ClientDataRootDescriptor descriptor{};
    CHECK_TRUE(authority.clientDataRootForNode(relay.nodeId, kNowMs + 6u,
                                               descriptor));
    CHECK_TRUE(descriptor.registered);
    CHECK_EQ(descriptor.nodeId, relay.nodeId);
    CHECK_EQ(descriptor.stream, assertion.stream);
    CHECK_EQ(descriptor.gameStartFingerprint,
             authority.facts().gameStartFingerprint);
    CHECK_EQ(descriptor.qspRouteGeneration, relay.qspRouteGeneration);
    CHECK_EQ(descriptor.qsrBindingGeneration, relay.bindingGeneration);
    CHECK_EQ(descriptor.dataRoute.port, relayData.port);
    CHECK_EQ(descriptor.qsrRoute.port, relayQsr.port);
    CHECK_EQ(descriptor.cursor.nextRecord, currentCursor.nextRecord);
    CHECK_EQ(descriptor.cursor.nextEvent, currentCursor.nextEvent);
    CHECK_EQ(descriptor.cursor.digest, currentCursor.digest);
    CHECK_EQ(descriptor.childCapacity, 3u);

    topology::ClientDataRootDescriptor expiredLookup{};
    CHECK_TRUE(!authority.clientDataRootForNode(
        relay.nodeId, descriptor.expiresMs, expiredLookup));

    CHECK_TRUE(authority.encodeControl(relay, 5u, assertionBytes.data(),
                                       assertionBytes.size(), assertionEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, assertionEnvelope.data(),
                                     assertionEnvelope.size(), kNowMs + 7u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    CHECK_EQ(authority.clientDataRootCount(), 1u);

    auto expectRejected = [&](const wire::ClientDataRootAssertion& candidate) {
        std::vector<uint8_t> candidateBytes;
        std::vector<uint8_t> candidateEnvelope;
        CHECK_TRUE(encodeClientDataRootAssertion(candidate, candidateBytes));
        if (candidateBytes.empty()) return;
        CHECK_TRUE(authority.encodeControl(relay, 6u, candidateBytes.data(),
                                           candidateBytes.size(),
                                           candidateEnvelope));
        CHECK_EQ(authority.decodeControl(relayQsr, candidateEnvelope.data(),
                                         candidateEnvelope.size(), kNowMs + 8u,
                                         decodedEnvelope),
                 topology::TopologyControlResult::BindingRejected);
        CHECK_EQ(authority.clientDataRootCount(), 1u);
    };

    wire::ClientDataRootAssertion wrong = assertion;
    ++wrong.stream.setOrSessionId;
    expectRejected(wrong);
    wrong = assertion;
    ++wrong.stream.gameEpoch;
    expectRejected(wrong);
    wrong = assertion;
    ++wrong.stream.compatibility.simulationContent.simulationRootSha256[0];
    expectRejected(wrong);
    wrong = assertion;
    ++wrong.stream.compatibility.runtimeBuild.peTimestamp;
    expectRejected(wrong);
    wrong = assertion;
    ++wrong.stream.streamRootIdentity;
    expectRejected(wrong);
    wrong = assertion;
    ++wrong.gameStartFingerprint;
    expectRejected(wrong);
    wrong = assertion;
    ++wrong.sourceGeneration;
    expectRejected(wrong);
    wrong = assertion;
    ++wrong.qspRouteGeneration;
    expectRejected(wrong);
    wrong = assertion;
    ++wrong.qsrBindingGeneration;
    ++wrong.dataRoute.generation;
    ++wrong.qsrRoute.generation;
    expectRejected(wrong);
    wrong = assertion;
    ++wrong.dataRoute.port;
    expectRejected(wrong);
    wrong = assertion;
    wrong.childCapacity = static_cast<uint32_t>(
        authority.facts().limits.maxChildrenPerRoot + 1u);
    expectRejected(wrong);
    wrong = assertion;
    wrong.clientRootNodeId = authority.facts().rootNodeId;
    expectRejected(wrong);

    wrong = assertion;
    ++wrong.cursor.nextRecord;
    ++wrong.cursor.nextEvent;
    wrong.cursor.digest ^= 0x01u;
    wrong.cacheRange = {wrong.cursor.nextRecord, wrong.cursor.nextRecord};
    expectRejected(wrong);
    wrong = assertion;
    --wrong.cursor.nextRecord;
    --wrong.cursor.nextEvent;
    wrong.cursor.digest ^= 0x01u;
    wrong.cacheRange = {0u, wrong.cursor.nextRecord};
    expectRejected(wrong);

    const model::VerifiedChainCursor advancedCursor{
        51u, 5u, 0xC11E17D00D445566ull};
    authority.setVerifiedCursor(advancedCursor);
    CHECK_EQ(authority.clientDataRootCount(), 0u);
    descriptor = {};
    CHECK_TRUE(!authority.clientDataRootForNode(relay.nodeId, kNowMs + 10u,
                                                descriptor));
    CHECK_TRUE(authority.encodeControl(relay, 6u, assertionBytes.data(),
                                       assertionBytes.size(), assertionEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, assertionEnvelope.data(),
                                     assertionEnvelope.size(), kNowMs + 9u,
                                     decodedEnvelope),
             topology::TopologyControlResult::BindingRejected);

    const wire::RelayReady advancedReady = relayReady(
        bindingRequest, bindingAck,
        model::VerifiedCacheRange{currentRange.firstCursor,
                                  advancedCursor.nextRecord},
        advancedCursor);
    std::vector<uint8_t> advancedReadyBytes;
    CHECK_TRUE(encodeRelayReady(advancedReady, advancedReadyBytes));
    CHECK_TRUE(authority.encodeControl(relay, 6u, advancedReadyBytes.data(),
                                       advancedReadyBytes.size(), readyEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, readyEnvelope.data(),
                                     readyEnvelope.size(), kNowMs + 9u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    const wire::ClientDataRootAssertion advancedAssertion =
        clientDataRootAssertion(authority, relay, bindingAck, advancedReady,
                                3u);
    std::vector<uint8_t> advancedAssertionBytes;
    CHECK_TRUE(encodeClientDataRootAssertion(advancedAssertion,
                                              advancedAssertionBytes));
    CHECK_TRUE(authority.encodeControl(relay, 7u, advancedAssertionBytes.data(),
                                       advancedAssertionBytes.size(),
                                       assertionEnvelope));
    CHECK_EQ(authority.decodeControl(relayQsr, assertionEnvelope.data(),
                                     assertionEnvelope.size(), kNowMs + 10u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    CHECK_EQ(authority.clientDataRootCount(), 1u);

    const topology::TopologyViewerProof rebound = viewerProof(
        relayProof.viewerId, 45911u, relayProof.viewerGeneration + 1u,
        relayProof.routeGeneration + 1u, relayProof.confirmedCursor + 1u);
    CHECK_TRUE(authority.beginViewerRebind(rebound, kNowMs + 11u, error));
    CHECK_EQ(authority.clientDataRootCount(), 0u);
    descriptor = {};
    CHECK_TRUE(!authority.clientDataRootForNode(relay.nodeId, kNowMs + 11u,
                                                descriptor));

    authority.revoke();
    descriptor = {};
    CHECK_TRUE(!authority.clientDataRootForNode(relay.nodeId, kNowMs + 12u,
                                                descriptor));
}

void testClientPlayerRootRequiresSeparateClaimAssertionAndExactTarget() {
    topology::TopologyAuthority authority{};
    CHECK_TRUE(initializeAuthority(authority));
    if (!authority.ready()) return;

    const topology::TopologyViewerProof targetProof = viewerProof(
        0xCA11000000000001ull, 45921u, 2u, 31u, 0u);
    topology::TopologyEnrollment target{};
    std::string error;
    CHECK_TRUE(authority.enroll(targetProof, kNowMs + 1u, target, error));
    if (!target.granted) return;
    const RouteEndpoint targetQsr{0x0100007Fu, 46921u};
    CHECK_TRUE(bindDistinctQsr(authority, target, targetQsr, 1u,
                               kNowMs + 2u));

    topology::ClientPlayerRootClaim claim = clientPlayerRootClaim(
        authority, wire::ClientPlayerRootRole::RemotePlayerTwo, 47921u,
        48921u);
    topology::ClientPlayerRootClaim wrongClaim = claim;
    ++wrongClaim.compatibility.runtimeBuild.peTimestamp;
    topology::ClientPlayerRootEnrollment rejected{};
    CHECK_TRUE(!authority.enrollClientPlayerRoot(
        wrongClaim, kNowMs + 3u, rejected, error));
    CHECK_EQ(authority.clientPlayerRootCount(), 0u);

    topology::ClientPlayerRootEnrollment root{};
    CHECK_TRUE(authority.enrollClientPlayerRoot(claim, kNowMs + 3u, root,
                                                error));
    if (!root.granted) return;
    CHECK_EQ(authority.clientPlayerRootCount(), 1u);
    CHECK_TRUE(root.clientRootNodeId != target.nodeId);
    CHECK_TRUE(root.clientRootNodeId != authority.facts().rootNodeId);

    topology::TopologyAuthoritySignatureVerifier verifier{};
    CHECK_TRUE(verifier.initialize(
        authority.facts().assignmentSigningPublicKey));
    std::vector<uint8_t> grantInput;
    CHECK_TRUE(wire::encodeClientPlayerRootGrantAttestationInput(root.grant,
                                                                  grantInput));
    CHECK_TRUE(verifier.verify(grantInput.data(), grantInput.size(),
                               root.grant.hostSignature));

    topology::ClientPlayerRootDescriptor descriptor{};
    CHECK_TRUE(authority.clientPlayerRootForNode(root.clientRootNodeId,
                                                 kNowMs + 3u, descriptor));
    CHECK_TRUE(descriptor.enrolled);
    CHECK_TRUE(!descriptor.asserted);
    CHECK_EQ(descriptor.binding.remotePlayerRole,
             wire::ClientPlayerRootRole::RemotePlayerTwo);
    CHECK_EQ(descriptor.binding.runtimeReceipt.playerSlot,
             static_cast<uint8_t>(
                 wire::ClientPlayerRootRole::RemotePlayerTwo));

    wire::ClientPlayerRootActivation activation{};
    model::RouteDescriptor targetRoute{};
    CHECK_TRUE(!authority.issueClientPlayerRootActivation(
        root.clientRootNodeId, target.nodeId, kNowMs + 4u, activation,
        targetRoute));

    wire::ClientPlayerRootAssertion assertion{};
    assertion.authorityGeneration = root.authorityGeneration;
    assertion.hostRootNodeId = root.hostRootNodeId;
    assertion.grantNonce = root.grant.grantNonce;
    assertion.binding = root.grant.binding;
    std::vector<uint8_t> assertionBytes;
    CHECK_TRUE(encodeClientPlayerRootAssertion(assertion, assertionBytes));
    std::vector<uint8_t> assertionEnvelope;
    CHECK_TRUE(authority.encodeClientPlayerRootControl(
        root, 1u, assertionBytes.data(), assertionBytes.size(),
        assertionEnvelope));
    topology::TopologyControlEnvelope decodedEnvelope{};
    const RouteEndpoint wrongRootQsr{0x0100007Fu, 48922u};
    CHECK_EQ(authority.decodeClientPlayerRootControl(
                 wrongRootQsr, assertionEnvelope.data(),
                 assertionEnvelope.size(), kNowMs + 4u, decodedEnvelope),
             topology::TopologyControlResult::EndpointMismatch);
    CHECK_EQ(authority.decodeControl(
                 RouteEndpoint{0x0100007Fu, 48921u},
                 assertionEnvelope.data(), assertionEnvelope.size(),
                 kNowMs + 4u, decodedEnvelope),
             topology::TopologyControlResult::UnknownNode);
    CHECK_EQ(authority.decodeClientPlayerRootControl(
                 RouteEndpoint{0x0100007Fu, 48921u},
                 assertionEnvelope.data(), assertionEnvelope.size(),
                 kNowMs + 4u, decodedEnvelope),
             topology::TopologyControlResult::Accepted);
    CHECK_TRUE(authority.clientPlayerRootForNode(root.clientRootNodeId,
                                                 kNowMs + 4u, descriptor));
    CHECK_TRUE(descriptor.asserted);

    CHECK_TRUE(authority.issueClientPlayerRootActivation(
        root.clientRootNodeId, target.nodeId, kNowMs + 5u, activation,
        targetRoute));
    CHECK_EQ(targetRoute.port, targetQsr.port);
    CHECK_EQ(activation.binding.clientRootNodeId, root.clientRootNodeId);
    CHECK_EQ(activation.targetViewerNodeId, target.nodeId);
    CHECK_EQ(activation.targetLease.parentId, root.clientRootNodeId);
    CHECK_EQ(activation.targetLease.parentDepth, 0u);
    CHECK_EQ(activation.targetLease.ancestryCount, 1u);
    CHECK_EQ(activation.targetLease.ancestry[0], root.clientRootNodeId);
    CHECK_EQ(activation.targetLease.childBinding.childViewerId,
             target.viewerId);
    CHECK_EQ(activation.targetLease.childBinding.childQspRoute.port,
             target.qspEndpoint.port);
    std::vector<uint8_t> activationInput;
    CHECK_TRUE(wire::encodeClientPlayerRootActivationAttestationInput(
        activation, activationInput));
    CHECK_TRUE(verifier.verify(activationInput.data(), activationInput.size(),
                               activation.hostSignature));

    wire::ClientPlayerRootActivation retry{};
    model::RouteDescriptor retryRoute{};
    CHECK_TRUE(authority.issueClientPlayerRootActivation(
        root.clientRootNodeId, target.nodeId, kNowMs + 6u, retry, retryRoute));
    CHECK_EQ(retry.activationNonce, activation.activationNonce);
    CHECK_EQ(retry.targetLease.replayNonce, activation.targetLease.replayNonce);
    CHECK_TRUE(retryRoute.kind == targetRoute.kind
               && retryRoute.sameEndpoint(targetRoute)
               && retryRoute.generation == targetRoute.generation
               && retryRoute.routeProved == targetRoute.routeProved);

    wire::ClientPlayerRootActivationReceipt receipt{};
    CHECK_TRUE(wire::makeClientPlayerRootActivationReceipt(activation,
                                                            receipt));
    std::vector<uint8_t> receiptBytes;
    CHECK_TRUE(encodeClientPlayerRootActivationReceipt(receipt, receiptBytes));
    std::vector<uint8_t> receiptEnvelope;
    CHECK_TRUE(authority.encodeControl(target, 2u, receiptBytes.data(),
                                       receiptBytes.size(), receiptEnvelope));
    CHECK_EQ(authority.decodeControl(
                 RouteEndpoint{0x0100007Fu, 46922u}, receiptEnvelope.data(),
                 receiptEnvelope.size(), kNowMs + 7u, decodedEnvelope),
             topology::TopologyControlResult::EndpointMismatch);
    CHECK_EQ(authority.decodeControl(targetQsr, receiptEnvelope.data(),
                                     receiptEnvelope.size(), kNowMs + 7u,
                                     decodedEnvelope),
             topology::TopologyControlResult::Accepted);

    topology::ClientPlayerRootActivationCommitRecipient pendingCommit{};
    CHECK_TRUE(authority.nextClientPlayerRootActivationCommit(pendingCommit));
    CHECK_TRUE(pendingCommit.qsrRoute.kind == targetRoute.kind
               && pendingCommit.qsrRoute.sameEndpoint(targetRoute)
               && pendingCommit.qsrRoute.generation == targetRoute.generation
               && pendingCommit.qsrRoute.routeProved
                   == targetRoute.routeProved);
    CHECK_EQ(pendingCommit.commit.activation.activationNonce,
             activation.activationNonce);
    std::vector<uint8_t> commitInput;
    CHECK_TRUE(wire::encodeClientPlayerRootActivationCommitAttestationInput(
        pendingCommit.commit, commitInput));
    CHECK_TRUE(verifier.verify(commitInput.data(), commitInput.size(),
                               pendingCommit.commit.hostSignature));
    std::vector<uint8_t> commitBytes;
    CHECK_TRUE(encodeClientPlayerRootActivationCommit(pendingCommit.commit,
                                                       commitBytes));
    std::vector<uint8_t> commitEnvelope;
    CHECK_TRUE(authority.encodeClientPlayerRootActivationCommitControl(
        pendingCommit, 3u, commitBytes.data(), commitBytes.size(),
        commitEnvelope));
    wire::ControlDatagram decodedCommit{};
    CHECK_EQ(wire::decodeControlDatagram(
                 commitBytes.data(), commitBytes.size(), decodedCommit),
             wire::DecodeResult::Ok);
    CHECK_EQ(decodedCommit.type,
             wire::ControlMessageType::ClientPlayerRootActivationCommit);
    CHECK_TRUE(authority.acknowledgeClientPlayerRootActivationCommit(
        pendingCommit));
    CHECK_TRUE(!authority.nextClientPlayerRootActivationCommit(pendingCommit));

    topology::TopologyViewerProof rebound = viewerProof(
        targetProof.viewerId, 45922u, targetProof.viewerGeneration + 1u,
        targetProof.routeGeneration + 1u, targetProof.confirmedCursor + 1u);
    rebound.qspStreaming = false;
    CHECK_TRUE(authority.beginViewerRebind(rebound, kNowMs + 8u, error));
    topology::ClientPlayerRootActivationRetirementRecipient pendingRetirement{};
    CHECK_TRUE(authority.nextClientPlayerRootActivationRetirement(
        pendingRetirement));
    CHECK_EQ(pendingRetirement.qsrRoute.port, targetQsr.port);
    CHECK_EQ(pendingRetirement.retirement.activation.activationNonce,
             activation.activationNonce);
    CHECK_EQ(pendingRetirement.retirement.reason,
             wire::ClientPlayerRootRetirementReason::TargetViewerDetached);
    std::vector<uint8_t> retirementInput;
    CHECK_TRUE(wire::encodeClientPlayerRootActivationRetirementAttestationInput(
        pendingRetirement.retirement, retirementInput));
    CHECK_TRUE(verifier.verify(retirementInput.data(), retirementInput.size(),
                               pendingRetirement.retirement.hostSignature));
    std::vector<uint8_t> retirementBytes;
    CHECK_TRUE(encodeClientPlayerRootActivationRetirement(
        pendingRetirement.retirement, retirementBytes));
    std::vector<uint8_t> retirementEnvelope;
    CHECK_TRUE(authority.encodeClientPlayerRootActivationRetirementControl(
        pendingRetirement, 4u, retirementBytes.data(), retirementBytes.size(),
        retirementEnvelope));
    CHECK_TRUE(authority.acknowledgeClientPlayerRootActivationRetirement(
        pendingRetirement));
    CHECK_TRUE(!authority.nextClientPlayerRootActivationRetirement(
        pendingRetirement));
    CHECK_TRUE(!authority.issueClientPlayerRootActivation(
        root.clientRootNodeId, target.nodeId, kNowMs + 9u, activation,
        targetRoute));

    authority.setVerifiedCursor(
        model::VerifiedChainCursor{1u, 1u, 0xCA11000000112233ull});
    descriptor = {};
    CHECK_TRUE(!authority.clientPlayerRootForNode(root.clientRootNodeId,
                                                  kNowMs + 10u, descriptor));
}

void testTopologyControlSequenceExhaustionNeverWraps() {
    uint64_t next = (std::numeric_limits<uint64_t>::max)() - 1u;
    uint64_t reserved = 0u;
    CHECK_TRUE(topology::reserveTopologyControlSequence(next, reserved));
    CHECK_EQ(reserved, (std::numeric_limits<uint64_t>::max)() - 1u);
    CHECK_EQ(next, (std::numeric_limits<uint64_t>::max)());

    CHECK_TRUE(topology::reserveTopologyControlSequence(next, reserved));
    CHECK_EQ(reserved, (std::numeric_limits<uint64_t>::max)());
    CHECK_EQ(next, 0u);

    reserved = 0xDEADBEEFu;
    CHECK_TRUE(!topology::reserveTopologyControlSequence(next, reserved));
    CHECK_EQ(next, 0u);
    CHECK_EQ(reserved, 0u);
}

}

int main() {
    testAuthorityExportsOnlyExactBoundChildLeaseFacts();
    testAuthorityRejectsLeaseBindingWithoutVerifiedCacheGenesis();
    testAuthorityAllocatesBoundUniqueNodeIds();
    testAuthorityRejectsIncompleteOrCollidingRootFacts();
    testAuthorityRejectsUnprovenDuplicateAndStaleBindings();
    testAuthorityRejectsCrossLaneEndpointAliases();
    testAuthorityRebindPreservesNodeAndWaitsForQspFinalize();
    testAuthorityRestartDropsPriorSessionNodesAndControls();
    testAuthorityRequiresBoundAuthenticatedMonotonicControls();
    testAuthorityStagesFirstQsrAdmissionUntilInnerValidation();
    testAuthorityMakesRootLeasesOneTimeAndQsrFailureFailSoft();
    testViewerLifecycleResultsPreserveRootAttachmentOwnership();
    testAuthorityStateBindsOnlyCurrentAttachedRecipient();
    testRelayDataBindingRequiresExactAttachedHostObservedRoutes();
    testRelayReadyRequiresHostBindingAndExplicitAssignment();
    testStagedRelayChildActivationDefersGenericRootAdmission();
    testPairedRelayChildActivationRequiresBothExactQtaRecipients();
    testFinalCommitEstablishesAuthorityAdjacencyAndGatesGrandchild();
    testClientDataRootRequiresExactAuthenticatedImmutablePrefix();
    testClientPlayerRootRequiresSeparateClaimAssertionAndExactTarget();
    testTopologyControlSequenceExhaustionNeverWraps();
    if (g_failures != 0) {
        std::fprintf(stderr,
                     "resilient topology authority tests FAILED: %d\n",
                     g_failures);
        return 1;
    }
    std::puts("resilient topology authority tests passed");
    return 0;
}
