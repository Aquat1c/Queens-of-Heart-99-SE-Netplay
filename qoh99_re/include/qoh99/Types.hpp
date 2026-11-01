/* GAME.EXE is 32-bit with ImageBase 0x400000 and no ASLR or relocations, so every VA is a stable constant. */
#pragma once
#include <cstdint>
#include <cstddef>

namespace qoh99 {

using GameAddr = std::uint32_t;

constexpr GameAddr kImageBase = 0x400000;
constexpr GameAddr kNullAddr  = 0;

}
