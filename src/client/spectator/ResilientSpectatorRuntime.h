#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "client/spectator/SpectatorRoute.h"
#include "spectator/ResilientSpectatorProtocol.h"

namespace qclient::spectator::resilient {

namespace model = qnetplay::spectator::resilient;
namespace wire = qnetplay::spectator::resilient::wire;

class ResilientControlTransport {
public:
    virtual ~ResilientControlTransport() = default;
    virtual bool sendControl(const model::RouteDescriptor& to,
                             const uint8_t* bytes,
                             size_t length) = 0;
};

class ResilientControlAuthorizer {
public:
    virtual ~ResilientControlAuthorizer() = default;
    virtual bool authorize(const RouteEndpoint& from,
                           const wire::ControlDatagram& datagram,
                           uint64_t nowMs) const = 0;

    virtual bool parentLeaseChildBinding(
        model::NodeId childId,
        model::ParentLeaseChildBinding& out) const {
        (void)childId;
        out = {};
        return false;
    }
};

struct ResilientRootControlConfig {
    bool enabled = false;
    model::NodeId nodeId = model::kInvalidNodeId;
    model::NodeKind nodeKind = model::NodeKind::HostRoot;
    model::RouteDescriptor route{};
    uint32_t capabilities = 0;
    size_t childCapacity = 0;
    uint32_t uploadCapacity = 0;
    uint64_t sourceGeneration = 0;
    uint64_t lastVerifiedProgressMs = 0;
    uint64_t leaseExpiryMs = 0;
    model::ResilientLimits limits{};
    qnetplay::spectator::SpectatorGrantKey parentLeaseKey{};
    std::shared_ptr<const ResilientControlAuthorizer> authorizer;
};

struct ResilientRootRuntimeConfig {
    model::StreamCompatibilityIdentity stream{};
    model::NodeAdvertisement localAdvertisement{};
    uint64_t gameId = 0;
    uint64_t initialDigest = 0;
    uint32_t firstTimelineFrame = 0;
    model::ResilientLimits limits{};
    qnetplay::spectator::SpectatorGrantKey parentLeaseKey{};
    std::shared_ptr<const ResilientControlAuthorizer> authorizer;
};

enum class ControlIngressResult : uint8_t {
    AcceptedAdvertisement = 1,
    AcceptedLeaseRedemption = 2,
    AcceptedDrainNotice = 3,
    NotRunning = 4,
    DecodeRejected = 5,
    IdentityRejected = 6,
    AuthenticationRejected = 7,
    EndpointRejected = 8,
    TopologyRejected = 9,
    LeaseRejected = 10,
    UnsupportedMessage = 11,
    AcceptedContinuityRequest = 12,
    ContinuityRejected = 13,
};

const char* controlIngressResultName(ControlIngressResult result);
bool controlIngressAccepted(ControlIngressResult result);

class ResilientSpectatorRootRuntime {
public:
    ResilientSpectatorRootRuntime() = default;
    ~ResilientSpectatorRootRuntime();

    ResilientSpectatorRootRuntime(const ResilientSpectatorRootRuntime&) = delete;
    ResilientSpectatorRootRuntime& operator=(
        const ResilientSpectatorRootRuntime&) = delete;

    bool start(const ResilientRootRuntimeConfig& config,
               ResilientControlTransport& transport);
    void stop();
    bool running() const { return _running; }

    const model::StreamCompatibilityIdentity* stream() const;
    const model::ResilientTopologyCoordinator* topology() const;
    model::ResilientTopologyCoordinator* topology();
    const model::VerifiedRelayCache* cache() const;

    bool noteVerifiedProgress(uint64_t nowMs);

    bool detachChild(model::NodeId childId);

    model::CacheAppendResult acceptArchiveRecord(
        const qnetplay::spectator::ConfirmedRecord& record);
    model::CacheEventAppendResult acceptArchiveEvent(
        const qnetplay::spectator::SetEvent& event);

    size_t issuedLeaseCount() const { return _issuedLeases.size(); }

    ControlIngressResult onDatagram(const RouteEndpoint& from,
                                    const uint8_t* bytes,
                                    size_t length,
                                    uint64_t nowMs);

    bool sendAdvertisement(const model::RouteDescriptor& to);
    bool issueAndSendParentOffer(model::NodeId childId,
                                 uint32_t requiredCursor,
                                 uint64_t expiryMs,
                                 uint64_t replayNonce);
    bool resendParentOffer(model::NodeId childId);
    bool sendParentCommit(const model::ParentLease& lease);
    bool sendDrainNotice(const model::RouteDescriptor& to,
                         const model::DrainNotice& notice);

private:
    static bool supportedUdpRoute(const model::RouteDescriptor& route);
    static bool endpointMatches(const RouteEndpoint& endpoint,
                                const model::RouteDescriptor& route);
    static const model::StreamCompatibilityIdentity* streamOf(
        const wire::ControlDatagram& datagram);
    bool send(const model::RouteDescriptor& to,
              const wire::ControlDatagram& datagram);
    bool validConfig(const ResilientRootRuntimeConfig& config) const;
    bool refreshLocalCacheAdvertisement();
    bool childBindingFor(model::NodeId childId,
                         model::ParentLeaseChildBinding& out) const;
    bool cacheSupportsChildBinding(
        const model::ParentLeaseChildBinding& binding) const;
    void expireIssuedLeases(uint64_t nowMs);
    bool retainIssuedLease(const model::ParentLease& lease);
    const model::ParentLease* findIssuedLease(
        const model::ParentLease& lease) const;
    const model::ParentLease* findIssuedLease(
        const model::LeaseBinding& binding) const;
    bool verifyContinuityRequest(
        const model::ContinuityRequest& request,
        const model::ParentLease& issued,
        const RouteEndpoint& from,
        uint64_t nowMs) const;

    bool _running = false;
    ResilientRootRuntimeConfig _config{};
    ResilientControlTransport* _transport = nullptr;
    std::unique_ptr<model::ResilientTopologyCoordinator> _topology;
    std::unique_ptr<model::ParentLeaseRegistry> _leases;
    std::unique_ptr<model::VerifiedRelayCache> _cache;
    std::vector<model::ParentLease> _issuedLeases;
};

}
