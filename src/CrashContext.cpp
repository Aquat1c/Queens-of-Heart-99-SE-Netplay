#include "CrashContext.h"

#include <cstring>

namespace qcrash {
namespace {

qnetplay::compat::CompatibilityIdentityWire g_cachedIdentity{};
volatile LONG g_cachedIdentityAvailable = 0;
volatile LONG g_crashReporterActive = 0;

AccessOperation classifyAccessOperation(const EXCEPTION_RECORD& record) {
    if ((record.ExceptionCode != EXCEPTION_ACCESS_VIOLATION
         && record.ExceptionCode != EXCEPTION_IN_PAGE_ERROR)
        || record.NumberParameters < 2u) {
        return AccessOperation::Unknown;
    }
    switch (record.ExceptionInformation[0]) {
        case 0u: return AccessOperation::Read;
        case 1u: return AccessOperation::Write;
        case 8u: return AccessOperation::Execute;
        default: return AccessOperation::Unknown;
    }
}

bool resolveProductionModule(const uintptr_t address, CrashModuleInfo& out) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module)
        || module == nullptr) {
        return false;
    }
    out.base = reinterpret_cast<uintptr_t>(module);
    const DWORD written = GetModuleFileNameA(module, out.name,
                                              static_cast<DWORD>(sizeof(out.name)));
    if (written == 0 || written >= sizeof(out.name)) {
        out.name[sizeof(out.name) - 1u] = '\0';
    }
    return true;
}

}

const char* accessOperationName(const AccessOperation operation) {
    switch (operation) {
        case AccessOperation::Read: return "read";
        case AccessOperation::Write: return "write";
        case AccessOperation::Execute: return "execute";
        default: return "unknown";
    }
}

bool captureExceptionContext(
    const EXCEPTION_POINTERS* exceptionPointers,
    const OwnedTickCrashSnapshot& tick,
    const qnetplay::compat::CompatibilityIdentityWire* compatibilityIdentity,
    CapturedCrashContext& out,
    CrashModuleResolver resolver,
    void* resolverContext) noexcept {
    if (exceptionPointers == nullptr
        || exceptionPointers->ExceptionRecord == nullptr
        || exceptionPointers->ContextRecord == nullptr) {
        return false;
    }

    CapturedCrashContext captured{};
    captured.schemaVersion = kCrashContextSchemaVersion;
    captured.tick = tick;
    std::memcpy(&captured.exception.exceptionRecord,
                exceptionPointers->ExceptionRecord,
                sizeof(captured.exception.exceptionRecord));
    captured.exception.exceptionRecord.ExceptionRecord = nullptr;
    std::memcpy(&captured.exception.contextRecord,
                exceptionPointers->ContextRecord,
                sizeof(captured.exception.contextRecord));
    captured.exception.exceptionCode =
        captured.exception.exceptionRecord.ExceptionCode;
    captured.exception.exceptionAddress = reinterpret_cast<uintptr_t>(
        captured.exception.exceptionRecord.ExceptionAddress);
    captured.exception.operation =
        classifyAccessOperation(captured.exception.exceptionRecord);
    if (captured.exception.exceptionRecord.NumberParameters >= 2u) {
        captured.exception.faultAddress = static_cast<uintptr_t>(
            captured.exception.exceptionRecord.ExceptionInformation[1]);
    }
#ifdef _M_IX86
    const CONTEXT& context = captured.exception.contextRecord;
    captured.exception.eip = context.Eip;
    captured.exception.eax = context.Eax;
    captured.exception.ebx = context.Ebx;
    captured.exception.ecx = context.Ecx;
    captured.exception.edx = context.Edx;
    captured.exception.esi = context.Esi;
    captured.exception.edi = context.Edi;
    captured.exception.ebp = context.Ebp;
    captured.exception.esp = context.Esp;
#endif

    CrashModuleInfo module{};
    const uintptr_t address = captured.exception.exceptionAddress;
    const bool moduleResolved = resolver != nullptr
        ? resolver(address, module, resolverContext)
        : resolveProductionModule(address, module);
    if (moduleResolved) {
        captured.exception.moduleBase = module.base;
        if (module.base != 0u && address >= module.base) {
            captured.exception.moduleRva = address - module.base;
        }
        std::memcpy(captured.exception.moduleName, module.name,
                    sizeof(captured.exception.moduleName));
        captured.exception.moduleName[
            sizeof(captured.exception.moduleName) - 1u] = '\0';
    }

    if (compatibilityIdentity != nullptr
        && qnetplay::compat::isValidCompatibilityIdentity(
            *compatibilityIdentity)) {
        captured.identityAvailable = 1u;
        std::memcpy(&captured.compatibilityIdentity, compatibilityIdentity,
                    sizeof(captured.compatibilityIdentity));
    }
    out = captured;
    return true;
}

void cacheCompatibilityIdentity(
    const qnetplay::compat::CompatibilityIdentityWire& identity) noexcept {
    InterlockedExchange(&g_cachedIdentityAvailable, 0);
    std::memcpy(&g_cachedIdentity, &identity, sizeof(g_cachedIdentity));
    MemoryBarrier();
    InterlockedExchange(&g_cachedIdentityAvailable, 1);
}

void clearCachedCompatibilityIdentityForTests() noexcept {
    InterlockedExchange(&g_cachedIdentityAvailable, 0);
    std::memset(&g_cachedIdentity, 0, sizeof(g_cachedIdentity));
}

bool copyCachedCompatibilityIdentity(
    qnetplay::compat::CompatibilityIdentityWire& out) noexcept {
    if (InterlockedCompareExchange(&g_cachedIdentityAvailable, 0, 0) == 0)
        return false;
    std::memcpy(&out, &g_cachedIdentity, sizeof(out));
    return qnetplay::compat::isValidCompatibilityIdentity(out);
}

void BoundedCrashHistory::append(const CapturedCrashContext& context) noexcept {
    records_[next_] = context;
    next_ = (next_ + 1u) % kCapacity;
    if (size_ < kCapacity) ++size_;
}

const CapturedCrashContext& BoundedCrashHistory::newest(
    const size_t indexFromNewest) const noexcept {
    static const CapturedCrashContext kEmpty{};
    if (indexFromNewest >= size_) return kEmpty;
    const size_t newestIndex = (next_ + kCapacity - 1u) % kCapacity;
    const size_t index = (newestIndex + kCapacity - indexFromNewest) % kCapacity;
    return records_[index];
}

void BoundedCrashHistory::clear() noexcept {
    std::memset(records_, 0, sizeof(records_));
    next_ = 0;
    size_ = 0;
}

bool invokeCopiedMiniDumpWriter(const CapturedCrashContext& captured,
                                const CopiedMiniDumpWriter writer,
                                void* const context) noexcept {
    if (writer == nullptr) return false;
    EXCEPTION_RECORD record = captured.exception.exceptionRecord;
    CONTEXT registers = captured.exception.contextRecord;
    return writer(record, registers, context);
}

bool tryEnterCrashReporter() noexcept {
    return InterlockedCompareExchange(&g_crashReporterActive, 1, 0) == 0;
}

void leaveCrashReporter() noexcept {
    InterlockedExchange(&g_crashReporterActive, 0);
}

}
