#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace testfw {

struct Case { const char* name; std::function<void()> fn; };

inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
inline const char*& currentTest() { static const char* n = ""; return n; }

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        cases().push_back({ name, std::move(fn) });
    }
};

inline void reportFail(const char* file, int line, const std::string& msg) {
    std::printf("[FAIL] %s (%s:%d): %s\n", currentTest(), file, line, msg.c_str());
    ++failures();
}

inline int runAll() {
    int ran = 0;
    for (auto& c : cases()) {
        currentTest() = c.name;
        int before = failures();
        c.fn();
        ++ran;
        if (failures() == before)
            std::printf("[ ok ] %s\n", c.name);
    }
    std::printf("%d tests ran, %d failures\n", ran, failures());
    if (failures() == 0)
        std::printf("ALL %d tests passed\n", ran);
    return failures() ? 1 : 0;
}

}

#define TEST(name) \
    static void test_##name(); \
    static testfw::Registrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond) \
    do { if (!(cond)) testfw::reportFail(__FILE__, __LINE__, #cond); } while (0)

#define CHECK_EQ(a, b) \
    do { auto va_ = (a); auto vb_ = (b); \
         if (!(va_ == vb_)) testfw::reportFail(__FILE__, __LINE__, \
             std::string(#a " == " #b " (got ") + std::to_string((long long)va_) + \
             " vs " + std::to_string((long long)vb_) + ")"); } while (0)
