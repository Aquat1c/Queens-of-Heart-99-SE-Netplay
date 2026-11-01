#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "CrashDump.h"
#include "Log.h"
#include "platform/CompatPreamble.h"
#include "platform/WidePath.h"

namespace qcrash {
namespace {

// Local mirrors of the DbgHelp declarations: including <dbghelp.h> could reintroduce a static dbghelp import.
struct MiniDumpExceptionInformation {
    DWORD ThreadId;
    EXCEPTION_POINTERS* ExceptionPointers;
    BOOL ClientPointers;
};
typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(HANDLE process,
                                          DWORD processId,
                                          HANDLE file,
                                          DWORD dumpType,
                                          const MiniDumpExceptionInformation* exception,
                                          const void* userStream,
                                          const void* callback);
constexpr DWORD kDumpType = 0x00000004u;

std::wstring g_dumpDir;
std::string  g_tag;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;

MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;
bool g_dbgHelpProbed = false;
volatile LONG g_caughtCrashSequence = 0;
BoundedCrashHistory g_caughtCrashHistory{};

// Resolve once at install time, never inside the exception filter, where loading a DLL can fault or deadlock.
void probeDbgHelp() {
    if (g_dbgHelpProbed) return;
    g_dbgHelpProbed = true;
    void* module = nullptr;
    if (!qplat::loadOptionalModule(L"dbghelp.dll", module) || module == nullptr) {
        QLOG("[crash] dbghelp.dll unavailable - minidumps DISABLED; the "
             "classified text record below is the authoritative report");
        return;
    }
    g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
        GetProcAddress(static_cast<HMODULE>(module), "MiniDumpWriteDump"));
    if (g_miniDumpWriteDump == nullptr) {
        QLOG("[crash] dbghelp.dll loaded but exports no MiniDumpWriteDump "
             "(stub implementation) - minidumps DISABLED");
        return;
    }
    QLOG("[crash] optional dbghelp.dll available; minidumps ENABLED");
}

const char* codeName(DWORD c) {
    switch (c) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIV0";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        default:                              return "?";
    }
}

std::wstring moduleFileNameW(HMODULE module) {
    std::wstring buffer(260, L'\0');
    for (int attempt = 0; attempt < 6; ++attempt) {
        const DWORD written = GetModuleFileNameW(
            module, &buffer[0], static_cast<DWORD>(buffer.size()));
        if (written == 0) return std::wstring();
        if (written < buffer.size()) {
            buffer.resize(written);
            return buffer;
        }
        buffer.assign(buffer.size() * 2, L'\0');
    }
    return std::wstring();
}

void hexDigest(const uint8_t* const bytes, const size_t count,
               char* const output, const size_t outputSize) {
    static const char kHex[] = "0123456789abcdef";
    if (output == nullptr || outputSize == 0u) return;
    if (bytes == nullptr || outputSize < count * 2u + 1u) {
        output[0] = '\0';
        return;
    }
    for (size_t index = 0u; index < count; ++index) {
        output[index * 2u] = kHex[(bytes[index] >> 4u) & 0x0Fu];
        output[index * 2u + 1u] = kHex[bytes[index] & 0x0Fu];
    }
    output[count * 2u] = '\0';
}

void writeCopiedMiniDump(const CapturedCrashContext& captured) {
    if (g_miniDumpWriteDump == nullptr) {
        QLOG("[CRASH]   minidump skipped (no MiniDumpWriteDump); classified "
             "text record above is complete");
        return;
    }
    if (g_dumpDir.empty()) return;

    wchar_t suffix[128];
    _snwprintf_s(suffix, 128, _TRUNCATE, L"\\crash_%hs_%lu.dmp",
                 g_tag.c_str(), GetCurrentProcessId());
    const std::wstring path = g_dumpDir + suffix;
    HANDLE f = CreateFileW(qplat::longPathW(path).c_str(), GENERIC_WRITE, 0,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        QLOG("[CRASH]   could not create dump file %s (err=%lu)",
             qplat::narrow(path).c_str(), GetLastError());
        return;
    }

    EXCEPTION_RECORD record = captured.exception.exceptionRecord;
    CONTEXT context = captured.exception.contextRecord;
    EXCEPTION_POINTERS pointers{};
    pointers.ExceptionRecord = &record;
    pointers.ContextRecord = &context;
    MiniDumpExceptionInformation mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = &pointers;
    mei.ClientPointers = FALSE;
    const BOOL ok = g_miniDumpWriteDump(GetCurrentProcess(),
                                        GetCurrentProcessId(), f, kDumpType,
                                        &mei, nullptr, nullptr);
    CloseHandle(f);
    QLOG("[CRASH]   minidump %s -> %s", ok ? "written" : "FAILED",
         qplat::narrow(path).c_str());
}

LONG WINAPI filter(EXCEPTION_POINTERS* ep) {
    if (!tryEnterCrashReporter())
        return EXCEPTION_CONTINUE_SEARCH;
    qoh99::Log::get().issue();
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);

    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(addr), &mod);
    const std::wstring modPathW = mod ? moduleFileNameW(mod) : std::wstring();
    std::string modName = modPathW.empty() ? std::string("?")
                                           : qplat::narrow(modPathW);
    if (modName.empty()) modName = "?";
    uintptr_t base = reinterpret_cast<uintptr_t>(mod);
    const char* shortName = modName.c_str();
    for (const char* p = modName.c_str(); *p; ++p)
        if (*p == '\\' || *p == '/') shortName = p + 1;

    QLOG("[CRASH] code=0x%08lX (%s) addr=%p module=%s base=%p rva=0x%IX%s",
         (unsigned long)code, codeName(code), addr, shortName, (void*)base,
         mod ? (a - base) : 0,
         (mod && base == 0x400000) ? "  (qoh99.exe -> +RVA maps to sub_)" : "");
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        QLOG("[CRASH]   access %s address 0x%IX",
             ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
             ep->ExceptionRecord->ExceptionInformation[1]);
    }
#ifdef _M_IX86
    CONTEXT* c = ep->ContextRecord;
    QLOG("[CRASH]   EIP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
         c->Eip, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
#endif

    if (g_miniDumpWriteDump == nullptr) {
        QLOG("[CRASH]   minidump skipped (no MiniDumpWriteDump); classified "
             "text record above is complete");
    } else if (!g_dumpDir.empty()) {
        wchar_t suffix[128];
        _snwprintf_s(suffix, 128, _TRUNCATE, L"\\crash_%hs_%lu.dmp",
                     g_tag.c_str(), GetCurrentProcessId());
        const std::wstring path = g_dumpDir + suffix;
        HANDLE f = CreateFileW(qplat::longPathW(path).c_str(), GENERIC_WRITE, 0,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MiniDumpExceptionInformation mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            BOOL ok = g_miniDumpWriteDump(GetCurrentProcess(),
                                          GetCurrentProcessId(), f, kDumpType,
                                          &mei, nullptr, nullptr);
            CloseHandle(f);
            QLOG("[CRASH]   minidump %s -> %s", ok ? "written" : "FAILED",
                 qplat::narrow(path).c_str());
        } else {
            QLOG("[CRASH]   could not create dump file %s (err=%lu)",
                 qplat::narrow(path).c_str(), GetLastError());
        }
    }

    qoh99::Log::get().flushNow();
    leaveCrashReporter();
    return g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}

bool minidumpsAvailable() { return g_miniDumpWriteDump != nullptr; }

void reportCaughtOwnedTick(const CapturedCrashContext& source) {
    if (!tryEnterCrashReporter()) return;

    qoh99::Log::get().issue();
    CapturedCrashContext captured = source;
    captured.sequence = static_cast<uint32_t>(
        InterlockedIncrement(&g_caughtCrashSequence));
    g_caughtCrashHistory.append(captured);
    const CopiedExceptionContext& exception = captured.exception;
    const OwnedTickCrashSnapshot& tick = captured.tick;
    const char* module = exception.moduleName[0] != '\0'
        ? exception.moduleName : "?";
    QLOG("[CRASH][owned-tick] sequence=%u code=0x%08lX (%s) "
         "addr=%p module=%s base=%p rva=0x%IX access=%s fault=0x%IX",
         captured.sequence,
         static_cast<unsigned long>(exception.exceptionCode),
         codeName(exception.exceptionCode),
         reinterpret_cast<void*>(exception.exceptionAddress), module,
         reinterpret_cast<void*>(exception.moduleBase), exception.moduleRva,
         accessOperationName(exception.operation), exception.faultAddress);
#ifdef _M_IX86
    QLOG("[CRASH][owned-tick] EIP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX "
         "EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
         exception.eip, exception.eax, exception.ebx, exception.ecx,
         exception.edx, exception.esi, exception.edi, exception.ebp,
         exception.esp);
#endif
    QLOG("[CRASH][owned-tick] frame=%u canonical=%u confirmed=%u "
         "rollbackOrigin=%u rollbackDepth=%u speculativeDepth=%u "
         "lifecycle=%u chars=%u/%u actions=%u/%u anim=%u:%u/%u:%u "
         "recentInputs=%u recentRollback=%u",
         tick.frame, tick.canonicalFrame, tick.confirmedFrame,
         tick.rollbackOrigin, static_cast<unsigned>(tick.rollbackDepth),
         static_cast<unsigned>(tick.speculativeDepth), tick.lifecycleState,
         static_cast<unsigned>(tick.characterIds[0]),
         static_cast<unsigned>(tick.characterIds[1]),
         static_cast<unsigned>(tick.actions[0]),
         static_cast<unsigned>(tick.actions[1]),
         static_cast<unsigned>(tick.animationIndices[0]),
         static_cast<unsigned>(tick.animationFrames[0]),
         static_cast<unsigned>(tick.animationIndices[1]),
         static_cast<unsigned>(tick.animationFrames[1]),
         static_cast<unsigned>(tick.recentInputCount),
         static_cast<unsigned>(tick.recentRollbackCount));
    if (captured.identityAvailable != 0u) {
        char executable[65] = {};
        char simulation[65] = {};
        char validation[65] = {};
        hexDigest(captured.compatibilityIdentity.runtimeBuild.executableSha256,
                  32u, executable, sizeof(executable));
        hexDigest(captured.compatibilityIdentity.simulationContent
                      .simulationRootSha256,
                  32u, simulation, sizeof(simulation));
        hexDigest(captured.compatibilityIdentity.simulationContent
                      .validationRootSha256,
                  32u, validation, sizeof(validation));
        QLOG("[CRASH][owned-tick] identity=available profile=%s exe=%s "
             "size=%llu simulationRoot=%s validationRoot=%s stateSchema=%llu "
             "mod=%llu timing=%llu",
             captured.compatibilityIdentity.runtimeBuild.profileId,
             executable,
             static_cast<unsigned long long>(captured.compatibilityIdentity
                 .runtimeBuild.executableSize),
             simulation, validation,
             static_cast<unsigned long long>(captured.compatibilityIdentity
                 .stateSchemaId),
             static_cast<unsigned long long>(captured.compatibilityIdentity
                 .modSimulationId),
             static_cast<unsigned long long>(captured.compatibilityIdentity
                 .timingProfileId));
    } else {
        QLOG("[CRASH][owned-tick] identity=unavailable");
    }
    writeCopiedMiniDump(captured);
    qoh99::Log::get().flushNow();
    leaveCrashReporter();
}

void installW(const std::wstring& dumpDir, const char* tag) {
    g_dumpDir = dumpDir;
    while (!g_dumpDir.empty()
           && (g_dumpDir.back() == L'\\' || g_dumpDir.back() == L'/')) {
        g_dumpDir.pop_back();
    }
    g_tag = tag ? tag : "x";
    probeDbgHelp();
    g_prev = SetUnhandledExceptionFilter(filter);
    QLOG("[crash] unhandled-exception handler installed (classified log always; "
         "minidumps=%s -> %s)",
         minidumpsAvailable() ? "available" : "unavailable",
         qplat::narrow(g_dumpDir).c_str());
}

void install(const std::string& dumpDir, const char* tag) {
    std::wstring wide = qplat::widen(dumpDir);
    if (wide.empty() && !dumpDir.empty()) wide = qplat::widenAcp(dumpDir);
    installW(wide, tag);
}

}
