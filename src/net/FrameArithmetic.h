// All netplay frontiers are one-past (half-open); never compare frames with raw unsigned subtraction.
#pragma once

#include <cstdint>

namespace qnet {

constexpr uint32_t kHalfSerialSpace = 0x80000000u;

constexpr bool frameAfter(uint32_t left, uint32_t right) {
    const uint32_t distance = left - right;
    return distance != 0u && distance < kHalfSerialSpace;
}

constexpr bool frameBefore(uint32_t left, uint32_t right) {
    return frameAfter(right, left);
}

constexpr bool frameAtOrAfter(uint32_t left, uint32_t right) {
    return left == right || frameAfter(left, right);
}

constexpr uint32_t forwardDistance(uint32_t from, uint32_t to) {
    return to - from;
}

constexpr int32_t signedLead(uint32_t ahead, uint32_t behind) {
    const uint32_t forward = ahead - behind;
    return forward < kHalfSerialSpace
        ? static_cast<int32_t>(forward)
        : -static_cast<int32_t>(behind - ahead);
}

}
