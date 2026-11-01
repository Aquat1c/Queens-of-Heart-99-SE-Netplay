#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "qoh99_probe.h"
#include "qoh99_functions.h"
#include "qoh99_registry.h"
#include "qoh99_state_machine.h"
#include "../Log.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdarg>

namespace qoh99 {
namespace {

inline bool applyInput(int p, int m) {
    const fn::ApplyLogicalFn apply = fn::applyLogical();
    return apply && apply(p, m) != 0;
}
inline int readInput(int p) {
    const fn::ReadLogicalFn read = fn::readLogical();
    return read ? read(p) : 0;
}
inline uint32_t rd32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

bool memReadable(const void* p, size_t n) {
    if (!p || n == 0) return false;
    const BYTE* a = static_cast<const BYTE*>(p); const BYTE* end = a + n;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end) {
        if (!VirtualQuery(a, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        a = static_cast<const BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}
bool safeCopy(void* dst, const void* src, size_t n) {
    if (!memReadable(src, n)) return false;
    __try { memcpy(dst, src, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

FILE* g_jl = nullptr;
void jl(const char* fmt, ...) {
    if (!g_jl) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_jl, fmt, ap); va_end(ap);
    fputc('\n', g_jl); fflush(g_jl);
}

struct Window {
    const char* name; uintptr_t addr; size_t size;
    std::vector<uint8_t> ref, cur, changed; bool primed = false; uint64_t samples = 0;
    void alloc(const char* n, uintptr_t a, size_t s) {
        name = n; addr = a; size = s;
        ref.assign(s, 0); cur.assign(s, 0); changed.assign(s, 0); primed = false; samples = 0;
    }
    void accumulate() {
        if (!safeCopy(cur.data(), reinterpret_cast<void*>(addr), size)) return;
        if (!primed) { ref = cur; primed = true; samples = 1; return; }
        for (size_t i = 0; i < size; ++i) if (cur[i] != ref[i]) { changed[i] = 1; ref[i] = cur[i]; }
        ++samples;
    }
    void dump(const char* event) {
        size_t total = 0, maxOff = 0;
        for (size_t i = 0; i < size; ++i) if (changed[i]) { ++total; maxOff = i; }
        char buf[4096]; int n = 0;
        n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
            "{\"event\":\"%s\",\"win\":\"%s\",\"addr\":\"0x%IX\",\"size\":%Iu,\"samples\":%llu,"
            "\"changedBytes\":%Iu,\"maxChangedOff\":%Iu,\"ranges\":[",
            event, name, addr, size, (unsigned long long)samples, total, total ? maxOff + 1 : 0);
        size_t i = 0; bool first = true;
        while (i < size) {
            if (!changed[i]) { ++i; continue; }
            size_t s = i; while (i < size && changed[i]) ++i;
            if (n < (int)sizeof(buf) - 40) {
                n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "%s[%Iu,%Iu]", first ? "" : ",", s, i - s);
                first = false;
            }
        }
        _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "]}");
        jl("%s", buf);
    }
    void resetAccum() { std::fill(changed.begin(), changed.end(), 0); primed = false; samples = 0; }
};

ProbeMode g_mode = ProbeMode::None;
bool      g_active = false;
bool      g_done = false;
bool      g_runtimeBindingsChecked = false;
uint64_t  g_frame = 0;
uint64_t  g_matchFrame = 0;
bool      g_inMatch = false;
std::string g_dir;

Window g_fighter[2];
Window g_helper[4];
const uintptr_t kHelperAddr[4] = { 0x614060, 0x61465C, 0x614C58, 0x615254 };
const size_t    kFighterWin = 0x1200;
const size_t    kHelperWin  = 0x800;

int      g_ipBit = 0;
uint64_t g_ipBitStart = 0;
int      g_prevRead[2] = { -1, -1 };

const uint64_t kBootTimeout = 60ull * 60;
const uint64_t kProbeBudget = 90ull * 60;

QohPhase phase() { return detectPhase(readLiveSnapshot()); }

void finish(const char* why) {
    jl("{\"event\":\"probe_end\",\"reason\":\"%s\",\"frames\":%llu}", why, (unsigned long long)g_frame);
    std::string md = g_dir.empty() ? std::string("logs\\qoh_probe_summary.md") : (g_dir + "\\qoh_probe_summary.md");
    if (FILE* f = fopen(md.c_str(), "a")) {
        fprintf(f, "- probe mode=%d ended: %s after %llu frames (matchFrames=%llu)\n",
                (int)g_mode, why, (unsigned long long)g_frame, (unsigned long long)g_matchFrame);
        fclose(f);
    }
    g_done = true;
    if (g_jl) { fclose(g_jl); g_jl = nullptr; }
}

void tickFighterWindows() {
    uint32_t table[4] = {0,0,0,0}; safeCopy(table, reinterpret_cast<void*>(0x4A37B4), sizeof(table));
    g_fighter[0].accumulate();
    g_fighter[1].accumulate();
}

void doFighterWrites() {
    tickFighterWindows();
    if ((g_matchFrame % 600) == 0 && g_matchFrame) {
        g_fighter[0].dump("fighter_writes");
        g_fighter[1].dump("fighter_writes");
    }
}

void doFighterSize() {
    tickFighterWindows();
    if ((g_matchFrame % 600) == 0 && g_matchFrame) {
        g_fighter[0].dump("fighter_size");
        g_fighter[1].dump("fighter_size");
    }
}

void doHelpers() {
    for (int i = 0; i < 4; ++i) {
        uint32_t p = rd32(kHelperAddr[i]);
        if ((g_matchFrame % 120) == 0)
            jl("{\"event\":\"helper\",\"addr\":\"0x%IX\",\"value\":\"0x%08X\",\"ptrValid\":%d}",
               kHelperAddr[i], p, memReadable(reinterpret_cast<void*>(p), 64) ? 1 : 0);
        g_helper[i].accumulate();
    }
    if ((g_matchFrame % 600) == 0 && g_matchFrame)
        for (int i = 0; i < 4; ++i) g_helper[i].dump("helper_writes");
}

void doFighterPointers() {
    static uint64_t lastScan = 0;
    if (g_matchFrame - lastScan < 60) return;
    lastScan = g_matchFrame;
    for (int fi = 0; fi < 2; ++fi) {
        uintptr_t base = g_fighter[fi].addr;
        std::vector<uint8_t> buf(kFighterWin);
        if (!safeCopy(buf.data(), reinterpret_cast<void*>(base), kFighterWin)) continue;
        for (size_t off = 0; off + 4 <= kFighterWin; off += 4) {
            uint32_t v; memcpy(&v, buf.data() + off, 4);
            bool inStatic = (v >= 0x400000 && v < 0x900000);
            bool valid = (v >= 0x10000) && memReadable(reinterpret_cast<void*>(v), 32);
            if (valid && (inStatic || v >= 0x00100000)) {
                const char* cls = inStatic ? "static" : "heap";
                jl("{\"event\":\"fighter_ptr\",\"fighter\":%d,\"off\":%Iu,\"value\":\"0x%08X\",\"class\":\"%s\"}",
                   fi, off, v, cls);
            }
        }
    }
}

void doInputMap() {
    for (int p = 0; p < 2; ++p) {
        int r = readInput(p);
        if (r != g_prevRead[p]) {
            jl("{\"event\":\"read_input\",\"player\":%d,\"word\":\"0x%04X\",\"frame\":%llu}",
               p, (unsigned)(r & 0xFFFF), (unsigned long long)g_matchFrame);
            g_prevRead[p] = r;
        }
    }
    const uint64_t hold = 24;
    if (g_matchFrame < 60) return;
    uint64_t t = g_matchFrame - g_ipBitStart;
    int mask = (g_ipBit < 16) ? (1 << g_ipBit) : 0;
    if (mask) applyInput(0, mask);
    if (t == 1) { g_fighter[0].resetAccum(); }
    if (t == hold) {
        g_fighter[0].dump("input_bit_signature");
        jl("{\"event\":\"input_bit\",\"bit\":\"0x%04X\",\"player\":0}", (unsigned)mask);
        g_ipBit++; g_ipBitStart = g_matchFrame;
        if (g_ipBit > 15) { finish("input_map_complete"); }
    }
}

void doIntro() {
    int matchSub = rd32(0x8719A4);
    uint8_t p1[8] = {0}; safeCopy(p1, reinterpret_cast<void*>(g_fighter[0].addr + 156), 8);
    uint8_t typeB = 0; safeCopy(&typeB, reinterpret_cast<void*>(g_fighter[0].addr + 174), 1);
    static int cls = 0;
    int inject = (cls == 1) ? 0x1 : (cls == 2) ? 0x10 : 0;
    if (inject) applyInput(0, inject);
    if ((g_matchFrame % 6) == 0) {
        jl("{\"event\":\"intro\",\"matchSub\":%d,\"injectClass\":%d,\"inject\":\"0x%X\","
           "\"fighterType@174\":%d,\"flags@156\":\"%02X%02X%02X%02X\"}",
           matchSub, cls, inject, typeB, p1[0], p1[1], p1[2], p1[3]);
        cls = (cls + 1) % 3;
    }
    if (matchSub == 0) { jl("{\"event\":\"intro_ended_gameplay\",\"frame\":%llu}", (unsigned long long)g_matchFrame); finish("intro_reached_gameplay"); }
}

void doScalarProbe(const char* tag, const uintptr_t* addrs, int n) {
    if ((g_matchFrame % 4) != 0) return;
    char buf[512]; int m = 0;
    m += _snprintf_s(buf + m, sizeof(buf) - m, _TRUNCATE, "{\"event\":\"%s\",\"f\":%llu", tag, (unsigned long long)g_matchFrame);
    for (int i = 0; i < n; ++i)
        m += _snprintf_s(buf + m, sizeof(buf) - m, _TRUNCATE, ",\"@0x%IX\":%d", addrs[i], (int)rd32(addrs[i]));
    _snprintf_s(buf + m, sizeof(buf) - m, _TRUNCATE, "}");
    jl("%s", buf);
}

}

void probeInit(ProbeMode mode, const char* sessionDir) {
    g_mode = mode; g_active = (mode != ProbeMode::None); g_done = false;
    g_runtimeBindingsChecked = false;
    g_frame = g_matchFrame = 0; g_inMatch = false;
    g_ipBit = 0; g_ipBitStart = 0; g_prevRead[0] = g_prevRead[1] = -1;
    g_dir = sessionDir ? sessionDir : "";
    g_fighter[0].alloc("P1@623E00", 0x623E00, kFighterWin);
    g_fighter[1].alloc("P2@626880", 0x626880, kFighterWin);
    for (int i = 0; i < 4; ++i) g_helper[i].alloc("helper", kHelperAddr[i], kHelperWin);
    if (g_active) {
        std::string p = (g_dir.empty() ? std::string("logs") : g_dir) + "\\qoh_probe.jsonl";
        g_jl = fopen(p.c_str(), "w");
        jl("{\"event\":\"probe_init\",\"mode\":%d}", (int)mode);
        QLOG("[probe] init mode=%d -> %s", (int)mode, p.c_str());
    }
}

bool probeActive() { return g_active && !g_done; }

void probeFrameTick() {
    if (!g_active || g_done) return;
    if (!g_runtimeBindingsChecked) {
        if (!runtime_profile::runtimeProfileSelectionFinalized()) return;
        g_runtimeBindingsChecked = true;
        if (!fn::readLogical() || !fn::applyLogical()) {
            QLOG("[probe] disabled: no qualified runtime profile supplies logical input bindings");
            finish("no_qualified_runtime_profile");
            return;
        }
    }
    ++g_frame;
    QohPhase ph = phase();

    bool inMatchLike = (ph == QohPhase::RoundActive || ph == QohPhase::RoundHitstopOrFreeze
                        || ph == QohPhase::CharaIntro);
    if (!g_inMatch) {
        if (inMatchLike) { g_inMatch = true; g_matchFrame = 0;
            jl("{\"event\":\"match_reached\",\"phase\":\"%s\",\"frame\":%llu}", phaseName(ph), (unsigned long long)g_frame); }
        else if (g_frame > kBootTimeout) { finish("boot_timeout_no_match"); }
        return;
    }
    ++g_matchFrame;
    if (g_matchFrame > kProbeBudget) { finish("probe_budget_reached"); return; }

    __try {
        switch (g_mode) {
            case ProbeMode::FighterWrites:   doFighterWrites(); break;
            case ProbeMode::FighterSize:     doFighterSize(); break;
            case ProbeMode::FighterPointers: tickFighterWindows(); doFighterPointers(); break;
            case ProbeMode::InputMap:        if (ph == QohPhase::RoundActive) doInputMap(); break;
            case ProbeMode::Intro:           doIntro(); break;
            case ProbeMode::Helpers:         doHelpers(); break;
            case ProbeMode::Timer: {
                static const uintptr_t t[] = { 0x6265C8, 0x626670, 0x871950, 0x613DE0 };
                doScalarProbe("timer_candidates", t, 4); break;
            }
            case ProbeMode::Rng: {
                static const uintptr_t r[] = { 0x4A9938, 0x4A993C, 0x4ABB24 };
                doScalarProbe("rng", r, 3); break;
            }
            case ProbeMode::Blocks:
                { static const uintptr_t b[] = { 0x623700, 0x6234E8, 0x6177CC }; doScalarProbe("blocks", b, 3); }
                break;
            case ProbeMode::All: doFighterWrites(); doFighterPointers(); break;
            default: break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        jl("{\"event\":\"probe_fault\",\"mode\":%d,\"frame\":%llu}", (int)g_mode, (unsigned long long)g_matchFrame);
        finish("fault");
    }
}

}
