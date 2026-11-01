#include "TestHarness.h"
#include "replay/ReplaySeekCache.h"

using qreplay::ReplaySeekCache;

QNP_TEST(ReplaySeekCacheSelectsTheNearestEarlierState) {
    ReplaySeekCache<int> cache(32u);
    QNP_CHECK(!cache.wants(0u));
    QNP_CHECK(!cache.wants(15u));
    QNP_REQUIRE(cache.wants(16u));
    QNP_REQUIRE(cache.insert(16u, 8u, std::make_unique<int>(101)));
    QNP_REQUIRE(cache.insert(32u, 8u, std::make_unique<int>(202)));
    QNP_CHECK(cache.find(15u) == nullptr);
    QNP_REQUIRE(cache.find(16u));
    QNP_CHECK(*cache.find(16u)->state == 101);
    QNP_CHECK(*cache.find(31u)->state == 101);
    QNP_CHECK(*cache.find(32u)->state == 202);
    cache.discardAfter(16u);
    QNP_CHECK(*cache.find(32u)->state == 101);
    QNP_REQUIRE(cache.insert(32u, 8u, std::make_unique<int>(303)));
    QNP_CHECK(*cache.find(32u)->state == 303);
    cache.clear();
    QNP_CHECK(cache.bytes() == 0u);
    QNP_CHECK(cache.find(32u) == nullptr);
    QNP_CHECK(cache.wants(16u));
}

QNP_TEST(ReplaySeekCacheKeepsHistoricalCoverageWithinItsMemoryBudget) {
    ReplaySeekCache<int> cache(32u);
    for (uint32_t cursor = 16u; cursor <= 160u; cursor += 16u) {
        if (cache.wants(cursor))
            cache.insert(cursor, 8u, std::make_unique<int>(cursor));
        QNP_CHECK(cache.bytes() <= 32u);
        for (uint32_t target = 0u; target <= cursor; ++target) {
            const auto* entry = cache.find(target);
            const uint32_t restored = entry ? entry->nextRecord : 0u;
            QNP_CHECK(restored <= target);
            QNP_CHECK(target - restored < cache.stride());
        }
    }
    QNP_REQUIRE(cache.find(159u));
    QNP_CHECK(*cache.find(159u)->state == 128);
    const auto used = cache.bytes();
    QNP_CHECK(!cache.insert(192u, 33u, std::make_unique<int>(192)));
    QNP_CHECK(cache.bytes() == used);
    QNP_CHECK(*cache.find(159u)->state == 128);
    cache.clear();
    QNP_CHECK(cache.wants(16u));
}
