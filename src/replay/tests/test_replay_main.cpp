#include "TestHarness.h"

#include <cstdio>

#if defined(_WIN32) && defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace qnetplay::test {

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

int& failedChecks() {
    static int failures = 0;
    return failures;
}

void recordFailure(const char* file, int line, const char* expression) {
    std::printf("    CHECK FAILED %s:%d  %s\n", file, line, expression);
    ++failedChecks();
}

Registrar::Registrar(const char* name, TestFn function) {
    registry().push_back(TestCase{ name, function });
}

#if defined(_WIN32) && defined(_MSC_VER)
bool runWithoutUnhandledSeh(TestFn function,
                            unsigned long& exceptionCode) {
    __try {
        function();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        exceptionCode = GetExceptionCode();
        return false;
    }
}
#endif

}

int main() {
    using namespace qnetplay::test;
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    int failedTests = 0;
    std::printf("Running %zu replay core tests\n\n", registry().size());
    for (const TestCase& test : registry()) {
        const int before = failedChecks();
        std::printf("[ RUN  ] %s\n", test.name);
#if defined(_WIN32) && defined(_MSC_VER)
        unsigned long exceptionCode = 0;
        if (!runWithoutUnhandledSeh(test.function, exceptionCode)) {
            std::printf(
                "    STRUCTURED EXCEPTION 0x%08lX in %s\n",
                exceptionCode, test.name);
            ++failedChecks();
            ++failedTests;
            std::printf("[FAIL ] %s\n", test.name);
            std::printf(
                "\nTest process stopped safely after an exception; "
                "%d checks failed\n",
                failedChecks());
            return 2;
        }
#else
        test.function();
#endif
        const bool passed = failedChecks() == before;
        std::printf("[ %s ] %s\n", passed ? " OK " : "FAIL", test.name);
        if (!passed) ++failedTests;
    }

    std::printf("\n%d/%zu tests passed, %d checks failed\n",
                static_cast<int>(registry().size()) - failedTests,
                registry().size(),
                failedChecks());
    return failedTests == 0 ? 0 : 1;
}
