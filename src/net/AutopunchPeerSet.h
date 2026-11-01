#pragma once

#include "AutopunchSession.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace qnet::autopunch {

inline constexpr size_t kMaxSourcePunchPeers = 32;
inline constexpr size_t kMaxSourcePunchPeersPerIpv4 = 4;
inline constexpr uint64_t kPunchPeerLifetimeMs = 15000;

using PunchNodeId = uint64_t;

struct PunchPeer {
    PunchNodeId remoteNodeId = 0;
    Endpoint mappedEndpoint{};
    uint16_t reportedInternalPort = 0;

    uint64_t routeGeneration = 1;
    uint64_t firstSeenMs = 0;
    uint64_t punchWindowStartMs = 0;
    uint64_t lastPunchMs = 0;
    uint64_t expiresMs = 0;

    bool immediateBurstPending = false;
    bool routeProved = false;
    uint32_t burstsSent = 0;
    uint32_t packetsSent = 0;
};

enum class PunchMappingResult : uint8_t {
    Ignored = 0,
    AcceptedNew,
    Refreshed,
    Rebound,
    RejectedMalformed,
    RejectedCapacity,
    RejectedPerIpCapacity,
    RejectedSelfOrRelay,
};

struct PunchPeerStats {
    uint32_t mappingsAccepted = 0;
    uint32_t mappingsRejected = 0;
    uint32_t probesReceived = 0;
    uint32_t peersExpired = 0;
    uint32_t punchesSent = 0;
    uint32_t lookupsSent = 0;
    uint32_t seededPeers = 0;
};

struct PendingSourceLookup {
    uint32_t targetIpv4NetworkOrder = 0;
    uint16_t targetPort = 0;        // HOST order, as encodeLookup expects
    uint64_t firstSentMs = 0;
    uint64_t lastSentMs = 0;
    uint64_t expiresMs = 0;
    uint32_t attempts = 0;
};

inline constexpr uint64_t kSourceLookupWindowMs = 10000;
inline constexpr size_t kMaxSourceLookups = 32;

class AutopunchPeerSet {
public:
    void configure(const Endpoint& relay);
    void reset();
    bool configured() const { return _configured; }

    void setLocalPort(uint16_t localPort) { _localPort = localPort; }

    bool addLookupTarget(uint32_t targetIpv4NetworkOrder, uint16_t targetPort,
                         uint64_t nowMs);

    bool addKnownPeer(const Endpoint& endpoint, uint64_t nowMs);

    bool bindNode(const Endpoint& mappedEndpoint, PunchNodeId nodeId);

    bool handleInboundMapping(const uint8_t* bytes, size_t length,
                              const Endpoint& source, uint64_t nowMs,
                              PunchMappingResult& result,
                              PunchNodeId nodeId = 0);

    bool handleProbe(const uint8_t* bytes, size_t length,
                     const Endpoint& source, uint64_t nowMs);

    void service(SendFn send, void* context, uint64_t nowMs);
    void expire(uint64_t nowMs);

    size_t size() const { return _peers.size(); }
    const PunchPeer* find(const Endpoint& endpoint) const;
    const PunchPeerStats& stats() const { return _stats; }

private:
    PunchMappingResult acceptMapping(const Mapping& mapping, uint64_t nowMs,
                                     PunchNodeId nodeId);
    size_t countForIpv4(uint32_t ipv4NetworkOrder) const;
    static bool due(uint64_t lastMs, uint64_t nowMs, uint64_t intervalMs);

    void serviceLookups(SendFn send, void* context, uint64_t nowMs);

    bool _configured = false;
    uint16_t _localPort = 0;
    Endpoint _relay{};
    std::vector<PunchPeer> _peers;
    std::vector<PendingSourceLookup> _lookups;
    PunchPeerStats _stats{};
};

}
