#pragma once

#include <cstddef>
#include <cstdint>

#include "net/RollingWindowStats.h"

namespace qnet {

enum class ReplyOutcome : uint8_t {
    Sample,
    Duplicate,
    Stale,
    Ambiguous,
    LocallyDelayed,
};

template <size_t InFlightCap = 64, size_t StatsCap = 128>
class RouteRttEstimator {
    static_assert(InFlightCap > 0 && StatsCap > 0, "capacities must be positive");

public:
    struct Counts {
        uint64_t sent = 0;
        uint64_t uniqueReplies = 0;
        uint64_t duplicates = 0;
        uint64_t stale = 0;
        uint64_t ambiguous = 0;
        uint64_t timeouts = 0;
        uint64_t losses = 0;
        uint64_t locallyDelayed = 0;
    };

    void reset() {
        ++generation_;
        inflightCount_ = 0;
        rtt_.reset();
        dwell_.reset();
        counts_ = Counts{};
        lastServiceTickMicros_ = 0;
        serviceGapMicros_ = 0;
        haveServiceTick_ = false;
    }

    uint32_t generation() const { return generation_; }

    void setLocalStallRejectMicros(int64_t micros) {
        localStallRejectMicros_ = micros > 0 ? micros : 0;
    }
    int64_t localStallRejectMicros() const { return localStallRejectMicros_; }

    void onServiceTick(int64_t nowMicros) {
        serviceGapMicros_ =
            haveServiceTick_ ? nowMicros - lastServiceTickMicros_ : 0;
        lastServiceTickMicros_ = nowMicros;
        haveServiceTick_ = true;
    }
    int64_t serviceGapMicros() const { return serviceGapMicros_; }

    uint32_t onProbeSent(int64_t sendMicros) {
        uint32_t seq = nextSeq_++;
        if (nextSeq_ == 0) nextSeq_ = 1;
        if (inflightCount_ == InFlightCap) {
            evictOldestAsLoss();
        }
        inflight_[inflightCount_++] = Inflight{seq, sendMicros, false};
        ++counts_.sent;
        return seq;
    }

    void onProbeRetransmitted(uint32_t seq) {
        const int i = find(seq);
        if (i >= 0) inflight_[i].retransmitted = true;
    }

    ReplyOutcome onReply(uint32_t seq, int64_t echoedSendMicros,
                         uint32_t replyGeneration, int64_t nowMicros,
                         int64_t responderDwellMicros) {
        if (replyGeneration != generation_) {
            ++counts_.stale;
            return ReplyOutcome::Stale;
        }
        const int i = find(seq);
        if (i < 0) {
            ++counts_.duplicates;
            return ReplyOutcome::Duplicate;
        }
        if (inflight_[i].sendMicros != echoedSendMicros) {
            ++counts_.stale;
            return ReplyOutcome::Stale;
        }
        const bool retransmitted = inflight_[i].retransmitted;
        removeAt(i);
        if (retransmitted) {
            ++counts_.ambiguous;
            return ReplyOutcome::Ambiguous;
        }
        if (localStallRejectMicros_ > 0
            && serviceGapMicros_ > localStallRejectMicros_) {
            ++counts_.locallyDelayed;
            return ReplyOutcome::LocallyDelayed;
        }
        int64_t rtt = nowMicros - echoedSendMicros;
        const int64_t dwell = responderDwellMicros > 0 ? responderDwellMicros : 0;
        rtt -= dwell;
        if (rtt < 0) rtt = 0;
        rtt_.add(rtt);
        if (dwell > 0) dwell_.add(dwell);
        ++counts_.uniqueReplies;
        return ReplyOutcome::Sample;
    }

    void expireOlderThan(int64_t nowMicros, int64_t timeoutMicros) {
        size_t w = 0;
        for (size_t r = 0; r < inflightCount_; ++r) {
            if (nowMicros - inflight_[r].sendMicros > timeoutMicros) {
                ++counts_.timeouts;
                ++counts_.losses;
                continue;
            }
            inflight_[w++] = inflight_[r];
        }
        inflightCount_ = w;
    }

    bool hasRtt() const { return rtt_.count() > 0; }
    const RollingWindowStats<StatsCap>& rtt() const { return rtt_; }
    const RollingWindowStats<StatsCap>& dwell() const { return dwell_; }
    Counts counts() const { return counts_; }
    size_t inFlight() const { return inflightCount_; }

    int64_t p50Micros() const { return rtt_.percentile(50); }
    int64_t p95Micros() const { return rtt_.percentile(95); }
    int64_t p99Micros() const { return rtt_.percentile(99); }
    int64_t maxMicros() const { return rtt_.maximum(); }
    int64_t jitterMicros() const { return rtt_.mad(); }

    uint32_t lossPerMille() const {
        return counts_.sent
            ? static_cast<uint32_t>(counts_.losses * 1000u / counts_.sent)
            : 0u;
    }

private:
    struct Inflight {
        uint32_t seq = 0;
        int64_t sendMicros = 0;
        bool retransmitted = false;
    };

    int find(uint32_t seq) const {
        for (size_t i = 0; i < inflightCount_; ++i)
            if (inflight_[i].seq == seq) return static_cast<int>(i);
        return -1;
    }

    void removeAt(size_t i) {
        inflight_[i] = inflight_[inflightCount_ - 1];
        --inflightCount_;
    }

    void evictOldestAsLoss() {
        size_t oldest = 0;
        for (size_t i = 1; i < inflightCount_; ++i)
            if (inflight_[i].sendMicros < inflight_[oldest].sendMicros) oldest = i;
        ++counts_.losses;
        removeAt(oldest);
    }

    Inflight inflight_[InFlightCap] = {};
    size_t inflightCount_ = 0;
    uint32_t nextSeq_ = 1;
    uint32_t generation_ = 1;
    int64_t localStallRejectMicros_ = 0;
    int64_t lastServiceTickMicros_ = 0;
    int64_t serviceGapMicros_ = 0;
    bool haveServiceTick_ = false;
    RollingWindowStats<StatsCap> rtt_;
    RollingWindowStats<StatsCap> dwell_;
    Counts counts_;
};

}
