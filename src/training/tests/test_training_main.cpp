#include "netplay/tests/TestHarness.h"

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

}

int main() {
    using namespace qnetplay::test;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failedTests = 0;
    std::printf("Running %zu training core tests\n\n", registry().size());
    for (const TestCase& test : registry()) {
        const int before = failedChecks();
        std::printf("[ RUN  ] %s\n", test.name);
        test.function();
        const bool passed = failedChecks() == before;
        std::printf("[ %s ] %s\n", passed ? " OK " : "FAIL", test.name);
        if (!passed) ++failedTests;
    }
    std::printf("\n%d/%zu tests passed, %d checks failed\n",
                static_cast<int>(registry().size()) - failedTests,
                registry().size(), failedChecks());
    return failedTests == 0 ? 0 : 1;
}
