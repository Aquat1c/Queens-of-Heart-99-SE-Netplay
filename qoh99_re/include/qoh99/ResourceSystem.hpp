/* Sprite surfaces dword_68BC00 and metadata dword_68C020 (stride 124) are load-once OS state: never savestate them, restoring crashes. */
#pragma once
#include "AddressMap.hpp"
#include "MemoryView.hpp"

namespace qoh99 {

class ResourceSystem {
public:
    explicit ResourceSystem(MemoryView& m) : m_(m) {}

    static constexpr std::uint32_t kSpriteMetaStride = 124;

    GameAddr spriteSurface(int i) const {
        return m_.get<GameAddr>(addr::kSpriteSurfaceTable + 4u * static_cast<GameAddr>(i));
    }
    GameAddr spriteMetaAt(int i) const {
        return addr::kSpriteMetaTable + kSpriteMetaStride * static_cast<GameAddr>(i);
    }

private:
    MemoryView& m_;
};

}
