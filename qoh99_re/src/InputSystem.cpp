#include "qoh99/InputSystem.hpp"

namespace qoh99 {

LogicalInput InputSystem::aiInput(std::uint32_t frame, int player) {
    std::uint32_t window = frame / 22;
    std::uint32_t h = window * 2654435761u + static_cast<std::uint32_t>(player) * 40503u;
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;

    std::uint32_t mask = 0;
    switch (h % 5) {
        case 0: mask |= input_mask::kDirLeft;  break;
        case 1: mask |= input_mask::kDirRight; break;
        case 2: mask |= input_mask::kDirA;     break;
        case 3: mask |= input_mask::kDirB;     break;
        default: break;
    }
    if ((h >> 8) & 1)
        mask |= (input_mask::kBtn1 << ((h >> 9) & 3));
    return { mask };
}

}
