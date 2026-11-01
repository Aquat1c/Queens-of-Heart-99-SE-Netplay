#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "../RelayService.h"
#include "../RelayServiceVerb.h"

#include "../../net/Protocol.h"
#include "../../netplay/WireProtocol.h"
#include "../../netplay/tests/TestHarness.h"
#include "../../spectator/SpectatorProtocol.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace qrelay;
using qnet::relay::DecodeError;
using qnet::relay::Hello;
using qnet::relay::Id128;
using qnet::relay::LaneDomain;
using qnet::relay::Register;
using qnet::relay::RegistrationToken;
using qnet::relay::RelayRole;
using qnet::relay::RoomId;
using qnet::relay::SpectatorHello;
using qnet::relay::SpectatorAdmission;
using qnet::relay::SpectatorParty;
using qnet::relay::SpectatorRouteKind;
using qnet::relay::SpectatorStatus;
using qnet::relay::SpectatorWelcome;
using qnet::relay::Welcome;
using qnet::relay::WelcomeStatus;

namespace {

qnetplay::compat::CompatibilityIdentityWire playerFixtureIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    (void)qnetplay::compat::setProfileId(identity, "fixture-profile");
    for (std::size_t index = 0u;
         index < qnetplay::compat::kCompatibilitySha256Bytes; ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<std::uint8_t>(0x12u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<std::uint8_t>(0x42u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<std::uint8_t>(0x82u + index);
    }
    identity.runtimeBuild.executableSize = 1u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.simulationContent.entryCount = 1u;
    identity.simulationContent.requiredEntryCount = 1u;
    identity.stateSchemaId = 1u;
    identity.modSimulationId = 1u;
    identity.timingProfileId = 1u;
    return identity;
}

uint64_t g_monotonicMs = 1'000'000;
uint64_t g_wallUnix = 1'700'000'000ull;

uint64_t testMonotonic() { return g_monotonicMs; }
uint64_t testWall() { return g_wallUnix; }

void resetClocks() {
    g_monotonicMs = 1'000'000;
    g_wallUnix = 1'700'000'000ull;
}

struct LogCapture {
    std::vector<std::string> lines;

    static void sink(void* const user, LogLevel, const char* const line) {
        static_cast<LogCapture*>(user)->lines.emplace_back(line);
    }

    bool contains(const std::string& needle) const {
        for (const std::string& line : lines) {
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    }
};

std::string hex64(const uint64_t value, const bool upper) {
    char text[32] = {};
    std::snprintf(text, sizeof(text), upper ? "%016llX" : "%016llx",
                  static_cast<unsigned long long>(value));
    return text;
}

bool logsLeak(const LogCapture& capture, const uint64_t secret) {
    if (secret == 0) return false;
    return capture.contains(hex64(secret, true))
        || capture.contains(hex64(secret, false));
}

uint64_t mix(uint64_t value, const uint64_t input) {
    value ^= input + 0x9E3779B97F4A7C15ull + (value << 6) + (value >> 2);
    return value;
}

uint64_t expectedMacLow(const SpectatorGrantRequest& request) {
    uint64_t value = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < request.keyLength; ++i)
        value = mix(value, request.key[i]);
    value = mix(value, request.grantId);
    value = mix(value, request.viewerId);
    value = mix(value, request.sessionId);
    value = mix(value, request.expiryUnix);
    value = mix(value, request.sourceGeneration);
    value = mix(value, request.resumeRecord);
    value = mix(value, request.resumeDigest);
    value = mix(value, request.route);
    return value | 1ull;
}

class TestVerifier final : public SpectatorGrantVerifier {
public:
    GrantVerdict verify(const SpectatorGrantRequest& request) const override {
        if (request.key == nullptr
            || request.keyLength != qnet::relay::kGrantKeyBytes) {
            return GrantVerdict::Malformed;
        }
        return request.macLow == expectedMacLow(request)
            ? GrantVerdict::Accepted
            : GrantVerdict::BadMac;
    }
};

uint16_t g_nextPort = 24601;

sockaddr_in loopback(const uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
    return address;
}

RelayServiceConfig testConfig() {
    RelayServiceConfig config{};
    config.bindAddress = "127.0.0.1";
    config.logToConsole = false;
    config.metricsIntervalMs = 0;
    config.maximumConnectionsPerIp = 64;
    config.maximumViewersPerIp = 32;
    config.connectionsPerIpPerSecond = 4096;
    return config;
}

bool startTestService(RelayService& service,
                      RelayServiceConfig config,
                      std::string& error) {
    service.setMonotonicClock(&testMonotonic);
    service.setWallClock(&testWall);
    for (int attempt = 0; attempt < 128; ++attempt) {
        config.port = g_nextPort++;
        if (g_nextPort > 25400) g_nextPort = 24601;
        if (!service.configure(config, error)) return false;
        if (service.start(error)) return true;
    }
    return false;
}

bool pumpUntilConnections(RelayService& service,
                          const uint32_t expected,
                          const int turns = 32) {
    for (int turn = 0; turn < turns; ++turn) {
        if (service.snapshot().connections == expected) return true;
        if (!service.pumpOnce(0)) return false;
    }
    return service.snapshot().connections == expected;
}

class TcpClient {
public:
    ~TcpClient() { close(); }

    bool open(const uint16_t port) {
        return openFrom(port, nullptr);
    }

    bool openFrom(const uint16_t port, const char* const localIpv4) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) return false;
        if (localIpv4 != nullptr) {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = 0;
            if (InetPtonA(AF_INET, localIpv4, &local.sin_addr) != 1
                || ::bind(socket_,
                          reinterpret_cast<const sockaddr*>(&local),
                          sizeof(local)) != 0) {
                close();
                return false;
            }
        }
        const sockaddr_in address = loopback(port);
        if (::connect(socket_,
                      reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) != 0) {
            close();
            return false;
        }
        u_long nonBlocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonBlocking);
        return true;
    }

    void close() {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    bool valid() const { return socket_ != INVALID_SOCKET; }

    bool send(const std::vector<uint8_t>& bytes) {
        if (socket_ == INVALID_SOCKET) return false;
        size_t offset = 0;
        for (int attempt = 0; attempt < 256 && offset < bytes.size();
             ++attempt) {
            const int sent = ::send(
                socket_,
                reinterpret_cast<const char*>(bytes.data() + offset),
                static_cast<int>(bytes.size() - offset),
                0);
            if (sent > 0) {
                offset += static_cast<size_t>(sent);
                continue;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        }
        return offset == bytes.size();
    }

    bool receiveRecord(RelayService& service,
                       std::vector<uint8_t>& body,
                       const int turns = 64) {
        for (int turn = 0; turn < turns; ++turn) {
            if (tryParse(body)) return true;
            service.pumpOnce(0);
            drain();
        }
        return tryParse(body);
    }

    bool closedByPeer(RelayService& service, const int turns = 64) {
        for (int turn = 0; turn < turns; ++turn) {
            char probe[512] = {};
            const int received = ::recv(socket_, probe, sizeof(probe), 0);
            if (received == 0) return true;
            if (received > 0) {
                buffer_.insert(buffer_.end(), probe, probe + received);
            } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                return true;
            }
            service.pumpOnce(0);
        }
        return false;
    }

private:
    void drain() {
        for (;;) {
            char probe[4096] = {};
            const int received = ::recv(socket_, probe, sizeof(probe), 0);
            if (received <= 0) return;
            buffer_.insert(buffer_.end(), probe, probe + received);
        }
    }

    bool tryParse(std::vector<uint8_t>& body) {
        drain();
        if (buffer_.size() < 2u) return false;
        const size_t length = static_cast<size_t>(buffer_[0])
            | (static_cast<size_t>(buffer_[1]) << 8u);
        if (buffer_.size() < length + 2u) return false;
        uint8_t type = qnet::MSG_NONE;
        const uint8_t* payload = nullptr;
        size_t payloadLength = 0;
        const bool ok = qnet::unpackMsg(
            buffer_.data() + 2u, length, type, payload, payloadLength);
        if (ok) body.assign(payload, payload + payloadLength);
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(length + 2u));
        lastType_ = type;
        return ok;
    }

public:
    uint8_t lastType() const { return lastType_; }

private:
    uint8_t lastType_ = qnet::MSG_NONE;

    SOCKET socket_ = INVALID_SOCKET;
    std::vector<uint8_t> buffer_;
};

class UdpClient {
public:
    ~UdpClient() {
        if (socket_ != INVALID_SOCKET) closesocket(socket_);
    }

    bool open() {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == INVALID_SOCKET) return false;
        const sockaddr_in address = loopback(0);
        if (::bind(socket_,
                   reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) != 0) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
            return false;
        }
        u_long nonBlocking = 1;
        ioctlsocket(socket_, FIONBIO, &nonBlocking);
        return true;
    }

    uint16_t port() const {
        sockaddr_in address{};
        int length = sizeof(address);
        if (getsockname(socket_,
                        reinterpret_cast<sockaddr*>(&address),
                        &length) != 0) {
            return 0;
        }
        return ntohs(address.sin_port);
    }

    bool sendTo(const uint16_t relayPort, const std::vector<uint8_t>& bytes) {
        const sockaddr_in address = loopback(relayPort);
        return ::sendto(
            socket_,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()),
            0,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == static_cast<int>(bytes.size());
    }

    bool receive(RelayService& service,
                 std::vector<uint8_t>& out,
                 const int turns = 64) {
        for (int turn = 0; turn < turns; ++turn) {
            std::array<uint8_t, 2048> bytes{};
            sockaddr_in source{};
            int length = sizeof(source);
            const int received = ::recvfrom(
                socket_,
                reinterpret_cast<char*>(bytes.data()),
                static_cast<int>(bytes.size()),
                0,
                reinterpret_cast<sockaddr*>(&source),
                &length);
            if (received > 0) {
                out.assign(bytes.begin(), bytes.begin() + received);
                return true;
            }
            service.pumpOnce(0);
        }
        return false;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
};

std::vector<uint8_t> frameRecord(const std::vector<uint8_t>& record) {
    std::array<uint8_t, qnet::kMaxMsgSize> message{};
    const size_t length = qnet::packMsg(
        qnet::relay::kTcpEnvelopeType,
        record.data(),
        record.size(),
        message.data(),
        message.size());
    std::vector<uint8_t> frame(length + 2u, 0u);
    frame[0] = static_cast<uint8_t>(length);
    frame[1] = static_cast<uint8_t>(length >> 8u);
    std::memcpy(frame.data() + 2u, message.data(), length);
    return frame;
}

std::vector<uint8_t> playerHelloFrame(const RoomId& room,
                                      const LaneDomain domain,
                                      const RelayRole role) {
    Hello hello{};
    hello.room = room;
    hello.domain = domain;
    hello.role = role;
    std::vector<uint8_t> encoded;
    qnet::relay::encodeHello(hello, encoded);
    return frameRecord(encoded);
}

RoomId testRoom(const uint64_t index) {
    return RoomId{ 0x0123456789ABCDEFull + index, 0xFEDCBA9876543210ull };
}

struct PairedRoom {
    TcpClient host;
    TcpClient client;
    Welcome hostWelcome{};
    Welcome clientWelcome{};
    bool ok = false;
};

bool pairRoom(RelayService& service,
              const uint16_t port,
              const RoomId& room,
              PairedRoom& out) {
    std::vector<uint8_t> body;
    if (!out.host.open(port)) return false;
    if (!out.host.send(playerHelloFrame(room, LaneDomain::Player,
                                        RelayRole::Host))) {
        return false;
    }
    if (!out.host.receiveRecord(service, body)) return false;
    Welcome waiting{};
    if (qnet::relay::decodeWelcome(body.data(), body.size(), waiting)
            != DecodeError::None
        || waiting.status != WelcomeStatus::Waiting) {
        return false;
    }

    if (!out.client.open(port)) return false;
    if (!out.client.send(playerHelloFrame(room, LaneDomain::Player,
                                          RelayRole::Client))) {
        return false;
    }
    if (!out.client.receiveRecord(service, body)) return false;
    if (qnet::relay::decodeWelcome(body.data(), body.size(),
                                   out.clientWelcome) != DecodeError::None
        || out.clientWelcome.status != WelcomeStatus::Accepted) {
        return false;
    }
    if (!out.host.receiveRecord(service, body)) return false;
    if (qnet::relay::decodeWelcome(body.data(), body.size(),
                                   out.hostWelcome) != DecodeError::None
        || out.hostWelcome.status != WelcomeStatus::Accepted) {
        return false;
    }
    out.ok = true;
    return true;
}

std::vector<uint8_t> registerRecord(const RoomId& room,
                                    const RelayRole role,
                                    const RegistrationToken& token) {
    Register registration{};
    registration.room = room;
    registration.domain = LaneDomain::Player;
    registration.role = role;
    registration.token = token;
    std::vector<uint8_t> encoded;
    qnet::relay::encodeRegister(registration, encoded);
    return encoded;
}

std::vector<uint8_t> playerHelloDatagram(
    const uint64_t sessionId,
    const qnetplay::wire::SessionCookie& cookie) {
    qnetplay::wire::HelloPayload hello{};
    hello.compatibilityId = 0x5153504C41594552ull;
    hello.role = qnetplay::wire::PeerRole::Client;
    hello.player = 1;
    hello.compatibilityIdentity = playerFixtureIdentity();
    std::vector<uint8_t> payload;
    std::vector<uint8_t> encoded;
    if (!qnetplay::wire::encodePayload(hello, payload)
        || !qnetplay::wire::encodeDatagram(
            qnetplay::wire::MessageType::Hello,
            sessionId,
            cookie,
            1,
            payload,
            encoded)) {
        return {};
    }
    return encoded;
}

std::vector<uint8_t> viewerHelloFrame(
    const RoomId& room,
    const std::vector<uint8_t>& grantKey,
    const uint64_t grantId,
    const uint64_t viewerId,
    const uint64_t sessionId,
    const uint64_t expiryUnix,
    const uint32_t generation,
    const bool corruptMac = false,
    const uint64_t macViewerId = 0) {
    SpectatorHello hello{};
    hello.room = room;
    hello.party = SpectatorParty::Viewer;
    hello.route = SpectatorRouteKind::Relay;
    hello.sessionId = sessionId;
    hello.grantId = grantId;
    hello.viewerId = viewerId;
    hello.expiryUnix = expiryUnix;
    hello.sourceGeneration = generation;
    hello.resumeRecord = 0;
    hello.resumeDigest = 0;

    SpectatorGrantRequest request{};
    request.key = grantKey.data();
    request.keyLength = grantKey.size();
    request.grantId = grantId;
    request.viewerId = macViewerId != 0 ? macViewerId : viewerId;
    request.sessionId = sessionId;
    request.expiryUnix = expiryUnix;
    request.sourceGeneration = generation;
    request.resumeRecord = 0;
    request.resumeDigest = 0;
    request.route = static_cast<uint8_t>(SpectatorRouteKind::Relay);
    hello.macLow = expectedMacLow(request);
    if (corruptMac) hello.macLow ^= 0x40ull;
    hello.macHigh = 0x1234;

    std::vector<uint8_t> encoded;
    qnet::relay::encodeSpectatorHello(hello, encoded);
    return frameRecord(encoded);
}

bool registerSource(RelayService& service,
                    const uint16_t port,
                    const RoomId& room,
                    const RegistrationToken& spectatorToken,
                    const uint64_t sessionId,
                    TcpClient& source,
                    SpectatorWelcome& welcome) {
    SpectatorHello hello{};
    hello.room = room;
    hello.party = SpectatorParty::Source;
    hello.admission = SpectatorAdmission::None;
    hello.sessionId = sessionId;
    hello.sourceToken = spectatorToken;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeSpectatorHello(hello, encoded)) return false;
    if (!source.open(port)) return false;
    if (!source.send(frameRecord(encoded))) return false;
    std::vector<uint8_t> body;
    if (!source.receiveRecord(service, body)) return false;
    return qnet::relay::decodeSpectatorWelcome(
               body.data(), body.size(), welcome) == DecodeError::None;
}

bool admitViewer(RelayService& service,
                 const uint16_t port,
                 const RoomId& room,
                 const SpectatorWelcome& sourceWelcome,
                 const uint64_t grantId,
                 const uint64_t viewerId,
                 const uint64_t sessionId,
                 TcpClient& viewer,
                 SpectatorWelcome& welcome,
                 const bool corruptMac = false,
                 const uint64_t macViewerId = 0,
                 const uint64_t expiryOverride = 0,
                 const uint64_t sessionOverride = 0) {
    const uint64_t expiry = expiryOverride != 0
        ? expiryOverride
        : g_wallUnix + 60ull;
    if (!viewer.open(port)) return false;
    if (!viewer.send(viewerHelloFrame(
            room,
            sourceWelcome.grantKey,
            grantId,
            viewerId,
            sessionOverride != 0 ? sessionOverride : sessionId,
            expiry,
            sourceWelcome.sourceGeneration,
            corruptMac,
            macViewerId))) {
        return false;
    }
    std::vector<uint8_t> body;
    if (!viewer.receiveRecord(service, body)) return false;
    return qnet::relay::decodeSpectatorWelcome(
               body.data(), body.size(), welcome) == DecodeError::None;
}

bool admitPublicViewer(
    RelayService& service,
    const uint16_t port,
    const RoomId& room,
    const RegistrationToken& viewerAccessToken,
    const uint64_t sessionId,
    TcpClient& viewer,
    SpectatorWelcome& welcome,
    const uint32_t resumeRecord = 0,
    const uint64_t resumeDigest = 0) {
    SpectatorHello hello{};
    hello.room = room;
    hello.party = SpectatorParty::Viewer;
    hello.route = SpectatorRouteKind::Relay;
    hello.admission = SpectatorAdmission::PublicToken;
    hello.sourceToken = viewerAccessToken;
    hello.sessionId = sessionId;
    hello.resumeRecord = resumeRecord;
    hello.resumeDigest = resumeDigest;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeSpectatorHello(hello, encoded)) return false;
    if (!viewer.open(port)) return false;
    if (!viewer.send(frameRecord(encoded))) return false;
    std::vector<uint8_t> body;
    if (!viewer.receiveRecord(service, body)) return false;
    return qnet::relay::decodeSpectatorWelcome(
               body.data(), body.size(), welcome) == DecodeError::None;
}

namespace qsp = qnetplay::spectator;

qsp::SessionIdentity spectatorIdentity(const uint64_t sessionId) {
    qsp::SessionIdentity identity{};
    identity.sessionId = sessionId;
    identity.epoch = 1;
    identity.cookie.low = 0xA5A5A5A5A5A5A5A5ull;
    identity.cookie.high = 0x5A5A5A5A5A5A5A5Aull;
    return identity;
}

std::vector<uint8_t> sourceHeartbeat(const uint64_t sessionId,
                                     const uint32_t sequence) {
    qsp::HeartbeatPayload heartbeat{};
    heartbeat.gameId = 0x900D;
    heartbeat.digest = 0xD16E57ull;
    heartbeat.nextRecord = 5;
    heartbeat.checkpointRecord = 0;
    heartbeat.sourcePhase = qsp::SourcePhase::Active;
    heartbeat.viewerCount = 1;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> encoded;
    if (!qsp::encodePayload(heartbeat, payload)
        || !qsp::encodeDatagram(qsp::MessageType::Heartbeat,
                                spectatorIdentity(sessionId),
                                sequence, payload, encoded)) {
        return {};
    }
    return encoded;
}

std::vector<uint8_t> viewerAck(const uint64_t sessionId,
                               const uint64_t viewerId,
                               const uint32_t generation,
                               const uint32_t sequence) {
    qsp::AckPayload ack{};
    ack.gameId = 0x900D;
    ack.digest = 0xD16E57ull;
    ack.viewerId = viewerId;
    ack.nextRecord = 5;
    ack.generation = generation;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> encoded;
    if (!qsp::encodePayload(ack, payload)
        || !qsp::encodeDatagram(qsp::MessageType::Ack,
                                spectatorIdentity(sessionId),
                                sequence, payload, encoded)) {
        return {};
    }
    return encoded;
}

std::vector<uint8_t> spectatorFrame(const SpectatorParty party,
                                    const uint64_t viewerId,
                                    const uint32_t generation,
                                    const std::vector<uint8_t>& inner) {
    qnet::relay::SpectatorFrameHeader header{};
    header.party = party;
    header.viewerId = viewerId;
    header.sourceGeneration = generation;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeSpectatorFrameHeader(header, encoded)) return {};
    encoded.insert(encoded.end(), inner.begin(), inner.end());
    return encoded;
}

bool receiveForwardedFrame(RelayService& service,
                           UdpClient& udp,
                           std::vector<uint8_t>& out,
                           const int attempts = 16) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::vector<uint8_t> datagram;
        if (!udp.receive(service, datagram, 8)) continue;
        if (qnet::relay::hasSpectatorFrameKind(datagram.data(),
                                               datagram.size())) {
            out = datagram;
            return true;
        }
    }
    return false;
}

bool bindSpectatorUdp(RelayService& service,
                      const uint16_t port,
                      const RoomId& room,
                      const SpectatorParty party,
                      const uint32_t generation,
                      const RegistrationToken& token,
                      const uint64_t viewerId,
                      UdpClient& udp) {
    qnet::relay::SpectatorRegister registration{};
    registration.room = room;
    registration.party = party;
    registration.sourceGeneration = generation;
    registration.token = token;
    registration.viewerId = viewerId;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeSpectatorRegister(registration, encoded))
        return false;
    if (!udp.sendTo(port, encoded)) return false;
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    return true;
}

}

#include "../../spectator/ResilientSpectatorProtocol.h"
#include "../../spectator/TopologyGrant.h"

namespace {

uint64_t expectedTopologyMacLow(const TopologyGrantRequest& request) {
    uint64_t value = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < request.keyLength; ++i)
        value = mix(value, request.key[i]);
    value = mix(value, request.grantId);
    value = mix(value, request.nodeId);
    value = mix(value, request.sessionId);
    value = mix(value, request.expiryUnix);
    value = mix(value, request.authorityGeneration);
    return value | 1ull;
}

class TestTopologyVerifier final : public TopologyGrantVerifier {
public:
    GrantVerdict verify(const TopologyGrantRequest& request) const override {
        if (request.key == nullptr
            || request.keyLength != qnet::relay::kGrantKeyBytes) {
            return GrantVerdict::Malformed;
        }
        return request.macLow == expectedTopologyMacLow(request)
            ? GrantVerdict::Accepted
            : GrantVerdict::BadMac;
    }
};

std::vector<uint8_t> qtaEnvelope(const uint64_t envelopeNodeId) {
    namespace rwire = qnetplay::spectator::resilient::wire;
    namespace rmodel = qnetplay::spectator::resilient;
    rwire::AuthorityState state{};
    if (!rmodel::makeStreamCompatibilityIdentity(
            playerFixtureIdentity(), 0x5151515151515151ull, 3u,
            0x4646464646464646ull, state.stream)) {
        return {};
    }
    state.authorityGeneration = 7u;
    state.rootNodeId = 1u;
    state.childNodeId = 2u;
    state.viewerId = 0x544F504F56494557ull;
    state.viewerGeneration = 1u;
    state.qspRouteGeneration = 1u;
    state.qsrIpv4BE = 0x0100007Fu;
    state.qsrPort = 40000u;
    state.qsrBindingGeneration = 1u;
    state.sequence = 9u;
    state.authorityExpiryMs = 60000u;
    state.eventTimeMs = 1234u;
    state.kind = rwire::AuthorityStateKind::Liveness;
    state.reason = rwire::AuthorityStateReason::None;
    state.livenessWindowMs = 1000u;
    rwire::ControlDatagram datagram{};
    datagram.type = rwire::ControlMessageType::AuthorityState;
    datagram.authorityState = state;
    std::vector<uint8_t> inner;
    if (!rwire::encodeControlDatagram(datagram, inner)) return {};

    std::vector<uint8_t> out(
        rwire::kTopologyQtaEnvelopePrefixBytes + inner.size(), 0u);
    const auto writeLe = [&out](const size_t offset, const uint64_t value,
                                const unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i)
            out[offset + i] = static_cast<uint8_t>(value >> (i * 8u));
    };
    writeLe(0u, rwire::kTopologyQtaEnvelopeMagic, 4u);
    writeLe(4u, rwire::kTopologyQtaEnvelopeVersion, 2u);
    out[6] = rwire::kTopologyQtaEnvelopeControlType;
    writeLe(8u, inner.size(), 2u);
    writeLe(12u, 0x0101010101010101ull, 8u);
    writeLe(20u, envelopeNodeId, 8u);
    writeLe(28u, 0x0303030303030303ull, 8u);
    for (size_t index = 0; index < rwire::kTopologyQtaEnvelopeMacBytes;
         ++index) {
        out[36u + index] = static_cast<uint8_t>(0x90u + index);
    }
    std::copy(inner.begin(), inner.end(),
              out.begin() + rwire::kTopologyQtaEnvelopePrefixBytes);
    return out;
}

std::vector<uint8_t> topologyFrame(const qnet::relay::TopologyParty party,
                                   const uint64_t nodeId,
                                   const uint32_t generation,
                                   const std::vector<uint8_t>& inner) {
    qnet::relay::TopologyFrameHeader header{};
    header.party = party;
    header.nodeId = nodeId;
    header.authorityGeneration = generation;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeTopologyFrameHeader(header, encoded)) return {};
    encoded.insert(encoded.end(), inner.begin(), inner.end());
    return encoded;
}

bool receiveForwardedTopologyFrame(RelayService& service,
                                   UdpClient& udp,
                                   std::vector<uint8_t>& out,
                                   const int attempts = 16) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::vector<uint8_t> datagram;
        if (!udp.receive(service, datagram, 8)) continue;
        if (qnet::relay::hasTopologyFrameKind(datagram.data(),
                                              datagram.size())) {
            out = datagram;
            return true;
        }
    }
    return false;
}

bool registerTopologyAuthority(RelayService& service,
                               const uint16_t port,
                               const RoomId& room,
                               const RegistrationToken& hostCapability,
                               const uint64_t sessionId,
                               TcpClient& authority,
                               qnet::relay::TopologyWelcome& welcome) {
    qnet::relay::TopologyHello hello{};
    hello.room = room;
    hello.party = qnet::relay::TopologyParty::Authority;
    hello.admission = qnet::relay::TopologyAdmission::None;
    hello.sessionId = sessionId;
    hello.authorityToken = hostCapability;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeTopologyHello(hello, encoded)) return false;
    if (!authority.open(port)) return false;
    if (!authority.send(frameRecord(encoded))) return false;
    std::vector<uint8_t> body;
    if (!authority.receiveRecord(service, body)) return false;
    return qnet::relay::decodeTopologyWelcome(
               body.data(), body.size(), welcome)
        == qnet::relay::DecodeError::None;
}

bool admitTopologyNode(RelayService& service,
                       const uint16_t port,
                       const RoomId& room,
                       const qnet::relay::TopologyWelcome& authorityWelcome,
                       const uint64_t grantId,
                       const uint64_t nodeId,
                       const uint64_t sessionId,
                       TcpClient& node,
                       qnet::relay::TopologyWelcome& welcome,
                       const bool corruptMac = false,
                       const uint64_t expiryOverride = 0,
                       const uint32_t generationOverride = 0,
                       const uint64_t sessionOverride = 0,
                       const bool realMac = false) {
    qnet::relay::TopologyHello hello{};
    hello.room = room;
    hello.party = qnet::relay::TopologyParty::Node;
    hello.admission = qnet::relay::TopologyAdmission::Grant;
    hello.sessionId = sessionOverride != 0 ? sessionOverride : sessionId;
    hello.grantId = grantId;
    hello.nodeId = nodeId;
    hello.expiryUnix =
        expiryOverride != 0 ? expiryOverride : g_wallUnix + 60ull;
    hello.authorityGeneration = generationOverride != 0
        ? generationOverride
        : authorityWelcome.authorityGeneration;

    if (realMac) {
        qnetplay::spectator::TopologyGrant grant{};
        grant.grantId = hello.grantId;
        grant.nodeId = hello.nodeId;
        grant.sessionId = hello.sessionId;
        grant.expiryUnix = hello.expiryUnix;
        grant.authorityGeneration = hello.authorityGeneration;
        qnetplay::spectator::TopologyGrantKey key{};
        if (authorityWelcome.grantKey.size() != key.size()) return false;
        std::memcpy(key.data(), authorityWelcome.grantKey.data(),
                    key.size());
        if (!qnetplay::spectator::issueTopologyGrant(key, grant))
            return false;
        hello.macLow = grant.macLow;
        hello.macHigh = grant.macHigh;
    } else {
        TopologyGrantRequest request{};
        request.key = authorityWelcome.grantKey.data();
        request.keyLength = authorityWelcome.grantKey.size();
        request.grantId = hello.grantId;
        request.nodeId = hello.nodeId;
        request.sessionId = hello.sessionId;
        request.expiryUnix = hello.expiryUnix;
        request.authorityGeneration = hello.authorityGeneration;
        hello.macLow = expectedTopologyMacLow(request);
        hello.macHigh = 0x5678u;
    }
    if (corruptMac) hello.macLow ^= 0x40ull;

    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeTopologyHello(hello, encoded)) return false;
    if (!node.open(port)) return false;
    if (!node.send(frameRecord(encoded))) return false;
    std::vector<uint8_t> body;
    if (!node.receiveRecord(service, body)) return false;
    return qnet::relay::decodeTopologyWelcome(
               body.data(), body.size(), welcome)
        == qnet::relay::DecodeError::None;
}

bool bindTopologyUdp(RelayService& service,
                     const uint16_t port,
                     const RoomId& room,
                     const qnet::relay::TopologyParty party,
                     const uint32_t generation,
                     const RegistrationToken& token,
                     const uint64_t nodeId,
                     UdpClient& udp) {
    qnet::relay::TopologyRegister registration{};
    registration.room = room;
    registration.party = party;
    registration.authorityGeneration = generation;
    registration.token = token;
    registration.nodeId = nodeId;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeTopologyRegister(registration, encoded))
        return false;
    if (!udp.sendTo(port, encoded)) return false;
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    return true;
}

}

QNP_TEST(relay_verb_parses_the_documented_option_syntax) {
    char argument0[] = "relay";
    char bindFlag[] = "--bind";
    char bindValue[] = "127.0.0.1";
    char portFlag[] = "--port";
    char portValue[] = "14999";
    char* argv[] = { argument0, bindFlag, bindValue, portFlag, portValue };

    RelayServiceConfig config{};
    bool showHelp = true;
    std::string error;
    QNP_REQUIRE(parseRelayOptions(5, argv, config, showHelp, error));
    QNP_CHECK(!showHelp);
    QNP_CHECK(error.empty());
    QNP_CHECK(config.bindAddress == "127.0.0.1");
    QNP_CHECK(config.port == 14999);
    QNP_CHECK(config.spectatorLaneEnabled);

    char equalsBind[] = "--bind=10.0.0.7";
    char spectator[] = "--spectator";
    char viewers[] = "--max-viewers=64";
    char spectatorRegistration[] =
        "--spectator-registration-timeout-ms=12000";
    char roomKib[] = "--room-kib";
    char roomKibValue[] = "128";
    char* argv2[] = {
        argument0, equalsBind, spectator, viewers, spectatorRegistration,
        roomKib, roomKibValue,
    };
    QNP_REQUIRE(parseRelayOptions(7, argv2, config, showHelp, error));
    QNP_CHECK(config.bindAddress == "10.0.0.7");
    QNP_CHECK(config.spectatorLaneEnabled);
    QNP_CHECK(config.maximumViewers == 64);
    QNP_CHECK(config.spectatorRegistrationTimeoutMs == 12000);
    QNP_CHECK(config.roomUdpBytesPerSecond == 128ull * 1024ull);

    char help[] = "--help";
    char* argv3[] = { argument0, help };
    showHelp = false;
    QNP_CHECK(parseRelayOptions(2, argv3, config, showHelp, error));
    QNP_CHECK(showHelp);
}

QNP_TEST(relay_verb_rejects_invalid_missing_and_duplicate_options) {
    char argument0[] = "relay";
    RelayServiceConfig config{};
    bool showHelp = false;
    std::string error;

    char portFlag[] = "--port";
    char* missing[] = { argument0, portFlag };
    QNP_CHECK(!parseRelayOptions(2, missing, config, showHelp, error));
    QNP_CHECK(!error.empty());

    char zero[] = "0";
    char* outOfRangeLow[] = { argument0, portFlag, zero };
    QNP_CHECK(!parseRelayOptions(3, outOfRangeLow, config, showHelp, error));

    char tooBig[] = "70000";
    char* outOfRangeHigh[] = { argument0, portFlag, tooBig };
    QNP_CHECK(!parseRelayOptions(3, outOfRangeHigh, config, showHelp, error));

    char notANumber[] = "1234x";
    char* malformed[] = { argument0, portFlag, notANumber };
    QNP_CHECK(!parseRelayOptions(3, malformed, config, showHelp, error));

    char valid[] = "14999";
    char* duplicate[] = { argument0, portFlag, valid, portFlag, valid };
    QNP_CHECK(!parseRelayOptions(5, duplicate, config, showHelp, error));
    QNP_CHECK(error.find("duplicate") != std::string::npos);

    char unknown[] = "--not-an-option";
    char* strange[] = { argument0, unknown };
    QNP_CHECK(!parseRelayOptions(2, strange, config, showHelp, error));
    QNP_CHECK(error.find("unknown argument") != std::string::npos);

    char spectator[] = "--spectator";
    char noSpectator[] = "--no-spectator";
    char* conflicting[] = { argument0, spectator, noSpectator };
    QNP_CHECK(!parseRelayOptions(3, conflicting, config, showHelp, error));
}

QNP_TEST(relay_service_exposes_stable_documented_exit_codes) {
    QNP_CHECK(static_cast<int>(ExitCode::Ok) == 0);
    QNP_CHECK(static_cast<int>(ExitCode::Usage) == 2);
    QNP_CHECK(static_cast<int>(ExitCode::BindFailure) == 3);
    QNP_CHECK(static_cast<int>(ExitCode::ConfigInvalid) == 4);
    QNP_CHECK(static_cast<int>(ExitCode::FatalRuntime) == 5);
    QNP_CHECK(std::string(exitCodeText(ExitCode::BindFailure))
              == "bind failure");

    char argument0[] = "relay";
    char help[] = "--help";
    char* helpArgv[] = { argument0, help };
    QNP_CHECK(runRelayVerb(2, helpArgv)
              == static_cast<int>(ExitCode::Ok));

    char unknown[] = "--nope";
    char* usageArgv[] = { argument0, unknown };
    QNP_CHECK(runRelayVerb(2, usageArgv)
              == static_cast<int>(ExitCode::Usage));

    char maxRooms[] = "--max-rooms";
    char zero[] = "0";
    char* invalidArgv[] = { argument0, maxRooms, zero };
    QNP_CHECK(runRelayVerb(3, invalidArgv)
              == static_cast<int>(ExitCode::ConfigInvalid));

    char bind[] = "--bind";
    char unreachable[] = "203.0.113.7";
    char port[] = "--port";
    char portValue[] = "24599";
    char* bindArgv[] = { argument0, bind, unreachable, port, portValue };
    QNP_CHECK(runRelayVerb(5, bindArgv)
              == static_cast<int>(ExitCode::BindFailure));

    RelayService unstarted;
    QNP_CHECK(unstarted.run() == ExitCode::FatalRuntime);

    std::string error;
    RelayServiceConfig config = testConfig();
    config.maximumViewersPerRoom = config.maximumViewers + 1u;
    RelayService service;
    QNP_CHECK(!service.configure(config, error));
    QNP_CHECK(!error.empty());
}

QNP_TEST(relay_service_serves_several_player_rooms_concurrently) {
    resetClocks();
    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, testConfig(), error));
    const uint16_t port = service.boundPort();

    PairedRoom rooms[3];
    for (int index = 0; index < 3; ++index) {
        QNP_REQUIRE(pairRoom(service, port, testRoom(
            static_cast<uint64_t>(index)), rooms[index]));
    }
    QNP_CHECK(service.snapshot().rooms == 3);
    QNP_CHECK(service.snapshot().pairedRooms == 3);

    for (int index = 0; index < 3; ++index) {
        std::vector<uint8_t> payload{
            static_cast<uint8_t>(index), 0x11, 0x22, 0x33,
        };
        std::array<uint8_t, qnet::kMaxMsgSize> message{};
        const size_t length = qnet::packMsg(
            0x40, payload.data(), payload.size(),
            message.data(), message.size());
        std::vector<uint8_t> frame(length + 2u, 0u);
        frame[0] = static_cast<uint8_t>(length);
        frame[1] = static_cast<uint8_t>(length >> 8u);
        std::memcpy(frame.data() + 2u, message.data(), length);
        QNP_CHECK(rooms[index].host.send(frame));
    }
    for (int index = 0; index < 3; ++index) {
        std::vector<uint8_t> body;
        QNP_REQUIRE(rooms[index].client.receiveRecord(service, body));
        QNP_CHECK(rooms[index].client.lastType() == 0x40);
        QNP_REQUIRE(body.size() == 4u);
        QNP_CHECK(body[0] == static_cast<uint8_t>(index));
    }

    QNP_CHECK(rooms[0].hostWelcome.registrationToken
              != rooms[1].hostWelcome.registrationToken);
    QNP_CHECK(rooms[0].hostWelcome.spectatorRegistrationToken
              != rooms[0].hostWelcome.registrationToken);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_service_isolates_a_flooding_room_from_healthy_rooms) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.maximumUdpPacketsPerRoomPerTurn = 4;
    config.maximumUdpPacketsPerTurn = 512;
    config.maximumUnboundPacketsPerTurn = 8;
    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    PairedRoom hostile;
    QNP_REQUIRE(pairRoom(service, port, testRoom(100), hostile));

    UdpClient hostileHost;
    UdpClient hostileClient;
    QNP_REQUIRE(hostileHost.open());
    QNP_REQUIRE(hostileClient.open());
    QNP_REQUIRE(hostileHost.sendTo(port, registerRecord(
        testRoom(100), RelayRole::Host,
        hostile.hostWelcome.registrationToken)));
    QNP_REQUIRE(hostileClient.sendTo(port, registerRecord(
        testRoom(100), RelayRole::Client,
        hostile.clientWelcome.registrationToken)));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);

    std::vector<uint8_t> junk(600, 0xAB);
    for (int burst = 0; burst < 400; ++burst)
        hostileHost.sendTo(port, junk);
    UdpClient stranger;
    QNP_REQUIRE(stranger.open());
    for (int burst = 0; burst < 200; ++burst)
        stranger.sendTo(port, junk);

    PairedRoom healthy;
    std::vector<uint8_t> body;
    QNP_REQUIRE(healthy.host.open(port));
    QNP_REQUIRE(healthy.host.send(playerHelloFrame(
        testRoom(101), LaneDomain::Player, RelayRole::Host)));
    QNP_REQUIRE(healthy.host.receiveRecord(service, body, 12));
    Welcome waiting{};
    QNP_CHECK(qnet::relay::decodeWelcome(body.data(), body.size(), waiting)
              == DecodeError::None);
    QNP_CHECK(waiting.status == WelcomeStatus::Waiting);

    QNP_REQUIRE(healthy.client.open(port));
    QNP_REQUIRE(healthy.client.send(playerHelloFrame(
        testRoom(101), LaneDomain::Player, RelayRole::Client)));
    QNP_REQUIRE(healthy.client.receiveRecord(service, body, 12));
    Welcome accepted{};
    QNP_CHECK(qnet::relay::decodeWelcome(body.data(), body.size(), accepted)
              == DecodeError::None);
    QNP_CHECK(accepted.status == WelcomeStatus::Accepted);

    const RelayServiceMetrics metrics = service.snapshot();
    QNP_CHECK(metrics.rooms == 2);
    QNP_CHECK(metrics.roomBudgetDrops > 0);
    QNP_CHECK(metrics.drops > metrics.roomBudgetDrops);
    QNP_CHECK(metrics.playerPacketsOut == 0);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_spectator_flood_cannot_spend_player_turn_budgets) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    config.maximumUdpPacketsPerTurn = 16;
    config.maximumUdpPacketsPerRoomPerTurn = 2;
    config.maximumUdpBytesPerRoomPerTurn = 1024;
    config.maximumUnboundPacketsPerTurn = 4;
    config.roomUdpPacketsPerSecond = 1'000'000;
    config.roomUdpBytesPerSecond = 1ull << 30;
    RelayService service;
    TestVerifier verifier;
    service.setSpectatorGrantVerifier(&verifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(150);
    const uint64_t spectatorSessionId = 0x535045435441544Full;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));

    UdpClient hostUdp;
    UdpClient clientUdp;
    QNP_REQUIRE(hostUdp.open());
    QNP_REQUIRE(clientUdp.open());
    QNP_REQUIRE(hostUdp.sendTo(port, registerRecord(
        room, RelayRole::Host, player.hostWelcome.registrationToken)));
    QNP_REQUIRE(clientUdp.sendTo(port, registerRecord(
        room, RelayRole::Client, player.clientWelcome.registrationToken)));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);

    TcpClient sourceControl;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room,
        player.hostWelcome.spectatorRegistrationToken,
        spectatorSessionId, sourceControl, sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);

    TcpClient viewerControl;
    SpectatorWelcome viewerWelcome{};
    constexpr uint64_t viewerId = 0x5153504543544154ull;
    QNP_REQUIRE(admitViewer(
        service, port, room, sourceWelcome,
        0x1001, viewerId, spectatorSessionId,
        viewerControl, viewerWelcome));
    QNP_REQUIRE(viewerWelcome.status == SpectatorStatus::Waiting);

    UdpClient sourceUdp;
    UdpClient viewerUdp;
    QNP_REQUIRE(sourceUdp.open());
    QNP_REQUIRE(viewerUdp.open());
    QNP_REQUIRE(bindSpectatorUdp(
        service, port, room, SpectatorParty::Source,
        sourceWelcome.sourceGeneration,
        sourceWelcome.registrationToken, 0, sourceUdp));
    QNP_REQUIRE(bindSpectatorUdp(
        service, port, room, SpectatorParty::Viewer,
        sourceWelcome.sourceGeneration,
        viewerWelcome.registrationToken, viewerId, viewerUdp));

    for (uint32_t sequence = 1;
         sequence <= config.maximumUdpPacketsPerTurn + 1u;
         ++sequence) {
        const std::vector<uint8_t> heartbeat =
            sourceHeartbeat(spectatorSessionId, sequence);
        const std::vector<uint8_t> frame = spectatorFrame(
            SpectatorParty::Source, viewerId,
            sourceWelcome.sourceGeneration, heartbeat);
        QNP_REQUIRE(!frame.empty());
        QNP_REQUIRE(sourceUdp.sendTo(port, frame));
    }

    const qnetplay::wire::SessionCookie playerCookie{
        0x0102030405060708ull,
        0x1112131415161718ull,
    };
    const std::vector<uint8_t> playerHello =
        playerHelloDatagram(0x504C415945525345ull, playerCookie);
    QNP_REQUIRE(!playerHello.empty());
    QNP_REQUIRE(clientUdp.sendTo(port, playerHello));

    const RelayServiceMetrics before = service.snapshot();
    QNP_REQUIRE(service.pumpOnce(0));
    const RelayServiceMetrics after = service.snapshot();
    QNP_CHECK(after.playerPacketsOut == before.playerPacketsOut + 1u);
    QNP_CHECK(after.spectatorPacketsOut
              == before.spectatorPacketsOut
                  + config.maximumUdpPacketsPerRoomPerTurn);
    QNP_CHECK(after.roomBudgetDrops > before.roomBudgetDrops);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_service_enforces_per_ip_connection_quota_and_rate) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.maximumConnectionsPerIp = 2;
    config.connectionsPerIpPerSecond = 1000;
    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    TcpClient first;
    TcpClient second;
    TcpClient third;
    QNP_REQUIRE(first.open(port));
    QNP_REQUIRE(second.open(port));
    QNP_REQUIRE(third.open(port));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);

    QNP_CHECK(service.snapshot().connections == 2);
    QNP_CHECK(service.snapshot().ipQuotaRejects >= 1);
    QNP_CHECK(third.closedByPeer(service, 16));

    service.requestStop();
    service.drain();

    resetClocks();
    RelayServiceConfig rateConfig = testConfig();
    rateConfig.maximumConnectionsPerIp = 64;
    rateConfig.connectionsPerIpPerSecond = 1;
    RelayService rateService;
    QNP_REQUIRE(startTestService(rateService, rateConfig, error));
    const uint16_t ratePort = rateService.boundPort();

    std::vector<TcpClient> clients(6);
    for (TcpClient& client : clients) {
        QNP_CHECK(client.open(ratePort));
        for (int turn = 0; turn < 3; ++turn) rateService.pumpOnce(0);
    }
    for (int turn = 0;
         turn < 64 && rateService.snapshot().ipQuotaRejects < 2; ++turn) {
        rateService.pumpOnce(0);
    }
    QNP_CHECK(rateService.snapshot().connections <= 4);
    QNP_CHECK(rateService.snapshot().ipQuotaRejects >= 2);

    rateService.requestStop();
    rateService.drain();
}

QNP_TEST(relay_service_sequential_churn_does_not_reset_the_ip_bucket) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.maximumConnectionsPerIp = 64;
    config.connectionsPerIpPerSecond = 1;
    config.ipRateStateRetentionMs = 60'000;

    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    for (int attempt = 0; attempt < 4; ++attempt) {
        TcpClient client;
        QNP_REQUIRE(client.open(port));
        QNP_REQUIRE(pumpUntilConnections(service, 1));
        client.close();
        QNP_REQUIRE(pumpUntilConnections(service, 0));
    }

    QNP_CHECK(service.snapshot().trackedIpStates == 1u);
    const uint64_t rejectsBefore = service.snapshot().ipQuotaRejects;
    TcpClient exhausted;
    QNP_REQUIRE(exhausted.open(port));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(service.snapshot().connections == 0u);
    QNP_CHECK(service.snapshot().ipQuotaRejects == rejectsBefore + 1u);
    QNP_CHECK(exhausted.closedByPeer(service, 8));

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_service_idle_ip_bucket_expires_only_after_its_ttl) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.maximumConnectionsPerIp = 64;
    config.connectionsPerIpPerSecond = 1;
    config.ipRateStateRetentionMs = 100;

    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    for (int attempt = 0; attempt < 4; ++attempt) {
        TcpClient client;
        QNP_REQUIRE(client.open(port));
        QNP_REQUIRE(pumpUntilConnections(service, 1));
        client.close();
        QNP_REQUIRE(pumpUntilConnections(service, 0));
    }
    QNP_REQUIRE(service.snapshot().trackedIpStates == 1u);

    g_monotonicMs += 99;
    TcpClient beforeTtl;
    QNP_REQUIRE(beforeTtl.open(port));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(service.snapshot().connections == 0u);
    QNP_CHECK(beforeTtl.closedByPeer(service, 8));
    QNP_CHECK(service.snapshot().trackedIpStates == 1u);

    g_monotonicMs += config.ipRateStateRetentionMs;
    TcpClient afterTtl;
    QNP_REQUIRE(afterTtl.open(port));
    QNP_REQUIRE(pumpUntilConnections(service, 1));
    QNP_CHECK(service.snapshot().trackedIpStates == 1u);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_service_rejected_address_churn_cannot_grow_the_ip_table) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.maximumTrackedIpStates = 2;
    config.ipRateStateRetentionMs = 1'000;
    config.connectionsPerIpPerSecond = 1000;

    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const char* const retained[] = { "127.0.0.2", "127.0.0.3" };
    for (const char* const address : retained) {
        TcpClient client;
        QNP_REQUIRE(client.openFrom(port, address));
        QNP_REQUIRE(pumpUntilConnections(service, 1));
        client.close();
        QNP_REQUIRE(pumpUntilConnections(service, 0));
    }
    QNP_REQUIRE(service.snapshot().trackedIpStates == 2u);

    const uint64_t rejectsBefore = service.snapshot().ipQuotaRejects;
    const char* const rejected[] = {
        "127.0.0.4", "127.0.0.5", "127.0.0.6", "127.0.0.7",
        "127.0.0.8", "127.0.0.9",
    };
    for (const char* const address : rejected) {
        TcpClient client;
        QNP_REQUIRE(client.openFrom(port, address));
        for (int turn = 0; turn < 4; ++turn) service.pumpOnce(0);
        QNP_CHECK(client.closedByPeer(service, 8));
        QNP_CHECK(service.snapshot().trackedIpStates == 2u);
    }
    QNP_CHECK(service.snapshot().ipQuotaRejects
              == rejectsBefore
                  + sizeof(rejected) / sizeof(rejected[0]));

    g_monotonicMs += config.ipRateStateRetentionMs;
    QNP_REQUIRE(service.pumpOnce(0));
    QNP_CHECK(service.snapshot().trackedIpStates == 0u);

    TcpClient recovered;
    QNP_REQUIRE(recovered.openFrom(port, "127.0.0.10"));
    QNP_REQUIRE(pumpUntilConnections(service, 1));
    QNP_CHECK(service.snapshot().trackedIpStates == 1u);
    g_monotonicMs += config.ipRateStateRetentionMs;
    QNP_REQUIRE(service.pumpOnce(0));
    QNP_CHECK(service.snapshot().connections == 1u);
    QNP_CHECK(service.snapshot().trackedIpStates == 1u);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_spectator_lane_is_disabled_until_explicitly_enabled) {
    resetClocks();
    RelayService service;
    std::string error;
    RelayServiceConfig disabled = testConfig();
    disabled.spectatorLaneEnabled = false;
    QNP_REQUIRE(startTestService(service, disabled, error));
    const uint16_t port = service.boundPort();

    PairedRoom room;
    QNP_REQUIRE(pairRoom(service, port, testRoom(200), room));

    TcpClient source;
    SpectatorWelcome welcome{};
    QNP_REQUIRE(registerSource(
        service, port, testRoom(200),
        room.hostWelcome.spectatorRegistrationToken, 0x5150, source, welcome));
    QNP_CHECK(welcome.status == SpectatorStatus::Unsupported);
    QNP_CHECK(welcome.registrationToken == RegistrationToken{});
    QNP_CHECK(welcome.grantKey.empty());
    QNP_CHECK(service.snapshot().spectatorRooms == 0);

    TcpClient legacy;
    QNP_REQUIRE(legacy.open(port));
    QNP_REQUIRE(legacy.send(playerHelloFrame(
        testRoom(201), LaneDomain::Spectator, RelayRole::Host)));
    std::vector<uint8_t> body;
    QNP_REQUIRE(legacy.receiveRecord(service, body));
    Welcome rejected{};
    QNP_CHECK(qnet::relay::decodeWelcome(body.data(), body.size(), rejected)
              == DecodeError::None);
    QNP_CHECK(rejected.status == WelcomeStatus::Rejected);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_spectator_grant_admission_matrix) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    config.maximumViewersPerRoom = 8;
    RelayService service;
    TestVerifier verifier;
    service.setSpectatorGrantVerifier(&verifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(300);
    const uint64_t sessionId = 0xABCDEF01ull;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));

    TcpClient source;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room, player.hostWelcome.spectatorRegistrationToken,
        sessionId, source, sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);
    QNP_CHECK(sourceWelcome.grantKey.size() == qnet::relay::kGrantKeyBytes);
    QNP_CHECK(sourceWelcome.sourceGeneration == 1);
    QNP_CHECK(sourceWelcome.registrationToken != RegistrationToken{});
    QNP_CHECK(service.snapshot().spectatorRooms == 1);

    TcpClient viewerOne;
    SpectatorWelcome welcome{};
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            1001, 501, sessionId, viewerOne, welcome));
    QNP_CHECK(welcome.status == SpectatorStatus::Waiting);
    QNP_CHECK(welcome.viewerId == 501);
    QNP_CHECK(welcome.registrationToken != RegistrationToken{});
    QNP_CHECK(welcome.grantKey.empty());
    QNP_CHECK(service.snapshot().viewers == 1);

    TcpClient badMac;
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            1002, 502, sessionId, badMac, welcome, true));
    QNP_CHECK(welcome.status == SpectatorStatus::GrantRejected);

    TcpClient expired;
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            1003, 503, sessionId, expired, welcome, false, 0,
                            g_wallUnix - 1ull));
    QNP_CHECK(welcome.status == SpectatorStatus::GrantExpired);

    TcpClient wrongViewer;
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            1004, 504, sessionId, wrongViewer, welcome,
                            false, 999));
    QNP_CHECK(welcome.status == SpectatorStatus::GrantRejected);

    TcpClient wrongSession;
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            1005, 505, sessionId, wrongSession, welcome,
                            false, 0, 0, sessionId ^ 0x55ull));
    QNP_CHECK(welcome.status == SpectatorStatus::GrantRejected);

    const uint64_t replayRejectsBefore =
        service.snapshot().grantReplayRejects;
    TcpClient replay;
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            1001, 501, sessionId, replay, welcome));
    QNP_CHECK(welcome.status == SpectatorStatus::GrantRejected);
    QNP_CHECK(service.snapshot().grantReplayRejects > replayRejectsBefore);

    QNP_CHECK(service.snapshot().viewers == 1);
    QNP_CHECK(service.snapshot().grantRejects >= 3);
    QNP_CHECK(service.snapshot().grantExpiredRejects >= 1);

    TcpClient eternal;
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            1006, 506, sessionId, eternal, welcome, false, 0,
                            g_wallUnix + 100000ull));
    QNP_CHECK(welcome.status == SpectatorStatus::GrantRejected);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_public_spectator_token_mints_independent_viewer_leases) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    config.maximumViewersPerRoom = 8;
    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(350);
    const uint64_t sessionId = 0x51504F484Cull;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));
    QNP_REQUIRE(player.hostWelcome.viewerAccessToken
                != RegistrationToken{});

    TcpClient source;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room,
        player.hostWelcome.spectatorRegistrationToken,
        sessionId, source, sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);

    TcpClient viewerOne;
    TcpClient viewerTwo;
    SpectatorWelcome welcomeOne{};
    SpectatorWelcome welcomeTwo{};
    QNP_REQUIRE(admitPublicViewer(
        service, port, room, player.hostWelcome.viewerAccessToken,
        sessionId, viewerOne, welcomeOne, 7, 0xABCDEFu));
    QNP_REQUIRE(admitPublicViewer(
        service, port, room, player.hostWelcome.viewerAccessToken,
        sessionId, viewerTwo, welcomeTwo, 11, 0x123456u));
    QNP_CHECK(welcomeOne.status == SpectatorStatus::Waiting);
    QNP_CHECK(welcomeTwo.status == SpectatorStatus::Waiting);
    QNP_CHECK(welcomeOne.viewerId != 0);
    QNP_CHECK(welcomeTwo.viewerId != 0);
    QNP_CHECK(welcomeOne.viewerId != welcomeTwo.viewerId);
    QNP_CHECK(welcomeOne.registrationToken != RegistrationToken{});
    QNP_CHECK(welcomeTwo.registrationToken != RegistrationToken{});
    QNP_CHECK(welcomeOne.registrationToken
              != welcomeTwo.registrationToken);
    QNP_CHECK(welcomeOne.sourceGeneration
              == sourceWelcome.sourceGeneration);
    QNP_CHECK(welcomeTwo.sourceGeneration
              == sourceWelcome.sourceGeneration);
    QNP_CHECK(service.snapshot().viewers == 2);

    RegistrationToken wrong =
        player.hostWelcome.viewerAccessToken;
    wrong.low ^= 0x40ull;
    TcpClient rejected;
    SpectatorWelcome rejectedWelcome{};
    QNP_REQUIRE(admitPublicViewer(
        service, port, room, wrong, sessionId,
        rejected, rejectedWelcome));
    QNP_CHECK(rejectedWelcome.status
              == SpectatorStatus::GrantRejected);
    QNP_CHECK(service.snapshot().viewers == 2);

    viewerOne.close();
    for (int turn = 0; turn < 64
         && service.snapshot().viewers != 1; ++turn) {
        service.pumpOnce(0);
    }
    QNP_CHECK(service.snapshot().viewers == 1);
    QNP_CHECK(service.snapshot().spectatorRooms == 1);
    QNP_CHECK(service.snapshot().pairedRooms == 1);
    QNP_CHECK(viewerTwo.valid());

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_unregistered_viewer_lease_expires_and_releases_capacity) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    config.maximumViewersPerRoom = 1;
    config.maximumViewers = 1;
    config.spectatorRegistrationTimeoutMs = 200;
    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(351);
    const uint64_t sessionId = 0x51504F4855ull;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));

    TcpClient source;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room,
        player.hostWelcome.spectatorRegistrationToken,
        sessionId, source, sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);
    UdpClient sourceUdp;
    QNP_REQUIRE(sourceUdp.open());
    QNP_REQUIRE(bindSpectatorUdp(
        service, port, room, SpectatorParty::Source,
        sourceWelcome.sourceGeneration,
        sourceWelcome.registrationToken, 0, sourceUdp));

    TcpClient stalled;
    SpectatorWelcome stalledWelcome{};
    QNP_REQUIRE(admitPublicViewer(
        service, port, room, player.hostWelcome.viewerAccessToken,
        sessionId, stalled, stalledWelcome));
    QNP_REQUIRE(stalledWelcome.status == SpectatorStatus::Accepted);
    QNP_CHECK(service.snapshot().viewers == 1);

    g_monotonicMs += 199;
    QNP_REQUIRE(bindSpectatorUdp(
        service, port, room, SpectatorParty::Source,
        sourceWelcome.sourceGeneration,
        sourceWelcome.registrationToken, 0, sourceUdp));
    QNP_CHECK(service.snapshot().viewers == 1);
    g_monotonicMs += 1;
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(service.snapshot().viewers == 0);
    QNP_CHECK(stalled.closedByPeer(service, 16));

    TcpClient recovered;
    SpectatorWelcome recoveredWelcome{};
    QNP_REQUIRE(admitPublicViewer(
        service, port, room, player.hostWelcome.viewerAccessToken,
        sessionId, recovered, recoveredWelcome));
    QNP_CHECK(recoveredWelcome.status == SpectatorStatus::Accepted);
    QNP_CHECK(recoveredWelcome.viewerId != stalledWelcome.viewerId);
    QNP_CHECK(service.snapshot().viewers == 1);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_retains_spectator_source_after_player_tcp_closes) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(352);
    const uint64_t sessionId = 0x51504F4852ull;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));
    const RegistrationToken publicToken =
        player.hostWelcome.viewerAccessToken;

    TcpClient source;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room,
        player.hostWelcome.spectatorRegistrationToken,
        sessionId, source, sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);
    UdpClient sourceUdp;
    QNP_REQUIRE(sourceUdp.open());
    QNP_REQUIRE(bindSpectatorUdp(
        service, port, room, SpectatorParty::Source,
        sourceWelcome.sourceGeneration,
        sourceWelcome.registrationToken, 0, sourceUdp));

    player.host.close();
    for (int turn = 0; turn < 64
         && service.snapshot().pairedRooms != 0; ++turn) {
        service.pumpOnce(0);
    }
    QNP_CHECK(service.snapshot().rooms == 1);
    QNP_CHECK(service.snapshot().pairedRooms == 0);
    QNP_CHECK(service.snapshot().spectatorRooms == 1);
    QNP_CHECK(player.client.closedByPeer(service, 16));

    TcpClient lateViewer;
    SpectatorWelcome lateWelcome{};
    QNP_REQUIRE(admitPublicViewer(
        service, port, room, publicToken,
        sessionId, lateViewer, lateWelcome));
    QNP_REQUIRE(lateWelcome.status == SpectatorStatus::Accepted);
    UdpClient viewerUdp;
    QNP_REQUIRE(viewerUdp.open());
    QNP_REQUIRE(bindSpectatorUdp(
        service, port, room, SpectatorParty::Viewer,
        sourceWelcome.sourceGeneration,
        lateWelcome.registrationToken, lateWelcome.viewerId, viewerUdp));

    const std::vector<uint8_t> heartbeat =
        sourceHeartbeat(sessionId, 1);
    const std::vector<uint8_t> outbound = spectatorFrame(
        SpectatorParty::Source, lateWelcome.viewerId,
        sourceWelcome.sourceGeneration, heartbeat);
    QNP_REQUIRE(sourceUdp.sendTo(port, outbound));
    std::vector<uint8_t> forwarded;
    QNP_CHECK(receiveForwardedFrame(
        service, viewerUdp, forwarded, 32));
    QNP_CHECK(forwarded == outbound);

    source.close();
    for (int turn = 0; turn < 64 && service.snapshot().rooms != 0; ++turn)
        service.pumpOnce(0);
    QNP_CHECK(service.snapshot().rooms == 0);
    QNP_CHECK(service.snapshot().spectatorRooms == 0);
    QNP_CHECK(service.snapshot().viewers == 0);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_lanes_cannot_cross_authenticate_or_share_capacity) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    config.maximumRooms = 1;
    config.maximumViewersPerRoom = 1;
    config.maximumViewers = 1;
    RelayService service;
    TestVerifier verifier;
    service.setSpectatorGrantVerifier(&verifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(400);
    const uint64_t sessionId = 0x77777ull;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));

    TcpClient impostor;
    SpectatorWelcome welcome{};
    QNP_REQUIRE(registerSource(
        service, port, room, player.hostWelcome.registrationToken,
        sessionId, impostor, welcome));
    QNP_CHECK(welcome.status == SpectatorStatus::GrantRejected);
    QNP_CHECK(service.snapshot().spectatorRooms == 0);

    UdpClient crossLane;
    QNP_REQUIRE(crossLane.open());
    QNP_REQUIRE(crossLane.sendTo(port, registerRecord(
        room, RelayRole::Host,
        player.hostWelcome.spectatorRegistrationToken)));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    std::vector<uint8_t> ignored;
    QNP_CHECK(!crossLane.receive(service, ignored, 8));

    TcpClient source;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room, player.hostWelcome.spectatorRegistrationToken,
        sessionId, source, sourceWelcome));
    QNP_CHECK(sourceWelcome.status == SpectatorStatus::Accepted);
    QNP_CHECK(service.snapshot().rooms == 1);
    QNP_CHECK(service.snapshot().pairedRooms == 1);

    TcpClient viewerOne;
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            2001, 601, sessionId, viewerOne, welcome));
    QNP_CHECK(welcome.status == SpectatorStatus::Waiting);

    TcpClient viewerTwo;
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            2002, 602, sessionId, viewerTwo, welcome));
    QNP_CHECK(welcome.status == SpectatorStatus::CapacityReached);
    QNP_CHECK(service.snapshot().viewers == 1);
    QNP_CHECK(service.snapshot().pairedRooms == 1);

    std::vector<uint8_t> payload{ 0x5A, 0x5B };
    std::array<uint8_t, qnet::kMaxMsgSize> message{};
    const size_t length = qnet::packMsg(
        0x41, payload.data(), payload.size(),
        message.data(), message.size());
    std::vector<uint8_t> frame(length + 2u, 0u);
    frame[0] = static_cast<uint8_t>(length);
    frame[1] = static_cast<uint8_t>(length >> 8u);
    std::memcpy(frame.data() + 2u, message.data(), length);
    QNP_CHECK(player.host.send(frame));
    std::vector<uint8_t> body;
    QNP_REQUIRE(player.client.receiveRecord(service, body));
    QNP_CHECK(body.size() == 2u && body[0] == 0x5A);

    const uint64_t crossBefore = service.snapshot().crossLaneRejects;
    QNP_CHECK(viewerOne.send(frame));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(service.snapshot().crossLaneRejects > crossBefore);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_spectator_viewer_tcp_connection_is_the_capacity_lease) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    RelayService service;
    TestVerifier verifier;
    service.setSpectatorGrantVerifier(&verifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(899);
    const uint64_t sessionId = 0x1BADB001ull;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));

    TcpClient sourceControl;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room, player.hostWelcome.spectatorRegistrationToken,
        sessionId, sourceControl, sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);

    TcpClient viewerControl;
    SpectatorWelcome viewerWelcome{};
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            4991, 891, sessionId, viewerControl,
                            viewerWelcome));
    QNP_REQUIRE(viewerWelcome.status == SpectatorStatus::Waiting);
    QNP_CHECK(service.snapshot().viewers == 1);
    QNP_CHECK(service.snapshot().spectatorRooms == 1);

    viewerControl.close();
    for (int turn = 0; turn < 64 && service.snapshot().viewers != 0;
         ++turn) {
        service.pumpOnce(0);
    }
    QNP_CHECK(service.snapshot().viewers == 0);
    QNP_CHECK(service.snapshot().viewersDetached >= 1);
    QNP_CHECK(service.snapshot().spectatorRooms == 1);
    QNP_CHECK(service.snapshot().pairedRooms == 1);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_spectator_data_plane_forwards_only_validated_qsp) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    RelayService service;
    TestVerifier verifier;
    service.setSpectatorGrantVerifier(&verifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(900);
    const uint64_t sessionId = 0x1BADB002ull;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));

    TcpClient sourceControl;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room, player.hostWelcome.spectatorRegistrationToken,
        sessionId, sourceControl, sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);
    const uint32_t generation = sourceWelcome.sourceGeneration;

    TcpClient viewerControl;
    SpectatorWelcome viewerWelcome{};
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            5001, 901, sessionId, viewerControl,
                            viewerWelcome));
    QNP_REQUIRE(viewerWelcome.status == SpectatorStatus::Waiting);

    UdpClient sourceUdp;
    UdpClient viewerUdp;
    QNP_REQUIRE(sourceUdp.open());
    QNP_REQUIRE(viewerUdp.open());
    QNP_REQUIRE(bindSpectatorUdp(service, port, room, SpectatorParty::Source,
                                 generation, sourceWelcome.registrationToken,
                                 0, sourceUdp));
    QNP_REQUIRE(bindSpectatorUdp(service, port, room, SpectatorParty::Viewer,
                                 generation, viewerWelcome.registrationToken,
                                 901, viewerUdp));

    const std::vector<uint8_t> heartbeat = sourceHeartbeat(sessionId, 21);
    QNP_REQUIRE(!heartbeat.empty());
    const std::vector<uint8_t> outbound = spectatorFrame(
        SpectatorParty::Source, 901, generation, heartbeat);
    QNP_REQUIRE(sourceUdp.sendTo(port, outbound));
    std::vector<uint8_t> received;
    QNP_REQUIRE(receiveForwardedFrame(service, viewerUdp, received));
    QNP_CHECK(received == outbound);

    const std::vector<uint8_t> ack =
        viewerAck(sessionId, 901, generation, 22);
    QNP_REQUIRE(!ack.empty());
    const std::vector<uint8_t> inbound = spectatorFrame(
        SpectatorParty::Viewer, 901, generation, ack);
    QNP_REQUIRE(viewerUdp.sendTo(port, inbound));
    QNP_REQUIRE(receiveForwardedFrame(service, sourceUdp, received));
    QNP_CHECK(received == inbound);
    QNP_CHECK(service.snapshot().spectatorPacketsOut == 2);
    QNP_CHECK(service.snapshot().playerPacketsOut == 0);

    std::vector<uint8_t> corrupt = outbound;
    corrupt.back() ^= 1;
    const uint64_t dropsBefore = service.snapshot().drops;
    QNP_REQUIRE(sourceUdp.sendTo(port, corrupt));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    std::vector<uint8_t> nothing;
    QNP_CHECK(!receiveForwardedFrame(service, viewerUdp, nothing, 2));
    QNP_CHECK(service.snapshot().drops > dropsBefore);

    const std::vector<uint8_t> impersonation = spectatorFrame(
        SpectatorParty::Viewer, 901, generation, heartbeat);
    QNP_REQUIRE(viewerUdp.sendTo(port, impersonation));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(!receiveForwardedFrame(service, sourceUdp, nothing, 2));

    const std::vector<uint8_t> stale = spectatorFrame(
        SpectatorParty::Source, 901, generation + 1u, heartbeat);
    QNP_REQUIRE(sourceUdp.sendTo(port, stale));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(!receiveForwardedFrame(service, viewerUdp, nothing, 2));

    UdpClient playerUdp;
    QNP_REQUIRE(playerUdp.open());
    QNP_REQUIRE(playerUdp.sendTo(port, registerRecord(
        room, RelayRole::Host, player.hostWelcome.registrationToken)));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    const uint64_t crossBefore = service.snapshot().crossLaneRejects;
    QNP_REQUIRE(playerUdp.sendTo(port, outbound));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(service.snapshot().crossLaneRejects > crossBefore);
    QNP_CHECK(!receiveForwardedFrame(service, viewerUdp, nothing, 2));

    const uint64_t crossBeforePlayer = service.snapshot().crossLaneRejects;
    std::vector<uint8_t> playerShaped(64, 0x5C);
    QNP_REQUIRE(sourceUdp.sendTo(port, playerShaped));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(service.snapshot().crossLaneRejects > crossBeforePlayer);

    qnet::relay::SpectatorDetach detach{};
    detach.room = room;
    detach.party = SpectatorParty::Viewer;
    detach.reason = qnet::relay::SpectatorDetachReason::ViewerRequested;
    detach.sourceGeneration = generation;
    detach.token = viewerWelcome.registrationToken;
    detach.viewerId = 901;
    std::vector<uint8_t> detachBytes;
    QNP_REQUIRE(qnet::relay::encodeSpectatorDetach(detach, detachBytes));
    QNP_REQUIRE(viewerUdp.sendTo(port, detachBytes));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(service.snapshot().viewers == 0);
    QNP_CHECK(service.snapshot().viewersDetached >= 1);

    bool sawSourceNotice = false;
    for (int attempt = 0; attempt < 8 && !sawSourceNotice; ++attempt) {
        std::vector<uint8_t> notice;
        if (!sourceUdp.receive(service, notice, 4)) break;
        qnet::relay::SpectatorDetach decoded{};
        if (qnet::relay::decodeSpectatorDetach(
                notice.data(), notice.size(), decoded)
            != DecodeError::None) {
            continue;
        }
        if (decoded.party == SpectatorParty::Source
            && decoded.viewerId == 901) {
            sawSourceNotice = true;
        }
    }
    QNP_CHECK(sawSourceNotice);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_log_rotation_stays_within_its_total_byte_budget) {
    resetClocks();
    char temporaryDirectory[MAX_PATH] = {};
    QNP_REQUIRE(GetTempPathA(MAX_PATH, temporaryDirectory) != 0);
    const std::string logPath =
        std::string(temporaryDirectory) + "qoh99_relay_rotation_test.log";
    const uint32_t files = 4;
    const uint64_t budget = 64ull * 1024ull;
    for (uint32_t index = 0; index <= files; ++index) {
        const std::string path = index == 0
            ? logPath
            : logPath + "." + std::to_string(index);
        std::remove(path.c_str());
    }

    RelayServiceConfig config = testConfig();
    config.logPath = logPath;
    config.logFiles = files;
    config.logMaximumTotalBytes = budget;
    config.metricsIntervalMs = 1000;
    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));

    for (int tick = 0; tick < 400; ++tick) {
        g_monotonicMs += 1000;
        service.pumpOnce(0);
    }
    service.requestStop();
    service.drain();

    uint64_t total = 0;
    bool rotated = false;
    for (uint32_t index = 0; index <= files; ++index) {
        const std::string path = index == 0
            ? logPath
            : logPath + "." + std::to_string(index);
        std::FILE* const handle = std::fopen(path.c_str(), "rb");
        if (handle == nullptr) continue;
        std::fseek(handle, 0, SEEK_END);
        const long size = std::ftell(handle);
        std::fclose(handle);
        if (size > 0) {
            total += static_cast<uint64_t>(size);
            if (index >= 1) rotated = true;
        }
        QNP_CHECK(index <= files);
        std::remove(path.c_str());
    }
    QNP_CHECK(rotated);
    QNP_CHECK(total > 0);
    QNP_CHECK(total <= budget + 1024ull);

    const std::string health = service.healthLine();
    QNP_CHECK(!health.empty());
    QNP_CHECK(health.find("rooms=") != std::string::npos);
}

QNP_TEST(relay_returns_the_authenticated_observed_endpoint) {
    resetClocks();
    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, testConfig(), error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(500);
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));

    UdpClient hostUdp;
    QNP_REQUIRE(hostUdp.open());
    const uint16_t hostPort = hostUdp.port();

    qnet::relay::ObservedRequest request{};
    request.room = room;
    request.domain = LaneDomain::Player;
    request.role = RelayRole::Host;
    request.token = player.hostWelcome.registrationToken;
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(qnet::relay::encodeObservedRequest(request, encoded));
    QNP_REQUIRE(hostUdp.sendTo(port, encoded));

    std::vector<uint8_t> answer;
    QNP_REQUIRE(hostUdp.receive(service, answer));
    qnet::relay::ObservedEndpoint observed{};
    QNP_REQUIRE(qnet::relay::decodeObservedEndpoint(
                    answer.data(), answer.size(), observed)
                == DecodeError::None);
    QNP_CHECK(observed.room == room);
    QNP_CHECK(observed.role == RelayRole::Host);
    QNP_CHECK(observed.token == player.hostWelcome.registrationToken);
    QNP_CHECK(observed.observed.port == hostPort);
    QNP_CHECK(observed.observed.ipv4 != 0);
    QNP_CHECK(service.snapshot().observedRequests == 1);

    UdpClient clientUdp;
    QNP_REQUIRE(clientUdp.open());
    qnet::relay::ObservedRequest clientRequest{};
    clientRequest.room = room;
    clientRequest.domain = LaneDomain::Player;
    clientRequest.role = RelayRole::Client;
    clientRequest.token = player.clientWelcome.registrationToken;
    QNP_CHECK(clientRequest.token != player.hostWelcome.registrationToken);
    QNP_REQUIRE(qnet::relay::encodeObservedRequest(clientRequest, encoded));
    QNP_REQUIRE(clientUdp.sendTo(port, encoded));
    QNP_REQUIRE(clientUdp.receive(service, answer));
    qnet::relay::ObservedEndpoint clientObserved{};
    QNP_REQUIRE(qnet::relay::decodeObservedEndpoint(
                    answer.data(), answer.size(), clientObserved)
                == DecodeError::None);
    QNP_CHECK(clientObserved.role == RelayRole::Client);
    QNP_CHECK(clientObserved.token
              == player.clientWelcome.registrationToken);
    QNP_CHECK(clientObserved.observed.port == clientUdp.port());
    QNP_CHECK(clientObserved.observed.port != hostPort);

    const uint64_t rejectsBefore = service.snapshot().observedRejects;
    UdpClient forger;
    QNP_REQUIRE(forger.open());
    qnet::relay::ObservedRequest forged{};
    forged.room = room;
    forged.domain = LaneDomain::Player;
    forged.role = RelayRole::Host;
    forged.token = RegistrationToken{
        player.hostWelcome.registrationToken.low ^ 1ull,
        player.hostWelcome.registrationToken.high,
    };
    QNP_REQUIRE(qnet::relay::encodeObservedRequest(forged, encoded));
    QNP_REQUIRE(forger.sendTo(port, encoded));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    std::vector<uint8_t> stolen;
    QNP_CHECK(!forger.receive(service, stolen, 8));

    forged.room = testRoom(501);
    forged.token = player.hostWelcome.registrationToken;
    QNP_REQUIRE(qnet::relay::encodeObservedRequest(forged, encoded));
    QNP_REQUIRE(forger.sendTo(port, encoded));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(!forger.receive(service, stolen, 8));
    QNP_CHECK(service.snapshot().observedRejects >= rejectsBefore + 2u);
    QNP_CHECK(service.snapshot().observedRequests == 2);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_observed_extension_degrades_explicitly_against_old_peers) {
    qnet::relay::ObservedRequest request{};
    request.room = testRoom(600);
    request.domain = LaneDomain::Player;
    request.role = RelayRole::Host;
    request.token = RegistrationToken{ 0x99, 0xAA };
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(qnet::relay::encodeObservedRequest(request, encoded));
    Register asRegister{};
    QNP_CHECK(qnet::relay::decodeRegister(
                  encoded.data(), encoded.size(), asRegister)
              == DecodeError::BadVersion);

    qnet::relay::ObservedEndpoint observed{};
    observed.room = request.room;
    observed.domain = LaneDomain::Player;
    observed.role = RelayRole::Host;
    observed.token = request.token;
    observed.observed = qnet::relay::Ipv4Endpoint{ 0x0100007Fu, 4000 };
    QNP_REQUIRE(qnet::relay::encodeObservedEndpoint(observed, encoded));
    qnet::relay::Ready asReady{};
    QNP_CHECK(qnet::relay::decodeReady(
                  encoded.data(), encoded.size(), asReady)
              == DecodeError::BadVersion);

    resetClocks();
    RelayService service;
    std::string error;
    QNP_REQUIRE(startTestService(service, testConfig(), error));
    const uint16_t port = service.boundPort();
    UdpClient stranger;
    QNP_REQUIRE(stranger.open());
    std::vector<uint8_t> future = encoded;
    future[4] = 0x7F;
    future[5] = 0x00;
    QNP_REQUIRE(stranger.sendTo(port, future));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    std::vector<uint8_t> ignored;
    QNP_CHECK(!stranger.receive(service, ignored, 8));
    QNP_CHECK(service.snapshot().drops >= 1);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_graceful_restart_gives_clients_a_bounded_failure_path) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    config.drainTimeoutMs = 200;
    config.retryAfterMs = 7000;
    RelayService service;
    TestVerifier verifier;
    service.setSpectatorGrantVerifier(&verifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(700);
    const uint64_t sessionId = 0x515151ull;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));

    TcpClient source;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room, player.hostWelcome.spectatorRegistrationToken,
        sessionId, source, sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);

    TcpClient viewer;
    SpectatorWelcome viewerWelcome{};
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            3001, 701, sessionId, viewer, viewerWelcome));
    QNP_REQUIRE(viewerWelcome.status == SpectatorStatus::Waiting);

    UdpClient viewerUdp;
    QNP_REQUIRE(viewerUdp.open());
    qnet::relay::SpectatorRegister registration{};
    registration.room = room;
    registration.party = SpectatorParty::Viewer;
    registration.sourceGeneration = sourceWelcome.sourceGeneration;
    registration.token = viewerWelcome.registrationToken;
    registration.viewerId = 701;
    std::vector<uint8_t> encoded;
    QNP_REQUIRE(qnet::relay::encodeSpectatorRegister(registration, encoded));
    QNP_REQUIRE(viewerUdp.sendTo(port, encoded));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);

    service.requestStop();
    QNP_CHECK(service.stopRequested());
    service.drain();

    std::vector<uint8_t> notice;
    bool sawServiceStopping = false;
    for (int attempt = 0; attempt < 8 && !sawServiceStopping; ++attempt) {
        if (!viewerUdp.receive(service, notice, 1)) break;
        qnet::relay::SpectatorDetach detach{};
        if (qnet::relay::decodeSpectatorDetach(
                notice.data(), notice.size(), detach) != DecodeError::None) {
            continue;
        }
        if (detach.reason
            == qnet::relay::SpectatorDetachReason::ServiceStopping) {
            sawServiceStopping = true;
            QNP_CHECK(detach.retryAfterMs == 7000);
            QNP_CHECK(detach.viewerId == 701);
        }
    }
    QNP_CHECK(sawServiceStopping);

    QNP_CHECK(player.host.closedByPeer(service, 4));
    QNP_CHECK(player.client.closedByPeer(service, 4));
    QNP_CHECK(!service.started());

    RelayService restarted;
    QNP_CHECK(startTestService(restarted, config, error));
    restarted.requestStop();
    restarted.drain();
}

QNP_TEST(relay_logs_never_contain_secret_material) {
    resetClocks();
    RelayServiceConfig config = testConfig();
    config.spectatorLaneEnabled = true;
    config.verbose = true;
    RelayService service;
    TestVerifier verifier;
    TestTopologyVerifier topologyVerifier;
    LogCapture capture;
    service.setSpectatorGrantVerifier(&verifier);
    service.setTopologyGrantVerifier(&topologyVerifier);
    service.setLogSink(&LogCapture::sink, &capture);
    std::string error;
    QNP_REQUIRE(startTestService(service, config, error));
    const uint16_t port = service.boundPort();

    const RoomId room = testRoom(800);
    const uint64_t sessionId = 0x1234567890ABCDEFull;
    PairedRoom player;
    QNP_REQUIRE(pairRoom(service, port, room, player));

    TcpClient source;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room, player.hostWelcome.spectatorRegistrationToken,
        sessionId, source, sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);

    TcpClient viewer;
    SpectatorWelcome viewerWelcome{};
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome,
                            4001, 801, sessionId, viewer, viewerWelcome));
    QNP_REQUIRE(viewerWelcome.status == SpectatorStatus::Waiting);

    UdpClient hostUdp;
    QNP_REQUIRE(hostUdp.open());
    QNP_REQUIRE(hostUdp.sendTo(port, registerRecord(
        room, RelayRole::Host, player.hostWelcome.registrationToken)));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);

    TcpClient topologyAuthority;
    qnet::relay::TopologyWelcome topologyAuthorityWelcome{};
    QNP_REQUIRE(registerTopologyAuthority(
        service, port, room, player.hostWelcome.spectatorRegistrationToken,
        sessionId, topologyAuthority, topologyAuthorityWelcome));
    QNP_REQUIRE(topologyAuthorityWelcome.status
                == qnet::relay::TopologyStatus::Accepted);
    TcpClient topologyNode;
    qnet::relay::TopologyWelcome topologyNodeWelcome{};
    QNP_REQUIRE(admitTopologyNode(
        service, port, room, topologyAuthorityWelcome, 4002, 802, sessionId,
        topologyNode, topologyNodeWelcome));
    QNP_REQUIRE(topologyNodeWelcome.status
                == qnet::relay::TopologyStatus::Waiting);

    service.requestStop();
    service.drain();

    QNP_REQUIRE(!capture.lines.empty());

    QNP_CHECK(!logsLeak(capture, room.low));
    QNP_CHECK(!logsLeak(capture, room.high));
    QNP_CHECK(!capture.contains(qnet::relay::formatRoomId(room)));
    QNP_CHECK(!logsLeak(capture, player.hostWelcome.registrationToken.low));
    QNP_CHECK(!logsLeak(capture, player.hostWelcome.registrationToken.high));
    QNP_CHECK(!logsLeak(capture, player.clientWelcome.registrationToken.low));
    QNP_CHECK(!logsLeak(capture,
                        player.hostWelcome.spectatorRegistrationToken.low));
    QNP_CHECK(!logsLeak(capture, player.hostWelcome.viewerAccessToken.low));
    QNP_CHECK(!logsLeak(capture, sourceWelcome.registrationToken.low));
    QNP_CHECK(!logsLeak(capture, viewerWelcome.registrationToken.low));
    QNP_REQUIRE(sourceWelcome.grantKey.size() == qnet::relay::kGrantKeyBytes);
    for (size_t offset = 0; offset + 8u <= sourceWelcome.grantKey.size();
         offset += 8u) {
        uint64_t word = 0;
        std::memcpy(&word, sourceWelcome.grantKey.data() + offset,
                    sizeof(word));
        QNP_CHECK(!logsLeak(capture, word));
    }
    QNP_CHECK(!logsLeak(capture,
                        topologyAuthorityWelcome.registrationToken.low));
    QNP_CHECK(!logsLeak(capture,
                        topologyAuthorityWelcome.registrationToken.high));
    QNP_CHECK(!logsLeak(capture, topologyNodeWelcome.registrationToken.low));
    QNP_CHECK(!logsLeak(capture, topologyNodeWelcome.registrationToken.high));
    QNP_REQUIRE(topologyAuthorityWelcome.grantKey.size()
                == qnet::relay::kGrantKeyBytes);
    for (size_t offset = 0;
         offset + 8u <= topologyAuthorityWelcome.grantKey.size();
         offset += 8u) {
        uint64_t word = 0;
        std::memcpy(&word, topologyAuthorityWelcome.grantKey.data() + offset,
                    sizeof(word));
        QNP_CHECK(!logsLeak(capture, word));
    }
    QNP_CHECK(!capture.contains("grantKey"));
    QNP_CHECK(!capture.contains("mac="));
    QNP_CHECK(!capture.contains("token="));
    QNP_CHECK(!capture.contains("cookie"));

    QNP_CHECK(capture.contains("room #"));
}

QNP_TEST(relay_topology_admission_matrix) {
    resetClocks();
    TestTopologyVerifier topologyVerifier;
    RelayService service;
    service.setTopologyGrantVerifier(&topologyVerifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, testConfig(), error));
    const uint16_t port = service.boundPort();
    const RoomId room = testRoom(600);
    PairedRoom paired;
    QNP_REQUIRE(pairRoom(service, port, room, paired));

    const uint64_t sessionId = 0x5151515151515151ull;

    {
        qnet::relay::TopologyWelcome premature{};
        qnet::relay::TopologyWelcome fakeAuthority{};
        fakeAuthority.authorityGeneration = 1u;
        fakeAuthority.grantKey.assign(qnet::relay::kGrantKeyBytes, 0x11u);
        TcpClient early;
        QNP_REQUIRE(admitTopologyNode(service, port, room, fakeAuthority,
                                      0x9001u, 0x71u, sessionId, early,
                                      premature));
        QNP_CHECK(premature.status
                  == qnet::relay::TopologyStatus::AuthorityUnavailable);
    }

    {
        qnet::relay::TopologyWelcome refused{};
        TcpClient impostor;
        QNP_REQUIRE(registerTopologyAuthority(
            service, port, room,
            paired.hostWelcome.viewerAccessToken, sessionId, impostor,
            refused));
        QNP_CHECK(refused.status
                  == qnet::relay::TopologyStatus::GrantRejected);
    }

    TcpClient authority;
    qnet::relay::TopologyWelcome authorityWelcome{};
    QNP_REQUIRE(registerTopologyAuthority(
        service, port, room,
        paired.hostWelcome.spectatorRegistrationToken, sessionId, authority,
        authorityWelcome));
    QNP_CHECK(authorityWelcome.status
              == qnet::relay::TopologyStatus::Accepted);
    QNP_CHECK(authorityWelcome.grantKey.size()
              == qnet::relay::kGrantKeyBytes);
    QNP_CHECK(authorityWelcome.authorityGeneration == 1u);
    QNP_CHECK(service.snapshot().topologyRooms == 1u);

    TcpClient node;
    qnet::relay::TopologyWelcome nodeWelcome{};
    QNP_REQUIRE(admitTopologyNode(service, port, room, authorityWelcome,
                                  0x9002u, 0x71u, sessionId, node,
                                  nodeWelcome));
    QNP_CHECK(nodeWelcome.status == qnet::relay::TopologyStatus::Waiting);
    QNP_CHECK(nodeWelcome.nodeId == 0x71u);
    QNP_CHECK(service.snapshot().topologyNodes == 1u);

    const RelayServiceMetrics before = service.snapshot();

    {
        TcpClient refused;
        qnet::relay::TopologyWelcome welcome{};
        QNP_REQUIRE(admitTopologyNode(service, port, room, authorityWelcome,
                                      0x9003u, 0x72u, sessionId, refused,
                                      welcome, true));
        QNP_CHECK(welcome.status
                  == qnet::relay::TopologyStatus::GrantRejected);
    }
    {
        TcpClient refused;
        qnet::relay::TopologyWelcome welcome{};
        QNP_REQUIRE(admitTopologyNode(service, port, room, authorityWelcome,
                                      0x9004u, 0x73u, sessionId, refused,
                                      welcome, false,
                                      g_wallUnix - 1u));
        QNP_CHECK(welcome.status
                  == qnet::relay::TopologyStatus::GrantExpired);
    }
    {
        TcpClient refused;
        qnet::relay::TopologyWelcome welcome{};
        QNP_REQUIRE(admitTopologyNode(service, port, room, authorityWelcome,
                                      0x9005u, 0x74u, sessionId, refused,
                                      welcome, false,
                                      g_wallUnix + 100000u));
        QNP_CHECK(welcome.status
                  == qnet::relay::TopologyStatus::GrantRejected);
    }
    {
        TcpClient refused;
        qnet::relay::TopologyWelcome welcome{};
        QNP_REQUIRE(admitTopologyNode(service, port, room, authorityWelcome,
                                      0x9006u, 0x75u, sessionId, refused,
                                      welcome, false, 0,
                                      7u));
        QNP_CHECK(welcome.status
                  == qnet::relay::TopologyStatus::Superseded);
    }
    {
        TcpClient refused;
        qnet::relay::TopologyWelcome welcome{};
        QNP_REQUIRE(admitTopologyNode(service, port, room, authorityWelcome,
                                      0x9002u, 0x76u, sessionId, refused,
                                      welcome));
        QNP_CHECK(welcome.status
                  == qnet::relay::TopologyStatus::GrantRejected);
    }
    {
        TcpClient refused;
        qnet::relay::TopologyWelcome welcome{};
        QNP_REQUIRE(admitTopologyNode(service, port, room, authorityWelcome,
                                      0x9007u, 0x77u, sessionId, refused,
                                      welcome, false, 0, 0,
                                      0x1234u));
        QNP_CHECK(welcome.status
                  == qnet::relay::TopologyStatus::GrantRejected);
    }

    const RelayServiceMetrics after = service.snapshot();
    QNP_CHECK(after.topologyGrantRejects >= before.topologyGrantRejects + 4u);
    QNP_CHECK(after.topologyGrantReplayRejects
              == before.topologyGrantReplayRejects + 1u);
    QNP_CHECK(after.topologyNodes == 1u);

    {
        QNP_CHECK(topologyGrantVerifierAvailable());
        RelayService defaultService;
        std::string defaultError;
        QNP_REQUIRE(startTestService(defaultService, testConfig(),
                                     defaultError));
        const uint16_t defaultPort = defaultService.boundPort();
        const RoomId defaultRoom = testRoom(601);
        PairedRoom defaultPaired;
        QNP_REQUIRE(pairRoom(defaultService, defaultPort, defaultRoom,
                             defaultPaired));
        TcpClient defaultAuthority;
        qnet::relay::TopologyWelcome defaultAuthorityWelcome{};
        QNP_REQUIRE(registerTopologyAuthority(
            defaultService, defaultPort, defaultRoom,
            defaultPaired.hostWelcome.spectatorRegistrationToken, sessionId,
            defaultAuthority, defaultAuthorityWelcome));
        QNP_CHECK(defaultAuthorityWelcome.status
                  == qnet::relay::TopologyStatus::Accepted);
        TcpClient foreignNode;
        qnet::relay::TopologyWelcome foreignWelcome{};
        QNP_REQUIRE(admitTopologyNode(
            defaultService, defaultPort, defaultRoom,
            defaultAuthorityWelcome, 0x9101u, 0x81u, sessionId, foreignNode,
            foreignWelcome));
        QNP_CHECK(foreignWelcome.status
                  == qnet::relay::TopologyStatus::GrantRejected);
        TcpClient realNode;
        qnet::relay::TopologyWelcome realWelcome{};
        QNP_REQUIRE(admitTopologyNode(
            defaultService, defaultPort, defaultRoom,
            defaultAuthorityWelcome, 0x9102u, 0x82u, sessionId, realNode,
            realWelcome, false, 0, 0, 0, true));
        QNP_CHECK(realWelcome.status
                  == qnet::relay::TopologyStatus::Waiting);
        QNP_CHECK(realWelcome.nodeId == 0x82u);
        defaultService.requestStop();
        defaultService.drain();
    }

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_topology_data_plane_forwards_only_validated_qta) {
    resetClocks();
    TestTopologyVerifier topologyVerifier;
    RelayService service;
    service.setTopologyGrantVerifier(&topologyVerifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, testConfig(), error));
    const uint16_t port = service.boundPort();
    const RoomId room = testRoom(610);
    PairedRoom paired;
    QNP_REQUIRE(pairRoom(service, port, room, paired));
    const uint64_t sessionId = 0x5151515151515151ull;
    const uint64_t nodeId = 0x71u;

    TcpClient authority;
    qnet::relay::TopologyWelcome authorityWelcome{};
    QNP_REQUIRE(registerTopologyAuthority(
        service, port, room,
        paired.hostWelcome.spectatorRegistrationToken, sessionId, authority,
        authorityWelcome));
    QNP_REQUIRE(authorityWelcome.status
                == qnet::relay::TopologyStatus::Accepted);
    TcpClient node;
    qnet::relay::TopologyWelcome nodeWelcome{};
    QNP_REQUIRE(admitTopologyNode(service, port, room, authorityWelcome,
                                  0x9002u, nodeId, sessionId, node,
                                  nodeWelcome));
    QNP_REQUIRE(nodeWelcome.status == qnet::relay::TopologyStatus::Waiting);

    UdpClient authorityUdp;
    UdpClient nodeUdp;
    QNP_REQUIRE(authorityUdp.open());
    QNP_REQUIRE(nodeUdp.open());
    QNP_REQUIRE(bindTopologyUdp(service, port, room,
                                qnet::relay::TopologyParty::Authority,
                                authorityWelcome.authorityGeneration,
                                authorityWelcome.registrationToken, 0u,
                                authorityUdp));
    QNP_REQUIRE(bindTopologyUdp(service, port, room,
                                qnet::relay::TopologyParty::Node,
                                nodeWelcome.authorityGeneration,
                                nodeWelcome.registrationToken, nodeId,
                                nodeUdp));

    {
        std::vector<uint8_t> datagram;
        bool nodeSawReady = false;
        for (int attempt = 0; attempt < 16 && !nodeSawReady; ++attempt) {
            if (!nodeUdp.receive(service, datagram, 8)) continue;
            qnet::relay::TopologyReady ready{};
            nodeSawReady = qnet::relay::decodeTopologyReady(
                               datagram.data(), datagram.size(), ready)
                    == qnet::relay::DecodeError::None
                && ready.peer.port == authorityUdp.port();
        }
        QNP_CHECK(nodeSawReady);
    }

    const std::vector<uint8_t> envelope = qtaEnvelope(nodeId);
    QNP_REQUIRE(!envelope.empty());
    const std::vector<uint8_t> upFrame = topologyFrame(
        qnet::relay::TopologyParty::Node, nodeId,
        nodeWelcome.authorityGeneration, envelope);
    QNP_REQUIRE(!upFrame.empty());
    QNP_REQUIRE(nodeUdp.sendTo(port, upFrame));
    std::vector<uint8_t> forwarded;
    QNP_REQUIRE(receiveForwardedTopologyFrame(service, authorityUdp,
                                              forwarded));
    QNP_CHECK(forwarded == upFrame);

    const std::vector<uint8_t> downFrame = topologyFrame(
        qnet::relay::TopologyParty::Authority, nodeId,
        authorityWelcome.authorityGeneration, envelope);
    QNP_REQUIRE(authorityUdp.sendTo(port, downFrame));
    QNP_REQUIRE(receiveForwardedTopologyFrame(service, nodeUdp, forwarded));
    QNP_CHECK(forwarded == downFrame);

    const RelayServiceMetrics beforeCorrupt = service.snapshot();
    std::vector<uint8_t> corrupted = upFrame;
    corrupted.back() ^= 0x01u;
    QNP_REQUIRE(nodeUdp.sendTo(port, corrupted));
    QNP_CHECK(!receiveForwardedTopologyFrame(service, authorityUdp,
                                             forwarded, 2));
    QNP_CHECK(service.snapshot().drops > beforeCorrupt.drops);

    const std::vector<uint8_t> foreignEnvelope = qtaEnvelope(0x99u);
    const std::vector<uint8_t> foreignFrame = topologyFrame(
        qnet::relay::TopologyParty::Node, nodeId,
        nodeWelcome.authorityGeneration, foreignEnvelope);
    const RelayServiceMetrics beforeForeign = service.snapshot();
    QNP_REQUIRE(nodeUdp.sendTo(port, foreignFrame));
    QNP_CHECK(!receiveForwardedTopologyFrame(service, authorityUdp,
                                             forwarded, 2));
    QNP_CHECK(service.snapshot().drops > beforeForeign.drops);

    const RelayServiceMetrics beforeStale = service.snapshot();
    const std::vector<uint8_t> staleFrame = topologyFrame(
        qnet::relay::TopologyParty::Node, nodeId,
        nodeWelcome.authorityGeneration + 1u, envelope);
    QNP_REQUIRE(!staleFrame.empty());
    QNP_REQUIRE(nodeUdp.sendTo(port, staleFrame));
    QNP_CHECK(!receiveForwardedTopologyFrame(service, authorityUdp,
                                             forwarded, 2));
    QNP_CHECK(service.snapshot().drops > beforeStale.drops);

    UdpClient playerUdp;
    QNP_REQUIRE(playerUdp.open());
    QNP_REQUIRE(playerUdp.sendTo(
        port, registerRecord(room, RelayRole::Client,
                             paired.clientWelcome.registrationToken)));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    const RelayServiceMetrics beforeCross = service.snapshot();
    QNP_REQUIRE(playerUdp.sendTo(port, upFrame));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(service.snapshot().crossLaneRejects
              > beforeCross.crossLaneRejects);

    const RelayServiceMetrics beforeSpectator = service.snapshot();
    const std::vector<uint8_t> spectatorish = spectatorFrame(
        SpectatorParty::Viewer, nodeId, 1u, envelope);
    QNP_REQUIRE(!spectatorish.empty());
    QNP_REQUIRE(nodeUdp.sendTo(port, spectatorish));
    for (int turn = 0; turn < 8; ++turn) service.pumpOnce(0);
    QNP_CHECK(service.snapshot().crossLaneRejects
              > beforeSpectator.crossLaneRejects);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_topology_teardown_releases_nodes_and_spares_other_lanes) {
    resetClocks();
    TestVerifier spectatorVerifier;
    TestTopologyVerifier topologyVerifier;
    RelayService service;
    service.setSpectatorGrantVerifier(&spectatorVerifier);
    service.setTopologyGrantVerifier(&topologyVerifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, testConfig(), error));
    const uint16_t port = service.boundPort();
    const RoomId room = testRoom(620);
    PairedRoom paired;
    QNP_REQUIRE(pairRoom(service, port, room, paired));
    const uint64_t sessionId = 0x5151515151515151ull;
    const uint64_t nodeId = 0x71u;

    TcpClient source;
    SpectatorWelcome sourceWelcome{};
    QNP_REQUIRE(registerSource(
        service, port, room,
        paired.hostWelcome.spectatorRegistrationToken, sessionId, source,
        sourceWelcome));
    QNP_REQUIRE(sourceWelcome.status == SpectatorStatus::Accepted);
    TcpClient viewer;
    SpectatorWelcome viewerWelcome{};
    QNP_REQUIRE(admitViewer(service, port, room, sourceWelcome, 0x8001u,
                            0x51u, sessionId, viewer, viewerWelcome));
    QNP_REQUIRE(viewerWelcome.status == SpectatorStatus::Waiting
                || viewerWelcome.status == SpectatorStatus::Accepted);

    TcpClient authority;
    qnet::relay::TopologyWelcome authorityWelcome{};
    QNP_REQUIRE(registerTopologyAuthority(
        service, port, room,
        paired.hostWelcome.spectatorRegistrationToken, sessionId, authority,
        authorityWelcome));
    QNP_REQUIRE(authorityWelcome.status
                == qnet::relay::TopologyStatus::Accepted);
    TcpClient node;
    qnet::relay::TopologyWelcome nodeWelcome{};
    QNP_REQUIRE(admitTopologyNode(service, port, room, authorityWelcome,
                                  0x9002u, nodeId, sessionId, node,
                                  nodeWelcome));
    UdpClient authorityUdp;
    UdpClient nodeUdp;
    QNP_REQUIRE(authorityUdp.open());
    QNP_REQUIRE(nodeUdp.open());
    QNP_REQUIRE(bindTopologyUdp(service, port, room,
                                qnet::relay::TopologyParty::Authority,
                                authorityWelcome.authorityGeneration,
                                authorityWelcome.registrationToken, 0u,
                                authorityUdp));
    QNP_REQUIRE(bindTopologyUdp(service, port, room,
                                qnet::relay::TopologyParty::Node,
                                nodeWelcome.authorityGeneration,
                                nodeWelcome.registrationToken, nodeId,
                                nodeUdp));
    QNP_CHECK(service.snapshot().topologyRooms == 1u);
    QNP_CHECK(service.snapshot().topologyNodes == 1u);

    const RelayServiceMetrics before = service.snapshot();
    authority.close();
    bool nodeSawDetach = false;
    for (int attempt = 0; attempt < 32 && !nodeSawDetach; ++attempt) {
        std::vector<uint8_t> datagram;
        if (!nodeUdp.receive(service, datagram, 8)) continue;
        qnet::relay::TopologyDetach detach{};
        nodeSawDetach = qnet::relay::decodeTopologyDetach(
                            datagram.data(), datagram.size(), detach)
                == qnet::relay::DecodeError::None
            && detach.reason
                == qnet::relay::TopologyDetachReason::AuthorityLost
            && detach.nodeId == nodeId;
    }
    QNP_CHECK(nodeSawDetach);
    QNP_CHECK(node.closedByPeer(service));
    QNP_CHECK(service.snapshot().topologyRooms == 0u);
    QNP_CHECK(service.snapshot().topologyNodes == 0u);
    QNP_CHECK(service.snapshot().topologyNodesDetached
              == before.topologyNodesDetached + 1u);

    QNP_CHECK(service.snapshot().spectatorRooms == 1u);
    QNP_CHECK(service.snapshot().viewers == 1u);

    service.requestStop();
    service.drain();
}

QNP_TEST(relay_topology_udp_admission_serves_tcp_less_nodes) {
    resetClocks();
    TestTopologyVerifier topologyVerifier;
    RelayService service;
    service.setTopologyGrantVerifier(&topologyVerifier);
    std::string error;
    QNP_REQUIRE(startTestService(service, testConfig(), error));
    const uint16_t port = service.boundPort();
    const RoomId room = testRoom(630);
    PairedRoom paired;
    QNP_REQUIRE(pairRoom(service, port, room, paired));
    const uint64_t sessionId = 0x5151515151515151ull;
    const uint64_t nodeId = 0x91u;

    TcpClient authority;
    qnet::relay::TopologyWelcome authorityWelcome{};
    QNP_REQUIRE(registerTopologyAuthority(
        service, port, room,
        paired.hostWelcome.spectatorRegistrationToken, sessionId, authority,
        authorityWelcome));
    QNP_REQUIRE(authorityWelcome.status
                == qnet::relay::TopologyStatus::Accepted);

    const auto nodeHelloBytes = [&](const uint64_t grantId,
                                    const uint64_t targetNode,
                                    const bool corruptMac) {
        qnet::relay::TopologyHello hello{};
        hello.room = room;
        hello.party = qnet::relay::TopologyParty::Node;
        hello.admission = qnet::relay::TopologyAdmission::Grant;
        hello.sessionId = sessionId;
        hello.grantId = grantId;
        hello.nodeId = targetNode;
        hello.expiryUnix = g_wallUnix + 60ull;
        hello.authorityGeneration = authorityWelcome.authorityGeneration;
        TopologyGrantRequest request{};
        request.key = authorityWelcome.grantKey.data();
        request.keyLength = authorityWelcome.grantKey.size();
        request.grantId = hello.grantId;
        request.nodeId = hello.nodeId;
        request.sessionId = hello.sessionId;
        request.expiryUnix = hello.expiryUnix;
        request.authorityGeneration = hello.authorityGeneration;
        hello.macLow = expectedTopologyMacLow(request);
        if (corruptMac) hello.macLow ^= 0x40ull;
        hello.macHigh = 0x5678u;
        std::vector<uint8_t> encoded;
        qnet::relay::encodeTopologyHello(hello, encoded);
        return encoded;
    };

    UdpClient nodeUdp;
    QNP_REQUIRE(nodeUdp.open());
    const std::vector<uint8_t> hello = nodeHelloBytes(0x9201u, nodeId, false);
    QNP_REQUIRE(!hello.empty());
    QNP_REQUIRE(nodeUdp.sendTo(port, hello));

    const auto receiveWelcome = [&](qnet::relay::TopologyWelcome& out) {
        for (int attempt = 0; attempt < 16; ++attempt) {
            std::vector<uint8_t> datagram;
            if (!nodeUdp.receive(service, datagram, 8)) continue;
            if (qnet::relay::decodeTopologyWelcome(
                    datagram.data(), datagram.size(), out)
                == qnet::relay::DecodeError::None) {
                return true;
            }
        }
        return false;
    };

    qnet::relay::TopologyWelcome nodeWelcome{};
    QNP_REQUIRE(receiveWelcome(nodeWelcome));
    QNP_CHECK(nodeWelcome.status == qnet::relay::TopologyStatus::Waiting);
    QNP_CHECK(nodeWelcome.nodeId == nodeId);
    QNP_CHECK(qnet::relay::isValidId(nodeWelcome.registrationToken));
    QNP_CHECK(nodeWelcome.grantKey.empty());
    QNP_CHECK(service.snapshot().topologyNodes == 1u);

    QNP_REQUIRE(nodeUdp.sendTo(port, hello));
    qnet::relay::TopologyWelcome retransmitted{};
    QNP_REQUIRE(receiveWelcome(retransmitted));
    QNP_CHECK(retransmitted.status == nodeWelcome.status);
    QNP_CHECK(retransmitted.registrationToken
              == nodeWelcome.registrationToken);
    QNP_CHECK(service.snapshot().topologyNodes == 1u);

    UdpClient authorityUdp;
    QNP_REQUIRE(authorityUdp.open());
    QNP_REQUIRE(bindTopologyUdp(service, port, room,
                                qnet::relay::TopologyParty::Authority,
                                authorityWelcome.authorityGeneration,
                                authorityWelcome.registrationToken, 0u,
                                authorityUdp));
    QNP_REQUIRE(bindTopologyUdp(service, port, room,
                                qnet::relay::TopologyParty::Node,
                                nodeWelcome.authorityGeneration,
                                nodeWelcome.registrationToken, nodeId,
                                nodeUdp));
    const std::vector<uint8_t> envelope = qtaEnvelope(nodeId);
    QNP_REQUIRE(!envelope.empty());
    const std::vector<uint8_t> frame = topologyFrame(
        qnet::relay::TopologyParty::Node, nodeId,
        nodeWelcome.authorityGeneration, envelope);
    QNP_REQUIRE(nodeUdp.sendTo(port, frame));
    std::vector<uint8_t> forwarded;
    QNP_REQUIRE(receiveForwardedTopologyFrame(service, authorityUdp,
                                              forwarded));
    QNP_CHECK(forwarded == frame);

    const RelayServiceMetrics before = service.snapshot();
    UdpClient forgedUdp;
    QNP_REQUIRE(forgedUdp.open());
    const std::vector<uint8_t> forged =
        nodeHelloBytes(0x9202u, 0x92u, true);
    QNP_REQUIRE(forgedUdp.sendTo(port, forged));
    bool refused = false;
    for (int attempt = 0; attempt < 16 && !refused; ++attempt) {
        std::vector<uint8_t> datagram;
        if (!forgedUdp.receive(service, datagram, 8)) continue;
        qnet::relay::TopologyWelcome welcome{};
        refused = qnet::relay::decodeTopologyWelcome(
                      datagram.data(), datagram.size(), welcome)
                == qnet::relay::DecodeError::None
            && welcome.status == qnet::relay::TopologyStatus::GrantRejected;
    }
    QNP_CHECK(refused);
    QNP_CHECK(service.snapshot().topologyNodes == before.topologyNodes);
    QNP_CHECK(service.snapshot().topologyGrantRejects
              > before.topologyGrantRejects);

    {
        qnet::relay::TopologyHello authorityHello{};
        authorityHello.room = room;
        authorityHello.party = qnet::relay::TopologyParty::Authority;
        authorityHello.admission = qnet::relay::TopologyAdmission::None;
        authorityHello.sessionId = sessionId;
        authorityHello.authorityToken =
            paired.hostWelcome.spectatorRegistrationToken;
        std::vector<uint8_t> encoded;
        QNP_REQUIRE(qnet::relay::encodeTopologyHello(authorityHello,
                                                     encoded));
        UdpClient impostor;
        QNP_REQUIRE(impostor.open());
        QNP_REQUIRE(impostor.sendTo(port, encoded));
        std::vector<uint8_t> answer;
        QNP_CHECK(!impostor.receive(service, answer, 8));
    }

    service.requestStop();
    service.drain();
}
