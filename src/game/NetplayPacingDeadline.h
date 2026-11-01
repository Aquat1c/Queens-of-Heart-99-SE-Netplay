#pragma once

#include <cstdint>

namespace qgame {

enum class PacingDeadlineDecision : uint8_t {
    WaitIdeal,
    WaitVisibleSpacing,
    Ready,
    ReadyRebased,
};

struct PacingDeadlinePoll {
    PacingDeadlineDecision decision =
        PacingDeadlineDecision::WaitIdeal;
    int64_t rebasedDebtFrames = 0;

    constexpr bool ready() const {
        return decision == PacingDeadlineDecision::Ready
            || decision == PacingDeadlineDecision::ReadyRebased;
    }

    constexpr bool rebased() const {
        return decision == PacingDeadlineDecision::ReadyRebased;
    }

    constexpr bool spacingBlocked() const {
        return decision
            == PacingDeadlineDecision::WaitVisibleSpacing;
    }
};

class NetplayPacingDeadline {
public:
    static constexpr int64_t kMinimumSpacingNumerator = 7;
    static constexpr int64_t kMinimumSpacingDenominator = 8;
    static constexpr int64_t kMinimumPublishSpacingNumerator = 1;
    static constexpr int64_t kMinimumPublishSpacingDenominator = 2;
    // Match TrialNetplay's presentation policy: debt beyond one complete
    // frame is stale wall time, not useful cadence to repay as a speed-up.
    static constexpr int64_t kTrueStallFrames = 1;

    void configure(const int64_t frameTicks) {
        setFrameTicks(frameTicks);
        reset();
    }

    void setFrameTicks(const int64_t frameTicks) {
        frameTicks_ = frameTicks;
        minimumSpacingTicks_ =
            frameTicks > 0
                ? (frameTicks * kMinimumSpacingNumerator)
                    / kMinimumSpacingDenominator
                : 0;
    }

    void reset() {
        idealDeadline_ = 0;
        earliestVisibleDeadline_ = 0;
        lastSample_ = 0;
    }

    void semanticHold(const int64_t now) {
        if (frameTicks_ <= 0 || now < 0)
            return;
        lastSample_ = now;
    }

    void anchorPresentationCompleted(const int64_t now) {
        if (frameTicks_ <= 0 || now < 0)
            return;
        idealDeadline_ = now + frameTicks_;
        earliestVisibleDeadline_ =
            now + minimumSpacingTicks_;
        lastSample_ = now;
    }

    void observePresentationCompleted(const int64_t now) {
        if (frameTicks_ <= 0 || now < 0)
            return;
        const int64_t completedSpacing =
            now
            + (frameTicks_ * kMinimumPublishSpacingNumerator)
                / kMinimumPublishSpacingDenominator;
        if (completedSpacing > earliestVisibleDeadline_)
            earliestVisibleDeadline_ = completedSpacing;
        lastSample_ = now;
    }

    PacingDeadlinePoll poll(const int64_t now) {
        if (frameTicks_ <= 0 || now < 0)
            return {};

        if (lastSample_ > 0 && now < lastSample_) {
            idealDeadline_ = now + frameTicks_;
            earliestVisibleDeadline_ =
                now + minimumSpacingTicks_;
            lastSample_ = now;
            return {PacingDeadlineDecision::WaitIdeal};
        }
        lastSample_ = now;

        if (idealDeadline_ <= 0) {
            idealDeadline_ = now + frameTicks_;
            earliestVisibleDeadline_ =
                now + minimumSpacingTicks_;
            return {PacingDeadlineDecision::Ready};
        }

        if (now < idealDeadline_)
            return {PacingDeadlineDecision::WaitIdeal};
        if (now < earliestVisibleDeadline_) {
            return {
                PacingDeadlineDecision::WaitVisibleSpacing,
            };
        }

        const int64_t lateness = now - idealDeadline_;
        PacingDeadlinePoll result{};
        result.decision = PacingDeadlineDecision::Ready;
        if (lateness > kTrueStallFrames * frameTicks_) {
            result.rebasedDebtFrames = lateness / frameTicks_;
            idealDeadline_ = now + frameTicks_;
            result.decision = PacingDeadlineDecision::ReadyRebased;
        } else {
            idealDeadline_ += frameTicks_;
        }

        const int64_t provisional =
            now + minimumSpacingTicks_;
        if (provisional > earliestVisibleDeadline_)
            earliestVisibleDeadline_ = provisional;
        return result;
    }

    int64_t debtTicks(const int64_t now) const {
        return now > idealDeadline_ && idealDeadline_ > 0
            ? now - idealDeadline_ : 0;
    }

    int64_t frameTicks() const {
        return frameTicks_;
    }

    int64_t minimumSpacingTicks() const {
        return minimumSpacingTicks_;
    }

    int64_t idealDeadline() const {
        return idealDeadline_;
    }

    int64_t earliestVisibleDeadline() const {
        return earliestVisibleDeadline_;
    }

private:
    int64_t frameTicks_ = 0;
    int64_t minimumSpacingTicks_ = 0;
    int64_t idealDeadline_ = 0;
    int64_t earliestVisibleDeadline_ = 0;
    int64_t lastSample_ = 0;
};

inline constexpr int32_t kNetPaceCorrectionTargetFrames = 1;
inline constexpr int32_t kNetPaceCorrectionPpmPerFrame = 2500;
inline constexpr int32_t kNetPaceCorrectionMaxRisePpm = 8000;
inline constexpr int32_t kNetPaceCorrectionMaxPpm = 25000;
inline constexpr int32_t kNetPaceCorrectionIntegralPpmPerStep = 1500;

struct NetplayPaceCorrection {
    int32_t periodPpm = 0;
    int32_t integralPpm = 0;
};

constexpr NetplayPaceCorrection planTrailingPaceCorrection(
    const int32_t advantage, const int32_t deadbandFrames,
    const int32_t currentPeriodPpm, const int32_t currentIntegralPpm) {
    const int32_t deadband = deadbandFrames < 1 ? 1 : deadbandFrames;
    const int32_t lag = advantage < 0 ? -advantage : 0;

    if (lag <= kNetPaceCorrectionTargetFrames)
        return {};

    int32_t integral = currentIntegralPpm < 0 ? 0 : currentIntegralPpm;
    int32_t demandMagnitude = 0;
    if (lag > deadband) {
        integral += kNetPaceCorrectionIntegralPpmPerStep;
        if (integral > kNetPaceCorrectionMaxPpm)
            integral = kNetPaceCorrectionMaxPpm;
        demandMagnitude =
            (lag - kNetPaceCorrectionTargetFrames)
                * kNetPaceCorrectionPpmPerFrame
            + integral;
    } else {
        demandMagnitude = currentPeriodPpm < 0 ? -currentPeriodPpm : 0;
    }
    if (demandMagnitude > kNetPaceCorrectionMaxPpm)
        demandMagnitude = kNetPaceCorrectionMaxPpm;

    int32_t magnitude = currentPeriodPpm < 0 ? -currentPeriodPpm : 0;
    if (demandMagnitude > magnitude) {
        const int32_t raised = magnitude + kNetPaceCorrectionMaxRisePpm;
        magnitude = raised < demandMagnitude ? raised : demandMagnitude;
    } else if (demandMagnitude < magnitude) {
        magnitude = demandMagnitude;
    }
    return {-magnitude, integral};
}

}
