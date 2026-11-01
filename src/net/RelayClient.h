#pragma once

#include "RelayProtocol.h"
#include "RouteCandidates.h"
#include "Sockets.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace qnet::relay {

constexpr uint64_t kRegisterIntervalMs = 500;
constexpr uint64_t kDirectPunchIntervalMs = 125;
constexpr uint8_t kDirectPunchByte = 0;

constexpr uint64_t kObservedRequestIntervalMs = 500;
constexpr uint32_t kMaxObservedRequests = 6;

const char* decodeErrorText(DecodeError error);

struct ObservedRegistration {
    bool valid = false;
    Endpoint observedPublic{};
    bool relaySupportsObserved = false;
    bool usableAsPublicCandidate = false;
};

bool isAcceptableObservedEndpoint(const Endpoint& endpoint);

bool isPublicObservedCandidate(const Endpoint& endpoint);

namespace observed_ext {

template <class W>
auto readWelcomeObservedImpl(const W& welcome, Ipv4Endpoint& out, int)
    -> decltype(static_cast<void>(welcome.observedPublic), true) {
    out = welcome.observedPublic;
    return true;
}

template <class W>
auto readWelcomeObservedImpl(const W& welcome, Ipv4Endpoint& out, long)
    -> decltype(static_cast<void>(welcome.observedEndpoint), true) {
    out = welcome.observedEndpoint;
    return true;
}

template <class W>
bool readWelcomeObservedImpl(const W&, Ipv4Endpoint&, ...) {
    return false;
}

template <class W>
bool readWelcomeObserved(const W& welcome, Ipv4Endpoint& out) {
    return readWelcomeObservedImpl(welcome, out, 0);
}

}

enum class PairPoll {
    Waiting,
    Ready,
    Rejected,
    Closed,
    Error,
};

struct PairResult {
    RoomId room{};
    LaneDomain domain = LaneDomain::Player;
    RelayRole role = RelayRole::Host;
    RegistrationToken token{};
    RegistrationToken spectatorRegistrationToken{};
    Id128 viewerAccessToken{};
    Endpoint relayEndpoint{};
    ObservedRegistration observed{};
};

bool generateRoomId(RoomId& out, std::string* error = nullptr,
                    bool* degradedRandom = nullptr);

bool deriveEndpointRoomId(const Endpoint& advertisedHost, RoomId& out);

class RelayPairClient {
public:
    bool begin(TcpConn& conn,
               const Endpoint& relayEndpoint,
               const RoomId& room,
               LaneDomain domain,
               RelayRole role,
               int connectTimeoutMs,
               std::string* error = nullptr);

    PairPoll poll(TcpConn& conn, int timeoutMs = 0,
                  std::string* error = nullptr);

    bool begun() const { return _begun; }
    bool ready() const { return _ready; }
    WelcomeStatus lastStatus() const { return _lastStatus; }
    const PairResult& result() const { return _result; }
    const std::string& errorText() const { return _error; }
    const ObservedRegistration& observedRegistration() const {
        return _result.observed;
    }

private:
    PairPoll fail(PairPoll outcome, const std::string& message,
                  std::string* error);

    bool _begun = false;
    bool _ready = false;
    bool _terminal = false;
    PairPoll _terminalOutcome = PairPoll::Error;
    WelcomeStatus _lastStatus = WelcomeStatus::Rejected;
    PairResult _result{};
    std::string _error;
};

enum class UdpInbound {
    NotHandled,
    Consumed,
    Ready,
    DirectPeerObserved,
};

struct RelayUdpStats {
    uint32_t registersSent = 0;
    uint32_t directPunchesSent = 0;
    uint32_t directPunchesReceived = 0;
    uint32_t readyAccepted = 0;
    uint32_t invalidRelayControl = 0;
    uint32_t observedRequestsSent = 0;
    uint32_t observedAccepted = 0;
    uint32_t observedRejected = 0;
};

using RelaySendFn = bool (*)(void* context, const Endpoint& destination,
                             const void* bytes, size_t length);

class RelayUdpClient {
public:
    bool configure(const PairResult& pair, std::string* error = nullptr);
    void reset();
    void setDirectPunchEnabled(bool enabled) {
        _directPunchEnabled = enabled;
    }

    bool service(UdpSock& socket, uint64_t nowMs,
                 std::string* error = nullptr);
    bool serviceWith(RelaySendFn send, void* context, uint64_t nowMs,
                     std::string* error = nullptr);

    UdpInbound handleInbound(const uint8_t* bytes, size_t length,
                             const Endpoint& source, uint64_t nowMs = 0);

    void setObservedDiscoveryEnabled(bool enabled) {
        _observedEnabled = enabled;
    }
    bool observedDiscoveryEnabled() const { return _observedEnabled; }

    bool observedDiscoveryExhausted() const { return _observedExhausted; }

    const ObservedRegistration& observedRegistration() const {
        return _observed;
    }

    bool applyObservedRegistration(const RegistrationToken& token,
                                   const Endpoint& observed,
                                   std::string* error = nullptr);

    bool configured() const { return _configured; }
    bool ready() const { return _ready; }
    bool directPeerObserved() const { return _directObserved; }
    const Endpoint& directPeer() const { return _directPeer; }
    const Endpoint& relayEndpoint() const { return _pair.relayEndpoint; }
    const RelayUdpStats& stats() const { return _stats; }

private:
    PairResult _pair{};
    Endpoint _directPeer{};
    ObservedRegistration _observed{};
    bool _configured = false;
    bool _ready = false;
    bool _directObserved = false;
    bool _directPunchEnabled = true;
    bool _observedEnabled = false;
    bool _observedExhausted = false;
    bool _observedRequestAttempted = false;
    bool _registerAttempted = false;
    bool _punchAttempted = false;
    uint32_t _observedRequestsSent = 0;
    uint64_t _lastObservedRequestMs = 0;
    uint64_t _lastRegisterMs = 0;
    uint64_t _lastPunchMs = 0;
    uint64_t _readyAtMs = 0;
    RelayUdpStats _stats{};
};

}
