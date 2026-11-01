#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "compat/CompatibilityIdentity.h"

namespace qcrash {

constexpr uint32_t kCrashContextSchemaVersion = 1u;
constexpr size_t kCrashRecentInputCapacity = 8u;
constexpr size_t kCrashRecentRollbackCapacity = 8u;

enum class AccessOperation : uint8_t {
    Unknown = 0,
    Read,
    Write,
    Execute,
};

const char* accessOperationName(AccessOperation operation);

struct CrashModuleInfo {
    uintptr_t base = 0;
    char name[128] = {};
};

using CrashModuleResolver = bool (*)(uintptr_t address,
                                     CrashModuleInfo& out,
                                     void* context);

struct RecentInputSample {
    uint32_t frame = 0;
    uint32_t playerOne = 0;
    uint32_t playerTwo = 0;
};

struct RecentRollbackDecision {
    uint32_t frame = 0;
    uint32_t origin = 0;
    uint16_t depth = 0;
    uint8_t replay = 0;
    uint8_t speculative = 0;
};

struct OwnedTickCrashSnapshot {
    uint32_t frame = 0;
    uint32_t canonicalFrame = 0;
    uint32_t confirmedFrame = 0;
    uint32_t rollbackOrigin = 0;
    uint16_t rollbackDepth = 0;
    uint16_t speculativeDepth = 0;
    uint32_t lifecycleState = 0;
    uint8_t characterIds[2] = {};
    uint16_t actions[2] = {};
    uint16_t animationIndices[2] = {};
    uint16_t animationFrames[2] = {};
    uint8_t recentInputCount = 0;
    uint8_t recentRollbackCount = 0;
    RecentInputSample recentInputs[kCrashRecentInputCapacity] = {};
    RecentRollbackDecision
        recentRollback[kCrashRecentRollbackCapacity] = {};
};

struct CopiedExceptionContext {
    DWORD exceptionCode = 0;
    uintptr_t exceptionAddress = 0;
    AccessOperation operation = AccessOperation::Unknown;
    uintptr_t faultAddress = 0;
    uintptr_t moduleBase = 0;
    uintptr_t moduleRva = 0;
    char moduleName[128] = {};
#ifdef _M_IX86
    uint32_t eip = 0;
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    uint32_t esi = 0;
    uint32_t edi = 0;
    uint32_t ebp = 0;
    uint32_t esp = 0;
#endif
    EXCEPTION_RECORD exceptionRecord{};
    CONTEXT contextRecord{};
};

struct CapturedCrashContext {
    uint32_t schemaVersion = kCrashContextSchemaVersion;
    uint32_t sequence = 0;
    CopiedExceptionContext exception{};
    OwnedTickCrashSnapshot tick{};
    uint8_t identityAvailable = 0;
    qnetplay::compat::CompatibilityIdentityWire compatibilityIdentity{};
};

bool captureExceptionContext(const EXCEPTION_POINTERS* exceptionPointers,
                             const OwnedTickCrashSnapshot& tick,
                             const qnetplay::compat::CompatibilityIdentityWire*
                                 compatibilityIdentity,
                             CapturedCrashContext& out,
                             CrashModuleResolver resolver = nullptr,
                             void* resolverContext = nullptr) noexcept;

void cacheCompatibilityIdentity(
    const qnetplay::compat::CompatibilityIdentityWire& identity) noexcept;
void clearCachedCompatibilityIdentityForTests() noexcept;
bool copyCachedCompatibilityIdentity(
    qnetplay::compat::CompatibilityIdentityWire& out) noexcept;

class BoundedCrashHistory {
public:
    static constexpr size_t kCapacity = 8u;

    void append(const CapturedCrashContext& context) noexcept;
    size_t size() const noexcept { return size_; }
    const CapturedCrashContext& newest(size_t indexFromNewest) const noexcept;
    void clear() noexcept;

private:
    CapturedCrashContext records_[kCapacity] = {};
    size_t next_ = 0;
    size_t size_ = 0;
};

using CopiedMiniDumpWriter = bool (*)(const EXCEPTION_RECORD&,
                                      const CONTEXT&,
                                      void* context);
bool invokeCopiedMiniDumpWriter(const CapturedCrashContext& captured,
                                CopiedMiniDumpWriter writer,
                                void* context) noexcept;

bool tryEnterCrashReporter() noexcept;
void leaveCrashReporter() noexcept;

}
