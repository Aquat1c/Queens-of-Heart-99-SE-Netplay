#include "input/SdlLifecycle.h"

#include <cstdio>

#include "Log.h"

#ifdef QOH99_WITH_SDL3
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <SDL3/SDL.h>
#include <cstring>
#endif

namespace qinput {

#ifdef QOH99_WITH_SDL3

namespace {
// SDL3 is delay-loaded: touching any SDL symbol before this probe succeeds terminates the process when the runtime is absent or stale.
bool sdlRuntimeAvailable() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    if (LoadLibraryW(L"SDL3.dll") != nullptr) {
        cached = 1;
    } else {
        cached = 0;
        const DWORD loadError = GetLastError();
        wchar_t here[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, here, MAX_PATH);
        QLOG("[controller.sdl] SDL3.dll did NOT load (LoadLibrary error %lu). "
             "Standardized controller profiles cannot run without it; raw "
             "profiles and the keyboard are unaffected. It is expected next "
             "to %ls",
             static_cast<unsigned long>(loadError),
             length != 0 && length < MAX_PATH ? here : L"the executable");
    }
    return cached == 1;
}

bool g_owned = false;
bool g_savedJoystickEvents = false;
bool g_savedGamepadEvents = false;
char g_savedThreadHint[16] = {};
bool g_hadThreadHint = false;

// One process-wide lock: SDL_InitSubSystem and SDL_QuitSubSystem must never overlap.
CRITICAL_SECTION g_lifecycleLock;
INIT_ONCE g_lifecycleLockOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK initLifecycleLock(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_lifecycleLock);
    return TRUE;
}

CRITICAL_SECTION& lifecycleLock() {
    InitOnceExecuteOnce(&g_lifecycleLockOnce, &initLifecycleLock, nullptr,
                        nullptr);
    return g_lifecycleLock;
}

struct LifecycleGuard {
    LifecycleGuard() { EnterCriticalSection(&lifecycleLock()); }
    ~LifecycleGuard() { LeaveCriticalSection(&lifecycleLock()); }
};

// Separate lock: waiting for the warm-up must never hold the lifecycle lock the warm-up needs.
CRITICAL_SECTION g_warmupLock;
INIT_ONCE g_warmupLockOnce = INIT_ONCE_STATIC_INIT;
HANDLE g_warmupThread = nullptr;

BOOL CALLBACK initWarmupLock(PINIT_ONCE, PVOID, PVOID*) {
    InitializeCriticalSection(&g_warmupLock);
    return TRUE;
}

CRITICAL_SECTION& warmupLock() {
    InitOnceExecuteOnce(&g_warmupLockOnce, &initWarmupLock, nullptr, nullptr);
    return g_warmupLock;
}

DWORD WINAPI warmupThread(LPVOID) {
    sdlPreloadRuntime();
    return 0;
}

SdlBootstrapResult bootstrapLocked() {
    if (g_owned) return SdlBootstrapResult::Ready;
    if (!sdlRuntimeAvailable())
        return SdlBootstrapResult::UnavailableBackend;
    if (SDL_WasInit(SDL_INIT_GAMEPAD) != 0
        || SDL_WasInit(SDL_INIT_JOYSTICK) != 0) {
        return SdlBootstrapResult::ForeignOwner;
    }
    const char* existing = SDL_GetHint(SDL_HINT_JOYSTICK_THREAD);
    g_hadThreadHint = existing != nullptr;
    if (g_hadThreadHint) {
        std::snprintf(g_savedThreadHint, sizeof(g_savedThreadHint), "%s",
                      existing);
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        return SdlBootstrapResult::UnavailableBackend;
    }
    g_savedJoystickEvents = SDL_JoystickEventsEnabled();
    g_savedGamepadEvents = SDL_GamepadEventsEnabled();
    SDL_SetJoystickEventsEnabled(false);
    SDL_SetGamepadEventsEnabled(false);
    g_owned = true;
    return SdlBootstrapResult::Ready;
}

}

void sdlPreloadRuntime() { (void)sdlRuntimeAvailable(); }

SdlBootstrapResult sdlBootstrapOnMainThread() {
    LifecycleGuard guard;
    return bootstrapLocked();
}

void sdlWarmupAsync() {
    EnterCriticalSection(&warmupLock());
    if (g_warmupThread == nullptr) {
        g_warmupThread =
            CreateThread(nullptr, 0, &warmupThread, nullptr, 0, nullptr);
    }
    const bool started = g_warmupThread != nullptr;
    LeaveCriticalSection(&warmupLock());
    if (!started) (void)sdlBootstrapOnMainThread();
}

void sdlWaitForWarmup() {
    EnterCriticalSection(&warmupLock());
    HANDLE thread = g_warmupThread;
    LeaveCriticalSection(&warmupLock());
    if (thread == nullptr) return;
    WaitForSingleObject(thread, INFINITE);
    EnterCriticalSection(&warmupLock());
    if (g_warmupThread == thread) {
        CloseHandle(g_warmupThread);
        g_warmupThread = nullptr;
    }
    LeaveCriticalSection(&warmupLock());
}

void sdlTeardownOnMainThread() {
    LifecycleGuard guard;
    if (!g_owned) return;
    SDL_SetJoystickEventsEnabled(g_savedJoystickEvents);
    SDL_SetGamepadEventsEnabled(g_savedGamepadEvents);
    // Release ONLY this module's reference; never process-global SDL_Quit.
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD,
                g_hadThreadHint ? g_savedThreadHint : nullptr);
    g_owned = false;
}

void sdlReleaseForLaunch() {
    // A warm-up inside SDL_InitSubSystem must finish before the quit, or the spawned game could inherit a half-owned subsystem.
    sdlWaitForWarmup();
    sdlTeardownOnMainThread();
}

IControllerBackend* createControllerBackend(void* focusWindow) {
    LifecycleGuard guard;
    if (g_owned) return createSdlControllerBackend();
    return createDirectInput8ControllerBackend(focusWindow);
}

const char* controllerBackendName() {
    LifecycleGuard guard;
    return g_owned ? "SDL3" : "DirectInput8";
}

bool standardizedBackendAvailable() {
    LifecycleGuard guard;
    return g_owned;
}

void sdlVersionText(char* out, unsigned int capacity) {
    if (!sdlRuntimeAvailable()) {
        std::snprintf(out, capacity, "SDL3.dll not present");
        return;
    }
    const int runtime = SDL_GetVersion();
    std::snprintf(out, capacity, "compiled %d.%d.%d runtime %d.%d.%d",
                  SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION,
                  SDL_VERSIONNUM_MAJOR(runtime),
                  SDL_VERSIONNUM_MINOR(runtime),
                  SDL_VERSIONNUM_MICRO(runtime));
}

#else

void sdlPreloadRuntime() {}

SdlBootstrapResult sdlBootstrapOnMainThread() {
    return SdlBootstrapResult::UnavailableBackend;
}
void sdlTeardownOnMainThread() {}
void sdlWarmupAsync() {}
void sdlWaitForWarmup() {}
void sdlReleaseForLaunch() {}

IControllerBackend* createControllerBackend(void* focusWindow) {
    return createDirectInput8ControllerBackend(focusWindow);
}
const char* controllerBackendName() { return "DirectInput8"; }
bool standardizedBackendAvailable() { return false; }

void sdlVersionText(char* out, unsigned int capacity) {
    std::snprintf(out, capacity, "not built");
}

#endif

}
