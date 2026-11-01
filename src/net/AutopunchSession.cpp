#include "AutopunchSession.h"

#include <cstring>

namespace qnet::autopunch {

namespace {

bool due(bool sentOnce, uint64_t last, uint64_t now, uint64_t interval) {
    if (!sentOnce) return true;
    if (now < last) return true;
    return now - last >= interval;
}

}

void AutopunchSession::configure(uint16_t localPort, const Endpoint& relay) {
    reset();
    _localPort = localPort;
    _relay = relay;
    _configured = localPort != 0;
}

void AutopunchSession::reset() {
    _configured = false;
    _localPort = 0;
    _relay = Endpoint{};
    _haveTarget = false;
    _targetIpv4 = 0;
    _targetPort = 0;
    _havePeerMapping = false;
    _peerMapping = Endpoint{};
    _peerReachable = false;
    _firstServiceMs = 0;
    _punchWindowStartMs = 0;
    _lastRegisterMs = 0;
    _lastLookupMs = 0;
    _lastPunchMs = 0;
    _registerSentOnce = false;
    _lookupSentOnce = false;
    _punchDue = false;
    _stats = SessionStats{};
}

void AutopunchSession::setTarget(uint32_t ipv4NetworkOrder,
                                 uint16_t portHostOrder) {
    _haveTarget = ipv4NetworkOrder != 0u && portHostOrder != 0u;
    _targetIpv4 = _haveTarget ? ipv4NetworkOrder : 0u;
    _targetPort = _haveTarget ? portHostOrder : 0u;
}

bool AutopunchSession::sendRegister(SendFn send, void* context) {
    uint8_t packet[kRegisterBytes];
    if (!encodeRegister(_localPort, packet)) return false;
    if (!send(context, _relay, packet, sizeof(packet))) return false;
    ++_stats.registersSent;
    return true;
}

bool AutopunchSession::sendLookup(SendFn send, void* context) {
    uint8_t packet[kLookupBytes];
    if (!encodeLookup(_localPort, _targetIpv4, _targetPort, packet))
        return false;
    if (!send(context, _relay, packet, sizeof(packet))) return false;
    ++_stats.lookupsSent;
    return true;
}

void AutopunchSession::sendPunchBurst(SendFn send, void* context,
                                      uint32_t count) {
    const uint8_t probe[kPunchBytes] = { kPunchByte };
    for (uint32_t i = 0; i < count; ++i) {
        if (send(context, _peerMapping, probe, sizeof(probe)))
            ++_stats.punchesSent;
    }
}

void AutopunchSession::service(SendFn send, void* context, uint64_t nowMs) {
    if (!_configured || send == nullptr) return;
    if (_firstServiceMs == 0) {
        _firstServiceMs = nowMs;
        _punchWindowStartMs = nowMs;
    }

    if (due(_registerSentOnce, _lastRegisterMs, nowMs, kRegisterIntervalMs)) {
        if (sendRegister(send, context)) {
            _registerSentOnce = true;
            _lastRegisterMs = nowMs;
        }
    }

    if (_haveTarget && !_havePeerMapping
        && due(_lookupSentOnce, _lastLookupMs, nowMs, kLookupIntervalMs)) {
        if (sendLookup(send, context)) {
            _lookupSentOnce = true;
            _lastLookupMs = nowMs;
        }
    }

    const bool windowOpen = (nowMs - _punchWindowStartMs) < kPunchWindowMs;
    if (_havePeerMapping && !_peerReachable && windowOpen
        && (_punchDue || due(true, _lastPunchMs, nowMs, kPunchIntervalMs))) {
        sendPunchBurst(send, context, kPunchBurst);
        _lastPunchMs = nowMs;
        _punchDue = false;
    }
}

bool AutopunchSession::handleInbound(const uint8_t* bytes, size_t length,
                                     const Endpoint& source, uint64_t nowMs) {
    if (!_configured || bytes == nullptr) return false;

    if (isPunchProbe(bytes, length)) {
        ++_stats.punchesReceived;
        if (_havePeerMapping && source.sa.sin_addr.s_addr == _peerMapping.sa.sin_addr.s_addr
            && source.sa.sin_port == _peerMapping.sa.sin_port) {
            _peerReachable = true;
        }
        return true;
    }

    if (length != kMappingBytes) return false;

    if (source.sa.sin_addr.s_addr != _relay.sa.sin_addr.s_addr
        || source.sa.sin_port != _relay.sa.sin_port)
        return false;

    Mapping mapping{};
    if (!decodeMapping(bytes, length, mapping)) {
        ++_stats.mappingsRejected;
        return true;
    }

    ++_stats.mappingsReceived;
    _stats.lastMappingInternalPort = mapping.internalPort;
    _stats.lastMappingNatPort = mapping.natPort;
    _stats.lastMappingIpv4NetworkOrder = mapping.ipv4NetworkOrder;

    const bool exactInternalTarget =
        mapping.ipv4NetworkOrder == _targetIpv4
        && mapping.internalPort == _targetPort;
    const bool externalPortAlias =
        mapping.ipv4NetworkOrder == _targetIpv4
        && mapping.natPort == _targetPort;
    const bool exactTargetIp = mapping.ipv4NetworkOrder == _targetIpv4;
    if (_haveTarget && !exactTargetIp) {
        ++_stats.mappingsRejected;
        ++_stats.mappingTargetMismatches;
        return true;
    }
    if (_haveTarget && !exactInternalTarget && externalPortAlias)
        ++_stats.mappingExternalPortAliases;
    if (_haveTarget && exactTargetIp
        && !exactInternalTarget && !externalPortAlias)
        ++_stats.mappingSameIpAlternates;
    Endpoint mapped{};
    mapped.sa.sin_addr.s_addr = mapping.ipv4NetworkOrder;
    mapped.sa.sin_port = htons(mapping.natPort);

    const bool changed = !_havePeerMapping
        || _peerMapping.sa.sin_addr.s_addr != mapped.sa.sin_addr.s_addr
        || _peerMapping.sa.sin_port != mapped.sa.sin_port;
    _peerMapping = mapped;
    _havePeerMapping = true;
    if (changed) {
        _peerReachable = false;
        _punchDue = true;
        _punchWindowStartMs = nowMs;
    }
    return true;
}

}
