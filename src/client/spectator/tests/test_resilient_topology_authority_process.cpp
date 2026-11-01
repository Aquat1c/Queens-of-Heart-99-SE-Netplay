#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "../SpectatorSidecar.h"

#include "../../../compat/CompatibilityIdentity.h"
#include "../../../netplay/WireProtocol.h"
#include "../../../spectator/ResilientSpectatorProtocol.h"
#include "../../../spectator/SpectatorGrant.h"
#include "../../../spectator/SpectatorSession.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace qsp = qnetplay::spectator;
namespace model = qnetplay::spectator::resilient;
namespace wire = qnetplay::spectator::resilient::wire;
namespace topology = qclient::spectator::topology;
using qclient::spectator::AdmissionPolicy;
using qclient::spectator::PublicationEvent;
using qclient::spectator::PublicationKind;
using qclient::spectator::RouteEndpoint;
using qclient::spectator::SidecarConfig;
using qclient::spectator::SidecarPhase;
using qclient::spectator::SpectatorSidecar;

constexpr uint64_t kSessionId = 0x544F504F4C4F4759ull;
constexpr uint64_t kEpoch = 0x0000000000000042ull;
constexpr uint64_t kSimulationContract = 0x515348544F504F31ull;
constexpr uint64_t kGameId = 0x515348544F504F32ull;
constexpr uint64_t kViewerId = 0x544F504F56494557ull;
constexpr uint32_t kViewerGeneration = 1u;
constexpr uint64_t kSourceGeneration = 1u;
constexpr uint64_t kInitialNowMs = 1000u;
constexpr uint64_t kPumpTimeoutMs = 18000u;
constexpr DWORD kChildTimeoutMs = 20000u;

constexpr uint32_t kControlMagic = 0x504F5451u;
constexpr uint16_t kControlVersion = 1u;
constexpr size_t kControlHeaderBytes = 8u;
constexpr size_t kTopologyEnvelopePrefixBytes = 52u;
constexpr char kTopologyCredentialDomain[] = "qoh99-topology-qsr-v1";

enum class ControlType : uint8_t {
    ChildReady = 1u,
    InitAckSent = 2u,
    Enrollment = 3u,
    LeafOnly = 4u,
    OfferReceived = 5u,
    RedemptionSent = 6u,
    QspAckAfterTopology = 7u,
    BeginDetach = 8u,
    Detached = 9u,
    Finish = 10u,
};

struct ControlMessage {
    ControlType type = ControlType::ChildReady;
    std::vector<uint8_t> payload{};
};

struct VerifiedCursor {
    uint32_t nextRecord = 0u;
    uint32_t nextEvent = 0u;
    uint64_t digest = 0u;
};

struct ChildEnrollment {
    topology::TopologyEnrollment enrollment{};
    VerifiedCursor transcript{};
    VerifiedCursor verified{};
};

void fail(const char* const phase, const std::string& detail) {
    std::fprintf(stderr, "[topology-authority-process] %s: %s\n", phase,
                 detail.c_str());
}

uint64_t nowMs() {
    return static_cast<uint64_t>(GetTickCount64());
}

bool elapsed(const uint64_t begin, const uint64_t limitMs) {
    return nowMs() - begin >= limitMs;
}

uint32_t loopbackIpv4BE() {
    return htonl(INADDR_LOOPBACK);
}

void appendU8(std::vector<uint8_t>& out, const uint8_t value) {
    out.push_back(value);
}

void appendU16(std::vector<uint8_t>& out, const uint16_t value) {
    appendU8(out, static_cast<uint8_t>(value));
    appendU8(out, static_cast<uint8_t>(value >> 8u));
}

void appendU32(std::vector<uint8_t>& out, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        appendU8(out, static_cast<uint8_t>(value >> shift));
}

void appendU64(std::vector<uint8_t>& out, const uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        appendU8(out, static_cast<uint8_t>(value >> shift));
}

uint16_t readLe16(const uint8_t* const in) {
    return static_cast<uint16_t>(in[0])
        | (static_cast<uint16_t>(in[1]) << 8u);
}

uint32_t readLe32(const uint8_t* const in) {
    uint32_t value = 0u;
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        value |= static_cast<uint32_t>(in[shift / 8u]) << shift;
    return value;
}

uint64_t readLe64(const uint8_t* const in) {
    uint64_t value = 0u;
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        value |= static_cast<uint64_t>(in[shift / 8u]) << shift;
    return value;
}

void writeLe16(uint8_t* const out, const uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8u);
}

void writeLe32(uint8_t* const out, const uint32_t value) {
    for (unsigned shift = 0u; shift < 32u; shift += 8u)
        out[shift / 8u] = static_cast<uint8_t>(value >> shift);
}

void writeLe64(uint8_t* const out, const uint64_t value) {
    for (unsigned shift = 0u; shift < 64u; shift += 8u)
        out[shift / 8u] = static_cast<uint8_t>(value >> shift);
}

class Reader {
public:
    Reader(const uint8_t* const bytes, const size_t length)
        : _bytes(bytes), _length(length) {}

    bool u8(uint8_t& out) {
        if (_offset >= _length) return false;
        out = _bytes[_offset++];
        return true;
    }

    bool u16(uint16_t& out) {
        if (_offset + 2u > _length) return false;
        out = readLe16(_bytes + _offset);
        _offset += 2u;
        return true;
    }

    bool u32(uint32_t& out) {
        if (_offset + 4u > _length) return false;
        out = readLe32(_bytes + _offset);
        _offset += 4u;
        return true;
    }

    bool u64(uint64_t& out) {
        if (_offset + 8u > _length) return false;
        out = readLe64(_bytes + _offset);
        _offset += 8u;
        return true;
    }

    bool bytes(uint8_t* const out, const size_t count) {
        if ((out == nullptr && count != 0u) || _offset + count > _length)
            return false;
        if (count != 0u) std::memcpy(out, _bytes + _offset, count);
        _offset += count;
        return true;
    }

    bool done() const { return _offset == _length; }

private:
    const uint8_t* _bytes = nullptr;
    size_t _length = 0u;
    size_t _offset = 0u;
};

bool encodeControl(const ControlType type, const std::vector<uint8_t>& payload,
                   std::vector<uint8_t>& out) {
    out.clear();
    if (payload.size() > 0xFFFFu) return false;
    try {
        out.assign(kControlHeaderBytes + payload.size(), 0u);
    } catch (...) {
        return false;
    }
    writeLe32(out.data(), kControlMagic);
    writeLe16(out.data() + 4u, kControlVersion);
    out[6] = static_cast<uint8_t>(type);
    out[7] = 0u;
    if (!payload.empty())
        std::memcpy(out.data() + kControlHeaderBytes, payload.data(),
                    payload.size());
    return true;
}

bool decodeControl(const uint8_t* const bytes, const size_t length,
                   ControlMessage& out) {
    out = {};
    if (bytes == nullptr || length < kControlHeaderBytes
        || readLe32(bytes) != kControlMagic
        || readLe16(bytes + 4u) != kControlVersion || bytes[7] != 0u) {
        return false;
    }
    const uint8_t rawType = bytes[6];
    if (rawType < static_cast<uint8_t>(ControlType::ChildReady)
        || rawType > static_cast<uint8_t>(ControlType::Finish)) {
        return false;
    }
    out.type = static_cast<ControlType>(rawType);
    try {
        out.payload.assign(bytes + kControlHeaderBytes, bytes + length);
    } catch (...) {
        return false;
    }
    return true;
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
    LoopbackUdpSocket() = default;
    LoopbackUdpSocket(const LoopbackUdpSocket&) = delete;
    LoopbackUdpSocket& operator=(const LoopbackUdpSocket&) = delete;
    ~LoopbackUdpSocket() { close(); }

    bool open(uint16_t& boundPort, std::string& error) {
        close();
        boundPort = 0u;
        _socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (_socket == INVALID_SOCKET) {
            error = winsockDetail("socket");
            return false;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = loopbackIpv4BE();
        address.sin_port = htons(0u);
        if (::bind(_socket, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) == SOCKET_ERROR) {
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

    bool send(const RouteEndpoint& destination, const uint8_t* const bytes,
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

    bool send(const RouteEndpoint& destination,
              const std::vector<uint8_t>& bytes, std::string& error) const {
        return send(destination, bytes.data(), bytes.size(), error);
    }

    ReceiveResult receive(std::vector<uint8_t>& bytes, RouteEndpoint& sender,
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
        std::array<uint8_t, 4096u> buffer{};
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
    HiddenProcess() = default;
    HiddenProcess(const HiddenProcess&) = delete;
    HiddenProcess& operator=(const HiddenProcess&) = delete;
    ~HiddenProcess() { cleanup(); }

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

    bool exited(DWORD& exitCode) const {
        exitCode = STILL_ACTIVE;
        return _process != nullptr && GetExitCodeProcess(_process, &exitCode)
            && exitCode != STILL_ACTIVE;
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

qsp::SessionIdentity sessionIdentity() {
    return qsp::SessionIdentity{
        kSessionId,
        kEpoch,
        qsp::SessionCookie{
            0xA1B2C3D4E5F60718ull,
            0x1029384756ABCDEFu,
        },
    };
}

qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = qnetplay::wire::kVersion;
    if (!qnetplay::compat::setProfileId(identity, "qoh99-se1126")) return {};
    for (size_t index = 0u;
         index < qnetplay::compat::kCompatibilitySha256Bytes; ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(0x21u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(0x51u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<uint8_t>(0x81u + index);
    }
    identity.runtimeBuild.executableSize = 704512u;
    identity.runtimeBuild.peTimestamp = 0x3A19A8A9u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.simulationContent.entryCount = 68u;
    identity.simulationContent.requiredEntryCount = 68u;
    identity.stateSchemaId = 0x544F504F53544154ull;
    identity.modSimulationId = 0x544F504F4D4F4453ull;
    identity.timingProfileId = 0x544F504F54494D45ull;
    return identity;
}

uint64_t stateHashFor(const uint32_t cursor) {
    return 0xA500000000000000ull
        ^ (static_cast<uint64_t>(cursor) * 0x1000003ull);
}

qsp::GameDescriptor gameDescriptor() {
    qsp::GameDescriptor descriptor{};
    descriptor.gameId = kGameId;
    descriptor.simulationContract = kSimulationContract;
    descriptor.compatibilityIdentity = compatibilityIdentity();
    descriptor.initialStateHash = stateHashFor(0u);
    descriptor.firstTimelineFrame = 4000u;
    descriptor.playerOneDelay = 2u;
    descriptor.playerTwoDelay = 2u;
    descriptor.rollbackWindow = 8u;
    descriptor.metadata = {0x54u, 0x4Fu, 0x50u, 0x4Fu};
    descriptor.initialDigest = qsp::expectedInitialDigest(descriptor);
    return descriptor;
}

qsp::ConfirmedRecord recordFor(const uint32_t index) {
    qsp::ConfirmedRecord record{};
    record.record = index;
    record.timelineFrame = gameDescriptor().firstTimelineFrame + index;
    record.playerOneInput = static_cast<uint16_t>(index & 0x000Fu);
    record.playerTwoInput = static_cast<uint16_t>((index + 3u) & 0x000Fu);
    record.stateHash = stateHashFor(index);
    return record;
}

SidecarConfig sidecarConfig() {
    SidecarConfig config{};
    config.identity = sessionIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = compatibilityIdentity();
    config.listenPort = 0u;
    config.policy = AdmissionPolicy::Public;
    config.maxViewers = 2u;
    config.retainAllRecords = true;
    config.maxRetainedBytes = 256u * 1024u;
    config.unixClock = [] { return uint64_t{1700000000u}; };
    return config;
}

topology::EndpointBinding rootQspBinding(const uint16_t port) {
    topology::EndpointBinding binding{};
    binding.local = RouteEndpoint{0u, port};
    binding.localProvenance = topology::EndpointProvenance::LocalBind;
    binding.advertised.kind = model::RouteKind::Direct;
    binding.advertised.ipv4BE = loopbackIpv4BE();
    binding.advertised.port = port;
    binding.advertised.generation = 1u;
    binding.advertised.routeProved = true;
    binding.advertisedProvenance =
        topology::EndpointProvenance::PublishedDirectCandidate;
    binding.generation = 1u;
    return binding;
}

topology::TopologyActivationRequest activationRequest(const uint16_t qspPort) {
    topology::TopologyActivationRequest request{};
    request.requested = true;
    request.rootQsp = rootQspBinding(qspPort);
    request.sourceGeneration = kSourceGeneration;
    return request;
}

bool appendBytes(std::vector<uint8_t>& out, const uint8_t* const bytes,
                 const size_t count) {
    if (bytes == nullptr && count != 0u) return false;
    try {
        out.insert(out.end(), bytes, bytes + count);
    } catch (...) {
        return false;
    }
    return true;
}

void appendRoute(std::vector<uint8_t>& out,
                 const model::RouteDescriptor& route) {
    appendU8(out, static_cast<uint8_t>(route.kind));
    appendU32(out, route.ipv4BE);
    appendU16(out, route.port);
    appendU64(out, route.generation);
    appendU8(out, route.routeProved ? 1u : 0u);
}

void appendBinding(std::vector<uint8_t>& out,
                   const topology::EndpointBinding& binding) {
    appendU32(out, binding.local.ipv4BE);
    appendU16(out, binding.local.port);
    appendRoute(out, binding.advertised);
    appendU8(out, static_cast<uint8_t>(binding.localProvenance));
    appendU8(out, static_cast<uint8_t>(binding.advertisedProvenance));
    appendU64(out, binding.generation);
}

bool appendCompatibility(
    std::vector<uint8_t>& out,
    const qnetplay::compat::CompatibilityIdentityWire& identity) {
    using namespace qnetplay::compat;
    appendU32(out, identity.schemaVersion);
    appendU32(out, identity.protocolVersion);
    return appendBytes(out, reinterpret_cast<const uint8_t*>(
                               identity.runtimeBuild.profileId),
                       kCompatibilityProfileIdBytes)
        && appendBytes(out, identity.runtimeBuild.executableSha256,
                       kCompatibilitySha256Bytes)
        && (appendU64(out, identity.runtimeBuild.executableSize), true)
        && (appendU32(out, identity.runtimeBuild.peTimestamp), true)
        && (appendU32(out, identity.simulationContent.manifestSchemaVersion),
            true)
        && (appendU32(out, identity.simulationContent.manifestFlags), true)
        && appendBytes(out, identity.simulationContent.simulationRootSha256,
                       kCompatibilitySha256Bytes)
        && appendBytes(out, identity.simulationContent.validationRootSha256,
                       kCompatibilitySha256Bytes)
        && (appendU32(out, identity.simulationContent.entryCount), true)
        && (appendU32(out, identity.simulationContent.requiredEntryCount),
            true)
        && (appendU64(out, identity.stateSchemaId), true)
        && (appendU64(out, identity.modSimulationId), true)
        && (appendU64(out, identity.timingProfileId), true)
        && (appendU32(out, identity.reserved), true);
}

void appendSerializedBinding(std::vector<uint8_t>& out,
                             const topology::EndpointBinding& binding) {
    appendBinding(out, binding);
}

bool readRoute(Reader& reader, model::RouteDescriptor& out) {
    uint8_t kind = 0u;
    uint8_t proved = 0u;
    if (!reader.u8(kind) || !reader.u32(out.ipv4BE) || !reader.u16(out.port)
        || !reader.u64(out.generation) || !reader.u8(proved)) {
        return false;
    }
    if (kind < static_cast<uint8_t>(model::RouteKind::Direct)
        || kind > static_cast<uint8_t>(model::RouteKind::TrafficRelay)
        || proved > 1u) {
        return false;
    }
    out.kind = static_cast<model::RouteKind>(kind);
    out.routeProved = proved != 0u;
    return true;
}

bool readBinding(Reader& reader, topology::EndpointBinding& out) {
    uint8_t localProvenance = 0u;
    uint8_t advertisedProvenance = 0u;
    if (!reader.u32(out.local.ipv4BE) || !reader.u16(out.local.port)
        || !readRoute(reader, out.advertised)
        || !reader.u8(localProvenance) || !reader.u8(advertisedProvenance)
        || !reader.u64(out.generation)) {
        return false;
    }
    if (localProvenance > static_cast<uint8_t>(
                              topology::EndpointProvenance::ProvenPunchedQsp)
        || advertisedProvenance > static_cast<uint8_t>(
                                   topology::EndpointProvenance::ProvenPunchedQsp)) {
        return false;
    }
    out.localProvenance =
        static_cast<topology::EndpointProvenance>(localProvenance);
    out.advertisedProvenance =
        static_cast<topology::EndpointProvenance>(advertisedProvenance);
    return out.validForAuthority();
}

bool encodeChildEnrollment(const ChildEnrollment& value,
                           std::vector<uint8_t>& out) {
    out.clear();
    const topology::TopologyEnrollment& enrollment = value.enrollment;
    if (!enrollment.granted) return false;
    appendU64(out, enrollment.nodeId);
    appendU64(out, enrollment.rootNodeId);
    appendU64(out, enrollment.authorityGeneration);
    appendU64(out, enrollment.expiresMs);
    appendU64(out, enrollment.bindingGeneration);
    appendSerializedBinding(out, enrollment.rootQsp);
    appendSerializedBinding(out, enrollment.rootQsr);
    if (!appendBytes(out, enrollment.assignmentSigningPublicKey.data(),
                     enrollment.assignmentSigningPublicKey.size())) {
        return false;
    }
    if (!appendBytes(out, enrollment.credential.data(),
                     enrollment.credential.size())) {
        return false;
    }
    appendU32(out, value.transcript.nextRecord);
    appendU32(out, value.transcript.nextEvent);
    appendU64(out, value.transcript.digest);
    appendU32(out, value.verified.nextRecord);
    appendU32(out, value.verified.nextEvent);
    appendU64(out, value.verified.digest);
    return true;
}

bool decodeChildEnrollment(const std::vector<uint8_t>& bytes,
                           ChildEnrollment& out) {
    out = {};
    Reader reader(bytes.data(), bytes.size());
    topology::TopologyEnrollment& enrollment = out.enrollment;
    if (!reader.u64(enrollment.nodeId) || !reader.u64(enrollment.rootNodeId)
        || !reader.u64(enrollment.authorityGeneration)
        || !reader.u64(enrollment.expiresMs)
        || !reader.u64(enrollment.bindingGeneration)
        || !readBinding(reader, enrollment.rootQsp)
        || !readBinding(reader, enrollment.rootQsr)
        || !reader.bytes(enrollment.assignmentSigningPublicKey.data(),
                         enrollment.assignmentSigningPublicKey.size())
        || !reader.bytes(enrollment.credential.data(),
                         enrollment.credential.size())
        || !reader.u32(out.transcript.nextRecord)
        || !reader.u32(out.transcript.nextEvent)
        || !reader.u64(out.transcript.digest)
        || !reader.u32(out.verified.nextRecord)
        || !reader.u32(out.verified.nextEvent)
        || !reader.u64(out.verified.digest) || !reader.done()) {
        return false;
    }
    enrollment.granted = enrollment.nodeId != model::kInvalidNodeId
        && enrollment.rootNodeId != model::kInvalidNodeId
        && enrollment.authorityGeneration != 0u && enrollment.expiresMs != 0u
        && enrollment.bindingGeneration != 0u;
    return enrollment.granted;
}

bool makeClientTopologyEnvelope(const ChildEnrollment& child,
                                const RouteEndpoint& qspEndpoint,
                                const uint32_t confirmedCursor,
                                const uint64_t sequence,
                                const uint8_t* const qsr,
                                const size_t qsrLength,
                                std::vector<uint8_t>& out) {
    out.clear();
    const topology::TopologyEnrollment& enrollment = child.enrollment;
    if (!enrollment.granted || !qspEndpoint.valid() || confirmedCursor != 0u
        || sequence == 0u || qsr == nullptr || qsrLength == 0u
        || qsrLength > wire::kMaxResilientControlDatagramBytes) {
        return false;
    }
    std::vector<uint8_t> macInput;
    const qsp::SessionIdentity identity = sessionIdentity();
    const qsp::GameDescriptor descriptor = gameDescriptor();
    const uint64_t streamRoot = qsp::gameStartFingerprint(
        qsp::canonicalGameStart(descriptor));
    try {
        macInput.reserve(512u + qsrLength);
    } catch (...) {
        return false;
    }
    if (!appendBytes(macInput,
                     reinterpret_cast<const uint8_t*>(
                         kTopologyCredentialDomain),
                     sizeof(kTopologyCredentialDomain) - 1u)) {
        return false;
    }
    appendU64(macInput, identity.sessionId);
    appendU64(macInput, identity.epoch);
    appendU64(macInput, identity.cookie.low);
    appendU64(macInput, identity.cookie.high);
    if (!appendCompatibility(macInput, compatibilityIdentity())) return false;
    appendU64(macInput, streamRoot);
    appendU64(macInput, enrollment.authorityGeneration);
    appendU64(macInput, enrollment.rootNodeId);
    appendBinding(macInput, enrollment.rootQsp);
    appendBinding(macInput, enrollment.rootQsr);
    appendU64(macInput, kSourceGeneration);
    appendU64(macInput, enrollment.nodeId);
    appendU64(macInput, kViewerId);
    appendU32(macInput, kViewerGeneration);
    appendU32(macInput, qspEndpoint.ipv4BE);
    appendU16(macInput, qspEndpoint.port);
    appendU8(macInput,
             static_cast<uint8_t>(topology::EndpointProvenance::ProvenDirectQsp));
    appendU64(macInput, kViewerGeneration);
    appendU64(macInput, enrollment.bindingGeneration);
    appendU32(macInput, confirmedCursor);
    appendU64(macInput, enrollment.expiresMs);
    appendU64(macInput, sequence);
    appendU32(macInput, child.transcript.nextRecord);
    appendU32(macInput, child.transcript.nextEvent);
    appendU64(macInput, child.transcript.digest);
    appendU8(macInput, 1u);
    const model::ResilientLimits limits{};
    appendU64(macInput, limits.maxTopologyNodes);
    appendU8(macInput, limits.maxActiveDepth);
    appendU64(macInput, limits.maxChildrenPerRoot);
    appendU64(macInput, limits.maxChildrenPerRelay);
    appendU64(macInput, limits.maxStandbyParents);
    appendU64(macInput, limits.standbyLeaseMs);
    if (!appendBytes(macInput, enrollment.assignmentSigningPublicKey.data(),
                     enrollment.assignmentSigningPublicKey.size())
        || !appendBytes(macInput, qsr, qsrLength)) {
        return false;
    }

    std::array<uint8_t, 32u> mac{};
    qsp::hmacSha256(enrollment.credential.data(), enrollment.credential.size(),
                    macInput.data(), macInput.size(), mac);
    try {
        out.assign(kTopologyEnvelopePrefixBytes + qsrLength, 0u);
    } catch (...) {
        return false;
    }
    writeLe32(out.data(), topology::kTopologyControlMagic);
    writeLe16(out.data() + 4u, topology::kTopologyControlVersion);
    out[6] = 1u;
    out[7] = 0u;
    writeLe16(out.data() + 8u, static_cast<uint16_t>(qsrLength));
    writeLe16(out.data() + 10u, 0u);
    writeLe64(out.data() + 12u, enrollment.authorityGeneration);
    writeLe64(out.data() + 20u, enrollment.nodeId);
    writeLe64(out.data() + 28u, sequence);
    std::memcpy(out.data() + 36u, mac.data(), topology::kTopologyMacBytes);
    std::memcpy(out.data() + kTopologyEnvelopePrefixBytes, qsr, qsrLength);
    return true;
}

bool makeChildAdvertisement(const ChildEnrollment& child,
                            const RouteEndpoint& qspEndpoint,
                            const RouteEndpoint& qsrEndpoint,
                            model::NodeAdvertisement& out) {
    out = {};
    model::StreamCompatibilityIdentity stream{};
    const qsp::GameDescriptor descriptor = gameDescriptor();
    if (!model::makeStreamCompatibilityIdentity(
            descriptor.compatibilityIdentity, kSessionId, kEpoch,
            qsp::gameStartFingerprint(qsp::canonicalGameStart(descriptor)),
            stream)) {
        return false;
    }
    out.nodeId = child.enrollment.nodeId;
    out.kind = model::NodeKind::SpectatorRelay;
    out.stream = stream;
    out.route.kind = model::RouteKind::Direct;
    out.route.ipv4BE = qspEndpoint.ipv4BE;
    out.route.port = qspEndpoint.port;
    out.route.generation = kViewerGeneration;
    out.route.routeProved = true;
    out.controlRoute.kind = model::RouteKind::Direct;
    out.controlRoute.ipv4BE = qsrEndpoint.ipv4BE;
    out.controlRoute.port = qsrEndpoint.port;
    out.controlRoute.generation = kViewerGeneration;
    out.controlRoute.routeProved = true;
    out.capabilityModelRevision = model::kResilientCapabilityModelRevision;
    out.capabilities = model::kCapabilityRelayCapableSpectator
        | model::kCapabilityTopologyLeaf;
    out.childCapacity = 0u;
    out.childCount = 0u;
    out.uploadCapacity = 1u;
    out.uploadInUse = 0u;
    out.cacheFirstCursor = child.transcript.nextRecord;
    out.cacheNextCursor = child.transcript.nextRecord;
    out.sourceGeneration = kSourceGeneration;
    out.routeGeneration = kViewerGeneration;
    out.lastProgressMs = 1u;
    out.leaseExpiryMs = child.enrollment.expiresMs;
    out.reachable = true;
    out.draining = false;
    return true;
}

bool decodeQtaControl(const std::vector<uint8_t>& bytes,
                      const ChildEnrollment& child,
                      wire::ControlDatagram& out) {
    out = {};
    if (bytes.size() < kTopologyEnvelopePrefixBytes
        || readLe32(bytes.data()) != topology::kTopologyControlMagic
        || readLe16(bytes.data() + 4u) != topology::kTopologyControlVersion
        || bytes[6] != 1u || bytes[7] != 0u
        || readLe16(bytes.data() + 10u) != 0u
        || readLe64(bytes.data() + 12u)
            != child.enrollment.authorityGeneration
        || readLe64(bytes.data() + 20u) != child.enrollment.nodeId) {
        return false;
    }
    const size_t qsrLength = readLe16(bytes.data() + 8u);
    if (qsrLength == 0u || qsrLength + kTopologyEnvelopePrefixBytes
            != bytes.size()) {
        return false;
    }
    return wire::decodeControlDatagram(
            bytes.data() + kTopologyEnvelopePrefixBytes, qsrLength,
            out) == wire::DecodeResult::Ok;
}

bool validQtaLease(const wire::ControlDatagram& datagram,
                   const ChildEnrollment& child,
                   model::ParentLease& out) {
    out = {};
    if (datagram.type != wire::ControlMessageType::ParentLease) return false;
    out = datagram.parentLease;
    return out.childId == child.enrollment.nodeId
        && out.parentId == child.enrollment.rootNodeId
        && out.ancestryCount == 1u
        && out.ancestry[0] == child.enrollment.rootNodeId
        && out.routeCount == 1u
        && out.routes[0].port == child.enrollment.rootQsp.advertised.port;
}

bool sendPrivateControl(LoopbackUdpSocket& socket,
                        const RouteEndpoint& destination,
                        const ControlType type,
                        const std::vector<uint8_t>& payload,
                        std::string& error) {
    std::vector<uint8_t> encoded;
    return encodeControl(type, payload, encoded)
        && socket.send(destination, encoded, error);
}

bool childQspIngressAccepted(const qsp::IngressResult result) {
    return result == qsp::IngressResult::Accepted
        || result == qsp::IngressResult::ReplayedSequence;
}

int runChild(const char* const qspPortText, const char* const controlPortText) {
    uint16_t qspPort = 0u;
    uint16_t controlPort = 0u;
    if (!parsePort(qspPortText, qspPort)
        || !parsePort(controlPortText, controlPort)) {
        return 2;
    }
    WinsockScope winsock;
    if (!winsock.start()) return 3;

    LoopbackUdpSocket socket;
    uint16_t childPort = 0u;
    std::string error;
    if (!socket.open(childPort, error)) {
        fail("child", error);
        return 4;
    }
    LoopbackUdpSocket qsrSocket;
    uint16_t qsrPort = 0u;
    if (!qsrSocket.open(qsrPort, error) || qsrPort == childPort) {
        fail("child", error.empty()
                          ? "could not bind a distinct QSR control socket"
                          : error);
        return 24;
    }
    const RouteEndpoint qspRoot{loopbackIpv4BE(), qspPort};
    const RouteEndpoint controlRoot{loopbackIpv4BE(), controlPort};
    const RouteEndpoint qspSelf{loopbackIpv4BE(), childPort};
    const RouteEndpoint qsrSelf{loopbackIpv4BE(), qsrPort};

    qsp::ViewerConfig viewerConfig{};
    viewerConfig.identity = sessionIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.secureRandom = kViewerId;
    viewerConfig.viewerNonce = kViewerId;
    qsp::SpectatorViewer viewer(viewerConfig);
    if (!viewer.isConfigured() || !viewer.begin(kGameId)) {
        fail("child", "SpectatorViewer fixture was not configured/begun");
        return 5;
    }
    std::vector<uint8_t> readyPayload;
    appendU16(readyPayload, childPort);
    if (!sendPrivateControl(socket, controlRoot, ControlType::ChildReady,
                            readyPayload, error)) {
        fail("child", error);
        return 6;
    }

    bool initAckSent = false;
    bool enrollmentReceived = false;
    bool nodeSent = false;
    bool offerReceived = false;
    bool redemptionSent = false;
    bool qspAckAfterTopology = false;
    bool detachRequested = false;
    bool detachedSent = false;
    ChildEnrollment enrollment{};
    const uint64_t started = nowMs();
    while (!elapsed(started, kPumpTimeoutMs)) {
        std::vector<uint8_t> bytes;
        RouteEndpoint sender{};
        const ReceiveResult received = socket.receive(bytes, sender, 3u, error);
        if (received == ReceiveResult::Error) {
            fail("child", error);
            return 7;
        }
        if (received == ReceiveResult::Received) {
            ControlMessage control{};
            if (sender == controlRoot
                && decodeControl(bytes.data(), bytes.size(), control)) {
                if (control.type == ControlType::Enrollment) {
                    if (enrollmentReceived
                        || !decodeChildEnrollment(control.payload, enrollment)
                        || enrollment.enrollment.rootQsr.advertised.port == qspPort
                        || enrollment.enrollment.rootQsp.advertised.port
                            != qspPort) {
                        fail("child", "invalid or duplicate enrollment control");
                        return 8;
                    }
                    enrollmentReceived = true;
                    model::NodeAdvertisement advertisement{};
                    std::vector<uint8_t> inner;
                    std::vector<uint8_t> envelope;
                    if (!makeChildAdvertisement(enrollment, qspSelf, qsrSelf,
                                                advertisement)) {
                        fail("child", "could not construct leaf advertisement");
                        return 9;
                    }
                    wire::ControlDatagram datagram{};
                    datagram.type = wire::ControlMessageType::NodeAdvertisement;
                    datagram.nodeAdvertisement = advertisement;
                    if (!wire::encodeControlDatagram(datagram, inner)
                        || !makeClientTopologyEnvelope(
                            enrollment, qspSelf, 0u, 1u, inner.data(), inner.size(),
                            envelope)
                        || !qsrSocket.send(
                            RouteEndpoint{
                                enrollment.enrollment.rootQsr.advertised.ipv4BE,
                                enrollment.enrollment.rootQsr.advertised.port},
                            envelope, error)) {
                        fail("child", error.empty()
                                      ? "could not send authenticated QSR node advertisement"
                                      : error);
                        return 10;
                    }
                    nodeSent = true;
                    if (!sendPrivateControl(socket, controlRoot,
                                            ControlType::LeafOnly, {}, error)) {
                        fail("child", error);
                        return 11;
                    }
                } else if (control.type == ControlType::BeginDetach) {
                    if (!qspAckAfterTopology || detachRequested
                        || !control.payload.empty() || !viewer.detach()) {
                        fail("child", "parent requested an invalid QSP detach");
                        return 12;
                    }
                    detachRequested = true;
                } else if (control.type == ControlType::Finish) {
                    if (initAckSent && enrollmentReceived && nodeSent
                        && offerReceived && redemptionSent
                        && qspAckAfterTopology && detachedSent) {
                        return 0;
                    }
                    fail("child", "parent finished before full QSP/QSR sequence");
                    return 13;
                }
            } else if (sender == qspRoot) {
                if (!childQspIngressAccepted(
                        viewer.onDatagram(bytes.data(), bytes.size(), nowMs()))) {
                    fail("child", "ordinary QSP ingress was rejected");
                    return 18;
                }
            }
        }

        std::vector<uint8_t> qsrBytes;
        RouteEndpoint qsrSender{};
        const ReceiveResult qsrReceived = qsrSocket.receive(
            qsrBytes, qsrSender, 0u, error);
        if (qsrReceived == ReceiveResult::Error) {
            fail("child", error);
            return 25;
        }
        if (qsrReceived == ReceiveResult::Received) {
            if (!enrollmentReceived
                || qsrSender.port
                    != enrollment.enrollment.rootQsr.advertised.port
                || qsrSender.ipv4BE
                    != enrollment.enrollment.rootQsr.advertised.ipv4BE
                || !topology::hasTopologyControlEnvelopeMagic(
                    qsrBytes.data(), qsrBytes.size())) {
                fail("child", "QSR response was not the expected parent offer");
                return 14;
            }
            wire::ControlDatagram qta{};
            if (!decodeQtaControl(qsrBytes, enrollment, qta)) {
                fail("child", "QSR response was not a valid topology envelope");
                return 14;
            }
            if (qta.type == wire::ControlMessageType::ParentCommit) continue;
            if (qta.type == wire::ControlMessageType::AuthorityState) {
                const wire::AuthorityState& state = qta.authorityState;
                if (state.authorityGeneration
                        != enrollment.enrollment.authorityGeneration
                    || state.rootNodeId != enrollment.enrollment.rootNodeId
                    || state.childNodeId != enrollment.enrollment.nodeId
                    || state.viewerId != kViewerId
                    || state.viewerGeneration != kViewerGeneration
                    || (state.kind == wire::AuthorityStateKind::Liveness
                        && state.livenessWindowMs == 0u)) {
                    fail("child", "QSR authority state did not match enrollment");
                    return 14;
                }
                continue;
            }
            model::ParentLease lease{};
            if (offerReceived || !validQtaLease(qta, enrollment, lease)) {
                fail("child", "QSR response was not the expected parent offer");
                return 14;
            }
            offerReceived = true;
            if (!sendPrivateControl(socket, controlRoot,
                                    ControlType::OfferReceived, {}, error)) {
                fail("child", error);
                return 15;
            }
            wire::ControlDatagram redeem{};
            redeem.type = wire::ControlMessageType::ParentLease;
            redeem.parentLease = lease;
            std::vector<uint8_t> inner;
            std::vector<uint8_t> envelope;
            if (!wire::encodeControlDatagram(redeem, inner)
                || !makeClientTopologyEnvelope(
                    enrollment, qspSelf, 0u, 2u, inner.data(), inner.size(),
                    envelope)
                || !qsrSocket.send(
                    RouteEndpoint{
                        enrollment.enrollment.rootQsr.advertised.ipv4BE,
                        enrollment.enrollment.rootQsr.advertised.port},
                    envelope, error)) {
                fail("child", error.empty()
                              ? "could not redeem the authenticated QSR offer"
                              : error);
                return 16;
            }
            redemptionSent = true;
            if (!sendPrivateControl(socket, controlRoot,
                                    ControlType::RedemptionSent, {}, error)) {
                fail("child", error);
                return 17;
            }
        }

        if (!initAckSent || redemptionSent) {
            for (const qsp::ViewerDatagram& packet
                 : viewer.pollOutbound(nowMs(), 4u)) {
                if (!socket.send(qspRoot, packet.bytes, error)) {
                    fail("child", error);
                    return 19;
                }
                if (packet.type == qsp::MessageType::InitAck) {
                    initAckSent = true;
                    if (!sendPrivateControl(socket, controlRoot,
                                            ControlType::InitAckSent, {}, error)) {
                        fail("child", error);
                        return 20;
                    }
                }
                if (redemptionSent && packet.type == qsp::MessageType::Ack
                    && viewer.receivedNextRecord() >= 1u
                    && !qspAckAfterTopology) {
                    qspAckAfterTopology = true;
                    if (!sendPrivateControl(
                            socket, controlRoot,
                            ControlType::QspAckAfterTopology, {}, error)) {
                        fail("child", error);
                        return 21;
                    }
                }
                if (detachRequested && packet.type == qsp::MessageType::Detach
                    && !detachedSent) {
                    detachedSent = true;
                    if (!sendPrivateControl(socket, controlRoot,
                                            ControlType::Detached, {}, error)) {
                        fail("child", error);
                        return 22;
                    }
                }
            }
        }
    }
    fail("child", "timed out waiting for authenticated topology flow");
    return 23;
}

bool publishStart(SpectatorSidecar& sidecar, uint64_t& sequence,
                  uint64_t now) {
    PublicationEvent event{};
    event.kind = PublicationKind::Start;
    event.sequence = sequence++;
    event.descriptor = gameDescriptor();
    sidecar.publish(event, now);
    return sidecar.phase() == SidecarPhase::Active;
}

VerifiedCursor publishVerifiedRecord(SpectatorSidecar& sidecar,
                                     uint64_t& sequence, uint64_t now) {
    const qsp::GameDescriptor descriptor = gameDescriptor();
    qsp::ConfirmedRecord record = recordFor(0u);
    record.rollingDigest = qsp::advanceRollingDigest(
        descriptor.initialDigest, descriptor.gameId, record);
    PublicationEvent recordEvent{};
    recordEvent.kind = PublicationKind::Record;
    recordEvent.sequence = sequence++;
    recordEvent.record = record;
    sidecar.publish(recordEvent, now);
    PublicationEvent proof{};
    proof.kind = PublicationKind::TailProof;
    proof.sequence = sequence++;
    proof.tailRecordCursor = 1u;
    proof.postTailStateHash = stateHashFor(1u);
    proof.hasPostTailProof = true;
    sidecar.publish(proof, now);
    return VerifiedCursor{1u, 0u, record.rollingDigest};
}

bool runForcedQsrBindFailure() {
    uint64_t sequence = 1u;
    uint64_t clock = kInitialNowMs;
    SpectatorSidecar sidecar(sidecarConfig());
    std::string error;
    if (!sidecar.start(qclient::spectator::makeUdpSidecarTransport(), nullptr,
                       clock, error)
        || sidecar.boundPort() == 0u || !publishStart(sidecar, sequence, clock)) {
        return false;
    }
    topology::TopologyActivationRequest request =
        activationRequest(sidecar.boundPort());
    request.forceQsrBindFailure = true;
    const bool configured = sidecar.configureTopologyAuthority(
        request, clock + 1u, error);
    const auto status = sidecar.status();
    const bool pass = !configured && sidecar.boundPort() != 0u
        && sidecar.phase() == SidecarPhase::Active
        && !status.topologyAuthorityReady && status.topologyQsrPort == 0u
        && status.failureReason.empty();
    (void)sidecar.stop(false, clock + 2u);
    return pass;
}

bool runParent() {
    WinsockScope winsock;
    if (!winsock.start()) {
        fail("parent", "WSAStartup failed");
        return false;
    }

    LoopbackUdpSocket controlSocket;
    uint16_t controlPort = 0u;
    std::string error;
    if (!controlSocket.open(controlPort, error)) {
        fail("parent", error);
        return false;
    }

    uint64_t clock = kInitialNowMs;
    uint64_t sequence = 1u;
    SpectatorSidecar sidecar(sidecarConfig());
    if (!sidecar.start(qclient::spectator::makeUdpSidecarTransport(), nullptr,
                       clock, error)
        || sidecar.boundPort() == 0u) {
        fail("parent", error.empty() ? "production sidecar QSP bind failed" : error);
        return false;
    }
    if (!publishStart(sidecar, sequence, clock)) {
        fail("parent", "production sidecar rejected the descriptor");
        return false;
    }
    if (!sidecar.configureTopologyAuthority(
            activationRequest(sidecar.boundPort()), clock + 1u, error)) {
        fail("parent", error.empty()
                           ? "production sidecar did not activate QSR authority"
                           : error);
        return false;
    }
    const auto initialStatus = sidecar.status();
    if (!initialStatus.topologyAuthorityReady
        || initialStatus.topologyQsrPort == 0u
        || initialStatus.topologyQsrPort == sidecar.boundPort()) {
        fail("parent", "QSR authority was not separately bound from QSP");
        return false;
    }

    HiddenProcess child;
    if (!child.launch(L"--topology-child "
                          + std::to_wstring(sidecar.boundPort()) + L" "
                          + std::to_wstring(controlPort),
                      error)) {
        fail("parent", error);
        return false;
    }

    RouteEndpoint childEndpoint{};
    bool childReady = false;
    bool initAckSent = false;
    bool enrollmentSent = false;
    bool leafOnly = false;
    bool offerReceived = false;
    bool redemptionSent = false;
    bool qspAckAfterTopology = false;
    bool attachmentObserved = false;
    bool detachRequested = false;
    bool detached = false;
    bool finishSent = false;
    unsigned postAckTurns = 0u;
    unsigned postDetachTurns = 0u;
    topology::TopologyEnrollment enrollment{};
    VerifiedCursor verified{};
    const uint64_t started = nowMs();

    while (!elapsed(started, kPumpTimeoutMs)) {
        ++clock;
        sidecar.service(clock);

        for (unsigned packet = 0u; packet < 4u; ++packet) {
            std::vector<uint8_t> bytes;
            RouteEndpoint sender{};
            const ReceiveResult received =
                controlSocket.receive(bytes, sender, 1u, error);
            if (received == ReceiveResult::Error) {
                DWORD childExit = STILL_ACTIVE;
                if (child.exited(childExit)) {
                    const auto status = sidecar.status();
                    fail("parent", "child exited early with "
                        + std::to_string(childExit)
                        + " topologyAttached="
                        + std::to_string(status.topologyAttached)
                        + " topologyEnrolled="
                        + std::to_string(status.topologyEnrolled));
                } else {
                    fail("parent", error);
                }
                return false;
            }
            if (received != ReceiveResult::Received) break;
            ControlMessage control{};
            if (sender.ipv4BE != loopbackIpv4BE()
                || !decodeControl(bytes.data(), bytes.size(), control)) {
                fail("parent", "received an invalid private child control packet");
                return false;
            }
            switch (control.type) {
            case ControlType::ChildReady: {
                Reader reader(control.payload.data(), control.payload.size());
                uint16_t childPort = 0u;
                if (childReady || !reader.u16(childPort) || !reader.done()
                    || childPort == 0u || sender.port != childPort) {
                    fail("parent", "child Ready did not bind one stable UDP endpoint");
                    return false;
                }
                childReady = true;
                childEndpoint = sender;
                break;
            }
            case ControlType::InitAckSent:
                if (!childReady || initAckSent || !control.payload.empty()) {
                    fail("parent", "unexpected child InitAck rendezvous");
                    return false;
                }
                initAckSent = true;
                break;
            case ControlType::LeafOnly:
                if (!enrollmentSent || leafOnly || !control.payload.empty()) {
                    fail("parent", "unexpected leaf-only topology status");
                    return false;
                }
                leafOnly = true;
                break;
            case ControlType::OfferReceived:
                if (!leafOnly || offerReceived || !control.payload.empty()) {
                    fail("parent", "unexpected root parent-offer status");
                    return false;
                }
                offerReceived = true;
                break;
            case ControlType::RedemptionSent:
                if (!offerReceived || redemptionSent || !control.payload.empty()) {
                    fail("parent", "unexpected parent-lease redemption status");
                    return false;
                }
                redemptionSent = true;
                break;
            case ControlType::QspAckAfterTopology:
                if (!redemptionSent || qspAckAfterTopology
                    || !control.payload.empty()) {
                    fail("parent", "ordinary QSP did not resume after QSR control");
                    return false;
                }
                qspAckAfterTopology = true;
                break;
            case ControlType::Detached:
                if (!detachRequested || detached || !control.payload.empty()) {
                    fail("parent", "unexpected QSP detach status");
                    return false;
                }
                detached = true;
                break;
            default:
                fail("parent", "child emitted an invalid private control type");
                return false;
            }
        }

        if (initAckSent && !enrollmentSent) {
            std::string enrollmentError;
            if (sidecar.enrollTopologyViewer(
                    kViewerId, kViewerGeneration, true, clock, enrollment,
                    enrollmentError)) {
                verified = publishVerifiedRecord(sidecar, sequence, ++clock);
                ChildEnrollment response{};
                response.enrollment = enrollment;
                response.transcript = {
                    enrollment.qtaCursor.nextRecord,
                    enrollment.qtaCursor.nextEvent,
                    enrollment.qtaCursor.digest,
                };
                response.verified = verified;
                std::vector<uint8_t> payload;
                if (!encodeChildEnrollment(response, payload)
                    || !sendPrivateControl(controlSocket, childEndpoint,
                                            ControlType::Enrollment, payload,
                                            error)) {
                    fail("parent", error.empty()
                                       ? "could not send topology enrollment"
                                       : error);
                    return false;
                }
                enrollmentSent = true;
            }
        }

        if (redemptionSent && !attachmentObserved) {
            const auto attached = sidecar.status();
            attachmentObserved = attached.topologyAttached == 1u
                && attached.topologyEnrolled == 1u;
        }

        if (qspAckAfterTopology && attachmentObserved && !detachRequested) {
            ++postAckTurns;
            if (postAckTurns >= 8u) {
                if (!sendPrivateControl(controlSocket, childEndpoint,
                                        ControlType::BeginDetach, {}, error)) {
                    fail("parent", error);
                    return false;
                }
                detachRequested = true;
            }
        }
        if (detached) {
            ++postDetachTurns;
            const auto afterDetach = sidecar.status();
            if (postDetachTurns >= 4u
                && (afterDetach.topologyAttached != 0u
                    || afterDetach.topologyEnrolled != 0u
                    || afterDetach.viewerCount != 0u)) {
                fail("parent", "QSP detach did not revoke the attached topology leaf");
                return false;
            }
            if (!finishSent && postDetachTurns >= 4u) {
                if (!sendPrivateControl(controlSocket, childEndpoint,
                                        ControlType::Finish, {}, error)) {
                    fail("parent", error);
                    return false;
                }
                finishSent = true;
            }
            if (finishSent && postDetachTurns >= 16u) break;
        }
    }

    if (!childReady || !initAckSent || !enrollmentSent || !leafOnly
        || !offerReceived || !redemptionSent || !qspAckAfterTopology
        || !attachmentObserved || !detachRequested || !detached
        || !finishSent) {
        fail("parent", "timed out before the complete QSP/QSR flow"
            " ready=" + std::to_string(childReady)
            + " initAck=" + std::to_string(initAckSent)
            + " enrolled=" + std::to_string(enrollmentSent)
            + " leaf=" + std::to_string(leafOnly)
            + " offer=" + std::to_string(offerReceived)
            + " redeem=" + std::to_string(redemptionSent)
            + " qspAck=" + std::to_string(qspAckAfterTopology)
            + " attached=" + std::to_string(attachmentObserved)
            + " detach=" + std::to_string(detachRequested)
            + " detached=" + std::to_string(detached)
            + " finish=" + std::to_string(finishSent));
        return false;
    }

    DWORD childExit = 0u;
    if (!child.waitForExit(kChildTimeoutMs, childExit, error)
        || childExit != 0u) {
        fail("parent", error.empty()
                           ? "child process failed with "
                               + std::to_string(childExit)
                           : error);
        return false;
    }

    const auto status = sidecar.status();
    if (!status.topologyAuthorityReady
        || status.topologyQsrPort == 0u
        || status.topologyQsrPort == sidecar.boundPort()
        || status.topologyEnrolled != 0u
        || status.topologyAttached != 0u
        || status.viewerCount != 0u
        || status.confirmedRecords < 1u
        || !status.failureReason.empty()) {
        fail("parent", "production sidecar status did not preserve QSP/QSR state");
        return false;
    }
    (void)sidecar.stop(false, ++clock);

    if (!runForcedQsrBindFailure()) {
        fail("parent", "forced QSR bind failure did not leave ordinary QSP active");
        return false;
    }
    return true;
}

}

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--topology-child") == 0)
        return runChild(argv[2], argv[3]);
    if (argc != 1) {
        std::fprintf(stderr, "usage: %s [--topology-child <qsp-port> <control-port>]\n",
                     argc > 0 ? argv[0]
                              : "qoh99_resilient_topology_authority_process");
        return 2;
    }
    if (!runParent()) return 1;
    std::puts("authenticated topology authority production UDP/process seam: PASS");
    return 0;
}
