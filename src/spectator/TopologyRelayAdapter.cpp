#include "spectator/TopologyRelayAdapter.h"

#include "spectator/ResilientSpectatorProtocol.h"

#include <limits>

namespace qnetplay::spectator {
namespace {

namespace rwire = qnetplay::spectator::resilient::wire;

bool sameId(const qnet::relay::Id128& left,
            const qnet::relay::Id128& right) {
    uint64_t difference = left.low ^ right.low;
    difference |= left.high ^ right.high;
    return difference == 0;
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

bool validEnvelopeShape(const uint8_t* bytes, size_t length) {
    return bytes != nullptr
        && length >= rwire::kTopologyQtaEnvelopePrefixBytes
                + rwire::kResilientControlHeaderBytes
        && length <= rwire::kTopologyQtaEnvelopePrefixBytes
                + rwire::kMaxResilientControlDatagramBytes
        && readLe32(bytes) == rwire::kTopologyQtaEnvelopeMagic;
}

}

bool validTopologyRelayBinding(const TopologyRelayBinding& binding) {
    return binding.enabled
        && qnet::relay::isValidId(binding.room)
        && qnet::relay::isValidId(binding.token)
        && binding.nodeId != 0
        && binding.authorityGeneration != 0;
}

bool TopologyRelayNodeAdapter::configure(const TopologyRelayBinding& binding) {
    reset();
    if (!validTopologyRelayBinding(binding)) return false;
    _binding = binding;
    return true;
}

void TopologyRelayNodeAdapter::reset() {
    _binding = {};
    _ready = false;
    _detached = false;
    _registrationAttempts = 0;
    _lastRegistrationMs = 0;
}

bool TopologyRelayNodeAdapter::encodeOutbound(const uint8_t* bytes,
                                              size_t length,
                                              std::vector<uint8_t>& out) const {
    out.clear();
    if (!_binding.enabled) {
        if (bytes == nullptr && length != 0) return false;
        if (length != 0) out.assign(bytes, bytes + length);
        return true;
    }
    if (!validTopologyRelayBinding(_binding)
        || !validEnvelopeShape(bytes, length)) {
        return false;
    }

    qnet::relay::TopologyFrameHeader header{};
    header.party = qnet::relay::TopologyParty::Node;
    header.nodeId = _binding.nodeId;
    header.authorityGeneration = _binding.authorityGeneration;
    if (!qnet::relay::encodeTopologyFrameHeader(header, out)) return false;
    out.insert(out.end(), bytes, bytes + length);
    return true;
}

TopologyRelayIngress TopologyRelayNodeAdapter::decodeInbound(
    bool fromRelay,
    const uint8_t* bytes,
    size_t length,
    std::vector<uint8_t>& out) {
    out.clear();
    if (!_binding.enabled || !fromRelay) {
        if (bytes == nullptr && length != 0)
            return TopologyRelayIngress::Rejected;
        if (length != 0) out.assign(bytes, bytes + length);
        return TopologyRelayIngress::PassThrough;
    }
    if (!validTopologyRelayBinding(_binding) || bytes == nullptr)
        return TopologyRelayIngress::Rejected;

    if (qnet::relay::hasTopologyFrameKind(bytes, length)) {
        qnet::relay::TopologyFrameHeader header{};
        if (qnet::relay::decodeTopologyFrameHeader(bytes, length, header)
                != qnet::relay::DecodeError::None
            || header.party != qnet::relay::TopologyParty::Authority
            || header.nodeId != _binding.nodeId
            || header.authorityGeneration != _binding.authorityGeneration
            || length <= qnet::relay::kTopologyFrameHeaderSize) {
            return TopologyRelayIngress::Rejected;
        }
        const uint8_t* inner = bytes + qnet::relay::kTopologyFrameHeaderSize;
        const size_t innerLength =
            length - qnet::relay::kTopologyFrameHeaderSize;
        if (!validEnvelopeShape(inner, innerLength))
            return TopologyRelayIngress::Rejected;
        out.assign(inner, inner + innerLength);
        _ready = true;
        return TopologyRelayIngress::Data;
    }

    qnet::relay::TopologyMessageKind kind{};
    if (qnet::relay::inspectTopologyMessageKind(bytes, length, kind)
            != qnet::relay::DecodeError::None) {
        return TopologyRelayIngress::Rejected;
    }
    if (kind == qnet::relay::TopologyMessageKind::Ready) {
        qnet::relay::TopologyReady ready{};
        if (qnet::relay::decodeTopologyReady(bytes, length, ready)
                != qnet::relay::DecodeError::None
            || ready.party != qnet::relay::TopologyParty::Node
            || !sameId(ready.room, _binding.room)
            || !sameId(ready.token, _binding.token)
            || ready.nodeId != _binding.nodeId
            || ready.authorityGeneration != _binding.authorityGeneration) {
            return TopologyRelayIngress::Rejected;
        }
        _ready = true;
        return TopologyRelayIngress::Ready;
    }
    if (kind == qnet::relay::TopologyMessageKind::Detach) {
        qnet::relay::TopologyDetach detach{};
        if (qnet::relay::decodeTopologyDetach(bytes, length, detach)
                != qnet::relay::DecodeError::None
            || detach.party != qnet::relay::TopologyParty::Node
            || !sameId(detach.room, _binding.room)
            || !sameId(detach.token, _binding.token)
            || detach.nodeId != _binding.nodeId
            || detach.authorityGeneration != _binding.authorityGeneration) {
            return TopologyRelayIngress::Rejected;
        }
        _detached = true;
        return TopologyRelayIngress::Detached;
    }

    return TopologyRelayIngress::Rejected;
}

bool TopologyRelayNodeAdapter::registrationDue(uint64_t nowMs) const {
    if (!_binding.enabled || _ready || _detached) return false;
    if (_registrationAttempts == 0) return true;
    if (nowMs < _lastRegistrationMs) return true;
    const uint64_t retryMs =
        _registrationAttempts < kTopologyRelayFastRegistrationAttempts
            ? kTopologyRelayRegistrationRetryMs
            : kTopologyRelaySlowRegistrationRetryMs;
    return nowMs - _lastRegistrationMs >= retryMs;
}

bool TopologyRelayNodeAdapter::encodeRegistration(uint64_t nowMs,
                                                  std::vector<uint8_t>& out,
                                                  bool force) {
    out.clear();
    if (!validTopologyRelayBinding(_binding) || _ready || _detached
        || (!force && !registrationDue(nowMs))) {
        return false;
    }
    qnet::relay::TopologyRegister registration{};
    registration.room = _binding.room;
    registration.party = qnet::relay::TopologyParty::Node;
    registration.authorityGeneration = _binding.authorityGeneration;
    registration.token = _binding.token;
    registration.nodeId = _binding.nodeId;
    if (!qnet::relay::encodeTopologyRegister(registration, out)) return false;
    if (_registrationAttempts != std::numeric_limits<uint32_t>::max())
        ++_registrationAttempts;
    _lastRegistrationMs = nowMs;
    return true;
}

bool TopologyRelayNodeAdapter::encodeDetach(
    qnet::relay::TopologyDetachReason reason,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!validTopologyRelayBinding(_binding)) return false;
    qnet::relay::TopologyDetach detach{};
    detach.room = _binding.room;
    detach.party = qnet::relay::TopologyParty::Node;
    detach.reason = reason;
    detach.authorityGeneration = _binding.authorityGeneration;
    detach.token = _binding.token;
    detach.nodeId = _binding.nodeId;
    return qnet::relay::encodeTopologyDetach(detach, out);
}

bool validTopologyRelayAuthorityBinding(
    const TopologyRelayAuthorityBinding& binding) {
    return binding.enabled
        && qnet::relay::isValidId(binding.room)
        && qnet::relay::isValidId(binding.token)
        && binding.authorityGeneration != 0;
}

bool TopologyRelayAuthorityAdapter::configure(
    const TopologyRelayAuthorityBinding& binding) {
    reset();
    if (!validTopologyRelayAuthorityBinding(binding)) return false;
    _binding = binding;
    return true;
}

void TopologyRelayAuthorityAdapter::reset() {
    _binding = {};
    _registrationAttempts = 0;
    _lastRegistrationMs = 0;
}

bool TopologyRelayAuthorityAdapter::encodeOutbound(
    const uint64_t nodeId,
    const uint8_t* bytes,
    const size_t length,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!validTopologyRelayAuthorityBinding(_binding) || nodeId == 0
        || !validEnvelopeShape(bytes, length)) {
        return false;
    }
    qnet::relay::TopologyFrameHeader header{};
    header.party = qnet::relay::TopologyParty::Authority;
    header.nodeId = nodeId;
    header.authorityGeneration = _binding.authorityGeneration;
    if (!qnet::relay::encodeTopologyFrameHeader(header, out)) return false;
    out.insert(out.end(), bytes, bytes + length);
    return true;
}

TopologyRelayIngress TopologyRelayAuthorityAdapter::decodeInbound(
    const bool fromRelay,
    const uint8_t* bytes,
    const size_t length,
    std::vector<uint8_t>& out,
    uint64_t& nodeId) {
    out.clear();
    nodeId = 0;
    if (!_binding.enabled || !fromRelay) {
        if (bytes == nullptr && length != 0)
            return TopologyRelayIngress::Rejected;
        if (length != 0) out.assign(bytes, bytes + length);
        return TopologyRelayIngress::PassThrough;
    }
    if (!validTopologyRelayAuthorityBinding(_binding) || bytes == nullptr)
        return TopologyRelayIngress::Rejected;

    if (qnet::relay::hasTopologyFrameKind(bytes, length)) {
        qnet::relay::TopologyFrameHeader header{};
        if (qnet::relay::decodeTopologyFrameHeader(bytes, length, header)
                != qnet::relay::DecodeError::None
            || header.party != qnet::relay::TopologyParty::Node
            || header.nodeId == 0
            || header.authorityGeneration != _binding.authorityGeneration
            || length <= qnet::relay::kTopologyFrameHeaderSize) {
            return TopologyRelayIngress::Rejected;
        }
        const uint8_t* inner = bytes + qnet::relay::kTopologyFrameHeaderSize;
        const size_t innerLength =
            length - qnet::relay::kTopologyFrameHeaderSize;
        if (!validEnvelopeShape(inner, innerLength))
            return TopologyRelayIngress::Rejected;
        out.assign(inner, inner + innerLength);
        nodeId = header.nodeId;
        return TopologyRelayIngress::Data;
    }

    qnet::relay::TopologyMessageKind kind{};
    if (qnet::relay::inspectTopologyMessageKind(bytes, length, kind)
            != qnet::relay::DecodeError::None) {
        return TopologyRelayIngress::Rejected;
    }
    if (kind == qnet::relay::TopologyMessageKind::Ready) {
        qnet::relay::TopologyReady ready{};
        if (qnet::relay::decodeTopologyReady(bytes, length, ready)
                != qnet::relay::DecodeError::None
            || ready.party != qnet::relay::TopologyParty::Authority
            || !sameId(ready.room, _binding.room)
            || !sameId(ready.token, _binding.token)
            || ready.nodeId == 0
            || ready.authorityGeneration != _binding.authorityGeneration) {
            return TopologyRelayIngress::Rejected;
        }
        nodeId = ready.nodeId;
        return TopologyRelayIngress::Ready;
    }
    if (kind == qnet::relay::TopologyMessageKind::Detach) {
        qnet::relay::TopologyDetach detach{};
        if (qnet::relay::decodeTopologyDetach(bytes, length, detach)
                != qnet::relay::DecodeError::None
            || detach.party != qnet::relay::TopologyParty::Authority
            || !sameId(detach.room, _binding.room)
            || !sameId(detach.token, _binding.token)
            || detach.authorityGeneration != _binding.authorityGeneration) {
            return TopologyRelayIngress::Rejected;
        }
        nodeId = detach.nodeId;
        return TopologyRelayIngress::Detached;
    }

    return TopologyRelayIngress::Rejected;
}

bool TopologyRelayAuthorityAdapter::registrationDue(
    const uint64_t nowMs) const {
    if (!_binding.enabled) return false;
    if (_registrationAttempts == 0) return true;
    if (nowMs < _lastRegistrationMs) return true;
    const uint64_t retryMs =
        _registrationAttempts < kTopologyRelayFastRegistrationAttempts
            ? kTopologyRelayRegistrationRetryMs
            : kTopologyRelayAuthorityRefreshMs;
    return nowMs - _lastRegistrationMs >= retryMs;
}

bool TopologyRelayAuthorityAdapter::encodeRegistration(
    const uint64_t nowMs, std::vector<uint8_t>& out, const bool force) {
    out.clear();
    if (!validTopologyRelayAuthorityBinding(_binding)
        || (!force && !registrationDue(nowMs))) {
        return false;
    }
    qnet::relay::TopologyRegister registration{};
    registration.room = _binding.room;
    registration.party = qnet::relay::TopologyParty::Authority;
    registration.authorityGeneration = _binding.authorityGeneration;
    registration.token = _binding.token;
    registration.nodeId = 0;
    if (!qnet::relay::encodeTopologyRegister(registration, out)) return false;
    if (_registrationAttempts != std::numeric_limits<uint32_t>::max())
        ++_registrationAttempts;
    _lastRegistrationMs = nowMs;
    return true;
}

bool TopologyRelayAuthorityAdapter::encodeDetach(
    const uint64_t nodeId,
    const qnet::relay::TopologyDetachReason reason,
    std::vector<uint8_t>& out) const {
    out.clear();
    if (!validTopologyRelayAuthorityBinding(_binding)) return false;
    qnet::relay::TopologyDetach detach{};
    detach.room = _binding.room;
    detach.party = qnet::relay::TopologyParty::Authority;
    detach.reason = reason;
    detach.authorityGeneration = _binding.authorityGeneration;
    detach.token = _binding.token;
    detach.nodeId = nodeId;
    return qnet::relay::encodeTopologyDetach(detach, out);
}

namespace {

bool allZeroBytes(const uint8_t* bytes, const size_t length) {
    if (bytes == nullptr) return true;
    uint8_t value = 0u;
    for (size_t index = 0u; index < length; ++index) value |= bytes[index];
    return value == 0u;
}

qnet::relay::RoomId roomFromWire(const uint8_t (&bytes)[16]) {
    qnet::relay::RoomId room{};
    for (unsigned index = 0u; index < 8u; ++index) {
        room.low |= static_cast<uint64_t>(bytes[index]) << (index * 8u);
        room.high |= static_cast<uint64_t>(bytes[8u + index]) << (index * 8u);
    }
    return room;
}

void roomToWire(const qnet::relay::RoomId& room, uint8_t (&bytes)[16]) {
    for (unsigned index = 0u; index < 8u; ++index) {
        bytes[index] = static_cast<uint8_t>(room.low >> (index * 8u));
        bytes[8u + index] = static_cast<uint8_t>(room.high >> (index * 8u));
    }
}

}

bool relayQsrOfferAbsent(const qnet::SpectateTopologyInfoV2& info) {
    return info.relayIpv4BE == 0u && info.relayPort == 0u
        && info.reservedRelay == 0u
        && allZeroBytes(info.relayRoomId, sizeof(info.relayRoomId))
        && info.relayGrantId == 0u && info.relayGrantExpiryUnix == 0u
        && info.relayGrantAuthorityGeneration == 0u
        && info.reservedRelayGrant == 0u
        && info.relayGrantMacLow == 0u && info.relayGrantMacHigh == 0u;
}

bool relayQsrOfferFromInfo(const qnet::SpectateTopologyInfoV2& info,
                           RelayQsrOffer& out) {
    out = {};
    if (relayQsrOfferAbsent(info)) return true;
    if (info.reservedRelay != 0u || info.reservedRelayGrant != 0u
        || info.relayIpv4BE == 0u || info.relayPort == 0u
        || allZeroBytes(info.relayRoomId, sizeof(info.relayRoomId))
        || info.relayGrantId == 0u || info.relayGrantExpiryUnix == 0u
        || info.relayGrantAuthorityGeneration == 0u
        || (info.relayGrantMacLow == 0u && info.relayGrantMacHigh == 0u)) {
        return false;
    }
    if (info.relayIpv4BE == info.rootQsr.advertisedIpv4BE
        && info.relayPort == info.rootQsr.advertisedPort) {
        return false;
    }
    if (info.childNodeId == 0u || info.sessionId == 0u) return false;

    RelayQsrOffer offer{};
    offer.present = true;
    offer.relayIpv4BE = info.relayIpv4BE;
    offer.relayPort = info.relayPort;
    offer.room = roomFromWire(info.relayRoomId);
    if (!qnet::relay::isValidId(offer.room)) return false;
    offer.grant.grantId = info.relayGrantId;
    offer.grant.nodeId = info.childNodeId;
    offer.grant.sessionId = info.sessionId;
    offer.grant.expiryUnix = info.relayGrantExpiryUnix;
    offer.grant.authorityGeneration = info.relayGrantAuthorityGeneration;
    offer.grant.macLow = info.relayGrantMacLow;
    offer.grant.macHigh = info.relayGrantMacHigh;
    out = offer;
    return true;
}

bool writeRelayQsrOffer(const RelayQsrOffer& offer,
                        qnet::SpectateTopologyInfoV2& out) {
    if (!offer.present) {
        out.relayIpv4BE = 0u;
        out.relayPort = 0u;
        out.reservedRelay = 0u;
        for (uint8_t& byte : out.relayRoomId) byte = 0u;
        out.relayGrantId = 0u;
        out.relayGrantExpiryUnix = 0u;
        out.relayGrantAuthorityGeneration = 0u;
        out.reservedRelayGrant = 0u;
        out.relayGrantMacLow = 0u;
        out.relayGrantMacHigh = 0u;
        return true;
    }
    if (offer.relayIpv4BE == 0u || offer.relayPort == 0u
        || !qnet::relay::isValidId(offer.room)
        || offer.grant.grantId == 0u || offer.grant.expiryUnix == 0u
        || offer.grant.authorityGeneration == 0u
        || (offer.grant.macLow == 0u && offer.grant.macHigh == 0u)
        || offer.grant.nodeId != out.childNodeId
        || offer.grant.sessionId != out.sessionId
        || (offer.relayIpv4BE == out.rootQsr.advertisedIpv4BE
            && offer.relayPort == out.rootQsr.advertisedPort)) {
        return false;
    }
    out.relayIpv4BE = offer.relayIpv4BE;
    out.relayPort = offer.relayPort;
    out.reservedRelay = 0u;
    roomToWire(offer.room, out.relayRoomId);
    out.relayGrantId = offer.grant.grantId;
    out.relayGrantExpiryUnix = offer.grant.expiryUnix;
    out.relayGrantAuthorityGeneration = offer.grant.authorityGeneration;
    out.reservedRelayGrant = 0u;
    out.relayGrantMacLow = offer.grant.macLow;
    out.relayGrantMacHigh = offer.grant.macHigh;
    return true;
}

bool relayBindingFromOffer(const RelayQsrOffer& offer,
                           const qnet::relay::RegistrationToken& token,
                           TopologyRelayBinding& out) {
    out = {};
    if (!offer.present || !qnet::relay::isValidId(offer.room)
        || !qnet::relay::isValidId(token) || offer.grant.nodeId == 0u
        || offer.grant.authorityGeneration == 0u) {
        return false;
    }
    out.enabled = true;
    out.room = offer.room;
    out.token = token;
    out.nodeId = offer.grant.nodeId;
    out.authorityGeneration = offer.grant.authorityGeneration;
    return validTopologyRelayBinding(out);
}

}
