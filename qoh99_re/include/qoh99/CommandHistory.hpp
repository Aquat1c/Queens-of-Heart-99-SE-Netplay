/* Layout at fighter+164: +8 direction ring 32 x dword, +136 hold counts 32 x dword, +264 head byte, +268 button ring 16 x dword, +332 hold counts 16 x dword, +396 head byte, +408..+432 move-detect scratch.
 * Saved by pointer: re-read fighter+164 after restoring the fixed regions, then restore the pointee. */
#pragma once
#include "Fighter.hpp"

namespace qoh99 {

namespace cmd_field {
constexpr std::uint32_t kDirRing   = 8;
constexpr std::uint32_t kDirHold   = 136;
constexpr std::uint32_t kDirHead   = 264;
constexpr std::uint32_t kBtnRing   = 268;
constexpr std::uint32_t kBtnHold   = 332;
constexpr std::uint32_t kBtnHead   = 396;
constexpr std::uint32_t kMinSize   = 0x1B4;
constexpr std::uint32_t kSaveSize  = 0x200;
constexpr int kDirDepth = 32;
constexpr int kBtnDepth = 16;
}

class CommandHistory {
public:
    CommandHistory(MemoryView& m, GameAddr base) : m_(m), base_(base) {}

    static CommandHistory ofFighter(MemoryView& m, const Fighter& f) {
        return CommandHistory(m, f.childCmdPtr());
    }

    GameAddr base() const { return base_; }
    bool valid() const { return base_ != kNullAddr; }

    std::uint8_t dirHead() const { return m_.get<std::uint8_t>(base_ + cmd_field::kDirHead); }
    std::uint8_t btnHead() const { return m_.get<std::uint8_t>(base_ + cmd_field::kBtnHead); }
    std::uint32_t dirAt(int i) const {
        return m_.get<std::uint32_t>(base_ + cmd_field::kDirRing +
                                     4u * (static_cast<std::uint32_t>(i) & 0x1F));
    }
    std::uint32_t btnAt(int i) const {
        return m_.get<std::uint32_t>(base_ + cmd_field::kBtnRing +
                                     4u * (static_cast<std::uint32_t>(i) & 0xF));
    }
    std::uint32_t dirHoldAt(int i) const {
        return m_.get<std::uint32_t>(base_ + cmd_field::kDirHold +
                                     4u * (static_cast<std::uint32_t>(i) & 0x1F));
    }
    std::uint32_t btnHoldAt(int i) const {
        return m_.get<std::uint32_t>(base_ + cmd_field::kBtnHold +
                                     4u * (static_cast<std::uint32_t>(i) & 0xF));
    }

    std::uint32_t latestDir() const { return dirAt(dirHead() + cmd_field::kDirDepth - 1); }
    std::uint32_t latestBtn() const { return btnAt(btnHead() + cmd_field::kBtnDepth - 1); }

    bool snapshot(void* out) const { return m_.read(base_, out, cmd_field::kSaveSize); }
    bool restore(const void* in)   { return m_.write(base_, in, cmd_field::kSaveSize); }

private:
    MemoryView& m_;
    GameAddr base_;
};

}
