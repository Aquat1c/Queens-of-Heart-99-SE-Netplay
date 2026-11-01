#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "client/spectator/ResilientTopologyAuthority.h"
#include "client/spectator/ResilientTopologyClient.h"
#include "client/spectator/ResilientSpectatorNodeRuntime.h"
#include "client/spectator/TopologyAuthoritySignature.h"
#include "spectator/TopologyRelayAdapter.h"
#include "net/ControlLink.h"
#include "net/Protocol.h"
#include "net/Sockets.h"
#include "spectator/SpectatorGrant.h"

namespace qclient::spectator::topology::client {

namespace testing {
struct ReleasedTopologyClientRuntimeTestAccess;
}

struct ReleasedTopologyClientConfig {
    bool enabled = false;
    qnet::Endpoint hostControl{};
    RouteEndpoint qspEndpoint{};
    RouteEndpoint rootQspEndpoint{};
    uint8_t qspRoute = qnet::SPECTATE_ROUTE_NONE;
    EndpointProvenance qspProvenance = EndpointProvenance::Unknown;
    uint64_t qspRouteGeneration = 0u;
    uint64_t viewerId = 0u;
    uint32_t viewerGeneration = 0u;
    uint64_t sessionId = 0u;
    uint64_t sessionEpoch = 0u;
    uint64_t spectatorCookieLow = 0u;
    uint64_t spectatorCookieHigh = 0u;
    qnetplay::compat::CompatibilityIdentityWire compatibility{};
    uint64_t streamRootIdentity = 0u;
    qnetplay::spectator::SpectatorGrant grant{};
    bool relayQsrPermitted = false;
};

enum class ReleasedTopologyClientState : uint8_t {
    Disabled = 0,
    WaitingForInitAck,
    ControlConnecting,
    AwaitingHostVersion,
    AwaitingControlChallenge,
    AwaitingEnrollmentResponse,
    AwaitingRootOffer,
    AwaitingReceipt,
    Attached,
    Unavailable,
};

struct ReleasedTopologyClientStatus {
    ReleasedTopologyClientState state = ReleasedTopologyClientState::Disabled;
    ClientState transcriptState = ClientState::Disabled;
    ClientFailure transcriptFailure = ClientFailure::None;
    const char* reason = "disabled";
};

const char* releasedTopologyClientStateName(ReleasedTopologyClientState state);

struct ReleasedTopologyAttachmentSnapshot {
    model::StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    model::NodeId rootNodeId = model::kInvalidNodeId;
    model::NodeId childNodeId = model::kInvalidNodeId;
    uint64_t parentSourceGeneration = 0u;
    uint64_t parentRouteGeneration = 0u;
    model::RouteDescriptor parentDataRoute{};
    model::ParentLeaseChildBinding childBinding{};
    model::VerifiedChainCursor committedCursor{};
    uint64_t credentialDeadlineMs = 0u;
    qnet::SpectateTopologyQtaLimitsWire qtaLimits{};

    bool valid() const {
        return stream.valid() && authorityGeneration != 0u
            && rootNodeId != model::kInvalidNodeId
            && childNodeId != model::kInvalidNodeId
            && parentSourceGeneration != 0u && parentRouteGeneration != 0u
            && parentDataRoute.valid() && childBinding.valid()
            && committedCursor.digest != 0u
            && credentialDeadlineMs != 0u
            && qtaLimits.maxTopologyNodes != 0u
            && qtaLimits.maxActiveDepth != 0u
            && qtaLimits.maxChildrenPerRoot != 0u
            && qtaLimits.maxChildrenPerRelay != 0u
            && qtaLimits.maxStandbyParents != 0u
            && qtaLimits.standbyLeaseMs != 0u;
    }
};

struct ReleasedTopologyParentRouteActivation {
    RouteEndpoint currentParentEndpoint{};
    RouteEndpoint candidateDataEndpoint{};
    uint64_t sourceGeneration = 0u;
    uint64_t routeGeneration = 0u;
    model::VerifiedChainCursor commonCursor{};
    uint64_t localDeadlineMs = 0u;
    uint64_t activationNonce = 0u;

    bool valid() const {
        return currentParentEndpoint.valid() && candidateDataEndpoint.valid()
            && currentParentEndpoint != candidateDataEndpoint
            && sourceGeneration != 0u
            && routeGeneration != 0u && commonCursor.digest != 0u
            && localDeadlineMs != 0u && activationNonce != 0u;
    }
};

struct ReleasedTopologyRelayBindingSnapshot {
    model::StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    model::NodeId rootNodeId = model::kInvalidNodeId;
    model::NodeId childNodeId = model::kInvalidNodeId;
    model::RouteDescriptor observedDataRoute{};
    model::RouteDescriptor observedControlRoute{};
    uint64_t credentialDeadlineMs = 0u;

    bool valid() const {
        return stream.valid() && authorityGeneration != 0u
            && rootNodeId != model::kInvalidNodeId
            && childNodeId != model::kInvalidNodeId
            && rootNodeId != childNodeId
            && observedDataRoute.valid() && observedDataRoute.routeProved
            && observedDataRoute.kind == model::RouteKind::Direct
            && observedControlRoute.valid() && observedControlRoute.routeProved
            && observedControlRoute.kind == model::RouteKind::Direct
            && !observedDataRoute.sameEndpoint(observedControlRoute)
            && observedDataRoute.generation != 0u
            && observedDataRoute.generation
                == observedControlRoute.generation
            && credentialDeadlineMs != 0u;
    }
};

inline constexpr uintptr_t kInvalidReleasedTopologyRelaySocket =
    ~static_cast<uintptr_t>(0);

// Re-register the WSA event when either field changes: Windows may recycle a numeric SOCKET after close.
struct ReleasedTopologyRelaySocketBorrow {
    uintptr_t socket = kInvalidReleasedTopologyRelaySocket;
    uint64_t generation = 0u;

    bool valid() const {
        return socket != kInvalidReleasedTopologyRelaySocket
            && generation != 0u;
    }
};

struct ReleasedTopologyRelayDatagram {
    RouteEndpoint destination{};
    std::vector<uint8_t> bytes;

    bool valid() const {
        return destination.valid() && !bytes.empty();
    }
};

enum class ReleasedTopologyRelayBindingState : uint8_t {
    Inactive = 0,
    Pending,
    Ready,
    Unavailable,
};

const char* releasedTopologyRelayBindingStateName(
    ReleasedTopologyRelayBindingState state);

class ReleasedTopologyClientRuntime final {
public:
    ReleasedTopologyClientRuntime();
    ~ReleasedTopologyClientRuntime();

    ReleasedTopologyClientRuntime(const ReleasedTopologyClientRuntime&) = delete;
    ReleasedTopologyClientRuntime& operator=(
        const ReleasedTopologyClientRuntime&) = delete;

    bool start(const ReleasedTopologyClientConfig& config, uint64_t nowMs);

    bool onQspInitAckTransmitted(uint64_t nowMs);

    void service(uint64_t nowMs);

    bool updateVerifiedQspFrontier(uint32_t nextRecord, uint32_t nextEvent,
                                   uint64_t digest);

    void stop();
    bool attached() const { return _state == ReleasedTopologyClientState::Attached; }
    bool active() const { return _state != ReleasedTopologyClientState::Disabled
                                && _state != ReleasedTopologyClientState::Unavailable; }
    ReleasedTopologyClientStatus status() const;
    bool attachmentSnapshot(ReleasedTopologyAttachmentSnapshot& out) const;
    bool parentRouteActivation(
        ReleasedTopologyParentRouteActivation& out, uint64_t nowMs) const;
    bool completeParentRouteActivationAfterIngress(
        uint64_t activationNonce, const RouteEndpoint& acceptedParent,
        uint32_t nextRecord, uint32_t nextEvent, uint64_t digest,
        uint64_t nowMs);
    bool beginRelayDataBinding(uintptr_t& ownedSocket, uint64_t nowMs);
    ReleasedTopologyRelayBindingState relayDataBindingState() const;
    bool relayDataBindingSnapshot(
        ReleasedTopologyRelayBindingSnapshot& out) const;
    ReleasedTopologyRelaySocketBorrow borrowedRelayDataSocket() const;

    bool startRelayNode(
        const qnetplay::spectator::SpectatorViewer& verifiedViewer,
        uint64_t nowMs);
    qclient::spectator::resilient::NodeRuntimeResult
    projectRelayViewer(
        const qnetplay::spectator::SpectatorViewer& verifiedViewer,
        uint64_t nowMs);
    qclient::spectator::resilient::NodeRuntimeResult
    ingestRelayChildDatagram(
        const RouteEndpoint& observedSource,
        const uint8_t* bytes, size_t length, uint64_t nowMs);
    qclient::spectator::resilient::NodeRuntimeResult
    ingestRelayChildDatagram(
        const RouteEndpoint& observedSource,
        qnetplay::spectator::EndpointId endpointToken,
        const uint8_t* bytes, size_t length, uint64_t nowMs);
    std::vector<ReleasedTopologyRelayDatagram>
    pollRelayChildDatagrams(uint64_t nowMs, size_t packetBudget = 0u);
    std::vector<qclient::spectator::resilient::NodeRuntimeDownstreamDatagram>
    pollRelayChildOutbound(uint64_t nowMs, size_t packetBudget = 0u);
    bool relayNodeRunning() const;
    void stopRelayNode();

private:
    friend struct testing::ReleasedTopologyClientRuntimeTestAccess;

    struct TranscriptFacts;
    class TranscriptBuilder;

    bool validateConfig(const ReleasedTopologyClientConfig& config) const;
    bool beginControl(uint64_t nowMs);
    bool restartControl(uint64_t nowMs);
    void serviceControl(uint64_t nowMs);
    void serviceActions(uint64_t nowMs);
    void serviceQsr(uint64_t nowMs);
    bool sendEnrollmentProof(const ClientAction& action, uint64_t nowMs);
    bool acceptEnrollmentInfo(const qnet::SpectateTopologyInfoV2& info,
                              uint64_t nowMs);
    bool setupQsrBootstrap(uint64_t nowMs);
    bool setupRelayQsrBootstrap(uint64_t nowMs);
    void serviceRelayQsrAdmission(uint64_t nowMs);
    bool acceptRelayQsrWelcome(const uint8_t* bytes, size_t length,
                               uint64_t nowMs);
    bool sendQsrDatagram(const std::vector<uint8_t>& envelope);
    bool sendQsrAction(const ClientAction& action, uint64_t nowMs);
    bool acceptQsrControl(const uint8_t* bytes, size_t length,
                          uint64_t nowMs);
    bool acceptRootOffer(const model::ParentLease& lease,
                         uint64_t nowMs);
    bool acceptParentCommit(
        const qnetplay::spectator::resilient::wire::ParentCommit& commit,
        uint64_t nowMs);
    bool makeRelayDataBindingRequest(
        qnetplay::spectator::resilient::wire::RelayDataBindingRequest& out)
        const;
    bool sendRelayDataBinding(uint64_t nowMs);
    void serviceRelayDataBinding(uint64_t nowMs);
    bool acceptRelayDataBindingAck(
        const qnetplay::spectator::resilient::wire::RelayDataBindingAck& ack,
        uint64_t nowMs);
    bool makeRelayReady(
        qnetplay::spectator::resilient::wire::RelayReady& out) const;
    bool sendRelayReady(uint64_t nowMs);
    void serviceRelayReady(uint64_t nowMs);
    bool acceptRelayChildAssignment(
        const qnetplay::spectator::resilient::wire::RelayChildAssignment&
            assignment,
        uint64_t nowMs);
    bool acceptRelayChildActivation(
        const qnetplay::spectator::resilient::wire::RelayChildActivation&
            activation,
        uint64_t nowMs);
    bool acceptAttachedRelayActivation(
        const qnetplay::spectator::resilient::wire::RelayChildActivation&
            activation,
        uint64_t nowMs);
    bool acceptRelayChildActivationCommit(
        const qnetplay::spectator::resilient::wire::RelayChildActivationCommit&
            commit,
        uint64_t nowMs);
    bool acceptRelayChildActivationRetirement(
        const qnetplay::spectator::resilient::wire::
            RelayChildActivationRetirement& retirement,
        uint64_t nowMs);
    bool acceptClientPlayerRootActivation(
        const qnetplay::spectator::resilient::wire::
            ClientPlayerRootActivation& activation,
        uint64_t nowMs);
    bool acceptClientPlayerRootActivationCommit(
        const qnetplay::spectator::resilient::wire::
            ClientPlayerRootActivationCommit& commit,
        uint64_t nowMs);
    bool acceptClientPlayerRootActivationRetirement(
        const qnetplay::spectator::resilient::wire::
            ClientPlayerRootActivationRetirement& retirement,
        uint64_t nowMs);
    bool acceptRelayChildRetirement(
        const qnetplay::spectator::resilient::wire::RelayChildRetirement&
            retirement,
        uint64_t nowMs);
    bool makeRelayNodeConfig(
        const qnetplay::spectator::SpectatorViewer& verifiedViewer,
        qclient::spectator::resilient::ResilientSpectatorNodeRuntimeConfig& out)
        const;
    bool relayAuthorityActive(uint64_t nowMs) const;
    void serviceRelayNode(uint64_t nowMs);
    struct RelayChildBinding {
        model::NodeId childNodeId = model::kInvalidNodeId;
        model::RouteDescriptor route{};
        uint64_t endpointBindingGeneration = 0u;
        qnetplay::spectator::EndpointId endpointToken = 0u;
        uint64_t replayNonce = 0u;
        model::LeaseBinding childLease{};
        std::optional<
            qclient::spectator::resilient::QtaVerifiedChildRetirement>
            retirement;
    };
    struct PendingRelayChildActivation {
        qnetplay::spectator::resilient::wire::RelayChildActivation
            activation{};
        uint64_t localDeadlineMs = 0u;
        bool committed = false;
    };
    struct PendingClientPlayerRootActivation {
        qnetplay::spectator::resilient::wire::ClientPlayerRootActivation
            activation{};
        uint64_t localDeadlineMs = 0u;
        bool committed = false;
    };
    const RelayChildBinding* relayChildForSource(
        const RouteEndpoint& observedSource) const;
    const RelayChildBinding* relayChildForEndpoint(
        qnetplay::spectator::EndpointId endpointToken) const;
    void pruneRetiredRelayChildren();
    bool routeMatchesEndpoint(const model::RouteDescriptor& route,
                              const RouteEndpoint& endpoint) const;
    void clearPendingRelayChildActivation();
    void retirePendingRelayChildActivation();
    void clearPendingClientPlayerRootActivation();
    void retirePendingClientPlayerRootActivation();
    void retainCompletedParentRouteActivation(
        const qnetplay::spectator::resilient::wire::RelayChildActivation&
            activation);
    void retainCompletedClientPlayerRootActivation(
        const qnetplay::spectator::resilient::wire::
            ClientPlayerRootActivation& activation);
    void clearPendingAttachedRelayActivation();
    void retirePendingAttachedRelayActivation();
    void retireCommittedAttachedRelayChild(uint64_t nowMs);
    bool activationLocalDeadline(
        const qnetplay::spectator::resilient::wire::RelayChildActivation&
            activation,
        uint64_t acceptedAtMs, uint64_t& out) const;
    bool activationLocalDeadline(
        const qnetplay::spectator::resilient::wire::
            ClientPlayerRootActivation& activation,
        uint64_t acceptedAtMs, uint64_t& out) const;
    void servicePairedActivations(uint64_t nowMs);
    void clearRelayDataBinding(bool unavailable = false);
    bool advanceRelayDataSocketGeneration();
    bool acceptAuthorityState(
        const qnetplay::spectator::resilient::wire::AuthorityState& state,
        uint64_t nowMs);
    void failSoft(const char* reason);
    void clearSensitiveState();

    ResilientTopologyClient _client;
    qnet::UdpSock _controlSocket;
    qnet::UdpControlLink _controlLink;
    qnet::UdpSock _qsrSocket;
    qnet::UdpSock _relayDataSocket;
    bool _relayQsrPermitted = false;
    qnetplay::spectator::RelayQsrOffer _relayQsrOffer{};
    qnetplay::spectator::TopologyRelayNodeAdapter _relayQsrAdapter;
    qnet::Endpoint _relayQsrEndpoint{};
    bool _relayQsrActive = false;
    uint64_t _nextRelayQsrHelloMs = 0u;
    ReleasedTopologyClientState _state = ReleasedTopologyClientState::Disabled;
    ClientFailure _lastTranscriptFailure = ClientFailure::None;
    std::string _reason = "disabled";
    uint64_t _sessionId = 0u;
    uint64_t _sessionEpoch = 0u;
    uint64_t _viewerId = 0u;
    uint32_t _viewerGeneration = 0u;
    uint64_t _correlation = 0u;
    uint64_t _spectatorCookieLow = 0u;
    uint64_t _spectatorCookieHigh = 0u;
    uint64_t _streamRootIdentity = 0u;
    uint64_t _qspRouteGeneration = 0u;
    uint8_t _peerChallenge[16]{};
    uint64_t _controlDeadlineMs = 0u;
    uint64_t _responseDeadlineMs = 0u;
    uint32_t _controlRestartCount = 0u;
    uint64_t _credentialDeadlineMs = 0u;
    uint64_t _livenessDeadlineMs = 0u;
    uint64_t _lastAuthorityStateSequence = 0u;
    uint64_t _offerDeadlineMs = 0u;
    model::VerifiedChainCursor _qspFrontier{};
    bool _qspFrontierObserved = false;
    uint8_t _qspRoute = qnet::SPECTATE_ROUTE_NONE;
    EndpointProvenance _qspProvenance = EndpointProvenance::Unknown;
    RouteEndpoint _qspEndpoint{};
    RouteEndpoint _rootQspEndpoint{};
    qnet::Endpoint _hostControl{};
    qnetplay::compat::CompatibilityIdentityWire _compatibility{};
    qnetplay::spectator::SpectatorGrant _grant{};
    std::unique_ptr<TranscriptFacts> _facts;
    std::unique_ptr<model::ParentLease> _offeredLease;
    std::unique_ptr<model::ParentLease> _attachedLease;
    TopologyAuthoritySignatureVerifier _assignmentVerifier;
    ReleasedTopologyAttachmentSnapshot _attachmentSnapshot{};
    std::shared_ptr<qclient::spectator::resilient::QtaAuthorityContext>
        _qtaAuthority;
    std::unique_ptr<qclient::spectator::resilient::ResilientSpectatorNodeRuntime>
        _relayNode;
    std::unique_ptr<qclient::spectator::resilient::QtaVerifiedCanonicalIngress>
        _relayCanonicalIngress;
    const qnetplay::spectator::SpectatorViewer* _relayViewer = nullptr;
    uint16_t _relayDataLocalPort = 0u;
    uint64_t _relayDataSocketGeneration = 0u;
    uint64_t _relayDataRequestNonce = 0u;
    uint64_t _relayDataNextAttemptMs = 0u;
    uint32_t _relayDataAttemptCount = 0u;
    bool _relayDataBindingPending = false;
    bool _relayDataBindingUnavailable = false;
    ReleasedTopologyRelayBindingSnapshot _relayDataBindingSnapshot{};
    std::vector<RelayChildBinding> _relayChildren;
    std::optional<PendingRelayChildActivation>
        _pendingRelayChildActivation;
    std::optional<PendingRelayChildActivation>
        _retiredRelayChildActivation;
    std::optional<PendingClientPlayerRootActivation>
        _pendingClientPlayerRootActivation;
    std::optional<PendingClientPlayerRootActivation>
        _retiredClientPlayerRootActivation;
    std::optional<qnetplay::spectator::resilient::wire::
        RelayChildActivationRetirement>
        _completedParentRouteActivation;
    std::optional<qnetplay::spectator::resilient::wire::
        ClientPlayerRootActivation>
        _completedClientPlayerRootActivation;
    std::optional<PendingRelayChildActivation>
        _pendingAttachedRelayActivation;
    std::optional<PendingRelayChildActivation>
        _retiredAttachedRelayActivation;
    model::VerifiedChainCursor _lastRelayReadyCursor{};
    uint64_t _relayReadyNextAttemptMs = 0u;
    uint32_t _relayReadyAttemptCount = 0u;
};

}
