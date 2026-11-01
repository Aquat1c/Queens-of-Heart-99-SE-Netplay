/* Double-buffered 118480-byte (0x1CED0) block: block0 @0x4ED140, block1 @0x50A010.
 * The active pointer is set before the selector flips, so never derive the active block from the selector: read dword_6234E8 or save both blocks. */
#pragma once
#include "AddressMap.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

namespace arena_field {
constexpr std::uint32_t kEntityArrayHdr   = 68892;
constexpr std::uint32_t kEntityArray      = 68904;
constexpr std::uint32_t kEntityTransform  = 68964;
constexpr std::uint32_t kEntityCount      = 85524;
constexpr std::uint32_t kEntityStride     = 104;
constexpr std::uint32_t kHudDescriptorOff = 0x4D4;
}

class ArenaFrame {
public:
    explicit ArenaFrame(MemoryView& m) : m_(m) {}

    static constexpr GameAddr kBlock0 = addr::kArenaBlock0;
    static constexpr GameAddr kBlock1 = addr::kArenaBlock1;
    static constexpr std::uint32_t kBlockSize = addr::kArenaBlockSize;

    GameAddr activeBlock() const { return m_.get<GameAddr>(addr::kArenaActivePtr); }
    bool activeBlockValid() const {
        GameAddr a = activeBlock();
        return a == kBlock0 || a == kBlock1;
    }
    GameAddr inactiveBlock() const {
        return activeBlock() == kBlock0 ? kBlock1 : kBlock0;
    }
    int selector() const { return m_.get<int>(addr::kArenaSelector); }
    int swapLatch() const { return m_.get<int>(addr::kArenaSwapLatch); }

    int entityCount() const {
        return m_.get<int>(activeBlock() + arena_field::kEntityCount);
    }
    GameAddr entityAt(int i) const {
        return activeBlock() + arena_field::kEntityArray +
               arena_field::kEntityStride * static_cast<GameAddr>(i);
    }

private:
    MemoryView& m_;
};

}
