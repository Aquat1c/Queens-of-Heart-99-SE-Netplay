#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "client/spectator/ResilientSpectatorRuntime.h"
#include "netplay/WireProtocol.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace model = qnetplay::spectator::resilient;
namespace runtime = qclient::spectator::resilient;
namespace wire = qnetplay::spectator::resilient::wire;

using qclient::spectator::RouteEndpoint;

constexpr model::NodeId kRootNodeId = 0x5153522D524F4F54ull;
constexpr model::NodeId kDirectChildNodeId = 0x5153522D44495245ull;
constexpr model::NodeId kPunchedChildNodeId = 0x5153522D50554E43ull;
constexpr uint64_t kSetOrSessionId = 0x5153522D53455353ull;
constexpr uint64_t kGameEpoch = 0x5153522D45504F43ull;
constexpr uint64_t kStreamRootIdentity = 0x5153522D5354524Dull;
constexpr uint64_t kGameId = 0x5153522D47414D45ull;
constexpr uint64_t kInitialDigest = 0x5153522D44494753ull;
constexpr uint64_t kChildViewerId = 0x5153522D56495753ull;
constexpr DWORD kSocketTimeoutMs = 5000u;
constexpr DWORD kChildTimeoutMs = 7000u;

uint32_t loopbackIpv4BE() {
    return htonl(INADDR_LOOPBACK);
}

uint64_t nowMs() {
    return static_cast<uint64_t>(GetTickCount64());
}

void fail(const char* const phase, const std::string& detail) {
    std::fprintf(stderr, "[qsr-local-process] %s: %s\n", phase,
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

class LoopbackUdpSocket {
public:
    LoopbackUdpSocket() = default;
    ~LoopbackUdpSocket() { close(); }

    LoopbackUdpSocket(const LoopbackUdpSocket&) = delete;
    LoopbackUdpSocket& operator=(const LoopbackUdpSocket&) = delete;

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

        const DWORD timeout = kSocketTimeoutMs;
        if (::setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&timeout),
                         sizeof(timeout)) == SOCKET_ERROR) {
            error = winsockDetail("setsockopt(SO_RCVTIMEO)");
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

    bool send(const model::RouteDescriptor& route, const uint8_t* bytes,
              const size_t length, std::string& error) const {
        if (_socket == INVALID_SOCKET || !route.valid() || bytes == nullptr
            || length == 0u || length > static_cast<size_t>(INT_MAX)) {
            error = "invalid loopback UDP send parameters";
            return false;
        }
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = route.ipv4BE;
        destination.sin_port = htons(route.port);
        const int sent = ::sendto(
            _socket, reinterpret_cast<const char*>(bytes),
            static_cast<int>(length), 0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        if (sent != static_cast<int>(length)) {
            error = winsockDetail("sendto");
            return false;
        }
        return true;
    }

    bool receive(std::vector<uint8_t>& bytes, RouteEndpoint& sender,
                 std::string& error) const {
        bytes.clear();
        sender = {};
        if (_socket == INVALID_SOCKET) {
            error = "receive requested on a closed loopback UDP socket";
            return false;
        }
        std::array<uint8_t, wire::kMaxResilientControlDatagramBytes> buffer{};
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = ::recvfrom(
            _socket, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received <= 0) {
            error = winsockDetail("recvfrom");
            return false;
        }
        sender.ipv4BE = from.sin_addr.s_addr;
        sender.port = ntohs(from.sin_port);
        bytes.assign(buffer.begin(), buffer.begin() + received);
        return true;
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

class SocketControlTransport final : public runtime::ResilientControlTransport {
public:
    explicit SocketControlTransport(LoopbackUdpSocket& socket)
        : _socket(socket) {}

    bool sendControl(const model::RouteDescriptor& to, const uint8_t* bytes,
                     const size_t length) override {
        _lastError.clear();
        return _socket.send(to, bytes, length, _lastError);
    }

    const std::string& lastError() const { return _lastError; }

private:
    LoopbackUdpSocket& _socket;
    std::string _lastError;
};

qnetplay::compat::CompatibilityIdentityWire makeCompatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    (void)qnetplay::compat::setProfileId(identity, "qoh99-se1126");
    for (size_t index = 0u;
         index < qnetplay::compat::kCompatibilitySha256Bytes; ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(0x11u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(0x51u + index);
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
    identity.stateSchemaId = 0x5153522D53544154ull;
    identity.modSimulationId = 0x5153522D4D4F4453ull;
    identity.timingProfileId = 0x5153522D54494D45ull;
    return identity;
}

bool makeStream(model::StreamCompatibilityIdentity& stream) {
    return model::makeStreamCompatibilityIdentity(
        makeCompatibilityIdentity(), kSetOrSessionId, kGameEpoch,
        kStreamRootIdentity, stream);
}

qnetplay::spectator::SpectatorGrantKey makeLeaseKey() {
    qnetplay::spectator::SpectatorGrantKey key{};
    for (size_t index = 0u; index < key.size(); ++index)
        key[index] = static_cast<uint8_t>(0xA0u + index);
    return key;
}

model::NodeId childNodeId(const model::RouteKind routeKind) {
    return routeKind == model::RouteKind::Punched ? kPunchedChildNodeId
                                                   : kDirectChildNodeId;
}

const char* routeKindName(const model::RouteKind routeKind) {
    return routeKind == model::RouteKind::Punched ? "punched" : "direct";
}

bool parseRouteKind(const char* const value, model::RouteKind& routeKind) {
    if (value == nullptr) return false;
    if (std::strcmp(value, "direct") == 0) {
        routeKind = model::RouteKind::Direct;
        return true;
    }
    if (std::strcmp(value, "punched") == 0) {
        routeKind = model::RouteKind::Punched;
        return true;
    }
    return false;
}

model::NodeAdvertisement makeAdvertisement(
    const model::NodeId nodeId, const model::NodeKind kind,
    const model::StreamCompatibilityIdentity& stream,
    const model::RouteKind routeKind, const uint16_t port) {
    model::NodeAdvertisement advertisement{};
    advertisement.nodeId = nodeId;
    advertisement.kind = kind;
    advertisement.stream = stream;
    advertisement.route.kind = routeKind;
    advertisement.route.ipv4BE = loopbackIpv4BE();
    advertisement.route.port = port;
    advertisement.route.generation = 1u;
    advertisement.route.routeProved = true;
    advertisement.capabilityModelRevision =
        model::kResilientCapabilityModelRevision;
    advertisement.capabilities = model::kCapabilityEphemeralEndpoint
        | model::kCapabilityMultiPeerAutopunch
        | model::kCapabilityMultiViewerRoot;
    advertisement.childCapacity = kind == model::NodeKind::SpectatorRelay
        ? 8u : 32u;
    advertisement.childCount = 0u;
    advertisement.uploadCapacity = 64u;
    advertisement.uploadInUse = 0u;
    advertisement.rttMs = 1u;
    advertisement.lossPerMille = 0u;
    advertisement.cacheFirstCursor = 0u;
    advertisement.cacheNextCursor = 128u;
    advertisement.sourceGeneration = 1u;
    advertisement.routeGeneration = 1u;
    advertisement.lastProgressMs = 1u;
    advertisement.leaseExpiryMs = 0u;
    advertisement.reachable = true;
    advertisement.draining = false;
    return advertisement;
}

bool sameEndpoint(const RouteEndpoint& endpoint,
                  const model::RouteDescriptor& route) {
    return endpoint.valid() && route.valid()
        && endpoint.ipv4BE == route.ipv4BE && endpoint.port == route.port;
}

class LocalLoopbackAuthorizer final
    : public runtime::ResilientControlAuthorizer {
public:
    LocalLoopbackAuthorizer(const model::RouteKind expectedRouteKind,
                            const model::NodeId expectedChildId,
                            const model::StreamCompatibilityIdentity& stream)
        : _expectedRouteKind(expectedRouteKind),
          _expectedChildId(expectedChildId),
          _stream(stream) {}

    bool authorize(const RouteEndpoint& from,
                   const wire::ControlDatagram& datagram,
                   const uint64_t observedNowMs) const override {
        if (observedNowMs == 0u || !from.valid()
            || from.ipv4BE != loopbackIpv4BE()) {
            return false;
        }
        if (datagram.type == wire::ControlMessageType::NodeAdvertisement) {
            const model::NodeAdvertisement& advertisement =
                datagram.nodeAdvertisement;
            const bool accepted = advertisement.nodeId == _expectedChildId
                && advertisement.kind == model::NodeKind::SpectatorRelay
                && advertisement.stream == _stream
                && advertisement.route.kind == _expectedRouteKind
                && advertisement.route.routeProved
                && sameEndpoint(from, advertisement.route);
            if (accepted) _observedChildRoute = advertisement.route;
            return accepted;
        }
        if (datagram.type == wire::ControlMessageType::ParentLease) {
            return datagram.parentLease.stream == _stream
                && datagram.parentLease.childId == _expectedChildId
                && datagram.parentLease.parentId == kRootNodeId;
        }
        return false;
    }

    bool parentLeaseChildBinding(
        const model::NodeId childId,
        model::ParentLeaseChildBinding& out) const override {
        out = {};
        if (childId != _expectedChildId || !_observedChildRoute.valid())
            return false;
        out.authorityGeneration = 1u;
        out.childViewerId = kChildViewerId;
        out.childViewerGeneration = 1u;
        out.childQspRoute = _observedChildRoute;
        out.childEndpointBindingGeneration = 1u;
        out.childRequestedCursor.nextRecord = 0u;
        out.childRequestedCursor.nextEvent = 0u;
        out.childRequestedCursor.digest = kInitialDigest;
        return out.valid();
    }

private:
    model::RouteKind _expectedRouteKind;
    model::NodeId _expectedChildId;
    model::StreamCompatibilityIdentity _stream{};
    mutable model::RouteDescriptor _observedChildRoute{};
};

class ChildProcess {
public:
    ChildProcess() = default;
    ~ChildProcess() { cleanup(); }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool launch(const uint16_t rootPort, const model::RouteKind routeKind,
                std::string& error) {
        cleanup();
        const std::wstring modulePath = selfPath();
        if (modulePath.empty()) {
            error = "GetModuleFileNameW did not return the test executable";
            return false;
        }
        std::wstring commandLine = L"\"" + modulePath + L"\" --qsr-child "
            + std::to_wstring(rootPort) + L" "
            + (routeKind == model::RouteKind::Punched ? L"punched" : L"direct");
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

int runChild(const char* const rootPortText, const char* const routeText) {
    uint16_t rootPort = 0u;
    model::RouteKind routeKind{};
    if (!parsePort(rootPortText, rootPort) || !parseRouteKind(routeText, routeKind))
        return 2;

    WinsockScope winsock;
    if (!winsock.start()) return 3;
    LoopbackUdpSocket childSocket;
    uint16_t childPort = 0u;
    std::string error;
    if (!childSocket.open(childPort, error)) return 4;

    model::StreamCompatibilityIdentity stream{};
    if (!makeStream(stream) || !stream.valid()) return 5;
    const model::NodeId nodeId = childNodeId(routeKind);
    const model::NodeAdvertisement advertisement = makeAdvertisement(
        nodeId, model::NodeKind::SpectatorRelay, stream, routeKind, childPort);
    wire::ControlDatagram advertisementDatagram{};
    advertisementDatagram.type = wire::ControlMessageType::NodeAdvertisement;
    advertisementDatagram.nodeAdvertisement = advertisement;
    std::vector<uint8_t> encodedAdvertisement;
    if (!wire::encodeControlDatagram(advertisementDatagram, encodedAdvertisement))
        return 6;

    model::RouteDescriptor rootRoute{};
    rootRoute.kind = model::RouteKind::Direct;
    rootRoute.ipv4BE = loopbackIpv4BE();
    rootRoute.port = rootPort;
    rootRoute.generation = 1u;
    rootRoute.routeProved = true;
    if (!childSocket.send(rootRoute, encodedAdvertisement.data(),
                          encodedAdvertisement.size(), error)) {
        return 7;
    }

    std::vector<uint8_t> returnedLease;
    RouteEndpoint leaseSender{};
    if (!childSocket.receive(returnedLease, leaseSender, error)) return 8;
    if (leaseSender.ipv4BE != loopbackIpv4BE() || leaseSender.port != rootPort)
        return 9;
    wire::ControlDatagram leaseDatagram{};
    if (wire::decodeControlDatagram(returnedLease.data(), returnedLease.size(),
                                    leaseDatagram)
            != wire::DecodeResult::Ok
        || leaseDatagram.type != wire::ControlMessageType::ParentLease
        || leaseDatagram.parentLease.stream != stream
        || leaseDatagram.parentLease.childId != nodeId
        || leaseDatagram.parentLease.parentId != kRootNodeId) {
        return 10;
    }

    std::vector<uint8_t> redemption;
    if (!wire::encodeControlDatagram(leaseDatagram, redemption)) return 11;
    if (!childSocket.send(rootRoute, redemption.data(), redemption.size(), error))
        return 12;
    return 0;
}

bool runRound(const model::RouteKind routeKind) {
    const char* const roundName = routeKindName(routeKind);
    LoopbackUdpSocket rootSocket;
    uint16_t rootPort = 0u;
    std::string error;
    if (!rootSocket.open(rootPort, error)) {
        fail(roundName, error);
        return false;
    }

    model::StreamCompatibilityIdentity stream{};
    if (!makeStream(stream) || !stream.valid()) {
        fail(roundName, "could not construct the CompatibilityIdentityWire-backed stream");
        return false;
    }
    const model::NodeId childId = childNodeId(routeKind);
    auto authorizer = std::make_shared<LocalLoopbackAuthorizer>(
        routeKind, childId, stream);
    SocketControlTransport transport(rootSocket);
    runtime::ResilientRootRuntimeConfig config{};
    config.stream = stream;
    config.localAdvertisement = makeAdvertisement(
        kRootNodeId, model::NodeKind::HostRoot, stream,
        model::RouteKind::Direct, rootPort);
    config.gameId = kGameId;
    config.initialDigest = kInitialDigest;
    config.parentLeaseKey = makeLeaseKey();
    config.authorizer = authorizer;

    runtime::ResilientSpectatorRootRuntime root;
    if (!root.start(config, transport)) {
        fail(roundName, "ResilientSpectatorRootRuntime::start rejected the local root");
        return false;
    }

    ChildProcess child;
    if (!child.launch(rootPort, routeKind, error)) {
        fail(roundName, error);
        return false;
    }

    std::vector<uint8_t> advertisementBytes;
    RouteEndpoint advertisementSender{};
    if (!rootSocket.receive(advertisementBytes, advertisementSender, error)) {
        fail(roundName, error);
        return false;
    }
    wire::ControlDatagram advertisement{};
    if (wire::decodeControlDatagram(advertisementBytes.data(),
                                    advertisementBytes.size(), advertisement)
            != wire::DecodeResult::Ok
        || advertisement.type != wire::ControlMessageType::NodeAdvertisement
        || advertisement.nodeAdvertisement.nodeId != childId
        || advertisement.nodeAdvertisement.route.kind != routeKind
        || !sameEndpoint(advertisementSender,
                         advertisement.nodeAdvertisement.route)) {
        fail(roundName, "child did not deliver the expected QSR NodeAdvertisement");
        return false;
    }
    if (root.onDatagram(advertisementSender, advertisementBytes.data(),
                        advertisementBytes.size(), nowMs())
        != runtime::ControlIngressResult::AcceptedAdvertisement) {
        fail(roundName, "root runtime did not accept the child advertisement");
        return false;
    }

    const uint64_t offerNowMs = nowMs();
    if (!root.issueAndSendParentOffer(childId, 0u, offerNowMs + 3000u,
                                      0xABCDEF0000000000ull
                                          | static_cast<uint64_t>(routeKind))) {
        const std::string detail = transport.lastError().empty()
            ? "root runtime did not issue/send ParentLease"
            : "root runtime could not send ParentLease: " + transport.lastError();
        fail(roundName, detail);
        return false;
    }

    std::vector<uint8_t> redemptionBytes;
    RouteEndpoint redemptionSender{};
    if (!rootSocket.receive(redemptionBytes, redemptionSender, error)) {
        fail(roundName, error);
        return false;
    }
    wire::ControlDatagram redemption{};
    if (wire::decodeControlDatagram(redemptionBytes.data(), redemptionBytes.size(),
                                    redemption)
            != wire::DecodeResult::Ok
        || redemption.type != wire::ControlMessageType::ParentLease
        || redemption.parentLease.childId != childId
        || redemption.parentLease.parentId != kRootNodeId
        || !sameEndpoint(redemptionSender,
                         advertisement.nodeAdvertisement.route)) {
        fail(roundName, "child did not return the expected QSR ParentLease");
        return false;
    }
    if (root.onDatagram(redemptionSender, redemptionBytes.data(),
                        redemptionBytes.size(), nowMs())
        != runtime::ControlIngressResult::AcceptedLeaseRedemption) {
        fail(roundName, "root runtime did not redeem the returned ParentLease");
        return false;
    }

    const model::ResilientTopologyCoordinator* const topology = root.topology();
    const model::TopologyNodeView* const attached =
        topology == nullptr ? nullptr : topology->find(childId);
    if (attached == nullptr || attached->activeParent != kRootNodeId
        || attached->depth != 1u || topology->rootChildCount(kRootNodeId) != 1u) {
        fail(roundName, "lease redemption did not attach the child to the root");
        return false;
    }

    DWORD childExitCode = 0u;
    if (!child.waitForExit(kChildTimeoutMs, childExitCode, error)) {
        fail(roundName, error);
        return false;
    }
    if (childExitCode != 0u) {
        fail(roundName, "child process returned " + std::to_string(childExitCode));
        return false;
    }
    return true;
}

}

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--qsr-child") == 0)
        return runChild(argv[2], argv[3]);
    if (argc != 1) {
        std::fprintf(stderr,
                     "usage: %s [--qsr-child <root-port> <direct|punched>]\n",
                     argc > 0 ? argv[0] : "qoh99_resilient_local_process");
        return 2;
    }

    WinsockScope winsock;
    if (!winsock.start()) {
        fail("startup", "WSAStartup failed");
        return 1;
    }
    const bool directPassed = runRound(model::RouteKind::Direct);
    const bool punchedPassed = runRound(model::RouteKind::Punched);
    if (!directPassed || !punchedPassed) return 1;
    std::puts("resilient spectator local process UDP root-control seam: PASS");
    return 0;
}
