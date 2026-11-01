/* One 0x5FC (1532-byte) actor object. Do NOT grow: changes past +0x5FC belong to adjacent pool objects. */
#pragma once
#include "AddressMap.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

namespace fighter_field {
constexpr std::uint32_t kActionState   = 48;
constexpr std::uint32_t kActionFrame   = 52;
constexpr std::uint32_t kActionLockEnd = 56;
constexpr std::uint32_t kAnimFrame     = 64;
constexpr std::uint32_t kNextAction    = 72;
constexpr std::uint32_t kTransitionFlag= 76;
constexpr std::uint32_t kPrevAction    = 80;
constexpr std::uint32_t kDispatchAction= 84;
constexpr std::uint32_t kInitStatus    = 88;
constexpr std::uint32_t kPosXSyncWord  = 136;
constexpr std::uint32_t kPosYSyncWord  = 140;
constexpr std::uint32_t kMoveGateByte  = 156;
constexpr std::uint32_t kChildCmdPtr   = 164;
constexpr std::uint32_t kFacing        = 170;   // Byte; 1 swaps the LEFT/RIGHT masks.
constexpr std::uint32_t kExGaugeOwner  = 171;   // Byte; P1/P3 = 0, P2/P4 = 1.
constexpr std::uint32_t kPlayerIndex   = 172;   // Byte index into inputSourceTable.
constexpr std::uint32_t kCharOrType    = 174;
constexpr std::uint32_t kComOwnership  = 1400;  // Dword; nonzero routes to the native COM AI.
constexpr std::uint32_t kPairedPtr     = 1436;
constexpr std::uint32_t kSize          = 0x5FC;
}

class Fighter {
public:
    Fighter(MemoryView& m, GameAddr base) : m_(m), base_(base) {}

    GameAddr base() const { return base_; }
    bool valid() const { return base_ != kNullAddr; }

    int actionState() const  { return m_.get<int>(base_ + fighter_field::kActionState); }
    int actionFrame() const  { return m_.get<int>(base_ + fighter_field::kActionFrame); }
    int actionLockEnd() const{ return m_.get<int>(base_ + fighter_field::kActionLockEnd); }
    int animFrame() const    { return m_.get<int>(base_ + fighter_field::kAnimFrame); }
    int nextAction() const   { return m_.get<int>(base_ + fighter_field::kNextAction); }
    std::int16_t posXSync() const { return m_.get<std::int16_t>(base_ + fighter_field::kPosXSyncWord); }
    std::int16_t posYSync() const { return m_.get<std::int16_t>(base_ + fighter_field::kPosYSyncWord); }
    GameAddr childCmdPtr() const  { return m_.get<GameAddr>(base_ + fighter_field::kChildCmdPtr); }
    bool facingSwapped() const    { return m_.get<std::uint8_t>(base_ + fighter_field::kFacing) == 1; }
    std::uint8_t exGaugeOwner() const { return m_.get<std::uint8_t>(base_ + fighter_field::kExGaugeOwner); }
    std::uint8_t playerIndex() const { return m_.get<std::uint8_t>(base_ + fighter_field::kPlayerIndex); }
    std::uint8_t charOrType() const  { return m_.get<std::uint8_t>(base_ + fighter_field::kCharOrType); }
    bool comOwned() const         { return m_.get<int>(base_ + fighter_field::kComOwnership) != 0; }
    GameAddr pairedPtr() const    { return m_.get<GameAddr>(base_ + fighter_field::kPairedPtr); }

    bool snapshot(void* out) const { return m_.read(base_, out, fighter_field::kSize); }
    bool restore(const void* in)   { return m_.write(base_, in, fighter_field::kSize); }

private:
    MemoryView& m_;
    GameAddr base_;
};

}
