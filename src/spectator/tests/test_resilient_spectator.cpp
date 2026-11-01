#include "../ResilientSpectatorDistribution.h"
#include "../ResilientSpectatorProtocol.h"
#include "ResilientSpectatorTestIdentity.h"

#include "../../replay/ReplayIntegrity.h"

#include <cstdio>

namespace {

using namespace qnetplay::spectator::resilient;
using qnetplay::spectator::resilient::test::syntheticStreamIdentityForTests;
using qnetplay::spectator::SpectatorGrantKey;

int g_failures = 0;

struct CloseLedger {
    std::vector<RelaySocketHandle> closed;
};

void closeRecordedSocket(RelaySocketHandle socket, void* context) {
    static_cast<CloseLedger*>(context)->closed.push_back(socket);
}

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
                       RouteKind routeKind = RouteKind::Direct,
                       uint32_t ipv4 = 0x0100007Fu,
                       uint16_t port = 40000u) {
    NodeAdvertisement out{};
    out.nodeId = id;
    out.kind = kind;
    out.stream = stream;
    out.route.kind = routeKind;
    out.route.ipv4BE = ipv4;
    out.route.port = port;
    out.route.generation = 1;
    out.route.routeProved = true;
    out.capabilities = kCapabilityEphemeralEndpoint
        | kCapabilityMultiPeerAutopunch | kCapabilityMultiViewerRoot;
    out.childCapacity = kind == NodeKind::SpectatorRelay ? 8u : 32u;
    out.uploadCapacity = 100u;
    out.lastProgressMs = 1u;
    out.reachable = true;
    out.cacheFirstCursor = 0;
    out.cacheNextCursor = 64;
    out.sourceGeneration = 1;
    out.routeGeneration = 1;
    return out;
}

RelayRecordEnvelope confirmedRecord(const StreamCompatibilityIdentity& stream,
                                    uint64_t gameId,
                                    uint64_t sourceGeneration,
                                    uint32_t cursor,
                                    uint64_t previousDigest) {
    RelayRecordEnvelope envelope{};
    envelope.stream = stream;
    envelope.gameId = gameId;
    envelope.sourceGeneration = sourceGeneration;
    envelope.confirmed = true;
    envelope.record.record = cursor;
    envelope.record.timelineFrame = 1000u + cursor;
    envelope.record.playerOneInput = static_cast<uint16_t>(cursor & 0x0Fu);
    envelope.record.playerTwoInput = static_cast<uint16_t>((cursor + 1u) & 0x0Fu);
    envelope.record.stateHash = 0xBEEF000000000000ull ^ cursor;
    envelope.record.rollingDigest = advanceRollingDigest(
        previousDigest, gameId, envelope.record);
    return envelope;
}

RelayEventEnvelope confirmedEvent(const StreamCompatibilityIdentity& stream,
                                  uint64_t gameId,
                                  uint64_t sourceGeneration,
                                  uint32_t eventIndex,
                                  uint32_t recordCursor,
                                  uint64_t previousDigest,
                                  uint32_t firstTimelineFrame) {
    RelayEventEnvelope envelope{};
    envelope.stream = stream;
    envelope.gameId = gameId;
    envelope.sourceGeneration = sourceGeneration;
    envelope.confirmed = true;
    envelope.event.eventIndex = eventIndex;
    envelope.event.recordCursor = recordCursor;
    envelope.event.recordDigest = previousDigest;
    envelope.event.timelineFrame = firstTimelineFrame + recordCursor;
    envelope.event.kind = qnetplay::spectator::SetEventKind::NetworkSample;
    envelope.event.valueA = 1234u;
    envelope.event.valueB = 17u;
    envelope.event.rollingDigest = advanceSetEventDigest(
        previousDigest, gameId, envelope.event);
    return envelope;
}

void identityIsExactAcrossAllFields() {
    const StreamCompatibilityIdentity identity =
        syntheticStreamIdentityForTests(0x1234u);
    CHECK_TRUE(identity.valid());
    CHECK_EQ(identity, syntheticStreamIdentityForTests(0x1234u));

    for (unsigned field = 0; field < 9; ++field) {
        StreamCompatibilityIdentity different = identity;
        switch (field) {
            case 0: ++different.setOrSessionId; break;
            case 1: ++different.gameEpoch; break;
            case 2: ++different.compatibility.runtimeBuild.executableSha256[0]; break;
            case 3: ++different.compatibility.simulationContent.simulationRootSha256[0]; break;
            case 4: ++different.compatibility.simulationContent.validationRootSha256[0]; break;
            case 5: ++different.compatibility.stateSchemaId; break;
            case 6: ++different.compatibility.modSimulationId; break;
            case 7: ++different.compatibility.timingProfileId; break;
            default: ++different.streamRootIdentity; break;
        }
        CHECK_TRUE(different.valid());
        CHECK_TRUE(different != identity);
    }

    StreamCompatibilityIdentity invalid = identity;
    for (uint8_t& byte : invalid.compatibility.simulationContent
                             .simulationRootSha256) {
        byte = 0;
    }
    CHECK_TRUE(!invalid.valid());
}

void hostAndClientRootsFanOutIndependently() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xA11CEu);
    ResilientTopologyCoordinator topology(stream);
    CHECK_TRUE(topology.configured());
    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(2, NodeKind::ClientRoot, stream,
                                       RouteKind::RootFallback, 0x0200007Fu,
                                       40001u)),
             NodeRegistrationResult::Accepted);

    CHECK_EQ(topology.registerNode(node(11, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0300007Fu,
                                       41001u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(12, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Punched, 0x0400007Fu,
                                       41002u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(13, NodeKind::SpectatorRelay, stream,
                                       RouteKind::TrafficRelay, 0x0500007Fu,
                                       41003u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(14, NodeKind::SpectatorRelay, stream,
                                       RouteKind::RootFallback, 0x0600007Fu,
                                       41004u)),
             NodeRegistrationResult::Accepted);

    CHECK_EQ(topology.attachRootChild(1, 11), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachRootChild(1, 12), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachRootChild(1, 13), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachRootChild(2, 14), TopologyAttachResult::Attached);
    CHECK_EQ(topology.rootCount(), 2u);
    CHECK_EQ(topology.rootChildCount(1), 3u);
    CHECK_EQ(topology.rootChildCount(2), 1u);
    CHECK_EQ(topology.attachRootChild(1, 2), TopologyAttachResult::ChildIsRoot);

    const TopologyNodeView* relay = topology.find(12);
    CHECK_TRUE(relay != nullptr);
    CHECK_EQ(relay->activeParent, 1u);
    CHECK_EQ(relay->depth, 1u);
    CHECK_EQ(relay->ancestryCount, 1u);
    CHECK_EQ(relay->ancestry[0], 1u);
}

void nodeIdentitySurvivesEndpointRebinding() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xB1D0u);
    ResilientTopologyCoordinator topology(stream);
    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);
    NodeAdvertisement relay = node(22, NodeKind::SpectatorRelay, stream,
                                   RouteKind::Punched, 0x0100007Fu, 42000u);
    CHECK_EQ(topology.registerNode(relay), NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.attachRootChild(1, 22), TopologyAttachResult::Attached);

    relay.route.port = 48127u;
    relay.route.generation = 2;
    relay.routeGeneration = 2;
    CHECK_EQ(topology.registerNode(relay), NodeRegistrationResult::UpdatedRoute);
    const TopologyNodeView* after = topology.find(22);
    CHECK_TRUE(after != nullptr);
    CHECK_EQ(after->advertisement.route.port, 48127u);
    CHECK_EQ(after->activeParent, 1u);
    CHECK_EQ(topology.rootChildCount(1), 1u);
}

void topologyRejectsWrongIdentityAndBoundsRootChildren() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xCAFEu);
    ResilientLimits limits{};
    limits.maxChildrenPerRoot = 2;
    ResilientTopologyCoordinator topology(stream, limits);
    CHECK_TRUE(topology.configured());
    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);

    NodeAdvertisement wrong = node(2, NodeKind::SpectatorRelay,
                                  syntheticStreamIdentityForTests(0xBEEFu));
    CHECK_EQ(topology.registerNode(wrong), NodeRegistrationResult::InvalidIdentity);
    wrong = node(2, NodeKind::SpectatorRelay, stream);
    wrong.capabilityModelRevision = kResilientCapabilityModelRevision + 1;
    CHECK_EQ(topology.registerNode(wrong),
             NodeRegistrationResult::UnsupportedCapabilityModel);

    for (NodeId id = 2; id <= 4; ++id) {
        CHECK_EQ(topology.registerNode(node(id, NodeKind::SpectatorRelay,
                                            stream, RouteKind::Direct,
                                            0x0100007Fu,
                                            static_cast<uint16_t>(43000u + id))),
                 NodeRegistrationResult::Accepted);
    }
    CHECK_EQ(topology.attachRootChild(1, 2), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachRootChild(1, 3), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachRootChild(1, 4),
             TopologyAttachResult::ParentAtCapacity);
}

void cacheForwardsOnlyVerifiedExactStreamRecords() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xCACEu);
    constexpr uint64_t kGameId = 0x9001u;
    constexpr uint64_t kInitialDigest = 0x1122334455667788ull;
    VerifiedRelayCache cache;
    CHECK_TRUE(cache.bind(stream, kGameId, kInitialDigest, 7));

    RelayRecordEnvelope speculative =
        confirmedRecord(stream, kGameId, 7, 0, kInitialDigest);
    speculative.confirmed = false;
    CHECK_EQ(cache.append(speculative), CacheAppendResult::Speculative);

    RelayRecordEnvelope wrongEpoch = speculative;
    wrongEpoch.confirmed = true;
    ++wrongEpoch.stream.gameEpoch;
    CHECK_EQ(cache.append(wrongEpoch), CacheAppendResult::WrongEpoch);
    RelayRecordEnvelope wrongContent = speculative;
    wrongContent.confirmed = true;
    ++wrongContent.stream.compatibility.simulationContent
                         .simulationRootSha256[0];
    CHECK_EQ(cache.append(wrongContent),
             CacheAppendResult::WrongContentIdentity);

    RelayRecordEnvelope record0 =
        confirmedRecord(stream, kGameId, 7, 0, kInitialDigest);
    CHECK_EQ(cache.append(record0), CacheAppendResult::Accepted);
    CHECK_EQ(cache.append(record0), CacheAppendResult::ExactDuplicate);

    RelayRecordEnvelope badDigest =
        confirmedRecord(stream, kGameId, 7, 1, record0.record.rollingDigest);
    ++badDigest.record.rollingDigest;
    CHECK_EQ(cache.append(badDigest), CacheAppendResult::DigestMismatch);
    RelayRecordEnvelope gap =
        confirmedRecord(stream, kGameId, 7, 2, record0.record.rollingDigest);
    CHECK_EQ(cache.append(gap), CacheAppendResult::Gap);

    RelayRecordEnvelope record1 =
        confirmedRecord(stream, kGameId, 7, 1, record0.record.rollingDigest);
    CHECK_EQ(cache.append(record1), CacheAppendResult::Accepted);
    CHECK_EQ(cache.range().firstCursor, 0u);
    CHECK_EQ(cache.range().nextCursor, 2u);
    CHECK_TRUE(cache.attachChild(99, 0));

    std::vector<VerifiedRelayRecord> delivered;
    CHECK_EQ(cache.readForChild(99, 0, 4, delivered), CacheReadResult::Accepted);
    CHECK_EQ(delivered.size(), 2u);
    CHECK_EQ(delivered[0].record, record0.record);
    CHECK_EQ(cache.readForChild(99, 3, 1, delivered),
             CacheReadResult::FutureCursor);
    CHECK_TRUE(!cache.acknowledgeChild(99, 3));
}

void cacheLimitsRequireJointRecordAndEventFootprint() {
    ResilientLimits limits{};
    limits.routeRaceRecordBudget = 1;
    limits.continuityOverlapRecords = 1;
    limits.retransmissionMarginRecords = 1;
    limits.bootstrapMarginRecords = 1;
    limits.maxCachedRecords = limits.requiredCacheRecords();
    limits.maxCachedEvents = limits.requiredCacheRecords();
    limits.maxCheckpointBytes = 1u;
    const size_t requiredPayload = limits.maxCachedRecords
            * sizeof(VerifiedRelayRecord)
        + limits.maxCachedEvents * sizeof(VerifiedRelayEvent);
    CHECK_TRUE(requiredPayload > 0u);
    limits.maxCachedBytes = requiredPayload - 1u;
    CHECK_TRUE(!limits.valid());
    limits.maxCachedBytes = requiredPayload;
    CHECK_TRUE(limits.valid());
}

void cacheBoundsMemoryAndResyncsOnlyLaggingChildren() {
    ResilientLimits limits{};
    limits.routeRaceRecordBudget = 1;
    limits.continuityOverlapRecords = 1;
    limits.retransmissionMarginRecords = 1;
    limits.bootstrapMarginRecords = 1;
    limits.maxCachedRecords = limits.requiredCacheRecords();
    limits.maxCachedEvents = limits.requiredCacheRecords();
    limits.maxCachedBytes = limits.maxCachedRecords
            * sizeof(VerifiedRelayRecord)
        + limits.maxCachedEvents * sizeof(VerifiedRelayEvent);
    limits.maxCheckpoints = 2;
    limits.maxCheckpointBytes = 128;
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xC0ACu);
    constexpr uint64_t kGameId = 0x9002u;
    constexpr uint64_t kInitialDigest = 0x8877665544332211ull;
    VerifiedRelayCache cache(limits);
    CHECK_TRUE(cache.bind(stream, kGameId, kInitialDigest, 4));
    CHECK_TRUE(cache.attachChild(100, 0));
    CHECK_TRUE(cache.attachChild(101, 0));

    uint64_t digest = kInitialDigest;
    for (uint32_t cursor = 0; cursor < 4; ++cursor) {
        RelayRecordEnvelope record =
            confirmedRecord(stream, kGameId, 4, cursor, digest);
        digest = record.record.rollingDigest;
        CHECK_EQ(cache.append(record), CacheAppendResult::Accepted);
    }
    CHECK_TRUE(cache.acknowledgeChild(100, 4));
    uint64_t checkpointDigest = 0;
    CHECK_TRUE(cache.digestAt(3, checkpointDigest));
    VerifiedCheckpoint checkpoint{};
    checkpoint.stream = stream;
    checkpoint.gameId = kGameId;
    checkpoint.sourceGeneration = 4;
    checkpoint.cursor = 3;
    checkpoint.digest = checkpointDigest;
    checkpoint.bytes = {1, 2, 3, 4};
    checkpoint.complete = true;
    checkpoint.integrityVerified = true;
    CHECK_EQ(cache.addCheckpoint(checkpoint), CacheCheckpointResult::Accepted);

    RelayRecordEnvelope record4 =
        confirmedRecord(stream, kGameId, 4, 4, digest);
    digest = record4.record.rollingDigest;
    CHECK_EQ(cache.append(record4), CacheAppendResult::Accepted);
    CHECK_EQ(cache.range().firstCursor, 1u);
    CHECK_EQ(cache.range().nextCursor, 5u);
    CHECK_EQ(cache.checkpointCount(), 1u);
    CHECK_TRUE(cache.retainedBytes() <= limits.maxCachedBytes);
    const CacheChildStatus* healthy = cache.child(100);
    const CacheChildStatus* lagging = cache.child(101);
    CHECK_TRUE(healthy != nullptr && !healthy->controlledResyncRequired);
    CHECK_TRUE(lagging != nullptr && lagging->controlledResyncRequired);

    std::vector<VerifiedRelayRecord> delivered;
    CHECK_EQ(cache.readForChild(100, 1, 4, delivered), CacheReadResult::Accepted);
    CHECK_EQ(delivered.size(), 4u);
    CHECK_EQ(cache.readForChild(101, 0, 4, delivered),
             CacheReadResult::ControlledResync);

    ContinuityProof proof{};
    CHECK_TRUE(cache.continuityProof(5, 2, proof));
    CHECK_TRUE(proof.available);
    CHECK_EQ(proof.overlapCursor, 3u);
    CHECK_TRUE(!cache.continuityProof(6, 1, proof));
}

void cacheTracksVerifiedSetEventsOnTheSharedChain() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xE71Eu);
    constexpr uint64_t kGameId = 0x9004u;
    constexpr uint64_t kInitialDigest = 0x1029384756ABCDEFull;
    constexpr uint32_t kFirstTimelineFrame = 700u;
    VerifiedRelayCache cache;
    CHECK_TRUE(cache.bind(stream, kGameId, kInitialDigest, 9,
                          kFirstTimelineFrame));

    RelayRecordEnvelope record0 =
        confirmedRecord(stream, kGameId, 9, 0, kInitialDigest);
    CHECK_EQ(cache.append(record0), CacheAppendResult::Accepted);

    RelayEventEnvelope event0 = confirmedEvent(
        stream, kGameId, 9, 0, 1, record0.record.rollingDigest,
        kFirstTimelineFrame);
    CHECK_EQ(cache.appendEvent(event0), CacheEventAppendResult::Accepted);
    CHECK_EQ(cache.appendEvent(event0),
             CacheEventAppendResult::ExactDuplicate);
    CHECK_EQ(cache.eventRange().firstEvent, 0u);
    CHECK_EQ(cache.eventRange().nextEvent, 1u);
    CHECK_EQ(cache.frontier().nextRecord, 1u);
    CHECK_EQ(cache.frontier().nextEvent, 1u);
    CHECK_EQ(cache.frontier().digest, event0.event.rollingDigest);

    uint64_t beforeBoundary = 0;
    uint64_t afterBoundary = 0;
    CHECK_TRUE(cache.digestAt(1, 0, beforeBoundary));
    CHECK_TRUE(cache.digestAt(1, 1, afterBoundary));
    CHECK_EQ(beforeBoundary, record0.record.rollingDigest);
    CHECK_EQ(afterBoundary, event0.event.rollingDigest);
    CHECK_TRUE(cache.digestAt(1, afterBoundary));
    CHECK_EQ(afterBoundary, event0.event.rollingDigest);

    RelayRecordEnvelope record1 = confirmedRecord(
        stream, kGameId, 9, 1, event0.event.rollingDigest);
    CHECK_EQ(cache.append(record1), CacheAppendResult::Accepted);
    CHECK_EQ(cache.frontier().nextRecord, 2u);
    CHECK_EQ(cache.frontier().nextEvent, 1u);
    CHECK_EQ(cache.frontier().digest, record1.record.rollingDigest);

    CHECK_TRUE(!cache.attachChild(87, 0, 0, kInitialDigest ^ 0x1ull));
    CHECK_TRUE(cache.attachChild(87, 0, 0, kInitialDigest));
    std::vector<VerifiedRelayRecord> records;
    CHECK_EQ(cache.readForChild(87, 0, 4, records), CacheReadResult::Accepted);
    CHECK_EQ(records.size(), 1u);
    CHECK_EQ(records[0].record, record0.record);
    CHECK_TRUE(!cache.acknowledgeChild(87, 1, 0,
                                       record0.record.rollingDigest ^ 0x1ull));
    CHECK_TRUE(cache.acknowledgeChild(87, 1, 0,
                                      record0.record.rollingDigest));
    CHECK_EQ(cache.readForChild(87, 1, 4, records),
             CacheReadResult::EventCatchupRequired);
    std::vector<VerifiedRelayEvent> events;
    CHECK_EQ(cache.readEventsForChild(87, 0, 4, events),
             CacheReadResult::Accepted);
    CHECK_EQ(events.size(), 1u);
    CHECK_EQ(events[0].event, event0.event);
    CHECK_TRUE(!cache.attachChild(87, 1, 1,
                                  record0.record.rollingDigest));
    CHECK_TRUE(cache.attachChild(87, 1, 1, event0.event.rollingDigest));
    CHECK_EQ(cache.readForChild(87, 1, 4, records), CacheReadResult::Accepted);
    CHECK_EQ(records.size(), 1u);
    CHECK_EQ(records[0].record, record1.record);

    ContinuityProof proof{};
    CHECK_TRUE(cache.continuityProof(2, 1, 1, proof));
    CHECK_EQ(proof.cacheFirstEvent, 0u);
    CHECK_EQ(proof.cacheNextEvent, 1u);
    CHECK_EQ(proof.overlapCursor, 1u);
    CHECK_EQ(proof.overlapEvent, 1u);
    CHECK_EQ(proof.overlapDigest, event0.event.rollingDigest);

    VerifiedRelayCache deferredTimeline;
    CHECK_TRUE(deferredTimeline.bind(stream, kGameId, kInitialDigest, 9));
    CHECK_EQ(deferredTimeline.appendEvent(event0),
             CacheEventAppendResult::TimelineUnbound);
    CHECK_TRUE(deferredTimeline.configureEventTimeline(kFirstTimelineFrame));
    CHECK_EQ(deferredTimeline.append(record0), CacheAppendResult::Accepted);
    CHECK_EQ(deferredTimeline.appendEvent(event0),
             CacheEventAppendResult::Accepted);
}

void cacheEvictsEventRunwayOnlyWithItsRecordBoundary() {
    ResilientLimits limits{};
    limits.routeRaceRecordBudget = 1;
    limits.continuityOverlapRecords = 1;
    limits.retransmissionMarginRecords = 1;
    limits.bootstrapMarginRecords = 1;
    limits.maxCachedRecords = limits.requiredCacheRecords();
    limits.maxCachedEvents = limits.maxCachedRecords;
    limits.maxCheckpointBytes = 1u;
    limits.maxCachedBytes = limits.maxCachedRecords
            * sizeof(VerifiedRelayRecord)
        + limits.maxCachedEvents * sizeof(VerifiedRelayEvent);
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xE71F);
    constexpr uint64_t kGameId = 0x9005u;
    constexpr uint64_t kInitialDigest = 0x66778899AABBCCDDull;
    constexpr uint32_t kFirstTimelineFrame = 800u;
    VerifiedRelayCache cache(limits);
    CHECK_TRUE(cache.bind(stream, kGameId, kInitialDigest, 6,
                          kFirstTimelineFrame));
    CHECK_TRUE(cache.attachChild(88, 0, 0));

    uint64_t digest = kInitialDigest;
    for (uint32_t cursor = 0; cursor < 5; ++cursor) {
        RelayEventEnvelope event = confirmedEvent(
            stream, kGameId, 6, cursor, cursor, digest,
            kFirstTimelineFrame);
        CHECK_EQ(cache.appendEvent(event), CacheEventAppendResult::Accepted);
        RelayRecordEnvelope record = confirmedRecord(
            stream, kGameId, 6, cursor, event.event.rollingDigest);
        CHECK_EQ(cache.append(record), CacheAppendResult::Accepted);
        digest = record.record.rollingDigest;
    }

    CHECK_EQ(cache.range().firstCursor, 1u);
    CHECK_EQ(cache.eventRange().firstEvent, 1u);
    CHECK_EQ(cache.recordCount(), 4u);
    CHECK_EQ(cache.eventCount(), 4u);
    const CacheChildStatus* child = cache.child(88);
    CHECK_TRUE(child != nullptr && child->controlledResyncRequired);
    std::vector<VerifiedRelayEvent> events;
    CHECK_EQ(cache.readEventsForChild(88, 0, 4, events),
             CacheReadResult::ControlledResync);
}

void cacheRetiresBoundaryBeforeRecordAfterEventFillsRunway() {
    ResilientLimits limits{};
    limits.routeRaceRecordBudget = 1;
    limits.continuityOverlapRecords = 1;
    limits.retransmissionMarginRecords = 1;
    limits.bootstrapMarginRecords = 1;
    limits.maxCachedRecords = limits.requiredCacheRecords();
    limits.maxCachedEvents = limits.maxCachedRecords;
    limits.maxCheckpointBytes = 1u;
    limits.maxCachedBytes = limits.maxCachedRecords
            * sizeof(VerifiedRelayRecord)
        + limits.maxCachedEvents * sizeof(VerifiedRelayEvent);
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xE720u);
    constexpr uint64_t kGameId = 0x9006u;
    constexpr uint64_t kInitialDigest = 0x8899AABBCCDDEEFFull;
    constexpr uint32_t kFirstTimelineFrame = 900u;
    VerifiedRelayCache cache(limits);
    CHECK_TRUE(cache.bind(stream, kGameId, kInitialDigest, 7,
                          kFirstTimelineFrame));

    uint64_t digest = kInitialDigest;
    for (uint32_t cursor = 0; cursor < 4u; ++cursor) {
        if (cursor < 3u) {
            RelayEventEnvelope event = confirmedEvent(
                stream, kGameId, 7, cursor, cursor, digest,
                kFirstTimelineFrame);
            CHECK_EQ(cache.appendEvent(event), CacheEventAppendResult::Accepted);
            digest = event.event.rollingDigest;
        }
        RelayRecordEnvelope record = confirmedRecord(
            stream, kGameId, 7, cursor, digest);
        CHECK_EQ(cache.append(record), CacheAppendResult::Accepted);
        digest = record.record.rollingDigest;
    }
    CHECK_EQ(cache.recordCount(), 4u);
    CHECK_EQ(cache.eventCount(), 3u);

    RelayEventEnvelope finalEvent = confirmedEvent(
        stream, kGameId, 7, 3u, 4u, digest, kFirstTimelineFrame);
    CHECK_EQ(cache.appendEvent(finalEvent), CacheEventAppendResult::Accepted);
    RelayRecordEnvelope finalRecord = confirmedRecord(
        stream, kGameId, 7, 4u, finalEvent.event.rollingDigest);
    CHECK_EQ(cache.append(finalRecord), CacheAppendResult::Accepted);
    CHECK_EQ(cache.range().firstCursor, 1u);
    CHECK_EQ(cache.range().nextCursor, 5u);
    CHECK_EQ(cache.eventRange().firstEvent, 1u);
    CHECK_EQ(cache.eventRange().nextEvent, 4u);
    CHECK_EQ(cache.recordCount(), 4u);
    CHECK_EQ(cache.eventCount(), 3u);
}

void relayOwnsDifferentUpstreamAndDownstreamSocketsExactlyOnce() {
    CloseLedger rejected{};
    {
        RelaySocketOwnership sockets;
        CHECK_TRUE(!sockets.adopt(77, 77, &closeRecordedSocket, &rejected));
        CHECK_TRUE(!sockets.configured());
    }
    CHECK_EQ(rejected.closed.size(), 1u);
    CHECK_EQ(rejected.closed[0], 77u);

    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xD0A5u);
    constexpr uint64_t kGameId = 0x9003u;
    constexpr uint64_t kInitialDigest = 0x123456789ABCDEF0ull;
    CloseLedger ledger{};
    {
        VerifiedRelayDownstream relay;
        CHECK_TRUE(relay.start(stream, kGameId, kInitialDigest, 5,
                               101, 202, &closeRecordedSocket, &ledger));
        CHECK_TRUE(relay.running());
        CHECK_EQ(relay.sockets().upstream(), 101u);
        CHECK_EQ(relay.sockets().downstream(), 202u);
        CHECK_TRUE(relay.cache().attachChild(71, 0));
        const RelayRecordEnvelope record =
            confirmedRecord(stream, kGameId, 5, 0, kInitialDigest);
        CHECK_EQ(relay.acceptUpstream(record), CacheAppendResult::Accepted);
        std::vector<VerifiedRelayRecord> delivered;
        CHECK_EQ(relay.serveChild(71, 0, 1, delivered),
                 CacheReadResult::Accepted);
        CHECK_EQ(delivered.size(), 1u);
        relay.stop();
        CHECK_TRUE(!relay.running());
        relay.stop();
    }
    CHECK_EQ(ledger.closed.size(), 2u);
    CHECK_EQ(ledger.closed[0], 101u);
    CHECK_EQ(ledger.closed[1], 202u);

    CloseLedger borrowedLedger{};
    VerifiedRelayCache sharedCache;
    CHECK_TRUE(sharedCache.bind(stream, kGameId, kInitialDigest, 5));
    {
        VerifiedRelayDownstream relay;
        CHECK_TRUE(relay.startWithCache(sharedCache, 303, 404,
                                        &closeRecordedSocket,
                                        &borrowedLedger));
        CHECK_TRUE(relay.running());
        CHECK_TRUE(relay.borrowsCache());
        CHECK_TRUE(&relay.cache() == &sharedCache);
        CHECK_TRUE(sharedCache.attachChild(72, 0));
        const RelayRecordEnvelope record =
            confirmedRecord(stream, kGameId, 5, 0, kInitialDigest);
        CHECK_EQ(relay.acceptUpstream(record), CacheAppendResult::Accepted);
        relay.stop();
        CHECK_TRUE(sharedCache.configured());
        CHECK_EQ(sharedCache.recordCount(), 1u);
    }
    CHECK_EQ(borrowedLedger.closed.size(), 2u);
    CHECK_EQ(borrowedLedger.closed[0], 303u);
    CHECK_EQ(borrowedLedger.closed[1], 404u);
}

SpectatorGrantKey leaseKey() {
    SpectatorGrantKey key{};
    for (size_t index = 0; index < key.size(); ++index)
        key[index] = static_cast<uint8_t>(0xA0u + index);
    return key;
}

ParentLeaseChildBinding childBindingFor(
    const ResilientTopologyCoordinator& topology, const NodeId childId,
    const uint32_t requestedRecord, const uint64_t nonce) {
    ParentLeaseChildBinding binding{};
    const TopologyNodeView* const child = topology.find(childId);
    if (child == nullptr) return binding;
    binding.authorityGeneration = 0xA11CE00000000000ull | nonce;
    binding.childViewerId = 0xC111D00000000000ull
        | (static_cast<uint64_t>(childId) << 16u) | (nonce & 0xFFFFu);
    binding.childViewerGeneration =
        static_cast<uint32_t>((nonce & 0x7FFFFFFFu) + 1u);
    binding.childQspRoute = child->advertisement.route;
    binding.childEndpointBindingGeneration = 0xB10D000000000000ull | nonce;
    binding.childRequestedCursor = {
        requestedRecord, 0u,
        0xD16E570000000000ull | nonce};
    return binding;
}

void parentLeaseIsAuthenticatedBoundAndSingleUse() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0x1EA5Eu);
    ResilientTopologyCoordinator topology(stream);
    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(2, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Punched, 0x0200007Fu, 45002u)),
             NodeRegistrationResult::Accepted);

    ParentLease lease{};
    const ParentLeaseChildBinding childBinding =
        childBindingFor(topology, 2, 8, 41);
    CHECK_TRUE(topology.makeUnsignedParentOffer(2, 1, 8, 1000, 41,
                                                 childBinding, lease));
    ParentLeaseRegistry registry(leaseKey());
    CHECK_TRUE(registry.issue(lease));

    ParentLeaseExpectation expected{};
    expected.stream = stream;
    expected.expectedChildId = 2;
    expected.expectedParentId = 1;
    expected.expectedChildBinding = childBinding;
    expected.nowMs = 100;
    expected.minimumSourceGeneration = 1;
    expected.minimumRouteGeneration = 1;
    expected.requiredCursor = 8;
    expected.maxActiveDepth = topology.limits().maxActiveDepth;
    expected.parentHasCapacity = true;
    expected.loopFree = true;
    CHECK_EQ(registry.verify(lease, expected), ParentLeaseVerdict::Accepted);
    CHECK_EQ(registry.redeem(lease, expected), ParentLeaseVerdict::Accepted);
    CHECK_EQ(registry.redeem(lease, expected), ParentLeaseVerdict::Replayed);
    CHECK_TRUE(registry.wasRedeemed(lease.replayNonce));
    CHECK_EQ(registry.verify(lease, expected), ParentLeaseVerdict::Replayed);
    CHECK_EQ(registry.verifyContinuity(lease, expected),
             ParentLeaseVerdict::Accepted);

    ParentLease tampered = lease;
    ++tampered.routes[0].port;
    CHECK_EQ(registry.verify(tampered, expected), ParentLeaseVerdict::BadMac);
    ParentLease wrongStream = lease;
    ++wrongStream.stream.compatibility.stateSchemaId;
    CHECK_TRUE(registry.issue(wrongStream));
    CHECK_EQ(registry.verify(wrongStream, expected),
             ParentLeaseVerdict::WrongStream);
    ParentLeaseExpectation wrongChild = expected;
    wrongChild.expectedChildId = 3;
    CHECK_EQ(registry.verify(lease, wrongChild), ParentLeaseVerdict::WrongChild);
    ParentLeaseExpectation wrongParent = expected;
    wrongParent.expectedParentId = 3;
    CHECK_EQ(registry.verify(lease, wrongParent), ParentLeaseVerdict::WrongParent);
    ParentLeaseExpectation staleSource = expected;
    staleSource.minimumSourceGeneration = 2;
    CHECK_EQ(registry.verify(lease, staleSource),
             ParentLeaseVerdict::StaleSourceGeneration);
    ParentLeaseExpectation staleRoute = expected;
    staleRoute.minimumRouteGeneration = 2;
    CHECK_EQ(registry.verify(lease, staleRoute),
             ParentLeaseVerdict::StaleRouteGeneration);
    ParentLease futureSource = lease;
    ++futureSource.parentSourceGeneration;
    CHECK_TRUE(registry.issue(futureSource));
    CHECK_EQ(registry.verify(futureSource, expected),
             ParentLeaseVerdict::StaleSourceGeneration);
    ParentLeaseExpectation noCapacity = expected;
    noCapacity.parentHasCapacity = false;
    CHECK_EQ(registry.verify(lease, noCapacity),
             ParentLeaseVerdict::CapacityReached);
    ParentLeaseExpectation loop = expected;
    loop.loopFree = false;
    CHECK_EQ(registry.verify(lease, loop), ParentLeaseVerdict::LoopRejected);
    ParentLeaseExpectation badCursor = expected;
    badCursor.requiredCursor = 65;
    CHECK_EQ(registry.verify(lease, badCursor),
             ParentLeaseVerdict::CursorUnavailable);
    ParentLeaseExpectation expired = expected;
    expired.nowMs = 1000;
    CHECK_EQ(registry.verify(lease, expired), ParentLeaseVerdict::Expired);

    ParentLease attachLease{};
    const ParentLeaseChildBinding attachBinding =
        childBindingFor(topology, 2, 8, 42);
    CHECK_TRUE(topology.makeUnsignedParentOffer(2, 1, 8, 1000, 42,
                                                 attachBinding, attachLease));
    CHECK_TRUE(registry.issue(attachLease));
    CHECK_EQ(topology.redeemAndAttach(registry, attachLease, 100, 8,
                                      attachBinding),
             ParentLeaseVerdict::Accepted);
    const TopologyNodeView* attached = topology.find(2);
    CHECK_TRUE(attached != nullptr);
    CHECK_EQ(attached->activeParent, 1u);
}

void topologyEnforcesLoopsDepthCapacityDrainAndDiverseStandbys() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0x70F0u);
    ResilientLimits limits{};
    limits.maxChildrenPerRelay = 1;
    limits.maxActiveDepth = 4;
    ResilientTopologyCoordinator topology(stream, limits);
    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(2, NodeKind::ClientRoot, stream,
                                       RouteKind::RootFallback, 0x0200007Fu,
                                       46002u)), NodeRegistrationResult::Accepted);
    for (NodeId id = 10; id <= 15; ++id) {
        const NodeKind kind = id == 15 ? NodeKind::TrafficRelay
                                       : NodeKind::SpectatorRelay;
        const RouteKind route = id == 15 ? RouteKind::TrafficRelay
                                          : RouteKind::Direct;
        CHECK_EQ(topology.registerNode(node(id, kind, stream, route,
                                            static_cast<uint32_t>(id),
                                            static_cast<uint16_t>(46000u + id))),
                 NodeRegistrationResult::Accepted);
    }
    CHECK_EQ(topology.attachRootChild(1, 10), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachChild(10, 11), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachChild(11, 12), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachChild(12, 13), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachChild(13, 14), TopologyAttachResult::DepthExceeded);

    CHECK_EQ(topology.attachChild(11, 14), TopologyAttachResult::ParentAtCapacity);
    CHECK_TRUE(topology.detach(10));
    CHECK_EQ(topology.attachChild(13, 10),
             TopologyAttachResult::ParentIsDescendant);

    CHECK_TRUE(topology.setDraining(2, true));
    CHECK_EQ(topology.attachRootChild(2, 14),
             TopologyAttachResult::ParentDraining);
    CHECK_TRUE(topology.setDraining(2, false));

    ResilientTopologyCoordinator selector(stream, limits);
    CHECK_EQ(selector.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(selector.registerNode(node(2, NodeKind::ClientRoot, stream,
                                       RouteKind::RootFallback, 0x0200007Fu,
                                       47002u)), NodeRegistrationResult::Accepted);
    CHECK_EQ(selector.registerNode(node(11, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0B00007Fu,
                                       47011u)), NodeRegistrationResult::Accepted);
    CHECK_EQ(selector.registerNode(node(15, NodeKind::TrafficRelay, stream,
                                       RouteKind::TrafficRelay, 0x0F00007Fu,
                                       47015u)), NodeRegistrationResult::Accepted);
    NodeAdvertisement uploadExhausted = node(16, NodeKind::SpectatorRelay,
                                              stream, RouteKind::Direct,
                                              0x1000007Fu, 47016u);
    uploadExhausted.uploadCapacity = 10;
    uploadExhausted.uploadInUse = 10;
    CHECK_EQ(selector.registerNode(uploadExhausted),
             NodeRegistrationResult::Accepted);
    NodeAdvertisement noVerifiedProgress = node(17, NodeKind::SpectatorRelay,
                                                 stream, RouteKind::Direct,
                                                 0x1100007Fu, 47017u);
    noVerifiedProgress.lastProgressMs = 0;
    CHECK_EQ(selector.registerNode(noVerifiedProgress),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(selector.attachRootChild(1, 11), TopologyAttachResult::Attached);
    ParentSelectionRequest request{};
    request.childId = 11;
    request.requiredCursor = 8;
    request.minimumSourceGeneration = 1;
    request.minimumRouteGeneration = 1;
    request.maxOffers = 10;
    const std::vector<ParentSelection> offers =
        selector.selectStandbyParents(request);
    CHECK_TRUE(!offers.empty());
    CHECK_EQ(offers.size(), limits.maxStandbyParents);
    CHECK_EQ(offers[0].parentId, 2u);
    CHECK_TRUE(offers[0].branchDiverse);
    CHECK_EQ(offers.back().parentId, 15u);
    CHECK_TRUE(offers.back().trafficRelayFallback);
    for (const ParentSelection& selection : offers) {
        CHECK_TRUE(selection.parentId != 16u);
        CHECK_TRUE(selection.parentId != 17u);
    }
    ParentLease rejectedOffer{};
    CHECK_TRUE(!selector.makeUnsignedParentOffer(
        11, 16, 8, 5000, 99, childBindingFor(selector, 11, 8, 99),
        rejectedOffer));
    CHECK_TRUE(!selector.makeUnsignedParentOffer(
        11, 17, 8, 5000, 100, childBindingFor(selector, 11, 8, 100),
        rejectedOffer));
}

ParentLease signedOffer(ResilientTopologyCoordinator& topology,
                        ParentLeaseRegistry& registry,
                        NodeId childId, NodeId parentId,
                        uint32_t requiredCursor, uint64_t nonce) {
    ParentLease lease{};
    CHECK_TRUE(topology.makeUnsignedParentOffer(childId, parentId,
                                                 requiredCursor, 5000,
                                                 nonce,
                                                 childBindingFor(
                                                     topology, childId,
                                                     requiredCursor, childId),
                                                 lease));
    CHECK_TRUE(registry.issue(lease));
    return lease;
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

void refreshControlChecksum(std::vector<uint8_t>& bytes) {
    if (bytes.size() < wire::kResilientControlHeaderBytes) return;
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        bytes[12u + shift / 8u] = 0u;
    const uint32_t checksum = qreplay::crc32c(bytes.data(), bytes.size());
    for (unsigned shift = 0u; shift < 32u; shift += 8u) {
        bytes[12u + shift / 8u] =
            static_cast<uint8_t>(checksum >> shift);
    }
}

void relayChildRetirementIsExactAndVersionBound() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xC411D0u);
    constexpr uint64_t kAuthorityGeneration = 0xA117000000000001ull;
    ResilientTopologyCoordinator topology(stream);
    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(2, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0200007Fu,
                                       47202u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(3, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0300007Fu,
                                       47203u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.attachRootChild(1, 2), TopologyAttachResult::Attached);

    ParentLeaseChildBinding relayBinding =
        childBindingFor(topology, 2, 8u, 0x7111u);
    relayBinding.authorityGeneration = kAuthorityGeneration;
    ParentLease relayLease{};
    CHECK_TRUE(topology.makeUnsignedParentOffer(
        2, 1, 8u, 2000u, 0x7111u, relayBinding, relayLease));

    ParentLeaseChildBinding childBinding =
        childBindingFor(topology, 3, 8u, 0x7112u);
    childBinding.authorityGeneration = kAuthorityGeneration;
    ParentLease childLease{};
    CHECK_TRUE(topology.makeUnsignedParentOffer(
        3, 2, 8u, 1500u, 0x7112u, childBinding, childLease));
    ParentLeaseRegistry registry(leaseKey());
    CHECK_TRUE(registry.issue(relayLease));
    CHECK_TRUE(registry.issue(childLease));

    wire::RelayChildRetirement retirement{};
    retirement.stream = stream;
    retirement.authorityGeneration = kAuthorityGeneration;
    retirement.rootNodeId = 1u;
    retirement.relayNodeId = 2u;
    retirement.childNodeId = 3u;
    retirement.relayLease = leaseBindingFor(relayLease);
    retirement.bindingRequestNonce = 0xA5516EEDu;
    retirement.childLease = leaseBindingFor(childLease);
    retirement.reason = wire::RelayChildRetirementReason::ChildDetached;

    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::RelayChildRetirement;
    datagram.relayChildRetirement = retirement;
    std::vector<uint8_t> encoded;
    const bool encodedRetirement =
        wire::encodeControlDatagram(datagram, encoded);
    CHECK_TRUE(encodedRetirement);
    CHECK_TRUE(encoded.size() > wire::kResilientControlHeaderBytes);
    if (!encodedRetirement
        || encoded.size() <= wire::kResilientControlHeaderBytes) {
        return;
    }
    CHECK_EQ(encoded[4u], static_cast<uint8_t>(wire::kResilientControlVersion));
    CHECK_EQ(encoded[5u], 0u);

    wire::ControlDatagram decoded{};
    CHECK_EQ(wire::decodeControlDatagram(encoded.data(), encoded.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type, wire::ControlMessageType::RelayChildRetirement);
    const wire::RelayChildRetirement& roundTrip =
        decoded.relayChildRetirement;
    CHECK_EQ(roundTrip.stream, retirement.stream);
    CHECK_EQ(roundTrip.authorityGeneration, retirement.authorityGeneration);
    CHECK_EQ(roundTrip.rootNodeId, retirement.rootNodeId);
    CHECK_EQ(roundTrip.relayNodeId, retirement.relayNodeId);
    CHECK_EQ(roundTrip.childNodeId, retirement.childNodeId);
    CHECK_EQ(roundTrip.relayLease.childId, retirement.relayLease.childId);
    CHECK_EQ(roundTrip.relayLease.parentId, retirement.relayLease.parentId);
    CHECK_EQ(roundTrip.relayLease.parentSourceGeneration,
             retirement.relayLease.parentSourceGeneration);
    CHECK_EQ(roundTrip.relayLease.routeGeneration,
             retirement.relayLease.routeGeneration);
    CHECK_EQ(roundTrip.relayLease.expiryMs, retirement.relayLease.expiryMs);
    CHECK_EQ(roundTrip.relayLease.replayNonce,
             retirement.relayLease.replayNonce);
    CHECK_EQ(roundTrip.relayLease.macLow, retirement.relayLease.macLow);
    CHECK_EQ(roundTrip.relayLease.macHigh, retirement.relayLease.macHigh);
    CHECK_TRUE(roundTrip.relayLease.childBinding
               == retirement.relayLease.childBinding);
    CHECK_EQ(roundTrip.bindingRequestNonce, retirement.bindingRequestNonce);
    CHECK_EQ(roundTrip.childLease.childId, retirement.childLease.childId);
    CHECK_EQ(roundTrip.childLease.parentId, retirement.childLease.parentId);
    CHECK_EQ(roundTrip.childLease.parentSourceGeneration,
             retirement.childLease.parentSourceGeneration);
    CHECK_EQ(roundTrip.childLease.routeGeneration,
             retirement.childLease.routeGeneration);
    CHECK_EQ(roundTrip.childLease.expiryMs, retirement.childLease.expiryMs);
    CHECK_EQ(roundTrip.childLease.replayNonce,
             retirement.childLease.replayNonce);
    CHECK_EQ(roundTrip.childLease.macLow, retirement.childLease.macLow);
    CHECK_EQ(roundTrip.childLease.macHigh, retirement.childLease.macHigh);
    CHECK_TRUE(roundTrip.childLease.childBinding
               == retirement.childLease.childBinding);
    CHECK_EQ(roundTrip.reason, retirement.reason);

    wire::ControlDatagram invalidDatagram = datagram;
    invalidDatagram.relayChildRetirement.childLease.childId = 4u;
    std::vector<uint8_t> rejected{0xA5u};
    CHECK_TRUE(!wire::encodeControlDatagram(invalidDatagram, rejected));
    CHECK_TRUE(rejected.empty());
    invalidDatagram = datagram;
    ++invalidDatagram.relayChildRetirement
          .childLease.childBinding.authorityGeneration;
    rejected.assign(1u, 0xA5u);
    CHECK_TRUE(!wire::encodeControlDatagram(invalidDatagram, rejected));
    CHECK_TRUE(rejected.empty());
    invalidDatagram = datagram;
    invalidDatagram.relayChildRetirement.childLease.macLow = 0u;
    invalidDatagram.relayChildRetirement.childLease.macHigh = 0u;
    rejected.assign(1u, 0xA5u);
    CHECK_TRUE(!wire::encodeControlDatagram(invalidDatagram, rejected));
    CHECK_TRUE(rejected.empty());

    std::vector<uint8_t> malformed = encoded;
    malformed.back() = 0u;
    refreshControlChecksum(malformed);
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(malformed.data(), malformed.size(),
                                         decoded),
             wire::DecodeResult::MalformedPayload);

    std::vector<uint8_t> oldVersion = encoded;
    oldVersion[4u] = 6u;
    oldVersion[5u] = 0u;
    refreshControlChecksum(oldVersion);
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(oldVersion.data(), oldVersion.size(),
                                         decoded),
             wire::DecodeResult::UnsupportedVersion);
}

void relayChildActivationIsPairedSignedAndVersionBound() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xAC71A77Eu);
    constexpr uint64_t kAuthorityGeneration = 0xA117000000000009ull;
    ResilientTopologyCoordinator topology(stream);
    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(2, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0200007Fu,
                                       47402u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(3, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0300007Fu,
                                       47403u)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.attachRootChild(1, 2), TopologyAttachResult::Attached);

    ParentLeaseChildBinding relayBinding =
        childBindingFor(topology, 2, 8u, 0x7211u);
    relayBinding.authorityGeneration = kAuthorityGeneration;
    ParentLease relayLease{};
    CHECK_TRUE(topology.makeUnsignedParentOffer(
        2, 1, 8u, 2000u, 0x7211u, relayBinding, relayLease));

    ParentLeaseChildBinding childBinding =
        childBindingFor(topology, 3, 8u, 0x7212u);
    childBinding.authorityGeneration = kAuthorityGeneration;
    ParentLease childLease{};
    CHECK_TRUE(topology.makeUnsignedParentOffer(
        3, 2, 8u, 1500u, 0x7212u, childBinding, childLease));
    const RouteDescriptor relayDataRoute{
        RouteKind::Direct, 0x0200007Fu, 48402u,
        relayBinding.childEndpointBindingGeneration, true};
    childLease.routeGeneration = relayDataRoute.generation;
    childLease.routes[0] = relayDataRoute;
    childLease.cacheRange = {8u, 8u};
    childLease.latestVerifiedCursor = 8u;
    ParentLeaseRegistry registry(leaseKey());
    CHECK_TRUE(registry.issue(relayLease));
    CHECK_TRUE(registry.issue(childLease));

    wire::RelayChildActivation activation{};
    activation.stream = stream;
    activation.authorityGeneration = kAuthorityGeneration;
    activation.rootNodeId = 1u;
    activation.relayNodeId = 2u;
    activation.childNodeId = 3u;
    activation.relayLease = leaseBindingFor(relayLease);
    activation.relayBindingRequestNonce = 0xAC71000000000001ull;
    activation.relayDataRoute = relayDataRoute;
    activation.relayControlRoute = {
        RouteKind::Direct, 0x0200007Fu, 49402u,
        relayBinding.childEndpointBindingGeneration, true};
    activation.childLease = childLease;
    activation.childDataRoute = childBinding.childQspRoute;
    activation.childControlRoute = {
        RouteKind::Direct, 0x0300007Fu, 49403u,
        childBinding.childEndpointBindingGeneration, true};
    activation.relayCacheRange = {8u, 8u};
    activation.commonCursor = childBinding.childRequestedCursor;
    activation.activationNonce = 0xAC71000000000002ull;
    activation.activationDeadlineMs = childLease.expiryMs;
    activation.hostSignature.fill(0x5Au);

    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::RelayChildActivation;
    datagram.relayChildActivation = activation;
    std::vector<uint8_t> encoded;
    CHECK_TRUE(wire::encodeControlDatagram(datagram, encoded));
    CHECK_TRUE(encoded.size() > wire::kResilientControlHeaderBytes);
    if (encoded.size() <= wire::kResilientControlHeaderBytes) return;

    wire::ControlDatagram decoded{};
    CHECK_EQ(wire::decodeControlDatagram(encoded.data(), encoded.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type, wire::ControlMessageType::RelayChildActivation);
    const wire::RelayChildActivation& roundTrip = decoded.relayChildActivation;
    CHECK_EQ(roundTrip.relayNodeId, activation.relayNodeId);
    CHECK_EQ(roundTrip.childNodeId, activation.childNodeId);
    CHECK_EQ(roundTrip.relayDataRoute.port, activation.relayDataRoute.port);
    CHECK_EQ(roundTrip.relayControlRoute.port,
             activation.relayControlRoute.port);
    CHECK_EQ(roundTrip.childDataRoute.port, activation.childDataRoute.port);
    CHECK_EQ(roundTrip.childControlRoute.port,
             activation.childControlRoute.port);
    CHECK_EQ(roundTrip.commonCursor.nextRecord,
             activation.commonCursor.nextRecord);
    CHECK_EQ(roundTrip.commonCursor.nextEvent,
             activation.commonCursor.nextEvent);
    CHECK_EQ(roundTrip.commonCursor.digest, activation.commonCursor.digest);
    CHECK_EQ(roundTrip.activationNonce, activation.activationNonce);
    CHECK_EQ(roundTrip.activationDeadlineMs, activation.activationDeadlineMs);
    CHECK_EQ(roundTrip.hostSignature, activation.hostSignature);

    std::vector<uint8_t> transcript;
    CHECK_TRUE(wire::encodeRelayChildActivationAttestationInput(
        activation, transcript));
    wire::RelayChildActivation changedNonce = activation;
    ++changedNonce.activationNonce;
    std::vector<uint8_t> changedTranscript;
    CHECK_TRUE(wire::encodeRelayChildActivationAttestationInput(
        changedNonce, changedTranscript));
    CHECK_TRUE(transcript != changedTranscript);

    wire::RelayChildActivationCommit commit{};
    commit.activation = activation;
    commit.hostSignature.fill(0xA6u);
    CHECK_TRUE(wire::validRelayChildActivationCommit(commit));
    wire::ControlDatagram commitDatagram{};
    commitDatagram.type = wire::ControlMessageType::RelayChildActivationCommit;
    commitDatagram.relayChildActivationCommit = commit;
    std::vector<uint8_t> commitBytes;
    CHECK_TRUE(wire::encodeControlDatagram(commitDatagram, commitBytes));
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(commitBytes.data(),
                                         commitBytes.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type, wire::ControlMessageType::RelayChildActivationCommit);
    CHECK_EQ(decoded.relayChildActivationCommit.activation.activationNonce,
             activation.activationNonce);
    CHECK_EQ(decoded.relayChildActivationCommit.activation.hostSignature,
             activation.hostSignature);
    CHECK_EQ(decoded.relayChildActivationCommit.hostSignature,
             commit.hostSignature);

    std::vector<uint8_t> commitTranscript;
    CHECK_TRUE(wire::encodeRelayChildActivationCommitAttestationInput(
        commit, commitTranscript));
    wire::RelayChildActivationCommit changedCommit = commit;
    ++changedCommit.activation.activationNonce;
    std::vector<uint8_t> changedCommitTranscript;
    CHECK_TRUE(wire::encodeRelayChildActivationCommitAttestationInput(
        changedCommit, changedCommitTranscript));
    CHECK_TRUE(commitTranscript != changedCommitTranscript);

    wire::RelayChildActivationCommit unsignedCommit = commit;
    unsignedCommit.hostSignature.fill(0u);
    CHECK_TRUE(!wire::validRelayChildActivationCommit(unsignedCommit));
    wire::ControlDatagram rejectedCommit = commitDatagram;
    rejectedCommit.relayChildActivationCommit = unsignedCommit;
    std::vector<uint8_t> rejectedCommitBytes{0xA5u};
    CHECK_TRUE(!wire::encodeControlDatagram(rejectedCommit,
                                             rejectedCommitBytes));
    CHECK_TRUE(rejectedCommitBytes.empty());

    wire::RelayChildActivationReceipt childReceipt{};
    CHECK_TRUE(wire::makeRelayChildActivationReceipt(
        activation, wire::RelayChildActivationReceiptRole::Child,
        childReceipt));
    wire::ControlDatagram receiptDatagram{};
    receiptDatagram.type = wire::ControlMessageType::RelayChildActivationReceipt;
    receiptDatagram.relayChildActivationReceipt = childReceipt;
    std::vector<uint8_t> receiptBytes;
    CHECK_TRUE(wire::encodeControlDatagram(receiptDatagram, receiptBytes));
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(receiptBytes.data(),
                                         receiptBytes.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type,
             wire::ControlMessageType::RelayChildActivationReceipt);
    CHECK_EQ(decoded.relayChildActivationReceipt.role,
             wire::RelayChildActivationReceiptRole::Child);
    CHECK_EQ(decoded.relayChildActivationReceipt.activationNonce,
             activation.activationNonce);

    wire::RelayChildActivationRetirement retirement{};
    retirement.stream = activation.stream;
    retirement.authorityGeneration = activation.authorityGeneration;
    retirement.rootNodeId = activation.rootNodeId;
    retirement.relayNodeId = activation.relayNodeId;
    retirement.childNodeId = activation.childNodeId;
    retirement.relayLease = activation.relayLease;
    retirement.childLease = leaseBindingFor(activation.childLease);
    retirement.relayBindingRequestNonce = activation.relayBindingRequestNonce;
    retirement.relayDataRoute = activation.relayDataRoute;
    retirement.relayControlRoute = activation.relayControlRoute;
    retirement.childDataRoute = activation.childDataRoute;
    retirement.childControlRoute = activation.childControlRoute;
    retirement.relayCacheRange = activation.relayCacheRange;
    retirement.commonCursor = activation.commonCursor;
    retirement.activationNonce = activation.activationNonce;
    retirement.activationDeadlineMs = activation.activationDeadlineMs;
    retirement.reason = wire::RelayChildRetirementReason::ChildBindingReplaced;
    wire::ControlDatagram retirementDatagram{};
    retirementDatagram.type =
        wire::ControlMessageType::RelayChildActivationRetirement;
    retirementDatagram.relayChildActivationRetirement = retirement;
    std::vector<uint8_t> retirementBytes;
    CHECK_TRUE(wire::encodeControlDatagram(retirementDatagram,
                                           retirementBytes));
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(retirementBytes.data(),
                                         retirementBytes.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type,
             wire::ControlMessageType::RelayChildActivationRetirement);
    CHECK_EQ(decoded.relayChildActivationRetirement.activationNonce,
             activation.activationNonce);
    CHECK_EQ(decoded.relayChildActivationRetirement.reason,
             wire::RelayChildRetirementReason::ChildBindingReplaced);

    std::vector<uint8_t> malformedRetirement = retirementBytes;
    malformedRetirement.back() = 0u;
    refreshControlChecksum(malformedRetirement);
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(malformedRetirement.data(),
                                         malformedRetirement.size(), decoded),
             wire::DecodeResult::MalformedPayload);

    wire::ControlDatagram badRoute = datagram;
    badRoute.relayChildActivation.childDataRoute =
        badRoute.relayChildActivation.relayDataRoute;
    std::vector<uint8_t> rejected{0xA5u};
    CHECK_TRUE(!wire::encodeControlDatagram(badRoute, rejected));
    CHECK_TRUE(rejected.empty());

    std::vector<uint8_t> malformedReceipt = receiptBytes;
    malformedReceipt.back() = 0u;
    refreshControlChecksum(malformedReceipt);
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(malformedReceipt.data(),
                                         malformedReceipt.size(), decoded),
             wire::DecodeResult::MalformedPayload);

    std::vector<uint8_t> oldVersion = encoded;
    oldVersion[4u] = 8u;
    oldVersion[5u] = 0u;
    refreshControlChecksum(oldVersion);
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(oldVersion.data(), oldVersion.size(),
                                         decoded),
             wire::DecodeResult::UnsupportedVersion);
}

void clientDataRootAssertionIsBoundAndVersioned() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xD00DCAFEu);
    wire::ClientDataRootAssertion assertion{};
    assertion.stream = stream;
    assertion.gameStartFingerprint = stream.streamRootIdentity;
    assertion.authorityGeneration = 0xD00D000000000001ull;
    assertion.clientRootNodeId = 42u;
    assertion.qspRouteGeneration = 11u;
    assertion.qsrBindingGeneration = 7u;
    assertion.sourceGeneration = 19u;
    assertion.dataRoute = {RouteKind::Direct, 0x2A00007Fu, 48142u, 7u, true};
    assertion.qsrRoute = {RouteKind::Direct, 0x2A00007Fu, 49142u, 7u, true};
    assertion.cacheRange = {32u, 64u};
    assertion.cursor = {64u, 9u, 0xD00DCAFE11223344ull};
    assertion.childCapacity = 3u;

    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::ClientDataRootAssertion;
    datagram.clientDataRootAssertion = assertion;
    std::vector<uint8_t> encoded;
    CHECK_TRUE(wire::encodeControlDatagram(datagram, encoded));
    CHECK_TRUE(encoded.size() > wire::kResilientControlHeaderBytes);
    if (encoded.size() <= wire::kResilientControlHeaderBytes) return;

    wire::ControlDatagram decoded{};
    CHECK_EQ(wire::decodeControlDatagram(encoded.data(), encoded.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type, wire::ControlMessageType::ClientDataRootAssertion);
    const wire::ClientDataRootAssertion& roundTrip =
        decoded.clientDataRootAssertion;
    CHECK_EQ(roundTrip.stream, assertion.stream);
    CHECK_EQ(roundTrip.gameStartFingerprint, assertion.gameStartFingerprint);
    CHECK_EQ(roundTrip.authorityGeneration, assertion.authorityGeneration);
    CHECK_EQ(roundTrip.clientRootNodeId, assertion.clientRootNodeId);
    CHECK_EQ(roundTrip.qspRouteGeneration, assertion.qspRouteGeneration);
    CHECK_EQ(roundTrip.qsrBindingGeneration, assertion.qsrBindingGeneration);
    CHECK_EQ(roundTrip.sourceGeneration, assertion.sourceGeneration);
    CHECK_EQ(roundTrip.dataRoute.port, assertion.dataRoute.port);
    CHECK_EQ(roundTrip.qsrRoute.port, assertion.qsrRoute.port);
    CHECK_EQ(roundTrip.cacheRange.firstCursor, assertion.cacheRange.firstCursor);
    CHECK_EQ(roundTrip.cacheRange.nextCursor, assertion.cacheRange.nextCursor);
    CHECK_EQ(roundTrip.cursor.nextRecord, assertion.cursor.nextRecord);
    CHECK_EQ(roundTrip.cursor.nextEvent, assertion.cursor.nextEvent);
    CHECK_EQ(roundTrip.cursor.digest, assertion.cursor.digest);
    CHECK_EQ(roundTrip.childCapacity, assertion.childCapacity);

    wire::ControlDatagram rejected = datagram;
    rejected.clientDataRootAssertion.childCapacity = 0u;
    std::vector<uint8_t> rejectedBytes{0xA5u};
    CHECK_TRUE(!wire::encodeControlDatagram(rejected, rejectedBytes));
    CHECK_TRUE(rejectedBytes.empty());
    rejected = datagram;
    rejected.clientDataRootAssertion.qsrRoute =
        rejected.clientDataRootAssertion.dataRoute;
    rejectedBytes.assign(1u, 0xA5u);
    CHECK_TRUE(!wire::encodeControlDatagram(rejected, rejectedBytes));
    CHECK_TRUE(rejectedBytes.empty());
    rejected = datagram;
    ++rejected.clientDataRootAssertion.dataRoute.generation;
    rejectedBytes.assign(1u, 0xA5u);
    CHECK_TRUE(!wire::encodeControlDatagram(rejected, rejectedBytes));
    CHECK_TRUE(rejectedBytes.empty());

    std::vector<uint8_t> oldVersion = encoded;
    oldVersion[4u] = 9u;
    oldVersion[5u] = 0u;
    refreshControlChecksum(oldVersion);
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(oldVersion.data(), oldVersion.size(),
                                         decoded),
             wire::DecodeResult::UnsupportedVersion);
}

void clientPlayerRootLifecycleIsSeparateAndVersioned() {
    using qnetplay::config_provenance::Stage;
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xC11E1700u);
    const auto provenance = qnetplay::config_provenance::makeCommittedWire(
        Stage::Default, Stage::LocallySelected, Stage::CasterAdvertised,
        Stage::PeerAdvertised);
    constexpr uint64_t kAuthorityGeneration = 0xC11E170000000001ull;
    constexpr NodeId kHostRoot = 1u;
    constexpr NodeId kClientRoot = 41u;
    constexpr NodeId kTarget = 42u;

    wire::ClientPlayerRootBinding binding{};
    binding.stream = stream;
    binding.gameStartFingerprint = stream.streamRootIdentity;
    binding.clientRootNodeId = kClientRoot;
    binding.remotePlayerRole = wire::ClientPlayerRootRole::RemotePlayerTwo;
    binding.configurationProvenance = provenance;
    binding.runtimeReceipt.processId = 991u;
    binding.runtimeReceipt.publisherGeneration = 7u;
    binding.runtimeReceipt.publisherNonce = 0xC11E170000000002ull;
    binding.runtimeReceipt.playerSlot = static_cast<uint8_t>(
        wire::ClientPlayerRootRole::RemotePlayerTwo);
    binding.runtimeReceipt.runtimeStage = Stage::RuntimeApplied;
    binding.runtimeReceipt.delay = 3u;
    binding.runtimeReceipt.rollback = 7u;
    binding.runtimeReceipt.remoteDelay = 4u;
    binding.runtimeReceipt.remoteRollback = 6u;
    binding.runtimeReceipt.provenance = provenance;
    binding.sourceGeneration = 13u;
    binding.qspRouteGeneration = 17u;
    binding.qsrBindingGeneration = 19u;
    binding.dataRoute = {RouteKind::Punched, 0x2900007Fu, 48141u, 17u, true};
    binding.qsrRoute = {RouteKind::Direct, 0x2900007Fu, 49141u, 19u, true};
    binding.cacheRange = {32u, 64u};
    binding.cursor = {64u, 9u, 0xC11E170011223344ull};
    binding.childCapacity = 2u;
    binding.expiresMs = 2000u;

    wire::ClientPlayerRootGrant grant{};
    grant.authorityGeneration = kAuthorityGeneration;
    grant.hostRootNodeId = kHostRoot;
    grant.grantNonce = 0xC11E170000000003ull;
    grant.binding = binding;
    grant.hostSignature.fill(0x31u);
    wire::ControlDatagram grantDatagram{};
    grantDatagram.type = wire::ControlMessageType::ClientPlayerRootGrant;
    grantDatagram.clientPlayerRootGrant = grant;
    std::vector<uint8_t> grantBytes;
    CHECK_TRUE(wire::encodeControlDatagram(grantDatagram, grantBytes));
    wire::ControlDatagram decoded{};
    CHECK_EQ(wire::decodeControlDatagram(grantBytes.data(), grantBytes.size(),
                                         decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type, wire::ControlMessageType::ClientPlayerRootGrant);
    CHECK_EQ(decoded.clientPlayerRootGrant.binding.clientRootNodeId,
             kClientRoot);
    CHECK_EQ(decoded.clientPlayerRootGrant.binding.runtimeReceipt.playerSlot,
             static_cast<uint8_t>(wire::ClientPlayerRootRole::RemotePlayerTwo));

    std::vector<uint8_t> grantTranscript;
    CHECK_TRUE(wire::encodeClientPlayerRootGrantAttestationInput(
        grant, grantTranscript));
    wire::ClientPlayerRootGrant changedGrant = grant;
    ++changedGrant.binding.qsrRoute.generation;
    std::vector<uint8_t> changedGrantTranscript;
    CHECK_TRUE(!wire::encodeClientPlayerRootGrantAttestationInput(
        changedGrant, changedGrantTranscript));

    wire::ClientPlayerRootAssertion assertion{};
    assertion.authorityGeneration = kAuthorityGeneration;
    assertion.hostRootNodeId = kHostRoot;
    assertion.grantNonce = grant.grantNonce;
    assertion.binding = binding;
    wire::ControlDatagram assertionDatagram{};
    assertionDatagram.type = wire::ControlMessageType::ClientPlayerRootAssertion;
    assertionDatagram.clientPlayerRootAssertion = assertion;
    std::vector<uint8_t> assertionBytes;
    CHECK_TRUE(wire::encodeControlDatagram(assertionDatagram, assertionBytes));
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(assertionBytes.data(),
                                         assertionBytes.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type,
             wire::ControlMessageType::ClientPlayerRootAssertion);

    ParentLeaseChildBinding childBinding{};
    childBinding.authorityGeneration = kAuthorityGeneration;
    childBinding.childViewerId = 0xC11E170000000004ull;
    childBinding.childViewerGeneration = 2u;
    childBinding.childQspRoute = {RouteKind::Direct, 0x2A00007Fu, 48142u,
                                  23u, true};
    childBinding.childEndpointBindingGeneration = 23u;
    childBinding.childRequestedCursor = binding.cursor;
    ParentLease lease{};
    lease.stream = stream;
    lease.childId = kTarget;
    lease.parentId = kClientRoot;
    lease.childBinding = childBinding;
    lease.parentDepth = 0u;
    lease.parentSourceGeneration = binding.sourceGeneration;
    lease.routeGeneration = binding.dataRoute.generation;
    lease.routes[0] = binding.dataRoute;
    lease.routeCount = 1u;
    lease.cacheRange = binding.cacheRange;
    lease.latestVerifiedCursor = binding.cursor.nextRecord;
    lease.childCapacity = binding.childCapacity;
    lease.expiryMs = binding.expiresMs;
    lease.replayNonce = 0xC11E170000000005ull;
    lease.ancestry[0] = kClientRoot;
    lease.ancestryCount = 1u;
    ParentLeaseRegistry leaseSigner(leaseKey());
    CHECK_TRUE(leaseSigner.issue(lease));

    wire::ClientPlayerRootActivation activation{};
    activation.authorityGeneration = kAuthorityGeneration;
    activation.hostRootNodeId = kHostRoot;
    activation.binding = binding;
    activation.targetViewerNodeId = kTarget;
    activation.targetLease = lease;
    activation.targetDataRoute = childBinding.childQspRoute;
    activation.targetQsrRoute = {RouteKind::Direct, 0x2A00007Fu, 49142u,
                                 23u, true};
    activation.activationNonce = 0xC11E170000000006ull;
    activation.activationDeadlineMs = lease.expiryMs;
    activation.hostSignature.fill(0x42u);
    wire::ControlDatagram activationDatagram{};
    activationDatagram.type = wire::ControlMessageType::ClientPlayerRootActivation;
    activationDatagram.clientPlayerRootActivation = activation;
    std::vector<uint8_t> activationBytes;
    CHECK_TRUE(wire::encodeControlDatagram(activationDatagram,
                                           activationBytes));
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(activationBytes.data(),
                                         activationBytes.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type,
             wire::ControlMessageType::ClientPlayerRootActivation);
    CHECK_EQ(decoded.clientPlayerRootActivation.targetLease.parentId,
             kClientRoot);
    CHECK_EQ(decoded.clientPlayerRootActivation.targetLease.ancestry[0],
             kClientRoot);

    std::vector<uint8_t> activationTranscript;
    CHECK_TRUE(wire::encodeClientPlayerRootActivationAttestationInput(
        activation, activationTranscript));
    wire::ClientPlayerRootActivation changedActivation = activation;
    ++changedActivation.targetLease.replayNonce;
    std::vector<uint8_t> changedActivationTranscript;
    CHECK_TRUE(wire::encodeClientPlayerRootActivationAttestationInput(
        changedActivation, changedActivationTranscript));
    CHECK_TRUE(activationTranscript != changedActivationTranscript);

    wire::ClientPlayerRootActivationReceipt receipt{};
    CHECK_TRUE(wire::makeClientPlayerRootActivationReceipt(activation, receipt));
    wire::ControlDatagram receiptDatagram{};
    receiptDatagram.type =
        wire::ControlMessageType::ClientPlayerRootActivationReceipt;
    receiptDatagram.clientPlayerRootActivationReceipt = receipt;
    std::vector<uint8_t> receiptBytes;
    CHECK_TRUE(wire::encodeControlDatagram(receiptDatagram, receiptBytes));

    wire::ClientPlayerRootActivationCommit commit{};
    commit.activation = activation;
    commit.hostSignature.fill(0x52u);
    CHECK_TRUE(wire::validClientPlayerRootActivationCommit(commit));
    wire::ControlDatagram commitDatagram{};
    commitDatagram.type =
        wire::ControlMessageType::ClientPlayerRootActivationCommit;
    commitDatagram.clientPlayerRootActivationCommit = commit;
    std::vector<uint8_t> commitBytes;
    CHECK_TRUE(wire::encodeControlDatagram(commitDatagram, commitBytes));
    std::vector<uint8_t> commitTranscript;
    CHECK_TRUE(wire::encodeClientPlayerRootActivationCommitAttestationInput(
        commit, commitTranscript));

    wire::ClientPlayerRootActivationRetirement retirement{};
    retirement.activation = activation;
    retirement.reason =
        wire::ClientPlayerRootRetirementReason::TargetViewerDetached;
    retirement.hostSignature.fill(0x62u);
    CHECK_TRUE(wire::validClientPlayerRootActivationRetirement(retirement));
    wire::ControlDatagram retirementDatagram{};
    retirementDatagram.type =
        wire::ControlMessageType::ClientPlayerRootActivationRetirement;
    retirementDatagram.clientPlayerRootActivationRetirement = retirement;
    std::vector<uint8_t> retirementBytes;
    CHECK_TRUE(wire::encodeControlDatagram(retirementDatagram,
                                           retirementBytes));
    std::vector<uint8_t> retirementTranscript;
    CHECK_TRUE(wire::encodeClientPlayerRootActivationRetirementAttestationInput(
        retirement, retirementTranscript));

    wire::ControlDatagram rejected = grantDatagram;
    rejected.clientPlayerRootGrant.binding.runtimeReceipt.playerSlot = 1u;
    std::vector<uint8_t> rejectedBytes{0xA5u};
    CHECK_TRUE(!wire::encodeControlDatagram(rejected, rejectedBytes));
    CHECK_TRUE(rejectedBytes.empty());
    rejected = activationDatagram;
    rejected.clientPlayerRootActivation.targetQsrRoute =
        rejected.clientPlayerRootActivation.targetDataRoute;
    rejectedBytes.assign(1u, 0xA5u);
    CHECK_TRUE(!wire::encodeControlDatagram(rejected, rejectedBytes));
    CHECK_TRUE(rejectedBytes.empty());
    std::vector<uint8_t> oldVersion = activationBytes;
    oldVersion[4u] = 10u;
    oldVersion[5u] = 0u;
    refreshControlChecksum(oldVersion);
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(oldVersion.data(), oldVersion.size(),
                                         decoded),
             wire::DecodeResult::UnsupportedVersion);
}

void parentLeaseChildBindingIsMandatoryAcrossHmacAndQsr() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xB1D0u);
    ResilientTopologyCoordinator topology(stream);
    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(2, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Punched, 0x0200007Fu,
                                       47002u)),
             NodeRegistrationResult::Accepted);

    const ParentLeaseChildBinding childBinding =
        childBindingFor(topology, 2, 8, 0x5150u);
    ParentLease lease{};
    CHECK_TRUE(topology.makeUnsignedParentOffer(2, 1, 8, 1000, 0x5150u,
                                                 childBinding, lease));
    ParentLeaseRegistry registry(leaseKey());
    CHECK_TRUE(registry.issue(lease));

    ParentLeaseExpectation expected{};
    expected.stream = stream;
    expected.expectedChildId = 2;
    expected.expectedParentId = 1;
    expected.expectedChildBinding = childBinding;
    expected.nowMs = 100;
    expected.minimumSourceGeneration = 1;
    expected.minimumRouteGeneration = 1;
    expected.requiredCursor = 8;
    expected.maxActiveDepth = topology.limits().maxActiveDepth;
    expected.parentHasCapacity = true;
    expected.loopFree = true;
    CHECK_EQ(registry.verify(lease, expected), ParentLeaseVerdict::Accepted);

    ParentLease legacyOffer{};
    CHECK_TRUE(!topology.makeUnsignedParentOffer(2, 1, 8, 1000, 0x5151u,
                                                  legacyOffer));
    ParentLease absentBinding = lease;
    absentBinding.childBinding = {};
    CHECK_TRUE(!registry.issue(absentBinding));
    ParentLease unprovedQsp = lease;
    unprovedQsp.childBinding.childQspRoute.routeProved = false;
    CHECK_TRUE(!registry.issue(unprovedQsp));
    wire::ControlDatagram unprovedLeaseDatagram{};
    unprovedLeaseDatagram.type = wire::ControlMessageType::ParentLease;
    unprovedLeaseDatagram.parentLease = unprovedQsp;
    std::vector<uint8_t> rejectedBytes{0xA5u};
    CHECK_TRUE(!wire::encodeControlDatagram(unprovedLeaseDatagram,
                                            rejectedBytes));
    CHECK_TRUE(rejectedBytes.empty());
    ParentLeaseExpectation absentExpectation = expected;
    absentExpectation.expectedChildBinding = {};
    CHECK_EQ(registry.verify(lease, absentExpectation),
             ParentLeaseVerdict::ChildBindingMismatch);

    const auto requireAuthenticatedMismatch =
        [&registry, &expected, &lease](ParentLease candidate) {
            CHECK_EQ(registry.verify(candidate, expected),
                     ParentLeaseVerdict::BadMac);
            CHECK_TRUE(registry.issue(candidate));
            CHECK_TRUE(candidate.macLow != lease.macLow
                       || candidate.macHigh != lease.macHigh);
            CHECK_EQ(registry.verify(candidate, expected),
                     ParentLeaseVerdict::ChildBindingMismatch);
        };
    ParentLease changed = lease;
    ++changed.childBinding.authorityGeneration;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    ++changed.childBinding.childViewerId;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    ++changed.childBinding.childViewerGeneration;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    changed.childBinding.childQspRoute.kind = RouteKind::Direct;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    ++changed.childBinding.childQspRoute.ipv4BE;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    ++changed.childBinding.childQspRoute.port;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    ++changed.childBinding.childQspRoute.generation;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    ++changed.childBinding.childEndpointBindingGeneration;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    ++changed.childBinding.childRequestedCursor.nextRecord;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    ++changed.childBinding.childRequestedCursor.nextEvent;
    requireAuthenticatedMismatch(changed);
    changed = lease;
    ++changed.childBinding.childRequestedCursor.digest;
    requireAuthenticatedMismatch(changed);

    wire::ControlDatagram leaseDatagram{};
    leaseDatagram.type = wire::ControlMessageType::ParentLease;
    leaseDatagram.parentLease = lease;
    std::vector<uint8_t> encoded;
    CHECK_TRUE(wire::encodeControlDatagram(leaseDatagram, encoded));
    wire::ControlDatagram decoded{};
    CHECK_EQ(wire::decodeControlDatagram(encoded.data(), encoded.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type, wire::ControlMessageType::ParentLease);
    CHECK_TRUE(decoded.parentLease.childBinding == childBinding);

    wire::ControlDatagram continuityDatagram{};
    continuityDatagram.type = wire::ControlMessageType::ContinuityRequest;
    ContinuityRequest& request = continuityDatagram.continuityRequest;
    request.stream = stream;
    request.lease = leaseBindingFor(lease);
    request.gameId = 0xB1D01A6ull;
    request.expectedSourceGeneration = lease.parentSourceGeneration;
    request.parentGeneration = 1u;
    request.exactNextCursor = childBinding.childRequestedCursor.nextRecord;
    request.exactNextEvent = childBinding.childRequestedCursor.nextEvent;
    request.overlapCursor = request.exactNextCursor;
    request.overlapEvent = request.exactNextEvent;
    request.overlapDigest = childBinding.childRequestedCursor.digest;
    encoded.clear();
    CHECK_TRUE(wire::encodeControlDatagram(continuityDatagram, encoded));
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(encoded.data(), encoded.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type, wire::ControlMessageType::ContinuityRequest);
    CHECK_TRUE(decoded.continuityRequest.lease.childBinding == childBinding);

    wire::ControlDatagram commitDatagram{};
    commitDatagram.type = wire::ControlMessageType::ParentCommit;
    wire::ParentCommit& commit = commitDatagram.parentCommit;
    commit.stream = stream;
    commit.childId = lease.childId;
    commit.parentId = lease.parentId;
    commit.lease = leaseBindingFor(lease);
    commit.sourceGeneration = lease.parentSourceGeneration;
    commit.routeGeneration = lease.routeGeneration;
    commit.exactNextCursor = childBinding.childRequestedCursor.nextRecord;
    commit.exactNextEvent = childBinding.childRequestedCursor.nextEvent;
    commit.overlapCursor = commit.exactNextCursor;
    commit.overlapEvent = commit.exactNextEvent;
    commit.overlapDigest = childBinding.childRequestedCursor.digest;
    encoded.clear();
    CHECK_TRUE(wire::encodeControlDatagram(commitDatagram, encoded));
    decoded = {};
    CHECK_EQ(wire::decodeControlDatagram(encoded.data(), encoded.size(), decoded),
             wire::DecodeResult::Ok);
    CHECK_EQ(decoded.type, wire::ControlMessageType::ParentCommit);
    CHECK_TRUE(decoded.parentCommit.lease.childBinding == childBinding);
}

void subtreeFailoverPreservesChildrenUntilExactContinuityCommits() {
    const StreamCompatibilityIdentity stream =
        syntheticStreamIdentityForTests(0xFA110u);
    constexpr uint64_t kGameId = 0xA100u;
    constexpr uint64_t kInitialDigest = 0xA1B2C3D4E5F60718ull;
    ResilientLimits limits{};
    limits.maxStandbyParents = 4;
    limits.maxConcurrentRouteRaces = 4;
    ResilientTopologyCoordinator topology(stream, limits);
    CHECK_EQ(topology.registerNode(node(1, NodeKind::HostRoot, stream)),
             NodeRegistrationResult::Accepted);
    NodeAdvertisement clientRoot = node(2, NodeKind::ClientRoot, stream,
                                        RouteKind::RootFallback, 0x0200007Fu,
                                        48002u);
    clientRoot.sourceGeneration = 2;
    CHECK_EQ(topology.registerNode(clientRoot), NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(3, NodeKind::TrafficRelay, stream,
                                       RouteKind::TrafficRelay, 0x0300007Fu,
                                       48003u)), NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(4, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0400007Fu,
                                       48004u)), NodeRegistrationResult::Accepted);
    NodeAdvertisement standbyParent = node(5, NodeKind::SpectatorRelay, stream,
                                           RouteKind::Punched, 0x0500007Fu,
                                           48005u);
    standbyParent.sourceGeneration = 2;
    CHECK_EQ(topology.registerNode(standbyParent), NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(10, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0A00007Fu,
                                       48010u)), NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(11, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0B00007Fu,
                                       48011u)), NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.registerNode(node(12, NodeKind::SpectatorRelay, stream,
                                       RouteKind::Direct, 0x0C00007Fu,
                                       48012u)), NodeRegistrationResult::Accepted);
    CHECK_EQ(topology.attachRootChild(1, 10), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachChild(10, 11), TopologyAttachResult::Attached);
    CHECK_EQ(topology.attachChild(11, 12), TopologyAttachResult::Attached);

    ParentLeaseRegistry registry(leaseKey());
    ResilientRelayNode relay(10, stream, kGameId, kInitialDigest,
                             topology, registry, limits);
    CHECK_TRUE(relay.start(1));
    CHECK_TRUE(relay.setInitialParent(1, 1, 1));
    CHECK_TRUE(relay.attachChild(11, 0));
    uint64_t digest = kInitialDigest;
    for (uint32_t cursor = 0; cursor < 4; ++cursor) {
        RelayRecordEnvelope record =
            confirmedRecord(stream, kGameId, 1, cursor, digest);
        digest = record.record.rollingDigest;
        CHECK_EQ(relay.cache().append(record), CacheAppendResult::Accepted);
    }

    const ParentLease clientOffer = signedOffer(topology, registry, 10, 2, 4, 101);
    const ParentLease trafficOffer = signedOffer(topology, registry, 10, 3, 4, 102);
    const ParentLease maliciousOffer = signedOffer(topology, registry, 10, 4, 4, 103);
    const ParentLease standbyOffer = signedOffer(topology, registry, 10, 5, 4, 104);
    CHECK_TRUE(relay.noteUpstreamFailure(UpstreamFailureReason::TransportClosed,
                                         100));
    CHECK_EQ(relay.state(), RelayRecoveryState::Suspect);
    CHECK_EQ(relay.childCount(), 1u);
    const ParentLeaseChildBinding relayBinding =
        childBindingFor(topology, 10, 4, 10);
    CHECK_EQ(relay.beginRecovery({clientOffer, trafficOffer, maliciousOffer,
                                  standbyOffer}, 2, 110, relayBinding), 4u);
    CHECK_EQ(relay.state(), RelayRecoveryState::RacingStandbys);
    CHECK_EQ(relay.childCount(), 1u);

    ContinuityRequest request{};
    CHECK_TRUE(relay.continuityRequest(request));
    CHECK_EQ(request.exactNextCursor, 4u);
    CHECK_EQ(request.overlapCursor, 2u);
    ContinuityRequest boundRequest{};
    CHECK_TRUE(relay.continuityRequest(2, boundRequest));
    CHECK_EQ(boundRequest.lease.childId, clientOffer.childId);
    CHECK_EQ(boundRequest.lease.parentId, clientOffer.parentId);
    CHECK_EQ(boundRequest.lease.parentSourceGeneration,
             clientOffer.parentSourceGeneration);
    CHECK_EQ(boundRequest.lease.routeGeneration, clientOffer.routeGeneration);
    CHECK_EQ(boundRequest.lease.expiryMs, clientOffer.expiryMs);
    CHECK_EQ(boundRequest.lease.replayNonce, clientOffer.replayNonce);
    CHECK_EQ(boundRequest.lease.macLow, clientOffer.macLow);
    CHECK_EQ(boundRequest.lease.macHigh, clientOffer.macHigh);
    CHECK_TRUE(boundRequest.lease.childBinding == relayBinding);
    CHECK_EQ(boundRequest.expectedSourceGeneration,
             clientOffer.parentSourceGeneration);
    CHECK_TRUE(!relay.continuityRequest(999, boundRequest));
    CHECK_EQ(boundRequest.lease.childId, kInvalidNodeId);
    ContinuityProof valid{};
    valid.stream = stream;
    valid.lease = leaseBindingFor(clientOffer);
    valid.gameId = kGameId;
    valid.sourceGeneration = clientOffer.parentSourceGeneration;
    valid.parentGeneration = relay.parentGeneration();
    valid.cacheFirstCursor = 0;
    valid.cacheNextCursor = 12;
    valid.overlapCursor = request.overlapCursor;
    valid.overlapDigest = request.overlapDigest;
    valid.available = true;

    ContinuityProof generationMismatch = valid;
    generationMismatch.lease = leaseBindingFor(trafficOffer);
    generationMismatch.sourceGeneration =
        trafficOffer.parentSourceGeneration;
    generationMismatch.parentGeneration = relay.parentGeneration() + 1u;
    CHECK_TRUE(!relay.submitContinuityProof(3, generationMismatch));

    ContinuityProof parentMismatch = valid;
    parentMismatch.lease = leaseBindingFor(trafficOffer);
    parentMismatch.sourceGeneration = trafficOffer.parentSourceGeneration;
    parentMismatch.lease.parentId = clientOffer.parentId;
    CHECK_TRUE(!relay.submitContinuityProof(3, parentMismatch));
    ContinuityProof nonceMismatch = valid;
    nonceMismatch.lease = leaseBindingFor(maliciousOffer);
    nonceMismatch.sourceGeneration = maliciousOffer.parentSourceGeneration;
    ++nonceMismatch.lease.replayNonce;
    CHECK_TRUE(!relay.submitContinuityProof(4, nonceMismatch));
    ContinuityProof macMismatch = valid;
    macMismatch.lease = leaseBindingFor(standbyOffer);
    macMismatch.sourceGeneration = standbyOffer.parentSourceGeneration;
    ++macMismatch.lease.macLow;
    CHECK_TRUE(!relay.submitContinuityProof(5, macMismatch));
    CHECK_EQ(relay.quarantinedAttemptCount(), 3u);
    CHECK_TRUE(relay.submitContinuityProof(2, valid));
    CHECK_EQ(relay.state(), RelayRecoveryState::VerifyingContinuity);
    CHECK_TRUE(relay.commitVerifiedParent(2, 120));
    CHECK_EQ(relay.state(), RelayRecoveryState::CommittedToNewParent);
    CHECK_EQ(relay.activeParent(), 2u);
    CHECK_EQ(relay.parentGeneration(), 2u);
    CHECK_EQ(relay.cache().sourceGeneration(), 2u);
    CHECK_EQ(relay.cancelledAttemptCount(), 0u);
    CHECK_EQ(relay.childCount(), 1u);
    CHECK_EQ(topology.find(10)->activeParent, 2u);
    CHECK_EQ(topology.find(11)->activeParent, 10u);
    CHECK_EQ(topology.find(12)->activeParent, 11u);
    CHECK_EQ(topology.find(11)->depth, 2u);
    CHECK_EQ(topology.find(12)->depth, 3u);
    relay.noteVerifiedProgress();
    CHECK_EQ(relay.state(), RelayRecoveryState::Stable);
    RelayRecordEnvelope staleSource = confirmedRecord(
        stream, kGameId, 1, 4, relay.cache().latestDigest());
    CHECK_EQ(relay.cache().append(staleSource),
             CacheAppendResult::WrongSourceGeneration);
    RelayRecordEnvelope freshSource = confirmedRecord(
        stream, kGameId, 2, 4, relay.cache().latestDigest());
    CHECK_EQ(relay.cache().append(freshSource), CacheAppendResult::Accepted);

    CHECK_EQ(topology.validateStandbyLease(registry, standbyOffer, 200, 4,
                                           relayBinding),
             ParentLeaseVerdict::Accepted);

    CHECK_TRUE(relay.noteUpstreamFailure(UpstreamFailureReason::NoVerifiedProgress,
                                         300));
    CHECK_EQ(relay.beginRecovery({}, 2, 310), 0u);
    CHECK_EQ(relay.state(), RelayRecoveryState::DegradedBufferOnly);
    CHECK_EQ(relay.childCount(), 1u);
    CHECK_TRUE(!relay.childFallbackPermitted());
    CHECK_TRUE(relay.recoveryFailureAnnounced());
    relay.markCacheExhausted();
    CHECK_EQ(relay.state(), RelayRecoveryState::RecoveryFailed);
    CHECK_TRUE(relay.childFallbackPermitted());

    CHECK_TRUE(relay.beginDrain(1000, {2, 3, 4, 5}, 9));
    CHECK_EQ(relay.state(), RelayRecoveryState::Draining);
    CHECK_TRUE(topology.find(10)->advertisement.draining);
    CHECK_TRUE(!relay.attachChild(99, 0));
    DrainNotice drain{};
    CHECK_TRUE(relay.drainNotice(drain));
    CHECK_EQ(drain.shutdownDeadlineMs, 1000u);
    CHECK_EQ(drain.lastAvailableCursor, 5u);
    CHECK_EQ(drain.sourceGeneration, 2u);
    CHECK_EQ(drain.activeChildIds.size(), 1u);
    CHECK_EQ(drain.recommendedReplacementParents.size(), 4u);
    CHECK_TRUE(relay.retireChild(11));
    CHECK_EQ(relay.state(), RelayRecoveryState::Closed);

    ResilientRelayNode timedDrain(5, stream, kGameId, kInitialDigest,
                                  topology, registry, limits);
    CHECK_TRUE(timedDrain.start(1));
    CHECK_TRUE(timedDrain.attachChild(98, 0));
    CHECK_TRUE(timedDrain.beginDrain(2000, {2}, 1));
    timedDrain.tick(1999);
    CHECK_EQ(timedDrain.state(), RelayRecoveryState::Draining);
    timedDrain.tick(2000);
    CHECK_EQ(timedDrain.state(), RelayRecoveryState::Closed);
}

}

int main() {
    identityIsExactAcrossAllFields();
    hostAndClientRootsFanOutIndependently();
    nodeIdentitySurvivesEndpointRebinding();
    topologyRejectsWrongIdentityAndBoundsRootChildren();
    cacheForwardsOnlyVerifiedExactStreamRecords();
    cacheLimitsRequireJointRecordAndEventFootprint();
    cacheBoundsMemoryAndResyncsOnlyLaggingChildren();
    cacheTracksVerifiedSetEventsOnTheSharedChain();
    cacheEvictsEventRunwayOnlyWithItsRecordBoundary();
    cacheRetiresBoundaryBeforeRecordAfterEventFillsRunway();
    relayOwnsDifferentUpstreamAndDownstreamSocketsExactlyOnce();
    parentLeaseIsAuthenticatedBoundAndSingleUse();
    relayChildRetirementIsExactAndVersionBound();
    relayChildActivationIsPairedSignedAndVersionBound();
    clientDataRootAssertionIsBoundAndVersioned();
    clientPlayerRootLifecycleIsSeparateAndVersioned();
    parentLeaseChildBindingIsMandatoryAcrossHmacAndQsr();
    topologyEnforcesLoopsDepthCapacityDrainAndDiverseStandbys();
    subtreeFailoverPreservesChildrenUntilExactContinuityCommits();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d resilient spectator E1 check(s) failed\n",
                     g_failures);
        return 1;
    }
    std::puts("resilient spectator core: PASS");
    return 0;
}
