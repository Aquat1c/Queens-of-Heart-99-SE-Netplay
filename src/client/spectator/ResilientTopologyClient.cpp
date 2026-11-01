#include "client/spectator/ResilientTopologyClient.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include "spectator/ResilientSpectatorProtocol.h"
#include "spectator/SpectatorGrant.h"

namespace qclient::spectator::topology::client {
namespace {

constexpr size_t kMagicOffset = 0u;
constexpr size_t kVersionOffset = 4u;
constexpr size_t kTypeOffset = 6u;
constexpr size_t kFlagsOffset = 7u;
constexpr size_t kPayloadLengthOffset = 8u;
constexpr size_t kReservedOffset = 10u;
constexpr size_t kAuthorityGenerationOffset = 12u;
constexpr size_t kNodeIdOffset = 20u;
constexpr size_t kSequenceOffset = 28u;
constexpr size_t kAuthenticationTagOffset = 36u;

void wipeBytes(void* bytes, size_t count) {
    volatile uint8_t* cursor = static_cast<volatile uint8_t*>(bytes);
    while (cursor != nullptr && count != 0u) {
        *cursor++ = 0u;
        --count;
    }
}

void wipeVector(std::vector<uint8_t>& bytes) {
    if (!bytes.empty()) wipeBytes(bytes.data(), bytes.size());
    bytes.clear();
}

bool allZero(const uint8_t* bytes, const size_t count) {
    if (bytes == nullptr || count == 0u) return true;
    uint8_t aggregate = 0u;
    for (size_t index = 0u; index < count; ++index) aggregate |= bytes[index];
    return aggregate == 0u;
}

bool equalConstantTime(const uint8_t* left, const uint8_t* right,
                       const size_t count) {
    if (left == nullptr || right == nullptr) return false;
    uint8_t difference = 0u;
    for (size_t index = 0u; index < count; ++index)
        difference |= static_cast<uint8_t>(left[index] ^ right[index]);
    return difference == 0u;
}

void writeLe16(uint8_t* out, const uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8u);
}

void writeLe32(uint8_t* out, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        out[shift / 8u] = static_cast<uint8_t>(value >> shift);
}

void writeLe64(uint8_t* out, const uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        out[shift / 8u] = static_cast<uint8_t>(value >> shift);
}

uint16_t readLe16(const uint8_t* in) {
    return static_cast<uint16_t>(in[0])
        | static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8u);
}

uint32_t readLe32(const uint8_t* in) {
    uint32_t value = 0u;
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        value |= static_cast<uint32_t>(in[shift / 8u]) << shift;
    return value;
}

uint64_t readLe64(const uint8_t* in) {
    uint64_t value = 0u;
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        value |= static_cast<uint64_t>(in[shift / 8u]) << shift;
    return value;
}

bool validPayload(const uint8_t* bytes, const size_t count,
                  const size_t maximum) {
    return bytes != nullptr && count != 0u && count <= maximum;
}

constexpr char kTopologyControlV2ProofKeyDomain[] =
    "qoh99-topology-control-v2-proof-key";
constexpr char kTopologyControlV2CanonicalDomain[] =
    "qoh99-topology-control-v2";
constexpr uint8_t kTopologyControlV2CanonicalVersion = 1u;

bool validControlDirection(const TopologyControlV2Direction direction) {
    return direction == TopologyControlV2Direction::Request
        || direction == TopologyControlV2Direction::Response;
}

bool makeTagWithKey(const uint8_t* key, const size_t keyBytes,
                    const uint8_t* canonicalInput,
                    const size_t canonicalInputBytes,
                    std::array<uint8_t, kQtaMacBytes>& out) {
    out.fill(0u);
    if (key == nullptr || keyBytes == 0u || canonicalInput == nullptr
        || canonicalInputBytes == 0u) {
        return false;
    }
    std::array<uint8_t, 32u> digest{};
    qnetplay::spectator::hmacSha256(key, keyBytes, canonicalInput,
                                    canonicalInputBytes, digest);
    std::copy_n(digest.data(), out.size(), out.data());
    wipeBytes(digest.data(), digest.size());
    return true;
}

}

bool deriveTopologyControlV2ProofKey(
    const uint64_t grantMacLow, const uint64_t grantMacHigh,
    std::array<uint8_t, kQtaMacBytes>& out) {
    out.fill(0u);
    std::array<uint8_t, kQtaMacBytes> grantMac{};
    std::array<uint8_t, 32u> digest{};
    writeLe64(grantMac.data(), grantMacLow);
    writeLe64(grantMac.data() + sizeof(uint64_t), grantMacHigh);
    qnetplay::spectator::hmacSha256(
        grantMac.data(), grantMac.size(),
        reinterpret_cast<const uint8_t*>(kTopologyControlV2ProofKeyDomain),
        sizeof(kTopologyControlV2ProofKeyDomain) - 1u, digest);
    std::copy_n(digest.data(), out.size(), out.data());
    wipeBytes(digest.data(), digest.size());
    wipeBytes(grantMac.data(), grantMac.size());
    return true;
}

bool makeTopologyControlV2CanonicalInput(
    const TopologyControlV2Direction direction,
    const uint8_t* packedBodyWithoutTag,
    const size_t packedBodyWithoutTagBytes,
    std::vector<uint8_t>& out) {
    wipeVector(out);
    if (!validControlDirection(direction)
        || packedBodyWithoutTag == nullptr
        || packedBodyWithoutTagBytes == 0u
        || packedBodyWithoutTagBytes > kTopologyClientAbsoluteMaxPayloadBytes) {
        return false;
    }
    constexpr size_t prefixBytes =
        sizeof(kTopologyControlV2CanonicalDomain) - 1u + 2u;
    try {
        out.reserve(prefixBytes + packedBodyWithoutTagBytes);
        out.insert(out.end(),
                   reinterpret_cast<const uint8_t*>(
                       kTopologyControlV2CanonicalDomain),
                   reinterpret_cast<const uint8_t*>(
                       kTopologyControlV2CanonicalDomain)
                       + sizeof(kTopologyControlV2CanonicalDomain) - 1u);
        out.push_back(kTopologyControlV2CanonicalVersion);
        out.push_back(static_cast<uint8_t>(direction));
        out.insert(out.end(), packedBodyWithoutTag,
                   packedBodyWithoutTag + packedBodyWithoutTagBytes);
    } catch (const std::bad_alloc&) {
        wipeVector(out);
        return false;
    }
    return true;
}

bool makeTopologyControlV2Tag(
    const uint64_t grantMacLow, const uint64_t grantMacHigh,
    const uint8_t* canonicalInput, const size_t canonicalInputBytes,
    std::array<uint8_t, kQtaMacBytes>& out) {
    std::array<uint8_t, kQtaMacBytes> key{};
    const bool derived = deriveTopologyControlV2ProofKey(grantMacLow,
                                                           grantMacHigh, key);
    const bool tagged = derived && makeTagWithKey(key.data(), key.size(),
                                                   canonicalInput,
                                                   canonicalInputBytes, out);
    wipeBytes(key.data(), key.size());
    if (!tagged) out.fill(0u);
    return tagged;
}

bool verifyTopologyControlV2Tag(
    const uint64_t grantMacLow, const uint64_t grantMacHigh,
    const uint8_t* canonicalInput, const size_t canonicalInputBytes,
    const std::array<uint8_t, kQtaMacBytes>& expected) {
    std::array<uint8_t, kQtaMacBytes> actual{};
    const bool tagged = makeTopologyControlV2Tag(
        grantMacLow, grantMacHigh, canonicalInput, canonicalInputBytes,
        actual);
    const bool matched = tagged && equalConstantTime(actual.data(),
                                                     expected.data(),
                                                     actual.size());
    wipeBytes(actual.data(), actual.size());
    return matched;
}

bool QtaWireFormat::valid() const {
    return magic != 0u && version != 0u && type != 0u
        && maxPayloadBytes != 0u
        && maxPayloadBytes <= kTopologyClientAbsoluteMaxPayloadBytes
        && maxPayloadBytes <= static_cast<size_t>(
            (std::numeric_limits<uint16_t>::max)());
}

bool RetryPolicy::valid() const {
    return initialRetryMs != 0u
        && maxRetryMs >= initialRetryMs
        && enrollmentTimeoutMs != 0u
        && qsrTimeoutMs != 0u
        && maxEnrollmentAttempts != 0u
        && maxQsrAttempts != 0u;
}

const char* clientStateName(const ClientState state) {
    switch (state) {
    case ClientState::Disabled: return "disabled";
    case ClientState::WaitingForInitAck: return "waiting-for-init-ack";
    case ClientState::EnrollmentPending: return "enrollment-pending";
    case ClientState::AwaitingEnrollment: return "awaiting-enrollment";
    case ClientState::AwaitingBootstrapPayload:
        return "awaiting-bootstrap-payload";
    case ClientState::AwaitingRootOffer: return "awaiting-root-offer";
    case ClientState::AwaitingRedemptionPayload:
        return "awaiting-redemption-payload";
    case ClientState::AwaitingReceipt: return "awaiting-receipt";
    case ClientState::Attached: return "attached";
    case ClientState::Failed: return "failed";
    }
    return "unknown";
}

const char* clientFailureName(const ClientFailure failure) {
    switch (failure) {
    case ClientFailure::None: return "none";
    case ClientFailure::InvalidBootstrap: return "invalid-bootstrap";
    case ClientFailure::InitAckOutOfOrder: return "init-ack-out-of-order";
    case ClientFailure::EnrollmentAuthenticationRejected:
        return "enrollment-authentication-rejected";
    case ClientFailure::EnrollmentRejected: return "enrollment-rejected";
    case ClientFailure::EnrollmentValidationRejected:
        return "enrollment-validation-rejected";
    case ClientFailure::EnrollmentTimedOut: return "enrollment-timed-out";
    case ClientFailure::QsrPayloadRejected: return "qsr-payload-rejected";
    case ClientFailure::QsrTimedOut: return "qsr-timed-out";
    case ClientFailure::RootOfferRejected: return "root-offer-rejected";
    case ClientFailure::ReceiptRejected: return "receipt-rejected";
    case ClientFailure::ResourceExhausted: return "resource-exhausted";
    case ClientFailure::StaleAction: return "stale-action";
    }
    return "unknown";
}

const char* qtaIngressResultName(const QtaIngressResult result) {
    switch (result) {
    case QtaIngressResult::Accepted: return "accepted";
    case QtaIngressResult::NotArmed: return "not-armed";
    case QtaIngressResult::StateRejected: return "state-rejected";
    case QtaIngressResult::Malformed: return "malformed";
    case QtaIngressResult::IdentityRejected: return "identity-rejected";
    case QtaIngressResult::ReplayRejected: return "replay-rejected";
    case QtaIngressResult::AuthenticationRejected:
        return "authentication-rejected";
    case QtaIngressResult::TranscriptRejected: return "transcript-rejected";
    case QtaIngressResult::ResourceExhausted: return "resource-exhausted";
    }
    return "unknown";
}

ResilientTopologyClient::SecretBuffer::~SecretBuffer() {
    clear();
}

bool ResilientTopologyClient::SecretBuffer::assign(const uint8_t* source,
                                                   const size_t sourceBytes) {
    clear();
    if (source == nullptr || sourceBytes == 0u
        || sourceBytes > bytes.size() || allZero(source, sourceBytes)) {
        return false;
    }
    std::copy_n(source, sourceBytes, bytes.data());
    size = sourceBytes;
    return true;
}

void ResilientTopologyClient::SecretBuffer::clear() {
    wipeBytes(bytes.data(), bytes.size());
    size = 0u;
}

bool ResilientTopologyClient::SecretBuffer::present() const {
    return size != 0u && !allZero(bytes.data(), size);
}

ResilientTopologyClient::~ResilientTopologyClient() {
    stop();
}

bool ResilientTopologyClient::validateBootstrap(
    const ClientBootstrap& bootstrap) const {
    if (!bootstrap.enabled) return true;
    return bootstrap.viewerId != 0u
        && bootstrap.viewerGeneration != 0u
        && bootstrap.sessionId != 0u
        && bootstrap.correlation != 0u
        && bootstrap.enrollmentProofKey != nullptr
        && bootstrap.enrollmentProofKeyBytes != 0u
        && bootstrap.enrollmentProofKeyBytes <= kTopologyClientMaxSecretBytes
        && !allZero(bootstrap.enrollmentProofKey,
                    bootstrap.enrollmentProofKeyBytes)
        && bootstrap.retry.valid()
        && bootstrap.qta.valid();
}

bool ResilientTopologyClient::start(const ClientBootstrap& bootstrap,
                                    const uint64_t ) {
    stop();
    if (!bootstrap.enabled) return true;
    if (!validateBootstrap(bootstrap)) {
        _state = ClientState::Failed;
        _failure = ClientFailure::InvalidBootstrap;
        return false;
    }
    if (!_enrollmentProof.assign(bootstrap.enrollmentProofKey,
                                 bootstrap.enrollmentProofKeyBytes)) {
        _state = ClientState::Failed;
        _failure = ClientFailure::InvalidBootstrap;
        return false;
    }
    _retry = bootstrap.retry;
    _qta = bootstrap.qta;
    _request.correlation = bootstrap.correlation;
    _request.viewerId = bootstrap.viewerId;
    _request.viewerGeneration = bootstrap.viewerGeneration;
    _request.sessionId = bootstrap.sessionId;
    _state = ClientState::WaitingForInitAck;
    _failure = ClientFailure::None;
    return true;
}

bool ResilientTopologyClient::onQspInitAckTransmitted(const uint64_t nowMs) {
    if (_state == ClientState::Disabled) return false;
    if (_state != ClientState::WaitingForInitAck) {
        fail(ClientFailure::InitAckOutOfOrder);
        return false;
    }
    _state = ClientState::EnrollmentPending;
    _stageDeadlineMs = addSaturating(nowMs, _retry.enrollmentTimeoutMs);
    _nextActionAtMs = nowMs;
    return true;
}

bool ResilientTopologyClient::restartEnrollment(const uint64_t correlation,
                                                const uint64_t nowMs) {
    if (correlation == 0u || correlation == _request.correlation
        || (_state != ClientState::EnrollmentPending
            && _state != ClientState::AwaitingEnrollment)
        || stageExpired(nowMs)) {
        return false;
    }
    _request.correlation = correlation;
    _completedEnrollmentAttempts = 0u;
    _nextActionAtMs = nowMs;
    _inFlightActionId = 0u;
    _inFlightKind = ClientActionKind::None;
    _inFlightSequence = 0u;
    return true;
}

uint64_t ResilientTopologyClient::addSaturating(const uint64_t left,
                                                const uint64_t right) const {
    const uint64_t maximum = (std::numeric_limits<uint64_t>::max)();
    return right > maximum - left ? maximum : left + right;
}

uint64_t ResilientTopologyClient::retryDelay(
    const uint32_t completedAttempts) const {
    uint64_t delay = _retry.initialRetryMs;
    for (uint32_t index = 1u; index < completedAttempts; ++index) {
        if (delay >= _retry.maxRetryMs / 2u) return _retry.maxRetryMs;
        delay *= 2u;
    }
    return (std::min)(delay, _retry.maxRetryMs);
}

bool ResilientTopologyClient::stageExpired(const uint64_t nowMs) const {
    return _stageDeadlineMs != 0u && nowMs >= _stageDeadlineMs;
}

bool ResilientTopologyClient::enrollmentActionDue(const uint64_t nowMs) const {
    return (_state == ClientState::EnrollmentPending
            || _state == ClientState::AwaitingEnrollment)
        && _completedEnrollmentAttempts < _retry.maxEnrollmentAttempts
        && nowMs >= _nextActionAtMs;
}

bool ResilientTopologyClient::qsrActionDue(const uint64_t nowMs) const {
    return (_state == ClientState::AwaitingRootOffer
            || _state == ClientState::AwaitingReceipt
            || (_state == ClientState::Attached
                && _pendingQsr
                    == PendingQsrKind::AttachedRelayActivationReceipt))
        && _pendingQsr != PendingQsrKind::None
        && !_pendingQsrPayload.empty()
        && _completedQsrAttempts < _retry.maxQsrAttempts
        && nowMs >= _nextActionAtMs;
}

bool ResilientTopologyClient::actionMatches(const uint64_t actionId) const {
    return actionId != 0u && actionId == _inFlightActionId
        && _inFlightKind != ClientActionKind::None;
}

void ResilientTopologyClient::issueAction(const ClientActionKind kind,
                                          const uint64_t sequence,
                                          ClientAction& out) {
    if (_inFlightActionId == 0u) {
        _inFlightActionId = _nextActionId++;
        if (_nextActionId == 0u) _nextActionId = 1u;
        _inFlightKind = kind;
        _inFlightSequence = sequence;
    }
    out = {};
    out.kind = _inFlightKind;
    out.actionId = _inFlightActionId;
    out.correlation = (_inFlightKind
            == ClientActionKind::SendActivationReceiptQsr
        || _inFlightKind
            == ClientActionKind::SendAttachedRelayActivationReceiptQsr)
        ? _expectedActivationReceiptCorrelation : _request.correlation;
    out.sequence = _inFlightSequence;
    if (_inFlightKind == ClientActionKind::SendEnrollmentRequest) {
        out.enrollment = _request;
    } else {
        out.qsrPayload = _pendingQsrPayload;
    }
}

bool ResilientTopologyClient::poll(const uint64_t nowMs, ClientAction& out) {
    out = {};
    if (_state == ClientState::Disabled || _state == ClientState::Failed
        || _state == ClientState::WaitingForInitAck) {
        return false;
    }
    if (_state != ClientState::Attached
        && _state != ClientState::AwaitingActivationIngress
        && stageExpired(nowMs)) {
        const bool enrollment = _state == ClientState::EnrollmentPending
            || _state == ClientState::AwaitingEnrollment;
        fail(enrollment ? ClientFailure::EnrollmentTimedOut
                        : ClientFailure::QsrTimedOut);
        return false;
    }
    try {
        if (_inFlightActionId != 0u) {
            issueAction(_inFlightKind, _inFlightSequence, out);
            return true;
        }
        if (enrollmentActionDue(nowMs)) {
            issueAction(ClientActionKind::SendEnrollmentRequest, 0u, out);
            return true;
        }
        if (qsrActionDue(nowMs)) {
            ClientActionKind kind = ClientActionKind::None;
            switch (_pendingQsr) {
            case PendingQsrKind::Bootstrap:
                kind = ClientActionKind::SendBootstrapQsr;
                break;
            case PendingQsrKind::Redemption:
                kind = ClientActionKind::SendRedemptionQsr;
                break;
            case PendingQsrKind::ActivationReceipt:
                kind = ClientActionKind::SendActivationReceiptQsr;
                break;
            case PendingQsrKind::AttachedRelayActivationReceipt:
                kind = ClientActionKind::SendAttachedRelayActivationReceiptQsr;
                break;
            case PendingQsrKind::None:
            default:
                return false;
            }
            issueAction(kind, _nextOutboundSequence, out);
            return true;
        }
    } catch (const std::bad_alloc&) {
        fail(ClientFailure::ResourceExhausted);
    }
    return false;
}

bool ResilientTopologyClient::completeAction(const uint64_t actionId,
                                             const bool transmitted,
                                             const uint64_t nowMs) {
    if (!actionMatches(actionId)) return false;
    const ClientActionKind kind = _inFlightKind;
    _inFlightActionId = 0u;
    _inFlightKind = ClientActionKind::None;
    _inFlightSequence = 0u;

    if (kind == ClientActionKind::SendEnrollmentRequest) {
        ++_completedEnrollmentAttempts;
        if (transmitted) _state = ClientState::AwaitingEnrollment;
        else _state = ClientState::EnrollmentPending;
        if (_completedEnrollmentAttempts >= _retry.maxEnrollmentAttempts) {
            if (!transmitted) fail(ClientFailure::EnrollmentTimedOut);
            else _nextActionAtMs = (std::numeric_limits<uint64_t>::max)();
            return transmitted;
        }
        _nextActionAtMs = addSaturating(
            nowMs, retryDelay(_completedEnrollmentAttempts));
        return true;
    }

    if (kind != ClientActionKind::SendBootstrapQsr
        && kind != ClientActionKind::SendRedemptionQsr
        && kind != ClientActionKind::SendActivationReceiptQsr
        && kind != ClientActionKind::SendAttachedRelayActivationReceiptQsr) {
        fail(ClientFailure::StaleAction);
        return false;
    }
    if ((kind == ClientActionKind::SendActivationReceiptQsr
         || kind == ClientActionKind::SendAttachedRelayActivationReceiptQsr)
        && _expectedActivationReceiptCorrelation == 0u) {
        fail(ClientFailure::StaleAction);
        return false;
    }
    ++_completedQsrAttempts;
    if (transmitted) {
        if (_nextOutboundSequence == (std::numeric_limits<uint64_t>::max)()) {
            fail(ClientFailure::QsrTimedOut);
            return false;
        }
        ++_nextOutboundSequence;
        if (kind == ClientActionKind::SendBootstrapQsr) {
            _bootstrapTransmitted = true;
        } else if (kind == ClientActionKind::SendRedemptionQsr) {
            _redemptionTransmitted = true;
        }
    }
    if (_completedQsrAttempts >= _retry.maxQsrAttempts) {
        if (kind == ClientActionKind::SendAttachedRelayActivationReceiptQsr) {
            wipeVector(_pendingQsrPayload);
            _pendingQsr = PendingQsrKind::None;
            _expectedActivationReceiptCorrelation = 0u;
            _completedQsrAttempts = 0u;
            _nextActionAtMs = 0u;
            return transmitted;
        }
        if (!transmitted) fail(ClientFailure::QsrTimedOut);
        else _nextActionAtMs = (std::numeric_limits<uint64_t>::max)();
        return transmitted;
    }
    _nextActionAtMs = addSaturating(nowMs,
                                    retryDelay(_completedQsrAttempts));
    return true;
}

bool ResilientTopologyClient::makeTag(
    const SecretBuffer& key, const uint8_t* canonicalInput,
    const size_t canonicalInputBytes,
    std::array<uint8_t, kQtaMacBytes>& out) const {
    out.fill(0u);
    if (!key.present() || canonicalInput == nullptr
        || canonicalInputBytes == 0u) {
        return false;
    }
    std::array<uint8_t, 32u> digest{};
    qnetplay::spectator::hmacSha256(key.data(), key.size, canonicalInput,
                                    canonicalInputBytes, digest);
    std::copy_n(digest.data(), out.size(), out.data());
    wipeBytes(digest.data(), digest.size());
    return true;
}

bool ResilientTopologyClient::checkTag(
    const SecretBuffer& key, const uint8_t* canonicalInput,
    const size_t canonicalInputBytes,
    const std::array<uint8_t, kQtaMacBytes>& expected) const {
    std::array<uint8_t, kQtaMacBytes> actual{};
    const bool generated = makeTag(key, canonicalInput, canonicalInputBytes,
                                   actual);
    const bool matched = generated && equalConstantTime(
        actual.data(), expected.data(), actual.size());
    wipeBytes(actual.data(), actual.size());
    return matched;
}

bool ResilientTopologyClient::makeEnrollmentRequestTag(
    const uint64_t actionId, const uint8_t* canonicalInput,
    const size_t canonicalInputBytes,
    std::array<uint8_t, kQtaMacBytes>& out) const {
    out.fill(0u);
    return actionMatches(actionId)
        && _inFlightKind == ClientActionKind::SendEnrollmentRequest
        && (_state == ClientState::EnrollmentPending
            || _state == ClientState::AwaitingEnrollment)
        && makeTag(_enrollmentProof, canonicalInput, canonicalInputBytes, out);
}

bool ResilientTopologyClient::acceptEnrollmentResponse(
    const EnrollmentResponse& response, const uint8_t* canonicalInput,
    const size_t canonicalInputBytes, const uint64_t nowMs) {
    if (_state != ClientState::AwaitingEnrollment) return false;
    if (!checkTag(_enrollmentProof, canonicalInput, canonicalInputBytes,
                  response.authenticationTag)) {
        fail(ClientFailure::EnrollmentAuthenticationRejected);
        return false;
    }
    if (!response.granted) {
        fail(ClientFailure::EnrollmentRejected);
        return false;
    }
    if (response.correlation != _request.correlation
        || response.viewerId != _request.viewerId
        || response.viewerGeneration != _request.viewerGeneration
        || response.sessionId != _request.sessionId
        || response.authorityGeneration == 0u || response.nodeId == 0u
        || response.expiresAtMs == 0u || response.qspRouteGeneration == 0u
        || response.bindingGeneration == 0u
        || allZero(response.qtaCredential.data(),
                   response.qtaCredential.size())) {
        fail(ClientFailure::EnrollmentValidationRejected);
        return false;
    }
    if (!_qtaCredential.assign(response.qtaCredential.data(),
                               response.qtaCredential.size())) {
        fail(ClientFailure::ResourceExhausted);
        return false;
    }
    _binding.correlation = response.correlation;
    _binding.viewerId = response.viewerId;
    _binding.viewerGeneration = response.viewerGeneration;
    _binding.sessionId = response.sessionId;
    _binding.authorityGeneration = response.authorityGeneration;
    _binding.nodeId = response.nodeId;
    _binding.expiresAtMs = response.expiresAtMs;
    _binding.qspRouteGeneration = response.qspRouteGeneration;
    _binding.bindingGeneration = response.bindingGeneration;
    _enrollmentProof.clear();
    _pendingQsr = PendingQsrKind::None;
    wipeVector(_pendingQsrPayload);
    _completedQsrAttempts = 0u;
    _nextOutboundSequence = 1u;
    _lastInboundSequence = 0u;
    _bootstrapTransmitted = false;
    _redemptionTransmitted = false;
    _state = ClientState::AwaitingBootstrapPayload;
    _stageDeadlineMs = addSaturating(nowMs, _retry.qsrTimeoutMs);
    _nextActionAtMs = 0u;
    return true;
}

bool ResilientTopologyClient::copyPayload(const uint8_t* payload,
                                          const size_t payloadBytes,
                                          std::vector<uint8_t>& out) {
    if (!validPayload(payload, payloadBytes, _qta.maxPayloadBytes)) return false;
    try {
        out.assign(payload, payload + payloadBytes);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

bool ResilientTopologyClient::setBootstrapQsrPayload(
    const uint8_t* payload, const size_t payloadBytes, const uint64_t nowMs) {
    if (_state != ClientState::AwaitingBootstrapPayload || !_qtaCredential.present()) {
        return false;
    }
    std::vector<uint8_t> replacement;
    if (!copyPayload(payload, payloadBytes, replacement)) {
        fail(ClientFailure::QsrPayloadRejected);
        return false;
    }
    wipeVector(_pendingQsrPayload);
    _pendingQsrPayload.swap(replacement);
    wipeVector(replacement);
    _pendingQsr = PendingQsrKind::Bootstrap;
    _completedQsrAttempts = 0u;
    _nextActionAtMs = nowMs;
    _state = ClientState::AwaitingRootOffer;
    return true;
}

bool ResilientTopologyClient::acceptRootOffer(const uint64_t offerCorrelation,
                                              const uint64_t ) {
    if (_state != ClientState::AwaitingRootOffer || !_bootstrapTransmitted
        || offerCorrelation == 0u) {
        fail(ClientFailure::RootOfferRejected);
        return false;
    }
    wipeVector(_pendingQsrPayload);
    _pendingQsr = PendingQsrKind::None;
    _expectedReceiptCorrelation = offerCorrelation;
    _expectedActivationReceiptCorrelation = 0u;
    _completedQsrAttempts = 0u;
    _inFlightActionId = 0u;
    _inFlightKind = ClientActionKind::None;
    _inFlightSequence = 0u;
    _state = ClientState::AwaitingRedemptionPayload;
    return true;
}

bool ResilientTopologyClient::setRedemptionQsrPayload(
    const uint8_t* payload, const size_t payloadBytes,
    const uint64_t receiptCorrelation, const uint64_t nowMs) {
    if (_state != ClientState::AwaitingRedemptionPayload
        || receiptCorrelation == 0u
        || receiptCorrelation != _expectedReceiptCorrelation) {
        fail(ClientFailure::ReceiptRejected);
        return false;
    }
    std::vector<uint8_t> replacement;
    if (!copyPayload(payload, payloadBytes, replacement)) {
        fail(ClientFailure::QsrPayloadRejected);
        return false;
    }
    wipeVector(_pendingQsrPayload);
    _pendingQsrPayload.swap(replacement);
    wipeVector(replacement);
    _pendingQsr = PendingQsrKind::Redemption;
    _completedQsrAttempts = 0u;
    _nextActionAtMs = nowMs;
    _state = ClientState::AwaitingReceipt;
    return true;
}

bool ResilientTopologyClient::setActivationReceiptQsrPayload(
    const uint8_t* const payload, const size_t payloadBytes,
    const uint64_t activationCorrelation, const uint64_t nowMs) {
    const bool replacesTransmittedBootstrap = _pendingQsr
            == PendingQsrKind::Bootstrap
        && _bootstrapTransmitted && _inFlightActionId == 0u;
    if (_state != ClientState::AwaitingRootOffer || !_bootstrapTransmitted
        || !_qtaCredential.present() || activationCorrelation == 0u
        || (_pendingQsr != PendingQsrKind::None
            && !replacesTransmittedBootstrap)
        || _inFlightActionId != 0u) {
        return false;
    }
    std::vector<uint8_t> replacement;
    if (!copyPayload(payload, payloadBytes, replacement)) {
        fail(ClientFailure::QsrPayloadRejected);
        return false;
    }
    wipeVector(_pendingQsrPayload);
    _pendingQsrPayload.swap(replacement);
    wipeVector(replacement);
    _pendingQsr = PendingQsrKind::ActivationReceipt;
    _expectedActivationReceiptCorrelation = activationCorrelation;
    _completedQsrAttempts = 0u;
    _nextActionAtMs = nowMs;
    return true;
}

bool ResilientTopologyClient::cancelActivationReceiptQsrPayload(
    const uint64_t activationCorrelation) {
    const bool queuedReceipt = _state == ClientState::AwaitingRootOffer
        && _pendingQsr == PendingQsrKind::ActivationReceipt
        && _expectedActivationReceiptCorrelation != 0u
        && activationCorrelation == _expectedActivationReceiptCorrelation;
    const bool awaitingIngress = _state
            == ClientState::AwaitingActivationIngress
        && _pendingActivationIngressCorrelation != 0u
        && activationCorrelation == _pendingActivationIngressCorrelation;
    if (activationCorrelation == 0u || (!queuedReceipt && !awaitingIngress)) {
        return false;
    }
    wipeVector(_pendingQsrPayload);
    _pendingQsr = PendingQsrKind::None;
    _expectedActivationReceiptCorrelation = 0u;
    _pendingActivationIngressCorrelation = 0u;
    _completedQsrAttempts = 0u;
    _nextActionAtMs = 0u;
    _inFlightActionId = 0u;
    _inFlightKind = ClientActionKind::None;
    _inFlightSequence = 0u;
    if (awaitingIngress) _state = ClientState::AwaitingRootOffer;
    return true;
}

bool ResilientTopologyClient::setAttachedRelayActivationReceiptQsrPayload(
    const uint8_t* const payload, const size_t payloadBytes,
    const uint64_t activationCorrelation, const uint64_t nowMs) {
    if (_state != ClientState::Attached || !_qtaCredential.present()
        || activationCorrelation == 0u
        || _pendingQsr != PendingQsrKind::None
        || _inFlightActionId != 0u) {
        return false;
    }
    std::vector<uint8_t> replacement;
    if (!copyPayload(payload, payloadBytes, replacement)) {
        return false;
    }
    wipeVector(_pendingQsrPayload);
    _pendingQsrPayload.swap(replacement);
    wipeVector(replacement);
    _pendingQsr = PendingQsrKind::AttachedRelayActivationReceipt;
    _expectedActivationReceiptCorrelation = activationCorrelation;
    _completedQsrAttempts = 0u;
    _nextActionAtMs = nowMs;
    return true;
}

bool ResilientTopologyClient::cancelAttachedRelayActivationReceiptQsrPayload(
    const uint64_t activationCorrelation) {
    if (_state != ClientState::Attached
        || _pendingQsr != PendingQsrKind::AttachedRelayActivationReceipt
        || _expectedActivationReceiptCorrelation == 0u
        || activationCorrelation == 0u
        || activationCorrelation != _expectedActivationReceiptCorrelation) {
        return false;
    }
    wipeVector(_pendingQsrPayload);
    _pendingQsr = PendingQsrKind::None;
    _expectedActivationReceiptCorrelation = 0u;
    _completedQsrAttempts = 0u;
    _nextActionAtMs = 0u;
    _inFlightActionId = 0u;
    _inFlightKind = ClientActionKind::None;
    _inFlightSequence = 0u;
    return true;
}

bool ResilientTopologyClient::acceptReceipt(const uint64_t receiptCorrelation,
                                            const uint64_t ) {
    if (_state != ClientState::AwaitingReceipt || !_redemptionTransmitted
        || receiptCorrelation == 0u
        || receiptCorrelation != _expectedReceiptCorrelation) {
        fail(ClientFailure::ReceiptRejected);
        return false;
    }
    wipeVector(_pendingQsrPayload);
    _pendingQsr = PendingQsrKind::None;
    _inFlightActionId = 0u;
    _inFlightKind = ClientActionKind::None;
    _inFlightSequence = 0u;
    _state = ClientState::Attached;
    return true;
}

bool ResilientTopologyClient::acceptPairedActivationCommit(
    const uint64_t activationCorrelation, const uint64_t ) {
    if (_state != ClientState::AwaitingRootOffer
        || _pendingQsr != PendingQsrKind::ActivationReceipt
        || _expectedActivationReceiptCorrelation == 0u
        || activationCorrelation == 0u
        || activationCorrelation != _expectedActivationReceiptCorrelation
        || _completedQsrAttempts == 0u || _inFlightActionId != 0u) {
        return false;
    }
    wipeVector(_pendingQsrPayload);
    _pendingQsr = PendingQsrKind::None;
    _expectedActivationReceiptCorrelation = 0u;
    _pendingActivationIngressCorrelation = activationCorrelation;
    _completedQsrAttempts = 0u;
    _nextActionAtMs = 0u;
    _state = ClientState::AwaitingActivationIngress;
    return true;
}

bool ResilientTopologyClient::acceptPairedActivationCompletion(
    const uint64_t activationCorrelation, const uint64_t ) {
    if (_state != ClientState::AwaitingActivationIngress
        || activationCorrelation == 0u
        || activationCorrelation != _pendingActivationIngressCorrelation
        || _inFlightActionId != 0u) {
        return false;
    }
    _pendingActivationIngressCorrelation = 0u;
    _state = ClientState::Attached;
    return true;
}

bool ResilientTopologyClient::makeQtaEnvelope(
    const ClientAction& action, const QtaMacInputBuilder& transcript,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!actionMatches(action.actionId)
        || (action.kind != ClientActionKind::SendBootstrapQsr
            && action.kind != ClientActionKind::SendRedemptionQsr
            && action.kind != ClientActionKind::SendActivationReceiptQsr
            && action.kind
                != ClientActionKind::SendAttachedRelayActivationReceiptQsr)
        || action.kind != _inFlightKind
        || action.sequence == 0u || action.sequence != _inFlightSequence
        || ((action.kind == ClientActionKind::SendActivationReceiptQsr
             || action.kind
                 == ClientActionKind::SendAttachedRelayActivationReceiptQsr)
            && (action.correlation == 0u
                || action.correlation
                    != _expectedActivationReceiptCorrelation))
        || action.qsrPayload.empty()
        || action.qsrPayload != _pendingQsrPayload
        || !_qta.valid() || !_qtaCredential.present()
        || _binding.authorityGeneration == 0u || _binding.nodeId == 0u) {
        return false;
    }
    std::vector<uint8_t> macInput;
    std::array<uint8_t, kQtaMacBytes> tag{};
    const bool transcriptBuilt = transcript.build(
        _binding.authorityGeneration, _binding.nodeId, action.sequence,
        action.qsrPayload.data(), action.qsrPayload.size(), macInput);
    const bool tagged = transcriptBuilt && makeTag(
        _qtaCredential, macInput.data(), macInput.size(), tag);
    wipeVector(macInput);
    if (!tagged) {
        wipeBytes(tag.data(), tag.size());
        return false;
    }
    try {
        out.assign(kQtaEnvelopePrefixBytes + action.qsrPayload.size(), 0u);
    } catch (const std::bad_alloc&) {
        wipeBytes(tag.data(), tag.size());
        return false;
    }
    writeLe32(out.data() + kMagicOffset, _qta.magic);
    writeLe16(out.data() + kVersionOffset, _qta.version);
    out[kTypeOffset] = _qta.type;
    out[kFlagsOffset] = 0u;
    writeLe16(out.data() + kPayloadLengthOffset,
              static_cast<uint16_t>(action.qsrPayload.size()));
    writeLe16(out.data() + kReservedOffset, 0u);
    writeLe64(out.data() + kAuthorityGenerationOffset,
              _binding.authorityGeneration);
    writeLe64(out.data() + kNodeIdOffset, _binding.nodeId);
    writeLe64(out.data() + kSequenceOffset, action.sequence);
    std::copy_n(tag.data(), tag.size(),
                out.data() + kAuthenticationTagOffset);
    std::copy_n(action.qsrPayload.data(), action.qsrPayload.size(),
                out.data() + kQtaEnvelopePrefixBytes);
    wipeBytes(tag.data(), tag.size());
    return true;
}

bool ResilientTopologyClient::makeAttachedQtaEnvelope(
    const uint8_t* const qsrPayload, const size_t qsrPayloadBytes,
    const QtaMacInputBuilder& transcript, std::vector<uint8_t>& out) {
    out.clear();
    if (_state != ClientState::Attached || !validPayload(
            qsrPayload, qsrPayloadBytes, _qta.maxPayloadBytes)
        || !_qta.valid() || !_qtaCredential.present()
        || _binding.authorityGeneration == 0u || _binding.nodeId == 0u
        || _nextOutboundSequence == 0u) {
        return false;
    }
    const uint64_t sequence = _nextOutboundSequence;
    std::vector<uint8_t> macInput;
    std::array<uint8_t, kQtaMacBytes> tag{};
    const bool transcriptBuilt = transcript.build(
        _binding.authorityGeneration, _binding.nodeId, sequence, qsrPayload,
        qsrPayloadBytes, macInput);
    const bool tagged = transcriptBuilt && makeTag(
        _qtaCredential, macInput.data(), macInput.size(), tag);
    wipeVector(macInput);
    if (!tagged) {
        wipeBytes(tag.data(), tag.size());
        return false;
    }
    try {
        out.assign(kQtaEnvelopePrefixBytes + qsrPayloadBytes, 0u);
    } catch (const std::bad_alloc&) {
        wipeBytes(tag.data(), tag.size());
        return false;
    }
    writeLe32(out.data() + kMagicOffset, _qta.magic);
    writeLe16(out.data() + kVersionOffset, _qta.version);
    out[kTypeOffset] = _qta.type;
    out[kFlagsOffset] = 0u;
    writeLe16(out.data() + kPayloadLengthOffset,
              static_cast<uint16_t>(qsrPayloadBytes));
    writeLe16(out.data() + kReservedOffset, 0u);
    writeLe64(out.data() + kAuthorityGenerationOffset,
              _binding.authorityGeneration);
    writeLe64(out.data() + kNodeIdOffset, _binding.nodeId);
    writeLe64(out.data() + kSequenceOffset, sequence);
    std::copy_n(tag.data(), tag.size(),
                out.data() + kAuthenticationTagOffset);
    std::copy_n(qsrPayload, qsrPayloadBytes,
                out.data() + kQtaEnvelopePrefixBytes);
    wipeBytes(tag.data(), tag.size());
    // Never reuse a QTA sequence after a send error: it may already be queued, and replaying one is unsafe while gaps are harmless.
    if (_nextOutboundSequence == (std::numeric_limits<uint64_t>::max)()) {
        _nextOutboundSequence = 0u;
    } else {
        ++_nextOutboundSequence;
    }
    return true;
}

bool ResilientTopologyClient::qsrStateAllowsIngress() const {
    return (_state == ClientState::AwaitingRootOffer && _bootstrapTransmitted)
        || _state == ClientState::AwaitingActivationIngress
        || (_state == ClientState::AwaitingReceipt && _redemptionTransmitted)
        || _state == ClientState::Attached;
}

QtaIngressResult ResilientTopologyClient::acceptQtaEnvelope(
    const uint8_t* bytes, const size_t length,
    const QtaMacInputBuilder& transcript, QtaEnvelope& out) {
    out = {};
    if (!_qtaCredential.present() || _binding.authorityGeneration == 0u
        || _binding.nodeId == 0u || !_qta.valid()) {
        return QtaIngressResult::NotArmed;
    }
    if (!qsrStateAllowsIngress()) return QtaIngressResult::StateRejected;
    if (bytes == nullptr || length < kQtaEnvelopePrefixBytes
        || length > kQtaEnvelopePrefixBytes + _qta.maxPayloadBytes
        || readLe32(bytes + kMagicOffset) != _qta.magic
        || readLe16(bytes + kVersionOffset) != _qta.version
        || bytes[kTypeOffset] != _qta.type || bytes[kFlagsOffset] != 0u
        || readLe16(bytes + kReservedOffset) != 0u) {
        return QtaIngressResult::Malformed;
    }
    const size_t payloadBytes = readLe16(bytes + kPayloadLengthOffset);
    if (payloadBytes == 0u || payloadBytes > _qta.maxPayloadBytes
        || payloadBytes + kQtaEnvelopePrefixBytes != length) {
        return QtaIngressResult::Malformed;
    }
    const uint64_t authorityGeneration =
        readLe64(bytes + kAuthorityGenerationOffset);
    const uint64_t nodeId = readLe64(bytes + kNodeIdOffset);
    const uint64_t sequence = readLe64(bytes + kSequenceOffset);
    if (authorityGeneration != _binding.authorityGeneration
        || nodeId != _binding.nodeId) {
        return QtaIngressResult::IdentityRejected;
    }
    if (sequence == 0u)
        return QtaIngressResult::ReplayRejected;
    bool pendingReceiptOutOfOrder = false;
    if (_state == ClientState::AwaitingReceipt
        && sequence <= _lastInboundSequence) {
        qnetplay::spectator::resilient::wire::ControlDatagram control{};
        pendingReceiptOutOfOrder =
            qnetplay::spectator::resilient::wire::decodeControlDatagram(
                bytes + kQtaEnvelopePrefixBytes, payloadBytes, control)
                == qnetplay::spectator::resilient::wire::DecodeResult::Ok
            && control.type == qnetplay::spectator::resilient::wire::ControlMessageType::ParentCommit;
    }
    if (!pendingReceiptOutOfOrder && sequence <= _lastInboundSequence)
        return QtaIngressResult::ReplayRejected;
    const uint8_t* payload = bytes + kQtaEnvelopePrefixBytes;
    std::vector<uint8_t> macInput;
    std::array<uint8_t, kQtaMacBytes> expected{};
    const bool transcriptBuilt = transcript.build(authorityGeneration, nodeId,
                                                  sequence, payload,
                                                  payloadBytes, macInput);
    const bool tagged = transcriptBuilt && makeTag(_qtaCredential,
                                                   macInput.data(),
                                                   macInput.size(), expected);
    wipeVector(macInput);
    if (!transcriptBuilt) {
        wipeBytes(expected.data(), expected.size());
        return QtaIngressResult::TranscriptRejected;
    }
    const bool authenticated = tagged && equalConstantTime(
        expected.data(), bytes + kAuthenticationTagOffset, expected.size());
    wipeBytes(expected.data(), expected.size());
    if (!authenticated) return QtaIngressResult::AuthenticationRejected;
    try {
        out.qsrPayload.assign(payload, payload + payloadBytes);
    } catch (const std::bad_alloc&) {
        return QtaIngressResult::ResourceExhausted;
    }
    out.authorityGeneration = authorityGeneration;
    out.nodeId = nodeId;
    out.sequence = sequence;
    std::copy_n(bytes + kAuthenticationTagOffset, out.authenticationTag.size(),
                out.authenticationTag.data());
    if (sequence > _lastInboundSequence) _lastInboundSequence = sequence;
    return QtaIngressResult::Accepted;
}

bool ResilientTopologyClient::enrollmentBinding(EnrollmentBinding& out) const {
    out = {};
    if (!_qtaCredential.present() || _binding.authorityGeneration == 0u
        || _binding.nodeId == 0u) {
        return false;
    }
    out = _binding;
    return true;
}

void ResilientTopologyClient::clearSecrets() {
    _enrollmentProof.clear();
    _qtaCredential.clear();
    wipeVector(_pendingQsrPayload);
}

void ResilientTopologyClient::resetNonSecretState() {
    _retry = {};
    _qta = {};
    _request = {};
    _binding = {};
    _pendingQsr = PendingQsrKind::None;
    _expectedReceiptCorrelation = 0u;
    _expectedActivationReceiptCorrelation = 0u;
    _pendingActivationIngressCorrelation = 0u;
    _stageDeadlineMs = 0u;
    _nextActionAtMs = 0u;
    _nextActionId = 1u;
    _inFlightActionId = 0u;
    _inFlightKind = ClientActionKind::None;
    _inFlightSequence = 0u;
    _nextOutboundSequence = 1u;
    _lastInboundSequence = 0u;
    _completedEnrollmentAttempts = 0u;
    _completedQsrAttempts = 0u;
    _bootstrapTransmitted = false;
    _redemptionTransmitted = false;
}

void ResilientTopologyClient::fail(const ClientFailure failure) {
    clearSecrets();
    resetNonSecretState();
    _state = ClientState::Failed;
    _failure = failure;
}

void ResilientTopologyClient::stop() {
    clearSecrets();
    resetNonSecretState();
    _state = ClientState::Disabled;
    _failure = ClientFailure::None;
}

}
