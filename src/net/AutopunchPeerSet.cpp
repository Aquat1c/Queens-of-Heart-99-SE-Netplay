#include "AutopunchPeerSet.h"

#include <algorithm>

namespace qnet::autopunch {

namespace {

Endpoint endpointFor(const Mapping& mapping) {
    Endpoint endpoint{};
    endpoint.sa.sin_addr.s_addr = mapping.ipv4NetworkOrder;
    endpoint.sa.sin_port = htons(mapping.natPort);
    return endpoint;
}

}

void AutopunchPeerSet::configure(const Endpoint& relay) {
    reset();
    if (relay.sa.sin_addr.s_addr == 0 || relay.sa.sin_port == 0) return;
    _relay = relay;
    _configured = true;
    _peers.reserve(kMaxSourcePunchPeers);
}

void AutopunchPeerSet::reset() {
    _configured = false;
    _localPort = 0;
    _relay = Endpoint{};
    _peers.clear();
    _lookups.clear();
    _stats = PunchPeerStats{};
}

bool AutopunchPeerSet::addLookupTarget(const uint32_t targetIpv4NetworkOrder,
                                       const uint16_t targetPort,
                                       const uint64_t nowMs) {
    if (!_configured || targetIpv4NetworkOrder == 0 || targetPort == 0)
        return false;
    if (targetIpv4NetworkOrder == _relay.sa.sin_addr.s_addr
        && targetPort == ntohs(_relay.sa.sin_port)) {
        return false;
    }
    for (PendingSourceLookup& lookup : _lookups) {
        if (lookup.targetIpv4NetworkOrder != targetIpv4NetworkOrder
            || lookup.targetPort != targetPort) {
            continue;
        }
        lookup.expiresMs = nowMs + kSourceLookupWindowMs;
        return true;
    }
    if (_lookups.size() >= kMaxSourceLookups) return false;
    PendingSourceLookup lookup{};
    lookup.targetIpv4NetworkOrder = targetIpv4NetworkOrder;
    lookup.targetPort = targetPort;
    lookup.firstSentMs = 0;
    lookup.lastSentMs = 0;
    lookup.expiresMs = nowMs + kSourceLookupWindowMs;
    _lookups.push_back(lookup);
    return true;
}

bool AutopunchPeerSet::addKnownPeer(const Endpoint& endpoint,
                                    const uint64_t nowMs) {
    if (!_configured) return false;
    if (endpoint.sa.sin_addr.s_addr == 0 || endpoint.sa.sin_port == 0)
        return false;
    if (endpoint == _relay) return false;
    expire(nowMs);
    for (PunchPeer& peer : _peers) {
        if (!(peer.mappedEndpoint == endpoint)) continue;
        peer.expiresMs = nowMs + kPunchPeerLifetimeMs;
        return true;
    }
    if (_peers.size() >= kMaxSourcePunchPeers) return false;
    if (countForIpv4(endpoint.sa.sin_addr.s_addr)
        >= kMaxSourcePunchPeersPerIpv4) {
        return false;
    }
    PunchPeer peer{};
    peer.mappedEndpoint = endpoint;
    peer.firstSeenMs = nowMs;
    peer.punchWindowStartMs = nowMs;
    peer.expiresMs = nowMs + kPunchPeerLifetimeMs;
    peer.immediateBurstPending = true;
    _peers.push_back(peer);
    ++_stats.seededPeers;
    return true;
}

void AutopunchPeerSet::serviceLookups(SendFn send, void* context,
                                      const uint64_t nowMs) {
    if (_localPort == 0) {
        _lookups.clear();
        return;
    }
    for (size_t i = _lookups.size(); i-- > 0;) {
        PendingSourceLookup& lookup = _lookups[i];
        if (nowMs >= lookup.expiresMs) {
            _lookups.erase(_lookups.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }
        if (!due(lookup.lastSentMs, nowMs, kLookupIntervalMs)) continue;
        uint8_t wire[kLookupBytes]{};
        if (!encodeLookup(_localPort, lookup.targetIpv4NetworkOrder,
                          lookup.targetPort, wire)) {
            _lookups.erase(_lookups.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }
        if (!send(context, _relay, wire, sizeof(wire))) continue;
        if (lookup.firstSentMs == 0) lookup.firstSentMs = nowMs;
        lookup.lastSentMs = nowMs;
        ++lookup.attempts;
        ++_stats.lookupsSent;
    }
}

bool AutopunchPeerSet::bindNode(const Endpoint& mappedEndpoint,
                                 PunchNodeId nodeId) {
    if (nodeId == 0) return false;
    for (PunchPeer& peer : _peers) {
        if (!(peer.mappedEndpoint == mappedEndpoint)) continue;
        if (peer.remoteNodeId != 0 && peer.remoteNodeId != nodeId)
            return false;
        peer.remoteNodeId = nodeId;
        return true;
    }
    return false;
}

bool AutopunchPeerSet::handleInboundMapping(
    const uint8_t* bytes, size_t length, const Endpoint& source,
    uint64_t nowMs, PunchMappingResult& result, PunchNodeId nodeId) {
    result = PunchMappingResult::Ignored;
    if (!_configured || !(source == _relay))
        return false;

    if (length != kMappingBytes) {
        if (length == kPunchBytes) return false;
        ++_stats.mappingsRejected;
        result = PunchMappingResult::RejectedMalformed;
        return true;
    }

    Mapping mapping{};
    if (!decodeMapping(bytes, length, mapping)) {
        ++_stats.mappingsRejected;
        result = PunchMappingResult::RejectedMalformed;
        return true;
    }
    result = acceptMapping(mapping, nowMs, nodeId);
    if (result == PunchMappingResult::AcceptedNew
        || result == PunchMappingResult::Refreshed
        || result == PunchMappingResult::Rebound) {
        ++_stats.mappingsAccepted;
    } else {
        ++_stats.mappingsRejected;
    }
    return true;
}

bool AutopunchPeerSet::handleProbe(const uint8_t* bytes, size_t length,
                                   const Endpoint& source, uint64_t nowMs) {
    if (!isPunchProbe(bytes, length)) return false;
    ++_stats.probesReceived;
    for (PunchPeer& peer : _peers) {
        if (!(peer.mappedEndpoint == source)) continue;
        peer.routeProved = true;
        peer.expiresMs = nowMs + kPunchPeerLifetimeMs;
        return true;
    }
    return true;
}

void AutopunchPeerSet::service(SendFn send, void* context, uint64_t nowMs) {
    if (!_configured || send == nullptr) return;
    expire(nowMs);
    serviceLookups(send, context, nowMs);
    const uint8_t probe[kPunchBytes] = {kPunchByte};
    for (PunchPeer& peer : _peers) {
        if (peer.routeProved) continue;
        const bool inWindow = nowMs >= peer.punchWindowStartMs
            && nowMs - peer.punchWindowStartMs < kPunchWindowMs;
        if (!inWindow) continue;
        if (!peer.immediateBurstPending
            && !due(peer.lastPunchMs, nowMs, kPunchIntervalMs)) {
            continue;
        }
        uint32_t sent = 0;
        for (uint32_t i = 0; i < kPunchBurst; ++i) {
            if (send(context, peer.mappedEndpoint, probe, sizeof(probe))) {
                ++sent;
                ++_stats.punchesSent;
            }
        }
        peer.packetsSent += sent;
        ++peer.burstsSent;
        peer.lastPunchMs = nowMs;
        peer.immediateBurstPending = false;
    }
}

void AutopunchPeerSet::expire(uint64_t nowMs) {
    const auto firstExpired = std::remove_if(
        _peers.begin(), _peers.end(), [nowMs](const PunchPeer& peer) {
            return peer.expiresMs != 0 && nowMs >= peer.expiresMs;
        });
    _stats.peersExpired += static_cast<uint32_t>(
        std::distance(firstExpired, _peers.end()));
    _peers.erase(firstExpired, _peers.end());
}

const PunchPeer* AutopunchPeerSet::find(const Endpoint& endpoint) const {
    for (const PunchPeer& peer : _peers)
        if (peer.mappedEndpoint == endpoint) return &peer;
    return nullptr;
}

PunchMappingResult AutopunchPeerSet::acceptMapping(
    const Mapping& mapping, uint64_t nowMs, PunchNodeId nodeId) {
    expire(nowMs);
    const Endpoint mapped = endpointFor(mapping);
    if (mapped == _relay) return PunchMappingResult::RejectedSelfOrRelay;

    PunchPeer* existing = nullptr;
    if (nodeId != 0) {
        for (PunchPeer& peer : _peers) {
            if (peer.remoteNodeId == nodeId) {
                existing = &peer;
                break;
            }
        }
    }
    if (existing == nullptr) {
        for (PunchPeer& peer : _peers) {
            if (peer.mappedEndpoint == mapped) {
                existing = &peer;
                break;
            }
        }
    }

    if (existing != nullptr) {
        const bool endpointChanged = !(existing->mappedEndpoint == mapped);
        existing->mappedEndpoint = mapped;
        existing->reportedInternalPort = mapping.internalPort;
        if (nodeId != 0) existing->remoteNodeId = nodeId;
        existing->expiresMs = nowMs + kPunchPeerLifetimeMs;
        if (!endpointChanged) return PunchMappingResult::Refreshed;
        ++existing->routeGeneration;
        existing->routeProved = false;
        existing->immediateBurstPending = true;
        existing->punchWindowStartMs = nowMs;
        existing->lastPunchMs = 0;
        return PunchMappingResult::Rebound;
    }

    if (_peers.size() >= kMaxSourcePunchPeers)
        return PunchMappingResult::RejectedCapacity;
    if (countForIpv4(mapping.ipv4NetworkOrder) >= kMaxSourcePunchPeersPerIpv4)
        return PunchMappingResult::RejectedPerIpCapacity;

    PunchPeer peer{};
    peer.remoteNodeId = nodeId;
    peer.mappedEndpoint = mapped;
    peer.reportedInternalPort = mapping.internalPort;
    peer.firstSeenMs = nowMs;
    peer.punchWindowStartMs = nowMs;
    peer.expiresMs = nowMs + kPunchPeerLifetimeMs;
    peer.immediateBurstPending = true;
    _peers.push_back(peer);
    return PunchMappingResult::AcceptedNew;
}

size_t AutopunchPeerSet::countForIpv4(uint32_t ipv4NetworkOrder) const {
    size_t count = 0;
    for (const PunchPeer& peer : _peers) {
        if (peer.mappedEndpoint.sa.sin_addr.s_addr == ipv4NetworkOrder)
            ++count;
    }
    return count;
}

bool AutopunchPeerSet::due(uint64_t lastMs, uint64_t nowMs,
                            uint64_t intervalMs) {
    return lastMs == 0 || nowMs < lastMs || nowMs - lastMs >= intervalMs;
}

}
