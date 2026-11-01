#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "client/spectator/ResilientSpectatorRuntime.h"
#include "spectator/SpectatorArchive.h"
#include "spectator/SpectatorSession.h"

namespace qclient::spectator::topology::client {
class ReleasedTopologyClientRuntime;
}

namespace qclient::spectator::resilient {

namespace model = qnetplay::spectator::resilient;

class ResilientSpectatorNodeRuntime;
class NodeRuntimeChildIngress;
class QtaVerifiedCanonicalIngress;
class QtaVerifiedChildRetirement;

#if defined(QOH99_NODE_RUNTIME_TEST_SEAM)
namespace test {
struct QtaAuthorityContextTestFactory;
struct QtaVerifiedParentLeaseTestFactory;
struct QtaVerifiedCanonicalIngressTestFactory;
struct QtaVerifiedChildRetirementTestFactory;
struct NodeRuntimeChildIngressTestFactory;
}
#endif

enum class NodeRuntimeIngressMode : uint8_t {
    SignedQsp = 0,
    CanonicalHostAttested = 1,
};

class QtaAuthorityContext
    : public std::enable_shared_from_this<QtaAuthorityContext> {
public:
    QtaAuthorityContext(const QtaAuthorityContext&) = delete;
    QtaAuthorityContext& operator=(const QtaAuthorityContext&) = delete;

private:
    struct NonceReservation;

    struct RedeemedNonce {
        uint64_t authorityGeneration = 0;
        uint64_t replayNonce = 0;
        model::NodeId childNodeId = model::kInvalidNodeId;
        model::ParentLeaseChildBinding childBinding{};
        bool retired = false;
    };

    struct ReservedNonce {
        uint64_t authorityGeneration = 0;
        uint64_t replayNonce = 0;
        uint64_t reservationId = 0;
    };

    explicit QtaAuthorityContext(uint64_t authorityGeneration,
                                 uint64_t localExpiryDeadlineMs,
                                 size_t maximumRedemptions);

    static std::shared_ptr<QtaAuthorityContext> create(
        uint64_t authorityGeneration, uint64_t localExpiryDeadlineMs,
        size_t maximumRedemptions);
    bool activeFor(uint64_t authorityGeneration, uint64_t nowMs) const;
    std::shared_ptr<void> reserve(uint64_t authorityGeneration,
                                  uint64_t replayNonce, uint64_t nowMs);
    bool commitReservation(const std::shared_ptr<void>& reservation,
                           uint64_t authorityGeneration,
                           uint64_t replayNonce,
                           model::NodeId childNodeId,
                           const model::ParentLeaseChildBinding& childBinding,
                           uint64_t nowMs);
    void cancelReservation(uint64_t authorityGeneration,
                           uint64_t replayNonce, uint64_t reservationId);
    bool markRetired(uint64_t authorityGeneration, uint64_t replayNonce,
                     model::NodeId childNodeId,
                     const model::ParentLeaseChildBinding& childBinding);
    bool matchesRetired(uint64_t authorityGeneration, uint64_t replayNonce,
                        model::NodeId childNodeId,
                        const model::ParentLeaseChildBinding& childBinding)
        const;
    void revoke();

    friend class ResilientSpectatorNodeRuntime;
    friend class QtaVerifiedParentLease;
    friend class QtaVerifiedCanonicalIngress;
    friend class
        ::qclient::spectator::topology::client::ReleasedTopologyClientRuntime;
#if defined(QOH99_NODE_RUNTIME_TEST_SEAM)
    friend struct test::QtaAuthorityContextTestFactory;
#endif

    const uint64_t _authorityGeneration;
    const uint64_t _localExpiryDeadlineMs;
    const size_t _maximumRedemptions;
    std::atomic<bool> _revoked{false};
    mutable std::mutex _redemptionMutex;
    std::vector<RedeemedNonce> _redeemed;
    std::vector<ReservedNonce> _reserved;
    uint64_t _nextReservationId = 1u;
};

class QtaVerifiedParentLease {
public:
    QtaVerifiedParentLease(const QtaVerifiedParentLease&) = delete;
    QtaVerifiedParentLease& operator=(const QtaVerifiedParentLease&) = delete;
    QtaVerifiedParentLease(QtaVerifiedParentLease&&) noexcept = default;
    QtaVerifiedParentLease& operator=(QtaVerifiedParentLease&&) noexcept =
        default;

private:
    struct Claims {
        model::ParentLease lease{};
        std::shared_ptr<QtaAuthorityContext> authority;
        std::shared_ptr<void> reservation;

        bool present() const {
            return authority != nullptr && reservation != nullptr;
        }
    };

    explicit QtaVerifiedParentLease(
        model::ParentLease lease,
        std::shared_ptr<QtaAuthorityContext> authority)
        : _lease(std::move(lease)), _authority(std::move(authority)) {}

    Claims takeClaims(uint64_t nowMs) &&;
    static bool commitClaims(Claims& claims, uint64_t nowMs,
                             model::NodeId childNodeId,
                             const model::ParentLeaseChildBinding& binding);

    friend class ResilientSpectatorNodeRuntime;
    friend class
        ::qclient::spectator::topology::client::ReleasedTopologyClientRuntime;
#if defined(QOH99_NODE_RUNTIME_TEST_SEAM)
    friend struct test::QtaVerifiedParentLeaseTestFactory;
#endif

    model::ParentLease _lease{};
    std::shared_ptr<QtaAuthorityContext> _authority;
    bool _consumed = false;
};

class QtaVerifiedChildRetirement {
public:
    QtaVerifiedChildRetirement(const QtaVerifiedChildRetirement&) = delete;
    QtaVerifiedChildRetirement& operator=(
        const QtaVerifiedChildRetirement&) = delete;
    QtaVerifiedChildRetirement(QtaVerifiedChildRetirement&&) noexcept =
        default;
    QtaVerifiedChildRetirement& operator=(
        QtaVerifiedChildRetirement&&) noexcept = default;

private:
    struct Claims {
        model::NodeId childNodeId = model::kInvalidNodeId;
        uint64_t replayNonce = 0u;
        model::ParentLeaseChildBinding childBinding{};
        std::shared_ptr<QtaAuthorityContext> authority;

        bool present() const {
            return authority != nullptr
                && childNodeId != model::kInvalidNodeId
                && replayNonce != 0u && childBinding.valid();
        }
    };

    explicit QtaVerifiedChildRetirement(
        model::NodeId childNodeId, uint64_t replayNonce,
        model::ParentLeaseChildBinding childBinding,
        std::shared_ptr<QtaAuthorityContext> authority)
        : _childNodeId(childNodeId), _replayNonce(replayNonce),
          _childBinding(std::move(childBinding)),
          _authority(std::move(authority)) {}

    Claims takeClaims() &&;

    friend class ResilientSpectatorNodeRuntime;
    friend class
        ::qclient::spectator::topology::client::ReleasedTopologyClientRuntime;
#if defined(QOH99_NODE_RUNTIME_TEST_SEAM)
    friend struct test::QtaVerifiedChildRetirementTestFactory;
#endif

    model::NodeId _childNodeId = model::kInvalidNodeId;
    uint64_t _replayNonce = 0u;
    model::ParentLeaseChildBinding _childBinding{};
    std::shared_ptr<QtaAuthorityContext> _authority;
    bool _consumed = false;
};

struct ResilientSpectatorNodeRuntimeConfig {
    model::StreamCompatibilityIdentity stream{};
    model::NodeAdvertisement localAdvertisement{};
    model::NodeAdvertisement initialParentAdvertisement{};

    qnetplay::spectator::SessionIdentity qspIdentity{};
    uint64_t simulationContract = 0;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity{};
    uint64_t gameId = 0;
    uint64_t initialDigest = 0;
    uint64_t sourceGeneration = 0;
    uint64_t authorityGeneration = 0;
    uint64_t initialParentGeneration = 0;
    uint64_t initialRouteGeneration = 0;
    uint8_t initialParentDepth = 0;
    std::array<model::NodeId, model::kMaxLeaseAncestry>
        initialParentAncestry{};
    uint8_t initialParentAncestryCount = 0;

    model::ResilientLimits limits{};
    NodeRuntimeIngressMode ingressMode = NodeRuntimeIngressMode::SignedQsp;
    bool productionRelay = false;
    qnetplay::spectator::SpectatorGrantKey parentLeaseKey{};
    qnetplay::spectator::ArchiveConfig downstreamArchive{};
    qnetplay::spectator::ViewerConfig upstreamViewer{};
    qnetplay::spectator::SourceConfig downstreamSource{};
};

class QtaVerifiedCanonicalIngress {
public:
    QtaVerifiedCanonicalIngress(const QtaVerifiedCanonicalIngress&) = delete;
    QtaVerifiedCanonicalIngress& operator=(
        const QtaVerifiedCanonicalIngress&) = delete;
    QtaVerifiedCanonicalIngress(QtaVerifiedCanonicalIngress&&) noexcept =
        default;
    QtaVerifiedCanonicalIngress& operator=(
        QtaVerifiedCanonicalIngress&&) noexcept = default;

private:
    explicit QtaVerifiedCanonicalIngress(
        std::shared_ptr<QtaAuthorityContext> authority,
        const ResilientSpectatorNodeRuntimeConfig& config,
        const qnetplay::spectator::SpectatorViewer& verifiedViewer)
        : _authority(std::move(authority)), _stream(config.stream),
          _qspIdentity(config.qspIdentity), _verifiedViewer(&verifiedViewer),
          _localNodeId(config.localAdvertisement.nodeId),
          _localQspRoute(config.localAdvertisement.route),
          _localRouteGeneration(config.localAdvertisement.routeGeneration),
          _parentNodeId(config.initialParentAdvertisement.nodeId),
          _parentQspRoute(config.initialParentAdvertisement.route),
          _parentSourceGeneration(
              config.initialParentAdvertisement.sourceGeneration),
          _parentGeneration(config.initialParentGeneration),
          _parentRouteGeneration(config.initialRouteGeneration),
          _sourceGeneration(config.sourceGeneration) {}

    bool matchesControl(const ResilientSpectatorNodeRuntimeConfig& config,
                        uint64_t nowMs) const;
    bool matches(const ResilientSpectatorNodeRuntimeConfig& config,
                 const qnetplay::spectator::SpectatorViewer& verifiedViewer,
                 uint64_t nowMs) const;
    const std::shared_ptr<QtaAuthorityContext>& authority() const {
        return _authority;
    }

    friend class ResilientSpectatorNodeRuntime;
    friend class
        ::qclient::spectator::topology::client::ReleasedTopologyClientRuntime;
#if defined(QOH99_NODE_RUNTIME_TEST_SEAM)
    friend struct test::QtaVerifiedCanonicalIngressTestFactory;
#endif

    std::shared_ptr<QtaAuthorityContext> _authority;
    model::StreamCompatibilityIdentity _stream{};
    qnetplay::spectator::SessionIdentity _qspIdentity{};
    const qnetplay::spectator::SpectatorViewer* _verifiedViewer = nullptr;
    model::NodeId _localNodeId = model::kInvalidNodeId;
    model::RouteDescriptor _localQspRoute{};
    uint64_t _localRouteGeneration = 0;
    model::NodeId _parentNodeId = model::kInvalidNodeId;
    model::RouteDescriptor _parentQspRoute{};
    uint64_t _parentSourceGeneration = 0;
    uint64_t _parentGeneration = 0;
    uint64_t _parentRouteGeneration = 0;
    uint64_t _sourceGeneration = 0;
};

enum class NodeRuntimeResult : uint8_t {
    Accepted = 0,
    NotRunning,
    InvalidConfiguration,
    UpstreamRejected,
    ChildRejected,
    IdentityRejected,
    ProjectionRejected,
    CacheRejected,
    ArchiveRejected,
    TerminalRejected,
};

const char* nodeRuntimeResultName(NodeRuntimeResult result);

struct NodeRuntimeDownstreamDatagram {
    qnetplay::spectator::EndpointId destination = 0;
    qnetplay::spectator::ViewerId viewer = 0;
    qnetplay::spectator::MessageType type =
        qnetplay::spectator::MessageType::Reject;
    std::vector<uint8_t> bytes;
};

class NodeRuntimeChildIngress {
public:
    NodeRuntimeChildIngress(const NodeRuntimeChildIngress&) = default;
    NodeRuntimeChildIngress& operator=(const NodeRuntimeChildIngress&) =
        default;
    NodeRuntimeChildIngress(NodeRuntimeChildIngress&&) noexcept = default;
    NodeRuntimeChildIngress& operator=(NodeRuntimeChildIngress&&) noexcept =
        default;

private:
    explicit NodeRuntimeChildIngress(
        model::RouteDescriptor observedQspRoute,
        uint64_t endpointBindingGeneration,
        qnetplay::spectator::EndpointId endpoint)
        : _observedQspRoute(std::move(observedQspRoute)),
          _endpointBindingGeneration(endpointBindingGeneration),
          _endpoint(endpoint) {}

    friend class ResilientSpectatorNodeRuntime;
    friend class
        ::qclient::spectator::topology::client::ReleasedTopologyClientRuntime;
#if defined(QOH99_NODE_RUNTIME_TEST_SEAM)
    friend struct test::NodeRuntimeChildIngressTestFactory;
#endif

    model::RouteDescriptor _observedQspRoute{};
    uint64_t _endpointBindingGeneration = 0;
    qnetplay::spectator::EndpointId _endpoint = 0;
};

// Single-thread-owned: a Sidecar service turn must serialize upstream ingress, child ingress, projection, and outbound polling.
class ResilientSpectatorNodeRuntime {
public:
    ResilientSpectatorNodeRuntime() = default;
    ~ResilientSpectatorNodeRuntime();

    ResilientSpectatorNodeRuntime(const ResilientSpectatorNodeRuntime&) = delete;
    ResilientSpectatorNodeRuntime& operator=(
        const ResilientSpectatorNodeRuntime&) = delete;

    bool start(const ResilientSpectatorNodeRuntimeConfig& config);
    void stop();
    bool running() const { return _running; }

    NodeRuntimeResult onUpstreamDatagram(const uint8_t* bytes,
                                         size_t length,
                                         uint64_t nowMs);
    std::vector<qnetplay::spectator::ViewerDatagram> pollUpstreamOutbound(
        uint64_t nowMs, size_t packetBudget = 4);

    NodeRuntimeResult registerAuthenticatedChild(
        const model::NodeAdvertisement& child,
        const model::ParentLease& lease, uint64_t nowMs);

    NodeRuntimeResult registerQtaVerifiedChild(
        QtaVerifiedParentLease&& verifiedLease, uint64_t nowMs);
    NodeRuntimeResult registerQtaVerifiedChild(
        QtaVerifiedParentLease&& verifiedLease, uint64_t nowMs,
        std::optional<QtaVerifiedChildRetirement>& outRetirement);

    NodeRuntimeResult projectCanonicalVerifiedPrefix(
        const QtaVerifiedCanonicalIngress& verifiedIngress,
        const qnetplay::spectator::SpectatorViewer& verifiedViewer,
        uint64_t nowMs);

    NodeRuntimeResult onChildDatagram(const NodeRuntimeChildIngress& ingress,
                                      const uint8_t* bytes,
                                      size_t length,
                                      uint64_t nowMs);
    std::vector<NodeRuntimeDownstreamDatagram> pollChildOutbound(
        uint64_t nowMs, size_t packetBudget = 0);

    bool noteUpstreamFailure(model::UpstreamFailureReason reason,
                             uint64_t nowMs);
    bool noteCanonicalAuthorityLoss(
        const QtaVerifiedCanonicalIngress& verifiedIngress,
        uint64_t nowMs);
    void tick(uint64_t nowMs);

    const model::ResilientRelayNode* relayNode() const;
    const qnetplay::spectator::SpectatorArchive* downstreamArchive() const;
    const qnetplay::spectator::SpectatorViewer* upstreamViewer() const;
    bool hasActiveChild(model::NodeId childNodeId) const;

private:
    friend class
        ::qclient::spectator::topology::client::ReleasedTopologyClientRuntime;
#if defined(QOH99_NODE_RUNTIME_TEST_SEAM)
    friend struct test::QtaVerifiedChildRetirementTestFactory;
#endif

    struct ChildQspBinding {
        model::NodeId nodeId = model::kInvalidNodeId;
        model::RouteDescriptor qspRoute{};
        uint64_t endpointBindingGeneration = 0;
        qnetplay::spectator::EndpointId endpoint = 0;
        qnetplay::spectator::ViewerId viewerId = 0;
        uint32_t viewerGeneration = 0;
        model::VerifiedChainCursor requestedCursor{};
        uint64_t handshakeDeadlineMs = 0;
        uint64_t replayNonce = 0;
        model::ParentLeaseChildBinding childBinding{};
        std::shared_ptr<QtaAuthorityContext> authority;
        bool endpointPinned = false;
        bool sourceAttached = false;
    };

    bool validConfig(const ResilientSpectatorNodeRuntimeConfig& config) const;
    bool canonicalIngress() const;
    bool canonicalAuthorityActive(uint64_t nowMs) const;
    bool bindCanonicalAuthority(
        const QtaVerifiedCanonicalIngress& verifiedIngress,
        const qnetplay::spectator::SpectatorViewer& verifiedViewer,
        uint64_t nowMs);
    void disableCanonicalAuthority();
    bool constrainedLeaseChild(const model::ParentLease& lease,
                               model::NodeAdvertisement& out) const;
    bool suppliedChildMatchesLease(
        const model::NodeAdvertisement& supplied,
        const model::NodeAdvertisement& constrained) const;
    NodeRuntimeResult admitLeaseChild(const model::NodeAdvertisement& child,
                                      const model::ParentLease& lease,
                                      std::shared_ptr<QtaAuthorityContext>
                                          qtaAuthority,
                                      uint64_t nowMs,
                                      QtaVerifiedParentLease::Claims*
                                          qtaClaims = nullptr,
                                      std::optional<
                                          QtaVerifiedChildRetirement>*
                                          retirementOut = nullptr);
    bool startDownstreamFromVerifiedGameStart(
        const qnetplay::spectator::GameStartPayload& start);
    NodeRuntimeResult projectVerifiedPrefix();
    NodeRuntimeResult projectVerifiedPrefixFromViewer(
        const qnetplay::spectator::SpectatorViewer& viewer);
    bool projectTerminal();
    bool projectTerminalFromViewer(
        const qnetplay::spectator::SpectatorViewer& viewer);
    NodeRuntimeResult acceptCanonicalVerifiedRecord(
        const qnetplay::spectator::ConfirmedRecord& record);
    NodeRuntimeResult acceptCanonicalVerifiedSetEvent(
        const qnetplay::spectator::SetEvent& event);
    bool sealCanonicalVerifiedTerminal(
        const qnetplay::spectator::ArchiveTerminal& terminal);
    ChildQspBinding* childBindingForIngress(
        const NodeRuntimeChildIngress& ingress);
    bool attachAuthenticatedChild(ChildQspBinding& binding,
                                  const NodeRuntimeChildIngress& ingress,
                                  const uint8_t* bytes,
                                  size_t length);
    bool prevalidateChildHello(
        const ChildQspBinding& binding,
        const qnetplay::spectator::HelloPayload& hello) const;
    bool bindAcceptedHello(ChildQspBinding& binding,
                           const NodeRuntimeChildIngress& ingress,
                           const qnetplay::spectator::HelloPayload& hello);
    bool prevalidateAuthenticatedAcknowledgement(
        const ChildQspBinding& binding, const uint8_t* bytes,
        size_t length) const;
    bool acknowledgeAuthenticatedChild(const ChildQspBinding& binding,
                                      const uint8_t* bytes,
                                      size_t length);
    void sweepChildBindings(uint64_t nowMs);
    void retireChildBinding(size_t index);
    NodeRuntimeResult retireQtaVerifiedChild(
        QtaVerifiedChildRetirement&& verifiedRetirement,
        uint64_t nowMs);
    ChildQspBinding* childBinding(model::NodeId childNodeId);
    const ChildQspBinding* childBinding(model::NodeId childNodeId) const;
    bool bindingRouteUsedByDifferentChild(
        model::NodeId childNodeId, const model::RouteDescriptor& route,
        uint64_t endpointBindingGeneration) const;
    bool bindingViewerUsedByDifferentChild(
        model::NodeId childNodeId,
        qnetplay::spectator::ViewerId viewerId) const;
    bool bindingEndpointUsedByDifferentChild(
        model::NodeId childNodeId,
        qnetplay::spectator::EndpointId endpoint) const;

    bool _running = false;
    bool _downstreamStarted = false;
    bool _downstreamSealed = false;
    uint32_t _projectedRecord = 0;
    uint32_t _projectedEvent = 0;
    ResilientSpectatorNodeRuntimeConfig _config{};
    std::unique_ptr<model::ResilientTopologyCoordinator> _topology;
    std::unique_ptr<model::ParentLeaseRegistry> _leases;
    std::unique_ptr<model::ResilientRelayNode> _relay;
    std::unique_ptr<qnetplay::spectator::SpectatorViewer> _upstream;
    std::unique_ptr<qnetplay::spectator::SpectatorArchive> _archive;
    std::unique_ptr<qnetplay::spectator::SpectatorSource> _downstream;
    std::vector<ChildQspBinding> _childBindings;
    std::shared_ptr<QtaAuthorityContext> _canonicalAuthority;
};

}
