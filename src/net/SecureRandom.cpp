#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "SecureRandom.h"

#include <cstring>

namespace qnet {
namespace {

using BCryptGenRandomFn = LONG(WINAPI*)(void* algorithm, unsigned char* buffer,
                                        unsigned long bytes,
                                        unsigned long flags);
using SystemFunction036Fn = BOOLEAN(WINAPI*)(void* buffer,
                                             unsigned long bytes);

constexpr unsigned long kUseSystemPreferredRng = 0x00000002ul;

constexpr unsigned long kMaxDrawChunk = 1u << 20;

struct Providers {
    BCryptGenRandomFn bcrypt = nullptr;
    SystemFunction036Fn rtl = nullptr;
    SecureRandomBackend backend = SecureRandomBackend::LastResort;
};

bool allZero(const void* data, size_t len) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] != 0) return false;
    }
    return true;
}

bool drawBCrypt(BCryptGenRandomFn fn, void* out, size_t len) {
    auto* bytes = static_cast<unsigned char*>(out);
    size_t done = 0;
    while (done < len) {
        const size_t remain = len - done;
        const unsigned long chunk = remain > kMaxDrawChunk
            ? kMaxDrawChunk
            : static_cast<unsigned long>(remain);
        if (fn(nullptr, bytes + done, chunk, kUseSystemPreferredRng) < 0)
            return false;
        done += chunk;
    }
    return true;
}

bool drawRtl(SystemFunction036Fn fn, void* out, size_t len) {
    auto* bytes = static_cast<unsigned char*>(out);
    size_t done = 0;
    while (done < len) {
        const size_t remain = len - done;
        const unsigned long chunk = remain > kMaxDrawChunk
            ? kMaxDrawChunk
            : static_cast<unsigned long>(remain);
        if (!fn(bytes + done, chunk)) return false;
        done += chunk;
    }
    return true;
}

uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

bool drawLastResort(void* out, size_t len) {
    static volatile LONG counter = 0;
    static const uint64_t processSalt = [] {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        uint64_t seed = static_cast<uint64_t>(qpc.QuadPart);
        seed ^= (static_cast<uint64_t>(ft.dwHighDateTime) << 32)
              | static_cast<uint64_t>(ft.dwLowDateTime);
        seed ^= static_cast<uint64_t>(GetCurrentProcessId())
              * 0x9E3779B97F4A7C15ull;
        return splitmix64(seed);
    }();

    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);

    uint64_t state = processSalt;
    state ^= static_cast<uint64_t>(qpc.QuadPart);
    state ^= (static_cast<uint64_t>(ft.dwHighDateTime) << 32)
           | static_cast<uint64_t>(ft.dwLowDateTime);
    state ^= static_cast<uint64_t>(GetCurrentThreadId()) << 17;
    state ^= static_cast<uint64_t>(GetTickCount64());
    state ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&state));
    state ^= static_cast<uint64_t>(
                 static_cast<unsigned long>(InterlockedIncrement(&counter)))
           * 0xD1342543DE82EF95ull;

    auto* bytes = static_cast<unsigned char*>(out);
    size_t written = 0;
    while (written < len) {
        const uint64_t block = splitmix64(state);
        const size_t chunk = (len - written) < sizeof(block)
            ? (len - written)
            : sizeof(block);
        std::memcpy(bytes + written, &block, chunk);
        written += chunk;
    }
    return true;
}

const Providers& providers() {
    // Must never run during DllMain: LoadLibraryW here would deadlock on the loader lock.
    static const Providers resolved = [] {
        Providers found;
        if (HMODULE module = ::LoadLibraryW(L"bcrypt.dll")) {
            found.bcrypt = reinterpret_cast<BCryptGenRandomFn>(
                ::GetProcAddress(module, "BCryptGenRandom"));
        }
        if (HMODULE module = ::LoadLibraryW(L"advapi32.dll")) {
            found.rtl = reinterpret_cast<SystemFunction036Fn>(
                ::GetProcAddress(module, "SystemFunction036"));
        }

        unsigned char probe[8] = {};
        if (found.bcrypt && drawBCrypt(found.bcrypt, probe, sizeof(probe))) {
            found.backend = SecureRandomBackend::BCrypt;
        } else if (found.rtl && drawRtl(found.rtl, probe, sizeof(probe))) {
            found.backend = SecureRandomBackend::RtlGen;
        } else {
            found.backend = SecureRandomBackend::LastResort;
        }
        SecureZeroMemory(probe, sizeof(probe));
        return found;
    }();
    return resolved;
}

}

const char* secureRandomBackendName(SecureRandomBackend backend) {
    switch (backend) {
    case SecureRandomBackend::None:       return "none";
    case SecureRandomBackend::BCrypt:     return "BCryptGenRandom";
    case SecureRandomBackend::RtlGen:     return "RtlGenRandom";
    case SecureRandomBackend::LastResort: return "last-resort mix (DEGRADED)";
    }
    return "unknown";
}

SecureRandomBackend secureRandomBackend() {
    return providers().backend;
}

const char* secureRandomBackendName() {
    return secureRandomBackendName(secureRandomBackend());
}

bool secureRandomBytesFrom(SecureRandomBackend backend, void* out, size_t len,
                           bool& degraded) {
    degraded = backend == SecureRandomBackend::LastResort;
    if (len == 0) return true;
    if (out == nullptr) return false;

    const Providers& available = providers();
    for (int attempt = 0; attempt < 4; ++attempt) {
        bool ok = false;
        switch (backend) {
        case SecureRandomBackend::BCrypt:
            ok = available.bcrypt != nullptr
                 && drawBCrypt(available.bcrypt, out, len);
            break;
        case SecureRandomBackend::RtlGen:
            ok = available.rtl != nullptr && drawRtl(available.rtl, out, len);
            break;
        case SecureRandomBackend::LastResort:
            ok = drawLastResort(out, len);
            break;
        case SecureRandomBackend::None:
        default:
            break;
        }
        if (!ok) break;
        if (len < kSecureRandomAllZeroRejectBytes || !allZero(out, len))
            return true;
    }
    std::memset(out, 0, len);
    return false;
}

bool secureRandomBytes(void* out, size_t len, bool& degraded) {
    return secureRandomBytesFrom(secureRandomBackend(), out, len, degraded);
}

uint64_t secureRandom64(bool& degraded) {
    uint64_t value = 0;
    if (!secureRandomBytes(&value, sizeof(value), degraded)) {
        degraded = true;
        return 0;
    }
    return value;
}

}
