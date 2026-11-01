#pragma once

#include <cstddef>
#include <vector>

namespace qnetplay::test {

using TestFn = void (*)();

struct TestCase {
    const char* name;
    TestFn function;
};

std::vector<TestCase>& registry();
int& failedChecks();
void recordFailure(const char* file, int line, const char* expression);

struct Registrar {
    Registrar(const char* name, TestFn function);
};

}

#define QNP_TEST(name) \
    static void test_##name(); \
    static ::qnetplay::test::Registrar register_##name(#name, &test_##name); \
    static void test_##name()

#define QNP_CHECK(expression) \
    do { \
        if (!(expression)) \
            ::qnetplay::test::recordFailure(__FILE__, __LINE__, #expression); \
    } while (0)

#define QNP_REQUIRE(expression) \
    do { \
        if (!(expression)) { \
            ::qnetplay::test::recordFailure(__FILE__, __LINE__, #expression); \
            return; \
        } \
    } while (0)
