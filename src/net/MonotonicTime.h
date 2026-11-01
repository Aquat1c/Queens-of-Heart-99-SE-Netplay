#pragma once

#include <cstdint>

namespace qnet {

constexpr int64_t ticksToMicros(int64_t ticks, int64_t freq) {
    if (freq <= 0 || ticks <= 0) {
        return 0;
    }
    const int64_t whole = ticks / freq;
    const int64_t remainder = ticks % freq;
    return whole * 1000000 + (remainder * 1000000) / freq;
}

constexpr int64_t ticksToMillis(int64_t ticks, int64_t freq) {
    if (freq <= 0 || ticks <= 0) {
        return 0;
    }
    const int64_t whole = ticks / freq;
    const int64_t remainder = ticks % freq;
    return whole * 1000 + (remainder * 1000) / freq;
}

constexpr int64_t microsToMillisRounded(int64_t micros) {
    return micros <= 0 ? 0 : (micros + 500) / 1000;
}

}
