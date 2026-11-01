#include "NodeRuntimeTestSeams.h"

#include "../../../netplay/WireProtocol.h"
#include "../../../spectator/SpectatorArchive.h"
#include "../../../spectator/SpectatorSession.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace runtime = qclient::spectator::resilient;
namespace model = qnetplay::spectator::resilient;
namespace compat = qnetplay::compat;
using namespace qnetplay::spectator;

static_assert(!std::is_default_constructible_v<
                  runtime::NodeRuntimeChildIngress>,
              "raw callers must not fabricate child ingress provenance");
static_assert(!std::is_constructible_v<runtime::QtaVerifiedParentLease,
                                       model::ParentLease,
                                       std::shared_ptr<
                                           runtime::QtaAuthorityContext>>,
              "QTA parent claims require the issuer boundary");
static_assert(!std::is_default_constructible_v<
                  runtime::QtaVerifiedChildRetirement>,
              "child retirement must be minted only after QTA admission");
static_assert(!std::is_constructible_v<
                  runtime::QtaVerifiedChildRetirement,
                  model::NodeId, uint64_t, model::ParentLeaseChildBinding,
                  std::shared_ptr<runtime::QtaAuthorityContext>>,
              "raw callers must not fabricate a child retirement capability");

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

constexpr uint64_t kGameId = 0x5153504E4F444531ull;
constexpr uint64_t kSimulationContract = 0x514F48393953494Dull;
constexpr model::NodeId kRootNode = 0x1001u;
constexpr model::NodeId kIntermediateRelayNode = 0x1801u;
constexpr model::NodeId kRelayNode = 0x2002u;
constexpr model::NodeId kChildNode = 0x3003u;
constexpr model::NodeId kGrandchildNode = 0x4004u;
constexpr EndpointId kUpstreamRelayEndpoint = 0xA001u;
constexpr EndpointId kChildEndpoint = 0xB002u;
constexpr uint32_t kFirstFrame = 7200u;
constexpr uint64_t kAuthorityGeneration = 0x415554484F524954ull;
constexpr uint64_t kChildBindingGeneration = 0x42494E44494E4731ull;

SessionIdentity qspIdentity() {
    return SessionIdentity{
        0x1020304050607080ull,
        0x0000000000000007ull,
        SessionCookie{
            0x1122334455667788ull,
            0x8877665544332211ull,
        },
    };
}

compat::CompatibilityIdentityWire compatibilityIdentity() {
    compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion = compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    CHECK_TRUE(compat::setProfileId(identity, "qoh99-se1126"));
    for (size_t index = 0u; index < compat::kCompatibilitySha256Bytes;
         ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(0x20u + index);
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
    identity.stateSchemaId = 0x535441544531u;
    identity.modSimulationId = 0x4D4F44494431u;
    identity.timingProfileId = 0x54494D494E4731u;
    CHECK_TRUE(compat::isValidCompatibilityIdentity(identity));
    return identity;
}

GameDescriptor gameDescriptor() {
    GameDescriptor descriptor{};
    descriptor.gameId = kGameId;
    descriptor.simulationContract = kSimulationContract;
    descriptor.compatibilityIdentity = compatibilityIdentity();
    descriptor.initialStateHash = 0xA55A000000000001ull;
    descriptor.firstTimelineFrame = kFirstFrame;
    descriptor.playerOneDelay = 2u;
    descriptor.playerTwoDelay = 3u;
    descriptor.rollbackWindow = 8u;
    descriptor.metadata = {0x51u, 0x53u, 0x50u, 0x2Du, 0x4Eu, 0x4Fu, 0x44u};
    descriptor.initialDigest = expectedInitialDigest(descriptor);
    return descriptor;
}

ConfirmedRecord recordFor(const uint32_t index) {
    ConfirmedRecord record{};
    record.record = index;
    record.timelineFrame = kFirstFrame + index;
    record.playerOneInput = static_cast<uint16_t>(index & 0x000Fu);
    record.playerTwoInput = static_cast<uint16_t>((index + 3u) & 0x000Fu);
    record.stateHash = 0xD15EA5E000000000ull
        ^ (static_cast<uint64_t>(index) * 0x101u);
    return record;
}

SetEvent stageEventAt(const SpectatorArchive& archive,
                      const uint32_t recordCursor) {
    SetEvent event{};
    event.eventIndex = 0u;
    event.recordCursor = recordCursor;
    event.timelineFrame = kFirstFrame + recordCursor;
    event.kind = SetEventKind::StageSelected;
    event.valueC = 4u;
    CHECK_TRUE(archive.chainDigestAt(kGameId, recordCursor, 0u,
                                     event.recordDigest));
    return event;
}

SetEvent networkSampleEventAt(const SpectatorArchive& archive,
                              const uint32_t eventIndex,
                              const uint32_t recordCursor) {
    SetEvent event{};
    event.eventIndex = eventIndex;
    event.recordCursor = recordCursor;
    event.timelineFrame = kFirstFrame + recordCursor;
    event.kind = SetEventKind::NetworkSample;
    event.valueA = 1000u + eventIndex;
    event.valueB = eventIndex;
    CHECK_TRUE(archive.chainDigestAt(kGameId, recordCursor, eventIndex,
                                     event.recordDigest));
    return event;
}

model::ResilientLimits smallLimits() {
    model::ResilientLimits limits{};
    limits.routeRaceRecordBudget = 1u;
    limits.continuityOverlapRecords = 1u;
    limits.retransmissionMarginRecords = 1u;
    limits.bootstrapMarginRecords = 1u;
    limits.maxCachedRecords = limits.requiredCacheRecords();
    limits.maxCachedEvents = kMaxSetEventsPerBatch;
    limits.maxCachedBytes = 64u * 1024u;
    CHECK_TRUE(limits.valid());
    return limits;
}

SpectatorGrantKey leaseKey() {
    SpectatorGrantKey key{};
    for (size_t index = 0u; index < key.size(); ++index)
        key[index] = static_cast<uint8_t>(0xA0u + index);
    return key;
}

model::VerifiedChainCursor cacheCursor(
    const model::VerifiedRelayCache& cache, const uint32_t nextRecord,
    const uint32_t nextEvent) {
    model::VerifiedChainCursor out{};
    out.nextRecord = nextRecord;
    out.nextEvent = nextEvent;
    CHECK_TRUE(cache.digestAt(nextRecord, nextEvent, out.digest));
    return out;
}

runtime::NodeRuntimeChildIngress childIngress(
    const model::RouteDescriptor& route,
    const uint64_t endpointBindingGeneration,
    const EndpointId endpoint) {
    return qclient::spectator::resilient::test::
        NodeRuntimeChildIngressTestFactory::make(
            route, endpointBindingGeneration, endpoint);
}

model::ParentLease signedChildLease(
    const runtime::ResilientSpectatorNodeRuntimeConfig& config,
    const model::NodeAdvertisement& child,
    const model::VerifiedRelayCache& cache, const uint64_t nowMs,
    const uint64_t nonce, const ViewerId childViewerId,
    const uint32_t childViewerGeneration,
    const model::RouteDescriptor& childQspRoute,
    const uint64_t childEndpointBindingGeneration,
    const model::VerifiedChainCursor& childRequestedCursor,
    const SpectatorGrantKey& signerKey,
    const uint64_t hostLeaseExpiryMs = 0u) {
    model::ParentLease lease{};
    lease.stream = config.stream;
    lease.childId = child.nodeId;
    lease.parentId = config.localAdvertisement.nodeId;
    lease.childBinding.authorityGeneration = config.authorityGeneration;
    lease.childBinding.childViewerId = childViewerId;
    lease.childBinding.childViewerGeneration = childViewerGeneration;
    lease.childBinding.childQspRoute = childQspRoute;
    lease.childBinding.childEndpointBindingGeneration =
        childEndpointBindingGeneration;
    lease.childBinding.childRequestedCursor = childRequestedCursor;
    lease.parentDepth = 1u;
    lease.parentSourceGeneration = cache.sourceGeneration();
    lease.routeGeneration = config.localAdvertisement.routeGeneration;
    lease.routeCount = 1u;
    lease.routes[0] = config.localAdvertisement.route;
    lease.cacheRange = cache.range();
    lease.latestVerifiedCursor = cache.range().nextCursor;
    lease.childCapacity = static_cast<uint32_t>(config.limits.maxChildrenPerRelay);
    lease.expiryMs = hostLeaseExpiryMs == 0u ? nowMs + 5000u
                                               : hostLeaseExpiryMs;
    lease.replayNonce = nonce;
    lease.ancestry[0] = kRootNode;
    lease.ancestry[1] = config.localAdvertisement.nodeId;
    lease.ancestryCount = 2u;
    model::ParentLeaseRegistry signer(signerKey);
    CHECK_TRUE(signer.issue(lease));
    return lease;
}

model::NodeAdvertisement advertisement(
    const model::NodeId nodeId, const model::NodeKind kind,
    const model::StreamCompatibilityIdentity& stream,
    const uint32_t ipv4BE, const uint16_t port,
    const uint64_t sourceGeneration, const uint64_t routeGeneration) {
    model::NodeAdvertisement node{};
    node.nodeId = nodeId;
    node.kind = kind;
    node.stream = stream;
    node.route.kind = model::RouteKind::Direct;
    node.route.ipv4BE = ipv4BE;
    node.route.port = port;
    node.route.generation = routeGeneration;
    node.route.routeProved = true;
    node.capabilityModelRevision = model::kResilientCapabilityModelRevision;
    node.capabilities = model::kCapabilityEphemeralEndpoint
        | model::kCapabilityMultiPeerAutopunch
        | model::kCapabilityMultiViewerRoot
        | model::kCapabilityRelayCapableSpectator;
    node.childCapacity = kind == model::NodeKind::SpectatorRelay ? 8u : 32u;
    node.uploadCapacity = 32u;
    node.rttMs = 11u;
    node.cacheFirstCursor = 0u;
    node.cacheNextCursor = 0u;
    node.sourceGeneration = sourceGeneration;
    node.routeGeneration = routeGeneration;
    node.lastProgressMs = 1u;
    node.reachable = true;
    return node;
}

runtime::ResilientSpectatorNodeRuntimeConfig nodeConfig(
    const GameDescriptor& descriptor) {
    runtime::ResilientSpectatorNodeRuntimeConfig config{};
    config.qspIdentity = qspIdentity();
    config.simulationContract = descriptor.simulationContract;
    config.compatibilityIdentity = descriptor.compatibilityIdentity;
    const GameStartPayload start = canonicalGameStart(descriptor);
    CHECK_TRUE(model::makeStreamCompatibilityIdentity(
        descriptor.compatibilityIdentity, config.qspIdentity.sessionId,
        config.qspIdentity.epoch, gameStartFingerprint(start), config.stream));
    config.gameId = descriptor.gameId;
    config.initialDigest = descriptor.initialDigest;
    config.sourceGeneration = 5u;
    config.authorityGeneration = kAuthorityGeneration;
    config.initialParentGeneration = 5u;
    config.initialRouteGeneration = 9u;
    config.limits = smallLimits();
    config.parentLeaseKey = leaseKey();
    config.localAdvertisement = advertisement(
        kRelayNode, model::NodeKind::SpectatorRelay, config.stream,
        0x0100007Fu, 43002u, config.sourceGeneration,
        config.initialRouteGeneration);
    config.initialParentAdvertisement = advertisement(
        kRootNode, model::NodeKind::HostRoot, config.stream,
        0x0100007Fu, 43001u, config.sourceGeneration,
        config.initialRouteGeneration);

    config.downstreamArchive.maxGames = 1u;
    config.downstreamArchive.maxRetainedRecords = 32u;
    config.downstreamArchive.segmentRecords = 4u;
    config.downstreamArchive.maxRetainedBytes = 64u * 1024u;
    config.downstreamSource.recordsPerBatch = 1u;
    config.downstreamSource.perViewerPacketBudget = 2u;
    config.downstreamSource.globalPacketBudget = 4u;
    config.upstreamViewer.secureRandom = 0xABCD1234ABCD1234ull;
    return config;
}

runtime::ResilientSpectatorNodeRuntimeConfig canonicalNodeConfig(
    const GameDescriptor& descriptor) {
    runtime::ResilientSpectatorNodeRuntimeConfig config = nodeConfig(descriptor);
    config.ingressMode = runtime::NodeRuntimeIngressMode::CanonicalHostAttested;
    config.productionRelay = true;
    config.localAdvertisement.controlRoute = config.localAdvertisement.route;
    config.localAdvertisement.controlRoute.port = 43012u;
    config.parentLeaseKey = {};
    return config;
}

runtime::ResilientSpectatorNodeRuntimeConfig nestedCanonicalNodeConfig(
    const GameDescriptor& descriptor) {
    runtime::ResilientSpectatorNodeRuntimeConfig config =
        canonicalNodeConfig(descriptor);
    config.initialParentAdvertisement = advertisement(
        kIntermediateRelayNode, model::NodeKind::SpectatorRelay, config.stream,
        0x0100007Fu, 43001u, config.sourceGeneration,
        config.initialRouteGeneration);
    config.initialParentDepth = 1u;
    config.initialParentAncestry[0] = kRootNode;
    config.initialParentAncestryCount = 1u;
    return config;
}

std::shared_ptr<runtime::QtaAuthorityContext> qtaAuthority(
    const runtime::ResilientSpectatorNodeRuntimeConfig& config,
    const uint64_t localExpiryDeadlineMs,
    const size_t maximumRedemptions = 16u) {
    return qclient::spectator::resilient::test::
        QtaAuthorityContextTestFactory::make(
            config.authorityGeneration, localExpiryDeadlineMs,
            maximumRedemptions);
}

runtime::QtaVerifiedCanonicalIngress canonicalIngress(
    std::shared_ptr<runtime::QtaAuthorityContext> authority,
    const runtime::ResilientSpectatorNodeRuntimeConfig& config,
    const SpectatorViewer& viewer) {
    return qclient::spectator::resilient::test::
        QtaVerifiedCanonicalIngressTestFactory::make(
            std::move(authority), config, viewer);
}

runtime::QtaVerifiedParentLease qtaParentLease(
    model::ParentLease lease,
    std::shared_ptr<runtime::QtaAuthorityContext> authority) {
    return qclient::spectator::resilient::test::
        QtaVerifiedParentLeaseTestFactory::make(
            std::move(lease), std::move(authority));
}

model::NodeAdvertisement canonicalChildAdvertisement(
    const model::NodeId nodeId,
    const runtime::ResilientSpectatorNodeRuntimeConfig& config) {
    model::NodeAdvertisement child = advertisement(
        nodeId, model::NodeKind::SpectatorRelay, config.stream,
        0x0100007Fu, static_cast<uint16_t>(43003u + (nodeId & 0x00FFu)),
        config.sourceGeneration, config.initialRouteGeneration);
    child.activeParent = kRelayNode;
    child.depth = 2u;
    child.ancestry[0] = kRootNode;
    child.ancestry[1] = kRelayNode;
    child.ancestryCount = 2u;
    return child;
}

SourceConfig upstreamSourceConfig(const GameDescriptor& descriptor,
                                  const size_t recordsPerBatch = 1u) {
    SourceConfig config{};
    config.identity = qspIdentity();
    config.simulationContract = descriptor.simulationContract;
    config.compatibilityIdentity = descriptor.compatibilityIdentity;
    config.recordsPerBatch = recordsPerBatch;
    config.perViewerPacketBudget = 2u;
    config.globalPacketBudget = 4u;
    return config;
}

bool acceptableSourceIngress(const ViewerDatagram& packet,
                             const IngressResult result) {
    return result == IngressResult::Accepted
        || result == IngressResult::ReplayedSequence
        || (packet.type == MessageType::InitAck
            && result == IngressResult::UnexpectedMessage);
}

struct QspRouteFixture {
    SpectatorArchive upstreamArchive{};
    SpectatorSource upstream;
    runtime::ResilientSpectatorNodeRuntime relay{};
    SpectatorViewer child;
    uint64_t nowMs = 1u;
    std::optional<runtime::NodeRuntimeChildIngress> childRoute;
    bool childEnabled = false;

    explicit QspRouteFixture(const GameDescriptor& descriptor,
                             const size_t upstreamRecordsPerBatch = 1u)
        : upstream(upstreamSourceConfig(descriptor, upstreamRecordsPerBatch),
                   upstreamArchive),
          child(ViewerConfig{qspIdentity(), descriptor.simulationContract,
                             descriptor.compatibilityIdentity,
                             0xCCDD1234CCDD1234ull}) {}

    bool start(const runtime::ResilientSpectatorNodeRuntimeConfig& config) {
        return upstreamArchive.isConfigured() && upstream.isConfigured()
            && child.isConfigured() && relay.start(config);
    }

    bool pumpOnce() {
        bool allAccepted = true;

        for (const ViewerDatagram& packet : relay.pollUpstreamOutbound(nowMs, 4u)) {
            if (!acceptableSourceIngress(
                    packet, upstream.onDatagram(kUpstreamRelayEndpoint,
                                                 packet.bytes.data(),
                                                 packet.bytes.size(), nowMs))) {
                allAccepted = false;
            }
        }
        for (const SourceDatagram& packet : upstream.pollOutbound(nowMs, 4u)) {
            if (relay.onUpstreamDatagram(packet.bytes.data(), packet.bytes.size(),
                                         nowMs) != runtime::NodeRuntimeResult::Accepted) {
                allAccepted = false;
            }
        }

        if (childEnabled) {
            if (!childRoute.has_value()) return false;
            for (const ViewerDatagram& packet : child.pollOutbound(nowMs, 4u)) {
                if (relay.onChildDatagram(*childRoute, packet.bytes.data(),
                                          packet.bytes.size(), nowMs)
                    != runtime::NodeRuntimeResult::Accepted) {
                    allAccepted = false;
                }
            }
            for (const runtime::NodeRuntimeDownstreamDatagram& packet
                 : relay.pollChildOutbound(nowMs, 4u)) {
                const IngressResult ingress = child.onDatagram(
                    packet.bytes.data(), packet.bytes.size(), nowMs);
                if (packet.destination != kChildEndpoint
                    || (ingress != IngressResult::Accepted
                        && ingress != IngressResult::ReplayedSequence)) {
                    allAccepted = false;
                }
            }
        }
        ++nowMs;
        return allAccepted;
    }

    bool pumpUntil(const uint32_t upstreamRecords,
                   const uint32_t childRecords,
                   const uint32_t childEvents,
                   const size_t iterations = 200u) {
        for (size_t iteration = 0u; iteration < iterations; ++iteration) {
            if (!pumpOnce()) return false;
            const SpectatorViewer* upstreamViewer = relay.upstreamViewer();
            if (upstreamViewer != nullptr
                && upstreamViewer->receivedNextRecord() >= upstreamRecords
                && (!childEnabled
                    || (child.receivedNextRecord() >= childRecords
                        && child.receivedNextEvent() >= childEvents))) {
                return true;
            }
        }
        return false;
    }
};

struct CanonicalViewerFixture {
    SpectatorArchive authorityArchive{};
    SpectatorSource authority;
    SpectatorViewer viewer;
    uint64_t nowMs = 1u;

    explicit CanonicalViewerFixture(const GameDescriptor& descriptor)
        : authority(upstreamSourceConfig(descriptor), authorityArchive),
          viewer(ViewerConfig{qspIdentity(), descriptor.simulationContract,
                              descriptor.compatibilityIdentity,
                              0xD00DCAFE00112233ull}) {}

    bool begin(const GameDescriptor& descriptor) {
        return authorityArchive.isConfigured() && authority.isConfigured()
            && viewer.isConfigured()
            && authorityArchive.beginGame(descriptor) == ArchiveResult::Ok
            && viewer.begin(descriptor.gameId);
    }

    bool pumpOnce() {
        bool accepted = true;
        for (const ViewerDatagram& packet : viewer.pollOutbound(nowMs, 4u)) {
            if (!acceptableSourceIngress(
                    packet, authority.onDatagram(kUpstreamRelayEndpoint,
                                                  packet.bytes.data(),
                                                  packet.bytes.size(), nowMs))) {
                accepted = false;
            }
        }
        for (const SourceDatagram& packet : authority.pollOutbound(nowMs, 4u)) {
            const IngressResult result = viewer.onDatagram(
                packet.bytes.data(), packet.bytes.size(), nowMs);
            if (result != IngressResult::Accepted
                && result != IngressResult::ReplayedSequence) {
                accepted = false;
            }
        }
        ++nowMs;
        return accepted;
    }

    bool pumpUntil(const uint32_t records, const uint32_t events,
                   const size_t iterations = 200u) {
        for (size_t iteration = 0u; iteration < iterations; ++iteration) {
            if (!pumpOnce()) return false;
            GameStartPayload start{};
            if (viewer.gameStart(start)
                && viewer.receivedNextRecord() >= records
                && viewer.receivedNextEvent() >= events) {
                return true;
            }
        }
        return false;
    }
};

bool prepareSignedProjection(
    QspRouteFixture& fixture,
    const runtime::ResilientSpectatorNodeRuntimeConfig& config,
    const GameDescriptor& descriptor, const uint32_t records = 1u,
    const bool seal = false) {
    if (!fixture.start(config)
        || fixture.upstreamArchive.beginGame(descriptor) != ArchiveResult::Ok) {
        return false;
    }
    for (uint32_t index = 0u; index < records; ++index) {
        if (fixture.upstreamArchive.appendConfirmed(
                kGameId, recordFor(index), nullptr) != ArchiveResult::Ok) {
            return false;
        }
    }
    if (seal && fixture.upstreamArchive.sealGame(
                    kGameId, TerminalReason::MatchEnded,
                    0xF00DCAFE12345678ull) != ArchiveResult::Ok) {
        return false;
    }
    return fixture.pumpUntil(records, 0u, 0u);
}

bool encodeChildHello(const runtime::ResilientSpectatorNodeRuntimeConfig& config,
                      const ViewerId viewerId, const uint32_t generation,
                      const model::VerifiedChainCursor& cursor,
                      const uint32_t sequence,
                      std::vector<uint8_t>& datagram) {
    HelloPayload hello{};
    hello.simulationContract = config.simulationContract;
    hello.compatibilityIdentity = config.compatibilityIdentity;
    hello.requestedGameId = config.gameId;
    hello.viewerId = viewerId;
    hello.generation = generation;
    hello.viewerNonce = viewerGenerationNonce(viewerId, generation);
    hello.resumeRecord = cursor.nextRecord;
    hello.resumeEvent = cursor.nextEvent;
    hello.resumeDigest = cursor.digest;
    std::vector<uint8_t> payload;
    return encodePayload(hello, payload)
        && encodeDatagram(MessageType::Hello, config.qspIdentity, sequence,
                          payload, datagram);
}

bool resignLease(model::ParentLease& lease, const SpectatorGrantKey& key) {
    return model::ParentLeaseRegistry(key).issue(lease);
}

void testQspProjectionEventChainBoundedCacheAndChildAck() {
    const GameDescriptor descriptor = gameDescriptor();
    QspRouteFixture fixture(descriptor);
    const runtime::ResilientSpectatorNodeRuntimeConfig config =
        nodeConfig(descriptor);
    CHECK_TRUE(fixture.start(config));

    CHECK_EQ(fixture.upstreamArchive.beginGame(descriptor), ArchiveResult::Ok);
    CHECK_EQ(fixture.upstreamArchive.appendConfirmed(
                 kGameId, recordFor(0u), nullptr), ArchiveResult::Ok);
    CHECK_EQ(fixture.upstreamArchive.appendConfirmed(
                 kGameId, recordFor(1u), nullptr), ArchiveResult::Ok);
    const SetEvent stage = stageEventAt(fixture.upstreamArchive, 2u);
    CHECK_EQ(fixture.upstreamArchive.appendEvent(kGameId, stage, nullptr),
             ArchiveResult::Ok);
    CHECK_EQ(fixture.upstreamArchive.appendConfirmed(
                 kGameId, recordFor(2u), nullptr), ArchiveResult::Ok);

    CHECK_TRUE(fixture.pumpUntil(3u, 0u, 0u));
    const model::ResilientRelayNode* relayNode = fixture.relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    CHECK_EQ(relayNode->cache().range().nextCursor, 3u);
    CHECK_EQ(relayNode->cache().eventRange().nextEvent, 1u);

    model::NodeAdvertisement child = advertisement(
        kChildNode, model::NodeKind::SpectatorRelay, config.stream,
        0x0100007Fu, 43003u, config.sourceGeneration,
        config.initialRouteGeneration);
    child.activeParent = kRelayNode;
    child.depth = 2u;
    child.ancestry[0] = kRootNode;
    child.ancestry[1] = kRelayNode;
    child.ancestryCount = 2u;
    CHECK_TRUE(fixture.child.begin(kGameId));
    const model::VerifiedChainCursor initialChildCursor = cacheCursor(
        relayNode->cache(), 0u, 0u);
    fixture.childRoute = childIngress(child.route, kChildBindingGeneration,
                                      kChildEndpoint);
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 child, model::ParentLease{}, fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 child, signedChildLease(config, child, relayNode->cache(),
                                          fixture.nowMs, 0xC001u,
                                          fixture.child.viewerId(),
                                          fixture.child.generation(), child.route,
                                          kChildBindingGeneration,
                                          initialChildCursor,
                                          config.parentLeaseKey),
                 fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);
    fixture.childEnabled = true;
    CHECK_TRUE(fixture.pumpUntil(3u, 3u, 1u));
    CHECK_TRUE(fixture.pumpOnce());

    CHECK_EQ(fixture.child.setTimeline().eventCount(), size_t{1});
    CHECK_EQ(fixture.child.receivedNextRecord(), 3u);
    CHECK_EQ(fixture.child.receivedNextEvent(), 1u);
    const model::CacheChildStatus* initialStatus =
        fixture.relay.relayNode()->cache().child(kChildNode);
    CHECK_TRUE(initialStatus != nullptr);
    if (initialStatus != nullptr) {
        CHECK_EQ(initialStatus->acknowledgedCursor, 3u);
        CHECK_EQ(initialStatus->acknowledgedEvent, 1u);
        CHECK_TRUE(!initialStatus->controlledResyncRequired);
    }

    uint64_t acknowledgedDigest = 0u;
    CHECK_TRUE(fixture.relay.relayNode()->cache().digestAt(
        3u, 1u, acknowledgedDigest));
    AckPayload badAck{};
    badAck.gameId = kGameId;
    badAck.viewerId = fixture.child.viewerId();
    badAck.generation = fixture.child.generation();
    badAck.nextRecord = 3u;
    badAck.nextEvent = 1u;
    badAck.digest = acknowledgedDigest;
    std::vector<uint8_t> badAckPayload;
    std::vector<uint8_t> badAckDatagram;
    CHECK_TRUE(encodePayload(badAck, badAckPayload));
    CHECK_TRUE(encodeDatagram(MessageType::Ack, config.qspIdentity, 1001u,
                              badAckPayload, badAckDatagram));
    const runtime::NodeRuntimeChildIngress wrongEndpoint = childIngress(
        child.route, kChildBindingGeneration, kChildEndpoint + 1u);
    CHECK_EQ(fixture.relay.onChildDatagram(
                 wrongEndpoint, badAckDatagram.data(), badAckDatagram.size(),
                 fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);
    badAck.digest ^= 0x1ull;
    CHECK_TRUE(encodePayload(badAck, badAckPayload));
    CHECK_TRUE(encodeDatagram(MessageType::Ack, config.qspIdentity, 1002u,
                              badAckPayload, badAckDatagram));
    CHECK_EQ(fixture.relay.onChildDatagram(
                 *fixture.childRoute, badAckDatagram.data(),
                 badAckDatagram.size(), fixture.nowMs),
             runtime::NodeRuntimeResult::CacheRejected);

    constexpr model::NodeId kEndpointConflictChild = 0x3005u;
    model::NodeAdvertisement endpointConflict = advertisement(
        kEndpointConflictChild, model::NodeKind::SpectatorRelay, config.stream,
        0x0100007Fu, 43003u, config.sourceGeneration,
        config.initialRouteGeneration);
    endpointConflict.activeParent = kRelayNode;
    endpointConflict.depth = 2u;
    endpointConflict.ancestry[0] = kRootNode;
    endpointConflict.ancestry[1] = kRelayNode;
    endpointConflict.ancestryCount = 2u;
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 endpointConflict,
                 signedChildLease(config, endpointConflict, relayNode->cache(),
                                  fixture.nowMs, 0xC003u,
                                  0xC003D00DC003D00Dull, 1u, child.route,
                                  kChildBindingGeneration,
                                  initialChildCursor, config.parentLeaseKey),
                 fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    for (uint32_t index = 3u; index < 9u; ++index) {
        CHECK_EQ(fixture.upstreamArchive.appendConfirmed(
                     kGameId, recordFor(index), nullptr), ArchiveResult::Ok);
    }
    CHECK_TRUE(fixture.pumpUntil(9u, 9u, 1u, 500u));
    CHECK_TRUE(fixture.pumpOnce());

    relayNode = fixture.relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;
    const model::VerifiedRelayCache& cache = relayNode->cache();
    CHECK_EQ(cache.range().nextCursor, 9u);
    CHECK_EQ(cache.recordCount(), config.limits.maxCachedRecords);
    CHECK_EQ(cache.range().firstCursor,
             9u - static_cast<uint32_t>(config.limits.maxCachedRecords));
    CHECK_EQ(cache.eventRange().nextEvent, 1u);
    const model::CacheChildStatus* finalStatus = cache.child(kChildNode);
    CHECK_TRUE(finalStatus != nullptr);
    if (finalStatus != nullptr) {
        CHECK_EQ(finalStatus->acknowledgedCursor, 9u);
        CHECK_EQ(finalStatus->acknowledgedEvent, 1u);
        CHECK_TRUE(!finalStatus->controlledResyncRequired);
    }
}

void testQspProjectionRetainsCompleteInboundRecordPacket() {
    const GameDescriptor descriptor = gameDescriptor();
    QspRouteFixture fixture(descriptor, kMaxRecordsPerBatch);
    const runtime::ResilientSpectatorNodeRuntimeConfig config =
        nodeConfig(descriptor);
    CHECK_TRUE(fixture.start(config));
    CHECK_EQ(fixture.upstreamArchive.beginGame(descriptor), ArchiveResult::Ok);
    for (uint32_t record = 0u;
         record < static_cast<uint32_t>(kMaxRecordsPerBatch); ++record) {
        CHECK_EQ(fixture.upstreamArchive.appendConfirmed(
                     kGameId, recordFor(record), nullptr), ArchiveResult::Ok);
    }

    CHECK_TRUE(fixture.pumpUntil(
        static_cast<uint32_t>(kMaxRecordsPerBatch), 0u, 0u, 500u));
    const model::ResilientRelayNode* relayNode = fixture.relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;
    const model::VerifiedRelayCache& cache = relayNode->cache();
    CHECK_EQ(cache.range().nextCursor,
             static_cast<uint32_t>(kMaxRecordsPerBatch));
    CHECK_EQ(cache.recordCount(), config.limits.maxCachedRecords);
    CHECK_EQ(cache.range().firstCursor,
             static_cast<uint32_t>(kMaxRecordsPerBatch)
                 - static_cast<uint32_t>(config.limits.maxCachedRecords));
}

void testQspProjectionRetainsCompleteInboundSetEventPacket() {
    const GameDescriptor descriptor = gameDescriptor();
    QspRouteFixture fixture(descriptor);
    const runtime::ResilientSpectatorNodeRuntimeConfig config =
        nodeConfig(descriptor);
    CHECK_TRUE(fixture.start(config));
    CHECK_EQ(fixture.upstreamArchive.beginGame(descriptor), ArchiveResult::Ok);
    for (uint32_t event = 0u;
         event < static_cast<uint32_t>(kMaxSetEventsPerBatch); ++event) {
        CHECK_EQ(fixture.upstreamArchive.appendEvent(
                     kGameId, networkSampleEventAt(fixture.upstreamArchive,
                                                     event, 0u), nullptr),
                 ArchiveResult::Ok);
    }

    bool projected = false;
    for (size_t iteration = 0u; iteration < 500u; ++iteration) {
        if (!fixture.pumpOnce()) break;
        const SpectatorViewer* upstream = fixture.relay.upstreamViewer();
        const model::ResilientRelayNode* relayNode = fixture.relay.relayNode();
        if (upstream != nullptr && relayNode != nullptr
            && upstream->receivedNextEvent() == kMaxSetEventsPerBatch
            && relayNode->cache().eventRange().nextEvent
                == kMaxSetEventsPerBatch) {
            projected = true;
            break;
        }
    }
    CHECK_TRUE(projected);
    const model::ResilientRelayNode* relayNode = fixture.relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;
    CHECK_EQ(relayNode->cache().eventCount(), kMaxSetEventsPerBatch);
    CHECK_EQ(relayNode->cache().eventRange().firstEvent, 0u);
    CHECK_EQ(relayNode->cache().eventRange().nextEvent,
             static_cast<uint32_t>(kMaxSetEventsPerBatch));
}

void testNonzeroResumeHelloBindsItsExactCacheTuple() {
    constexpr model::NodeId kResumeChildNode = 0x3004u;
    constexpr EndpointId kResumeChildEndpoint = 0xB003u;
    const GameDescriptor descriptor = gameDescriptor();
    QspRouteFixture fixture(descriptor);
    const runtime::ResilientSpectatorNodeRuntimeConfig config =
        nodeConfig(descriptor);
    CHECK_TRUE(fixture.start(config));

    CHECK_EQ(fixture.upstreamArchive.beginGame(descriptor), ArchiveResult::Ok);
    CHECK_EQ(fixture.upstreamArchive.appendConfirmed(
                 kGameId, recordFor(0u), nullptr), ArchiveResult::Ok);
    CHECK_EQ(fixture.upstreamArchive.appendConfirmed(
                 kGameId, recordFor(1u), nullptr), ArchiveResult::Ok);
    CHECK_EQ(fixture.upstreamArchive.appendEvent(
                 kGameId, stageEventAt(fixture.upstreamArchive, 2u), nullptr),
             ArchiveResult::Ok);
    CHECK_EQ(fixture.upstreamArchive.appendConfirmed(
                 kGameId, recordFor(2u), nullptr), ArchiveResult::Ok);
    CHECK_TRUE(fixture.pumpUntil(3u, 0u, 0u));

    model::NodeAdvertisement child = advertisement(
        kResumeChildNode, model::NodeKind::SpectatorRelay, config.stream,
        0x0100007Fu, 43004u, config.sourceGeneration,
        config.initialRouteGeneration);
    child.activeParent = kRelayNode;
    child.depth = 2u;
    child.ancestry[0] = kRootNode;
    child.ancestry[1] = kRelayNode;
    child.ancestryCount = 2u;
    const model::ResilientRelayNode* relayNode = fixture.relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;
    constexpr ViewerId kResumeViewer = 0xC001D00DC001D00Dull;
    constexpr uint32_t kResumeGeneration = 1u;
    const model::VerifiedChainCursor resumeCursor = cacheCursor(
        relayNode->cache(), 3u, 1u);
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 child, signedChildLease(config, child, relayNode->cache(),
                                          fixture.nowMs, 0xC002u,
                                          kResumeViewer, kResumeGeneration,
                                          child.route,
                                          kChildBindingGeneration + 1u,
                                          resumeCursor, config.parentLeaseKey),
                 fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);

    CHECK_TRUE(resumeCursor.digest != 0u);

    HelloPayload hello{};
    hello.simulationContract = config.simulationContract;
    hello.compatibilityIdentity = config.compatibilityIdentity;
    hello.requestedGameId = kGameId;
    hello.viewerId = kResumeViewer;
    hello.generation = kResumeGeneration;
    hello.viewerNonce = viewerGenerationNonce(hello.viewerId, hello.generation);
    hello.resumeRecord = 3u;
    hello.resumeEvent = 1u;
    hello.resumeDigest = resumeCursor.digest;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    CHECK_TRUE(encodePayload(hello, payload));
    CHECK_TRUE(encodeDatagram(MessageType::Hello, config.qspIdentity, 1u,
                              payload, datagram));
    CHECK_EQ(fixture.relay.onChildDatagram(
                 childIngress(child.route, kChildBindingGeneration + 1u,
                              kResumeChildEndpoint),
                 datagram.data(), datagram.size(), fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);
    const model::CacheChildStatus* status = relayNode->cache().child(
        kResumeChildNode);
    CHECK_TRUE(status != nullptr);
    if (status != nullptr) {
        CHECK_EQ(status->acknowledgedCursor, 3u);
        CHECK_EQ(status->acknowledgedEvent, 1u);
        CHECK_TRUE(!status->controlledResyncRequired);
    }
}

void testRejectsRelayEventSuffixSmallerThanLegalPacket() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig config = nodeConfig(descriptor);
    config.limits.maxCachedEvents = kMaxSetEventsPerBatch - 1u;
    CHECK_TRUE(config.limits.valid());
    runtime::ResilientSpectatorNodeRuntime relay{};
    CHECK_TRUE(!relay.start(config));
}

void testQspUpstreamIdentityMismatchIsRejectedBeforeProjection() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntime relay{};
    const runtime::ResilientSpectatorNodeRuntimeConfig config =
        nodeConfig(descriptor);
    CHECK_TRUE(relay.start(config));

    GameStartPayload wrong = canonicalGameStart(descriptor);
    const SpectatorViewer* upstream = relay.upstreamViewer();
    CHECK_TRUE(upstream != nullptr);
    if (upstream == nullptr) return;
    wrong.viewerId = upstream->viewerId();
    wrong.generation = upstream->generation();
    wrong.viewerNonce = viewerGenerationNonce(wrong.viewerId, wrong.generation);
    wrong.compatibilityIdentity.simulationContent.validationRootSha256[0]
        ^= 0x5Au;
    wrong.initialDigest = computeInitialDigest(wrong);
    wrong.resumeDigest = wrong.initialDigest;

    std::vector<uint8_t> payload;
    std::vector<uint8_t> packet;
    CHECK_TRUE(encodePayload(wrong, payload));
    CHECK_TRUE(encodeDatagram(MessageType::GameStart, config.qspIdentity,
                              1u, payload, packet));
    CHECK_EQ(relay.onUpstreamDatagram(packet.data(), packet.size(), 1u),
             runtime::NodeRuntimeResult::UpstreamRejected);
    CHECK_EQ(upstream->receivedNextRecord(), 0u);
    ArchiveExtent extent{};
    CHECK_TRUE(relay.downstreamArchive() != nullptr);
    if (relay.downstreamArchive() != nullptr) {
        CHECK_TRUE(!relay.downstreamArchive()->extent(kGameId, extent));
    }
}

void testRuntimeRejectsPhysicalRouteCollisions() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig signedConfig =
        nodeConfig(descriptor);
    signedConfig.initialParentAdvertisement.route =
        signedConfig.localAdvertisement.route;
    signedConfig.initialParentAdvertisement.route.generation += 1u;
    signedConfig.initialParentAdvertisement.routeGeneration =
        signedConfig.initialParentAdvertisement.route.generation;
    signedConfig.initialRouteGeneration =
        signedConfig.initialParentAdvertisement.routeGeneration;
    runtime::ResilientSpectatorNodeRuntime relay{};
    CHECK_TRUE(!relay.start(signedConfig));

    runtime::ResilientSpectatorNodeRuntimeConfig canonicalConfig =
        canonicalNodeConfig(descriptor);
    canonicalConfig.localAdvertisement.controlRoute =
        canonicalConfig.localAdvertisement.route;
    canonicalConfig.localAdvertisement.controlRoute.generation =
        canonicalConfig.localAdvertisement.route.generation;
    CHECK_TRUE(!relay.start(canonicalConfig));
}

void testSignedLeaseAncestryAndPhysicalBindingCollisions() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig config = nodeConfig(descriptor);
    config.limits.maxChildrenPerRelay = 2u;
    config.localAdvertisement.childCapacity = 2u;
    QspRouteFixture fixture(descriptor);
    CHECK_TRUE(prepareSignedProjection(fixture, config, descriptor));
    const model::ResilientRelayNode* relayNode = fixture.relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;

    const model::VerifiedChainCursor requested = cacheCursor(
        relayNode->cache(), 0u, 0u);
    const model::NodeAdvertisement firstChild =
        canonicalChildAdvertisement(kChildNode, config);
    const model::ParentLease correct = signedChildLease(
        config, firstChild, relayNode->cache(), fixture.nowMs, 0xBA01u,
        0xBA01000000000001ull, 1u, firstChild.route, 0xBA010001u,
        requested, config.parentLeaseKey);

    model::ParentLease omittedParent = correct;
    omittedParent.ancestry[1] = model::kInvalidNodeId;
    omittedParent.ancestryCount = 1u;
    CHECK_TRUE(resignLease(omittedParent, config.parentLeaseKey));
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 firstChild, omittedParent, fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    model::ParentLease wrongImmediateParent = correct;
    wrongImmediateParent.ancestry[1] = 0xBADA55u;
    CHECK_TRUE(resignLease(wrongImmediateParent, config.parentLeaseKey));
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 firstChild, wrongImmediateParent, fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    model::NodeAdvertisement collidesWithLocal =
        canonicalChildAdvertisement(kChildNode + 10u, config);
    collidesWithLocal.route = config.localAdvertisement.route;
    ++collidesWithLocal.route.generation;
    collidesWithLocal.routeGeneration = collidesWithLocal.route.generation;
    const model::ParentLease localCollision = signedChildLease(
        config, collidesWithLocal, relayNode->cache(), fixture.nowMs,
        0xBA02u, 0xBA02000000000001ull, 1u, collidesWithLocal.route,
        0xBA020001u, requested, config.parentLeaseKey);
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 collidesWithLocal, localCollision, fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 firstChild, correct, fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);

    model::NodeAdvertisement sameEndpoint =
        canonicalChildAdvertisement(kChildNode + 1u, config);
    sameEndpoint.route = firstChild.route;
    ++sameEndpoint.route.generation;
    sameEndpoint.routeGeneration = sameEndpoint.route.generation;
    model::ParentLease endpointCollision = signedChildLease(
        config, sameEndpoint, relayNode->cache(), fixture.nowMs, 0xBA03u,
        0xBA03000000000001ull, 1u, sameEndpoint.route, 0xBA030001u,
        requested, config.parentLeaseKey);
    endpointCollision.childCapacity = 1u;
    CHECK_TRUE(resignLease(endpointCollision, config.parentLeaseKey));
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 sameEndpoint, endpointCollision, fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    const model::NodeAdvertisement duplicateViewer =
        canonicalChildAdvertisement(kChildNode + 2u, config);
    model::ParentLease viewerCollision = signedChildLease(
        config, duplicateViewer, relayNode->cache(), fixture.nowMs, 0xBA04u,
        0xBA01000000000001ull, 1u, duplicateViewer.route, 0xBA040001u,
        requested, config.parentLeaseKey);
    viewerCollision.childCapacity = 1u;
    CHECK_TRUE(resignLease(viewerCollision, config.parentLeaseKey));
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 duplicateViewer, viewerCollision, fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    std::vector<uint8_t> rawHello;
    CHECK_TRUE(encodeChildHello(config, 0xBA05000000000001ull, 1u, requested,
                                1u, rawHello));
    CHECK_EQ(fixture.relay.onChildDatagram(
                 childIngress(duplicateViewer.route, 0xBA050001u,
                              kChildEndpoint + 5u),
                 rawHello.data(), rawHello.size(), fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);
}

void testChildEndpointPinningAndSlotRetirement() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig config = nodeConfig(descriptor);
    config.limits.maxChildrenPerRelay = 2u;
    config.localAdvertisement.childCapacity = 2u;
    QspRouteFixture fixture(descriptor);
    CHECK_TRUE(prepareSignedProjection(fixture, config, descriptor));
    const model::ResilientRelayNode* relayNode = fixture.relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;
    const model::VerifiedChainCursor requested = cacheCursor(
        relayNode->cache(), 0u, 0u);
    const model::NodeAdvertisement firstChild =
        canonicalChildAdvertisement(kChildNode, config);
    const model::NodeAdvertisement secondChild =
        canonicalChildAdvertisement(kChildNode + 1u, config);
    const ViewerId firstViewer = 0xBB01000000000001ull;
    const ViewerId secondViewer = 0xBB02000000000001ull;
    const uint64_t firstBinding = 0xBB010001u;
    const uint64_t secondBinding = 0xBB020001u;
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 firstChild,
                 signedChildLease(config, firstChild, relayNode->cache(),
                                  fixture.nowMs, 0xBB01u, firstViewer, 1u,
                                  firstChild.route, firstBinding, requested,
                                  config.parentLeaseKey),
                 fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);
    model::ParentLease secondLease = signedChildLease(
        config, secondChild, relayNode->cache(), fixture.nowMs, 0xBB02u,
        secondViewer, 1u, secondChild.route, secondBinding, requested,
        config.parentLeaseKey);
    secondLease.childCapacity = 1u;
    CHECK_TRUE(resignLease(secondLease, config.parentLeaseKey));
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 secondChild, secondLease, fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);

    std::vector<uint8_t> firstHello;
    std::vector<uint8_t> secondHello;
    CHECK_TRUE(encodeChildHello(config, firstViewer, 1u, requested, 1u,
                                firstHello));
    CHECK_TRUE(encodeChildHello(config, secondViewer, 1u, requested, 1u,
                                secondHello));
    CHECK_EQ(fixture.relay.onChildDatagram(
                 childIngress(firstChild.route, firstBinding, kChildEndpoint),
                 firstHello.data(), firstHello.size(), fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_EQ(fixture.relay.onChildDatagram(
                 childIngress(secondChild.route, secondBinding, kChildEndpoint),
                 secondHello.data(), secondHello.size(), fixture.nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    runtime::ResilientSpectatorNodeRuntimeConfig lifecycleConfig =
        nodeConfig(descriptor);
    lifecycleConfig.limits.maxChildrenPerRelay = 1u;
    lifecycleConfig.localAdvertisement.childCapacity = 1u;
    lifecycleConfig.downstreamSource.handshakeDeadlineMs = 5u;
    QspRouteFixture lifecycle(descriptor);
    CHECK_TRUE(prepareSignedProjection(lifecycle, lifecycleConfig, descriptor));
    const model::ResilientRelayNode* lifecycleNode = lifecycle.relay.relayNode();
    CHECK_TRUE(lifecycleNode != nullptr);
    if (lifecycleNode == nullptr) return;
    const model::VerifiedChainCursor lifecycleCursor = cacheCursor(
        lifecycleNode->cache(), 0u, 0u);
    const model::NodeAdvertisement timedOut =
        canonicalChildAdvertisement(kChildNode + 20u, lifecycleConfig);
    CHECK_EQ(lifecycle.relay.registerAuthenticatedChild(
                 timedOut,
                 signedChildLease(lifecycleConfig, timedOut,
                                  lifecycleNode->cache(), lifecycle.nowMs,
                                  0xBB20u, 0xBB20000000000001ull, 1u,
                                  timedOut.route, 0xBB200001u, lifecycleCursor,
                                  lifecycleConfig.parentLeaseKey),
                 lifecycle.nowMs),
             runtime::NodeRuntimeResult::Accepted);
    lifecycle.relay.tick(lifecycle.nowMs
                         + lifecycleConfig.downstreamSource.handshakeDeadlineMs);
    CHECK_TRUE(lifecycleNode->cache().child(timedOut.nodeId) == nullptr);

    const model::NodeAdvertisement detached =
        canonicalChildAdvertisement(kChildNode + 21u, lifecycleConfig);
    const ViewerId detachedViewer = 0xBB21000000000001ull;
    const uint64_t detachedBinding = 0xBB210001u;
    CHECK_EQ(lifecycle.relay.registerAuthenticatedChild(
                 detached,
                 signedChildLease(lifecycleConfig, detached,
                                  lifecycleNode->cache(), lifecycle.nowMs + 5u,
                                  0xBB21u, detachedViewer, 1u, detached.route,
                                  detachedBinding, lifecycleCursor,
                                  lifecycleConfig.parentLeaseKey),
                 lifecycle.nowMs + 5u),
             runtime::NodeRuntimeResult::Accepted);
    std::vector<uint8_t> detachHello;
    CHECK_TRUE(encodeChildHello(lifecycleConfig, detachedViewer, 1u,
                                lifecycleCursor, 1u, detachHello));
    const runtime::NodeRuntimeChildIngress detachedIngress = childIngress(
        detached.route, detachedBinding, kChildEndpoint + 21u);
    CHECK_EQ(lifecycle.relay.onChildDatagram(
                 detachedIngress, detachHello.data(), detachHello.size(),
                 lifecycle.nowMs + 5u),
             runtime::NodeRuntimeResult::Accepted);
    DetachPayload detach{};
    detach.viewerId = detachedViewer;
    detach.generation = 1u;
    detach.nextRecord = lifecycleCursor.nextRecord;
    detach.nextEvent = lifecycleCursor.nextEvent;
    detach.digest = lifecycleCursor.digest;
    std::vector<uint8_t> detachPayload;
    std::vector<uint8_t> detachDatagram;
    CHECK_TRUE(encodePayload(detach, detachPayload));
    CHECK_TRUE(encodeDatagram(MessageType::Detach, lifecycleConfig.qspIdentity,
                              2u, detachPayload, detachDatagram));
    CHECK_EQ(lifecycle.relay.onChildDatagram(
                 detachedIngress, detachDatagram.data(), detachDatagram.size(),
                 lifecycle.nowMs + 5u),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(lifecycleNode->cache().child(detached.nodeId) == nullptr);
}

void testTerminalAckMirrorsTailAndFreesChildCapacity() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig config = nodeConfig(descriptor);
    config.limits.maxChildrenPerRelay = 1u;
    config.localAdvertisement.childCapacity = 1u;
    QspRouteFixture fixture(descriptor);
    CHECK_TRUE(prepareSignedProjection(fixture, config, descriptor));
    const model::ResilientRelayNode* relayNode = fixture.relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;
    const model::VerifiedChainCursor origin = cacheCursor(
        relayNode->cache(), 0u, 0u);
    const model::VerifiedChainCursor tail = cacheCursor(
        relayNode->cache(), 1u, 0u);
    const model::NodeAdvertisement child =
        canonicalChildAdvertisement(kChildNode + 30u, config);
    constexpr ViewerId kTerminalViewer = 0xBB30000000000001ull;
    constexpr uint64_t kTerminalBinding = 0xBB300001u;
    CHECK_EQ(fixture.relay.registerAuthenticatedChild(
                 child, signedChildLease(config, child, relayNode->cache(),
                                          fixture.nowMs, 0xBB30u,
                                          kTerminalViewer, 1u, child.route,
                                          kTerminalBinding, origin,
                                          config.parentLeaseKey),
                 fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);
    const runtime::NodeRuntimeChildIngress ingress = childIngress(
        child.route, kTerminalBinding, kChildEndpoint + 30u);
    std::vector<uint8_t> hello;
    CHECK_TRUE(encodeChildHello(config, kTerminalViewer, 1u, origin, 1u, hello));
    CHECK_EQ(fixture.relay.onChildDatagram(ingress, hello.data(), hello.size(),
                                           fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);

    CHECK_TRUE(!fixture.relay.pollChildOutbound(fixture.nowMs, 4u).empty());
    InitAckPayload init{};
    init.gameId = kGameId;
    init.viewerId = kTerminalViewer;
    init.generation = 1u;
    init.viewerNonce = viewerGenerationNonce(kTerminalViewer, 1u);
    init.startFingerprint = gameStartFingerprint(canonicalGameStart(descriptor));
    init.resumeRecord = origin.nextRecord;
    init.resumeEvent = origin.nextEvent;
    init.resumeDigest = origin.digest;
    std::vector<uint8_t> initPayload;
    std::vector<uint8_t> initDatagram;
    CHECK_TRUE(encodePayload(init, initPayload));
    CHECK_TRUE(encodeDatagram(MessageType::InitAck, config.qspIdentity, 2u,
                              initPayload, initDatagram));
    CHECK_EQ(fixture.relay.onChildDatagram(ingress, initDatagram.data(),
                                           initDatagram.size(), fixture.nowMs),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(!fixture.relay.pollChildOutbound(fixture.nowMs + 1u, 4u).empty());
    AckPayload tailAck{};
    tailAck.gameId = kGameId;
    tailAck.viewerId = kTerminalViewer;
    tailAck.generation = 1u;
    tailAck.nextRecord = tail.nextRecord;
    tailAck.nextEvent = tail.nextEvent;
    tailAck.digest = tail.digest;
    std::vector<uint8_t> tailAckPayload;
    std::vector<uint8_t> tailAckDatagram;
    CHECK_TRUE(encodePayload(tailAck, tailAckPayload));
    CHECK_TRUE(encodeDatagram(MessageType::Ack, config.qspIdentity, 3u,
                              tailAckPayload, tailAckDatagram));
    CHECK_EQ(fixture.relay.onChildDatagram(ingress, tailAckDatagram.data(),
                                           tailAckDatagram.size(),
                                           fixture.nowMs + 1u),
             runtime::NodeRuntimeResult::Accepted);

    CHECK_EQ(fixture.upstreamArchive.sealGame(
                 kGameId, TerminalReason::MatchEnded,
                 0xF00DCAFE12345678ull),
             ArchiveResult::Ok);
    bool downstreamSealed = false;
    for (size_t iteration = 0u; iteration < 64u; ++iteration) {
        if (!fixture.pumpOnce()) break;
        const SpectatorArchive* archive = fixture.relay.downstreamArchive();
        ArchiveExtent extent{};
        if (archive != nullptr && archive->extent(kGameId, extent)
            && extent.sealed) {
            downstreamSealed = true;
            break;
        }
    }
    CHECK_TRUE(downstreamSealed);
    if (!downstreamSealed) return;
    ArchiveExtent terminalExtent{};
    CHECK_TRUE(fixture.upstreamArchive.extent(kGameId, terminalExtent));
    bool sentTerminal = false;
    for (const runtime::NodeRuntimeDownstreamDatagram& packet
         : fixture.relay.pollChildOutbound(fixture.nowMs + 1u, 4u)) {
        if (packet.type == MessageType::Terminal) sentTerminal = true;
    }
    CHECK_TRUE(sentTerminal);

    TerminalAckPayload terminalAck{};
    terminalAck.gameId = kGameId;
    terminalAck.viewerId = kTerminalViewer;
    terminalAck.generation = 1u;
    terminalAck.nextRecord = terminalExtent.terminal.nextRecord;
    terminalAck.nextEvent = terminalExtent.terminal.nextEvent;
    terminalAck.finalDigest = terminalExtent.terminal.finalDigest;
    terminalAck.postTailStateHash = terminalExtent.terminal.postTailStateHash;
    terminalAck.reason = terminalExtent.terminal.reason;
    std::vector<uint8_t> terminalAckPayload;
    std::vector<uint8_t> terminalAckDatagram;
    CHECK_TRUE(encodePayload(terminalAck, terminalAckPayload));
    CHECK_TRUE(encodeDatagram(MessageType::TerminalAck, config.qspIdentity,
                              4u, terminalAckPayload, terminalAckDatagram));
    CHECK_EQ(fixture.relay.onChildDatagram(
                 ingress, terminalAckDatagram.data(), terminalAckDatagram.size(),
                 fixture.nowMs + 1u),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(relayNode->cache().child(child.nodeId) == nullptr);
}

bool populateCanonicalViewer(CanonicalViewerFixture& fixture,
                             const GameDescriptor& descriptor,
                             const uint32_t records = 2u,
                             const bool seal = false) {
    if (!fixture.begin(descriptor)) return false;
    const SetEvent event = networkSampleEventAt(fixture.authorityArchive,
                                                 0u, 0u);
    if (fixture.authorityArchive.appendEvent(kGameId, event, nullptr)
        != ArchiveResult::Ok) {
        return false;
    }
    for (uint32_t index = 0u; index < records; ++index) {
        if (fixture.authorityArchive.appendConfirmed(
                kGameId, recordFor(index), nullptr) != ArchiveResult::Ok) {
            return false;
        }
    }
    if (seal && fixture.authorityArchive.sealGame(
                    kGameId, TerminalReason::MatchEnded,
                    0xFACEC0DE12345678ull) != ArchiveResult::Ok) {
        return false;
    }
    return fixture.pumpUntil(records, 1u);
}

void testCanonicalIngressRejectsRawQspAndRequiresOpaqueAuthority() {
    const GameDescriptor descriptor = gameDescriptor();
    const runtime::ResilientSpectatorNodeRuntimeConfig config =
        canonicalNodeConfig(descriptor);
    runtime::ResilientSpectatorNodeRuntime relay{};
    CHECK_TRUE(relay.start(config));
    CHECK_TRUE(relay.upstreamViewer() == nullptr);
    CHECK_TRUE(relay.pollUpstreamOutbound(1u, 4u).empty());

    const GameStartPayload start = canonicalGameStart(descriptor);
    std::vector<uint8_t> rawPayload;
    std::vector<uint8_t> rawPacket;
    CHECK_TRUE(encodePayload(start, rawPayload));
    CHECK_TRUE(encodeDatagram(MessageType::GameStart, config.qspIdentity, 1u,
                              rawPayload, rawPacket));
    CHECK_EQ(relay.onUpstreamDatagram(rawPacket.data(), rawPacket.size(), 1u),
             runtime::NodeRuntimeResult::UpstreamRejected);

    CanonicalViewerFixture fixture(descriptor);
    CHECK_TRUE(populateCanonicalViewer(fixture, descriptor, 2u, true));
    const uint64_t nowMs = 100u;
    const std::shared_ptr<runtime::QtaAuthorityContext> authority =
        qtaAuthority(config, nowMs + 1000u);
    const runtime::QtaVerifiedCanonicalIngress ingress = canonicalIngress(
        authority, config, fixture.viewer);
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(ingress, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::Accepted);

    runtime::ResilientSpectatorNodeRuntimeConfig bad = config;
    bad.stream.gameEpoch += 1u;
    const runtime::QtaVerifiedCanonicalIngress badStream = canonicalIngress(
        authority, bad, fixture.viewer);
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(badStream, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::UpstreamRejected);
    bad = config;
    ++bad.qspIdentity.epoch;
    const runtime::QtaVerifiedCanonicalIngress badQsp = canonicalIngress(
        authority, bad, fixture.viewer);
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(badQsp, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::UpstreamRejected);
    bad = config;
    ++bad.localAdvertisement.route.port;
    const runtime::QtaVerifiedCanonicalIngress badLocal = canonicalIngress(
        authority, bad, fixture.viewer);
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(badLocal, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::UpstreamRejected);
    bad = config;
    ++bad.initialParentAdvertisement.nodeId;
    const runtime::QtaVerifiedCanonicalIngress badParent = canonicalIngress(
        authority, bad, fixture.viewer);
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(badParent, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::UpstreamRejected);
    bad = config;
    ++bad.sourceGeneration;
    const runtime::QtaVerifiedCanonicalIngress badSource = canonicalIngress(
        authority, bad, fixture.viewer);
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(badSource, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::UpstreamRejected);

    ViewerConfig wrongViewerConfig{};
    wrongViewerConfig.identity = config.qspIdentity;
    wrongViewerConfig.simulationContract = config.simulationContract;
    wrongViewerConfig.compatibilityIdentity = config.compatibilityIdentity;
    wrongViewerConfig.secureRandom = 0xDEAD000000000001ull;
    SpectatorViewer wrongViewer(wrongViewerConfig);
    CHECK_TRUE(wrongViewer.isConfigured());
    const runtime::QtaVerifiedCanonicalIngress badViewer = canonicalIngress(
        authority, config, wrongViewer);
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(badViewer, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::UpstreamRejected);
    const std::shared_ptr<runtime::QtaAuthorityContext> wrongAuthority =
        qtaAuthority(config, nowMs + 1000u);
    const runtime::QtaVerifiedCanonicalIngress badAuthority =
        canonicalIngress(wrongAuthority, config, fixture.viewer);
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(badAuthority, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::UpstreamRejected);
    CHECK_TRUE(!relay.noteUpstreamFailure(
        model::UpstreamFailureReason::TransportClosed, nowMs));
    CHECK_TRUE(relay.running());

    const SpectatorArchive* downstream = relay.downstreamArchive();
    ArchiveExtent sourceExtent{};
    ArchiveExtent downstreamExtent{};
    CHECK_TRUE(fixture.authorityArchive.extent(kGameId, sourceExtent));
    CHECK_TRUE(downstream != nullptr);
    if (downstream != nullptr) {
        CHECK_TRUE(downstream->extent(kGameId, downstreamExtent));
        CHECK_EQ(downstreamExtent.nextRecord, sourceExtent.nextRecord);
        CHECK_EQ(downstreamExtent.nextEvent, sourceExtent.nextEvent);
        CHECK_EQ(downstreamExtent.digest, sourceExtent.digest);
    }
}

void testCanonicalQtaLeasePinsExactChildTupleAndRejectsIngressMismatch() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig config =
        canonicalNodeConfig(descriptor);
    config.limits.maxChildrenPerRelay = 2u;
    config.localAdvertisement.childCapacity = 2u;
    const uint64_t nowMs = 100u;
    CanonicalViewerFixture fixture(descriptor);
    CHECK_TRUE(populateCanonicalViewer(fixture, descriptor, 1u));
    const std::shared_ptr<runtime::QtaAuthorityContext> authority =
        qtaAuthority(config, nowMs + 1000u);
    const runtime::QtaVerifiedCanonicalIngress canonical = canonicalIngress(
        authority, config, fixture.viewer);

    runtime::ResilientSpectatorNodeRuntime relay{};
    CHECK_TRUE(relay.start(config));
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(canonical, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::Accepted);
    const model::ResilientRelayNode* relayNode = relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;

    const model::NodeAdvertisement firstChild =
        canonicalChildAdvertisement(kChildNode, config);
    constexpr ViewerId kBoundViewer = 0xAA01000000000001ull;
    constexpr uint32_t kBoundGeneration = 3u;
    constexpr uint64_t kBoundEndpointGeneration = 0xAA010001u;
    const model::VerifiedChainCursor requested = cacheCursor(
        relayNode->cache(), 0u, 0u);
    const model::ParentLease exact = signedChildLease(
        config, firstChild, relayNode->cache(), nowMs, 0xAA01u,
        kBoundViewer, kBoundGeneration, firstChild.route,
        kBoundEndpointGeneration, requested, leaseKey());

    const std::shared_ptr<runtime::QtaAuthorityContext> otherContext =
        qtaAuthority(config, nowMs + 1000u);
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(exact, otherContext), nowMs),
             runtime::NodeRuntimeResult::ChildRejected);
    CHECK_EQ(relay.registerQtaVerifiedChild(qtaParentLease(exact, authority),
                                            nowMs),
             runtime::NodeRuntimeResult::Accepted);

    const model::NodeAdvertisement replayChild =
        canonicalChildAdvertisement(kChildNode + 1u, config);
    const model::ParentLease replayedNonce = signedChildLease(
        config, replayChild, relayNode->cache(), nowMs, 0xAA01u,
        kBoundViewer + 1u, kBoundGeneration, replayChild.route,
        kBoundEndpointGeneration + 1u, requested, leaseKey());
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(replayedNonce, authority), nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    const runtime::NodeRuntimeChildIngress ingress = childIngress(
        firstChild.route, kBoundEndpointGeneration, kChildEndpoint);
    HelloPayload hello{};
    hello.simulationContract = config.simulationContract;
    hello.compatibilityIdentity = config.compatibilityIdentity;
    hello.requestedGameId = kGameId;
    hello.viewerId = kBoundViewer;
    hello.generation = kBoundGeneration;
    hello.viewerNonce = viewerGenerationNonce(hello.viewerId,
                                               hello.generation);
    hello.resumeRecord = requested.nextRecord;
    hello.resumeEvent = requested.nextEvent;
    hello.resumeDigest = requested.digest;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> datagram;
    CHECK_TRUE(encodePayload(hello, payload));
    CHECK_TRUE(encodeDatagram(MessageType::Hello, config.qspIdentity, 1u,
                              payload, datagram));
    CHECK_EQ(relay.onChildDatagram(ingress, datagram.data(), datagram.size(),
                                   nowMs),
             runtime::NodeRuntimeResult::Accepted);

    const runtime::NodeRuntimeChildIngress wrongEndpoint = childIngress(
        firstChild.route, kBoundEndpointGeneration, kChildEndpoint + 1u);
    CHECK_EQ(relay.onChildDatagram(wrongEndpoint, datagram.data(),
                                   datagram.size(), nowMs),
             runtime::NodeRuntimeResult::ChildRejected);
    model::RouteDescriptor reboundRoute = firstChild.route;
    ++reboundRoute.port;
    const runtime::NodeRuntimeChildIngress rebound = childIngress(
        reboundRoute, kBoundEndpointGeneration, kChildEndpoint);
    CHECK_EQ(relay.onChildDatagram(rebound, datagram.data(), datagram.size(),
                                   nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    HelloPayload wrongIdentity = hello;
    ++wrongIdentity.viewerId;
    wrongIdentity.viewerNonce = viewerGenerationNonce(
        wrongIdentity.viewerId, wrongIdentity.generation);
    CHECK_TRUE(encodePayload(wrongIdentity, payload));
    CHECK_TRUE(encodeDatagram(MessageType::Hello, config.qspIdentity, 2u,
                              payload, datagram));
    CHECK_EQ(relay.onChildDatagram(ingress, datagram.data(), datagram.size(),
                                   nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    HelloPayload wrongGeneration = hello;
    ++wrongGeneration.generation;
    wrongGeneration.viewerNonce = viewerGenerationNonce(
        wrongGeneration.viewerId, wrongGeneration.generation);
    CHECK_TRUE(encodePayload(wrongGeneration, payload));
    CHECK_TRUE(encodeDatagram(MessageType::Hello, config.qspIdentity, 3u,
                              payload, datagram));
    CHECK_EQ(relay.onChildDatagram(ingress, datagram.data(), datagram.size(),
                                   nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    HelloPayload wrongCursor = hello;
    ++wrongCursor.resumeRecord;
    CHECK_TRUE(encodePayload(wrongCursor, payload));
    CHECK_TRUE(encodeDatagram(MessageType::Hello, config.qspIdentity, 4u,
                              payload, datagram));
    CHECK_EQ(relay.onChildDatagram(ingress, datagram.data(), datagram.size(),
                                   nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    HelloPayload wrongDigest = hello;
    wrongDigest.resumeDigest ^= 0x1ull;
    CHECK_TRUE(encodePayload(wrongDigest, payload));
    CHECK_TRUE(encodeDatagram(MessageType::Hello, config.qspIdentity, 5u,
                              payload, datagram));
    CHECK_EQ(relay.onChildDatagram(ingress, datagram.data(), datagram.size(),
                                   nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    CHECK_TRUE(encodePayload(hello, payload));
    CHECK_TRUE(encodeDatagram(MessageType::Hello, config.qspIdentity, 1u,
                              payload, datagram));
    CHECK_EQ(relay.onChildDatagram(ingress, datagram.data(), datagram.size(),
                                   nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    MigratePayload migrate{};
    migrate.viewerId = kBoundViewer;
    migrate.oldGeneration = kBoundGeneration;
    migrate.newGeneration = kBoundGeneration + 1u;
    migrate.resumeRecord = requested.nextRecord;
    migrate.resumeEvent = requested.nextEvent;
    migrate.resumeDigest = requested.digest;
    CHECK_TRUE(encodePayload(migrate, payload));
    CHECK_TRUE(encodeDatagram(MessageType::Migrate, config.qspIdentity, 6u,
                              payload, datagram));
    CHECK_EQ(relay.onChildDatagram(ingress, datagram.data(), datagram.size(),
                                   nowMs),
             runtime::NodeRuntimeResult::ChildRejected);
}

void testCanonicalQtaGrandchildRequiresExactSignedAncestorPrefix() {
    const GameDescriptor descriptor = gameDescriptor();
    const uint64_t nowMs = 100u;
    const runtime::ResilientSpectatorNodeRuntimeConfig nested =
        nestedCanonicalNodeConfig(descriptor);
    CanonicalViewerFixture fixture(descriptor);
    CHECK_TRUE(populateCanonicalViewer(fixture, descriptor, 1u));

    runtime::ResilientSpectatorNodeRuntimeConfig defaulted = nested;
    defaulted.initialParentDepth = 0u;
    defaulted.initialParentAncestry = {};
    defaulted.initialParentAncestryCount = 0u;
    const std::shared_ptr<runtime::QtaAuthorityContext> defaultAuthority =
        qtaAuthority(defaulted, nowMs + 1000u);
    const runtime::QtaVerifiedCanonicalIngress defaultIngress = canonicalIngress(
        defaultAuthority, defaulted, fixture.viewer);
    runtime::ResilientSpectatorNodeRuntime defaultRelay{};
    CHECK_TRUE(defaultRelay.start(defaulted));
    CHECK_EQ(defaultRelay.projectCanonicalVerifiedPrefix(
                 defaultIngress, fixture.viewer, nowMs),
             runtime::NodeRuntimeResult::Accepted);
    const model::ResilientRelayNode* const defaultNode =
        defaultRelay.relayNode();
    CHECK_TRUE(defaultNode != nullptr);
    if (defaultNode == nullptr) return;

    model::NodeAdvertisement grandchild = canonicalChildAdvertisement(
        kGrandchildNode, nested);
    grandchild.depth = 3u;
    grandchild.ancestry = {};
    grandchild.ancestry[0] = kRootNode;
    grandchild.ancestry[1] = kIntermediateRelayNode;
    grandchild.ancestry[2] = kRelayNode;
    grandchild.ancestryCount = 3u;
    const model::VerifiedChainCursor requested = cacheCursor(
        defaultNode->cache(), 0u, 0u);
    model::ParentLease signedGrandchild = signedChildLease(
        nested, grandchild, defaultNode->cache(), nowMs, 0xAD710001u,
        0xAD71000100000001ull, 1u, grandchild.route, 0xAD710002u,
        requested, leaseKey());
    signedGrandchild.parentDepth = 2u;
    signedGrandchild.ancestry = {};
    signedGrandchild.ancestry[0] = kRootNode;
    signedGrandchild.ancestry[1] = kIntermediateRelayNode;
    signedGrandchild.ancestry[2] = kRelayNode;
    signedGrandchild.ancestryCount = 3u;
    CHECK_TRUE(resignLease(signedGrandchild, leaseKey()));

    CHECK_EQ(defaultRelay.registerQtaVerifiedChild(
                 qtaParentLease(signedGrandchild, defaultAuthority), nowMs),
             runtime::NodeRuntimeResult::ChildRejected);
    CHECK_TRUE(!defaultRelay.hasActiveChild(kGrandchildNode));

    runtime::ResilientSpectatorNodeRuntimeConfig forged = nested;
    forged.initialParentAncestry[0] = kRootNode + 1u;
    const std::shared_ptr<runtime::QtaAuthorityContext> forgedAuthority =
        qtaAuthority(forged, nowMs + 1000u);
    const runtime::QtaVerifiedCanonicalIngress forgedIngress = canonicalIngress(
        forgedAuthority, forged, fixture.viewer);
    runtime::ResilientSpectatorNodeRuntime forgedRelay{};
    CHECK_TRUE(forgedRelay.start(forged));
    CHECK_EQ(forgedRelay.projectCanonicalVerifiedPrefix(
                 forgedIngress, fixture.viewer, nowMs),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_EQ(forgedRelay.registerQtaVerifiedChild(
                 qtaParentLease(signedGrandchild, forgedAuthority), nowMs),
             runtime::NodeRuntimeResult::ChildRejected);
    CHECK_TRUE(!forgedRelay.hasActiveChild(kGrandchildNode));

    const std::shared_ptr<runtime::QtaAuthorityContext> exactAuthority =
        qtaAuthority(nested, nowMs + 1000u);
    const runtime::QtaVerifiedCanonicalIngress exactIngress = canonicalIngress(
        exactAuthority, nested, fixture.viewer);
    runtime::ResilientSpectatorNodeRuntime exactRelay{};
    CHECK_TRUE(exactRelay.start(nested));
    CHECK_EQ(exactRelay.projectCanonicalVerifiedPrefix(
                 exactIngress, fixture.viewer, nowMs),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_EQ(exactRelay.registerQtaVerifiedChild(
                 qtaParentLease(signedGrandchild, exactAuthority), nowMs),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(exactRelay.hasActiveChild(kGrandchildNode));

    runtime::ResilientSpectatorNodeRuntimeConfig repeated = nested;
    repeated.initialParentDepth = 2u;
    repeated.initialParentAncestry[1] = kRootNode;
    repeated.initialParentAncestryCount = 2u;
    runtime::ResilientSpectatorNodeRuntime repeatedRelay{};
    CHECK_TRUE(!repeatedRelay.start(repeated));

    runtime::ResilientSpectatorNodeRuntimeConfig overflowed = nested;
    overflowed.initialParentDepth = overflowed.limits.maxActiveDepth;
    overflowed.initialParentAncestryCount = overflowed.limits.maxActiveDepth;
    overflowed.initialParentAncestry = {};
    overflowed.initialParentAncestry[0] = kRootNode;
    overflowed.initialParentAncestry[1] = kIntermediateRelayNode;
    overflowed.initialParentAncestry[2] = 0x1A01u;
    runtime::ResilientSpectatorNodeRuntime overflowedRelay{};
    CHECK_TRUE(!overflowedRelay.start(overflowed));
}

void testCanonicalQtaNonceReservationReleasesStaleAssignment() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig config =
        canonicalNodeConfig(descriptor);
    config.limits.maxChildrenPerRelay = 1u;
    config.localAdvertisement.childCapacity = 1u;
    const uint64_t nowMs = 100u;
    CanonicalViewerFixture fixture(descriptor);
    CHECK_TRUE(populateCanonicalViewer(fixture, descriptor, 1u));
    const std::shared_ptr<runtime::QtaAuthorityContext> authority =
        qtaAuthority(config, nowMs + 10000u);
    const runtime::QtaVerifiedCanonicalIngress canonical = canonicalIngress(
        authority, config, fixture.viewer);

    runtime::ResilientSpectatorNodeRuntime relay{};
    CHECK_TRUE(relay.start(config));
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(canonical, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::Accepted);
    const model::ResilientRelayNode* relayNode = relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;

    const model::NodeAdvertisement child =
        canonicalChildAdvertisement(kChildNode, config);
    const model::VerifiedChainCursor requested = cacheCursor(
        relayNode->cache(), 0u, 0u);
    const model::ParentLease valid = signedChildLease(
        config, child, relayNode->cache(), nowMs, 0xAC710001u,
        0xAC71000100000001ull, 1u, child.route, 0xAC710002u, requested,
        leaseKey());

    model::ParentLease stale = valid;
    stale.cacheRange.firstCursor = 1u;
    stale.cacheRange.nextCursor = 1u;
    stale.latestVerifiedCursor = 1u;
    CHECK_TRUE(resignLease(stale, leaseKey()));
    std::optional<runtime::QtaVerifiedChildRetirement> rejectedRetirement;
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(stale, authority), nowMs, rejectedRetirement),
             runtime::NodeRuntimeResult::ChildRejected);
    CHECK_TRUE(!rejectedRetirement.has_value());
    CHECK_TRUE(!relay.hasActiveChild(child.nodeId));

    std::optional<runtime::QtaVerifiedChildRetirement> acceptedRetirement;
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(valid, authority), nowMs, acceptedRetirement),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(acceptedRetirement.has_value());
    CHECK_TRUE(relay.hasActiveChild(child.nodeId));

    std::optional<runtime::QtaVerifiedChildRetirement> replayRetirement;
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(valid, authority), nowMs, replayRetirement),
             runtime::NodeRuntimeResult::ChildRejected);
    CHECK_TRUE(!replayRetirement.has_value());
}

void testCanonicalQtaChildAdmissionAllowsSafeCacheAdvanceOnly() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig config =
        canonicalNodeConfig(descriptor);
    config.limits.maxChildrenPerRelay = 2u;
    config.localAdvertisement.childCapacity = 2u;
    const uint64_t nowMs = 100u;
    CanonicalViewerFixture fixture(descriptor);
    CHECK_TRUE(populateCanonicalViewer(fixture, descriptor, 1u));
    const std::shared_ptr<runtime::QtaAuthorityContext> authority =
        qtaAuthority(config, nowMs + 10000u);
    const runtime::QtaVerifiedCanonicalIngress canonical = canonicalIngress(
        authority, config, fixture.viewer);

    runtime::ResilientSpectatorNodeRuntime relay{};
    CHECK_TRUE(relay.start(config));
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(canonical, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::Accepted);
    const model::ResilientRelayNode* relayNode = relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;

    const model::VerifiedChainCursor signedBoundary = cacheCursor(
        relayNode->cache(), 0u, 0u);
    const model::NodeAdvertisement advancingChild =
        canonicalChildAdvertisement(kChildNode, config);
    const model::ParentLease signedBeforeAdvance = signedChildLease(
        config, advancingChild, relayNode->cache(), nowMs, 0xAC720001u,
        0xAC72000100000001ull, 1u, advancingChild.route, 0xAC720002u,
        signedBoundary, leaseKey());

    CHECK_EQ(fixture.authorityArchive.appendConfirmed(
                 kGameId, recordFor(1u), nullptr),
             ArchiveResult::Ok);
    CHECK_TRUE(fixture.pumpUntil(2u, 1u));
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(canonical, fixture.viewer,
                                                  nowMs + 1u),
             runtime::NodeRuntimeResult::Accepted);
    const model::ResilientRelayNode* const advanced = relay.relayNode();
    CHECK_TRUE(advanced != nullptr);
    if (advanced == nullptr) return;
    CHECK_EQ(advanced->cache().range().nextCursor, 2u);
    CHECK_TRUE(advanced->cache().range().containsBoundary(
        signedBoundary.nextRecord));

    std::optional<runtime::QtaVerifiedChildRetirement> advancingRetirement;
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(signedBeforeAdvance, authority), nowMs + 1u,
                 advancingRetirement),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(advancingRetirement.has_value());

    const model::NodeAdvertisement futureChild = canonicalChildAdvertisement(
        kChildNode + 1u, config);
    model::ParentLease future = signedChildLease(
        config, futureChild, advanced->cache(), nowMs + 1u, 0xAC720003u,
        0xAC72000300000001ull, 1u, futureChild.route, 0xAC720004u,
        signedBoundary, leaseKey());
    future.cacheRange.nextCursor = advanced->cache().range().nextCursor + 1u;
    future.latestVerifiedCursor = future.cacheRange.nextCursor;
    CHECK_TRUE(resignLease(future, leaseKey()));
    std::optional<runtime::QtaVerifiedChildRetirement> futureRetirement;
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(future, authority), nowMs + 1u,
                 futureRetirement),
             runtime::NodeRuntimeResult::ChildRejected);
    CHECK_TRUE(!futureRetirement.has_value());

    for (uint32_t record = 2u; record < 6u; ++record) {
        CHECK_EQ(fixture.authorityArchive.appendConfirmed(
                     kGameId, recordFor(record), nullptr),
                 ArchiveResult::Ok);
    }
    CHECK_TRUE(fixture.pumpUntil(6u, 1u));
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(canonical, fixture.viewer,
                                                  nowMs + 2u),
             runtime::NodeRuntimeResult::Accepted);
    const model::ResilientRelayNode* const evicted = relay.relayNode();
    CHECK_TRUE(evicted != nullptr);
    if (evicted == nullptr) return;
    CHECK_TRUE(!evicted->cache().range().containsBoundary(
        signedBoundary.nextRecord));
    const model::NodeAdvertisement evictedChild = canonicalChildAdvertisement(
        kChildNode + 2u, config);
    model::ParentLease evictedLease = signedBeforeAdvance;
    evictedLease.childId = evictedChild.nodeId;
    evictedLease.childBinding.childViewerId = 0xAC72000500000001ull;
    evictedLease.childBinding.childQspRoute = evictedChild.route;
    evictedLease.childBinding.childEndpointBindingGeneration = 0xAC720006u;
    evictedLease.replayNonce = 0xAC720007u;
    CHECK_TRUE(resignLease(evictedLease, leaseKey()));
    std::optional<runtime::QtaVerifiedChildRetirement> evictedRetirement;
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(evictedLease, authority), nowMs + 2u,
                 evictedRetirement),
             runtime::NodeRuntimeResult::ChildRejected);
    CHECK_TRUE(!evictedRetirement.has_value());
}

void testCanonicalQtaRetirementCapabilityRejectsReplayAndStaleBinding() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig config =
        canonicalNodeConfig(descriptor);
    config.limits.maxChildrenPerRelay = 1u;
    config.localAdvertisement.childCapacity = 1u;
    const uint64_t nowMs = 100u;
    CanonicalViewerFixture fixture(descriptor);
    CHECK_TRUE(populateCanonicalViewer(fixture, descriptor, 1u));
    const std::shared_ptr<runtime::QtaAuthorityContext> authority =
        qtaAuthority(config, nowMs + 100000u);
    const runtime::QtaVerifiedCanonicalIngress canonical = canonicalIngress(
        authority, config, fixture.viewer);

    runtime::ResilientSpectatorNodeRuntime relay{};
    CHECK_TRUE(relay.start(config));
    CHECK_EQ(relay.projectCanonicalVerifiedPrefix(canonical, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::Accepted);
    const model::ResilientRelayNode* relayNode = relay.relayNode();
    CHECK_TRUE(relayNode != nullptr);
    if (relayNode == nullptr) return;
    const model::VerifiedChainCursor requested = cacheCursor(
        relayNode->cache(), 0u, 0u);

    const model::NodeAdvertisement alreadyRetiredChild =
        canonicalChildAdvertisement(kChildNode, config);
    const model::ParentLease alreadyRetiredLease = signedChildLease(
        config, alreadyRetiredChild, relayNode->cache(), nowMs, 0xAC730001u,
        0xAC73000100000001ull, 1u, alreadyRetiredChild.route, 0xAC730002u,
        requested, leaseKey());
    std::optional<runtime::QtaVerifiedChildRetirement> alreadyRetired;
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(alreadyRetiredLease, authority), nowMs,
                 alreadyRetired),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(alreadyRetired.has_value());
    relay.tick(nowMs + config.downstreamSource.handshakeDeadlineMs);
    CHECK_TRUE(!relay.hasActiveChild(alreadyRetiredChild.nodeId));
    CHECK_EQ(qclient::spectator::resilient::test::
                 QtaVerifiedChildRetirementTestFactory::retire(
                     relay, std::move(*alreadyRetired),
                     nowMs + config.downstreamSource.handshakeDeadlineMs),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_EQ(qclient::spectator::resilient::test::
                 QtaVerifiedChildRetirementTestFactory::retire(
                     relay, std::move(*alreadyRetired),
                     nowMs + config.downstreamSource.handshakeDeadlineMs),
             runtime::NodeRuntimeResult::ChildRejected);

    const uint64_t laterMs =
        nowMs + config.downstreamSource.handshakeDeadlineMs + 1u;
    const model::NodeAdvertisement replaceable = canonicalChildAdvertisement(
        kChildNode + 1u, config);
    const model::ParentLease replaceableLease = signedChildLease(
        config, replaceable, relayNode->cache(), laterMs, 0xAC730003u,
        0xAC73000300000001ull, 1u, replaceable.route, 0xAC730004u,
        requested, leaseKey());
    std::optional<runtime::QtaVerifiedChildRetirement> staleRetirement;
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(replaceableLease, authority), laterMs,
                 staleRetirement),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(staleRetirement.has_value());
    relay.tick(laterMs + config.downstreamSource.handshakeDeadlineMs);
    CHECK_TRUE(!relay.hasActiveChild(replaceable.nodeId));

    model::NodeAdvertisement replacement = replaceable;
    replacement.route.port = static_cast<uint16_t>(replacement.route.port + 9u);
    replacement.route.generation += 1u;
    replacement.routeGeneration = replacement.route.generation;
    const model::ParentLease replacementLease = signedChildLease(
        config, replacement, relayNode->cache(),
        laterMs + config.downstreamSource.handshakeDeadlineMs, 0xAC730005u,
        0xAC73000500000001ull, 1u, replacement.route, 0xAC730006u,
        requested, leaseKey());
    std::optional<runtime::QtaVerifiedChildRetirement> replacementRetirement;
    CHECK_EQ(relay.registerQtaVerifiedChild(
                 qtaParentLease(replacementLease, authority),
                 laterMs + config.downstreamSource.handshakeDeadlineMs,
                 replacementRetirement),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(replacementRetirement.has_value());
    CHECK_TRUE(relay.hasActiveChild(replacement.nodeId));

    CHECK_EQ(qclient::spectator::resilient::test::
                 QtaVerifiedChildRetirementTestFactory::retire(
                     relay, std::move(*staleRetirement),
                     laterMs + config.downstreamSource.handshakeDeadlineMs),
             runtime::NodeRuntimeResult::ChildRejected);
    CHECK_TRUE(relay.hasActiveChild(replacement.nodeId));
    CHECK_EQ(qclient::spectator::resilient::test::
                 QtaVerifiedChildRetirementTestFactory::retire(
                     relay, std::move(*replacementRetirement),
                     laterMs + config.downstreamSource.handshakeDeadlineMs),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_TRUE(!relay.hasActiveChild(replacement.nodeId));
    CHECK_EQ(qclient::spectator::resilient::test::
                 QtaVerifiedChildRetirementTestFactory::retire(
                     relay, std::move(*replacementRetirement),
                     laterMs + config.downstreamSource.handshakeDeadlineMs),
             runtime::NodeRuntimeResult::ChildRejected);
}

void testCanonicalAuthorityLifetimeAndNonceLedgerSurviveRuntimeRestart() {
    const GameDescriptor descriptor = gameDescriptor();
    runtime::ResilientSpectatorNodeRuntimeConfig config =
        canonicalNodeConfig(descriptor);
    config.limits.maxChildrenPerRelay = 1u;
    config.localAdvertisement.childCapacity = 1u;
    const uint64_t nowMs = 100u;
    CanonicalViewerFixture fixture(descriptor);
    CHECK_TRUE(populateCanonicalViewer(fixture, descriptor, 1u));
    const std::shared_ptr<runtime::QtaAuthorityContext> authority =
        qtaAuthority(config, nowMs + 10u);
    const runtime::QtaVerifiedCanonicalIngress canonical = canonicalIngress(
        authority, config, fixture.viewer);

    runtime::ResilientSpectatorNodeRuntime first{};
    CHECK_TRUE(first.start(config));
    CHECK_EQ(first.projectCanonicalVerifiedPrefix(canonical, fixture.viewer,
                                                  nowMs),
             runtime::NodeRuntimeResult::Accepted);
    const model::ResilientRelayNode* firstNode = first.relayNode();
    CHECK_TRUE(firstNode != nullptr);
    if (firstNode == nullptr) return;
    const model::NodeAdvertisement child =
        canonicalChildAdvertisement(kChildNode, config);
    const model::VerifiedChainCursor requested = cacheCursor(
        firstNode->cache(), 0u, 0u);
    const model::ParentLease lease = signedChildLease(
        config, child, firstNode->cache(), nowMs, 0xAA55u,
        0xAA55000000000001ull, 1u, child.route, 0xAA550001u, requested,
        leaseKey(), 1u);
    CHECK_EQ(first.registerQtaVerifiedChild(qtaParentLease(lease, authority),
                                            nowMs),
             runtime::NodeRuntimeResult::Accepted);
    first.stop();

    runtime::ResilientSpectatorNodeRuntime restarted{};
    CHECK_TRUE(restarted.start(config));
    const runtime::QtaVerifiedCanonicalIngress restartedCanonical =
        canonicalIngress(authority, config, fixture.viewer);
    CHECK_EQ(restarted.projectCanonicalVerifiedPrefix(restartedCanonical,
                                                      fixture.viewer, nowMs),
             runtime::NodeRuntimeResult::Accepted);
    CHECK_EQ(restarted.registerQtaVerifiedChild(
                 qtaParentLease(lease, authority), nowMs),
             runtime::NodeRuntimeResult::ChildRejected);

    restarted.tick(nowMs + 10u);
    CHECK_TRUE(!restarted.running());

    const std::shared_ptr<runtime::QtaAuthorityContext> revocable =
        qtaAuthority(config, nowMs + 1000u);
    const runtime::QtaVerifiedCanonicalIngress revocableCanonical =
        canonicalIngress(revocable, config, fixture.viewer);
    runtime::ResilientSpectatorNodeRuntime revoked{};
    CHECK_TRUE(revoked.start(config));
    CHECK_EQ(revoked.projectCanonicalVerifiedPrefix(revocableCanonical,
                                                    fixture.viewer, nowMs),
             runtime::NodeRuntimeResult::Accepted);
    qclient::spectator::resilient::test::QtaAuthorityContextTestFactory::
        revoke(revocable);
    CHECK_TRUE(revoked.pollChildOutbound(nowMs, 4u).empty());
    CHECK_TRUE(!revoked.running());
}

}

int main() {
    testQspProjectionEventChainBoundedCacheAndChildAck();
    testQspProjectionRetainsCompleteInboundRecordPacket();
    testQspProjectionRetainsCompleteInboundSetEventPacket();
    testNonzeroResumeHelloBindsItsExactCacheTuple();
    testRejectsRelayEventSuffixSmallerThanLegalPacket();
    testQspUpstreamIdentityMismatchIsRejectedBeforeProjection();
    testRuntimeRejectsPhysicalRouteCollisions();
    testSignedLeaseAncestryAndPhysicalBindingCollisions();
    testChildEndpointPinningAndSlotRetirement();
    testTerminalAckMirrorsTailAndFreesChildCapacity();
    testCanonicalIngressRejectsRawQspAndRequiresOpaqueAuthority();
    testCanonicalQtaLeasePinsExactChildTupleAndRejectsIngressMismatch();
    testCanonicalQtaGrandchildRequiresExactSignedAncestorPrefix();
    testCanonicalQtaNonceReservationReleasesStaleAssignment();
    testCanonicalQtaChildAdmissionAllowsSafeCacheAdvanceOnly();
    testCanonicalQtaRetirementCapabilityRejectsReplayAndStaleBinding();
    testCanonicalAuthorityLifetimeAndNonceLedgerSurviveRuntimeRestart();
    if (g_failures != 0) {
        std::fprintf(stderr, "resilient spectator node runtime tests FAILED: %d\n",
                     g_failures);
        return 1;
    }
    std::puts("resilient spectator node runtime tests passed");
    return 0;
}
