#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "client/spectator/ResilientSpectatorRuntime.h"
#include "client/spectator/TopologyAuthoritySignature.h"
#include "netplay/ConfigurationProvenance.h"
#include "spectator/TopologyRelayAdapter.h"

namespace qclient::spectator::topology {

namespace model = qnetplay::spectator::resilient;
namespace wire = qnetplay::spectator::resilient::wire;

constexpr uint32_t kTopologyControlMagic =
    wire::kTopologyQtaEnvelopeMagic;
constexpr uint16_t kTopologyControlVersion = wire::kTopologyQtaEnvelopeVersion;
constexpr size_t kTopologyCredentialBytes = 32u;
constexpr size_t kTopologyMacBytes = wire::kTopologyQtaEnvelopeMacBytes;

bool hasTopologyControlEnvelopeMagic(const uint8_t* bytes, size_t length);

bool reserveTopologyControlSequence(uint64_t& next, uint64_t& reserved);

enum class EndpointProvenance : uint8_t {
    Unknown = 0,
    LocalBind = 1,
    PublishedDirectCandidate = 2,
    ProvenDirectQsp = 3,
    ProvenPunchedQsp = 4,
    ProvenRelayQsp = 5,
};

inline bool relayProvenance(const EndpointProvenance provenance) {
    return provenance == EndpointProvenance::ProvenRelayQsp;
}

struct EndpointBinding {
    RouteEndpoint local{};
    model::RouteDescriptor advertised{};
    EndpointProvenance localProvenance = EndpointProvenance::Unknown;
    EndpointProvenance advertisedProvenance = EndpointProvenance::Unknown;
    uint64_t generation = 0;

    bool validForAuthority() const;
};

struct TopologyActivationRequest {
    bool requested = false;
    EndpointBinding rootQsp{};
    uint64_t sourceGeneration = 0;
    model::ResilientLimits limits{};
    bool forceQsrBindFailure = false;
    bool forceQsrServiceFailure = false;
};

struct TopologyAuthorityFacts {
    qnetplay::spectator::SessionIdentity session{};
    qnetplay::compat::CompatibilityIdentityWire compatibility{};
    uint64_t streamRootIdentity = 0;
    uint64_t gameStartFingerprint = 0;
    uint64_t sourceGeneration = 0;
    uint64_t authorityGeneration = 0;
    model::NodeId rootNodeId = model::kInvalidNodeId;
    EndpointBinding rootQsp{};
    EndpointBinding rootQsr{};
    model::ResilientLimits limits{};
    TopologyAuthorityPublicBlob assignmentSigningPublicKey{};
    uint64_t createdMs = 0;
    uint64_t expiresMs = 0;
    bool revoked = false;

    bool complete() const;
};

struct TopologyViewerProof {
    uint64_t viewerId = 0;
    uint32_t viewerGeneration = 0;
    RouteEndpoint endpoint{};
    EndpointProvenance provenance = EndpointProvenance::Unknown;
    uint64_t routeGeneration = 0;
    uint32_t confirmedCursor = 0;
    bool qspStreaming = false;
    bool relayCapable = false;
};

struct TopologyViewerClaim {
    uint64_t sessionId = 0;
    uint64_t sessionEpoch = 0;
    uint64_t viewerId = 0;
    uint32_t viewerGeneration = 0;
    RouteEndpoint qspEndpoint{};
    EndpointProvenance qspProvenance = EndpointProvenance::Unknown;
    uint64_t qspRouteGeneration = 0;
    uint64_t discoveryGeneration = 0;
    qnetplay::compat::CompatibilityIdentityWire compatibility{};
    uint64_t streamRootIdentity = 0;
};

struct ClientPlayerRootClaim {
    uint64_t sessionId = 0u;
    uint64_t sessionEpoch = 0u;
    wire::ClientPlayerRootRole remotePlayerRole =
        wire::ClientPlayerRootRole::RemotePlayerOne;
    qnetplay::compat::CompatibilityIdentityWire compatibility{};
    qnetplay::config_provenance::ConfigurationProvenanceWire
        configurationProvenance{};
    uint64_t streamRootIdentity = 0u;
    uint64_t gameStartFingerprint = 0u;
    uint32_t publisherGeneration = 0u;
    uint64_t publisherNonce = 0u;
    wire::ClientPlayerRootRuntimeReceipt runtimeReceipt{};
    uint64_t sourceGeneration = 0u;
    uint64_t qspRouteGeneration = 0u;
    uint64_t qsrBindingGeneration = 0u;
    model::RouteDescriptor dataRoute{};
    model::RouteDescriptor qsrRoute{};
    model::VerifiedCacheRange cacheRange{};
    model::VerifiedChainCursor cursor{};
    uint32_t childCapacity = 0u;
    uint64_t expiresMs = 0u;
};

bool writeClientPlayerRootEnvelopeMacInputFromFacts(
    const TopologyAuthorityFacts& facts, uint64_t grantNonce,
    const wire::ClientPlayerRootBinding& binding, uint64_t sequence,
    const uint8_t* qsrBytes, size_t qsrLength, std::vector<uint8_t>& out);
bool encodeClientPlayerRootEnvelopeFromFacts(
    const TopologyAuthorityFacts& facts, uint64_t grantNonce,
    const wire::ClientPlayerRootBinding& binding,
    const std::array<uint8_t, kTopologyCredentialBytes>& credential,
    uint64_t sequence, const uint8_t* qsrBytes, size_t qsrLength,
    std::vector<uint8_t>& out);
bool decodeClientPlayerRootEnvelopeFromFacts(
    const TopologyAuthorityFacts& facts, uint64_t grantNonce,
    const wire::ClientPlayerRootBinding& binding,
    const std::array<uint8_t, kTopologyCredentialBytes>& credential,
    const uint8_t* bytes, size_t length, uint64_t minSequenceExclusive,
    const uint8_t*& outPayload, size_t& outPayloadLength,
    uint64_t& outSequence);

struct ClientPlayerRootDescriptor {
    bool enrolled = false;
    bool asserted = false;
    uint64_t grantNonce = 0u;
    wire::ClientPlayerRootBinding binding{};
};

// `credential` must never be logged or placed in a spectator ViewerProof/E0 grant.
struct ClientPlayerRootEnrollment {
    bool granted = false;
    model::NodeId clientRootNodeId = model::kInvalidNodeId;
    model::NodeId hostRootNodeId = model::kInvalidNodeId;
    uint64_t authorityGeneration = 0u;
    uint64_t expiresMs = 0u;
    wire::ClientPlayerRootGrant grant{};
    std::array<uint8_t, kTopologyCredentialBytes> credential{};
    EndpointBinding rootQsp{};
    EndpointBinding rootQsr{};
    uint64_t hostSourceGeneration = 0u;
    TopologyAuthorityPublicBlob assignmentSigningPublicKey{};
};

struct TopologyEnrollment {
    bool granted = false;
    model::NodeId nodeId = model::kInvalidNodeId;
    model::NodeId rootNodeId = model::kInvalidNodeId;
    uint64_t authorityGeneration = 0;
    uint64_t expiresMs = 0;
    uint64_t bindingGeneration = 0;
    EndpointBinding rootQsp{};
    EndpointBinding rootQsr{};
    qnetplay::spectator::SessionIdentity session{};
    qnetplay::compat::CompatibilityIdentityWire compatibility{};
    uint64_t streamRootIdentity = 0;
    uint64_t sourceGeneration = 0;
    uint64_t viewerId = 0;
    uint32_t viewerGeneration = 0;
    RouteEndpoint qspEndpoint{};
    EndpointProvenance qspProvenance = EndpointProvenance::Unknown;
    uint64_t qspRouteGeneration = 0;
    uint32_t confirmedCursor = 0;
    model::VerifiedChainCursor qtaCursor{};
    model::ResilientLimits limits{};
    TopologyAuthorityPublicBlob assignmentSigningPublicKey{};
    std::array<uint8_t, kTopologyCredentialBytes> credential{};
    // The grant inside is a bearer token: never log it or emit it for a direct or punched enrollment.
    qnetplay::spectator::RelayQsrOffer relayQsrOffer{};
};

struct AuthorityStateRecipient {
    model::RouteDescriptor qsrRoute{};
    wire::AuthorityState state{};
};

struct RelayChildRetirementRecipient {
    model::RouteDescriptor qsrRoute{};
    wire::RelayChildRetirement retirement{};
};

struct RelayChildActivationRetirementRecipient {
    model::RouteDescriptor qsrRoute{};
    wire::RelayChildActivationRetirement retirement{};
};

struct RelayChildActivationCommitRecipient {
    model::RouteDescriptor qsrRoute{};
    wire::RelayChildActivationCommit commit{};
};

struct ClientPlayerRootActivationCommitRecipient {
    model::RouteDescriptor qsrRoute{};
    wire::ClientPlayerRootActivationCommit commit{};
};

struct ClientPlayerRootActivationRetirementRecipient {
    model::RouteDescriptor qsrRoute{};
    wire::ClientPlayerRootActivationRetirement retirement{};
};

struct ClientDataRootDescriptor {
    bool registered = false;
    model::NodeId nodeId = model::kInvalidNodeId;
    model::StreamCompatibilityIdentity stream{};
    uint64_t gameStartFingerprint = 0u;
    uint64_t authorityGeneration = 0u;
    uint64_t qspRouteGeneration = 0u;
    uint64_t qsrBindingGeneration = 0u;
    uint64_t sourceGeneration = 0u;
    model::RouteDescriptor dataRoute{};
    model::RouteDescriptor qsrRoute{};
    model::VerifiedCacheRange cacheRange{};
    model::VerifiedChainCursor cursor{};
    uint32_t childCapacity = 0u;
    uint64_t expiresMs = 0u;
};

enum class StagedRelayChildActivationResult : uint8_t {
    None = 0,
    Ready,
    Cancelled,
};

struct TopologyControlEnvelope {
    uint64_t authorityGeneration = 0;
    model::NodeId nodeId = model::kInvalidNodeId;
    uint64_t sequence = 0;
    std::array<uint8_t, kTopologyMacBytes> mac{};
    std::vector<uint8_t> qsr{};
};

enum class TopologyControlResult : uint8_t {
    Accepted = 1,
    NotReady,
    Malformed,
    UnknownNode,
    AuthorityMismatch,
    Expired,
    EndpointMismatch,
    Replay,
    AuthenticationRejected,
    BindingRejected,
};

const char* topologyControlResultName(TopologyControlResult result);

class TopologyAuthority final
    : public resilient::ResilientControlAuthorizer {
public:
    enum class AttachmentOwner : uint8_t {
        None = 0,
        Root,
        RelayActivation,
    };

    struct ViewerLifecycleResult {
        model::NodeId nodeId = model::kInvalidNodeId;
        AttachmentOwner releasedAttachmentOwner = AttachmentOwner::None;
        bool releasedAttachment = false;

        bool requiresGenericRootDetach() const {
            return releasedAttachment
                && releasedAttachmentOwner == AttachmentOwner::Root;
        }
    };

    TopologyAuthority() = default;
    ~TopologyAuthority();

    TopologyAuthority(const TopologyAuthority&) = delete;
    TopologyAuthority& operator=(const TopologyAuthority&) = delete;

    bool initialize(const qnetplay::spectator::SessionIdentity& session,
                    const qnetplay::compat::CompatibilityIdentityWire& compatibility,
                    uint64_t streamRootIdentity,
                    const TopologyActivationRequest& request,
                    const EndpointBinding& qsrBinding,
                    uint64_t nowMs,
                    std::string& error);
    void revoke();
    bool ready() const;
    const TopologyAuthorityFacts& facts() const { return _facts; }
    const qnetplay::spectator::SpectatorGrantKey& leaseKeyForRuntime() const {
        return _leaseKey;
    }

    bool enroll(const TopologyViewerProof& proof, uint64_t nowMs,
                TopologyEnrollment& out, std::string& error);
    bool detachViewer(uint64_t viewerId, ViewerLifecycleResult& out);
    bool detachViewer(uint64_t viewerId, model::NodeId& detachedNodeId);
    bool beginViewerRebind(const TopologyViewerProof& proof,
                           uint64_t nowMs, ViewerLifecycleResult& out,
                           std::string& error);
    bool beginViewerRebind(const TopologyViewerProof& proof,
                           uint64_t nowMs, std::string& error);
    bool refreshViewerBinding(const TopologyViewerProof& proof,
                              uint64_t nowMs, ViewerLifecycleResult& out,
                              std::string& error);
    bool refreshViewerBinding(const TopologyViewerProof& proof,
                              uint64_t nowMs, std::string& error);

    bool encodeControl(const TopologyEnrollment& enrollment,
                       uint64_t sequence,
                       const uint8_t* qsrBytes, size_t qsrLength,
                       std::vector<uint8_t>& out) const;
    TopologyControlResult decodeControl(const RouteEndpoint& from,
                                        const uint8_t* bytes, size_t length,
                                        uint64_t nowMs,
                                        TopologyControlEnvelope& out);

    bool authorize(const RouteEndpoint& from,
                   const wire::ControlDatagram& datagram,
                   uint64_t nowMs) const override;
    bool parentLeaseChildBinding(
        model::NodeId childId,
        model::ParentLeaseChildBinding& out) const override;
    bool reserveRootOffer(model::NodeId childId, uint64_t nowMs,
                          uint64_t& nonce, uint64_t& expiryMs);
    bool confirmRedemption(const model::ParentLease& lease,
                           uint64_t nowMs);
    bool rollbackRedemption(const model::ParentLease& lease);
    void releaseOffer(model::NodeId childId, uint64_t nonce);
    void setVerifiedCursor(const model::VerifiedChainCursor& cursor);
    uint32_t requiredCursor(model::NodeId childId) const;
    bool enrollmentForEndpoint(const RouteEndpoint& endpoint,
                               TopologyEnrollment& out) const;
    bool enrollmentForQsrEndpoint(const RouteEndpoint& endpoint,
                                  TopologyEnrollment& out) const;
    bool enrollmentForNode(model::NodeId nodeId,
                           TopologyEnrollment& out) const;
    bool nodeForViewer(uint64_t viewerId, model::NodeId& out) const;
    bool isAttached(model::NodeId nodeId) const;
    size_t enrolledCount() const;
    size_t attachedCount() const;
    size_t reservedCount() const;
    bool registerClientDataRoot(
        const wire::ClientDataRootAssertion& assertion, uint64_t nowMs,
        ClientDataRootDescriptor& out);
    bool clientDataRootForNode(model::NodeId nodeId, uint64_t nowMs,
                               ClientDataRootDescriptor& out) const;
    size_t clientDataRootCount() const;

    bool enrollClientPlayerRoot(const ClientPlayerRootClaim& claim,
                                uint64_t nowMs,
                                ClientPlayerRootEnrollment& out,
                                std::string& error);
    bool encodeClientPlayerRootControl(
        const ClientPlayerRootEnrollment& enrollment, uint64_t sequence,
        const uint8_t* qsrBytes, size_t qsrLength,
        std::vector<uint8_t>& out) const;
    TopologyControlResult decodeClientPlayerRootControl(
        const RouteEndpoint& from, const uint8_t* bytes, size_t length,
        uint64_t nowMs, TopologyControlEnvelope& out);
    bool clientPlayerRootForNode(model::NodeId nodeId, uint64_t nowMs,
                                 ClientPlayerRootDescriptor& out) const;
    size_t clientPlayerRootCount() const;

    std::string diagnostics() const;

    bool serviceExpiry(uint64_t nowMs,
                       std::vector<model::NodeId>& detachedNodes);

    bool collectAuthorityState(
        wire::AuthorityStateKind kind, wire::AuthorityStateReason reason,
        uint64_t nowMs, std::vector<AuthorityStateRecipient>& out,
        model::NodeId onlyChild = model::kInvalidNodeId);

    bool childAdvertisement(model::NodeId nodeId,
                            model::NodeAdvertisement& out) const;
    bool relayDataBindingAck(
        model::NodeId nodeId,
        const wire::RelayDataBindingRequest& request,
        wire::RelayDataBindingAck& out) const;
    bool issueRelayChildAssignment(
        model::NodeId relayNodeId, model::NodeId childNodeId,
        uint64_t nowMs, wire::RelayChildAssignment& out,
        model::RouteDescriptor& outRelayQsrRoute);

    bool issueRelayChildActivation(
        model::NodeId relayNodeId, model::NodeId childNodeId,
        uint64_t nowMs, wire::RelayChildActivation& out,
        model::RouteDescriptor& outRelayQsrRoute,
        model::RouteDescriptor& outChildQsrRoute);

    bool issueClientPlayerRootActivation(
        model::NodeId clientRootNodeId, model::NodeId targetViewerNodeId,
        uint64_t nowMs, wire::ClientPlayerRootActivation& out,
        model::RouteDescriptor& outTargetQsrRoute);

    bool stageRelayChildActivation(model::NodeId relayNodeId,
                                   model::NodeId childNodeId,
                                   uint64_t nowMs);
    StagedRelayChildActivationResult consumeStagedRelayChildActivation(
        model::NodeId childNodeId, uint64_t nowMs,
        model::NodeId& outRelayNodeId);

    bool nextRelayChildRetirement(
        RelayChildRetirementRecipient& out) const;
    bool encodeRelayChildRetirementControl(
        const RelayChildRetirementRecipient& recipient, uint64_t sequence,
        const uint8_t* qsrBytes, size_t qsrLength,
        std::vector<uint8_t>& out) const;
    bool acknowledgeRelayChildRetirement(
        const RelayChildRetirementRecipient& recipient);
    bool nextRelayChildActivationRetirement(
        RelayChildActivationRetirementRecipient& out) const;
    bool encodeRelayChildActivationRetirementControl(
        const RelayChildActivationRetirementRecipient& recipient,
        uint64_t sequence, const uint8_t* qsrBytes, size_t qsrLength,
        std::vector<uint8_t>& out) const;
    bool acknowledgeRelayChildActivationRetirement(
        const RelayChildActivationRetirementRecipient& recipient);
    bool nextRelayChildActivationCommit(
        RelayChildActivationCommitRecipient& out) const;
    bool encodeRelayChildActivationCommitControl(
        const RelayChildActivationCommitRecipient& recipient,
        uint64_t sequence, const uint8_t* qsrBytes, size_t qsrLength,
        std::vector<uint8_t>& out) const;
    bool markRelayChildActivationCommitSent(
        const RelayChildActivationCommitRecipient& recipient,
        uint64_t nowMs);

    bool nextClientPlayerRootActivationCommit(
        ClientPlayerRootActivationCommitRecipient& out) const;
    bool encodeClientPlayerRootActivationCommitControl(
        const ClientPlayerRootActivationCommitRecipient& recipient,
        uint64_t sequence, const uint8_t* qsrBytes, size_t qsrLength,
        std::vector<uint8_t>& out) const;
    bool acknowledgeClientPlayerRootActivationCommit(
        const ClientPlayerRootActivationCommitRecipient& recipient);
    bool nextClientPlayerRootActivationRetirement(
        ClientPlayerRootActivationRetirementRecipient& out) const;
    bool encodeClientPlayerRootActivationRetirementControl(
        const ClientPlayerRootActivationRetirementRecipient& recipient,
        uint64_t sequence, const uint8_t* qsrBytes, size_t qsrLength,
        std::vector<uint8_t>& out) const;
    bool acknowledgeClientPlayerRootActivationRetirement(
        const ClientPlayerRootActivationRetirementRecipient& recipient);
    bool encodeClientPlayerRootControlToRoot(
        model::NodeId clientRootNodeId, uint64_t sequence,
        const uint8_t* qsrBytes, size_t qsrLength, std::vector<uint8_t>& out,
        model::RouteDescriptor& outQsrRoute) const;
    bool assertedClientPlayerRootNode(uint64_t nowMs,
                                      model::NodeId& out) const;
    bool clientPlayerRootGrantForNode(model::NodeId clientRootNodeId,
                                      wire::ClientPlayerRootGrant& out) const;
    bool queueRelayChildRetirementsForServiceStop();

private:
    struct EnrollmentRecord {
        model::NodeId nodeId = model::kInvalidNodeId;
        uint64_t viewerId = 0;
        uint32_t viewerGeneration = 0;
        RouteEndpoint qspEndpoint{};
        EndpointProvenance qspProvenance = EndpointProvenance::Unknown;
        RouteEndpoint qsrEndpoint{};
        bool qsrBound = false;
        RouteEndpoint relayDataEndpoint{};
        bool relayDataBound = false;
        uint64_t relayDataRequestNonce = 0u;
        uint64_t relayDataBindingGeneration = 0u;
        bool relayReady = false;
        model::VerifiedCacheRange relayReadyCacheRange{};
        model::VerifiedChainCursor relayReadyCursor{};
        uint64_t routeGeneration = 0;
        uint64_t bindingGeneration = 0;
        uint32_t confirmedCursor = 0;
        model::VerifiedChainCursor qtaCursor{};
        uint64_t expiryMs = 0;
        uint64_t lastInboundSequence = 0;
        uint64_t nextAuthorityStateSequence = 1u;
        uint64_t lastLivenessSentMs = 0u;
        std::array<uint8_t, kTopologyCredentialBytes> credential{};
        bool qspStreaming = false;
        bool attached = false;
        AttachmentOwner attachmentOwner = AttachmentOwner::None;
        model::LeaseBinding attachedLease{};
        uint8_t attachedDepth = 0u;
        std::array<model::NodeId, model::kMaxLeaseAncestry> attachedAncestry{};
        uint8_t attachedAncestryCount = 0u;
    };
    struct OfferReservation {
        model::NodeId childId = model::kInvalidNodeId;
        uint64_t nonce = 0;
        uint64_t expiryMs = 0;
        model::VerifiedChainCursor cursor{};
        bool redeemed = false;
    };
    struct RelayAssignmentReservation {
        model::NodeId relayNodeId = model::kInvalidNodeId;
        model::NodeId childNodeId = model::kInvalidNodeId;
        uint64_t bindingRequestNonce = 0u;
        model::ParentLease childLease{};
    };
    struct RelayActivationReservation {
        wire::RelayChildActivation activation{};
        bool relayReceipt = false;
        bool childReceipt = false;
        wire::RelayChildActivationCommit commit{};
        bool commitCreated = false;
        bool relayCommitSentLocally = false;
        bool childCommitSentLocally = false;
        bool adjacencyAttached = false;
    };
    struct RelayActivationStage {
        model::NodeId relayNodeId = model::kInvalidNodeId;
        model::NodeId childNodeId = model::kInvalidNodeId;
        model::LeaseBinding relayLease{};
        RouteEndpoint relayQsrEndpoint{};
        RouteEndpoint relayDataEndpoint{};
        uint64_t relayBindingGeneration = 0u;
        uint64_t relayDataRequestNonce = 0u;
        model::VerifiedCacheRange relayReadyCacheRange{};
        model::VerifiedChainCursor relayReadyCursor{};
        uint64_t childViewerId = 0u;
        uint32_t childViewerGeneration = 0u;
        RouteEndpoint childQspEndpoint{};
        EndpointProvenance childQspProvenance =
            EndpointProvenance::Unknown;
        uint64_t childRouteGeneration = 0u;
        uint64_t childBindingGeneration = 0u;
        model::VerifiedChainCursor childQtaCursor{};
        uint64_t expiryMs = 0u;
    };
    struct PendingRelayChildRetirement {
        RelayChildRetirementRecipient recipient{};
        EnrollmentRecord qtaRecord{};
    };
    struct PendingRelayChildActivationRetirement {
        RelayChildActivationRetirementRecipient recipient{};
        EnrollmentRecord qtaRecord{};
    };
    struct PendingRelayChildActivationCommit {
        RelayChildActivationCommitRecipient recipient{};
        EnrollmentRecord qtaRecord{};
    };
    struct ClientDataRootRecord {
        ClientDataRootDescriptor descriptor{};
    };
    struct ClientPlayerRootRecord {
        wire::ClientPlayerRootBinding binding{};
        uint64_t grantNonce = 0u;
        wire::ClientPlayerRootGrant grant{};
        std::array<uint8_t, kTopologyCredentialBytes> credential{};
        uint64_t lastInboundSequence = 0u;
        bool asserted = false;
    };
    struct ClientPlayerRootActivationReservation {
        wire::ClientPlayerRootActivation activation{};
        bool receiptAccepted = false;
        wire::ClientPlayerRootActivationCommit commit{};
        bool commitCreated = false;
    };
    struct PendingClientPlayerRootActivationCommit {
        ClientPlayerRootActivationCommitRecipient recipient{};
        EnrollmentRecord qtaRecord{};
    };
    struct PendingClientPlayerRootActivationRetirement {
        ClientPlayerRootActivationRetirementRecipient recipient{};
        EnrollmentRecord qtaRecord{};
    };

    bool drawNonzero(uint64_t& out);
    bool drawCredential(std::array<uint8_t, kTopologyCredentialBytes>& out);
    bool nodeIdInUse(model::NodeId nodeId) const;
    EnrollmentRecord* findMutable(model::NodeId nodeId);
    const EnrollmentRecord* find(model::NodeId nodeId) const;
    const EnrollmentRecord* findByViewer(uint64_t viewerId) const;
    const EnrollmentRecord* findByQspEndpoint(
        const RouteEndpoint& endpoint) const;
    const EnrollmentRecord* findByQsrEndpoint(
        const RouteEndpoint& endpoint) const;
    const EnrollmentRecord* findByRelayDataEndpoint(
        const RouteEndpoint& endpoint) const;
    ClientPlayerRootRecord* findClientPlayerRootMutable(model::NodeId nodeId);
    const ClientPlayerRootRecord* findClientPlayerRoot(
        model::NodeId nodeId) const;
    bool endpointConflictsWithTopologyBinding(
        const RouteEndpoint& endpoint,
        model::NodeId exemptNodeId = model::kInvalidNodeId) const;
    bool endpointConflictsWithClientPlayerRootBinding(
        const RouteEndpoint& endpoint,
        model::NodeId exemptNodeId = model::kInvalidNodeId) const;
    bool populateEnrollment(const EnrollmentRecord& record,
                            TopologyEnrollment& out) const;
    bool validBindingProof(const TopologyViewerProof& proof) const;
    bool validProof(const TopologyViewerProof& proof) const;
    bool writeEnvelopeMacInput(const EnrollmentRecord& record,
                               uint64_t sequence,
                               const uint8_t* qsrBytes, size_t qsrLength,
                               std::vector<uint8_t>& out) const;
    bool encodeControlForRecord(const EnrollmentRecord& record,
                                uint64_t sequence,
                                const uint8_t* qsrBytes, size_t qsrLength,
                                std::vector<uint8_t>& out) const;
    bool makeChildAdvertisement(const EnrollmentRecord& record,
                                const RouteEndpoint& qsrEndpoint,
                                model::NodeAdvertisement& out) const;
    bool makeParentLeaseChildBinding(
        const EnrollmentRecord& record,
        model::ParentLeaseChildBinding& out) const;
    bool acceptRelayDataBinding(EnrollmentRecord& record,
                                const wire::RelayDataBindingRequest& request,
                                const RouteEndpoint& observedDataEndpoint,
                                uint64_t nowMs);
    bool makeRelayDataBindingAck(
        const EnrollmentRecord& record,
        wire::RelayDataBindingAck& out) const;
    bool relayReadyMatches(const EnrollmentRecord& record,
                           const wire::RelayReady& ready,
                           uint64_t nowMs) const;
    bool acceptRelayReady(EnrollmentRecord& record,
                          const wire::RelayReady& ready,
                          uint64_t nowMs);
    bool clientDataRootAssertionMatches(
        const EnrollmentRecord& record,
        const wire::ClientDataRootAssertion& assertion,
        uint64_t nowMs) const;
    void releaseClientDataRootForNode(model::NodeId nodeId);
    void releaseAllClientDataRoots();
    bool validClientPlayerRootClaim(const ClientPlayerRootClaim& claim,
                                    uint64_t nowMs) const;
    bool makeClientPlayerRootBinding(const ClientPlayerRootClaim& claim,
                                     model::NodeId nodeId,
                                     wire::ClientPlayerRootBinding& out) const;
    bool populateClientPlayerRootEnrollment(
        const ClientPlayerRootRecord& record,
        ClientPlayerRootEnrollment& out) const;
    bool writeClientPlayerRootEnvelopeMacInput(
        const ClientPlayerRootRecord& record, uint64_t sequence,
        const uint8_t* qsrBytes, size_t qsrLength,
        std::vector<uint8_t>& out) const;
    bool encodeClientPlayerRootControlForRecord(
        const ClientPlayerRootRecord& record, uint64_t sequence,
        const uint8_t* qsrBytes, size_t qsrLength,
        std::vector<uint8_t>& out) const;
    bool clientPlayerRootAssertionMatches(
        const ClientPlayerRootRecord& record,
        const wire::ClientPlayerRootAssertion& assertion,
        const RouteEndpoint& from, uint64_t nowMs) const;
    bool acceptClientPlayerRootAssertion(
        ClientPlayerRootRecord& record,
        const wire::ClientPlayerRootAssertion& assertion,
        const RouteEndpoint& from, uint64_t nowMs);
    bool clientPlayerRootActivationMatchesCurrentBindings(
        const ClientPlayerRootActivationReservation& reservation,
        const ClientPlayerRootRecord& root,
        const EnrollmentRecord& target, uint64_t nowMs) const;
    bool clientPlayerRootActivationReceiptMatches(
        const ClientPlayerRootActivationReservation& reservation,
        const EnrollmentRecord& target,
        const wire::ClientPlayerRootActivationReceipt& receipt,
        uint64_t nowMs) const;
    bool acceptClientPlayerRootActivationReceipt(
        EnrollmentRecord& target,
        const wire::ClientPlayerRootActivationReceipt& receipt,
        uint64_t nowMs);
    bool ensureClientPlayerRootActivationCommit(
        ClientPlayerRootActivationReservation& activation);
    bool queueClientPlayerRootActivationCommit(
        const ClientPlayerRootActivationReservation& activation);
    void discardClientPlayerRootActivationCommits(
        const wire::ClientPlayerRootActivation& activation);
    bool queueClientPlayerRootActivationRetirement(
        const ClientPlayerRootActivationReservation& activation,
        wire::ClientPlayerRootRetirementReason reason);
    bool releaseClientPlayerRootActivationsForNode(
        model::NodeId nodeId,
        wire::ClientPlayerRootRetirementReason reason);
    bool releaseAllClientPlayerRootActivations(
        wire::ClientPlayerRootRetirementReason reason);
    bool releaseClientPlayerRootRecord(model::NodeId nodeId,
                                       wire::ClientPlayerRootRetirementReason
                                           reason);
    void releaseAllClientPlayerRoots();
    bool relayAssignmentNonceInUse(uint64_t nonce) const;
    bool relayActivationNonceInUse(uint64_t nonce) const;
    bool activationMatchesCurrentBindings(
        const RelayActivationReservation& reservation,
        const EnrollmentRecord& relay, const EnrollmentRecord& child,
        uint64_t nowMs) const;
    bool activationReceiptMatches(
        const RelayActivationReservation& reservation,
        const EnrollmentRecord& sender,
        const wire::RelayChildActivationReceipt& receipt,
        uint64_t nowMs) const;
    bool relayActivationStageMatchesCurrentBindings(
        const RelayActivationStage& stage, const EnrollmentRecord& relay,
        const EnrollmentRecord& child, uint64_t nowMs,
        bool requireChildQsrBound) const;
    void discardRelayActivationStagesForNode(model::NodeId nodeId);
    void discardAllRelayActivationStages();
    bool acceptRelayChildActivationReceipt(
        EnrollmentRecord& sender,
        const wire::RelayChildActivationReceipt& receipt,
        uint64_t nowMs);
    bool ensureRelayChildActivationCommit(
        RelayActivationReservation& activation);
    bool queueRelayChildActivationCommit(
        const RelayActivationReservation& activation);
    void discardRelayChildActivationCommits(
        const wire::RelayChildActivation& activation);
    bool establishRelayChildActivationAdjacency(
        RelayActivationReservation& activation, uint64_t nowMs);
    void rollbackRelayChildActivationAdjacency(
        RelayActivationReservation& activation);
    bool queueRelayChildActivationRetirement(
        const RelayActivationReservation& activation,
        wire::RelayChildRetirementReason reason);
    bool releaseRelayChildActivationsForNode(
        model::NodeId nodeId, wire::RelayChildRetirementReason reason);
    bool releaseAllRelayChildActivations(
        wire::RelayChildRetirementReason reason);
    bool queueRelayChildRetirement(
        const RelayAssignmentReservation& assignment,
        wire::RelayChildRetirementReason reason);
    bool releaseRelayAssignmentsForNode(
        model::NodeId nodeId,
        wire::RelayChildRetirementReason reason);
    bool releaseAllRelayAssignments(
        wire::RelayChildRetirementReason reason);
    bool verifyControlDatagram(const EnrollmentRecord& record,
                               const wire::ControlDatagram& datagram,
                               const RouteEndpoint& from,
                               uint64_t nowMs) const;
    void expire(uint64_t nowMs,
                std::vector<model::NodeId>* detachedNodes = nullptr);

    TopologyAuthorityFacts _facts{};
    qnetplay::spectator::SpectatorGrantKey _leaseKey{};
    std::vector<EnrollmentRecord> _enrollments;
    std::vector<model::NodeId> _retiredNodeIds;
    std::vector<OfferReservation> _offers;
    std::vector<RelayAssignmentReservation> _relayAssignments;
    std::vector<RelayActivationReservation> _relayActivations;
    std::vector<RelayActivationStage> _relayActivationStages;
    std::vector<ClientDataRootRecord> _clientDataRoots;
    std::vector<ClientPlayerRootRecord> _clientPlayerRoots;
    std::vector<ClientPlayerRootActivationReservation>
        _clientPlayerRootActivations;
    std::vector<PendingRelayChildRetirement> _pendingRelayChildRetirements;
    std::vector<PendingRelayChildActivationRetirement>
        _pendingRelayChildActivationRetirements;
    std::vector<PendingRelayChildActivationCommit>
        _pendingRelayChildActivationCommits;
    std::vector<PendingClientPlayerRootActivationCommit>
        _pendingClientPlayerRootActivationCommits;
    std::vector<PendingClientPlayerRootActivationRetirement>
        _pendingClientPlayerRootActivationRetirements;
    model::VerifiedChainCursor _verifiedCursor{};
    TopologyAuthoritySigner _assignmentSigner;
};

struct TopologyRelayLaneConfig {
    bool enabled = false;
    RouteEndpoint relay{};
    qnet::relay::RoomId room{};
    qnet::relay::RegistrationToken authorityToken{};
    uint64_t sessionId = 0;
};

class TopologyQsrControl final
    : public resilient::ResilientControlTransport {
public:
    TopologyQsrControl();
    ~TopologyQsrControl() override;

    TopologyQsrControl(const TopologyQsrControl&) = delete;
    TopologyQsrControl& operator=(const TopologyQsrControl&) = delete;

    bool open(const TopologyActivationRequest& request,
              const EndpointBinding& rootQsp,
              EndpointBinding& qsrBinding,
              std::string& error);
    bool configureRelayLane(const TopologyRelayLaneConfig& config,
                            std::string& warning);
    bool relayLaneReady() const;
    bool issueRelayQsrOffer(model::NodeId nodeId,
                            uint64_t sessionId,
                            uint64_t nowUnix,
                            qnetplay::spectator::RelayQsrOffer& out);
    void attach(TopologyAuthority& authority,
                resilient::ResilientSpectatorRootRuntime& root);
    void detach();
    void close();
    bool open() const;
    const EndpointBinding& binding() const;
    bool service(uint64_t nowMs);
    bool flushRelayChildRetirements();
    bool flushRelayChildActivationRetirements();
    bool flushRelayChildActivationCommits(uint64_t nowMs);
    void forgetNode(model::NodeId nodeId);
    bool sendAuthorityState(wire::AuthorityStateKind kind,
                            wire::AuthorityStateReason reason,
                            uint64_t nowMs,
                            model::NodeId onlyChild = model::kInvalidNodeId);
    bool issueRelayChildAssignment(model::NodeId relayNodeId,
                                   model::NodeId childNodeId,
                                   uint64_t nowMs);
    bool issueRelayChildActivation(model::NodeId relayNodeId,
                                   model::NodeId childNodeId,
                                   uint64_t nowMs);
    bool issueClientPlayerRootActivation(model::NodeId clientRootNodeId,
                                         model::NodeId targetViewerNodeId,
                                         uint64_t nowMs);
    bool flushClientPlayerRootActivationCommits(uint64_t nowMs);
    bool flushClientPlayerRootActivationRetirements();
    bool sendClientRootControl(model::NodeId clientRootNodeId,
                               const uint8_t* bytes, size_t length);
    bool sendControl(const model::RouteDescriptor& to,
                     const uint8_t* bytes, size_t length) override;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
