#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "NodeRuntimeTestSeams.h"

#include "../../../netplay/WireProtocol.h"
#include "../../../spectator/SpectatorArchive.h"
#include "../../../spectator/SpectatorSession.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace runtime = qclient::spectator::resilient;
namespace model = qnetplay::spectator::resilient;
namespace compat = qnetplay::compat;
using namespace qnetplay::spectator;

constexpr uint64_t kGameId = 0x5153504C4F43414Cull;
constexpr uint64_t kSimulationContract = 0x514F48393953494Dull;
constexpr uint32_t kFirstFrame = 7600u;
constexpr model::NodeId kRootNode = 0x5153502D524F4F54ull;
constexpr model::NodeId kRelayNode = 0x5153502D52454C59ull;
constexpr model::NodeId kChildNode = 0x5153502D4348494Cull;
constexpr EndpointId kParentSourceEndpoint = 0x51535001u;
constexpr EndpointId kRelayChildEndpoint = 0x51535002u;
constexpr uint64_t kAuthorityGeneration = 0x5153502D41555448ull;
constexpr ViewerId kChildViewerId = 0x9A8B7C6D5E4F3021ull;
constexpr uint32_t kChildViewerGeneration = 1u;
constexpr uint64_t kChildEndpointBindingGeneration =
    0x5153502D4348494Cull;
constexpr uint32_t kExpectedRecords = 4u;
constexpr uint32_t kExpectedEvents = 1u;
constexpr DWORD kProcessTimeoutMs = 18000u;
constexpr uint64_t kPumpTimeoutMs = 14000u;

constexpr uint32_t kControlMagic = 0x54505351u;
constexpr size_t kControlBytes = 5u + (4u * sizeof(uint32_t));

enum class ControlType : uint8_t {
    Ready = 1u,
    ChildReady = 2u,
    ChildGo = 3u,
    ChildDone = 4u,
    ChildDetached = 5u,
    Status = 6u,
    Finish = 7u,
};

struct UdpEndpoint {
    uint32_t ipv4BE = 0u;
    uint16_t port = 0u;

    bool valid() const { return ipv4BE != 0u && port != 0u; }
};

bool operator==(const UdpEndpoint& left, const UdpEndpoint& right) {
    return left.ipv4BE == right.ipv4BE && left.port == right.port;
}

uint32_t loopbackIpv4BE() {
    return htonl(INADDR_LOOPBACK);
}

uint64_t nowMs() {
    return static_cast<uint64_t>(GetTickCount64());
}

bool elapsed(const uint64_t begin, const uint64_t limitMs) {
    return nowMs() - begin >= limitMs;
}

void writeLe32(uint8_t* const out, const uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
}

uint32_t readLe32(const uint8_t* const bytes) {
    return static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8u)
        | (static_cast<uint32_t>(bytes[2]) << 16u)
        | (static_cast<uint32_t>(bytes[3]) << 24u);
}

struct ControlMessage {
    ControlType type = ControlType::Ready;
    uint32_t first = 0u;
    uint32_t second = 0u;
    uint32_t third = 0u;
    uint32_t fourth = 0u;
};

std::vector<uint8_t> encodeControl(const ControlMessage& message) {
    std::vector<uint8_t> out(kControlBytes, 0u);
    writeLe32(out.data(), kControlMagic);
    out[4] = static_cast<uint8_t>(message.type);
    writeLe32(out.data() + 5u, message.first);
    writeLe32(out.data() + 9u, message.second);
    writeLe32(out.data() + 13u, message.third);
    writeLe32(out.data() + 17u, message.fourth);
    return out;
}

bool decodeControl(const uint8_t* const bytes, const size_t length,
                   ControlMessage& out) {
    if (bytes == nullptr || length != kControlBytes
        || readLe32(bytes) != kControlMagic) {
        return false;
    }
    const uint8_t rawType = bytes[4];
    if (rawType < static_cast<uint8_t>(ControlType::Ready)
        || rawType > static_cast<uint8_t>(ControlType::Finish)) {
        return false;
    }
    out.type = static_cast<ControlType>(rawType);
    out.first = readLe32(bytes + 5u);
    out.second = readLe32(bytes + 9u);
    out.third = readLe32(bytes + 13u);
    out.fourth = readLe32(bytes + 17u);
    return true;
}

void fail(const char* const phase, const std::string& detail) {
    std::fprintf(stderr, "[qsp-local-process] %s: %s\n", phase,
                 detail.c_str());
}

std::string winsockDetail(const char* const operation) {
    return std::string(operation) + " failed (WSA="
        + std::to_string(WSAGetLastError()) + ")";
}

class WinsockScope {
public:
    bool start() {
        WSADATA data{};
        _started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        return _started;
    }

    ~WinsockScope() {
        if (_started) WSACleanup();
    }

private:
    bool _started = false;
};

enum class ReceiveResult { Received, Timeout, Error };

class LoopbackUdpSocket {
public:
    ~LoopbackUdpSocket() { close(); }

    LoopbackUdpSocket(const LoopbackUdpSocket&) = delete;
    LoopbackUdpSocket& operator=(const LoopbackUdpSocket&) = delete;
    LoopbackUdpSocket() = default;

    bool open(uint16_t& boundPort, std::string& error) {
        close();
        boundPort = 0u;
        _socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_socket == INVALID_SOCKET) {
            error = winsockDetail("socket");
            return false;
        }
        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_addr.s_addr = loopbackIpv4BE();
        bindAddress.sin_port = htons(0u);
        if (::bind(_socket, reinterpret_cast<const sockaddr*>(&bindAddress),
                   sizeof(bindAddress)) == SOCKET_ERROR) {
            error = winsockDetail("bind(loopback)");
            close();
            return false;
        }
        sockaddr_in actual{};
        int actualLength = sizeof(actual);
        if (::getsockname(_socket, reinterpret_cast<sockaddr*>(&actual),
                          &actualLength) == SOCKET_ERROR) {
            error = winsockDetail("getsockname");
            close();
            return false;
        }
        boundPort = ntohs(actual.sin_port);
        if (boundPort == 0u) {
            error = "getsockname returned a zero loopback UDP port";
            close();
            return false;
        }
        return true;
    }

    bool send(const UdpEndpoint& destination, const uint8_t* const bytes,
              const size_t length, std::string& error) const {
        if (_socket == INVALID_SOCKET || !destination.valid()
            || bytes == nullptr || length == 0u
            || length > static_cast<size_t>(INT_MAX)) {
            error = "invalid loopback UDP send parameters";
            return false;
        }
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = destination.ipv4BE;
        to.sin_port = htons(destination.port);
        const int sent = ::sendto(
            _socket, reinterpret_cast<const char*>(bytes),
            static_cast<int>(length), 0,
            reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        if (sent != static_cast<int>(length)) {
            error = winsockDetail("sendto");
            return false;
        }
        return true;
    }

    bool send(const UdpEndpoint& destination, const std::vector<uint8_t>& bytes,
              std::string& error) const {
        return send(destination, bytes.data(), bytes.size(), error);
    }

    ReceiveResult receive(std::vector<uint8_t>& bytes, UdpEndpoint& sender,
                          const DWORD timeoutMs, std::string& error) const {
        bytes.clear();
        sender = {};
        if (_socket == INVALID_SOCKET) {
            error = "receive requested on a closed loopback UDP socket";
            return ReceiveResult::Error;
        }
        fd_set readSet{};
        FD_ZERO(&readSet);
        FD_SET(_socket, &readSet);
        timeval wait{};
        wait.tv_sec = static_cast<long>(timeoutMs / 1000u);
        wait.tv_usec = static_cast<long>((timeoutMs % 1000u) * 1000u);
        const int selected = ::select(0, &readSet, nullptr, nullptr, &wait);
        if (selected == 0) return ReceiveResult::Timeout;
        if (selected == SOCKET_ERROR) {
            error = winsockDetail("select");
            return ReceiveResult::Error;
        }
        std::array<uint8_t, kMaxSpectatorDatagramSize> buffer{};
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = ::recvfrom(
            _socket, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received <= 0) {
            error = winsockDetail("recvfrom");
            return ReceiveResult::Error;
        }
        sender.ipv4BE = from.sin_addr.s_addr;
        sender.port = ntohs(from.sin_port);
        bytes.assign(buffer.begin(), buffer.begin() + received);
        return ReceiveResult::Received;
    }

    void close() {
        if (_socket != INVALID_SOCKET) {
            closesocket(_socket);
            _socket = INVALID_SOCKET;
        }
    }

private:
    SOCKET _socket = INVALID_SOCKET;
};

bool parsePort(const char* const value, uint16_t& out) {
    out = 0u;
    if (value == nullptr || *value == '\0') return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0u || parsed > 65535u)
        return false;
    out = static_cast<uint16_t>(parsed);
    return true;
}

class HiddenProcess {
public:
    ~HiddenProcess() { cleanup(); }

    HiddenProcess(const HiddenProcess&) = delete;
    HiddenProcess& operator=(const HiddenProcess&) = delete;
    HiddenProcess() = default;

    bool launch(const std::wstring& arguments, std::string& error) {
        cleanup();
        const std::wstring modulePath = selfPath();
        if (modulePath.empty()) {
            error = "GetModuleFileNameW did not return the test executable";
            return false;
        }
        std::wstring commandLine = L"\"" + modulePath + L"\" " + arguments;
        std::vector<wchar_t> mutableCommand(commandLine.begin(),
                                              commandLine.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(modulePath.c_str(), mutableCommand.data(), nullptr,
                            nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &process)) {
            error = "CreateProcessW failed (Win32="
                + std::to_string(GetLastError()) + ")";
            return false;
        }
        _process = process.hProcess;
        _thread = process.hThread;
        return true;
    }

    bool waitForExit(const DWORD timeoutMs, DWORD& exitCode,
                     std::string& error) {
        if (_process == nullptr) {
            error = "wait requested without a child process";
            return false;
        }
        const DWORD waited = WaitForSingleObject(_process, timeoutMs);
        if (waited != WAIT_OBJECT_0) {
            error = waited == WAIT_TIMEOUT
                ? "child process timed out"
                : "WaitForSingleObject(child) failed (Win32="
                    + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!GetExitCodeProcess(_process, &exitCode)) {
            error = "GetExitCodeProcess failed (Win32="
                + std::to_string(GetLastError()) + ")";
            return false;
        }
        closeHandles();
        return true;
    }

private:
    static std::wstring selfPath() {
        std::vector<wchar_t> buffer(MAX_PATH);
        for (;;) {
            const DWORD written = GetModuleFileNameW(
                nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (written == 0u) return {};
            if (written + 1u < buffer.size())
                return std::wstring(buffer.data(), written);
            if (buffer.size() >= 32768u) return {};
            buffer.resize(buffer.size() * 2u);
        }
    }

    void closeHandles() {
        if (_thread != nullptr) {
            CloseHandle(_thread);
            _thread = nullptr;
        }
        if (_process != nullptr) {
            CloseHandle(_process);
            _process = nullptr;
        }
    }

    void cleanup() {
        if (_process != nullptr
            && WaitForSingleObject(_process, 0u) == WAIT_TIMEOUT) {
            (void)TerminateProcess(_process, 1u);
            (void)WaitForSingleObject(_process, 1000u);
        }
        closeHandles();
    }

    HANDLE _process = nullptr;
    HANDLE _thread = nullptr;
};

SessionIdentity qspIdentity() {
    return SessionIdentity{
        0x1122334455667788ull,
        0x0000000000000009ull,
        SessionCookie{
            0x1234567890ABCDEFull,
            0x0FEDCBA098765432ull,
        },
    };
}

compat::CompatibilityIdentityWire compatibilityIdentity() {
    compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion = compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    if (!compat::setProfileId(identity, "qoh99-se1126")) return {};
    for (size_t index = 0u; index < compat::kCompatibilitySha256Bytes;
         ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(0x31u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(0x61u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<uint8_t>(0x91u + index);
    }
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.stateSchemaId = 0x5153504C4F43414Cull;
    identity.modSimulationId = 0x5153504D4F44554Cull;
    identity.timingProfileId = 0x51535054494D4552ull;
    return identity;
}

GameDescriptor gameDescriptor() {
    GameDescriptor descriptor{};
    descriptor.gameId = kGameId;
    descriptor.simulationContract = kSimulationContract;
    descriptor.compatibilityIdentity = compatibilityIdentity();
    descriptor.initialStateHash = 0xD15EA5E000000001ull;
    descriptor.firstTimelineFrame = kFirstFrame;
    descriptor.playerOneDelay = 2u;
    descriptor.playerTwoDelay = 3u;
    descriptor.rollbackWindow = 8u;
    descriptor.metadata = {0x51u, 0x53u, 0x50u, 0x2Du, 0x4Cu, 0x50u};
    descriptor.initialDigest = expectedInitialDigest(descriptor);
    return descriptor;
}

ConfirmedRecord recordFor(const uint32_t record) {
    ConfirmedRecord out{};
    out.record = record;
    out.timelineFrame = kFirstFrame + record;
    out.playerOneInput = static_cast<uint16_t>(record & 0x000Fu);
    out.playerTwoInput = static_cast<uint16_t>((record + 5u) & 0x000Fu);
    out.stateHash = 0xD15EA5E000000000ull
        ^ (static_cast<uint64_t>(record) * 0x101u);
    return out;
}

bool stageEventAt(const SpectatorArchive& archive,
                  qnetplay::spectator::SetEvent& event) {
    event = {};
    event.eventIndex = 0u;
    event.recordCursor = 2u;
    event.timelineFrame = kFirstFrame + event.recordCursor;
    event.kind = SetEventKind::StageSelected;
    event.valueC = 4u;
    return archive.chainDigestAt(kGameId, event.recordCursor, 0u,
                                 event.recordDigest);
}

SpectatorGrantKey leaseKey() {
    SpectatorGrantKey key{};
    for (size_t index = 0u; index < key.size(); ++index)
        key[index] = static_cast<uint8_t>(0xA0u + index);
    return key;
}

model::ResilientLimits relayLimits() {
    model::ResilientLimits limits{};
    limits.routeRaceRecordBudget = 1u;
    limits.continuityOverlapRecords = 1u;
    limits.retransmissionMarginRecords = 1u;
    limits.bootstrapMarginRecords = 1u;
    limits.maxCachedRecords = limits.requiredCacheRecords();
    limits.maxCachedEvents = kMaxSetEventsPerBatch;
    limits.maxCachedBytes = 64u * 1024u;
    return limits;
}

model::NodeAdvertisement advertisement(
    const model::NodeId nodeId, const model::NodeKind kind,
    const model::StreamCompatibilityIdentity& stream, const uint16_t port,
    const uint64_t sourceGeneration, const uint64_t routeGeneration) {
    model::NodeAdvertisement node{};
    node.nodeId = nodeId;
    node.kind = kind;
    node.stream = stream;
    node.route.kind = model::RouteKind::Direct;
    node.route.ipv4BE = loopbackIpv4BE();
    node.route.port = port;
    node.route.generation = routeGeneration;
    node.route.routeProved = true;
    node.capabilityModelRevision = model::kResilientCapabilityModelRevision;
    node.capabilities = model::kCapabilityEphemeralEndpoint
        | model::kCapabilityMultiPeerAutopunch
        | model::kCapabilityMultiViewerRoot
        | model::kCapabilityRelayCapableSpectator;
    node.childCapacity = kind == model::NodeKind::SpectatorRelay ? 8u : 32u;
    node.uploadCapacity = 32u;
    node.rttMs = 1u;
    node.cacheFirstCursor = 0u;
    node.cacheNextCursor = 0u;
    node.sourceGeneration = sourceGeneration;
    node.routeGeneration = routeGeneration;
    node.lastProgressMs = 1u;
    node.reachable = true;
    return node;
}

bool makeStream(const GameDescriptor& descriptor,
                model::StreamCompatibilityIdentity& stream) {
    return model::makeStreamCompatibilityIdentity(
        descriptor.compatibilityIdentity, qspIdentity().sessionId,
        qspIdentity().epoch, gameStartFingerprint(canonicalGameStart(descriptor)),
        stream);
}

runtime::ResilientSpectatorNodeRuntimeConfig relayConfig(
    const GameDescriptor& descriptor, const uint16_t parentPort,
    const uint16_t relayPort, const uint16_t controlPort) {
    runtime::ResilientSpectatorNodeRuntimeConfig config{};
    if (!makeStream(descriptor, config.stream)) return {};
    config.qspIdentity = qspIdentity();
    config.simulationContract = descriptor.simulationContract;
    config.compatibilityIdentity = descriptor.compatibilityIdentity;
    config.gameId = descriptor.gameId;
    config.initialDigest = descriptor.initialDigest;
    config.sourceGeneration = 5u;
    config.authorityGeneration = kAuthorityGeneration;
    config.initialParentGeneration = 5u;
    config.initialRouteGeneration = 9u;
    config.limits = relayLimits();
    config.ingressMode = runtime::NodeRuntimeIngressMode::CanonicalHostAttested;
    config.productionRelay = true;
    config.localAdvertisement = advertisement(
        kRelayNode, model::NodeKind::SpectatorRelay, config.stream, relayPort,
        config.sourceGeneration, config.initialRouteGeneration);
    config.localAdvertisement.controlRoute = config.localAdvertisement.route;
    config.localAdvertisement.controlRoute.port = controlPort;
    config.initialParentAdvertisement = advertisement(
        kRootNode, model::NodeKind::HostRoot, config.stream, parentPort,
        config.sourceGeneration, config.initialRouteGeneration);
    config.downstreamArchive.maxGames = 1u;
    config.downstreamArchive.maxRetainedRecords = 32u;
    config.downstreamArchive.segmentRecords = 4u;
    config.downstreamArchive.maxRetainedBytes = 64u * 1024u;
    config.downstreamSource.recordsPerBatch = 1u;
    config.downstreamSource.perViewerPacketBudget = 2u;
    config.downstreamSource.globalPacketBudget = 4u;
    config.upstreamViewer.secureRandom = 0xA1B2C3D4E5F60718ull;
    return config;
}

bool signChildLease(const runtime::ResilientSpectatorNodeRuntimeConfig& config,
                    const model::NodeAdvertisement& child,
                    const model::VerifiedRelayCache& cache,
                    const uint64_t issuedAtMs, model::ParentLease& lease) {
    lease = {};
    lease.stream = config.stream;
    lease.childId = child.nodeId;
    lease.parentId = config.localAdvertisement.nodeId;
    lease.childBinding.authorityGeneration = config.authorityGeneration;
    lease.childBinding.childViewerId = kChildViewerId;
    lease.childBinding.childViewerGeneration = kChildViewerGeneration;
    lease.childBinding.childQspRoute = child.route;
    lease.childBinding.childEndpointBindingGeneration =
        kChildEndpointBindingGeneration;
    lease.childBinding.childRequestedCursor.nextRecord = 0u;
    lease.childBinding.childRequestedCursor.nextEvent = 0u;
    if (!cache.digestAt(0u, 0u,
                        lease.childBinding.childRequestedCursor.digest)) {
        return false;
    }
    lease.parentDepth = 1u;
    lease.parentSourceGeneration = cache.sourceGeneration();
    lease.routeGeneration = config.localAdvertisement.routeGeneration;
    lease.routeCount = 1u;
    lease.routes[0] = config.localAdvertisement.route;
    lease.cacheRange = cache.range();
    lease.latestVerifiedCursor = cache.range().nextCursor;
    lease.childCapacity =
        static_cast<uint32_t>(config.limits.maxChildrenPerRelay);
    lease.expiryMs = issuedAtMs + 5000u;
    lease.replayNonce = 0x5153504C45415345ull ^ issuedAtMs;
    lease.ancestry[0] = kRootNode;
    lease.ancestry[1] = config.localAdvertisement.nodeId;
    lease.ancestryCount = 2u;
    model::ParentLeaseRegistry signer(leaseKey());
    return signer.issue(lease);
}

ViewerConfig canonicalUpstreamConfig(
    const runtime::ResilientSpectatorNodeRuntimeConfig& config) {
    ViewerConfig upstream{};
    upstream.identity = config.qspIdentity;
    upstream.simulationContract = config.simulationContract;
    upstream.compatibilityIdentity = config.compatibilityIdentity;
    upstream.secureRandom = 0xA1B2C3D4E5F60718ull;
    upstream.relayIngestOnly = true;
    upstream.maxRelayRetainedRecords = static_cast<uint32_t>(
        (std::max)(config.limits.maxCachedRecords,
                   qnetplay::spectator::kMaxRecordsPerBatch));
    upstream.maxRelayRetainedEvents = static_cast<uint32_t>(
        (std::max)(config.limits.maxCachedEvents,
                   qnetplay::spectator::kMaxSetEventsPerBatch));
    return upstream;
}

SourceConfig sourceConfig(const GameDescriptor& descriptor) {
    SourceConfig config{};
    config.identity = qspIdentity();
    config.simulationContract = descriptor.simulationContract;
    config.compatibilityIdentity = descriptor.compatibilityIdentity;
    config.recordsPerBatch = 1u;
    config.perViewerPacketBudget = 2u;
    config.globalPacketBudget = 4u;
    return config;
}

ViewerConfig childConfig(const GameDescriptor& descriptor) {
    ViewerConfig config{};
    config.identity = qspIdentity();
    config.simulationContract = descriptor.simulationContract;
    config.compatibilityIdentity = descriptor.compatibilityIdentity;
    config.secureRandom = kChildViewerId;
    return config;
}

bool sourceIngressAccepted(const uint8_t* const bytes, const size_t length,
                           const IngressResult result) {
    if (result == IngressResult::Accepted
        || result == IngressResult::ReplayedSequence) {
        return true;
    }
    Datagram datagram{};
    return result == IngressResult::UnexpectedMessage
        && decodeDatagram(bytes, length, qspIdentity(), datagram)
            == DecodeError::None
        && datagram.header.type == MessageType::InitAck;
}

bool viewerIngressAccepted(const IngressResult result) {
    return result == IngressResult::Accepted
        || result == IngressResult::ReplayedSequence;
}

int runRelay(const char* const parentPortText) {
    uint16_t parentPort = 0u;
    if (!parsePort(parentPortText, parentPort)) return 2;
    WinsockScope winsock;
    if (!winsock.start()) return 3;

    LoopbackUdpSocket socket;
    uint16_t relayPort = 0u;
    std::string error;
    if (!socket.open(relayPort, error)) {
        fail("relay", error);
        return 4;
    }
    LoopbackUdpSocket controlSocket;
    uint16_t controlPort = 0u;
    if (!controlSocket.open(controlPort, error)) {
        fail("relay", error);
        return 5;
    }
    const GameDescriptor descriptor = gameDescriptor();
    const runtime::ResilientSpectatorNodeRuntimeConfig config =
        relayConfig(descriptor, parentPort, relayPort, controlPort);
    if (!config.stream.valid() || !config.limits.valid()) return 6;
    runtime::ResilientSpectatorNodeRuntime relay;
    if (!relay.start(config)) {
        fail("relay", "ResilientSpectatorNodeRuntime::start rejected fixture");
        return 7;
    }
    SpectatorViewer upstream(canonicalUpstreamConfig(config));
    if (!upstream.isConfigured() || !upstream.begin(kGameId)) {
        fail("relay", "canonical upstream viewer was not configured/begun");
        return 8;
    }
    const std::shared_ptr<runtime::QtaAuthorityContext> authority =
        qclient::spectator::resilient::test::QtaAuthorityContextTestFactory::
            make(config.authorityGeneration, nowMs() + kPumpTimeoutMs + 1000u);
    if (authority == nullptr) {
        fail("relay", "could not create canonical authority context");
        return 9;
    }

    const UdpEndpoint parent{loopbackIpv4BE(), parentPort};
    if (!socket.send(parent, encodeControl({ControlType::Ready}), error)) {
        fail("relay", error);
        return 10;
    }

    UdpEndpoint childEndpoint{};
    std::optional<runtime::NodeRuntimeChildIngress> childIngress;
    bool childReady = false;
    bool childRegistered = false;
    bool childDone = false;
    bool childAckProved = false;
    bool childDetachReported = false;
    uint32_t acknowledgedRecords = 0u;
    uint32_t acknowledgedEvents = 0u;
    const uint64_t started = nowMs();
    while (!elapsed(started, kPumpTimeoutMs)) {
        std::vector<uint8_t> bytes;
        UdpEndpoint sender{};
        const ReceiveResult received = socket.receive(bytes, sender, 2u, error);
        if (received == ReceiveResult::Error) {
            fail("relay", error);
            return 11;
        }
        if (received == ReceiveResult::Received) {
            ControlMessage control{};
            if (decodeControl(bytes.data(), bytes.size(), control)) {
                if (sender.ipv4BE != loopbackIpv4BE()) return 12;
                if (control.type == ControlType::ChildReady && !childReady) {
                    childEndpoint = sender;
                    childReady = true;
                } else if (control.type == ControlType::ChildDone
                           && childRegistered && sender == childEndpoint) {
                    childDone = true;
                }
            } else if (sender == parent) {
                const uint64_t receivedAt = nowMs();
                if (!viewerIngressAccepted(upstream.onDatagram(
                        bytes.data(), bytes.size(), receivedAt))) {
                    fail("relay", "canonical upstream viewer rejected QSP packet");
                    return 13;
                }
                const runtime::QtaVerifiedCanonicalIngress ingress =
                    qclient::spectator::resilient::test::
                        QtaVerifiedCanonicalIngressTestFactory::make(
                            authority, config, upstream);
                if (relay.projectCanonicalVerifiedPrefix(ingress, upstream,
                                                         receivedAt)
                    != runtime::NodeRuntimeResult::Accepted) {
                    fail("relay", "canonical upstream prefix was not projected");
                    return 14;
                }
            } else if (childRegistered && childIngress.has_value()
                       && sender == childEndpoint) {
                if (relay.onChildDatagram(*childIngress,
                                          bytes.data(), bytes.size(), nowMs())
                    != runtime::NodeRuntimeResult::Accepted) {
                    fail("relay", "child QSP packet was not accepted");
                    return 15;
                }
            }
        }

        for (const ViewerDatagram& packet : upstream.pollOutbound(nowMs(), 4u)) {
            if (!socket.send(parent, packet.bytes, error)) {
                fail("relay", error);
                return 16;
            }
        }

        const model::ResilientRelayNode* relayNode = relay.relayNode();
        if (childReady && !childRegistered && relayNode != nullptr
            && relayNode->cache().range().nextCursor >= kExpectedRecords
            && relayNode->cache().eventRange().nextEvent >= kExpectedEvents) {
            model::NodeAdvertisement child = advertisement(
                kChildNode, model::NodeKind::SpectatorRelay, config.stream,
                childEndpoint.port, config.sourceGeneration,
                config.initialRouteGeneration);
            child.activeParent = kRelayNode;
            child.depth = 2u;
            child.ancestry[0] = kRootNode;
            child.ancestry[1] = kRelayNode;
            child.ancestryCount = 2u;
            const uint64_t leaseNowMs = nowMs();
            model::ParentLease lease{};
            if (!signChildLease(config, child, relayNode->cache(), leaseNowMs,
                                lease)
                || relay.registerQtaVerifiedChild(
                       qclient::spectator::resilient::test::
                           QtaVerifiedParentLeaseTestFactory::make(
                               lease, authority),
                       leaseNowMs)
                    != runtime::NodeRuntimeResult::Accepted
                || !socket.send(childEndpoint,
                                encodeControl({ControlType::ChildGo}), error)) {
                fail("relay", "could not register/activate authenticated child");
                return 17;
            }
            childIngress = qclient::spectator::resilient::test::
                NodeRuntimeChildIngressTestFactory::make(
                    child.route, kChildEndpointBindingGeneration,
                    kRelayChildEndpoint);
            childRegistered = true;
        }

        if (childRegistered) {
            for (const runtime::NodeRuntimeDownstreamDatagram& packet
                 : relay.pollChildOutbound(nowMs(), 4u)) {
                if (packet.destination != kRelayChildEndpoint
                    || !socket.send(childEndpoint, packet.bytes, error)) {
                    fail("relay", "could not send downstream QSP packet");
                    return 18;
                }
            }
        }

        relayNode = relay.relayNode();
        const model::CacheChildStatus* childStatus = relayNode == nullptr
            ? nullptr : relayNode->cache().child(kChildNode);
        if (childStatus != nullptr
            && childStatus->acknowledgedCursor >= kExpectedRecords
            && childStatus->acknowledgedEvent >= kExpectedEvents) {
            childAckProved = true;
            acknowledgedRecords = childStatus->acknowledgedCursor;
            acknowledgedEvents = childStatus->acknowledgedEvent;
        }
        if (childDone && childAckProved && childStatus == nullptr
            && !childDetachReported) {
            if (!relay.running()
                || !socket.send(parent,
                                encodeControl({ControlType::ChildDetached}),
                                error)) {
                fail("relay", "child detach stopped relay or status send failed");
                return 19;
            }
            childDetachReported = true;
        }
        if (childDetachReported && relayNode != nullptr
            && relayNode->cache().range().nextCursor >= kExpectedRecords + 1u
            && relayNode->cache().eventRange().nextEvent >= kExpectedEvents) {
            const ControlMessage status{
                ControlType::Status,
                relayNode->cache().range().nextCursor,
                relayNode->cache().eventRange().nextEvent,
                acknowledgedRecords,
                acknowledgedEvents,
            };
            if (!socket.send(parent, encodeControl(status), error)
                || !socket.send(childEndpoint,
                                encodeControl({ControlType::Finish}), error)) {
                fail("relay", "could not send success status/finish");
                return 20;
            }
            return 0;
        }
    }
    fail("relay", "timed out waiting for canonical projection, child Detach, or post-detach upstream record");
    return 21;
}

int runChild(const char* const relayPortText) {
    uint16_t relayPort = 0u;
    if (!parsePort(relayPortText, relayPort)) return 20;
    WinsockScope winsock;
    if (!winsock.start()) return 21;
    LoopbackUdpSocket socket;
    uint16_t childPort = 0u;
    std::string error;
    if (!socket.open(childPort, error)) {
        fail("child", error);
        return 22;
    }
    const GameDescriptor descriptor = gameDescriptor();
    SpectatorViewer viewer(childConfig(descriptor));
    if (!viewer.isConfigured() || !viewer.begin(kGameId)) {
        fail("child", "SpectatorViewer fixture was not configured/begun");
        return 23;
    }
    const UdpEndpoint relay{loopbackIpv4BE(), relayPort};
    if (!socket.send(relay, encodeControl({ControlType::ChildReady}), error)) {
        fail("child", error);
        return 24;
    }

    bool activated = false;
    bool detachArmed = false;
    bool detachSent = false;
    bool doneSent = false;
    uint64_t completedAt = 0u;
    const uint64_t started = nowMs();
    while (!elapsed(started, kPumpTimeoutMs)) {
        std::vector<uint8_t> bytes;
        UdpEndpoint sender{};
        const ReceiveResult received = socket.receive(bytes, sender, 2u, error);
        if (received == ReceiveResult::Error) {
            fail("child", error);
            return 25;
        }
        if (received == ReceiveResult::Received) {
            if (!(sender == relay)) return 26;
            ControlMessage control{};
            if (decodeControl(bytes.data(), bytes.size(), control)) {
                if (control.type == ControlType::ChildGo) {
                    activated = true;
                } else if (control.type == ControlType::Finish) {
                    if (viewer.receivedNextRecord() < kExpectedRecords
                        || viewer.receivedNextEvent() < kExpectedEvents
                        || !doneSent) {
                        fail("child", "finished before QSP record/event receipt");
                        return 27;
                    }
                    return 0;
                }
            } else if (activated
                       && !viewerIngressAccepted(viewer.onDatagram(
                           bytes.data(), bytes.size(), nowMs()))) {
                fail("child", "downstream QSP packet was rejected");
                return 28;
            }
        }

        if (activated) {
            for (const ViewerDatagram& packet : viewer.pollOutbound(nowMs(), 4u)) {
                if (!socket.send(relay, packet.bytes, error)) {
                    fail("child", error);
                    return 29;
                }
                if (packet.type == MessageType::Detach) detachSent = true;
            }
            if (viewer.receivedNextRecord() >= kExpectedRecords
                && viewer.receivedNextEvent() >= kExpectedEvents) {
                if (completedAt == 0u) completedAt = nowMs();
                if (!detachArmed && nowMs() - completedAt >= 20u) {
                    if (!viewer.detach()) {
                        fail("child", "could not arm authenticated QSP Detach");
                        return 30;
                    }
                    detachArmed = true;
                }
                if (detachSent && !doneSent) {
                    if (!socket.send(relay,
                                     encodeControl({ControlType::ChildDone}),
                                     error)) {
                        fail("child", error);
                        return 31;
                    }
                    doneSent = true;
                }
            }
        }
    }
    fail("child", "timed out waiting for downstream QSP completion/detach");
    return 32;
}

bool prepareSource(SpectatorArchive& archive, SpectatorSource& source,
                   const GameDescriptor& descriptor) {
    if (!archive.isConfigured() || !source.isConfigured()
        || archive.beginGame(descriptor) != ArchiveResult::Ok) {
        return false;
    }
    if (archive.appendConfirmed(kGameId, recordFor(0u), nullptr)
            != ArchiveResult::Ok
        || archive.appendConfirmed(kGameId, recordFor(1u), nullptr)
            != ArchiveResult::Ok) {
        return false;
    }
    qnetplay::spectator::SetEvent event{};
    if (!stageEventAt(archive, event)
        || archive.appendEvent(kGameId, event, nullptr) != ArchiveResult::Ok
        || archive.appendConfirmed(kGameId, recordFor(2u), nullptr)
            != ArchiveResult::Ok
        || archive.appendConfirmed(kGameId, recordFor(3u), nullptr)
            != ArchiveResult::Ok) {
        return false;
    }
    return true;
}

bool waitForRelayReady(LoopbackUdpSocket& socket, UdpEndpoint& relay,
                       std::string& error) {
    const uint64_t started = nowMs();
    while (!elapsed(started, 4000u)) {
        std::vector<uint8_t> bytes;
        UdpEndpoint sender{};
        const ReceiveResult received = socket.receive(bytes, sender, 10u, error);
        if (received == ReceiveResult::Error) return false;
        if (received != ReceiveResult::Received) continue;
        ControlMessage control{};
        if (sender.ipv4BE == loopbackIpv4BE()
            && decodeControl(bytes.data(), bytes.size(), control)
            && control.type == ControlType::Ready) {
            relay = sender;
            return true;
        }
    }
    error = "timed out waiting for relay Ready";
    return false;
}

bool runParent() {
    WinsockScope winsock;
    if (!winsock.start()) {
        fail("parent", "WSAStartup failed");
        return false;
    }
    LoopbackUdpSocket sourceSocket;
    uint16_t sourcePort = 0u;
    std::string error;
    if (!sourceSocket.open(sourcePort, error)) {
        fail("parent", error);
        return false;
    }
    const GameDescriptor descriptor = gameDescriptor();
    if (!compat::isValidCompatibilityIdentity(descriptor.compatibilityIdentity)) {
        fail("parent", "invalid CompatibilityIdentityWire fixture");
        return false;
    }
    SpectatorArchive sourceArchive{};
    SpectatorSource source(sourceConfig(descriptor), sourceArchive);
    if (!prepareSource(sourceArchive, source, descriptor)) {
        fail("parent", "could not prepare upstream QSP source/archive");
        return false;
    }

    HiddenProcess relayProcess;
    if (!relayProcess.launch(L"--qsp-relay " + std::to_wstring(sourcePort),
                             error)) {
        fail("parent", error);
        return false;
    }
    UdpEndpoint relay{};
    if (!waitForRelayReady(sourceSocket, relay, error)) {
        fail("parent", error);
        return false;
    }
    HiddenProcess viewerProcess;
    if (!viewerProcess.launch(L"--qsp-child " + std::to_wstring(relay.port),
                              error)) {
        fail("parent", error);
        return false;
    }

    bool statusReceived = false;
    bool childDetachReceived = false;
    bool postDetachRecordAppended = false;
    ControlMessage status{};
    const uint64_t started = nowMs();
    while (!elapsed(started, kPumpTimeoutMs)) {
        std::vector<uint8_t> bytes;
        UdpEndpoint sender{};
        const ReceiveResult received = sourceSocket.receive(bytes, sender, 2u,
                                                            error);
        if (received == ReceiveResult::Error) {
            fail("parent", error);
            return false;
        }
        if (received == ReceiveResult::Received) {
            if (!(sender == relay)) {
                fail("parent", "received a packet from an unexpected endpoint");
                return false;
            }
            ControlMessage control{};
            if (decodeControl(bytes.data(), bytes.size(), control)) {
                if (control.type == ControlType::ChildDetached) {
                    if (postDetachRecordAppended
                        || sourceArchive.appendConfirmed(
                               kGameId, recordFor(kExpectedRecords), nullptr)
                               != ArchiveResult::Ok) {
                        fail("parent", "could not append post-detach source record");
                        return false;
                    }
                    childDetachReceived = true;
                    postDetachRecordAppended = true;
                } else if (control.type == ControlType::Status) {
                    status = control;
                    statusReceived = true;
                    break;
                }
            } else {
                const IngressResult ingress = source.onDatagram(
                    kParentSourceEndpoint, bytes.data(), bytes.size(), nowMs());
                if (!sourceIngressAccepted(bytes.data(), bytes.size(), ingress)) {
                fail("parent", "source rejected relay QSP ingress");
                return false;
                }
            }
        }
        for (const SourceDatagram& packet : source.pollOutbound(nowMs(), 4u)) {
            if (!sourceSocket.send(relay, packet.bytes, error)) {
                fail("parent", error);
                return false;
            }
        }
    }
    if (!statusReceived || !childDetachReceived || !postDetachRecordAppended) {
        fail("parent", "timed out waiting for child detach or post-detach relay status");
        return false;
    }
    if (status.first < kExpectedRecords + 1u
        || status.second < kExpectedEvents
        || status.third < kExpectedRecords || status.fourth < kExpectedEvents) {
        fail("parent", "relay status did not prove post-detach upstream projection and child Ack");
        return false;
    }

    DWORD relayExit = 0u;
    DWORD viewerExit = 0u;
    if (!relayProcess.waitForExit(kProcessTimeoutMs, relayExit, error)
        || relayExit != 0u) {
        fail("parent", error.empty()
             ? "relay subprocess failed with " + std::to_string(relayExit)
             : error);
        return false;
    }
    if (!viewerProcess.waitForExit(kProcessTimeoutMs, viewerExit, error)
        || viewerExit != 0u) {
        fail("parent", error.empty()
             ? "viewer subprocess failed with " + std::to_string(viewerExit)
             : error);
        return false;
    }
    return true;
}

}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--qsp-relay") == 0)
        return runRelay(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "--qsp-child") == 0)
        return runChild(argv[2]);
    if (argc != 1) {
        std::fprintf(stderr,
                     "usage: %s [--qsp-relay <parent-port>|--qsp-child <relay-port>]\n",
                     argc > 0 ? argv[0] : "qoh99_resilient_qsp_local_process");
        return 2;
    }
    if (!runParent()) return 1;
    std::puts("resilient spectator QSP local process relay seam: PASS");
    return 0;
}
