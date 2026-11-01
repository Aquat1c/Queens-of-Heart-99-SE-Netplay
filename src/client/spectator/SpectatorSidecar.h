#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "client/spectator/ResilientSpectatorRuntime.h"
#include "client/spectator/ResilientTopologyAuthority.h"
#include "client/spectator/SpectatorRoute.h"
#include "net/Protocol.h"
#include "net/Sockets.h"
#include "spectator/SpectatorArchive.h"
#include "spectator/SpectatorConnectedArchiveV4.h"
#include "spectator/SpectatorConnectedSessionV4.h"
#include "spectator/SpectatorProtocol.h"
#include "spectator/SpectatorSession.h"

namespace qipc {
struct IpcConfig;
struct SpectatorClientPublisherRuntimeAppliedReceipt;
}

namespace qclient::spectator {

constexpr uint32_t kPublicationMagic = 0x53504351u;
constexpr uint16_t kPublicationVersion = 2;
constexpr size_t kPublicationHeaderSize = 36;
constexpr size_t kMaxPublicationFrameBytes = 2u * 1024u * 1024u;
constexpr size_t kPublicationDrainBudget = 1024;

enum class PublicationWireKind : uint16_t {
    Hello = 1,
    Start = 2,
    Confirmed = 3,
    Checkpoint = 4,
    Seal = 5,
    TailProof = 6,
    PublicationFault = 7,
    ConnectedDescriptorV4 = 8,
    ConnectedPublicationV4 = 9,
    ChainedSetEvent = 10,
};

enum class PublicationKind : uint8_t {
    Start = 1,
    Record = 2,
    Checkpoint = 3,
    Seal = 4,
    Pause = 5,
    Resume = 6,
    LossReport = 7,
    TailProof = 8,
    ConnectedDescriptorV4 = 9,
    ConnectedPublicationV4 = 10,
    ChainedSetEvent = 11,
};

struct PublicationEvent {
    PublicationKind kind = PublicationKind::Record;
    uint64_t sequence = 0;

    qnetplay::spectator::GameDescriptor descriptor{};

    qnetplay::spectator::ConfirmedRecord record{};

    qnetplay::spectator::SetEvent setEvent{};

    uint64_t checkpointId = 0;
    uint32_t checkpointRecord = 0;
    uint64_t checkpointDigest = 0;
    std::vector<uint8_t> checkpointBytes;

    qnetplay::spectator::TerminalReason reason =
        qnetplay::spectator::TerminalReason::MatchEnded;
    uint64_t postTailStateHash = 0;
    bool hasPostTailProof = false;
    uint32_t terminalRecordCursor = 0;
    uint64_t terminalPrefixDigest = 0;
    bool hasTerminalPrefixProof = false;
    uint16_t publicationFaultReason = 0;
    uint32_t tailRecordCursor = 0;

    qnetplay::spectator::ConnectedSetDescriptorV4 connectedDescriptor{};
    qnetplay::spectator::ConnectedPublicationV4 connectedPublication{};
};

class PublicationChannel {
public:
    virtual ~PublicationChannel() = default;
    virtual int drain(std::vector<PublicationEvent>& out, size_t maxEvents) = 0;
    virtual bool connected() const = 0;
    virtual bool reconnecting() const { return false; }
    virtual void close() = 0;
};

std::unique_ptr<PublicationChannel> makePipePublicationChannel(uint32_t gamePid,
                                                               std::string& error);

bool decodePublicationFrame(const uint8_t* frame,
                            size_t length,
                            std::vector<PublicationEvent>& out);

struct SourceAutopunchConfig {
    bool enabled = false;
    RouteEndpoint relay{};
};

struct SourceRelayConfig {
    bool enabled = false;
    RouteEndpoint relay{};
    qnet::relay::RoomId room{};
    qnet::relay::RegistrationToken sourceToken{};
    qnetplay::spectator::SessionIdentity identity{};
};

enum class SidecarTransportEventKind : uint8_t {
    ViewerDetached = 1,
    RelaySourceRetrying = 2,
    RelaySourceUnavailable = 3,
    RelaySourceReady = 4,
};

struct SidecarTransportEvent {
    SidecarTransportEventKind kind =
        SidecarTransportEventKind::ViewerDetached;
    RouteEndpoint endpoint{};
    uint64_t viewerId = 0;
};

class SidecarTransport {
public:
    virtual ~SidecarTransport() = default;
    virtual bool open(uint16_t port, std::string& error) = 0;
    virtual uint16_t boundPort() const = 0;
    virtual void close() = 0;
    virtual int recvFrom(uint8_t* buffer, size_t capacity, RouteEndpoint& from) = 0;
    virtual bool sendTo(const RouteEndpoint& to, const uint8_t* bytes, size_t length) = 0;
    virtual bool pollEvent(SidecarTransportEvent& event) {
        (void)event;
        return false;
    }

    virtual bool traversalSummary(std::string& out) const {
        out.clear();
        return false;
    }

    virtual bool configureSourceAutopunch(const SourceAutopunchConfig& config,
                                          std::string& warning) {
        if (!config.enabled) {
            warning.clear();
            return true;
        }
        warning = "this spectator transport does not support source AutoPunch";
        return false;
    }
    virtual bool configureSourceRelay(const SourceRelayConfig& config,
                                      std::string& warning) {
        if (!config.enabled) {
            warning.clear();
            return true;
        }
        warning =
            "this spectator transport does not support source relay service";
        return false;
    }
    virtual uint32_t relaySourceGeneration() const { return 0; }
    virtual bool relayViewerIdentity(const RouteEndpoint& endpoint,
                                     uint64_t& viewerId) const {
        (void)endpoint;
        viewerId = 0;
        return false;
    }
    virtual bool signRelayGrant(
        qnetplay::spectator::SpectatorGrant& grant) const {
        (void)grant;
        return false;
    }
    virtual bool verifyRelayGrant(
        const qnetplay::spectator::SpectatorGrant& grant,
        uint64_t nowUnix) const {
        (void)grant;
        (void)nowUnix;
        return false;
    }

    virtual void serviceTraversal(uint64_t nowMs) { (void)nowMs; }

    virtual bool noteAdmittedViewer(uint32_t peerIpv4NetworkOrder,
                                    uint16_t controlPort, uint16_t dataPort,
                                    uint64_t nowMs) {
        (void)peerIpv4NetworkOrder; (void)controlPort;
        (void)dataPort; (void)nowMs;
        return false;
    }
};

// `preboundSocket` is an owned SOCKET adopted exactly once; all bits one means bind normally.
inline constexpr uintptr_t kInvalidPreboundSpectatorSocket =
    ~static_cast<uintptr_t>(0);
std::unique_ptr<SidecarTransport> makeUdpSidecarTransport(
    uintptr_t preboundSocket = kInvalidPreboundSpectatorSocket);

enum class AdmissionPolicy : uint8_t {
    Public = 1,
    AskHost = 2,
    CodeOnly = 3,
    Disabled = 4,
};

enum class AdmissionDecision : uint8_t {
    Admit = 1,
    Pending = 2,
    Deny = 3,
};

enum class AdmissionDenyReason : uint8_t {
    None = 0,
    PolicyDisabled,
    HostDeclined,
    GrantRequired,
    GrantInvalid,
    GrantExpired,
    GrantRevoked,
    CapacityFull,
    PerIpQuota,
    PerGrantQuota,
    RateLimited,
    UploadBudgetExhausted,
    ServiceFailed,
    ServiceExpired,
    Kicked,
};

const char* admissionDenyText(AdmissionDenyReason reason);

struct GrantRequest {
    uint64_t viewerId = 0;
    uint32_t resumeRecord = 0;
    uint64_t resumeDigest = 0;
    uint8_t route = 0;
    uint32_t lifetimeSeconds = 300;
    uint32_t peerIpv4BE = 0;
};

// Values 1..9 must stay identical to qnetplay::spectator::SourcePhase.
enum class SidecarPhase : uint8_t {
    Idle = 0,
    Waiting = 1,
    Active = 2,
    Paused = 3,
    Checkpointing = 4,
    Sealed = 5,
    Draining = 6,
    Retained = 7,
    Expired = 8,
    Failed = 9,
};

const char* sidecarPhaseName(SidecarPhase phase);
qnetplay::spectator::SourcePhase toWireSourcePhase(SidecarPhase phase);

enum class SourceStatusCode : uint16_t {
    Disabled = 0,
    WaitingForStart = 1,
    Streaming = 2,
    PausedHeld = 3,
    Checkpointing = 4,
    SealedDraining = 5,
    CompletedClean = 6,
    PeerQuitSourceStopped = 7,
    ProtocolOrArchiveFailure = 8,
    CapacityRejection = 9,
    RouteLostResuming = 10,
    TerminalProofMismatch = 11,
    RetainedPostGame = 12,
    RetentionExpired = 13,
};

const char* sourceStatusText(SourceStatusCode code);

enum class ViewerServiceState : uint8_t {
    PendingApproval = 1,
    Handshaking = 2,
    Streaming = 3,
    AwaitingTerminalAck = 4,
    Completed = 5,
    DeadlineExpired = 6,
    Detached = 7,
    Kicked = 8,
};

const char* viewerServiceStateName(ViewerServiceState state);

struct ViewerStatus {
    uint64_t viewerId = 0;
    uint32_t generation = 0;
    RouteEndpoint endpoint{};
    uint16_t routeLabel = 0;
    ViewerServiceState state = ViewerServiceState::Handshaking;
    uint32_t bootstrapRecord = 0;
    uint32_t acknowledgedRecord = 0;
    uint32_t backlogRecords = 0;
    uint64_t admittedAtMs = 0;
    uint64_t lastProgressMs = 0;
    uint64_t bytesSent = 0;
    std::string grantTag;
};

struct ViewerTerminalOutcome {
    uint64_t viewerId = 0;
    uint32_t generation = 0;
    ViewerServiceState state = ViewerServiceState::Completed;
    uint32_t acknowledgedRecord = 0;
};

struct TopologyViewerSelection {
    uint64_t viewerId = 0;
    uint32_t viewerGeneration = 0;
};

struct SidecarStatus {
    SidecarPhase phase = SidecarPhase::Idle;
    SourceStatusCode code = SourceStatusCode::Disabled;
    const char* codeText = "";
    bool serving = false;
    uint16_t boundPort = 0;
    AdmissionPolicy policy = AdmissionPolicy::Public;
    bool clientPrivateRootDormant = false;
    bool clientPrivateRuntimeApplied = false;
    bool clientRootPrepared = false;
    bool clientRootAsserted = false;
    bool clientRootCommitted = false;
    bool clientRootServing = false;
    uint16_t clientRootQsrPort = 0;

    uint64_t gameId = 0;
    bool hasGame = false;
    uint32_t confirmedRecords = 0;
    uint32_t firstAvailableRecord = 0;
    size_t retainedRecordCount = 0;
    size_t allocatedRecordBytes = 0;
    uint32_t newestCheckpointRecord = 0;
    size_t checkpointCount = 0;

    bool presenceHasCurrentGame = false;
    bool presenceCurrentGameEnded = false;
    uint32_t presenceTimelineFrame = 0u;
    uint8_t presenceActiveFighterMask = 0u;
    std::array<uint8_t, 4> presenceCharacterIds{
        {0xFFu, 0xFFu, 0xFFu, 0xFFu}};
    std::array<uint8_t, 4> presencePaletteIndices{};
    std::array<uint8_t, 2> presenceRoundWins{};
    std::array<uint16_t, 2> presenceMatchWins{};
    std::array<std::string, 2> presencePlayerNames{};

    bool connectedV4Bound = false;
    bool connectedV4Failed = false;
    bool connectedV4Sealed = false;
    uint64_t connectedSetStreamId = 0u;
    uint32_t connectedNextItemSequence = 0u;
    uint32_t connectedNextRecordCursor = 0u;
    size_t connectedCompleteGames = 0u;
    uint64_t connectedRetainedBytes = 0u;

    bool sealed = false;
    bool sealedIsTruncation = false;
    qnetplay::spectator::TerminalReason terminalReason =
        qnetplay::spectator::TerminalReason::MatchEnded;
    uint64_t postTailStateHash = 0;
    bool hasPostTailProof = false;
    uint32_t terminalRecordCursor = 0;
    uint64_t terminalPrefixDigest = 0;
    uint64_t conflictingTerminalRequests = 0;
    uint32_t truncatedRecords = 0;

    size_t viewerCount = 0;
    size_t viewerCapacity = 0;
    size_t pendingApprovals = 0;
    size_t reservationCount = 0;
    size_t capacityReservations = 0;
    size_t consumedGrantTombstones = 0;
    size_t viewersCompleted = 0;
    size_t viewersDeadlineExpired = 0;
    size_t viewersKicked = 0;
    size_t viewersDetached = 0;
    size_t admissionRejections = 0;

    bool topologyAuthorityReady = false;
    uint64_t topologyAuthorityGeneration = 0;
    uint64_t topologyRootNodeId = 0;
    uint16_t topologyQsrPort = 0;
    size_t topologyEnrolled = 0;
    size_t topologyAttached = 0;

    uint64_t retentionRemainingMs = 0;
    uint64_t drainDeadlineRemainingMs = 0;

    std::string failureReason;
    bool playersUnaffected = true;

    uint64_t ingressPackets = 0;
    uint64_t ingressBytes = 0;
    uint64_t ingressRateLimited = 0;
    uint64_t ingressDecoded = 0;
    uint64_t ingressRejected = 0;
    size_t trackedAddressCount = 0;
    uint64_t overflowAddressPackets = 0;
    uint64_t egressPackets = 0;
    uint64_t egressBytes = 0;
    uint64_t uploadThrottledPackets = 0;
    uint64_t publicationEvents = 0;
    uint64_t publicationGaps = 0;
    uint64_t tailProofs = 0;
    uint64_t tailProofCommits = 0;
    uint64_t spoolFailures = 0;
    uint64_t diagnosticsEmitted = 0;
    uint64_t diagnosticsDropped = 0;
    uint64_t diagnosticBytes = 0;

    std::vector<ViewerStatus> viewers;
    std::vector<ViewerTerminalOutcome> terminalOutcomes;
};

enum class DiagEvent : uint16_t {
    MalformedDatagram = 0,
    RateLimited,
    UnknownViewer,
    AdmissionDenied,
    CapacityRejected,
    SpoolFailure,
    PublicationGap,
    ViewerTimeout,
    TerminalDeadline,
    RouteLost,
    MirrorDivergence,
    TransportError,
    Count,
};

const char* diagEventName(DiagEvent event);

struct DiagnosticsConfig {
    uint64_t windowMs = 5000;
    size_t maxEmitsPerWindow = 8;
    uint64_t maxTotalBytes = 256u * 1024u;
    std::function<void(const char*)> sink;
};

class CoalescedDiagnostics {
public:
    explicit CoalescedDiagnostics(const DiagnosticsConfig& config = {});

    bool note(DiagEvent event, uint64_t nowMs, const char* detail);

    uint64_t emitted() const { return _emitted; }
    uint64_t dropped() const { return _dropped; }
    uint64_t bytes() const { return _bytes; }
    uint64_t droppedFor(DiagEvent event) const;

private:
    struct Slot {
        uint64_t windowStartMs = 0;
        uint64_t suppressed = 0;
        uint64_t suppressedTotal = 0;
        bool windowStarted = false;
        bool emittedThisWindow = false;
    };

    DiagnosticsConfig _config{};
    Slot _slots[static_cast<size_t>(DiagEvent::Count)]{};
    uint64_t _globalWindowStartMs = 0;
    bool _globalWindowStarted = false;
    size_t _emitsThisWindow = 0;
    uint64_t _emitted = 0;
    uint64_t _dropped = 0;
    uint64_t _bytes = 0;
};

struct RateLimitConfig {
    uint32_t globalPacketsPerSecond = 2400;
    uint32_t globalBurstPackets = 128;
    uint64_t globalBytesPerSecond = 2u * 1024u * 1024u;
    uint64_t globalBurstBytes = 128u * 1024u;

    uint32_t perIpPacketsPerSecond = 240;
    uint32_t perIpBurstPackets = 32;
    uint64_t perIpBytesPerSecond = 256u * 1024u;
    uint64_t perIpBurstBytes = 32u * 1024u;
    size_t maxViewersPerIp = 4;
    uint32_t perIpViolationsBeforeCooldown = 64;
    uint64_t perIpCooldownMs = 10000;
    size_t maxTrackedAddresses = 512;

    uint32_t perGrantPacketsPerSecond = 60;
    uint32_t perGrantBurstPackets = 16;
};

enum class IngressOutcome : uint8_t {
    Accepted = 1,
    ServiceUnavailable = 2,
    RateLimitedGlobal = 3,
    RateLimitedAddress = 4,
    RateLimitedGrant = 5,
    AddressCooldown = 6,
    Malformed = 7,
    AdmissionDenied = 8,
    ProtocolRejected = 9,
};

const char* ingressOutcomeName(IngressOutcome outcome);

struct MirrorPrefix {
    uint64_t gameId = 0;
    uint64_t startFingerprint = 0;
    uint32_t nextRecord = 0;
    uint64_t digest = 0;
    uint64_t checkpointId = 0;
    uint32_t checkpointRecord = 0;
    uint64_t checkpointDigest = 0;
};

enum class MirrorResult : uint8_t {
    Accepted = 1,
    NoLocalArchive = 2,
    GameMismatch = 3,
    FingerprintMismatch = 4,
    DigestDivergence = 5,
    CursorAhead = 6,
    ServiceFailed = 7,
};

const char* mirrorResultText(MirrorResult result);

struct SidecarConfig {
    qnetplay::spectator::SessionIdentity identity{};
    uint64_t simulationContract = 0;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity{};

    uint16_t listenPort = 0;

    uintptr_t preboundSocket = kInvalidPreboundSpectatorSocket;

    struct ClientPrivateRootConfig {
        bool enabled = false;
        uint32_t publisherGeneration = 0;
        uint64_t publisherNonce = 0;
    } clientPrivateRoot{};

    SourceAutopunchConfig sourceAutopunch{};
    SourceRelayConfig sourceRelay{};

    resilient::ResilientRootControlConfig resilientRoot{};

    topology::TopologyActivationRequest topologyActivation{};

    AdmissionPolicy policy = AdmissionPolicy::Public;
    size_t maxViewers = 32;
    size_t maxReservations = 256;
    uint64_t uploadBudgetBytesPerSecond = 3u * 1024u * 1024u;
    uint64_t uploadBurstBytes = 512u * 1024u;

    uint32_t maxRetainedRecords = 720000;
    size_t maxRetainedBytes = 32u * 1024u * 1024u;
    bool retainAllRecords = false;
    size_t maxCheckpoints = 4;
    size_t maxCheckpointBytes = 8u * 1024u * 1024u;

    uint64_t drainDeadlineMs = 8000;
    uint64_t retentionMs = 120000;

    uint64_t handshakeDeadlineMs = 10000;
    uint64_t noProgressDeadlineMs = 20000;
    uint64_t terminalAckDeadlineMs = 15000;
    uint64_t heartbeatIntervalMs = 1000;
    uint32_t maxViewerUnackedRecords = 8192;
    size_t maxViewerUnackedBytes = 512u * 1024u;

    size_t controlPacketBudget = 8;
    size_t dataPacketBudget = 24;
    size_t ingressDatagramBudget = 128;

    RateLimitConfig rateLimits{};
    DiagnosticsConfig diagnostics{};

    std::function<void(qnetplay::spectator::SpectatorGrant&)> signGrant;
    std::function<bool(const qnetplay::spectator::SpectatorGrant&,
                       uint64_t nowUnix)>
        verifyGrant;
    std::function<uint64_t()> unixClock;
    std::function<uint64_t()> randomU64;
};

class ClientPrivateRuntimeAppliedReceiver {
public:
    ClientPrivateRuntimeAppliedReceiver();
    ~ClientPrivateRuntimeAppliedReceiver();

    ClientPrivateRuntimeAppliedReceiver(
        const ClientPrivateRuntimeAppliedReceiver&) = delete;
    ClientPrivateRuntimeAppliedReceiver& operator=(
        const ClientPrivateRuntimeAppliedReceiver&) = delete;

    bool open(uint32_t gamePid, uint32_t publisherGeneration,
              std::string& error);
    bool receive(uint32_t timeoutMs,
                 qipc::SpectatorClientPublisherRuntimeAppliedReceipt& out,
                 std::string& error);
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

struct ClientPrivateRootStreamFacts {
    bool available = false;
    uint16_t schemaVersion = 1u;
    uint64_t sessionId = 0u;
    uint64_t sessionEpoch = 0u;
    qnetplay::compat::CompatibilityIdentityWire compatibility{};
    uint64_t streamRootIdentity = 0u;
    uint64_t gameStartFingerprint = 0u;
    uint64_t collectorGeneration = 0u;
    uint64_t publisherNonce = 0u;
    uint8_t playerSlot = 0u;
    uint32_t gameProcessId = 0u;
    uint8_t appliedDelay = 0u;
    uint8_t appliedRollback = 0u;
    uint8_t appliedRemoteDelay = 0u;
    uint8_t appliedRemoteRollback = 0u;
    qnetplay::config_provenance::ConfigurationProvenanceWire provenance{};
    uint32_t cacheFirstCursor = 0u;
    uint32_t cacheNextCursor = 0u;
    uint32_t nextEvent = 0u;
    uint64_t chainDigest = 0u;
    uint16_t collectorQspPort = 0u;
    uint16_t clientRootQsrPort = 0u;
};

struct ClientPlayerRootArmFacts {
    uint64_t clientRootNodeId = 0u;
    uint64_t hostRootNodeId = 0u;
    uint64_t authorityGeneration = 0u;
    uint64_t expiresMs = 0u;
    qnet::SpectateTopologyEndpointBindingWire rootQsp{};
    qnet::SpectateTopologyEndpointBindingWire rootQsr{};
    uint64_t hostSourceGeneration = 0u;
    std::array<uint8_t, qnet::kSpectateTopologyAuthorityPublicKeyBytes>
        authorityPublicKey{};
    std::array<uint8_t, qnet::kClientRootCredentialBytes> credential{};
    std::vector<uint8_t> grantBlob;
};

class SpectatorSidecar {
public:
    explicit SpectatorSidecar(const SidecarConfig& config);
    ~SpectatorSidecar();

    SpectatorSidecar(const SpectatorSidecar&) = delete;
    SpectatorSidecar& operator=(const SpectatorSidecar&) = delete;

    bool start(std::unique_ptr<SidecarTransport> transport,
               std::unique_ptr<PublicationChannel> channel,
               uint64_t nowMs,
               std::string& error);

    bool configureTopologyAuthority(
        const topology::TopologyActivationRequest& request,
        uint64_t nowMs, std::string& error);

    bool confirmClientPrivateRuntimeApplied(
        const qipc::IpcConfig& launchConfig,
        const qipc::SpectatorClientPublisherRuntimeAppliedReceipt& receipt,
        uint32_t expectedProcessId, std::string& error);
    bool clientPrivateRootStreamFacts(ClientPrivateRootStreamFacts& out) const;
    bool prepareClientPlayerRootQsr(uint16_t& outPort, std::string& error);
    bool armClientPlayerRoot(const ClientPlayerRootArmFacts& facts,
                             uint64_t nowMs, std::string& error);
    void withdrawClientPlayerRoot(const char* reason);

    bool enrollTopologyViewer(uint64_t viewerId, uint32_t viewerGeneration,
                               bool relayCapable, uint64_t nowMs,
                               topology::TopologyEnrollment& out,
                               std::string& error);
    bool enrollTopologyViewerExact(
        const topology::TopologyViewerClaim& claim,
        const qnetplay::spectator::SpectatorGrant& grant,
        uint64_t nowMs, topology::TopologyEnrollment& out,
        std::string& error);
    bool relayTopologyLaneAvailable() const;
    bool enrollClientPlayerRoot(
        const topology::ClientPlayerRootClaim& claim, uint64_t nowMs,
        topology::ClientPlayerRootEnrollment& out,
        std::vector<uint8_t>& encodedGrant, std::string& error);
    bool issueTopologyRelayChildAssignment(
        qnetplay::spectator::resilient::NodeId relayNodeId,
        qnetplay::spectator::resilient::NodeId childNodeId,
        uint64_t nowMs, std::string& error);
    bool issueTopologyRelayChildActivation(
        qnetplay::spectator::resilient::NodeId relayNodeId,
        qnetplay::spectator::resilient::NodeId childNodeId,
        uint64_t nowMs, std::string& error);
    bool issueTopologyRelayChildActivationForViewers(
        const TopologyViewerSelection& relay,
        const TopologyViewerSelection& child,
        uint64_t nowMs, std::string& error);
    bool issueClientPlayerRootActivationForViewer(
        const TopologyViewerSelection& target, uint64_t nowMs,
        std::string& error);
    void rollbackTopologyViewer(uint64_t viewerId);
    bool verifyTopologyViewerGrant(
        const qnetplay::spectator::SpectatorGrant& grant,
        uint32_t viewerGeneration, uint64_t nowMs) const;

    void service(uint64_t nowMs);

    void requestSeal(qnetplay::spectator::TerminalReason reason,
                     uint32_t authoritativeRecordCursor,
                     uint64_t authoritativePrefixDigest,
                     uint64_t postTailStateHash,
                     bool hasPrefixProof,
                     bool hasPostTailProof,
                     uint64_t nowMs);

    void requestSeal(qnetplay::spectator::TerminalReason reason,
                     uint64_t postTailStateHash,
                     bool hasProof,
                     uint64_t nowMs);

    bool stop(bool drain, uint64_t nowMs);
    bool stopped() const { return _stopped; }

    uint64_t drainDeadlineMs() const { return _config.drainDeadlineMs; }

    using TerminalOutcome = ViewerTerminalOutcome;
    const std::vector<TerminalOutcome>& terminalOutcomes() const {
        return _terminalOutcomes;
    }

    void publish(const PublicationEvent& event, uint64_t nowMs);

    IngressOutcome onDatagram(const RouteEndpoint& from,
                              const uint8_t* bytes,
                              size_t length,
                              uint64_t nowMs);

    void setAdmissionPolicy(AdmissionPolicy policy);
    AdmissionPolicy admissionPolicy() const { return _policy; }
    void setMaxViewers(size_t maxViewers);
    void setUploadBudgetBytesPerSecond(uint64_t bytesPerSecond);

    bool issueGrant(const GrantRequest& request,
                    qnetplay::spectator::SpectatorGrant& out,
                    AdmissionDenyReason& denyReason,
                    uint64_t nowMs);

    std::vector<uint64_t> pendingApprovals() const;
    bool approveViewer(uint64_t viewerId, uint64_t nowMs);
    bool declineViewer(uint64_t viewerId, uint64_t nowMs);

    bool kickViewer(uint64_t viewerId, uint64_t nowMs);
    bool revokeGrant(uint64_t grantId, uint64_t nowMs);
    bool releaseReservation(uint64_t viewerId, uint64_t nowMs = 0);

    void armReattach(uint64_t viewerId, uint64_t nowMs);

    uint32_t archivedSetEventCount() const;

    bool resumePointIsOnOurChain(uint32_t resumeRecord, uint32_t resumeEvent,
                                 uint64_t resumeDigest) const;

    bool noteAdmittedViewer(uint32_t peerIpv4NetworkOrder, uint16_t controlPort,
                            uint16_t dataPort, uint64_t nowMs = 0);

    MirrorResult verifyPeerPrefix(const MirrorPrefix& prefix) const;
    MirrorResult acceptPeerPrefix(const MirrorPrefix& prefix, uint64_t nowMs);
    bool isAlternateSource() const { return _alternateSource; }
    MirrorPrefix localPrefix() const;

    SidecarStatus status() const;
    SidecarPhase phase() const { return _phase; }
    const qnetplay::spectator::SpectatorArchive* archiveForTest() const {
        return _archive.get();
    }
    uint16_t boundPort() const;

    void injectSpoolFailure(const char* reason, uint64_t nowMs);

private:
    struct Bucket {
        double tokens = 0.0;
        double capacity = 0.0;
        double refillPerMs = 0.0;
        uint64_t lastMs = 0;
        void configure(double perSecond, double burst, uint64_t nowMs);
        bool take(double cost, uint64_t nowMs);
    };

    struct AddressState {
        Bucket packets{};
        Bucket bytes{};
        uint32_t violations = 0;
        uint64_t cooldownUntilMs = 0;
        size_t viewers = 0;
        uint64_t lastSeenMs = 0;
    };

    struct QuotaAddress {
        bool relay = false;
        uint64_t value = 0;

        bool operator==(const QuotaAddress& other) const {
            return relay == other.relay && value == other.value;
        }
        bool operator<(const QuotaAddress& other) const {
            return relay != other.relay ? relay < other.relay
                                        : value < other.value;
        }
    };

    struct Reservation {
        qnetplay::spectator::SpectatorGrant grant{};
        uint32_t peerIpv4BE = 0;
        uint64_t issuedAtMs = 0;
        uint64_t expiresAtMs = 0;
        uint64_t capacityHeldUntilMs = 0;
        bool approved = true;
        bool declined = false;
        bool revoked = false;
        bool bound = false;
        bool consumed = false;
        uint64_t reattachUntilMs = 0;
    };

    struct ViewerRecord {
        uint64_t viewerId = 0;
        uint64_t endpointToken = 0;
        uint32_t generation = 0;
        RouteEndpoint endpoint{};
        QuotaAddress ingressQuotaAddress{};
        QuotaAddress capacityQuotaAddress{};
        uint16_t routeLabel = 0;
        ViewerServiceState state = ViewerServiceState::Handshaking;
        uint32_t bootstrapRecord = 0;
        uint64_t admittedAtMs = 0;
        uint64_t firstAdmittedAtMs = 0;
        uint64_t lastProgressMs = 0;
        uint64_t lastHeartbeatMs = 0;
        uint32_t lastAckedRecord = 0;
        uint64_t bytesSent = 0;
        uint64_t grantId = 0;
        Bucket grantPackets{};
        bool terminalPhaseEntered = false;
        uint64_t terminalPhaseAtMs = 0;
    };

    void setPhase(SidecarPhase phase, uint64_t nowMs);
    void failService(const char* reason, uint64_t nowMs);
    void logViewerSummary(uint64_t nowMs);
    bool helloLogDue(uint64_t viewerId, uint64_t nowMs);
    void applyStart(const PublicationEvent& event, uint64_t nowMs);
    void applyRecord(const PublicationEvent& event, uint64_t nowMs);
    void applyCheckpoint(const PublicationEvent& event, uint64_t nowMs);
    void applyTailProof(const PublicationEvent& event, uint64_t nowMs);
    void applyPublicationFault(const PublicationEvent& event, uint64_t nowMs);
    void applyConnectedDescriptorV4(
        const PublicationEvent& event, uint64_t nowMs);
    void applyConnectedPublicationV4(
        const PublicationEvent& event, uint64_t nowMs);
    void refreshConnectedPresence(
        const qnetplay::spectator::ConnectedPublicationV4& publication);
    void startResilientRoot(uint64_t nowMs);
    void stopResilientRoot(
        uint64_t nowMs,
        topology::wire::AuthorityStateReason reason =
            topology::wire::AuthorityStateReason::ServiceStopping);
    void stopResilientRuntime(uint64_t nowMs,
                              topology::wire::AuthorityStateReason reason);
    bool makeTopologyViewerProof(uint64_t viewerId,
                                 uint32_t viewerGeneration,
                                 bool relayCapable,
                                 bool requireStreaming,
                                 topology::TopologyViewerProof& out,
                                 std::string& error) const;
    bool refreshTopologyViewerBinding(uint64_t viewerId,
                                      uint32_t viewerGeneration,
                                      uint64_t nowMs);
    bool beginTopologyViewerRebind(uint64_t viewerId,
                                   uint32_t viewerGeneration,
                                   uint64_t nowMs);
    void detachTopologyViewer(
        uint64_t viewerId, uint64_t nowMs,
        topology::wire::AuthorityStateReason reason =
            topology::wire::AuthorityStateReason::ViewerDetached,
        bool emitRevocation = true);
    void handleUpstreamLoss(const char* detail, uint64_t nowMs);
    void sealTruncated(const char* reason, uint64_t nowMs);
    bool flushPendingRecord(uint64_t postStateProof, uint64_t nowMs);
    bool exactTerminalPrefix(uint32_t recordCursor,
                             uint64_t prefixDigest) const;
    bool rememberTerminalRequest(
        qnetplay::spectator::TerminalReason reason,
        uint32_t recordCursor,
        uint64_t prefixDigest,
        uint64_t postTailStateHash,
        bool prefixProof,
        bool postTailProof,
        uint64_t nowMs);

    AdmissionDecision admit(
                            const RouteEndpoint& from,
                            const qnetplay::spectator::HelloPayload& hello,
                            uint64_t nowMs,
                            AdmissionDenyReason& denyReason);
    // `requestedGameId` MUST echo what the viewer asked for; the viewer discards a Reject that does not match.
    void sendReject(const RouteEndpoint& to,
                    uint64_t viewerId,
                    uint32_t generation,
                    qnetplay::spectator::RejectReason reason,
                    uint64_t nowMs,
                    uint64_t requestedGameId =
                        qnetplay::spectator::kLatestGameId);
    void sendHeartbeat(const RouteEndpoint& to, uint64_t nowMs);
    // Shares the source's single monotonic source->viewer sequence; a private counter here breaks viewer anti-replay.
    uint32_t nextOutboundSequence();
    bool transmit(const RouteEndpoint& to,
                  const std::vector<uint8_t>& bytes,
                  bool control,
                  uint64_t nowMs);

    void pumpOutbound(uint64_t nowMs);
    void handleTransportEvent(const SidecarTransportEvent& event,
                              uint64_t nowMs);
    void enforceDeadlines(uint64_t nowMs);
    void retireViewer(uint64_t viewerId,
                      ViewerServiceState finalState,
                      uint64_t nowMs);
    void removeProvisionalViewer(uint64_t viewerId);
    void retireRelayIngress(uint64_t nowMs);
    void sweepReservations(uint64_t nowMs);
    bool reservationExpired(const Reservation& reservation,
                            uint64_t nowMs,
                            uint64_t unixNow) const;
    bool reservationHoldsCapacity(const Reservation& reservation,
                                  uint64_t nowMs,
                                  uint64_t unixNow) const;
    bool reservationGrantValid(const Reservation& reservation,
                               uint64_t unixNow) const;
    bool consumeUnboundReservationFromDetach(
        const RouteEndpoint& from,
        const qnetplay::spectator::DetachPayload& detach,
        uint64_t nowMs);
    void publishResumeFloor();

    static QuotaAddress directQuotaAddress(uint32_t ipv4BE);
    QuotaAddress ingressQuotaAddress(const RouteEndpoint& endpoint) const;
    AddressState& addressState(const QuotaAddress& quotaAddress,
                               uint64_t nowMs);
    void pruneAddresses(uint64_t nowMs);

    static uint64_t endpointToken(const RouteEndpoint& endpoint);
    uint64_t mintU64();
    uint64_t nowUnix() const;

    SidecarConfig _config{};
    AdmissionPolicy _policy = AdmissionPolicy::Public;
    bool _clientPrivateRootDormant = false;
    bool _topologyRelayLaneReady = false;
    bool _clientPrivateRuntimeApplied = false;
    struct ClientPrivateRuntimeProjection {
        uint32_t processId = 0u;
        uint8_t playerSlot = 0u;
        uint8_t delay = 0u;
        uint8_t rollback = 0u;
        uint8_t remoteDelay = 0u;
        uint8_t remoteRollback = 0u;
        qnetplay::config_provenance::ConfigurationProvenanceWire
            provenance{};
    } _clientPrivateRuntimeReceipt{};
    struct ClientPlayerRootControl {
        bool prepared = false;
        bool armed = false;
        bool asserted = false;
        bool committed = false;
        qnet::UdpSock qsrSocket{};
        uint16_t qsrPort = 0u;
        topology::TopologyAuthorityFacts macFacts{};
        uint64_t grantNonce = 0u;
        qnetplay::spectator::resilient::wire::ClientPlayerRootBinding
            binding{};
        std::array<uint8_t, qnet::kClientRootCredentialBytes> credential{};
        uint64_t nextAssertSequence = 1u;
        uint64_t lastServerSequence = 0u;
        uint64_t nextAssertSendMs = 0u;
        uint64_t assertDeadlineMs = 0u;
        qnetplay::spectator::resilient::wire::ClientPlayerRootActivation
            activation{};
        bool activationRetained = false;
        bool servingActive = false;
    } _clientRoot{};
    void serviceClientPlayerRoot(uint64_t nowMs);
    void activateClientRootServing(uint64_t nowMs);
    void deactivateClientRootServing(uint64_t nowMs, const char* reason);
    size_t _maxViewers = 32;
    uint32_t _fallbackSequence = 1;

    std::unique_ptr<SidecarTransport> _transport;
    std::unique_ptr<PublicationChannel> _channel;
    std::unique_ptr<qnetplay::spectator::SpectatorArchive> _archive;
    std::unique_ptr<qnetplay::spectator::SpectatorSource> _source;
    std::unique_ptr<
        qnetplay::spectator::SpectatorConnectedArchiveV4>
        _connectedArchiveV4;
    std::unique_ptr<
        qnetplay::spectator::SpectatorConnectedSourceSessionV4>
        _connectedSourceV4;
    std::unique_ptr<resilient::ResilientControlTransport>
        _resilientControlTransport;
    std::unique_ptr<resilient::ResilientSpectatorRootRuntime>
        _resilientRoot;
    topology::TopologyActivationRequest _topologyActivation{};
    bool _topologyActivationConfigured = false;
    std::unique_ptr<topology::TopologyQsrControl> _topologyQsr;
    std::shared_ptr<topology::TopologyAuthority> _topologyAuthority;
    bool _connectedV4Degraded = false;
    std::string _connectedV4Failure;
    bool _presenceHasCurrentGame = false;
    bool _presenceCurrentGameEnded = false;
    uint32_t _presenceTimelineFrame = 0u;
    uint8_t _presenceActiveFighterMask = 0u;
    std::array<uint8_t, 4> _presenceCharacterIds{
        {0xFFu, 0xFFu, 0xFFu, 0xFFu}};
    std::array<uint8_t, 4> _presencePaletteIndices{};
    std::array<uint8_t, 2> _presenceRoundWins{};
    std::array<uint16_t, 2> _presenceMatchWins{};
    std::array<std::string, 2> _presencePlayerNames{};
    mutable CoalescedDiagnostics _diagnostics;

    SidecarPhase _phase = SidecarPhase::Idle;
    SourceStatusCode _statusCode = SourceStatusCode::Disabled;
    bool _started = false;
    bool _stopped = false;
    bool _serviceFailed = false;
    bool _alternateSource = false;
    std::string _failureReason;

    bool _hasGame = false;
    uint64_t _gameId = 0;
    uint64_t _startFingerprint = 0;
    qnetplay::spectator::GameDescriptor _descriptor{};
    bool _hasPending = false;
    qnetplay::spectator::ConfirmedRecord _pending{};
    uint32_t _committedRecords = 0;
    std::deque<qnetplay::spectator::SetEvent> _heldSetEvents;
    void applyChainedSetEvent(const PublicationEvent& event, uint64_t nowMs);
    void drainHeldSetEvents(uint64_t nowMs);
    bool _hasPendingCheckpointRef = false;
    qnetplay::spectator::ArchiveCheckpointRef _pendingCheckpointRef{};
    void retryPendingCheckpointRef();
    uint32_t _truncatedRecords = 0;
    uint64_t _expectedPublicationSequence = 1;
    bool _hasProvenPostTail = false;
    uint64_t _provenPostTailHash = 0;
    uint64_t _tailProofs = 0;
    uint64_t _tailProofCommits = 0;

    bool _sealed = false;
    bool _sealTruncated = false;
    bool _hasPostTailProof = false;
    uint64_t _postTailStateHash = 0;
    qnetplay::spectator::TerminalReason _terminalReason =
        qnetplay::spectator::TerminalReason::MatchEnded;
    bool _terminalRequested = false;
    qnetplay::spectator::TerminalReason _requestedTerminalReason =
        qnetplay::spectator::TerminalReason::MatchEnded;
    uint32_t _requestedTerminalRecordCursor = 0;
    uint64_t _requestedTerminalPrefixDigest = 0;
    uint64_t _requestedPostTailStateHash = 0;
    bool _requestedPrefixProof = false;
    bool _requestedPostTailProof = false;
    uint32_t _terminalRecordCursor = 0;
    uint64_t _terminalPrefixDigest = 0;
    bool _terminalPrefixProven = false;
    uint64_t _terminalConflicts = 0;

    struct CheckpointEntry {
        uint64_t checkpointId = 0;
        uint32_t recordCursor = 0;
        uint64_t digest = 0;
        std::vector<uint8_t> bytes;
    };
    std::deque<CheckpointEntry> _checkpoints;
    size_t _checkpointBytes = 0;

    std::map<uint64_t, ViewerRecord> _viewers;
    std::map<uint64_t, uint64_t> _tokenToViewerId;
    std::map<uint64_t, RouteEndpoint> _endpointsByToken;
    std::map<uint64_t, Reservation> _reservations;
    std::map<QuotaAddress, AddressState> _addresses;
    AddressState _overflowAddress{};

    Bucket _globalPackets{};
    Bucket _globalBytes{};
    Bucket _upload{};

    uint64_t _drainDeadlineMs = 0;
    uint64_t _retentionUntilMs = 0;
    uint64_t _lastServiceMs = 0;
    uint64_t _lastViewerSummaryMs = 0;
    std::map<uint64_t, uint64_t> _helloLogMs;

    uint64_t _ingressPackets = 0;
    uint64_t _ingressBytes = 0;
    uint64_t _ingressRateLimited = 0;
    uint64_t _ingressDecoded = 0;
    uint64_t _ingressRejected = 0;
    uint64_t _overflowAddressPackets = 0;
    uint64_t _egressPackets = 0;
    uint64_t _egressBytes = 0;
    uint64_t _uploadThrottled = 0;
    uint64_t _publicationEvents = 0;
    uint64_t _publicationGaps = 0;
    uint64_t _spoolFailures = 0;
    static constexpr size_t kMaxTerminalOutcomes = 64;
    std::vector<TerminalOutcome> _terminalOutcomes;
    size_t _viewersCompleted = 0;
    size_t _viewersExpired = 0;
    size_t _viewersKicked = 0;
    size_t _viewersDetached = 0;
    size_t _admissionRejections = 0;
    uint64_t _randomCounter = 0;
};

bool startSidecar(const SidecarConfig& config,
                  uint32_t gamePid,
                  std::string& error);
bool startClientPrivateSidecar(const SidecarConfig& config,
                               uint32_t gamePid,
                               std::string& error);

inline constexpr size_t kSidecarControlDatagramMaxBytes = 2048;
struct SidecarControlDatagram {
    std::array<uint8_t, kSidecarControlDatagramMaxBytes> bytes{};
    size_t length = 0;
    qnet::Endpoint from{};
};
void setSidecarControlIngressEnabled(bool enabled);
bool pollSidecarControlDatagram(SidecarControlDatagram& out);

SidecarStatus sidecarStatus();
bool sidecarConfirmClientPrivateRuntimeApplied(
    const qipc::IpcConfig& launchConfig,
    const qipc::SpectatorClientPublisherRuntimeAppliedReceipt& receipt,
    uint32_t expectedProcessId, std::string& error);

void sealSidecar(qnetplay::spectator::TerminalReason reason,
                 uint64_t postTailStateHash,
                 bool hasProof);

bool stopSidecar(bool drain);

bool retainSidecarAfterGame();

bool sidecarSetAdmissionPolicy(AdmissionPolicy policy);
bool sidecarKickViewer(uint64_t viewerId);
bool sidecarRevokeGrant(uint64_t grantId);
bool sidecarReleaseViewerReservation(uint64_t viewerId);
bool sidecarApproveViewer(uint64_t viewerId);
bool sidecarDeclineViewer(uint64_t viewerId);
bool sidecarNoteAdmittedViewer(uint32_t peerIpv4NetworkOrder,
                               uint16_t controlPort, uint16_t dataPort);

bool sidecarIssueGrant(const GrantRequest& request,
                       qnetplay::spectator::SpectatorGrant& out,
                       AdmissionDenyReason& denyReason);
bool configureSidecarTopologyAuthority(
    const topology::TopologyActivationRequest& request,
    std::string& error);
bool sidecarEnrollTopologyViewer(
    uint64_t viewerId, uint32_t viewerGeneration, bool relayCapable,
    uint64_t nowMs, topology::TopologyEnrollment& out, std::string& error);
bool sidecarEnrollTopologyViewerExact(
    const topology::TopologyViewerClaim& claim,
    const qnetplay::spectator::SpectatorGrant& grant, uint64_t nowMs,
    topology::TopologyEnrollment& out, std::string& error);
bool sidecarEnrollClientPlayerRoot(
    const topology::ClientPlayerRootClaim& claim, uint64_t nowMs,
    topology::ClientPlayerRootEnrollment& out,
    std::vector<uint8_t>& encodedGrant, std::string& error);
bool sidecarClientPrivateRootStreamFacts(ClientPrivateRootStreamFacts& out);
bool sidecarPrepareClientPlayerRootQsr(uint16_t& outPort, std::string& error);
bool sidecarArmClientPlayerRoot(const ClientPlayerRootArmFacts& facts,
                                std::string& error);
void sidecarWithdrawClientPlayerRoot(const char* reason);
bool sidecarIssueTopologyRelayChildAssignment(
    qnetplay::spectator::resilient::NodeId relayNodeId,
    qnetplay::spectator::resilient::NodeId childNodeId,
    std::string& error);
bool sidecarIssueTopologyRelayChildActivationForViewers(
    const TopologyViewerSelection& relay,
    const TopologyViewerSelection& child,
    std::string& error);
bool sidecarIssueClientPlayerRootActivationForViewer(
    const TopologyViewerSelection& target, std::string& error);
void sidecarDetachTopologyViewer(uint64_t viewerId);
bool sidecarVerifyTopologyViewerGrant(
    const qnetplay::spectator::SpectatorGrant& grant,
    uint32_t viewerGeneration, uint64_t nowMs);
bool sidecarRunning();

}
