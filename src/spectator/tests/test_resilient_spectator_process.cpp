#include "../ResilientSpectatorDistribution.h"
#include "ResilientSpectatorTestIdentity.h"

#include <cstdio>
#include <vector>

namespace {

using namespace qnetplay::spectator::resilient;
using qnetplay::spectator::resilient::test::syntheticStreamIdentityForTests;
using qnetplay::spectator::SpectatorGrantKey;

int g_failures = 0;

#define CHECK_TRUE(expr)                                                     \
    do {                                                                     \
        if (!(expr)) {                                                       \
            std::fprintf(stderr, "%s:%d CHECK failed: %s\n",                \
                         __FILE__, __LINE__, #expr);                         \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

#define CHECK_EQ(left, right)                                                \
    do {                                                                     \
        const auto checkLeft = (left);                                       \
        const auto checkRight = (right);                                     \
        if (!(checkLeft == checkRight)) {                                    \
            std::fprintf(stderr, "%s:%d CHECK_EQ failed: %s != %s\n",       \
                         __FILE__, __LINE__, #left, #right);                 \
            ++g_failures;                                                    \
        }                                                                    \
    } while (false)

NodeAdvertisement node(NodeId id, NodeKind kind,
                       const StreamCompatibilityIdentity& stream,
                       RouteKind routeKind, uint16_t port) {
    NodeAdvertisement out{};
    out.nodeId = id;
    out.kind = kind;
    out.stream = stream;
    out.route.kind = routeKind;
    out.route.ipv4BE = 0x0100007Fu + static_cast<uint32_t>(id);
    out.route.port = port;
    out.route.generation = 1;
    out.route.routeProved = true;
    out.capabilities = kCapabilityEphemeralEndpoint
        | kCapabilityMultiPeerAutopunch | kCapabilityMultiViewerRoot
        | kCapabilityRelayCapableSpectator | kCapabilityStandbyParent
        | kCapabilitySubtreeFailover | kCapabilityGracefulDrain;
    out.childCapacity = kind == NodeKind::SpectatorRelay ? 8u : 32u;
    out.uploadCapacity = 100u;
    out.lastProgressMs = 1u;
    out.reachable = true;
    out.cacheFirstCursor = 0;
    out.cacheNextCursor = 128;
    out.sourceGeneration = 1;
    out.routeGeneration = 1;
    return out;
}

SpectatorGrantKey key() {
    SpectatorGrantKey out{};
    for (size_t index = 0; index < out.size(); ++index)
        out[index] = static_cast<uint8_t>(0x51u + index);
    return out;
}

ParentLeaseChildBinding childBindingFor(
    const ResilientTopologyCoordinator& topology, const NodeId childId,
    const uint32_t requestedRecord) {
    ParentLeaseChildBinding binding{};
    const TopologyNodeView* const child = topology.find(childId);
    if (child == nullptr) return binding;
    binding.authorityGeneration = 0xA11CE00000000000ull | childId;
    binding.childViewerId = 0xC111D00000000000ull | childId;
    binding.childViewerGeneration = static_cast<uint32_t>(childId);
    binding.childQspRoute = child->advertisement.route;
    binding.childEndpointBindingGeneration = 0xB10D000000000000ull | childId;
    binding.childRequestedCursor = {
        requestedRecord, 0u, 0xD16E570000000000ull | childId};
    return binding;
}

RelayRecordEnvelope record(const StreamCompatibilityIdentity& stream,
                           uint64_t gameId, uint64_t sourceGeneration,
                           uint32_t cursor, uint64_t previousDigest) {
    RelayRecordEnvelope out{};
    out.stream = stream;
    out.gameId = gameId;
    out.sourceGeneration = sourceGeneration;
    out.confirmed = true;
    out.record.record = cursor;
    out.record.timelineFrame = 100u + cursor;
    out.record.playerOneInput = static_cast<uint16_t>(cursor & 0x0Fu);
    out.record.playerTwoInput = static_cast<uint16_t>((cursor + 3u) & 0x0Fu);
    out.record.stateHash = 0x600D000000000000ull ^ cursor;
    out.record.rollingDigest = advanceRollingDigest(previousDigest, gameId,
                                                     out.record);
    return out;
}

ParentLease offer(ResilientTopologyCoordinator& topology,
                  ParentLeaseRegistry& registry, NodeId child, NodeId parent,
                  uint32_t cursor, uint64_t nonce) {
    ParentLease out{};
    CHECK_TRUE(topology.makeUnsignedParentOffer(child, parent, cursor, 5000,
                                                 nonce,
                                                 childBindingFor(topology,
                                                                 child, cursor),
                                                 out));
    CHECK_TRUE(registry.issue(out));
    return out;
}

LeaseBinding leaseBindingFor(const ParentLease& lease) {
    LeaseBinding out{};
    out.childId = lease.childId;
    out.parentId = lease.parentId;
    out.parentSourceGeneration = lease.parentSourceGeneration;
    out.routeGeneration = lease.routeGeneration;
    out.expiryMs = lease.expiryMs;
    out.replayNonce = lease.replayNonce;
    out.macLow = lease.macLow;
    out.macHigh = lease.macHigh;
    out.childBinding = lease.childBinding;
    return out;
}

ContinuityProof continuityFor(const ResilientRelayNode& relay,
                              const StreamCompatibilityIdentity& stream,
                              uint64_t gameId, uint64_t sourceGeneration,
                              uint32_t offeredNext,
                              const ParentLease& offeredLease) {
    ContinuityRequest request{};
    CHECK_TRUE(relay.continuityRequest(request));
    ContinuityProof out{};
    out.stream = stream;
    out.lease = leaseBindingFor(offeredLease);
    out.gameId = gameId;
    out.sourceGeneration = sourceGeneration;
    out.parentGeneration = request.parentGeneration;
    out.cacheFirstCursor = 0;
    out.cacheNextCursor = offeredNext;
    out.overlapCursor = request.overlapCursor;
    out.overlapDigest = request.overlapDigest;
    out.available = true;
    return out;
}

void qualifyRootFanoutAndSubtreeRecovery() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xF011u);
    constexpr uint64_t kGameId = 0x4455u;
    constexpr uint64_t kInitialDigest = 0x1020304050607080ull;
    ResilientLimits limits{};
    limits.maxStandbyParents = 3;
    limits.maxConcurrentRouteRaces = 3;
    ResilientTopologyCoordinator topology(stream, limits);
    ParentLeaseRegistry leases(key());

    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream,
                                       RouteKind::Direct, 49001u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(2, NodeKind::ClientRoot, stream,
                                       RouteKind::RootFallback, 49002u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(10, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 49010u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(11, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Punched, 49011u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(12, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 49012u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(13, NodeKind::SpectatorRelay, stream,
                                       RouteKind::TrafficRelay, 49013u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(14, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Punched, 49014u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(15, NodeKind::TrafficRelay, stream,
                                       RouteKind::TrafficRelay, 49015u)),
             NodeRegistrationResult::Accepted);

    CHECK_EQ(topology.attachRootChild(1, 10), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachRootChild(1, 13), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachRootChild(1, 14), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachRootChild(1, 15), TopologyAttachResult::Attached);
    CHECK_EQ(topology.rootChildCount(1), 4u);
    RouteDescriptor fallback = topology.find(14)->advertisement.route;
    fallback.kind = RouteKind::TrafficRelay;
    fallback.generation = 2;
    CHECK_TRUE(topology.refreshRoute(14, fallback, 2));
    CHECK_EQ(topology.find(14)->advertisement.nodeId, 14u);
    CHECK_EQ(topology.find(14)->advertisement.route.kind,
             RouteKind::TrafficRelay);

    CHECK_EQ(topology.attachChild(10, 11), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachChild(11, 12), TopologyAttachResult::Attached);
    ResilientRelayNode s1(10, stream, kGameId, kInitialDigest,
                          topology, leases, limits);
    CHECK_TRUE(s1.start(1));
    CHECK_TRUE(s1.setInitialParent(1, 1, 1));
    CHECK_TRUE(s1.attachChild(11, 0));
    uint64_t digest = kInitialDigest;
    for (uint32_t cursor = 0; cursor < 6; ++cursor) {
        const RelayRecordEnvelope next = record(stream, kGameId, 1, cursor,
                                                digest);
        digest = next.record.rollingDigest;
        CHECK_EQ(s1.cache().append(next), CacheAppendResult::Accepted);
    }

    const ParentLease clientOffer = offer(topology, leases, 10, 2, 6, 1001);
    const ParentLease relayOffer = offer(topology, leases, 10, 15, 6, 1002);
    CHECK_TRUE(s1.noteUpstreamFailure(UpstreamFailureReason::TransportClosed,
                                      100));
    CHECK_EQ(s1.beginRecovery({clientOffer, relayOffer}, 2, 110,
                              childBindingFor(topology, 10, 6)),
             2u);
    const ContinuityProof clientProof = continuityFor(s1, stream, kGameId, 1,
                                                      16, clientOffer);
    CHECK_TRUE(s1.submitContinuityProof(2, clientProof));
    CHECK_TRUE(s1.commitVerifiedParent(2, 120));
    CHECK_EQ(s1.activeParent(), 2u);
    CHECK_EQ(topology.find(10)->activeParent, 2u);
    CHECK_EQ(topology.find(11)->activeParent, 10u);
    CHECK_EQ(topology.find(12)->activeParent, 11u);
    CHECK_EQ(s1.childCount(), 1u);
    CHECK_TRUE(!s1.childFallbackPermitted());
    CHECK_EQ(s1.cancelledAttemptCount(), 1u);
    s1.noteVerifiedProgress();
    CHECK_EQ(s1.state(), RelayRecoveryState::Stable);

    ResilientRelayNode s3(12, stream, kGameId, kInitialDigest,
                          topology, leases, limits);
    CHECK_TRUE(s3.start(1));
    CHECK_TRUE(s3.setInitialParent(11, 1, 1));
    uint64_t childDigest = kInitialDigest;
    for (uint32_t cursor = 0; cursor < 3; ++cursor) {
        const RelayRecordEnvelope next = record(stream, kGameId, 1, cursor,
                                                childDigest);
        childDigest = next.record.rollingDigest;
        CHECK_EQ(s3.cache().append(next), CacheAppendResult::Accepted);
    }
    const ParentLease s3Offer = offer(topology, leases, 12, 2, 3, 2001);
    CHECK_TRUE(s3.noteUpstreamFailure(UpstreamFailureReason::TransportClosed,
                                      200));
    CHECK_EQ(s3.beginRecovery({s3Offer}, 1, 210,
                              childBindingFor(topology, 12, 3)),
             1u);
    const ContinuityProof s3Proof = continuityFor(s3, stream, kGameId, 1, 8,
                                                  s3Offer);
    CHECK_TRUE(s3.submitContinuityProof(2, s3Proof));
    CHECK_TRUE(s3.commitVerifiedParent(2, 220));
    CHECK_EQ(topology.find(12)->activeParent, 2u);
    CHECK_EQ(topology.find(10)->activeParent, 2u);
    CHECK_EQ(topology.find(11)->activeParent, 10u);

    for (uint64_t generation = 3; generation < 20; ++generation) {
        RouteDescriptor updated = topology.find(13)->advertisement.route;
        updated.port = static_cast<uint16_t>(50000u + generation);
        updated.generation = generation;
        CHECK_TRUE(topology.refreshRoute(13, updated, generation));
    }
    CHECK_EQ(topology.nodeCount(), 8u);
    CHECK_EQ(topology.find(13)->advertisement.routeGeneration, 19u);
}

}

int main() {
    qualifyRootFanoutAndSubtreeRecovery();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d deterministic qualification check(s) failed\n",
                     g_failures);
        return 1;
    }
    std::puts("resilient spectator process-model qualification: PASS");
    return 0;
}
