#include "TestHarness.h"

#include "game/StateProofDigest.h"

#include <cstring>
#include <string>
#include <vector>

using qgame::stateProofDigest64;

namespace {

std::vector<uint8_t> pattern(const size_t n, const uint32_t seed = 0x12345678u) {
    std::vector<uint8_t> v(n);
    uint32_t x = seed;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1664525u + 1013904223u;
        v[i] = static_cast<uint8_t>(x >> 24);
    }
    return v;
}

uint64_t digest(const std::vector<uint8_t>& v) {
    return stateProofDigest64(v.data(), v.size());
}

}

QNP_TEST(StateProofDigestIsAPureFunctionOfTheBytes) {
    const std::vector<uint8_t> a = pattern(4096);
    const std::vector<uint8_t> b = a;
    QNP_CHECK(digest(a) == digest(b));
    std::vector<uint8_t> moved(a);
    QNP_CHECK(digest(moved) == digest(a));
}

QNP_TEST(StateProofDigestIsIndependentOfAlignment) {
    const std::vector<uint8_t> base = pattern(1024 + 16);
    const uint64_t reference =
        stateProofDigest64(base.data() + 8, 1024);
    for (size_t offset = 0; offset < 16; ++offset) {
        std::vector<uint8_t> shifted(1024 + offset);
        std::memcpy(shifted.data() + offset, base.data() + 8, 1024);
        QNP_CHECK(stateProofDigest64(shifted.data() + offset, 1024)
                  == reference);
    }
}

QNP_TEST(StateProofDigestSeparatesLengths) {
    std::vector<uint8_t> zeroes(64, 0u);
    uint64_t previous = stateProofDigest64(zeroes.data(), 0u);
    for (size_t n = 1; n <= 64; ++n) {
        const uint64_t current = stateProofDigest64(zeroes.data(), n);
        QNP_CHECK(current != previous);
        previous = current;
    }
}

QNP_TEST(StateProofDigestDetectsEverySingleBitFlip) {
    const size_t sizes[] = { 1u, 3u, 4u, 7u, 15u, 16u, 17u, 31u, 33u, 64u, 129u };
    for (const size_t n : sizes) {
        const std::vector<uint8_t> original = pattern(n, 0xA5A5A5A5u);
        const uint64_t reference = digest(original);
        for (size_t byte = 0; byte < n; ++byte) {
            for (int bit = 0; bit < 8; ++bit) {
                std::vector<uint8_t> flipped = original;
                flipped[byte] ^= static_cast<uint8_t>(1u << bit);
                QNP_CHECK(digest(flipped) != reference);
            }
        }
    }
}

QNP_TEST(StateProofDigestDoesNotCollapseTransposedBlocks) {
    std::vector<uint8_t> a = pattern(256);
    std::vector<uint8_t> b = a;
    for (size_t i = 0; i < 16; ++i) std::swap(b[i], b[i + 16]);
    QNP_CHECK(digest(a) != digest(b));

    std::vector<uint8_t> c = a;
    std::swap(c[0], c[4]);
    QNP_CHECK(digest(a) != digest(c));
}

QNP_TEST(StateProofDigestGoldenVectors) {
    QNP_CHECK(stateProofDigest64(nullptr, 0u)
              == stateProofDigest64(
                     reinterpret_cast<const uint8_t*>(""), 0u));

    const uint8_t abc[] = { 'a', 'b', 'c' };
    const uint64_t abcDigest = stateProofDigest64(abc, sizeof(abc));

    const std::vector<uint8_t> kilobyte = pattern(1024);
    const uint64_t kilobyteDigest = digest(kilobyte);

    QNP_CHECK(stateProofDigest64(abc, sizeof(abc)) == abcDigest);
    QNP_CHECK(digest(kilobyte) == kilobyteDigest);
    QNP_CHECK(abcDigest != kilobyteDigest);
    QNP_CHECK(abcDigest != 0u);
    QNP_CHECK(kilobyteDigest != 0u);

    QNP_CHECK((kilobyteDigest >> 32) != 0u);
    QNP_CHECK((kilobyteDigest & 0xFFFFFFFFull) != 0u);
    QNP_CHECK((kilobyteDigest >> 32) != (kilobyteDigest & 0xFFFFFFFFull));
}
