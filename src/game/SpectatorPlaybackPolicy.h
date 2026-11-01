#pragma once

#include <cstddef>
#include <cstdint>

namespace qgame {

constexpr uint64_t kSpectatorGameplayEntryTimeoutMs = 30000u;
constexpr uint64_t kSpectatorGameStartTimeoutMs = 300000u;
constexpr uint64_t kSpectatorStartRendezvousTimeoutMs = 30000u;

constexpr uint64_t kSpectatorSourceGoneMs = 45000u;

constexpr uint64_t kSpectatorSetSeekWaitMs = 15000u;

constexpr uint64_t kSpectatorLayoutHoldDeadlineMs = 10000u;

enum class SpectatorStartWait : uint32_t {
    KeepWaiting = 0,
    SourceGone = 1,
    NeverAnswered = 2,
    Expired = 3,
};

constexpr SpectatorStartWait spectatorStartWaitVerdict(bool sawSourceContact,
                                                       uint64_t contactAgeMs,
                                                       uint64_t waitedMs) {
    return sawSourceContact
        ? (contactAgeMs >= kSpectatorSourceGoneMs
               ? SpectatorStartWait::SourceGone
               : (waitedMs >= kSpectatorGameStartTimeoutMs
                      ? SpectatorStartWait::Expired
                      : SpectatorStartWait::KeepWaiting))
        : (waitedMs >= kSpectatorSourceGoneMs
               ? SpectatorStartWait::NeverAnswered
               : SpectatorStartWait::KeepWaiting);
}

constexpr std::size_t kSpectatorStartupBufferRecords = 120u;
constexpr std::size_t kSpectatorLiveTargetRecords = 120u;

constexpr std::size_t kSpectatorRebufferRecords = 90u;

constexpr std::size_t kSpectatorCatchupBandRecords = 300u;
constexpr std::size_t kSpectatorCatchupEnterRecords =
    kSpectatorLiveTargetRecords + kSpectatorCatchupBandRecords;
static_assert(kSpectatorRebufferRecords < kSpectatorLiveTargetRecords,
              "rebuffer recovery must resume below the steady target");
static_assert(kSpectatorLiveTargetRecords < kSpectatorCatchupEnterRecords,
              "catch-up hysteresis needs a non-empty band");

constexpr std::size_t kSpectatorCushionStepRecords = 120u;
constexpr std::size_t kSpectatorCushionMaxBonusRecords = 360u;

constexpr std::size_t spectatorCushionBonus(uint64_t underrunEpisodes) {
    const uint64_t bonus = underrunEpisodes * kSpectatorCushionStepRecords;
    return bonus > kSpectatorCushionMaxBonusRecords
        ? kSpectatorCushionMaxBonusRecords
        : static_cast<std::size_t>(bonus);
}
constexpr std::size_t spectatorLiveTarget(std::size_t cushionBonus) {
    return kSpectatorLiveTargetRecords + cushionBonus;
}
constexpr std::size_t spectatorCatchupEnter(std::size_t cushionBonus) {
    return kSpectatorCatchupEnterRecords + cushionBonus;
}

constexpr std::size_t kSpectatorElasticNearLowRecords = 60u;
constexpr std::size_t kSpectatorElasticLowRecords = 30u;
constexpr std::size_t kSpectatorElasticCriticalRecords = 15u;
constexpr uint32_t kSpectatorElasticNearLowPermille = 970u;
constexpr uint32_t kSpectatorElasticLowPermille = 900u;
constexpr uint32_t kSpectatorElasticCriticalPermille = 750u;
static_assert(kSpectatorElasticCriticalRecords < kSpectatorElasticLowRecords
              && kSpectatorElasticLowRecords
                     < kSpectatorElasticNearLowRecords
              && kSpectatorElasticNearLowRecords
                     < kSpectatorRebufferRecords,
              "elastic watermarks must sit below the rebuffer level");
static_assert(kSpectatorElasticCriticalPermille
                  < kSpectatorElasticLowPermille
              && kSpectatorElasticLowPermille
                  < kSpectatorElasticNearLowPermille
              && kSpectatorElasticNearLowPermille < 1000u,
              "elastic cadence must recover monotonically toward 1x");
constexpr uint64_t kSpectatorCatchupSliceMicros = 24000u;
constexpr uint64_t kSpectatorCatchupMenuSliceMicros = 60000u;
constexpr uint64_t spectatorCatchupSliceMicros(bool inGameplay) {
    return inGameplay ? kSpectatorCatchupSliceMicros
                      : kSpectatorCatchupMenuSliceMicros;
}

enum class SpectatorPreStateDecision {
    Accept,
    RetryEntry,
    Reject,
};

constexpr SpectatorPreStateDecision spectatorPreStateDecision(
    bool segmentActive,
    uint64_t expectedHash,
    uint64_t observedHash,
    uint64_t entryElapsedMs) {
    if (expectedHash != 0 && expectedHash == observedHash)
        return SpectatorPreStateDecision::Accept;
    if (!segmentActive
        && expectedHash != 0
        && observedHash != 0
        && entryElapsedMs < kSpectatorGameplayEntryTimeoutMs) {
        return SpectatorPreStateDecision::RetryEntry;
    }
    return SpectatorPreStateDecision::Reject;
}

constexpr bool spectatorSourceStopsOnDispatchReturn(
    uint32_t sceneBefore,
    int dispatchResult) {
    return dispatchResult == 0
        && (sceneBefore == 1u || sceneBefore == 3u);
}

constexpr uint64_t kSpectatorSourceIdleDwellMs = 2000u;

enum class SpectatorWait : uint32_t {
    Filling = 0,
    SourceIdle = 1,
    AwaitingEvent = 2,
    Joining = 3,
};

constexpr bool spectatorSourceHoldsNothingMore(bool frontierKnown,
                                               uint32_t sourceNextRecord,
                                               uint32_t receivedNextRecord,
                                               uint64_t frontierStaticMs) {
    return frontierKnown
        && receivedNextRecord == sourceNextRecord
        && frontierStaticMs >= kSpectatorSourceIdleDwellMs;
}

constexpr bool spectatorPlaybackPrimed(std::size_t bufferedRecords,
                                       bool hasTerminal,
                                       bool phaseComplete,
                                       bool rebuffering = false,
                                       std::size_t cushionBonus = 0u,
                                       bool sourceExhausted = false) {
    if (bufferedRecords == 0u) return false;
    if (hasTerminal || phaseComplete || sourceExhausted) return true;
    return bufferedRecords >= (rebuffering
        ? kSpectatorRebufferRecords
        : kSpectatorStartupBufferRecords) + cushionBonus;
}

constexpr bool spectatorEmptyBufferIsUnderrun(bool frontierKnown,
                                              uint32_t sourceNextRecord,
                                              uint32_t receivedNextRecord) {
    return !frontierKnown || sourceNextRecord != receivedNextRecord;
}

constexpr uint32_t spectatorElasticSpeedPermille(
    std::size_t bufferedRecords) {
    if (bufferedRecords >= kSpectatorElasticNearLowRecords)
        return 1000u;
    if (bufferedRecords >= kSpectatorElasticLowRecords)
        return kSpectatorElasticNearLowPermille;
    if (bufferedRecords >= kSpectatorElasticCriticalRecords)
        return kSpectatorElasticLowPermille;
    return bufferedRecords != 0u
        ? kSpectatorElasticCriticalPermille
        : 0u;
}

struct SpectatorElasticPlan {
    bool advance = true;
    uint32_t nextCredit = 0u;
    uint32_t speedPermille = 1000u;
};

constexpr SpectatorElasticPlan spectatorElasticPlan(
    std::size_t bufferedRecords,
    uint32_t priorCredit) {
    SpectatorElasticPlan plan{};
    plan.speedPermille = spectatorElasticSpeedPermille(bufferedRecords);
    if (plan.speedPermille >= 1000u) return plan;
    if (plan.speedPermille == 0u) {
        plan.advance = false;
        return plan;
    }
    const uint32_t credit = (priorCredit % 1000u) + plan.speedPermille;
    plan.advance = credit >= 1000u;
    plan.nextCredit = plan.advance ? credit - 1000u : credit;
    return plan;
}

struct SpectatorCatchupPlan {
    std::size_t tickBudget = 0u;
    bool catchingUp = false;
    uint32_t speedPermille = 1000u;
};

constexpr uint32_t kSpectatorCatchupMaxPermille = 24000u;

constexpr uint32_t spectatorCatchupSpeedPermille(std::size_t bufferedRecords,
                                                std::size_t cushionBonus = 0u,
                                                bool inGameplay = true) {
    if (!inGameplay) return kSpectatorCatchupMaxPermille;
    const std::size_t target = spectatorLiveTarget(cushionBonus);
    const std::size_t excess =
        bufferedRecords > target ? bufferedRecords - target : 0u;
    if (excess > 9600u) return 24000u;
    if (excess > 4800u) return 16000u;
    if (excess > 2400u) return 8000u;
    if (excess > 1200u) return 4000u;
    return 2000u;
}

constexpr SpectatorCatchupPlan spectatorCatchupPlan(
    std::size_t bufferedRecords,
    bool wasCatchingUp,
    std::size_t cushionBonus = 0u,
    bool inGameplay = true,
    bool forceMaximum = false) {
    SpectatorCatchupPlan plan{};
    if (bufferedRecords == 0u) {
        plan.tickBudget = 0u;
        plan.catchingUp = false;
        return plan;
    }
    const bool aboveTarget =
        bufferedRecords > spectatorLiveTarget(cushionBonus);
    if (forceMaximum || !inGameplay) {
        plan.catchingUp = aboveTarget;
    } else {
        plan.catchingUp = wasCatchingUp
            ? aboveTarget
            : (bufferedRecords > spectatorCatchupEnter(cushionBonus));
    }
    if (!plan.catchingUp) {
        plan.tickBudget = 1u;
        return plan;
    }
    plan.speedPermille = forceMaximum
        ? kSpectatorCatchupMaxPermille
        : spectatorCatchupSpeedPermille(bufferedRecords, cushionBonus,
                                        inGameplay);
    plan.tickBudget = static_cast<std::size_t>(plan.speedPermille / 1000u);
    if (plan.tickBudget == 0u) plan.tickBudget = 1u;
    return plan;
}

struct SpectatorCatchupSlot {
    std::size_t ticks = 0u;
    uint32_t nextCredit = 0u;
};

constexpr SpectatorCatchupSlot spectatorCatchupSlot(
    const SpectatorCatchupPlan& plan,
    uint32_t priorCredit) {
    SpectatorCatchupSlot slot{};
    slot.ticks = plan.tickBudget;
    if (!plan.catchingUp) return slot;
    const uint32_t fraction = plan.speedPermille % 1000u;
    const uint32_t credit = (priorCredit % 1000u) + fraction;
    if (credit >= 1000u) {
        ++slot.ticks;
        slot.nextCredit = credit - 1000u;
    } else {
        slot.nextCredit = credit;
    }
    return slot;
}

constexpr bool spectatorJumpToLiveNeeded(std::size_t bufferedRecords,
                                         std::size_t cushionBonus = 0u) {
    return bufferedRecords > spectatorLiveTarget(cushionBonus);
}

constexpr std::size_t spectatorCatchupTickBudget(
    std::size_t bufferedRecords) {
    return spectatorCatchupPlan(bufferedRecords, true).tickBudget;
}

constexpr uint64_t spectatorEstimatedSlotsToLive(
    std::size_t bufferedRecords,
    bool catchingUp,
    std::size_t cushionBonus = 0u,
    bool inGameplay = true) {
    const std::size_t target = spectatorLiveTarget(cushionBonus);
    if (bufferedRecords <= target) return 0u;
    const SpectatorCatchupPlan plan =
        spectatorCatchupPlan(bufferedRecords, catchingUp, cushionBonus,
                             inGameplay);
    if (!plan.catchingUp || plan.speedPermille <= 1000u) return 0u;
    const uint64_t netPermille = plan.speedPermille - 1000u;
    const uint64_t excess = bufferedRecords - target;
    return (excess * 1000u + netPermille - 1u) / netPermille;
}

constexpr bool spectatorCatchupMustYield(
    bool dispatcherBoundary,
    bool gameplayBoundary,
    bool phaseBoundary,
    uint32_t currentScene,
    uint32_t requestedScene) {
    return dispatcherBoundary
        || gameplayBoundary
        || phaseBoundary
        || currentScene != requestedScene;
}

}
