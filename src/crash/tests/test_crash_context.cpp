#include "../../CrashContext.h"

#include <cstdio>
#include <cstring>

namespace {

int g_failures = 0;

#define CRASH_CHECK(expression) \
    do { \
        if (!(expression)) { \
            std::printf("    CHECK FAILED %s:%d  %s\n", \
                        __FILE__, __LINE__, #expression); \
            ++g_failures; \
        } \
    } while (0)

qnetplay::compat::CompatibilityIdentityWire fixtureIdentity() {
    qnetplay::compat::CompatibilityIdentityWire identity{};
    identity.schemaVersion =
        qnetplay::compat::kCompatibilityIdentitySchemaVersion;
    identity.protocolVersion = 1u;
    (void)qnetplay::compat::setProfileId(identity, "fixture-profile");
    for (size_t index = 0u;
         index < qnetplay::compat::kCompatibilitySha256Bytes; ++index) {
        identity.runtimeBuild.executableSha256[index] =
            static_cast<uint8_t>(0x10u + index);
        identity.simulationContent.simulationRootSha256[index] =
            static_cast<uint8_t>(0x40u + index);
        identity.simulationContent.validationRootSha256[index] =
            static_cast<uint8_t>(0x80u + index);
    }
    identity.runtimeBuild.executableSize = 1u;
    identity.simulationContent.manifestSchemaVersion = 1u;
    identity.simulationContent.manifestFlags =
        qoh99::content::ManifestFlagComplete;
    identity.simulationContent.entryCount = 1u;
    identity.simulationContent.requiredEntryCount = 1u;
    identity.stateSchemaId = 1u;
    identity.modSimulationId = 1u;
    identity.timingProfileId = 1u;
    return identity;
}

bool fakeModuleResolver(uintptr_t address, qcrash::CrashModuleInfo& out,
                        void*) {
    if (address < 0x401000u || address >= 0x402000u) return false;
    out.base = 0x400000u;
    std::strcpy(out.name, "qoh99.exe");
    return true;
}

qcrash::CapturedCrashContext captureFixture(uintptr_t accessKind,
                                             const qnetplay::compat::CompatibilityIdentityWire*
                                                 identity = nullptr) {
    EXCEPTION_RECORD chained{};
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    record.ExceptionRecord = &chained;
    record.ExceptionAddress = reinterpret_cast<void*>(0x401234u);
    record.NumberParameters = 2u;
    record.ExceptionInformation[0] = accessKind;
    record.ExceptionInformation[1] = 0xDEADBEEFu;
    CONTEXT registers{};
#ifdef _M_IX86
    registers.Eip = 0x401234u;
    registers.Eax = 0x01020304u;
    registers.Ebx = 0x11121314u;
    registers.Ecx = 0x21222324u;
    registers.Edx = 0x31323334u;
    registers.Esi = 0x41424344u;
    registers.Edi = 0x51525354u;
    registers.Ebp = 0x61626364u;
    registers.Esp = 0x71727374u;
#endif
    EXCEPTION_POINTERS pointers{ &record, &registers };
    qcrash::OwnedTickCrashSnapshot snapshot{};
    snapshot.frame = 100u;
    snapshot.canonicalFrame = 100u;
    snapshot.confirmedFrame = 96u;
    snapshot.rollbackOrigin = 97u;
    snapshot.rollbackDepth = 3u;
    snapshot.speculativeDepth = 2u;
    snapshot.lifecycleState = 2u;
    snapshot.characterIds[0] = 4u;
    snapshot.characterIds[1] = 7u;
    snapshot.recentInputCount = 1u;
    snapshot.recentInputs[0] = {100u, 0x11u, 0x22u};
    qcrash::CapturedCrashContext captured{};
    CRASH_CHECK(qcrash::captureExceptionContext(
        &pointers, snapshot, identity, captured, fakeModuleResolver, nullptr));
    return captured;
}

void testAccessClassificationAndCopiedRegisters() {
    const auto read = captureFixture(0u);
    const auto write = captureFixture(1u);
    const auto execute = captureFixture(8u);
    CRASH_CHECK(read.exception.operation == qcrash::AccessOperation::Read);
    CRASH_CHECK(write.exception.operation == qcrash::AccessOperation::Write);
    CRASH_CHECK(execute.exception.operation == qcrash::AccessOperation::Execute);
    CRASH_CHECK(read.exception.faultAddress == 0xDEADBEEFu);
    CRASH_CHECK(read.exception.exceptionRecord.ExceptionRecord == nullptr);
    CRASH_CHECK(read.tick.frame == 100u);
    CRASH_CHECK(read.tick.recentInputs[0].playerTwo == 0x22u);
#ifdef _M_IX86
    CRASH_CHECK(read.exception.eip == 0x401234u);
    CRASH_CHECK(read.exception.eax == 0x01020304u);
    CRASH_CHECK(read.exception.esp == 0x71727374u);
#endif
}

void testModuleRvaMapping() {
    const auto captured = captureFixture(0u);
    CRASH_CHECK(captured.exception.moduleBase == 0x400000u);
    CRASH_CHECK(captured.exception.moduleRva == 0x1234u);
    CRASH_CHECK(std::strcmp(captured.exception.moduleName, "qoh99.exe") == 0);
}

void testIdentityAvailability() {
    qcrash::clearCachedCompatibilityIdentityForTests();
    qnetplay::compat::CompatibilityIdentityWire identity{};
    CRASH_CHECK(!qcrash::copyCachedCompatibilityIdentity(identity));
    const auto early = captureFixture(0u, nullptr);
    CRASH_CHECK(early.identityAvailable == 0u);

    const auto expected = fixtureIdentity();
    qcrash::cacheCompatibilityIdentity(expected);
    CRASH_CHECK(qcrash::copyCachedCompatibilityIdentity(identity));
    const auto initialized = captureFixture(0u, &identity);
    CRASH_CHECK(initialized.identityAvailable == 1u);
    CRASH_CHECK(qnetplay::compat::semanticallyEqual(
        expected, initialized.compatibilityIdentity));
    qcrash::clearCachedCompatibilityIdentityForTests();
}

bool successfulDumpWriter(const EXCEPTION_RECORD& record,
                          const CONTEXT& context, void* state) {
    bool* called = static_cast<bool*>(state);
    *called = true;
    return record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION
#ifdef _M_IX86
        && context.Eip == 0x401234u
#endif
        ;
}

bool failedDumpWriter(const EXCEPTION_RECORD&, const CONTEXT&, void*) {
    return false;
}

void testMiniDumpWriterAndBoundedHistory() {
    const auto captured = captureFixture(0u);
    bool called = false;
    CRASH_CHECK(qcrash::invokeCopiedMiniDumpWriter(
        captured, successfulDumpWriter, &called));
    CRASH_CHECK(called);
    CRASH_CHECK(!qcrash::invokeCopiedMiniDumpWriter(
        captured, failedDumpWriter, nullptr));
    CRASH_CHECK(!qcrash::invokeCopiedMiniDumpWriter(captured, nullptr, nullptr));

    qcrash::BoundedCrashHistory history{};
    for (uint32_t sequence = 0u; sequence < 12u; ++sequence) {
        qcrash::CapturedCrashContext item = captured;
        item.sequence = sequence;
        history.append(item);
    }
    CRASH_CHECK(history.size() == qcrash::BoundedCrashHistory::kCapacity);
    CRASH_CHECK(history.newest(0u).sequence == 11u);
    CRASH_CHECK(history.newest(7u).sequence == 4u);
    CRASH_CHECK(history.newest(8u).sequence == 0u);
}

void testRecursiveReporterSuppression() {
    qcrash::leaveCrashReporter();
    CRASH_CHECK(qcrash::tryEnterCrashReporter());
    CRASH_CHECK(!qcrash::tryEnterCrashReporter());
    qcrash::leaveCrashReporter();
    CRASH_CHECK(qcrash::tryEnterCrashReporter());
    qcrash::leaveCrashReporter();
}

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("Running copied crash-context tests\n\n");
    testAccessClassificationAndCopiedRegisters();
    testModuleRvaMapping();
    testIdentityAvailability();
    testMiniDumpWriterAndBoundedHistory();
    testRecursiveReporterSuppression();
    std::printf("\n%d crash-context checks failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
