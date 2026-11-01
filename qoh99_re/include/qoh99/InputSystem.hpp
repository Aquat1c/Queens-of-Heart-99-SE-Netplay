#pragma once
#include <cstdint>
#include "AddressMap.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

namespace input_mask {
constexpr std::uint32_t kDirUp   = 0x10000;
constexpr std::uint32_t kDirDown = 0x20000;
constexpr std::uint32_t kDirA    = kDirUp;
constexpr std::uint32_t kDirB    = kDirDown;
constexpr std::uint32_t kDirLeft = 0x40000;
constexpr std::uint32_t kDirRight= 0x80000;
constexpr std::uint32_t kBtn1    = 0x0001;
constexpr std::uint32_t kBtn2    = 0x0002;
constexpr std::uint32_t kBtn3    = 0x0004;
constexpr std::uint32_t kBtn4    = 0x0008;
constexpr std::uint32_t kDirBits = 0xF0000;
constexpr std::uint32_t kBtnBits = 0x0000F;
constexpr std::uint32_t kSysF12    = 0x100;
constexpr std::uint32_t kSysDelete = 0x200;
constexpr std::uint32_t kSysConfirm = kSysF12;
constexpr std::uint32_t kSysCancel  = kSysDelete;
}

struct LogicalInput {
    std::uint32_t mask = 0;

    std::uint32_t dirs() const { return mask & input_mask::kDirBits; }
    std::uint32_t buttons() const { return mask & input_mask::kBtnBits; }

    LogicalInput withFacingSwap() const {
        std::uint32_t m = mask & ~(input_mask::kDirLeft | input_mask::kDirRight);
        if (mask & input_mask::kDirRight)      m |= input_mask::kDirLeft;
        else if (mask & input_mask::kDirLeft)  m |= input_mask::kDirRight;
        return { m };
    }
};

class NetInputPacker {
public:
    // packed = (btn & 0xF) | ((dir & 0xF0000) >> 8) | ((sys & 0x300) << 4) | seq << 16
    static constexpr std::uint32_t pack(std::uint32_t logical, std::uint32_t sysWord,
                                        std::uint32_t frameSeq) {
        return (logical & 0xFu)
             | ((logical & 0xF0000u) >> 8)
             | ((sysWord & 0x300u) << 4)
             | (frameSeq << 16);
    }
    // receiver: dword_66ECD4 = (packed & 0xFF) | ((packed & 0xF00) << 8)
    static constexpr std::uint32_t unpackLogical(std::uint32_t packed) {
        return (packed & 0xFFu) | ((packed & 0xF00u) << 8);
    }
    static constexpr std::uint32_t unpackSys(std::uint32_t packed) {
        return (packed >> 12) & 3u;
    }
    static constexpr std::uint32_t unpackSeq(std::uint32_t packed) {
        return packed >> 16;
    }
};

class InputSystem {
public:
    explicit InputSystem(MemoryView& m) : m_(m) {}

    std::uint8_t keyState(int scancode) const {
        return m_.get<std::uint8_t>(addr::kKeyState + static_cast<GameAddr>(scancode & 0xFF));
    }
    bool injectKeyState(int scancode, std::uint8_t v) {
        return m_.set<std::uint8_t>(addr::kKeyState + static_cast<GameAddr>(scancode & 0xFF), v);
    }
    std::uint32_t sessionWord() const { return m_.get<std::uint32_t>(addr::kSessionInputWord); }
    std::uint32_t remoteInput() const { return m_.get<std::uint32_t>(addr::kNetRemoteInput); }
    bool setRemoteInput(std::uint32_t v) { return m_.set<std::uint32_t>(addr::kNetRemoteInput, v); }

    static LogicalInput aiInput(std::uint32_t frame, int player);

private:
    MemoryView& m_;
};

}
