#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace qgame {

constexpr uint32_t kPacingBurstThresholdUs = 8000u;
constexpr uint32_t kPacingLateThresholdUs = 25000u;
constexpr uint64_t kPacingReportWindowUs = 1000000ull;
constexpr size_t kPacingIntervalCapacity = 128u;

constexpr bool nativeSimulationAdvanced(
    const bool gameplay,
    const uint32_t counterBefore,
    const uint32_t counterAfter,
    const uint32_t arenaSelectorBefore,
    const uint32_t arenaSelectorAfter) {
    return gameplay
        && (counterBefore != counterAfter
            || arenaSelectorBefore != arenaSelectorAfter);
}

struct NetplayPacingReport {
    uint64_t elapsedUs = 0;
    uint32_t visibleTicks = 0;
    uint32_t gameplayTicks = 0;
    uint32_t nativeAdvanceTicks = 0;
    uint32_t nativeAdvanceMisses = 0;
    uint32_t intervalSamples = 0;
    uint32_t minimumIntervalUs = 0;
    uint32_t p95IntervalUs = 0;
    uint32_t maximumIntervalUs = 0;
    uint32_t burstIntervals = 0;
    uint32_t lateIntervals = 0;

    double visibleFps() const {
        return elapsedUs == 0
            ? 0.0
            : static_cast<double>(visibleTicks) * 1000000.0
                / static_cast<double>(elapsedUs);
    }
};

class NetplayPacingWindow {
public:
    void reset() {
        windowStartedUs_ = 0;
        lastVisibleUs_ = 0;
        visibleTicks_ = 0;
        gameplayTicks_ = 0;
        nativeAdvanceTicks_ = 0;
        intervalCount_ = 0;
        burstIntervals_ = 0;
        lateIntervals_ = 0;
    }

    void observe(uint64_t nowUs, bool gameplay,
                 bool nativeCounterAdvanced) {
        if (windowStartedUs_ == 0)
            windowStartedUs_ = nowUs;
        if (lastVisibleUs_ != 0 && nowUs >= lastVisibleUs_) {
            const uint64_t raw = nowUs - lastVisibleUs_;
            const uint32_t interval = static_cast<uint32_t>(
                raw > UINT32_MAX ? UINT32_MAX : raw);
            if (intervalCount_ < intervals_.size())
                intervals_[intervalCount_++] = interval;
            if (interval < kPacingBurstThresholdUs)
                ++burstIntervals_;
            if (interval > kPacingLateThresholdUs)
                ++lateIntervals_;
        }
        lastVisibleUs_ = nowUs;
        ++visibleTicks_;
        if (gameplay) {
            ++gameplayTicks_;
            if (nativeCounterAdvanced)
                ++nativeAdvanceTicks_;
        }
    }

    bool reportDue(uint64_t nowUs) const {
        return windowStartedUs_ != 0
            && nowUs >= windowStartedUs_
            && nowUs - windowStartedUs_ >= kPacingReportWindowUs;
    }

    NetplayPacingReport finish(uint64_t nowUs) {
        NetplayPacingReport report{};
        if (windowStartedUs_ != 0 && nowUs >= windowStartedUs_)
            report.elapsedUs = nowUs - windowStartedUs_;
        report.visibleTicks = visibleTicks_;
        report.gameplayTicks = gameplayTicks_;
        report.nativeAdvanceTicks = nativeAdvanceTicks_;
        report.nativeAdvanceMisses =
            gameplayTicks_ >= nativeAdvanceTicks_
                ? gameplayTicks_ - nativeAdvanceTicks_ : 0u;
        report.intervalSamples = static_cast<uint32_t>(intervalCount_);
        report.burstIntervals = burstIntervals_;
        report.lateIntervals = lateIntervals_;
        if (intervalCount_ != 0) {
            std::array<uint32_t, kPacingIntervalCapacity> sorted{};
            std::copy_n(intervals_.begin(), intervalCount_, sorted.begin());
            std::sort(sorted.begin(), sorted.begin() + intervalCount_);
            report.minimumIntervalUs = sorted[0];
            report.maximumIntervalUs = sorted[intervalCount_ - 1u];
            const size_t p95Index =
                (intervalCount_ * 95u + 99u) / 100u - 1u;
            report.p95IntervalUs = sorted[p95Index];
        }

        windowStartedUs_ = nowUs;
        visibleTicks_ = 0;
        gameplayTicks_ = 0;
        nativeAdvanceTicks_ = 0;
        intervalCount_ = 0;
        burstIntervals_ = 0;
        lateIntervals_ = 0;
        return report;
    }

private:
    uint64_t windowStartedUs_ = 0;
    uint64_t lastVisibleUs_ = 0;
    uint32_t visibleTicks_ = 0;
    uint32_t gameplayTicks_ = 0;
    uint32_t nativeAdvanceTicks_ = 0;
    std::array<uint32_t, kPacingIntervalCapacity> intervals_{};
    size_t intervalCount_ = 0;
    uint32_t burstIntervals_ = 0;
    uint32_t lateIntervals_ = 0;
};

}
