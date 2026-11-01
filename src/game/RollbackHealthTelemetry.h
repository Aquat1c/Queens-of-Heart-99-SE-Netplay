#pragma once

#include "../netplay/RollbackSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace qgame {

constexpr uint64_t kRollbackHealthReportWindowUs = 1000000ull;
constexpr double kQohTargetFps = 60.60;

enum class AdapterHoldReason : uint8_t {
    None,
    TestBarrier,
    DelayDrain,
    SessionEvent,
    LayoutRefresh,
    ExactInput,
    PresentationGate,
    RollbackSetup,
    LayoutBarrier,
    CorrectionWait,
    FrameAdvantage,
    Count,
};

inline const char* adapterHoldReasonName(const AdapterHoldReason reason) {
    switch (reason) {
        case AdapterHoldReason::None: return "none";
        case AdapterHoldReason::TestBarrier: return "test-barrier";
        case AdapterHoldReason::DelayDrain: return "delay-drain";
        case AdapterHoldReason::SessionEvent: return "session-event";
        case AdapterHoldReason::LayoutRefresh: return "layout-refresh";
        case AdapterHoldReason::ExactInput: return "exact-input";
        case AdapterHoldReason::PresentationGate:
            return "presentation-gate";
        case AdapterHoldReason::RollbackSetup: return "rollback-setup";
        case AdapterHoldReason::LayoutBarrier: return "layout-barrier";
        case AdapterHoldReason::CorrectionWait: return "correction-wait";
        case AdapterHoldReason::FrameAdvantage: return "frame-advantage";
        case AdapterHoldReason::Count: break;
    }
    return "unknown";
}

enum class RollbackStallTransitionKind : uint8_t {
    None,
    Began,
    ReasonChanged,
    Ended,
};

struct RollbackStallTransition {
    RollbackStallTransitionKind kind =
        RollbackStallTransitionKind::None;
    qnetplay::StallReason from = qnetplay::StallReason::None;
    qnetplay::StallReason to = qnetplay::StallReason::None;

    uint64_t episodeDurationUs = 0;
    uint64_t previousReasonDurationUs = 0;

    bool occurred() const {
        return kind != RollbackStallTransitionKind::None;
    }
};

struct AdapterHoldTransition {
    RollbackStallTransitionKind kind =
        RollbackStallTransitionKind::None;
    AdapterHoldReason from = AdapterHoldReason::None;
    AdapterHoldReason to = AdapterHoldReason::None;
    uint64_t episodeDurationUs = 0;
    uint64_t previousReasonDurationUs = 0;

    bool occurred() const {
        return kind != RollbackStallTransitionKind::None;
    }
};

struct RollbackHealthObservation {
    RollbackStallTransition stall{};
    AdapterHoldTransition adapterHold{};
    bool reportDue = false;
    bool clockRegressed = false;
    bool statsRegressed = false;
};

struct RollbackHealthReport {
    uint64_t elapsedUs = 0;

    uint64_t newForwardFrames = 0;
    uint32_t newForwardSpeedPermille = 0;
    uint64_t confirmedFrames = 0;
    uint32_t confirmedLagFrames = 0;
    uint32_t maximumConfirmedLagFrames = 0;

    uint64_t replayedFrames = 0;
    uint64_t rollbackCount = 0;
    uint64_t rollbackFrames = 0;

    uint64_t activeWallUs = 0;
    uint64_t stalledWallUs = 0;
    std::array<uint64_t,
               static_cast<size_t>(qnetplay::StallReason::Count)>
        stalledWallUsByReason{};
    uint64_t adapterHoldWallUs = 0;
    std::array<uint64_t,
               static_cast<size_t>(AdapterHoldReason::Count)>
        adapterHoldWallUsByReason{};
    uint64_t runnableWallUs = 0;
    uint64_t blockedWallUs = 0;
    AdapterHoldReason adapterHoldReason = AdapterHoldReason::None;
    uint64_t noProgressAgeUs = 0;

    uint64_t clockRegressionFaults = 0;
    uint64_t statsRegressionFaults = 0;
    uint64_t totalClockRegressionFaults = 0;
    uint64_t totalStatsRegressionFaults = 0;

    qnetplay::RollbackDiagnostics diagnostics{};

    double newForwardFps() const {
        return elapsedUs == 0
            ? 0.0
            : static_cast<double>(newForwardFrames) * 1000000.0
                / static_cast<double>(elapsedUs);
    }

    double confirmedFps() const {
        return elapsedUs == 0
            ? 0.0
            : static_cast<double>(confirmedFrames) * 1000000.0
                / static_cast<double>(elapsedUs);
    }
};

class RollbackHealthTelemetry {
public:
    RollbackHealthTelemetry() = default;

    void reset() {
        initialized_ = false;
        lastNowUs_ = 0;
        lastStats_ = {};
        latestDiagnostics_ = {};
        lastStallReason_ = qnetplay::StallReason::None;
        stallEpisodeAgeUs_ = 0;
        stallReasonAgeUs_ = 0;
        lastAdapterHoldReason_ = AdapterHoldReason::None;
        adapterHoldEpisodeAgeUs_ = 0;
        adapterHoldReasonAgeUs_ = 0;
        noProgressAgeUs_ = 0;

        windowElapsedUs_ = 0;
        windowNewForwardFrames_ = 0;
        windowConfirmedFrames_ = 0;
        windowReplayedFrames_ = 0;
        windowRollbackCount_ = 0;
        windowRollbackFrames_ = 0;
        windowActiveWallUs_ = 0;
        windowStalledWallUs_ = 0;
        windowStalledWallUsByReason_.fill(0);
        windowAdapterHoldWallUs_ = 0;
        windowAdapterHoldWallUsByReason_.fill(0);
        windowRunnableWallUs_ = 0;
        windowBlockedWallUs_ = 0;
        windowMaximumConfirmedLagFrames_ = 0;
        windowClockRegressionFaults_ = 0;
        windowStatsRegressionFaults_ = 0;

        totalObservedWallUs_ = 0;
        totalActiveWallUs_ = 0;
        totalStalledWallUs_ = 0;
        totalStalledWallUsByReason_.fill(0);
        totalAdapterHoldWallUs_ = 0;
        totalAdapterHoldWallUsByReason_.fill(0);
        totalRunnableWallUs_ = 0;
        totalBlockedWallUs_ = 0;
        totalClockRegressionFaults_ = 0;
        totalStatsRegressionFaults_ = 0;
    }

    RollbackHealthObservation observe(
        const uint64_t nowUs,
        const qnetplay::RollbackDiagnostics& diagnostics,
        const qnetplay::RollbackStats& stats,
        const AdapterHoldReason adapterHoldReason =
            AdapterHoldReason::None) {
        RollbackHealthObservation observation{};
        const qnetplay::StallReason currentReason =
            normalizedStallReason(diagnostics.activeStallReason);
        const AdapterHoldReason currentAdapterHoldReason =
            normalizedAdapterHoldReason(adapterHoldReason);

        if (!initialized_) {
            initialized_ = true;
            lastNowUs_ = nowUs;
            lastStats_ = stats;
            latestDiagnostics_ = diagnostics;
            lastStallReason_ = currentReason;
            lastAdapterHoldReason_ = currentAdapterHoldReason;
            windowMaximumConfirmedLagFrames_ =
                confirmedLagFrames(diagnostics);
            if (currentReason != qnetplay::StallReason::None) {
                observation.stall.kind =
                    RollbackStallTransitionKind::Began;
                observation.stall.to = currentReason;
            }
            if (currentAdapterHoldReason != AdapterHoldReason::None) {
                observation.adapterHold.kind =
                    RollbackStallTransitionKind::Began;
                observation.adapterHold.to = currentAdapterHoldReason;
            }
            return observation;
        }

        uint64_t elapsedUs = 0;
        if (nowUs < lastNowUs_) {
            observation.clockRegressed = true;
            ++windowClockRegressionFaults_;
            ++totalClockRegressionFaults_;
            lastNowUs_ = nowUs;
        } else {
            elapsedUs = nowUs - lastNowUs_;
            lastNowUs_ = nowUs;
        }

        attributeWallTime(elapsedUs);

        const bool statsRegressed =
            stats.visibleAdvances < lastStats_.visibleAdvances
            || stats.confirmedFrames < lastStats_.confirmedFrames
            || stats.replayedFrames < lastStats_.replayedFrames
            || stats.rollbackCount < lastStats_.rollbackCount
            || stats.rollbackFrames < lastStats_.rollbackFrames;
        if (statsRegressed) {
            observation.statsRegressed = true;
            ++windowStatsRegressionFaults_;
            ++totalStatsRegressionFaults_;
        }

        const uint64_t newForwardDelta = monotonicDelta(
            stats.visibleAdvances, lastStats_.visibleAdvances);
        windowNewForwardFrames_ += newForwardDelta;
        windowConfirmedFrames_ += monotonicDelta(
            stats.confirmedFrames, lastStats_.confirmedFrames);
        windowReplayedFrames_ += monotonicDelta(
            stats.replayedFrames, lastStats_.replayedFrames);
        windowRollbackCount_ += monotonicDelta(
            stats.rollbackCount, lastStats_.rollbackCount);
        windowRollbackFrames_ += monotonicDelta(
            stats.rollbackFrames, lastStats_.rollbackFrames);
        lastStats_ = stats;

        if (newForwardDelta != 0 || statsRegressed)
            noProgressAgeUs_ = 0;
        else
            noProgressAgeUs_ = saturatingAdd(noProgressAgeUs_, elapsedUs);

        observation.stall = transitionTo(currentReason);
        observation.adapterHold =
            transitionAdapterHoldTo(currentAdapterHoldReason);
        latestDiagnostics_ = diagnostics;
        const uint32_t lag = confirmedLagFrames(diagnostics);
        if (lag > windowMaximumConfirmedLagFrames_)
            windowMaximumConfirmedLagFrames_ = lag;
        observation.reportDue = reportDue();
        return observation;
    }

    bool reportDue() const {
        return initialized_
            && windowElapsedUs_ >= kRollbackHealthReportWindowUs;
    }

    RollbackHealthReport finishReport() {
        RollbackHealthReport report{};
        report.elapsedUs = windowElapsedUs_;
        report.newForwardFrames = windowNewForwardFrames_;
        report.newForwardSpeedPermille = speedPermille(
            windowNewForwardFrames_, windowElapsedUs_);
        report.confirmedFrames = windowConfirmedFrames_;
        report.confirmedLagFrames =
            confirmedLagFrames(latestDiagnostics_);
        report.maximumConfirmedLagFrames =
            windowMaximumConfirmedLagFrames_;
        report.replayedFrames = windowReplayedFrames_;
        report.rollbackCount = windowRollbackCount_;
        report.rollbackFrames = windowRollbackFrames_;
        report.activeWallUs = windowActiveWallUs_;
        report.stalledWallUs = windowStalledWallUs_;
        report.stalledWallUsByReason =
            windowStalledWallUsByReason_;
        report.adapterHoldWallUs = windowAdapterHoldWallUs_;
        report.adapterHoldWallUsByReason =
            windowAdapterHoldWallUsByReason_;
        report.runnableWallUs = windowRunnableWallUs_;
        report.blockedWallUs = windowBlockedWallUs_;
        report.adapterHoldReason = lastAdapterHoldReason_;
        report.noProgressAgeUs = noProgressAgeUs_;
        report.clockRegressionFaults =
            windowClockRegressionFaults_;
        report.statsRegressionFaults =
            windowStatsRegressionFaults_;
        report.totalClockRegressionFaults =
            totalClockRegressionFaults_;
        report.totalStatsRegressionFaults =
            totalStatsRegressionFaults_;
        report.diagnostics = latestDiagnostics_;

        windowElapsedUs_ = 0;
        windowNewForwardFrames_ = 0;
        windowConfirmedFrames_ = 0;
        windowReplayedFrames_ = 0;
        windowRollbackCount_ = 0;
        windowRollbackFrames_ = 0;
        windowActiveWallUs_ = 0;
        windowStalledWallUs_ = 0;
        windowStalledWallUsByReason_.fill(0);
        windowAdapterHoldWallUs_ = 0;
        windowAdapterHoldWallUsByReason_.fill(0);
        windowRunnableWallUs_ = 0;
        windowBlockedWallUs_ = 0;
        windowMaximumConfirmedLagFrames_ =
            confirmedLagFrames(latestDiagnostics_);
        windowClockRegressionFaults_ = 0;
        windowStatsRegressionFaults_ = 0;
        return report;
    }

    uint64_t totalObservedWallUs() const {
        return totalObservedWallUs_;
    }
    uint64_t totalActiveWallUs() const {
        return totalActiveWallUs_;
    }
    uint64_t totalStalledWallUs() const {
        return totalStalledWallUs_;
    }
    const std::array<uint64_t,
                     static_cast<size_t>(qnetplay::StallReason::Count)>&
    totalStalledWallUsByReason() const {
        return totalStalledWallUsByReason_;
    }
    uint64_t totalAdapterHoldWallUs() const {
        return totalAdapterHoldWallUs_;
    }
    const std::array<uint64_t,
                     static_cast<size_t>(AdapterHoldReason::Count)>&
    totalAdapterHoldWallUsByReason() const {
        return totalAdapterHoldWallUsByReason_;
    }
    uint64_t totalRunnableWallUs() const {
        return totalRunnableWallUs_;
    }
    uint64_t totalBlockedWallUs() const {
        return totalBlockedWallUs_;
    }
    uint64_t totalClockRegressionFaults() const {
        return totalClockRegressionFaults_;
    }
    uint64_t totalStatsRegressionFaults() const {
        return totalStatsRegressionFaults_;
    }
    qnetplay::StallReason currentStallReason() const {
        return lastStallReason_;
    }
    uint64_t currentStallEpisodeAgeUs() const {
        return stallEpisodeAgeUs_;
    }
    uint64_t currentStallReasonAgeUs() const {
        return stallReasonAgeUs_;
    }
    AdapterHoldReason currentAdapterHoldReason() const {
        return lastAdapterHoldReason_;
    }
    uint64_t currentAdapterHoldEpisodeAgeUs() const {
        return adapterHoldEpisodeAgeUs_;
    }
    uint64_t currentAdapterHoldReasonAgeUs() const {
        return adapterHoldReasonAgeUs_;
    }
    uint64_t currentNoProgressAgeUs() const {
        return noProgressAgeUs_;
    }

private:
    using StallWallArray = std::array<
        uint64_t, static_cast<size_t>(qnetplay::StallReason::Count)>;
    using AdapterHoldWallArray = std::array<
        uint64_t, static_cast<size_t>(AdapterHoldReason::Count)>;

    static uint64_t monotonicDelta(
        const uint64_t current, const uint64_t previous) {
        return current >= previous ? current - previous : 0;
    }

    static uint64_t saturatingAdd(
        const uint64_t left, const uint64_t right) {
        const uint64_t maximum =
            (std::numeric_limits<uint64_t>::max)();
        return right > maximum - left ? maximum : left + right;
    }

    static qnetplay::StallReason normalizedStallReason(
        const qnetplay::StallReason reason) {
        const size_t index = static_cast<size_t>(reason);
        return index < static_cast<size_t>(qnetplay::StallReason::Count)
            ? reason
            : qnetplay::StallReason::InvalidSession;
    }

    static AdapterHoldReason normalizedAdapterHoldReason(
        const AdapterHoldReason reason) {
        const size_t index = static_cast<size_t>(reason);
        return index < static_cast<size_t>(AdapterHoldReason::Count)
            ? reason
            : AdapterHoldReason::None;
    }

    static uint32_t confirmedLagFrames(
        const qnetplay::RollbackDiagnostics& diagnostics) {
        return diagnostics.valid
            ? diagnostics.frontier - diagnostics.nextConfirmedFrame
            : 0u;
    }

    static uint32_t speedPermille(
        const uint64_t frames, const uint64_t elapsedUs) {
        if (elapsedUs == 0)
            return 0;
        const long double scaled =
            static_cast<long double>(frames) * 1000000.0L * 1000.0L
            / (static_cast<long double>(elapsedUs)
               * static_cast<long double>(kQohTargetFps));
        const long double maximum =
            static_cast<long double>(
                (std::numeric_limits<uint32_t>::max)());
        return scaled >= maximum
            ? (std::numeric_limits<uint32_t>::max)()
            : static_cast<uint32_t>(scaled + 0.5L);
    }

    void attributeWallTime(const uint64_t elapsedUs) {
        windowElapsedUs_ =
            saturatingAdd(windowElapsedUs_, elapsedUs);
        totalObservedWallUs_ =
            saturatingAdd(totalObservedWallUs_, elapsedUs);
        const bool schedulerStalled =
            lastStallReason_ != qnetplay::StallReason::None;
        const bool adapterHeld =
            lastAdapterHoldReason_ != AdapterHoldReason::None;
        if (!schedulerStalled) {
            windowActiveWallUs_ =
                saturatingAdd(windowActiveWallUs_, elapsedUs);
            totalActiveWallUs_ =
                saturatingAdd(totalActiveWallUs_, elapsedUs);
        } else {
            windowStalledWallUs_ =
                saturatingAdd(windowStalledWallUs_, elapsedUs);
            totalStalledWallUs_ =
                saturatingAdd(totalStalledWallUs_, elapsedUs);
            const size_t reasonIndex =
                static_cast<size_t>(lastStallReason_);
            windowStalledWallUsByReason_[reasonIndex] = saturatingAdd(
                windowStalledWallUsByReason_[reasonIndex], elapsedUs);
            totalStalledWallUsByReason_[reasonIndex] = saturatingAdd(
                totalStalledWallUsByReason_[reasonIndex], elapsedUs);
            stallEpisodeAgeUs_ =
                saturatingAdd(stallEpisodeAgeUs_, elapsedUs);
            stallReasonAgeUs_ =
                saturatingAdd(stallReasonAgeUs_, elapsedUs);
        }

        if (adapterHeld) {
            windowAdapterHoldWallUs_ =
                saturatingAdd(windowAdapterHoldWallUs_, elapsedUs);
            totalAdapterHoldWallUs_ =
                saturatingAdd(totalAdapterHoldWallUs_, elapsedUs);
            const size_t reasonIndex =
                static_cast<size_t>(lastAdapterHoldReason_);
            windowAdapterHoldWallUsByReason_[reasonIndex] = saturatingAdd(
                windowAdapterHoldWallUsByReason_[reasonIndex], elapsedUs);
            totalAdapterHoldWallUsByReason_[reasonIndex] = saturatingAdd(
                totalAdapterHoldWallUsByReason_[reasonIndex], elapsedUs);
            adapterHoldEpisodeAgeUs_ =
                saturatingAdd(adapterHoldEpisodeAgeUs_, elapsedUs);
            adapterHoldReasonAgeUs_ =
                saturatingAdd(adapterHoldReasonAgeUs_, elapsedUs);
        }

        if (schedulerStalled || adapterHeld) {
            windowBlockedWallUs_ =
                saturatingAdd(windowBlockedWallUs_, elapsedUs);
            totalBlockedWallUs_ =
                saturatingAdd(totalBlockedWallUs_, elapsedUs);
        } else {
            windowRunnableWallUs_ =
                saturatingAdd(windowRunnableWallUs_, elapsedUs);
            totalRunnableWallUs_ =
                saturatingAdd(totalRunnableWallUs_, elapsedUs);
        }
    }

    RollbackStallTransition transitionTo(
        const qnetplay::StallReason currentReason) {
        RollbackStallTransition transition{};
        if (currentReason == lastStallReason_)
            return transition;

        transition.from = lastStallReason_;
        transition.to = currentReason;
        transition.episodeDurationUs = stallEpisodeAgeUs_;
        transition.previousReasonDurationUs = stallReasonAgeUs_;

        if (lastStallReason_ == qnetplay::StallReason::None) {
            transition.kind = RollbackStallTransitionKind::Began;
            transition.episodeDurationUs = 0;
            transition.previousReasonDurationUs = 0;
            stallEpisodeAgeUs_ = 0;
            stallReasonAgeUs_ = 0;
        } else if (currentReason == qnetplay::StallReason::None) {
            transition.kind = RollbackStallTransitionKind::Ended;
            stallEpisodeAgeUs_ = 0;
            stallReasonAgeUs_ = 0;
        } else {
            transition.kind =
                RollbackStallTransitionKind::ReasonChanged;
            stallReasonAgeUs_ = 0;
        }

        lastStallReason_ = currentReason;
        return transition;
    }

    AdapterHoldTransition transitionAdapterHoldTo(
        const AdapterHoldReason currentReason) {
        AdapterHoldTransition transition{};
        if (currentReason == lastAdapterHoldReason_)
            return transition;

        transition.from = lastAdapterHoldReason_;
        transition.to = currentReason;
        transition.episodeDurationUs = adapterHoldEpisodeAgeUs_;
        transition.previousReasonDurationUs =
            adapterHoldReasonAgeUs_;

        if (lastAdapterHoldReason_ == AdapterHoldReason::None) {
            transition.kind = RollbackStallTransitionKind::Began;
            transition.episodeDurationUs = 0;
            transition.previousReasonDurationUs = 0;
            adapterHoldEpisodeAgeUs_ = 0;
            adapterHoldReasonAgeUs_ = 0;
        } else if (currentReason == AdapterHoldReason::None) {
            transition.kind = RollbackStallTransitionKind::Ended;
            adapterHoldEpisodeAgeUs_ = 0;
            adapterHoldReasonAgeUs_ = 0;
        } else {
            transition.kind =
                RollbackStallTransitionKind::ReasonChanged;
            adapterHoldReasonAgeUs_ = 0;
        }

        lastAdapterHoldReason_ = currentReason;
        return transition;
    }

    bool initialized_ = false;
    uint64_t lastNowUs_ = 0;
    qnetplay::RollbackStats lastStats_{};
    qnetplay::RollbackDiagnostics latestDiagnostics_{};
    qnetplay::StallReason lastStallReason_ =
        qnetplay::StallReason::None;
    uint64_t stallEpisodeAgeUs_ = 0;
    uint64_t stallReasonAgeUs_ = 0;
    AdapterHoldReason lastAdapterHoldReason_ =
        AdapterHoldReason::None;
    uint64_t adapterHoldEpisodeAgeUs_ = 0;
    uint64_t adapterHoldReasonAgeUs_ = 0;
    uint64_t noProgressAgeUs_ = 0;

    uint64_t windowElapsedUs_ = 0;
    uint64_t windowNewForwardFrames_ = 0;
    uint64_t windowConfirmedFrames_ = 0;
    uint64_t windowReplayedFrames_ = 0;
    uint64_t windowRollbackCount_ = 0;
    uint64_t windowRollbackFrames_ = 0;
    uint64_t windowActiveWallUs_ = 0;
    uint64_t windowStalledWallUs_ = 0;
    StallWallArray windowStalledWallUsByReason_{};
    uint64_t windowAdapterHoldWallUs_ = 0;
    AdapterHoldWallArray windowAdapterHoldWallUsByReason_{};
    uint64_t windowRunnableWallUs_ = 0;
    uint64_t windowBlockedWallUs_ = 0;
    uint32_t windowMaximumConfirmedLagFrames_ = 0;
    uint64_t windowClockRegressionFaults_ = 0;
    uint64_t windowStatsRegressionFaults_ = 0;

    uint64_t totalObservedWallUs_ = 0;
    uint64_t totalActiveWallUs_ = 0;
    uint64_t totalStalledWallUs_ = 0;
    StallWallArray totalStalledWallUsByReason_{};
    uint64_t totalAdapterHoldWallUs_ = 0;
    AdapterHoldWallArray totalAdapterHoldWallUsByReason_{};
    uint64_t totalRunnableWallUs_ = 0;
    uint64_t totalBlockedWallUs_ = 0;
    uint64_t totalClockRegressionFaults_ = 0;
    uint64_t totalStatsRegressionFaults_ = 0;
};

}
