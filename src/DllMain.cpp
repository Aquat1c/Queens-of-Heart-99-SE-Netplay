#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "GameHooks.h"
#include "game/DPlayTransport.h"
#include "game/DirectBoot.h"
#include "game/SessionPresentation.h"
#include "game/RuntimeDelayControl.h"
#include "qoh99/qoh99_probe.h"
#include "Log.h"
#include "Paths.h"
#include "CrashDump.h"
#include "ipc/IpcConfig.h"
#include "ipc/BuildContract.h"
#include "input/SdlLifecycle.h"
#include "game/replay/ReplayRuntime.h"
#include "game/spectator/SpectatorPublisher.h"
#include <cstring>
#include <cstdlib>

using namespace qoh99;

// The caster validates this ABI marker without loading the DLL, so it must stay in the PE image.
extern "C" __declspec(dllexport) const char qoh99NetplayBuildContract[] =
    QOH99_BUILD_CONTRACT_MARKER;

extern "C" __declspec(dllexport) unsigned long
qoh99np_presentation_disposition() {
    return static_cast<unsigned long>(
        qoh99::queryPresentationDisposition());
}

extern "C" __declspec(dllexport) int
qoh99np_owns_pacing() {
    return qoh99::ownsNetplayPacing() ? 1 : 0;
}

extern "C" __declspec(dllexport) int
qoh99np_owns_network_pacing() {
    return qoh99::ownsNetworkPacing() ? 1 : 0;
}

// ABI-v1 exported a void-returning qoh99np_presentation_wait_slot; resolving that name as an int function would read undefined EAX as a publish gate.
extern "C" __declspec(dllexport) int
qoh99np_presentation_try_acquire_slot_v2() {
    return qoh99::tryAcquirePresentationSlot() ? 1 : 0;
}

extern "C" __declspec(dllexport) void
qoh99np_presentation_commit() {
    qoh99::observePresentationCommit();
}

extern "C" __declspec(dllexport) void
qoh99np_presentation_failed_v3(unsigned long reason) {
    qoh99::observePresentationFailure(reason);
}

extern "C" __declspec(dllexport) void
qoh99np_presentation_report(unsigned long normalVisible,
                            unsigned long suppressed,
                            unsigned long correctedFinal) {
    qoh99::reportWrapperPresentationCounters(
        normalVisible, suppressed, correctedFinal);
}

static RunMode readModeFromEnv() {
    char buf[32] = {0};
    DWORD n = GetEnvironmentVariableA("QOH99_NP_MODE", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return RunMode::Off;
    for (char* p = buf; *p; ++p) *p = (char)tolower((unsigned char)*p);
    if (std::strcmp(buf, "observe")  == 0) return RunMode::Observe;
    if (std::strcmp(buf, "selftest") == 0) return RunMode::SelfTest;
    return RunMode::Off;
}

static const char* modeName(RunMode m) {
    switch (m) {
        case RunMode::Observe:  return "observe";
        case RunMode::SelfTest: return "selftest";
        case RunMode::Netplay:  return "netplay";
        case RunMode::Spectator:return "spectator";
        case RunMode::Replay:   return "replay";
        default:                return "off";
    }
}

static void signalStartupFailure();
static bool g_ipcConfigRejected = false;

static bool readIpcConfig(qipc::IpcConfig& cfg) {
    wchar_t pipeName[64];
    qipc::ipcPipeName(pipeName, 64, GetCurrentProcessId());
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 20; ++attempt) {
        pipe = CreateFileW(pipeName, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(pipeName, 200);
        else Sleep(50);
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    DWORD read = 0;
    BOOL ok = TRUE;
    const DWORD deadline = GetTickCount() + 5000;
    auto* dst = reinterpret_cast<uint8_t*>(&cfg);
    while (read < sizeof(cfg)) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            ok = FALSE;
            break;
        }
        if (available == 0) {
            if (static_cast<LONG>(GetTickCount() - deadline) >= 0) {
                SetLastError(ERROR_TIMEOUT);
                ok = FALSE;
                break;
            }
            Sleep(5);
            continue;
        }
        DWORD chunk = 0;
        DWORD want = static_cast<DWORD>(sizeof(cfg) - read);
        if (available < want) want = available;
        if (!ReadFile(pipe, dst + read, want, &chunk, nullptr) || chunk == 0) {
            ok = FALSE;
            break;
        }
        read += chunk;
    }
    CloseHandle(pipe);
    if (!ok || read != sizeof(cfg) ||
        cfg.magic != qipc::kIpcMagic || cfg.version != qipc::kIpcVersion) {
        QLOG("[ipc] pipe present but config invalid "
             "(ok=%d read=%lu/%zu magic=%08X version=%u expectedVersion=%u)",
             ok, read, sizeof(cfg), cfg.magic, cfg.version, qipc::kIpcVersion);
        g_ipcConfigRejected = true;
        signalStartupFailure();
        return false;
    }
    cfg.sessionDir[sizeof(cfg.sessionDir) - 1] = '\0';
    cfg.localName[sizeof(cfg.localName) - 1] = '\0';
    cfg.remoteName[sizeof(cfg.remoteName) - 1] = '\0';
    if (cfg.mode > qipc::MODE_REPLAY ||
        cfg.isHost > 1 ||
        (cfg.mode != qipc::MODE_SPECTATOR && cfg.mode != qipc::MODE_REPLAY
         && (cfg.localPlayer < 1 || cfg.localPlayer > 2)) ||
        (cfg.mode == qipc::MODE_SPECTATOR && cfg.localPlayer != 0) ||
        qipc::validateSessionDir(cfg) != nullptr ||
        qipc::validateReplayLaunch(cfg) != nullptr ||
        qipc::validateRecorderLaunch(cfg) != nullptr ||
        qipc::validateSpectatorService(cfg) != nullptr ||
        qipc::validateSpectatorSocketHandoff(cfg) != nullptr ||
        qipc::validateSpectatorRelayViewer(cfg) != nullptr ||
        qipc::validateSpectatorTopologyClient(cfg) != nullptr ||
        qipc::validateSpectatorClientPrivatePublisher(cfg) != nullptr ||
        cfg.delay > qgame::kMaximumGameplayDelay ||
        cfg.rollback > qgame::kMaximumGameplayRollback ||
        cfg.remoteDelay > qgame::kMaximumGameplayDelay ||
        cfg.remoteRollback > qgame::kMaximumGameplayRollback ||
        cfg.maxDelay < cfg.delay || cfg.maxRollback < cfg.rollback ||
        cfg.directBoot > 1 ||
        cfg.vsRow > qipc::kVsRow1v1 ||
        !((cfg.probeMode <= static_cast<uint8_t>(qoh99::ProbeMode::Helpers)) ||
          cfg.probeMode == static_cast<uint8_t>(qoh99::ProbeMode::All)) ||
        cfg.hasSocket > 1 || cfg.hasSpectatorSocket > 1 ||
        cfg.serveSpectators > 1 ||
        cfg.hideNativeScore > 1 || cfg.hideNativeTimer > 1 ||
        cfg.disableExtraFunctions > 1 ||
        qipc::validateTrainingLaunch(cfg) != nullptr ||
        !qgame::isCanonicalPlayerNameForCodePage(
            cfg.localName, cfg.localNameCodePage) ||
        !qgame::isCanonicalPlayerNameForCodePage(
            cfg.remoteName, cfg.remoteNameCodePage) ||
        (cfg.mode == qipc::MODE_NETPLAY &&
          (!cfg.hasSocket || !cfg.peerAddrBE || !cfg.peerPortBE ||
           cfg.compatibilityId != qipc::kRuntimeCompatibilityId ||
           qipc::validateIpcCompatibilityIdentity(cfg) != nullptr ||
           qipc::validateIpcConfigurationProvenance(cfg) != nullptr ||
           (cfg.cookieLow == 0 && cfg.cookieHigh == 0) ||
          cfg.sessionId == 0)) ||
        (cfg.mode == qipc::MODE_SPECTATOR &&
         (cfg.isHost || !cfg.hasSocket || !cfg.peerAddrBE
          || !cfg.peerPortBE
          || cfg.compatibilityId != qipc::kRuntimeCompatibilityId
          || qipc::validateIpcCompatibilityIdentity(cfg) != nullptr
          || (cfg.spectatorCookieLow == 0
              && cfg.spectatorCookieHigh == 0)
          || cfg.sessionId == 0))) {
        const char* sessionDirWhy = qipc::validateSessionDir(cfg);
        const char* replayWhy = qipc::validateReplayLaunch(cfg);
        const char* recorderWhy = qipc::validateRecorderLaunch(cfg);
        const char* spectatorWhy = qipc::validateSpectatorService(cfg);
        const char* spectatorSocketWhy =
            qipc::validateSpectatorSocketHandoff(cfg);
        const char* spectatorRelayWhy =
            qipc::validateSpectatorRelayViewer(cfg);
        const char* spectatorTopologyWhy =
            qipc::validateSpectatorTopologyClient(cfg);
        const char* spectatorClientPublisherWhy =
            qipc::validateSpectatorClientPrivatePublisher(cfg);
        const char* compatibilityWhy =
            qipc::validateIpcCompatibilityIdentity(cfg);
        const char* configurationProvenanceWhy =
            cfg.mode == qipc::MODE_NETPLAY
                ? qipc::validateIpcConfigurationProvenance(cfg) : nullptr;
        QLOG("[ipc] config values invalid "
             "(mode=%u host=%u player=%u delay=%u rollback=%u directBoot=%u "
             "vsRow=%u hasSocket=%u hasSpectatorSocket=%u "
             "serveSpectators=%u spectatorService=%u recordEnabled=%u "
             "compatFlags=%02X hideScore=%u hideTimer=%u training=%u "
             "trainingReason='%s' sessionDirReason='%s' replayReason='%s' "
              "recorderReason='%s' "
              "spectatorReason='%s' spectatorSocketReason='%s' "
               "spectatorRelayReason='%s' "
               "spectatorTopologyReason='%s' "
               "spectatorClientPublisherReason='%s' "
               "compatibilityReason='%s' configurationProvenanceReason='%s')",
             cfg.mode, cfg.isHost, cfg.localPlayer, cfg.delay, cfg.rollback,
             cfg.directBoot, cfg.vsRow, cfg.hasSocket,
             cfg.hasSpectatorSocket, cfg.serveSpectators,
             cfg.spectatorServiceMode, cfg.recordEnabled, cfg.compatFlags,
             cfg.hideNativeScore, cfg.hideNativeTimer,
             cfg.trainingEnabled,
             qipc::validateTrainingLaunch(cfg)
                 ? qipc::validateTrainingLaunch(cfg) : "none",
             sessionDirWhy ? sessionDirWhy : "none",
             replayWhy ? replayWhy : "none",
              recorderWhy ? recorderWhy : "none",
              spectatorWhy ? spectatorWhy : "none",
              spectatorSocketWhy ? spectatorSocketWhy : "none",
               spectatorRelayWhy ? spectatorRelayWhy : "none",
               spectatorTopologyWhy ? spectatorTopologyWhy : "none",
               spectatorClientPublisherWhy
                   ? spectatorClientPublisherWhy : "none",
               compatibilityWhy ? compatibilityWhy : "none",
              configurationProvenanceWhy ? configurationProvenanceWhy : "none");
        g_ipcConfigRejected = true;
        signalStartupFailure();
        return false;
    }
    {
        const bool spectator = cfg.mode == qipc::MODE_SPECTATOR;
        const char* inputWhy = nullptr;
        if (cfg.inputEnvelope.magic != qinput::InputConfigEnvelope{}.magic) {
            inputWhy = "input envelope magic mismatch";
        } else if (cfg.inputEnvelope.schemaVersion
                   != qinput::kInputConfigSchemaVersion) {
            inputWhy = "input schema version mismatch";
        } else if (cfg.inputEnvelope.byteSize
                   != sizeof(qinput::InputLaunchConfig)) {
            inputWhy = "input payload length mismatch";
        } else if (cfg.inputEnvelope.canonicalHash
                   != qinput::hashInputConfigBytes(
                          &cfg.inputConfig, sizeof(cfg.inputConfig))) {
            inputWhy = "input payload hash mismatch";
        } else {
            inputWhy = qinput::validateInputLaunchConfig(cfg.inputConfig,
                                                         spectator);
            if (inputWhy == nullptr)
                inputWhy = qinput::validateCompiledModeControls(
                    cfg.modeControls, cfg.inputConfig, spectator);
        }
        if (inputWhy != nullptr
            && (cfg.mode == qipc::MODE_NETPLAY
                || cfg.mode == qipc::MODE_SPECTATOR
                || cfg.mode == qipc::MODE_REPLAY
                || cfg.trainingEnabled != 0)) {
            QLOG("[ipc] input launch configuration REJECTED: %s", inputWhy);
            g_ipcConfigRejected = true;
            signalStartupFailure();
            return false;
        }
    }
    qgame::storeCanonicalPlayerNameForCodePage(
        cfg.localName, cfg.localName, cfg.localNameCodePage);
    qgame::storeCanonicalPlayerNameForCodePage(
        cfg.remoteName, cfg.remoteName, cfg.remoteNameCodePage);
    QLOG("[ipc] config: mode=%u host=%u localPlayer=%u delay=%u rollback=%u "
         "playerSocket=%u spectatorSocket=%u serveSpectators=%u "
         "inputMode=%u inputProbes=%u modeControls=%u training=%u",
         cfg.mode, cfg.isHost, cfg.localPlayer, cfg.delay, cfg.rollback,
          cfg.hasSocket, cfg.hasSpectatorSocket, cfg.serveSpectators,
          static_cast<unsigned>(cfg.inputConfig.sourceMode),
           cfg.inputConfig.probeCount, cfg.modeControls.count,
           cfg.trainingEnabled);
    if (cfg.mode == qipc::MODE_NETPLAY) {
        QLOG("[config-provenance] v=1 role=runtime stage=%s committed=1 "
             "local_delay=%u local_rollback=%u "
             "local_delay_origin=%s local_rollback_origin=%s "
             "peer_delay=%u peer_rollback=%u "
             "peer_delay_origin=%s peer_rollback_origin=%s "
             "local_runtime_applied=0 peer_runtime_applied=0 "
             "peer_runtime_attestation=%s",
             qnetplay::config_provenance::stageName(
                 qnetplay::config_provenance::Stage::IpcDelivered),
             cfg.delay, cfg.rollback,
             qnetplay::config_provenance::stageName(
                 static_cast<qnetplay::config_provenance::Stage>(
                     cfg.configurationProvenance.localDelayOrigin)),
             qnetplay::config_provenance::stageName(
                 static_cast<qnetplay::config_provenance::Stage>(
                     cfg.configurationProvenance.localRollbackOrigin)),
             cfg.remoteDelay, cfg.remoteRollback,
             qnetplay::config_provenance::stageName(
                 static_cast<qnetplay::config_provenance::Stage>(
                     cfg.configurationProvenance.peerDelayOrigin)),
             qnetplay::config_provenance::stageName(
                 static_cast<qnetplay::config_provenance::Stage>(
                     cfg.configurationProvenance.peerRollbackOrigin)),
             qnetplay::config_provenance::stageName(
                 static_cast<qnetplay::config_provenance::Stage>(
                     cfg.configurationProvenance.peerRuntimeAttestation)));
    }
    return true;
}

static void emitSpectatorClientPublisherRuntimeAppliedReceipt(
    const qipc::IpcConfig& cfg) {
    if (cfg.spectatorClientPublisherEnabled != 1u) return;

    qipc::SpectatorClientPublisherRuntimeAppliedReceipt receipt{};
    const uint32_t pid = static_cast<uint32_t>(GetCurrentProcessId());
    if (!qipc::buildSpectatorClientPublisherRuntimeAppliedReceipt(
            cfg, pid, receipt)) {
        QLOG("[spectator.client-publisher] RuntimeApplied receipt refused by "
             "private configuration validation");
        return;
    }

    uint8_t bytes[qipc::kSpectatorClientPublisherRuntimeAppliedReceiptBytes]{};
    size_t byteCount = 0u;
    if (!qipc::encodeSpectatorClientPublisherRuntimeAppliedReceipt(
            receipt, bytes, sizeof(bytes), byteCount)) {
        QLOG("[spectator.client-publisher] RuntimeApplied receipt encoding "
             "failed");
        return;
    }

    wchar_t pipeName[128]{};
    qipc::ipcSpectatorClientPublisherPipeName(
        pipeName, sizeof(pipeName) / sizeof(pipeName[0]), pid,
        cfg.spectatorClientPublisherGeneration);
    HANDLE pipe = CreateFileW(pipeName, GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            QLOG("[spectator.client-publisher] RuntimeApplied receiver "
                 "unavailable (error=%lu generation=%u)", error,
                 cfg.spectatorClientPublisherGeneration);
        }
        return;
    }

    HANDLE completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (completion == nullptr) {
        QLOG("[spectator.client-publisher] RuntimeApplied receipt event setup "
             "failed (error=%lu generation=%u)", GetLastError(),
             cfg.spectatorClientPublisherGeneration);
        CloseHandle(pipe);
        return;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = completion;
    DWORD written = 0u;
    BOOL wrote = WriteFile(pipe, bytes, static_cast<DWORD>(byteCount),
                           &written, &overlapped);
    DWORD writeError = wrote ? ERROR_SUCCESS : GetLastError();
    if (!wrote && writeError == ERROR_IO_PENDING) {
        constexpr DWORD kReceiptWriteDeadlineMs = 250u;
        const DWORD wait = WaitForSingleObject(completion,
                                                kReceiptWriteDeadlineMs);
        if (wait == WAIT_OBJECT_0) {
            wrote = GetOverlappedResult(pipe, &overlapped, &written, FALSE);
            writeError = wrote ? ERROR_SUCCESS : GetLastError();
        } else {
            const DWORD waitError = wait == WAIT_FAILED
                ? GetLastError() : ERROR_TIMEOUT;
            (void)CancelIoEx(pipe, &overlapped);
            DWORD ignored = 0u;
            (void)GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
            writeError = waitError;
        }
    }
    CloseHandle(completion);
    CloseHandle(pipe);
    if (wrote && written == byteCount) {
        QLOG("[spectator.client-publisher] RuntimeApplied receipt emitted "
             "(generation=%u bytes=%u)",
             cfg.spectatorClientPublisherGeneration,
             static_cast<unsigned>(byteCount));
    } else {
        QLOG("[spectator.client-publisher] RuntimeApplied receipt write "
             "unavailable (error=%lu generation=%u)", writeError,
             cfg.spectatorClientPublisherGeneration);
    }
}

static RunMode wireModeToRunMode(uint8_t m) {
    switch (m) {
        case qipc::MODE_OBSERVE:  return RunMode::Observe;
        case qipc::MODE_SELFTEST: return RunMode::SelfTest;
        case qipc::MODE_NETPLAY:  return RunMode::Netplay;
        case qipc::MODE_SPECTATOR:return RunMode::Spectator;
        case qipc::MODE_REPLAY:   return RunMode::Replay;
        default:                  return RunMode::Off;
    }
}

// The game must stay suspended until this signal: its netplay init must run after the CoCreateInstance hook is live, or it creates the real DirectPlay object and dialogs.
static void signalStartupEvent(bool ready) {
    wchar_t ev[64];
    if (ready)
        qipc::ipcReadyEventName(ev, 64, GetCurrentProcessId());
    else
        qipc::ipcFailedEventName(ev, 64, GetCurrentProcessId());
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, ev);
    if (h) { SetEvent(h); CloseHandle(h); }
}

static void signalReady() { signalStartupEvent(true); }
static void signalStartupFailure() {
    Log::get().issue();
    Log::get().flushNow();
    signalStartupEvent(false);
}

static bool launcherCreatedReadyEvent() {
    wchar_t ev[64];
    qipc::ipcReadyEventName(ev, 64, GetCurrentProcessId());
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, ev);
    if (h) { CloseHandle(h); return true; }
    return false;
}

static DWORD WINAPI initThread(LPVOID) {
    // Process-lifetime module: pin outside the loader lock before installing hooks; never unmap code under hooks or the UDP pump.
    HMODULE pinnedModule = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCSTR>(&initThread),
            &pinnedModule)) {
        QLOG("[init] failed to pin runtime module error=%lu",
             GetLastError());
        signalStartupFailure();
        return 0;
    }

    qipc::IpcConfig cfg{};
    bool haveIpc = readIpcConfig(cfg);
    if (!haveIpc && g_ipcConfigRejected) {
        QLOG("[init] rejecting malformed/inconsistent IPC config");
        return 0;
    }

    if (haveIpc && qnetplay::compat::isValidCompatibilityIdentity(
            cfg.compatibilityIdentity)) {
        qcrash::cacheCompatibilityIdentity(cfg.compatibilityIdentity);
    }

    std::string dumpDir = qpath::logsRoot();
    if (haveIpc && cfg.sessionDir[0]) {
        std::string p = std::string(cfg.sessionDir) + "\\dll.log";
        Log::get().reopen(p.c_str());
        dumpDir = cfg.sessionDir;
    }
    qoh99::setSessionDir(dumpDir.c_str());
    qcrash::install(dumpDir, haveIpc ? (cfg.isHost ? "host" : "client") : "offline");
    QLOG("============================================================");
    QLOG("  qoh99_netplay.dll  built " __DATE__ " " __TIME__ "  pid=%lu", GetCurrentProcessId());
    if (haveIpc)
        QLOG("  IPC: mode=%u host=%u localPlayer=%u localName='%s' "
             "remoteName='%s' nameCodePages=[%u,%u] delay=%u "
             "rollback=%u playerSocket=%u spectatorSocket=%u "
             "serveSpectators=%u peer=%08X:%u",
             cfg.mode, cfg.isHost, cfg.localPlayer,
             cfg.localName, cfg.remoteName,
             cfg.localNameCodePage, cfg.remoteNameCodePage,
             cfg.delay, cfg.rollback,
             cfg.hasSocket, cfg.hasSpectatorSocket, cfg.serveSpectators,
             cfg.peerAddrBE, ntohs(cfg.peerPortBE));
    else
        QLOG("  no IPC config (bare launch)");

    if (haveIpc)
        qoh99::setNativeHudSuppression(cfg.hideNativeScore != 0,
                                       cfg.hideNativeTimer != 0);
    if (haveIpc)
        qoh99::setModularNativeHudEligible(
            cfg.trainingEnabled != 0 || cfg.mode == qipc::MODE_REPLAY);
    if (haveIpc)
        qoh99::setNativeDeveloperKeyLaunchPolicy(
            cfg.disableExtraFunctions == 0);
    if (haveIpc
        && (cfg.mode == qipc::MODE_NETPLAY
            || cfg.mode == qipc::MODE_SPECTATOR))
        qoh99::setNetplayExtraFunctionPolicy(cfg.disableExtraFunctions != 0);
    if (haveIpc)
        qoh99::setSpectatorServiceMode(cfg.spectatorServiceMode);
    if (haveIpc && cfg.recordEnabled != 0
        && cfg.mode != qipc::MODE_REPLAY) {
        const bool recorderUp = qgame::replay::startReplayRecorder(cfg);
        QLOG("[init] confirmed-timeline recorder %s (generation=%u)",
             recorderUp ? "up" : "unavailable - recording OFF",
             cfg.recordChannelGeneration);
        qoh99::armRecorder(recorderUp, cfg.recordChannelGeneration);
    } else if (haveIpc) {
        qoh99::armRecorder(false, 0);
    }
    qoh99::armTraining(haveIpc && cfg.trainingEnabled != 0);

    // This worker only preloads and validates the SDL runtime; SDL subsystem calls must stay on QOH's main thread.
    if (haveIpc && cfg.mode != qipc::MODE_REPLAY) {
        bool anyProbes = cfg.inputConfig.probeCount > 0;
        for (uint32_t slot = 0; !anyProbes && slot < 3; ++slot) {
            if (cfg.offlineExtraPresent[slot] != 0
                && cfg.offlineExtraConfigs[slot].probeCount > 0) {
                anyProbes = true;
            }
        }
        if (anyProbes) {
            const DWORD startedMs = GetTickCount();
            qinput::sdlPreloadRuntime();
            char sdlVersion[64];
            qinput::sdlVersionText(sdlVersion, sizeof(sdlVersion));
            QLOG("[input-v2] SDL runtime PRELOADED (map+validate only, no "
                 "subsystem API) in %lums sdl=%s - subsystem init happens on "
                 "QOH's main thread after resume",
                 static_cast<unsigned long>(GetTickCount() - startedMs),
                 sdlVersion);
        }
    }

    if (haveIpc && cfg.mode == qipc::MODE_NETPLAY) {
        QLOG("  run mode = netplay (own pipeline, game in LOCAL mode)");
        QLOG("============================================================");
        bool ok = qgame::startNetplayLink(cfg);
        QLOG("[init] netplay UDP link %s", ok ? "up" : "FAILED");
        if (!ok) {
            signalStartupFailure();
            return 0;
        }
        if (cfg.directBoot) {
            QLOG("[init] netplay direct-boot -> VS character select (vsRow=%u)", cfg.vsRow);
            if (!qgame::startNetworkDirectBootToCharSelect(cfg.vsRow)) {
                QLOG("[init] netplay direct-boot arm FAILED");
                qgame::stopDPlayTransport();
                signalStartupFailure();
                return 0;
            }
        }
        if (cfg.spectatorServiceMode == qipc::SPECTATOR_SERVICE_SIDECAR) {
            const bool clientPrivate =
                cfg.spectatorClientPublisherEnabled == 1u;
            const bool sidecarUp = clientPrivate
                ? qgame::spectator_publish::startClientPrivateSidecarPublisher(cfg)
                : qgame::spectator_publish::startSidecarPublisher(cfg);
            QLOG("[init] spectator %s publisher %s (player session is "
                 "unaffected either way)",
                 clientPrivate ? "client-private" : "host",
                 sidecarUp ? "up" : "UNAVAILABLE");
        }
        qoh99::setInputLaunchConfig(cfg.inputConfig, false);
        qoh99::setModeControls(cfg.modeControls);
        qoh99::setAudioVolumes(cfg.musicVolume, cfg.soundVolume);
        qoh99::setSessionAuthority(cfg.isHost != 0);
        armNetplay(cfg.localPlayer, cfg.delay, cfg.rollback,
                   cfg.remoteDelay, cfg.remoteRollback,
                   cfg.configurationProvenance,
                   cfg.serveSpectators != 0,
                   cfg.localName, cfg.localNameCodePage,
                   cfg.remoteName, cfg.remoteNameCodePage);
        emitSpectatorClientPublisherRuntimeAppliedReceipt(cfg);
        if (!installHooks(RunMode::Netplay)) {
            QLOG("[init] mandatory netplay hook installation FAILED");
            qgame::stopDPlayTransport();
            signalStartupFailure();
            return 0;
        }
        signalReady();
        return 0;
    }

    if (haveIpc && cfg.mode == qipc::MODE_SPECTATOR) {
        QLOG("  run mode = spectator (receive-only confirmed inputs)");
        QLOG("============================================================");
        if (!qgame::startSpectatorLink(cfg)) {
            QLOG("[init] spectator UDP link FAILED");
            signalStartupFailure();
            return 0;
        }
        if (cfg.directBoot) {
            QLOG("[init] spectator direct-boot -> VS character select "
                 "(vsRow=%u)", cfg.vsRow);
            if (!qgame::startNetworkDirectBootToCharSelect(cfg.vsRow)) {
                QLOG("[init] spectator direct-boot arm FAILED");
                qgame::stopDPlayTransport();
                signalStartupFailure();
                return 0;
            }
        }
        qoh99::setInputLaunchConfig(cfg.inputConfig, true);
        qoh99::setModeControls(cfg.modeControls);
        qoh99::setAudioVolumes(cfg.musicVolume, cfg.soundVolume);
        setSpectatorWantsLatest(
            cfg.spectatorWantsNewestCheckpoint != 0u);
        armSpectator();
        if (!installHooks(RunMode::Spectator)) {
            QLOG("[init] mandatory spectator hook installation FAILED");
            qgame::stopDPlayTransport();
            signalStartupFailure();
            return 0;
        }
        signalReady();
        return 0;
    }

    if (haveIpc && cfg.mode == qipc::MODE_REPLAY) {
        QLOG("  run mode = replay (caster-validated file playback; no socket, "
             "no player identity; takeover input dormant)");
        QLOG("============================================================");
        qoh99::setAudioVolumes(cfg.musicVolume, cfg.soundVolume);
        qoh99::setReplayTakeoverInputConfig(cfg.inputConfig);
        qoh99::armReplay(cfg.replayManifestDigest, cfg.replaySpeedPermille,
                         cfg.replayMatchOrdinal);
        if (!qgame::replay::startReplayRuntime(cfg)) {
            QLOG("[init] replay broker channel FAILED to open");
            signalStartupFailure();
            return 0;
        }
        if (cfg.directBoot) {
            QLOG("[init] replay direct-boot deferred until finite GameStart "
                 "is validated (vsRow=%u)", cfg.vsRow);
        }
        if (!installHooks(RunMode::Replay)) {
            QLOG("[init] mandatory replay hook installation FAILED");
            qgame::replay::stopReplayRuntime();
            signalStartupFailure();
            return 0;
        }
        const uint64_t replayReadyDeadline = GetTickCount64()
            + qipc::kReplayPrebufferDeadlineMs;
        while (!qgame::replay::replayChannelReadyV3()
               && !qgame::replay::replayChannelFailedV3()
               && GetTickCount64() < replayReadyDeadline) {
            Sleep(1);
        }
        if (!qgame::replay::replayChannelReadyV3()) {
            QLOG("[init] replay finite prebuffer did not become ready "
                 "(channelFailed=%u deadlineExpired=%u)",
                 qgame::replay::replayChannelFailedV3() ? 1u : 0u,
                 GetTickCount64() >= replayReadyDeadline ? 1u : 0u);
            qgame::replay::stopReplayRuntime();
            signalStartupFailure();
            return 0;
        }
        QLOG("[init] replay Stage A ready: bound finite prebuffer validated");
        signalReady();
        return 0;
    }

    if (haveIpc) {
        qoh99::setInputLaunchConfig(cfg.inputConfig, false);
        qoh99::setModeControls(cfg.modeControls);
        qoh99::setAudioVolumes(cfg.musicVolume, cfg.soundVolume);
        qinput::sdlPreloadRuntime();
        if (qipc::hashOfflineExtras(cfg) != cfg.offlineExtrasHash) {
            QLOG("[init] offline extra input payload hash mismatch - "
                 "ignoring players 2..4");
        } else {
            for (uint32_t slot = 0; slot < 3; ++slot) {
                if (cfg.offlineExtraPresent[slot] == 0) continue;
                const char* verdict = qinput::validateInputLaunchConfig(
                    cfg.offlineExtraConfigs[slot], false);
                if (verdict != nullptr) {
                    QLOG("[init] offline player %u config REJECTED: %s",
                         slot + 2u, verdict);
                    continue;
                }
                qoh99::setOfflineExtraInputConfig(
                    slot, cfg.offlineExtraConfigs[slot]);
            }
        }
    }
    RunMode mode = haveIpc ? wireModeToRunMode(cfg.mode) : readModeFromEnv();
    const bool suspendedByLauncher = launcherCreatedReadyEvent();
    if (!suspendedByLauncher) {
        signalReady();
        Sleep(1500);
    }
    QLOG("  run mode = %s   (%s, %s)", modeName(mode),
         haveIpc ? "IPC" : "env QOH99_NP_MODE",
         suspendedByLauncher ? "suspended install" : "deferred install");
    QLOG("============================================================");
    if (haveIpc && cfg.directBoot) {
        const bool unattended =
            cfg.probeMode != 0 || mode == RunMode::SelfTest;
        QLOG("[init] starting offline direct-boot (vsRow=%u, %s%s)",
             cfg.vsRow,
             cfg.trainingEnabled ? "explicit training"
             : unattended ? "unattended full boot"
                          : "stop at character select",
             cfg.trainingEnabled
                 ? ", P1 selects both characters" : "");
        if (cfg.trainingEnabled) {
            if (!qgame::startTrainingDirectBootToCharSelect(cfg.vsRow)) {
                QLOG("[init] training direct-boot arm FAILED");
                signalStartupFailure();
                return 0;
            }
        } else if (unattended) {
            qoh99::setUnattendedHarnessLaunch(true);
            qgame::startDirectBoot(cfg.vsRow);
        } else {
            qoh99::enableOfflineVsScoreboard();
            qgame::startDirectBootToCharSelect(cfg.vsRow);
        }
    }

    if (haveIpc && cfg.probeMode != 0) {
        QLOG("[init] starting probe mode=%u", cfg.probeMode);
        qoh99::probeInit(static_cast<qoh99::ProbeMode>(cfg.probeMode),
                         cfg.sessionDir[0] ? cfg.sessionDir : nullptr);
    }

    if (mode == RunMode::Off) {
        QLOG("[init] inert - launch via qoh99caster, or set QOH99_NP_MODE=observe|selftest");
        signalReady();
        return 0;
    }
    const bool hooksInstalled = installHooks(mode);
    if (!hooksInstalled)
        QLOG("[init] hook installation FAILED for mode=%s", modeName(mode));
    if (!hooksInstalled && haveIpc && cfg.trainingEnabled != 0) {
        QLOG("[init] explicit training requires its dispatcher/input/config "
             "hook set; refusing a vanilla-looking partial launch");
        signalStartupFailure();
        return 0;
    }
    signalReady();
    return 0;
}

/* All initialization runs outside the loader lock; DllMain only stores the module handle, disables thread callbacks and starts the fallback watchdog. */
static HMODULE g_selfModule = nullptr;
static volatile LONG g_initializeStarted = 0;
constexpr DWORD kInitializeFallbackMs = 400;

extern "C" __declspec(dllexport) int qoh99np_initialize(void) {
    // The export and the fallback watchdog race; the loser must not start a second runtime.
    if (InterlockedCompareExchange(&g_initializeStarted, 1, 0) != 0)
        return 1;
    qpath::ensureDir(qpath::logsRoot());
    std::string early = qpath::logsRoot() + "\\dll_early.log";
    Log::get().open(early.c_str());
    QLOG("[init] qoh99np_initialize() entered outside the loader lock "
         "(module=%p)", static_cast<void*>(g_selfModule));
    HANDLE init = CreateThread(nullptr, 0, initThread, nullptr, 0, nullptr);
    if (init == nullptr) {
        QLOG("[init] CreateThread FAILED (error=%lu)", GetLastError());
        signalStartupFailure();
        return 0;
    }
    CloseHandle(init);
    return 1;
}

// Creating a thread under the loader lock is safe but doing work there is not, so this thread only waits and delegates.
static DWORD WINAPI initFallbackThread(LPVOID) {
    const DWORD deadline = GetTickCount() + kInitializeFallbackMs;
    while (InterlockedCompareExchange(&g_initializeStarted, 0, 0) == 0
           && static_cast<LONG>(GetTickCount() - deadline) < 0) {
        Sleep(5);
    }
    if (InterlockedCompareExchange(&g_initializeStarted, 0, 0) == 0)
        (void)qoh99np_initialize();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            g_selfModule = hModule;
            DisableThreadLibraryCalls(hModule);
            HANDLE fallback =
                CreateThread(nullptr, 0, initFallbackThread, nullptr, 0,
                             nullptr);
            if (fallback != nullptr) CloseHandle(fallback);
            break;
        }
        case DLL_PROCESS_DETACH:
            // Never wait or tear down active hooks while holding the loader lock.
            Log::get().markProcessExit();
            break;
    }
    return TRUE;
}
