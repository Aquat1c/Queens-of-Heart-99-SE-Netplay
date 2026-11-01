#include "../ResilientTopologyClient.h"

#include "../../../spectator/ResilientSpectatorProtocol.h"
#include "../../../spectator/SpectatorGrant.h"
#include "../../../spectator/SpectatorProtocol.h"
#include "../../../spectator/tests/ResilientSpectatorTestIdentity.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

namespace topology = qclient::spectator::topology::client;
namespace model = qnetplay::spectator::resilient;
namespace wire = qnetplay::spectator::resilient::wire;
namespace qsp = qnetplay::spectator;

constexpr uint64_t kViewerId = 0x1122334455667788ull;
constexpr uint32_t kViewerGeneration = 7u;
constexpr uint64_t kSessionId = 0x8877665544332211ull;
constexpr uint64_t kCorrelation = 0x13579BDF2468ACE0ull;
constexpr uint64_t kAuthorityGeneration = 0xAABBCCDDEEFF0011ull;
constexpr uint64_t kNodeId = 0x1020304050607080ull;
constexpr uint64_t kExpiresAtMs = 90000u;
constexpr uint64_t kQspRouteGeneration = 31u;
constexpr uint64_t kBindingGeneration = 4u;
constexpr uint64_t kGrantMacLow = 0xCAFEBABE0BADF00Dull;
constexpr uint64_t kGrantMacHigh = 0x0123456789ABCDEFull;
constexpr size_t kQtaEnvelopePrefixBytes = 52u;
constexpr size_t kQtaAuthorityGenerationOffset = 12u;
constexpr size_t kQtaAuthenticationTagOffset = 36u;

int g_failures = 0;

#define CHECK_TRUE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::fprintf(stderr, "%s:%d CHECK failed: %s\n",             \
                         __FILE__, __LINE__, #expr);                        \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

#define CHECK_EQ(left, right)                                                \
    do {                                                                     \
        const auto checkLeft = (left);                                       \
        const auto checkRight = (right);                                     \
        if (!(checkLeft == checkRight)) {                                    \
            std::fprintf(stderr, "%s:%d CHECK_EQ failed: %s != %s\n",   \
                         __FILE__, __LINE__, #left, #right);                \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

void appendU64(std::vector<uint8_t>& out, const uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

void appendU32(std::vector<uint8_t>& out, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

class TestQtaTranscript final : public topology::QtaMacInputBuilder {
public:
    bool build(const uint64_t authorityGeneration, const uint64_t nodeId,
               const uint64_t sequence, const uint8_t* payload,
               const size_t payloadBytes,
               std::vector<uint8_t>& out) const override {
        out.clear();
        if (payload == nullptr || payloadBytes == 0u) return false;
        try {
            static constexpr char domain[] = "test-topology-qta-transcript";
            out.insert(out.end(), domain, domain + sizeof(domain) - 1u);
            appendU64(out, authorityGeneration);
            appendU64(out, nodeId);
            appendU64(out, sequence);
            out.insert(out.end(), payload, payload + payloadBytes);
        } catch (...) {
            out.clear();
            return false;
        }
        return true;
    }
};

topology::ClientBootstrap bootstrap(
    std::array<uint8_t, topology::kQtaMacBytes>& proofKey) {
    topology::ClientBootstrap value{};
    value.enabled = true;
    value.viewerId = kViewerId;
    value.viewerGeneration = kViewerGeneration;
    value.sessionId = kSessionId;
    value.correlation = kCorrelation;
    value.enrollmentProofKey = proofKey.data();
    value.enrollmentProofKeyBytes = proofKey.size();
    value.retry.initialRetryMs = 10u;
    value.retry.maxRetryMs = 40u;
    value.retry.enrollmentTimeoutMs = 100u;
    value.retry.qsrTimeoutMs = 100u;
    value.retry.maxEnrollmentAttempts = 3u;
    value.retry.maxQsrAttempts = 3u;
    value.qta.magic = 0x31415451u;
    value.qta.version = 1u;
    value.qta.type = 1u;
    value.qta.maxPayloadBytes = 1024u;
    return value;
}

std::vector<uint8_t> responseBody() {
    return {0x52u, 0x45u, 0x53u, 0x50u, 0x02u, 0x00u, 0x01u, 0x00u,
            0x11u, 0x22u, 0x33u, 0x44u};
}

topology::EnrollmentResponse response() {
    topology::EnrollmentResponse value{};
    value.granted = true;
    value.correlation = kCorrelation;
    value.viewerId = kViewerId;
    value.viewerGeneration = kViewerGeneration;
    value.sessionId = kSessionId;
    value.authorityGeneration = kAuthorityGeneration;
    value.nodeId = kNodeId;
    value.expiresAtMs = kExpiresAtMs;
    value.qspRouteGeneration = kQspRouteGeneration;
    value.bindingGeneration = kBindingGeneration;
    for (size_t index = 0u; index < value.qtaCredential.size(); ++index)
        value.qtaCredential[index] = static_cast<uint8_t>(0x40u + index);
    return value;
}

bool signResponse(topology::EnrollmentResponse& value) {
    std::vector<uint8_t> canonical;
    const std::vector<uint8_t> body = responseBody();
    if (!topology::makeTopologyControlV2CanonicalInput(
            topology::TopologyControlV2Direction::Response,
            body.data(), body.size(), canonical)) {
        return false;
    }
    return topology::makeTopologyControlV2Tag(
        kGrantMacLow, kGrantMacHigh, canonical.data(), canonical.size(),
        value.authenticationTag);
}

bool acceptResponse(topology::ResilientTopologyClient& client,
                    const uint64_t nowMs) {
    topology::EnrollmentResponse value = response();
    if (!signResponse(value)) return false;
    std::vector<uint8_t> canonical;
    const std::vector<uint8_t> body = responseBody();
    if (!topology::makeTopologyControlV2CanonicalInput(
            topology::TopologyControlV2Direction::Response,
            body.data(), body.size(), canonical)) {
        return false;
    }
    return client.acceptEnrollmentResponse(value, canonical.data(),
                                           canonical.size(), nowMs);
}

bool advanceToAwaitingResponse(topology::ResilientTopologyClient& client,
                               const uint64_t nowMs,
                               topology::ClientAction* actionOut = nullptr) {
    std::array<uint8_t, topology::kQtaMacBytes> proofKey{};
    if (!topology::deriveTopologyControlV2ProofKey(
            kGrantMacLow, kGrantMacHigh, proofKey)) {
        return false;
    }
    if (!client.start(bootstrap(proofKey), nowMs)
        || !client.onQspInitAckTransmitted(nowMs)) {
        return false;
    }
    topology::ClientAction action{};
    if (!client.poll(nowMs, action)
        || action.kind != topology::ClientActionKind::SendEnrollmentRequest) {
        return false;
    }
    std::vector<uint8_t> requestCanonical;
    const std::vector<uint8_t> requestBody = {
        0x52u, 0x45u, 0x51u, 0x02u, 0x00u, 0x01u,
    };
    if (!topology::makeTopologyControlV2CanonicalInput(
            topology::TopologyControlV2Direction::Request,
            requestBody.data(), requestBody.size(), requestCanonical)) {
        return false;
    }
    std::array<uint8_t, topology::kQtaMacBytes> requestTag{};
    if (!client.makeEnrollmentRequestTag(action.actionId,
                                         requestCanonical.data(),
                                         requestCanonical.size(), requestTag)
        || !topology::verifyTopologyControlV2Tag(
            kGrantMacLow, kGrantMacHigh, requestCanonical.data(),
            requestCanonical.size(), requestTag)
        || !client.completeAction(action.actionId, true, nowMs)) {
        return false;
    }
    if (actionOut != nullptr) *actionOut = action;
    return true;
}

bool advanceToOfferWait(topology::ResilientTopologyClient& client,
                        const uint64_t nowMs,
                        std::vector<uint8_t>* initialQta = nullptr) {
    if (!advanceToAwaitingResponse(client, nowMs)
        || !acceptResponse(client, nowMs + 1u)) {
        return false;
    }
    const std::array<uint8_t, 4u> bootstrapPayload = {
        0xAAu, 0xBBu, 0xCCu, 0xDDu,
    };
    if (!client.setBootstrapQsrPayload(bootstrapPayload.data(),
                                       bootstrapPayload.size(), nowMs + 2u)) {
        return false;
    }
    topology::ClientAction action{};
    TestQtaTranscript transcript;
    std::vector<uint8_t> qta;
    const bool sent = client.poll(nowMs + 2u, action)
        && action.kind == topology::ClientActionKind::SendBootstrapQsr
        && client.makeQtaEnvelope(action, transcript, qta)
        && client.completeAction(action.actionId, true, nowMs + 2u);
    if (sent && initialQta != nullptr) *initialQta = qta;
    return sent;
}

model::ParentLease b10ParentLease() {
    model::ParentLease lease{};
    lease.stream = model::test::syntheticStreamIdentityForTests(0xB10u);
    lease.childId = kNodeId;
    lease.parentId = kNodeId + 1u;
    lease.childBinding.authorityGeneration = kAuthorityGeneration;
    lease.childBinding.childViewerId = kViewerId;
    lease.childBinding.childViewerGeneration = kViewerGeneration;
    lease.childBinding.childQspRoute = model::RouteDescriptor{
        model::RouteKind::Direct, 0x0100007Fu, 46003u,
        kQspRouteGeneration, true};
    lease.childBinding.childEndpointBindingGeneration = kBindingGeneration;
    lease.childBinding.childRequestedCursor = {
        1u, 0u, 0x1122334455667788ull};
    lease.parentDepth = 0u;
    lease.parentSourceGeneration = 1u;
    lease.routeGeneration = kQspRouteGeneration;
    lease.routes[0] = model::RouteDescriptor{
        model::RouteKind::Direct, 0x0100007Fu, 46001u,
        kQspRouteGeneration, true};
    lease.routeCount = 1u;
    lease.cacheRange = model::VerifiedCacheRange{1u, 1u};
    lease.latestVerifiedCursor = 1u;
    lease.childCapacity = 1u;
    lease.expiryMs = kExpiresAtMs;
    lease.replayNonce = 0x99887766u;
    lease.ancestry[0] = lease.parentId;
    lease.ancestryCount = 1u;
    lease.macLow = 0xD0C0B0A090807060ull;
    lease.macHigh = 0x0102030405060708ull;
    return lease;
}

wire::ParentCommit b10ParentCommit(const model::ParentLease& lease) {
    wire::ParentCommit commit{};
    commit.stream = lease.stream;
    commit.childId = lease.childId;
    commit.parentId = lease.parentId;
    commit.lease = model::LeaseBinding{
        lease.childId, lease.parentId, lease.parentSourceGeneration,
        lease.routeGeneration, lease.expiryMs, lease.replayNonce,
        lease.macLow, lease.macHigh, lease.childBinding};
    commit.sourceGeneration = lease.parentSourceGeneration;
    commit.routeGeneration = lease.routeGeneration;
    commit.exactNextCursor = 1u;
    commit.exactNextEvent = 0u;
    commit.overlapCursor = 1u;
    commit.overlapEvent = 0u;
    commit.overlapDigest = 0x1122334455667788ull;
    return commit;
}

bool encodeB10Lease(std::vector<uint8_t>& bytes) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::ParentLease;
    datagram.parentLease = b10ParentLease();
    return wire::encodeControlDatagram(datagram, bytes);
}

bool encodeB10Commit(std::vector<uint8_t>& bytes) {
    const model::ParentLease lease = b10ParentLease();
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::ParentCommit;
    datagram.parentCommit = b10ParentCommit(lease);
    return wire::encodeControlDatagram(datagram, bytes);
}

bool encodeB10AuthorityLiveness(std::vector<uint8_t>& bytes) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::AuthorityState;
    datagram.authorityState.stream = b10ParentLease().stream;
    datagram.authorityState.authorityGeneration = kAuthorityGeneration;
    datagram.authorityState.rootNodeId = kNodeId + 1u;
    datagram.authorityState.childNodeId = kNodeId;
    datagram.authorityState.viewerId = kViewerId;
    datagram.authorityState.viewerGeneration = kViewerGeneration;
    datagram.authorityState.qspRouteGeneration = kQspRouteGeneration;
    datagram.authorityState.qsrIpv4BE = 0x0100007Fu;
    datagram.authorityState.qsrPort = 46002u;
    datagram.authorityState.qsrBindingGeneration = kBindingGeneration;
    datagram.authorityState.kind = wire::AuthorityStateKind::Liveness;
    datagram.authorityState.reason = wire::AuthorityStateReason::None;
    datagram.authorityState.sequence = 1u;
    datagram.authorityState.authorityExpiryMs = kExpiresAtMs;
    datagram.authorityState.eventTimeMs = 100u;
    datagram.authorityState.livenessWindowMs = 50u;
    return wire::encodeControlDatagram(datagram, bytes);
}

bool makeBootstrapQta(topology::ResilientTopologyClient& sender,
                      const uint8_t* const payload,
                      const size_t payloadBytes,
                      const uint64_t nowMs,
                      std::vector<uint8_t>& qta) {
    qta.clear();
    if (!advanceToAwaitingResponse(sender, nowMs)
        || !acceptResponse(sender, nowMs + 1u)
        || !sender.setBootstrapQsrPayload(payload, payloadBytes, nowMs + 2u)) {
        return false;
    }
    topology::ClientAction action{};
    TestQtaTranscript transcript;
    return sender.poll(nowMs + 2u, action)
        && action.kind == topology::ClientActionKind::SendBootstrapQsr
        && sender.makeQtaEnvelope(action, transcript, qta)
        && sender.completeAction(action.actionId, true, nowMs + 2u);
}

bool advanceToAwaitingReceipt(topology::ResilientTopologyClient& client,
                              const uint64_t nowMs) {
    if (!advanceToOfferWait(client, nowMs)
        || !client.acceptRootOffer(0x99887766u, nowMs + 20u)) {
        return false;
    }
    const std::array<uint8_t, 3u> redemption = {0x31u, 0x32u, 0x33u};
    if (!client.setRedemptionQsrPayload(redemption.data(), redemption.size(),
                                        0x99887766u, nowMs + 20u)) {
        return false;
    }
    topology::ClientAction action{};
    TestQtaTranscript transcript;
    std::vector<uint8_t> outbound;
    return client.poll(nowMs + 20u, action)
        && action.kind == topology::ClientActionKind::SendRedemptionQsr
        && client.makeQtaEnvelope(action, transcript, outbound)
        && client.completeAction(action.actionId, true, nowMs + 20u);
}

bool makeB10Hello(std::vector<uint8_t>& out) {
    out.clear();
    const model::StreamCompatibilityIdentity stream =
        model::test::syntheticStreamIdentityForTests(0xB10u);
    qsp::HelloPayload hello{};
    hello.simulationContract = 0x5153502D42313031ull;
    hello.compatibilityIdentity = stream.compatibility;
    hello.requestedGameId = qsp::kLatestGameId;
    hello.viewerNonce = 0xB10B10B10B10B10ull;
    hello.viewerId = 0xB10B10B10B10B11ull;
    hello.generation = 1u;
    const qsp::SessionIdentity identity{
        kSessionId, 1u, qsp::SessionCookie{kGrantMacLow, kGrantMacHigh}};
    std::vector<uint8_t> payload;
    return qsp::encodePayload(hello, payload)
        && qsp::encodeDatagram(qsp::MessageType::Hello, identity, 1u,
                               payload, out);
}

void testCanonicalControlAuthentication() {
    std::array<uint8_t, topology::kQtaMacBytes> key{};
    CHECK_TRUE(topology::deriveTopologyControlV2ProofKey(
        kGrantMacLow, kGrantMacHigh, key));
    CHECK_TRUE((key != std::array<uint8_t, topology::kQtaMacBytes>{}));

    const std::array<uint8_t, 4u> body = {1u, 2u, 3u, 4u};
    std::vector<uint8_t> request;
    std::vector<uint8_t> responseInput;
    CHECK_TRUE(topology::makeTopologyControlV2CanonicalInput(
        topology::TopologyControlV2Direction::Request, body.data(),
        body.size(), request));
    CHECK_TRUE(topology::makeTopologyControlV2CanonicalInput(
        topology::TopologyControlV2Direction::Response, body.data(),
        body.size(), responseInput));
    CHECK_TRUE(request != responseInput);
    std::array<uint8_t, topology::kQtaMacBytes> tag{};
    CHECK_TRUE(topology::makeTopologyControlV2Tag(
        kGrantMacLow, kGrantMacHigh, request.data(), request.size(), tag));
    CHECK_TRUE(topology::verifyTopologyControlV2Tag(
        kGrantMacLow, kGrantMacHigh, request.data(), request.size(), tag));
    CHECK_TRUE(!topology::verifyTopologyControlV2Tag(
        kGrantMacLow, kGrantMacHigh, responseInput.data(),
        responseInput.size(), tag));
}

void testInitAckAndEnrollmentGate() {
    topology::ResilientTopologyClient client{};
    std::array<uint8_t, topology::kQtaMacBytes> proofKey{};
    CHECK_TRUE(topology::deriveTopologyControlV2ProofKey(
        kGrantMacLow, kGrantMacHigh, proofKey));
    CHECK_TRUE(client.start(bootstrap(proofKey), 100u));
    topology::ClientAction action{};
    CHECK_TRUE(!client.poll(100u, action));
    CHECK_EQ(client.state(), topology::ClientState::WaitingForInitAck);
    CHECK_TRUE(client.onQspInitAckTransmitted(100u));
    CHECK_TRUE(client.poll(100u, action));
    CHECK_EQ(action.kind, topology::ClientActionKind::SendEnrollmentRequest);
    CHECK_TRUE(client.completeAction(action.actionId, true, 100u));
    CHECK_EQ(client.state(), topology::ClientState::AwaitingEnrollment);
    CHECK_TRUE(acceptResponse(client, 101u));
    CHECK_EQ(client.state(), topology::ClientState::AwaitingBootstrapPayload);
}

void testEnrollmentCorrelationAndSecretInvalidation() {
    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToAwaitingResponse(client, 100u));
    topology::EnrollmentResponse rejected = response();
    rejected.correlation ^= 1u;
    CHECK_TRUE(signResponse(rejected));
    const std::vector<uint8_t> body = responseBody();
    std::vector<uint8_t> canonical;
    CHECK_TRUE(topology::makeTopologyControlV2CanonicalInput(
        topology::TopologyControlV2Direction::Response, body.data(),
        body.size(), canonical));
    CHECK_TRUE(!client.acceptEnrollmentResponse(rejected, canonical.data(),
                                                canonical.size(), 101u));
    CHECK_EQ(client.failure(), topology::ClientFailure::EnrollmentValidationRejected);
    topology::EnrollmentBinding binding{};
    CHECK_TRUE(!client.enrollmentBinding(binding));
    client.stop();
    CHECK_EQ(client.state(), topology::ClientState::Disabled);
    CHECK_TRUE(!client.enrollmentBinding(binding));
}

void testEnrollmentCorrelationRotationRejectsOldResponse() {
    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToAwaitingResponse(client, 100u));

    topology::EnrollmentResponse stale = response();
    CHECK_TRUE(signResponse(stale));
    const std::vector<uint8_t> staleBody = responseBody();
    std::vector<uint8_t> staleCanonical;
    CHECK_TRUE(topology::makeTopologyControlV2CanonicalInput(
        topology::TopologyControlV2Direction::Response, staleBody.data(),
        staleBody.size(), staleCanonical));

    constexpr uint64_t kFreshCorrelation = kCorrelation + 0x100u;
    CHECK_TRUE(client.restartEnrollment(kFreshCorrelation, 120u));
    topology::ClientAction retry{};
    CHECK_TRUE(client.poll(120u, retry));
    CHECK_EQ(retry.kind, topology::ClientActionKind::SendEnrollmentRequest);
    CHECK_EQ(retry.correlation, kFreshCorrelation);
    CHECK_TRUE(client.completeAction(retry.actionId, true, 120u));

    CHECK_TRUE(!client.acceptEnrollmentResponse(stale, staleCanonical.data(),
                                                staleCanonical.size(), 121u));
    CHECK_EQ(client.failure(), topology::ClientFailure::EnrollmentValidationRejected);
}

void testQtaAuthenticationAndReplay() {
    topology::ResilientTopologyClient sender{};
    topology::ResilientTopologyClient receiver{};
    CHECK_TRUE(advanceToOfferWait(sender, 100u));
    CHECK_TRUE(advanceToOfferWait(receiver, 100u));

    topology::ClientAction action{};
    CHECK_TRUE(sender.poll(112u, action));
    CHECK_EQ(action.kind, topology::ClientActionKind::SendBootstrapQsr);
    TestQtaTranscript transcript;
    std::vector<uint8_t> qta;
    CHECK_TRUE(sender.makeQtaEnvelope(action, transcript, qta));
    CHECK_TRUE(sender.completeAction(action.actionId, true, 112u));

    topology::QtaEnvelope decoded{};
    CHECK_EQ(receiver.acceptQtaEnvelope(qta.data(), qta.size(), transcript,
                                        decoded),
             topology::QtaIngressResult::Accepted);
    CHECK_EQ(decoded.sequence, action.sequence);
    CHECK_TRUE(decoded.qsrPayload == action.qsrPayload);
    CHECK_EQ(receiver.acceptQtaEnvelope(qta.data(), qta.size(), transcript,
                                        decoded),
             topology::QtaIngressResult::ReplayRejected);

    topology::ClientAction second{};
    CHECK_TRUE(sender.poll(132u, second));
    std::vector<uint8_t> badQta;
    CHECK_TRUE(sender.makeQtaEnvelope(second, transcript, badQta));
    badQta[36u] ^= 0x80u;
    CHECK_EQ(receiver.acceptQtaEnvelope(badQta.data(), badQta.size(),
                                        transcript, decoded),
             topology::QtaIngressResult::AuthenticationRejected);
}

void testReceiptIsTheAttachmentBoundary() {
    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToOfferWait(client, 100u));
    CHECK_TRUE(client.acceptRootOffer(0x99887766u, 120u));
    const std::array<uint8_t, 3u> redemption = {0x31u, 0x32u, 0x33u};
    CHECK_TRUE(client.setRedemptionQsrPayload(redemption.data(),
                                              redemption.size(),
                                              0x99887766u, 120u));
    topology::ClientAction action{};
    TestQtaTranscript transcript;
    std::vector<uint8_t> qta;
    CHECK_TRUE(client.poll(120u, action));
    CHECK_EQ(action.kind, topology::ClientActionKind::SendRedemptionQsr);
    CHECK_TRUE(client.makeQtaEnvelope(action, transcript, qta));
    CHECK_TRUE(client.completeAction(action.actionId, true, 120u));
    CHECK_TRUE(!client.attached());
    CHECK_TRUE(!client.acceptReceipt(0x99887767u, 121u));
    CHECK_EQ(client.failure(), topology::ClientFailure::ReceiptRejected);

    topology::ResilientTopologyClient accepted{};
    CHECK_TRUE(advanceToOfferWait(accepted, 100u));
    CHECK_TRUE(accepted.acceptRootOffer(0x99887766u, 120u));
    CHECK_TRUE(accepted.setRedemptionQsrPayload(redemption.data(),
                                                redemption.size(),
                                                0x99887766u, 120u));
    CHECK_TRUE(accepted.poll(120u, action));
    CHECK_TRUE(accepted.makeQtaEnvelope(action, transcript, qta));
    CHECK_TRUE(accepted.completeAction(action.actionId, true, 120u));
    CHECK_TRUE(accepted.acceptReceipt(0x99887766u, 121u));
    CHECK_TRUE(accepted.attached());
}

void testActivationReceiptIsNarrowPreAttachmentAction() {
    topology::ResilientTopologyClient client;
    CHECK_TRUE(advanceToOfferWait(client, 100u));
    CHECK_EQ(client.state(), topology::ClientState::AwaitingRootOffer);

    const std::array<uint8_t, 5u> receipt = {
        0x52u, 0x43u, 0x50u, 0x54u, 0x01u,
    };
    CHECK_TRUE(!client.setActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0u, 101u));
    CHECK_TRUE(client.setActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0xA17C0DEu, 101u));
    CHECK_TRUE(!client.setActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0xA17C0DFu, 101u));

    topology::ClientAction action{};
    TestQtaTranscript transcript;
    std::vector<uint8_t> envelope;
    CHECK_TRUE(client.poll(101u, action));
    CHECK_EQ(action.kind,
             topology::ClientActionKind::SendActivationReceiptQsr);
    CHECK_EQ(action.correlation, 0xA17C0DEu);
    CHECK_EQ(client.state(), topology::ClientState::AwaitingRootOffer);
    CHECK_TRUE(client.makeQtaEnvelope(action, transcript, envelope));
    CHECK_TRUE(client.completeAction(action.actionId, true, 101u));
    CHECK_EQ(client.state(), topology::ClientState::AwaitingRootOffer);

    std::vector<uint8_t> generic;
    CHECK_TRUE(!client.makeAttachedQtaEnvelope(
        receipt.data(), receipt.size(), transcript, generic));
    CHECK_TRUE(client.poll(111u, action));
    CHECK_EQ(action.kind,
             topology::ClientActionKind::SendActivationReceiptQsr);
    CHECK_EQ(action.correlation, 0xA17C0DEu);
    CHECK_TRUE(client.makeQtaEnvelope(action, transcript, envelope));
    CHECK_TRUE(client.completeAction(action.actionId, true, 111u));

    topology::ResilientTopologyClient disabled;
    CHECK_TRUE(!disabled.setActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0xA17C0DEu, 1u));

    topology::ResilientTopologyClient cancelled;
    CHECK_TRUE(advanceToOfferWait(cancelled, 200u));
    CHECK_TRUE(cancelled.setActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0xBADC0DEu, 201u));
    CHECK_TRUE(cancelled.poll(201u, action));
    CHECK_TRUE(!cancelled.cancelActivationReceiptQsrPayload(0xBADC0DFu));
    CHECK_TRUE(cancelled.cancelActivationReceiptQsrPayload(0xBADC0DEu));
    CHECK_TRUE(!cancelled.completeAction(action.actionId, true, 201u));
    CHECK_TRUE(!cancelled.poll(211u, action));
}

void testPairedActivationRequiresAcceptedCandidateIngress() {
    const std::array<uint8_t, 5u> receipt = {
        0x52u, 0x43u, 0x50u, 0x54u, 0x02u,
    };
    constexpr uint64_t kActivationNonce = 0xA11CE55u;

    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToOfferWait(client, 100u));
    CHECK_TRUE(client.setActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), kActivationNonce, 101u));
    topology::ClientAction action{};
    TestQtaTranscript transcript{};
    std::vector<uint8_t> envelope;
    CHECK_TRUE(client.poll(101u, action));
    CHECK_EQ(action.kind,
             topology::ClientActionKind::SendActivationReceiptQsr);
    CHECK_TRUE(client.makeQtaEnvelope(action, transcript, envelope));
    CHECK_TRUE(client.completeAction(action.actionId, true, 101u));

    CHECK_TRUE(!client.acceptPairedActivationCommit(kActivationNonce + 1u,
                                                    102u));
    CHECK_TRUE(client.acceptPairedActivationCommit(kActivationNonce, 102u));
    CHECK_EQ(client.state(), topology::ClientState::AwaitingActivationIngress);
    CHECK_TRUE(!client.attached());
    CHECK_TRUE(!client.acceptPairedActivationCompletion(kActivationNonce + 1u,
                                                        103u));
    CHECK_TRUE(!client.acceptPairedActivationCommit(kActivationNonce, 103u));
    CHECK_TRUE(!client.poll(1000u, action));
    CHECK_EQ(client.state(), topology::ClientState::AwaitingActivationIngress);
    CHECK_TRUE(!client.attached());
    CHECK_TRUE(client.acceptPairedActivationCompletion(kActivationNonce,
                                                       1001u));
    CHECK_TRUE(client.attached());

    topology::ResilientTopologyClient retired{};
    CHECK_TRUE(advanceToOfferWait(retired, 200u));
    CHECK_TRUE(retired.setActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), kActivationNonce, 201u));
    CHECK_TRUE(retired.poll(201u, action));
    CHECK_TRUE(retired.makeQtaEnvelope(action, transcript, envelope));
    CHECK_TRUE(retired.completeAction(action.actionId, true, 201u));
    CHECK_TRUE(retired.acceptPairedActivationCommit(kActivationNonce, 202u));
    CHECK_TRUE(!retired.cancelActivationReceiptQsrPayload(
        kActivationNonce + 1u));
    CHECK_TRUE(retired.cancelActivationReceiptQsrPayload(kActivationNonce));
    CHECK_EQ(retired.state(), topology::ClientState::AwaitingRootOffer);
    CHECK_TRUE(!retired.attached());
    CHECK_TRUE(!retired.acceptPairedActivationCompletion(kActivationNonce,
                                                         203u));
}

void testAttachedRelayActivationReceiptIsNarrowAndNonDestructive() {
    const std::array<uint8_t, 5u> receipt = {
        0x52u, 0x4Cu, 0x59u, 0x52u, 0x01u,
    };
    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToOfferWait(client, 100u));
    CHECK_TRUE(!client.setAttachedRelayActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0xA771C0DEu, 101u));
    CHECK_TRUE(client.acceptRootOffer(0x99887766u, 120u));
    const std::array<uint8_t, 3u> redemption = {0x31u, 0x32u, 0x33u};
    CHECK_TRUE(client.setRedemptionQsrPayload(redemption.data(),
                                              redemption.size(),
                                              0x99887766u, 120u));
    topology::ClientAction action{};
    TestQtaTranscript transcript;
    std::vector<uint8_t> envelope;
    CHECK_TRUE(client.poll(120u, action));
    CHECK_TRUE(client.makeQtaEnvelope(action, transcript, envelope));
    CHECK_TRUE(client.completeAction(action.actionId, true, 120u));
    CHECK_TRUE(client.acceptReceipt(0x99887766u, 121u));
    CHECK_TRUE(client.attached());

    CHECK_TRUE(!client.setActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0xA771C0DEu, 122u));
    CHECK_TRUE(!client.setAttachedRelayActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0u, 122u));
    CHECK_TRUE(client.setAttachedRelayActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0xA771C0DEu, 122u));
    CHECK_TRUE(client.poll(122u, action));
    CHECK_EQ(action.kind,
             topology::ClientActionKind::SendAttachedRelayActivationReceiptQsr);
    CHECK_EQ(action.correlation, 0xA771C0DEu);
    CHECK_TRUE(client.makeQtaEnvelope(action, transcript, envelope));
    CHECK_TRUE(client.completeAction(action.actionId, true, 122u));
    CHECK_TRUE(client.attached());

    topology::ResilientTopologyClient cancelled{};
    CHECK_TRUE(advanceToOfferWait(cancelled, 200u));
    CHECK_TRUE(cancelled.acceptRootOffer(0x99887766u, 220u));
    CHECK_TRUE(cancelled.setRedemptionQsrPayload(redemption.data(),
                                                 redemption.size(),
                                                 0x99887766u, 220u));
    CHECK_TRUE(cancelled.poll(220u, action));
    CHECK_TRUE(cancelled.completeAction(action.actionId, true, 220u));
    CHECK_TRUE(cancelled.acceptReceipt(0x99887766u, 221u));
    CHECK_TRUE(cancelled.setAttachedRelayActivationReceiptQsrPayload(
        receipt.data(), receipt.size(), 0xA771C0DEu, 222u));
    CHECK_TRUE(cancelled.poll(222u, action));
    CHECK_TRUE(!cancelled.cancelAttachedRelayActivationReceiptQsrPayload(
        0xA771C0DFu));
    CHECK_TRUE(cancelled.cancelAttachedRelayActivationReceiptQsrPayload(
        0xA771C0DEu));
    CHECK_TRUE(!cancelled.completeAction(action.actionId, true, 222u));
    CHECK_TRUE(cancelled.attached());
    CHECK_TRUE(!cancelled.poll(232u, action));
}

void testAttachedClientAdmitsAuthenticatedAuthorityStateEnvelope() {
    topology::ResilientTopologyClient attached{};
    topology::ResilientTopologyClient sender{};
    CHECK_TRUE(advanceToOfferWait(attached, 100u));
    CHECK_TRUE(attached.acceptRootOffer(0x99887766u, 120u));
    const std::array<uint8_t, 3u> redemption = {0x31u, 0x32u, 0x33u};
    CHECK_TRUE(attached.setRedemptionQsrPayload(redemption.data(),
                                                redemption.size(),
                                                0x99887766u, 120u));
    topology::ClientAction redemptionAction{};
    TestQtaTranscript transcript;
    std::vector<uint8_t> redemptionQta;
    CHECK_TRUE(attached.poll(120u, redemptionAction));
    CHECK_TRUE(attached.makeQtaEnvelope(redemptionAction, transcript,
                                        redemptionQta));
    CHECK_TRUE(attached.completeAction(redemptionAction.actionId, true, 120u));
    CHECK_TRUE(attached.acceptReceipt(0x99887766u, 121u));
    CHECK_TRUE(attached.attached());

    CHECK_TRUE(advanceToOfferWait(sender, 100u));
    topology::ClientAction authorityAction{};
    CHECK_TRUE(sender.poll(112u, authorityAction));
    std::vector<uint8_t> authorityQta;
    CHECK_TRUE(sender.makeQtaEnvelope(authorityAction, transcript,
                                      authorityQta));
    topology::QtaEnvelope decoded{};
    CHECK_EQ(attached.acceptQtaEnvelope(authorityQta.data(), authorityQta.size(),
                                        transcript, decoded),
             topology::QtaIngressResult::Accepted);
    CHECK_TRUE(!decoded.qsrPayload.empty());
    CHECK_EQ(attached.acceptQtaEnvelope(authorityQta.data(), authorityQta.size(),
                                        transcript, decoded),
             topology::QtaIngressResult::ReplayRejected);

    CHECK_TRUE(sender.completeAction(authorityAction.actionId, true, 112u));
    topology::ClientAction laterAuthorityAction{};
    std::vector<uint8_t> unauthenticated;
    CHECK_TRUE(sender.poll(132u, laterAuthorityAction));
    CHECK_TRUE(sender.makeQtaEnvelope(laterAuthorityAction, transcript,
                                      unauthenticated));
    unauthenticated[36u] ^= 0x80u;
    CHECK_EQ(attached.acceptQtaEnvelope(unauthenticated.data(),
                                        unauthenticated.size(), transcript,
                                        decoded),
             topology::QtaIngressResult::AuthenticationRejected);
    CHECK_TRUE(attached.attached());
}

void testB10_31_QspBytesCannotEnterTheQsrCredentialLane() {
    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToOfferWait(client, 100u));
    std::vector<uint8_t> qspHello;
    CHECK_TRUE(makeB10Hello(qspHello));
    topology::QtaEnvelope decoded{};
    TestQtaTranscript transcript;
    CHECK_EQ(client.acceptQtaEnvelope(qspHello.data(), qspHello.size(),
                                      transcript, decoded),
             topology::QtaIngressResult::Malformed);
    CHECK_EQ(client.state(), topology::ClientState::AwaitingRootOffer);
}

void testB10_32_QsrBytesCannotEnterTheQspLane() {
    std::vector<uint8_t> qsr;
    CHECK_TRUE(encodeB10Lease(qsr));
    wire::ControlDatagram decodedQsr{};
    CHECK_EQ(wire::decodeControlDatagram(qsr.data(), qsr.size(), decodedQsr),
             wire::DecodeResult::Ok);
    CHECK_EQ(decodedQsr.type, wire::ControlMessageType::ParentLease);

    qsp::Datagram decodedQsp{};
    const qsp::SessionIdentity identity{
        kSessionId, 1u, qsp::SessionCookie{kGrantMacLow, kGrantMacHigh}};
    CHECK_EQ(qsp::decodeDatagram(qsr.data(), qsr.size(), identity, decodedQsp),
             qsp::DecodeError::BadMagic);
}

void testB10_05_AuthorityGenerationModifiedInParentLeaseEnvelope() {
    std::vector<uint8_t> leaseBytes;
    CHECK_TRUE(encodeB10Lease(leaseBytes));
    wire::ControlDatagram leaseDatagram{};
    CHECK_EQ(wire::decodeControlDatagram(leaseBytes.data(), leaseBytes.size(),
                                         leaseDatagram), wire::DecodeResult::Ok);
    CHECK_EQ(leaseDatagram.type, wire::ControlMessageType::ParentLease);

    topology::ResilientTopologyClient sender{};
    std::vector<uint8_t> qta;
    CHECK_TRUE(makeBootstrapQta(sender, leaseBytes.data(), leaseBytes.size(),
                                 100u, qta));
    CHECK_TRUE(qta.size() > kQtaAuthorityGenerationOffset + sizeof(uint64_t));
    if (qta.size() <= kQtaAuthorityGenerationOffset + sizeof(uint64_t)) return;

    topology::ResilientTopologyClient probe{};
    CHECK_TRUE(advanceToOfferWait(probe, 100u));
    TestQtaTranscript transcript;
    topology::QtaEnvelope decoded{};
    CHECK_EQ(probe.acceptQtaEnvelope(qta.data(), qta.size(), transcript,
                                     decoded), topology::QtaIngressResult::Accepted);

    qta[kQtaAuthorityGenerationOffset] ^= 0x01u;
    topology::ResilientTopologyClient receiver{};
    CHECK_TRUE(advanceToOfferWait(receiver, 100u));
    CHECK_EQ(receiver.acceptQtaEnvelope(qta.data(), qta.size(), transcript,
                                        decoded), topology::QtaIngressResult::IdentityRejected);
    CHECK_EQ(receiver.state(), topology::ClientState::AwaitingRootOffer);
}

void testB10_06_AuthorityGenerationModifiedInParentCommitEnvelope() {
    std::vector<uint8_t> commitBytes;
    CHECK_TRUE(encodeB10Commit(commitBytes));
    wire::ControlDatagram commitDatagram{};
    CHECK_EQ(wire::decodeControlDatagram(commitBytes.data(), commitBytes.size(),
                                         commitDatagram), wire::DecodeResult::Ok);
    CHECK_EQ(commitDatagram.type, wire::ControlMessageType::ParentCommit);

    topology::ResilientTopologyClient sender{};
    std::vector<uint8_t> qta;
    CHECK_TRUE(makeBootstrapQta(sender, commitBytes.data(), commitBytes.size(),
                                 100u, qta));
    CHECK_TRUE(qta.size() > kQtaAuthorityGenerationOffset + sizeof(uint64_t));
    if (qta.size() <= kQtaAuthorityGenerationOffset + sizeof(uint64_t)) return;

    TestQtaTranscript transcript;
    topology::ResilientTopologyClient probe{};
    CHECK_TRUE(advanceToAwaitingReceipt(probe, 100u));
    topology::QtaEnvelope decoded{};
    CHECK_EQ(probe.acceptQtaEnvelope(qta.data(), qta.size(), transcript,
                                     decoded), topology::QtaIngressResult::Accepted);

    qta[kQtaAuthorityGenerationOffset] ^= 0x01u;
    topology::ResilientTopologyClient receiver{};
    CHECK_TRUE(advanceToAwaitingReceipt(receiver, 100u));
    CHECK_EQ(receiver.acceptQtaEnvelope(qta.data(), qta.size(), transcript,
                                        decoded), topology::QtaIngressResult::IdentityRejected);
    CHECK_EQ(receiver.state(), topology::ClientState::AwaitingReceipt);
}

void testB10_33_ViewerGrantCannotSubstituteForQtaCredential() {
    topology::ResilientTopologyClient sender{};
    topology::ResilientTopologyClient receiver{};
    std::vector<uint8_t> qta;
    CHECK_TRUE(advanceToOfferWait(sender, 100u, &qta));
    CHECK_TRUE(advanceToOfferWait(receiver, 100u));
    CHECK_TRUE(qta.size() > kQtaEnvelopePrefixBytes);
    if (qta.size() <= kQtaEnvelopePrefixBytes) return;

    TestQtaTranscript transcript;
    std::vector<uint8_t> grantMacInput;
    CHECK_TRUE(transcript.build(kAuthorityGeneration, kNodeId, 1u,
                                qta.data() + kQtaEnvelopePrefixBytes,
                                qta.size() - kQtaEnvelopePrefixBytes,
                                grantMacInput));
    std::array<uint8_t, topology::kQtaMacBytes> grantTag{};
    CHECK_TRUE(topology::makeTopologyControlV2Tag(
        kGrantMacLow, kGrantMacHigh, grantMacInput.data(), grantMacInput.size(),
        grantTag));
    std::copy_n(grantTag.data(), grantTag.size(),
                qta.data() + kQtaAuthenticationTagOffset);

    topology::QtaEnvelope decoded{};
    CHECK_EQ(receiver.acceptQtaEnvelope(qta.data(), qta.size(), transcript,
                                        decoded),
             topology::QtaIngressResult::AuthenticationRejected);
    CHECK_EQ(receiver.state(), topology::ClientState::AwaitingRootOffer);
}

void testB10_36_ExactQsrRequestTimeout() {
    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToOfferWait(client, 100u));
    topology::ClientAction action{};
    CHECK_TRUE(!client.poll(201u, action));
    CHECK_EQ(client.state(), topology::ClientState::Failed);
    CHECK_EQ(client.failure(), topology::ClientFailure::QsrTimedOut);
}

void testB10_43_QsrRetryBudgetExhaustsWithoutFourthSend() {
    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToOfferWait(client, 100u));
    TestQtaTranscript transcript;
    topology::ClientAction action{};
    std::vector<uint8_t> qta;

    CHECK_TRUE(client.poll(112u, action));
    CHECK_EQ(action.kind, topology::ClientActionKind::SendBootstrapQsr);
    CHECK_EQ(action.sequence, 2u);
    CHECK_TRUE(client.makeQtaEnvelope(action, transcript, qta));
    CHECK_TRUE(client.completeAction(action.actionId, true, 112u));

    CHECK_TRUE(client.poll(132u, action));
    CHECK_EQ(action.kind, topology::ClientActionKind::SendBootstrapQsr);
    CHECK_EQ(action.sequence, 3u);
    CHECK_TRUE(client.makeQtaEnvelope(action, transcript, qta));
    CHECK_TRUE(client.completeAction(action.actionId, true, 132u));

    CHECK_TRUE(!client.poll(133u, action));
    CHECK_EQ(client.state(), topology::ClientState::AwaitingRootOffer);
    CHECK_TRUE(!client.poll(201u, action));
    CHECK_EQ(client.state(), topology::ClientState::Failed);
    CHECK_EQ(client.failure(), topology::ClientFailure::QsrTimedOut);
}

void testB10_44_StopWhileQsrRetryIsPending() {
    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToOfferWait(client, 100u));
    client.stop();
    topology::ClientAction action{};
    topology::EnrollmentBinding binding{};
    CHECK_EQ(client.state(), topology::ClientState::Disabled);
    CHECK_TRUE(!client.poll(112u, action));
    CHECK_TRUE(!client.enrollmentBinding(binding));
}

void testPendingReceiptAllowsOnlyAuthenticatedCommitAfterOvertake() {
    topology::ResilientTopologyClient sender{};
    topology::ResilientTopologyClient receiver{};
    std::vector<uint8_t> commitBytes;
    CHECK_TRUE(encodeB10Commit(commitBytes));
    std::vector<uint8_t> firstQta;
    CHECK_TRUE(makeBootstrapQta(sender, commitBytes.data(), commitBytes.size(),
                                100u, firstQta));
    CHECK_TRUE(advanceToOfferWait(receiver, 100u));
    CHECK_TRUE(receiver.acceptRootOffer(0x99887766u, 120u));
    const std::array<uint8_t, 3u> redemption = {0x31u, 0x32u, 0x33u};
    CHECK_TRUE(receiver.setRedemptionQsrPayload(redemption.data(),
                                                redemption.size(),
                                                0x99887766u, 120u));
    topology::ClientAction redemptionAction{};
    TestQtaTranscript transcript;
    std::vector<uint8_t> redemptionQta;
    CHECK_TRUE(receiver.poll(120u, redemptionAction));
    CHECK_TRUE(receiver.makeQtaEnvelope(redemptionAction, transcript,
                                        redemptionQta));
    CHECK_TRUE(receiver.completeAction(redemptionAction.actionId, true, 120u));
    CHECK_EQ(receiver.state(), topology::ClientState::AwaitingReceipt);

    topology::ClientAction overtakingAction{};
    std::vector<uint8_t> overtakingQta;
    CHECK_TRUE(sender.poll(112u, overtakingAction));
    CHECK_EQ(overtakingAction.sequence, 2u);
    CHECK_TRUE(sender.makeQtaEnvelope(overtakingAction, transcript,
                                      overtakingQta));
    CHECK_TRUE(sender.completeAction(overtakingAction.actionId, true, 112u));

    topology::QtaEnvelope decoded{};
    CHECK_EQ(receiver.acceptQtaEnvelope(overtakingQta.data(),
                                        overtakingQta.size(), transcript,
                                        decoded), topology::QtaIngressResult::Accepted);
    CHECK_EQ(receiver.acceptQtaEnvelope(firstQta.data(), firstQta.size(),
                                        transcript, decoded),
             topology::QtaIngressResult::Accepted);
    CHECK_TRUE(receiver.acceptReceipt(0x99887766u, 121u));
    CHECK_TRUE(receiver.attached());
}

void testPendingReceiptRejectsStaleNonCommitQta() {
    const auto checkPayload = [](const std::vector<uint8_t>& payload) {
        topology::ResilientTopologyClient sender{};
        topology::ResilientTopologyClient receiver{};
        std::vector<uint8_t> firstQta;
        CHECK_TRUE(makeBootstrapQta(sender, payload.data(), payload.size(),
                                    100u, firstQta));
        CHECK_TRUE(advanceToAwaitingReceipt(receiver, 100u));

        topology::ClientAction higherAction{};
        TestQtaTranscript transcript;
        std::vector<uint8_t> higherQta;
        CHECK_TRUE(sender.poll(112u, higherAction));
        CHECK_EQ(higherAction.sequence, 2u);
        CHECK_TRUE(sender.makeQtaEnvelope(higherAction, transcript, higherQta));
        CHECK_TRUE(sender.completeAction(higherAction.actionId, true, 112u));

        topology::QtaEnvelope decoded{};
        CHECK_EQ(receiver.acceptQtaEnvelope(higherQta.data(), higherQta.size(),
                                            transcript, decoded),
                 topology::QtaIngressResult::Accepted);
        CHECK_EQ(receiver.acceptQtaEnvelope(firstQta.data(), firstQta.size(),
                                            transcript, decoded),
                 topology::QtaIngressResult::ReplayRejected);
        CHECK_EQ(receiver.state(), topology::ClientState::AwaitingReceipt);
    };

    std::vector<uint8_t> leaseBytes;
    CHECK_TRUE(encodeB10Lease(leaseBytes));
    if (!leaseBytes.empty()) checkPayload(leaseBytes);

    std::vector<uint8_t> stateBytes;
    CHECK_TRUE(encodeB10AuthorityLiveness(stateBytes));
    if (!stateBytes.empty()) checkPayload(stateBytes);
}

void testBoundedEnrollmentTimeout() {
    topology::ResilientTopologyClient client{};
    CHECK_TRUE(advanceToAwaitingResponse(client, 10u));
    topology::ClientAction action{};
    CHECK_TRUE(!client.poll(110u, action));
    CHECK_EQ(client.state(), topology::ClientState::Failed);
    CHECK_EQ(client.failure(), topology::ClientFailure::EnrollmentTimedOut);
}

}

bool runB10Case(const uint32_t b10Case) {
    switch (b10Case) {
    case 5u:
        testB10_05_AuthorityGenerationModifiedInParentLeaseEnvelope();
        return true;
    case 6u:
        testB10_06_AuthorityGenerationModifiedInParentCommitEnvelope();
        return true;
    case 31u:
        testB10_31_QspBytesCannotEnterTheQsrCredentialLane();
        return true;
    case 32u:
        testB10_32_QsrBytesCannotEnterTheQspLane();
        return true;
    case 33u:
        testB10_33_ViewerGrantCannotSubstituteForQtaCredential();
        return true;
    case 36u:
        testB10_36_ExactQsrRequestTimeout();
        return true;
    case 43u:
        testB10_43_QsrRetryBudgetExhaustsWithoutFourthSend();
        return true;
    case 44u:
        testB10_44_StopWhileQsrRetryIsPending();
        return true;
    default:
        return false;
    }
}

int finishTests() {
    if (g_failures != 0) {
        std::fprintf(stderr, "resilient topology client tests: %d failure(s)\n",
                     g_failures);
        return 1;
    }
    std::puts("resilient topology client tests passed");
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--b10-case") == 0) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || parsed > 45u
            || !runB10Case(static_cast<uint32_t>(parsed))) {
            std::fprintf(stderr, "unsupported core B10 case: %s\n", argv[2]);
            return 2;
        }
        return finishTests();
    }
    if (argc != 1) {
        std::fprintf(stderr, "usage: %s [--b10-case <5|6|31|32|33|36|43|44>]\n",
                     argc > 0 ? argv[0] : "qoh99_resilient_topology_client_tests");
        return 2;
    }
    testCanonicalControlAuthentication();
    testInitAckAndEnrollmentGate();
    testEnrollmentCorrelationAndSecretInvalidation();
    testEnrollmentCorrelationRotationRejectsOldResponse();
    testQtaAuthenticationAndReplay();
    testReceiptIsTheAttachmentBoundary();
    testActivationReceiptIsNarrowPreAttachmentAction();
    testPairedActivationRequiresAcceptedCandidateIngress();
    testAttachedRelayActivationReceiptIsNarrowAndNonDestructive();
    testAttachedClientAdmitsAuthenticatedAuthorityStateEnvelope();
    testB10_05_AuthorityGenerationModifiedInParentLeaseEnvelope();
    testB10_06_AuthorityGenerationModifiedInParentCommitEnvelope();
    testB10_31_QspBytesCannotEnterTheQsrCredentialLane();
    testB10_32_QsrBytesCannotEnterTheQspLane();
    testB10_33_ViewerGrantCannotSubstituteForQtaCredential();
    testB10_36_ExactQsrRequestTimeout();
    testB10_43_QsrRetryBudgetExhaustsWithoutFourthSend();
    testB10_44_StopWhileQsrRetryIsPending();
    testPendingReceiptAllowsOnlyAuthenticatedCommitAfterOvertake();
    testPendingReceiptRejectsStaleNonCommitQta();
    testBoundedEnrollmentTimeout();
    return finishTests();
}
