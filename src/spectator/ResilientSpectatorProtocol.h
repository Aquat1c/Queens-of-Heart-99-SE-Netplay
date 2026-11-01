/* Every scalar is written explicitly little-endian; object memory and compiler packing never cross this wire. */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "netplay/ConfigurationProvenance.h"
#include "ResilientSpectatorDistribution.h"

namespace qnetplay::spectator::resilient::wire {

// Little-endian bytes spell "QSR1".
constexpr uint32_t kResilientControlMagic = 0x31525351u;
constexpr uint16_t kResilientControlVersion = 12u;
constexpr size_t kResilientControlHeaderBytes = 16u;
constexpr size_t kMaxResilientControlDatagramBytes =
    kMaxSpectatorDatagramSize;
constexpr uint32_t kTopologyQtaEnvelopeMagic = 0x31415451u; // "QTA1" LE
constexpr uint16_t kTopologyQtaEnvelopeVersion = 1u;
constexpr uint8_t kTopologyQtaEnvelopeControlType = 1u;
constexpr size_t kTopologyQtaEnvelopeMacBytes = 16u;
// magic + version + control type + reserved + payload length + reserved
// + authority generation + node id + sequence + MAC tag.
constexpr size_t kTopologyQtaEnvelopePrefixBytes =
    4u + 2u + 1u + 1u + 2u + 2u + 8u + 8u + 8u
    + kTopologyQtaEnvelopeMacBytes;
constexpr size_t kMaxDrainNoticeNodeIds = 32u;
constexpr uint32_t kTopologyAuthorityStateMaxLivenessWindowMs = 10000u;
constexpr size_t kRelayChildAssignmentHostSignatureBytes = 64u;
constexpr size_t kRelayChildActivationHostSignatureBytes = 64u;
constexpr size_t kRelayChildActivationCommitHostSignatureBytes = 64u;
constexpr size_t kClientPlayerRootGrantHostSignatureBytes = 64u;
constexpr size_t kClientPlayerRootActivationHostSignatureBytes = 64u;
constexpr size_t kClientPlayerRootActivationCommitHostSignatureBytes = 64u;
constexpr size_t kClientPlayerRootActivationRetirementHostSignatureBytes =
    64u;

enum class ControlMessageType : uint8_t {
    NodeAdvertisement = 1,
    ParentLease = 2,
    ContinuityRequest = 3,
    ContinuityProof = 4,
    ParentCommit = 5,
    DrainNotice = 6,
    AuthorityState = 7,
    RelayDataBindingRequest = 8,
    RelayDataBindingAck = 9,
    RelayReady = 10,
    RelayChildAssignment = 11,
    RelayChildRetirement = 12,
    RelayChildActivation = 13,
    RelayChildActivationReceipt = 14,
    RelayChildActivationRetirement = 15,
    RelayChildActivationCommit = 16,
    ClientDataRootAssertion = 17,
    ClientPlayerRootGrant = 18,
    ClientPlayerRootAssertion = 19,
    ClientPlayerRootActivation = 20,
    ClientPlayerRootActivationReceipt = 21,
    ClientPlayerRootActivationCommit = 22,
    ClientPlayerRootActivationRetirement = 23,
};

enum class DecodeResult : uint8_t {
    Ok = 0,
    NullInput,
    TooSmall,
    TooLarge,
    WrongMagic,
    UnsupportedVersion,
    UnknownMessageType,
    HeaderReservedNonZero,
    LengthMismatch,
    ChecksumMismatch,
    MalformedPayload,
};

struct ParentCommit {
    StreamCompatibilityIdentity stream{};
    NodeId childId = kInvalidNodeId;
    NodeId parentId = kInvalidNodeId;
    LeaseBinding lease{};
    uint64_t sourceGeneration = 0;
    uint64_t routeGeneration = 0;
    uint32_t exactNextCursor = 0;
    uint32_t exactNextEvent = 0;
    uint32_t overlapCursor = 0;
    uint32_t overlapEvent = 0;
    uint64_t overlapDigest = 0;
};

struct BoundDrainNotice {
    StreamCompatibilityIdentity stream{};
    NodeId nodeId = kInvalidNodeId;
    DrainNotice notice{};
};

enum class AuthorityStateKind : uint8_t {
    Liveness = 1,
    Revocation = 2,
};

enum class AuthorityStateReason : uint8_t {
    None = 0,
    HostStopping = 1,
    AuthorityExpired = 2,
    RouteReplaced = 3,
    SessionReplaced = 4,
    ServiceStopping = 5,
    ViewerDetached = 6,
};

enum class RelayChildRetirementReason : uint8_t {
    AuthorityRevoked = 1,
    ChildDetached = 2,
    ChildBindingReplaced = 3,
    LeaseExpired = 4,
    ServiceStopping = 5,
};

enum class RelayChildActivationReceiptRole : uint8_t {
    Relay = 1,
    Child = 2,
};

enum class ClientPlayerRootRole : uint8_t {
    RemotePlayerOne = 1,
    RemotePlayerTwo = 2,
};

struct ClientPlayerRootRuntimeReceipt {
    uint32_t processId = 0u;
    uint32_t publisherGeneration = 0u;
    uint64_t publisherNonce = 0u;
    uint8_t playerSlot = 0u;
    qnetplay::config_provenance::Stage runtimeStage =
        qnetplay::config_provenance::Stage::Unknown;
    uint8_t delay = 0u;
    uint8_t rollback = 0u;
    uint8_t remoteDelay = 0u;
    uint8_t remoteRollback = 0u;
    qnetplay::config_provenance::ConfigurationProvenanceWire provenance{};
};

struct ClientPlayerRootBinding {
    StreamCompatibilityIdentity stream{};
    uint64_t gameStartFingerprint = 0u;
    NodeId clientRootNodeId = kInvalidNodeId;
    ClientPlayerRootRole remotePlayerRole =
        ClientPlayerRootRole::RemotePlayerOne;
    qnetplay::config_provenance::ConfigurationProvenanceWire
        configurationProvenance{};
    ClientPlayerRootRuntimeReceipt runtimeReceipt{};
    uint64_t sourceGeneration = 0u;
    uint64_t qspRouteGeneration = 0u;
    uint64_t qsrBindingGeneration = 0u;
    RouteDescriptor dataRoute{};
    RouteDescriptor qsrRoute{};
    VerifiedCacheRange cacheRange{};
    VerifiedChainCursor cursor{};
    uint32_t childCapacity = 0u;
    uint64_t expiresMs = 0u;
};

struct ClientPlayerRootGrant {
    uint64_t authorityGeneration = 0u;
    NodeId hostRootNodeId = kInvalidNodeId;
    uint64_t grantNonce = 0u;
    ClientPlayerRootBinding binding{};
    std::array<uint8_t, kClientPlayerRootGrantHostSignatureBytes>
        hostSignature{};
};

struct ClientPlayerRootAssertion {
    uint64_t authorityGeneration = 0u;
    NodeId hostRootNodeId = kInvalidNodeId;
    uint64_t grantNonce = 0u;
    ClientPlayerRootBinding binding{};
};

struct ClientPlayerRootActivation {
    uint64_t authorityGeneration = 0u;
    NodeId hostRootNodeId = kInvalidNodeId;
    ClientPlayerRootBinding binding{};
    NodeId targetViewerNodeId = kInvalidNodeId;
    ParentLease targetLease{};
    RouteDescriptor targetDataRoute{};
    RouteDescriptor targetQsrRoute{};
    uint64_t activationNonce = 0u;
    uint64_t activationDeadlineMs = 0u;
    std::array<uint8_t, kClientPlayerRootActivationHostSignatureBytes>
        hostSignature{};
};

struct ClientPlayerRootActivationReceipt {
    ClientPlayerRootActivation activation{};
};

struct ClientPlayerRootActivationCommit {
    ClientPlayerRootActivation activation{};
    std::array<uint8_t, kClientPlayerRootActivationCommitHostSignatureBytes>
        hostSignature{};
};

enum class ClientPlayerRootRetirementReason : uint8_t {
    AuthorityRevoked = 1,
    ClientRootExpired = 2,
    ClientRootBindingReplaced = 3,
    TargetViewerDetached = 4,
    LeaseExpired = 5,
    ServiceStopping = 6,
};

struct ClientPlayerRootActivationRetirement {
    ClientPlayerRootActivation activation{};
    ClientPlayerRootRetirementReason reason =
        ClientPlayerRootRetirementReason::AuthorityRevoked;
    std::array<uint8_t,
               kClientPlayerRootActivationRetirementHostSignatureBytes>
        hostSignature{};
};

struct AuthorityState {
    StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    NodeId rootNodeId = kInvalidNodeId;
    NodeId childNodeId = kInvalidNodeId;
    uint64_t viewerId = 0u;
    uint32_t viewerGeneration = 0u;
    uint64_t qspRouteGeneration = 0u;
    uint32_t qsrIpv4BE = 0u;
    uint16_t qsrPort = 0u;
    uint64_t qsrBindingGeneration = 0u;
    AuthorityStateKind kind = AuthorityStateKind::Liveness;
    AuthorityStateReason reason = AuthorityStateReason::None;
    uint64_t sequence = 0u;
    uint64_t authorityExpiryMs = 0u;
    uint64_t eventTimeMs = 0u;
    uint32_t livenessWindowMs = 0u;
};

struct RelayDataBindingRequest {
    StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    NodeId rootNodeId = kInvalidNodeId;
    NodeId childNodeId = kInvalidNodeId;
    LeaseBinding lease{};
    uint64_t requestNonce = 0u;
};

struct RelayDataBindingAck {
    StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    NodeId rootNodeId = kInvalidNodeId;
    NodeId childNodeId = kInvalidNodeId;
    LeaseBinding lease{};
    uint64_t requestNonce = 0u;
    RouteDescriptor observedDataRoute{};
    RouteDescriptor observedControlRoute{};
};

struct RelayReady {
    StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    NodeId rootNodeId = kInvalidNodeId;
    NodeId relayNodeId = kInvalidNodeId;
    LeaseBinding relayLease{};
    uint64_t bindingRequestNonce = 0u;
    RouteDescriptor observedDataRoute{};
    RouteDescriptor observedControlRoute{};
    VerifiedCacheRange cacheRange{};
    VerifiedChainCursor cacheCursor{};
};

struct RelayChildAssignment {
    StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    NodeId rootNodeId = kInvalidNodeId;
    NodeId relayNodeId = kInvalidNodeId;
    NodeId childNodeId = kInvalidNodeId;
    LeaseBinding relayLease{};
    uint64_t bindingRequestNonce = 0u;
    RouteDescriptor observedDataRoute{};
    RouteDescriptor observedControlRoute{};
    ParentLease childLease{};
    // Raw ECDSA P-256 r||s signature over encodeRelayChildAssignmentAttestationInput(), made with the authority's enrollment-bound key.
    std::array<uint8_t, kRelayChildAssignmentHostSignatureBytes>
        hostSignature{};
};

struct RelayChildRetirement {
    StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    NodeId rootNodeId = kInvalidNodeId;
    NodeId relayNodeId = kInvalidNodeId;
    NodeId childNodeId = kInvalidNodeId;
    LeaseBinding relayLease{};
    uint64_t bindingRequestNonce = 0u;
    LeaseBinding childLease{};
    RelayChildRetirementReason reason =
        RelayChildRetirementReason::AuthorityRevoked;
};

struct RelayChildActivation {
    StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    NodeId rootNodeId = kInvalidNodeId;
    NodeId relayNodeId = kInvalidNodeId;
    NodeId childNodeId = kInvalidNodeId;
    LeaseBinding relayLease{};
    uint64_t relayBindingRequestNonce = 0u;
    RouteDescriptor relayDataRoute{};
    RouteDescriptor relayControlRoute{};
    ParentLease childLease{};
    RouteDescriptor childDataRoute{};
    RouteDescriptor childControlRoute{};
    VerifiedCacheRange relayCacheRange{};
    VerifiedChainCursor commonCursor{};
    uint64_t activationNonce = 0u;
    uint64_t activationDeadlineMs = 0u;
    // Raw ECDSA P-256 r||s signature over encodeRelayChildActivationAttestationInput().
    std::array<uint8_t, kRelayChildActivationHostSignatureBytes>
        hostSignature{};
};

struct RelayChildActivationCommit {
    RelayChildActivation activation{};
    std::array<uint8_t, kRelayChildActivationCommitHostSignatureBytes>
        hostSignature{};
};

struct RelayChildActivationReceipt {
    StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    NodeId rootNodeId = kInvalidNodeId;
    NodeId relayNodeId = kInvalidNodeId;
    NodeId childNodeId = kInvalidNodeId;
    LeaseBinding relayLease{};
    LeaseBinding childLease{};
    uint64_t relayBindingRequestNonce = 0u;
    RouteDescriptor relayDataRoute{};
    RouteDescriptor relayControlRoute{};
    RouteDescriptor childDataRoute{};
    RouteDescriptor childControlRoute{};
    VerifiedCacheRange relayCacheRange{};
    VerifiedChainCursor commonCursor{};
    uint64_t activationNonce = 0u;
    uint64_t activationDeadlineMs = 0u;
    RelayChildActivationReceiptRole role =
        RelayChildActivationReceiptRole::Relay;
};

struct RelayChildActivationRetirement {
    StreamCompatibilityIdentity stream{};
    uint64_t authorityGeneration = 0u;
    NodeId rootNodeId = kInvalidNodeId;
    NodeId relayNodeId = kInvalidNodeId;
    NodeId childNodeId = kInvalidNodeId;
    LeaseBinding relayLease{};
    LeaseBinding childLease{};
    uint64_t relayBindingRequestNonce = 0u;
    RouteDescriptor relayDataRoute{};
    RouteDescriptor relayControlRoute{};
    RouteDescriptor childDataRoute{};
    RouteDescriptor childControlRoute{};
    VerifiedCacheRange relayCacheRange{};
    VerifiedChainCursor commonCursor{};
    uint64_t activationNonce = 0u;
    uint64_t activationDeadlineMs = 0u;
    RelayChildRetirementReason reason =
        RelayChildRetirementReason::AuthorityRevoked;
};

struct ClientDataRootAssertion {
    StreamCompatibilityIdentity stream{};
    uint64_t gameStartFingerprint = 0u;
    uint64_t authorityGeneration = 0u;
    NodeId clientRootNodeId = kInvalidNodeId;
    uint64_t qspRouteGeneration = 0u;
    uint64_t qsrBindingGeneration = 0u;
    uint64_t sourceGeneration = 0u;
    RouteDescriptor dataRoute{};
    RouteDescriptor qsrRoute{};
    VerifiedCacheRange cacheRange{};
    VerifiedChainCursor cursor{};
    uint32_t childCapacity = 0u;
};

struct ControlDatagram {
    ControlMessageType type = ControlMessageType::NodeAdvertisement;
    NodeAdvertisement nodeAdvertisement{};
    ParentLease parentLease{};
    ContinuityRequest continuityRequest{};
    ContinuityProof continuityProof{};
    ParentCommit parentCommit{};
    BoundDrainNotice drainNotice{};
    AuthorityState authorityState{};
    RelayDataBindingRequest relayDataBindingRequest{};
    RelayDataBindingAck relayDataBindingAck{};
    RelayReady relayReady{};
    RelayChildAssignment relayChildAssignment{};
    RelayChildRetirement relayChildRetirement{};
    RelayChildActivation relayChildActivation{};
    RelayChildActivationReceipt relayChildActivationReceipt{};
    RelayChildActivationRetirement relayChildActivationRetirement{};
    RelayChildActivationCommit relayChildActivationCommit{};
    ClientDataRootAssertion clientDataRootAssertion{};
    ClientPlayerRootGrant clientPlayerRootGrant{};
    ClientPlayerRootAssertion clientPlayerRootAssertion{};
    ClientPlayerRootActivation clientPlayerRootActivation{};
    ClientPlayerRootActivationReceipt clientPlayerRootActivationReceipt{};
    ClientPlayerRootActivationCommit clientPlayerRootActivationCommit{};
    ClientPlayerRootActivationRetirement
        clientPlayerRootActivationRetirement{};
};

bool hasResilientControlMagic(const uint8_t* bytes, size_t length);

bool encodeControlDatagram(const ControlDatagram& datagram,
                           std::vector<uint8_t>& out);

bool encodeRelayChildAssignmentAttestationInput(
    const RelayChildAssignment& assignment, std::vector<uint8_t>& out);

bool encodeRelayChildActivationAttestationInput(
    const RelayChildActivation& activation, std::vector<uint8_t>& out);

bool encodeRelayChildActivationCommitAttestationInput(
    const RelayChildActivationCommit& commit, std::vector<uint8_t>& out);

bool validRelayChildActivationCommit(
    const RelayChildActivationCommit& commit);

bool makeRelayChildActivationReceipt(
    const RelayChildActivation& activation,
    RelayChildActivationReceiptRole role,
    RelayChildActivationReceipt& out);

bool encodeClientPlayerRootGrantAttestationInput(
    const ClientPlayerRootGrant& grant, std::vector<uint8_t>& out);
bool encodeClientPlayerRootActivationAttestationInput(
    const ClientPlayerRootActivation& activation, std::vector<uint8_t>& out);
bool encodeClientPlayerRootActivationCommitAttestationInput(
    const ClientPlayerRootActivationCommit& commit,
    std::vector<uint8_t>& out);
bool encodeClientPlayerRootActivationRetirementAttestationInput(
    const ClientPlayerRootActivationRetirement& retirement,
    std::vector<uint8_t>& out);

bool validClientPlayerRootActivationCommit(
    const ClientPlayerRootActivationCommit& commit);
bool validClientPlayerRootActivationRetirement(
    const ClientPlayerRootActivationRetirement& retirement);
bool makeClientPlayerRootActivationReceipt(
    const ClientPlayerRootActivation& activation,
    ClientPlayerRootActivationReceipt& out);

DecodeResult decodeControlDatagram(const uint8_t* bytes, size_t length,
                                   ControlDatagram& out);

const char* decodeResultName(DecodeResult result);

}
