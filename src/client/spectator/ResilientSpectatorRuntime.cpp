#include "client/spectator/ResilientSpectatorRuntime.h"

#include "spectator/SpectatorRelayAdapter.h"

#include <algorithm>
#include <new>
#include <utility>

namespace qclient::spectator::resilient {
namespace {

bool hasNonzeroKey(const qnetplay::spectator::SpectatorGrantKey& key) {
    return std::any_of(key.begin(), key.end(),
                       [](const uint8_t byte) { return byte != 0u; });
}

bool rootKind(const model::NodeKind kind) {
    return kind == model::NodeKind::HostRoot
        || kind == model::NodeKind::ClientRoot;
}

bool sameStream(const model::StreamCompatibilityIdentity& left,
                const model::StreamCompatibilityIdentity& right) {
    return left == right;
}

bool sameRoute(const model::RouteDescriptor& left,
               const model::RouteDescriptor& right) {
    return left.kind == right.kind && left.ipv4BE == right.ipv4BE
        && left.port == right.port && left.generation == right.generation
        && left.routeProved == right.routeProved;
}

model::LeaseBinding leaseBindingOf(const model::ParentLease& lease) {
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

bool sameLeaseBinding(const model::LeaseBinding& left,
                      const model::LeaseBinding& right) {
    return left.childId == right.childId && left.parentId == right.parentId
        && left.parentSourceGeneration == right.parentSourceGeneration
        && left.routeGeneration == right.routeGeneration
        && left.expiryMs == right.expiryMs
        && left.replayNonce == right.replayNonce
        && left.macLow == right.macLow && left.macHigh == right.macHigh
        && left.childBinding == right.childBinding;
}

bool sameParentLease(const model::ParentLease& left,
                     const model::ParentLease& right) {
    if (!sameStream(left.stream, right.stream)
        || left.childId != right.childId || left.parentId != right.parentId
        || left.parentDepth != right.parentDepth
        || left.parentSourceGeneration != right.parentSourceGeneration
        || left.routeGeneration != right.routeGeneration
        || left.routeCount != right.routeCount
        || left.childBinding != right.childBinding
        || left.cacheRange.firstCursor != right.cacheRange.firstCursor
        || left.cacheRange.nextCursor != right.cacheRange.nextCursor
        || left.latestVerifiedCursor != right.latestVerifiedCursor
        || left.childCapacity != right.childCapacity
        || left.expiryMs != right.expiryMs
        || left.replayNonce != right.replayNonce
        || left.ancestryCount != right.ancestryCount
        || left.macLow != right.macLow || left.macHigh != right.macHigh) {
        return false;
    }
    for (size_t index = 0u; index < left.routes.size(); ++index)
        if (!sameRoute(left.routes[index], right.routes[index])) return false;
    for (size_t index = 0u; index < left.ancestry.size(); ++index)
        if (left.ancestry[index] != right.ancestry[index]) return false;
    return true;
}

bool cacheAppendAccepted(const model::CacheAppendResult result) {
    return result == model::CacheAppendResult::Accepted
        || result == model::CacheAppendResult::ExactDuplicate;
}

bool cacheEventAppendAccepted(const model::CacheEventAppendResult result) {
    return result == model::CacheEventAppendResult::Accepted
        || result == model::CacheEventAppendResult::ExactDuplicate;
}

}

ResilientSpectatorRootRuntime::~ResilientSpectatorRootRuntime() {
    stop();
}

bool ResilientSpectatorRootRuntime::supportedUdpRoute(
    const model::RouteDescriptor& route) {
    return route.valid() && route.routeProved
        && route.kind != model::RouteKind::TrafficRelay;
}

bool ResilientSpectatorRootRuntime::endpointMatches(
    const RouteEndpoint& endpoint, const model::RouteDescriptor& route) {
    return endpoint.valid() && route.valid()
        && endpoint.ipv4BE == route.ipv4BE && endpoint.port == route.port;
}

namespace {

bool relayVirtualRouteSupported(const model::RouteDescriptor& route) {
    return route.valid() && route.routeProved
        && route.kind == model::RouteKind::TrafficRelay;
}

bool nodeRoutesAdmissible(const RouteEndpoint& from,
                          const model::RouteDescriptor& route,
                          const model::RouteDescriptor& controlRoute) {
    const bool relayIngress =
        from.ipv4BE == qnetplay::spectator::kRelayVirtualIpv4BE;
    if (relayIngress) {
        return relayVirtualRouteSupported(route)
            && relayVirtualRouteSupported(controlRoute);
    }
    return route.valid() && route.routeProved
        && route.kind != model::RouteKind::TrafficRelay
        && controlRoute.valid() && controlRoute.routeProved
        && controlRoute.kind != model::RouteKind::TrafficRelay;
}

bool advertisedRoutePairAdmissible(const model::RouteDescriptor& route,
                                   const model::RouteDescriptor& controlRoute) {
    const bool relayPair = route.kind == model::RouteKind::TrafficRelay
        || controlRoute.kind == model::RouteKind::TrafficRelay;
    if (relayPair) {
        return relayVirtualRouteSupported(route)
            && relayVirtualRouteSupported(controlRoute);
    }
    return route.valid() && route.routeProved
        && controlRoute.valid() && controlRoute.routeProved;
}

}

const model::StreamCompatibilityIdentity*
ResilientSpectatorRootRuntime::streamOf(
    const wire::ControlDatagram& datagram) {
    switch (datagram.type) {
    case wire::ControlMessageType::NodeAdvertisement:
        return &datagram.nodeAdvertisement.stream;
    case wire::ControlMessageType::ParentLease:
        return &datagram.parentLease.stream;
    case wire::ControlMessageType::ContinuityRequest:
        return &datagram.continuityRequest.stream;
    case wire::ControlMessageType::ContinuityProof:
        return &datagram.continuityProof.stream;
    case wire::ControlMessageType::ParentCommit:
        return &datagram.parentCommit.stream;
    case wire::ControlMessageType::DrainNotice:
        return &datagram.drainNotice.stream;
    case wire::ControlMessageType::AuthorityState:
        return &datagram.authorityState.stream;
    }
    return nullptr;
}

bool ResilientSpectatorRootRuntime::validConfig(
    const ResilientRootRuntimeConfig& config) const {
    const model::NodeAdvertisement& local = config.localAdvertisement;
    return config.stream.valid() && config.gameId != 0u
        && config.limits.valid() && hasNonzeroKey(config.parentLeaseKey)
        && config.authorizer != nullptr && local.nodeId != model::kInvalidNodeId
        && rootKind(local.kind) && sameStream(local.stream, config.stream)
        && supportedUdpRoute(local.route)
        && supportedUdpRoute(local.resolvedControlRoute())
        && local.capabilityModelRevision
            == model::kResilientCapabilityModelRevision
        && local.childCount == 0u && local.uploadInUse == 0u
        && local.sourceGeneration != 0u && local.routeGeneration != 0u
        && local.route.generation == local.routeGeneration
        && local.cacheFirstCursor <= local.cacheNextCursor;
}

bool ResilientSpectatorRootRuntime::start(
    const ResilientRootRuntimeConfig& config,
    ResilientControlTransport& transport) {
    stop();
    if (!validConfig(config)) return false;

    std::unique_ptr<model::VerifiedRelayCache> cache(
        new (std::nothrow) model::VerifiedRelayCache(config.limits));
    if (!cache || !cache->bind(config.stream, config.gameId,
                               config.initialDigest,
                               config.localAdvertisement.sourceGeneration,
                               config.firstTimelineFrame)) {
        return false;
    }

    ResilientRootRuntimeConfig boundConfig = config;
    boundConfig.localAdvertisement.cacheFirstCursor = cache->range().firstCursor;
    boundConfig.localAdvertisement.cacheNextCursor = cache->range().nextCursor;

    std::unique_ptr<model::ResilientTopologyCoordinator> topology(
        new (std::nothrow) model::ResilientTopologyCoordinator(
            boundConfig.stream, boundConfig.limits));
    std::unique_ptr<model::ParentLeaseRegistry> leases(
        new (std::nothrow) model::ParentLeaseRegistry(
            boundConfig.parentLeaseKey));
    if (!topology || !leases || !topology->configured()
        || topology->registerNode(boundConfig.localAdvertisement)
            != model::NodeRegistrationResult::Accepted) {
        return false;
    }

    try {
        _issuedLeases.reserve(boundConfig.limits.maxTopologyNodes);
    } catch (const std::bad_alloc&) {
        return false;
    }

    _config = boundConfig;
    _transport = &transport;
    _topology = std::move(topology);
    _leases = std::move(leases);
    _cache = std::move(cache);
    _running = true;
    return true;
}

void ResilientSpectatorRootRuntime::stop() {
    _running = false;
    _transport = nullptr;
    _issuedLeases.clear();
    _cache.reset();
    _leases.reset();
    _topology.reset();
    _config = {};
}

const model::StreamCompatibilityIdentity*
ResilientSpectatorRootRuntime::stream() const {
    return _running ? &_config.stream : nullptr;
}

const model::ResilientTopologyCoordinator*
ResilientSpectatorRootRuntime::topology() const {
    return _running ? _topology.get() : nullptr;
}

model::ResilientTopologyCoordinator* ResilientSpectatorRootRuntime::topology() {
    return _running ? _topology.get() : nullptr;
}

const model::VerifiedRelayCache* ResilientSpectatorRootRuntime::cache() const {
    return _running ? _cache.get() : nullptr;
}

bool ResilientSpectatorRootRuntime::noteVerifiedProgress(const uint64_t nowMs) {
    if (!_running || _topology == nullptr || nowMs == 0u) return false;
    _config.localAdvertisement.lastProgressMs = nowMs;
    const model::NodeRegistrationResult refreshed =
        _topology->registerNode(_config.localAdvertisement);
    return refreshed == model::NodeRegistrationResult::Accepted
        || refreshed == model::NodeRegistrationResult::UpdatedRoute;
}

bool ResilientSpectatorRootRuntime::detachChild(const model::NodeId childId) {
    if (!_running || _topology == nullptr
        || childId == model::kInvalidNodeId) {
        return false;
    }
    const bool detached = _topology->detach(childId);
    const auto stale = std::remove_if(
        _issuedLeases.begin(), _issuedLeases.end(),
        [childId](const model::ParentLease& lease) {
            return lease.childId == childId;
        });
    _issuedLeases.erase(stale, _issuedLeases.end());
    return detached;
}

bool ResilientSpectatorRootRuntime::refreshLocalCacheAdvertisement() {
    if (!_running || _topology == nullptr || _cache == nullptr
        || !_cache->configured()) {
        return false;
    }
    _config.localAdvertisement.cacheFirstCursor = _cache->range().firstCursor;
    _config.localAdvertisement.cacheNextCursor = _cache->range().nextCursor;
    _config.localAdvertisement.sourceGeneration = _cache->sourceGeneration();
    const model::NodeRegistrationResult refreshed =
        _topology->registerNode(_config.localAdvertisement);
    return refreshed == model::NodeRegistrationResult::Accepted
        || refreshed == model::NodeRegistrationResult::UpdatedRoute;
}

bool ResilientSpectatorRootRuntime::childBindingFor(
    const model::NodeId childId,
    model::ParentLeaseChildBinding& out) const {
    out = {};
    return _running && _config.authorizer != nullptr
        && childId != model::kInvalidNodeId
        && _config.authorizer->parentLeaseChildBinding(childId, out)
        && out.valid();
}

bool ResilientSpectatorRootRuntime::cacheSupportsChildBinding(
    const model::ParentLeaseChildBinding& binding) const {
    if (!_running || _cache == nullptr || !binding.valid()) return false;

    uint32_t exactEvent = 0u;
    uint64_t exactDigest = 0u;
    return _cache->eventCursorAt(binding.childRequestedCursor.nextRecord,
                                 exactEvent)
        && exactEvent == binding.childRequestedCursor.nextEvent
        && _cache->digestAt(binding.childRequestedCursor.nextRecord,
                            binding.childRequestedCursor.nextEvent,
                            exactDigest)
        && exactDigest == binding.childRequestedCursor.digest;
}

model::CacheAppendResult ResilientSpectatorRootRuntime::acceptArchiveRecord(
    const qnetplay::spectator::ConfirmedRecord& record) {
    if (!_running || _cache == nullptr) {
        return model::CacheAppendResult::NotConfigured;
    }
    model::RelayRecordEnvelope envelope{};
    envelope.stream = _config.stream;
    envelope.gameId = _config.gameId;
    envelope.sourceGeneration = _cache->sourceGeneration();
    envelope.confirmed = true;
    envelope.record = record;
    const model::CacheAppendResult result = _cache->append(envelope);
    if (cacheAppendAccepted(result) && !refreshLocalCacheAdvertisement()) {
        stop();
        return model::CacheAppendResult::NotConfigured;
    }
    return result;
}

model::CacheEventAppendResult ResilientSpectatorRootRuntime::acceptArchiveEvent(
    const qnetplay::spectator::SetEvent& event) {
    if (!_running || _cache == nullptr) {
        return model::CacheEventAppendResult::NotConfigured;
    }
    model::RelayEventEnvelope envelope{};
    envelope.stream = _config.stream;
    envelope.gameId = _config.gameId;
    envelope.sourceGeneration = _cache->sourceGeneration();
    envelope.confirmed = true;
    envelope.event = event;
    const model::CacheEventAppendResult result = _cache->appendEvent(envelope);
    if (cacheEventAppendAccepted(result) && !refreshLocalCacheAdvertisement()) {
        stop();
        return model::CacheEventAppendResult::NotConfigured;
    }
    return result;
}

void ResilientSpectatorRootRuntime::expireIssuedLeases(const uint64_t nowMs) {
    const auto expired = std::remove_if(
        _issuedLeases.begin(), _issuedLeases.end(),
        [nowMs](const model::ParentLease& lease) {
            return lease.expiryMs <= nowMs;
        });
    _issuedLeases.erase(expired, _issuedLeases.end());
}

bool ResilientSpectatorRootRuntime::retainIssuedLease(
    const model::ParentLease& lease) {
    const auto sameChild = std::remove_if(
        _issuedLeases.begin(), _issuedLeases.end(),
        [&lease](const model::ParentLease& existing) {
            return existing.childId == lease.childId;
        });
    _issuedLeases.erase(sameChild, _issuedLeases.end());
    if (_issuedLeases.size() >= _config.limits.maxTopologyNodes) return false;
    try {
        _issuedLeases.push_back(lease);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

const model::ParentLease* ResilientSpectatorRootRuntime::findIssuedLease(
    const model::ParentLease& lease) const {
    const auto found = std::find_if(
        _issuedLeases.begin(), _issuedLeases.end(),
        [&lease](const model::ParentLease& candidate) {
            return sameParentLease(candidate, lease);
        });
    return found == _issuedLeases.end() ? nullptr : &*found;
}

const model::ParentLease* ResilientSpectatorRootRuntime::findIssuedLease(
    const model::LeaseBinding& binding) const {
    const auto found = std::find_if(
        _issuedLeases.begin(), _issuedLeases.end(),
        [&binding](const model::ParentLease& candidate) {
            return sameLeaseBinding(leaseBindingOf(candidate), binding);
        });
    return found == _issuedLeases.end() ? nullptr : &*found;
}

bool ResilientSpectatorRootRuntime::verifyContinuityRequest(
    const model::ContinuityRequest& request,
    const model::ParentLease& issued,
    const RouteEndpoint& from, const uint64_t nowMs) const {
    if (_topology == nullptr || _leases == nullptr || _cache == nullptr
        || request.gameId != _config.gameId
        || request.expectedSourceGeneration != _cache->sourceGeneration()
        || !sameLeaseBinding(request.lease, leaseBindingOf(issued))
        || issued.parentId != _config.localAdvertisement.nodeId
        || issued.expiryMs <= nowMs) {
        return false;
    }

    const model::TopologyNodeView* const child =
        _topology->find(issued.childId);
    if (child == nullptr
        || !nodeRoutesAdmissible(from, child->advertisement.route,
                                 child->advertisement.resolvedControlRoute())
        || !endpointMatches(from,
                            child->advertisement.resolvedControlRoute())) {
        return false;
    }

    model::ParentLeaseExpectation expectation{};
    expectation.stream = _config.stream;
    expectation.expectedChildId = issued.childId;
    expectation.expectedParentId = issued.parentId;
    expectation.expectedChildBinding = issued.childBinding;
    expectation.nowMs = nowMs;
    expectation.minimumSourceGeneration = _cache->sourceGeneration();
    expectation.minimumRouteGeneration =
        _config.localAdvertisement.routeGeneration;
    expectation.requiredCursor = request.exactNextCursor;
    expectation.maxActiveDepth = _config.limits.maxActiveDepth;
    expectation.parentHasCapacity = true;
    expectation.loopFree = true;
    if (!_leases->wasRedeemed(issued.replayNonce)
        || _leases->verifyContinuity(issued, expectation)
            != model::ParentLeaseVerdict::Accepted) {
        return false;
    }

    uint32_t expectedExactEvent = 0;
    uint32_t expectedOverlapEvent = 0;
    uint64_t expectedOverlapDigest = 0;
    return _cache->eventCursorAt(request.exactNextCursor, expectedExactEvent)
        && expectedExactEvent == request.exactNextEvent
        && _cache->eventCursorAt(request.overlapCursor, expectedOverlapEvent)
        && expectedOverlapEvent == request.overlapEvent
        && _cache->digestAt(request.overlapCursor, request.overlapEvent,
                            expectedOverlapDigest)
        && expectedOverlapDigest == request.overlapDigest;
}

bool ResilientSpectatorRootRuntime::send(
    const model::RouteDescriptor& to, const wire::ControlDatagram& datagram) {
    if (!_running || _transport == nullptr
        || (!supportedUdpRoute(to) && !relayVirtualRouteSupported(to))) {
        return false;
    }
    std::vector<uint8_t> encoded;
    return wire::encodeControlDatagram(datagram, encoded)
        && _transport->sendControl(to, encoded.data(), encoded.size());
}

bool ResilientSpectatorRootRuntime::sendAdvertisement(
    const model::RouteDescriptor& to) {
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::NodeAdvertisement;
    datagram.nodeAdvertisement = _config.localAdvertisement;
    return send(to, datagram);
}

bool ResilientSpectatorRootRuntime::issueAndSendParentOffer(
    const model::NodeId childId, const uint32_t requiredCursor,
    const uint64_t expiryMs, const uint64_t replayNonce) {
    if (!_running || _topology == nullptr || _leases == nullptr
        || _cache == nullptr || !refreshLocalCacheAdvertisement()) {
        return false;
    }
    const model::TopologyNodeView* const child = _topology->find(childId);
    if (child == nullptr
        || !advertisedRoutePairAdmissible(
            child->advertisement.route,
            child->advertisement.resolvedControlRoute()))
        return false;

    model::ParentLeaseChildBinding childBinding{};
    if (!childBindingFor(childId, childBinding)
        || childBinding.childRequestedCursor.nextRecord != requiredCursor
        || !cacheSupportsChildBinding(childBinding)) {
        return false;
    }

    model::ParentLease lease{};
    if (!_topology->makeUnsignedParentOffer(
            childId, _config.localAdvertisement.nodeId, requiredCursor,
            expiryMs, replayNonce, childBinding, lease)
        || !_leases->issue(lease)) {
        return false;
    }
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::ParentLease;
    datagram.parentLease = lease;
    if (!retainIssuedLease(lease)) return false;
    if (send(child->advertisement.resolvedControlRoute(), datagram)) return true;
    const auto retained = std::remove_if(
        _issuedLeases.begin(), _issuedLeases.end(),
        [&lease](const model::ParentLease& candidate) {
            return sameParentLease(candidate, lease);
        });
    _issuedLeases.erase(retained, _issuedLeases.end());
    return false;
}

bool ResilientSpectatorRootRuntime::resendParentOffer(
    const model::NodeId childId) {
    if (!_running || _topology == nullptr || _leases == nullptr) return false;
    const auto issued = std::find_if(
        _issuedLeases.begin(), _issuedLeases.end(),
        [childId](const model::ParentLease& lease) {
            return lease.childId == childId;
        });
    if (issued == _issuedLeases.end() || _leases->wasRedeemed(
                                         issued->replayNonce)) {
        return false;
    }
    const model::TopologyNodeView* const child = _topology->find(childId);
    if (child == nullptr
        || !advertisedRoutePairAdmissible(
            child->advertisement.route,
            child->advertisement.resolvedControlRoute()))
        return false;
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::ParentLease;
    datagram.parentLease = *issued;
    return send(child->advertisement.resolvedControlRoute(), datagram);
}

bool ResilientSpectatorRootRuntime::sendParentCommit(
    const model::ParentLease& lease) {
    if (!_running || _topology == nullptr || _leases == nullptr
        || _cache == nullptr) {
        return false;
    }
    const model::ParentLease* const issued = findIssuedLease(lease);
    if (issued == nullptr || !sameParentLease(*issued, lease)
        || !_leases->wasRedeemed(lease.replayNonce)) {
        return false;
    }
    const model::TopologyNodeView* const child = _topology->find(lease.childId);
    if (child == nullptr
        || !advertisedRoutePairAdmissible(
            child->advertisement.route,
            child->advertisement.resolvedControlRoute()))
        return false;

    const model::VerifiedChainCursor frontier = _cache->frontier();
    uint32_t eventCursor = 0u;
    uint64_t digest = 0u;
    if (!_cache->eventCursorAt(frontier.nextRecord, eventCursor)
        || eventCursor != frontier.nextEvent
        || !_cache->digestAt(frontier.nextRecord, frontier.nextEvent, digest)
        || digest != frontier.digest) {
        return false;
    }

    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::ParentCommit;
    wire::ParentCommit& commit = datagram.parentCommit;
    commit.stream = _config.stream;
    commit.childId = lease.childId;
    commit.parentId = lease.parentId;
    commit.lease = leaseBindingOf(lease);
    commit.sourceGeneration = lease.parentSourceGeneration;
    commit.routeGeneration = lease.routeGeneration;
    commit.exactNextCursor = frontier.nextRecord;
    commit.exactNextEvent = frontier.nextEvent;
    commit.overlapCursor = frontier.nextRecord;
    commit.overlapEvent = frontier.nextEvent;
    commit.overlapDigest = frontier.digest;
    return send(child->advertisement.resolvedControlRoute(), datagram);
}

bool ResilientSpectatorRootRuntime::sendDrainNotice(
    const model::RouteDescriptor& to, const model::DrainNotice& notice) {
    if (!_running) return false;
    wire::ControlDatagram datagram{};
    datagram.type = wire::ControlMessageType::DrainNotice;
    datagram.drainNotice.stream = _config.stream;
    datagram.drainNotice.nodeId = _config.localAdvertisement.nodeId;
    datagram.drainNotice.notice = notice;
    return send(to, datagram);
}

ControlIngressResult ResilientSpectatorRootRuntime::onDatagram(
    const RouteEndpoint& from, const uint8_t* const bytes,
    const size_t length, const uint64_t nowMs) {
    if (!_running || _topology == nullptr || _leases == nullptr
        || _config.authorizer == nullptr) {
        return ControlIngressResult::NotRunning;
    }

    wire::ControlDatagram datagram{};
    if (wire::decodeControlDatagram(bytes, length, datagram)
        != wire::DecodeResult::Ok) {
        return ControlIngressResult::DecodeRejected;
    }
    const model::StreamCompatibilityIdentity* const stream = streamOf(datagram);
    if (stream == nullptr || !sameStream(*stream, _config.stream))
        return ControlIngressResult::IdentityRejected;
    if (!_config.authorizer->authorize(from, datagram, nowMs))
        return ControlIngressResult::AuthenticationRejected;

    expireIssuedLeases(nowMs);

    switch (datagram.type) {
    case wire::ControlMessageType::NodeAdvertisement: {
        const model::NodeAdvertisement& advertisement =
            datagram.nodeAdvertisement;
        if (advertisement.nodeId == _config.localAdvertisement.nodeId
            || !nodeRoutesAdmissible(from, advertisement.route,
                                     advertisement.resolvedControlRoute())
            || !endpointMatches(from,
                                advertisement.resolvedControlRoute())) {
            return ControlIngressResult::EndpointRejected;
        }
        const model::NodeRegistrationResult registered =
            _topology->registerNode(advertisement);
        return registered == model::NodeRegistrationResult::Accepted
                || registered == model::NodeRegistrationResult::UpdatedRoute
            ? ControlIngressResult::AcceptedAdvertisement
            : ControlIngressResult::TopologyRejected;
    }
    case wire::ControlMessageType::ParentLease: {
        const model::ParentLease& lease = datagram.parentLease;
        if (lease.parentId != _config.localAdvertisement.nodeId) {
            return ControlIngressResult::EndpointRejected;
        }
        if (findIssuedLease(lease) == nullptr) {
            return ControlIngressResult::LeaseRejected;
        }
        const model::TopologyNodeView* const child = _topology->find(lease.childId);
        if (child == nullptr
            || !nodeRoutesAdmissible(
                from, child->advertisement.route,
                child->advertisement.resolvedControlRoute())
            || !endpointMatches(from,
                                child->advertisement.resolvedControlRoute())) {
            return ControlIngressResult::EndpointRejected;
        }
        model::ParentLeaseChildBinding expectedChildBinding{};
        if (!childBindingFor(lease.childId, expectedChildBinding)
            || lease.childBinding != expectedChildBinding
            || !cacheSupportsChildBinding(expectedChildBinding)) {
            return ControlIngressResult::LeaseRejected;
        }
        const model::ParentLeaseVerdict redeemed = _topology->redeemAndAttach(
            *_leases, lease, nowMs,
            expectedChildBinding.childRequestedCursor.nextRecord,
            expectedChildBinding);
        return redeemed == model::ParentLeaseVerdict::Accepted
            ? ControlIngressResult::AcceptedLeaseRedemption
            : ControlIngressResult::LeaseRejected;
    }
    case wire::ControlMessageType::DrainNotice: {
        const wire::BoundDrainNotice& bound = datagram.drainNotice;
        const model::TopologyNodeView* const node =
            _topology->find(bound.nodeId);
        if (bound.nodeId == _config.localAdvertisement.nodeId
            || node == nullptr
            || !nodeRoutesAdmissible(
                from, node->advertisement.route,
                node->advertisement.resolvedControlRoute())
            || !endpointMatches(from,
                                node->advertisement.resolvedControlRoute())
            || node->advertisement.sourceGeneration
                != bound.notice.sourceGeneration
            || node->advertisement.routeGeneration
                != bound.notice.routeGeneration) {
            return ControlIngressResult::EndpointRejected;
        }
        return _topology->setDraining(bound.nodeId, true)
            ? ControlIngressResult::AcceptedDrainNotice
            : ControlIngressResult::TopologyRejected;
    }
    case wire::ControlMessageType::ContinuityRequest: {
        const model::ContinuityRequest& request = datagram.continuityRequest;
        const model::ParentLease* const issued =
            findIssuedLease(request.lease);
        if (issued == nullptr
            || !verifyContinuityRequest(request, *issued, from, nowMs)) {
            return ControlIngressResult::ContinuityRejected;
        }
        const uint32_t overlapRecords =
            request.exactNextCursor - request.overlapCursor;
        model::ContinuityProof proof{};
        if (!_cache->continuityProof(request.exactNextCursor,
                                    request.exactNextEvent,
                                    overlapRecords, proof)
            || !proof.available
            || proof.overlapCursor != request.overlapCursor
            || proof.overlapEvent != request.overlapEvent
            || proof.overlapDigest != request.overlapDigest) {
            return ControlIngressResult::ContinuityRejected;
        }
        proof.lease = request.lease;
        proof.parentGeneration = request.parentGeneration;
        wire::ControlDatagram response{};
        response.type = wire::ControlMessageType::ContinuityProof;
        response.continuityProof = proof;
        const model::TopologyNodeView* const child =
            _topology->find(issued->childId);
        return child != nullptr
                && nodeRoutesAdmissible(
                    from, child->advertisement.route,
                    child->advertisement.resolvedControlRoute())
                && send(child->advertisement.resolvedControlRoute(), response)
            ? ControlIngressResult::AcceptedContinuityRequest
            : ControlIngressResult::ContinuityRejected;
    }
    case wire::ControlMessageType::ContinuityProof:
    case wire::ControlMessageType::ParentCommit:
        return ControlIngressResult::ContinuityRejected;
    }
    return ControlIngressResult::UnsupportedMessage;
}

const char* controlIngressResultName(const ControlIngressResult result) {
    switch (result) {
    case ControlIngressResult::AcceptedAdvertisement:
        return "accepted-advertisement";
    case ControlIngressResult::AcceptedLeaseRedemption:
        return "accepted-lease-redemption";
    case ControlIngressResult::AcceptedDrainNotice:
        return "accepted-drain-notice";
    case ControlIngressResult::NotRunning: return "not-running";
    case ControlIngressResult::DecodeRejected: return "decode-rejected";
    case ControlIngressResult::IdentityRejected: return "identity-rejected";
    case ControlIngressResult::AuthenticationRejected:
        return "authentication-rejected";
    case ControlIngressResult::EndpointRejected: return "endpoint-rejected";
    case ControlIngressResult::TopologyRejected: return "topology-rejected";
    case ControlIngressResult::LeaseRejected: return "lease-rejected";
    case ControlIngressResult::UnsupportedMessage: return "unsupported-message";
    case ControlIngressResult::AcceptedContinuityRequest:
        return "accepted-continuity-request";
    case ControlIngressResult::ContinuityRejected: return "continuity-rejected";
    }
    return "unknown";
}

bool controlIngressAccepted(const ControlIngressResult result) {
    return result == ControlIngressResult::AcceptedAdvertisement
        || result == ControlIngressResult::AcceptedLeaseRedemption
        || result == ControlIngressResult::AcceptedDrainNotice
        || result == ControlIngressResult::AcceptedContinuityRequest;
}

}
