#pragma once

#include <cstddef>
#include <cstdint>

namespace qnet {

enum class SecureRandomBackend : uint8_t {
    None = 0,
    BCrypt = 1,
    RtlGen = 2,
    LastResort = 3,
};

constexpr size_t kSecureRandomAllZeroRejectBytes = 8;

bool secureRandomBytes(void* out, size_t len, bool& degraded);

uint64_t secureRandom64(bool& degraded);

const char* secureRandomBackendName();
const char* secureRandomBackendName(SecureRandomBackend backend);

SecureRandomBackend secureRandomBackend();

bool secureRandomBytesFrom(SecureRandomBackend backend, void* out, size_t len,
                           bool& degraded);

}
