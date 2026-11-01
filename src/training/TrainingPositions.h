#pragma once

#include <cstdint>

#include "input/InputActions.h"
#include "training/TrainingTypes.h"

namespace qtraining {

enum class PositionPlanError : uint32_t {
    None = 0,
    InvalidModifier = 1,
    InvalidPreset = 2,
    InvalidConfiguration = 3,
    ArithmeticOverflow = 4,
    FighterOutsideWorld = 5,
    FightersCrossed = 6,
    InvalidFacing = 7,
    InvalidCamera = 8,
    EdgeGapNotSatisfied = 9,
};

struct ResetModifierResult {
    bool valid = false;
    PositionPreset preset = PositionPreset::RoundStart;
    PositionPlanError error = PositionPlanError::InvalidModifier;
};

struct FighterHorizontalBounds {
    int32_t left = 0;
    int32_t right = 0;
};

struct PositionMathConfig {
    int32_t worldLeft = 0;
    int32_t worldRight = 256;
    int32_t cameraMin = 0;
    int32_t cameraMax = 192;
    int32_t cameraBias = 32;
    int32_t anchorSeparation = 128;
    int32_t closeContactGap = 0;
    int32_t minimumEdgeGap = 0;
    int32_t nativeP1X = 64;
    int32_t nativeP2X = 192;
    int32_t nativeCameraX = 96;
    FighterHorizontalBounds p1Bounds{};
    FighterHorizontalBounds p2Bounds{};
};

struct PositionPlan {
    bool valid = false;
    PositionPlanError error = PositionPlanError::InvalidPreset;
    PositionPreset preset = PositionPreset::RoundStart;
    int32_t p1X = 0;
    int32_t p2X = 0;
    uint8_t p1Facing = 0;
    uint8_t p2Facing = 1;
    int32_t cameraX = 0;
    int32_t anchorSeparation = 0;
};

ResetModifierResult positionPresetFromResetModifier(
    uint32_t logicalDirections);

PositionPlan planPositionPreset(PositionPreset preset,
                                const PositionMathConfig& config);

PositionPlanError validatePositionPlan(const PositionPlan& plan,
                                       const PositionMathConfig& config);

}
