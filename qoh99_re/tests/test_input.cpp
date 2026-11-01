#include "TestFramework.hpp"
#include "qoh99/InputSystem.hpp"

using namespace qoh99;

TEST(input_pack_single_bits) {
    CHECK_EQ(NetInputPacker::pack(input_mask::kBtn1, 0, 0), 0x1u);
    CHECK_EQ(NetInputPacker::pack(input_mask::kBtn4, 0, 0), 0x8u);
    CHECK_EQ(NetInputPacker::pack(input_mask::kDirA, 0, 0), 0x100u);
    CHECK_EQ(NetInputPacker::pack(input_mask::kDirB, 0, 0), 0x200u);
    CHECK_EQ(NetInputPacker::pack(input_mask::kDirLeft, 0, 0), 0x400u);
    CHECK_EQ(NetInputPacker::pack(input_mask::kDirRight, 0, 0), 0x800u);
    CHECK_EQ(NetInputPacker::pack(0, input_mask::kSysConfirm, 0), 0x1000u);
    CHECK_EQ(NetInputPacker::pack(0, input_mask::kSysCancel, 0), 0x2000u);
    CHECK_EQ(NetInputPacker::pack(0, 0, 0xABCD), 0xABCD0000u);
}

TEST(input_pack_unpack_all_combinations) {
    for (std::uint32_t dirs = 0; dirs < 16; ++dirs) {
        for (std::uint32_t btns = 0; btns < 16; ++btns) {
            std::uint32_t logical = (dirs << 16) | btns;
            std::uint32_t packed = NetInputPacker::pack(logical, 0, 0x1234);
            CHECK_EQ(NetInputPacker::unpackLogical(packed), logical);
            CHECK_EQ(NetInputPacker::unpackSeq(packed), 0x1234u);
        }
    }
}

TEST(input_facing_swap) {
    LogicalInput in{ input_mask::kDirLeft | input_mask::kBtn2 };
    LogicalInput sw = in.withFacingSwap();
    CHECK_EQ(sw.mask, input_mask::kDirRight | input_mask::kBtn2);
    CHECK_EQ(sw.withFacingSwap().mask, in.mask);
    LogicalInput ud{ input_mask::kDirUp | input_mask::kDirDown };
    CHECK_EQ(ud.withFacingSwap().mask, ud.mask);
    CHECK_EQ(input_mask::kDirUp, 0x10000u);
    CHECK_EQ(input_mask::kDirDown, 0x20000u);
    LogicalInput both{ input_mask::kDirLeft | input_mask::kDirRight };
    CHECK_EQ(both.withFacingSwap().mask, input_mask::kDirLeft);
}

TEST(input_ai_generator_deterministic) {
    for (std::uint32_t f = 0; f < 500; f += 7)
        CHECK_EQ(InputSystem::aiInput(f, 0).mask, InputSystem::aiInput(f, 0).mask);
    CHECK_EQ(InputSystem::aiInput(44, 1).mask, InputSystem::aiInput(65, 1).mask);
    bool differs = false;
    for (std::uint32_t f = 0; f < 44 * 20; f += 22)
        if (InputSystem::aiInput(f, 0).mask != InputSystem::aiInput(f, 1).mask)
            differs = true;
    CHECK(differs);
    for (std::uint32_t f = 0; f < 44 * 20; f += 11) {
        std::uint32_t m = InputSystem::aiInput(f, 0).mask;
        CHECK_EQ(m & ~(input_mask::kDirBits | input_mask::kBtnBits), 0u);
    }
}

TEST(input_keystate_injection_via_view) {
    BufferMemoryView m;
    InputSystem in(m);
    CHECK(in.injectKeyState(0x2C, 0x80));
    CHECK_EQ(in.keyState(0x2C), 0x80);
    CHECK_EQ(in.keyState(0x2D), 0);
}
