#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <chrono>
#include <future>
#include "CrashDump.h"
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>
#include "../net/MonotonicTime.h"
#include "../net/PacingBudget.h"
#include "../net/Protocol.h"
#include "../net/Autopunch.h"
#include "../net/AutopunchPeerSet.h"
#include "../net/AutopunchSession.h"
#include "../net/RelayClient.h"
#include "../net/RelayRoute.h"
#include "../net/RouteRttEstimator.h"
#include "../net/Sockets.h"
#include "../net/PortMapper.h"
#include "../netplay/NetplayCore.h"
#include "../netplay/ConfigurationProvenance.h"
#include "../netplay/MatchConfig.h"
#include "../ipc/IpcConfig.h"
#include "../ipc/BuildContract.h"
#include "../game/SessionPresentation.h"
#include "../platform/CompatEnv.h"
#include "../platform/CompatPreamble.h"
#include "../platform/WidePath.h"
#include "../Log.h"
#include "../Paths.h"
#include "Launcher.h"
#include "../game/GameErrorJournal.h"
#include "../game/SessionLogHighlights.h"
#include "AntivirusPreflight.h"
#include "SessionLogArchive.h"
#include "../input/InputCompiler.h"
#include "../input/KeyboardScanCodec.h"
#include "../input/ControllerPreview.h"
#include "../input/DeviceMigration.h"
#include "../input/SdlLifecycle.h"
#include "../input/InputProfile.h"
#include "../input/ModeControlPolicy.h"
#include <fstream>
#include <iterator>
#include "ConsoleUi.h"
#include "PublicIp.h"
#include "SdlUi.h"
#include "WaveGain.h"
#include "i18n/Tr.h"
#include "RawInputCapture.h"
#include "FCommandText.h"
#include "replay/ReplayBrowserModel.h"
#include "replay/ReplayBroker.h"
#include "replay/ReplayBrokerV2.h"
#include "replay/ReplayCatalog.h"
#include "replay/ReplayFolderPlayback.h"
#include "replay/ReplayPlaybackOutcome.h"
#include "replay/ReplayCompatibility.h"
#include "replay/ReplayCompatibilityV2.h"
#include "replay/ReplayParser.h"
#include "replay/ReplayParserV2.h"
#include "replay/ReplayProductV2.h"
#include "replay/ReplayBattleLogFile.h"
#include "replay/ReplayBattleStats.h"
#include "replay/ReplayRecorderHost.h"
#include "replay/ReplaySetNaming.h"
#include "../replay/CharacterCatalog.h"
#include "../replay/ReplayIntegrity.h"
#include "../replay/ReplaySimulationContractV2.h"
#include "../net/ControlLink.h"
#include "../net/RouteCandidates.h"
#include "../net/SecureRandom.h"
#include "../platform/PeImage.h"
#include "../platform/WineSocketRepair.h"
#include "BuildIdentity.h"
#include "qoh99/generated/BuildProfiles.hpp"
#include "spectator/SpectatorRoute.h"
#include "spectator/SpectatorRelayAdapter.h"
#include "client/spectator/ResilientTopologyAuthority.h"
#include "client/spectator/ResilientTopologyClient.h"
#include "client/spectator/ReleasedClientRootControl.h"
#include "client/spectator/ReleasedTopologyControlV2.h"
#include "spectator/SpectatorSidecar.h"
#include "../spectator/SpectatorGrant.h"
#include <ctime>
#include "../relay/RelayServiceVerb.h"

using namespace qnet;

int qoh99_run_tests();

bool parsePortNumber(const std::string& text, uint16_t& port) {
    if (text.empty()) return false;
    unsigned long value = 0;
    for (const unsigned char ch : text) {
        if (ch < '0' || ch > '9') return false;
        const unsigned long digit = static_cast<unsigned long>(ch - '0');
        if (value > (65535ul - digit) / 10ul) return false;
        value = value * 10ul + digit;
    }
    if (value == 0) return false;
    port = static_cast<uint16_t>(value);
    return true;
}

uint16_t configuredPortOrDefault(int value, uint16_t fallback) {
    return value >= 1 && value <= 65535
        ? static_cast<uint16_t>(value)
        : fallback;
}

bool parseHostPort(const std::string& in, std::string& ip, uint16_t& port, uint16_t defPort) {
    std::string s = in;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty()) return false;
    size_t colon = s.find_last_of(':');
    std::string ipPart = (colon == std::string::npos) ? s : s.substr(0, colon);
    uint16_t outPort = defPort;
    if (colon != std::string::npos) {
        std::string portPart = s.substr(colon + 1);
        if (!parsePortNumber(portPart, outPort)) return false;
    }
    if (outPort == 0) return false;
    qnet::Endpoint ep;
    if (!qnet::Endpoint::parse(ipPart, outPort, ep)) return false;
    ip = ipPart;
    port = outPort;
    return true;
}

void initializeIpcPlayerIdentityDefaults(
    qipc::IpcConfig& config, const std::string& localName) {
    qgame::storeCanonicalPlayerNameForCodePage(
        config.localName, localName, CP_UTF8);
    qgame::storeCanonicalPlayerNameForCodePage(
        config.remoteName, "Player", CP_UTF8);
    config.localNameCodePage = CP_UTF8;
    config.remoteNameCodePage = CP_UTF8;
}

std::string canonicalUtf8Nickname(std::string_view value) {
    return qgame::canonicalPlayerNameForCodePage(value, CP_UTF8);
}

std::string encodeNicknameUtf8Hex(std::string_view value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 2u);
    for (const unsigned char byte : value) {
        encoded.push_back(kHex[byte >> 4u]);
        encoded.push_back(kHex[byte & 0x0Fu]);
    }
    return encoded;
}

bool decodeNicknameUtf8Hex(std::string_view encoded, std::string& out) {
    if (encoded.empty() || (encoded.size() & 1u) != 0u
        || encoded.size() > qgame::kPlayerNameDisplayBytes * 2u) {
        return false;
    }
    auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(encoded.size() / 2u);
    for (size_t i = 0; i < encoded.size(); i += 2u) {
        const int high = digit(encoded[i]);
        const int low = digit(encoded[i + 1u]);
        if (high < 0 || low < 0)
            return false;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    if (!qgame::isCanonicalPlayerNameForCodePage(decoded, CP_UTF8))
        return false;
    out = std::move(decoded);
    return true;
}

bool processArgumentUtf8(int expectedArgc, int index, std::string& out) {
    int wideArgc = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &wideArgc);
    if (wideArgv == nullptr)
        return false;
    const bool inRange = wideArgc == expectedArgc
        && index >= 0 && index < wideArgc;
    std::string converted;
    if (inRange)
        converted = qplat::narrow(std::wstring(wideArgv[index]));
    const bool convertedOk = inRange
        && (wideArgv[index][0] == L'\0' || !converted.empty());
    LocalFree(wideArgv);
    if (!convertedOk)
        return false;
    out = std::move(converted);
    return true;
}

bool parseSpectatorKey(const std::string& text, uint64_t& low, uint64_t& high) {
    return qclient::spectator::parseSpectateKeyHex(text, high, low);
}

bool qoh99SpectatorPreflightMayLaunch(
    const bool forcedRelay,
    const bool preflightOk,
    const bool inconclusive,
    const uint16_t routeLabel) {
    if (!forcedRelay) return true;
    return preflightOk
        && !inconclusive
        && routeLabel == static_cast<uint16_t>(
               qclient::spectator::SpectatorRouteLabel::Relay);
}

int qoh99ClassifyChildExit(const bool haveChildExit,
                           const DWORD childExit,
                           const DWORD lifecycleFailure) {
    if (haveChildExit && childExit != 0 && childExit != 1
        && childExit != STILL_ACTIVE) {
        return static_cast<int>(childExit);
    }
    if (lifecycleFailure != ERROR_SUCCESS)
        return static_cast<int>(lifecycleFailure);
    if (haveChildExit && (childExit == 0 || childExit == 1)) return 0;
    return static_cast<int>(ERROR_GEN_FAILURE);
}

int qoh99PreferNativeChildExit(const int childExit,
                               const int semanticFailure) {
    return childExit != 0 ? childExit : semanticFailure;
}

uint16_t qoh99ReplayStartupFlags(uint16_t presentationFlags,
                                 bool pauseAtStart, bool headless) {
    if (headless) return 0u;
    return presentationFlags | (pauseAtStart ? 0u : qipc::kReplayAutoStart);
}

bool qoh99SpectatorAdmissionReleaseRequired(const bool routeOk,
                                             const uint64_t grantId,
                                             const uint64_t viewerId) {
    return routeOk && grantId != 0 && viewerId != 0;
}

std::string qoh99SpectatorE0SourceEvidenceLine(
    const qclient::spectator::SidecarStatus& status) {
    namespace qs = qclient::spectator;
    std::string line = "[spectator-e0] source event=status phase=";
    line += qs::sidecarPhaseName(status.phase);
    line += " serving=" + std::to_string(status.serving ? 1 : 0);
    line += " confirmed_cursor=" + std::to_string(status.confirmedRecords);
    line += " first_available_cursor="
        + std::to_string(status.firstAvailableRecord);
    line += " viewers=" + std::to_string(status.viewerCount);
    line += " capacity=" + std::to_string(status.viewerCapacity);
    return line;
}

namespace {

std::string spectatorE0EndpointText(
    const qclient::spectator::RouteEndpoint& endpoint) {
    if (!endpoint.valid()) return "unavailable";
    return qnet::formatIpv4BE(endpoint.ipv4BE) + ":"
        + std::to_string(endpoint.port);
}

const char* spectatorE0RouteLabelText(const uint16_t label) {
    using qclient::spectator::SpectatorRouteLabel;
    switch (static_cast<SpectatorRouteLabel>(label)) {
    case SpectatorRouteLabel::None: return "none";
    case SpectatorRouteLabel::LanDirect: return "lan-direct";
    case SpectatorRouteLabel::PublicDirect: return "public-direct";
    case SpectatorRouteLabel::PublicDirectHairpin:
        return "public-direct-hairpin";
    case SpectatorRouteLabel::Punched: return "punched";
    case SpectatorRouteLabel::Relay: return "relay";
    }
    return "unknown";
}

const char* spectatorE0DisconnectReasonText(
    const qclient::spectator::ViewerServiceState state) {
    using qclient::spectator::ViewerServiceState;
    switch (state) {
    case ViewerServiceState::Completed: return "completed";
    case ViewerServiceState::DeadlineExpired: return "deadline-expired";
    case ViewerServiceState::Detached: return "detached";
    case ViewerServiceState::Kicked: return "kicked";
    default: return "unavailable";
    }
}

std::string spectatorE0ViewerFields(
    const qclient::spectator::ViewerStatus& viewer) {
    namespace qs = qclient::spectator;
    char viewerId[17]{};
    std::snprintf(viewerId, sizeof(viewerId), "%016llX",
                  static_cast<unsigned long long>(viewer.viewerId));
    std::string fields = "viewer=";
    fields += viewerId;
    fields += " generation=" + std::to_string(viewer.generation);
    fields += " route_label=";
    fields += spectatorE0RouteLabelText(viewer.routeLabel);
    fields += " route_label_id=" + std::to_string(viewer.routeLabel);
    fields += " endpoint=" + spectatorE0EndpointText(viewer.endpoint);
    fields += " endpoint_provenance=source-observed";
    fields += " state=";
    fields += qs::viewerServiceStateName(viewer.state);
    fields += " bootstrap_cursor=" + std::to_string(viewer.bootstrapRecord);
    fields += " ack_cursor=" + std::to_string(viewer.acknowledgedRecord);
    fields += " backlog_records=" + std::to_string(viewer.backlogRecords);
    fields += " bytes_sent=" + std::to_string(viewer.bytesSent);
    return fields;
}

}

std::string qoh99SpectatorE0ViewerEvidenceLine(
    const qclient::spectator::ViewerStatus& viewer) {
    return "[spectator-e0] qsp event=status qsp_hello=accepted "
        "qsp_capability=accepted compatibility=accepted "
        "admission=source-accepted "
        + spectatorE0ViewerFields(viewer);
}

std::string qoh99SpectatorE0ViewerAttachEvidenceLine(
    const qclient::spectator::ViewerStatus& viewer) {
    return "[spectator-e0] qsp event=hello-accepted "
        "qsp_proof=accepted qsp_capability=accepted "
        "compatibility=accepted admission=source-accepted "
        + spectatorE0ViewerFields(viewer);
}

std::string qoh99SpectatorE0ViewerDisconnectEvidenceLine(
    const qclient::spectator::ViewerStatus& viewer,
    const qclient::spectator::ViewerTerminalOutcome* outcome) {
    namespace qs = qclient::spectator;
    const bool exactOutcome = outcome != nullptr
        && outcome->viewerId == viewer.viewerId
        && outcome->generation == viewer.generation;
    std::string line = "[spectator-e0] qsp event=viewer-not-active ";
    line += exactOutcome
        ? "disconnect_evidence=terminal-outcome "
        : "disconnect_evidence=terminal-outcome-unavailable ";
    line += spectatorE0ViewerFields(viewer);
    if (!exactOutcome) return line + " disconnect_reason=unavailable";
    line += " disconnect_reason=";
    line += spectatorE0DisconnectReasonText(outcome->state);
    line += " terminal_ack_cursor="
        + std::to_string(outcome->acknowledgedRecord);
    return line;
}

namespace {

class SpectatorAdmissionReleaseGuard {
public:
    SpectatorAdmissionReleaseGuard(bool armed, std::function<void()> release)
        : _armed(armed), _release(std::move(release)) {}

    ~SpectatorAdmissionReleaseGuard() { release(); }

    SpectatorAdmissionReleaseGuard(
        const SpectatorAdmissionReleaseGuard&) = delete;
    SpectatorAdmissionReleaseGuard& operator=(
        const SpectatorAdmissionReleaseGuard&) = delete;

    void release() noexcept {
        if (!_armed) return;
        _armed = false;
        if (_release) _release();
    }

private:
    bool _armed = false;
    std::function<void()> _release;
};

}

bool qoh99SpectatorAdmissionReleaseGuardSelfTest() {
    int calls = 0;
    {
        SpectatorAdmissionReleaseGuard guard(
            true, [&calls]() { ++calls; });
    }
    if (calls != 1) return false;
    {
        SpectatorAdmissionReleaseGuard guard(
            false, [&calls]() { ++calls; });
    }
    if (calls != 1) return false;
    {
        SpectatorAdmissionReleaseGuard guard(
            true, [&calls]() { ++calls; });
        guard.release();
        guard.release();
    }
    return calls == 2;
}

bool buildSpectateAddress(const std::string& publicIpv4,
                          const uint32_t* localIpv4BE, size_t localCount,
                          uint16_t port, uint32_t sessionId,
                          uint64_t keyLow, uint64_t keyHigh,
                          qnet::SpectateAddress& out,
                          std::string& error) {
    error.clear();
    if (port == 0 || sessionId == 0 || (keyLow == 0 && keyHigh == 0)) {
        error = "invalid spectator session, port, or key";
        return false;
    }

    (void)publicIpv4;

    qnet::SpectateAddress address{};
    address.sessionId = sessionId;
    address.hasKey = true;
    address.keyLow = keyLow;
    address.keyHigh = keyHigh;

    qnet::CandidateEndpoint privateEndpoints[qnet::kMaxPrivateCandidates]{};
    size_t privateCount = qnet::collectPrivateCandidates(
        localIpv4BE, localCount, qnet::swapPort16(port), privateEndpoints,
        qnet::kMaxPrivateCandidates);

    uint32_t loopbackBE = 0;
    const char loopbackText[] = "127.0.0.1";
    if (!qnet::parseIpv4Text(loopbackText, sizeof(loopbackText) - 1,
                            loopbackBE)) {
        error = "internal loopback address is invalid";
        return false;
    }
    bool haveLoopback = false;
    for (size_t i = 0; i < privateCount; ++i) {
        if (privateEndpoints[i].ipv4BE == loopbackBE) {
            haveLoopback = true;
            break;
        }
    }
    if (!haveLoopback) {
        qnet::CandidateEndpoint loopback{};
        loopback.kind = qnet::CandidateKind::PrivateLan;
        loopback.ipv4BE = loopbackBE;
        loopback.portBE = qnet::swapPort16(port);
        if (privateCount < qnet::kMaxPrivateCandidates) {
            privateEndpoints[privateCount++] = loopback;
        } else {
            privateEndpoints[qnet::kMaxPrivateCandidates - 1] = loopback;
        }
    }
    for (size_t i = 0; i < privateCount; ++i)
        address.endpoints[address.endpointCount++] = privateEndpoints[i];

    if (qnet::validateCandidateSet(address.endpoints, address.endpointCount)
        != qnet::CandidateSetError::None) {
        error = "generated spectator candidates failed validation";
        return false;
    }
    out = address;
    return true;
}

const qnet::CandidateEndpoint* preferredSpectatorSourceEndpoint(
    const qnet::SpectateAddress& address) {
    for (size_t i = 0; i < address.endpointCount; ++i) {
        if (address.endpoints[i].kind == qnet::CandidateKind::PublicObserved)
            return &address.endpoints[i];
    }
    for (size_t i = 0; i < address.endpointCount; ++i) {
        if (address.endpoints[i].kind == qnet::CandidateKind::PrivateLan
            && !qnet::isLoopbackBE(address.endpoints[i].ipv4BE)) {
            return &address.endpoints[i];
        }
    }
    return address.endpointCount != 0 ? &address.endpoints[0] : nullptr;
}

namespace {

using ConfigProvenanceStage = qnetplay::config_provenance::Stage;

std::string g_sessionDir;
qclient::SessionLogWriterLease g_sessionLogWriterLease{};
std::string g_localName = "Player";
qclient::RuntimeInstallationIdentity g_preflightInstallation{};
std::string g_preflightInstallationPath;
bool g_havePreflightInstallation = false;
struct GameFilesCheck {
    bool ok = false;
    std::string path;
    std::string error;
};
GameFilesCheck g_gameFiles;
bool g_gameFilesChecked = false;
std::string g_replayContentManifestDirectory;
bool g_devMode = false;
uint32_t g_spectatorRetainDiagnosticMs = 0;

struct RelayOptions {
    bool enabled = false;
    bool forceRelay = false;
    bool automaticAlias = false;
    qnet::Endpoint server{};
    qnet::relay::RoomId room{};
    bool roomProvided = false;
};

constexpr uint16_t kDefaultRelayPort = 14764;

struct RelayRuntime {
    qnet::relay::PairResult pair{};
    bool forceRelay = false;
};

void emitSessionPreamble();

void initSessionLog() {
    const qclient::SessionLogArchiveRecovery recovered =
        qclient::recoverAbandonedSessionLogDirectories(qpath::logsRoot());
    const qclient::SessionLogRetentionResult pruned =
        qclient::pruneAgedSessionLogs(qpath::logsRoot());
    g_sessionDir = qpath::makeSessionDir();
    g_sessionLogWriterLease = qclient::acquireSessionLogWriterLease(
        g_sessionDir);
    qoh99::Log::get().openUtf8(g_sessionDir + "\\client.log");
    qcrash::install(g_sessionDir, "caster");
    QLOG("qoh99caster client log  built " __DATE__ " " __TIME__ "  session=%s", g_sessionDir.c_str());
    if (g_sessionLogWriterLease.markerPath.empty()) {
        QLOG("[log-archive] caster writer marker unavailable; this session "
             "will be retained if an unexpected exit prevents normal cleanup");
    }
    if (recovered.inspected != 0u) {
        QLOG("[log-archive] startup recovery inspected=%u archived=%u "
             "deferred=%u retained=%u",
             recovered.inspected, recovered.archived, recovered.deferred,
             recovered.retained);
    }
    if (pruned.inspected != 0u) {
        QLOG("[log-retention] older than %u day(s): inspected=%u "
             "folders=%u archives=%u kept-in-use=%u failed=%u",
             qclient::kSessionLogRetentionDays, pruned.inspected,
             pruned.removedDirectories, pruned.removedArchives,
             pruned.keptInUse, pruned.failed);
    }
    emitSessionPreamble();
}

struct Settings {
    std::string nickname = "Player";
    std::string gamePath;
    std::string dllPath = "qoh99_netplay.dll";
    uint16_t    port     = 7500;
    int         delay    = 3;
    int         rollback = 0;
    ConfigProvenanceStage delayProvenance = ConfigProvenanceStage::Default;
    ConfigProvenanceStage rollbackProvenance = ConfigProvenanceStage::Default;
    std::string joinIp   = "127.0.0.1";
    uint16_t    joinPort = 7500;
    int         musicVolume = 100;
    int         soundVolume = 100;
    bool        hideNativeScore = true;
    bool        hideNativeTimer = true;
    bool        disableExtraFunctions = true;
    int         authMaxRollback = qnet::kCapabilityRollback;
    int         authMaxDelay    = qnet::kCapabilityDelay;

    // Stored as qclient::spectator::AdmissionPolicy: 1 Public, 2 Ask me, 4 Off; value 3 migrates to Ask me.
    int         spectatorAdmission = 1;
    int         spectatorMaxViewers = 32;
    int         spectatorPort = 0;
    bool        clientPrivateSpectatorPublisher = false;

    int         language = 0;
    bool        recordNetplay = true;
    bool        recordLocalVs = true;
    bool        saveSpectatorSessions = true;
    bool        battleLog = true;
    std::string replayRoot;
    bool        showIncompleteReplays = true;
    bool        showTrashedReplays = false;
    bool        showReplayNetworkDetails = true;
    int         replayFlushSeconds = 2;
    int         replayStorageWarnMb = 4096;
    int         replayRetentionDays = 0;
    bool        replayDefaultAudio = true;
    bool        replayPauseAtStart = false;

    void load() {
        std::string storedUtf8;
        if (decodeNicknameUtf8Hex(
                qui::config::getStr("nicknameUtf8Hex"), storedUtf8)) {
            nickname = storedUtf8;
        } else {
            nickname = canonicalUtf8Nickname(qgame::playerNameForUtf8Display(
                qui::config::getStr("nickname", nickname), GetACP()));
        }
        gamePath = qui::config::getStr("gamePath", gamePath);
        dllPath  = qui::config::getStr("dllPath", dllPath);
        port     = configuredPortOrDefault(
            qui::config::getInt("port", port), port);
        delay    = qui::config::getInt("delay", delay);
        rollback = qui::config::getInt("rollback", rollback);
        joinIp   = qui::config::getStr("joinIp", joinIp);
        joinPort = configuredPortOrDefault(
            qui::config::getInt("joinPort", joinPort), joinPort);
        musicVolume = qui::config::getInt("musicVolume", musicVolume);
        if (musicVolume < 0 || musicVolume > 100) musicVolume = 100;
        soundVolume = qui::config::getInt("soundVolume", soundVolume);
        if (soundVolume < 0 || soundVolume > 100) soundVolume = 100;
        hideNativeScore = qui::config::getInt(
            "hideNativeScore", hideNativeScore ? 1 : 0) != 0;
        hideNativeTimer = qui::config::getInt(
            "hideNativeTimer", hideNativeTimer ? 1 : 0) != 0;
        disableExtraFunctions = qui::config::getInt(
            "disableExtraFunctions", disableExtraFunctions ? 1 : 0) != 0;
        authMaxRollback = qui::config::getInt(
            "authMaxRollback", authMaxRollback);
        authMaxDelay = qui::config::getInt("authMaxDelay", authMaxDelay);
        constexpr int kPreviousCapabilityRollback = 15;
        if (authMaxRollback == kPreviousCapabilityRollback)
            authMaxRollback = qnet::kCapabilityRollback;
        if (authMaxRollback < 0 || authMaxRollback > qnet::kCapabilityRollback)
            authMaxRollback = qnet::kCapabilityRollback;
        if (authMaxDelay < 0 || authMaxDelay > qnet::kCapabilityDelay)
            authMaxDelay = qnet::kCapabilityDelay;
        spectatorAdmission =
            qui::config::getInt("spectatorAdmission", spectatorAdmission);
        if (spectatorAdmission == 3) spectatorAdmission = 2;
        if (spectatorAdmission < 1 || spectatorAdmission > 4)
            spectatorAdmission = 1;
        spectatorMaxViewers =
            qui::config::getInt("spectatorMaxViewers", spectatorMaxViewers);
        if (spectatorMaxViewers < 1 || spectatorMaxViewers > 256)
            spectatorMaxViewers = 32;
        spectatorPort = qui::config::getInt("spectatorPort", spectatorPort);
        if (spectatorPort < 0 || spectatorPort > 65535) spectatorPort = 0;
        clientPrivateSpectatorPublisher = qui::config::getInt(
            "clientPrivateSpectatorPublisher",
            clientPrivateSpectatorPublisher ? 1 : 0) != 0;
        language = qui::config::getInt("language", language);
        if (language != 0 && language != 1) language = 0;
        recordNetplay =
            qui::config::getInt("recordNetplay", recordNetplay ? 1 : 0) != 0;
        recordLocalVs =
            qui::config::getInt("recordLocalVs", recordLocalVs ? 1 : 0) != 0;
        saveSpectatorSessions = qui::config::getInt(
            "saveSpectatorSessions", saveSpectatorSessions ? 1 : 0) != 0;
        battleLog = qui::config::getInt("battleLog", battleLog ? 1 : 0) != 0;
        replayRoot = qui::config::getStr("replayRoot", replayRoot);
        showIncompleteReplays = qui::config::getInt(
            "showIncompleteReplays", showIncompleteReplays ? 1 : 0) != 0;
        showTrashedReplays = qui::config::getInt(
            "showTrashedReplays", showTrashedReplays ? 1 : 0) != 0;
        showReplayNetworkDetails = qui::config::getInt(
            "showReplayNetworkDetails",
            showReplayNetworkDetails ? 1 : 0) != 0;
        replayFlushSeconds =
            qui::config::getInt("replayFlushSeconds", replayFlushSeconds);
        if (replayFlushSeconds < 1 || replayFlushSeconds > 60)
            replayFlushSeconds = 2;
        replayStorageWarnMb =
            qui::config::getInt("replayStorageWarnMb", replayStorageWarnMb);
        if (replayStorageWarnMb < 0 || replayStorageWarnMb > 1024 * 1024)
            replayStorageWarnMb = 4096;
        replayRetentionDays = 0;
        replayDefaultAudio = qui::config::getInt(
            "replayDefaultAudio", replayDefaultAudio ? 1 : 0) != 0;
        replayPauseAtStart = qui::config::getInt("replayPauseAtStart", 0) != 0;
    }
    static void dropRetiredKeys() {
        static const char* const kRetired[] = {
            "relayAddress",
            "replayDefaultSpeedPermille",
        };
        for (const char* key : kRetired) qui::config::removeKey(key);
    }

    void save() const {
        dropRetiredKeys();
        const std::string utf8Nickname = canonicalUtf8Nickname(nickname);
        qui::config::setStr(
            "nicknameUtf8Hex", encodeNicknameUtf8Hex(utf8Nickname));
        const bool ascii = std::all_of(
            utf8Nickname.begin(), utf8Nickname.end(),
            [](unsigned char byte) { return byte < 0x80u; });
        qui::config::setStr("nickname", ascii ? utf8Nickname : "Player");
        qui::config::setStr("gamePath", gamePath);
        qui::config::setStr("dllPath", dllPath);
        qui::config::setInt("port", port);
        qui::config::setInt("delay", delay);
        qui::config::setInt("rollback", rollback);
        qui::config::setStr("joinIp", joinIp);
        qui::config::setInt("joinPort", joinPort);
        qui::config::setInt("musicVolume", musicVolume);
        qui::config::setInt("soundVolume", soundVolume);
        qui::config::setInt("hideNativeScore", hideNativeScore ? 1 : 0);
        qui::config::setInt("hideNativeTimer", hideNativeTimer ? 1 : 0);
        qui::config::setInt("disableExtraFunctions", disableExtraFunctions ? 1 : 0);
        qui::config::setInt("authMaxRollback", authMaxRollback);
        qui::config::setInt("authMaxDelay", authMaxDelay);
        qui::config::setInt("spectatorAdmission", spectatorAdmission);
        qui::config::setInt("spectatorMaxViewers", spectatorMaxViewers);
        qui::config::setInt("spectatorPort", spectatorPort);
        qui::config::setInt("clientPrivateSpectatorPublisher",
                            clientPrivateSpectatorPublisher ? 1 : 0);
        qui::config::setInt("language", language);
        qui::config::setInt("recordNetplay", recordNetplay ? 1 : 0);
        qui::config::setInt("recordLocalVs", recordLocalVs ? 1 : 0);
        qui::config::setInt("saveSpectatorSessions",
                            saveSpectatorSessions ? 1 : 0);
        qui::config::setInt("battleLog", battleLog ? 1 : 0);
        qui::config::setStr("replayRoot", replayRoot);
        qui::config::setInt("showIncompleteReplays",
                            showIncompleteReplays ? 1 : 0);
        qui::config::setInt("showTrashedReplays", showTrashedReplays ? 1 : 0);
        qui::config::setInt("showReplayNetworkDetails",
                            showReplayNetworkDetails ? 1 : 0);
        qui::config::setInt("replayFlushSeconds", replayFlushSeconds);
        qui::config::setInt("replayStorageWarnMb", replayStorageWarnMb);
        qui::config::setInt("replayRetentionDays", replayRetentionDays);
        qui::config::setInt("replayDefaultAudio", replayDefaultAudio ? 1 : 0);
        qui::config::setInt("replayPauseAtStart", replayPauseAtStart ? 1 : 0);
    }
};

std::wstring replayRootOf(const Settings& s) {
    if (!s.replayRoot.empty()) {
        const std::wstring configured = qplat::widen(s.replayRoot);
        if (!configured.empty()) return configured;
    }
    return qplat::replayRootW();
}

qclient::spectator::AdmissionPolicy admissionPolicyOf(const Settings& s) {
    switch (s.spectatorAdmission) {
        case 2: return qclient::spectator::AdmissionPolicy::AskHost;
        case 4: return qclient::spectator::AdmissionPolicy::Disabled;
        default: break;
    }
    return qclient::spectator::AdmissionPolicy::Public;
}

const char* admissionPolicyName(qclient::spectator::AdmissionPolicy policy) {
    switch (policy) {
        case qclient::spectator::AdmissionPolicy::Public:
            return "anyone with the host address";
        case qclient::spectator::AdmissionPolicy::AskHost:
            return "ask me first";
        case qclient::spectator::AdmissionPolicy::CodeOnly:
            return "private internal service";
        case qclient::spectator::AdmissionPolicy::Disabled:
            return "off (no spectator port)";
    }
    return "unknown";
}

struct RecordingPlan {
    bool enabled = false;
    uint32_t generation = 0;
    uint64_t nonce = 0;
    std::string nameHint;
    int8_t battleSide = qclient::replay::kBattleSideSpectated;
    std::string offReason;
};

RecordingPlan planRecording(const Settings& s, const qipc::IpcConfig& cfg) {
    RecordingPlan plan;
    if (cfg.mode == qipc::MODE_REPLAY) return plan;
    if (cfg.trainingEnabled != 0) return plan;
    if (cfg.probeMode != 0 || cfg.mode == qipc::MODE_SELFTEST) return plan;

    bool wanted = false;
    switch (cfg.mode) {
        case qipc::MODE_NETPLAY:
            wanted = s.recordNetplay;
            plan.nameHint = "netplay";
            if (cfg.localPlayer == 1u || cfg.localPlayer == 2u) {
                plan.battleSide = static_cast<int8_t>(cfg.localPlayer - 1u);
            }
            if (!wanted) plan.offReason = "auto-record netplay is off";
            break;
        case qipc::MODE_SPECTATOR:
            wanted = s.saveSpectatorSessions;
            plan.nameHint = "spectator";
            if (!wanted) plan.offReason = "saving spectator sessions is off";
            break;
        case qipc::MODE_OBSERVE:
            if (cfg.vsRow == qipc::kVsRow1vCpu) {
                plan.offReason = "VS CPU matches are not recorded";
                return plan;
            } else {
                wanted = s.recordLocalVs;
                plan.nameHint = "local";
                plan.battleSide = qclient::replay::kBattleSideOffline;
                if (!wanted) plan.offReason = "auto-record local VS is off";
            }
            break;
        default:
            return plan;
    }
    if (!wanted) return plan;

    bool degradedRandom = false;
    uint32_t generation = static_cast<uint32_t>(
        qnet::secureRandom64(degradedRandom) & 0xFFFFFFFFull);
    if (generation == 0) generation = 1;
    uint64_t nonce = qnet::secureRandom64(degradedRandom);
    if (nonce == 0) {
        plan.offReason = "the recorder channel nonce could not be generated";
        return plan;
    }
    if (degradedRandom)
        QLOG("[replay.record] channel credentials used the last-resort RNG path");
    plan.enabled = true;
    plan.generation = generation;
    plan.nonce = nonce;
    return plan;
}

bool autopunchRelayEndpoint(Endpoint& endpoint, std::string& error) {
    char overrideText[128] = {};
    const DWORD overrideLength = GetEnvironmentVariableA(
        "QOH99_PUNCH_RELAY_OVERRIDE", overrideText,
        static_cast<DWORD>(sizeof(overrideText)));
    if (overrideLength > 0 && overrideLength < sizeof(overrideText)) {
        std::string ip;
        uint16_t port = 0;
        if (!parseHostPort(std::string(overrideText), ip, port,
                           qnet::autopunch::kRelayPort)
            || !Endpoint::parse(ip, port, endpoint)) {
            error = "Invalid QOH99_PUNCH_RELAY_OVERRIDE '"
                  + std::string(overrideText) + "'";
            return false;
        }
        error.clear();
        return true;
    }
    ResolvedEndpoint resolved{};
    ResolveError resolveError = ResolveError::None;
    std::string resolveDetail;
    if (!resolveEndpoint(qnet::autopunch::kRelayHost,
                         qnet::autopunch::kRelayPort, kDefaultResolveTimeoutMs,
                         resolved, resolveError, resolveDetail)) {
        error = std::string("Could not resolve the hole-punch relay '")
              + qnet::autopunch::kRelayHost + "': "
              + (resolveDetail.empty() ? resolveErrorText(resolveError)
                                       : resolveDetail);
        return false;
    }
    endpoint = resolved.endpoint;
    error.clear();
    return true;
}

bool configuredRelayServer(const Settings& settings, Endpoint& endpoint,
                           std::string& error) {
    (void)settings;
    char overrideText[128] = {};
    const DWORD overrideLength = GetEnvironmentVariableA(
        "QOH99_RELAY_OVERRIDE", overrideText,
        static_cast<DWORD>(sizeof(overrideText)));
    if (overrideLength == 0 || overrideLength >= sizeof(overrideText)) {
        error = "no traffic relay is configured (set QOH99_RELAY_OVERRIDE to "
                "run one); hole punching is used instead";
        return false;
    }
    std::string ip;
    uint16_t port = 0;
    if (!parseHostPort(std::string(overrideText), ip, port, kDefaultRelayPort)
        || !Endpoint::parse(ip, port, endpoint)) {
        error = "Invalid QOH99_RELAY_OVERRIDE '" + std::string(overrideText)
              + "'";
        return false;
    }
    error.clear();
    return true;
}

struct HostControlEndpoint {
    UdpSock socket;
    qnet::UdpControlListener listener;
    UdpSock sidecarControlSocket;
    qnet::UdpControlListener sidecarControlListener;
    bool sidecarControlIngress = false;
    qnet::autopunch::AutopunchSession punch;
    qnet::autopunch::AutopunchPeerSet punchPeers;
    bool punchReady = false;
    std::string punchStatus;
    uint64_t serviceCalls = 0;
    uint64_t nextServiceEvidenceMs = 0;

    ~HostControlEndpoint() {
        if (sidecarControlIngress)
            qclient::spectator::setSidecarControlIngressEnabled(false);
    }

    bool open(uint16_t advertisedPort, std::string& error) {
        if (!socket.open(advertisedPort)) {
            error = "UDP port " + std::to_string(advertisedPort)
                  + " is already in use - another app may own it (another netplay tool "
                    "uses 10800 by default). Close it or pick another port.\n"
                    "Check: netstat -ano | findstr "
                  + std::to_string(advertisedPort);
            return false;
        }
        listener.open(socket);
        listener.setForeignSink(
            [this](const uint8_t* bytes, size_t length, const Endpoint& from) {
                const uint64_t nowMs = nowUs() / 1000ull;
                qnet::autopunch::PunchMappingResult mappingResult{};
                if (punchPeers.handleInboundMapping(
                        bytes, length, from, nowMs, mappingResult)) {
                    qnet::autopunch::Mapping mapping{};
                    if (qnet::autopunch::decodeMapping(
                            bytes, length, mapping)
                        && (mappingResult
                                == qnet::autopunch::PunchMappingResult::AcceptedNew
                            || mappingResult
                                == qnet::autopunch::PunchMappingResult::Refreshed
                            || mappingResult
                                == qnet::autopunch::PunchMappingResult::Rebound)) {
                        Endpoint peer{};
                        peer.sa.sin_addr.s_addr = mapping.ipv4NetworkOrder;
                        peer.sa.sin_port = htons(mapping.natPort);
                        QLOG("[host-control] punch mapping peer=%s result=%u "
                             "tracked=%zu internalPort=%u",
                             peer.str().c_str(),
                             static_cast<unsigned>(mappingResult),
                             punchPeers.size(),
                             static_cast<unsigned>(mapping.internalPort));
                    } else {
                        QLOG("[host-control] punch mapping rejected result=%u "
                             "tracked=%zu",
                             static_cast<unsigned>(mappingResult),
                             punchPeers.size());
                    }
                    punchPeers.service(
                        [](void* context, const Endpoint& destination,
                           const void* payload, size_t payloadLength) -> bool {
                            return static_cast<UdpSock*>(context)->sendTo(
                                destination, payload, payloadLength);
                        },
                        &socket, nowMs);
                    return;
                }
                (void)punchPeers.handleProbe(bytes, length, from, nowMs);
            });

        Endpoint relay{};
        std::string relayError;
        if (autopunchRelayEndpoint(relay, relayError)) {
            punch.configure(advertisedPort, relay);
            punchPeers.configure(relay);
            punchReady = punch.configured() && punchPeers.configured();
            punchStatus = punchReady
                ? ("registered with " + std::string(qnet::autopunch::kRelayHost))
                : std::string("could not register with the hole-punch relay");
        } else {
            punchStatus = relayError;
        }
        QLOG("[host-control] listening on UDP %u punch=%d (%s)",
             static_cast<unsigned>(advertisedPort), punchReady ? 1 : 0,
             punchStatus.c_str());
        error.clear();
        return true;
    }

    bool openSidecarControlIngress(UdpSock& source, std::string& warning) {
        warning.clear();
        if (!source.isOpen() || source.boundPort() == 0) {
            warning = "the spectator source socket is not bound";
            return false;
        }
        WSAPROTOCOL_INFOW protocolInfo{};
        if (WSADuplicateSocketW(source.raw(), GetCurrentProcessId(),
                                &protocolInfo) != 0) {
            warning = "could not duplicate the spectator source socket (WSA "
                    + std::to_string(WSAGetLastError()) + ")";
            return false;
        }
        (void)qplat::repairDuplicatedUdpProtocolInfo(protocolInfo);
        SOCKET duplicate = WSASocketW(FROM_PROTOCOL_INFO,
                                      FROM_PROTOCOL_INFO,
                                      FROM_PROTOCOL_INFO,
                                      &protocolInfo, 0, 0);
        if (duplicate == INVALID_SOCKET) {
            warning = "could not open the spectator control bridge (WSA "
                    + std::to_string(WSAGetLastError()) + ")";
            return false;
        }
        if (!sidecarControlSocket.adopt(duplicate)
            || sidecarControlSocket.boundPort() != source.boundPort()) {
            warning = "the spectator control bridge did not retain its source port";
            sidecarControlSocket.close();
            return false;
        }
        sidecarControlListener.openInjected(
            sidecarControlSocket,
            [](qnet::UdpControlListener& injected) {
                constexpr size_t kDrainBudget = 64;
                qclient::spectator::SidecarControlDatagram datagram{};
                for (size_t i = 0; i < kDrainBudget
                     && qclient::spectator::pollSidecarControlDatagram(
                         datagram); ++i) {
                    injected.offerDatagram(datagram.bytes.data(),
                                           datagram.length, datagram.from);
                }
            });
        qclient::spectator::setSidecarControlIngressEnabled(true);
        sidecarControlIngress = true;
        QLOG("[host-control] sidecar ingress armed local=%u receiveOwner=sidecar",
             static_cast<unsigned>(sidecarControlSocket.boundPort()));
        return true;
    }

    qnet::UdpControlLink* acceptOne() {
        if (qnet::UdpControlLink* link = listener.acceptOne()) return link;
        return sidecarControlListener.acceptOne();
    }

    void release(qnet::UdpControlLink* link) {
        listener.release(link);
        sidecarControlListener.release(link);
    }

    void service(int waitMs = 0) {
        const uint64_t nowMs = nowUs() / 1000ull;
        ++serviceCalls;
        if (nowMs >= nextServiceEvidenceMs) {
            const qnet::autopunch::SessionStats& stats = punch.stats();
            QLOG("[host-control] service alive local=%u calls=%llu "
                 "registers=%u trackedMappings=%zu datagrams=%llu "
                 "hellos=%llu foreign=%llu links=%zu capacityRejects=%u "
                 "sidecarIngress=%d sidecarDatagrams=%llu sidecarHellos=%llu "
                 "sidecarLinks=%zu",
                 static_cast<unsigned>(socket.boundPort()),
                 static_cast<unsigned long long>(serviceCalls),
                 stats.registersSent, punchPeers.size(),
                 static_cast<unsigned long long>(
                     listener.datagramsReceived()),
                 static_cast<unsigned long long>(
                     listener.helloDatagramsReceived()),
                 static_cast<unsigned long long>(
                     listener.foreignDatagramsReceived()),
                 listener.liveLinks(), listener.rejectedForCapacity(),
                 sidecarControlIngress ? 1 : 0,
                 static_cast<unsigned long long>(
                     sidecarControlListener.datagramsReceived()),
                 static_cast<unsigned long long>(
                     sidecarControlListener.helloDatagramsReceived()),
                 sidecarControlListener.liveLinks());
            nextServiceEvidenceMs = nowMs + 5000ull;
        }
        if (punchReady) {
            const auto send =
                [](void* context, const Endpoint& destination,
                   const void* bytes, size_t length) -> bool {
                    return static_cast<UdpSock*>(context)->sendTo(
                        destination, bytes, length);
                };
            punch.service(send, &socket, nowMs);
            punchPeers.service(send, &socket, nowMs);
        }
        listener.pump(waitMs);
        if (sidecarControlIngress) sidecarControlListener.pump(0);
    }
};

bool escPressedFocused();

bool dialHostControl(UdpSock& socket, const Endpoint& advertised,
                     qnet::UdpControlLink& link, qui::Console* con,
                     int budgetMs, std::string& error,
                     const char* purpose = "player") {
    Endpoint relay{};
    std::string relayError;
    const bool punchReady = autopunchRelayEndpoint(relay, relayError);
    if (!punchReady)
        QLOG("[dial] hole punching unavailable: %s", relayError.c_str());

    QLOG("[control-dial] purpose=%s stage=start target=%s budgetMs=%d "
         "punch=%d relay=%s",
         purpose, advertised.str().c_str(), budgetMs, punchReady ? 1 : 0,
         punchReady ? relay.str().c_str() : "unavailable");

    qnet::ControlDialStatus lastStatus{};

    const auto tick = [&](const qnet::ControlDialStatus& status) -> bool {
        lastStatus = status;
        const std::string text =
            "Reaching the host... "
            + std::to_string(status.elapsedMs / 1000ull) + " s"
            + (status.peerMappingKnown ? " (punching)" : "")
            + " (Esc cancels)";
        if (con != nullptr) {
            con->statusLine(text);
            return con->pollKey(0) != VK_ESCAPE;
        }
        return !escPressedFocused();
    };

    const bool ok = qnet::dialControlLink(
        socket, advertised, punchReady ? &relay : nullptr, budgetMs, link,
        tick, error);
    Endpoint lastMapping{};
    lastMapping.sa.sin_addr.s_addr =
        lastStatus.lastMappingIpv4NetworkOrder;
    lastMapping.sa.sin_port = htons(lastStatus.lastMappingNatPort);
    const std::string lastMappingText =
        lastStatus.lastMappingIpv4NetworkOrder != 0
            && lastStatus.lastMappingNatPort != 0
        ? lastMapping.str() : std::string("none");
    if (ok) {
        QLOG("[dial] control established over UDP to %s (hellos=%u acks=%u%s)",
             link.peer().str().c_str(), link.stats().hellosSent,
             link.stats().helloAcksReceived,
             link.nonceDegraded() ? ", DEGRADED entropy for the link nonce"
                                  : "");
        QLOG("[control-dial] purpose=%s stage=established peer=%s "
             "elapsedMs=%llu peerMapping=%d mappingRx=%u mappingReject=%u "
             "targetMismatch=%u externalAlias=%u sameIpAlternate=%u lastMapping=%s "
             "lastInternal=%u hellos=%u acks=%u foreign=%u rejected=%u",
             purpose, link.peer().str().c_str(),
             static_cast<unsigned long long>(lastStatus.elapsedMs),
             lastStatus.peerMappingKnown ? 1 : 0,
             lastStatus.mappingsReceived, lastStatus.mappingsRejected,
             lastStatus.mappingTargetMismatches,
             lastStatus.mappingExternalPortAliases,
             lastStatus.mappingSameIpAlternates,
             lastMappingText.c_str(),
             static_cast<unsigned>(lastStatus.lastMappingInternalPort),
             link.stats().hellosSent, link.stats().helloAcksReceived,
             link.stats().foreignDatagrams, link.stats().rejected);
    } else {
        if (!punchReady && !error.empty())
            error += " (" + relayError + ")";
        QLOG("[dial] FAILED to %s: %s (hellos=%u sendFailures=%u "
             "socketError=%d acks=%u rejected=%u)",
             advertised.str().c_str(), error.c_str(),
             link.stats().hellosSent, link.stats().sendFailures,
             link.lastError(), link.stats().helloAcksReceived,
             link.stats().rejected);
        QLOG("[control-dial] purpose=%s stage=failed target=%s "
             "elapsedMs=%llu punch=%d peerMapping=%d mappingRx=%u "
             "mappingReject=%u targetMismatch=%u externalAlias=%u "
             "sameIpAlternate=%u "
             "lastMapping=%s lastInternal=%u hellos=%u acks=%u "
             "foreign=%u rejected=%u sendFailures=%u socketError=%d "
             "reason='%s'",
             purpose, advertised.str().c_str(),
             static_cast<unsigned long long>(lastStatus.elapsedMs),
             punchReady ? 1 : 0, lastStatus.peerMappingKnown ? 1 : 0,
             lastStatus.mappingsReceived, lastStatus.mappingsRejected,
             lastStatus.mappingTargetMismatches,
             lastStatus.mappingExternalPortAliases,
             lastStatus.mappingSameIpAlternates,
             lastMappingText.c_str(),
             static_cast<unsigned>(lastStatus.lastMappingInternalPort),
             link.stats().hellosSent, link.stats().helloAcksReceived,
             link.stats().foreignDatagrams, link.stats().rejected,
             link.stats().sendFailures, link.lastError(),
             error.c_str());
    }
    return ok;
}

bool automaticRelayOptions(const Settings& settings,
                           const Endpoint& advertisedHost,
                           RelayOptions& relay, std::string& error) {
    relay = {};
    if (!configuredRelayServer(settings, relay.server, error)
        || !qnet::relay::deriveEndpointRoomId(
            advertisedHost, relay.room)) {
        if (error.empty())
            error = "Could not derive the automatic relay room";
        return false;
    }
    relay.enabled = true;
    relay.roomProvided = true;
    relay.automaticAlias = true;
    return true;
}

bool discoverObservedHostEndpoint(const Settings& settings,
                                  const Endpoint& relayServer,
                                  Endpoint& observed,
                                  std::string& error) {
    (void)settings;
    qnet::relay::RoomId discoveryRoom{};
    bool degradedRandom = false;
    if (!qnet::relay::generateRoomId(discoveryRoom, &error, &degradedRandom)) {
        if (error.empty()) error = "could not generate a discovery room";
        return false;
    }
    if (degradedRandom)
        QLOG("[host-auto] discovery room used the last-resort RNG path");

    TcpConn conn;
    qnet::relay::RelayPairClient probe;
    if (!probe.begin(conn, relayServer, discoveryRoom,
                     qnet::relay::LaneDomain::Player,
                     qnet::relay::RelayRole::Host, 5000, &error)) {
        if (error.empty()) error = "relay control connection failed";
        return false;
    }

    const uint64_t deadlineUs = nowUs() + 5ull * 1000ull * 1000ull;
    while (nowUs() < deadlineUs) {
        const qnet::relay::PairPoll poll = probe.poll(conn, 200, &error);
        if (poll == qnet::relay::PairPoll::Closed
            || poll == qnet::relay::PairPoll::Error
            || poll == qnet::relay::PairPoll::Rejected) {
            if (error.empty()) error = "relay rejected the discovery request";
            break;
        }
        const qnet::relay::ObservedRegistration& seen =
            probe.observedRegistration();
        if (seen.valid) {
            if (!seen.usableAsPublicCandidate) {
                QLOG("[host-auto] relay-observed endpoint %s is not a public "
                     "candidate; using it as a rendezvous alias only",
                     seen.observedPublic.str().c_str());
            }
            observed = seen.observedPublic;
            conn.close();
            return true;
        }
    }
    if (error.empty()) {
        error = probe.observedRegistration().relaySupportsObserved
            ? "the relay did not report an observed address in time"
            : "this relay predates the observed-address extension; update it "
              "or set a public IPv4 manually";
    }
    conn.close();
    return false;
}

std::string fileExists(const std::string& p) {
    if (p.empty()) return std::string();
    const std::wstring wide = qplat::widen(p);
    if (wide.empty()) return std::string();
    return (qplat::fileExistsW(wide) || qplat::directoryExistsW(wide))
        ? p : std::string();
}
std::string defaultGamePath() {
    const std::string beside = qpath::exeDir();
    for (const char* name : {"qoh99.exe", "GAME.EXE"}) {
        if (!fileExists(beside + name).empty()) return name;
        if (!fileExists(name).empty()) return name;
    }
    return "qoh99.exe";
}
std::string absPath(const std::string& p) {
    if (p.empty()) return p;
    const std::wstring wide = qplat::widen(p);
    if (wide.empty()) return p;
    const std::wstring absolute = qplat::absolutePathW(wide);
    return absolute.empty() ? p : qplat::narrow(absolute);
}
std::string dirOf(const std::string& p) {
    if (p.empty()) return std::string();
    const std::wstring wide = qplat::widen(p);
    if (wide.empty()) return std::string();
    const std::wstring parent = qplat::dirOfW(wide);
    return parent.empty() ? std::string() : qplat::narrow(parent);
}

qclient::replay::ContentManifest replayContentManifestForGameDirectory(
    const std::string& gameDirectory, const bool forceRebuild = false) {
    static std::mutex mutex;
    static qclient::replay::ContentHashCache cache;
    static const qclient::replay::ContentManifestPolicy policy{};
    static std::string cachedDirectory;
    static std::wstring loadedCachePath;
    static qclient::replay::ContentManifest cachedManifest;
    std::lock_guard<std::mutex> guard(mutex);
    if (!forceRebuild && gameDirectory == cachedDirectory)
        return cachedManifest;
    const std::wstring root = qplat::widen(gameDirectory);
    if (root.empty()) {
        qclient::replay::ContentManifest failed{};
        failed.failureDetail =
            "the game directory could not be represented as Unicode";
        cachedDirectory = gameDirectory;
        cachedManifest = failed;
        return failed;
    }
    const std::wstring cachePath =
        qclient::replay::ContentHashCache::pathForGameDirectory(root);
    if (loadedCachePath != cachePath) {
        loadedCachePath = cachePath;
        cache.clear();
        const bool loaded = cache.load(cachePath);
        QLOG("[content-cache] %s '%s' (%zu entries)",
             loaded ? "loaded" : "no usable cache at",
             qplat::narrow(cachePath).c_str(), cache.size());
    }
    const uint64_t startedMs = nowUs() / 1000;
    cachedDirectory = gameDirectory;
    cachedManifest = qclient::replay::ContentManifestBuilder::build(
        root, policy, &cache);
    const uint64_t elapsedMs = nowUs() / 1000 - startedMs;
    QLOG("[content-cache] manifest built in %llu ms (files=%u cached=%u "
         "hashed=%llu bytes)",
         static_cast<unsigned long long>(elapsedMs), cachedManifest.filesHashed,
         cachedManifest.filesFromCache,
         static_cast<unsigned long long>(cachedManifest.bytesHashed));
    if (cachedManifest.bytesHashed > 0u) {
        const std::wstring directory = qplat::dirOfW(cachePath);
        if (!qplat::ensureDirW(directory) || !cache.save(cachePath)) {
            QLOG("[content-cache] could not save '%s'; the next launch will "
                 "hash everything again (this is slow, never incorrect)",
                 qplat::narrow(cachePath).c_str());
        }
    }
    return cachedManifest;
}
bool isRootedPath(const std::string& p) {
    if (p.size() >= 2 && p[1] == ':') return true;
    if (p.size() >= 2 && (p[0] == '\\' || p[0] == '/')) return true;
    return false;
}
std::string portablePath(const std::string& p) {
    if (p.empty() || isRootedPath(p)) return absPath(p);
    const std::wstring wide = qplat::widen(p);
    if (wide.empty()) return p;
    const std::wstring beside = qplat::exeDirW() + wide;
    if (qplat::fileExistsW(beside)) {
        const std::wstring absolute = qplat::absolutePathW(beside);
        return qplat::narrow(absolute.empty() ? beside : absolute);
    }
    return absPath(p);
}
std::string resolveGamePath(const Settings& s) {
    return portablePath(s.gamePath.empty() ? defaultGamePath() : s.gamePath);
}
std::string resolveDllPath(const Settings& s) {
    return portablePath(s.dllPath);
}

uint8_t configuredRoundsToWin(const Settings& s) {
    const std::string path =
        dirOf(resolveGamePath(s)) + "\\System\\QOHcnf.key";
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::vector<uint8_t> data;
    data.assign(std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
    constexpr size_t kFileSize = 8044;
    constexpr size_t kProfileBase = 4;
    constexpr size_t kProfileStride = 764;
    constexpr size_t kRoundsOffset = 48;
    constexpr uint32_t kProfileCount = 10;
    if (data.size() != kFileSize) return 0;
    const auto readLe32 = [&data](size_t at) {
        return static_cast<uint32_t>(data[at])
            | (static_cast<uint32_t>(data[at + 1]) << 8)
            | (static_cast<uint32_t>(data[at + 2]) << 16)
            | (static_cast<uint32_t>(data[at + 3]) << 24);
    };
    const uint32_t active = readLe32(0);
    if (active >= kProfileCount) return 0;
    const uint32_t roundsMinusOne = readLe32(
        kProfileBase + active * kProfileStride + kRoundsOffset);
    if (roundsMinusOne > qnetplay::kMaxRoundsToWinMinusOne) return 0;
    return static_cast<uint8_t>(roundsMinusOne + 1u);
}

bool validAdvertisedRounds(uint8_t rounds) {
    return rounds >= 1u
        && rounds <= qnetplay::kMaxRoundsToWinMinusOne + 1u;
}

std::string firstToRoundsText(uint8_t rounds) {
    if (!validAdvertisedRounds(rounds)) return "round count unavailable";
    return "first to " + std::to_string(rounds) + " round"
         + (rounds == 1u ? "" : "s");
}

void emitSessionPreamble() {
    qplat::PreambleContext context;
    context.component = "caster";
    context.buildContract = qipc::kBuildContractMarker;
    context.stateRoot = qplat::stateRootW();
    context.stateRootFallback = qplat::stateRootUsedFallback();

    const std::wstring exeDirW = qplat::exeDirW();
    const std::string gameUtf8 = portablePath(defaultGamePath());
    const std::wstring gameW = qplat::widen(gameUtf8);
    const std::wstring gameDirW =
        gameW.empty() ? std::wstring() : qplat::dirOfW(gameW);

    const auto addModule = [&context](const wchar_t* name,
                                      const std::wstring& path) {
        if (path.empty()) return;
        qplat::PreambleModule module;
        module.name = name;
        module.path = path;
        context.modules.push_back(std::move(module));
    };
    addModule(L"qoh99caster.exe", qplat::exePathW());
    addModule(L"game", gameW);
    addModule(L"qoh99_netplay.dll", exeDirW + L"qoh99_netplay.dll");
    addModule(L"ddraw.dll",
              gameDirW.empty() ? std::wstring() : gameDirW + L"\\ddraw.dll");
    addModule(L"SDL3.dll", exeDirW + L"SDL3.dll");

    context.extra = "sessionDir=" + g_sessionDir
                  + " stateRootSource=" + qpath::stateRootSource();

    qplat::writeSessionPreamble(context, [](const std::string& line) {
        QLOG("[preamble] %s", line.c_str());
    });
}

void loadOfflinePlayerProfiles(qinput::InputProfile out[3],
                               bool configured[3]) {
    for (int i = 0; i < 3; ++i) configured[i] = false;
    std::ifstream file("qoh99caster-controls.json", std::ios::binary);
    if (!file) return;
    std::string textContent((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    qinput::ControlsFile controls;
    std::string error;
    if (!qinput::parseControlsFile(textContent, controls, error)) return;
    for (int player = 1; player < 4; ++player) {
        const std::string& id = controls.activeByPlayer[player];
        if (id.empty()) continue;
        for (const qinput::InputProfile& profile : controls.profiles) {
            if (profile.id != id) continue;
            bool anyBinding = false;
            for (const auto& bindings : profile.actions)
                if (!bindings.empty()) anyBinding = true;
            if (anyBinding) {
                out[player - 1] = profile;
                configured[player - 1] = true;
            }
            break;
        }
    }
}

void fillOfflineExtraConfigs(qipc::IpcConfig& cfg, bool offline) {
    std::memset(cfg.offlineExtraPresent, 0,
                sizeof(cfg.offlineExtraPresent));
    for (int i = 0; i < 3; ++i)
        cfg.offlineExtraConfigs[i] = qinput::InputLaunchConfig{};
    if (offline) {
        qinput::InputProfile profiles[3];
        bool configured[3] = {};
        loadOfflinePlayerProfiles(profiles, configured);
        for (int i = 0; i < 3; ++i) {
            if (!configured[i]) continue;
            const qinput::CompileResult compiled =
                qinput::compileInputProfile(profiles[i], false);
            if (!compiled.ok) {
                QLOG("[controls] player %d profile does not compile (%s); "
                     "that player stays on the game's own keys",
                     i + 2, compiled.error.c_str());
                continue;
            }
            cfg.offlineExtraConfigs[i] = compiled.config;
            cfg.offlineExtraPresent[i] = 1;
        }
    }
    cfg.offlineExtrasHash = qipc::hashOfflineExtras(cfg);
}

qinput::InputProfile loadActiveInputProfile(qinput::ControlsFile* snapshot) {
    qinput::ControlsFile defaults = qinput::makeDefaultControlsFile();
    qinput::InputProfile active = defaults.profiles.front();
    if (snapshot != nullptr) *snapshot = defaults;
    std::ifstream file("qoh99caster-controls.json", std::ios::binary);
    if (!file) {
        QLOG("[controls] no qoh99caster-controls.json; using QOH keyboard "
             "defaults");
        return active;
    }
    std::string textContent((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    qinput::ControlsFile controls;
    std::string error;
    if (!qinput::parseControlsFile(textContent, controls, error)) {
        QLOG("[controls] REJECTED qoh99caster-controls.json: %s "
             "(using QOH keyboard defaults)", error.c_str());
        return active;
    }
    if (snapshot != nullptr) *snapshot = controls;
    bool found = false;
    for (const qinput::InputProfile& profile : controls.profiles) {
        if (profile.id == controls.activeProfileId) {
            active = profile;
            found = true;
        }
    }
    QLOG("[controls] loaded qoh99caster-controls.json profiles=%zu "
         "active=%s%s sourceMode=%u device='%s' guid=%s vid=%04X pid=%04X "
         "pathHash=%016llX",
         controls.profiles.size(),
         found ? active.id.c_str() : "<none>",
         found ? "" : " (falling back to QOH keyboard defaults)",
         static_cast<unsigned>(active.sourceMode),
         active.device.normalizedName.empty()
             ? "<none>" : active.device.normalizedName.c_str(),
         active.device.guid.empty() ? "<none>" : active.device.guid.c_str(),
         active.device.vendor, active.device.product,
         static_cast<unsigned long long>(active.device.pathFingerprint));
    return active;
}

void logConfigurationProvenance(
    const char* role, const ConfigProvenanceStage stage, const bool committed,
    const unsigned localDelay, const unsigned localRollback,
    const ConfigProvenanceStage localDelayOrigin,
    const ConfigProvenanceStage localRollbackOrigin,
    const unsigned peerDelay, const unsigned peerRollback,
    const ConfigProvenanceStage peerDelayOrigin,
    const ConfigProvenanceStage peerRollbackOrigin,
    const ConfigProvenanceStage peerRuntimeAttestation =
        ConfigProvenanceStage::Unknown) {
    QLOG("[config-provenance] v=1 role=%s stage=%s committed=%u "
         "local_delay=%u local_rollback=%u "
         "local_delay_origin=%s local_rollback_origin=%s "
         "peer_delay=%u peer_rollback=%u "
         "peer_delay_origin=%s peer_rollback_origin=%s "
         "peer_runtime_attestation=%s",
         role, qnetplay::config_provenance::stageName(stage),
         committed ? 1u : 0u, localDelay, localRollback,
         qnetplay::config_provenance::stageName(localDelayOrigin),
         qnetplay::config_provenance::stageName(localRollbackOrigin),
         peerDelay, peerRollback,
         qnetplay::config_provenance::stageName(peerDelayOrigin),
         qnetplay::config_provenance::stageName(peerRollbackOrigin),
         qnetplay::config_provenance::stageName(peerRuntimeAttestation));
}

void setCommittedConfigurationProvenance(
    qipc::IpcConfig& config, const ConfigProvenanceStage localDelayOrigin,
    const ConfigProvenanceStage localRollbackOrigin,
    const ConfigProvenanceStage peerDelayOrigin,
    const ConfigProvenanceStage peerRollbackOrigin) {
    config.configurationProvenance =
        qnetplay::config_provenance::makeCommittedWire(
            localDelayOrigin, localRollbackOrigin,
            peerDelayOrigin, peerRollbackOrigin);
}

qipc::IpcConfig baseConfig(const Settings& s, uint8_t mode, uint8_t isHost, uint8_t localPlayer) {
    qipc::IpcConfig c{};
    c.mode = mode;
    c.isHost = isHost;
    c.localPlayer = localPlayer;
    c.delay = static_cast<uint8_t>(s.delay);
    c.rollback = static_cast<uint8_t>(s.rollback);
    if (mode == qipc::MODE_NETPLAY) {
        setCommittedConfigurationProvenance(
            c, ConfigProvenanceStage::Default,
            ConfigProvenanceStage::Default,
            ConfigProvenanceStage::PeerAdvertised,
            ConfigProvenanceStage::PeerAdvertised);
    }
    c.maxDelay = 15;
    c.maxRollback = 15;
    c.directBoot = 1;
    c.vsRow = qipc::kVsRow1v1;
    fillOfflineExtraConfigs(c, false);
    c.musicVolume = static_cast<uint8_t>(s.musicVolume);
    c.soundVolume = static_cast<uint8_t>(s.soundVolume);
    c.hideNativeScore = s.hideNativeScore ? 1 : 0;
    c.hideNativeTimer = s.hideNativeTimer ? 1 : 0;
    c.disableExtraFunctions = 1;
    c.sessionId = GetTickCount();
    {
        const size_t needed = g_sessionDir.size();
        if (needed + 1 <= qipc::kSessionDirBytes) {
            std::memcpy(c.sessionDir, g_sessionDir.data(), needed);
            c.sessionDir[needed] = '\0';
            c.sessionDirBytes = static_cast<uint16_t>(needed);
        } else {
            c.sessionDirBytes = static_cast<uint16_t>(qipc::kSessionDirBytes);
            QLOG("[launch] session directory is %zu UTF-8 bytes but the IPC "
                 "field holds %zu - the launch will be REFUSED rather than "
                 "truncated. Set QOH99_NETPLAY_STATE_DIR to a shorter path. "
                 "(%s)",
                 needed, qipc::kSessionDirBytes - 1, g_sessionDir.c_str());
        }
    }
    {
        const qplat::CompatInfo& compat = qplat::detectCompat();
        c.compatFlags = static_cast<uint8_t>(
            (compat.wine ? qipc::kCompatFlagWine : 0)
            | (compat.proton ? qipc::kCompatFlagProton : 0));
    }
    c.spectatorServiceMode = c.serveSpectators
        ? qipc::SPECTATOR_SERVICE_INPROCESS
        : qipc::SPECTATOR_SERVICE_OFF;
    c.recordEnabled = 0;
    c.reservedV13a = 0;
    c.replayChannelGeneration = 0;
    c.recordChannelGeneration = 0;
    c.recordChannelNonce = 0;
    c.replayBrokerNonce = 0;
    c.replayManifestDigest = 0;
    c.replayIpcSchemaVersion = 0;
    c.replayFeatureBits = 0;
    c.replayMatchOrdinal = 0;
    c.replaySpeedPermille = 0;
    std::memset(
        c.replayContentManifestRoot, 0,
        sizeof(c.replayContentManifestRoot));
    c.replayGameGeneration = 0;
    c.replayExpectedFrameCount = 0;
    c.replayExpectedEventCount = 0;
    c.replayGameUuidLow = 0;
    c.replayGameUuidHigh = 0;
    c.replayValidatedSourceByteCount = 0;
    std::memset(
        c.replayValidatedSourceRangeDigest, 0,
        sizeof(c.replayValidatedSourceRangeDigest));
    c.replayEndpointKind = 0;
    c.replayHeadless = 0;
    c.replayPresentationFlags = 0;
    c.spectatorGrantId = 0;
    c.spectatorGrantExpiryUnix = 0;
    c.spectatorGrantMacLow = 0;
    c.spectatorGrantMacHigh = 0;
    c.spectatorMaxViewers = 0;
    c.reservedV13b = 0;
    c.spectatorTopologyEnabled = 0;
    c.spectatorTopologyRouteKind = 0;
    c.spectatorTopologyHostControlPort = 0;
    c.spectatorTopologyHostControlIpv4BE = 0;
    c.spectatorTopologyQspRouteGeneration = 0;
    c.spectatorTopologyGrantSourceGeneration = 0;
    c.spectatorSequenceBase = 0;
    c.spectatorClientPublisherEnabled = 0;
    std::memset(c.reservedV22, 0, sizeof(c.reservedV22));
    c.spectatorClientPublisherGeneration = 0;
    c.spectatorClientPublisherNonce = 0;
    initializeIpcPlayerIdentityDefaults(c, s.nickname);
    {
        const bool spectator = mode == qipc::MODE_SPECTATOR;
        qinput::ControlsFile controls = qinput::makeDefaultControlsFile();
        qinput::InputProfile active = spectator
            ? qinput::InputProfile{}
            : loadActiveInputProfile(&controls);
        qinput::CompileResult compiled = qinput::compileInputProfile(
            active, spectator, &controls);
        if (!compiled.ok) {
            QLOG("[controls] active profile REJECTED by compiler: %s "
                 "(using QOH keyboard defaults)", compiled.error.c_str());
            qinput::ControlsFile fallback = qinput::makeDefaultControlsFile();
            compiled = qinput::compileInputProfile(
                fallback.profiles.front(), spectator, &fallback);
        }
        c.inputConfig = compiled.config;
        c.modeControls = compiled.modeControls;
        c.inputEnvelope.schemaVersion = qinput::kInputConfigSchemaVersion;
        c.inputEnvelope.byteSize = sizeof(qinput::InputLaunchConfig);
        c.inputEnvelope.canonicalHash = qinput::hashInputConfigBytes(
            &c.inputConfig, sizeof(c.inputConfig));
        QLOG("[controls] compiled input payload mode=%u probes=%u "
             "modeControls=%u profileHash=%016llX",
             static_cast<unsigned>(c.inputConfig.sourceMode),
             c.inputConfig.probeCount,
             c.modeControls.count,
             static_cast<unsigned long long>(c.inputConfig.profileHash));
    }
    return c;
}

struct OsErrorExplanation {
    const char* symbol;
    const char* player;
};

OsErrorExplanation explainOsError(const int err) {
    switch (err) {
    case 10013: return {"WSAEACCES",
        "Windows blocked the connection. A firewall or security tool is "
        "usually the cause - allow the caster through it and try again."};
    case 10035: return {"WSAEWOULDBLOCK",
        "The network was busy for a moment. Trying again usually clears it."};
    case 10038: return {"WSAENOTSOCK",
        "The network connection was closed before it could be used."};
    case 10048: return {"WSAEADDRINUSE",
        "That port is busy. Something else on this PC is already using it - "
        "most often another copy of the caster that is still running. Close "
        "it, or pick a different port in Settings."};
    case 10049: return {"WSAEADDRNOTAVAIL",
        "This PC has no network address to send from. If you just connected "
        "or disconnected a VPN or network adapter, reconnect and try again."};
    case 10051: return {"WSAENETUNREACH",
        "This PC currently has no route to the internet."};
    case 10054: return {"WSAECONNRESET",
        "The other side closed the connection - usually their game or caster "
        "was closed, or their connection dropped."};
    case 10060: return {"WSAETIMEDOUT",
        "There was no answer in time. The address or port may be wrong, or a "
        "firewall on either side may be dropping the connection."};
    case 10061: return {"WSAECONNREFUSED",
        "Nothing was listening at that address and port. Check the address, "
        "and make sure the host has started hosting before you join."};
    case 10064: return {"WSAEHOSTDOWN",
        "The other PC did not respond - it may be off or asleep."};
    case 10065: return {"WSAEHOSTUNREACH",
        "That address could not be reached from this network."};
    case 11001: return {"WSAHOST_NOT_FOUND",
        "That address could not be looked up. Check it for typos."};
    default:    return {nullptr, nullptr};
    }
}

std::string diagnosticContext(const int osErr) {
    std::string out;
    const auto add = [&out](const std::string& line) {
        if (!line.empty()) { if (!out.empty()) out += "\n"; out += line; }
    };
    if (osErr) {
        const OsErrorExplanation e = explainOsError(osErr);
        add("Network error: " + std::to_string(osErr)
            + (e.symbol ? std::string(" (") + e.symbol + ")" : std::string()));
    }
    if (g_havePreflightInstallation) {
        const auto& id = g_preflightInstallation;
        add("Game: " + g_preflightInstallationPath);
        const auto* profile = qoh99::runtime_profile::findProfile(
            id.executable.runtimeProfileId);
        add(std::string("Edition: ")
            + (profile ? profile->displayName : "unrecognized")
            + " [" + (profile ? profile->runtimeProfileKey : "unknown") + "]");
        add("Executable SHA-256: " + id.executable.sha256);
        add(std::string("Content manifest: ")
            + qoh99::content::dispositionName(id.content.disposition)
            + " (" + std::to_string(id.content.identity.entryCount)
            + " entries, " + (id.content.admissionAllowed() ? "admitted"
                                                            : "not admitted")
            + ")");
    }
    add(std::string("Build: ") + qipc::kBuildContractMarker);
    if (!g_sessionDir.empty()) add("Logs: " + g_sessionDir);
    return out;
}

int fail(qui::Console* con, const std::string& title, std::string detail,
         int wsaErr = 0, const std::string& siteDetail = std::string(),
         bool saveDiagnostics = true) {
    if (saveDiagnostics) qoh99::Log::get().issue();
    std::string technical = diagnosticContext(wsaErr);
    if (!siteDetail.empty()) technical = siteDetail + "\n" + technical;
    QLOG("[fail] %s: %s\n%s", title.c_str(), detail.c_str(), technical.c_str());
    if (con) {
        if (wsaErr) {
            const OsErrorExplanation e = explainOsError(wsaErr);
            if (e.player) detail += std::string("\n\n") + e.player;
        }
        con->message(title, detail, technical);
    } else {
        if (wsaErr) {
            const OsErrorExplanation e = explainOsError(wsaErr);
            if (e.player) detail += std::string("\n\n") + e.player;
        }
        std::printf("ERROR: %s: %s\n\n%s\n", title.c_str(), detail.c_str(),
                    technical.c_str());
    }
    return 1;
}

std::string playerFacingEditionName(
    const qoh99::generated_model::RuntimeProfileId id) {
    switch (id) {
    case qoh99::generated_model::RuntimeProfileId::Qoh99Se1126:
        return "November 11 game build";
    case qoh99::generated_model::RuntimeProfileId::Qoh99DecemberCandidate:
        return "December game build";
    default:
        return "";
    }
}

std::string casterBanner(const std::string& nickname) {
    std::string out = std::string("QOH99 Netplay ") + qclient::ui::kCasterVersion
                    + "  |  " + nickname;
    const std::string& edition = qclient::ui::detectedGameEdition();
    if (!edition.empty()) out += "  |  " + edition;
    return out;
}

std::string replayRefusalMessage(
    const qclient::replay::ReplayPreflightStatusV2 status) {
    using S = qclient::replay::ReplayPreflightStatusV2;
    switch (status) {
    case S::CompatibilityIdentityMismatch:
    case S::CompatibilityRejected:
        return "This recording was made by a version of netplay too different "
               "from this one to replay safely.\n\nIt is not damaged, and "
               "nothing is wrong with your game - the two versions simply do "
               "not agree on enough to guarantee the replay would show you "
               "what actually happened.";
    case S::CompatibilityIdentityUnattested:
        return "This recording predates the information the caster needs to "
               "confirm it would play back accurately.\n\nIt is not damaged. "
               "Recordings made from now on carry that information.";
    case S::FileDigestInvalid:
    case S::SourceRangeInvalid:
    case S::StructuralInvalid:
    case S::TimelineInvalid:
        return "This replay file is damaged or incomplete.\n\nIf it was "
               "copied or downloaded, try getting it again - a file cut short "
               "in transfer looks exactly like this.";
    case S::AllocationFailure:
        return "There was not enough memory to load this replay.\n\nClosing "
               "other programs and trying again usually fixes it.";
    default:
        return "This replay could not be prepared for playback.\n\nPress [D] "
               "for the details to include in a bug report.";
    }
}

std::string gameExeMissingMessage(const std::string& path) {
    return "The game could not be found where the caster expected it.\n\n"
           "Looked for:\n  " + path
         + "\n\nPut the caster in the same folder as the game, or set the "
           "game's location in Settings -> Player and files.";
}

std::string netplayDllMissingMessage(const std::string& path) {
    return "Part of the netplay installation is missing.\n\n"
           "Looked for:\n  " + path
         + "\n\nThis file ships next to the caster - if it is gone, unpack "
           "the download again and keep all of its files together.";
}

std::string networkUnavailableMessage() {
    return "Windows would not start networking for the caster.\n\n"
           "This is almost always a firewall or security tool blocking it. "
           "Allow the caster through, then try again. Restarting Windows "
           "clears it if nothing else does.";
}

void info(qui::Console* con, const std::string& line) {
    QLOG("[flow] %s", line.c_str());
    if (con) con->statusLine(line);
    else     std::printf("%s\n", line.c_str());
}

struct CasterWindowSearch {
    DWORD processId;
    HWND found;
};

BOOL CALLBACK findCasterWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<CasterWindowSearch*>(parameter);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search->processId || !IsWindowVisible(window)
        || GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }
    search->found = window;
    return FALSE;
}

HWND casterMainWindow() {
    CasterWindowSearch search{GetCurrentProcessId(), nullptr};
    EnumWindows(findCasterWindow, reinterpret_cast<LPARAM>(&search));
    return search.found != nullptr ? search.found : GetConsoleWindow();
}

std::vector<uint8_t> readSmallFile(const std::wstring& path) {
    constexpr LONGLONG kLimitBytes = 16LL * 1024 * 1024;
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::vector<uint8_t> bytes;
    LARGE_INTEGER length{};
    if (GetFileSizeEx(file, &length) && length.QuadPart > 0
        && length.QuadPart <= kLimitBytes) {
        bytes.resize(static_cast<std::size_t>(length.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                      &read, nullptr)
            || read != bytes.size()) {
            bytes.clear();
        }
    }
    CloseHandle(file);
    return bytes;
}

constexpr double kJoinSoundLoudestDbfs = -32.0;

void announceOpponentJoined(qui::Console* con, const Settings& s) {
    if (!con) return;
    const std::string gameDirectory = dirOf(resolveGamePath(s));
    std::vector<uint8_t> sound;
    double gainDb = 0.0;
    const char* source = "RANYU.WAV";
    if (!gameDirectory.empty()) {
        const std::vector<uint8_t> cue = readSmallFile(
            qplat::widen(gameDirectory)
            + L"\\SYSTEM\\BGM\\WAVE\\RANYU.WAV");
        sound = qclient::quietedPcmWave(cue.data(), cue.size(),
                                        kJoinSoundLoudestDbfs, &gainDb);
    }
    if (sound.empty()) {
        source = "the bundled Asterisk sound";
        const HMODULE self = GetModuleHandleW(nullptr);
        const HRSRC resource = FindResourceW(
            self, MAKEINTRESOURCEW(QOH99_RES_OPPONENT_JOINED_SOUND), L"WAVE");
        const HGLOBAL loaded =
            resource != nullptr ? LoadResource(self, resource) : nullptr;
        const auto* bytes = loaded != nullptr
            ? static_cast<const uint8_t*>(LockResource(loaded)) : nullptr;
        if (bytes != nullptr) {
            sound = qclient::quietedPcmWave(
                bytes, SizeofResource(self, resource),
                kJoinSoundLoudestDbfs, &gainDb);
        }
    }
    static std::vector<uint8_t> playing;
    BOOL played = FALSE;
    if (!sound.empty()) {
        PlaySoundW(nullptr, nullptr, 0);
        playing.swap(sound);
        played = PlaySoundW(reinterpret_cast<LPCWSTR>(playing.data()),
                            nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
    const HWND window = casterMainWindow();
    bool flashed = false;
    if (window != nullptr && GetForegroundWindow() != window) {
        FLASHWINFO flash{};
        flash.cbSize = sizeof(flash);
        flash.hwnd = window;
        flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
        FlashWindowEx(&flash);
        flashed = true;
    }
    QLOG("[flow] opponent joined; sound %s%s (gain %.1f dB, loudest 400 ms "
         "at most %.0f dBFS), window %s",
         played ? "playing "
                : "unavailable (no audio output, or no plain PCM sound), "
                  "last tried ",
         source, gainDb, kJoinSoundLoudestDbfs,
         flashed ? "flashing until focused"
                 : (window != nullptr ? "already in front" : "not found"));
}

const GameFilesCheck& gameFilesCheck(const Settings& s) {
    const std::string game = resolveGamePath(s);
    if (g_gameFilesChecked && g_gameFiles.path == game) return g_gameFiles;

    g_gameFiles = GameFilesCheck{};
    g_gameFiles.path = game;
    g_gameFilesChecked = true;
    g_havePreflightInstallation = false;
    g_preflightInstallation = {};
    g_preflightInstallationPath.clear();
    qclient::ui::setDetectedGameEdition(std::string());

    if (game.empty() || fileExists(game).empty()) {
        g_gameFiles.error =
            "The game executable was not found.\n\nLooked for:\n  "
            + (game.empty() ? std::string("(no game path configured)") : game)
            + "\n\nNeeded: the November or December edition of Queen's of "
              "Heart '99."
            + "\n\nPut the caster in the game folder, or set the path in "
              "Settings -> Player and files.";
        QLOG("[game-check] REJECTED path='%s': not found", game.c_str());
        return g_gameFiles;
    }
    std::string error;
    const uint64_t startedMs = nowUs() / 1000;
    if (!qclient::validateRuntimeInstallationForAdmission(
            game, &g_preflightInstallation, error)) {
        g_preflightInstallation = {};
        g_gameFiles.error = error + "\n\nFile checked:\n  " + game
            + "\n\nChange it in Settings -> Player and files.";
        QLOG("[game-check] REJECTED path='%s': %s", game.c_str(),
             error.c_str());
        return g_gameFiles;
    }
    const uint64_t admissionMs = nowUs() / 1000 - startedMs;
    g_preflightInstallationPath = game;
    g_havePreflightInstallation = true;
    qclient::ui::setDetectedGameEdition(
        playerFacingEditionName(g_preflightInstallation.executable
                                    .runtimeProfileId));
    g_gameFiles.ok = true;
    QLOG("[game-check] accepted path='%s' edition='%s' sha256=%s "
         "admission=%llu ms content=%s (%u entries)",
         game.c_str(), qclient::ui::detectedGameEdition().c_str(),
         g_preflightInstallation.executable.sha256.c_str(),
         static_cast<unsigned long long>(admissionMs),
         qoh99::content::dispositionName(
             g_preflightInstallation.content.disposition),
         g_preflightInstallation.content.identity.entryCount);
    return g_gameFiles;
}

bool preflightOnlineArtifacts(const Settings& s, qui::Console* con,
                              bool needReplayContentIdentity = true) {
    const std::string game = resolveGamePath(s);
    const std::string dll = resolveDllPath(s);
    const bool cached = g_gameFilesChecked && g_gameFiles.path == game;
    if (!cached) info(con, "Checking the game files...");
    if (!gameFilesCheck(s).ok) {
        fail(con, "Game files check failed", g_gameFiles.error);
        return false;
    }
    std::string error;
    if (!qclient::preflightLaunchArtifacts(game, dll, error)) {
        fail(con, "Game files check failed", error);
        return false;
    }
    (void)needReplayContentIdentity;
    QLOG("[preflight] online artifacts accepted game='%s' dll='%s' "
         "installation=%s", game.c_str(), dll.c_str(),
         cached ? "cached" : "checked");
    return true;
}

bool localCompatibilityIdentityFor(const Settings& s,
                                   qnetplay::compat::CompatibilityIdentity& out,
                                   std::string& error) {
    if (!gameFilesCheck(s).ok) {
        error = g_gameFiles.error;
        return false;
    }
    if (!qnetplay::compat::isValidCompatibilityIdentity(
            g_preflightInstallation.compatibility)) {
        error = "local compatibility identity is incomplete after content "
                "preflight";
        return false;
    }
    out = g_preflightInstallation.compatibility;
    error.clear();
    return true;
}

bool localRuntimeArtifactIdentityFor(
    const Settings& s, qnet::RuntimeArtifactIdentityWire& out,
    std::string& error) {
    const std::string caster = qplat::narrow(qplat::exePathW());
    const std::string game = resolveGamePath(s);
    const std::string wrapper = dirOf(game) + "\\ddraw.dll";
    if (caster.empty() || game.empty() || wrapper.empty()) {
        out = {};
        error = "could not resolve the installed netplay files";
        return false;
    }
    return qclient::makeRuntimeArtifactIdentity(
        caster, resolveDllPath(s), wrapper, out, error);
}

const char* buildMismatchPlayerText() {
    return "Your build differs from your opponent's. One installation is "
           "older or newer. Update both, then try again.";
}

bool replayContentRootFor(const Settings& s,
                          std::array<uint8_t, 32>& out) {
    if (!gameFilesCheck(s).ok) return false;
    const auto& identity = g_preflightInstallation.content.identity;
    bool anySet = false;
    for (const uint8_t byte : identity.simulationRootSha256)
        if (byte != 0u) { anySet = true; break; }
    if (!anySet) return false;
    std::copy(identity.simulationRootSha256.begin(),
              identity.simulationRootSha256.end(), out.begin());
    return true;
}

qreplay::LegacyContentRootsV3 legacyContentRootsFor(const Settings& s) {
    qreplay::LegacyContentRootsV3 roots{};
    if (!gameFilesCheck(s).ok) return roots;
    const auto& identity = g_preflightInstallation.content.identity;
    const auto nonzero = [](const qoh99::content::Digest& digest) {
        for (const uint8_t byte : digest)
            if (byte != 0u) return true;
        return false;
    };
    if (!nonzero(identity.legacySimulationRootSha256)
        || !nonzero(identity.legacyValidationRootSha256)) {
        return roots;
    }
    std::memcpy(roots.simulationRootSha256,
                identity.legacySimulationRootSha256.data(),
                identity.legacySimulationRootSha256.size());
    std::memcpy(roots.validationRootSha256,
                identity.legacyValidationRootSha256.data(),
                identity.legacyValidationRootSha256.size());
    roots.manifestSchemaVersion =
        qoh99::content::kSupersededSimulationContentSchemaVersion;
    roots.valid = true;
    return roots;
}

bool sessionExecutableIdentity(const Settings& s,
                               qclient::ExecutableIdentity& out,
                               std::string& error) {
    const std::string game = resolveGamePath(s);
    if (gameFilesCheck(s).ok && g_preflightInstallationPath == game) {
        out = g_preflightInstallation.executable;
        error.clear();
        return true;
    }
    return qclient::inspectExecutable(game, out, error);
}

template <class T>
bool bodyAs(const TcpConn::Received& rx, T& out) {
    if (rx.body.size() != sizeof(T)) return false;
    std::memcpy(&out, rx.body.data(), sizeof(T));
    return true;
}
std::string errorTextOf(const TcpConn::Received& rx) {
    ErrorText et{};
    if (!bodyAs(rx, et)) return "(malformed error text)";
    et.text[sizeof(et.text) - 1] = '\0';
    return et.text;
}

bool sendControlError(ControlLink& ctrl, const std::string& text) {
    ErrorText error{};
    std::snprintf(error.text, sizeof(error.text), "%.119s", text.c_str());
    return ctrl.sendMsg(MSG_ERROR_TEXT, &error, sizeof(error));
}

bool escPressedFocused() {
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) &&
           GetConsoleWindow() == GetForegroundWindow();
}

struct PumpCtx {
    qui::Console* con = nullptr;
    bool haveRtt = false;
    uint32_t lastRttMs = 0;
    uint64_t pingSentUs[8] = {};
    uint32_t pingSeqNext = 0;
    uint64_t lastHeardUs = 0;
    uint64_t received = 0;
    SettingsPending pending{};
    bool havePending = false;
    ConfirmConfig confirmed{};
    bool haveConfirmed = false;
    bool malformedConfirmed = false;
    SpectateInfo spectateInfo{};
    bool haveSpectateInfo = false;
};

enum class Pump { Got, Timeout, PeerClosed, PeerError, RemoteError };

Pump pumpRecv(ControlLink& ctrl, PumpCtx& ctx, uint8_t expected, int timeoutMs,
              TcpConn::Received& rx, std::string* remoteErr = nullptr) {
    const uint64_t deadline = nowUs() + (uint64_t)timeoutMs * 1000;
    for (;;) {
        int64_t remainMs = ((int64_t)deadline - (int64_t)nowUs()) / 1000;
        if (remainMs < 0) remainMs = 0;
        TcpConn::Rs rs = ctrl.recvMsgEx(rx, (int)remainMs);
        switch (rs) {
            case TcpConn::Rs::Timeout: return Pump::Timeout;
            case TcpConn::Rs::Closed:  return Pump::PeerClosed;
            case TcpConn::Rs::Corrupt: return Pump::PeerError;
            case TcpConn::Rs::Error:   return Pump::PeerError;
            case TcpConn::Rs::Msg:     break;
        }
        ctx.lastHeardUs = nowUs();
        ++ctx.received;
        if (rx.type == expected) return Pump::Got;
        switch (rx.type) {
            case MSG_PING:
                ctrl.sendMsg(MSG_PONG, rx.body.data(), rx.body.size());
                break;
            case MSG_PONG: {
                Ping p{};
                if (bodyAs(rx, p)) {
                    uint64_t& slot = ctx.pingSentUs[p.seq % 8];
                    if (slot && p.sendTimeUs == slot) {
                        ctx.lastRttMs = (uint32_t)((nowUs() - slot) / 1000);
                        ctx.haveRtt = true;
                        slot = 0;
                    }
                }
                break;
            }
            case MSG_SETTINGS_PENDING:
                if (bodyAs(rx, ctx.pending)) ctx.havePending = true;
                break;
            case MSG_CONFIRM_CONFIG:
                if (bodyAs(rx, ctx.confirmed)) ctx.haveConfirmed = true;
                else ctx.malformedConfirmed = true;
                break;
            case MSG_PING_STATS: {
                PingStats st{};
                if (bodyAs(rx, st) && !ctx.haveRtt) {
                    ctx.lastRttMs = (uint32_t)(st.avgRttUs / 1000);
                    ctx.haveRtt = true;
                }
                break;
            }
            case MSG_SPECTATE_INFO:
                if (bodyAs(rx, ctx.spectateInfo))
                    ctx.haveSpectateInfo = true;
                break;
            case MSG_ERROR_TEXT:
                if (remoteErr) *remoteErr = errorTextOf(rx);
                return Pump::RemoteError;
            default:
                QLOG("[pump] ignoring unexpected msg type=%u size=%zu", rx.type, rx.body.size());
                break;
        }
        if (expected == MSG_NONE && nowUs() >= deadline) return Pump::Timeout;
    }
}

Pump pumpRecvEither(ControlLink& ctrl, PumpCtx& ctx, uint8_t firstExpected,
                    uint8_t secondExpected, int timeoutMs,
                    TcpConn::Received& rx,
                    std::string* remoteErr = nullptr) {
    const uint64_t deadline = nowUs() + (uint64_t)timeoutMs * 1000;
    for (;;) {
        int64_t remainMs = ((int64_t)deadline - (int64_t)nowUs()) / 1000;
        if (remainMs < 0) remainMs = 0;
        TcpConn::Rs rs = ctrl.recvMsgEx(rx, (int)remainMs);
        switch (rs) {
            case TcpConn::Rs::Timeout: return Pump::Timeout;
            case TcpConn::Rs::Closed:  return Pump::PeerClosed;
            case TcpConn::Rs::Corrupt: return Pump::PeerError;
            case TcpConn::Rs::Error:   return Pump::PeerError;
            case TcpConn::Rs::Msg:     break;
        }
        ctx.lastHeardUs = nowUs();
        if (rx.type == firstExpected || rx.type == secondExpected)
            return Pump::Got;
        switch (rx.type) {
            case MSG_PING:
                ctrl.sendMsg(MSG_PONG, rx.body.data(), rx.body.size());
                break;
            case MSG_PONG: {
                Ping ping{};
                if (bodyAs(rx, ping)) {
                    uint64_t& slot = ctx.pingSentUs[ping.seq % 8];
                    if (slot && ping.sendTimeUs == slot) {
                        ctx.lastRttMs =
                            (uint32_t)((nowUs() - slot) / 1000);
                        ctx.haveRtt = true;
                        slot = 0;
                    }
                }
                break;
            }
            case MSG_SETTINGS_PENDING:
                if (bodyAs(rx, ctx.pending)) ctx.havePending = true;
                break;
            case MSG_CONFIRM_CONFIG:
                if (bodyAs(rx, ctx.confirmed)) ctx.haveConfirmed = true;
                else ctx.malformedConfirmed = true;
                break;
            case MSG_PING_STATS: {
                PingStats stats{};
                if (bodyAs(rx, stats) && !ctx.haveRtt) {
                    ctx.lastRttMs =
                        (uint32_t)(stats.avgRttUs / 1000);
                    ctx.haveRtt = true;
                }
                break;
            }
            case MSG_SPECTATE_INFO:
                if (bodyAs(rx, ctx.spectateInfo)) ctx.haveSpectateInfo = true;
                break;
            case MSG_ERROR_TEXT:
                if (remoteErr) *remoteErr = errorTextOf(rx);
                return Pump::RemoteError;
            default:
                QLOG("[pump] ignoring unexpected msg type=%u size=%zu",
                     rx.type, rx.body.size());
                break;
        }
    }
}

bool sendPing(ControlLink& ctrl, PumpCtx& ctx) {
    Ping p{ ctx.pingSeqNext, nowUs() };
    if (!ctrl.sendMsg(MSG_PING, &p, sizeof(p))) return false;
    ctx.pingSentUs[p.seq % 8] = p.sendTimeUs;
    ctx.pingSeqNext++;
    return true;
}

constexpr uint64_t kSettingsHeartbeatEveryUs = 1000000ull;
constexpr uint64_t kSettingsHeartbeatTimeoutUs = 10000000ull;

enum class SettingsHeartbeatResult {
    Alive,
    SendFailed,
    TimedOut,
};

SettingsHeartbeatResult serviceSettingsHeartbeat(
        ControlLink& ctrl, PumpCtx& ctx, uint64_t& lastPingUs,
        const char* role, const char* phase) {
    const uint64_t now = nowUs();
    if (ctx.lastHeardUs == 0) ctx.lastHeardUs = now;
    if (now > ctx.lastHeardUs
        && now - ctx.lastHeardUs > kSettingsHeartbeatTimeoutUs) {
        QLOG("[settings-heartbeat] role=%s phase=%s state=timed-out "
             "silent-ms=%llu rx=%llu transport=%s: %s", role, phase,
             static_cast<unsigned long long>((now - ctx.lastHeardUs) / 1000),
             static_cast<unsigned long long>(ctx.received),
             ctrl.transportName(),
             ctx.received == 0
                 ? "nothing was EVER received on this link - suspect the read "
                   "path, not the peer"
                 : "the peer stopped sending");
        return SettingsHeartbeatResult::TimedOut;
    }
    if (lastPingUs == 0
        || (now > lastPingUs
            && now - lastPingUs >= kSettingsHeartbeatEveryUs)) {
        if (lastPingUs == 0) {
            QLOG("[settings-heartbeat] role=%s phase=%s state=armed "
                 "interval-ms=%llu timeout-ms=%llu", role, phase,
                 static_cast<unsigned long long>(
                     kSettingsHeartbeatEveryUs / 1000),
                 static_cast<unsigned long long>(
                     kSettingsHeartbeatTimeoutUs / 1000));
        }
        if (!sendPing(ctrl, ctx)) {
            QLOG("[settings-heartbeat] role=%s phase=%s state=send-failed "
                 "socket-error=%d", role, phase, ctrl.lastError());
            return SettingsHeartbeatResult::SendFailed;
        }
        lastPingUs = now;
    }
    return SettingsHeartbeatResult::Alive;
}

void showSettingsWait(qui::Console* con, const std::string& line) {
    if (con) con->clear();
    info(con, line);
}

qui::Tone pingToneOf(uint32_t rttMs) {
    return rttMs < 60 ? qui::Tone::Good
         : rttMs < 120 ? qui::Tone::Warn
         : qui::Tone::Bad;
}
const char* pingWordOf(uint32_t rttMs) {
    return rttMs < 60 ? "good" : rttMs < 120 ? "okay" : "rough";
}

void screenHeader(std::vector<qui::Row>& rows, const std::string& title) {
    rows.push_back({ { " " + title, qui::Tone::Accent } });
    rows.push_back({ { " " + std::string(64, '-'), qui::Tone::Dim } });
    rows.push_back({});
}

qui::Row pingRow(const PumpCtx& ctx) {
    if (!ctx.haveRtt)
        return { { "  Lobby ping: ", qui::Tone::Default },
                 { "measuring...", qui::Tone::Dim } };
    return { { "  Lobby ping: ", qui::Tone::Default },
             { std::to_string(ctx.lastRttMs) + " ms",
               pingToneOf(ctx.lastRttMs) },
             { "   (the game connection is measured right before "
               "starting)", qui::Tone::Dim } };
}

bool generateSessionCookie(qnetplay::wire::SessionCookie& cookie) {
    cookie = {};
    NTSTATUS status = BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(&cookie),
        static_cast<ULONG>(sizeof(cookie)), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0 && (cookie.low != 0 || cookie.high != 0);
}

const char* relayStatusText(qnet::relay::WelcomeStatus status) {
    using qnet::relay::WelcomeStatus;
    switch (status) {
        case WelcomeStatus::Accepted:     return "accepted";
        case WelcomeStatus::Waiting:      return "waiting for the other player";
        case WelcomeStatus::RoomNotFound: return "room not found";
        case WelcomeStatus::RoomFull:     return "room already has this role";
        case WelcomeStatus::Rejected:     return "request rejected";
        case WelcomeStatus::ServerFull:   return "relay is at capacity";
    }
    return "unknown relay status";
}

const char* routeText(uint8_t routeKind) {
    return routeKind == ROUTE_DIRECT ? "punched-direct"
         : routeKind == ROUTE_RELAY  ? "relay"
         : "invalid";
}

const char* routeLabelText(uint8_t label) {
    switch (static_cast<qnet::RouteLabel>(label)) {
        case qnet::RouteLabel::LanDirect:    return "lan-direct";
        case qnet::RouteLabel::PublicDirect: return "public-direct";
        case qnet::RouteLabel::PublicDirectPossibleHairpin:
            return "public-direct (possible NAT hairpin)";
        case qnet::RouteLabel::Relay:        return "relay";
        case qnet::RouteLabel::None:         break;
    }
    return "invalid";
}

const char* plainRouteText(uint8_t label) {
    switch (static_cast<qnet::RouteLabel>(label)) {
        case qnet::RouteLabel::LanDirect:    return "direct, same network";
        case qnet::RouteLabel::PublicDirect: return "direct";
        case qnet::RouteLabel::PublicDirectPossibleHairpin:
            return "direct";
        case qnet::RouteLabel::Relay:        return "through the relay";
        case qnet::RouteLabel::None:         break;
    }
    return "unknown";
}

bool validRouteSelectionFields(uint8_t routeKind, uint8_t routeLabel,
                               uint8_t reserved) {
    if (reserved != 0) return false;
    const auto label = static_cast<qnet::RouteLabel>(routeLabel);
    if (routeKind == ROUTE_DIRECT) return qnet::isDirect(label);
    if (routeKind == ROUTE_RELAY) return label == qnet::RouteLabel::Relay;
    return false;
}

uint8_t directLabelForPeer(uint32_t peerIpv4BE, bool sharedObservedPublic) {
    qnet::RouteCandidate candidate{};
    candidate.label = qnet::isAdvertisablePrivateBE(peerIpv4BE)
        ? qnet::RouteLabel::LanDirect
        : qnet::RouteLabel::PublicDirect;
    candidate.sameObservedPublicAddr = sharedObservedPublic;
    return static_cast<uint8_t>(qnet::resolveLabel(candidate));
}

bool establishRelayPair(TcpConn& ctrl, const RelayOptions& options,
                        qnet::relay::RelayRole role, RelayRuntime& runtime,
                        qui::Console* con, std::string& error,
                        HostControlEndpoint* hostControl = nullptr) {
    qnet::relay::RelayPairClient pair;
    if (!pair.begin(ctrl, options.server, options.room,
                    qnet::relay::LaneDomain::Player, role, 5000, &error)) {
        return false;
    }

    const uint64_t startedUs = nowUs();
    const uint64_t deadlineUs = startedUs + 60000000ull;
    for (;;) {
        qnet::relay::PairPoll result = pair.poll(ctrl, 250, &error);
        if (hostControl != nullptr) hostControl->service(0);
        switch (result) {
            case qnet::relay::PairPoll::Ready:
                runtime.pair = pair.result();
                runtime.forceRelay = options.forceRelay;
                QLOG("[relay] paired room=%s server=%s role=%s forceRelay=%d",
                     qnet::relay::formatRoomId(options.room).c_str(),
                     options.server.str().c_str(),
                     role == qnet::relay::RelayRole::Host ? "host" : "client",
                     options.forceRelay ? 1 : 0);
                return true;
            case qnet::relay::PairPoll::Waiting:
                break;
            case qnet::relay::PairPoll::Rejected:
                if (error.empty())
                    error = std::string("Relay rejected the room: ")
                          + relayStatusText(pair.lastStatus());
                return false;
            case qnet::relay::PairPoll::Closed:
                if (error.empty()) error = "Relay closed while pairing the room";
                return false;
            case qnet::relay::PairPoll::Error:
                if (error.empty()) error = "Relay pairing failed";
                return false;
        }

        if (nowUs() >= deadlineUs) {
            error = "Timed out waiting 60 s for the other relay-room player";
            return false;
        }
        const std::string status =
            "Relay room " + qnet::relay::formatRoomId(options.room)
            + " - waiting for the other player... "
            + std::to_string((nowUs() - startedUs) / 1000000ull) + " s";
        if (con) {
            con->statusLine(status + " (Esc cancels)");
            if (con->pollKey(0) == VK_ESCAPE) {
                error = "Relay-room wait cancelled";
                return false;
            }
        } else {
            static uint64_t lastPrintUs = 0;
            if (nowUs() - lastPrintUs >= 5000000ull) {
                std::printf("%s\n", status.c_str());
                lastPrintUs = nowUs();
            }
            if (escPressedFocused()) {
                error = "Relay-room wait cancelled";
                return false;
            }
        }
    }
}

qnetplay::Endpoint toCoreEndpoint(const Endpoint& endpoint) {
    return qnetplay::Endpoint{
        endpoint.sa.sin_addr.s_addr,
        ntohs(endpoint.sa.sin_port),
    };
}

Endpoint toSocketEndpoint(const qnetplay::Endpoint& endpoint) {
    Endpoint out;
    out.sa.sin_addr.s_addr = endpoint.ipv4;
    out.sa.sin_port = htons(endpoint.port);
    return out;
}

struct RouteProbeService {
    struct Target {
        bool active = false;
        bool probing = true;
        uint8_t routeId = 0;
        uint8_t label = 0;
        Endpoint dest{};
        RouteRttEstimator<32, 128> estimator;
        int64_t lastProbeUs = 0;
    };

    uint64_t sessionId = 0;
    qnetplay::wire::SessionCookie cookie{};
    qnetplay::wire::PeerRole localRole = qnetplay::wire::PeerRole::Host;
    Target targets[qnet::kMaxRouteCandidates + 1];
    uint32_t outboundSequence = 1;
    uint64_t probesAnswered = 0;
    uint64_t invalidProbeDrops = 0;
    int64_t ackTokens = 16;
    int64_t ackRefillUs = 0;

    void configure(uint64_t session,
                   const qnetplay::wire::SessionCookie& sessionCookie,
                   qnetplay::wire::PeerRole role) {
        sessionId = session;
        cookie = sessionCookie;
        localRole = role;
    }

    Target* findTarget(uint8_t routeId) {
        for (Target& t : targets)
            if (t.active && t.routeId == routeId) return &t;
        return nullptr;
    }

    Target* addTarget(uint8_t routeId, uint8_t label, const Endpoint& dest) {
        if (Target* existing = findTarget(routeId)) {
            if (!(existing->dest == dest)) {
                existing->estimator.reset();
                existing->lastProbeUs = 0;
            }
            existing->label = label;
            existing->dest = dest;
            return existing;
        }
        for (Target& t : targets) {
            if (t.active) continue;
            t.active = true;
            t.probing = true;
            t.routeId = routeId;
            t.label = label;
            t.dest = dest;
            t.estimator.reset();
            t.lastProbeUs = 0;
            return &t;
        }
        return nullptr;
    }

    void clearTargets() {
        for (Target& t : targets) {
            if (t.active) t.estimator.reset();
            t.active = false;
        }
    }

    void freezeTarget(uint8_t routeId) {
        if (Target* t = findTarget(routeId)) t->probing = false;
    }

    bool consume(UdpSock& udp, const uint8_t* bytes, size_t n,
                 const Endpoint& source) {
        namespace wire = qnetplay::wire;
        if (n < wire::kHeaderSize) return false;
        const uint8_t type = bytes[6];
        if (type != static_cast<uint8_t>(wire::MessageType::RouteProbe)
            && type != static_cast<uint8_t>(wire::MessageType::RouteProbeAck)) {
            return false;
        }
        const int64_t receivedUs = static_cast<int64_t>(nowUs());
        wire::Datagram datagram{};
        if (wire::decodeDatagram(bytes, n, sessionId, cookie, datagram)
                != wire::DecodeError::None) {
            ++invalidProbeDrops;
            return true;
        }
        if (datagram.header.type == wire::MessageType::RouteProbe) {
            wire::RouteProbePayload probe{};
            if (!wire::decodePayload(datagram.payload, probe)
                || probe.role == localRole) {
                ++invalidProbeDrops;
                return true;
            }
            const int64_t sendUs = static_cast<int64_t>(nowUs());
            if (ackRefillUs == 0) ackRefillUs = sendUs;
            const int64_t refill = (sendUs - ackRefillUs) / 250;
            ackTokens = ackTokens + refill > 16 ? 16 : ackTokens + refill;
            ackRefillUs += refill * 250;
            if (ackTokens <= 0) {
                ++invalidProbeDrops;
                return true;
            }
            --ackTokens;
            wire::RouteProbeAckPayload ack{};
            ack.sequence = probe.sequence;
            ack.generation = probe.generation;
            ack.echoStampMicros = probe.stampMicros;
            const int64_t dwell = sendUs - receivedUs;
            ack.dwellMicros = dwell > 0
                ? static_cast<uint32_t>(dwell < 1000000 ? dwell : 1000000)
                : 0u;
            ack.routeId = probe.routeId;
            ack.role = localRole;
            std::vector<uint8_t> payload, packet;
            if (wire::encodePayload(ack, payload)
                && wire::encodeDatagram(wire::MessageType::RouteProbeAck,
                                        sessionId, cookie, outboundSequence++,
                                        payload, packet)) {
                Endpoint destination = source;
                udp.sendTo(destination, packet.data(), packet.size());
                ++probesAnswered;
            }
            return true;
        }
        wire::RouteProbeAckPayload ack{};
        if (!wire::decodePayload(datagram.payload, ack)
            || ack.role == localRole) {
            ++invalidProbeDrops;
            return true;
        }
        Target* target = findTarget(ack.routeId);
        if (target == nullptr || !(target->dest == source)) {
            ++invalidProbeDrops;
            return true;
        }
        target->estimator.onReply(
            ack.sequence, static_cast<int64_t>(ack.echoStampMicros),
            ack.generation, receivedUs,
            static_cast<int64_t>(ack.dwellMicros));
        return true;
    }

    void pumpProbes(UdpSock& udp, int64_t spacingUs = 12000) {
        namespace wire = qnetplay::wire;
        const int64_t now = static_cast<int64_t>(nowUs());
        for (Target& t : targets) {
            if (!t.active || !t.probing) continue;
            t.estimator.expireOlderThan(now, 1000000);
            if (t.lastProbeUs != 0 && now - t.lastProbeUs < spacingUs)
                continue;
            wire::RouteProbePayload probe{};
            probe.sequence = t.estimator.onProbeSent(now);
            probe.generation = t.estimator.generation();
            probe.stampMicros = static_cast<uint64_t>(now);
            probe.routeId = t.routeId;
            probe.role = localRole;
            std::vector<uint8_t> payload, packet;
            if (wire::encodePayload(probe, payload)
                && wire::encodeDatagram(wire::MessageType::RouteProbe,
                                        sessionId, cookie, outboundSequence++,
                                        payload, packet)) {
                udp.sendTo(t.dest, packet.data(), packet.size());
                t.lastProbeUs = now;
            }
        }
    }

    bool windowComplete(uint8_t routeId, uint64_t minUnique = 32) {
        const Target* t = findTarget(routeId);
        return t != nullptr && t->estimator.counts().uniqueReplies >= minUnique;
    }

    void logStats(const char* role, const char* phase, uint8_t routeId) {
        const Target* t = findTarget(routeId);
        if (t == nullptr) return;
        const auto counts = t->estimator.counts();
        QLOG("[route-stats] role=%s phase=%s routeId=%u label=%s "
             "p50us=%lld p95us=%lld p99us=%lld jitterus=%lld "
             "lossPermille=%u samples=%llu sent=%llu dup=%llu stale=%llu "
             "ambiguous=%llu dwellP95us=%lld answered=%llu drops=%llu",
             role, phase, t->routeId, routeLabelText(t->label),
             static_cast<long long>(t->estimator.p50Micros()),
             static_cast<long long>(t->estimator.p95Micros()),
             static_cast<long long>(t->estimator.p99Micros()),
             static_cast<long long>(t->estimator.jitterMicros()),
             t->estimator.lossPerMille(),
             static_cast<unsigned long long>(counts.uniqueReplies),
             static_cast<unsigned long long>(counts.sent),
             static_cast<unsigned long long>(counts.duplicates),
             static_cast<unsigned long long>(counts.stale),
             static_cast<unsigned long long>(counts.ambiguous),
             static_cast<long long>(t->estimator.dwell().percentile(95)),
             static_cast<unsigned long long>(probesAnswered),
             static_cast<unsigned long long>(invalidProbeDrops));
    }

    void exportPrelaunch(qipc::IpcConfig& cfg, uint8_t routeId) {
        const Target* t = findTarget(routeId);
        if (t == nullptr || !t->estimator.hasRtt()) return;
        cfg.routeLabel = t->label;
        cfg.prelaunchRouteP50Us =
            static_cast<uint32_t>(t->estimator.p50Micros());
        cfg.prelaunchRouteP95Us =
            static_cast<uint32_t>(t->estimator.p95Micros());
        cfg.prelaunchRouteJitterUs =
            static_cast<uint32_t>(t->estimator.jitterMicros());
        cfg.prelaunchRouteLossPerMille =
            static_cast<uint16_t>(t->estimator.lossPerMille());
        cfg.prelaunchRouteSamples = static_cast<uint16_t>(
            t->estimator.counts().uniqueReplies > 0xFFFF
                ? 0xFFFF
                : t->estimator.counts().uniqueReplies);
    }
};

constexpr uint8_t kCommittedRouteId = 0;

bool svcPumpUdp(UdpSock& udp, RouteProbeService& svc, std::string& error) {
    uint8_t packet[qnetplay::wire::kMaxDatagramSize];
    for (int i = 0; i < 64; ++i) {
        Endpoint source;
        const int n = udp.recvFrom(packet, sizeof(packet), source);
        if (n < 0) {
            error = "UDP receive failed during route probing";
            return false;
        }
        if (n == 0) break;
        svc.consume(udp, packet, static_cast<size_t>(n), source);
    }
    return true;
}

qnet::RouteCandidates buildCandidateAdvertisement(
    uint32_t sessionId, uint16_t advertisedUdpPort,
    uint32_t reflectedPeerIpv4BE) {
    qnet::RouteCandidates msg{};
    msg.sessionId = sessionId;
    msg.observedPublicIpv4BE = reflectedPeerIpv4BE;
    uint32_t locals[16] = {};
    const size_t localCount = collectLocalIpv4BE(locals, 16);
    qnet::CandidateEndpoint privates[qnet::kMaxPrivateCandidates] = {};
    const size_t privateCount = collectPrivateCandidates(
        locals, localCount, htons(advertisedUdpPort), privates,
        qnet::kMaxPrivateCandidates);
    for (size_t i = 0; i < privateCount; ++i) {
        qnet::RouteCandidateWire& entry = msg.entries[msg.count];
        entry.kind = static_cast<uint8_t>(privates[i].kind);
        entry.routeId = static_cast<uint8_t>(msg.count + 1);
        entry.ipv4BE = privates[i].ipv4BE;
        entry.portBE = privates[i].portBE;
        ++msg.count;
    }
    return msg;
}

bool acceptCandidateAdvertisement(const qnet::RouteCandidates& msg,
                                  uint32_t expectedSessionId,
                                  qnet::CandidateEndpoint* out,
                                  uint8_t* outRouteIds,
                                  size_t* outCount,
                                  std::string& error) {
    if (msg.sessionId != expectedSessionId) {
        error = "Candidate advertisement carries a foreign session";
        return false;
    }
    if (msg.reserved[0] != 0 || msg.reserved[1] != 0 || msg.reserved[2] != 0) {
        error = "Candidate advertisement reserved bytes must be zero";
        return false;
    }
    if (msg.count > qnet::kMaxRouteCandidates) {
        error = "Candidate advertisement exceeds the bounded candidate set";
        return false;
    }
    qnet::CandidateEndpoint entries[qnet::kMaxRouteCandidates] = {};
    for (size_t i = 0; i < msg.count; ++i) {
        entries[i].kind =
            static_cast<qnet::CandidateKind>(msg.entries[i].kind);
        entries[i].ipv4BE = msg.entries[i].ipv4BE;
        entries[i].portBE = msg.entries[i].portBE;
        if (msg.entries[i].routeId == kCommittedRouteId) {
            error = "Candidate advertisement reuses the committed-route id";
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (msg.entries[j].routeId == msg.entries[i].routeId) {
                error = "Candidate advertisement repeats a route id";
                return false;
            }
        }
        outRouteIds[i] = msg.entries[i].routeId;
    }
    if (msg.count > 0) {
        const qnet::CandidateSetError verdict =
            qnet::validateCandidateSet(entries, msg.count);
        if (verdict != qnet::CandidateSetError::None) {
            error = "Candidate advertisement failed validation (code "
                  + std::to_string(static_cast<int>(verdict)) + ")";
            return false;
        }
    }
    for (size_t i = 0; i < msg.count; ++i) out[i] = entries[i];
    *outCount = msg.count;
    return true;
}

struct RaceOutcome {
    Endpoint winner{};
    uint8_t label = 0;
    uint8_t routeId = 0;
};

bool raceDirectCandidates(UdpSock& udp, RouteProbeService& svc,
                          const qnet::CandidateEndpoint* candidates,
                          const uint8_t* routeIds, size_t count,
                          uint32_t localPublicIpv4BE,
                          uint32_t peerPublicIpv4BE,
                          RaceOutcome& outcome, std::string& error) {
    if (count == 0) return false;
    svc.clearTargets();
    for (size_t i = 0; i < count; ++i) {
        Endpoint dest;
        dest.sa.sin_family = AF_INET;
        dest.sa.sin_addr.s_addr = candidates[i].ipv4BE;
        dest.sa.sin_port = candidates[i].portBE;
        svc.addTarget(routeIds[i],
                      static_cast<uint8_t>(
                          qnet::candidateBaseLabel(candidates[i].kind)),
                      dest);
    }

    constexpr uint64_t kRaceWindowMs = 1500;
    constexpr uint64_t kSettleMs = 300;
    constexpr uint64_t kValidateUnique = 8;
    const uint64_t startedMs = nowUs() / 1000;
    for (;;) {
        const uint64_t elapsedMs = nowUs() / 1000 - startedMs;
        if (elapsedMs > kRaceWindowMs) break;
        svc.pumpProbes(udp);
        if (!svcPumpUdp(udp, svc, error)) return false;

        if (elapsedMs >= kSettleMs) {
            qnet::RouteCandidate scored[qnet::kMaxRouteCandidates] = {};
            for (size_t i = 0; i < count; ++i) {
                RouteProbeService::Target* t = svc.findTarget(routeIds[i]);
                if (t == nullptr) continue;
                scored[i].label = qnet::candidateBaseLabel(candidates[i].kind);
                scored[i].validated =
                    t->estimator.counts().uniqueReplies >= kValidateUnique;
                scored[i].p95Micros =
                    static_cast<int>(t->estimator.p95Micros());
                scored[i].lossPerMille =
                    static_cast<int>(t->estimator.lossPerMille());
                scored[i].sameObservedPublicAddr =
                    candidates[i].kind == qnet::CandidateKind::PublicObserved
                    && localPublicIpv4BE != 0 && peerPublicIpv4BE != 0
                    && localPublicIpv4BE == peerPublicIpv4BE;
            }
            const qnet::RouteSelection selection = qnet::selectRoute(
                scored, count, false,
                qnet::RouteSelectionConfig{});
            if (selection.index >= 0) {
                const size_t i = static_cast<size_t>(selection.index);
                outcome.winner.sa.sin_family = AF_INET;
                outcome.winner.sa.sin_addr.s_addr = candidates[i].ipv4BE;
                outcome.winner.sa.sin_port = candidates[i].portBE;
                outcome.label = static_cast<uint8_t>(selection.label);
                outcome.routeId = routeIds[i];
                svc.logStats("race", "won", routeIds[i]);
                QLOG("[route-race] selected routeId=%u label=%s after %llu ms "
                     "(%zu candidates)",
                     outcome.routeId, routeLabelText(outcome.label),
                     static_cast<unsigned long long>(elapsedMs), count);
                return true;
            }
        }
        udp.waitReadable(1);
    }
    QLOG("[route-race] no direct candidate validated within %llu ms "
         "(%zu candidates) - continuing with the fallback path",
         1500ull, count);
    return false;
}

struct AutopunchOutcome {
    bool discovered = false;
    qnet::CandidateEndpoint candidate{};
    bool peerAnswered = false;
    qnet::autopunch::SessionStats stats{};
};

bool autopunchFallbackPass(UdpSock& udp, RouteProbeService& svc,
                           uint16_t localPort,
                           uint32_t peerIpv4BE, uint16_t peerPortBE,
                           AutopunchOutcome& outcome, std::string& error) {
    Endpoint relay{};
    std::string relayError;
    if (!autopunchRelayEndpoint(relay, relayError)) {
        QLOG("[autopunch] skipped: %s", relayError.c_str());
        return false;
    }

    qnet::autopunch::AutopunchSession session;
    session.configure(localPort, relay);
    if (!session.configured()) {
        error = "autopunch: local port is not bound";
        return false;
    }
    session.setTarget(peerIpv4BE, ntohs(peerPortBE));

    QLOG("[autopunch] pass start relay=%s localPort=%u target=%s",
         relay.str().c_str(), static_cast<unsigned>(localPort),
         [&] {
             Endpoint t{};
             t.sa.sin_family = AF_INET;
             t.sa.sin_addr.s_addr = peerIpv4BE;
             t.sa.sin_port = peerPortBE;
             return t.str();
         }().c_str());

    constexpr uint64_t kWindowMs = 4000;
    const uint64_t startedMs = nowUs() / 1000;
    auto sendVia = [](void* context, const Endpoint& destination,
                      const void* bytes, size_t length) -> bool {
        return static_cast<UdpSock*>(context)->sendTo(destination, bytes,
                                                      length);
    };

    for (;;) {
        const uint64_t nowMs = nowUs() / 1000;
        if (nowMs - startedMs > kWindowMs) break;
        session.service(sendVia, &udp, nowMs);

        for (;;) {
            uint8_t buffer[2048];
            Endpoint from{};
            const int received =
                udp.recvFrom(buffer, sizeof(buffer), from);
            if (received <= 0) break;
            if (session.handleInbound(buffer, static_cast<size_t>(received),
                                      from, nowMs)) {
                continue;
            }
            svc.consume(udp, buffer, static_cast<size_t>(received), from);
        }

        if (session.peerReachable()) break;
        udp.waitReadable(1);
    }

    outcome.stats = session.stats();
    outcome.peerAnswered = session.peerReachable();
    if (session.havePeerMapping()) {
        outcome.discovered = true;
        outcome.candidate.kind = qnet::CandidateKind::PublicObserved;
        outcome.candidate.ipv4BE = session.peerMapping().sa.sin_addr.s_addr;
        outcome.candidate.portBE = session.peerMapping().sa.sin_port;
    }
    QLOG("[autopunch] pass done discovered=%d reachable=%d mapped=%s "
         "registers=%u lookups=%u mappings=%u rejected=%u punchesOut=%u "
         "punchesIn=%u",
         outcome.discovered ? 1 : 0, outcome.peerAnswered ? 1 : 0,
         outcome.discovered ? session.peerMapping().str().c_str() : "none",
         outcome.stats.registersSent, outcome.stats.lookupsSent,
         outcome.stats.mappingsReceived, outcome.stats.mappingsRejected,
         outcome.stats.punchesSent, outcome.stats.punchesReceived);
    error.clear();
    return outcome.discovered;
}

bool driveUdpCore(UdpSock& udp, qnetplay::NetplayCore& core,
                  uint64_t nowMs, std::string& error,
                  RouteProbeService* probes,
                  qnet::autopunch::AutopunchSession* punch = nullptr);

bool sampleCommittedRoute(UdpSock& udp, RouteProbeService& svc,
                          qnetplay::NetplayCore* core, uint8_t label,
                          const Endpoint& dest, const char* role,
                          std::string& error) {
    svc.addTarget(kCommittedRouteId, label, dest);
    const uint64_t startedMs = nowUs() / 1000;
    while (nowUs() / 1000 - startedMs < 2000) {
        svc.pumpProbes(udp);
        if (core != nullptr) {
            if (!driveUdpCore(udp, *core, nowUs() / 1000, error, &svc))
                return false;
        } else if (!svcPumpUdp(udp, svc, error)) {
            return false;
        }
        if (svc.windowComplete(kCommittedRouteId)) break;
        udp.waitReadable(1);
    }
    svc.freezeTarget(kCommittedRouteId);
    svc.logStats(role, "prelaunch", kCommittedRouteId);
    if (!svc.windowComplete(kCommittedRouteId)) {
        QLOG("[route-stats] role=%s phase=prelaunch window INCOMPLETE "
             "(loss or peer stopped pumping); recommendation evidence will "
             "carry the reduced sample count", role);
    }
    return true;
}

const char* classificationText(qnet::Classification c) {
    switch (c) {
        case qnet::Classification::FullSpeed:     return "full-speed";
        case qnet::Classification::Marginal:      return "marginal";
        case qnet::Classification::Underbuffered: return "underbuffered";
    }
    return "unknown";
}

struct RouteRecommendation {
    bool hasEvidence = false;
    bool windowComplete = false;
    uint32_t p95Ms = 0;
    int64_t p95Us = 0, jitterUs = 0;
    int64_t meanUs = 0, minimumUs = 0, maximumUs = 0;
    uint32_t lossPerMille = 0;
    uint64_t samples = 0;
    qnet::DirectionalRequirement required{};
    int floorFrames = 0;
    qnet::DirectionalBudget budget{};
    qnet::Classification hostReceiveClass = qnet::Classification::FullSpeed;
    qnet::Classification clientReceiveClass = qnet::Classification::FullSpeed;
    qnet::Allocation lowLatency{};
    qnet::Allocation smoother{};
};

RouteRecommendation computeRouteRecommendation(
    RouteProbeService& svc,
    const PeerTuning& hostTuning, const PeerTuning& clientTuning) {
    RouteRecommendation out{};
    RouteProbeService::Target* t = svc.findTarget(kCommittedRouteId);
    if (t == nullptr || !t->estimator.hasRtt()) return out;
    const auto counts = t->estimator.counts();
    out.hasEvidence = true;
    out.windowComplete = counts.uniqueReplies >= 32;
    out.p95Us = t->estimator.p95Micros();
    out.jitterUs = t->estimator.jitterMicros();
    out.meanUs = t->estimator.rtt().mean();
    out.minimumUs = t->estimator.rtt().minimum();
    out.maximumUs = t->estimator.rtt().maximum();
    out.lossPerMille = t->estimator.lossPerMille();
    out.samples = counts.uniqueReplies;
    out.p95Ms = static_cast<uint32_t>(
        qnet::microsToMillisRounded(out.p95Us));
    out.required = qnet::symmetricRequirementForRoute(
        qnet::RouteRttMs{out.p95Ms});
    out.floorFrames = qnet::oneWayFrames(
        qnet::RouteRttMs{static_cast<uint32_t>(
            qnet::microsToMillisRounded(out.meanUs))});
    out.budget = qnet::directionalBudget(
        hostTuning.delay, hostTuning.rollback,
        clientTuning.delay, clientTuning.rollback);
    out.hostReceiveClass =
        qnet::classify(out.budget.hostReceive, out.required.hostFrames);
    out.clientReceiveClass =
        qnet::classify(out.budget.clientReceive, out.required.clientFrames);
    const qnet::PeerAuthorization hostAuth{
        hostTuning.maxRollback, hostTuning.maxDelay};
    const qnet::PeerAuthorization clientAuth{
        clientTuning.maxRollback, clientTuning.maxDelay};
    out.lowLatency = qnet::recommend(out.required, hostAuth, clientAuth,
                                     qnet::Preset::LowLatency);
    out.smoother = qnet::recommend(out.required, hostAuth, clientAuth,
                                   qnet::Preset::Smoother);
    return out;
}

static int worseRequirement(int a, int b) {
    if (a < 0) return b;
    if (b < 0) return a;
    return a > b ? a : b;
}

static uint8_t requiredFramesWire(int requiredFrames) {
    if (requiredFrames <= 0) return 0u;
    return static_cast<uint8_t>(requiredFrames > 255 ? 255 : requiredFrames);
}

void logRouteRecommendation(const char* role,
                            const RouteRecommendation& rec,
                            const PeerTuning& hostTuning,
                            const PeerTuning& clientTuning) {
    if (!rec.hasEvidence) {
        QLOG("[route-recommend] role=%s NO-EVIDENCE (no selected-route "
             "sample window); keeping the players' explicit settings", role);
        return;
    }
    QLOG("[route-recommend] role=%s evidence p95us=%lld p95ms=%u "
         "jitterus=%lld lossPermille=%u samples=%llu complete=%d | "
         "required host=%d client=%d | current hostD%u/R%u clientD%u/R%u "
         "budget host=%d client=%d class host=%s client=%s | "
         "lowLatency hostD%d/R%d clientD%d/R%d | "
         "smoother hostD%d/R%d clientD%d/R%d",
         role,
         static_cast<long long>(rec.p95Us), rec.p95Ms,
         static_cast<long long>(rec.jitterUs), rec.lossPerMille,
         static_cast<unsigned long long>(rec.samples),
         rec.windowComplete ? 1 : 0,
         rec.required.hostFrames, rec.required.clientFrames,
         hostTuning.delay, hostTuning.rollback,
         clientTuning.delay, clientTuning.rollback,
         rec.budget.hostReceive, rec.budget.clientReceive,
         classificationText(rec.hostReceiveClass),
         classificationText(rec.clientReceiveClass),
         rec.lowLatency.hostDelay, rec.lowLatency.hostMaxRollback,
         rec.lowLatency.clientDelay, rec.lowLatency.clientMaxRollback,
         rec.smoother.hostDelay, rec.smoother.hostMaxRollback,
         rec.smoother.clientDelay, rec.smoother.clientMaxRollback);
    const bool hostShort =
        rec.hostReceiveClass != qnet::Classification::FullSpeed;
    const bool clientShort =
        rec.clientReceiveClass != qnet::Classification::FullSpeed;
    if (hostShort || clientShort) {
        const bool anyUnderbuffered =
            rec.hostReceiveClass == qnet::Classification::Underbuffered
            || rec.clientReceiveClass == qnet::Classification::Underbuffered;
        QLOG("[route-recommend] role=%s WARNING: current settings are %s for "
             "the measured route tail; the session will hold or run below "
             "full speed until coverage EXCEEDS the requirement (host=%s "
             "client=%s, required host=%d client=%d, budget host=%d "
             "client=%d)", role,
             anyUnderbuffered ? "UNDERBUFFERED" : "only JUST-FIT",
             classificationText(rec.hostReceiveClass),
             classificationText(rec.clientReceiveClass),
             rec.required.hostFrames, rec.required.clientFrames,
             rec.budget.hostReceive, rec.budget.clientReceive);
    }
}

enum class RecommendationScreenResult {
    Confirmed,
    CancelledLocally,
    PeerCancelled,
    PeerDisconnected,
    PeerConnectionFailed,
};

RecommendationScreenResult recommendationScreen(
        qui::Console& con, ControlLink& ctrl, PumpCtx& ctx,
        UdpSock& udp, RouteProbeService& svc,
        bool isHost, uint8_t routeLabel,
        const RouteRecommendation& rec,
        const PeerTuning& peerTuning,
        int peerRequiredFrames,
        int& delay, int& rollback,
        int authMaxDelay, int authMaxRollback,
        std::string& peerMessage) {
    const int floorRequirement =
        worseRequirement(rec.floorFrames, peerRequiredFrames);
    const int comfortOwnReceive =
        isHost ? rec.required.hostFrames : rec.required.clientFrames;
    const int comfortPeerReceive =
        isHost ? rec.required.clientFrames : rec.required.hostFrames;
    const int myLow = isHost ? rec.lowLatency.hostDelay
                             : rec.lowLatency.clientDelay;
    const int myLowR = isHost ? rec.lowLatency.hostMaxRollback
                              : rec.lowLatency.clientMaxRollback;
    const int mySmooth = isHost ? rec.smoother.hostDelay
                                : rec.smoother.clientDelay;
    const int mySmoothR = isHost ? rec.smoother.hostMaxRollback
                                 : rec.smoother.clientMaxRollback;

    delay = myLow <= authMaxDelay ? myLow : authMaxDelay;
    rollback = myLowR <= authMaxRollback ? myLowR : authMaxRollback;

    int sel = 0;
    TcpConn::Received rx;
    std::string remoteErr;
    uint64_t lastHeartbeatUs = 0;
    uint64_t lastMirrorUs = 0;
    for (;;) {
        std::string dropError;
        if (!svcPumpUdp(udp, svc, dropError)) {
            peerMessage = dropError.empty()
                ? "The game connection failed during match settings."
                : dropError;
            return RecommendationScreenResult::PeerConnectionFailed;
        }
        const Pump p = pumpRecv(ctrl, ctx, MSG_NONE, 0, rx, &remoteErr);
        if (p == Pump::RemoteError) {
            peerMessage = remoteErr.empty()
                ? "Your opponent left during match settings."
                : remoteErr;
            return RecommendationScreenResult::PeerCancelled;
        }
        if (p == Pump::PeerClosed) {
            peerMessage = "Your opponent left during match settings.";
            return RecommendationScreenResult::PeerDisconnected;
        }
        if (p == Pump::PeerError) {
            peerMessage =
                "The connection to your opponent was lost during match settings.";
            return RecommendationScreenResult::PeerConnectionFailed;
        }
        const SettingsHeartbeatResult heartbeat = serviceSettingsHeartbeat(
            ctrl, ctx, lastHeartbeatUs, isHost ? "host" : "client",
            "selection");
        if (heartbeat != SettingsHeartbeatResult::Alive) {
            peerMessage = heartbeat == SettingsHeartbeatResult::TimedOut
                ? "Your opponent stopped responding during match settings."
                : "The connection to your opponent failed during match settings.";
            return RecommendationScreenResult::PeerConnectionFailed;
        }

        if (lastMirrorUs == 0 || nowUs() - lastMirrorUs >= 250000ull) {
            SettingsPending mirror{
                static_cast<uint8_t>(delay), static_cast<uint8_t>(rollback),
                static_cast<uint16_t>(ctx.lastRttMs > 65535u
                                          ? 65535u : ctx.lastRttMs)};
            ctrl.sendMsg(MSG_SETTINGS_PENDING, &mirror, sizeof(mirror));
            lastMirrorUs = nowUs();
        }
        const bool peerLive = ctx.havePending;
        const int peerDelayNow =
            peerLive ? static_cast<int>(ctx.pending.delay) : peerTuning.delay;
        const int peerRollbackNow =
            peerLive ? static_cast<int>(ctx.pending.rollback)
                     : peerTuning.rollback;

        const int myFloorRollback = qnet::clampInt(
            floorRequirement + 1 - peerDelayNow, 0, authMaxRollback);
        if (rollback < myFloorRollback) rollback = myFloorRollback;
        const int myFloorDelay = qnet::clampInt(
            floorRequirement + 1 - peerTuning.maxRollback, 0, authMaxDelay);
        if (delay < myFloorDelay) delay = myFloorDelay;

        char evidence[200];
        std::snprintf(evidence, sizeof(evidence),
                      "  Ping: %.1f ms on average (%.1f best, %.1f worst) "
                      "- %s, %llu checks%s",
                      rec.meanUs / 1000.0, rec.minimumUs / 1000.0,
                      rec.maximumUs / 1000.0,
                      plainRouteText(routeLabel),
                      static_cast<unsigned long long>(rec.samples),
                      rec.windowComplete ? "" : " so far");
        char quality[160];
        qui::Tone qualityTone = qui::Tone::Good;
        if (rec.lossPerMille == 0) {
            std::snprintf(quality, sizeof(quality),
                          "  Steady connection - no packets lost.");
        } else if (rec.lossPerMille <= 20) {
            std::snprintf(quality, sizeof(quality),
                          "  Losing %.1f%% of packets - occasional hitches "
                          "are possible.", rec.lossPerMille / 10.0);
            qualityTone = qui::Tone::Warn;
        } else {
            std::snprintf(quality, sizeof(quality),
                          "  Losing %.1f%% of packets - expect a rough "
                          "match.", rec.lossPerMille / 10.0);
            qualityTone = qui::Tone::Bad;
        }

        qui::ScreenSpec spec;
        screenHeader(spec.above, "Connection measured");
        spec.above.push_back({ { evidence, qui::Tone::Accent } });
        spec.above.push_back({ { quality, qualityTone } });
        spec.above.push_back(
            { { "  Recommended for this connection: "
                    + std::to_string(myLow) + " frame"
                    + (myLow == 1 ? "" : "s") + " of input delay, rollback "
                    + std::to_string(myLowR) + ".", qui::Tone::Default } });
        if (peerLive) {
            const int myCoverage = peerDelayNow + rollback;
            const int theirCoverage = delay + peerRollbackNow;
            const int myNeeded = comfortOwnReceive + 1;
            const int theirNeeded = comfortPeerReceive + 1;
            const bool covered =
                myCoverage >= myNeeded && theirCoverage >= theirNeeded;
            spec.above.push_back(
                { { "  Your opponent is on input delay "
                        + std::to_string(peerDelayNow) + ", rollback "
                        + std::to_string(peerRollbackNow) + ".",
                    qui::Tone::Default } });
            spec.above.push_back(
                { { "  This route needs "
                        + std::to_string(myNeeded > theirNeeded ? myNeeded
                                                                : theirNeeded)
                        + " frames of cover each way. You have "
                        + std::to_string(myCoverage) + ", they have "
                        + std::to_string(theirCoverage) + ".",
                    covered ? qui::Tone::Good : qui::Tone::Bad } });
            if (!covered) {
                spec.above.push_back(
                    { { "  Below that the match runs slow for BOTH of you. "
                        "Raise rollback, or ask your",
                        qui::Tone::Bad } });
                spec.above.push_back(
                    { { "  opponent to add input delay - either one covers "
                        "your side.",
                        qui::Tone::Bad } });
            }
        } else {
            spec.above.push_back(
                { { "  Waiting to see what your opponent picks.",
                    qui::Tone::Dim } });
            spec.above.push_back({});
        }
        spec.above.push_back({});
        spec.items.push_back({ { "Input delay   < " + std::to_string(delay)
                                   + " >", qui::Tone::Default },
                               { myFloorDelay > 0
                                     ? "   frames your inputs wait (needs at "
                                       "least " + std::to_string(myFloorDelay)
                                           + " so your opponent keeps up, "
                                             "up to "
                                           + std::to_string(authMaxDelay) + ")"
                                     : "   frames your inputs wait (up to "
                                           + std::to_string(authMaxDelay)
                                           + ")",
                                 qui::Tone::Dim } });
        spec.items.push_back({ { "Rollback      < " + std::to_string(rollback)
                                   + " >", qui::Tone::Default },
                               { myFloorRollback > 0
                                     ? "   how far the game may rewind "
                                       "(at least "
                                           + std::to_string(myFloorRollback)
                                           + " while your opponent is on "
                                             "delay "
                                           + std::to_string(peerDelayNow)
                                           + ", up to "
                                           + std::to_string(authMaxRollback)
                                           + ")"
                                     : "   how far the game may rewind "
                                       "(your opponent's delay already covers "
                                       "this connection)",
                                 qui::Tone::Dim } });
        spec.items.push_back({ { "Use the recommended settings",
                                 qui::Tone::Default },
                               { "   (delay " + std::to_string(myLow)
                                     + ", rollback " + std::to_string(myLowR)
                                     + ")", qui::Tone::Dim } });
        spec.items.push_back({ { "Smoother picture, more input lag",
                                 qui::Tone::Default },
                               { "   (delay " + std::to_string(mySmooth)
                                     + ", rollback "
                                     + std::to_string(mySmoothR) + ")",
                                 qui::Tone::Dim } });
        spec.items.push_back({ { "Start the game", qui::Tone::Good } });
        spec.below.push_back({});
        spec.below.push_back({ { "  Up/Down = move - Left/Right = adjust - "
                                 "Enter = choose - Esc = cancel",
                                 qui::Tone::Dim } });

        switch (con.menuTick(spec, sel, 50)) {
            case qui::kTimeout: break;
            case qui::kLeft:
                if (sel == 0 && delay > myFloorDelay) delay--;
                if (sel == 1 && rollback > myFloorRollback) rollback--;
                break;
            case qui::kRight:
                if (sel == 0 && delay < authMaxDelay) delay++;
                if (sel == 1 && rollback < authMaxRollback) rollback++;
                break;
            case 2:
                delay = myLow <= authMaxDelay ? myLow : authMaxDelay;
                rollback = myLowR <= authMaxRollback ? myLowR
                                                     : authMaxRollback;
                break;
            case 3:
                delay = mySmooth <= authMaxDelay ? mySmooth : authMaxDelay;
                rollback = mySmoothR <= authMaxRollback ? mySmoothR
                                                        : authMaxRollback;
                break;
            case 4:
                return RecommendationScreenResult::Confirmed;
            case qui::kBack: {
                const std::string notice = isHost
                    ? "The host left during match settings."
                    : "The joining player left during match settings.";
                const bool sent = sendControlError(ctrl, notice);
                QLOG("[settings-confirm] local cancel role=%s notice-sent=%d",
                     isHost ? "host" : "client", sent ? 1 : 0);
                ctrl.closeGraceful();
                return RecommendationScreenResult::CancelledLocally;
            }
            default:
                break;
        }
    }
}

bool driveUdpCore(UdpSock& udp, qnetplay::NetplayCore& core,
                  uint64_t nowMs, std::string& error,
                  RouteProbeService* probes,
                  qnet::autopunch::AutopunchSession* punch) {
    if (punch != nullptr && punch->configured()) {
        punch->service(
            [](void* context, const Endpoint& destination, const void* bytes,
               size_t length) -> bool {
                return static_cast<UdpSock*>(context)->sendTo(destination, bytes,
                                                              length);
            },
            &udp, nowMs);
    }
    for (const auto& packet : core.pollOutbound(nowMs)) {
        Endpoint destination = toSocketEndpoint(packet.destination);
        if (!udp.sendTo(destination, packet.bytes.data(), packet.bytes.size())) {
            error = "UDP rendezvous send failed";
            return false;
        }
    }

    uint8_t packet[qnetplay::wire::kMaxDatagramSize];
    for (int i = 0; i < 64; ++i) {
        Endpoint source;
        int n = udp.recvFrom(packet, sizeof(packet), source);
        if (n < 0) {
            error = "UDP rendezvous receive failed";
            return false;
        }
        if (n == 0) break;
        if (punch != nullptr
            && punch->handleInbound(packet, static_cast<size_t>(n), source,
                                    nowMs)) {
            continue;
        }
        if (probes != nullptr
            && probes->consume(udp, packet, static_cast<size_t>(n), source)) {
            continue;
        }
        qnetplay::IngressResult accepted =
            core.onDatagram(toCoreEndpoint(source), packet,
                            static_cast<size_t>(n), nowMs);
        if (accepted != qnetplay::IngressResult::Accepted) {
            static uint64_t rejected = 0;
            if (++rejected <= 8 || (rejected % 100) == 0)
                QLOG("[udp-rendezvous] rejected datagram result=%u from=%s",
                     static_cast<unsigned>(accepted), source.str().c_str());
        }
    }
    return true;
}

bool clientUdpRendezvous(UdpSock& udp, const Endpoint& host,
                         const NetplayConfig& negotiated,
                         qnetplay::Endpoint& lockedPeer,
                         std::string& error,
                         RouteProbeService* svc = nullptr,
                         uint8_t committedLabel = 0) {
    qnetplay::CoreConfig config{};
    config.localRole = qnetplay::wire::PeerRole::Client;
    config.localPlayer = negotiated.hostPlayer == 1 ? 1 : 0;
    config.compatibilityId = negotiated.compatibilityId;
    config.compatibilityIdentity = negotiated.compatibilityIdentity;
    config.sessionId = negotiated.sessionId;
    config.cookie = {
        negotiated.playerCookieLow, negotiated.playerCookieHigh
    };
    config.expectedPeer = toCoreEndpoint(host);
    qnetplay::NetplayCore core(config);

    const uint64_t deadline = nowUs() / 1000 + 10000;
    while (nowUs() / 1000 < deadline) {
        const uint64_t nowMs = nowUs() / 1000;
        if (!driveUdpCore(udp, core, nowMs, error, svc)) return false;
        if (core.isEstablished()) {
            lockedPeer = core.peerEndpoint();
            if (svc != nullptr) {
                const uint8_t label = committedLabel != 0
                    ? committedLabel
                    : directLabelForPeer(lockedPeer.ipv4, false);
                return sampleCommittedRoute(
                    udp, *svc, &core, label,
                    toSocketEndpoint(lockedPeer), "client", error);
            }
            return true;
        }
        udp.waitReadable(svc != nullptr ? 1 : 20);
    }
    error = "UDP rendezvous timed out waiting for the host";
    return false;
}

bool hostUdpRendezvous(ControlLink& ctrl, PumpCtx& pump, UdpSock& udp,
                       const Endpoint& tcpPeer,
                       const NetplayConfig& negotiated,
                       qnetplay::Endpoint& lockedPeer,
                       TuningProposal& proposal,
                       std::string& error,
                       RouteProbeService* svc = nullptr) {
    qnetplay::CoreConfig config{};
    config.localRole = qnetplay::wire::PeerRole::Host;
    config.localPlayer = negotiated.hostPlayer == 1 ? 0 : 1;
    config.compatibilityId = negotiated.compatibilityId;
    config.compatibilityIdentity = negotiated.compatibilityIdentity;
    config.sessionId = negotiated.sessionId;
    config.cookie = {
        negotiated.playerCookieLow, negotiated.playerCookieHigh
    };
    config.expectedPeer = { tcpPeer.sa.sin_addr.s_addr, 0 };
    config.learnPeerPort = true;
    qnetplay::NetplayCore core(config);

    qnet::autopunch::AutopunchSession hostPunch;
    {
        Endpoint punchRelay{};
        std::string punchRelayError;
        if (autopunchRelayEndpoint(punchRelay, punchRelayError)) {
            hostPunch.configure(udp.boundPort(), punchRelay);
            QLOG("[autopunch] host registering localPort=%u relay=%s",
                 static_cast<unsigned>(udp.boundPort()),
                 punchRelay.str().c_str());
        } else {
            QLOG("[autopunch] host pass unavailable: %s",
                 punchRelayError.c_str());
        }
    }

    TcpConn::Received rx;
    std::string remoteError;
    bool samplingStarted = false;
    bool punchLogged = false;
    uint64_t deadline = nowUs() / 1000 + 10000;
    while (nowUs() / 1000 < deadline) {
        const uint64_t nowMs = nowUs() / 1000;
        if (!driveUdpCore(udp, core, nowMs, error, svc, &hostPunch))
            return false;
        if (!punchLogged && hostPunch.havePeerMapping()) {
            punchLogged = true;
            QLOG("[autopunch] host learned joiner mapping=%s (punching)",
                 hostPunch.peerMapping().str().c_str());
        }

        if (svc != nullptr && !samplingStarted && core.isEstablished()) {
            const qnetplay::Endpoint peer = core.peerEndpoint();
            samplingStarted = true;
            if (!sampleCommittedRoute(udp, *svc, &core,
                                      directLabelForPeer(peer.ipv4, false),
                                      toSocketEndpoint(peer), "host", error)) {
                return false;
            }
            deadline = nowUs() / 1000 + 180000;
            continue;
        }
        if (svc != nullptr && samplingStarted) svc->pumpProbes(udp);

        switch (pumpRecv(ctrl, pump, MSG_TUNING_PROPOSAL,
                         svc != nullptr ? 1 : 10, rx,
                         &remoteError)) {
            case Pump::Got:
                if (!core.isEstablished()) {
                    error = "Client proposed tuning before UDP endpoint validation";
                    return false;
                }
                if (!bodyAs(rx, proposal)
                    || proposal.sessionId != negotiated.sessionId
                    || proposal.routeKind != ROUTE_DIRECT
                    || !validRouteSelectionFields(proposal.routeKind,
                                                  proposal.routeLabel,
                                                  0u)
                    || proposal.clientTuning.delay > qnet::kCapabilityDelay
                    || proposal.clientTuning.rollback
                           > qnet::kCapabilityRollback
                    || proposal.clientTuning.maxDelay
                        < proposal.clientTuning.delay
                    || proposal.clientTuning.maxRollback
                        < proposal.clientTuning.rollback) {
                    error = "Client sent invalid peer-local tuning proposal";
                    return false;
                }
                lockedPeer = core.peerEndpoint();
                if (svc != nullptr) {
                    if (RouteProbeService::Target* t =
                            svc->findTarget(kCommittedRouteId)) {
                        t->label = proposal.routeLabel;
                    }
                    svc->logStats("host", "committed", kCommittedRouteId);
                }
                return true;
            case Pump::PeerClosed:
                error = "Client disconnected during UDP rendezvous";
                return false;
            case Pump::PeerError:
                error = "Control socket failed during UDP rendezvous";
                return false;
            case Pump::RemoteError:
                error = "Client rejected UDP rendezvous: " + remoteError;
                return false;
            case Pump::Timeout:
                break;
        }
        udp.waitReadable(svc != nullptr && samplingStarted ? 1 : 10);
    }
    error = "UDP rendezvous timed out waiting for the client";
    return false;
}

qnetplay::CoreConfig relayRouteCoreConfig(
    bool host,
    const NetplayConfig& negotiated,
    const Endpoint& peer)
{
    qnetplay::CoreConfig config{};
    config.localRole = host
        ? qnetplay::wire::PeerRole::Host
        : qnetplay::wire::PeerRole::Client;
    config.localPlayer = host
        ? (negotiated.hostPlayer == 1 ? 0 : 1)
        : (negotiated.hostPlayer == 1 ? 1 : 0);
    config.compatibilityId = negotiated.compatibilityId;
    config.compatibilityIdentity = negotiated.compatibilityIdentity;
    config.sessionId = negotiated.sessionId;
    config.cookie = {
        negotiated.playerCookieLow, negotiated.playerCookieHigh
    };
    config.expectedPeer = toCoreEndpoint(peer);
    config.learnPeerPort = false;
    return config;
}

bool sendRouteCore(UdpSock& udp, qnetplay::NetplayCore& core,
                   uint64_t nowMs, const char* route,
                   std::string& error) {
    for (const auto& packet : core.pollOutbound(nowMs)) {
        const Endpoint destination = toSocketEndpoint(packet.destination);
        if (!udp.sendTo(destination, packet.bytes.data(),
                        packet.bytes.size())) {
            error = std::string(route) + " UDP rendezvous send failed";
            return false;
        }
    }
    return true;
}

bool receiveRelayRoutesEx(UdpSock& udp,
                          qnet::relay::RelayUdpClient& traversal,
                          qnetplay::NetplayCore* direct,
                          const Endpoint& directEndpoint,
                          qnetplay::NetplayCore* relayed,
                          uint64_t nowMs,
                          std::string& error,
                          RouteProbeService* probes = nullptr) {
    uint8_t packet[qnetplay::wire::kMaxDatagramSize];
    for (int i = 0; i < 64; ++i) {
        Endpoint source;
        const int n = udp.recvFrom(packet, sizeof(packet), source);
        if (n < 0) {
            error = "Relay rendezvous receive failed";
            return false;
        }
        if (n == 0) break;

        const qnet::relay::UdpInbound control =
            traversal.handleInbound(packet, static_cast<size_t>(n),
                                    source, nowMs);
        if (control != qnet::relay::UdpInbound::NotHandled)
            continue;

        if (probes != nullptr
            && probes->consume(udp, packet, static_cast<size_t>(n), source)) {
            continue;
        }

        qnetplay::NetplayCore* destination = nullptr;
        const char* route = "unknown";
        if (direct && source == directEndpoint) {
            destination = direct;
            route = "direct";
        } else if (relayed && source == traversal.relayEndpoint()) {
            destination = relayed;
            route = "relay";
        }
        if (!destination) {
            static uint64_t unknown = 0;
            if (++unknown <= 8 || (unknown % 100) == 0)
                QLOG("[relay-rendezvous] ignored unbound source=%s size=%d",
                     source.str().c_str(), n);
            continue;
        }

        const qnetplay::IngressResult accepted =
            destination->onDatagram(toCoreEndpoint(source), packet,
                                    static_cast<size_t>(n), nowMs);
        if (accepted != qnetplay::IngressResult::Accepted) {
            static uint64_t rejected = 0;
            if (++rejected <= 8 || (rejected % 100) == 0)
                QLOG("[relay-rendezvous] %s packet rejected result=%u "
                     "source=%s", route, static_cast<unsigned>(accepted),
                     source.str().c_str());
        }
    }
    return true;
}

bool receiveRelayRoutes(UdpSock& udp,
                        qnet::relay::RelayUdpClient& traversal,
                        qnetplay::NetplayCore* direct,
                        qnetplay::NetplayCore* relayed,
                        uint64_t nowMs,
                        std::string& error,
                        RouteProbeService* probes = nullptr) {
    return receiveRelayRoutesEx(udp, traversal, direct,
                                traversal.directPeer(), relayed, nowMs,
                                error, probes);
}

bool waitForRelayCandidates(UdpSock& udp,
                            qnet::relay::RelayUdpClient& traversal,
                            const qnet::relay::PairResult& pair,
                            bool directPunchEnabled,
                            std::string& error) {
    if (!traversal.configure(pair, &error)) return false;
    traversal.setDirectPunchEnabled(directPunchEnabled);
    const uint64_t deadline = nowUs() / 1000 + 5000;
    while (nowUs() / 1000 < deadline) {
        const uint64_t nowMs = nowUs() / 1000;
        if (!traversal.service(udp, nowMs, &error)
            || !receiveRelayRoutes(udp, traversal, nullptr, nullptr,
                                   nowMs, error)) {
            return false;
        }
        if (traversal.ready()) {
            QLOG("[relay-rendezvous] candidates ready direct=%s relay=%s",
                 traversal.directPeer().str().c_str(),
                 traversal.relayEndpoint().str().c_str());
            return true;
        }
        udp.waitReadable(20);
    }
    error = "Relay UDP registration timed out after 5 s";
    return false;
}

bool clientRelayUdpRendezvous(UdpSock& udp,
                              const RelayRuntime& relay,
                              const NetplayConfig& negotiated,
                              qnetplay::Endpoint& lockedPeer,
                              uint8_t& routeKind,
                              std::string& error,
                              RouteProbeService* svc = nullptr,
                              const qnet::CandidateEndpoint* hostCandidates
                                  = nullptr,
                              const uint8_t* hostRouteIds = nullptr,
                              size_t hostCandidateCount = 0,
                              uint32_t localPublicIpv4BE = 0,
                              uint8_t* committedLabel = nullptr,
                              uint8_t* committedRouteId = nullptr) {
    qnet::relay::RelayUdpClient traversal;
    if (!waitForRelayCandidates(udp, traversal, relay.pair,
                                !relay.forceRelay, error))
        return false;

    Endpoint directTarget = traversal.directPeer();
    const uint32_t peerPublicBE = traversal.directPeer().sa.sin_addr.s_addr;
    uint8_t directLabel = directLabelForPeer(
        peerPublicBE,
        localPublicIpv4BE != 0 && localPublicIpv4BE == peerPublicBE);
    uint8_t directRouteId = kCommittedRouteId;
    if (svc != nullptr && hostCandidateCount > 0 && !relay.forceRelay) {
        svc->clearTargets();
        size_t raced = 0;
        for (size_t i = 0; i < hostCandidateCount; ++i) {
            if (hostCandidates[i].kind != qnet::CandidateKind::PrivateLan)
                continue;
            Endpoint dest;
            dest.sa.sin_family = AF_INET;
            dest.sa.sin_addr.s_addr = hostCandidates[i].ipv4BE;
            dest.sa.sin_port = hostCandidates[i].portBE;
            svc->addTarget(hostRouteIds[i],
                           static_cast<uint8_t>(qnet::RouteLabel::LanDirect),
                           dest);
            ++raced;
        }
        if (raced > 0) {
            svc->addTarget(kCommittedRouteId, directLabel, directTarget);
            constexpr uint64_t kRaceWindowMs = 900;
            constexpr uint64_t kSettleMs = 300;
            constexpr uint64_t kValidateUnique = 8;
            const uint64_t raceStartMs = nowUs() / 1000;
            while (nowUs() / 1000 - raceStartMs < kRaceWindowMs) {
                const uint64_t nowMs = nowUs() / 1000;
                if (!traversal.service(udp, nowMs, &error)) return false;
                svc->pumpProbes(udp);
                if (!receiveRelayRoutes(udp, traversal, nullptr, nullptr,
                                        nowMs, error, svc)) {
                    return false;
                }
                if (nowUs() / 1000 - raceStartMs >= kSettleMs) {
                    RouteProbeService::Target* best = nullptr;
                    for (RouteProbeService::Target& t : svc->targets) {
                        if (!t.active || t.routeId == kCommittedRouteId)
                            continue;
                        if (t.estimator.counts().uniqueReplies
                                < kValidateUnique)
                            continue;
                        if (best == nullptr
                            || t.estimator.p95Micros()
                                   < best->estimator.p95Micros()) {
                            best = &t;
                        }
                    }
                    if (best != nullptr) {
                        RouteProbeService::Target* observed =
                            svc->findTarget(kCommittedRouteId);
                        const bool observedValidated = observed != nullptr
                            && observed->estimator.counts().uniqueReplies
                                   >= kValidateUnique;
                        if (!observedValidated
                            || best->estimator.p95Micros() + 3000
                                   < observed->estimator.p95Micros()) {
                            directTarget = best->dest;
                            directLabel = static_cast<uint8_t>(
                                qnet::RouteLabel::LanDirect);
                            directRouteId = best->routeId;
                        }
                        QLOG("[route-race] relay-flow direct target %s "
                             "routeId=%u label=%s",
                             directTarget.str().c_str(), directRouteId,
                             routeLabelText(directLabel));
                        break;
                    }
                }
                udp.waitReadable(1);
            }
            svc->clearTargets();
        }
    }

    qnetplay::NetplayCore direct(relayRouteCoreConfig(
        false, negotiated, directTarget));
    qnetplay::NetplayCore relayed(relayRouteCoreConfig(
        false, negotiated, traversal.relayEndpoint()));
    qnet::relay::RouteSelector selector(relay.forceRelay);
    const uint64_t startedMs = nowUs() / 1000;
    if (!selector.start(startedMs)) {
        error = "Internal relay route selector configuration is invalid";
        return false;
    }

    while (!selector.expired(nowUs() / 1000)) {
        const uint64_t nowMs = nowUs() / 1000;
        if (!traversal.service(udp, nowMs, &error)) return false;
        if (selector.shouldProbeDirect()
            && !sendRouteCore(udp, direct, nowMs, "Direct", error)) {
            return false;
        }
        if (selector.shouldProbeRelay(nowMs)
            && !sendRouteCore(udp, relayed, nowMs, "Relay", error)) {
            return false;
        }
        if (!receiveRelayRoutesEx(udp, traversal, &direct, directTarget,
                                  &relayed, nowMs, error, svc)) {
            return false;
        }

        const qnet::relay::SelectedRoute selected = selector.update(
            nowMs, direct.isEstablished(), relayed.isEstablished());
        if (selected == qnet::relay::SelectedRoute::Direct) {
            lockedPeer = direct.peerEndpoint();
            routeKind = ROUTE_DIRECT;
            QLOG("[relay-rendezvous] client committed punched-direct after "
                 "%llu ms", static_cast<unsigned long long>(
                     selector.elapsed(nowMs)));
            if (committedLabel != nullptr) *committedLabel = directLabel;
            if (committedRouteId != nullptr) *committedRouteId = directRouteId;
            if (svc != nullptr) {
                return sampleCommittedRoute(
                    udp, *svc, &direct, directLabel,
                    toSocketEndpoint(lockedPeer), "client", error);
            }
            return true;
        }
        if (selected == qnet::relay::SelectedRoute::Relay) {
            lockedPeer = relayed.peerEndpoint();
            routeKind = ROUTE_RELAY;
            QLOG("[relay-rendezvous] client committed relay after %llu ms",
                 static_cast<unsigned long long>(selector.elapsed(nowMs)));
            if (committedLabel != nullptr)
                *committedLabel = static_cast<uint8_t>(qnet::RouteLabel::Relay);
            if (committedRouteId != nullptr)
                *committedRouteId = kCommittedRouteId;
            if (svc != nullptr) {
                return sampleCommittedRoute(
                    udp, *svc, &relayed,
                    static_cast<uint8_t>(qnet::RouteLabel::Relay),
                    toSocketEndpoint(lockedPeer), "client", error);
            }
            return true;
        }
        udp.waitReadable(svc != nullptr ? 1 : 10);
    }
    error = "Neither punched-direct nor relay UDP route established within 12 s";
    return false;
}

bool validRelayProposal(const TuningProposal& proposal,
                        const NetplayConfig& negotiated) {
    return proposal.sessionId == negotiated.sessionId
        && (proposal.routeKind == ROUTE_DIRECT
            || proposal.routeKind == ROUTE_RELAY)
        && validRouteSelectionFields(proposal.routeKind,
                                     proposal.routeLabel,
                                     0u)
        && proposal.clientTuning.delay <= qnet::kCapabilityDelay
        && proposal.clientTuning.rollback <= qnet::kCapabilityRollback
        && proposal.clientTuning.maxDelay
            >= proposal.clientTuning.delay
        && proposal.clientTuning.maxRollback
            >= proposal.clientTuning.rollback;
}

bool finalConfirmationMatchesProposal(
        const ConfirmConfig& confirmed,
        const TuningProposal& proposal,
        const NetplayConfig& negotiated) {
    return confirmed.sessionId == negotiated.sessionId
        && confirmed.sessionId == proposal.sessionId
        && confirmed.routeKind == proposal.routeKind
        && confirmed.routeLabel == proposal.routeLabel
        && confirmed.routeId == proposal.routeId
        && validRouteSelectionFields(confirmed.routeKind,
                                     confirmed.routeLabel,
                                     confirmed.reserved)
        && confirmed.clientTuning.delay <= qnet::kCapabilityDelay
        && confirmed.clientTuning.rollback <= qnet::kCapabilityRollback
        && confirmed.clientTuning.maxDelay
            == proposal.clientTuning.maxDelay
        && confirmed.clientTuning.maxRollback
            == proposal.clientTuning.maxRollback
        && confirmed.clientTuning.maxDelay
            >= confirmed.clientTuning.delay
        && confirmed.clientTuning.maxRollback
            >= confirmed.clientTuning.rollback;
}

bool hostRelayUdpRendezvous(ControlLink& ctrl, PumpCtx& pump, UdpSock& udp,
                            const RelayRuntime& relay,
                            const NetplayConfig& negotiated,
                            qnetplay::Endpoint& lockedPeer,
                            TuningProposal& proposal,
                            std::string& error,
                            RouteProbeService* svc = nullptr,
                            const qnet::CandidateEndpoint* clientCandidates
                                = nullptr,
                            size_t clientCandidateCount = 0) {
    qnet::relay::RelayUdpClient traversal;
    if (!waitForRelayCandidates(udp, traversal, relay.pair,
                                !relay.forceRelay, error))
        return false;

    qnetplay::NetplayCore direct(relayRouteCoreConfig(
        true, negotiated, traversal.directPeer()));
    qnetplay::NetplayCore relayed(relayRouteCoreConfig(
        true, negotiated, traversal.relayEndpoint()));

    struct CandidateAcceptor {
        Endpoint endpoint{};
        std::unique_ptr<qnetplay::NetplayCore> core;
    };
    std::vector<CandidateAcceptor> acceptors;
    for (size_t i = 0; i < clientCandidateCount
                       && acceptors.size() < qnet::kMaxPrivateCandidates;
         ++i) {
        if (clientCandidates[i].kind != qnet::CandidateKind::PrivateLan)
            continue;
        CandidateAcceptor acceptor;
        acceptor.endpoint.sa.sin_family = AF_INET;
        acceptor.endpoint.sa.sin_addr.s_addr = clientCandidates[i].ipv4BE;
        acceptor.endpoint.sa.sin_port = clientCandidates[i].portBE;
        if (acceptor.endpoint == traversal.directPeer()) continue;
        acceptor.core = std::make_unique<qnetplay::NetplayCore>(
            relayRouteCoreConfig(true, negotiated, acceptor.endpoint));
        acceptors.push_back(std::move(acceptor));
    }

    auto establishedDirect =
        [&]() -> std::pair<qnetplay::NetplayCore*, const Endpoint*> {
        if (direct.isEstablished())
            return {&direct, nullptr};
        for (CandidateAcceptor& acceptor : acceptors) {
            if (acceptor.core->isEstablished())
                return {acceptor.core.get(), &acceptor.endpoint};
        }
        return {nullptr, nullptr};
    };

    TcpConn::Received rx;
    std::string remoteError;
    bool samplingStarted = false;
    uint64_t deadline = nowUs() / 1000
                      + qnet::relay::kRouteDeadlineMs;
    bool deadlineExtended = false;
    while (nowUs() / 1000 < deadline) {
        const uint64_t nowMs = nowUs() / 1000;
        if (!traversal.service(udp, nowMs, &error)
            || !sendRouteCore(udp, direct, nowMs, "Direct", error)
            || !sendRouteCore(udp, relayed, nowMs, "Relay", error)) {
            return false;
        }
        for (CandidateAcceptor& acceptor : acceptors) {
            if (!sendRouteCore(udp, *acceptor.core, nowMs,
                               "DirectCandidate", error)) {
                return false;
            }
        }
        {
            uint8_t packet[qnetplay::wire::kMaxDatagramSize];
            for (int i = 0; i < 64; ++i) {
                Endpoint source;
                const int n = udp.recvFrom(packet, sizeof(packet), source);
                if (n < 0) {
                    error = "Relay rendezvous receive failed";
                    return false;
                }
                if (n == 0) break;
                if (traversal.handleInbound(packet, static_cast<size_t>(n),
                                            source, nowMs)
                        != qnet::relay::UdpInbound::NotHandled) {
                    continue;
                }
                if (svc != nullptr
                    && svc->consume(udp, packet, static_cast<size_t>(n),
                                    source)) {
                    continue;
                }
                qnetplay::NetplayCore* destination = nullptr;
                if (source == traversal.directPeer()) {
                    destination = &direct;
                } else if (source == traversal.relayEndpoint()) {
                    destination = &relayed;
                } else {
                    for (CandidateAcceptor& acceptor : acceptors) {
                        if (source == acceptor.endpoint) {
                            destination = acceptor.core.get();
                            break;
                        }
                    }
                }
                if (destination == nullptr) {
                    static uint64_t unknown = 0;
                    if (++unknown <= 8 || (unknown % 100) == 0)
                        QLOG("[relay-rendezvous] ignored unbound source=%s "
                             "size=%d", source.str().c_str(), n);
                    continue;
                }
                destination->onDatagram(toCoreEndpoint(source), packet,
                                        static_cast<size_t>(n), nowMs);
            }
        }

        if (svc != nullptr && !samplingStarted) {
            const auto [establishedCore, candidateEndpoint] =
                establishedDirect();
            if (establishedCore != nullptr) {
                const qnetplay::Endpoint peer =
                    establishedCore->peerEndpoint();
                svc->addTarget(kCommittedRouteId,
                               candidateEndpoint != nullptr
                                   ? static_cast<uint8_t>(
                                         qnet::RouteLabel::LanDirect)
                                   : directLabelForPeer(peer.ipv4, false),
                               toSocketEndpoint(peer));
                samplingStarted = true;
            } else if (relayed.isEstablished()) {
                svc->addTarget(
                    kCommittedRouteId,
                    static_cast<uint8_t>(qnet::RouteLabel::Relay),
                    toSocketEndpoint(relayed.peerEndpoint()));
                samplingStarted = true;
            }
        }
        if (svc != nullptr && samplingStarted) svc->pumpProbes(udp);
        if (samplingStarted && !deadlineExtended) {
            deadline = nowUs() / 1000 + 180000;
            deadlineExtended = true;
        }

        switch (pumpRecv(ctrl, pump, MSG_TUNING_PROPOSAL,
                         svc != nullptr ? 1 : 10, rx,
                         &remoteError)) {
            case Pump::Got: {
                if (!bodyAs(rx, proposal)
                    || !validRelayProposal(proposal, negotiated)) {
                    error = "Client sent an invalid relay-route tuning proposal";
                    return false;
                }
                qnetplay::NetplayCore* selected = nullptr;
                if (proposal.routeKind == ROUTE_DIRECT) {
                    selected = establishedDirect().first;
                } else if (relayed.isEstablished()) {
                    selected = &relayed;
                }
                if (selected == nullptr) {
                    error = std::string("Client proposed ")
                          + routeText(proposal.routeKind)
                          + " before the matching host route was validated";
                    return false;
                }
                lockedPeer = selected->peerEndpoint();
                QLOG("[relay-rendezvous] host committed %s endpoint=%08X:%u "
                     "label=%s",
                     routeText(proposal.routeKind), lockedPeer.ipv4,
                     lockedPeer.port, routeLabelText(proposal.routeLabel));
                if (svc != nullptr) {
                    if (RouteProbeService::Target* t =
                            svc->findTarget(kCommittedRouteId)) {
                        t->dest = toSocketEndpoint(lockedPeer);
                        t->label = proposal.routeLabel;
                    }
                    return sampleCommittedRoute(
                        udp, *svc, selected, proposal.routeLabel,
                        toSocketEndpoint(lockedPeer), "host", error);
                }
                return true;
            }
            case Pump::PeerClosed:
                error = "Client disconnected during relay UDP rendezvous";
                return false;
            case Pump::PeerError:
                error = "Relayed control stream failed during UDP rendezvous";
                return false;
            case Pump::RemoteError:
                error = "Client rejected relay UDP rendezvous: " + remoteError;
                return false;
            case Pump::Timeout:
                break;
        }
        udp.waitReadable(svc != nullptr ? 1 : 10);
    }
    error = "Relay UDP rendezvous timed out waiting for the client route";
    return false;
}

bool validClientIntentRecord(const ClientIntent& intent) {
    if (!qnet::validateClientIntent(intent.intent) || intent.reserved3 != 0)
        return false;
    if (intent.intent != INTENT_SPECTATE && intent.spectatorDataPort != 0)
        return false;
    if (intent.intent == INTENT_JOIN) {
        return intent.spectatorRoute == SPECTATE_ROUTE_NONE
            && intent.sessionId == 0 && intent.keyLow == 0
            && intent.keyHigh == 0 && intent.viewerId == 0
            && intent.resumeRecord == 0 && intent.resumeDigest == 0
            && intent.spectatorCapabilities == 0;
    }
    if (intent.intent == INTENT_STATUS) {
        return intent.spectatorRoute == SPECTATE_ROUTE_NONE
            && intent.sessionId == 0 && intent.keyLow == 0
            && intent.keyHigh == 0 && intent.viewerId == 0
            && intent.resumeRecord == 0 && intent.resumeDigest == 0
            && intent.spectatorCapabilities == 0;
    }
    if (intent.intent == INTENT_SPECTATE_RELEASE) {
        return intent.spectatorRoute == SPECTATE_ROUTE_NONE
            && intent.sessionId != 0
            && (intent.keyLow != 0 || intent.keyHigh != 0)
            && intent.viewerId != 0
            && intent.resumeRecord == 0 && intent.resumeDigest == 0
            && intent.spectatorCapabilities == 0;
    }
    const bool credentialEmpty =
        intent.sessionId == 0 && intent.keyLow == 0 && intent.keyHigh == 0;
    const bool credentialComplete =
        intent.sessionId != 0
        && (intent.keyLow != 0 || intent.keyHigh != 0);
    const bool legacyTopology = intent.spectatorCapabilities
        == kSpectateCapabilityRelayTopology;
    const bool releasedTopologyV2 = intent.spectatorCapabilities
        == kSpectateCapabilityReleasedTopologyV2Mask;
    if (legacyTopology || releasedTopologyV2) {
        return credentialComplete
            && intent.viewerId != 0
            && intent.resumeRecord != 0
            && intent.resumeDigest == 0
            && (intent.spectatorRoute == SPECTATE_ROUTE_DIRECT
                || intent.spectatorRoute == SPECTATE_ROUTE_PUNCHED
                || intent.spectatorRoute == SPECTATE_ROUTE_RELAY);
    }
    return intent.viewerId != 0
        && intent.spectatorCapabilities == 0
        && (intent.spectatorRoute == SPECTATE_ROUTE_DIRECT
            || intent.spectatorRoute == SPECTATE_ROUTE_PUNCHED
            || intent.spectatorRoute == SPECTATE_ROUTE_RELAY)
        && (credentialEmpty || credentialComplete);
}

bool clientIntentRecordSelfTest() {
    ClientIntent status{};
    status.intent = INTENT_STATUS;
    if (!validClientIntentRecord(status)) return false;
    ClientIntent malformedStatus = status;
    malformedStatus.viewerId = 1u;
    if (validClientIntentRecord(malformedStatus)) return false;
    malformedStatus = status;
    malformedStatus.spectatorRoute = SPECTATE_ROUTE_DIRECT;
    if (validClientIntentRecord(malformedStatus)) return false;

    ClientIntent release{};
    release.intent = INTENT_SPECTATE_RELEASE;
    release.spectatorRoute = SPECTATE_ROUTE_NONE;
    release.sessionId = 7;
    release.keyLow = 11;
    release.keyHigh = 13;
    release.viewerId = 17;
    if (!validClientIntentRecord(release)) return false;

    const auto rejects = [](ClientIntent candidate) {
        return !validClientIntentRecord(candidate);
    };
    ClientIntent changed = release;
    changed.spectatorRoute = SPECTATE_ROUTE_DIRECT;
    if (!rejects(changed)) return false;
    changed = release;
    changed.sessionId = 0;
    if (!rejects(changed)) return false;
    changed = release;
    changed.keyLow = 0;
    changed.keyHigh = 0;
    if (!rejects(changed)) return false;
    changed = release;
    changed.viewerId = 0;
    if (!rejects(changed)) return false;
    changed = release;
    changed.resumeRecord = 1;
    if (!rejects(changed)) return false;
    changed = release;
    changed.resumeDigest = 1;
    if (!rejects(changed)) return false;
    changed = release;
    changed.spectatorCapabilities = 1;
    if (!rejects(changed)) return false;
    changed = release;
    changed.reserved3 = 1;
    if (!rejects(changed)) return false;
    changed = release;
    changed.spectatorDataPort = 1;
    if (!rejects(changed)) return false;

    ClientIntent topology{};
    topology.intent = INTENT_SPECTATE;
    topology.spectatorRoute = SPECTATE_ROUTE_DIRECT;
    topology.spectatorCapabilities = kSpectateCapabilityRelayTopology;
    topology.sessionId = 7;
    topology.keyLow = 11;
    topology.keyHigh = 13;
    topology.viewerId = 17;
    topology.resumeRecord = 3;
    if (!validClientIntentRecord(topology)) return false;
    changed = topology;
    changed.spectatorRoute = SPECTATE_ROUTE_RELAY;
    if (!validClientIntentRecord(changed)) return false;
    changed = topology;
    changed.spectatorRoute = SPECTATE_ROUTE_RELAY;
    changed.resumeRecord = 0;
    if (!rejects(changed)) return false;
    changed = topology;
    changed.spectatorRoute = SPECTATE_ROUTE_NONE;
    if (!rejects(changed)) return false;
    changed = topology;
    changed.resumeRecord = 0;
    if (!rejects(changed)) return false;
    changed = topology;
    changed.resumeDigest = 1;
    if (!rejects(changed)) return false;
    changed = topology;
    changed.spectatorCapabilities =
        kSpectateCapabilityReleasedTopologyV2Mask;
    if (!validClientIntentRecord(changed)) return false;
    changed = topology;
    changed.spectatorCapabilities |= 4;
    if (!rejects(changed)) return false;
    return true;
}

SpectatorGrantWire grantToWire(
    const qnetplay::spectator::SpectatorGrant& grant) {
    SpectatorGrantWire wire{};
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

bool grantFromWire(const SpectatorGrantWire& wire,
                   qnetplay::spectator::SpectatorGrant& grant) {
    for (const uint8_t byte : wire.reserved)
        if (byte != 0) return false;
    if (wire.grantId == 0 || wire.viewerId == 0 || wire.sessionId == 0
        || wire.expiryUnix == 0 || wire.sourceGeneration == 0
        || (wire.macLow == 0 && wire.macHigh == 0)
        || (wire.route != SPECTATE_ROUTE_DIRECT
            && wire.route != SPECTATE_ROUTE_PUNCHED
            && wire.route != SPECTATE_ROUTE_RELAY)) {
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

void clearSpectateSecrets(SpectateInfo& answer) {
    answer.keyLow = 0;
    answer.keyHigh = 0;
    answer.relayViewerAccessLow = 0;
    answer.relayViewerAccessHigh = 0;
    answer.grant = {};
}

void storePresenceName(char (&destination)[32], const std::string& utf8) {
    qgame::storeCanonicalPlayerNameForCodePage(destination, utf8, CP_UTF8);
}

LivePresence makeLivePresence(const std::string& playerOne,
                              const std::string& playerTwo,
                              uint8_t roundsToWin,
                              uint32_t sessionId,
                              LivePresenceState state) {
    LivePresence presence{};
    presence.schemaVersion = kLivePresenceSchemaVersion;
    presence.state = static_cast<uint8_t>(state);
    presence.sessionId = sessionId;
    presence.roundsToWin = roundsToWin;
    std::memset(presence.characterIds, 0xFF,
                sizeof(presence.characterIds));
    storePresenceName(presence.playerNames[0], playerOne);
    if (!playerTwo.empty())
        storePresenceName(presence.playerNames[1], playerTwo);
    return presence;
}

void refreshLivePresenceFromSidecar(
    LivePresence& presence,
    const qclient::spectator::SidecarStatus& sidecar) {
    presence.viewerCount = static_cast<uint16_t>(
        (std::min<size_t>)(sidecar.viewerCount, 0xFFFFu));
    presence.viewerCapacity = static_cast<uint16_t>(
        (std::min<size_t>)(sidecar.viewerCapacity, 0xFFFFu));
    presence.confirmedTimelineFrame = sidecar.presenceTimelineFrame;
    presence.flags = 0u;

    if (sidecar.presenceActiveFighterMask != 0u) {
        presence.flags |= kLivePresenceRosterValid;
        presence.activeFighterMask =
            sidecar.presenceActiveFighterMask;
        std::copy(sidecar.presenceCharacterIds.begin(),
                  sidecar.presenceCharacterIds.end(),
                  presence.characterIds);
        std::copy(sidecar.presencePaletteIndices.begin(),
                  sidecar.presencePaletteIndices.end(),
                  presence.paletteIndices);
    } else {
        presence.activeFighterMask = 0u;
        std::memset(presence.characterIds, 0xFF,
                    sizeof(presence.characterIds));
        std::memset(presence.paletteIndices, 0,
                    sizeof(presence.paletteIndices));
    }
    if (sidecar.presenceHasCurrentGame) {
        presence.flags |= kLivePresenceRoundScoreValid;
        std::copy(sidecar.presenceRoundWins.begin(),
                  sidecar.presenceRoundWins.end(), presence.roundWins);
        presence.state = static_cast<uint8_t>(LIVE_PRESENCE_MATCH);
    } else {
        presence.roundWins[0] = 0u;
        presence.roundWins[1] = 0u;
        presence.state = static_cast<uint8_t>(
            LIVE_PRESENCE_CHARACTER_SELECT);
    }
    if (sidecar.connectedV4Bound) {
        presence.flags |= kLivePresenceSetScoreValid;
        std::copy(sidecar.presenceMatchWins.begin(),
                  sidecar.presenceMatchWins.end(), presence.matchWins);
    } else {
        presence.matchWins[0] = 0u;
        presence.matchWins[1] = 0u;
    }
    for (size_t lane = 0u; lane < 2u; ++lane) {
        if (!sidecar.presencePlayerNames[lane].empty()) {
            storePresenceName(presence.playerNames[lane],
                              sidecar.presencePlayerNames[lane]);
        }
    }
    if (sidecar.phase == qclient::spectator::SidecarPhase::Retained
        || sidecar.phase == qclient::spectator::SidecarPhase::Sealed
        || sidecar.phase == qclient::spectator::SidecarPhase::Draining
        || sidecar.phase == qclient::spectator::SidecarPhase::Expired) {
        presence.state = static_cast<uint8_t>(LIVE_PRESENCE_RETAINED);
    }
}

bool topologyInfoFromEnrollment(
    const uint32_t sessionId,
    const qclient::spectator::topology::TopologyEnrollment& enrollment,
    SpectateTopologyInfo& out) {
    out = {};
    if (sessionId == 0u || !enrollment.granted
        || enrollment.authorityGeneration == 0u
        || enrollment.rootNodeId
               == qnetplay::spectator::resilient::kInvalidNodeId
        || enrollment.nodeId
               == qnetplay::spectator::resilient::kInvalidNodeId
        || enrollment.rootNodeId == enrollment.nodeId
        || enrollment.expiresMs == 0u || enrollment.bindingGeneration == 0u
        || !enrollment.rootQsp.validForAuthority()
        || !enrollment.rootQsr.validForAuthority()
        || !enrollment.rootQsp.advertised.routeProved) {
        return false;
    }
    uint8_t credentialOr = 0u;
    for (const uint8_t byte : enrollment.credential) credentialOr |= byte;
    if (credentialOr == 0u) return false;

    out.state = SPECTATE_TOPOLOGY_GRANTED;
    out.rootQspRouteKind = static_cast<uint8_t>(
        enrollment.rootQsp.advertised.kind);
    out.rootQspRouteProved = 1u;
    out.rootQsrRouteKind = static_cast<uint8_t>(
        enrollment.rootQsr.advertised.kind);
    out.rootQsrRouteProved = enrollment.rootQsr.advertised.routeProved ? 1u : 0u;
    out.sessionId = sessionId;
    out.authorityGeneration = enrollment.authorityGeneration;
    out.rootNodeId = enrollment.rootNodeId;
    out.childNodeId = enrollment.nodeId;
    out.expiryMs = enrollment.expiresMs;
    out.bindingGeneration = enrollment.bindingGeneration;
    out.rootQspIpv4BE = enrollment.rootQsp.advertised.ipv4BE;
    out.rootQspPort = enrollment.rootQsp.advertised.port;
    out.rootQspRouteGeneration =
        enrollment.rootQsp.advertised.generation;
    out.rootQsrIpv4BE = enrollment.rootQsr.advertised.ipv4BE;
    out.rootQsrPort = enrollment.rootQsr.advertised.port;
    out.rootQsrRouteGeneration =
        enrollment.rootQsr.advertised.generation;
    std::memcpy(out.topologyCredential, enrollment.credential.data(),
                sizeof(out.topologyCredential));
    return true;
}

void wipeTopologyControlBuffer(std::vector<uint8_t>& bytes) {
    if (!bytes.empty()) {
        volatile uint8_t* cursor = bytes.data();
        size_t remaining = bytes.size();
        while (remaining-- != 0u) *cursor++ = 0u;
    }
    bytes.clear();
}

bool topologyEndpointBindingToWire(
    const qclient::spectator::topology::EndpointBinding& binding,
    SpectateTopologyEndpointBindingWire& out) {
    out = {};
    if (!binding.validForAuthority()) return false;
    out.localIpv4BE = binding.local.ipv4BE;
    out.localPort = binding.local.port;
    out.advertisedRouteKind =
        static_cast<uint8_t>(binding.advertised.kind);
    out.advertisedIpv4BE = binding.advertised.ipv4BE;
    out.advertisedPort = binding.advertised.port;
    out.advertisedRouteGeneration = binding.advertised.generation;
    out.advertisedRouteProved = binding.advertised.routeProved ? 1u : 0u;
    out.localProvenance = static_cast<uint8_t>(binding.localProvenance);
    out.advertisedProvenance =
        static_cast<uint8_t>(binding.advertisedProvenance);
    out.bindingGeneration = binding.generation;
    return true;
}

bool topologyInfoV2FromEnrollment(
    const uint64_t requestNonce,
    const qclient::spectator::topology::TopologyEnrollment& enrollment,
    SpectateTopologyInfoV2& out) {
    using qclient::spectator::topology::EndpointProvenance;
    out = {};
    const bool qspDirect = enrollment.qspProvenance
        == EndpointProvenance::ProvenDirectQsp;
    const bool qspPunched = enrollment.qspProvenance
        == EndpointProvenance::ProvenPunchedQsp;
    if (requestNonce == 0u || !enrollment.granted
        || !qnetplay::spectator::isValidIdentity(enrollment.session)
        || !qnetplay::compat::isValidCompatibilityIdentity(
               enrollment.compatibility)
        || enrollment.streamRootIdentity == 0u
        || enrollment.sourceGeneration == 0u
        || enrollment.authorityGeneration == 0u
        || enrollment.rootNodeId
               == qnetplay::spectator::resilient::kInvalidNodeId
        || enrollment.nodeId
               == qnetplay::spectator::resilient::kInvalidNodeId
        || enrollment.rootNodeId == enrollment.nodeId
        || enrollment.expiresMs == 0u || enrollment.bindingGeneration == 0u
        || enrollment.viewerId == 0u || enrollment.viewerGeneration == 0u
        || !enrollment.qspEndpoint.valid() || !(qspDirect || qspPunched)
        || enrollment.qspRouteGeneration == 0u || !enrollment.limits.valid()
        || !topologyEndpointBindingToWire(enrollment.rootQsp, out.rootQsp)
        || !topologyEndpointBindingToWire(enrollment.rootQsr, out.rootQsr)
        || out.rootQsr.advertisedRouteKind
               != static_cast<uint8_t>(
                   qnetplay::spectator::resilient::RouteKind::Direct)
        || out.rootQsp.advertisedRouteProved != 1u
        || out.rootQsr.advertisedRouteProved != 0u
        || out.rootQsr.advertisedProvenance
               != static_cast<uint8_t>(
                   qclient::spectator::topology::EndpointProvenance::
                       PublishedDirectCandidate)
        || out.rootQsp.advertisedPort == out.rootQsr.advertisedPort
        || out.rootQsp.localPort == out.rootQsr.localPort) {
        out = {};
        return false;
    }
    uint8_t credentialOr = 0u;
    for (const uint8_t byte : enrollment.credential) credentialOr |= byte;
    if (credentialOr == 0u) {
        out = {};
        return false;
    }

    out.schemaVersion = kSpectateTopologyControlSchemaVersion;
    out.requiredFeatures = kSpectateTopologyControlRequiredFeatures;
    out.state = SPECTATE_TOPOLOGY_GRANTED;
    out.qspRoute = qspDirect ? SPECTATE_ROUTE_DIRECT
                              : SPECTATE_ROUTE_PUNCHED;
    out.qspProvenance = static_cast<uint8_t>(enrollment.qspProvenance);
    out.rootQspRouteKind = out.rootQsp.advertisedRouteKind;
    out.rootQspRouteProved = out.rootQsp.advertisedRouteProved;
    out.rootQsrRouteKind = out.rootQsr.advertisedRouteKind;
    out.rootQsrRouteProved = out.rootQsr.advertisedRouteProved;
    out.requestNonce = requestNonce;
    out.sessionId = enrollment.session.sessionId;
    out.sessionEpoch = enrollment.session.epoch;
    out.viewerId = enrollment.viewerId;
    out.viewerGeneration = enrollment.viewerGeneration;
    out.confirmedCursor = enrollment.confirmedCursor;
    out.streamRootIdentity = enrollment.streamRootIdentity;
    out.compatibilityIdentity = enrollment.compatibility;
    out.authorityGeneration = enrollment.authorityGeneration;
    out.sourceGeneration = enrollment.sourceGeneration;
    out.rootNodeId = enrollment.rootNodeId;
    out.childNodeId = enrollment.nodeId;
    out.expiryMs = enrollment.expiresMs;
    out.bindingGeneration = enrollment.bindingGeneration;
    out.qspIpv4BE = enrollment.qspEndpoint.ipv4BE;
    out.qspPort = enrollment.qspEndpoint.port;
    out.qspRouteGeneration = enrollment.qspRouteGeneration;
    out.qtaCursor.nextRecord = enrollment.qtaCursor.nextRecord;
    out.qtaCursor.nextEvent = enrollment.qtaCursor.nextEvent;
    out.qtaCursor.digest = enrollment.qtaCursor.digest;
    out.qtaLimits.maxTopologyNodes =
        static_cast<uint64_t>(enrollment.limits.maxTopologyNodes);
    out.qtaLimits.maxActiveDepth = enrollment.limits.maxActiveDepth;
    out.qtaLimits.maxChildrenPerRoot =
        static_cast<uint64_t>(enrollment.limits.maxChildrenPerRoot);
    out.qtaLimits.maxChildrenPerRelay =
        static_cast<uint64_t>(enrollment.limits.maxChildrenPerRelay);
    out.qtaLimits.maxStandbyParents =
        static_cast<uint64_t>(enrollment.limits.maxStandbyParents);
    out.qtaLimits.standbyLeaseMs = enrollment.limits.standbyLeaseMs;
    std::memcpy(out.topologyCredential, enrollment.credential.data(),
                sizeof(out.topologyCredential));
    return true;
}

bool makeTopologyV2Canonical(
    const qclient::spectator::topology::client::TopologyControlV2Direction
        direction,
    const void* record, const size_t bytesWithoutTag,
    std::vector<uint8_t>& out) {
    return qclient::spectator::topology::client::
        makeTopologyControlV2CanonicalInput(
            direction, static_cast<const uint8_t*>(record), bytesWithoutTag,
            out);
}

bool signTopologyInfoV2(
    SpectateTopologyInfoV2& info,
    const qnetplay::spectator::SpectatorGrant& grant) {
    std::vector<uint8_t> canonical;
    std::array<uint8_t,
               qclient::spectator::topology::client::kQtaMacBytes> tag{};
    const bool ready = makeTopologyV2Canonical(
            qclient::spectator::topology::client::
                TopologyControlV2Direction::Response,
            &info, offsetof(SpectateTopologyInfoV2, responseTag), canonical)
        && qclient::spectator::topology::client::makeTopologyControlV2Tag(
            grant.macLow, grant.macHigh, canonical.data(), canonical.size(),
            tag);
    wipeTopologyControlBuffer(canonical);
    if (!ready) return false;
    std::memcpy(info.responseTag, tag.data(), tag.size());
    volatile uint8_t* cursor = tag.data();
    for (size_t index = 0u; index < tag.size(); ++index) cursor[index] = 0u;
    return true;
}

struct ClientRootOfferState {
    std::mutex mutex;
    bool active = false;
    bool consumed = false;
    uint8_t token[qnet::kClientRootTokenBytes]{};
    uint32_t sessionId = 0;
    uint64_t sessionEpoch = 0;
    uint8_t remotePlayerRole = 0;
    uint64_t expiresAtMs = 0;
};
ClientRootOfferState g_clientRootOffer;

struct ClientRootOfferRetained {
    bool valid = false;
    qnet::ClientRootOffer offer{};
};
ClientRootOfferRetained g_clientRootOfferRetained;

void wipeClientRootToken(uint8_t (&token)[qnet::kClientRootTokenBytes]) {
    volatile uint8_t* cursor = token;
    for (size_t index = 0u; index < sizeof(token); ++index) cursor[index] = 0u;
}

void hostMintAndSendClientRootOffer(ControlLink& ctrl,
                                    const uint32_t sessionId) {
    qnet::ClientRootOffer offer{};
    offer.schemaVersion = 1u;
    offer.remotePlayerRole = 2u;
    offer.sessionId = sessionId;
    offer.sessionEpoch = 1u;
    offer.offerExpiryMs = nowUs() / 1000ull + 600000ull;
    offer.hostSourceGeneration = 1u;
    bool degraded = false;
    const bool generated = qnet::secureRandomBytes(
        offer.claimToken, sizeof(offer.claimToken), degraded);
    uint8_t tokenOr = 0u;
    for (const uint8_t byte : offer.claimToken) tokenOr |= byte;
    if (!generated || degraded || tokenOr == 0u) {
        std::memset(offer.claimToken, 0, sizeof(offer.claimToken));
        QLOG("[client-root] offer disabled: no secure random source");
    } else {
        std::lock_guard<std::mutex> lock(g_clientRootOffer.mutex);
        g_clientRootOffer.active = true;
        g_clientRootOffer.consumed = false;
        std::memcpy(g_clientRootOffer.token, offer.claimToken,
                    sizeof(g_clientRootOffer.token));
        g_clientRootOffer.sessionId = sessionId;
        g_clientRootOffer.sessionEpoch = offer.sessionEpoch;
        g_clientRootOffer.remotePlayerRole = offer.remotePlayerRole;
        g_clientRootOffer.expiresAtMs = offer.offerExpiryMs;
    }
    (void)ctrl.sendMsg(MSG_CLIENT_ROOT_OFFER, &offer, sizeof(offer));
    std::memset(offer.claimToken, 0, sizeof(offer.claimToken));
}

template <typename Link>
void handleClientRootClaimVisitor(Link& visitorLink, PumpCtx& ctx,
                                  const ClientIntent& intent) {
    namespace clientroot = qclient::spectator::topology::clientroot;
    uint8_t token[qnet::kClientRootTokenBytes]{};
    uint64_t sessionEpoch = 0u;
    uint32_t sessionId = 0u;
    uint8_t expectedRole = 0u;
    {
        std::lock_guard<std::mutex> lock(g_clientRootOffer.mutex);
        if (!g_clientRootOffer.active || g_clientRootOffer.consumed
            || intent.sessionId != g_clientRootOffer.sessionId
            || nowUs() / 1000ull >= g_clientRootOffer.expiresAtMs) {
            return;
        }
        std::memcpy(token, g_clientRootOffer.token, sizeof(token));
        sessionId = g_clientRootOffer.sessionId;
        sessionEpoch = g_clientRootOffer.sessionEpoch;
        expectedRole = g_clientRootOffer.remotePlayerRole;
    }
    uint8_t challenge[16]{};
    bool degraded = false;
    const bool generated =
        qnet::secureRandomBytes(challenge, sizeof(challenge), degraded);
    uint8_t challengeOr = 0u;
    for (const uint8_t byte : challenge) challengeOr |= byte;
    if (!generated || degraded || challengeOr == 0u
        || !visitorLink.sendMsg(MSG_CLIENT_ROOT_CHALLENGE, challenge,
                                sizeof(challenge))) {
        wipeClientRootToken(token);
        return;
    }
    TcpConn::Received rx;
    std::string remoteErr;
    if (pumpRecv(visitorLink, ctx, MSG_CLIENT_ROOT_CLAIM, 3000, rx,
                 &remoteErr) != Pump::Got) {
        wipeClientRootToken(token);
        return;
    }
    qnet::ClientRootClaim claim{};
    if (!bodyAs(rx, claim)
        || std::memcmp(claim.peerChallenge, challenge, sizeof(challenge))
            != 0) {
        wipeClientRootToken(token);
        return;
    }
    std::array<uint8_t, 16u> claimTag{};
    std::memcpy(claimTag.data(), claim.requestTag, claimTag.size());
    if (!clientroot::verifyControlTag(
            token, sizeof(token), clientroot::Direction::Claim,
            reinterpret_cast<const uint8_t*>(&claim),
            offsetof(qnet::ClientRootClaim, requestTag), claimTag)) {
        wipeClientRootToken(token);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_clientRootOffer.mutex);
        g_clientRootOffer.consumed = true;
    }
    qnet::ClientRootEnrollment response{};
    response.schemaVersion = 1u;
    response.granted = 0u;
    response.requestNonce = claim.requestNonce;
    qclient::spectator::topology::ClientPlayerRootClaim authorityClaim{};
    if (clientroot::claimFromWire(claim, expectedRole, sessionId,
                                  sessionEpoch, authorityClaim)) {
        qclient::spectator::topology::ClientPlayerRootEnrollment enrollment{};
        std::vector<uint8_t> encodedGrant;
        std::string enrollError;
        if (qclient::spectator::sidecarEnrollClientPlayerRoot(
                authorityClaim, nowUs() / 1000ull, enrollment, encodedGrant,
                enrollError)) {
            if (!clientroot::enrollmentToWire(claim.requestNonce, enrollment,
                                              encodedGrant.data(),
                                              encodedGrant.size(),
                                              response)) {
                response = {};
                response.schemaVersion = 1u;
                response.granted = 0u;
                response.requestNonce = claim.requestNonce;
            }
        } else {
            QLOG("[client-root] enrollment refused: %s", enrollError.c_str());
        }
        std::fill(enrollment.credential.begin(), enrollment.credential.end(),
                  uint8_t{0u});
        std::fill(encodedGrant.begin(), encodedGrant.end(), uint8_t{0u});
    }
    std::array<uint8_t, 16u> responseTag{};
    if (clientroot::makeControlTag(
            token, sizeof(token), clientroot::Direction::Enrollment,
            reinterpret_cast<const uint8_t*>(&response),
            offsetof(qnet::ClientRootEnrollment, responseTag), responseTag)) {
        std::memcpy(response.responseTag, responseTag.data(),
                    responseTag.size());
        (void)visitorLink.sendMsg(MSG_CLIENT_ROOT_ENROLLMENT, &response,
                                  sizeof(response));
        QLOG("[client-root] claim %s for session %u",
             response.granted == 1u ? "granted" : "refused", sessionId);
    }
    std::memset(&response, 0, sizeof(response));
    responseTag.fill(0u);
    wipeClientRootToken(token);
    visitorLink.closeGraceful(200);
}

bool localIpv4Toward(const Endpoint& host, uint32_t& outIpv4BE) {
    outIpv4BE = 0u;
    UdpSock probe;
    if (!probe.open(0)) return false;
    if (::connect(probe.raw(), reinterpret_cast<const sockaddr*>(&host.sa),
                  sizeof(host.sa)) != 0) {
        return false;
    }
    sockaddr_in local{};
    int length = sizeof(local);
    if (::getsockname(probe.raw(), reinterpret_cast<sockaddr*>(&local),
                      &length) != 0
        || local.sin_addr.s_addr == 0u) {
        return false;
    }
    outIpv4BE = local.sin_addr.s_addr;
    return true;
}

void runClientRootClaimTask(const std::string& hostIp,
                            const uint16_t hostControlPort) {
    namespace clientroot = qclient::spectator::topology::clientroot;
    if (!g_clientRootOfferRetained.valid) return;
    qnet::ClientRootOffer offer = g_clientRootOfferRetained.offer;
    g_clientRootOfferRetained.valid = false;
    uint8_t tokenOr = 0u;
    for (const uint8_t byte : offer.claimToken) tokenOr |= byte;
    if (tokenOr == 0u) {
        QLOG("[client-root] no usable launch offer; collector stays dormant");
        return;
    }
    uint16_t qsrPort = 0u;
    std::string prepareError;
    if (!qclient::spectator::sidecarPrepareClientPlayerRootQsr(qsrPort,
                                                               prepareError)) {
        QLOG("[client-root] QSR prepare failed: %s", prepareError.c_str());
        std::memset(offer.claimToken, 0, sizeof(offer.claimToken));
        return;
    }
    qclient::spectator::ClientPrivateRootStreamFacts facts{};
    if (!qclient::spectator::sidecarClientPrivateRootStreamFacts(facts)
        || !facts.available) {
        QLOG("[client-root] confirmed stream facts unavailable; "
             "collector stays dormant");
        std::memset(offer.claimToken, 0, sizeof(offer.claimToken));
        return;
    }
    Endpoint host{};
    uint32_t localIpv4BE = 0u;
    if (!Endpoint::parse(hostIp, hostControlPort, host)
        || !localIpv4Toward(host, localIpv4BE)) {
        QLOG("[client-root] host control endpoint unusable for the claim");
        std::memset(offer.claimToken, 0, sizeof(offer.claimToken));
        return;
    }
    bool succeeded = false;
    for (unsigned attempt = 0u; attempt < 3u && !succeeded; ++attempt) {
        UdpSock claimSocket;
        if (!claimSocket.open(0)) break;
        qnet::UdpControlLink claimLink;
        std::string dialError;
        if (!dialHostControl(claimSocket, host, claimLink, nullptr, 5000,
                             dialError)) {
            continue;
        }
        PumpCtx ctx;
        ctx.lastHeardUs = nowUs();
        TcpConn::Received rx;
        std::string remoteErr;
        VersionConfig hostVer{};
        if (pumpRecv(claimLink, ctx, MSG_VERSION_CONFIG, 4000, rx, &remoteErr)
                != Pump::Got
            || !bodyAs(rx, hostVer)
            || hostVer.protocolVersion != kProtocolVersion) {
            continue;
        }
        VersionConfig myVer{kProtocolVersion};
        claimLink.sendMsg(MSG_VERSION_CONFIG, &myVer, sizeof(myVer));
        ClientIntent intent{};
        intent.intent = INTENT_CLIENT_ROOT;
        intent.sessionId = offer.sessionId;
        claimLink.sendMsg(MSG_CLIENT_INTENT, &intent, sizeof(intent));
        if (pumpRecv(claimLink, ctx, MSG_CLIENT_ROOT_CHALLENGE, 4000, rx,
                     &remoteErr) != Pump::Got
            || rx.body.size() != 16u) {
            continue;
        }
        qnet::ClientRootClaim claim{};
        claim.schemaVersion = 1u;
        claim.remotePlayerRole = offer.remotePlayerRole;
        claim.playerSlot = facts.playerSlot;
        bool nonceDegraded = false;
        claim.requestNonce = qnet::secureRandom64(nonceDegraded);
        if (claim.requestNonce == 0u) claim.requestNonce = 1u;
        std::memcpy(claim.peerChallenge, rx.body.data(), 16u);
        claim.sessionId = offer.sessionId;
        claim.sessionEpoch = offer.sessionEpoch;
        claim.processId = facts.gameProcessId;
        claim.publisherGeneration =
            static_cast<uint32_t>(facts.collectorGeneration);
        claim.publisherNonce = facts.publisherNonce;
        claim.runtimeStage = 7u;
        claim.delay = facts.appliedDelay;
        claim.rollback = facts.appliedRollback;
        claim.remoteDelay = facts.appliedRemoteDelay;
        claim.remoteRollback = facts.appliedRemoteRollback;
        claim.provenance = facts.provenance;
        claim.compatibilityIdentity = facts.compatibility;
        claim.streamRootIdentity = facts.streamRootIdentity;
        claim.gameStartFingerprint = facts.gameStartFingerprint;
        claim.sourceGeneration = offer.hostSourceGeneration;
        claim.qspRouteGeneration = 1u;
        claim.qsrBindingGeneration = 1u;
        claim.dataRoute.kind = 0u;
        claim.dataRoute.routeProved = 1u;
        claim.dataRoute.port = facts.collectorQspPort;
        claim.dataRoute.ipv4BE = localIpv4BE;
        claim.dataRoute.generation = 1u;
        claim.qsrRoute.kind = 0u;
        claim.qsrRoute.routeProved = 1u;
        claim.qsrRoute.port = qsrPort;
        claim.qsrRoute.ipv4BE = localIpv4BE;
        claim.qsrRoute.generation = 1u;
        claim.cacheFirstCursor = facts.cacheFirstCursor;
        claim.cacheNextCursor = facts.cacheNextCursor;
        claim.cursorNextRecord = facts.cacheNextCursor;
        claim.cursorNextEvent = facts.nextEvent;
        claim.cursorDigest = facts.chainDigest;
        claim.childCapacity = 4u;
        claim.expiresMs = offer.offerExpiryMs;
        std::array<uint8_t, 16u> tag{};
        if (!clientroot::makeControlTag(
                offer.claimToken, sizeof(offer.claimToken),
                clientroot::Direction::Claim,
                reinterpret_cast<const uint8_t*>(&claim),
                offsetof(qnet::ClientRootClaim, requestTag), tag)) {
            std::memset(&claim, 0, sizeof(claim));
            continue;
        }
        std::memcpy(claim.requestTag, tag.data(), tag.size());
        tag.fill(0u);
        claimLink.sendMsg(MSG_CLIENT_ROOT_CLAIM, &claim, sizeof(claim));
        const uint64_t requestNonce = claim.requestNonce;
        std::memset(&claim, 0, sizeof(claim));
        if (pumpRecv(claimLink, ctx, MSG_CLIENT_ROOT_ENROLLMENT, 5000, rx,
                     &remoteErr) != Pump::Got) {
            continue;
        }
        qnet::ClientRootEnrollment enrollment{};
        if (!bodyAs(rx, enrollment)) continue;
        std::array<uint8_t, 16u> responseTag{};
        std::memcpy(responseTag.data(), enrollment.responseTag,
                    responseTag.size());
        if (!clientroot::verifyControlTag(
                offer.claimToken, sizeof(offer.claimToken),
                clientroot::Direction::Enrollment,
                reinterpret_cast<const uint8_t*>(&enrollment),
                offsetof(qnet::ClientRootEnrollment, responseTag),
                responseTag)) {
            std::memset(&enrollment, 0, sizeof(enrollment));
            continue;
        }
        if (enrollment.granted != 1u) {
            QLOG("[client-root] host refused the enrollment; collector "
                 "stays dormant");
            std::memset(&enrollment, 0, sizeof(enrollment));
            break;
        }
        std::array<uint8_t, qnet::kClientRootCredentialBytes> credential{};
        std::vector<uint8_t> grantBlob;
        if (!clientroot::enrollmentFromWire(enrollment, requestNonce,
                                            credential, grantBlob)) {
            std::memset(&enrollment, 0, sizeof(enrollment));
            continue;
        }
        qclient::spectator::ClientPlayerRootArmFacts armFacts{};
        armFacts.clientRootNodeId = enrollment.clientRootNodeId;
        armFacts.hostRootNodeId = enrollment.hostRootNodeId;
        armFacts.authorityGeneration = enrollment.authorityGeneration;
        armFacts.expiresMs = enrollment.expiresMs;
        armFacts.rootQsp = enrollment.rootQsp;
        armFacts.rootQsr = enrollment.rootQsr;
        armFacts.hostSourceGeneration = enrollment.hostSourceGeneration;
        std::memcpy(armFacts.authorityPublicKey.data(),
                    enrollment.authorityPublicKey,
                    armFacts.authorityPublicKey.size());
        armFacts.credential = credential;
        armFacts.grantBlob = std::move(grantBlob);
        std::string armError;
        if (qclient::spectator::sidecarArmClientPlayerRoot(armFacts,
                                                           armError)) {
            QLOG("[client-root] enrolled and armed; asserting to the host "
                 "authority");
            succeeded = true;
        } else {
            QLOG("[client-root] arm failed: %s", armError.c_str());
        }
        std::fill(armFacts.credential.begin(), armFacts.credential.end(),
                  uint8_t{0u});
        std::fill(armFacts.grantBlob.begin(), armFacts.grantBlob.end(),
                  uint8_t{0u});
        credential.fill(0u);
        std::memset(&enrollment, 0, sizeof(enrollment));
        claimLink.closeGraceful(200);
    }
    std::memset(offer.claimToken, 0, sizeof(offer.claimToken));
    if (!succeeded) {
        QLOG("[client-root] claim task ended without an armed root; the "
             "player match is unaffected");
    }
}

bool verifyTopologyProofV2Tag(
    const SpectateTopologyProofV2& proof,
    const qnetplay::spectator::SpectatorGrant& grant) {
    std::vector<uint8_t> canonical;
    std::array<uint8_t,
               qclient::spectator::topology::client::kQtaMacBytes> tag{};
    std::memcpy(tag.data(), proof.requestTag, tag.size());
    const bool accepted = makeTopologyV2Canonical(
            qclient::spectator::topology::client::
                TopologyControlV2Direction::Request,
            &proof, offsetof(SpectateTopologyProofV2, requestTag), canonical)
        && qclient::spectator::topology::client::verifyTopologyControlV2Tag(
            grant.macLow, grant.macHigh, canonical.data(), canonical.size(),
            tag);
    wipeTopologyControlBuffer(canonical);
    volatile uint8_t* cursor = tag.data();
    for (size_t index = 0u; index < tag.size(); ++index) cursor[index] = 0u;
    return accepted;
}

bool validateTopologyProofV2(
    const SpectateTopologyProofV2& proof, const ClientIntent& intent,
    const SpectateInfo& answer,
    qnetplay::spectator::SpectatorGrant& grant) {
    grant = {};
    if (proof.schemaVersion != kSpectateTopologyControlSchemaVersion
        || proof.requiredFeatures != kSpectateTopologyControlRequiredFeatures
        || proof.requestNonce == 0u || proof.viewerId != intent.viewerId
        || proof.viewerGeneration != intent.resumeRecord
        || proof.qspRoute != intent.spectatorRoute
        || proof.qspRouteGeneration == 0u || proof.qspIpv4BE == 0u
        || proof.qspPort == 0u || proof.streamRootIdentity == 0u
        || proof.reservedRoute[0] != 0u || proof.reservedRoute[1] != 0u
        || proof.reservedRoute[2] != 0u || proof.reservedEndpoint != 0u
        || !qnetplay::compat::isValidCompatibilityIdentity(
               proof.compatibilityIdentity)
        || !grantFromWire(proof.grant, grant)
        || grant.viewerId != intent.viewerId
        || grant.sessionId != answer.sessionId
        || grant.route != proof.qspRoute
        || (proof.qspRoute != SPECTATE_ROUTE_DIRECT
            && proof.qspRoute != SPECTATE_ROUTE_PUNCHED)) {
        grant = {};
        return false;
    }
    return verifyTopologyProofV2Tag(proof, grant);
}

void serveIntentDuringMatch(HostControlEndpoint& control,
                            const SpectateInfo& live,
                            qui::Console* con) {
    constexpr int kSpectateInfoCloseGraceMs = 1500;
    control.service(0);
    qnet::UdpControlLink* visitor = control.acceptOne();
    if (visitor == nullptr) return;
    struct Release {
        HostControlEndpoint& control;
        qnet::UdpControlLink* link;
        ~Release() { control.release(link); }
    } release{ control, visitor };
    qnet::UdpControlLink& visitorLink = *visitor;
    QLOG("[host-control-visitor] stage=accepted peer=%s",
         visitorLink.peer().str().c_str());
    PumpCtx ctx;
    ctx.lastHeardUs = nowUs();
    TcpConn::Received rx;
    std::string remoteErr;
    VersionConfig myVer{ kProtocolVersion };
    if (!visitorLink.sendMsg(MSG_VERSION_CONFIG, &myVer, sizeof(myVer))) {
        QLOG("[host-control-visitor] stage=version-send result=failed "
             "peer=%s socketError=%d",
             visitorLink.peer().str().c_str(), visitorLink.lastError());
        return;
    }
    const Pump versionPump =
        pumpRecv(visitorLink, ctx, MSG_VERSION_CONFIG, 2000, rx, &remoteErr);
    if (versionPump != Pump::Got) {
        QLOG("[host-control-visitor] stage=version-receive result=failed "
             "peer=%s pump=%d socketError=%d remote='%s'",
             visitorLink.peer().str().c_str(),
             static_cast<int>(versionPump), visitorLink.lastError(),
             remoteErr.c_str());
        return;
    }
    VersionConfig theirVer{};
    const bool versionDecoded = bodyAs(rx, theirVer);
    if (!versionDecoded || theirVer.protocolVersion != kProtocolVersion) {
        ErrorText et{};
        std::snprintf(et.text, sizeof(et.text),
                      "Protocol mismatch: host v%u - update the older side",
                      kProtocolVersion);
        visitorLink.sendMsg(MSG_ERROR_TEXT, &et, sizeof(et));
        visitorLink.closeGraceful(kSpectateInfoCloseGraceMs);
        QLOG("[host-control-visitor] stage=version result=%s peer=%s "
             "remote=%u local=%u bodyBytes=%zu",
             versionDecoded ? "mismatch" : "malformed",
             visitorLink.peer().str().c_str(), theirVer.protocolVersion,
             kProtocolVersion, rx.body.size());
        return;
    }
    const Pump intentPump =
        pumpRecv(visitorLink, ctx, MSG_CLIENT_INTENT, 2000, rx, &remoteErr);
    if (intentPump != Pump::Got) {
        QLOG("[host-control-visitor] stage=intent-receive result=failed "
             "peer=%s pump=%d socketError=%d remote='%s'",
             visitorLink.peer().str().c_str(),
             static_cast<int>(intentPump), visitorLink.lastError(),
             remoteErr.c_str());
        return;
    }
    ClientIntent intent{};
    if (!bodyAs(rx, intent) || !validClientIntentRecord(intent)) {
        QLOG("[host-control-visitor] stage=intent result=malformed peer=%s "
             "bodyBytes=%zu",
             visitorLink.peer().str().c_str(), rx.body.size());
        return;
    }
    QLOG("[host-control-visitor] stage=intent result=ok peer=%s intent=%u "
         "requestedSession=%u viewer=%s route=%u",
         visitorLink.peer().str().c_str(),
         static_cast<unsigned>(intent.intent), intent.sessionId,
         qclient::spectator::redactedTag(intent.viewerId).c_str(),
         static_cast<unsigned>(intent.spectatorRoute));
    SpectateInfo answer = live;
    const qclient::spectator::SidecarStatus sidecar =
        qclient::spectator::sidecarStatus();
    answer.viewerCount = static_cast<uint16_t>(
        (std::min<size_t>)(sidecar.viewerCount, 0xFFFFu));
    answer.viewerCapacity = static_cast<uint16_t>(
        (std::min<size_t>)(sidecar.viewerCapacity, 0xFFFFu));
    answer.admissionState = SPECTATE_ADMISSION_NONE;
    refreshLivePresenceFromSidecar(answer.presence, sidecar);

    if (intent.intent == INTENT_STATUS) {
        SpectateInfo publicAnswer{};
        publicAnswer.state = answer.state;
        publicAnswer.presence = answer.presence;
        const bool responseSent = visitorLink.sendMsg(
            MSG_SPECTATE_INFO, &publicAnswer, sizeof(publicAnswer));
        visitorLink.closeGraceful(kSpectateInfoCloseGraceMs);
        QLOG("[host-control-visitor] stage=response intent=status result=%s "
             "peer=%s presenceState=%u socketError=%d",
             responseSent ? "sent" : "failed",
             visitorLink.peer().str().c_str(),
             static_cast<unsigned>(publicAnswer.presence.state),
             visitorLink.lastError());
        return;
    }

    if (intent.intent == INTENT_SPECTATE_RELEASE) {
        const bool exactLiveCapability =
            answer.state == SPECTATE_LIVE
            && answer.sessionId != 0
            && intent.sessionId == answer.sessionId
            && intent.keyLow == answer.keyLow
            && intent.keyHigh == answer.keyHigh
            && (answer.keyLow != 0 || answer.keyHigh != 0);
        if (!exactLiveCapability) {
            QLOG("[spectator-admission] release refused viewer=%s "
                 "reason=credential-mismatch",
                 qclient::spectator::redactedTag(intent.viewerId).c_str());
            return;
        }
        const bool existed =
            qclient::spectator::sidecarReleaseViewerReservation(
                intent.viewerId);
        answer.state = SPECTATE_LIVE;
        answer.admissionState = SPECTATE_ADMISSION_RELEASED;
        answer.grant = {};
        visitorLink.sendMsg(MSG_SPECTATE_INFO, &answer, sizeof(answer));
        visitorLink.closeGraceful(kSpectateInfoCloseGraceMs);
        QLOG("[spectator-admission] release acknowledged viewer=%s "
             "present=%d",
             qclient::spectator::redactedTag(intent.viewerId).c_str(),
             existed ? 1 : 0);
        return;
    }

    if (intent.intent == INTENT_CLIENT_ROOT) {
        handleClientRootClaimVisitor(visitorLink, ctx, intent);
        return;
    }

    const bool legacyTopologyRequest = intent.intent == INTENT_SPECTATE
        && intent.spectatorCapabilities == kSpectateCapabilityRelayTopology;
    const bool releasedTopologyV2Request = intent.intent == INTENT_SPECTATE
        && intent.spectatorCapabilities
               == kSpectateCapabilityReleasedTopologyV2Mask;
    if (legacyTopologyRequest || releasedTopologyV2Request) {
        SpectateTopologyChallengeV2 linkChallenge{};
        if (releasedTopologyV2Request) {
            linkChallenge.schemaVersion = kSpectateTopologyControlSchemaVersion;
            linkChallenge.requiredFeatures =
                kSpectateTopologyControlRequiredFeatures;
            linkChallenge.sessionId = answer.sessionId;
            linkChallenge.viewerId = intent.viewerId;
            linkChallenge.viewerGeneration = intent.resumeRecord;
            bool degraded = false;
            const bool generated = qnet::secureRandomBytes(
                linkChallenge.peerChallenge,
                sizeof(linkChallenge.peerChallenge), degraded);
            uint8_t challengeOr = 0u;
            for (const uint8_t byte : linkChallenge.peerChallenge)
                challengeOr |= byte;
            if (!generated || degraded || challengeOr == 0u
                || !visitorLink.sendMsg(MSG_SPECTATE_TOPOLOGY_CHALLENGE_V2,
                                        &linkChallenge,
                                        sizeof(linkChallenge))) {
                std::memset(&linkChallenge, 0, sizeof(linkChallenge));
                return;
            }
        }
        if (pumpRecvEither(visitorLink, ctx,
                           MSG_SPECTATE_TOPOLOGY_PROOF,
                           MSG_SPECTATE_TOPOLOGY_PROOF_V2, 2000, rx,
                           &remoteErr) != Pump::Got) {
            std::memset(&linkChallenge, 0, sizeof(linkChallenge));
            return;
        }
        const bool exactLiveCapability =
            answer.state == SPECTATE_LIVE
            && answer.sessionId != 0
            && intent.sessionId == answer.sessionId
            && intent.keyLow == answer.keyLow
            && intent.keyHigh == answer.keyHigh
            && (answer.keyLow != 0 || answer.keyHigh != 0);
        if (rx.type == MSG_SPECTATE_TOPOLOGY_PROOF) {
            std::memset(&linkChallenge, 0, sizeof(linkChallenge));
            if (!legacyTopologyRequest) return;
            SpectateTopologyInfo topologyInfo{};
            SpectateTopologyProof proof{};
            const bool proofReceived = bodyAs(rx, proof) && proof.reserved == 0u
                && proof.viewerGeneration == intent.resumeRecord;
            qnetplay::spectator::SpectatorGrant viewerGrant{};
            const bool validViewerGrant = proofReceived
                && grantFromWire(proof.grant, viewerGrant)
                && viewerGrant.viewerId == intent.viewerId
                && qclient::spectator::sidecarVerifyTopologyViewerGrant(
                    viewerGrant, intent.resumeRecord, nowUs() / 1000ull);
            bool granted = false;
            if (exactLiveCapability && validViewerGrant) {
                qclient::spectator::topology::TopologyEnrollment enrollment{};
                std::string topologyError;
                if (qclient::spectator::sidecarEnrollTopologyViewer(
                        intent.viewerId, intent.resumeRecord,
                        true, nowUs() / 1000ull, enrollment,
                        topologyError)) {
                    granted = topologyInfoFromEnrollment(answer.sessionId,
                                                         enrollment, topologyInfo);
                }
            }
            visitorLink.sendMsg(MSG_SPECTATE_TOPOLOGY_INFO, &topologyInfo,
                                sizeof(topologyInfo));
            visitorLink.closeGraceful(200);
            std::memset(&proof, 0, sizeof(proof));
            std::memset(&viewerGrant, 0, sizeof(viewerGrant));
            QLOG("[spectator-topology] private enrollment %s viewer=%s",
                 granted ? "granted" : "refused",
                 qclient::spectator::redactedTag(intent.viewerId).c_str());
            return;
        }

        SpectateTopologyProofV2 proof{};
        if (!releasedTopologyV2Request || !bodyAs(rx, proof)) {
            std::memset(&linkChallenge, 0, sizeof(linkChallenge));
            return;
        }
        qnetplay::spectator::SpectatorGrant viewerGrant{};
        const bool proofValid = qclient::spectator::topology::controlv2::
            validateProof(proof, intent, answer, linkChallenge, viewerGrant);
        std::memset(&linkChallenge, 0, sizeof(linkChallenge));
        SpectateTopologyInfoV2 topologyInfo{};
        bool granted = false;
        if (proofValid && exactLiveCapability) {
            qclient::spectator::topology::TopologyViewerClaim claim{};
            claim.sessionId = proof.grant.sessionId;
            claim.sessionEpoch = proof.sessionEpoch;
            claim.viewerId = proof.viewerId;
            claim.viewerGeneration = proof.viewerGeneration;
            claim.qspEndpoint = { proof.qspIpv4BE, proof.qspPort };
            claim.qspProvenance = proof.qspRoute == SPECTATE_ROUTE_RELAY
                ? qclient::spectator::topology::EndpointProvenance::
                      ProvenRelayQsp
                : (proof.qspRoute == SPECTATE_ROUTE_DIRECT
                       ? qclient::spectator::topology::EndpointProvenance::
                             ProvenDirectQsp
                       : qclient::spectator::topology::EndpointProvenance::
                             ProvenPunchedQsp);
            claim.qspRouteGeneration = proof.qspRouteGeneration;
            claim.discoveryGeneration = proof.discoveryGeneration;
            claim.compatibility = proof.compatibilityIdentity;
            claim.streamRootIdentity = proof.streamRootIdentity;
            qclient::spectator::topology::TopologyEnrollment enrollment{};
            std::string topologyError;
            constexpr unsigned kTopologyQspAcceptanceAttempts = 20u;
            for (unsigned attempt = 0u;
                 attempt < kTopologyQspAcceptanceAttempts && !granted;
                 ++attempt) {
                if (qclient::spectator::sidecarEnrollTopologyViewerExact(
                        claim, viewerGrant, nowUs() / 1000ull, enrollment,
                        topologyError)) {
                    granted = qclient::spectator::topology::controlv2::
                        infoFromEnrollment(proof.requestNonce, enrollment,
                                           topologyInfo)
                        && enrollment.session.sessionId == answer.sessionId
                        && enrollment.session.epoch == proof.sessionEpoch
                        && enrollment.sourceGeneration
                               == proof.discoveryGeneration
                        && enrollment.viewerId == proof.viewerId
                        && enrollment.viewerGeneration == proof.viewerGeneration
                        && (claim.qspProvenance
                                    == qclient::spectator::topology::
                                        EndpointProvenance::ProvenRelayQsp
                                ? enrollment.qspEndpoint.ipv4BE
                                    == qnetplay::spectator::kRelayVirtualIpv4BE
                                : enrollment.qspEndpoint == claim.qspEndpoint)
                        && enrollment.qspProvenance == claim.qspProvenance
                        && enrollment.qspRouteGeneration
                               == claim.qspRouteGeneration
                        && enrollment.streamRootIdentity
                               == claim.streamRootIdentity
                        && qnetplay::compat::semanticallyEqual(
                               enrollment.compatibility, claim.compatibility)
                        && topologyInfo.qspRoute == proof.qspRoute;
                    if (!granted)
                        qclient::spectator::sidecarDetachTopologyViewer(
                            proof.viewerId);
                    break;
                }
                if (topologyError
                    != "topology enrollment requires a current proved QSP streaming generation") {
                    break;
                }
                Sleep(10u);
            }
        }
        if (!granted) {
            if (!proofValid) {
                std::memset(&proof, 0, sizeof(proof));
                std::memset(&viewerGrant, 0, sizeof(viewerGrant));
                return;
            }
            topologyInfo = {};
            topologyInfo.schemaVersion = kSpectateTopologyControlSchemaVersion;
            topologyInfo.requiredFeatures =
                kSpectateTopologyControlRequiredFeatures;
            topologyInfo.state = SPECTATE_TOPOLOGY_UNAVAILABLE;
            topologyInfo.requestNonce = proof.requestNonce;
            topologyInfo.sessionId = proof.grant.sessionId;
            topologyInfo.sessionEpoch = proof.sessionEpoch;
            topologyInfo.viewerId = proof.viewerId;
            topologyInfo.viewerGeneration = proof.viewerGeneration;
            topologyInfo.streamRootIdentity = proof.streamRootIdentity;
            topologyInfo.compatibilityIdentity = proof.compatibilityIdentity;
            topologyInfo.sourceGeneration = proof.discoveryGeneration;
        }
        const bool signedResponse = qclient::spectator::topology::controlv2::
            signInfo(topologyInfo, viewerGrant);
        const bool responseSent = signedResponse
            && visitorLink.sendMsg(MSG_SPECTATE_TOPOLOGY_INFO_V2, &topologyInfo,
                                   sizeof(topologyInfo));
        if (!responseSent && granted)
            qclient::spectator::sidecarDetachTopologyViewer(proof.viewerId);
        std::memset(&topologyInfo, 0, sizeof(topologyInfo));
        std::memset(&proof, 0, sizeof(proof));
        std::memset(&viewerGrant, 0, sizeof(viewerGrant));
        visitorLink.closeGraceful(200);
        QLOG("[spectator-topology] released enrollment %s viewer=%s",
             granted ? "granted" : "refused",
             qclient::spectator::redactedTag(intent.viewerId).c_str());
        return;
    }

    if (intent.intent == INTENT_SPECTATE
        && answer.state == SPECTATE_LIVE) {
        const auto policy =
            static_cast<qclient::spectator::AdmissionPolicy>(
                answer.admissionPolicy);
        const bool provesCode =
            intent.sessionId == answer.sessionId
            && intent.keyLow == answer.keyLow
            && intent.keyHigh == answer.keyHigh;
        if (policy == qclient::spectator::AdmissionPolicy::Disabled) {
            answer.admissionState = SPECTATE_ADMISSION_UNAVAILABLE;
            clearSpectateSecrets(answer);
        } else if (policy
                       == qclient::spectator::AdmissionPolicy::CodeOnly
                   && !provesCode) {
            answer.admissionState = SPECTATE_ADMISSION_REQUIRED;
            clearSpectateSecrets(answer);
        } else if ((policy
                        == qclient::spectator::AdmissionPolicy::Public
                    || policy
                        == qclient::spectator::AdmissionPolicy::AskHost)
                   && intent.sessionId == 0) {
            answer.admissionState = SPECTATE_ADMISSION_NONE;
        } else {
            qclient::spectator::GrantRequest request{};
            request.viewerId = intent.viewerId;
            request.resumeRecord = intent.resumeRecord;
            request.resumeDigest = intent.resumeDigest;
            request.route = intent.spectatorRoute;
            request.lifetimeSeconds = 300;
            request.peerIpv4BE =
                visitorLink.peer().sa.sin_addr.s_addr;
            qnetplay::spectator::SpectatorGrant grant{};
            qclient::spectator::AdmissionDenyReason deny =
                qclient::spectator::AdmissionDenyReason::None;
            if (!qclient::spectator::sidecarIssueGrant(
                    request, grant, deny)) {
                answer.admissionState =
                    deny == qclient::spectator::AdmissionDenyReason::CapacityFull
                    || deny == qclient::spectator::AdmissionDenyReason::PerIpQuota
                        ? SPECTATE_ADMISSION_CAPACITY
                        : SPECTATE_ADMISSION_UNAVAILABLE;
                answer.grant = {};
            } else {
                qclient::spectator::sidecarNoteAdmittedViewer(
                    visitorLink.peer().sa.sin_addr.s_addr,
                    ntohs(visitorLink.peer().sa.sin_port),
                    intent.spectatorDataPort);
                bool approved = true;
                if (policy
                    == qclient::spectator::AdmissionPolicy::AskHost) {
                    approved = false;
                    if (con != nullptr) {
                        const int choice = con->menu(
                            "Spectator request",
                            { "Allow this viewer once" },
                            "Decline");
                        approved = choice == 0;
                    }
                    if (approved)
                        approved =
                            qclient::spectator::sidecarApproveViewer(
                                grant.viewerId);
                    else
                        (void)qclient::spectator::sidecarDeclineViewer(
                            grant.viewerId);
                }
                if (approved) {
                    answer.admissionState = SPECTATE_ADMISSION_GRANTED;
                    answer.grant = grantToWire(grant);
                } else {
                    answer.admissionState = SPECTATE_ADMISSION_REFUSED;
                    answer.grant = {};
                }
            }
        }
    } else if (intent.intent == INTENT_JOIN
               && answer.admissionPolicy
                      != static_cast<uint8_t>(
                          qclient::spectator::AdmissionPolicy::Public)) {
        clearSpectateSecrets(answer);
    }
    const bool responseSent =
        visitorLink.sendMsg(MSG_SPECTATE_INFO, &answer, sizeof(answer));
    visitorLink.closeGraceful(kSpectateInfoCloseGraceMs);
    QLOG("[host-control-visitor] stage=response intent=%s result=%s peer=%s "
         "requestedSession=%u state=%u admission=%u socketError=%d",
         intent.intent == INTENT_SPECTATE ? "spectate" : "join",
         responseSent ? "sent" : "failed",
         visitorLink.peer().str().c_str(), intent.sessionId,
         static_cast<unsigned>(answer.state),
         static_cast<unsigned>(answer.admissionState),
         visitorLink.lastError());
}

class RetainedHostControlOwner {
public:
    ~RetainedHostControlOwner() { stop(); }

    bool start(std::shared_ptr<HostControlEndpoint> endpoint,
               const SpectateInfo& live) {
        stop();
        if (!endpoint || live.state != SPECTATE_LIVE
            || live.sessionId == 0
            || (live.keyLow == 0 && live.keyHigh == 0)) {
            return false;
        }
        _stop.store(false);
        try {
            _worker = std::thread(
                [this, endpoint = std::move(endpoint), live]() {
                    run(endpoint, live);
                });
        } catch (...) {
            _stop.store(true);
            return false;
        }
        SetThreadPriority(_worker.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
        return true;
    }

    void stop() {
        _stop.store(true);
        if (_worker.joinable()) _worker.join();
    }

private:
    void run(const std::shared_ptr<HostControlEndpoint>& endpoint,
             const SpectateInfo& live) {
        QLOG("[spectator-control] retained owner started session=%u",
             live.sessionId);
        bool expired = false;
        while (!_stop.load()) {
            const qclient::spectator::SidecarStatus status =
                qclient::spectator::sidecarStatus();
            if (!qclient::spectator::sidecarRunning()
                || status.phase
                       == qclient::spectator::SidecarPhase::Expired
                || status.phase
                       == qclient::spectator::SidecarPhase::Failed) {
                expired = true;
                break;
            }
            if (status.serving) {
                serveIntentDuringMatch(*endpoint, live, nullptr);
            } else {
                SpectateInfo unavailable = live;
                unavailable.state = SPECTATE_NO_MATCH;
                clearSpectateSecrets(unavailable);
                serveIntentDuringMatch(*endpoint, unavailable, nullptr);
            }
            Sleep(10);
        }
        if (expired) {
            (void)qclient::spectator::stopSidecar(false);
        }
        QLOG("[spectator-control] retained owner stopped session=%u "
             "reason=%s",
             live.sessionId, expired ? "sidecar-expired" : "replacement");
    }

    std::atomic<bool> _stop{true};
    std::thread _worker;
};

class ScopedHostControlPump {
public:
    explicit ScopedHostControlPump(
        std::shared_ptr<HostControlEndpoint> endpoint)
        : _endpoint(std::move(endpoint)) {
        if (!_endpoint) return;
        try {
            _worker = std::thread([this]() {
                while (!_stop.load()) _endpoint->service(10);
            });
            SetThreadPriority(
                _worker.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
        } catch (...) {
            _stop.store(true);
            _endpoint.reset();
        }
    }

    ~ScopedHostControlPump() { stop(); }

    void stop() {
        _stop.store(true);
        if (_worker.joinable()) _worker.join();
    }

private:
    std::shared_ptr<HostControlEndpoint> _endpoint;
    std::atomic<bool> _stop{false};
    std::thread _worker;
};

RetainedHostControlOwner& retainedHostControlOwner() {
    static RetainedHostControlOwner owner;
    return owner;
}

void retireRetainedSpectatorService() {
    retainedHostControlOwner().stop();
    if (qclient::spectator::sidecarRunning())
        (void)qclient::spectator::stopSidecar(false);
}

void logSpectatorE0Lifecycle(
    const qclient::spectator::SidecarStatus& status,
    std::map<uint64_t, qclient::spectator::ViewerStatus>& previous) {
    namespace qs = qclient::spectator;
    std::map<uint64_t, qs::ViewerStatus> current;
    for (const qs::ViewerStatus& viewer : status.viewers) {
        const auto known = previous.find(viewer.viewerId);
        if (known == previous.end()) {
            const std::string line =
                qoh99SpectatorE0ViewerAttachEvidenceLine(viewer);
            QLOG("%s", line.c_str());
        } else if (known->second.generation != viewer.generation) {
            std::string line = "[spectator-e0] qsp event=generation-updated "
                "qsp_proof=accepted qsp_capability=accepted "
                "compatibility=accepted admission=source-accepted "
                "previous_generation="
                + std::to_string(known->second.generation) + " ";
            line += spectatorE0ViewerFields(viewer);
            QLOG("%s", line.c_str());
        } else if (known->second.state != viewer.state) {
            std::string line;
            if (known->second.state == qs::ViewerServiceState::Handshaking
                && viewer.state == qs::ViewerServiceState::Streaming) {
                line = "[spectator-e0] qsp event=bootstrap-confirmed "
                    "qsp_init_ack=accepted ";
            } else {
                line = "[spectator-e0] qsp event=state-transition "
                    "previous_state=";
                line += qs::viewerServiceStateName(known->second.state);
                line += " ";
            }
            line += spectatorE0ViewerFields(viewer);
            QLOG("%s", line.c_str());
        }
        current[viewer.viewerId] = viewer;
    }

    for (const auto& entry : previous) {
        if (current.find(entry.first) != current.end()) continue;
        const qs::ViewerStatus& viewer = entry.second;
        const qs::ViewerTerminalOutcome* outcome = nullptr;
        for (auto found = status.terminalOutcomes.rbegin();
             found != status.terminalOutcomes.rend(); ++found) {
            if (found->viewerId == viewer.viewerId
                && found->generation == viewer.generation) {
                outcome = &*found;
                break;
            }
        }
        const std::string line =
            qoh99SpectatorE0ViewerDisconnectEvidenceLine(viewer, outcome);
        QLOG("%s", line.c_str());
    }
    previous.swap(current);
}

std::string sidecarStatusLine() {
    namespace qs = qclient::spectator;
    const qs::SidecarStatus status = qs::sidecarStatus();
    std::string line = "Spectators: ";
    line += qs::sourceStatusText(status.code);
    line += " (";
    line += qs::sidecarPhaseName(status.phase);
    line += ")";
    if (status.boundPort != 0)
        line += " | UDP " + std::to_string(status.boundPort);
    line += " | " + std::to_string(status.viewerCount) + "/"
          + std::to_string(status.viewerCapacity) + " watching";
    if (status.pendingApprovals != 0)
        line += " | " + std::to_string(status.pendingApprovals)
              + " waiting for approval";
    uint32_t backlog = 0;
    for (const qs::ViewerStatus& viewer : status.viewers)
        if (viewer.backlogRecords > backlog) backlog = viewer.backlogRecords;
    line += " | backlog " + std::to_string(backlog);
    if (!status.failureReason.empty())
        line += " | " + status.failureReason;
    return line;
}

std::string topologyViewerSelectionLabel(
    const qclient::spectator::ViewerStatus& viewer) {
    return "Viewer " + qclient::spectator::redactedTag(viewer.viewerId)
        + " | QSP generation " + std::to_string(viewer.generation);
}

void selectTopologyRelayChild(qui::Console& con) {
    namespace qs = qclient::spectator;
    const qs::SidecarStatus status = qs::sidecarStatus();
    if (!status.serving || !status.topologyAuthorityReady) {
        con.message("Topology activation unavailable",
                    "The optional spectator topology authority is not ready. "
                    "Ordinary spectators are unaffected.");
        return;
    }

    std::vector<qs::ViewerStatus> candidates;
    candidates.reserve(status.viewers.size());
    for (const qs::ViewerStatus& viewer : status.viewers) {
        if (viewer.viewerId != 0u && viewer.generation != 0u)
            candidates.push_back(viewer);
    }
    if (candidates.size() < 2u) {
        con.message("Topology activation unavailable",
                    "Two active viewers are required before the host can "
                    "select a relay and child. No viewer is promoted "
                    "automatically.");
        return;
    }

    std::vector<std::string> relayChoices;
    relayChoices.reserve(candidates.size());
    for (const qs::ViewerStatus& viewer : candidates)
        relayChoices.push_back(topologyViewerSelectionLabel(viewer));
    const int relayIndex = con.menu(
        "Select topology relay (S1)", relayChoices, "Cancel");
    if (relayIndex < 0
        || relayIndex >= static_cast<int>(candidates.size())) {
        return;
    }
    const qs::ViewerStatus relay = candidates[static_cast<size_t>(relayIndex)];

    std::vector<qs::ViewerStatus> childCandidates;
    std::vector<std::string> childChoices;
    childCandidates.reserve(candidates.size() - 1u);
    childChoices.reserve(candidates.size() - 1u);
    for (const qs::ViewerStatus& viewer : candidates) {
        if (viewer.viewerId == relay.viewerId) continue;
        childCandidates.push_back(viewer);
        childChoices.push_back(topologyViewerSelectionLabel(viewer));
    }
    const int childIndex = con.menu(
        "Select topology child (S2)", childChoices, "Cancel");
    if (childIndex < 0
        || childIndex >= static_cast<int>(childCandidates.size())) {
        return;
    }
    const qs::ViewerStatus child =
        childCandidates[static_cast<size_t>(childIndex)];

    const int confirmation = con.menu(
        "Start the selected topology activation?",
        { "Activate this exact relay-child pair" }, "Cancel");
    if (confirmation != 0) return;

    const qs::TopologyViewerSelection relaySelection{
        relay.viewerId, relay.generation };
    const qs::TopologyViewerSelection childSelection{
        child.viewerId, child.generation };
    std::string error;
    if (!qs::sidecarIssueTopologyRelayChildActivationForViewers(
            relaySelection, childSelection, error)) {
        QLOG("[spectator-topology] host activation selection refused "
             "relay=%s generation=%u child=%s generation=%u",
             qs::redactedTag(relay.viewerId).c_str(), relay.generation,
             qs::redactedTag(child.viewerId).c_str(), child.generation);
        con.message("Topology activation not started",
                    "The selected viewers are no longer eligible. Ordinary "
                    "spectator service is unchanged.",
                    error.empty() ? "The topology authority rejected the "
                                    "selected pair."
                                  : error);
        return;
    }

    QLOG("[spectator-topology] host activation selection accepted "
         "relay=%s generation=%u child=%s generation=%u",
         qs::redactedTag(relay.viewerId).c_str(), relay.generation,
         qs::redactedTag(child.viewerId).c_str(), child.generation);
    con.message("Topology selection accepted",
                "The topology authority accepted this exact pair. It may "
                "wait for S2's authenticated control registration and both "
                "activation receipts; this does not yet prove a live child "
                "route.");
}

std::string recorderHostStateText(
    qreplay::ipc::v3::RecorderHostState state) {
    using State = qreplay::ipc::v3::RecorderHostState;
    switch (state) {
        case State::Listening: return "waiting for game";
        case State::Connected: return "connected";
        case State::WaitingForGame: return "waiting for match";
        case State::WritingGame: return "recording";
        case State::FinalizingGame: return "saving";
        case State::Draining: return "finishing";
        case State::Finished: return "finished";
        case State::FailedTransport: return "transport failed";
        case State::FailedFile: return "storage failed";
    }
    return "unknown";
}

std::string recorderStatusLine(
    const qclient::replay::ReplayRecorderHostV3& host) {
    namespace qr = qclient::replay;
    const qr::RecorderStatusV3 status = host.status();
    std::string line = "Replay: " + recorderHostStateText(status.state);
    line += " | " + std::to_string(status.gamesCommitted)
          + " game(s) saved";
    if (status.state == qreplay::ipc::v3::RecorderHostState::WritingGame)
        line += " | frame " + std::to_string(status.nextRecord);
    if (status.failed && !status.detail.empty())
        line += " | " + status.detail;
    return line;
}

int waitForLaunchedGame(qclient::LaunchResult& result, qui::Console* con,
                        DWORD timeoutMs, bool hiddenDesktop,
                        const std::string& spectateAddress = "",
                        HostControlEndpoint* liveListener = nullptr,
                        const SpectateInfo* liveInfo = nullptr,
                        bool showSidecarStatus = false,
                        const qclient::replay::ReplayRecorderHostV3* recorder =
                            nullptr,
                        bool* cleanNativeExitOut = nullptr) {
    if (cleanNativeExitOut) *cleanNativeExitOut = false;
    if (con) {
        con->status("Game started (pid " + std::to_string(result.pid) + ").");
        if (!result.error.empty())
            con->status("note: " + result.error);
        con->status(hiddenDesktop
            ? "Game running on a HIDDEN desktop (not on your screen) - this window waits until it exits."
            : "Game running - this window waits here until the game exits.");
        if (!spectateAddress.empty())
            con->status("Spectate address: " + spectateAddress);
    } else {
        std::printf("Launched GAME.EXE pid=%lu%s\n", result.pid,
                    hiddenDesktop ? " (hidden desktop)" : "");
    }
    const uint64_t deadlineUs = timeoutMs == INFINITE
        ? UINT64_MAX
        : nowUs() + static_cast<uint64_t>(timeoutMs) * 1000ull;
    uint64_t copyNoticeUntilUs = 0;
    uint64_t nextServiceLineUs = 0;
    uint64_t nextSpectatorEvidenceLogUs = 0;
    size_t lastLoggedViewerCount = static_cast<size_t>(-1);
    bool topologyStatusLogged = false;
    bool lastTopologyAuthorityReady = false;
    size_t lastTopologyEnrolled = 0u;
    size_t lastTopologyAttached = 0u;
    std::map<uint64_t, qclient::spectator::ViewerStatus>
        previousSpectatorEvidenceViewers;
    DWORD lifecycleFailure = ERROR_SUCCESS;
    const char* completion = "process-signaled";
    for (;;) {
        const DWORD wait = WaitForSingleObject(result.process, 250);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            lifecycleFailure = GetLastError();
            if (lifecycleFailure == ERROR_SUCCESS)
                lifecycleFailure = ERROR_GEN_FAILURE;
            completion = "wait-failed";
            QLOG("[launch] process wait FAILED pid=%lu error=%lu",
                 result.pid, lifecycleFailure);

            DWORD currentExit = STILL_ACTIVE;
            if (GetExitCodeProcess(result.process, &currentExit)
                && currentExit == STILL_ACTIVE) {
                if (!TerminateProcess(result.process, lifecycleFailure)) {
                    QLOG("[launch] could not terminate child after wait "
                         "failure pid=%lu error=%lu",
                         result.pid, GetLastError());
                } else {
                    (void)WaitForSingleObject(result.process, 3000);
                }
            }
            break;
        }
        bool spectatorServing = !showSidecarStatus;
        qclient::spectator::SidecarStatus sidecar{};
        if (showSidecarStatus) {
            sidecar = qclient::spectator::sidecarStatus();
            spectatorServing = sidecar.serving;
            if (sidecar.viewerCount != lastLoggedViewerCount) {
                lastLoggedViewerCount = sidecar.viewerCount;
                QLOG("[spectator-contract] v=2 state=viewer-count "
                     "count=%zu capacity=%zu",
                     sidecar.viewerCount, sidecar.viewerCapacity);
            }
            if (!topologyStatusLogged
                || sidecar.topologyAuthorityReady != lastTopologyAuthorityReady
                || sidecar.topologyEnrolled != lastTopologyEnrolled
                || sidecar.topologyAttached != lastTopologyAttached) {
                topologyStatusLogged = true;
                lastTopologyAuthorityReady = sidecar.topologyAuthorityReady;
                lastTopologyEnrolled = sidecar.topologyEnrolled;
                lastTopologyAttached = sidecar.topologyAttached;
                QLOG("[spectator-topology] source authorityReady=%u "
                     "enrolled=%zu attached=%zu",
                     sidecar.topologyAuthorityReady ? 1u : 0u,
                     sidecar.topologyEnrolled, sidecar.topologyAttached);
            }
            logSpectatorE0Lifecycle(sidecar, previousSpectatorEvidenceViewers);
            const uint64_t evidenceNowUs = nowUs();
            if (sidecar.viewerCount != 0
                && evidenceNowUs >= nextSpectatorEvidenceLogUs) {
                const std::string sourceLine =
                    qoh99SpectatorE0SourceEvidenceLine(sidecar);
                QLOG("%s", sourceLine.c_str());
                for (const qclient::spectator::ViewerStatus& viewer :
                     sidecar.viewers) {
                    const std::string viewerLine =
                        qoh99SpectatorE0ViewerEvidenceLine(viewer);
                    QLOG("%s", viewerLine.c_str());
                }
                nextSpectatorEvidenceLogUs = evidenceNowUs + 1000000ull;
            }
        }
        if (con && (showSidecarStatus || recorder != nullptr)
            && nowUs() >= copyNoticeUntilUs && nowUs() >= nextServiceLineUs) {
            std::string line;
            if (showSidecarStatus) line = sidecarStatusLine();
            if (recorder != nullptr) {
                if (!line.empty()) line += "   ";
                line += recorderStatusLine(*recorder);
            }
            if (!spectateAddress.empty() && spectatorServing)
                line += "   [S] copies the address";
            if (showSidecarStatus && spectatorServing
                && sidecar.topologyAuthorityReady) {
                line += "   [T] selects topology pair";
            }
            con->statusLine(line);
            nextServiceLineUs = nowUs() + 1000000ull;
        }
        const bool topologySelectionAvailable = showSidecarStatus
            && sidecar.topologyAuthorityReady;
        if (con && spectatorServing
            && (!spectateAddress.empty() || topologySelectionAvailable)) {
            if (!showSidecarStatus && recorder == nullptr
                && nowUs() >= copyNoticeUntilUs)
                con->statusLine("Game running - [S] copies the spectate address");
            const int key = con->pollKey(0);
            if (key == 'S' && !spectateAddress.empty()) {
                const bool copied = con->copyText(spectateAddress);
                QLOG("[launch] copy spectate address ok=%d", copied ? 1 : 0);
                con->statusLine(copied
                    ? "Spectate address copied to the clipboard."
                    : "Could not open the clipboard.");
                copyNoticeUntilUs = nowUs() + 2000000ull;
            } else if (key == 'T' && topologySelectionAvailable) {
                selectTopologyRelayChild(*con);
            }
        }
        if (liveListener != nullptr && liveInfo != nullptr) {
            if (spectatorServing) {
                serveIntentDuringMatch(*liveListener, *liveInfo, con);
            } else {
                SpectateInfo unavailable = *liveInfo;
                unavailable.state = SPECTATE_NO_MATCH;
                clearSpectateSecrets(unavailable);
                serveIntentDuringMatch(*liveListener, unavailable, con);
            }
        }
        if (nowUs() >= deadlineUs) {
            QLOG("[launch] timeout %lums reached -> terminating game pid=%lu",
                 timeoutMs, result.pid);
            std::printf("Probe timeout reached; terminating game pid=%lu\n",
                        result.pid);
            lifecycleFailure = ERROR_TIMEOUT;
            completion = "timeout";
            if (!TerminateProcess(result.process, ERROR_TIMEOUT)) {
                QLOG("[launch] timeout termination FAILED pid=%lu error=%lu",
                     result.pid, GetLastError());
            } else {
                (void)WaitForSingleObject(result.process, 3000);
            }
            break;
        }
    }

    DWORD childExit = STILL_ACTIVE;
    const bool haveChildExit =
        GetExitCodeProcess(result.process, &childExit) != FALSE;
    if (!haveChildExit && lifecycleFailure == ERROR_SUCCESS) {
        lifecycleFailure = GetLastError();
        if (lifecycleFailure == ERROR_SUCCESS)
            lifecycleFailure = ERROR_GEN_FAILURE;
        completion = "exit-query-failed";
    }
    const int exitStatus = qoh99ClassifyChildExit(
        haveChildExit, childExit, lifecycleFailure);
    bool cleanNativeExit = exitStatus == 0;
    QLOG("[launch] child-exit pid=%lu completion=%s queried=%d "
         "native=0x%08lX lifecycle=%lu caster=0x%08X",
         result.pid, completion, haveChildExit ? 1 : 0,
         static_cast<unsigned long>(childExit),
         static_cast<unsigned long>(lifecycleFailure),
         static_cast<unsigned>(exitStatus));
    if (!result.gameDirectory.empty()) {
        std::vector<std::string> reasons;
        std::string journalPath;
        for (const std::string& path :
             qgame::gameErrorJournalCandidates(result.gameDirectory)) {
            const qgame::GameErrorJournalFile journal =
                qgame::readGameErrorJournalFile(path, result.launchedAtFileTime);
            if (!journal.present) continue;
            if (!journal.fromThisRun) {
                QLOG("[game-error] %s exists but predates this launch; ignored",
                     path.c_str());
                continue;
            }
            cleanNativeExit = false;
            QLOG_ISSUE("[game-error] the game wrote %zu line(s) to %s this run:",
                 journal.lines.size(), path.c_str());
            for (const std::string& line : journal.lines) {
                QLOG("[game-error]   %s", line.c_str());
                reasons.push_back(line);
            }
            if (journalPath.empty()) journalPath = path;
        }
        if (reasons.empty()) {
            QLOG("[game-error] the game wrote no QOHerror.txt this run");
        } else if (con) {
            std::string details = "From " + journalPath + ":";
            for (const std::string& line : reasons) details += "\n  " + line;
            con->message(
                "The game reported an error and closed",
                reasons.front()
                    + "\n\nA file the game needs is missing or damaged. Copy the "
                      "complete game folder again from a source that is known "
                      "to be good (see README, Troubleshooting).",
                details);
        }
    }
    if (!result.sessionDirectory.empty()) {
        static const char* const kCrash[] = { "[CRASH]" };
        const std::vector<std::string> crashLines = qgame::readLogHighlights(
            result.sessionDirectory + "\\dll.log", qgame::kDllLogLaunchBanner,
            kCrash, 1u, 12u);
        const bool exitLooksLikeCrash = haveChildExit
            && childExit != STILL_ACTIVE && childExit >= 0xC0000000ul;
        if (!crashLines.empty() || exitLooksLikeCrash) {
            cleanNativeExit = false;
            char code[32];
            std::snprintf(code, sizeof(code), "0x%08lX",
                          static_cast<unsigned long>(childExit));
            std::string body = crashLines.empty()
                ? std::string("The game process ended with code ") + code + "."
                : crashLines.front();
            body += "\n\nKeep this run's logs\\session_* folder (dll.log and "
                    "the crash dump are in it) and send it along with what you "
                    "were doing when it happened.";
            std::string details = "From " + result.sessionDirectory + "\\dll.log:";
            for (const std::string& line : crashLines) details += "\n  " + line;
            QLOG_ISSUE("[fail] The game crashed: %s", crashLines.empty() ? code : crashLines.front().c_str());
            if (con) con->message("The game crashed", body, details);
        }
    }
    if (haveChildExit && childExit != STILL_ACTIVE)
        qclient::releaseSessionLogWriterLease(result.sessionLogWriterLease);
    CloseHandle(result.process);
    result.process = nullptr;
    if (result.desktop) {
        CloseDesktop(result.desktop);
        result.desktop = nullptr;
    }
    if (cleanNativeExitOut) *cleanNativeExitOut = cleanNativeExit;
    return exitStatus;
}

void ensureWrapperLanguage(const std::string& gameDir, int language) {
    if (gameDir.empty()) return;
    const std::string path = gameDir + "\\ddraw_wrapper.ini";
    char value[8];
    std::snprintf(value, sizeof(value), "%d", language == 1 ? 1 : 0);
    WritePrivateProfileStringA("Overlay", "Language", value, path.c_str());
}

void ensureWrapperDisplayConfig(const std::string& gameDir) {
    const std::string path = gameDir + "\\System\\QOHcnf.key";
    std::vector<uint8_t> data;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        data.assign(std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>());
    }
    if (data.size() != 8044) return;
    uint32_t active = static_cast<uint32_t>(data[0])
        | (static_cast<uint32_t>(data[1]) << 8)
        | (static_cast<uint32_t>(data[2]) << 16)
        | (static_cast<uint32_t>(data[3]) << 24);
    if (active >= 10) active = 0;
    const auto readDword = [&data](const size_t at) {
        return static_cast<uint32_t>(data[at])
            | (static_cast<uint32_t>(data[at + 1]) << 8)
            | (static_cast<uint32_t>(data[at + 2]) << 16)
            | (static_cast<uint32_t>(data[at + 3]) << 24);
    };
    const auto writeDword = [&data](const size_t at, const uint32_t value) {
        data[at] = static_cast<uint8_t>(value & 0xFFu);
        data[at + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
        data[at + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
        data[at + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    };

    const size_t screenModeAt = 4 + active * 764 + 108;
    const size_t flagsAt = 4 + active * 764 + 376;
    const uint32_t fullscreen = readDword(screenModeAt);
    const uint32_t flags = readDword(flagsAt);
    const bool fixScreenMode = fullscreen != 1;
    const bool fixFullColor = (flags & 1u) == 0u;
    if (!fixScreenMode && !fixFullColor) return;

    if (fixScreenMode) writeDword(screenModeAt, 1u);
    if (fixFullColor) writeDword(flagsAt, flags | 1u);

    CopyFileA(path.c_str(), (path + ".prelaunch.bak").c_str(), FALSE);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
        if (!out.flush()) return;
    }
    if (MoveFileExA(tmp.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        QLOG("[launch] QOHcnf.key profile %u normalised before launch: "
             "screenMode %u->%u fullColour %u->%u (flags 0x%08X->0x%08X); "
             "previous file kept as .prelaunch.bak",
             active, fullscreen, fixScreenMode ? 1u : fullscreen,
             flags & 1u, 1u, flags,
             fixFullColor ? (flags | 1u) : flags);
    }
}

void startRecorderForLaunch(const Settings& s, const RecordingPlan& plan,
                            uint32_t gamePid,
                            qclient::replay::ReplayRecorderHostV3& recorder,
                            bool& started, qui::Console* con) {
    started = false;
    if (!plan.enabled) {
        if (!plan.offReason.empty())
            QLOG("[replay.record] not recording: %s", plan.offReason.c_str());
        return;
    }
    qnetplay::compat::CompatibilityIdentity compatibilityIdentity{};
    std::string compatibilityError;
    if (!localCompatibilityIdentityFor(
            s, compatibilityIdentity, compatibilityError)) {
        info(con, "Not recording this match: the simulation-content "
                  "identity could not be qualified"
                  + (compatibilityError.empty()
                         ? std::string()
                         : std::string(" (") + compatibilityError + ")."));
        return;
    }
    const std::wstring root = replayRootOf(s);
    if (!qplat::ensureDirW(root)) {
        info(con, "Not recording this match: the replay folder "
                  + qplat::narrow(root) + " could not be created.");
        return;
    }
    qclient::replay::RecorderHostConfigV3 config;
    config.replayRoot = root;
    config.gamePid = gamePid;
    config.launchGeneration = plan.generation;
    config.launchNonce = plan.nonce;
    config.runtimeCompatibilityId = qipc::kRuntimeCompatibilityId;
    config.compatibilityIdentity = compatibilityIdentity;
    config.nameHint = plan.nameHint;
    config.battleLogLocalSide = plan.battleSide;
    config.battleLogLocalName = canonicalUtf8Nickname(s.nickname);
    config.battleLogEnabled = s.battleLog;
    config.flushEveryMs =
        static_cast<uint32_t>(s.replayFlushSeconds) * 1000u;
    if (!recorder.start(config)) {
        const qclient::replay::RecorderStatusV3 status = recorder.status();
        info(con, std::string("Not recording this match: ")
                  + (status.detail.empty()
                         ? "the recorder could not be armed"
                         : status.detail)
                  + ". The match itself is unaffected.");
        return;
    }
    started = true;
    QLOG("[replay.record] caster recorder armed generation=%u root='%s' "
         "hint='%s'", plan.generation, qplat::narrow(root).c_str(),
         plan.nameHint.c_str());
}

void reportRecorderOutcome(qclient::replay::ReplayRecorderHostV3& recorder,
                           qui::Console* con) {
    namespace qr = qclient::replay;
    recorder.stop();
    const qr::RecorderStatusV3 status = recorder.status();
    if (status.gamesCommitted != 0u && !status.lastFinalPath.empty()) {
        info(con, std::to_string(status.gamesCommitted)
                  + " replay game(s) saved. Latest: "
                  + qplat::narrow(status.lastFinalPath) + ".");
        if (status.failed && !status.detail.empty())
            info(con, "The final open game was not completed: "
                      + status.detail + ".");
        return;
    }
    std::string why = status.detail;
    info(con, std::string("Replay not completed: ")
              + (why.empty() ? std::string("the recorder never started")
                             : why)
              + ". Nothing was written; incomplete recordings are discarded.");
}

int reportSpectatorRuntimeExit(qui::Console* con, int childExit);

int launch(const Settings& s, qipc::IpcConfig cfg, SOCKET udp, qui::Console* con,
           DWORD timeoutMs = INFINITE, bool hiddenDesktop = false,
           SOCKET spectatorUdp = INVALID_SOCKET) {
    std::string game = resolveGamePath(s);
    std::string dll  = resolveDllPath(s);
    std::string wd   = dirOf(game);
    ensureWrapperDisplayConfig(wd);
    ensureWrapperLanguage(wd, s.language);
    if (fileExists(game).empty()) return fail(con, "The game could not be started", gameExeMissingMessage(game));
    if (fileExists(dll).empty())  return fail(con, "The game could not be started", netplayDllMissingMessage(dll));
    if (cfg.mode == qipc::MODE_SPECTATOR && !g_sessionDir.empty()) {
        std::remove((g_sessionDir + "\\netplay_failure.txt").c_str());
    }
    RecordingPlan recording = planRecording(s, cfg);
    if (recording.enabled) {
        std::array<uint8_t, 32> contentRoot{};
        if (!replayContentRootFor(s, contentRoot)) {
            recording.enabled = false;
            recording.generation = 0u;
            recording.nonce = 0u;
            recording.offReason =
                "the simulation-content manifest was incomplete";
            info(con, "Not recording this match: "
                      + recording.offReason
                      + ". The match itself is unaffected.");
        } else {
            std::memcpy(
                cfg.replayContentManifestRoot, contentRoot.data(),
                contentRoot.size());
            QLOG("[replay.record] content root = the curated simulation "
                 "catalogue (%u entries), not a directory scan - files with "
                 "no simulation role cannot invalidate this recording",
                 g_preflightInstallation.content.identity.entryCount);
        }
    }
    cfg.recordEnabled = recording.enabled ? 1 : 0;
    cfg.recordChannelGeneration = recording.generation;
    cfg.recordChannelNonce = recording.nonce;
    QLOG("[launch] game='%s' dll='%s' mode=%u host=%u training=%u "
         "playerUdp=%s spectatorUdp=%s hiddenDesktop=%d record=%u",
         game.c_str(), dll.c_str(), cfg.mode, cfg.isHost,
         cfg.trainingEnabled,
         udp == INVALID_SOCKET ? "none" : "bound",
         spectatorUdp == INVALID_SOCKET ? "none" : "dedicated",
         (int)hiddenDesktop, cfg.recordEnabled);

    qclient::PreparedLaunch prepared = qclient::prepareInjected(
        game, dll, wd, "", cfg, udp, hiddenDesktop, spectatorUdp);
    if (!prepared.ok) {
        QLOG("[launch] prepare failed: %s", prepared.error.c_str());
        return fail(con, "The game could not be started",prepared.error);
    }
    qclient::replay::ReplayRecorderHostV3 recorder;
    bool recorderStarted = false;
    startRecorderForLaunch(s, recording, prepared.pid, recorder,
                           recorderStarted, con);

    qclient::LaunchResult r = qclient::resumePrepared(std::move(prepared));
    QLOG("[launch] result ok=%d pid=%lu err='%s'", r.ok, r.pid, r.error.c_str());
    if (!r.ok) {
        if (recorderStarted) recorder.stop();
        if (r.desktop) CloseDesktop(r.desktop);
        return fail(con, "The game could not be started",r.error);
    }
    if (cfg.mode == qipc::MODE_SPECTATOR) {
        QLOG("[spectator-contract] v=1 state=launch-ready "
             "receiveOnly=1 session=%u pid=%lu hiddenDesktop=%d",
             cfg.sessionId, r.pid, hiddenDesktop ? 1 : 0);
    }
    if (!r.error.empty()) info(con, "launch note: " + r.error);
    const int rc = waitForLaunchedGame(
        r, con, timeoutMs, hiddenDesktop, "", nullptr, nullptr,
        false,
        recorderStarted ? &recorder : nullptr);
    if (recorderStarted) reportRecorderOutcome(recorder, con);
    if (cfg.mode == qipc::MODE_SPECTATOR) {
        return reportSpectatorRuntimeExit(con, rc);
    }
    return rc;
}

int launchWithReplayBroker(const Settings& s, qipc::IpcConfig cfg,
                           qui::Console* con, bool hiddenDesktop,
                           qclient::replay::ReplayBroker& broker,
                           qclient::replay::BrokerConfig brokerConfig) {
    const std::string game = resolveGamePath(s);
    const std::string dll = resolveDllPath(s);
    const std::string wd = dirOf(game);
    ensureWrapperDisplayConfig(wd);
    ensureWrapperLanguage(wd, s.language);
    if (fileExists(game).empty())
        return fail(con, "The game could not be started", gameExeMissingMessage(game));
    if (fileExists(dll).empty())
        return fail(con, "The game could not be started", netplayDllMissingMessage(dll));

    bool brokerStarted = false;
    const qclient::BeforeInjectionCallback startBroker =
        [&](DWORD pid, std::string& error) {
            brokerConfig.gamePid = pid;
            if (!broker.start(brokerConfig)) {
                error = "The private replay channel could not be created.";
                return false;
            }
            brokerStarted = true;
            QLOG("[replay.play] broker listening before injection pid=%lu "
                 "generation=%u",
                 pid, brokerConfig.generation);
            return true;
        };
    qclient::PreparedLaunch prepared = qclient::prepareInjected(
        game, dll, wd, "", cfg, INVALID_SOCKET, hiddenDesktop,
        INVALID_SOCKET, startBroker);
    if (!prepared.ok) {
        if (brokerStarted) broker.stop();
        QLOG("[replay.play] prepare failed: %s", prepared.error.c_str());
        return fail(con, "The game could not be started",prepared.error);
    }

    qclient::LaunchResult result = qclient::resumePrepared(std::move(prepared));
    QLOG("[replay.play] launch ok=%d pid=%lu err='%s'",
         result.ok, result.pid, result.error.c_str());
    if (!result.ok) {
        if (result.desktop) CloseDesktop(result.desktop);
        return fail(con, "The game could not be started",result.error);
    }
    if (!result.error.empty()) info(con, "launch note: " + result.error);
    return waitForLaunchedGame(result, con, INFINITE, hiddenDesktop);
}

int launchWithReplayBrokerV2(
    const Settings& s, qipc::IpcConfig cfg,
    qui::Console* con, bool hiddenDesktop,
    qclient::replay::ReplayBrokerV2& broker,
    qclient::replay::ReplayBrokerLaunchV2 brokerLaunch,
    qreplay::ipc::v3::PlaybackResultPayload* playbackResultOut = nullptr,
    bool* playbackResultReceivedOut = nullptr) {
    namespace qr = qclient::replay;

    const std::string game = resolveGamePath(s);
    const std::string dll = resolveDllPath(s);
    const std::string wd = dirOf(game);
    ensureWrapperDisplayConfig(wd);
    ensureWrapperLanguage(wd, s.language);
    if (fileExists(game).empty())
        return fail(con, "The game could not be started", gameExeMissingMessage(game));
    if (fileExists(dll).empty())
        return fail(con, "The game could not be started", netplayDllMissingMessage(dll));

    bool brokerListening = false;
    bool brokerServeStarted = false;
    qr::ReplayBrokerRunResultV2 brokerResult{};
    std::thread brokerThread;
    const qclient::BeforeInjectionCallback startBroker =
        [&](DWORD pid, std::string& error) {
            brokerLaunch.gamePid = pid;
            std::unique_ptr<qr::IReplayBrokerListenerV2> listener =
                qr::createWin32ReplayBrokerListenerV2();
            if (!listener
                || !broker.listenBeforeInjection(
                    brokerLaunch, std::move(listener))
                || !broker.readyForInjection()) {
                const qr::ReplayBrokerStatusV2 status = broker.status();
                error = status.detail.empty()
                    ? "The private Q99R-v2 replay channel could not be "
                      "created before injection."
                    : status.detail;
                return false;
            }
            brokerListening = true;
            // Arm the broker now: prepareInjected() waits for the validated prebuffer while QOH is suspended, so serving after it returns would deadlock.
            if (!broker.markInjectionStarted()) {
                error = "The replay broker could not arm its pre-injection "
                        "listener.";
                return false;
            }
            std::unique_ptr<qr::IReplayProcessMonitorV2> process =
                qr::createWin32ReplayProcessMonitorV2(pid);
            if (!process) {
                error = "The caster could not monitor the suspended replay "
                        "process.";
                return false;
            }
            HANDLE failClosedProcess = OpenProcess(
                PROCESS_TERMINATE | SYNCHRONIZE
                    | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE, pid);
            if (failClosedProcess == nullptr) {
                error = "The caster could not create the replay lifecycle "
                        "guard.";
                return false;
            }
            try {
                brokerThread = std::thread(
                    [&broker, &brokerResult,
                     process = std::move(process),
                     failClosedProcess]() mutable {
                        brokerResult = broker.serve(*process);
                        if (!brokerResult.success
                            && !brokerResult.processExited) {
                            DWORD exitCode = STILL_ACTIVE;
                            if (GetExitCodeProcess(
                                    failClosedProcess, &exitCode)
                                && exitCode == STILL_ACTIVE) {
                                QLOG("[replay.play.v2] broker failed closed "
                                     "state=%s detail='%s'",
                                     qr::replayBrokerStateTextV2(
                                         brokerResult.state),
                                     brokerResult.detail.c_str());
                                (void)TerminateProcess(
                                    failClosedProcess,
                                    ERROR_INVALID_DATA);
                                (void)WaitForSingleObject(
                                    failClosedProcess, 3000);
                            }
                        }
                        CloseHandle(failClosedProcess);
                    });
            } catch (...) {
                CloseHandle(failClosedProcess);
                error = "The caster could not start the replay broker "
                        "worker.";
                return false;
            }
            brokerServeStarted = true;
            QLOG("[replay.play.v2] broker listening before injection "
                 "pid=%lu generation=%u game=%016llX%016llX",
                 pid, brokerLaunch.launchGeneration,
                 static_cast<unsigned long long>(
                     cfg.replayGameUuidHigh),
                 static_cast<unsigned long long>(
                     cfg.replayGameUuidLow));
            return true;
        };

    qclient::PreparedLaunch prepared = qclient::prepareInjected(
        game, dll, wd, "", cfg, INVALID_SOCKET, hiddenDesktop,
        INVALID_SOCKET, startBroker);
    if (!prepared.ok) {
        if (brokerListening) broker.cancel();
        if (brokerThread.joinable()) brokerThread.join();
        QLOG("[replay.play.v2] prepare failed: %s",
             prepared.error.c_str());
        return fail(con, "The game could not be started",prepared.error);
    }
    if (!brokerServeStarted) {
        prepared.abort();
        broker.cancel();
        if (brokerThread.joinable()) brokerThread.join();
        return fail(con, "Replay launch failed",
                    "The replay broker worker was not started.");
    }

    qclient::LaunchResult result =
        qclient::resumePrepared(std::move(prepared));
    QLOG("[replay.play.v2] launch ok=%d pid=%lu err='%s'",
         result.ok, result.pid, result.error.c_str());
    if (!result.ok) {
        broker.cancel();
        if (brokerThread.joinable()) brokerThread.join();
        if (result.desktop) CloseDesktop(result.desktop);
        return fail(con, "The game could not be started",result.error);
    }
    if (!result.error.empty()) info(con, "launch note: " + result.error);

    bool cleanNativeExit = false;
    const int childRc = waitForLaunchedGame(
        result, con, INFINITE, hiddenDesktop,
        "", nullptr, nullptr, false, nullptr, &cleanNativeExit);
    brokerThread.join();
    broker.cancel();

    if (playbackResultReceivedOut != nullptr)
        *playbackResultReceivedOut = brokerResult.hasPlaybackResult;
    if (playbackResultOut != nullptr && brokerResult.hasPlaybackResult)
        *playbackResultOut = brokerResult.playbackResult;

    const bool userExited = brokerResult.state
            == qr::ReplayBrokerStateV2::Cancelled
        || (brokerResult.hasPlaybackResult
            && brokerResult.playbackResult.reason
                == qreplay::ipc::v3::PlaybackResultReason::UserExit);
    if (!brokerResult.success && userExited) {
        QLOG("[replay.play.v2] closed by the user frames=%u events=%u "
             "nativeExit=0x%08X",
             broker.status().sentFrames, broker.status().sentEvents,
             static_cast<unsigned>(childRc));
        info(con, "playback closed - you exited the replay");
        return childRc;
    }
    if (!brokerResult.success && qr::replayClosedBeforeResultV2(
            !hiddenDesktop, cleanNativeExit, brokerResult)) {
        QLOG("[replay.play.v2] native viewer closed before result state=%s",
             qr::replayBrokerStateTextV2(brokerResult.state));
        info(con, "Replay closed.");
        return childRc;
    }
    if (!brokerResult.success) {
        if (brokerResult.nextGameRefused)
            return fail(con, "The next replay could not be played", brokerResult.detail);
        QLOG("[replay.play.v2] terminal failure state=%s processExited=%d "
             "nativeExit=0x%08X detail='%s'",
             qr::replayBrokerStateTextV2(brokerResult.state),
             brokerResult.processExited ? 1 : 0,
             brokerResult.processExitCode,
             brokerResult.detail.c_str());
        std::string technical = std::string("Broker state: ")
            + qr::replayBrokerStateTextV2(brokerResult.state)
            + "\nGame process exited: "
            + (brokerResult.processExited ? "yes" : "no");
        if (brokerResult.processExited) {
            char code[48];
            std::snprintf(code, sizeof(code), "\nExit code: 0x%08X",
                          brokerResult.processExitCode);
            technical += code;
        }
        if (!brokerResult.detail.empty())
            technical += "\nRuntime detail: " + brokerResult.detail;
        return fail(
            con, "The replay stopped early",
            "This recording could not be played all the way through.\n\n"
            "If it is an old recording, it may have been made by a version of "
            "netplay that is too different from this one. Newer recordings "
            "should still play normally.\n\n"
            "Press [D] for the details to include in a bug report.",
            0, technical);
    }
    const bool behaviourOnly = brokerResult.hasPlaybackResult
        && brokerResult.playbackResult.reason
            == qreplay::ipc::v3::PlaybackResultReason::
                VerifiedBehaviourOnly;
    const bool diverged = brokerResult.hasPlaybackResult
        && brokerResult.playbackResult.reason
            == qreplay::ipc::v3::PlaybackResultReason::CompletedWithDivergence;
    QLOG("[replay.play.v2] playback complete mode=%s reason=%u frames=%u "
         "events=%u nativeExit=0x%08X",
         diverged ? "diverged" : behaviourOnly ? "behaviour-verified" : "byte-exact",
         brokerResult.hasPlaybackResult
             ? static_cast<unsigned>(
                 brokerResult.playbackResult.reason)
             : 0u,
         broker.status().sentFrames, broker.status().sentEvents,
         static_cast<unsigned>(childRc));
    info(con, qr::replayPlaybackCompletionTextV2(brokerResult.playbackResult.reason));
    return childRc;
}

RuntimeSync runtimeSyncFor(const NetplayConfig& negotiated,
                           uint8_t routeKind, bool host) {
    RuntimeSync sync{};
    sync.sessionId = negotiated.sessionId;
    sync.compatibilityId = negotiated.compatibilityId;
    sync.compatibilityIdentity = negotiated.compatibilityIdentity;
    sync.cookieLow = negotiated.playerCookieLow;
    sync.cookieHigh = negotiated.playerCookieHigh;
    sync.routeKind = routeKind;
    sync.senderRole = host ? RUNTIME_ROLE_HOST : RUNTIME_ROLE_CLIENT;
    return sync;
}

bool validRuntimeSync(const RuntimeSync& sync,
                      const NetplayConfig& negotiated,
                      uint8_t routeKind, bool localHost) {
    const uint8_t expectedRole =
        localHost ? RUNTIME_ROLE_CLIENT : RUNTIME_ROLE_HOST;
    return sync.sessionId == negotiated.sessionId
        && sync.compatibilityId == negotiated.compatibilityId
        && qnetplay::compat::semanticallyEqual(
            sync.compatibilityIdentity, negotiated.compatibilityIdentity)
        && sync.cookieLow == negotiated.playerCookieLow
        && sync.cookieHigh == negotiated.playerCookieHigh
        && sync.routeKind == routeKind
        && sync.senderRole == expectedRole
        && sync.reserved[0] == 0
        && sync.reserved[1] == 0;
}

struct SidecarLaunch {
    bool enabled = false;
    qclient::spectator::SidecarConfig config{};
    UdpSock* preboundSourceSocket = nullptr;
    std::string spectateAddress;
    std::string unavailableReason;
    uint32_t sessionId = 0;
    uint16_t advertisedPort = 0;
    qclient::spectator::AdmissionPolicy policy =
        qclient::spectator::AdmissionPolicy::Public;
};

struct ClientPrivateSidecarLaunch {
    bool enabled = false;
    qclient::spectator::SidecarConfig config{};
    UdpSock* preboundSourceSocket = nullptr;
    qclient::spectator::ClientPrivateRuntimeAppliedReceiver receiptReceiver;
    uint16_t boundPort = 0u;
};

struct ClientPrivatePublisherPlan {
    bool enabled = false;
    uint32_t generation = 0u;
    uint64_t nonce = 0u;
    std::string offReason;
};

ClientPrivatePublisherPlan planClientPrivatePublisher(const Settings& settings) {
    ClientPrivatePublisherPlan plan;
    if (!settings.clientPrivateSpectatorPublisher) {
        plan.offReason = "join-side client-private publisher is disabled";
        return plan;
    }

    bool generationDegraded = false;
    bool nonceDegraded = false;
    const uint64_t generationRandom =
        qnet::secureRandom64(generationDegraded);
    const uint64_t nonce = qnet::secureRandom64(nonceDegraded);
    uint32_t generation = static_cast<uint32_t>(generationRandom);
    if (generation == 0u)
        generation = static_cast<uint32_t>(generationRandom >> 32u);
    if (generationDegraded || nonceDegraded || generation == 0u || nonce == 0u) {
        plan.offReason =
            "a cryptographically strong client-private receipt identity could not be created";
        QLOG("[spectator.client-private] disabled: secure receipt identity "
             "unavailable (generationDegraded=%d nonceDegraded=%d)",
             generationDegraded ? 1 : 0, nonceDegraded ? 1 : 0);
        return plan;
    }
    plan.enabled = true;
    plan.generation = generation;
    plan.nonce = nonce;
    return plan;
}

void disableClientPrivatePublisher(qipc::IpcConfig& cfg) {
    cfg.serveSpectators = 0u;
    cfg.spectatorServiceMode = qipc::SPECTATOR_SERVICE_OFF;
    cfg.spectatorMaxViewers = 0u;
    cfg.spectatorClientPublisherEnabled = 0u;
    std::memset(cfg.reservedV22, 0, sizeof(cfg.reservedV22));
    cfg.spectatorClientPublisherGeneration = 0u;
    cfg.spectatorClientPublisherNonce = 0u;
}

bool prepareClientPrivateSidecarLaunch(
    qipc::IpcConfig& cfg, const ClientPrivatePublisherPlan& plan,
    UdpSock& sourceSocket, ClientPrivateSidecarLaunch& out,
    std::string& error) {
    out.receiptReceiver.close();
    out.enabled = false;
    out.config = {};
    out.preboundSourceSocket = nullptr;
    out.boundPort = 0u;
    error.clear();
    if (!plan.enabled) {
        error = plan.offReason.empty()
            ? "the client-private publisher was not planned"
            : plan.offReason;
        return false;
    }
    if (!sourceSocket.open(0u) || sourceSocket.boundPort() == 0u) {
        const int socketError = sourceSocket.lastError();
        sourceSocket.close();
        error = "the client-private source could not bind an ephemeral UDP socket";
        QLOG("[spectator.client-private] disabled: port-0 source bind failed "
             "(error=%d)", socketError);
        return false;
    }

    cfg.serveSpectators = 1u;
    cfg.spectatorServiceMode = qipc::SPECTATOR_SERVICE_SIDECAR;
    cfg.spectatorMaxViewers = 0u;
    cfg.spectatorClientPublisherEnabled = 1u;
    std::memset(cfg.reservedV22, 0, sizeof(cfg.reservedV22));
    cfg.spectatorClientPublisherGeneration = plan.generation;
    cfg.spectatorClientPublisherNonce = plan.nonce;
    if (const char* why = qipc::validateSpectatorService(cfg)) {
        error = std::string("the client-private IPC launch was rejected: ") + why;
        sourceSocket.close();
        disableClientPrivatePublisher(cfg);
        return false;
    }
    if (const char* why = qipc::validateSpectatorSocketHandoff(cfg)) {
        error = std::string("the client-private source handoff was rejected: ")
            + why;
        sourceSocket.close();
        disableClientPrivatePublisher(cfg);
        return false;
    }

    qclient::spectator::SidecarConfig config{};
    config.identity.sessionId = cfg.sessionId;
    config.identity.epoch = 1u;
    config.identity.cookie.low = cfg.spectatorCookieLow;
    config.identity.cookie.high = cfg.spectatorCookieHigh;
    config.simulationContract = cfg.compatibilityId;
    config.compatibilityIdentity = cfg.compatibilityIdentity;
    config.listenPort = sourceSocket.boundPort();
    config.policy = qclient::spectator::AdmissionPolicy::CodeOnly;
    config.maxViewers = 1u;
    config.maxReservations = 1u;
    config.clientPrivateRoot.enabled = true;
    config.clientPrivateRoot.publisherGeneration = plan.generation;
    config.clientPrivateRoot.publisherNonce = plan.nonce;

    out.enabled = true;
    out.config = std::move(config);
    out.preboundSourceSocket = &sourceSocket;
    out.boundPort = sourceSocket.boundPort();
    QLOG("[spectator.client-private] prepared dormant collector on local UDP "
         "port %u (generation=%u)", static_cast<unsigned>(out.boundPort),
         plan.generation);
    return true;
}

struct SpectatorServing {
    qclient::spectator::AdmissionPolicy policy =
        qclient::spectator::AdmissionPolicy::Public;
    bool available = false;
    uint16_t port = 0;
    uint16_t publicPort = 0;
    bool publicPortProven = false;
    std::string unavailableReason;
};

void startSpectatorPortMapping(qnet::portmap::PortMapper& mapper,
                               const SpectatorServing& serving) {
    if (!serving.available || serving.port == 0) return;
    if (GetEnvironmentVariableA("QOH99_NP_TEST_SPECTATOR_SOURCE_PORT",
                                nullptr, 0) != 0) {
        QLOG("[port-map] skipped: the test spectator port is pinned, so this "
             "session does not touch the router");
        return;
    }
    mapper.requestUdp(serving.port, "QOH99 spectators");
    QLOG("[port-map] asking the router to open UDP %u for spectators",
         static_cast<unsigned>(serving.port));
}

void refreshSpectatorPortMapping(const qnet::portmap::PortMapper& mapper,
                                 SpectatorServing& serving) {
    const qnet::portmap::MappingStatus status = mapper.status();
    if (status.state != qnet::portmap::MappingState::Mapped) return;
    if (status.externalPort == 0) return;
    serving.publicPort = status.externalPort;
    serving.publicPortProven = true;
}

void reportSpectatorPortMapping(const qnet::portmap::PortMapper& mapper,
                                bool& reported, qui::Console* con,
                                SpectatorServing& serving) {
    refreshSpectatorPortMapping(mapper, serving);
    if (reported) return;
    const qnet::portmap::MappingStatus status = mapper.status();
    if (status.state == qnet::portmap::MappingState::Idle
        || status.state == qnet::portmap::MappingState::Working) {
        return;
    }
    reported = true;
    if (status.state == qnet::portmap::MappingState::Mapped) {
        QLOG("[port-map] mapped spectator UDP %u -> internal %u via %s: %s",
             static_cast<unsigned>(status.externalPort),
             static_cast<unsigned>(status.internalPort),
             status.method.c_str(), status.detail.c_str());
        info(con, "Your router opened UDP "
                  + std::to_string(status.externalPort)
                  + " for spectators (" + status.method + ").");
        return;
    }
    QLOG("[port-map] spectator port not mapped: %s", status.detail.c_str());
    info(con, "Spectator port: " + status.detail
              + ". Watching still works over hole punching.");
}

SpectatorServing prepareSpectatorEndpoint(const Settings& s,
                                          UdpSock& sourceSocket,
                                          qui::Console* con) {
    SpectatorServing serving;
    serving.policy = admissionPolicyOf(s);
    if (serving.policy == qclient::spectator::AdmissionPolicy::Disabled) {
        serving.unavailableReason =
            "spectator serving is switched off in Settings";
        info(con, "Spectators: off (Settings -> Let people watch). No "
                  "spectator port is opened.");
        return serving;
    }
    uint16_t requestedPort = 0;
    bool testRequestedPort = false;
    char requestedPortText[16] = {};
    const DWORD requestedPortLength = GetEnvironmentVariableA(
        "QOH99_NP_TEST_SPECTATOR_SOURCE_PORT", requestedPortText,
        static_cast<DWORD>(sizeof(requestedPortText)));
    if (requestedPortLength != 0) {
        testRequestedPort = true;
        const bool truncated = requestedPortLength >= sizeof(requestedPortText);
        const std::string requested = truncated
            ? std::string("<oversize>")
            : std::string(requestedPortText, requestedPortLength);
        if (truncated || !parsePortNumber(requested, requestedPort)) {
            serving.unavailableReason =
                "the test spectator source port was invalid";
            QLOG("[host] spectator source bind attempt rejected: invalid "
                 "test port '%s'; optional spectator service disabled; "
                 "player session continues",
                 requested.c_str());
            info(con, "Spectators are unavailable this session: "
                      + serving.unavailableReason
                      + ". Hosting continues and the players are unaffected.");
            return serving;
        }
    }
    const char* portMode = "ephemeral";
    if (testRequestedPort) {
        portMode = "test-explicit";
    } else if (s.spectatorPort > 0 && s.spectatorPort <= 65535) {
        requestedPort = static_cast<uint16_t>(s.spectatorPort);
        portMode = "configured";
    } else if (s.port != 0 && s.port < 65535) {
        requestedPort = static_cast<uint16_t>(s.port + 1);
        portMode = "port+1";
    }
    QLOG("[host] spectator source bind attempt mode=%s port=%u", portMode,
         static_cast<unsigned>(requestedPort));
    bool opened = sourceSocket.open(requestedPort);
    if (!opened && requestedPort != 0 && !testRequestedPort) {
        const int firstError = sourceSocket.lastError();
        const uint16_t firstChoice = requestedPort;
        constexpr int kLadder = 8;
        for (int step = 1; step <= kLadder && !opened; ++step) {
            const int candidate = static_cast<int>(firstChoice) + step;
            if (candidate > 65535) break;
            sourceSocket.close();
            requestedPort = static_cast<uint16_t>(candidate);
            opened = sourceSocket.open(requestedPort);
            if (opened) {
                portMode = "ladder";
                QLOG("[host] spectator source port %u was busy (err=%d); "
                     "using %u instead - still a fixed port a router can "
                     "forward",
                     static_cast<unsigned>(firstChoice), firstError,
                     static_cast<unsigned>(requestedPort));
                info(con, "Spectator port " + std::to_string(firstChoice)
                          + " is in use; using "
                          + std::to_string(requestedPort) + " instead.");
            }
        }
        if (!opened) {
            QLOG("[host] spectator ports %u..%u are all unavailable "
                 "(first err=%d); falling back to an OS-assigned port. "
                 "Spectators can still be reached over a mapped, punched or "
                 "LAN route, but a router forwarding rule for %u will NOT "
                 "apply this session",
                 static_cast<unsigned>(firstChoice),
                 static_cast<unsigned>(firstChoice + kLadder), firstError,
                 static_cast<unsigned>(firstChoice));
            info(con, "Spectator ports " + std::to_string(firstChoice) + "-"
                      + std::to_string(firstChoice + kLadder)
                      + " are all in use; using a temporary port instead. A "
                        "router rule you wrote for them will not apply this "
                        "session.");
            sourceSocket.close();
            requestedPort = 0;
            portMode = "ephemeral-fallback";
            opened = sourceSocket.open(0);
        }
    }
    const uint16_t boundPort = opened ? sourceSocket.boundPort() : 0;
    if (!opened || boundPort == 0) {
        const int socketError = sourceSocket.lastError();
        sourceSocket.close();
        serving.unavailableReason = testRequestedPort
            ? "the requested spectator UDP source socket could not be opened"
            : "an ephemeral spectator UDP socket could not be opened";
        QLOG("[host] spectator source bind failed mode=%s port=%u err=%d; "
             "optional spectator service disabled; player session continues "
             "WITHOUT spectator serving",
             portMode, static_cast<unsigned>(requestedPort), socketError);
        info(con, "Spectators are unavailable this session: "
                  + serving.unavailableReason
                  + ". Hosting continues and the players are unaffected.");
        return serving;
    }
    serving.port = boundPort;
    serving.available = true;
    QLOG("[host] spectator source bound mode=%s UDP %u policy=%s "
         "(forwardable=%d)",
         portMode, static_cast<unsigned>(serving.port),
         admissionPolicyName(serving.policy),
         (portMode[0] == 'e') ? 0 : 1);
    return serving;
}

bool testSpectatorStartupFailureRequested() {
    char value[64] = {};
    const DWORD length = GetEnvironmentVariableA(
        "QOH99_NP_TEST_SPECTATOR_STARTUP_FAILURE", value,
        static_cast<DWORD>(sizeof(value)));
    return length != 0 && length < sizeof(value)
        && std::string(value, length) == "after-source-acquisition";
}

void publishSpectatorStartupContract(const SidecarLaunch& sidecar,
                                     bool sidecarStarted,
                                     qui::Console* con) {
    const bool available = sidecar.enabled && sidecarStarted;
    if (available) {
        if (sidecar.spectateAddress.empty()) {
            QLOG("[spectator-access] player-address-unavailable");
            if (con) {
                con->status("Spectator service is running. Viewers should use "
                            "the same ip:port used to join this host.");
            } else {
                std::printf("[spectator-access] address-unavailable\n");
                std::fflush(stdout);
            }
        } else {
            if (con) {
                con->status("Spectate address (press S to copy): "
                            + sidecar.spectateAddress);
            } else {
                std::printf("[spectator-access] address=%s\n",
                            sidecar.spectateAddress.c_str());
                std::fflush(stdout);
            }
            QLOG("[spectator-access] address=%s",
                 sidecar.spectateAddress.c_str());
        }
        info(con, std::string("Who may watch: ")
                  + admissionPolicyName(sidecar.policy));
        QLOG("[spectator-contract] v=1 state=access-ready "
             "transport=direct-udp receiveOnly=1 port=%u session=%u",
             static_cast<unsigned>(sidecar.advertisedPort), sidecar.sessionId);
        if (!con) {
            std::printf("[spectator-contract] v=1 state=access-ready "
                        "transport=direct-udp receiveOnly=1 port=%u "
                        "session=%u\n",
                        static_cast<unsigned>(sidecar.advertisedPort),
                        sidecar.sessionId);
            std::fflush(stdout);
        }
    } else {
        const char* reason = sidecar.unavailableReason.empty()
            ? "the spectator source did not complete startup"
            : sidecar.unavailableReason.c_str();
        info(con, std::string("Spectators are unavailable this session (")
                  + reason + "); the match itself is unaffected.");
        QLOG("[spectator-contract] v=1 state=unavailable session=%u "
             "reason='%s' no-address-published",
             sidecar.sessionId, reason);
        QLOG("[host] optional spectator service disabled; player session "
             "continues; no spectator address published");
        if (!con) {
            std::printf("[spectator-contract] v=1 state=unavailable "
                        "session=%u\n", sidecar.sessionId);
            std::fflush(stdout);
        }
    }
    QLOG("[host] spectator serving=%d port=%u session=%u%s",
         available ? 1 : 0, static_cast<unsigned>(
             available ? sidecar.advertisedPort : 0), sidecar.sessionId,
         available ? "" : " (no spectator address published)");
}

qclient::spectator::SidecarConfig sidecarConfigFor(
    const Settings& s, const SpectatorServing& serving,
    const NetplayConfig& negotiated,
    const RelayRuntime* playerRelay,
    const qnet::CandidateEndpoint* rootQspCandidate) {
    qclient::spectator::SidecarConfig config;
    config.identity.sessionId = negotiated.sessionId;
    // Epoch 1 is the value the game's source and viewer use; on a mismatch every datagram is rejected as another session.
    config.identity.epoch = 1;
    config.identity.cookie.low = negotiated.spectatorCookieLow;
    config.identity.cookie.high = negotiated.spectatorCookieHigh;
    config.simulationContract = negotiated.compatibilityId;
    config.compatibilityIdentity = negotiated.compatibilityIdentity;
    config.listenPort = serving.port;
    config.policy = serving.policy;
    config.maxViewers = static_cast<size_t>(s.spectatorMaxViewers);

    if (serving.available && rootQspCandidate != nullptr
        && rootQspCandidate->kind != qnet::CandidateKind::Relay
        && rootQspCandidate->ipv4BE != 0u
        && rootQspCandidate->portBE != 0u
        && ntohs(rootQspCandidate->portBE) == serving.port) {
        qclient::spectator::topology::TopologyActivationRequest& topology =
            config.topologyActivation;
        topology.requested = true;
        topology.rootQsp.local =
            qclient::spectator::RouteEndpoint{0u, serving.port};
        topology.rootQsp.localProvenance =
            qclient::spectator::topology::EndpointProvenance::LocalBind;
        topology.rootQsp.advertised.kind =
            qnetplay::spectator::resilient::RouteKind::Direct;
        topology.rootQsp.advertised.ipv4BE = rootQspCandidate->ipv4BE;
        topology.rootQsp.advertised.port =
            ntohs(rootQspCandidate->portBE);
        topology.rootQsp.advertised.generation = 1u;
        topology.rootQsp.advertised.routeProved = true;
        topology.rootQsp.advertisedProvenance =
            qclient::spectator::topology::EndpointProvenance::
                PublishedDirectCandidate;
        topology.rootQsp.generation = 1u;
        topology.sourceGeneration = 1u;
    }

    if (playerRelay != nullptr
        && playerRelay->pair.role == qnet::relay::RelayRole::Host
        && qnet::relay::isValidId(
               playerRelay->pair.spectatorRegistrationToken)) {
        config.sourceRelay.enabled = true;
        config.sourceRelay.relay.ipv4BE =
            playerRelay->pair.relayEndpoint.sa.sin_addr.s_addr;
        config.sourceRelay.relay.port =
            ntohs(playerRelay->pair.relayEndpoint.sa.sin_port);
        config.sourceRelay.room = playerRelay->pair.room;
        config.sourceRelay.sourceToken =
            playerRelay->pair.spectatorRegistrationToken;
        config.sourceRelay.identity = config.identity;
    }

    Endpoint punchRelay;
    std::string punchError;
    if (autopunchRelayEndpoint(punchRelay, punchError)) {
        config.sourceAutopunch.enabled = true;
        config.sourceAutopunch.relay.ipv4BE =
            punchRelay.sa.sin_addr.s_addr;
        config.sourceAutopunch.relay.port =
            ntohs(punchRelay.sa.sin_port);
    } else {
        QLOG("[spectator.sidecar] source AutoPunch unavailable: %s; "
             "direct spectator serving remains enabled",
             punchError.empty() ? "relay resolution failed"
                                : punchError.c_str());
    }

    auto grantKey = std::make_shared<qnetplay::spectator::SpectatorGrantKey>();
    {
        bool degraded = false;
        for (size_t i = 0; i < grantKey->size(); i += sizeof(uint64_t)) {
            const uint64_t chunk = qnet::secureRandom64(degraded);
            std::memcpy(grantKey->data() + i, &chunk, sizeof(chunk));
        }
        if (degraded)
            QLOG("[spectator.sidecar] the grant key used the last-resort RNG "
                 "path");
    }
    config.signGrant = [grantKey](qnetplay::spectator::SpectatorGrant& grant) {
        qnetplay::spectator::issueGrant(*grantKey, grant);
    };
    config.verifyGrant =
        [grantKey](const qnetplay::spectator::SpectatorGrant& grant,
                   uint64_t nowUnix) {
            return qnetplay::spectator::verifyGrant(
                       *grantKey, grant, grant.viewerId, grant.sessionId,
                       nowUnix)
                == qnetplay::spectator::GrantVerdict::Ok;
        };
    config.unixClock = []() {
        return static_cast<uint64_t>(std::time(nullptr));
    };
    config.randomU64 = []() {
        bool degraded = false;
        return qnet::secureRandom64(degraded);
    };
    return config;
}

struct RuntimeFailure {
    bool present = false;
    unsigned code = 0;
    std::string reason;
};

std::string readGracefulEndReason() {
    if (g_sessionDir.empty()) return {};
    const std::string path = g_sessionDir + "\\netplay_end.txt";
    FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return {};
    std::string reason;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "reason=", 7) == 0) {
            reason = line + 7;
            while (!reason.empty()
                   && (reason.back() == '\n' || reason.back() == '\r')) {
                reason.pop_back();
            }
        }
    }
    std::fclose(f);
    return reason;
}

RuntimeFailure readRuntimeFailure() {
    RuntimeFailure failure{};
    if (g_sessionDir.empty()) return failure;
    const std::string path = g_sessionDir + "\\netplay_failure.txt";
    FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return failure;
    char line[512];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "code=", 5) == 0) {
            failure.code = static_cast<unsigned>(std::strtoul(line + 5,
                                                              nullptr, 10));
        } else if (std::strncmp(line, "reason=", 7) == 0) {
            failure.reason = line + 7;
            while (!failure.reason.empty()
                   && (failure.reason.back() == '\n'
                       || failure.reason.back() == '\r')) {
                failure.reason.pop_back();
            }
        }
    }
    std::fclose(f);
    failure.present = !failure.reason.empty();
    return failure;
}

void reportRuntimeFailure(qui::Console* con, const RuntimeFailure& failure) {
    if (!failure.present) return;
    QLOG_ISSUE("[game-failure] code=%u reason=%s", failure.code,
         failure.reason.c_str());
    if (con == nullptr) return;
    std::string body = "The match was stopped because ";
    body += failure.reason;
    body += ".\n\n";
    if (failure.reason.find("differ") != std::string::npos) {
        body += "Both players must run the SAME game files. Rollback "
                "netplay replays each other's inputs against an identical "
                "copy of the game, so any difference in the installed "
                "content makes the two sides drift apart within seconds. "
                "Stopping is what prevents a match that silently desyncs.\n\n"
                "Compare your game folder with your opponent's - the same "
                "SE build, the same System folder, and no extra mods or "
                "replaced sound files on either side.";
    } else {
        body += "The full diagnosis is in this session's dll.log.";
    }
    con->message("Match stopped", body);
}

int reportSpectatorRuntimeExit(qui::Console* con, const int childExit) {
    const RuntimeFailure failure = readRuntimeFailure();
    if (failure.present) {
        QLOG_ISSUE("[spectator-runtime] state=failed code=%u reason='%s'",
             failure.code, failure.reason.c_str());
        const std::string detail =
            failure.reason.empty()
                ? std::string("The spectator runtime stopped with an "
                              "unspecified failure. See dll.log.")
                : failure.reason;
        const int semanticFailure =
            fail(con, "Spectator stopped", detail);
        return qoh99PreferNativeChildExit(childExit, semanticFailure);
    }
    if (childExit != 0) {
        char detail[160];
        std::snprintf(
            detail, sizeof(detail),
            "The spectator game process exited unexpectedly "
            "(native code 0x%08X). See dll.log and the caster log.",
            static_cast<unsigned>(childExit));
        QLOG_ISSUE("[spectator-runtime] state=child-exit-failed native=0x%08X",
             static_cast<unsigned>(childExit));
        (void)fail(con, "Spectator stopped", detail);
        return childExit;
    }
    QLOG("[spectator-runtime] state=closed-cleanly");
    return 0;
}

bool launchOnlineSynchronized(
    const Settings& settings, qipc::IpcConfig cfg,
    SOCKET playerSocket, SOCKET spectatorSocket,
    ControlLink& ctrl, PumpCtx& pump,
    const NetplayConfig& negotiated, uint8_t routeKind,
    bool localHost, bool retainRelayLease,
    qui::Console* con, bool hiddenDesktop, std::string& error,
    const std::string& spectateAddress = "",
    std::shared_ptr<HostControlEndpoint> liveListener = {},
    const SpectateInfo* liveInfo = nullptr,
    SidecarLaunch* sidecar = nullptr,
    ClientPrivateSidecarLaunch* clientPrivateSidecar = nullptr)
{
    const std::string game = resolveGamePath(settings);
    const std::string dll = resolveDllPath(settings);
    const std::string workingDirectory = dirOf(game);
    RecordingPlan recording = planRecording(settings, cfg);
    if (recording.enabled) {
        std::array<uint8_t, 32> contentRoot{};
        if (!replayContentRootFor(settings, contentRoot)) {
            recording.enabled = false;
            recording.generation = 0u;
            recording.nonce = 0u;
            recording.offReason =
                "the simulation-content manifest was incomplete";
            info(con, "Not recording this match: "
                      + recording.offReason
                      + ". The match itself is unaffected.");
        } else {
            std::memcpy(
                cfg.replayContentManifestRoot, contentRoot.data(),
                contentRoot.size());
        }
    }
    cfg.recordEnabled = recording.enabled ? 1 : 0;
    cfg.recordChannelGeneration = recording.generation;
    cfg.recordChannelNonce = recording.nonce;
    QLOG("[launch-sync] preparing game='%s' dll='%s' host=%d route=%s "
         "playerUdp=%s spectatorUdp=%s record=%u",
         game.c_str(), dll.c_str(), localHost ? 1 : 0,
         routeText(routeKind),
         playerSocket == INVALID_SOCKET ? "none" : "bound",
         spectatorSocket == INVALID_SOCKET ? "none" : "dedicated",
         cfg.recordEnabled);
    const bool clientPrivateRequested =
        clientPrivateSidecar != nullptr && clientPrivateSidecar->enabled;
    if ((sidecar != nullptr && sidecar->enabled) && clientPrivateRequested) {
        error = "a public host sidecar and a client-private collector cannot share one launch";
        return false;
    }
    if ((cfg.spectatorClientPublisherEnabled == 1u)
        != clientPrivateRequested) {
        error = "the client-private publisher IPC role did not match its local collector";
        return false;
    }
    info(con, "Getting the game ready...");

    qclient::PreparedLaunch prepared = qclient::prepareInjected(
        game, dll, workingDirectory, "", cfg, playerSocket,
        hiddenDesktop, spectatorSocket);
    if (!prepared.ok) {
        error = "Local runtime preparation failed: " + prepared.error;
        sendControlError(ctrl, error);
        ctrl.closeGraceful();
        return false;
    }
    QLOG("[launch-sync] local runtime ready pid=%lu", prepared.pid);

    qclient::replay::ReplayRecorderHostV3 recorder;
    bool recorderStarted = false;
    startRecorderForLaunch(settings, recording, prepared.pid, recorder,
                           recorderStarted, con);

    bool sidecarStarted = false;
    bool clientPrivateSidecarStarted = false;
    std::string sidecarUnavailableReason;
    if (sidecar != nullptr && sidecar->enabled) {
        if (sidecar->preboundSourceSocket == nullptr
            || !sidecar->preboundSourceSocket->isOpen()
            || sidecar->preboundSourceSocket->boundPort() == 0) {
            sidecarUnavailableReason =
                "the ephemeral source socket was lost before sidecar startup";
            QLOG("[spectator.sidecar] UNAVAILABLE: the ephemeral source socket "
                 "was lost before sidecar startup");
        } else {
            // The sidecar adopts this exact OS-assigned socket handle and must never close or rebind it.
            sidecar->config.preboundSocket = static_cast<uintptr_t>(
                sidecar->preboundSourceSocket->release());
            std::string sidecarError;
            if (qclient::spectator::startSidecar(sidecar->config, prepared.pid,
                                                 sidecarError)) {
                const qclient::spectator::SidecarStatus status =
                    qclient::spectator::sidecarStatus();
                if (status.serving
                    && status.boundPort == sidecar->advertisedPort) {
                    sidecarStarted = true;
                    QLOG("[spectator.sidecar] serving on UDP %u policy=%s",
                         static_cast<unsigned>(status.boundPort),
                         admissionPolicyName(sidecar->config.policy));
                } else {
                    sidecarUnavailableReason =
                        "the spectator sidecar did not retain its advertised "
                        "source endpoint";
                    qclient::spectator::stopSidecar(false);
                    QLOG("[spectator.sidecar] UNAVAILABLE: %s (the player "
                         "match is unaffected)",
                         sidecarUnavailableReason.c_str());
                }
            } else {
                qclient::spectator::stopSidecar(false);
                sidecarUnavailableReason = sidecarError.empty()
                    ? "the spectator sidecar could not start"
                    : sidecarError;
                QLOG("[spectator.sidecar] UNAVAILABLE: %s (the player match is "
                     "unaffected)", sidecarUnavailableReason.c_str());
            }
        }
    }

    if (clientPrivateRequested) {
        std::string privateUnavailableReason;
        if (clientPrivateSidecar->preboundSourceSocket == nullptr
            || !clientPrivateSidecar->preboundSourceSocket->isOpen()
            || clientPrivateSidecar->preboundSourceSocket->boundPort() == 0u) {
            privateUnavailableReason =
                "the client-private ephemeral source socket was lost before startup";
        } else if (!clientPrivateSidecar->receiptReceiver.open(
                       prepared.pid,
                       cfg.spectatorClientPublisherGeneration,
                       privateUnavailableReason)) {
        } else {
            clientPrivateSidecar->config.preboundSocket =
                static_cast<uintptr_t>(
                    clientPrivateSidecar->preboundSourceSocket->release());
            std::string privateSidecarError;
            if (qclient::spectator::startClientPrivateSidecar(
                    clientPrivateSidecar->config, prepared.pid,
                    privateSidecarError)) {
                const qclient::spectator::SidecarStatus status =
                    qclient::spectator::sidecarStatus();
                if (status.clientPrivateRootDormant
                    && !status.clientPrivateRuntimeApplied
                    && !status.serving
                    && status.boundPort == clientPrivateSidecar->boundPort) {
                    clientPrivateSidecarStarted = true;
                    QLOG("[spectator.client-private] dormant local collector "
                         "armed on UDP %u (generation=%u; no public root)",
                         static_cast<unsigned>(status.boundPort),
                         cfg.spectatorClientPublisherGeneration);
                } else {
                    privateUnavailableReason =
                        "the client-private collector did not retain its dormant source endpoint";
                    (void)qclient::spectator::stopSidecar(false);
                }
            } else {
                privateUnavailableReason = privateSidecarError.empty()
                    ? "the client-private collector could not start"
                    : privateSidecarError;
                (void)qclient::spectator::stopSidecar(false);
            }
        }
        if (!clientPrivateSidecarStarted) {
            clientPrivateSidecar->receiptReceiver.close();
            QLOG("[spectator.client-private] unavailable: %s; player match "
                 "continues without a client root",
                 privateUnavailableReason.empty()
                     ? "collector startup failed"
                     : privateUnavailableReason.c_str());
        }
    }

    std::string activeSpectateAddress = sidecar != nullptr
        ? sidecar->spectateAddress : spectateAddress;
    SpectateInfo unavailableSpectateInfo =
        liveInfo != nullptr ? *liveInfo : SpectateInfo{};
    const SpectateInfo* activeLiveInfo = liveInfo;
    if (sidecar != nullptr && sidecar->enabled && !sidecarStarted) {
        activeSpectateAddress.clear();
        unavailableSpectateInfo.state = SPECTATE_NO_MATCH;
        clearSpectateSecrets(unavailableSpectateInfo);
        activeLiveInfo =
            liveInfo != nullptr ? &unavailableSpectateInfo : nullptr;
        sidecar->unavailableReason = sidecarUnavailableReason.empty()
            ? "the spectator sidecar did not complete startup"
            : sidecarUnavailableReason;
        QLOG("[spectator-contract] v=2 state=withdrawn reason=sidecar-start");
    }
    struct SidecarStopper {
        bool active = false;
        ~SidecarStopper() {
            if (active) qclient::spectator::stopSidecar(false);
        }
    } sidecarStopper;
    sidecarStopper.active = sidecarStarted || clientPrivateSidecarStarted;

    const RuntimeSync local =
        runtimeSyncFor(negotiated, routeKind, localHost);
    if (!ctrl.sendMsg(MSG_RUNTIME_READY, &local, sizeof(local))) {
        error = "Could not send local runtime-ready barrier";
        ctrl.close();
        return false;
    }
    info(con, "Waiting for opponent...");

    TcpConn::Received received;
    std::string remoteError;
    RuntimeSync peer{};
    const Pump barrierPump = pumpRecv(ctrl, pump, MSG_RUNTIME_READY, 45000,
                                      received, &remoteError);
    if (barrierPump != Pump::Got) {
        QLOG("[launch-sync] runtime-ready barrier failed pump=%d "
             "socketError=%d relayed=%d",
             static_cast<int>(barrierPump), ctrl.lastError(),
             retainRelayLease ? 1 : 0);
    }
    switch (barrierPump) {
        case Pump::Got:
            if (!bodyAs(received, peer)
                || !validRuntimeSync(peer, negotiated, routeKind,
                                     localHost)) {
                error = "Peer sent an invalid runtime-ready barrier";
                sendControlError(ctrl, error);
                ctrl.closeGraceful();
                return false;
            }
            break;
        case Pump::Timeout:
            error = "Peer did not prepare its runtime within 45 s";
            sendControlError(ctrl, error);
            ctrl.closeGraceful();
            return false;
        case Pump::PeerClosed:
            error = "Peer disconnected while preparing its runtime";
            return false;
        case Pump::RemoteError:
            error = "Peer runtime preparation failed: " + remoteError;
            return false;
        default:
            error = "Control stream failed during runtime-ready barrier";
            return false;
    }

    info(con, "Both players are ready - starting the game!");
    if (!ctrl.sendMsg(MSG_RUNTIME_START, &local, sizeof(local))) {
        error = "Could not send local runtime-start barrier";
        ctrl.close();
        return false;
    }
    switch (pumpRecv(ctrl, pump, MSG_RUNTIME_START, 15000,
                     received, &remoteError)) {
        case Pump::Got:
            if (!bodyAs(received, peer)
                || !validRuntimeSync(peer, negotiated, routeKind,
                                     localHost)) {
                error = "Peer sent an invalid runtime-start barrier";
                sendControlError(ctrl, error);
                ctrl.closeGraceful();
                return false;
            }
            break;
        case Pump::Timeout:
            error = "Peer did not enter the runtime-start barrier within 15 s";
            sendControlError(ctrl, error);
            ctrl.closeGraceful();
            return false;
        case Pump::PeerClosed:
            error = "Peer disconnected before synchronized game start";
            return false;
        case Pump::RemoteError:
            error = "Peer rejected synchronized game start: " + remoteError;
            return false;
        default:
            error = "Control stream failed during runtime-start barrier";
            return false;
    }

    qclient::LaunchResult launched =
        qclient::resumePrepared(std::move(prepared));
    if (!launched.ok) {
        error = "Could not resume prepared game: " + launched.error;
        sendControlError(ctrl, error);
        ctrl.closeGraceful();
        return false;
    }
    QLOG("[launch-sync] synchronized resume complete pid=%lu", launched.pid);
    if (clientPrivateSidecarStarted) {
        qipc::SpectatorClientPublisherRuntimeAppliedReceipt receipt{};
        std::string receiptError;
        constexpr uint32_t kClientPrivateReceiptDeadlineMs = 5000u;
        const bool received = clientPrivateSidecar->receiptReceiver.receive(
            kClientPrivateReceiptDeadlineMs, receipt, receiptError);
        const bool confirmed = received
            && qclient::spectator::sidecarConfirmClientPrivateRuntimeApplied(
                cfg, receipt, launched.pid, receiptError);
        if (!confirmed) {
            QLOG("[spectator.client-private] RuntimeApplied receipt rejected: "
                 "%s; dormant collector stopped (player match continues)",
                 receiptError.empty() ? "unknown local receipt failure"
                                      : receiptError.c_str());
            (void)qclient::spectator::stopSidecar(false);
            clientPrivateSidecarStarted = false;
            sidecarStopper.active = sidecarStarted;
        } else {
            QLOG("[spectator.client-private] RuntimeApplied receipt accepted "
                 "(generation=%u; root remains inactive pending authenticated "
                 "host enrollment)",
                 cfg.spectatorClientPublisherGeneration);
            std::thread(runClientRootClaimTask, settings.joinIp,
                        settings.joinPort)
                .detach();
        }
    }
    if (sidecar != nullptr && sidecarStarted
        && sidecar->config.topologyActivation.requested) {
        std::string topologyError;
        if (!qclient::spectator::configureSidecarTopologyAuthority(
                sidecar->config.topologyActivation, topologyError)) {
            QLOG("[spectator-topology] unavailable after player resume; E0 "
                 "continues without resilient topology");
        } else {
            QLOG("[spectator-topology] authority activation requested after "
                 "player resume; awaiting accepted source descriptor");
        }
    }
    if (!g_sessionDir.empty()) {
        std::remove((g_sessionDir + "\\netplay_failure.txt").c_str());
        std::remove((g_sessionDir + "\\netplay_end.txt").c_str());
    }
    if (retainRelayLease) {
        QLOG("[relay] retaining control lease until the game exits");
    } else {
        ctrl.closeGraceful();
    }
    if (liveListener != nullptr) {
        if (auto* acceptedPlayer =
                dynamic_cast<qnet::UdpControlLink*>(&ctrl)) {
            liveListener->release(acceptedPlayer);
            QLOG("[host-control] released committed player control slot "
                 "before spectator service");
        }
    }
    if (sidecar != nullptr) {
        publishSpectatorStartupContract(*sidecar, sidecarStarted, con);
    }
    waitForLaunchedGame(launched, con, INFINITE, hiddenDesktop,
                        activeSpectateAddress, liveListener.get(), activeLiveInfo,
                        sidecarStarted,
                        recorderStarted ? &recorder : nullptr);
    const RuntimeFailure runtimeFailure = readRuntimeFailure();
    reportRuntimeFailure(con, runtimeFailure);
    if (!runtimeFailure.present) {
        const std::string endReason = readGracefulEndReason();
        if (!endReason.empty())
            info(con, "Session ended: " + endReason + ".");
    }
    if (sidecarStarted) {
        info(con, "Letting spectators finish the last part of the match...");
        const bool retained =
            qclient::spectator::retainSidecarAfterGame();
        const qclient::spectator::SidecarStatus final =
            qclient::spectator::sidecarStatus();
        QLOG("[spectator.sidecar] retained=%d phase=%s completed=%zu "
             "expired=%zu "
             "kicked=%zu detached=%zu truncatedRecords=%u",
             retained ? 1 : 0,
             qclient::spectator::sidecarPhaseName(final.phase),
             final.viewersCompleted,
             final.viewersDeadlineExpired, final.viewersKicked,
             final.viewersDetached, final.truncatedRecords);
        if (!retained) {
            info(con, "Some spectators did not finish before the drain "
                      "deadline; they were cut off at the last record this "
                      "caster could prove.");
        } else {
            sidecarStopper.active = false;
        }

        bool retainedControlStarted = false;
        if (retained && con != nullptr && liveListener != nullptr
            && activeLiveInfo != nullptr
            && activeLiveInfo->state == SPECTATE_LIVE) {
            retainedControlStarted =
                retainedHostControlOwner().start(
                    liveListener, *activeLiveInfo);
            QLOG("[spectator-retention] production-control-owner=%d "
                 "session=%u",
                 retainedControlStarted ? 1 : 0,
                 activeLiveInfo->sessionId);
            if (!retainedControlStarted) {
                info(con, "The completed spectator archive could not keep "
                          "its admission endpoint open; late joins are now "
                          "disabled.");
                (void)qclient::spectator::stopSidecar(false);
            }
        }

        if (retained && g_spectatorRetainDiagnosticMs != 0) {
            const uint64_t retainStartedUs = nowUs();
            const uint64_t retainDeadlineUs =
                retainStartedUs
                + static_cast<uint64_t>(
                      g_spectatorRetainDiagnosticMs) * 1000ull;
            size_t lastViewerCount = final.viewerCount;
            QLOG("[spectator-retention] enter remainingMs=%u viewers=%zu "
                 "phase=%s",
                 g_spectatorRetainDiagnosticMs, lastViewerCount,
                 qclient::spectator::sidecarPhaseName(final.phase));
            while (nowUs() < retainDeadlineUs) {
                const qclient::spectator::SidecarStatus status =
                    qclient::spectator::sidecarStatus();
                if (status.viewerCount != lastViewerCount) {
                    lastViewerCount = status.viewerCount;
                    const uint64_t now = nowUs();
                    const uint64_t remainingUs =
                        now < retainDeadlineUs
                            ? retainDeadlineUs - now : 0;
                    QLOG("[spectator-retention] late-admission viewers=%zu "
                         "capacity=%zu remainingMs=%llu phase=%s",
                         status.viewerCount, status.viewerCapacity,
                         static_cast<unsigned long long>(
                             remainingUs / 1000ull),
                         qclient::spectator::sidecarPhaseName(status.phase));
                }
                if (liveListener != nullptr && activeLiveInfo != nullptr) {
                    if (status.serving) {
                        serveIntentDuringMatch(
                            *liveListener, *activeLiveInfo, nullptr);
                    } else {
                        SpectateInfo unavailable = *activeLiveInfo;
                        unavailable.state = SPECTATE_NO_MATCH;
                        clearSpectateSecrets(unavailable);
                        serveIntentDuringMatch(
                            *liveListener, unavailable, nullptr);
                    }
                }
                Sleep(10);
            }
            QLOG("[spectator-retention] expiry remainingMs=0 viewers=%zu",
                 qclient::spectator::sidecarStatus().viewerCount);
            const bool stopped =
                qclient::spectator::stopSidecar(false);
            QLOG("[spectator-retention] stop ok=%d", stopped ? 1 : 0);
        } else if (!retained) {
            qclient::spectator::stopSidecar(false);
            sidecarStopper.active = false;
        } else if (con != nullptr && !retainedControlStarted) {
            sidecarStopper.active = false;
        }
    }
    if (clientPrivateSidecarStarted) {
        const bool stopped = qclient::spectator::stopSidecar(false);
        QLOG("[spectator.client-private] dormant collector stopped after "
             "player exit (ok=%d)", stopped ? 1 : 0);
        clientPrivateSidecarStarted = false;
        sidecarStopper.active = false;
    }
    if (recorderStarted) reportRecorderOutcome(recorder, con);
    return true;
}

bool prepareInteractiveOfflineFCommand(const Settings& s,
                                       std::string& error) {
    const std::string path =
        dirOf(resolveGamePath(s)) + "\\LocalConfig\\FCommand.txt";
    std::string original;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            const DWORD attrs = GetFileAttributesA(path.c_str());
            const DWORD attributeError = GetLastError();
            if (attrs == INVALID_FILE_ATTRIBUTES
                && (attributeError == ERROR_FILE_NOT_FOUND
                    || attributeError == ERROR_PATH_NOT_FOUND)) {
                return true;
            }
            error = "Could not read " + path;
            return false;
        }
        original.assign(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>());
    }
    const std::string canonical =
        qclient::fcommand::canonicalCrLfText(original);
    if (canonical == original) return true;

    CopyFileA(path.c_str(), (path + ".caster.bak").c_str(), FALSE);
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(
            temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Could not write a temporary FCommand.txt file.";
            return false;
        }
        out.write(canonical.data(),
                  static_cast<std::streamsize>(canonical.size()));
        if (!out.flush()) {
            error = "Could not finish writing FCommand.txt.";
            return false;
        }
    }
    if (!MoveFileExA(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not replace FCommand.txt. Close the game and retry.";
        return false;
    }
    QLOG("[fcommand] normalized CRLF separators for native offline parser");
    return true;
}

int runOffline(const Settings& s, uint8_t wireMode, uint8_t vsRow, qui::Console* con,
               bool hiddenDesktop = false, bool directBoot = true,
               bool trainingEnabled = false,
               bool allowNativeDeveloperKeys = false) {
    if (allowNativeDeveloperKeys) {
        std::string error;
        if (!prepareInteractiveOfflineFCommand(s, error))
            return fail(con, "FCommand.txt could not be prepared", error);
    }
    qipc::IpcConfig cfg = baseConfig(s, wireMode, 1, 1);
    cfg.trainingEnabled = trainingEnabled ? 1 : 0;
    cfg.disableExtraFunctions =
        allowNativeDeveloperKeys && !trainingEnabled ? 0 : 1;
    cfg.vsRow = trainingEnabled ? qipc::kVsRow1v1 : vsRow;
    fillOfflineExtraConfigs(cfg, !trainingEnabled);
    cfg.directBoot = (directBoot || trainingEnabled)
        && (wireMode == qipc::MODE_NETPLAY || wireMode == qipc::MODE_OBSERVE
            || wireMode == qipc::MODE_SELFTEST) ? 1 : 0;
    if (const char* why = qipc::validateTrainingLaunch(cfg)) {
        return fail(con, "Training launch rejected", why);
    }
    const int result =
        launch(s, cfg, INVALID_SOCKET, con, INFINITE, hiddenDesktop);
    if (trainingEnabled && con) con->discardPendingInput();
    return result;
}

uint8_t probeModeFromName(const std::string& n) {
    if (n == "input-map")        return 1;
    if (n == "fighter-writes")   return 2;
    if (n == "fighter-pointers") return 3;
    if (n == "fighter-size")     return 4;
    if (n == "intro")            return 5;
    if (n == "blocks")           return 6;
    if (n == "timer")            return 7;
    if (n == "rng")              return 8;
    if (n == "helpers")          return 9;
    if (n == "all")              return 255;
    return 0;
}

int runProbe(const Settings& s, uint8_t probeMode, qui::Console* con, bool hiddenDesktop = false) {
    if (!probeMode) { std::printf("Unknown probe mode.\n"); return 1; }
    QLOG("[probe] launching with probeMode=%u hiddenDesktop=%d", probeMode, (int)hiddenDesktop);
    qipc::IpcConfig cfg = baseConfig(s, qipc::MODE_OBSERVE, 1, 1);
    cfg.vsRow = qipc::kVsRow1v1;
    cfg.directBoot = 1;
    cfg.probeMode = probeMode;
    return launch(s, cfg, INVALID_SOCKET, con, 150000, hiddenDesktop);
}

enum class HostAttempt { Launched, LoopBack, Quit, Failed };

HostAttempt hostNegotiate(Settings& s, qui::Console* con, ControlLink& ctrl,
                           const Endpoint& clientEp, UdpSock& udp,
                           UdpSock& spectatorUdp,
                           const SpectatorServing& serving,
                           int flagDelay, int flagRollback,
                           bool hiddenDesktop,
                           RelayRuntime* relay = nullptr,
                           bool rendezvousOnly = false,
                           std::shared_ptr<HostControlEndpoint> liveListener =
                               {},
                           const std::string& advertisedPlayerAddress = {}) {
    std::unique_ptr<ScopedHostControlPump> relayControlPump;
    if (relay != nullptr && liveListener != nullptr) {
        relayControlPump =
            std::make_unique<ScopedHostControlPump>(liveListener);
    }
    PumpCtx ctx; ctx.con = con;
    ctx.lastHeardUs = nowUs();
    TcpConn::Received rx;
    std::string remoteErr;
    auto loopBack = [&](const std::string& why) {
        if (con) con->clear();
        info(con, why + " - waiting for a new opponent...");
        ctrl.close();
        return HostAttempt::LoopBack;
    };

    SpectatorServing effective = serving;

    VersionConfig myVer{ kProtocolVersion };
    if (!ctrl.sendMsg(MSG_VERSION_CONFIG, &myVer, sizeof(myVer)))
        return loopBack("send failed (version), err=" + std::to_string(ctrl.lastError()));
    switch (pumpRecv(ctrl, ctx, MSG_VERSION_CONFIG, 5000, rx, &remoteErr)) {
        case Pump::Got: break;
        case Pump::Timeout:     return loopBack("No version from client within 5 s");
        case Pump::PeerClosed:  return loopBack("Client disconnected during version exchange");
        case Pump::RemoteError: return loopBack("Client error: " + remoteErr);
        default:                return loopBack("Socket error, err=" + std::to_string(ctrl.lastError()));
    }
    VersionConfig theirVer{};
    if (!bodyAs(rx, theirVer))
        return loopBack("Malformed version from client");
    if (theirVer.protocolVersion != kProtocolVersion) {
        ErrorText et{}; std::snprintf(et.text, sizeof(et.text),
            "Protocol mismatch: host v%u, client v%u - update the older side",
            kProtocolVersion, theirVer.protocolVersion);
        bool sent = ctrl.sendMsg(MSG_ERROR_TEXT, &et, sizeof(et));
        QLOG("[host] version mismatch (error-text sent=%d)", (int)sent);
        ctrl.closeGraceful();
        return loopBack(std::string(et.text));
    }

    switch (pumpRecv(ctrl, ctx, MSG_CLIENT_INTENT, 5000, rx, &remoteErr)) {
        case Pump::Got: break;
        case Pump::Timeout:     return loopBack("No intent from client within 5 s");
        case Pump::PeerClosed:  return loopBack("Client disconnected before declaring intent");
        case Pump::RemoteError: return loopBack("Client error: " + remoteErr);
        default:                return loopBack("Socket error, err=" + std::to_string(ctrl.lastError()));
    }
    ClientIntent theirIntent{};
    if (!bodyAs(rx, theirIntent)
        || !validClientIntentRecord(theirIntent))
        return loopBack("Malformed intent from client");
    if (theirIntent.intent != INTENT_JOIN) {
        SpectateInfo idleInfo{};
        idleInfo.state = SPECTATE_NO_MATCH;
        idleInfo.presence = makeLivePresence(
            s.nickname, std::string(), configuredRoundsToWin(s), 0u,
            LIVE_PRESENCE_HOSTING);
        const bool responseSent =
            ctrl.sendMsg(MSG_SPECTATE_INFO, &idleInfo, sizeof(idleInfo));
        ctrl.closeGraceful(1500);
        QLOG("[host-control-visitor] stage=idle-response intent=%u result=%s "
             "peer=%s state=%u socketError=%d",
             static_cast<unsigned>(theirIntent.intent),
             responseSent ? "sent" : "failed", clientEp.str().c_str(),
             static_cast<unsigned>(idleInfo.state), ctrl.lastError());
        return loopBack(
            theirIntent.intent == INTENT_SPECTATE
                ? "A viewer checked in (no match running yet)"
                : (theirIntent.intent == INTENT_STATUS
                       ? "A status client checked in"
                       : "A stale spectator release checked in (no live session)"));
    }

    qnetplay::compat::CompatibilityIdentity localCompatibility{};
    std::string localCompatibilityError;
    if (!localCompatibilityIdentityFor(
            s, localCompatibility, localCompatibilityError)) {
        return loopBack("Local simulation-content preflight failed: "
                        + localCompatibilityError);
    }
    RuntimeArtifactIdentityWire localRuntimeArtifacts{};
    std::string localRuntimeArtifactsError;
    if (!localRuntimeArtifactIdentityFor(
            s, localRuntimeArtifacts, localRuntimeArtifactsError)) {
        QLOG("[netplay] local runtime artifact identity failed: %s",
             localRuntimeArtifactsError.c_str());
        return loopBack("Local netplay build check failed. Restart the "
                        "launcher or reinstall the current build.");
    }
    InitialConfig myInit{};
    qgame::storeCanonicalPlayerNameForCodePage(
        myInit.name, s.nickname, CP_UTF8);
    myInit.nameCodePage = CP_UTF8;
    myInit.isHost = 1; myInit.dataPort = udp.boundPort();
    myInit.roundsToWin = configuredRoundsToWin(s);
    myInit.compatibilityIdentity = localCompatibility;
    myInit.runtimeArtifacts = localRuntimeArtifacts;
    if (!ctrl.sendMsg(MSG_INITIAL_CONFIG, &myInit, sizeof(myInit)))
        return loopBack("send failed (initial), err=" + std::to_string(ctrl.lastError()));
    switch (pumpRecv(ctrl, ctx, MSG_INITIAL_CONFIG, 5000, rx, &remoteErr)) {
        case Pump::Got: break;
        case Pump::Timeout:     return loopBack("No initial config from client within 5 s");
        case Pump::PeerClosed:  return loopBack("Client disconnected during initial exchange");
        case Pump::RemoteError: return loopBack("Client error: " + remoteErr);
        default:                return loopBack("Socket error, err=" + std::to_string(ctrl.lastError()));
    }
    InitialConfig theirInit{};
    if (!bodyAs(rx, theirInit)) return loopBack("Malformed initial config from client");
    if (theirInit.isHost != 0 || theirInit.dataPort == 0
        || theirInit.roundsToWin
            > qnetplay::kMaxRoundsToWinMinusOne + 1u
        || theirInit.reserved[0] != 0 || theirInit.reserved[1] != 0
        || theirInit.reserved[2] != 0)
        return loopBack("Invalid client identity/UDP endpoint");
    const qnetplay::compat::IdentityMismatch initialMismatch =
        qnetplay::compat::compatibilityMismatch(
            localCompatibility, theirInit.compatibilityIdentity);
    if (initialMismatch != qnetplay::compat::IdentityMismatch::None) {
        QLOG("[netplay] client compatibility rejected: %s",
             qnetplay::compat::identityMismatchReason(initialMismatch));
        return loopBack(
            std::string("A player could not join: ")
            + qnetplay::compat::identityMismatchPlayerText(initialMismatch)
            + ".");
    }
    const RuntimeArtifactMismatch artifactMismatch = runtimeArtifactMismatch(
        localRuntimeArtifacts, theirInit.runtimeArtifacts);
    if (artifactMismatch != RuntimeArtifactMismatch::None) {
        QLOG("[netplay] client build rejected: %s",
             runtimeArtifactMismatchReason(artifactMismatch));
        const std::string playerText = buildMismatchPlayerText();
        const bool sent = sendControlError(ctrl, playerText);
        QLOG("[netplay] client build rejection sent=%d", sent ? 1 : 0);
        ctrl.closeGraceful();
        return loopBack("A player could not join: " + playerText);
    }
    theirInit.name[sizeof(theirInit.name) - 1] = '\0';
    if (!qgame::isCanonicalPlayerNameForCodePage(
            theirInit.name, theirInit.nameCodePage)) {
        return loopBack("Invalid client nickname/code-page encoding");
    }
    std::string clientName = theirInit.name;
    const std::string clientDisplayName =
        qgame::playerNameForUtf8Display(
            clientName, theirInit.nameCodePage);
    QLOG("[host] client '%s' at %s (their UDP data port=%u - diagnostic only, "
         "the DLL learns the real peer from its first datagram)",
         clientDisplayName.c_str(), clientEp.str().c_str(),
         theirInit.dataPort);
    info(con, clientDisplayName + " joined ("
              + firstToRoundsText(myInit.roundsToWin) + ").");
    announceOpponentJoined(con, s);

    int pongs = 0;
    uint64_t burstDeadline = nowUs() + 3000000ull;
    for (int i = 0; i < 5 && nowUs() < burstDeadline; ++i) {
        sendPing(ctrl, ctx);
        uint32_t seqSent = ctx.pingSeqNext - 1;
        uint64_t oneDeadline = nowUs() + 1000000ull;
        while (nowUs() < oneDeadline && ctx.pingSentUs[seqSent % 8] != 0) {
            switch (pumpRecv(ctrl, ctx, MSG_NONE, 50, rx, &remoteErr)) {
                case Pump::PeerClosed:  return loopBack("Client disconnected during ping burst");
                case Pump::RemoteError: return loopBack("Client error: " + remoteErr);
                case Pump::PeerError:   return loopBack("Socket error during ping burst");
                default: break;
            }
        }
        if (ctx.pingSentUs[seqSent % 8] == 0) pongs++;
    }
    if (!pongs) return loopBack("Peer not answering pings");
    PingStats st{ (uint64_t)ctx.lastRttMs * 1000, (uint32_t)pongs, 5 };
    ctrl.sendMsg(MSG_PING_STATS, &st, sizeof(st));
    info(con, "Control link: " + std::to_string(ctx.lastRttMs) + " ms ("
              + std::to_string(pongs)
              + "/5, diagnostic) - the gameplay route is measured before start.");

    int delay, rollback;
    ConfigProvenanceStage hostDelayOrigin = ConfigProvenanceStage::Default;
    ConfigProvenanceStage hostRollbackOrigin =
        ConfigProvenanceStage::Default;
    if (con) {
        delay    = s.delay;
        rollback = s.rollback;
        info(con, "Measuring the connection to " + clientDisplayName + "...");
    } else {
        delay    = flagDelay    >= 0 ? flagDelay    : s.delay;
        rollback = flagRollback >= 0 ? flagRollback : s.rollback;
        hostDelayOrigin = flagDelay >= 0
            ? ConfigProvenanceStage::LocallySelected : s.delayProvenance;
        hostRollbackOrigin = flagRollback >= 0
            ? ConfigProvenanceStage::LocallySelected : s.rollbackProvenance;
        info(con, "Unattended settings: delay=" + std::to_string(delay) +
                  (flagDelay >= 0 ? " (flag)" : " (ini default)") +
                  " rollback=" + std::to_string(rollback) +
                  (flagRollback >= 0 ? " (flag)" : " (ini default)"));
    }

    logConfigurationProvenance(
        "host",
        (hostDelayOrigin == ConfigProvenanceStage::Default
         && hostRollbackOrigin == ConfigProvenanceStage::Default)
            ? ConfigProvenanceStage::Default
            : ConfigProvenanceStage::LocallySelected,
        false, static_cast<unsigned>(delay),
        static_cast<unsigned>(rollback), hostDelayOrigin, hostRollbackOrigin,
        0u, 0u, ConfigProvenanceStage::Unknown,
        ConfigProvenanceStage::Unknown);

    if (delay < 0 || delay > qnet::kCapabilityDelay
        || rollback < 0 || rollback > qnet::kCapabilityRollback)
        return loopBack("Refusing out-of-range session settings");
    qnetplay::wire::SessionCookie playerCookie, spectatorCookie;
    if (!generateSessionCookie(playerCookie)
        || !generateSessionCookie(spectatorCookie)
        || spectatorCookie == playerCookie)
        return loopBack("BCryptGenRandom failed while creating session credentials");
    NetplayConfig np{};
    np.hostTuning = PeerTuning{
        static_cast<uint8_t>(delay),
        static_cast<uint8_t>(rollback),
        static_cast<uint8_t>(
            s.authMaxDelay >= delay ? s.authMaxDelay : delay),
        static_cast<uint8_t>(
            s.authMaxRollback >= rollback ? s.authMaxRollback : rollback)
    };
    np.hostPlayer = 1;
    np.sessionId = static_cast<uint32_t>(
        playerCookie.low ^ (playerCookie.low >> 32)
        ^ playerCookie.high ^ (playerCookie.high >> 32));
    if (np.sessionId == 0) np.sessionId = 1;
    np.compatibilityId = qipc::kRuntimeCompatibilityId;
    np.compatibilityIdentity = localCompatibility;
    np.playerCookieLow = playerCookie.low;
    np.playerCookieHigh = playerCookie.high;
    np.spectatorCookieLow = spectatorCookie.low;
    np.spectatorCookieHigh = spectatorCookie.high;
    if (effective.available && testSpectatorStartupFailureRequested()) {
        spectatorUdp.close();
        effective.available = false;
        effective.port = 0;
        effective.unavailableReason =
            "test forced spectator startup failure after source acquisition";
        QLOG("[spectator.sidecar] UNAVAILABLE: %s; optional spectator service "
             "disabled before game publication; player session continues",
             effective.unavailableReason.c_str());
    }
    std::string spectateAddress;
    std::string spectatorRouteError;
    qnet::SpectateAddress shareAddress{};
    bool shareAddressValid = false;
    if (effective.available) {
        std::string spectateIp = qclient::public_ip::value();
        uint16_t spectateControlPort = s.port;
        std::string advertisedIp;
        uint16_t advertisedPort = 0;
        if (!advertisedPlayerAddress.empty()
            && parseHostPort(advertisedPlayerAddress, advertisedIp,
                             advertisedPort, s.port)) {
            spectateIp = advertisedIp;
            spectateControlPort = advertisedPort;
        }
        Endpoint advertisedControl{};
        if (!spectateIp.empty()
            && Endpoint::parse(
                spectateIp, spectateControlPort, advertisedControl)) {
            spectateAddress = advertisedControl.str();
        }
        uint32_t localIpv4BE[16]{};
        const size_t localCount =
            collectLocalIpv4BE(localIpv4BE, sizeof(localIpv4BE)
                                            / sizeof(localIpv4BE[0]));
        std::string addressError;
        if (buildSpectateAddress(
                spectateIp, localIpv4BE, localCount, effective.port,
                np.sessionId, spectatorCookie.low, spectatorCookie.high,
                shareAddress, addressError)) {
            shareAddress.controlPort = spectateControlPort;
            if (effective.publicPortProven && effective.publicPort != 0
                && !spectateIp.empty()) {
                Endpoint publicSource{};
                const bool parsed = Endpoint::parse(
                    spectateIp, effective.publicPort, publicSource);
                const uint32_t publicBE =
                    parsed ? publicSource.sa.sin_addr.s_addr : 0u;
                const bool routable = parsed && publicBE != 0
                    && !qnet::isNonUnicastBE(publicBE)
                    && !qnet::isPrivateBE(publicBE)
                    && !qnet::isLoopbackBE(publicBE)
                    && !qnet::isLinkLocalBE(publicBE);
                if (!routable && parsed) {
                    QLOG("[spectator-access] mapped port %u not advertised: "
                         "%s is not a routable public address, so the LAN "
                         "candidates already cover it",
                         static_cast<unsigned>(effective.publicPort),
                         spectateIp.c_str());
                }
                if (routable
                    && shareAddress.endpointCount
                           < qnet::kMaxSpectateEndpoints) {
                    qnet::CandidateEndpoint mapped{};
                    mapped.kind = qnet::CandidateKind::PublicObserved;
                    mapped.ipv4BE = publicSource.sa.sin_addr.s_addr;
                    mapped.portBE = publicSource.sa.sin_port;
                    shareAddress.endpoints[shareAddress.endpointCount++] =
                        mapped;
                    QLOG("[spectator-access] advertising mapped public "
                         "endpoint %s (internal UDP %u)",
                         publicSource.str().c_str(),
                         static_cast<unsigned>(effective.port));
                }
            }
            if (relay != nullptr) {
                qnet::CandidateEndpoint relayEndpoint{};
                relayEndpoint.kind = qnet::CandidateKind::Relay;
                relayEndpoint.ipv4BE =
                    relay->pair.relayEndpoint.sa.sin_addr.s_addr;
                relayEndpoint.portBE =
                    relay->pair.relayEndpoint.sa.sin_port;
                if (shareAddress.endpointCount
                        >= qnet::kMaxSpectateEndpoints
                    || relayEndpoint.ipv4BE == 0
                    || relayEndpoint.portBE == 0) {
                    addressError =
                        "the player relay returned no usable spectator endpoint";
                } else {
                    shareAddress.endpoints[
                        shareAddress.endpointCount++] = relayEndpoint;
                    shareAddress.hasRelayRoom = true;
                    shareAddress.relayRoomLow = relay->pair.room.low;
                    shareAddress.relayRoomHigh = relay->pair.room.high;
                    if (effective.policy
                        == qclient::spectator::AdmissionPolicy::Public) {
                        shareAddress.hasRelayAccessToken = true;
                        shareAddress.relayAccessTokenLow =
                            relay->pair.viewerAccessToken.low;
                        shareAddress.relayAccessTokenHigh =
                            relay->pair.viewerAccessToken.high;
                    }
                }
            }
            if (qnet::validateCandidateSet(
                    shareAddress.endpoints,
                    shareAddress.endpointCount)
                != qnet::CandidateSetError::None) {
                addressError =
                    "the combined direct/relay spectator routes are invalid";
            } else {
                shareAddressValid = true;
            }
        }
        if (!shareAddressValid) {
            spectatorRouteError = addressError.empty()
                ? "source candidate construction failed" : addressError;
            QLOG("[spectator-access] source candidates unavailable: %s; "
                 "player-address discovery remains active",
                 spectatorRouteError.c_str());
        }
    }

    int drained = udp.drain();
    if (drained) QLOG("[host] drained %d stale UDP datagrams before rendezvous", drained);
    if (!ctrl.sendMsg(MSG_NETPLAY_CONFIG, &np, sizeof(np)))
        return loopBack("send failed (config), err=" + std::to_string(ctrl.lastError()));
    QLOG("[host] session descriptor sent (our delay=%u rollback=%u)",
         np.hostTuning.delay, np.hostTuning.rollback);
    logConfigurationProvenance(
        "host", ConfigProvenanceStage::CasterAdvertised,
        false, np.hostTuning.delay, np.hostTuning.rollback,
        hostDelayOrigin, hostRollbackOrigin, 0u, 0u,
        ConfigProvenanceStage::Unknown, ConfigProvenanceStage::Unknown);

    qnet::RouteCandidates myCandidates = buildCandidateAdvertisement(
        np.sessionId, udp.boundPort(), clientEp.sa.sin_addr.s_addr);
    if (!ctrl.sendMsg(MSG_ROUTE_CANDIDATES, &myCandidates,
                      sizeof(myCandidates)))
        return loopBack("send failed (route candidates), err="
                        + std::to_string(ctrl.lastError()));
    QLOG("[host] advertised %u direct candidate(s)", myCandidates.count);
    qnet::CandidateEndpoint clientCandidates[qnet::kMaxRouteCandidates] = {};
    uint8_t clientRouteIds[qnet::kMaxRouteCandidates] = {};
    size_t clientCandidateCount = 0;
    switch (pumpRecv(ctrl, ctx, MSG_ROUTE_CANDIDATES, 5000, rx, &remoteErr)) {
        case Pump::Got: {
            qnet::RouteCandidates theirs{};
            std::string candidateError;
            if (!bodyAs(rx, theirs)
                || !acceptCandidateAdvertisement(
                       theirs, np.sessionId, clientCandidates,
                       clientRouteIds, &clientCandidateCount,
                       candidateError)) {
                return loopBack("Client candidate advertisement rejected: "
                                + candidateError);
            }
            QLOG("[host] accepted %zu client candidate(s)",
                 clientCandidateCount);
            break;
        }
        case Pump::Timeout:
            return loopBack("No candidate advertisement from client within 5 s");
        case Pump::PeerClosed:
            return loopBack("Client disconnected during candidate exchange");
        case Pump::RemoteError:
            return loopBack("Client error: " + remoteErr);
        default:
            return loopBack("Socket error during candidate exchange, err="
                            + std::to_string(ctrl.lastError()));
    }
    RouteProbeService probeService;
    probeService.configure(np.sessionId,
                           {np.playerCookieLow, np.playerCookieHigh},
                           qnetplay::wire::PeerRole::Host);

    info(con, "Checking the connection to your opponent...");
    qnetplay::Endpoint lockedPeer;
    TuningProposal proposal{};
    std::string udpError;
    const bool rendezvousOk = relay
        ? hostRelayUdpRendezvous(ctrl, ctx, udp, *relay, np, lockedPeer,
                                 proposal, udpError, &probeService,
                                 clientCandidates, clientCandidateCount)
        : hostUdpRendezvous(ctrl, ctx, udp, clientEp, np, lockedPeer,
                            proposal, udpError, &probeService);
    if (!rendezvousOk)
        return loopBack(udpError);
    QLOG("[host] UDP peer locked to %08X:%u route=%s; peer-local client "
         "tuning proposal d%u/r%u; opening settings",
         lockedPeer.ipv4, lockedPeer.port,
         routeText(proposal.routeKind),
         proposal.clientTuning.delay, proposal.clientTuning.rollback);
    logConfigurationProvenance(
        "host", ConfigProvenanceStage::PeerAdvertised,
        false, static_cast<unsigned>(delay),
        static_cast<unsigned>(rollback), hostDelayOrigin, hostRollbackOrigin,
        proposal.clientTuning.delay, proposal.clientTuning.rollback,
        ConfigProvenanceStage::PeerAdvertised,
        ConfigProvenanceStage::PeerAdvertised);

    PeerTuning finalHostTuning = np.hostTuning;
    int hostRequiredOwn = -1;
    int hostRequiredPeer = -1;
    {
        const RouteRecommendation recommendation = computeRouteRecommendation(
            probeService, np.hostTuning, proposal.clientTuning);
        const int clientReported =
            proposal.requiredFrames != 0 ? proposal.requiredFrames : -1;
        if (recommendation.hasEvidence) {
            hostRequiredOwn = worseRequirement(
                recommendation.floorFrames, clientReported);
            hostRequiredPeer = hostRequiredOwn;
        } else {
            hostRequiredOwn = hostRequiredPeer = clientReported;
        }
        logRouteRecommendation("host", recommendation, np.hostTuning,
                               proposal.clientTuning);
        if (recommendation.hasEvidence) {
            logConfigurationProvenance(
                "host", ConfigProvenanceStage::ProvisionalRecommendation,
                false, recommendation.lowLatency.hostDelay,
                recommendation.lowLatency.hostMaxRollback,
                ConfigProvenanceStage::ProvisionalRecommendation,
                ConfigProvenanceStage::ProvisionalRecommendation,
                proposal.clientTuning.delay, proposal.clientTuning.rollback,
                ConfigProvenanceStage::PeerAdvertised,
                ConfigProvenanceStage::PeerAdvertised);
        }
        if (con && recommendation.hasEvidence) {
            int chosenDelay = finalHostTuning.delay;
            int chosenRollback = finalHostTuning.rollback;
            std::string settingsMessage;
            const RecommendationScreenResult settingsResult =
                recommendationScreen(
                    *con, ctrl, ctx, udp, probeService,
                    true, proposal.routeLabel,
                    recommendation, proposal.clientTuning,
                    proposal.requiredFrames != 0 ? proposal.requiredFrames
                                                 : -1,
                    chosenDelay, chosenRollback,
                    finalHostTuning.maxDelay,
                    finalHostTuning.maxRollback, settingsMessage);
            if (settingsResult != RecommendationScreenResult::Confirmed) {
                if (settingsResult
                    == RecommendationScreenResult::CancelledLocally) {
                    return loopBack("You left match settings");
                }
                return loopBack(settingsMessage);
            }
            finalHostTuning.delay = static_cast<uint8_t>(chosenDelay);
            finalHostTuning.rollback = static_cast<uint8_t>(chosenRollback);
            delay = chosenDelay;
            rollback = chosenRollback;
            hostDelayOrigin = ConfigProvenanceStage::LocallySelected;
            hostRollbackOrigin = ConfigProvenanceStage::LocallySelected;
            logConfigurationProvenance(
                "host", ConfigProvenanceStage::LocallySelected,
                false, finalHostTuning.delay,
                finalHostTuning.rollback, hostDelayOrigin, hostRollbackOrigin,
                proposal.clientTuning.delay, proposal.clientTuning.rollback,
                ConfigProvenanceStage::PeerAdvertised,
                ConfigProvenanceStage::PeerAdvertised);
        }
    }

    ConfirmConfig confirmed{};
    bool gotConfirmation = false;
    if (ctx.malformedConfirmed)
        return loopBack("Client sent a malformed final tuning confirmation");
    if (ctx.haveConfirmed) {
        confirmed = ctx.confirmed;
        ctx.haveConfirmed = false;
        gotConfirmation = true;
    }
    uint64_t settingsHeartbeatUs = 0;
    if (!gotConfirmation) {
        showSettingsWait(con, "Waiting for opponent to confirm settings...");
    }
    const uint64_t confirmationDeadlineMs = nowUs() / 1000 + 180000;
    while (!gotConfirmation
           && nowUs() / 1000 < confirmationDeadlineMs) {
        if (!svcPumpUdp(udp, probeService, udpError))
            return loopBack(udpError);
        switch (pumpRecv(ctrl, ctx, MSG_CONFIRM_CONFIG, 1, rx,
                         &remoteErr)) {
            case Pump::Got:
                if (!bodyAs(rx, confirmed))
                    return loopBack(
                        "Client sent a malformed final tuning confirmation");
                gotConfirmation = true;
                break;
            case Pump::Timeout:
                break;
            case Pump::PeerClosed:
                return loopBack(
                    "Client disconnected before confirming settings");
            case Pump::RemoteError:
                return loopBack("Client error: " + remoteErr);
            default:
                return loopBack(
                    "Control socket failed while confirming settings");
        }
        if (!gotConfirmation) {
            const SettingsHeartbeatResult heartbeat = serviceSettingsHeartbeat(
                ctrl, ctx, settingsHeartbeatUs, "host", "peer-confirm");
            if (heartbeat == SettingsHeartbeatResult::TimedOut) {
                return loopBack(
                    "Client stopped responding while confirming settings");
            }
            if (heartbeat == SettingsHeartbeatResult::SendFailed) {
                return loopBack(
                    "Control connection failed while confirming settings");
            }
        }
        udp.waitReadable(1);
    }
    if (!gotConfirmation)
        return loopBack("Client did not confirm session settings in time");
    if (!finalConfirmationMatchesProposal(confirmed, proposal, np))
        return loopBack(
            "Client changed its route or tuning limits after proposing them");

    if (hostRequiredOwn >= 0) {
        const int floored = qnet::enforcedOwnRollback(
            hostRequiredOwn, confirmed.clientTuning.delay,
            finalHostTuning.rollback, finalHostTuning.maxRollback);
        if (floored != finalHostTuning.rollback) {
            QLOG("[route-floor] role=host rollback %u -> %d (required=%d "
                 "clientDelay=%u authMax=%u): the committed pair could not "
                 "run this route at full speed",
                 finalHostTuning.rollback, floored,
                 hostRequiredOwn, confirmed.clientTuning.delay,
                 finalHostTuning.maxRollback);
            info(con, "Rollback raised to " + std::to_string(floored)
                      + " - the minimum this connection can keep up with.");
            finalHostTuning.rollback = static_cast<uint8_t>(floored);
            rollback = floored;
        }
    }
    if (hostRequiredPeer >= 0) {
        const int flooredDelay = qnet::enforcedOwnDelay(
            hostRequiredPeer, confirmed.clientTuning.maxRollback,
            finalHostTuning.delay, finalHostTuning.maxDelay);
        if (flooredDelay != finalHostTuning.delay) {
            QLOG("[route-floor] role=host delay %u -> %d (required=%d "
                 "clientAuthRollback=%u authMaxDelay=%u): the client's "
                 "rollback ceiling cannot cover this route on its own",
                 finalHostTuning.delay, flooredDelay, hostRequiredPeer,
                 confirmed.clientTuning.maxRollback,
                 finalHostTuning.maxDelay);
            info(con, "Input delay raised to " + std::to_string(flooredDelay)
                      + " - the minimum your opponent's connection can keep "
                        "up with.");
            finalHostTuning.delay = static_cast<uint8_t>(flooredDelay);
            delay = flooredDelay;
        }
    }

    RouteCommit commit{};
    commit.sessionId = np.sessionId;
    commit.routeKind = confirmed.routeKind;
    commit.routeLabel = confirmed.routeLabel;
    commit.routeId = confirmed.routeId;
    commit.hostTuning = finalHostTuning;
    commit.requiredFrames = requiredFramesWire(hostRequiredOwn);
    if (!ctrl.sendMsg(MSG_ROUTE_COMMIT, &commit, sizeof(commit)))
        return loopBack("send failed (route commit), err="
                        + std::to_string(ctrl.lastError()));
    QLOG("[host] route commit ACK sent session=%u route=%s label=%s",
         commit.sessionId, routeText(commit.routeKind),
         routeLabelText(commit.routeLabel));
    if (!rendezvousOnly) {
        hostMintAndSendClientRootOffer(ctrl, commit.sessionId);
    }
    logConfigurationProvenance(
        "host", ConfigProvenanceStage::SessionCommitted,
        true, commit.hostTuning.delay, commit.hostTuning.rollback,
        hostDelayOrigin, hostRollbackOrigin,
        confirmed.clientTuning.delay, confirmed.clientTuning.rollback,
        ConfigProvenanceStage::PeerAdvertised,
        ConfigProvenanceStage::PeerAdvertised);
    QLOG("[route-label] role=host session=%u kind=%s label=%s routeId=%u",
         commit.sessionId, routeText(commit.routeKind),
         routeLabelText(commit.routeLabel), commit.routeId);
    info(con, std::string("Connection ready (")
              + routeText(confirmed.routeKind) + ") - opponent uses delay "
              + std::to_string(confirmed.clientTuning.delay)
              + " | rollback "
              + std::to_string(confirmed.clientTuning.rollback)
              + ".");
    QLOG("[route-contract] v=1 role=host state=committed session=%u "
         "route=%s",
         commit.sessionId, routeText(commit.routeKind));
    if (!con) {
        std::printf("[route-contract] v=1 role=host state=committed "
                    "session=%u route=%s\n",
                    commit.sessionId, routeText(commit.routeKind));
        std::printf("UDP path validated (%s)\n",
                    routeText(confirmed.routeKind));
        std::fflush(stdout);
    }
    QLOG("[route-contract] UDP path validated (%s)",
         routeText(confirmed.routeKind));
    if (rendezvousOnly) {
        RouteCommit acknowledged{};
        switch (pumpRecv(ctrl, ctx, MSG_ROUTE_COMMIT, 5000,
                         rx, &remoteErr)) {
            case Pump::Got:
                if (!bodyAs(rx, acknowledged)
                    || acknowledged.sessionId != commit.sessionId
                    || acknowledged.routeKind != commit.routeKind
                    || acknowledged.routeLabel != commit.routeLabel
                    || acknowledged.routeId != commit.routeId
                    || acknowledged.requiredFrames != commit.requiredFrames) {
                    QLOG("[host] rendezvous-only route acknowledgement "
                         "was invalid");
                    ctrl.close();
                    return HostAttempt::Failed;
                }
                break;
            case Pump::RemoteError:
                QLOG("[host] rendezvous-only route acknowledgement "
                     "rejected: %s", remoteErr.c_str());
                ctrl.close();
                return HostAttempt::Failed;
            default:
                QLOG("[host] rendezvous-only route acknowledgement "
                     "timed out or disconnected");
                ctrl.close();
                return HostAttempt::Failed;
        }
        info(con, "Rendezvous-only diagnostic completed before process creation.");
        QLOG("[host] rendezvous-only complete session=%u route=%s",
             np.sessionId, routeText(confirmed.routeKind));
        ctrl.closeGraceful();
        return HostAttempt::Launched;
    }

    drained = udp.drain();
    if (drained) QLOG("[host] drained %d stale UDP datagrams before launch", drained);

    Settings s2 = s; s2.delay = delay; s2.rollback = rollback;
    s2.delayProvenance = hostDelayOrigin;
    s2.rollbackProvenance = hostRollbackOrigin;
    qipc::IpcConfig cfg = baseConfig(s2, qipc::MODE_NETPLAY, 1, 1);
    cfg.vsRow = qipc::kVsRow1v1; cfg.directBoot = 1; cfg.sessionId = np.sessionId;
    cfg.compatibilityId = np.compatibilityId;
    cfg.compatibilityIdentity = np.compatibilityIdentity;
    cfg.cookieLow = np.playerCookieLow;
    cfg.cookieHigh = np.playerCookieHigh;
    cfg.spectatorCookieLow = np.spectatorCookieLow;
    cfg.spectatorCookieHigh = np.spectatorCookieHigh;
    cfg.remoteDelay = confirmed.clientTuning.delay;
    cfg.remoteRollback = confirmed.clientTuning.rollback;
    setCommittedConfigurationProvenance(
        cfg, hostDelayOrigin, hostRollbackOrigin,
        ConfigProvenanceStage::PeerAdvertised,
        ConfigProvenanceStage::PeerAdvertised);
    qgame::storeCanonicalPlayerNameForCodePage(
        cfg.remoteName, clientName, theirInit.nameCodePage);
    cfg.remoteNameCodePage = theirInit.nameCodePage;
    cfg.serveSpectators = effective.available ? 1 : 0;
    cfg.spectatorServiceMode = effective.available
        ? qipc::SPECTATOR_SERVICE_SIDECAR
        : qipc::SPECTATOR_SERVICE_OFF;
    cfg.spectatorMaxViewers = effective.available
        ? static_cast<uint16_t>(s.spectatorMaxViewers)
        : 0;
    cfg.peerAddrBE = lockedPeer.ipv4;
    cfg.peerPortBE = htons(lockedPeer.port);
    probeService.exportPrelaunch(cfg, kCommittedRouteId);
    std::string launchError;
    SpectateInfo liveInfo{};
    liveInfo.state = effective.available ? SPECTATE_LIVE : SPECTATE_NO_MATCH;
    liveInfo.spectatorPort = effective.available ? effective.port : 0;
    liveInfo.sessionId = np.sessionId;
    liveInfo.keyLow = effective.available ? spectatorCookie.low : 0;
    liveInfo.keyHigh = effective.available ? spectatorCookie.high : 0;
    liveInfo.admissionPolicy =
        static_cast<uint8_t>(effective.policy);
    liveInfo.viewerCapacity = effective.available
        ? static_cast<uint16_t>(s.spectatorMaxViewers) : 0;
    liveInfo.simulationContract = np.compatibilityId;
    liveInfo.compatibilityIdentity = np.compatibilityIdentity;
    liveInfo.presence = makeLivePresence(
        s.nickname, clientDisplayName, myInit.roundsToWin, np.sessionId,
        LIVE_PRESENCE_CHARACTER_SELECT);
    liveInfo.presence.viewerCapacity = effective.available
        ? static_cast<uint16_t>(s.spectatorMaxViewers) : 0u;
    if (effective.available && relay != nullptr) {
        liveInfo.relayRoomLow = relay->pair.room.low;
        liveInfo.relayRoomHigh = relay->pair.room.high;
        if (effective.policy
            == qclient::spectator::AdmissionPolicy::Public) {
            liveInfo.relayViewerAccessLow =
                relay->pair.viewerAccessToken.low;
            liveInfo.relayViewerAccessHigh =
                relay->pair.viewerAccessToken.high;
        }
    }
    if (effective.available) {
        liveInfo.sourceRoutes.sessionId = np.sessionId;
        if (shareAddressValid) {
            liveInfo.sourceRoutes.count =
                static_cast<uint8_t>(shareAddress.endpointCount);
            for (size_t i = 0; i < shareAddress.endpointCount; ++i) {
                const qnet::CandidateEndpoint& source =
                    shareAddress.endpoints[i];
                qnet::RouteCandidateWire& destination =
                    liveInfo.sourceRoutes.entries[i];
                destination.kind = static_cast<uint8_t>(source.kind);
                destination.routeId = static_cast<uint8_t>(i + 1);
                destination.portBE = source.portBE;
                destination.ipv4BE = source.ipv4BE;
            }
        }
    }
    SidecarLaunch sidecar;
    sidecar.enabled = effective.available;
    const qnet::CandidateEndpoint* topologyRootQspCandidate =
        shareAddressValid
            ? preferredSpectatorSourceEndpoint(shareAddress) : nullptr;
    if (topologyRootQspCandidate != nullptr
        && topologyRootQspCandidate->kind == qnet::CandidateKind::Relay) {
        topologyRootQspCandidate = nullptr;
    }
    sidecar.config = sidecarConfigFor(s, effective, np, relay,
                                      topologyRootQspCandidate);
    sidecar.preboundSourceSocket = &spectatorUdp;
    sidecar.spectateAddress = spectateAddress;
    sidecar.unavailableReason = effective.unavailableReason;
    sidecar.sessionId = np.sessionId;
    sidecar.advertisedPort = effective.available ? effective.port : 0;
    sidecar.policy = effective.policy;
    if (relayControlPump) relayControlPump->stop();
    if (!launchOnlineSynchronized(
            s2, cfg, udp.raw(),
            INVALID_SOCKET,
            ctrl, ctx, np,
            confirmed.routeKind, true, relay != nullptr, con,
            hiddenDesktop, launchError, spectateAddress,
            liveListener, liveListener ? &liveInfo : nullptr,
            &sidecar)) {
        fail(con, "The game could not be started",launchError);
        return HostAttempt::Failed;
    }
    return HostAttempt::Launched;
}

int runHost(Settings& s, qui::Console* con, int flagDelay = -1,
             int flagRollback = -1, bool hiddenDesktop = false,
             const RelayOptions* relayOptions = nullptr,
             bool rendezvousOnly = false) {
    if (!preflightOnlineArtifacts(s, con, s.recordNetplay)) return 1;
    retireRetainedSpectatorService();
    QLOG("[host] starting on port %u (v%u relay=%d)", s.port,
         kProtocolVersion,
         relayOptions && relayOptions->enabled ? 1 : 0);
    if (!netInit()) return fail(con, "Netplay could not start", networkUnavailableMessage());

    UdpSock udp;
    if (!udp.open(0))
        return fail(con, "Cannot host",
                    "Could not open a UDP socket for match traffic.",
                    udp.lastError());
    UdpSock spectatorUdp;
    SpectatorServing serving = prepareSpectatorEndpoint(s, spectatorUdp, con);
    qnet::portmap::PortMapper spectatorPortMap;
    startSpectatorPortMapping(spectatorPortMap, serving);
    bool spectatorPortMapReported = false;

    if (relayOptions && relayOptions->enabled) {
        std::shared_ptr<HostControlEndpoint> control;
        if (serving.available) {
            control = std::make_shared<HostControlEndpoint>();
            std::string controlError;
            if (!control->open(s.port, controlError)) {
                QLOG("[host] spectator control endpoint unavailable on relay "
                     "host: %s; player relay continues",
                     controlError.c_str());
                info(con, "Spectators are unavailable this session: "
                          + controlError
                          + " The relayed player match is unaffected.");
                serving.available = false;
                serving.unavailableReason =
                    "the spectator admission endpoint could not bind";
                spectatorUdp.close();
                control.reset();
            } else {
                std::string bridgeWarning;
                if (!control->openSidecarControlIngress(
                        spectatorUdp, bridgeWarning)) {
                    QLOG("[host-control] sidecar ingress unavailable: %s",
                         bridgeWarning.c_str());
                }
                if (!control->punchReady) {
                    info(con, "Spectator hole punching is unavailable this "
                              "session (" + control->punchStatus
                              + "); direct/relay viewing remains available.");
                }
            }
        }
        const std::string spectatorText = serving.available
            ? std::to_string(serving.port)
            : std::string("unavailable");
        const std::string room =
            qnet::relay::formatRoomId(relayOptions->room);
        info(con, "Relay server: " + relayOptions->server.str());
        info(con, "Relay room: " + room);
        info(con, "Share: qoh99caster join --relay "
                  + relayOptions->server.str() + " --room " + room);
        info(con, std::string("Player route: ")
                  + (relayOptions->forceRelay
                     ? "forced relay"
                     : "punched direct first, relay fallback after 1500 ms"));
        info(con, "Local player UDP " + std::to_string(s.port)
                  + "; isolated direct spectator UDP " + spectatorText);

        TcpConn ctrlConn;
        TcpControlLink ctrl(ctrlConn);
        RelayRuntime relay{};
        std::string relayError;
        if (!establishRelayPair(ctrlConn, *relayOptions,
                                qnet::relay::RelayRole::Host,
                                relay, con, relayError, control.get())) {
            return fail(con, "Relay room failed", relayError,
                        ctrl.lastError());
        }
        info(con, "Relay control pair established; negotiating...");
        refreshSpectatorPortMapping(spectatorPortMap, serving);
        const HostAttempt result = hostNegotiate(
            s, con, ctrl, relayOptions->server, udp, spectatorUdp, serving,
            flagDelay, flagRollback, hiddenDesktop, &relay,
            rendezvousOnly, control);
        return result == HostAttempt::Launched ? 0
             : result == HostAttempt::Quit ? 0 : 1;
    }

    const std::string spectatorText = serving.available
        ? std::to_string(serving.port)
        : std::string("unavailable");
    auto control = std::make_shared<HostControlEndpoint>();
    std::string controlError;
    if (!control->open(s.port, controlError))
        return fail(con, "Cannot host on port " + std::to_string(s.port),
                    controlError, control->socket.lastError());
    if (serving.available) {
        std::string bridgeWarning;
        if (!control->openSidecarControlIngress(
                spectatorUdp, bridgeWarning)) {
            QLOG("[host-control] sidecar ingress unavailable: %s",
                 bridgeWarning.c_str());
        }
    }
    if (con) con->clear();
    info(con, "Hosting players on UDP port " + std::to_string(s.port)
              + ", spectators on dedicated UDP port " + spectatorText
              + " - waiting for a client... (Esc cancels)");
    if (!control->punchReady)
        info(con, "Hole punching is unavailable this session ("
                  + control->punchStatus
                  + "); players will need a reachable port.");

    for (;;) {
        qnet::UdpControlLink* link = nullptr;
        uint64_t waitStart = nowUs();
        while (link == nullptr) {
            control->service(200);
            link = control->acceptOne();
            if (link != nullptr) break;
            reportSpectatorPortMapping(spectatorPortMap,
                                       spectatorPortMapReported, con,
                                       serving);
            if (con) {
                con->statusLine("Waiting for a client... " +
                                std::to_string((nowUs() - waitStart) / 1000000ull) + " s (Esc cancels)");
                if (con->pollKey(0) == VK_ESCAPE) { QLOG("[host] cancelled by user"); return 0; }
            } else if (escPressedFocused()) {
                QLOG("[host] cancelled by user (Esc, console focused)");
                return 0;
            }
        }
        const Endpoint clientEp = link->peer();
        info(con, "Client connected from " + clientEp.str() + "; negotiating...");
        refreshSpectatorPortMapping(spectatorPortMap, serving);
        const HostAttempt attempt =
            hostNegotiate(s, con, *link, clientEp, udp, spectatorUdp,
                          serving, flagDelay, flagRollback, hiddenDesktop,
                          nullptr, rendezvousOnly, control);
        switch (attempt) {
            case HostAttempt::Launched: return 0;
            case HostAttempt::Quit:     return 0;
            case HostAttempt::LoopBack:
                control->release(link);
                continue;
            case HostAttempt::Failed:   return 1;
        }
    }
}

int runAutomaticHost(Settings& s, qui::Console* con, int flagDelay = -1,
                      int flagRollback = -1,
                      bool hiddenDesktop = false,
                      bool forceRelay = false,
                      bool rendezvousOnly = false) {
    if (!preflightOnlineArtifacts(s, con, s.recordNetplay)) return 1;
    retireRetainedSpectatorService();
    QLOG("[host-auto] starting port=%u autopunchRelay=%s:%u",
         s.port, qnet::autopunch::kRelayHost,
         static_cast<unsigned>(qnet::autopunch::kRelayPort));
    if (!netInit()) return fail(con, "Netplay could not start", networkUnavailableMessage());
    qclient::public_ip::refresh();

    UdpSock udp;
    if (!udp.open(0))
        return fail(con, "Cannot host",
                    "Could not open a UDP socket for match traffic.",
                    udp.lastError());
    UdpSock spectatorUdp;
    SpectatorServing serving = prepareSpectatorEndpoint(s, spectatorUdp, con);
    qnet::portmap::PortMapper spectatorPortMap;
    startSpectatorPortMapping(spectatorPortMap, serving);
    bool spectatorPortMapReported = false;

    auto control = std::make_shared<HostControlEndpoint>();
    std::string controlError;
    const bool controlAvailable = control->open(s.port, controlError);
    const bool directListener = !forceRelay && controlAvailable;
    if (!controlAvailable) {
        QLOG("[host-auto] control endpoint unavailable (%s); relay "
             "registration remains eligible",
             controlError.c_str());
        info(con, "Could not open the control port: " + controlError
                  + " Trying automatic relay hosting...");
        if (serving.available) {
            serving.available = false;
            serving.unavailableReason =
                "the spectator admission endpoint could not bind";
            spectatorUdp.close();
            QLOG("[host-auto] spectator service withdrawn because its "
                 "control endpoint is unavailable");
        }
        control.reset();
    } else {
        if (serving.available) {
            std::string bridgeWarning;
            if (!control->openSidecarControlIngress(
                    spectatorUdp, bridgeWarning)) {
                QLOG("[host-control] sidecar ingress unavailable: %s",
                     bridgeWarning.c_str());
            }
        }
        if (forceRelay) {
            QLOG("[host-auto] player direct selection disabled by diagnostic "
                 "override; spectator control endpoint remains active");
            info(con, "Diagnostic forced-relay mode; direct player hosting is "
                      "disabled, spectator admission remains available.");
        } else if (!control->punchReady) {
            info(con, "Hole punching is unavailable this session ("
                      + control->punchStatus
                      + "); players will need a reachable port.");
        }
    }
    const std::string spectatorText = serving.available
        ? std::to_string(serving.port)
        : std::string("unavailable");

    Endpoint relayServer;
    std::string relaySettingError;
    const bool relayConfigured =
        configuredRelayServer(s, relayServer, relaySettingError);
    if (!relayConfigured) {
        QLOG("[host-auto] %s", relaySettingError.c_str());
        info(con, relaySettingError + "; direct hosting remains active.");
        if (!directListener)
            return fail(con, "No usable hosting route",
                        "The control port could not be opened and the relay "
                        "setting is invalid.");
    }

    RelayOptions relayOptions{};
    qnet::relay::RelayPairClient pair;
    TcpConn relayCtrlConn;
    TcpControlLink relayCtrl(relayCtrlConn);
    bool roomReady = false;
    bool pairBegun = false;
    bool lookupFailureReported = false;
    bool observedDiscoveryTried = false;
    uint64_t nextRelayAttemptUs = 0;
    uint64_t waitStartUs = nowUs();
    std::string advertisedText;
    std::string copyNotice;
    uint64_t copyNoticeUntilUs = 0;

    if (con) con->clear();
    info(con, "Hosting on UDP " + std::to_string(s.port)
              + " with direct-first automatic relay fallback; spectator UDP "
              + spectatorText + ".");
    info(con, "Detecting your public IPv4 address...");

    for (;;) {
        if (advertisedText.empty() && !lookupFailureReported
            && !qclient::public_ip::pending()) {
            const std::string publicIp = qclient::public_ip::value();
            Endpoint advertised;
            if (publicIp != "n/a" && !publicIp.empty()
                && Endpoint::parse(publicIp, s.port, advertised)) {
                advertisedText = advertised.str();
                info(con, "Share: " + advertisedText);
                info(con, "Press C to copy ip:port; Esc cancels hosting.");
                QLOG("[host-auto] advertised=%s",
                     advertisedText.c_str());
                if (relayConfigured
                    && automaticRelayOptions(
                        s, advertised, relayOptions, relaySettingError)) {
                    roomReady = true;
                    relayOptions.forceRelay = forceRelay;
                    QLOG("[host-auto] relay=%s room=%s",
                         relayOptions.server.str().c_str(),
                         qnet::relay::formatRoomId(
                             relayOptions.room).c_str());
                } else if (relayConfigured) {
                    QLOG("[host-auto] relay alias unavailable: %s",
                         relaySettingError.c_str());
                    info(con, relaySettingError
                              + "; direct hosting remains active.");
                }
            } else {
                lookupFailureReported = true;
                const std::string why = "Public IPv4 lookup failed";
                QLOG("[host-auto] %s; HTTP discovery is advisory only",
                     why.c_str());
                info(con, why + "; asking the relay for the observed address.");
                if (relayConfigured && !observedDiscoveryTried) {
                    observedDiscoveryTried = true;
                    Endpoint observedHost{};
                    std::string observeError;
                    if (discoverObservedHostEndpoint(
                            s, relayServer, observedHost, observeError)) {
                        Endpoint advertised{};
                        advertised.sa.sin_addr.s_addr =
                            observedHost.sa.sin_addr.s_addr;
                        advertised.sa.sin_port = htons(s.port);
                        advertisedText = advertised.str();
                        info(con, "Share: " + advertisedText
                                  + "  (observed by the relay)");
                        info(con,
                             "Press C to copy ip:port; Esc cancels hosting.");
                        QLOG("[host-auto] advertised=%s source=relay-observed",
                             advertisedText.c_str());
                        if (automaticRelayOptions(s, advertised, relayOptions,
                                                  relaySettingError)) {
                            roomReady = true;
                            relayOptions.forceRelay = forceRelay;
                            QLOG("[host-auto] relay=%s room=%s "
                                 "source=relay-observed",
                                 relayOptions.server.str().c_str(),
                                 qnet::relay::formatRoomId(
                                     relayOptions.room).c_str());
                        }
                    } else {
                        QLOG("[host-auto] relay-observed discovery failed: %s",
                             observeError.c_str());
                        info(con, "The relay could not report an observed "
                                  "address: " + observeError);
                    }
                }
                if (!directListener && !roomReady)
                    return fail(con, "No usable hosting route",
                                "The control port could not be opened, the "
                                "relay could not report this host's observed "
                                "public address, and public IPv4 discovery is "
                                "unavailable.");
                if (!directListener)
                    info(con, "Direct hosting unavailable; relay route active.");
                else
                    info(con, "Direct hosting remains active.");
            }
        }

        const uint64_t now = nowUs();
        if (roomReady && relayConfigured && !pairBegun
            && now >= nextRelayAttemptUs) {
            relayCtrl.close();
            pair = qnet::relay::RelayPairClient{};
            std::string beginError;
            if (pair.begin(relayCtrlConn, relayOptions.server,
                           relayOptions.room,
                           qnet::relay::LaneDomain::Player,
                           qnet::relay::RelayRole::Host,
                           1500, &beginError)) {
                pairBegun = true;
                QLOG("[host-auto] relay room registered/waiting");
            } else {
                nextRelayAttemptUs = now + 5000000ull;
                QLOG("[host-auto] relay registration failed: %s; retry in "
                     "5 s", beginError.c_str());
            }
        }

        if (pairBegun) {
            std::string pairError;
            const qnet::relay::PairPoll result =
                pair.poll(relayCtrlConn, 0, &pairError);
            if (result == qnet::relay::PairPoll::Ready) {
                RelayRuntime runtime{};
                runtime.pair = pair.result();
                runtime.forceRelay = relayOptions.forceRelay;
                info(con, "Direct connection was unavailable; relay pair "
                          "found. Trying hole punch, then relay...");
                QLOG("[host-auto] selected relayed control room=%s",
                     qnet::relay::formatRoomId(
                         relayOptions.room).c_str());
                refreshSpectatorPortMapping(spectatorPortMap, serving);
                const HostAttempt attempt = hostNegotiate(
                    s, con, relayCtrl, relayOptions.server, udp,
                    spectatorUdp, serving, flagDelay, flagRollback,
                    hiddenDesktop, &runtime, rendezvousOnly, control,
                    advertisedText);
                if (attempt == HostAttempt::Launched
                    || attempt == HostAttempt::Quit)
                    return 0;
                if (attempt == HostAttempt::Failed) return 1;
                pairBegun = false;
                nextRelayAttemptUs = nowUs() + 250000ull;
                waitStartUs = nowUs();
                continue;
            }
            if (result == qnet::relay::PairPoll::Rejected
                || result == qnet::relay::PairPoll::Closed
                || result == qnet::relay::PairPoll::Error) {
                QLOG("[host-auto] relay wait ended: %s; retry in 5 s",
                     pairError.c_str());
                relayCtrl.close();
                pairBegun = false;
                nextRelayAttemptUs = nowUs() + 5000000ull;
            }
        }

        qnet::UdpControlLink* directLink = nullptr;
        Endpoint clientEp;
        if (controlAvailable) {
            control->service(50);
            if (directListener)
                directLink = control->listener.acceptOne();
            if (directLink != nullptr) clientEp = directLink->peer();
            if (!directListener) {
                while (qnet::UdpControlLink* skipped =
                           control->listener.acceptOne()) {
                    skipped->close();
                    control->listener.release(skipped);
                }
            }
        } else {
            Sleep(50);
        }
        if (directLink != nullptr) {
            if (pairBegun) {
                QLOG("[host-auto] direct control won before relay acceptance; "
                     "cancelling waiting relay room");
                relayCtrl.close();
                pairBegun = false;
            }
            info(con, "Direct client connected from " + clientEp.str()
                      + "; negotiating...");
            QLOG("[host-auto] selected direct control candidate=%s",
                 clientEp.str().c_str());
            refreshSpectatorPortMapping(spectatorPortMap, serving);
            const HostAttempt result = hostNegotiate(
                s, con, *directLink, clientEp, udp, spectatorUdp, serving,
                flagDelay, flagRollback, hiddenDesktop, nullptr,
                rendezvousOnly, control, advertisedText);
            if (result == HostAttempt::Launched
                || result == HostAttempt::Quit)
                return 0;
            if (result == HostAttempt::Failed) return 1;
            control->listener.release(directLink);
            nextRelayAttemptUs = 0;
            waitStartUs = nowUs();
            continue;
        }

        if (con) {
            std::vector<qui::Row> rows;
            screenHeader(rows, "Hosting a game");
            if (!advertisedText.empty()) {
                rows.push_back({ { "  Share this address with your opponent:",
                                   qui::Tone::Default } });
                rows.push_back({ { "     ", qui::Tone::Default },
                                 { advertisedText, qui::Tone::Accent },
                                 { "     [C] copy", qui::Tone::Dim } });
            } else if (!lookupFailureReported) {
                rows.push_back({ { "  Detecting your public address...",
                                   qui::Tone::Dim } });
                rows.push_back({ { "  (players on your network can already "
                                   "join via your local address, port "
                                   + std::to_string(s.port) + ")",
                                   qui::Tone::Dim } });
            } else {
                rows.push_back({ { "  Public address lookup failed - share "
                                   "your IP and port "
                                   + std::to_string(s.port) + " manually.",
                                   qui::Tone::Warn } });
            }
            rows.push_back({});
            rows.push_back({ { "  Waiting for an opponent... ",
                               qui::Tone::Default },
                             { std::to_string(
                                   (nowUs() - waitStartUs) / 1000000ull) + " s",
                               qui::Tone::Accent } });
            std::string routes = directListener
                ? "direct connection ready"
                : "direct connection unavailable";
            if (roomReady && relayConfigured)
                routes += pairBegun ? " | relay fallback ready"
                                    : " | relay fallback retrying";
            rows.push_back({ { "  " + routes, qui::Tone::Dim } });
            if (!copyNotice.empty() && nowUs() < copyNoticeUntilUs)
                rows.push_back({ { "  " + copyNotice, qui::Tone::Good } });
            rows.push_back({});
            rows.push_back({ { advertisedText.empty()
                                   ? std::string("  Esc cancel")
                                   : std::string("  [C] copy address | Esc cancel"),
                               qui::Tone::Dim } });
            con->draw(rows);
            const int key = con->pollKey(0);
            if (key == VK_ESCAPE) {
                QLOG("[host-auto] cancelled by user");
                return 0;
            }
            if (key == 'C' && !advertisedText.empty()) {
                const bool copied = con->copyText(advertisedText);
                QLOG("[host-auto] copy advertised endpoint ok=%d",
                     copied ? 1 : 0);
                copyNotice = copied ? "Copied " + advertisedText
                                    : "Could not open the clipboard";
                copyNoticeUntilUs = nowUs() + 2000000ull;
            }
        } else if (escPressedFocused()) {
            QLOG("[host-auto] cancelled by user (Esc, console focused)");
            return 0;
        }
        reportSpectatorPortMapping(spectatorPortMap,
                                   spectatorPortMapReported, con, serving);
    }
}

enum class SpectatorRouteForce : uint8_t {
    Auto = 0,
    Direct,
    Punch,
    Relay,
};

const char* spectatorRouteForceName(SpectatorRouteForce mode) {
    switch (mode) {
    case SpectatorRouteForce::Auto:   return "auto";
    case SpectatorRouteForce::Direct: return "direct";
    case SpectatorRouteForce::Punch:  return "punch";
    case SpectatorRouteForce::Relay:  return "relay";
    }
    return "invalid";
}

int runSpectator(const Settings& s, uint32_t sessionId,
                  uint64_t spectatorCookieLow, uint64_t spectatorCookieHigh,
                  qui::Console* con, bool hiddenDesktop,
                  uint16_t hostControlPort = 0,
                  SpectatorRouteForce routeForce =
                      SpectatorRouteForce::Auto,
                  bool topologyOptIn = false);

int runJoin(Settings& s, qui::Console* con, bool hiddenDesktop = false,
             const RelayOptions* relayOptions = nullptr,
             const RelayOptions* automaticFallback = nullptr,
             bool rendezvousOnly = false) {
    if (!preflightOnlineArtifacts(s, con, s.recordNetplay)) return 1;
    QLOG("[join] connecting to %s:%u (v%u relay=%d)",
         s.joinIp.c_str(), s.joinPort, kProtocolVersion,
         relayOptions && relayOptions->enabled ? 1 : 0);
    if (!netInit()) return fail(con, "Netplay could not start", networkUnavailableMessage());

    UdpSock udp;
    if (!udp.open(0))
        return fail(con, "Netplay could not start",
                    "The caster could not open the network connection it "
                    "needs to send game data.", udp.lastError());

    TcpConn ctrlConn;
    TcpControlLink relayCtrl(ctrlConn);
    UdpSock joinControl;
    qnet::UdpControlLink udpCtrl;
    ControlLink* controlLink = nullptr;
    RelayRuntime relay{};
    if (relayOptions && relayOptions->enabled) {
        info(con, "Connecting outward to relay "
                  + relayOptions->server.str() + " room "
                  + qnet::relay::formatRoomId(relayOptions->room) + " ...");
        const uint64_t retryDeadlineUs = nowUs() + 12000000ull;
        bool waitingReported = false;
        for (;;) {
            std::string relayError;
            if (establishRelayPair(ctrlConn, *relayOptions,
                                   qnet::relay::RelayRole::Client,
                                   relay, con, relayError))
                break;
            const bool hostNotRegistered =
                relayOptions->automaticAlias
                && relayError.find("room not found") != std::string::npos
                && nowUs() < retryDeadlineUs;
            if (!hostNotRegistered)
                return fail(con, "Relay room failed", relayError,
                            relayCtrl.lastError());
            if (!waitingReported) {
                info(con, "Waiting for the host's automatic relay "
                          "registration...");
                waitingReported = true;
            }
            relayCtrl.close();
            Sleep(250);
        }
        info(con, "Relay control pair established. Negotiating...");
        controlLink = &relayCtrl;
    } else {
        info(con, "Reaching " + s.joinIp + ":"
                  + std::to_string(s.joinPort) + " ...");
        Endpoint advertised{};
        if (!Endpoint::parse(s.joinIp, s.joinPort, advertised))
            return fail(con, "Cannot reach " + s.joinIp + ":"
                        + std::to_string(s.joinPort),
                        "That is not a usable address.");
        std::string dialError;
        if (!joinControl.open(0))
            return fail(con, "Could not join",
                        "The caster could not open the network connection it "
                        "needs to reach the host.", joinControl.lastError());
        constexpr int kDialWithFallbackMs = 15000;
        constexpr int kDialOnlyRouteMs = 20000;
        const int dialBudgetMs =
            (automaticFallback && automaticFallback->enabled)
                ? kDialWithFallbackMs : kDialOnlyRouteMs;
        if (!dialHostControl(joinControl, advertised, udpCtrl, con,
                             dialBudgetMs,
                             dialError)) {
            const int e = 0;
            std::string why = dialError;
            if (automaticFallback && automaticFallback->enabled) {
                QLOG("[join] direct control dial failed err=%d (%s); "
                     "automatic relay "
                     "fallback room=%s",
                     e, why.c_str(),
                     qnet::relay::formatRoomId(
                         automaticFallback->room).c_str());
                QLOG("[route-fallback-contract] v=1 role=client "
                     "from=direct to=automatic-relay stage=tcp-control "
                     "reason=connect-failed error=%d room=%s",
                     e, qnet::relay::formatRoomId(
                         automaticFallback->room).c_str());
                info(con, "Direct connection failed; switching to "
                          "AutoPunch/relay automatically...");
                udp.close();
                return runJoin(s, con, hiddenDesktop, automaticFallback,
                               nullptr, rendezvousOnly);
            }
            return fail(con, "Cannot reach " + s.joinIp + ":"
                        + std::to_string(s.joinPort), why, e);
        }
        controlLink = &udpCtrl;
        info(con, "Connected. Negotiating...");
    }
    ControlLink& ctrl = *controlLink;

    qnetplay::compat::CompatibilityIdentity localCompatibility{};
    std::string localCompatibilityError;
    if (!localCompatibilityIdentityFor(
            s, localCompatibility, localCompatibilityError)) {
        return fail(con, "Game files check failed", localCompatibilityError);
    }
    RuntimeArtifactIdentityWire localRuntimeArtifacts{};
    std::string localRuntimeArtifactsError;
    if (!localRuntimeArtifactIdentityFor(
            s, localRuntimeArtifacts, localRuntimeArtifactsError)) {
        QLOG("[netplay] local runtime artifact identity failed: %s",
             localRuntimeArtifactsError.c_str());
        return fail(con, "Build check failed",
                    "Could not verify this installation's netplay build. "
                    "Restart it or reinstall the current build.");
    }

    PumpCtx ctx; ctx.con = con;
    ctx.lastHeardUs = nowUs();
    TcpConn::Received rx;
    std::string remoteErr;

    const auto retryAutomaticRelay =
        [&](const char* const stage, const std::string& reason) -> bool {
            if (!automaticFallback || !automaticFallback->enabled)
                return false;
            QLOG("[join] direct control failed before %s (%s); automatic "
                 "relay fallback room=%s",
                 stage, reason.c_str(),
                 qnet::relay::formatRoomId(automaticFallback->room).c_str());
            QLOG("[route-fallback-contract] v=1 role=client "
                 "from=direct to=automatic-relay stage=%s "
                 "reason=pre-session-control room=%s",
                 stage,
                 qnet::relay::formatRoomId(automaticFallback->room).c_str());
            info(con, "Direct connection failed; switching to "
                      "AutoPunch/relay automatically...");
            ctrl.closeGraceful();
            udp.close();
            return true;
        };

    switch (pumpRecv(ctrl, ctx, MSG_VERSION_CONFIG, 5000, rx, &remoteErr)) {
        case Pump::Got: break;
        case Pump::Timeout:
            if (retryAutomaticRelay("version", "no version from host"))
                return runJoin(s, con, hiddenDesktop, automaticFallback,
                               nullptr, rendezvousOnly);
            return fail(con, "Couldn't connect", "No version from host within 5 s");
        case Pump::PeerClosed:
            if (retryAutomaticRelay("version", "host disconnected"))
                return runJoin(s, con, hiddenDesktop, automaticFallback,
                               nullptr, rendezvousOnly);
            return fail(con, "Couldn't connect", "Host disconnected during version exchange");
        case Pump::RemoteError: return fail(con, "Host error", remoteErr);
        default:
            if (retryAutomaticRelay("version", "control socket error"))
                return runJoin(s, con, hiddenDesktop, automaticFallback,
                               nullptr, rendezvousOnly);
            return fail(con, "Couldn't connect",
                        "The connection to the host failed.", ctrl.lastError());
    }
    VersionConfig hostVer{};
    if (!bodyAs(rx, hostVer)) return fail(con, "Couldn't connect", "Malformed version from host");
    VersionConfig myVer{ kProtocolVersion };
    ctrl.sendMsg(MSG_VERSION_CONFIG, &myVer, sizeof(myVer));
    {
        ClientIntent joinIntent{ INTENT_JOIN };
        ctrl.sendMsg(MSG_CLIENT_INTENT, &joinIntent, sizeof(joinIntent));
    }
    if (hostVer.protocolVersion != kProtocolVersion) {
        std::string text = "Protocol mismatch: host v" + std::to_string(hostVer.protocolVersion)
                         + ", we are v" + std::to_string(kProtocolVersion) + " - update the older side.";
        if (pumpRecv(ctrl, ctx, MSG_ERROR_TEXT, 1500, rx, &remoteErr) == Pump::Got)
            text = errorTextOf(rx);
        else if (!remoteErr.empty()) text = remoteErr;
        return fail(con, "Version mismatch", text);
    }

    const auto offerLiveSpectate = [&](bool& handled) -> int {
        handled = false;
        if (!ctx.haveSpectateInfo || ctx.spectateInfo.state != SPECTATE_LIVE)
            return 0;
        handled = true;
        if (ctx.spectateInfo.keyLow == 0
            && ctx.spectateInfo.keyHigh == 0) {
            return fail(
                con, "Match in progress",
                "That host is not accepting address-based spectators.");
        }
        if (con != nullptr
            && con->menu("They're already in a match",
                         { "Watch it as a spectator" },
                         "Back") == 0) {
            Settings viewer = s;
            viewer.joinPort = ctx.spectateInfo.spectatorPort;
            return runSpectator(viewer,
                                ctx.spectateInfo.sessionId,
                                ctx.spectateInfo.keyLow,
                                ctx.spectateInfo.keyHigh, con,
                                hiddenDesktop, s.joinPort);
        }
        return fail(con, "Match in progress",
                    "That player is already in a match.");
    };

    switch (pumpRecv(ctrl, ctx, MSG_INITIAL_CONFIG, 5000, rx, &remoteErr)) {
        case Pump::Got: break;
        case Pump::Timeout: {
            bool handled = false;
            const int result = offerLiveSpectate(handled);
            if (handled) return result;
            return fail(con, "Couldn't connect", "No initial config from host within 5 s");
        }
        case Pump::PeerClosed: {
            bool handled = false;
            const int result = offerLiveSpectate(handled);
            if (handled) return result;
            return fail(con, "Couldn't connect", "Host disconnected during initial exchange");
        }
        case Pump::RemoteError: return fail(con, "Host error", remoteErr);
        default:                return fail(con, "Couldn't connect",
                        "The connection to the host failed.", ctrl.lastError());
    }
    InitialConfig hostInit{};
    if (!bodyAs(rx, hostInit)) return fail(con, "Couldn't connect", "Malformed initial config from host");
    if (hostInit.isHost != 1 || hostInit.dataPort == 0
        || hostInit.roundsToWin
            > qnetplay::kMaxRoundsToWinMinusOne + 1u
        || hostInit.reserved[0] != 0 || hostInit.reserved[1] != 0
        || hostInit.reserved[2] != 0)
        return fail(con, "Couldn't connect",
                    "The host answered, but not in a way the caster "
                    "understood. Check that you and the host are both running "
                    "the same version of the netplay build.");
    const qnetplay::compat::IdentityMismatch hostInitialMismatch =
        qnetplay::compat::compatibilityMismatch(
            localCompatibility, hostInit.compatibilityIdentity);
    if (hostInitialMismatch != qnetplay::compat::IdentityMismatch::None) {
        QLOG("[netplay] host compatibility rejected: %s",
             qnetplay::compat::identityMismatchReason(hostInitialMismatch));
        const std::string why = std::string("This match cannot start: ")
            + qnetplay::compat::identityMismatchPlayerText(
                hostInitialMismatch)
            + ".";
        ErrorText et{};
        std::snprintf(et.text, sizeof(et.text), "%s", why.c_str());
        (void)ctrl.sendMsg(MSG_ERROR_TEXT, &et, sizeof(et));
        return fail(con, "Couldn't connect", why);
    }
    const RuntimeArtifactMismatch hostArtifactMismatch =
        runtimeArtifactMismatch(localRuntimeArtifacts,
                                hostInit.runtimeArtifacts);
    if (hostArtifactMismatch != RuntimeArtifactMismatch::None) {
        QLOG("[netplay] host build rejected: %s",
             runtimeArtifactMismatchReason(hostArtifactMismatch));
        const std::string playerText = buildMismatchPlayerText();
        const bool sent = sendControlError(ctrl, playerText);
        QLOG("[netplay] host build rejection sent=%d", sent ? 1 : 0);
        ctrl.closeGraceful();
        return fail(con, "Build mismatch", playerText);
    }
    hostInit.name[sizeof(hostInit.name) - 1] = '\0';
    if (!qgame::isCanonicalPlayerNameForCodePage(
            hostInit.name, hostInit.nameCodePage)) {
        return fail(
            con, "Couldn't connect",
            "Invalid host nickname/code-page encoding");
    }
    std::string hostName = hostInit.name;
    const std::string hostDisplayName =
        qgame::playerNameForUtf8Display(
            hostName, hostInit.nameCodePage);
    InitialConfig myInit{};
    qgame::storeCanonicalPlayerNameForCodePage(
        myInit.name, s.nickname, CP_UTF8);
    myInit.nameCodePage = CP_UTF8;
    myInit.isHost = 0; myInit.dataPort = udp.boundPort();
    myInit.roundsToWin = configuredRoundsToWin(s);
    myInit.compatibilityIdentity = localCompatibility;
    myInit.runtimeArtifacts = localRuntimeArtifacts;
    if (!ctrl.sendMsg(MSG_INITIAL_CONFIG, &myInit, sizeof(myInit)))
        return fail(con, "Couldn't connect", "send failed (initial)", ctrl.lastError());
    info(con, "Joined " + hostDisplayName + " ("
              + firstToRoundsText(hostInit.roundsToWin) + ").");
    announceOpponentJoined(con, s);

    const uint64_t capUs = 180000000ull;
    const uint64_t startUs = nowUs();
    uint64_t lastPingUs = 0, lastPendingUs = 0;
    int lobbySel = 0;
    int localDelay = qnet::clampInt(s.delay, 0, qnet::kCapabilityDelay);
    int localRollback =
        qnet::clampInt(s.rollback, 0, qnet::kCapabilityRollback);
    ConfigProvenanceStage clientDelayOrigin = s.delayProvenance;
    ConfigProvenanceStage clientRollbackOrigin = s.rollbackProvenance;
    logConfigurationProvenance(
        "client",
        (clientDelayOrigin == ConfigProvenanceStage::Default
         && clientRollbackOrigin == ConfigProvenanceStage::Default)
            ? ConfigProvenanceStage::Default
            : ConfigProvenanceStage::LocallySelected,
        false, static_cast<unsigned>(localDelay),
        static_cast<unsigned>(localRollback), clientDelayOrigin,
        clientRollbackOrigin, 0u, 0u, ConfigProvenanceStage::Unknown,
        ConfigProvenanceStage::Unknown);
    NetplayConfig np{};
    for (;;) {
        uint64_t now = nowUs();
        if (now - startUs > capUs) {
            ErrorText et{}; std::snprintf(et.text, sizeof(et.text),
                "Client timed out waiting for settings (180 s)");
            bool sent = ctrl.sendMsg(MSG_ERROR_TEXT, &et, sizeof(et));
            QLOG("[join] settings wait cap expired (error-text sent=%d)", (int)sent);
            ctrl.closeGraceful();
            return fail(con, "Timed out", "The host did not start the game within 180 s.");
        }
        if (ctx.lastHeardUs && now - ctx.lastHeardUs > 15000000ull) {
            return fail(con, "Host not responding",
                        "Nothing heard from the host for 15 s (their app hung or the connection dropped).");
        }
        if (now - lastPingUs >= 2000000ull) { sendPing(ctrl, ctx); lastPingUs = now; }
        if (now - lastPendingUs >= 500000ull) {
            SettingsPending sp{ (uint8_t)localDelay, (uint8_t)localRollback,
                                (uint16_t)(ctx.haveRtt ? ctx.lastRttMs : 0) };
            ctrl.sendMsg(MSG_SETTINGS_PENDING, &sp, sizeof(sp));
            lastPendingUs = now;
        }

        Pump p = pumpRecv(ctrl, ctx, MSG_NETPLAY_CONFIG, con ? 10 : 250, rx, &remoteErr);
        if (p == Pump::Got) {
            if (!bodyAs(rx, np)) return fail(con, "Couldn't connect", "Malformed NetplayConfig from host");
            break;
        }
        if (p == Pump::RemoteError) return fail(con, "The host ended the session", remoteErr);
        if (p == Pump::PeerClosed)  return fail(con, "Connection lost", "The host disconnected before starting the game.");
        if (p == Pump::PeerError)   return fail(con, "Connection lost", "Network error while waiting for the host.", ctrl.lastError());

        if (con) {
            qui::ScreenSpec spec;
            screenHeader(spec.above, "Game lobby");
            spec.above.push_back({ { "  Host: ", qui::Tone::Default },
                                   { hostDisplayName, qui::Tone::Accent } });
            spec.above.push_back(
                { { "  Match: "
                        + firstToRoundsText(hostInit.roundsToWin),
                    qui::Tone::Default } });
            spec.above.push_back(pingRow(ctx));
            spec.above.push_back({});
            spec.above.push_back({ { "  The host measures the connection "
                                     "and starts the game.",
                                     qui::Tone::Dim } });
            spec.above.push_back({ { "  During the match, '-' and '=' "
                                     "lower/raise your own input delay.",
                                     qui::Tone::Dim } });
            spec.below.push_back({});
            spec.below.push_back({ { "  Waiting "
                                     + std::to_string((now - startUs) / 1000000ull)
                                     + " s / 180 s - Esc = leave",
                                     qui::Tone::Dim } });
            switch (con->menuTick(spec, lobbySel, 40)) {
                case qui::kTimeout: break;
                case qui::kBack: {
                    ErrorText et{}; std::snprintf(et.text, sizeof(et.text), "Client cancelled");
                    bool sent = ctrl.sendMsg(MSG_ERROR_TEXT, &et, sizeof(et));
                    QLOG("[join] cancelled by user (error-text sent=%d)", (int)sent);
                    ctrl.closeGraceful();
                    return fail(con, "Left the lobby", "You left while waiting for the host.", 0, {}, false);
                }
                default: break;
            }
        } else {
            std::string line = "Host " + hostDisplayName
                             + " is starting the game...  control link: "
                + (ctx.haveRtt ? std::to_string(ctx.lastRttMs) + " ms" : "?")
                + (ctx.havePending ? "   host: delay=" + std::to_string(ctx.pending.delay)
                                     + " rollback=" + std::to_string(ctx.pending.rollback)
                                     + "   yours: delay=" + std::to_string(localDelay)
                                     + " rollback=" + std::to_string(localRollback) : "")
                + "   [" + std::to_string((now - startUs) / 1000000ull) + "/180 s, Esc cancels]";
            static uint64_t lastPrintUs = 0;
            if (now - lastPrintUs > 5000000ull) { std::printf("%s\n", line.c_str()); lastPrintUs = now; }
            if (escPressedFocused()) {
                ErrorText et{}; std::snprintf(et.text, sizeof(et.text), "Client cancelled");
                ctrl.sendMsg(MSG_ERROR_TEXT, &et, sizeof(et));
                ctrl.closeGraceful();
                return fail(con, "Cancelled", "You cancelled while waiting for the host.", 0, {}, false);
            }
        }
    }

    const qnetplay::compat::IdentityMismatch netplayMismatch =
        qnetplay::compat::compatibilityMismatch(
            localCompatibility, np.compatibilityIdentity);
    if (netplayMismatch != qnetplay::compat::IdentityMismatch::None) {
        QLOG("[netplay] host compatibility rejected at handoff: %s",
             qnetplay::compat::identityMismatchReason(netplayMismatch));
        const std::string why = std::string("This match cannot start: ")
            + qnetplay::compat::identityMismatchPlayerText(netplayMismatch)
            + ".";
        ErrorText et{};
        std::snprintf(et.text, sizeof(et.text), "%s", why.c_str());
        (void)ctrl.sendMsg(MSG_ERROR_TEXT, &et, sizeof(et));
        return fail(con, "Couldn't connect", why);
    }

    if (np.hostTuning.delay > qnet::kCapabilityDelay
        || np.hostTuning.rollback > qnet::kCapabilityRollback ||
        np.hostTuning.maxDelay < np.hostTuning.delay ||
        np.hostTuning.maxRollback < np.hostTuning.rollback ||
        (np.hostPlayer != 1 && np.hostPlayer != 2) ||
        np.compatibilityId != qipc::kRuntimeCompatibilityId ||
        (np.playerCookieLow == 0 && np.playerCookieHigh == 0) ||
        (np.spectatorCookieLow == 0 && np.spectatorCookieHigh == 0) ||
        (np.playerCookieLow == np.spectatorCookieLow
         && np.playerCookieHigh == np.spectatorCookieHigh) ||
        np.sessionId == 0) {
        ErrorText et{};
        std::snprintf(et.text, sizeof(et.text),
                      "Host sent invalid/incompatible netplay settings");
        ctrl.sendMsg(MSG_ERROR_TEXT, &et, sizeof(et));
        return fail(con, "Couldn't connect", et.text);
    }
    logConfigurationProvenance(
        "client", ConfigProvenanceStage::CasterAdvertised,
        false, static_cast<unsigned>(localDelay),
        static_cast<unsigned>(localRollback), clientDelayOrigin,
        clientRollbackOrigin, np.hostTuning.delay, np.hostTuning.rollback,
        ConfigProvenanceStage::CasterAdvertised,
        ConfigProvenanceStage::CasterAdvertised);

    qnet::CandidateEndpoint hostCandidates[qnet::kMaxRouteCandidates] = {};
    uint8_t hostRouteIds[qnet::kMaxRouteCandidates] = {};
    size_t hostCandidateCount = 0;
    uint32_t reflectedOwnPublicBE = 0;
    switch (pumpRecv(ctrl, ctx, MSG_ROUTE_CANDIDATES, 5000, rx, &remoteErr)) {
        case Pump::Got: {
            qnet::RouteCandidates theirs{};
            std::string candidateError;
            if (!bodyAs(rx, theirs)
                || !acceptCandidateAdvertisement(
                       theirs, np.sessionId, hostCandidates, hostRouteIds,
                       &hostCandidateCount, candidateError)) {
                return fail(con, "Couldn't connect",
                            "Host candidate advertisement rejected: "
                                + candidateError);
            }
            reflectedOwnPublicBE = theirs.observedPublicIpv4BE;
            QLOG("[join] accepted %zu host candidate(s), reflected own "
                 "public=%08X", hostCandidateCount, reflectedOwnPublicBE);
            break;
        }
        case Pump::Timeout:
            return fail(con, "Couldn't connect",
                        "No candidate advertisement from host within 5 s");
        case Pump::PeerClosed:
            return fail(con, "Connection lost",
                        "The host disconnected during candidate exchange.");
        case Pump::RemoteError:
            return fail(con, "Host error", remoteErr);
        default:
            return fail(con, "Connection lost",
                        "Network error during candidate exchange.",
                        ctrl.lastError());
    }
    {
        uint32_t hostAddrForReflection = 0;
        Endpoint typedHost;
        if (Endpoint::parse(s.joinIp, hostInit.dataPort, typedHost))
            hostAddrForReflection = typedHost.sa.sin_addr.s_addr;
        qnet::RouteCandidates mine = buildCandidateAdvertisement(
            np.sessionId, udp.boundPort(), hostAddrForReflection);
        if (!ctrl.sendMsg(MSG_ROUTE_CANDIDATES, &mine, sizeof(mine)))
            return fail(con, "Couldn't connect",
                        "send failed (route candidates)", ctrl.lastError());
        QLOG("[join] advertised %u direct candidate(s)", mine.count);
    }
    RouteProbeService probeService;
    probeService.configure(np.sessionId,
                           {np.playerCookieLow, np.playerCookieHigh},
                           qnetplay::wire::PeerRole::Client);

    int drained = udp.drain();
    if (drained) QLOG("[join] drained %d stale UDP datagrams before rendezvous", drained);

    qnetplay::Endpoint lockedPeer;
    uint8_t routeKind = ROUTE_DIRECT;
    uint8_t committedLabel = 0;
    uint8_t committedRouteId = kCommittedRouteId;
    std::string udpError;
    bool rendezvousOk = false;
    if (relayOptions && relayOptions->enabled) {
        info(con, "Setting up the game connection (direct punch + relay probes)...");
        rendezvousOk = clientRelayUdpRendezvous(
            udp, relay, np, lockedPeer, routeKind, udpError, &probeService,
            hostCandidates, hostRouteIds, hostCandidateCount,
            reflectedOwnPublicBE, &committedLabel, &committedRouteId);
    } else {
        Endpoint peer;
        if (!Endpoint::parse(s.joinIp, hostInit.dataPort, peer))
            return fail(con, "Couldn't connect",
                        "Host UDP endpoint is not a valid IPv4 address");
        info(con, "Setting up the game connection...");
        if (hostCandidateCount > 0) {
            qnet::CandidateEndpoint raceSet[qnet::kMaxRouteCandidates + 1];
            uint8_t raceIds[qnet::kMaxRouteCandidates + 1];
            size_t raceCount = 0;
            qnet::CandidateEndpoint typedCandidate{};
            typedCandidate.kind =
                qnet::isAdvertisablePrivateBE(peer.sa.sin_addr.s_addr)
                    ? qnet::CandidateKind::PrivateLan
                    : qnet::CandidateKind::PublicObserved;
            typedCandidate.ipv4BE = peer.sa.sin_addr.s_addr;
            typedCandidate.portBE = peer.sa.sin_port;
            bool typedAlreadyAdvertised = false;
            for (size_t i = 0; i < hostCandidateCount; ++i) {
                if (qnet::sameEndpoint(hostCandidates[i], typedCandidate)) {
                    typedAlreadyAdvertised = true;
                    break;
                }
            }
            if (!typedAlreadyAdvertised) {
                raceSet[raceCount] = typedCandidate;
                raceIds[raceCount] = 255;
                ++raceCount;
            }
            for (size_t i = 0; i < hostCandidateCount; ++i) {
                raceSet[raceCount] = hostCandidates[i];
                raceIds[raceCount] = hostRouteIds[i];
                ++raceCount;
            }
            RaceOutcome outcome;
            if (raceDirectCandidates(udp, probeService, raceSet, raceIds,
                                     raceCount, reflectedOwnPublicBE,
                                     typedCandidate.kind
                                             == qnet::CandidateKind::
                                                 PublicObserved
                                         ? typedCandidate.ipv4BE
                                         : 0,
                                     outcome, udpError)) {
                peer = outcome.winner;
                committedLabel = outcome.label;
                committedRouteId = outcome.routeId;
            } else {
                AutopunchOutcome punched;
                std::string punchError;
                if (autopunchFallbackPass(udp, probeService, udp.boundPort(),
                                          typedCandidate.ipv4BE,
                                          typedCandidate.portBE, punched,
                                          punchError)
                    && punched.discovered) {
                    qnet::CandidateEndpoint punchedSet[1] = { punched.candidate };
                    const uint8_t punchedIds[1] = { 254 };
                    RaceOutcome punchedOutcome;
                    if (raceDirectCandidates(udp, probeService, punchedSet,
                                             punchedIds, 1,
                                             reflectedOwnPublicBE,
                                             punched.candidate.ipv4BE,
                                             punchedOutcome, udpError)) {
                        peer = punchedOutcome.winner;
                        committedLabel = punchedOutcome.label;
                        committedRouteId = punchedOutcome.routeId;
                        info(con, "Connected through hole punching.");
                    }
                }
            }
            probeService.clearTargets();
        }
        rendezvousOk = clientUdpRendezvous(udp, peer, np, lockedPeer,
                                           udpError, &probeService,
                                           committedLabel);
    }
    if (!rendezvousOk) {
        ErrorText et{};
        std::snprintf(et.text, sizeof(et.text), "%.119s", udpError.c_str());
        ctrl.sendMsg(MSG_ERROR_TEXT, &et, sizeof(et));
        ctrl.closeGraceful();
        if (!relayOptions && automaticFallback
            && automaticFallback->enabled) {
            QLOG("[join] direct UDP failed (%s); automatic relay fallback "
                 "room=%s",
                 udpError.c_str(),
                 qnet::relay::formatRoomId(
                     automaticFallback->room).c_str());
            QLOG("[route-fallback-contract] v=1 role=client "
                 "from=direct to=automatic-relay stage=udp-rendezvous "
                 "reason=validation-failed error=0 room=%s",
                 qnet::relay::formatRoomId(
                     automaticFallback->room).c_str());
            info(con, "Direct UDP path failed; switching to "
                      "AutoPunch/relay automatically...");
            udp.close();
            return runJoin(s, con, hiddenDesktop, automaticFallback,
                           nullptr, rendezvousOnly);
        }
        return fail(con, "Couldn't establish the game connection", udpError);
    }
    if (localDelay < 0 || localDelay > qnet::kCapabilityDelay
        || localRollback < 0 || localRollback > qnet::kCapabilityRollback)
        return fail(con, "Invalid local settings",
                    "Input delay must be 0-"
                        + std::to_string(qnet::kCapabilityDelay)
                        + " and rollback 0-"
                        + std::to_string(qnet::kCapabilityRollback) + ".");

    const PeerTuning provisionalClientTuning{
        static_cast<uint8_t>(localDelay), static_cast<uint8_t>(localRollback),
        static_cast<uint8_t>(
            s.authMaxDelay >= localDelay ? s.authMaxDelay : localDelay),
        static_cast<uint8_t>(
            s.authMaxRollback >= localRollback ? s.authMaxRollback
                                               : localRollback)};
    const RouteRecommendation measured = computeRouteRecommendation(
        probeService, np.hostTuning, provisionalClientTuning);
    TuningProposal proposal{};
    proposal.sessionId = np.sessionId;
    proposal.routeKind = routeKind;
    proposal.routeLabel = committedLabel != 0
        ? committedLabel
        : (routeKind == ROUTE_RELAY
               ? static_cast<uint8_t>(qnet::RouteLabel::Relay)
               : directLabelForPeer(lockedPeer.ipv4,
                                    false));
    proposal.routeId = committedRouteId;
    proposal.clientTuning = provisionalClientTuning;
    proposal.requiredFrames = measured.hasEvidence
        ? requiredFramesWire(measured.floorFrames)
        : 0u;
    if (!ctrl.sendMsg(MSG_TUNING_PROPOSAL, &proposal, sizeof(proposal)))
        return fail(con, "Couldn't connect",
                    "send failed (tuning proposal)", ctrl.lastError());

    int clientRequiredOwn = -1;
    {
        const PeerTuning& provisionalClient = proposal.clientTuning;
        const RouteRecommendation& recommendation = measured;
        if (recommendation.hasEvidence)
            clientRequiredOwn = recommendation.floorFrames;
        logRouteRecommendation("client", recommendation, np.hostTuning,
                               provisionalClient);
        if (recommendation.hasEvidence) {
            logConfigurationProvenance(
                "client", ConfigProvenanceStage::ProvisionalRecommendation,
                false, recommendation.lowLatency.clientDelay,
                recommendation.lowLatency.clientMaxRollback,
                ConfigProvenanceStage::ProvisionalRecommendation,
                ConfigProvenanceStage::ProvisionalRecommendation,
                np.hostTuning.delay, np.hostTuning.rollback,
                ConfigProvenanceStage::CasterAdvertised,
                ConfigProvenanceStage::CasterAdvertised);
        }
        if (con && recommendation.hasEvidence) {
            int chosenDelay = localDelay;
            int chosenRollback = localRollback;
            std::string settingsMessage;
            const RecommendationScreenResult settingsResult =
                recommendationScreen(
                    *con, ctrl, ctx, udp, probeService,
                    false, proposal.routeLabel,
                    recommendation, np.hostTuning,
                    -1,
                    chosenDelay, chosenRollback,
                    provisionalClient.maxDelay,
                    provisionalClient.maxRollback, settingsMessage);
            if (settingsResult != RecommendationScreenResult::Confirmed) {
                if (settingsResult
                    == RecommendationScreenResult::CancelledLocally) {
                    return fail(con, "Match setup cancelled",
                                "You left during match settings.", 0, {}, false);
                }
                return fail(con, "Opponent left", settingsMessage, 0, {}, false);
            }
            localDelay = chosenDelay;
            localRollback = chosenRollback;
            clientDelayOrigin = ConfigProvenanceStage::LocallySelected;
            clientRollbackOrigin = ConfigProvenanceStage::LocallySelected;
            logConfigurationProvenance(
                "client", ConfigProvenanceStage::LocallySelected,
                false, static_cast<unsigned>(chosenDelay),
                static_cast<unsigned>(chosenRollback), clientDelayOrigin,
                clientRollbackOrigin, np.hostTuning.delay,
                np.hostTuning.rollback,
                ConfigProvenanceStage::CasterAdvertised,
                ConfigProvenanceStage::CasterAdvertised);
        }
    }

    ConfirmConfig confirmed{};
    confirmed.sessionId = np.sessionId;
    confirmed.routeKind = proposal.routeKind;
    confirmed.routeLabel = proposal.routeLabel;
    confirmed.routeId = proposal.routeId;
    confirmed.clientTuning = PeerTuning{
        static_cast<uint8_t>(localDelay),
        static_cast<uint8_t>(localRollback),
        proposal.clientTuning.maxDelay,
        proposal.clientTuning.maxRollback
    };
    if (!ctrl.sendMsg(MSG_CONFIRM_CONFIG, &confirmed, sizeof(confirmed)))
        return fail(con, "Couldn't connect", "send failed (confirm)", ctrl.lastError());
    RouteCommit commit{};
    {
        showSettingsWait(con,
                         "Waiting for opponent to confirm settings...");
        bool gotCommit = false;
        uint64_t settingsHeartbeatUs = 0;
        const uint64_t commitDeadlineMs = nowUs() / 1000 + 180000;
        while (!gotCommit && nowUs() / 1000 < commitDeadlineMs) {
            if (!svcPumpUdp(udp, probeService, udpError))
                return fail(con, "Couldn't connect", udpError);
            switch (pumpRecv(ctrl, ctx, MSG_ROUTE_COMMIT, 1, rx,
                             &remoteErr)) {
                case Pump::Got:
                    if (!bodyAs(rx, commit)
                        || commit.sessionId != np.sessionId
                        || commit.routeKind != routeKind
                        || commit.routeLabel != confirmed.routeLabel
                        || commit.routeId != confirmed.routeId
                        || commit.hostTuning.delay > qnet::kCapabilityDelay
                        || commit.hostTuning.rollback
                               > qnet::kCapabilityRollback
                        || commit.hostTuning.maxDelay
                            < commit.hostTuning.delay
                        || commit.hostTuning.maxRollback
                            < commit.hostTuning.rollback) {
                        return fail(con, "Couldn't connect",
                                    "Host sent an invalid route-commit ACK");
                    }
                    gotCommit = true;
                    break;
                case Pump::Timeout:
                    break;
                case Pump::PeerClosed:
                    return fail(con, "Couldn't connect",
                                "Host disconnected before route commitment");
                case Pump::RemoteError:
                    return fail(con, "Host error", remoteErr);
                default:
                    return fail(con, "Couldn't connect",
                                "Socket error waiting for route commitment",
                                ctrl.lastError());
            }
            if (!gotCommit) {
                const SettingsHeartbeatResult heartbeat =
                    serviceSettingsHeartbeat(ctrl, ctx, settingsHeartbeatUs,
                                             "client", "route-commit");
                if (heartbeat == SettingsHeartbeatResult::TimedOut) {
                    return fail(
                        con, "Opponent stopped responding",
                        "Nothing was heard from the host for 10 seconds while "
                        "confirming settings.");
                }
                if (heartbeat == SettingsHeartbeatResult::SendFailed) {
                    return fail(con, "Connection lost",
                                "The control connection failed while waiting "
                                "for the host to confirm settings.",
                                ctrl.lastError());
                }
            }
        }
        if (!gotCommit) {
            return fail(con, "Couldn't connect",
                        "Host did not acknowledge the committed UDP route "
                        "in time");
        }
    }
    QLOG("[join] host route commit ACK session=%u route=%s label=%s",
         commit.sessionId, routeText(commit.routeKind),
         routeLabelText(commit.routeLabel));
    const int clientRequiredCommit = worseRequirement(
        clientRequiredOwn,
        commit.requiredFrames != 0 ? commit.requiredFrames : -1);
    if (clientRequiredCommit >= 0) {
        const int floored = qnet::enforcedOwnRollback(
            clientRequiredCommit, commit.hostTuning.delay,
            confirmed.clientTuning.rollback,
            confirmed.clientTuning.maxRollback);
        if (floored != confirmed.clientTuning.rollback) {
            QLOG("[route-floor] role=client rollback %u -> %d (required=%d "
                 "own=%d host=%u hostDelay=%u authMax=%u): the committed pair "
                 "could not run this route at full speed",
                 confirmed.clientTuning.rollback, floored,
                 clientRequiredCommit, clientRequiredOwn,
                 static_cast<unsigned>(commit.requiredFrames),
                 commit.hostTuning.delay,
                 confirmed.clientTuning.maxRollback);
            info(con, "Rollback raised to " + std::to_string(floored)
                      + " - the minimum this connection can keep up with.");
            confirmed.clientTuning.rollback = static_cast<uint8_t>(floored);
            localRollback = floored;
        }
    }
    logConfigurationProvenance(
        "client", ConfigProvenanceStage::SessionCommitted,
        true, confirmed.clientTuning.delay,
        confirmed.clientTuning.rollback, clientDelayOrigin,
        clientRollbackOrigin, commit.hostTuning.delay,
        commit.hostTuning.rollback, ConfigProvenanceStage::CasterAdvertised,
        ConfigProvenanceStage::CasterAdvertised);
    QLOG("[route-label] role=client session=%u kind=%s label=%s routeId=%u",
         commit.sessionId, routeText(commit.routeKind),
         routeLabelText(commit.routeLabel), commit.routeId);
    QLOG("[join] session host d%u/r%u, local d%u/r%u hostPlayer=%u; "
         "UDP peer=%08X:%u route=%s validated; confirmed; launching",
         commit.hostTuning.delay, commit.hostTuning.rollback,
         confirmed.clientTuning.delay, confirmed.clientTuning.rollback,
         np.hostPlayer, lockedPeer.ipv4, lockedPeer.port,
         routeText(confirmed.routeKind));
    info(con, std::string("Connection ready (")
              + routeText(confirmed.routeKind) + ") - your delay "
              + std::to_string(confirmed.clientTuning.delay)
              + " | rollback "
              + std::to_string(confirmed.clientTuning.rollback)
              + "; host delay " + std::to_string(commit.hostTuning.delay)
              + " | rollback " + std::to_string(commit.hostTuning.rollback)
              + ".");
    QLOG("[route-contract] v=1 role=client state=committed session=%u "
         "route=%s",
         commit.sessionId, routeText(commit.routeKind));
    if (!con) {
        std::printf("[route-contract] v=1 role=client state=committed "
                    "session=%u route=%s\n",
                    commit.sessionId, routeText(commit.routeKind));
        std::printf("Session route=%s\n",
                    routeText(confirmed.routeKind));
        std::fflush(stdout);
    }
    QLOG("[route-contract] Session route=%s",
         routeText(confirmed.routeKind));
    if (rendezvousOnly) {
        if (!ctrl.sendMsg(MSG_ROUTE_COMMIT, &commit, sizeof(commit))) {
            return fail(con, "Couldn't connect",
                        "Could not acknowledge the committed route",
                        ctrl.lastError());
        }
        info(con, "Rendezvous-only diagnostic completed before process creation.");
        QLOG("[join] rendezvous-only complete session=%u route=%s",
             np.sessionId, routeText(confirmed.routeKind));
        ctrl.closeGraceful();
        return 0;
    }
    {
        g_clientRootOfferRetained.valid = false;
        TcpConn::Received offerRx;
        std::string offerErr;
        if (pumpRecv(ctrl, ctx, MSG_CLIENT_ROOT_OFFER, 3000, offerRx,
                     &offerErr) == Pump::Got) {
            qnet::ClientRootOffer offer{};
            if (bodyAs(offerRx, offer) && offer.schemaVersion == 1u
                && offer.sessionId == np.sessionId) {
                g_clientRootOfferRetained.offer = offer;
                g_clientRootOfferRetained.valid = true;
                QLOG("[client-root] launch offer retained for session %u",
                     offer.sessionId);
            }
            std::memset(&offer, 0, sizeof(offer));
        } else {
            QLOG("[client-root] no launch offer received; the optional "
                 "client root stays disabled");
        }
    }

    drained = udp.drain();
    if (drained) QLOG("[join] drained %d stale UDP datagrams before launch", drained);

    Settings s2 = s;
    s2.delay = localDelay; s2.rollback = localRollback;
    s2.delayProvenance = clientDelayOrigin;
    s2.rollbackProvenance = clientRollbackOrigin;
    qipc::IpcConfig cfg = baseConfig(s2, qipc::MODE_NETPLAY, 0,
                                     (np.hostPlayer == 1 ? 2 : 1));
    cfg.vsRow = qipc::kVsRow1v1; cfg.directBoot = 1; cfg.sessionId = np.sessionId;
    cfg.compatibilityId = np.compatibilityId;
    cfg.compatibilityIdentity = np.compatibilityIdentity;
    cfg.cookieLow = np.playerCookieLow;
    cfg.cookieHigh = np.playerCookieHigh;
    cfg.spectatorCookieLow = np.spectatorCookieLow;
    cfg.spectatorCookieHigh = np.spectatorCookieHigh;
    cfg.remoteDelay = commit.hostTuning.delay;
    cfg.remoteRollback = commit.hostTuning.rollback;
    setCommittedConfigurationProvenance(
        cfg, clientDelayOrigin, clientRollbackOrigin,
        ConfigProvenanceStage::CasterAdvertised,
        ConfigProvenanceStage::CasterAdvertised);
    qgame::storeCanonicalPlayerNameForCodePage(
        cfg.remoteName, hostName, hostInit.nameCodePage);
    cfg.remoteNameCodePage = hostInit.nameCodePage;
    disableClientPrivatePublisher(cfg);
    cfg.peerAddrBE = lockedPeer.ipv4;
    cfg.peerPortBE = htons(lockedPeer.port);
    probeService.exportPrelaunch(cfg, kCommittedRouteId);

    UdpSock clientPrivateSource;
    ClientPrivateSidecarLaunch clientPrivateSidecar;
    const ClientPrivatePublisherPlan clientPrivatePlan =
        planClientPrivatePublisher(s2);
    if (clientPrivatePlan.enabled) {
        std::string clientPrivateError;
        if (!prepareClientPrivateSidecarLaunch(
                cfg, clientPrivatePlan, clientPrivateSource,
                clientPrivateSidecar, clientPrivateError)) {
            QLOG("[spectator.client-private] optional collector disabled "
                 "before launch: %s; player match continues",
                 clientPrivateError.empty()
                     ? "preparation failed"
                     : clientPrivateError.c_str());
        }
    }
    std::string launchError;
    if (!launchOnlineSynchronized(
            s2, cfg, udp.raw(), INVALID_SOCKET, ctrl, ctx, np,
            confirmed.routeKind, false,
            relayOptions && relayOptions->enabled, con, hiddenDesktop,
            launchError, "", {}, nullptr, nullptr,
            clientPrivateSidecar.enabled ? &clientPrivateSidecar : nullptr)) {
        return fail(con, "The game could not be started",launchError);
    }
    return 0;
}

int runAutomaticJoin(Settings& s, qui::Console* con,
                      bool hiddenDesktop = false,
                      bool forceRelay = false,
                      bool rendezvousOnly = false) {
    Endpoint advertisedHost;
    RelayOptions fallback{};
    std::string relayError;
    if (!Endpoint::parse(s.joinIp, s.joinPort, advertisedHost)
        || !automaticRelayOptions(
            s, advertisedHost, fallback, relayError)) {
        info(con, relayError + "; trying direct only.");
        return runJoin(s, con, hiddenDesktop, nullptr, nullptr,
                       rendezvousOnly);
    }
    QLOG("[join-auto] target=%s relay=%s room=%s",
         advertisedHost.str().c_str(), fallback.server.str().c_str(),
         qnet::relay::formatRoomId(fallback.room).c_str());
    if (forceRelay) {
        fallback.forceRelay = true;
        info(con, "Diagnostic forced-relay mode; skipping the direct "
                  "control attempt.");
        return runJoin(s, con, hiddenDesktop, &fallback, nullptr,
                       rendezvousOnly);
    }
    info(con, "Trying direct first; AutoPunch/relay fallback is automatic.");
    return runJoin(s, con, hiddenDesktop, nullptr, &fallback,
                   rendezvousOnly);
}

bool queryHostSpectateInfo(const std::string& ip, uint16_t port,
                           SpectateInfo& out, std::string& error,
                           ClientIntentKind requestIntent =
                               INTENT_SPECTATE) {
    QLOG("[spectate-discovery] stage=start target=%s:%u timeoutMs=8000",
         ip.c_str(), static_cast<unsigned>(port));
    if (!netInit()) {
        error = networkUnavailableMessage();
        QLOG("[spectate-discovery] stage=network-init result=failed "
             "reason='%s'", error.c_str());
        return false;
    }
    UdpSock probeSocket;
    if (!probeSocket.open(0)) {
        error = "could not open a socket to ask the host";
        QLOG("[spectate-discovery] stage=socket-open result=failed error=%d",
             probeSocket.lastError());
        return false;
    }
    Endpoint advertised{};
    if (!Endpoint::parse(ip, port, advertised)) {
        error = ip + ":" + std::to_string(port) + " is not a usable address";
        QLOG("[spectate-discovery] stage=address result=invalid target=%s:%u",
             ip.c_str(), static_cast<unsigned>(port));
        return false;
    }
    qnet::UdpControlLink probeLink;
    std::string dialError;
    if (!dialHostControl(probeSocket, advertised, probeLink, nullptr, 8000,
                         dialError, "spectator-discovery")) {
        error = "The host's caster did not answer at " + ip + ":"
              + std::to_string(port)
              + ". Check that they are still hosting on that exact port and "
                "that UDP traffic is allowed. This spectator attempt did not "
                "interrupt either player.";
        QLOG("[spectate-discovery] stage=control-dial result=failed "
             "target=%s:%u reason='%s' isolation=player-session-untouched",
             ip.c_str(), static_cast<unsigned>(port), dialError.c_str());
        return false;
    }
    QLOG("[spectate-discovery] stage=control-dial result=ok peer=%s",
         probeLink.peer().str().c_str());
    PumpCtx ctx;
    ctx.lastHeardUs = nowUs();
    TcpConn::Received rx;
    std::string remoteErr;
    const Pump versionPump =
        pumpRecv(probeLink, ctx, MSG_VERSION_CONFIG, 5000, rx, &remoteErr);
    if (versionPump != Pump::Got) {
        error = remoteErr.empty() ? "no answer from the host" : remoteErr;
        QLOG("[spectate-discovery] stage=version result=failed pump=%d "
             "socketError=%d remote='%s'",
             static_cast<int>(versionPump), probeLink.lastError(),
             remoteErr.c_str());
        return false;
    }
    VersionConfig hostVer{};
    if (!bodyAs(rx, hostVer) || hostVer.protocolVersion != kProtocolVersion) {
        error = "the host runs a different version - update the older side";
        QLOG("[spectate-discovery] stage=version result=mismatch host=%u "
             "local=%u bodyBytes=%zu",
             hostVer.protocolVersion, kProtocolVersion, rx.body.size());
        return false;
    }
    VersionConfig myVer{ kProtocolVersion };
    if (!probeLink.sendMsg(MSG_VERSION_CONFIG, &myVer, sizeof(myVer))) {
        error = "could not answer the host's version check";
        QLOG("[spectate-discovery] stage=version-reply result=failed "
             "socketError=%d", probeLink.lastError());
        return false;
    }
    ClientIntent intent{};
    intent.intent = static_cast<uint8_t>(requestIntent);
    if (requestIntent == INTENT_SPECTATE) {
        intent.spectatorRoute = SPECTATE_ROUTE_DIRECT;
        bool degraded = false;
        intent.viewerId = qnet::secureRandom64(degraded);
        if (intent.viewerId == 0) intent.viewerId = 1;
    }
    if (!probeLink.sendMsg(MSG_CLIENT_INTENT, &intent, sizeof(intent))) {
        error = "could not send the spectate request";
        QLOG("[spectate-discovery] stage=intent-send result=failed "
             "socketError=%d", probeLink.lastError());
        return false;
    }
    QLOG("[spectate-discovery] stage=intent-send result=ok viewer=%s",
         qclient::spectator::redactedTag(intent.viewerId).c_str());
    const Pump infoPump =
        pumpRecv(probeLink, ctx, MSG_SPECTATE_INFO, 5000, rx, &remoteErr);
    if (infoPump != Pump::Got) {
        error = remoteErr.empty()
            ? "the host did not answer the spectate request" : remoteErr;
        QLOG("[spectate-discovery] stage=spectate-info result=failed pump=%d "
             "socketError=%d remote='%s'",
             static_cast<int>(infoPump), probeLink.lastError(),
             remoteErr.c_str());
        return false;
    }
    if (!bodyAs(rx, out)) {
        error = "malformed answer from the host";
        QLOG("[spectate-discovery] stage=spectate-info result=malformed "
             "bodyBytes=%zu", rx.body.size());
        return false;
    }
    QLOG("[spectate-discovery] stage=complete state=%u session=%u "
         "sourcePort=%u admission=%u viewers=%u/%u presenceState=%u "
         "presenceFlags=%u",
         static_cast<unsigned>(out.state), out.sessionId,
         static_cast<unsigned>(out.spectatorPort),
         static_cast<unsigned>(out.admissionState),
         static_cast<unsigned>(out.viewerCount),
         static_cast<unsigned>(out.viewerCapacity),
         static_cast<unsigned>(out.presence.state),
         static_cast<unsigned>(out.presence.flags));
    probeLink.closeGraceful();
    return true;
}

bool validLivePresenceImpl(const LivePresence& presence) {
    if (presence.schemaVersion != kLivePresenceSchemaVersion
        || presence.state < LIVE_PRESENCE_HOSTING
        || presence.state > LIVE_PRESENCE_RETAINED
        || (presence.flags & ~(kLivePresenceRosterValid
                              | kLivePresenceRoundScoreValid
                              | kLivePresenceSetScoreValid)) != 0u
        || (presence.activeFighterMask & ~0x0Fu) != 0u
        || presence.roundsToWin < 1u || presence.roundsToWin > 3u
        || presence.playerNames[0][31] != '\0'
        || presence.playerNames[1][31] != '\0') {
        return false;
    }
    if (presence.state == LIVE_PRESENCE_HOSTING) {
        if (presence.sessionId != 0u) return false;
    } else if (presence.sessionId == 0u) {
        return false;
    }
    for (size_t lane = 0u; lane < 2u; ++lane) {
        const std::string name = presence.playerNames[lane];
        if (!name.empty()
            && !qgame::isCanonicalPlayerNameForCodePage(name, CP_UTF8)) {
            return false;
        }
    }
    if ((presence.flags & kLivePresenceRosterValid) != 0u) {
        if (presence.activeFighterMask == 0u) return false;
        for (size_t slot = 0u; slot < 4u; ++slot) {
            const bool active =
                (presence.activeFighterMask & (1u << slot)) != 0u;
            if (active) {
                if (!qreplay::CharacterCatalog::forSeBuild().isSelectable(
                        presence.characterIds[slot])
                    || presence.paletteIndices[slot] > 3u) {
                    return false;
                }
            } else if (presence.characterIds[slot] != 0xFFu
                       || presence.paletteIndices[slot] != 0u) {
                return false;
            }
        }
    } else {
        if (presence.activeFighterMask != 0u) return false;
        for (size_t slot = 0u; slot < 4u; ++slot) {
            if (presence.characterIds[slot] != 0xFFu
                || presence.paletteIndices[slot] != 0u) {
                return false;
            }
        }
    }
    const bool roundScoreValid =
        (presence.flags & kLivePresenceRoundScoreValid) != 0u;
    if (roundScoreValid
        ? (presence.roundWins[0] > presence.roundsToWin
           || presence.roundWins[1] > presence.roundsToWin)
        : (presence.roundWins[0] != 0u || presence.roundWins[1] != 0u)) {
        return false;
    }
    const bool setScoreValid =
        (presence.flags & kLivePresenceSetScoreValid) != 0u;
    if (!setScoreValid
        && (presence.matchWins[0] != 0u || presence.matchWins[1] != 0u)) {
        return false;
    }
    if (presence.state == LIVE_PRESENCE_HOSTING
        && (presence.flags != 0u || presence.playerNames[1][0] != '\0')) {
        return false;
    }
    if (presence.state == LIVE_PRESENCE_CHARACTER_SELECT
        && ((presence.flags & (kLivePresenceRosterValid
                               | kLivePresenceRoundScoreValid)) != 0u)) {
        return false;
    }
    if (presence.state == LIVE_PRESENCE_MATCH
        && ((presence.flags & (kLivePresenceRosterValid
                               | kLivePresenceRoundScoreValid))
            != (kLivePresenceRosterValid
                | kLivePresenceRoundScoreValid))) {
        return false;
    }
    return true;
}

const char* livePresenceStateText(uint8_t state) {
    switch (state) {
        case LIVE_PRESENCE_HOSTING: return "hosting";
        case LIVE_PRESENCE_CHARACTER_SELECT: return "character_select";
        case LIVE_PRESENCE_MATCH: return "match";
        case LIVE_PRESENCE_RETAINED: return "retained";
    }
    return "unknown";
}

std::string jsonString(const std::string& value) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() + 2u);
    out.push_back('"');
    for (const unsigned char byte : value) {
        if (byte == '"' || byte == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(byte));
        } else if (byte < 0x20u) {
            out += "\\u00";
            out.push_back(kHex[byte >> 4u]);
            out.push_back(kHex[byte & 0x0Fu]);
        } else {
            out.push_back(static_cast<char>(byte));
        }
    }
    out.push_back('"');
    return out;
}

std::string livePresenceJsonImpl(const LivePresence& presence) {
    const bool rosterValid =
        (presence.flags & kLivePresenceRosterValid) != 0u;
    const bool roundScoreValid =
        (presence.flags & kLivePresenceRoundScoreValid) != 0u;
    const bool setScoreValid =
        (presence.flags & kLivePresenceSetScoreValid) != 0u;
    std::ostringstream out;
    out << "{\"schema\":" << presence.schemaVersion
        << ",\"state\":" << jsonString(livePresenceStateText(presence.state))
        << ",\"session_id\":" << presence.sessionId
        << ",\"rounds_to_win\":"
        << static_cast<unsigned>(presence.roundsToWin)
        << ",\"confirmed_timeline_frame\":"
        << presence.confirmedTimelineFrame
        << ",\"spectators\":{\"current\":" << presence.viewerCount
        << ",\"capacity\":" << presence.viewerCapacity << "}"
        << ",\"players\":[";
    const qreplay::CharacterCatalog& catalog =
        qreplay::CharacterCatalog::forSeBuild();
    for (size_t lane = 0u; lane < 2u; ++lane) {
        if (lane != 0u) out << ',';
        out << "{\"side\":" << (lane + 1u)
            << ",\"nickname\":"
            << jsonString(presence.playerNames[lane])
            << ",\"character_id\":";
        const bool active = rosterValid
            && (presence.activeFighterMask & (1u << lane)) != 0u;
        if (active) {
            const uint8_t characterId = presence.characterIds[lane];
            out << static_cast<unsigned>(characterId)
                << ",\"character\":"
                << jsonString(catalog.displayName(characterId))
                << ",\"palette\":"
                << static_cast<unsigned>(presence.paletteIndices[lane]);
        } else {
            out << "null,\"character\":null,\"palette\":null";
        }
        out << ",\"round_wins\":";
        if (roundScoreValid)
            out << static_cast<unsigned>(presence.roundWins[lane]);
        else
            out << "null";
        out << ",\"match_wins\":";
        if (setScoreValid)
            out << presence.matchWins[lane];
        else
            out << "null";
        out << '}';
    }
    out << "]}";
    return out.str();
}

static bool g_spectateWantsLatest = false;

int runSpectator(const Settings& s, uint32_t sessionId,
                  uint64_t spectatorCookieLow, uint64_t spectatorCookieHigh,
                  qui::Console* con, bool hiddenDesktop,
                  uint16_t hostControlPort,
                  SpectatorRouteForce routeForce,
                  const bool topologyOptIn) {
    namespace qs = qclient::spectator;
    if (!sessionId || (spectatorCookieLow == 0 && spectatorCookieHigh == 0))
        return fail(con, "Invalid spectator access",
                    qs::preflightFailureText(
                        qs::PreflightFailure::InvalidTarget));
    if (!preflightOnlineArtifacts(s, con, s.saveSpectatorSessions)) return 1;
    qnetplay::compat::CompatibilityIdentity localCompatibility{};
    std::string localCompatibilityError;
    if (!localCompatibilityIdentityFor(
            s, localCompatibility, localCompatibilityError)) {
        return fail(con, "Game files check failed", localCompatibilityError);
    }
    if (!netInit()) return fail(con, "Netplay could not start", networkUnavailableMessage());

    Endpoint host;
    if (!Endpoint::parse(s.joinIp, s.joinPort, host))
        return fail(con, "Invalid spectator host",
                    "Use a numeric IPv4 address and valid UDP port.");

    qs::SpectateTarget target;
    target.hostText = s.joinIp;
    target.hostPort = hostControlPort;
    target.directUdpPort = s.joinPort;
    target.sessionId = sessionId;
    target.epoch = 1;
    target.keyLow = spectatorCookieLow;
    target.keyHigh = spectatorCookieHigh;
    target.simulationContract = qipc::kRuntimeCompatibilityId;
    target.compatibilityIdentity = localCompatibility;
    if (target.relayHostText.empty()) {
        Endpoint relayServer;
        std::string relayError;
        std::string relayIp;
        uint16_t relayPort = 0;
        if (configuredRelayServer(s, relayServer, relayError)
            && parseHostPort(relayServer.str(), relayIp, relayPort,
                             kDefaultRelayPort)) {
            target.relayHostText = relayIp;
            target.relayPort = relayPort;
            QLOG("[spectate] traffic relay %s:%u", relayIp.c_str(),
                 static_cast<unsigned>(relayPort));
        } else {
            target.allowRelayFallback = false;
            QLOG("[spectate] no usable traffic relay (%s); direct and "
                 "punched routes only", relayError.c_str());
        }
    }
    if (!target.relayHostText.empty()
        && !qnet::relay::isValidId(target.relayRoom)) {
        QLOG("[spectate] traffic relay endpoint present without an exact "
             "room; relay fallback remains unavailable");
    }
    {
        Endpoint punchRelay{};
        std::string punchError;
        if (autopunchRelayEndpoint(punchRelay, punchError)) {
            target.punchRelayIpv4BE = punchRelay.sa.sin_addr.s_addr;
            target.punchRelayPort = ntohs(punchRelay.sa.sin_port);
            QLOG("[spectate] punch relay %s", punchRelay.str().c_str());
        } else {
            QLOG("[spectate] no punch relay (%s)", punchError.c_str());
        }
    }

    switch (routeForce) {
    case SpectatorRouteForce::Auto:
        break;
    case SpectatorRouteForce::Direct:
        target.directRaceMs = 3000;
        target.allowPunch = false;
        target.allowRelayFallback = false;
        target.requestedRoute = SPECTATE_ROUTE_DIRECT;
        break;
    case SpectatorRouteForce::Punch:
        target.directRaceMs = 0;
        target.allowPunch = true;
        target.allowRelayFallback = false;
        target.requestedRoute = SPECTATE_ROUTE_PUNCHED;
        break;
    case SpectatorRouteForce::Relay:
        target.directRaceMs = 0;
        target.allowPunch = false;
        target.allowRelayFallback = true;
        target.requestedRoute = SPECTATE_ROUTE_RELAY;
        break;
    }
    QLOG("[spectate-route-force] mode=%s directRaceMs=%u punch=%d relay=%d",
         spectatorRouteForceName(routeForce), target.directRaceMs,
         target.allowPunch ? 1 : 0,
         target.allowRelayFallback ? 1 : 0);

    uint64_t lastPollUs = 0;
    bool cancelRequested = false;
    const auto cancelled = [&]() -> bool {
        if (cancelRequested) return true;
        const uint64_t now = nowUs();
        if (now - lastPollUs < 100000ull) return false;
        lastPollUs = now;
        if (con != nullptr) {
            if (con->pollKey(0) == VK_ESCAPE) cancelRequested = true;
        } else if (escPressedFocused()) {
            cancelRequested = true;
        }
        return cancelRequested;
    };

    info(con, "Checking that " + host.str() + " can actually be watched "
              "(Esc cancels)...");

    {
        bool degraded = false;
        target.viewerId = qnet::secureRandom64(degraded);
        if (target.viewerId == 0) target.viewerId = 1;
        if (degraded)
            QLOG("[spectate] viewer identity used the last-resort RNG path");
    }

    std::unique_ptr<qs::RouteIo> routeIo = qs::makeSystemRouteIo();
    if (!routeIo)
        return fail(con, "Could not start spectating",
                    "The caster could not set up the connection it needs to "
                    "watch this match.");
    const qs::SpectatePreflightResult route =
        qs::preflightSpectateWith(target, 30000, cancelled, *routeIo);
    QLOG("[spectate] preflight ok=%d failure=%u inconclusive=%d route=%s "
         "fallback=%s elapsed=%ums sourceLive=%d waiting=%d detach=%d "
         "released=%d reason='%s' key=%s",
         route.ok ? 1 : 0, static_cast<unsigned>(route.failure),
         route.inconclusive ? 1 : 0,
         qs::routeLabelName(
         static_cast<qs::SpectatorRouteLabel>(route.routeLabel)),
         route.fallbackReason.empty() ? "none" : route.fallbackReason.c_str(),
         route.elapsedMs, route.sourceConfirmedLive ? 1 : 0,
         route.waitingForStart ? 1 : 0, route.detachSent ? 1 : 0,
         route.reservationsReleased ? 1 : 0,
         route.failureReason.empty() ? "none" : route.failureReason.c_str(),
         qs::redactedTag(spectatorCookieLow ^ spectatorCookieHigh).c_str());
    const uint64_t admittedViewerId =
        route.grant.viewerId != 0 ? route.grant.viewerId : route.viewerId;
    SpectatorAdmissionReleaseGuard admissionRelease(
        qoh99SpectatorAdmissionReleaseRequired(
            route.ok, route.grant.grantId, admittedViewerId),
        [&routeIo, &target, admittedViewerId]() {
            std::string releaseError;
            const bool released = routeIo->controlRelease(
                target, admittedViewerId, releaseError);
            QLOG("[spectator-admission] viewer release after caster lifecycle "
                 "viewer=%s acknowledged=%d detail='%s'",
                 qclient::spectator::redactedTag(admittedViewerId).c_str(),
                 released ? 1 : 0, releaseError.c_str());
        });
    const bool forcedRelay =
        routeForce == SpectatorRouteForce::Relay;
    if (!qoh99SpectatorPreflightMayLaunch(
            forcedRelay, route.ok, route.inconclusive, route.routeLabel)) {
        std::string detail;
        if (!route.failureReason.empty()) {
            detail = route.failureReason;
        } else if (route.failure != qs::PreflightFailure::None) {
            detail = qs::preflightFailureText(route.failure);
        } else {
            detail = "The preflight did not confirm the spectator relay route.";
        }
        QLOG("[spectate] forced relay was not proved; refusing launch "
             "(failure=%u inconclusive=%d route=%s)",
             static_cast<unsigned>(route.failure),
             route.inconclusive ? 1 : 0,
             qs::routeLabelName(
                 static_cast<qs::SpectatorRouteLabel>(route.routeLabel)));
        return fail(
            con, "Can't watch through the relay",
            detail + " No spectator process was started because "
                     "--spectator-route relay requires a proved relay route.");
    }
    if (!route.ok) {
        if (route.failure == qs::PreflightFailure::Cancelled) {
            info(con, qs::preflightFailureText(qs::PreflightFailure::Cancelled));
            return 0;
        }
        if (qs::isDefinitivePreflightFailure(route.failure)) {
            std::string detail = qs::preflightFailureText(route.failure);
            if (!route.failureReason.empty()) detail = route.failureReason;
            return fail(con, "Can't watch that match", detail, 0,
                        route.routeSummary);
        }
        QLOG("[spectate] preflight INCONCLUSIVE (failure=%u: %s); launching "
             "anyway and holding at the rendezvous",
             static_cast<unsigned>(route.failure),
             route.failureReason.c_str());
        info(con, "The match has not answered yet (" + route.failureReason
                  + ") Opening the spectator view anyway - it will wait at the "
                    "rendezvous until the match starts. Close the window to "
                    "stop waiting.");
    }

    qnetplay::spectator::RelayViewerBinding relayHandoff{};
    const bool relayRoute =
        route.ok
        && route.routeLabel == static_cast<uint16_t>(
               qs::SpectatorRouteLabel::Relay);
    if (relayRoute && !routeIo->relayViewerBinding(relayHandoff)) {
        return fail(con, "Can't watch that match",
                    "The relay route was proved without a complete runtime "
                    "handoff. The game was not started.");
    }

    UdpSock udp;
    const uintptr_t proved = routeIo->detachViewerSocket();
    if (proved != ~uintptr_t(0)) {
        if (!udp.adopt(static_cast<SOCKET>(proved))) {
            const int adoptError = WSAGetLastError();
            return fail(
                con, "Can't watch that match",
                "The proved spectator socket could not be handed to the "
                "runtime. The game was not started.",
                adoptError != 0 ? adoptError : WSAEINVAL);
        }
        QLOG("[spectate] adopted the proved viewer socket, local port %u",
             static_cast<unsigned>(udp.boundPort()));
    } else if (!udp.open(0)) {
        return fail(con, "Could not start spectating",
                    "The caster could not open the network connection it "
                    "needs to receive the match.", udp.lastError());
    }

    Settings local = s;
    local.delay = 0;
    local.rollback = 0;
    qipc::IpcConfig cfg =
        baseConfig(local, qipc::MODE_SPECTATOR, 0,
                   0);
    cfg.sessionId = sessionId;
    cfg.compatibilityId = qipc::kRuntimeCompatibilityId;
    cfg.compatibilityIdentity = localCompatibility;
    cfg.spectatorCookieLow = spectatorCookieLow;
    cfg.spectatorCookieHigh = spectatorCookieHigh;
    cfg.spectatorWantsNewestCheckpoint =
        g_spectateWantsLatest ? 1u : 0u;
    if (route.confirmedEndpoint.valid()) {
        cfg.peerAddrBE = route.confirmedEndpoint.ipv4BE;
        cfg.peerPortBE = htons(route.confirmedEndpoint.port);
    } else {
        cfg.peerAddrBE = host.sa.sin_addr.s_addr;
        cfg.peerPortBE = host.sa.sin_port;
    }
    cfg.spectatorGrantId = route.grant.grantId;
    cfg.spectatorGrantExpiryUnix = route.grant.expiryUnix;
    cfg.spectatorGrantMacLow = route.grant.macLow;
    cfg.spectatorGrantMacHigh = route.grant.macHigh;
    cfg.spectatorViewerId = route.viewerId != 0 ? route.viewerId
                                                : target.viewerId;
    cfg.spectatorGameId = route.gameId;
    cfg.spectatorGeneration = route.generation;
    cfg.spectatorResumeRecord = target.resumeRecord;
    cfg.spectatorResumeDigest = target.resumeDigest;
    cfg.spectatorSequenceBase = route.ok ? route.nextSequence : 0u;
    QLOG("[spectate] handoff sequenceBase=%u (preflight spent %u)",
         cfg.spectatorSequenceBase,
         cfg.spectatorSequenceBase != 0u ? cfg.spectatorSequenceBase - 1u : 0u);
    if (relayRoute) {
        cfg.spectatorRelayActive = 1;
        cfg.spectatorRelayRoomLow = relayHandoff.room.low;
        cfg.spectatorRelayRoomHigh = relayHandoff.room.high;
        cfg.spectatorRelayTokenLow = relayHandoff.token.low;
        cfg.spectatorRelayTokenHigh = relayHandoff.token.high;
        cfg.spectatorRelaySourceGeneration =
            relayHandoff.sourceGeneration;
    }
    const bool topologyRoute =
        route.grant.route == SPECTATE_ROUTE_DIRECT
        || route.grant.route == SPECTATE_ROUTE_PUNCHED;
    const bool topologyGrantMatchesHandoff =
        route.grant.grantId != 0
        && route.grant.viewerId == cfg.spectatorViewerId
        && route.grant.sessionId == cfg.sessionId
        && route.grant.expiryUnix != 0
        && route.grant.sourceGeneration != 0
        && route.grant.resumeRecord == cfg.spectatorResumeRecord
        && route.grant.resumeDigest == cfg.spectatorResumeDigest
        && (route.grant.macLow != 0 || route.grant.macHigh != 0);
    const bool topologyCommonReady =
        topologyOptIn && route.ok && topologyGrantMatchesHandoff
        && target.hostPort != 0 && host.sa.sin_addr.s_addr != 0
        && route.generation != 0;
    const bool topologyDirectReady =
        topologyCommonReady && !relayRoute && topologyRoute;
    const bool relayHandoffComplete =
        relayRoute && cfg.spectatorRelayActive == 1
        && (cfg.spectatorRelayRoomLow != 0 || cfg.spectatorRelayRoomHigh != 0)
        && (cfg.spectatorRelayTokenLow != 0
            || cfg.spectatorRelayTokenHigh != 0)
        && cfg.spectatorRelaySourceGeneration != 0;
    const bool topologyRelayReady =
        topologyCommonReady && relayHandoffComplete
        && route.grant.route == SPECTATE_ROUTE_RELAY;
    const bool topologyBootstrapReady =
        topologyDirectReady || topologyRelayReady;
    if (topologyBootstrapReady) {
        cfg.spectatorTopologyEnabled = 1;
        cfg.spectatorTopologyRouteKind =
            topologyRelayReady ? qipc::kSpectatorTopologyRouteRelay
                               : route.grant.route;
        cfg.spectatorTopologyHostControlIpv4BE = host.sa.sin_addr.s_addr;
        cfg.spectatorTopologyHostControlPort = target.hostPort;
        cfg.spectatorTopologyQspRouteGeneration = route.generation;
        cfg.spectatorTopologyGrantSourceGeneration =
            route.grant.sourceGeneration;
        QLOG("[spectator-topology] opt-in bootstrap armed route=%u "
             "qspGeneration=%u sourceGeneration=%u",
             static_cast<unsigned>(cfg.spectatorTopologyRouteKind),
             static_cast<unsigned>(cfg.spectatorGeneration),
             static_cast<unsigned>(cfg.spectatorTopologyGrantSourceGeneration));
    } else if (topologyOptIn) {
        QLOG("[spectator-topology] opt-in unavailable for this launch; "
             "ordinary E0 viewing continues");
    }
    cfg.directBoot = 1;
    cfg.vsRow = qipc::kVsRow1v1;
    cfg.serveSpectators = 0;
    cfg.spectatorServiceMode = qipc::SPECTATOR_SERVICE_OFF;

    if (route.ok) {
        std::string ready = "Route confirmed (";
        ready += qs::routeLabelName(
            static_cast<qs::SpectatorRouteLabel>(route.routeLabel));
        ready += ")";
        if (route.grant.grantId != 0) ready += " with a spectator pass";
        if (route.waitingForStart)
            ready += " - the match has not started yet; the view will hold at "
                     "the rendezvous until it does";
        else if (!route.statusText.empty())
            ready += " - " + route.statusText;
        info(con, ready + ".");
    }
    info(con, "Starting the spectator view (session "
              + std::to_string(sessionId) + ")...");
    return launch(local, cfg, udp.raw(), con, INFINITE, hiddenDesktop);
}

namespace controls_ui {

const char* kControlsFileName = "qoh99caster-controls.json";
const char* kWrapperIniName = "ddraw_wrapper.ini";

std::string modeControlDikList(const qinput::ControlsFile& file,
                               qinput::ModeControlAction action) {
    std::string out;
    char token[8];
    for (const qinput::ProfileBinding& binding :
         file.modeControls[static_cast<size_t>(action)]) {
        if (binding.type != qinput::CompiledBindingType::KeyboardScan)
            continue;
        if (!out.empty()) out += ',';
        std::snprintf(token, sizeof(token), "0x%02X", binding.dik);
        out += token;
    }
    return out;
}

std::string derivedWrapperHotkeys(const qinput::ControlsFile& file) {
    std::string out =
        "[Hotkeys]\r\n"
        "; Change these in the caster Controls menu. Empty = disabled.\r\n";
    const auto add = [&](const char* name, qinput::ModeControlAction action,
                         const char* description) {
        out += "; ";
        out += description;
        out += "\r\n";
        out += name;
        out += '=';
        out += modeControlDikList(file, action);
        out += "\r\n";
    };
    add("TogglePresentation", qinput::ModeControlAction::TogglePresentationMode,
        "Change display mode.");
    add("ToggleOverlayMenu", qinput::ModeControlAction::ToggleOverlayMenu,
        "Open or close the overlay menu.");
    add("ToggleInputViewer", qinput::ModeControlAction::ToggleInputViewer,
        "Show or hide input history.");
    add("ReplayPause", qinput::ModeControlAction::ReplayPause,
        "Pause or resume a replay.");
    add("ReplayStep", qinput::ModeControlAction::ReplayStep,
        "Advance one paused replay frame.");
    add("ReplayStepBack", qinput::ModeControlAction::ReplayStepBack,
        "Go back one replay frame and pause.");
    add("ReplayRestart", qinput::ModeControlAction::ReplayRestart,
        "Return to the beginning of a replay and pause.");
    add("ReplaySlower", qinput::ModeControlAction::ReplaySlower,
        "Slow a replay.");
    add("ReplayFaster", qinput::ModeControlAction::ReplayFaster,
        "Speed up a replay.");
    add("ReplayToggleCollisionDisplay",
        qinput::ModeControlAction::ReplayToggleCollisionDisplay,
        "Show or hide hitboxes, hurtboxes, and collision boxes.");
    add("ReplayTakeoverExit", qinput::ModeControlAction::ReplayTakeoverExit,
        "Exit takeover.");
    add("ReplayTakeoverP1", qinput::ModeControlAction::ReplayTakeoverP1,
        "Take over Player 1.");
    add("ReplayTakeoverP2", qinput::ModeControlAction::ReplayTakeoverP2,
        "Take over Player 2.");
    add("ReplayTakeoverRetry", qinput::ModeControlAction::ReplayTakeoverRetry,
        "Retry takeover.");
    add("ReplayTakeoverDelayDecrease",
        qinput::ModeControlAction::ReplayTakeoverDelayDecrease,
        "Lower takeover delay.");
    add("ReplayTakeoverDelayIncrease",
        qinput::ModeControlAction::ReplayTakeoverDelayIncrease,
        "Raise takeover delay.");
    return out;
}

std::string replaceWrapperHotkeysSection(const std::string& original,
                                         const qinput::ControlsFile& file) {
    std::istringstream stream(original);
    std::string line;
    std::string preserved;
    bool skipping = false;
    bool inserted = false;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string section = line;
        const size_t first = section.find_first_not_of(" \t");
        const size_t last = section.find_last_not_of(" \t");
        if (first == std::string::npos) {
            section.clear();
        } else {
            section = section.substr(first, last - first + 1u);
            std::transform(section.begin(), section.end(), section.begin(),
                           [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
        }
        if (!section.empty() && section.front() == '['
            && section.back() == ']') {
            if (section == "[hotkeys]") {
                if (!inserted) {
                    preserved += derivedWrapperHotkeys(file);
                    inserted = true;
                }
                skipping = true;
                continue;
            }
            skipping = false;
        }
        if (!skipping) preserved += line + "\r\n";
    }
    if (!inserted) {
        if (!preserved.empty() && preserved.back() != '\n') preserved += "\r\n";
        preserved += derivedWrapperHotkeys(file);
    }
    return preserved;
}

bool writeWholeFile(const std::string& path, const std::string& bytes,
                    std::string& error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { error = "cannot open " + path; return false; }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) { error = "write failed for " + path; return false; }
    return true;
}

bool readWholeFileState(const std::string& path, bool& exists,
                        std::string& bytes, std::string& error) {
    exists = false;
    bytes.clear();
    const DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            exists = false;
            bytes.clear();
            return true;
        }
        error = "cannot inspect " + path + " (error "
              + std::to_string(code) + ")";
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        error = path + " is a directory";
        return false;
    }
    exists = true;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot read " + path;
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in),
                 std::istreambuf_iterator<char>());
    if (in.bad()) {
        error = "read failed for " + path;
        return false;
    }
    return true;
}

bool controlsSourceMatches(bool expectedExists,
                           const std::string& expectedBytes,
                           std::string& error) {
    bool exists = false;
    std::string bytes;
    if (!readWholeFileState(kControlsFileName, exists, bytes, error))
        return false;
    if (exists != expectedExists || (exists && bytes != expectedBytes)) {
        error = "controls changed on disk while this page was open; reopen "
                "Controls so no player profile is overwritten";
        return false;
    }
    return true;
}

bool saveControlsFileAtomic(const qinput::ControlsFile& file,
                            bool originalExisted,
                            const std::string& originalBytes,
                            bool preserveRejectedOriginal,
                            std::string& error,
                            int* blockingPlayer = nullptr) {
    if (blockingPlayer != nullptr) *blockingPlayer = -1;
    if (!controlsSourceMatches(originalExisted, originalBytes, error))
        return false;
    for (const qinput::InputProfile& profile : file.profiles) {
        bool complete = true;
        for (const auto& bindings : profile.actions)
            if (bindings.empty()) complete = false;
        if (!complete && profile.id != file.activeProfileId) continue;
        const qinput::CompileResult compiled = qinput::compileInputProfile(
            profile, false,
            profile.id == file.activeProfileId ? &file : nullptr);
        if (!compiled.ok) {
            int owner = -1;
            for (int player = 0; player < 4; ++player) {
                if (file.activeByPlayer[player] == profile.id) {
                    owner = player;
                    break;
                }
            }
            if (owner < 0 && profile.id == file.activeProfileId) owner = 0;
            if (blockingPlayer != nullptr) *blockingPlayer = owner;
            error = owner >= 0
                ? "player " + std::to_string(owner + 1) + " (profile '"
                      + profile.name + "'): " + compiled.error
                : "profile '" + profile.name + "': " + compiled.error;
            return false;
        }
    }
    if (!file.pendingModeMigrations.empty()) {
        error = "choose the owner of each migrated mode control before saving";
        return false;
    }
    const std::string serialized = qinput::serializeControlsFile(file);
    qinput::ControlsFile reparsed{};
    std::string readBackError;
    if (!qinput::parseControlsFile(serialized, reparsed, readBackError)
        || qinput::serializeControlsFile(reparsed) != serialized) {
        error = "generated controls failed read-back validation";
        if (!readBackError.empty()) error += ": " + readBackError;
        return false;
    }
    const std::string tempSuffix = ".tmp."
        + std::to_string(GetCurrentProcessId()) + "."
        + std::to_string(GetTickCount64());
    const std::string controlsTemp =
        std::string(kControlsFileName) + tempSuffix;
    const std::string iniTemp = std::string(kWrapperIniName) + tempSuffix;
    std::string oldIni;
    bool hadIni = false;
    {
        std::ifstream in(kWrapperIniName, std::ios::binary);
        if (in) {
            hadIni = true;
            oldIni.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
        }
    }
    const std::string newIni = replaceWrapperHotkeysSection(oldIni, file);
    if (!writeWholeFile(controlsTemp, serialized, error)) {
        DeleteFileA(controlsTemp.c_str());
        return false;
    }
    bool tempExists = false;
    std::string tempBytes;
    qinput::ControlsFile tempParsed{};
    readBackError.clear();
    if (!readWholeFileState(controlsTemp, tempExists, tempBytes, error)
        || !tempExists || tempBytes != serialized
        || !qinput::parseControlsFile(tempBytes, tempParsed, readBackError)
        || qinput::serializeControlsFile(tempParsed) != serialized) {
        DeleteFileA(controlsTemp.c_str());
        if (error.empty()) {
            error = "temporary controls file failed read-back validation";
            if (!readBackError.empty()) error += ": " + readBackError;
        }
        return false;
    }
    if (!writeWholeFile(iniTemp, newIni, error)) {
        DeleteFileA(controlsTemp.c_str());
        DeleteFileA(iniTemp.c_str());
        return false;
    }
    if (!controlsSourceMatches(originalExisted, originalBytes, error)) {
        DeleteFileA(controlsTemp.c_str());
        DeleteFileA(iniTemp.c_str());
        return false;
    }
    if (originalExisted
        && !CopyFileA(kControlsFileName,
                      (std::string(kControlsFileName) + ".bak").c_str(),
                      FALSE)) {
        const DWORD code = GetLastError();
        DeleteFileA(controlsTemp.c_str());
        DeleteFileA(iniTemp.c_str());
        error = "could not back up existing controls (error "
              + std::to_string(code) + ")";
        return false;
    }
    if (preserveRejectedOriginal && originalExisted) {
        const std::string recovery = std::string(kControlsFileName)
            + ".rejected." + std::to_string(GetCurrentProcessId()) + "."
            + std::to_string(GetTickCount64()) + ".json";
        if (!CopyFileA(kControlsFileName, recovery.c_str(), TRUE)) {
            const DWORD code = GetLastError();
            DeleteFileA(controlsTemp.c_str());
            DeleteFileA(iniTemp.c_str());
            error = "could not preserve unreadable controls (error "
                  + std::to_string(code) + ")";
            return false;
        }
        bool recoveryExists = false;
        std::string recoveryBytes;
        if (!readWholeFileState(recovery, recoveryExists, recoveryBytes,
                                error)
            || !recoveryExists || recoveryBytes != originalBytes) {
            DeleteFileA(controlsTemp.c_str());
            DeleteFileA(iniTemp.c_str());
            if (error.empty()) error = "unreadable-controls backup mismatch";
            return false;
        }
        QLOG("[controls] unreadable original preserved as %s",
             recovery.c_str());
    }
    const bool iniBackupCreated = hadIni && CopyFileA(
        kWrapperIniName,
        (std::string(kWrapperIniName) + ".bak").c_str(), FALSE) != FALSE;
    if (!MoveFileExA(iniTemp.c_str(), kWrapperIniName,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(controlsTemp.c_str());
        DeleteFileA(iniTemp.c_str());
        error = std::string("atomic replace failed for ") + kWrapperIniName;
        return false;
    }
    if (!MoveFileExA(controlsTemp.c_str(), kControlsFileName,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD jsonMoveError = GetLastError();
        const std::string rollback = std::string(kWrapperIniName) + ".rollback";
        bool restored = false;
        std::string restoreError;
        if (hadIni && writeWholeFile(rollback, oldIni, restoreError)) {
            restored = MoveFileExA(
                rollback.c_str(), kWrapperIniName,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        } else if (!hadIni) {
            restored = DeleteFileA(kWrapperIniName) != FALSE
                || GetLastError() == ERROR_FILE_NOT_FOUND;
        }
        DeleteFileA(controlsTemp.c_str());
        error = "atomic replace failed for qoh99caster-controls.json (error "
              + std::to_string(jsonMoveError) + "); ";
        if (restored) {
            error += "wrapper hotkeys were restored";
        } else if (hadIni && GetFileAttributesA(rollback.c_str())
                               != INVALID_FILE_ATTRIBUTES) {
            error += "automatic INI restore failed; exact prior bytes remain in "
                   + rollback;
        } else {
            error += iniBackupCreated
                ? "automatic INI restore failed; use ddraw_wrapper.ini.bak"
                : "automatic INI restore failed and no recovery file could be written";
            if (!restoreError.empty()) error += " (" + restoreError + ")";
        }
        return false;
    }
    return true;
}

qinput::ControlsFile loadControlsFileOrDefault() {
    qinput::ControlsFile file{};
    std::ifstream in(kControlsFileName, std::ios::binary);
    if (in) {
        std::string textContent((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        std::string error;
        qinput::ControlsFile parsed{};
        if (qinput::parseControlsFile(textContent, parsed, error)) {
            return parsed;
        }
        QLOG("[controls] existing %s rejected: %s", kControlsFileName,
             error.c_str());
    }
    return qinput::makeDefaultControlsFile();
}

struct ControlsRow {
    enum class Kind : uint8_t {
        Header,
        Action,
        ModeControls,
        MapAll,
        Player,
        Source,
        Device,
        Socd,
        Focus,
        Reset,
        Save,
    };
    Kind kind = Kind::Header;
    int index = 0;
};

const char* const kActionLabels[8] = {
    "Up", "Down", "Left", "Right", "A", "B", "C", "D"};
const char* const kSocdLabels[4] = {
    "game default (both count)",
    "stop moving",
    "newest direction wins",
    "up wins"};

std::string padded(const std::string& text, size_t width) {
    std::string out = text;
    if (out.size() < width) out.append(width - out.size(), ' ');
    return out;
}

qinput::InputProfile* playerProfileOf(qinput::ControlsFile& file,
                                      int player) {
    std::string& id = file.activeByPlayer[player];
    if (player == 0 && id.empty()) id = file.activeProfileId;
    if (!id.empty()) {
        for (qinput::InputProfile& profile : file.profiles)
            if (profile.id == id) return &profile;
    }
    if (player == 0) return nullptr;
    qinput::InputProfile fresh{};
    fresh.id = "player" + std::to_string(player + 1);
    fresh.name = "Player " + std::to_string(player + 1);
    fresh.sourceMode = qinput::CompiledSourceMode::Keyboard;
    for (const qinput::InputProfile& profile : file.profiles)
        if (profile.id == fresh.id) fresh.id += "x";
    file.profiles.push_back(fresh);
    id = fresh.id;
    return &file.profiles.back();
}

qinput::InputProfile* configuredPlayerProfileOf(qinput::ControlsFile& file,
                                                int player) {
    const std::string& id = player == 0 && file.activeByPlayer[0].empty()
        ? file.activeProfileId : file.activeByPlayer[player];
    if (id.empty()) return nullptr;
    for (qinput::InputProfile& profile : file.profiles)
        if (profile.id == id) return &profile;
    return nullptr;
}

qinput::InputProfile* activeProfileOf(qinput::ControlsFile& file) {
    for (qinput::InputProfile& profile : file.profiles)
        if (profile.id == file.activeProfileId) return &profile;
    if (file.profiles.empty()) return nullptr;
    file.activeProfileId = file.profiles.front().id;
    return &file.profiles.front();
}

bool isKeyboardBinding(const qinput::ProfileBinding& binding) {
    return binding.type == qinput::CompiledBindingType::KeyboardScan;
}

const qinput::ProfileBinding* keyboardSlot(
    const std::vector<qinput::ProfileBinding>& bindings) {
    for (const qinput::ProfileBinding& binding : bindings)
        if (isKeyboardBinding(binding)) return &binding;
    return nullptr;
}

const qinput::ProfileBinding* controllerSlot(
    const std::vector<qinput::ProfileBinding>& bindings) {
    for (const qinput::ProfileBinding& binding : bindings)
        if (!isKeyboardBinding(binding)) return &binding;
    return nullptr;
}

void replaceKeyboardSlot(std::vector<qinput::ProfileBinding>& bindings,
                         uint8_t dik) {
    for (size_t i = bindings.size(); i-- > 0;)
        if (isKeyboardBinding(bindings[i]))
            bindings.erase(bindings.begin() + static_cast<long>(i));
    qinput::ProfileBinding binding{};
    binding.type = qinput::CompiledBindingType::KeyboardScan;
    binding.dik = dik;
    bindings.push_back(binding);
}

void replaceControllerSlot(std::vector<qinput::ProfileBinding>& bindings,
                           const qinput::ProfileBinding& binding) {
    for (size_t i = bindings.size(); i-- > 0;)
        if (!isKeyboardBinding(bindings[i]))
            bindings.erase(bindings.begin() + static_cast<long>(i));
    bindings.push_back(binding);
}

bool sameControllerBinding(const qinput::ProfileBinding& a,
                           const qinput::ProfileBinding& b) {
    return a.type == b.type && a.probeKind == b.probeKind
        && a.token == b.token && a.polarity == b.polarity
        && a.hatBit == b.hatBit;
}

std::string conflictOwner(const qinput::InputProfile& profile, uint8_t dik,
                          int skipAction) {
    for (int action = 0; action < 8; ++action) {
        if (action == skipAction) continue;
        const qinput::ProfileBinding* slot =
            keyboardSlot(profile.actions[action]);
        if (slot != nullptr && slot->dik == dik) return kActionLabels[action];
    }
    return std::string();
}

std::string controllerConflictOwner(const qinput::InputProfile& profile,
                                    const qinput::ProfileBinding& binding,
                                    int skipAction) {
    for (int action = 0; action < 8; ++action) {
        if (action == skipAction) continue;
        const qinput::ProfileBinding* slot =
            controllerSlot(profile.actions[action]);
        if (slot != nullptr && sameControllerBinding(*slot, binding))
            return kActionLabels[action];
    }
    return std::string();
}

void removeDik(qinput::InputProfile& profile, uint8_t dik) {
    for (auto& bindings : profile.actions)
        for (size_t i = bindings.size(); i-- > 0;)
            if (isKeyboardBinding(bindings[i]) && bindings[i].dik == dik)
                bindings.erase(bindings.begin() + static_cast<long>(i));
}

std::string stealDikAcrossPlayers(qinput::ControlsFile& file, uint8_t dik,
                                  int skipPlayer, int skipAction) {
    std::string owner;
    for (int player = 0; player < 4; ++player) {
        const std::string& id = player == 0
            ? (file.activeByPlayer[0].empty() ? file.activeProfileId
                                              : file.activeByPlayer[0])
            : file.activeByPlayer[player];
        if (id.empty()) continue;
        for (qinput::InputProfile& profile : file.profiles) {
            if (profile.id != id) continue;
            if (owner.empty()) {
                const std::string local = conflictOwner(
                    profile, dik, player == skipPlayer ? skipAction : -1);
                if (!local.empty()) {
                    owner = player == skipPlayer
                        ? local
                        : "Player " + std::to_string(player + 1) + "'s "
                              + local;
                }
            }
            removeDik(profile, dik);
        }
    }
    return owner;
}

void removeControllerBinding(qinput::InputProfile& profile,
                             const qinput::ProfileBinding& binding) {
    for (auto& bindings : profile.actions)
        for (size_t i = bindings.size(); i-- > 0;)
            if (!isKeyboardBinding(bindings[i])
                && sameControllerBinding(bindings[i], binding))
                bindings.erase(bindings.begin() + static_cast<long>(i));
}

bool profileHasControllerBindings(const qinput::InputProfile& profile) {
    for (const auto& bindings : profile.actions)
        if (controllerSlot(bindings) != nullptr) return true;
    return false;
}

bool profileUsesController(const qinput::InputProfile& profile) {
    return profile.sourceMode != qinput::CompiledSourceMode::Keyboard
        || profileHasControllerBindings(profile)
        || !profile.device.guid.empty();
}

bool bindingRowIsBound(const std::vector<qinput::ProfileBinding>& bindings) {
    return keyboardSlot(bindings) != nullptr
        || controllerSlot(bindings) != nullptr;
}

std::string bindingRowText(
    const std::vector<qinput::ProfileBinding>& bindings,
    bool preferController, bool required) {
    const qinput::ProfileBinding* key = keyboardSlot(bindings);
    const qinput::ProfileBinding* pad = controllerSlot(bindings);
    std::string keyText = key != nullptr
        ? std::string(qinput::dikDisplayLabel(key->dik)) : std::string();
    std::string padText = pad != nullptr
        ? qinput::controllerBindingLabel(*pad) : std::string();
    if (pad != nullptr && padText.empty()) padText = "controller";
    if (keyText.empty() && padText.empty()) return required ? "not set" : "-";
    if (keyText.empty()) return padText;
    if (padText.empty()) return keyText;
    return preferController ? padText + "  +  " + keyText
                            : keyText + "  +  " + padText;
}

std::string sourceRowText(const qinput::InputProfile& profile,
                          const std::string& connectedPadName) {
    if (!profileUsesController(profile)) return "keyboard";
    if (!connectedPadName.empty()) return "controller: " + connectedPadName;
    if (profile.device.guid.empty()) return "controller: none chosen";
    return "controller: "
         + (profile.device.normalizedName.empty()
                ? std::string("saved controller")
                : profile.device.normalizedName)
         + " (unplugged)";
}

void stripControllerBindings(qinput::InputProfile& profile) {
    for (auto& bindings : profile.actions)
        for (size_t i = bindings.size(); i-- > 0;)
            if (!isKeyboardBinding(bindings[i]))
                bindings.erase(bindings.begin() + static_cast<long>(i));
    profile.axisCalibration.clear();
}

bool profileHasStandardizedBindings(const qinput::InputProfile& profile) {
    for (const auto& bindings : profile.actions) {
        for (const qinput::ProfileBinding& binding : bindings) {
            if (qinput::compiledProbeNeedsStandardizedBackend(
                    binding.probeKind)) return true;
        }
    }
    return false;
}

bool profileBackendMatchesPreview(const qinput::InputProfile& profile,
                                  const qinput::ControllerPreview& preview) {
    if (profile.probeBackend.empty()) return true;
    return (profile.probeBackend == "sdl") == preview.usingSdlBackend();
}

uint64_t savedSerialHash(const qinput::ProfileDevice& device) {
    if (!device.serial.empty())
        return qinput::identityStringHash(device.serial.c_str());
    return device.serialHash;
}

bool sameSavedPhysicalDevice(const qinput::ProfileDevice& a,
                             const qinput::ProfileDevice& b) {
    const uint64_t serialA = savedSerialHash(a);
    const uint64_t serialB = savedSerialHash(b);
    if (serialA != 0 && serialB != 0) return serialA == serialB;
    return a.pathFingerprint != 0 && b.pathFingerprint != 0
        && a.pathFingerprint == b.pathFingerprint && a.guid == b.guid;
}

void normalizeProfile(qinput::InputProfile& profile) {
    if (profileHasControllerBindings(profile)) {
        profile.sourceMode =
            qinput::CompiledSourceMode::KeyboardAndController;
    } else {
        profile.sourceMode = qinput::CompiledSourceMode::Keyboard;
        profile.mapping.clear();
        profile.probeBackend.clear();
    }
}

struct UnifiedCapture {
    enum class Kind : uint8_t {
        Cancelled, Timeout, Key, Pad, PadCluster, PadLost
    };
    Kind kind = Kind::Cancelled;
    uint8_t dik = 0;
    qinput::ProfileBinding pad{};
    std::string padLabel;
    qinput::MovementBindings cluster{};
    std::string error;
};

UnifiedCapture captureUnified(qui::Console& con,
                              qinput::ControllerPreview& preview,
                              const std::string& what,
                              bool directionRow, bool sessionRow,
                              bool useController,
                              bool allowEither = false) {
    UnifiedCapture result{};
    const bool padActive = useController && preview.hasSelection();

    std::vector<qui::Row> rows;
    rows.push_back({ { "  Set a control", qui::Tone::Title } });
    rows.push_back({});
    rows.push_back({ { "  ", qui::Tone::Default },
                     { what, qui::Tone::Accent } });
    rows.push_back({});
    if (padActive && directionRow
        && preview.directionsMustBindIndividually()) {
        rows.push_back({ { "  This controller reports each direction as its "
                           "own button, so", qui::Tone::Default } });
        rows.push_back({ { "  set them one at a time. Press the button for "
                           "this direction.", qui::Tone::Default } });
    } else if (padActive && directionRow) {
        rows.push_back({ { "  Push the d-pad or a stick in any direction - "
                           "that sets all four", qui::Tone::Default } });
        rows.push_back({ { "  directions at once.", qui::Tone::Default } });
    } else if (padActive) {
        rows.push_back({ { allowEither
                               ? "  Press a key or controller button/d-pad."
                               : sessionRow
                                     ? "  Press a controller button."
                                     : "  Press a controller control.",
                           qui::Tone::Default } });
    } else {
        rows.push_back({ { "  Press the key you want.",
                           qui::Tone::Default } });
    }
    rows.push_back({});
    rows.push_back({ { "  Release it to confirm. Esc cancels.",
                       qui::Tone::Dim } });
    if (useController && !preview.hasSelection() && !allowEither) {
        result.error = "this player has no controller - pick one first";
        result.kind = UnifiedCapture::Kind::Cancelled;
        return result;
    }
    con.draw(rows);

    qui::Console::ScanCaptureState scan;
    con.beginScanCapture(scan);
    if (padActive) preview.beginCapture(directionRow);

    const DWORD startedMs = GetTickCount();
    for (;;) {
        if (GetTickCount() - startedMs >= 15000u) {
            result.kind = UnifiedCapture::Kind::Timeout;
            result.error = "nothing pressed";
            return result;
        }
        qui::Console::CapturedScan captured{};
        switch (con.stepScanCapture(scan, captured)) {
            case qui::Console::ScanStep::Committed: {
                if (padActive && !allowEither) break;
                const qinput::DikResolution dik = qinput::dikFromRawScan(
                    {captured.makeCode, captured.extended, false});
                if (!dik.valid) {
                    QLOG("[controls] key REFUSED for %s: make=0x%02X "
                         "extended=%u has no DIK identity",
                         what.c_str(),
                         static_cast<unsigned>(captured.makeCode),
                         captured.extended ? 1u : 0u);
                    result.error =
                        "that key cannot be used - choose another";
                    result.kind = UnifiedCapture::Kind::Cancelled;
                    return result;
                }
                result.kind = UnifiedCapture::Kind::Key;
                result.dik = dik.dik;
                QLOG("[controls] captured key dik=0x%02X '%s' "
                     "(make=0x%02X extended=%u) for %s",
                     static_cast<unsigned>(dik.dik),
                     qinput::dikDisplayLabel(dik.dik),
                     static_cast<unsigned>(captured.makeCode),
                     captured.extended ? 1u : 0u,
                     what.c_str());
                return result;
            }
            case qui::Console::ScanStep::Cancelled:
                result.error = "cancelled";
                result.kind = UnifiedCapture::Kind::Cancelled;
                return result;
            case qui::Console::ScanStep::Pending:
                break;
        }
        if (padActive) {
            qinput::ProfileBinding single{};
            std::string label;
            qinput::MovementBindings cluster{};
            switch (preview.stepCapture(single, label, cluster)) {
                case qinput::ControllerPreview::Step::Committed:
                    if (directionRow
                        && !preview.directionsMustBindIndividually()) {
                        result.kind = UnifiedCapture::Kind::PadCluster;
                        result.cluster = cluster;
                    } else if (
                        sessionRow
                        && single.type
                               == qinput::CompiledBindingType::ProbeAxis
                        && !qinput::probeAxisIsAnalogTrigger(
                               single.probeKind, single.token)) {
                        result.error = "use a button, the d-pad or a trigger "
                                       "here (not a stick)";
                        result.kind = UnifiedCapture::Kind::Cancelled;
                    } else {
                        result.kind = UnifiedCapture::Kind::Pad;
                        result.pad = single;
                        result.padLabel = label;
                    }
                    return result;
                case qinput::ControllerPreview::Step::Failed:
                    result.error = "the controller stopped responding "
                                   "(unplugged?)";
                    result.kind = UnifiedCapture::Kind::PadLost;
                    return result;
                case qinput::ControllerPreview::Step::Pending:
                    break;
            }
        }
        Sleep(8);
    }
}

bool openPreviewWithNotice(qui::Console& con,
                           qinput::ControllerPreview& preview) {
    if (preview.isOpen()) return true;
    con.draw({
        qui::Row{ { "  Controls", qui::Tone::Title } },
        qui::Row{},
        qui::Row{ { "  Detecting controllers...", qui::Tone::Accent } },
        qui::Row{},
        qui::Row{ { "  A DualSense/DualShock can take a few seconds the first "
                    "time.", qui::Tone::Dim } } });
    return preview.open();
}

void controllerPicker(qui::Console& con, qinput::ControllerPreview& preview,
                      std::vector<qinput::PreviewDevice>& devices,
                      qinput::InputProfile& profile, bool& dirty,
                      std::string& note,
                      std::string& pendingStripProfileId) {
    std::string migrationNote;
    for (;;) {
        if (!openPreviewWithNotice(con, preview)) {
            note = "controller support is unavailable on this machine";
            return;
        }
        devices = preview.devices();
        QLOG("[controller.catalog] backend=%s sdl=%s%s%s candidates=%zu",
             qinput::controllerBackendName(),
             qinput::sdlBootstrapResultText(preview.lastBootstrap()),
             preview.backendFailure().empty() ? "" : " backend-failure=",
             preview.backendFailure().c_str(), devices.size());
        for (size_t i = 0; i < devices.size(); ++i) {
            const qinput::PreviewDevice& d = devices[i];
            QLOG("[controller.catalog] #%zu name='%s' backend=%s open=%s "
                 "axes=%u buttons=%u hats=%u vid=%04X pid=%04X ver=%04X "
                 "guid=%s pathHash=%016llX live=%u%s%s%s",
                 i, d.name.c_str(),
                 d.isGamepad ? "sdl-gamepad" : "raw-joystick",
                 d.probeOpened ? "ok" : "FAILED",
                 static_cast<unsigned>(d.axisCount),
                 static_cast<unsigned>(d.buttonCount),
                 static_cast<unsigned>(d.hatCount),
                 d.vendor, d.product, d.version, d.guid.c_str(),
                 static_cast<unsigned long long>(d.pathFingerprint),
                 d.liveHandle,
                 d.probeError.empty() ? "" : " error=",
                 d.probeError.c_str(),
                 qinput::isSteamVirtualController(d.vendor, d.product)
                     ? " steam-virtual=1"
                     : (qinput::isValveInputDevice(d.vendor)
                            ? " valve-device=1" : ""));
        }
        if (migrationNote.empty() && profileHasControllerBindings(profile)
            && (profileHasStandardizedBindings(profile)
                || (!profile.probeBackend.empty()
                    && profileBackendMatchesPreview(profile, preview)))) {
            qinput::BoundDeviceKey bound{};
            bound.hasSerialHash = profile.device.serialHash != 0;
            bound.serialHash = profile.device.serialHash;
            bound.hasPathFingerprint = profile.device.pathFingerprint != 0;
            bound.pathFingerprint = profile.device.pathFingerprint;
            bound.guid = profile.device.guid.c_str();
            bound.vendor = profile.device.vendor;
            bound.product = profile.device.product;

            qinput::DeviceRequirements need{};
            for (const auto& bindings : profile.actions) {
                for (const qinput::ProfileBinding& b : bindings) {
                    const uint16_t wanted =
                        static_cast<uint16_t>(b.token) + 1u;
                    switch (b.probeKind) {
                        case qinput::ProbeKind::GamepadButton:
                        case qinput::ProbeKind::GamepadAxis:
                            need.needsStandardized = true;
                            break;
                        case qinput::ProbeKind::RawButton:
                            if (wanted > need.buttons) need.buttons = wanted;
                            break;
                        case qinput::ProbeKind::RawAxis:
                            if (wanted > need.axes) need.axes = wanted;
                            break;
                        case qinput::ProbeKind::RawHat:
                            if (wanted > need.hats) need.hats = wanted;
                            break;
                        default: break;
                    }
                }
            }

            std::vector<qinput::MigrationCandidate> shortlist;
            shortlist.reserve(devices.size());
            for (const qinput::PreviewDevice& d : devices) {
                qinput::MigrationCandidate c{};
                c.hasSerialHash = d.hasSerial;
                c.serialHash = d.serialHash;
                c.hasPathFingerprint = d.hasPathFingerprint;
                c.pathFingerprint = d.pathFingerprint;
                c.guid = d.guid.c_str();
                c.vendor = d.vendor;
                c.product = d.product;
                c.isGamepad = d.isGamepad;
                c.selectable = d.selectable();
                c.axes = d.axisCount;
                c.buttons = d.buttonCount;
                c.hats = d.hatCount;
                shortlist.push_back(c);
            }
            const qinput::MigrationPlan plan = qinput::planDeviceMigration(
                bound, need, shortlist.data(), shortlist.size());
            QLOG("[controller.migrate] verdict=%d compatible=%zu "
                 "needs=%s/a%u/b%u/h%u boundGuid=%s",
                 static_cast<int>(plan.verdict), plan.compatibleCount,
                 need.needsStandardized ? "standardized" : "raw",
                 static_cast<unsigned>(need.axes),
                 static_cast<unsigned>(need.buttons),
                 static_cast<unsigned>(need.hats),
                 profile.device.guid.c_str());
            if (plan.verdict == qinput::MigrationVerdict::Migrate) {
                const qinput::PreviewDevice& target =
                    devices[plan.candidateIndex];
                if (preview.select(target, !need.needsStandardized)) {
                    preview.applySelectionTo(profile);
                    dirty = true;
                    migrationNote = "your old controller is gone - bindings "
                                    "moved to " + target.name;
                    QLOG("[controller.migrate] adopted name='%s' guid=%s",
                         target.name.c_str(), target.guid.c_str());
                } else {
                    QLOG("[controller.migrate] target '%s' would not open",
                         target.name.c_str());
                }
            } else if (plan.verdict == qinput::MigrationVerdict::Ambiguous) {
                migrationNote = "your old controller is gone and more than "
                                "one could replace it - pick the right one";
            } else if (plan.verdict
                       == qinput::MigrationVerdict::NoCompatibleCandidate) {
                migrationNote = "your old controller is gone and nothing "
                                "here has enough controls for its bindings";
            }
        }

        std::vector<std::string> options;
        for (size_t i = 0; i < devices.size(); ++i) {
            std::string name = devices[i].name;
            size_t sameNameBefore = 0;
            for (size_t j = 0; j < i; ++j)
                if (devices[j].name == devices[i].name) ++sameNameBefore;
            size_t sameNameTotal = 0;
            for (const qinput::PreviewDevice& other : devices)
                if (other.name == devices[i].name) ++sameNameTotal;
            if (sameNameTotal > 1)
                name += " (#" + std::to_string(sameNameBefore + 1) + ")";
            if (preview.hasSelection()
                && preview.selection().guid == devices[i].guid
                && preview.selection().pathFingerprint
                       == devices[i].pathFingerprint) {
                name += "   <- current";
            }
            const std::string shape = devices[i].capabilitySummary();
            if (!shape.empty()) name += "  [" + shape + "]";
            if (qinput::isSteamVirtualController(devices[i].vendor,
                                                 devices[i].product)) {
                name += "  (Steam Input virtual pad)";
            }
            options.push_back(name);
        }
        options.push_back("Look again (rescan)");
        options.push_back("No controller (keyboard only)");
        bool steamVirtualPresent = false;
        bool anyUnopenable = false;
        for (const qinput::PreviewDevice& d : devices) {
            if (qinput::isSteamVirtualController(d.vendor, d.product))
                steamVirtualPresent = true;
            else if (!d.probeOpened) anyUnopenable = true;
        }
        if (!migrationNote.empty()) note = migrationNote;
        const std::string& backendFailure = preview.backendFailure();
        const std::string title =
            !backendFailure.empty()
                ? ("No controller can be opened: " + backendFailure)
                : (steamVirtualPresent && anyUnopenable
                       ? std::string("Steam Input appears to have taken your "
                                     "controller - turn it off for this pad, "
                                     "or pick the Steam virtual pad below")
                       : (devices.empty()
                              ? "No controller detected - plug one in"
                              : "Which controller is yours?"));
        const int choice = con.menu(
            title.c_str(),
            options, "Back");
        if (choice == qui::kBack) return;
        if (choice == static_cast<int>(devices.size())) continue;
        if (choice == static_cast<int>(devices.size()) + 1) {
            preview.deselect();
            profile.device = qinput::ProfileDevice{};
            profile.mapping.clear();
            profile.probeBackend.clear();
            profile.sourceMode = qinput::CompiledSourceMode::Keyboard;
            if (profileHasControllerBindings(profile)) {
                stripControllerBindings(profile);
                note = "keyboard only - controller bindings removed";
            } else {
                note = "keyboard only";
            }
            dirty = true;
            return;
        }
        if (choice >= 0 && choice < static_cast<int>(devices.size())) {
            qinput::PreviewDevice picked =
                devices[static_cast<size_t>(choice)];
            if (!picked.selectable()) {
                note = picked.name + " cannot be used: "
                     + picked.capabilitySummary();
                return;
            }
            std::vector<qinput::PreviewDevice> tied;
            for (const qinput::PreviewDevice& other : devices) {
                if (qinput::ControllerPreview::indistinguishable(picked,
                                                                other)) {
                    tied.push_back(other);
                }
            }
            if (tied.size() > 1) {
                con.draw({ qui::Row{ { "  Which controller?",
                                       qui::Tone::Title } },
                           qui::Row{},
                           qui::Row{ { "  Two identical controllers are "
                                       "connected and nothing else can",
                                       qui::Tone::Default } },
                           qui::Row{ { "  tell them apart.",
                                       qui::Tone::Default } },
                           qui::Row{},
                           qui::Row{ { "  Press a button on the controller "
                                       "you want to use.",
                                       qui::Tone::Accent } },
                           qui::Row{},
                           qui::Row{ { "  Esc cancels.", qui::Tone::Dim } } });
                if (!preview.beginIdentify(tied)) {
                    note = "could not watch both controllers at once - "
                           "unplug one and try again";
                    return;
                }
                size_t winner = 0;
                bool identified = false;
                qui::Console::ScanCaptureState scan;
                con.beginScanCapture(scan);
                const DWORD startedMs = GetTickCount();
                for (;;) {
                    qui::Console::CapturedScan captured{};
                    if (con.stepScanCapture(scan, captured)
                        == qui::Console::ScanStep::Cancelled) {
                        break;
                    }
                    if (GetTickCount() - startedMs >= 15000u) break;
                    const auto step = preview.stepIdentify(winner);
                    if (step == qinput::ControllerPreview::Identify::
                                    Identified) {
                        identified = true;
                        break;
                    }
                    if (step
                        == qinput::ControllerPreview::Identify::Failed) {
                        break;
                    }
                    Sleep(8);
                }
                preview.endIdentify();
                if (!identified) {
                    note = "no controller identified - nothing was changed";
                    return;
                }
                picked = tied[winner];
            }
            bool bindRaw = false;
            if (picked.isGamepad) {
                const bool adapterBox =
                    qinput::isMultiConsoleAdapterBox(picked.vendor);
                std::vector<std::string> howOptions;
                if (adapterBox) {
                    howOptions.push_back(
                        "By position (recommended for this adapter)");
                    howOptions.push_back(
                        "Standardized buttons (Windows has a layout, but it "
                        "describes the adapter, not your pad)");
                } else {
                    howOptions.push_back(
                        "Standardized buttons (recommended)");
                    howOptions.push_back(
                        "By position (custom / adapter layout)");
                }
                const int how = con.menu(
                    adapterBox
                        ? "How should this adapter be bound? Windows has a "
                          "layout for it, but the buttons it reports belong "
                          "to whatever console pad is plugged in, so that "
                          "layout is usually wrong for your pad."
                        : "How should this controller be bound? Standardized "
                          "names survive replugging and port changes. "
                          "Binding by position uses the current backend's "
                          "raw layout.",
                    howOptions, "Back");
                if (how == qui::kBack) return;
                bindRaw = adapterBox ? (how == 0) : (how == 1);
            }
            const bool nextSemantic = picked.isGamepad && !bindRaw
                && preview.usingSdlBackend();
            const std::string nextBackend = preview.usingSdlBackend()
                ? "sdl" : "dinput8";
            const bool currentSemantic = !profile.mapping.empty()
                || profileHasStandardizedBindings(profile);
            const bool sameRawClass = profile.device.guid == picked.guid
                || (profile.device.vendor != 0 && profile.device.product != 0
                    && profile.device.vendor == picked.vendor
                    && profile.device.product == picked.product);
            const bool clearOldBindings = profileHasControllerBindings(profile)
                && (currentSemantic != nextSemantic
                    || (!nextSemantic
                        && (profile.probeBackend != nextBackend
                            || !sameRawClass)));
            if (clearOldBindings) {
                const int replace = con.menu(
                    "This changes the controller's binding layout. Existing "
                    "controller bindings cannot be interpreted safely in "
                    "the new layout, so they are replaced by the first "
                    "control you bind.",
                    {"Continue - the old bindings are kept until then"},
                    "Cancel");
                if (replace != 0) return;
            }
            if (preview.select(picked, bindRaw)) {
                if (clearOldBindings) pendingStripProfileId = profile.id;
                dirty = true;
                QLOG("[controller.select] opened name='%s' backend=%s "
                     "semantic=%d profileDevice='%s' guid=%s pathHash=%016llX "
                     "sourceMode=%u",
                     picked.name.c_str(),
                     bindRaw ? "raw-joystick (chosen)"
                             : (picked.isGamepad ? "sdl-gamepad"
                                                 : "raw-joystick"),
                     preview.semantic() ? 1 : 0,
                     profile.device.normalizedName.c_str(),
                     profile.device.guid.c_str(),
                     static_cast<unsigned long long>(
                         profile.device.pathFingerprint),
                     static_cast<unsigned>(profile.sourceMode));
                note = (clearOldBindings
                            ? "old controller bindings cleared; "
                            : std::string())
                     + "your controller: " + picked.name + " ("
                     + picked.capabilitySummary() + ", bound "
                     + (preview.semantic() ? "by name" : "by position") + ")";
                return;
            }
            QLOG("[controller.select] FAILED to open name='%s' backend=%s "
                 "live=%u axes=%u buttons=%u hats=%u - the device enumerated "
                 "and probed open, so this is the real open path failing",
                 picked.name.c_str(),
                 picked.isGamepad ? "sdl-gamepad" : "raw-joystick",
                 picked.liveHandle,
                 static_cast<unsigned>(picked.axisCount),
                 static_cast<unsigned>(picked.buttonCount),
                 static_cast<unsigned>(picked.hatCount));
            note = "could not open " + picked.name
                 + " - is another program using it?";
            return;
        }
    }
}

std::string modeBindingList(
        const std::vector<qinput::ProfileBinding>& bindings) {
    if (bindings.empty()) return "not set";
    std::string out;
    for (const qinput::ProfileBinding& binding : bindings) {
        if (!out.empty()) out += " + ";
        if (isKeyboardBinding(binding)) {
            out += qinput::dikDisplayLabel(binding.dik);
        } else {
            std::string label = qinput::controllerBindingLabel(binding);
            out += label.empty() ? "controller" : label;
        }
    }
    return out;
}

std::string modeConflictDescription(
        const qinput::ControlsFile& file,
        const std::vector<qinput::ModeControlConflict>& conflicts) {
    std::string out;
    for (const qinput::ModeControlConflict& conflict : conflicts) {
        if (!out.empty()) out += ", ";
        if (conflict.fighter) {
            out += file.profiles[conflict.profileIndex].name + " / ";
            out += kActionLabels[conflict.fightAction];
        } else {
            const qinput::ModeControlDescriptor* descriptor =
                qinput::modeControlDescriptor(conflict.modeAction);
            out += descriptor != nullptr ? descriptor->displayName
                                         : "unknown mode action";
        }
    }
    return out;
}

std::string modeControlRowStatus(
        const qinput::ControlsFile& file,
        const qinput::ModeControlDescriptor& descriptor) {
    const auto& bindings = file.modeControls[
        static_cast<size_t>(descriptor.action)];
    bool keyboard = false;
    bool controller = false;
    bool conflict = false;
    for (const qinput::ProfileBinding& binding : bindings) {
        keyboard |= isKeyboardBinding(binding);
        controller |= !isKeyboardBinding(binding);
        conflict |= !qinput::findModeControlConflicts(
                         file, descriptor.action, binding).empty();
    }
    const char* device = keyboard && controller ? "keyboard+controller"
        : controller ? "controller" : keyboard ? "keyboard" : "unbound";
    const char* capability = qinput::modeControlRuntimeCapabilityReason(
        descriptor.action, true, true);
    return std::string(device) + "; conflict: "
        + (conflict ? "YES" : "none") + "; runtime: "
        + (capability != nullptr ? capability : "ready");
}

std::vector<qinput::ModeControlAction> fighterModeConflicts(
        const qinput::ControlsFile& file,
        const qinput::ProfileBinding& binding) {
    std::vector<qinput::ModeControlAction> result;
    for (const qinput::ModeControlDescriptor& descriptor
         : qinput::kModeControlDescriptors) {
        if (!qinput::modeDomainOverlapsFighter(descriptor.domain)) continue;
        for (const qinput::ProfileBinding& candidate : file.modeControls[
             static_cast<size_t>(descriptor.action)]) {
            if (qinput::samePhysicalBinding(candidate, binding)) {
                result.push_back(descriptor.action);
                break;
            }
        }
    }
    return result;
}

bool moveBindingFromModesToFighter(
        qui::Console& con, qinput::ControlsFile& file,
        const qinput::ProfileBinding& binding, const std::string& destination) {
    const std::vector<qinput::ModeControlAction> conflicts =
        fighterModeConflicts(file, binding);
    if (conflicts.empty()) return true;
    std::string owners;
    for (qinput::ModeControlAction action : conflicts) {
        if (!owners.empty()) owners += ", ";
        const qinput::ModeControlDescriptor* descriptor =
            qinput::modeControlDescriptor(action);
        owners += descriptor != nullptr ? descriptor->displayName : "mode control";
    }
    if (con.menu("Binding is already used by " + owners,
                 {"Move binding to " + destination}, "Cancel") != 0) {
        return false;
    }
    for (qinput::ModeControlAction action : conflicts) {
        auto& bindings = file.modeControls[static_cast<size_t>(action)];
        bindings.erase(std::remove_if(
            bindings.begin(), bindings.end(),
            [&](const qinput::ProfileBinding& candidate) {
                return qinput::samePhysicalBinding(candidate, binding);
            }), bindings.end());
    }
    return true;
}

bool resolveModeMigrations(qui::Console& con, qinput::ControlsFile& file) {
    bool changed = false;
    while (!file.pendingModeMigrations.empty()) {
        const qinput::ModeControlMigrationChoice choice =
            file.pendingModeMigrations.front();
        const qinput::ModeControlDescriptor* descriptor =
            qinput::modeControlDescriptor(choice.action);
        std::vector<int> players;
        std::vector<std::string> labels;
        for (int player = 0; player < 4; ++player) {
            if (choice.playerBindings[player].empty()) continue;
            players.push_back(player);
            labels.push_back("Use Player " + std::to_string(player + 1)
                + ": " + modeBindingList(choice.playerBindings[player]));
        }
        const int picked = con.menu(
            std::string("Choose migrated binding: ")
                + (descriptor != nullptr ? descriptor->displayName
                                         : "mode control"),
            labels, "Decide later");
        if (picked < 0 || picked >= static_cast<int>(players.size()))
            return changed;
        file.modeControls[static_cast<size_t>(choice.action)].clear();
        for (const qinput::ProfileBinding& binding :
             choice.playerBindings[players[picked]]) {
            std::string error;
            if (!qinput::moveModeControlBinding(file, choice.action, binding,
                                                error)) {
                con.draw({{{"  Migration failed: " + error,
                            qui::Tone::Bad}}});
                Sleep(1000);
                return changed;
            }
        }
        file.pendingModeMigrations.erase(file.pendingModeMigrations.begin());
        changed = true;
    }
    return changed;
}

bool modeControlsMenu(qui::Console& con, qinput::ControlsFile& file,
                      qinput::ControllerPreview& preview,
                      bool selectedPlayerUsesController) {
    bool changed = resolveModeMigrations(con, file);
    struct DomainItem {
        qinput::ModeControlDomain domain;
        const char* label;
    };
    static constexpr DomainItem domains[] = {
        {qinput::ModeControlDomain::WrapperGlobal, "Wrapper global"},
        {qinput::ModeControlDomain::OnlinePlayer, "Online player"},
        {qinput::ModeControlDomain::Replay, "Replay"},
        {qinput::ModeControlDomain::Training, "Training"},
    };
    for (;;) {
        std::vector<std::string> domainLabels;
        for (const DomainItem& domain : domains) domainLabels.push_back(domain.label);
        const int domainIndex = con.menu("Mode controls", domainLabels,
                                         "Back to player controls");
        if (domainIndex < 0 || domainIndex >= 4) return changed;
        for (;;) {
            std::vector<const qinput::ModeControlDescriptor*> actions;
            std::vector<std::string> labels;
            for (const qinput::ModeControlDescriptor& descriptor
                 : qinput::kModeControlDescriptors) {
                if (descriptor.domain != domains[domainIndex].domain) continue;
                actions.push_back(&descriptor);
                const auto& bindings = file.modeControls[
                    static_cast<size_t>(descriptor.action)];
                labels.push_back(std::string(descriptor.displayName) + "  ["
                    + modeBindingList(bindings) + "]  {"
                    + modeControlRowStatus(file, descriptor) + "}");
            }
            const int actionIndex = con.menu(domains[domainIndex].label, labels,
                                             "Back to mode groups");
            if (actionIndex < 0
                || actionIndex >= static_cast<int>(actions.size())) break;
            const qinput::ModeControlDescriptor& descriptor =
                *actions[actionIndex];
            auto& current = file.modeControls[
                static_cast<size_t>(descriptor.action)];
            std::vector<std::string> operations;
            operations.push_back(current.size() < 4
                ? "Add binding alias" : "Add binding alias (limit reached)");
            operations.push_back("Clear every binding");
            const int operation = con.menu(descriptor.displayName, operations,
                                           "Back");
            if (operation < 0) continue;
            if (operation == 1) {
                if (!current.empty()) { current.clear(); changed = true; }
                continue;
            }
            if (current.size() >= 4) continue;
            const bool controllerAllowed = descriptor.devicePolicy
                == qinput::ModeControlDevicePolicy::
                       KeyboardOrSelectedControllerDigital;
            UnifiedCapture captured = captureUnified(
                con, preview, descriptor.displayName, false, true,
                controllerAllowed && selectedPlayerUsesController,
                controllerAllowed);
            qinput::ProfileBinding binding{};
            if (captured.kind == UnifiedCapture::Kind::Key) {
                binding.type = qinput::CompiledBindingType::KeyboardScan;
                binding.dik = captured.dik;
            } else if (captured.kind == UnifiedCapture::Kind::Pad) {
                binding = captured.pad;
            } else {
                continue;
            }
            std::string validation;
            if (!qinput::validModeControlBinding(descriptor.action, binding,
                                                 validation)) {
                con.draw({{{"  Cannot bind: " + validation, qui::Tone::Bad}}});
                Sleep(1000);
                continue;
            }
            const std::vector<qinput::ModeControlConflict> conflicts =
                qinput::findModeControlConflicts(file, descriptor.action,
                                                 binding);
            if (!conflicts.empty()) {
                const std::string owners = modeConflictDescription(file,
                                                                   conflicts);
                const int move = con.menu(
                    "Binding is already used by " + owners,
                    {"Move binding to " + std::string(descriptor.displayName)},
                    "Cancel");
                if (move != 0) continue;
            }
            if (qinput::moveModeControlBinding(file, descriptor.action,
                                               binding, validation)) {
                changed = true;
            }
        }
    }
}

void controlsMenu(qui::Console& con) {
    std::string pendingStripProfileId;
    int saveBlockingPlayer = -1;
    qinput::ControlsFile file{};
    std::string loadWarning;
    bool originalExisted = false;
    bool originalReadable = true;
    std::string originalBytes;
    std::string loadError;
    if (!readWholeFileState(kControlsFileName, originalExisted, originalBytes,
                            loadError)) {
        originalReadable = false;
        file = qinput::makeDefaultControlsFile();
        loadWarning = "Saved controls are inaccessible (" + loadError
                    + "). The original will not be replaced.";
        QLOG("[controls] existing %s could not be read: %s",
             kControlsFileName, loadError.c_str());
    } else if (originalExisted
               && !qinput::parseControlsFile(originalBytes, file,
                                             loadError)) {
        originalReadable = false;
        file = qinput::makeDefaultControlsFile();
        loadWarning = "Saved controls were rejected (" + loadError
                    + "). The original is untouched; recovery needs "
                      "confirmation.";
        QLOG("[controls] existing %s rejected: %s",
             kControlsFileName, loadError.c_str());
    }
    if (file.profiles.empty()) {
        file = qinput::makeDefaultControlsFile();
    }

    bool dirty = false;
    std::string note;
    int selection = 0;
    int editPlayer = 0;
    qinput::sdlWarmupAsync();
    qinput::ControllerPreview preview;
    std::vector<qinput::PreviewDevice> devices;
    int previewPlayer = -1;
    bool recoveryApproved = false;

    const auto saveMenu = [&](std::string& error) -> bool {
        if (!dirty) return true;
        if (!originalReadable && !recoveryApproved) {
            const int choice = con.menu(
                "The existing controls file cannot be read. Rebuilding can "
                "discard profiles it contains, including Players 2-4. Its "
                "exact bytes will be kept in a separate recovery file.",
                {"Keep editing; do not replace the file",
                 "Back up the unreadable file and rebuild all profiles"},
                "Keep editing; do not replace the file");
            if (choice != 1) {
                error = "unreadable controls were not replaced";
                return false;
            }
            recoveryApproved = true;
        }
        for (qinput::InputProfile& profile : file.profiles)
            normalizeProfile(profile);
        return saveControlsFileAtomic(file, originalExisted, originalBytes,
                                      !originalReadable, error,
                                      &saveBlockingPlayer);
    };

    for (;;) {
        if (activeProfileOf(file) == nullptr) {
            file.profiles.push_back(qinput::makeDefaultKeyboardProfile());
            file.activeProfileId = file.profiles.front().id;
            file.activeByPlayer[0] = file.activeProfileId;
            continue;
        }
        qinput::InputProfile* active = playerProfileOf(file, editPlayer);
        if (active == nullptr) {
            file.activeByPlayer[0].clear();
            continue;
        }
        const bool usesController = profileUsesController(*active);

        if (previewPlayer != editPlayer) {
            previewPlayer = editPlayer;
            preview.deselect();
            if (usesController && !active->device.guid.empty()
                && openPreviewWithNotice(con, preview)
                && profileBackendMatchesPreview(*active, preview)) {
                devices = preview.devices();
                qinput::CompiledDeviceMatcher matcher{};
                std::snprintf(matcher.guid, sizeof(matcher.guid), "%s",
                              active->device.guid.c_str());
                matcher.vendor = active->device.vendor;
                matcher.product = active->device.product;
                matcher.version = active->device.version;
                std::snprintf(matcher.normalizedName,
                              sizeof(matcher.normalizedName), "%s",
                              active->device.normalizedName.c_str());
                if (!active->device.serial.empty()) {
                    matcher.hasSerial = 1;
                    matcher.serialHash = qinput::identityStringHash(
                        active->device.serial.c_str());
                } else if (active->device.serialHash != 0) {
                    matcher.hasSerial = 1;
                    matcher.serialHash = active->device.serialHash;
                }
                if (active->device.pathFingerprint != 0) {
                    matcher.hasPathFingerprint = 1;
                    matcher.pathFingerprint = active->device.pathFingerprint;
                }
                std::vector<qinput::DeviceCandidate> candidates;
                candidates.reserve(devices.size());
                for (const qinput::PreviewDevice& device : devices) {
                    qinput::DeviceCandidate candidate{};
                    candidate.liveHandle = device.liveHandle;
                    std::snprintf(candidate.guid, sizeof(candidate.guid),
                                  "%s", device.guid.c_str());
                    candidate.vendor = device.vendor;
                    candidate.product = device.product;
                    candidate.version = device.version;
                    std::snprintf(candidate.normalizedName,
                                  sizeof(candidate.normalizedName), "%s",
                                  device.name.c_str());
                    candidate.hasSerial = device.hasSerial;
                    candidate.serialHash = device.serialHash;
                    candidate.hasPathFingerprint = device.hasPathFingerprint;
                    candidate.pathFingerprint = device.pathFingerprint;
                    candidates.push_back(candidate);
                }
                const qinput::IdentityResolution resolution =
                    qinput::resolveDeviceIdentity(
                        matcher, candidates.data(),
                        static_cast<uint32_t>(candidates.size()));
                if (resolution.outcome == qinput::IdentityOutcome::Unique) {
                    const bool raw = active->mapping.empty();
                    (void)preview.select(
                        devices[resolution.candidateIndex], raw,
                        raw ? nullptr : active->mapping.data(),
                        raw ? 0u : static_cast<uint32_t>(
                            active->mapping.size()));
                } else {
                    QLOG("[controller.reconnect] not reopening saved device: "
                         "%s (candidates=%zu guid=%s)",
                         resolution.outcome
                                 == qinput::IdentityOutcome::Ambiguous
                             ? "two or more are indistinguishable"
                             : "none of the connected devices match",
                         devices.size(), active->device.guid.c_str());
                }
            }
        }

        std::vector<ControlsRow> map;
        qui::ScreenSpec spec;
        spec.reportClearKey = true;
        spec.above.push_back({ { "  Controls", qui::Tone::Title } });
        spec.above.push_back(
            { { "  " + std::string(62, '-'), qui::Tone::Dim } });
        if (!loadWarning.empty()) {
            spec.above.push_back({ { "  " + loadWarning, qui::Tone::Bad } });
        }
        spec.above.push_back({
            { "  Changes apply to the next game you start.",
              qui::Tone::Dim },
            { dirty ? "   (unsaved changes)" : "", qui::Tone::Warn } });
        spec.above.push_back({});

        auto pushHeader = [&](const std::string& title,
                              const std::string& columns = std::string()) {
            map.push_back({ControlsRow::Kind::Header, -1});
            spec.headers.push_back(spec.items.size());
            qui::Row row{ { padded("  " + title, 26), qui::Tone::Accent } };
            if (!columns.empty())
                row.push_back({ columns, qui::Tone::Dim });
            spec.items.push_back(std::move(row));
        };
        auto pushSpacer = [&]() {
            map.push_back({ControlsRow::Kind::Header, -1});
            spec.items.push_back({});
        };
        auto pushOption = [&](ControlsRow::Kind kind, int index,
                              const std::string& label,
                              const std::string& value,
                              qui::Tone valueTone = qui::Tone::Default) {
            map.push_back({kind, index});
            spec.items.push_back({
                { padded(label, 26), qui::Tone::Default },
                { value, valueTone } });
        };
        auto pushBindingRow = [&](ControlsRow::Kind kind, int index,
                                  const std::string& label,
                                  const std::vector<qinput::ProfileBinding>&
                                      bindings,
                                  bool required) {
            const bool bound = bindingRowIsBound(bindings);
            map.push_back({kind, index});
            spec.items.push_back({
                { padded(label, 26), qui::Tone::Default },
                { bindingRowText(bindings, usesController, required),
                  bound ? qui::Tone::Good
                        : required ? qui::Tone::Bad
                                   : qui::Tone::Dim } });
        };

        pushOption(ControlsRow::Kind::Player, 0, "Setting up",
                   "Player " + std::to_string(editPlayer + 1)
                       + (editPlayer == 0
                              ? "  (you; online and offline)"
                              : "  (offline on this PC)"),
                   qui::Tone::Accent);
        pushOption(ControlsRow::Kind::Source, 0, "Plays with",
                   sourceRowText(*active,
                                 preview.hasSelection()
                                     ? preview.selection().name
                                     : std::string()),
                   qui::Tone::Accent);
        pushOption(ControlsRow::Kind::MapAll, 0, "Set up everything",
                   "press each control in turn", qui::Tone::Dim);
        for (int action = 0; action < 8; ++action)
            pushBindingRow(ControlsRow::Kind::Action, action,
                           std::string("  ") + kActionLabels[action],
                           active->actions[action], editPlayer == 0);
        if (editPlayer == 0)
            pushOption(ControlsRow::Kind::ModeControls, 0,
                       "  Mode controls", "wrapper / online / replay / training",
                       qui::Tone::Accent);
        pushOption(ControlsRow::Kind::Socd, 0, "  Left+Right together",
                   kSocdLabels[static_cast<int>(active->horizontalSocd) & 3]);
        pushOption(ControlsRow::Kind::Focus, 0, "  Play when unfocused",
                   active->focusPolicy
                           == qinput::FocusPolicy::AllowBackground
                       ? "yes (offline and testing only)" : "no");
        pushOption(ControlsRow::Kind::Reset, 0, "  Reset to defaults",
                   "arrow keys + Z X C V", qui::Tone::Dim);
        pushOption(ControlsRow::Kind::Save, 0, "  Save and return",
                   dirty ? "you have unsaved changes" : "",
                   dirty ? qui::Tone::Warn : qui::Tone::Dim);

        if (!note.empty())
            spec.below.push_back({ { "  " + note, qui::Tone::Warn } });
        spec.below.push_back({
            { "  Enter set - Delete clear - Left/Right change/pick player - "
              "Esc save and back", qui::Tone::Dim } });
        spec.below.push_back({
            { "  Mode controls are global and apply after Save on the next launch.",
              qui::Tone::Dim } });

        const int result = con.menuTick(spec, selection, 30000);
        if (result == qui::kTimeout) continue;
        if (result == qui::kBack) {
            if (!dirty) { preview.close(); return; }
            std::string error;
            if (saveMenu(error)) {
                QLOG("[controls] saved on exit: %s", kControlsFileName);
                preview.close();
                return;
            }
            QLOG("[controls] NOT SAVED on exit: %s", error.c_str());
            std::vector<std::string> stuck;
            stuck.push_back("Go back and fix it");
            stuck.push_back("Leave without saving (your changes are lost)");
            const int choice = con.menu(
                "These controls cannot be saved: " + error, stuck, "");
            if (choice == 1) {
                QLOG("[controls] left the page without saving after a "
                     "refused save: %s", error.c_str());
                preview.close();
                return;
            }
            note = "not saved: " + error;
            continue;
        }
        if (result < 0 && result != qui::kClear && result != qui::kLeft
            && result != qui::kRight) {
            continue;
        }
        const int row = selection;
        if (row < 0 || row >= static_cast<int>(map.size())) continue;
        const ControlsRow entry = map[static_cast<size_t>(row)];
        if (entry.kind == ControlsRow::Kind::Header) continue;
        note.clear();

        if (result == qui::kClear) {
            if (entry.kind == ControlsRow::Kind::Action) {
                active->actions[entry.index].clear();
                note = std::string(kActionLabels[entry.index])
                     + " cleared - set it again before playing";
                dirty = true;
            }
            continue;
        }

        if (result == qui::kLeft || result == qui::kRight) {
            const int delta = result == qui::kRight ? 1 : 3;
            if (entry.kind == ControlsRow::Kind::Player) {
                editPlayer = (editPlayer + (result == qui::kRight ? 1 : 3))
                    & 3;
                selection = row;
                continue;
            }
            if (entry.kind == ControlsRow::Kind::Socd) {
                active->horizontalSocd = static_cast<qinput::SocdPolicy>(
                    (static_cast<int>(active->horizontalSocd) + delta) & 3);
                dirty = true;
            } else if (entry.kind == ControlsRow::Kind::Focus) {
                active->focusPolicy =
                    active->focusPolicy
                            == qinput::FocusPolicy::GameWindowOnly
                        ? qinput::FocusPolicy::AllowBackground
                        : qinput::FocusPolicy::GameWindowOnly;
                dirty = true;
            }
            continue;
        }

        // Old bindings must go BEFORE the new one is written, or removeControllerBinding would strip the binding just committed.
        auto payPendingStrip = [&](qinput::InputProfile& profile) {
            if (pendingStripProfileId.empty()
                || pendingStripProfileId != profile.id) {
                return;
            }
            pendingStripProfileId.clear();
            stripControllerBindings(profile);
            QLOG("[controls] layout change applied: profile '%s' lost its "
                 "old controller bindings now that a new one replaced them",
                 profile.name.c_str());
        };
        auto bindOne = [&](int action,
                           bool* clusterSet = nullptr) -> bool {
            const bool directionRow = action >= 0 && action < 4;
            const std::string what = kActionLabels[action];
            UnifiedCapture captured = captureUnified(
                con, preview, what, directionRow, false,
                usesController);
            switch (captured.kind) {
                case UnifiedCapture::Kind::Key: {
                    qinput::ProfileBinding binding{};
                    binding.type = qinput::CompiledBindingType::KeyboardScan;
                    binding.dik = captured.dik;
                    if (!moveBindingFromModesToFighter(
                            con, file, binding, what)) return false;
                    const std::string owner = stealDikAcrossPlayers(
                        file, captured.dik, editPlayer, action);
                    auto& bindings = active->actions[action];
                    replaceKeyboardSlot(bindings, captured.dik);
                    dirty = true;
                    note = std::string(
                               qinput::dikDisplayLabel(captured.dik))
                         + " -> " + what;
                    if (!owner.empty())
                        note += "   (taken from " + owner + ")";
                    return true;
                }
                case UnifiedCapture::Kind::Pad: {
                    if (!moveBindingFromModesToFighter(
                            con, file, captured.pad, what)) return false;
                    payPendingStrip(*active);
                    const std::string owner = controllerConflictOwner(
                        *active, captured.pad, action);
                    removeControllerBinding(*active, captured.pad);
                    auto& bindings = active->actions[action];
                    replaceControllerSlot(bindings, captured.pad);
                    preview.applySelectionTo(*active);
                    dirty = true;
                    note = captured.padLabel + " -> " + what;
                    if (!owner.empty())
                        note += "   (taken from " + owner + ")";
                    return true;
                }
                case UnifiedCapture::Kind::PadCluster: {
                    payPendingStrip(*active);
                    removeControllerBinding(*active, captured.cluster.up);
                    removeControllerBinding(*active, captured.cluster.down);
                    removeControllerBinding(*active, captured.cluster.left);
                    removeControllerBinding(*active,
                                            captured.cluster.right);
                    replaceControllerSlot(active->actions[0],
                                          captured.cluster.up);
                    replaceControllerSlot(active->actions[1],
                                          captured.cluster.down);
                    replaceControllerSlot(active->actions[2],
                                          captured.cluster.left);
                    replaceControllerSlot(active->actions[3],
                                          captured.cluster.right);
                    preview.applySelectionTo(*active);
                    dirty = true;
                    note = captured.cluster.label
                         + " now moves in all four directions";
                    if (clusterSet != nullptr) *clusterSet = true;
                    return true;
                }
                case UnifiedCapture::Kind::PadLost:
                case UnifiedCapture::Kind::Cancelled:
                case UnifiedCapture::Kind::Timeout:
                    note = captured.error;
                    return false;
            }
            return false;
        };

        switch (entry.kind) {
            case ControlsRow::Kind::Header:
                break;
            case ControlsRow::Kind::Action:
                (void)bindOne(entry.index);
                break;
            case ControlsRow::Kind::ModeControls:
                if (modeControlsMenu(con, file, preview, usesController)) {
                    dirty = true;
                    previewPlayer = -1;
                }
                break;
            case ControlsRow::Kind::MapAll: {
                int mapped = 0;
                bool stopped = false;
                for (int action = 0; action < 4 && !stopped; ++action) {
                    bool clusterSet = false;
                    if (!bindOne(action, &clusterSet)) {
                        stopped = true;
                        break;
                    }
                    ++mapped;
                    if (clusterSet) { mapped = 4; break; }
                }
                for (int action = 4; action < 8 && !stopped; ++action) {
                    if (!bindOne(action)) { stopped = true; break; }
                    ++mapped;
                }
                note = stopped
                    ? "setup stopped early (" + std::to_string(mapped)
                        + "/8 set) - " + note
                    : "all 8 controls set";
                break;
            }
            case ControlsRow::Kind::Player:
                editPlayer = (editPlayer + 1) & 3;
                break;
            case ControlsRow::Kind::Source:
            case ControlsRow::Kind::Device:
                if (entry.kind == ControlsRow::Kind::Source
                    && usesController) {
                    stripControllerBindings(*active);
                    active->sourceMode =
                        qinput::CompiledSourceMode::Keyboard;
                    active->device = qinput::ProfileDevice{};
                    active->mapping.clear();
                    active->probeBackend.clear();
                    preview.deselect();
                    dirty = true;
                    note = "Player " + std::to_string(editPlayer + 1)
                         + " now plays with the keyboard";
                    break;
                }
                {
                    const bool hadSelection = preview.hasSelection();
                    controllerPicker(con, preview, devices, *active, dirty,
                                     pendingStripProfileId,
                                     note);
                    if (preview.hasSelection()) {
                        for (int other = 0; other < 4; ++other) {
                            if (other == editPlayer) continue;
                            qinput::InputProfile* otherProfile =
                                configuredPlayerProfileOf(file, other);
                            if (otherProfile == nullptr
                                || otherProfile == active
                                || !sameSavedPhysicalDevice(
                                       otherProfile->device,
                                       active->device)) {
                                continue;
                            }
                            stripControllerBindings(*otherProfile);
                            otherProfile->device =
                                qinput::ProfileDevice{};
                            otherProfile->mapping.clear();
                            otherProfile->probeBackend.clear();
                            otherProfile->sourceMode =
                                qinput::CompiledSourceMode::Keyboard;
                            note += "  (taken from Player "
                                 + std::to_string(other + 1) + ")";
                        }
                        active->sourceMode =
                            qinput::CompiledSourceMode::Controller;
                        for (auto& bindings : active->actions)
                            for (size_t i = bindings.size(); i-- > 0;)
                                if (isKeyboardBinding(bindings[i]))
                                    bindings.erase(bindings.begin()
                                                   + static_cast<long>(i));
                        dirty = true;
                    } else if (!hadSelection) {
                    }
                }
                break;
            case ControlsRow::Kind::Socd:
                active->horizontalSocd = static_cast<qinput::SocdPolicy>(
                    (static_cast<int>(active->horizontalSocd) + 1) & 3);
                dirty = true;
                break;
            case ControlsRow::Kind::Focus:
                active->focusPolicy =
                    active->focusPolicy
                            == qinput::FocusPolicy::GameWindowOnly
                        ? qinput::FocusPolicy::AllowBackground
                        : qinput::FocusPolicy::GameWindowOnly;
                dirty = true;
                break;
            case ControlsRow::Kind::Reset:
                if (editPlayer == 0) {
                    const std::string keepId = active->id;
                    *active = qinput::makeDefaultKeyboardProfile();
                    active->id = keepId;
                    note = "reset: arrow keys move, Z X C V attack";
                } else {
                    const std::string keepId = active->id;
                    const std::string keepName = active->name;
                    *active = qinput::InputProfile{};
                    active->id = keepId;
                    active->name = keepName;
                    active->sourceMode =
                        qinput::CompiledSourceMode::Keyboard;
                    note = "Player " + std::to_string(editPlayer + 1)
                         + " cleared (uses the game's own keys again)";
                }
                preview.deselect();
                previewPlayer = -1;
                dirty = true;
                break;
            case ControlsRow::Kind::Save: {
                std::string error;
                if (saveMenu(error)) {
                    QLOG("[controls] saved: %s", kControlsFileName);
                    preview.close();
                    return;
                }
                QLOG("[controls] NOT SAVED: %s", error.c_str());
                if (saveBlockingPlayer >= 0
                    && saveBlockingPlayer != editPlayer) {
                    const std::string who =
                        "Player " + std::to_string(saveBlockingPlayer + 1);
                    const int go = con.menu(
                        "Not saved - " + error,
                        {"Go to " + who + "'s controls and finish them"},
                        "Stay here");
                    if (go == 0) {
                        editPlayer = saveBlockingPlayer;
                        preview.deselect();
                        previewPlayer = -1;
                        note = who + ": finish the unbound controls, then "
                                     "save again";
                        break;
                    }
                }
                note = "not saved: " + error;
                break;
            }
        }
    }
}

}

// QOHcnf.key: 8044-byte SE format, dword@0 = active profile, 764-byte profile blocks from offset 4.
namespace gamecfg_ui {

constexpr size_t kQohCnfSize = 8044;
constexpr size_t kProfileBase = 4;
constexpr size_t kProfileBytes = 764;
constexpr size_t kProfileCount = 10;
constexpr size_t kOffFullscreen = 108;   // dword: 1 = fullscreen
constexpr size_t kOffBgmMode = 60;       // dword: 0 Wave/1 MIDI/3 off/4 CD
constexpr size_t kOffBgmPath = 116;
constexpr size_t kOffFlags = 376;        // bit0 16-bit color, bit1 NoSound, bit2 VSync

std::string qohCnfPathOf(const Settings& s) {
    return dirOf(resolveGamePath(s)) + "\\System\\QOHcnf.key";
}

uint32_t rdDword(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint32_t>(data[offset])
        | (static_cast<uint32_t>(data[offset + 1]) << 8)
        | (static_cast<uint32_t>(data[offset + 2]) << 16)
        | (static_cast<uint32_t>(data[offset + 3]) << 24);
}
void wrDword(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset] = static_cast<uint8_t>(value & 0xFF);
    data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

const char* bgmModeName(uint32_t mode) {
    switch (mode) {
        case 0: return "Wave (normal - plays from System\\Bgm)";
        case 1: return "MIDI (synthesizer)";
        case 3: return "OFF - no music at all";
        case 4: return "CD audio - SILENT without the game disc";
        default: return "unknown";
    }
}
uint32_t nextBgmMode(uint32_t mode) {
    switch (mode) {
        case 0: return 1;
        case 1: return 4;
        case 4: return 3;
        default: return 0;
    }
}

void gameConfigMenu(qui::Console& con, const Settings& s) {
    const std::string path = qohCnfPathOf(s);
    std::vector<uint8_t> data;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            con.message("Game config not found",
                        "Could not open\n  " + path
                            + "\nCheck the game path in Settings.");
            return;
        }
        data.assign(std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>());
    }
    if (data.size() != kQohCnfSize) {
        con.message("Unrecognized config format",
                    "This QOHcnf.key is " + std::to_string(data.size())
                        + " bytes; the editor knows the "
                        + std::to_string(kQohCnfSize)
                        + "-byte SE format only.\nStart the game once so "
                          "it rewrites the file, then try again.");
        return;
    }

    bool dirty = false;
    for (;;) {
        uint32_t active = rdDword(data, 0);
        if (active >= kProfileCount) active = 0;
        const size_t base = kProfileBase + active * kProfileBytes;
        const uint32_t bgmMode = rdDword(data, base + kOffBgmMode);
        const uint32_t fullscreen = rdDword(data, base + kOffFullscreen);
        const uint8_t flags = data[base + kOffFlags];
        std::string bgmPath;
        for (size_t i = 0; i < 96; ++i) {
            const char c =
                static_cast<char>(data[base + kOffBgmPath + i]);
            if (c == '\0') break;
            bgmPath += c;
        }

        const int c = con.menu(
            std::string("Game config  (System\\QOHcnf.key)")
                + (dirty ? "   [unsaved]" : ""),
            {
                "Profile in use:       " + std::to_string(active + 1)
                    + " of 10",
                "Fullscreen:           "
                    + std::string(fullscreen
                          ? "yes (normal - resizable window + F11)"
                          : "NO - tiny FIXED window, no resize/F11!"),
                "Music:                "
                    + std::string(bgmModeName(bgmMode)),
                "All sound (NoSound):  "
                    + std::string((flags & 0x02) ? "EVERYTHING MUTED"
                                                 : "normal"),
                "16-bit color:         "
                    + std::string((flags & 0x01) ? "yes" : "no"),
                "VSync:                "
                    + std::string((flags & 0x04) ? "on" : "off"),
                "Music folder:         "
                    + (bgmPath.empty() ? "<unset>" : bgmPath)
                    + "   (read-only)",
                "Save to QOHcnf.key",
            },
            "Back");
        switch (c) {
            case 0:
                wrDword(data, 0, (active + 1) % kProfileCount);
                dirty = true;
                break;
            case 1:
                wrDword(data, base + kOffFullscreen,
                        fullscreen ? 0u : 1u);
                dirty = true;
                break;
            case 2:
                wrDword(data, base + kOffBgmMode, nextBgmMode(bgmMode));
                dirty = true;
                break;
            case 3:
                data[base + kOffFlags] =
                    static_cast<uint8_t>(flags ^ 0x02);
                dirty = true;
                break;
            case 4:
                data[base + kOffFlags] =
                    static_cast<uint8_t>(flags ^ 0x01);
                dirty = true;
                break;
            case 5:
                data[base + kOffFlags] =
                    static_cast<uint8_t>(flags ^ 0x04);
                dirty = true;
                break;
            case 6:
                break;
            case 7: {
                // The game rewrites this file when it EXITS, so edits made while it runs would be overwritten.
                CopyFileA(path.c_str(), (path + ".caster.bak").c_str(),
                          FALSE);
                const std::string tmp = path + ".tmp";
                {
                    std::ofstream out(tmp,
                                      std::ios::binary | std::ios::trunc);
                    if (!out) {
                        con.message("Not saved",
                                    "Could not write a temporary file "
                                    "next to QOHcnf.key.");
                        break;
                    }
                    out.write(reinterpret_cast<const char*>(data.data()),
                              static_cast<std::streamsize>(data.size()));
                    if (!out.flush()) {
                        con.message("Not saved",
                                    "Write to the temporary file failed.");
                        break;
                    }
                }
                if (!MoveFileExA(tmp.c_str(), path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING
                                     | MOVEFILE_WRITE_THROUGH)) {
                    con.message("Not saved",
                                "Could not replace QOHcnf.key (is the "
                                "game running?).");
                    break;
                }
                dirty = false;
                con.status("Saved. Previous file kept as "
                           "QOHcnf.key.caster.bak");
                break;
            }
            case qui::kBack:
                if (dirty
                    && con.menu("Discard unsaved game-config changes?",
                                { "Discard them" }, "Keep editing") != 0)
                    break;
                return;
            default: break;
        }
    }
}

}

// <gameDir>\LocalConfig\FCommand.Txt is read at boot only when a 10-byte ConfigCheckFile.dmy sits next to it. Keys are case-sensitive whitespace-separated "Key Value" tokens; // starts a comment.
namespace fcmd_ui {

struct State {
    int extraKeys = -1;
    int systemPause = -1;
    int debugWindow = -1;
    int noSound = -1;
    int vsync = -1;
    int waitTimeMode = -1;
    int printMaster = 0;
    int printDec = 0;
    int comVsAsobi = 0;
    long setWaitTime = -1;
    long waitCount = -1;
    long extraSkip = -1;
    std::string priorityTop;
    std::string bgmPlayType;
    std::string bgmPath;
};

std::string dirPathOf(const Settings& s) {
    return dirOf(resolveGamePath(s)) + "\\LocalConfig";
}

std::vector<std::string> tokenizeLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::string cur;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
        if (c == ' ' || c == '\t' || c == '\r') {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

struct Line {
    std::string text;
    std::string managedKey;
};

int parseBool(const std::string& v) {
    if (v == "TRUE") return 1;
    if (v == "FALSE") return 0;
    return -1;
}

void parseFile(const std::string& text, State& st, std::vector<Line>& lines,
               bool& hadNetToken) {
    hadNetToken = false;
    std::string cur;
    std::vector<std::string> raw;
    for (char c : text) {
        if (c == '\n') { raw.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) raw.push_back(cur);
    for (const std::string& lineText : raw) {
        Line line{lineText, ""};
        const std::vector<std::string> t = tokenizeLine(lineText);
        if (!t.empty()) {
            const std::string& k = t[0];
            const std::string v = t.size() > 1 ? t[1] : std::string();
            int b;
            if (_stricmp(k.c_str(), "Net") == 0 && t.size() == 1) {
                hadNetToken = true;
                line.managedKey = "<net>";
            } else if (k == "ExtraFunctionEnable"
                       && (b = parseBool(v)) >= 0) {
                st.extraKeys = b; line.managedKey = k;
            } else if (k == "SystemPause" && (b = parseBool(v)) >= 0) {
                st.systemPause = b; line.managedKey = k;
            } else if (k == "DebugWindow" && (b = parseBool(v)) >= 0) {
                st.debugWindow = b; line.managedKey = k;
            } else if (k == "NoSound" && (b = parseBool(v)) >= 0) {
                st.noSound = b; line.managedKey = k;
            } else if (k == "Vsync" && (b = parseBool(v)) >= 0) {
                st.vsync = b; line.managedKey = k;
            } else if (k == "WaitTimeMode" && (b = parseBool(v)) >= 0) {
                st.waitTimeMode = b; line.managedKey = k;
            } else if (k == "PrintMasterDamage" && t.size() == 1) {
                st.printMaster = 1; line.managedKey = k;
            } else if (k == "PrintDecDamage" && t.size() == 1) {
                st.printDec = 1; line.managedKey = k;
            } else if (k == "ComVsAsobiVersion" && t.size() == 1) {
                st.comVsAsobi = 1; line.managedKey = k;
            } else if (k == "SetWaitTime" && !v.empty()) {
                st.setWaitTime = atol(v.c_str()); line.managedKey = k;
            } else if (k == "WaitCount" && !v.empty()) {
                st.waitCount = atol(v.c_str()); line.managedKey = k;
            } else if (k == "ExtraSkip" && !v.empty()) {
                st.extraSkip = atol(v.c_str()); line.managedKey = k;
            } else if (k == "PriorityTop"
                       && (v == "RealTime" || v == "Normal")) {
                st.priorityTop = v; line.managedKey = k;
            } else if (k == "BgmPlayType"
                       && (v == "Stream" || v == "Buffer")) {
                st.bgmPlayType = v; line.managedKey = k;
            } else if (k == "BgmPath" && !v.empty()) {
                st.bgmPath = v; line.managedKey = k;
            }
        }
        lines.push_back(line);
    }
}

std::string emitKey(const State& st, const std::string& key) {
    auto boolLine = [&key](int v) {
        return v < 0 ? std::string()
                     : key + " " + (v ? "TRUE" : "FALSE");
    };
    auto intLine = [&key](long v) {
        return v < 0 ? std::string() : key + " " + std::to_string(v);
    };
    if (key == "ExtraFunctionEnable") return boolLine(st.extraKeys);
    if (key == "SystemPause") return boolLine(st.systemPause);
    if (key == "DebugWindow") return boolLine(st.debugWindow);
    if (key == "NoSound") return boolLine(st.noSound);
    if (key == "Vsync") return boolLine(st.vsync);
    if (key == "WaitTimeMode") return boolLine(st.waitTimeMode);
    if (key == "PrintMasterDamage")
        return st.printMaster ? key : std::string();
    if (key == "PrintDecDamage") return st.printDec ? key : std::string();
    if (key == "ComVsAsobiVersion")
        return st.comVsAsobi ? key : std::string();
    if (key == "SetWaitTime") return intLine(st.setWaitTime);
    if (key == "WaitCount") return intLine(st.waitCount);
    if (key == "ExtraSkip") return intLine(st.extraSkip);
    if (key == "PriorityTop")
        return st.priorityTop.empty() ? std::string()
                                      : key + " " + st.priorityTop;
    if (key == "BgmPlayType")
        return st.bgmPlayType.empty() ? std::string()
                                      : key + " " + st.bgmPlayType;
    if (key == "BgmPath")
        return st.bgmPath.empty() ? std::string() : key + " " + st.bgmPath;
    return std::string();
}

const char* const kManagedKeys[] = {
    "ExtraFunctionEnable", "SystemPause", "DebugWindow",
    "PrintMasterDamage", "PrintDecDamage", "ComVsAsobiVersion",
    "WaitTimeMode", "SetWaitTime", "WaitCount", "ExtraSkip", "Vsync",
    "PriorityTop", "NoSound", "BgmPlayType", "BgmPath",
};

bool saveFiles(qui::Console& con, const std::string& dir, const State& st,
               const std::vector<Line>& lines) {
    qpath::ensureDir(dir);

    const std::string dmy = dir + "\\ConfigCheckFile.dmy";
    bool dmyOk = false;
    {
        std::ifstream in(dmy, std::ios::binary | std::ios::ate);
        dmyOk = in && in.tellg() == std::streampos(10);
    }
    if (!dmyOk) {
        std::ofstream out(dmy, std::ios::binary | std::ios::trunc);
        if (!out || !(out << "qoh-caster").flush()) {
            con.message("Not saved",
                        "Could not write ConfigCheckFile.dmy in\n  " + dir);
            return false;
        }
    }

    std::string text;
    std::set<std::string> emitted;
    for (const Line& line : lines) {
        if (line.managedKey.empty()) {
            qclient::fcommand::appendCrLfLine(text, line.text);
        } else if (line.managedKey != "<net>"
                   && emitted.insert(line.managedKey).second) {
            const std::string repl = emitKey(st, line.managedKey);
            if (!repl.empty())
                qclient::fcommand::appendCrLfLine(text, repl);
        }
    }
    for (const char* key : kManagedKeys) {
        if (emitted.count(key)) continue;
        const std::string repl = emitKey(st, key);
        if (!repl.empty())
            qclient::fcommand::appendCrLfLine(text, repl);
    }

    const std::string path = dir + "\\FCommand.txt";
    CopyFileA(path.c_str(), (path + ".caster.bak").c_str(), FALSE);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            con.message("Not saved",
                        "Could not write a temporary file in\n  " + dir);
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out.flush()) {
            con.message("Not saved", "Write to the temporary file failed.");
            return false;
        }
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        con.message("Not saved",
                    "Could not replace FCommand.txt (is the game running?).");
        return false;
    }
    return true;
}

const char* triName(int v) {
    return v < 0 ? "(not set)" : v ? "TRUE" : "FALSE";
}
int nextTri(int v) {
    return v < 0 ? 1 : v == 1 ? 0 : -1;
}
std::string intName(long v, const char* whenAbsent) {
    return v < 0 ? std::string(whenAbsent) : std::to_string(v);
}

void extraFunctionsMenu(qui::Console& con, const Settings& s) {
    const std::string dir = dirPathOf(s);
    State st;
    std::vector<Line> lines;
    bool hadNet = false;
    {
        std::ifstream in(dir + "\\FCommand.txt", std::ios::binary);
        if (in) {
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            parseFile(text, st, lines, hadNet);
        }
    }
    bool dirty = false;
    if (hadNet) {
        dirty = true;
        con.message(
            "Removed the 'Net' switch",
            "This FCommand.txt contained the hidden 'Net' keyword, which "
            "boots QOH's OWN 2001-era DirectPlay netplay lobby instead of "
            "the normal game. Caster sessions need the game in local mode, "
            "so the switch will be dropped when you save.");
    }

    for (;;) {
        const int c = con.menu(
            std::string("Extra functions  (LocalConfig\\FCommand.txt)")
                + (dirty ? "   [unsaved]" : ""),
            {
                "Hidden F-keys (F2..F9):  "
                    + std::string(triName(st.extraKeys)),
                "Pause keys only:         "
                    + std::string(triName(st.systemPause)),
                "Debug window:            "
                    + std::string(triName(st.debugWindow)),
                "Print damage (total):    "
                    + std::string(st.printMaster ? "on" : "off"),
                "Print damage (per-hit):  "
                    + std::string(st.printDec ? "on" : "off"),
                "AI exhibition unlock:    "
                    + std::string(st.comVsAsobi ? "on" : "off"),
                "Process priority:        "
                    + (st.priorityTop.empty() ? "(not set)"
                                              : st.priorityTop),
                "Frame limiter (QPC):     "
                    + std::string(triName(st.waitTimeMode)),
                "SetWaitTime (QPC ticks): "
                    + intName(st.setWaitTime, "(auto-measured)"),
                "WaitCount (divide FPS):  " + intName(st.waitCount, "(off)"),
                "ExtraSkip (render skip): " + intName(st.extraSkip, "(off)"),
                "Vsync:                   " + std::string(triName(st.vsync)),
                "Mute all (NoSound):      "
                    + std::string(triName(st.noSound)),
                "BGM load mode:           "
                    + (st.bgmPlayType.empty() ? "(not set)"
                                              : st.bgmPlayType),
                "Save to FCommand.txt",
            },
            "Back");
        int v = 0;
        switch (c) {
            case 0:
                st.extraKeys = nextTri(st.extraKeys);
                dirty = true;
                if (st.extraKeys == 1)
                    con.message(
                        "Hidden F-keys enabled (offline)",
                        "In a match:\n"
                        "  F2 / F3   computer control P1 / P2\n"
                        "  F4        HP/SP auto refill\n"
                        "  F5        back to character select\n"
                        "  F6        reset round\n"
                        "  F7        show hitboxes\n"
                        "  F9        pause   (F8 = one frame forward)\n"
                        "  F12       freeze  (Delete = quit the match)\n\n"
                        "These are the game's own developer keys. Netplay "
                        "launches neutralize them (they would desync) "
                        "unless you also allow them under Settings.");
                break;
            case 1: st.systemPause = nextTri(st.systemPause); dirty = true; break;
            case 2: st.debugWindow = nextTri(st.debugWindow); dirty = true; break;
            case 3: st.printMaster = !st.printMaster; dirty = true; break;
            case 4: st.printDec = !st.printDec; dirty = true; break;
            case 5: st.comVsAsobi = !st.comVsAsobi; dirty = true; break;
            case 6:
                st.priorityTop = st.priorityTop.empty() ? "RealTime"
                    : st.priorityTop == "RealTime" ? "Normal"
                                                   : std::string();
                dirty = true;
                break;
            case 7: st.waitTimeMode = nextTri(st.waitTimeMode); dirty = true; break;
            case 8:
                if (con.promptInt(
                        "Frame period in QPC ticks (0 = remove, keep the "
                        "game's auto-measured 60 FPS; wrong values run the "
                        "game too fast or too slow)",
                        v, st.setWaitTime < 0 ? 0 : (int)st.setWaitTime, 0,
                        100000000)) {
                    st.setWaitTime = v <= 0 ? -1 : v;
                    dirty = true;
                }
                break;
            case 9:
                if (con.promptInt(
                        "Run at (frame rate / N). 0 = remove", v,
                        st.waitCount < 0 ? 0 : (int)st.waitCount, 0, 60)) {
                    st.waitCount = v <= 0 ? -1 : v;
                    dirty = true;
                }
                break;
            case 10:
                if (con.promptInt(
                        "Render every N-th frame, logic full speed. "
                        "0 = remove", v,
                        st.extraSkip < 0 ? 0 : (int)st.extraSkip, 0, 60)) {
                    st.extraSkip = v <= 0 ? -1 : v;
                    dirty = true;
                }
                break;
            case 11: st.vsync = nextTri(st.vsync); dirty = true; break;
            case 12: st.noSound = nextTri(st.noSound); dirty = true; break;
            case 13:
                st.bgmPlayType = st.bgmPlayType.empty() ? "Stream"
                    : st.bgmPlayType == "Stream" ? "Buffer" : std::string();
                dirty = true;
                break;
            case 14:
                if (saveFiles(con, dir, st, lines)) {
                    dirty = false;
                    lines.clear();
                    State reread;
                    bool dummy = false;
                    std::ifstream in(dir + "\\FCommand.txt",
                                     std::ios::binary);
                    if (in) {
                        std::string text(
                            (std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
                        parseFile(text, reread, lines, dummy);
                        st = reread;
                    }
                    con.status("Saved. The game reads this at launch.");
                }
                break;
            case qui::kBack:
                if (dirty
                    && con.menu("Discard unsaved FCommand changes?",
                                { "Discard them" }, "Keep editing") != 0)
                    break;
                return;
            default: break;
        }
    }
}

}

namespace replays_ui {
void replaysMenu(qui::Console& con, Settings& s);
}

void replayStorageMenu(qui::Console& con, Settings& s) {
    for (;;) {
        const std::string root = s.replayRoot.empty()
            ? qplat::narrow(qplat::replayRootW()) + "   (default)"
            : s.replayRoot;
        const int c = con.menu("Replays and storage", {
            "Auto-record netplay:        "
                + std::string(s.recordNetplay ? "on" : "off"),
            "Auto-record local VS:       "
                + std::string(s.recordLocalVs ? "on" : "off"),
            "Training / replay-of-replay: never recorded",
            "Save spectator sessions:    "
                + std::string(s.saveSpectatorSessions
                                  ? "yes" : "no (ask each time)"),
            "Replay folder:              " + root,
            "Show incomplete recordings: "
                + std::string(s.showIncompleteReplays ? "yes" : "no"),
            "Show the Trash:             "
                + std::string(s.showTrashedReplays ? "yes" : "no"),
            "Show network details:       "
                + std::string(s.showReplayNetworkDetails
                                  ? "yes" : "no (hidden in the browser)"),
            "Recorder flush interval:    "
                + std::to_string(s.replayFlushSeconds) + " s",
            "Warn below free space:      "
                + std::to_string(s.replayStorageWarnMb) + " MiB",
            "Retention policy:           never delete automatically",
            "Default playback audio:     "
                + std::string(s.replayDefaultAudio ? "on" : "off"),
            "Battle log and statistics:  "
                + std::string(s.battleLog ? "on" : "off"),
            "Pause at replay start:      "
                + std::string(s.replayPauseAtStart ? "on" : "off"),
        }, "Back");
        if (c == qui::kBack) { s.save(); return; }
        std::string text;
        int value = 0;
        switch (c) {
            case 0: s.recordNetplay = !s.recordNetplay; break;
            case 1: s.recordLocalVs = !s.recordLocalVs; break;
            case 2:
                con.message("Never recorded",
                            "Training mode and playing a replay of a replay "
                            "are never recorded. Neither produces a recording "
                            "the library could serve, so there is nothing to "
                            "switch on.");
                break;
            case 3: s.saveSpectatorSessions = !s.saveSpectatorSessions; break;
            case 4:
                if (con.promptLine(
                        "Replay folder (blank = the default beside the caster)",
                        text, s.replayRoot)) {
                    s.replayRoot = text;
                    if (!s.replayRoot.empty()) {
                        const std::wstring wide = qplat::widen(s.replayRoot);
                        if (wide.empty() || !qplat::ensureDirW(wide)) {
                            con.message("That folder cannot be used",
                                        "The path could not be created or is "
                                        "not writable. The previous folder is "
                                        "kept.");
                            s.replayRoot.clear();
                        }
                    }
                }
                break;
            case 5:
                s.showIncompleteReplays = !s.showIncompleteReplays;
                break;
            case 6: s.showTrashedReplays = !s.showTrashedReplays; break;
            case 7:
                s.showReplayNetworkDetails = !s.showReplayNetworkDetails;
                if (!s.showReplayNetworkDetails)
                    con.message("Network details hidden",
                                "The browser will stop showing the "
                                "delay/rollback/RTT summary.\n\n"
                                "To be exact: this hides it, it does not "
                                "remove it. What a recording already contains "
                                "was written by the game runtime and is still "
                                "in the file.");
                break;
            case 8:
                if (con.promptInt("Recorder flush interval (seconds)", value,
                                  s.replayFlushSeconds, 1, 60))
                    s.replayFlushSeconds = value;
                break;
            case 9:
                if (con.promptInt("Warn when free space drops below (MiB)",
                                  value, s.replayStorageWarnMb, 0, 1024 * 1024))
                    s.replayStorageWarnMb = value;
                break;
            case 10:
                con.message("Retention policy",
                            "Nothing is ever deleted automatically. Replays "
                            "leave the library only when you move them to the "
                            "Trash yourself, and permanent deletion is a "
                            "separate confirmed action.\n\n"
                            "This build implements no automatic pruning rule, "
                            "so there is no other policy to choose.");
                break;
            case 11: s.replayDefaultAudio = !s.replayDefaultAudio; break;
            case 12:
                s.battleLog = !s.battleLog;
                if (!s.battleLog)
                    con.message("Battle log off",
                                "New games will not be written to the log, so "
                                "the statistics stop at today.\n\n"
                                "Nothing is removed: the file keeps every "
                                "game it already has, and turning this back "
                                "on resumes appending to it.");
                break;
            case 13: s.replayPauseAtStart = !s.replayPauseAtStart; break;
            default: break;
        }
        s.save();
    }
}

void spectatorServingMenu(qui::Console& con, Settings& s) {
    for (;;) {
        const int automaticPort = (s.port != 0 && s.port < 65535)
            ? static_cast<int>(s.port) + 1 : 0;
        const int c = con.menu("Let people watch your matches", {
            std::string("Who may watch:   ")
                + admissionPolicyName(admissionPolicyOf(s)),
            "Maximum viewers: " + std::to_string(s.spectatorMaxViewers),
            std::string("Spectator port:  ")
                + (s.spectatorPort > 0
                       ? std::to_string(s.spectatorPort)
                       : (automaticPort != 0
                              ? "automatic (" + std::to_string(automaticPort)
                                    + ")"
                              : "automatic")),
            "What to forward on your router...",
        }, "Back");
        if (c == qui::kBack) { s.save(); return; }
        if (c == 0) {
            const int choice = con.menu(
                "Who may watch your matches",
                { "Anyone with my host address",
                  "Ask me first",
                  "Nobody - do not open a spectator port at all" },
                "Back");
            static constexpr int kPolicies[] = {1, 2, 4};
            if (choice >= 0 && choice < 3)
                s.spectatorAdmission = kPolicies[choice];
        } else if (c == 1) {
            int value = 0;
            if (con.promptInt("Maximum simultaneous viewers", value,
                              s.spectatorMaxViewers, 1, 256))
                s.spectatorMaxViewers = value;
        } else if (c == 2) {
            int value = 0;
            if (con.promptInt(
                    "Spectator UDP port (0 = automatic, your game port + 1)",
                    value, s.spectatorPort, 0, 65535))
                s.spectatorPort = value;
        } else if (c == 3) {
            const int automatic = (s.port != 0 && s.port < 65535)
                ? static_cast<int>(s.port) + 1 : 0;
            const int spectator =
                s.spectatorPort > 0 ? s.spectatorPort : automatic;
            const std::string spectatorText = spectator != 0
                ? std::to_string(spectator) : std::string("(automatic)");
            con.message(
                "What to forward on your router",
                "Players and spectators use DIFFERENT UDP ports. Forwarding "
                "only your\ngame port lets people play you and not watch "
                "you.\n\n"
                "  UDP " + std::to_string(s.port)
                    + "   the game, for the other player\n"
                  "  UDP " + spectatorText + "   spectators\n\n"
                  "You normally do not have to do anything. The caster asks "
                  "your router\nto open the spectator port automatically "
                  "while you host, and closes\nit again when you stop. If "
                  "your router refuses, it falls back to automatic\nhole "
                  "punching.\n\n"
                  "Forward these two ports by hand only if both of those "
                  "fail. The\nspectator port used to be picked at random "
                  "every time, so no forwarding\nrule could name it; it is "
                  "now this fixed number.");
        }
        s.save();
    }
}

void settingsPlayerAndFilesMenu(qui::Console& con, Settings& s) {
    for (;;) {
        const std::string g =
            s.gamePath.empty() ? defaultGamePath() : s.gamePath;
        const int c = con.menu("Player and files", {
            "Nickname:     " + s.nickname,
            "Game path:    " + g,
            "Netplay DLL:  " + s.dllPath,
        }, "Back");
        if (c == qui::kBack) { s.save(); return; }
        std::string tmp;
        switch (c) {
            case 0:
                if (con.promptLine(
                        "Your nickname", tmp, s.nickname, CP_UTF8,
                        qgame::kPlayerNameDisplayBytes)) {
                    s.nickname = canonicalUtf8Nickname(tmp);
                }
                break;
            case 1:
                if (con.promptLine("Path to GAME.EXE", tmp, g))
                    s.gamePath = tmp;
                break;
            case 2:
                if (con.promptLine("Path to qoh99_netplay.dll", tmp,
                                   s.dllPath))
                    s.dllPath = tmp;
                break;
        }
        s.save();
    }
}

void settingsConnectionMenu(qui::Console& con, Settings& s) {
    for (;;) {
        const int c = con.menu("Connection", {
            "Host port:              " + std::to_string(s.port),
            std::string("Hole-punch relay:       ")
                + qnet::autopunch::kRelayHost + ":"
                + std::to_string(qnet::autopunch::kRelayPort) + "  (built in)",
        }, "Back");
        if (c == qui::kBack) { s.save(); return; }
        std::string tmp;
        int v = 0;
        switch (c) {
            case 0:
                if (con.promptInt("Port to host games on", v, s.port, 1,
                                  65535))
                    s.port = static_cast<uint16_t>(v);
                break;
            case 1:
                con.message(
                    "Hole-punch relay",
                    std::string("Built in: ") + qnet::autopunch::kRelayHost
                        + ":" + std::to_string(qnet::autopunch::kRelayPort)
                        + ".\n\nIt is only used to introduce two players to "
                          "each other so they can connect directly; it never "
                          "carries any of the match. There is nothing to "
                          "configure, and nothing is sent to it beyond your "
                          "port number and the address you are connecting to.");
                break;
        }
        s.save();
    }
}

void settingsAudioMenu(qui::Console& con, Settings& s) {
    for (;;) {
        const int c = con.menu("Audio", {
            "Music volume:  "
                + (s.musicVolume == 0 ? std::string("off")
                                      : std::to_string(s.musicVolume)),
            "Sound volume:  "
                + (s.soundVolume == 0 ? std::string("off")
                                      : std::to_string(s.soundVolume)),
        }, "Back");
        if (c == qui::kBack) { s.save(); return; }
        int v = 0;
        switch (c) {
            case 0:
                if (con.promptInt("Music volume (0-100, 0 = off)", v,
                                  s.musicVolume, 0, 100))
                    s.musicVolume = v;
                break;
            case 1:
                if (con.promptInt("Sound volume (0-100, 0 = off)", v,
                                  s.soundVolume, 0, 100))
                    s.soundVolume = v;
                break;
        }
        s.save();
    }
}

void settingsInGameDisplayMenu(qui::Console& con, Settings& s) {
    for (;;) {
        const int c = con.menu("In-game display and keys", {
            "Hide in-game SCORE:    "
                + std::string(s.hideNativeScore ? "yes" : "no"),
            "Hide in-game TIME:     "
                + std::string(s.hideNativeTimer ? "yes" : "no"),
            "In-game panel:         configured on Controls page",
        }, "Back");
        if (c == qui::kBack) { s.save(); return; }
        switch (c) {
            case 0: s.hideNativeScore = !s.hideNativeScore; break;
            case 1: s.hideNativeTimer = !s.hideNativeTimer; break;
            case 2:
                con.message(
                    "The in-game panel",
                    "Its Controls-page binding opens and closes the panel while the game "
                    "is running.\n"
                    "Offline only: training, local versus, free play and "
                    "replay playback.\n"
                    "Online the game thread also pumps netplay, so opening the "
                    "panel would\n"
                    "stall your opponent; the always-on match HUD carries that "
                    "job instead.\n"
                    "\n"
                    "The panel owns what you change while you play and watch: "
                    "dummy\n"
                    "behaviour and recovery, position resets, health / meter / "
                    "guard gauge,\n"
                    "recording, save states, pause and frame step, the overlay "
                    "position\n"
                    "editor, and the raw diagnostics for a bug report.\n"
                    "\n"
                    "With the panel closed, configured wrapper controls still "
                    "toggle the\n"
                    "input viewer and presentation mode in their allowed modes.\n"
                    "\n"
                    "What you set once stays here in Settings: nickname, "
                    "audio, connection,\n"
                    "spectators, replays, game config and the hidden F-keys. "
                    "The panel does\n"
                    "not repeat them.\n"
                    "\n"
                    "Input delay and rollback are not settings: the caster "
                    "measures the\n"
                    "connection after your opponent joins and offers the "
                    "numbers for that\n"
                    "route, and in the match the online controls change them "
                    "whenever you\n"
                    "like.");
                break;
        }
        s.save();
    }
}

void settingsMenu(qui::Console& con, Settings& s) {
    for (;;) {
        const int c = con.menu("Settings", {
            std::string(qi18n::tr("Language")) + ":        "
                + qi18n::languageName(qi18n::language()),
            "Player and files...",
            "Connection...",
            "Audio...",
            "In-game display and keys...",
            "Game config (fullscreen, music, vsync)...",
            "Extra functions (hidden F-keys, FCommand.txt)...",
            "Let people watch your matches...",
            "Replays and storage...",
        }, "Back");
        if (c == qui::kBack) { s.save(); return; }
        switch (c) {
            case 0:
                s.language = s.language == 0 ? 1 : 0;
                qi18n::setLanguageFromIniValue(s.language);
                s.save();
                break;
            case 1: settingsPlayerAndFilesMenu(con, s); break;
            case 2: settingsConnectionMenu(con, s); break;
            case 3: settingsAudioMenu(con, s); break;
            case 4: settingsInGameDisplayMenu(con, s); break;
            case 5: gamecfg_ui::gameConfigMenu(con, s); break;
            case 6: fcmd_ui::extraFunctionsMenu(con, s); break;
            case 7: spectatorServingMenu(con, s); break;
            case 8: replayStorageMenu(con, s); break;
        }
        s.save();
    }
}

void offlineMenu(qui::Console& con, Settings& s) {
    int c = con.menu("Play offline", {
        "Training",
        "VS Player",
        "VS CPU",
        "Free Play",
    }, "Back");
    // Offline modes must NOT be MODE_NETPLAY: it sets network mode (dword_4AAA50=1) with no peer, hanging the game.
    switch (c) {
        case 0:
            runOffline(s, qipc::MODE_OBSERVE, qipc::kVsRow1v1, &con,
                       false, true,
                       true);
            break;
        case 1:
            runOffline(s, qipc::MODE_OBSERVE, qipc::kVsRow1v1, &con,
                       false, true, false, true);
            break;
        case 2:
            runOffline(s, qipc::MODE_OBSERVE, qipc::kVsRow1vCpu, &con,
                       false, true, false, true);
            break;
        case 3: runOffline(s, qipc::MODE_OBSERVE, qipc::kVsRow1v1, &con,
                            false,
                            false,
                            false,
                            true); break;
        default: break;
    }
}

void diagnosticsMenu(qui::Console& con, Settings& s) {
    for (;;) {
        int c = con.menu("Diagnostics (developer)", {
            "Run transport tests",
            "Savestate self-test  (validate rollback regions)",
            "Observe mode  (log game state, no changes)",
        }, "Back");
        switch (c) {
            case 0: con.clear(); qoh99_run_tests(); con.pause(); break;
            case 1: runOffline(s, qipc::MODE_SELFTEST, qipc::kVsRow1v1, &con); break;
            case 2: runOffline(s, qipc::MODE_OBSERVE,  qipc::kVsRow1v1, &con); break;
            case qui::kBack: return;
        }
    }
}

int interactive() {
    qui::Console con;
    Settings s;
    s.gamePath = defaultGamePath();
    s.load();
    qi18n::setLanguageFromIniValue(s.language);
    if (s.gamePath.empty()) s.gamePath = defaultGamePath();
    g_localName = s.nickname;
    qclient::public_ip::start();

    con.statusLine("Checking the game files...");
    GameFilesCheck game = gameFilesCheck(s);
    std::string warnedFor;
    if (!game.ok) {
        con.message("Wrong or missing game", game.error);
        warnedFor = game.path;
    } else {
    }

    for (;;) {
        const std::string currentGame = resolveGamePath(s);
        if (currentGame != game.path) {
            con.statusLine("Checking the game files...");
            game = gameFilesCheck(s);
            if (!game.ok && game.path != warnedFor) {
                con.message("Wrong or missing game", game.error);
                warnedFor = game.path;
            } else if (game.ok) {
                warnedFor.clear();
            }
        }
        con.setBanner(casterBanner(s.nickname)
                      + (game.ok ? "" : "  [!] game not usable"));
        std::vector<std::string> items = {
            "Host a game  (port " + std::to_string(s.port) + ")",
            "Join a game",
            "Spectate a game",
            "Replays",
            "Play offline",
            "Controls",
            "Settings",
        };
        if (g_devMode) items.push_back("Diagnostics");
        int c = con.menu("Main menu", items, "Quit");
        switch (c) {
            case 0:
                runAutomaticHost(s, &con);
                break;
            case 1: {
                std::string addr;
                std::string def = s.joinIp + ":" + std::to_string(s.joinPort);
                while (con.promptLine("Host address (ip[:port])", addr, def)) {
                    std::string ip; uint16_t port = 0;
                    if (!parseHostPort(addr, ip, port, s.joinPort)) {
                        con.message("Invalid address",
                                    "Use a numeric IPv4 address, optionally with a port.\n"
                                    "e.g. 192.168.1.10:7500  or  192.168.1.10");
                        continue;
                    }
                    s.joinIp = ip; s.joinPort = port;
                    s.save();
                    runAutomaticJoin(s, &con);
                    break;
                }
                break;
            }
            case 2: {
                std::string addr;
                std::string def = s.joinIp + ":" + std::to_string(s.joinPort);
                if (!con.promptLine(
                        "Player address (ip[:port])", addr, def)) {
                    break;
                }
                std::string ip; uint16_t port = 0;
                if (!parseHostPort(addr, ip, port, s.joinPort)) {
                    con.message("Invalid address",
                                "Use a numeric IPv4 address with an optional "
                                "port, for example 192.168.1.10:10800.");
                    break;
                }
                QLOG("[spectate-entry] type=player-address target=%s:%u",
                     ip.c_str(), static_cast<unsigned>(port));
                s.joinIp = ip; s.joinPort = port; s.save();
                bool watching = true;
                while (watching) {
                    SpectateInfo info{};
                    std::string why;
                    if (!queryHostSpectateInfo(ip, port, info, why)) {
                        con.message("Can't watch right now", why);
                        break;
                    }
                    if (info.state == SPECTATE_LIVE) {
                        if (info.keyLow == 0 && info.keyHigh == 0) {
                            const std::string why =
                                info.admissionState
                                    == SPECTATE_ADMISSION_REQUIRED
                                ? "This host is not accepting address-based "
                                  "spectators."
                                : "This host is not accepting spectators "
                                  "right now.";
                            con.message("Can't watch right now", why);
                            break;
                        }
                        Settings viewer = s;
                        viewer.joinIp = ip;
                        viewer.joinPort = info.spectatorPort;
                        runSpectator(viewer, info.sessionId, info.keyLow,
                                     info.keyHigh, &con,
                                     false,
                                     port);
                        break;
                    }
                    const int choice = con.menu(
                        "No match is running at " + ip,
                        { "Join as the opponent",
                          "Wait for their match to start" },
                        "Back");
                    if (choice == 0) {
                        runAutomaticJoin(s, &con);
                        break;
                    }
                    if (choice != 1) break;
                    con.status("Waiting for a match to start at " + ip
                               + " - Esc cancels...");
                    for (int slice = 0; slice < 40; ++slice) {
                        if (con.pollKey(100) == 27) { watching = false;
                                                      break; }
                    }
                }
                break;
            }
            case 3: replays_ui::replaysMenu(con, s); break;
            case 4: offlineMenu(con, s); break;
            case 5: controls_ui::controlsMenu(con); break;
            case 6: settingsMenu(con, s); break;
            case 7: if (g_devMode) diagnosticsMenu(con, s); break;
            case qui::kBack: return 0;
        }
    }
}

struct Args {
    std::string verb, ip, mode;
    Settings s;
    uint8_t wireMode = qipc::MODE_OBSERVE;
    bool hiddenDesktop = false;
    bool rendezvousOnly = false;
    int flagDelay = -1, flagRollback = -1;
    SpectatorRouteForce spectatorRouteForce =
        SpectatorRouteForce::Auto;
    bool spectatorRouteForceProvided = false;
    bool spectatorTopologyOptIn = false;
    bool spectatorWantsLatest = false;
    uint32_t spectatorRetainMs = 0;
    bool spectatorRetainProvided = false;
    RelayOptions relay;
};

bool parseArgs(int argc, char** argv, Args& a) {
    a.verb = argv[1];
    int i = 2;
    if ((a.verb == "join" || a.verb == "spectate" || a.verb == "status")
        && i < argc && argv[i][0] != '-') {
        std::string ip; uint16_t port = 0;
        if (!parseHostPort(argv[i], ip, port, a.s.joinPort)) {
            std::printf("Invalid host address '%s' - use ip[:port], e.g. 192.168.1.10:7500\n", argv[i]);
            return false;
        }
        a.s.joinIp = ip; a.s.joinPort = port;
        i++;
        if (i < argc && argv[i][0] != '-') {
            const std::string value = argv[i++];
            uint16_t parsedPort = 0;
            if (!parsePortNumber(value, parsedPort)) {
                std::printf("Invalid host port '%s' - expected 1..65535\n",
                            value.c_str());
                return false;
            }
            a.s.joinPort = parsedPort;
        }
    }
    if (a.verb == "host" && i < argc && argv[i][0] != '-') {
        const std::string value = argv[i++];
        uint16_t parsedPort = 0;
        if (!parsePortNumber(value, parsedPort)) {
            std::printf("Invalid host port '%s' - expected 1..65535\n",
                        value.c_str());
            return false;
        }
        a.s.port = parsedPort;
    }
    a.wireMode = qipc::MODE_OBSERVE;
    for (; i < argc; i++) {
        std::string o = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if      (o == "--game")     a.s.gamePath = next();
        else if (o == "--dll")      a.s.dllPath  = next();
        else if (o == "--nickname") {
            if (i + 1 >= argc
                || !qgame::isNicknameOptionValue(argv[i + 1])) {
                std::printf(
                    "--nickname requires a non-empty value that is not "
                    "another option\n");
                return false;
            }
            const int nicknameIndex = ++i;
            std::string nicknameUtf8;
            if (!processArgumentUtf8(argc, nicknameIndex, nicknameUtf8)) {
                nicknameUtf8 = qgame::playerNameForUtf8Display(
                    argv[nicknameIndex], GetACP());
            }
            a.s.nickname = canonicalUtf8Nickname(nicknameUtf8);
        }
        else if (o == "--delay") {
            a.flagDelay = atoi(next().c_str());
            a.s.delay = a.flagDelay;
            a.s.delayProvenance = ConfigProvenanceStage::LocallySelected;
        }
        else if (o == "--rollback") {
            a.flagRollback = atoi(next().c_str());
            a.s.rollback = a.flagRollback;
            a.s.rollbackProvenance = ConfigProvenanceStage::LocallySelected;
        }
        else if (o == "--selftest") a.wireMode   = qipc::MODE_SELFTEST;
        else if (o == "--observe")  a.wireMode   = qipc::MODE_OBSERVE;
        else if (o == "--mode")     a.mode       = next();
        else if (o == "--relay") {
            const std::string value = next();
            std::string ip;
            uint16_t port = 0;
            if (!parseHostPort(value, ip, port, kDefaultRelayPort)
                || !Endpoint::parse(ip, port, a.relay.server)) {
                std::printf("Invalid relay address '%s' - use numeric "
                            "ip[:port]\n", value.c_str());
                return false;
            }
            a.relay.enabled = true;
        }
        else if (o == "--room") {
            const std::string value = next();
            if (!qnet::relay::parseRoomId(value, a.relay.room)) {
                std::printf("Invalid relay room '%s' - expected 32 nonzero "
                            "hex digits\n", value.c_str());
                return false;
            }
            a.relay.roomProvided = true;
        }
        else if (o == "--hide-game-score") a.s.hideNativeScore = true;
        else if (o == "--hide-game-timer") a.s.hideNativeTimer = true;
        else if (o == "--hide-game-hud")   { a.s.hideNativeScore = true;
                                             a.s.hideNativeTimer = true; }
        else if (o == "--show-game-score") a.s.hideNativeScore = false;
        else if (o == "--show-game-timer") a.s.hideNativeTimer = false;
        else if (o == "--show-game-hud")   { a.s.hideNativeScore = false;
                                             a.s.hideNativeTimer = false; }
        else if (o == "--allow-extra-functions") {
            std::printf("--allow-extra-functions is no longer supported: "
                        "live network sessions always disable QOH's "
                        "local-only developer inputs\n");
            return false;
        }
        else if (o == "--disable-extra-functions") a.s.disableExtraFunctions = true;
        else if (o == "--force-relay") a.relay.forceRelay = true;
        else if (o == "--spectator-route"
                 || o.rfind("--spectator-route=", 0) == 0) {
            std::string value;
            if (o == "--spectator-route") {
                if (i + 1 >= argc) {
                    std::printf("--spectator-route requires "
                                "auto, direct, punch, or relay\n");
                    return false;
                }
                value = argv[++i];
            } else {
                value = o.substr(std::strlen("--spectator-route="));
            }
            if (value == "auto")
                a.spectatorRouteForce = SpectatorRouteForce::Auto;
            else if (value == "direct")
                a.spectatorRouteForce = SpectatorRouteForce::Direct;
            else if (value == "punch")
                a.spectatorRouteForce = SpectatorRouteForce::Punch;
            else if (value == "relay")
                a.spectatorRouteForce = SpectatorRouteForce::Relay;
            else {
                std::printf("Invalid spectator route '%s' - expected "
                            "auto, direct, punch, or relay\n",
                            value.c_str());
                return false;
            }
            a.spectatorRouteForceProvided = true;
        }
        else if (o == "--spectator-retain-ms") {
            if (i + 1 >= argc) {
                std::printf("--spectator-retain-ms requires 1..120000\n");
                return false;
            }
            const std::string value = argv[++i];
            char* end = nullptr;
            const unsigned long parsed =
                std::strtoul(value.c_str(), &end, 10);
            if (value.empty() || end == nullptr || *end != '\0'
                || parsed == 0 || parsed > 120000ul) {
                std::printf("Invalid spectator retention '%s' - expected "
                            "1..120000 milliseconds\n", value.c_str());
                return false;
            }
            a.spectatorRetainMs = static_cast<uint32_t>(parsed);
            a.spectatorRetainProvided = true;
        }
        else if (o == "--spectator-topology") {
            a.spectatorTopologyOptIn = true;
        }
        else if (o == "--latest") {
            a.spectatorWantsLatest = true;
        }
        else if (o == "--rendezvous-only") a.rendezvousOnly = true;
        else if (o == "--dev") { }
        else if (o == "--hidden-desktop" || o == "--background") a.hiddenDesktop = true;
        else if (o == "--trace-gameplay-residual") SetEnvironmentVariableA("QOH99_NP_TRACE_RESIDUAL", "1");
        else if (o == "--pacing-replay")           SetEnvironmentVariableA("QOH99_NP_PACING_REPLAY", "1");
        else if (o == "--replay-tick") {
            std::string m = next();
            SetEnvironmentVariableA("QOH99_NP_REPLAY_TICK", m.empty() ? "deterministic" : m.c_str());
        }
        else if (o == "--double-replay")           SetEnvironmentVariableA("QOH99_NP_DOUBLE_REPLAY", "1");
        else if (o == "--cosmetic-rng") {
            std::string m = next();
            SetEnvironmentVariableA("QOH99_NP_COSMETIC_RNG", m.empty() ? "trace" : m.c_str());
        }
        else if (o == "--k") {
            std::string k = next();
            SetEnvironmentVariableA("QOH99_NP_K", k.c_str());
        }
        else if (o == "--input-test")              SetEnvironmentVariableA("QOH99_NP_INPUT_TEST", "1");
        else if (o == "--input-sequence") {
            std::string m = next();
            if (m == "deterministic") SetEnvironmentVariableA("QOH99_NP_INPUT_OVERRIDE", "1");
        }
        else { std::printf("Unknown option: %s\n", o.c_str()); return false; }
    }
    if (a.s.gamePath.empty()) a.s.gamePath = defaultGamePath();
    if ((a.relay.enabled || a.relay.roomProvided || a.relay.forceRelay)
        && a.verb != "host" && a.verb != "join") {
        std::printf("Player relay options are valid only with host or join\n");
        return false;
    }
    if (a.rendezvousOnly && a.verb != "host" && a.verb != "join") {
        std::printf("--rendezvous-only is valid only with host or join\n");
        return false;
    }
    if (a.spectatorRouteForceProvided && a.verb != "spectate") {
        std::printf("--spectator-route is valid only with spectate\n");
        return false;
    }
    if (a.spectatorWantsLatest && a.verb != "spectate") {
        std::printf("--latest only applies to the spectate verb\n");
        return false;
    }
    if (a.spectatorTopologyOptIn && a.verb != "spectate") {
        std::printf("--spectator-topology is valid only with spectate\n");
        return false;
    }
    if (a.spectatorRetainProvided && a.verb != "host") {
        std::printf("--spectator-retain-ms is valid only with host\n");
        return false;
    }
    if (a.relay.roomProvided && !a.relay.enabled) {
        std::printf("--room requires --relay <ip[:port]>\n");
        return false;
    }
    if (a.relay.enabled && a.verb == "join" && !a.relay.roomProvided) {
        std::printf("Relay join requires --room <32-hex>\n");
        return false;
    }
    if (a.relay.enabled && a.verb == "host" && !a.relay.roomProvided) {
        std::string roomError;
        if (!qnet::relay::generateRoomId(a.relay.room, &roomError)) {
            std::printf("Could not generate relay room: %s\n",
                        roomError.c_str());
            return false;
        }
    }
    return true;
}

qclient::replay::RuntimeIdentity currentRuntimeIdentity(const Settings& s) {
    qclient::replay::RuntimeIdentity id{};
    id.runtimeSimulationId = qipc::kRuntimeCompatibilityId;
    id.characterCatalogId =
        qreplay::CharacterCatalog::forSeBuild().catalogId();
    id.containerReaderMajor = qreplay::kContainerReaderMajor;
    id.supportsFourPlayer = false;
    id.supportsCpuFighters = false;
    id.supportsPortableCheckpoint = false;
    id.supportsEffectiveFighterDomain = false;
    qclient::ExecutableIdentity exe{};
    std::string error;
    const std::string game = resolveGamePath(s);
    std::array<uint8_t, 32> contentRoot{};
    if (sessionExecutableIdentity(s, exe, error)
        && qreplay::fromHex(exe.sha256, id.executableSha256)
        && replayContentRootFor(s, contentRoot)) {
        id.contentManifestRoot = contentRoot;
        id.known = true;
    }
    return id;
}

qclient::replay::ReplayRuntimeIdentityV2 currentRuntimeIdentityV2(
    const Settings& s) {
    qclient::replay::ReplayRuntimeIdentityV2 id{};
    id.containerReaderMajor = qreplay::kContainerReaderMajorV2;
    qclient::ExecutableIdentity exe{};
    std::array<uint8_t, 32> executableDigest{};
    std::string error;
    const std::string game = resolveGamePath(s);
    qnetplay::compat::CompatibilityIdentity compatibilityIdentity{};
    if (!localCompatibilityIdentityFor(
            s, compatibilityIdentity, error)) {
        QLOG("[replay.play.v2] complete runtime/content identity unavailable "
             "exe='%s' reason='%s'", game.c_str(), error.c_str());
        return id;
    }
    std::array<uint8_t, 32> contentRoot{};
    const bool haveContentRoot = replayContentRootFor(s, contentRoot);
    if (!sessionExecutableIdentity(s, exe, error)
        || !qreplay::fromHex(exe.sha256, executableDigest)
        || !haveContentRoot) {
        QLOG("[replay.play.v2] runtime identity unavailable exe='%s' "
             "inspect='%s' curatedContentRoot=%s",
             game.c_str(), error.c_str(),
             haveContentRoot ? "present" : "unavailable");
        return id;
    }
    id.contract = qreplay::makeSeSimulationContractV2(
        qipc::kRuntimeCompatibilityId,
        executableDigest, contentRoot);
    id.compatibilityIdentity = compatibilityIdentity;
    id.legacyContentRoots = legacyContentRootsFor(s);
    id.known = qreplay::isValidSimulationContractV2(id.contract)
        && qnetplay::compat::isValidCompatibilityIdentity(
            id.compatibilityIdentity);
    if (!id.known) {
        const bool contractOk =
            qreplay::isValidSimulationContractV2(id.contract);
        const bool identityOk =
            qnetplay::compat::isValidCompatibilityIdentity(
                id.compatibilityIdentity);
        QLOG("[replay.play.v2] runtime identity incomplete: "
             "simulationContract=%s compatibilityIdentity=%s edition='%s' "
             "sha256=%s contentRoot=%s",
             contractOk ? "ok" : "INVALID",
             identityOk ? "ok" : "INVALID",
             qclient::ui::detectedGameEdition().empty()
                 ? "(unidentified)"
                 : qclient::ui::detectedGameEdition().c_str(),
             exe.sha256.c_str(),
             haveContentRoot ? "present" : "unavailable");
    }
    return id;
}

bool hasFinalReplayExtension(const std::wstring& path) {
    static constexpr wchar_t kExtension[] = L".q99r";
    constexpr size_t kExtensionChars = 5u;
    if (path.size() < kExtensionChars) return false;
    const size_t start = path.size() - kExtensionChars;
    for (size_t i = 0u; i < kExtensionChars; ++i) {
        wchar_t value = path[start + i];
        if (value >= L'A' && value <= L'Z')
            value = static_cast<wchar_t>(value - L'A' + L'a');
        if (value != kExtension[i]) return false;
    }
    return true;
}

bool loadReplayPreflightInputV2(const std::wstring& path,
                               qclient::replay::ReplayPreflightInputV2& input,
                               std::string& detail) {
    namespace qr = qclient::replay;
    if (!qr::isSafeReplayPath(path) || !hasFinalReplayExtension(path)) {
        detail = "The next replay must be a finalized .q99r file inside a configured replay root.";
        return false;
    }
    qr::ParseOptionsV2 options{};
    options.retainTimeline = true;
    options.allowUnsealedPrefix = false;
    options.verifyFileDigest = true;
    options.maxRetainedFrames = qreplay::kMaxPlaybackPrebufferFramesV2;
    options.maxRetainedEvents = qreplay::kMaxTimelineEventsV2;
    qr::ParsedReplayV2 parsed{};
    const qreplay::ParseResult parse = qr::ReplayParserV2::parseFile(path, options, parsed);
    if (!parse.ok()) {
        detail = "Only parser-verified one-game Q99R-v2 files enter playback. "
                 "Legacy set replays remain decode-only. ";
        detail += qreplay::parseStatusText(parse.status);
        if (!parse.detail.empty()) detail += ": " + parse.detail;
        return false;
    }
    if (!qr::makeReplayPreflightInputV2(parsed, input, detail)) {
        if (detail.empty()) detail = "The parsed game has no launchable V2 source proof.";
        return false;
    }
    return true;
}

int runReplayPlayback(const Settings& s, const std::wstring& path,
                      uint32_t matchOrdinal,
                       qui::Console* con, bool hiddenDesktop,
                       qreplay::ipc::v3::PlaybackResultPayload*
                           playbackResultOut = nullptr,
                       bool* playbackResultReceivedOut = nullptr,
                       uint16_t presentationFlags = qipc::kReplayHoldFinal | qipc::kReplayShowTransport,
                       qclient::replay::ReplayBrokerNextGameProviderV2 nextGame = {}) {
    namespace qr = qclient::replay;
    if (playbackResultOut != nullptr)
        *playbackResultOut =
            qreplay::ipc::v3::PlaybackResultPayload{};
    if (playbackResultReceivedOut != nullptr)
        *playbackResultReceivedOut = false;
    if (!qr::isSafeReplayPath(path))
        return fail(con, "This replay cannot be opened",
                    "That path is not inside a configured replay root.");
    if (!hasFinalReplayExtension(path))
        return fail(con, "This replay cannot be opened",
                    "Only finalized .q99r replay files can be opened.");
    if (matchOrdinal > 1u)
        return fail(
            con, "This replay cannot be played",
            "Q99R-v2 stores one game per file; choose the file itself rather "
            "than a set match number.");
    if (!preflightOnlineArtifacts(s, con)) return 1;

    qr::ReplayPreflightInputV2 input{};
    std::string inputDetail;
    if (!loadReplayPreflightInputV2(path, input, inputDetail))
        return fail(con, "This replay cannot be played", inputDetail);
    const qr::ReplayRuntimeIdentityV2 runtimeIdentity =
        currentRuntimeIdentityV2(s);
    qr::ReplayBrokerV2 broker;
    const qr::ReplayPreflightResultV2 prepared =
        broker.prepare(input, runtimeIdentity);
    if (!prepared.ok()) {
        QLOG("[replay.preflight] refused status=%s detail='%s' technical='%s'",
             qr::replayPreflightStatusTextV2(prepared.status),
             prepared.detail.c_str(),
             prepared.technicalDetail.c_str());
        const bool identityRefusal =
            prepared.status
                == qr::ReplayPreflightStatusV2::CompatibilityIdentityMismatch
            || prepared.status
                == qr::ReplayPreflightStatusV2::CompatibilityRejected
            || prepared.status
                == qr::ReplayPreflightStatusV2::
                    CompatibilityIdentityUnattested;
        const std::string body =
            (identityRefusal && !prepared.detail.empty())
                ? prepared.detail
                : std::string(replayRefusalMessage(prepared.status));
        std::string technical = "Preflight status: "
            + std::string(qr::replayPreflightStatusTextV2(prepared.status));
        if (!prepared.detail.empty())
            technical += "\nPreflight detail: " + prepared.detail;
        if (!prepared.technicalDetail.empty())
            technical += "\nDiagnostic: " + prepared.technicalDetail;
        return fail(con, "This replay cannot be played", body, 0, technical);
    }

    bool degradedNonce = false;
    const uint64_t nonce = qnet::secureRandom64(degradedNonce);
    bool degradedGeneration = false;
    const uint64_t generationRandom =
        qnet::secureRandom64(degradedGeneration);
    uint32_t launchGeneration =
        static_cast<uint32_t>(generationRandom);
    if (launchGeneration == 0u)
        launchGeneration =
            static_cast<uint32_t>(generationRandom >> 32u);
    if (nonce == 0u || launchGeneration == 0u)
        return fail(
            con, "Replay launch failed",
            "The caster could not mint a nonzero private replay channel "
            "identity.");
    if (degradedNonce || degradedGeneration)
        QLOG("[replay.play.v2] private channel identity used the "
             "last-resort RNG path");

    qipc::IpcConfig cfg =
        baseConfig(s, qipc::MODE_REPLAY, 0, 0);
    cfg.compatibilityId = qipc::kRuntimeCompatibilityId;
    cfg.directBoot = 1;
    cfg.vsRow = qipc::kVsRow1v1;
    cfg.serveSpectators = 0;
    cfg.spectatorServiceMode = qipc::SPECTATOR_SERVICE_OFF;
    if (!s.replayDefaultAudio) {
        cfg.musicVolume = 0;
        cfg.soundVolume = 0;
    }
    cfg.recordEnabled = 0;
    cfg.replayChannelGeneration = launchGeneration;
    cfg.replayBrokerNonce = nonce;
    cfg.replayManifestDigest = 0u;
    cfg.replayIpcSchemaVersion =
        qipc::kReplayPlaybackIpcSchemaVersion;
    cfg.replayFeatureBits =
        qipc::kReplayPlaybackRequiredFeatureBits;
    if (prepared.compatibility.supersededStateSchema)
        cfg.replayFeatureBits |=
            qipc::kReplayPlaybackSupersededStateSchemaBit;
    cfg.replayMatchOrdinal = 0u;
    cfg.replaySpeedPermille = 1000u;
    std::memcpy(
        cfg.replayContentManifestRoot,
        input.start.manifest.contract.contentManifestRoot.data(),
        input.start.manifest.contract.contentManifestRoot.size());
    cfg.replayGameGeneration =
        input.start.manifest.identity.gameGeneration;
    cfg.replayExpectedFrameCount = prepared.frameCount;
    cfg.replayExpectedEventCount = prepared.eventCount;
    cfg.replayGameUuidLow =
        input.start.manifest.identity.gameUuidLow;
    cfg.replayGameUuidHigh =
        input.start.manifest.identity.gameUuidHigh;
    cfg.replayValidatedSourceByteCount =
        input.source.validatedSourceByteCount;
    std::memcpy(
        cfg.replayValidatedSourceRangeDigest,
        input.source.validatedSourceRangeDigest.data(),
        input.source.validatedSourceRangeDigest.size());
    cfg.replayEndpointKind =
        static_cast<uint8_t>(input.endpoint.kind);
    cfg.replayHeadless = hiddenDesktop ? 1u : 0u;
    cfg.replayPresentationFlags = qoh99ReplayStartupFlags(
        presentationFlags, s.replayPauseAtStart, hiddenDesktop);
    if (const char* why = qipc::validateReplayLaunch(cfg))
        return fail(con, "Replay launch contract rejected", why);

    qr::ReplayBrokerLaunchV2 brokerLaunch{};
    brokerLaunch.launchGeneration = launchGeneration;
    brokerLaunch.launchNonce = nonce;
    brokerLaunch.headless = hiddenDesktop;
    const qreplay::ReplayVerificationModeV2 verificationMode =
        prepared.compatibility.verificationMode;
    const bool behaviourVerified = verificationMode
        == qreplay::ReplayVerificationModeV2::BehaviourVerified;
    QLOG("[replay.play.v2] verification mode=%s generation=%u",
         qreplay::replayVerificationModeTextV2(verificationMode),
         cfg.replayGameGeneration);
    info(con, "Replay ready: " + std::to_string(prepared.frameCount)
              + " confirmed frame(s), "
              + std::to_string(prepared.eventCount)
              + " lifecycle event(s), fully prebuffered.");
    info(con,
         behaviourVerified
             ? (cfg.replayGameGeneration > 1u
                ? std::string(
                    "Verification: behaviour-verified (game "
                    + std::to_string(cfg.replayGameGeneration)
                    + " of this session). A rematch starts on top of the "
                      "previous game's memory, which a fresh boot cannot "
                      "reproduce byte for byte; inputs, lifecycle, dispatcher "
                      "results, digest chain and terminal are all still "
                      "enforced every frame.")
                : std::string(
                    "Verification: behaviour-verified. This recording was "
                    "made under an earlier state-proof schema, so its stored "
                    "hashes cannot be reproduced bit for bit by this build; "
                    "inputs, lifecycle, dispatcher results, digest chain and "
                    "terminal are all still enforced every frame."))
             : std::string(
                 "Verification: byte-exact. Every recorded pre/post state "
                 "hash must reproduce bit for bit."));

    if (nextGame && !broker.setNextGameProvider(std::move(nextGame)))
        return fail(con, "Replay launch failed", "The replay folder could not be armed.");
    input = qr::ReplayPreflightInputV2{};
    return launchWithReplayBrokerV2(
        s, cfg, con, hiddenDesktop, broker, brokerLaunch,
        playbackResultOut, playbackResultReceivedOut);
}

namespace replays_ui {

namespace qr = qclient::replay;

constexpr size_t kDetailPaneColumns = 48;
constexpr size_t kPaneGutterColumns = 3;

constexpr size_t kMatchColumnCap = 64;

constexpr size_t kDetailRowsPerPage = 20;

std::string fitCell(const std::string& text, size_t width) {
    if (width == 0) return std::string();
    std::string cell = text;
    if (cell.size() > width) {
        cell = width > 3 ? cell.substr(0, width - 3) + "..."
                         : cell.substr(0, width);
    }
    cell.append(width - cell.size(), ' ');
    return cell;
}

void wrapInto(std::vector<std::string>& out, const std::string& prefix,
              const std::string& text, size_t width) {
    if (text.empty()) return;
    const std::string full = prefix + text;
    if (width < 12 || full.size() <= width) {
        out.push_back(full);
        return;
    }
    size_t indentSize = 0;
    while (indentSize < prefix.size() && prefix[indentSize] == ' ')
        ++indentSize;
    if (indentSize + 12u > width) indentSize = 0;
    const std::string indent(indentSize, ' ');
    size_t pos = 0;
    bool first = true;
    while (pos < full.size()) {
        const size_t room = first ? width : width - indentSize;
        size_t take = full.size() - pos < room ? full.size() - pos : room;
        if (pos + take < full.size()) {
            const size_t space = full.rfind(' ', pos + take);
            if (space != std::string::npos && space > pos + room / 3)
                take = space - pos;
        }
        out.push_back((first ? std::string() : indent)
                      + full.substr(pos, take));
        pos += take;
        while (pos < full.size() && full[pos] == ' ') ++pos;
        first = false;
    }
}

const char* ownershipShort(qreplay::SlotOwnership ownership) {
    switch (ownership) {
        case qreplay::SlotOwnership::RecordedHuman: return "human";
        case qreplay::SlotOwnership::NativeCpu:     return "CPU";
        case qreplay::SlotOwnership::Inactive:      break;
    }
    return "inactive";
}

qui::Tone toneFor(qr::BrowserStatusTone tone) {
    switch (tone) {
        case qr::BrowserStatusTone::Ready:     return qui::Tone::Good;
        case qr::BrowserStatusTone::Pending:   return qui::Tone::Dim;
        case qr::BrowserStatusTone::Attention: return qui::Tone::Warn;
        case qr::BrowserStatusTone::Broken:    return qui::Tone::Bad;
        case qr::BrowserStatusTone::Recording: return qui::Tone::Accent;
    }
    return qui::Tone::Default;
}

struct ListColumns {
    size_t date = 16;
    size_t match = 32;
    size_t score = 6;
    size_t length = 6;
    size_t status = 20;
    size_t total = 0;
};

ListColumns listColumnsFor(size_t width) {
    ListColumns columns{};
    const size_t fixed = 2u + columns.date + 2u + 2u + columns.score + 2u
        + columns.length + 2u + columns.status;
    columns.match = width > fixed + 16u ? width - fixed : 16u;
    if (columns.match > kMatchColumnCap) columns.match = kMatchColumnCap;
    columns.total = fixed + columns.match;
    return columns;
}

std::string listHeaderFor(const ListColumns& columns) {
    return "  " + fitCell("Recorded", columns.date) + "  "
        + fitCell("Match", columns.match) + "  "
        + fitCell("Rounds", columns.score) + "  "
        + fitCell("Length", columns.length) + "  Status";
}

std::string listLineFor(const qr::BrowserRow& row,
                        const ListColumns& columns) {
    std::string label = row.matchLabelText;
    if (row.inTrash) label += "  (Trash)";
    const std::string score = row.scoreText == "..." ? "-" : row.scoreText;
    std::string text = row.favorite ? "* " : "  ";
    text += fitCell(row.dateText, columns.date);
    text += "  ";
    text += fitCell(label, columns.match);
    text += "  ";
    text += fitCell(score, columns.score);
    text += "  ";
    text += fitCell(row.durationText, columns.length);
    text += "  ";
    return text;
}

std::string participantSummaryFor(const qr::BrowserDetails& details) {
    std::string out;
    for (const qr::RosterParticipant& participant : details.participants) {
        std::string who =
            participant.ownership == qreplay::SlotOwnership::NativeCpu
                ? std::string("CPU")
                : participant.laneName;
        if (who.empty()) who = "P" + std::to_string(participant.slot + 1);
        if (!out.empty()) out += "  -  ";
        out += who + " plays " + participant.displayName;
    }
    return out;
}

std::string matchCountTextFor(uint32_t count) {
    return std::to_string(count) + (count == 1u ? " match" : " matches");
}

std::vector<std::string> plainWarningsFor(const qr::BrowserDetails& details) {
    std::vector<std::string> out;
    bool unknownCharacter = false;
    bool proofMismatch = false;
    for (const qr::RosterParticipant& participant : details.participants) {
        if (participant.unknownId) unknownCharacter = true;
        if (participant.selectorProofPresent
            && !participant.selectorProofVerified) {
            proofMismatch = true;
        }
    }
    if (unknownCharacter) {
        out.push_back("This replay uses a character this copy of the game "
                      "doesn't know about.");
    }
    if (proofMismatch) {
        out.push_back("The character data in this file doesn't match what "
                      "was recorded.");
    }
    if (!details.provenanceText.empty())
        out.push_back(details.provenanceText);
    return out;
}

std::vector<qui::Row> buildSummaryRows(const qr::BrowserDetails& details,
                                       size_t width) {
    std::vector<qui::Row> rows;
    if (!details.valid) {
        rows.push_back({ { "  Nothing selected.", qui::Tone::Dim } });
        return rows;
    }
    const std::string right = details.sourceFriendlyText + "  |  "
        + matchCountTextFor(details.matchCount) + "  |  "
        + details.durationText;
    const size_t titleRoom = width > right.size() + 4u
        ? width - right.size() - 4u
        : width;
    rows.push_back({ { "  ", qui::Tone::Default },
                     { fitCell(details.titleText, titleRoom),
                       qui::Tone::Accent },
                     { "  " + right, qui::Tone::Dim } });

    std::vector<std::string> lines;

    std::string status;
    if (details.statusTone != qr::BrowserStatusTone::Ready) {
        status = details.watchabilityText;
        if (!details.verificationText.empty())
            status += "  " + details.verificationText;
    }
    if (!status.empty()) {
        lines.clear();
        wrapInto(lines, "", status, width);
        for (const std::string& line : lines)
            rows.push_back({ { "  " + line, toneFor(details.statusTone) } });
    }

    for (const std::string& warning : plainWarningsFor(details)) {
        lines.clear();
        wrapInto(lines, "! ", warning, width);
        for (const std::string& line : lines)
            rows.push_back({ { "  " + line, qui::Tone::Warn } });
    }
    if (!details.noteText.empty() || !details.tags.empty()) {
        std::string notes;
        if (!details.tags.empty()) {
            for (const std::string& tag : details.tags) {
                notes += notes.empty() ? "Tags: " : ", ";
                notes += tag;
            }
        }
        if (!details.noteText.empty()) {
            if (!notes.empty()) notes += "   ";
            notes += "Note: " + details.noteText;
        }
        lines.clear();
        wrapInto(lines, "", notes, width);
        for (const std::string& line : lines)
            rows.push_back({ { "  " + line, qui::Tone::Dim } });
    }
    return rows;
}

std::vector<std::string> buildDetailLines(const qr::BrowserDetails& details,
                                          const std::wstring& root,
                                          size_t width,
                                          bool listPane = false) {
    std::vector<std::string> lines;
    if (!details.valid) {
        lines.push_back("No replay selected.");
        return lines;
    }
    wrapInto(lines, "", details.titleText, width);
    if (!details.recordedTitleText.empty()
        && details.recordedTitleText != details.titleText) {
        wrapInto(lines, "", "Renamed by you - recorded as \""
                     + details.recordedTitleText + "\"", width);
    }
    wrapInto(lines, "", details.dateText + "     "
                 + details.sourceFriendlyText + "     "
                 + details.durationText + "     "
                 + matchCountTextFor(details.matchCount), width);
    lines.push_back("");

    lines.push_back(details.statusShort);
    const bool quiet =
        listPane && details.statusTone == qr::BrowserStatusTone::Ready;
    if (!quiet) {
        wrapInto(lines, "  ", details.watchabilityText, width);
        if (!details.verificationText.empty())
            wrapInto(lines, "  ", details.verificationText, width);
    }
    if (!details.fileCheckText.empty()) {
        wrapInto(lines, "  ", details.fileCheckText
                     + " Use M > Check this file if you want that.", width);
    }
    for (const std::string& warning : plainWarningsFor(details))
        wrapInto(lines, "  ! ", warning, width);
    lines.push_back("");

    lines.push_back("Players");
    for (const qr::RosterParticipant& participant : details.participants) {
        std::string who = participant.laneName.empty()
            ? "P" + std::to_string(participant.slot + 1)
            : participant.laneName;
        std::string line = fitCell(who, 12u) + " plays "
            + fitCell(participant.displayName, 16u) + "  side "
            + std::to_string(participant.side + 1);
        if (participant.ownership == qreplay::SlotOwnership::NativeCpu)
            line += "  (CPU)";
        wrapInto(lines, "  ", line, width);
    }

    lines.push_back("");
    lines.push_back("Result");
    wrapInto(lines, "  ", details.scoreText, width);
    if (!details.seriesText.empty())
        wrapInto(lines, "  ", details.seriesText, width);

    if (!details.matches.empty()) {
        lines.push_back("");
        lines.push_back("Matches");
        for (const qr::BrowserMatchRow& match : details.matches) {
            std::string line = fitCell(std::to_string(match.ordinal), 4u)
                + fitCell(match.outcomeText, 22u)
                + fitCell(match.durationText, 8u);
            if (match.roundCount != 0) {
                line += std::to_string(match.roundCount)
                    + (match.roundCount == 1u ? " round" : " rounds");
            }
            if (!match.complete) line += "  (incomplete)";
            wrapInto(lines, "  ", line, width);
        }
        if (details.matchesTruncated) {
            wrapInto(lines, "  ",
                     "Only the first matches are listed here. Use "
                     "M > Check this file to read the whole index.", width);
        }
    }

    if (!details.tags.empty() || !details.noteText.empty()) {
        lines.push_back("");
        lines.push_back("Your notes");
        if (!details.tags.empty()) {
            std::string tags;
            for (const std::string& tag : details.tags) {
                if (!tags.empty()) tags += ", ";
                tags += tag;
            }
            wrapInto(lines, "  Tags: ", tags, width);
        }
        wrapInto(lines, "  Note: ", details.noteText, width);
    }

    lines.push_back("");
    lines.push_back("File");
    wrapInto(lines, "  ", details.storageText + "     " + details.sizeText
                 + "     " + qplat::narrow(root), width);
    return lines;
}

std::vector<std::string> buildDeveloperLines(const qr::BrowserDetails& details,
                                             const Settings& s, size_t width) {
    std::vector<std::string> lines;
    if (!details.valid) {
        lines.push_back("No replay selected.");
        return lines;
    }
    wrapInto(lines, "", details.storageText, width);
    lines.push_back("");

    lines.push_back("Compatibility");
    wrapInto(lines, "  state           ", details.statusBadge, width);
    wrapInto(lines, "  detail          ", details.statusDetail, width);
    wrapInto(lines, "  verification    ",
             qreplay::replayVerificationModeTextV2(details.verificationMode),
             width);
    wrapInto(lines, "  reason          ",
             details.incompatibilityText.empty()
                 ? std::string("none")
                 : details.incompatibilityText,
             width);
    lines.push_back("");

    lines.push_back("Integrity");
    wrapInto(lines, "  file            ", details.integrityText, width);
    wrapInto(lines, "  simulation      ", details.simulationVerificationText,
             width);
    lines.push_back(std::string("  recorded summary only   ")
                    + (details.recordedSummaryOnly ? "yes" : "no"));
    lines.push_back("");

    lines.push_back("Container");
    lines.push_back("  matches " + std::to_string(details.matchCount)
                    + "    frames " + std::to_string(details.totalFrames)
                    + "    truncated "
                    + (details.matchesTruncated ? "yes" : "no"));
    wrapInto(lines, "  first playable  ", details.firstPlayableText, width);
    wrapInto(lines, "  route           ", details.routeText, width);
    wrapInto(lines, "  stage           ", details.stageText, width);
    wrapInto(lines, "  cpu contract    ", details.cpuContractText, width);
    lines.push_back("");

    lines.push_back("Network");
    if (s.showReplayNetworkDetails)
        wrapInto(lines, "  ", details.networkText, width);
    else
        lines.push_back("  hidden (Settings -> Replays and storage)");
    lines.push_back("");

    lines.push_back("Roster");
    for (const qr::RosterParticipant& participant : details.participants) {
        std::string line = "slot "
            + std::to_string(static_cast<unsigned>(participant.slot))
            + "  side " + std::to_string(static_cast<unsigned>(participant.side))
            + "  " + fitCell(participant.displayName, 18u)
            + " id "
            + std::to_string(static_cast<unsigned>(participant.characterId))
            + "  pal "
            + std::to_string(static_cast<unsigned>(participant.palette))
            + "  " + ownershipShort(participant.ownership)
            + "  lane "
            + std::to_string(static_cast<unsigned>(participant.sourceLane));
        if (!participant.laneName.empty())
            line += " \"" + participant.laneName + "\"";
        if (participant.selectorProofPresent) {
            line += participant.selectorProofVerified ? "  proof verified"
                                                      : "  proof MISMATCH";
        }
        if (participant.unknownId) line += "  [not in this build's catalog]";
        if (participant.hiddenColumn) line += "  [hidden column]";
        if (participant.standAlternate) line += "  [alternate]";
        wrapInto(lines, "  ", line, width);
    }

    if (!details.matches.empty()) {
        lines.push_back("");
        lines.push_back("Matches");
        for (const qr::BrowserMatchRow& match : details.matches) {
            std::string line = "match " + std::to_string(match.ordinal)
                + "  " + match.resultText + "  " + match.durationText
                + "  frames " + std::to_string(match.frameCount)
                + "  rounds " + std::to_string(match.roundCount);
            if (!match.independentBootstrap) line += "  (dependent bootstrap)";
            if (!match.complete) line += "  (incomplete)";
            wrapInto(lines, "  ", line, width);
        }
    }

    lines.push_back("");
    lines.push_back("Identity");
    for (const std::string& identity : details.identityLines)
        wrapInto(lines, "  ", identity, width);

    if (!details.warnings.empty()) {
        lines.push_back("");
        lines.push_back("Warnings");
        for (const std::string& warning : details.warnings)
            wrapInto(lines, "  ! ", warning, width);
    }

    lines.push_back("");
    lines.push_back("Advanced");
    lines.push_back("  V  Check this file        "
                    "Y  Play and check against your game");
    return lines;
}

uint64_t freeMegabytesAt(const std::wstring& root) {
    ULARGE_INTEGER freeForCaller{};
    ULARGE_INTEGER total{};
    ULARGE_INTEGER free{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &freeForCaller, &total, &free))
        return 0;
    return static_cast<uint64_t>(freeForCaller.QuadPart / (1024ull * 1024ull));
}

enum class ReplayBucket { All, Network, Offline, Spectate };

const char* replayBucketName(ReplayBucket bucket) {
    switch (bucket) {
        case ReplayBucket::Network:  return "Network";
        case ReplayBucket::Offline:  return "Offline";
        case ReplayBucket::Spectate: return "Spectated";
        case ReplayBucket::All:      break;
    }
    return "All";
}

ReplayBucket nextReplayBucket(ReplayBucket bucket) {
    switch (bucket) {
        case ReplayBucket::All:      return ReplayBucket::Network;
        case ReplayBucket::Network:  return ReplayBucket::Offline;
        case ReplayBucket::Offline:  return ReplayBucket::Spectate;
        case ReplayBucket::Spectate: break;
    }
    return ReplayBucket::All;
}

struct BattleIndex {
    bool readable = false;
    std::vector<qr::BattleLogRowV1> rows;
    std::map<std::pair<uint64_t, uint64_t>, size_t> byGame;
    size_t unloggedGames = 0u;
};

struct Page {
    qr::ReplayScanner scanner;
    qr::ReplayAnnotationStore annotations;
    std::vector<std::wstring> folders;
    int browseIndex = 0;
    BattleIndex battleLog;
    qr::ReplayBrowserModel model;
    std::wstring root;
    std::wstring annotationsPath;
    uint64_t shownGeneration = 0;
    bool haveSnapshot = false;
    bool refreshAfterAction = false;
    bool detailsPage = false;
    bool developerPage = false;
    size_t detailsScroll = 0;
    uint64_t loggedStableId = 0;
    ReplayBucket bucket = ReplayBucket::All;
    std::string notice;
    qui::Tone noticeTone = qui::Tone::Dim;
};

const char* actionLabel(qr::BrowserAction action) {
    switch (action) {
        case qr::BrowserAction::WatchSet: return "Watch";
        case qr::BrowserAction::WatchMatch: return "Watch one match";
        case qr::BrowserAction::VerifyFile: return "Check this file";
        case qr::BrowserAction::VerifySimulation: return "Play and check";
        case qr::BrowserAction::Rename: return "Rename";
        case qr::BrowserAction::ToggleFavorite: return "Favourite";
        case qr::BrowserAction::EditTags: return "Tags";
        case qr::BrowserAction::EditNote: return "Note";
        case qr::BrowserAction::ExportMatch: return "Save one match";
        case qr::BrowserAction::ImportExternal: return "Open a replay file";
        case qr::BrowserAction::RevealFolder: return "Show in Explorer";
        case qr::BrowserAction::MoveToTrash: return "Move to Trash";
        case qr::BrowserAction::UndoTrash: return "Undo delete";
        case qr::BrowserAction::PermanentPurge: return "Delete permanently";
        case qr::BrowserAction::ClearFilters: return "Clear search";
        case qr::BrowserAction::Refresh: return "Refresh";
        case qr::BrowserAction::StartAtRound:
        case qr::BrowserAction::Count:
            break;
    }
    return qr::browserActionText(action);
}

const char* actionMenuLabel(qr::BrowserAction action) {
    switch (action) {
        case qr::BrowserAction::WatchSet: return "Watch";
        case qr::BrowserAction::WatchMatch: return "Watch just one match";
        case qr::BrowserAction::ExportMatch:
            return "Save one match as its own replay";
        case qr::BrowserAction::EditNote: return "Add a note";
        case qr::BrowserAction::EditTags: return "Edit tags";
        case qr::BrowserAction::ImportExternal: return "Open a replay file...";
        case qr::BrowserAction::RevealFolder: return "Show in Explorer";
        case qr::BrowserAction::VerifyFile: return "Check this file";
        case qr::BrowserAction::UndoTrash: return "Undo delete";
        case qr::BrowserAction::PermanentPurge:
            return "Delete permanently (cannot be undone)";
        case qr::BrowserAction::ClearFilters: return "Clear search and filters";
        case qr::BrowserAction::Refresh: return "Refresh the list";
        default:
            break;
    }
    return actionLabel(action);
}

void setNotice(Page& page, const std::string& text, qui::Tone tone) {
    page.notice = text;
    page.noticeTone = tone;
}

void reportOutcome(Page& page, const qr::ActionOutcome& outcome) {
    const std::string action = actionLabel(outcome.action);
    if (outcome.ok) {
        if ((!outcome.producedPath.empty()
             && outcome.action != qr::BrowserAction::RevealFolder)
            || outcome.action == qr::BrowserAction::PermanentPurge)
            page.refreshAfterAction = true;
        switch (outcome.action) {
            case qr::BrowserAction::ToggleFavorite: {
                const bool favorite = page.model.rowCount() != 0u
                    && page.model.row(page.model.selectedIndex()).favorite;
                setNotice(page, favorite ? "Added to favourites."
                                         : "Removed from favourites.",
                          qui::Tone::Good);
                return;
            }
            case qr::BrowserAction::MoveToTrash:
                setNotice(page, "Moved to Trash. Press U to put "
                                "it back.", qui::Tone::Good);
                return;
            case qr::BrowserAction::UndoTrash:
                setNotice(page, "Put back from the Trash.", qui::Tone::Good);
                return;
            default:
                break;
        }
        std::string text = action + ": done";
        if (!outcome.message.empty()) text = action + ": " + outcome.message;
        if (!outcome.producedPath.empty())
            text += " -> " + qplat::narrow(outcome.producedPath);
        setNotice(page, text, qui::Tone::Good);
        return;
    }
    std::string why = outcome.reason != qr::ActionDisableReason::None
        ? qr::actionDisableReasonPlainText(outcome.reason)
        : outcome.message;
    if (why.empty()) why = outcome.message;
    if (why.empty()) why = "not available";
    setNotice(page, action + ": " + why, qui::Tone::Warn);
}

qr::ActionOutcome runAction(qui::Console& con, Page& page,
                            const qr::ActionRequest& request) {
    qr::ActionOutcome outcome = page.model.requestAction(request);
    if (!outcome.needsConfirmation) return outcome;
    const int choice = con.menu(
        outcome.confirmationPrompt.empty() ? std::string("Are you sure?")
                                           : outcome.confirmationPrompt,
        { std::string("Yes - ") + actionLabel(request.action) },
        "No, cancel");
    if (choice != 0) {
        page.model.cancelPendingAction();
        qr::ActionOutcome cancelled{};
        cancelled.action = request.action;
        cancelled.message = "cancelled";
        return cancelled;
    }
    return page.model.confirmAction(outcome.confirmationToken);
}

void startScan(Page& page, const Settings& s) {
    qr::ReplayScanner::Config config;
    config.roots.push_back(page.root);
    config.includeTrash = s.showTrashedReplays;
    config.useCache = true;
    const std::wstring cacheDir = page.root + L"\\.cache";
    if (qplat::ensureDirW(cacheDir))
        config.cachePath = cacheDir + L"\\catalog.bin";
    else
        config.useCache = false;
    page.scanner.start(config);
}

bool verifiedPlaybackResult(
    const qreplay::ipc::v3::PlaybackResultPayload& result) {
    using Reason = qreplay::ipc::v3::PlaybackResultReason;
    return result.reason == Reason::VerifiedCompleted
        || result.reason == Reason::VerifiedAbnormalEnd
        || result.reason == Reason::VerifiedBehaviourOnly
        || result.reason == Reason::CompletedWithDivergence;
}

const char* playbackResultText(
    qreplay::ipc::v3::PlaybackResultReason reason) {
    using Reason = qreplay::ipc::v3::PlaybackResultReason;
    switch (reason) {
        case Reason::VerifiedCompleted: return "completed game verified";
        case Reason::VerifiedAbnormalEnd: return "recorded abnormal ending verified";
        case Reason::VerifiedBehaviourOnly:
            return "verified complete (behaviour-verified: the byte-exact "
                   "state proof does not apply to this recording)";
        case Reason::CompletedWithDivergence:
            return "played to the end, but stopped matching the recording "
                   "partway through - what you saw after that point is this "
                   "build's simulation, not the original match";
        case Reason::VerifiedUnsealedPrefix: return "unsupported incomplete replay endpoint";
        case Reason::UserExit: return "playback closed by the user";
        case Reason::Unsupported: return "unsupported replay operation";
        case Reason::BrokerFailure: return "replay broker failure";
        case Reason::BootstrapMismatch: return "initial game state mismatch";
        case Reason::FramePreHashMismatch: return "pre-frame state mismatch";
        case Reason::FramePostHashMismatch: return "post-frame state mismatch";
        case Reason::RollingDigestMismatch: return "rolling digest mismatch";
        case Reason::TerminalMismatch: return "terminal state mismatch";
        case Reason::PresentationAckTimeout: return "final presentation timed out";
        case Reason::DeviceIsolationViolation: return "live input isolation violation";
        case Reason::InternalFailure: return "internal replay failure";
    }
    return "unknown replay result";
}

bool watchAndReturn(qui::Console& con, Page& page, Settings& s,
                    const qr::WatchRequest& watch,
                    qreplay::ipc::v3::PlaybackResultPayload* resultOut =
                        nullptr,
                    bool fullPreflight = false) {
    if (!watch.valid || watch.path.empty()) {
        setNotice(page, "That replay cannot be watched.", qui::Tone::Warn);
        return false;
    }

    const qr::BrowserDetails& details = page.model.details();
    if (fullPreflight) {
        std::string body = details.titleText + "\n"
            + (watch.matchId == 0u && watch.matchOrdinal == 0u
                   ? std::string("Whole set")
                   : "Match " + std::to_string(watch.matchOrdinal))
            + (watch.startRound != 0u
                   ? ", from round " + std::to_string(watch.startRound)
                   : std::string())
            + "\n\n"
            + "Status: " + details.statusBadge;
        if (!details.statusDetail.empty()) body += " - " + details.statusDetail;
        if (!details.integrityText.empty())
            body += "\nFile integrity: " + details.integrityText;
        if (!details.simulationVerificationText.empty())
            body += "\nSimulation: " + details.simulationVerificationText;
        if (!details.firstPlayableText.empty())
            body += "\nFirst playable: " + details.firstPlayableText;
        if (!details.incompatibilityText.empty())
            body += "\nLimitations: " + details.incompatibilityText;
        body += std::string("\n\nAudio: ")
              + (s.replayDefaultAudio ? "on" : "off")
              + "   (Settings -> Replays and storage)";
        con.message("About to play", body);
    } else {
        std::string question;
        for (const qr::BrowserMatchRow& match : details.matches) {
            if (watch.matchOrdinal != 0u && match.ordinal == watch.matchOrdinal
                && !match.independentBootstrap) {
                question = "Watching match " + std::to_string(match.ordinal)
                    + " starts playback from an earlier match.";
                break;
            }
        }
        if (question.empty()) {
            for (const qr::BrowserMatchRow& match : details.matches) {
                if (match.complete) continue;
                if (watch.matchOrdinal != 0u
                    && match.ordinal != watch.matchOrdinal) {
                    continue;
                }
                question = "This recording stops partway through and will "
                           "just end.";
                break;
            }
        }
        if (!question.empty()) {
            const int choice = con.menu(question, { "Play anyway" }, "Cancel");
            if (choice != 0) {
                setNotice(page, "Playback cancelled.", qui::Tone::Dim);
                return false;
            }
        }
    }

    const qr::BrowserViewState view = page.model.captureViewState();
    const uint64_t stableId = watch.stableId;
    qreplay::ipc::v3::PlaybackResultPayload playbackResult{};
    bool playbackResultReceived = false;
    con.clear();
    const int playbackRc = runReplayPlayback(
        s, watch.path, watch.matchOrdinal, &con,
        false, &playbackResult,
        &playbackResultReceived);
    if (resultOut != nullptr) *resultOut = playbackResult;
    page.model.restoreViewState(view);
    if (stableId != 0) page.model.selectByStableId(stableId);
    page.scanner.requestRefresh();
    if (playbackRc == 0 && (!playbackResultReceived || playbackResult.reason
            == qreplay::ipc::v3::PlaybackResultReason::UserExit)) {
        setNotice(page, "Replay closed.", qui::Tone::Dim);
    } else if (playbackResultReceived && verifiedPlaybackResult(playbackResult)) {
        setNotice(page,
                  std::string("Playback finished: ")
                      + playbackResultText(playbackResult.reason) + ".",
                  qui::Tone::Good);
    } else {
        setNotice(page,
                  !playbackResultReceived
                      ? "Playback ended without a verified runtime result."
                      : std::string("Playback stopped: ")
                          + playbackResultText(playbackResult.reason) + ".",
                  qui::Tone::Warn);
    }
    return playbackRc == 0 && playbackResultReceived
        && verifiedPlaybackResult(playbackResult);
}

void watchFolderAndReturn(qui::Console& con, Page& page, const Settings& s,
                          const std::wstring& directory) {
    const auto snapshot = page.scanner.snapshot();
    if (!snapshot || !snapshot->complete) {
        setNotice(page, "The replay folder is still loading. Try again when "
                        "the scan finishes.", qui::Tone::Dim);
        return;
    }
    const std::vector<size_t> order = qr::folderPlaybackOrder(*snapshot, directory);
    if (order.empty()) {
        setNotice(page, "That folder has no replays. Open a set folder first.",
                  qui::Tone::Dim);
        return;
    }
    const qr::BrowserViewState view = page.model.captureViewState();
    size_t completed = 0u;
    const auto runtimeIdentity = currentRuntimeIdentityV2(s);
    const auto nextGame = [&](const qreplay::ipc::v3::PlaybackResultPayload& result) {
        qr::ReplayBrokerNextGameV2 next{};
        ++completed;
        if (completed == order.size()) return next;
        const qr::ReplayEntry& entry = snapshot->entries[order[completed]];
        qr::ReplayPreflightInputV2 input{};
        std::string detail;
        if (!loadReplayPreflightInputV2(entry.path, input, detail)) {
            next.error = "Game " + std::to_string(completed + 1u) + ": " + detail;
            return next;
        }
        const auto preflight = qr::prepareReplayV2(input, runtimeIdentity, next.replay);
        if (!preflight.ok()) {
            next.error = "Game " + std::to_string(completed + 1u) + ": "
                + (preflight.detail.empty() ? replayRefusalMessage(preflight.status)
                                           : preflight.detail);
            QLOG("[replay.folder] next game refused status=%s technical='%s'",
                 qr::replayPreflightStatusTextV2(preflight.status),
                 preflight.technicalDetail.c_str());
            return next;
        }
        next.presentationFlags = qipc::kReplayAutoStart
            | (result.presentationFlags & qipc::kReplayDisplayFlagsMask);
        if (completed + 1u == order.size()) next.presentationFlags |= qipc::kReplayHoldFinal;
        QLOG("[replay.folder] next game=%u/%u frames=%u flags=%u",
             static_cast<unsigned>(completed + 1u), static_cast<unsigned>(order.size()),
             next.replay->frameCount(), next.presentationFlags);
        return next;
    };
    uint16_t firstFlags = qipc::kReplayShowTransport;
    if (order.size() == 1u) firstFlags |= qipc::kReplayHoldFinal;
    con.clear();
    info(&con, "Folder playback: " + std::to_string(order.size())
         + " game(s) in one game window.");
    qreplay::ipc::v3::PlaybackResultPayload result{};
    bool received = false;
    const int rc = runReplayPlayback(s, snapshot->entries[order.front()].path,
        0u, &con, false, &result, &received, firstFlags, nextGame);
    if (rc != 0 || completed != order.size()) {
        const bool cancelled = rc == 0 && (!received || result.reason
            == qreplay::ipc::v3::PlaybackResultReason::UserExit);
        setNotice(page, "Folder playback stopped at game "
            + std::to_string((std::min)(completed + 1u, order.size())) + " of "
            + std::to_string(order.size()) + ": "
            + (cancelled ? "Replay closed" : "playback could not continue") + ".",
            cancelled ? qui::Tone::Dim : qui::Tone::Warn);
    }
    page.model.restoreViewState(view);
    if (rc == 0 && completed == order.size())
        setNotice(page, "Folder playback finished: " + std::to_string(completed)
                  + " game(s).", qui::Tone::Good);
}

bool pickMatch(qui::Console& con, Page& page, const std::string& title,
               uint32_t& ordinal) {
    const qr::BrowserDetails& details = page.model.details();
    if (details.matches.empty()) {
        setNotice(page, "This replay has no indexed matches.", qui::Tone::Warn);
        return false;
    }
    std::vector<std::string> items;
    items.reserve(details.matches.size());
    for (const qr::BrowserMatchRow& match : details.matches) {
        std::string label = "Match " + std::to_string(match.ordinal) + "   "
            + match.matchupText + "   " + match.outcomeText + "   "
            + match.durationText;
        if (match.roundCount != 0) {
            label += "   " + std::to_string(match.roundCount)
                + (match.roundCount == 1u ? " round" : " rounds");
        }
        if (!match.complete) label += "   (incomplete)";
        if (!match.independentBootstrap)
            label += "   (needs the earlier matches)";
        items.push_back(label);
    }
    const int choice = con.menu(title, items, "Back");
    if (choice < 0 || static_cast<size_t>(choice) >= details.matches.size())
        return false;
    ordinal = details.matches[static_cast<size_t>(choice)].ordinal;
    page.model.expandMatch(ordinal);
    return true;
}

size_t rebuildBattleLogFromLibrary(qui::Console& con, Page& page,
                                   const std::string& myName) {
    namespace qrp = qclient::replay;
    std::shared_ptr<const qr::ReplaySnapshot> snapshot =
        page.scanner.snapshot();
    if (!snapshot) return 0u;

    std::string existingText;
    std::set<std::pair<uint64_t, uint64_t>> known;
    if (qrp::readBattleLogFile(page.root, existingText)) {
        for (const qrp::BattleLogRowV1& row :
                 qrp::decodeBattleLogV1(existingText)) {
            known.insert({ row.gameUuidHigh, row.gameUuidLow });
        }
    }

    std::vector<const qr::ReplayEntry*> ordered;
    ordered.reserve(snapshot->entries.size());
    for (const qr::ReplayEntry& entry : snapshot->entries) {
        if (!entry.isV2 || !entry.hasEndV2 || entry.inTrash) continue;
        if (entry.isPart) continue;
        ordered.push_back(&entry);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const qr::ReplayEntry* a, const qr::ReplayEntry* b) {
                  const int64_t at = a->startV2.manifest.display
                                        .recordedUnixSeconds;
                  const int64_t bt = b->startV2.manifest.display
                                        .recordedUnixSeconds;
                  if (at != bt) return at < bt;
                  return a->startV2.manifest.identity.gameGeneration
                       < b->startV2.manifest.identity.gameGeneration;
              });

    size_t written = 0u;
    for (const qr::ReplayEntry* const entry : ordered) {
        const qreplay::GameManifestV2& manifest = entry->startV2.manifest;
        const std::pair<uint64_t, uint64_t> id(manifest.identity.gameUuidHigh,
                                               manifest.identity.gameUuidLow);
        if (known.count(id) != 0u) continue;

        qrp::BattleLogRowV1 row;
        row.gameUuidHigh = id.first;
        row.gameUuidLow = id.second;
        row.setUuidHigh = manifest.display.seriesUuidHigh;
        row.setUuidLow = manifest.display.seriesUuidLow;
        row.gameNo = manifest.identity.gameGeneration;
        row.startUnix = manifest.display.recordedUnixSeconds;
        row.setStartUnix = manifest.display.recordedUnixSeconds;
        row.timezoneOffsetMinutes = manifest.display.timezoneOffsetMinutes;
        row.sourceKind = static_cast<uint8_t>(manifest.display.sourceKind);

        switch (qr::resolvedSourceOf(*entry)) {
            case qr::ReplaySourceResolved::Offline:
                row.mySide = qrp::kBattleSideOffline;
                break;
            case qr::ReplaySourceResolved::Netplay: {
                const std::string one =
                    qreplay::replayTextV2(manifest.lanes[0].name);
                const std::string two =
                    qreplay::replayTextV2(manifest.lanes[1].name);
                if (!myName.empty() && one == myName && two != myName) {
                    row.mySide = qrp::kBattleSideOne;
                } else if (!myName.empty() && two == myName && one != myName) {
                    row.mySide = qrp::kBattleSideTwo;
                }
                break;
            }
            default:
                break;
        }

        row.characterOne = manifest.slots[0].identity.nativeCharacterId;
        row.characterTwo = manifest.slots[1].identity.nativeCharacterId;
        row.paletteOne = manifest.slots[0].identity.paletteIndex;
        row.paletteTwo = manifest.slots[1].identity.paletteIndex;
        row.characterCatalogId = manifest.contract.characterCatalogId;

        row.winningSideMask = entry->endV2.winningSideMask;
        row.roundsOne = static_cast<int16_t>(entry->endV2.roundTallies[0]);
        row.roundsTwo = static_cast<int16_t>(entry->endV2.roundTallies[1]);
        row.terminalReason = static_cast<uint16_t>(entry->endV2.reason);
        row.frames = entry->totalFrames;
        row.roundsToWin =
            entry->startV2.start.configuration.roundsToWinMinusOne + 1u;
        row.stageId = manifest.stage.nativeStageId;
        row.bytes = entry->identity.sizeBytes;
        uint64_t digest = 0u;
        for (size_t i = 0u; i < 8u; ++i) {
            digest = (digest << 8)
                | static_cast<uint64_t>(entry->endV2.canonicalGameDigest[i]);
        }
        row.digest = digest;
        row.nameOne = qreplay::replayTextV2(manifest.lanes[0].name);
        row.nameTwo = qreplay::replayTextV2(manifest.lanes[1].name);
        row.myName = myName;
        std::string relative;
        if (!entry->relativeDir.empty())
            relative = qplat::narrow(entry->relativeDir) + "\\";
        relative += qplat::narrow(entry->fileName);
        row.relativePath = relative;

        if (!qrp::appendBattleLogRow(page.root, row)) {
            con.message(qi18n::tr("Rebuild stopped"),
                        qi18n::tr("The battle log could not be written. "
                                  "What was rebuilt up to that point is "
                                  "kept."));
            break;
        }
        known.insert(id);
        ++written;
    }
    return written;
}

bool offerBattleLogRebuild(qui::Console& con, Page& page, Settings& s) {
    const int choice = con.menu(
        "Rebuild from the replay library",
        { "Rebuild, and my name in those recordings is \"" + s.nickname
              + "\"",
          "Rebuild without a name  (play time only, no wins or losses)" },
        "Back");
    if (choice < 0 || choice > 1) return false;

    std::string myName = choice == 0 ? s.nickname : std::string();
    if (choice == 0 && !con.promptLine(
            "Which name is yours in those recordings?", myName, s.nickname)) {
        return false;
    }
    const size_t written = rebuildBattleLogFromLibrary(con, page, myName);
    if (written == 0u) {
        con.message(qi18n::tr("Nothing to rebuild"),
                    qi18n::tr("No replay in the library carries a game the "
                              "log does not already have. Recordings made by "
                              "older builds cannot be rebuilt from."));
        return false;
    }
    con.message(qi18n::tr("Rebuilt"),
                std::to_string(written) + " "
                    + qi18n::tr("games were added to the battle log from the "
                                "replay library. What each recording could "
                                "not say - the length of a set's individual "
                                "games, and which side you were on where the "
                                "name did not pick one out - is left out "
                                "rather than guessed."));
    return true;
}

std::string battleCharacterName(uint8_t id) {
    if (id == qr::kBattleCharacterUnknown) return "?";
    const std::string name =
        qreplay::CharacterCatalog::forSeBuild().displayName(id);
    return name.empty() ? std::to_string(static_cast<unsigned>(id)) : name;
}

std::string battleDateText(int64_t unixSeconds, int32_t offsetMinutes) {
    if (unixSeconds <= 0) return std::string("--");
    int month = 1;
    int day = 1;
    qr::civilMonthDayFromUnix(unixSeconds, offsetMinutes, month, day);
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "%02d-%02d", month, day);
    return std::string(buffer);
}

std::string battleNameText(const std::string& name) {
    return name.empty() ? std::string(qi18n::tr("(no name)")) : name;
}

std::string battleRecordText(const qr::BattleCountV2& count) {
    std::string text = qr::battleCountText(count);
    const int32_t rate = qr::battleWinRateTenths(count);
    if (rate >= 0) {
        char percent[24];
        std::snprintf(percent, sizeof(percent), " (%d.%d%%)",
                      static_cast<int>(rate / 10), static_cast<int>(rate % 10));
        text += percent;
    }
    return text;
}

bool battleEntryGameKey(const qr::ReplayEntry& entry,
                        std::pair<uint64_t, uint64_t>& key) {
    if (!entry.isV2 || !entry.hasManifest) return false;
    key = { entry.startV2.manifest.identity.gameUuidHigh,
            entry.startV2.manifest.identity.gameUuidLow };
    return true;
}

void countUnloggedGames(Page& page) {
    page.battleLog.unloggedGames = 0u;
    if (!page.battleLog.readable) return;
    std::shared_ptr<const qr::ReplaySnapshot> snapshot =
        page.scanner.snapshot();
    if (!snapshot) return;
    for (const qr::ReplayEntry& entry : snapshot->entries) {
        if (!entry.hasEndV2 || entry.inTrash || entry.isPart) continue;
        std::pair<uint64_t, uint64_t> key;
        if (!battleEntryGameKey(entry, key)) continue;
        if (page.battleLog.byGame.count(key) == 0u)
            ++page.battleLog.unloggedGames;
    }
}

void loadBattleIndex(Page& page, const Settings& s) {
    page.battleLog = BattleIndex{};
    if (!s.battleLog) return;
    std::string text;
    if (!qr::readBattleLogFile(page.root, text)) return;
    page.battleLog.readable = true;
    page.battleLog.rows = qr::decodeBattleLogV1(text);
    for (size_t i = 0u; i < page.battleLog.rows.size(); ++i) {
        const qr::BattleLogRowV1& row = page.battleLog.rows[i];
        const std::pair<uint64_t, uint64_t> key(row.gameUuidHigh,
                                                row.gameUuidLow);
        auto found = page.battleLog.byGame.find(key);
        if (found == page.battleLog.byGame.end()) {
            page.battleLog.byGame.emplace(key, i);
            continue;
        }
        const bool keptLocal =
            qr::battleGameVerdict(page.battleLog.rows[found->second])
            != qr::BattleGameVerdict::NoLocalSide;
        const bool thisLocal = qr::battleGameVerdict(row)
            != qr::BattleGameVerdict::NoLocalSide;
        if (!keptLocal && thisLocal) found->second = i;
    }
    countUnloggedGames(page);
}

const qr::BattleLogRowV1* battleRowFor(const Page& page,
                                       const qr::ReplayEntry* entry) {
    if (entry == nullptr) return nullptr;
    std::pair<uint64_t, uint64_t> key;
    if (!battleEntryGameKey(*entry, key)) return nullptr;
    const auto found = page.battleLog.byGame.find(key);
    return found == page.battleLog.byGame.end()
        ? nullptr : &page.battleLog.rows[found->second];
}

std::vector<qr::BattleLogRowV1> battleRowsUnder(const Page& page,
                                                const std::wstring& dir) {
    if (dir.empty()) return page.battleLog.rows;
    std::vector<qr::BattleLogRowV1> rows;
    std::shared_ptr<const qr::ReplaySnapshot> snapshot =
        page.scanner.snapshot();
    if (!snapshot) return rows;
    std::set<std::pair<uint64_t, uint64_t>> seen;
    for (const qr::ReplayEntry& entry : snapshot->entries) {
        if (entry.inTrash) continue;
        const std::wstring& at = entry.relativeDir;
        const bool inside = at == dir
            || (at.size() > dir.size() && at.compare(0, dir.size(), dir) == 0
                && at[dir.size()] == L'\\');
        if (!inside) continue;
        const qr::BattleLogRowV1* row = battleRowFor(page, &entry);
        if (row == nullptr) continue;
        if (!seen.insert({ row->gameUuidHigh, row->gameUuidLow }).second)
            continue;
        rows.push_back(*row);
    }
    return rows;
}

bool battleSingleSet(const std::vector<qr::BattleLogRowV1>& rows) {
    if (rows.empty()) return false;
    const uint64_t high = rows.front().setUuidHigh;
    const uint64_t low = rows.front().setUuidLow;
    if (high == 0u && low == 0u) return false;
    for (const qr::BattleLogRowV1& row : rows) {
        if (row.setUuidHigh != high || row.setUuidLow != low) return false;
    }
    return true;
}

std::string battleSetLine(const qr::BattleSetSummary& set) {
    std::string line = battleDateText(set.startUnix,
                                      set.timezoneOffsetMinutes) + "  ";
    if (set.hasLocalSide) {
        line += std::string(qi18n::tr("vs")) + " "
              + battleNameText(set.opponent) + "  "
              + battleRecordText(set.result);
    } else {
        line += qi18n::tr("(spectated)");
    }
    line += "  " + std::to_string(set.games) + " "
          + (set.games == 1u ? qi18n::tr("game") : qi18n::tr("games"))
          + "  " + qr::battleDurationText(set.seconds);
    return line;
}

std::string battleGameResultLine(const qr::BattleLogRowV1& row) {
    const qr::BattleGameVerdict verdict = qr::battleGameVerdict(row);
    std::string rounds;
    if (row.roundsOne >= 0 && row.roundsTwo >= 0) {
        const bool mineFirst = row.mySide != qr::kBattleSideTwo;
        rounds = " " + std::to_string(mineFirst ? row.roundsOne : row.roundsTwo)
               + "-" + std::to_string(mineFirst ? row.roundsTwo : row.roundsOne);
    }
    if (verdict == qr::BattleGameVerdict::NoLocalSide)
        return std::string(qi18n::tr(qr::battleGameSidesText(row))) + rounds;
    return std::string(qi18n::tr(qr::battleGameVerdictText(verdict))) + rounds;
}

std::string battleGameMatchup(const qr::BattleLogRowV1& row) {
    const bool mineSecond = row.mySide == qr::kBattleSideTwo;
    return battleCharacterName(mineSecond ? row.characterTwo
                                          : row.characterOne)
        + " vs "
        + battleCharacterName(mineSecond ? row.characterOne
                                         : row.characterTwo);
}

qui::Tone battleVerdictTone(qr::BattleGameVerdict verdict) {
    switch (verdict) {
        case qr::BattleGameVerdict::Won:  return qui::Tone::Good;
        case qr::BattleGameVerdict::Lost: return qui::Tone::Bad;
        default:                          return qui::Tone::Dim;
    }
}

std::string battleHeadlineFor(const Page& page) {
    if (!page.battleLog.readable) return std::string();
    const std::wstring& scope = page.model.filter().directoryScope;
    const std::vector<qr::BattleLogRowV1> rows = battleRowsUnder(page, scope);
    if (rows.empty()) {
        if (scope.empty() && page.battleLog.unloggedGames != 0u) {
            return " " + std::string(qi18n::tr("Battle log is empty."))
                 + " " + qi18n::tr("L builds it from the finished games here.");
        }
        return std::string();
    }
    const qr::BattleStatsSummary summary = qr::summarizeBattleLog(rows);
    if (battleSingleSet(rows) && !summary.sets_.empty())
        return " " + std::string(qi18n::tr("This set")) + "  "
             + battleSetLine(summary.sets_.front());
    std::string line = " " + std::string(scope.empty()
                                              ? qi18n::tr("Battle log")
                                              : qi18n::tr("In here"));
    if (summary.countedGames != 0u) {
        line += "  " + battleRecordText(summary.games) + "  |  "
              + std::to_string(summary.countedSets) + " "
              + qi18n::tr("sets") + " " + qr::battleCountText(summary.sets);
    } else {
        line += "  " + std::to_string(summary.rows) + " "
              + qi18n::tr("games");
    }
    line += "  |  " + qr::battleDurationText(summary.totalSeconds);
    if (!summary.recentOpponent.empty()) {
        line += "  |  " + std::string(qi18n::tr("Last played")) + " "
              + summary.recentOpponent;
    }
    return line;
}

std::vector<qui::Row> battleGameRows(const Page& page,
                                     const qr::ReplayEntry* entry,
                                     size_t width) {
    std::vector<qui::Row> out;
    if (!page.battleLog.readable || entry == nullptr) return out;
    const qr::BattleLogRowV1* row = battleRowFor(page, entry);
    if (row == nullptr) {
        if (entry->isV2 && entry->hasEndV2) {
            out.push_back({ { fitCell(std::string("  ")
                                  + qi18n::tr("Not in the battle log.")
                                  + (page.battleLog.unloggedGames != 0u
                                         ? std::string("  ")
                                               + qi18n::tr("L adds the "
                                                           "library's games "
                                                           "to it.")
                                         : std::string()),
                              width),
                              qui::Tone::Dim } });
        }
        return out;
    }
    const qr::BattleGameVerdict verdict = qr::battleGameVerdict(*row);
    out.push_back({ { "  " + fitCell(std::string(qi18n::tr("This game")), 11u),
                      qui::Tone::Dim },
                    { fitCell(battleGameResultLine(*row), 12u),
                      battleVerdictTone(verdict) },
                    { fitCell(battleGameMatchup(*row) + "   "
                                  + qr::battleDurationText(
                                        qr::battleLogSecondsFromFrames(
                                            row->frames)),
                              width > 25u ? width - 25u : 0u),
                      qui::Tone::Default } });

    if (row->setUuidHigh != 0u || row->setUuidLow != 0u) {
        const std::vector<qr::BattleLogRowV1> games = qr::battleSetGames(
            page.battleLog.rows, row->setUuidHigh, row->setUuidLow);
        const qr::BattleStatsSummary set = qr::summarizeBattleLog(games);
        if (games.size() > 1u && !set.sets_.empty()) {
            size_t position = 0u;
            for (size_t i = 0u; i < games.size(); ++i) {
                if (games[i].gameUuidHigh == row->gameUuidHigh
                    && games[i].gameUuidLow == row->gameUuidLow) {
                    position = i + 1u;
                }
            }
            std::string text = battleSetLine(set.sets_.front());
            if (position != 0u) {
                text += "  (" + std::string(qi18n::tr("game")) + " "
                      + std::to_string(position) + ")";
            }
            out.push_back({ { "  " + fitCell(std::string(qi18n::tr("This set")),
                                             11u),
                              qui::Tone::Dim },
                            { fitCell(text, width > 13u ? width - 13u : 0u),
                              qui::Tone::Default } });
        }
    }

    const std::string opponent = qr::battleOpponentOf(*row);
    if (verdict != qr::BattleGameVerdict::NoLocalSide && !opponent.empty()) {
        const qr::BattleStatsSummary against =
            qr::summarizeAgainst(page.battleLog.rows, opponent);
        if (against.countedGames > 1u) {
            out.push_back({ { "  " + fitCell(std::string(qi18n::tr("Against"))
                                                 + " " + opponent, 11u),
                              qui::Tone::Dim },
                            { fitCell(battleRecordText(against.games) + "  |  "
                                          + std::to_string(against.countedSets)
                                          + " " + qi18n::tr("sets") + " "
                                          + qr::battleCountText(against.sets)
                                          + "  |  "
                                          + qr::battleDurationText(
                                                against.totalSeconds),
                                      width > 13u ? width - 13u : 0u),
                              qui::Tone::Default } });
        }
    }
    return out;
}

std::vector<qui::Row> battleFolderRows(const Page& page,
                                       const std::wstring& dir, size_t width,
                                       size_t maxLines) {
    std::vector<qui::Row> out;
    if (!page.battleLog.readable || maxLines == 0u) return out;
    const std::vector<qr::BattleLogRowV1> rows = battleRowsUnder(page, dir);
    if (rows.empty()) {
        out.push_back({ { fitCell(std::string("  ")
                                      + qi18n::tr("No battle log for the "
                                                  "replays in here."),
                                  width),
                          qui::Tone::Dim } });
        return out;
    }
    const qr::BattleStatsSummary summary = qr::summarizeBattleLog(rows);

    if (battleSingleSet(rows) && !summary.sets_.empty()) {
        const qr::BattleSetSummary& set = summary.sets_.front();
        out.push_back({ { fitCell("  " + battleSetLine(set), width),
                          qui::Tone::Accent } });
        const std::vector<qr::BattleLogRowV1> games = qr::battleSetGames(
            rows, set.setUuidHigh, set.setUuidLow);
        const size_t room = maxLines > 1u ? maxLines - 1u : 0u;
        const size_t shown = games.size() <= room
            ? games.size() : (room > 0u ? room - 1u : 0u);
        for (size_t i = 0u; i < shown; ++i) {
            const qr::BattleLogRowV1& game = games[i];
            out.push_back(
                { { "  " + fitCell(std::string(qi18n::tr("G"))
                                       + std::to_string(i + 1u), 5u),
                    qui::Tone::Dim },
                  { fitCell(battleGameMatchup(game), 26u), qui::Tone::Default },
                  { fitCell(battleGameResultLine(game), 12u),
                    battleVerdictTone(qr::battleGameVerdict(game)) },
                  { qr::battleDurationText(
                        qr::battleLogSecondsFromFrames(game.frames)),
                    qui::Tone::Dim } });
        }
        if (games.size() > shown) {
            out.push_back({ { "  ... " + std::to_string(games.size() - shown)
                                  + " " + qi18n::tr("more games"),
                              qui::Tone::Dim } });
        }
        return out;
    }

    std::string totals = "  ";
    if (summary.countedGames != 0u) {
        totals += std::to_string(summary.countedGames) + " "
                + qi18n::tr("games") + "  " + battleRecordText(summary.games)
                + "  |  " + std::to_string(summary.countedSets) + " "
                + qi18n::tr("sets") + " " + qr::battleCountText(summary.sets);
    } else {
        totals += std::to_string(summary.rows) + " " + qi18n::tr("games");
    }
    totals += "  |  " + qr::battleDurationText(summary.totalSeconds);
    out.push_back({ { fitCell(totals, width), qui::Tone::Accent } });
    if (summary.countedGames == 0u && out.size() < maxLines) {
        out.push_back({ { fitCell(std::string("  ")
                                      + qi18n::tr("Spectated and offline "
                                                  "games are play time, not "
                                                  "results."),
                                  width),
                          qui::Tone::Dim } });
    }
    if (!summary.opponents.empty() && out.size() < maxLines) {
        std::string line = "  " + std::string(qi18n::tr("Opponents")) + "  ";
        for (size_t i = 0u; i < summary.opponents.size() && i < 4u; ++i) {
            if (i != 0u) line += ",  ";
            line += battleNameText(summary.opponents[i].name) + " "
                  + qr::battleCountText(summary.opponents[i].games);
        }
        if (summary.opponents.size() > 4u) {
            line += "  +" + std::to_string(summary.opponents.size() - 4u);
        }
        out.push_back({ { fitCell(line, width), qui::Tone::Default } });
    }
    if (!summary.characters.empty() && out.size() < maxLines) {
        std::string line = "  " + std::string(qi18n::tr("My characters"))
                         + "  ";
        for (size_t i = 0u; i < summary.characters.size() && i < 4u; ++i) {
            if (i != 0u) line += ",  ";
            line += battleCharacterName(summary.characters[i].characterId)
                  + " " + qr::battleCountText(summary.characters[i].results);
        }
        out.push_back({ { fitCell(line, width), qui::Tone::Default } });
    }
    if (summary.mostPlayedMatchup != 0xFFFFu && out.size() < maxLines) {
        out.push_back(
            { { fitCell("  " + std::string(qi18n::tr("Most played")) + "  "
                            + battleCharacterName(static_cast<uint8_t>(
                                  summary.mostPlayedMatchup >> 8))
                            + " vs "
                            + battleCharacterName(static_cast<uint8_t>(
                                  summary.mostPlayedMatchup & 0xFFu)),
                        width),
                qui::Tone::Default } });
    }
    if (!summary.sets_.empty() && out.size() < maxLines) {
        out.push_back({ { fitCell("  " + std::string(qi18n::tr("Latest set"))
                                      + "  "
                                      + battleSetLine(summary.sets_.front()),
                                  width),
                          qui::Tone::Default } });
    }
    return out;
}

std::wstring battleFolderPathFor(const Page& page, const std::wstring& name) {
    const std::wstring& scope = page.model.filter().directoryScope;
    if (name == L"..") {
        const size_t cut = scope.rfind(L'\\');
        return cut == std::wstring::npos ? std::wstring()
                                         : scope.substr(0, cut);
    }
    return scope.empty() ? name : scope + L"\\" + name;
}

std::vector<std::string> battleRowsAsLines(const std::vector<qui::Row>& rows) {
    std::vector<std::string> lines;
    lines.reserve(rows.size());
    for (const qui::Row& row : rows) {
        std::string line;
        for (const qui::Span& span : row) line += span.text;
        while (!line.empty() && line.front() == ' ') line.erase(0, 1);
        lines.push_back(line);
    }
    return lines;
}

void refreshFolders(Page& page) {
    const std::wstring& scope = page.model.filter().directoryScope;
    std::set<std::wstring> names;
    std::shared_ptr<const qr::ReplaySnapshot> snapshot =
        page.scanner.snapshot();
    if (snapshot) {
        for (const qr::ReplayEntry& entry : snapshot->entries) {
            const std::wstring& dir = entry.relativeDir;
            if (dir.size() <= scope.size()) continue;
            if (!scope.empty()) {
                if (dir.compare(0, scope.size(), scope) != 0) continue;
                if (dir[scope.size()] != L'\\') continue;
            }
            const size_t start = scope.empty() ? 0u : scope.size() + 1u;
            const size_t end = dir.find(L'\\', start);
            const std::wstring child = end == std::wstring::npos
                ? dir.substr(start)
                : dir.substr(start, end - start);
            if (!child.empty()) names.insert(child);
        }
    }
    page.folders.clear();
    if (!scope.empty()) page.folders.push_back(L"..");
    for (const std::wstring& name : names) page.folders.push_back(name);
    if (page.browseIndex < 0) page.browseIndex = 0;
}

std::string browseScopeText(const Page& page) {
    const std::wstring& scope = page.model.filter().directoryScope;
    if (scope.empty()) return qplat::narrow(page.root);
    return qplat::narrow(page.root) + "\\" + qplat::narrow(scope);
}

void sortMenu(qui::Console& con, Page& page) {
    const std::array<qr::SortKey, 8> keys = {
        qr::SortKey::GameOrder, qr::SortKey::Newest, qr::SortKey::Oldest,
        qr::SortKey::TitleAsc, qr::SortKey::DurationDesc,
        qr::SortKey::MatchCountDesc, qr::SortKey::ResultAsc,
        qr::SortKey::StatusAsc
    };
    std::vector<std::string> labels;
    int selected = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        labels.emplace_back(qr::sortKeyText(keys[i]));
        if (keys[i] == page.model.sort()) selected = static_cast<int>(i);
    }
    const int choice = con.menu("Sort replays by", labels, "Back", selected);
    if (choice >= 0 && static_cast<size_t>(choice) < keys.size())
        page.model.setSort(keys[static_cast<size_t>(choice)]);
}

void runSimulationCheck(qui::Console& con, Page& page, Settings& s) {
    const qr::ActionState state =
        page.model.actionState(qr::BrowserAction::VerifySimulation);
    if (!state.enabled) {
        setNotice(page, std::string("Play and check: ")
                      + qr::actionDisableReasonPlainText(state.reason),
                  qui::Tone::Warn);
        return;
    }
    qr::ActionRequest request;
    request.action = qr::BrowserAction::VerifySimulation;
    const qr::ActionOutcome outcome = runAction(con, page, request);
    reportOutcome(page, outcome);
    if (!outcome.ok || !outcome.watch.valid) return;

    qreplay::ipc::v3::PlaybackResultPayload result{};
    const bool verified = watchAndReturn(
        con, page, s, outcome.watch, &result,
        true);
    using Reason = qreplay::ipc::v3::PlaybackResultReason;
    if (verified
        && result.reason == Reason::VerifiedCompleted
        && result.hasFileDigest
        && result.hasCanonicalGameDigest
        && page.annotations.recordSimulationVerification(
            outcome.watch.replayUuidLow,
            outcome.watch.replayUuidHigh,
            result.runtimeContract.contentManifestRoot,
            result.runtimeContract.runtimeSimulationId,
            static_cast<int64_t>(std::time(nullptr)))) {
        const bool saved = page.annotations.saveIfDirty();
        page.model.refreshView();
        page.scanner.requestRefresh();
        setNotice(page,
                  saved
                      ? "Simulation verified against the installed runtime "
                        "and recorded in the replay catalog."
                      : "Simulation verified, but its local catalog badge "
                        "could not be saved.",
                  saved ? qui::Tone::Good : qui::Tone::Warn);
    } else if (verified) {
        setNotice(page,
                  std::string("Simulation result: ")
                      + playbackResultText(result.reason)
                      + "; no completed-game badge was recorded.",
                  qui::Tone::Warn);
    }
}

void applyReplayBucket(Page& page) {
    qr::ReplayFilter& filter = page.model.filter();
    const ReplayBucket bucket = page.bucket;
    const bool all = bucket == ReplayBucket::All;
    filter.sourceNetplay = all || bucket == ReplayBucket::Network;
    filter.sourceOffline = all || bucket == ReplayBucket::Offline;
    filter.sourceSpectator = all || bucket == ReplayBucket::Spectate;
    filter.sourceRecovered = all;
    page.model.refreshView();
}

void replaysMenu(qui::Console& con, Settings& s) {
    Page page;
    page.root = replayRootOf(s);
    if (!qplat::ensureDirW(page.root)) {
        con.message("Replays unavailable",
                    "The replay folder " + qplat::narrow(page.root)
                        + " could not be created. Choose another replay "
                          "folder in Settings -> Replays and storage.");
        return;
    }
    page.annotationsPath = page.root + L"\\annotations.q99a";
    if (!page.annotations.load(page.annotationsPath)) {
        QLOG("[replays] annotation store at '%s' was not loaded; favorites, "
             "titles, tags and notes start empty for this session",
             qplat::narrow(page.annotationsPath).c_str());
    }
    page.model.setAnnotations(&page.annotations);
    struct BrowserIdentities {
        qr::RuntimeIdentity v1{};
        qr::ReplayRuntimeIdentityV2 v2{};
    };
    gameFilesCheck(s);
    auto identityFuture = std::async(std::launch::async, [&s]() {
        BrowserIdentities out{};
        out.v1 = currentRuntimeIdentity(s);
        out.v2 = currentRuntimeIdentityV2(s);
        return out;
    });
    bool identitiesInstalled = false;
    loadBattleIndex(page, s);
    refreshFolders(page);
    page.model.setReplayRoot(page.root);
    page.model.filter().showIncomplete = s.showIncompleteReplays;
    page.model.refreshView();
    startScan(page, s);

    const uint64_t freeMb = freeMegabytesAt(page.root);
    int scrollTop = 0;

    for (;;) {
        if (page.refreshAfterAction && !page.scanner.scanning()) {
            page.scanner.requestRefresh();
            page.refreshAfterAction = false;
        }
        if (!identitiesInstalled && identityFuture.valid()
            && identityFuture.wait_for(std::chrono::seconds(0))
                == std::future_status::ready) {
            const BrowserIdentities identities = identityFuture.get();
            page.model.setRuntimeIdentity(identities.v1);
            page.model.setRuntimeIdentityV2(identities.v2);
            identitiesInstalled = true;
        }
        page.model.setScanning(page.scanner.scanning());
        const uint64_t generation = page.scanner.generation();
        if (!page.haveSnapshot || generation != page.shownGeneration) {
            std::shared_ptr<const qr::ReplaySnapshot> snapshot =
                page.scanner.snapshot();
            if (snapshot) {
                const bool selectedFolder = page.browseIndex >= 0
                    && static_cast<size_t>(page.browseIndex) < page.folders.size();
                const std::wstring folder = selectedFolder
                    ? page.folders[static_cast<size_t>(page.browseIndex)]
                    : std::wstring();
                const bool selectedFile = !selectedFolder && !page.model.empty();
                page.model.setSnapshot(snapshot);
                page.shownGeneration = generation;
                page.haveSnapshot = true;
                refreshFolders(page);
                if (selectedFile && !page.model.empty()) {
                    page.browseIndex = static_cast<int>(
                        page.folders.size() + page.model.selectedIndex());
                } else if (selectedFolder) {
                    const auto found = std::find(page.folders.begin(),
                                                 page.folders.end(), folder);
                    if (found != page.folders.end())
                        page.browseIndex = static_cast<int>(found - page.folders.begin());
                }
                countUnloggedGames(page);
            }
        }

        page.model.setIdentityPending(!identitiesInstalled);

        const int columns = con.columns();
        const size_t pageWidth =
            static_cast<size_t>(columns > 8 ? columns - 4 : 40);
        const size_t splitCost = kDetailPaneColumns + kPaneGutterColumns;
        const size_t splitLeftWidth =
            pageWidth > splitCost ? pageWidth - splitCost : 0u;
        const bool wide = splitLeftWidth != 0u
            && listColumnsFor(splitLeftWidth).match >= kMatchColumnCap;
        const size_t rightWidth = wide ? kDetailPaneColumns : 0u;
        const size_t leftWidth = wide ? splitLeftWidth : pageWidth;
        const ListColumns listColumns = listColumnsFor(leftWidth);

        const qr::BrowserDetails& details = page.model.details();
        if (details.valid && details.stableId != page.loggedStableId) {
            page.loggedStableId = details.stableId;
            QLOG("[replays] selected '%s' bootstrap='%s' inputDomain='%s'",
                 details.storageText.c_str(), details.bootstrapText.c_str(),
                 details.inputDomainText.c_str());
        }

        std::vector<qui::Row> rows;
        rows.push_back({ { " Replays", qui::Tone::Title } });
        {
            const bool narrowed = page.model.filter().active()
                || !page.model.query().empty();
            std::string header = " " + std::to_string(page.model.rowCount());
            if (narrowed) {
                header += " of " + std::to_string(page.model.totalFileCount())
                    + " replays shown";
            } else {
                header += page.model.rowCount() == 1u ? " replay" : " replays";
            }
            header += "  |  sorted by ";
            header += qr::sortKeyText(page.model.sort());
            header += "  |  " + browseScopeText(page);
            rows.push_back({ { header, qui::Tone::Dim } });
        }
        {
            const std::string headline = battleHeadlineFor(page);
            if (!headline.empty())
                rows.push_back({ { headline, qui::Tone::Accent } });
        }
        {
            std::string state = std::string(" Audio ")
                + (s.replayDefaultAudio ? "on" : "off");
            if (page.scanner.scanning()) {
                std::shared_ptr<const qr::ReplaySnapshot> live =
                    page.scanner.snapshot();
                const size_t loaded = live ? live->entries.size() : 0u;
                const size_t total = live ? live->filesSeen : 0u;
                state += total > loaded
                    ? "   loading newest " + std::to_string(loaded) + " of "
                          + std::to_string(total) + "..."
                    : std::string("   scanning...");
            }
            rows.push_back({ { state, qui::Tone::Dim } });
        }
        if (freeMb != 0 && s.replayStorageWarnMb > 0
            && freeMb < static_cast<uint64_t>(s.replayStorageWarnMb)) {
            rows.push_back({ { " Low disk space: only "
                                   + std::to_string(freeMb)
                                   + " MiB free where replays are stored.",
                               qui::Tone::Warn } });
        }
        rows.push_back({});

        const int listRows = rightWidth != 0 ? 14 : 12;
        const size_t folderCount = page.folders.size();
        const size_t fileCount = page.model.rowCount();
        const size_t browseTotal = folderCount + fileCount;
        if (page.browseIndex < 0) page.browseIndex = 0;
        if (browseTotal != 0u
            && page.browseIndex > static_cast<int>(browseTotal) - 1) {
            page.browseIndex = static_cast<int>(browseTotal) - 1;
        }
        const bool onFolder =
            static_cast<size_t>(page.browseIndex) < folderCount;
        if (!onFolder && fileCount != 0u) {
            page.model.select(
                static_cast<size_t>(page.browseIndex) - folderCount);
        }
        const size_t count = page.model.rowCount();
        const int selected = page.browseIndex;
        if (selected < scrollTop) scrollTop = selected;
        if (selected >= scrollTop + listRows)
            scrollTop = selected - listRows + 1;
        if (scrollTop < 0) scrollTop = 0;
        if (browseTotal == 0u) scrollTop = 0;
        page.model.setScrollTop(static_cast<size_t>(scrollTop));

        std::vector<std::string> pageLines;
        if (page.detailsPage) {
            pageLines = page.developerPage
                ? buildDeveloperLines(details, s, pageWidth)
                : buildDetailLines(details, page.root, pageWidth);
        }

        if (page.detailsPage) {
            rows.push_back({ { page.developerPage
                                   ? " Replays  >  Details  >  Developer"
                                   : " Replays  >  Details",
                               qui::Tone::Accent } });
            rows.push_back({ { " " + std::string(pageWidth, '-'),
                               qui::Tone::Dim } });
            for (size_t i = page.detailsScroll;
                 i < pageLines.size()
                     && i < page.detailsScroll + kDetailRowsPerPage; ++i) {
                rows.push_back({ { " " + pageLines[i], qui::Tone::Default } });
            }
            if (page.detailsScroll + kDetailRowsPerPage < pageLines.size())
                rows.push_back({ { " ... PgDn for more", qui::Tone::Dim } });
        } else if (count == 0 && folderCount == 0u) {
            rows.push_back({ { " " + page.model.emptyStateText(),
                               qui::Tone::Dim } });
        } else {
            std::vector<std::string> paneLines;
            if (rightWidth != 0) {
                if (onFolder) {
                    paneLines = battleRowsAsLines(battleFolderRows(
                        page,
                        battleFolderPathFor(
                            page, page.folders[static_cast<size_t>(
                                      page.browseIndex)]),
                        rightWidth, static_cast<size_t>(listRows)));
                } else {
                    paneLines = battleRowsAsLines(battleGameRows(
                        page, page.model.entryForRow(page.model.selectedIndex()),
                        rightWidth));
                    if (!paneLines.empty()) paneLines.push_back(std::string());
                    for (std::string& line : buildDetailLines(
                             details, page.root, rightWidth, true)) {
                        paneLines.push_back(std::move(line));
                    }
                }
            }
            rows.push_back({ { "   ", qui::Tone::Dim },
                             { listHeaderFor(listColumns), qui::Tone::Dim } });
            for (int i = 0; i < listRows; ++i) {
                const size_t browse = static_cast<size_t>(scrollTop + i);
                qui::Row row;
                if (browse < folderCount) {
                    const bool isSelected =
                        browse == static_cast<size_t>(page.browseIndex);
                    const std::wstring& name = page.folders[browse];
                    const std::string label = name == L".."
                        ? std::string("..  (up one folder)")
                        : qplat::narrow(name) + "\\";
                    row.push_back({ isSelected ? " > " : "   ",
                                    qui::Tone::Dim });
                    row.push_back({ fitCell(label, listColumns.total),
                                    isSelected ? qui::Tone::Selected
                                               : qui::Tone::Accent });
                } else if (browse - folderCount < count) {
                    const size_t index = browse - folderCount;
                    const qr::BrowserRow& listRow = page.model.row(index);
                    const bool isSelected =
                        browse == static_cast<size_t>(page.browseIndex);
                    row.push_back({ isSelected ? " > " : "   ",
                                    qui::Tone::Dim });
                    row.push_back({ listLineFor(listRow, listColumns),
                                    isSelected ? qui::Tone::Selected
                                               : qui::Tone::Default });
                    std::string status = listRow.statusShort;
                    qui::Tone statusTone = toneFor(listRow.statusTone);
                    if (listRow.statusTone == qr::BrowserStatusTone::Ready) {
                        const qr::BattleLogRowV1* logged = battleRowFor(
                            page, page.model.entryForRow(index));
                        const qr::BattleGameVerdict verdict = logged != nullptr
                            ? qr::battleGameVerdict(*logged)
                            : qr::BattleGameVerdict::NoLocalSide;
                        if (verdict != qr::BattleGameVerdict::NoLocalSide) {
                            status = qi18n::tr(
                                qr::battleGameVerdictText(verdict));
                            statusTone = battleVerdictTone(verdict);
                        }
                    }
                    row.push_back({ fitCell(status, listColumns.status),
                                    isSelected ? qui::Tone::Selected
                                               : statusTone });
                } else {
                    row.push_back({ "   ", qui::Tone::Dim });
                    row.push_back({ fitCell("", listColumns.total),
                                    qui::Tone::Default });
                }
                if (rightWidth != 0) {
                    row.push_back({ " | ", qui::Tone::Dim });
                    const size_t line = static_cast<size_t>(i);
                    row.push_back({ line < paneLines.size()
                                        ? fitCell(paneLines[line], rightWidth)
                                        : std::string(),
                                    qui::Tone::Default });
                }
                rows.push_back(std::move(row));
            }
            if (rightWidth == 0) {
                rows.push_back({ { " " + std::string(pageWidth, '-'),
                                   qui::Tone::Dim } });
                if (onFolder) {
                    for (qui::Row& line : battleFolderRows(
                             page,
                             battleFolderPathFor(
                                 page, page.folders[static_cast<size_t>(
                                           page.browseIndex)]),
                             pageWidth, 7u)) {
                        rows.push_back(std::move(line));
                    }
                } else {
                    std::vector<qui::Row> summary =
                        buildSummaryRows(details, pageWidth);
                    std::vector<qui::Row> battle = battleGameRows(
                        page, page.model.entryForRow(page.model.selectedIndex()),
                        pageWidth);
                    for (size_t i = 0u; i < summary.size(); ++i) {
                        rows.push_back(std::move(summary[i]));
                        if (i == 0u) {
                            for (qui::Row& line : battle)
                                rows.push_back(std::move(line));
                        }
                    }
                }
            } else if (paneLines.size() > static_cast<size_t>(listRows)) {
                qui::Row more;
                more.push_back({ "   ", qui::Tone::Dim });
                more.push_back({ fitCell("", listColumns.total),
                                 qui::Tone::Default });
                more.push_back({ " | ", qui::Tone::Dim });
                more.push_back({ fitCell("... Tab shows the rest", rightWidth),
                                 qui::Tone::Dim });
                rows.push_back(std::move(more));
            }
        }

        rows.push_back({});
        if (!page.notice.empty())
            rows.push_back({ { " " + page.notice, page.noticeTone } });
        if (page.developerPage) {
            rows.push_back({ { " PgUp/PgDn scroll    V check this file    "
                               "Y play and check    D back to details",
                               qui::Tone::Dim } });
        } else if (page.detailsPage) {
            rows.push_back({ { " PgUp/PgDn scroll    D developer details    "
                               "Esc back",
                               qui::Tone::Dim } });
        } else {
            rows.push_back({ { onFolder
                                   ? " Enter open folder    P play folder    Esc back"
                                   : " Enter watch    P play folder    F favourite    "
                                     "D details    Del delete",
                               qui::Tone::Dim } });
            std::string hint = " / search    B "
                + std::string(replayBucketName(page.bucket))
                + "    Tab sort    R rescan";
            if (page.model.actionState(qr::BrowserAction::UndoTrash).enabled)
                hint += "    U undo delete";
            if (page.model.actionState(qr::BrowserAction::ClearFilters)
                    .enabled) {
                hint += "    C clear ";
                hint += page.model.query().empty()
                    ? std::string("filters")
                    : "search \"" + page.model.query() + "\"";
            }
            if (page.battleLog.unloggedGames != 0u) {
                hint += "    L log "
                      + std::to_string(page.battleLog.unloggedGames)
                      + " unlogged";
            }
            hint += "    Esc back";
            rows.push_back({ { hint, qui::Tone::Dim } });
        }
        con.draw(rows);

        const int key = con.pollKey(400);
        switch (key) {
            case 0:
                break;
            case VK_ESCAPE:
                if (page.developerPage) {
                    page.developerPage = false;
                    page.detailsScroll = 0;
                    break;
                }
                if (page.detailsPage) {
                    page.detailsPage = false;
                    page.detailsScroll = 0;
                    break;
                }
                page.scanner.stop();
                page.annotations.saveIfDirty();
                return;
            case VK_UP:
                if (page.browseIndex > 0) --page.browseIndex;
                page.detailsScroll = 0;
                break;
            case VK_DOWN:
                ++page.browseIndex;
                page.detailsScroll = 0;
                break;
            case VK_PRIOR:
                if (page.detailsPage) {
                    page.detailsScroll =
                        page.detailsScroll >= kDetailRowsPerPage
                            ? page.detailsScroll - kDetailRowsPerPage
                            : 0;
                } else {
                    page.browseIndex -= listRows;
                    if (page.browseIndex < 0) page.browseIndex = 0;
                }
                break;
            case VK_NEXT:
                if (page.detailsPage) {
                    if (page.detailsScroll + kDetailRowsPerPage
                            < pageLines.size()) {
                        page.detailsScroll += kDetailRowsPerPage;
                    }
                } else {
                    page.browseIndex += listRows;
                }
                break;
            case VK_HOME:
                page.browseIndex = 0;
                page.detailsScroll = 0;
                break;
            case VK_END:
                page.browseIndex = browseTotal != 0u
                    ? static_cast<int>(browseTotal) - 1 : 0;
                page.detailsScroll = 0;
                break;
            case VK_TAB:
                sortMenu(con, page);
                break;
            case VK_LEFT:
            case VK_RIGHT: {
                const bool forward = key == VK_RIGHT;
                if (page.detailsPage) {
                    page.detailsScroll += forward ? 10 : -10;
                    if (page.detailsScroll < 0) page.detailsScroll = 0;
                    break;
                }
                page.browseIndex += forward ? listRows : -listRows;
                if (page.browseIndex < 0) page.browseIndex = 0;
                break;
            }
            case VK_RETURN: {
                if (onFolder) {
                    std::wstring scope = page.model.filter().directoryScope;
                    const std::wstring& name =
                        page.folders[static_cast<size_t>(page.browseIndex)];
                    if (name == L"..") {
                        const size_t cut = scope.find_last_of(L'\\');
                        scope = cut == std::wstring::npos
                            ? std::wstring() : scope.substr(0, cut);
                    } else {
                        scope = scope.empty() ? name : scope + L"\\" + name;
                    }
                    page.model.filter().directoryScope = scope;
                    page.model.refreshView();
                    refreshFolders(page);
                    page.browseIndex = 0;
                    scrollTop = 0;
                    page.detailsScroll = 0;
                    break;
                }
                qr::ActionRequest request;
                request.action = qr::BrowserAction::WatchSet;
                const qr::ActionOutcome outcome = runAction(con, page, request);
                reportOutcome(page, outcome);
                if (outcome.ok && outcome.watch.valid)
                    watchAndReturn(con, page, s, outcome.watch);
                break;
            }
            case 'D':
                if (onFolder && !page.detailsPage) break;
                if (page.detailsPage) {
                    page.developerPage = !page.developerPage;
                } else {
                    page.detailsPage = true;
                }
                page.detailsScroll = 0;
                break;
            case 'P': {
                if (page.detailsPage || page.developerPage) break;
                if (!identitiesInstalled) {
                    setNotice(page, "Checking the installed game files...", qui::Tone::Dim);
                    break;
                }
                std::wstring directory = page.model.filter().directoryScope;
                if (onFolder) {
                    const auto& name = page.folders[static_cast<size_t>(page.browseIndex)];
                    if (name == L"..") {
                        const size_t cut = directory.find_last_of(L'\\');
                        directory = cut == std::wstring::npos
                            ? std::wstring() : directory.substr(0, cut);
                    } else {
                        directory = directory.empty() ? name : directory + L"\\" + name;
                    }
                }
                watchFolderAndReturn(con, page, s, directory);
                break;
            }
            case 'V': {
                if (!page.developerPage) break;
                qr::ActionRequest request;
                request.action = qr::BrowserAction::VerifyFile;
                reportOutcome(page, runAction(con, page, request));
                break;
            }
            case 'Y':
                if (page.developerPage) runSimulationCheck(con, page, s);
                break;
            case 'F': {
                if (onFolder) break;
                qr::ActionRequest request;
                request.action = qr::BrowserAction::ToggleFavorite;
                reportOutcome(page, runAction(con, page, request));
                break;
            }
            case 'C': {
                if (!page.model.actionState(qr::BrowserAction::ClearFilters)
                         .enabled) {
                    break;
                }
                qr::ActionRequest request;
                request.action = qr::BrowserAction::ClearFilters;
                reportOutcome(page, runAction(con, page, request));
                break;
            }
            case 'R':
                if (page.scanner.scanning()) {
                    setNotice(page, "A replay scan is already in progress.", qui::Tone::Dim);
                    break;
                }
                page.scanner.requestRefresh();
                loadBattleIndex(page, s);
                setNotice(page, "Rescanning the replay folder...",
                          qui::Tone::Dim);
                break;
            case VK_DELETE: {
                if (onFolder) {
                    setNotice(page,
                              "That is a folder. Open it and delete the "
                              "replays inside, or remove it yourself.",
                              qui::Tone::Dim);
                    break;
                }
                const bool inTrash = page.model.rowCount() != 0u
                    && page.model.row(page.model.selectedIndex()).inTrash;
                const qr::BrowserAction action = inTrash
                    ? qr::BrowserAction::PermanentPurge
                    : qr::BrowserAction::MoveToTrash;
                const qr::ActionState state = page.model.actionState(action);
                if (!state.enabled) {
                    setNotice(page,
                              std::string(actionLabel(action)) + ": "
                                  + qr::actionDisableReasonPlainText(
                                        state.reason),
                              qui::Tone::Warn);
                    break;
                }
                if (inTrash) {
                    const int confirm = con.menu(
                        "Delete this replay permanently?",
                        {"Delete permanently - this cannot be undone"},
                        "Keep it");
                    if (confirm != 0) break;
                }
                qr::ActionRequest request;
                request.action = action;
                reportOutcome(page, runAction(con, page, request));
                break;
            }
            case 'U': {
                if (!page.model.actionState(qr::BrowserAction::UndoTrash)
                         .enabled) {
                    break;
                }
                qr::ActionRequest request;
                request.action = qr::BrowserAction::UndoTrash;
                reportOutcome(page, runAction(con, page, request));
                break;
            }
            case 'L':
                if (page.detailsPage || page.developerPage) break;
                if (page.battleLog.unloggedGames == 0u) break;
                if (offerBattleLogRebuild(con, page, s))
                    loadBattleIndex(page, s);
                break;
            case 'B': {
                if (page.detailsPage || page.developerPage) break;
                page.bucket = nextReplayBucket(page.bucket);
                applyReplayBucket(page);
                page.notice = std::string("showing ")
                    + replayBucketName(page.bucket);
                page.noticeTone = qui::Tone::Dim;
                break;
            }
            case VK_OEM_2: {
                std::string query = page.model.query();
                std::string entered;
                if (con.promptLine("Search replays", entered, query))
                    page.model.setQuery(entered);
                break;
            }
            default:
                break;
        }
    }
}

}

int runReplayVerb(int argc, char** argv) {
    namespace qr = qclient::replay;
    const std::string sub = (argc >= 3) ? argv[2] : "";
    const auto usageReplay = []() {
        std::printf(
            "qoh99caster replay list [--root <directory>]\n"
            "qoh99caster replay inspect <file>\n"
            "qoh99caster replay verify <file> [--simulate] "
            "[--game <qoh99.exe>] [--dll <qoh99_netplay.dll>]\n"
            "qoh99caster replay play <file> [--match <ordinal>] "
            "[--game <qoh99.exe>] "
            "[--dll <qoh99_netplay.dll>]\n"
            "  playback speed, pause and frame-step are controls inside the "
            "running game, not launch options.\n");
        return 2;
    };
    if (sub.empty() || sub == "--help" || sub == "help") return usageReplay();

    std::wstring rootOverride;
    std::wstring target;
    std::string gameOverride;
    std::string dllOverride;
    uint32_t matchOrdinal = 0;
    bool simulate = false;
    for (int i = 3; i < argc; ++i) {
        const std::string opt = argv[i];
        if (opt == "--root" && i + 1 < argc) rootOverride = qplat::widen(argv[++i]);
        else if (opt == "--game" && i + 1 < argc)
            gameOverride = argv[++i];
        else if (opt == "--dll" && i + 1 < argc)
            dllOverride = argv[++i];
        else if (opt == "--match" && i + 1 < argc)
            matchOrdinal = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (opt == "--speed" && i + 1 < argc) {
            ++i;
            std::printf("note: --speed is no longer a launch option. "
                        "Playback starts at 1x and speed, pause and "
                        "frame-step are controls inside the running game.\n");
        }
        else if (opt == "--simulate") simulate = true;
        else if (!opt.empty() && opt[0] != '-' && target.empty())
            target = qplat::widen(opt);
    }

    if (sub == "list") {
        qr::ReplayScanner::Config cfg;
        cfg.roots.push_back(rootOverride.empty() ? qplat::replayRootW()
                                                 : rootOverride);
        cfg.useCache = false;
        std::atomic<bool> cancelled{ false };
        const qr::ReplaySnapshot snap =
            qr::ReplayScanner::scanOnce(cfg, nullptr, &cancelled, 1);
        std::printf("%zu replay(s) in %s\n", snap.entries.size(),
                    qplat::narrow(cfg.roots.front()).c_str());
        for (const qr::ReplayEntry& e : snap.entries) {
            std::printf("  %-44s %6u frames  %2u match(es)  %s\n",
                        qplat::narrow(e.fileName).c_str(),
                        static_cast<unsigned>(e.totalFrames),
                        static_cast<unsigned>(e.matchCount),
                        e.parsed
                            ? (e.hasFooter ? "complete" : "incomplete")
                            : qreplay::parseStatusText(e.parseStatus));
        }
        return 0;
    }

    if (target.empty()) return usageReplay();
    if (!qr::isSafeReplayPath(target)) {
        std::printf("Refused: %s is not inside a configured replay root or "
                    "contains an unsafe path element.\n",
                    qplat::narrow(target).c_str());
        return 1;
    }
    if (!hasFinalReplayExtension(target)) {
        std::printf("Refused: only finalized .q99r replay files are supported.\n");
        return 1;
    }

    if (sub == "verify" && simulate) {
        Settings s;
        s.load();
        if (!gameOverride.empty()) s.gamePath = gameOverride;
        if (!dllOverride.empty()) s.dllPath = dllOverride;
        qreplay::ipc::v3::PlaybackResultPayload playbackResult{};
        bool received = false;
        const int playbackRc = runReplayPlayback(
            s, target, 0u,
            nullptr, true, &playbackResult, &received);
        if (!received) {
            std::printf("simulation  : no authenticated runtime result\n");
            return 1;
        }
        std::printf("simulation  : %s\n",
                    replays_ui::playbackResultText(playbackResult.reason));
        std::printf("endpoint    : %u  records consumed: %u\n",
                    static_cast<unsigned>(playbackResult.endpointKind),
                    static_cast<unsigned>(
                        playbackResult.nextRecordConsumed));
        std::printf("state       : rolling %016llX  post-tail %016llX\n",
                    static_cast<unsigned long long>(
                        playbackResult.finalRollingDigest),
                    static_cast<unsigned long long>(
                        playbackResult.postTailStateHash));
        std::printf("input guard : %llu live device call(s)\n",
                    static_cast<unsigned long long>(
                        playbackResult.deviceCallCount));
        return playbackRc == 0
                && replays_ui::verifiedPlaybackResult(playbackResult)
            ? 0 : 1;
    }

    if (sub == "inspect" || sub == "verify") {
        bool v2 = false;
        {
            qr::FileByteSource source;
            uint8_t prefix[6] = {};
            if (source.open(target)
                && source.read(0u, prefix, sizeof(prefix))) {
                const uint32_t magic =
                    static_cast<uint32_t>(prefix[0])
                    | (static_cast<uint32_t>(prefix[1]) << 8u)
                    | (static_cast<uint32_t>(prefix[2]) << 16u)
                    | (static_cast<uint32_t>(prefix[3]) << 24u);
                const uint16_t major =
                    static_cast<uint16_t>(prefix[4])
                    | static_cast<uint16_t>(
                        static_cast<uint16_t>(prefix[5]) << 8u);
                v2 = magic == qreplay::kReplayMagic
                    && major == qreplay::kContainerVersionMajorV2;
            }
        }
        if (v2) {
            qr::ParseOptionsV2 options{};
            options.retainTimeline = false;
            options.allowUnsealedPrefix = false;
            options.verifyFileDigest = sub == "verify";
            qr::ParsedReplayV2 parsed{};
            const qreplay::ParseResult result =
                qr::ReplayParserV2::parseFile(target, options, parsed);
            const char* classification = "invalid";
            switch (parsed.classification) {
                case qr::ReplayClassificationV2::Invalid:
                    classification = "invalid";
                    break;
                case qr::ReplayClassificationV2::MetadataOnly:
                    classification = "metadata only";
                    break;
                case qr::ReplayClassificationV2::UnsealedValidatedPrefix:
                    classification = "incomplete (not playable)";
                    break;
                case qr::ReplayClassificationV2::Sealed:
                    classification = "sealed game";
                    break;
                case qr::ReplayClassificationV2::
                        FooterCompleteRenameFailure:
                    classification = "sealed game (legacy publication state)";
                    break;
            }
            std::printf("file        : %s\n", qplat::narrow(target).c_str());
            std::printf("format      : Q99R v2 (one game per file)\n");
            std::printf("status      : %s%s%s\n",
                        qreplay::parseStatusText(result.status),
                        result.detail.empty() ? "" : " - ",
                        result.detail.c_str());
            if (!result.ok()) {
                std::printf("stopped at  : byte %llu (chunk %u, frame %u)\n",
                            static_cast<unsigned long long>(result.offset),
                            static_cast<unsigned>(result.chunkSequence),
                            static_cast<unsigned>(
                                result.lastValidTimelineFrame));
            }
            std::printf("class       : %s\n", classification);
            std::printf("chunks      : %u  frames: %u  events: %u\n",
                        static_cast<unsigned>(parsed.chunkCount),
                        static_cast<unsigned>(parsed.frameCount),
                        static_cast<unsigned>(parsed.eventCount));
            std::printf("footer      : %s\n",
                        parsed.hasFooter ? "present" : "ABSENT (incomplete)");
            if (!parsed.palettes.empty()) {
                std::printf("palettes    : ");
                for (size_t i = 0u; i < parsed.palettes.size(); ++i) {
                    std::printf("%sP%d colour %u",
                                i == 0u ? "" : ", ",
                                static_cast<int>(parsed.palettes[i].side) + 1,
                                static_cast<unsigned>(
                                    parsed.palettes[i].variant) + 1u);
                }
                std::printf(" (custom)\n");
            }
            if (sub == "verify") {
                std::printf("file digest : %s\n",
                            parsed.fileDigestVerified
                                ? "File intact"
                                : (parsed.hasFooter ? "MISMATCH"
                                                    : "not sealed"));
            }
            if (parsed.hasStart) {
                std::printf("game uuid   : %016llX%016llX\n",
                            static_cast<unsigned long long>(
                                parsed.start.manifest.identity.gameUuidHigh),
                            static_cast<unsigned long long>(
                                parsed.start.manifest.identity.gameUuidLow));
                std::printf("stage       : %u%s\n",
                            static_cast<unsigned>(
                                parsed.start.manifest.stage.nativeStageId),
                             parsed.start.manifest.stage.verified
                                 ? " (verified)" : " (unverified)");
            }
            if (parsed.hasEnd) {
                std::printf("terminal    : %u%s\n",
                            static_cast<unsigned>(parsed.end.reason),
                            parsed.end.reason
                                    == qreplay::TerminalReason::Completed
                                ? " (completed)"
                                : " (abnormal)");
            }
            return result.ok() ? 0 : 1;
        }

        qr::ParseOptions options;
        options.mode = (sub == "verify") ? qr::ParseMode::Verify
                                         : qr::ParseMode::Metadata;
        options.requireFooter = sub == "verify";
        qr::ParsedReplay parsed;
        const qreplay::ParseResult result =
            qr::ReplayParser::parseFile(target, options, parsed);
        std::printf("file        : %s\n", qplat::narrow(target).c_str());
        std::printf("status      : %s%s%s\n",
                    qreplay::parseStatusText(result.status),
                    result.detail.empty() ? "" : " - ",
                    result.detail.c_str());
        if (!result.ok())
            std::printf("stopped at  : byte %llu (chunk %u, frame %u)\n",
                        static_cast<unsigned long long>(result.offset),
                        static_cast<unsigned>(result.chunkSequence),
                        static_cast<unsigned>(result.lastValidTimelineFrame));
        std::printf("chunks      : %u  frames: %u  matches: %zu\n",
                    static_cast<unsigned>(parsed.chunkCount),
                    static_cast<unsigned>(parsed.totalFrames),
                    parsed.matches.size());
        std::printf("footer      : %s\n",
                    parsed.hasFooter ? "present" : "ABSENT (incomplete)");
        if (parsed.fileDigestChecked)
            std::printf("file digest : %s\n",
                        parsed.fileDigestMatched ? "File intact"
                                                 : "MISMATCH");
        const qreplay::CharacterCatalog& catalog =
            qreplay::CharacterCatalog::forSeBuild();
        for (const qr::MatchSpan& m : parsed.matches) {
            std::string roster;
            for (size_t slot = 0; slot < qreplay::kFighterSlots; ++slot) {
                if (m.ownership[slot] == qreplay::SlotOwnership::Inactive)
                    continue;
                if (!roster.empty()) roster += " / ";
                roster += catalog.displayName(m.roster[slot].nativeCharacterId);
                if (m.ownership[slot] == qreplay::SlotOwnership::NativeCpu)
                    roster += " (CPU)";
            }
            std::printf("  match %-3u %6u frames  %s\n",
                        static_cast<unsigned>(m.ordinal),
                        static_cast<unsigned>(m.frameCount), roster.c_str());
        }
        return result.ok() ? 0 : 1;
    }

    if (sub == "play") {
        Settings s;
        s.load();
        if (!gameOverride.empty()) s.gamePath = gameOverride;
        if (!dllOverride.empty()) s.dllPath = dllOverride;
        return runReplayPlayback(s, target, matchOrdinal,
                                 nullptr, false);
    }
    return usageReplay();
}

int runWineFixture(int argc, char** argv) {
    bool asChild = false;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "--fixture-child") == 0) asChild = true;

    if (asChild) {
        std::printf("fixture-child ok pid=%lu\n",
                    static_cast<unsigned long>(GetCurrentProcessId()));
        std::fflush(stdout);
        Sleep(5000);
        return 0;
    }

    const qplat::CompatInfo& compat = qplat::detectCompat();
    std::printf("platform          : %s\n",
                compat.wine
                    ? (compat.proton ? "wine (proton)" : "wine")
                    : "native windows");
    if (compat.wine && !compat.wineVersion.empty())
        std::printf("wine version      : %s\n", compat.wineVersion.c_str());

    int failures = 0;
    const auto phase = [&](const char* name, bool ok, const std::string& detail) {
        std::printf("%-18s: %-5s %s\n", name, ok ? "ok" : "FAIL",
                    detail.c_str());
        if (!ok) ++failures;
    };

    const std::wstring stateRoot = qplat::stateRootW();
    const bool stateOk = !stateRoot.empty()
        && qplat::ensureDirW(stateRoot)
        && qplat::isDirWritableW(stateRoot);
    phase("state directory", stateOk,
          qplat::narrow(stateRoot) + " (source="
              + std::string(qplat::narrow(qplat::stateRootSourceName())) + ")");

    std::vector<std::wstring> environment;
    const bool envRead = qplat::readParentEnvironmentW(environment);
    const size_t envCountBefore = environment.size();
    const bool envMerged = envRead && qplat::mergeDdrawOverride(environment);
    std::vector<wchar_t> envBlock;
    const bool envBuilt = envMerged
        && qplat::buildEnvironmentBlockW(environment, envBlock);
    {
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "%zu entries in, %zu out, block %zu wchar",
                      envCountBefore, environment.size(), envBlock.size());
        phase("environment", envBuilt, detail);
    }

    bool socketOk = false;
    std::string socketDetail;
    {
        const bool winsockUp = netInit();
        SOCKET probe = winsockUp
            ? ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
            : INVALID_SOCKET;
        if (probe == INVALID_SOCKET) {
            socketDetail = winsockUp ? "socket() failed" : "WSAStartup failed";
        } else {
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            if (::bind(probe, reinterpret_cast<sockaddr*>(&addr),
                       sizeof(addr)) != 0) {
                socketDetail = "bind() failed";
            } else {
                WSAPROTOCOL_INFOW info{};
                if (WSADuplicateSocketW(probe, GetCurrentProcessId(), &info)
                    != 0) {
                    socketDetail = "WSADuplicateSocketW failed";
                } else {
                    const bool repaired =
                        qplat::repairDuplicatedUdpProtocolInfo(info);
                    SOCKET adopted = WSASocketW(FROM_PROTOCOL_INFO,
                                                FROM_PROTOCOL_INFO,
                                                FROM_PROTOCOL_INFO, &info, 0, 0);
                    socketOk = adopted != INVALID_SOCKET;
                    socketDetail = qplat::describeProtocolInfo(info)
                        + (repaired ? " (repaired)" : " (as reported)");
                    if (adopted != INVALID_SOCKET) closesocket(adopted);
                }
            }
            closesocket(probe);
        }
        phase("udp duplication", socketOk, socketDetail);
    }

    bool ipcOk = false;
    std::string ipcDetail;
    {
        wchar_t pipeName[64];
        qipc::ipcPipeName(pipeName, 64, GetCurrentProcessId());
        HANDLE pipe = CreateNamedPipeW(
            pipeName,
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED
                | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
            sizeof(qipc::IpcConfig) + 64, sizeof(qipc::IpcConfig) + 64, 0,
            nullptr);
        wchar_t eventName[64];
        qipc::ipcReadyEventName(eventName, 64, GetCurrentProcessId());
        HANDLE evt = CreateEventW(nullptr, TRUE, FALSE, eventName);
        ipcOk = pipe != INVALID_HANDLE_VALUE && evt != nullptr
            && SetEvent(evt) && WaitForSingleObject(evt, 0) == WAIT_OBJECT_0;
        ipcDetail = ipcOk ? "named pipe + Local\\ event"
                          : "CreateNamedPipeW/CreateEventW failed";
        if (evt) CloseHandle(evt);
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        phase("ipc primitives", ipcOk, ipcDetail);
    }

    bool injectOk = false;
    std::string injectDetail;
    {
        std::wstring self = qplat::exePathW();
        std::wstring cmd = L"\"" + self + L"\" --wine-fixture --fixture-child";
        std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
        mutableCmd.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(self.c_str(), mutableCmd.data(), nullptr, nullptr,
                            FALSE,
                            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                            envBuilt ? envBlock.data() : nullptr,
                            nullptr, &si, &pi)) {
            injectDetail = "CreateProcessW failed";
        } else {
            uintptr_t base = 0;
            bool usedCandidate = false;
            std::string err;
            const uintptr_t localKernel32 =
                reinterpret_cast<uintptr_t>(GetModuleHandleW(L"kernel32.dll"));
            qplat::PeImageInfo localInfo{};
            std::string localErr;
            const bool haveLocal =
                localKernel32 != 0
                && qplat::remoteValidateModule(GetCurrentProcess(),
                                               localKernel32, localInfo,
                                               localErr);
            bool verifiedBeforeStart = false;
            if (haveLocal) {
                verifiedBeforeStart =
                    qplat::remoteModuleBaseOrValidatedCandidate(
                        pi.hProcess, L"kernel32.dll", localKernel32,
                        localInfo.sizeOfImage, "LoadLibraryW", base,
                        usedCandidate, err);
            }
            const bool located = verifiedBeforeStart || base != 0;
            if (!haveLocal) {
                injectDetail = "local kernel32 not inspectable: " + localErr;
            } else if (!located) {
                injectDetail = "candidate resolve: " + err;
            } else {
                ResumeThread(pi.hThread);
                uintptr_t confirmed = 0;
                std::string confirmErr;
                bool confirmedOk = false;
                for (int attempt = 0; attempt < 40 && !confirmedOk; ++attempt) {
                    Sleep(25);
                    confirmedOk = qplat::remoteModuleBase(
                        pi.hProcess, L"kernel32.dll", confirmed, confirmErr);
                }
                char detail[240];
                if (!confirmedOk) {
                    std::snprintf(detail, sizeof(detail),
                                  "candidate=0x%08llX (%s) but post-loader "
                                  "confirm failed: %s",
                                  static_cast<unsigned long long>(base),
                                  verifiedBeforeStart ? "verified pre-start"
                                                      : "deferred",
                                  confirmErr.c_str());
                    injectDetail = detail;
                } else {
                    qplat::PeImageInfo info{};
                    if (!qplat::remoteValidateModule(pi.hProcess, confirmed,
                                                     info, err)) {
                        injectDetail = "remoteValidateModule: " + err;
                    } else {
                        std::snprintf(
                            detail, sizeof(detail),
                            "kernel32 base=0x%08llX (%s) machine=0x%04X size=%u",
                            static_cast<unsigned long long>(confirmed),
                            verifiedBeforeStart
                                ? (usedCandidate ? "candidate verified pre-start"
                                                 : "enumerated pre-start")
                                : "candidate, confirmed after resume",
                            static_cast<unsigned>(info.machine),
                            static_cast<unsigned>(info.sizeOfImage));
                        injectDetail = detail;
                        injectOk = info.machine == IMAGE_FILE_MACHINE_I386;
                        if (!injectOk)
                            injectDetail += " (expected PE32/i386)";
                    }
                }
            }
            TerminateProcess(pi.hProcess, 0);
            WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        phase("suspended child", injectOk, injectDetail);
    }

    std::printf("\n%s\n",
                failures == 0
                    ? "All launch mechanics available. This does NOT prove "
                      "D3D, audio, controller, or gameplay behaviour."
                    : "One or more launch mechanics are unavailable; see the "
                      "FAIL lines above.");
    return failures == 0 ? 0 : 1;
}

int runAnalyzeLayout(int argc, char** argv) {
    if (argc < 4) {
        std::printf(
            "Usage: qoh99caster analyze-layout <yours.txt> <theirs.txt>\n\n"
            "Both files are written as logs/session_*/layout_mismatch.txt\n"
            "when a match stops because the installs differ.\n");
        return 1;
    }
    const auto read = [](const char* path,
                         std::map<std::string, std::string>& out,
                         std::string& contract) -> bool {
        FILE* f = std::fopen(path, "r");
        if (f == nullptr) {
            std::printf("cannot open %s\n", path);
            return false;
        }
        char line[512];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            std::string text(line);
            while (!text.empty()
                   && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            if (text.empty() || text[0] == '#') continue;
            std::size_t firstSpace = text.find(' ');
            if (firstSpace == std::string::npos) continue;
            const std::string kind = text.substr(0, firstSpace);
            std::string rest = text.substr(firstSpace + 1);
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            if (kind == "contract") { contract = rest; continue; }
            std::string key = kind;
            std::size_t secondSpace = rest.find(' ');
            if (kind == "region" || kind == "component") {
                key += " " + rest.substr(0, secondSpace);
                rest = secondSpace == std::string::npos
                     ? std::string()
                     : rest.substr(secondSpace + 1);
            }
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            out[key] = rest;
        }
        std::fclose(f);
        return true;
    };

    std::map<std::string, std::string> mine, theirs;
    std::string mineContract, theirsContract;
    if (!read(argv[2], mine, mineContract)) return 1;
    if (!read(argv[3], theirs, theirsContract)) return 1;

    std::printf("comparing:\n  yours : %s\n  theirs: %s\n\n", argv[2],
                argv[3]);
    if (mineContract != theirsContract) {
        std::printf("BUILD CONTRACT DIFFERS - you are running different\n"
                    "versions of the netplay package. Nothing else below\n"
                    "matters until both sides run the same release.\n\n"
                    "  yours : %s\n  theirs: %s\n",
                    mineContract.c_str(), theirsContract.c_str());
        return 2;
    }

    int differences = 0;
    for (const auto& entry : mine) {
        const auto found = theirs.find(entry.first);
        if (found == theirs.end()) {
            std::printf("ONLY YOURS   %s = %s\n", entry.first.c_str(),
                        entry.second.c_str());
            ++differences;
        } else if (found->second != entry.second) {
            std::printf("DIFFERS      %s\n    yours : %s\n    theirs: %s\n",
                        entry.first.c_str(), entry.second.c_str(),
                        found->second.c_str());
            ++differences;
        }
    }
    for (const auto& entry : theirs) {
        if (mine.find(entry.first) == mine.end()) {
            std::printf("ONLY THEIRS  %s = %s\n", entry.first.c_str(),
                        entry.second.c_str());
            ++differences;
        }
    }

    if (differences == 0) {
        std::printf("No differences. Both installs agree on every component\n"
                    "and region, so the mismatch was not in the layout - keep\n"
                    "the two dll.log files for a closer look.\n");
        return 0;
    }
    std::printf("\n%d difference(s).\n\n", differences);
    std::printf(
        "What to do with this:\n"
        "  component audioManifest / region audio*  the sound or BGM files\n"
        "      differ. Copy the host's System folder over yours.\n"
        "  component fpu                            different CPU/driver\n"
        "      floating-point state. Report this - it is not yours to fix.\n"
        "  region <gameplay state>                  the two installs are\n"
        "      running different game builds or patches.\n");
    return differences == 0 ? 0 : 3;
}

void usage() {
    std::printf(
        "qoh99caster - QOH99 online play\n\n"
        "  (no args)              open the menu\n"
        "  host [port]            host a game (defaults to the saved port)\n"
        "  join <ip[:port]>       join a host\n"
        "  spectate <ip[:port]>   watch a game at the player's host address\n"
        "  status <ip[:port]>     print the host's live match data as JSON\n"
        "  replays                browse and watch saved replays\n"
        "  offline                launch the game for local play\n"
        "\n"
        "  options:  --game <GAME.EXE>   --dll <qoh99_netplay.dll>\n"
        "            --nickname <name>   UTF-8 display name (20 bytes max)\n"
        "            --delay N --rollback N   tuning for unattended host/join\n"
        "            --show-game-score / --show-game-timer / --show-game-hud\n"
        "                bring back QOH's own SCORE/TIME (both are hidden by\n"
        "                default so the overlay can use that space)\n"
        "\n"
        "  developer diagnostics: qoh99caster help dev\n");
}

void usageDev() {
    usage();
    std::printf(
        "\ndeveloper verbs and flags:\n"
        "  analyze-layout <a> <b>     compare two layout_mismatch.txt dumps\n"
        "        and report exactly which component or region differs\n"
        "  --tests                    run transport unit tests\n"
        "  --dev                      show the Diagnostics submenu in the interactive menu\n"
        "  relay [--bind IPV4] [--port N] [...]\n"
        "        run the rendezvous + raw UDP relay service in this process.\n"
        "        The standalone qoh99relay.exe is retired; this verb IS the\n"
        "        service. Defaults: bind 0.0.0.0, port 14764.\n"
        "        `qoh99caster relay --help` lists every service option.\n"
        "  replay list [--root <directory>]\n"
        "  replay inspect <file>\n"
        "  replay verify <file> [--simulate]\n"
        "  replay play <file> [--match <ordinal>]\n"
        "        headless access to the same validated replay model the\n"
        "        Replays page uses. `inspect` and structural `verify` never\n"
        "        launch QOH; `play` and `verify --simulate` use exactly the\n"
        "        same preflight, compatibility decision, broker and run mode\n"
        "        as the browser -- there is no weaker parser and no force path.\n"
        "  replays                    open the Replays page (also in the menu)\n"
        "  --wine-fixture             game-free launch fixture: proves process\n"
        "        creation, environment merge, remote module/RVA resolution, the\n"
        "        named pipe, the Local\\ events, and UDP socket duplication plus\n"
        "        adoption, using this executable as its own child. It qualifies\n"
        "        a Wine/Proton prefix before anyone owns a copy of the game, and\n"
        "        proves nothing about D3D, audio, controllers, or gameplay.\n"
        "  host --relay <ip[:port]> [--room <32-hex>]\n"
        "        explicit relay-room hosting (diagnostic; automatic fallback\n"
        "        covers the normal path).\n"
        "  join --relay <ip[:port]> --room <32-hex> [--force-relay]\n"
        "        outbound control pairing + direct UDP hole punch first, then\n"
        "        raw UDP relay fallback; --force-relay is a diagnostic override.\n"
        "  spectate <ip[:port]>\n"
        "        use the same player address shown by the host.\n"
        "        --spectator-route auto|direct|punch|relay forces one route\n"
        "        for real-caster diagnostics; ordinary play uses auto.\n"
        "  host ... --spectator-retain-ms N\n"
        "        headless late-join diagnostic: keep the completed spectator\n"
        "        archive/service alive for 1..120000 ms after the game exits.\n"
        "  offline [--selftest|--observe] [--mode 1v2|1vcpu|training] [--trace-gameplay-residual]\n"
        "  probe --<input-map|fighter-writes|fighter-pointers|fighter-size|intro|blocks|timer|rng|helpers|all> [--hidden-desktop]\n"
        "  analyze-desync <desync_dump.jsonl>   summarize a session's desync dump\n"
        "  flags:\n"
        "        --rendezvous-only          diagnostic: preflight + route commit, no game process\n"
        "        --hidden-desktop           run the game on an invisible desktop (won't take over your screen)\n"
        "        --trace-gameplay-residual  dump per-tick gate snapshots to diagnose the K=1 residual\n"
        "        --pacing-replay            EXPERIMENT: record/replay the sub_4704E0 pacing verdict\n"
        "        --replay-tick <mode>       replay tick mode: deterministic|natural|pacingshim (default natural)\n"
        "        --double-replay            diagnostic: replay each frame twice + dump the [feeder] diff\n"
        "        --cosmetic-rng <mode>      RNG hook: trace (count/attribute) | whitelist | off\n"
        "        --k <1|2|4|8>              pin the selftest K window (default: auto-grow 1->8)\n"
        "        --input-test               drive P1 with the verification script + report [input_test]\n"
        "        --input-sequence deterministic   enable the pseudo-AI input stream (both players)\n");
}

}

bool validLivePresence(const qnet::LivePresence& presence) {
    return validLivePresenceImpl(presence);
}

std::string livePresenceJson(const qnet::LivePresence& presence) {
    return livePresenceJsonImpl(presence);
}

std::string qoh99ControlsBindingText(
    const std::vector<qinput::ProfileBinding>& bindings,
    bool preferController, bool required) {
    return controls_ui::bindingRowText(bindings, preferController, required);
}

std::string qoh99ControlsSourceText(const qinput::InputProfile& profile,
                                    const std::string& connectedPadName) {
    return controls_ui::sourceRowText(profile, connectedPadName);
}

bool qoh99ControlsUsesController(const qinput::InputProfile& profile) {
    return controls_ui::profileUsesController(profile);
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--tests") == 0) {
        if (!clientIntentRecordSelfTest()) {
            std::printf("[FAIL] CTRL13 client-intent structural selftest\n");
            return 1;
        }
        return qoh99_run_tests();
    }

    if (argc >= 2 && std::strcmp(argv[1], "help") == 0) {
        if (argc >= 3 && (std::strcmp(argv[2], "dev") == 0
                          || std::strcmp(argv[2], "--dev") == 0)) {
            usageDev();
        } else {
            usage();
        }
        return 0;
    }

    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--dev") == 0) g_devMode = true;

    const bool skipAvPreflight =
        qclient::antivirusPreflightShouldSkip(argc, argv);
    const qclient::AntivirusPreflightReport avReport =
        skipAvPreflight ? qclient::AntivirusPreflightReport{}
                        : qclient::preflightAntivirus();
    if (avReport.degraded) {
        std::printf("\n");
        for (const std::string& line :
                 qclient::antivirusPreflightMessages(avReport)) {
            std::printf("  %s\n", line.c_str());
        }
        std::printf(
            "\n  Run defender_exclusions.cmd as administrator to add the "
            "exclusion,\n  or see README.txt under \"Antivirus false "
            "positives\".\n\n");
    }
    qclient::markRunStarted();

    initSessionLog();
    struct SessionProcessCleanup {
        ~SessionProcessCleanup() {
            qclient::markRunEndedCleanly();
            retireRetainedSpectatorService();
            qoh99::Log::get().close();
            qclient::releaseSessionLogWriterLease(g_sessionLogWriterLease);
            if (RemoveDirectoryW(qplat::widen(g_sessionDir).c_str())) return;
            const qclient::SessionLogArchiveResult archive =
                qclient::archiveClosedSessionLogDirectory(g_sessionDir);
            if (!archive.archivePath.empty()) {
                std::printf("%s: %s\n",
                            qclient::sessionLogArchiveStatusText(archive.status),
                            archive.archivePath.c_str());
            } else {
                std::printf("%s.\n",
                            qclient::sessionLogArchiveStatusText(archive.status));
            }
        }
    } sessionProcessCleanup;

    if (argc < 2 || std::strcmp(argv[1], "menu") == 0
        || std::strcmp(argv[1], "--dev") == 0)
        return interactive();

    if (std::strcmp(argv[1], "analyze-layout") == 0)
        return runAnalyzeLayout(argc, argv);

    if (std::strcmp(argv[1], "analyze-desync") == 0) {
        if (argc < 3) { std::printf("Usage: qoh99caster analyze-desync <desync_dump.jsonl>\n"); return 1; }
        FILE* f = std::fopen(argv[2], "r");
        if (!f) { std::printf("cannot open %s\n", argv[2]); return 1; }
        char line[2048];
        int n = 0, advMismatch = 0, rngDiverged = 0, restoreDirty = 0;
        std::map<std::string, int> byRegion;
        while (std::fgets(line, sizeof(line), f)) {
            n++;
            auto field = [&](const char* key) -> std::string {
                std::string k = std::string("\"") + key + "\":";
                const char* p = std::strstr(line, k.c_str());
                if (!p) return "";
                p += k.size();
                if (*p == '"') { const char* e = std::strchr(p + 1, '"'); return e ? std::string(p + 1, e) : ""; }
                const char* e = p; while (*e && *e != ',' && *e != '}') ++e;
                return std::string(p, e);
            };
            byRegion[field("region")]++;
            if (field("advanceTruth") != field("advanceReplay")) advMismatch++;
            if (field("gameSeedTruth") != field("gameSeedReplay")) rngDiverged++;
            if (field("restoreDiff") != "0") restoreDirty++;
        }
        std::fclose(f);
        std::printf("desync_dump.jsonl: %d desyncs\n", n);
        std::printf("  gameplay-RNG diverged : %d\n", rngDiverged);
        std::printf("  advance mismatch      : %d\n", advMismatch);
        std::printf("  dirty restore         : %d\n", restoreDirty);
        std::printf("  by region:\n");
        for (auto& kv : byRegion) std::printf("    %-24s %d\n", kv.first.c_str(), kv.second);
        return n ? 2 : 0;
    }

    if (std::strcmp(argv[1], "probe") == 0) {
        Settings ps; ps.load(); ps.gamePath = ps.gamePath.empty() ? defaultGamePath() : ps.gamePath;
        g_localName = ps.nickname;
        std::string m = (argc >= 3) ? argv[2] : "";
        if (m.rfind("--", 0) == 0) m = m.substr(2);
        bool hidden = false;
        for (int i = 3; i < argc; ++i) {
            std::string o = argv[i];
            if (o == "--game" && i + 1 < argc) ps.gamePath = argv[++i];
            else if (o == "--dll" && i + 1 < argc) ps.dllPath = argv[++i];
            else if (o == "--hidden-desktop" || o == "--background") hidden = true;
        }
        uint8_t pm = probeModeFromName(m);
        if (!pm) { std::printf("Usage: qoh99caster probe --<input-map|fighter-writes|fighter-pointers|fighter-size|intro|blocks|timer|rng|helpers|all> [--hidden-desktop]\n"); return 1; }
        return runProbe(ps, pm, nullptr, hidden);
    }

    if (std::strcmp(argv[1], "relay") == 0)
        return qrelay::runRelayVerb(argc - 1, argv + 1);

    if (std::strcmp(argv[1], "replay") == 0)
        return runReplayVerb(argc, argv);

    if (std::strcmp(argv[1], "replays") == 0) {
        qui::Console con;
        Settings rs;
        rs.gamePath = defaultGamePath();
        rs.load();
        if (rs.gamePath.empty()) rs.gamePath = defaultGamePath();
        g_localName = rs.nickname;
        con.statusLine("Checking the game files...");
        gameFilesCheck(rs);

        con.setBanner(casterBanner(rs.nickname));
        replays_ui::replaysMenu(con, rs);

        return 0;
    }

    if (std::strcmp(argv[1], "--wine-fixture") == 0
        || std::strcmp(argv[1], "wine-fixture") == 0)
        return runWineFixture(argc, argv);

    Args a;
    a.s.load();
    if (!parseArgs(argc, argv, a)) { usage(); return 1; }
    qi18n::setLanguageFromIniValue(a.s.language);
    g_spectatorRetainDiagnosticMs = a.spectatorRetainMs;
    g_localName = a.s.nickname;
    if (a.verb == "host")
        return a.relay.enabled
            ? runHost(a.s, nullptr, a.flagDelay, a.flagRollback,
                      a.hiddenDesktop, &a.relay, a.rendezvousOnly)
            : runAutomaticHost(a.s, nullptr, a.flagDelay, a.flagRollback,
                               a.hiddenDesktop, a.relay.forceRelay,
                               a.rendezvousOnly);
    if (a.verb == "join")
        return a.relay.enabled
            ? runJoin(a.s, nullptr, a.hiddenDesktop, &a.relay, nullptr,
                      a.rendezvousOnly)
            : runAutomaticJoin(a.s, nullptr, a.hiddenDesktop,
                               a.relay.forceRelay, a.rendezvousOnly);
    if (a.verb == "spectate") {
        g_spectateWantsLatest = a.spectatorWantsLatest;
        SpectateInfo info{};
        std::string discoveryError;
        const uint16_t controlPort = a.s.joinPort;
        if (!queryHostSpectateInfo(
                a.s.joinIp, controlPort, info, discoveryError)) {
            return fail(nullptr, "Can't reach that spectator host",
                        discoveryError);
        }
        if (info.state != SPECTATE_LIVE) {
            return fail(nullptr, "No match to watch",
                        "That caster answered, but it is not currently "
                        "serving a live match.");
        }
        if (info.spectatorPort == 0
            || (info.keyLow == 0 && info.keyHigh == 0)) {
            return fail(nullptr, "Can't watch that match",
                        "The host is not accepting address-based "
                        "spectators right now.");
        }
        a.s.joinPort = info.spectatorPort;
        return runSpectator(
            a.s, info.sessionId, info.keyLow, info.keyHigh, nullptr,
            a.hiddenDesktop, controlPort, a.spectatorRouteForce,
            a.spectatorTopologyOptIn);
    }
    if (a.verb == "status") {
        SpectateInfo info{};
        std::string discoveryError;
        if (!queryHostSpectateInfo(
                a.s.joinIp, a.s.joinPort, info, discoveryError,
                INTENT_STATUS)) {
            std::fprintf(stderr, "Could not query %s:%u: %s\n",
                         a.s.joinIp.c_str(),
                         static_cast<unsigned>(a.s.joinPort),
                         discoveryError.c_str());
            return 2;
        }
        if (!validLivePresence(info.presence)) {
            std::fprintf(stderr,
                         "Host returned malformed live presence data.\n");
            return 3;
        }
        std::printf("%s\n", livePresenceJson(info.presence).c_str());
        return 0;
    }
    if (a.verb == "offline") {
        const bool training = a.mode == "training";
        const bool allowNativeDeveloperKeys =
            a.wireMode == qipc::MODE_OBSERVE && !training;
        uint8_t vsRow = (a.mode == "1vcpu")
            ? qipc::kVsRow1vCpu : qipc::kVsRow1v1;
        return runOffline(a.s, a.wireMode, vsRow, nullptr, a.hiddenDesktop,
                           true, training,
                           allowNativeDeveloperKeys);
    }
    usage();
    return 1;
}
