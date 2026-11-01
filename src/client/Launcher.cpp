#include "Launcher.h"
#include "BuildIdentity.h"
#include "../game/SessionLogHighlights.h"
#include "../ipc/BuildContract.h"
#include "../input/SdlLifecycle.h"
#include "../platform/CompatEnv.h"
#include "../platform/PeImage.h"
#include "../platform/WidePath.h"
#include "../platform/WineSocketRepair.h"
#include "../Log.h"
#include <cstdio>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace qclient {

static std::string lastErr(const char* what) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s (GetLastError=%lu)", what, GetLastError());
    return buf;
}

static HANDLE gameLifetimeJob() {
    static HANDLE job = [] () -> HANDLE {
        HANDLE h = CreateJobObjectW(nullptr, nullptr);
        if (h == nullptr) {
            QLOG("[launcher] CreateJobObject failed error=%lu - the game will "
                 "not be killed with the caster", GetLastError());
            return nullptr;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits))) {
            QLOG("[launcher] SetInformationJobObject failed error=%lu - the "
                 "game will not be killed with the caster", GetLastError());
            CloseHandle(h);
            return nullptr;
        }
        return h;
    }();
    return job;
}

static void bindGameToCasterLifetime(HANDLE process, DWORD pid) {
    HANDLE job = gameLifetimeJob();
    if (job == nullptr) return;
    if (AssignProcessToJobObject(job, process)) {
        QLOG("[launcher] pid=%lu bound to the caster lifetime job", pid);
        return;
    }
    QLOG("[launcher] AssignProcessToJobObject failed for pid=%lu error=%lu - "
         "the game will not be killed with the caster", pid, GetLastError());
}

static std::string winErr(const char* what, DWORD error) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s (GetLastError=%lu)", what, error);
    return buf;
}

static std::string logPath(const std::wstring& path) {
    return qplat::narrow(path);
}

DWORD launchPhaseTimeoutMs(DWORD baseMs) {
    return scaleLaunchTimeoutMs(baseMs, qplat::detectCompat().wine);
}

struct PhaseWait {
    DWORD result = WAIT_TIMEOUT;
    DWORD elapsedMs = 0;
    DWORD lastError = ERROR_SUCCESS;
};

static PhaseWait waitPhase(const char* phase, DWORD count,
                           const HANDLE* handles, DWORD timeoutMs) {
    PhaseWait out;
    const DWORD started = GetTickCount();
    DWORD nextHeartbeat = kPhaseHeartbeatMs;
    for (;;) {
        const DWORD elapsed = GetTickCount() - started;
        if (elapsed >= timeoutMs) {
            out.result = WAIT_TIMEOUT;
            out.elapsedMs = elapsed;
            return out;
        }
        const DWORD remaining = timeoutMs - elapsed;
        const DWORD slice =
            remaining < kPhaseHeartbeatMs ? remaining : kPhaseHeartbeatMs;
        const DWORD waited =
            WaitForMultipleObjects(count, handles, FALSE, slice);
        if (waited == WAIT_TIMEOUT) {
            const DWORD now = GetTickCount() - started;
            if (now >= nextHeartbeat) {
                QLOG("[launcher] phase '%s' still waiting after %lu ms "
                     "(deadline %lu ms)", phase, now, timeoutMs);
                nextHeartbeat = now + kPhaseHeartbeatMs;
            }
            continue;
        }
        out.result = waited;
        out.lastError = (waited == WAIT_FAILED) ? GetLastError() : ERROR_SUCCESS;
        out.elapsedMs = GetTickCount() - started;
        return out;
    }
}

static std::string phaseTimeoutError(const char* phase, DWORD elapsedMs,
                                     DWORD deadlineMs) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "launch phase '%s' timed out after %lu ms (deadline %lu ms)",
                  phase, elapsedMs, deadlineMs);
    return buf;
}

static bool boundIpv4DatagramPort(SOCKET socket, uint16_t& port,
                                  std::string& error) {
    port = 0;
    int type = 0;
    int typeSize = sizeof(type);
    if (getsockopt(socket, SOL_SOCKET, SO_TYPE,
                   reinterpret_cast<char*>(&type), &typeSize) != 0) {
        error = "getsockopt(SO_TYPE) failed (err="
              + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    if (type != SOCK_DGRAM) {
        error = "socket is not UDP";
        return false;
    }
    sockaddr_in local{};
    int localSize = sizeof(local);
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&local),
                    &localSize) != 0) {
        error = "getsockname failed (err="
              + std::to_string(WSAGetLastError()) + ")";
        return false;
    }
    if (local.sin_family != AF_INET || localSize < sizeof(sockaddr_in)) {
        error = "socket is not bound as IPv4 UDP";
        return false;
    }
    port = ntohs(local.sin_port);
    if (port == 0) {
        error = "socket has no bound UDP port";
        return false;
    }
    return true;
}

static bool connectPipeWithTimeout(HANDLE pipe, DWORD timeoutMs, std::string& err) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        err = lastErr("CreateEventW(pipe connect)");
        return false;
    }

    BOOL connected = ConnectNamedPipe(pipe, &ov);
    if (!connected) {
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        } else if (error == ERROR_IO_PENDING) {
            const PhaseWait wait =
                waitPhase("pipe connect", 1, &ov.hEvent, timeoutMs);
            if (wait.result == WAIT_OBJECT_0) {
                DWORD ignored = 0;
                connected = GetOverlappedResult(pipe, &ov, &ignored, FALSE);
                if (!connected)
                    err = lastErr("GetOverlappedResult(pipe connect)");
            } else {
                // The OVERLAPPED lives on this stack: drain the cancellation synchronously before returning.
                CancelIoEx(pipe, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &ov, &ignored, TRUE);
                err = (wait.result == WAIT_TIMEOUT)
                    ? phaseTimeoutError("pipe connect", wait.elapsedMs,
                                        timeoutMs)
                    : winErr("WaitForSingleObject(pipe connect)",
                             wait.lastError);
            }
        } else {
            err = winErr("ConnectNamedPipe", error);
        }
    }

    CloseHandle(ov.hEvent);
    return connected == TRUE;
}

static bool writePipeWithTimeout(HANDLE pipe, const void* data, DWORD size,
                                 DWORD timeoutMs, DWORD& written, std::string& err) {
    written = 0;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        err = lastErr("CreateEventW(pipe write)");
        return false;
    }

    BOOL ok = WriteFile(pipe, data, size, &written, &ov);
    if (!ok) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            const PhaseWait wait =
                waitPhase("config write", 1, &ov.hEvent, timeoutMs);
            if (wait.result == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(pipe, &ov, &written, FALSE);
                if (!ok)
                    err = lastErr("GetOverlappedResult(pipe write)");
            } else {
                CancelIoEx(pipe, &ov);
                DWORD ignored = 0;
                GetOverlappedResult(pipe, &ov, &ignored, TRUE);
                err = (wait.result == WAIT_TIMEOUT)
                    ? phaseTimeoutError("config write", wait.elapsedMs,
                                        timeoutMs)
                    : winErr("WaitForSingleObject(pipe write)",
                             wait.lastError);
            }
        } else {
            err = winErr("WriteFile(pipe)", error);
        }
    }

    CloseHandle(ov.hEvent);
    return ok == TRUE && written == size;
}

static std::wstring moduleFileNameW(HMODULE module) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD copied = GetModuleFileNameW(
            module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) return std::wstring();
        if (copied < buffer.size() - 1
            && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return std::wstring(buffer.data(), copied);
        if (buffer.size() >= 64 * 1024) return std::wstring();
        buffer.resize(buffer.size() * 2);
    }
}

struct LoadLibrarySite {
    std::wstring moduleBaseName;
    std::wstring modulePath;
    uintptr_t moduleBase = 0;
    uintptr_t rva = 0;
    qplat::PeImageInfo image{};
};

static bool resolveLoadLibrarySite(LoadLibrarySite& site, std::string& err) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        err = lastErr("GetModuleHandleW(kernel32.dll)");
        return false;
    }
    FARPROC target = GetProcAddress(kernel32, "LoadLibraryW");
    if (!target) {
        err = lastErr("GetProcAddress(LoadLibraryW)");
        return false;
    }
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(target), &owner)
        || owner == nullptr) {
        err = lastErr("GetModuleHandleExW(LoadLibraryW owner)");
        return false;
    }
    site.modulePath = moduleFileNameW(owner);
    if (site.modulePath.empty()) {
        err = lastErr("GetModuleFileNameW(LoadLibraryW owner)");
        return false;
    }
    site.moduleBaseName = qplat::baseNameW(site.modulePath);
    site.moduleBase = reinterpret_cast<uintptr_t>(owner);
    const uintptr_t address = reinterpret_cast<uintptr_t>(target);
    if (address < site.moduleBase) {
        err = "LoadLibraryW resolved below its own module base";
        return false;
    }
    site.rva = address - site.moduleBase;
    if (!qplat::inspectLoadedImage(owner, site.image, err)) {
        err = "local " + qplat::narrow(site.moduleBaseName)
            + " is not a readable PE image: " + err;
        return false;
    }
    if (site.image.sizeOfImage == 0 || site.rva >= site.image.sizeOfImage) {
        err = "LoadLibraryW lies outside its own module image";
        return false;
    }
    return true;
}

static bool injectDll(HANDLE hProc, const std::wstring& dllPathW,
                      std::string& err) {
    const DWORD deadlineMs = launchPhaseTimeoutMs(kInjectTimeoutMs);
    const DWORD startedMs = GetTickCount();

    const std::wstring absoluteW = qplat::absolutePathW(dllPathW);
    const std::wstring& payload = absoluteW.empty() ? dllPathW : absoluteW;
    const SIZE_T payloadBytes = (payload.size() + 1) * sizeof(wchar_t);

    LoadLibrarySite site;
    if (!resolveLoadLibrarySite(site, err)) {
        QLOG("[launcher][inject] phase=resolve-local FAILED: %s", err.c_str());
        return false;
    }
    QLOG("[launcher][inject] phase=resolve-local owner='%s' base=%p rva=%08X "
         "machine=%04X image=%u path='%s'",
         logPath(site.moduleBaseName).c_str(),
         reinterpret_cast<void*>(site.moduleBase),
         static_cast<unsigned>(site.rva), site.image.machine,
         site.image.sizeOfImage, logPath(site.modulePath).c_str());

    uintptr_t remoteBase = 0;
    std::string moduleErr;
    bool usedCandidate = false;
    bool verifiedBeforeStart = qplat::remoteModuleBaseOrValidatedCandidate(
        hProc, site.moduleBaseName.c_str(), site.moduleBase,
        site.image.sizeOfImage, "LoadLibraryW", remoteBase, usedCandidate,
        moduleErr);
    const bool located = verifiedBeforeStart || remoteBase != 0;
    if (!located) {
        err = "could not locate " + qplat::narrow(site.moduleBaseName)
            + " in the game process: " + moduleErr;
    } else if (verifiedBeforeStart) {
        QLOG("[launcher][inject] phase=remote-module base=%p source=%s "
             "verified=pre-start",
             reinterpret_cast<void*>(remoteBase),
             usedCandidate ? "validated candidate" : "target module list");
    } else {
        QLOG("[launcher][inject] phase=remote-module base=%p source=candidate "
             "verified=deferred (%s)",
             reinterpret_cast<void*>(remoteBase), moduleErr.c_str());
    }
    if (!located) {
        QLOG("[launcher][inject] phase=remote-module FAILED: %s", err.c_str());
        return false;
    }

    qplat::PeImageInfo remoteImage{};
    if (verifiedBeforeStart) {
        std::string validateErr;
        if (!qplat::remoteValidateModule(hProc, remoteBase, remoteImage,
                                         validateErr)) {
            err = "the game's " + qplat::narrow(site.moduleBaseName)
                + " failed PE validation: " + validateErr;
            QLOG("[launcher][inject] phase=validate-remote FAILED: %s",
                 err.c_str());
            return false;
        }
        if (remoteImage.machine != site.image.machine
            || remoteImage.sizeOfImage != site.image.sizeOfImage) {
            char detail[256];
            std::snprintf(detail, sizeof(detail),
                          "the game's %s is a different image than this "
                          "process's (machine %04X/%04X size %u/%u)",
                          qplat::narrow(site.moduleBaseName).c_str(),
                          site.image.machine, remoteImage.machine,
                          site.image.sizeOfImage, remoteImage.sizeOfImage);
            err = detail;
            QLOG("[launcher][inject] phase=validate-remote FAILED: %s",
                 err.c_str());
            return false;
        }
        if (remoteImage.timestamp != site.image.timestamp) {
            QLOG("[launcher][inject] phase=validate-remote WARNING: %s COFF "
                 "timestamp differs (local %08X, game %08X) - continuing on "
                 "the machine/size identity match",
                 qplat::narrow(site.moduleBaseName).c_str(),
                 site.image.timestamp, remoteImage.timestamp);
        }
        if (site.rva >= remoteImage.sizeOfImage) {
            err = "the LoadLibraryW RVA is outside the game's module image";
            QLOG("[launcher][inject] phase=validate-remote FAILED: %s",
                 err.c_str());
            return false;
        }
    } else {
        QLOG("[launcher][inject] phase=validate-remote deferred: the target's "
             "loader has not mapped %s yet; identity is confirmed after the "
             "remote thread runs",
             qplat::narrow(site.moduleBaseName).c_str());
    }
    const uintptr_t remoteTarget = remoteBase + site.rva;
    QLOG("[launcher][inject] phase=validate-remote ok base=%p rva=%08X "
         "target=%p size=%u stamp=%08X",
         reinterpret_cast<void*>(remoteBase), static_cast<unsigned>(site.rva),
         reinterpret_cast<void*>(remoteTarget), remoteImage.sizeOfImage,
         remoteImage.timestamp);

    void* remote = VirtualAllocEx(hProc, nullptr, payloadBytes,
                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        err = lastErr("VirtualAllocEx(DLL path)");
        QLOG("[launcher][inject] phase=alloc FAILED: %s", err.c_str());
        return false;
    }
    if (!WriteProcessMemory(hProc, remote, payload.c_str(), payloadBytes,
                            nullptr)) {
        err = lastErr("WriteProcessMemory(DLL path)");
        QLOG("[launcher][inject] phase=write FAILED: %s", err.c_str());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE th = CreateRemoteThread(
        hProc, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteTarget), remote, 0,
        nullptr);
    if (!th) {
        err = lastErr("CreateRemoteThread(LoadLibraryW)");
        QLOG("[launcher][inject] phase=create-thread FAILED: %s", err.c_str());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return false;
    }

    const PhaseWait wait = waitPhase("injection", 1, &th, deadlineMs);
    if (wait.result != WAIT_OBJECT_0) {
        bool stillRunning = wait.result == WAIT_TIMEOUT;
        if (stillRunning) {
            DWORD code = 0;
            if (GetExitCodeThread(th, &code) && code != STILL_ACTIVE)
                stillRunning = false;
        }
        if (stillRunning) {
            // Do not free: the remote LoadLibraryW may still be reading this page.
            QLOG("[launcher][inject] phase=wait timed out with the remote "
                 "thread STILL RUNNING - leaving the %llu-byte argument page "
                 "mapped in the child on purpose (the process is terminated "
                 "next)",
                 static_cast<unsigned long long>(payloadBytes));
        } else {
            VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        }
        err = (wait.result == WAIT_TIMEOUT)
            ? phaseTimeoutError("injection", wait.elapsedMs, deadlineMs)
            : winErr("WaitForSingleObject(remote LoadLibraryW)",
                     wait.lastError);
        QLOG("[launcher][inject] phase=wait FAILED after %lu ms: %s",
             wait.elapsedMs, err.c_str());
        CloseHandle(th);
        return false;
    }

    DWORD exitCode = 0;
    const bool haveExit = GetExitCodeThread(th, &exitCode) != FALSE;
    const DWORD exitError = haveExit ? ERROR_SUCCESS : GetLastError();
    CloseHandle(th);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    if (!haveExit) {
        err = winErr("GetExitCodeThread(remote LoadLibraryW)", exitError);
        QLOG("[launcher][inject] phase=exit-code FAILED: %s", err.c_str());
        return false;
    }
    if (exitCode == 0 || exitCode == STILL_ACTIVE) {
        err = exitCode == 0
            ? "remote LoadLibraryW returned NULL (the DLL failed to load in "
              "the game process)"
            : "remote LoadLibraryW did not complete";
        QLOG("[launcher][inject] phase=exit-code FAILED: %s (exit=%08lX)",
             err.c_str(), exitCode);
        return false;
    }
    if (!verifiedBeforeStart) {
        uintptr_t confirmedBase = 0;
        std::string confirmErr;
        if (!qplat::remoteModuleBase(hProc, site.moduleBaseName.c_str(),
                                     confirmedBase, confirmErr)) {
            err = "the game's " + qplat::narrow(site.moduleBaseName)
                + " could not be confirmed after injection: " + confirmErr;
            QLOG("[launcher][inject] phase=confirm-remote FAILED: %s",
                 err.c_str());
            return false;
        }
        if (confirmedBase != remoteBase) {
            char detail[224];
            std::snprintf(detail, sizeof(detail),
                          "the game's %s is based at %p, not the %p this "
                          "launcher called - the injected entry point was not "
                          "the resolved function",
                          qplat::narrow(site.moduleBaseName).c_str(),
                          reinterpret_cast<void*>(confirmedBase),
                          reinterpret_cast<void*>(remoteBase));
            err = detail;
            QLOG("[launcher][inject] phase=confirm-remote FAILED: %s",
                 err.c_str());
            return false;
        }
        qplat::PeImageInfo confirmedImage{};
        std::string validateErr;
        if (!qplat::remoteValidateModule(hProc, confirmedBase, confirmedImage,
                                         validateErr)
            || confirmedImage.machine != site.image.machine
            || confirmedImage.sizeOfImage != site.image.sizeOfImage) {
            err = "the game's " + qplat::narrow(site.moduleBaseName)
                + " failed post-injection PE identity validation: "
                + (validateErr.empty() ? std::string("machine/size mismatch")
                                       : validateErr);
            QLOG("[launcher][inject] phase=confirm-remote FAILED: %s",
                 err.c_str());
            return false;
        }
        QLOG("[launcher][inject] phase=confirm-remote ok base=%p machine=%04X "
             "size=%u (identity proven after loader init)",
             reinterpret_cast<void*>(confirmedBase), confirmedImage.machine,
             confirmedImage.sizeOfImage);
    }
    QLOG("[launcher][inject] phase=done module=%08lX elapsed=%lu ms "
         "(deadline %lu ms) dll='%s'",
         exitCode, GetTickCount() - startedMs, deadlineMs,
         logPath(payload).c_str());
    return true;
}

bool preflightInjectedLaunch(const std::string& gameExePath,
                              const std::string& dllPath,
                              std::string& error,
                              RuntimeInstallationIdentity* installationOut) {
    RuntimeInstallationIdentity installation{};
    if (!validateRuntimeInstallationForAdmission(gameExePath, &installation,
                                                 error)) {
        QLOG("[launcher] executable/content admission gate FAILED: %s",
             error.c_str());
        return false;
    }
    if (installationOut != nullptr) *installationOut = installation;
    if (!installation.executable.variantKey.empty()) {
        QLOG("[launcher] executable is the attested variant '%s': physical "
             "sha256=%s canonicalised to %s. The 26-byte Tir_Mes1.Img gate is "
             "the only difference and the wrapper NOPs it at startup, so the "
             "running image matches the canonical build exactly.",
             installation.executable.variantKey.c_str(),
             installation.executable.physicalSha256.c_str(),
             installation.executable.sha256.c_str());
    }
    QLOG("[launcher] executable/content admission accepted: sha256=%s size=%llu coff=%08X simRoot=%s validationRoot=%s",
         installation.executable.sha256.c_str(),
         static_cast<unsigned long long>(installation.executable.fileSize),
         installation.executable.coffTimestamp,
         qoh99::content::digestHex(
             installation.content.identity.simulationRootSha256).c_str(),
         qoh99::content::digestHex(
             installation.content.identity.validationRootSha256).c_str());

    return preflightLaunchArtifacts(gameExePath, dllPath, error);
}

bool preflightLaunchArtifacts(const std::string& gameExePath,
                              const std::string& dllPath,
                              std::string& error) {
    if (!validateNetplayDllContract(dllPath, error)) {
        QLOG("[launcher] DLL build-contract gate FAILED: %s",
             error.c_str());
        return false;
    }
    QLOG("[launcher] DLL build-contract accepted: ipc=%u size=%zu ctrl=%u",
         qipc::kIpcVersion, sizeof(qipc::IpcConfig),
         qnet::kProtocolVersion);

    const std::wstring gameW = qplat::absolutePathW(qplat::widen(gameExePath));
    const std::wstring gameDirW =
        gameW.empty() ? std::wstring() : qplat::dirOfW(gameW);
    const std::wstring wrapperW =
        gameDirW.empty() ? std::wstring() : gameDirW + L"\\ddraw.dll";
    if (wrapperW.empty()) {
        error = "could not resolve the game directory for the ddraw wrapper";
        QLOG("[launcher] ddraw wrapper gate FAILED: %s", error.c_str());
        return false;
    }
    const std::string wrapperPath = qplat::narrow(wrapperW);
    if (!qplat::fileExistsW(wrapperW)) {
        error = "packaged ddraw.dll is missing the required pacing ABI (no "
                "ddraw.dll beside the game at " + wrapperPath + ")";
        QLOG("[launcher] ddraw wrapper gate FAILED: %s", error.c_str());
        return false;
    }
    if (!validatePackagedDdrawWrapper(wrapperPath, error)) {
        QLOG("[launcher] ddraw wrapper gate FAILED: %s (path='%s')",
             error.c_str(), wrapperPath.c_str());
        return false;
    }
    QLOG("[launcher] packaged ddraw wrapper accepted: '%s' "
         "(PE32/i386, exports EFZDDRAW_GetNetplayPacingAbi + "
         "EFZDDRAW_SetFrameLimit)", wrapperPath.c_str());

    error.clear();
    return true;
}

PreparedLaunch::~PreparedLaunch() {
    abort();
}

PreparedLaunch::PreparedLaunch(PreparedLaunch&& other) noexcept
    : ok(other.ok),
      pid(other.pid),
      process(other.process),
      mainThread(other.mainThread),
      desktop(other.desktop),
      inputReadyEvent(other.inputReadyEvent),
      inputFailedEvent(other.inputFailedEvent),
      inputReadyFatal(other.inputReadyFatal),
      sessionLogWriterLease(std::move(other.sessionLogWriterLease)),
      error(std::move(other.error)) {
    other.ok = false;
    other.pid = 0;
    other.process = nullptr;
    other.mainThread = nullptr;
    other.desktop = nullptr;
    other.inputReadyEvent = nullptr;
    other.inputFailedEvent = nullptr;
    other.sessionLogWriterLease = {};
}

PreparedLaunch& PreparedLaunch::operator=(PreparedLaunch&& other) noexcept {
    if (this == &other) return *this;
    abort();
    ok = other.ok;
    pid = other.pid;
    process = other.process;
    mainThread = other.mainThread;
    desktop = other.desktop;
    inputReadyEvent = other.inputReadyEvent;
    inputFailedEvent = other.inputFailedEvent;
    inputReadyFatal = other.inputReadyFatal;
    sessionLogWriterLease = std::move(other.sessionLogWriterLease);
    error = std::move(other.error);
    other.ok = false;
    other.pid = 0;
    other.process = nullptr;
    other.mainThread = nullptr;
    other.desktop = nullptr;
    other.inputReadyEvent = nullptr;
    other.inputFailedEvent = nullptr;
    other.sessionLogWriterLease = {};
    return *this;
}

void PreparedLaunch::abort() {
    bool processExited = false;
    if (process) {
        TerminateProcess(process, 1);
        processExited = WaitForSingleObject(process, 3000) == WAIT_OBJECT_0;
    }
    if (mainThread) {
        CloseHandle(mainThread);
        mainThread = nullptr;
    }
    if (process) {
        CloseHandle(process);
        process = nullptr;
    }
    if (desktop) {
        CloseDesktop(desktop);
        desktop = nullptr;
    }
    if (inputReadyEvent) {
        CloseHandle(inputReadyEvent);
        inputReadyEvent = nullptr;
    }
    if (inputFailedEvent) {
        CloseHandle(inputFailedEvent);
        inputFailedEvent = nullptr;
    }
    if (processExited)
        releaseSessionLogWriterLease(sessionLogWriterLease);
    ok = false;
}

PreparedLaunch prepareInjected(const std::string& gameExePath,
                               const std::string& dllPath,
                               const std::string& workingDir,
                               const std::string& cmdArgs,
                               qipc::IpcConfig cfg,
                               SOCKET udpSocket,
                               bool hiddenDesktop,
                               SOCKET spectatorSocket,
                               const BeforeInjectionCallback&
                                   beforeInjection) {
    PreparedLaunch res;

    const bool servesSpectators = cfg.serveSpectators == 1;
    const bool inProcessSpectatorSource = servesSpectators
        && cfg.spectatorServiceMode == qipc::SPECTATOR_SERVICE_INPROCESS;
    const bool needsPlayerSocket =
        cfg.mode == qipc::MODE_NETPLAY || cfg.mode == qipc::MODE_SPECTATOR;
    if (cfg.serveSpectators > 1) {
        res.error = "invalid serveSpectators flag";
        return res;
    }
    if (const char* sessionDirWhy = qipc::validateSessionDir(cfg)) {
        char detail[192];
        std::snprintf(detail, sizeof(detail),
                      "the session directory cannot be handed to the game: %s "
                      "(%u bytes, limit %u)",
                      sessionDirWhy,
                      static_cast<unsigned>(cfg.sessionDirBytes),
                      static_cast<unsigned>(qipc::kSessionDirBytes) - 1u);
        res.error = detail;
        QLOG("[launcher] %s", res.error.c_str());
        return res;
    }
    if (const char* replayWhy = qipc::validateReplayLaunch(cfg)) {
        res.error = std::string("invalid replay launch: ") + replayWhy;
        QLOG("[launcher] %s", res.error.c_str());
        return res;
    }
    if (const char* recorderWhy = qipc::validateRecorderLaunch(cfg)) {
        res.error = std::string("invalid recorder launch: ") + recorderWhy;
        QLOG("[launcher] %s", res.error.c_str());
        return res;
    }
    if (const char* spectatorWhy = qipc::validateSpectatorService(cfg)) {
        res.error = std::string("invalid spectator service configuration: ")
                  + spectatorWhy;
        QLOG("[launcher] %s", res.error.c_str());
        return res;
    }
    if (servesSpectators
        && (cfg.mode != qipc::MODE_NETPLAY || cfg.isHost != 1)) {
        res.error = "a spectator server is valid only for the authoritative netplay host";
        return res;
    }
    if (needsPlayerSocket && udpSocket == INVALID_SOCKET) {
        res.error = "netplay/spectator launch requires a bound player UDP socket";
        return res;
    }
    if (inProcessSpectatorSource && spectatorSocket == INVALID_SOCKET) {
        res.error = "spectator serving requested without a dedicated bound UDP socket";
        return res;
    }
    if (!inProcessSpectatorSource && spectatorSocket != INVALID_SOCKET) {
        res.error = "a dedicated spectator socket is valid only for an in-process spectator source";
        return res;
    }
    if (inProcessSpectatorSource && spectatorSocket == udpSocket) {
        res.error = "player and spectator traffic require distinct UDP sockets";
        return res;
    }
    if (needsPlayerSocket) {
        uint16_t playerPort = 0;
        std::string socketError;
        if (!boundIpv4DatagramPort(udpSocket, playerPort, socketError)) {
            res.error = "invalid player UDP socket: " + socketError;
            return res;
        }
        if (inProcessSpectatorSource) {
            uint16_t spectatorPort = 0;
            if (!boundIpv4DatagramPort(spectatorSocket, spectatorPort,
                                       socketError)) {
                res.error = "invalid dedicated spectator UDP socket: "
                          + socketError;
                return res;
            }
            if (spectatorPort == playerPort) {
                res.error = "player and spectator UDP sockets are bound to "
                            "the same port";
                return res;
            }
        }
    }

    RuntimeInstallationIdentity launchInstallation{};
    if (!preflightInjectedLaunch(gameExePath, dllPath, res.error,
                                 &launchInstallation))
        return res;
    if (needsPlayerSocket) {
        if (!qnetplay::compat::isValidCompatibilityIdentity(
                cfg.compatibilityIdentity)) {
            res.error = "netplay/spectator launch has no complete simulation "
                        "compatibility identity";
            return res;
        }
        if (!qnetplay::compat::semanticallyEqual(
                cfg.compatibilityIdentity, launchInstallation.compatibility)) {
            const qnetplay::compat::IdentityMismatch drift =
                qnetplay::compat::compatibilityMismatch(
                    cfg.compatibilityIdentity,
                    launchInstallation.compatibility);
            res.error = std::string(
                "your game folder changed while the match was being set up (")
                + qnetplay::compat::identityMismatchReason(drift)
                + "). Close anything editing the game files and try again.";
            return res;
        }
    }

    qinput::sdlReleaseForLaunch();

    const std::wstring gameExeW = qplat::absolutePathW(qplat::widen(gameExePath));
    res.gameDirectory = qplat::narrow(qplat::dirOfW(gameExeW));
    {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        res.launchedAtFileTime =
            (static_cast<unsigned long long>(now.dwHighDateTime) << 32)
            | now.dwLowDateTime;
    }
    if (gameExeW.empty()) {
        res.error = "the game path is not a representable absolute path: "
                  + gameExePath;
        QLOG("[launcher] %s", res.error.c_str());
        return res;
    }
    const std::wstring dllPathW = qplat::absolutePathW(qplat::widen(dllPath));
    if (dllPathW.empty()) {
        res.error = "the netplay DLL path is not a representable absolute "
                    "path: " + dllPath;
        QLOG("[launcher] %s", res.error.c_str());
        return res;
    }
    std::wstring workingDirW = workingDir.empty()
        ? qplat::dirOfW(gameExeW)
        : qplat::absolutePathW(qplat::widen(workingDir));
    if (!workingDir.empty() && workingDirW.empty()) {
        res.error = "the working directory is not a representable path: "
                  + workingDir;
        QLOG("[launcher] %s", res.error.c_str());
        return res;
    }

    std::wstring cmdlineW = L"\"" + gameExeW + L"\"";
    if (!cmdArgs.empty()) {
        const std::wstring argsW = qplat::widen(cmdArgs);
        if (argsW.empty()) {
            res.error = "the command-line arguments are not valid UTF-8";
            QLOG("[launcher] %s", res.error.c_str());
            return res;
        }
        cmdlineW += L" " + argsW;
    }
    std::vector<wchar_t> mutableCmd(cmdlineW.begin(), cmdlineW.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    HDESK hDesk = nullptr;
    std::wstring deskName = L"qoh99np_hidden";
    if (hiddenDesktop) {
        hDesk = CreateDesktopW(deskName.c_str(), nullptr, nullptr, 0,
                               GENERIC_ALL, nullptr);
        if (hDesk) {
            si.lpDesktop = &deskName[0];
            QLOG("[launcher] launching on hidden desktop '%s' (game will NOT appear on screen)",
                 logPath(deskName).c_str());
        } else {
            res.error = lastErr("CreateDesktopW(qoh99np_hidden)");
            QLOG("[launcher] CreateDesktopW('%s') FAILED (%lu) - refusing visible fallback for a hidden-desktop launch",
                 logPath(deskName).c_str(), GetLastError());
            return res;
        }
    }

    const qplat::CompatInfo& compat = qplat::detectCompat();
    DWORD creationFlags = CREATE_SUSPENDED;
    void* environmentBlock = nullptr;
    std::vector<std::wstring> environmentEntries;
    std::vector<wchar_t> environmentStorage;
    std::wstring effectiveOverride;
    {
        std::string envFailure;
        if (!qplat::readParentEnvironmentW(environmentEntries))
            envFailure = "could not read this process's environment block";
        else if (!qplat::mergeDdrawOverride(environmentEntries))
            envFailure = "could not merge the native ddraw override";
        else {
            const bool freePlay = cfg.mode == qipc::MODE_OBSERVE
                && cfg.directBoot == 0 && cfg.trainingEnabled == 0;
            const std::wstring skipIntro =
                freePlay ? L"QOH99_SKIP_INTRO=0" : L"QOH99_SKIP_INTRO=1";
            bool replaced = false;
            for (std::wstring& entry : environmentEntries) {
                if (entry.size() >= 17
                    && _wcsnicmp(entry.c_str(), L"QOH99_SKIP_INTRO=", 17) == 0) {
                    entry = skipIntro;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) environmentEntries.push_back(skipIntro);
            QLOG("[launcher] opening movie: %s (mode=%u directBoot=%u training=%u)",
                 freePlay ? "played (free play)" : "skipped",
                 (unsigned)cfg.mode, (unsigned)cfg.directBoot,
                 (unsigned)cfg.trainingEnabled);

            const std::wstring sessionKey = L"QOH99_NETPLAY_SESSION_DIR=";
            const std::wstring sessionValue = sessionKey + qplat::widen(
                std::string(cfg.sessionDir, cfg.sessionDirBytes));
            bool sessionReplaced = false;
            for (std::wstring& entry : environmentEntries) {
                if (_wcsnicmp(entry.c_str(), sessionKey.c_str(), sessionKey.size()) == 0) {
                    entry = sessionValue;
                    sessionReplaced = true;
                }
            }
            if (!sessionReplaced) environmentEntries.push_back(sessionValue);

            if (!qplat::buildEnvironmentBlockW(environmentEntries,
                                               environmentStorage))
                envFailure = "could not build the child environment block";
        }

        if (envFailure.empty()) {
            environmentBlock = environmentStorage.data();
            creationFlags |= CREATE_UNICODE_ENVIRONMENT;
            for (const std::wstring& entry : environmentEntries) {
                if (entry.size() > 17
                    && _wcsnicmp(entry.c_str(), L"WINEDLLOVERRIDES=", 17) == 0) {
                    effectiveOverride = entry.substr(17);
                    break;
                }
            }
            QLOG("[launcher] child environment: %zu entries, "
                 "WINEDLLOVERRIDES='%s' (wine=%d proton=%d)",
                 environmentEntries.size(),
                 logPath(effectiveOverride).c_str(),
                 compat.wine ? 1 : 0, compat.proton ? 1 : 0);
        } else if (compat.wine) {
            res.error = "cannot force the native ddraw override: " + envFailure;
            QLOG("[launcher] %s - refusing to launch under Wine without it",
                 res.error.c_str());
            if (hDesk) CloseDesktop(hDesk);
            return res;
        } else {
            QLOG("[launcher] child environment: %s - inheriting the caster "
                 "environment (native Windows: no ddraw override needed)",
                 envFailure.c_str());
        }
    }

    const std::string sessionDirUtf8(
        cfg.sessionDir, static_cast<size_t>(cfg.sessionDirBytes));
    res.sessionDirectory = sessionDirUtf8;
    SessionLogWriterLease pendingSessionWriterLease =
        acquirePendingSessionLogWriterLease(sessionDirUtf8);
    if (pendingSessionWriterLease.markerPath.empty()) {
        QLOG("[launcher] could not create pre-launch session marker; "
             "uncertain sessions will be retained");
    }
    if (!CreateProcessW(gameExeW.c_str(), mutableCmd.data(), nullptr, nullptr,
                        FALSE, creationFlags, environmentBlock,
                        workingDirW.empty() ? nullptr : workingDirW.c_str(),
                        &si, &pi)) {
        res.error = lastErr("CreateProcessW");
        QLOG("[launcher] CreateProcessW FAILED: %s (app='%s' cwd='%s')",
             res.error.c_str(), logPath(gameExeW).c_str(),
             logPath(workingDirW).c_str());
        releaseSessionLogWriterLease(pendingSessionWriterLease);
        if (hDesk) CloseDesktop(hDesk);
        return res;
    }
    bindGameToCasterLifetime(pi.hProcess, pi.dwProcessId);

    res.sessionLogWriterLease = acquireSessionLogWriterLease(
        sessionDirUtf8, pi.dwProcessId);
    if (res.sessionLogWriterLease.markerPath.empty()) {
        QLOG("[launcher] could not create game session-writer marker for "
             "pid=%lu; the unresolved launch marker will retain this folder",
             pi.dwProcessId);
    } else {
        releaseSessionLogWriterLease(pendingSessionWriterLease);
    }
    res.desktop = hDesk;
    QLOG("[launcher] created game suspended pid=%lu app='%s' cmd='%s' "
         "cwd='%s' unicodeEnv=%d%s",
         pi.dwProcessId, logPath(gameExeW).c_str(), logPath(cmdlineW).c_str(),
         logPath(workingDirW).c_str(),
         (creationFlags & CREATE_UNICODE_ENVIRONMENT) ? 1 : 0,
         hDesk ? " [hidden-desktop]" : "");

    if (beforeInjection) {
        std::string callbackError;
        if (!beforeInjection(pi.dwProcessId, callbackError)) {
            res.error = callbackError.empty()
                ? "pre-injection launch preparation failed"
                : callbackError;
            QLOG("[launcher] pre-injection callback FAILED for pid=%lu: %s",
                 pi.dwProcessId, res.error.c_str());
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            if (hDesk) {
                CloseDesktop(hDesk);
                res.desktop = nullptr;
            }
            return res;
        }
        QLOG("[launcher] pre-injection callback ready for pid=%lu",
             pi.dwProcessId);
    }

    const bool isOnlineMode =
        cfg.mode == qipc::MODE_NETPLAY || cfg.mode == qipc::MODE_SPECTATOR;
    const bool needsInputRuntimeReady =
        isOnlineMode || cfg.trainingEnabled != 0u;

    wchar_t evName[64];
    qipc::ipcReadyEventName(evName, 64, pi.dwProcessId);
    HANDLE readyEvent = nullptr;
    HANDLE failedEvent = nullptr;
    {
        readyEvent = CreateEventW(nullptr, TRUE, FALSE, evName);
        if (!readyEvent) {
            res.error = lastErr("CreateEventW(ready)");
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
            return res;
        }
        wchar_t failedName[64];
        qipc::ipcFailedEventName(failedName, 64, pi.dwProcessId);
        failedEvent = CreateEventW(nullptr, TRUE, FALSE, failedName);
        if (!failedEvent) {
            res.error = lastErr("CreateEventW(startup failure)");
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(readyEvent);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
            return res;
        }
    }
    if (needsInputRuntimeReady) {
        res.inputReadyFatal = isOnlineMode;
        wchar_t inputReadyName[64];
        qipc::ipcInputReadyEventName(inputReadyName, 64, pi.dwProcessId);
        res.inputReadyEvent =
            CreateEventW(nullptr, TRUE, FALSE, inputReadyName);
        wchar_t inputFailedName[64];
        qipc::ipcInputFailedEventName(inputFailedName, 64, pi.dwProcessId);
        res.inputFailedEvent =
            CreateEventW(nullptr, TRUE, FALSE, inputFailedName);
        if (!res.inputReadyEvent || !res.inputFailedEvent) {
            res.error = lastErr("CreateEventW(input runtime readiness)");
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(readyEvent);
            CloseHandle(failedEvent);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
            return res;
        }
    }

    wchar_t pipeName[64];
    qipc::ipcPipeName(pipeName, 64, pi.dwProcessId);
    DWORD pipeOpenMode = PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED
                       | FILE_FLAG_FIRST_PIPE_INSTANCE;
    DWORD pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
#ifdef PIPE_REJECT_REMOTE_CLIENTS
    pipeMode |= PIPE_REJECT_REMOTE_CLIENTS;
#endif
    HANDLE pipe = CreateNamedPipeW(
        pipeName, pipeOpenMode,
        pipeMode, 1,
        sizeof(qipc::IpcConfig) + 64, sizeof(qipc::IpcConfig) + 64, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        res.error = lastErr("CreateNamedPipeW");
        TerminateProcess(pi.hProcess, 1);
        if (readyEvent) CloseHandle(readyEvent);
        if (failedEvent) CloseHandle(failedEvent);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
        return res;
    }

    cfg.magic = qipc::kIpcMagic;
    cfg.version = qipc::kIpcVersion;
    cfg.hasSocket = 0;
    cfg.hasSpectatorSocket = 0;
    ZeroMemory(&cfg.protocolInfo, sizeof(cfg.protocolInfo));
    ZeroMemory(&cfg.spectatorProtocolInfo,
               sizeof(cfg.spectatorProtocolInfo));
    if (udpSocket != INVALID_SOCKET) {
        if (WSADuplicateSocketW(udpSocket, pi.dwProcessId, &cfg.protocolInfo) == 0) {
            QLOG("[launcher][wp005] player socket duplicated: %s",
                 qplat::describeProtocolInfo(cfg.protocolInfo).c_str());
            if (qplat::repairDuplicatedUdpProtocolInfo(cfg.protocolInfo)) {
                QLOG("[launcher][wp005] player protocol info REPAIRED -> %s",
                     qplat::describeProtocolInfo(cfg.protocolInfo).c_str());
            }
            cfg.hasSocket = 1;
        } else if (isOnlineMode) {
            res.error = "WSADuplicateSocketW failed (err=" + std::to_string(WSAGetLastError()) + ")";
            QLOG("[launcher] online socket dup FAILED: %s - aborting launch",
                 res.error.c_str());
            CloseHandle(pipe);
            if (readyEvent) CloseHandle(readyEvent);
            if (failedEvent) CloseHandle(failedEvent);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
            return res;
        } else {
            res.error = "WSADuplicateSocketW failed (continuing without socket)";
        }
    }

    if (spectatorSocket != INVALID_SOCKET) {
        if (WSADuplicateSocketW(spectatorSocket, pi.dwProcessId,
                                &cfg.spectatorProtocolInfo) == 0) {
            QLOG("[launcher][wp005] spectator socket duplicated: %s",
                 qplat::describeProtocolInfo(
                     cfg.spectatorProtocolInfo).c_str());
            if (qplat::repairDuplicatedUdpProtocolInfo(
                    cfg.spectatorProtocolInfo)) {
                QLOG("[launcher][wp005] spectator protocol info REPAIRED "
                     "-> %s",
                     qplat::describeProtocolInfo(
                         cfg.spectatorProtocolInfo).c_str());
            }
            cfg.hasSpectatorSocket = 1;
        } else {
            res.error = "WSADuplicateSocketW(spectator) failed (err="
                      + std::to_string(WSAGetLastError()) + ")";
            QLOG("[launcher] dedicated spectator socket dup FAILED: %s "
                 "- aborting launch", res.error.c_str());
            CloseHandle(pipe);
            if (readyEvent) CloseHandle(readyEvent);
            if (failedEvent) CloseHandle(failedEvent);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
            return res;
        }
    }
    if (inProcessSpectatorSource && !cfg.hasSpectatorSocket) {
        res.error = "dedicated spectator UDP socket was not duplicated";
        QLOG("[launcher] %s - aborting launch", res.error.c_str());
        CloseHandle(pipe);
        if (readyEvent) CloseHandle(readyEvent);
        if (failedEvent) CloseHandle(failedEvent);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
        return res;
    }

    QLOG("[launcher] socket dup: player=%u spectator=%u serveSpectators=%u; "
         "injecting DLL '%s'",
         cfg.hasSocket, cfg.hasSpectatorSocket, cfg.serveSpectators,
         logPath(dllPathW).c_str());
    std::string err;
    if (!injectDll(pi.hProcess, dllPathW, err)) {
        res.error = err;
        QLOG("[launcher] injectDll FAILED: %s", err.c_str());
        CloseHandle(pipe);
        if (readyEvent) CloseHandle(readyEvent);
        if (failedEvent) CloseHandle(failedEvent);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
        return res;
    }
    QLOG("[launcher] DLL injected ok; writing config over pipe");

    DWORD written = 0;
    std::string pipeError;
    const DWORD pipeConnectMs = launchPhaseTimeoutMs(kPipeConnectTimeoutMs);
    const DWORD configWriteMs = launchPhaseTimeoutMs(kConfigWriteTimeoutMs);
    BOOL connected =
        connectPipeWithTimeout(pipe, pipeConnectMs, pipeError) ? TRUE : FALSE;
    bool configWritten = connected
        && writePipeWithTimeout(pipe, &cfg, static_cast<DWORD>(sizeof(cfg)),
                                configWriteMs, written, pipeError);
    if (!configWritten)
        res.error = pipeError.empty() ? "IPC config write incomplete" : pipeError;
    QLOG("[launcher] config written (connected=%d, %lu/%zu bytes); "
         "onlineMode=%d",
         connected, written, sizeof(cfg), isOnlineMode);
    if (!configWritten) {
        if (res.error.empty())
            res.error = "IPC config write incomplete (" + std::to_string(written) + " bytes)";
        QLOG("[launcher] config handover FAILED: %s - aborting launch", res.error.c_str());
        CloseHandle(pipe);
        if (readyEvent) CloseHandle(readyEvent);
        if (failedEvent) CloseHandle(failedEvent);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
        return res;
    }

    {
        HANDLE startupEvents[3] = { readyEvent, failedEvent, pi.hProcess };
        const DWORD stageAMs = launchPhaseTimeoutMs(
            cfg.mode == qipc::MODE_REPLAY
                ? kReplayStageAReadyTimeoutMs
                : kStageAReadyTimeoutMs);
        const PhaseWait wait =
            waitPhase("Stage A", 3, startupEvents, stageAMs);
        const DWORD w = wait.result;
        QLOG("[launcher] Stage-A startup-event wait -> %s after %lu ms "
             "(deadline %lu ms)",
             w == WAIT_OBJECT_0 ? "READY" :
             w == WAIT_OBJECT_0 + 1 ? "DLL REJECTED" :
             w == WAIT_OBJECT_0 + 2 ? "PROCESS EXITED" : "FAILED/TIMEOUT",
             wait.elapsedMs, stageAMs);
        if (w != WAIT_OBJECT_0) {
            res.error = (w == WAIT_OBJECT_0 + 1)
                ? "The mod's runtime refused to start inside the game."
                : (w == WAIT_OBJECT_0 + 2)
                    ? "The game process ended before the mod's runtime was ready."
                : (w == WAIT_TIMEOUT)
                    ? phaseTimeoutError("Stage A (DLL HookReady)",
                                        wait.elapsedMs, stageAMs)
                    : winErr("WaitForMultipleObjects(startup events)",
                             wait.lastError);
            if (w == WAIT_OBJECT_0 + 1 || w == WAIT_OBJECT_0 + 2) {
                static const char* const kReasons[] = {
                    "REFUS", "FAILED", "MISMATCH", "rolling back",
                    "incomplete", "[CRASH]", "refuse", "rejected"
                };
                const std::vector<std::string> said = qgame::readLogHighlights(
                    sessionDirUtf8 + "\\dll.log", qgame::kDllLogLaunchBanner,
                    kReasons, sizeof(kReasons) / sizeof(kReasons[0]), 6u);
                if (!said.empty()) {
                    res.error += "\n\nThe runtime's own log says:";
                    for (const std::string& line : said)
                        res.error += "\n  " + line;
                } else {
                    res.error += "\n\nIts log (" + sessionDirUtf8
                        + "\\dll.log) has no explanation this time.";
                }
            }
            QLOG("[launcher] Stage-A ready-event FAILED - aborting launch");
            CloseHandle(pipe);
            if (readyEvent) CloseHandle(readyEvent);
            if (failedEvent) CloseHandle(failedEvent);
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            if (hDesk) { CloseDesktop(hDesk); res.desktop = nullptr; }
            return res;
        }
    }

    CloseHandle(pipe);
    if (readyEvent) CloseHandle(readyEvent);
    if (failedEvent) CloseHandle(failedEvent);

    res.ok = true;
    res.pid = pi.dwProcessId;
    res.process = pi.hProcess;
    res.mainThread = pi.hThread;
    QLOG("[launcher] prepared suspended game pid=%lu; awaiting peer barrier",
         res.pid);
    return res;
}

LaunchResult resumePrepared(PreparedLaunch&& prepared) {
    LaunchResult result;
    if (!prepared.ok || !prepared.process || !prepared.mainThread) {
        result.error = prepared.error.empty()
            ? "prepared launch is not valid"
            : prepared.error;
        prepared.abort();
        return result;
    }

    QLOG("[launcher] resuming prepared game main thread pid=%lu",
         prepared.pid);
    if (ResumeThread(prepared.mainThread) == static_cast<DWORD>(-1)) {
        result.error = "launch phase 'resume' failed: "
                     + lastErr("ResumeThread");
        QLOG("[launcher] ResumeThread FAILED: %s", result.error.c_str());
        prepared.abort();
        return result;
    }

    if (prepared.inputReadyEvent != nullptr) {
        HANDLE waits[3] = {prepared.inputReadyEvent,
                           prepared.inputFailedEvent, prepared.process};
        const DWORD stageBDeadlineMs =
            launchPhaseTimeoutMs(kStageBInputTimeoutMs);
        const PhaseWait wait =
            waitPhase("Stage B", 3, waits, stageBDeadlineMs);
        const DWORD waited = wait.result;
        const DWORD stageBMs = wait.elapsedMs;
        const bool gameGone = waited == WAIT_OBJECT_0 + 2;
        if (waited != WAIT_OBJECT_0) {
            result.error = waited == WAIT_OBJECT_0 + 1
                ? "DLL reported input-runtime startup failure"
                : (gameGone
                       ? "game exited before input runtime became ready"
                       : phaseTimeoutError("Stage B (InputRuntimeReady)",
                                           stageBMs, stageBDeadlineMs));
            if (!prepared.inputReadyFatal && !gameGone) {
                QLOG("[launcher] Stage-B InputRuntimeReady not ready after "
                     "%lums (%s) - CONTINUING offline; configured pads may "
                     "come online a moment into the session",
                     stageBMs, result.error.c_str());
                result.error.clear();
                CloseHandle(prepared.inputReadyEvent);
                prepared.inputReadyEvent = nullptr;
                CloseHandle(prepared.inputFailedEvent);
                prepared.inputFailedEvent = nullptr;
                Sleep(50);
                CloseHandle(prepared.mainThread);
                prepared.mainThread = nullptr;
                result.ok = true;
                result.pid = prepared.pid;
                result.process = prepared.process;
                result.desktop = prepared.desktop;
                result.gameDirectory = prepared.gameDirectory;
                result.launchedAtFileTime = prepared.launchedAtFileTime;
                result.sessionDirectory = prepared.sessionDirectory;
                result.sessionLogWriterLease =
                    std::move(prepared.sessionLogWriterLease);
                prepared.ok = false;
                prepared.pid = 0;
                prepared.process = nullptr;
                prepared.desktop = nullptr;
                prepared.sessionLogWriterLease = {};
                return result;
            }
            QLOG("[launcher] Stage-B InputRuntimeReady FAILED after %lums: %s",
                 stageBMs, result.error.c_str());
            prepared.abort();
            return result;
        }
        QLOG("[launcher] Stage-B InputRuntimeReady pid=%lu in %lums",
             prepared.pid, stageBMs);
        CloseHandle(prepared.inputReadyEvent);
        prepared.inputReadyEvent = nullptr;
        CloseHandle(prepared.inputFailedEvent);
        prepared.inputFailedEvent = nullptr;
    }

    Sleep(50);
    CloseHandle(prepared.mainThread);
    prepared.mainThread = nullptr;
    result.ok = true;
    result.pid = prepared.pid;
    result.process = prepared.process;
    result.desktop = prepared.desktop;
    result.gameDirectory = prepared.gameDirectory;
    result.launchedAtFileTime = prepared.launchedAtFileTime;
    result.sessionDirectory = prepared.sessionDirectory;
    result.sessionLogWriterLease = std::move(prepared.sessionLogWriterLease);
    result.error = std::move(prepared.error);
    prepared.ok = false;
    prepared.pid = 0;
    prepared.process = nullptr;
    prepared.desktop = nullptr;
    prepared.sessionLogWriterLease = {};
    return result;
}

LaunchResult launchInjected(const std::string& gameExePath,
                            const std::string& dllPath,
                            const std::string& workingDir,
                            const std::string& cmdArgs,
                            qipc::IpcConfig cfg,
                            SOCKET udpSocket,
                            bool hiddenDesktop,
                            SOCKET spectatorSocket) {
    PreparedLaunch prepared = prepareInjected(
        gameExePath, dllPath, workingDir, cmdArgs, cfg, udpSocket,
        hiddenDesktop, spectatorSocket, {});
    if (!prepared.ok) {
        LaunchResult failed;
        failed.error = prepared.error;
        return failed;
    }
    return resumePrepared(std::move(prepared));
}

}
