#include "../ResilientSpectatorRuntime.h"

#include "../../../netplay/WireProtocol.h"
#include "../../../netplay/tests/TestHarness.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace {

namespace runtime = qclient::spectator::resilient;
namespace model = qnetplay::spectator::resilient;
namespace wire = qnetplay::spectator::resilient::wire;

using qclient::spectator::RouteEndpoint;

constexpr uint64_t kSetId = 0x1122334455667788ull;
constexpr uint64_t kEpoch = 0x1020304050607080ull;
constexpr uint64_t kStreamRoot = 0x8877665544332211ull;
constexpr uint64_t kGameId = 0x6162636465666768ull;
constexpr uint64_t kInitialDigest = 0xCAFEBABE01234567ull;
constexpr uint64_t kNowMs = 500u;
constexpr model::NodeId kRootId = 1001u;
constexpr model::NodeId kChildId = 2002u;

qnetplay::compat::CompatibilityIdentityWire makeCompatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    (void)qnetplay::compat::setProfileId(identity, "qoh99-se1126");
    for (size_t index = 0u;
         index < qnetplay::compat::kCompatibilitySha256Bytes; ++index) {
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
    identity.stateSchemaId = 0x51u;
    identity.modSimulationId = 0x52u;
    identity.timingProfileId = 0x53u;
    return identity;
}

model::StreamCompatibilityIdentity makeStream() {
    model::StreamCompatibilityIdentity stream{};
    (void)model::makeStreamCompatibilityIdentity(
        makeCompatibilityIdentity(), kSetId, kEpoch, kStreamRoot, stream);
    return stream;
}

bool exactIdentityEqual(
    const qnetplay::compat::CompatibilityIdentityWire& left,
    const qnetplay::compat::CompatibilityIdentityWire& right) {
    return left.schemaVersion == right.schemaVersion
        && left.protocolVersion == right.protocolVersion
        && std::memcmp(left.runtimeBuild.profileId, right.runtimeBuild.profileId,
                       qnetplay::compat::kCompatibilityProfileIdBytes) == 0
        && std::memcmp(left.runtimeBuild.executableSha256,
                       right.runtimeBuild.executableSha256,
                       qnetplay::compat::kCompatibilitySha256Bytes) == 0
        && left.runtimeBuild.executableSize == right.runtimeBuild.executableSize
        && left.runtimeBuild.peTimestamp == right.runtimeBuild.peTimestamp
        && left.simulationContent.manifestSchemaVersion
            == right.simulationContent.manifestSchemaVersion
        && left.simulationContent.manifestFlags
            == right.simulationContent.manifestFlags
        && std::memcmp(left.simulationContent.simulationRootSha256,
                       right.simulationContent.simulationRootSha256,
                       qnetplay::compat::kCompatibilitySha256Bytes) == 0
        && std::memcmp(left.simulationContent.validationRootSha256,
                       right.simulationContent.validationRootSha256,
                       qnetplay::compat::kCompatibilitySha256Bytes) == 0
        && left.simulationContent.entryCount
            == right.simulationContent.entryCount
        && left.simulationContent.requiredEntryCount
            == right.simulationContent.requiredEntryCount
        && left.stateSchemaId == right.stateSchemaId
        && left.modSimulationId == right.modSimulationId
        && left.timingProfileId == right.timingProfileId
        && left.reserved == right.reserved;
}

model::NodeAdvertisement makeAdvertisement(
    const model::NodeId nodeId, const model::NodeKind kind,
    const model::StreamCompatibilityIdentity& stream,
    const model::RouteKind routeKind, const uint32_t ipv4BE,
    const uint16_t port, const uint64_t routeGeneration = 7u) {
    model::NodeAdvertisement advertisement{};
    advertisement.nodeId = nodeId;
    advertisement.kind = kind;
    advertisement.stream = stream;
    advertisement.route.kind = routeKind;
    advertisement.route.ipv4BE = ipv4BE;
    advertisement.route.port = port;
    advertisement.route.generation = routeGeneration;
    advertisement.route.routeProved = true;
    advertisement.capabilityModelRevision =
        model::kResilientCapabilityModelRevision;
    advertisement.capabilities = model::kCapabilityEphemeralEndpoint
        | model::kCapabilityMultiPeerAutopunch
        | model::kCapabilityMultiViewerRoot;
    advertisement.childCapacity = kind == model::NodeKind::SpectatorRelay
        ? 8u : 32u;
    advertisement.childCount = 0u;
    advertisement.uploadCapacity = 64u;
    advertisement.uploadInUse = 0u;
    advertisement.rttMs = 12u;
    advertisement.lossPerMille = 0u;
    advertisement.cacheFirstCursor = 0u;
    advertisement.cacheNextCursor = 128u;
    advertisement.sourceGeneration = 5u;
    advertisement.routeGeneration = routeGeneration;
    advertisement.lastProgressMs = 400u;
    advertisement.leaseExpiryMs = 0u;
    advertisement.reachable = true;
    advertisement.draining = false;
    return advertisement;
}

struct FakeControlTransport final : runtime::ResilientControlTransport {
    struct Sent {
        model::RouteDescriptor to{};
        std::vector<uint8_t> bytes;
    };

    bool acceptSends = true;
    std::vector<Sent> sent;

    bool sendControl(const model::RouteDescriptor& to,
                     const uint8_t* bytes, size_t length) override {
        if (!acceptSends || bytes == nullptr || length == 0u) return false;
        sent.push_back(Sent{to, std::vector<uint8_t>(bytes, bytes + length)});
        return true;
    }
};

struct FakeAuthorizer final : runtime::ResilientControlAuthorizer {
    bool allow = true;
    mutable size_t calls = 0u;
    model::NodeId boundChildId = model::kInvalidNodeId;
    model::ParentLeaseChildBinding childBinding{};

    bool authorize(const RouteEndpoint& from,
                   const wire::ControlDatagram& datagram,
                   uint64_t nowMs) const override {
        ++calls;
        return allow && from.valid() && nowMs != 0u
            && datagram.type != wire::ControlMessageType::ParentCommit;
    }

    bool parentLeaseChildBinding(
        const model::NodeId childId,
        model::ParentLeaseChildBinding& out) const override {
        out = {};
        if (!allow || childId == model::kInvalidNodeId
            || childId != boundChildId || !childBinding.valid()) {
            return false;
        }
        out = childBinding;
        return true;
    }
};

qnetplay::spectator::SpectatorGrantKey makeLeaseKey() {
    qnetplay::spectator::SpectatorGrantKey key{};
    for (size_t index = 0u; index < key.size(); ++index)
        key[index] = static_cast<uint8_t>(0xA0u + index);
    return key;
}

runtime::ResilientRootRuntimeConfig makeRuntimeConfig(
    const std::shared_ptr<FakeAuthorizer>& authorizer) {
    runtime::ResilientRootRuntimeConfig config{};
    config.stream = makeStream();
    config.localAdvertisement = makeAdvertisement(
        kRootId, model::NodeKind::HostRoot, config.stream,
        model::RouteKind::Direct, 0x0100007Fu, 42001u);
    config.gameId = kGameId;
    config.initialDigest = kInitialDigest;
    config.parentLeaseKey = makeLeaseKey();
    config.authorizer = authorizer;
    return config;
}

bool encodeAdvertisement(const model::NodeAdvertisement& advertisement,
                         std::vector<uint8_t>& out) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::NodeAdvertisement;
    datagram.nodeAdvertisement = advertisement;
    return wire::encodeControlDatagram(datagram, out);
}

RouteEndpoint endpointFor(const model::NodeAdvertisement& advertisement) {
    return RouteEndpoint{advertisement.route.ipv4BE, advertisement.route.port};
}

RouteEndpoint controlEndpointFor(
    const model::NodeAdvertisement& advertisement) {
    const model::RouteDescriptor& control =
        advertisement.resolvedControlRoute();
    return RouteEndpoint{control.ipv4BE, control.port};
}

model::ParentLeaseChildBinding makeChildBinding(
    const model::NodeAdvertisement& child, const uint32_t nextRecord,
    const uint32_t nextEvent, const uint64_t digest) {
    model::ParentLeaseChildBinding binding{};
    binding.authorityGeneration = 0xA701u;
    binding.childViewerId = 0xB702u;
    binding.childViewerGeneration = 3u;
    binding.childQspRoute = child.route;
    binding.childEndpointBindingGeneration = 0xC703u;
    binding.childRequestedCursor = {nextRecord, nextEvent, digest};
    return binding;
}

void bindChild(FakeAuthorizer& authorizer,
               const model::NodeAdvertisement& child,
               const uint32_t nextRecord, const uint32_t nextEvent,
               const uint64_t digest) {
    authorizer.boundChildId = child.nodeId;
    authorizer.childBinding = makeChildBinding(
        child, nextRecord, nextEvent, digest);
}

qnetplay::spectator::ConfirmedRecord makeVerifiedRecord(
    const uint32_t record, const uint64_t previousDigest) {
    qnetplay::spectator::ConfirmedRecord result{};
    result.record = record;
    result.timelineFrame = record;
    result.playerOneInput = static_cast<uint16_t>(record & 0x0Fu);
    result.playerTwoInput = static_cast<uint16_t>((record + 1u) & 0x0Fu);
    result.stateHash = 0xABCD000000000000ull | record;
    result.rollingDigest = qnetplay::spectator::advanceRollingDigest(
        previousDigest, kGameId, result);
    return result;
}

model::LeaseBinding bindingFor(const model::ParentLease& lease) {
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

void writeLe32(uint8_t* out, const uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8u);
    out[2] = static_cast<uint8_t>(value >> 16u);
    out[3] = static_cast<uint8_t>(value >> 24u);
}

}

QNP_TEST(resilient_qsr_round_trips_the_complete_immutable_identity) {
    const model::StreamCompatibilityIdentity stream = makeStream();
    QNP_REQUIRE(stream.valid());
    const model::NodeAdvertisement advertisement = makeAdvertisement(
        kChildId, model::NodeKind::SpectatorRelay, stream,
        model::RouteKind::Punched, 0x0200007Fu, 42002u);

    wire::ControlDatagram input{};
    input.type = wire::ControlMessageType::NodeAdvertisement;
    input.nodeAdvertisement = advertisement;
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(wire::encodeControlDatagram(input, encoded));
    QNP_CHECK(encoded.size() <= wire::kMaxResilientControlDatagramBytes);
    QNP_CHECK(wire::hasResilientControlMagic(encoded.data(), encoded.size()));

    wire::ControlDatagram decoded{};
    QNP_REQUIRE(wire::decodeControlDatagram(encoded.data(), encoded.size(),
                                            decoded)
                == wire::DecodeResult::Ok);
    QNP_CHECK(decoded.type == wire::ControlMessageType::NodeAdvertisement);
    QNP_CHECK(decoded.nodeAdvertisement.nodeId == advertisement.nodeId);
    QNP_CHECK(decoded.nodeAdvertisement.stream.setOrSessionId
              == stream.setOrSessionId);
    QNP_CHECK(decoded.nodeAdvertisement.stream.gameEpoch == stream.gameEpoch);
    QNP_CHECK(decoded.nodeAdvertisement.stream.streamRootIdentity
              == stream.streamRootIdentity);
    QNP_CHECK(exactIdentityEqual(decoded.nodeAdvertisement.stream.compatibility,
                                 stream.compatibility));
    QNP_CHECK(decoded.nodeAdvertisement.stream.compatibility
                  .runtimeBuild.peTimestamp
              == 0x3A19A8A9u);
    QNP_CHECK(!decoded.nodeAdvertisement.controlRoute.valid());
    QNP_CHECK(decoded.nodeAdvertisement.resolvedControlRoute().sameEndpoint(
        decoded.nodeAdvertisement.route));
}

QNP_TEST(resilient_qsr_current_binds_topology_and_exact_parent_lease) {
    const model::StreamCompatibilityIdentity stream = makeStream();
    model::NodeAdvertisement advertisement = makeAdvertisement(
        kChildId, model::NodeKind::SpectatorRelay, stream,
        model::RouteKind::Punched, 0x0200007Fu, 42002u);
    advertisement.activeParent = kRootId;
    advertisement.depth = 1u;
    advertisement.ancestry[0] = kRootId;
    advertisement.ancestryCount = 1u;
    advertisement.controlRoute = advertisement.route;
    advertisement.controlRoute.port = 43002u;

    wire::ControlDatagram nodeDatagram{};
    nodeDatagram.type = wire::ControlMessageType::NodeAdvertisement;
    nodeDatagram.nodeAdvertisement = advertisement;
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(wire::encodeControlDatagram(nodeDatagram, encoded));
    QNP_REQUIRE(encoded.size() > 5u);
    QNP_CHECK(encoded[4] == wire::kResilientControlVersion);
    QNP_CHECK(encoded[5] == 0u);

    wire::ControlDatagram decoded{};
    QNP_REQUIRE(wire::decodeControlDatagram(encoded.data(), encoded.size(),
                                            decoded)
                == wire::DecodeResult::Ok);
    QNP_CHECK(decoded.nodeAdvertisement.activeParent == kRootId);
    QNP_CHECK(decoded.nodeAdvertisement.depth == 1u);
    QNP_CHECK(decoded.nodeAdvertisement.ancestryCount == 1u);
    QNP_CHECK(decoded.nodeAdvertisement.ancestry[0] == kRootId);
    QNP_CHECK(decoded.nodeAdvertisement.route.port == 42002u);
    QNP_CHECK(decoded.nodeAdvertisement.controlRoute.port == 43002u);

    std::vector<uint8_t> prior = encoded;
    prior[4] = static_cast<uint8_t>(wire::kResilientControlVersion - 1u);
    QNP_CHECK(wire::decodeControlDatagram(prior.data(), prior.size(), decoded)
              == wire::DecodeResult::UnsupportedVersion);

    advertisement.ancestry[0] = 99u;
    nodeDatagram.nodeAdvertisement = advertisement;
    QNP_CHECK(!wire::encodeControlDatagram(nodeDatagram, encoded));

    model::LeaseBinding lease{};
    lease.childId = kChildId;
    lease.parentId = kRootId;
    lease.parentSourceGeneration = 5u;
    lease.routeGeneration = 7u;
    lease.expiryMs = 9000u;
    lease.replayNonce = 0xC0FFEEu;
    lease.macLow = 0x12345678u;
    lease.macHigh = 0xABCDEF01u;
    lease.childBinding = makeChildBinding(advertisement, 128u, 9u,
                                          0xA1B2C3D4u);

    model::ContinuityRequest request{};
    request.stream = stream;
    request.lease = lease;
    request.gameId = kGameId;
    request.expectedSourceGeneration = lease.parentSourceGeneration;
    request.parentGeneration = 7u;
    request.exactNextCursor = 128u;
    request.exactNextEvent = 9u;
    request.overlapCursor = 96u;
    request.overlapEvent = 7u;
    request.overlapDigest = 0xA1B2C3D4u;
    wire::ControlDatagram requestDatagram{};
    requestDatagram.type = wire::ControlMessageType::ContinuityRequest;
    requestDatagram.continuityRequest = request;
    QNP_REQUIRE(wire::encodeControlDatagram(requestDatagram, encoded));
    QNP_REQUIRE(wire::decodeControlDatagram(encoded.data(), encoded.size(),
                                            decoded)
                == wire::DecodeResult::Ok);
    QNP_CHECK(decoded.continuityRequest.lease.replayNonce == lease.replayNonce);
    QNP_CHECK(decoded.continuityRequest.lease.parentId == kRootId);

    request.lease.routeGeneration++;
    requestDatagram.continuityRequest = request;
    QNP_CHECK(wire::encodeControlDatagram(requestDatagram, encoded));
    request.lease.parentSourceGeneration++;
    requestDatagram.continuityRequest = request;
    QNP_CHECK(!wire::encodeControlDatagram(requestDatagram, encoded));

    model::ContinuityProof proof{};
    proof.stream = stream;
    proof.lease = lease;
    proof.gameId = kGameId;
    proof.sourceGeneration = lease.parentSourceGeneration;
    proof.parentGeneration = request.parentGeneration;
    proof.cacheFirstCursor = 64u;
    proof.cacheNextCursor = 128u;
    proof.cacheFirstEvent = 3u;
    proof.cacheNextEvent = 9u;
    proof.overlapCursor = 96u;
    proof.overlapEvent = 7u;
    proof.overlapDigest = 0xA1B2C3D4u;
    proof.available = true;
    wire::ControlDatagram proofDatagram{};
    proofDatagram.type = wire::ControlMessageType::ContinuityProof;
    proofDatagram.continuityProof = proof;
    QNP_REQUIRE(wire::encodeControlDatagram(proofDatagram, encoded));
    QNP_REQUIRE(wire::decodeControlDatagram(encoded.data(), encoded.size(),
                                            decoded)
                == wire::DecodeResult::Ok);
    QNP_CHECK(decoded.continuityProof.lease.replayNonce == lease.replayNonce);

    wire::ParentCommit commit{};
    commit.stream = stream;
    commit.childId = kChildId;
    commit.parentId = kRootId;
    commit.lease = lease;
    commit.sourceGeneration = lease.parentSourceGeneration;
    commit.routeGeneration = lease.routeGeneration;
    commit.exactNextCursor = 128u;
    commit.exactNextEvent = 9u;
    commit.overlapCursor = 96u;
    commit.overlapEvent = 7u;
    commit.overlapDigest = 0xA1B2C3D4u;
    wire::ControlDatagram commitDatagram{};
    commitDatagram.type = wire::ControlMessageType::ParentCommit;
    commitDatagram.parentCommit = commit;
    QNP_REQUIRE(wire::encodeControlDatagram(commitDatagram, encoded));
    QNP_REQUIRE(wire::decodeControlDatagram(encoded.data(), encoded.size(),
                                            decoded)
                == wire::DecodeResult::Ok);
    QNP_CHECK(decoded.parentCommit.lease.replayNonce == lease.replayNonce);

    commit.lease.parentId = 77u;
    commitDatagram.parentCommit = commit;
    QNP_CHECK(!wire::encodeControlDatagram(commitDatagram, encoded));
}

QNP_TEST(resilient_qsr_rejects_corruption_and_identity_mutation) {
    const model::StreamCompatibilityIdentity stream = makeStream();
    const model::NodeAdvertisement advertisement = makeAdvertisement(
        kChildId, model::NodeKind::SpectatorRelay, stream,
        model::RouteKind::Direct, 0x0200007Fu, 42002u);
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeAdvertisement(advertisement, encoded));
    QNP_REQUIRE(!encoded.empty());
    encoded.back() ^= 0x01u;
    wire::ControlDatagram decoded{};
    QNP_CHECK(wire::decodeControlDatagram(encoded.data(), encoded.size(),
                                          decoded)
              == wire::DecodeResult::ChecksumMismatch);

    auto authorizer = std::make_shared<FakeAuthorizer>();
    FakeControlTransport transport{};
    runtime::ResilientSpectatorRootRuntime root{};
    QNP_REQUIRE(root.start(makeRuntimeConfig(authorizer), transport));

    model::NodeAdvertisement mutated = advertisement;
    mutated.stream.compatibility.simulationContent.validationRootSha256[0]
        ^= 0x5Au;
    std::vector<uint8_t> reencoded;
    QNP_REQUIRE(encodeAdvertisement(mutated, reencoded));
    QNP_CHECK(root.onDatagram(endpointFor(mutated), reencoded.data(),
                              reencoded.size(), kNowMs)
              == runtime::ControlIngressResult::IdentityRejected);
    QNP_CHECK(authorizer->calls == 0u);
    QNP_CHECK(root.topology()->find(kChildId) == nullptr);
}

QNP_TEST(resilient_qsr_rejects_legacy_and_traffic_relay_control) {
    auto authorizer = std::make_shared<FakeAuthorizer>();
    FakeControlTransport transport{};
    runtime::ResilientSpectatorRootRuntime root{};
    QNP_REQUIRE(root.start(makeRuntimeConfig(authorizer), transport));

    std::array<uint8_t, 4> legacy{};
    writeLe32(legacy.data(), qnetplay::spectator::kSpectatorMagic);
    QNP_CHECK(root.onDatagram(RouteEndpoint{0x0200007Fu, 42002u},
                              legacy.data(), legacy.size(), kNowMs)
              == runtime::ControlIngressResult::DecodeRejected);

    const model::NodeAdvertisement trafficRelay = makeAdvertisement(
        kChildId, model::NodeKind::SpectatorRelay, makeStream(),
        model::RouteKind::TrafficRelay, 0x0200007Fu, 42002u);
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeAdvertisement(trafficRelay, encoded));
    QNP_CHECK(root.onDatagram(endpointFor(trafficRelay), encoded.data(),
                              encoded.size(), kNowMs)
              == runtime::ControlIngressResult::EndpointRejected);
    QNP_CHECK(root.topology()->find(kChildId) == nullptr);
}

QNP_TEST(resilient_qsr_rejects_an_unauthenticated_advertisement) {
    auto authorizer = std::make_shared<FakeAuthorizer>();
    authorizer->allow = false;
    FakeControlTransport transport{};
    runtime::ResilientSpectatorRootRuntime root{};
    QNP_REQUIRE(root.start(makeRuntimeConfig(authorizer), transport));

    const model::NodeAdvertisement child = makeAdvertisement(
        kChildId, model::NodeKind::SpectatorRelay, makeStream(),
        model::RouteKind::Direct, 0x0200007Fu, 42002u);
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeAdvertisement(child, encoded));
    QNP_CHECK(root.onDatagram(endpointFor(child), encoded.data(),
                              encoded.size(), kNowMs)
              == runtime::ControlIngressResult::AuthenticationRejected);
    QNP_CHECK(authorizer->calls == 1u);
    QNP_CHECK(root.topology()->find(kChildId) == nullptr);
}

QNP_TEST(resilient_qsr_admits_authenticated_direct_and_punched_lease_flow) {
    const std::array<model::RouteKind, 2> routeKinds = {
        model::RouteKind::Direct, model::RouteKind::Punched};
    for (size_t index = 0u; index < routeKinds.size(); ++index) {
        auto authorizer = std::make_shared<FakeAuthorizer>();
        FakeControlTransport transport{};
        runtime::ResilientSpectatorRootRuntime root{};
        QNP_REQUIRE(root.start(makeRuntimeConfig(authorizer), transport));

        const model::NodeAdvertisement child = makeAdvertisement(
            kChildId, model::NodeKind::SpectatorRelay, makeStream(),
            routeKinds[index], 0x0200007Fu,
            static_cast<uint16_t>(42002u + index));
        std::vector<uint8_t> advertisement;
        QNP_REQUIRE(encodeAdvertisement(child, advertisement));
        QNP_REQUIRE(root.onDatagram(endpointFor(child), advertisement.data(),
                                    advertisement.size(), kNowMs)
                    == runtime::ControlIngressResult::AcceptedAdvertisement);

        bindChild(*authorizer, child, 0u, 0u, kInitialDigest);
        QNP_REQUIRE(root.issueAndSendParentOffer(
            kChildId, 0u, kNowMs + 5000u, 0xBADC0FFEu + index));
        QNP_REQUIRE(transport.sent.size() == 1u);
        QNP_CHECK(transport.sent.front().to.sameEndpoint(
            child.resolvedControlRoute()));
        QNP_CHECK(transport.sent.front().to.kind
                  == child.resolvedControlRoute().kind);

        wire::ControlDatagram lease{};
        QNP_REQUIRE(wire::decodeControlDatagram(
            transport.sent.front().bytes.data(),
            transport.sent.front().bytes.size(), lease)
                    == wire::DecodeResult::Ok);
        QNP_REQUIRE(lease.type == wire::ControlMessageType::ParentLease);
        QNP_CHECK(lease.parentLease.stream == makeStream());
        QNP_CHECK(lease.parentLease.childId == kChildId);
        QNP_CHECK(lease.parentLease.parentId == kRootId);

        QNP_CHECK(root.onDatagram(endpointFor(child),
                                  transport.sent.front().bytes.data(),
                                  transport.sent.front().bytes.size(),
                                  kNowMs + 1u)
                  == runtime::ControlIngressResult::AcceptedLeaseRedemption);
        const model::TopologyNodeView* admitted = root.topology()->find(kChildId);
        QNP_REQUIRE(admitted != nullptr);
        QNP_CHECK(admitted->activeParent == kRootId);
        QNP_CHECK(admitted->depth == 1u);
        QNP_CHECK(authorizer->calls == 2u);
    }
}

QNP_TEST(resilient_root_requires_the_exact_authorized_child_binding) {
    auto authorizer = std::make_shared<FakeAuthorizer>();
    FakeControlTransport transport{};
    runtime::ResilientSpectatorRootRuntime root{};
    QNP_REQUIRE(root.start(makeRuntimeConfig(authorizer), transport));

    const model::NodeAdvertisement child = makeAdvertisement(
        kChildId, model::NodeKind::SpectatorRelay, makeStream(),
        model::RouteKind::Direct, 0x0200007Fu, 42002u);
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeAdvertisement(child, encoded));
    QNP_REQUIRE(root.onDatagram(endpointFor(child), encoded.data(),
                                encoded.size(), kNowMs)
                == runtime::ControlIngressResult::AcceptedAdvertisement);

    bindChild(*authorizer, child, 0u, 0u, kInitialDigest);
    ++authorizer->childBinding.childQspRoute.port;
    QNP_CHECK(!root.issueAndSendParentOffer(
        kChildId, 0u, kNowMs + 5000u, 0xC0FFEEu));
    QNP_CHECK(root.issuedLeaseCount() == 0u);
    QNP_CHECK(transport.sent.empty());

    bindChild(*authorizer, child, 0u, 0u, kInitialDigest);
    QNP_REQUIRE(root.issueAndSendParentOffer(
        kChildId, 0u, kNowMs + 5000u, 0xC0FFEEu));
    QNP_REQUIRE(transport.sent.size() == 1u);

    wire::ControlDatagram offer{};
    QNP_REQUIRE(wire::decodeControlDatagram(
        transport.sent.front().bytes.data(), transport.sent.front().bytes.size(),
        offer) == wire::DecodeResult::Ok);
    QNP_REQUIRE(offer.type == wire::ControlMessageType::ParentLease);

    wire::ControlDatagram wrong = offer;
    ++wrong.parentLease.childBinding.childViewerGeneration;
    QNP_REQUIRE(wire::encodeControlDatagram(wrong, encoded));
    QNP_CHECK(root.onDatagram(endpointFor(child), encoded.data(), encoded.size(),
                              kNowMs + 1u)
              == runtime::ControlIngressResult::LeaseRejected);

    QNP_REQUIRE(root.onDatagram(endpointFor(child),
                                transport.sent.front().bytes.data(),
                                transport.sent.front().bytes.size(),
                                kNowMs + 1u)
                == runtime::ControlIngressResult::AcceptedLeaseRedemption);
}

QNP_TEST(resilient_qsr_keeps_relay_data_and_control_routes_separate) {
    auto authorizer = std::make_shared<FakeAuthorizer>();
    FakeControlTransport transport{};
    runtime::ResilientSpectatorRootRuntime root{};
    runtime::ResilientRootRuntimeConfig config = makeRuntimeConfig(authorizer);
    config.localAdvertisement.controlRoute = config.localAdvertisement.route;
    config.localAdvertisement.controlRoute.port = 43001u;
    QNP_REQUIRE(root.start(config, transport));

    model::NodeAdvertisement child = makeAdvertisement(
        kChildId, model::NodeKind::SpectatorRelay, makeStream(),
        model::RouteKind::Punched, 0x0200007Fu, 42002u);
    child.controlRoute = child.route;
    child.controlRoute.kind = model::RouteKind::Direct;
    child.controlRoute.port = 43002u;
    QNP_REQUIRE(child.route.valid());
    QNP_REQUIRE(child.controlRoute.valid());
    QNP_CHECK(!child.route.sameEndpoint(child.controlRoute));

    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeAdvertisement(child, encoded));
    QNP_CHECK(root.onDatagram(endpointFor(child), encoded.data(), encoded.size(),
                              kNowMs)
              == runtime::ControlIngressResult::EndpointRejected);
    QNP_CHECK(root.topology()->find(kChildId) == nullptr);
    QNP_REQUIRE(root.onDatagram(controlEndpointFor(child), encoded.data(),
                                encoded.size(), kNowMs)
                == runtime::ControlIngressResult::AcceptedAdvertisement);

    const qnetplay::spectator::ConfirmedRecord first =
        makeVerifiedRecord(0u, kInitialDigest);
    const qnetplay::spectator::ConfirmedRecord second =
        makeVerifiedRecord(1u, first.rollingDigest);
    QNP_REQUIRE(root.acceptArchiveRecord(first)
                == model::CacheAppendResult::Accepted);
    QNP_REQUIRE(root.acceptArchiveRecord(second)
                == model::CacheAppendResult::Accepted);

    bindChild(*authorizer, child, 2u, 0u, second.rollingDigest);
    QNP_REQUIRE(root.issueAndSendParentOffer(
        kChildId, 2u, kNowMs + 5000u, 0xBADC0FFEu));
    QNP_REQUIRE(transport.sent.size() == 1u);
    QNP_CHECK(transport.sent.back().to.sameEndpoint(child.controlRoute));
    QNP_CHECK(!transport.sent.back().to.sameEndpoint(child.route));

    wire::ControlDatagram offer{};
    QNP_REQUIRE(wire::decodeControlDatagram(
        transport.sent.back().bytes.data(), transport.sent.back().bytes.size(),
        offer) == wire::DecodeResult::Ok);
    QNP_REQUIRE(offer.type == wire::ControlMessageType::ParentLease);
    QNP_REQUIRE(offer.parentLease.routeCount == 1u);
    QNP_CHECK(offer.parentLease.routes[0].sameEndpoint(
        config.localAdvertisement.route));
    QNP_CHECK(!offer.parentLease.routes[0].sameEndpoint(
        config.localAdvertisement.controlRoute));

    QNP_CHECK(root.onDatagram(endpointFor(child),
                              transport.sent.front().bytes.data(),
                              transport.sent.front().bytes.size(), kNowMs + 1u)
              == runtime::ControlIngressResult::EndpointRejected);
    QNP_REQUIRE(root.onDatagram(controlEndpointFor(child),
                                transport.sent.front().bytes.data(),
                                transport.sent.front().bytes.size(), kNowMs + 1u)
                == runtime::ControlIngressResult::AcceptedLeaseRedemption);

    QNP_REQUIRE(root.sendParentCommit(offer.parentLease));
    QNP_REQUIRE(transport.sent.size() == 2u);
    QNP_CHECK(transport.sent.back().to.sameEndpoint(child.controlRoute));

    model::ContinuityRequest request{};
    request.stream = makeStream();
    request.lease = bindingFor(offer.parentLease);
    request.gameId = kGameId;
    request.expectedSourceGeneration = offer.parentLease.parentSourceGeneration;
    request.parentGeneration = 7u;
    request.exactNextCursor = 2u;
    request.exactNextEvent = 0u;
    request.overlapCursor = 0u;
    request.overlapEvent = 0u;
    request.overlapDigest = kInitialDigest;
    wire::ControlDatagram continuity{};
    continuity.type = wire::ControlMessageType::ContinuityRequest;
    continuity.continuityRequest = request;
    QNP_REQUIRE(wire::encodeControlDatagram(continuity, encoded));
    QNP_CHECK(root.onDatagram(endpointFor(child), encoded.data(), encoded.size(),
                              kNowMs + 2u)
              == runtime::ControlIngressResult::ContinuityRejected);
    QNP_CHECK(transport.sent.size() == 2u);
    QNP_REQUIRE(root.onDatagram(controlEndpointFor(child), encoded.data(),
                                encoded.size(), kNowMs + 2u)
                == runtime::ControlIngressResult::AcceptedContinuityRequest);
    QNP_REQUIRE(transport.sent.size() == 3u);
    QNP_CHECK(transport.sent.back().to.sameEndpoint(child.controlRoute));
    wire::ControlDatagram proof{};
    QNP_REQUIRE(wire::decodeControlDatagram(
        transport.sent.back().bytes.data(), transport.sent.back().bytes.size(),
        proof) == wire::DecodeResult::Ok);
    QNP_CHECK(proof.type == wire::ControlMessageType::ContinuityProof);
}

QNP_TEST(resilient_root_cache_uses_archive_canonical_frontier_for_continuity) {
    auto authorizer = std::make_shared<FakeAuthorizer>();
    FakeControlTransport transport{};
    runtime::ResilientSpectatorRootRuntime root{};
    QNP_REQUIRE(root.start(makeRuntimeConfig(authorizer), transport));
    QNP_REQUIRE(root.cache() != nullptr);
    QNP_CHECK(root.cache()->range().firstCursor == 0u);
    QNP_CHECK(root.cache()->range().nextCursor == 0u);

    const model::NodeAdvertisement child = makeAdvertisement(
        kChildId, model::NodeKind::SpectatorRelay, makeStream(),
        model::RouteKind::Direct, 0x0200007Fu, 42002u);
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(encodeAdvertisement(child, encoded));
    QNP_REQUIRE(root.onDatagram(endpointFor(child), encoded.data(),
                                encoded.size(), kNowMs)
                == runtime::ControlIngressResult::AcceptedAdvertisement);

    qnetplay::spectator::ConfirmedRecord first =
        makeVerifiedRecord(0u, kInitialDigest);
    QNP_REQUIRE(root.acceptArchiveRecord(first)
                == model::CacheAppendResult::Accepted);
    qnetplay::spectator::ConfirmedRecord second =
        makeVerifiedRecord(1u, first.rollingDigest);
    QNP_REQUIRE(root.acceptArchiveRecord(second)
                == model::CacheAppendResult::Accepted);
    qnetplay::spectator::ConfirmedRecord invalid = second;
    invalid.record = 2u;
    invalid.timelineFrame = 2u;
    invalid.rollingDigest ^= 0x01u;
    QNP_CHECK(root.acceptArchiveRecord(invalid)
              == model::CacheAppendResult::DigestMismatch);

    qnetplay::spectator::SetEvent event{};
    event.eventIndex = 0u;
    event.recordCursor = 2u;
    event.recordDigest = second.rollingDigest;
    event.timelineFrame = 2u;
    event.kind = qnetplay::spectator::SetEventKind::NetworkSample;
    event.valueA = 1234u;
    event.valueB = 17u;
    event.rollingDigest = qnetplay::spectator::advanceSetEventDigest(
        event.recordDigest, kGameId, event);
    QNP_REQUIRE(root.acceptArchiveEvent(event)
                == model::CacheEventAppendResult::Accepted);
    QNP_REQUIRE(root.cache() != nullptr);
    QNP_CHECK(root.cache()->range().nextCursor == 2u);
    QNP_CHECK(root.cache()->eventRange().nextEvent == 1u);

    const model::VerifiedChainCursor frontier = root.cache()->frontier();
    bindChild(*authorizer, child, frontier.nextRecord, frontier.nextEvent,
              frontier.digest);
    QNP_REQUIRE(root.issueAndSendParentOffer(
        kChildId, 2u, kNowMs + 5000u, 0xBADC0FFEu));
    QNP_REQUIRE(root.issuedLeaseCount() == 1u);
    QNP_REQUIRE(transport.sent.size() == 1u);
    wire::ControlDatagram offer{};
    QNP_REQUIRE(wire::decodeControlDatagram(
        transport.sent.back().bytes.data(), transport.sent.back().bytes.size(),
        offer) == wire::DecodeResult::Ok);
    QNP_REQUIRE(offer.type == wire::ControlMessageType::ParentLease);

    QNP_REQUIRE(root.onDatagram(endpointFor(child),
                                transport.sent.back().bytes.data(),
                                transport.sent.back().bytes.size(),
                                kNowMs + 1u)
                == runtime::ControlIngressResult::AcceptedLeaseRedemption);

    model::ContinuityRequest request{};
    request.stream = makeStream();
    request.lease = bindingFor(offer.parentLease);
    request.gameId = kGameId;
    request.expectedSourceGeneration = offer.parentLease.parentSourceGeneration;
    request.parentGeneration = 7u;
    request.exactNextCursor = 2u;
    request.exactNextEvent = 1u;
    request.overlapCursor = 0u;
    request.overlapEvent = 0u;
    request.overlapDigest = kInitialDigest;
    wire::ControlDatagram control{};
    control.type = wire::ControlMessageType::ContinuityRequest;
    control.continuityRequest = request;
    QNP_REQUIRE(wire::encodeControlDatagram(control, encoded));
    QNP_REQUIRE(root.onDatagram(endpointFor(child), encoded.data(),
                                encoded.size(), kNowMs + 2u)
                == runtime::ControlIngressResult::AcceptedContinuityRequest);
    QNP_REQUIRE(transport.sent.size() == 2u);

    wire::ControlDatagram proof{};
    QNP_REQUIRE(wire::decodeControlDatagram(
        transport.sent.back().bytes.data(), transport.sent.back().bytes.size(),
        proof) == wire::DecodeResult::Ok);
    QNP_CHECK(proof.type == wire::ControlMessageType::ContinuityProof);
    QNP_CHECK(proof.continuityProof.available);
    QNP_CHECK(proof.continuityProof.cacheNextCursor == 2u);
    QNP_CHECK(proof.continuityProof.cacheNextEvent == 1u);
    QNP_CHECK(proof.continuityProof.lease.replayNonce
              == offer.parentLease.replayNonce);

    request.lease.replayNonce ^= 1u;
    control.continuityRequest = request;
    QNP_REQUIRE(wire::encodeControlDatagram(control, encoded));
    QNP_CHECK(root.onDatagram(endpointFor(child), encoded.data(),
                              encoded.size(), kNowMs + 3u)
              == runtime::ControlIngressResult::ContinuityRejected);
    QNP_CHECK(transport.sent.size() == 2u);

    request.lease = bindingFor(offer.parentLease);
    control.continuityRequest = request;
    QNP_REQUIRE(wire::encodeControlDatagram(control, encoded));
    QNP_CHECK(root.onDatagram(endpointFor(child), encoded.data(),
                              encoded.size(), offer.parentLease.expiryMs)
              == runtime::ControlIngressResult::ContinuityRejected);
    QNP_CHECK(root.issuedLeaseCount() == 0u);
}
