// Native logical mask: buttons bits 0..3 (A,B,C,D), directions bits 16..19 (Up 0x10000, Down 0x20000, Left 0x40000, Right 0x80000).
// Compact wire mask: buttons bits 0..3, directions bits 8..11; valid mask exactly 0x0F0F, unknown bits fail closed at every boundary.
#pragma once

#include <cstdint>

namespace qinput {

// Semantic action indices in QOH's native action-table order (sub_484D70).
enum class FightAction : uint8_t {
    Up = 0,
    Down = 1,
    Left = 2,
    Right = 3,
    A = 4,
    B = 5,
    C = 6,
    D = 7,
};

constexpr uint8_t kFightActionCount = 8;

constexpr const char* fightActionName(FightAction action) {
    switch (action) {
        case FightAction::Up: return "Up";
        case FightAction::Down: return "Down";
        case FightAction::Left: return "Left";
        case FightAction::Right: return "Right";
        case FightAction::A: return "A";
        case FightAction::B: return "B";
        case FightAction::C: return "C";
        case FightAction::D: return "D";
    }
    return "?";
}

constexpr uint16_t kCanonicalFightMask = 0x0F0F;

constexpr uint16_t kCompactUp = 0x0100;
constexpr uint16_t kCompactDown = 0x0200;
constexpr uint16_t kCompactLeft = 0x0400;
constexpr uint16_t kCompactRight = 0x0800;
constexpr uint16_t kCompactA = 0x0001;
constexpr uint16_t kCompactB = 0x0002;
constexpr uint16_t kCompactC = 0x0004;
constexpr uint16_t kCompactD = 0x0008;

constexpr uint16_t compactBit(FightAction action) {
    switch (action) {
        case FightAction::Up: return kCompactUp;
        case FightAction::Down: return kCompactDown;
        case FightAction::Left: return kCompactLeft;
        case FightAction::Right: return kCompactRight;
        case FightAction::A: return kCompactA;
        case FightAction::B: return kCompactB;
        case FightAction::C: return kCompactC;
        case FightAction::D: return kCompactD;
    }
    return 0;
}

constexpr bool isCanonicalFightInput(uint16_t value) {
    return (value & static_cast<uint16_t>(~kCanonicalFightMask)) == 0;
}

constexpr uint32_t kLogicalUp = 0x10000u;
constexpr uint32_t kLogicalDown = 0x20000u;
constexpr uint32_t kLogicalLeft = 0x40000u;
constexpr uint32_t kLogicalRight = 0x80000u;
constexpr uint32_t kLogicalA = 0x00001u;
constexpr uint32_t kLogicalB = 0x00002u;
constexpr uint32_t kLogicalC = 0x00004u;
constexpr uint32_t kLogicalD = 0x00008u;
constexpr uint32_t kLogicalDirectionMask = 0xF0000u;
constexpr uint32_t kLogicalButtonMask = 0x0000Fu;
constexpr uint32_t kLogicalFightMask =
    kLogicalDirectionMask | kLogicalButtonMask;

constexpr uint32_t logicalBit(FightAction action) {
    switch (action) {
        case FightAction::Up: return kLogicalUp;
        case FightAction::Down: return kLogicalDown;
        case FightAction::Left: return kLogicalLeft;
        case FightAction::Right: return kLogicalRight;
        case FightAction::A: return kLogicalA;
        case FightAction::B: return kLogicalB;
        case FightAction::C: return kLogicalC;
        case FightAction::D: return kLogicalD;
    }
    return 0;
}

constexpr bool isCanonicalLogicalFightInput(uint32_t value) {
    return (value & ~kLogicalFightMask) == 0;
}

constexpr uint16_t compactFromLogical(uint32_t logical) {
    return static_cast<uint16_t>((logical & kLogicalButtonMask)
                                 | ((logical >> 8) & 0x0F00u));
}

// Input must be canonical: validate with isCanonicalFightInput() before converting.
constexpr uint32_t logicalFromCompact(uint16_t compact) {
    return (static_cast<uint32_t>(compact) & kLogicalButtonMask)
        | ((static_cast<uint32_t>(compact) & 0x0F00u) << 8);
}

static_assert(compactFromLogical(logicalFromCompact(0x0F0F)) == 0x0F0F,
              "compact/logical round trip");
static_assert(logicalFromCompact(compactFromLogical(0xF000F)) == 0xF000F,
              "logical/compact round trip");
static_assert(compactFromLogical(kLogicalUp) == kCompactUp, "Up mapping");
static_assert(compactFromLogical(kLogicalDown) == kCompactDown,
              "Down mapping");
static_assert(compactFromLogical(kLogicalLeft) == kCompactLeft,
              "Left mapping");
static_assert(compactFromLogical(kLogicalRight) == kCompactRight,
              "Right mapping");
static_assert(compactFromLogical(kLogicalA) == kCompactA, "A mapping");
static_assert(compactFromLogical(kLogicalD) == kCompactD, "D mapping");

// Facing (sub_47BF70): when facing == 1 horizontal directions swap; Right is tested first, so Left+Right becomes Left. Up+Down stays simultaneous.
constexpr uint32_t applyNativeFacing(uint32_t logical, uint8_t facing) {
    if (facing != 1) return logical;
    const uint32_t rest = logical & ~(kLogicalLeft | kLogicalRight);
    if (logical & kLogicalRight) return rest | kLogicalLeft;
    if (logical & kLogicalLeft) return rest | kLogicalRight;
    return logical;
}

static_assert(applyNativeFacing(kLogicalLeft | kLogicalRight, 1)
                  == kLogicalLeft,
              "simultaneous LR with facing 1 becomes native Left");
static_assert(applyNativeFacing(kLogicalUp | kLogicalDown, 1)
                  == (kLogicalUp | kLogicalDown),
              "Up+Down stays simultaneous under facing");

}
