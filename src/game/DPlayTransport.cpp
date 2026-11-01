#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "DPlayTransport.h"
#include "../qoh99_game.h"
#include "../qoh99/qoh99_vanilla_net_protocol.h"
#include "../netplay/NetplayCore.h"
#include "../spectator/SpectatorSession.h"
#include "../spectator/SpectatorRelayAdapter.h"
#include "../client/spectator/ResilientTopologyClientRuntime.h"
#include "../net/MonotonicTime.h"
#include "../net/PacingBudget.h"
#include "../net/RollingWindowStats.h"
#include "../net/RouteRttEstimator.h"
#include "../platform/WidePath.h"
#include "../platform/WineSocketRepair.h"
#include "../Log.h"
#include "SessionPresentation.h"
#include "SpectatorPlaybackPolicy.h"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace qgame {

using namespace qoh99;

namespace {

constexpr uint32_t kHelloMagic = 0xC0FFEE01u;
volatile LONG g_gotRealMsg = 0;

constexpr uint32_t kPingMagic = 0xC0FFEE02u;
constexpr uint32_t kPongMagic = 0xC0FFEE03u;
#pragma pack(push, 1)
struct HelloMsg  { uint32_t magic; uint64_t cookieLow, cookieHigh; };
struct XportPing { uint32_t magic; uint64_t cookieLow, cookieHigh; uint32_t seq; uint64_t tUs; };
#pragma pack(pop)

volatile LONG g_statTxInputs = 0;
volatile LONG g_statRxInputs = 0;
volatile LONG g_statRttMs   = -1;
volatile LONG g_statGameplayDelayRequested = 0;
volatile LONG g_statGameplayDelayApplied = 0;
volatile LONG g_statGameplayDelayPending = 0;
volatile LONG g_statIngressResults[
    static_cast<size_t>(qnetplay::IngressResult::Count)] = {};
volatile LONG g_statLastIngressResult = -1;
volatile LONG g_statLastIngressTick = 0;
volatile LONG g_statCrossDomainDrops = 0;
volatile LONG g_statWrongSourceDrops = 0;
volatile LONG g_statReceiveErrors = 0;
volatile LONG g_statSendFailures = 0;
volatile LONG g_statLastSocketError = 0;
volatile LONG g_gameThreadStage = 0;
volatile LONG g_gameThreadFrame = 0;
volatile LONG g_gameThreadStageTick = 0;
// Even = stable snapshot, odd = writer in progress.
volatile LONG g_gameThreadHeartbeatSequence = 0;
volatile LONG g_gameThreadStageCounts[
    static_cast<size_t>(
        NetTransportDiagnostics::GameThreadStage::Count)] = {};
HWND g_gameWnd = nullptr;
SRWLOCK g_titleLock = SRWLOCK_INIT;

// Set by startNetplayLink(): the pump then must never write game globals or dispatch datagrams into game code.
bool g_ownPipeline = false;

// The player link core is always accessed under g_netCs.
std::unique_ptr<qnetplay::NetplayCore> g_playerCore;
NetReadyState g_compatLocalReady{};
bool g_hasCompatLocalReady = false;

// The archive has its own mutex; viewer state is shared with the game thread under g_specCs; source state is pump-only.
std::unique_ptr<qnetplay::spectator::SpectatorArchive> g_spectatorArchive;
std::unique_ptr<qnetplay::spectator::SpectatorSource> g_spectatorSource;
std::unique_ptr<qnetplay::spectator::SpectatorViewer> g_spectatorViewer;
std::unique_ptr<qclient::spectator::topology::client::
                    ReleasedTopologyClientRuntime>
    g_releasedTopologyClient;
bool g_releasedTopologyClientAttempted = false;
bool g_releasedTopologyClientAttachedLogged = false;
// A Windows SOCKET value can be reused after close: never identify this borrow by its numeric handle alone.
bool g_releasedTopologyRelayBindingAttempted = false;
bool g_releasedTopologyRelayServingLogged = false;
bool g_releasedTopologyRelayUnavailableLogged = false;
CRITICAL_SECTION g_specCs;
bool g_specCsInit = false;
bool g_spectatorViewerMode = false;
uint64_t g_spectatorLastValidMs = 0;
volatile LONG g_spectatorSourceViewerCount = 0;
qnetplay::spectator::RelayViewerAdapter g_spectatorRelayViewer;
volatile LONG g_spectatorShutdownDetachSent = 0;
// One atomic snapshot: bit 0=primed, bit 1=catching up, bit 2=paused.
volatile LONG g_spectatorPresentationForTitle = 0;
constexpr LONG kSpectatorTitlePrimed = 1 << 0;
constexpr LONG kSpectatorTitleCatchingUp = 1 << 1;
constexpr LONG kSpectatorTitlePaused = 1 << 2;
// The wait readout: reason in bits 0-1, records in hand in 2-15, target in 16-29; both counts saturate.
volatile LONG g_spectatorWaitForTitle = 0;
constexpr uint32_t kSpectatorWaitCountMax = 0x3FFFu;

constexpr uint64_t kSpectatorPacketsPerSecond = 240;
constexpr uint64_t kSpectatorBurstPackets = 16;
constexpr uint64_t kSpectatorTokenScale = 1000;
uint64_t g_spectatorPacketTokens =
    kSpectatorBurstPackets * kSpectatorTokenScale;
uint64_t g_spectatorTokenRefillMs = 0;

// Packet shim decisions use a private RNG on the pump thread and must never consume QOH's synchronized RNG.
struct TestNetworkProfile {
    bool enabled = false;
    const char* name = "none";
    uint32_t baseDelayMs = 0;
    uint32_t jitterMs = 0;
    uint32_t lossBasisPoints = 0;
    uint32_t duplicateBasisPoints = 0;
    uint32_t reorderBasisPoints = 0;
    uint32_t reorderExtraMs = 0;
};
enum class TestNetworkDomain : uint8_t {
    Player = 1,
    SpectatorSource = 2,
    SpectatorViewer = 3,
};
struct TestDelayedDatagram {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in destination{};
    std::vector<uint8_t> bytes;
    const char* label = "test-net";
};
struct TestNetworkLane {
    TestNetworkDomain domain = TestNetworkDomain::Player;
    uint64_t key = 0;
    uint32_t rng = 1;
    uint64_t logical = 0;
    uint64_t queued = 0;
    uint64_t dropped = 0;
    uint64_t duplicated = 0;
    uint64_t reordered = 0;
    uint64_t sent = 0;
    uint64_t queueOverflows = 0;
    std::multimap<uint64_t, TestDelayedDatagram> queue;
};
TestNetworkProfile g_testNetworkProfile{};
std::unordered_map<uint64_t, TestNetworkLane> g_testNetworkLanes;
uint32_t g_testNetworkSeed = 0x51A7E5EEu;
constexpr LONG kTestNetworkStatsDisabled = 0;
constexpr LONG kTestNetworkStatsArmed = 1;
constexpr LONG kTestNetworkStatsRequested = 2;
constexpr LONG kTestNetworkStatsEmitting = 3;
constexpr LONG kTestNetworkStatsEmitted = 4;
volatile LONG g_testNetworkStatsState = kTestNetworkStatsDisabled;

constexpr uint32_t kNetMagicInputs = 0xC0FFEE10u;
constexpr uint32_t kNetMagicReady  = 0xC0FFEE11u;
constexpr uint32_t kNetMagicSync   = 0xC0FFEE12u;
constexpr int      kInputRedundancy = 30;
constexpr uint32_t kInputRingSize   = 1024;

#pragma pack(push, 1)
struct NetInputsMsg {
    uint32_t magic;
    uint64_t cookieLow, cookieHigh;
    uint32_t frame;
    uint16_t inputs[kInputRedundancy];
};
struct NetReadyMsg {
    uint32_t magic;
    uint64_t cookieLow, cookieHigh;
    uint32_t seedG, seedC, sel;
};
struct NetSyncMsg  {
    uint32_t magic;
    uint64_t cookieLow, cookieHigh;
    uint32_t frame, rngG, rngC, sel, stateCrc;
    int32_t hp0, hp1;
};
#pragma pack(pop)

CRITICAL_SECTION g_netCs;
bool     g_netCsInit = false;
uint16_t g_remoteRing[kInputRingSize] = {};
uint16_t g_localRing[kInputRingSize]  = {};
// Each slot carries the frame it holds: the ring wraps every 1024 frames, so a plain "has" bit would serve stale input.
constexpr uint32_t kNoFrame = 0xFFFFFFFFu;
uint32_t g_remoteFrameOf[kInputRingSize];
uint32_t g_localFrameOf[kInputRingSize];
void netRingsReset() {
    for (uint32_t i = 0; i < kInputRingSize; ++i) {
        g_remoteFrameOf[i] = kNoFrame;
        g_localFrameOf[i]  = kNoFrame;
        g_remoteRing[i] = 0; g_localRing[i] = 0;
    }
}
volatile LONG g_remoteHigh = -1;
volatile LONG g_peerLastRxMs = 0;
volatile LONG g_localReady = 0, g_remoteReady = 0;
NetReadyMsg g_localReadyMsg{}, g_remoteReadyMsg{};
constexpr int kSyncRingSize = 16;
NetSyncMsg g_peerSyncRing[kSyncRingSize] = {};
volatile LONG g_syncWrite = 0;
volatile LONG g_syncRead  = 0;

void netSessionReset() {
    netRingsReset();
    InterlockedExchange(&g_remoteHigh, -1);
    InterlockedExchange(&g_peerLastRxMs, 0);
    InterlockedExchange(&g_localReady, 0);
    InterlockedExchange(&g_remoteReady, 0);
    InterlockedExchange(&g_syncWrite, 0);
    InterlockedExchange(&g_syncRead, 0);
    InterlockedExchange(&g_statTxInputs, 0);
    InterlockedExchange(&g_statRxInputs, 0);
    InterlockedExchange(&g_statRttMs, -1);
    for (volatile LONG& count : g_statIngressResults)
        InterlockedExchange(&count, 0);
    InterlockedExchange(&g_statLastIngressResult, -1);
    InterlockedExchange(&g_statLastIngressTick, 0);
    InterlockedExchange(&g_statCrossDomainDrops, 0);
    InterlockedExchange(&g_statWrongSourceDrops, 0);
    InterlockedExchange(&g_statReceiveErrors, 0);
    InterlockedExchange(&g_statSendFailures, 0);
    InterlockedExchange(&g_statLastSocketError, 0);
    InterlockedExchange(&g_gameThreadStage, 0);
    InterlockedExchange(&g_gameThreadFrame, 0);
    InterlockedExchange(&g_gameThreadStageTick, 0);
    InterlockedExchange(&g_gameThreadHeartbeatSequence, 0);
    for (volatile LONG& count : g_gameThreadStageCounts)
        InterlockedExchange(&count, 0);
    InterlockedExchange(&g_gotRealMsg, 0);
    g_localReadyMsg = {};
    g_remoteReadyMsg = {};
    std::memset(g_peerSyncRing, 0, sizeof(g_peerSyncRing));
}

// Largest game message is the type-6 match CONFIG at 4232 bytes. Send cap and receive buffer must exceed it: recvfrom fails with WSAEMSGSIZE and discards the datagram otherwise.
constexpr int kMaxDatagram = 8192;

SOCKET   g_udp = INVALID_SOCKET;
SOCKET   g_spectatorUdp = INVALID_SOCKET;
sockaddr_in g_peer{};
volatile LONG g_peerKnown = 0;
transport_detail::SpectatorParentRouteTransitionGate
    g_spectatorParentRouteGate;
bool g_spectatorParentRouteGateInitialized = false;
uint64_t g_spectatorParentRouteArmedActivationId = 0u;
HANDLE   g_pumpThread = nullptr;
volatile LONG g_running = 0;

WSAEVENT g_udpReadEvent = WSA_INVALID_EVENT;
WSAEVENT g_specReadEvent = WSA_INVALID_EVENT;
WSAEVENT g_pumpWakeEvent = WSA_INVALID_EVENT;
WSAEVENT g_relayReadEvent = WSA_INVALID_EVENT;
uintptr_t g_relayReadEventSocket =
    qclient::spectator::topology::client::
        kInvalidReleasedTopologyRelaySocket;
uint64_t g_relayReadEventGeneration = 0u;
uint64_t g_relayReadEventFailedGeneration = 0u;
volatile LONG g_pumpWakeRequests = 0;
volatile LONG g_pumpEarlyWakes = 0;
volatile LONGLONG g_enqueuePendingTicks = 0;
LONGLONG g_pumpQpcFreq = 1;
qnet::RollingWindowStats<128> g_enqueueToSendUs;
qnet::RollingWindowStats<128> g_localDwellUs;

qnet::RouteRttEstimator<32, 128> g_routeEstimator;
bool g_routeWindowStarted = false;
int64_t g_routeLastProbeUs = 0;
uint32_t g_routeHeaderSeq = 1;
volatile LONG g_statRouteP50Ms = -1;
volatile LONG g_statPingLastUs = -1;
volatile LONG g_statPingMeanUs = -1;
volatile LONG g_statPingMinUs = -1;
volatile LONG g_statPingMaxUs = -1;
volatile LONG g_statPingP95Us = -1;
volatile LONG g_statPingJitterUs = 0;
volatile LONG g_statPingLossPerMille = 0;
volatile LONG g_statPingSamples = 0;
uint64_t g_routeProbesAnswered = 0;
uint64_t g_routeProbeDrops = 0;
int64_t g_routeAckTokens = 16;
int64_t g_routeAckRefillUs = 0;

static const char* routeLabelTextRt(uint8_t label) {
    switch (label) {
        case 1: return "lan-direct";
        case 2: return "public-direct";
        case 3: return "public-direct (possible NAT hairpin)";
        case 4: return "relay";
    }
    return "unlabeled";
}

inline int64_t realQpcTicks() {
    LARGE_INTEGER c;
    return QueryPerformanceCounter(&c) ? c.QuadPart : 0;
}
inline void signalPumpWake() {
    if (g_pumpWakeEvent != WSA_INVALID_EVENT) {
        InterlockedIncrement(&g_pumpWakeRequests);
        WSASetEvent(g_pumpWakeEvent);
    }
}

CRITICAL_SECTION g_rxCs;
bool     g_rxCsInit = false;
bool     g_wsaStarted = false;

qipc::IpcConfig g_cfg{};
uint32_t g_localId  = 1;
uint32_t g_remoteId = 2;

BOOL CALLBACK findGameWndCb(HWND h, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetParent(h)) {
        *reinterpret_cast<HWND*>(out) = h;
        return FALSE;
    }
    return TRUE;
}

struct CloseGameWindowsContext {
    uint32_t matched = 0u;
    uint32_t posted = 0u;
};

BOOL CALLBACK closeGameWindowsCb(HWND h, LPARAM opaque) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || GetParent(h)
        || !IsWindowVisible(h)) {
        return TRUE;
    }
    char title[256] = {};
    (void)GetWindowTextA(h, title, sizeof(title));
    if (std::strstr(title, "QOH99") == nullptr) return TRUE;
    CloseGameWindowsContext& context =
        *reinterpret_cast<CloseGameWindowsContext*>(opaque);
    ++context.matched;
    if (PostMessageA(h, WM_CLOSE, 0, 0)) ++context.posted;
    return TRUE;
}

bool setGameWindowTitleBounded(const char* title) {
    if (!title) return false;
    const std::wstring wideTitle = qplat::widen(title);
    if (title[0] != '\0' && wideTitle.empty()) return false;
    AcquireSRWLockExclusive(&g_titleLock);
    if (!g_gameWnd || !IsWindow(g_gameWnd)) {
        g_gameWnd = nullptr;
        EnumWindows(findGameWndCb, reinterpret_cast<LPARAM>(&g_gameWnd));
        if (!g_gameWnd) {
            ReleaseSRWLockExclusive(&g_titleLock);
            return false;
        }
    }

    const LRESULT stored = DefWindowProcW(
        g_gameWnd, WM_SETTEXT, 0,
        reinterpret_cast<LPARAM>(wideTitle.c_str()));
    if (!stored && !IsWindow(g_gameWnd))
        g_gameWnd = nullptr;
    ReleaseSRWLockExclusive(&g_titleLock);
    return stored != 0;
}

void updateWindowTitle() {
    if (g_spectatorViewerMode && g_specCsInit && g_spectatorViewer) {
        qnetplay::spectator::GameStartPayload start{};
        qnetplay::spectator::ArchiveTerminal terminal{};
        bool hasStart = false;
        bool hasTerminal = false;
        size_t buffered = 0;
        uint32_t received = 0;
        uint32_t playback = 0;
        qnetplay::spectator::ViewerPhase phase =
            qnetplay::spectator::ViewerPhase::Idle;
        EnterCriticalSection(&g_specCs);
        hasStart = g_spectatorViewer->gameStart(start);
        hasTerminal = g_spectatorViewer->terminal(terminal);
        buffered = g_spectatorViewer->bufferedPlaybackRecords();
        received = g_spectatorViewer->receivedNextRecord();
        playback = g_spectatorViewer->playbackNextRecord();
        phase = g_spectatorViewer->phase();
        LeaveCriticalSection(&g_specCs);

        std::string laneNames[4] = {"Player 1", "Player 2", "Player 3",
                                    "Player 4"};
        uint8_t activeLaneMask = 0x3u;
        if (hasStart) {
            qnetplay::spectator::ReadyMetadata metadata{};
            if (qnetplay::spectator::decodeReadyMetadata(
                    start.metadata, metadata)) {
                activeLaneMask = metadata.activeLaneMask;
                for (int lane = 0; lane < 4; ++lane) {
                    if (metadata.playerName[lane][0] == '\0') continue;
                    laneNames[lane] = playerNameForUtf8Display(
                        metadata.playerName[lane],
                        metadata.playerNameCodePage[lane]);
                }
            }
        }
        std::string matchup;
        {
            std::string left, right;
            for (int lane = 0; lane < 4; ++lane) {
                if ((activeLaneMask & (1u << lane)) == 0) continue;
                std::string& side = (lane % 2 == 0) ? left : right;
                if (!side.empty()) side += " + ";
                side += laneNames[lane];
            }
            if (left.empty()) left = "?";
            if (right.empty()) right = "?";
            matchup = left + " vs " + right;
        }

        const size_t backlog = buffered;
        const LONG presentation = InterlockedCompareExchange(
            &g_spectatorPresentationForTitle, 0, 0);
        const bool primed =
            (presentation & kSpectatorTitlePrimed) != 0;
        const bool catchingUp =
            (presentation & kSpectatorTitleCatchingUp) != 0;
        const bool paused =
            (presentation & kSpectatorTitlePaused) != 0;
        char state[80];
        switch (phase) {
            case qnetplay::spectator::ViewerPhase::Complete:
                std::snprintf(state, sizeof(state), "COMPLETE");
                break;
            case qnetplay::spectator::ViewerPhase::Failed:
                std::snprintf(state, sizeof(state), "FAILED");
                break;
            case qnetplay::spectator::ViewerPhase::Rejected:
                std::snprintf(state, sizeof(state), "REJECTED");
                break;
            case qnetplay::spectator::ViewerPhase::Streaming:
                if (hasTerminal) {
                    std::snprintf(state, sizeof(state),
                                  "SEALED (%zu left)", backlog);
                } else if (paused) {
                    std::snprintf(state, sizeof(state), "PAUSED");
                } else if (!primed || backlog == 0u) {
                    const uint32_t wait = static_cast<uint32_t>(
                        InterlockedCompareExchange(
                            &g_spectatorWaitForTitle, 0, 0));
                    const uint32_t have = (wait >> 2) & kSpectatorWaitCountMax;
                    const uint32_t target =
                        (wait >> 16) & kSpectatorWaitCountMax;
                    switch (static_cast<qgame::SpectatorWait>(wait & 3u)) {
                    case qgame::SpectatorWait::SourceIdle:
                        std::snprintf(state, sizeof(state),
                                      "WAITING FOR THE MATCH (%u held)", have);
                        break;
                    case qgame::SpectatorWait::AwaitingEvent:
                        std::snprintf(state, sizeof(state),
                                      "BUFFERING - needs a set event (%u "
                                      "held)", have);
                        break;
                    case qgame::SpectatorWait::Joining:
                        std::snprintf(state, sizeof(state), "JOINING");
                        break;
                    case qgame::SpectatorWait::Filling:
                    default:
                        std::snprintf(state, sizeof(state), "BUFFERING %u/%u",
                                      have, target);
                        break;
                    }
                } else if (catchingUp) {
                    const size_t excess = backlog
                            > qgame::kSpectatorLiveTargetRecords
                        ? backlog - qgame::kSpectatorLiveTargetRecords
                        : 0u;
                    const double excessSeconds =
                        static_cast<double>(excess) * 10.0 / 606.0;
                    std::snprintf(state, sizeof(state),
                                  "CATCHING UP (%zu excess, ~%.1fs)",
                                  excess, excessSeconds);
                } else {
                    std::snprintf(state, sizeof(state), "LIVE");
                }
                break;
            default:
                std::snprintf(state, sizeof(state),
                              hasStart ? "BUFFERING" : "CONNECTING");
                break;
        }
        char title[256];
        std::snprintf(
            title, sizeof(title),
            "QOH99 SPECTATOR | %s | %s | game %llu f %u/%u buffer %zu",
            matchup.c_str(), state,
            static_cast<unsigned long long>(
                hasStart ? start.gameId
                         : qnetplay::spectator::kLatestGameId),
            playback, received, buffered);
        setGameWindowTitleBounded(title);
        return;
    }

    char rttStr[16];
    long rtt = InterlockedCompareExchange(&g_statRttMs, 0, 0);
    if (rtt < 0) std::snprintf(rttStr, sizeof(rttStr), "?");
    else         std::snprintf(rttStr, sizeof(rttStr), "%ldms", rtt);
    const long localFrames =
        InterlockedCompareExchange(&g_statTxInputs, 0, 0);
    long remoteFrames =
        InterlockedCompareExchange(&g_statRxInputs, 0, 0);
    long peerAckFrames = -1;
    bool established = false;
    qnetplay::TerminalState terminal{};
    if (g_playerCore && g_netCsInit) {
        EnterCriticalSection(&g_netCs);
        const uint32_t remoteAck = g_playerCore->remoteAckThrough();
        const uint32_t peerAck = g_playerCore->peerAckThrough();
        established = g_playerCore->isEstablished();
        terminal = g_playerCore->terminalState();
        LeaveCriticalSection(&g_netCs);
        remoteFrames = remoteAck == qnetplay::wire::kNoFrame
            ? 0
            : static_cast<long>((std::min)(
                  remoteAck + 1u,
                  static_cast<uint32_t>(LONG_MAX)));
        peerAckFrames = peerAck == qnetplay::wire::kNoFrame
            ? 0
            : static_cast<long>((std::min)(
                  peerAck + 1u,
                  static_cast<uint32_t>(LONG_MAX)));
    }
    const std::string playerOne = playerNameForUtf8Display(
        g_cfg.localPlayer == 1 ? g_cfg.localName : g_cfg.remoteName,
        g_cfg.localPlayer == 1
            ? g_cfg.localNameCodePage : g_cfg.remoteNameCodePage);
    const std::string playerTwo = playerNameForUtf8Display(
        g_cfg.localPlayer == 2 ? g_cfg.localName : g_cfg.remoteName,
        g_cfg.localPlayer == 2
            ? g_cfg.localNameCodePage : g_cfg.remoteNameCodePage);
    const long requestedDelay = InterlockedCompareExchange(
        &g_statGameplayDelayRequested, 0, 0);
    const long appliedDelay = InterlockedCompareExchange(
        &g_statGameplayDelayApplied, 0, 0);
    const bool delayPending = InterlockedCompareExchange(
        &g_statGameplayDelayPending, 0, 0) != 0;
    char delayText[24];
    if (delayPending && requestedDelay != appliedDelay) {
        std::snprintf(delayText, sizeof(delayText), "d%ld>%ld",
                      appliedDelay, requestedDelay);
    } else {
        std::snprintf(delayText, sizeof(delayText), "d%ld",
                      appliedDelay);
    }

    char t[256];
    if (peerAckFrames >= 0) {
        QLOG("[netstats] established=%u rttMs=%ld "
             "localSubmitted=%ld remoteContiguous=%ld peerAcked=%ld "
             "acceptedInputPackets=%ld terminal=%u/%s(%u)/%u "
             "reasonId=%08X reason='%s'",
             established ? 1u : 0u, rtt,
             localFrames, remoteFrames, peerAckFrames,
             InterlockedCompareExchange(&g_statRxInputs, 0, 0),
             terminal.active ? 1u : 0u,
             qnetplay::wire::terminalErrorCodeName(terminal.code),
             static_cast<unsigned>(terminal.code),
             terminal.detail, terminal.reasonId,
             terminal.reason.data());
        if (terminal.active) {
            std::snprintf(
                t, sizeof(t),
                "QOH99 NETPLAY STOPPED | %s(P1) vs %s(P2) | "
                "%s %s(%u) id %08X: %.72s | see logs",
                playerOne.c_str(), playerTwo.c_str(),
                terminal.fromPeer ? "peer" : "local",
                qnetplay::wire::terminalErrorCodeName(terminal.code),
                static_cast<unsigned>(terminal.code),
                terminal.reasonId, terminal.reason.data());
        } else {
            std::snprintf(
                t, sizeof(t),
                "QOH99 Netplay | %s(P1) vs %s(P2) | you P%u",
                playerOne.c_str(), playerTwo.c_str(), g_cfg.localPlayer);
        }
    } else {
        std::snprintf(
            t, sizeof(t),
            "QOH99 Netplay | %s(P1) vs %s(P2) | you P%u",
            playerOne.c_str(), playerTwo.c_str(), g_cfg.localPlayer);
    }
    setGameWindowTitleBounded(t);
}

typedef void (__thiscall* DispatchFn)(void* msg, int idFrom, int idTo);

DispatchFn dispatcher() { return reinterpret_cast<DispatchFn>(*reinterpret_cast<uint32_t*>(kNetDispatchPtr)); }

inline void wr32(uintptr_t a, uint32_t v) { *reinterpret_cast<volatile uint32_t*>(a) = v; }
inline uint32_t rd32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

static std::string epStr(const sockaddr_in& a);

static std::string epStr(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, const_cast<in_addr*>(&a.sin_addr), ip, sizeof(ip));
    char b[64]; wsprintfA(b, "%s:%u", ip, ntohs(a.sin_port));
    return b;
}

// Vanilla sizes from sub_45D970: 12 bytes for scalar and input types, 4232 for the type-6 config, and at least 12 for variable-length type-4 chat.
static bool msgSizeValid(uint32_t type, int n) {
    using namespace qoh99::vnet;
    switch (type) {
        case MSG_INPUT: case MSG_SEQ: case MSG_SEED:
        case MSG_SIDE:  case MSG_PARAM5: case MSG_READY:
            return n >= 12;
        case MSG_CHAT:
            return n >= 12 && n <= kMaxDatagram;
        case MSG_CONFIG:
            return n == (int)kConfigWireSize;
        default:
            return false;
    }
}

static bool cookieMatches(uint64_t low, uint64_t high) {
    return low == g_cfg.cookieLow && high == g_cfg.cookieHigh;
}

static void sendHello() {
    if (g_udp == INVALID_SOCKET || InterlockedCompareExchange(&g_peerKnown, 1, 1) != 1) return;
    const HelloMsg hello{ kHelloMagic, g_cfg.cookieLow, g_cfg.cookieHigh };
    ::sendto(g_udp, reinterpret_cast<const char*>(&hello), sizeof(hello), 0,
             reinterpret_cast<sockaddr*>(&g_peer), sizeof(g_peer));
}

static bool sameEndpoint(const sockaddr_in& left,
                         const sockaddr_in& right) {
    return left.sin_family == AF_INET
        && right.sin_family == AF_INET
        && left.sin_addr.s_addr == right.sin_addr.s_addr
        && left.sin_port == right.sin_port;
}

static bool makeNonblocking(SOCKET socket, const char* domain);
static bool getLocalSocketAddress(
    SOCKET socket, sockaddr_in& address, const char* domain);

static qnetplay::Endpoint coreEndpoint(const sockaddr_in& address) {
    return qnetplay::Endpoint{
        address.sin_addr.s_addr,
        ntohs(address.sin_port),
    };
}

static sockaddr_in socketEndpoint(const qnetplay::Endpoint& endpoint) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = endpoint.ipv4;
    address.sin_port = htons(endpoint.port);
    return address;
}

static bool directQspEndpointCandidate(const sockaddr_in& peer,
                                       sockaddr_in& out) {
    out = {};
    if (g_udp == INVALID_SOCKET || peer.sin_family != AF_INET
        || peer.sin_addr.s_addr == 0u || peer.sin_port == 0u) {
        return false;
    }
    sockaddr_in bound{};
    int boundBytes = sizeof(bound);
    if (::getsockname(g_udp, reinterpret_cast<sockaddr*>(&bound), &boundBytes)
            != 0
        || boundBytes != sizeof(bound) || bound.sin_family != AF_INET
        || bound.sin_port == 0u) {
        return false;
    }
    if (bound.sin_addr.s_addr != 0u) {
        out = bound;
        return true;
    }

    SOCKET routeProbe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (routeProbe == INVALID_SOCKET) return false;
    const int connected = ::connect(routeProbe,
                                    reinterpret_cast<const sockaddr*>(&peer),
                                    sizeof(peer));
    sockaddr_in selected{};
    int selectedBytes = sizeof(selected);
    const bool selectedRoute = connected == 0
        && ::getsockname(routeProbe,
                         reinterpret_cast<sockaddr*>(&selected),
                         &selectedBytes) == 0
        && selectedBytes == sizeof(selected) && selected.sin_family == AF_INET
        && selected.sin_addr.s_addr != 0u;
    ::closesocket(routeProbe);
    if (!selectedRoute) return false;
    selected.sin_port = bound.sin_port;
    out = selected;
    return true;
}

static bool releasedTopologyClientConfigForInitAck(
    const qnetplay::spectator::GameStartPayload& start,
    qclient::spectator::topology::client::ReleasedTopologyClientConfig& out) {
    using qclient::spectator::topology::EndpointProvenance;
    out = {};
    const bool directRoute =
        g_cfg.spectatorTopologyRouteKind == qipc::kSpectatorTopologyRouteDirect
        && g_cfg.spectatorRelayActive == 0u;
    const bool relayRoute =
        g_cfg.spectatorTopologyRouteKind == qipc::kSpectatorTopologyRouteRelay
        && g_cfg.spectatorRelayActive == 1u;
    if (g_cfg.spectatorTopologyEnabled != 1u
        || (!directRoute && !relayRoute)
        || g_udp == INVALID_SOCKET
        || g_peer.sin_family != AF_INET || g_peer.sin_addr.s_addr == 0u
        || g_peer.sin_port == 0u || start.viewerId != g_cfg.spectatorViewerId
        || start.generation != g_cfg.spectatorGeneration
        || !qnetplay::compat::semanticallyEqual(start.compatibilityIdentity,
                                                 g_cfg.compatibilityIdentity)) {
        return false;
    }
    uint32_t qspIpv4BE = 0u;
    uint16_t qspPort = 0u;
    if (relayRoute) {
        if (!g_spectatorRelayViewer.enabled()
            || g_peer.sin_addr.s_addr != g_cfg.peerAddrBE
            || g_peer.sin_port != g_cfg.peerPortBE) {
            return false;
        }
        qspIpv4BE = g_cfg.peerAddrBE;
        qspPort = ntohs(g_cfg.peerPortBE);
    } else {
        sockaddr_in local{};
        if (!directQspEndpointCandidate(g_peer, local)) {
            return false;
        }
        qspIpv4BE = local.sin_addr.s_addr;
        qspPort = ntohs(local.sin_port);
    }
    const uint64_t root = qnetplay::spectator::gameStartFingerprint(start);
    if (root == 0u) return false;
    out.enabled = true;
    out.hostControl.sa.sin_family = AF_INET;
    out.hostControl.sa.sin_addr.s_addr =
        g_cfg.spectatorTopologyHostControlIpv4BE;
    out.hostControl.sa.sin_port =
        htons(g_cfg.spectatorTopologyHostControlPort);
    out.qspEndpoint = { qspIpv4BE, qspPort };
    out.rootQspEndpoint = { g_peer.sin_addr.s_addr, ntohs(g_peer.sin_port) };
    out.qspRoute = relayRoute ? qnet::SPECTATE_ROUTE_RELAY
                              : qnet::SPECTATE_ROUTE_DIRECT;
    out.qspProvenance = relayRoute ? EndpointProvenance::ProvenRelayQsp
                                   : EndpointProvenance::ProvenDirectQsp;
    out.qspRouteGeneration = g_cfg.spectatorTopologyQspRouteGeneration;
    out.viewerId = g_cfg.spectatorViewerId;
    out.viewerGeneration = g_cfg.spectatorGeneration;
    out.sessionId = g_cfg.sessionId;
    out.sessionEpoch = 1u;
    out.spectatorCookieLow = g_cfg.spectatorCookieLow;
    out.spectatorCookieHigh = g_cfg.spectatorCookieHigh;
    out.compatibility = g_cfg.compatibilityIdentity;
    out.streamRootIdentity = root;
    out.grant.grantId = g_cfg.spectatorGrantId;
    out.grant.viewerId = g_cfg.spectatorViewerId;
    out.grant.sessionId = g_cfg.sessionId;
    out.grant.expiryUnix = g_cfg.spectatorGrantExpiryUnix;
    out.grant.sourceGeneration = g_cfg.spectatorTopologyGrantSourceGeneration;
    out.grant.resumeRecord = g_cfg.spectatorResumeRecord;
    out.grant.resumeDigest = g_cfg.spectatorResumeDigest;
    out.grant.route = out.qspRoute;
    out.grant.macLow = g_cfg.spectatorGrantMacLow;
    out.grant.macHigh = g_cfg.spectatorGrantMacHigh;
    out.relayQsrPermitted = relayRoute;
    return true;
}

static bool releasedTopologyClientQspFrontier(
    uint32_t& nextRecord, uint32_t& nextEvent, uint64_t& digest,
    qnetplay::spectator::ViewerPhase& phase) {
    nextRecord = 0u;
    nextEvent = 0u;
    digest = 0u;
    phase = qnetplay::spectator::ViewerPhase::Idle;
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    nextRecord = g_spectatorViewer->receivedNextRecord();
    nextEvent = g_spectatorViewer->receivedNextEvent();
    digest = g_spectatorViewer->receivedDigest();
    phase = g_spectatorViewer->phase();
    LeaveCriticalSection(&g_specCs);
    return true;
}

static bool releasedTopologyE0Terminal(
    const qnetplay::spectator::ViewerPhase phase) {
    using qnetplay::spectator::ViewerPhase;
    return phase == ViewerPhase::Idle || phase == ViewerPhase::Complete
        || phase == ViewerPhase::Rejected || phase == ViewerPhase::Failed
        || phase == ViewerPhase::Detached;
}

static void armReleasedTopologyClientAfterInitAck(const uint64_t nowMs) {
    if (g_releasedTopologyClientAttempted
        || g_cfg.spectatorTopologyEnabled != 1u) {
        return;
    }
    g_releasedTopologyClientAttempted = true;
    if (g_cfg.spectatorTopologyRouteKind
        == qipc::kSpectatorTopologyRoutePunched) {
        QLOG("[spectator-topology] punched QSR is not runtime-proven; E0 continues");
        return;
    }
    qnetplay::spectator::GameStartPayload start{};
    bool haveStart = false;
    if (g_specCsInit && g_spectatorViewer) {
        EnterCriticalSection(&g_specCs);
        haveStart = g_spectatorViewer->gameStart(start);
        LeaveCriticalSection(&g_specCs);
    }
    qclient::spectator::topology::client::ReleasedTopologyClientConfig config{};
    if (!haveStart || !releasedTopologyClientConfigForInitAck(start, config)) {
        QLOG("[spectator-topology] released bootstrap unavailable; E0 continues");
        return;
    }
    std::unique_ptr<qclient::spectator::topology::client::
                        ReleasedTopologyClientRuntime>
        runtime(new (std::nothrow)
                    qclient::spectator::topology::client::
                        ReleasedTopologyClientRuntime());
    uint32_t nextRecord = 0u;
    uint32_t nextEvent = 0u;
    uint64_t digest = 0u;
    qnetplay::spectator::ViewerPhase phase =
        qnetplay::spectator::ViewerPhase::Idle;
    if (!runtime || !runtime->start(config, nowMs)
        || !releasedTopologyClientQspFrontier(nextRecord, nextEvent, digest,
                                              phase)
        || releasedTopologyE0Terminal(phase)
        || !runtime->updateVerifiedQspFrontier(nextRecord, nextEvent, digest)
        || !runtime->onQspInitAckTransmitted(nowMs)) {
        QLOG("[spectator-topology] released bootstrap failed; E0 continues");
        return;
    }
    g_releasedTopologyClient = std::move(runtime);
    g_releasedTopologyClientAttachedLogged = false;
    QLOG("[spectator-topology] released %s enrollment armed after QSP InitAck",
         config.relayQsrPermitted ? "relayed" : "direct");
}

static qnetplay::spectator::ViewerId spectatorViewerId(
    const sockaddr_in& address) {
    return (static_cast<uint64_t>(address.sin_addr.s_addr) << 16)
        | static_cast<uint64_t>(ntohs(address.sin_port));
}

static sockaddr_in spectatorViewerAddress(
    qnetplay::spectator::ViewerId viewer) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = static_cast<uint32_t>(viewer >> 16);
    address.sin_port = htons(static_cast<uint16_t>(viewer));
    return address;
}

static uint32_t datagramMagic(const uint8_t* bytes, size_t length) {
    uint32_t magic = 0;
    if (bytes && length >= sizeof(magic))
        std::memcpy(&magic, bytes, sizeof(magic));
    return magic;
}

static const char* testNetworkDomainName(TestNetworkDomain domain) {
    switch (domain) {
    case TestNetworkDomain::Player: return "player";
    case TestNetworkDomain::SpectatorSource: return "spectator-source";
    case TestNetworkDomain::SpectatorViewer: return "spectator-viewer";
    default: return "unknown";
    }
}

static uint64_t testNetworkLaneKey(
    TestNetworkDomain domain, const sockaddr_in& destination) {
    const uint64_t endpoint =
        (static_cast<uint64_t>(destination.sin_addr.s_addr) << 16)
        | static_cast<uint64_t>(ntohs(destination.sin_port));
    return (static_cast<uint64_t>(domain) << 56) | endpoint;
}

static uint32_t mixTestNetworkSeed(uint32_t seed, uint64_t laneKey) {
    uint64_t value =
        laneKey ^ (static_cast<uint64_t>(seed) << 32) ^ seed;
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    value ^= value >> 31;
    uint32_t mixed =
        static_cast<uint32_t>(value ^ (value >> 32));
    return mixed == 0 ? 1u : mixed;
}

static uint32_t nextTestNetworkRandom(TestNetworkLane& lane) {
    uint32_t value = lane.rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    lane.rng = value == 0 ? 1u : value;
    return lane.rng;
}

static TestNetworkLane& testNetworkLane(
    TestNetworkDomain domain, const sockaddr_in& destination) {
    const uint64_t key = testNetworkLaneKey(domain, destination);
    const auto inserted = g_testNetworkLanes.emplace(
        key, TestNetworkLane{});
    TestNetworkLane& lane = inserted.first->second;
    if (inserted.second) {
        lane.domain = domain;
        lane.key = key;
        lane.rng = mixTestNetworkSeed(g_testNetworkSeed, key);
        QLOG("[test-net] LANE domain=%s key=%016llX seed=%08X dst=%s",
             testNetworkDomainName(domain),
             static_cast<unsigned long long>(key), lane.rng,
             epStr(destination).c_str());
    }
    return lane;
}

static void resetTestNetworkImpairment() {
    InterlockedExchange(
        &g_testNetworkStatsState, kTestNetworkStatsDisabled);
    g_testNetworkProfile = TestNetworkProfile{};
    g_testNetworkLanes.clear();
    g_testNetworkSeed = 0x51A7E5EEu;
}

static void configureTestNetworkImpairment() {
    resetTestNetworkImpairment();
    char value[64] = {};
    const DWORD length = GetEnvironmentVariableA(
        "QOH99_NP_TEST_NET_PROFILE", value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) return;

    struct NamedProfile {
        const char* optIn;
        const char* name;
        uint32_t delay;
        uint32_t jitter;
        uint32_t loss;
        uint32_t duplicate;
        uint32_t reorder;
        uint32_t reorderExtra;
    };
    static constexpr NamedProfile kProfiles[] = {
        { "live-e2e:stable-low",  "stable-low",  8,   0, 0,    0,   0,   0 },
        { "live-e2e:stable-120",  "stable-120",  50,  0, 0,    0,   0,   0 },
        { "live-e2e:stable-high", "stable-high", 100, 0, 0,    0,   0,   0 },
        { "live-e2e:stable-250",  "stable-250", 115,  0, 0,    0,   0,   0 },
        { "live-e2e:stable-300",  "stable-300", 140,  0, 0,    0,   0,   0 },
        { "live-e2e:jitter-low",  "jitter-low",  15, 25, 0,  200, 400,  45 },
        { "live-e2e:jitter-high", "jitter-high", 90, 70, 0,  250, 500, 120 },
        { "live-e2e:packet-loss", "packet-loss", 35, 10, 1500, 250, 500, 80 },
        { "live-e2e:asym-updown", "asym-updown", 50,  0, 0,    0,   0,   0 },
        { "live-e2e:asym-downup", "asym-downup",  1,  0, 0,    0,   0,   0 },
    };
    for (const NamedProfile& profile : kProfiles) {
        if (std::strcmp(value, profile.optIn) != 0) continue;
        g_testNetworkProfile.enabled = true;
        g_testNetworkProfile.name = profile.name;
        g_testNetworkProfile.baseDelayMs = profile.delay;
        g_testNetworkProfile.jitterMs = profile.jitter;
        g_testNetworkProfile.lossBasisPoints = profile.loss;
        g_testNetworkProfile.duplicateBasisPoints = profile.duplicate;
        g_testNetworkProfile.reorderBasisPoints = profile.reorder;
        g_testNetworkProfile.reorderExtraMs = profile.reorderExtra;
        break;
    }
    if (!g_testNetworkProfile.enabled) return;

    if (std::strcmp(
            g_testNetworkProfile.name, "packet-loss") == 0
        && !g_cfg.isHost) {
        g_testNetworkProfile.baseDelayMs = 44;
        g_testNetworkProfile.jitterMs = 13;
        g_testNetworkProfile.lossBasisPoints = 1875;
        g_testNetworkProfile.reorderExtraMs = 100;
    }
    if (std::strcmp(g_testNetworkProfile.name, "asym-updown") == 0
        && !g_cfg.isHost) {
        g_testNetworkProfile.baseDelayMs = 1;
    }
    if (std::strcmp(g_testNetworkProfile.name, "asym-downup") == 0
        && !g_cfg.isHost) {
        g_testNetworkProfile.baseDelayMs = 50;
    }

    char seedText[32] = {};
    const DWORD seedLength = GetEnvironmentVariableA(
        "QOH99_NP_TEST_NET_SEED", seedText, sizeof(seedText));
    if (seedLength != 0 && seedLength < sizeof(seedText)) {
        char* end = nullptr;
        const unsigned long parsed =
            std::strtoul(seedText, &end, 0);
        if (end != seedText && *end == '\0')
            g_testNetworkSeed = static_cast<uint32_t>(parsed);
    }
    if (g_testNetworkSeed == 0) g_testNetworkSeed = 1;
    QLOG("[test-net] ENABLED profile=%s seed=%08X role=%s "
         "delay=%u jitter=+/- %u lossBp=%u dupBp=%u "
         "reorderBp=%u reorderExtraMs=%u (isolated live-E2E only)",
         g_testNetworkProfile.name, g_testNetworkSeed,
         g_cfg.isHost ? "host" :
             (g_spectatorViewerMode ? "viewer" : "client"),
         g_testNetworkProfile.baseDelayMs,
         g_testNetworkProfile.jitterMs,
         g_testNetworkProfile.lossBasisPoints,
         g_testNetworkProfile.duplicateBasisPoints,
         g_testNetworkProfile.reorderBasisPoints,
         g_testNetworkProfile.reorderExtraMs);
    InterlockedExchange(
        &g_testNetworkStatsState, kTestNetworkStatsArmed);
}

static void requestTestNetworkStatsSnapshotNoWait() {
    InterlockedCompareExchange(
        &g_testNetworkStatsState,
        kTestNetworkStatsRequested,
        kTestNetworkStatsArmed);
}

static void serviceTestNetworkStatsSnapshotRequest() {
    if (InterlockedCompareExchange(
            &g_testNetworkStatsState,
            kTestNetworkStatsEmitting,
            kTestNetworkStatsRequested)
        != kTestNetworkStatsRequested) {
        return;
    }

    if (!g_testNetworkProfile.enabled) {
        InterlockedExchange(
            &g_testNetworkStatsState, kTestNetworkStatsEmitted);
        return;
    }

    uint64_t logical = 0;
    uint64_t queued = 0;
    uint64_t sent = 0;
    uint64_t dropped = 0;
    uint64_t duplicated = 0;
    uint64_t reordered = 0;
    uint64_t pending = 0;
    uint64_t queueOverflows = 0;
    for (const auto& entry : g_testNetworkLanes) {
        const TestNetworkLane& lane = entry.second;
        logical += lane.logical;
        queued += lane.queued;
        sent += lane.sent;
        dropped += lane.dropped;
        duplicated += lane.duplicated;
        reordered += lane.reordered;
        pending += lane.queue.size();
        queueOverflows += lane.queueOverflows;
        QLOG("[test-net] LANE-STATS domain=%s key=%016llX "
             "seed=%08X logical=%llu queued=%llu sent=%llu "
             "dropped=%llu duplicated=%llu reordered=%llu "
             "pending=%zu queueOverflows=%llu",
             testNetworkDomainName(lane.domain),
             static_cast<unsigned long long>(lane.key), lane.rng,
             static_cast<unsigned long long>(lane.logical),
             static_cast<unsigned long long>(lane.queued),
             static_cast<unsigned long long>(lane.sent),
             static_cast<unsigned long long>(lane.dropped),
             static_cast<unsigned long long>(lane.duplicated),
             static_cast<unsigned long long>(lane.reordered),
             lane.queue.size(),
             static_cast<unsigned long long>(
                 lane.queueOverflows));
    }
    QLOG("[test-net] STATS profile=%s logical=%llu queued=%llu "
         "sent=%llu dropped=%llu duplicated=%llu reordered=%llu "
         "pending=%llu queueOverflows=%llu lanes=%zu",
         g_testNetworkProfile.name,
         static_cast<unsigned long long>(logical),
         static_cast<unsigned long long>(queued),
         static_cast<unsigned long long>(sent),
         static_cast<unsigned long long>(dropped),
         static_cast<unsigned long long>(duplicated),
         static_cast<unsigned long long>(reordered),
         static_cast<unsigned long long>(pending),
         static_cast<unsigned long long>(queueOverflows),
         g_testNetworkLanes.size());
    InterlockedExchange(
        &g_testNetworkStatsState, kTestNetworkStatsEmitted);
}

static bool sendDatagramRaw(SOCKET socket,
                            const sockaddr_in& destination,
                            const std::vector<uint8_t>& bytes,
                            const char* domain) {
    if (socket == INVALID_SOCKET || bytes.empty()) return false;
    const int sent = ::sendto(
        socket,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(
            const_cast<sockaddr_in*>(&destination)),
        sizeof(destination));
    if (sent == static_cast<int>(bytes.size())) return true;
    const int socketError =
        sent == SOCKET_ERROR ? WSAGetLastError() : 0;
    InterlockedIncrement(&g_statSendFailures);
    InterlockedExchange(&g_statLastSocketError, socketError);
    static uint64_t failures = 0;
    if (++failures <= 8 || failures % 300 == 0) {
        QLOG("[%s] sendto failed/short #%llu sent=%d wanted=%zu err=%d dst=%s",
             domain, static_cast<unsigned long long>(failures), sent,
             bytes.size(), socketError, epStr(destination).c_str());
    }
    return false;
}

static uint32_t testNetworkDelayMs(TestNetworkLane& lane) {
    uint32_t delay = g_testNetworkProfile.baseDelayMs;
    const uint32_t jitter = g_testNetworkProfile.jitterMs;
    if (jitter != 0) {
        const uint32_t span = jitter * 2u + 1u;
        const int32_t offset =
            static_cast<int32_t>(nextTestNetworkRandom(lane) % span)
            - static_cast<int32_t>(jitter);
        const int32_t adjusted =
            static_cast<int32_t>(delay) + offset;
        delay = adjusted > 0 ? static_cast<uint32_t>(adjusted) : 0u;
    }
    return delay;
}

static bool queueTestNetworkDatagram(
    TestNetworkLane& lane, SOCKET socket,
    const sockaddr_in& destination,
    const std::vector<uint8_t>& bytes, const char* label,
    uint64_t dueMs) {
    const size_t maxQueued =
        lane.domain == TestNetworkDomain::Player ? 8192u : 2048u;
    if (lane.queue.size() >= maxQueued) {
        ++lane.queueOverflows;
        ++lane.dropped;
        if (lane.queueOverflows <= 8
            || (lane.queueOverflows % 300u) == 0u) {
            QLOG("[test-net] DROP queue overflow #%llu domain=%s "
                 "queued=%zu",
                 static_cast<unsigned long long>(
                     lane.queueOverflows),
                 testNetworkDomainName(lane.domain), lane.queue.size());
        }
        return false;
    }
    TestDelayedDatagram delayed{};
    delayed.socket = socket;
    delayed.destination = destination;
    delayed.bytes = bytes;
    delayed.label = label;
    lane.queue.emplace(dueMs, std::move(delayed));
    ++lane.queued;
    return true;
}

static void flushTestNetworkImpairment(
    TestNetworkDomain domain, uint64_t nowMs) {
    for (auto& entry : g_testNetworkLanes) {
        TestNetworkLane& lane = entry.second;
        if (lane.domain != domain) continue;
        while (!lane.queue.empty()
               && lane.queue.begin()->first <= nowMs) {
            auto packet = lane.queue.begin();
            if (sendDatagramRaw(
                    packet->second.socket,
                    packet->second.destination,
                    packet->second.bytes,
                    packet->second.label)) {
                ++lane.sent;
            }
            lane.queue.erase(packet);
        }
    }
}

static bool sendDatagramOn(SOCKET socket,
                           const sockaddr_in& destination,
                           const std::vector<uint8_t>& bytes,
                           TestNetworkDomain domain,
                           const char* label) {
    if (!g_testNetworkProfile.enabled)
        return sendDatagramRaw(socket, destination, bytes, label);
    if (socket == INVALID_SOCKET || bytes.empty()) return false;
    TestNetworkLane& lane = testNetworkLane(domain, destination);
    ++lane.logical;
    if (g_testNetworkProfile.lossBasisPoints != 0
        && (nextTestNetworkRandom(lane) % 10000u)
            < g_testNetworkProfile.lossBasisPoints) {
        ++lane.dropped;
        return true;
    }

    const uint64_t nowMs = GetTickCount64();
    uint32_t delayMs = testNetworkDelayMs(lane);
    if (g_testNetworkProfile.reorderBasisPoints != 0
        && (nextTestNetworkRandom(lane) % 10000u)
            < g_testNetworkProfile.reorderBasisPoints) {
        delayMs += g_testNetworkProfile.reorderExtraMs;
        ++lane.reordered;
    }
    if (!queueTestNetworkDatagram(
            lane, socket, destination, bytes, label,
            nowMs + delayMs)) {
        return true;
    }
    if (g_testNetworkProfile.duplicateBasisPoints != 0
        && (nextTestNetworkRandom(lane) % 10000u)
            < g_testNetworkProfile.duplicateBasisPoints) {
        ++lane.duplicated;
        const uint32_t duplicateSkew =
            1u + (nextTestNetworkRandom(lane) % 17u);
        (void)queueTestNetworkDatagram(
            lane, socket, destination, bytes, label,
            nowMs + delayMs + duplicateSkew);
    }
    return true;
}

static size_t spectatorSourcePacketBudget(uint64_t nowMs) {
    const uint64_t capacity =
        kSpectatorBurstPackets * kSpectatorTokenScale;
    if (g_spectatorTokenRefillMs == 0 || nowMs < g_spectatorTokenRefillMs) {
        g_spectatorTokenRefillMs = nowMs;
    } else if (nowMs > g_spectatorTokenRefillMs) {
        const uint64_t elapsedMs = nowMs - g_spectatorTokenRefillMs;
        const uint64_t refill = elapsedMs * kSpectatorPacketsPerSecond;
        g_spectatorPacketTokens = (std::min)(
            capacity, g_spectatorPacketTokens + refill);
        g_spectatorTokenRefillMs = nowMs;
    }
    return static_cast<size_t>((std::min<uint64_t>)(
        8, g_spectatorPacketTokens / kSpectatorTokenScale));
}

static void serviceSpectatorRelayRegistration(uint64_t nowMs) {
    if (!g_spectatorViewerMode || !g_spectatorRelayViewer.enabled()
        || g_udp == INVALID_SOCKET
        || InterlockedCompareExchange(&g_peerKnown, 0, 0) != 1) {
        return;
    }
    std::vector<uint8_t> registration;
    if (!g_spectatorRelayViewer.encodeRegistration(
            nowMs, registration)) {
        return;
    }
    (void)sendDatagramOn(
        g_udp, g_peer, registration,
        TestNetworkDomain::SpectatorViewer,
        "spectator-relay-register");
}

static void servicePlayerOutbound(uint64_t nowMs) {
    flushTestNetworkImpairment(TestNetworkDomain::Player, nowMs);
    std::vector<qnetplay::OutboundDatagram> playerPackets;
    if (g_playerCore && g_netCsInit) {
        EnterCriticalSection(&g_netCs);
        playerPackets = g_playerCore->pollOutbound(nowMs);
        const long rtt = g_playerCore->hasRtt()
            ? static_cast<long>(g_playerCore->rttMs())
            : -1;
        InterlockedExchange(&g_statRttMs, rtt);
        LeaveCriticalSection(&g_netCs);
    }
    for (const qnetplay::OutboundDatagram& packet : playerPackets) {
        const sockaddr_in destination =
            socketEndpoint(packet.destination);
        sendDatagramOn(
            g_udp, destination, packet.bytes,
            TestNetworkDomain::Player, "netlink");
    }
    if (!playerPackets.empty()) {
        const LONGLONG enq = InterlockedExchange64(&g_enqueuePendingTicks, 0);
        if (enq != 0) {
            const int64_t us =
                qnet::ticksToMicros(realQpcTicks() - enq, g_pumpQpcFreq);
            if (us >= 0) g_enqueueToSendUs.add(us);
        }
    }
}

constexpr uint32_t kSpectatorTailDrainDeadlineMs = 3000u;
volatile LONG  g_spectatorTailDrainArmed = 0;
volatile LONG  g_spectatorTailDrainComplete = 0;
volatile LONG64 g_spectatorTailDrainDeadlineAt = 0;

void beginTailDrainInternal(uint32_t deadlineMs) {
    if (InterlockedCompareExchange(&g_spectatorTailDrainArmed, 1, 0) != 0)
        return;
    const uint32_t bounded =
        deadlineMs == 0 ? kSpectatorTailDrainDeadlineMs : deadlineMs;
    InterlockedExchange64(
        &g_spectatorTailDrainDeadlineAt,
        static_cast<LONG64>(GetTickCount64() + bounded));
    InterlockedExchange(&g_spectatorTailDrainComplete, 0);
    QLOG("[spectator-source] sealed-tail drain armed deadline=%ums viewers=%ld "
         "(delivery continues on the pump thread; the game thread is not "
         "blocked and publication has already stopped)",
         bounded,
         InterlockedCompareExchange(&g_spectatorSourceViewerCount, 0, 0));
}

bool tailDrainPendingInternal() {
    return InterlockedCompareExchange(&g_spectatorTailDrainArmed, 0, 0) != 0
        && InterlockedCompareExchange(&g_spectatorTailDrainComplete, 0, 0) == 0;
}

static void serviceSpectatorTailDrain(uint64_t nowMs) {
    if (!tailDrainPendingInternal()) return;
    const LONG viewers =
        InterlockedCompareExchange(&g_spectatorSourceViewerCount, 0, 0);
    const uint64_t deadline = static_cast<uint64_t>(
        InterlockedCompareExchange64(&g_spectatorTailDrainDeadlineAt, 0, 0));
    if (viewers == 0) {
        InterlockedExchange(&g_spectatorTailDrainComplete, 1);
        QLOG("[spectator-source] sealed-tail drain COMPLETE: every viewer "
             "acknowledged the terminal and was retired");
        return;
    }
    if (nowMs >= deadline) {
        InterlockedExchange(&g_spectatorTailDrainComplete, 1);
        QLOG("[spectator-source] sealed-tail drain deadline expired with %ld "
             "viewer(s) still attached; they keep the prefix they received and "
             "will report an incomplete tail",
             viewers);
    }
}

using ReleasedTopologyRelaySocketBorrow =
    qclient::spectator::topology::client::ReleasedTopologyRelaySocketBorrow;

static bool sameReleasedTopologyRelayBorrow(
    const ReleasedTopologyRelaySocketBorrow& left,
    const ReleasedTopologyRelaySocketBorrow& right) {
    return left.socket == right.socket && left.generation == right.generation;
}

static ReleasedTopologyRelaySocketBorrow
currentReleasedTopologyRelayBorrow() {
    if (!g_releasedTopologyClient) return {};
    return g_releasedTopologyClient->borrowedRelayDataSocket();
}

static bool relayReadEventMatchesBorrow(
    const ReleasedTopologyRelaySocketBorrow& borrow) {
    return borrow.valid() && g_relayReadEvent != WSA_INVALID_EVENT
        && borrow.socket == g_relayReadEventSocket
        && borrow.generation == g_relayReadEventGeneration;
}

static void clearRelayReadEventAlias() {
    g_relayReadEvent = WSA_INVALID_EVENT;
    g_relayReadEventSocket = qclient::spectator::topology::client::
        kInvalidReleasedTopologyRelaySocket;
    g_relayReadEventGeneration = 0u;
}

static void unregisterReleasedTopologyRelayReadEvent() {
    if (g_relayReadEvent == WSA_INVALID_EVENT) {
        clearRelayReadEventAlias();
        return;
    }
    const ReleasedTopologyRelaySocketBorrow current =
        currentReleasedTopologyRelayBorrow();
    if (relayReadEventMatchesBorrow(current)) {
        const SOCKET socket = static_cast<SOCKET>(current.socket);
        (void)WSAEventSelect(socket, nullptr, 0);
        u_long nonblocking = 1;
        (void)ioctlsocket(socket, FIONBIO, &nonblocking);
    }
    WSACloseEvent(g_relayReadEvent);
    clearRelayReadEventAlias();
}

static bool synchronizeReleasedTopologyRelayReadEvent(
    const bool eventModeUsable) {
    if (!eventModeUsable) {
        unregisterReleasedTopologyRelayReadEvent();
        return false;
    }
    ReleasedTopologyRelaySocketBorrow borrow =
        currentReleasedTopologyRelayBorrow();
    if (!borrow.valid()) {
        unregisterReleasedTopologyRelayReadEvent();
        return false;
    }
    if (relayReadEventMatchesBorrow(borrow)) return true;

    unregisterReleasedTopologyRelayReadEvent();
    borrow = currentReleasedTopologyRelayBorrow();
    if (!borrow.valid()) return false;
    if (g_relayReadEventFailedGeneration == borrow.generation)
        return false;

    const WSAEVENT event = WSACreateEvent();
    if (event == WSA_INVALID_EVENT) {
        g_relayReadEventFailedGeneration = borrow.generation;
        QLOG("[spectator-topology] downstream QSP event create failed err=%d; "
             "bounded polling continues",
             WSAGetLastError());
        return false;
    }
    const SOCKET socket = static_cast<SOCKET>(borrow.socket);
    if (WSAEventSelect(socket, event, FD_READ) != 0) {
        const int error = WSAGetLastError();
        if (sameReleasedTopologyRelayBorrow(
                borrow, currentReleasedTopologyRelayBorrow())) {
            (void)WSAEventSelect(socket, nullptr, 0);
            u_long nonblocking = 1;
            (void)ioctlsocket(socket, FIONBIO, &nonblocking);
        }
        WSACloseEvent(event);
        g_relayReadEventFailedGeneration = borrow.generation;
        QLOG("[spectator-topology] downstream QSP event select failed err=%d; "
             "bounded polling continues",
             error);
        return false;
    }
    if (!sameReleasedTopologyRelayBorrow(
            borrow, currentReleasedTopologyRelayBorrow())) {
        WSACloseEvent(event);
        return false;
    }
    g_relayReadEvent = event;
    g_relayReadEventSocket = borrow.socket;
    g_relayReadEventGeneration = borrow.generation;
    g_relayReadEventFailedGeneration = 0u;
    return true;
}

static bool createReleasedTopologyRelaySocket(SOCKET& out) {
    out = INVALID_SOCKET;
    SOCKET candidate = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (candidate == INVALID_SOCKET) {
        QLOG("[spectator-topology] downstream QSP socket() failed err=%d; "
             "ordinary E0 viewing continues",
             WSAGetLastError());
        return false;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (::bind(candidate, reinterpret_cast<const sockaddr*>(&local),
               sizeof(local)) != 0
        || !makeNonblocking(candidate, "spectator-topology-relay")) {
        QLOG("[spectator-topology] downstream QSP port-0 bind/setup failed "
             "err=%d; ordinary E0 viewing continues",
             WSAGetLastError());
        ::closesocket(candidate);
        return false;
    }
    sockaddr_in bound{};
    if (!getLocalSocketAddress(candidate, bound, "spectator-topology-relay")) {
        ::closesocket(candidate);
        return false;
    }
    out = candidate;
    return true;
}

static void resetReleasedTopologyRelayPumpState() {
    g_releasedTopologyRelayBindingAttempted = false;
    g_releasedTopologyRelayServingLogged = false;
    g_releasedTopologyRelayUnavailableLogged = false;
    g_relayReadEventFailedGeneration = 0u;
}

static void serviceReleasedTopologyRelay(const uint64_t nowMs) {
    using qclient::spectator::topology::client::ReleasedTopologyRelayBindingState;
    using qclient::spectator::resilient::NodeRuntimeResult;

    if (!g_releasedTopologyClient) return;
    const ReleasedTopologyRelayBindingState bindingState =
        g_releasedTopologyClient->relayDataBindingState();
    if (bindingState == ReleasedTopologyRelayBindingState::Inactive) {
        if (g_releasedTopologyRelayBindingAttempted) return;
        g_releasedTopologyRelayBindingAttempted = true;
        SOCKET candidate = INVALID_SOCKET;
        if (!createReleasedTopologyRelaySocket(candidate)) return;
        uintptr_t ownedSocket = static_cast<uintptr_t>(candidate);
        const bool bindingStarted =
            g_releasedTopologyClient->beginRelayDataBinding(ownedSocket, nowMs);
        if (ownedSocket != qclient::spectator::topology::client::
                               kInvalidReleasedTopologyRelaySocket) {
            ::closesocket(static_cast<SOCKET>(ownedSocket));
        }
        if (bindingStarted) {
            g_releasedTopologyRelayUnavailableLogged = false;
            QLOG("[spectator-topology] downstream QSP binding requested from "
                 "one DPlay-created port-0 socket");
        } else {
            QLOG("[spectator-topology] downstream QSP binding was not armed; "
                 "ordinary E0 viewing continues");
        }
        return;
    }
    if (bindingState == ReleasedTopologyRelayBindingState::Pending) return;
    if (bindingState == ReleasedTopologyRelayBindingState::Unavailable) {
        if (!g_releasedTopologyRelayUnavailableLogged) {
            g_releasedTopologyRelayUnavailableLogged = true;
            QLOG("[spectator-topology] downstream QSP binding unavailable; "
                 "ordinary E0 viewing continues");
        }
        return;
    }
    if (bindingState != ReleasedTopologyRelayBindingState::Ready) return;

    const ReleasedTopologyRelaySocketBorrow borrow =
        currentReleasedTopologyRelayBorrow();
    if (!borrow.valid()) {
        QLOG("[spectator-topology] ready downstream QSP binding had no valid "
             "borrow; retiring optional relay serving");
        g_releasedTopologyClient->stopRelayNode();
        return;
    }

    bool nodeReady = false;
    if (g_specCsInit && g_spectatorViewer) {
        EnterCriticalSection(&g_specCs);
        if (g_releasedTopologyClient->relayNodeRunning()) {
            nodeReady = g_releasedTopologyClient->projectRelayViewer(
                *g_spectatorViewer, nowMs) == NodeRuntimeResult::Accepted;
        } else {
            nodeReady = g_releasedTopologyClient->startRelayNode(
                *g_spectatorViewer, nowMs);
        }
        LeaveCriticalSection(&g_specCs);
    }
    if (!nodeReady) {
        QLOG("[spectator-topology] downstream QSP node projection rejected; "
             "retiring optional relay serving");
        g_releasedTopologyClient->stopRelayNode();
        return;
    }
    if (!sameReleasedTopologyRelayBorrow(
            borrow, currentReleasedTopologyRelayBorrow())) {
        return;
    }
    if (!g_releasedTopologyRelayServingLogged) {
        g_releasedTopologyRelayServingLogged = true;
        QLOG("[spectator-topology] downstream QSP relay node serving from "
             "host-observed binding");
    }

    const auto outbound = g_releasedTopologyClient->pollRelayChildDatagrams(
        nowMs, transport_detail::kModernSpectatorIngressBudget);
    if (!sameReleasedTopologyRelayBorrow(
            borrow, currentReleasedTopologyRelayBorrow())) {
        return;
    }
    const SOCKET socket = static_cast<SOCKET>(borrow.socket);
    for (const auto& packet : outbound) {
        if (!sameReleasedTopologyRelayBorrow(
                borrow, currentReleasedTopologyRelayBorrow())) {
            break;
        }
        if (!packet.valid()) continue;
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = packet.destination.ipv4BE;
        destination.sin_port = htons(packet.destination.port);
        (void)sendDatagramRaw(
            socket, destination, packet.bytes, "spectator-topology-relay");
    }
}

static void serviceReleasedTopologyParentRouteActivation(const uint64_t nowMs) {
    using qclient::spectator::topology::client::
        ReleasedTopologyParentRouteActivation;

    if (!g_releasedTopologyClient) return;

    ReleasedTopologyParentRouteActivation activation{};
    const bool haveProjection =
        g_releasedTopologyClient->parentRouteActivation(activation, nowMs)
        && activation.valid();

    if (g_spectatorParentRouteArmedActivationId != 0u
        && (!haveProjection
            || activation.activationNonce
                   != g_spectatorParentRouteArmedActivationId)) {
        const uint64_t retiredActivationId =
            g_spectatorParentRouteArmedActivationId;
        g_spectatorParentRouteArmedActivationId = 0u;
        (void)spectatorViewerCancelValidatedParentRouteTransition(
            retiredActivationId);
    }

    if (!haveProjection
        || g_spectatorParentRouteArmedActivationId != 0u) {
        return;
    }

    transport_detail::SpectatorParentRouteTransitionAuthorization
        authorization{};
    // RouteEndpoint keeps addresses in network order but its port in host order; only the port is converted for sockaddr use.
    authorization.currentParentIpv4BE =
        activation.currentParentEndpoint.ipv4BE;
    authorization.currentParentPortBE =
        htons(activation.currentParentEndpoint.port);
    authorization.candidateIpv4BE = activation.candidateDataEndpoint.ipv4BE;
    authorization.candidatePortBE = htons(activation.candidateDataEndpoint.port);
    authorization.sourceGeneration = activation.sourceGeneration;
    authorization.routeGeneration = activation.routeGeneration;
    authorization.nextRecord = activation.commonCursor.nextRecord;
    authorization.nextEvent = activation.commonCursor.nextEvent;
    authorization.digest = activation.commonCursor.digest;
    authorization.localDeadlineMs = activation.localDeadlineMs;
    authorization.activationId = activation.activationNonce;

    if (spectatorViewerArmValidatedParentRouteTransition(authorization)) {
        g_spectatorParentRouteArmedActivationId = activation.activationNonce;
    }
}

static void retireReleasedTopologyClient() {
    const uint64_t retiredActivationId =
        g_spectatorParentRouteArmedActivationId;
    g_spectatorParentRouteArmedActivationId = 0u;
    if (retiredActivationId != 0u) {
        (void)spectatorViewerCancelValidatedParentRouteTransition(
            retiredActivationId);
    }
    unregisterReleasedTopologyRelayReadEvent();
    if (g_releasedTopologyClient) {
        g_releasedTopologyClient->stopRelayNode();
        g_releasedTopologyClient->stop();
        g_releasedTopologyClient.reset();
    }
    resetReleasedTopologyRelayPumpState();
}

static void serviceSpectatorOutbound(uint64_t nowMs) {
    bool parentRouteExpired = false;
    uint64_t expiredParentRouteActivationId = 0u;
    if (g_spectatorParentRouteGateInitialized && g_specCsInit) {
        EnterCriticalSection(&g_specCs);
        parentRouteExpired = g_spectatorParentRouteGate.expire(nowMs);
        if (parentRouteExpired) {
            expiredParentRouteActivationId =
                g_spectatorParentRouteArmedActivationId;
            g_spectatorParentRouteArmedActivationId = 0u;
            if (g_spectatorViewer
                && expiredParentRouteActivationId != 0u) {
                (void)g_spectatorViewer
                    ->cancelValidatedParentRouteTransition(
                        expiredParentRouteActivationId);
            }
        }
        LeaveCriticalSection(&g_specCs);
    }
    if (parentRouteExpired) {
        QLOG("[spectator-topology] parent-route activation expired; retaining "
             "the existing QSP source");
    }
    flushTestNetworkImpairment(
        TestNetworkDomain::SpectatorSource, nowMs);
    flushTestNetworkImpairment(
        TestNetworkDomain::SpectatorViewer, nowMs);
    serviceSpectatorRelayRegistration(nowMs);
    if (g_spectatorSource) {
        std::vector<qnetplay::spectator::SourceDatagram> packets;
        const size_t packetBudget = spectatorSourcePacketBudget(nowMs);
        if (packetBudget != 0)
            packets = g_spectatorSource->pollOutbound(nowMs, packetBudget);
        const uint64_t consumed =
            static_cast<uint64_t>(packets.size()) * kSpectatorTokenScale;
        g_spectatorPacketTokens = consumed <= g_spectatorPacketTokens
            ? g_spectatorPacketTokens - consumed
            : 0;
        InterlockedExchange(
            &g_spectatorSourceViewerCount,
            static_cast<LONG>(g_spectatorSource->viewerCount()));
        for (const auto& packet : packets) {
            const sockaddr_in destination =
                spectatorViewerAddress(packet.destination);
            sendDatagramOn(
                g_spectatorUdp, destination, packet.bytes,
                TestNetworkDomain::SpectatorSource,
                "spectator-source");
        }
    }

    std::vector<qnetplay::spectator::ViewerDatagram> viewerPackets;
    if (g_spectatorViewer && g_specCsInit) {
        EnterCriticalSection(&g_specCs);
        viewerPackets = g_spectatorViewer->pollOutbound(nowMs);
        LeaveCriticalSection(&g_specCs);
    }
    for (const auto& packet : viewerPackets) {
        sockaddr_in destination = g_peer;
        if (packet.route
            == qnetplay::spectator::ViewerDatagramRoute::
                   ValidatedPendingParent) {
            bool pendingRouteCurrent = false;
            if (g_spectatorParentRouteGateInitialized && g_specCsInit) {
                EnterCriticalSection(&g_specCs);
                pendingRouteCurrent =
                    packet.parentRouteActivationId != 0u
                    && packet.parentRouteActivationId
                           == g_spectatorParentRouteArmedActivationId
                    && g_spectatorParentRouteGate.pendingEndpoint(
                        destination);
                LeaveCriticalSection(&g_specCs);
            }
            if (!pendingRouteCurrent) {
                static uint64_t staleCandidateHelloDrops = 0;
                if (++staleCandidateHelloDrops <= 8
                    || staleCandidateHelloDrops % 300 == 0) {
                    QLOG("[spectator-topology] DROP stale candidate Hello "
                         "#%llu",
                         static_cast<unsigned long long>(
                             staleCandidateHelloDrops));
                }
                continue;
            }
        } else if (packet.route
                   != qnetplay::spectator::ViewerDatagramRoute::
                          ActiveParent) {
            continue;
        }
        const std::vector<uint8_t>* wire = &packet.bytes;
        std::vector<uint8_t> framed;
        if (g_spectatorRelayViewer.enabled()) {
            if (!g_spectatorRelayViewer.encodeOutbound(
                    packet.bytes.data(), packet.bytes.size(), framed)) {
                static uint64_t relayEncodeDrops = 0;
                if (++relayEncodeDrops <= 8
                    || relayEncodeDrops % 300 == 0) {
                    QLOG("[spectator-relay] DROP outbound QSP frame #%llu "
                         "size=%zu",
                         static_cast<unsigned long long>(relayEncodeDrops),
                         packet.bytes.size());
                }
                continue;
            }
            wire = &framed;
        }
        const bool acceptedForSend = sendDatagramOn(
            g_udp, destination, *wire,
            TestNetworkDomain::SpectatorViewer,
            "spectator-viewer");
        const bool relayedTopologyPermitted =
            g_cfg.spectatorTopologyEnabled == 1u
            && g_cfg.spectatorTopologyRouteKind
                   == qipc::kSpectatorTopologyRouteRelay
            && g_cfg.spectatorRelayActive == 1u;
        if (acceptedForSend
            && (!g_spectatorRelayViewer.enabled()
                || relayedTopologyPermitted)
            && packet.type == qnetplay::spectator::MessageType::InitAck) {
            armReleasedTopologyClientAfterInitAck(nowMs);
        }
    }
    if (g_releasedTopologyClient) {
        const auto priorTopologyStatus = g_releasedTopologyClient->status();
        if (priorTopologyStatus.state
            == qclient::spectator::topology::client::
                ReleasedTopologyClientState::Unavailable) {
            QLOG("[spectator-topology] released topology leaf retired: %s; "
                 "ordinary E0 viewing continues", priorTopologyStatus.reason);
            retireReleasedTopologyClient();
            g_releasedTopologyClientAttachedLogged = false;
        }
    }
    if (g_releasedTopologyClient) {
        uint32_t nextRecord = 0u;
        uint32_t nextEvent = 0u;
        uint64_t digest = 0u;
        qnetplay::spectator::ViewerPhase phase =
            qnetplay::spectator::ViewerPhase::Idle;
        if (!releasedTopologyClientQspFrontier(nextRecord, nextEvent, digest,
                                               phase)
            || releasedTopologyE0Terminal(phase)) {
            retireReleasedTopologyClient();
            QLOG("[spectator-topology] ordinary E0 terminal state cleared topology leaf");
        } else if (!g_releasedTopologyClient->updateVerifiedQspFrontier(
                       nextRecord, nextEvent, digest)) {
            retireReleasedTopologyClient();
            QLOG("[spectator-topology] rejected non-monotonic QSP frontier; E0 continues");
        } else {
            g_releasedTopologyClient->service(nowMs);
            serviceReleasedTopologyParentRouteActivation(nowMs);
            const auto topologyStatus = g_releasedTopologyClient->status();
            if (topologyStatus.state
                == qclient::spectator::topology::client::
                    ReleasedTopologyClientState::Unavailable) {
                QLOG("[spectator-topology] released topology leaf retired: %s; "
                     "ordinary E0 viewing continues", topologyStatus.reason);
                retireReleasedTopologyClient();
                g_releasedTopologyClientAttachedLogged = false;
            } else {
                if (!g_releasedTopologyClientAttachedLogged
                    && topologyStatus.state
                        == qclient::spectator::topology::client::
                            ReleasedTopologyClientState::Attached) {
                    g_releasedTopologyClientAttachedLogged = true;
                    QLOG("[spectator-topology] released topology leaf attached; "
                         "ordinary E0 viewing continues");
                }
                if (topologyStatus.state
                    == qclient::spectator::topology::client::
                        ReleasedTopologyClientState::Attached) {
                    serviceReleasedTopologyRelay(nowMs);
                }
            }
        }
    }
}

static void handleSpectatorDatagram(const sockaddr_in& from,
                                    const uint8_t* bytes,
                                    size_t length,
                                    uint64_t nowMs) {
    const bool spectatorMagic =
        datagramMagic(bytes, length)
            == qnetplay::spectator::kSpectatorMagic;
    if (!transport_detail::modernDatagramMatchesDomain(
            transport_detail::ModernSocketDomain::Spectator,
            spectatorMagic)
        || length < qnetplay::spectator::kSpectatorHeaderSize
        || length > qnetplay::spectator::kMaxSpectatorDatagramSize) {
        static uint64_t crossDomainDrops = 0;
        if (++crossDomainDrops <= 8
            || crossDomainDrops % 300 == 0) {
            QLOG("[spectator] DROP non-spectator/malformed datagram #%llu "
                 "size=%zu from=%s",
                 static_cast<unsigned long long>(crossDomainDrops),
                 length, epStr(from).c_str());
        }
        return;
    }

    if (g_spectatorSource) {
        const auto result = g_spectatorSource->onDatagram(
            spectatorViewerId(from), bytes, length, nowMs);
        InterlockedExchange(
            &g_spectatorSourceViewerCount,
            static_cast<LONG>(g_spectatorSource->viewerCount()));
        if (result != qnetplay::spectator::IngressResult::Accepted) {
            static uint64_t drops = 0;
            if (++drops <= 8 || drops % 300 == 0) {
                QLOG("[spectator-source] DROP #%llu result=%u size=%zu from=%s",
                     static_cast<unsigned long long>(drops),
                     static_cast<unsigned>(result), length,
                     epStr(from).c_str());
            }
        }
        return;
    }

    if (g_spectatorViewer && g_specCsInit) {
        EnterCriticalSection(&g_specCs);
        const auto ingress = g_spectatorParentRouteGateInitialized
            ? g_spectatorParentRouteGate.classify(from, nowMs)
            : (sameEndpoint(from, g_peer)
                   ? transport_detail::SpectatorParentRouteIngress::Active
                   : transport_detail::SpectatorParentRouteIngress::Rejected);
        if (ingress
            == transport_detail::SpectatorParentRouteIngress::Rejected) {
            LeaveCriticalSection(&g_specCs);
            static uint64_t wrongSources = 0;
            if (++wrongSources <= 8 || wrongSources % 300 == 0) {
                QLOG("[spectator-viewer] DROP wrong source #%llu from=%s expected=%s",
                     static_cast<unsigned long long>(wrongSources),
                     epStr(from).c_str(), epStr(g_peer).c_str());
            }
            return;
        }
        const uint64_t pendingActivationId =
            g_spectatorParentRouteArmedActivationId;
        const auto result = ingress
                == transport_detail::SpectatorParentRouteIngress::Pending
            ? (pendingActivationId == 0u
                   ? qnetplay::spectator::IngressResult::UnexpectedMessage
                   : g_spectatorViewer->onValidatedParentRouteDatagram(
                         pendingActivationId, bytes, length, nowMs))
            : g_spectatorViewer->onDatagram(bytes, length, nowMs);
        bool parentRouteCommitted = false;
        bool parentRouteCompletionRejected = false;
        bool parentRouteCommitAborted = false;
        if (result == qnetplay::spectator::IngressResult::Accepted) {
            if (ingress
                == transport_detail::SpectatorParentRouteIngress::Active) {
                g_spectatorLastValidMs = nowMs;
            } else if (pendingActivationId == 0u
                       || !g_spectatorParentRouteGate
                               .preparePendingAfterAcceptedIngress(
                                   from, nowMs)) {
                parentRouteCommitAborted = true;
                (void)g_spectatorViewer
                    ->cancelValidatedParentRouteTransition(
                        pendingActivationId);
                (void)g_spectatorParentRouteGate
                    .cancelPendingActivation(pendingActivationId);
                g_spectatorParentRouteArmedActivationId = 0u;
            } else if (!g_spectatorViewer
                            ->commitValidatedParentRouteIngress(
                                pendingActivationId, nowMs)) {
                parentRouteCommitAborted = true;
                (void)g_spectatorViewer
                    ->cancelValidatedParentRouteTransition(
                        pendingActivationId);
                (void)g_spectatorParentRouteGate
                    .cancelPendingActivation(pendingActivationId);
                g_spectatorParentRouteArmedActivationId = 0u;
            } else {
                g_spectatorParentRouteGate.commitPreparedPendingIngress(from);
                g_peer = from;
                InterlockedExchange(&g_peerKnown, 1);
                parentRouteCommitted = true;
                g_spectatorLastValidMs = nowMs;
                const qclient::spectator::RouteEndpoint observedParent{
                    from.sin_addr.s_addr, ntohs(from.sin_port)};
                parentRouteCompletionRejected =
                    g_releasedTopologyClient == nullptr
                    || g_spectatorParentRouteArmedActivationId == 0u
                    || !g_releasedTopologyClient
                           ->completeParentRouteActivationAfterIngress(
                               g_spectatorParentRouteArmedActivationId,
                               observedParent,
                               g_spectatorViewer->receivedNextRecord(),
                               g_spectatorViewer->receivedNextEvent(),
                               g_spectatorViewer->receivedDigest(), nowMs);
            }
        }
        LeaveCriticalSection(&g_specCs);
        if (parentRouteCommitAborted) {
            QLOG("[spectator-topology] candidate ingress could not complete "
                 "the local transaction; retaining the existing QSP source");
            retireReleasedTopologyClient();
            return;
        }
        if (parentRouteCompletionRejected) {
            QLOG("[spectator-topology] candidate QSP ingress committed but "
                 "paired topology completion was rejected; retiring topology "
                 "only");
            retireReleasedTopologyClient();
            return;
        }
        if (parentRouteCommitted) {
            QLOG("[spectator-topology] parent-route transition committed "
                 "after accepted candidate QSP ingress");
        }
        if (result != qnetplay::spectator::IngressResult::Accepted) {
            static uint64_t drops = 0;
            if (++drops <= 8 || drops % 300 == 0) {
                QLOG("[spectator-viewer] DROP #%llu result=%u size=%zu",
                     static_cast<unsigned long long>(drops),
                     static_cast<unsigned>(result), length);
            }
        }
    }
}

static void handlePlayerDatagram(const sockaddr_in& from,
                                 const uint8_t* bytes,
                                 size_t length,
                                 uint64_t nowMs) {
    const bool spectatorMagic =
        datagramMagic(bytes, length)
            == qnetplay::spectator::kSpectatorMagic;
    if (!transport_detail::modernDatagramMatchesDomain(
            transport_detail::ModernSocketDomain::Player,
            spectatorMagic)) {
        InterlockedIncrement(&g_statCrossDomainDrops);
        static uint64_t crossDomainDrops = 0;
        if (++crossDomainDrops <= 8
            || crossDomainDrops % 300 == 0) {
            QLOG("[netlink] DROP spectator datagram on player socket #%llu "
                 "size=%zu from=%s",
                 static_cast<unsigned long long>(crossDomainDrops),
                 length, epStr(from).c_str());
        }
        return;
    }

    if (g_spectatorViewerMode || !g_playerCore || !g_netCsInit
        || !sameEndpoint(from, g_peer)) {
        if (!g_spectatorViewerMode && g_playerCore && g_netCsInit)
            InterlockedIncrement(&g_statWrongSourceDrops);
        return;
    }

    if (length >= qnetplay::wire::kHeaderSize
        && (bytes[6] == static_cast<uint8_t>(
                qnetplay::wire::MessageType::RouteProbe)
            || bytes[6] == static_cast<uint8_t>(
                qnetplay::wire::MessageType::RouteProbeAck))) {
        namespace wire = qnetplay::wire;
        const int64_t recvUs =
            qnet::ticksToMicros(realQpcTicks(), g_pumpQpcFreq);
        wire::Datagram datagram{};
        if (wire::decodeDatagram(
                bytes, length, g_cfg.sessionId,
                wire::SessionCookie{g_cfg.cookieLow, g_cfg.cookieHigh},
                datagram) != wire::DecodeError::None) {
            ++g_routeProbeDrops;
            return;
        }
        const wire::PeerRole localRole = g_cfg.isHost
            ? wire::PeerRole::Host
            : wire::PeerRole::Client;
        if (datagram.header.type == wire::MessageType::RouteProbe) {
            wire::RouteProbePayload probe{};
            if (!wire::decodePayload(datagram.payload, probe)
                || probe.role == localRole) {
                ++g_routeProbeDrops;
                return;
            }
            const int64_t sendUs =
                qnet::ticksToMicros(realQpcTicks(), g_pumpQpcFreq);
            if (g_routeAckRefillUs == 0) g_routeAckRefillUs = sendUs;
            const int64_t refill = (sendUs - g_routeAckRefillUs) / 250;
            g_routeAckTokens = g_routeAckTokens + refill > 16
                ? 16 : g_routeAckTokens + refill;
            g_routeAckRefillUs += refill * 250;
            if (g_routeAckTokens <= 0) {
                ++g_routeProbeDrops;
                return;
            }
            --g_routeAckTokens;
            wire::RouteProbeAckPayload ack{};
            ack.sequence = probe.sequence;
            ack.generation = probe.generation;
            ack.echoStampMicros = probe.stampMicros;
            const int64_t dwell = sendUs - recvUs;
            ack.dwellMicros = dwell > 0
                ? static_cast<uint32_t>(dwell < 1000000 ? dwell : 1000000)
                : 0u;
            ack.routeId = probe.routeId;
            ack.role = localRole;
            std::vector<uint8_t> payload, packet;
            if (wire::encodePayload(ack, payload)
                && wire::encodeDatagram(
                       wire::MessageType::RouteProbeAck, g_cfg.sessionId,
                       wire::SessionCookie{g_cfg.cookieLow, g_cfg.cookieHigh},
                       g_routeHeaderSeq++, payload, packet)) {
                sendDatagramOn(g_udp, g_peer, packet,
                               TestNetworkDomain::Player, "netlink");
                ++g_routeProbesAnswered;
            }
            return;
        }
        wire::RouteProbeAckPayload ack{};
        if (!wire::decodePayload(datagram.payload, ack)
            || ack.role == localRole) {
            ++g_routeProbeDrops;
            return;
        }
        g_routeEstimator.onReply(
            ack.sequence, static_cast<int64_t>(ack.echoStampMicros),
            ack.generation, recvUs, static_cast<int64_t>(ack.dwellMicros));
        if (!g_routeWindowStarted) return;
        if (g_routeEstimator.counts().uniqueReplies >= 8) {
            InterlockedExchange(
                &g_statRouteP50Ms,
                static_cast<LONG>(qnet::microsToMillisRounded(
                    g_routeEstimator.p50Micros())));
        }
        {
            const auto& window = g_routeEstimator.rtt();
            if (!window.empty()) {
                InterlockedExchange(&g_statPingLastUs,
                                    static_cast<LONG>(window.newest()));
                InterlockedExchange(&g_statPingMeanUs,
                                    static_cast<LONG>(window.mean()));
                InterlockedExchange(&g_statPingMinUs,
                                    static_cast<LONG>(window.minimum()));
                InterlockedExchange(&g_statPingMaxUs,
                                    static_cast<LONG>(window.maximum()));
                InterlockedExchange(
                    &g_statPingP95Us,
                    static_cast<LONG>(g_routeEstimator.p95Micros()));
                InterlockedExchange(
                    &g_statPingJitterUs,
                    static_cast<LONG>(g_routeEstimator.jitterMicros()));
                InterlockedExchange(
                    &g_statPingLossPerMille,
                    static_cast<LONG>(g_routeEstimator.lossPerMille()));
                InterlockedExchange(
                    &g_statPingSamples,
                    static_cast<LONG>(window.count()));
            }
        }
        return;
    }

    qnetplay::IngressResult result{};
    EnterCriticalSection(&g_netCs);
    result = g_playerCore->onDatagram(
        coreEndpoint(from), bytes, length, nowMs);
    LeaveCriticalSection(&g_netCs);
    const size_t resultIndex = static_cast<size_t>(result);
    if (resultIndex
        < static_cast<size_t>(qnetplay::IngressResult::Count)) {
        InterlockedIncrement(&g_statIngressResults[resultIndex]);
    }
    InterlockedExchange(
        &g_statLastIngressResult,
        static_cast<LONG>(static_cast<unsigned>(result)));
    InterlockedExchange(
        &g_statLastIngressTick,
        static_cast<LONG>(static_cast<DWORD>(nowMs)));
    if (result == qnetplay::IngressResult::Accepted) {
        // Message type is at byte six in the explicitly encoded v4 header.
        if (length > 6
            && bytes[6] == static_cast<uint8_t>(
                qnetplay::wire::MessageType::Inputs)) {
            InterlockedIncrement(&g_statRxInputs);
            if (!g_routeWindowStarted) {
                g_routeWindowStarted = true;
                const auto before = g_routeEstimator.counts();
                g_routeEstimator.reset();
                g_routeLastProbeUs = 0;
                InterlockedExchange(&g_statPingMeanUs, -1);
                InterlockedExchange(&g_statPingLastUs, -1);
                InterlockedExchange(&g_statPingMinUs, -1);
                InterlockedExchange(&g_statPingMaxUs, -1);
                InterlockedExchange(&g_statPingP95Us, -1);
                InterlockedExchange(&g_statPingJitterUs, 0);
                InterlockedExchange(&g_statPingLossPerMille, 0);
                InterlockedExchange(&g_statPingSamples, 0);
                QLOG("[route-sample] window started at the peer's first input "
                     "(discarded %llu boot-window sample(s); the peer's "
                     "process was not servicing its socket yet)",
                     static_cast<unsigned long long>(before.uniqueReplies));
            }
        }
        return;
    }

    static uint64_t playerDrops = 0;
    if (++playerDrops <= 8 || playerDrops % 300 == 0) {
        QLOG("[netlink] DROP #%llu result=%s(%u) size=%zu from=%s",
             static_cast<unsigned long long>(playerDrops),
             qnetplay::ingressResultName(result),
             static_cast<unsigned>(result), length,
             epStr(from).c_str());
    }
}

struct SocketDrainResult {
    size_t datagrams = 0;
    bool saturated = false;
};

static SocketDrainResult drainModernSocket(
    SOCKET socket,
    transport_detail::ModernSocketDomain domain,
    size_t budget,
    std::vector<uint8_t>& buffer) {
    SocketDrainResult result{};
    if (socket == INVALID_SOCKET || budget == 0)
        return result;

    for (size_t turn = 0; turn < budget; ++turn) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int received = ::recvfrom(
            socket, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (received < 0) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                InterlockedIncrement(&g_statReceiveErrors);
                InterlockedExchange(&g_statLastSocketError, error);
            }
            if (transport_detail::modernRecvErrorEndsTurn(error))
                break;

            ++result.datagrams;
            if (error != WSAEMSGSIZE
                && InterlockedCompareExchange(
                       &g_running, 0, 0) != 0) {
                QLOG("[%s] recvfrom error %d",
                     domain
                             == transport_detail::ModernSocketDomain::Player
                         ? "netlink"
                         : "spectator",
                     error);
                break;
            }
            continue;
        }

        ++result.datagrams;
        if (received == 0 || fromLen != sizeof(sockaddr_in)
            || from.sin_family != AF_INET) {
            continue;
        }

        const size_t length = static_cast<size_t>(received);
        if (domain
            == transport_detail::ModernSocketDomain::Player) {
            handlePlayerDatagram(
                from, buffer.data(), length, GetTickCount64());
        } else {
            if (g_spectatorViewerMode
                && g_spectatorRelayViewer.enabled()) {
                std::vector<uint8_t> inner;
                const auto relayIngress =
                    g_spectatorRelayViewer.decodeInbound(
                        sameEndpoint(from, g_peer),
                        buffer.data(), length, inner);
                if (relayIngress
                    == qnetplay::spectator::RelayViewerIngress::Data) {
                    handleSpectatorDatagram(
                        from, inner.data(), inner.size(),
                        GetTickCount64());
                    continue;
                }
                if (relayIngress
                    == qnetplay::spectator::RelayViewerIngress::Ready) {
                    continue;
                }
                if (relayIngress
                    == qnetplay::spectator::RelayViewerIngress::Detached) {
                    static uint64_t relayDetaches = 0;
                    bool terminalLatched = false;
                    if (g_specCsInit && g_spectatorViewer) {
                        EnterCriticalSection(&g_specCs);
                        qnetplay::spectator::ArchiveTerminal terminal{};
                        terminalLatched =
                            g_spectatorViewer->terminal(terminal);
                        (void)g_spectatorViewer->reportTransportFailure(
                            qnetplay::spectator::IngressResult::RouteLost);
                        LeaveCriticalSection(&g_specCs);
                    }
                    if (++relayDetaches <= 4) {
                        QLOG("[spectator-relay] relay detached this viewer "
                             "(generation=%u terminalLatched=%d)",
                             g_cfg.spectatorRelaySourceGeneration,
                             terminalLatched ? 1 : 0);
                    }
                    continue;
                }
                if (relayIngress
                    == qnetplay::spectator::RelayViewerIngress::Rejected) {
                    static uint64_t relayEnvelopeDrops = 0;
                    if (++relayEnvelopeDrops <= 8
                        || relayEnvelopeDrops % 300 == 0) {
                        QLOG("[spectator-relay] DROP malformed/foreign relay "
                             "record #%llu size=%zu from=%s",
                             static_cast<unsigned long long>(
                                 relayEnvelopeDrops),
                             length, epStr(from).c_str());
                    }
                    continue;
                }
            }
            handleSpectatorDatagram(
                from, buffer.data(), length, GetTickCount64());
        }
    }
    result.saturated = result.datagrams == budget;
    return result;
}

static SocketDrainResult drainReleasedTopologyRelaySocket(
    const size_t budget, std::vector<uint8_t>& buffer) {
    SocketDrainResult result{};
    if (budget == 0u || !g_releasedTopologyClient
        || !g_releasedTopologyClient->relayNodeRunning()) {
        return result;
    }
    const ReleasedTopologyRelaySocketBorrow borrow =
        currentReleasedTopologyRelayBorrow();
    if (!borrow.valid()) return result;
    const SOCKET socket = static_cast<SOCKET>(borrow.socket);

    for (size_t turn = 0u; turn < budget; ++turn) {
        if (!sameReleasedTopologyRelayBorrow(
                borrow, currentReleasedTopologyRelayBorrow())) {
            break;
        }
        sockaddr_in from{};
        int fromLength = sizeof(from);
        const int received = ::recvfrom(
            socket, reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received < 0) {
            const int error = WSAGetLastError();
            if (transport_detail::modernRecvErrorEndsTurn(error)) break;
            ++result.datagrams;
            if (error != WSAEMSGSIZE
                && InterlockedCompareExchange(&g_running, 0, 0) != 0) {
                static uint64_t relayReceiveErrors = 0u;
                if (++relayReceiveErrors <= 8u
                    || relayReceiveErrors % 300u == 0u) {
                    QLOG("[spectator-topology] downstream QSP recvfrom error "
                         "%d", error);
                }
                break;
            }
            continue;
        }

        ++result.datagrams;
        if (received == 0 || fromLength != sizeof(sockaddr_in)
            || from.sin_family != AF_INET) {
            continue;
        }
        const qclient::spectator::RouteEndpoint observedSource{
            from.sin_addr.s_addr,
            ntohs(from.sin_port),
        };
        const auto ingress = g_releasedTopologyClient->ingestRelayChildDatagram(
            observedSource, buffer.data(), static_cast<size_t>(received),
            GetTickCount64());
        if (!sameReleasedTopologyRelayBorrow(
                borrow, currentReleasedTopologyRelayBorrow())) {
            break;
        }
        if (ingress
            != qclient::spectator::resilient::NodeRuntimeResult::Accepted) {
            static uint64_t relayChildRejects = 0u;
            if (++relayChildRejects <= 8u
                || relayChildRejects % 300u == 0u) {
                QLOG("[spectator-topology] DROP downstream QSP packet #%llu "
                     "size=%d result=%u",
                     static_cast<unsigned long long>(relayChildRejects),
                     received,
                     static_cast<unsigned>(ingress));
            }
        }
    }
    result.saturated = result.datagrams == budget;
    return result;
}

static void emitRouteTelemetry() {
    QLOG("[route] enqueueToSendUs p50=%lld p95=%lld max=%lld n=%u | "
         "localDwellUs p50=%lld n=%u | wakeReq=%ld earlyWake=%ld | rttMs=%ld",
         static_cast<long long>(g_enqueueToSendUs.percentile(50)),
         static_cast<long long>(g_enqueueToSendUs.percentile(95)),
         static_cast<long long>(g_enqueueToSendUs.maximum()),
         static_cast<unsigned>(g_enqueueToSendUs.count()),
         static_cast<long long>(g_localDwellUs.percentile(50)),
         static_cast<unsigned>(g_localDwellUs.count()),
         InterlockedCompareExchange(&g_pumpWakeRequests, 0, 0),
         InterlockedCompareExchange(&g_pumpEarlyWakes, 0, 0),
         InterlockedCompareExchange(&g_statRttMs, 0, 0));
    NetTransportDiagnostics threadHealth{};
    (void)netGetTransportDiagnostics(threadHealth);
    QLOG("[game-thread-health-v1] stage=%s(%u) frame=%u ageMs=%llu "
         "counts=[driver:%u drain:%u scheduler:%u nativeIn:%u presentWait:%u "
         "nativeOut:%u commit:%u present:%u] "
         "lastValidRxAgeMs=%llu lastInputAgeMs=%llu ingress=%u/%u "
         "last=%u/%s(%u)/%llu "
         "socketErrors=%u/%u/%d",
         netGameThreadStageName(threadHealth.gameThreadStage),
         static_cast<unsigned>(threadHealth.gameThreadStage),
         threadHealth.gameThreadFrame,
         static_cast<unsigned long long>(
             threadHealth.gameThreadStageAgeMs),
         threadHealth.gameThreadStageCounts[
             static_cast<size_t>(
                 NetTransportDiagnostics::GameThreadStage::DriverEnter)],
         threadHealth.gameThreadStageCounts[
             static_cast<size_t>(
                 NetTransportDiagnostics::GameThreadStage::TransportDrain)],
         threadHealth.gameThreadStageCounts[
             static_cast<size_t>(
                 NetTransportDiagnostics::GameThreadStage::Scheduler)],
         threadHealth.gameThreadStageCounts[
             static_cast<size_t>(
                 NetTransportDiagnostics::GameThreadStage::NativeEnter)],
         threadHealth.gameThreadStageCounts[
             static_cast<size_t>(
                 NetTransportDiagnostics::GameThreadStage::PresentationWait)],
         threadHealth.gameThreadStageCounts[
             static_cast<size_t>(
                 NetTransportDiagnostics::GameThreadStage::NativeReturn)],
         threadHealth.gameThreadStageCounts[
             static_cast<size_t>(
                 NetTransportDiagnostics::GameThreadStage::CanonicalCommit)],
         threadHealth.gameThreadStageCounts[
             static_cast<size_t>(
                 NetTransportDiagnostics::GameThreadStage::
                     PresentationCommit)],
         static_cast<unsigned long long>(
             threadHealth.core.lastValidReceiveAgeMs),
         static_cast<unsigned long long>(
             threadHealth.core.lastNewRemoteInputAgeMs),
         threadHealth.ingressAccepted,
         threadHealth.ingressRejected,
         threadHealth.hasLastIngress ? 1u : 0u,
         qnetplay::ingressResultName(threadHealth.lastIngress),
         static_cast<unsigned>(threadHealth.lastIngress),
         static_cast<unsigned long long>(
             threadHealth.lastIngressAgeMs),
         threadHealth.receiveErrors,
         threadHealth.sendFailures,
         threadHealth.lastSocketError);
    if (g_playerCore && !g_spectatorViewerMode) {
        const auto counts = g_routeEstimator.counts();
        QLOG("[route-sample] label=%s p50us=%lld p95us=%lld p99us=%lld "
             "jitterus=%lld lossPermille=%u samples=%llu sent=%llu "
             "answered=%llu drops=%llu localStalls=%llu | prelaunch p50us=%u "
             "p95us=%u jitterus=%u lossPermille=%u samples=%u",
             routeLabelTextRt(g_cfg.routeLabel),
             static_cast<long long>(g_routeEstimator.p50Micros()),
             static_cast<long long>(g_routeEstimator.p95Micros()),
             static_cast<long long>(g_routeEstimator.p99Micros()),
             static_cast<long long>(g_routeEstimator.jitterMicros()),
             g_routeEstimator.lossPerMille(),
             static_cast<unsigned long long>(counts.uniqueReplies),
             static_cast<unsigned long long>(counts.sent),
             static_cast<unsigned long long>(g_routeProbesAnswered),
             static_cast<unsigned long long>(g_routeProbeDrops),
             static_cast<unsigned long long>(counts.locallyDelayed),
             g_cfg.prelaunchRouteP50Us, g_cfg.prelaunchRouteP95Us,
             g_cfg.prelaunchRouteJitterUs,
             static_cast<unsigned>(g_cfg.prelaunchRouteLossPerMille),
             static_cast<unsigned>(g_cfg.prelaunchRouteSamples));
        if (g_routeEstimator.counts().uniqueReplies >= 32) {
            const uint32_t p95Ms = static_cast<uint32_t>(
                qnet::microsToMillisRounded(g_routeEstimator.p95Micros()));
            const qnet::DirectionalRequirement required =
                qnet::symmetricRequirementForRoute(qnet::RouteRttMs{p95Ms});
            const int hostDelay =
                g_cfg.isHost ? g_cfg.delay : g_cfg.remoteDelay;
            const int hostRollback =
                g_cfg.isHost ? g_cfg.rollback : g_cfg.remoteRollback;
            const int clientDelay =
                g_cfg.isHost ? g_cfg.remoteDelay : g_cfg.delay;
            const int clientRollback =
                g_cfg.isHost ? g_cfg.remoteRollback : g_cfg.rollback;
            const qnet::DirectionalBudget budget = qnet::directionalBudget(
                hostDelay, hostRollback, clientDelay, clientRollback);
            const qnet::Classification hostClass = qnet::classify(
                budget.hostReceive, required.hostFrames);
            const qnet::Classification clientClass = qnet::classify(
                budget.clientReceive, required.clientFrames);
            const auto classText = [](qnet::Classification c) {
                return c == qnet::Classification::FullSpeed ? "full-speed"
                     : c == qnet::Classification::Marginal  ? "marginal"
                                                            : "underbuffered";
            };
            QLOG("[route-runtime-class] p95ms=%u required host=%d client=%d "
                 "budget host=%d client=%d class host=%s client=%s",
                 p95Ms, required.hostFrames, required.clientFrames,
                 budget.hostReceive, budget.clientReceive,
                 classText(hostClass), classText(clientClass));
        }
    }
}

DWORD WINAPI pumpThread(LPVOID) {
    QLOG("[dplay] receive pump started (peerKnown=%ld peer=%s)",
         InterlockedCompareExchange(&g_peerKnown, 0, 0),
         InterlockedCompareExchange(&g_peerKnown, 1, 1) == 1 ? epStr(g_peer).c_str() : "?");
    {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f) && f.QuadPart > 0) g_pumpQpcFreq = f.QuadPart;
    }
    bool eventModeUsable = true;
    g_pumpWakeEvent = WSACreateEvent();
    if (g_pumpWakeEvent == WSA_INVALID_EVENT) {
        QLOG("[netlink] WSACreateEvent(wake) failed err=%d; event mode OFF",
             WSAGetLastError());
        eventModeUsable = false;
    }
    if (g_udp != INVALID_SOCKET) {
        g_udpReadEvent = WSACreateEvent();
        if (g_udpReadEvent == WSA_INVALID_EVENT
            || WSAEventSelect(g_udp, g_udpReadEvent, FD_READ) != 0) {
            QLOG("[netlink] WSAEventSelect(player) failed err=%d; event mode OFF",
                 WSAGetLastError());
            eventModeUsable = false;
        }
    }
    if (g_spectatorUdp != INVALID_SOCKET) {
        g_specReadEvent = WSACreateEvent();
        if (g_specReadEvent == WSA_INVALID_EVENT
            || WSAEventSelect(g_spectatorUdp, g_specReadEvent, FD_READ) != 0) {
            QLOG("[netlink] WSAEventSelect(spectator) failed err=%d; event mode OFF",
                 WSAGetLastError());
            eventModeUsable = false;
        }
    }
    const auto disableEventMode = [&](const char* why) {
        const bool wasUsable = eventModeUsable;
        eventModeUsable = false;
        if (wasUsable) {
            QLOG("[netlink] disabling WSA event mode for this pump "
                 "generation (%s); falling back to the bounded nonblocking drain",
                 why);
        }
        u_long nonblocking = 1;
        if (g_udp != INVALID_SOCKET && g_udpReadEvent != WSA_INVALID_EVENT) {
            WSAEventSelect(g_udp, nullptr, 0);
            ioctlsocket(g_udp, FIONBIO, &nonblocking);
        }
        if (g_spectatorUdp != INVALID_SOCKET
            && g_specReadEvent != WSA_INVALID_EVENT) {
            WSAEventSelect(g_spectatorUdp, nullptr, 0);
            ioctlsocket(g_spectatorUdp, FIONBIO, &nonblocking);
        }
        if (g_udpReadEvent != WSA_INVALID_EVENT) {
            WSACloseEvent(g_udpReadEvent);
            g_udpReadEvent = WSA_INVALID_EVENT;
        }
        if (g_specReadEvent != WSA_INVALID_EVENT) {
            WSACloseEvent(g_specReadEvent);
            g_specReadEvent = WSA_INVALID_EVENT;
        }
        unregisterReleasedTopologyRelayReadEvent();
    };
    if (!eventModeUsable) disableEventMode("event setup failed");
    std::vector<uint8_t> buf(kMaxDatagram);
    uint64_t lastHelloMs = 0, lastStateMs = 0, packets = 0;
    uint32_t pingSeq = 0;
    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        g_routeEstimator.onServiceTick(
            qnet::ticksToMicros(realQpcTicks(), g_pumpQpcFreq));
        serviceTestNetworkStatsSnapshotRequest();
        serviceSpectatorTailDrain(GetTickCount64());
        if (g_playerCore || g_spectatorViewerMode) {
            const uint64_t nowMs = GetTickCount64();
            servicePlayerOutbound(nowMs);

            if (g_playerCore && !g_spectatorViewerMode
                && InterlockedCompareExchange(&g_peerKnown, 0, 0) != 0) {
                const int64_t nowProbeUs =
                    qnet::ticksToMicros(realQpcTicks(), g_pumpQpcFreq);
                g_routeEstimator.expireOlderThan(nowProbeUs, 1500000);
                if (g_routeLastProbeUs == 0
                    || nowProbeUs - g_routeLastProbeUs >= 250000) {
                    namespace wire = qnetplay::wire;
                    wire::RouteProbePayload probe{};
                    probe.sequence =
                        g_routeEstimator.onProbeSent(nowProbeUs);
                    probe.generation = g_routeEstimator.generation();
                    probe.stampMicros =
                        static_cast<uint64_t>(nowProbeUs);
                    probe.routeId = 0;
                    probe.role = g_cfg.isHost ? wire::PeerRole::Host
                                              : wire::PeerRole::Client;
                    std::vector<uint8_t> payload, packet;
                    if (wire::encodePayload(probe, payload)
                        && wire::encodeDatagram(
                               wire::MessageType::RouteProbe,
                               g_cfg.sessionId,
                               wire::SessionCookie{g_cfg.cookieLow,
                                                   g_cfg.cookieHigh},
                               g_routeHeaderSeq++, payload, packet)) {
                        sendDatagramOn(g_udp, g_peer, packet,
                                       TestNetworkDomain::Player, "netlink");
                        g_routeLastProbeUs = nowProbeUs;
                    }
                }
            }

            if ((g_playerCore || g_spectatorViewerMode)
                && nowMs - lastStateMs >= 1000) {
                lastStateMs = nowMs;
                emitRouteTelemetry();
                updateWindowTitle();
            }

            (void)synchronizeReleasedTopologyRelayReadEvent(eventModeUsable);
            WSAEVENT waitEvents[4];
            DWORD nWait = 0;
            if (eventModeUsable) {
                if (g_udpReadEvent != WSA_INVALID_EVENT)
                    waitEvents[nWait++] = g_udpReadEvent;
                if (g_spectatorSource && g_specReadEvent != WSA_INVALID_EVENT)
                    waitEvents[nWait++] = g_specReadEvent;
                const ReleasedTopologyRelaySocketBorrow relayBorrowForWait =
                    currentReleasedTopologyRelayBorrow();
                if (relayReadEventMatchesBorrow(relayBorrowForWait))
                    waitEvents[nWait++] = g_relayReadEvent;
                if (g_pumpWakeEvent != WSA_INVALID_EVENT)
                    waitEvents[nWait++] = g_pumpWakeEvent;
            }
            if (nWait == 0) {
                if (g_playerCore && g_udp != INVALID_SOCKET)
                    drainModernSocket(
                        g_udp, transport_detail::ModernSocketDomain::Player,
                        transport_detail::kModernPlayerIngressBudget, buf);
                serviceSpectatorOutbound(GetTickCount64());
                (void)synchronizeReleasedTopologyRelayReadEvent(
                    eventModeUsable);
                if (g_spectatorSource && g_spectatorUdp != INVALID_SOCKET)
                    drainModernSocket(
                        g_spectatorUdp, transport_detail::ModernSocketDomain::Spectator,
                        transport_detail::kModernSpectatorIngressBudget, buf);
                else if (g_spectatorViewerMode && g_udp != INVALID_SOCKET)
                    drainModernSocket(
                        g_udp, transport_detail::ModernSocketDomain::Spectator,
                        transport_detail::kModernSpectatorIngressBudget, buf);
                (void)drainReleasedTopologyRelaySocket(
                    transport_detail::kModernSpectatorIngressBudget, buf);
                Sleep(1);
                continue;
            }
            const DWORD waitRc =
                WSAWaitForMultipleEvents(nWait, waitEvents, FALSE, 10, FALSE);
            if (waitRc == WSA_WAIT_FAILED) {
                if (InterlockedCompareExchange(&g_running, 0, 0) != 0)
                    QLOG("[netlink] WSAWaitForMultipleEvents err=%d",
                         WSAGetLastError());
                disableEventMode("WSAWaitForMultipleEvents failed");
                Sleep(1);
                continue;
            }
            if (waitRc >= WSA_WAIT_EVENT_0 && waitRc < WSA_WAIT_EVENT_0 + nWait)
                InterlockedIncrement(&g_pumpEarlyWakes);
            if (g_pumpWakeEvent != WSA_INVALID_EVENT)
                WSAResetEvent(g_pumpWakeEvent);
            WSANETWORKEVENTS netEv;
            bool enumerationFailed = false;
            if (g_udp != INVALID_SOCKET && g_udpReadEvent != WSA_INVALID_EVENT
                && WSAEnumNetworkEvents(g_udp, g_udpReadEvent, &netEv) != 0) {
                enumerationFailed = true;
            }
            if (g_spectatorUdp != INVALID_SOCKET
                && g_specReadEvent != WSA_INVALID_EVENT
                && WSAEnumNetworkEvents(g_spectatorUdp, g_specReadEvent, &netEv)
                       != 0) {
                enumerationFailed = true;
            }
            const ReleasedTopologyRelaySocketBorrow relayBorrowForEnum =
                currentReleasedTopologyRelayBorrow();
            if (relayReadEventMatchesBorrow(relayBorrowForEnum)) {
                if (WSAEnumNetworkEvents(
                        static_cast<SOCKET>(relayBorrowForEnum.socket),
                        g_relayReadEvent, &netEv) != 0) {
                    enumerationFailed = true;
                }
            } else if (g_relayReadEvent != WSA_INVALID_EVENT) {
                unregisterReleasedTopologyRelayReadEvent();
            }
            if (enumerationFailed && InterlockedCompareExchange(&g_running, 0, 0) != 0) {
                QLOG("[netlink] WSAEnumNetworkEvents err=%d", WSAGetLastError());
                disableEventMode("WSAEnumNetworkEvents failed");
            }
            if (InterlockedCompareExchange(&g_running, 1, 1) != 1)
                continue;

            SocketDrainResult playerDrain{};
            if (g_playerCore && g_udp != INVALID_SOCKET) {
                playerDrain = drainModernSocket(
                    g_udp,
                    transport_detail::ModernSocketDomain::Player,
                    transport_detail::kModernPlayerIngressBudget,
                    buf);
            }

            serviceSpectatorOutbound(GetTickCount64());
            (void)synchronizeReleasedTopologyRelayReadEvent(eventModeUsable);
            SocketDrainResult spectatorDrain{};
            if (g_spectatorSource && g_spectatorUdp != INVALID_SOCKET) {
                spectatorDrain = drainModernSocket(
                    g_spectatorUdp,
                    transport_detail::ModernSocketDomain::Spectator,
                    transport_detail::kModernSpectatorIngressBudget,
                    buf);
            } else if (g_spectatorViewerMode && g_udp != INVALID_SOCKET) {
                spectatorDrain = drainModernSocket(
                    g_udp,
                    transport_detail::ModernSocketDomain::Spectator,
                    transport_detail::kModernSpectatorIngressBudget,
                    buf);
            }
            const SocketDrainResult relayDrain =
                drainReleasedTopologyRelaySocket(
                    transport_detail::kModernSpectatorIngressBudget, buf);

            if (playerDrain.saturated
                || spectatorDrain.saturated || relayDrain.saturated) {
                static uint64_t saturatedTurns = 0;
                if (++saturatedTurns <= 8
                    || saturatedTurns % 300 == 0) {
                    QLOG("[netlink] bounded ingress saturation #%llu "
                         "player=%zu/%zu spectator=%zu/%zu relay=%zu/%zu",
                         static_cast<unsigned long long>(
                             saturatedTurns),
                         playerDrain.datagrams,
                          transport_detail::kModernPlayerIngressBudget,
                          spectatorDrain.datagrams,
                          transport_detail::kModernSpectatorIngressBudget,
                          relayDrain.datagrams,
                          transport_detail::kModernSpectatorIngressBudget);
                }
            }
            continue;
        }

        uint64_t nowMs = GetTickCount64();
        if (!InterlockedCompareExchange(&g_gotRealMsg, 0, 0) && nowMs - lastHelloMs >= 200) {
            sendHello();
            lastHelloMs = nowMs;
            if (!g_ownPipeline && g_cfg.isHost
                && InterlockedCompareExchange(&g_peerKnown, 1, 1) == 1
                && rd32(kNetReadyAddr) != 1) {
                wr32(kNetClientGotAddr, 1);
            }
        }

        if (nowMs - lastStateMs >= 1000) {
            lastStateMs = nowMs;
            QLOG("[dplay] state: net=%u clientGot(4AAA48)=%u ready(4AAA4C)=%u side(4AAA44)=%u scene=%d matchSub=%d msgCnt=%u",
                 rd32(kNetIsNetworkAddr), rd32(kNetClientGotAddr), rd32(kNetReadyAddr),
                 rd32(kNetSideAddr), (int)rd32(kSceneStateAddr), (int)rd32(kMatchSubAddr),
                 rd32(0x66EE28));
            if (InterlockedCompareExchange(&g_peerKnown, 1, 1) == 1) {
                XportPing xp{ kPingMagic, g_cfg.cookieLow, g_cfg.cookieHigh, ++pingSeq,
                              GetTickCount64() };
                ::sendto(g_udp, reinterpret_cast<const char*>(&xp), sizeof(xp), 0,
                         reinterpret_cast<sockaddr*>(&g_peer), sizeof(g_peer));
            }
            updateWindowTitle();
        }

        fd_set rd; FD_ZERO(&rd); FD_SET(g_udp, &rd);
        timeval tv{ 0, 100 * 1000 };
        if (select(0, &rd, nullptr, nullptr, &tv) != 1) continue;

        sockaddr_in from{}; int fromLen = sizeof(from);
        int n = ::recvfrom(g_udp, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0,
                           reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 0) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK && e != WSAECONNRESET)
                QLOG("[dplay] recvfrom error %d (datagram lost)", e);
            continue;
        }
        if (n == 0) continue;

        if (n < static_cast<int>(sizeof(uint32_t))) {
            QLOG("[dplay] DROP undersized datagram size=%d from=%s",
                 n, epStr(from).c_str());
            continue;
        }

        uint32_t type = 0;
        std::memcpy(&type, buf.data(), sizeof(type));

        if (InterlockedCompareExchange(&g_peerKnown, 0, 0) != 0) {
            if (from.sin_family != g_peer.sin_family ||
                from.sin_addr.s_addr != g_peer.sin_addr.s_addr ||
                from.sin_port != g_peer.sin_port) {
                static uint64_t rejectedSources = 0;
                if (++rejectedSources <= 8 || (rejectedSources % 300) == 0)
                    QLOG("[dplay] DROP wrong-source datagram #%llu size=%d from=%s expected=%s",
                         (unsigned long long)rejectedSources, n,
                         epStr(from).c_str(), epStr(g_peer).c_str());
                continue;
            }
        } else {
            if (n != static_cast<int>(sizeof(HelloMsg)) || type != kHelloMagic) {
                QLOG("[dplay] DROP pre-handshake datagram type=%08X size=%d from=%s",
                     type, n, epStr(from).c_str());
                continue;
            }
            const auto* hello = reinterpret_cast<const HelloMsg*>(buf.data());
            if (!cookieMatches(hello->cookieLow, hello->cookieHigh)) {
                QLOG("[dplay] DROP pre-handshake HELLO with wrong cookie from=%s",
                     epStr(from).c_str());
                continue;
            }
            g_peer = from;
            InterlockedExchange(&g_peerKnown, 1);
            QLOG("[dplay] peer locked from HELLO: %s", epStr(from).c_str());
            // Never write game globals here: dword_4AAA48 opens the native boot-batch gate, and sub_45DF10 then dereferences a DirectPlay object that is never created.
            if (!g_ownPipeline && g_cfg.isHost) {
                wr32(kNetClientGotAddr, 1);
                QLOG("[dplay] host set dword_4AAA48=1 (client connected)");
            }
        }

        if (n == static_cast<int>(sizeof(HelloMsg)) && type == kHelloMagic) {
            const auto* hello = reinterpret_cast<const HelloMsg*>(buf.data());
            if (!cookieMatches(hello->cookieLow, hello->cookieHigh)) {
                QLOG("[dplay] DROP HELLO with wrong cookie");
                continue;
            }
            if (!g_ownPipeline && g_cfg.isHost && !rd32(kNetClientGotAddr)) {
                wr32(kNetClientGotAddr, 1);
                QLOG("[dplay] host got HELLO -> dword_4AAA48=1");
            }
            if (g_ownPipeline)
                InterlockedExchange(&g_peerLastRxMs, (LONG)GetTickCount());
            continue;
        }
        if (n == (int)sizeof(XportPing) && type == kPingMagic) {
            const int64_t dwellStart = realQpcTicks();
            XportPing* xp = reinterpret_cast<XportPing*>(buf.data());
            if (!cookieMatches(xp->cookieLow, xp->cookieHigh)) {
                QLOG("[dplay] DROP PING with wrong cookie");
                continue;
            }
            xp->magic = kPongMagic;
            ::sendto(g_udp, reinterpret_cast<const char*>(xp), sizeof(*xp), 0,
                     reinterpret_cast<sockaddr*>(&g_peer), sizeof(g_peer));
            g_localDwellUs.add(
                qnet::ticksToMicros(realQpcTicks() - dwellStart, g_pumpQpcFreq));
            if (g_ownPipeline)
                InterlockedExchange(&g_peerLastRxMs, (LONG)GetTickCount());
            continue;
        }
        if (n == (int)sizeof(XportPing) && type == kPongMagic) {
            const XportPing* xp = reinterpret_cast<const XportPing*>(buf.data());
            if (!cookieMatches(xp->cookieLow, xp->cookieHigh)) {
                QLOG("[dplay] DROP PONG with wrong cookie");
                continue;
            }
            InterlockedExchange(&g_statRttMs, (LONG)(GetTickCount64() - xp->tUs));
            if (g_ownPipeline)
                InterlockedExchange(&g_peerLastRxMs, (LONG)GetTickCount());
            continue;
        }

        if (g_ownPipeline) {
            if (n == (int)sizeof(NetInputsMsg) && type == kNetMagicInputs) {
                const NetInputsMsg* m = reinterpret_cast<const NetInputsMsg*>(buf.data());
                if (!cookieMatches(m->cookieLow, m->cookieHigh)) {
                    QLOG("[netlink] DROP INPUTS with wrong cookie");
                    continue;
                }
                EnterCriticalSection(&g_netCs);
                for (int i = 0; i < kInputRedundancy; ++i) {
                    int64_t f = (int64_t)m->frame - (kInputRedundancy - 1 - i);
                    if (f < 0) continue;
                    uint32_t slot = (uint32_t)f & (kInputRingSize - 1);
                    if (g_remoteFrameOf[slot] != kNoFrame &&
                        (int32_t)((uint32_t)f - g_remoteFrameOf[slot]) < 0) continue;
                    g_remoteRing[slot]    = m->inputs[i];
                    g_remoteFrameOf[slot] = (uint32_t)f;
                }
                if ((LONG)m->frame > InterlockedCompareExchange(&g_remoteHigh, 0, 0))
                    InterlockedExchange(&g_remoteHigh, (LONG)m->frame);
                LeaveCriticalSection(&g_netCs);
                InterlockedExchange(&g_peerLastRxMs, (LONG)GetTickCount());
                InterlockedIncrement(&g_statRxInputs);
                continue;
            }
            if (n == (int)sizeof(NetReadyMsg) && type == kNetMagicReady) {
                const NetReadyMsg* m = reinterpret_cast<const NetReadyMsg*>(buf.data());
                if (!cookieMatches(m->cookieLow, m->cookieHigh)) {
                    QLOG("[netlink] DROP READY with wrong cookie");
                    continue;
                }
                EnterCriticalSection(&g_netCs);
                std::memcpy(&g_remoteReadyMsg, buf.data(), sizeof(NetReadyMsg));
                LeaveCriticalSection(&g_netCs);
                if (!InterlockedExchange(&g_remoteReady, 1))
                    QLOG("[netlink] peer READY (seedG=%08X seedC=%08X sel=%08X)",
                         g_remoteReadyMsg.seedG, g_remoteReadyMsg.seedC, g_remoteReadyMsg.sel);
                InterlockedExchange(&g_peerLastRxMs, (LONG)GetTickCount());
                continue;
            }
            if (n == (int)sizeof(NetSyncMsg) && type == kNetMagicSync) {
                const NetSyncMsg* m = reinterpret_cast<const NetSyncMsg*>(buf.data());
                if (!cookieMatches(m->cookieLow, m->cookieHigh)) {
                    QLOG("[netlink] DROP SYNC with wrong cookie");
                    continue;
                }
                EnterCriticalSection(&g_netCs);
                LONG w = InterlockedCompareExchange(&g_syncWrite, 0, 0);
                std::memcpy(&g_peerSyncRing[w % kSyncRingSize], buf.data(), sizeof(NetSyncMsg));
                InterlockedExchange(&g_syncWrite, w + 1);
                LeaveCriticalSection(&g_netCs);
                InterlockedExchange(&g_peerLastRxMs, (LONG)GetTickCount());
                continue;
            }
            QLOG("[netlink] ignoring unknown datagram type=%08X size=%d", type, n);
            continue;
        }

        // Validate before dispatch: sub_45D6B0 trusts the type and reads fixed-size fields (type 6 copies 0x334 bytes from wire offset 3412).
        if (!msgSizeValid(type, n)) {
            QLOG("[dplay] DROP malformed datagram type=%u size=%d from=%s",
                 type, n, epStr(from).c_str());
            continue;
        }

        InterlockedExchange(&g_gotRealMsg, 1);
        ++packets;
        if (type == 0) InterlockedIncrement(&g_statRxInputs);
        if (packets <= 20 || (packets % 300) == 0)
            QLOG("[dplay] RECV #%llu type=%u size=%d from=%s -> dispatch",
                 (unsigned long long)packets, type, n, epStr(from).c_str());

        {
            EnterCriticalSection(&g_rxCs);
            DispatchFn d = dispatcher();
            if (d) d(buf.data(), (int)g_remoteId, (int)g_localId);
            LeaveCriticalSection(&g_rxCs);
        }
    }
    serviceTestNetworkStatsSnapshotRequest();
    unregisterReleasedTopologyRelayReadEvent();
    if (g_udpReadEvent != WSA_INVALID_EVENT) {
        WSACloseEvent(g_udpReadEvent);
        g_udpReadEvent = WSA_INVALID_EVENT;
    }
    if (g_specReadEvent != WSA_INVALID_EVENT) {
        WSACloseEvent(g_specReadEvent);
        g_specReadEvent = WSA_INVALID_EVENT;
    }
    if (g_pumpWakeEvent != WSA_INVALID_EVENT) {
        WSACloseEvent(g_pumpWakeEvent);
        g_pumpWakeEvent = WSA_INVALID_EVENT;
    }
    QLOG("[dplay] receive pump stopped (%llu packets)", (unsigned long long)packets);
    return 0;
}

static bool makeNonblocking(SOCKET socket, const char* domain) {
    u_long nonblocking = 1;
    if (ioctlsocket(socket, FIONBIO, &nonblocking) == 0)
        return true;
    QLOG("[%s] ioctlsocket(FIONBIO) FAILED err=%d",
         domain, WSAGetLastError());
    return false;
}

static bool getLocalSocketAddress(
    SOCKET socket, sockaddr_in& address, const char* domain) {
    address = {};
    int length = sizeof(address);
    if (getsockname(
            socket, reinterpret_cast<sockaddr*>(&address),
            &length) == 0
        && length == sizeof(address)
        && address.sin_family == AF_INET
        && address.sin_port != 0) {
        return true;
    }
    QLOG("[%s] getsockname FAILED/invalid err=%d",
         domain, WSAGetLastError());
    return false;
}

static bool adoptDuplicatedSocket(
    const WSAPROTOCOL_INFOW& protocolInfo,
    SOCKET& destination,
    const char* domain) {
    WSAPROTOCOL_INFOW info = protocolInfo;
    const std::string beforeRepair = qplat::describeProtocolInfo(info);
    if (qplat::protocolInfoNeedsUdpRepair(info)) {
        const bool changed = qplat::repairDuplicatedUdpProtocolInfo(info);
        QLOG("[%s] duplicated socket protocol info repaired=%d before=%s "
             "after=%s (Wine leaves these unset; the same "
             "difference)",
             domain, changed ? 1 : 0, beforeRepair.c_str(),
             qplat::describeProtocolInfo(info).c_str());
    }
    destination = WSASocketW(
        FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO,
        FROM_PROTOCOL_INFO, &info, 0, 0);
    if (destination == INVALID_SOCKET) {
        QLOG("[%s] WSASocketW(from shared) FAILED err=%d",
             domain, WSAGetLastError());
        return false;
    }
    if (!makeNonblocking(destination, domain)) {
        closesocket(destination);
        destination = INVALID_SOCKET;
        return false;
    }
    return true;
}

bool adoptSocket(const qipc::IpcConfig& cfg) {
    InterlockedExchange(&g_peerKnown, 0);
    std::memset(&g_peer, 0, sizeof(g_peer));
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        QLOG("[dplay] WSAStartup FAILED");
        return false;
    }
    g_wsaStarted = true;
    if (cfg.hasSocket) {
        if (!adoptDuplicatedSocket(
                cfg.protocolInfo, g_udp, "dplay-player")) {
            return false;
        }
        sockaddr_in la{};
        if (!getLocalSocketAddress(g_udp, la, "dplay-player"))
            return false;
        QLOG("[dplay] adopted duplicated primary UDP socket (local %s)",
             epStr(la).c_str());
    } else {
        g_udp = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_udp == INVALID_SOCKET) { QLOG("[dplay] socket() FAILED err=%d", WSAGetLastError()); return false; }
        sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = INADDR_ANY; sa.sin_port = 0;
        if (bind(
                g_udp, reinterpret_cast<sockaddr*>(&sa),
                sizeof(sa)) != 0
            || !makeNonblocking(g_udp, "dplay-player")) {
            QLOG("[dplay] fresh socket bind/config FAILED err=%d",
                 WSAGetLastError());
            return false;
        }
        QLOG("[dplay] opened FRESH UDP socket (config had no shared socket!)");
    }
    if (cfg.peerAddrBE && cfg.peerPortBE) {
        g_peer.sin_family = AF_INET;
        g_peer.sin_addr.s_addr = cfg.peerAddrBE;
        g_peer.sin_port = cfg.peerPortBE;
        InterlockedExchange(&g_peerKnown, 1);
        QLOG("[dplay] peer preset from config: %s", epStr(g_peer).c_str());
    } else {
        QLOG("[dplay] peer NOT preset (host side; will learn from client hello)");
    }
    return true;
}

static bool adoptSpectatorSourceSocket(
    const qipc::IpcConfig& cfg) {
    if (!cfg.isHost || !cfg.serveSpectators
        || !cfg.hasSpectatorSocket) {
        QLOG("[spectator-source] missing required dedicated UDP socket");
        return false;
    }
    if (!adoptDuplicatedSocket(
            cfg.spectatorProtocolInfo, g_spectatorUdp,
            "spectator-source")) {
        return false;
    }

    sockaddr_in playerLocal{};
    sockaddr_in spectatorLocal{};
    if (!getLocalSocketAddress(
            g_udp, playerLocal, "dplay-player")
        || !getLocalSocketAddress(
            g_spectatorUdp, spectatorLocal,
            "spectator-source")) {
        return false;
    }
    if (playerLocal.sin_port == spectatorLocal.sin_port) {
        QLOG("[spectator-source] ABORT: player and spectator sockets "
             "share local UDP port %u",
             ntohs(playerLocal.sin_port));
        return false;
    }

    QLOG("[spectator-source] adopted dedicated UDP socket (local %s; "
         "player local %s)",
         epStr(spectatorLocal).c_str(),
         epStr(playerLocal).c_str());
    return true;
}

}

void spectatorSourceBeginTailDrain(uint32_t deadlineMs) {
    beginTailDrainInternal(deadlineMs);
}

bool spectatorSourceTailDrainPending() {
    return tailDrainPendingInternal();
}

void netSetLocalGameplayDelayStatus(uint8_t requested,
                                    uint8_t applied,
                                    bool pending) {
    InterlockedExchange(
        &g_statGameplayDelayRequested,
        static_cast<LONG>(requested));
    InterlockedExchange(
        &g_statGameplayDelayApplied,
        static_cast<LONG>(applied));
    InterlockedExchange(
        &g_statGameplayDelayPending,
        pending ? 1 : 0);
}

void requestTestNetworkStatsSnapshot() {
    requestTestNetworkStatsSnapshotNoWait();

    LONG state = InterlockedCompareExchange(
        &g_testNetworkStatsState, 0, 0);
    if (state == kTestNetworkStatsDisabled
        || state == kTestNetworkStatsEmitted) {
        return;
    }

    const uint64_t deadline = GetTickCount64() + 1000u;
    while (InterlockedCompareExchange(
               &g_testNetworkStatsState, 0, 0)
               != kTestNetworkStatsEmitted
           && InterlockedCompareExchange(&g_running, 0, 0) == 1
           && GetTickCount64() < deadline) {
        Sleep(1);
    }
}

void requestSpectatorViewerDetach() {
    if (InterlockedCompareExchange(
            &g_spectatorShutdownDetachSent, 1, 0) != 0) {
        return;
    }
    if (!g_spectatorViewerMode || g_udp == INVALID_SOCKET
        || InterlockedCompareExchange(&g_peerKnown, 0, 0) != 1) {
        return;
    }

    if (g_spectatorViewer && g_specCsInit) {
        std::vector<qnetplay::spectator::ViewerDatagram> detachPackets;
        EnterCriticalSection(&g_specCs);
        if (g_spectatorViewer->detach()) {
            detachPackets =
                g_spectatorViewer->pollOutbound(GetTickCount64(), 1);
        }
        LeaveCriticalSection(&g_specCs);
        for (const auto& packet : detachPackets) {
            const std::vector<uint8_t>* wire = &packet.bytes;
            std::vector<uint8_t> framed;
            if (g_spectatorRelayViewer.enabled()) {
                if (!g_spectatorRelayViewer.encodeOutbound(
                        packet.bytes.data(), packet.bytes.size(), framed)) {
                    QLOG("[spectator-viewer] shutdown QSP Detach relay "
                         "framing failed");
                    continue;
                }
                wire = &framed;
            }
            (void)sendDatagramRaw(
                g_udp, g_peer, *wire, "spectator-viewer-detach");
        }
    }

    if (g_spectatorRelayViewer.enabled()) {
        std::vector<uint8_t> detach;
        if (g_spectatorRelayViewer.encodeDetach(
                qnet::relay::SpectatorDetachReason::ViewerRequested,
                detach)) {
            (void)sendDatagramRaw(
                g_udp, g_peer, detach, "spectator-relay-detach");
        }
    }
}

bool spectatorViewerArmValidatedParentRouteTransition(
    const transport_detail::SpectatorParentRouteTransitionAuthorization&
        authorization) {
    using qclient::spectator::topology::client::ReleasedTopologyClientState;
    if (!g_spectatorViewerMode || !g_specCsInit || !g_spectatorViewer
        || !g_releasedTopologyClient
        || g_spectatorRelayViewer.enabled()
        || InterlockedCompareExchange(&g_peerKnown, 0, 0) != 1
        || !g_spectatorParentRouteGateInitialized
        || g_releasedTopologyClient->status().state
               != ReleasedTopologyClientState::AwaitingRootOffer) {
        return false;
    }

    if (g_cfg.spectatorTopologyGrantSourceGeneration == 0u
        || authorization.sourceGeneration
               != static_cast<uint64_t>(
                   g_cfg.spectatorTopologyGrantSourceGeneration)
        || authorization.currentParentIpv4BE != g_cfg.peerAddrBE
        || authorization.currentParentPortBE != g_cfg.peerPortBE
        || g_peer.sin_addr.s_addr != g_cfg.peerAddrBE
        || g_peer.sin_port != g_cfg.peerPortBE) {
        return false;
    }

    bool armed = false;
    EnterCriticalSection(&g_specCs);
    sockaddr_in active{};
    const qnetplay::spectator::ViewerPhase phase =
        g_spectatorViewer->phase();
    if (g_spectatorParentRouteGate.activeEndpoint(active)
        && sameEndpoint(active, g_peer)
        && !releasedTopologyE0Terminal(phase)) {
        transport_detail::SpectatorParentRouteCursor current{};
        current.sourceGeneration = static_cast<uint64_t>(
            g_cfg.spectatorTopologyGrantSourceGeneration);
        current.nextRecord = g_spectatorViewer->receivedNextRecord();
        current.nextEvent = g_spectatorViewer->receivedNextEvent();
        current.digest = g_spectatorViewer->receivedDigest();
        if (g_spectatorParentRouteGate.armValidated(
                authorization, current, GetTickCount64())) {
            armed = g_spectatorViewer
                ->armValidatedParentRouteTransition(
                    authorization.activationId, authorization.nextRecord,
                    authorization.nextEvent, authorization.digest);
            if (!armed) {
                (void)g_spectatorParentRouteGate
                    .cancelPendingActivation(authorization.activationId);
            }
        }
    }
    LeaveCriticalSection(&g_specCs);

    if (armed) {
        QLOG("[spectator-topology] verified parent-route activation armed; "
             "old QSP source remains active pending candidate ingress");
        signalPumpWake();
    }
    return armed;
}

bool spectatorViewerCancelValidatedParentRouteTransition(
    const uint64_t activationId) {
    if (activationId == 0u || !g_spectatorViewerMode || !g_specCsInit
        || !g_spectatorParentRouteGateInitialized) {
        return false;
    }
    EnterCriticalSection(&g_specCs);
    bool cancelled = false;
    if (g_spectatorParentRouteGate.cancelPendingActivation(activationId)) {
        cancelled = g_spectatorViewer != nullptr
            && g_spectatorViewer->cancelValidatedParentRouteTransition(
                activationId);
    }
    LeaveCriticalSection(&g_specCs);
    if (cancelled) {
        QLOG("[spectator-topology] parent-route activation retired; retaining "
             "the existing QSP source");
        signalPumpWake();
    }
    return cancelled;
}

void stopDPlayTransport() {
    requestTestNetworkStatsSnapshotNoWait();

    if (g_pumpThread != nullptr && g_spectatorSource
        && InterlockedCompareExchange(&g_spectatorSourceViewerCount, 0, 0) > 0) {
        spectatorSourceBeginTailDrain(kSpectatorTailDrainDeadlineMs);
        const uint64_t deadline =
            GetTickCount64() + kSpectatorTailDrainDeadlineMs;
        while (spectatorSourceTailDrainPending()
               && InterlockedCompareExchange(&g_running, 0, 0) == 1
               && GetTickCount64() < deadline) {
            Sleep(2);
        }
        QLOG("[spectator-source] tail drain finished before teardown "
             "(viewersRemaining=%ld)",
             InterlockedCompareExchange(&g_spectatorSourceViewerCount, 0, 0));
    }

    requestSpectatorViewerDetach();

    InterlockedExchange(&g_running, 0);
    if (g_pumpWakeEvent != WSA_INVALID_EVENT) WSASetEvent(g_pumpWakeEvent);
    if (g_udp != INVALID_SOCKET) { closesocket(g_udp); g_udp = INVALID_SOCKET; }
    if (g_spectatorUdp != INVALID_SOCKET) {
        closesocket(g_spectatorUdp);
        g_spectatorUdp = INVALID_SOCKET;
    }
    bool workerStopped = true;
    if (g_pumpThread) {
        DWORD wait = WaitForSingleObject(g_pumpThread, 2000);
        workerStopped = (wait == WAIT_OBJECT_0);
        if (!workerStopped)
            QLOG("[dplay] receive pump did not stop cleanly (wait=%lu); preserving shared state",
                 wait);
        CloseHandle(g_pumpThread);
        g_pumpThread = nullptr;
    }
    if (!workerStopped) return;
    serviceTestNetworkStatsSnapshotRequest();
    resetTestNetworkImpairment();
    retireReleasedTopologyClient();
    g_releasedTopologyClientAttempted = false;
    g_releasedTopologyClientAttachedLogged = false;
    g_playerCore.reset();
    // Destroy the source before the archive it references.
    g_spectatorSource.reset();
    g_spectatorViewer.reset();
    g_spectatorArchive.reset();
    if (g_netCsInit) { DeleteCriticalSection(&g_netCs); g_netCsInit = false; }
    if (g_specCsInit) { DeleteCriticalSection(&g_specCs); g_specCsInit = false; }
    if (g_rxCsInit) { DeleteCriticalSection(&g_rxCs); g_rxCsInit = false; }
    if (g_wsaStarted) { WSACleanup(); g_wsaStarted = false; }
    InterlockedExchange(&g_peerKnown, 0);
    g_ownPipeline = false;
    g_spectatorViewerMode = false;
    InterlockedExchange(&g_spectatorPresentationForTitle, 0);
    InterlockedExchange(&g_spectatorWaitForTitle, 0);
    g_spectatorParentRouteGate.clear();
    g_spectatorParentRouteGateInitialized = false;
    g_spectatorParentRouteArmedActivationId = 0u;
    g_spectatorRelayViewer.reset();
    InterlockedExchange(&g_spectatorShutdownDetachSent, 0);
    g_spectatorLastValidMs = 0;
    g_spectatorPacketTokens =
        kSpectatorBurstPackets * kSpectatorTokenScale;
    g_spectatorTokenRefillMs = 0;
    InterlockedExchange(&g_spectatorSourceViewerCount, 0);
    g_hasCompatLocalReady = false;
}

bool startNetplayLink(const qipc::IpcConfig& cfg) {
    QLOG("[netlink] starting: side=%s localPlayer=%u delay=%u rollback=%u",
         cfg.isHost ? "HOST" : "CLIENT", cfg.localPlayer, cfg.delay, cfg.rollback);
    const char* spectatorServiceWhy =
        qipc::validateSpectatorService(cfg);
    const char* spectatorSocketWhy =
        qipc::validateSpectatorSocketHandoff(cfg);
    if (cfg.localPlayer < 1 || cfg.localPlayer > 2
        || cfg.sessionId == 0
        || cfg.compatibilityId != qipc::kRuntimeCompatibilityId
        || qipc::validateIpcCompatibilityIdentity(cfg) != nullptr
        || (cfg.cookieLow == 0 && cfg.cookieHigh == 0)
        || !cfg.hasSocket
        || !cfg.peerAddrBE || !cfg.peerPortBE
        || spectatorServiceWhy != nullptr
        || spectatorSocketWhy != nullptr) {
        QLOG("[netlink] ABORT: invalid player/session/spectator configuration "
             "spectatorServiceReason='%s' spectatorSocketReason='%s'",
             spectatorServiceWhy ? spectatorServiceWhy : "none",
             spectatorSocketWhy ? spectatorSocketWhy : "none");
        return false;
    }

    g_cfg = cfg;
    g_ownPipeline = true;
    g_spectatorViewerMode = false;
    g_localId = directPlayLocalPlayerId(cfg.localPlayer);
    g_remoteId = directPlayRemotePlayerId(cfg.localPlayer);
    netSetLocalGameplayDelayStatus(cfg.delay, cfg.delay, false);
    InitializeCriticalSection(&g_rxCs);
    g_rxCsInit = true;
    InitializeCriticalSection(&g_netCs);
    g_netCsInit = true;
    netSessionReset();
    g_routeEstimator.reset();
    g_routeEstimator.setLocalStallRejectMicros(25000);
    g_routeWindowStarted = false;
    g_routeLastProbeUs = 0;
    g_routeProbesAnswered = 0;
    g_routeProbeDrops = 0;
    g_routeAckTokens = 16;
    g_routeAckRefillUs = 0;
    InterlockedExchange(&g_statRouteP50Ms, -1);
    InterlockedExchange(&g_statPingLastUs, -1);
    InterlockedExchange(&g_statPingMeanUs, -1);
    InterlockedExchange(&g_statPingMinUs, -1);
    InterlockedExchange(&g_statPingMaxUs, -1);
    InterlockedExchange(&g_statPingP95Us, -1);
    InterlockedExchange(&g_statPingJitterUs, 0);
    InterlockedExchange(&g_statPingLossPerMille, 0);
    InterlockedExchange(&g_statPingSamples, 0);
    g_hasCompatLocalReady = false;
    configureTestNetworkImpairment();
    if (!adoptSocket(cfg)) {
        QLOG("[netlink] ABORT: socket setup failed");
        stopDPlayTransport();
        return false;
    }
    const bool spectatorInProcess = cfg.serveSpectators != 0
        && cfg.spectatorServiceMode == qipc::SPECTATOR_SERVICE_INPROCESS;
    bool spectatorSourceUsable = spectatorInProcess;
    if (spectatorInProcess && !adoptSpectatorSourceSocket(cfg)) {
        QLOG("[netlink] dedicated spectator socket setup failed; continuing "
             "WITHOUT spectator service (the player session is unaffected)");
        spectatorSourceUsable = false;
        if (g_spectatorUdp != INVALID_SOCKET) {
            closesocket(g_spectatorUdp);
            g_spectatorUdp = INVALID_SOCKET;
        }
    } else if (cfg.serveSpectators
               && cfg.spectatorServiceMode
                      == qipc::SPECTATOR_SERVICE_SIDECAR) {
        QLOG("[netlink] spectator service = SIDECAR: the caster owns the "
             "socket, admission, fanout and terminal drain; this process only "
             "publishes confirmed records");
    } else if (cfg.spectatorServiceMode == qipc::SPECTATOR_SERVICE_OFF) {
        QLOG("[netlink] spectator service = OFF: no adjacent UDP port and no "
             "archive");
    }

    qnetplay::CoreConfig coreConfig{};
    coreConfig.localRole = cfg.isHost
        ? qnetplay::wire::PeerRole::Host
        : qnetplay::wire::PeerRole::Client;
    coreConfig.localPlayer = static_cast<uint8_t>(cfg.localPlayer - 1);
    coreConfig.compatibilityId = cfg.compatibilityId;
    coreConfig.compatibilityIdentity = cfg.compatibilityIdentity;
    coreConfig.sessionId = cfg.sessionId;
    coreConfig.cookie = qnetplay::wire::SessionCookie{
        cfg.cookieLow,
        cfg.cookieHigh,
    };
    coreConfig.expectedPeer = qnetplay::Endpoint{
        cfg.peerAddrBE,
        ntohs(cfg.peerPortBE),
    };
    coreConfig.learnPeerPort = false;
    coreConfig.firstInputFrame = 0;
    coreConfig.maxPastFrames = 1023;
    coreConfig.maxFutureFrames = 120;
    g_playerCore =
        std::make_unique<qnetplay::NetplayCore>(coreConfig);
    if (g_playerCore->hasTerminalError()) {
        QLOG("[netlink] ABORT: NetplayCore rejected configuration");
        stopDPlayTransport();
        return false;
    }

    if (spectatorSourceUsable) {
        g_spectatorPacketTokens =
            kSpectatorBurstPackets * kSpectatorTokenScale;
        g_spectatorTokenRefillMs = 0;
        g_spectatorArchive =
            std::make_unique<qnetplay::spectator::SpectatorArchive>();
        qnetplay::spectator::SourceConfig sourceConfig{};
        sourceConfig.identity.sessionId = cfg.sessionId;
        sourceConfig.identity.epoch = 1;
        sourceConfig.identity.cookie = {
            cfg.spectatorCookieLow,
            cfg.spectatorCookieHigh,
        };
        sourceConfig.simulationContract = cfg.compatibilityId;
        sourceConfig.compatibilityIdentity = cfg.compatibilityIdentity;
        sourceConfig.perViewerPacketBudget = 1;
        sourceConfig.globalPacketBudget = 8;
        g_spectatorSource =
            std::make_unique<qnetplay::spectator::SpectatorSource>(
                sourceConfig, *g_spectatorArchive);
        InterlockedExchange(&g_spectatorSourceViewerCount, 0);
        if (!g_spectatorArchive->isConfigured()
            || !g_spectatorSource->isConfigured()) {
            QLOG("[spectator-source] configuration invalid; spectator service "
                 "DISABLED for this session (the player session is unaffected)");
            g_spectatorSource.reset();
            g_spectatorArchive.reset();
            if (g_spectatorUdp != INVALID_SOCKET) {
                closesocket(g_spectatorUdp);
                g_spectatorUdp = INVALID_SOCKET;
            }
        } else {
            QLOG("[spectator-source] armed: session=%u epoch=1 maxViewers=%zu",
                 cfg.sessionId, sourceConfig.maxViewers);
        }
    }

    InterlockedExchange(&g_running, 1);
    g_pumpThread = CreateThread(nullptr, 0, pumpThread, nullptr, 0, nullptr);
    if (!g_pumpThread) {
        QLOG("[netlink] ABORT: CreateThread(receive pump) failed error=%lu", GetLastError());
        stopDPlayTransport();
        return false;
    }
    QLOG("[netlink] READY (local mode; game DirectPlay untouched) pumpThread=%p", (void*)g_pumpThread);
    return true;
}

bool netAcceptedCompatibilityIdentity(
    qnetplay::compat::CompatibilityIdentityWire& out) {
    if (!g_ownPipeline
        || !qnetplay::compat::isValidCompatibilityIdentity(
            g_cfg.compatibilityIdentity)) {
        out = {};
        return false;
    }
    out = g_cfg.compatibilityIdentity;
    return true;
}

bool startSpectatorLink(const qipc::IpcConfig& cfg) {
    QLOG("[spectator-viewer] starting receive-only link");
    if (cfg.isHost || cfg.localPlayer != 0
        || cfg.sessionId == 0
        || cfg.compatibilityId != qipc::kRuntimeCompatibilityId
        || qipc::validateIpcCompatibilityIdentity(cfg) != nullptr
        || (cfg.spectatorCookieLow == 0
            && cfg.spectatorCookieHigh == 0)
        || !cfg.hasSocket
        || !cfg.peerAddrBE || !cfg.peerPortBE
        || qipc::validateSpectatorRelayViewer(cfg) != nullptr
        || qipc::validateSpectatorTopologyClient(cfg) != nullptr) {
        QLOG("[spectator-viewer] ABORT: invalid endpoint/session/cookie");
        return false;
    }

    g_cfg = cfg;
    g_spectatorParentRouteGate.clear();
    g_spectatorParentRouteGateInitialized = false;
    g_spectatorParentRouteArmedActivationId = 0u;
    retireReleasedTopologyClient();
    g_releasedTopologyClientAttempted = false;
    g_releasedTopologyClientAttachedLogged = false;
    g_ownPipeline = false;
    g_spectatorViewerMode = true;
    InterlockedExchange(&g_spectatorPresentationForTitle, 0);
    InterlockedExchange(&g_spectatorWaitForTitle, 0);
    InterlockedExchange(&g_spectatorShutdownDetachSent, 0);
    configureTestNetworkImpairment();
    InitializeCriticalSection(&g_specCs);
    g_specCsInit = true;
    if (!adoptSocket(cfg)) {
        QLOG("[spectator-viewer] ABORT: socket setup failed");
        stopDPlayTransport();
        return false;
    }

    g_spectatorRelayViewer.reset();
    if (cfg.spectatorRelayActive) {
        qnetplay::spectator::RelayViewerBinding binding{};
        binding.enabled = true;
        binding.room = {
            cfg.spectatorRelayRoomLow,
            cfg.spectatorRelayRoomHigh,
        };
        binding.token = {
            cfg.spectatorRelayTokenLow,
            cfg.spectatorRelayTokenHigh,
        };
        binding.viewerId = cfg.spectatorViewerId;
        binding.sourceGeneration =
            cfg.spectatorRelaySourceGeneration;
        binding.identity.sessionId = cfg.sessionId;
        binding.identity.epoch = 1;
        binding.identity.cookie = {
            cfg.spectatorCookieLow,
            cfg.spectatorCookieHigh,
        };
        if (!g_spectatorRelayViewer.configure(binding)) {
            QLOG("[spectator-viewer] ABORT: invalid relay viewer binding");
            stopDPlayTransport();
            return false;
        }
        QLOG("[spectator-relay] runtime handoff armed generation=%u "
             "(outer framing required; raw QSP disabled)",
             cfg.spectatorRelaySourceGeneration);
    }

    qnetplay::spectator::ViewerConfig viewerConfig{};
    viewerConfig.identity.sessionId = cfg.sessionId;
    viewerConfig.identity.epoch = 1;
    viewerConfig.identity.cookie = {
        cfg.spectatorCookieLow,
        cfg.spectatorCookieHigh,
    };
    viewerConfig.simulationContract = cfg.compatibilityId;
    viewerConfig.compatibilityIdentity = cfg.compatibilityIdentity;
    viewerConfig.wantsNewestCheckpoint =
        cfg.spectatorWantsNewestCheckpoint != 0u;
    viewerConfig.initialSequence = cfg.spectatorSequenceBase;

    if (cfg.spectatorViewerId == 0) {
        QLOG("[spectator-viewer] ABORT: no viewer identity was handed over "
             "(the caster must mint it; the runtime must not invent one)");
        stopDPlayTransport();
        return false;
    }
    viewerConfig.secureRandom = cfg.spectatorViewerId;
    viewerConfig.viewerNonce = cfg.spectatorViewerId;
    g_spectatorViewer =
        std::make_unique<qnetplay::spectator::SpectatorViewer>(
            viewerConfig);
    QLOG("[spectator-viewer] constructed wantsNewestCheckpoint=%d "
         "(cfg byte=%u) sequenceBase=%u",
         viewerConfig.wantsNewestCheckpoint ? 1 : 0,
         static_cast<unsigned>(cfg.spectatorWantsNewestCheckpoint),
         viewerConfig.initialSequence);
    const uint64_t requestedGame =
        cfg.spectatorGameId != 0 ? cfg.spectatorGameId
                                 : qnetplay::spectator::kLatestGameId;
    if (!g_spectatorViewer->isConfigured()
        || !g_spectatorViewer->begin(requestedGame)) {
        QLOG("[spectator-viewer] ABORT: viewer state configuration failed");
        stopDPlayTransport();
        return false;
    }
    if (!g_spectatorParentRouteGate.reset(g_peer)) {
        QLOG("[spectator-viewer] ABORT: invalid initial parent route");
        stopDPlayTransport();
        return false;
    }
    g_spectatorParentRouteGateInitialized = true;
    QLOG("[spectator-viewer] handoff viewer=%016llX game=%016llX gen=%u "
         "resume=%u",
         static_cast<unsigned long long>(cfg.spectatorViewerId),
         static_cast<unsigned long long>(requestedGame),
         cfg.spectatorGeneration, cfg.spectatorResumeRecord);

    g_spectatorLastValidMs = 0;
    InterlockedExchange(&g_running, 1);
    g_pumpThread =
        CreateThread(nullptr, 0, pumpThread, nullptr, 0, nullptr);
    if (!g_pumpThread) {
        QLOG("[spectator-viewer] ABORT: CreateThread failed error=%lu",
             GetLastError());
        stopDPlayTransport();
        return false;
    }
    QLOG("[spectator-viewer] READY source=%s session=%u epoch=1",
         epStr(g_peer).c_str(), cfg.sessionId);
    return true;
}

bool netSetSimulationFrame(uint32_t frame) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->setSimulationFrame(frame);
    LeaveCriticalSection(&g_netCs);
    return ok;
}

bool netSubmitLocalInput(uint32_t frame, uint16_t packed) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->submitLocalInput(frame, packed);
    LeaveCriticalSection(&g_netCs);
    if (ok) {
        InterlockedExchange(
            &g_statTxInputs,
            static_cast<LONG>(std::min<uint32_t>(
                frame + 1u, static_cast<uint32_t>(LONG_MAX))));
        InterlockedCompareExchange64(&g_enqueuePendingTicks, realQpcTicks(), 0);
        signalPumpWake();
    }
    return ok;
}

bool netGetLocalExact(uint32_t frame, uint16_t& out) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->getLocalInput(frame, out);
    LeaveCriticalSection(&g_netCs);
    return ok;
}

bool netGetRemoteExact(uint32_t frame, uint16_t& out) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->getRemoteInput(frame, out);
    LeaveCriticalSection(&g_netCs);
    return ok;
}

uint32_t netRemoteAckThrough() {
    if (!g_netCsInit || !g_playerCore) return kNetNoFrame;
    EnterCriticalSection(&g_netCs);
    const uint32_t frame = g_playerCore->remoteAckThrough();
    LeaveCriticalSection(&g_netCs);
    return frame;
}

uint32_t netPeerAckThrough() {
    if (!g_netCsInit || !g_playerCore) return kNetNoFrame;
    EnterCriticalSection(&g_netCs);
    const uint32_t frame = g_playerCore->peerAckThrough();
    LeaveCriticalSection(&g_netCs);
    return frame;
}

bool netLinkEstablished() {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool established = g_playerCore->isEstablished();
    LeaveCriticalSection(&g_netCs);
    return established;
}

bool netPeerAlive() {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool alive = g_playerCore->isAlive(GetTickCount64());
    LeaveCriticalSection(&g_netCs);
    return alive;
}

bool netPeerPingStats(NetPingStats& out) {
    const LONG mean = InterlockedCompareExchange(&g_statPingMeanUs, 0, 0);
    if (mean < 0) return false;
    out.meanMicros = static_cast<uint32_t>(mean);
    const LONG last = InterlockedCompareExchange(&g_statPingLastUs, 0, 0);
    out.lastMicros = last < 0 ? out.meanMicros
                              : static_cast<uint32_t>(last);
    const LONG minimum =
        InterlockedCompareExchange(&g_statPingMinUs, 0, 0);
    const LONG maximum =
        InterlockedCompareExchange(&g_statPingMaxUs, 0, 0);
    const LONG p95 = InterlockedCompareExchange(&g_statPingP95Us, 0, 0);
    out.minimumMicros = minimum < 0 ? out.meanMicros
                                    : static_cast<uint32_t>(minimum);
    out.maximumMicros = maximum < 0 ? out.meanMicros
                                    : static_cast<uint32_t>(maximum);
    out.p95Micros = p95 < 0 ? out.meanMicros : static_cast<uint32_t>(p95);
    out.jitterMicros = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_statPingJitterUs, 0, 0));
    out.lossPerMille = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_statPingLossPerMille, 0, 0));
    out.samples = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_statPingSamples, 0, 0));
    return true;
}

const char* netGameThreadStageName(
    const NetTransportDiagnostics::GameThreadStage stage) {
    using Stage = NetTransportDiagnostics::GameThreadStage;
    switch (stage) {
        case Stage::None: return "none";
        case Stage::DriverEnter: return "driver-enter";
        case Stage::TransportDrain: return "transport-drain";
        case Stage::Scheduler: return "scheduler";
        case Stage::NativeEnter: return "native-enter";
        case Stage::PresentationWait: return "presentation-wait";
        case Stage::NativeReturn: return "native-return";
        case Stage::CanonicalCommit: return "canonical-commit";
        case Stage::PresentationCommit: return "presentation-commit";
        case Stage::Count: break;
    }
    return "unknown";
}

void netPublishGameThreadHeartbeat(
    const NetTransportDiagnostics::GameThreadStage stage,
    const uint32_t frame) {
    const size_t index = static_cast<size_t>(stage);
    if (index == 0
        || index >= static_cast<size_t>(
            NetTransportDiagnostics::GameThreadStage::Count)) {
        return;
    }
    InterlockedIncrement(&g_gameThreadHeartbeatSequence);
    InterlockedExchange(
        &g_gameThreadFrame, static_cast<LONG>(frame));
    InterlockedExchange(
        &g_gameThreadStageTick,
        static_cast<LONG>(GetTickCount()));
    InterlockedIncrement(&g_gameThreadStageCounts[index]);
    MemoryBarrier();
    InterlockedExchange(
        &g_gameThreadStage, static_cast<LONG>(index));
    InterlockedIncrement(&g_gameThreadHeartbeatSequence);
}

bool netGetTransportDiagnostics(NetTransportDiagnostics& out) {
    out = {};
    out.hasPing = netPeerPingStats(out.ping);

    uint64_t accepted = 0;
    uint64_t rejected = 0;
    for (size_t i = 0; i < out.ingressByResult.size(); ++i) {
        const LONG raw =
            InterlockedCompareExchange(&g_statIngressResults[i], 0, 0);
        const uint32_t count =
            raw > 0 ? static_cast<uint32_t>(raw) : 0u;
        out.ingressByResult[i] = count;
        if (i == static_cast<size_t>(
                     qnetplay::IngressResult::Accepted)) {
            accepted += count;
        } else {
            rejected += count;
        }
    }
    out.ingressAccepted = static_cast<uint32_t>(
        (std::min)(accepted, static_cast<uint64_t>(UINT32_MAX)));
    out.ingressRejected = static_cast<uint32_t>(
        (std::min)(rejected, static_cast<uint64_t>(UINT32_MAX)));
    const LONG lastResult =
        InterlockedCompareExchange(&g_statLastIngressResult, 0, 0);
    if (lastResult >= 0
        && static_cast<size_t>(lastResult)
             < static_cast<size_t>(qnetplay::IngressResult::Count)) {
        out.hasLastIngress = true;
        out.lastIngress =
            static_cast<qnetplay::IngressResult>(lastResult);
        const DWORD lastTick = static_cast<DWORD>(
            InterlockedCompareExchange(
                &g_statLastIngressTick, 0, 0));
        if (lastTick != 0)
            out.lastIngressAgeMs = GetTickCount() - lastTick;
    }
    const auto nonnegativeCounter = [](volatile LONG* value) {
        const LONG raw = InterlockedCompareExchange(value, 0, 0);
        return raw > 0 ? static_cast<uint32_t>(raw) : 0u;
    };
    out.crossDomainDrops =
        nonnegativeCounter(&g_statCrossDomainDrops);
    out.wrongSourceDrops =
        nonnegativeCounter(&g_statWrongSourceDrops);
    out.receiveErrors =
        nonnegativeCounter(&g_statReceiveErrors);
    out.sendFailures =
        nonnegativeCounter(&g_statSendFailures);
    out.lastSocketError = InterlockedCompareExchange(
        &g_statLastSocketError, 0, 0);
    for (unsigned attempt = 0; attempt < 8u; ++attempt) {
        const LONG sequenceBefore = InterlockedCompareExchange(
            &g_gameThreadHeartbeatSequence, 0, 0);
        if ((sequenceBefore & 1) != 0)
            continue;
        const LONG stageRaw =
            InterlockedCompareExchange(&g_gameThreadStage, 0, 0);
        const uint32_t frame = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_gameThreadFrame, 0, 0));
        const DWORD stageTick = static_cast<DWORD>(
            InterlockedCompareExchange(&g_gameThreadStageTick, 0, 0));
        std::array<uint32_t,
                   static_cast<size_t>(
                       NetTransportDiagnostics::GameThreadStage::Count)>
            counts{};
        for (size_t i = 0; i < counts.size(); ++i) {
            const LONG raw = InterlockedCompareExchange(
                &g_gameThreadStageCounts[i], 0, 0);
            counts[i] = raw > 0
                ? static_cast<uint32_t>(raw) : 0u;
        }
        MemoryBarrier();
        const LONG sequenceAfter = InterlockedCompareExchange(
            &g_gameThreadHeartbeatSequence, 0, 0);
        if (sequenceBefore != sequenceAfter
            || (sequenceAfter & 1) != 0) {
            continue;
        }
        if (stageRaw >= 0
            && static_cast<size_t>(stageRaw)
                < static_cast<size_t>(
                    NetTransportDiagnostics::GameThreadStage::Count)) {
            out.gameThreadStage =
                static_cast<
                    NetTransportDiagnostics::GameThreadStage>(stageRaw);
        }
        out.gameThreadFrame = frame;
        if (stageTick != 0)
            out.gameThreadStageAgeMs = GetTickCount() - stageTick;
        out.gameThreadStageCounts = counts;
        break;
    }

    if (g_netCsInit && g_playerCore) {
        EnterCriticalSection(&g_netCs);
        out.core =
            g_playerCore->diagnostics(GetTickCount64());
        LeaveCriticalSection(&g_netCs);
        out.available = true;
    }
    return out.available;
}

uint32_t netPeerRttMicros() {
    NetPingStats stats{};
    if (netPeerPingStats(stats)) return stats.meanMicros;
    if (!g_netCsInit || !g_playerCore) return 0;
    EnterCriticalSection(&g_netCs);
    const uint32_t rtt =
        g_playerCore->hasRtt() ? g_playerCore->rttMs() : 0;
    LeaveCriticalSection(&g_netCs);
    return rtt * 1000u;
}

uint32_t netPeerRttMs() {
    NetPingStats stats{};
    if (netPeerPingStats(stats))
        return qnet::microsToMillisRounded(
            static_cast<int64_t>(stats.meanMicros));
    if (!g_netCsInit || !g_playerCore) return 0;
    EnterCriticalSection(&g_netCs);
    const uint32_t rtt =
        g_playerCore->hasRtt() ? g_playerCore->rttMs() : 0;
    LeaveCriticalSection(&g_netCs);
    return rtt;
}

bool netPeerHasRtt() {
    NetPingStats stats{};
    if (netPeerPingStats(stats)) return true;
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool has = g_playerCore->hasRtt();
    LeaveCriticalSection(&g_netCs);
    return has;
}

bool netPeerHasSimFrontier() {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool has = g_playerCore->hasPeerSimFrontier();
    LeaveCriticalSection(&g_netCs);
    return has;
}

uint32_t netPeerSimFrontier() {
    if (!g_netCsInit || !g_playerCore) return kNetNoFrame;
    EnterCriticalSection(&g_netCs);
    const uint32_t frontier = g_playerCore->peerSimFrontier();
    LeaveCriticalSection(&g_netCs);
    return frontier;
}

uint64_t netPeerFrontierAgeMs() {
    if (!g_netCsInit || !g_playerCore) return ~0ull;
    EnterCriticalSection(&g_netCs);
    const uint64_t age =
        g_playerCore->peerFrontierAgeMs(GetTickCount64());
    LeaveCriticalSection(&g_netCs);
    return age;
}

void netPublishLocalPressure(const uint32_t inputProducedThrough,
                             const uint32_t confirmedFrontier,
                             const uint16_t predictionDepth,
                             const uint8_t correctionDebt,
                             const uint8_t runState,
                             const uint32_t holdGeneration) {
    if (!g_netCsInit || !g_playerCore) return;
    qnetplay::wire::PressureReport report{};
    report.inputProducedThrough = inputProducedThrough;
    report.confirmedFrontier = confirmedFrontier;
    report.predictionDepth = predictionDepth;
    report.correctionDebt = correctionDebt;
    report.runState = runState > qnetplay::wire::kMaxRunStateOrdinal
        ? 0u : runState;
    report.holdGeneration = holdGeneration;
    EnterCriticalSection(&g_netCs);
    g_playerCore->setPressureReport(report);
    LeaveCriticalSection(&g_netCs);
}

bool netPeerPressure(NetPeerPressure& out) {
    if (!g_netCsInit || !g_playerCore) return false;
    qnetplay::wire::PressureReport report{};
    uint64_t ageMs = 0;
    EnterCriticalSection(&g_netCs);
    const bool has = g_playerCore->peerPressureReport(
        report, ageMs, GetTickCount64());
    LeaveCriticalSection(&g_netCs);
    if (!has) return false;
    out.inputProducedThrough = report.inputProducedThrough;
    out.confirmedFrontier = report.confirmedFrontier;
    out.holdGeneration = report.holdGeneration;
    out.predictionDepth = report.predictionDepth;
    out.correctionDebt = report.correctionDebt;
    out.runState = report.runState;
    out.ageMs = ageMs;
    return true;
}

bool netSubmitLocalReady(const NetReadyState& ready) {
    if (!g_netCsInit || !g_playerCore
        || !netReadyMatchConfigValid(ready)) {
        return false;
    }
    const qnetplay::wire::ReadyPayload payload{
        ready.seedGameplay,
        ready.seedCosmetic,
        ready.selector,
        0,
        ready.bootstrapHash,
        ready.timerMode,
        ready.aiDifficulty,
        ready.lifeMultiplier,
        ready.roundsToWinMinusOne,
    };
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->submitLocalReady(payload);
    if (ok) {
        g_compatLocalReady = ready;
        g_hasCompatLocalReady = true;
    }
    LeaveCriticalSection(&g_netCs);
    return ok;
}

bool netGetRemoteReady(NetReadyState& ready) {
    if (!g_netCsInit || !g_playerCore) return false;
    qnetplay::wire::ReadyPayload payload{};
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->getRemoteReady(payload);
    LeaveCriticalSection(&g_netCs);
    if (ok) {
        ready.seedGameplay = payload.seedGameplay;
        ready.seedCosmetic = payload.seedCosmetic;
        ready.selector = payload.selector;
        ready.bootstrapHash = payload.bootstrapHash;
        ready.timerMode = payload.timerMode;
        ready.aiDifficulty = payload.aiDifficulty;
        ready.lifeMultiplier = payload.lifeMultiplier;
        ready.roundsToWinMinusOne =
            payload.roundsToWinMinusOne;
    }
    return ok;
}

bool netReadyComplete() {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool complete = g_playerCore->readyComplete();
    LeaveCriticalSection(&g_netCs);
    return complete;
}

bool netSubmitConfirmedHash(const NetConfirmedHash& hash) {
    if (!g_netCsInit || !g_playerCore
        || hash.frame == qnetplay::wire::kLayoutFingerprintFrame) {
        return false;
    }
    qnetplay::wire::SyncHashPayload payload{};
    payload.tag.layoutGeneration = hash.layoutGeneration;
    payload.tag.epoch = hash.epoch;
    payload.tag.frame = hash.frame;
    payload.rngGameplay = hash.rngGameplay;
    payload.rngCosmetic = hash.rngCosmetic;
    payload.rngSelector = hash.rngSelector;
    payload.hp0 = hash.hp0;
    payload.hp1 = hash.hp1;
    payload.gameplayHash = hash.gameplayHash;
    EnterCriticalSection(&g_netCs);
    const qnetplay::SyncHashSubmitResult result =
        g_playerCore->submitLocalSyncHashDetailed(payload);
    qnetplay::wire::SyncHashTag last{};
    const bool hasLast = g_playerCore->lastLocalSyncHashTag(last);
    const size_t pending = g_playerCore->pendingLocalSyncHashCount();
    LeaveCriticalSection(&g_netCs);
    const bool ok =
        result == qnetplay::SyncHashSubmitResult::Accepted
        || result == qnetplay::SyncHashSubmitResult::ExactDuplicate;
    if (!ok) {
        QLOG("[netlink] confirmed-hash submit rejected result=%s(%u) "
             "tag=[%u,%u,%u] pending=%zu last=%s[%u,%u,%u]",
             qnetplay::syncHashSubmitResultName(result),
             static_cast<unsigned>(result),
             payload.tag.layoutGeneration, payload.tag.epoch,
             payload.tag.frame, pending, hasLast ? "" : "none/",
             last.layoutGeneration, last.epoch, last.frame);
    }
    return ok;
}

bool netSubmitSessionEvent(
    const qnetplay::wire::SessionEventPayload& payload) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->submitSessionEvent(payload);
    LeaveCriticalSection(&g_netCs);
    return ok;
}

void netClearSessionEvent() {
    if (!g_netCsInit || !g_playerCore) return;
    EnterCriticalSection(&g_netCs);
    g_playerCore->clearSessionEvent();
    LeaveCriticalSection(&g_netCs);
}

bool netTakeSessionEvent(qnetplay::wire::SessionEventPayload& out) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool got = g_playerCore->takeSessionEvent(out);
    LeaveCriticalSection(&g_netCs);
    return got;
}

bool netSubmitRematchChoice(
    const qnetplay::wire::RematchChoicePayload& payload) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->submitRematchChoice(payload);
    LeaveCriticalSection(&g_netCs);
    return ok;
}

bool netTakeRematchChoice(qnetplay::wire::RematchChoicePayload& out) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool got = g_playerCore->takeRematchChoice(out);
    LeaveCriticalSection(&g_netCs);
    return got;
}

bool netSubmitPalette(const qnetplay::wire::PalettePayload& payload) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->submitPalette(payload);
    LeaveCriticalSection(&g_netCs);
    return ok;
}

bool netTakePalette(qnetplay::wire::PalettePayload& out) {
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const bool got = g_playerCore->takePalette(out);
    LeaveCriticalSection(&g_netCs);
    return got;
}

bool netTakeConfirmedHash(NetConfirmedHash& hash) {
    if (!g_netCsInit || !g_playerCore) return false;
    qnetplay::wire::SyncHashPayload payload{};
    EnterCriticalSection(&g_netCs);
    const bool ok =
        g_playerCore->peekRemoteSyncHash(payload)
        && payload.tag.frame != qnetplay::wire::kLayoutFingerprintFrame
        && g_playerCore->takeRemoteSyncHash(payload);
    LeaveCriticalSection(&g_netCs);
    if (!ok) return false;
    hash.layoutGeneration = payload.tag.layoutGeneration;
    hash.epoch = payload.tag.epoch;
    hash.frame = payload.tag.frame;
    hash.rngGameplay = payload.rngGameplay;
    hash.rngCosmetic = payload.rngCosmetic;
    hash.rngSelector = payload.rngSelector;
    hash.hp0 = payload.hp0;
    hash.hp1 = payload.hp1;
    hash.gameplayHash = payload.gameplayHash;
    return true;
}

bool netSubmitLayoutFingerprint(
    const NetLayoutFingerprint& fingerprint) {
    if (!g_netCsInit || !g_playerCore
        || fingerprint.layoutGeneration == 0
        || fingerprint.epoch == 0
        || fingerprint.digest == 0) {
        return false;
    }
    qnetplay::wire::SyncHashPayload payload{};
    payload.tag.layoutGeneration = fingerprint.layoutGeneration;
    payload.tag.epoch = fingerprint.epoch;
    payload.tag.frame = qnetplay::wire::kLayoutFingerprintFrame;
    payload.gameplayHash = fingerprint.digest;
    payload.rngGameplay = fingerprint.audioManifest32;
    payload.rngCosmetic = fingerprint.state32;
    payload.rngSelector = fingerprint.phase32;
    payload.hp0 = static_cast<int32_t>(fingerprint.fpu32);
    payload.hp1 = static_cast<int32_t>(fingerprint.virtualAudio32);
    EnterCriticalSection(&g_netCs);
    const bool ok = g_playerCore->submitLocalSyncHash(payload);
    LeaveCriticalSection(&g_netCs);
    return ok;
}

bool netTakeLayoutFingerprint(
    NetLayoutFingerprint& fingerprint) {
    if (!g_netCsInit || !g_playerCore) return false;
    qnetplay::wire::SyncHashPayload payload{};
    EnterCriticalSection(&g_netCs);
    const bool ok =
        g_playerCore->peekRemoteSyncHash(payload)
        && payload.tag.frame == qnetplay::wire::kLayoutFingerprintFrame
        && g_playerCore->takeRemoteSyncHash(payload);
    LeaveCriticalSection(&g_netCs);
    if (!ok) return false;
    fingerprint.layoutGeneration = payload.tag.layoutGeneration;
    fingerprint.epoch = payload.tag.epoch;
    fingerprint.digest = payload.gameplayHash;
    fingerprint.audioManifest32 = payload.rngGameplay;
    fingerprint.state32 = payload.rngCosmetic;
    fingerprint.phase32 = payload.rngSelector;
    fingerprint.fpu32 = static_cast<uint32_t>(payload.hp0);
    fingerprint.virtualAudio32 = static_cast<uint32_t>(payload.hp1);
    return true;
}

bool netPeekLayoutFingerprint(
    NetLayoutFingerprint& fingerprint) {
    if (!g_netCsInit || !g_playerCore) return false;
    qnetplay::wire::SyncHashPayload payload{};
    EnterCriticalSection(&g_netCs);
    const bool ok =
        g_playerCore->peekRemoteSyncHash(payload)
        && payload.tag.frame == qnetplay::wire::kLayoutFingerprintFrame;
    LeaveCriticalSection(&g_netCs);
    if (!ok) return false;
    fingerprint.layoutGeneration = payload.tag.layoutGeneration;
    fingerprint.epoch = payload.tag.epoch;
    fingerprint.digest = payload.gameplayHash;
    fingerprint.audioManifest32 = payload.rngGameplay;
    fingerprint.state32 = payload.rngCosmetic;
    fingerprint.phase32 = payload.rngSelector;
    fingerprint.fpu32 = static_cast<uint32_t>(payload.hp0);
    fingerprint.virtualAudio32 = static_cast<uint32_t>(payload.hp1);
    return true;
}

NetLayoutMismatchExplanation netLayoutComponentMismatch(
    const NetLayoutFingerprint& local, const NetLayoutFingerprint& peer) {
    if (local.state32 != peer.state32) {
        return { "post-load gameplay state",
                 "the two sides' game state differed immediately after "
                 "loading", true };
    }
    if (local.phase32 != peer.phase32) {
        return { "match phase",
                 "the two sides were in different parts of the match", false };
    }
    if (local.fpu32 != peer.fpu32) {
        return { "floating-point environment",
                 "the two machines' floating-point environment differs",
                 false };
    }
    return { nullptr, "the two sides' game state diverged", false };
}

bool netLayoutFingerprintAcknowledged(
    uint32_t layoutGeneration,
    uint32_t epoch) {
    if (!g_netCsInit || !g_playerCore
        || layoutGeneration == 0 || epoch == 0) {
        return false;
    }
    const qnetplay::wire::SyncHashTag tag{
        layoutGeneration,
        epoch,
        qnetplay::wire::kLayoutFingerprintFrame,
    };
    EnterCriticalSection(&g_netCs);
    const bool acknowledged =
        g_playerCore->localSyncHashAcknowledged(tag);
    LeaveCriticalSection(&g_netCs);
    return acknowledged;
}

bool netGetLinkError(NetLinkError& error) {
    error = {};
    if (!g_netCsInit || !g_playerCore) return false;
    EnterCriticalSection(&g_netCs);
    const qnetplay::TerminalState terminal =
        g_playerCore->terminalState();
    LeaveCriticalSection(&g_netCs);
    error.active = terminal.active;
    error.fromPeer = terminal.fromPeer;
    error.code = static_cast<uint32_t>(terminal.code);
    error.detail = terminal.detail;
    error.reasonId = terminal.reasonId;
    std::memcpy(error.reason, terminal.reason.data(),
                sizeof(error.reason));
    error.reason[sizeof(error.reason) - 1u] = '\0';
    return terminal.active;
}

void netSetLinkError(uint32_t code, uint32_t detail,
                     const char* reason) {
    if (!g_netCsInit || !g_playerCore) return;
    EnterCriticalSection(&g_netCs);
    g_playerCore->setLocalTerminalError(
        static_cast<qnetplay::wire::TerminalErrorCode>(code),
        detail, reason);
    LeaveCriticalSection(&g_netCs);
}

bool spectatorSourceActive() {
    return g_spectatorArchive && g_spectatorSource;
}

size_t spectatorSourceViewerCount() {
    const LONG count = InterlockedCompareExchange(
        &g_spectatorSourceViewerCount, 0, 0);
    return count > 0 ? static_cast<size_t>(count) : 0;
}

qnetplay::spectator::ArchiveResult spectatorSourceBeginGame(
    const qnetplay::spectator::GameDescriptor& descriptor) {
    if (!g_spectatorArchive || !g_spectatorSource
        || descriptor.simulationContract != g_cfg.compatibilityId) {
        return qnetplay::spectator::ArchiveResult::InvalidArgument;
    }
    const auto result = g_spectatorArchive->beginGame(descriptor);
    if (result != qnetplay::spectator::ArchiveResult::Ok
        && result !=
            qnetplay::spectator::ArchiveResult::ExactDuplicate) {
        QLOG("[spectator-source] begin game=%llu failed result=%u",
             static_cast<unsigned long long>(descriptor.gameId),
             static_cast<unsigned>(result));
    }
    return result;
}

qnetplay::spectator::ArchiveResult spectatorSourceAppendConfirmed(
    uint64_t gameId,
    const qnetplay::spectator::ConfirmedRecord& record,
    qnetplay::spectator::ConfirmedRecord* stored) {
    if (!g_spectatorArchive || !g_spectatorSource)
        return qnetplay::spectator::ArchiveResult::InvalidArgument;
    const auto result = g_spectatorArchive->appendConfirmed(
        gameId, record, stored);
    if (result != qnetplay::spectator::ArchiveResult::Ok
        && result !=
            qnetplay::spectator::ArchiveResult::ExactDuplicate) {
        QLOG("[spectator-source] append game=%llu record=%u failed result=%u",
             static_cast<unsigned long long>(gameId), record.record,
             static_cast<unsigned>(result));
    }
    return result;
}

qnetplay::spectator::ArchiveResult spectatorSourceSealGame(
    uint64_t gameId,
    qnetplay::spectator::TerminalReason reason,
    uint64_t postTailStateHash) {
    if (!g_spectatorArchive || !g_spectatorSource)
        return qnetplay::spectator::ArchiveResult::InvalidArgument;
    const auto result = g_spectatorArchive->sealGame(
        gameId, reason, postTailStateHash);
    if (result != qnetplay::spectator::ArchiveResult::Ok
        && result !=
            qnetplay::spectator::ArchiveResult::ExactDuplicate) {
        QLOG("[spectator-source] seal game=%llu failed result=%u",
             static_cast<unsigned long long>(gameId),
             static_cast<unsigned>(result));
    }
    return result;
}

void spectatorPublishPlaybackWait(qgame::SpectatorWait reason,
                                  uint32_t haveRecords,
                                  uint32_t targetRecords) {
    const uint32_t have = haveRecords > kSpectatorWaitCountMax
        ? kSpectatorWaitCountMax : haveRecords;
    const uint32_t target = targetRecords > kSpectatorWaitCountMax
        ? kSpectatorWaitCountMax : targetRecords;
    InterlockedExchange(
        &g_spectatorWaitForTitle,
        static_cast<LONG>((static_cast<uint32_t>(reason) & 3u)
                          | (have << 2) | (target << 16)));
}

void spectatorPublishPlaybackPresentation(bool bufferPrimed,
                                           bool catchingUp,
                                           bool paused) {
    const LONG presentation =
        (bufferPrimed ? kSpectatorTitlePrimed : 0)
        | (catchingUp ? kSpectatorTitleCatchingUp : 0)
        | (paused ? kSpectatorTitlePaused : 0);
    InterlockedExchange(&g_spectatorPresentationForTitle, presentation);
}

bool spectatorViewerCheckpointProgress(uint32_t& chunksSeen,
                                       uint32_t& chunkCount) {
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    const bool got = g_spectatorViewer->checkpointAssemblyProgress(
        chunksSeen, chunkCount);
    LeaveCriticalSection(&g_specCs);
    return got;
}

bool spectatorViewerTakeCheckpoint(qnetplay::spectator::Checkpoint& out) {
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    const bool got = g_spectatorViewer->takeAssembledCheckpoint(out);
    LeaveCriticalSection(&g_specCs);
    return got;
}

bool spectatorViewerResumeAtCheckpoint(
    uint32_t recordCursor, uint32_t eventCursor, uint64_t chainDigest,
    const qnetplay::spectator::SpectatorSetTimeline::BindSeed& seed) {
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    const bool ok = g_spectatorViewer->resumeAtCheckpoint(
        recordCursor, eventCursor, chainDigest, seed);
    LeaveCriticalSection(&g_specCs);
    return ok;
}

uint32_t spectatorViewerSetEventCount() {
    if (!g_specCsInit || !g_spectatorViewer) return 0u;
    EnterCriticalSection(&g_specCs);
    const uint32_t count = g_spectatorViewer->receivedNextEvent();
    LeaveCriticalSection(&g_specCs);
    return count;
}

bool spectatorViewerSetEventAt(uint32_t index,
                               qnetplay::spectator::SetEvent& out) {
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    const bool got = g_spectatorViewer->setTimeline().event(index, out);
    LeaveCriticalSection(&g_specCs);
    return got;
}

bool spectatorViewerGetStatus(SpectatorViewerStatus& status) {
    status = {};
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    status.active = true;
    status.phase = g_spectatorViewer->phase();
    status.failure = g_spectatorViewer->failure();
    status.gameId = g_spectatorViewer->gameId();
    status.receivedNextRecord =
        g_spectatorViewer->receivedNextRecord();
    status.playbackNextRecord =
        g_spectatorViewer->playbackNextRecord();
    status.bufferedRecords =
        g_spectatorViewer->bufferedPlaybackRecords();
    qnetplay::spectator::GameStartPayload start{};
    status.hasGameStart = g_spectatorViewer->gameStart(start);
    qnetplay::spectator::ArchiveTerminal terminal{};
    status.hasTerminal = g_spectatorViewer->terminal(terminal);
    const uint64_t nowMs = GetTickCount64();
    status.linkAlive = g_spectatorLastValidMs != 0
        && nowMs >= g_spectatorLastValidMs
        && nowMs - g_spectatorLastValidMs <= 15000
        && status.phase != qnetplay::spectator::ViewerPhase::Failed
        && status.phase != qnetplay::spectator::ViewerPhase::Rejected;
    status.sawSourceContact = g_spectatorLastValidMs != 0;
    status.sourceContactAgeMs =
        (g_spectatorLastValidMs != 0 && nowMs >= g_spectatorLastValidMs)
            ? nowMs - g_spectatorLastValidMs
            : 0;
    {
        qnetplay::spectator::SetMatchIndexEntry match{};
        if (g_spectatorViewer->setTimeline().currentMatch(
                status.playbackNextRecord, match)) {
            status.currentMatchId = match.matchId;
        }
        qnetplay::spectator::HeartbeatPayload beat{};
        if (g_spectatorViewer->sourceStatus(beat)) {
            status.sourceFrontierKnown = true;
            status.sourceNextRecord = beat.nextRecord;
            status.sourceMatchId = beat.currentMatchId;
            status.sourcePhase = static_cast<uint8_t>(beat.sourcePhase);
            static const void* s_frontierViewer = nullptr;
            static uint32_t s_frontierRecord = 0;
            static uint64_t s_frontierSinceMs = 0;
            const void* const frontierIdentity = g_spectatorViewer.get();
            if (s_frontierViewer != frontierIdentity
                || s_frontierRecord != beat.nextRecord
                || s_frontierSinceMs == 0 || nowMs < s_frontierSinceMs) {
                s_frontierViewer = frontierIdentity;
                s_frontierRecord = beat.nextRecord;
                s_frontierSinceMs = nowMs;
            }
            status.sourceFrontierStaticMs = nowMs - s_frontierSinceMs;
            static const void* s_beatViewer = nullptr;
            static uint8_t s_beatPhase = 0;
            static uint32_t s_beatMatch = 0;
            static uint16_t s_beatViewers = 0;
            const uint8_t phase = static_cast<uint8_t>(beat.sourcePhase);
            const void* const viewerIdentity = g_spectatorViewer.get();
            if (s_beatViewer != viewerIdentity
                || phase != s_beatPhase || beat.currentMatchId != s_beatMatch
                || beat.viewerCount != s_beatViewers) {
                QLOG("[spectator-viewer] source heartbeat %s: phase=%u "
                     "next=%u checkpoint=%u match=%u viewers=%u "
                     "(playback=%u received=%u)",
                     s_beatViewer == viewerIdentity ? "changed" : "first",
                     static_cast<unsigned>(phase), beat.nextRecord,
                     beat.checkpointRecord, beat.currentMatchId,
                     static_cast<unsigned>(beat.viewerCount),
                     status.playbackNextRecord, status.receivedNextRecord);
                s_beatViewer = viewerIdentity;
                s_beatPhase = phase;
                s_beatMatch = beat.currentMatchId;
                s_beatViewers = beat.viewerCount;
            }
        }
        status.reorderedRecords = g_spectatorViewer->reorderedRecordCount();
        status.awaitingEventIndex = g_spectatorViewer->awaitingEventIndex();
        status.appliedEvents = g_spectatorViewer->appliedEventCount();
    }
    LeaveCriticalSection(&g_specCs);
    return true;
}

bool spectatorViewerGetGameStart(
    qnetplay::spectator::GameStartPayload& start) {
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    const bool ok = g_spectatorViewer->gameStart(start);
    LeaveCriticalSection(&g_specCs);
    return ok;
}

bool spectatorViewerPeekConfirmed(
    qnetplay::spectator::ConfirmedRecord& record) {
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    const bool ok = g_spectatorViewer->peekPlaybackRecord(record);
    LeaveCriticalSection(&g_specCs);
    return ok;
}

qnetplay::spectator::IngressResult spectatorViewerCommitConfirmed(
    uint32_t record,
    uint64_t observedPreStateHash,
    uint64_t observedPostStateHash) {
    if (!g_specCsInit || !g_spectatorViewer)
        return qnetplay::spectator::IngressResult::Failed;
    EnterCriticalSection(&g_specCs);
    const auto result = g_spectatorViewer->commitPlaybackRecord(
        record, observedPreStateHash, observedPostStateHash);
    LeaveCriticalSection(&g_specCs);
    return result;
}

bool spectatorViewerGetTerminal(
    qnetplay::spectator::ArchiveTerminal& terminal) {
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    const bool ok = g_spectatorViewer->terminal(terminal);
    LeaveCriticalSection(&g_specCs);
    return ok;
}

bool spectatorViewerGetRejection(
    qnetplay::spectator::RejectPayload& rejection) {
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    const bool ok = g_spectatorViewer->rejection(rejection);
    LeaveCriticalSection(&g_specCs);
    return ok;
}

bool spectatorViewerResume() {
    if (!g_specCsInit || !g_spectatorViewer) return false;
    EnterCriticalSection(&g_specCs);
    const bool ok = g_spectatorViewer->resume();
    LeaveCriticalSection(&g_specCs);
    return ok;
}

void netShowTerminalStatus(uint32_t code, uint32_t detail,
                           const char* reason, bool graceful) {
    char title[256] = {};
    if (graceful) {
        std::snprintf(
            title, sizeof(title),
            "QOH99 - opponent left the match | %.128s",
            reason ? reason : "opponent left");
    } else {
        std::snprintf(
            title, sizeof(title),
            "QOH99 NETPLAY STOPPED | local %s(%u) detail %u | "
            "%.112s | see logs",
            qnetplay::wire::terminalErrorCodeName(
                static_cast<qnetplay::wire::TerminalErrorCode>(code)),
            code, detail, reason ? reason : "runtime failure");
    }
    setGameWindowTitleBounded(title);
}

bool netRequestGameWindowClose() {
    CloseGameWindowsContext context{};
    (void)EnumWindows(
        closeGameWindowsCb, reinterpret_cast<LPARAM>(&context));
    if (context.posted != 0u) {
        QLOG("[netlink] requested clean game close (WM_CLOSE) "
             "windows=%u/%u", context.posted, context.matched);
        return true;
    }
    return false;
}

void netSendGoodbyeNow(uint32_t code, uint32_t detail) {
    if (!g_netCsInit || !g_playerCore) return;
    std::vector<qnetplay::OutboundDatagram> packets;
    EnterCriticalSection(&g_netCs);
    g_playerCore->setLocalTerminalError(
        static_cast<qnetplay::wire::TerminalErrorCode>(code), detail,
        "local game closed cleanly");
    packets = g_playerCore->pollOutbound(GetTickCount64());
    LeaveCriticalSection(&g_netCs);
    for (const qnetplay::OutboundDatagram& packet : packets) {
        const sockaddr_in destination = socketEndpoint(packet.destination);
        sendDatagramOn(g_udp, destination, packet.bytes,
                       TestNetworkDomain::Player, "netlink");
    }
    QLOG("[netlink] sent peer goodbye (code=%s(%u) detail=%u, "
         "%zu pkt) on clean quit",
         qnetplay::wire::terminalErrorCodeName(
             static_cast<qnetplay::wire::TerminalErrorCode>(code)),
         code, detail, packets.size());
}

void netSendInputs(uint32_t frame, uint16_t packed) {
    netSubmitLocalInput(frame, packed);
}

bool netGetRemote(uint32_t frame, uint16_t& out) {
    return netGetRemoteExact(frame, out);
}

void netGetLocalRing(uint32_t frame, uint16_t& out) {
    if (!netGetLocalExact(frame, out)) out = 0;
}

uint32_t netRemoteHighWater() {
    const uint32_t frame = netRemoteAckThrough();
    return frame == kNetNoFrame ? 0 : frame;
}

void netSetLocalReady(uint32_t seedGameplay,
                      uint32_t seedCosmetic,
                      uint32_t selector) {
    netSubmitLocalReady(NetReadyState{
        seedGameplay,
        seedCosmetic,
        selector,
        1,
    });
}

bool netBothReady(uint32_t& hostSeedG,
                  uint32_t& hostSeedC,
                  uint32_t& hostSel) {
    if (!netReadyComplete()) return false;
    NetReadyState remote{};
    if (!netGetRemoteReady(remote)) return false;

    if (g_cfg.isHost) {
        if (!g_hasCompatLocalReady) return false;
        hostSeedG = g_compatLocalReady.seedGameplay;
        hostSeedC = g_compatLocalReady.seedCosmetic;
        hostSel = g_compatLocalReady.selector;
    } else {
        hostSeedG = remote.seedGameplay;
        hostSeedC = remote.seedCosmetic;
        hostSel = remote.selector;
    }
    return true;
}

void netSendSyncHash(uint32_t frame,
                     uint32_t rngG,
                     uint32_t rngC,
                     uint32_t sel,
                     int32_t hp0,
                     int32_t hp1,
                     uint32_t stateCrc) {
    NetConfirmedHash hash{};
    hash.frame = frame;
    hash.rngGameplay = rngG;
    hash.rngCosmetic = rngC;
    hash.rngSelector = sel;
    hash.hp0 = hp0;
    hash.hp1 = hp1;
    hash.gameplayHash = stateCrc;
    netSubmitConfirmedHash(hash);
}

bool netTakeSyncHash(uint32_t& frame,
                     uint32_t& rngG,
                     uint32_t& rngC,
                     uint32_t& sel,
                     int32_t& hp0,
                     int32_t& hp1,
                     uint32_t& stateCrc) {
    NetConfirmedHash hash{};
    if (!netTakeConfirmedHash(hash)) return false;
    frame = hash.frame;
    rngG = hash.rngGameplay;
    rngC = hash.rngCosmetic;
    sel = hash.rngSelector;
    hp0 = hash.hp0;
    hp1 = hash.hp1;
    stateCrc = static_cast<uint32_t>(hash.gameplayHash);
    return true;
}

}
