#include "client/spectator/ResilientSpectatorNodeRuntime.h"

#include "input/InputActions.h"

#include <algorithm>
#include <limits>
#include <new>

namespace qclient::spectator::resilient {
namespace {

bool hasNonzeroKey(const qnetplay::spectator::SpectatorGrantKey& key) {
    return std::any_of(key.begin(), key.end(),
                       [](const uint8_t byte) { return byte != 0u; });
}

bool ingressModeValid(const NodeRuntimeIngressMode mode) {
    return mode == NodeRuntimeIngressMode::SignedQsp
        || mode == NodeRuntimeIngressMode::CanonicalHostAttested;
}

bool sameIdentity(
    const qnetplay::compat::CompatibilityIdentityWire& left,
    const qnetplay::compat::CompatibilityIdentityWire& right) {
    return qnetplay::compat::semanticallyEqual(left, right);
}

bool sourceResultAccepted(const qnetplay::spectator::ArchiveResult result) {
    return result == qnetplay::spectator::ArchiveResult::Ok
        || result == qnetplay::spectator::ArchiveResult::ExactDuplicate;
}

bool cacheResultAccepted(const model::CacheAppendResult result) {
    return result == model::CacheAppendResult::Accepted
        || result == model::CacheAppendResult::ExactDuplicate;
}

bool cacheEventResultAccepted(const model::CacheEventAppendResult result) {
    return result == model::CacheEventAppendResult::Accepted
        || result == model::CacheEventAppendResult::ExactDuplicate;
}

bool sameRoute(const model::RouteDescriptor& left,
               const model::RouteDescriptor& right) {
    return left.kind == right.kind && left.ipv4BE == right.ipv4BE
        && left.port == right.port && left.generation == right.generation
        && left.routeProved == right.routeProved;
}

bool emptyRoute(const model::RouteDescriptor& route) {
    return sameRoute(route, model::RouteDescriptor{});
}

bool samePhysicalEndpoint(const model::RouteDescriptor& left,
                          const model::RouteDescriptor& right) {
    return left.sameEndpoint(right);
}

bool validExternalAncestorPrefix(
    const ResilientSpectatorNodeRuntimeConfig& config) {
    const size_t count = config.initialParentAncestryCount;
    if (count != config.initialParentDepth
        || count > config.initialParentAncestry.size()
        || config.initialParentDepth >= config.limits.maxActiveDepth) {
        return false;
    }
    const model::NodeId parent = config.initialParentAdvertisement.nodeId;
    const model::NodeId local = config.localAdvertisement.nodeId;
    for (size_t index = 0u; index < config.initialParentAncestry.size();
         ++index) {
        const model::NodeId ancestor = config.initialParentAncestry[index];
        if (index >= count) {
            if (ancestor != model::kInvalidNodeId) return false;
            continue;
        }
        if (ancestor == model::kInvalidNodeId || ancestor == parent
            || ancestor == local) {
            return false;
        }
        for (size_t previous = 0u; previous < index; ++previous) {
            if (config.initialParentAncestry[previous] == ancestor)
                return false;
        }
    }
    return true;
}

bool deadlineAfter(const uint64_t nowMs, const uint64_t durationMs,
                   uint64_t& out) {
    if (durationMs == 0u
        || nowMs > (std::numeric_limits<uint64_t>::max)() - durationMs) {
        return false;
    }
    out = nowMs + durationMs;
    return true;
}

bool deadlineReached(const uint64_t nowMs, const uint64_t deadlineMs) {
    return deadlineMs == 0u || nowMs >= deadlineMs;
}

bool intervalElapsed(const uint64_t nowMs, const uint64_t sinceMs,
                     const uint64_t intervalMs) {
    return intervalMs == 0u
        || (nowMs >= sinceMs && nowMs - sinceMs >= intervalMs);
}

struct ChildPacketIdentity {
    qnetplay::spectator::MessageType type =
        qnetplay::spectator::MessageType::Reject;
    qnetplay::spectator::ViewerId viewerId = 0;
    uint32_t generation = 0;
};

bool decodeChildPacketIdentity(
    const qnetplay::spectator::Datagram& datagram,
    ChildPacketIdentity& out) {
    ChildPacketIdentity decoded{};
    decoded.type = datagram.header.type;
    switch (decoded.type) {
    case qnetplay::spectator::MessageType::Hello: {
        qnetplay::spectator::HelloPayload payload{};
        if (!qnetplay::spectator::decodePayload(datagram.payload, payload))
            return false;
        decoded.viewerId = payload.viewerId;
        decoded.generation = payload.generation;
        break;
    }
    case qnetplay::spectator::MessageType::InitAck: {
        qnetplay::spectator::InitAckPayload payload{};
        if (!qnetplay::spectator::decodePayload(datagram.payload, payload))
            return false;
        decoded.viewerId = payload.viewerId;
        decoded.generation = payload.generation;
        break;
    }
    case qnetplay::spectator::MessageType::Ack: {
        qnetplay::spectator::AckPayload payload{};
        if (!qnetplay::spectator::decodePayload(datagram.payload, payload))
            return false;
        decoded.viewerId = payload.viewerId;
        decoded.generation = payload.generation;
        break;
    }
    case qnetplay::spectator::MessageType::TerminalAck: {
        qnetplay::spectator::TerminalAckPayload payload{};
        if (!qnetplay::spectator::decodePayload(datagram.payload, payload))
            return false;
        decoded.viewerId = payload.viewerId;
        decoded.generation = payload.generation;
        break;
    }
    case qnetplay::spectator::MessageType::Detach: {
        qnetplay::spectator::DetachPayload payload{};
        if (!qnetplay::spectator::decodePayload(datagram.payload, payload))
            return false;
        decoded.viewerId = payload.viewerId;
        decoded.generation = payload.generation;
        break;
    }
    case qnetplay::spectator::MessageType::Migrate:
    default:
        return false;
    }
    out = decoded;
    return true;
}

}

struct QtaAuthorityContext::NonceReservation {
    std::weak_ptr<QtaAuthorityContext> owner;
    uint64_t authorityGeneration = 0u;
    uint64_t replayNonce = 0u;
    uint64_t reservationId = 0u;

    ~NonceReservation() {
        const std::shared_ptr<QtaAuthorityContext> context = owner.lock();
        if (context != nullptr) {
            context->cancelReservation(authorityGeneration, replayNonce,
                                       reservationId);
        }
    }
};

QtaAuthorityContext::QtaAuthorityContext(
    const uint64_t authorityGeneration, const uint64_t localExpiryDeadlineMs,
    const size_t maximumRedemptions)
    : _authorityGeneration(authorityGeneration),
      _localExpiryDeadlineMs(localExpiryDeadlineMs),
      _maximumRedemptions(maximumRedemptions) {
    _redeemed.reserve(maximumRedemptions);
    _reserved.reserve(maximumRedemptions);
}

std::shared_ptr<QtaAuthorityContext> QtaAuthorityContext::create(
    const uint64_t authorityGeneration, const uint64_t localExpiryDeadlineMs,
    const size_t maximumRedemptions) {
    if (authorityGeneration == 0u || localExpiryDeadlineMs == 0u
        || maximumRedemptions == 0u) {
        return {};
    }
    try {
        return std::shared_ptr<QtaAuthorityContext>(new QtaAuthorityContext(
            authorityGeneration, localExpiryDeadlineMs, maximumRedemptions));
    } catch (const std::bad_alloc&) {
        return {};
    }
}

bool QtaAuthorityContext::activeFor(const uint64_t authorityGeneration,
                                    const uint64_t nowMs) const {
    return authorityGeneration != 0u
        && authorityGeneration == _authorityGeneration
        && !_revoked.load(std::memory_order_acquire)
        && nowMs < _localExpiryDeadlineMs;
}

std::shared_ptr<void> QtaAuthorityContext::reserve(
    const uint64_t authorityGeneration, const uint64_t replayNonce,
    const uint64_t nowMs) {
    if (replayNonce == 0u || !activeFor(authorityGeneration, nowMs))
        return {};
    std::lock_guard<std::mutex> lock(_redemptionMutex);
    if (!activeFor(authorityGeneration, nowMs)
        || _redeemed.size() + _reserved.size() >= _maximumRedemptions
        || std::any_of(_redeemed.begin(), _redeemed.end(),
                       [authorityGeneration, replayNonce](
                           const RedeemedNonce& redeemed) {
                           return redeemed.authorityGeneration
                                   == authorityGeneration
                               && redeemed.replayNonce == replayNonce;
                       })
        || std::any_of(_reserved.begin(), _reserved.end(),
                       [authorityGeneration, replayNonce](
                           const ReservedNonce& reserved) {
                           return reserved.authorityGeneration
                                   == authorityGeneration
                               && reserved.replayNonce == replayNonce;
                       })) {
        return {};
    }
    if (_nextReservationId == 0u) return {};
    const uint64_t reservationId = _nextReservationId++;
    try {
        _reserved.push_back(ReservedNonce{
            authorityGeneration, replayNonce, reservationId});
        std::shared_ptr<NonceReservation> reservation(
            new NonceReservation{weak_from_this(), authorityGeneration,
                                 replayNonce, reservationId});
        return std::shared_ptr<void>(std::move(reservation));
    } catch (const std::bad_alloc&) {
        const auto found = std::find_if(
            _reserved.begin(), _reserved.end(),
            [reservationId](const ReservedNonce& reserved) {
                return reserved.reservationId == reservationId;
            });
        if (found != _reserved.end()) _reserved.erase(found);
        return {};
    }
}

bool QtaAuthorityContext::commitReservation(
    const std::shared_ptr<void>& opaqueReservation,
    const uint64_t authorityGeneration, const uint64_t replayNonce,
    const model::NodeId childNodeId,
    const model::ParentLeaseChildBinding& childBinding,
    const uint64_t nowMs) {
    if (opaqueReservation == nullptr || replayNonce == 0u
        || childNodeId == model::kInvalidNodeId || !childBinding.valid()
        || !activeFor(authorityGeneration, nowMs)) {
        return false;
    }
    const std::shared_ptr<NonceReservation> reservation(
        opaqueReservation,
        static_cast<NonceReservation*>(opaqueReservation.get()));
    if (reservation == nullptr || reservation->owner.lock().get() != this
        || reservation->authorityGeneration != authorityGeneration
        || reservation->replayNonce != replayNonce
        || reservation->reservationId == 0u) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_redemptionMutex);
    if (!activeFor(authorityGeneration, nowMs)) return false;
    const auto pending = std::find_if(
        _reserved.begin(), _reserved.end(),
        [authorityGeneration, replayNonce, &reservation](
            const ReservedNonce& reserved) {
            return reserved.authorityGeneration == authorityGeneration
                && reserved.replayNonce == replayNonce
                && reserved.reservationId == reservation->reservationId;
        });
    if (pending == _reserved.end()
        || std::any_of(_redeemed.begin(), _redeemed.end(),
                       [authorityGeneration, replayNonce](
                           const RedeemedNonce& redeemed) {
                           return redeemed.authorityGeneration
                                   == authorityGeneration
                               && redeemed.replayNonce == replayNonce;
                       })) {
        return false;
    }
    try {
        _redeemed.push_back(RedeemedNonce{authorityGeneration, replayNonce,
                                          childNodeId, childBinding, false});
    } catch (const std::bad_alloc&) {
        return false;
    }
    _reserved.erase(pending);
    return true;
}

void QtaAuthorityContext::cancelReservation(
    const uint64_t authorityGeneration, const uint64_t replayNonce,
    const uint64_t reservationId) {
    if (authorityGeneration == 0u || replayNonce == 0u
        || reservationId == 0u) {
        return;
    }
    std::lock_guard<std::mutex> lock(_redemptionMutex);
    const auto pending = std::find_if(
        _reserved.begin(), _reserved.end(),
        [authorityGeneration, replayNonce, reservationId](
            const ReservedNonce& reserved) {
            return reserved.authorityGeneration == authorityGeneration
                && reserved.replayNonce == replayNonce
                && reserved.reservationId == reservationId;
        });
    if (pending != _reserved.end()) _reserved.erase(pending);
}

bool QtaAuthorityContext::markRetired(
    const uint64_t authorityGeneration, const uint64_t replayNonce,
    const model::NodeId childNodeId,
    const model::ParentLeaseChildBinding& childBinding) {
    if (authorityGeneration == 0u || replayNonce == 0u
        || childNodeId == model::kInvalidNodeId || !childBinding.valid()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_redemptionMutex);
    const auto redeemed = std::find_if(
        _redeemed.begin(), _redeemed.end(),
        [authorityGeneration, replayNonce, childNodeId, &childBinding](
            const RedeemedNonce& entry) {
            return entry.authorityGeneration == authorityGeneration
                && entry.replayNonce == replayNonce
                && entry.childNodeId == childNodeId
                && entry.childBinding == childBinding;
        });
    if (redeemed == _redeemed.end()) return false;
    redeemed->retired = true;
    return true;
}

bool QtaAuthorityContext::matchesRetired(
    const uint64_t authorityGeneration, const uint64_t replayNonce,
    const model::NodeId childNodeId,
    const model::ParentLeaseChildBinding& childBinding) const {
    if (authorityGeneration == 0u || replayNonce == 0u
        || childNodeId == model::kInvalidNodeId || !childBinding.valid()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_redemptionMutex);
    return std::any_of(
        _redeemed.begin(), _redeemed.end(),
        [authorityGeneration, replayNonce, childNodeId, &childBinding](
            const RedeemedNonce& entry) {
            return entry.retired
                && entry.authorityGeneration == authorityGeneration
                && entry.replayNonce == replayNonce
                && entry.childNodeId == childNodeId
                && entry.childBinding == childBinding;
        });
}

void QtaAuthorityContext::revoke() {
    _revoked.store(true, std::memory_order_release);
}

QtaVerifiedParentLease::Claims QtaVerifiedParentLease::takeClaims(
    const uint64_t nowMs) && {
    Claims claims{};
    if (_consumed) return claims;
    _consumed = true;

    std::shared_ptr<QtaAuthorityContext> authority = std::move(_authority);
    model::ParentLease lease = std::move(_lease);
    if (authority == nullptr) {
        return claims;
    }
    std::shared_ptr<void> reservation = authority->reserve(
        lease.childBinding.authorityGeneration, lease.replayNonce, nowMs);
    if (reservation == nullptr) return claims;
    claims.lease = std::move(lease);
    claims.authority = std::move(authority);
    claims.reservation = std::move(reservation);
    return claims;
}

bool QtaVerifiedParentLease::commitClaims(
    Claims& claims, const uint64_t nowMs, const model::NodeId childNodeId,
    const model::ParentLeaseChildBinding& binding) {
    if (!claims.present()) return false;
    const bool committed = claims.authority->commitReservation(
        claims.reservation, binding.authorityGeneration,
        claims.lease.replayNonce, childNodeId, binding, nowMs);
    if (committed) claims.reservation.reset();
    return committed;
}

QtaVerifiedChildRetirement::Claims
QtaVerifiedChildRetirement::takeClaims() && {
    Claims claims{};
    if (_consumed) return claims;
    _consumed = true;
    claims.childNodeId = _childNodeId;
    claims.replayNonce = _replayNonce;
    claims.childBinding = std::move(_childBinding);
    claims.authority = std::move(_authority);
    _childNodeId = model::kInvalidNodeId;
    _replayNonce = 0u;
    return claims;
}

bool QtaVerifiedCanonicalIngress::matchesControl(
    const ResilientSpectatorNodeRuntimeConfig& config,
    const uint64_t nowMs) const {
    return _authority != nullptr
        && _authority->activeFor(config.authorityGeneration, nowMs)
        && _stream == config.stream
        && _qspIdentity == config.qspIdentity
        && _localNodeId == config.localAdvertisement.nodeId
        && sameRoute(_localQspRoute, config.localAdvertisement.route)
        && _localRouteGeneration == config.localAdvertisement.routeGeneration
        && _parentNodeId == config.initialParentAdvertisement.nodeId
        && sameRoute(_parentQspRoute,
                     config.initialParentAdvertisement.route)
        && _parentSourceGeneration
            == config.initialParentAdvertisement.sourceGeneration
        && _parentGeneration == config.initialParentGeneration
        && _parentRouteGeneration == config.initialRouteGeneration
        && _sourceGeneration == config.sourceGeneration;
}

bool QtaVerifiedCanonicalIngress::matches(
    const ResilientSpectatorNodeRuntimeConfig& config,
    const qnetplay::spectator::SpectatorViewer& verifiedViewer,
    const uint64_t nowMs) const {
    return _verifiedViewer == &verifiedViewer
        && matchesControl(config, nowMs);
}

const char* nodeRuntimeResultName(const NodeRuntimeResult result) {
    switch (result) {
    case NodeRuntimeResult::Accepted: return "accepted";
    case NodeRuntimeResult::NotRunning: return "not-running";
    case NodeRuntimeResult::InvalidConfiguration: return "invalid-configuration";
    case NodeRuntimeResult::UpstreamRejected: return "upstream-rejected";
    case NodeRuntimeResult::ChildRejected: return "child-rejected";
    case NodeRuntimeResult::IdentityRejected: return "identity-rejected";
    case NodeRuntimeResult::ProjectionRejected: return "projection-rejected";
    case NodeRuntimeResult::CacheRejected: return "cache-rejected";
    case NodeRuntimeResult::ArchiveRejected: return "archive-rejected";
    case NodeRuntimeResult::TerminalRejected: return "terminal-rejected";
    }
    return "unknown";
}

ResilientSpectatorNodeRuntime::~ResilientSpectatorNodeRuntime() {
    stop();
}

bool ResilientSpectatorNodeRuntime::validConfig(
    const ResilientSpectatorNodeRuntimeConfig& config) const {
    const model::NodeAdvertisement& local = config.localAdvertisement;
    const model::NodeAdvertisement& parent = config.initialParentAdvertisement;
    const model::RouteDescriptor& localControl = local.resolvedControlRoute();
    const bool signedQsp =
        config.ingressMode == NodeRuntimeIngressMode::SignedQsp;
    const bool canonical =
        config.ingressMode == NodeRuntimeIngressMode::CanonicalHostAttested;
    return config.stream.valid()
        && ingressModeValid(config.ingressMode)
        && qnetplay::spectator::isValidIdentity(config.qspIdentity)
        && config.qspIdentity.sessionId == config.stream.setOrSessionId
        && config.qspIdentity.epoch == config.stream.gameEpoch
        && config.simulationContract != 0u
        && sameIdentity(config.compatibilityIdentity,
                        config.stream.compatibility)
        && config.gameId != 0u && config.initialDigest != 0u
        && config.sourceGeneration != 0u
        && config.authorityGeneration != 0u && config.limits.valid()
        && config.limits.maxCachedRecords
            <= (std::numeric_limits<uint32_t>::max)()
        && config.limits.maxCachedEvents
            <= (std::numeric_limits<uint32_t>::max)()
        && config.limits.maxCachedEvents
            >= qnetplay::spectator::kMaxSetEventsPerBatch
        && ((signedQsp && hasNonzeroKey(config.parentLeaseKey))
            || (canonical && !hasNonzeroKey(config.parentLeaseKey)))
        && local.nodeId != model::kInvalidNodeId
        && local.kind == model::NodeKind::SpectatorRelay
        && local.stream == config.stream
        && local.sourceGeneration == config.sourceGeneration
        && local.route.valid() && local.route.routeProved
        && local.routeGeneration == local.route.generation
        && parent.nodeId != model::kInvalidNodeId
        && parent.nodeId != local.nodeId
        && parent.stream == config.stream
        && parent.route.valid() && parent.route.routeProved
        && parent.routeGeneration == parent.route.generation
        && !samePhysicalEndpoint(local.route, parent.route)
        && parent.sourceGeneration >= config.sourceGeneration
        && config.initialParentGeneration != 0u
        && config.initialRouteGeneration == parent.routeGeneration
        && validExternalAncestorPrefix(config)
        && (!config.productionRelay
            || (local.controlRoute.valid() && local.controlRoute.routeProved
                && localControl.generation == local.routeGeneration
                && !samePhysicalEndpoint(localControl, local.route)))
        && config.downstreamArchive.maxRetainedRecords
            >= config.limits.requiredCacheRecords()
        && config.downstreamArchive.maxRetainedBytes
            >= config.limits.maxCachedBytes;
}

bool ResilientSpectatorNodeRuntime::start(
    const ResilientSpectatorNodeRuntimeConfig& config) {
    stop();
    if (!validConfig(config)) return false;

    const bool canonical =
        config.ingressMode == NodeRuntimeIngressMode::CanonicalHostAttested;
    qnetplay::spectator::ViewerConfig upstream{};
    if (!canonical) {
        upstream = config.upstreamViewer;
        upstream.identity = config.qspIdentity;
        upstream.simulationContract = config.simulationContract;
        upstream.compatibilityIdentity = config.compatibilityIdentity;
        upstream.relayIngestOnly = true;
        upstream.maxRelayRetainedRecords = static_cast<uint32_t>(
            (std::max)(config.limits.maxCachedRecords,
                       qnetplay::spectator::kMaxRecordsPerBatch));
        upstream.maxRelayRetainedEvents = static_cast<uint32_t>(
            (std::max)(config.limits.maxCachedEvents,
                       qnetplay::spectator::kMaxSetEventsPerBatch));
    }

    qnetplay::spectator::SourceConfig downstream = config.downstreamSource;
    downstream.identity = config.qspIdentity;
    downstream.simulationContract = config.simulationContract;
    downstream.compatibilityIdentity = config.compatibilityIdentity;
    downstream.maxViewers = (std::min)(
        downstream.maxViewers, config.limits.maxChildrenPerRelay);

    std::unique_ptr<model::ResilientTopologyCoordinator> topology(
        new (std::nothrow) model::ResilientTopologyCoordinator(
            config.stream, config.limits));
    std::unique_ptr<model::ParentLeaseRegistry> leases(
        new (std::nothrow) model::ParentLeaseRegistry(
            config.parentLeaseKey, canonical ? 0u : 256u));
    std::unique_ptr<qnetplay::spectator::SpectatorViewer> viewer;
    if (!canonical) {
        viewer.reset(new (std::nothrow) qnetplay::spectator::SpectatorViewer(
            upstream));
    }
    std::unique_ptr<qnetplay::spectator::SpectatorArchive> archive(
        new (std::nothrow) qnetplay::spectator::SpectatorArchive(
            config.downstreamArchive));
    if (!topology || !leases || (!canonical && !viewer) || !archive
        || !topology->configured()
        || (!canonical && !viewer->isConfigured()) || !archive->isConfigured()
        || topology->registerNode(config.initialParentAdvertisement)
            != model::NodeRegistrationResult::Accepted
        || topology->registerNode(config.localAdvertisement)
            != model::NodeRegistrationResult::Accepted
        || !topology->seedExternalAncestorPrefix(
            config.initialParentAdvertisement.nodeId,
            config.initialParentDepth, config.initialParentAncestry,
            config.initialParentAncestryCount)
        || topology->attachChild(config.initialParentAdvertisement.nodeId,
                                 config.localAdvertisement.nodeId)
            != model::TopologyAttachResult::Attached) {
        return false;
    }

    std::unique_ptr<model::ResilientRelayNode> relay(
        new (std::nothrow) model::ResilientRelayNode(
            config.localAdvertisement.nodeId, config.stream, config.gameId,
            config.initialDigest, *topology, *leases, config.limits));
    if (!relay || !relay->start(config.sourceGeneration)
        || !relay->setInitialParent(config.initialParentAdvertisement.nodeId,
                                    config.initialParentGeneration,
                                    config.initialRouteGeneration)
        || (!canonical && !viewer->begin(config.gameId))) {
        return false;
    }

    std::unique_ptr<qnetplay::spectator::SpectatorSource> source(
        new (std::nothrow) qnetplay::spectator::SpectatorSource(
            downstream, *archive));
    if (!source || !source->isConfigured()) return false;
    try {
        _childBindings.clear();
        _childBindings.reserve(config.limits.maxChildrenPerRelay);
    } catch (const std::bad_alloc&) {
        _childBindings.clear();
        return false;
    }

    _config = config;
    _topology = std::move(topology);
    _leases = std::move(leases);
    _relay = std::move(relay);
    _upstream = std::move(viewer);
    _archive = std::move(archive);
    _downstream = std::move(source);
    _running = true;
    return true;
}

void ResilientSpectatorNodeRuntime::stop() {
    _running = false;
    for (const ChildQspBinding& binding : _childBindings) {
        if (binding.authority != nullptr && binding.replayNonce != 0u
            && binding.childBinding.valid()) {
            (void)binding.authority->markRetired(
                binding.childBinding.authorityGeneration, binding.replayNonce,
                binding.nodeId, binding.childBinding);
        }
    }
    _downstream.reset();
    _archive.reset();
    _upstream.reset();
    _childBindings.clear();
    _canonicalAuthority.reset();
    if (_relay) _relay->close();
    _relay.reset();
    _leases.reset();
    _topology.reset();
    _config = {};
    _downstreamStarted = false;
    _downstreamSealed = false;
    _projectedRecord = 0u;
    _projectedEvent = 0u;
}

bool ResilientSpectatorNodeRuntime::canonicalIngress() const {
    return _config.ingressMode == NodeRuntimeIngressMode::CanonicalHostAttested;
}

bool ResilientSpectatorNodeRuntime::canonicalAuthorityActive(
    const uint64_t nowMs) const {
    return _canonicalAuthority != nullptr
        && _canonicalAuthority->activeFor(_config.authorityGeneration, nowMs);
}

bool ResilientSpectatorNodeRuntime::bindCanonicalAuthority(
    const QtaVerifiedCanonicalIngress& verifiedIngress,
    const qnetplay::spectator::SpectatorViewer& verifiedViewer,
    const uint64_t nowMs) {
    if (!verifiedIngress.matches(_config, verifiedViewer, nowMs)) return false;
    const std::shared_ptr<QtaAuthorityContext>& authority =
        verifiedIngress.authority();
    if (_canonicalAuthority != nullptr && _canonicalAuthority != authority)
        return false;
    _canonicalAuthority = authority;
    return true;
}

void ResilientSpectatorNodeRuntime::disableCanonicalAuthority() {
    stop();
}

bool ResilientSpectatorNodeRuntime::startDownstreamFromVerifiedGameStart(
    const qnetplay::spectator::GameStartPayload& start) {
    if (!_running || _downstreamStarted || !_archive || !_relay)
        return false;
    if (start.gameId != _config.gameId
        || start.simulationContract != _config.simulationContract
        || !sameIdentity(start.compatibilityIdentity,
                         _config.compatibilityIdentity)
        || start.initialDigest != _config.initialDigest
        || qnetplay::spectator::gameStartFingerprint(start)
            != _config.stream.streamRootIdentity) {
        return false;
    }

    qnetplay::spectator::GameDescriptor descriptor{};
    descriptor.gameId = start.gameId;
    descriptor.simulationContract = start.simulationContract;
    descriptor.compatibilityIdentity = start.compatibilityIdentity;
    descriptor.initialStateHash = start.initialStateHash;
    descriptor.initialDigest = start.initialDigest;
    descriptor.firstTimelineFrame = start.firstTimelineFrame;
    descriptor.playerOneDelay = start.playerOneDelay;
    descriptor.playerTwoDelay = start.playerTwoDelay;
    descriptor.rollbackWindow = start.rollbackWindow;
    descriptor.metadata = start.metadata;
    if (_archive->beginGame(descriptor) != qnetplay::spectator::ArchiveResult::Ok
        || !_relay->cache().configureEventTimeline(start.firstTimelineFrame)) {
        return false;
    }
    _downstreamStarted = true;
    return true;
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::projectVerifiedPrefix() {
    if (!_running || !_upstream || !_archive || !_relay)
        return NodeRuntimeResult::NotRunning;
    return projectVerifiedPrefixFromViewer(*_upstream);
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::projectVerifiedPrefixFromViewer(
    const qnetplay::spectator::SpectatorViewer& viewer) {
    if (!_running || !_archive || !_relay || !viewer.isConfigured())
        return NodeRuntimeResult::NotRunning;
    if (!_downstreamStarted) {
        qnetplay::spectator::GameStartPayload start{};
        if (!viewer.gameStart(start)
            || !startDownstreamFromVerifiedGameStart(start)) {
            return NodeRuntimeResult::IdentityRejected;
        }
    }

    qnetplay::spectator::VerifiedRelayExportExtent extent{};
    if (!viewer.verifiedRelayExportExtent(extent)
        || _projectedRecord < extent.firstRecord
        || _projectedEvent < extent.firstEvent) {
        return NodeRuntimeResult::ProjectionRejected;
    }

    for (;;) {
        qnetplay::spectator::SetEvent event{};
        if (_projectedEvent < extent.nextEvent
            && (!viewer.verifiedSetEventAt(_projectedEvent, event)
                || event.recordCursor < _projectedRecord)) {
            return NodeRuntimeResult::ProjectionRejected;
        }
        if (_projectedEvent < extent.nextEvent
            && event.recordCursor == _projectedRecord) {
            model::RelayEventEnvelope envelope{};
            envelope.stream = _config.stream;
            envelope.gameId = _config.gameId;
            envelope.sourceGeneration = _relay->cache().sourceGeneration();
            envelope.confirmed = true;
            envelope.event = event;
            if (!cacheEventResultAccepted(_relay->cache().appendEvent(envelope)))
                return NodeRuntimeResult::CacheRejected;
            if (!sourceResultAccepted(_archive->appendEvent(
                    _config.gameId, event, nullptr))) {
                return NodeRuntimeResult::ArchiveRejected;
            }
            ++_projectedEvent;
            continue;
        }

        if (_projectedRecord >= extent.nextRecord) break;
        qnetplay::spectator::ConfirmedRecord record{};
        if (!viewer.verifiedRecordAt(_projectedRecord, record))
            return NodeRuntimeResult::ProjectionRejected;
        model::RelayRecordEnvelope envelope{};
        envelope.stream = _config.stream;
        envelope.gameId = _config.gameId;
        envelope.sourceGeneration = _relay->cache().sourceGeneration();
        envelope.confirmed = true;
        envelope.record = record;
        if (!cacheResultAccepted(_relay->cache().append(envelope)))
            return NodeRuntimeResult::CacheRejected;
        if (!sourceResultAccepted(_archive->appendConfirmed(
                _config.gameId, record, nullptr))) {
            return NodeRuntimeResult::ArchiveRejected;
        }
        ++_projectedRecord;
        _relay->noteVerifiedProgress();
    }

    return projectTerminalFromViewer(viewer)
        ? NodeRuntimeResult::Accepted
        : NodeRuntimeResult::TerminalRejected;
}

bool ResilientSpectatorNodeRuntime::projectTerminal() {
    return _upstream != nullptr && projectTerminalFromViewer(*_upstream);
}

bool ResilientSpectatorNodeRuntime::projectTerminalFromViewer(
    const qnetplay::spectator::SpectatorViewer& viewer) {
    if (!_running || !_downstreamStarted || _downstreamSealed || !_archive)
        return _downstreamSealed || _running;
    qnetplay::spectator::ArchiveTerminal terminal{};
    if (!viewer.terminal(terminal)) return true;
    qnetplay::spectator::VerifiedRelayExportExtent extent{};
    if (!viewer.verifiedRelayExportExtent(extent)
        || terminal.nextRecord != extent.nextRecord
        || terminal.nextEvent != extent.nextEvent
        || terminal.finalDigest != extent.digest) {
        return false;
    }
    return sealCanonicalVerifiedTerminal(terminal);
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::onUpstreamDatagram(
    const uint8_t* const bytes, const size_t length, const uint64_t nowMs) {
    if (_running && canonicalIngress())
        return NodeRuntimeResult::UpstreamRejected;
    if (!_running || !_upstream) return NodeRuntimeResult::NotRunning;
    const qnetplay::spectator::IngressResult ingress =
        _upstream->onDatagram(bytes, length, nowMs);
    if (ingress == qnetplay::spectator::IngressResult::ReplayedSequence)
        return NodeRuntimeResult::Accepted;
    if (ingress != qnetplay::spectator::IngressResult::Accepted)
        return NodeRuntimeResult::UpstreamRejected;
    return projectVerifiedPrefix();
}

std::vector<qnetplay::spectator::ViewerDatagram>
ResilientSpectatorNodeRuntime::pollUpstreamOutbound(
    const uint64_t nowMs, const size_t packetBudget) {
    return _running && _upstream
        ? _upstream->pollOutbound(nowMs, packetBudget)
        : std::vector<qnetplay::spectator::ViewerDatagram>{};
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::registerAuthenticatedChild(
    const model::NodeAdvertisement& child, const model::ParentLease& lease,
    const uint64_t nowMs) {
    if (_running && canonicalIngress()) return NodeRuntimeResult::ChildRejected;
    if (!_running || !_topology || !_leases || !_relay)
        return NodeRuntimeResult::NotRunning;
    sweepChildBindings(nowMs);
    if (!_running || !_topology || !_leases || !_relay)
        return NodeRuntimeResult::NotRunning;

    model::NodeAdvertisement constrained{};
    if (!constrainedLeaseChild(lease, constrained))
        return NodeRuntimeResult::ChildRejected;
    if (!suppliedChildMatchesLease(child, constrained)) {
        return NodeRuntimeResult::IdentityRejected;
    }
    return admitLeaseChild(constrained, lease, {}, nowMs);
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::registerQtaVerifiedChild(
    QtaVerifiedParentLease&& verifiedLease, const uint64_t nowMs) {
    std::optional<QtaVerifiedChildRetirement> discardedRetirement;
    return registerQtaVerifiedChild(std::move(verifiedLease), nowMs,
                                    discardedRetirement);
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::registerQtaVerifiedChild(
    QtaVerifiedParentLease&& verifiedLease, const uint64_t nowMs,
    std::optional<QtaVerifiedChildRetirement>& outRetirement) {
    if (!_running || !_topology || !_relay) return NodeRuntimeResult::NotRunning;
    if (!canonicalIngress()) return NodeRuntimeResult::ChildRejected;
    if (outRetirement.has_value()) return NodeRuntimeResult::ChildRejected;
    if (_canonicalAuthority != nullptr && !canonicalAuthorityActive(nowMs)) {
        disableCanonicalAuthority();
        return NodeRuntimeResult::ChildRejected;
    }
    sweepChildBindings(nowMs);
    if (!_running || !_topology || !_relay) return NodeRuntimeResult::NotRunning;

    QtaVerifiedParentLease::Claims claims =
        std::move(verifiedLease).takeClaims(nowMs);
    if (!claims.present() || _canonicalAuthority == nullptr
        || claims.authority != _canonicalAuthority
        || !canonicalAuthorityActive(nowMs)
        || claims.lease.childBinding.authorityGeneration
            != _config.authorityGeneration) {
        if (_canonicalAuthority != nullptr && !canonicalAuthorityActive(nowMs))
            disableCanonicalAuthority();
        return NodeRuntimeResult::ChildRejected;
    }
    model::NodeAdvertisement constrained{};
    if (!constrainedLeaseChild(claims.lease, constrained))
        return NodeRuntimeResult::IdentityRejected;
    return admitLeaseChild(constrained, claims.lease,
                           claims.authority, nowMs, &claims, &outRetirement);
}

bool ResilientSpectatorNodeRuntime::constrainedLeaseChild(
    const model::ParentLease& lease, model::NodeAdvertisement& out) const {
    out = {};
    const model::ParentLeaseChildBinding& binding = lease.childBinding;
    if (!binding.valid() || binding.authorityGeneration
            != _config.authorityGeneration
        || lease.stream != _config.stream
        || lease.childId == model::kInvalidNodeId
        || lease.childId == _config.localAdvertisement.nodeId
        || lease.parentId != _config.localAdvertisement.nodeId
        || lease.parentSourceGeneration != _config.sourceGeneration
        || lease.childBinding.childQspRoute.generation == 0u) {
        return false;
    }

    out.nodeId = lease.childId;
    out.kind = model::NodeKind::SpectatorRelay;
    out.stream = lease.stream;
    out.route = binding.childQspRoute;
    out.activeParent = lease.parentId;
    out.capabilityModelRevision = model::kResilientCapabilityModelRevision;
    out.capabilities = model::kCapabilityTopologyLeaf;
    out.childCapacity = 0u;
    out.uploadCapacity = 0u;
    out.cacheFirstCursor = binding.childRequestedCursor.nextRecord;
    out.cacheNextCursor = binding.childRequestedCursor.nextRecord;
    out.sourceGeneration = lease.parentSourceGeneration;
    out.routeGeneration = binding.childQspRoute.generation;
    out.lastProgressMs = 1u;
    out.reachable = true;
    return true;
}

bool ResilientSpectatorNodeRuntime::suppliedChildMatchesLease(
    const model::NodeAdvertisement& supplied,
    const model::NodeAdvertisement& constrained) const {
    return supplied.nodeId == constrained.nodeId
        && supplied.kind == model::NodeKind::SpectatorRelay
        && supplied.stream == constrained.stream
        && sameRoute(supplied.route, constrained.route)
        && supplied.sourceGeneration == constrained.sourceGeneration
        && supplied.routeGeneration == constrained.routeGeneration;
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::admitLeaseChild(
    const model::NodeAdvertisement& child, const model::ParentLease& lease,
    std::shared_ptr<QtaAuthorityContext> qtaAuthority,
    const uint64_t nowMs, QtaVerifiedParentLease::Claims* const qtaClaims,
    std::optional<QtaVerifiedChildRetirement>* const retirementOut) {
    const bool qtaVerified = qtaAuthority != nullptr;
    if (!_running || !_topology || !_relay || (!qtaVerified && !_leases))
        return NodeRuntimeResult::NotRunning;
    if (qtaVerified
        && (qtaClaims == nullptr || !qtaClaims->present()
            || retirementOut == nullptr || retirementOut->has_value())) {
        return NodeRuntimeResult::ChildRejected;
    }

    const model::ParentLeaseChildBinding& binding = lease.childBinding;
    const model::TopologyNodeView* const parent = _topology->find(
        _config.localAdvertisement.nodeId);
    const model::TopologyNodeView* const existingChild = _topology->find(
        child.nodeId);
    const model::VerifiedCacheRange current = _relay->cache().range();
    uint64_t requestedDigest = 0u;
    if (parent == nullptr || parent->advertisement.nodeId
            != _config.localAdvertisement.nodeId
        || !binding.valid()
        || binding.authorityGeneration != _config.authorityGeneration
        || lease.replayNonce == 0u || lease.expiryMs == 0u
        // Signed-QSP is host-clock domain: never compare canonical QTA's host-issued lease timestamp with this local monotonic clock.
        || (!qtaVerified && lease.expiryMs <= nowMs)
        || (qtaVerified
            && !qtaAuthority->activeFor(_config.authorityGeneration, nowMs))
        || child.nodeId == model::kInvalidNodeId
        || child.nodeId == _config.localAdvertisement.nodeId
        || child.kind != model::NodeKind::SpectatorRelay
        || child.stream != _config.stream
        || child.sourceGeneration != _relay->cache().sourceGeneration()
        || child.routeGeneration != child.route.generation
        || !sameRoute(child.route, binding.childQspRoute)
        || samePhysicalEndpoint(child.route, _config.localAdvertisement.route)
        || parent->depth >= _config.limits.maxActiveDepth
        || (existingChild != nullptr
            && existingChild->activeParent != model::kInvalidNodeId)
        || childBinding(child.nodeId) != nullptr
        || bindingRouteUsedByDifferentChild(
            child.nodeId, binding.childQspRoute,
            binding.childEndpointBindingGeneration)
        || bindingViewerUsedByDifferentChild(child.nodeId,
                                             binding.childViewerId)
        || !_relay->cache().digestAt(
            binding.childRequestedCursor.nextRecord,
            binding.childRequestedCursor.nextEvent, requestedDigest)
        || requestedDigest != binding.childRequestedCursor.digest) {
        return NodeRuntimeResult::ChildRejected;
    }

    const size_t advertisedCapacity =
        (parent->advertisement.capabilities & model::kCapabilityTopologyLeaf)
            != 0u
        ? 0u
        : (parent->advertisement.childCapacity == 0u
               ? _config.limits.maxChildrenPerRelay
               : (std::min)(_config.limits.maxChildrenPerRelay,
                            parent->advertisement.childCapacity));
    if (parent->children.size() >= advertisedCapacity
        || advertisedCapacity > (std::numeric_limits<uint32_t>::max)()) {
        return NodeRuntimeResult::ChildRejected;
    }
    const uint32_t remainingCapacity = static_cast<uint32_t>(
        advertisedCapacity - parent->children.size());
    if (lease.stream != _config.stream || lease.childId != child.nodeId
        || lease.parentId != _config.localAdvertisement.nodeId
        || lease.parentDepth != parent->depth
        || lease.parentSourceGeneration != _relay->cache().sourceGeneration()
        || lease.routeGeneration != parent->advertisement.routeGeneration
        || lease.routeCount != 1u
        || !sameRoute(lease.routes[0], parent->advertisement.route)
        || !emptyRoute(lease.routes[1]) || !emptyRoute(lease.routes[2])
        || lease.latestVerifiedCursor != lease.cacheRange.nextCursor
        || current.nextCursor < lease.latestVerifiedCursor
        || !lease.cacheRange.containsBoundary(
            binding.childRequestedCursor.nextRecord)
        || !current.containsBoundary(binding.childRequestedCursor.nextRecord)
        || !_relay->cache().eventRange().containsBoundary(
            binding.childRequestedCursor.nextEvent)
        || lease.childCapacity != remainingCapacity
        || parent->ancestryCount >= lease.ancestry.size()
        || lease.ancestryCount
            != static_cast<uint8_t>(parent->ancestryCount + 1u)) {
        return NodeRuntimeResult::ChildRejected;
    }
    for (size_t index = 0u; index < parent->ancestryCount; ++index) {
        if (lease.ancestry[index] != parent->ancestry[index]
            || lease.ancestry[index] == model::kInvalidNodeId
            || lease.ancestry[index] == child.nodeId
            || lease.ancestry[index] == _config.localAdvertisement.nodeId) {
            return NodeRuntimeResult::ChildRejected;
        }
        for (size_t prior = 0u; prior < index; ++prior) {
            if (lease.ancestry[prior] == lease.ancestry[index])
                return NodeRuntimeResult::ChildRejected;
        }
    }
    const size_t immediateParentIndex = parent->ancestryCount;
    if (lease.ancestry[immediateParentIndex]
            != _config.localAdvertisement.nodeId
        || lease.ancestry[immediateParentIndex] == child.nodeId) {
        return NodeRuntimeResult::ChildRejected;
    }
    for (size_t prior = 0u; prior < immediateParentIndex; ++prior) {
        if (lease.ancestry[prior] == lease.ancestry[immediateParentIndex])
            return NodeRuntimeResult::ChildRejected;
    }
    for (size_t index = lease.ancestryCount; index < lease.ancestry.size();
         ++index) {
        if (lease.ancestry[index] != model::kInvalidNodeId)
            return NodeRuntimeResult::ChildRejected;
    }

    if (!qtaVerified) {
        model::ParentLeaseExpectation expectation{};
        expectation.stream = _config.stream;
        expectation.expectedChildId = child.nodeId;
        expectation.expectedParentId = _config.localAdvertisement.nodeId;
        expectation.expectedChildBinding = binding;
        expectation.nowMs = nowMs;
        expectation.minimumSourceGeneration = _relay->cache().sourceGeneration();
        expectation.minimumRouteGeneration = parent->advertisement.routeGeneration;
        expectation.requiredCursor = current.nextCursor;
        expectation.maxActiveDepth = _config.limits.maxActiveDepth;
        expectation.parentHasCapacity = true;
        expectation.loopFree = true;
        if (_leases->verify(lease, expectation)
            != model::ParentLeaseVerdict::Accepted) {
            return NodeRuntimeResult::ChildRejected;
        }
    }

    uint64_t handshakeDeadlineMs = 0u;
    if (!deadlineAfter(nowMs, _config.downstreamSource.handshakeDeadlineMs,
                       handshakeDeadlineMs)) {
        return NodeRuntimeResult::ChildRejected;
    }
    if (qtaVerified && !qtaAuthority->activeFor(
            _config.authorityGeneration, nowMs)) {
        return NodeRuntimeResult::ChildRejected;
    }

    model::NodeAdvertisement registration = child;
    registration.activeParent = model::kInvalidNodeId;
    registration.depth = 0u;
    registration.ancestry = {};
    registration.ancestryCount = 0u;
    const model::NodeRegistrationResult registrationResult =
        _topology->registerNode(registration);
    if (registrationResult != model::NodeRegistrationResult::Accepted
        && registrationResult != model::NodeRegistrationResult::UpdatedRoute) {
        return NodeRuntimeResult::ChildRejected;
    }

    const model::ParentLeaseVerdict attached = qtaVerified
        ? (_topology->attachChild(_config.localAdvertisement.nodeId,
                                  child.nodeId)
                   == model::TopologyAttachResult::Attached
               ? model::ParentLeaseVerdict::Accepted
               : model::ParentLeaseVerdict::TopologyRejected)
        : _topology->redeemAndAttach(*_leases, lease, nowMs,
                                     current.nextCursor, binding);
    if (attached != model::ParentLeaseVerdict::Accepted
        || !_relay->attachChild(child.nodeId,
                                binding.childRequestedCursor.nextRecord,
                                binding.childRequestedCursor.nextEvent,
                                binding.childRequestedCursor.digest)) {
        (void)_topology->detach(child.nodeId);
        return NodeRuntimeResult::ChildRejected;
    }

    ChildQspBinding installed{};
    installed.nodeId = child.nodeId;
    installed.qspRoute = binding.childQspRoute;
    installed.endpointBindingGeneration =
        binding.childEndpointBindingGeneration;
    installed.viewerId = binding.childViewerId;
    installed.viewerGeneration = binding.childViewerGeneration;
    installed.requestedCursor = binding.childRequestedCursor;
    installed.handshakeDeadlineMs = handshakeDeadlineMs;
    installed.replayNonce = qtaVerified ? lease.replayNonce : 0u;
    installed.childBinding = qtaVerified
        ? binding : model::ParentLeaseChildBinding{};
    installed.authority = qtaAuthority;
    try {
        _childBindings.push_back(installed);
    } catch (const std::bad_alloc&) {
        if (!_childBindings.empty()
            && _childBindings.back().nodeId == child.nodeId) {
            _childBindings.pop_back();
        }
        (void)_relay->retireChild(child.nodeId);
        (void)_topology->detach(child.nodeId);
        return NodeRuntimeResult::ChildRejected;
    }
    if (qtaVerified) {
        if (!QtaVerifiedParentLease::commitClaims(
                *qtaClaims, nowMs, child.nodeId, binding)) {
            _childBindings.pop_back();
            (void)_relay->retireChild(child.nodeId);
            (void)_topology->detach(child.nodeId);
            return NodeRuntimeResult::ChildRejected;
        }
        QtaVerifiedChildRetirement retirement(child.nodeId,
                                               lease.replayNonce, binding,
                                               qtaAuthority);
        retirementOut->emplace(std::move(retirement));
    }
    return NodeRuntimeResult::Accepted;
}

NodeRuntimeResult
ResilientSpectatorNodeRuntime::projectCanonicalVerifiedPrefix(
    const QtaVerifiedCanonicalIngress& verifiedIngress,
    const qnetplay::spectator::SpectatorViewer& verifiedViewer,
    const uint64_t nowMs) {
    if (!_running) return NodeRuntimeResult::NotRunning;
    if (!canonicalIngress()) return NodeRuntimeResult::UpstreamRejected;
    if (!bindCanonicalAuthority(verifiedIngress, verifiedViewer, nowMs)) {
        if (_canonicalAuthority != nullptr
            && !canonicalAuthorityActive(nowMs)) {
            disableCanonicalAuthority();
        }
        return NodeRuntimeResult::UpstreamRejected;
    }
    return projectVerifiedPrefixFromViewer(verifiedViewer);
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::acceptCanonicalVerifiedRecord(
    const qnetplay::spectator::ConfirmedRecord& record) {
    if (!_relay || !_archive) return NodeRuntimeResult::NotRunning;
    if (record.record != _projectedRecord
        || !qinput::isCanonicalFightInput(record.playerOneInput)
        || !qinput::isCanonicalFightInput(record.playerTwoInput)) {
        return NodeRuntimeResult::ProjectionRejected;
    }
    qnetplay::spectator::GameDescriptor descriptor{};
    if (!_archive->descriptor(_config.gameId, descriptor)
        || descriptor.firstTimelineFrame
               > (std::numeric_limits<uint32_t>::max)() - record.record
        || record.timelineFrame != descriptor.firstTimelineFrame + record.record) {
        return NodeRuntimeResult::ProjectionRejected;
    }
    model::RelayRecordEnvelope envelope{};
    envelope.stream = _config.stream;
    envelope.gameId = _config.gameId;
    envelope.sourceGeneration = _relay->cache().sourceGeneration();
    envelope.confirmed = true;
    envelope.record = record;
    if (!cacheResultAccepted(_relay->cache().append(envelope)))
        return NodeRuntimeResult::CacheRejected;
    if (!sourceResultAccepted(_archive->appendConfirmed(
            _config.gameId, record, nullptr))) {
        return NodeRuntimeResult::ArchiveRejected;
    }
    ++_projectedRecord;
    _relay->noteVerifiedProgress();
    return NodeRuntimeResult::Accepted;
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::acceptCanonicalVerifiedSetEvent(
    const qnetplay::spectator::SetEvent& event) {
    if (!_relay || !_archive) return NodeRuntimeResult::NotRunning;
    if (event.eventIndex != _projectedEvent
        || event.recordCursor != _projectedRecord) {
        return NodeRuntimeResult::ProjectionRejected;
    }
    model::RelayEventEnvelope envelope{};
    envelope.stream = _config.stream;
    envelope.gameId = _config.gameId;
    envelope.sourceGeneration = _relay->cache().sourceGeneration();
    envelope.confirmed = true;
    envelope.event = event;
    if (!cacheEventResultAccepted(_relay->cache().appendEvent(envelope)))
        return NodeRuntimeResult::CacheRejected;
    if (!sourceResultAccepted(_archive->appendEvent(
            _config.gameId, event, nullptr))) {
        return NodeRuntimeResult::ArchiveRejected;
    }
    ++_projectedEvent;
    return NodeRuntimeResult::Accepted;
}

bool ResilientSpectatorNodeRuntime::sealCanonicalVerifiedTerminal(
    const qnetplay::spectator::ArchiveTerminal& terminal) {
    if (!_running || !_downstreamStarted || _downstreamSealed || !_relay
        || !_archive) {
        return false;
    }
    const model::VerifiedChainCursor frontier = _relay->cache().frontier();
    qnetplay::spectator::ArchiveExtent mirror{};
    if (terminal.nextRecord != _projectedRecord
        || terminal.nextEvent != _projectedEvent
        || terminal.nextRecord != frontier.nextRecord
        || terminal.nextEvent != frontier.nextEvent
        || terminal.finalDigest != frontier.digest
        || !_archive->extent(_config.gameId, mirror)
        || mirror.nextRecord != terminal.nextRecord
        || mirror.nextEvent != terminal.nextEvent
        || mirror.digest != terminal.finalDigest) {
        return false;
    }
    const qnetplay::spectator::ArchiveResult sealed = _archive->sealGame(
        _config.gameId, terminal.reason, terminal.postTailStateHash);
    if (!sourceResultAccepted(sealed)) return false;
    _downstreamSealed = true;
    return true;
}

ResilientSpectatorNodeRuntime::ChildQspBinding*
ResilientSpectatorNodeRuntime::childBinding(const model::NodeId childNodeId) {
    const auto found = std::find_if(
        _childBindings.begin(), _childBindings.end(),
        [childNodeId](const ChildQspBinding& binding) {
            return binding.nodeId == childNodeId;
        });
    return found == _childBindings.end() ? nullptr : &*found;
}

const ResilientSpectatorNodeRuntime::ChildQspBinding*
ResilientSpectatorNodeRuntime::childBinding(
    const model::NodeId childNodeId) const {
    const auto found = std::find_if(
        _childBindings.begin(), _childBindings.end(),
        [childNodeId](const ChildQspBinding& binding) {
            return binding.nodeId == childNodeId;
        });
    return found == _childBindings.end() ? nullptr : &*found;
}

bool ResilientSpectatorNodeRuntime::bindingRouteUsedByDifferentChild(
    const model::NodeId childNodeId, const model::RouteDescriptor& route,
    const uint64_t endpointBindingGeneration) const {
    (void)endpointBindingGeneration;
    return std::any_of(
        _childBindings.begin(), _childBindings.end(),
        [childNodeId, &route](
            const ChildQspBinding& binding) {
            return binding.nodeId != childNodeId
                && samePhysicalEndpoint(binding.qspRoute, route);
        });
}

bool ResilientSpectatorNodeRuntime::bindingViewerUsedByDifferentChild(
    const model::NodeId childNodeId,
    const qnetplay::spectator::ViewerId viewerId) const {
    return viewerId != 0u && std::any_of(
        _childBindings.begin(), _childBindings.end(),
        [childNodeId, viewerId](const ChildQspBinding& binding) {
            return binding.nodeId != childNodeId && binding.viewerId == viewerId;
        });
}

bool ResilientSpectatorNodeRuntime::bindingEndpointUsedByDifferentChild(
    const model::NodeId childNodeId,
    const qnetplay::spectator::EndpointId endpoint) const {
    return endpoint != 0u && std::any_of(
        _childBindings.begin(), _childBindings.end(),
        [childNodeId, endpoint](const ChildQspBinding& binding) {
            return binding.nodeId != childNodeId && binding.endpointPinned
                && binding.endpoint == endpoint;
        });
}

ResilientSpectatorNodeRuntime::ChildQspBinding*
ResilientSpectatorNodeRuntime::childBindingForIngress(
    const NodeRuntimeChildIngress& ingress) {
    if (!ingress._observedQspRoute.valid()
        || !ingress._observedQspRoute.routeProved
        || ingress._endpointBindingGeneration == 0u
        || ingress._endpoint == 0u) {
        return nullptr;
    }
    const auto found = std::find_if(
        _childBindings.begin(), _childBindings.end(),
        [&ingress](const ChildQspBinding& binding) {
            return binding.endpointBindingGeneration
                    == ingress._endpointBindingGeneration
                && sameRoute(binding.qspRoute, ingress._observedQspRoute);
        });
    return found == _childBindings.end() ? nullptr : &*found;
}

bool ResilientSpectatorNodeRuntime::attachAuthenticatedChild(
    ChildQspBinding& binding, const NodeRuntimeChildIngress& ingress,
    const uint8_t* const bytes, const size_t length) {
    if (_topology == nullptr || _relay == nullptr || bytes == nullptr) return false;
    const model::TopologyNodeView* child = _topology->find(binding.nodeId);
    if (child == nullptr
        || child->activeParent != _config.localAdvertisement.nodeId) {
        return false;
    }
    qnetplay::spectator::Datagram datagram{};
    ChildPacketIdentity identity{};
    if (qnetplay::spectator::decodeDatagram(bytes, length, _config.qspIdentity,
                                             datagram)
            != qnetplay::spectator::DecodeError::None
        || !decodeChildPacketIdentity(datagram, identity)) {
        return false;
    }
    if (identity.viewerId != binding.viewerId
        || identity.generation != binding.viewerGeneration) {
        return false;
    }
    if (binding.endpointPinned && ingress._endpoint != binding.endpoint)
        return false;
    if (!binding.endpointPinned
        && bindingEndpointUsedByDifferentChild(binding.nodeId,
                                                ingress._endpoint)) {
        return false;
    }
    if (!binding.endpointPinned
        && identity.type != qnetplay::spectator::MessageType::Hello) {
        return false;
    }
    if (identity.type == qnetplay::spectator::MessageType::Hello) {
        qnetplay::spectator::HelloPayload hello{};
        return qnetplay::spectator::decodePayload(datagram.payload, hello)
            && prevalidateChildHello(binding, hello);
    }
    return binding.sourceAttached;
}

bool ResilientSpectatorNodeRuntime::prevalidateChildHello(
    const ChildQspBinding& binding,
    const qnetplay::spectator::HelloPayload& hello) const {
    const bool exactCursor =
        hello.resumeRecord == binding.requestedCursor.nextRecord
        && hello.resumeEvent == binding.requestedCursor.nextEvent
        && hello.resumeDigest == binding.requestedCursor.digest;
    const bool canonicalSeedlessOrigin =
        binding.requestedCursor.nextRecord == 0u
        && binding.requestedCursor.nextEvent == 0u
        && hello.resumeRecord == 0u && hello.resumeEvent == 0u
        && hello.resumeDigest == 0u;
    return hello.simulationContract == _config.simulationContract
        && sameIdentity(hello.compatibilityIdentity,
                        _config.compatibilityIdentity)
        && hello.requestedGameId == _config.gameId
        && hello.viewerId == binding.viewerId
        && hello.generation == binding.viewerGeneration
        && hello.viewerNonce == qnetplay::spectator::viewerGenerationNonce(
            binding.viewerId, binding.viewerGeneration)
        && (exactCursor || canonicalSeedlessOrigin);
}

bool ResilientSpectatorNodeRuntime::bindAcceptedHello(
    ChildQspBinding& binding, const NodeRuntimeChildIngress& ingress,
    const qnetplay::spectator::HelloPayload& hello) {
    if (_relay == nullptr || _downstream == nullptr) return false;
    if (binding.sourceAttached || binding.endpointPinned
        || !prevalidateChildHello(binding, hello)) {
        return false;
    }
    qnetplay::spectator::SourceViewerSnapshot snapshot{};
    if (!_downstream->viewerSnapshot(hello.viewerId, snapshot)
        || snapshot.retired || snapshot.gameId != _config.gameId
        || snapshot.endpoint != ingress._endpoint
        || snapshot.viewerId != binding.viewerId
        || snapshot.generation != binding.viewerGeneration
        || snapshot.acknowledgedNextRecord != binding.requestedCursor.nextRecord
        || snapshot.acknowledgedNextEvent != binding.requestedCursor.nextEvent
        || snapshot.acknowledgedDigest != binding.requestedCursor.digest) {
        return false;
    }
    const model::CacheChildStatus* const cacheChild = _relay->cache().child(
        binding.nodeId);
    if (cacheChild == nullptr
        || cacheChild->acknowledgedCursor != binding.requestedCursor.nextRecord
        || cacheChild->acknowledgedEvent != binding.requestedCursor.nextEvent) {
        return false;
    }
    binding.endpoint = ingress._endpoint;
    binding.endpointPinned = true;
    binding.sourceAttached = true;
    return true;
}

bool ResilientSpectatorNodeRuntime::prevalidateAuthenticatedAcknowledgement(
    const ChildQspBinding& binding, const uint8_t* const bytes,
    const size_t length) const {
    if (_relay == nullptr || bytes == nullptr) return false;
    qnetplay::spectator::Datagram datagram{};
    if (qnetplay::spectator::decodeDatagram(bytes, length, _config.qspIdentity,
                                             datagram)
            != qnetplay::spectator::DecodeError::None) {
        return false;
    }
    switch (datagram.header.type) {
    case qnetplay::spectator::MessageType::Ack: {
        qnetplay::spectator::AckPayload ack{};
        return qnetplay::spectator::decodePayload(datagram.payload, ack)
            && ack.gameId == _config.gameId
            && ack.nextRecord >= binding.requestedCursor.nextRecord
            && ack.nextEvent >= binding.requestedCursor.nextEvent
            && _relay->cache().canAcknowledgeChild(
                binding.nodeId, ack.nextRecord, ack.nextEvent, ack.digest);
    }
    case qnetplay::spectator::MessageType::Detach: {
        qnetplay::spectator::DetachPayload detach{};
        if (!qnetplay::spectator::decodePayload(datagram.payload, detach))
            return false;
        const bool seedlessInitialDetach =
            detach.nextRecord == 0u && detach.nextEvent == 0u
            && detach.digest == 0u
            && binding.requestedCursor.nextRecord == 0u
            && binding.requestedCursor.nextEvent == 0u;
        return seedlessInitialDetach
            || (detach.nextRecord >= binding.requestedCursor.nextRecord
                && detach.nextEvent >= binding.requestedCursor.nextEvent
                && _relay->cache().canAcknowledgeChild(
                    binding.nodeId, detach.nextRecord, detach.nextEvent,
                    detach.digest));
    }
    case qnetplay::spectator::MessageType::TerminalAck: {
        qnetplay::spectator::TerminalAckPayload terminal{};
        return qnetplay::spectator::decodePayload(datagram.payload, terminal)
            && terminal.nextRecord >= binding.requestedCursor.nextRecord
            && terminal.nextEvent >= binding.requestedCursor.nextEvent
            && _relay->cache().canAcknowledgeChild(
                binding.nodeId, terminal.nextRecord, terminal.nextEvent,
                terminal.finalDigest);
    }
    case qnetplay::spectator::MessageType::InitAck: {
        qnetplay::spectator::InitAckPayload init{};
        return qnetplay::spectator::decodePayload(datagram.payload, init)
            && init.gameId == _config.gameId
            && init.viewerId == binding.viewerId
            && init.generation == binding.viewerGeneration
            && init.viewerNonce == qnetplay::spectator::viewerGenerationNonce(
                binding.viewerId, binding.viewerGeneration)
            && init.resumeRecord == binding.requestedCursor.nextRecord
            && init.resumeEvent == binding.requestedCursor.nextEvent
            && init.resumeDigest == binding.requestedCursor.digest;
    }
    default:
        return true;
    }
}

bool ResilientSpectatorNodeRuntime::acknowledgeAuthenticatedChild(
    const ChildQspBinding& binding, const uint8_t* const bytes,
    const size_t length) {
    if (!_relay || bytes == nullptr) return false;
    qnetplay::spectator::Datagram datagram{};
    if (qnetplay::spectator::decodeDatagram(bytes, length, _config.qspIdentity,
                                             datagram)
        != qnetplay::spectator::DecodeError::None) {
        return false;
    }
    switch (datagram.header.type) {
    case qnetplay::spectator::MessageType::Ack: {
        qnetplay::spectator::AckPayload ack{};
        return qnetplay::spectator::decodePayload(datagram.payload, ack)
            && ack.gameId == _config.gameId
            && _relay->cache().acknowledgeChild(
                binding.nodeId, ack.nextRecord, ack.nextEvent, ack.digest);
    }
    case qnetplay::spectator::MessageType::Detach: {
        qnetplay::spectator::DetachPayload detach{};
        if (!qnetplay::spectator::decodePayload(datagram.payload, detach))
            return false;
        if (detach.nextRecord == 0u && detach.nextEvent == 0u
            && detach.digest == 0u
            && binding.requestedCursor.nextRecord == 0u
            && binding.requestedCursor.nextEvent == 0u) {
            return true;
        }
        return _relay->cache().acknowledgeChild(
            binding.nodeId, detach.nextRecord, detach.nextEvent, detach.digest);
    }
    case qnetplay::spectator::MessageType::TerminalAck: {
        qnetplay::spectator::TerminalAckPayload terminal{};
        return qnetplay::spectator::decodePayload(datagram.payload, terminal)
            && terminal.gameId == _config.gameId
            && _relay->cache().acknowledgeChild(
                binding.nodeId, terminal.nextRecord, terminal.nextEvent,
                terminal.finalDigest);
    }
    default:
        return true;
    }
}

void ResilientSpectatorNodeRuntime::retireChildBinding(const size_t index) {
    if (index >= _childBindings.size()) return;
    const ChildQspBinding binding = _childBindings[index];
    if (binding.authority != nullptr && binding.replayNonce != 0u
        && binding.childBinding.valid()) {
        (void)binding.authority->markRetired(
            binding.childBinding.authorityGeneration, binding.replayNonce,
            binding.nodeId, binding.childBinding);
    }
    if (_downstream != nullptr && binding.sourceAttached)
        (void)_downstream->removeViewer(binding.viewerId);
    if (_relay != nullptr) (void)_relay->retireChild(binding.nodeId);
    if (_topology != nullptr) (void)_topology->detach(binding.nodeId);
    _childBindings.erase(_childBindings.begin()
                         + static_cast<std::ptrdiff_t>(index));
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::retireQtaVerifiedChild(
    QtaVerifiedChildRetirement&& verifiedRetirement,
    const uint64_t nowMs) {
    QtaVerifiedChildRetirement::Claims claims =
        std::move(verifiedRetirement).takeClaims();
    if (!claims.present()) return NodeRuntimeResult::ChildRejected;
    if (!_running || !_topology || !_relay || !canonicalIngress()) {
        return NodeRuntimeResult::NotRunning;
    }
    if (_canonicalAuthority == nullptr
        || claims.authority != _canonicalAuthority
        || !canonicalAuthorityActive(nowMs)
        || claims.childBinding.authorityGeneration
            != _config.authorityGeneration) {
        if (_canonicalAuthority != nullptr && !canonicalAuthorityActive(nowMs)) {
            disableCanonicalAuthority();
        }
        return NodeRuntimeResult::ChildRejected;
    }

    ChildQspBinding* const active = childBinding(claims.childNodeId);
    if (active != nullptr) {
        if (active->authority != claims.authority
            || active->replayNonce != claims.replayNonce
            || active->childBinding != claims.childBinding) {
            return NodeRuntimeResult::ChildRejected;
        }
        if (!claims.authority->markRetired(
                claims.childBinding.authorityGeneration, claims.replayNonce,
                claims.childNodeId, claims.childBinding)) {
            return NodeRuntimeResult::ChildRejected;
        }
        const auto found = std::find_if(
            _childBindings.begin(), _childBindings.end(),
            [&claims](const ChildQspBinding& candidate) {
                return candidate.nodeId == claims.childNodeId;
            });
        if (found == _childBindings.end()) return NodeRuntimeResult::ChildRejected;
        retireChildBinding(static_cast<size_t>(
            std::distance(_childBindings.begin(), found)));
        return NodeRuntimeResult::Accepted;
    }

    return claims.authority->matchesRetired(
               claims.childBinding.authorityGeneration, claims.replayNonce,
               claims.childNodeId, claims.childBinding)
        ? NodeRuntimeResult::Accepted
        : NodeRuntimeResult::ChildRejected;
}

void ResilientSpectatorNodeRuntime::sweepChildBindings(
    const uint64_t nowMs) {
    for (size_t index = _childBindings.size(); index > 0u; --index) {
        const size_t current = index - 1u;
        const ChildQspBinding& binding = _childBindings[current];
        bool retire = !binding.sourceAttached
            && deadlineReached(nowMs, binding.handshakeDeadlineMs);
        if (!retire && binding.sourceAttached && _downstream != nullptr) {
            qnetplay::spectator::SourceViewerSnapshot snapshot{};
            if (!_downstream->viewerSnapshot(binding.viewerId, snapshot)
                || snapshot.retired) {
                retire = true;
            } else if (intervalElapsed(nowMs, snapshot.lastValidReceiveMs,
                                       _config.downstreamSource.idleTimeoutMs)
                       || (snapshot.phase
                               == qnetplay::spectator::SourceViewerPhase::
                                   AwaitingInitAck
                           && intervalElapsed(
                               nowMs, snapshot.phaseEnteredMs,
                               _config.downstreamSource.handshakeDeadlineMs))
                       || (snapshot.phase
                               == qnetplay::spectator::SourceViewerPhase::
                                   AwaitingTerminalAck
                           && intervalElapsed(
                               nowMs, snapshot.phaseEnteredMs,
                               _config.downstreamSource.terminalAckDeadlineMs))
                       || (snapshot.phase
                               == qnetplay::spectator::SourceViewerPhase::
                                   Streaming
                           && !snapshot.caughtUp
                           && intervalElapsed(
                               nowMs, snapshot.lastProgressMs,
                               _config.downstreamSource.noProgressDeadlineMs))) {
                retire = true;
            }
        }
        if (retire) retireChildBinding(current);
    }
}

NodeRuntimeResult ResilientSpectatorNodeRuntime::onChildDatagram(
    const NodeRuntimeChildIngress& ingress,
    const uint8_t* const bytes, const size_t length, const uint64_t nowMs) {
    if (!_running || !_downstream || !_relay) return NodeRuntimeResult::NotRunning;
    if (canonicalIngress()) {
        if (_canonicalAuthority == nullptr) return NodeRuntimeResult::ChildRejected;
        if (!canonicalAuthorityActive(nowMs)) {
            disableCanonicalAuthority();
            return NodeRuntimeResult::ChildRejected;
        }
    }
    sweepChildBindings(nowMs);
    if (!_running || !_downstream || !_relay)
        return NodeRuntimeResult::NotRunning;
    qnetplay::spectator::Datagram datagram{};
    if (qnetplay::spectator::decodeDatagram(bytes, length, _config.qspIdentity,
                                             datagram)
            != qnetplay::spectator::DecodeError::None) {
        return NodeRuntimeResult::ChildRejected;
    }
    ChildQspBinding* const binding = childBindingForIngress(ingress);
    const bool terminalAcknowledgement = datagram.header.type
        == qnetplay::spectator::MessageType::TerminalAck;
    if (!_downstreamStarted
        || (_downstreamSealed && !terminalAcknowledgement)
        || binding == nullptr
        || !attachAuthenticatedChild(*binding, ingress, bytes, length)) {
        return NodeRuntimeResult::ChildRejected;
    }
    if (binding->authority != nullptr
        && !binding->authority->activeFor(_config.authorityGeneration, nowMs)) {
        disableCanonicalAuthority();
        return NodeRuntimeResult::ChildRejected;
    }
    if (!prevalidateAuthenticatedAcknowledgement(*binding, bytes, length))
        return NodeRuntimeResult::CacheRejected;
    const qnetplay::spectator::IngressResult result = _downstream->onDatagram(
        ingress._endpoint, bytes, length, nowMs);
    const bool detached = datagram.header.type
            == qnetplay::spectator::MessageType::Detach
        && result == qnetplay::spectator::IngressResult::Detached;
    if (result != qnetplay::spectator::IngressResult::Accepted && !detached) {
        return NodeRuntimeResult::ChildRejected;
    }
    if (datagram.header.type == qnetplay::spectator::MessageType::Hello
        && !binding->sourceAttached) {
        qnetplay::spectator::HelloPayload hello{};
        if (!qnetplay::spectator::decodePayload(datagram.payload, hello)) {
            return NodeRuntimeResult::ChildRejected;
        }
        if (!bindAcceptedHello(*binding, ingress, hello)) {
            (void)_downstream->removeViewer(hello.viewerId);
            return NodeRuntimeResult::ChildRejected;
        }
    }
    const bool terminal = datagram.header.type
            == qnetplay::spectator::MessageType::Detach
        || datagram.header.type
            == qnetplay::spectator::MessageType::TerminalAck;
    if (!acknowledgeAuthenticatedChild(*binding, bytes, length)) {
        if (terminal) {
            const model::NodeId nodeId = binding->nodeId;
            const auto found = std::find_if(
                _childBindings.begin(), _childBindings.end(),
                [nodeId](const ChildQspBinding& candidate) {
                    return candidate.nodeId == nodeId;
                });
            if (found != _childBindings.end()) {
                retireChildBinding(static_cast<size_t>(
                    std::distance(_childBindings.begin(), found)));
            }
        }
        return NodeRuntimeResult::CacheRejected;
    }
    if (terminal) {
        const model::NodeId nodeId = binding->nodeId;
        const auto found = std::find_if(
            _childBindings.begin(), _childBindings.end(),
            [nodeId](const ChildQspBinding& candidate) {
                return candidate.nodeId == nodeId;
            });
        if (found == _childBindings.end()) return NodeRuntimeResult::ChildRejected;
        retireChildBinding(static_cast<size_t>(
            std::distance(_childBindings.begin(), found)));
    }
    return NodeRuntimeResult::Accepted;
}

std::vector<NodeRuntimeDownstreamDatagram>
ResilientSpectatorNodeRuntime::pollChildOutbound(const uint64_t nowMs,
                                                 const size_t packetBudget) {
    std::vector<NodeRuntimeDownstreamDatagram> out;
    if (!_running || !_downstream) return out;
    if (canonicalIngress()) {
        if (_canonicalAuthority == nullptr) return out;
        if (!canonicalAuthorityActive(nowMs)) {
            disableCanonicalAuthority();
            return out;
        }
    }
    sweepChildBindings(nowMs);
    if (!_running || !_downstream) return out;
    const std::vector<qnetplay::spectator::SourceDatagram> packets =
        _downstream->pollOutbound(nowMs, packetBudget);
    sweepChildBindings(nowMs);
    if (!_running || !_downstream) return out;
    out.reserve(packets.size());
    for (const qnetplay::spectator::SourceDatagram& packet : packets) {
        const bool stillBound = std::any_of(
            _childBindings.begin(), _childBindings.end(),
            [&packet](const ChildQspBinding& binding) {
                return binding.sourceAttached
                    && binding.endpointPinned
                    && binding.endpoint == packet.destination
                    && binding.viewerId == packet.viewer;
            });
        if (!stillBound) continue;
        NodeRuntimeDownstreamDatagram copy{};
        copy.destination = packet.destination;
        copy.viewer = packet.viewer;
        copy.type = packet.type;
        copy.bytes = packet.bytes;
        out.push_back(std::move(copy));
    }
    return out;
}

bool ResilientSpectatorNodeRuntime::noteUpstreamFailure(
    const model::UpstreamFailureReason reason, const uint64_t nowMs) {
    return _running && !canonicalIngress() && _relay
        && _relay->noteUpstreamFailure(reason, nowMs);
}

bool ResilientSpectatorNodeRuntime::noteCanonicalAuthorityLoss(
    const QtaVerifiedCanonicalIngress& verifiedIngress,
    const uint64_t nowMs) {
    if (!_running || !canonicalIngress() || _canonicalAuthority == nullptr
        || !canonicalAuthorityActive(nowMs)
        || !verifiedIngress.matchesControl(_config, nowMs)
        || verifiedIngress.authority() != _canonicalAuthority) {
        return false;
    }
    disableCanonicalAuthority();
    return true;
}

void ResilientSpectatorNodeRuntime::tick(const uint64_t nowMs) {
    if (!_running) return;
    if (canonicalIngress()) {
        if (_canonicalAuthority != nullptr
            && !canonicalAuthorityActive(nowMs)) {
            disableCanonicalAuthority();
            return;
        }
        sweepChildBindings(nowMs);
        return;
    }
    sweepChildBindings(nowMs);
    if (_relay) _relay->tick(nowMs);
}

const model::ResilientRelayNode*
ResilientSpectatorNodeRuntime::relayNode() const {
    return _running ? _relay.get() : nullptr;
}

const qnetplay::spectator::SpectatorArchive*
ResilientSpectatorNodeRuntime::downstreamArchive() const {
    return _running ? _archive.get() : nullptr;
}

const qnetplay::spectator::SpectatorViewer*
ResilientSpectatorNodeRuntime::upstreamViewer() const {
    return _running ? _upstream.get() : nullptr;
}

bool ResilientSpectatorNodeRuntime::hasActiveChild(
    const model::NodeId childNodeId) const {
    return _running && childNodeId != model::kInvalidNodeId
        && childBinding(childNodeId) != nullptr;
}

}
