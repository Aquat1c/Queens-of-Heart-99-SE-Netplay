/* GAME.EXE has no ASLR and no relocations: it loads at 0x400000, so these absolute VAs are stable. */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "qoh99_instrument.h"
#include "qoh99_instrument_policy.h"

#define QH_MATCH_STATE_BLOCK0      ( 0x4ED140u )
#define QH_MATCH_STATE_BLOCK1      ( 0x50A010u )
#define QH_MATCH_STATE_BLOCK_SIZE  ( 118480u )
#define QH_MATCH_STATE_ACTIVE_PTR  ( (uint32_t *) 0x6234E8u )
#define QH_MATCH_STATE_SELECTOR    ( (uint32_t *) 0x623700u )
#define QH_FIGHTER_PTR_TABLE       ( (uint32_t *) 0x4A37B4u )
#define QH_RNG_SEED_GAMEPLAY       ( (uint32_t *) 0x4A9938u )
#define QH_RNG_SEED_COSMETIC       ( (uint32_t *) 0x4A993Cu )
#define QH_RNG_SELECTOR            ( (uint32_t *) 0x4ABB24u )
#define QH_HEALTH_BASE             ( (int32_t  *) 0x6265C8u )
#define QH_SCORE_BASE              ( (int32_t  *) 0x626670u )
#define QH_GAME_STATE_SELECTOR     ( (uint32_t *) 0x8719A4u )
#define QH_IN_MATCH_STATE          ( (uint32_t *) 0x66EE38u )
#define QH_SCENE_STATE             ( (uint32_t *) 0x6234ECu ) /* Play-session scene; 2 means match. */
#define QH_SCENE_NEXT              ( (uint32_t *) 0x61801Cu )
#define QH_MENU_STATE              ( (uint32_t *) 0x4AB784u )
#define QH_SUB48CA20_STATE         ( (uint32_t *) 0x4AB8C4u ) /* Substate of sub_48CA20; 4 means launch match. */
#define QH_MENU_CURSOR             ( (uint32_t *) 0x4AB8D0u )
#define QH_VSMODE_ROW              ( (uint32_t *) 0x4AA458u )
#define QH_VSMODE_CONFIRM          ( (uint32_t *) 0x4AA450u )

#define QH_FIGHTER_SLOTS           ( 4 )
#define QH_FIGHTER_WINDOW          ( 8192u )

#define QH_AUTO_DUMP_FRAMES        ( 3600ull )

static CRITICAL_SECTION g_cs;
static int              g_csInit = 0;
static FILE            *g_log    = NULL;
static volatile LONG    g_instrumentPolicy = 0;

static int instr_enabled(void)
{
    LONG policy = InterlockedCompareExchange(&g_instrumentPolicy, 0, 0);
    if (policy == 0) {
        char value[16] = {0};
        DWORD length = GetEnvironmentVariableA(
            "QOH99_DDRAW_INSTRUMENT", value, (DWORD)sizeof(value));
        LONG resolved =
            length > 0 && length < sizeof(value)
            && qoh99_instr_policy_exact_opt_in(value)
                ? 2
                : 1;
        policy = InterlockedCompareExchange(
            &g_instrumentPolicy, resolved, 0);
        if (policy == 0)
            policy = resolved;
    }
    return policy == 2;
}

static void ilog(const char *fmt, ...)
{
    va_list ap;
    SYSTEMTIME st;
    if (!g_log) return;
    if (g_csInit) EnterCriticalSection(&g_cs);
    GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    if (g_csInit) LeaveCriticalSection(&g_cs);
}

static int mem_readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    const BYTE *addr = (const BYTE *)p;
    const BYTE *end  = addr + n;
    if (!p || n == 0) return 0;
    while (addr < end) {
        if (!VirtualQuery(addr, &mbi, sizeof(mbi))) return 0;
        if (mbi.State != MEM_COMMIT) return 0;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
        addr = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
    }
    return 1;
}

static int safe_copy(void *dst, const void *src, size_t n)
{
    if (!mem_readable(src, n)) return 0;
    __try {
        memcpy(dst, src, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

typedef struct {
    const char *name;
    uint32_t    baseVA;
    uint32_t    lastPtr;
    size_t      size;
    BYTE       *ref;
    BYTE       *cur;
    BYTE       *changed;
    int         primed;
    uint64_t    samples;
} Region;

static Region g_block[2];
static Region g_fighter[QH_FIGHTER_SLOTS];
static uint64_t g_frame = 0;
static uint64_t g_mono  = 0;
static int      g_ready = 0;

static uint32_t g_prevScene   = 0xFFFFFFFF;
static uint32_t g_prevNext    = 0xFFFFFFFF;
static uint32_t g_prevInMatch = 0xFFFFFFFF;
static uint32_t g_prevGs      = 0xFFFFFFFF;
static uint32_t g_prevMenu    = 0xFFFFFFFF;
static uint32_t g_prevSub48   = 0xFFFFFFFF;
static uint32_t g_prevCursor  = 0xFFFFFFFF;
static uint32_t g_prevVsRow   = 0xFFFFFFFF;
static uint32_t g_prevVsConf  = 0xFFFFFFFF;
static int      g_inMatchRun  = 0;
static uint64_t g_lastAutoDump = 0;

static int region_alloc(Region *r, const char *name, uint32_t baseVA, size_t size)
{
    r->name = name; r->baseVA = baseVA; r->lastPtr = 0; r->size = size;
    r->ref     = (BYTE *)calloc(1, size);
    r->cur     = (BYTE *)calloc(1, size);
    r->changed = (BYTE *)calloc(1, size);
    r->primed = 0; r->samples = 0;
    return (r->ref && r->cur && r->changed);
}

static void region_reset_accum(Region *r)
{
    if (r->changed) memset(r->changed, 0, r->size);
    r->primed = 0; r->samples = 0;
}

static void region_accumulate(Region *r, uint32_t curPtr)
{
    if (!safe_copy(r->cur, (const void *)(uintptr_t)curPtr, r->size))
        return;
    if (!r->primed || (r->baseVA == 0 && curPtr != r->lastPtr)) {
        memcpy(r->ref, r->cur, r->size);
        if (r->baseVA == 0 && r->primed && curPtr != r->lastPtr)
            ilog("%s pointer changed %08X -> %08X (re-baseline)", r->name, r->lastPtr, curPtr);
        r->lastPtr = curPtr;
        r->primed = 1;
        r->samples = 1;
        return;
    }
    for (size_t i = 0; i < r->size; i++) {
        if (r->cur[i] != r->ref[i]) {
            r->changed[i] = 1;
            r->ref[i] = r->cur[i];
        }
    }
    r->samples++;
}

static void region_dump(Region *r)
{
    size_t total = 0, minOff = r->size, maxOff = 0, ranges = 0;
    const size_t gapMerge = 64, minLen = 1;
    if (!g_log || !r->changed) return;
    for (size_t k = 0; k < r->size; k++)
        if (r->changed[k]) { total++; if (k < minOff) minOff = k; if (k > maxOff) maxOff = k; }
    ilog("--- %s  baseVA=%08X size=%zu samples=%llu changedBytes=%zu span=[0x%zX..0x%zX] (%.1f%%)",
         r->name, r->baseVA, r->size, (unsigned long long)r->samples, total,
         total ? minOff : 0, total ? maxOff + 1 : 0,
         r->size ? (100.0 * (double)total / (double)r->size) : 0.0);
    if (total == 0) return;
    size_t i = 0;
    while (i < r->size) {
        if (!r->changed[i]) { i++; continue; }
        size_t start = i, end = i, j = i;
        while (j < r->size) {
            if (r->changed[j]) { end = j; j++; }
            else {
                size_t g = j, cnt = 0;
                while (g < r->size && !r->changed[g] && cnt < gapMerge) { g++; cnt++; }
                if (g < r->size && r->changed[g] && cnt < gapMerge) j = g;
                else break;
            }
        }
        if (end + 1 - start >= minLen) {
            ilog("    [+0x%05zX .. +0x%05zX)  %zuB", start, end + 1, end + 1 - start);
            ranges++;
        }
        i = end + 1;
    }
    ilog("    -> %zu coalesced ranges (gapMerge=%zu)", ranges, gapMerge);
}

static void instr_init_once(void)
{
    if (!instr_enabled()) return;
    if (g_ready) return;
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = 1; }
    if (!g_log) { CreateDirectoryA("logs", NULL); g_log = fopen("logs\\instrument.log", "w"); }
    if (!g_log) g_log = fopen("qoh99_instrument.log", "w");
    ilog("============================================================");
    ilog("  QOH99 memory-diff instrument  built " __DATE__ " " __TIME__);
    ilog("  AUTO: resets at match start, dumps at match end (no keys needed)");
    ilog("  F2 = manual reset accumulation   F3 = manual dump findings now");
    ilog("============================================================");
    region_alloc(&g_block[0], "STATEBLOCK[0]@4ED140", QH_MATCH_STATE_BLOCK0, QH_MATCH_STATE_BLOCK_SIZE);
    region_alloc(&g_block[1], "STATEBLOCK[1]@50A010", QH_MATCH_STATE_BLOCK1, QH_MATCH_STATE_BLOCK_SIZE);
    for (int i = 0; i < QH_FIGHTER_SLOTS; i++) {
        static const char *names[QH_FIGHTER_SLOTS] =
            { "FIGHTER[0]", "FIGHTER[1]", "FIGHTER[2]", "FIGHTER[3]" };
        region_alloc(&g_fighter[i], names[i], 0, QH_FIGHTER_WINDOW);
    }
    g_ready = 1;
}

static void instr_reset_accum_all(void)
{
    region_reset_accum(&g_block[0]);
    region_reset_accum(&g_block[1]);
    for (int i = 0; i < QH_FIGHTER_SLOTS; i++) region_reset_accum(&g_fighter[i]);
    g_frame = 0;
}

void qoh99_instr_on_frame(void)
{
    if (!instr_enabled()) return;
    instr_init_once();
    g_mono++;

    uint32_t scene=0xFFFFFFFF, next=0xFFFFFFFF, inMatch=0xFFFFFFFF,
             gs=0xFFFFFFFF, menu=0xFFFFFFFF;
    safe_copy(&scene,   QH_SCENE_STATE,        4);
    safe_copy(&next,    QH_SCENE_NEXT,         4);
    safe_copy(&inMatch, QH_IN_MATCH_STATE,     4);
    safe_copy(&gs,      QH_GAME_STATE_SELECTOR,4);
    safe_copy(&menu,    QH_MENU_STATE,         4);

    if (menu != g_prevMenu) {
        ilog("[state] menu  %d -> %d  (mono=%llu)", (int)g_prevMenu, (int)menu, (unsigned long long)g_mono);
        g_prevMenu = menu;
    }
    if (scene != g_prevScene || next != g_prevNext) {
        ilog("[state] scene %d -> %d  (next=%d, mono=%llu)",
             (int)g_prevScene, (int)scene, (int)next, (unsigned long long)g_mono);
        g_prevScene = scene; g_prevNext = next;
    }
    if (gs != g_prevGs) {
        ilog("[state] matchSub %d -> %d  (mono=%llu)", (int)g_prevGs, (int)gs, (unsigned long long)g_mono);
        g_prevGs = gs;
    }

    {
        uint32_t sub48=0xFFFFFFFF, cursor=0xFFFFFFFF, vsRow=0xFFFFFFFF, vsConf=0xFFFFFFFF;
        safe_copy(&sub48,  QH_SUB48CA20_STATE, 4);
        safe_copy(&cursor, QH_MENU_CURSOR,     4);
        safe_copy(&vsRow,  QH_VSMODE_ROW,      4);
        safe_copy(&vsConf, QH_VSMODE_CONFIRM,  4);
        if (sub48 != g_prevSub48) {
            ilog("[boot] sub48CA20 %d -> %d  (scene=%d next=%d mono=%llu)",
                 (int)g_prevSub48, (int)sub48, (int)scene, (int)next, (unsigned long long)g_mono);
            g_prevSub48 = sub48;
        }
        if (cursor != g_prevCursor) {
            ilog("[boot] menuCursor %d -> %d  (scene=%d sub48=%d mono=%llu)",
                 (int)g_prevCursor, (int)cursor, (int)scene, (int)sub48, (unsigned long long)g_mono);
            g_prevCursor = cursor;
        }
        if (vsRow != g_prevVsRow) {
            ilog("[boot] vsModeRow %d -> %d  (scene=%d mono=%llu)",
                 (int)g_prevVsRow, (int)vsRow, (int)scene, (unsigned long long)g_mono);
            g_prevVsRow = vsRow;
        }
        if (vsConf != g_prevVsConf) {
            ilog("[boot] vsModeConfirm %d -> %d  (scene=%d vsRow=%d mono=%llu)",
                 (int)g_prevVsConf, (int)vsConf, (int)scene, (int)vsRow, (unsigned long long)g_mono);
            g_prevVsConf = vsConf;
        }
    }

    /* 0x66EE38 is a netplay-only flag (stays 0 in local matches); do not gate match detection on it. */
    (void)g_prevInMatch;
    int matchActive = (scene == 2);
    if (matchActive && !g_inMatchRun) {
        instr_reset_accum_all();
        g_inMatchRun = 1;
        g_lastAutoDump = g_mono;
        ilog("=== MATCH START (scene->2, auto-reset accumulation) mono=%llu ===",
             (unsigned long long)g_mono);
    } else if (!matchActive && g_inMatchRun) {
        ilog("=== MATCH END (scene %d, was 2) - auto-dump ===", (int)scene);
        qoh99_instr_dump("match-end");
        g_inMatchRun = 0;
    }

    uint32_t activePtr = 0, selector = 0;
    uint32_t fptr[QH_FIGHTER_SLOTS] = {0,0,0,0};
    int haveBlock = 0;
    if (safe_copy(&activePtr, QH_MATCH_STATE_ACTIVE_PTR, 4) &&
        (activePtr == QH_MATCH_STATE_BLOCK0 || activePtr == QH_MATCH_STATE_BLOCK1)) {
        haveBlock = 1;
        safe_copy(&selector, QH_MATCH_STATE_SELECTOR, 4);
        g_frame++;

        Region *blk = (activePtr == QH_MATCH_STATE_BLOCK1) ? &g_block[1] : &g_block[0];
        region_accumulate(blk, activePtr);

        safe_copy(fptr, QH_FIGHTER_PTR_TABLE, sizeof(fptr));
        for (int i = 0; i < QH_FIGHTER_SLOTS; i++) {
            if (fptr[i] >= 0x10000 && mem_readable((void *)(uintptr_t)fptr[i], 64))
                region_accumulate(&g_fighter[i], fptr[i]);
        }

        if (g_inMatchRun && g_mono - g_lastAutoDump >= QH_AUTO_DUMP_FRAMES) {
            qoh99_instr_dump("periodic");
            g_lastAutoDump = g_mono;
        }
    }

    if (haveBlock && (g_frame <= 3 || (g_frame % 300) == 0)) {
        uint32_t rngG=0, rngC=0, rngSel=0;
        int32_t hp[2]={0,0}, sc[2]={0,0};
        safe_copy(&rngG, QH_RNG_SEED_GAMEPLAY, 4);
        safe_copy(&rngC, QH_RNG_SEED_COSMETIC, 4);
        safe_copy(&rngSel, QH_RNG_SELECTOR, 4);
        safe_copy(&hp[0], QH_HEALTH_BASE + 11*0, 4);
        safe_copy(&hp[1], QH_HEALTH_BASE + 11*1, 4);
        safe_copy(&sc[0], QH_SCORE_BASE + 0, 4);
        safe_copy(&sc[1], QH_SCORE_BASE + 1, 4);
        ilog("frame=%llu sel=%u active=%08X scene=%d matchSub=%d inMatch=%d rng[g=%08X c=%08X sel=%u] hp=[%d,%d] score=[%d,%d] fighters=[%08X %08X %08X %08X]",
             (unsigned long long)g_frame, selector, activePtr, (int)scene, (int)gs, (int)inMatch,
             rngG, rngC, rngSel, hp[0], hp[1], sc[0], sc[1],
             fptr[0], fptr[1], fptr[2], fptr[3]);
    }
}

void qoh99_instr_reset(void)
{
    if (!instr_enabled()) return;
    if (!g_ready) return;
    instr_reset_accum_all();
    ilog("=== accumulation RESET (frame counter zeroed, mono=%llu) ===",
         (unsigned long long)g_mono);
}

void qoh99_instr_dump(const char *reason)
{
    if (!instr_enabled()) return;
    if (!g_ready) { instr_init_once(); }
    ilog("======== DUMP (%s) at frame=%llu ========", reason ? reason : "?", (unsigned long long)g_frame);
    region_dump(&g_block[0]);
    region_dump(&g_block[1]);
    for (int i = 0; i < QH_FIGHTER_SLOTS; i++) {
        if (g_fighter[i].primed) region_dump(&g_fighter[i]);
        else ilog("--- %s: never seen (null/unreadable pointer)", g_fighter[i].name);
    }
    ilog("======== END DUMP ========");
}

void qoh99_instr_shutdown(void)
{
    if (!instr_enabled()) return;
    if (g_ready) qoh99_instr_dump("shutdown");
    if (g_log) { fclose(g_log); g_log = NULL; }
    for (int i = 0; i < 2; i++) { free(g_block[i].ref); free(g_block[i].cur); free(g_block[i].changed); }
    for (int i = 0; i < QH_FIGHTER_SLOTS; i++) { free(g_fighter[i].ref); free(g_fighter[i].cur); free(g_fighter[i].changed); }
    g_ready = 0;
}
