#include "TestHarness.h"

#include "net/MonotonicTime.h"
#include "net/RollingWindowStats.h"
#include "net/RouteRttEstimator.h"

using namespace qnet;

QNP_TEST(TicksToMicrosIsExactAtZeroAndOrdinaryUptime) {
    constexpr int64_t f = 10000000;
    QNP_CHECK(ticksToMicros(0, f) == 0);
    QNP_CHECK(ticksToMicros(f, f) == 1000000);
    QNP_CHECK(ticksToMicros(f / 2, f) == 500000);
    QNP_CHECK(ticksToMicros(f / 1000, f) == 1000);
    QNP_CHECK(ticksToMillis(f, f) == 1000);
    QNP_CHECK(ticksToMillis(f / 2, f) == 500);
    QNP_CHECK(ticksToMicros(-5, f) == 0);
    QNP_CHECK(ticksToMicros(f, 0) == 0);
}

QNP_TEST(TicksToMicrosSurvivesTheNaiveMultiplicationOverflow) {
    constexpr int64_t f = 10000000;
    const int64_t ticks = 10000000LL * 1000000LL;
    QNP_CHECK(ticksToMicros(ticks, f) == 1000000000000LL);
    QNP_CHECK(ticksToMicros(ticks + f / 2, f) == 1000000500000LL);
    QNP_CHECK(ticksToMicros(9000000000000000000LL, f) == 900000000000000000LL);
}

QNP_TEST(RollingWindowPercentilesAreNearestRank) {
    RollingWindowStats<128> s;
    for (int64_t v = 1; v <= 100; ++v) s.add(v);
    QNP_CHECK(s.count() == 100);
    QNP_CHECK(s.minimum() == 1);
    QNP_CHECK(s.maximum() == 100);
    QNP_CHECK(s.mean() == 50);
    QNP_CHECK(s.median() == 50);
    QNP_CHECK(s.percentile(50) == 50);
    QNP_CHECK(s.percentile(90) == 90);
    QNP_CHECK(s.percentile(95) == 95);
    QNP_CHECK(s.percentile(99) == 99);
    QNP_CHECK(s.percentile(0) == 1);
    QNP_CHECK(s.percentile(100) == 100);
}

QNP_TEST(OneLargeOutlierDoesNotBecomeTheP95Delay) {
    RollingWindowStats<128> s;
    for (int i = 0; i < 32; ++i) s.add(10);
    s.add(10000);
    QNP_CHECK(s.maximum() == 10000);
    QNP_CHECK(s.percentile(95) == 10);
    QNP_CHECK(s.median() == 10);
    QNP_CHECK(s.mad() == 0);
}

QNP_TEST(OneZeroSampleCannotEraseAHighTail) {
    RollingWindowStats<128> s;
    for (int i = 0; i < 40; ++i) s.add(100);
    s.add(0);
    QNP_CHECK(s.minimum() == 0);
    QNP_CHECK(s.percentile(95) == 100);
    QNP_CHECK(s.median() == 100);
}

QNP_TEST(RollingWindowIsBoundedAndOverwritesOldest) {
    RollingWindowStats<4> s;
    s.add(1);
    s.add(2);
    s.add(3);
    s.add(4);
    QNP_CHECK(s.count() == 4 && s.minimum() == 1 && s.maximum() == 4);
    s.add(5);
    s.add(6);
    QNP_CHECK(s.count() == 4);
    QNP_CHECK(s.minimum() == 3 && s.maximum() == 6);
    QNP_CHECK(s.mean() == 4);
}

QNP_TEST(ResetClearsTheWindow) {
    RollingWindowStats<32> s;
    for (int i = 0; i < 20; ++i) s.add(500);
    QNP_CHECK(s.count() == 20 && s.percentile(95) == 500);
    s.reset();
    QNP_CHECK(s.empty() && s.count() == 0);
    QNP_CHECK(s.percentile(95) == 0 && s.maximum() == 0 && s.mean() == 0);
    s.add(7);
    QNP_CHECK(s.count() == 1 && s.percentile(95) == 7);
}

QNP_TEST(StddevAndMadAgreeOnCleanData) {
    RollingWindowStats<128> s;
    for (int r = 0; r < 10; ++r)
        for (int64_t v = 8; v <= 12; ++v) s.add(v);
    QNP_CHECK(s.mean() == 10);
    QNP_CHECK(s.median() == 10);
    QNP_CHECK(s.stddev() >= 1 && s.stddev() <= 2);
    QNP_CHECK(s.mad() >= 1 && s.mad() <= 2);
}

QNP_TEST(RouteEstimatorUniqueReplyProducesSample) {
    RouteRttEstimator<64, 128> est;
    const uint32_t seq = est.onProbeSent(0);
    const ReplyOutcome o = est.onReply(seq, 0, est.generation(),
                                       8000, 0);
    QNP_CHECK(o == ReplyOutcome::Sample);
    QNP_CHECK(est.hasRtt());
    QNP_CHECK(est.p50Micros() == 8000);
    QNP_CHECK(est.counts().uniqueReplies == 1 && est.counts().sent == 1);
    QNP_CHECK(est.inFlight() == 0);
}

QNP_TEST(RouteEstimatorSubtractsResponderDwell) {
    RouteRttEstimator<64, 128> est;
    const uint32_t seq = est.onProbeSent(0);
    est.onReply(seq, 0, est.generation(), 10000, 3000);
    QNP_CHECK(est.p50Micros() == 7000);
}

QNP_TEST(RouteEstimatorDuplicateReplyIsNotResampled) {
    RouteRttEstimator<64, 128> est;
    const uint32_t seq = est.onProbeSent(0);
    QNP_CHECK(est.onReply(seq, 0, est.generation(), 8000, 0) == ReplyOutcome::Sample);
    QNP_CHECK(est.onReply(seq, 0, est.generation(), 9000, 0) == ReplyOutcome::Duplicate);
    QNP_CHECK(est.counts().uniqueReplies == 1 && est.counts().duplicates == 1);
    QNP_CHECK(est.rtt().count() == 1);
    QNP_CHECK(est.onReply(999u, 0, est.generation(), 9000, 0) == ReplyOutcome::Duplicate);
}

QNP_TEST(RouteEstimatorRetransmittedProbeIsAmbiguous) {
    RouteRttEstimator<64, 128> est;
    const uint32_t seq = est.onProbeSent(0);
    est.onProbeRetransmitted(seq);
    const ReplyOutcome o = est.onReply(seq, 0, est.generation(), 8000, 0);
    QNP_CHECK(o == ReplyOutcome::Ambiguous);
    QNP_CHECK(!est.hasRtt());
    QNP_CHECK(est.counts().ambiguous == 1 && est.counts().uniqueReplies == 0);
}

QNP_TEST(RouteEstimatorResetRejectsOldGenerationReply) {
    RouteRttEstimator<64, 128> est;
    const uint32_t oldGen = est.generation();
    const uint32_t seq = est.onProbeSent(0);
    est.reset();
    QNP_CHECK(est.generation() != oldGen);
    QNP_CHECK(est.onReply(seq, 0, oldGen, 8000, 0) == ReplyOutcome::Stale);
    QNP_CHECK(!est.hasRtt() && est.counts().stale == 1);
    QNP_CHECK(est.counts().uniqueReplies == 0);
}

QNP_TEST(RouteEstimatorRejectsMismatchedEchoStamp) {
    RouteRttEstimator<64, 128> est;
    const uint32_t seq = est.onProbeSent(1000);
    QNP_CHECK(est.onReply(seq, 999, est.generation(), 8000, 0)
              == ReplyOutcome::Stale);
    QNP_CHECK(!est.hasRtt() && est.counts().stale == 1);
}

QNP_TEST(RouteEstimatorMatchesReorderedRepliesBySeq) {
    RouteRttEstimator<64, 128> est;
    const uint32_t a = est.onProbeSent(0);
    const uint32_t b = est.onProbeSent(1000);
    QNP_CHECK(est.onReply(b, 1000, est.generation(), 5000, 0) == ReplyOutcome::Sample);
    QNP_CHECK(est.onReply(a, 0, est.generation(), 6000, 0) == ReplyOutcome::Sample);
    QNP_CHECK(est.counts().uniqueReplies == 2);
    QNP_CHECK(est.rtt().minimum() == 4000 && est.rtt().maximum() == 6000);
}

QNP_TEST(RouteEstimatorTimeoutCountsLoss) {
    RouteRttEstimator<64, 128> est;
    const uint32_t seq = est.onProbeSent(0);
    est.expireOlderThan(100000, 50000);
    QNP_CHECK(est.counts().timeouts == 1 && est.counts().losses == 1);
    QNP_CHECK(est.inFlight() == 0);
    QNP_CHECK(est.onReply(seq, 0, est.generation(), 120000, 0) == ReplyOutcome::Duplicate);
    QNP_CHECK(!est.hasRtt());
}

QNP_TEST(RouteEstimatorLossRatioIsPerMille) {
    RouteRttEstimator<64, 128> est;
    for (int i = 0; i < 10; ++i) est.onProbeSent(static_cast<int64_t>(i) * 1000);
    est.expireOlderThan(52000, 50000);
    QNP_CHECK(est.counts().losses == 2);
    QNP_CHECK(est.lossPerMille() == 200);
}

QNP_TEST(RouteEstimatorTailIgnoresLoneSpike) {
    RouteRttEstimator<64, 128> est;
    for (int i = 0; i < 40; ++i) {
        const uint32_t s = est.onProbeSent(0);
        est.onReply(s, 0, est.generation(), 8000, 0);
    }
    const uint32_t spike = est.onProbeSent(0);
    est.onReply(spike, 0, est.generation(), 500000, 0);
    QNP_CHECK(est.maxMicros() == 500000);
    QNP_CHECK(est.p95Micros() == 8000);
}

QNP_TEST(RouteEstimatorDiscardsRepliesOurOwnLoopWasLateToRead) {
    RouteRttEstimator<64, 128> est;
    est.setLocalStallRejectMicros(25000);

    int64_t now = 0;
    for (int i = 0; i < 20; ++i) {
        now += 1000;
        est.onServiceTick(now);
        const uint32_t s = est.onProbeSent(now - 3000);
        QNP_CHECK(est.onReply(s, now - 3000, est.generation(), now, 0)
                  == ReplyOutcome::Sample);
    }
    QNP_CHECK(est.p95Micros() == 3000);
    QNP_CHECK(est.counts().uniqueReplies == 20);

    now += 500000;
    est.onServiceTick(now);
    for (int i = 0; i < 10; ++i) {
        const uint32_t s = est.onProbeSent(now - 500000);
        QNP_CHECK(est.onReply(s, now - 500000, est.generation(), now, 0)
                  == ReplyOutcome::LocallyDelayed);
    }
    QNP_CHECK(est.counts().locallyDelayed == 10);
    QNP_CHECK(est.counts().uniqueReplies == 20);
    QNP_CHECK(est.p95Micros() == 3000);
    QNP_CHECK(est.maxMicros() == 3000);

    QNP_CHECK(est.counts().losses == 0);
    QNP_CHECK(est.lossPerMille() == 0);
    QNP_CHECK(est.inFlight() == 0);

    now += 1000;
    est.onServiceTick(now);
    const uint32_t s = est.onProbeSent(now - 3000);
    QNP_CHECK(est.onReply(s, now - 3000, est.generation(), now, 0)
              == ReplyOutcome::Sample);
    QNP_CHECK(est.p95Micros() == 3000);
}

QNP_TEST(RouteEstimatorKeepsGenuinelyLargeRttWhenTheLoopIsHealthy) {
    RouteRttEstimator<64, 128> est;
    est.setLocalStallRejectMicros(25000);
    int64_t now = 0;
    for (int i = 0; i < 10; ++i) {
        now += 1000;
        est.onServiceTick(now);
        const uint32_t s = est.onProbeSent(now - 300000);
        QNP_CHECK(est.onReply(s, now - 300000, est.generation(), now, 0)
                  == ReplyOutcome::Sample);
    }
    QNP_CHECK(est.p50Micros() == 300000);
    QNP_CHECK(est.counts().locallyDelayed == 0);
}

QNP_TEST(RouteEstimatorLocalStallRuleIsOffUntilConfigured) {
    RouteRttEstimator<64, 128> est;
    QNP_CHECK(est.localStallRejectMicros() == 0);
    est.onServiceTick(0);
    est.onServiceTick(9000000);
    const uint32_t s = est.onProbeSent(9000000 - 4000);
    QNP_CHECK(est.onReply(s, 9000000 - 4000, est.generation(), 9000000, 0)
              == ReplyOutcome::Sample);
    QNP_CHECK(est.p50Micros() == 4000);
}

QNP_TEST(RouteEstimatorResetKeepsTheLocalStallThreshold) {
    RouteRttEstimator<64, 128> est;
    est.setLocalStallRejectMicros(25000);
    est.reset();
    QNP_CHECK(est.localStallRejectMicros() == 25000);
    est.onServiceTick(0);
    est.onServiceTick(100000);
    const uint32_t s = est.onProbeSent(0);
    QNP_CHECK(est.onReply(s, 0, est.generation(), 100000, 0)
              == ReplyOutcome::LocallyDelayed);
}
