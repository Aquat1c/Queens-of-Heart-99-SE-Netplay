#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include "../netplay/NetplayCore.h"
#include <cstddef>
#include <cstdint>
#include "../ipc/IpcConfig.h"
#include "../netplay/MatchConfig.h"
#include "../spectator/SpectatorSession.h"
#include "SpectatorPlaybackPolicy.h"

namespace qgame {

namespace transport_detail {

constexpr size_t kModernPlayerIngressBudget = 512;
constexpr size_t kModernSpectatorIngressBudget = 64;
static_assert(kModernPlayerIngressBudget != 0
              && kModernSpectatorIngressBudget != 0,
              "each socket domain needs a nonzero receive budget");

enum class ModernSocketDomain : uint8_t {
    Player,
    Spectator,
};

constexpr bool modernDatagramMatchesDomain(
    ModernSocketDomain domain, bool hasSpectatorMagic) {
    return domain == ModernSocketDomain::Spectator
        ? hasSpectatorMagic
        : !hasSpectatorMagic;
}

constexpr bool modernRecvErrorEndsTurn(int error) {
    return error == WSAEWOULDBLOCK || error == WSAECONNRESET;
}

// `currentParentPortBE` and `candidatePortBE` are in network byte order, matching sockaddr_in::sin_port.
struct SpectatorParentRouteTransitionAuthorization {
    uint32_t currentParentIpv4BE = 0u;
    uint16_t currentParentPortBE = 0u;
    uint16_t reservedCurrentParent = 0u;
    uint32_t candidateIpv4BE = 0u;
    uint16_t candidatePortBE = 0u;
    uint16_t reserved = 0u;
    uint64_t sourceGeneration = 0u;
    uint64_t routeGeneration = 0u;
    uint32_t nextRecord = 0u;
    uint32_t nextEvent = 0u;
    uint64_t digest = 0u;
    uint64_t localDeadlineMs = 0u;
    uint64_t activationId = 0u;
};

struct SpectatorParentRouteCursor {
    uint64_t sourceGeneration = 0u;
    uint32_t nextRecord = 0u;
    uint32_t nextEvent = 0u;
    uint64_t digest = 0u;
};

enum class SpectatorParentRouteIngress : uint8_t {
    Rejected,
    Active,
    Pending,
};

class SpectatorParentRouteTransitionGate {
public:
    bool reset(const sockaddr_in& active) {
        if (!validEndpoint(active)) return false;
        _active = active;
        _activeKnown = true;
        _pending = false;
        _pendingAuthorization = {};
        _highestArmedRouteGeneration = 0u;
        _lastActivationId = 0u;
        return true;
    }

    void clear() {
        _active = {};
        _activeKnown = false;
        _pending = false;
        _pendingAuthorization = {};
        _highestArmedRouteGeneration = 0u;
        _lastActivationId = 0u;
    }

    bool armValidated(
        const SpectatorParentRouteTransitionAuthorization& authorization,
        const SpectatorParentRouteCursor& current,
        uint64_t nowMs) {
        expire(nowMs);
        if (!_activeKnown || _pending
            || !validAuthorization(authorization)
            || !sameCursor(authorization, current)
            || authorization.localDeadlineMs <= nowMs
            || authorization.routeGeneration
                   <= _highestArmedRouteGeneration
            || authorization.activationId == _lastActivationId) {
            return false;
        }

        const sockaddr_in candidate = endpointFor(authorization);
        if (!sameEndpoint(currentParentFor(authorization), _active)
            || !validEndpoint(candidate)
            || sameEndpoint(candidate, _active))
            return false;

        _pendingAuthorization = authorization;
        _pending = true;
        _highestArmedRouteGeneration = authorization.routeGeneration;
        _lastActivationId = authorization.activationId;
        return true;
    }

    bool expire(uint64_t nowMs) {
        if (!_pending || nowMs < _pendingAuthorization.localDeadlineMs)
            return false;
        _pending = false;
        _pendingAuthorization = {};
        return true;
    }

    void cancelPending() {
        _pending = false;
        _pendingAuthorization = {};
    }

    bool cancelPendingActivation(uint64_t activationId) {
        if (!_pending || activationId == 0u
            || _pendingAuthorization.activationId != activationId) {
            return false;
        }
        cancelPending();
        return true;
    }

    SpectatorParentRouteIngress classify(const sockaddr_in& source,
                                         uint64_t nowMs) {
        (void)expire(nowMs);
        if (_activeKnown && sameEndpoint(source, _active))
            return SpectatorParentRouteIngress::Active;
        if (_pending && sameEndpoint(source,
                                     endpointFor(_pendingAuthorization))) {
            return SpectatorParentRouteIngress::Pending;
        }
        return SpectatorParentRouteIngress::Rejected;
    }

    bool preparePendingAfterAcceptedIngress(const sockaddr_in& source,
                                            uint64_t nowMs) {
        (void)expire(nowMs);
        if (!_pending
            || !sameEndpoint(source, endpointFor(_pendingAuthorization))) {
            return false;
        }
        return true;
    }

    void commitPreparedPendingIngress(const sockaddr_in& source) {
        if (!_pending
            || !sameEndpoint(source, endpointFor(_pendingAuthorization))) {
            return;
        }
        _active = endpointFor(_pendingAuthorization);
        _activeKnown = true;
        _pending = false;
        _pendingAuthorization = {};
    }

    bool commitPendingAfterAcceptedIngress(const sockaddr_in& source,
                                           uint64_t nowMs) {
        if (!preparePendingAfterAcceptedIngress(source, nowMs)) return false;
        commitPreparedPendingIngress(source);
        return true;
    }

    bool activeEndpoint(sockaddr_in& out) const {
        if (!_activeKnown) {
            out = {};
            return false;
        }
        out = _active;
        return true;
    }

    bool pendingEndpoint(sockaddr_in& out) const {
        if (!_pending) {
            out = {};
            return false;
        }
        out = endpointFor(_pendingAuthorization);
        return true;
    }

    bool pending() const { return _pending; }
    uint64_t highestArmedRouteGeneration() const {
        return _highestArmedRouteGeneration;
    }

private:
    static bool validEndpoint(const sockaddr_in& endpoint) {
        return endpoint.sin_family == AF_INET
            && endpoint.sin_addr.s_addr != 0u
            && endpoint.sin_port != 0u;
    }

    static bool sameEndpoint(const sockaddr_in& left,
                             const sockaddr_in& right) {
        return left.sin_family == AF_INET
            && right.sin_family == AF_INET
            && left.sin_addr.s_addr == right.sin_addr.s_addr
            && left.sin_port == right.sin_port;
    }

    static bool sameCursor(
        const SpectatorParentRouteTransitionAuthorization& authorization,
        const SpectatorParentRouteCursor& current) {
        return authorization.sourceGeneration == current.sourceGeneration
            && authorization.nextRecord == current.nextRecord
            && authorization.nextEvent == current.nextEvent
            && authorization.digest == current.digest;
    }

    static bool validAuthorization(
        const SpectatorParentRouteTransitionAuthorization& authorization) {
        return authorization.currentParentIpv4BE != 0u
            && authorization.currentParentPortBE != 0u
            && authorization.candidateIpv4BE != 0u
            && authorization.candidatePortBE != 0u
            && authorization.reservedCurrentParent == 0u
            && authorization.reserved == 0u
            && authorization.sourceGeneration != 0u
            && authorization.routeGeneration != 0u
            && authorization.localDeadlineMs != 0u
            && authorization.activationId != 0u;
    }

    static sockaddr_in endpointFor(
        const SpectatorParentRouteTransitionAuthorization& authorization) {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = authorization.candidateIpv4BE;
        endpoint.sin_port = authorization.candidatePortBE;
        return endpoint;
    }

    static sockaddr_in currentParentFor(
        const SpectatorParentRouteTransitionAuthorization& authorization) {
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = authorization.currentParentIpv4BE;
        endpoint.sin_port = authorization.currentParentPortBE;
        return endpoint;
    }

    sockaddr_in _active{};
    bool _activeKnown = false;
    bool _pending = false;
    SpectatorParentRouteTransitionAuthorization _pendingAuthorization{};
    uint64_t _highestArmedRouteGeneration = 0u;
    uint64_t _lastActivationId = 0u;
};

}

void stopDPlayTransport();

void requestTestNetworkStatsSnapshot();
void requestSpectatorViewerDetach();

bool spectatorViewerArmValidatedParentRouteTransition(
    const transport_detail::SpectatorParentRouteTransitionAuthorization&
        authorization);

bool spectatorViewerCancelValidatedParentRouteTransition(
    uint64_t activationId);

bool startNetplayLink(const qipc::IpcConfig& cfg);

bool netAcceptedCompatibilityIdentity(
    qnetplay::compat::CompatibilityIdentityWire& out);

bool startSpectatorLink(const qipc::IpcConfig& cfg);

void netShowTerminalStatus(uint32_t code, uint32_t detail,
                           const char* reason, bool graceful = false);

bool netRequestGameWindowClose();

void netSendGoodbyeNow(uint32_t code, uint32_t detail);

constexpr uint32_t kNetNoFrame = 0xFFFFFFFFu;

bool netSetSimulationFrame(uint32_t frame);
bool netSubmitLocalInput(uint32_t frame, uint16_t packed);
bool netGetLocalExact(uint32_t frame, uint16_t& out);
bool netGetRemoteExact(uint32_t frame, uint16_t& out);
uint32_t netRemoteAckThrough();
uint32_t netPeerAckThrough();
bool netLinkEstablished();
bool netPeerAlive();
struct NetPingStats {
    uint32_t lastMicros = 0;
    uint32_t meanMicros = 0;
    uint32_t minimumMicros = 0;
    uint32_t maximumMicros = 0;
    uint32_t p95Micros = 0;
    uint32_t jitterMicros = 0;
    uint32_t lossPerMille = 0;
    uint32_t samples = 0;
};

struct NetTransportDiagnostics {
    bool available = false;
    qnetplay::CoreLinkDiagnostics core{};
    bool hasPing = false;
    NetPingStats ping{};
    uint32_t ingressAccepted = 0;
    uint32_t ingressRejected = 0;
    bool hasLastIngress = false;
    qnetplay::IngressResult lastIngress =
        qnetplay::IngressResult::Count;
    uint64_t lastIngressAgeMs = 0;
    std::array<
        uint32_t,
        static_cast<size_t>(qnetplay::IngressResult::Count)>
        ingressByResult{};
    uint32_t crossDomainDrops = 0;
    uint32_t wrongSourceDrops = 0;
    uint32_t receiveErrors = 0;
    uint32_t sendFailures = 0;
    int32_t lastSocketError = 0;
    enum class GameThreadStage : uint8_t {
        None = 0,
        DriverEnter = 1,
        TransportDrain = 2,
        Scheduler = 3,
        NativeEnter = 4,
        PresentationWait = 5,
        NativeReturn = 6,
        CanonicalCommit = 7,
        PresentationCommit = 8,
        Count = 9,
    };
    GameThreadStage gameThreadStage = GameThreadStage::None;
    uint32_t gameThreadFrame = 0;
    uint64_t gameThreadStageAgeMs = 0;
    std::array<
        uint32_t,
        static_cast<size_t>(GameThreadStage::Count)>
        gameThreadStageCounts{};
};

bool netPeerPingStats(NetPingStats& out);
bool netGetTransportDiagnostics(NetTransportDiagnostics& out);
void netPublishGameThreadHeartbeat(
    NetTransportDiagnostics::GameThreadStage stage,
    uint32_t frame);
const char* netGameThreadStageName(
    NetTransportDiagnostics::GameThreadStage stage);
uint32_t netPeerRttMicros();
uint32_t netPeerRttMs();
bool netPeerHasRtt();
bool netPeerHasSimFrontier();
uint32_t netPeerSimFrontier();
uint64_t netPeerFrontierAgeMs();

struct NetPeerPressure {
    uint32_t inputProducedThrough = 0xFFFFFFFFu;
    uint32_t confirmedFrontier = 0xFFFFFFFFu;
    uint32_t holdGeneration = 0;
    uint16_t predictionDepth = 0;
    uint8_t correctionDebt = 0;
    uint8_t runState = 0;
    uint64_t ageMs = 0;
};
void netPublishLocalPressure(uint32_t inputProducedThrough,
                             uint32_t confirmedFrontier,
                             uint16_t predictionDepth,
                             uint8_t correctionDebt,
                             uint8_t runState,
                             uint32_t holdGeneration);
bool netPeerPressure(NetPeerPressure& out);

void netSetLocalGameplayDelayStatus(uint8_t requested,
                                    uint8_t applied,
                                    bool pending);

struct NetReadyState {
    uint32_t seedGameplay = 0;
    uint32_t seedCosmetic = 0;
    uint32_t selector = 0;
    uint64_t bootstrapHash = 0;
    uint32_t timerMode = 0;
    uint32_t aiDifficulty = 0;
    uint32_t lifeMultiplier = 0;
    uint32_t roundsToWinMinusOne = 0;
};

constexpr bool netReadyMatchConfigValid(const NetReadyState& ready) {
    return ready.selector <= 1u
        && qnetplay::isSupportedQohMatchConfig(
        ready.timerMode,
        ready.aiDifficulty,
        ready.lifeMultiplier,
        ready.roundsToWinMinusOne);
}

bool netSubmitLocalReady(const NetReadyState& ready);
bool netGetRemoteReady(NetReadyState& ready);
bool netReadyComplete();

struct NetConfirmedHash {
    uint32_t layoutGeneration = 0;
    uint32_t epoch = 0;
    uint32_t frame = 0;
    uint32_t rngGameplay = 0;
    uint32_t rngCosmetic = 0;
    uint32_t rngSelector = 0;
    int32_t hp0 = 0;
    int32_t hp1 = 0;
    uint64_t gameplayHash = 0;
};

bool netSubmitConfirmedHash(const NetConfirmedHash& hash);
bool netTakeConfirmedHash(NetConfirmedHash& hash);

bool netSubmitSessionEvent(
    const qnetplay::wire::SessionEventPayload& payload);
void netClearSessionEvent();
bool netTakeSessionEvent(qnetplay::wire::SessionEventPayload& out);

bool netSubmitRematchChoice(
    const qnetplay::wire::RematchChoicePayload& payload);
bool netTakeRematchChoice(qnetplay::wire::RematchChoicePayload& out);
bool netSubmitPalette(const qnetplay::wire::PalettePayload& payload);
bool netTakePalette(qnetplay::wire::PalettePayload& out);

struct NetLayoutFingerprint {
    uint32_t layoutGeneration = 0;
    uint32_t epoch = 0;
    uint64_t digest = 0;

    uint32_t audioManifest32 = 0;
    uint32_t state32 = 0;
    uint32_t phase32 = 0;
    uint32_t fpu32 = 0;
    uint32_t virtualAudio32 = 0;
};

struct NetLayoutMismatchExplanation {
    const char* component = nullptr;
    const char* playerFacing = "the two sides' game state diverged";
    bool causedByGameFiles = false;
};

NetLayoutMismatchExplanation netLayoutComponentMismatch(
    const NetLayoutFingerprint& local, const NetLayoutFingerprint& peer);

bool netSubmitLayoutFingerprint(const NetLayoutFingerprint& fingerprint);
bool netPeekLayoutFingerprint(NetLayoutFingerprint& fingerprint);
bool netTakeLayoutFingerprint(NetLayoutFingerprint& fingerprint);
bool netLayoutFingerprintAcknowledged(uint32_t layoutGeneration,
                                      uint32_t epoch);

struct NetLinkError {
    bool active = false;
    bool fromPeer = false;
    uint32_t code = 0;
    uint32_t detail = 0;
    uint32_t reasonId = 0;
    char reason[qnetplay::wire::kTerminalReasonBytes] = {};
};

bool netGetLinkError(NetLinkError& error);
void netSetLinkError(uint32_t code, uint32_t detail,
                     const char* reason = nullptr);

bool spectatorSourceActive();
size_t spectatorSourceViewerCount();
qnetplay::spectator::ArchiveResult spectatorSourceBeginGame(
    const qnetplay::spectator::GameDescriptor& descriptor);
qnetplay::spectator::ArchiveResult spectatorSourceAppendConfirmed(
    uint64_t gameId,
    const qnetplay::spectator::ConfirmedRecord& record,
    qnetplay::spectator::ConfirmedRecord* stored = nullptr);
qnetplay::spectator::ArchiveResult spectatorSourceSealGame(
    uint64_t gameId,
    qnetplay::spectator::TerminalReason reason,
    uint64_t postTailStateHash);

struct SpectatorViewerStatus {
    bool active = false;
    bool linkAlive = false;
    bool hasGameStart = false;
    bool hasTerminal = false;
    qnetplay::spectator::ViewerPhase phase =
        qnetplay::spectator::ViewerPhase::Idle;
    qnetplay::spectator::IngressResult failure =
        qnetplay::spectator::IngressResult::Accepted;
    uint64_t gameId = qnetplay::spectator::kLatestGameId;
    uint32_t receivedNextRecord = 0;
    uint32_t playbackNextRecord = 0;
    size_t bufferedRecords = 0;
    bool sawSourceContact = false;
    uint64_t sourceContactAgeMs = 0;
    uint32_t currentMatchId = 0;
    bool sourceFrontierKnown = false;
    uint32_t sourceNextRecord = 0;
    uint32_t sourceMatchId = 0;
    uint64_t sourceFrontierStaticMs = 0;
    uint8_t sourcePhase = 0;
    size_t reorderedRecords = 0;
    uint32_t awaitingEventIndex = 0;
    uint32_t appliedEvents = 0;
};

void spectatorSourceBeginTailDrain(uint32_t deadlineMs);
bool spectatorSourceTailDrainPending();

bool spectatorViewerCheckpointProgress(uint32_t& chunksSeen,
                                       uint32_t& chunkCount);
bool spectatorViewerTakeCheckpoint(qnetplay::spectator::Checkpoint& out);
bool spectatorViewerResumeAtCheckpoint(
    uint32_t recordCursor, uint32_t eventCursor, uint64_t chainDigest,
    const qnetplay::spectator::SpectatorSetTimeline::BindSeed& seed);

uint32_t spectatorViewerSetEventCount();
bool spectatorViewerSetEventAt(uint32_t index,
                               qnetplay::spectator::SetEvent& out);

bool spectatorViewerGetStatus(SpectatorViewerStatus& status);
void spectatorPublishPlaybackPresentation(bool bufferPrimed,
                                           bool catchingUp,
                                           bool paused);

void spectatorPublishPlaybackWait(SpectatorWait reason,
                                  uint32_t haveRecords,
                                  uint32_t targetRecords);
bool spectatorViewerGetGameStart(
    qnetplay::spectator::GameStartPayload& start);
bool spectatorViewerPeekConfirmed(
    qnetplay::spectator::ConfirmedRecord& record);
qnetplay::spectator::IngressResult spectatorViewerCommitConfirmed(
    uint32_t record,
    uint64_t observedPreStateHash,
    uint64_t observedPostStateHash);
bool spectatorViewerGetTerminal(
    qnetplay::spectator::ArchiveTerminal& terminal);
bool spectatorViewerGetRejection(
    qnetplay::spectator::RejectPayload& rejection);
bool spectatorViewerResume();

void netSendInputs(uint32_t frame, uint16_t packed);
bool netGetRemote(uint32_t frame, uint16_t& out);
void netGetLocalRing(uint32_t frame, uint16_t& out);
uint32_t netRemoteHighWater();
void netSetLocalReady(uint32_t seedGameplay, uint32_t seedCosmetic, uint32_t selector);
bool netBothReady(uint32_t& hostSeedG, uint32_t& hostSeedC, uint32_t& hostSel);

void netSendSyncHash(uint32_t frame, uint32_t rngG, uint32_t rngC, uint32_t sel,
                     int32_t hp0, int32_t hp1, uint32_t stateCrc);
bool netTakeSyncHash(uint32_t& frame, uint32_t& rngG, uint32_t& rngC, uint32_t& sel,
                     int32_t& hp0, int32_t& hp1, uint32_t& stateCrc);

}
