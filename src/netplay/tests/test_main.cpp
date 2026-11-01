#include "TestHarness.h"

#include "../../net/Sockets.h"

#include <cstdio>

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
    if (!qnet::netInit()) {
        std::printf("WSAStartup failed\n");
        return 1;
    }

    int failedTests = 0;
    std::printf("Running %zu network P0 tests\n\n", registry().size());
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
            qnet::netShutdown();
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
    qnet::netShutdown();
    return failedTests == 0 ? 0 : 1;
}
