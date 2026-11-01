#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <functional>
#include <string>
#include "../ipc/IpcConfig.h"
#include "BuildIdentity.h"
#include "SessionLogArchive.h"

namespace qclient {

constexpr DWORD kInjectTimeoutMs       = 10000;
constexpr DWORD kPipeConnectTimeoutMs  = 10000;
constexpr DWORD kConfigWriteTimeoutMs  =  5000;
constexpr DWORD kStageAReadyTimeoutMs  = 15000;
constexpr DWORD kReplayStageAReadyTimeoutMs =
    qipc::kReplayPrebufferDeadlineMs + 10000u;
constexpr DWORD kStageBInputTimeoutMs  = 30000;
constexpr DWORD kColdPrefixTimeoutScale = 3;
constexpr DWORD kMaxPhaseTimeoutMs      = 180000;
constexpr DWORD kPhaseHeartbeatMs       = 2000;

constexpr DWORD scaleLaunchTimeoutMs(DWORD baseMs, bool wine) {
    if (!wine || baseMs == 0 || baseMs == INFINITE) return baseMs;
    const unsigned long long scaled =
        static_cast<unsigned long long>(baseMs) * kColdPrefixTimeoutScale;
    return scaled > kMaxPhaseTimeoutMs
        ? kMaxPhaseTimeoutMs
        : static_cast<DWORD>(scaled);
}

DWORD launchPhaseTimeoutMs(DWORD baseMs);

bool preflightInjectedLaunch(const std::string& gameExePath,
                             const std::string& dllPath,
                             std::string& error,
                             RuntimeInstallationIdentity* installation = nullptr);

bool preflightLaunchArtifacts(const std::string& gameExePath,
                              const std::string& dllPath,
                              std::string& error);

struct LaunchResult {
    bool ok = false;
    DWORD pid = 0;
    HANDLE process = nullptr;
    HDESK  desktop = nullptr;
    std::string gameDirectory;
    unsigned long long launchedAtFileTime = 0;
    std::string sessionDirectory;
    SessionLogWriterLease sessionLogWriterLease{};
    std::string error;
};

struct PreparedLaunch {
    bool ok = false;
    DWORD pid = 0;
    HANDLE process = nullptr;
    HANDLE mainThread = nullptr;
    HDESK desktop = nullptr;
    std::string gameDirectory;
    unsigned long long launchedAtFileTime = 0;
    std::string sessionDirectory;
    HANDLE inputReadyEvent = nullptr;
    HANDLE inputFailedEvent = nullptr;
    bool inputReadyFatal = false;
    SessionLogWriterLease sessionLogWriterLease{};
    std::string error;

    PreparedLaunch() = default;
    ~PreparedLaunch();
    PreparedLaunch(const PreparedLaunch&) = delete;
    PreparedLaunch& operator=(const PreparedLaunch&) = delete;
    PreparedLaunch(PreparedLaunch&& other) noexcept;
    PreparedLaunch& operator=(PreparedLaunch&& other) noexcept;

    void abort();
};

using BeforeInjectionCallback =
    std::function<bool(DWORD pid, std::string& error)>;

PreparedLaunch prepareInjected(const std::string& gameExePath,
                               const std::string& dllPath,
                               const std::string& workingDir,
                               const std::string& cmdArgs,
                               qipc::IpcConfig cfg,
                               SOCKET udpSocket,
                               bool hiddenDesktop = false,
                               SOCKET spectatorSocket = INVALID_SOCKET,
                               const BeforeInjectionCallback&
                                   beforeInjection = {});

LaunchResult resumePrepared(PreparedLaunch&& prepared);

LaunchResult launchInjected(const std::string& gameExePath,
                            const std::string& dllPath,
                            const std::string& workingDir,
                            const std::string& cmdArgs,
                            qipc::IpcConfig cfg,
                            SOCKET udpSocket,
                            bool hiddenDesktop = false,
                            SOCKET spectatorSocket = INVALID_SOCKET);

}
