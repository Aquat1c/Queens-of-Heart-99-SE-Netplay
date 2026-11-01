#pragma once

#include "Autopunch.h"
#include "Sockets.h"

#include <cstdint>

namespace qnet::autopunch {

using SendFn = bool (*)(void* context, const Endpoint& destination,
                        const void* bytes, size_t length);

struct SessionStats {
    uint32_t registersSent = 0;
    uint32_t lookupsSent = 0;
    uint32_t mappingsReceived = 0;
    uint32_t mappingsRejected = 0;
    uint32_t mappingTargetMismatches = 0;
    uint32_t mappingExternalPortAliases = 0;
    uint32_t mappingSameIpAlternates = 0;
    uint16_t lastMappingInternalPort = 0;
    uint16_t lastMappingNatPort = 0;
    uint32_t lastMappingIpv4NetworkOrder = 0;
    uint32_t punchesSent = 0;
    uint32_t punchesReceived = 0;
};

inline constexpr uint64_t kRegisterIntervalMs = 2000;
inline constexpr uint64_t kLookupIntervalMs = 1000;
inline constexpr uint32_t kPunchBurst = 4;
inline constexpr uint64_t kPunchIntervalMs = 250;
inline constexpr uint64_t kPunchWindowMs = 10000;

class AutopunchSession {
public:
    void configure(uint16_t localPort, const Endpoint& relay);
    void reset();
    bool configured() const { return _configured; }

    // The port is in host order: encodeLookup serialises it big-endian itself.
    void setTarget(uint32_t ipv4NetworkOrder, uint16_t portHostOrder);
    bool haveTarget() const { return _haveTarget; }

    void service(SendFn send, void* context, uint64_t nowMs);

    bool handleInbound(const uint8_t* bytes, size_t length,
                       const Endpoint& source, uint64_t nowMs);

    bool havePeerMapping() const { return _havePeerMapping; }
    const Endpoint& peerMapping() const { return _peerMapping; }

    bool peerReachable() const { return _peerReachable; }

    const SessionStats& stats() const { return _stats; }

private:
    bool sendRegister(SendFn send, void* context);
    bool sendLookup(SendFn send, void* context);
    void sendPunchBurst(SendFn send, void* context, uint32_t count);

    bool _configured = false;
    uint16_t _localPort = 0;
    Endpoint _relay{};

    bool _haveTarget = false;
    uint32_t _targetIpv4 = 0;
    uint16_t _targetPort = 0;

    bool _havePeerMapping = false;
    Endpoint _peerMapping{};
    bool _peerReachable = false;

    uint64_t _firstServiceMs = 0;
    uint64_t _punchWindowStartMs = 0;
    uint64_t _lastRegisterMs = 0;
    uint64_t _lastLookupMs = 0;
    uint64_t _lastPunchMs = 0;
    bool _registerSentOnce = false;
    bool _lookupSentOnce = false;
    bool _punchDue = false;

    SessionStats _stats{};
};

}
