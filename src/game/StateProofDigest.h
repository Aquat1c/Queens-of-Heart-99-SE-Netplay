#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace qgame {

inline constexpr uint32_t kStateProofP1 = 2654435761u;
inline constexpr uint32_t kStateProofP2 = 2246822519u;
inline constexpr uint32_t kStateProofP3 = 3266489917u;
inline constexpr uint32_t kStateProofP4 = 668265263u;
inline constexpr uint32_t kStateProofP5 = 374761393u;

inline uint32_t stateProofRotl(const uint32_t x, const int r) {
    return (x << r) | (x >> (32 - r));
}

inline uint32_t stateProofRead32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline uint64_t stateProofDigest64(const uint8_t* p, const size_t n) {
    const uint8_t* const end = p + n;
    uint32_t a = kStateProofP1 + kStateProofP2;
    uint32_t b = kStateProofP2;
    uint32_t c = 0u;
    uint32_t d = 0u - kStateProofP1;
    while (static_cast<size_t>(end - p) >= 16u) {
        a = stateProofRotl(
                a + stateProofRead32(p + 0) * kStateProofP2, 13)
            * kStateProofP1;
        b = stateProofRotl(
                b + stateProofRead32(p + 4) * kStateProofP2, 13)
            * kStateProofP1;
        c = stateProofRotl(
                c + stateProofRead32(p + 8) * kStateProofP2, 13)
            * kStateProofP1;
        d = stateProofRotl(
                d + stateProofRead32(p + 12) * kStateProofP2, 13)
            * kStateProofP1;
        p += 16;
    }
    uint32_t h = stateProofRotl(a, 1) + stateProofRotl(b, 7)
               + stateProofRotl(c, 12) + stateProofRotl(d, 18);
    h += static_cast<uint32_t>(n);
    while (static_cast<size_t>(end - p) >= 4u) {
        h += stateProofRead32(p) * kStateProofP3;
        h = stateProofRotl(h, 17) * kStateProofP4;
        p += 4;
    }
    while (p < end) {
        h += static_cast<uint32_t>(*p++) * kStateProofP5;
        h = stateProofRotl(h, 11) * kStateProofP1;
    }
    h ^= h >> 15; h *= kStateProofP2;
    h ^= h >> 13; h *= kStateProofP3;
    h ^= h >> 16;
    uint32_t g = (a ^ stateProofRotl(b, 9)) + (c ^ stateProofRotl(d, 21))
               + static_cast<uint32_t>(n);
    g ^= g >> 16; g *= kStateProofP1;
    g ^= g >> 13; g *= kStateProofP4;
    g ^= g >> 16;
    return (static_cast<uint64_t>(g) << 32) | h;
}

}
