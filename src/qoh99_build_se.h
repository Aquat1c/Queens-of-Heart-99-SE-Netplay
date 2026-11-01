#pragma once

#include <cstddef>
#include <cstdint>

namespace qoh99::se {

inline bool verifyCodeBytes(std::uintptr_t va, const std::uint8_t* expect,
                            std::size_t n) {
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(va);
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] != expect[i]) return false;
    return true;
}

}
