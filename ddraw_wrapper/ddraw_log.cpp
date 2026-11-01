#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ddraw_log.h"
#include "../src/Log.h"

volatile long qoh99_wlog_gate = -1;

#if defined(QOH99_WLOG_FORCE_LEVEL)
#define WLOG_DEFAULT_LEVEL QOH99_WLOG_FORCE_LEVEL
#else
#define WLOG_DEFAULT_LEVEL QOH99_WLOG_ON
#endif

#define WLOG_MSG_MAX      1024
#define WLOG_RATE_SLOTS   24
#define WLOG_RATE_KEY     144
#define WLOG_PATH_MAX     4096

#define WLOG_CAP_BYTES         (4L * 1024L * 1024L)
#define WLOG_CAP_BYTES_VERBOSE (16L * 1024L * 1024L)

#define WLOG_LINES_PER_SECOND  400u

enum {
    WLOG_INIT_NONE = 0,
    WLOG_INIT_BUSY = 1,
    WLOG_INIT_READY = 2,
    WLOG_INIT_DISABLED = 3
};

typedef struct WlogRateEntry {
    unsigned int hash;
    char         key[WLOG_RATE_KEY];
    DWORD        lastEmitTick;
    unsigned int dropped;
    int          used;
} WlogRateEntry;

static CRITICAL_SECTION g_cs;
static volatile LONG    g_initState = WLOG_INIT_NONE;
static volatile LONG    g_exiting = 0;
static char             g_path[WLOG_PATH_MAX];
static long             g_capBytes = WLOG_CAP_BYTES;
static int              g_level = QOH99_WLOG_OFF;
static bool             g_continuous = false;
static int              g_sharedFile = 0;
static DWORD            g_rateWindowMs = 1000;
static WlogRateEntry    g_rate[WLOG_RATE_SLOTS];
static DWORD            g_secondTick = 0;
static unsigned int     g_secondLines = 0;
static unsigned int     g_secondDropped = 0;

static unsigned int wlog_hash(const char *text)
{
    unsigned int hash = 2166136261u;
    while (*text) {
        hash ^= (unsigned char)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static void wlog_module_dir(char *out, size_t cap)
{
    HMODULE self = NULL;
    DWORD n = 0;
    char *slash;
    wchar_t wide[WLOG_PATH_MAX];

    out[0] = '\0';
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)(void *)&wlog_module_dir,
            &self)) {
        return;
    }
    n = GetModuleFileNameW(self, wide, WLOG_PATH_MAX);
    if (n > 0 && n < WLOG_PATH_MAX)
        n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL);
    else
        n = 0;
    if (n == 0 || n >= cap) {
        out[0] = '\0';
        return;
    }
    slash = strrchr(out, '\\');
    if (slash)
        slash[1] = '\0';
    else
        out[0] = '\0';
}

static void wlog_open_destination(const char *sessionDir)
{
    char dir[WLOG_PATH_MAX];
    if (sessionDir && sessionDir[0]) {
        size_t len = strlen(sessionDir);
        int needSlash = sessionDir[len - 1] != '\\' && sessionDir[len - 1] != '/';
        snprintf(g_path, sizeof(g_path), "%s%sddraw_wrapper.log",
                 sessionDir, needSlash ? "\\" : "");
        g_sharedFile = 1;
    } else {
        wlog_module_dir(dir, sizeof(dir));
        snprintf(g_path, sizeof(g_path), "%sddraw_wrapper.log", dir);
    }
    qoh99::Log::get().reopenUtf8(g_path, g_continuous, g_capBytes);
}

static int wlog_resolve_level(char *sessionDir, size_t sessionCap)
{
    char value[64];
    wchar_t wideSession[WLOG_PATH_MAX];
    DWORD n;
    int level = -1;

    n = GetEnvironmentVariableA("QOH99_WRAPPER_LOG", value, (DWORD)sizeof(value));
    if (n > 0 && n < sizeof(value)) {
        if (lstrcmpiA(value, "verbose") == 0 || lstrcmpiA(value, "trace") == 0
            || lstrcmpiA(value, "2") == 0) {
            level = QOH99_WLOG_VERBOSE;
        } else if (lstrcmpiA(value, "0") == 0 || lstrcmpiA(value, "off") == 0
                   || lstrcmpiA(value, "false") == 0
                   || lstrcmpiA(value, "no") == 0) {
            level = QOH99_WLOG_OFF;
        } else {
            level = QOH99_WLOG_ON;
        }
    }

    sessionDir[0] = '\0';
    n = GetEnvironmentVariableW(L"QOH99_NETPLAY_SESSION_DIR", wideSession,
                                WLOG_PATH_MAX);
    if (n > 0 && n < WLOG_PATH_MAX)
        WideCharToMultiByte(CP_UTF8, 0, wideSession, -1, sessionDir,
                           (int)sessionCap, NULL, NULL);

    g_continuous = level > QOH99_WLOG_OFF || qoh99::Log::fullLoggingRequested();
#if defined(QOH99_WLOG_FORCE_LEVEL)
    if (level < 0) g_continuous = QOH99_WLOG_FORCE_LEVEL > 0;
#endif
    if (level < 0) level = WLOG_DEFAULT_LEVEL;
    return level;
}

static void wlog_emit_locked(const char *text)
{
    qoh99::Log::get().printf("%s", text);
}

static void wlog_emitf_locked(const char *fmt, ...)
{
    char text[WLOG_MSG_MAX];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(text, sizeof(text) - 1, fmt, ap);
    va_end(ap);
    text[sizeof(text) - 1] = '\0';
    wlog_emit_locked(text);
}

static int wlog_rate_admit_locked(const char *text)
{
    const DWORD now = GetTickCount();
    const unsigned int hash = wlog_hash(text);
    int i;
    int freeSlot = -1;
    int oldestSlot = 0;
    DWORD oldestTick = 0;

    if (g_secondTick == 0 || (now - g_secondTick) >= 1000u) {
        if (g_secondDropped != 0) {
            wlog_emitf_locked(
                "[wrapper-log] rate ceiling dropped %u lines in the previous second",
                g_secondDropped);
        }
        g_secondTick = now;
        g_secondLines = 0;
        g_secondDropped = 0;
    }
    if (g_secondLines >= WLOG_LINES_PER_SECOND) {
        ++g_secondDropped;
        return 0;
    }

    for (i = 0; i < WLOG_RATE_SLOTS; ++i) {
        if (!g_rate[i].used) {
            if (freeSlot < 0)
                freeSlot = i;
            continue;
        }
        if (g_rate[i].hash == hash
            && strncmp(g_rate[i].key, text, WLOG_RATE_KEY - 1) == 0) {
            if ((now - g_rate[i].lastEmitTick) < g_rateWindowMs) {
                ++g_rate[i].dropped;
                return 0;
            }
            if (g_rate[i].dropped != 0) {
                wlog_emitf_locked(
                    "[wrapper-log] previous message repeated %u more times in %lu ms",
                    g_rate[i].dropped,
                    (unsigned long)(now - g_rate[i].lastEmitTick));
                g_rate[i].dropped = 0;
            }
            g_rate[i].lastEmitTick = now;
            ++g_secondLines;
            return 1;
        }
        if (oldestTick == 0 || (DWORD)(now - g_rate[i].lastEmitTick) > oldestTick) {
            oldestTick = (DWORD)(now - g_rate[i].lastEmitTick);
            oldestSlot = i;
        }
    }

    if (freeSlot < 0) {
        freeSlot = oldestSlot;
        if (g_rate[freeSlot].used && g_rate[freeSlot].dropped != 0) {
            wlog_emitf_locked(
                "[wrapper-log] \"%.60s\" repeated %u more times",
                g_rate[freeSlot].key, g_rate[freeSlot].dropped);
        }
    }

    g_rate[freeSlot].used = 1;
    g_rate[freeSlot].hash = hash;
    lstrcpynA(g_rate[freeSlot].key, text, WLOG_RATE_KEY);
    g_rate[freeSlot].lastEmitTick = now;
    g_rate[freeSlot].dropped = 0;
    ++g_secondLines;
    return 1;
}

void qoh99_wlog_init(void)
{
    char sessionDir[WLOG_PATH_MAX];
    int level;
    LONG previous;

    if (InterlockedCompareExchange(&g_initState, WLOG_INIT_BUSY, WLOG_INIT_NONE)
        != WLOG_INIT_NONE) {
        return;
    }

    level = wlog_resolve_level(sessionDir, sizeof(sessionDir));
    g_level = level;
    if (level == QOH99_WLOG_OFF) {
        qoh99_wlog_gate = 0;
        InterlockedExchange(&g_initState, WLOG_INIT_DISABLED);
        return;
    }

    g_capBytes = (level >= QOH99_WLOG_VERBOSE)
        ? WLOG_CAP_BYTES_VERBOSE : WLOG_CAP_BYTES;
    g_rateWindowMs = (level >= QOH99_WLOG_VERBOSE) ? 250u : 1000u;

    InitializeCriticalSection(&g_cs);
    wlog_open_destination(sessionDir[0] ? sessionDir : NULL);

    previous = InterlockedExchange(&g_initState, WLOG_INIT_READY);
    (void)previous;
    qoh99_wlog_gate = (long)level;

    EnterCriticalSection(&g_cs);
    wlog_emitf_locked(
        "============================================================");
    wlog_emitf_locked(
        "QOH99 DirectDraw wrapper log  built " __DATE__ " " __TIME__);
    wlog_emitf_locked(
        "pid=%lu level=%d(%s) path=%s shared=%d cap=%ld bytes",
        (unsigned long)GetCurrentProcessId(), level,
        level >= QOH99_WLOG_VERBOSE ? "verbose" : "on",
        g_path, g_sharedFile, g_capBytes);
    if (sessionDir[0]) {
        wlog_emitf_locked("session directory: %s", sessionDir);
    } else {
        wlog_emitf_locked(
            "session directory: (QOH99_NETPLAY_SESSION_DIR unset; logging beside the wrapper)");
    }
    wlog_emitf_locked(
        "============================================================");
    LeaveCriticalSection(&g_cs);
}

int qoh99_wlog_level(void)
{
    if (InterlockedCompareExchange(&g_initState, 0, 0) == WLOG_INIT_NONE)
        qoh99_wlog_init();
    return g_level;
}

static void qoh99_wlog_write_va(const char *fmt, va_list ap, bool issue = false)
{
    char text[WLOG_MSG_MAX];

    if (InterlockedCompareExchange(&g_exiting, 0, 0) != 0)
        return;
    if (InterlockedCompareExchange(&g_initState, 0, 0) != WLOG_INIT_READY) {
        qoh99_wlog_init();
        if (InterlockedCompareExchange(&g_initState, 0, 0) != WLOG_INIT_READY)
            return;
    }
    if (g_level == QOH99_WLOG_OFF)
        return;

    vsnprintf(text, sizeof(text) - 1, fmt, ap);
    text[sizeof(text) - 1] = '\0';

    EnterCriticalSection(&g_cs);
    if (issue || wlog_rate_admit_locked(text))
        wlog_emit_locked(text);
    LeaveCriticalSection(&g_cs);
}

void qoh99_wlog_write(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    qoh99_wlog_write_va(fmt, ap);
    va_end(ap);
}

void qoh99_wlog_write_verbose(const char *fmt, ...)
{
    va_list ap;
    if (qoh99_wlog_level() < QOH99_WLOG_VERBOSE)
        return;
    va_start(ap, fmt);
    qoh99_wlog_write_va(fmt, ap);
    va_end(ap);
}

void qoh99_wlog_issue(void)
{
    qoh99_wlog_init();
    if (InterlockedCompareExchange(&g_initState, 0, 0) == WLOG_INIT_READY)
        qoh99::Log::get().issue();
}

void qoh99_wlog_write_issue(const char *fmt, ...)
{
    qoh99_wlog_issue();
    va_list ap;
    va_start(ap, fmt);
    qoh99_wlog_write_va(fmt, ap, true);
    va_end(ap);
}

void qoh99_wlog_mark_process_exit(void)
{
    InterlockedExchange(&g_exiting, 1);
    qoh99_wlog_gate = 0;
    if (InterlockedCompareExchange(&g_initState, 0, 0) == WLOG_INIT_READY)
        qoh99::Log::get().markProcessExit();
}

void qoh99_wlog_close(void)
{
    if (InterlockedCompareExchange(&g_initState, 0, 0) != WLOG_INIT_READY)
        return;
    InterlockedExchange(&g_exiting, 1);
    qoh99_wlog_gate = 0;
    qoh99::Log::get().close();
}
