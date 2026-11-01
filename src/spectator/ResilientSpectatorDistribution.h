#pragma once

#include "SpectatorGrant.h"
#include "SpectatorProtocol.h"
#include "../compat/CompatibilityIdentity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qnetplay::spectator::resilient {

using NodeId = uint64_t;
constexpr NodeId kInvalidNodeId = 0;
constexpr size_t kMaxTopologyAncestry = 8;

struct StreamCompatibilityIdentity {
    uint64_t setOrSessionId = 0;
    uint64_t gameEpoch = 0;
    qnetplay::compat::CompatibilityIdentityWire compatibility{};
    uint64_t streamRootIdentity = 0;

    bool valid() const;
    bool operator==(const StreamCompatibilityIdentity& other) const;
    bool operator!=(const StreamCompatibilityIdentity& other) const {
        return !(*this == other);
    }
};

bool makeStreamCompatibilityIdentity(
    const qnetplay::compat::CompatibilityIdentityWire& compatibility,
    uint64_t setOrSessionId, uint64_t gameEpoch, uint64_t streamRootIdentity,
    StreamCompatibilityIdentity& out);

enum class NodeKind : uint8_t {
    HostRoot,
    ClientRoot,
    SpectatorRelay,
    TrafficRelay,
};

enum class RouteKind : uint8_t {
    Direct,
    Punched,
    TrafficRelay,
    RootFallback,
};

constexpr uint16_t kResilientCapabilityModelRevision = 1;
enum ResilientCapability : uint32_t {
    kCapabilityEphemeralEndpoint = 1u << 0,
    kCapabilityMultiPeerAutopunch = 1u << 1,
    kCapabilityMultiViewerRoot = 1u << 2,
    kCapabilityRelayCapableSpectator = 1u << 3,
    kCapabilityStandbyParent = 1u << 4,
    kCapabilitySubtreeFailover = 1u << 5,
    kCapabilityGracefulDrain = 1u << 6,
    kCapabilityTopologyLeaf = 1u << 7,
};

struct RouteDescriptor {
    RouteKind kind = RouteKind::Direct;
    uint32_t ipv4BE = 0;
    uint16_t port = 0;
    uint64_t generation = 0;
    bool routeProved = false;

    bool valid() const;
    bool sameEndpoint(const RouteDescriptor& other) const;
};

struct ResilientLimits {
    size_t maxTopologyNodes = 64;
    uint8_t maxActiveDepth = 3;
    size_t maxChildrenPerRoot = 32;
    size_t maxChildrenPerRelay = 8;
    size_t maxPendingChildren = 8;
    size_t maxStandbyParents = 3;
    size_t maxConcurrentRouteRaces = 3;

    uint32_t routeRaceMs = 3000;
    uint32_t routeRaceRecordBudget = 192;
    uint32_t continuityOverlapRecords = 128;
    uint32_t retransmissionMarginRecords = 128;
    uint32_t bootstrapMarginRecords = 256;
    size_t maxCachedRecords = 704;
    size_t maxCachedEvents = 256;
    size_t maxCachedBytes = 128u * 1024u;
    size_t maxCheckpoints = 4;
    size_t maxCheckpointBytes = 16u * 1024u;

    uint64_t standbyLeaseMs = 15000;
    uint64_t recoveryBudgetMs = 6000;

    size_t requiredCacheRecords() const;
    bool valid() const;
};

struct VerifiedCacheRange {
    uint32_t firstCursor = 0;
    uint32_t nextCursor = 0;

    bool containsRecord(uint32_t cursor) const {
        return cursor >= firstCursor && cursor < nextCursor;
    }
    bool containsBoundary(uint32_t cursor) const {
        return cursor >= firstCursor && cursor <= nextCursor;
    }
};

struct VerifiedEventRange {
    uint32_t firstEvent = 0;
    uint32_t nextEvent = 0;

    bool containsEvent(uint32_t cursor) const {
        return cursor >= firstEvent && cursor < nextEvent;
    }
    bool containsBoundary(uint32_t cursor) const {
        return cursor >= firstEvent && cursor <= nextEvent;
    }
};

struct VerifiedChainCursor {
    uint32_t nextRecord = 0;
    uint32_t nextEvent = 0;
    uint64_t digest = 0;
};

struct RelayRecordEnvelope {
    StreamCompatibilityIdentity stream{};
    uint64_t gameId = 0;
    uint64_t sourceGeneration = 0;
    bool confirmed = false;
    ConfirmedRecord record{};
};

struct VerifiedRelayRecord {
    ConfirmedRecord record{};
    uint64_t sourceGeneration = 0;
};

struct RelayEventEnvelope {
    StreamCompatibilityIdentity stream{};
    uint64_t gameId = 0;
    uint64_t sourceGeneration = 0;
    bool confirmed = false;
    SetEvent event{};
};

struct VerifiedRelayEvent {
    SetEvent event{};
    uint64_t sourceGeneration = 0;
};

struct VerifiedCheckpoint {
    StreamCompatibilityIdentity stream{};
    uint64_t gameId = 0;
    uint64_t sourceGeneration = 0;
    uint32_t cursor = 0;
    uint32_t eventCursor = 0;
    uint64_t digest = 0;
    std::vector<uint8_t> bytes;
    bool complete = false;
    bool integrityVerified = false;
};

enum class CacheAppendResult : uint8_t {
    Accepted,
    ExactDuplicate,
    NotConfigured,
    WrongStream,
    WrongEpoch,
    WrongContentIdentity,
    WrongGame,
    WrongSourceGeneration,
    Speculative,
    Gap,
    ConflictingDuplicate,
    DigestMismatch,
    MemoryLimit,
};

enum class CacheEventAppendResult : uint8_t {
    Accepted,
    ExactDuplicate,
    NotConfigured,
    WrongStream,
    WrongEpoch,
    WrongContentIdentity,
    WrongGame,
    WrongSourceGeneration,
    Speculative,
    TimelineUnbound,
    Gap,
    ConflictingDuplicate,
    BoundaryMismatch,
    TimelineMismatch,
    DigestMismatch,
    MemoryLimit,
};

enum class CacheCheckpointResult : uint8_t {
    Accepted,
    NotConfigured,
    WrongStream,
    WrongGame,
    WrongSourceGeneration,
    Incomplete,
    IntegrityUnverified,
    CursorUnavailable,
    DigestMismatch,
    MemoryLimit,
};

enum class CacheReadResult : uint8_t {
    Accepted,
    UnknownChild,
    ControlledResync,
    EventCatchupRequired,
    FutureCursor,
    InvalidRequest,
};

struct CacheChildStatus {
    NodeId childId = kInvalidNodeId;
    uint32_t acknowledgedCursor = 0;
    uint32_t acknowledgedEvent = 0;
    bool controlledResyncRequired = false;
};

struct ParentLeaseChildBinding {
    uint64_t authorityGeneration = 0;
    uint64_t childViewerId = 0;
    uint32_t childViewerGeneration = 0;
    RouteDescriptor childQspRoute{};
    uint64_t childEndpointBindingGeneration = 0;
    VerifiedChainCursor childRequestedCursor{};

    bool valid() const;
    bool operator==(const ParentLeaseChildBinding& other) const;
    bool operator!=(const ParentLeaseChildBinding& other) const {
        return !(*this == other);
    }
};

struct LeaseBinding {
    NodeId childId = kInvalidNodeId;
    NodeId parentId = kInvalidNodeId;
    uint64_t parentSourceGeneration = 0;
    uint64_t routeGeneration = 0;
    uint64_t expiryMs = 0;
    uint64_t replayNonce = 0;
    uint64_t macLow = 0;
    uint64_t macHigh = 0;
    ParentLeaseChildBinding childBinding{};
};

struct ContinuityProof {
    StreamCompatibilityIdentity stream{};
    LeaseBinding lease{};
    uint64_t gameId = 0;
    uint64_t sourceGeneration = 0;
    uint64_t parentGeneration = 0;
    uint32_t cacheFirstCursor = 0;
    uint32_t cacheNextCursor = 0;
    uint32_t cacheFirstEvent = 0;
    uint32_t cacheNextEvent = 0;
    uint32_t overlapCursor = 0;
    uint32_t overlapEvent = 0;
    uint64_t overlapDigest = 0;
    uint32_t checkpointCursor = 0;
    uint32_t checkpointEvent = 0;
    uint64_t checkpointDigest = 0;
    bool available = false;
};

class VerifiedRelayCache {
public:
    explicit VerifiedRelayCache(const ResilientLimits& limits = {});

    bool bind(const StreamCompatibilityIdentity& stream, uint64_t gameId,
              uint64_t initialDigest, uint64_t sourceGeneration);
    bool bind(const StreamCompatibilityIdentity& stream, uint64_t gameId,
              uint64_t initialDigest, uint64_t sourceGeneration,
              uint32_t firstTimelineFrame);
    bool configureEventTimeline(uint32_t firstTimelineFrame);
    void reset();
    bool configured() const { return _configured; }
    const VerifiedCacheRange& range() const { return _range; }
    const VerifiedEventRange& eventRange() const { return _eventRange; }
    VerifiedChainCursor frontier() const {
        return VerifiedChainCursor{
            _range.nextCursor, _eventRange.nextEvent, _nextDigest};
    }
    uint64_t latestDigest() const { return _nextDigest; }
    uint64_t sourceGeneration() const { return _sourceGeneration; }
    size_t recordCount() const { return _records.size(); }
    size_t eventCount() const { return _events.size(); }
    size_t checkpointCount() const { return _checkpoints.size(); }
    size_t retainedBytes() const;

    CacheAppendResult append(const RelayRecordEnvelope& envelope);
    CacheEventAppendResult appendEvent(const RelayEventEnvelope& envelope);
    CacheCheckpointResult addCheckpoint(const VerifiedCheckpoint& checkpoint);

    bool attachChild(NodeId childId, uint32_t acknowledgedCursor,
                     uint32_t acknowledgedEvent,
                     uint64_t acknowledgedDigest);
    bool attachChild(NodeId childId, uint32_t acknowledgedCursor,
                     uint32_t acknowledgedEvent);
    bool attachChild(NodeId childId, uint32_t acknowledgedCursor);
    bool detachChild(NodeId childId);
    bool acknowledgeChild(NodeId childId, uint32_t nextCursor,
                           uint32_t nextEvent, uint64_t acknowledgedDigest);
    bool acknowledgeChild(NodeId childId, uint32_t nextCursor,
                           uint32_t nextEvent);
    bool acknowledgeChild(NodeId childId, uint32_t nextCursor);
    bool canAcknowledgeChild(NodeId childId, uint32_t nextCursor,
                             uint32_t nextEvent,
                             uint64_t acknowledgedDigest) const;
    const CacheChildStatus* child(NodeId childId) const;

    CacheReadResult readForChild(NodeId childId, uint32_t startCursor,
                                 size_t maxRecords,
                                 std::vector<VerifiedRelayRecord>& out) const;
    CacheReadResult readEventsForChild(NodeId childId,
                                       uint32_t startEvent,
                                       size_t maxEvents,
                                       std::vector<VerifiedRelayEvent>& out) const;
    bool digestAt(uint32_t cursor, uint64_t& digest) const;
    bool digestAt(uint32_t recordCursor, uint32_t eventCursor,
                  uint64_t& digest) const;
    bool eventCursorAt(uint32_t recordCursor, uint32_t& out) const;
    bool continuityProof(uint32_t exactNextCursor,
                         uint32_t overlapRecords,
                         ContinuityProof& out) const;
    bool continuityProof(uint32_t exactNextCursor,
                         uint32_t exactNextEvent,
                         uint32_t overlapRecords,
                         ContinuityProof& out) const;
    bool adoptSourceGeneration(uint64_t sourceGeneration,
                               uint32_t exactNextCursor,
                               uint32_t overlapCursor,
                               uint64_t overlapDigest);
    bool adoptSourceGeneration(uint64_t sourceGeneration,
                               uint32_t exactNextCursor,
                               uint32_t exactNextEvent,
                               uint32_t overlapCursor,
                               uint32_t overlapEvent,
                               uint64_t overlapDigest);
    bool canAdoptSourceGeneration(uint64_t sourceGeneration,
                                  uint32_t exactNextCursor,
                                  uint32_t overlapCursor,
                                  uint64_t overlapDigest) const;
    bool canAdoptSourceGeneration(uint64_t sourceGeneration,
                                  uint32_t exactNextCursor,
                                  uint32_t exactNextEvent,
                                  uint32_t overlapCursor,
                                  uint32_t overlapEvent,
                                  uint64_t overlapDigest) const;
    bool newestCheckpointIdentity(uint32_t maxCursor, uint32_t maxEvent,
                                  uint32_t& cursor, uint32_t& eventCursor,
                                  uint64_t& digest) const;

private:
    struct StoredCheckpoint {
        uint32_t cursor = 0;
        uint32_t eventCursor = 0;
        uint64_t digest = 0;
        uint64_t sourceGeneration = 0;
        std::vector<uint8_t> bytes;
    };

    CacheAppendResult identityResult(
        const StreamCompatibilityIdentity& candidate) const;
    CacheEventAppendResult eventIdentityResult(
        const StreamCompatibilityIdentity& candidate) const;
    uint32_t eventCursorBefore(uint32_t recordCursor) const;
    uint32_t eventCursorAtOrBefore(uint32_t recordCursor) const;
    bool evictOldestRecordBoundary();
    void trimEventsToRecordRange();
    void evictToBounds();
    void trimCheckpointsToBounds();
    void markLaggingChildrenForResync();

    bool _configured = false;
    ResilientLimits _limits{};
    StreamCompatibilityIdentity _stream{};
    uint64_t _gameId = 0;
    uint64_t _sourceGeneration = 0;
    VerifiedCacheRange _range{};
    VerifiedEventRange _eventRange{};
    uint64_t _firstDigest = 0;
    uint64_t _nextDigest = 0;
    uint32_t _firstTimelineFrame = 0;
    bool _eventTimelineBound = false;
    std::vector<VerifiedRelayRecord> _records;
    std::vector<VerifiedRelayEvent> _events;
    std::vector<StoredCheckpoint> _checkpoints;
    std::vector<CacheChildStatus> _children;
};

using RelaySocketHandle = uintptr_t;
constexpr RelaySocketHandle kInvalidRelaySocket = 0;
using RelaySocketCloseFn = void (*)(RelaySocketHandle socket, void* context);

class RelaySocketOwnership {
public:
    RelaySocketOwnership() = default;
    ~RelaySocketOwnership();
    RelaySocketOwnership(const RelaySocketOwnership&) = delete;
    RelaySocketOwnership& operator=(const RelaySocketOwnership&) = delete;

    bool adopt(RelaySocketHandle upstream, RelaySocketHandle downstream,
               RelaySocketCloseFn closeFn, void* closeContext);
    void close();
    bool configured() const { return _upstream != kInvalidRelaySocket; }
    RelaySocketHandle upstream() const { return _upstream; }
    RelaySocketHandle downstream() const { return _downstream; }

private:
    RelaySocketHandle _upstream = kInvalidRelaySocket;
    RelaySocketHandle _downstream = kInvalidRelaySocket;
    RelaySocketCloseFn _closeFn = nullptr;
    void* _closeContext = nullptr;
};

class VerifiedRelayDownstream {
public:
    explicit VerifiedRelayDownstream(const ResilientLimits& limits = {});
    ~VerifiedRelayDownstream();

    bool start(const StreamCompatibilityIdentity& stream, uint64_t gameId,
               uint64_t initialDigest, uint64_t sourceGeneration,
               RelaySocketHandle upstream, RelaySocketHandle downstream,
               RelaySocketCloseFn closeFn, void* closeContext);
    bool startWithCache(VerifiedRelayCache& cache,
                        RelaySocketHandle upstream,
                        RelaySocketHandle downstream,
                        RelaySocketCloseFn closeFn, void* closeContext);
    void stop();
    bool running() const { return _running; }
    bool borrowsCache() const { return _borrowedCache; }
    const RelaySocketOwnership& sockets() const { return _sockets; }
    VerifiedRelayCache& cache() { return *_cache; }
    const VerifiedRelayCache& cache() const { return *_cache; }

    CacheAppendResult acceptUpstream(const RelayRecordEnvelope& envelope);
    CacheEventAppendResult acceptUpstreamEvent(
        const RelayEventEnvelope& envelope);
    CacheReadResult serveChild(NodeId childId, uint32_t startCursor,
                               size_t maxRecords,
                               std::vector<VerifiedRelayRecord>& out) const;
    CacheReadResult serveChildEvents(NodeId childId, uint32_t startEvent,
                                     size_t maxEvents,
                                     std::vector<VerifiedRelayEvent>& out) const;

private:
    bool _running = false;
    bool _borrowedCache = false;
    VerifiedRelayCache _ownedCache;
    VerifiedRelayCache* _cache = &_ownedCache;
    RelaySocketOwnership _sockets;
};

constexpr size_t kMaxLeaseRoutes = 3;
constexpr size_t kMaxLeaseAncestry = kMaxTopologyAncestry;

// The HMAC covers every field below except macLow/macHigh; its key must never be a viewer cookie or a simulation-content identity.
struct ParentLease {
    StreamCompatibilityIdentity stream{};
    NodeId childId = kInvalidNodeId;
    NodeId parentId = kInvalidNodeId;
    ParentLeaseChildBinding childBinding{};
    uint8_t parentDepth = 0;
    uint64_t parentSourceGeneration = 0;
    uint64_t routeGeneration = 0;
    std::array<RouteDescriptor, kMaxLeaseRoutes> routes{};
    uint8_t routeCount = 0;
    VerifiedCacheRange cacheRange{};
    uint32_t latestVerifiedCursor = 0;
    uint32_t childCapacity = 0;
    uint64_t expiryMs = 0;
    uint64_t replayNonce = 0;
    std::array<NodeId, kMaxLeaseAncestry> ancestry{};
    uint8_t ancestryCount = 0;
    uint64_t macLow = 0;
    uint64_t macHigh = 0;
};

struct ParentLeaseExpectation {
    StreamCompatibilityIdentity stream{};
    NodeId expectedChildId = kInvalidNodeId;
    NodeId expectedParentId = kInvalidNodeId;
    ParentLeaseChildBinding expectedChildBinding{};
    uint64_t nowMs = 0;
    uint64_t minimumSourceGeneration = 0;
    uint64_t minimumRouteGeneration = 0;
    uint32_t requiredCursor = 0;
    uint8_t maxActiveDepth = 0;
    bool parentHasCapacity = false;
    bool loopFree = false;
};

enum class ParentLeaseVerdict : uint8_t {
    Accepted,
    Malformed,
    BadMac,
    Expired,
    WrongChild,
    WrongParent,
    ChildBindingMismatch,
    WrongStream,
    StaleSourceGeneration,
    StaleRouteGeneration,
    CursorUnavailable,
    CapacityReached,
    LoopRejected,
    DepthExceeded,
    Replayed,
    ReplayMemoryBound,
    TopologyRejected,
};

class ParentLeaseRegistry {
public:
    ParentLeaseRegistry(const SpectatorGrantKey& key,
                        size_t maxRememberedNonces = 256);

    bool issue(ParentLease& lease) const;
    ParentLeaseVerdict verify(const ParentLease& lease,
                              const ParentLeaseExpectation& expectation) const;
    ParentLeaseVerdict verifyContinuity(
        const ParentLease& lease,
        const ParentLeaseExpectation& expectation) const;
    ParentLeaseVerdict redeem(const ParentLease& lease,
                              const ParentLeaseExpectation& expectation);
    bool wasRedeemed(uint64_t replayNonce) const;
    void expire(uint64_t nowMs);
    size_t rememberedNonceCount() const { return _redeemed.size(); }

private:
    struct RedeemedNonce {
        uint64_t nonce = 0;
        uint64_t expiryMs = 0;
    };

    SpectatorGrantKey _key{};
    size_t _maxRememberedNonces = 0;
    std::vector<RedeemedNonce> _redeemed;
};

struct NodeAdvertisement {
    NodeId nodeId = kInvalidNodeId;
    NodeKind kind = NodeKind::SpectatorRelay;
    StreamCompatibilityIdentity stream{};
    RouteDescriptor route{};
    RouteDescriptor controlRoute{};

    const RouteDescriptor& resolvedControlRoute() const {
        return controlRoute.valid() ? controlRoute : route;
    }

    NodeId activeParent = kInvalidNodeId;
    uint8_t depth = 0;
    std::array<NodeId, kMaxTopologyAncestry> ancestry{};
    uint8_t ancestryCount = 0;
    uint16_t capabilityModelRevision = kResilientCapabilityModelRevision;
    uint32_t capabilities = 0;
    size_t childCapacity = 0;
    size_t childCount = 0;
    uint32_t uploadCapacity = 0;
    uint32_t uploadInUse = 0;
    uint32_t rttMs = 0;
    uint32_t lossPerMille = 0;
    uint32_t cacheFirstCursor = 0;
    uint32_t cacheNextCursor = 0;
    uint64_t sourceGeneration = 0;
    uint64_t routeGeneration = 0;
    uint64_t lastProgressMs = 0;
    uint64_t leaseExpiryMs = 0;
    bool reachable = false;
    bool draining = false;
};

struct TopologyNodeView {
    NodeAdvertisement advertisement{};
    NodeId activeParent = kInvalidNodeId;
    uint8_t depth = 0;
    std::array<NodeId, kMaxLeaseAncestry> ancestry{};
    size_t ancestryCount = 0;
    std::vector<NodeId> children;
};

enum class NodeRegistrationResult : uint8_t {
    Accepted,
    UpdatedRoute,
    InvalidNode,
    InvalidIdentity,
    UnsupportedCapabilityModel,
    CapacityReached,
    NodeIdConflict,
};

enum class TopologyAttachResult : uint8_t {
    Attached,
    UnknownNode,
    StreamMismatch,
    SelfParent,
    ParentNotRoot,
    ChildIsRoot,
    ParentUnreachable,
    ParentDraining,
    ParentAtCapacity,
    DepthExceeded,
    ParentIsDescendant,
    StaleSourceGeneration,
    StaleRouteGeneration,
    AlreadyAttachedElsewhere,
};

struct ParentSelectionRequest {
    NodeId childId = kInvalidNodeId;
    uint32_t requiredCursor = 0;
    uint64_t minimumSourceGeneration = 0;
    uint64_t minimumRouteGeneration = 0;
    size_t maxOffers = 0;
};

struct ParentSelection {
    NodeId parentId = kInvalidNodeId;
    RouteDescriptor route{};
    bool branchDiverse = false;
    bool trafficRelayFallback = false;
};

class ResilientTopologyCoordinator {
public:
    ResilientTopologyCoordinator(const StreamCompatibilityIdentity& stream,
                                 const ResilientLimits& limits = {});

    bool configured() const { return _configured; }
    const StreamCompatibilityIdentity& stream() const { return _stream; }
    const ResilientLimits& limits() const { return _limits; }

    NodeRegistrationResult registerNode(const NodeAdvertisement& advertisement);
    bool refreshRoute(NodeId nodeId, const RouteDescriptor& route,
                      uint64_t routeGeneration);
    bool setDraining(NodeId nodeId, bool draining);

    bool seedExternalAncestorPrefix(
        NodeId nodeId, uint8_t depth,
        const std::array<NodeId, kMaxLeaseAncestry>& ancestry,
        uint8_t ancestryCount);

    TopologyAttachResult attachRootChild(NodeId rootId, NodeId childId);
    TopologyAttachResult attachChild(NodeId parentId, NodeId childId);
    TopologyAttachResult replaceParentAfterContinuity(NodeId parentId,
                                                      NodeId childId);
    bool detach(NodeId childId);

    std::vector<ParentSelection> selectStandbyParents(
        const ParentSelectionRequest& request) const;
    bool makeUnsignedParentOffer(NodeId childId, NodeId parentId,
                                  uint32_t requiredCursor, uint64_t expiryMs,
                                  uint64_t replayNonce, ParentLease& out) const;
    bool makeUnsignedParentOffer(NodeId childId, NodeId parentId,
                                  uint32_t requiredCursor, uint64_t expiryMs,
                                  uint64_t replayNonce,
                                  const ParentLeaseChildBinding& childBinding,
                                  ParentLease& out) const;
    ParentLeaseVerdict redeemAndAttach(ParentLeaseRegistry& registry,
                                       const ParentLease& lease,
                                       uint64_t nowMs,
                                       uint32_t requiredCursor);
    ParentLeaseVerdict redeemAndAttach(
        ParentLeaseRegistry& registry, const ParentLease& lease,
        uint64_t nowMs, uint32_t requiredCursor,
        const ParentLeaseChildBinding& expectedChildBinding);
    ParentLeaseVerdict validateStandbyLease(const ParentLeaseRegistry& registry,
                                             const ParentLease& lease,
                                             uint64_t nowMs,
                                             uint32_t requiredCursor) const;
    ParentLeaseVerdict validateStandbyLease(
        const ParentLeaseRegistry& registry, const ParentLease& lease,
        uint64_t nowMs, uint32_t requiredCursor,
        const ParentLeaseChildBinding& expectedChildBinding) const;
    ParentLeaseVerdict redeemAndReparentAfterContinuity(
        ParentLeaseRegistry& registry, const ParentLease& lease,
        uint64_t nowMs, uint32_t requiredCursor);
    ParentLeaseVerdict redeemAndReparentAfterContinuity(
        ParentLeaseRegistry& registry, const ParentLease& lease,
        uint64_t nowMs, uint32_t requiredCursor,
        const ParentLeaseChildBinding& expectedChildBinding);

    const TopologyNodeView* find(NodeId nodeId) const;
    size_t nodeCount() const { return _nodes.size(); }
    size_t rootCount() const;
    size_t rootChildCount(NodeId rootId) const;

private:
    bool isRoot(const TopologyNodeView& node) const;
    size_t childLimit(const TopologyNodeView& node) const;
    TopologyAttachResult attachmentEligibility(NodeId parentId, NodeId childId,
                                               bool requireRoot,
                                               bool permitReparent = false) const;
    bool wouldCreateLoop(const TopologyNodeView& parent,
                         NodeId childId) const;
    NodeId branchRoot(const TopologyNodeView& node) const;
    size_t subtreeHeight(const TopologyNodeView& node) const;
    void rewriteDescendantTopology(TopologyNodeView& node);
    TopologyNodeView* findMutable(NodeId nodeId);
    bool _configured = false;
    StreamCompatibilityIdentity _stream{};
    ResilientLimits _limits{};
    std::vector<TopologyNodeView> _nodes;
};

enum class RelayRecoveryState : uint8_t {
    Stable,
    Suspect,
    RacingStandbys,
    VerifyingContinuity,
    CommittedToNewParent,
    DegradedBufferOnly,
    RecoveryFailed,
    Draining,
    Closed,
};

enum class UpstreamFailureReason : uint8_t {
    TransportClosed,
    HeartbeatTimeout,
    NoVerifiedProgress,
    DrainNotice,
    LeaseExpired,
    StreamMismatch,
    DigestMismatch,
    ParentRecoveryFailure,
    RouteFatal,
};

struct ContinuityRequest {
    StreamCompatibilityIdentity stream{};
    LeaseBinding lease{};
    uint64_t gameId = 0;
    uint64_t expectedSourceGeneration = 0;
    uint64_t parentGeneration = 0;
    uint32_t exactNextCursor = 0;
    uint32_t exactNextEvent = 0;
    uint32_t overlapCursor = 0;
    uint32_t overlapEvent = 0;
    uint64_t overlapDigest = 0;
    uint32_t checkpointCursor = 0;
    uint32_t checkpointEvent = 0;
    uint64_t checkpointDigest = 0;
};

struct RecoveryAttemptView {
    NodeId parentId = kInvalidNodeId;
    RouteDescriptor route{};
    bool continuityVerified = false;
    bool cancelled = false;
    bool quarantined = false;
};

struct ChildRecoveryNotice {
    RelayRecoveryState state = RelayRecoveryState::Closed;
    uint32_t latestVerifiedCursor = 0;
    uint32_t cacheRunwayRecords = 0;
    uint64_t recoveryGeneration = 0;
    bool childFallbackPermitted = false;
};

struct DrainNotice {
    uint64_t shutdownDeadlineMs = 0;
    uint32_t lastAvailableCursor = 0;
    uint32_t cacheEndCursor = 0;
    uint64_t sourceGeneration = 0;
    uint64_t routeGeneration = 0;
    std::vector<NodeId> recommendedReplacementParents;
    std::vector<NodeId> activeChildIds;
};

class ResilientRelayNode {
public:
    ResilientRelayNode(NodeId nodeId,
                       const StreamCompatibilityIdentity& stream,
                       uint64_t gameId, uint64_t initialDigest,
                       ResilientTopologyCoordinator& topology,
                       ParentLeaseRegistry& leases,
                       const ResilientLimits& limits = {});

    bool start(uint64_t sourceGeneration);
    bool configureEventTimeline(uint32_t firstTimelineFrame);
    bool setInitialParent(NodeId parentId, uint64_t parentGeneration,
                          uint64_t routeGeneration);
    RelayRecoveryState state() const { return _state; }
    NodeId activeParent() const { return _activeParent; }
    uint64_t parentGeneration() const { return _parentGeneration; }
    uint64_t recoveryGeneration() const { return _recoveryGeneration; }
    const VerifiedRelayCache& cache() const { return _cache; }
    VerifiedRelayCache& cache() { return _cache; }

    bool attachChild(NodeId childId, uint32_t acknowledgedCursor);
    bool attachChild(NodeId childId, uint32_t acknowledgedCursor,
                     uint32_t acknowledgedEvent,
                     uint64_t acknowledgedDigest);
    bool retireChild(NodeId childId);
    size_t childCount() const { return _children.size(); }
    const std::vector<NodeId>& children() const { return _children; }

    bool noteUpstreamFailure(UpstreamFailureReason reason, uint64_t nowMs);
    size_t beginRecovery(const std::vector<ParentLease>& offers,
                          uint32_t overlapRecords, uint64_t nowMs);
    size_t beginRecovery(const std::vector<ParentLease>& offers,
                         uint32_t overlapRecords, uint64_t nowMs,
                         const ParentLeaseChildBinding& expectedChildBinding);
    bool continuityRequest(NodeId parentId, ContinuityRequest& out) const;
    bool continuityRequest(ContinuityRequest& out) const;
    bool submitContinuityProof(NodeId parentId,
                               const ContinuityProof& proof);
    bool commitVerifiedParent(NodeId parentId, uint64_t nowMs);
    void noteVerifiedProgress();
    void tick(uint64_t nowMs);
    void markCacheExhausted();
    bool childFallbackPermitted() const;
    ChildRecoveryNotice childRecoveryNotice() const;

    bool beginDrain(uint64_t shutdownDeadlineMs,
                    const std::vector<NodeId>& replacementParents,
                    uint64_t routeGeneration);
    bool drainNotice(DrainNotice& out) const;
    void close();

    size_t recoveryAttemptCount() const { return _attempts.size(); }
    size_t cancelledAttemptCount() const;
    size_t quarantinedAttemptCount() const;
    bool recoveryFailureAnnounced() const { return _recoveryFailureAnnounced; }

private:
    struct RecoveryAttempt {
        ParentLease lease{};
        ContinuityProof proof{};
        bool continuityVerified = false;
        bool cancelled = false;
        bool quarantined = false;
    };

    RecoveryAttempt* findAttempt(NodeId parentId);
    const RecoveryAttempt* findAttempt(NodeId parentId) const;
    void quarantine(RecoveryAttempt& attempt);
    bool hasUsableAttempt() const;
    void cancelLosingAttempts(NodeId winnerId);

    NodeId _nodeId = kInvalidNodeId;
    StreamCompatibilityIdentity _stream{};
    uint64_t _gameId = 0;
    uint64_t _initialDigest = 0;
    ResilientLimits _limits{};
    ResilientTopologyCoordinator* _topology = nullptr;
    ParentLeaseRegistry* _leases = nullptr;
    VerifiedRelayCache _cache;
    RelayRecoveryState _state = RelayRecoveryState::Closed;
    UpstreamFailureReason _lastFailure = UpstreamFailureReason::TransportClosed;
    NodeId _activeParent = kInvalidNodeId;
    uint64_t _parentGeneration = 0;
    uint64_t _routeGeneration = 0;
    uint64_t _recoveryGeneration = 0;
    uint64_t _recoveryDeadlineMs = 0;
    uint64_t _drainDeadlineMs = 0;
    uint32_t _requestedOverlapRecords = 0;
    ParentLeaseChildBinding _expectedChildBinding{};
    ContinuityProof _localContinuity{};
    bool _cacheExhausted = false;
    bool _recoveryFailureAnnounced = false;
    std::vector<NodeId> _children;
    std::vector<NodeId> _drainReplacementParents;
    std::vector<RecoveryAttempt> _attempts;
};

}
