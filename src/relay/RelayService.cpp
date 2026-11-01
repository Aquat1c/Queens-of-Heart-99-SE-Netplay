#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <bcrypt.h>

#include "RelayService.h"

#include "RelayServerCore.h"
#include "../net/Protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#if defined(__has_include)
#  if __has_include("../spectator/SpectatorGrant.h")
#    define QRELAY_HAS_SPECTATOR_GRANT 1
#  endif
#endif
#ifndef QRELAY_HAS_SPECTATOR_GRANT
#  define QRELAY_HAS_SPECTATOR_GRANT 0
#endif
#if QRELAY_HAS_SPECTATOR_GRANT
#  include "../spectator/SpectatorGrant.h"
#endif

#if defined(__has_include)
#  if __has_include("../spectator/TopologyGrant.h")
#    define QRELAY_HAS_TOPOLOGY_GRANT 1
#  endif
#endif
#ifndef QRELAY_HAS_TOPOLOGY_GRANT
#  define QRELAY_HAS_TOPOLOGY_GRANT 0
#endif
#if QRELAY_HAS_TOPOLOGY_GRANT
#  include "../spectator/TopologyGrant.h"
#endif

namespace qrelay {
namespace {

using qnet::relay::Id128;
using qnet::relay::Ipv4Endpoint;
using qnet::relay::LaneDomain;
using qnet::relay::Ready;
using qnet::relay::Register;
using qnet::relay::RegistrationToken;
using qnet::relay::RelayRole;
using qnet::relay::RoomId;
using qnet::relay::SpectatorDetachReason;
using qnet::relay::SpectatorAdmission;
using qnet::relay::SpectatorFrameHeader;
using qnet::relay::SpectatorParty;
using qnet::relay::SpectatorRouteKind;
using qnet::relay::SpectatorStatus;
using qnet::relay::TopologyAdmission;
using qnet::relay::TopologyDetachReason;
using qnet::relay::TopologyFrameHeader;
using qnet::relay::TopologyParty;
using qnet::relay::TopologyStatus;
using qnet::relay::Welcome;
using qnet::relay::WelcomeStatus;
using qnetplay::relay::server::PlayerDatagramError;
using qnetplay::relay::server::PlayerDatagramInfo;
using qnetplay::relay::server::SpectatorDatagramError;
using qnetplay::relay::server::SpectatorDatagramInfo;
using qnetplay::relay::server::TokenBucket;
using qnetplay::relay::server::TopologyDatagramError;
using qnetplay::relay::server::TopologyDatagramInfo;

namespace qsp = qnetplay::spectator;

constexpr ULONG kTcpKeepAliveIdleMs = 30'000;
constexpr ULONG kTcpKeepAliveIntervalMs = 10'000;
constexpr size_t kMaximumPrePairBytes = 64 * 1024;
constexpr size_t kMaximumQueuedTcpBytes = 256 * 1024;
constexpr uint64_t kTcpByteBurstMultiplier = 2;
constexpr size_t kSocketReadBuffer = 32 * 1024;
constexpr uint64_t kCleanupIntervalMs = 250;
constexpr size_t kLatencySamples = 256;
constexpr size_t kMaximumUsedGrantIds = 4096;
constexpr size_t kMaximumSpectatorUdpFrameBytes =
    qnet::relay::kSpectatorFrameHeaderSize
    + qsp::kMaxSpectatorDatagramSize;
constexpr size_t kMaximumTopologyUdpFrameBytes =
    qnet::relay::kTopologyFrameHeaderSize
    + qsp::resilient::wire::kTopologyQtaEnvelopePrefixBytes
    + qsp::resilient::wire::kMaxResilientControlDatagramBytes;
constexpr size_t kMaximumUdpFrameBytes =
    kMaximumSpectatorUdpFrameBytes > kMaximumTopologyUdpFrameBytes
        ? kMaximumSpectatorUdpFrameBytes
        : kMaximumTopologyUdpFrameBytes;

uint64_t defaultMonotonicMs() {
    return GetTickCount64();
}

uint64_t defaultWallUnixSeconds() {
    FILETIME filetime{};
    GetSystemTimeAsFileTime(&filetime);
    ULARGE_INTEGER value{};
    value.LowPart = filetime.dwLowDateTime;
    value.HighPart = filetime.dwHighDateTime;
    constexpr uint64_t kUnixEpochTicks = 116444736000000000ull;
    if (value.QuadPart < kUnixEpochTicks) return 0;
    return (value.QuadPart - kUnixEpochTicks) / 10000000ull;
}

uint64_t microsecondsNow() {
    LARGE_INTEGER frequency{};
    LARGE_INTEGER counter{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0
        || !QueryPerformanceCounter(&counter)) {
        return 0;
    }
    const int64_t whole = counter.QuadPart / frequency.QuadPart;
    const int64_t remainder = counter.QuadPart % frequency.QuadPart;
    return static_cast<uint64_t>(
        whole * 1000000 + (remainder * 1000000) / frequency.QuadPart);
}

bool setNonBlocking(const SOCKET socket) {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

bool idIsZero(const Id128& id) {
    return id.low == 0 && id.high == 0;
}

bool sameId(const Id128& left, const Id128& right) {
    const std::array<uint64_t, 2> a{ left.low, left.high };
    const std::array<uint64_t, 2> b{ right.low, right.high };
    return qnetplay::relay::server::constantTimeEqual(
        a.data(), b.data(), sizeof(a));
}

struct IdLess {
    bool operator()(const Id128& left, const Id128& right) const {
        if (left.high != right.high) return left.high < right.high;
        return left.low < right.low;
    }
};

struct EndpointKey {
    uint32_t ipv4 = 0;
    uint16_t port = 0;
};

struct EndpointLess {
    bool operator()(const EndpointKey& left, const EndpointKey& right) const {
        if (left.ipv4 != right.ipv4) return left.ipv4 < right.ipv4;
        return left.port < right.port;
    }
};

EndpointKey endpointKey(const sockaddr_in& endpoint) {
    return EndpointKey{
        endpoint.sin_addr.s_addr,
        ntohs(endpoint.sin_port),
    };
}

bool sameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_addr.s_addr == right.sin_addr.s_addr
        && left.sin_port == right.sin_port;
}

Ipv4Endpoint protocolEndpoint(const sockaddr_in& endpoint) {
    Ipv4Endpoint out{};
    out.ipv4 = endpoint.sin_addr.s_addr;
    out.port = ntohs(endpoint.sin_port);
    return out;
}

std::string endpointText(const sockaddr_in& endpoint) {
    char address[INET_ADDRSTRLEN] = {};
    InetNtopA(
        AF_INET,
        const_cast<IN_ADDR*>(&endpoint.sin_addr),
        address,
        static_cast<DWORD>(sizeof(address)));
    char result[64] = {};
    std::snprintf(
        result,
        sizeof(result),
        "%s:%u",
        address[0] != '\0' ? address : "?",
        static_cast<unsigned>(ntohs(endpoint.sin_port)));
    return result;
}

const char* roleText(const RelayRole role) {
    return role == RelayRole::Host ? "host" : "client";
}

size_t roleIndex(const RelayRole role) {
    return role == RelayRole::Host ? 0u : 1u;
}

RelayRole oppositeRole(const RelayRole role) {
    return role == RelayRole::Host ? RelayRole::Client : RelayRole::Host;
}

qnetplay::wire::PeerRole playerRole(const RelayRole role) {
    return role == RelayRole::Host
        ? qnetplay::wire::PeerRole::Host
        : qnetplay::wire::PeerRole::Client;
}

class DefaultGrantVerifier final : public SpectatorGrantVerifier {
public:
    GrantVerdict verify(const SpectatorGrantRequest& request) const override {
        if (request.key == nullptr
            || request.keyLength != qnet::relay::kGrantKeyBytes) {
            return GrantVerdict::Malformed;
        }
        if (request.grantId == 0 || request.viewerId == 0
            || request.sessionId == 0 || request.expiryUnix == 0
            || (request.macLow == 0 && request.macHigh == 0)) {
            return GrantVerdict::Malformed;
        }
#if QRELAY_HAS_SPECTATOR_GRANT
        qnetplay::spectator::SpectatorGrant grant{};
        grant.grantId = static_cast<decltype(grant.grantId)>(request.grantId);
        grant.viewerId =
            static_cast<decltype(grant.viewerId)>(request.viewerId);
        grant.sessionId =
            static_cast<decltype(grant.sessionId)>(request.sessionId);
        grant.expiryUnix =
            static_cast<decltype(grant.expiryUnix)>(request.expiryUnix);
        grant.sourceGeneration =
            static_cast<decltype(grant.sourceGeneration)>(
                request.sourceGeneration);
        grant.resumeRecord =
            static_cast<decltype(grant.resumeRecord)>(request.resumeRecord);
        grant.resumeDigest =
            static_cast<decltype(grant.resumeDigest)>(request.resumeDigest);
        grant.macLow = static_cast<decltype(grant.macLow)>(request.macLow);
        grant.macHigh = static_cast<decltype(grant.macHigh)>(request.macHigh);
        grant.route = static_cast<decltype(grant.route)>(request.route);

        qnetplay::spectator::SpectatorGrantKey key{};
        std::memcpy(key.data(), request.key, key.size());
        const auto verdict = qnetplay::spectator::verifyGrant(
            key, grant, request.viewerId, request.sessionId,
            static_cast<uint64_t>(std::time(nullptr)));
        switch (verdict) {
            case qnetplay::spectator::GrantVerdict::Ok:
                return GrantVerdict::Accepted;
            case qnetplay::spectator::GrantVerdict::Malformed:
                return GrantVerdict::Malformed;
            default:
                return GrantVerdict::BadMac;
        }
#else
        return GrantVerdict::Unsupported;
#endif
    }
};

class DefaultTopologyGrantVerifier final : public TopologyGrantVerifier {
public:
    GrantVerdict verify(const TopologyGrantRequest& request) const override {
        if (request.key == nullptr
            || request.keyLength != qnet::relay::kGrantKeyBytes) {
            return GrantVerdict::Malformed;
        }
        if (request.grantId == 0 || request.nodeId == 0
            || request.sessionId == 0 || request.expiryUnix == 0
            || request.authorityGeneration == 0
            || (request.macLow == 0 && request.macHigh == 0)) {
            return GrantVerdict::Malformed;
        }
#if QRELAY_HAS_TOPOLOGY_GRANT
        qnetplay::spectator::TopologyGrant grant{};
        grant.grantId = request.grantId;
        grant.nodeId = request.nodeId;
        grant.sessionId = request.sessionId;
        grant.expiryUnix = request.expiryUnix;
        grant.authorityGeneration = request.authorityGeneration;
        grant.macLow = request.macLow;
        grant.macHigh = request.macHigh;
        qnetplay::spectator::TopologyGrantKey key{};
        std::memcpy(key.data(), request.key, key.size());
        const auto verdict = qnetplay::spectator::verifyTopologyGrant(
            key, grant, request.nodeId, request.sessionId, request.nowUnix);
        switch (verdict) {
            case qnetplay::spectator::GrantVerdict::Ok:
                return GrantVerdict::Accepted;
            case qnetplay::spectator::GrantVerdict::Malformed:
                return GrantVerdict::Malformed;
            default:
                return GrantVerdict::BadMac;
        }
#else
        return GrantVerdict::Unsupported;
#endif
    }
};

class Logger {
public:
    ~Logger() { close(); }

    void configure(const std::string& path,
                   const uint64_t maximumTotalBytes,
                   const uint32_t files,
                   const bool console) {
        close();
        path_ = path;
        files_ = files == 0 ? 1u : files;
        perFileBytes_ = maximumTotalBytes / files_;
        if (perFileBytes_ == 0) perFileBytes_ = 1;
        console_ = console;
        currentBytes_ = 0;
        if (!path_.empty()) open();
    }

    void setSink(const LogSink sink, void* const user) {
        sink_ = sink;
        sinkUser_ = user;
    }

    void write(const LogLevel level, const char* const line) {
        if (sink_ != nullptr) sink_(sinkUser_, level, line);
        if (console_) {
            std::FILE* const stream =
                level == LogLevel::Error ? stderr : stdout;
            std::fprintf(stream, "%s\n", line);
        }
        if (handle_ == nullptr) return;
        const size_t length = std::strlen(line) + 1u;
        if (currentBytes_ + length > perFileBytes_) rotate();
        if (handle_ == nullptr) return;
        std::fprintf(handle_, "%s\n", line);
        std::fflush(handle_);
        currentBytes_ += length;
    }

private:
    void open() {
        handle_ = std::fopen(path_.c_str(), "ab");
        if (handle_ == nullptr) return;
        std::fseek(handle_, 0, SEEK_END);
        const long position = std::ftell(handle_);
        currentBytes_ = position > 0 ? static_cast<uint64_t>(position) : 0u;
    }

    void close() {
        if (handle_ != nullptr) {
            std::fclose(handle_);
            handle_ = nullptr;
        }
    }

    void rotate() {
        close();
        std::string oldest = path_ + "." + std::to_string(files_ - 1u);
        std::remove(oldest.c_str());
        for (uint32_t index = files_ - 1u; index >= 1u; --index) {
            const std::string from = index == 1u
                ? path_
                : path_ + "." + std::to_string(index - 1u);
            const std::string to = path_ + "." + std::to_string(index);
            std::remove(to.c_str());
            std::rename(from.c_str(), to.c_str());
            if (index == 1u) break;
        }
        currentBytes_ = 0;
        open();
    }

    std::string path_;
    uint32_t files_ = 4;
    uint64_t perFileBytes_ = 0;
    uint64_t currentBytes_ = 0;
    bool console_ = true;
    std::FILE* handle_ = nullptr;
    LogSink sink_ = nullptr;
    void* sinkUser_ = nullptr;
};

enum class Lane : uint8_t {
    Pending = 0,
    Player = 1,
    Spectator = 2,
    Topology = 3,
};

struct Connection {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in remote{};
    uint64_t acceptedMs = 0;
    uint64_t lastActivityMs = 0;
    bool joined = false;
    bool assignedRoom = false;
    bool closeAfterFlush = false;
    Lane lane = Lane::Pending;
    RoomId room{};
    RelayRole role = RelayRole::Host;
    SpectatorParty party = SpectatorParty::Viewer;
    uint64_t viewerId = 0;
    TopologyParty topologyParty = TopologyParty::Node;
    uint64_t nodeId = 0;
    std::vector<uint8_t> helloBytes;
    std::vector<uint8_t> prePairBytes;
    std::vector<uint8_t> sendBytes;
    size_t sendOffset = 0;
    TokenBucket tcpBytes;
    uint32_t turnBytes = 0;
};

struct PeerState {
    SOCKET tcpSocket = INVALID_SOCKET;
    RegistrationToken token{};
    bool udpRegistered = false;
    sockaddr_in udpEndpoint{};
    uint64_t lastUdpMs = 0;
    TokenBucket registrationRate;
    TokenBucket packetRate;
    TokenBucket byteRate;
};

struct ViewerState {
    uint64_t viewerId = 0;
    uint64_t grantId = 0;
    SOCKET tcpSocket = INVALID_SOCKET;
    uint32_t ipv4 = 0;
    RegistrationToken token{};
    bool udpRegistered = false;
    sockaddr_in udpEndpoint{};
    uint64_t admittedMs = 0;
    uint64_t lastActivityMs = 0;
    uint64_t expiryUnix = 0;
    uint32_t generation = 0;
    TokenBucket registrationRate;
    TokenBucket packetRate;
    TokenBucket byteRate;
    uint64_t bytesIn = 0;
    uint64_t bytesOut = 0;
};

struct SpectatorLane {
    bool sourceRegistered = false;
    SOCKET sourceTcpSocket = INVALID_SOCKET;
    uint32_t sourceIpv4 = 0;
    RegistrationToken sourceToken{};
    std::array<uint8_t, qnet::relay::kGrantKeyBytes> grantKey{};
    uint32_t generation = 0;
    uint64_t sessionId = 0;
    bool identityBound = false;
    qsp::SessionIdentity identity{};
    bool sourceUdpRegistered = false;
    sockaddr_in sourceUdpEndpoint{};
    uint64_t sourceAdmittedMs = 0;
    uint64_t sourceLastUdpMs = 0;
    uint64_t lastActivityMs = 0;
    TokenBucket registrationRate;
    TokenBucket packetRate;
    TokenBucket byteRate;
    std::map<uint64_t, ViewerState> viewers;
    std::map<uint64_t, uint64_t> usedGrantIds;
    uint64_t bytesIn = 0;
    uint64_t bytesOut = 0;
    uint32_t turnPackets = 0;
    uint32_t turnBytes = 0;
};

struct NodeState {
    uint64_t nodeId = 0;
    uint64_t grantId = 0;
    SOCKET tcpSocket = INVALID_SOCKET;
    uint32_t ipv4 = 0;
    RegistrationToken token{};
    bool udpRegistered = false;
    sockaddr_in udpEndpoint{};
    uint64_t admittedMs = 0;
    uint64_t lastActivityMs = 0;
    uint64_t expiryUnix = 0;
    uint32_t generation = 0;
    TokenBucket registrationRate;
    TokenBucket packetRate;
    TokenBucket byteRate;
    uint64_t bytesIn = 0;
    uint64_t bytesOut = 0;
};

struct TopologyLane {
    bool authorityRegistered = false;
    SOCKET authorityTcpSocket = INVALID_SOCKET;
    uint32_t authorityIpv4 = 0;
    RegistrationToken authorityToken{};
    std::array<uint8_t, qnet::relay::kGrantKeyBytes> grantKey{};
    uint32_t generation = 0;
    uint64_t sessionId = 0;
    bool authorityUdpRegistered = false;
    sockaddr_in authorityUdpEndpoint{};
    uint64_t authorityAdmittedMs = 0;
    uint64_t authorityLastUdpMs = 0;
    uint64_t lastActivityMs = 0;
    TokenBucket registrationRate;
    TokenBucket packetRate;
    TokenBucket byteRate;
    std::map<uint64_t, NodeState> nodes;
    std::map<uint64_t, uint64_t> usedGrantIds;
    uint64_t bytesIn = 0;
    uint64_t bytesOut = 0;
    uint32_t turnPackets = 0;
    uint32_t turnBytes = 0;
};

struct Room {
    RoomId id{};
    uint64_t ordinal = 0;
    uint32_t tag = 0;
    std::array<PeerState, 2> peers{};
    RegistrationToken spectatorRegistrationToken{};
    RegistrationToken viewerAccessToken{};
    uint64_t createdMs = 0;
    uint64_t lastActivityMs = 0;
    bool paired = false;
    bool retainedSpectatorOnly = false;
    bool playerIdentityBound = false;
    uint64_t playerSessionId = 0;
    qnetplay::wire::SessionCookie playerCookie{};
    uint32_t ownerIpv4 = 0;
    uint32_t turnPackets = 0;
    uint32_t turnBytes = 0;
    SpectatorLane spectator;
    TopologyLane topology;
};

struct UdpBinding {
    RoomId room{};
    Lane lane = Lane::Player;
    RelayRole role = RelayRole::Host;
    SpectatorParty party = SpectatorParty::Viewer;
    uint64_t viewerId = 0;
    TopologyParty topologyParty = TopologyParty::Node;
    uint64_t nodeId = 0;
};

struct IpState {
    uint32_t connections = 0;
    uint32_t rooms = 0;
    uint32_t viewers = 0;
    uint32_t nodes = 0;
    TokenBucket connectionRate;
    bool rateInitialized = false;
    uint64_t lastSeenMs = 0;
};

}

struct RelayService::State {
    RelayServiceConfig config;
    bool configured = false;
    bool started = false;
    bool winsockStarted = false;
    bool draining = false;
    bool fatal = false;
    std::atomic<bool> stop{ false };

    SOCKET tcpListener = INVALID_SOCKET;
    SOCKET udpSocket = INVALID_SOCKET;
    sockaddr_in boundEndpoint{};
    uint16_t boundPort = 0;

    std::map<SOCKET, Connection> connections;
    std::map<RoomId, Room, IdLess> rooms;
    std::map<EndpointKey, UdpBinding, EndpointLess> udpBindings;
    std::map<uint32_t, IpState> ips;

    uint64_t (*monotonicMs)() = &defaultMonotonicMs;
    uint64_t (*wallUnixSeconds)() = &defaultWallUnixSeconds;
    const SpectatorGrantVerifier* verifier = nullptr;
    const TopologyGrantVerifier* topologyVerifier = nullptr;

    Logger logger;
    RelayServiceMetrics metrics;
    uint64_t startedMs = 0;
    uint64_t lastCleanupMs = 0;
    uint64_t lastMetricsMs = 0;
    uint64_t roomOrdinal = 0;
    uint64_t roomTagSalt = 0;
    uint32_t totalViewers = 0;
    uint32_t spectatorRooms = 0;
    uint32_t totalTopologyNodes = 0;
    uint32_t topologyRooms = 0;
    uint32_t unboundTurnPackets = 0;
    size_t turnCursor = 0;
    std::array<uint64_t, kLatencySamples> latency{};
    size_t latencyIndex = 0;
    size_t latencyCount = 0;

    void log(LogLevel level, const char* format, ...);
    std::string roomLabel(const Room& room) const;
    uint32_t roomTag(const RoomId& id) const;

    Connection* findConnection(SOCKET socket);
    Room* findRoom(const RoomId& id);
    PeerState* findPeer(Room& room, RelayRole role);

    IpState& ipState(uint32_t ipv4);
    void releaseIp(uint32_t ipv4, bool connection, bool room, bool viewer,
                   bool node = false);
    void pruneIdleIpStates(uint64_t now);

    bool generateToken(RegistrationToken& out);
    bool generateRoomTokens(Room& room);
    bool generateGrantKey(std::array<uint8_t, qnet::relay::kGrantKeyBytes>& out);

    bool appendSend(Connection& connection, const uint8_t* bytes, size_t length);
    bool sendControlRecord(Connection& connection,
                           const std::vector<uint8_t>& record);
    bool sendWelcome(Connection& connection,
                     WelcomeStatus status,
                     const RegistrationToken& registrationToken = {},
                     const RegistrationToken& spectatorRegistrationToken = {},
                     const RegistrationToken& viewerAccessToken = {});
    bool sendSpectatorWelcome(Connection& connection,
                              const qnet::relay::SpectatorWelcome& welcome);
    void sendSpectatorDetachTo(const sockaddr_in& endpoint,
                               const RoomId& room,
                               SpectatorParty party,
                               const RegistrationToken& token,
                               uint64_t viewerId,
                               uint32_t generation,
                               SpectatorDetachReason reason);

    bool sendTopologyWelcome(Connection& connection,
                             const qnet::relay::TopologyWelcome& welcome);
    void sendTopologyDetachTo(const sockaddr_in& endpoint,
                              const RoomId& room,
                              TopologyParty party,
                              const RegistrationToken& token,
                              uint64_t nodeId,
                              uint32_t generation,
                              TopologyDetachReason reason);

    void rejectConnection(Connection& connection,
                          const qnet::relay::Hello& hello,
                          WelcomeStatus status);
    void rejectSpectator(Connection& connection,
                         const qnet::relay::SpectatorHello& hello,
                         SpectatorStatus status);
    void rejectTopology(Connection& connection,
                        const qnet::relay::TopologyHello& hello,
                        TopologyStatus status);

    void acceptConnections(uint64_t now);
    void receiveUdp(uint64_t now);
    void receiveTcp(SOCKET socket);
    void flushTcp(SOCKET socket);
    void processHello(SOCKET socket, const qnet::relay::Hello& hello);
    void processSpectatorHello(SOCKET socket,
                               const qnet::relay::SpectatorHello& hello);
    void processProxyBytes(SOCKET socket, const uint8_t* bytes, size_t length);
    void pairRoom(Room& room, uint64_t now);
    Room* bindPlayerRegistration(const RoomId& roomId,
                                 RelayRole role,
                                 const RegistrationToken& token,
                                 const sockaddr_in& source,
                                 uint64_t now);
    void processRegister(const Register& registration,
                         const sockaddr_in& source,
                         uint64_t now);
    void processObservedRequest(const qnet::relay::ObservedRequest& request,
                                const sockaddr_in& source,
                                uint64_t now);
    void processSpectatorRegister(
        const qnet::relay::SpectatorRegister& registration,
        const sockaddr_in& source,
        uint64_t now);
    void processSpectatorDetach(const qnet::relay::SpectatorDetach& detach,
                                const sockaddr_in& source,
                                uint64_t now);
    void processTopologyHello(SOCKET socket,
                              const qnet::relay::TopologyHello& hello);
    TopologyStatus admitTopologyNode(Room& room,
                                     const qnet::relay::TopologyHello& hello,
                                     uint32_t ipv4,
                                     SOCKET tcpSocket,
                                     uint64_t now,
                                     qnet::relay::TopologyWelcome& welcome);
    void processTopologyHelloDatagram(
        const qnet::relay::TopologyHello& hello,
        const sockaddr_in& source,
        uint64_t now);
    void processTopologyRegister(
        const qnet::relay::TopologyRegister& registration,
        const sockaddr_in& source,
        uint64_t now);
    void processTopologyDetach(const qnet::relay::TopologyDetach& detach,
                               const sockaddr_in& source,
                               uint64_t now);
    void processTopologyFrame(const uint8_t* bytes,
                              size_t length,
                              const UdpBinding& binding,
                              const sockaddr_in& source,
                              uint64_t now);
    void sendTopologyReady(Room& room, NodeState& node);
    void detachTopologyNode(Room& room,
                            uint64_t nodeId,
                            TopologyDetachReason reason,
                            bool notify);
    void closeTopologyAuthority(Room& room,
                                TopologyDetachReason reason,
                                bool notify);
    void processPlayerDatagram(const uint8_t* bytes,
                               size_t length,
                               const UdpBinding& binding,
                               const sockaddr_in& source,
                               uint64_t now);
    void processSpectatorFrame(const uint8_t* bytes,
                               size_t length,
                               const UdpBinding& binding,
                               const sockaddr_in& source,
                               uint64_t now);
    void sendReady(Room& room, RelayRole recipient);
    void sendSpectatorReady(Room& room, ViewerState& viewer);

    void detachViewer(Room& room,
                      uint64_t viewerId,
                      SpectatorDetachReason reason,
                      bool notify);
    void closeSpectatorSource(Room& room,
                              SpectatorDetachReason reason,
                              bool notify);

    void cleanupExpired(uint64_t now);
    void closeConnection(SOCKET socket, const char* reason);
    void retainSpectatorOnly(Room& room, const char* reason);
    void closeSingle(SOCKET socket, const char* reason);
    void closeRoom(const RoomId& roomId, const char* reason);
    void shutdownSockets();
    void emitHealth(bool final);
    void recordLatency(uint64_t micros);
    uint64_t percentileLatency(unsigned percentile) const;
};

namespace {

DefaultGrantVerifier g_defaultVerifier;
DefaultTopologyGrantVerifier g_defaultTopologyVerifier;

}

const SpectatorGrantVerifier& defaultSpectatorGrantVerifier() {
    return g_defaultVerifier;
}

bool spectatorGrantVerifierAvailable() {
    return QRELAY_HAS_SPECTATOR_GRANT != 0;
}

const TopologyGrantVerifier& defaultTopologyGrantVerifier() {
    return g_defaultTopologyVerifier;
}

bool topologyGrantVerifierAvailable() {
    return QRELAY_HAS_TOPOLOGY_GRANT != 0;
}

const char* exitCodeText(const ExitCode code) {
    switch (code) {
        case ExitCode::Ok: return "ok";
        case ExitCode::Usage: return "usage";
        case ExitCode::BindFailure: return "bind failure";
        case ExitCode::ConfigInvalid: return "invalid configuration";
        case ExitCode::FatalRuntime: return "fatal runtime error";
    }
    return "unknown";
}

namespace {

bool requireRange(const uint64_t value,
                  const uint64_t low,
                  const uint64_t high,
                  const char* const name,
                  std::string& error) {
    if (value >= low && value <= high) return true;
    char text[192] = {};
    std::snprintf(
        text,
        sizeof(text),
        "%s must be between %llu and %llu (got %llu)",
        name,
        static_cast<unsigned long long>(low),
        static_cast<unsigned long long>(high),
        static_cast<unsigned long long>(value));
    error = text;
    return false;
}

}

bool validateRelayServiceConfig(const RelayServiceConfig& config,
                                std::string& error) {
    error.clear();
    if (config.bindAddress.empty()) {
        error = "--bind requires a numeric IPv4 address";
        return false;
    }
    in_addr probe{};
    if (InetPtonA(AF_INET, config.bindAddress.c_str(), &probe) != 1) {
        error = "--bind requires a numeric IPv4 address: "
            + config.bindAddress;
        return false;
    }
    if (config.port == 0) {
        error = "--port must be between 1 and 65535 (got 0)";
        return false;
    }
    if (!requireRange(config.maximumRooms, 1, 4096, "--max-rooms", error))
        return false;
    if (!requireRange(config.maximumConnectionsPerIp, 1, 1024,
                      "--max-connections-per-ip", error)) {
        return false;
    }
    if (!requireRange(config.maximumRoomsPerIp, 1, 4096,
                      "--max-rooms-per-ip", error)) {
        return false;
    }
    if (!requireRange(config.maximumSpectatorRooms, 0, 4096,
                      "--max-spectator-rooms", error)) {
        return false;
    }
    if (!requireRange(config.maximumViewersPerRoom, 1, 1024,
                      "--max-viewers-per-room", error)) {
        return false;
    }
    if (!requireRange(config.maximumViewers, 1, 65535,
                      "--max-viewers", error)) {
        return false;
    }
    if (!requireRange(config.maximumViewersPerIp, 1, 1024,
                      "--max-viewers-per-ip", error)) {
        return false;
    }
    if (config.maximumViewersPerRoom > config.maximumViewers) {
        error = "--max-viewers-per-room must not exceed --max-viewers";
        return false;
    }
    if (!requireRange(config.maximumGrantLifetimeSeconds, 1, 86400,
                      "--max-grant-lifetime", error)) {
        return false;
    }
    if (!requireRange(config.maximumTopologyRooms, 0, 4096,
                      "--max-topology-rooms", error)) {
        return false;
    }
    if (!requireRange(config.maximumTopologyNodesPerRoom, 1, 1024,
                      "--max-topology-nodes-per-room", error)) {
        return false;
    }
    if (!requireRange(config.maximumTopologyNodes, 1, 65535,
                      "--max-topology-nodes", error)) {
        return false;
    }
    if (!requireRange(config.maximumTopologyNodesPerIp, 1, 1024,
                      "--max-topology-nodes-per-ip", error)) {
        return false;
    }
    if (config.maximumTopologyNodesPerRoom > config.maximumTopologyNodes) {
        error = "--max-topology-nodes-per-room must not exceed "
                "--max-topology-nodes";
        return false;
    }
    if (!requireRange(config.tcpBytesPerSecond, 4096, 1ull << 30,
                      "--tcp-bytes-per-second", error)) {
        return false;
    }
    if (!requireRange(config.roomUdpBytesPerSecond, 4096, 1ull << 30,
                      "--room-kib", error)) {
        return false;
    }
    if (!requireRange(config.roomUdpPacketsPerSecond, 1, 1000000,
                      "--room-packets-per-second", error)) {
        return false;
    }
    if (!requireRange(config.viewerUdpBytesPerSecond, 4096, 1ull << 30,
                      "--viewer-kib", error)) {
        return false;
    }
    if (!requireRange(config.viewerUdpPacketsPerSecond, 1, 1000000,
                      "--viewer-packets-per-second", error)) {
        return false;
    }
    if (!requireRange(config.registrationsPerSecond, 1, 10000,
                      "--registrations-per-second", error)) {
        return false;
    }
    if (!requireRange(config.connectionsPerIpPerSecond, 1, 10000,
                      "--connections-per-ip-per-second", error)) {
        return false;
    }
    if (!requireRange(config.ipRateStateRetentionMs, 100, 3600000,
                      "ipRateStateRetentionMs", error)) {
        return false;
    }
    if (!requireRange(config.maximumTrackedIpStates, 1, 65536,
                      "maximumTrackedIpStates", error)) {
        return false;
    }
    if (!requireRange(config.maximumUdpPacketsPerTurn, 16, 65536,
                      "--udp-packets-per-turn", error)) {
        return false;
    }
    if (!requireRange(config.maximumUdpPacketsPerRoomPerTurn, 1, 65536,
                      "--room-packets-per-turn", error)) {
        return false;
    }
    if (config.maximumUdpPacketsPerRoomPerTurn
        > config.maximumUdpPacketsPerTurn) {
        error = "--room-packets-per-turn must not exceed "
            "--udp-packets-per-turn";
        return false;
    }
    if (!requireRange(config.maximumUdpBytesPerRoomPerTurn, 1024, 1u << 24,
                      "--room-bytes-per-turn", error)) {
        return false;
    }
    if (!requireRange(config.maximumUnboundPacketsPerTurn, 1, 65536,
                      "--unbound-packets-per-turn", error)) {
        return false;
    }
    if (!requireRange(config.maximumAcceptsPerTurn, 1, 4096,
                      "--accepts-per-turn", error)) {
        return false;
    }
    if (!requireRange(config.maximumTcpBytesPerConnectionPerTurn,
                      1024, 1u << 22, "--tcp-bytes-per-turn", error)) {
        return false;
    }
    if (!requireRange(config.pollIntervalMs, 1, 10000,
                      "--poll-interval-ms", error)) {
        return false;
    }
    if (!requireRange(config.joinTimeoutMs, 100, 600000,
                      "--join-timeout-ms", error)) {
        return false;
    }
    if (!requireRange(config.pairTimeoutMs, 100, 3600000,
                      "--pair-timeout-ms", error)) {
        return false;
    }
    if (!requireRange(config.registrationTimeoutMs, 100, 3600000,
                      "--registration-timeout-ms", error)) {
        return false;
    }
    if (!requireRange(config.spectatorRegistrationTimeoutMs, 100, 120000,
                      "--spectator-registration-timeout-ms", error)) {
        return false;
    }
    if (!requireRange(config.viewerIdleTimeoutMs, 1000, 3600000,
                      "--viewer-idle-ms", error)) {
        return false;
    }
    if (!requireRange(config.maximumRoomIdleMs, 1000,
                      48ull * 60ull * 60ull * 1000ull,
                      "--room-idle-ms", error)) {
        return false;
    }
    if (!requireRange(config.drainTimeoutMs, 0, 60000, "--drain-ms", error))
        return false;
    if (!requireRange(config.retryAfterMs, 0, 3600000,
                      "--retry-after-ms", error)) {
        return false;
    }
    if (config.metricsIntervalMs != 0
        && !requireRange(config.metricsIntervalMs, 1000, 3600000,
                         "--metrics-ms", error)) {
        return false;
    }
    if (!config.logPath.empty()) {
        if (!requireRange(config.logMaximumTotalBytes, 64ull * 1024ull,
                          4ull << 30, "--log-max-kib", error)) {
            return false;
        }
        if (!requireRange(config.logFiles, 2, 64, "--log-files", error))
            return false;
    }
    return true;
}

void RelayService::State::log(const LogLevel level,
                              const char* const format,
                              ...) {
    char line[512] = {};
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    logger.write(level, line);
}

uint32_t RelayService::State::roomTag(const RoomId& id) const {
    uint64_t mixed = id.low ^ (id.high * 0x9E3779B97F4A7C15ull) ^ roomTagSalt;
    mixed ^= mixed >> 33;
    mixed *= 0xFF51AFD7ED558CCDull;
    mixed ^= mixed >> 33;
    mixed *= 0xC4CEB9FE1A85EC53ull;
    mixed ^= mixed >> 33;
    return static_cast<uint32_t>(mixed);
}

std::string RelayService::State::roomLabel(const Room& room) const {
    char text[48] = {};
    std::snprintf(
        text,
        sizeof(text),
        "room #%llu/%08X",
        static_cast<unsigned long long>(room.ordinal),
        room.tag);
    return text;
}

Connection* RelayService::State::findConnection(const SOCKET socket) {
    const auto it = connections.find(socket);
    return it != connections.end() ? &it->second : nullptr;
}

Room* RelayService::State::findRoom(const RoomId& id) {
    const auto it = rooms.find(id);
    return it != rooms.end() ? &it->second : nullptr;
}

PeerState* RelayService::State::findPeer(Room& room, const RelayRole role) {
    return &room.peers[roleIndex(role)];
}

IpState& RelayService::State::ipState(const uint32_t ipv4) {
    IpState& state = ips[ipv4];
    if (!state.rateInitialized) {
        state.connectionRate.reset(
            config.connectionsPerIpPerSecond,
            config.connectionsPerIpPerSecond * 4u,
            monotonicMs());
        state.rateInitialized = true;
    }
    state.lastSeenMs = monotonicMs();
    return state;
}

void RelayService::State::releaseIp(const uint32_t ipv4,
                                    const bool connection,
                                    const bool room,
                                    const bool viewer,
                                    const bool node) {
    const auto it = ips.find(ipv4);
    if (it == ips.end()) return;
    if (connection && it->second.connections > 0) --it->second.connections;
    if (room && it->second.rooms > 0) --it->second.rooms;
    if (viewer && it->second.viewers > 0) --it->second.viewers;
    if (node && it->second.nodes > 0) --it->second.nodes;
    if (it->second.connections == 0 && it->second.rooms == 0
        && it->second.viewers == 0 && it->second.nodes == 0) {
        it->second.lastSeenMs = monotonicMs();
    }
}

void RelayService::State::pruneIdleIpStates(const uint64_t now) {
    for (auto it = ips.begin(); it != ips.end();) {
        const IpState& state = it->second;
        const bool inactive =
            state.connections == 0 && state.rooms == 0 && state.viewers == 0
            && state.nodes == 0;
        const uint64_t idleMs =
            now > state.lastSeenMs ? now - state.lastSeenMs : 0u;
        if (inactive
            && idleMs >= config.ipRateStateRetentionMs) {
            it = ips.erase(it);
        } else {
            ++it;
        }
    }
}

bool RelayService::State::generateToken(RegistrationToken& out) {
    out = {};
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::array<uint8_t, 16> random{};
        const NTSTATUS status = BCryptGenRandom(
            nullptr,
            random.data(),
            static_cast<ULONG>(random.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) return false;
        std::memcpy(&out.low, random.data(), sizeof(out.low));
        std::memcpy(&out.high, random.data() + sizeof(out.low),
                    sizeof(out.high));
        if (!idIsZero(out)) return true;
    }
    out = {};
    return false;
}

bool RelayService::State::generateGrantKey(
    std::array<uint8_t, qnet::relay::kGrantKeyBytes>& out) {
    const NTSTATUS status = BCryptGenRandom(
        nullptr,
        out.data(),
        static_cast<ULONG>(out.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) return false;
    for (const uint8_t byte : out) {
        if (byte != 0) return true;
    }
    return false;
}

bool RelayService::State::generateRoomTokens(Room& room) {
    std::array<RegistrationToken*, 4> tokens{
        &room.peers[0].token,
        &room.peers[1].token,
        &room.spectatorRegistrationToken,
        &room.viewerAccessToken,
    };
    for (size_t i = 0; i < tokens.size(); ++i) {
        bool unique = false;
        for (int attempt = 0; attempt < 8 && !unique; ++attempt) {
            if (!generateToken(*tokens[i])) return false;
            unique = true;
            for (size_t previous = 0; previous < i; ++previous) {
                if (sameId(*tokens[i], *tokens[previous])) {
                    unique = false;
                    break;
                }
            }
        }
        if (!unique) return false;
    }
    return true;
}

bool RelayService::State::appendSend(Connection& connection,
                                     const uint8_t* const bytes,
                                     const size_t length) {
    if (length == 0) return true;
    if (bytes == nullptr) return false;

    if (connection.sendOffset != 0
        && connection.sendOffset == connection.sendBytes.size()) {
        connection.sendBytes.clear();
        connection.sendOffset = 0;
    } else if (connection.sendOffset > 64 * 1024) {
        connection.sendBytes.erase(
            connection.sendBytes.begin(),
            connection.sendBytes.begin()
                + static_cast<std::ptrdiff_t>(connection.sendOffset));
        connection.sendOffset = 0;
    }
    const size_t pending = connection.sendBytes.size() - connection.sendOffset;
    if (length > kMaximumQueuedTcpBytes
        || pending > kMaximumQueuedTcpBytes - length) {
        return false;
    }
    connection.sendBytes.insert(
        connection.sendBytes.end(), bytes, bytes + length);
    return true;
}

bool RelayService::State::sendControlRecord(
    Connection& connection,
    const std::vector<uint8_t>& record) {
    std::array<uint8_t, qnet::kMaxMsgSize> message{};
    const size_t messageLength = qnet::packMsg(
        qnet::relay::kTcpEnvelopeType,
        record.data(),
        record.size(),
        message.data(),
        message.size());
    if (messageLength == 0 || messageLength > 0xFFFFu)
        return false;
    std::vector<uint8_t> frame(messageLength + 2u, 0u);
    frame[0] = static_cast<uint8_t>(messageLength);
    frame[1] = static_cast<uint8_t>(messageLength >> 8u);
    std::memcpy(frame.data() + 2u, message.data(), messageLength);
    return appendSend(connection, frame.data(), frame.size());
}

bool RelayService::State::sendWelcome(
    Connection& connection,
    const WelcomeStatus status,
    const RegistrationToken& registrationToken,
    const RegistrationToken& spectatorRegistrationToken,
    const RegistrationToken& viewerAccessToken) {
    Welcome welcome{};
    welcome.room = connection.room;
    welcome.domain = LaneDomain::Player;
    welcome.role = connection.role;
    welcome.status = status;
    welcome.registrationToken = registrationToken;
    welcome.spectatorRegistrationToken = spectatorRegistrationToken;
    welcome.viewerAccessToken = viewerAccessToken;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeWelcome(welcome, encoded))
        return false;
    return sendControlRecord(connection, encoded);
}

bool RelayService::State::sendSpectatorWelcome(
    Connection& connection,
    const qnet::relay::SpectatorWelcome& welcome) {
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeSpectatorWelcome(welcome, encoded))
        return false;
    return sendControlRecord(connection, encoded);
}

void RelayService::State::rejectConnection(
    Connection& connection,
    const qnet::relay::Hello& hello,
    const WelcomeStatus status) {
    connection.room = hello.room;
    connection.role = hello.role;
    connection.joined = true;
    connection.assignedRoom = false;
    ++metrics.rejects;
    if (!sendWelcome(connection, status))
        connection.sendBytes.clear();
    connection.closeAfterFlush = true;
}

void RelayService::State::rejectSpectator(
    Connection& connection,
    const qnet::relay::SpectatorHello& hello,
    const SpectatorStatus status) {
    connection.room = hello.room;
    connection.lane = Lane::Spectator;
    connection.party = hello.party;
    connection.joined = true;
    connection.assignedRoom = false;
    ++metrics.rejects;
    if (status == SpectatorStatus::CapacityReached)
        ++metrics.capacityRejects;
    qnet::relay::SpectatorWelcome welcome{};
    welcome.room = hello.room;
    welcome.party = hello.party;
    welcome.status = status;
    welcome.viewerId = hello.party == SpectatorParty::Viewer
        ? hello.viewerId
        : 0u;
    welcome.sessionId = hello.sessionId;
    welcome.retryAfterMs = config.retryAfterMs;
    welcome.viewerCapacity =
        static_cast<uint16_t>((std::min)(config.maximumViewersPerRoom,
                                         uint32_t{ 0xFFFFu }));
    if (!sendSpectatorWelcome(connection, welcome))
        connection.sendBytes.clear();
    if (config.verbose) {
        const Room* const room = findRoom(hello.room);
        log(LogLevel::Info,
            "[%s] spectator %s rejected status=%u session=%u from %s",
            room != nullptr ? roomLabel(*room).c_str() : "room-missing",
            hello.party == SpectatorParty::Source ? "source" : "viewer",
            static_cast<unsigned>(status),
            static_cast<unsigned>(hello.sessionId),
            endpointText(connection.remote).c_str());
    }
    connection.closeAfterFlush = true;
}

void RelayService::State::sendSpectatorDetachTo(
    const sockaddr_in& endpoint,
    const RoomId& room,
    const SpectatorParty party,
    const RegistrationToken& token,
    const uint64_t viewerId,
    const uint32_t generation,
    const SpectatorDetachReason reason) {
    if (udpSocket == INVALID_SOCKET || idIsZero(token)) return;
    qnet::relay::SpectatorDetach detach{};
    detach.room = room;
    detach.party = party;
    detach.reason = reason;
    detach.sourceGeneration = generation;
    detach.token = token;
    detach.viewerId = viewerId;
    detach.retryAfterMs = config.retryAfterMs;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeSpectatorDetach(detach, encoded)) return;
    (void)sendto(
        udpSocket,
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<int>(encoded.size()),
        0,
        reinterpret_cast<const sockaddr*>(&endpoint),
        sizeof(endpoint));
}

void RelayService::State::pairRoom(Room& room, const uint64_t now) {
    if (room.paired
        || room.peers[0].tcpSocket == INVALID_SOCKET
        || room.peers[1].tcpSocket == INVALID_SOCKET) {
        return;
    }
    if (!generateRoomTokens(room)) {
        const RoomId id = room.id;
        closeRoom(id, "secure token generation failed");
        return;
    }

    for (PeerState& peer : room.peers) {
        peer.registrationRate.reset(
            config.registrationsPerSecond,
            config.registrationsPerSecond * 2u,
            now);
        peer.packetRate.reset(
            config.roomUdpPacketsPerSecond,
            config.roomUdpPacketsPerSecond * 2u,
            now);
        peer.byteRate.reset(
            config.roomUdpBytesPerSecond,
            config.roomUdpBytesPerSecond * 2u,
            now);
    }
    room.paired = true;
    room.lastActivityMs = now;

    Connection* const host = findConnection(room.peers[0].tcpSocket);
    Connection* const client = findConnection(room.peers[1].tcpSocket);
    if (host == nullptr || client == nullptr
        || !sendWelcome(
            *host,
            WelcomeStatus::Accepted,
            room.peers[0].token,
            room.spectatorRegistrationToken,
            room.viewerAccessToken)
        || !sendWelcome(
            *client,
            WelcomeStatus::Accepted,
            room.peers[1].token)) {
        const RoomId id = room.id;
        closeRoom(id, "welcome queue overflow");
        return;
    }

    if (!appendSend(
            *client, host->prePairBytes.data(), host->prePairBytes.size())
        || !appendSend(
            *host, client->prePairBytes.data(), client->prePairBytes.size())) {
        const RoomId id = room.id;
        closeRoom(id, "pre-pair proxy queue overflow");
        return;
    }
    host->prePairBytes.clear();
    client->prePairBytes.clear();

    log(LogLevel::Info,
        "[%s] paired host=%s client=%s",
        roomLabel(room).c_str(),
        endpointText(host->remote).c_str(),
        endpointText(client->remote).c_str());
}

void RelayService::State::processHello(const SOCKET socket,
                                       const qnet::relay::Hello& hello) {
    Connection* connection = findConnection(socket);
    if (connection == nullptr) return;
    connection->room = hello.room;
    connection->role = hello.role;
    connection->joined = true;
    connection->lane = Lane::Player;

    if (hello.domain != LaneDomain::Player || idIsZero(hello.room)) {
        rejectConnection(*connection, hello, WelcomeStatus::Rejected);
        return;
    }

    Room* room = findRoom(hello.room);
    if (hello.role == RelayRole::Client && room == nullptr) {
        rejectConnection(*connection, hello, WelcomeStatus::RoomNotFound);
        return;
    }
    if (hello.role == RelayRole::Host && room == nullptr) {
        const uint32_t ipv4 = connection->remote.sin_addr.s_addr;
        if (rooms.size() >= config.maximumRooms) {
            rejectConnection(*connection, hello, WelcomeStatus::ServerFull);
            return;
        }
        if (ipState(ipv4).rooms >= config.maximumRoomsPerIp) {
            ++metrics.ipQuotaRejects;
            rejectConnection(*connection, hello, WelcomeStatus::ServerFull);
            return;
        }
        Room created{};
        created.id = hello.room;
        created.ordinal = ++roomOrdinal;
        created.tag = roomTag(hello.room);
        created.createdMs = connection->lastActivityMs;
        created.lastActivityMs = connection->lastActivityMs;
        created.ownerIpv4 = ipv4;
        const auto result = rooms.emplace(hello.room, std::move(created));
        room = &result.first->second;
        ++ipState(ipv4).rooms;
        ++metrics.roomsOpened;
    }
    if (room == nullptr || room->paired || room->retainedSpectatorOnly) {
        rejectConnection(*connection, hello, WelcomeStatus::RoomFull);
        return;
    }

    PeerState* const slot = findPeer(*room, hello.role);
    if (slot->tcpSocket != INVALID_SOCKET) {
        rejectConnection(*connection, hello, WelcomeStatus::RoomFull);
        return;
    }

    slot->tcpSocket = socket;
    connection->assignedRoom = true;
    room->lastActivityMs = connection->lastActivityMs;
    if (hello.role == RelayRole::Host) {
        if (!sendWelcome(*connection, WelcomeStatus::Waiting)) {
            const RoomId id = room->id;
            closeRoom(id, "waiting welcome queue overflow");
            return;
        }
    }
    pairRoom(*room, connection->lastActivityMs);
}

void RelayService::State::processProxyBytes(const SOCKET socket,
                                            const uint8_t* const bytes,
                                            const size_t length) {
    if (length == 0) return;
    Connection* const source = findConnection(socket);
    if (source == nullptr || !source->assignedRoom) return;
    if (source->lane == Lane::Spectator) {
        ++metrics.crossLaneRejects;
        closeSingle(socket, "spectator lane does not proxy TCP bytes");
        return;
    }
    Room* const room = findRoom(source->room);
    if (room == nullptr) return;

    if (!room->paired) {
        if (length > kMaximumPrePairBytes
            || source->prePairBytes.size()
                > kMaximumPrePairBytes - length) {
            const RoomId id = room->id;
            closeRoom(id, "pre-pair TCP buffer exceeded");
            return;
        }
        source->prePairBytes.insert(
            source->prePairBytes.end(), bytes, bytes + length);
        return;
    }

    const PeerState* const destinationPeer =
        findPeer(*room, oppositeRole(source->role));
    Connection* const destination =
        findConnection(destinationPeer->tcpSocket);
    if (destination == nullptr
        || !appendSend(*destination, bytes, length)) {
        const RoomId id = room->id;
        closeRoom(id, "TCP proxy queue exceeded");
        return;
    }
    metrics.tcpBytesProxied += length;
    room->lastActivityMs = source->lastActivityMs;
}

void RelayService::State::processSpectatorHello(
    const SOCKET socket,
    const qnet::relay::SpectatorHello& hello) {
    Connection* connection = findConnection(socket);
    if (connection == nullptr) return;
    connection->joined = true;
    connection->lane = Lane::Spectator;
    connection->room = hello.room;
    connection->party = hello.party;

    if (!config.spectatorLaneEnabled) {
        rejectSpectator(*connection, hello, SpectatorStatus::Unsupported);
        return;
    }
    const SpectatorGrantVerifier* const grantVerifier =
        verifier != nullptr ? verifier : &defaultSpectatorGrantVerifier();

    Room* const room = findRoom(hello.room);
    if (room == nullptr
        || (!room->paired && !room->retainedSpectatorOnly)) {
        rejectSpectator(*connection, hello, SpectatorStatus::RoomNotFound);
        return;
    }
    SpectatorLane& lane = room->spectator;
    const uint64_t now = connection->lastActivityMs;

    if (hello.party == SpectatorParty::Source) {
        if (!room->paired) {
            rejectSpectator(
                *connection, hello, SpectatorStatus::SourceUnavailable);
            return;
        }
        Connection* const hostTcp =
            findConnection(room->peers[roleIndex(RelayRole::Host)].tcpSocket);
        if (hostTcp == nullptr
            || hostTcp->remote.sin_addr.s_addr
                != connection->remote.sin_addr.s_addr
            || !sameId(room->spectatorRegistrationToken, hello.sourceToken)) {
            ++metrics.grantRejects;
            rejectSpectator(
                *connection, hello, SpectatorStatus::GrantRejected);
            return;
        }
        if (!lane.sourceRegistered
            && spectatorRooms >= config.maximumSpectatorRooms) {
            rejectSpectator(
                *connection, hello, SpectatorStatus::CapacityReached);
            return;
        }
        if (lane.sourceRegistered) {
            closeSpectatorSource(
                *room, SpectatorDetachReason::Superseded, true);
        }
        RegistrationToken token{};
        std::array<uint8_t, qnet::relay::kGrantKeyBytes> key{};
        if (!generateToken(token) || !generateGrantKey(key)) {
            rejectSpectator(
                *connection, hello, SpectatorStatus::SourceUnavailable);
            return;
        }
        lane.sourceRegistered = true;
        lane.sourceTcpSocket = socket;
        lane.sourceIpv4 = connection->remote.sin_addr.s_addr;
        lane.sourceToken = token;
        lane.grantKey = key;
        lane.generation += 1u;
        lane.sessionId = hello.sessionId;
        lane.identityBound = false;
        lane.identity = {};
        lane.sourceUdpRegistered = false;
        lane.sourceAdmittedMs = now;
        lane.sourceLastUdpMs = 0;
        lane.lastActivityMs = now;
        lane.registrationRate.reset(
            config.registrationsPerSecond,
            config.registrationsPerSecond * 2u,
            now);
        lane.packetRate.reset(
            config.roomUdpPacketsPerSecond,
            config.roomUdpPacketsPerSecond * 2u,
            now);
        lane.byteRate.reset(
            config.roomUdpBytesPerSecond,
            config.roomUdpBytesPerSecond * 2u,
            now);
        lane.usedGrantIds.clear();
        ++spectatorRooms;
        connection->assignedRoom = true;
        connection->viewerId = 0;

        qnet::relay::SpectatorWelcome welcome{};
        welcome.room = room->id;
        welcome.party = SpectatorParty::Source;
        welcome.status = SpectatorStatus::Accepted;
        welcome.registrationToken = token;
        welcome.sessionId = lane.sessionId;
        welcome.sourceGeneration = lane.generation;
        welcome.viewerCapacity =
            static_cast<uint16_t>((std::min)(config.maximumViewersPerRoom,
                                             uint32_t{ 0xFFFFu }));
        welcome.grantKey.assign(key.begin(), key.end());
        if (!sendSpectatorWelcome(*connection, welcome)) {
            closeSpectatorSource(
                *room, SpectatorDetachReason::ProtocolViolation, false);
            closeSingle(socket, "spectator welcome queue overflow");
            return;
        }
        log(LogLevel::Info,
            "[%s] spectator source registered generation=%u from %s",
            roomLabel(*room).c_str(),
            lane.generation,
            endpointText(connection->remote).c_str());
        return;
    }

    if (!lane.sourceRegistered) {
        rejectSpectator(*connection, hello, SpectatorStatus::SourceUnavailable);
        return;
    }
    if (hello.sessionId != lane.sessionId) {
        ++metrics.grantRejects;
        rejectSpectator(*connection, hello, SpectatorStatus::GrantRejected);
        return;
    }
    if (hello.route != SpectatorRouteKind::Relay) {
        ++metrics.grantRejects;
        rejectSpectator(*connection, hello, SpectatorStatus::GrantRejected);
        return;
    }

    const bool publicAdmission =
        hello.admission == SpectatorAdmission::PublicToken;
    uint64_t viewerId = hello.viewerId;
    uint64_t grantId = hello.grantId;
    uint64_t expiryUnix = hello.expiryUnix;
    const uint64_t nowUnix = wallUnixSeconds();

    if (publicAdmission) {
        if (!sameId(room->viewerAccessToken, hello.sourceToken)) {
            ++metrics.grantRejects;
            rejectSpectator(
                *connection, hello, SpectatorStatus::GrantRejected);
            return;
        }
        RegistrationToken entropy{};
        bool uniqueViewer = false;
        for (unsigned attempt = 0; attempt < 8 && !uniqueViewer; ++attempt) {
            if (!generateToken(entropy)) {
                rejectSpectator(
                    *connection, hello, SpectatorStatus::SourceUnavailable);
                return;
            }
            viewerId = entropy.low ^ entropy.high;
            uniqueViewer = viewerId != 0
                && lane.viewers.find(viewerId) == lane.viewers.end();
        }
        if (!uniqueViewer) {
            rejectSpectator(
                *connection, hello, SpectatorStatus::SourceUnavailable);
            return;
        }
        grantId = 0;
        expiryUnix = 0;
    } else {
        if (hello.admission != SpectatorAdmission::Grant
            || hello.sourceGeneration != lane.generation) {
            ++metrics.grantRejects;
            rejectSpectator(*connection, hello, SpectatorStatus::Superseded);
            return;
        }
        if (hello.expiryUnix <= nowUnix) {
            ++metrics.grantExpiredRejects;
            rejectSpectator(
                *connection, hello, SpectatorStatus::GrantExpired);
            return;
        }
        if (hello.expiryUnix - nowUnix
            > config.maximumGrantLifetimeSeconds) {
            ++metrics.grantRejects;
            rejectSpectator(
                *connection, hello, SpectatorStatus::GrantRejected);
            return;
        }
        if (lane.usedGrantIds.find(hello.grantId)
            != lane.usedGrantIds.end()) {
            ++metrics.grantReplayRejects;
            rejectSpectator(
                *connection, hello, SpectatorStatus::GrantRejected);
            return;
        }

        SpectatorGrantRequest request{};
        request.key = lane.grantKey.data();
        request.keyLength = lane.grantKey.size();
        request.grantId = hello.grantId;
        request.viewerId = hello.viewerId;
        request.sessionId = hello.sessionId;
        request.expiryUnix = hello.expiryUnix;
        request.sourceGeneration = hello.sourceGeneration;
        request.resumeRecord = hello.resumeRecord;
        request.resumeDigest = hello.resumeDigest;
        request.route = static_cast<uint8_t>(hello.route);
        request.macLow = hello.macLow;
        request.macHigh = hello.macHigh;
        const GrantVerdict verdict = grantVerifier->verify(request);
        if (verdict == GrantVerdict::Unsupported) {
            rejectSpectator(
                *connection, hello, SpectatorStatus::Unsupported);
            return;
        }
        if (verdict != GrantVerdict::Accepted) {
            ++metrics.grantRejects;
            rejectSpectator(
                *connection, hello, SpectatorStatus::GrantRejected);
            return;
        }
    }

    const uint32_t ipv4 = connection->remote.sin_addr.s_addr;
    const auto existing = lane.viewers.find(viewerId);
    const bool migration = existing != lane.viewers.end();
    if (!migration) {
        if (lane.viewers.size() >= config.maximumViewersPerRoom
            || totalViewers >= config.maximumViewers) {
            rejectSpectator(
                *connection, hello, SpectatorStatus::CapacityReached);
            return;
        }
        if (ipState(ipv4).viewers >= config.maximumViewersPerIp) {
            ++metrics.ipQuotaRejects;
            rejectSpectator(
                *connection, hello, SpectatorStatus::CapacityReached);
            return;
        }
    }
    if (lane.usedGrantIds.size() >= kMaximumUsedGrantIds) {
        rejectSpectator(*connection, hello, SpectatorStatus::RateLimited);
        return;
    }

    RegistrationToken token{};
    if (!generateToken(token)) {
        rejectSpectator(*connection, hello, SpectatorStatus::SourceUnavailable);
        return;
    }

    if (migration) {
        ViewerState& viewer = existing->second;
        if (viewer.udpRegistered) {
            sendSpectatorDetachTo(
                viewer.udpEndpoint, room->id, SpectatorParty::Viewer,
                viewer.token, viewer.viewerId, lane.generation,
                SpectatorDetachReason::EndpointMigrated);
            udpBindings.erase(endpointKey(viewer.udpEndpoint));
            viewer.udpRegistered = false;
        }
        if (viewer.tcpSocket != INVALID_SOCKET
            && viewer.tcpSocket != socket) {
            const SOCKET previous = viewer.tcpSocket;
            viewer.tcpSocket = INVALID_SOCKET;
            closeSingle(previous, "spectator viewer migrated");
        }
        if (viewer.ipv4 != ipv4) {
            releaseIp(viewer.ipv4, false, false, true);
            ++ipState(ipv4).viewers;
            viewer.ipv4 = ipv4;
        }
        viewer.grantId = grantId;
        viewer.token = token;
        viewer.tcpSocket = socket;
        viewer.expiryUnix = expiryUnix;
        viewer.generation = lane.generation;
        viewer.admittedMs = now;
        viewer.lastActivityMs = now;
    } else {
        ViewerState viewer{};
        viewer.viewerId = viewerId;
        viewer.grantId = grantId;
        viewer.tcpSocket = socket;
        viewer.ipv4 = ipv4;
        viewer.token = token;
        viewer.admittedMs = now;
        viewer.lastActivityMs = now;
        viewer.expiryUnix = expiryUnix;
        viewer.generation = lane.generation;
        viewer.registrationRate.reset(
            config.registrationsPerSecond,
            config.registrationsPerSecond * 2u,
            now);
        viewer.packetRate.reset(
            config.viewerUdpPacketsPerSecond,
            config.viewerUdpPacketsPerSecond * 2u,
            now);
        viewer.byteRate.reset(
            config.viewerUdpBytesPerSecond,
            config.viewerUdpBytesPerSecond * 2u,
            now);
        lane.viewers.emplace(viewerId, std::move(viewer));
        ++ipState(ipv4).viewers;
        ++totalViewers;
        ++metrics.viewersAdmitted;
    }
    if (!publicAdmission)
        lane.usedGrantIds.emplace(grantId, nowUnix);
    lane.lastActivityMs = now;
    room->lastActivityMs = now;
    connection->assignedRoom = true;
    connection->viewerId = viewerId;

    qnet::relay::SpectatorWelcome welcome{};
    welcome.room = room->id;
    welcome.party = SpectatorParty::Viewer;
    welcome.status = lane.sourceUdpRegistered
        ? SpectatorStatus::Accepted
        : SpectatorStatus::Waiting;
    welcome.registrationToken = token;
    welcome.viewerId = viewerId;
    welcome.sessionId = lane.sessionId;
    welcome.sourceGeneration = lane.generation;
    welcome.viewerCount =
        static_cast<uint16_t>((std::min<size_t>)(lane.viewers.size(), 0xFFFFu));
    welcome.viewerCapacity =
        static_cast<uint16_t>((std::min)(config.maximumViewersPerRoom,
                                         uint32_t{ 0xFFFFu }));
    if (!sendSpectatorWelcome(*connection, welcome)) {
        detachViewer(*room, viewerId,
                     SpectatorDetachReason::ProtocolViolation, false);
        closeSingle(socket, "spectator welcome queue overflow");
        return;
    }
    if (config.verbose) {
        log(LogLevel::Info,
            "[%s] viewer admitted generation=%u viewers=%zu from %s",
            roomLabel(*room).c_str(),
            lane.generation,
            lane.viewers.size(),
            endpointText(connection->remote).c_str());
    }
}

void RelayService::State::detachViewer(Room& room,
                                       const uint64_t viewerId,
                                       const SpectatorDetachReason reason,
                                       const bool notify) {
    SpectatorLane& lane = room.spectator;
    const auto it = lane.viewers.find(viewerId);
    if (it == lane.viewers.end()) return;
    ViewerState viewer = it->second;
    lane.viewers.erase(it);
    if (viewer.udpRegistered) {
        if (notify) {
            sendSpectatorDetachTo(
                viewer.udpEndpoint, room.id, SpectatorParty::Viewer,
                viewer.token, viewer.viewerId, lane.generation, reason);
        }
        udpBindings.erase(endpointKey(viewer.udpEndpoint));
    }
    if (viewer.tcpSocket != INVALID_SOCKET) {
        Connection* const connection = findConnection(viewer.tcpSocket);
        if (connection != nullptr) {
            connection->assignedRoom = false;
            closeSingle(viewer.tcpSocket, "spectator viewer detached");
        }
    }
    releaseIp(viewer.ipv4, false, false, true);
    if (totalViewers > 0) --totalViewers;
    ++metrics.viewersDetached;
    if (lane.sourceUdpRegistered && notify) {
        sendSpectatorDetachTo(
            lane.sourceUdpEndpoint, room.id, SpectatorParty::Source,
            lane.sourceToken, viewer.viewerId, lane.generation, reason);
    }
}

void RelayService::State::closeSpectatorSource(
    Room& room,
    const SpectatorDetachReason reason,
    const bool notify) {
    SpectatorLane& lane = room.spectator;
    if (!lane.sourceRegistered) return;
    std::vector<uint64_t> viewerIds;
    viewerIds.reserve(lane.viewers.size());
    for (const auto& entry : lane.viewers) viewerIds.push_back(entry.first);
    for (const uint64_t viewerId : viewerIds)
        detachViewer(room, viewerId, reason, notify);

    if (lane.sourceUdpRegistered) {
        udpBindings.erase(endpointKey(lane.sourceUdpEndpoint));
        lane.sourceUdpRegistered = false;
    }
    if (lane.sourceTcpSocket != INVALID_SOCKET) {
        Connection* const connection = findConnection(lane.sourceTcpSocket);
        const SOCKET socket = lane.sourceTcpSocket;
        lane.sourceTcpSocket = INVALID_SOCKET;
        if (connection != nullptr) {
            connection->assignedRoom = false;
            closeSingle(socket, "spectator source closed");
        }
    }
    lane.sourceRegistered = false;
    lane.sourceToken = {};
    lane.grantKey.fill(0);
    lane.identityBound = false;
    lane.identity = {};
    lane.sourceAdmittedMs = 0;
    lane.sourceLastUdpMs = 0;
    lane.usedGrantIds.clear();
    if (spectatorRooms > 0) --spectatorRooms;
}

bool RelayService::State::sendTopologyWelcome(
    Connection& connection,
    const qnet::relay::TopologyWelcome& welcome) {
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeTopologyWelcome(welcome, encoded))
        return false;
    return sendControlRecord(connection, encoded);
}

void RelayService::State::sendTopologyDetachTo(
    const sockaddr_in& endpoint,
    const RoomId& room,
    const TopologyParty party,
    const RegistrationToken& token,
    const uint64_t nodeId,
    const uint32_t generation,
    const TopologyDetachReason reason) {
    if (udpSocket == INVALID_SOCKET || idIsZero(token)) return;
    qnet::relay::TopologyDetach detach{};
    detach.room = room;
    detach.party = party;
    detach.reason = reason;
    detach.authorityGeneration = generation;
    detach.token = token;
    detach.nodeId = nodeId;
    detach.retryAfterMs = config.retryAfterMs;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeTopologyDetach(detach, encoded)) return;
    (void)sendto(
        udpSocket,
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<int>(encoded.size()),
        0,
        reinterpret_cast<const sockaddr*>(&endpoint),
        sizeof(endpoint));
}

void RelayService::State::rejectTopology(
    Connection& connection,
    const qnet::relay::TopologyHello& hello,
    const TopologyStatus status) {
    connection.room = hello.room;
    connection.lane = Lane::Topology;
    connection.topologyParty = hello.party;
    connection.joined = true;
    connection.assignedRoom = false;
    ++metrics.rejects;
    if (status == TopologyStatus::CapacityReached)
        ++metrics.capacityRejects;
    qnet::relay::TopologyWelcome welcome{};
    welcome.room = hello.room;
    welcome.party = hello.party;
    welcome.status = status;
    welcome.retryAfterMs = config.retryAfterMs;
    if (!sendTopologyWelcome(connection, welcome))
        connection.sendBytes.clear();
    if (config.verbose) {
        const Room* const room = findRoom(hello.room);
        log(LogLevel::Info,
            "[%s] topology %s rejected status=%u from %s",
            room != nullptr ? roomLabel(*room).c_str() : "room-missing",
            hello.party == TopologyParty::Authority ? "authority" : "node",
            static_cast<unsigned>(status),
            endpointText(connection.remote).c_str());
    }
    connection.closeAfterFlush = true;
}

void RelayService::State::processTopologyHello(
    const SOCKET socket,
    const qnet::relay::TopologyHello& hello) {
    Connection* connection = findConnection(socket);
    if (connection == nullptr) return;
    connection->joined = true;
    connection->lane = Lane::Topology;
    connection->room = hello.room;
    connection->topologyParty = hello.party;

    if (!config.topologyLaneEnabled) {
        rejectTopology(*connection, hello, TopologyStatus::Unsupported);
        return;
    }

    Room* const room = findRoom(hello.room);
    if (room == nullptr
        || (!room->paired && !room->retainedSpectatorOnly)) {
        rejectTopology(*connection, hello, TopologyStatus::RoomNotFound);
        return;
    }
    TopologyLane& lane = room->topology;
    const uint64_t now = connection->lastActivityMs;

    if (hello.party == TopologyParty::Authority) {
        if (!room->paired) {
            rejectTopology(
                *connection, hello, TopologyStatus::AuthorityUnavailable);
            return;
        }
        Connection* const hostTcp =
            findConnection(room->peers[roleIndex(RelayRole::Host)].tcpSocket);
        if (hostTcp == nullptr
            || hostTcp->remote.sin_addr.s_addr
                != connection->remote.sin_addr.s_addr
            || !sameId(room->spectatorRegistrationToken,
                       hello.authorityToken)) {
            ++metrics.topologyGrantRejects;
            rejectTopology(
                *connection, hello, TopologyStatus::GrantRejected);
            return;
        }
        if (!lane.authorityRegistered
            && topologyRooms >= config.maximumTopologyRooms) {
            rejectTopology(
                *connection, hello, TopologyStatus::CapacityReached);
            return;
        }
        if (lane.authorityRegistered) {
            closeTopologyAuthority(
                *room, TopologyDetachReason::Superseded, true);
        }
        RegistrationToken token{};
        std::array<uint8_t, qnet::relay::kGrantKeyBytes> key{};
        if (!generateToken(token) || !generateGrantKey(key)) {
            rejectTopology(
                *connection, hello, TopologyStatus::AuthorityUnavailable);
            return;
        }
        lane.authorityRegistered = true;
        lane.authorityTcpSocket = socket;
        lane.authorityIpv4 = connection->remote.sin_addr.s_addr;
        lane.authorityToken = token;
        lane.grantKey = key;
        lane.generation += 1u;
        lane.sessionId = hello.sessionId;
        lane.authorityUdpRegistered = false;
        lane.authorityAdmittedMs = now;
        lane.authorityLastUdpMs = 0;
        lane.lastActivityMs = now;
        lane.registrationRate.reset(
            config.registrationsPerSecond,
            config.registrationsPerSecond * 2u,
            now);
        lane.packetRate.reset(
            config.roomUdpPacketsPerSecond,
            config.roomUdpPacketsPerSecond * 2u,
            now);
        lane.byteRate.reset(
            config.roomUdpBytesPerSecond,
            config.roomUdpBytesPerSecond * 2u,
            now);
        lane.usedGrantIds.clear();
        ++topologyRooms;
        connection->assignedRoom = true;
        connection->nodeId = 0;

        qnet::relay::TopologyWelcome welcome{};
        welcome.room = room->id;
        welcome.party = TopologyParty::Authority;
        welcome.status = TopologyStatus::Accepted;
        welcome.registrationToken = token;
        welcome.sessionId = lane.sessionId;
        welcome.authorityGeneration = lane.generation;
        welcome.nodeCapacity =
            static_cast<uint16_t>((std::min)(config.maximumTopologyNodesPerRoom,
                                             uint32_t{ 0xFFFFu }));
        welcome.grantKey.assign(key.begin(), key.end());
        if (!sendTopologyWelcome(*connection, welcome)) {
            closeTopologyAuthority(
                *room, TopologyDetachReason::ProtocolViolation, false);
            closeSingle(socket, "topology welcome queue overflow");
            return;
        }
        log(LogLevel::Info,
            "[%s] topology authority registered generation=%u from %s",
            roomLabel(*room).c_str(),
            lane.generation,
            endpointText(connection->remote).c_str());
        return;
    }

    qnet::relay::TopologyWelcome welcome{};
    const TopologyStatus status = admitTopologyNode(
        *room, hello, connection->remote.sin_addr.s_addr, socket, now,
        welcome);
    if (status != TopologyStatus::Accepted
        && status != TopologyStatus::Waiting) {
        rejectTopology(*connection, hello, status);
        return;
    }
    connection->assignedRoom = true;
    connection->nodeId = hello.nodeId;
    if (!sendTopologyWelcome(*connection, welcome)) {
        detachTopologyNode(*room, hello.nodeId,
                           TopologyDetachReason::ProtocolViolation, false);
        closeSingle(socket, "topology welcome queue overflow");
        return;
    }
    if (config.verbose) {
        log(LogLevel::Info,
            "[%s] topology node admitted generation=%u nodes=%zu from %s",
            roomLabel(*room).c_str(),
            lane.generation,
            lane.nodes.size(),
            endpointText(connection->remote).c_str());
    }
}

TopologyStatus RelayService::State::admitTopologyNode(
    Room& room,
    const qnet::relay::TopologyHello& hello,
    const uint32_t ipv4,
    const SOCKET tcpSocket,
    const uint64_t now,
    qnet::relay::TopologyWelcome& welcome) {
    TopologyLane& lane = room.topology;
    welcome = {};
    welcome.room = room.id;
    welcome.party = TopologyParty::Node;

    const auto fail = [&welcome](const TopologyStatus status) {
        welcome.status = status;
        return status;
    };

    if (!lane.authorityRegistered)
        return fail(TopologyStatus::AuthorityUnavailable);
    if (hello.sessionId != lane.sessionId) {
        ++metrics.topologyGrantRejects;
        return fail(TopologyStatus::GrantRejected);
    }
    if (hello.authorityGeneration != lane.generation) {
        ++metrics.topologyGrantRejects;
        return fail(TopologyStatus::Superseded);
    }

    const uint64_t nowUnix = wallUnixSeconds();
    const auto existing = lane.nodes.find(hello.nodeId);
    const bool migration = existing != lane.nodes.end();

    if (migration && existing->second.grantId == hello.grantId
        && existing->second.generation == lane.generation) {
        NodeState& node = existing->second;
        node.lastActivityMs = now;
        lane.lastActivityMs = now;
        welcome.status = lane.authorityUdpRegistered
            ? TopologyStatus::Accepted
            : TopologyStatus::Waiting;
        welcome.registrationToken = node.token;
        welcome.nodeId = node.nodeId;
        welcome.sessionId = lane.sessionId;
        welcome.authorityGeneration = lane.generation;
        welcome.nodeCount = static_cast<uint16_t>(
            (std::min<size_t>)(lane.nodes.size(), 0xFFFFu));
        welcome.nodeCapacity = static_cast<uint16_t>(
            (std::min)(config.maximumTopologyNodesPerRoom,
                       uint32_t{ 0xFFFFu }));
        return welcome.status;
    }

    if (hello.expiryUnix <= nowUnix) {
        ++metrics.topologyGrantRejects;
        return fail(TopologyStatus::GrantExpired);
    }
    if (hello.expiryUnix - nowUnix > config.maximumGrantLifetimeSeconds) {
        ++metrics.topologyGrantRejects;
        return fail(TopologyStatus::GrantRejected);
    }
    if (lane.usedGrantIds.find(hello.grantId) != lane.usedGrantIds.end()) {
        ++metrics.topologyGrantReplayRejects;
        return fail(TopologyStatus::GrantRejected);
    }

    TopologyGrantRequest request{};
    request.key = lane.grantKey.data();
    request.keyLength = lane.grantKey.size();
    request.grantId = hello.grantId;
    request.nodeId = hello.nodeId;
    request.sessionId = hello.sessionId;
    request.expiryUnix = hello.expiryUnix;
    request.authorityGeneration = hello.authorityGeneration;
    request.macLow = hello.macLow;
    request.macHigh = hello.macHigh;
    request.nowUnix = nowUnix;
    const TopologyGrantVerifier* const grantVerifier =
        topologyVerifier != nullptr
            ? topologyVerifier
            : &defaultTopologyGrantVerifier();
    const GrantVerdict verdict = grantVerifier->verify(request);
    if (verdict == GrantVerdict::Unsupported)
        return fail(TopologyStatus::Unsupported);
    if (verdict != GrantVerdict::Accepted) {
        ++metrics.topologyGrantRejects;
        return fail(TopologyStatus::GrantRejected);
    }

    if (!migration) {
        if (lane.nodes.size() >= config.maximumTopologyNodesPerRoom
            || totalTopologyNodes >= config.maximumTopologyNodes) {
            return fail(TopologyStatus::CapacityReached);
        }
        if (ipState(ipv4).nodes >= config.maximumTopologyNodesPerIp) {
            ++metrics.ipQuotaRejects;
            return fail(TopologyStatus::CapacityReached);
        }
    }
    if (lane.usedGrantIds.size() >= kMaximumUsedGrantIds)
        return fail(TopologyStatus::RateLimited);

    RegistrationToken token{};
    if (!generateToken(token))
        return fail(TopologyStatus::AuthorityUnavailable);

    if (migration) {
        NodeState& node = existing->second;
        if (node.udpRegistered) {
            sendTopologyDetachTo(
                node.udpEndpoint, room.id, TopologyParty::Node,
                node.token, node.nodeId, lane.generation,
                TopologyDetachReason::EndpointMigrated);
            udpBindings.erase(endpointKey(node.udpEndpoint));
            node.udpRegistered = false;
        }
        if (node.tcpSocket != INVALID_SOCKET
            && node.tcpSocket != tcpSocket) {
            const SOCKET previous = node.tcpSocket;
            node.tcpSocket = INVALID_SOCKET;
            closeSingle(previous, "topology node migrated");
        }
        if (node.ipv4 != ipv4) {
            releaseIp(node.ipv4, false, false, false, true);
            ++ipState(ipv4).nodes;
            node.ipv4 = ipv4;
        }
        node.grantId = hello.grantId;
        node.token = token;
        node.tcpSocket = tcpSocket;
        node.expiryUnix = hello.expiryUnix;
        node.generation = lane.generation;
        node.admittedMs = now;
        node.lastActivityMs = now;
    } else {
        NodeState node{};
        node.nodeId = hello.nodeId;
        node.grantId = hello.grantId;
        node.tcpSocket = tcpSocket;
        node.ipv4 = ipv4;
        node.token = token;
        node.admittedMs = now;
        node.lastActivityMs = now;
        node.expiryUnix = hello.expiryUnix;
        node.generation = lane.generation;
        node.registrationRate.reset(
            config.registrationsPerSecond,
            config.registrationsPerSecond * 2u,
            now);
        node.packetRate.reset(
            config.viewerUdpPacketsPerSecond,
            config.viewerUdpPacketsPerSecond * 2u,
            now);
        node.byteRate.reset(
            config.viewerUdpBytesPerSecond,
            config.viewerUdpBytesPerSecond * 2u,
            now);
        lane.nodes.emplace(hello.nodeId, std::move(node));
        ++ipState(ipv4).nodes;
        ++totalTopologyNodes;
        ++metrics.topologyNodesAdmitted;
    }
    lane.usedGrantIds.emplace(hello.grantId, nowUnix);
    lane.lastActivityMs = now;
    room.lastActivityMs = now;

    welcome.status = lane.authorityUdpRegistered
        ? TopologyStatus::Accepted
        : TopologyStatus::Waiting;
    welcome.registrationToken = token;
    welcome.nodeId = hello.nodeId;
    welcome.sessionId = lane.sessionId;
    welcome.authorityGeneration = lane.generation;
    welcome.nodeCount = static_cast<uint16_t>(
        (std::min<size_t>)(lane.nodes.size(), 0xFFFFu));
    welcome.nodeCapacity = static_cast<uint16_t>(
        (std::min)(config.maximumTopologyNodesPerRoom, uint32_t{ 0xFFFFu }));
    return welcome.status;
}

void RelayService::State::processTopologyHelloDatagram(
    const qnet::relay::TopologyHello& hello,
    const sockaddr_in& source,
    const uint64_t now) {
    if (!config.topologyLaneEnabled) return;
    if (hello.party != TopologyParty::Node) return;
    Room* const room = findRoom(hello.room);
    if (room == nullptr
        || (!room->paired && !room->retainedSpectatorOnly)) {
        return;
    }
    qnet::relay::TopologyWelcome welcome{};
    const TopologyStatus status = admitTopologyNode(
        *room, hello, source.sin_addr.s_addr, INVALID_SOCKET, now, welcome);
    if (status != TopologyStatus::Accepted
        && status != TopologyStatus::Waiting) {
        ++metrics.rejects;
        if (status == TopologyStatus::CapacityReached)
            ++metrics.capacityRejects;
        welcome.retryAfterMs = config.retryAfterMs;
    }
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeTopologyWelcome(welcome, encoded)) return;
    (void)sendto(
        udpSocket,
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<int>(encoded.size()),
        0,
        reinterpret_cast<const sockaddr*>(&source),
        sizeof(source));
}

void RelayService::State::detachTopologyNode(
    Room& room,
    const uint64_t nodeId,
    const TopologyDetachReason reason,
    const bool notify) {
    TopologyLane& lane = room.topology;
    const auto it = lane.nodes.find(nodeId);
    if (it == lane.nodes.end()) return;
    NodeState node = it->second;
    lane.nodes.erase(it);
    if (node.udpRegistered) {
        if (notify) {
            sendTopologyDetachTo(
                node.udpEndpoint, room.id, TopologyParty::Node,
                node.token, node.nodeId, lane.generation, reason);
        }
        udpBindings.erase(endpointKey(node.udpEndpoint));
    }
    if (node.tcpSocket != INVALID_SOCKET) {
        Connection* const connection = findConnection(node.tcpSocket);
        if (connection != nullptr) {
            connection->assignedRoom = false;
            closeSingle(node.tcpSocket, "topology node detached");
        }
    }
    releaseIp(node.ipv4, false, false, false, true);
    if (totalTopologyNodes > 0) --totalTopologyNodes;
    ++metrics.topologyNodesDetached;
    if (lane.authorityUdpRegistered && notify) {
        sendTopologyDetachTo(
            lane.authorityUdpEndpoint, room.id, TopologyParty::Authority,
            lane.authorityToken, node.nodeId, lane.generation, reason);
    }
}

void RelayService::State::closeTopologyAuthority(
    Room& room,
    const TopologyDetachReason reason,
    const bool notify) {
    TopologyLane& lane = room.topology;
    if (!lane.authorityRegistered) return;
    std::vector<uint64_t> nodeIds;
    nodeIds.reserve(lane.nodes.size());
    for (const auto& entry : lane.nodes) nodeIds.push_back(entry.first);
    for (const uint64_t nodeId : nodeIds)
        detachTopologyNode(room, nodeId, reason, notify);

    if (lane.authorityUdpRegistered) {
        udpBindings.erase(endpointKey(lane.authorityUdpEndpoint));
        lane.authorityUdpRegistered = false;
    }
    if (lane.authorityTcpSocket != INVALID_SOCKET) {
        Connection* const connection =
            findConnection(lane.authorityTcpSocket);
        const SOCKET socket = lane.authorityTcpSocket;
        lane.authorityTcpSocket = INVALID_SOCKET;
        if (connection != nullptr) {
            connection->assignedRoom = false;
            closeSingle(socket, "topology authority closed");
        }
    }
    lane.authorityRegistered = false;
    lane.authorityToken = {};
    lane.grantKey.fill(0);
    lane.authorityAdmittedMs = 0;
    lane.authorityLastUdpMs = 0;
    lane.usedGrantIds.clear();
    if (topologyRooms > 0) --topologyRooms;
}

void RelayService::State::sendTopologyReady(Room& room, NodeState& node) {
    TopologyLane& lane = room.topology;
    if (!lane.authorityUdpRegistered || !node.udpRegistered) return;

    qnet::relay::TopologyReady toNode{};
    toNode.room = room.id;
    toNode.party = TopologyParty::Node;
    toNode.authorityGeneration = lane.generation;
    toNode.token = node.token;
    toNode.nodeId = node.nodeId;
    toNode.peer = protocolEndpoint(lane.authorityUdpEndpoint);
    std::vector<uint8_t> encoded;
    if (qnet::relay::encodeTopologyReady(toNode, encoded)) {
        (void)sendto(
            udpSocket,
            reinterpret_cast<const char*>(encoded.data()),
            static_cast<int>(encoded.size()),
            0,
            reinterpret_cast<const sockaddr*>(&node.udpEndpoint),
            sizeof(node.udpEndpoint));
    }

    qnet::relay::TopologyReady toAuthority{};
    toAuthority.room = room.id;
    toAuthority.party = TopologyParty::Authority;
    toAuthority.authorityGeneration = lane.generation;
    toAuthority.token = lane.authorityToken;
    toAuthority.nodeId = node.nodeId;
    toAuthority.peer = protocolEndpoint(node.udpEndpoint);
    encoded.clear();
    if (qnet::relay::encodeTopologyReady(toAuthority, encoded)) {
        (void)sendto(
            udpSocket,
            reinterpret_cast<const char*>(encoded.data()),
            static_cast<int>(encoded.size()),
            0,
            reinterpret_cast<const sockaddr*>(&lane.authorityUdpEndpoint),
            sizeof(lane.authorityUdpEndpoint));
    }
}

void RelayService::State::processTopologyRegister(
    const qnet::relay::TopologyRegister& registration,
    const sockaddr_in& source,
    const uint64_t now) {
    if (!config.topologyLaneEnabled) return;
    Room* const room = findRoom(registration.room);
    if (room == nullptr) return;
    TopologyLane& lane = room->topology;
    if (!lane.authorityRegistered) return;
    if (registration.authorityGeneration != lane.generation) return;

    const EndpointKey newKey = endpointKey(source);
    const auto existingBinding = udpBindings.find(newKey);

    if (registration.party == TopologyParty::Authority) {
        if (!sameId(lane.authorityToken, registration.token)
            || source.sin_addr.s_addr != lane.authorityIpv4
            || !lane.registrationRate.consume(1, now)) {
            return;
        }
        if (existingBinding != udpBindings.end()
            && (existingBinding->second.lane != Lane::Topology
                || !sameId(existingBinding->second.room, room->id)
                || existingBinding->second.topologyParty
                    != TopologyParty::Authority)) {
            ++metrics.crossLaneRejects;
            return;
        }
        if (lane.authorityUdpRegistered
            && !sameEndpoint(lane.authorityUdpEndpoint, source)) {
            udpBindings.erase(endpointKey(lane.authorityUdpEndpoint));
        }
        const bool first = !lane.authorityUdpRegistered;
        lane.authorityUdpRegistered = true;
        lane.authorityUdpEndpoint = source;
        lane.authorityLastUdpMs = now;
        lane.lastActivityMs = now;
        room->lastActivityMs = now;
        UdpBinding binding{};
        binding.room = room->id;
        binding.lane = Lane::Topology;
        binding.topologyParty = TopologyParty::Authority;
        udpBindings[newKey] = binding;
        for (auto& entry : lane.nodes)
            sendTopologyReady(*room, entry.second);
        if (first) {
            log(LogLevel::Info,
                "[%s] topology authority UDP registered from %s",
                roomLabel(*room).c_str(),
                endpointText(source).c_str());
        }
        return;
    }

    const auto it = lane.nodes.find(registration.nodeId);
    if (it == lane.nodes.end()) return;
    NodeState& node = it->second;
    if (!sameId(node.token, registration.token)
        || source.sin_addr.s_addr != node.ipv4
        || !node.registrationRate.consume(1, now)) {
        return;
    }
    if (existingBinding != udpBindings.end()
        && (existingBinding->second.lane != Lane::Topology
            || !sameId(existingBinding->second.room, room->id)
            || existingBinding->second.topologyParty != TopologyParty::Node
            || existingBinding->second.nodeId != node.nodeId)) {
        ++metrics.crossLaneRejects;
        return;
    }
    if (node.udpRegistered && !sameEndpoint(node.udpEndpoint, source))
        udpBindings.erase(endpointKey(node.udpEndpoint));
    node.udpRegistered = true;
    node.udpEndpoint = source;
    node.lastActivityMs = now;
    UdpBinding binding{};
    binding.room = room->id;
    binding.lane = Lane::Topology;
    binding.topologyParty = TopologyParty::Node;
    binding.nodeId = node.nodeId;
    udpBindings[newKey] = binding;
    lane.lastActivityMs = now;
    room->lastActivityMs = now;
    sendTopologyReady(*room, node);
}

void RelayService::State::processTopologyDetach(
    const qnet::relay::TopologyDetach& detach,
    const sockaddr_in& source,
    const uint64_t now) {
    if (!config.topologyLaneEnabled) return;
    Room* const room = findRoom(detach.room);
    if (room == nullptr) return;
    TopologyLane& lane = room->topology;
    if (!lane.authorityRegistered
        || detach.authorityGeneration != lane.generation) {
        return;
    }
    lane.lastActivityMs = now;

    if (detach.party == TopologyParty::Authority) {
        if (!sameId(lane.authorityToken, detach.token)
            || source.sin_addr.s_addr != lane.authorityIpv4) {
            return;
        }
        if (detach.nodeId != 0u) {
            detachTopologyNode(*room, detach.nodeId, detach.reason, true);
            return;
        }
        closeTopologyAuthority(
            *room, TopologyDetachReason::AuthorityLost, true);
        return;
    }
    const auto it = lane.nodes.find(detach.nodeId);
    if (it == lane.nodes.end()) return;
    if (!sameId(it->second.token, detach.token)
        || source.sin_addr.s_addr != it->second.ipv4) {
        return;
    }
    detachTopologyNode(*room, detach.nodeId,
                       TopologyDetachReason::NodeRequested, true);
}

void RelayService::State::processTopologyFrame(const uint8_t* const bytes,
                                               const size_t length,
                                               const UdpBinding& binding,
                                               const sockaddr_in& source,
                                               const uint64_t now) {
    if (!config.topologyLaneEnabled) return;
    Room* const room = findRoom(binding.room);
    if (room == nullptr) return;
    TopologyLane& lane = room->topology;
    if (!lane.authorityRegistered) return;

    TopologyFrameHeader header{};
    if (qnet::relay::decodeTopologyFrameHeader(bytes, length, header)
        != qnet::relay::DecodeError::None) {
        ++metrics.drops;
        return;
    }
    if (header.party != binding.topologyParty
        || header.authorityGeneration != lane.generation) {
        ++metrics.drops;
        return;
    }

    const uint8_t* const inner =
        bytes + qnet::relay::kTopologyFrameHeaderSize;
    const size_t innerLength =
        length - qnet::relay::kTopologyFrameHeaderSize;

    ++metrics.topologyPacketsIn;
    metrics.topologyBytesIn += innerLength;
    lane.bytesIn += innerLength;

    TopologyDatagramInfo info{};
    if (qnetplay::relay::server::inspectTopologyControlDatagram(
            inner, innerLength, info) != TopologyDatagramError::None) {
        ++metrics.drops;
        return;
    }

    sockaddr_in destination{};
    if (binding.topologyParty == TopologyParty::Authority) {
        if (!sameEndpoint(lane.authorityUdpEndpoint, source)) return;
        if (!lane.packetRate.consume(1, now)
            || !lane.byteRate.consume(length, now)) {
            ++metrics.rateLimitDrops;
            ++metrics.drops;
            return;
        }
        if (info.nodeId != header.nodeId) {
            ++metrics.drops;
            return;
        }
        const auto it = lane.nodes.find(header.nodeId);
        if (it == lane.nodes.end() || !it->second.udpRegistered) {
            ++metrics.drops;
            return;
        }
        destination = it->second.udpEndpoint;
        it->second.bytesOut += innerLength;
        it->second.lastActivityMs = now;
    } else {
        const auto it = lane.nodes.find(binding.nodeId);
        if (it == lane.nodes.end()) return;
        NodeState& node = it->second;
        if (header.nodeId != node.nodeId || info.nodeId != node.nodeId) {
            ++metrics.drops;
            return;
        }
        if (!node.udpRegistered || !sameEndpoint(node.udpEndpoint, source))
            return;
        if (!lane.authorityUdpRegistered) {
            ++metrics.drops;
            return;
        }
        if (!node.packetRate.consume(1, now)
            || !node.byteRate.consume(length, now)) {
            ++metrics.rateLimitDrops;
            ++metrics.drops;
            return;
        }
        destination = lane.authorityUdpEndpoint;
        node.bytesIn += innerLength;
        node.lastActivityMs = now;
    }

    const int sent = sendto(
        udpSocket,
        reinterpret_cast<const char*>(bytes),
        static_cast<int>(length),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));
    if (sent == static_cast<int>(length)) {
        ++metrics.topologyPacketsOut;
        metrics.topologyBytesOut += innerLength;
        lane.bytesOut += innerLength;
        lane.lastActivityMs = now;
        room->lastActivityMs = now;
    }
}

void RelayService::State::receiveTcp(const SOCKET socket) {
    Connection* connection = findConnection(socket);
    if (connection == nullptr) return;
    const uint32_t allowance = config.maximumTcpBytesPerConnectionPerTurn;
    if (connection->turnBytes >= allowance) return;
    const size_t budget = (std::min<size_t>)(
        kSocketReadBuffer, allowance - connection->turnBytes);

    std::array<uint8_t, kSocketReadBuffer> buffer{};
    const int received = recv(
        socket,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(budget),
        0);
    if (received == 0) {
        closeConnection(socket, "TCP peer closed");
        return;
    }
    if (received == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK)
            closeConnection(socket, "TCP receive failed");
        return;
    }

    connection = findConnection(socket);
    if (connection == nullptr) return;
    const uint64_t now = monotonicMs();
    connection->lastActivityMs = now;
    connection->turnBytes += static_cast<uint32_t>(received);
    if (!connection->tcpBytes.consume(
            static_cast<uint64_t>(received), now)) {
        ++metrics.rateLimitDrops;
        closeConnection(socket, "TCP byte rate exceeded");
        return;
    }

    if (!connection->joined) {
        connection->helloBytes.insert(
            connection->helloBytes.end(),
            buffer.begin(),
            buffer.begin() + received);
        if (connection->helloBytes.size() < 2u) return;
        const size_t frameLength =
            static_cast<size_t>(connection->helloBytes[0])
            | (static_cast<size_t>(connection->helloBytes[1]) << 8u);
        if (frameLength > qnet::kMaxMsgSize
            || frameLength < sizeof(qnet::MsgHeader)) {
            closeSingle(socket, "invalid relay TCP envelope length");
            return;
        }
        const size_t totalFrameLength = frameLength + 2u;
        if (connection->helloBytes.size() < totalFrameLength) return;

        uint8_t envelopeType = qnet::MSG_NONE;
        const uint8_t* body = nullptr;
        size_t bodyLength = 0;
        if (!qnet::unpackMsg(
                connection->helloBytes.data() + 2u,
                frameLength,
                envelopeType,
                body,
                bodyLength)
            || envelopeType != qnet::relay::kTcpEnvelopeType) {
            closeSingle(socket, "invalid relay Hello");
            return;
        }

        uint16_t version = 0;
        const bool haveVersion =
            qnet::relay::peekRelayVersion(body, bodyLength, version);
        std::vector<uint8_t> trailing(
            connection->helloBytes.begin()
                + static_cast<std::ptrdiff_t>(totalFrameLength),
            connection->helloBytes.end());
        connection->helloBytes.clear();

        if (haveVersion && version == qnet::relay::kSpectatorVersion) {
            qnet::relay::SpectatorHello spectatorHello{};
            if (qnet::relay::decodeSpectatorHello(
                    body, bodyLength, spectatorHello)
                != qnet::relay::DecodeError::None) {
                closeSingle(socket, "invalid spectator Hello");
                return;
            }
            processSpectatorHello(socket, spectatorHello);
            return;
        }

        if (haveVersion && version == qnet::relay::kTopologyVersion) {
            qnet::relay::TopologyHello topologyHello{};
            if (qnet::relay::decodeTopologyHello(
                    body, bodyLength, topologyHello)
                != qnet::relay::DecodeError::None) {
                closeSingle(socket, "invalid topology Hello");
                return;
            }
            processTopologyHello(socket, topologyHello);
            return;
        }

        qnet::relay::Hello hello{};
        if (!haveVersion
            || qnet::relay::decodeHello(body, bodyLength, hello)
                != qnet::relay::DecodeError::None) {
            closeSingle(socket, "invalid relay Hello");
            return;
        }
        processHello(socket, hello);
        connection = findConnection(socket);
        if (connection != nullptr && connection->assignedRoom
            && !trailing.empty()) {
            processProxyBytes(socket, trailing.data(), trailing.size());
        }
        return;
    }
    processProxyBytes(socket, buffer.data(), static_cast<size_t>(received));
}

void RelayService::State::flushTcp(const SOCKET socket) {
    Connection* connection = findConnection(socket);
    if (connection == nullptr) return;
    while (connection->sendOffset < connection->sendBytes.size()) {
        const size_t remaining =
            connection->sendBytes.size() - connection->sendOffset;
        const int sent = send(
            socket,
            reinterpret_cast<const char*>(
                connection->sendBytes.data() + connection->sendOffset),
            static_cast<int>((std::min)(
                remaining,
                static_cast<size_t>(
                    (std::numeric_limits<int>::max)()))),
            0);
        if (sent == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) return;
            closeConnection(socket, "TCP send failed");
            return;
        }
        if (sent == 0) return;
        connection->sendOffset += static_cast<size_t>(sent);
        connection->lastActivityMs = monotonicMs();
    }
    connection->sendBytes.clear();
    connection->sendOffset = 0;
    if (connection->closeAfterFlush)
        closeSingle(socket, "relay request rejected");
}

void RelayService::State::acceptConnections(const uint64_t now) {
    pruneIdleIpStates(now);
    for (uint32_t accepted = 0;
         accepted < config.maximumAcceptsPerTurn;
         ++accepted) {
        sockaddr_in remote{};
        int remoteLength = sizeof(remote);
        const SOCKET socket = accept(
            tcpListener,
            reinterpret_cast<sockaddr*>(&remote),
            &remoteLength);
        if (socket == INVALID_SOCKET) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
                log(LogLevel::Warn, "accept failed: %d", error);
            return;
        }
        const uint32_t ipv4 = remote.sin_addr.s_addr;
        const size_t maximumConnections =
            static_cast<size_t>(config.maximumRooms) * 2u
            + static_cast<size_t>(config.maximumViewers)
            + static_cast<size_t>(config.maximumSpectatorRooms)
            + 32u;
        const bool globalCapacity =
            connections.size() < maximumConnections;
        auto ipIt = ips.find(ipv4);
        const bool tracked = ipIt != ips.end();
        const bool trackingCapacity =
            tracked || ips.size() < config.maximumTrackedIpStates;
        if (!globalCapacity || !trackingCapacity) {
            ++metrics.ipQuotaRejects;
            ++metrics.rejects;
            if (tracked) ipIt->second.lastSeenMs = now;
            if (config.verbose) {
                log(LogLevel::Info,
                    "[tcp %s] rejected at accept: global=%zu/%zu "
                    "tracked-ip=%zu/%u",
                    endpointText(remote).c_str(),
                    connections.size(),
                    maximumConnections,
                    ips.size(),
                    config.maximumTrackedIpStates);
            }
            closesocket(socket);
            continue;
        }
        if (!tracked) {
            IpState state{};
            state.connectionRate.reset(
                config.connectionsPerIpPerSecond,
                config.connectionsPerIpPerSecond * 4u,
                now);
            state.rateInitialized = true;
            state.lastSeenMs = now;
            ipIt = ips.emplace(ipv4, std::move(state)).first;
        }
        IpState& ip = ipIt->second;
        ip.lastSeenMs = now;
        const bool addressCapacity =
            ip.connections < config.maximumConnectionsPerIp;
        const bool rateAccepted =
            addressCapacity
            ? ip.connectionRate.consume(1, now)
            : false;
        if (!addressCapacity || !rateAccepted) {
            ++metrics.ipQuotaRejects;
            ++metrics.rejects;
            if (config.verbose) {
                log(LogLevel::Info,
                    "[tcp %s] rejected at accept: global=%zu/%zu ip=%u/%u rate=%s",
                    endpointText(remote).c_str(),
                    connections.size(),
                    maximumConnections,
                    ip.connections,
                    config.maximumConnectionsPerIp,
                    rateAccepted ? "ok" : "limited");
            }
            closesocket(socket);
            continue;
        }
        if (!setNonBlocking(socket)) {
            closesocket(socket);
            continue;
        }
        BOOL noDelay = TRUE;
        BOOL keepAlive = TRUE;
        (void)setsockopt(
            socket,
            IPPROTO_TCP,
            TCP_NODELAY,
            reinterpret_cast<const char*>(&noDelay),
            sizeof(noDelay));
        (void)setsockopt(
            socket,
            SOL_SOCKET,
            SO_KEEPALIVE,
            reinterpret_cast<const char*>(&keepAlive),
            sizeof(keepAlive));
        tcp_keepalive keepAliveTiming{};
        keepAliveTiming.onoff = 1;
        keepAliveTiming.keepalivetime = kTcpKeepAliveIdleMs;
        keepAliveTiming.keepaliveinterval = kTcpKeepAliveIntervalMs;
        DWORD keepAliveBytes = 0;
        (void)WSAIoctl(
            socket,
            SIO_KEEPALIVE_VALS,
            &keepAliveTiming,
            sizeof(keepAliveTiming),
            nullptr,
            0,
            &keepAliveBytes,
            nullptr,
            nullptr);

        Connection connection{};
        connection.socket = socket;
        connection.remote = remote;
        connection.acceptedMs = now;
        connection.lastActivityMs = now;
        connection.tcpBytes.reset(
            config.tcpBytesPerSecond,
            config.tcpBytesPerSecond * kTcpByteBurstMultiplier,
            now);
        connections.emplace(socket, std::move(connection));
        ++ip.connections;
        if (config.verbose) {
            log(LogLevel::Info,
                "[tcp %s] accepted: global=%zu/%zu ip=%u/%u",
                endpointText(remote).c_str(),
                connections.size(),
                maximumConnections,
                ip.connections,
                config.maximumConnectionsPerIp);
        }
    }
}

void RelayService::State::sendReady(Room& room, const RelayRole recipient) {
    PeerState* const own = findPeer(room, recipient);
    const PeerState* const peer = findPeer(room, oppositeRole(recipient));
    if (!own->udpRegistered || !peer->udpRegistered) return;

    Ready ready{};
    ready.room = room.id;
    ready.domain = LaneDomain::Player;
    ready.role = recipient;
    ready.token = own->token;
    ready.peer = protocolEndpoint(peer->udpEndpoint);
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeReady(ready, encoded)) return;
    (void)sendto(
        udpSocket,
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<int>(encoded.size()),
        0,
        reinterpret_cast<const sockaddr*>(&own->udpEndpoint),
        sizeof(own->udpEndpoint));
}

Room* RelayService::State::bindPlayerRegistration(
    const RoomId& roomId,
    const RelayRole role,
    const RegistrationToken& token,
    const sockaddr_in& source,
    const uint64_t now) {
    Room* const room = findRoom(roomId);
    if (room == nullptr || !room->paired) return nullptr;
    PeerState* const peer = findPeer(*room, role);
    Connection* const tcp = findConnection(peer->tcpSocket);
    if (tcp == nullptr
        || source.sin_addr.s_addr != tcp->remote.sin_addr.s_addr
        || !sameId(peer->token, token)
        || !peer->registrationRate.consume(1, now)) {
        return nullptr;
    }

    const EndpointKey newKey = endpointKey(source);
    const auto existingBinding = udpBindings.find(newKey);
    if (existingBinding != udpBindings.end()
        && (existingBinding->second.lane != Lane::Player
            || !sameId(existingBinding->second.room, room->id)
            || existingBinding->second.role != role)) {
        return nullptr;
    }
    if (peer->udpRegistered && !sameEndpoint(peer->udpEndpoint, source))
        udpBindings.erase(endpointKey(peer->udpEndpoint));

    const bool firstRegistration = !peer->udpRegistered;
    peer->udpRegistered = true;
    peer->udpEndpoint = source;
    peer->lastUdpMs = now;
    UdpBinding binding{};
    binding.room = room->id;
    binding.lane = Lane::Player;
    binding.role = role;
    udpBindings[newKey] = binding;
    room->lastActivityMs = now;
    if (firstRegistration) {
        log(LogLevel::Info,
            "[%s] UDP %s registered from %s",
            roomLabel(*room).c_str(),
            roleText(role),
            endpointText(source).c_str());
    }
    return room;
}

void RelayService::State::processRegister(const Register& registration,
                                          const sockaddr_in& source,
                                          const uint64_t now) {
    if (registration.domain != LaneDomain::Player) {
        ++metrics.crossLaneRejects;
        return;
    }
    Room* const room = bindPlayerRegistration(
        registration.room, registration.role, registration.token, source, now);
    if (room == nullptr) return;
    sendReady(*room, RelayRole::Host);
    sendReady(*room, RelayRole::Client);
}

void RelayService::State::processObservedRequest(
    const qnet::relay::ObservedRequest& request,
    const sockaddr_in& source,
    const uint64_t now) {
    if (request.domain != LaneDomain::Player) {
        ++metrics.crossLaneRejects;
        return;
    }
    Room* const room = bindPlayerRegistration(
        request.room, request.role, request.token, source, now);
    if (room == nullptr) {
        ++metrics.observedRejects;
        return;
    }

    qnet::relay::ObservedEndpoint observed{};
    observed.room = room->id;
    observed.domain = LaneDomain::Player;
    observed.role = request.role;
    observed.token = request.token;
    observed.observed = protocolEndpoint(source);
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeObservedEndpoint(observed, encoded)) {
        ++metrics.observedRejects;
        return;
    }
    const int sent = sendto(
        udpSocket,
        reinterpret_cast<const char*>(encoded.data()),
        static_cast<int>(encoded.size()),
        0,
        reinterpret_cast<const sockaddr*>(&source),
        sizeof(source));
    if (sent != static_cast<int>(encoded.size())) {
        ++metrics.observedRejects;
        return;
    }
    ++metrics.observedRequests;
    if (config.verbose) {
        log(LogLevel::Info,
            "[%s] observed endpoint reported to %s (%s)",
            roomLabel(*room).c_str(),
            endpointText(source).c_str(),
            roleText(request.role));
    }
    sendReady(*room, RelayRole::Host);
    sendReady(*room, RelayRole::Client);
}

void RelayService::State::sendSpectatorReady(Room& room,
                                             ViewerState& viewer) {
    SpectatorLane& lane = room.spectator;
    if (!lane.sourceUdpRegistered || !viewer.udpRegistered) return;

    qnet::relay::SpectatorReady toViewer{};
    toViewer.room = room.id;
    toViewer.party = SpectatorParty::Viewer;
    toViewer.sourceGeneration = lane.generation;
    toViewer.token = viewer.token;
    toViewer.viewerId = viewer.viewerId;
    toViewer.peer = protocolEndpoint(lane.sourceUdpEndpoint);
    std::vector<uint8_t> encoded;
    if (qnet::relay::encodeSpectatorReady(toViewer, encoded)) {
        (void)sendto(
            udpSocket,
            reinterpret_cast<const char*>(encoded.data()),
            static_cast<int>(encoded.size()),
            0,
            reinterpret_cast<const sockaddr*>(&viewer.udpEndpoint),
            sizeof(viewer.udpEndpoint));
    }

    qnet::relay::SpectatorReady toSource{};
    toSource.room = room.id;
    toSource.party = SpectatorParty::Source;
    toSource.sourceGeneration = lane.generation;
    toSource.token = lane.sourceToken;
    toSource.viewerId = viewer.viewerId;
    toSource.peer = protocolEndpoint(viewer.udpEndpoint);
    encoded.clear();
    if (qnet::relay::encodeSpectatorReady(toSource, encoded)) {
        (void)sendto(
            udpSocket,
            reinterpret_cast<const char*>(encoded.data()),
            static_cast<int>(encoded.size()),
            0,
            reinterpret_cast<const sockaddr*>(&lane.sourceUdpEndpoint),
            sizeof(lane.sourceUdpEndpoint));
    }
}

void RelayService::State::processSpectatorRegister(
    const qnet::relay::SpectatorRegister& registration,
    const sockaddr_in& source,
    const uint64_t now) {
    if (!config.spectatorLaneEnabled) return;
    Room* const room = findRoom(registration.room);
    if (room == nullptr) return;
    SpectatorLane& lane = room->spectator;
    if (!lane.sourceRegistered) return;
    if (registration.sourceGeneration != lane.generation) return;

    const EndpointKey newKey = endpointKey(source);
    const auto existingBinding = udpBindings.find(newKey);

    if (registration.party == SpectatorParty::Source) {
        if (!sameId(lane.sourceToken, registration.token)
            || source.sin_addr.s_addr != lane.sourceIpv4
            || !lane.registrationRate.consume(1, now)) {
            return;
        }
        if (existingBinding != udpBindings.end()
            && (existingBinding->second.lane != Lane::Spectator
                || !sameId(existingBinding->second.room, room->id)
                || existingBinding->second.party != SpectatorParty::Source)) {
            ++metrics.crossLaneRejects;
            return;
        }
        if (lane.sourceUdpRegistered
            && !sameEndpoint(lane.sourceUdpEndpoint, source)) {
            udpBindings.erase(endpointKey(lane.sourceUdpEndpoint));
        }
        const bool first = !lane.sourceUdpRegistered;
        lane.sourceUdpRegistered = true;
        lane.sourceUdpEndpoint = source;
        lane.sourceLastUdpMs = now;
        lane.lastActivityMs = now;
        room->lastActivityMs = now;
        UdpBinding binding{};
        binding.room = room->id;
        binding.lane = Lane::Spectator;
        binding.party = SpectatorParty::Source;
        udpBindings[newKey] = binding;
        for (auto& entry : lane.viewers)
            sendSpectatorReady(*room, entry.second);
        if (first) {
            log(LogLevel::Info,
                "[%s] spectator source UDP registered from %s",
                roomLabel(*room).c_str(),
                endpointText(source).c_str());
        }
        return;
    }

    const auto it = lane.viewers.find(registration.viewerId);
    if (it == lane.viewers.end()) return;
    ViewerState& viewer = it->second;
    if (!sameId(viewer.token, registration.token)
        || source.sin_addr.s_addr != viewer.ipv4
        || !viewer.registrationRate.consume(1, now)) {
        return;
    }
    if (existingBinding != udpBindings.end()
        && (existingBinding->second.lane != Lane::Spectator
            || !sameId(existingBinding->second.room, room->id)
            || existingBinding->second.party != SpectatorParty::Viewer
            || existingBinding->second.viewerId != viewer.viewerId)) {
        ++metrics.crossLaneRejects;
        return;
    }
    if (viewer.udpRegistered && !sameEndpoint(viewer.udpEndpoint, source))
        udpBindings.erase(endpointKey(viewer.udpEndpoint));
    viewer.udpRegistered = true;
    viewer.udpEndpoint = source;
    viewer.lastActivityMs = now;
    UdpBinding binding{};
    binding.room = room->id;
    binding.lane = Lane::Spectator;
    binding.party = SpectatorParty::Viewer;
    binding.viewerId = viewer.viewerId;
    udpBindings[newKey] = binding;
    lane.lastActivityMs = now;
    room->lastActivityMs = now;
    sendSpectatorReady(*room, viewer);
}

void RelayService::State::processSpectatorDetach(
    const qnet::relay::SpectatorDetach& detach,
    const sockaddr_in& source,
    const uint64_t now) {
    if (!config.spectatorLaneEnabled) return;
    Room* const room = findRoom(detach.room);
    if (room == nullptr) return;
    SpectatorLane& lane = room->spectator;
    if (!lane.sourceRegistered
        || detach.sourceGeneration != lane.generation) {
        return;
    }
    lane.lastActivityMs = now;

    if (detach.party == SpectatorParty::Source) {
        if (!sameId(lane.sourceToken, detach.token)
            || source.sin_addr.s_addr != lane.sourceIpv4) {
            return;
        }
        if (detach.viewerId != 0u) {
            detachViewer(*room, detach.viewerId, detach.reason, true);
            return;
        }
        const bool reclaimRetainedRoom = room->retainedSpectatorOnly;
        const RoomId roomId = room->id;
        closeSpectatorSource(*room, SpectatorDetachReason::SourceLost, true);
        if (reclaimRetainedRoom)
            closeRoom(roomId, "retained spectator source detached");
        return;
    }
    const auto it = lane.viewers.find(detach.viewerId);
    if (it == lane.viewers.end()) return;
    if (!sameId(it->second.token, detach.token)
        || source.sin_addr.s_addr != it->second.ipv4) {
        return;
    }
    detachViewer(*room, detach.viewerId,
                 SpectatorDetachReason::ViewerRequested, true);
}

void RelayService::State::processPlayerDatagram(const uint8_t* const bytes,
                                                const size_t length,
                                                const UdpBinding& binding,
                                                const sockaddr_in& source,
                                                const uint64_t now) {
    Room* const room = findRoom(binding.room);
    if (room == nullptr || !room->paired) return;
    PeerState* const sender = findPeer(*room, binding.role);
    PeerState* const destination = findPeer(*room, oppositeRole(binding.role));
    if (!sender->udpRegistered || !destination->udpRegistered) return;
    if (!sameEndpoint(sender->udpEndpoint, source)) return;
    if (!sender->packetRate.consume(1, now)
        || !sender->byteRate.consume(length, now)) {
        ++metrics.rateLimitDrops;
        ++metrics.drops;
        return;
    }

    ++metrics.playerPacketsIn;
    metrics.playerBytesIn += length;

    PlayerDatagramInfo info{};
    if (qnetplay::relay::server::inspectPlayerDatagram(
            bytes, length, info) != PlayerDatagramError::None) {
        ++metrics.drops;
        return;
    }
    if (info.carriesRole) {
        if (info.role != playerRole(binding.role)) {
            ++metrics.drops;
            return;
        }
        if ((binding.role == RelayRole::Client
                && info.type != qnetplay::wire::MessageType::Hello)
            || (binding.role == RelayRole::Host
                && info.type != qnetplay::wire::MessageType::HelloAck)) {
            ++metrics.drops;
            return;
        }
    }

    if (!room->playerIdentityBound) {
        if (binding.role != RelayRole::Client
            || info.type != qnetplay::wire::MessageType::Hello) {
            ++metrics.drops;
            return;
        }
        room->playerIdentityBound = true;
        room->playerSessionId = info.sessionId;
        room->playerCookie = info.cookie;
    } else {
        const std::array<uint64_t, 2> expected{
            room->playerCookie.low,
            room->playerCookie.high,
        };
        const std::array<uint64_t, 2> received{
            info.cookie.low,
            info.cookie.high,
        };
        if (info.sessionId != room->playerSessionId
            || !qnetplay::relay::server::constantTimeEqual(
                expected.data(), received.data(), sizeof(expected))) {
            ++metrics.drops;
            return;
        }
    }

    const int sent = sendto(
        udpSocket,
        reinterpret_cast<const char*>(bytes),
        static_cast<int>(length),
        0,
        reinterpret_cast<const sockaddr*>(&destination->udpEndpoint),
        sizeof(destination->udpEndpoint));
    if (sent == static_cast<int>(length)) {
        ++metrics.playerPacketsOut;
        metrics.playerBytesOut += length;
        sender->lastUdpMs = now;
        room->lastActivityMs = now;
    }
}

void RelayService::State::processSpectatorFrame(const uint8_t* const bytes,
                                                const size_t length,
                                                const UdpBinding& binding,
                                                const sockaddr_in& source,
                                                const uint64_t now) {
    if (!config.spectatorLaneEnabled) return;
    Room* const room = findRoom(binding.room);
    if (room == nullptr) return;
    SpectatorLane& lane = room->spectator;
    if (!lane.sourceRegistered) return;

    SpectatorFrameHeader header{};
    if (qnet::relay::decodeSpectatorFrameHeader(bytes, length, header)
        != qnet::relay::DecodeError::None) {
        ++metrics.drops;
        return;
    }
    if (header.party != binding.party
        || header.sourceGeneration != lane.generation) {
        ++metrics.drops;
        return;
    }

    const uint8_t* const inner = bytes + qnet::relay::kSpectatorFrameHeaderSize;
    const size_t innerLength =
        length - qnet::relay::kSpectatorFrameHeaderSize;

    ++metrics.spectatorPacketsIn;
    metrics.spectatorBytesIn += innerLength;
    lane.bytesIn += innerLength;

    sockaddr_in destination{};
    if (binding.party == SpectatorParty::Source) {
        if (!sameEndpoint(lane.sourceUdpEndpoint, source)) return;
        if (!lane.packetRate.consume(1, now)
            || !lane.byteRate.consume(length, now)) {
            ++metrics.rateLimitDrops;
            ++metrics.drops;
            return;
        }
        SpectatorDatagramInfo info{};
        if (qnetplay::relay::server::inspectSpectatorDatagram(
                inner, innerLength, qsp::PeerRole::Source, info)
            != SpectatorDatagramError::None) {
            ++metrics.drops;
            return;
        }
        if (!lane.identityBound) {
            if (info.identity.sessionId != lane.sessionId) {
                ++metrics.drops;
                return;
            }
            lane.identityBound = true;
            lane.identity = info.identity;
        } else if (info.identity != lane.identity) {
            ++metrics.drops;
            return;
        }
        const auto it = lane.viewers.find(header.viewerId);
        if (it == lane.viewers.end() || !it->second.udpRegistered) {
            ++metrics.drops;
            return;
        }
        if (info.carriesViewerId && info.viewerId != header.viewerId) {
            ++metrics.drops;
            return;
        }
        destination = it->second.udpEndpoint;
        it->second.bytesOut += innerLength;
        it->second.lastActivityMs = now;
    } else {
        const auto it = lane.viewers.find(binding.viewerId);
        if (it == lane.viewers.end()) return;
        ViewerState& viewer = it->second;
        if (header.viewerId != viewer.viewerId) {
            ++metrics.drops;
            return;
        }
        if (!viewer.udpRegistered || !sameEndpoint(viewer.udpEndpoint, source))
            return;
        if (!lane.sourceUdpRegistered) {
            ++metrics.drops;
            return;
        }
        if (!viewer.packetRate.consume(1, now)
            || !viewer.byteRate.consume(length, now)) {
            ++metrics.rateLimitDrops;
            ++metrics.drops;
            return;
        }
        SpectatorDatagramInfo info{};
        if (qnetplay::relay::server::inspectSpectatorDatagram(
                inner, innerLength, qsp::PeerRole::Viewer, info)
            != SpectatorDatagramError::None) {
            ++metrics.drops;
            return;
        }
        if (lane.identityBound) {
            if (info.identity != lane.identity) {
                ++metrics.drops;
                return;
            }
        } else if (info.identity.sessionId != lane.sessionId) {
            ++metrics.drops;
            return;
        }
        if (info.carriesViewerId && info.viewerId != viewer.viewerId) {
            ++metrics.drops;
            return;
        }
        destination = lane.sourceUdpEndpoint;
        viewer.bytesIn += innerLength;
        viewer.lastActivityMs = now;
    }

    const int sent = sendto(
        udpSocket,
        reinterpret_cast<const char*>(bytes),
        static_cast<int>(length),
        0,
        reinterpret_cast<const sockaddr*>(&destination),
        sizeof(destination));
    if (sent == static_cast<int>(length)) {
        ++metrics.spectatorPacketsOut;
        metrics.spectatorBytesOut += innerLength;
        lane.bytesOut += innerLength;
        lane.lastActivityMs = now;
        room->lastActivityMs = now;
    }
}

void RelayService::State::receiveUdp(const uint64_t now) {
    const uint32_t maximumReceivePackets =
        config.maximumUdpPacketsPerTurn * 2u
        + config.maximumUnboundPacketsPerTurn;
    uint32_t playerTurnPackets = 0;
    uint32_t spectatorTurnPackets = 0;
    uint32_t topologyTurnPackets = 0;
    for (uint32_t packet = 0;
         packet < maximumReceivePackets;
         ++packet) {
        std::array<uint8_t, kMaximumUdpFrameBytes + 1> bytes{};
        sockaddr_in source{};
        int sourceLength = sizeof(source);
        const int received = recvfrom(
            udpSocket,
            reinterpret_cast<char*>(bytes.data()),
            static_cast<int>(bytes.size()),
            0,
            reinterpret_cast<sockaddr*>(&source),
            &sourceLength);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEMSGSIZE
                && error != WSAECONNRESET) {
                log(LogLevel::Warn, "UDP receive failed: %d", error);
            }
            if (error == WSAEMSGSIZE || error == WSAECONNRESET) {
                ++metrics.drops;
                continue;
            }
            return;
        }
        if (received <= 0) return;

        const size_t length = static_cast<size_t>(received);
        uint16_t version = 0;
        const bool relayRecord =
            qnet::relay::peekRelayVersion(bytes.data(), length, version);

        if (relayRecord
            && !qnet::relay::hasSpectatorFrameKind(bytes.data(), length)
            && !qnet::relay::hasTopologyFrameKind(bytes.data(), length)) {
            if (unboundTurnPackets >= config.maximumUnboundPacketsPerTurn) {
                ++metrics.drops;
                continue;
            }
            ++unboundTurnPackets;
            if (version == qnet::relay::kVersion) {
                Register registration{};
                if (qnet::relay::decodeRegister(
                        bytes.data(), length, registration)
                    == qnet::relay::DecodeError::None) {
                    processRegister(registration, source, now);
                }
            } else if (version == qnet::relay::kSpectatorVersion) {
                qnet::relay::SpectatorMessageKind kind{};
                const qnet::relay::DecodeError spectatorKindError =
                    qnet::relay::inspectSpectatorMessageKind(
                        bytes.data(), length, kind);
                if (spectatorKindError
                        == qnet::relay::DecodeError::None
                    && kind
                        == qnet::relay::SpectatorMessageKind::Register) {
                    qnet::relay::SpectatorRegister registration{};
                    if (qnet::relay::decodeSpectatorRegister(
                            bytes.data(), length, registration)
                        == qnet::relay::DecodeError::None) {
                        processSpectatorRegister(registration, source, now);
                    }
                } else if (spectatorKindError
                               == qnet::relay::DecodeError::None
                           && kind
                    == qnet::relay::SpectatorMessageKind::Detach) {
                    qnet::relay::SpectatorDetach detach{};
                    if (qnet::relay::decodeSpectatorDetach(
                            bytes.data(), length, detach)
                        == qnet::relay::DecodeError::None) {
                        processSpectatorDetach(detach, source, now);
                    }
                } else if (spectatorKindError
                               != qnet::relay::DecodeError::None
                           && qnet::relay::kSpectatorVersion
                                  == qnet::relay::kObservedVersion) {
                    // Spectator wire v3 and the observed-endpoint extension share protocol version 3 with disjoint kinds (0x11..0x16 vs 0x30..0x31): demultiplex by kind/shape.
                    qnet::relay::ObservedRequest request{};
                    if (qnet::relay::decodeObservedRequest(
                            bytes.data(), length, request)
                        == qnet::relay::DecodeError::None) {
                        processObservedRequest(request, source, now);
                    } else {
                        ++metrics.drops;
                    }
                } else {
                    ++metrics.drops;
                }
            } else if (version == qnet::relay::kTopologyVersion) {
                qnet::relay::TopologyMessageKind kind{};
                const qnet::relay::DecodeError topologyKindError =
                    qnet::relay::inspectTopologyMessageKind(
                        bytes.data(), length, kind);
                if (topologyKindError == qnet::relay::DecodeError::None
                    && kind == qnet::relay::TopologyMessageKind::Hello) {
                    qnet::relay::TopologyHello topologyHello{};
                    if (qnet::relay::decodeTopologyHello(
                            bytes.data(), length, topologyHello)
                        == qnet::relay::DecodeError::None) {
                        processTopologyHelloDatagram(
                            topologyHello, source, now);
                    }
                } else if (topologyKindError
                               == qnet::relay::DecodeError::None
                    && kind == qnet::relay::TopologyMessageKind::Register) {
                    qnet::relay::TopologyRegister registration{};
                    if (qnet::relay::decodeTopologyRegister(
                            bytes.data(), length, registration)
                        == qnet::relay::DecodeError::None) {
                        processTopologyRegister(registration, source, now);
                    }
                } else if (topologyKindError
                               == qnet::relay::DecodeError::None
                           && kind
                               == qnet::relay::TopologyMessageKind::Detach) {
                    qnet::relay::TopologyDetach detach{};
                    if (qnet::relay::decodeTopologyDetach(
                            bytes.data(), length, detach)
                        == qnet::relay::DecodeError::None) {
                        processTopologyDetach(detach, source, now);
                    }
                } else {
                    ++metrics.drops;
                }
            } else if (version == qnet::relay::kObservedVersion) {
                qnet::relay::ObservedRequest request{};
                if (qnet::relay::decodeObservedRequest(
                        bytes.data(), length, request)
                    == qnet::relay::DecodeError::None) {
                    processObservedRequest(request, source, now);
                } else {
                    ++metrics.drops;
                }
            } else {
                ++metrics.drops;
            }
            continue;
        }

        const auto bindingIt = udpBindings.find(endpointKey(source));
        if (bindingIt == udpBindings.end()) {
            if (unboundTurnPackets >= config.maximumUnboundPacketsPerTurn) {
                ++metrics.drops;
                continue;
            }
            ++unboundTurnPackets;
            ++metrics.drops;
            continue;
        }
        const UdpBinding binding = bindingIt->second;
        Room* const room = findRoom(binding.room);
        if (room == nullptr) {
            udpBindings.erase(bindingIt);
            ++metrics.drops;
            continue;
        }
        uint32_t& laneTurnPackets = binding.lane == Lane::Player
            ? playerTurnPackets
            : binding.lane == Lane::Spectator
                ? spectatorTurnPackets
                : topologyTurnPackets;
        uint32_t& roomTurnPackets = binding.lane == Lane::Player
            ? room->turnPackets
            : binding.lane == Lane::Spectator
                ? room->spectator.turnPackets
                : room->topology.turnPackets;
        uint32_t& roomTurnBytes = binding.lane == Lane::Player
            ? room->turnBytes
            : binding.lane == Lane::Spectator
                ? room->spectator.turnBytes
                : room->topology.turnBytes;
        if (laneTurnPackets >= config.maximumUdpPacketsPerTurn) {
            ++metrics.drops;
            continue;
        }
        ++laneTurnPackets;
        if (roomTurnPackets >= config.maximumUdpPacketsPerRoomPerTurn
            || roomTurnBytes + length
                > config.maximumUdpBytesPerRoomPerTurn) {
            ++metrics.roomBudgetDrops;
            ++metrics.drops;
            continue;
        }
        ++roomTurnPackets;
        roomTurnBytes += static_cast<uint32_t>(length);

        if (binding.lane == Lane::Player) {
            if (relayRecord) {
                ++metrics.crossLaneRejects;
                ++metrics.drops;
                continue;
            }
            processPlayerDatagram(bytes.data(), length, binding, source, now);
        } else if (binding.lane == Lane::Spectator) {
            if (!relayRecord || !qnet::relay::hasSpectatorFrameKind(
                    bytes.data(), length)) {
                ++metrics.crossLaneRejects;
                ++metrics.drops;
                continue;
            }
            processSpectatorFrame(bytes.data(), length, binding, source, now);
        } else {
            if (!relayRecord || !qnet::relay::hasTopologyFrameKind(
                    bytes.data(), length)) {
                ++metrics.crossLaneRejects;
                ++metrics.drops;
                continue;
            }
            processTopologyFrame(bytes.data(), length, binding, source, now);
        }
    }
}

void RelayService::State::closeSingle(const SOCKET socket,
                                      const char* const reason) {
    const auto it = connections.find(socket);
    if (it == connections.end()) return;
    const uint32_t ipv4 = it->second.remote.sin_addr.s_addr;
    if (config.verbose && reason != nullptr && reason[0] != '\0') {
        log(LogLevel::Info,
            "[tcp %s] closed: %s",
            endpointText(it->second.remote).c_str(),
            reason);
    }
    closesocket(it->second.socket);
    connections.erase(it);
    releaseIp(ipv4, true, false, false);
}

void RelayService::State::closeRoom(const RoomId& roomId,
                                    const char* const reason) {
    const auto roomIt = rooms.find(roomId);
    if (roomIt == rooms.end()) return;
    closeSpectatorSource(
        roomIt->second, SpectatorDetachReason::SourceLost, true);
    closeTopologyAuthority(
        roomIt->second, TopologyDetachReason::AuthorityLost, true);

    const Room room = roomIt->second;
    rooms.erase(roomIt);
    for (const PeerState& peer : room.peers) {
        if (peer.udpRegistered)
            udpBindings.erase(endpointKey(peer.udpEndpoint));
    }
    for (const PeerState& peer : room.peers) {
        const auto connectionIt = connections.find(peer.tcpSocket);
        if (connectionIt != connections.end()) {
            const uint32_t ipv4 = connectionIt->second.remote.sin_addr.s_addr;
            closesocket(connectionIt->second.socket);
            connections.erase(connectionIt);
            releaseIp(ipv4, true, false, false);
        }
    }
    releaseIp(room.ownerIpv4, false, true, false);
    ++metrics.roomsClosed;
    log(LogLevel::Info,
        "[room #%llu/%08X] closed: %s",
        static_cast<unsigned long long>(room.ordinal),
        room.tag,
        reason != nullptr && reason[0] != '\0' ? reason : "closed");
}

void RelayService::State::retainSpectatorOnly(
    Room& room,
    const char* const reason) {
    if (room.retainedSpectatorOnly) return;
    if (!room.spectator.sourceRegistered) {
        closeRoom(room.id, reason);
        return;
    }

    closeTopologyAuthority(room, TopologyDetachReason::AuthorityLost, true);
    const uint64_t now = monotonicMs();
    for (PeerState& peer : room.peers) {
        if (peer.udpRegistered)
            udpBindings.erase(endpointKey(peer.udpEndpoint));
        const SOCKET peerSocket = peer.tcpSocket;
        peer = PeerState{};
        if (peerSocket == INVALID_SOCKET) continue;
        Connection* const connection = findConnection(peerSocket);
        if (connection != nullptr) {
            connection->assignedRoom = false;
            closeSingle(peerSocket, "player session ended; spectator retained");
        }
    }
    room.paired = false;
    room.retainedSpectatorOnly = true;
    room.playerIdentityBound = false;
    room.playerSessionId = 0;
    room.playerCookie = {};
    room.lastActivityMs = now;
    log(LogLevel::Info,
        "[room #%llu/%08X] player session ended; retaining spectator source",
        static_cast<unsigned long long>(room.ordinal),
        room.tag);
}

void RelayService::State::closeConnection(const SOCKET socket,
                                          const char* const reason) {
    Connection* const connection = findConnection(socket);
    if (connection == nullptr) return;
    if (connection->lane == Lane::Topology && connection->assignedRoom) {
        const RoomId id = connection->room;
        const TopologyParty party = connection->topologyParty;
        const uint64_t nodeId = connection->nodeId;
        Room* const room = findRoom(id);
        if (room != nullptr) {
            if (party == TopologyParty::Authority) {
                closeTopologyAuthority(
                    *room, TopologyDetachReason::AuthorityLost, true);
            } else {
                detachTopologyNode(*room, nodeId,
                                   TopologyDetachReason::NodeRequested,
                                   true);
            }
        }
        closeSingle(socket, reason);
        return;
    }
    if (connection->lane == Lane::Spectator && connection->assignedRoom) {
        const RoomId id = connection->room;
        const SpectatorParty party = connection->party;
        const uint64_t viewerId = connection->viewerId;
        Room* const room = findRoom(id);
        if (room != nullptr) {
            if (party == SpectatorParty::Source) {
                const bool reclaimRetainedRoom =
                    room->retainedSpectatorOnly;
                closeSpectatorSource(
                    *room, SpectatorDetachReason::SourceLost, true);
                if (reclaimRetainedRoom)
                    closeRoom(id, "retained spectator source lease closed");
            } else {
                detachViewer(*room, viewerId,
                             SpectatorDetachReason::ViewerRequested, true);
            }
        }
        closeSingle(socket, reason);
        return;
    }
    if (connection->assignedRoom) {
        const RoomId id = connection->room;
        Room* const room = findRoom(id);
        if (room != nullptr && room->spectator.sourceRegistered)
            retainSpectatorOnly(*room, reason);
        else
            closeRoom(id, reason);
    } else {
        closeSingle(socket, reason);
    }
}

// Stamps can be ahead of the turn's captured `now`; unsigned subtraction must not wrap here.
static uint64_t elapsedSince(const uint64_t now, const uint64_t stamp) {
    return now > stamp ? now - stamp : 0u;
}

void RelayService::State::cleanupExpired(const uint64_t now) {
    std::vector<SOCKET> staleConnections;
    for (const auto& entry : connections) {
        const Connection& connection = entry.second;
        if (!connection.joined
            && elapsedSince(now, connection.acceptedMs) > config.joinTimeoutMs) {
            staleConnections.push_back(entry.first);
        }
    }
    for (const SOCKET socket : staleConnections)
        closeSingle(socket, "relay Hello timeout");

    const uint64_t nowUnix = wallUnixSeconds();
    std::vector<std::pair<RoomId, uint64_t>> expiredViewers;
    std::vector<RoomId> expiredSources;
    for (auto& entry : rooms) {
        SpectatorLane& lane = entry.second.spectator;
        if (!lane.sourceRegistered) continue;
        for (const auto& viewerEntry : lane.viewers) {
            const ViewerState& viewer = viewerEntry.second;
            if (viewer.expiryUnix != 0 && nowUnix >= viewer.expiryUnix) {
                expiredViewers.emplace_back(entry.first, viewer.viewerId);
            } else if (!viewer.udpRegistered
                && elapsedSince(now, viewer.admittedMs)
                       >= config.spectatorRegistrationTimeoutMs) {
                expiredViewers.emplace_back(entry.first, viewer.viewerId);
            } else if (viewer.udpRegistered
                && elapsedSince(now, viewer.lastActivityMs) > config.viewerIdleTimeoutMs) {
                expiredViewers.emplace_back(entry.first, viewer.viewerId);
            }
        }
        const uint64_t sourceRegistrationStamp =
            lane.sourceUdpRegistered
                ? lane.sourceLastUdpMs : lane.sourceAdmittedMs;
        if (sourceRegistrationStamp != 0
            && elapsedSince(now, sourceRegistrationStamp)
                   > config.registrationTimeoutMs) {
            expiredSources.push_back(entry.first);
        }
        for (auto it = lane.usedGrantIds.begin();
             it != lane.usedGrantIds.end();) {
            if (nowUnix > it->second + config.maximumGrantLifetimeSeconds * 2u)
                it = lane.usedGrantIds.erase(it);
            else
                ++it;
        }
    }
    for (const auto& expired : expiredViewers) {
        Room* const room = findRoom(expired.first);
        if (room != nullptr) {
            detachViewer(*room, expired.second,
                         SpectatorDetachReason::Expired, true);
        }
    }
    for (const RoomId& roomId : expiredSources) {
        Room* const room = findRoom(roomId);
        if (room == nullptr || !room->spectator.sourceRegistered) continue;
        const bool reclaimRetainedRoom = room->retainedSpectatorOnly;
        closeSpectatorSource(
            *room, SpectatorDetachReason::Expired, true);
        if (reclaimRetainedRoom)
            closeRoom(roomId, "retained spectator source expired");
    }

    std::vector<std::pair<RoomId, uint64_t>> expiredNodes;
    std::vector<RoomId> expiredAuthorities;
    for (auto& entry : rooms) {
        TopologyLane& lane = entry.second.topology;
        if (!lane.authorityRegistered) continue;
        for (const auto& nodeEntry : lane.nodes) {
            const NodeState& node = nodeEntry.second;
            if (node.expiryUnix != 0 && nowUnix >= node.expiryUnix) {
                expiredNodes.emplace_back(entry.first, node.nodeId);
            } else if (!node.udpRegistered
                && elapsedSince(now, node.admittedMs)
                       >= config.spectatorRegistrationTimeoutMs) {
                expiredNodes.emplace_back(entry.first, node.nodeId);
            } else if (node.udpRegistered
                && elapsedSince(now, node.lastActivityMs)
                       > config.viewerIdleTimeoutMs) {
                expiredNodes.emplace_back(entry.first, node.nodeId);
            }
        }
        const uint64_t authorityStamp = lane.authorityUdpRegistered
            ? lane.authorityLastUdpMs
            : lane.authorityAdmittedMs;
        if (authorityStamp != 0
            && elapsedSince(now, authorityStamp)
                   > config.registrationTimeoutMs) {
            expiredAuthorities.push_back(entry.first);
        }
        for (auto it = lane.usedGrantIds.begin();
             it != lane.usedGrantIds.end();) {
            if (nowUnix > it->second + config.maximumGrantLifetimeSeconds * 2u)
                it = lane.usedGrantIds.erase(it);
            else
                ++it;
        }
    }
    for (const auto& expired : expiredNodes) {
        Room* const room = findRoom(expired.first);
        if (room != nullptr) {
            detachTopologyNode(*room, expired.second,
                               TopologyDetachReason::Expired, true);
        }
    }
    for (const RoomId& roomId : expiredAuthorities) {
        Room* const room = findRoom(roomId);
        if (room == nullptr || !room->topology.authorityRegistered) continue;
        closeTopologyAuthority(
            *room, TopologyDetachReason::Expired, true);
    }

    std::vector<std::pair<RoomId, const char*>> staleRooms;
    for (const auto& entry : rooms) {
        const Room& room = entry.second;
        if (elapsedSince(now, room.lastActivityMs) > config.maximumRoomIdleMs) {
            staleRooms.emplace_back(entry.first,
                                    "maximum idle lifetime reached");
        } else if (!room.paired && !room.retainedSpectatorOnly
            && elapsedSince(now, room.createdMs) > config.pairTimeoutMs) {
            staleRooms.emplace_back(entry.first, "pair timeout");
        } else if (room.retainedSpectatorOnly
            && !room.spectator.sourceRegistered) {
            staleRooms.emplace_back(
                entry.first, "retained spectator source unavailable");
        } else if (room.paired
            && (!room.peers[0].udpRegistered
                || !room.peers[1].udpRegistered)
            && elapsedSince(now, room.lastActivityMs) > config.registrationTimeoutMs) {
            staleRooms.emplace_back(
                entry.first, "UDP registration timeout");
        }
    }
    for (const auto& stale : staleRooms)
        closeRoom(stale.first, stale.second);

    pruneIdleIpStates(now);
}

void RelayService::State::shutdownSockets() {
    for (auto& entry : connections) {
        if (entry.second.socket != INVALID_SOCKET)
            closesocket(entry.second.socket);
    }
    connections.clear();
    rooms.clear();
    udpBindings.clear();
    ips.clear();
    totalViewers = 0;
    spectatorRooms = 0;
    if (udpSocket != INVALID_SOCKET) {
        closesocket(udpSocket);
        udpSocket = INVALID_SOCKET;
    }
    if (tcpListener != INVALID_SOCKET) {
        closesocket(tcpListener);
        tcpListener = INVALID_SOCKET;
    }
    if (winsockStarted) {
        WSACleanup();
        winsockStarted = false;
    }
    started = false;
}

void RelayService::State::recordLatency(const uint64_t micros) {
    latency[latencyIndex] = micros;
    latencyIndex = (latencyIndex + 1u) % latency.size();
    if (latencyCount < latency.size()) ++latencyCount;
}

uint64_t RelayService::State::percentileLatency(
    const unsigned percentile) const {
    if (latencyCount == 0) return 0;
    std::vector<uint64_t> samples(
        latency.begin(),
        latency.begin() + static_cast<std::ptrdiff_t>(latencyCount));
    size_t index = (samples.size() * percentile) / 100u;
    if (index >= samples.size()) index = samples.size() - 1u;
    std::nth_element(
        samples.begin(),
        samples.begin() + static_cast<std::ptrdiff_t>(index),
        samples.end());
    return samples[index];
}

void RelayService::State::emitHealth(const bool final) {
    RelayServiceMetrics current = metrics;
    current.uptimeMs = monotonicMs() - startedMs;
    current.rooms = static_cast<uint32_t>(rooms.size());
    current.connections = static_cast<uint32_t>(connections.size());
    current.viewers = totalViewers;
    current.spectatorRooms = spectatorRooms;
    current.trackedIpStates = static_cast<uint32_t>(ips.size());
    current.p95TurnMicros = percentileLatency(95);

    uint32_t paired = 0;
    uint32_t pending = 0;
    for (const auto& entry : rooms) {
        if (entry.second.paired) ++paired;
    }
    for (const auto& entry : connections) {
        if (!entry.second.joined) ++pending;
    }
    log(LogLevel::Info,
        "health%s uptime=%llus rooms=%u/%u spectator_rooms=%u viewers=%u "
        "conn=%u pending=%u ips=%u/%u player_in=%lluKiB player_out=%lluKiB "
        "spec_in=%lluKiB spec_out=%lluKiB drops=%llu(budget=%llu rate=%llu) "
        "rejects=%llu(ip=%llu cap=%llu grant=%llu expired=%llu replay=%llu "
        "cross=%llu) observed=%llu/%llu p95=%lluus max=%lluus",
        final ? " final" : "",
        static_cast<unsigned long long>(current.uptimeMs / 1000ull),
        paired,
        current.rooms,
        current.spectatorRooms,
        current.viewers,
        current.connections,
        pending,
        current.trackedIpStates,
        config.maximumTrackedIpStates,
        static_cast<unsigned long long>(current.playerBytesIn / 1024ull),
        static_cast<unsigned long long>(current.playerBytesOut / 1024ull),
        static_cast<unsigned long long>(current.spectatorBytesIn / 1024ull),
        static_cast<unsigned long long>(current.spectatorBytesOut / 1024ull),
        static_cast<unsigned long long>(current.drops),
        static_cast<unsigned long long>(current.roomBudgetDrops),
        static_cast<unsigned long long>(current.rateLimitDrops),
        static_cast<unsigned long long>(current.rejects),
        static_cast<unsigned long long>(current.ipQuotaRejects),
        static_cast<unsigned long long>(current.capacityRejects),
        static_cast<unsigned long long>(current.grantRejects),
        static_cast<unsigned long long>(current.grantExpiredRejects),
        static_cast<unsigned long long>(current.grantReplayRejects),
        static_cast<unsigned long long>(current.crossLaneRejects),
        static_cast<unsigned long long>(current.observedRequests),
        static_cast<unsigned long long>(current.observedRejects),
        static_cast<unsigned long long>(current.p95TurnMicros),
        static_cast<unsigned long long>(current.maxTurnMicros));
}

RelayService::RelayService() : state_(new State()) {}

RelayService::~RelayService() {
    if (state_) state_->shutdownSockets();
}

bool RelayService::configure(const RelayServiceConfig& config,
                             std::string& error) {
    if (state_->started) {
        error = "the service is already started";
        return false;
    }
    if (!validateRelayServiceConfig(config, error))
        return false;
    state_->config = config;
    state_->configured = true;
    state_->logger.configure(
        config.logPath,
        config.logMaximumTotalBytes,
        config.logFiles,
        config.logToConsole);
    return true;
}

const RelayServiceConfig& RelayService::config() const {
    return state_->config;
}

void RelayService::setLogSink(const LogSink sink, void* const user) {
    state_->logger.setSink(sink, user);
}

void RelayService::setSpectatorGrantVerifier(
    const SpectatorGrantVerifier* const verifier) {
    state_->verifier = verifier;
}

void RelayService::setTopologyGrantVerifier(
    const TopologyGrantVerifier* const verifier) {
    state_->topologyVerifier = verifier;
}

void RelayService::setMonotonicClock(uint64_t (*clockMs)()) {
    if (clockMs != nullptr) state_->monotonicMs = clockMs;
}

void RelayService::setWallClock(uint64_t (*clockUnixSeconds)()) {
    if (clockUnixSeconds != nullptr) state_->wallUnixSeconds = clockUnixSeconds;
}

bool RelayService::start(std::string& error) {
    State& s = *state_;
    error.clear();
    if (!s.configured) {
        error = "configure() must succeed before start()";
        return false;
    }
    if (s.started) {
        error = "the service is already started";
        return false;
    }

    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        error = "WSAStartup failed";
        return false;
    }
    s.winsockStarted = true;

    s.boundEndpoint = {};
    s.boundEndpoint.sin_family = AF_INET;
    s.boundEndpoint.sin_port = htons(s.config.port);
    if (InetPtonA(
            AF_INET,
            s.config.bindAddress.c_str(),
            &s.boundEndpoint.sin_addr) != 1) {
        error = "--bind requires a numeric IPv4 address: "
            + s.config.bindAddress;
        s.shutdownSockets();
        return false;
    }

    char detail[192] = {};
    s.tcpListener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s.tcpListener == INVALID_SOCKET) {
        std::snprintf(detail, sizeof(detail),
                      "TCP socket() failed: %d", WSAGetLastError());
        error = detail;
        s.shutdownSockets();
        return false;
    }
    BOOL exclusive = TRUE;
    (void)setsockopt(
        s.tcpListener,
        SOL_SOCKET,
        SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive),
        sizeof(exclusive));
    if (bind(
            s.tcpListener,
            reinterpret_cast<const sockaddr*>(&s.boundEndpoint),
            sizeof(s.boundEndpoint)) != 0
        || listen(s.tcpListener, SOMAXCONN) != 0
        || !setNonBlocking(s.tcpListener)) {
        std::snprintf(detail, sizeof(detail),
                      "TCP bind/listen failed on %s:%u: %d",
                      s.config.bindAddress.c_str(),
                      static_cast<unsigned>(s.config.port),
                      WSAGetLastError());
        error = detail;
        s.shutdownSockets();
        return false;
    }

    s.udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s.udpSocket == INVALID_SOCKET) {
        std::snprintf(detail, sizeof(detail),
                      "UDP socket() failed: %d", WSAGetLastError());
        error = detail;
        s.shutdownSockets();
        return false;
    }
    (void)setsockopt(
        s.udpSocket,
        SOL_SOCKET,
        SO_EXCLUSIVEADDRUSE,
        reinterpret_cast<const char*>(&exclusive),
        sizeof(exclusive));
    int receiveBuffer = 1024 * 1024;
    int sendBuffer = 1024 * 1024;
    (void)setsockopt(
        s.udpSocket,
        SOL_SOCKET,
        SO_RCVBUF,
        reinterpret_cast<const char*>(&receiveBuffer),
        sizeof(receiveBuffer));
    (void)setsockopt(
        s.udpSocket,
        SOL_SOCKET,
        SO_SNDBUF,
        reinterpret_cast<const char*>(&sendBuffer),
        sizeof(sendBuffer));
    if (bind(
            s.udpSocket,
            reinterpret_cast<const sockaddr*>(&s.boundEndpoint),
            sizeof(s.boundEndpoint)) != 0
        || !setNonBlocking(s.udpSocket)) {
        std::snprintf(detail, sizeof(detail),
                      "UDP bind failed on %s:%u: %d",
                      s.config.bindAddress.c_str(),
                      static_cast<unsigned>(s.config.port),
                      WSAGetLastError());
        error = detail;
        s.shutdownSockets();
        return false;
    }

    sockaddr_in actual{};
    int actualLength = sizeof(actual);
    s.boundPort = s.config.port;
    if (getsockname(
            s.tcpListener,
            reinterpret_cast<sockaddr*>(&actual),
            &actualLength) == 0) {
        s.boundPort = ntohs(actual.sin_port);
    }

    std::array<uint8_t, 8> salt{};
    if (BCryptGenRandom(
            nullptr,
            salt.data(),
            static_cast<ULONG>(salt.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0) {
        std::memcpy(&s.roomTagSalt, salt.data(), sizeof(s.roomTagSalt));
    }

    s.started = true;
    s.draining = false;
    s.fatal = false;
    s.stop.store(false, std::memory_order_release);
    s.startedMs = s.monotonicMs();
    s.lastCleanupMs = s.startedMs;
    s.lastMetricsMs = s.startedMs;

    s.log(LogLevel::Info,
          "qoh99 relay service listening on %s:%u (TCP+UDP)",
          s.config.bindAddress.c_str(),
          static_cast<unsigned>(s.boundPort));
    s.log(LogLevel::Info,
          "player lane: rooms=%u pair=%us register=%us "
          "udp=%llu packets/s, %llu KiB/s",
          s.config.maximumRooms,
          s.config.pairTimeoutMs / 1000u,
          s.config.registrationTimeoutMs / 1000u,
          static_cast<unsigned long long>(s.config.roomUdpPacketsPerSecond),
          static_cast<unsigned long long>(
              s.config.roomUdpBytesPerSecond / 1024ull));
    if (!s.config.spectatorLaneEnabled) {
        s.log(LogLevel::Info,
              "spectator lane: disabled by --no-spectator");
    } else if (!spectatorGrantVerifierAvailable() && s.verifier == nullptr) {
        s.log(LogLevel::Warn,
              "spectator lane: requested but no grant verifier is linked; "
              "every viewer will receive Unsupported");
    } else {
        s.log(LogLevel::Info,
              "spectator lane: enabled wire=v%u rooms=%u viewers=%u "
              "per-room=%u per-ip=%u",
              static_cast<unsigned>(qnet::relay::kSpectatorVersion),
              s.config.maximumSpectatorRooms,
              s.config.maximumViewers,
              s.config.maximumViewersPerRoom,
              s.config.maximumViewersPerIp);
    }
    return true;
}

bool RelayService::started() const {
    return state_->started;
}

uint16_t RelayService::boundPort() const {
    return state_->boundPort;
}

void RelayService::requestStop() {
    state_->stop.store(true, std::memory_order_release);
}

bool RelayService::stopRequested() const {
    return state_->stop.load(std::memory_order_acquire);
}

bool RelayService::pumpOnce(const int pollTimeoutMs) {
    State& s = *state_;
    if (!s.started) return false;
    const uint64_t turnBegin = microsecondsNow();
    const uint64_t now = s.monotonicMs();

    for (auto& entry : s.rooms) {
        entry.second.turnPackets = 0;
        entry.second.turnBytes = 0;
        entry.second.spectator.turnPackets = 0;
        entry.second.spectator.turnBytes = 0;
        entry.second.topology.turnPackets = 0;
        entry.second.topology.turnBytes = 0;
    }
    for (auto& entry : s.connections) entry.second.turnBytes = 0;
    s.unboundTurnPackets = 0;

    std::vector<WSAPOLLFD> poll;
    poll.reserve(s.connections.size() + 2u);
    poll.push_back(WSAPOLLFD{
        s.draining ? INVALID_SOCKET : s.tcpListener,
        POLLRDNORM,
        0,
    });
    poll.push_back(WSAPOLLFD{
        s.udpSocket,
        s.draining ? static_cast<short>(0) : static_cast<short>(POLLRDNORM),
        0,
    });
    for (const auto& entry : s.connections) {
        const Connection& connection = entry.second;
        short events = s.draining ? 0 : POLLRDNORM;
        if (connection.sendOffset < connection.sendBytes.size())
            events = static_cast<short>(events | POLLWRNORM);
        poll.push_back(WSAPOLLFD{
            entry.first,
            events,
            0,
        });
    }

    const int ready = WSAPoll(
        poll.data(),
        static_cast<ULONG>(poll.size()),
        pollTimeoutMs);
    if (ready == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error != WSAEINTR) {
            s.log(LogLevel::Error, "WSAPoll failed: %d", error);
            s.fatal = true;
            return false;
        }
        return true;
    }

    if (!s.draining) {
        if (poll[0].revents & POLLRDNORM) s.acceptConnections(now);
        if (poll[1].revents & POLLRDNORM) s.receiveUdp(now);
    }

    const size_t total = poll.size();
    if (total > 2u) {
        const size_t count = total - 2u;
        for (size_t step = 0; step < count; ++step) {
            const size_t index = 2u + ((step + s.turnCursor) % count);
            const SOCKET socket = poll[index].fd;
            const short events = poll[index].revents;
            if (events == 0) continue;
            if (!s.draining && (events & POLLRDNORM)) s.receiveTcp(socket);
            if (s.findConnection(socket) != nullptr
                && (events & POLLWRNORM)) {
                s.flushTcp(socket);
            }
            if (s.findConnection(socket) != nullptr
                && (events & (POLLERR | POLLHUP | POLLNVAL))) {
                s.closeConnection(socket, "TCP poll failure");
            }
        }
        ++s.turnCursor;
    }

    const uint64_t cleanupIntervalMs =
        (std::min<uint64_t>)(
            (std::min<uint64_t>)(kCleanupIntervalMs,
                s.config.spectatorRegistrationTimeoutMs),
            s.config.ipRateStateRetentionMs);
    if (now - s.lastCleanupMs >= cleanupIntervalMs) {
        s.lastCleanupMs = now;
        s.cleanupExpired(now);
    }
    if (s.config.metricsIntervalMs != 0
        && now - s.lastMetricsMs >= s.config.metricsIntervalMs) {
        s.lastMetricsMs = now;
        s.emitHealth(false);
    }

    ++s.metrics.turns;
    const uint64_t turnEnd = microsecondsNow();
    const uint64_t micros = turnEnd > turnBegin ? turnEnd - turnBegin : 0u;
    const uint64_t serviceMicros = micros > static_cast<uint64_t>(
                                       pollTimeoutMs > 0 ? pollTimeoutMs : 0)
                                       * 1000ull
        ? micros - static_cast<uint64_t>(pollTimeoutMs > 0 ? pollTimeoutMs : 0)
            * 1000ull
        : micros;
    const uint64_t recorded = ready == 0 ? 0u : serviceMicros;
    s.recordLatency(recorded);
    if (recorded > s.metrics.maxTurnMicros)
        s.metrics.maxTurnMicros = recorded;
    return true;
}

void RelayService::drain() {
    State& s = *state_;
    if (!s.started || s.draining) return;
    s.draining = true;

    for (auto& entry : s.rooms) {
        Room& room = entry.second;
        SpectatorLane& lane = room.spectator;
        if (!lane.sourceRegistered) continue;
        for (auto& viewerEntry : lane.viewers) {
            ViewerState& viewer = viewerEntry.second;
            if (!viewer.udpRegistered) continue;
            s.sendSpectatorDetachTo(
                viewer.udpEndpoint, room.id, SpectatorParty::Viewer,
                viewer.token, viewer.viewerId, lane.generation,
                SpectatorDetachReason::ServiceStopping);
        }
        if (lane.sourceUdpRegistered) {
            s.sendSpectatorDetachTo(
                lane.sourceUdpEndpoint, room.id, SpectatorParty::Source,
                lane.sourceToken, 0u, lane.generation,
                SpectatorDetachReason::ServiceStopping);
        }
    }
    for (auto& entry : s.rooms) {
        Room& room = entry.second;
        TopologyLane& lane = room.topology;
        if (!lane.authorityRegistered) continue;
        for (auto& nodeEntry : lane.nodes) {
            NodeState& node = nodeEntry.second;
            if (!node.udpRegistered) continue;
            s.sendTopologyDetachTo(
                node.udpEndpoint, room.id, TopologyParty::Node,
                node.token, node.nodeId, lane.generation,
                TopologyDetachReason::ServiceStopping);
        }
        if (lane.authorityUdpRegistered) {
            s.sendTopologyDetachTo(
                lane.authorityUdpEndpoint, room.id, TopologyParty::Authority,
                lane.authorityToken, 0u, lane.generation,
                TopologyDetachReason::ServiceStopping);
        }
    }

    const uint64_t deadline = s.monotonicMs() + s.config.drainTimeoutMs;
    s.log(LogLevel::Info,
          "shutdown requested; draining %zu rooms and %zu connections "
          "within %ums",
          s.rooms.size(),
          s.connections.size(),
          s.config.drainTimeoutMs);

    for (;;) {
        bool pending = false;
        for (const auto& entry : s.connections) {
            if (entry.second.sendOffset < entry.second.sendBytes.size()) {
                pending = true;
                break;
            }
        }
        if (!pending) break;
        if (s.monotonicMs() >= deadline) {
            s.log(LogLevel::Warn,
                  "drain deadline reached with queued control bytes; "
                  "closing anyway");
            break;
        }
        if (!pumpOnce(10)) break;
    }

    s.emitHealth(true);
    s.shutdownSockets();
}

ExitCode RelayService::run() {
    State& s = *state_;
    if (!s.started) return ExitCode::FatalRuntime;
    while (!stopRequested()) {
        if (!pumpOnce(static_cast<int>(s.config.pollIntervalMs))) break;
    }
    const bool fatal = s.fatal;
    drain();
    return fatal ? ExitCode::FatalRuntime : ExitCode::Ok;
}

RelayServiceMetrics RelayService::snapshot() const {
    State& s = *state_;
    RelayServiceMetrics current = s.metrics;
    current.uptimeMs = s.started ? s.monotonicMs() - s.startedMs : 0u;
    current.rooms = static_cast<uint32_t>(s.rooms.size());
    current.connections = static_cast<uint32_t>(s.connections.size());
    current.viewers = s.totalViewers;
    current.spectatorRooms = s.spectatorRooms;
    current.topologyRooms = s.topologyRooms;
    current.topologyNodes = s.totalTopologyNodes;
    current.trackedIpStates = static_cast<uint32_t>(s.ips.size());
    current.p95TurnMicros = s.percentileLatency(95);
    uint32_t paired = 0;
    uint32_t pending = 0;
    for (const auto& entry : s.rooms) {
        if (entry.second.paired) ++paired;
    }
    for (const auto& entry : s.connections) {
        if (!entry.second.joined) ++pending;
    }
    current.pairedRooms = paired;
    current.pendingConnections = pending;
    return current;
}

std::string RelayService::healthLine() const {
    const RelayServiceMetrics current = snapshot();
    char text[640] = {};
    std::snprintf(
        text,
        sizeof(text),
        "rooms=%u/%u spectator_rooms=%u viewers=%u topology_rooms=%u "
        "nodes=%u conn=%u ips=%u/%u "
        "player_in=%llu player_out=%llu spec_in=%llu spec_out=%llu "
        "topo_in=%llu topo_out=%llu "
        "drops=%llu rejects=%llu grant_rejects=%llu p95=%lluus",
        current.pairedRooms,
        current.rooms,
        current.spectatorRooms,
        current.viewers,
        current.topologyRooms,
        current.topologyNodes,
        current.connections,
        current.trackedIpStates,
        state_->config.maximumTrackedIpStates,
        static_cast<unsigned long long>(current.playerBytesIn),
        static_cast<unsigned long long>(current.playerBytesOut),
        static_cast<unsigned long long>(current.spectatorBytesIn),
        static_cast<unsigned long long>(current.spectatorBytesOut),
        static_cast<unsigned long long>(current.topologyBytesIn),
        static_cast<unsigned long long>(current.topologyBytesOut),
        static_cast<unsigned long long>(current.drops),
        static_cast<unsigned long long>(current.rejects),
        static_cast<unsigned long long>(current.grantRejects),
        static_cast<unsigned long long>(current.p95TurnMicros));
    return text;
}

}
