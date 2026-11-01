#include "TestHarness.h"

#include "input/InputActions.h"

using namespace qinput;

QNP_TEST(ActionsAllCombinationsRoundTrip) {
    for (uint32_t combo = 0; combo < 256; ++combo) {
        uint32_t logical = 0;
        for (uint8_t action = 0; action < 8; ++action) {
            if (combo & (1u << action))
                logical |= logicalBit(static_cast<FightAction>(action));
        }
        const uint16_t compact = compactFromLogical(logical);
        QNP_CHECK(isCanonicalFightInput(compact));
        QNP_CHECK(logicalFromCompact(compact) == logical);
        QNP_CHECK(compactFromLogical(logicalFromCompact(compact))
                  == compact);
    }
}

QNP_TEST(ActionsInvalidValuesRejected) {
    int invalid = 0;
    for (uint32_t value = 0; value <= 0xFFFF; ++value) {
        const bool valid =
            isCanonicalFightInput(static_cast<uint16_t>(value));
        if ((value & ~0x0F0Fu) != 0) {
            QNP_CHECK(!valid);
            ++invalid;
        } else {
            QNP_CHECK(valid);
        }
    }
    QNP_CHECK(invalid == 0x10000 - 0x100);
}

QNP_TEST(ActionsEachForbiddenBitRejects) {
    for (int bit = 0; bit < 16; ++bit) {
        const uint16_t value = static_cast<uint16_t>(1u << bit);
        const bool forbidden = (value & ~kCanonicalFightMask) != 0;
        QNP_CHECK(isCanonicalFightInput(value) == !forbidden);
    }
}

QNP_TEST(ActionsOrderMatchesNativeTable) {
    QNP_CHECK(logicalBit(FightAction::Up) == 0x10000u);
    QNP_CHECK(logicalBit(FightAction::Down) == 0x20000u);
    QNP_CHECK(logicalBit(FightAction::Left) == 0x40000u);
    QNP_CHECK(logicalBit(FightAction::Right) == 0x80000u);
    QNP_CHECK(logicalBit(FightAction::A) == 0x1u);
    QNP_CHECK(logicalBit(FightAction::B) == 0x2u);
    QNP_CHECK(logicalBit(FightAction::C) == 0x4u);
    QNP_CHECK(logicalBit(FightAction::D) == 0x8u);
}

QNP_TEST(ActionsFacingBehavior) {
    QNP_CHECK(applyNativeFacing(kLogicalLeft, 0) == kLogicalLeft);
    QNP_CHECK(applyNativeFacing(kLogicalLeft, 1) == kLogicalRight);
    QNP_CHECK(applyNativeFacing(kLogicalRight, 1) == kLogicalLeft);
    QNP_CHECK(applyNativeFacing(kLogicalLeft | kLogicalRight, 1)
              == kLogicalLeft);
    QNP_CHECK(applyNativeFacing(kLogicalUp | kLogicalDown, 1)
              == (kLogicalUp | kLogicalDown));
    const uint32_t buttons =
        kLogicalA | kLogicalB | kLogicalC | kLogicalD;
    QNP_CHECK(applyNativeFacing(buttons, 1) == buttons);
    QNP_CHECK(applyNativeFacing(buttons | kLogicalRight, 1)
              == (buttons | kLogicalLeft));
}

QNP_TEST(ActionsMalformedNativeBitsCharacterized) {
    QNP_CHECK(!isCanonicalFightInput(0x0010));
    QNP_CHECK(!isCanonicalFightInput(0x00F0));
    QNP_CHECK(!isCanonicalFightInput(0x4000));
    QNP_CHECK(!isCanonicalFightInput(0x8000));
    const uint32_t garbageLogical = 0xF00F0u | kLogicalA | kLogicalUp;
    const uint16_t packed =
        compactFromLogical(garbageLogical & kLogicalFightMask);
    QNP_CHECK(isCanonicalFightInput(packed));
    QNP_CHECK(packed == (kCompactA | kCompactUp
                         | kCompactLeft | kCompactRight | kCompactDown));
}
