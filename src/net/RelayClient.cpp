#include "RelayClient.h"

#include "SecureRandom.h"
#include "../Log.h"

#include <vector>

namespace qnet::relay {
namespace {

bool isZero(const Id128& id) {
    return id.low == 0 && id.high == 0;
}

void publishError(const std::string& value, std::string* out) {
    if (out) *out = value;
}

bool validSocketEndpoint(const Endpoint& endpoint) {
    if (endpoint.sa.sin_family != AF_INET ||
        endpoint.sa.sin_addr.s_addr == 0 ||
        endpoint.sa.sin_port == 0) {
        return false;
    }
    const Ipv4Endpoint wire{
        endpoint.sa.sin_addr.s_addr,
        ntohs(endpoint.sa.sin_port),
    };
    return isValidEndpoint(wire);
}

Endpoint fromWireEndpoint(const Ipv4Endpoint& endpoint) {
    Endpoint out;
    out.sa.sin_family = AF_INET;
    out.sa.sin_addr.s_addr = endpoint.ipv4;
    out.sa.sin_port = htons(endpoint.port);
    return out;
}

bool endpointIpText(const Endpoint& endpoint, std::string& out) {
    char text[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &endpoint.sa.sin_addr, text, sizeof(text)))
        return false;
    out.assign(text);
    return true;
}

const char* welcomeStatusText(WelcomeStatus status) {
    switch (status) {
    case WelcomeStatus::Accepted:     return "accepted";
    case WelcomeStatus::Waiting:      return "waiting for peer";
    case WelcomeStatus::RoomNotFound: return "room not found";
    case WelcomeStatus::RoomFull:     return "room full";
    case WelcomeStatus::Rejected:     return "request rejected";
    case WelcomeStatus::ServerFull:   return "relay server full";
    }
    return "invalid status";
}

bool intervalDue(bool attempted, uint64_t lastMs, uint64_t nowMs,
                 uint64_t intervalMs) {
    return !attempted || nowMs < lastMs || nowMs - lastMs >= intervalMs;
}

bool udpSockSend(void* context, const Endpoint& destination,
                 const void* bytes, size_t length) {
    auto* socket = static_cast<UdpSock*>(context);
    return socket && socket->sendTo(destination, bytes, length);
}

}

const char* decodeErrorText(DecodeError error) {
    switch (error) {
    case DecodeError::None:            return "none";
    case DecodeError::TooShort:        return "too short";
    case DecodeError::BadMagic:        return "bad magic";
    case DecodeError::BadVersion:      return "bad version";
    case DecodeError::BadKind:         return "bad kind";
    case DecodeError::BadLength:       return "bad length";
    case DecodeError::BadReserved:     return "non-zero reserved field";
    case DecodeError::BadDomain:       return "bad lane domain";
    case DecodeError::BadRole:         return "bad relay role";
    case DecodeError::BadStatus:       return "bad welcome status";
    case DecodeError::InvalidRoom:     return "invalid room";
    case DecodeError::InvalidToken:    return "invalid token";
    case DecodeError::InvalidEndpoint: return "invalid endpoint";
    case DecodeError::InvalidGrant:    return "invalid spectator grant";
    case DecodeError::InvalidParty:    return "invalid spectator party";
    }
    return "unknown decode error";
}

bool isAcceptableObservedEndpoint(const Endpoint& endpoint) {
    if (endpoint.sa.sin_family != AF_INET) return false;
    const uint32_t addrBE = endpoint.sa.sin_addr.s_addr;
    if (addrBE == 0 || endpoint.sa.sin_port == 0) return false;
    if (isNonUnicastBE(addrBE)) return false;
    if (isLinkLocalBE(addrBE)) return false;
    return true;
}

bool isPublicObservedCandidate(const Endpoint& endpoint) {
    if (!isAcceptableObservedEndpoint(endpoint)) return false;
    const uint32_t addrBE = endpoint.sa.sin_addr.s_addr;
    return !isPrivateBE(addrBE) && !isLoopbackBE(addrBE);
}

bool generateRoomId(RoomId& out, std::string* error, bool* degradedRandom) {
    if (degradedRandom) *degradedRandom = false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        RoomId candidate{};
        bool degraded = false;
        if (!secureRandomBytes(&candidate, sizeof(candidate), degraded)) {
            publishError(
                std::string("secure random provider (")
                    + secureRandomBackendName()
                    + ") failed while creating relay room",
                error);
            return false;
        }
        if (degraded) {
            if (degradedRandom) *degradedRandom = true;
            QLOG("[relay] WARNING: relay room id drawn from the %s provider",
                 secureRandomBackendName());
        }
        if (!isZero(candidate)) {
            out = candidate;
            publishError({}, error);
            return true;
        }
    }
    publishError("secure random provider repeatedly returned an all-zero room",
                 error);
    return false;
}

bool deriveEndpointRoomId(const Endpoint& advertisedHost, RoomId& out) {
    if (!validSocketEndpoint(advertisedHost)) return false;
    std::string endpointText;
    if (!endpointIpText(advertisedHost, endpointText)) return false;
    endpointText += ":" + std::to_string(ntohs(advertisedHost.sa.sin_port));

    auto hash = [&](uint64_t value) {
        static constexpr char kDomain[] = "QOH99-AUTO-ROOM-V1|";
        for (const unsigned char ch : std::string(kDomain) + endpointText) {
            value ^= ch;
            value *= 1099511628211ull;
        }
        value ^= value >> 30;
        value *= 0xBF58476D1CE4E5B9ull;
        value ^= value >> 27;
        value *= 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    };
    RoomId candidate{
        hash(1469598103934665603ull),
        hash(0x84222325CBF29CE4ull),
    };
    if (isZero(candidate)) candidate.low = 1;
    out = candidate;
    return true;
}

PairPoll RelayPairClient::fail(PairPoll outcome, const std::string& message,
                               std::string* error) {
    _terminal = true;
    _terminalOutcome = outcome;
    _error = message;
    publishError(_error, error);
    return outcome;
}

bool RelayPairClient::begin(TcpConn& conn,
                            const Endpoint& relayEndpoint,
                            const RoomId& room,
                            LaneDomain domain,
                            RelayRole role,
                            int connectTimeoutMs,
                            std::string* error) {
    _begun = false;
    _ready = false;
    _terminal = false;
    _terminalOutcome = PairPoll::Error;
    _lastStatus = WelcomeStatus::Rejected;
    _result = {};
    _error.clear();
    publishError({}, error);

    if (!isValidId(room)) {
        fail(PairPoll::Error, "relay room id is invalid", error);
        return false;
    }
    if (!isValidDomain(domain) || !isValidRole(role)) {
        fail(PairPoll::Error, "relay lane domain or role is invalid", error);
        return false;
    }
    if (!validSocketEndpoint(relayEndpoint)) {
        fail(PairPoll::Error, "relay endpoint must be a numeric IPv4 address and non-zero port",
             error);
        return false;
    }
    if (connectTimeoutMs < 0) {
        fail(PairPoll::Error, "relay connect timeout cannot be negative", error);
        return false;
    }

    Hello hello{};
    hello.room = room;
    hello.domain = domain;
    hello.role = role;
    std::vector<uint8_t> encoded;
    if (!encodeHello(hello, encoded)) {
        fail(PairPoll::Error, "failed to encode relay Hello", error);
        return false;
    }

    std::string ip;
    if (!endpointIpText(relayEndpoint, ip)) {
        fail(PairPoll::Error, "failed to format numeric relay IPv4 address", error);
        return false;
    }
    if (!conn.connectTo(ip, ntohs(relayEndpoint.sa.sin_port),
                        connectTimeoutMs)) {
        fail(PairPoll::Error, "failed to connect to relay TCP endpoint", error);
        return false;
    }
    if (!conn.sendMsg(kTcpEnvelopeType, encoded.data(), encoded.size())) {
        fail(PairPoll::Error, "failed to send relay Hello", error);
        return false;
    }

    _result.room = room;
    _result.domain = domain;
    _result.role = role;
    _result.relayEndpoint = relayEndpoint;
    _begun = true;
    return true;
}

PairPoll RelayPairClient::poll(TcpConn& conn, int timeoutMs,
                               std::string* error) {
    publishError({}, error);
    if (_ready) return PairPoll::Ready;
    if (_terminal) {
        publishError(_error, error);
        return _terminalOutcome;
    }
    if (!_begun)
        return fail(PairPoll::Error, "relay pairing has not begun", error);
    if (timeoutMs < 0)
        return fail(PairPoll::Error, "relay poll timeout cannot be negative", error);

    TcpConn::Received received;
    switch (conn.recvMsgEx(received, timeoutMs)) {
    case TcpConn::Rs::Timeout:
        return PairPoll::Waiting;
    case TcpConn::Rs::Closed:
        return fail(PairPoll::Closed,
                    "relay TCP connection closed before Welcome", error);
    case TcpConn::Rs::Corrupt:
        return fail(PairPoll::Error,
                    "relay TCP stream contained a corrupt qnet frame", error);
    case TcpConn::Rs::Error:
        return fail(PairPoll::Error,
                    "relay TCP receive failed before Welcome", error);
    case TcpConn::Rs::Msg:
        break;
    }

    if (received.type != kTcpEnvelopeType) {
        return fail(PairPoll::Error,
                    "unexpected qnet frame before relay Welcome", error);
    }
    Welcome welcome{};
    const DecodeError decoded =
        decodeWelcome(received.body.data(), received.body.size(), welcome);
    if (decoded != DecodeError::None) {
        return fail(PairPoll::Error,
                    std::string("invalid relay Welcome: ") +
                        decodeErrorText(decoded),
                    error);
    }
    if (welcome.room != _result.room ||
        welcome.domain != _result.domain ||
        welcome.role != _result.role) {
        return fail(PairPoll::Error,
                    "relay Welcome did not echo room, lane, and role", error);
    }

    _lastStatus = welcome.status;
    if (welcome.status == WelcomeStatus::Waiting)
        return PairPoll::Waiting;
    if (welcome.status != WelcomeStatus::Accepted) {
        return fail(PairPoll::Rejected,
                    std::string("relay rejected pairing: ") +
                        welcomeStatusText(welcome.status),
                    error);
    }
    if (!isValidId(welcome.registrationToken)) {
        return fail(PairPoll::Error,
                    "accepted relay Welcome omitted its registration token",
                    error);
    }

    _result.token = welcome.registrationToken;
    _result.spectatorRegistrationToken =
        welcome.spectatorRegistrationToken;
    _result.viewerAccessToken = welcome.viewerAccessToken;

    _result.observed = ObservedRegistration{};
    Ipv4Endpoint wireObserved{};
    if (observed_ext::readWelcomeObserved(welcome, wireObserved)
        && isValidEndpoint(wireObserved)) {
        const Endpoint observed = fromWireEndpoint(wireObserved);
        if (isAcceptableObservedEndpoint(observed)) {
            _result.observed.relaySupportsObserved = true;
            _result.observed.valid = true;
            _result.observed.observedPublic = observed;
            _result.observed.usableAsPublicCandidate =
                isPublicObservedCandidate(observed);
            QLOG("[relay] observed public endpoint %s (public-candidate=%d)",
                 observed.str().c_str(),
                 _result.observed.usableAsPublicCandidate ? 1 : 0);
        } else {
            QLOG("[relay] discarded an unusable observed endpoint from the "
                 "relay Welcome");
        }
    }

    _ready = true;
    // Stream handoff boundary: nothing after this point may call recvMsgEx, so a coalesced peer qnet frame stays buffered.
    return PairPoll::Ready;
}

void RelayUdpClient::reset() {
    _pair = {};
    _directPeer = {};
    _observed = {};
    _configured = false;
    _ready = false;
    _directObserved = false;
    _directPunchEnabled = true;
    _observedEnabled = false;
    _observedExhausted = false;
    _observedRequestAttempted = false;
    _observedRequestsSent = 0;
    _lastObservedRequestMs = 0;
    _registerAttempted = false;
    _punchAttempted = false;
    _lastRegisterMs = 0;
    _lastPunchMs = 0;
    _readyAtMs = 0;
    _stats = {};
}

bool RelayUdpClient::configure(const PairResult& pair, std::string* error) {
    reset();
    publishError({}, error);
    if (!isValidId(pair.room) || !isValidId(pair.token)) {
        publishError("relay UDP configuration has an invalid room or token",
                     error);
        return false;
    }
    if (!isValidDomain(pair.domain) || !isValidRole(pair.role)) {
        publishError("relay UDP configuration has an invalid lane or role",
                     error);
        return false;
    }
    if (!validSocketEndpoint(pair.relayEndpoint)) {
        publishError("relay UDP configuration has an invalid server endpoint",
                     error);
        return false;
    }
    _pair = pair;
    _observed = pair.observed;
    _observedEnabled = pair.domain == LaneDomain::Player
                    && pair.role == RelayRole::Host;
    _configured = true;
    return true;
}

bool RelayUdpClient::applyObservedRegistration(const RegistrationToken& token,
                                               const Endpoint& observed,
                                               std::string* error) {
    publishError({}, error);
    if (!_configured) {
        publishError("relay UDP client is not configured", error);
        return false;
    }
    if (!isValidId(token) || token != _pair.token) {
        publishError(
            "relay reported an observed endpoint for a different registration",
            error);
        return false;
    }
    if (!isAcceptableObservedEndpoint(observed)) {
        publishError("relay reported an unusable observed endpoint", error);
        return false;
    }
    if (_observed.valid && !(_observed.observedPublic == observed)) {
        _observed = ObservedRegistration{};
        publishError("relay reported conflicting observed endpoints for this "
                     "registration",
                     error);
        return false;
    }

    _observed.relaySupportsObserved = true;
    _observed.valid = true;
    _observed.observedPublic = observed;
    _observed.usableAsPublicCandidate = isPublicObservedCandidate(observed);
    return true;
}

bool RelayUdpClient::service(UdpSock& socket, uint64_t nowMs,
                             std::string* error) {
    if (!socket.isOpen()) {
        publishError("cannot service relay UDP on a closed socket", error);
        return false;
    }
    return serviceWith(&udpSockSend, &socket, nowMs, error);
}

bool RelayUdpClient::serviceWith(RelaySendFn send, void* context,
                                 uint64_t nowMs, std::string* error) {
    publishError({}, error);
    if (!_configured) {
        publishError("relay UDP client is not configured", error);
        return false;
    }
    if (!send) {
        publishError("relay UDP send callback is null", error);
        return false;
    }

    if (!_ready &&
        intervalDue(_registerAttempted, _lastRegisterMs, nowMs,
                    kRegisterIntervalMs)) {
        Register registration{};
        registration.room = _pair.room;
        registration.domain = _pair.domain;
        registration.role = _pair.role;
        registration.token = _pair.token;
        std::vector<uint8_t> encoded;
        if (!encodeRegister(registration, encoded)) {
            publishError("failed to encode relay UDP Register", error);
            return false;
        }

        _registerAttempted = true;
        _lastRegisterMs = nowMs;
        if (!send(context, _pair.relayEndpoint,
                  encoded.data(), encoded.size())) {
            publishError("failed to send relay UDP Register", error);
            return false;
        }
        ++_stats.registersSent;
    }

    if (_observedEnabled && !_observed.valid && !_observedExhausted &&
        intervalDue(_observedRequestAttempted, _lastObservedRequestMs, nowMs,
                    kObservedRequestIntervalMs)) {
        _observedRequestAttempted = true;
        _lastObservedRequestMs = nowMs;
        if (_observedRequestsSent >= kMaxObservedRequests) {
            _observedExhausted = true;
        } else {
            ObservedRequest request{};
            request.room = _pair.room;
            request.domain = _pair.domain;
            request.role = _pair.role;
            request.token = _pair.token;
            std::vector<uint8_t> encoded;
            if (!encodeObservedRequest(request, encoded)) {
                _observedExhausted = true;
            } else if (send(context, _pair.relayEndpoint, encoded.data(),
                            encoded.size())) {
                ++_observedRequestsSent;
                ++_stats.observedRequestsSent;
            }
        }
    }

    if (_ready && _directPunchEnabled &&
        intervalDue(_punchAttempted, _lastPunchMs, nowMs,
                    kDirectPunchIntervalMs)) {
        _punchAttempted = true;
        _lastPunchMs = nowMs;
        if (!send(context, _directPeer,
                  &kDirectPunchByte, sizeof(kDirectPunchByte))) {
            publishError("failed to send direct relay-rendezvous punch", error);
            return false;
        }
        ++_stats.directPunchesSent;
    }
    return true;
}

UdpInbound RelayUdpClient::handleInbound(const uint8_t* bytes, size_t length,
                                         const Endpoint& source,
                                         uint64_t nowMs) {
    if (!_configured || !bytes)
        return UdpInbound::NotHandled;

    if (source == _pair.relayEndpoint) {
        if (!hasRelayMagic(bytes, length))
            return UdpInbound::NotHandled;

        uint16_t recordVersion = 0;
        if (!peekRelayVersion(bytes, length, recordVersion)) {
            ++_stats.invalidRelayControl;
            return UdpInbound::Consumed;
        }
        if (recordVersion == kObservedVersion) {
            ObservedEndpoint answer{};
            if (decodeObservedEndpoint(bytes, length, answer)
                    != DecodeError::None ||
                answer.room != _pair.room ||
                answer.domain != _pair.domain ||
                answer.role != _pair.role ||
                answer.token != _pair.token ||
                !isValidEndpoint(answer.observed)) {
                ++_stats.observedRejected;
                return UdpInbound::Consumed;
            }
            if (applyObservedRegistration(_pair.token,
                                          fromWireEndpoint(answer.observed))) {
                ++_stats.observedAccepted;
            } else {
                ++_stats.observedRejected;
            }
            return UdpInbound::Consumed;
        }
        if (recordVersion != kVersion) {
            ++_stats.invalidRelayControl;
            return UdpInbound::Consumed;
        }

        MessageKind kind = MessageKind::Hello;
        if (inspectMessageKind(bytes, length, kind) != DecodeError::None ||
            kind != MessageKind::Ready) {
            ++_stats.invalidRelayControl;
            return UdpInbound::Consumed;
        }

        Ready ready{};
        if (decodeReady(bytes, length, ready) != DecodeError::None ||
            ready.room != _pair.room ||
            ready.domain != _pair.domain ||
            ready.role != _pair.role ||
            ready.token != _pair.token ||
            !isValidEndpoint(ready.peer)) {
            ++_stats.invalidRelayControl;
            return UdpInbound::Consumed;
        }

        const Endpoint direct = fromWireEndpoint(ready.peer);
        if (direct == _pair.relayEndpoint) {
            ++_stats.invalidRelayControl;
            return UdpInbound::Consumed;
        }
        if (_ready) {
            if (!(direct == _directPeer))
                ++_stats.invalidRelayControl;
            return UdpInbound::Consumed;
        }

        _directPeer = direct;
        _ready = true;
        _readyAtMs = nowMs;
        ++_stats.readyAccepted;
        return UdpInbound::Ready;
    }

    if (_ready && length == 1 && bytes[0] == kDirectPunchByte &&
        source == _directPeer) {
        ++_stats.directPunchesReceived;
        if (!_directObserved) {
            _directObserved = true;
            return UdpInbound::DirectPeerObserved;
        }
        return UdpInbound::Consumed;
    }

    return UdpInbound::NotHandled;
}

}
