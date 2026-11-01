#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "../ReleasedClientRootControl.h"
#include "../ReleasedTopologyControlV2.h"
#include "../ResilientTopologyClientRuntime.h"
#include "../SpectatorSidecar.h"

#include "../../../ipc/IpcConfig.h"

#include "../../../compat/CompatibilityIdentity.h"
#include "../../../net/ControlLink.h"
#include "../../../net/Protocol.h"
#include "../../../net/RelayProtocol.h"
#include "../../../net/Sockets.h"
#include "../../../netplay/WireProtocol.h"
#include "../../../relay/RelayService.h"
#include "../../../spectator/SpectatorGrant.h"
#include "../../../spectator/SpectatorRelayAdapter.h"
#include "../../../spectator/SpectatorSession.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace qclient::spectator::topology::client::testing {

struct ReleasedTopologyClientRuntimeTestAccess final {
    using Runtime = qclient::spectator::topology::client::
        ReleasedTopologyClientRuntime;
    using Activation = qnetplay::spectator::resilient::wire::
        RelayChildActivation;
    using Retirement = qnetplay::spectator::resilient::wire::
        RelayChildActivationRetirement;
    using ClientRootActivation = qnetplay::spectator::resilient::wire::
        ClientPlayerRootActivation;
    using ClientRootRetirement = qnetplay::spectator::resilient::wire::
        ClientPlayerRootActivationRetirement;

    static void installCompletedS2Activation(Runtime& runtime,
                                             const Activation& activation) {
        runtime._state = ReleasedTopologyClientState::Attached;
        runtime._reason = "attached-via-paired-activation";
        runtime.retainCompletedParentRouteActivation(activation);
    }

    static bool acceptRetirement(Runtime& runtime,
                                 const Retirement& retirement,
                                 const uint64_t nowMs) {
        return runtime.acceptRelayChildActivationRetirement(retirement, nowMs);
    }

    static bool hasCompletedS2Activation(const Runtime& runtime) {
        return runtime._completedParentRouteActivation.has_value();
    }

    static bool completedS2Retirement(const Runtime& runtime,
                                      Retirement& out) {
        out = {};
        if (!runtime._completedParentRouteActivation.has_value()) return false;
        out = *runtime._completedParentRouteActivation;
        return true;
    }

    static bool installCompletedClientRootActivation(
        Runtime& runtime, const ClientRootActivation& activation,
        const TopologyAuthorityPublicBlob& publicKey) {
        runtime._state = ReleasedTopologyClientState::Attached;
        runtime._reason = "attached-via-client-player-root-activation";
        if (!runtime._assignmentVerifier.initialize(publicKey)) return false;
        runtime.retainCompletedClientPlayerRootActivation(activation);
        return runtime._completedClientPlayerRootActivation.has_value();
    }

    static bool acceptClientRootRetirement(
        Runtime& runtime, const ClientRootRetirement& retirement,
        const uint64_t nowMs) {
        return runtime.acceptClientPlayerRootActivationRetirement(retirement,
                                                                   nowMs);
    }

    static bool hasCompletedClientRootActivation(const Runtime& runtime) {
        return runtime._completedClientPlayerRootActivation.has_value();
    }
};

}

namespace {

namespace qsp = qnetplay::spectator;
namespace topology = qclient::spectator::topology;
namespace controlv2 = qclient::spectator::topology::controlv2;
namespace model = qnetplay::spectator::resilient;
namespace wire = qnetplay::spectator::resilient::wire;

using qclient::spectator::AdmissionDenyReason;
using qclient::spectator::AdmissionPolicy;
using qclient::spectator::GrantRequest;
using qclient::spectator::PublicationEvent;
using qclient::spectator::PublicationKind;
using qclient::spectator::RouteEndpoint;
using qclient::spectator::SidecarConfig;
using qclient::spectator::SidecarPhase;
using qclient::spectator::SpectatorSidecar;

constexpr uint64_t kSessionId = 0x00000000744F504Full;
constexpr uint64_t kEpoch = 0x0000000000000042ull;
constexpr uint64_t kSimulationContract = 0x515348544F504331ull;
constexpr uint64_t kGameId = 0x515348544F504332ull;
constexpr uint64_t kViewerId = 0x544F504F56494557ull;
constexpr uint64_t kPhaseCRelayViewerId = 0x5048432D52454C31ull;
constexpr uint64_t kPhaseCChildViewerId = 0x5048432D43484C32ull;
constexpr uint64_t kPhaseDGrandchildViewerId = 0x5048442D47524333ull;
constexpr uint64_t kInitialNowMs = 1000u;
constexpr uint64_t kPumpTimeoutMs = 18000u;
constexpr DWORD kChildTimeoutMs = 20000u;
constexpr uint64_t kPhaseCPumpTimeoutMs = 24000u;
constexpr DWORD kPhaseCChildTimeoutMs = 26000u;

void fail(const char* const phase, const std::string& detail) {
    std::fprintf(stderr, "[released-topology-client-process] %s: %s\n", phase,
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

qnet::Endpoint loopbackEndpoint(const uint16_t port) {
    qnet::Endpoint endpoint{};
    endpoint.sa.sin_addr.s_addr = loopbackIpv4BE();
    endpoint.sa.sin_port = htons(port);
    return endpoint;
}

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

bool parseB10Case(const char* const value, uint32_t& out) {
    out = 0u;
    if (value == nullptr || *value == '\0') return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0u || parsed > 46u)
        return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool b10ExpectsTopologyFailure(const uint32_t b10Case) {
    return b10Case != 0u && b10Case != 27u && b10Case != 37u
        && b10Case != 39u;
}

enum class B10FailureTiming : uint8_t {
    None,
    BeforeAttachment,
    AfterAttachment,
};

B10FailureTiming b10FailureTiming(const uint32_t b10Case) {
    if (!b10ExpectsTopologyFailure(b10Case)) return B10FailureTiming::None;
    return b10Case == 3u ? B10FailureTiming::AfterAttachment
                          : B10FailureTiming::BeforeAttachment;
}

const char* b10ExpectedFailureReason(const uint32_t b10Case) {
    switch (b10Case) {
    case 1u:
    case 8u:
    case 15u:
    case 16u:
    case 28u:
    case 41u:
        return "topology-response-transcript-rejected";
    case 2u:
    case 3u:
        return "topology-credential-expired";
    case 4u:
    case 7u:
    case 13u:
    case 14u:
    case 19u:
    case 20u:
    case 22u:
    case 42u:
        return "qsr-timed-out";
    case 5u:
    case 6u:
        return "topology-qsr-control-rejected";
    case 10u:
    case 11u:
    case 12u:
    case 9u:
    case 17u:
    case 18u:
    case 26u:
    case 29u:
    case 30u:
    case 35u:
    case 40u:
    case 46u:
        return "topology-response-binding-rejected";
    case 34u:
        return "topology-control-setup-timed-out";
    case 21u:
    case 38u:
        return "topology-control-setup-timed-out";
    case 23u:
        return "topology-response-authentication-rejected";
    case 24u:
    case 25u:
        return "topology-control-response-malformed";
    case 45u:
        return "topology-authority-revoked";
    default:
        return nullptr;
    }
}

enum class B10InfoDelivery : uint8_t {
    Exact,
    Duplicate,
    Truncated,
    Oversized,
};

B10InfoDelivery prepareB10Info(const uint32_t b10Case,
                                qnet::SpectateTopologyInfoV2& info,
                                const qsp::SpectatorGrant& grant) {
    switch (b10Case) {
    case 1u: info.expiryMs = 0u; break;
    case 4u: ++info.authorityGeneration; break;
    case 7u: info.authorityGeneration ^= 0x0100000000000000ull; break;
    case 8u: ++info.sessionId; break;
    case 9u: ++info.sessionEpoch; break;
    case 10u: info.compatibilityIdentity.stateSchemaId ^= 0x01u; break;
    case 11u: ++info.streamRootIdentity; break;
    case 12u: ++info.sourceGeneration; break;
    case 13u: ++info.rootNodeId; break;
    case 14u: ++info.childNodeId; break;
    case 15u: ++info.viewerId; break;
    case 16u: ++info.viewerGeneration; break;
    case 17u: info.qspIpv4BE ^= 0x00010000u; break;
    case 18u: ++info.qspRouteGeneration; break;
    case 19u: ++info.rootQsr.advertisedPort; break;
    case 20u: ++info.bindingGeneration; break;
    case 21u: ++info.requestNonce; break;
    case 22u: ++info.qtaCursor.digest; break;
    case 23u: break;
    case 24u: return B10InfoDelivery::Truncated;
    case 25u: return B10InfoDelivery::Oversized;
    case 26u: ++info.schemaVersion; break;
    case 27u: return B10InfoDelivery::Duplicate;
    case 28u: ++info.sessionId; break;
    case 29u:
        info.rootQsr.advertisedPort = info.rootQsp.advertisedPort;
        info.rootQsr.localPort = info.rootQsp.localPort;
        break;
    case 30u:
        info.rootQsr.advertisedIpv4BE = info.rootQsp.advertisedIpv4BE;
        info.rootQsr.advertisedPort = info.rootQsp.advertisedPort;
        break;
    case 35u: info.qspRoute = qnet::SPECTATE_ROUTE_RELAY; break;
    case 40u: ++info.qspRouteGeneration; break;
    case 41u: ++info.viewerGeneration; break;
    case 42u: ++info.authorityGeneration; break;
    case 46u: info.relayIpv4BE = 0x0100007Fu; break;
    default: break;
    }
    if (!controlv2::signInfo(info, grant)) return B10InfoDelivery::Truncated;
    if (b10Case == 23u) info.responseTag[0] ^= 0x80u;
    return B10InfoDelivery::Exact;
}

class NetScope final {
public:
    bool start() {
        _started = qnet::netInit();
        return _started;
    }

    ~NetScope() {
        if (_started) qnet::netShutdown();
    }

private:
    bool _started = false;
};

class HiddenProcess final {
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
        const HANDLE childOut = GetStdHandle(STD_OUTPUT_HANDLE);
        const HANDLE childErr = GetStdHandle(STD_ERROR_HANDLE);
        BOOL inheritHandles = FALSE;
        if (childOut != nullptr && childOut != INVALID_HANDLE_VALUE
            && childErr != nullptr && childErr != INVALID_HANDLE_VALUE) {
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.hStdOutput = childOut;
            startup.hStdError = childErr;
            inheritHandles = TRUE;
        }
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(modulePath.c_str(), mutableCommand.data(), nullptr,
                            nullptr, inheritHandles, 0, nullptr, nullptr,
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
    identity.simulationContent.manifestFlags = qoh99::content::ManifestFlagComplete;
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
    descriptor.metadata = {0x54u, 0x4Fu, 0x50u, 0x32u};
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

qsp::SpectatorGrantKey grantKey() {
    qsp::SpectatorGrantKey key{};
    for (size_t index = 0u; index < key.size(); ++index)
        key[index] = static_cast<uint8_t>(0x31u + index);
    return key;
}

SidecarConfig sidecarConfig() {
    const qsp::SpectatorGrantKey key = grantKey();
    auto randomState = std::make_shared<uint64_t>(0x9000000000000000ull);
    SidecarConfig config{};
    config.identity = sessionIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = compatibilityIdentity();
    config.listenPort = 0u;
    config.policy = AdmissionPolicy::Public;
    config.maxViewers = 2u;
    config.maxReservations = 4u;
    config.retainAllRecords = true;
    config.maxRetainedBytes = 256u * 1024u;
    config.unixClock = [] { return uint64_t{1700000000u}; };
    config.randomU64 = [randomState] { return ++*randomState; };
    config.signGrant = [key](qsp::SpectatorGrant& grant) {
        (void)qsp::issueGrant(key, grant);
    };
    config.verifyGrant = [key](const qsp::SpectatorGrant& grant,
                               const uint64_t unixNow) {
        return qsp::verifyGrant(key, grant, grant.viewerId, kSessionId,
                                unixNow) == qsp::GrantVerdict::Ok;
    };
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

topology::EndpointBinding rootQsrBinding(const uint16_t port) {
    topology::EndpointBinding binding{};
    binding.local = RouteEndpoint{0u, port};
    binding.localProvenance = topology::EndpointProvenance::LocalBind;
    binding.advertised.kind = model::RouteKind::Direct;
    binding.advertised.ipv4BE = loopbackIpv4BE();
    binding.advertised.port = port;
    binding.advertised.generation = 1u;
    binding.advertised.routeProved = false;
    binding.advertisedProvenance =
        topology::EndpointProvenance::PublishedDirectCandidate;
    binding.generation = 1u;
    return binding;
}

topology::TopologyActivationRequest activationRequest(
    const uint16_t qspPort, const uint64_t standbyLeaseMs = 0u) {
    topology::TopologyActivationRequest request{};
    request.requested = true;
    request.rootQsp = rootQspBinding(qspPort);
    request.sourceGeneration = 1u;
    if (standbyLeaseMs != 0u)
        request.limits.standbyLeaseMs = standbyLeaseMs;
    return request;
}

topology::TopologyActivationRequest phaseCActivationRequest(
    const uint16_t qspPort) {
    topology::TopologyActivationRequest request = activationRequest(qspPort);
    request.limits.maxTopologyNodes = 3u;
    request.limits.maxChildrenPerRoot = 1u;
    request.limits.maxChildrenPerRelay = 1u;
    return request;
}

topology::TopologyActivationRequest phaseDActivationRequest(
    const uint16_t qspPort) {
    topology::TopologyActivationRequest request = phaseCActivationRequest(
        qspPort);
    request.limits.maxTopologyNodes = 4u;
    request.limits.standbyLeaseMs = 60000u;
    return request;
}

model::RouteDescriptor completedS2RetirementRoute(const uint16_t port,
                                                  const uint64_t generation) {
    model::RouteDescriptor route{};
    route.kind = model::RouteKind::Direct;
    route.ipv4BE = loopbackIpv4BE();
    route.port = port;
    route.generation = generation;
    route.routeProved = true;
    return route;
}

bool makeCompletedS2RetirementActivation(wire::RelayChildActivation& out) {
    out = {};
    if (!model::makeStreamCompatibilityIdentity(
            compatibilityIdentity(), kSessionId, kEpoch,
            0x4353325245544952ull, out.stream)) {
        return false;
    }
    constexpr model::NodeId kRoot = 0x4353524F4F540001ull;
    constexpr model::NodeId kRelay = 0x4353533152454C31ull;
    constexpr model::NodeId kChild = 0x4353533243484C44ull;
    out.authorityGeneration = 0x4353415554483031ull;
    out.rootNodeId = kRoot;
    out.relayNodeId = kRelay;
    out.childNodeId = kChild;
    out.relayDataRoute = completedS2RetirementRoute(39101u, 71u);
    out.relayControlRoute = completedS2RetirementRoute(39102u, 72u);
    out.childDataRoute = completedS2RetirementRoute(39103u, 73u);
    out.childControlRoute = completedS2RetirementRoute(39104u, 74u);
    out.relayLease.childId = kRelay;
    out.relayLease.parentId = kRoot;
    out.relayLease.parentSourceGeneration = 17u;
    out.relayLease.routeGeneration = out.relayDataRoute.generation;
    out.relayLease.expiryMs = 90000u;
    out.relayLease.replayNonce = 0x435353315245504Cull;
    out.relayLease.macLow = 0x4353533152454C4Cull;
    out.relayLease.macHigh = 0x4353533152454D48ull;
    out.relayLease.childBinding.authorityGeneration = out.authorityGeneration;
    out.relayLease.childBinding.childViewerId = kPhaseCRelayViewerId;
    out.relayLease.childBinding.childViewerGeneration = 1u;
    out.relayLease.childBinding.childQspRoute = out.relayDataRoute;
    out.relayLease.childBinding.childEndpointBindingGeneration = 71u;
    out.relayLease.childBinding.childRequestedCursor = {75u, 7u,
                                                         0x435352454C415931ull};
    out.relayBindingRequestNonce = 0x435352454C415942ull;
    out.childLease.stream = out.stream;
    out.childLease.childId = kChild;
    out.childLease.parentId = kRelay;
    out.childLease.parentDepth = 1u;
    out.childLease.parentSourceGeneration = 17u;
    out.childLease.routeGeneration = out.childDataRoute.generation;
    out.childLease.routes[0] = out.childDataRoute;
    out.childLease.routeCount = 1u;
    out.childLease.cacheRange = {50u, 75u};
    out.childLease.latestVerifiedCursor = 75u;
    out.childLease.childCapacity = 1u;
    out.childLease.expiryMs = 90000u;
    out.childLease.replayNonce = 0x4353533243484C44ull;
    out.childLease.ancestry[0] = kRoot;
    out.childLease.ancestry[1] = kRelay;
    out.childLease.ancestryCount = 2u;
    out.childLease.macLow = 0x435353324C454153ull;
    out.childLease.macHigh = 0x435353324D414321ull;
    out.childLease.childBinding.authorityGeneration = out.authorityGeneration;
    out.childLease.childBinding.childViewerId = kPhaseCChildViewerId;
    out.childLease.childBinding.childViewerGeneration = 1u;
    out.childLease.childBinding.childQspRoute = out.childDataRoute;
    out.childLease.childBinding.childEndpointBindingGeneration = 73u;
    out.childLease.childBinding.childRequestedCursor = {75u, 7u,
                                                         0x43534348494C4431ull};
    out.relayCacheRange = {50u, 75u};
    out.commonCursor = {75u, 7u, 0x4353434F4D4D4F4Eull};
    out.activationNonce = 0x4353414354495645ull;
    out.activationDeadlineMs = 91000u;
    return true;
}

bool makeCompletedClientRootRetirementActivation(
    wire::ClientPlayerRootActivation& out,
    topology::TopologyAuthoritySigner& signer) {
    using qnetplay::config_provenance::Stage;
    out = {};
    if (!signer.ready()
        || !model::makeStreamCompatibilityIdentity(
            compatibilityIdentity(), kSessionId, kEpoch,
            0x4350524F4F544331ull, out.binding.stream)) {
        return false;
    }
    constexpr model::NodeId kHost = 0x4350524F4F544831ull;
    constexpr model::NodeId kClientRoot = 0x4350524F4F544332ull;
    constexpr model::NodeId kTarget = 0x4350524F4F544333ull;
    constexpr uint64_t kAuthority = 0x4350524F4F544341ull;
    const auto provenance = qnetplay::config_provenance::makeCommittedWire(
        Stage::Default, Stage::LocallySelected, Stage::CasterAdvertised,
        Stage::PeerAdvertised);
    out.authorityGeneration = kAuthority;
    out.hostRootNodeId = kHost;
    out.binding.gameStartFingerprint = out.binding.stream.streamRootIdentity;
    out.binding.clientRootNodeId = kClientRoot;
    out.binding.remotePlayerRole = wire::ClientPlayerRootRole::RemotePlayerTwo;
    out.binding.configurationProvenance = provenance;
    out.binding.runtimeReceipt.processId = 4442u;
    out.binding.runtimeReceipt.publisherGeneration = 12u;
    out.binding.runtimeReceipt.publisherNonce = 0x4350524F4F544401ull;
    out.binding.runtimeReceipt.playerSlot = static_cast<uint8_t>(
        wire::ClientPlayerRootRole::RemotePlayerTwo);
    out.binding.runtimeReceipt.runtimeStage = Stage::RuntimeApplied;
    out.binding.runtimeReceipt.delay = 3u;
    out.binding.runtimeReceipt.rollback = 7u;
    out.binding.runtimeReceipt.remoteDelay = 4u;
    out.binding.runtimeReceipt.remoteRollback = 6u;
    out.binding.runtimeReceipt.provenance = provenance;
    out.binding.sourceGeneration = 17u;
    out.binding.qspRouteGeneration = 71u;
    out.binding.qsrBindingGeneration = 72u;
    out.binding.dataRoute = completedS2RetirementRoute(39105u, 71u);
    out.binding.qsrRoute = completedS2RetirementRoute(39106u, 72u);
    out.binding.cacheRange = {50u, 75u};
    out.binding.cursor = {75u, 7u, 0x4350524F4F544402ull};
    out.binding.childCapacity = 1u;
    out.binding.expiresMs = 90000u;

    out.targetViewerNodeId = kTarget;
    out.targetDataRoute = completedS2RetirementRoute(39107u, 73u);
    out.targetQsrRoute = completedS2RetirementRoute(39108u, 73u);
    out.targetLease.stream = out.binding.stream;
    out.targetLease.childId = kTarget;
    out.targetLease.parentId = kClientRoot;
    out.targetLease.childBinding.authorityGeneration = kAuthority;
    out.targetLease.childBinding.childViewerId = kViewerId;
    out.targetLease.childBinding.childViewerGeneration = 1u;
    out.targetLease.childBinding.childQspRoute = out.targetDataRoute;
    out.targetLease.childBinding.childEndpointBindingGeneration = 73u;
    out.targetLease.childBinding.childRequestedCursor = out.binding.cursor;
    out.targetLease.parentDepth = 0u;
    out.targetLease.parentSourceGeneration = out.binding.sourceGeneration;
    out.targetLease.routeGeneration = out.binding.dataRoute.generation;
    out.targetLease.routes[0] = out.binding.dataRoute;
    out.targetLease.routeCount = 1u;
    out.targetLease.cacheRange = out.binding.cacheRange;
    out.targetLease.latestVerifiedCursor = out.binding.cursor.nextRecord;
    out.targetLease.childCapacity = out.binding.childCapacity;
    out.targetLease.expiryMs = out.binding.expiresMs;
    out.targetLease.replayNonce = 0x4350524F4F544403ull;
    out.targetLease.ancestry[0] = kClientRoot;
    out.targetLease.ancestryCount = 1u;
    out.targetLease.macLow = 0x4350524F4F544404ull;
    out.targetLease.macHigh = 0x4350524F4F544405ull;
    out.activationNonce = 0x4350524F4F544406ull;
    out.activationDeadlineMs = out.targetLease.expiryMs;

    std::vector<uint8_t> input;
    return wire::encodeClientPlayerRootActivationAttestationInput(out, input)
        && signer.sign(input.data(), input.size(), out.hostSignature);
}

bool signCompletedClientRootRetirement(
    wire::ClientPlayerRootActivationRetirement& retirement,
    const topology::TopologyAuthoritySigner& signer) {
    retirement.hostSignature.fill(0u);
    std::vector<uint8_t> input;
    return wire::encodeClientPlayerRootActivationRetirementAttestationInput(
               retirement, input)
        && signer.sign(input.data(), input.size(), retirement.hostSignature);
}

bool testCompletedS2RetirementTupleExactness() {
    using Runtime = topology::client::ReleasedTopologyClientRuntime;
    using Access = topology::client::testing::ReleasedTopologyClientRuntimeTestAccess;

    wire::RelayChildActivation activation{};
    if (!makeCompletedS2RetirementActivation(activation)) {
        fail("completed-s2-retirement", "could not construct exact activation fixture");
        return false;
    }
    Runtime runtime{};
    Access::installCompletedS2Activation(runtime, activation);
    wire::RelayChildActivationRetirement exact{};
    if (!Access::completedS2Retirement(runtime, exact)
        || !Access::hasCompletedS2Activation(runtime)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Attached
        || !runtime.active()) {
        fail("completed-s2-retirement",
             "completed S2 activation was not retained for Type-15 comparison");
        return false;
    }

    wire::RelayChildActivationRetirement wrongRoute = exact;
    ++wrongRoute.childDataRoute.port;
    if (!Access::acceptRetirement(runtime, wrongRoute, 100u)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Attached
        || !runtime.active() || !Access::hasCompletedS2Activation(runtime)) {
        fail("completed-s2-retirement",
             "wrong completed-S2 route retirement was not inert");
        return false;
    }
    wire::RelayChildActivationRetirement staleNonce = exact;
    ++staleNonce.activationNonce;
    if (!Access::acceptRetirement(runtime, staleNonce, 101u)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Attached
        || !runtime.active() || !Access::hasCompletedS2Activation(runtime)) {
        fail("completed-s2-retirement",
             "stale completed-S2 activation retirement was not inert");
        return false;
    }
    wire::RelayChildActivationRetirement wrongLease = exact;
    ++wrongLease.relayLease.replayNonce;
    if (!Access::acceptRetirement(runtime, wrongLease, 102u)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Attached
        || !runtime.active() || !Access::hasCompletedS2Activation(runtime)) {
        fail("completed-s2-retirement",
             "wrong completed-S2 lease retirement was not inert");
        return false;
    }

    exact.reason = wire::RelayChildRetirementReason::ChildBindingReplaced;
    if (!Access::acceptRetirement(runtime, exact, 103u)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Unavailable
        || runtime.active()
        || std::strcmp(runtime.status().reason,
                       "topology-parent-route-activation-retired") != 0
        || Access::hasCompletedS2Activation(runtime)) {
        fail("completed-s2-retirement",
             "exact completed-S2 Type-15 did not retire topology fail-soft");
        return false;
    }
    if (!Access::acceptRetirement(runtime, exact, 104u)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Unavailable
        || std::strcmp(runtime.status().reason,
                       "topology-parent-route-activation-retired") != 0) {
        fail("completed-s2-retirement",
             "duplicate completed-S2 Type-15 changed terminal topology state");
        return false;
    }
    return true;
}

bool testCompletedClientRootRetirementTupleExactness() {
    using Runtime = topology::client::ReleasedTopologyClientRuntime;
    using Access = topology::client::testing::ReleasedTopologyClientRuntimeTestAccess;

    topology::TopologyAuthoritySigner signer{};
    topology::TopologyAuthorityPublicBlob publicKey{};
    wire::ClientPlayerRootActivation activation{};
    if (!signer.generate() || !signer.publicBlob(publicKey)
        || !makeCompletedClientRootRetirementActivation(activation, signer)) {
        fail("completed-client-root-retirement",
             "could not construct host-signed client-root activation fixture");
        return false;
    }
    Runtime runtime{};
    if (!Access::installCompletedClientRootActivation(runtime, activation,
                                                       publicKey)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Attached
        || !runtime.active()
        || !Access::hasCompletedClientRootActivation(runtime)) {
        fail("completed-client-root-retirement",
             "completed client-root activation was not retained");
        return false;
    }

    wire::ClientPlayerRootActivationRetirement exact{};
    exact.activation = activation;
    exact.reason = wire::ClientPlayerRootRetirementReason::TargetViewerDetached;
    if (!signCompletedClientRootRetirement(exact, signer)) {
        fail("completed-client-root-retirement",
             "could not sign exact client-root retirement fixture");
        return false;
    }

    wire::ClientPlayerRootActivationRetirement wrongRoute = exact;
    wrongRoute.activation.targetDataRoute.port += 2u;
    wrongRoute.activation.targetLease.childBinding.childQspRoute.port += 2u;
    if (!signCompletedClientRootRetirement(wrongRoute, signer)
        || !Access::acceptClientRootRetirement(runtime, wrongRoute, 100u)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Attached
        || !runtime.active()
        || !Access::hasCompletedClientRootActivation(runtime)) {
        fail("completed-client-root-retirement",
             "wrong host-signed client-root route retirement was not inert");
        return false;
    }
    wire::ClientPlayerRootActivationRetirement wrongNonce = exact;
    ++wrongNonce.activation.activationNonce;
    if (!signCompletedClientRootRetirement(wrongNonce, signer)
        || !Access::acceptClientRootRetirement(runtime, wrongNonce, 101u)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Attached
        || !runtime.active()
        || !Access::hasCompletedClientRootActivation(runtime)) {
        fail("completed-client-root-retirement",
             "wrong host-signed client-root nonce retirement was not inert");
        return false;
    }
    if (!Access::acceptClientRootRetirement(runtime, exact, 103u)
        || runtime.status().state
            != topology::client::ReleasedTopologyClientState::Unavailable
        || runtime.active() || Access::hasCompletedClientRootActivation(runtime)
        || std::strcmp(runtime.status().reason,
                       "topology-client-player-root-activation-retired") != 0) {
        fail("completed-client-root-retirement",
             "exact host-signed client-root retirement did not fail soft");
        return false;
    }
    return true;
}

uint32_t phaseCInitialRunwayRecords() {
    const size_t required = model::ResilientLimits{}.requiredCacheRecords();
    if (required == 0u
        || required >= static_cast<size_t>(
            (std::numeric_limits<uint32_t>::max)())) {
        return 0u;
    }
    return static_cast<uint32_t>(required);
}

bool publishStart(SpectatorSidecar& sidecar, uint64_t& sequence,
                  const uint64_t now) {
    PublicationEvent event{};
    event.kind = PublicationKind::Start;
    event.sequence = sequence++;
    event.descriptor = gameDescriptor();
    sidecar.publish(event, now);
    return sidecar.phase() == SidecarPhase::Active;
}

bool publishVerifiedRecord(SpectatorSidecar& sidecar, uint64_t& sequence,
                           const uint64_t now) {
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
    return true;
}

model::VerifiedChainCursor verifiedCursorAfterFirstRecord() {
    const qsp::GameDescriptor descriptor = gameDescriptor();
    qsp::ConfirmedRecord record = recordFor(0u);
    record.rollingDigest = qsp::advanceRollingDigest(
        descriptor.initialDigest, descriptor.gameId, record);
    return model::VerifiedChainCursor{1u, 0u, record.rollingDigest};
}

model::ParentLease manualParentLease(
    const topology::TopologyAuthority& authority,
    const topology::TopologyEnrollment& enrollment, const uint64_t nonce,
    const uint64_t expiryMs, const model::VerifiedChainCursor& cursor) {
    model::ParentLease lease{};
    const topology::TopologyAuthorityFacts& facts = authority.facts();
    (void)model::makeStreamCompatibilityIdentity(
        facts.compatibility, facts.session.sessionId, facts.session.epoch,
        facts.streamRootIdentity, lease.stream);
    lease.childId = enrollment.nodeId;
    lease.parentId = facts.rootNodeId;
    (void)authority.parentLeaseChildBinding(enrollment.nodeId,
                                            lease.childBinding);
    lease.parentDepth = 0u;
    lease.parentSourceGeneration = facts.sourceGeneration;
    lease.routeGeneration = facts.rootQsp.generation;
    lease.routes[0] = facts.rootQsp.advertised;
    lease.routeCount = 1u;
    lease.cacheRange = model::VerifiedCacheRange{
        lease.childBinding.childRequestedCursor.nextRecord,
        cursor.nextRecord};
    lease.latestVerifiedCursor = cursor.nextRecord;
    lease.childCapacity = 1u;
    lease.expiryMs = expiryMs;
    lease.replayNonce = nonce;
    lease.ancestry[0] = facts.rootNodeId;
    lease.ancestryCount = 1u;
    lease.macLow = 0x5A746F706F4C6561ull ^ nonce;
    lease.macHigh = 0x7365526563656970ull ^ expiryMs;
    return lease;
}

wire::ParentCommit manualParentCommit(
    const model::ParentLease& lease,
    const model::VerifiedChainCursor& cursor) {
    wire::ParentCommit commit{};
    commit.stream = lease.stream;
    commit.childId = lease.childId;
    commit.parentId = lease.parentId;
    commit.lease = model::LeaseBinding{
        lease.childId, lease.parentId, lease.parentSourceGeneration,
        lease.routeGeneration, lease.expiryMs, lease.replayNonce,
        lease.macLow, lease.macHigh, lease.childBinding};
    commit.sourceGeneration = lease.parentSourceGeneration;
    commit.routeGeneration = lease.routeGeneration;
    commit.exactNextCursor = cursor.nextRecord;
    commit.exactNextEvent = cursor.nextEvent;
    commit.overlapCursor = cursor.nextRecord;
    commit.overlapEvent = cursor.nextEvent;
    commit.overlapDigest = cursor.digest;
    return commit;
}

bool sendManualQtaPayload(qnet::UdpSock& socket,
                          const topology::TopologyAuthority& authority,
                          const topology::TopologyEnrollment& enrollment,
                          const RouteEndpoint& destination,
                          const uint64_t qtaSequence,
                          const uint8_t* const payload,
                          const size_t payloadLength,
                          const bool corruptMac = false) {
    if (payload == nullptr || payloadLength == 0u || !destination.valid())
        return false;
    std::vector<uint8_t> wrapped;
    if (!authority.encodeControl(enrollment, qtaSequence, payload,
                                 payloadLength, wrapped)) {
        return false;
    }
    constexpr size_t kTopologyQtaMacOffset = 36u;
    if (corruptMac) {
        if (wrapped.size() <= kTopologyQtaMacOffset) return false;
        wrapped[kTopologyQtaMacOffset] ^= 0x80u;
    }
    qnet::Endpoint endpoint{};
    endpoint.sa.sin_addr.s_addr = destination.ipv4BE;
    endpoint.sa.sin_port = htons(destination.port);
    return socket.sendTo(endpoint, wrapped.data(), wrapped.size());
}

bool sendManualQtaDatagram(qnet::UdpSock& socket,
                           const topology::TopologyAuthority& authority,
                           const topology::TopologyEnrollment& enrollment,
                           const RouteEndpoint& destination,
                           const uint64_t qtaSequence,
                           const wire::ControlDatagram& datagram,
                           const bool corruptMac = false) {
    std::vector<uint8_t> payload;
    return wire::encodeControlDatagram(datagram, payload)
        && sendManualQtaPayload(socket, authority, enrollment, destination,
                                qtaSequence, payload.data(), payload.size(),
                                corruptMac);
}

qnet::SpectatorGrantWire grantToWire(const qsp::SpectatorGrant& grant) {
    qnet::SpectatorGrantWire wire{};
    wire.grantId = grant.grantId;
    wire.viewerId = grant.viewerId;
    wire.sessionId = grant.sessionId;
    wire.expiryUnix = grant.expiryUnix;
    wire.sourceGeneration = grant.sourceGeneration;
    wire.resumeRecord = grant.resumeRecord;
    wire.resumeDigest = grant.resumeDigest;
    wire.route = grant.route;
    wire.macLow = grant.macLow;
    wire.macHigh = grant.macHigh;
    return wire;
}

bool grantFromWire(const qnet::SpectatorGrantWire& wire,
                   qsp::SpectatorGrant& grant) {
    for (const uint8_t byte : wire.reserved)
        if (byte != 0u) return false;
    if (wire.grantId == 0u || wire.viewerId != kViewerId
        || wire.sessionId != kSessionId || wire.expiryUnix == 0u
        || wire.sourceGeneration == 0u
        || (wire.route != qnet::SPECTATE_ROUTE_DIRECT
            && wire.route != qnet::SPECTATE_ROUTE_PUNCHED)
        || (wire.macLow == 0u && wire.macHigh == 0u)) {
        return false;
    }
    grant = {};
    grant.grantId = wire.grantId;
    grant.viewerId = wire.viewerId;
    grant.sessionId = wire.sessionId;
    grant.expiryUnix = wire.expiryUnix;
    grant.sourceGeneration = wire.sourceGeneration;
    grant.resumeRecord = wire.resumeRecord;
    grant.resumeDigest = wire.resumeDigest;
    grant.route = wire.route;
    grant.macLow = wire.macLow;
    grant.macHigh = wire.macHigh;
    return true;
}

qnet::SpectateInfo makeLiveInfo(const uint16_t qspPort,
                                 const qsp::SpectatorGrant& grant) {
    qnet::SpectateInfo info{};
    const qsp::SessionIdentity identity = sessionIdentity();
    info.state = qnet::SPECTATE_LIVE;
    info.spectatorPort = qspPort;
    info.sessionId = static_cast<uint32_t>(identity.sessionId);
    info.keyLow = identity.cookie.low;
    info.keyHigh = identity.cookie.high;
    info.sourceRoutes.sessionId = info.sessionId;
    info.sourceRoutes.count = 1u;
    info.sourceRoutes.entries[0].kind = 1u;
    info.sourceRoutes.entries[0].routeId = 1u;
    info.sourceRoutes.entries[0].ipv4BE = loopbackIpv4BE();
    info.sourceRoutes.entries[0].portBE = htons(qspPort);
    info.admissionState = qnet::SPECTATE_ADMISSION_GRANTED;
    info.admissionPolicy = static_cast<uint8_t>(AdmissionPolicy::Public);
    info.viewerCount = 0u;
    info.viewerCapacity = 2u;
    info.simulationContract = kSimulationContract;
    info.grant = grantToWire(grant);
    info.compatibilityIdentity = compatibilityIdentity();
    return info;
}

qnet::SpectateInfo makeLiveInfoForViewer(const uint16_t qspPort,
                                          const uint64_t viewerId,
                                          const qsp::SpectatorGrant& grant) {
    qnet::SpectateInfo info{};
    const qsp::SessionIdentity identity = sessionIdentity();
    info.state = qnet::SPECTATE_LIVE;
    info.spectatorPort = qspPort;
    info.sessionId = static_cast<uint32_t>(identity.sessionId);
    info.keyLow = identity.cookie.low;
    info.keyHigh = identity.cookie.high;
    info.sourceRoutes.sessionId = info.sessionId;
    info.sourceRoutes.count = 1u;
    info.sourceRoutes.entries[0].kind = 1u;
    info.sourceRoutes.entries[0].routeId = 1u;
    info.sourceRoutes.entries[0].ipv4BE = loopbackIpv4BE();
    info.sourceRoutes.entries[0].portBE = htons(qspPort);
    info.admissionState = qnet::SPECTATE_ADMISSION_GRANTED;
    info.admissionPolicy = static_cast<uint8_t>(AdmissionPolicy::Public);
    info.viewerCount = 0u;
    info.viewerCapacity = 2u;
    info.simulationContract = kSimulationContract;
    info.grant = grantToWire(grant);
    info.compatibilityIdentity = compatibilityIdentity();
    (void)viewerId;
    return info;
}

bool validIntent(const qnet::ClientIntent& intent,
                 const uint16_t capabilities,
                 const uint32_t expectedResumeRecord,
                 const uint8_t expectedRoute = qnet::SPECTATE_ROUTE_DIRECT) {
    const qsp::SessionIdentity identity = sessionIdentity();
    return intent.intent == qnet::INTENT_SPECTATE
        && intent.spectatorRoute == expectedRoute
        && intent.spectatorCapabilities == capabilities
        && intent.sessionId == identity.sessionId
        && intent.keyLow == identity.cookie.low
        && intent.keyHigh == identity.cookie.high
        && intent.viewerId == kViewerId
        && intent.resumeRecord == expectedResumeRecord
        && intent.reserved3 == 0u && intent.resumeDigest == 0u;
}

bool waitForInitialInfo(const qnet::Endpoint& host,
                        qnet::SpectateInfo& out, std::string& error) {
    out = {};
    qnet::UdpSock socket;
    if (!socket.open(0u)) {
        error = "could not open ordinary E0 control socket";
        return false;
    }
    qnet::UdpControlLink link;
    link.startClient(socket, host, nowMs());
    const uint64_t started = nowMs();
    bool sentVersion = false;
    while (!elapsed(started, 6000u)) {
        link.poll(0);
        if (!link.isOpen()) {
            error = "ordinary E0 control link closed";
            return false;
        }
        for (unsigned budget = 0u; budget < 8u; ++budget) {
            qnet::ControlLink::Received rx{};
            const qnet::ControlLink::Rs result = link.recvMsgEx(rx, 0);
            if (result == qnet::ControlLink::Rs::Timeout) break;
            if (result != qnet::ControlLink::Rs::Msg) {
                error = "ordinary E0 control receive failed";
                return false;
            }
            if (!sentVersion) {
                qnet::VersionConfig version{};
                if (rx.type != qnet::MSG_VERSION_CONFIG
                    || rx.body.size() != sizeof(version)) {
                    error = "ordinary E0 host version was malformed";
                    return false;
                }
                std::memcpy(&version, rx.body.data(), sizeof(version));
                if (version.protocolVersion != qnet::kProtocolVersion) {
                    error = "ordinary E0 host version mismatched";
                    return false;
                }
                qnet::VersionConfig local{qnet::kProtocolVersion};
                qnet::ClientIntent intent{};
                intent.intent = qnet::INTENT_SPECTATE;
                intent.spectatorRoute = qnet::SPECTATE_ROUTE_DIRECT;
                intent.sessionId = static_cast<uint32_t>(kSessionId);
                intent.keyLow = sessionIdentity().cookie.low;
                intent.keyHigh = sessionIdentity().cookie.high;
                intent.viewerId = kViewerId;
                intent.resumeRecord = 0u;
                if (!link.sendMsg(qnet::MSG_VERSION_CONFIG, &local,
                                  sizeof(local))
                    || !link.sendMsg(qnet::MSG_CLIENT_INTENT, &intent,
                                     sizeof(intent))) {
                    error = "ordinary E0 control could not send intent";
                    return false;
                }
                sentVersion = true;
                continue;
            }
            if (rx.type != qnet::MSG_SPECTATE_INFO
                || rx.body.size() != sizeof(out)) {
                error = "ordinary E0 response was not SpectateInfo";
                return false;
            }
            std::memcpy(&out, rx.body.data(), sizeof(out));
            if (out.state != qnet::SPECTATE_LIVE
                || out.sessionId != kSessionId
                || out.spectatorPort == 0u
                || out.keyLow != sessionIdentity().cookie.low
                || out.keyHigh != sessionIdentity().cookie.high
                || out.admissionState != qnet::SPECTATE_ADMISSION_GRANTED
                || !qnetplay::compat::semanticallyEqual(
                       out.compatibilityIdentity, compatibilityIdentity())) {
                error = "ordinary E0 response did not carry exact live facts";
                return false;
            }
            link.close();
            return true;
        }
        Sleep(1u);
    }
    error = "ordinary E0 control response timed out";
    return false;
}

bool qspIngressAccepted(const qsp::IngressResult result) {
    return result == qsp::IngressResult::Accepted
        || result == qsp::IngressResult::ReplayedSequence;
}

int runChild(const char* const qspPortText, const char* const controlPortText,
             const bool awaitAuthorityRevocation,
             const bool awaitAuthorityLivenessTimeout,
             const bool authorityStateValidation,
             const uint32_t b10Case,
             const bool relayDataBindingValidation) {
    uint16_t qspPort = 0u;
    uint16_t controlPort = 0u;
    if (!parsePort(qspPortText, qspPort)
        || !parsePort(controlPortText, controlPort)) {
        return 2;
    }
    NetScope network;
    if (!network.start()) return 3;

    const qnet::Endpoint rootQsp = loopbackEndpoint(qspPort);
    const qnet::Endpoint hostControl = loopbackEndpoint(controlPort);
    qnet::SpectateInfo e0{};
    std::string error;
    if (!waitForInitialInfo(hostControl, e0, error)) {
        fail("child", error);
        return 4;
    }
    qsp::SpectatorGrant grant{};
    if (!grantFromWire(e0.grant, grant)) {
        fail("child", "ordinary E0 grant was not an exact supported viewer grant");
        return 5;
    }

    qnet::UdpSock qspSocket;
    if (!qspSocket.open(0u) || qspSocket.boundPort() == 0u) {
        fail("child", "could not open ordinary QSP viewer socket");
        return 6;
    }
    qsp::ViewerConfig viewerConfig{};
    viewerConfig.identity = sessionIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.secureRandom = kViewerId;
    viewerConfig.viewerNonce = kViewerId;
    qsp::SpectatorViewer viewer(viewerConfig);
    if (!viewer.isConfigured() || !viewer.begin(kGameId)) {
        fail("child", "SpectatorViewer fixture was not configured/begun");
        return 7;
    }

    topology::client::ReleasedTopologyClientRuntime runtime;
    bool initAckSent = false;
    bool runtimeStarted = false;
    bool detachArmed = false;
    bool detachSent = false;
    const bool expectTopologyFailure = b10ExpectsTopologyFailure(b10Case);
    const B10FailureTiming expectedB10FailureTiming = b10FailureTiming(b10Case);
    const char* const expectedB10FailureReason =
        b10ExpectedFailureReason(b10Case);
    bool expectedTopologyFailureObserved = false;
    bool topologyAttachmentObserved = false;
    bool attachmentSnapshotObserved = false;
    bool authorityStatePauseArmed = false;
    uint64_t authorityStatePauseAt = 0u;
    uint64_t authorityStatePauseUntil = 0u;
    bool relayDataBindingStarted = false;
    bool relayDataBindingReady = false;
    bool relayDataBindingLifecycleClosed = false;
    uint16_t relayDataPort = 0u;
    SOCKET relayDataSocket = INVALID_SOCKET;
    uint64_t relayDataBorrowGeneration = 0u;
    const uint64_t started = nowMs();
    std::array<uint8_t, 4096u> inbound{};
    while (!elapsed(started, kPumpTimeoutMs)) {
        const uint64_t now = nowMs();
        for (unsigned budget = 0u; budget < 16u; ++budget) {
            qnet::Endpoint sender{};
            const int received = qspSocket.recvFrom(inbound.data(), inbound.size(),
                                                     sender);
            if (received == 0) break;
            if (received < 0) {
                fail("child", "ordinary QSP receive failed");
                return 8;
            }
            if (!(sender == rootQsp)) continue;
            if (!qspIngressAccepted(viewer.onDatagram(
                    inbound.data(), static_cast<size_t>(received), now))) {
                fail("child", "ordinary QSP ingress was rejected");
                return 9;
            }
        }

        for (const qsp::ViewerDatagram& packet : viewer.pollOutbound(now, 4u)) {
            if (!qspSocket.sendTo(rootQsp, packet.bytes.data(),
                                  packet.bytes.size())) {
                fail("child", "ordinary QSP send failed");
                return 10;
            }
            if (packet.type == qsp::MessageType::InitAck) {
                if (initAckSent || runtimeStarted) {
                    fail("child", "ordinary QSP emitted duplicate InitAck");
                    return 11;
                }
                initAckSent = true;
                topology::client::ReleasedTopologyClientConfig config{};
                config.enabled = true;
                config.hostControl = hostControl;
                config.qspEndpoint = RouteEndpoint{
                    loopbackIpv4BE(), qspSocket.boundPort()};
                config.rootQspEndpoint = RouteEndpoint{
                    loopbackIpv4BE(), qspPort};
                config.qspRoute = grant.route;
                config.qspProvenance = grant.route
                        == qnet::SPECTATE_ROUTE_PUNCHED
                    ? topology::EndpointProvenance::ProvenPunchedQsp
                    : topology::EndpointProvenance::ProvenDirectQsp;
                config.qspRouteGeneration = viewer.generation();
                config.viewerId = viewer.viewerId();
                config.viewerGeneration = viewer.generation();
                config.sessionId = kSessionId;
                config.sessionEpoch = kEpoch;
                config.spectatorCookieLow = e0.keyLow;
                config.spectatorCookieHigh = e0.keyHigh;
                config.compatibility = e0.compatibilityIdentity;
                config.streamRootIdentity = qsp::gameStartFingerprint(
                    qsp::canonicalGameStart(gameDescriptor()));
                config.grant = grant;
                if (!runtime.start(config, now)
                    || !runtime.onQspInitAckTransmitted(now)
                    || !runtime.updateVerifiedQspFrontier(
                        viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                        viewer.receivedDigest())) {
                    fail("child", "released topology runtime refused a real InitAck");
                    return 12;
                }
                runtimeStarted = true;
            }
            if (packet.type == qsp::MessageType::Detach) detachSent = true;
        }

        if (runtimeStarted
            && runtime.status().state
                != topology::client::ReleasedTopologyClientState::Unavailable) {
            if (!runtime.updateVerifiedQspFrontier(
                    viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                    viewer.receivedDigest())) {
                fail("child", "released topology runtime rejected QSP frontier");
                return 13;
            }
            bool pauseTopologyService = false;
            if (authorityStateValidation && runtime.attached()) {
                if (!authorityStatePauseArmed) {
                    authorityStatePauseArmed = true;
                    authorityStatePauseAt = now + 600u;
                    authorityStatePauseUntil = now + 2200u;
                }
                pauseTopologyService = now >= authorityStatePauseAt
                    && now < authorityStatePauseUntil;
            }
            if (!pauseTopologyService) runtime.service(now);
        }
        const topology::client::ReleasedTopologyClientStatus status =
            runtime.status();
        if (runtime.attached()) {
            topologyAttachmentObserved = true;
            topology::client::ReleasedTopologyAttachmentSnapshot snapshot{};
            if (!runtime.attachmentSnapshot(snapshot) || !snapshot.valid()
                || snapshot.childBinding.childQspRoute.ipv4BE
                    != loopbackIpv4BE()
                || snapshot.childBinding.childQspRoute.port
                    != qspSocket.boundPort()
                || snapshot.parentDataRoute.ipv4BE != loopbackIpv4BE()
                || snapshot.parentDataRoute.port != qspPort
                || snapshot.committedCursor.nextRecord
                    != viewer.receivedNextRecord()
                || snapshot.committedCursor.nextEvent
                    != viewer.receivedNextEvent()
                || snapshot.committedCursor.digest != viewer.receivedDigest()
                || snapshot.credentialDeadlineMs <= now) {
                fail("child", "post-commit attachment snapshot was not exact");
                return 23;
            }
            attachmentSnapshotObserved = true;
            if (relayDataBindingValidation && !relayDataBindingStarted) {
                if (runtime.relayDataBindingState()
                        != topology::client::ReleasedTopologyRelayBindingState::Inactive) {
                    fail("child", "relay data binding was not inactive before exact handoff");
                    return 24;
                }
                qnet::UdpSock relayDataOwner;
                if (!relayDataOwner.open(0u)
                    || relayDataOwner.boundPort() == 0u) {
                    fail("child", "could not open exact relay-data fixture socket");
                    return 24;
                }
                relayDataPort = relayDataOwner.boundPort();
                relayDataSocket = relayDataOwner.release();
                uintptr_t ownedRelayDataSocket =
                    static_cast<uintptr_t>(relayDataSocket);
                const topology::client::ReleasedTopologyRelaySocketBorrow
                    pendingBorrow = runtime.borrowedRelayDataSocket();
                if (relayDataSocket == INVALID_SOCKET
                    || !runtime.beginRelayDataBinding(ownedRelayDataSocket, now)
                    || ownedRelayDataSocket
                        != topology::client::kInvalidReleasedTopologyRelaySocket
                    || runtime.relayDataBindingState()
                        != topology::client::ReleasedTopologyRelayBindingState::Pending
                    || pendingBorrow.valid()) {
                    fail("child", "client runtime did not retain the exact relay-data socket");
                    return 24;
                }
                const topology::client::ReleasedTopologyRelaySocketBorrow
                    adoptedBorrow = runtime.borrowedRelayDataSocket();
                if (adoptedBorrow.valid()
                    || adoptedBorrow.socket
                        != topology::client::kInvalidReleasedTopologyRelaySocket
                    || adoptedBorrow.generation == 0u) {
                    fail("child", "relay socket adoption did not invalidate the prior borrow generation");
                    return 24;
                }
                relayDataBorrowGeneration = adoptedBorrow.generation;
                relayDataBindingStarted = true;
            }
            if (relayDataBindingValidation && relayDataBindingStarted
                && !relayDataBindingReady) {
                const auto bindingState = runtime.relayDataBindingState();
                if (bindingState
                    == topology::client::ReleasedTopologyRelayBindingState::Ready) {
                    topology::client::ReleasedTopologyRelayBindingSnapshot binding{};
                    sockaddr_in actual{};
                    int actualLength = sizeof(actual);
                    const topology::client::ReleasedTopologyRelaySocketBorrow
                        readyBorrow = runtime.borrowedRelayDataSocket();
                    if (!runtime.relayDataBindingSnapshot(binding)
                        || !binding.valid()
                        || binding.observedDataRoute.ipv4BE != loopbackIpv4BE()
                        || binding.observedDataRoute.port != relayDataPort
                        || binding.observedControlRoute.sameEndpoint(
                            binding.observedDataRoute)
                        || !readyBorrow.valid()
                        || readyBorrow.socket
                            != static_cast<uintptr_t>(relayDataSocket)
                        || readyBorrow.generation != relayDataBorrowGeneration
                        || getsockname(relayDataSocket,
                                       reinterpret_cast<sockaddr*>(&actual),
                                       &actualLength) != 0
                        || ntohs(actual.sin_port) != relayDataPort) {
                        fail("child", "relay binding did not preserve the host-observed socket route");
                        return 24;
                    }
                    if (!runtime.startRelayNode(viewer, now)
                        || !runtime.relayNodeRunning()
                        || runtime.projectRelayViewer(viewer, now)
                            != qclient::spectator::resilient::NodeRuntimeResult::Accepted) {
                        fail("child", "canonical relay facade did not start from the locked viewer");
                        return 24;
                    }
                    const uint8_t unassignedChildByte = 0x5Au;
                    const RouteEndpoint unassignedChild{
                        loopbackIpv4BE(), static_cast<uint16_t>(
                            relayDataPort == 65535u ? 1u : relayDataPort + 1u)};
                    if (runtime.ingestRelayChildDatagram(
                            unassignedChild, 0xC10D0001u,
                            &unassignedChildByte, 1u, now)
                            != qclient::spectator::resilient::
                                NodeRuntimeResult::ChildRejected
                        || !runtime.pollRelayChildOutbound(now).empty()) {
                        fail("child", "relay facade admitted a child without private QTA assignment");
                        return 24;
                    }
                    runtime.stopRelayNode();
                    const topology::client::ReleasedTopologyRelaySocketBorrow
                        retiredBorrow = runtime.borrowedRelayDataSocket();
                    if (runtime.relayDataBindingState()
                            != topology::client::ReleasedTopologyRelayBindingState::Unavailable
                        || retiredBorrow.valid()
                        || retiredBorrow.socket
                            != topology::client::kInvalidReleasedTopologyRelaySocket
                        || retiredBorrow.generation <= readyBorrow.generation
                        || runtime.relayNodeRunning()
                        || !runtime.attached()
                        || getsockname(relayDataSocket,
                                       reinterpret_cast<sockaddr*>(&actual),
                                       &actualLength) != SOCKET_ERROR
                        || WSAGetLastError() != WSAENOTSOCK) {
                        fail("child", "relay stop did not close the client-owned socket exactly once");
                        return 24;
                    }
                    runtime.stopRelayNode();
                    const topology::client::ReleasedTopologyRelaySocketBorrow
                        retiredBorrowAgain = runtime.borrowedRelayDataSocket();
                    if (runtime.relayDataBindingState()
                            != topology::client::ReleasedTopologyRelayBindingState::Unavailable
                        || retiredBorrowAgain.valid()
                        || retiredBorrowAgain.socket
                            != topology::client::kInvalidReleasedTopologyRelaySocket
                        || retiredBorrowAgain.generation
                            != retiredBorrow.generation
                        || runtime.relayNodeRunning()) {
                        fail("child", "relay stop was not idempotent");
                        return 24;
                    }
                    relayDataBindingReady = true;
                    relayDataBindingLifecycleClosed = true;
                } else if (bindingState
                           != topology::client::ReleasedTopologyRelayBindingState::Pending) {
                    fail("child", "relay binding left pending state before an exact QSR acknowledgement");
                    return 24;
                }
            }
            if (expectedB10FailureTiming
                == B10FailureTiming::BeforeAttachment) {
                fail("child", "B10 negative unexpectedly attached topology");
                return 17;
            }
        }
        if (authorityStateValidation && authorityStatePauseArmed
            && now >= authorityStatePauseUntil
            && status.state
                != topology::client::ReleasedTopologyClientState::Unavailable) {
            fail("child", "authority-state filtering extended the liveness deadline");
            return 22;
        }
        if (status.state == topology::client::ReleasedTopologyClientState::Unavailable) {
            if (authorityStateValidation
                && (!authorityStatePauseArmed
                    || now < authorityStatePauseUntil)) {
                fail("child", "authority-state negative retired topology before the queued-heartbeat deadline check");
                return 21;
            }
            const char* const expectedReason = awaitAuthorityRevocation
                ? "topology-authority-revoked"
                : (authorityStateValidation || awaitAuthorityLivenessTimeout
                       ? "topology-authority-liveness-timed-out" : nullptr);
            if ((expectedReason != nullptr || expectTopologyFailure)
                && (expectedReason != nullptr
                    ? std::strcmp(status.reason, expectedReason) == 0
                    : expectedB10FailureReason != nullptr
                        && std::strcmp(status.reason,
                                       expectedB10FailureReason) == 0)
                && (expectedB10FailureTiming
                        != B10FailureTiming::AfterAttachment
                    || (topologyAttachmentObserved
                        && attachmentSnapshotObserved))
                && (!authorityStateValidation
                    || (authorityStatePauseArmed
                        && now >= authorityStatePauseUntil))
                && viewer.phase() == qsp::ViewerPhase::Streaming
                && viewer.receivedNextRecord() >= 1u) {
                return 0;
            }
            if ((expectedReason != nullptr || expectTopologyFailure)) {
                if (expectedReason == nullptr
                    && expectedB10FailureReason == nullptr) {
                    fail("child", "B10 negative has no exact failure assertion");
                    return 19;
                }
                if (expectedB10FailureTiming
                        == B10FailureTiming::AfterAttachment
                    && !topologyAttachmentObserved) {
                    fail("child", "B10 post-attachment negative failed before attach");
                    return 18;
                }
                if ((expectedReason != nullptr
                        && std::strcmp(status.reason, expectedReason) != 0)
                    || (expectedReason == nullptr
                        && std::strcmp(status.reason,
                                       expectedB10FailureReason) != 0)) {
                    fail("child", std::string("unexpected B10 topology failure: ")
                        + status.reason);
                    return 20;
                }
                expectedTopologyFailureObserved = true;
            } else {
                fail("child", std::string("released topology runtime failed softly: ")
                    + status.reason);
                return 14;
            }
        }
        if (expectedTopologyFailureObserved
            && viewer.phase() == qsp::ViewerPhase::Streaming
            && viewer.receivedNextRecord() >= 1u
            && (!authorityStateValidation
                || (authorityStatePauseArmed
                    && now >= authorityStatePauseUntil))) {
            return 0;
        }
        if (!awaitAuthorityRevocation && !awaitAuthorityLivenessTimeout
            && !authorityStateValidation
            && !expectTopologyFailure
            && runtime.attached()
            && attachmentSnapshotObserved
            && (!relayDataBindingValidation
                || relayDataBindingLifecycleClosed)
            && viewer.receivedNextRecord() >= 1u
            && !detachArmed) {
            if (!viewer.detach()) {
                fail("child", "ordinary QSP viewer refused detach after attach");
                return 15;
            }
            detachArmed = true;
        }
        if (detachSent) {
            if (relayDataBindingValidation
                && (!relayDataBindingStarted || !relayDataBindingReady
                    || !relayDataBindingLifecycleClosed)) {
                fail("child", "ordinary detach raced relay binding lifecycle coverage");
                return 24;
            }
            runtime.stop();
            return 0;
        }
        Sleep(1u);
    }
    fail("child", "timed out before released control/QSP/QSR completion");
    return 16;
}

bool readLinkMessages(qnet::UdpControlLink* const link,
                      std::vector<qnet::ControlLink::Received>& out,
                      std::string& error) {
    if (link == nullptr) return true;
    for (unsigned budget = 0u; budget < 8u; ++budget) {
        qnet::ControlLink::Received rx{};
        const qnet::ControlLink::Rs result = link->recvMsgEx(rx, 0);
        if (result == qnet::ControlLink::Rs::Timeout) return true;
        if (result != qnet::ControlLink::Rs::Msg) {
            error = "host control link receive failed";
            return false;
        }
        out.push_back(std::move(rx));
    }
    return true;
}

struct TopologyAttempt {
    qnet::UdpControlLink* link = nullptr;
    bool versionSeen = false;
    bool intentSeen = false;
    bool challengeIssued = false;
    bool proofReceived = false;
    qnet::SpectateTopologyProofV2 proof{};
    qnet::SpectateTopologyChallengeV2 challenge{};
    qsp::SpectatorGrant proofGrant{};
    topology::TopologyViewerClaim claim{};
};

bool runParent(const bool awaitAuthorityRevocation,
               const bool awaitAuthorityLivenessTimeout,
               const uint32_t b10Case = 0u,
               const bool authorityStateValidation = false,
               const bool relayDataBindingValidation = false) {
    NetScope network;
    if (!network.start()) {
        fail("parent", "netInit failed");
        return false;
    }

    uint64_t clock = kInitialNowMs;
    uint64_t sequence = 1u;
    const bool b10FailureScenario = b10ExpectsTopologyFailure(b10Case);
    const B10FailureTiming expectedB10FailureTiming = b10FailureTiming(b10Case);
    const bool manualAuthorityScenario = b10Case == 45u
        || authorityStateValidation;
    SpectatorSidecar sidecar(sidecarConfig());
    std::string error;
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

    qnet::UdpSock b10ManualQsr;
    topology::TopologyAuthority b10ManualAuthority{};
    topology::TopologyEnrollment b10ManualEnrollment{};
    bool b10ManualAuthorityReady = false;
    bool b10ManualEnrollmentCreated = false;
    if (manualAuthorityScenario) {
        if (!b10ManualQsr.open(0u) || b10ManualQsr.boundPort() == 0u) {
            fail("parent", "could not open manual authority QSR socket");
            return false;
        }
        const topology::EndpointBinding manualQsrBinding =
            rootQsrBinding(b10ManualQsr.boundPort());
        error.clear();
        if (!b10ManualAuthority.initialize(
                sessionIdentity(), compatibilityIdentity(),
                qsp::gameStartFingerprint(
                    qsp::canonicalGameStart(gameDescriptor())),
                activationRequest(sidecar.boundPort()), manualQsrBinding,
                clock, error)) {
            fail("parent", error.empty()
                               ? "could not initialize manual QSR authority"
                               : error);
            return false;
        }
        b10ManualAuthority.setVerifiedCursor(
            model::VerifiedChainCursor{0u, 0u, gameDescriptor().initialDigest});
        b10ManualAuthorityReady = true;
    }

    const uint8_t topologyQspRoute = b10Case == 34u
        ? qnet::SPECTATE_ROUTE_PUNCHED : qnet::SPECTATE_ROUTE_DIRECT;
    qsp::SpectatorGrant issuedGrant{};
    GrantRequest request{};
    request.viewerId = kViewerId;
    request.route = topologyQspRoute;
    request.peerIpv4BE = loopbackIpv4BE();
    request.lifetimeSeconds = 300u;
    AdmissionDenyReason denied = AdmissionDenyReason::None;
    if (!sidecar.issueGrant(request, issuedGrant, denied, clock)
        || issuedGrant.viewerId != kViewerId
        || issuedGrant.route != topologyQspRoute) {
        fail("parent", "production sidecar could not issue the ordinary E0 grant");
        return false;
    }
    const qnet::SpectateInfo live = makeLiveInfo(sidecar.boundPort(), issuedGrant);

    qnet::UdpSock controlSocket;
    if (!controlSocket.open(0u) || controlSocket.boundPort() == 0u) {
        fail("parent", "could not open host control socket");
        return false;
    }
    qnet::UdpControlListener listener;
    listener.open(controlSocket);

    HiddenProcess child;
    std::wstring childArguments = L"--released-topology-child "
        + std::to_wstring(sidecar.boundPort()) + L" "
        + std::to_wstring(controlSocket.boundPort());
    if (awaitAuthorityRevocation) {
        childArguments += L" --await-authority-revocation";
    } else if (awaitAuthorityLivenessTimeout) {
        childArguments += L" --await-authority-liveness-timeout";
    } else if (authorityStateValidation) {
        childArguments += L" --authority-state-validation";
    } else if (relayDataBindingValidation) {
        childArguments += L" --relay-data-binding-validation";
    } else if (b10Case != 0u) {
        childArguments += L" --b10-case " + std::to_wstring(b10Case);
    }
    if (!child.launch(childArguments,
                       error)) {
        fail("parent", error);
        return false;
    }

    qnet::UdpControlLink* e0Link = nullptr;
    bool e0ControlAccepted = false;
    TopologyAttempt firstTopology{};
    TopologyAttempt retryTopology{};
    bool firstTopologyAccepted = false;
    bool retryTopologyAccepted = false;
    bool e0VersionSeen = false;
    bool e0IntentSeen = false;
    bool e0InfoSent = false;
    uint64_t e0ReleaseAt = 0u;
    bool challengeNegativesPassed = false;
    bool firstInfoBuiltAndDropped = false;
    bool firstEnrollmentRetained = false;
    bool firstLinkStayedOpenUntilRetry = false;
    bool retryChallengeFresh = false;
    bool firstProofRejectedOnRetryChallenge = false;
    qnet::SpectateTopologyInfoV2 staleInfo{};
    bool staleInfoRetained = false;
    bool staleInfoReplayed = false;
    uint64_t staleInfoReplayReleaseAt = 0u;
    bool staleInfoRejectedBeforeFresh = false;
    bool topologyResponseSent = false;
    uint64_t topologyReleaseAt = 0u;
    bool publishedRecord = false;
    bool attachmentObserved = false;
    bool authorityRevocationTriggered = false;
    bool e0SurvivedAuthorityRevocation = false;
    bool livenessSuppressed = false;
    bool e0SurvivedLivenessLoss = false;
    bool b10RecordPublishedAfterWithheldInfo = false;
    uint64_t b10E0EvidenceReleaseAt = 0u;
    bool b10CredentialExpiryAuthorityReconfigured = false;
    bool b10ExpiredEnrollmentReleased = false;
    bool b10SuppressTopologyService = false;
    bool b10PunchedProofRejected = false;
    uint32_t b10ManualBootstrapQtas = 0u;
    bool b10ManualRevocationSent = false;
    model::VerifiedChainCursor authorityStateCursor{};
    model::ParentLease authorityStateLease{};
    wire::AuthorityState authorityStateBase{};
    RouteEndpoint authorityStateChildQsr{};
    uint64_t authorityStateOutboundQtaSequence = 1u;
    uint64_t authorityStateCommitSentAt = 0u;
    bool authorityStateLeaseSent = false;
    bool authorityStateAttached = false;
    bool authorityStateInitialBatchSent = false;
    bool authorityStateQueuedLivenessSent = false;
    const uint64_t started = nowMs();
    while (!elapsed(started, kPumpTimeoutMs)) {
        ++clock;
        if ((!awaitAuthorityLivenessTimeout || !livenessSuppressed)
            && !b10SuppressTopologyService) {
            sidecar.service(clock);
        }
        if (b10Case == 45u && b10ManualAuthorityReady
            && !b10ManualRevocationSent) {
            std::array<uint8_t, 4096u> b10QsrInbound{};
            for (unsigned budget = 0u; budget < 8u; ++budget) {
                qnet::Endpoint sender{};
                const int received = b10ManualQsr.recvFrom(
                    b10QsrInbound.data(), b10QsrInbound.size(), sender);
                if (received == 0) break;
                if (received < 0) {
                    fail("parent", "B10 revocation QSR receive failed");
                    return false;
                }
                topology::TopologyControlEnvelope envelope{};
                const RouteEndpoint from{
                    sender.sa.sin_addr.s_addr, ntohs(sender.sa.sin_port)};
                if (b10ManualAuthority.decodeControl(
                        from, b10QsrInbound.data(),
                        static_cast<size_t>(received), clock, envelope)
                    != topology::TopologyControlResult::Accepted) {
                    fail("parent", "B10 bootstrap QTA was not accepted by authority");
                    return false;
                }
                wire::ControlDatagram bootstrap{};
                if (wire::decodeControlDatagram(envelope.qsr.data(),
                                                envelope.qsr.size(), bootstrap)
                        != wire::DecodeResult::Ok
                    || bootstrap.type != wire::ControlMessageType::NodeAdvertisement) {
                    fail("parent", "B10 retry did not carry a NodeAdvertisement");
                    return false;
                }
                ++b10ManualBootstrapQtas;
            }
            if (b10ManualBootstrapQtas >= 2u) {
                std::vector<topology::AuthorityStateRecipient> recipients;
                if (!b10ManualAuthority.collectAuthorityState(
                        wire::AuthorityStateKind::Revocation,
                        wire::AuthorityStateReason::AuthorityExpired, clock,
                        recipients)
                    || recipients.size() != 1u) {
                    fail("parent", "B10 retry revocation could not target its QSR child");
                    return false;
                }
                wire::ControlDatagram stateDatagram{};
                stateDatagram.type = wire::ControlMessageType::AuthorityState;
                stateDatagram.authorityState = recipients.front().state;
                std::vector<uint8_t> stateBytes;
                std::vector<uint8_t> wrapped;
                if (!wire::encodeControlDatagram(stateDatagram, stateBytes)
                    || !b10ManualAuthority.encodeControl(
                        b10ManualEnrollment, 1u, stateBytes.data(),
                        stateBytes.size(), wrapped)) {
                    fail("parent", "B10 retry revocation could not be authenticated");
                    return false;
                }
                qnet::Endpoint destination{};
                destination.sa.sin_addr.s_addr = recipients.front().qsrRoute.ipv4BE;
                destination.sa.sin_port = htons(recipients.front().qsrRoute.port);
                if (!b10ManualQsr.sendTo(destination, wrapped.data(),
                                         wrapped.size())) {
                    fail("parent", "B10 retry revocation send failed");
                    return false;
                }
                b10ManualRevocationSent = true;
            }
        }
        if (authorityStateValidation && b10ManualAuthorityReady) {
            std::array<uint8_t, 4096u> authorityStateInbound{};
            for (unsigned budget = 0u; budget < 8u; ++budget) {
                qnet::Endpoint sender{};
                const int received = b10ManualQsr.recvFrom(
                    authorityStateInbound.data(), authorityStateInbound.size(),
                    sender);
                if (received == 0) break;
                if (received < 0) {
                    fail("parent", "authority-state QSR receive failed");
                    return false;
                }
                const RouteEndpoint from{
                    sender.sa.sin_addr.s_addr, ntohs(sender.sa.sin_port)};
                topology::TopologyControlEnvelope envelope{};
                if (b10ManualAuthority.decodeControl(
                        from, authorityStateInbound.data(),
                        static_cast<size_t>(received), clock, envelope)
                    != topology::TopologyControlResult::Accepted) {
                    fail("parent", "authority-state client QTA was not accepted");
                    return false;
                }
                wire::ControlDatagram inbound{};
                if (wire::decodeControlDatagram(envelope.qsr.data(),
                                                envelope.qsr.size(), inbound)
                    != wire::DecodeResult::Ok) {
                    fail("parent", "authority-state client QTA body was malformed");
                    return false;
                }
                if (inbound.type == wire::ControlMessageType::NodeAdvertisement) {
                    if (!authorityStateLeaseSent) {
                        if (!publishedRecord || authorityStateCursor.nextRecord == 0u) {
                            fail("parent", "authority-state offer preceded verified E0 record");
                            return false;
                        }
                        uint64_t nonce = 0u;
                        uint64_t expiryMs = 0u;
                        if (!b10ManualAuthority.reserveRootOffer(
                                b10ManualEnrollment.nodeId, clock, nonce,
                                expiryMs)) {
                            fail("parent", "authority-state root could not reserve parent offer");
                            return false;
                        }
                        authorityStateLease = manualParentLease(
                            b10ManualAuthority, b10ManualEnrollment, nonce,
                            expiryMs, authorityStateCursor);
                        wire::ControlDatagram leaseDatagram{};
                        leaseDatagram.type = wire::ControlMessageType::ParentLease;
                        leaseDatagram.parentLease = authorityStateLease;
                        authorityStateChildQsr = from;
                        if (!sendManualQtaDatagram(
                                b10ManualQsr, b10ManualAuthority,
                                b10ManualEnrollment, authorityStateChildQsr,
                                authorityStateOutboundQtaSequence++,
                                leaseDatagram)) {
                            fail("parent", "authority-state parent offer could not be sent");
                            return false;
                        }
                        authorityStateLeaseSent = true;
                    } else if (!authorityStateAttached) {
                        wire::ControlDatagram leaseDatagram{};
                        leaseDatagram.type = wire::ControlMessageType::ParentLease;
                        leaseDatagram.parentLease = authorityStateLease;
                        if (!sendManualQtaDatagram(
                                b10ManualQsr, b10ManualAuthority,
                                b10ManualEnrollment, from,
                                authorityStateOutboundQtaSequence++,
                                leaseDatagram)) {
                            fail("parent", "authority-state parent offer retry could not be sent");
                            return false;
                        }
                    }
                    continue;
                }
                if (inbound.type != wire::ControlMessageType::ParentLease) {
                    fail("parent", "authority-state client sent unexpected QSR control");
                    return false;
                }
                if (authorityStateAttached) {
                    continue;
                }
                if (!authorityStateLeaseSent
                    || !b10ManualAuthority.authorize(from, inbound, clock)
                    || !b10ManualAuthority.confirmRedemption(
                        inbound.parentLease, clock)) {
                    fail("parent", "authority-state lease redemption was rejected");
                    return false;
                }
                authorityStateChildQsr = from;
                wire::ControlDatagram commitDatagram{};
                commitDatagram.type = wire::ControlMessageType::ParentCommit;
                commitDatagram.parentCommit = manualParentCommit(
                    authorityStateLease, authorityStateCursor);
                if (!sendManualQtaDatagram(
                        b10ManualQsr, b10ManualAuthority, b10ManualEnrollment,
                        authorityStateChildQsr,
                        authorityStateOutboundQtaSequence++, commitDatagram)) {
                    fail("parent", "authority-state parent receipt could not be sent");
                    return false;
                }
                authorityStateAttached = true;

                std::vector<topology::AuthorityStateRecipient> recipients;
                if (!b10ManualAuthority.collectAuthorityState(
                        wire::AuthorityStateKind::Liveness,
                        wire::AuthorityStateReason::None, clock, recipients)
                    || recipients.size() != 1u) {
                    fail("parent", "authority-state could not target attached child");
                    return false;
                }
                authorityStateBase = recipients.front().state;
                authorityStateBase.livenessWindowMs = 1000u;
                wire::ControlDatagram exactLiveness{};
                exactLiveness.type = wire::ControlMessageType::AuthorityState;
                exactLiveness.authorityState = authorityStateBase;
                if (!sendManualQtaDatagram(
                        b10ManualQsr, b10ManualAuthority, b10ManualEnrollment,
                        authorityStateChildQsr,
                        authorityStateOutboundQtaSequence++, exactLiveness)) {
                    fail("parent", "authority-state exact liveness could not be sent");
                    return false;
                }

                wire::ControlDatagram wrongBinding = exactLiveness;
                wrongBinding.authorityState.sequence = authorityStateBase.sequence + 1u;
                ++wrongBinding.authorityState.qsrBindingGeneration;
                wrongBinding.authorityState.livenessWindowMs = 5000u;
                if (!sendManualQtaDatagram(
                        b10ManualQsr, b10ManualAuthority, b10ManualEnrollment,
                        authorityStateChildQsr,
                        authorityStateOutboundQtaSequence++, wrongBinding)) {
                    fail("parent", "authority-state wrong binding could not be sent");
                    return false;
                }
                wire::ControlDatagram replay = exactLiveness;
                replay.authorityState.livenessWindowMs = 5000u;
                if (!sendManualQtaDatagram(
                        b10ManualQsr, b10ManualAuthority, b10ManualEnrollment,
                        authorityStateChildQsr,
                        authorityStateOutboundQtaSequence++, replay)) {
                    fail("parent", "authority-state replay could not be sent");
                    return false;
                }
                const std::array<uint8_t, 1u> malformed{{0xA5u}};
                if (!sendManualQtaPayload(
                        b10ManualQsr, b10ManualAuthority, b10ManualEnrollment,
                        authorityStateChildQsr,
                        authorityStateOutboundQtaSequence++, malformed.data(),
                        malformed.size())) {
                    fail("parent", "authority-state malformed QTA body could not be sent");
                    return false;
                }
                if (!sendManualQtaDatagram(
                        b10ManualQsr, b10ManualAuthority, b10ManualEnrollment,
                        authorityStateChildQsr,
                        authorityStateOutboundQtaSequence++, exactLiveness,
                        true)) {
                    fail("parent", "authority-state invalid-MAC QTA could not be sent");
                    return false;
                }
                authorityStateInitialBatchSent = true;
                authorityStateCommitSentAt = nowMs();
            }
            if (authorityStateInitialBatchSent
                && !authorityStateQueuedLivenessSent
                && elapsed(authorityStateCommitSentAt, 900u)) {
                wire::ControlDatagram queuedLiveness{};
                queuedLiveness.type = wire::ControlMessageType::AuthorityState;
                queuedLiveness.authorityState = authorityStateBase;
                queuedLiveness.authorityState.sequence =
                    authorityStateBase.sequence + 2u;
                queuedLiveness.authorityState.livenessWindowMs = 5000u;
                queuedLiveness.authorityState.eventTimeMs = clock;
                if (!sendManualQtaDatagram(
                        b10ManualQsr, b10ManualAuthority, b10ManualEnrollment,
                        authorityStateChildQsr,
                        authorityStateOutboundQtaSequence++, queuedLiveness)) {
                    fail("parent", "authority-state queued liveness could not be sent");
                    return false;
                }
                authorityStateQueuedLivenessSent = true;
            }
        }
        listener.pump(0);

        for (;;) {
            qnet::UdpControlLink* const accepted = listener.acceptOne();
            if (accepted == nullptr) break;
            qnet::VersionConfig version{qnet::kProtocolVersion};
            if (!accepted->sendMsg(qnet::MSG_VERSION_CONFIG, &version,
                                   sizeof(version))) {
                fail("parent", "host could not send control version");
                return false;
            }
            if (!e0ControlAccepted) {
                e0Link = accepted;
                e0ControlAccepted = true;
            } else if (!firstTopologyAccepted) {
                firstTopology.link = accepted;
                firstTopologyAccepted = true;
            } else if ((firstInfoBuiltAndDropped || b10Case == 34u)
                       && !retryTopologyAccepted
                       && !topologyResponseSent) {
                firstLinkStayedOpenUntilRetry = firstTopology.link != nullptr
                    && firstTopology.link->isOpen();
                if (!firstLinkStayedOpenUntilRetry) {
                    fail("parent", "first topology link closed before response timeout retry");
                    return false;
                }
                retryTopology.link = accepted;
                retryTopologyAccepted = true;
                listener.release(firstTopology.link);
                firstTopology.link = nullptr;
            } else {
                fail("parent", "unexpected control client during response-loss retry");
                return false;
            }
        }

        std::vector<qnet::ControlLink::Received> messages;
        if (!readLinkMessages(e0Link, messages, error)) {
            fail("parent", error);
            return false;
        }
        for (const qnet::ControlLink::Received& rx : messages) {
            if (!e0VersionSeen) {
                qnet::VersionConfig version{};
                if (rx.type != qnet::MSG_VERSION_CONFIG
                    || rx.body.size() != sizeof(version)) {
                    fail("parent", "ordinary E0 child version was malformed");
                    return false;
                }
                std::memcpy(&version, rx.body.data(), sizeof(version));
                if (version.protocolVersion != qnet::kProtocolVersion) {
                    fail("parent", "ordinary E0 child version mismatched");
                    return false;
                }
                e0VersionSeen = true;
                continue;
            }
            if (!e0IntentSeen) {
                qnet::ClientIntent intent{};
                if (rx.type != qnet::MSG_CLIENT_INTENT
                    || rx.body.size() != sizeof(intent)) {
                    fail("parent", "ordinary E0 child intent was malformed");
                    return false;
                }
                std::memcpy(&intent, rx.body.data(), sizeof(intent));
                if (!validIntent(intent, 0u, 0u)
                    || !e0Link->sendMsg(qnet::MSG_SPECTATE_INFO, &live,
                                        sizeof(live))) {
                    fail("parent", "ordinary E0 admission could not respond");
                    return false;
                }
                e0IntentSeen = true;
                e0InfoSent = true;
                e0ReleaseAt = nowMs() + 250u;
                continue;
            }
            fail("parent", "ordinary E0 child sent unexpected control data");
            return false;
        }

        messages.clear();
        TopologyAttempt* const activeTopology = retryTopologyAccepted
            ? &retryTopology
            : firstTopologyAccepted ? &firstTopology : nullptr;
        const bool servicingRetryTopology = retryTopologyAccepted;
        if (!readLinkMessages(activeTopology != nullptr
                                  ? activeTopology->link
                                  : nullptr,
                              messages, error)) {
            fail("parent", error);
            return false;
        }
        for (const qnet::ControlLink::Received& rx : messages) {
            if (activeTopology == nullptr) {
                fail("parent", "topology control data arrived before link acceptance");
                return false;
            }
            if (!activeTopology->versionSeen) {
                qnet::VersionConfig version{};
                if (rx.type != qnet::MSG_VERSION_CONFIG
                    || rx.body.size() != sizeof(version)) {
                    fail("parent", "released topology child version was malformed");
                    return false;
                }
                std::memcpy(&version, rx.body.data(), sizeof(version));
                if (version.protocolVersion != qnet::kProtocolVersion) {
                    fail("parent", "released topology child version mismatched");
                    return false;
                }
                activeTopology->versionSeen = true;
                continue;
            }
            if (!activeTopology->intentSeen) {
                qnet::ClientIntent intent{};
                if (rx.type != qnet::MSG_CLIENT_INTENT
                    || rx.body.size() != sizeof(intent)) {
                    fail("parent", "released topology intent was malformed");
                    return false;
                }
                std::memcpy(&intent, rx.body.data(), sizeof(intent));
                if (!validIntent(
                        intent, qnet::kSpectateCapabilityReleasedTopologyV2Mask,
                        1u, topologyQspRoute)) {
                    fail("parent", "released topology intent did not bind E0 facts");
                    return false;
                }
                activeTopology->intentSeen = true;
                activeTopology->challenge = {};
                activeTopology->challenge.schemaVersion =
                    qnet::kSpectateTopologyControlSchemaVersion;
                activeTopology->challenge.requiredFeatures =
                    qnet::kSpectateTopologyControlRequiredFeatures;
                activeTopology->challenge.sessionId = kSessionId;
                activeTopology->challenge.viewerId = kViewerId;
                activeTopology->challenge.viewerGeneration = 1u;
                const uint8_t challengeSeed = servicingRetryTopology
                    ? 0xB1u
                    : 0x91u;
                for (size_t index = 0u;
                     index < sizeof(activeTopology->challenge.peerChallenge);
                     ++index) {
                    activeTopology->challenge.peerChallenge[index] =
                        static_cast<uint8_t>(challengeSeed + index);
                }
                uint8_t challengeOr = 0u;
                for (const uint8_t byte : activeTopology->challenge.peerChallenge)
                    challengeOr |= byte;
                if (challengeOr == 0u || activeTopology->link == nullptr
                    || !activeTopology->link->sendMsg(
                        qnet::MSG_SPECTATE_TOPOLOGY_CHALLENGE_V2,
                        &activeTopology->challenge,
                        sizeof(activeTopology->challenge))) {
                    fail("parent", "host could not issue released V2 link challenge");
                    return false;
                }
                activeTopology->challengeIssued = true;
                continue;
            }
            qnet::SpectateTopologyProofV2 candidate{};
            if (rx.type != qnet::MSG_SPECTATE_TOPOLOGY_PROOF_V2
                || rx.body.size() != sizeof(candidate)) {
                fail("parent", "released topology proof was malformed");
                return false;
            }
            std::memcpy(&candidate, rx.body.data(), sizeof(candidate));
            qnet::ClientIntent proofIntent{};
            proofIntent.intent = qnet::INTENT_SPECTATE;
            proofIntent.spectatorRoute = topologyQspRoute;
            proofIntent.spectatorCapabilities =
                qnet::kSpectateCapabilityReleasedTopologyV2Mask;
            proofIntent.sessionId = static_cast<uint32_t>(kSessionId);
            proofIntent.keyLow = sessionIdentity().cookie.low;
            proofIntent.keyHigh = sessionIdentity().cookie.high;
            proofIntent.viewerId = kViewerId;
            proofIntent.resumeRecord = 1u;
            qsp::SpectatorGrant candidateGrant{};
            const bool proofValid = activeTopology->challengeIssued
                && controlv2::validateProof(candidate, proofIntent, live,
                                            activeTopology->challenge,
                                            candidateGrant)
                && candidateGrant == issuedGrant;
            if (!proofValid) {
                std::memset(&candidate, 0, sizeof(candidate));
                std::memset(&candidateGrant, 0, sizeof(candidateGrant));
                fail("parent", "production V2 proof verifier rejected E0 grant proof");
                return false;
            }
            if (activeTopology->proofReceived) {
                std::memset(&candidate, 0, sizeof(candidate));
                std::memset(&candidateGrant, 0, sizeof(candidateGrant));
                continue;
            }
            activeTopology->proof = candidate;
            activeTopology->proofGrant = candidateGrant;
            activeTopology->claim.sessionId = candidate.grant.sessionId;
            activeTopology->claim.sessionEpoch = candidate.sessionEpoch;
            activeTopology->claim.viewerId = candidate.viewerId;
            activeTopology->claim.viewerGeneration = candidate.viewerGeneration;
            activeTopology->claim.qspEndpoint =
                RouteEndpoint{candidate.qspIpv4BE, candidate.qspPort};
            activeTopology->claim.qspProvenance = topologyQspRoute
                    == qnet::SPECTATE_ROUTE_PUNCHED
                ? topology::EndpointProvenance::ProvenPunchedQsp
                : topology::EndpointProvenance::ProvenDirectQsp;
            activeTopology->claim.qspRouteGeneration =
                candidate.qspRouteGeneration;
            activeTopology->claim.discoveryGeneration =
                candidate.discoveryGeneration;
            activeTopology->claim.compatibility = candidate.compatibilityIdentity;
            activeTopology->claim.streamRootIdentity =
                candidate.streamRootIdentity;
            std::memset(&candidate, 0, sizeof(candidate));
            std::memset(&candidateGrant, 0, sizeof(candidateGrant));

            if (!servicingRetryTopology) {
                qnet::SpectateTopologyChallengeV2 zeroChallenge =
                    activeTopology->challenge;
                std::memset(zeroChallenge.peerChallenge, 0,
                            sizeof(zeroChallenge.peerChallenge));
                qsp::SpectatorGrant zeroRejectedGrant{};
                const bool zeroChallengeRejected =
                    !controlv2::validateProof(activeTopology->proof,
                                              proofIntent, live, zeroChallenge,
                                              zeroRejectedGrant)
                    && zeroRejectedGrant.grantId == 0u;
                std::memset(&zeroChallenge, 0, sizeof(zeroChallenge));
                std::memset(&zeroRejectedGrant, 0, sizeof(zeroRejectedGrant));
                qnet::SpectateTopologyChallengeV2 freshChallenge =
                    activeTopology->challenge;
                freshChallenge.peerChallenge[0] ^= 0x5Au;
                qsp::SpectatorGrant replayRejectedGrant{};
                const bool replayRejected =
                    !controlv2::validateProof(activeTopology->proof,
                                              proofIntent, live, freshChallenge,
                                              replayRejectedGrant)
                    && replayRejectedGrant.grantId == 0u;
                std::memset(&freshChallenge, 0, sizeof(freshChallenge));
                std::memset(&replayRejectedGrant, 0,
                            sizeof(replayRejectedGrant));
                challengeNegativesPassed = zeroChallengeRejected
                    && replayRejected;
                if (!challengeNegativesPassed) {
                    fail("parent", "challenge-negative proof reached acceptance");
                    return false;
                }
            } else {
                retryChallengeFresh = std::memcmp(
                    firstTopology.challenge.peerChallenge,
                    activeTopology->challenge.peerChallenge,
                    sizeof(activeTopology->challenge.peerChallenge)) != 0;
                qsp::SpectatorGrant replayedFirstGrant{};
                firstProofRejectedOnRetryChallenge = retryChallengeFresh
                    && !controlv2::validateProof(firstTopology.proof,
                                                  proofIntent, live,
                                                  activeTopology->challenge,
                                                  replayedFirstGrant)
                    && replayedFirstGrant.grantId == 0u;
                std::memset(&replayedFirstGrant, 0,
                            sizeof(replayedFirstGrant));
                if (!retryChallengeFresh
                    || !firstProofRejectedOnRetryChallenge) {
                    fail("parent", "retry did not require a fresh V2 proof challenge");
                    return false;
                }
            }
            activeTopology->proofReceived = true;
        }

        if (activeTopology != nullptr && activeTopology->proofReceived
            && !topologyResponseSent
            && (!firstInfoBuiltAndDropped || servicingRetryTopology)) {
            if ((b10Case == 2u || b10Case == 3u) && servicingRetryTopology
                && !b10CredentialExpiryAuthorityReconfigured) {
                error.clear();
                if (!sidecar.configureTopologyAuthority(
                        activationRequest(sidecar.boundPort(),
                                          b10Case == 2u ? 1000u : 2000u),
                        ++clock,
                        error)) {
                    fail("parent", error.empty()
                                       ? "could not arm B10 credential-expiry authority"
                                       : error);
                    return false;
                }
                b10CredentialExpiryAuthorityReconfigured = true;
            }
            topology::TopologyEnrollment enrollment{};
            std::string enrollmentError;
            bool enrolled = false;
            if (manualAuthorityScenario) {
                if (!b10ManualAuthorityReady) {
                    enrollmentError = "manual QSR authority was not ready";
                } else if (!b10ManualEnrollmentCreated) {
                    topology::TopologyViewerProof manualProof{};
                    manualProof.viewerId = activeTopology->claim.viewerId;
                    manualProof.viewerGeneration =
                        activeTopology->claim.viewerGeneration;
                    manualProof.endpoint = activeTopology->claim.qspEndpoint;
                    manualProof.provenance = activeTopology->claim.qspProvenance;
                    manualProof.routeGeneration =
                        activeTopology->claim.qspRouteGeneration;
                    manualProof.confirmedCursor = 0u;
                    manualProof.qspStreaming = true;
                    manualProof.relayCapable = true;
                    enrolled = b10ManualAuthority.enroll(
                        manualProof, clock, b10ManualEnrollment, enrollmentError);
                    b10ManualEnrollmentCreated = enrolled;
                    if (enrolled) enrollment = b10ManualEnrollment;
                } else {
                    enrollment = b10ManualEnrollment;
                    enrolled = true;
                }
            } else {
                enrolled = sidecar.enrollTopologyViewerExact(
                    activeTopology->claim, activeTopology->proofGrant, clock,
                    enrollment, enrollmentError);
            }
            if (enrolled) {
                qnet::SpectateTopologyInfoV2 response{};
                if (!controlv2::infoFromEnrollment(activeTopology->proof.requestNonce,
                                                   enrollment, response)
                    || !controlv2::signInfo(response,
                                             activeTopology->proofGrant)) {
                    if (!manualAuthorityScenario)
                        sidecar.rollbackTopologyViewer(kViewerId);
                    fail("parent", "production V2 topology response could not be built");
                    return false;
                }
                if (!firstInfoBuiltAndDropped) {
                    staleInfo = response;
                    staleInfoRetained = staleInfo.requestNonce
                        == activeTopology->proof.requestNonce;
                    std::memset(&response, 0, sizeof(response));
                    firstInfoBuiltAndDropped = true;
                    const auto retained = sidecar.status();
                    firstEnrollmentRetained = manualAuthorityScenario
                        ? b10ManualEnrollmentCreated && retained.viewerCount == 1u
                        : retained.viewerCount == 1u
                            && retained.topologyEnrolled == 1u
                            && retained.topologyAttached == 0u;
                    if (b10Case == 38u) {
                        if (!publishVerifiedRecord(sidecar, sequence, ++clock)) {
                            fail("parent", "sidecar could not publish B10 timeout record");
                            return false;
                        }
                        publishedRecord = true;
                        b10RecordPublishedAfterWithheldInfo = true;
                        b10E0EvidenceReleaseAt = nowMs() + 200u;
                    }
                } else {
                    if (!servicingRetryTopology || activeTopology->link == nullptr) {
                        std::memset(&response, 0, sizeof(response));
                        if (!manualAuthorityScenario)
                            sidecar.rollbackTopologyViewer(kViewerId);
                        fail("parent", "retry V2 topology response lost its control link");
                        return false;
                    }
                    if (!staleInfoReplayed) {
                        if (!staleInfoRetained
                            || staleInfo.requestNonce
                                == activeTopology->proof.requestNonce
                            || !activeTopology->link->sendMsg(
                                qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2, &staleInfo,
                                sizeof(staleInfo))) {
                            std::memset(&response, 0, sizeof(response));
                            if (!manualAuthorityScenario)
                                sidecar.rollbackTopologyViewer(kViewerId);
                            fail("parent", "stale V2 Info replay could not be injected");
                            return false;
                        }
                        staleInfoReplayed = true;
                        staleInfoReplayReleaseAt = nowMs() + 200u;
                        std::memset(&response, 0, sizeof(response));
                    } else if (nowMs() < staleInfoReplayReleaseAt) {
                        std::memset(&response, 0, sizeof(response));
                    } else {
                        if (b10Case == 2u && !publishedRecord) {
                            if (!publishVerifiedRecord(sidecar, sequence,
                                                       ++clock)) {
                                std::memset(&response, 0, sizeof(response));
                                if (!manualAuthorityScenario)
                                    sidecar.rollbackTopologyViewer(kViewerId);
                                fail("parent", "sidecar could not publish B10 expiry record");
                                return false;
                            }
                            publishedRecord = true;
                            b10E0EvidenceReleaseAt = nowMs() + 200u;
                            std::memset(&response, 0, sizeof(response));
                            continue;
                        }
                        if (b10Case == 2u
                            && nowMs() < b10E0EvidenceReleaseAt) {
                            std::memset(&response, 0, sizeof(response));
                            continue;
                        }
                        const auto beforeFresh = sidecar.status();
                        staleInfoRejectedBeforeFresh = manualAuthorityScenario
                            ? b10ManualEnrollmentCreated
                                && beforeFresh.viewerCount == 1u
                            : beforeFresh.topologyAttached == 0u
                                && beforeFresh.topologyEnrolled == 1u;
                        bool sent = staleInfoRejectedBeforeFresh;
                        if (b10Case == 0u) {
                            sent = sent && activeTopology->link->sendMsg(
                                qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2, &response,
                                sizeof(response));
                        } else {
                            const B10InfoDelivery delivery = prepareB10Info(
                                b10Case, response, activeTopology->proofGrant);
                            if (delivery == B10InfoDelivery::Truncated) {
                                std::array<uint8_t,
                                           sizeof(qnet::SpectateTopologyInfoV2)>
                                    wire{};
                                std::memcpy(wire.data(), &response,
                                            sizeof(response));
                                sent = sent && activeTopology->link->sendMsg(
                                    qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2,
                                    wire.data(), wire.size() - 1u);
                            } else if (delivery == B10InfoDelivery::Oversized) {
                                std::vector<uint8_t> wire(sizeof(response) + 1u);
                                std::memcpy(wire.data(), &response,
                                            sizeof(response));
                                sent = sent && activeTopology->link->sendMsg(
                                    qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2,
                                    wire.data(), wire.size());
                            } else {
                                sent = sent && activeTopology->link->sendMsg(
                                    qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2, &response,
                                    sizeof(response));
                                if (delivery == B10InfoDelivery::Duplicate) {
                                    sent = sent && activeTopology->link->sendMsg(
                                        qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2,
                                        &response, sizeof(response));
                                }
                            }
                        }
                        if (!sent) {
                            std::memset(&response, 0, sizeof(response));
                            if (!manualAuthorityScenario)
                                sidecar.rollbackTopologyViewer(kViewerId);
                            fail("parent", b10Case == 0u
                                               ? "stale V2 Info advanced before fresh response"
                                               : "B10 V2 Info injection failed");
                            return false;
                        }
                        std::memset(&response, 0, sizeof(response));
                        topologyResponseSent = true;
                        topologyReleaseAt = nowMs() + 500u;
                        if (!publishedRecord
                            && !publishVerifiedRecord(sidecar, sequence,
                                                      ++clock)) {
                            fail("parent", "sidecar could not publish a verified record");
                            return false;
                        }
                        publishedRecord = true;
                        if (authorityStateValidation) {
                            authorityStateCursor = verifiedCursorAfterFirstRecord();
                            b10ManualAuthority.setVerifiedCursor(
                                authorityStateCursor);
                        }
                        if (b10Case == 2u) {
                            b10SuppressTopologyService = true;
                        } else if (b10Case != 0u) {
                            b10E0EvidenceReleaseAt = nowMs() + 200u;
                        }
                    }
                }
            } else if (b10Case == 34u
                       && enrollmentError
                           == "the released topology proof did not match the proved QSP route") {
                if (!b10PunchedProofRejected) {
                    b10PunchedProofRejected = true;
                    if (!publishedRecord
                        && !publishVerifiedRecord(sidecar, sequence, ++clock)) {
                        fail("parent", "sidecar could not publish B10 punched-proof record");
                        return false;
                    }
                    publishedRecord = true;
                    b10E0EvidenceReleaseAt = nowMs() + 200u;
                }
            } else if (enrollmentError
                       != "topology enrollment requires a current proved QSP streaming generation") {
                fail("parent", enrollmentError.empty()
                                   ? "exact topology enrollment failed"
                                   : enrollmentError);
                return false;
            }
        }

        const auto status = sidecar.status();
        attachmentObserved = attachmentObserved
            || (status.topologyAttached == 1u && status.topologyEnrolled == 1u);
        if (b10Case == 3u && attachmentObserved) {
            b10SuppressTopologyService = true;
        }
        if (awaitAuthorityRevocation && attachmentObserved && publishedRecord
            && !authorityRevocationTriggered) {
            error.clear();
            if (!sidecar.configureTopologyAuthority(
                    activationRequest(sidecar.boundPort()), ++clock, error)) {
                fail("parent", error.empty()
                                   ? "could not trigger authority revocation"
                                   : error);
                return false;
            }
            authorityRevocationTriggered = true;
        }
        if (awaitAuthorityLivenessTimeout && attachmentObserved
            && publishedRecord) {
            livenessSuppressed = true;
        }
        if (awaitAuthorityRevocation && authorityRevocationTriggered) {
            const auto afterRevoke = sidecar.status();
            e0SurvivedAuthorityRevocation = afterRevoke.viewerCount == 1u
                && afterRevoke.topologyEnrolled == 0u
                && afterRevoke.topologyAttached == 0u
                && afterRevoke.failureReason.empty();
        }
        if (awaitAuthorityLivenessTimeout && livenessSuppressed) {
            const auto afterLivenessLoss = sidecar.status();
            e0SurvivedLivenessLoss = afterLivenessLoss.viewerCount == 1u
                && afterLivenessLoss.topologyEnrolled == 1u
                && afterLivenessLoss.topologyAttached == 1u
                && afterLivenessLoss.failureReason.empty();
        }
        if ((!b10FailureScenario) && e0Link != nullptr && e0InfoSent
            && nowMs() >= e0ReleaseAt) {
            listener.release(e0Link);
            e0Link = nullptr;
        }
        if (retryTopology.link != nullptr && topologyResponseSent
            && nowMs() >= topologyReleaseAt) {
            listener.release(retryTopology.link);
            retryTopology.link = nullptr;
        }
        if (awaitAuthorityRevocation && authorityRevocationTriggered
            && e0SurvivedAuthorityRevocation) {
            break;
        }
        if (awaitAuthorityLivenessTimeout && livenessSuppressed
            && e0SurvivedLivenessLoss) {
            break;
        }
        if (b10Case == 38u && firstInfoBuiltAndDropped
            && b10RecordPublishedAfterWithheldInfo && e0InfoSent
            && nowMs() >= b10E0EvidenceReleaseAt) {
            break;
        }
        if (b10Case == 34u && b10PunchedProofRejected && publishedRecord
            && e0InfoSent && nowMs() >= b10E0EvidenceReleaseAt) {
            break;
        }
        if (b10FailureScenario && topologyResponseSent && publishedRecord
            && e0InfoSent && nowMs() >= b10E0EvidenceReleaseAt
            && (b10Case != 3u || attachmentObserved)
            && (b10Case != 45u || b10ManualRevocationSent)) {
            break;
        }
        if (authorityStateValidation && authorityStateAttached
            && authorityStateInitialBatchSent
            && authorityStateQueuedLivenessSent && publishedRecord
            && e0InfoSent) {
            break;
        }
        if (!awaitAuthorityRevocation && attachmentObserved && publishedRecord
            && status.topologyAttached == 0u && status.topologyEnrolled == 0u
            && status.viewerCount == 0u) {
            break;
        }
        Sleep(1u);
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

    if (b10Case == 2u) {
        clock += 2000u;
        sidecar.service(clock);
        const auto afterExpiry = sidecar.status();
        b10ExpiredEnrollmentReleased = afterExpiry.topologyAuthorityReady
            && afterExpiry.topologyEnrolled == 0u
            && afterExpiry.topologyAttached == 0u
            && afterExpiry.viewerCount == 1u
            && afterExpiry.failureReason.empty();
    }

    const auto status = sidecar.status();
    const bool topologyReachedExpectedTerminalState = b10FailureScenario
        ? status.viewerCount == 1u && status.failureReason.empty()
            && (expectedB10FailureTiming == B10FailureTiming::AfterAttachment
                    ? attachmentObserved
                    : !attachmentObserved && status.topologyAttached == 0u)
        : awaitAuthorityRevocation
        ? authorityRevocationTriggered && e0SurvivedAuthorityRevocation
            && status.topologyAuthorityReady && status.topologyQsrPort != 0u
            && status.topologyQsrPort != sidecar.boundPort()
            && status.topologyEnrolled == 0u && status.topologyAttached == 0u
            && status.viewerCount == 1u
        : awaitAuthorityLivenessTimeout
            ? livenessSuppressed && e0SurvivedLivenessLoss
                && status.topologyAuthorityReady && status.topologyQsrPort != 0u
                && status.topologyQsrPort != sidecar.boundPort()
                && status.topologyEnrolled == 1u && status.topologyAttached == 1u
                && status.viewerCount == 1u
            : authorityStateValidation
                ? authorityStateAttached && authorityStateInitialBatchSent
                    && authorityStateQueuedLivenessSent
                    && b10ManualAuthorityReady
                    && b10ManualAuthority.isAttached(
                        b10ManualEnrollment.nodeId)
                    && status.topologyAuthorityReady
                    && status.topologyQsrPort != 0u
                    && status.topologyQsrPort != sidecar.boundPort()
                    && status.topologyEnrolled == 0u
                    && status.topologyAttached == 0u
                    && status.viewerCount == 1u
            : status.topologyAuthorityReady && status.topologyQsrPort != 0u
            && status.topologyQsrPort != sidecar.boundPort()
            && status.topologyEnrolled == 0u && status.topologyAttached == 0u
            && status.viewerCount == 0u;
    const bool topologyControlLinksVerified = (b10Case == 38u || b10Case == 34u)
        ? firstTopologyAccepted && firstTopology.versionSeen
            && firstTopology.intentSeen && firstTopology.challengeIssued
            && firstTopology.proofReceived
        : firstTopologyAccepted && retryTopologyAccepted
            && firstTopology.versionSeen && firstTopology.intentSeen
            && firstTopology.challengeIssued && firstTopology.proofReceived
            && retryTopology.versionSeen && retryTopology.intentSeen
            && retryTopology.challengeIssued && retryTopology.proofReceived;
    const bool b10ManualRevocationVerified = b10Case != 45u
        || (b10ManualAuthorityReady && b10ManualEnrollmentCreated
            && b10ManualBootstrapQtas >= 2u && b10ManualRevocationSent
            && b10ManualAuthority.reservedCount() == 0u
            && !b10ManualAuthority.isAttached(b10ManualEnrollment.nodeId));
    const bool b10CredentialExpiryReleaseVerified = b10Case != 2u
        || (b10CredentialExpiryAuthorityReconfigured
            && b10ExpiredEnrollmentReleased);
    const bool controlRetryVerified = b10Case == 34u
        ? b10PunchedProofRejected && publishedRecord
        : b10Case == 38u
        ? firstInfoBuiltAndDropped && firstEnrollmentRetained
            && b10RecordPublishedAfterWithheldInfo
        : firstInfoBuiltAndDropped && firstEnrollmentRetained
            && firstLinkStayedOpenUntilRetry && retryChallengeFresh
            && firstProofRejectedOnRetryChallenge && staleInfoRetained
            && staleInfoReplayed && staleInfoRejectedBeforeFresh;
    const bool passed = e0ControlAccepted && e0VersionSeen && e0IntentSeen
        && e0InfoSent && topologyControlLinksVerified
        && challengeNegativesPassed && controlRetryVerified
        && (topologyResponseSent || b10Case == 38u || b10Case == 34u)
        && publishedRecord && (attachmentObserved || b10FailureScenario
            || (authorityStateValidation && authorityStateAttached))
        && topologyReachedExpectedTerminalState && status.confirmedRecords >= 1u
        && status.failureReason.empty() && b10ManualRevocationVerified
        && b10CredentialExpiryReleaseVerified;
    if (!passed) {
        fail("parent", "released control/QSP/QSR process flow did not close cleanly");
        return false;
    }
    std::memset(&staleInfo, 0, sizeof(staleInfo));
    (void)sidecar.stop(false, ++clock);
    return true;
}

constexpr uint32_t kPhaseCSignalMagic = 0x31504351u;
constexpr size_t kPhaseCSignalBytes = 16u;

enum class PhaseCSignalType : uint8_t {
    Hello = 1u,
    RelayReady = 2u,
    DownstreamBootstrapSent = 3u,
    StartDownstream = 4u,
    RelayAcceptedHello = 5u,
    RelayAcceptedAck = 6u,
    RelayAcceptedDetach = 7u,
    RelayUpstreamAfterDetach = 8u,
    DownstreamStreaming = 9u,
    DownstreamDetachSent = 10u,
    RelaySourceRecordAcknowledged = 11u,
    DownstreamSourceRecordAcknowledged = 12u,
    Failure = 13u,
    RelayAckObserved = 14u,
    DownstreamActivationCommitted = 15u,
    DownstreamRelayReady = 16u,
    DownstreamAcceptedHello = 17u,
    DownstreamAcceptedAck = 18u,
    DownstreamAcceptedDetach = 19u,
    GrandchildBootstrapSent = 20u,
    GrandchildActivationCommitted = 21u,
    GrandchildStreaming = 22u,
    GrandchildDetachSent = 23u,
    GrandchildSourceRecordAcknowledged = 24u,
    RequestRelayRebind = 25u,
    RelayRebindSent = 26u,
    DownstreamTopologyRetiredE0Safe = 27u,
};

enum class PhaseCRole : uint32_t {
    Relay = 1u,
    Downstream = 2u,
    Grandchild = 3u,
};

struct PhaseCSignal {
    PhaseCSignalType type = PhaseCSignalType::Hello;
    uint32_t value = 0u;
    uint32_t detail = 0u;
};

void phaseCWriteLe32(uint8_t* const out, const uint32_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFFu);
    out[1] = static_cast<uint8_t>((value >> 8u) & 0xFFu);
    out[2] = static_cast<uint8_t>((value >> 16u) & 0xFFu);
    out[3] = static_cast<uint8_t>((value >> 24u) & 0xFFu);
}

uint32_t phaseCReadLe32(const uint8_t* const bytes) {
    return static_cast<uint32_t>(bytes[0])
        | (static_cast<uint32_t>(bytes[1]) << 8u)
        | (static_cast<uint32_t>(bytes[2]) << 16u)
        | (static_cast<uint32_t>(bytes[3]) << 24u);
}

qsp::GameDescriptor clientRootDescriptor() {
    qsp::GameDescriptor descriptor = gameDescriptor();
    descriptor.simulationContract = qipc::kRuntimeCompatibilityId;
    descriptor.initialDigest = 0u;
    descriptor.initialDigest = qsp::expectedInitialDigest(descriptor);
    return descriptor;
}

bool publishStartWith(SpectatorSidecar& sidecar, uint64_t& sequence,
                      const uint64_t now,
                      const qsp::GameDescriptor& descriptor) {
    PublicationEvent event{};
    event.kind = PublicationKind::Start;
    event.sequence = sequence++;
    event.descriptor = descriptor;
    sidecar.publish(event, now);
    return sidecar.phase() == SidecarPhase::Active;
}

bool publishRecordWith(SpectatorSidecar& sidecar, uint64_t& sequence,
                       const uint64_t now,
                       const qsp::GameDescriptor& descriptor) {
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
    return true;
}

qipc::IpcConfig clientPrivateLaunchConfig() {
    qipc::IpcConfig cfg{};
    cfg.mode = qipc::MODE_NETPLAY;
    cfg.isHost = 0u;
    cfg.localPlayer = 2u;
    cfg.delay = 2u;
    cfg.rollback = 7u;
    cfg.remoteDelay = 2u;
    cfg.remoteRollback = 7u;
    cfg.sessionId = static_cast<uint32_t>(kSessionId);
    cfg.compatibilityId = qipc::kRuntimeCompatibilityId;
    cfg.cookieLow = 0xABCD1234u;
    const qsp::SessionIdentity identity = sessionIdentity();
    cfg.spectatorCookieLow = identity.cookie.low;
    cfg.spectatorCookieHigh = identity.cookie.high;
    cfg.hasSocket = 1u;
    cfg.peerAddrBE = 0x0100007Fu;
    cfg.peerPortBE = 0x4C1Du;
    cfg.serveSpectators = 1u;
    cfg.spectatorServiceMode = qipc::SPECTATOR_SERVICE_SIDECAR;
    cfg.hasSpectatorSocket = 0u;
    cfg.spectatorClientPublisherEnabled = 1u;
    cfg.spectatorClientPublisherGeneration = 17u;
    cfg.spectatorClientPublisherNonce = 0x8A4C2E17061B3D55ull;
    cfg.compatibilityIdentity = compatibilityIdentity();
    cfg.configurationProvenance =
        qnetplay::config_provenance::makeCommittedWire(
            qnetplay::config_provenance::Stage::LocallySelected,
            qnetplay::config_provenance::Stage::LocallySelected,
            qnetplay::config_provenance::Stage::CasterAdvertised,
            qnetplay::config_provenance::Stage::CasterAdvertised);
    return cfg;
}

constexpr uint64_t kClientRootAnchorViewerId = 0x414E4348524F4F54ull;

topology::TopologyActivationRequest clientRootActivationRequest(
    const uint16_t qspPort) {
    topology::TopologyActivationRequest request = activationRequest(qspPort);
    request.limits.maxTopologyNodes = 8u;
    request.limits.maxChildrenPerRoot = 1u;
    request.limits.standbyLeaseMs = 60000u;
    return request;
}

qsp::ViewerConfig clientRootViewerConfig(const uint64_t viewerId) {
    qsp::ViewerConfig config{};
    config.identity = sessionIdentity();
    config.simulationContract = qipc::kRuntimeCompatibilityId;
    config.compatibilityIdentity = compatibilityIdentity();
    config.secureRandom = viewerId;
    config.viewerNonce = viewerId;
    return config;
}

qnet::SpectateInfo clientRootLiveInfoForViewer(
    const uint16_t qspPort, const qsp::SpectatorGrant& grant) {
    qnet::SpectateInfo info = makeLiveInfoForViewer(qspPort, grant.viewerId,
                                                    grant);
    info.simulationContract = qipc::kRuntimeCompatibilityId;
    return info;
}

topology::client::ReleasedTopologyClientConfig clientRootRuntimeConfigFor(
    const qnet::Endpoint& hostControl, const qnet::SpectateInfo& e0,
    const qsp::SpectatorGrant& grant, const qnet::UdpSock& qspSocket,
    const qsp::SpectatorViewer& viewer, const uint16_t rootQspPort) {
    topology::client::ReleasedTopologyClientConfig config{};
    config.enabled = true;
    config.hostControl = hostControl;
    config.qspEndpoint = RouteEndpoint{loopbackIpv4BE(),
                                       qspSocket.boundPort()};
    config.rootQspEndpoint = RouteEndpoint{loopbackIpv4BE(), rootQspPort};
    config.qspRoute = grant.route;
    config.qspProvenance = grant.route == qnet::SPECTATE_ROUTE_PUNCHED
        ? topology::EndpointProvenance::ProvenPunchedQsp
        : topology::EndpointProvenance::ProvenDirectQsp;
    config.qspRouteGeneration = viewer.generation();
    config.viewerId = viewer.viewerId();
    config.viewerGeneration = viewer.generation();
    config.sessionId = kSessionId;
    config.sessionEpoch = kEpoch;
    config.spectatorCookieLow = e0.keyLow;
    config.spectatorCookieHigh = e0.keyHigh;
    config.compatibility = e0.compatibilityIdentity;
    config.streamRootIdentity = qsp::gameStartFingerprint(
        qsp::canonicalGameStart(clientRootDescriptor()));
    config.grant = grant;
    return config;
}

bool publishChainedRecordWith(SpectatorSidecar& sidecar, uint64_t& sequence,
                              const uint64_t now,
                              const qsp::GameDescriptor& descriptor,
                              const uint32_t index) {
    uint64_t digest = descriptor.initialDigest;
    qsp::ConfirmedRecord record{};
    for (uint32_t cursor = 0u; cursor <= index; ++cursor) {
        record = recordFor(cursor);
        record.rollingDigest = qsp::advanceRollingDigest(
            digest, descriptor.gameId, record);
        digest = record.rollingDigest;
    }
    PublicationEvent recordEvent{};
    recordEvent.kind = PublicationKind::Record;
    recordEvent.sequence = sequence++;
    recordEvent.record = record;
    sidecar.publish(recordEvent, now);
    PublicationEvent proof{};
    proof.kind = PublicationKind::TailProof;
    proof.sequence = sequence++;
    proof.tailRecordCursor = index + 1u;
    proof.postTailStateHash = stateHashFor(index + 1u);
    proof.hasPostTailProof = true;
    sidecar.publish(proof, now);
    return true;
}

bool clientRootCasterIntentValid(const qnet::ClientIntent& intent,
                                 const uint64_t viewerId,
                                 const uint16_t capabilities,
                                 const uint32_t expectedResumeRecord) {
    const qsp::SessionIdentity identity = sessionIdentity();
    return intent.intent == qnet::INTENT_SPECTATE
        && intent.spectatorRoute == qnet::SPECTATE_ROUTE_DIRECT
        && intent.spectatorCapabilities == capabilities
        && intent.sessionId == identity.sessionId
        && intent.keyLow == identity.cookie.low
        && intent.keyHigh == identity.cookie.high
        && intent.viewerId == viewerId
        && intent.resumeRecord == expectedResumeRecord
        && intent.reserved3 == 0u && intent.resumeDigest == 0u;
}

struct ClientRootCasterViewer {
    uint64_t viewerId = 0u;
    uint8_t challengeSeed = 0u;
    qsp::SpectatorGrant grant{};
    qnet::SpectateInfo live{};
    bool e0InfoSent = false;
    bool challengeSent = false;
    bool proofReceived = false;
    bool infoSent = false;
    uint64_t requestNonce = 0u;
    qnet::SpectateTopologyChallengeV2 challenge{};
    topology::TopologyViewerClaim claim{};
    qnet::UdpControlLink* topologyLink = nullptr;
};

enum class ClientRootCasterLane : uint8_t { Unknown, OrdinaryE0, TopologyV2 };

struct ClientRootCasterLink {
    qnet::UdpControlLink* link = nullptr;
    bool versionSeen = false;
    bool releaseAfterResponse = false;
    ClientRootCasterLane lane = ClientRootCasterLane::Unknown;
    ClientRootCasterViewer* viewer = nullptr;
};

bool clientRootCasterPump(SpectatorSidecar& host,
                          qnet::UdpControlListener& listener,
                          const std::array<ClientRootCasterViewer*, 2u>& viewers,
                          std::vector<ClientRootCasterLink>& links,
                          uint64_t& clock, std::string& lastEnrollError,
                          std::string& error) {
    listener.pump(0);
    for (;;) {
        qnet::UdpControlLink* const accepted = listener.acceptOne();
        if (accepted == nullptr) break;
        qnet::VersionConfig version{qnet::kProtocolVersion};
        if (!accepted->sendMsg(qnet::MSG_VERSION_CONFIG, &version,
                               sizeof(version))) {
            error = "could not send control protocol version";
            return false;
        }
        links.push_back(ClientRootCasterLink{accepted});
    }
    for (ClientRootCasterLink& state : links) {
        if (state.link == nullptr) continue;
        std::vector<qnet::ControlLink::Received> messages;
        if (!readLinkMessages(state.link, messages, error)) return false;
        for (const qnet::ControlLink::Received& rx : messages) {
            if (!state.versionSeen) {
                qnet::VersionConfig version{};
                if (rx.type != qnet::MSG_VERSION_CONFIG
                    || rx.body.size() != sizeof(version)) {
                    error = "caster child version was malformed";
                    return false;
                }
                std::memcpy(&version, rx.body.data(), sizeof(version));
                if (version.protocolVersion != qnet::kProtocolVersion) {
                    error = "caster child version mismatched";
                    return false;
                }
                state.versionSeen = true;
                continue;
            }
            if (state.lane == ClientRootCasterLane::Unknown) {
                qnet::ClientIntent intent{};
                if (rx.type != qnet::MSG_CLIENT_INTENT
                    || rx.body.size() != sizeof(intent)) {
                    error = "caster initial intent was malformed";
                    return false;
                }
                std::memcpy(&intent, rx.body.data(), sizeof(intent));
                ClientRootCasterViewer* viewer = nullptr;
                for (ClientRootCasterViewer* const candidate : viewers) {
                    if (candidate != nullptr
                        && candidate->viewerId == intent.viewerId) {
                        viewer = candidate;
                        break;
                    }
                }
                const bool topologyIntent = intent.spectatorCapabilities
                    == qnet::kSpectateCapabilityReleasedTopologyV2Mask;
                if (viewer == nullptr
                    || !clientRootCasterIntentValid(
                        intent, viewer->viewerId,
                        topologyIntent
                            ? qnet::kSpectateCapabilityReleasedTopologyV2Mask
                            : static_cast<uint16_t>(0u),
                        topologyIntent ? 1u : 0u)) {
                    error = "caster intent did not bind exact E0 facts";
                    return false;
                }
                state.viewer = viewer;
                if (!topologyIntent) {
                    if (viewer->e0InfoSent
                        || !state.link->sendMsg(qnet::MSG_SPECTATE_INFO,
                                                &viewer->live,
                                                sizeof(viewer->live))) {
                        error = "ordinary E0 grant response could not be sent";
                        return false;
                    }
                    viewer->e0InfoSent = true;
                    state.lane = ClientRootCasterLane::OrdinaryE0;
                    state.releaseAfterResponse = true;
                    continue;
                }
                if (viewer->infoSent || viewer->challengeSent) {
                    error = "duplicate topology control link";
                    return false;
                }
                viewer->challenge = {};
                viewer->challenge.schemaVersion =
                    qnet::kSpectateTopologyControlSchemaVersion;
                viewer->challenge.requiredFeatures =
                    qnet::kSpectateTopologyControlRequiredFeatures;
                viewer->challenge.sessionId = kSessionId;
                viewer->challenge.viewerId = viewer->viewerId;
                viewer->challenge.viewerGeneration = 1u;
                for (size_t index = 0u;
                     index < sizeof(viewer->challenge.peerChallenge);
                     ++index) {
                    viewer->challenge.peerChallenge[index] =
                        static_cast<uint8_t>(viewer->challengeSeed + index);
                }
                if (!state.link->sendMsg(
                        qnet::MSG_SPECTATE_TOPOLOGY_CHALLENGE_V2,
                        &viewer->challenge, sizeof(viewer->challenge))) {
                    error = "V2 topology challenge could not be sent";
                    return false;
                }
                viewer->challengeSent = true;
                viewer->topologyLink = state.link;
                state.lane = ClientRootCasterLane::TopologyV2;
                continue;
            }
            if (state.lane == ClientRootCasterLane::OrdinaryE0) {
                error = "ordinary E0 child sent unexpected follow-up control";
                return false;
            }
            if (state.viewer == nullptr
                || rx.type != qnet::MSG_SPECTATE_TOPOLOGY_PROOF_V2
                || rx.body.size() != sizeof(qnet::SpectateTopologyProofV2)) {
                error = "topology proof was malformed";
                return false;
            }
            if (state.viewer->infoSent) continue;
            qnet::SpectateTopologyProofV2 proof{};
            std::memcpy(&proof, rx.body.data(), sizeof(proof));
            qnet::ClientIntent proofIntent{};
            proofIntent.intent = qnet::INTENT_SPECTATE;
            proofIntent.spectatorRoute = qnet::SPECTATE_ROUTE_DIRECT;
            proofIntent.spectatorCapabilities =
                qnet::kSpectateCapabilityReleasedTopologyV2Mask;
            proofIntent.sessionId = static_cast<uint32_t>(kSessionId);
            proofIntent.keyLow = sessionIdentity().cookie.low;
            proofIntent.keyHigh = sessionIdentity().cookie.high;
            proofIntent.viewerId = state.viewer->viewerId;
            proofIntent.resumeRecord = 1u;
            qsp::SpectatorGrant proofGrant{};
            if (!controlv2::validateProof(proof, proofIntent,
                                          state.viewer->live,
                                          state.viewer->challenge, proofGrant)
                || !(proofGrant == state.viewer->grant)) {
                error = "production V2 verifier rejected a caster proof";
                return false;
            }
            if (state.viewer->proofReceived
                && proof.requestNonce != state.viewer->requestNonce) {
                error = "topology proof changed its request nonce";
                return false;
            }
            topology::TopologyViewerClaim claim{};
            claim.sessionId = proof.grant.sessionId;
            claim.sessionEpoch = proof.sessionEpoch;
            claim.viewerId = proof.viewerId;
            claim.viewerGeneration = proof.viewerGeneration;
            claim.qspEndpoint = RouteEndpoint{proof.qspIpv4BE, proof.qspPort};
            claim.qspProvenance =
                topology::EndpointProvenance::ProvenDirectQsp;
            claim.qspRouteGeneration = proof.qspRouteGeneration;
            claim.discoveryGeneration = proof.discoveryGeneration;
            claim.compatibility = proof.compatibilityIdentity;
            claim.streamRootIdentity = proof.streamRootIdentity;
            state.viewer->claim = claim;
            state.viewer->requestNonce = proof.requestNonce;
            state.viewer->proofReceived = true;
            std::memset(&proof, 0, sizeof(proof));
            std::memset(&proofGrant, 0, sizeof(proofGrant));
        }
    }
    for (ClientRootCasterLink& state : links) {
        if (!state.releaseAfterResponse || state.link == nullptr) continue;
        listener.release(state.link);
        state.link = nullptr;
    }
    for (ClientRootCasterViewer* const viewer : viewers) {
        if (viewer == nullptr || !viewer->proofReceived || viewer->infoSent)
            continue;
        topology::TopologyEnrollment enrollment{};
        std::string enrollError;
        if (!host.enrollTopologyViewerExact(viewer->claim, viewer->grant,
                                            ++clock, enrollment,
                                            enrollError)) {
            lastEnrollError = enrollError;
            continue;
        }
        qnet::SpectateTopologyInfoV2 info{};
        if (!controlv2::infoFromEnrollment(viewer->requestNonce, enrollment,
                                           info)
            || !controlv2::signInfo(info, viewer->grant)
            || viewer->topologyLink == nullptr
            || !viewer->topologyLink->sendMsg(
                qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2, &info, sizeof(info))) {
            host.rollbackTopologyViewer(viewer->viewerId);
            error = "signed V2 topology response could not be built/sent";
            return false;
        }
        viewer->infoSent = true;
    }
    return true;
}

bool runClientRootQualification() {
    NetScope network;
    if (!network.start()) {
        fail("client-root-q", "netInit failed");
        return false;
    }
    uint64_t clock = kInitialNowMs;
    uint64_t hostSequence = 1u;
    uint64_t privateSequence = 1u;
    std::string error;

    const qsp::GameDescriptor descriptor = clientRootDescriptor();
    SidecarConfig hostConfig = sidecarConfig();
    hostConfig.simulationContract = qipc::kRuntimeCompatibilityId;
    SpectatorSidecar host(hostConfig);
    if (!host.start(qclient::spectator::makeUdpSidecarTransport(), nullptr,
                    clock, error)
        || host.boundPort() == 0u) {
        fail("client-root-q", "host sidecar QSP bind failed");
        return false;
    }
    if (!publishStartWith(host, hostSequence, clock, descriptor)) {
        fail("client-root-q", "host sidecar rejected the descriptor");
        return false;
    }
    if (!host.configureTopologyAuthority(
            clientRootActivationRequest(host.boundPort()), ++clock, error)) {
        fail("client-root-q", "host QSR authority did not activate");
        return false;
    }
    if (!publishRecordWith(host, hostSequence, ++clock, descriptor)) {
        fail("client-root-q", "host sidecar rejected the confirmed record");
        return false;
    }
    const auto hostStatus = host.status();
    if (!hostStatus.topologyAuthorityReady
        || hostStatus.topologyQsrPort == 0u) {
        fail("client-root-q", "host QSR authority is not separately bound");
        return false;
    }

    SidecarConfig privateConfig = sidecarConfig();
    privateConfig.clientPrivateRoot.enabled = true;
    privateConfig.clientPrivateRoot.publisherGeneration = 17u;
    privateConfig.clientPrivateRoot.publisherNonce = 0x8A4C2E17061B3D55ull;
    privateConfig.maxViewers = 1u;
    privateConfig.policy = AdmissionPolicy::CodeOnly;
    privateConfig.signGrant = {};
    privateConfig.verifyGrant = {};
    privateConfig.simulationContract = qipc::kRuntimeCompatibilityId;
    SpectatorSidecar collector(privateConfig);
    if (!collector.start(qclient::spectator::makeUdpSidecarTransport(),
                         nullptr, clock, error)
        || collector.boundPort() == 0u) {
        fail("client-root-q", "client-private collector QSP bind failed");
        return false;
    }
    if (!collector.status().clientPrivateRootDormant) {
        fail("client-root-q", "collector did not start dormant");
        return false;
    }
    if (collector.boundPort() == host.boundPort()) {
        fail("client-root-q", "collector reused the host QSP port");
        return false;
    }
    if (!publishStartWith(collector, privateSequence, clock, descriptor)
        || !publishRecordWith(collector, privateSequence, clock,
                              descriptor)) {
        fail("client-root-q", "collector rejected the confirmed stream");
        return false;
    }

    qclient::spectator::ClientPrivateRootStreamFacts earlyFacts{};
    if (collector.clientPrivateRootStreamFacts(earlyFacts)
        || earlyFacts.available) {
        fail("client-root-q", "stream facts leaked before RuntimeApplied");
        return false;
    }
    {
        qclient::spectator::topology::ClientPlayerRootEnrollment refused{};
        std::vector<uint8_t> refusedGrant;
        qclient::spectator::topology::ClientPlayerRootClaim emptyClaim{};
        if (collector.enrollClientPlayerRoot(emptyClaim, clock, refused,
                                             refusedGrant, error)) {
            fail("client-root-q", "dormant collector minted an enrollment");
            return false;
        }
    }

    const qipc::IpcConfig launchConfig = clientPrivateLaunchConfig();
    qipc::SpectatorClientPublisherRuntimeAppliedReceipt receipt{};
    if (!qipc::buildSpectatorClientPublisherRuntimeAppliedReceipt(
            launchConfig, 4242u, receipt)) {
        const char* const why =
            qipc::validateSpectatorClientPrivatePublisher(launchConfig);
        std::fprintf(stderr, "[client-root-q] receipt build refused: %s\n",
                     why != nullptr ? why : "shape check failed");
        fail("client-root-q", "RuntimeApplied receipt build failed");
        return false;
    }
    if (!collector.confirmClientPrivateRuntimeApplied(launchConfig, receipt,
                                                      4242u, error)) {
        fail("client-root-q", "RuntimeApplied confirm failed");
        return false;
    }

    uint16_t clientRootQsrPort = 0u;
    if (!collector.prepareClientPlayerRootQsr(clientRootQsrPort, error)
        || clientRootQsrPort == 0u
        || clientRootQsrPort == collector.boundPort()) {
        fail("client-root-q", "client-root QSR socket not distinctly bound");
        return false;
    }

    qclient::spectator::ClientPrivateRootStreamFacts facts{};
    if (!collector.clientPrivateRootStreamFacts(facts) || !facts.available
        || facts.cacheNextCursor == 0u || facts.chainDigest == 0u
        || facts.collectorQspPort != collector.boundPort()
        || facts.clientRootQsrPort != clientRootQsrPort) {
        fail("client-root-q", "confirmed stream facts incomplete");
        return false;
    }

    qclient::spectator::topology::ClientPlayerRootClaim claim{};
    claim.sessionId = facts.sessionId;
    claim.sessionEpoch = facts.sessionEpoch;
    claim.remotePlayerRole = wire::ClientPlayerRootRole::RemotePlayerTwo;
    claim.compatibility = facts.compatibility;
    claim.configurationProvenance = facts.provenance;
    claim.streamRootIdentity = facts.streamRootIdentity;
    claim.gameStartFingerprint = facts.gameStartFingerprint;
    claim.publisherGeneration =
        static_cast<uint32_t>(facts.collectorGeneration);
    claim.publisherNonce = facts.publisherNonce;
    claim.runtimeReceipt.processId = facts.gameProcessId;
    claim.runtimeReceipt.publisherGeneration = claim.publisherGeneration;
    claim.runtimeReceipt.publisherNonce = claim.publisherNonce;
    claim.runtimeReceipt.playerSlot = facts.playerSlot;
    claim.runtimeReceipt.runtimeStage =
        qnetplay::config_provenance::Stage::RuntimeApplied;
    claim.runtimeReceipt.delay = facts.appliedDelay;
    claim.runtimeReceipt.rollback = facts.appliedRollback;
    claim.runtimeReceipt.remoteDelay = facts.appliedRemoteDelay;
    claim.runtimeReceipt.remoteRollback = facts.appliedRemoteRollback;
    claim.runtimeReceipt.provenance = facts.provenance;
    claim.sourceGeneration = 1u;
    claim.qspRouteGeneration = 1u;
    claim.qsrBindingGeneration = 1u;
    claim.dataRoute.kind = model::RouteKind::Direct;
    claim.dataRoute.ipv4BE = 0x0100007Fu;
    claim.dataRoute.port = facts.collectorQspPort;
    claim.dataRoute.generation = 1u;
    claim.dataRoute.routeProved = true;
    claim.qsrRoute.kind = model::RouteKind::Direct;
    claim.qsrRoute.ipv4BE = 0x0100007Fu;
    claim.qsrRoute.port = clientRootQsrPort;
    claim.qsrRoute.generation = 1u;
    claim.qsrRoute.routeProved = true;
    claim.cacheRange.firstCursor = facts.cacheFirstCursor;
    claim.cacheRange.nextCursor = facts.cacheNextCursor;
    claim.cursor.nextRecord = facts.cacheNextCursor;
    claim.cursor.nextEvent = facts.nextEvent;
    claim.cursor.digest = facts.chainDigest;
    claim.childCapacity = 1u;
    claim.expiresMs = clock + 600000u;

    for (unsigned tick = 0u; tick < 8u; ++tick) host.service(clock += 10u);

    qclient::spectator::topology::ClientPlayerRootEnrollment enrollment{};
    std::vector<uint8_t> encodedGrant;
    if (!host.enrollClientPlayerRoot(claim, ++clock, enrollment,
                                     encodedGrant, error)) {
        std::fprintf(stderr, "[client-root-q] frontier-claim refused: %s\n",
                     error.c_str());
        qclient::spectator::topology::ClientPlayerRootClaim probe = claim;
        probe.cacheRange = {0u, 0u};
        probe.cursor = {0u, 0u, descriptor.initialDigest};
        std::string probeError;
        qclient::spectator::topology::ClientPlayerRootEnrollment probeOut{};
        std::vector<uint8_t> probeGrant;
        if (host.enrollClientPlayerRoot(probe, ++clock, probeOut, probeGrant,
                                        probeError)) {
            std::fprintf(stderr,
                         "[client-root-q] PROBE: empty boundary accepted - "
                         "host verified cursor never advanced\n");
        } else {
            std::fprintf(stderr, "[client-root-q] PROBE also refused: %s\n",
                         probeError.c_str());
        }
        fail("client-root-q", "host enrollment refused");
        return false;
    }
    if (!enrollment.granted || encodedGrant.empty()) {
        fail("client-root-q", "host enrollment incomplete");
        return false;
    }

    qclient::spectator::ClientPlayerRootArmFacts armFacts{};
    armFacts.clientRootNodeId = enrollment.clientRootNodeId;
    armFacts.hostRootNodeId = enrollment.hostRootNodeId;
    armFacts.authorityGeneration = enrollment.authorityGeneration;
    armFacts.expiresMs = enrollment.expiresMs;
    qnet::ClientRootEnrollment wireEnrollment{};
    if (!qclient::spectator::topology::clientroot::enrollmentToWire(
            0xC0FFEEull, enrollment, encodedGrant.data(),
            encodedGrant.size(), wireEnrollment)) {
        fail("client-root-q", "enrollment wire serialization failed");
        return false;
    }
    armFacts.rootQsp = wireEnrollment.rootQsp;
    armFacts.rootQsr = wireEnrollment.rootQsr;
    armFacts.hostSourceGeneration = enrollment.hostSourceGeneration;
    std::copy_n(enrollment.assignmentSigningPublicKey.data(),
                armFacts.authorityPublicKey.size(),
                armFacts.authorityPublicKey.data());
    armFacts.credential = enrollment.credential;
    armFacts.grantBlob = encodedGrant;
    if (!collector.armClientPlayerRoot(armFacts, clock, error)) {
        fail("client-root-q", "arm refused");
        return false;
    }

    bool asserted = false;
    for (unsigned iteration = 0u; iteration < 400u && !asserted;
         ++iteration) {
        clock += 25u;
        host.service(clock);
        collector.service(clock);
        asserted = collector.status().clientRootAsserted;
        Sleep(1);
    }
    if (!asserted) {
        fail("client-root-q",
             "the authority never acknowledged the assertion");
        return false;
    }

    if (!collector.status().clientPrivateRootDormant
        || collector.status().clientRootServing) {
        fail("client-root-q", "root served without a signed commit");
        return false;
    }

    qnet::UdpSock controlSocket;
    if (!controlSocket.open(0u) || controlSocket.boundPort() == 0u) {
        fail("client-root-q", "could not open caster control socket");
        return false;
    }
    qnet::UdpControlListener listener;
    listener.open(controlSocket);
    const qnet::Endpoint casterControl =
        loopbackEndpoint(controlSocket.boundPort());
    const qnet::Endpoint hostQspEndpoint = loopbackEndpoint(host.boundPort());

    ClientRootCasterViewer anchorCaster{};
    anchorCaster.viewerId = kClientRootAnchorViewerId;
    anchorCaster.challengeSeed = 0xA5u;
    ClientRootCasterViewer targetCaster{};
    targetCaster.viewerId = kViewerId;
    targetCaster.challengeSeed = 0xB7u;
    const auto issueQualificationGrant =
        [&](ClientRootCasterViewer& viewer) {
            GrantRequest request{};
            request.viewerId = viewer.viewerId;
            request.route = qnet::SPECTATE_ROUTE_DIRECT;
            request.peerIpv4BE = loopbackIpv4BE();
            request.lifetimeSeconds = 300u;
            AdmissionDenyReason denied = AdmissionDenyReason::None;
            if (!host.issueGrant(request, viewer.grant, denied, ++clock)
                || viewer.grant.viewerId != viewer.viewerId
                || viewer.grant.route != qnet::SPECTATE_ROUTE_DIRECT) {
                return false;
            }
            viewer.live = clientRootLiveInfoForViewer(host.boundPort(),
                                                      viewer.grant);
            return true;
        };
    if (!issueQualificationGrant(anchorCaster)
        || !issueQualificationGrant(targetCaster)) {
        fail("client-root-q", "host could not issue the ordinary E0 grants");
        return false;
    }

    const std::array<ClientRootCasterViewer*, 2u> casterViewers{
        &anchorCaster, &targetCaster};
    std::vector<ClientRootCasterLink> casterLinks;
    std::string lastEnrollError;

    qnet::UdpSock anchorQsp;
    if (!anchorQsp.open(0u) || anchorQsp.boundPort() == 0u) {
        fail("client-root-q", "could not open the anchor QSP socket");
        return false;
    }
    qsp::SpectatorViewer anchorViewer(
        clientRootViewerConfig(kClientRootAnchorViewerId));
    if (!anchorViewer.isConfigured() || !anchorViewer.begin(kGameId)) {
        fail("client-root-q", "anchor viewer was not configured/begun");
        return false;
    }
    topology::client::ReleasedTopologyClientRuntime anchorRuntime;
    bool anchorRuntimeStarted = false;
    bool anchorInitAckSent = false;
    std::array<uint8_t, 4096u> anchorInbound{};
    const auto anchorUpkeep = [&](const uint64_t now) -> bool {
        for (unsigned budget = 0u; budget < 16u; ++budget) {
            qnet::Endpoint sender{};
            const int received = anchorQsp.recvFrom(
                anchorInbound.data(), anchorInbound.size(), sender);
            if (received == 0) break;
            if (received < 0) {
                fail("client-root-q", "anchor QSP receive failed");
                return false;
            }
            if (!(sender == hostQspEndpoint)) continue;
            if (!qspIngressAccepted(anchorViewer.onDatagram(
                    anchorInbound.data(), static_cast<size_t>(received),
                    now))) {
                fail("client-root-q", "anchor QSP ingress was rejected");
                return false;
            }
        }
        for (const qsp::ViewerDatagram& packet :
             anchorViewer.pollOutbound(now, 4u)) {
            if (!anchorQsp.sendTo(hostQspEndpoint, packet.bytes.data(),
                                  packet.bytes.size())) {
                fail("client-root-q", "anchor QSP send failed");
                return false;
            }
            if (packet.type == qsp::MessageType::InitAck
                && !anchorInitAckSent) {
                anchorInitAckSent = true;
                if (!anchorRuntime.start(
                        clientRootRuntimeConfigFor(
                            casterControl, anchorCaster.live,
                            anchorCaster.grant, anchorQsp, anchorViewer,
                            host.boundPort()),
                        now)
                    || !anchorRuntime.onQspInitAckTransmitted(now)
                    || !anchorRuntime.updateVerifiedQspFrontier(
                        anchorViewer.receivedNextRecord(),
                        anchorViewer.receivedNextEvent(),
                        anchorViewer.receivedDigest())) {
                    fail("client-root-q",
                         "anchor released runtime refused a real InitAck");
                    return false;
                }
                anchorRuntimeStarted = true;
            }
        }
        if (anchorRuntimeStarted && anchorRuntime.active()) {
            if (!anchorRuntime.updateVerifiedQspFrontier(
                    anchorViewer.receivedNextRecord(),
                    anchorViewer.receivedNextEvent(),
                    anchorViewer.receivedDigest())) {
                fail("client-root-q", "anchor rejected the QSP frontier");
                return false;
            }
            anchorRuntime.service(now);
            if (!anchorRuntime.active()) {
                fail("client-root-q",
                     std::string("anchor topology failed: ")
                         + anchorRuntime.status().reason);
                return false;
            }
        }
        return true;
    };
    const auto qualificationTurn = [&]() -> bool {
        ++clock;
        host.service(clock);
        collector.service(clock);
        if (!clientRootCasterPump(host, listener, casterViewers, casterLinks,
                                  clock, lastEnrollError, error)) {
            fail("client-root-q", error);
            return false;
        }
        if (!anchorUpkeep(nowMs())) return false;
        Sleep(1u);
        return true;
    };

    const uint64_t anchorStarted = nowMs();
    while (!anchorRuntime.attached()) {
        if (elapsed(anchorStarted, 10000u)) {
            fail("client-root-q",
                 std::string("anchor never attached to the host root; "
                             "last enroll refusal: ")
                     + lastEnrollError);
            return false;
        }
        if (!qualificationTurn()) return false;
    }
    {
        topology::client::ReleasedTopologyAttachmentSnapshot snapshot{};
        if (!anchorRuntime.attachmentSnapshot(snapshot) || !snapshot.valid()
            || snapshot.parentDataRoute.port != host.boundPort()
            || host.status().topologyAttached != 1u) {
            fail("client-root-q", "anchor attachment was not the host root");
            return false;
        }
    }

    {
        std::string anchorActivationError;
        if (host.issueClientPlayerRootActivationForViewer(
                qclient::spectator::TopologyViewerSelection{
                    kClientRootAnchorViewerId, 1u},
                ++clock, anchorActivationError)) {
            fail("client-root-q",
                 "an attached root child accepted a client-root activation");
            return false;
        }
    }

    HiddenProcess viewerChild;
    if (!viewerChild.launch(
            std::wstring(L"--client-root-viewer-child ")
                + std::to_wstring(host.boundPort()) + L" "
                + std::to_wstring(controlSocket.boundPort()),
            error)) {
        fail("client-root-q", error);
        return false;
    }

    bool activationIssued = false;
    std::string activationError;
    const uint64_t activationStarted = nowMs();
    while (!activationIssued) {
        if (elapsed(activationStarted, 12000u)) {
            fail("client-root-q",
                 std::string("client-root activation was never issuable: ")
                     + activationError + "; last enroll refusal: "
                     + lastEnrollError);
            return false;
        }
        if (!qualificationTurn()) return false;
        if (targetCaster.infoSent
            && host.issueClientPlayerRootActivationForViewer(
                qclient::spectator::TopologyViewerSelection{kViewerId, 1u},
                ++clock, activationError)) {
            activationIssued = true;
        }
    }

    const uint64_t servingStarted = nowMs();
    while (!collector.status().clientRootServing
           || !collector.status().clientRootCommitted
           || collector.status().clientPrivateRootDormant) {
        if (elapsed(servingStarted, 10000u)) {
            fail("client-root-q",
                 "the signed commit never armed client-root serving");
            return false;
        }
        if (!qualificationTurn()) return false;
    }

    if (!publishChainedRecordWith(collector, privateSequence, ++clock,
                                  descriptor, 1u)) {
        fail("client-root-q", "collector rejected the advancing record");
        return false;
    }

    bool viewerAdmitted = false;
    const uint64_t attachStarted = nowMs();
    for (;;) {
        if (elapsed(attachStarted, 12000u)) {
            fail("client-root-q",
                 viewerAdmitted
                     ? "the served viewer never detached/released capacity"
                     : "the activated viewer was never admitted by the root");
            return false;
        }
        if (!qualificationTurn()) return false;
        const size_t servedViewers = collector.status().viewerCount;
        if (servedViewers == 1u) viewerAdmitted = true;
        if (viewerAdmitted && servedViewers == 0u) break;
    }
    {
        DWORD exitCode = 1u;
        if (!viewerChild.waitForExit(kChildTimeoutMs, exitCode, error)) {
            fail("client-root-q", error);
            return false;
        }
        if (exitCode != 0u) {
            fail("client-root-q",
                 "released viewer child exited with code "
                     + std::to_string(exitCode));
            return false;
        }
    }

    {
        const auto probeDenied = [&](const uint64_t probeViewerId) -> bool {
            qnet::UdpSock probeSocket;
            if (!probeSocket.open(0u) || probeSocket.boundPort() == 0u) {
                fail("client-root-q", "could not open a probe QSP socket");
                return false;
            }
            qsp::SpectatorViewer probe(clientRootViewerConfig(probeViewerId));
            if (!probe.isConfigured() || !probe.begin(kGameId)) {
                fail("client-root-q", "probe viewer was not configured");
                return false;
            }
            const qnet::Endpoint collectorQsp =
                loopbackEndpoint(collector.boundPort());
            std::array<uint8_t, 4096u> probeInbound{};
            for (unsigned turn = 0u; turn < 40u; ++turn) {
                if (!qualificationTurn()) return false;
                for (const qsp::ViewerDatagram& packet :
                     probe.pollOutbound(nowMs(), 4u)) {
                    if (!probeSocket.sendTo(collectorQsp,
                                            packet.bytes.data(),
                                            packet.bytes.size())) {
                        fail("client-root-q", "probe QSP send failed");
                        return false;
                    }
                }
                for (unsigned budget = 0u; budget < 8u; ++budget) {
                    qnet::Endpoint sender{};
                    const int received = probeSocket.recvFrom(
                        probeInbound.data(), probeInbound.size(), sender);
                    if (received <= 0) break;
                    if (sender == collectorQsp) {
                        (void)probe.onDatagram(
                            probeInbound.data(),
                            static_cast<size_t>(received), nowMs());
                    }
                }
                if (probe.phase() == qsp::ViewerPhase::Streaming) {
                    fail("client-root-q",
                         "a non-activation-bound probe was served");
                    return false;
                }
            }
            if (collector.status().viewerCount != 0u) {
                fail("client-root-q",
                     "a denied probe still consumed collector capacity");
                return false;
            }
            return true;
        };
        if (!collector.status().clientRootServing) {
            fail("client-root-q",
                 "serving ended before the admission negatives");
            return false;
        }
        if (!probeDenied(0x505249564154453Full)) return false;
        if (!probeDenied(kViewerId)) return false;
    }

    host.rollbackTopologyViewer(kViewerId);
    const uint64_t retirementStarted = nowMs();
    while (collector.status().clientRootServing
           || collector.status().clientRootCommitted
           || !collector.status().clientPrivateRootDormant) {
        if (elapsed(retirementStarted, 10000u)) {
            fail("client-root-q",
                 "the signed retirement never withdrew client-root serving");
            return false;
        }
        if (!qualificationTurn()) return false;
    }

    {
        qnet::UdpSock probeSocket;
        qsp::SpectatorViewer probe(
            clientRootViewerConfig(0x505249564154453Full));
        if (!probeSocket.open(0u) || !probe.isConfigured()
            || !probe.begin(kGameId)) {
            fail("client-root-q", "dormancy probe was not configured");
            return false;
        }
        const qnet::Endpoint collectorQsp =
            loopbackEndpoint(collector.boundPort());
        std::array<uint8_t, 4096u> probeInbound{};
        for (unsigned turn = 0u; turn < 20u; ++turn) {
            if (!qualificationTurn()) return false;
            for (const qsp::ViewerDatagram& packet :
                 probe.pollOutbound(nowMs(), 4u)) {
                if (!probeSocket.sendTo(collectorQsp, packet.bytes.data(),
                                        packet.bytes.size())) {
                    fail("client-root-q", "dormancy probe send failed");
                    return false;
                }
            }
            for (unsigned budget = 0u; budget < 8u; ++budget) {
                qnet::Endpoint sender{};
                const int received = probeSocket.recvFrom(
                    probeInbound.data(), probeInbound.size(), sender);
                if (received <= 0) break;
                if (sender == collectorQsp) {
                    (void)probe.onDatagram(probeInbound.data(),
                                           static_cast<size_t>(received),
                                           nowMs());
                }
            }
            if (probe.phase() == qsp::ViewerPhase::Streaming
                || collector.status().viewerCount != 0u) {
                fail("client-root-q", "the dormant collector served a probe");
                return false;
            }
        }
    }

    collector.withdrawClientPlayerRoot("qualification teardown");
    if (collector.status().clientRootAsserted
        || collector.status().clientRootPrepared) {
        fail("client-root-q", "withdrawal did not clear control state");
        return false;
    }
    collector.withdrawClientPlayerRoot("second withdrawal is a no-op");

    if (!publishChainedRecordWith(collector, privateSequence, ++clock,
                                  descriptor, 2u)) {
        fail("client-root-q", "collector stopped after root withdrawal");
        return false;
    }

    {
        std::string probeError;
        if (host.issueClientPlayerRootActivationForViewer(
                qclient::spectator::TopologyViewerSelection{
                    kClientRootAnchorViewerId, 1u},
                ++clock, probeError)) {
            fail("client-root-q",
                 "an attached anchor became activatable pre-sweep");
            return false;
        }
        if (probeError == "no asserted client-player data root is available") {
            fail("client-root-q",
                 "the stale root assertion vanished before the sweep");
            return false;
        }
    }
    if (!publishChainedRecordWith(host, hostSequence, ++clock, descriptor,
                                  1u)) {
        fail("client-root-q", "host rejected its own frontier advance");
        return false;
    }
    for (unsigned tick = 0u; tick < 8u; ++tick) {
        if (!qualificationTurn()) return false;
    }
    {
        std::string probeError;
        if (host.issueClientPlayerRootActivationForViewer(
                qclient::spectator::TopologyViewerSelection{
                    kClientRootAnchorViewerId, 1u},
                ++clock, probeError)) {
            fail("client-root-q",
                 "a stale client root stayed activatable past the sweep");
            return false;
        }
        if (probeError != "no asserted client-player data root is available") {
            fail("client-root-q",
                 "the frontier advance did not revoke the stale assertion: "
                     + probeError);
            return false;
        }
    }

    uint16_t recoveredQsrPort = 0u;
    if (!collector.prepareClientPlayerRootQsr(recoveredQsrPort, error)
        || recoveredQsrPort == 0u) {
        fail("client-root-q", "client-root QSR could not be re-prepared");
        return false;
    }
    qclient::spectator::ClientPrivateRootStreamFacts recoveredFacts{};
    if (!collector.clientPrivateRootStreamFacts(recoveredFacts)
        || !recoveredFacts.available
        || recoveredFacts.cacheNextCursor < 3u
        || recoveredFacts.clientRootQsrPort != recoveredQsrPort) {
        fail("client-root-q", "post-withdrawal stream facts incomplete");
        return false;
    }
    uint64_t hostFrontierDigest = descriptor.initialDigest;
    for (uint32_t cursor = 0u; cursor < 2u; ++cursor) {
        qsp::ConfirmedRecord record = recordFor(cursor);
        record.rollingDigest = qsp::advanceRollingDigest(
            hostFrontierDigest, descriptor.gameId, record);
        hostFrontierDigest = record.rollingDigest;
    }
    qclient::spectator::topology::ClientPlayerRootClaim recoveryClaim = claim;
    recoveryClaim.qsrRoute.port = recoveredQsrPort;
    recoveryClaim.cacheRange.firstCursor = recoveredFacts.cacheFirstCursor;
    recoveryClaim.cacheRange.nextCursor = 2u;
    recoveryClaim.cursor.nextRecord = 2u;
    recoveryClaim.cursor.nextEvent = 0u;
    recoveryClaim.cursor.digest = hostFrontierDigest;
    recoveryClaim.expiresMs = clock + 600000u;
    for (unsigned tick = 0u; tick < 8u; ++tick) {
        if (!qualificationTurn()) return false;
    }
    qclient::spectator::topology::ClientPlayerRootEnrollment
        recoveryEnrollment{};
    std::vector<uint8_t> recoveryGrant;
    if (!host.enrollClientPlayerRoot(recoveryClaim, ++clock,
                                     recoveryEnrollment, recoveryGrant,
                                     error)) {
        fail("client-root-q",
             std::string("recovery claim refused: ") + error);
        return false;
    }
    if (!recoveryEnrollment.granted || recoveryGrant.empty()) {
        fail("client-root-q", "recovery enrollment incomplete");
        return false;
    }
    {
        qclient::spectator::ClientPlayerRootArmFacts recoveryArm{};
        recoveryArm.clientRootNodeId = recoveryEnrollment.clientRootNodeId;
        recoveryArm.hostRootNodeId = recoveryEnrollment.hostRootNodeId;
        recoveryArm.authorityGeneration =
            recoveryEnrollment.authorityGeneration;
        recoveryArm.expiresMs = recoveryEnrollment.expiresMs;
        qnet::ClientRootEnrollment recoveryWire{};
        if (!qclient::spectator::topology::clientroot::enrollmentToWire(
                0xC0FFEEull, recoveryEnrollment, recoveryGrant.data(),
                recoveryGrant.size(), recoveryWire)) {
            fail("client-root-q", "recovery enrollment wire failed");
            return false;
        }
        recoveryArm.rootQsp = recoveryWire.rootQsp;
        recoveryArm.rootQsr = recoveryWire.rootQsr;
        recoveryArm.hostSourceGeneration =
            recoveryEnrollment.hostSourceGeneration;
        std::copy_n(recoveryEnrollment.assignmentSigningPublicKey.data(),
                    recoveryArm.authorityPublicKey.size(),
                    recoveryArm.authorityPublicKey.data());
        recoveryArm.credential = recoveryEnrollment.credential;
        recoveryArm.grantBlob = recoveryGrant;
        if (!collector.armClientPlayerRoot(recoveryArm, clock, error)) {
            fail("client-root-q",
                 std::string("recovery arm refused: ") + error);
            return false;
        }
    }
    {
        const uint64_t recoveryStarted = nowMs();
        while (!collector.status().clientRootAsserted) {
            if (elapsed(recoveryStarted, 10000u)) {
                fail("client-root-q",
                     "the recovered root was never re-asserted");
                return false;
            }
            if (!qualificationTurn()) return false;
        }
    }
    collector.withdrawClientPlayerRoot("recovery teardown");
    if (collector.status().clientRootAsserted
        || collector.status().clientRootPrepared) {
        fail("client-root-q", "recovery withdrawal did not clear state");
        return false;
    }
    if (!publishChainedRecordWith(collector, privateSequence, ++clock,
                                  descriptor, 3u)) {
        fail("client-root-q", "collector stopped after recovery teardown");
        return false;
    }

    anchorRuntime.stop();
    (void)anchorViewer.detach();
    for (unsigned drain = 0u; drain < 8u; ++drain) {
        if (!qualificationTurn()) return false;
    }
    collector.stop(false, clock);
    host.stop(false, clock);
    std::printf("client-root qualification passed\n");
    return true;
}

int runClientRootViewerChild(const char* const qspPortText,
                             const char* const controlPortText) {
    uint16_t qspPort = 0u;
    uint16_t controlPort = 0u;
    if (!parsePort(qspPortText, qspPort)
        || !parsePort(controlPortText, controlPort)) {
        return 2;
    }
    NetScope network;
    if (!network.start()) return 3;

    const qnet::Endpoint rootQsp = loopbackEndpoint(qspPort);
    const qnet::Endpoint hostControl = loopbackEndpoint(controlPort);
    qnet::SpectateInfo e0{};
    std::string error;
    if (!waitForInitialInfo(hostControl, e0, error)) {
        fail("client-root-child", error);
        return 25;
    }
    qsp::SpectatorGrant grant{};
    if (!grantFromWire(e0.grant, grant)) {
        fail("client-root-child", "E0 grant was not an exact viewer grant");
        return 25;
    }

    qnet::UdpSock qspSocket;
    if (!qspSocket.open(0u) || qspSocket.boundPort() == 0u) {
        fail("client-root-child", "could not open the QSP viewer socket");
        return 26;
    }
    qsp::SpectatorViewer viewer(clientRootViewerConfig(kViewerId));
    if (!viewer.isConfigured() || !viewer.begin(kGameId)) {
        fail("client-root-child", "viewer was not configured/begun");
        return 26;
    }

    topology::client::ReleasedTopologyClientRuntime runtime;
    topology::client::ReleasedTopologyParentRouteActivation activation{};
    bool runtimeStarted = false;
    bool initAckSent = false;
    bool routeActivationArmed = false;
    bool attached = false;
    bool snapshotChecked = false;
    bool detachArmed = false;
    bool detachSent = false;
    qnet::Endpoint activeQsp = rootQsp;
    qnet::Endpoint candidateQsp{};
    std::array<uint8_t, 4096u> inbound{};
    const uint64_t started = nowMs();
    while (!elapsed(started, kPumpTimeoutMs)) {
        const uint64_t now = nowMs();
        for (unsigned budget = 0u; budget < 32u; ++budget) {
            qnet::Endpoint sender{};
            const int received = qspSocket.recvFrom(inbound.data(),
                                                    inbound.size(), sender);
            if (received == 0) break;
            if (received < 0) {
                fail("client-root-child", "QSP receive failed");
                return 27;
            }
            if (sender == activeQsp) {
                if (!qspIngressAccepted(viewer.onDatagram(
                        inbound.data(), static_cast<size_t>(received),
                        now))) {
                    fail("client-root-child",
                         "active QSP source datagram was rejected");
                    return 27;
                }
                continue;
            }
            if (routeActivationArmed && sender == candidateQsp) {
                const qsp::IngressResult ingress =
                    viewer.onValidatedParentRouteDatagram(
                        activation.activationNonce, inbound.data(),
                        static_cast<size_t>(received), now);
                if (ingress != qsp::IngressResult::Accepted) continue;
                if (!viewer.commitValidatedParentRouteIngress(
                        activation.activationNonce, now)
                    || !runtime.completeParentRouteActivationAfterIngress(
                        activation.activationNonce,
                        RouteEndpoint{sender.sa.sin_addr.s_addr,
                                      ntohs(sender.sa.sin_port)},
                        viewer.receivedNextRecord(),
                        viewer.receivedNextEvent(), viewer.receivedDigest(),
                        now)) {
                    fail("client-root-child",
                         "validated collector ingress did not complete the "
                         "client-root attachment");
                    return 28;
                }
                activeQsp = candidateQsp;
                routeActivationArmed = false;
                attached = true;
            }
        }

        for (const qsp::ViewerDatagram& packet :
             viewer.pollOutbound(now, 8u)) {
            qnet::Endpoint destination = activeQsp;
            if (packet.route
                == qsp::ViewerDatagramRoute::ValidatedPendingParent) {
                if (!routeActivationArmed
                    || packet.parentRouteActivationId
                        != activation.activationNonce) {
                    fail("client-root-child",
                         "candidate Hello escaped its activation");
                    return 29;
                }
                destination = candidateQsp;
            } else if (packet.route != qsp::ViewerDatagramRoute::ActiveParent
                       || packet.parentRouteActivationId != 0u) {
                fail("client-root-child", "unknown viewer route tag");
                return 29;
            }
            if (!qspSocket.sendTo(destination, packet.bytes.data(),
                                  packet.bytes.size())) {
                fail("client-root-child", "QSP send failed");
                return 29;
            }
            if (packet.type == qsp::MessageType::InitAck && !initAckSent) {
                initAckSent = true;
                if (!runtime.start(
                        clientRootRuntimeConfigFor(hostControl, e0, grant,
                                                   qspSocket, viewer,
                                                   qspPort),
                        now)
                    || !runtime.onQspInitAckTransmitted(now)
                    || !runtime.updateVerifiedQspFrontier(
                        viewer.receivedNextRecord(),
                        viewer.receivedNextEvent(),
                        viewer.receivedDigest())) {
                    fail("client-root-child",
                         "released runtime refused a real InitAck");
                    return 30;
                }
                runtimeStarted = true;
            }
            if (packet.type == qsp::MessageType::Detach) detachSent = true;
        }

        if (runtimeStarted && runtime.active()) {
            if (!runtime.updateVerifiedQspFrontier(
                    viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                    viewer.receivedDigest())) {
                fail("client-root-child", "runtime rejected the QSP frontier");
                return 30;
            }
            runtime.service(now);
        }
        const topology::client::ReleasedTopologyClientStatus status =
            runtime.status();
        if (status.state
            == topology::client::ReleasedTopologyClientState::Unavailable) {
            fail("client-root-child",
                 std::string("released topology failed: ") + status.reason);
            return 31;
        }
        if (runtimeStarted && !routeActivationArmed && !attached
            && status.state == topology::client::
                   ReleasedTopologyClientState::AwaitingRootOffer) {
            topology::client::ReleasedTopologyParentRouteActivation next{};
            if (runtime.parentRouteActivation(next, now)) {
                if (!next.valid()
                    || next.currentParentEndpoint.ipv4BE != loopbackIpv4BE()
                    || next.currentParentEndpoint.port != qspPort
                    || next.candidateDataEndpoint.ipv4BE != loopbackIpv4BE()
                    || next.candidateDataEndpoint.port == qspPort
                    || !viewer.armValidatedParentRouteTransition(
                        next.activationNonce, next.commonCursor.nextRecord,
                        next.commonCursor.nextEvent,
                        next.commonCursor.digest)) {
                    fail("client-root-child",
                         "the signed client-root candidate was not exact");
                    return 32;
                }
                activation = next;
                candidateQsp =
                    loopbackEndpoint(next.candidateDataEndpoint.port);
                routeActivationArmed = true;
            }
        }
        if (attached && !snapshotChecked) {
            topology::client::ReleasedTopologyAttachmentSnapshot snapshot{};
            if (!runtime.attachmentSnapshot(snapshot) || !snapshot.valid()
                || snapshot.parentDataRoute.ipv4BE != loopbackIpv4BE()
                || snapshot.parentDataRoute.port == qspPort
                || snapshot.parentDataRoute.port
                    != ntohs(candidateQsp.sa.sin_port)
                || snapshot.childBinding.childViewerId != kViewerId
                || std::strcmp(runtime.status().reason,
                               "attached-via-client-player-root-activation")
                    != 0) {
                fail("client-root-child",
                     "client-root attachment snapshot was not exact");
                return 33;
            }
            snapshotChecked = true;
        }
        if (attached && snapshotChecked && !detachArmed
            && viewer.receivedNextRecord() >= 2u) {
            if (!viewer.detach()) {
                fail("client-root-child",
                     "viewer refused detach after the served record");
                return 34;
            }
            detachArmed = true;
        }
        if (detachSent) {
            runtime.stop();
            return 0;
        }
        Sleep(1u);
    }
    fail("client-root-child",
         "timed out before client-root serving completion");
    return 35;
}

bool phaseCEncodeSignal(const PhaseCSignal& signal,
                        std::array<uint8_t, kPhaseCSignalBytes>& out) {
    out.fill(0u);
    const uint8_t raw = static_cast<uint8_t>(signal.type);
    if (raw < static_cast<uint8_t>(PhaseCSignalType::Hello)
        || raw > static_cast<uint8_t>(
            PhaseCSignalType::DownstreamTopologyRetiredE0Safe)) {
        return false;
    }
    phaseCWriteLe32(out.data(), kPhaseCSignalMagic);
    out[4] = raw;
    phaseCWriteLe32(out.data() + 8u, signal.value);
    phaseCWriteLe32(out.data() + 12u, signal.detail);
    return true;
}

bool phaseCDecodeSignal(const uint8_t* const bytes, const size_t length,
                        PhaseCSignal& out) {
    out = {};
    if (bytes == nullptr || length != kPhaseCSignalBytes
        || phaseCReadLe32(bytes) != kPhaseCSignalMagic
        || bytes[5] != 0u || bytes[6] != 0u || bytes[7] != 0u) {
        return false;
    }
    const uint8_t raw = bytes[4];
    if (raw < static_cast<uint8_t>(PhaseCSignalType::Hello)
        || raw > static_cast<uint8_t>(
            PhaseCSignalType::DownstreamTopologyRetiredE0Safe)) {
        return false;
    }
    out.type = static_cast<PhaseCSignalType>(raw);
    out.value = phaseCReadLe32(bytes + 8u);
    out.detail = phaseCReadLe32(bytes + 12u);
    return true;
}

bool phaseCSendSignal(qnet::UdpSock& socket, const qnet::Endpoint& to,
                      const PhaseCSignal& signal) {
    std::array<uint8_t, kPhaseCSignalBytes> bytes{};
    return phaseCEncodeSignal(signal, bytes)
        && socket.sendTo(to, bytes.data(), bytes.size());
}

bool phaseCDrainSignals(qnet::UdpSock& socket,
                        std::vector<std::pair<qnet::Endpoint, PhaseCSignal>>& out,
                        std::string& error) {
    std::array<uint8_t, kPhaseCSignalBytes> bytes{};
    for (unsigned budget = 0u; budget < 32u; ++budget) {
        qnet::Endpoint from{};
        const int received = socket.recvFrom(bytes.data(), bytes.size(), from);
        if (received == 0) return true;
        if (received < 0) {
            error = "Phase-C progress socket receive failed";
            return false;
        }
        PhaseCSignal signal{};
        if (phaseCDecodeSignal(bytes.data(), static_cast<size_t>(received),
                               signal)) {
            out.emplace_back(from, signal);
        }
    }
    return true;
}

bool phaseCGrantFromWire(const qnet::SpectatorGrantWire& wire,
                         const uint64_t expectedViewerId,
                         qsp::SpectatorGrant& grant) {
    for (const uint8_t byte : wire.reserved)
        if (byte != 0u) return false;
    if (wire.grantId == 0u || wire.viewerId != expectedViewerId
        || wire.sessionId != kSessionId || wire.expiryUnix == 0u
        || wire.sourceGeneration == 0u
        || (wire.route != qnet::SPECTATE_ROUTE_DIRECT
            && wire.route != qnet::SPECTATE_ROUTE_PUNCHED)
        || (wire.macLow == 0u && wire.macHigh == 0u)) {
        return false;
    }
    grant = {};
    grant.grantId = wire.grantId;
    grant.viewerId = wire.viewerId;
    grant.sessionId = wire.sessionId;
    grant.expiryUnix = wire.expiryUnix;
    grant.sourceGeneration = wire.sourceGeneration;
    grant.resumeRecord = wire.resumeRecord;
    grant.resumeDigest = wire.resumeDigest;
    grant.route = wire.route;
    grant.macLow = wire.macLow;
    grant.macHigh = wire.macHigh;
    return true;
}

bool phaseCWaitForInitialInfo(const qnet::Endpoint& host,
                              const uint64_t viewerId,
                              qnet::SpectateInfo& out,
                              std::string& error) {
    out = {};
    qnet::UdpSock socket;
    if (!socket.open(0u)) {
        error = "could not open Phase-C ordinary E0 control socket";
        return false;
    }
    qnet::UdpControlLink link;
    link.startClient(socket, host, nowMs());
    const uint64_t started = nowMs();
    bool sentVersion = false;
    while (!elapsed(started, 6000u)) {
        link.poll(0);
        if (!link.isOpen()) {
            error = "Phase-C ordinary E0 control link closed";
            return false;
        }
        for (unsigned budget = 0u; budget < 8u; ++budget) {
            qnet::ControlLink::Received rx{};
            const qnet::ControlLink::Rs result = link.recvMsgEx(rx, 0);
            if (result == qnet::ControlLink::Rs::Timeout) break;
            if (result != qnet::ControlLink::Rs::Msg) {
                error = "Phase-C ordinary E0 control receive failed";
                return false;
            }
            if (!sentVersion) {
                qnet::VersionConfig version{};
                if (rx.type != qnet::MSG_VERSION_CONFIG
                    || rx.body.size() != sizeof(version)) {
                    error = "Phase-C ordinary E0 host version was malformed";
                    return false;
                }
                std::memcpy(&version, rx.body.data(), sizeof(version));
                if (version.protocolVersion != qnet::kProtocolVersion) {
                    error = "Phase-C ordinary E0 host version mismatched";
                    return false;
                }
                qnet::VersionConfig local{qnet::kProtocolVersion};
                qnet::ClientIntent intent{};
                intent.intent = qnet::INTENT_SPECTATE;
                intent.spectatorRoute = qnet::SPECTATE_ROUTE_DIRECT;
                intent.sessionId = static_cast<uint32_t>(kSessionId);
                intent.keyLow = sessionIdentity().cookie.low;
                intent.keyHigh = sessionIdentity().cookie.high;
                intent.viewerId = viewerId;
                intent.resumeRecord = 0u;
                if (!link.sendMsg(qnet::MSG_VERSION_CONFIG, &local,
                                  sizeof(local))
                    || !link.sendMsg(qnet::MSG_CLIENT_INTENT, &intent,
                                     sizeof(intent))) {
                    error = "Phase-C ordinary E0 control could not send intent";
                    return false;
                }
                sentVersion = true;
                continue;
            }
            if (rx.type != qnet::MSG_SPECTATE_INFO
                || rx.body.size() != sizeof(out)) {
                error = "Phase-C ordinary E0 response was not SpectateInfo";
                return false;
            }
            std::memcpy(&out, rx.body.data(), sizeof(out));
            if (out.state != qnet::SPECTATE_LIVE
                || out.sessionId != kSessionId || out.spectatorPort == 0u
                || out.keyLow != sessionIdentity().cookie.low
                || out.keyHigh != sessionIdentity().cookie.high
                || out.admissionState != qnet::SPECTATE_ADMISSION_GRANTED
                || !qnetplay::compat::semanticallyEqual(
                    out.compatibilityIdentity, compatibilityIdentity())) {
                error = "Phase-C ordinary E0 response did not carry exact live facts";
                return false;
            }
            qsp::SpectatorGrant grant{};
            if (!phaseCGrantFromWire(out.grant, viewerId, grant)) {
                error = "Phase-C ordinary E0 grant was not exact";
                return false;
            }
            link.close();
            return true;
        }
        Sleep(1u);
    }
    error = "Phase-C ordinary E0 response timed out";
    return false;
}

qsp::ViewerConfig phaseCViewerConfig(const uint64_t viewerId) {
    qsp::ViewerConfig config{};
    config.identity = sessionIdentity();
    config.simulationContract = kSimulationContract;
    config.compatibilityIdentity = compatibilityIdentity();
    config.secureRandom = viewerId;
    config.viewerNonce = viewerId;
    return config;
}

topology::client::ReleasedTopologyClientConfig phaseCRuntimeConfig(
    const qnet::Endpoint& hostControl, const qnet::SpectateInfo& e0,
    const qsp::SpectatorGrant& grant, const qnet::UdpSock& qspSocket,
    const qsp::SpectatorViewer& viewer, const uint16_t rootQspPort) {
    topology::client::ReleasedTopologyClientConfig config{};
    config.enabled = true;
    config.hostControl = hostControl;
    config.qspEndpoint = RouteEndpoint{loopbackIpv4BE(), qspSocket.boundPort()};
    config.rootQspEndpoint = RouteEndpoint{loopbackIpv4BE(), rootQspPort};
    config.qspRoute = grant.route;
    config.qspProvenance = grant.route == qnet::SPECTATE_ROUTE_PUNCHED
        ? topology::EndpointProvenance::ProvenPunchedQsp
        : topology::EndpointProvenance::ProvenDirectQsp;
    config.qspRouteGeneration = viewer.generation();
    config.viewerId = viewer.viewerId();
    config.viewerGeneration = viewer.generation();
    config.sessionId = kSessionId;
    config.sessionEpoch = kEpoch;
    config.spectatorCookieLow = e0.keyLow;
    config.spectatorCookieHigh = e0.keyHigh;
    config.compatibility = e0.compatibilityIdentity;
    config.streamRootIdentity = qsp::gameStartFingerprint(
        qsp::canonicalGameStart(gameDescriptor()));
    config.grant = grant;
    return config;
}

bool phaseCParsePorts(const char* const qspText, const char* const controlText,
                      const char* const progressText, uint16_t& qspPort,
                      uint16_t& controlPort, uint16_t& progressPort) {
    return parsePort(qspText, qspPort) && parsePort(controlText, controlPort)
        && parsePort(progressText, progressPort);
}

bool phaseCRawSend(const SOCKET socket, const RouteEndpoint& destination,
                   const uint8_t* const bytes, const size_t length) {
    if (socket == INVALID_SOCKET || !destination.valid() || bytes == nullptr
        || length == 0u || length > static_cast<size_t>(INT_MAX)) {
        return false;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = destination.ipv4BE;
    to.sin_port = htons(destination.port);
    return ::sendto(socket, reinterpret_cast<const char*>(bytes),
                    static_cast<int>(length), 0,
                    reinterpret_cast<const sockaddr*>(&to), sizeof(to))
        == static_cast<int>(length);
}

class PhaseCResumedOrdinaryQspPeer {
public:
    bool begin(const qsp::SpectatorViewer& bootstrap,
               const uint32_t expectedRecordCursor,
               std::string& error) {
        _failure.clear();
        _active = false;
        _hasGameStart = false;
        _initAckSent = false;
        _recordReceived = false;
        _ackSent = false;
        _detachAuthorized = false;
        _detachSent = false;
        _receivedRecords.clear();
        _inboundSequences = {};
        _nextOutboundSequence = 1u;
        _lastHelloSent = 0u;
        _lastAckSent = 0u;

        if (!bootstrap.gameStart(_bootstrapStart)) {
            error = "ordinary relay child lost its proved bootstrap GameStart";
            return false;
        }
        _gameId = bootstrap.gameId();
        _viewerId = bootstrap.viewerId();
        _generation = bootstrap.generation();
        _viewerNonce = qsp::viewerGenerationNonce(_viewerId, _generation);
        _nextRecord = bootstrap.receivedNextRecord();
        _nextEvent = bootstrap.receivedNextEvent();
        _digest = bootstrap.receivedDigest();
        _initialRecord = _nextRecord;
        _initialDigest = _digest;
        _expectedStartFingerprint = qsp::gameStartFingerprint(_bootstrapStart);

        if (_gameId != kGameId || _viewerId != kPhaseCChildViewerId
            || _generation == 0u || _viewerNonce == 0u
            || _nextRecord != expectedRecordCursor
            || _nextEvent != 0u || _digest == 0u
            || _expectedStartFingerprint == 0u) {
            error = "ordinary relay child bootstrap facts did not match its frozen assignment boundary";
            return false;
        }
        _active = true;
        return true;
    }

    bool active() const { return _active && _failure.empty(); }
    bool streaming() const { return _recordReceived && _ackSent; }
    bool detachSent() const { return _detachSent; }
    uint32_t receivedNextRecord() const { return _nextRecord; }
    const std::string& failure() const { return _failure; }

    void authorizeDetach() { _detachAuthorized = true; }

    std::vector<qsp::ViewerDatagram> pollOutbound(const uint64_t now) {
        std::vector<qsp::ViewerDatagram> out;
        if (!active()) return out;

        if (!_hasGameStart) {
            if (_lastHelloSent == 0u || now - _lastHelloSent >= 200u) {
                qsp::HelloPayload hello{};
                hello.simulationContract = kSimulationContract;
                hello.compatibilityIdentity = compatibilityIdentity();
                hello.requestedGameId = _gameId;
                hello.viewerNonce = _viewerNonce;
                hello.viewerId = _viewerId;
                hello.resumeDigest = _digest;
                hello.resumeRecord = _nextRecord;
                hello.generation = _generation;
                hello.resumeEvent = _nextEvent;
                if (!append(qsp::MessageType::Hello, hello, out)) return {};
                _lastHelloSent = now;
            }
            return out;
        }

        if (!_initAckSent) {
            qsp::InitAckPayload init{};
            init.gameId = _gameId;
            init.viewerNonce = _viewerNonce;
            init.startFingerprint = _expectedStartFingerprint;
            init.resumeDigest = _initialDigest;
            init.viewerId = _viewerId;
            init.resumeRecord = _initialRecord;
            init.generation = _generation;
            init.resumeEvent = _nextEvent;
            if (!append(qsp::MessageType::InitAck, init, out)) return {};
            _initAckSent = true;
            return out;
        }

        if (_recordReceived
            && (!_ackSent || now - _lastAckSent >= 100u)) {
            qsp::AckPayload ack{};
            ack.gameId = _gameId;
            ack.digest = _digest;
            ack.viewerId = _viewerId;
            ack.nextRecord = _nextRecord;
            ack.generation = _generation;
            ack.nextEvent = _nextEvent;
            if (!append(qsp::MessageType::Ack, ack, out)) return {};
            _ackSent = true;
            _lastAckSent = now;
            return out;
        }

        if (_ackSent && _detachAuthorized && !_detachSent) {
            qsp::DetachPayload detach{};
            detach.viewerId = _viewerId;
            detach.digest = _digest;
            detach.nextRecord = _nextRecord;
            detach.generation = _generation;
            detach.nextEvent = _nextEvent;
            if (!append(qsp::MessageType::Detach, detach, out)) return {};
            _detachSent = true;
        }
        return out;
    }

    bool onDatagram(const uint8_t* const bytes, const size_t length,
                    std::string& error) {
        if (!active()) {
            error = _failure.empty()
                ? "ordinary relay child was not active"
                : _failure;
            return false;
        }
        qsp::Datagram datagram{};
        if (qsp::decodeDatagram(bytes, length, sessionIdentity(), datagram)
                != qsp::DecodeError::None
            || datagram.header.direction != qsp::Direction::SourceToViewer
            || datagram.header.senderRole != qsp::PeerRole::Source
            || !datagram.header.compatibilityIdentityAttested
            || !qsp::sequenceWindowAdmit(_inboundSequences,
                                         datagram.header.sequence)) {
            return failPeer("ordinary relay child received an invalid source QSP datagram",
                            error);
        }

        switch (datagram.header.type) {
        case qsp::MessageType::GameStart:
            return receiveGameStart(datagram.payload, error);
        case qsp::MessageType::ConfirmedInputBatch:
            return receiveRecordBatch(datagram.payload, error);
        case qsp::MessageType::Heartbeat:
            return receiveHeartbeat(datagram.payload, error);
        case qsp::MessageType::Reject: {
            qsp::RejectPayload reject{};
            if (!qsp::decodePayload(datagram.payload, reject)
                || reject.viewerId != _viewerId
                || reject.generation != _generation
                || reject.viewerNonce != _viewerNonce) {
                return failPeer("ordinary relay child received a malformed reject",
                                error);
            }
            return failPeer("ordinary relay child was rejected by S1 (reason "
                            + std::to_string(static_cast<unsigned>(reject.reason))
                            + ")", error);
        }
        case qsp::MessageType::SetEvent:
            return failPeer("ordinary relay child received an unexpected set event",
                            error);
        case qsp::MessageType::Terminal:
            return failPeer("ordinary relay child received an unexpected terminal",
                            error);
        case qsp::MessageType::CheckpointOffer:
        case qsp::MessageType::CheckpointChunk:
            return failPeer("ordinary relay child received an unexpected checkpoint packet",
                            error);
        case qsp::MessageType::Hello:
        case qsp::MessageType::InitAck:
        case qsp::MessageType::Ack:
        case qsp::MessageType::TerminalAck:
        case qsp::MessageType::Detach:
        case qsp::MessageType::Migrate:
            return failPeer("ordinary relay child received a wrong-direction QSP packet",
                            error);
        }
        return failPeer("ordinary relay child received an unknown QSP packet",
                        error);
    }

private:
    template <typename Payload>
    bool append(const qsp::MessageType type, const Payload& payload,
                std::vector<qsp::ViewerDatagram>& out) {
        std::vector<uint8_t> encodedPayload;
        qsp::ViewerDatagram packet{};
        packet.type = type;
        packet.sequence = _nextOutboundSequence;
        if (!qsp::encodePayload(payload, encodedPayload)
            || !qsp::encodeDatagram(type, sessionIdentity(), packet.sequence,
                                    encodedPayload, packet.bytes)) {
            _failure = "ordinary relay child could not encode its QSP packet";
            return false;
        }
        ++_nextOutboundSequence;
        out.push_back(std::move(packet));
        return true;
    }

    bool failPeer(const std::string& detail, std::string& error) {
        _failure = detail;
        error = _failure;
        return false;
    }

    bool receiveGameStart(const std::vector<uint8_t>& payload,
                          std::string& error) {
        qsp::GameStartPayload start{};
        if (!qsp::decodePayload(payload, start)
            || start.gameId != _gameId
            || start.simulationContract != kSimulationContract
            || !qnetplay::compat::semanticallyEqual(
                start.compatibilityIdentity, compatibilityIdentity())
            || start.viewerId != _viewerId
            || start.generation != _generation
            || start.viewerNonce != _viewerNonce
            || start.resumeRecord != _initialRecord
            || start.resumeEvent != _nextEvent
            || start.resumeDigest != _initialDigest
            || start.initialDigest != _bootstrapStart.initialDigest
            || qsp::gameStartFingerprint(start) != _expectedStartFingerprint) {
            return failPeer("ordinary relay child received a mismatched S1 GameStart",
                            error);
        }
        _hasGameStart = true;
        return true;
    }

    bool receiveRecordBatch(const std::vector<uint8_t>& payload,
                            std::string& error) {
        qsp::ConfirmedInputBatchPayload batch{};
        if (!_initAckSent || !qsp::decodePayload(payload, batch)
            || batch.gameId != _gameId || batch.records.empty()
            || batch.baseEventIndex != _nextEvent
            || batch.startRecord < _initialRecord
            || batch.startRecord > _nextRecord) {
            return failPeer("ordinary relay child received an invalid S1 record batch",
                            error);
        }

        uint64_t expectedBase = _initialDigest;
        if (batch.startRecord > _initialRecord) {
            const uint64_t prior = static_cast<uint64_t>(batch.startRecord)
                - static_cast<uint64_t>(_initialRecord) - 1u;
            if (prior >= _receivedRecords.size()) {
                return failPeer("ordinary relay child could not prove a record-batch base",
                                error);
            }
            expectedBase = _receivedRecords[static_cast<size_t>(prior)].rollingDigest;
        }
        if (batch.baseDigest != expectedBase) {
            return failPeer("ordinary relay child record batch had a mismatched base digest",
                            error);
        }

        uint64_t batchDigest = batch.baseDigest;
        for (const qsp::ConfirmedRecord& record : batch.records) {
            if (record.record < _nextRecord) {
                const uint64_t offset = static_cast<uint64_t>(record.record)
                    - static_cast<uint64_t>(_initialRecord);
                if (offset >= _receivedRecords.size()
                    || _receivedRecords[static_cast<size_t>(offset)] != record) {
                    return failPeer("ordinary relay child record retransmit conflicted",
                                    error);
                }
            } else {
                if (record.record != _nextRecord
                    || _bootstrapStart.firstTimelineFrame
                        > (std::numeric_limits<uint32_t>::max)()
                            - record.record
                    || record.timelineFrame
                        != _bootstrapStart.firstTimelineFrame + record.record
                    || record.rollingDigest != qsp::advanceRollingDigest(
                        batchDigest, _gameId, record)) {
                    return failPeer("ordinary relay child record chain validation failed",
                                    error);
                }
                _receivedRecords.push_back(record);
                ++_nextRecord;
                _digest = record.rollingDigest;
                _recordReceived = true;
            }
            batchDigest = record.rollingDigest;
        }
        if (batch.endDigest != batchDigest) {
            return failPeer("ordinary relay child record batch had a mismatched end digest",
                            error);
        }
        return true;
    }

    bool receiveHeartbeat(const std::vector<uint8_t>& payload,
                          std::string& error) {
        qsp::HeartbeatPayload heartbeat{};
        if (!_hasGameStart || !qsp::decodePayload(payload, heartbeat)
            || heartbeat.gameId != _gameId
            || heartbeat.nextRecord < _nextRecord
            || heartbeat.nextEvent < _nextEvent
            || (heartbeat.nextRecord == _nextRecord
                && heartbeat.nextEvent == _nextEvent
                && heartbeat.digest != _digest)) {
            return failPeer("ordinary relay child received an invalid S1 heartbeat",
                            error);
        }
        return true;
    }

    bool _active = false;
    bool _hasGameStart = false;
    bool _initAckSent = false;
    bool _recordReceived = false;
    bool _ackSent = false;
    bool _detachAuthorized = false;
    bool _detachSent = false;
    uint64_t _gameId = 0u;
    uint64_t _viewerId = 0u;
    uint64_t _viewerNonce = 0u;
    uint64_t _digest = 0u;
    uint64_t _initialDigest = 0u;
    uint64_t _expectedStartFingerprint = 0u;
    uint32_t _generation = 0u;
    uint32_t _nextRecord = 0u;
    uint32_t _nextEvent = 0u;
    uint32_t _initialRecord = 0u;
    uint32_t _nextOutboundSequence = 1u;
    uint64_t _lastHelloSent = 0u;
    uint64_t _lastAckSent = 0u;
    qsp::ReplayWindow _inboundSequences{};
    qsp::GameStartPayload _bootstrapStart{};
    std::vector<qsp::ConfirmedRecord> _receivedRecords;
    std::string _failure;
};

int runPhaseDIntermediateChild(const char* const qspPortText,
                               const char* const controlPortText,
                               const char* const progressPortText,
                               const bool completedS2RetirementValidation = false) {
    uint16_t qspPort = 0u;
    uint16_t controlPort = 0u;
    uint16_t progressPort = 0u;
    if (!phaseCParsePorts(qspPortText, controlPortText, progressPortText,
                          qspPort, controlPort, progressPort)) {
        return 2;
    }
    const uint32_t initialRunwayRecords = phaseCInitialRunwayRecords();
    if (initialRunwayRecords == 0u) {
        fail("phase-d-s2", "default resilient cache runway is not representable");
        return 3;
    }
    NetScope network;
    if (!network.start()) return 4;

    const qnet::Endpoint rootQsp = loopbackEndpoint(qspPort);
    const qnet::Endpoint hostControl = loopbackEndpoint(controlPort);
    const qnet::Endpoint progressHost = loopbackEndpoint(progressPort);
    qnet::SpectateInfo e0{};
    std::string error;
    if (!phaseCWaitForInitialInfo(hostControl, kPhaseCChildViewerId, e0,
                                  error)) {
        fail("phase-d-s2", error);
        return 5;
    }
    qsp::SpectatorGrant grant{};
    if (!phaseCGrantFromWire(e0.grant, kPhaseCChildViewerId, grant)) {
        fail("phase-d-s2", "ordinary E0 grant did not bind S2");
        return 6;
    }
    qnet::UdpSock qspSocket;
    qnet::UdpSock progressSocket;
    if (!qspSocket.open(0u) || qspSocket.boundPort() == 0u
        || !progressSocket.open(0u)) {
        fail("phase-d-s2", "could not open S2 UDP sockets");
        return 7;
    }
    const uint16_t enrolledQspPort = qspSocket.boundPort();
    if (!phaseCSendSignal(progressSocket, progressHost,
                          {PhaseCSignalType::Hello,
                           static_cast<uint32_t>(PhaseCRole::Downstream), 0u})) {
        fail("phase-d-s2", "could not publish S2 process readiness");
        return 8;
    }

    qsp::SpectatorViewer viewer(phaseCViewerConfig(kPhaseCChildViewerId));
    if (!viewer.isConfigured() || !viewer.begin(kGameId)) {
        fail("phase-d-s2", "S2 bootstrap viewer was not configured/begun");
        return 9;
    }
    topology::client::ReleasedTopologyClientRuntime runtime;
    bool topologyStarted = false;
    bool bootstrapFrozen = false;
    bool routeActivationArmed = false;
    topology::client::ReleasedTopologyParentRouteActivation activation{};
    qnet::Endpoint activeQsp = rootQsp;
    qnet::Endpoint candidateQsp{};
    bool attached = false;
    bool relayBindingStarted = false;
    bool relayServing = false;
    uint16_t relayDataPort = 0u;
    bool grandchildHelloAccepted = false;
    bool grandchildAckAccepted = false;
    bool grandchildDetachAccepted = false;
    bool detachRequestedByParent = false;
    bool upstreamDetachRequested = false;
    bool upstreamDetachSent = false;
    uint64_t detachSentAt = 0u;
    uint64_t lastBootstrapSignal = 0u;
    uint64_t lastAttachmentSignal = 0u;
    uint64_t lastRelayReadySignal = 0u;
    uint64_t lastChildHelloSignal = 0u;
    uint64_t lastChildAckSignal = 0u;
    uint64_t lastChildDetachSignal = 0u;
    uint64_t lastStreamingSignal = 0u;
    uint64_t lastDetachSignal = 0u;
    uint64_t lastSourceRecordSignal = 0u;
    bool completedS2RetirementObserved = false;
    uint64_t completedS2RetirementObservedAt = 0u;
    uint64_t lastCompletedS2RetirementSignal = 0u;
    std::array<uint8_t, 4096u> inbound{};
    const uint64_t started = nowMs();
    while (!elapsed(started, kPhaseCPumpTimeoutMs)) {
        const uint64_t now = nowMs();
        std::vector<std::pair<qnet::Endpoint, PhaseCSignal>> signals;
        if (!phaseCDrainSignals(progressSocket, signals, error)) {
            fail("phase-d-s2", error);
            return 10;
        }
        for (const auto& item : signals) {
            if (!(item.first == progressHost)) continue;
            if (item.second.type == PhaseCSignalType::RelayAckObserved)
                detachRequestedByParent = true;
        }

        for (unsigned budget = 0u; budget < 32u; ++budget) {
            qnet::Endpoint sender{};
            const int received = qspSocket.recvFrom(inbound.data(), inbound.size(),
                                                     sender);
            if (received == 0) break;
            if (received < 0) {
                fail("phase-d-s2", "S2 QSP receive failed");
                return 11;
            }
            if (sender == activeQsp) {
                if (!qspIngressAccepted(viewer.onDatagram(
                        inbound.data(), static_cast<size_t>(received), now))) {
                    fail("phase-d-s2", "active S2 QSP source datagram was rejected");
                    return 12;
                }
                continue;
            }
            if (routeActivationArmed && sender == candidateQsp) {
                const qsp::IngressResult ingress =
                    viewer.onValidatedParentRouteDatagram(
                        activation.activationNonce, inbound.data(),
                        static_cast<size_t>(received), now);
                if (ingress != qsp::IngressResult::Accepted) {
                    continue;
                }
                if (!viewer.commitValidatedParentRouteIngress(
                        activation.activationNonce, now)
                    || !runtime.completeParentRouteActivationAfterIngress(
                        activation.activationNonce,
                        RouteEndpoint{sender.sa.sin_addr.s_addr,
                                      ntohs(sender.sa.sin_port)},
                        viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                        viewer.receivedDigest(), now)) {
                    fail("phase-d-s2",
                         "same-generation S1 ingress did not complete S2 attachment");
                    return 13;
                }
                activeQsp = candidateQsp;
                routeActivationArmed = false;
                attached = true;
            }
        }

        for (const qsp::ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
            qnet::Endpoint destination = activeQsp;
            if (packet.route == qsp::ViewerDatagramRoute::ValidatedPendingParent) {
                if (!routeActivationArmed
                    || packet.parentRouteActivationId != activation.activationNonce) {
                    fail("phase-d-s2", "stale candidate Hello escaped its activation");
                    return 14;
                }
                destination = candidateQsp;
            } else if (packet.route != qsp::ViewerDatagramRoute::ActiveParent
                       || packet.parentRouteActivationId != 0u) {
                fail("phase-d-s2", "unknown viewer route tag");
                return 15;
            }
            if (!qspSocket.sendTo(destination, packet.bytes.data(),
                                  packet.bytes.size())) {
                fail("phase-d-s2", "S2 QSP send failed");
                return 16;
            }
            if (packet.type == qsp::MessageType::InitAck && !topologyStarted) {
                if (!runtime.start(phaseCRuntimeConfig(
                        hostControl, e0, grant, qspSocket, viewer, qspPort), now)
                    || !runtime.onQspInitAckTransmitted(now)
                    || !runtime.updateVerifiedQspFrontier(
                        viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                        viewer.receivedDigest())) {
                    fail("phase-d-s2", "released S2 runtime rejected real InitAck");
                    return 17;
                }
                topologyStarted = true;
            }
            if (packet.type == qsp::MessageType::Detach && destination == activeQsp) {
                upstreamDetachSent = true;
                if (detachSentAt == 0u) detachSentAt = now;
            }
        }

        if (topologyStarted && runtime.active()) {
            if (!runtime.updateVerifiedQspFrontier(
                    viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                    viewer.receivedDigest())) {
                fail("phase-d-s2", "released S2 rejected verified QSP frontier");
                return 18;
            }
            runtime.service(now);
            if (!runtime.active()) {
                if (completedS2RetirementValidation
                    && runtime.status().state
                        == topology::client::ReleasedTopologyClientState::Unavailable
                    && std::strcmp(runtime.status().reason,
                                   "topology-parent-route-activation-retired") == 0
                    && viewer.phase() == qsp::ViewerPhase::Streaming
                    && viewer.receivedNextRecord()
                        >= initialRunwayRecords + 1u) {
                    completedS2RetirementObserved = true;
                    if (completedS2RetirementObservedAt == 0u)
                        completedS2RetirementObservedAt = now;
                } else {
                    fail("phase-d-s2", std::string("released S2 topology failed: ")
                        + runtime.status().reason);
                    return 19;
                }
            }
            if (!runtime.active() && completedS2RetirementObserved) {
            } else if (!runtime.active()) {
                fail("phase-d-s2", std::string("released S2 topology failed: ")
                    + runtime.status().reason);
                return 19;
            }
            if (runtime.status().state
                == topology::client::ReleasedTopologyClientState::AwaitingRootOffer) {
                bootstrapFrozen = true;
            }
            if (bootstrapFrozen && !routeActivationArmed && !attached) {
                topology::client::ReleasedTopologyParentRouteActivation next{};
                if (runtime.parentRouteActivation(next, now)) {
                    if (!next.valid()
                        || next.currentParentEndpoint.ipv4BE != loopbackIpv4BE()
                        || next.currentParentEndpoint.port != qspPort
                        || next.candidateDataEndpoint.ipv4BE != loopbackIpv4BE()
                        || next.candidateDataEndpoint.port == qspPort
                        || !viewer.armValidatedParentRouteTransition(
                            next.activationNonce, next.commonCursor.nextRecord,
                            next.commonCursor.nextEvent,
                            next.commonCursor.digest)) {
                        fail("phase-d-s2",
                             "S2 rejected the exact signed candidate handoff");
                        return 20;
                    }
                    activation = next;
                    candidateQsp = loopbackEndpoint(next.candidateDataEndpoint.port);
                    routeActivationArmed = true;
                }
            }
        }

        if (runtime.attached() && !attached) {
            fail("phase-d-s2", "S2 attached without candidate ingress commit");
            return 21;
        }
        if (attached && !relayBindingStarted) {
            qnet::UdpSock relayOwner;
            if (!relayOwner.open(0u) || relayOwner.boundPort() == 0u) {
                fail("phase-d-s2", "could not create S2 relay data socket");
                return 22;
            }
            relayDataPort = relayOwner.boundPort();
            uintptr_t owned = static_cast<uintptr_t>(relayOwner.release());
            if (!runtime.beginRelayDataBinding(owned, now)
                || owned != topology::client::kInvalidReleasedTopologyRelaySocket) {
                if (owned != topology::client::kInvalidReleasedTopologyRelaySocket)
                    closesocket(static_cast<SOCKET>(owned));
                fail("phase-d-s2", "released S2 did not adopt exact relay socket");
                return 23;
            }
            relayBindingStarted = true;
        }
        if (relayBindingStarted
            && runtime.relayDataBindingState()
                == topology::client::ReleasedTopologyRelayBindingState::Ready
            && !relayServing) {
            if (!runtime.startRelayNode(viewer, now)
                || runtime.projectRelayViewer(viewer, now)
                    != qclient::spectator::resilient::NodeRuntimeResult::Accepted
                || !runtime.relayNodeRunning()) {
                fail("phase-d-s2", "released S2 relay facade did not start");
                return 24;
            }
            topology::client::ReleasedTopologyRelayBindingSnapshot binding{};
            if (!runtime.relayDataBindingSnapshot(binding) || !binding.valid()
                || binding.observedDataRoute.port != relayDataPort) {
                fail("phase-d-s2", "host did not acknowledge S2's exact relay socket");
                return 25;
            }
            relayServing = true;
        }
        if (relayServing && !upstreamDetachRequested
            && !completedS2RetirementObserved) {
            if (runtime.projectRelayViewer(viewer, now)
                != qclient::spectator::resilient::NodeRuntimeResult::Accepted) {
                fail("phase-d-s2", "S2 canonical relay projection stopped");
                return 26;
            }
            const auto borrowed = runtime.borrowedRelayDataSocket();
            if (!borrowed.valid()) {
                fail("phase-d-s2", "S2 relay socket borrow became invalid");
                return 27;
            }
            const SOCKET dataSocket = static_cast<SOCKET>(borrowed.socket);
            for (unsigned budget = 0u; budget < 32u; ++budget) {
                sockaddr_in rawSender{};
                int rawSenderLength = sizeof(rawSender);
                const int received = ::recvfrom(
                    dataSocket, reinterpret_cast<char*>(inbound.data()),
                    static_cast<int>(inbound.size()), 0,
                    reinterpret_cast<sockaddr*>(&rawSender), &rawSenderLength);
                if (received == SOCKET_ERROR) {
                    const int wsa = WSAGetLastError();
                    if (wsa == WSAEWOULDBLOCK) break;
                    fail("phase-d-s2", "S2 relay socket receive failed");
                    return 28;
                }
                if (received <= 0) continue;
                qsp::Datagram decoded{};
                const bool decodedQsp = qsp::decodeDatagram(
                    inbound.data(), static_cast<size_t>(received),
                    sessionIdentity(), decoded) == qsp::DecodeError::None;
                const auto result = runtime.ingestRelayChildDatagram(
                    RouteEndpoint{rawSender.sin_addr.s_addr,
                                  ntohs(rawSender.sin_port)},
                    inbound.data(), static_cast<size_t>(received), now);
                if (result != qclient::spectator::resilient::NodeRuntimeResult::Accepted)
                    continue;
                if (decodedQsp && decoded.header.type == qsp::MessageType::Hello)
                    grandchildHelloAccepted = true;
                if (decodedQsp && decoded.header.type == qsp::MessageType::Ack)
                    grandchildAckAccepted = true;
                if (decodedQsp && decoded.header.type == qsp::MessageType::Detach)
                    grandchildDetachAccepted = true;
            }
            for (const auto& packet : runtime.pollRelayChildDatagrams(now, 32u)) {
                if (!packet.valid()
                    || !phaseCRawSend(dataSocket, packet.destination,
                                      packet.bytes.data(), packet.bytes.size())) {
                    fail("phase-d-s2", "S2 relay socket send failed");
                    return 29;
                }
            }
        }

        if (detachRequestedByParent && grandchildDetachAccepted
            && !upstreamDetachRequested) {
            if (!viewer.detach()) {
                fail("phase-d-s2", "S2 could not queue its bounded upstream detach");
                return 30;
            }
            upstreamDetachRequested = true;
        }

        if (bootstrapFrozen && now - lastBootstrapSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamBootstrapSent,
                                    enrolledQspPort, 0u});
            lastBootstrapSignal = now;
        }
        if (attached && now - lastAttachmentSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamActivationCommitted,
                                    activation.candidateDataEndpoint.port, 0u});
            lastAttachmentSignal = now;
        }
        if (relayServing && now - lastRelayReadySignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamRelayReady,
                                    relayDataPort, 0u});
            lastRelayReadySignal = now;
        }
        if (grandchildHelloAccepted && now - lastChildHelloSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamAcceptedHello, 0u, 0u});
            lastChildHelloSignal = now;
        }
        if (grandchildAckAccepted && now - lastChildAckSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamAcceptedAck, 0u, 0u});
            lastChildAckSignal = now;
        }
        if (grandchildDetachAccepted && now - lastChildDetachSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamAcceptedDetach, 0u, 0u});
            lastChildDetachSignal = now;
        }
        if (viewer.receivedNextRecord() >= initialRunwayRecords + 1u
            && now - lastStreamingSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamStreaming,
                                    viewer.receivedNextRecord(), 0u});
            lastStreamingSignal = now;
        }
        if (upstreamDetachSent && now - lastDetachSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamDetachSent, 0u, 0u});
            lastDetachSignal = now;
        }
        if (viewer.receivedNextRecord() >= initialRunwayRecords
            && now - lastSourceRecordSignal >= 100u) {
            (void)phaseCSendSignal(
                progressSocket, progressHost,
                {PhaseCSignalType::DownstreamSourceRecordAcknowledged,
                 viewer.receivedNextRecord(), viewer.receivedNextEvent()});
            lastSourceRecordSignal = now;
        }
        if (completedS2RetirementValidation && completedS2RetirementObserved
            && now - lastCompletedS2RetirementSignal >= 100u) {
            (void)phaseCSendSignal(
                progressSocket, progressHost,
                {PhaseCSignalType::DownstreamTopologyRetiredE0Safe,
                 viewer.receivedNextRecord(), viewer.receivedNextEvent()});
            lastCompletedS2RetirementSignal = now;
        }
        if (completedS2RetirementValidation
            && completedS2RetirementObservedAt != 0u
            && now - completedS2RetirementObservedAt >= 350u) {
            runtime.stop();
            return 0;
        }
        if (upstreamDetachSent && detachSentAt != 0u
            && now - detachSentAt >= 350u) {
            runtime.stop();
            return 0;
        }
        Sleep(1u);
    }
    fail("phase-d-s2", "timed out before S1->S2->S3 relay proof");
    runtime.stop();
    return 31;
}

int runPhaseDGrandchild(const char* const qspPortText,
                        const char* const controlPortText,
                        const char* const progressPortText) {
    uint16_t qspPort = 0u;
    uint16_t controlPort = 0u;
    uint16_t progressPort = 0u;
    if (!phaseCParsePorts(qspPortText, controlPortText, progressPortText,
                          qspPort, controlPort, progressPort)) {
        return 2;
    }
    const uint32_t initialRunwayRecords = phaseCInitialRunwayRecords();
    if (initialRunwayRecords == 0u) {
        fail("phase-d-s3", "default resilient cache runway is not representable");
        return 3;
    }
    NetScope network;
    if (!network.start()) return 4;

    const qnet::Endpoint rootQsp = loopbackEndpoint(qspPort);
    const qnet::Endpoint hostControl = loopbackEndpoint(controlPort);
    const qnet::Endpoint progressHost = loopbackEndpoint(progressPort);
    qnet::SpectateInfo e0{};
    std::string error;
    if (!phaseCWaitForInitialInfo(hostControl, kPhaseDGrandchildViewerId, e0,
                                  error)) {
        fail("phase-d-s3", error);
        return 5;
    }
    qsp::SpectatorGrant grant{};
    if (!phaseCGrantFromWire(e0.grant, kPhaseDGrandchildViewerId, grant)) {
        fail("phase-d-s3", "ordinary E0 grant did not bind S3");
        return 6;
    }
    qnet::UdpSock qspSocket;
    qnet::UdpSock progressSocket;
    if (!qspSocket.open(0u) || qspSocket.boundPort() == 0u
        || !progressSocket.open(0u)) {
        fail("phase-d-s3", "could not open S3 UDP sockets");
        return 7;
    }
    const uint16_t enrolledQspPort = qspSocket.boundPort();
    if (!phaseCSendSignal(progressSocket, progressHost,
                          {PhaseCSignalType::Hello,
                           static_cast<uint32_t>(PhaseCRole::Grandchild), 0u})) {
        fail("phase-d-s3", "could not publish S3 process readiness");
        return 8;
    }

    qsp::SpectatorViewer viewer(phaseCViewerConfig(kPhaseDGrandchildViewerId));
    if (!viewer.isConfigured() || !viewer.begin(kGameId)) {
        fail("phase-d-s3", "S3 bootstrap viewer was not configured/begun");
        return 9;
    }
    topology::client::ReleasedTopologyClientRuntime runtime;
    bool topologyStarted = false;
    bool bootstrapFrozen = false;
    bool routeActivationArmed = false;
    topology::client::ReleasedTopologyParentRouteActivation activation{};
    qnet::Endpoint activeQsp = rootQsp;
    qnet::Endpoint candidateQsp{};
    bool attached = false;
    bool detachRequestedByParent = false;
    bool detachRequested = false;
    bool detachSent = false;
    uint64_t detachSentAt = 0u;
    uint64_t lastBootstrapSignal = 0u;
    uint64_t lastAttachmentSignal = 0u;
    uint64_t lastStreamingSignal = 0u;
    uint64_t lastDetachSignal = 0u;
    uint64_t lastSourceRecordSignal = 0u;
    std::array<uint8_t, 4096u> inbound{};
    const uint64_t started = nowMs();
    while (!elapsed(started, kPhaseCPumpTimeoutMs)) {
        const uint64_t now = nowMs();
        std::vector<std::pair<qnet::Endpoint, PhaseCSignal>> signals;
        if (!phaseCDrainSignals(progressSocket, signals, error)) {
            fail("phase-d-s3", error);
            return 10;
        }
        for (const auto& item : signals) {
            if (item.first == progressHost
                && item.second.type == PhaseCSignalType::RelayAckObserved) {
                detachRequestedByParent = true;
            }
        }

        for (unsigned budget = 0u; budget < 32u; ++budget) {
            qnet::Endpoint sender{};
            const int received = qspSocket.recvFrom(inbound.data(), inbound.size(),
                                                     sender);
            if (received == 0) break;
            if (received < 0) {
                fail("phase-d-s3", "S3 QSP receive failed");
                return 11;
            }
            if (sender == activeQsp) {
                if (!qspIngressAccepted(viewer.onDatagram(
                        inbound.data(), static_cast<size_t>(received), now))) {
                    fail("phase-d-s3", "active S3 QSP source datagram was rejected");
                    return 12;
                }
                continue;
            }
            if (routeActivationArmed && sender == candidateQsp) {
                const qsp::IngressResult ingress =
                    viewer.onValidatedParentRouteDatagram(
                        activation.activationNonce, inbound.data(),
                        static_cast<size_t>(received), now);
                if (ingress != qsp::IngressResult::Accepted) continue;
                if (!viewer.commitValidatedParentRouteIngress(
                        activation.activationNonce, now)
                    || !runtime.completeParentRouteActivationAfterIngress(
                        activation.activationNonce,
                        RouteEndpoint{sender.sa.sin_addr.s_addr,
                                      ntohs(sender.sa.sin_port)},
                        viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                        viewer.receivedDigest(), now)) {
                    fail("phase-d-s3",
                         "same-generation S2 ingress did not complete S3 attachment");
                    return 13;
                }
                activeQsp = candidateQsp;
                routeActivationArmed = false;
                attached = true;
            }
        }

        if (detachRequestedByParent && attached && !detachRequested) {
            if (!viewer.detach()) {
                fail("phase-d-s3", "S3 could not queue its bounded detach");
                return 14;
            }
            detachRequested = true;
        }
        for (const qsp::ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
            qnet::Endpoint destination = activeQsp;
            if (packet.route == qsp::ViewerDatagramRoute::ValidatedPendingParent) {
                if (!routeActivationArmed
                    || packet.parentRouteActivationId != activation.activationNonce) {
                    fail("phase-d-s3", "stale candidate Hello escaped its activation");
                    return 15;
                }
                destination = candidateQsp;
            } else if (packet.route != qsp::ViewerDatagramRoute::ActiveParent
                       || packet.parentRouteActivationId != 0u) {
                fail("phase-d-s3", "unknown viewer route tag");
                return 16;
            }
            if (!qspSocket.sendTo(destination, packet.bytes.data(),
                                  packet.bytes.size())) {
                fail("phase-d-s3", "S3 QSP send failed");
                return 17;
            }
            if (packet.type == qsp::MessageType::InitAck && !topologyStarted) {
                if (!runtime.start(phaseCRuntimeConfig(
                        hostControl, e0, grant, qspSocket, viewer, qspPort), now)
                    || !runtime.onQspInitAckTransmitted(now)
                    || !runtime.updateVerifiedQspFrontier(
                        viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                        viewer.receivedDigest())) {
                    fail("phase-d-s3", "released S3 runtime rejected real InitAck");
                    return 18;
                }
                topologyStarted = true;
            }
            if (packet.type == qsp::MessageType::Detach && destination == activeQsp) {
                detachSent = true;
                if (detachSentAt == 0u) detachSentAt = now;
            }
        }

        if (topologyStarted && runtime.active()) {
            if (!runtime.updateVerifiedQspFrontier(
                    viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                    viewer.receivedDigest())) {
                fail("phase-d-s3", "released S3 rejected verified QSP frontier");
                return 19;
            }
            runtime.service(now);
            if (!runtime.active()) {
                fail("phase-d-s3", std::string("released S3 topology failed: ")
                    + runtime.status().reason);
                return 20;
            }
            if (runtime.status().state
                == topology::client::ReleasedTopologyClientState::AwaitingRootOffer) {
                bootstrapFrozen = true;
            }
            if (bootstrapFrozen && !routeActivationArmed && !attached) {
                topology::client::ReleasedTopologyParentRouteActivation next{};
                if (runtime.parentRouteActivation(next, now)) {
                    if (!next.valid()
                        || next.currentParentEndpoint.ipv4BE != loopbackIpv4BE()
                        || next.currentParentEndpoint.port != qspPort
                        || next.candidateDataEndpoint.ipv4BE != loopbackIpv4BE()
                        || next.candidateDataEndpoint.port == qspPort
                        || !viewer.armValidatedParentRouteTransition(
                            next.activationNonce, next.commonCursor.nextRecord,
                            next.commonCursor.nextEvent,
                            next.commonCursor.digest)) {
                        fail("phase-d-s3",
                             "S3 rejected the exact signed candidate handoff");
                        return 21;
                    }
                    activation = next;
                    candidateQsp = loopbackEndpoint(next.candidateDataEndpoint.port);
                    routeActivationArmed = true;
                }
            }
        }
        if (runtime.attached() && !attached) {
            fail("phase-d-s3", "S3 attached without candidate ingress commit");
            return 22;
        }

        if (bootstrapFrozen && now - lastBootstrapSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::GrandchildBootstrapSent,
                                    enrolledQspPort, 0u});
            lastBootstrapSignal = now;
        }
        if (attached && now - lastAttachmentSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::GrandchildActivationCommitted,
                                    activation.candidateDataEndpoint.port, 0u});
            lastAttachmentSignal = now;
        }
        if (viewer.receivedNextRecord() >= initialRunwayRecords + 2u
            && now - lastStreamingSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::GrandchildStreaming,
                                    viewer.receivedNextRecord(), 0u});
            lastStreamingSignal = now;
        }
        if (detachSent && now - lastDetachSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::GrandchildDetachSent, 0u, 0u});
            lastDetachSignal = now;
        }
        if (viewer.receivedNextRecord() >= initialRunwayRecords + 1u
            && now - lastSourceRecordSignal >= 100u) {
            (void)phaseCSendSignal(
                progressSocket, progressHost,
                {PhaseCSignalType::GrandchildSourceRecordAcknowledged,
                 viewer.receivedNextRecord(), viewer.receivedNextEvent()});
            lastSourceRecordSignal = now;
        }
        if (detachSent && detachSentAt != 0u && now - detachSentAt >= 350u) {
            runtime.stop();
            return 0;
        }
        Sleep(1u);
    }
    fail("phase-d-s3", "timed out before independent S3 proof");
    runtime.stop();
    return 23;
}

int runPhaseCRelayChild(const char* const qspPortText,
                        const char* const controlPortText,
                        const char* const progressPortText,
                        const bool pairedActivation,
                        const bool completedS2RetirementValidation = false) {
    uint16_t qspPort = 0u;
    uint16_t controlPort = 0u;
    uint16_t progressPort = 0u;
    if (!phaseCParsePorts(qspPortText, controlPortText, progressPortText,
                          qspPort, controlPort, progressPort)) {
        return 2;
    }
    (void)pairedActivation;
    const uint32_t initialRunwayRecords = phaseCInitialRunwayRecords();
    if (initialRunwayRecords == 0u) {
        fail("phase-c-relay", "default resilient cache runway is not representable");
        return 24;
    }
    NetScope network;
    if (!network.start()) return 3;

    const qnet::Endpoint rootQsp = loopbackEndpoint(qspPort);
    const qnet::Endpoint hostControl = loopbackEndpoint(controlPort);
    const qnet::Endpoint progressHost = loopbackEndpoint(progressPort);
    qnet::SpectateInfo e0{};
    std::string error;
    if (!phaseCWaitForInitialInfo(hostControl, kPhaseCRelayViewerId, e0,
                                  error)) {
        fail("phase-c-relay", error);
        return 4;
    }
    qsp::SpectatorGrant grant{};
    if (!phaseCGrantFromWire(e0.grant, kPhaseCRelayViewerId, grant)) {
        fail("phase-c-relay", "ordinary E0 grant did not bind the relay viewer");
        return 5;
    }
    qnet::UdpSock qspSocket;
    qnet::UdpSock migrationQspSocket;
    qnet::UdpSock progressSocket;
    if (!qspSocket.open(0u) || qspSocket.boundPort() == 0u
        || !progressSocket.open(0u)) {
        fail("phase-c-relay", "could not open Phase-C relay UDP sockets");
        return 6;
    }
    if (!phaseCSendSignal(progressSocket, progressHost,
                          {PhaseCSignalType::Hello,
                           static_cast<uint32_t>(PhaseCRole::Relay), 0u})) {
        fail("phase-c-relay", "could not publish relay process readiness");
        return 7;
    }

    qsp::SpectatorViewer viewer(phaseCViewerConfig(kPhaseCRelayViewerId));
    if (!viewer.isConfigured() || !viewer.begin(kGameId)) {
        fail("phase-c-relay", "ordinary relay viewer was not configured/begun");
        return 8;
    }
    topology::client::ReleasedTopologyClientRuntime runtime;
    bool topologyStarted = false;
    bool relayBindingStarted = false;
    bool relayServing = false;
    bool relayReadySent = false;
    bool childHelloAccepted = false;
    bool childAckAccepted = false;
    bool childDetachAccepted = false;
    bool upstreamAfterDetachSent = false;
    bool relayRebindRequested = false;
    bool relayRebindStarted = false;
    bool relayRebindDatagramSent = false;
    bool topologyRetiredAfterRebind = false;
    uint64_t relayRebindSentAt = 0u;
    uint16_t relayDataPort = 0u;
    uint64_t lastRelayReadySignal = 0u;
    uint64_t lastHelloSignal = 0u;
    uint64_t lastAckSignal = 0u;
    uint64_t lastDetachSignal = 0u;
    uint64_t lastUpstreamSignal = 0u;
    uint64_t lastSourceRecordSignal = 0u;
    uint64_t lastRelayRebindSignal = 0u;
    std::array<uint8_t, 4096u> inbound{};
    const uint64_t started = nowMs();
    while (!elapsed(started, kPhaseCPumpTimeoutMs)) {
        const uint64_t now = nowMs();
        qnet::UdpSock* const activeQspSocket = relayRebindStarted
            ? &migrationQspSocket : &qspSocket;
        std::vector<std::pair<qnet::Endpoint, PhaseCSignal>> signals;
        if (!phaseCDrainSignals(progressSocket, signals, error)) {
            fail("phase-c-relay", error);
            return 25;
        }
        for (const auto& item : signals) {
            if (item.first == progressHost
                && item.second.type == PhaseCSignalType::RequestRelayRebind) {
                relayRebindRequested = true;
            }
        }
        for (unsigned budget = 0u; budget < 16u; ++budget) {
            qnet::Endpoint sender{};
            const int received = activeQspSocket->recvFrom(
                inbound.data(), inbound.size(), sender);
            if (received == 0) break;
            if (received < 0) {
                fail("phase-c-relay", "upstream QSP receive failed");
                return 9;
            }
            if (!(sender == rootQsp)) continue;
            if (!qspIngressAccepted(viewer.onDatagram(
                    inbound.data(), static_cast<size_t>(received), now))) {
                fail("phase-c-relay", "upstream QSP datagram was rejected");
                return 10;
            }
        }
        if (completedS2RetirementValidation && relayRebindRequested
            && !relayRebindStarted) {
            if (!migrationQspSocket.open(0u)
                || migrationQspSocket.boundPort() == 0u
                || !viewer.migrate(grant.grantId, grant.macLow, grant.macHigh)) {
                fail("phase-c-relay", "ordinary relay viewer rejected bounded rebind");
                return 26;
            }
            relayRebindStarted = true;
        }
        for (const qsp::ViewerDatagram& packet : viewer.pollOutbound(now, 8u)) {
            if (!activeQspSocket->sendTo(rootQsp, packet.bytes.data(),
                                         packet.bytes.size())) {
                fail("phase-c-relay", "upstream QSP send failed");
                return 11;
            }
            if (packet.type == qsp::MessageType::InitAck) {
                if (topologyStarted) {
                    if (!completedS2RetirementValidation
                        || !relayRebindDatagramSent) {
                        fail("phase-c-relay",
                             "released relay runtime rejected real InitAck");
                        return 12;
                    }
                } else if (!runtime.start(
                               phaseCRuntimeConfig(hostControl, e0, grant,
                                                   qspSocket, viewer, qspPort), now)
                           || !runtime.onQspInitAckTransmitted(now)
                           || !runtime.updateVerifiedQspFrontier(
                               viewer.receivedNextRecord(),
                               viewer.receivedNextEvent(),
                               viewer.receivedDigest())) {
                    fail("phase-c-relay", "released relay runtime rejected real InitAck");
                    return 12;
                }
                topologyStarted = true;
            }
            if (completedS2RetirementValidation
                && packet.type == qsp::MessageType::Migrate) {
                relayRebindDatagramSent = true;
                if (relayRebindSentAt == 0u) relayRebindSentAt = now;
            }
        }

        if (topologyStarted && runtime.active()) {
            if ((!completedS2RetirementValidation || !relayRebindDatagramSent)
                && !runtime.updateVerifiedQspFrontier(
                    viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                    viewer.receivedDigest())) {
                fail("phase-c-relay", "released relay rejected the verified QSP frontier");
                return 13;
            }
            runtime.service(now);
            if (!runtime.active()) {
                if (completedS2RetirementValidation && relayRebindDatagramSent) {
                    topologyRetiredAfterRebind = true;
                } else {
                    fail("phase-c-relay", std::string("released relay topology failed: ")
                        + runtime.status().reason);
                    return 14;
                }
            }
        }

        if (!topologyRetiredAfterRebind && runtime.attached()
            && !relayBindingStarted) {
            qnet::UdpSock relayOwner;
            if (!relayOwner.open(0u) || relayOwner.boundPort() == 0u) {
                fail("phase-c-relay", "could not create DPlay-facing relay socket");
                return 15;
            }
            relayDataPort = relayOwner.boundPort();
            uintptr_t owned = static_cast<uintptr_t>(relayOwner.release());
            if (!runtime.beginRelayDataBinding(owned, now)
                || owned != topology::client::kInvalidReleasedTopologyRelaySocket) {
                if (owned != topology::client::kInvalidReleasedTopologyRelaySocket)
                    closesocket(static_cast<SOCKET>(owned));
                fail("phase-c-relay", "released relay did not adopt exact data socket");
                return 16;
            }
            relayBindingStarted = true;
        }

        if (!topologyRetiredAfterRebind && relayBindingStarted
            && runtime.relayDataBindingState()
                == topology::client::ReleasedTopologyRelayBindingState::Ready
            && !relayServing) {
            if (!runtime.startRelayNode(viewer, now)
                || runtime.projectRelayViewer(viewer, now)
                    != qclient::spectator::resilient::NodeRuntimeResult::Accepted
                || !runtime.relayNodeRunning()) {
                fail("phase-c-relay", "canonical released relay facade did not start");
                return 17;
            }
            topology::client::ReleasedTopologyRelayBindingSnapshot binding{};
            if (!runtime.relayDataBindingSnapshot(binding) || !binding.valid()
                || binding.observedDataRoute.port != relayDataPort) {
                fail("phase-c-relay", "host did not acknowledge the exact relay socket");
                return 18;
            }
            relayServing = true;
            relayReadySent = true;
        }

        if (relayServing && !(completedS2RetirementValidation
                              && relayRebindDatagramSent)) {
            if (runtime.projectRelayViewer(viewer, now)
                != qclient::spectator::resilient::NodeRuntimeResult::Accepted) {
                fail("phase-c-relay", "canonical relay projection stopped");
                return 19;
            }
            const auto borrowed = runtime.borrowedRelayDataSocket();
            if (!borrowed.valid()) {
                fail("phase-c-relay", "DPlay-facing relay socket borrow became invalid");
                return 20;
            }
            const SOCKET dataSocket = static_cast<SOCKET>(borrowed.socket);
            for (unsigned budget = 0u; budget < 32u; ++budget) {
                sockaddr_in rawSender{};
                int rawSenderLength = sizeof(rawSender);
                const int received = ::recvfrom(
                    dataSocket, reinterpret_cast<char*>(inbound.data()),
                    static_cast<int>(inbound.size()), 0,
                    reinterpret_cast<sockaddr*>(&rawSender), &rawSenderLength);
                if (received == SOCKET_ERROR) {
                    const int wsa = WSAGetLastError();
                    if (wsa == WSAEWOULDBLOCK) break;
                    fail("phase-c-relay", "DPlay-facing relay socket receive failed");
                    return 21;
                }
                if (received <= 0) continue;
                qsp::Datagram decoded{};
                const bool decodedQsp = qsp::decodeDatagram(
                    inbound.data(), static_cast<size_t>(received),
                    sessionIdentity(), decoded) == qsp::DecodeError::None;
                const qclient::spectator::resilient::NodeRuntimeResult result =
                    runtime.ingestRelayChildDatagram(
                        RouteEndpoint{rawSender.sin_addr.s_addr,
                                      ntohs(rawSender.sin_port)},
                        inbound.data(), static_cast<size_t>(received), now);
                if (result != qclient::spectator::resilient::NodeRuntimeResult::Accepted)
                    continue;
                if (decodedQsp && decoded.header.type == qsp::MessageType::Hello)
                    childHelloAccepted = true;
                if (decodedQsp && decoded.header.type == qsp::MessageType::Ack)
                    childAckAccepted = true;
                if (decodedQsp && decoded.header.type == qsp::MessageType::Detach)
                    childDetachAccepted = true;
            }
            for (const topology::client::ReleasedTopologyRelayDatagram& packet
                 : runtime.pollRelayChildDatagrams(now, 32u)) {
                if (!packet.valid()
                    || !phaseCRawSend(dataSocket, packet.destination,
                                      packet.bytes.data(), packet.bytes.size())) {
                    fail("phase-c-relay", "DPlay-facing relay socket send failed");
                    return 22;
                }
            }
        }

        if (relayReadySent && now - lastRelayReadySignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::RelayReady, relayDataPort,
                                    0u});
            lastRelayReadySignal = now;
        }
        if (childHelloAccepted && now - lastHelloSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::RelayAcceptedHello, 0u, 0u});
            lastHelloSignal = now;
        }
        if (childAckAccepted && now - lastAckSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::RelayAcceptedAck, 0u, 0u});
            lastAckSignal = now;
        }
        if (childDetachAccepted && now - lastDetachSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::RelayAcceptedDetach, 0u, 0u});
            lastDetachSignal = now;
        }
        if (childDetachAccepted
            && viewer.receivedNextRecord() >= initialRunwayRecords + 1u) {
            upstreamAfterDetachSent = true;
        }
        if (upstreamAfterDetachSent && now - lastUpstreamSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::RelayUpstreamAfterDetach,
                                    viewer.receivedNextRecord(), 0u});
            lastUpstreamSignal = now;
            if (lastUpstreamSignal != 0u && now - started > 400u) {
                runtime.stop();
                return 0;
            }
        }
        if (viewer.receivedNextRecord() >= initialRunwayRecords
            && now - lastSourceRecordSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::RelaySourceRecordAcknowledged,
                                    viewer.receivedNextRecord(),
                                    viewer.receivedNextEvent()});
            lastSourceRecordSignal = now;
        }
        if (completedS2RetirementValidation && relayRebindDatagramSent
            && now - lastRelayRebindSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::RelayRebindSent, 0u, 0u});
            lastRelayRebindSignal = now;
        }
        if (completedS2RetirementValidation && relayRebindSentAt != 0u
            && now - relayRebindSentAt >= 500u) {
            runtime.stop();
            return 0;
        }
        Sleep(1u);
    }
    fail("phase-c-relay", "timed out before assignment/child/detach continuity proof");
    return 23;
}

int runPhaseCDownstreamChild(const char* const qspPortText,
                             const char* const controlPortText,
                             const char* const progressPortText,
                             const bool pairedActivation,
                             const bool completedS2RetirementValidation = false) {
    if (pairedActivation) {
        return runPhaseDIntermediateChild(
            qspPortText, controlPortText, progressPortText,
            completedS2RetirementValidation);
    }
    uint16_t qspPort = 0u;
    uint16_t controlPort = 0u;
    uint16_t progressPort = 0u;
    if (!phaseCParsePorts(qspPortText, controlPortText, progressPortText,
                          qspPort, controlPort, progressPort)) {
        return 2;
    }
    const uint32_t initialRunwayRecords = phaseCInitialRunwayRecords();
    if (initialRunwayRecords == 0u) {
        fail("phase-c-child", "default resilient cache runway is not representable");
        return 21;
    }
    NetScope network;
    if (!network.start()) return 3;

    const qnet::Endpoint rootQsp = loopbackEndpoint(qspPort);
    const qnet::Endpoint hostControl = loopbackEndpoint(controlPort);
    const qnet::Endpoint progressHost = loopbackEndpoint(progressPort);
    qnet::SpectateInfo e0{};
    std::string error;
    if (!phaseCWaitForInitialInfo(hostControl, kPhaseCChildViewerId, e0,
                                  error)) {
        fail("phase-c-child", error);
        return 4;
    }
    qsp::SpectatorGrant grant{};
    if (!phaseCGrantFromWire(e0.grant, kPhaseCChildViewerId, grant)) {
        fail("phase-c-child", "ordinary E0 grant did not bind the downstream viewer");
        return 5;
    }
    qnet::UdpSock qspSocket;
    qnet::UdpSock progressSocket;
    if (!qspSocket.open(0u) || qspSocket.boundPort() == 0u
        || !progressSocket.open(0u)) {
        fail("phase-c-child", "could not open Phase-C downstream UDP sockets");
        return 6;
    }
    const uint16_t enrolledQspPort = qspSocket.boundPort();
    if (!phaseCSendSignal(progressSocket, progressHost,
                          {PhaseCSignalType::Hello,
                           static_cast<uint32_t>(PhaseCRole::Downstream), 0u})) {
        fail("phase-c-child", "could not publish downstream process readiness");
        return 7;
    }

    qsp::SpectatorViewer bootstrapViewer(
        phaseCViewerConfig(kPhaseCChildViewerId));
    if (!bootstrapViewer.isConfigured() || !bootstrapViewer.begin(kGameId)) {
        fail("phase-c-child", "ordinary downstream bootstrap viewer was not configured/begun");
        return 8;
    }
    topology::client::ReleasedTopologyClientRuntime runtime;
    bool topologyStarted = false;
    bool bootstrapFrozen = false;
    bool activationCommitted = false;
    topology::client::ReleasedTopologyParentRouteActivation
        committedActivation{};
    bool startDownstream = false;
    uint16_t relayPort = 0u;
    qnet::Endpoint relayEndpoint{};
    PhaseCResumedOrdinaryQspPeer downstreamPeer;
    bool downstreamPeerStarted = false;
    bool downstreamStreaming = false;
    uint64_t lastBootstrapSignal = 0u;
    uint64_t lastActivationCommitSignal = 0u;
    uint64_t lastStreamingSignal = 0u;
    uint64_t lastDetachSignal = 0u;
    uint64_t lastSourceRecordSignal = 0u;
    uint64_t detachSentAt = 0u;
    std::array<uint8_t, 4096u> inbound{};
    const uint64_t started = nowMs();
    while (!elapsed(started, kPhaseCPumpTimeoutMs)) {
        const uint64_t now = nowMs();
        std::vector<std::pair<qnet::Endpoint, PhaseCSignal>> signals;
        if (!phaseCDrainSignals(progressSocket, signals, error)) {
            fail("phase-c-child", error);
            return 9;
        }
        for (const auto& item : signals) {
            if (!(item.first == progressHost)) continue;
            if (item.second.type == PhaseCSignalType::StartDownstream
                && item.second.value != 0u && item.second.value <= 65535u) {
                relayPort = static_cast<uint16_t>(item.second.value);
                relayEndpoint = loopbackEndpoint(relayPort);
                startDownstream = true;
            } else if (item.second.type
                       == PhaseCSignalType::RelayAckObserved) {
                downstreamPeer.authorizeDetach();
            }
        }
        if (startDownstream && !downstreamPeerStarted) {
            if (pairedActivation
                && (!activationCommitted || !committedActivation.valid()
                    || committedActivation.candidateDataEndpoint.port
                        != relayPort
                    || committedActivation.currentParentEndpoint.port
                        != qspPort)) {
                fail("phase-d-child",
                     "parent asked S2 to switch before its exact final-commit projection");
                return 10;
            }
            runtime.stop();
            if (qspSocket.boundPort() != enrolledQspPort
                || !downstreamPeer.begin(bootstrapViewer,
                                         initialRunwayRecords, error)) {
                fail("phase-c-child", error.empty()
                    ? "ordinary relay child lost its enrolled QSP endpoint"
                    : error);
                return 10;
            }
            downstreamPeerStarted = true;
        }

        for (unsigned budget = 0u; budget < 16u; ++budget) {
            qnet::Endpoint sender{};
            const int received = qspSocket.recvFrom(inbound.data(), inbound.size(),
                                                     sender);
            if (received == 0) break;
            if (received < 0) {
                fail("phase-c-child", "downstream QSP receive failed");
                return 11;
            }
            if (downstreamPeerStarted) {
                if (!(sender == relayEndpoint)) continue;
                if (!downstreamPeer.onDatagram(
                        inbound.data(), static_cast<size_t>(received), error)) {
                    fail("phase-c-child", error);
                    return 12;
                }
            } else if (sender == rootQsp
                       && !qspIngressAccepted(bootstrapViewer.onDatagram(
                           inbound.data(), static_cast<size_t>(received), now))) {
                fail("phase-c-child", "ordinary bootstrap QSP datagram was rejected");
                return 13;
            }
        }

        if (!downstreamPeerStarted) {
            for (const qsp::ViewerDatagram& packet
                 : bootstrapViewer.pollOutbound(now, 8u)) {
                if (!qspSocket.sendTo(rootQsp, packet.bytes.data(),
                                      packet.bytes.size())) {
                    fail("phase-c-child", "ordinary bootstrap QSP send failed");
                    return 14;
                }
                if (packet.type == qsp::MessageType::InitAck) {
                    if (topologyStarted
                        || !runtime.start(phaseCRuntimeConfig(
                            hostControl, e0, grant, qspSocket, bootstrapViewer,
                            qspPort), now)
                        || !runtime.onQspInitAckTransmitted(now)
                        || !runtime.updateVerifiedQspFrontier(
                            bootstrapViewer.receivedNextRecord(),
                            bootstrapViewer.receivedNextEvent(),
                            bootstrapViewer.receivedDigest())) {
                        fail("phase-c-child", "released downstream runtime rejected real InitAck");
                        return 15;
                    }
                    topologyStarted = true;
                }
            }
            if (topologyStarted && runtime.active()
                && (!bootstrapFrozen || pairedActivation)) {
                if (!runtime.updateVerifiedQspFrontier(
                        bootstrapViewer.receivedNextRecord(),
                        bootstrapViewer.receivedNextEvent(),
                        bootstrapViewer.receivedDigest())) {
                    fail("phase-c-child", "released downstream rejected verified QSP frontier");
                    return 16;
                }
                runtime.service(now);
                if (!runtime.active()) {
                    fail("phase-c-child", std::string("released downstream topology failed: ")
                        + runtime.status().reason);
                    return 17;
                }
                if (runtime.status().state
                    == topology::client::ReleasedTopologyClientState::AwaitingRootOffer) {
                    bootstrapFrozen = true;
                }
                if (pairedActivation && bootstrapFrozen) {
                    topology::client::ReleasedTopologyParentRouteActivation
                        activation{};
                    if (runtime.parentRouteActivation(activation, now)) {
                        if (!activation.valid()
                            || activation.currentParentEndpoint.port != qspPort
                            || activation.candidateDataEndpoint.port == qspPort) {
                            fail("phase-d-child",
                                 "released runtime exposed an invalid final-commit handoff");
                            return 17;
                        }
                        committedActivation = activation;
                        activationCommitted = true;
                    }
                }
            }
        } else {
            const std::vector<qsp::ViewerDatagram> outbound =
                downstreamPeer.pollOutbound(now);
            if (!downstreamPeer.active()) {
                fail("phase-c-child", downstreamPeer.failure().empty()
                    ? "ordinary relay child lost its public QSP state"
                    : downstreamPeer.failure());
                return 18;
            }
            for (const qsp::ViewerDatagram& packet : outbound) {
                if (!qspSocket.sendTo(relayEndpoint, packet.bytes.data(),
                                      packet.bytes.size())) {
                    fail("phase-c-child", "ordinary relay child QSP send failed");
                    return 18;
                }
            }
            downstreamStreaming = downstreamPeer.streaming();
            if (downstreamPeer.detachSent() && detachSentAt == 0u) {
                detachSentAt = now;
            }
        }

        if (bootstrapFrozen && now - lastBootstrapSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamBootstrapSent,
                                    qspSocket.boundPort(), 0u});
            lastBootstrapSignal = now;
        }
        if (activationCommitted && now - lastActivationCommitSignal >= 100u) {
            (void)phaseCSendSignal(
                progressSocket, progressHost,
                {PhaseCSignalType::DownstreamActivationCommitted,
                 committedActivation.candidateDataEndpoint.port, 0u});
            lastActivationCommitSignal = now;
        }
        if (downstreamStreaming && now - lastStreamingSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamStreaming,
                                    downstreamPeer.receivedNextRecord(), 0u});
            lastStreamingSignal = now;
        }
        if (downstreamPeer.detachSent() && now - lastDetachSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, progressHost,
                                   {PhaseCSignalType::DownstreamDetachSent, 0u, 0u});
            lastDetachSignal = now;
        }
        if (!downstreamPeerStarted
            && bootstrapViewer.receivedNextRecord() >= initialRunwayRecords
            && now - lastSourceRecordSignal >= 100u) {
            (void)phaseCSendSignal(
                progressSocket, progressHost,
                {PhaseCSignalType::DownstreamSourceRecordAcknowledged,
                 bootstrapViewer.receivedNextRecord(),
                 bootstrapViewer.receivedNextEvent()});
            lastSourceRecordSignal = now;
        }
        if (downstreamPeer.detachSent() && detachSentAt != 0u
            && now - detachSentAt >= 350u) {
            return 0;
        }
        Sleep(1u);
    }
    fail("phase-c-child", "timed out before ordinary relay-child proof");
    return 20;
}

bool publishPhaseCInitialRunway(SpectatorSidecar& sidecar,
                                uint64_t& sequence,
                                const uint32_t recordCount,
                                const uint64_t now) {
    if (recordCount == 0u) return false;
    const qsp::GameDescriptor descriptor = gameDescriptor();
    uint64_t digest = descriptor.initialDigest;
    for (uint32_t index = 0u; index < recordCount; ++index) {
        qsp::ConfirmedRecord record = recordFor(index);
        record.rollingDigest = qsp::advanceRollingDigest(
            digest, descriptor.gameId, record);
        digest = record.rollingDigest;
        PublicationEvent recordEvent{};
        recordEvent.kind = PublicationKind::Record;
        recordEvent.sequence = sequence++;
        recordEvent.record = record;
        sidecar.publish(recordEvent, now);
    }
    PublicationEvent proof{};
    proof.kind = PublicationKind::TailProof;
    proof.sequence = sequence++;
    proof.tailRecordCursor = recordCount;
    proof.postTailStateHash = stateHashFor(recordCount);
    proof.hasPostTailProof = true;
    sidecar.publish(proof, now);
    return sidecar.status().confirmedRecords >= recordCount;
}

bool publishPhaseCRecord(SpectatorSidecar& sidecar, uint64_t& sequence,
                         const uint32_t recordIndex, const uint64_t now) {
    const qsp::GameDescriptor descriptor = gameDescriptor();
    uint64_t digest = descriptor.initialDigest;
    qsp::ConfirmedRecord record{};
    for (uint32_t index = 0u; index <= recordIndex; ++index) {
        record = recordFor(index);
        record.rollingDigest = qsp::advanceRollingDigest(
            digest, descriptor.gameId, record);
        digest = record.rollingDigest;
    }
    PublicationEvent recordEvent{};
    recordEvent.kind = PublicationKind::Record;
    recordEvent.sequence = sequence++;
    recordEvent.record = record;
    sidecar.publish(recordEvent, now);
    PublicationEvent proof{};
    proof.kind = PublicationKind::TailProof;
    proof.sequence = sequence++;
    proof.tailRecordCursor = recordIndex + 1u;
    proof.postTailStateHash = stateHashFor(recordIndex + 1u);
    proof.hasPostTailProof = true;
    sidecar.publish(proof, now);
    return sidecar.status().confirmedRecords >= recordIndex + 1u;
}

bool phaseCValidIntent(const qnet::ClientIntent& intent,
                       const uint64_t viewerId, const uint16_t capabilities,
                       const uint32_t expectedResumeRecord) {
    const qsp::SessionIdentity identity = sessionIdentity();
    return intent.intent == qnet::INTENT_SPECTATE
        && intent.spectatorRoute == qnet::SPECTATE_ROUTE_DIRECT
        && intent.spectatorCapabilities == capabilities
        && intent.sessionId == identity.sessionId
        && intent.keyLow == identity.cookie.low
        && intent.keyHigh == identity.cookie.high
        && intent.viewerId == viewerId
        && intent.resumeRecord == expectedResumeRecord
        && intent.reserved3 == 0u && intent.resumeDigest == 0u;
}

struct PhaseCParticipant {
    uint64_t viewerId = 0u;
    qsp::SpectatorGrant grant{};
    qnet::SpectateInfo live{};
    bool e0InfoSent = false;
    bool topologyProofReceived = false;
    bool topologyEnrolled = false;
    bool topologyInfoSent = false;
    uint64_t topologyRequestNonce = 0u;
    qnet::UdpControlLink* topologyLink = nullptr;
    qnet::SpectateTopologyChallengeV2 challenge{};
    topology::TopologyViewerClaim claim{};
    topology::TopologyEnrollment enrollment{};
};

enum class PhaseCLinkLane : uint8_t { Unknown, OrdinaryE0, TopologyV2 };

struct PhaseCControlLink {
    qnet::UdpControlLink* link = nullptr;
    bool versionSeen = false;
    bool releaseAfterResponse = false;
    PhaseCLinkLane lane = PhaseCLinkLane::Unknown;
    PhaseCParticipant* participant = nullptr;
};

PhaseCParticipant* phaseCFindParticipant(
    PhaseCParticipant& relay, PhaseCParticipant& child,
    PhaseCParticipant& grandchild,
    const uint64_t viewerId) {
    if (relay.viewerId == viewerId) return &relay;
    if (child.viewerId == viewerId) return &child;
    if (grandchild.viewerId == viewerId) return &grandchild;
    return nullptr;
}

bool runPhaseCParent(const bool pairedActivation = false,
                     const bool completedS2RetirementValidation = false) {
    const bool phaseDGrandchild = pairedActivation
        && !completedS2RetirementValidation;
    const uint32_t initialRunwayRecords = phaseCInitialRunwayRecords();
    if (initialRunwayRecords == 0u) {
        fail("phase-c-parent", "default resilient cache runway is not representable");
        return false;
    }
    NetScope network;
    if (!network.start()) {
        fail("phase-c-parent", "netInit failed");
        return false;
    }

    uint64_t clock = kInitialNowMs;
    uint64_t sequence = 1u;
    SidecarConfig phaseConfig = sidecarConfig();
    if (phaseDGrandchild) phaseConfig.maxViewers = 3u;
    SpectatorSidecar sidecar(phaseConfig);
    std::string error;
    if (!sidecar.start(qclient::spectator::makeUdpSidecarTransport(), nullptr,
                       clock, error)
        || sidecar.boundPort() == 0u) {
        fail("phase-c-parent", error.empty() ? "sidecar QSP bind failed" : error);
        return false;
    }
    if (!publishStart(sidecar, sequence, ++clock)
        || !sidecar.configureTopologyAuthority(
            (pairedActivation
                 ? phaseDActivationRequest(sidecar.boundPort())
                 : phaseCActivationRequest(sidecar.boundPort())),
            ++clock, error)) {
        fail("phase-c-parent", error.empty()
            ? "sidecar did not start the production topology authority" : error);
        (void)sidecar.stop(false, ++clock);
        return false;
    }
    const auto initial = sidecar.status();
    if (!initial.topologyAuthorityReady || initial.topologyQsrPort == 0u
        || initial.topologyQsrPort == sidecar.boundPort()) {
        fail("phase-c-parent", "production QSP/QSR root bindings were not distinct");
        (void)sidecar.stop(false, ++clock);
        return false;
    }

    PhaseCParticipant relayParticipant{};
    relayParticipant.viewerId = kPhaseCRelayViewerId;
    PhaseCParticipant childParticipant{};
    childParticipant.viewerId = kPhaseCChildViewerId;
    PhaseCParticipant grandchildParticipant{};
    grandchildParticipant.viewerId = kPhaseDGrandchildViewerId;
    const auto issuePhaseCGrant = [&](PhaseCParticipant& participant) {
        GrantRequest request{};
        request.viewerId = participant.viewerId;
        request.route = qnet::SPECTATE_ROUTE_DIRECT;
        request.peerIpv4BE = loopbackIpv4BE();
        request.lifetimeSeconds = 300u;
        AdmissionDenyReason denied = AdmissionDenyReason::None;
        if (!sidecar.issueGrant(request, participant.grant, denied, ++clock)
            || participant.grant.viewerId != participant.viewerId
            || participant.grant.route != qnet::SPECTATE_ROUTE_DIRECT) {
            return false;
        }
        participant.live = makeLiveInfoForViewer(sidecar.boundPort(),
                                                 participant.viewerId,
                                                 participant.grant);
        return true;
    };
    if (!issuePhaseCGrant(relayParticipant)
        || !issuePhaseCGrant(childParticipant)
        || (phaseDGrandchild && !issuePhaseCGrant(grandchildParticipant))) {
        fail("phase-c-parent", "sidecar could not issue two exact ordinary grants");
        (void)sidecar.stop(false, ++clock);
        return false;
    }
    const auto sendTopologyInfo = [&](PhaseCParticipant& participant) {
        qnet::SpectateTopologyInfoV2 info{};
        return participant.topologyLink != nullptr
            && participant.topologyEnrolled
            && controlv2::infoFromEnrollment(participant.topologyRequestNonce,
                                              participant.enrollment, info)
            && controlv2::signInfo(info, participant.grant)
            && participant.topologyLink->sendMsg(
                qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2, &info, sizeof(info));
    };

    qnet::UdpSock controlSocket;
    qnet::UdpSock progressSocket;
    if (!controlSocket.open(0u) || controlSocket.boundPort() == 0u
        || !progressSocket.open(0u) || progressSocket.boundPort() == 0u) {
        fail("phase-c-parent", "could not open Phase-C host control/progress sockets");
        (void)sidecar.stop(false, ++clock);
        return false;
    }
    qnet::UdpControlListener listener;
    listener.open(controlSocket);

    HiddenProcess relayProcess;
    HiddenProcess downstreamProcess;
    HiddenProcess grandchildProcess;
    const auto launchChild = [&](HiddenProcess& process, const wchar_t* mode) {
        return process.launch(std::wstring(mode) + L" "
                              + std::to_wstring(sidecar.boundPort()) + L" "
                              + std::to_wstring(controlSocket.boundPort()) + L" "
                              + std::to_wstring(progressSocket.boundPort()), error);
    };
    if (!launchChild(relayProcess, completedS2RetirementValidation
            ? L"--phase-d-relay-child-retirement"
            : pairedActivation
                ? L"--phase-d-relay-child" : L"--phase-c-relay-child")) {
        fail("phase-c-parent", error);
        (void)sidecar.stop(false, ++clock);
        return false;
    }

    std::vector<PhaseCControlLink> links;
    bool downstreamLaunched = false;
    bool grandchildLaunched = false;
    bool relayProgressKnown = false;
    bool downstreamProgressKnown = false;
    bool grandchildProgressKnown = false;
    qnet::Endpoint relayProgress{};
    qnet::Endpoint downstreamProgress{};
    qnet::Endpoint grandchildProgress{};
    bool relayReady = false;
    bool relaySourceRecordAcknowledged = false;
    uint16_t relayDataPort = 0u;
    bool downstreamBootstrapSent = false;
    bool downstreamSourceRecordAcknowledged = false;
    bool handoffIssued = false;
    bool activationStaged = false;
    bool downstreamActivationCommitted = false;
    bool downstreamRelayReady = false;
    uint16_t downstreamRelayDataPort = 0u;
    bool grandchildActivationStaged = false;
    bool grandchildHandoffIssued = false;
    bool grandchildBootstrapSent = false;
    bool grandchildSourceRecordAcknowledged = false;
    bool grandchildActivationCommitted = false;
    bool grandchildStreaming = false;
    bool grandchildDetachSent = false;
    bool relayAcceptedHello = false;
    bool relayAcceptedAck = false;
    bool relayAcceptedDetach = false;
    bool downstreamAcceptedHello = false;
    bool downstreamAcceptedAck = false;
    bool downstreamAcceptedDetach = false;
    bool downstreamStreaming = false;
    bool downstreamDetachSent = false;
    bool initialRunwayPublished = false;
    bool postEnrollmentRecordPublished = false;
    bool postGrandchildRecordPublished = false;
    bool relayUpstreamAfterDetach = false;
    bool relayRebindSent = false;
    bool downstreamTopologyRetiredE0Safe = false;
    uint64_t lastStartSignal = 0u;
    uint64_t lastAckObservedSignal = 0u;
    uint64_t lastGrandchildAckObservedSignal = 0u;
    uint64_t lastDownstreamStopSignal = 0u;
    uint64_t lastRelayRebindRequestSignal = 0u;
    bool phaseProofClosed = false;
    const uint64_t started = nowMs();
    while (!elapsed(started, kPhaseCPumpTimeoutMs)) {
        ++clock;
        sidecar.service(clock);
        listener.pump(0);
        for (;;) {
            qnet::UdpControlLink* const accepted = listener.acceptOne();
            if (accepted == nullptr) break;
            qnet::VersionConfig version{qnet::kProtocolVersion};
            if (!accepted->sendMsg(qnet::MSG_VERSION_CONFIG, &version,
                                   sizeof(version))) {
                fail("phase-c-parent", "could not send control protocol version");
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            links.push_back(PhaseCControlLink{accepted});
        }

        for (PhaseCControlLink& state : links) {
            if (state.link == nullptr) continue;
            std::vector<qnet::ControlLink::Received> messages;
            if (!readLinkMessages(state.link, messages, error)) {
                fail("phase-c-parent", error);
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            for (const qnet::ControlLink::Received& rx : messages) {
                if (!state.versionSeen) {
                    qnet::VersionConfig version{};
                    if (rx.type != qnet::MSG_VERSION_CONFIG
                        || rx.body.size() != sizeof(version)) {
                        fail("phase-c-parent", "Phase-C child version was malformed");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    std::memcpy(&version, rx.body.data(), sizeof(version));
                    if (version.protocolVersion != qnet::kProtocolVersion) {
                        fail("phase-c-parent", "Phase-C child version mismatched");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    state.versionSeen = true;
                    continue;
                }

                if (state.lane == PhaseCLinkLane::Unknown) {
                    qnet::ClientIntent intent{};
                    if (rx.type != qnet::MSG_CLIENT_INTENT
                        || rx.body.size() != sizeof(intent)) {
                        fail("phase-c-parent", "Phase-C initial intent was malformed");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    std::memcpy(&intent, rx.body.data(), sizeof(intent));
                    PhaseCParticipant* const participant = phaseCFindParticipant(
                        relayParticipant, childParticipant, grandchildParticipant,
                        intent.viewerId);
                    const bool topologyIntent = intent.spectatorCapabilities
                        == qnet::kSpectateCapabilityReleasedTopologyV2Mask;
                    const uint16_t expectedCapabilities = topologyIntent
                        ? qnet::kSpectateCapabilityReleasedTopologyV2Mask : 0u;
                    const uint32_t expectedResume = topologyIntent ? 1u : 0u;
                    if (participant == nullptr || !phaseCValidIntent(
                            intent, intent.viewerId, expectedCapabilities,
                            expectedResume)) {
                        fail("phase-c-parent", "Phase-C intent did not bind exact E0 facts"
                            " (viewer=" + std::to_string(intent.viewerId)
                            + ", caps=" + std::to_string(intent.spectatorCapabilities)
                            + ", route=" + std::to_string(intent.spectatorRoute)
                            + ", resume=" + std::to_string(intent.resumeRecord)
                            + ", kind=" + std::to_string(intent.intent)
                            + ", session=" + std::to_string(intent.sessionId)
                            + ", keyLow=" + std::to_string(intent.keyLow)
                            + ")");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    state.participant = participant;
                    if (!topologyIntent) {
                        if (participant->e0InfoSent
                            || !state.link->sendMsg(qnet::MSG_SPECTATE_INFO,
                                                    &participant->live,
                                                    sizeof(participant->live))) {
                            fail("phase-c-parent", "could not send ordinary E0 grant response");
                            (void)sidecar.stop(false, ++clock);
                            return false;
                        }
                        participant->e0InfoSent = true;
                        state.lane = PhaseCLinkLane::OrdinaryE0;
                        state.releaseAfterResponse = true;
                        continue;
                    }
                    if (participant->topologyInfoSent) {
                        fail("phase-c-parent", "duplicate Phase-C topology link");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    state.lane = PhaseCLinkLane::TopologyV2;
                    participant->challenge = {};
                    participant->challenge.schemaVersion =
                        qnet::kSpectateTopologyControlSchemaVersion;
                    participant->challenge.requiredFeatures =
                        qnet::kSpectateTopologyControlRequiredFeatures;
                    participant->challenge.sessionId = kSessionId;
                    participant->challenge.viewerId = participant->viewerId;
                    participant->challenge.viewerGeneration = 1u;
                    const uint8_t seed = participant->viewerId
                        == kPhaseCRelayViewerId ? 0xC1u
                        : participant->viewerId == kPhaseCChildViewerId
                        ? 0xD1u : 0xE1u;
                    for (size_t index = 0u;
                         index < sizeof(participant->challenge.peerChallenge);
                         ++index) {
                        participant->challenge.peerChallenge[index] =
                            static_cast<uint8_t>(seed + index);
                    }
                    if (!state.link->sendMsg(
                            qnet::MSG_SPECTATE_TOPOLOGY_CHALLENGE_V2,
                            &participant->challenge,
                            sizeof(participant->challenge))) {
                        fail("phase-c-parent", "could not send V2 topology challenge");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    continue;
                }

                if (state.lane == PhaseCLinkLane::OrdinaryE0) {
                    fail("phase-c-parent", "ordinary E0 child sent unexpected follow-up control");
                    (void)sidecar.stop(false, ++clock);
                    return false;
                }
                if (state.participant == nullptr
                    || rx.type != qnet::MSG_SPECTATE_TOPOLOGY_PROOF_V2
                    || rx.body.size() != sizeof(qnet::SpectateTopologyProofV2)) {
                    fail("phase-c-parent", "Phase-C topology proof was malformed");
                    (void)sidecar.stop(false, ++clock);
                    return false;
                }
                if (state.participant->topologyInfoSent) continue;
                qnet::SpectateTopologyProofV2 proof{};
                std::memcpy(&proof, rx.body.data(), sizeof(proof));
                qnet::ClientIntent proofIntent{};
                proofIntent.intent = qnet::INTENT_SPECTATE;
                proofIntent.spectatorRoute = qnet::SPECTATE_ROUTE_DIRECT;
                proofIntent.spectatorCapabilities =
                    qnet::kSpectateCapabilityReleasedTopologyV2Mask;
                proofIntent.sessionId = static_cast<uint32_t>(kSessionId);
                proofIntent.keyLow = sessionIdentity().cookie.low;
                proofIntent.keyHigh = sessionIdentity().cookie.high;
                proofIntent.viewerId = state.participant->viewerId;
                proofIntent.resumeRecord = 1u;
                qsp::SpectatorGrant proofGrant{};
                if (!controlv2::validateProof(proof, proofIntent,
                                              state.participant->live,
                                              state.participant->challenge,
                                              proofGrant)
                    || !(proofGrant == state.participant->grant)) {
                    fail("phase-c-parent", "production V2 verifier rejected a Phase-C proof");
                    (void)sidecar.stop(false, ++clock);
                    return false;
                }
                topology::TopologyViewerClaim claim{};
                claim.sessionId = proof.grant.sessionId;
                claim.sessionEpoch = proof.sessionEpoch;
                claim.viewerId = proof.viewerId;
                claim.viewerGeneration = proof.viewerGeneration;
                claim.qspEndpoint = RouteEndpoint{proof.qspIpv4BE, proof.qspPort};
                claim.qspProvenance = topology::EndpointProvenance::ProvenDirectQsp;
                claim.qspRouteGeneration = proof.qspRouteGeneration;
                claim.discoveryGeneration = proof.discoveryGeneration;
                claim.compatibility = proof.compatibilityIdentity;
                claim.streamRootIdentity = proof.streamRootIdentity;
                if (state.participant->topologyProofReceived
                    && proof.requestNonce
                        != state.participant->topologyRequestNonce) {
                    fail("phase-c-parent", "Phase-C topology proof changed its request nonce");
                    (void)sidecar.stop(false, ++clock);
                    return false;
                }
                if (state.participant->viewerId == kPhaseCRelayViewerId
                    && !initialRunwayPublished) {
                    if (!publishPhaseCInitialRunway(
                            sidecar, sequence, initialRunwayRecords, ++clock)) {
                        fail("phase-c-parent",
                             "sidecar rejected the initial relay cache runway");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    initialRunwayPublished = true;
                }
                state.participant->claim = claim;
                state.participant->topologyRequestNonce = proof.requestNonce;
                state.participant->topologyLink = state.link;
                state.participant->topologyProofReceived = true;
                std::memset(&proof, 0, sizeof(proof));
                std::memset(&proofGrant, 0, sizeof(proofGrant));
            }
        }
        for (PhaseCControlLink& state : links) {
            if (!state.releaseAfterResponse || state.link == nullptr) continue;
            listener.release(state.link);
            state.link = nullptr;
        }

        const std::array<PhaseCParticipant*, 3u> participants{
            &relayParticipant, &childParticipant, &grandchildParticipant};
        const size_t participantCount = phaseDGrandchild ? 3u : 2u;
        for (size_t participantIndex = 0u;
             participantIndex < participantCount; ++participantIndex) {
            PhaseCParticipant* const participant = participants[participantIndex];
            if (!participant->topologyProofReceived) {
                continue;
            }
            const bool sourceAcknowledged = participant->viewerId
                == kPhaseCRelayViewerId
                ? relaySourceRecordAcknowledged
                : participant->viewerId == kPhaseCChildViewerId
                ? downstreamSourceRecordAcknowledged
                : grandchildSourceRecordAcknowledged;
            if (!sourceAcknowledged) continue;
            if (!participant->topologyEnrolled) {
                topology::TopologyEnrollment enrollment{};
                std::string enrollmentError;
                if (!sidecar.enrollTopologyViewerExact(
                        participant->claim, participant->grant, clock,
                        enrollment, enrollmentError)) {
                    fail("phase-c-parent", enrollmentError.empty()
                        ? "sidecar refused exact Phase-C topology enrollment"
                        : enrollmentError);
                    (void)sidecar.stop(false, ++clock);
                    return false;
                }
                participant->enrollment = enrollment;
                participant->topologyEnrolled = true;
            }
            const bool deferS2Info = pairedActivation
                && participant->viewerId == kPhaseCChildViewerId
                && !activationStaged;
            const bool deferS3Info = phaseDGrandchild
                && participant->viewerId == kPhaseDGrandchildViewerId
                && !grandchildActivationStaged;
            if (participant->topologyInfoSent || deferS2Info || deferS3Info)
                continue;
            if (!sendTopologyInfo(*participant)) {
                fail("phase-c-parent", "could not issue signed Phase-C topology info");
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            participant->topologyInfoSent = true;
        }

        std::vector<std::pair<qnet::Endpoint, PhaseCSignal>> signals;
        if (!phaseCDrainSignals(progressSocket, signals, error)) {
            fail("phase-c-parent", error);
            (void)sidecar.stop(false, ++clock);
            return false;
        }
        for (const auto& item : signals) {
            const qnet::Endpoint& from = item.first;
            const PhaseCSignal& signal = item.second;
            if (signal.type == PhaseCSignalType::Failure) {
                fail("phase-c-parent", "a Phase-C child reported failure");
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            if (signal.type == PhaseCSignalType::Hello) {
                if (signal.value == static_cast<uint32_t>(PhaseCRole::Relay)) {
                    if (relayProgressKnown && !(from == relayProgress)) {
                        fail("phase-c-parent", "conflicting relay progress endpoint");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    relayProgress = from;
                    relayProgressKnown = true;
                } else if (signal.value
                           == static_cast<uint32_t>(PhaseCRole::Downstream)) {
                    if (downstreamProgressKnown && !(from == downstreamProgress)) {
                        fail("phase-c-parent", "conflicting downstream progress endpoint");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    downstreamProgress = from;
                    downstreamProgressKnown = true;
                } else if (signal.value
                           == static_cast<uint32_t>(PhaseCRole::Grandchild)) {
                    if (grandchildProgressKnown
                        && !(from == grandchildProgress)) {
                        fail("phase-d-parent",
                             "conflicting grandchild progress endpoint");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    grandchildProgress = from;
                    grandchildProgressKnown = true;
                }
                continue;
            }
            if (relayProgressKnown && from == relayProgress) {
                switch (signal.type) {
                case PhaseCSignalType::RelayReady:
                    if (signal.value == 0u || signal.value > 65535u) {
                        fail("phase-c-parent", "relay reported an invalid data port");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    relayReady = true;
                    relayDataPort = static_cast<uint16_t>(signal.value);
                    break;
                case PhaseCSignalType::RelaySourceRecordAcknowledged:
                    relaySourceRecordAcknowledged =
                        signal.value >= initialRunwayRecords;
                    break;
                case PhaseCSignalType::RelayAcceptedHello:
                    relayAcceptedHello = true;
                    break;
                case PhaseCSignalType::RelayAcceptedAck:
                    relayAcceptedAck = true;
                    break;
                case PhaseCSignalType::RelayAcceptedDetach:
                    relayAcceptedDetach = true;
                    break;
                case PhaseCSignalType::RelayUpstreamAfterDetach:
                    relayUpstreamAfterDetach =
                        signal.value >= initialRunwayRecords + 1u;
                    break;
                case PhaseCSignalType::RelayRebindSent:
                    if (!completedS2RetirementValidation) {
                        fail("phase-c-parent",
                             "unexpected relay-rebind progress receipt");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    relayRebindSent = true;
                    break;
                default: break;
                }
                continue;
            }
            if (downstreamProgressKnown && from == downstreamProgress) {
                if (signal.type == PhaseCSignalType::DownstreamBootstrapSent) {
                    downstreamBootstrapSent = signal.value != 0u;
                } else if (signal.type
                           == PhaseCSignalType::DownstreamSourceRecordAcknowledged) {
                    downstreamSourceRecordAcknowledged =
                        signal.value >= initialRunwayRecords;
                } else if (signal.type == PhaseCSignalType::DownstreamStreaming) {
                    downstreamStreaming = pairedActivation
                        ? signal.value >= initialRunwayRecords + 1u
                        : signal.value >= 1u;
                } else if (signal.type == PhaseCSignalType::DownstreamDetachSent) {
                    downstreamDetachSent = true;
                } else if (signal.type
                           == PhaseCSignalType::DownstreamActivationCommitted) {
                    if (signal.value == 0u || signal.value > 65535u
                        || (relayDataPort != 0u
                            && signal.value != relayDataPort)) {
                        fail("phase-d-parent",
                             "S2 reported an unexpected final-commit relay endpoint");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    downstreamActivationCommitted = true;
                } else if (signal.type
                           == PhaseCSignalType::DownstreamRelayReady) {
                    if (signal.value == 0u || signal.value > 65535u
                        || signal.value == relayDataPort
                        || signal.value == sidecar.boundPort()) {
                        fail("phase-d-parent", "S2 reported an invalid relay data port");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    downstreamRelayReady = true;
                    downstreamRelayDataPort = static_cast<uint16_t>(signal.value);
                } else if (signal.type
                           == PhaseCSignalType::DownstreamAcceptedHello) {
                    downstreamAcceptedHello = true;
                } else if (signal.type
                           == PhaseCSignalType::DownstreamAcceptedAck) {
                    downstreamAcceptedAck = true;
                } else if (signal.type
                           == PhaseCSignalType::DownstreamAcceptedDetach) {
                    downstreamAcceptedDetach = true;
                } else if (signal.type
                           == PhaseCSignalType::DownstreamTopologyRetiredE0Safe) {
                    if (!completedS2RetirementValidation
                        || signal.value < initialRunwayRecords + 1u) {
                        fail("phase-d-parent",
                             "unexpected or non-E0-safe completed-S2 retirement");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    downstreamTopologyRetiredE0Safe = true;
                }
                continue;
            }
            if (grandchildProgressKnown && from == grandchildProgress) {
                if (signal.type == PhaseCSignalType::GrandchildBootstrapSent) {
                    grandchildBootstrapSent = signal.value != 0u;
                } else if (signal.type
                           == PhaseCSignalType::GrandchildSourceRecordAcknowledged) {
                    grandchildSourceRecordAcknowledged =
                        signal.value >= initialRunwayRecords + 1u;
                } else if (signal.type
                           == PhaseCSignalType::GrandchildActivationCommitted) {
                    if (signal.value == 0u || signal.value > 65535u
                        || (downstreamRelayDataPort != 0u
                            && signal.value != downstreamRelayDataPort)) {
                        fail("phase-d-parent",
                             "S3 reported an unexpected S2 relay endpoint");
                        (void)sidecar.stop(false, ++clock);
                        return false;
                    }
                    grandchildActivationCommitted = true;
                } else if (signal.type == PhaseCSignalType::GrandchildStreaming) {
                    grandchildStreaming = signal.value >= initialRunwayRecords + 2u;
                } else if (signal.type == PhaseCSignalType::GrandchildDetachSent) {
                    grandchildDetachSent = true;
                }
            }
        }

        if (!downstreamLaunched && relayReady && initialRunwayPublished
            && relayParticipant.topologyInfoSent) {
            if (!launchChild(downstreamProcess, completedS2RetirementValidation
                    ? L"--phase-d-downstream-child-retirement"
                    : pairedActivation
                        ? L"--phase-d-downstream-child"
                        : L"--phase-c-downstream-child")) {
                fail("phase-c-parent", error);
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            downstreamLaunched = true;
        }
        if (pairedActivation && !activationStaged && relayReady
            && relayDataPort != 0u && relayParticipant.topologyInfoSent
            && childParticipant.topologyEnrolled) {
            std::string activationError;
            if (sidecar.issueTopologyRelayChildActivation(
                    relayParticipant.enrollment.nodeId,
                    childParticipant.enrollment.nodeId, clock,
                    activationError)) {
                activationStaged = true;
                handoffIssued = true;
            }
        }
        if (pairedActivation && activationStaged
            && childParticipant.topologyEnrolled
            && !childParticipant.topologyInfoSent) {
            if (!sendTopologyInfo(childParticipant)) {
                fail("phase-d-parent",
                     "could not issue staged S2 topology info");
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            childParticipant.topologyInfoSent = true;
        }
        if (phaseDGrandchild && !grandchildLaunched
            && downstreamActivationCommitted && downstreamRelayReady
            && downstreamRelayDataPort != 0u
            && childParticipant.topologyInfoSent) {
            if (!launchChild(grandchildProcess, L"--phase-d-grandchild")) {
                fail("phase-d-parent", error);
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            grandchildLaunched = true;
        }
        if (phaseDGrandchild && !grandchildActivationStaged
            && downstreamActivationCommitted && downstreamRelayReady
            && childParticipant.topologyInfoSent
            && grandchildParticipant.topologyEnrolled) {
            std::string activationError;
            if (sidecar.issueTopologyRelayChildActivation(
                    childParticipant.enrollment.nodeId,
                    grandchildParticipant.enrollment.nodeId, clock,
                    activationError)) {
                grandchildActivationStaged = true;
                grandchildHandoffIssued = true;
            }
        }
        if (phaseDGrandchild && grandchildActivationStaged
            && grandchildParticipant.topologyEnrolled
            && !grandchildParticipant.topologyInfoSent) {
            if (!sendTopologyInfo(grandchildParticipant)) {
                fail("phase-d-parent", "could not issue staged S3 topology info");
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            grandchildParticipant.topologyInfoSent = true;
        }
        if (!pairedActivation && !handoffIssued && relayReady
            && relayDataPort != 0u && downstreamBootstrapSent
            && relayParticipant.topologyInfoSent
            && childParticipant.topologyInfoSent) {
            std::string assignmentError;
            if (sidecar.issueTopologyRelayChildAssignment(
                    relayParticipant.enrollment.nodeId,
                    childParticipant.enrollment.nodeId, clock, assignmentError)) {
                handoffIssued = true;
            }
        }
        if (!pairedActivation && handoffIssued
            && downstreamProgressKnown
            && nowMs() - lastStartSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, downstreamProgress,
                                   {PhaseCSignalType::StartDownstream,
                                    relayDataPort, 0u});
            lastStartSignal = nowMs();
        }
        if (relayAcceptedHello && !postEnrollmentRecordPublished) {
            if (!publishPhaseCRecord(
                    sidecar, sequence, initialRunwayRecords, ++clock)) {
                fail("phase-c-parent", "sidecar rejected post-enrollment verified record");
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            postEnrollmentRecordPublished = true;
        }
        if (completedS2RetirementValidation && relayProgressKnown
            && downstreamActivationCommitted && downstreamRelayReady
            && downstreamStreaming && relayAcceptedHello && relayAcceptedAck
            && !relayRebindSent
            && nowMs() - lastRelayRebindRequestSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, relayProgress,
                                   {PhaseCSignalType::RequestRelayRebind,
                                    0u, 0u});
            lastRelayRebindRequestSignal = nowMs();
        }
        if (!pairedActivation && relayAcceptedAck && downstreamProgressKnown
            && nowMs() - lastAckObservedSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, downstreamProgress,
                                   {PhaseCSignalType::RelayAckObserved, 1u, 0u});
            lastAckObservedSignal = nowMs();
        }
        if (phaseDGrandchild && downstreamAcceptedHello
            && !postGrandchildRecordPublished) {
            if (!publishPhaseCRecord(
                    sidecar, sequence, initialRunwayRecords + 1u, ++clock)) {
                fail("phase-d-parent", "sidecar rejected post-S3 verified record");
                (void)sidecar.stop(false, ++clock);
                return false;
            }
            postGrandchildRecordPublished = true;
        }
        if (phaseDGrandchild && downstreamAcceptedAck && grandchildStreaming
            && grandchildProgressKnown
            && nowMs() - lastGrandchildAckObservedSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, grandchildProgress,
                                   {PhaseCSignalType::RelayAckObserved, 1u, 0u});
            lastGrandchildAckObservedSignal = nowMs();
        }
        if (phaseDGrandchild && downstreamAcceptedDetach && downstreamProgressKnown
            && nowMs() - lastDownstreamStopSignal >= 100u) {
            (void)phaseCSendSignal(progressSocket, downstreamProgress,
                                   {PhaseCSignalType::RelayAckObserved, 1u, 0u});
            lastDownstreamStopSignal = nowMs();
        }
        if ((completedS2RetirementValidation && handoffIssued
             && activationStaged && downstreamActivationCommitted
             && downstreamRelayReady && downstreamStreaming
             && relayAcceptedHello && relayAcceptedAck
             && relayRebindSent && downstreamTopologyRetiredE0Safe
             && initialRunwayPublished && postEnrollmentRecordPublished)
            || (!pairedActivation && handoffIssued
             && relayAcceptedHello && relayAcceptedAck
             && relayAcceptedDetach && downstreamStreaming && downstreamDetachSent
             && initialRunwayPublished && postEnrollmentRecordPublished
             && relayUpstreamAfterDetach)
            || (phaseDGrandchild && handoffIssued && activationStaged
                && downstreamActivationCommitted && downstreamRelayReady
                && grandchildHandoffIssued && grandchildActivationStaged
                && grandchildActivationCommitted && grandchildStreaming
                && grandchildDetachSent && downstreamAcceptedHello
                && downstreamAcceptedAck && downstreamAcceptedDetach
                && relayAcceptedHello && relayAcceptedAck
                && relayAcceptedDetach && downstreamStreaming
                && downstreamDetachSent && initialRunwayPublished
                && postEnrollmentRecordPublished && postGrandchildRecordPublished
                && relayUpstreamAfterDetach)) {
            phaseProofClosed = true;
            break;
        }
        Sleep(1u);
    }

    if (!phaseProofClosed) {
        if (completedS2RetirementValidation) {
            fail("phase-d-parent",
                 "timed out before completed-S2 Type-15 retirement"
                 " (s1Ready=" + std::to_string(relayReady)
                 + ", s2Attached="
                 + std::to_string(downstreamActivationCommitted)
                 + ", s2RelayReady=" + std::to_string(downstreamRelayReady)
                 + ", s2Streaming=" + std::to_string(downstreamStreaming)
                 + ", s1Hello=" + std::to_string(relayAcceptedHello)
                 + ", s1Ack=" + std::to_string(relayAcceptedAck)
                 + ", migrateSent=" + std::to_string(relayRebindSent)
                 + ", s2RetiredE0Safe="
                 + std::to_string(downstreamTopologyRetiredE0Safe)
                 + ", runway=" + std::to_string(initialRunwayPublished)
                 + ", postS2=" + std::to_string(postEnrollmentRecordPublished)
                 + ")");
        } else if (pairedActivation) {
            fail("phase-d-parent",
                 "timed out before real S1->S2->S3 closure"
                 " (s1Ready=" + std::to_string(relayReady)
                 + ", s2Attached=" + std::to_string(downstreamActivationCommitted)
                 + ", s2RelayReady=" + std::to_string(downstreamRelayReady)
                 + ", s3Launched=" + std::to_string(grandchildLaunched)
                 + ", s3Bootstrap=" + std::to_string(grandchildBootstrapSent)
                 + ", s3SourceAck="
                 + std::to_string(grandchildSourceRecordAcknowledged)
                 + ", s3Enrolled="
                 + std::to_string(grandchildParticipant.topologyEnrolled)
                 + ", s3Staged=" + std::to_string(grandchildActivationStaged)
                 + ", s3Info="
                 + std::to_string(grandchildParticipant.topologyInfoSent)
                 + ", s3Attached=" + std::to_string(grandchildActivationCommitted)
                 + ", s3Streaming=" + std::to_string(grandchildStreaming)
                 + ", s3DetachSent=" + std::to_string(grandchildDetachSent)
                 + ", s2Hello=" + std::to_string(downstreamAcceptedHello)
                 + ", s2Ack=" + std::to_string(downstreamAcceptedAck)
                 + ", s2Detach=" + std::to_string(downstreamAcceptedDetach)
                 + ", s2Streaming=" + std::to_string(downstreamStreaming)
                 + ", s2DetachSent=" + std::to_string(downstreamDetachSent)
                 + ", s1Hello=" + std::to_string(relayAcceptedHello)
                 + ", s1Ack=" + std::to_string(relayAcceptedAck)
                 + ", s1Detach=" + std::to_string(relayAcceptedDetach)
                 + ", s1Upstream=" + std::to_string(relayUpstreamAfterDetach)
                 + ", runway=" + std::to_string(initialRunwayPublished)
                 + ", postS2=" + std::to_string(postEnrollmentRecordPublished)
                 + ", postS3=" + std::to_string(postGrandchildRecordPublished)
                 + ")");
        } else {
            fail("phase-c-parent",
                 "timed out before source/relay/downstream closure");
        }
        (void)sidecar.stop(false, ++clock);
        return false;
    }

    DWORD relayExit = 0u;
    DWORD childExit = 0u;
    DWORD grandchildExit = 0u;
    if (phaseDGrandchild) {
        const bool grandchildExited = grandchildLaunched
            && grandchildProcess.waitForExit(kPhaseCChildTimeoutMs,
                                             grandchildExit, error);
        if (!grandchildExited || grandchildExit != 0u) {
            fail("phase-d-parent", error.empty()
                ? "S3 process failed with " + std::to_string(grandchildExit)
                : error);
            (void)sidecar.stop(false, ++clock);
            return false;
        }
        error.clear();
    }
    const bool childExited = downstreamLaunched
        && downstreamProcess.waitForExit(kPhaseCChildTimeoutMs, childExit, error);
    if (!childExited || childExit != 0u) {
        fail(pairedActivation ? "phase-d-parent" : "phase-c-parent",
            error.empty()
                ? "downstream process failed with " + std::to_string(childExit)
                : error);
        (void)sidecar.stop(false, ++clock);
        return false;
    }
    error.clear();
    const bool relayExited = relayProcess.waitForExit(kPhaseCChildTimeoutMs,
                                                      relayExit, error);
    if (!relayExited || relayExit != 0u) {
        fail(pairedActivation ? "phase-d-parent" : "phase-c-parent",
            error.empty()
                ? "released relay child failed with " + std::to_string(relayExit)
                : error);
        (void)sidecar.stop(false, ++clock);
        return false;
    }
    const auto status = sidecar.status();
    const bool legacyPassed = relayParticipant.e0InfoSent
        && relayParticipant.topologyInfoSent && childParticipant.e0InfoSent
        && childParticipant.topologyInfoSent && relayProgressKnown
        && downstreamProgressKnown && relayReady && downstreamBootstrapSent
        && handoffIssued && (!pairedActivation || activationStaged)
        && (!pairedActivation || downstreamActivationCommitted)
        && relayAcceptedHello && relayAcceptedAck
        && relayAcceptedDetach && downstreamStreaming && downstreamDetachSent
        && initialRunwayPublished && postEnrollmentRecordPublished
        && relayUpstreamAfterDetach
        && status.confirmedRecords >= initialRunwayRecords + 1u
        && status.failureReason.empty();
    const bool pairedPassed = relayParticipant.e0InfoSent
        && relayParticipant.topologyInfoSent && childParticipant.e0InfoSent
        && childParticipant.topologyInfoSent
        && grandchildParticipant.e0InfoSent
        && grandchildParticipant.topologyInfoSent
        && relayProgressKnown && downstreamProgressKnown
        && grandchildProgressKnown && relayReady && downstreamBootstrapSent
        && downstreamActivationCommitted && downstreamRelayReady
        && handoffIssued && activationStaged
        && grandchildBootstrapSent && grandchildHandoffIssued
        && grandchildActivationStaged && grandchildActivationCommitted
        && downstreamAcceptedHello && downstreamAcceptedAck
        && downstreamAcceptedDetach && grandchildStreaming && grandchildDetachSent
        && relayAcceptedHello && relayAcceptedAck && relayAcceptedDetach
        && downstreamStreaming && downstreamDetachSent
        && initialRunwayPublished && postEnrollmentRecordPublished
        && postGrandchildRecordPublished && relayUpstreamAfterDetach
        && status.confirmedRecords >= initialRunwayRecords + 2u
        && status.failureReason.empty();
    const bool completedS2RetirementPassed = relayParticipant.e0InfoSent
        && relayParticipant.topologyInfoSent && childParticipant.e0InfoSent
        && childParticipant.topologyInfoSent && relayProgressKnown
        && downstreamProgressKnown && relayReady && downstreamBootstrapSent
        && handoffIssued && activationStaged && downstreamActivationCommitted
        && downstreamRelayReady && relayAcceptedHello && relayAcceptedAck
        && downstreamStreaming && relayRebindSent
        && downstreamTopologyRetiredE0Safe && initialRunwayPublished
        && postEnrollmentRecordPublished
        && status.confirmedRecords >= initialRunwayRecords + 1u
        && status.failureReason.empty();
    const bool passed = completedS2RetirementValidation
        ? completedS2RetirementPassed
        : pairedActivation ? pairedPassed : legacyPassed;
    if (!passed) {
        fail(pairedActivation ? "phase-d-parent" : "phase-c-parent",
             completedS2RetirementValidation
                ? "completed-S2 Type-15 retirement process proof did not close"
                : pairedActivation
                ? "real S1->S2->S3 paired process proof did not close"
                : "source/relay/ordinary-child process proof did not close");
        (void)sidecar.stop(false, ++clock);
        return false;
    }
    (void)sidecar.stop(false, ++clock);
    return true;
}

#pragma pack(push, 1)
struct RelayQsrHandoffWire {
    uint64_t roomLow = 0;
    uint64_t roomHigh = 0;
    uint64_t tokenLow = 0;
    uint64_t tokenHigh = 0;
    uint64_t viewerId = 0;
    uint32_t sourceGeneration = 0;
    uint16_t relayPort = 0;
    uint16_t reserved = 0;
};
#pragma pack(pop)

constexpr uint8_t kRelayQsrHandoffMsg = 0x6Eu;

std::vector<uint8_t> relayFrameRecord(const std::vector<uint8_t>& record) {
    std::array<uint8_t, qnet::kMaxMsgSize> message{};
    const size_t length = qnet::packMsg(
        qnet::relay::kTcpEnvelopeType, record.data(), record.size(),
        message.data(), message.size());
    std::vector<uint8_t> frame(length + 2u, 0u);
    frame[0] = static_cast<uint8_t>(length);
    frame[1] = static_cast<uint8_t>(length >> 8u);
    std::memcpy(frame.data() + 2u, message.data(), length);
    return frame;
}

class RelayTcpPeer {
public:
    ~RelayTcpPeer() { close(); }

    bool open(const uint16_t port) {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = loopbackIpv4BE();
        if (::connect(socket_, reinterpret_cast<const sockaddr*>(&address),
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

    bool send(const std::vector<uint8_t>& bytes) {
        if (socket_ == INVALID_SOCKET) return false;
        size_t offset = 0;
        for (int attempt = 0; attempt < 256 && offset < bytes.size();
             ++attempt) {
            const int sent = ::send(
                socket_,
                reinterpret_cast<const char*>(bytes.data() + offset),
                static_cast<int>(bytes.size() - offset), 0);
            if (sent > 0) {
                offset += static_cast<size_t>(sent);
                continue;
            }
            if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
        }
        return offset == bytes.size();
    }

    bool receiveRecord(qrelay::RelayService& service,
                       std::vector<uint8_t>& body,
                       const int turns = 256) {
        for (int turn = 0; turn < turns; ++turn) {
            if (tryParse(body)) return true;
            service.pumpOnce(0);
            drain();
        }
        return tryParse(body);
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
        const bool ok = qnet::unpackMsg(buffer_.data() + 2u, length, type,
                                        payload, payloadLength);
        if (ok) body.assign(payload, payload + payloadLength);
        buffer_.erase(
            buffer_.begin(),
            buffer_.begin() + static_cast<std::ptrdiff_t>(length + 2u));
        return ok;
    }

    SOCKET socket_ = INVALID_SOCKET;
    std::vector<uint8_t> buffer_;
};

struct RelayPairedRoom {
    RelayTcpPeer host;
    RelayTcpPeer guest;
    qnet::relay::Welcome hostWelcome{};
    qnet::relay::Welcome guestWelcome{};
};

std::vector<uint8_t> relayPlayerHello(const qnet::relay::RoomId& room,
                                      const qnet::relay::RelayRole role) {
    qnet::relay::Hello hello{};
    hello.room = room;
    hello.domain = qnet::relay::LaneDomain::Player;
    hello.role = role;
    std::vector<uint8_t> encoded;
    qnet::relay::encodeHello(hello, encoded);
    return relayFrameRecord(encoded);
}

bool relayPairPlayerRoom(qrelay::RelayService& service,
                         const uint16_t port,
                         const qnet::relay::RoomId& room,
                         RelayPairedRoom& out) {
    std::vector<uint8_t> body;
    if (!out.host.open(port)
        || !out.host.send(relayPlayerHello(room, qnet::relay::RelayRole::Host))
        || !out.host.receiveRecord(service, body)) {
        return false;
    }
    qnet::relay::Welcome waiting{};
    if (qnet::relay::decodeWelcome(body.data(), body.size(), waiting)
            != qnet::relay::DecodeError::None
        || waiting.status != qnet::relay::WelcomeStatus::Waiting) {
        return false;
    }
    if (!out.guest.open(port)
        || !out.guest.send(
            relayPlayerHello(room, qnet::relay::RelayRole::Client))
        || !out.guest.receiveRecord(service, body)) {
        return false;
    }
    if (qnet::relay::decodeWelcome(body.data(), body.size(), out.guestWelcome)
            != qnet::relay::DecodeError::None
        || out.guestWelcome.status != qnet::relay::WelcomeStatus::Accepted) {
        return false;
    }
    if (!out.host.receiveRecord(service, body)) return false;
    return qnet::relay::decodeWelcome(body.data(), body.size(),
                                      out.hostWelcome)
            == qnet::relay::DecodeError::None
        && out.hostWelcome.status == qnet::relay::WelcomeStatus::Accepted;
}

bool relayAdmitViewer(qrelay::RelayService& service,
                      const uint16_t port,
                      const qnet::relay::RoomId& room,
                      const qsp::SpectatorGrant& grant,
                      RelayTcpPeer& lease,
                      qnet::relay::SpectatorWelcome& welcome) {
    qnet::relay::SpectatorHello hello{};
    hello.room = room;
    hello.party = qnet::relay::SpectatorParty::Viewer;
    hello.route = qnet::relay::SpectatorRouteKind::Relay;
    hello.admission = qnet::relay::SpectatorAdmission::Grant;
    hello.sessionId = grant.sessionId;
    hello.grantId = grant.grantId;
    hello.viewerId = grant.viewerId;
    hello.expiryUnix = grant.expiryUnix;
    hello.sourceGeneration = grant.sourceGeneration;
    hello.resumeRecord = grant.resumeRecord;
    hello.resumeDigest = grant.resumeDigest;
    hello.macLow = grant.macLow;
    hello.macHigh = grant.macHigh;
    std::vector<uint8_t> encoded;
    if (!qnet::relay::encodeSpectatorHello(hello, encoded)) return false;
    if (!lease.open(port) || !lease.send(relayFrameRecord(encoded)))
        return false;
    std::vector<uint8_t> body;
    if (!lease.receiveRecord(service, body)) return false;
    return qnet::relay::decodeSpectatorWelcome(body.data(), body.size(),
                                               welcome)
        == qnet::relay::DecodeError::None;
}

int runRelayQsrChild(const char* const relayPortText,
                     const char* const controlPortText) {
    uint16_t relayPort = 0u;
    uint16_t controlPort = 0u;
    if (!parsePort(relayPortText, relayPort)
        || !parsePort(controlPortText, controlPort)) {
        return 2;
    }
    NetScope network;
    if (!network.start()) return 3;

    const qnet::Endpoint relayEndpoint = loopbackEndpoint(relayPort);
    const qnet::Endpoint hostControl = loopbackEndpoint(controlPort);

    qnet::SpectateInfo e0{};
    RelayQsrHandoffWire handoff{};
    {
        qnet::UdpSock socket;
        if (!socket.open(0u)) {
            fail("relay-qsr-child", "could not open E0 control socket");
            return 4;
        }
        qnet::UdpControlLink link;
        link.startClient(socket, hostControl, nowMs());
        const uint64_t startedAt = nowMs();
        bool sentVersion = false;
        bool infoSeen = false;
        bool handoffSeen = false;
        while (!elapsed(startedAt, 8000u) && (!infoSeen || !handoffSeen)) {
            link.poll(0);
            if (!link.isOpen()) {
                fail("relay-qsr-child", "E0 control link closed");
                return 4;
            }
            for (unsigned budget = 0u; budget < 8u; ++budget) {
                qnet::ControlLink::Received rx{};
                const qnet::ControlLink::Rs result = link.recvMsgEx(rx, 0);
                if (result == qnet::ControlLink::Rs::Timeout) break;
                if (result != qnet::ControlLink::Rs::Msg) {
                    fail("relay-qsr-child", "E0 control receive failed");
                    return 4;
                }
                if (!sentVersion) {
                    qnet::VersionConfig version{};
                    if (rx.type != qnet::MSG_VERSION_CONFIG
                        || rx.body.size() != sizeof(version)) {
                        fail("relay-qsr-child", "host version malformed");
                        return 4;
                    }
                    qnet::VersionConfig local{qnet::kProtocolVersion};
                    qnet::ClientIntent intent{};
                    intent.intent = qnet::INTENT_SPECTATE;
                    intent.spectatorRoute = qnet::SPECTATE_ROUTE_RELAY;
                    intent.sessionId = static_cast<uint32_t>(kSessionId);
                    intent.keyLow = sessionIdentity().cookie.low;
                    intent.keyHigh = sessionIdentity().cookie.high;
                    intent.viewerId = kViewerId;
                    intent.resumeRecord = 0u;
                    if (!link.sendMsg(qnet::MSG_VERSION_CONFIG, &local,
                                      sizeof(local))
                        || !link.sendMsg(qnet::MSG_CLIENT_INTENT, &intent,
                                         sizeof(intent))) {
                        fail("relay-qsr-child", "could not send E0 intent");
                        return 4;
                    }
                    sentVersion = true;
                    continue;
                }
                if (!infoSeen) {
                    if (rx.type != qnet::MSG_SPECTATE_INFO
                        || rx.body.size() != sizeof(e0)) {
                        fail("relay-qsr-child", "E0 response was not info");
                        return 4;
                    }
                    std::memcpy(&e0, rx.body.data(), sizeof(e0));
                    infoSeen = true;
                    continue;
                }
                if (rx.type != kRelayQsrHandoffMsg
                    || rx.body.size() != sizeof(handoff)) {
                    fail("relay-qsr-child", "relay handoff was malformed");
                    return 4;
                }
                std::memcpy(&handoff, rx.body.data(), sizeof(handoff));
                handoffSeen = true;
            }
            Sleep(1u);
        }
        link.close();
        if (!infoSeen || !handoffSeen || handoff.relayPort != relayPort
            || handoff.reserved != 0u || handoff.sourceGeneration == 0u) {
            fail("relay-qsr-child",
                 std::string("relay handoff was incomplete (info ")
                     + (infoSeen ? "1" : "0") + " handoff "
                     + (handoffSeen ? "1" : "0") + " port "
                     + std::to_string(handoff.relayPort) + " generation "
                     + std::to_string(handoff.sourceGeneration) + ")");
            return 4;
        }
    }

    qsp::SpectatorGrant grant{};
    {
        const qnet::SpectatorGrantWire& wireGrant = e0.grant;
        for (const uint8_t byte : wireGrant.reserved)
            if (byte != 0u) return 5;
        if (wireGrant.grantId == 0u || wireGrant.viewerId != kViewerId
            || wireGrant.sessionId != kSessionId || wireGrant.expiryUnix == 0u
            || wireGrant.sourceGeneration != handoff.sourceGeneration
            || wireGrant.route != qnet::SPECTATE_ROUTE_RELAY
            || (wireGrant.macLow == 0u && wireGrant.macHigh == 0u)) {
            fail("relay-qsr-child", "E0 grant was not a relay viewer grant");
            return 5;
        }
        grant.grantId = wireGrant.grantId;
        grant.viewerId = wireGrant.viewerId;
        grant.sessionId = wireGrant.sessionId;
        grant.expiryUnix = wireGrant.expiryUnix;
        grant.sourceGeneration = wireGrant.sourceGeneration;
        grant.resumeRecord = wireGrant.resumeRecord;
        grant.resumeDigest = wireGrant.resumeDigest;
        grant.route = wireGrant.route;
        grant.macLow = wireGrant.macLow;
        grant.macHigh = wireGrant.macHigh;
    }

    qnet::UdpSock qspSocket;
    if (!qspSocket.open(0u) || qspSocket.boundPort() == 0u) {
        fail("relay-qsr-child", "could not open QSP viewer socket");
        return 6;
    }
    qsp::RelayViewerBinding viewerBinding{};
    viewerBinding.enabled = true;
    viewerBinding.room =
        qnet::relay::RoomId{handoff.roomLow, handoff.roomHigh};
    viewerBinding.token =
        qnet::relay::RegistrationToken{handoff.tokenLow, handoff.tokenHigh};
    viewerBinding.viewerId = handoff.viewerId;
    viewerBinding.sourceGeneration = handoff.sourceGeneration;
    viewerBinding.identity = sessionIdentity();
    qsp::RelayViewerAdapter relayViewer;
    if (!relayViewer.configure(viewerBinding)) {
        fail("relay-qsr-child", "relay viewer binding was incomplete");
        return 6;
    }

    qsp::ViewerConfig viewerConfig{};
    viewerConfig.identity = sessionIdentity();
    viewerConfig.simulationContract = kSimulationContract;
    viewerConfig.compatibilityIdentity = compatibilityIdentity();
    viewerConfig.secureRandom = kViewerId;
    viewerConfig.viewerNonce = kViewerId;
    qsp::SpectatorViewer viewer(viewerConfig);
    if (!viewer.isConfigured() || !viewer.begin(kGameId)) {
        fail("relay-qsr-child", "SpectatorViewer was not configured");
        return 7;
    }

    topology::client::ReleasedTopologyClientRuntime runtime;
    bool initAckSent = false;
    bool runtimeStarted = false;
    bool detachArmed = false;
    bool detachSent = false;
    bool relaySnapshotObserved = false;
    const uint64_t started = nowMs();
    std::array<uint8_t, 4096u> inbound{};
    std::vector<uint8_t> unwrapped;
    std::vector<uint8_t> framed;
    while (!elapsed(started, kPumpTimeoutMs)) {
        const uint64_t now = nowMs();
        if (!relayViewer.ready() && !relayViewer.detached()) {
            std::vector<uint8_t> registration;
            if (relayViewer.encodeRegistration(now, registration)) {
                (void)qspSocket.sendTo(relayEndpoint, registration.data(),
                                       registration.size());
            }
        }
        for (unsigned budget = 0u; budget < 16u; ++budget) {
            qnet::Endpoint sender{};
            const int received = qspSocket.recvFrom(
                inbound.data(), inbound.size(), sender);
            if (received == 0) break;
            if (received < 0) {
                fail("relay-qsr-child", "QSP receive failed");
                return 8;
            }
            if (!(sender == relayEndpoint)) continue;
            const qsp::RelayViewerIngress ingress = relayViewer.decodeInbound(
                true, inbound.data(), static_cast<size_t>(received),
                unwrapped);
            if (ingress == qsp::RelayViewerIngress::Detached) {
                fail("relay-qsr-child", "relay detached the E0 viewer early");
                return 8;
            }
            if (ingress != qsp::RelayViewerIngress::Data) continue;
            if (!qspIngressAccepted(viewer.onDatagram(
                    unwrapped.data(), unwrapped.size(), now))) {
                fail("relay-qsr-child", "relayed QSP ingress was rejected");
                return 9;
            }
        }

        for (const qsp::ViewerDatagram& packet : viewer.pollOutbound(now, 4u)) {
            if (!relayViewer.encodeOutbound(packet.bytes.data(),
                                            packet.bytes.size(), framed)
                || !qspSocket.sendTo(relayEndpoint, framed.data(),
                                     framed.size())) {
                fail("relay-qsr-child", "relayed QSP send failed");
                return 10;
            }
            if (packet.type == qsp::MessageType::InitAck) {
                if (initAckSent || runtimeStarted) {
                    fail("relay-qsr-child", "duplicate InitAck");
                    return 11;
                }
                initAckSent = true;
                topology::client::ReleasedTopologyClientConfig config{};
                config.enabled = true;
                config.hostControl = hostControl;
                config.qspEndpoint = RouteEndpoint{loopbackIpv4BE(),
                                                   relayPort};
                config.rootQspEndpoint = RouteEndpoint{loopbackIpv4BE(),
                                                       relayPort};
                config.qspRoute = qnet::SPECTATE_ROUTE_RELAY;
                config.qspProvenance =
                    topology::EndpointProvenance::ProvenRelayQsp;
                config.qspRouteGeneration = viewer.generation();
                config.viewerId = viewer.viewerId();
                config.viewerGeneration = viewer.generation();
                config.sessionId = kSessionId;
                config.sessionEpoch = kEpoch;
                config.spectatorCookieLow = e0.keyLow;
                config.spectatorCookieHigh = e0.keyHigh;
                config.compatibility = e0.compatibilityIdentity;
                config.streamRootIdentity = qsp::gameStartFingerprint(
                    qsp::canonicalGameStart(gameDescriptor()));
                config.grant = grant;
                config.relayQsrPermitted = true;
                if (!runtime.start(config, now)
                    || !runtime.onQspInitAckTransmitted(now)
                    || !runtime.updateVerifiedQspFrontier(
                        viewer.receivedNextRecord(),
                        viewer.receivedNextEvent(),
                        viewer.receivedDigest())) {
                    fail("relay-qsr-child",
                         "runtime refused the relayed InitAck bootstrap");
                    return 12;
                }
                runtimeStarted = true;
            }
            if (packet.type == qsp::MessageType::Detach) detachSent = true;
        }

        if (runtimeStarted
            && runtime.status().state
                != topology::client::ReleasedTopologyClientState::Unavailable) {
            if (!runtime.updateVerifiedQspFrontier(
                    viewer.receivedNextRecord(), viewer.receivedNextEvent(),
                    viewer.receivedDigest())) {
                fail("relay-qsr-child", "runtime rejected the QSP frontier");
                return 13;
            }
            runtime.service(now);
        }
        const topology::client::ReleasedTopologyClientStatus status =
            runtime.status();
        if (status.state
            == topology::client::ReleasedTopologyClientState::Unavailable) {
            fail("relay-qsr-child",
                 std::string("relayed topology runtime failed: ")
                     + status.reason);
            return 14;
        }
        if (runtime.attached() && !relaySnapshotObserved) {
            topology::client::ReleasedTopologyAttachmentSnapshot snapshot{};
            if (!runtime.attachmentSnapshot(snapshot) || !snapshot.valid()
                || snapshot.childBinding.childQspRoute.kind
                    != model::RouteKind::TrafficRelay
                || snapshot.childBinding.childQspRoute.ipv4BE
                    != qsp::kRelayVirtualIpv4BE
                || snapshot.committedCursor.nextRecord
                    != viewer.receivedNextRecord()
                || snapshot.credentialDeadlineMs <= now) {
                fail("relay-qsr-child",
                     "relayed attachment snapshot was not exact");
                return 23;
            }
            relaySnapshotObserved = true;
        }
        if (runtime.attached() && relaySnapshotObserved
            && viewer.receivedNextRecord() >= 1u && !detachArmed) {
            if (!viewer.detach()) {
                fail("relay-qsr-child", "viewer refused detach after attach");
                return 15;
            }
            detachArmed = true;
        }
        if (detachSent) {
            runtime.stop();
            return 0;
        }
        Sleep(1u);
    }
    fail("relay-qsr-child", "timed out before relayed QSR completion");
    return 16;
}

bool runRelayQsrParent() {
    NetScope network;
    if (!network.start()) {
        fail("relay-qsr-parent", "netInit failed");
        return false;
    }

    qrelay::RelayService relay;
    qrelay::RelayServiceConfig relayConfig{};
    relayConfig.bindAddress = "127.0.0.1";
    relayConfig.logToConsole = false;
    relayConfig.metricsIntervalMs = 0;
    relayConfig.maximumConnectionsPerIp = 64;
    relayConfig.maximumViewersPerIp = 32;
    relayConfig.maximumTopologyNodesPerIp = 32;
    relayConfig.connectionsPerIpPerSecond = 4096;
    std::string error;
    bool relayStarted = false;
    for (uint16_t candidate = 27601; candidate < 27900 && !relayStarted;
         ++candidate) {
        relayConfig.port = candidate;
        if (!relay.configure(relayConfig, error)) break;
        relayStarted = relay.start(error);
    }
    if (!relayStarted) {
        fail("relay-qsr-parent",
             error.empty() ? "in-process relay could not start" : error);
        return false;
    }
    const uint16_t relayPort = relay.boundPort();

    const qnet::relay::RoomId room{0x51535252454C4159ull,
                                   0x544F504F51535252ull};
    RelayPairedRoom paired;
    if (!relayPairPlayerRoom(relay, relayPort, room, paired)) {
        fail("relay-qsr-parent", "player room did not pair on the relay");
        return false;
    }
    if (!qnet::relay::isValidId(
            paired.hostWelcome.spectatorRegistrationToken)) {
        fail("relay-qsr-parent", "host welcome carried no room capability");
        return false;
    }

    uint64_t clock = kInitialNowMs;
    uint64_t sequence = 1u;
    SidecarConfig config = sidecarConfig();
    config.unixClock = {};
    config.sourceRelay.enabled = true;
    config.sourceRelay.relay = RouteEndpoint{loopbackIpv4BE(), relayPort};
    config.sourceRelay.room = room;
    config.sourceRelay.sourceToken =
        paired.hostWelcome.spectatorRegistrationToken;
    config.sourceRelay.identity = sessionIdentity();
    SpectatorSidecar sidecar(config);
    if (!sidecar.start(qclient::spectator::makeUdpSidecarTransport(), nullptr,
                       clock, error)
        || sidecar.boundPort() == 0u) {
        fail("relay-qsr-parent",
             error.empty() ? "sidecar QSP bind failed" : error);
        return false;
    }
    if (!publishStart(sidecar, sequence, ++clock)) {
        fail("relay-qsr-parent", "sidecar rejected the descriptor");
        return false;
    }
    if (!sidecar.configureTopologyAuthority(
            activationRequest(sidecar.boundPort()), ++clock, error)) {
        fail("relay-qsr-parent",
             error.empty() ? "QSR authority did not activate" : error);
        return false;
    }

    const uint64_t bringupStarted = nowMs();
    bool laneReady = false;
    while (!elapsed(bringupStarted, 10000u)) {
        relay.pumpOnce(0);
        sidecar.service(++clock);
        if (sidecar.relayTopologyLaneAvailable()) {
            laneReady = true;
            break;
        }
        Sleep(1u);
    }
    if (!laneReady) {
        fail("relay-qsr-parent",
             "authority topology lane did not enroll on the relay");
        return false;
    }

    qsp::SpectatorGrant issuedGrant{};
    GrantRequest request{};
    request.viewerId = kViewerId;
    request.route = qnet::SPECTATE_ROUTE_RELAY;
    request.peerIpv4BE = 0u;
    request.lifetimeSeconds = 300u;
    AdmissionDenyReason denied = AdmissionDenyReason::None;
    if (!sidecar.issueGrant(request, issuedGrant, denied, ++clock)
        || issuedGrant.viewerId != kViewerId
        || issuedGrant.route != qnet::SPECTATE_ROUTE_RELAY
        || issuedGrant.sourceGeneration == 0u) {
        fail("relay-qsr-parent", "sidecar could not issue the relay grant");
        return false;
    }
    RelayTcpPeer viewerLease;
    qnet::relay::SpectatorWelcome viewerWelcome{};
    if (!relayAdmitViewer(relay, relayPort, room, issuedGrant, viewerLease,
                          viewerWelcome)
        || (viewerWelcome.status != qnet::relay::SpectatorStatus::Accepted
            && viewerWelcome.status != qnet::relay::SpectatorStatus::Waiting)
        || viewerWelcome.viewerId != kViewerId
        || !qnet::relay::isValidId(viewerWelcome.registrationToken)) {
        fail("relay-qsr-parent",
             "relay refused the production viewer grant (status "
                 + std::to_string(static_cast<unsigned>(viewerWelcome.status))
                 + ")");
        return false;
    }

    qnet::UdpSock controlSocket;
    if (!controlSocket.open(0u) || controlSocket.boundPort() == 0u) {
        fail("relay-qsr-parent", "could not open host control socket");
        return false;
    }
    qnet::UdpControlListener listener;
    listener.open(controlSocket);

    HiddenProcess child;
    const std::wstring childArguments = L"--relay-qsr-child "
        + std::to_wstring(relayPort) + L" "
        + std::to_wstring(controlSocket.boundPort());
    if (!child.launch(childArguments, error)) {
        fail("relay-qsr-parent", error);
        return false;
    }

    const qnet::SpectateInfo live = makeLiveInfo(relayPort, issuedGrant);
    RelayQsrHandoffWire handoff{};
    handoff.roomLow = room.low;
    handoff.roomHigh = room.high;
    handoff.tokenLow = viewerWelcome.registrationToken.low;
    handoff.tokenHigh = viewerWelcome.registrationToken.high;
    handoff.viewerId = kViewerId;
    handoff.sourceGeneration = issuedGrant.sourceGeneration;
    handoff.relayPort = relayPort;

    qnet::UdpControlLink* e0Link = nullptr;
    TopologyAttempt attempt{};
    bool e0Accepted = false;
    bool topologyAccepted = false;
    bool e0VersionSeen = false;
    bool e0IntentSeen = false;
    bool e0InfoSent = false;
    uint64_t e0ReleaseAt = 0u;
    bool topologyResponseSent = false;
    uint64_t topologyReleaseAt = 0u;
    bool publishedRecord = false;
    bool attachmentObserved = false;
    bool enrollmentWasRelay = false;
    const uint64_t started = nowMs();
    while (!elapsed(started, kPhaseCPumpTimeoutMs)) {
        relay.pumpOnce(0);
        sidecar.service(++clock);
        listener.pump(0);
        qnet::UdpControlLink* const accepted = listener.acceptOne();
        if (accepted != nullptr) {
            if (!e0Accepted) {
                e0Link = accepted;
                e0Accepted = true;
                qnet::VersionConfig version{qnet::kProtocolVersion};
                if (!e0Link->sendMsg(qnet::MSG_VERSION_CONFIG, &version,
                                     sizeof(version))) {
                    fail("relay-qsr-parent", "could not greet E0 child");
                    return false;
                }
            } else if (!topologyAccepted) {
                attempt.link = accepted;
                topologyAccepted = true;
                qnet::VersionConfig version{qnet::kProtocolVersion};
                if (!attempt.link->sendMsg(qnet::MSG_VERSION_CONFIG, &version,
                                           sizeof(version))) {
                    fail("relay-qsr-parent", "could not greet topology child");
                    return false;
                }
            } else {
                fail("relay-qsr-parent", "unexpected third control client");
                return false;
            }
        }

        std::vector<qnet::ControlLink::Received> messages;
        if (!readLinkMessages(e0Link, messages, error)) {
            fail("relay-qsr-parent", error);
            return false;
        }
        for (const qnet::ControlLink::Received& rx : messages) {
            if (!e0VersionSeen) {
                qnet::VersionConfig version{};
                if (rx.type != qnet::MSG_VERSION_CONFIG
                    || rx.body.size() != sizeof(version)) {
                    fail("relay-qsr-parent", "E0 child version malformed");
                    return false;
                }
                e0VersionSeen = true;
                continue;
            }
            if (!e0IntentSeen) {
                qnet::ClientIntent intent{};
                if (rx.type != qnet::MSG_CLIENT_INTENT
                    || rx.body.size() != sizeof(intent)) {
                    fail("relay-qsr-parent", "E0 child intent malformed");
                    return false;
                }
                std::memcpy(&intent, rx.body.data(), sizeof(intent));
                if (!validIntent(intent, 0u, 0u, qnet::SPECTATE_ROUTE_RELAY)
                    || !e0Link->sendMsg(qnet::MSG_SPECTATE_INFO, &live,
                                        sizeof(live))
                    || !e0Link->sendMsg(kRelayQsrHandoffMsg, &handoff,
                                        sizeof(handoff))) {
                    fail("relay-qsr-parent", "E0 admission could not respond");
                    return false;
                }
                e0IntentSeen = true;
                e0InfoSent = true;
                e0ReleaseAt = nowMs() + 250u;
                continue;
            }
            fail("relay-qsr-parent", "unexpected E0 control data");
            return false;
        }

        messages.clear();
        if (!readLinkMessages(attempt.link, messages, error)) {
            fail("relay-qsr-parent", error);
            return false;
        }
        for (const qnet::ControlLink::Received& rx : messages) {
            if (!attempt.versionSeen) {
                qnet::VersionConfig version{};
                if (rx.type != qnet::MSG_VERSION_CONFIG
                    || rx.body.size() != sizeof(version)) {
                    fail("relay-qsr-parent", "topology version malformed");
                    return false;
                }
                attempt.versionSeen = true;
                continue;
            }
            if (!attempt.intentSeen) {
                qnet::ClientIntent intent{};
                if (rx.type != qnet::MSG_CLIENT_INTENT
                    || rx.body.size() != sizeof(intent)) {
                    fail("relay-qsr-parent", "topology intent malformed");
                    return false;
                }
                std::memcpy(&intent, rx.body.data(), sizeof(intent));
                if (!validIntent(
                        intent,
                        qnet::kSpectateCapabilityReleasedTopologyV2Mask, 1u,
                        qnet::SPECTATE_ROUTE_RELAY)) {
                    fail("relay-qsr-parent",
                         "topology intent did not bind relay E0 facts");
                    return false;
                }
                attempt.intentSeen = true;
                attempt.challenge = {};
                attempt.challenge.schemaVersion =
                    qnet::kSpectateTopologyControlSchemaVersion;
                attempt.challenge.requiredFeatures =
                    qnet::kSpectateTopologyControlRequiredFeatures;
                attempt.challenge.sessionId = kSessionId;
                attempt.challenge.viewerId = kViewerId;
                attempt.challenge.viewerGeneration = 1u;
                for (size_t index = 0u;
                     index < sizeof(attempt.challenge.peerChallenge);
                     ++index) {
                    attempt.challenge.peerChallenge[index] =
                        static_cast<uint8_t>(0xC5u + index);
                }
                if (attempt.link == nullptr
                    || !attempt.link->sendMsg(
                        qnet::MSG_SPECTATE_TOPOLOGY_CHALLENGE_V2,
                        &attempt.challenge, sizeof(attempt.challenge))) {
                    fail("relay-qsr-parent", "could not issue V2 challenge");
                    return false;
                }
                attempt.challengeIssued = true;
                continue;
            }
            qnet::SpectateTopologyProofV2 candidate{};
            if (rx.type != qnet::MSG_SPECTATE_TOPOLOGY_PROOF_V2
                || rx.body.size() != sizeof(candidate)) {
                fail("relay-qsr-parent", "topology proof malformed");
                return false;
            }
            std::memcpy(&candidate, rx.body.data(), sizeof(candidate));
            if (attempt.proofReceived) continue;
            qnet::ClientIntent proofIntent{};
            proofIntent.intent = qnet::INTENT_SPECTATE;
            proofIntent.spectatorRoute = qnet::SPECTATE_ROUTE_RELAY;
            proofIntent.spectatorCapabilities =
                qnet::kSpectateCapabilityReleasedTopologyV2Mask;
            proofIntent.sessionId = static_cast<uint32_t>(kSessionId);
            proofIntent.keyLow = sessionIdentity().cookie.low;
            proofIntent.keyHigh = sessionIdentity().cookie.high;
            proofIntent.viewerId = kViewerId;
            proofIntent.resumeRecord = 1u;
            qsp::SpectatorGrant candidateGrant{};
            if (!controlv2::validateProof(candidate, proofIntent, live,
                                          attempt.challenge, candidateGrant)
                || !(candidateGrant == issuedGrant)
                || candidate.qspRoute != qnet::SPECTATE_ROUTE_RELAY) {
                fail("relay-qsr-parent",
                     "V2 proof did not carry the exact relay E0 grant");
                return false;
            }
            attempt.proofReceived = true;
            attempt.claim.sessionId = candidate.grant.sessionId;
            attempt.claim.sessionEpoch = candidate.sessionEpoch;
            attempt.claim.viewerId = candidate.viewerId;
            attempt.claim.viewerGeneration = candidate.viewerGeneration;
            attempt.claim.qspEndpoint =
                RouteEndpoint{candidate.qspIpv4BE, candidate.qspPort};
            attempt.claim.qspProvenance =
                topology::EndpointProvenance::ProvenRelayQsp;
            attempt.claim.qspRouteGeneration = candidate.qspRouteGeneration;
            attempt.claim.discoveryGeneration = candidate.discoveryGeneration;
            attempt.claim.compatibility = candidate.compatibilityIdentity;
            attempt.claim.streamRootIdentity = candidate.streamRootIdentity;

            topology::TopologyEnrollment enrollment{};
            std::string enrollError;
            bool enrolled = false;
            for (unsigned tries = 0u; tries < 20u && !enrolled; ++tries) {
                relay.pumpOnce(0);
                sidecar.service(++clock);
                enrolled = sidecar.enrollTopologyViewerExact(
                    attempt.claim, candidateGrant, clock, enrollment,
                    enrollError);
                if (!enrolled
                    && enrollError
                        != "topology enrollment requires a current proved QSP streaming generation") {
                    break;
                }
                if (!enrolled) Sleep(10u);
            }
            if (!enrolled) {
                fail("relay-qsr-parent",
                     "relay-ingress enrollment failed: " + enrollError);
                return false;
            }
            enrollmentWasRelay = enrollment.qspProvenance
                    == topology::EndpointProvenance::ProvenRelayQsp
                && enrollment.qspEndpoint.ipv4BE == qsp::kRelayVirtualIpv4BE
                && enrollment.relayQsrOffer.present
                && enrollment.relayQsrOffer.grant.nodeId == enrollment.nodeId;
            if (!enrollmentWasRelay) {
                fail("relay-qsr-parent",
                     "enrollment did not carry the relay-ingress offer");
                return false;
            }
            qnet::SpectateTopologyInfoV2 response{};
            if (!controlv2::infoFromEnrollment(candidate.requestNonce,
                                               enrollment, response)
                || response.qspRoute != qnet::SPECTATE_ROUTE_RELAY
                || !controlv2::signInfo(response, candidateGrant)
                || attempt.link == nullptr
                || !attempt.link->sendMsg(qnet::MSG_SPECTATE_TOPOLOGY_INFO_V2,
                                          &response, sizeof(response))) {
                fail("relay-qsr-parent",
                     "signed relayed Info-V2 could not be delivered");
                return false;
            }
            std::memset(&response, 0, sizeof(response));
            topologyResponseSent = true;
            topologyReleaseAt = nowMs() + 250u;
            if (!publishVerifiedRecord(sidecar, sequence, ++clock)) {
                fail("relay-qsr-parent", "could not publish the record");
                return false;
            }
            publishedRecord = true;
        }

        const auto status = sidecar.status();
        if (status.topologyAttached >= 1u) attachmentObserved = true;
        if (e0Link != nullptr && e0InfoSent && nowMs() >= e0ReleaseAt) {
            listener.release(e0Link);
            e0Link = nullptr;
        }
        if (attempt.link != nullptr && topologyResponseSent
            && nowMs() >= topologyReleaseAt) {
            listener.release(attempt.link);
            attempt.link = nullptr;
        }
        if (attachmentObserved && publishedRecord
            && status.topologyAttached == 0u && status.topologyEnrolled == 0u
            && status.viewerCount == 0u) {
            break;
        }
        Sleep(1u);
    }

    DWORD childExit = 0u;
    if (!child.waitForExit(kPhaseCChildTimeoutMs, childExit, error)
        || childExit != 0u) {
        const qrelay::RelayServiceMetrics failureMetrics = relay.snapshot();
        fail("relay-qsr-parent",
             (error.empty()
                  ? "relayed child failed with " + std::to_string(childExit)
                  : error)
                 + " (lane " + (laneReady ? "up" : "down") + " nodes "
                 + std::to_string(failureMetrics.topologyNodesAdmitted)
                 + " in " + std::to_string(failureMetrics.topologyPacketsIn)
                 + " drops " + std::to_string(failureMetrics.drops)
                 + " grantRejects "
                 + std::to_string(failureMetrics.topologyGrantRejects) + ")");
        return false;
    }

    const qrelay::RelayServiceMetrics metrics = relay.snapshot();
    const bool relayCarriedQsr = metrics.topologyNodesAdmitted >= 1u
        && metrics.topologyPacketsIn > 0u;
    const bool passed = attachmentObserved && publishedRecord
        && enrollmentWasRelay && relayCarriedQsr
        && sidecar.relayTopologyLaneAvailable();
    if (!passed) {
        fail("relay-qsr-parent",
             "relayed QSR fallback process proof did not close");
    }
    (void)sidecar.stop(false, ++clock);
    relay.requestStop();
    relay.drain();
    return passed;
}

}

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--relay-qsr-child") == 0)
        return runRelayQsrChild(argv[2], argv[3]);
    if (argc == 2 && std::strcmp(argv[1], "--relay-qsr-validation") == 0)
        return runRelayQsrParent() ? 0 : 1;
    if (argc == 5 && std::strcmp(argv[1], "--phase-c-relay-child") == 0)
        return runPhaseCRelayChild(argv[2], argv[3], argv[4], false);
    if (argc == 5 && std::strcmp(argv[1], "--phase-c-downstream-child") == 0)
        return runPhaseCDownstreamChild(argv[2], argv[3], argv[4], false);
    if (argc == 5 && std::strcmp(argv[1], "--phase-d-relay-child") == 0)
        return runPhaseCRelayChild(argv[2], argv[3], argv[4], true);
    if (argc == 5
        && std::strcmp(argv[1], "--phase-d-relay-child-retirement") == 0) {
        return runPhaseCRelayChild(argv[2], argv[3], argv[4], true, true);
    }
    if (argc == 5 && std::strcmp(argv[1], "--phase-d-downstream-child") == 0)
        return runPhaseCDownstreamChild(argv[2], argv[3], argv[4], true);
    if (argc == 5
        && std::strcmp(argv[1], "--phase-d-downstream-child-retirement") == 0) {
        return runPhaseCDownstreamChild(argv[2], argv[3], argv[4], true, true);
    }
    if (argc == 5 && std::strcmp(argv[1], "--phase-d-grandchild") == 0)
        return runPhaseDGrandchild(argv[2], argv[3], argv[4]);
    if (argc == 4
        && std::strcmp(argv[1], "--client-root-viewer-child") == 0) {
        return runClientRootViewerChild(argv[2], argv[3]);
    }
    if ((argc == 4 || argc == 5 || argc == 6)
        && std::strcmp(argv[1], "--released-topology-child") == 0) {
        const bool awaitAuthorityRevocation = argc == 5
            && std::strcmp(argv[4], "--await-authority-revocation") == 0;
        const bool awaitAuthorityLivenessTimeout = argc == 5
            && std::strcmp(argv[4], "--await-authority-liveness-timeout") == 0;
        const bool authorityStateValidation = argc == 5
            && std::strcmp(argv[4], "--authority-state-validation") == 0;
        const bool relayDataBindingValidation = argc == 5
            && std::strcmp(argv[4], "--relay-data-binding-validation") == 0;
        uint32_t b10Case = 0u;
        const bool b10Child = argc == 6
            && std::strcmp(argv[4], "--b10-case") == 0
            && parseB10Case(argv[5], b10Case);
        if ((argc == 5 && !awaitAuthorityRevocation
             && !awaitAuthorityLivenessTimeout && !authorityStateValidation
             && !relayDataBindingValidation)
            || (argc == 6 && !b10Child)) {
            std::fprintf(stderr, "unknown released-topology child option: %s\n", argv[4]);
            return 2;
        }
        return runChild(argv[2], argv[3], awaitAuthorityRevocation,
                        awaitAuthorityLivenessTimeout,
                        authorityStateValidation, b10Case,
                        relayDataBindingValidation);
    }
    if (argc == 3 && std::strcmp(argv[1], "--b10-case") == 0) {
        uint32_t b10Case = 0u;
        if (!parseB10Case(argv[2], b10Case)) {
            std::fprintf(stderr, "unknown B10 case: %s\n", argv[2]);
            return 2;
        }
        return runParent(false, false, b10Case) ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--authority-state-validation") == 0)
        return runParent(false, false, 0u, true) ? 0 : 1;
    if (argc == 2
        && std::strcmp(argv[1], "--relay-data-binding-validation") == 0) {
        return runParent(false, false, 0u, false, true) ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--phase-c-relay-process") == 0)
        return runPhaseCParent() ? 0 : 1;
    if (argc == 2
        && std::strcmp(argv[1], "--paired-activation-validation") == 0) {
        return testCompletedS2RetirementTupleExactness()
                && runPhaseCParent(true)
                && runPhaseCParent(true, true) ? 0 : 1;
    }
    if (argc == 2
        && std::strcmp(argv[1], "--completed-s2-retirement-validation") == 0) {
        return testCompletedS2RetirementTupleExactness()
                && runPhaseCParent(true, true) ? 0 : 1;
    }
    if (argc == 2
        && std::strcmp(argv[1], "--client-root-retirement-validation") == 0) {
        return testCompletedClientRootRetirementTupleExactness() ? 0 : 1;
    }
    if (argc == 2
        && std::strcmp(argv[1], "--client-root-qualification") == 0) {
        return runClientRootQualification() ? 0 : 1;
    }
    if (argc != 1) {
        std::fprintf(stderr,
                     "usage: %s [--released-topology-child <qsp-port> <control-port> "
                     "[--await-authority-revocation|--await-authority-liveness-timeout|"
                     "--authority-state-validation|"
                     "--relay-data-binding-validation|"
                     "--b10-case <1..45>]] [--b10-case <1..45>] "
                     "[--phase-c-relay-process|--paired-activation-validation|"
                     "--completed-s2-retirement-validation|"
                     "--client-root-retirement-validation]\n",
                     argc > 0 ? argv[0]
                              : "qoh99_resilient_topology_client_process");
        return 2;
    }
    if (!testCompletedClientRootRetirementTupleExactness()
        || !runParent(false, false) || !runParent(true, false)
        || !runParent(false, true) || !runParent(false, false, 0u, true)) {
        return 1;
    }
    std::puts("released topology client production UDP/process seams: PASS");
    return 0;
}
