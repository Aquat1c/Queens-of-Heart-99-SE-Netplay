#pragma once

#include <cstdint>

#include "game/NetplayRunState.h"

namespace qgame {

inline constexpr uint32_t kRollbackBalanceWarmupFrames = 60u;
inline constexpr uint64_t kRollbackBalancePressureFreshMs = 250u;

enum class RollbackBalanceReason : uint8_t {
    Ready,
    Disabled,
    OutsideRollbackGameplay,
    WarmingUp,
    MissingPeerReport,
    StalePeerReport,
    InvalidLocalDepth,
    InvalidPeerDepth,
    PeerStateNotPaceable,
};

constexpr const char* rollbackBalanceReasonName(
    const RollbackBalanceReason reason) {
    switch (reason) {
        case RollbackBalanceReason::Ready: return "ready";
        case RollbackBalanceReason::Disabled: return "disabled";
        case RollbackBalanceReason::OutsideRollbackGameplay:
            return "outside-rollback-gameplay";
        case RollbackBalanceReason::WarmingUp: return "warming-up";
        case RollbackBalanceReason::MissingPeerReport:
            return "missing-peer-report";
        case RollbackBalanceReason::StalePeerReport:
            return "stale-peer-report";
        case RollbackBalanceReason::InvalidLocalDepth:
            return "invalid-local-depth";
        case RollbackBalanceReason::InvalidPeerDepth:
            return "invalid-peer-depth";
        case RollbackBalanceReason::PeerStateNotPaceable:
            return "peer-state-not-paceable";
    }
    return "unknown";
}

constexpr bool rollbackBalancePeerStatePaceable(const uint8_t rawState) {
    switch (static_cast<NetplayRunState>(rawState)) {
        case NetplayRunState::Running:
        case NetplayRunState::CorrectionCatchUp:
        case NetplayRunState::PredictionPressure:
        case NetplayRunState::EmergencyNetworkHold:
        case NetplayRunState::RecoveryCatchUp:
            return true;
        case NetplayRunState::HoldNegotiating:
        case NetplayRunState::ExactLifecycleBarrier:
        case NetplayRunState::ExternalSuspension:
        case NetplayRunState::Terminated:
            return false;
    }
    return false;
}

struct RollbackBalanceInput {
    bool enabled = true;
    bool rollbackGameplay = false;
    bool hasPeerReport = false;
    uint32_t gameplayFramesObserved = 0;
    uint64_t peerReportAgeMs = 0;
    uint16_t localPredictionDepth = 0;
    uint16_t localPredictionLimit = 0;
    uint16_t peerPredictionDepth = 0;
    uint16_t peerPredictionLimit = 0;
    uint8_t peerRunState = 0;
};

struct RollbackBalanceSample {
    bool usable = false;
    int32_t predictionDepthDelta = 0;
    RollbackBalanceReason reason =
        RollbackBalanceReason::OutsideRollbackGameplay;
};

constexpr RollbackBalanceSample classifyRollbackBalance(
    const RollbackBalanceInput& in) {
    if (!in.enabled)
        return {false, 0, RollbackBalanceReason::Disabled};
    if (!in.rollbackGameplay) {
        return {false, 0,
                RollbackBalanceReason::OutsideRollbackGameplay};
    }
    if (in.gameplayFramesObserved < kRollbackBalanceWarmupFrames)
        return {false, 0, RollbackBalanceReason::WarmingUp};
    if (!in.hasPeerReport)
        return {false, 0, RollbackBalanceReason::MissingPeerReport};
    if (in.peerReportAgeMs > kRollbackBalancePressureFreshMs)
        return {false, 0, RollbackBalanceReason::StalePeerReport};
    if (in.localPredictionDepth > in.localPredictionLimit)
        return {false, 0, RollbackBalanceReason::InvalidLocalDepth};
    if (in.peerPredictionDepth > in.peerPredictionLimit)
        return {false, 0, RollbackBalanceReason::InvalidPeerDepth};
    if (!rollbackBalancePeerStatePaceable(in.peerRunState))
        return {false, 0, RollbackBalanceReason::PeerStateNotPaceable};

    return {
        true,
        static_cast<int32_t>(in.localPredictionDepth)
            - static_cast<int32_t>(in.peerPredictionDepth),
        RollbackBalanceReason::Ready,
    };
}

}
