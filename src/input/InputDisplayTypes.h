#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "input/InputActions.h"

namespace qinput {

// Deliberately separate from kTrainingApiVersion: a training ABI break must not disable the replay input viewer.
constexpr uint32_t kInputDisplayApiVersion = 1u;

// Lane index is the hkInputRead player index 0..3, never the fighter slot byte (+172) or the EX owner side (+171).
constexpr uint32_t kInputDisplayLaneCount = 4u;

constexpr uint32_t kInputDisplayHistoryCapacity = 32u;

constexpr uint32_t kInputDisplayHoldCap = 999u;

constexpr uint32_t kInputDisplayNoLane = 0xFFFFFFFFu;

// None must stay 0: a zeroed or unwritten frame has to resolve to "draw nothing".
enum class InputDisplayMode : uint32_t {
    None = 0,
    Training = 1,
    ReplayPlayback = 2,
    Count = 3,
};

constexpr bool isPublishableInputDisplayMode(InputDisplayMode mode) {
    return mode == InputDisplayMode::Training
        || mode == InputDisplayMode::ReplayPlayback;
}

constexpr const char* inputDisplayModeName(InputDisplayMode mode) {
    switch (mode) {
        case InputDisplayMode::None: return "none";
        case InputDisplayMode::Training: return "training";
        case InputDisplayMode::ReplayPlayback: return "replay";
        case InputDisplayMode::Count: break;
    }
    return "?";
}

// Neutral is 0; the eight compass points are 1..8 in screen-clockwise order starting at Right.
enum class InputDisplayDirection : uint32_t {
    Neutral = 0,
    Right = 1,
    DownRight = 2,
    Down = 3,
    DownLeft = 4,
    Left = 5,
    UpLeft = 6,
    Up = 7,
    UpRight = 8,
    Count = 9,
};

constexpr uint32_t kInputDisplayDirectionCount =
    static_cast<uint32_t>(InputDisplayDirection::Count);
constexpr uint32_t kInputDisplayCompassCount = 8u;
constexpr uint32_t kInputDisplayNoCompass = 0xFFFFFFFFu;

constexpr uint32_t kInputDisplayNibbleUp = 0x1u;
constexpr uint32_t kInputDisplayNibbleDown = 0x2u;
constexpr uint32_t kInputDisplayNibbleLeft = 0x4u;
constexpr uint32_t kInputDisplayNibbleRight = 0x8u;
constexpr uint32_t kInputDisplayNibbleMask = 0xFu;

constexpr uint32_t inputDisplayDirectionNibble(uint32_t logicalMask) {
    return (((logicalMask & kLogicalUp) != 0u) ? kInputDisplayNibbleUp : 0u)
        | (((logicalMask & kLogicalDown) != 0u) ? kInputDisplayNibbleDown : 0u)
        | (((logicalMask & kLogicalLeft) != 0u) ? kInputDisplayNibbleLeft : 0u)
        | (((logicalMask & kLogicalRight) != 0u) ? kInputDisplayNibbleRight
                                                 : 0u);
}

constexpr InputDisplayDirection inputDisplayDirectionFromNibble(
        uint32_t nibble) {
    const bool up = (nibble & kInputDisplayNibbleUp) != 0u;
    const bool down = (nibble & kInputDisplayNibbleDown) != 0u;
    const bool left = (nibble & kInputDisplayNibbleLeft) != 0u;
    const bool right = (nibble & kInputDisplayNibbleRight) != 0u;
    const bool holdUp = up && !down;
    const bool holdDown = down && !up;
    const bool holdLeft = left && !right;
    const bool holdRight = right && !left;
    if (holdRight) {
        if (holdUp) return InputDisplayDirection::UpRight;
        if (holdDown) return InputDisplayDirection::DownRight;
        return InputDisplayDirection::Right;
    }
    if (holdLeft) {
        if (holdUp) return InputDisplayDirection::UpLeft;
        if (holdDown) return InputDisplayDirection::DownLeft;
        return InputDisplayDirection::Left;
    }
    if (holdUp) return InputDisplayDirection::Up;
    if (holdDown) return InputDisplayDirection::Down;
    return InputDisplayDirection::Neutral;
}

constexpr InputDisplayDirection inputDisplayDirection(uint32_t logicalMask) {
    return inputDisplayDirectionFromNibble(
        inputDisplayDirectionNibble(logicalMask));
}

constexpr uint32_t inputDisplayCompassIndex(InputDisplayDirection direction) {
    return (direction == InputDisplayDirection::Neutral
            || direction >= InputDisplayDirection::Count)
        ? kInputDisplayNoCompass
        : static_cast<uint32_t>(direction) - 1u;
}

constexpr bool inputDisplayDirectionIsDiagonal(
        InputDisplayDirection direction) {
    return direction == InputDisplayDirection::UpRight
        || direction == InputDisplayDirection::DownRight
        || direction == InputDisplayDirection::DownLeft
        || direction == InputDisplayDirection::UpLeft;
}

constexpr const char* inputDisplayDirectionName(
        InputDisplayDirection direction) {
    switch (direction) {
        case InputDisplayDirection::Neutral: return "Neutral";
        case InputDisplayDirection::Right: return "Right";
        case InputDisplayDirection::DownRight: return "Down-Right";
        case InputDisplayDirection::Down: return "Down";
        case InputDisplayDirection::DownLeft: return "Down-Left";
        case InputDisplayDirection::Left: return "Left";
        case InputDisplayDirection::UpLeft: return "Up-Left";
        case InputDisplayDirection::Up: return "Up";
        case InputDisplayDirection::UpRight: return "Up-Right";
        case InputDisplayDirection::Count: break;
    }
    return "?";
}

constexpr FightAction kInputDisplayButtonOrder[4] = {
    FightAction::A, FightAction::B, FightAction::C, FightAction::D,
};
constexpr uint32_t kInputDisplayButtonCount = 4u;

constexpr bool inputDisplayButtonPressed(uint32_t logicalMask,
                                         FightAction action) {
    return (logicalMask & logicalBit(action)) != 0u;
}

struct InputDisplayEntryV1 {
    uint32_t logicalMask = 0;
    uint32_t heldSteps = 0;
};

struct InputDisplayLaneV1 {
    uint32_t active = 0;
    uint32_t currentMask = 0;
    uint32_t entryCount = 0;
    uint32_t head = 0;
    InputDisplayEntryV1 entries[kInputDisplayHistoryCapacity] = {};
};

struct InputDisplaySnapshotV1 {
    uint32_t apiVersion = kInputDisplayApiVersion;
    uint32_t structSize = sizeof(InputDisplaySnapshotV1);
    uint32_t mode = static_cast<uint32_t>(InputDisplayMode::None);
    uint32_t producerGeneration = 0;
    uint32_t stepSerialLow = 0;
    uint32_t stepSerialHigh = 0;
    uint32_t drivenLane = kInputDisplayNoLane;
    uint32_t laneCount = kInputDisplayLaneCount;
    uint32_t historyCapacity = kInputDisplayHistoryCapacity;
    uint32_t holdCap = kInputDisplayHoldCap;
    InputDisplayLaneV1 lanes[kInputDisplayLaneCount] = {};
};

// Append-only: a growth adds its new size here and never edits or removes an earlier entry.
constexpr uint32_t kInputDisplaySnapshotV1BaseSize = 1128u;
constexpr uint32_t kInputDisplaySnapshotV1SizeTiers[] = {1128u};
constexpr uint32_t kInputDisplaySnapshotV1TierCount =
    sizeof(kInputDisplaySnapshotV1SizeTiers)
    / sizeof(kInputDisplaySnapshotV1SizeTiers[0]);

constexpr bool inputDisplayTiersAscend() {
    for (uint32_t i = 1; i < kInputDisplaySnapshotV1TierCount; ++i) {
        if (kInputDisplaySnapshotV1SizeTiers[i]
            <= kInputDisplaySnapshotV1SizeTiers[i - 1]) {
            return false;
        }
    }
    return true;
}

constexpr uint32_t inputDisplayTierFor(uint32_t callerSize) {
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < kInputDisplaySnapshotV1TierCount; ++i) {
        if (kInputDisplaySnapshotV1SizeTiers[i] <= callerSize)
            chosen = kInputDisplaySnapshotV1SizeTiers[i];
    }
    return chosen;
}

constexpr uint32_t inputDisplayOlderSlot(uint32_t slot) {
    return (slot + kInputDisplayHistoryCapacity - 1u)
        % kInputDisplayHistoryCapacity;
}

constexpr uint32_t inputDisplayLaneSlotAt(const InputDisplayLaneV1& lane,
                                          uint32_t ordinal) {
    if (lane.entryCount == 0u || lane.entryCount > kInputDisplayHistoryCapacity)
        return kInputDisplayNoLane;
    if (lane.head >= kInputDisplayHistoryCapacity) return kInputDisplayNoLane;
    if (ordinal >= lane.entryCount) return kInputDisplayNoLane;
    return (lane.head + kInputDisplayHistoryCapacity - ordinal)
        % kInputDisplayHistoryCapacity;
}

constexpr bool inputDisplayLaneStructurallyValid(
        const InputDisplayLaneV1& lane) {
    if (lane.active > 1u) return false;
    if (lane.entryCount > kInputDisplayHistoryCapacity) return false;
    if (lane.head >= kInputDisplayHistoryCapacity) return false;
    return true;
}

constexpr void inputDisplayLaneClearHistory(InputDisplayLaneV1& lane) {
    lane.entryCount = 0u;
    lane.head = 0u;
    lane.currentMask = 0u;
    for (uint32_t i = 0; i < kInputDisplayHistoryCapacity; ++i) {
        lane.entries[i].logicalMask = 0u;
        lane.entries[i].heldSteps = 0u;
    }
}

enum class InputDisplayAppend : uint32_t {
    Rejected = 0,
    Extended = 1,
    Started = 2,
};

inline InputDisplayAppend inputDisplayLaneAppend(InputDisplayLaneV1& lane,
                                                 uint32_t logicalMask) {
    if (!isCanonicalLogicalFightInput(logicalMask))
        return InputDisplayAppend::Rejected;
    if (!inputDisplayLaneStructurallyValid(lane))
        return InputDisplayAppend::Rejected;

    if (lane.entryCount != 0u
        && lane.entries[lane.head].logicalMask == logicalMask) {
        if (lane.entries[lane.head].heldSteps < kInputDisplayHoldCap)
            ++lane.entries[lane.head].heldSteps;
        lane.currentMask = logicalMask;
        return InputDisplayAppend::Extended;
    }

    if (lane.entryCount != 0u)
        lane.head = (lane.head + 1u) % kInputDisplayHistoryCapacity;
    lane.entries[lane.head].logicalMask = logicalMask;
    lane.entries[lane.head].heldSteps = 1u;
    if (lane.entryCount < kInputDisplayHistoryCapacity) ++lane.entryCount;
    lane.currentMask = logicalMask;
    return InputDisplayAppend::Started;
}

constexpr bool inputDisplaySnapshotRenderable(
        const InputDisplaySnapshotV1& snapshot) {
    if (snapshot.apiVersion != kInputDisplayApiVersion) return false;
    if (snapshot.structSize < kInputDisplaySnapshotV1BaseSize) return false;
    if (snapshot.structSize > sizeof(InputDisplaySnapshotV1)) return false;
    if (!isPublishableInputDisplayMode(
            static_cast<InputDisplayMode>(snapshot.mode))) {
        return false;
    }
    if (snapshot.laneCount == 0u || snapshot.laneCount > kInputDisplayLaneCount)
        return false;
    if (snapshot.historyCapacity != kInputDisplayHistoryCapacity) return false;
    if (snapshot.holdCap != kInputDisplayHoldCap) return false;
    if (static_cast<InputDisplayMode>(snapshot.mode)
        == InputDisplayMode::Training) {
        if (snapshot.drivenLane >= snapshot.laneCount) return false;
    } else if (snapshot.drivenLane != kInputDisplayNoLane) {
        return false;
    }
    bool anyActive = false;
    for (uint32_t lane = 0; lane < snapshot.laneCount; ++lane) {
        const InputDisplayLaneV1& view = snapshot.lanes[lane];
        if (!inputDisplayLaneStructurallyValid(view)) return false;
        if (!isCanonicalLogicalFightInput(view.currentMask)) return false;
        for (uint32_t i = 0; i < kInputDisplayHistoryCapacity; ++i) {
            if (!isCanonicalLogicalFightInput(view.entries[i].logicalMask))
                return false;
            if (view.entries[i].heldSteps > kInputDisplayHoldCap) return false;
        }
        if (view.active != 0u) anyActive = true;
    }
    return anyActive;
}

static_assert(sizeof(InputDisplayEntryV1) == 8,
              "input display entry ABI changed");
static_assert(sizeof(InputDisplayLaneV1) == 272,
              "input display lane ABI changed");
static_assert(sizeof(InputDisplaySnapshotV1) == 1128,
              "input display snapshot ABI changed");
static_assert(sizeof(InputDisplaySnapshotV1)
                  == kInputDisplaySnapshotV1BaseSize,
              "the frozen input display prefix is the whole v1 struct");
static_assert(sizeof(InputDisplaySnapshotV1) % sizeof(uint32_t) == 0,
              "input display must stay word-copyable by the publisher");
static_assert(offsetof(InputDisplayLaneV1, entries) == 16,
              "input display lane header must stay four words");
static_assert(offsetof(InputDisplaySnapshotV1, mode) == 8
                  && offsetof(InputDisplaySnapshotV1, drivenLane) == 24
                  && offsetof(InputDisplaySnapshotV1, lanes) == 40,
              "input display ABI offsets changed");
static_assert(offsetof(InputDisplaySnapshotV1, lanes)
                      + 3u * sizeof(InputDisplayLaneV1)
                  == 856,
              "input display lane stride changed");
static_assert(kInputDisplaySnapshotV1SizeTiers[0]
                  == kInputDisplaySnapshotV1BaseSize,
              "the first released input display tier is the frozen prefix");
static_assert(kInputDisplaySnapshotV1SizeTiers[
                      kInputDisplaySnapshotV1TierCount - 1]
                  == sizeof(InputDisplaySnapshotV1),
              "a growth must append its new size to the tier table");
static_assert(inputDisplayTiersAscend(),
              "released input display tiers must ascend and never repeat");
static_assert(std::is_standard_layout<InputDisplayEntryV1>::value
                  && std::is_trivially_copyable<InputDisplayEntryV1>::value
                  && std::is_standard_layout<InputDisplayLaneV1>::value
                  && std::is_trivially_copyable<InputDisplayLaneV1>::value
                  && std::is_standard_layout<InputDisplaySnapshotV1>::value
                  && std::is_trivially_copyable<
                         InputDisplaySnapshotV1>::value,
              "input display snapshot must remain POD");

static_assert(inputDisplayDirection(0u) == InputDisplayDirection::Neutral, "");
static_assert(inputDisplayDirection(kLogicalRight)
                  == InputDisplayDirection::Right, "");
static_assert(inputDisplayDirection(kLogicalRight | kLogicalDown)
                  == InputDisplayDirection::DownRight, "");
static_assert(inputDisplayDirection(kLogicalDown)
                  == InputDisplayDirection::Down, "");
static_assert(inputDisplayDirection(kLogicalLeft | kLogicalDown)
                  == InputDisplayDirection::DownLeft, "");
static_assert(inputDisplayDirection(kLogicalLeft)
                  == InputDisplayDirection::Left, "");
static_assert(inputDisplayDirection(kLogicalLeft | kLogicalUp)
                  == InputDisplayDirection::UpLeft, "");
static_assert(inputDisplayDirection(kLogicalUp)
                  == InputDisplayDirection::Up, "");
static_assert(inputDisplayDirection(kLogicalRight | kLogicalUp)
                  == InputDisplayDirection::UpRight, "");
static_assert(inputDisplayDirection(kLogicalUp | kLogicalDown)
                  == InputDisplayDirection::Neutral,
              "an opposing vertical pair cancels the vertical axis");
static_assert(inputDisplayDirection(kLogicalLeft | kLogicalRight)
                  == InputDisplayDirection::Neutral,
              "an opposing horizontal pair cancels the horizontal axis");
static_assert(inputDisplayDirection(kLogicalUp | kLogicalDown | kLogicalLeft)
                  == InputDisplayDirection::Left,
              "a cancelling pair must not disturb the other axis");
static_assert(inputDisplayDirection(kLogicalUp | kLogicalDown | kLogicalRight)
                  == InputDisplayDirection::Right, "");
static_assert(inputDisplayDirection(kLogicalLeft | kLogicalRight | kLogicalUp)
                  == InputDisplayDirection::Up, "");
static_assert(inputDisplayDirection(kLogicalLeft | kLogicalRight | kLogicalDown)
                  == InputDisplayDirection::Down, "");
static_assert(inputDisplayDirection(kLogicalDirectionMask)
                  == InputDisplayDirection::Neutral,
              "all four directions cancel to neutral");
static_assert(inputDisplayDirection(kLogicalButtonMask)
                  == InputDisplayDirection::Neutral,
              "button bits must not participate in direction decoding");
static_assert(inputDisplayCompassIndex(InputDisplayDirection::Right) == 0u
                  && inputDisplayCompassIndex(InputDisplayDirection::UpRight)
                         == 7u
                  && inputDisplayCompassIndex(InputDisplayDirection::Neutral)
                         == kInputDisplayNoCompass,
              "compass indices must run clockwise from Right with no neutral");

}
