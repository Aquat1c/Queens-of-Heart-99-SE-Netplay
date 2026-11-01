#include "training/TrainingPositions.h"

#include <algorithm>
#include <limits>

namespace qtraining {

namespace {

bool toInt32(int64_t value, int32_t& out) {
    if (value < std::numeric_limits<int32_t>::min()
        || value > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    out = static_cast<int32_t>(value);
    return true;
}

int32_t clampCamera(int64_t value, const PositionMathConfig& config) {
    if (value < config.cameraMin) return config.cameraMin;
    if (value > config.cameraMax) return config.cameraMax;
    return static_cast<int32_t>(value);
}

bool validConfig(const PositionMathConfig& config) {
    return config.worldLeft < config.worldRight
        && config.cameraMin <= config.cameraMax
        && config.anchorSeparation > 0
        && config.closeContactGap >= 0
        && config.minimumEdgeGap >= 0
        && config.p1Bounds.left <= config.p1Bounds.right
        && config.p2Bounds.left <= config.p2Bounds.right;
}

// Separation floor from sub_4637C0's box-centre facing test: 2*separation > (l.left + l.right) - (r.left + r.right), or the pair turns forever.
int64_t centreOrderedSeparationFloor(
    const FighterHorizontalBounds& leftBounds,
    const FighterHorizontalBounds& rightBounds) {
    const int64_t doubledDelta = static_cast<int64_t>(leftBounds.left)
        + leftBounds.right - rightBounds.left - rightBounds.right;
    const int64_t halved = doubledDelta >= 0
        ? doubledDelta / 2
        : -(((-doubledDelta) + 1) / 2);
    return halved + 1;
}

int64_t closeAnchorSeparation(const FighterHorizontalBounds& leftBounds,
                              const FighterHorizontalBounds& rightBounds,
                              int32_t contactGap) {
    int64_t separation = static_cast<int64_t>(leftBounds.right)
        - rightBounds.left + contactGap;
    const int64_t centreFloor =
        centreOrderedSeparationFloor(leftBounds, rightBounds);
    if (separation < centreFloor) separation = centreFloor;
    return separation < 1 ? 1 : separation;
}

}

ResetModifierResult positionPresetFromResetModifier(
    uint32_t logicalDirections) {
    ResetModifierResult result{};
    if ((logicalDirections & ~qinput::kLogicalDirectionMask) != 0)
        return result;

    switch (logicalDirections) {
        case 0:
            result.valid = true;
            result.preset = PositionPreset::RoundStart;
            result.error = PositionPlanError::None;
            return result;
        case qinput::kLogicalLeft:
            result.valid = true;
            result.preset = PositionPreset::LeftCorner;
            result.error = PositionPlanError::None;
            return result;
        case qinput::kLogicalDown:
            result.valid = true;
            result.preset = PositionPreset::Center;
            result.error = PositionPlanError::None;
            return result;
        case qinput::kLogicalRight:
            result.valid = true;
            result.preset = PositionPreset::RightCorner;
            result.error = PositionPlanError::None;
            return result;
        case qinput::kLogicalUp:
            result.valid = true;
            result.preset = PositionPreset::VeryClose;
            result.error = PositionPlanError::None;
            return result;
        default:
            return result;
    }
}

PositionPlanError validatePositionPlan(
    const PositionPlan& plan, const PositionMathConfig& config) {
    if (!validConfig(config))
        return PositionPlanError::InvalidConfiguration;
    if (plan.p1Facing > 1 || plan.p2Facing > 1)
        return PositionPlanError::InvalidFacing;
    if (plan.cameraX < config.cameraMin
        || plan.cameraX > config.cameraMax) {
        return PositionPlanError::InvalidCamera;
    }

    const int64_t p1Left =
        static_cast<int64_t>(plan.p1X) + config.p1Bounds.left;
    const int64_t p1Right =
        static_cast<int64_t>(plan.p1X) + config.p1Bounds.right;
    const int64_t p2Left =
        static_cast<int64_t>(plan.p2X) + config.p2Bounds.left;
    const int64_t p2Right =
        static_cast<int64_t>(plan.p2X) + config.p2Bounds.right;
    if (p1Left < config.worldLeft || p1Right > config.worldRight
        || p2Left < config.worldLeft || p2Right > config.worldRight) {
        return PositionPlanError::FighterOutsideWorld;
    }

    const bool p1IsLeft = plan.p1X <= plan.p2X;
    if (p1IsLeft) {
        if (plan.p1Facing != 0 || plan.p2Facing != 1)
            return PositionPlanError::InvalidFacing;
    } else {
        if (plan.p2Facing != 0 || plan.p1Facing != 1)
            return PositionPlanError::InvalidFacing;
    }
    const int64_t leftBoxLeft = p1IsLeft ? p1Left : p2Left;
    const int64_t leftBoxRight = p1IsLeft ? p1Right : p2Right;
    const int64_t rightBoxLeft = p1IsLeft ? p2Left : p1Left;
    const int64_t rightBoxRight = p1IsLeft ? p2Right : p1Right;
    if (leftBoxLeft > rightBoxLeft || leftBoxRight > rightBoxRight)
        return PositionPlanError::FightersCrossed;
    if (config.minimumEdgeGap > 0
        && leftBoxRight + config.minimumEdgeGap > rightBoxLeft) {
        return PositionPlanError::EdgeGapNotSatisfied;
    }
    return PositionPlanError::None;
}

PositionPlan planPositionPreset(
    PositionPreset preset, const PositionMathConfig& config) {
    PositionPlan plan{};
    plan.preset = preset;
    if (!validConfig(config)) {
        plan.error = PositionPlanError::InvalidConfiguration;
        return plan;
    }
    if (preset < PositionPreset::RoundStart
        || preset > PositionPreset::VeryClose) {
        plan.error = PositionPlanError::InvalidPreset;
        return plan;
    }

    int64_t separation = static_cast<int64_t>(config.anchorSeparation);
    switch (preset) {
        case PositionPreset::LeftCorner:
            separation = closeAnchorSeparation(
                config.p2Bounds, config.p1Bounds, config.closeContactGap);
            break;
        case PositionPreset::Center:
        case PositionPreset::VeryClose:
        case PositionPreset::RightCorner:
            separation = closeAnchorSeparation(
                config.p1Bounds, config.p2Bounds, config.closeContactGap);
            break;
        case PositionPreset::RoundStart:
        case PositionPreset::FromModifier:
            break;
    }
    const int64_t centeredP1 =
        (static_cast<int64_t>(config.worldLeft) + config.worldRight
         - config.p1Bounds.left - separation - config.p2Bounds.right) / 2;

    int64_t p1 = 0;
    int64_t p2 = 0;
    switch (preset) {
        case PositionPreset::RoundStart:
            p1 = config.nativeP1X;
            p2 = config.nativeP2X;
            plan.p1Facing = 0;
            plan.p2Facing = 1;
            plan.cameraX = config.nativeCameraX;
            break;

        case PositionPreset::LeftCorner:
            p2 = static_cast<int64_t>(config.worldLeft)
                - config.p2Bounds.left;
            p1 = p2 + separation;
            plan.p2Facing = 0;
            plan.p1Facing = 1;
            break;

        case PositionPreset::Center:
        case PositionPreset::VeryClose:
            p1 = centeredP1;
            p2 = p1 + separation;
            plan.p1Facing = 0;
            plan.p2Facing = 1;
            break;

        case PositionPreset::RightCorner:
            p2 = static_cast<int64_t>(config.worldRight)
                - config.p2Bounds.right;
            p1 = p2 - separation;
            plan.p1Facing = 0;
            plan.p2Facing = 1;
            break;

        case PositionPreset::FromModifier:
            break;
    }

    if (!toInt32(p1, plan.p1X) || !toInt32(p2, plan.p2X)
        || !toInt32(separation, plan.anchorSeparation)) {
        plan.error = PositionPlanError::ArithmeticOverflow;
        return plan;
    }
    if (preset != PositionPreset::RoundStart) {
        const int64_t midpoint = (p1 + p2) / 2;
        plan.cameraX = clampCamera(
            midpoint - config.cameraBias, config);
    }
    plan.error = validatePositionPlan(plan, config);
    plan.valid = plan.error == PositionPlanError::None;
    return plan;
}

}
