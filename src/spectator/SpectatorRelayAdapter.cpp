#include "spectator/SpectatorRelayAdapter.h"

#include <cstring>
#include <limits>

namespace qnetplay::spectator {
namespace {

bool sameId(const qnet::relay::Id128& left,
            const qnet::relay::Id128& right) {
    uint64_t difference = left.low ^ right.low;
    difference |= left.high ^ right.high;
    return difference == 0;
}

bool viewerPayloadMatches(const Datagram& datagram,
                          uint64_t expectedViewerId) {
    if (expectedViewerId == 0
        || datagram.header.senderRole != PeerRole::Viewer) {
        return false;
    }

    switch (datagram.header.type) {
    case MessageType::Hello: {
        HelloPayload payload{};
        return decodePayload(datagram.payload, payload)
            && payload.viewerId == expectedViewerId;
    }
    case MessageType::InitAck: {
        InitAckPayload payload{};
        return decodePayload(datagram.payload, payload)
            && payload.viewerId == expectedViewerId;
    }
    case MessageType::Ack: {
        AckPayload payload{};
        return decodePayload(datagram.payload, payload)
            && payload.viewerId == expectedViewerId;
    }
    case MessageType::TerminalAck: {
        TerminalAckPayload payload{};
        return decodePayload(datagram.payload, payload)
            && payload.viewerId == expectedViewerId;
    }
    case MessageType::Detach: {
        DetachPayload payload{};
        return decodePayload(datagram.payload, payload)
            && payload.viewerId == expectedViewerId;
    }
    case MessageType::Migrate: {
        MigratePayload payload{};
        return decodePayload(datagram.payload, payload)
            && payload.viewerId == expectedViewerId;
    }

    case MessageType::GameStart:
    case MessageType::ConfirmedInputBatch:
    case MessageType::Terminal:
    case MessageType::Reject:
    case MessageType::Heartbeat:
    case MessageType::CheckpointOffer:
    case MessageType::CheckpointChunk:
    case MessageType::SetEvent:
        return false;
    }
    return false;
}

bool validInner(const SessionIdentity& identity,
                const uint8_t* bytes,
                size_t length,
                PeerRole expectedSender,
                uint64_t expectedViewerId = 0) {
    if (bytes == nullptr || length < kSpectatorHeaderSize
        || length > kMaxSpectatorDatagramSize) {
        return false;
    }
    Datagram datagram{};
    if (decodeDatagram(bytes, length, identity, datagram)
            != DecodeError::None
        || datagram.header.senderRole != expectedSender) {
        return false;
    }
    return expectedSender != PeerRole::Viewer
        || viewerPayloadMatches(datagram, expectedViewerId);
}

}

bool validRelayViewerBinding(const RelayViewerBinding& binding) {
    return binding.enabled
        && qnet::relay::isValidId(binding.room)
        && qnet::relay::isValidId(binding.token)
        && binding.viewerId != 0
        && binding.sourceGeneration != 0
        && isValidIdentity(binding.identity);
}

bool RelayViewerAdapter::configure(const RelayViewerBinding& binding) {
    reset();
    if (!validRelayViewerBinding(binding)) return false;
    _binding = binding;
    return true;
}

void RelayViewerAdapter::reset() {
    _binding = {};
    _ready = false;
    _detached = false;
    _registrationAttempts = 0;
    _lastRegistrationMs = 0;
}

bool RelayViewerAdapter::encodeOutbound(const uint8_t* bytes,
                                        size_t length,
                                        std::vector<uint8_t>& out) const {
    out.clear();
    if (!_binding.enabled) {
        if (bytes == nullptr && length != 0) return false;
        if (length != 0) out.assign(bytes, bytes + length);
        return true;
    }
    if (!validRelayViewerBinding(_binding)
        || !validInner(_binding.identity, bytes, length, PeerRole::Viewer,
                       _binding.viewerId)) {
        return false;
    }

    qnet::relay::SpectatorFrameHeader header{};
    header.party = qnet::relay::SpectatorParty::Viewer;
    header.viewerId = _binding.viewerId;
    header.sourceGeneration = _binding.sourceGeneration;
    if (!qnet::relay::encodeSpectatorFrameHeader(header, out)) return false;
    out.insert(out.end(), bytes, bytes + length);
    return true;
}

RelayViewerIngress RelayViewerAdapter::decodeInbound(
    bool fromRelay,
    const uint8_t* bytes,
    size_t length,
    std::vector<uint8_t>& out) {
    out.clear();
    if (!_binding.enabled || !fromRelay) {
        if (bytes == nullptr && length != 0)
            return RelayViewerIngress::Rejected;
        if (length != 0) out.assign(bytes, bytes + length);
        return RelayViewerIngress::PassThrough;
    }
    if (!validRelayViewerBinding(_binding) || bytes == nullptr)
        return RelayViewerIngress::Rejected;

    if (qnet::relay::hasSpectatorFrameKind(bytes, length)) {
        qnet::relay::SpectatorFrameHeader header{};
        if (qnet::relay::decodeSpectatorFrameHeader(bytes, length, header)
                != qnet::relay::DecodeError::None
            || header.party != qnet::relay::SpectatorParty::Source
            || header.viewerId != _binding.viewerId
            || header.sourceGeneration != _binding.sourceGeneration
            || length <= qnet::relay::kSpectatorFrameHeaderSize) {
            return RelayViewerIngress::Rejected;
        }
        const uint8_t* inner =
            bytes + qnet::relay::kSpectatorFrameHeaderSize;
        const size_t innerLength =
            length - qnet::relay::kSpectatorFrameHeaderSize;
        if (!validInner(
                _binding.identity, inner, innerLength, PeerRole::Source))
            return RelayViewerIngress::Rejected;
        out.assign(inner, inner + innerLength);
        _ready = true;
        return RelayViewerIngress::Data;
    }

    qnet::relay::SpectatorMessageKind kind{};
    if (qnet::relay::inspectSpectatorMessageKind(bytes, length, kind)
            != qnet::relay::DecodeError::None) {
        return RelayViewerIngress::Rejected;
    }
    if (kind == qnet::relay::SpectatorMessageKind::Ready) {
        qnet::relay::SpectatorReady ready{};
        if (qnet::relay::decodeSpectatorReady(bytes, length, ready)
                != qnet::relay::DecodeError::None
            || ready.party != qnet::relay::SpectatorParty::Viewer
            || !sameId(ready.room, _binding.room)
            || !sameId(ready.token, _binding.token)
            || ready.viewerId != _binding.viewerId
            || ready.sourceGeneration != _binding.sourceGeneration) {
            return RelayViewerIngress::Rejected;
        }
        _ready = true;
        return RelayViewerIngress::Ready;
    }
    if (kind == qnet::relay::SpectatorMessageKind::Detach) {
        qnet::relay::SpectatorDetach detach{};
        if (qnet::relay::decodeSpectatorDetach(bytes, length, detach)
                != qnet::relay::DecodeError::None
            || detach.party != qnet::relay::SpectatorParty::Viewer
            || !sameId(detach.room, _binding.room)
            || !sameId(detach.token, _binding.token)
            || detach.viewerId != _binding.viewerId
            || detach.sourceGeneration != _binding.sourceGeneration) {
            return RelayViewerIngress::Rejected;
        }
        _detached = true;
        return RelayViewerIngress::Detached;
    }

    return RelayViewerIngress::Rejected;
}

bool RelayViewerAdapter::registrationDue(uint64_t nowMs) const {
    if (!_binding.enabled || _ready || _detached) {
        return false;
    }
    if (_registrationAttempts == 0) return true;
    if (nowMs < _lastRegistrationMs) return true;
    const uint64_t retryMs =
        _registrationAttempts < kRelayViewerFastRegistrationAttempts
            ? kRelayViewerRegistrationRetryMs
            : kRelayViewerSlowRegistrationRetryMs;
    return nowMs - _lastRegistrationMs >= retryMs;
}

bool RelayViewerAdapter::encodeRegistration(uint64_t nowMs,
                                            std::vector<uint8_t>& out,
                                            bool force) {
    out.clear();
    if (!validRelayViewerBinding(_binding)
        || _ready || _detached
        || (!force && !registrationDue(nowMs))) {
        return false;
    }
    qnet::relay::SpectatorRegister registration{};
    registration.room = _binding.room;
    registration.party = qnet::relay::SpectatorParty::Viewer;
    registration.sourceGeneration = _binding.sourceGeneration;
    registration.token = _binding.token;
    registration.viewerId = _binding.viewerId;
    if (!qnet::relay::encodeSpectatorRegister(registration, out))
        return false;
    if (_registrationAttempts != std::numeric_limits<uint32_t>::max())
        ++_registrationAttempts;
    _lastRegistrationMs = nowMs;
    return true;
}

bool RelayViewerAdapter::encodeDetach(
    qnet::relay::SpectatorDetachReason reason,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!validRelayViewerBinding(_binding)) return false;
    qnet::relay::SpectatorDetach detach{};
    detach.room = _binding.room;
    detach.party = qnet::relay::SpectatorParty::Viewer;
    detach.reason = reason;
    detach.sourceGeneration = _binding.sourceGeneration;
    detach.token = _binding.token;
    detach.viewerId = _binding.viewerId;
    return qnet::relay::encodeSpectatorDetach(detach, out);
}

bool validRelaySourceBinding(const RelaySourceBinding& binding) {
    return binding.enabled
        && qnet::relay::isValidId(binding.room)
        && qnet::relay::isValidId(binding.token)
        && binding.sourceGeneration != 0
        && isValidIdentity(binding.identity);
}

bool RelaySourceAdapter::configure(const RelaySourceBinding& binding) {
    reset();
    if (!validRelaySourceBinding(binding)) return false;
    _binding = binding;
    return true;
}

void RelaySourceAdapter::reset() {
    _binding = {};
    _viewers.clear();
    _nextVirtualPort = 1;
    _lastRegistrationMs = 0;
}

RelayVirtualEndpoint RelaySourceAdapter::ensureViewer(uint64_t viewerId) {
    if (viewerId == 0) return {};
    for (const ViewerRoute& viewer : _viewers)
        if (viewer.viewerId == viewerId) return viewer.endpoint;

    // 239.255.255.254 is an in-process routing key only; UdpSidecarTransport intercepts it before any sendto call.
    for (uint32_t attempt = 0; attempt < 65535u; ++attempt) {
        const uint16_t candidate =
            _nextVirtualPort == 0 ? 1 : _nextVirtualPort;
        _nextVirtualPort =
            candidate == 65535u ? 1
                                : static_cast<uint16_t>(candidate + 1u);
        bool used = false;
        for (const ViewerRoute& viewer : _viewers)
            used |= viewer.endpoint.port == candidate;
        if (used) continue;
        const RelayVirtualEndpoint endpoint{
            kRelayVirtualIpv4BE, candidate
        };
        _viewers.push_back(ViewerRoute{ viewerId, endpoint });
        return endpoint;
    }
    return {};
}

void RelaySourceAdapter::removeViewer(uint64_t viewerId) {
    for (auto it = _viewers.begin(); it != _viewers.end(); ++it) {
        if (it->viewerId != viewerId) continue;
        _viewers.erase(it);
        return;
    }
}

bool RelaySourceAdapter::viewerForEndpoint(
    const RelayVirtualEndpoint& endpoint,
    uint64_t& viewerId) const {
    for (const ViewerRoute& viewer : _viewers) {
        if (!(viewer.endpoint == endpoint)) continue;
        viewerId = viewer.viewerId;
        return true;
    }
    return false;
}

RelaySourceIngress RelaySourceAdapter::decodeInbound(
    bool fromRelay,
    const uint8_t* bytes,
    size_t length,
    std::vector<uint8_t>& out,
    RelayVirtualEndpoint& from,
    uint64_t* detachedViewerId) {
    out.clear();
    from = {};
    if (detachedViewerId != nullptr) *detachedViewerId = 0;
    if (!_binding.enabled || !fromRelay) {
        if (bytes == nullptr && length != 0)
            return RelaySourceIngress::Rejected;
        if (length != 0) out.assign(bytes, bytes + length);
        return RelaySourceIngress::PassThrough;
    }
    if (!validRelaySourceBinding(_binding) || bytes == nullptr)
        return RelaySourceIngress::Rejected;

    if (qnet::relay::hasSpectatorFrameKind(bytes, length)) {
        qnet::relay::SpectatorFrameHeader header{};
        if (qnet::relay::decodeSpectatorFrameHeader(bytes, length, header)
                != qnet::relay::DecodeError::None
            || header.party != qnet::relay::SpectatorParty::Viewer
            || header.viewerId == 0
            || header.sourceGeneration != _binding.sourceGeneration
            || length <= qnet::relay::kSpectatorFrameHeaderSize) {
            return RelaySourceIngress::Rejected;
        }
        const uint8_t* inner =
            bytes + qnet::relay::kSpectatorFrameHeaderSize;
        const size_t innerLength =
            length - qnet::relay::kSpectatorFrameHeaderSize;
        if (!validInner(
                _binding.identity, inner, innerLength, PeerRole::Viewer,
                header.viewerId))
            return RelaySourceIngress::Rejected;
        from = ensureViewer(header.viewerId);
        if (!from.valid()) return RelaySourceIngress::Rejected;
        out.assign(inner, inner + innerLength);
        return RelaySourceIngress::Data;
    }

    qnet::relay::SpectatorMessageKind kind{};
    if (qnet::relay::inspectSpectatorMessageKind(bytes, length, kind)
            != qnet::relay::DecodeError::None) {
        return RelaySourceIngress::Rejected;
    }
    if (kind == qnet::relay::SpectatorMessageKind::Ready) {
        qnet::relay::SpectatorReady ready{};
        if (qnet::relay::decodeSpectatorReady(bytes, length, ready)
                != qnet::relay::DecodeError::None
            || ready.party != qnet::relay::SpectatorParty::Source
            || !sameId(ready.room, _binding.room)
            || !sameId(ready.token, _binding.token)
            || ready.viewerId == 0
            || ready.sourceGeneration != _binding.sourceGeneration) {
            return RelaySourceIngress::Rejected;
        }
        from = ensureViewer(ready.viewerId);
        return from.valid() ? RelaySourceIngress::Ready
                            : RelaySourceIngress::Rejected;
    }
    if (kind == qnet::relay::SpectatorMessageKind::Detach) {
        qnet::relay::SpectatorDetach detach{};
        if (qnet::relay::decodeSpectatorDetach(bytes, length, detach)
                != qnet::relay::DecodeError::None
            || detach.party != qnet::relay::SpectatorParty::Source
            || !sameId(detach.room, _binding.room)
            || !sameId(detach.token, _binding.token)
            || detach.sourceGeneration != _binding.sourceGeneration) {
            return RelaySourceIngress::Rejected;
        }
        if (detach.viewerId != 0) {
            if (detachedViewerId != nullptr)
                *detachedViewerId = detach.viewerId;
            for (const ViewerRoute& viewer : _viewers) {
                if (viewer.viewerId != detach.viewerId) continue;
                from = viewer.endpoint;
                break;
            }
            removeViewer(detach.viewerId);
        } else {
            _viewers.clear();
        }
        return RelaySourceIngress::Detached;
    }
    return RelaySourceIngress::Rejected;
}

bool RelaySourceAdapter::encodeOutbound(
    const RelayVirtualEndpoint& to,
    const uint8_t* bytes,
    size_t length,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!validRelaySourceBinding(_binding)
        || !validInner(_binding.identity, bytes, length, PeerRole::Source)) {
        return false;
    }
    uint64_t viewerId = 0;
    if (!viewerForEndpoint(to, viewerId)) return false;
    qnet::relay::SpectatorFrameHeader header{};
    header.party = qnet::relay::SpectatorParty::Source;
    header.viewerId = viewerId;
    header.sourceGeneration = _binding.sourceGeneration;
    if (!qnet::relay::encodeSpectatorFrameHeader(header, out)) return false;
    out.insert(out.end(), bytes, bytes + length);
    return true;
}

bool RelaySourceAdapter::encodeRegistration(uint64_t nowMs,
                                            std::vector<uint8_t>& out,
                                            bool force) {
    out.clear();
    if (!validRelaySourceBinding(_binding)) return false;
    if (!force && _lastRegistrationMs != 0
        && nowMs >= _lastRegistrationMs
        && nowMs - _lastRegistrationMs
               < kRelaySourceRegistrationRefreshMs) {
        return false;
    }
    qnet::relay::SpectatorRegister registration{};
    registration.room = _binding.room;
    registration.party = qnet::relay::SpectatorParty::Source;
    registration.sourceGeneration = _binding.sourceGeneration;
    registration.token = _binding.token;
    registration.viewerId = 0;
    if (!qnet::relay::encodeSpectatorRegister(registration, out))
        return false;
    _lastRegistrationMs = nowMs;
    return true;
}

bool RelaySourceAdapter::encodeDetach(
    uint64_t viewerId,
    qnet::relay::SpectatorDetachReason reason,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!validRelaySourceBinding(_binding)) return false;
    qnet::relay::SpectatorDetach detach{};
    detach.room = _binding.room;
    detach.party = qnet::relay::SpectatorParty::Source;
    detach.reason = reason;
    detach.sourceGeneration = _binding.sourceGeneration;
    detach.token = _binding.token;
    detach.viewerId = viewerId;
    return qnet::relay::encodeSpectatorDetach(detach, out);
}

}
