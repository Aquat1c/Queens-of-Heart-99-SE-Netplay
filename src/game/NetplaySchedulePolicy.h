#pragma once

#include <cstdint>

#include "net/FrameArithmetic.h"

namespace qgame {

enum class WorkKind : uint8_t {
    Advance,
    Correct,
    Hold,
};

enum class HoldCause : uint8_t {
    None,
    NotCadence,
    LocalInputMissing,
    PredictionLimit,
    LifecycleBoundary,
};

enum class DebtDisposition : uint8_t {
    None,
    CreateOne,
    Consume,
};

struct SchedulePlan {
    WorkKind kind = WorkKind::Hold;
    HoldCause hold = HoldCause::None;
    uint32_t forwardFrames = 0;
    uint32_t visibleIndex = 0;
    DebtDisposition debt = DebtDisposition::None;
    uint32_t debtConsumed = 0;
    int32_t frameAdvantage = 0;
    bool frameAdvantageValid = false;
};

struct FrameDomains {
    uint32_t simFrontier = 0;
    uint32_t remoteActualPrefix = 0;
    uint32_t peerSimFrontier = 0;
    bool     peerSampleFresh = false;
    uint8_t  localMaxRollback = 0;
};

struct OpportunityInputs {
    bool cadenceOpportunity = false;
    bool pendingCorrection = false;
    bool localInputReady = false;
    bool remoteActualAtFrontier = false;
    bool lifecycleExactBoundary = false;
    bool sessionEventDrain = false;
    uint32_t recoverableDebtFrames = 0;
    uint32_t safeForwardHeadroom = 0;
};

struct SchedulePolicyConfig {
    uint32_t maxCatchupExtraPerOpportunity = 1;
    int32_t  frameAdvantageDeadband = 2;
};

constexpr uint32_t predictionDepth(const FrameDomains& d) {
    return qnet::frameAfter(d.simFrontier, d.remoteActualPrefix)
        ? qnet::forwardDistance(d.remoteActualPrefix, d.simFrontier)
        : 0u;
}

constexpr int32_t frameAdvantage(const FrameDomains& d) {
    return d.peerSampleFresh ? qnet::signedLead(d.simFrontier, d.peerSimFrontier) : 0;
}

inline SchedulePlan planSchedule(const FrameDomains& d, const OpportunityInputs& in,
                                 const SchedulePolicyConfig& cfg) {
    SchedulePlan p{};
    p.frameAdvantageValid = d.peerSampleFresh;
    p.frameAdvantage = frameAdvantage(d);

    if (!in.cadenceOpportunity) {
        p.kind = WorkKind::Hold;
        p.hold = HoldCause::NotCadence;
        return p;
    }
    if (in.pendingCorrection) {
        p.kind = WorkKind::Correct;
        return p;
    }
    if (!in.localInputReady) {
        p.kind = WorkKind::Hold;
        p.hold = HoldCause::LocalInputMissing;
        return p;
    }
    if (in.lifecycleExactBoundary && !in.remoteActualAtFrontier) {
        p.kind = WorkKind::Hold;
        p.hold = HoldCause::LifecycleBoundary;
        return p;
    }
    const uint32_t depth = predictionDepth(d);
    if (!in.remoteActualAtFrontier && depth >= d.localMaxRollback) {
        p.kind = WorkKind::Hold;
        p.hold = HoldCause::PredictionLimit;
        p.debt = DebtDisposition::CreateOne;
        return p;
    }

    uint32_t wanted = in.sessionEventDrain
        ? 0u
        : in.recoverableDebtFrames;
    if (!in.sessionEventDrain
        && p.frameAdvantageValid
        && p.frameAdvantage < -cfg.frameAdvantageDeadband) {
        const uint32_t behindBy =
            static_cast<uint32_t>(-p.frameAdvantage - cfg.frameAdvantageDeadband);
        if (behindBy > wanted) wanted = behindBy;
    }
    uint32_t extra = wanted;
    if (extra > cfg.maxCatchupExtraPerOpportunity) extra = cfg.maxCatchupExtraPerOpportunity;
    if (extra > in.safeForwardHeadroom) extra = in.safeForwardHeadroom;

    p.kind = WorkKind::Advance;
    p.forwardFrames = 1u + extra;
    p.visibleIndex = p.forwardFrames - 1u;
    if (extra > 0u) {
        p.debt = DebtDisposition::Consume;
        p.debtConsumed = extra;
    }
    return p;
}

class CadenceDebt {
public:
    static constexpr uint32_t kMaxFrames = 8;

    void reset() { debt_ = 0; }

    void apply(const SchedulePlan& plan) {
        if (plan.debt == DebtDisposition::CreateOne) {
            if (debt_ < kMaxFrames) ++debt_;
        } else if (plan.debt == DebtDisposition::Consume) {
            debt_ = plan.debtConsumed >= debt_ ? 0u : debt_ - plan.debtConsumed;
        }
    }

    void createBounded(uint32_t frames) {
        const uint32_t room = kMaxFrames - debt_;
        debt_ += frames < room ? frames : room;
    }

    void discardExternal() { debt_ = 0; }

    uint32_t frames() const { return debt_; }
    bool saturated() const { return debt_ >= kMaxFrames; }

private:
    uint32_t debt_ = 0;
};

inline void applyRebasedFrames(CadenceDebt& debt, int64_t rebasedFrames,
                               HoldCause lastHold) {
    if (rebasedFrames <= 0) return;
    if (lastHold != HoldCause::PredictionLimit) return;
    const uint64_t frames = static_cast<uint64_t>(rebasedFrames);
    debt.createBounded(frames > CadenceDebt::kMaxFrames
                           ? CadenceDebt::kMaxFrames
                           : static_cast<uint32_t>(frames));
}

}
