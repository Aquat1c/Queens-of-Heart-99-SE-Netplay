#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qclient::spectator::topology::client {

// The outer QTA envelope uses a fixed 52-byte framing prefix.
constexpr size_t kQtaEnvelopePrefixBytes = 52u;
constexpr size_t kQtaCredentialBytes = 32u;
constexpr size_t kQtaMacBytes = 16u;
constexpr size_t kTopologyClientMaxSecretBytes = 128u;

constexpr size_t kTopologyClientAbsoluteMaxPayloadBytes = 65535u;

enum class TopologyControlV2Direction : uint8_t {
    Request = 1u,
    Response = 2u,
};

bool deriveTopologyControlV2ProofKey(
    uint64_t grantMacLow, uint64_t grantMacHigh,
    std::array<uint8_t, kQtaMacBytes>& out);

bool makeTopologyControlV2CanonicalInput(
    TopologyControlV2Direction direction,
    const uint8_t* packedBodyWithoutTag, size_t packedBodyWithoutTagBytes,
    std::vector<uint8_t>& out);

bool makeTopologyControlV2Tag(
    uint64_t grantMacLow, uint64_t grantMacHigh,
    const uint8_t* canonicalInput, size_t canonicalInputBytes,
    std::array<uint8_t, kQtaMacBytes>& out);
bool verifyTopologyControlV2Tag(
    uint64_t grantMacLow, uint64_t grantMacHigh,
    const uint8_t* canonicalInput, size_t canonicalInputBytes,
    const std::array<uint8_t, kQtaMacBytes>& expected);

struct QtaWireFormat {
    uint32_t magic = 0u;
    uint16_t version = 0u;
    uint8_t type = 0u;
    size_t maxPayloadBytes = 0u;

    bool valid() const;
};

struct RetryPolicy {
    uint64_t initialRetryMs = 250u;
    uint64_t maxRetryMs = 2000u;
    uint64_t enrollmentTimeoutMs = 10000u;
    uint64_t qsrTimeoutMs = 10000u;
    uint32_t maxEnrollmentAttempts = 4u;
    uint32_t maxQsrAttempts = 5u;

    bool valid() const;
};

struct ClientBootstrap {
    bool enabled = false;
    uint64_t viewerId = 0u;
    uint32_t viewerGeneration = 0u;
    uint64_t sessionId = 0u;
    uint64_t correlation = 0u;
    const uint8_t* enrollmentProofKey = nullptr;
    size_t enrollmentProofKeyBytes = 0u;
    RetryPolicy retry{};
    QtaWireFormat qta{};
};

struct EnrollmentRequest {
    uint64_t correlation = 0u;
    uint64_t viewerId = 0u;
    uint32_t viewerGeneration = 0u;
    uint64_t sessionId = 0u;
};

struct EnrollmentResponse {
    bool granted = false;
    uint64_t correlation = 0u;
    uint64_t viewerId = 0u;
    uint32_t viewerGeneration = 0u;
    uint64_t sessionId = 0u;
    uint64_t authorityGeneration = 0u;
    uint64_t nodeId = 0u;
    uint64_t expiresAtMs = 0u;
    uint64_t qspRouteGeneration = 0u;
    uint64_t bindingGeneration = 0u;
    std::array<uint8_t, kQtaCredentialBytes> qtaCredential{};
    std::array<uint8_t, kQtaMacBytes> authenticationTag{};
};

struct EnrollmentBinding {
    uint64_t correlation = 0u;
    uint64_t viewerId = 0u;
    uint32_t viewerGeneration = 0u;
    uint64_t sessionId = 0u;
    uint64_t authorityGeneration = 0u;
    uint64_t nodeId = 0u;
    uint64_t expiresAtMs = 0u;
    uint64_t qspRouteGeneration = 0u;
    uint64_t bindingGeneration = 0u;
};

class QtaMacInputBuilder {
public:
    virtual ~QtaMacInputBuilder() = default;

    virtual bool build(uint64_t authorityGeneration,
                       uint64_t nodeId,
                       uint64_t sequence,
                       const uint8_t* qsrPayload,
                       size_t qsrPayloadBytes,
                       std::vector<uint8_t>& out) const = 0;
};

struct QtaEnvelope {
    uint64_t authorityGeneration = 0u;
    uint64_t nodeId = 0u;
    uint64_t sequence = 0u;
    std::array<uint8_t, kQtaMacBytes> authenticationTag{};
    std::vector<uint8_t> qsrPayload{};
};

enum class ClientState : uint8_t {
    Disabled = 0,
    WaitingForInitAck,
    EnrollmentPending,
    AwaitingEnrollment,
    AwaitingBootstrapPayload,
    AwaitingRootOffer,
    AwaitingActivationIngress,
    AwaitingRedemptionPayload,
    AwaitingReceipt,
    Attached,
    Failed,
};

enum class ClientFailure : uint8_t {
    None = 0,
    InvalidBootstrap,
    InitAckOutOfOrder,
    EnrollmentAuthenticationRejected,
    EnrollmentRejected,
    EnrollmentValidationRejected,
    EnrollmentTimedOut,
    QsrPayloadRejected,
    QsrTimedOut,
    RootOfferRejected,
    ReceiptRejected,
    ResourceExhausted,
    StaleAction,
};

enum class ClientActionKind : uint8_t {
    None = 0,
    SendEnrollmentRequest,
    SendBootstrapQsr,
    SendRedemptionQsr,
    SendActivationReceiptQsr,
    SendAttachedRelayActivationReceiptQsr,
};

struct ClientAction {
    ClientActionKind kind = ClientActionKind::None;
    uint64_t actionId = 0u;
    uint64_t correlation = 0u;
    uint64_t sequence = 0u;
    EnrollmentRequest enrollment{};
    std::vector<uint8_t> qsrPayload{};
};

enum class QtaIngressResult : uint8_t {
    Accepted = 0,
    NotArmed,
    StateRejected,
    Malformed,
    IdentityRejected,
    ReplayRejected,
    AuthenticationRejected,
    TranscriptRejected,
    ResourceExhausted,
};

const char* clientStateName(ClientState state);
const char* clientFailureName(ClientFailure failure);
const char* qtaIngressResultName(QtaIngressResult result);

class ResilientTopologyClient final {
public:
    ResilientTopologyClient() = default;
    ~ResilientTopologyClient();

    ResilientTopologyClient(const ResilientTopologyClient&) = delete;
    ResilientTopologyClient& operator=(const ResilientTopologyClient&) = delete;

    bool start(const ClientBootstrap& bootstrap, uint64_t nowMs);

    bool onQspInitAckTransmitted(uint64_t nowMs);

    bool restartEnrollment(uint64_t correlation, uint64_t nowMs);

    bool poll(uint64_t nowMs, ClientAction& out);

    bool completeAction(uint64_t actionId, bool transmitted, uint64_t nowMs);

    bool makeEnrollmentRequestTag(uint64_t actionId,
                                  const uint8_t* canonicalInput,
                                  size_t canonicalInputBytes,
                                  std::array<uint8_t, kQtaMacBytes>& out) const;

    bool acceptEnrollmentResponse(
        const EnrollmentResponse& response,
        const uint8_t* canonicalInput,
        size_t canonicalInputBytes,
        uint64_t nowMs);

    bool setBootstrapQsrPayload(const uint8_t* payload, size_t payloadBytes,
                                uint64_t nowMs);

    bool acceptRootOffer(uint64_t offerCorrelation, uint64_t nowMs);

    bool setRedemptionQsrPayload(const uint8_t* payload, size_t payloadBytes,
                                 uint64_t receiptCorrelation,
                                 uint64_t nowMs);

    bool setActivationReceiptQsrPayload(const uint8_t* payload,
                                        size_t payloadBytes,
                                        uint64_t activationCorrelation,
                                        uint64_t nowMs);

    bool cancelActivationReceiptQsrPayload(uint64_t activationCorrelation);

    bool setAttachedRelayActivationReceiptQsrPayload(
        const uint8_t* payload, size_t payloadBytes,
        uint64_t activationCorrelation, uint64_t nowMs);

    bool cancelAttachedRelayActivationReceiptQsrPayload(
        uint64_t activationCorrelation);

    bool acceptReceipt(uint64_t receiptCorrelation, uint64_t nowMs);

    bool acceptPairedActivationCommit(uint64_t activationCorrelation,
                                      uint64_t nowMs);
    bool acceptPairedActivationCompletion(uint64_t activationCorrelation,
                                          uint64_t nowMs);

    bool makeQtaEnvelope(const ClientAction& action,
                         const QtaMacInputBuilder& transcript,
                         std::vector<uint8_t>& out) const;

    bool makeAttachedQtaEnvelope(const uint8_t* qsrPayload,
                                 size_t qsrPayloadBytes,
                                 const QtaMacInputBuilder& transcript,
                                 std::vector<uint8_t>& out);

    QtaIngressResult acceptQtaEnvelope(const uint8_t* bytes, size_t length,
                                       const QtaMacInputBuilder& transcript,
                                       QtaEnvelope& out);

    bool enrollmentBinding(EnrollmentBinding& out) const;
    ClientState state() const { return _state; }
    ClientFailure failure() const { return _failure; }
    bool enabled() const { return _state != ClientState::Disabled; }
    bool attached() const { return _state == ClientState::Attached; }

    void stop();

private:
    struct SecretBuffer {
        std::array<uint8_t, kTopologyClientMaxSecretBytes> bytes{};
        size_t size = 0u;

        ~SecretBuffer();
        bool assign(const uint8_t* source, size_t sourceBytes);
        void clear();
        bool present() const;
        const uint8_t* data() const { return bytes.data(); }
    };

    enum class PendingQsrKind : uint8_t {
        None = 0,
        Bootstrap,
        Redemption,
        ActivationReceipt,
        AttachedRelayActivationReceipt,
    };

    bool validateBootstrap(const ClientBootstrap& bootstrap) const;
    bool makeTag(const SecretBuffer& key, const uint8_t* canonicalInput,
                 size_t canonicalInputBytes,
                 std::array<uint8_t, kQtaMacBytes>& out) const;
    bool checkTag(const SecretBuffer& key, const uint8_t* canonicalInput,
                  size_t canonicalInputBytes,
                  const std::array<uint8_t, kQtaMacBytes>& expected) const;
    void fail(ClientFailure failure);
    void clearSecrets();
    void resetNonSecretState();
    uint64_t retryDelay(uint32_t completedAttempts) const;
    uint64_t addSaturating(uint64_t left, uint64_t right) const;
    bool stageExpired(uint64_t nowMs) const;
    bool qsrStateAllowsIngress() const;
    bool qsrActionDue(uint64_t nowMs) const;
    bool enrollmentActionDue(uint64_t nowMs) const;
    bool actionMatches(uint64_t actionId) const;
    void issueAction(ClientActionKind kind, uint64_t sequence,
                     ClientAction& out);
    bool copyPayload(const uint8_t* payload, size_t payloadBytes,
                     std::vector<uint8_t>& out);

    ClientState _state = ClientState::Disabled;
    ClientFailure _failure = ClientFailure::None;
    RetryPolicy _retry{};
    QtaWireFormat _qta{};
    EnrollmentRequest _request{};
    EnrollmentBinding _binding{};
    SecretBuffer _enrollmentProof{};
    SecretBuffer _qtaCredential{};
    PendingQsrKind _pendingQsr = PendingQsrKind::None;
    std::vector<uint8_t> _pendingQsrPayload{};
    uint64_t _expectedReceiptCorrelation = 0u;
    uint64_t _expectedActivationReceiptCorrelation = 0u;
    uint64_t _pendingActivationIngressCorrelation = 0u;
    uint64_t _stageDeadlineMs = 0u;
    uint64_t _nextActionAtMs = 0u;
    uint64_t _nextActionId = 1u;
    uint64_t _inFlightActionId = 0u;
    ClientActionKind _inFlightKind = ClientActionKind::None;
    uint64_t _inFlightSequence = 0u;
    uint64_t _nextOutboundSequence = 1u;
    uint64_t _lastInboundSequence = 0u;
    uint32_t _completedEnrollmentAttempts = 0u;
    uint32_t _completedQsrAttempts = 0u;
    bool _bootstrapTransmitted = false;
    bool _redemptionTransmitted = false;
};

}
